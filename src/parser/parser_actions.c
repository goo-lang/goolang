// Semantic-action helpers for parser.y's grammar rules, extracted from the
// generated parser.tab.c's former epilogue (everything after the second %%
// except yyerror, which now lives in src/parser/parser_errors.c alongside
// the rest of the parser's error-handling machinery). See
// include/parser/parser_actions.h for the rationale and the declarations
// parser.tab.c links against.
//
// This is a pure C-extraction refactor: every function body below is
// unchanged from its prior home in parser.y (only `static` was dropped,
// since these are now called from a different translation unit).

#include "parser/parser_actions.h"

#include "ast.h"
#include "lexer.h"
#include "token.h"

// Bison token macros (PLUS, MINUS, ASSIGN, ...) for bison_token_to_token_type.
#include "parser.tab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global AST root, set by the `program` grammar rule.
ASTNode* ast_root = NULL;

// See the header for why func_signature stashes params/result here.
ASTNode* g_func_signature_params = NULL;
ASTNode* g_func_signature_result = NULL;

// Defined in src/parser/lexer_bridge.c; each consumer translation unit
// declares its own extern (mirrors parser.y's own declaration for yyerror).
extern Lexer* current_lexer;

// Go grouped-name shorthand `(x, y int)` == `(x int, y int)`, applied to a
// parsed parameter or result list. The func_param machinery has no comma-
// shared-type rule, so a leading `x` (no type of its own) parses as an
// ANONYMOUS entry whose `type` is the bare type-name `x`. Reinterpret each such
// entry as a NAME borrowing the type of the next explicitly-typed named entry
// to its right — matching Go's `IdentifierList Type` group. Only runs when the
// list already has a truly-named entry, so a genuinely anonymous list
// `(int, int)` is left untouched. Each borrowed type is DEEP-CLONED so every
// VarDecl owns its own node (AST teardown frees each ->type once). Action-only
// (grammar unchanged), so the parser conflict count is unaffected.
void reinterpret_grouped_names(ASTNode* list) {
    int grp_has_named = 0;
    for (ASTNode* p = list; p; p = p->next) {
        VarDeclNode* vd = (VarDeclNode*)p;
        if (vd->name_count > 0 && vd->names && vd->names[0]) { grp_has_named = 1; break; }
    }
    if (!grp_has_named) return;
    for (ASTNode* p = list; p; p = p->next) {
        VarDeclNode* vd = (VarDeclNode*)p;
        if (vd->name_count > 0) continue;              // already a name
        if (!vd->type || vd->type->type != AST_BASIC_TYPE) continue; // not a bare-name candidate
        ASTNode* shared = NULL;                        // the group's declared type
        for (ASTNode* q = p->next; q; q = q->next) {
            VarDeclNode* qd = (VarDeclNode*)q;
            if (qd->name_count > 0 && qd->type) { shared = qd->type; break; }
        }
        if (!shared) continue;                         // no group type to the right
        ASTNode* cloned = ast_type_clone(shared);
        if (!cloned) continue;                         // unclonable shared type: leave as-is
        BasicTypeNode* bt = (BasicTypeNode*)vd->type;  // misparsed type-name == the intended name
        vd->names = (char**)malloc(sizeof(char*));
        vd->names[0] = strdup(bt->name);
        vd->name_count = 1;
        ast_node_free(vd->type);                       // drop the misparsed bare type-name
        vd->type = cloned;
    }
}

// F4: correct deep-copy of a constant-expression node. Deliberately NOT
// ast_node_copy(): that helper allocates only sizeof(ASTNode) and then writes
// derived-struct fields past the allocation (a latent heap overflow), so it
// cannot be used to clone Literal/Identifier/Binary/Unary nodes. Here we use
// the real constructors, which allocate the correct struct size. Covers the
// const-expr surface F4 supports; anything else (calls, selectors, indexing)
// returns NULL — a bare spec repeating such a value is left without an
// initializer and rejected downstream with the normal clean error.
ASTNode* clone_const_value(const ASTNode* n) {
    if (!n) return NULL;
    switch (n->type) {
        case AST_IDENTIFIER: {
            IdentifierNode* src = (IdentifierNode*)n;
            return (ASTNode*)ast_identifier_new(src->name, n->pos);
        }
        case AST_LITERAL: {
            LiteralNode* src = (LiteralNode*)n;
            // Preserve the exact byte length so a repeated const string spec
            // keeps embedded NULs (ast_literal_new's str_dup would truncate).
            if (src->literal_type == TOKEN_STRING)
                return (ASTNode*)ast_string_literal_new(src->value, src->length, n->pos);
            return (ASTNode*)ast_literal_new(src->literal_type, src->value, n->pos);
        }
        case AST_BINARY_EXPR: {
            BinaryExprNode* src = (BinaryExprNode*)n;
            ASTNode* l = clone_const_value(src->left);
            ASTNode* r = clone_const_value(src->right);
            return (ASTNode*)ast_binary_expr_new(l, src->operator, r, n->pos);
        }
        case AST_UNARY_EXPR: {
            UnaryExprNode* src = (UnaryExprNode*)n;
            ASTNode* operand = clone_const_value(src->operand);
            return (ASTNode*)ast_unary_expr_new(src->operator, operand, n->pos);
        }
        default:
            return NULL;
    }
}

// F4: replace the identifier `iota` with an integer literal equal to `idx`,
// recursively, inside a const spec's value expression. Handles the constant-
// expression surface that iota appears in — a bare `iota`, and `iota` nested
// in binary/unary expressions (e.g. `iota * 2`, `-iota`; and `1 << iota` once
// bitwise operators land — a separate gap). Parenthesised exprs need no case:
// `( e )` reduces to `e` directly (no wrapper node). `iota` buried in a
// call/index/selector is left untouched and will surface as an ordinary
// "undefined variable 'iota'" error — acceptable for v1 (those forms are
// vanishingly rare in real const blocks).
void substitute_iota(ASTNode** slot, long idx) {
    if (!slot || !*slot) return;
    ASTNode* n = *slot;

    if (n->type == AST_IDENTIFIER) {
        IdentifierNode* id = (IdentifierNode*)n;
        if (id->name && strcmp(id->name, "iota") == 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%ld", idx);
            ASTNode* lit = (ASTNode*)ast_literal_new(TOKEN_INT, buf, n->pos);
            lit->next = n->next;   // preserve sibling chain (NULL in expr context)
            n->next = NULL;        // detach so the free below doesn't recurse
            ast_node_free(n);
            *slot = lit;
        }
        return;
    }

    switch (n->type) {
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)n;
            substitute_iota(&b->left, idx);
            substitute_iota(&b->right, idx);
            break;
        }
        case AST_UNARY_EXPR: {
            UnaryExprNode* u = (UnaryExprNode*)n;
            substitute_iota(&u->operand, idx);
            break;
        }
        default:
            break;
    }
}

// F4: build one ConstDeclNode for a grouped-const spec. `value` may be NULL
// for a bare spec (filled in later by desugar_const_group). Mirrors the
// single-const construction in the const_decl rule.
ASTNode* const_spec_new(ASTNode* name_ident, ASTNode* value) {
    ConstDeclNode* c = (ConstDeclNode*)malloc(sizeof(ConstDeclNode));
    c->base.type = AST_CONST_DECL;
    c->base.pos = get_current_position();
    c->base.node_type = NULL;
    c->base.next = NULL;

    IdentifierNode* ident = (IdentifierNode*)name_ident;
    c->names = malloc(sizeof(char*));
    c->names[0] = strdup(ident->name);
    c->name_count = 1;
    c->type = NULL;
    c->values = value;
    c->is_comptime = 0;

    ast_node_free(name_ident);
    return (ASTNode*)c;
}

// F4: turn a chain of grouped-const specs into a chain of ordinary single
// const decls. Walks the specs in order, tracking the iota ordinal and the
// pristine (pre-substitution) value template for bare-spec repetition, then
// substitutes iota into each spec's value. A leading bare spec (no prior
// value) keeps values==NULL and is rejected downstream as a const without an
// initializer — the same clean error the single-const path already gives.
ASTNode* desugar_const_group(ASTNode* spec_chain) {
    ASTNode* template = NULL;  // owned pristine (pre-iota) clone of last value
    long idx = 0;

    for (ASTNode* n = spec_chain; n; n = n->next) {
        ConstDeclNode* c = (ConstDeclNode*)n;

        if (c->values == NULL) {
            // Bare spec: repeat the previous value with this spec's iota.
            if (template) {
                c->values = clone_const_value(template);
            }
        } else {
            // Fresh value: snapshot it (pre-iota) so following bare specs
            // repeat THIS expression with their own iota, then substitute
            // into the spec's own value in place.
            if (template) ast_node_free(template);
            template = clone_const_value(c->values);
        }

        if (c->values) {
            substitute_iota(&c->values, idx);
        }
        idx++;
    }

    if (template) ast_node_free(template);
    return spec_chain;
}

// P1.2: build one VarDeclNode for a grouped-var spec. Exactly one of `type`
// / `value` may be NULL (never both — var_spec's grammar has no bare
// `identifier`-alone arm) depending on which of the three spec forms
// matched. Mirrors const_spec_new's shape; unlike it, nothing is deferred
// for a later desugar pass to fill in.
ASTNode* var_spec_new(ASTNode* name_ident, ASTNode* type, ASTNode* value) {
    VarDeclNode* var = ast_var_decl_new(get_current_position());

    IdentifierNode* ident = (IdentifierNode*)name_ident;
    var->names = malloc(sizeof(char*));
    var->names[0] = strdup(ident->name);
    var->name_count = 1;
    var->type = type;
    var->values = value;

    ast_node_free(name_ident);
    return (ASTNode*)var;
}

// P1.2: turn a chain of grouped-var specs into a chain of ordinary single
// var decls. Unlike desugar_const_group, there is no iota ordinal and no
// value-inheritance pass to run here: every var_spec already built a
// complete, self-contained VarDeclNode (var_spec_new, above). Kept as its
// own function — mirroring desugar_const_group's shape/call site in the
// VAR LPAREN var_spec_list RPAREN arm — as the natural extension point if
// grouped var ever needs a group-wide pass (e.g. shared type inference
// across specs); today it is deliberately a pass-through.
ASTNode* desugar_var_group(ASTNode* spec_chain) {
    return spec_chain;
}

// F6: build a 2-target/2-value MultiAssignNode. Targets and values are each
// chained via ->next (t1->t2, v1->v2). For `:=` the targets are new names;
// for `=` they are existing lvalues. Simultaneous-eval semantics are enforced
// in codegen, not here.
ASTNode* multi_assign_2_new(ASTNode* t1, ASTNode* t2,
                             ASTNode* v1, ASTNode* v2, int is_short_decl) {
    MultiAssignNode* ma = (MultiAssignNode*)malloc(sizeof(MultiAssignNode));
    ma->base.type = AST_MULTI_ASSIGN;
    ma->base.pos = get_current_position();
    ma->base.node_type = NULL;
    ma->base.next = NULL;

    t1->next = t2;
    t2->next = NULL;
    v1->next = v2;
    v2->next = NULL;

    ma->targets = t1;
    ma->values = v1;
    ma->count = 2;
    ma->is_short_decl = is_short_decl;
    return (ASTNode*)ma;
}

ASTNode* multi_assign_call_new(ASTNode* t1, ASTNode* t2, ASTNode* call) {
    MultiAssignNode* ma = (MultiAssignNode*)malloc(sizeof(MultiAssignNode));
    ma->base.type = AST_MULTI_ASSIGN;
    ma->base.pos = get_current_position();
    ma->base.node_type = NULL;
    ma->base.next = NULL;

    t1->next = t2;
    t2->next = NULL;
    call->next = NULL;      /* single value; codegen destructures its fields */

    ma->targets = t1;
    ma->values = call;
    ma->count = 2;
    ma->is_short_decl = 0;
    return (ASTNode*)ma;
}

// Build a compound-assignment statement (`x += e`, `x &= e`, ...). The compound
// operator token (TOKEN_PLUS_ASSIGN etc.) is kept on the BinaryExprNode and
// lowered in codegen to load-op-store — this avoids duplicating the LHS AST
// (there is no safe deep-copy: ast_node_copy under-allocates). Wrapped in an
// ExprStmt to match the plain-assignment shape.
ASTNode* compound_assign_stmt(ASTNode* lhs, TokenType op, ASTNode* rhs) {
    BinaryExprNode* binary = ast_binary_expr_new(lhs, op, rhs, get_current_position());
    ExprStmtNode* es = (ExprStmtNode*)malloc(sizeof(ExprStmtNode));
    es->base.type = AST_EXPR_STMT;
    es->base.pos = get_current_position();
    es->base.node_type = NULL;
    es->base.next = NULL;
    es->expr = (ASTNode*)binary;
    return (ASTNode*)es;
}

ASTNode* struct_literal_new(char* type_name_owned, ASTNode* inits) {
    StructLiteralNode* lit = (StructLiteralNode*)calloc(1, sizeof(StructLiteralNode));
    lit->base.type = AST_STRUCT_LITERAL;
    lit->base.pos = get_current_position();
    lit->type_name = type_name_owned;  /* NULL => elided (type inferred) */
    lit->field_values = inits;
    lit->field_count = 0;
    for (ASTNode* a = inits; a; a = a->next) lit->field_count++;
    /* Recover the field names struct_lit_init stashed on each init's node_type
       slot. NULL entry = positional init. is_keyed = any name present. */
    lit->is_keyed = 0;
    lit->field_names = NULL;
    if (lit->field_count > 0) {
        lit->field_names = (char**)calloc(lit->field_count, sizeof(char*));
        size_t i = 0;
        for (ASTNode* a = inits; a; a = a->next, i++) {
            lit->field_names[i] = (char*)a->node_type;
            a->node_type = NULL;
            if (lit->field_names[i]) lit->is_keyed = 1;
        }
        if (!lit->is_keyed) {
            free(lit->field_names);
            lit->field_names = NULL;
        }
    }
    return (ASTNode*)lit;
}

ASTNode* map_literal_new(ASTNode* map_type_node, ASTNode* entries) {
    MapLitNode* lit = (MapLitNode*)malloc(sizeof(MapLitNode));
    lit->base.type = AST_PAREN_EXPR;
    lit->base.pos = get_current_position();
    lit->base.node_type = NULL;
    lit->base.next = NULL;
    lit->map_type = map_type_node;
    if (entries) {
        lit->keys = entries;
        lit->values = (ASTNode*)((ASTNode*)entries)->node_type;
        ((ASTNode*)entries)->node_type = NULL;
    } else {
        lit->keys = NULL;
        lit->values = NULL;
    }
    return (ASTNode*)lit;
}

// Rule-action hoists (thin-fat-actions pass) ------------------------------
// See include/parser/parser_actions.h for the per-function provenance
// (which parser.y rule arm(s) each one replaces).

ASTNode* func_result_from_params(ASTNode* params_list) {
    ASTNode* list = params_list;
    // Grouped named results: Go's `(x, y int)` shorthand. Shared with the
    // parameter path via reinterpret_grouped_names (see its definition).
    reinterpret_grouped_names(list);
    size_t count = 0; int any_named = 0;
    for (ASTNode* p = list; p; p = p->next) {
        count++;
        VarDeclNode* vd = (VarDeclNode*)p;
        if (vd->name_count > 0 && vd->names && vd->names[0]) any_named = 1;
    }
    if (count == 1 && !any_named) {
        VarDeclNode* only = (VarDeclNode*)list;
        ASTNode* t = only->type;
        only->type = NULL;     // hand the type to the caller before freeing the wrapper
        ast_node_free(list);   // frees the lone wrapper VarDecl, not the type
        return t;
    } else {
        size_t idx = 0;
        for (ASTNode* p = list; p; p = p->next, idx++) {
            VarDeclNode* vd = (VarDeclNode*)p;
            if (vd->name_count == 0 || !vd->names) {
                char buf[16]; snprintf(buf, sizeof(buf), "_%zu", idx);
                vd->names = malloc(sizeof(char*));
                vd->names[0] = strdup(buf); vd->name_count = 1;
            }
        }
        StructTypeNode* st = (StructTypeNode*)malloc(sizeof(StructTypeNode));
        st->base.type = AST_STRUCT_TYPE;
        st->base.pos = get_current_position();
        st->base.node_type = NULL;
        st->base.next = NULL;
        st->fields = list;
        st->is_result_tuple = 1;   // parser-synthesized result list
        return (ASTNode*)st;
    }
}

ASTNode* const_decl_new(ASTNode* name_ident, ASTNode* type, ASTNode* value, int is_comptime) {
    ConstDeclNode* const_node = (ConstDeclNode*)malloc(sizeof(ConstDeclNode));
    const_node->base.type = AST_CONST_DECL;
    const_node->base.pos = get_current_position();
    const_node->base.node_type = NULL;
    const_node->base.next = NULL;

    IdentifierNode* ident = (IdentifierNode*)name_ident;
    const_node->names = malloc(sizeof(char*));
    const_node->names[0] = strdup(ident->name);
    const_node->name_count = 1;
    const_node->type = type;
    const_node->values = value;
    const_node->is_comptime = is_comptime;

    ast_node_free(name_ident);
    return (ASTNode*)const_node;
}

ASTNode* call_expr_new(ASTNode* function, ASTNode* args, int has_spread) {
    CallExprNode* call = (CallExprNode*)malloc(sizeof(CallExprNode));
    call->base.type = AST_CALL_EXPR;
    call->base.pos = get_current_position();
    call->base.node_type = NULL;
    call->base.next = NULL;
    call->function = function;
    call->args = args;
    call->has_spread = has_spread;
    call->type_args = NULL;      // Function generics Task 6
    call->type_arg_count = 0;
    call->comptime_value_args = NULL;   // Comptime value params (fix round 3)
    call->comptime_value_arg_count = 0;
    return (ASTNode*)call;
}

ASTNode* index_expr_new(ASTNode* expr, ASTNode* index) {
    IndexExprNode* node = (IndexExprNode*)malloc(sizeof(IndexExprNode));
    node->base.type = AST_INDEX_EXPR;
    node->base.pos = get_current_position();
    node->base.node_type = NULL;
    node->base.next = NULL;
    node->expr = expr;
    node->index = index;
    return (ASTNode*)node;
}

ASTNode* slice_index_expr_new(ASTNode* expr, ASTNode* low, ASTNode* high) {
    SliceIndexExprNode* slice = (SliceIndexExprNode*)malloc(sizeof(SliceIndexExprNode));
    slice->base.type = AST_SLICE_INDEX_EXPR;
    slice->base.pos = get_current_position();
    slice->base.node_type = NULL;
    slice->base.next = NULL;
    slice->expr = expr;
    slice->low = low;
    slice->high = high;
    return (ASTNode*)slice;
}

ASTNode* selector_expr_new(ASTNode* expr, ASTNode* ident_node) {
    IdentifierNode* ident = (IdentifierNode*)ident_node;
    SelectorExprNode* selector = (SelectorExprNode*)malloc(sizeof(SelectorExprNode));
    selector->base.type = AST_SELECTOR_EXPR;
    selector->base.pos = get_current_position();
    selector->base.node_type = NULL;
    selector->base.next = NULL;
    selector->expr = expr;
    selector->selector = strdup(ident->name);
    ast_node_free(ident_node);
    return (ASTNode*)selector;
}

ASTNode* type_assert_expr_new(ASTNode* expr, ASTNode* asserted_type) {
    TypeAssertNode* ta = (TypeAssertNode*)malloc(sizeof(TypeAssertNode));
    ta->base.type = AST_TYPE_ASSERT;
    ta->base.pos = get_current_position();
    ta->base.node_type = NULL;
    ta->base.next = NULL;
    ta->expr = expr;
    ta->asserted_type = asserted_type;
    return (ASTNode*)ta;
}

ASTNode* func_param_new(ASTNode* name_ident, ASTNode* type, int is_variadic, int is_comptime) {
    VarDeclNode* param = ast_var_decl_new(get_current_position());
    if (name_ident) {
        IdentifierNode* ident = (IdentifierNode*)name_ident;
        param->names = malloc(sizeof(char*));
        param->names[0] = strdup(ident->name);
        param->name_count = 1;
    } else {
        param->names = NULL;
        param->name_count = 0;
    }
    param->type = type;
    param->values = NULL; // Parameters don't have initial values
    param->is_variadic_param = is_variadic;
    param->is_comptime_param = is_comptime;
    if (name_ident) ast_node_free(name_ident);
    return (ASTNode*)param;
}

ASTNode* var_decl_new_1(ASTNode* name_ident, ASTNode* type, ASTNode* value) {
    VarDeclNode* var = ast_var_decl_new(get_current_position());
    IdentifierNode* ident = (IdentifierNode*)name_ident;
    var->names = malloc(sizeof(char*));
    var->names[0] = strdup(ident->name);
    var->name_count = 1;
    var->type = type;
    var->values = value;
    ast_node_free(name_ident);
    return (ASTNode*)var;
}

ASTNode* var_decl_new_2(ASTNode* name_ident1, ASTNode* name_ident2, ASTNode* type) {
    VarDeclNode* var = ast_var_decl_new(get_current_position());
    IdentifierNode* i1 = (IdentifierNode*)name_ident1;
    IdentifierNode* i2 = (IdentifierNode*)name_ident2;
    var->names = malloc(sizeof(char*) * 2);
    var->names[0] = strdup(i1->name);
    var->names[1] = strdup(i2->name);
    var->name_count = 2;
    var->type = type;
    ast_node_free(name_ident1);
    ast_node_free(name_ident2);
    return (ASTNode*)var;
}

ASTNode* var_decl_new_3(ASTNode* name_ident1, ASTNode* name_ident2, ASTNode* name_ident3, ASTNode* type) {
    VarDeclNode* var = ast_var_decl_new(get_current_position());
    IdentifierNode* i1 = (IdentifierNode*)name_ident1;
    IdentifierNode* i2 = (IdentifierNode*)name_ident2;
    IdentifierNode* i3 = (IdentifierNode*)name_ident3;
    var->names = malloc(sizeof(char*) * 3);
    var->names[0] = strdup(i1->name);
    var->names[1] = strdup(i2->name);
    var->names[2] = strdup(i3->name);
    var->name_count = 3;
    var->type = type;
    ast_node_free(name_ident1);
    ast_node_free(name_ident2);
    ast_node_free(name_ident3);
    return (ASTNode*)var;
}

ASTNode* short_var_decl_new_1(ASTNode* name_ident, ASTNode* value) {
    VarDeclNode* var = ast_var_decl_new(get_current_position());
    IdentifierNode* ident = (IdentifierNode*)name_ident;
    var->names = malloc(sizeof(char*));
    var->names[0] = strdup(ident->name);
    var->name_count = 1;
    var->values = value;
    var->is_short_decl = 1;
    ast_node_free(name_ident);
    return (ASTNode*)var;
}

ASTNode* short_var_decl_new_2(ASTNode* name_ident1, ASTNode* name_ident2, ASTNode* value) {
    // Multi-LHS short var decl `a, b := expr`. Destructuring is resolved at
    // codegen time: the RHS must produce a TYPE_STRUCT with at least 2
    // fields, and a/b are bound to its fields 0 and 1 respectively.
    VarDeclNode* var = ast_var_decl_new(get_current_position());
    IdentifierNode* i1 = (IdentifierNode*)name_ident1;
    IdentifierNode* i2 = (IdentifierNode*)name_ident2;
    var->names = malloc(sizeof(char*) * 2);
    var->names[0] = strdup(i1->name);
    var->names[1] = strdup(i2->name);
    var->name_count = 2;
    var->values = value;
    var->is_short_decl = 1;
    ast_node_free(name_ident1);
    ast_node_free(name_ident2);
    return (ASTNode*)var;
}

ASTNode* struct_field_new(ASTNode* name_ident, ASTNode* type) {
    // Reuse VarDeclNode for fields — same shape as a function parameter.
    // type_from_ast for AST_STRUCT_TYPE will walk this chain and build the
    // Type's struct_type.fields[].
    IdentifierNode* ident = (IdentifierNode*)name_ident;
    VarDeclNode* field = ast_var_decl_new(get_current_position());
    field->names = malloc(sizeof(char*));
    field->names[0] = strdup(ident->name);
    field->name_count = 1;
    field->type = type;
    field->values = NULL;
    ast_node_free(name_ident);
    return (ASTNode*)field;
}

ASTNode* slice_lit_new(ASTNode* elements, ASTNode* elem_type) {
    SliceLitNode* lit = (SliceLitNode*)malloc(sizeof(SliceLitNode));
    lit->base.type = AST_SLICE_EXPR;
    lit->base.pos = get_current_position();
    lit->base.node_type = NULL;
    lit->base.next = NULL;
    lit->elements = elements;
    lit->elem_type = elem_type;
    return (ASTNode*)lit;
}

ASTNode* array_lit_new(ASTNode* elements, ASTNode* array_type) {
    ArrayLitNode* lit = (ArrayLitNode*)malloc(sizeof(ArrayLitNode));
    lit->base.type = AST_ARRAY_LITERAL;
    lit->base.pos = get_current_position();
    lit->base.node_type = NULL;
    lit->base.next = NULL;
    lit->elements = elements;
    lit->array_type = array_type;
    return (ASTNode*)lit;
}

ASTNode* struct_lit_empty_new(ASTNode* type_ident) {
    IdentifierNode* type_ident_node = (IdentifierNode*)type_ident;
    StructLiteralNode* lit = (StructLiteralNode*)calloc(1, sizeof(StructLiteralNode));
    lit->base.type = AST_STRUCT_LITERAL;
    lit->base.pos = get_current_position();
    lit->type_name = strdup(type_ident_node->name);
    ast_node_free(type_ident);
    lit->is_keyed = 0;
    lit->field_values = NULL;
    lit->field_names = NULL;
    lit->field_count = 0;
    return (ASTNode*)lit;
}

ASTNode* struct_lit_init_keyed(ASTNode* key_ident, ASTNode* value) {
    // Keyed init. Stash the owned field-name string on the init's node_type
    // slot (same parse-time piggyback map_entry_list uses for its values
    // chain); struct_lit's reducer moves it into field_names[] and clears
    // the slot before type-check runs.
    IdentifierNode* key = (IdentifierNode*)key_ident;
    ASTNode* result = value;
    result->node_type = (Type*)strdup(key->name);
    ast_node_free(key_ident);
    return result;
}

ASTNode* map_entry_new(ASTNode* key, ASTNode* value) {
    // The KEY node is returned. The matching VALUE is stashed on
    // key->node_type as a side-channel; map_entry_list extracts and
    // re-chains it into a parallel values list.
    ASTNode* k = key;
    k->node_type = (Type*)value;
    return k;
}

ASTNode* map_entry_list_append(ASTNode* keys_head, ASTNode* new_key) {
    // Append key to keys-chain; append value to values-chain (the
    // values-chain head is stashed on the keys-list-head's node_type field
    // so we can recover both from one stack value).
    ASTNode* values_head = (ASTNode*)keys_head->node_type;
    ASTNode* new_val = (ASTNode*)new_key->node_type;
    new_key->node_type = NULL;
    ast_add_child(keys_head, new_key);
    ast_add_child(values_head, new_val);
    return keys_head;
}

// catch_expr: the P2.9 value-yielding arrow arm, `expression CATCH FAT_ARROW
// expression`. `f() catch => -1` has no bound error variable — the fallback
// expression itself is the handler's recovery value. Reuses the existing
// value-producing catch machinery (type_check_catch_expr's trailing-expr
// check; generate_catch_body_value's PHI merge) by wrapping `fallback` in a
// synthetic one-statement block (an ExprStmt), the exact AST shape those two
// functions already expect from `operand catch e { fallback }` — this is
// sugar over proven machinery, not a new code path.
CatchExprNode* catch_expr_arrow_new(ASTNode* operand, ASTNode* fallback) {
    ExprStmtNode* es = (ExprStmtNode*)malloc(sizeof(ExprStmtNode));
    es->base.type = AST_EXPR_STMT;
    es->base.pos = get_current_position();
    es->base.node_type = NULL;
    es->base.next = NULL;
    es->expr = fallback;

    BlockStmtNode* block = ast_block_stmt_new(get_current_position());
    block->statements = (ASTNode*)es;

    // No bound error variable (NULL) — ast_catch_expr_new's str_dup(NULL)
    // safely yields NULL, matching every `catch_expr->error_var` guard in
    // the type checker and codegen (both already skip the binding when NULL).
    return ast_catch_expr_new(operand, NULL, (ASTNode*)block, get_current_position());
}

// Helper function to get current position
Position get_current_position(void) {
    Position pos = {1, 1, 0, "<unknown>"};
    if (current_lexer) {
        pos = current_lexer->pos;
    }
    return pos;
}

// Convert Bison tokens to TokenType enum values
TokenType bison_token_to_token_type(int bison_token) {
    switch (bison_token) {
        case PLUS: return TOKEN_PLUS;
        case MINUS: return TOKEN_MINUS;
        case MULTIPLY: return TOKEN_MULTIPLY;
        case DIVIDE: return TOKEN_DIVIDE;
        case MODULO: return TOKEN_MODULO;
        case ASSIGN: return TOKEN_ASSIGN;
        case SHORT_ASSIGN: return TOKEN_SHORT_ASSIGN;
        case EQ: return TOKEN_EQ;
        case NE: return TOKEN_NE;
        case LT: return TOKEN_LT;
        case LE: return TOKEN_LE;
        case GT: return TOKEN_GT;
        case GE: return TOKEN_GE;
        case AND: return TOKEN_AND;
        case OR: return TOKEN_OR;
        case NOT: return TOKEN_NOT;
        case BIT_AND: return TOKEN_BIT_AND;
        case AND_NOT: return TOKEN_AND_NOT;
        case BIT_OR: return TOKEN_BIT_OR;
        case BIT_XOR: return TOKEN_BIT_XOR;
        case BIT_NOT: return TOKEN_BIT_NOT;
        case LSHIFT: return TOKEN_LSHIFT;
        case RSHIFT: return TOKEN_RSHIFT;
        case ARROW: return TOKEN_ARROW;
        case INCREMENT: return TOKEN_INCREMENT;
        case DECREMENT: return TOKEN_DECREMENT;
        default: return TOKEN_UNKNOWN;
    }
}
