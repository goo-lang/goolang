// returns_fresh: does the caller OWN what a function returns? (finding C2)
// See include/returns_fresh.h for the contract.

#include "returns_fresh.h"
#include "shim_signatures.h"
#include "token.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>

// One function, the way param_escape registered it: a top-level AST_FUNC_DECL
// with a name, in declaration order. Index i matches pe->summaries[i].
typedef struct {
    const char*         name;   // borrowed from the AST
    const FuncDeclNode* decl;   // borrowed from the AST
} FreshFunc;

typedef struct {
    FreshFunc* items;
    size_t     count;
} FreshFuncs;

// Any function with that name, a method included: a user `make` or `new`
// shadows the builtin.
static bool funcs_have_name(const FreshFuncs* funcs, const char* name) {
    for (size_t i = 0; i < funcs->count; i++) {
        if (strcmp(funcs->items[i].name, name) == 0) { return true; }
    }
    return false;
}

// THE QUESTION IS NOT return_escapes'. That bit asks "is the result derived
// from one of F's PARAMETERS", and a value from a global, or one F also stored
// in a container, has no parameter taint. ARC read "no" as "owned" and freed
// GS[0] and r.items[0]. Ownership needs a positive proof instead, and every
// shape this section does not recognise answers "borrowed", which only leaks.
//
// A return path proves ownership when it gives:
//   - a fresh allocation: &T{}, new, make, a composite literal, or a string
//     concatenation (goo_string_concat copies both operands into new memory);
//   - a literal, nil included, which no release can free;
//   - the result of a shim whose table row says it does not alias an argument;
//   - the result of a Goo function that is itself returns_fresh;
//   - a local whose every binding is one of the above, whose address is never
//     taken, and whose only escape reason is RETURN.
//
// THE FIXPOINT RUNS ON THE DUAL. A recursive function (`return mk(n - 1)`)
// depends on its own answer. Starting "fresh" at false and only raising it can
// never prove such a function, because the recursive path waits on itself.
// So the iteration computes "MAY return a borrowed value", which starts false
// and only rises when a path reaches a borrowed source -- the usual least
// fixpoint of a may-analysis. returns_fresh is its negation. It is sound
// because a value that a terminating call returns comes from a finite chain of
// calls, and the last call in that chain returned it by a non-recursive path.
//
// It runs AFTER param_escape's escape fixpoint, not inside it. The local rule
// needs local_escape's reasons, and local_escape needs finished parameter
// summaries. Nothing in that fixpoint reads returns_fresh, so the order changes
// no answer. It is also why this pass is its own unit: param_escape cannot
// depend on local_escape, which already depends on param_escape.

typedef struct {
    const char* name;   // borrowed from the AST
    ASTNode*    value;  // the bound value, or NULL when the walk cannot name one
} FreshBinding;

typedef struct {
    ASTNode* value;
    bool     local_in_scope;  // an identifier that names a local at this return
} FreshReturn;

// One function's walk. Every array borrows its strings from the AST.
typedef struct {
    const char**  scope;        // locals declared in enclosing blocks, innermost last
    size_t        scope_len, scope_cap;
    FreshBinding* bindings;     // EVERY binding of every local, across all scopes
    size_t        binding_count, binding_cap;
    const char**  addr_taken;   // every name that `&name` or a closure reaches
    size_t        addr_count, addr_cap;
    FreshReturn*  returns;
    size_t        return_count, return_cap;
    bool          bare_return;  // a bare `return` in a function with results
    bool          unreadable;   // a construct this walk does not model was met
} FreshScan;

static void scan_push_name(const char*** arr, size_t* len, size_t* cap, const char* name) {
    if (*len >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        void* grown = *arr;
        *arr = xrealloc(grown, *cap * sizeof(**arr));
    }
    (*arr)[(*len)++] = name;
}

static void scan_declare(FreshScan* s, const char* name) {
    if (!name || strcmp(name, "_") == 0) { return; }
    scan_push_name(&s->scope, &s->scope_len, &s->scope_cap, name);
}

static void scan_bind(FreshScan* s, const char* name, ASTNode* value) {
    if (!name || strcmp(name, "_") == 0) { return; }
    if (s->binding_count >= s->binding_cap) {
        s->binding_cap = s->binding_cap ? s->binding_cap * 2U : 8U;
        s->bindings = xrealloc(s->bindings, s->binding_cap * sizeof(*s->bindings));
    }
    s->bindings[s->binding_count++] = (FreshBinding){ .name = name, .value = value };
}

static bool scan_in_scope(const FreshScan* s, const char* name) {
    for (size_t i = s->scope_len; i > 0U; i--) {
        if (strcmp(s->scope[i - 1U], name) == 0) { return true; }
    }
    return false;
}

static bool fresh_is_assign_token(TokenType op) {
    switch (op) {
        case TOKEN_ASSIGN:
        case TOKEN_SHORT_ASSIGN:
        case TOKEN_PLUS_ASSIGN:
        case TOKEN_MINUS_ASSIGN:
        case TOKEN_MUL_ASSIGN:
        case TOKEN_DIV_ASSIGN:
        case TOKEN_MOD_ASSIGN:
        case TOKEN_AND_ASSIGN:
        case TOKEN_OR_ASSIGN:
        case TOKEN_XOR_ASSIGN:
        case TOKEN_LSHIFT_ASSIGN:
        case TOKEN_RSHIFT_ASSIGN:
            return true;
        default:
            return false;
    }
}

// Collects every `&name`. A write through a pointer to a local changes what
// the local holds without a binding this walk can see, so such a local proves
// nothing. An expression kind not listed here marks the function unreadable:
// it might hide exactly such a pointer.
static void scan_expr(FreshScan* s, ASTNode* e) {
    if (!e) { return; }
    switch (e->type) {
        case AST_IDENTIFIER:
        case AST_LITERAL:
            return;
        // A type is a leaf: `make([]T, n)` and `new(T)` pass one as an argument.
        case AST_BASIC_TYPE:
        case AST_ARRAY_TYPE:
        case AST_SLICE_TYPE:
        case AST_MAP_TYPE:
        case AST_CHAN_TYPE:
        case AST_FUNC_TYPE:
        case AST_INTERFACE_TYPE:
        case AST_STRUCT_TYPE:
        case AST_POINTER_TYPE:
        case AST_REFERENCE_TYPE:
        case AST_ERROR_UNION_TYPE:
        case AST_NULLABLE_TYPE:
        case AST_UNSAFE_PTR_TYPE:
            return;
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)e;
            scan_expr(s, b->left);
            scan_expr(s, b->right);
            return;
        }
        case AST_UNARY_EXPR: {
            UnaryExprNode* u = (UnaryExprNode*)e;
            if (u->operator == TOKEN_BIT_AND && u->operand && u->operand->type == AST_IDENTIFIER) {
                scan_push_name(&s->addr_taken, &s->addr_count, &s->addr_cap,
                               ((IdentifierNode*)u->operand)->name);
            }
            scan_expr(s, u->operand);
            return;
        }
        case AST_POSTFIX_EXPR: {
            // `x++` rebinds x to a value no AST node describes.
            ASTNode* op = ((PostfixExprNode*)e)->operand;
            if (op && op->type == AST_IDENTIFIER) {
                scan_bind(s, ((IdentifierNode*)op)->name, NULL);
            }
            scan_expr(s, op);
            return;
        }
        case AST_INDEX_EXPR: {
            IndexExprNode* ie = (IndexExprNode*)e;
            scan_expr(s, ie->expr);
            scan_expr(s, ie->index);
            return;
        }
        case AST_SLICE_INDEX_EXPR: {
            SliceIndexExprNode* se = (SliceIndexExprNode*)e;
            scan_expr(s, se->expr);
            scan_expr(s, se->low);
            scan_expr(s, se->high);
            return;
        }
        case AST_SELECTOR_EXPR:
            scan_expr(s, ((SelectorExprNode*)e)->expr);
            return;
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)e;
            scan_expr(s, call->function);
            for (ASTNode* a = call->args; a; a = a->next) { scan_expr(s, a); }
            return;
        }
        case AST_FUNC_LIT: {
            // A closure holds its captures by reference, which is an address
            // taken. The body is not walked: its returns are not F's.
            const FuncLitNode* lit = (const FuncLitNode*)e;
            for (size_t i = 0; i < lit->captured_count; i++) {
                scan_push_name(&s->addr_taken, &s->addr_count, &s->addr_cap,
                               lit->captured_names[i]);
            }
            return;
        }
        case AST_STRUCT_LITERAL:
            for (ASTNode* v = ((StructLiteralNode*)e)->field_values; v; v = v->next) {
                scan_expr(s, v);
            }
            return;
        case AST_SLICE_EXPR:   // slice literal (SliceLitNode)
            for (ASTNode* v = ((SliceLitNode*)e)->elements; v; v = v->next) { scan_expr(s, v); }
            return;
        case AST_ARRAY_LITERAL:
            for (ASTNode* v = ((ArrayLitNode*)e)->elements; v; v = v->next) { scan_expr(s, v); }
            return;
        case AST_KEYED_ELEMENT:
            scan_expr(s, ((KeyedElementNode*)e)->key);
            scan_expr(s, ((KeyedElementNode*)e)->value);
            return;
        case AST_PAREN_EXPR: { // map literal (MapLitNode)
            MapLitNode* ml = (MapLitNode*)e;
            for (ASTNode* k = ml->keys; k; k = k->next) { scan_expr(s, k); }
            for (ASTNode* v = ml->values; v; v = v->next) { scan_expr(s, v); }
            return;
        }
        case AST_SLICE_CONVERSION:
            scan_expr(s, ((SliceConvNode*)e)->operand);
            return;
        case AST_TYPE_ASSERT:
            scan_expr(s, ((TypeAssertNode*)e)->expr);
            return;
        default:
            s->unreadable = true;
            return;
    }
}

// One value for several names is a multi-result call. A Goo call's results
// can be one object (`return b, b`), so its names get no value to prove fresh.
// A selector call keeps the call as the binding: classify_value accepts only an
// audited shim row there, and every multi-result shim is an error union whose
// two sides are never one object. release_decision's seed_names makes the same
// split.
static ASTNode* multi_result_binding(ASTNode* values, size_t value_count) {
    if (value_count != 1U || values->type != AST_CALL_EXPR) { return NULL; }
    const ASTNode* fn = ((const CallExprNode*)values)->function;
    return (fn && fn->type == AST_SELECTOR_EXPR) ? values : NULL;
}

static void scan_bind_names(FreshScan* s, char** names, size_t name_count, ASTNode* values,
                            bool declare) {
    size_t value_count = 0;
    for (const ASTNode* v = values; v; v = v->next) { value_count++; }
    ASTNode* v = values;
    for (size_t i = 0; i < name_count; i++) {
        ASTNode* bound = (value_count == name_count) ? v
                       : multi_result_binding(values, value_count);
        if (declare) { scan_declare(s, names[i]); }
        scan_bind(s, names[i], bound);
        if (v) { v = v->next; }
    }
}

static void scan_stmts(FreshScan* s, ASTNode* stmt, bool has_results);

// Walks `stmt` in a scope of its own, so the names it declares are gone after.
static void scan_scoped(FreshScan* s, ASTNode* stmt, bool has_results) {
    size_t mark = s->scope_len;
    scan_stmts(s, stmt, has_results);
    s->scope_len = mark;
}

// An assignment expression, from a statement or from a select case's comm.
// Returns true when `e` was one.
static bool scan_assignment(FreshScan* s, ASTNode* e) {
    if (!e || e->type != AST_BINARY_EXPR) { return false; }
    BinaryExprNode* b = (BinaryExprNode*)e;
    if (!fresh_is_assign_token(b->operator)) { return false; }
    if (b->left && b->left->type == AST_IDENTIFIER) {
        const char* name = ((IdentifierNode*)b->left)->name;
        if (b->operator == TOKEN_SHORT_ASSIGN) { scan_declare(s, name); }
        // A compound operator stores `lhs op rhs`, which has no AST node.
        bool plain = b->operator == TOKEN_ASSIGN || b->operator == TOKEN_SHORT_ASSIGN;
        scan_bind(s, name, plain ? b->right : NULL);
    }
    scan_expr(s, b->left);
    scan_expr(s, b->right);
    return true;
}

static void scan_stmts(FreshScan* s, ASTNode* stmt, bool has_results) {
    for (; stmt; stmt = stmt->next) {
        switch (stmt->type) {
            case AST_BLOCK_STMT:
                scan_scoped(s, ((BlockStmtNode*)stmt)->statements, has_results);
                break;

            case AST_EXPR_STMT: {
                ASTNode* e = ((ExprStmtNode*)stmt)->expr;
                if (!scan_assignment(s, e)) { scan_expr(s, e); }
                break;
            }

            case AST_IF_STMT: {
                IfStmtNode* n = (IfStmtNode*)stmt;
                scan_expr(s, n->condition);
                scan_scoped(s, n->then_stmt, has_results);
                scan_scoped(s, n->else_stmt, has_results);
                break;
            }

            case AST_IF_LET_STMT: {
                IfLetStmtNode* n = (IfLetStmtNode*)stmt;
                scan_expr(s, n->nullable_expr);
                size_t mark = s->scope_len;
                scan_declare(s, n->var_name);
                scan_bind(s, n->var_name, NULL);   // aliases what the nullable held
                scan_stmts(s, n->then_stmt, has_results);
                s->scope_len = mark;
                scan_scoped(s, n->else_stmt, has_results);
                break;
            }

            case AST_FOR_STMT: {
                ForStmtNode* n = (ForStmtNode*)stmt;
                size_t mark = s->scope_len;
                if (n->range_expr) {
                    scan_expr(s, n->range_expr);
                    // Range variables are views of the ranged value.
                    scan_declare(s, n->key_name);
                    scan_bind(s, n->key_name, NULL);
                    scan_declare(s, n->value_name);
                    scan_bind(s, n->value_name, NULL);
                } else {
                    scan_stmts(s, n->init, has_results);
                    scan_expr(s, n->condition);
                    scan_stmts(s, n->post, has_results);
                }
                scan_stmts(s, n->body, has_results);
                s->scope_len = mark;
                break;
            }

            case AST_RETURN_STMT: {
                ReturnStmtNode* n = (ReturnStmtNode*)stmt;
                if (!n->values) {
                    if (has_results) { s->bare_return = true; }
                    break;
                }
                for (ASTNode* v = n->values; v; v = v->next) {
                    scan_expr(s, v);
                    if (s->return_count >= s->return_cap) {
                        s->return_cap = s->return_cap ? s->return_cap * 2U : 4U;
                        s->returns = xrealloc(s->returns, s->return_cap * sizeof(*s->returns));
                    }
                    bool local = v->type == AST_IDENTIFIER
                                 && scan_in_scope(s, ((IdentifierNode*)v)->name);
                    s->returns[s->return_count++] = (FreshReturn){ .value = v, .local_in_scope = local };
                }
                break;
            }

            case AST_GO_STMT:
                scan_expr(s, ((GoStmtNode*)stmt)->call);
                break;

            case AST_DEFER_STMT:
                scan_expr(s, ((DeferStmtNode*)stmt)->call);
                break;

            case AST_BREAK_STMT:
            case AST_CONTINUE_STMT:
                break;

            case AST_VAR_DECL: {
                VarDeclNode* n = (VarDeclNode*)stmt;
                for (ASTNode* v = n->values; v; v = v->next) { scan_expr(s, v); }
                scan_bind_names(s, n->names, n->name_count, n->values, true);
                break;
            }

            case AST_CONST_DECL: {
                ConstDeclNode* n = (ConstDeclNode*)stmt;
                for (ASTNode* v = n->values; v; v = v->next) { scan_expr(s, v); }
                // A constant holds no allocation, so it proves nothing.
                for (size_t i = 0; i < n->name_count; i++) {
                    scan_declare(s, n->names[i]);
                    scan_bind(s, n->names[i], NULL);
                }
                break;
            }

            case AST_MULTI_ASSIGN: {
                MultiAssignNode* n = (MultiAssignNode*)stmt;
                size_t tcount = 0, vcount = 0;
                for (ASTNode* t = n->targets; t; t = t->next) { tcount++; scan_expr(s, t); }
                for (ASTNode* v = n->values; v; v = v->next) { vcount++; scan_expr(s, v); }
                ASTNode* v = n->values;
                for (ASTNode* t = n->targets; t; t = t->next) {
                    if (t->type == AST_IDENTIFIER) {
                        const char* name = ((IdentifierNode*)t)->name;
                        if (n->is_short_decl) { scan_declare(s, name); }
                        // A mismatch is a multi-result call: see scan_bind_names.
                        scan_bind(s, name, (tcount == vcount) ? v
                                           : multi_result_binding(n->values, vcount));
                    }
                    if (v) { v = v->next; }
                }
                break;
            }

            case AST_SWITCH_STMT: {
                SwitchStmtNode* n = (SwitchStmtNode*)stmt;
                scan_expr(s, n->tag);
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type != AST_CASE_CLAUSE) { s->unreadable = true; continue; }
                    CaseClauseNode* cc = (CaseClauseNode*)c;
                    for (ASTNode* e = cc->exprs; e; e = e->next) { scan_expr(s, e); }
                    scan_scoped(s, cc->body, has_results);
                }
                break;
            }

            case AST_TYPE_SWITCH: {
                TypeSwitchNode* n = (TypeSwitchNode*)stmt;
                scan_expr(s, n->expr);
                const char* bind = (n->bind_name && n->bind_name->type == AST_IDENTIFIER)
                                   ? ((IdentifierNode*)n->bind_name)->name : NULL;
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type != AST_TYPE_CASE) { s->unreadable = true; continue; }
                    size_t mark = s->scope_len;
                    scan_declare(s, bind);
                    scan_bind(s, bind, NULL);   // a view of the interface's data
                    scan_stmts(s, ((TypeCaseNode*)c)->body, has_results);
                    s->scope_len = mark;
                }
                break;
            }

            case AST_SELECT_STMT: {
                SelectStmtNode* n = (SelectStmtNode*)stmt;
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type != AST_SELECT_CASE) { s->unreadable = true; continue; }
                    SelectCaseNode* sc = (SelectCaseNode*)c;
                    size_t mark = s->scope_len;
                    if (!scan_assignment(s, sc->comm)) { scan_expr(s, sc->comm); }
                    // A received value came from someone else.
                    if (sc->bind_name) {
                        if (sc->is_declare == 1) { scan_declare(s, sc->bind_name); }
                        scan_bind(s, sc->bind_name, NULL);
                    }
                    scan_stmts(s, sc->body, has_results);
                    s->scope_len = mark;
                }
                break;
            }

            case AST_UNSAFE_STMT:
                scan_scoped(s, ((UnsafeStmtNode*)stmt)->body, has_results);
                break;

            case AST_ARENA_BLOCK:
                scan_scoped(s, ((ArenaBlockNode*)stmt)->body, has_results);
                break;

            case AST_ASSERT_STMT: {
                AssertStmtNode* n = (AssertStmtNode*)stmt;
                scan_expr(s, n->condition);
                scan_expr(s, n->message);
                break;
            }

            case AST_ASSUME_STMT:
                scan_expr(s, ((AssumeStmtNode*)stmt)->condition);
                break;

            default:
                // An unmodelled statement might bind a local or return a value.
                s->unreadable = true;
                break;
        }
    }
}

static void scan_free(FreshScan* s) {
    free(s->scope);
    free(s->bindings);
    free(s->addr_taken);
    free(s->returns);
}

bool returns_fresh_addr_taken(const FuncDeclNode* decl, const char*** names_out,
                              size_t* count_out) {
    *names_out = NULL;
    *count_out = 0U;
    if (!decl || !decl->body) {
        return true;
    }
    FreshScan scan = {0};
    scan_stmts(&scan, decl->body, decl->return_type != NULL);
    bool readable = !scan.unreadable;
    // The array moves to the caller; scan_free then frees the rest.
    *names_out = scan.addr_taken;
    *count_out = scan.addr_count;
    scan.addr_taken = NULL;
    scan_free(&scan);
    return readable;
}

// What one function's return paths need: either a path that is borrowed
// whatever the callees say, or the Goo callees whose results it returns.
typedef struct {
    bool         borrowed;
    const char** callees;   // borrowed from the AST
    size_t       callee_count, callee_cap;
} FreshFacts;

typedef struct {
    const FreshFunc*          f;
    const FreshFuncs*         funcs;
    const LocalEscapeSummary* locals;   // NULL when local_escape has none for f
    FreshScan*                scan;
    FreshFacts*               facts;
} FreshClassify;

static bool name_in(const char* const* arr, size_t n, const char* name) {
    for (size_t i = 0; i < n; i++) {
        if (arr[i] && strcmp(arr[i], name) == 0) { return true; }
    }
    return false;
}

// A parameter or a named result. Either one holds a value from outside F's own
// allocations: the caller's argument, or a value the walk has no binding for.
static bool is_param_or_result(const FreshFunc* f, const char* name) {
    // Every parameter name, the receiver included: the parser splices it in as
    // the head of `params`.
    for (const ASTNode* p = f->decl->params; p; p = p->next) {
        if (p->type != AST_VAR_DECL) { continue; }
        const VarDeclNode* vd = (const VarDeclNode*)p;
        if (name_in((const char* const*)vd->names, vd->name_count, name)) { return true; }
    }
    // Named results. A synthetic `_N` is the parser's name for an UNNAMED
    // result and `_` is a discard; neither is a variable, so neither counts,
    // the same filter param_escape's collect_result_names applies.
    const ASTNode* rt = f->decl->return_type;
    if (!rt || rt->type != AST_STRUCT_TYPE) { return false; }
    for (const ASTNode* fld = ((const StructTypeNode*)rt)->fields; fld; fld = fld->next) {
        if (fld->type != AST_VAR_DECL) { continue; }
        const VarDeclNode* vd = (const VarDeclNode*)fld;
        for (size_t i = 0; vd->names && i < vd->name_count; i++) {
            const char* nm = vd->names[i];
            if (!nm || strcmp(nm, "_") == 0 || is_synthetic_result_name(nm)) { continue; }
            if (strcmp(nm, name) == 0) { return true; }
        }
    }
    return false;
}

// Any name that is not a package-level one inside F: a callee spelled this way
// is a closure or a method value, not the function the registry knows.
static bool names_a_variable(const FreshClassify* c, const char* name) {
    if (is_param_or_result(c->f, name)) { return true; }
    for (size_t i = 0; i < c->scan->binding_count; i++) {
        if (strcmp(c->scan->bindings[i].name, name) == 0) { return true; }
    }
    return false;
}

static void facts_add_callee(FreshFacts* facts, const char* name) {
    scan_push_name(&facts->callees, &facts->callee_count, &facts->callee_cap, name);
}

static bool is_fresh_composite(const ASTNode* e) {
    switch (e->type) {
        case AST_STRUCT_LITERAL:
        case AST_SLICE_EXPR:      // slice literal (SliceLitNode)
        case AST_ARRAY_LITERAL:
        case AST_PAREN_EXPR:      // map literal (MapLitNode)
            return true;
        default:
            return false;
    }
}

// Classifies a value that is NOT a bare identifier: fresh, borrowed, or fresh
// if a named Goo callee turns out to be.
static void classify_value(FreshClassify* c, const ASTNode* v) {
    if (!v) { c->facts->borrowed = true; return; }
    if (is_fresh_composite(v)) { return; }
    // A literal is not an allocation, but no release of it can free anything:
    // a string literal's header is GOO_RC_IMMORTAL, nil is NULL, and a scalar
    // has no release slot. release_decision's value_is_release_safe accepts it
    // for the same reason, and a `return ""` error path must not make every
    // other path of the function leak.
    if (v->type == AST_LITERAL) { return; }
    if (v->type == AST_UNARY_EXPR) {
        const UnaryExprNode* u = (const UnaryExprNode*)v;
        if (u->operator == TOKEN_BIT_AND && u->operand && is_fresh_composite(u->operand)) {
            return;
        }
        c->facts->borrowed = true;
        return;
    }
    // goo_string_concat copies both operands into new memory. An integer `+`
    // lands here too, and codegen keeps no release slot for an integer.
    if (v->type == AST_BINARY_EXPR && ((const BinaryExprNode*)v)->operator == TOKEN_PLUS) { return; }
    if (v->type != AST_CALL_EXPR) { c->facts->borrowed = true; return; }

    const ASTNode* fn = ((const CallExprNode*)v)->function;
    if (fn && fn->type == AST_IDENTIFIER) {
        const char* name = ((const IdentifierNode*)fn)->name;
        if (names_a_variable(c, name)) { c->facts->borrowed = true; return; }
        bool builtin_alloc = strcmp(name, "new") == 0 || strcmp(name, "make") == 0;
        if (builtin_alloc && !funcs_have_name(c->funcs, name)) { return; }
        facts_add_callee(c->facts, name);
        return;
    }
    if (fn && fn->type == AST_SELECTOR_EXPR) {
        const SelectorExprNode* sel = (const SelectorExprNode*)fn;
        // A base that is a variable makes this a method call, not a package one.
        // qir.* results are opaque QIR handles with no ARC header, so a
        // non_retaining qir entry proves nothing about ownership. Same refusal
        // as call_result_is_owned in release_decision.c.
        if (sel->expr && sel->expr->type == AST_IDENTIFIER && sel->selector
            && !names_a_variable(c, ((const IdentifierNode*)sel->expr)->name)
            && strcmp(((const IdentifierNode*)sel->expr)->name, "qir") != 0
            && shim_signature_is_non_retaining(((const IdentifierNode*)sel->expr)->name,
                                               sel->selector) != 0) {
            return;
        }
    }
    c->facts->borrowed = true;
}

// A returned local proves ownership only when nothing else can hold or change
// its value: every binding is fresh, no pointer to it exists, and local_escape
// saw it leave F by RETURN alone. Bindings are matched by NAME across every
// scope, so a same-named local elsewhere in F can only make this stricter.
static void classify_local(FreshClassify* c, const FreshReturn* r) {
    const char* name = ((const IdentifierNode*)r->value)->name;
    if (!r->local_in_scope || is_param_or_result(c->f, name)) { c->facts->borrowed = true; return; }
    if (name_in(c->scan->addr_taken, c->scan->addr_count, name)) { c->facts->borrowed = true; return; }

    // A local that local_escape never saw has no verdict: it counts as escaping.
    bool seen = false;
    EscapeReasons why = 0U;
    for (size_t i = 0; c->locals && i < c->locals->local_count; i++) {
        if (strcmp(c->locals->local_names[i], name) == 0) {
            why = c->locals->reasons[i];
            seen = true;
            break;
        }
    }
    if (!seen || (why & ~ESCAPE_REASON_RETURN) != 0U) { c->facts->borrowed = true; return; }

    bool any = false;
    for (size_t i = 0; i < c->scan->binding_count; i++) {
        const FreshBinding* b = &c->scan->bindings[i];
        if (strcmp(b->name, name) != 0) { continue; }
        any = true;
        // classify_value answers borrowed for NULL and for an identifier: the
        // other name may still hold the value, so it is an alias, not an owner.
        classify_value(c, b->value);
    }
    if (!any) { c->facts->borrowed = true; }
}

// The number of result slots. Several results come as an AST_STRUCT_TYPE of
// VarDecl fields, and so does one NAMED result. An anonymous struct result
// type has the same shape and can count as several, which only refuses more.
static size_t result_slot_count(const FuncDeclNode* decl) {
    if (!decl->return_type) { return 0; }
    if (decl->return_type->type != AST_STRUCT_TYPE) { return 1; }
    size_t n = 0;
    for (const ASTNode* fld = ((const StructTypeNode*)decl->return_type)->fields; fld; fld = fld->next) {
        n += (fld->type == AST_VAR_DECL) ? ((const VarDeclNode*)fld)->name_count : 1U;
    }
    return n;
}

static void collect_fresh_facts(const FreshFuncs* funcs, const FreshFunc* f,
                                const LocalEscapeSummary* locals, FreshFacts* facts) {
    // No body means nothing to prove from.
    if (!f->decl->body) { facts->borrowed = true; return; }
    // Each result can be fresh on its own while two of them are one object
    // (`return b, b`, or `return b, &Holder{p: b}`). A caller that owned both
    // would free the object twice, so no multi-result function is fresh.
    if (result_slot_count(f->decl) > 1U) { facts->borrowed = true; return; }

    FreshScan scan = {0};
    ASTNode* body = f->decl->body;
    scan_stmts(&scan, body, f->decl->return_type != NULL);
    if (scan.unreadable || scan.bare_return) {
        // A bare return gives the named results, which hold values from no
        // binding this walk records: their zero values, or a parameter.
        facts->borrowed = true;
        scan_free(&scan);
        return;
    }

    FreshClassify c = { .f = f, .funcs = funcs, .locals = locals, .scan = &scan, .facts = facts };
    for (size_t i = 0; i < scan.return_count && !facts->borrowed; i++) {
        const FreshReturn* r = &scan.returns[i];
        if (r->value->type == AST_IDENTIFIER) { classify_local(&c, r);
        } else { classify_value(&c, r->value); }
    }
    scan_free(&scan);
}

// local_escape keeps one summary per function WITH A BODY, in declaration
// order, and the registry keeps one per function. Pair them by walking both,
// not by name: two methods can share a name. The name of each pair is checked,
// and the first mismatch ends the pairing.
static const LocalEscapeSummary** pair_local_summaries(const FreshFuncs* reg,
                                                       const LocalEscapeResult* le) {
    const LocalEscapeSummary** out = xcalloc(reg->count, sizeof(*out));   // count > 0: collect_funcs refuses 0
    size_t li = 0;
    for (size_t fi = 0; le && fi < reg->count; fi++) {
        if (!reg->items[fi].decl->body) { continue; }
        if (li >= le->count) { break; }
        const LocalEscapeSummary* ls = &le->summaries[li++];
        // Once one pair disagrees, every later pair may be shifted, and a
        // shifted pair can still match by name when two methods share one.
        // Leave this entry and all later ones NULL: those functions prove
        // nothing about their locals.
        if (!ls->function_name || strcmp(ls->function_name, reg->items[fi].name) != 0) { break; }
        out[fi] = ls;
    }
    return out;
}

static void compute_over(const FreshFuncs* reg, ParamEscapeResult* result,
                         const LocalEscapeResult* le) {
    // collect_funcs refuses an empty program, so every array below has n > 0
    // entries. n is read once: the arrays are sized from it, and the loops
    // must use the same bound.
    const size_t n = reg->count;
    GOO_ASSERT(n > 0U);
    const LocalEscapeSummary** locals = pair_local_summaries(reg, le);
    FreshFacts* facts = xcalloc(n, sizeof(*facts));
    for (size_t fi = 0; fi < n; fi++) {
        collect_fresh_facts(reg, &reg->items[fi], locals[fi], &facts[fi]);
    }

    // may_borrow only rises, so this stops after at most n + 1 passes.
    bool* may_borrow = xcalloc(n, sizeof(bool));
    for (size_t fi = 0; fi < n; fi++) { may_borrow[fi] = facts[fi].borrowed; }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t fi = 0; fi < n; fi++) {
            if (may_borrow[fi]) { continue; }
            for (size_t k = 0; k < facts[fi].callee_count; k++) {
                const char* callee = facts[fi].callees[k];
                // A bare call never calls a method, so only a function with no
                // receiver can match. EVERY such function with that name must
                // be fresh, and no match at all is an unknown callee: a func
                // variable, or a name this registry never saw.
                bool found = false, borrowed = false;
                for (size_t gj = 0; gj < n; gj++) {
                    if (reg->items[gj].decl->receiver) { continue; }
                    if (strcmp(reg->items[gj].name, callee) != 0) { continue; }
                    found = true;
                    if (may_borrow[gj]) { borrowed = true; }
                }
                if (!found || borrowed) {
                    may_borrow[fi] = true;
                    changed = true;
                    break;
                }
            }
        }
    }

    for (size_t fi = 0; fi < n && fi < result->count; fi++) {
        result->summaries[fi].returns_fresh = !may_borrow[fi];
    }

    free(may_borrow);
    for (size_t fi = 0; fi < n; fi++) { free(facts[fi].callees); }
    free(facts);
    free(locals);
}

// The functions param_escape_analyze registered, in its order. Returns false
// when they do not line up with `pe` one for one, by count and by name: the
// summaries then belong to some other program, and nothing is proved.
static bool collect_funcs(ASTNode* program, const ParamEscapeResult* pe, FreshFuncs* out) {
    out->items = NULL;
    out->count = 0;
    if (!program || program->type != AST_PROGRAM || pe->count == 0U) { return false; }
    out->items = xcalloc(pe->count, sizeof(*out->items));
    for (const ASTNode* d = ((ProgramNode*)program)->decls; d; d = d->next) {
        if (d->type != AST_FUNC_DECL) { continue; }
        const FuncDeclNode* fd = (const FuncDeclNode*)d;
        if (!fd->name) { continue; }
        if (out->count >= pe->count) { return false; }
        const char* sname = pe->summaries[out->count].function_name;
        if (!sname || strcmp(sname, fd->name) != 0) { return false; }
        out->items[out->count++] = (FreshFunc){ .name = fd->name, .decl = fd };
    }
    return out->count == pe->count;
}

void returns_fresh_compute(ASTNode* program, ParamEscapeResult* pe,
                           const LocalEscapeResult* le) {
    // Every exit below leaves returns_fresh false, which only leaks.
    if (!pe || pe->fail_closed || !le) { return; }
    FreshFuncs funcs;
    if (collect_funcs(program, pe, &funcs)) { compute_over(&funcs, pe, le); }
    free(funcs.items);
}
