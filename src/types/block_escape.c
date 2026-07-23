// Arena leg — Task 7b: per-alloc-site block-escape decisions.
// See include/block_escape.h and
// docs/superpowers/specs/2026-07-07-arena-7b-block-escape-decision-design.md
// for the design this file implements.
//
// SOUNDNESS SIBLING of src/types/param_escape.c (Task 7a): this file adapts
// that module's taint-propagation engine — the same TaintSet
// representation, the same expr_taint/walk_stmt switch shape, the same
// sink vocabulary (return / non-local store / closure capture / goroutine
// arg / retaining call argument), and the same local-fixpoint loop for
// for-loop back-edges — with two coordinates moved: the taint SOURCE is
// each allocation site inside an `arena {}` block (7a's source was a
// function parameter), and the escape BOUNDARY is that arena block (7a's
// boundary was the enclosing function). A soundness fix to the shared
// taint-propagation shape in ONE of these two files MUST be mirrored in
// the other (until/unless a third consumer justifies extracting a shared
// core). param_escape.c itself is left untouched by this task — this is a
// new, from-scratch module, exactly as param_escape.c was relative to
// src/types/escape_analysis.c (see that file's own header comment).
//
// ---------------------------------------------------------------------
// How this differs structurally from param_escape.c (two-pass per block)
// ---------------------------------------------------------------------
// 7a's "sources" (function parameters) are known up front, so it can
// pre-seed a LocalEnv and run straight to a fixpoint. 7b's sources
// (allocation sites) are discovered by WALKING the arena block's body, and
// that discovery must happen ONCE, before the taint fixpoint starts,
// so every site gets a stable bit index across fixpoint passes. So each
// arena-block "unit" gets:
//
//   Pass 1 (discover_stmt/discover_expr): a structural, non-taint walk of
//   the WHOLE program that (a) finds every `arena {}` block (each becomes
//   a Unit, in the order its ArenaBlockNode is first visited — pre-order)
//   and (b) assigns every `new(T)` / `&composite` node to the site list of
//   its INNERMOST enclosing arena block. A site textually inside a nested
//   arena block is never added to an outer block's site list — it is
//   collected as that inner Unit's own, separate site list instead. A site
//   outside every arena block is simply never added anywhere (see row 14
//   of the test matrix): `current_unit == SIZE_MAX` while walking it.
//
//   Pass 2 (expr_taint/walk_stmt, driven by analyze_unit): for each Unit,
//   an independent taint-fixpoint walk of THAT unit's own body only. A
//   name is a "local" of this unit's LocalEnv iff a var/:=/etc. declared
//   it somewhere textually inside the unit's body (walk_stmt transparently
//   descends into a NESTED arena block exactly like param_escape.c does —
//   so a variable declared inside a nested block still counts as "declared
//   textually within" the outer unit too, per the design doc's literal
//   wording). Because ownership was already resolved in Pass 1 purely by
//   AST node pointer identity, when this unit's ctx walk re-encounters a
//   site node that belongs to a DIFFERENT (nested) unit, expr_taint simply
//   does not recognize it as one of ITS OWN sites (find_site_index misses)
//   and falls through to the ordinary structural-recursion case — which
//   correctly still picks up any of THIS unit's own already-tainted
//   locals referenced inside that nested node's subexpressions, without
//   crediting the nested site's own identity to this unit's bit space.
// ---------------------------------------------------------------------

#include "block_escape.h"
#include "nonretaining.h"
#include "token.h"
#include <stdlib.h>
#include <string.h>

#define BLOCK_ESCAPE_NO_UNIT ((size_t)-1)

// =============================================================================
// TaintSet: a growable "which of this UNIT's sites may this value alias"
// bitset (copy of param_escape.c's TaintSet — see that file's header
// comment on why this module does not #include or reuse it directly:
// separate, from-scratch module). One instance is sized to the CURRENT
// unit's site_count and never resized after creation.
// =============================================================================

typedef struct {
    bool*  bits;
    size_t n;
} TaintSet;

static TaintSet taint_set_new(size_t n) {
    TaintSet t;
    t.n = n;
    t.bits = n ? calloc(n, sizeof(bool)) : NULL;
    return t;
}

static void taint_set_free(TaintSet* t) {
    if (!t) return;
    free(t->bits);
    t->bits = NULL;
    t->n = 0;
}

static bool taint_set_union_into(TaintSet* dst, const TaintSet* src) {
    if (!dst || !src) return false;
    bool changed = false;
    size_t n = dst->n < src->n ? dst->n : src->n;
    for (size_t i = 0; i < n; i++) {
        if (src->bits[i] && !dst->bits[i]) {
            dst->bits[i] = true;
            changed = true;
        }
    }
    return changed;
}

static TaintSet taint_set_copy(const TaintSet* src) {
    TaintSet t = taint_set_new(src->n);
    for (size_t i = 0; i < src->n; i++) t.bits[i] = src->bits[i];
    return t;
}

static TaintSet taint_set_all(size_t n) {
    TaintSet t = taint_set_new(n);
    for (size_t i = 0; i < n; i++) t.bits[i] = true;
    return t;
}

// =============================================================================
// Pass 1: Unit/site discovery. A Unit is one `arena {}` block instance.
// site_nodes[] holds this unit's OWN sites (borrowed AST node pointers),
// in source order, excluding any site owned by a nested arena block.
// =============================================================================

typedef struct {
    ASTNode*  body;         // ArenaBlockNode's ->body (borrowed)
    ASTNode** site_nodes;   // owned array of borrowed pointers
    size_t    site_count;
    size_t    site_cap;
} Unit;

typedef struct {
    Unit*  items;
    size_t count;
    size_t cap;
} UnitList;

static void unit_list_free(UnitList* list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) free(list->items[i].site_nodes);
    free(list->items);
    list->items = NULL;
    list->count = list->cap = 0;
}

// Appends a new (empty) unit for `body` and returns its index, or
// BLOCK_ESCAPE_NO_UNIT on allocation failure. Returned index stays valid
// even if a later push reallocs `list->items` — callers must always index
// through `list`, never cache a raw Unit* across a call that can grow it.
static size_t unit_list_push(UnitList* list, ASTNode* body) {
    if (list->count >= list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 8;
        Unit* grown = realloc(list->items, new_cap * sizeof(Unit));
        if (!grown) return BLOCK_ESCAPE_NO_UNIT;
        list->items = grown;
        list->cap = new_cap;
    }
    Unit* u = &list->items[list->count];
    u->body = body;
    u->site_nodes = NULL;
    u->site_count = 0;
    u->site_cap = 0;
    return list->count++;
}

// Records `site_node` as belonging to unit `unit_idx` (a no-op, not a
// failure, when unit_idx == BLOCK_ESCAPE_NO_UNIT: a site outside every
// arena block is simply never recorded anywhere — see test row 14).
// Returns false only on allocation failure.
static bool site_append(UnitList* list, size_t unit_idx, ASTNode* site_node) {
    if (unit_idx == BLOCK_ESCAPE_NO_UNIT) return true;
    Unit* u = &list->items[unit_idx]; // re-fetched fresh every call; safe under reallocation
    if (u->site_count >= u->site_cap) {
        size_t new_cap = u->site_cap ? u->site_cap * 2 : 4;
        ASTNode** grown = realloc(u->site_nodes, new_cap * sizeof(ASTNode*));
        if (!grown) return false;
        u->site_nodes = grown;
        u->site_cap = new_cap;
    }
    u->site_nodes[u->site_count++] = site_node;
    return true;
}

static bool is_new_call(ASTNode* expr) {
    if (!expr || expr->type != AST_CALL_EXPR) return false;
    CallExprNode* call = (CallExprNode*)expr;
    return call->function && call->function->type == AST_IDENTIFIER &&
           strcmp(((IdentifierNode*)call->function)->name, "new") == 0;
}

static bool is_addr_of_composite(ASTNode* expr) {
    if (!expr || expr->type != AST_UNARY_EXPR) return false;
    UnaryExprNode* u = (UnaryExprNode*)expr;
    return u->operator == TOKEN_BIT_AND && u->operand && u->operand->type == AST_STRUCT_LITERAL;
}

static bool discover_stmt(ASTNode* stmt, UnitList* units, size_t unit_idx);

// Recurses into every expression sub-node this front-end can produce,
// looking for allocation-site nodes (see is_new_call/is_addr_of_composite)
// and recording each one against `unit_idx`. Deliberately does NOT recurse
// into a FuncLitNode's body (mirrors param_escape.c's own choice not to
// re-walk closure bodies): a `new(...)`/`&composite` written inside a
// closure literal is not classified as a site of the enclosing arena block
// this cut — conservative miss, documented in the task report, not fixed
// here. Returns false only on allocation failure.
static bool discover_expr(ASTNode* expr, UnitList* units, size_t unit_idx) {
    if (!expr) return true;

    if (is_new_call(expr)) {
        if (!site_append(units, unit_idx, expr)) return false;
        // new(T)'s sole "argument" is a type name, not a value expression;
        // nothing further to recurse into.
        return true;
    }
    if (is_addr_of_composite(expr)) {
        if (!site_append(units, unit_idx, expr)) return false;
        // Still recurse into the composite's own field values below (a
        // field value can itself contain a nested allocation site).
    }

    switch (expr->type) {
        case AST_IDENTIFIER:
        case AST_LITERAL:
            return true;
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)expr;
            return discover_expr(b->left, units, unit_idx) && discover_expr(b->right, units, unit_idx);
        }
        case AST_UNARY_EXPR:
            return discover_expr(((UnaryExprNode*)expr)->operand, units, unit_idx);
        case AST_POSTFIX_EXPR:
            return discover_expr(((PostfixExprNode*)expr)->operand, units, unit_idx);
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            if (!discover_expr(call->function, units, unit_idx)) return false;
            for (ASTNode* a = call->args; a; a = a->next) {
                if (!discover_expr(a, units, unit_idx)) return false;
            }
            return true;
        }
        case AST_INDEX_EXPR: {
            IndexExprNode* ie = (IndexExprNode*)expr;
            return discover_expr(ie->expr, units, unit_idx) && discover_expr(ie->index, units, unit_idx);
        }
        case AST_SLICE_INDEX_EXPR: {
            SliceIndexExprNode* se = (SliceIndexExprNode*)expr;
            return discover_expr(se->expr, units, unit_idx) &&
                   discover_expr(se->low, units, unit_idx) &&
                   discover_expr(se->high, units, unit_idx);
        }
        case AST_SELECTOR_EXPR:
            return discover_expr(((SelectorExprNode*)expr)->expr, units, unit_idx);
        case AST_FUNC_LIT:
            // Opaque — see doc comment above.
            return true;
        case AST_STRUCT_LITERAL: {
            StructLiteralNode* sl = (StructLiteralNode*)expr;
            for (ASTNode* v = sl->field_values; v; v = v->next) {
                if (!discover_expr(v, units, unit_idx)) return false;
            }
            return true;
        }
        case AST_SLICE_EXPR: { // slice literal (SliceLitNode)
            SliceLitNode* sl = (SliceLitNode*)expr;
            for (ASTNode* e = sl->elements; e; e = e->next) {
                if (!discover_expr(e, units, unit_idx)) return false;
            }
            return true;
        }
        case AST_ARRAY_LITERAL: {
            ArrayLitNode* al = (ArrayLitNode*)expr;
            for (ASTNode* e = al->elements; e; e = e->next) {
                if (!discover_expr(e, units, unit_idx)) return false;
            }
            return true;
        }
        case AST_KEYED_ELEMENT:
            return discover_expr(((KeyedElementNode*)expr)->value, units, unit_idx);
        case AST_PAREN_EXPR: { // map literal (MapLitNode)
            MapLitNode* ml = (MapLitNode*)expr;
            for (ASTNode* k = ml->keys; k; k = k->next) {
                if (!discover_expr(k, units, unit_idx)) return false;
            }
            for (ASTNode* v = ml->values; v; v = v->next) {
                if (!discover_expr(v, units, unit_idx)) return false;
            }
            return true;
        }
        case AST_SLICE_CONVERSION:
            return discover_expr(((SliceConvNode*)expr)->operand, units, unit_idx);
        case AST_TYPE_ASSERT:
            return discover_expr(((TypeAssertNode*)expr)->expr, units, unit_idx);
        default:
            // Genuinely unhandled expression kind: nothing further
            // discoverable inside it via this walker. Any allocation
            // buried inside is simply never classified as a site (safe
            // miss — 7c's lookup then defaults to heap for it).
            return true;
    }
}

// Discover pass over a statement list (mirrors param_escape.c's walk_stmt
// coverage of statement kinds, minus any taint semantics — this pass only
// finds arena blocks (spawning new units) and allocation sites (recording
// them against the current unit). Returns false only on allocation failure.
static bool discover_stmt(ASTNode* stmt, UnitList* units, size_t unit_idx) {
    for (; stmt; stmt = stmt->next) {
        switch (stmt->type) {
            case AST_BLOCK_STMT:
                if (!discover_stmt(((BlockStmtNode*)stmt)->statements, units, unit_idx)) return false;
                break;

            case AST_EXPR_STMT:
                if (!discover_expr(((ExprStmtNode*)stmt)->expr, units, unit_idx)) return false;
                break;

            case AST_IF_STMT: {
                IfStmtNode* n = (IfStmtNode*)stmt;
                if (!discover_expr(n->condition, units, unit_idx)) return false;
                if (!discover_stmt(n->then_stmt, units, unit_idx)) return false;
                if (!discover_stmt(n->else_stmt, units, unit_idx)) return false;
                break;
            }

            case AST_IF_LET_STMT: {
                IfLetStmtNode* n = (IfLetStmtNode*)stmt;
                if (!discover_expr(n->nullable_expr, units, unit_idx)) return false;
                if (!discover_stmt(n->then_stmt, units, unit_idx)) return false;
                if (!discover_stmt(n->else_stmt, units, unit_idx)) return false;
                break;
            }

            case AST_FOR_STMT: {
                ForStmtNode* n = (ForStmtNode*)stmt;
                if (n->range_expr) {
                    if (!discover_expr(n->range_expr, units, unit_idx)) return false;
                } else {
                    if (n->init && !discover_stmt(n->init, units, unit_idx)) return false;
                    if (n->condition && !discover_expr(n->condition, units, unit_idx)) return false;
                    if (n->post && !discover_stmt(n->post, units, unit_idx)) return false;
                }
                if (!discover_stmt(n->body, units, unit_idx)) return false;
                break;
            }

            case AST_RETURN_STMT: {
                ReturnStmtNode* n = (ReturnStmtNode*)stmt;
                for (ASTNode* v = n->values; v; v = v->next) {
                    if (!discover_expr(v, units, unit_idx)) return false;
                }
                break;
            }

            case AST_GO_STMT:
                if (!discover_expr(((GoStmtNode*)stmt)->call, units, unit_idx)) return false;
                break;

            case AST_DEFER_STMT:
                if (!discover_expr(((DeferStmtNode*)stmt)->call, units, unit_idx)) return false;
                break;

            case AST_BREAK_STMT:
            case AST_CONTINUE_STMT:
                break;

            case AST_VAR_DECL: {
                VarDeclNode* n = (VarDeclNode*)stmt;
                for (ASTNode* v = n->values; v; v = v->next) {
                    if (!discover_expr(v, units, unit_idx)) return false;
                }
                break;
            }

            case AST_CONST_DECL: {
                ConstDeclNode* n = (ConstDeclNode*)stmt;
                for (ASTNode* v = n->values; v; v = v->next) {
                    if (!discover_expr(v, units, unit_idx)) return false;
                }
                break;
            }

            case AST_MULTI_ASSIGN: {
                MultiAssignNode* n = (MultiAssignNode*)stmt;
                for (ASTNode* v = n->values; v; v = v->next) {
                    if (!discover_expr(v, units, unit_idx)) return false;
                }
                for (ASTNode* t = n->targets; t; t = t->next) {
                    if (!discover_expr(t, units, unit_idx)) return false;
                }
                break;
            }

            case AST_SWITCH_STMT: {
                SwitchStmtNode* n = (SwitchStmtNode*)stmt;
                if (n->tag && !discover_expr(n->tag, units, unit_idx)) return false;
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type == AST_CASE_CLAUSE) {
                        CaseClauseNode* cc = (CaseClauseNode*)c;
                        for (ASTNode* e = cc->exprs; e; e = e->next) {
                            if (!discover_expr(e, units, unit_idx)) return false;
                        }
                        if (!discover_stmt(cc->body, units, unit_idx)) return false;
                    }
                }
                break;
            }

            case AST_TYPE_SWITCH: {
                TypeSwitchNode* n = (TypeSwitchNode*)stmt;
                if (!discover_expr(n->expr, units, unit_idx)) return false;
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type == AST_TYPE_CASE) {
                        if (!discover_stmt(((TypeCaseNode*)c)->body, units, unit_idx)) return false;
                    }
                }
                break;
            }

            case AST_SELECT_STMT: {
                SelectStmtNode* n = (SelectStmtNode*)stmt;
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type == AST_SELECT_CASE) {
                        SelectCaseNode* sc = (SelectCaseNode*)c;
                        if (!discover_stmt(sc->comm, units, unit_idx)) return false;
                        if (!discover_stmt(sc->body, units, unit_idx)) return false;
                    }
                }
                break;
            }

            case AST_UNSAFE_STMT:
                if (!discover_stmt(((UnsafeStmtNode*)stmt)->body, units, unit_idx)) return false;
                break;

            case AST_ARENA_BLOCK: {
                ASTNode* body = ((ArenaBlockNode*)stmt)->body;
                size_t new_idx = unit_list_push(units, body);
                if (new_idx == BLOCK_ESCAPE_NO_UNIT) return false;
                // A site declared textually within this block belongs to
                // IT, not to whatever unit enclosed it (innermost-block
                // rule) — walk its body under the NEW unit's index. Any
                // arena block nested even deeper inside `body` is
                // discovered by this same recursive call and becomes its
                // own further-nested unit.
                if (!discover_stmt(body, units, new_idx)) return false;
                break;
            }

            case AST_ASSERT_STMT: {
                AssertStmtNode* n = (AssertStmtNode*)stmt;
                if (!discover_expr(n->condition, units, unit_idx)) return false;
                if (n->message && !discover_expr(n->message, units, unit_idx)) return false;
                break;
            }

            case AST_ASSUME_STMT:
                if (!discover_expr(((AssumeStmtNode*)stmt)->condition, units, unit_idx)) return false;
                break;

            default:
                // Genuinely unhandled statement kind: nothing further
                // discoverable inside it (safe miss, same rationale as
                // discover_expr's default case).
                break;
        }
    }
    return true;
}

// =============================================================================
// Pass 2: per-unit taint fixpoint (adapted from param_escape.c's
// LocalEnv/Ctx/expr_taint/walk_stmt — see this file's header comment for
// exactly what moved).
// =============================================================================

typedef struct {
    char*    name;   // owned
    TaintSet taint;
} LocalVar;

typedef struct {
    LocalVar* vars;
    size_t    count;
    size_t    capacity;
} LocalEnv;

static void local_env_free(LocalEnv* env) {
    for (size_t i = 0; i < env->count; i++) {
        free(env->vars[i].name);
        taint_set_free(&env->vars[i].taint);
    }
    free(env->vars);
    env->vars = NULL;
    env->count = env->capacity = 0;
}

static LocalVar* local_env_find(LocalEnv* env, const char* name) {
    for (size_t i = 0; i < env->count; i++) {
        if (strcmp(env->vars[i].name, name) == 0) return &env->vars[i];
    }
    return NULL;
}

// Adds `name` seeded with a COPY of `value` if not already present;
// otherwise unions `value` into the existing entry. Returns true if this
// grew the map's information (fresh binding, or a bit flipped false->true)
// — the caller's local-fixpoint loop uses this to know whether another
// pass is needed.
static bool local_env_add_or_union(LocalEnv* env, const char* name, const TaintSet* value) {
    LocalVar* lv = local_env_find(env, name);
    if (lv) {
        return taint_set_union_into(&lv->taint, value);
    }
    if (env->count >= env->capacity) {
        size_t new_cap = env->capacity ? env->capacity * 2 : 8;
        LocalVar* grown = realloc(env->vars, new_cap * sizeof(LocalVar));
        if (!grown) return false;
        env->vars = grown;
        env->capacity = new_cap;
    }
    env->vars[env->count].name = strdup(name);
    env->vars[env->count].taint = taint_set_copy(value);
    env->count++;
    return true;
}

typedef struct {
    const ParamEscapeResult* summaries; // may be NULL; borrowed
    LocalEnv* env;
    ASTNode** site_nodes;  // this unit's OWN sites, borrowed, same order as escapes[]
    size_t    site_count;
    bool*     escapes;     // accumulator, length site_count, only ever set true
} Ctx;

// Linear scan — a unit's own site count is expected to stay small (it is
// bounded by how many new(...)/&composite expressions appear directly
// inside one arena block, excluding nested sub-blocks).
static size_t find_site_index(Ctx* ctx, ASTNode* node) {
    for (size_t i = 0; i < ctx->site_count; i++) {
        if (ctx->site_nodes[i] == node) return i;
    }
    return BLOCK_ESCAPE_NO_UNIT;
}

static void mark_escapes(Ctx* ctx, const TaintSet* t) {
    size_t n = t->n < ctx->site_count ? t->n : ctx->site_count;
    for (size_t i = 0; i < n; i++) {
        if (t->bits[i]) ctx->escapes[i] = true;
    }
}

static void mark_all_escapes(Ctx* ctx) {
    for (size_t i = 0; i < ctx->site_count; i++) ctx->escapes[i] = true;
}

static bool is_assign_op(TokenType op) {
    switch (op) {
        case TOKEN_ASSIGN:
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

static TaintSet expr_taint(Ctx* ctx, ASTNode* expr);
static void walk_stmt(Ctx* ctx, ASTNode* stmt, bool* env_changed);

// Sink #2 (store to a non-block-local location). Membership in ctx->env IS
// this module's definition of "a plain local of THIS unit" — see this
// file's header comment: it is seeded ONLY by var/:=/etc. encountered
// while walking this unit's own body, so a function parameter, an
// outer-scope (pre-block) local, or a package global is correctly absent
// and therefore a sink target. `_` is a pure discard: neither a sink nor a
// local. Any non-identifier lvalue (*p, obj.field, arr[k], ...) is
// unconditionally a sink, same conservative simplification param_escape.c
// documents for its own sink #2.
static void assign_to_lvalue(Ctx* ctx, ASTNode* lhs, const TaintSet* rhs_taint, bool* env_changed) {
    if (!lhs) {
        mark_escapes(ctx, rhs_taint);
        return;
    }
    if (lhs->type == AST_IDENTIFIER) {
        const char* name = ((IdentifierNode*)lhs)->name;
        if (strcmp(name, "_") == 0) {
            return;
        }
        LocalVar* lv = local_env_find(ctx->env, name);
        if (lv) {
            if (taint_set_union_into(&lv->taint, rhs_taint)) *env_changed = true;
            return;
        }
        mark_escapes(ctx, rhs_taint);
        return;
    }
    mark_escapes(ctx, rhs_taint);
}

// Sink #5 (retaining call argument) + the call-result taint rule, using
// `summaries` (7a's ParamEscapeResult) in place of param_escape.c's own
// Registry lookup. param_escape_lookup/param_escape_param_escapes already
// return the conservative "true"/NULL on an unknown function or a NULL
// result, so "summaries == NULL" and "external/unregistered callee" fall
// out through the exact same code path here.
static TaintSet call_taint(Ctx* ctx, CallExprNode* call) {
    size_t n = ctx->site_count;

    size_t argc = 0;
    for (ASTNode* a = call->args; a; a = a->next) argc++;

    TaintSet* arg_taints = NULL;
    if (argc > 0) arg_taints = calloc(argc, sizeof(TaintSet));
    size_t i = 0;
    for (ASTNode* a = call->args; a; a = a->next, i++) {
        arg_taints[i] = expr_taint(ctx, a);
    }

    const char* callee_name = NULL;
    if (call->function && call->function->type == AST_IDENTIFIER) {
        callee_name = ((IdentifierNode*)call->function)->name;
    } else {
        TaintSet ft = expr_taint(ctx, call->function);
        taint_set_free(&ft);
    }
    const ParamEscapeSummary* callee = param_escape_lookup(ctx->summaries, callee_name);
    // 7a' non-retaining whitelist: only for calls that do NOT resolve to a user
    // function (callee == NULL) — a user body, even one shadowing a builtin
    // name, is analysed by its real summary above.
    bool whitelisted = (callee == NULL) && goo_callee_is_non_retaining(call->function);

    for (i = 0; i < argc; i++) {
        bool retains;
        bool variadic_tail = call->has_spread && (i == argc - 1);
        if (whitelisted) {
            retains = false; // whitelisted external retains no argument (7a')
        } else if (variadic_tail) {
            retains = true;
        } else if (callee) {
            retains = (i < callee->param_count) ? callee->escapes[i] : true;
        } else {
            retains = true; // external/unregistered/no-summaries: pure-conservative
        }
        if (retains) mark_escapes(ctx, &arg_taints[i]);
    }

    TaintSet result = taint_set_new(n);
    if (whitelisted) {
        // A whitelisted external returns no argument-derived pointer (len/cap ->
        // int, print* -> void/(int,error), Sprintf -> a fresh string), so its
        // result carries none of the arguments' taint.
    } else if (callee) {
        if (callee->return_escapes) {
            for (i = 0; i < argc; i++) taint_set_union_into(&result, &arg_taints[i]);
        }
    } else {
        for (i = 0; i < argc; i++) taint_set_union_into(&result, &arg_taints[i]);
    }

    for (i = 0; i < argc; i++) taint_set_free(&arg_taints[i]);
    free(arg_taints);
    return result;
}

// Sink #4 (goroutine): every argument of the launched call escapes
// unconditionally, independent of the callee's own summary — identical to
// param_escape.c's handle_go_call.
static void handle_go_call(Ctx* ctx, ASTNode* call_node) {
    if (!call_node || call_node->type != AST_CALL_EXPR) {
        TaintSet t = expr_taint(ctx, call_node);
        mark_escapes(ctx, &t);
        taint_set_free(&t);
        return;
    }
    CallExprNode* call = (CallExprNode*)call_node;

    if (!(call->function && call->function->type == AST_IDENTIFIER)) {
        TaintSet ft = expr_taint(ctx, call->function);
        taint_set_free(&ft);
    }

    for (ASTNode* a = call->args; a; a = a->next) {
        TaintSet t = expr_taint(ctx, a);
        mark_escapes(ctx, &t);
        taint_set_free(&t);
    }
}

// A defer'd call runs at the enclosing FUNCTION's exit, which always happens
// AFTER this arena block has closed and freed its arena. So a value passed to
// a defer outlives the block exactly the way a goroutine argument does — its
// arguments (snapshotted at defer-time and read at function exit) must escape
// the block, or they would be arena-freed before the deferred call reads them
// (a use-after-free). This is the ONE place block-escape must diverge from
// param_escape.c: at FUNCTION granularity a defer runs within the frame, so
// param_escape.c correctly treats it as an ordinary call; at BLOCK granularity
// the defer fires past the block boundary, so here it is an unconditional
// escape — identical treatment to handle_go_call above (sink #4), NOT the
// retention-based sink #5.
static void handle_defer_call(Ctx* ctx, ASTNode* call_node) {
    handle_go_call(ctx, call_node);
}

static TaintSet expr_taint(Ctx* ctx, ASTNode* expr) {
    size_t n = ctx->site_count;
    if (!expr) return taint_set_new(n);

    switch (expr->type) {
        case AST_IDENTIFIER: {
            const char* name = ((IdentifierNode*)expr)->name;
            LocalVar* lv = local_env_find(ctx->env, name);
            if (lv) return taint_set_copy(&lv->taint);
            return taint_set_new(n); // outer-scope/global/unknown identifier => ∅
        }
        case AST_LITERAL:
            return taint_set_new(n);
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)expr;
            TaintSet l = expr_taint(ctx, b->left);
            TaintSet r = expr_taint(ctx, b->right);
            taint_set_union_into(&l, &r);
            taint_set_free(&r);
            return l;
        }
        case AST_UNARY_EXPR: {
            UnaryExprNode* u = (UnaryExprNode*)expr;
            if (is_addr_of_composite(expr)) {
                size_t idx = find_site_index(ctx, expr);
                if (idx != BLOCK_ESCAPE_NO_UNIT) {
                    // Own site: self-bit UNION the taint carried by any of
                    // the composite's own field values (e.g. row 13's
                    // `&Node{next: n}` — if this composite escapes, so
                    // must the site `n` embedded within it).
                    TaintSet t = taint_set_new(n);
                    t.bits[idx] = true;
                    TaintSet fields = expr_taint(ctx, u->operand);
                    taint_set_union_into(&t, &fields);
                    taint_set_free(&fields);
                    return t;
                }
                // Belongs to a nested unit (or, defensively, is not
                // actually one of THIS unit's registered sites): fall
                // through to the generic recursion below, which still
                // picks up any of THIS unit's own taint referenced inside
                // it without crediting the foreign site's own identity.
            }
            return expr_taint(ctx, u->operand);
        }
        case AST_POSTFIX_EXPR:
            return expr_taint(ctx, ((PostfixExprNode*)expr)->operand);
        case AST_INDEX_EXPR: {
            IndexExprNode* ie = (IndexExprNode*)expr;
            TaintSet base = expr_taint(ctx, ie->expr);
            TaintSet idx = expr_taint(ctx, ie->index);
            taint_set_union_into(&base, &idx);
            taint_set_free(&idx);
            return base;
        }
        case AST_SLICE_INDEX_EXPR: {
            SliceIndexExprNode* se = (SliceIndexExprNode*)expr;
            TaintSet base = expr_taint(ctx, se->expr);
            TaintSet lo = expr_taint(ctx, se->low);
            taint_set_union_into(&base, &lo);
            taint_set_free(&lo);
            TaintSet hi = expr_taint(ctx, se->high);
            taint_set_union_into(&base, &hi);
            taint_set_free(&hi);
            return base;
        }
        case AST_SELECTOR_EXPR:
            return expr_taint(ctx, ((SelectorExprNode*)expr)->expr);
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            if (is_new_call(expr)) {
                size_t idx = find_site_index(ctx, expr);
                if (idx != BLOCK_ESCAPE_NO_UNIT) {
                    TaintSet t = taint_set_new(n);
                    t.bits[idx] = true;
                    return t; // new(T)'s arg is a type, nothing to recurse into
                }
                // Foreign new(...) (nested unit's own site): fall through
                // to call_taint below — harmless, "new" is never a
                // registered user function so this is treated as an
                // ordinary external call, but its type-name "argument"
                // carries no taint either way.
            }
            return call_taint(ctx, call);
        }
        case AST_FUNC_LIT: {
            // Sink #3 (closure capture) — identical to param_escape.c:
            // read captured_names[] as populated by the type checker
            // (type_check_program must run before this analysis), do NOT
            // re-walk the closure body.
            FuncLitNode* lit = (FuncLitNode*)expr;
            TaintSet t = taint_set_new(n);
            for (size_t i = 0; i < lit->captured_count; i++) {
                LocalVar* lv = local_env_find(ctx->env, lit->captured_names[i]);
                if (lv) taint_set_union_into(&t, &lv->taint);
            }
            mark_escapes(ctx, &t);
            return t;
        }
        case AST_STRUCT_LITERAL: {
            StructLiteralNode* sl = (StructLiteralNode*)expr;
            TaintSet t = taint_set_new(n);
            for (ASTNode* v = sl->field_values; v; v = v->next) {
                TaintSet vt = expr_taint(ctx, v);
                taint_set_union_into(&t, &vt);
                taint_set_free(&vt);
            }
            return t;
        }
        case AST_SLICE_EXPR: { // slice literal (SliceLitNode)
            SliceLitNode* sl = (SliceLitNode*)expr;
            TaintSet t = taint_set_new(n);
            for (ASTNode* e = sl->elements; e; e = e->next) {
                TaintSet et = expr_taint(ctx, e);
                taint_set_union_into(&t, &et);
                taint_set_free(&et);
            }
            return t;
        }
        case AST_ARRAY_LITERAL: {
            ArrayLitNode* al = (ArrayLitNode*)expr;
            TaintSet t = taint_set_new(n);
            for (ASTNode* e = al->elements; e; e = e->next) {
                TaintSet et = expr_taint(ctx, e);
                taint_set_union_into(&t, &et);
                taint_set_free(&et);
            }
            return t;
        }
        case AST_KEYED_ELEMENT:
            return expr_taint(ctx, ((KeyedElementNode*)expr)->value);
        case AST_PAREN_EXPR: { // map literal (MapLitNode)
            MapLitNode* ml = (MapLitNode*)expr;
            TaintSet t = taint_set_new(n);
            for (ASTNode* k = ml->keys; k; k = k->next) {
                TaintSet kt = expr_taint(ctx, k);
                taint_set_union_into(&t, &kt);
                taint_set_free(&kt);
            }
            for (ASTNode* v = ml->values; v; v = v->next) {
                TaintSet vt = expr_taint(ctx, v);
                taint_set_union_into(&t, &vt);
                taint_set_free(&vt);
            }
            return t;
        }
        case AST_SLICE_CONVERSION:
            return expr_taint(ctx, ((SliceConvNode*)expr)->operand);
        case AST_TYPE_ASSERT:
            return expr_taint(ctx, ((TypeAssertNode*)expr)->expr);
        default:
            // Genuinely unhandled expression kind: conservative escape of
            // every site — same rationale as param_escape.c's default arm.
            {
                TaintSet t = taint_set_all(n);
                mark_escapes(ctx, &t);
                return t;
            }
    }
}

// Seeds/updates locals for a VarDeclNode/ConstDeclNode-shaped
// names/name_count/values triple. Identical to param_escape.c's
// seed_names_from_values.
static void seed_names_from_values(Ctx* ctx, char** names, size_t name_count,
                                    ASTNode* values, bool* env_changed) {
    TaintSet combined = taint_set_new(ctx->site_count);
    for (ASTNode* v = values; v; v = v->next) {
        TaintSet t = expr_taint(ctx, v);
        taint_set_union_into(&combined, &t);
        taint_set_free(&t);
    }
    for (size_t i = 0; i < name_count; i++) {
        if (strcmp(names[i], "_") == 0) continue;
        if (local_env_add_or_union(ctx->env, names[i], &combined)) *env_changed = true;
    }
    taint_set_free(&combined);
}

static void walk_stmt(Ctx* ctx, ASTNode* stmt, bool* env_changed) {
    for (; stmt; stmt = stmt->next) {
        switch (stmt->type) {
            case AST_BLOCK_STMT:
                walk_stmt(ctx, ((BlockStmtNode*)stmt)->statements, env_changed);
                break;

            case AST_EXPR_STMT: {
                ASTNode* e = ((ExprStmtNode*)stmt)->expr;
                if (e && e->type == AST_BINARY_EXPR) {
                    BinaryExprNode* b = (BinaryExprNode*)e;
                    if (is_assign_op(b->operator)) {
                        TaintSet rhs = expr_taint(ctx, b->right);
                        assign_to_lvalue(ctx, b->left, &rhs, env_changed);
                        taint_set_free(&rhs);
                        break;
                    }
                    if (b->operator == TOKEN_ARROW) {
                        // Channel send `ch <- v`: the sent value LEAVES this
                        // block — a receiver (another goroutine, or code
                        // running after the block) reads it once the arena is
                        // already freed. So taint(v) escapes the block, exactly
                        // like a goroutine/defer argument (a bare send of an
                        // arena value was a use-after-free before this).
                        // `<-ch` receive is a UNARY ARROW (a fresh in-bound
                        // value), correctly NOT a sink — handled by expr_taint.
                        TaintSet lt = expr_taint(ctx, b->left);
                        taint_set_free(&lt);
                        TaintSet rhs = expr_taint(ctx, b->right);
                        mark_escapes(ctx, &rhs);
                        taint_set_free(&rhs);
                        break;
                    }
                }
                TaintSet t = expr_taint(ctx, e);
                taint_set_free(&t);
                break;
            }

            case AST_IF_STMT: {
                IfStmtNode* n = (IfStmtNode*)stmt;
                TaintSet t = expr_taint(ctx, n->condition);
                taint_set_free(&t);
                walk_stmt(ctx, n->then_stmt, env_changed);
                walk_stmt(ctx, n->else_stmt, env_changed);
                break;
            }

            case AST_IF_LET_STMT: {
                IfLetStmtNode* n = (IfLetStmtNode*)stmt;
                TaintSet t = expr_taint(ctx, n->nullable_expr);
                if (n->var_name && strcmp(n->var_name, "_") != 0) {
                    if (local_env_add_or_union(ctx->env, n->var_name, &t)) *env_changed = true;
                }
                taint_set_free(&t);
                walk_stmt(ctx, n->then_stmt, env_changed);
                walk_stmt(ctx, n->else_stmt, env_changed);
                break;
            }

            case AST_FOR_STMT: {
                ForStmtNode* n = (ForStmtNode*)stmt;
                if (n->range_expr) {
                    TaintSet t = expr_taint(ctx, n->range_expr);
                    if (n->key_name && strcmp(n->key_name, "_") != 0) {
                        TaintSet empty = taint_set_new(ctx->site_count);
                        if (local_env_add_or_union(ctx->env, n->key_name, &empty)) *env_changed = true;
                        taint_set_free(&empty);
                    }
                    if (n->value_name && strcmp(n->value_name, "_") != 0) {
                        if (local_env_add_or_union(ctx->env, n->value_name, &t)) *env_changed = true;
                    }
                    taint_set_free(&t);
                } else {
                    if (n->init) walk_stmt(ctx, n->init, env_changed);
                    if (n->condition) {
                        TaintSet t = expr_taint(ctx, n->condition);
                        taint_set_free(&t);
                    }
                    if (n->post) walk_stmt(ctx, n->post, env_changed);
                }
                walk_stmt(ctx, n->body, env_changed);
                break;
            }

            case AST_RETURN_STMT: {
                // Sink #1: returning from the enclosing FUNCTION definitely
                // means this value outlives the arena block being exited.
                ReturnStmtNode* n = (ReturnStmtNode*)stmt;
                for (ASTNode* v = n->values; v; v = v->next) {
                    TaintSet t = expr_taint(ctx, v);
                    mark_escapes(ctx, &t);
                    taint_set_free(&t);
                }
                break;
            }

            case AST_GO_STMT:
                handle_go_call(ctx, ((GoStmtNode*)stmt)->call);
                break;

            case AST_DEFER_STMT:
                handle_defer_call(ctx, ((DeferStmtNode*)stmt)->call);
                break;

            case AST_BREAK_STMT:
            case AST_CONTINUE_STMT:
                break;

            case AST_VAR_DECL: {
                VarDeclNode* n = (VarDeclNode*)stmt;
                seed_names_from_values(ctx, n->names, n->name_count, n->values, env_changed);
                break;
            }

            case AST_CONST_DECL: {
                ConstDeclNode* n = (ConstDeclNode*)stmt;
                seed_names_from_values(ctx, n->names, n->name_count, n->values, env_changed);
                break;
            }

            case AST_MULTI_ASSIGN: {
                MultiAssignNode* n = (MultiAssignNode*)stmt;
                TaintSet combined = taint_set_new(ctx->site_count);
                for (ASTNode* v = n->values; v; v = v->next) {
                    TaintSet t = expr_taint(ctx, v);
                    taint_set_union_into(&combined, &t);
                    taint_set_free(&t);
                }
                if (n->is_short_decl) {
                    for (ASTNode* tgt = n->targets; tgt; tgt = tgt->next) {
                        if (tgt->type == AST_IDENTIFIER) {
                            const char* nm = ((IdentifierNode*)tgt)->name;
                            if (strcmp(nm, "_") != 0) {
                                if (local_env_add_or_union(ctx->env, nm, &combined)) *env_changed = true;
                            }
                        } else {
                            mark_escapes(ctx, &combined);
                        }
                    }
                } else {
                    for (ASTNode* tgt = n->targets; tgt; tgt = tgt->next) {
                        assign_to_lvalue(ctx, tgt, &combined, env_changed);
                    }
                }
                taint_set_free(&combined);
                break;
            }

            case AST_SWITCH_STMT: {
                SwitchStmtNode* n = (SwitchStmtNode*)stmt;
                if (n->tag) {
                    TaintSet t = expr_taint(ctx, n->tag);
                    taint_set_free(&t);
                }
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type == AST_CASE_CLAUSE) {
                        CaseClauseNode* cc = (CaseClauseNode*)c;
                        for (ASTNode* e = cc->exprs; e; e = e->next) {
                            TaintSet t = expr_taint(ctx, e);
                            taint_set_free(&t);
                        }
                        walk_stmt(ctx, cc->body, env_changed);
                    }
                }
                break;
            }

            case AST_TYPE_SWITCH: {
                TypeSwitchNode* n = (TypeSwitchNode*)stmt;
                TaintSet t = expr_taint(ctx, n->expr);
                if (n->bind_name && n->bind_name->type == AST_IDENTIFIER) {
                    const char* bn = ((IdentifierNode*)n->bind_name)->name;
                    if (strcmp(bn, "_") != 0) {
                        if (local_env_add_or_union(ctx->env, bn, &t)) *env_changed = true;
                    }
                }
                taint_set_free(&t);
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type == AST_TYPE_CASE) {
                        walk_stmt(ctx, ((TypeCaseNode*)c)->body, env_changed);
                    }
                }
                break;
            }

            case AST_SELECT_STMT: {
                SelectStmtNode* n = (SelectStmtNode*)stmt;
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type == AST_SELECT_CASE) {
                        SelectCaseNode* sc = (SelectCaseNode*)c;
                        walk_stmt(ctx, sc->comm, env_changed);
                        walk_stmt(ctx, sc->body, env_changed);
                    }
                }
                break;
            }

            case AST_UNSAFE_STMT:
                walk_stmt(ctx, ((UnsafeStmtNode*)stmt)->body, env_changed);
                break;

            case AST_ARENA_BLOCK:
                // Transparent pass-through — see this file's header
                // comment: ownership of any site inside was already
                // resolved in Pass 1 by AST node identity, so re-walking
                // a nested block's body here under THIS unit's ctx cannot
                // credit a foreign site to this unit's bit space (see
                // expr_taint's is_new_call/is_addr_of_composite handling).
                walk_stmt(ctx, ((ArenaBlockNode*)stmt)->body, env_changed);
                break;

            case AST_ASSERT_STMT: {
                AssertStmtNode* n = (AssertStmtNode*)stmt;
                TaintSet t = expr_taint(ctx, n->condition);
                taint_set_free(&t);
                if (n->message) {
                    TaintSet tm = expr_taint(ctx, n->message);
                    taint_set_free(&tm);
                }
                break;
            }

            case AST_ASSUME_STMT: {
                TaintSet t = expr_taint(ctx, ((AssumeStmtNode*)stmt)->condition);
                taint_set_free(&t);
                break;
            }

            default:
                // Genuinely unhandled statement kind: conservative escape
                // of every site in this unit.
                mark_all_escapes(ctx);
                break;
        }
    }
}

// Runs one unit's intraprocedural taint analysis to a LOCAL fixpoint (same
// termination argument as param_escape.c's analyze_function_body: the
// taint map only grows, so repeating the whole-body walk until nothing
// changes is sound and also handles for-loop back-edges). LocalEnv starts
// EMPTY here — unlike a function's params, an arena block has no
// pre-existing "parameters" to seed; every local comes from a var/:=/etc.
// encountered during the walk itself.
static void analyze_unit(const ParamEscapeResult* summaries, Unit* u, bool* escapes) {
    LocalEnv env = {0};
    Ctx ctx = {
        .summaries = summaries,
        .env = &env,
        .site_nodes = u->site_nodes,
        .site_count = u->site_count,
        .escapes = escapes,
    };

    // Defensive backstop, not the termination argument (see
    // param_escape.c's identical comment on its own cap): the taint map is
    // a finite monotone lattice, so while(changed) always terminates on
    // its own. Fails CLOSED (marks every site escaping) if ever hit.
    const size_t MAX_LOCAL_PASSES = 4096;
    bool changed = true;
    size_t pass = 0;
    while (changed) {
        changed = false;
        pass++;
        if (pass > MAX_LOCAL_PASSES) {
            for (size_t i = 0; i < u->site_count; i++) escapes[i] = true;
            break;
        }
        walk_stmt(&ctx, u->body, &changed);
    }

    local_env_free(&env);
}

// =============================================================================
// Public API
// =============================================================================

BlockEscapeResult* block_escape_analyze(ASTNode* program, const ParamEscapeResult* summaries) {
    UnitList units = {0};

    if (program && program->type == AST_PROGRAM) {
        ProgramNode* prog = (ProgramNode*)program;
        for (ASTNode* d = prog->decls; d; d = d->next) {
            if (d->type == AST_FUNC_DECL) {
                FuncDeclNode* fd = (FuncDeclNode*)d;
                if (!discover_stmt(fd->body, &units, BLOCK_ESCAPE_NO_UNIT)) {
                    unit_list_free(&units);
                    return NULL;
                }
            }
        }
    }

    size_t total = 0;
    for (size_t i = 0; i < units.count; i++) total += units.items[i].site_count;

    BlockEscapeResult* result = xmalloc(sizeof(BlockEscapeResult));
    if (!result) {
        unit_list_free(&units);
        return NULL;
    }
    result->count = total;
    result->decisions = NULL;
    if (total > 0) {
        result->decisions = calloc(total, sizeof(BlockEscapeDecision));
        if (!result->decisions) {
            free(result);
            unit_list_free(&units);
            return NULL;
        }
    }

    size_t out_idx = 0;
    for (size_t i = 0; i < units.count; i++) {
        Unit* u = &units.items[i];
        bool* escapes = u->site_count ? calloc(u->site_count, sizeof(bool)) : NULL;
        if (u->site_count && !escapes) {
            free(result->decisions);
            free(result);
            unit_list_free(&units);
            return NULL;
        }

        analyze_unit(summaries, u, escapes);

        for (size_t s = 0; s < u->site_count; s++) {
            result->decisions[out_idx].site = u->site_nodes[s];
            result->decisions[out_idx].escapes_block = escapes[s];
            out_idx++;
        }
        free(escapes);
    }

    unit_list_free(&units);
    return result;
}

void block_escape_result_free(BlockEscapeResult* result) {
    if (!result) return;
    free(result->decisions); // decisions[].site is borrowed, not owned
    free(result);
}

bool block_escape_site_escapes(const BlockEscapeResult* result, const ASTNode* site) {
    if (!result || !site) return true; // conservative miss
    for (size_t i = 0; i < result->count; i++) {
        if (result->decisions[i].site == site) return result->decisions[i].escapes_block;
    }
    return true; // unknown/not classified as a site: conservative miss
}
