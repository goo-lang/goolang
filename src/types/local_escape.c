// ARC leg: per-LOCAL escape summaries. See include/local_escape.h for the
// contract, the soundness invariant, and why this module exists at all.
//
// THIRD SOUNDNESS SIBLING of param_escape.c and block_escape.c. This file was
// derived from block_escape.c rather than written fresh, deliberately: the
// taint-propagation engine, the sink vocabulary and the fixpoint are the
// safety property here, and re-deriving them by hand would risk dropping one
// of the corrections they already carry (the discarded-callee-taint receiver
// hole cost two separate fixes before it was closed in both files).
//
// What moved, relative to block_escape.c:
//
//   - The UNIT is a function body, not an `arena {}` block.
//   - The SOURCES are locals, seeded with their own bit at their declaration
//     in seed_local. block_escape's sources were `new(T)` / `&T{}` alloc
//     sites, and those self-bit arms are removed here.
//   - Discovery REUSES the engine: pass A runs walk_stmt with zero-width
//     taint sets purely so seed_local registers every declared name. A
//     hand-written structural walker would have to know every declaration
//     shape and would drift the first time one was added.
//
// The sinks are untouched: return, store to a non-local, closure capture,
// goroutine argument, channel send, defer argument, and a retaining call
// argument (including a method-call RECEIVER, which is not a member of
// call->args and needed its own marking).

#include "local_escape.h"
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
// Pass 1: unit discovery. A Unit is one FUNCTION body (block_escape's unit was
// one `arena {}` block). local_names[] holds every local this function
// declares, in first-seen order, and each name is one bit in the taint sets.
//
// DISCOVERY REUSES THE ENGINE rather than duplicating it. Every declaration
// form funnels through local_env_add_or_union, so running walk_stmt ONCE with
// a zero-width taint set populates a LocalEnv with exactly the set of declared
// names and marks nothing. Reading the names back out of that env is the
// discovery pass. A separate structural walker would have to enumerate every
// declaration shape (var, const, :=, range key/value, type-switch binding,
// catch binding, ...) and would drift out of sync with the engine the first
// time a shape was added — which is precisely the failure mode the two-way
// mirroring between param_escape and block_escape already produced once.
//
// SHADOWING is collapsed by name: two locals of the same name in different
// scopes share one bit, so if either escapes both are reported escaping. That
// is over-marking, which the soundness rule permits.
// =============================================================================

typedef struct {
    ASTNode*  body;         // FuncDeclNode's ->body (borrowed)
    char*     fn_name;      // owned
    char**    local_names;  // owned array of owned strings
    size_t    local_count;
    size_t    local_cap;
} Unit;

typedef struct {
    Unit*  items;
    size_t count;
    size_t cap;
} UnitList;

static void unit_list_free(UnitList* list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].fn_name);
        for (size_t j = 0; j < list->items[i].local_count; j++) {
            free(list->items[i].local_names[j]);
        }
        free(list->items[i].local_names);
    }
    free(list->items);
    list->items = NULL;
    list->count = list->cap = 0;
}

// Appends an empty unit for `body` and returns its index, or
// BLOCK_ESCAPE_NO_UNIT on allocation failure.
static size_t unit_list_push(UnitList* list, ASTNode* body, const char* fn_name) {
    if (list->count >= list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 8;
        Unit* grown = realloc(list->items, new_cap * sizeof(Unit));
        if (!grown) return BLOCK_ESCAPE_NO_UNIT;
        list->items = grown;
        list->cap = new_cap;
    }
    Unit* u = &list->items[list->count];
    u->body = body;
    u->fn_name = fn_name ? strdup(fn_name) : NULL;
    if (fn_name && !u->fn_name) return BLOCK_ESCAPE_NO_UNIT;
    u->local_names = NULL;
    u->local_count = 0;
    u->local_cap = 0;
    return list->count++;
}

// Records `name` as one of this unit's locals. Idempotent by name (see the
// shadowing note above). Returns false only on allocation failure.
static bool unit_add_local(Unit* u, const char* name) {
    if (!name || strcmp(name, "_") == 0) return true;
    for (size_t i = 0; i < u->local_count; i++) {
        if (strcmp(u->local_names[i], name) == 0) return true;
    }
    if (u->local_count >= u->local_cap) {
        size_t new_cap = u->local_cap ? u->local_cap * 2 : 8;
        char** grown = realloc(u->local_names, new_cap * sizeof(char*));
        if (!grown) return false;
        u->local_names = grown;
        u->local_cap = new_cap;
    }
    u->local_names[u->local_count] = strdup(name);
    if (!u->local_names[u->local_count]) return false;
    u->local_count++;
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
    char**    local_names;  // this unit's locals, borrowed, same order as escapes[]
    size_t    site_count;   // == local_count; kept as the engine's width name
    bool*     escapes;      // accumulator, length site_count, only ever set true
    Unit*     unit;         // discovery target during pass A; NULL during pass B
} Ctx;

// Linear scan — a unit's own site count is expected to stay small (it is
// bounded by how many new(...)/&composite expressions appear directly
// inside one arena block, excluding nested sub-blocks).
// A local's own bit, by NAME. block_escape matched an alloc-site AST node
// here; a local is identified by its name, because the same declaration can be
// re-walked across fixpoint passes and must land on the same bit.
static size_t find_local_index(Ctx* ctx, const char* name) {
    if (!name) return BLOCK_ESCAPE_NO_UNIT;
    for (size_t i = 0; i < ctx->site_count; i++) {
        if (ctx->local_names[i] && strcmp(ctx->local_names[i], name) == 0) return i;
    }
    return BLOCK_ESCAPE_NO_UNIT;
}

// Declaration binding — the ONE place a local becomes a taint source.
//
// Pass A (ctx->unit != NULL) REGISTERS the name and seeds nothing: that is how
// discovery reuses this engine instead of duplicating its knowledge of every
// declaration shape.
//
// Pass B ORs the local's OWN bit into the seed. That is what makes `x` in
// `x := new(int)` a source, exactly as block_escape seeded an alloc site's
// self-bit. An assignment to an existing local does NOT come through here (it
// unions through assign_to_lvalue), so a local keeps its own bit for life.
static bool seed_local(Ctx* ctx, const char* name, const TaintSet* value) {
    if (ctx->unit) {
        if (!unit_add_local(ctx->unit, name)) {
            // Allocation failure during discovery. Fail CLOSED: an unregistered
            // local has no bit, so it is never reported non-escaping, and the
            // public lookup answers `true` on a miss.
            return false;
        }
        return local_env_add_or_union(ctx->env, name, value);
    }
    size_t idx = find_local_index(ctx, name);
    if (idx == BLOCK_ESCAPE_NO_UNIT) {
        // Not a discovered local (e.g. "_", or a shape discovery missed).
        // Seeding no own bit is safe: it simply never becomes a source.
        return local_env_add_or_union(ctx->env, name, value);
    }
    TaintSet seeded = taint_set_copy(value);
    if (idx < seeded.n) seeded.bits[idx] = true;
    bool changed = local_env_add_or_union(ctx->env, name, &seeded);
    taint_set_free(&seeded);
    return changed;
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
        // The CALLEE's taint escapes, exactly as in handle_go_call above.
        //
        // This used to compute the taint and DISCARD it, which was the same
        // under-marking hole handle_go_call closed for `go p.m()`, left open
        // on the ORDINARY call path: `p.m()` inside an arena block leaves `p`
        // arena-eligible, because a method's receiver is NOT a member of
        // call->args, so the retain-all rule for an unresolved callee below
        // never reaches it. A method that stores its receiver then dangles
        // once the block frees the arena. Verified at the IR level before the
        // fix (`&T{}` emitted goo_arena_alloc, valgrind: invalid read).
        //
        // Only the non-identifier shape is marked. An IDENTIFIER callee keeps
        // the old behaviour on purpose: calling a closure through a local
        // (`c()`) reads its captured cells but cannot leak the environment
        // itself, which is what row 29 pins.
        //
        // param_escape.c carries the identical fix — soundness sibling.
        TaintSet ft = expr_taint(ctx, call->function);
        mark_escapes(ctx, &ft);
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

    // The CALLEE's taint escapes too, not just the arguments.
    //
    // This used to compute the callee's taint and DISCARD it, and for a
    // non-identifier callee that was an under-marking hole: `go p.m()` inside
    // an arena block left `p` arena-eligible, because the call carries no
    // arguments and nothing else marks the receiver. The method body then
    // dereferences `p` after the block has freed it. Verified at the IR level
    // before the fix — the `new(T)` emitted `goo_arena_alloc`.
    //
    // This was once masked by a SEPARATE defect: `go p.m()` failed module
    // verification, so no program of this shape built. That defect is FIXED
    // (the `go` statement handed codegen_generate_method_value a signature
    // that still carried the receiver), and `examples/arena_go_method_probe.goo`
    // is now the end-to-end gate for the marking below. It was confirmed to
    // fail under valgrind with that mark_escapes call removed. The old note
    // also said "on a pointer receiver", which was wrong — a value receiver
    // failed identically. Separately, it stops being masked the moment
    // closure environments become allocation sites:
    // `go f()` on a local holding a closure takes the identifier path, which
    // did not even compute the taint.
    //
    // So the taint is now computed and marked for EVERY callee shape,
    // identifier included. A top-level function name resolves to the empty
    // set, making the extra work a no-op there. Over-marking is always safe;
    // under-marking is what dangles a pointer.
    {
        TaintSet ft = expr_taint(ctx, call->function);
        mark_escapes(ctx, &ft);
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
            // No alloc-site self-bit here: block_escape made `&T{...}` a
            // SOURCE, but this module's sources are locals, seeded at their
            // declaration. `&expr` simply carries whatever `expr` carries.
            UnaryExprNode* u = (UnaryExprNode*)expr;
            return expr_taint(ctx, u->operand);
        }
        case AST_POSTFIX_EXPR:
            // `i++`. Without this arm it falls to the default below, which
            // marks EVERY local escaping — and `for i := 0; i < n; i++` is
            // the most common loop in Go, so the omission cost the analysis
            // essentially all of its precision. Verified: row 4 passed with
            // `i = i + 1` and failed with `i++`.
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
            // `new(T)` is not a source here either — see AST_UNARY_EXPR.
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

            // Phase 1a: this literal's own environment is a site (registered
            // by discover_expr above, iff it captures anything). Its self-bit
            // goes in AFTER the mark_escapes call, NEVER before — before, the
            // environment would mark ITSELF escaping at its own definition and
            // registering it as a site would achieve nothing at all.
            //
            // Sink #3 is deliberately NOT relaxed by this: the captured values
            // above still escape, whatever the environment's own fate turns
            // out to be. The two are independent, and only the environment's
            // fate is new here. From this point the self-bit rides the
            // ordinary taint engine, so the environment escapes exactly when
            // the closure VALUE does — assignment, return, `go`, `defer`, or a
            // retaining call argument. Calling the closure does not leak it: a
            // body reads its captured cells but cannot store the env itself.
            //
            // No closure-env self-bit here: that was block_escape's source
            // model. A func literal contributes the taint of what it CAPTURES,
            // and the capture itself is already a sink above.
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
        if (seed_local(ctx, names[i], &combined)) *env_changed = true;
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
                    if (seed_local(ctx, n->var_name, &t)) *env_changed = true;
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
                        if (seed_local(ctx, n->key_name, &empty)) *env_changed = true;
                        taint_set_free(&empty);
                    }
                    if (n->value_name && strcmp(n->value_name, "_") != 0) {
                        if (seed_local(ctx, n->value_name, &t)) *env_changed = true;
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
                                if (seed_local(ctx, nm, &combined)) *env_changed = true;
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
                        if (seed_local(ctx, bn, &t)) *env_changed = true;
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
    // PASS A — discovery. Zero-width taint sets, so every sink marks nothing
    // and the walk exists only to drive seed_local, which registers each
    // declared name on the unit. This is why discovery cannot drift away from
    // the engine: it IS the engine.
    {
        LocalEnv env = {0};
        Ctx ctx = {
            .summaries = summaries,
            .env = &env,
            .local_names = NULL,
            .site_count = 0,
            .escapes = NULL,
            .unit = u,
        };
        bool ignored = false;
        walk_stmt(&ctx, u->body, &ignored);
        local_env_free(&env);
    }

    if (u->local_count == 0) return;

    // PASS B — the real fixpoint, now that the bit width is known.
    LocalEnv env = {0};
    Ctx ctx = {
        .summaries = summaries,
        .env = &env,
        .local_names = u->local_names,
        .site_count = u->local_count,
        .escapes = escapes,
        .unit = NULL,
    };

    // Defensive backstop, not the termination argument (see param_escape.c's
    // identical comment): the taint map is a finite monotone lattice, so
    // while(changed) always terminates on its own. Fails CLOSED (marks every
    // local escaping) if ever hit.
    const size_t MAX_LOCAL_PASSES = 4096;
    bool changed = true;
    size_t pass = 0;
    while (changed) {
        changed = false;
        pass++;
        if (pass > MAX_LOCAL_PASSES) {
            for (size_t i = 0; i < u->local_count; i++) escapes[i] = true;
            break;
        }
        walk_stmt(&ctx, u->body, &changed);
    }

    local_env_free(&env);
}

// =============================================================================
// Public API
// =============================================================================

LocalEscapeResult* local_escape_analyze(ASTNode* program,
                                        const ParamEscapeResult* summaries) {
    UnitList units = {0};

    if (program && program->type == AST_PROGRAM) {
        ProgramNode* prog = (ProgramNode*)program;
        for (ASTNode* d = prog->decls; d; d = d->next) {
            if (d->type != AST_FUNC_DECL) continue;
            FuncDeclNode* fd = (FuncDeclNode*)d;
            if (!fd->name || !fd->body) continue;
            if (unit_list_push(&units, fd->body, fd->name) == BLOCK_ESCAPE_NO_UNIT) {
                unit_list_free(&units);
                return NULL;
            }
        }
    }

    LocalEscapeResult* result = xmalloc(sizeof(LocalEscapeResult));
    result->count = 0;
    result->summaries = NULL;
    if (units.count > 0) {
        result->summaries = calloc(units.count, sizeof(LocalEscapeSummary));
        if (!result->summaries) {
            free(result);
            unit_list_free(&units);
            return NULL;
        }
    }

    for (size_t i = 0; i < units.count; i++) {
        Unit* u = &units.items[i];
        bool* escapes = NULL;

        // Pass A runs inside analyze_unit and fills u->local_names, so the
        // accumulator cannot be sized until after it. analyze_unit therefore
        // allocates nothing and this loop owns the two-step.
        {
            LocalEnv env = {0};
            Ctx ctx = { .summaries = summaries, .env = &env, .local_names = NULL,
                        .site_count = 0, .escapes = NULL, .unit = u };
            bool ignored = false;
            walk_stmt(&ctx, u->body, &ignored);
            local_env_free(&env);
        }
        if (u->local_count > 0) {
            escapes = calloc(u->local_count, sizeof(bool));
            if (!escapes) {
                local_escape_result_free(result);
                unit_list_free(&units);
                return NULL;
            }
        }

        // analyze_unit re-runs pass A harmlessly (unit_add_local is idempotent
        // by name) and then runs the fixpoint.
        analyze_unit(summaries, u, escapes);

        LocalEscapeSummary* s = &result->summaries[result->count];
        s->function_name = u->fn_name;   // ownership MOVES out of the unit
        u->fn_name = NULL;
        s->local_names = u->local_names; // ownership MOVES out of the unit
        s->local_count = u->local_count;
        u->local_names = NULL;
        u->local_count = 0;
        s->escapes = escapes;
        result->count++;
    }

    unit_list_free(&units);
    return result;
}

void local_escape_result_free(LocalEscapeResult* result) {
    if (!result) return;
    for (size_t i = 0; i < result->count; i++) {
        LocalEscapeSummary* s = &result->summaries[i];
        free(s->function_name);
        for (size_t j = 0; j < s->local_count; j++) free(s->local_names[j]);
        free(s->local_names);
        free(s->escapes);
    }
    free(result->summaries);
    free(result);
}

// Conservative on EVERY miss, per the soundness contract: an unknown function
// or an unknown local reports `true`. A consumer frees on `false`, so a miss
// must never be mistaken for "provably dies here".
bool local_escape_local_escapes(const LocalEscapeResult* result,
                                const char* fn, const char* local) {
    if (!result || !fn || !local) return true;
    for (size_t i = 0; i < result->count; i++) {
        const LocalEscapeSummary* s = &result->summaries[i];
        if (!s->function_name || strcmp(s->function_name, fn) != 0) continue;
        for (size_t j = 0; j < s->local_count; j++) {
            if (s->local_names[j] && strcmp(s->local_names[j], local) == 0) {
                return s->escapes[j];
            }
        }
        return true;  // known function, unknown local -> conservative
    }
    return true;      // unknown function -> conservative
}
