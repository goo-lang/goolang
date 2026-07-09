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
