// Arena leg — Task 7a: interprocedural param-escape summaries.
// See include/param_escape.h and
// docs/superpowers/specs/2026-07-07-arena-7a-param-escape-summaries-design.md
// for the design this file implements.
//
// This is a from-scratch module. It intentionally does NOT reuse any of
// src/types/escape_analysis.c's algorithms (that file's EscapeAnalyzer is a
// hollow TODO-stub walker that only handles a handful of node kinds and
// counts *parameter nodes* rather than flattened parameter *names* — wrong
// for the "func f(a, b int)" grouped-name case this task must get right).
// Only the EscapeKind vocabulary name is shared conceptually (this module
// does not even need memory_safety.h — it exposes its own bool escapes[]).

#include "param_escape.h"
#include "nonretaining.h"
#include "token.h"
#include <stdlib.h>
#include <string.h>

// =============================================================================
// TaintSet: a growable "which of this function's params may this value
// alias" bitset. One instance is sized to the CURRENT function's param_count
// and never resized after creation (every taint set within one function's
// analysis pass shares that width).
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

static bool taint_set_empty(const TaintSet* t) {
    for (size_t i = 0; i < t->n; i++) {
        if (t->bits[i]) return false;
    }
    return true;
}

// Unions src into dst (dst may be narrower/wider; only overlapping indices
// are touched — in practice both always share the same n within one
// function's analysis). Returns true if dst changed.
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
// Registry: one FuncInfo per registered AST_FUNC_DECL (ordinary functions and
// methods alike — a method's receiver is already spliced as params[0] by the
// parser, so no special-casing is needed here).
// =============================================================================

typedef struct {
    char*         name;          // owned
    FuncDeclNode* decl;          // borrowed (owned by the AST)
    char**        param_names;   // owned array of owned strings, length param_count
    size_t        param_count;
    bool*         escapes;       // owned, length param_count
    bool          return_escapes;
} FuncInfo;

typedef struct {
    FuncInfo* items;
    size_t    count;
    size_t    capacity;
} Registry;

static void registry_free(Registry* reg) {
    if (!reg) return;
    for (size_t i = 0; i < reg->count; i++) {
        FuncInfo* f = &reg->items[i];
        free(f->name);
        for (size_t j = 0; j < f->param_count; j++) free(f->param_names[j]);
        free(f->param_names);
        free(f->escapes);
    }
    free(reg->items);
    reg->items = NULL;
    reg->count = reg->capacity = 0;
}

// Returns false only on allocation failure.
static bool registry_add(Registry* reg, FuncDeclNode* decl) {
    if (reg->count >= reg->capacity) {
        size_t new_cap = reg->capacity ? reg->capacity * 2 : 8;
        FuncInfo* grown = realloc(reg->items, new_cap * sizeof(FuncInfo));
        if (!grown) return false;
        reg->items = grown;
        reg->capacity = new_cap;
    }

    FuncInfo* info = &reg->items[reg->count];
    memset(info, 0, sizeof(*info));
    info->name = strdup(decl->name);
    if (!info->name) return false;
    info->decl = decl;

    size_t count = 0;
    for (ASTNode* p = decl->params; p; p = p->next) {
        if (p->type != AST_VAR_DECL) continue;
        count += ((VarDeclNode*)p)->name_count;
    }
    info->param_count = count;
    if (count > 0) {
        info->param_names = calloc(count, sizeof(char*));
        info->escapes = calloc(count, sizeof(bool));
        if (!info->param_names || !info->escapes) return false;
        size_t idx = 0;
        for (ASTNode* p = decl->params; p; p = p->next) {
            if (p->type != AST_VAR_DECL) continue;
            VarDeclNode* vd = (VarDeclNode*)p;
            for (size_t i = 0; i < vd->name_count; i++) {
                info->param_names[idx] = strdup(vd->names[i]);
                if (!info->param_names[idx]) return false;
                idx++;
            }
        }
    }
    info->return_escapes = false;
    reg->count++;
    return true;
}

static FuncInfo* registry_find(Registry* reg, const char* name) {
    if (!reg || !name) return NULL;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->items[i].name, name) == 0) return &reg->items[i];
    }
    return NULL;
}

// Registers every top-level AST_FUNC_DECL (ordinary functions and methods).
// Goo has no nested function DECLARATIONS (only func LITERALS, which are not
// AST_FUNC_DECL), so a single top-level walk of program->decls is complete.
// Returns false only on allocation failure; a NULL/non-program AST yields an
// empty (but valid) registry.
static bool collect_functions(Registry* reg, ASTNode* program) {
    if (!program || program->type != AST_PROGRAM) return true;
    ProgramNode* prog = (ProgramNode*)program;
    for (ASTNode* d = prog->decls; d; d = d->next) {
        if (d->type == AST_FUNC_DECL) {
            FuncDeclNode* fd = (FuncDeclNode*)d;
            if (fd->name) {
                if (!registry_add(reg, fd)) return false;
            }
        }
    }
    return true;
}

// =============================================================================
// LocalEnv: per-function-analysis-pass map from local name (params + any
// var/:= declared name) to its current taint set. Membership in this map IS
// this module's definition of "a plain local of F" for the assignment-sink
// rule (sink #2) — anything NOT in this map (a global, or a name from an
// enclosing scope) is conservatively a store-escape target.
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
// grew the map's information (a fresh binding, or a taint bit flipped
// false->true) — the caller's local-fixpoint loop uses this to know whether
// another pass is needed. Returns false (silently, fails closed by simply
// not registering the local) only on allocation failure.
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

// =============================================================================
// Analysis context threaded through one function's body walk.
// =============================================================================

typedef struct {
    Registry* reg;
    LocalEnv* env;
    size_t    param_count;
    bool*     escapes;         // accumulator, length param_count, only ever set true
    bool*     return_escapes;  // accumulator, single bool, only ever set true
} Ctx;

static void mark_escapes(Ctx* ctx, const TaintSet* t) {
    size_t n = t->n < ctx->param_count ? t->n : ctx->param_count;
    for (size_t i = 0; i < n; i++) {
        if (t->bits[i]) ctx->escapes[i] = true;
    }
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

// Sink #2 (store to a non-local location). `lhs` is the assignment target;
// `rhs_taint` is the already-computed taint of the value being stored.
// Assigning to a bare identifier that is a plain local of F (a param, or a
// var/:= declared inside F) is PROPAGATION, not escape. The blank identifier
// `_` is a pure discard: neither a sink nor a local. Everything else — a
// global/enclosing-scope identifier, or any non-identifier lvalue (*p,
// obj.field, arr[k], a slice expression, ...) — is conservatively a sink,
// regardless of whether that lvalue's own base happens to itself be a plain
// local (see the design's sink #2 wording; treating every non-identifier
// lvalue as a sink over-approximates a small number of cases where a field
// of a genuinely local struct is stored through — documented as a known
// conservative simplification, not fixed here).
static void assign_to_lvalue(Ctx* ctx, ASTNode* lhs, const TaintSet* rhs_taint, bool* env_changed) {
    if (!lhs) {
        mark_escapes(ctx, rhs_taint);
        return;
    }
    if (lhs->type == AST_IDENTIFIER) {
        const char* name = ((IdentifierNode*)lhs)->name;
        if (strcmp(name, "_") == 0) {
            return; // blank: discard, not a sink
        }
        LocalVar* lv = local_env_find(ctx->env, name);
        if (lv) {
            if (taint_set_union_into(&lv->taint, rhs_taint)) *env_changed = true;
            return;
        }
        // Not a known local of F: a global or a variable from an enclosing
        // scope. Sink.
        mark_escapes(ctx, rhs_taint);
        return;
    }
    // *p, obj.field, arr[k], s[lo:hi], ... — always a sink (see doc comment).
    mark_escapes(ctx, rhs_taint);
}

// Sink #5 (retaining call argument) + the call-result taint rule. Also
// applies to a call reached as a bare expression statement (result
// discarded) since the sink fires purely from argument passing.
static TaintSet call_taint(Ctx* ctx, CallExprNode* call) {
    size_t n = ctx->param_count;

    size_t argc = 0;
    for (ASTNode* a = call->args; a; a = a->next) argc++;

    TaintSet* arg_taints = NULL;
    if (argc > 0) arg_taints = calloc(argc, sizeof(TaintSet));
    size_t i = 0;
    for (ASTNode* a = call->args; a; a = a->next, i++) {
        arg_taints[i] = expr_taint(ctx, a);
    }

    FuncInfo* callee = NULL;
    if (call->function && call->function->type == AST_IDENTIFIER) {
        callee = registry_find(ctx->reg, ((IdentifierNode*)call->function)->name);
    } else {
        // Not a plain-identifier callee (selector-expr method/package call,
        // an immediately-invoked func literal, ...): evaluate it for side
        // effects only (e.g. a func literal's captures still sink via
        // expr_taint's AST_FUNC_LIT case). callee stays NULL => external.
        TaintSet ft = expr_taint(ctx, call->function);
        taint_set_free(&ft);
    }
    // 7a' non-retaining whitelist: only for calls that do NOT resolve to a user
    // function (callee == NULL) — a user body, even one shadowing a builtin
    // name, is analysed by its real summary.
    bool whitelisted = (callee == NULL) && goo_callee_is_non_retaining(call->function);

    for (i = 0; i < argc; i++) {
        bool retains;
        bool variadic_tail = call->has_spread && (i == argc - 1);
        if (whitelisted) {
            // A whitelisted external (len/cap/print/println, fmt.Print*/Sprintf)
            // provably retains no argument — even a spread one.
            retains = false;
        } else if (variadic_tail) {
            // Spread/variadic tail: conservative — retain unless every
            // covered position is known non-retaining, which this module
            // does not attempt to prove. Always retain.
            retains = true;
        } else if (callee) {
            retains = (i < callee->param_count) ? callee->escapes[i] : true;
        } else {
            // External/unregistered/body-less callee: pure-conservative,
            // every position retains.
            retains = true;
        }
        if (retains) mark_escapes(ctx, &arg_taints[i]);
    }

    // Call-result taint: sound over-approximation per the design — if the
    // callee summary says it MAY return an arg-derived value, we cannot
    // tell which arg, so the result is tainted by the union of ALL args.
    TaintSet result = taint_set_new(n);
    if (whitelisted) {
        // A whitelisted external returns no argument-derived pointer, so its
        // result carries none of the arguments' taint (result stays ∅).
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

// Sink #4 (goroutine). Every argument of the launched call escapes
// unconditionally, independent of the callee's own summary. A func-literal
// callee's OWN captures still sink via expr_taint's AST_FUNC_LIT case
// (sink #3) when we evaluate call->function below — that is a distinct
// mechanism from "every argument of this call" and both can fire together.
static void handle_go_call(Ctx* ctx, ASTNode* call_node) {
    if (!call_node || call_node->type != AST_CALL_EXPR) {
        // Grammar guarantees `go` is always followed by a call_expr; this
        // is just defense in depth for a malformed/unexpected AST.
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

// A defer'd call executes synchronously as part of F's own teardown, before
// F's frame is gone — unlike `go`, it does not hand the value to a
// possibly-outlasting concurrent context. Not explicitly enumerated as its
// own sink kind in the design; treated the same as an ordinary call
// expression statement (sink #5 only, not the unconditional sink #4
// treatment `go` gets). Documented judgment call — see task report.
static void handle_defer_call(Ctx* ctx, ASTNode* call_node) {
    if (!call_node) return;
    TaintSet t = expr_taint(ctx, call_node);
    taint_set_free(&t);
}

static TaintSet expr_taint(Ctx* ctx, ASTNode* expr) {
    size_t n = ctx->param_count;
    if (!expr) return taint_set_new(n);

    switch (expr->type) {
        case AST_IDENTIFIER: {
            const char* name = ((IdentifierNode*)expr)->name;
            LocalVar* lv = local_env_find(ctx->env, name);
            if (lv) return taint_set_copy(&lv->taint);
            return taint_set_new(n); // global/unknown identifier => ∅
        }
        case AST_LITERAL:
            return taint_set_new(n);
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)expr;
            // Assignment is only ever produced at the statement level by
            // this grammar (see simple_stmt), never nested as a
            // sub-expression, but union both sides regardless if we ever
            // see one here — harmless and still conservative.
            TaintSet l = expr_taint(ctx, b->left);
            TaintSet r = expr_taint(ctx, b->right);
            taint_set_union_into(&l, &r);
            taint_set_free(&r);
            return l;
        }
        case AST_UNARY_EXPR:
            return expr_taint(ctx, ((UnaryExprNode*)expr)->operand);
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
        case AST_CALL_EXPR:
            return call_taint(ctx, (CallExprNode*)expr);
        case AST_FUNC_LIT: {
            // Sink #3 (closure capture). We read captured_names[] as
            // populated by the type checker; we deliberately do NOT re-walk
            // the closure body (see the design doc and ast.h's doc comment
            // on FuncLitNode.captured_names).
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
        // Slice literal (repurposes the AST_SLICE_EXPR tag — see SliceLitNode's
        // doc comment in ast.h).
        case AST_SLICE_EXPR: {
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
        // Map literal (repurposes the AST_PAREN_EXPR tag — see MapLitNode's
        // doc comment in ast.h; a parenthesized expression parses as plain
        // identity and never produces this node).
        case AST_PAREN_EXPR: {
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
            // Genuinely unhandled expression kind (GPU/WASM/comptime/match/
            // pattern nodes, etc.): conservative escape of every param —
            // we cannot cheaply enumerate exactly which params are
            // "mentioned within it" without a fully generic AST walker, so
            // the safe over-approximation is "all of them, right now".
            {
                TaintSet t = taint_set_all(n);
                mark_escapes(ctx, &t);
                return t;
            }
    }
}

static void mark_all_escapes(Ctx* ctx) {
    for (size_t i = 0; i < ctx->param_count; i++) ctx->escapes[i] = true;
}

// Seeds/updates locals for a VarDeclNode or ConstDeclNode-shaped
// names/name_count/values triple (var_decl, short_var_decl, and const_decl
// all share this shape). A value-list shorter than the name list (grouped
// no-initializer decl, or a single multi-return call destructured across
// several names) is handled by giving every name the UNION of all provided
// values' taint — imprecise when names don't line up 1:1 with values, but
// safe (never under-marks).
static void seed_names_from_values(Ctx* ctx, char** names, size_t name_count,
                                    ASTNode* values, bool* env_changed) {
    TaintSet combined = taint_set_new(ctx->param_count);
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
                        // Sink #2 uses only the RHS's taint per the design
                        // ("rhs is tainted with i"), for both plain `=` and
                        // compound `op=` forms.
                        TaintSet rhs = expr_taint(ctx, b->right);
                        assign_to_lvalue(ctx, b->left, &rhs, env_changed);
                        taint_set_free(&rhs);
                        break;
                    }
                    if (b->operator == TOKEN_ARROW) {
                        // Channel send `ch <- v`: the sent value is received by
                        // another goroutine or by the caller, so it outlives
                        // this function — taint(v)'s params escape. (Unlike a
                        // defer, this is a true escape at BOTH function and
                        // block granularity, so param_escape and block_escape
                        // both handle it. `<-ch` receive is a UNARY ARROW, a
                        // fresh in-bound value, correctly not a sink.)
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
                    // Index/key is an int, never tainted; the per-iteration
                    // VALUE is conservatively tainted by the whole
                    // collection's taint (element-precision is not tracked).
                    if (n->key_name && strcmp(n->key_name, "_") != 0) {
                        TaintSet empty = taint_set_new(ctx->param_count);
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
                ReturnStmtNode* n = (ReturnStmtNode*)stmt;
                for (ASTNode* v = n->values; v; v = v->next) {
                    TaintSet t = expr_taint(ctx, v);
                    mark_escapes(ctx, &t);
                    if (!taint_set_empty(&t)) *ctx->return_escapes = true;
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
                TaintSet combined = taint_set_new(ctx->param_count);
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
                // Genuinely unhandled statement kind: conservative escape of
                // every one of F's params (see expr_taint's default arm for
                // the same rationale).
                mark_all_escapes(ctx);
                break;
        }
    }
}

// Runs F's intraprocedural taint analysis to a LOCAL fixpoint (the taint map
// only grows, so repeating the whole-body walk until it stops changing is
// sound — this also handles for-loop back-edges, since a second pass
// re-encounters the loop body with the previous pass's taint already
// applied). Sinks accumulate monotonically into local_escapes/
// local_return_escapes across passes; re-firing an already-fired sink is
// harmless.
static void analyze_function_body(Registry* reg, FuncInfo* f, bool* local_escapes, bool* local_return_escapes) {
    LocalEnv env = {0};
    for (size_t i = 0; i < f->param_count; i++) {
        TaintSet seed = taint_set_new(f->param_count);
        seed.bits[i] = true;
        local_env_add_or_union(&env, f->param_names[i], &seed);
        taint_set_free(&seed);
    }

    Ctx ctx = {
        .reg = reg,
        .env = &env,
        .param_count = f->param_count,
        .escapes = local_escapes,
        .return_escapes = local_return_escapes,
    };

    // Defensive backstop, not the termination argument: the taint map is a
    // finite monotone lattice (#locals * #params bits), so the while(changed)
    // loop below always terminates on its own. This cap only guards against
    // an implementation bug turning that into an infinite loop.
    const size_t MAX_LOCAL_PASSES = 4096;
    bool changed = true;
    size_t pass = 0;
    while (changed) {
        changed = false;
        pass++;
        if (pass > MAX_LOCAL_PASSES) {
            for (size_t i = 0; i < f->param_count; i++) local_escapes[i] = true;
            *local_return_escapes = true;
            break;
        }
        if (f->decl->body) walk_stmt(&ctx, f->decl->body, &changed);
    }

    local_env_free(&env);
}

// =============================================================================
// Public API
// =============================================================================

ParamEscapeResult* param_escape_analyze(ASTNode* program) {
    Registry reg = {0};
    if (!collect_functions(&reg, program)) {
        registry_free(&reg);
        return NULL;
    }

    size_t total_bits = 0;
    for (size_t i = 0; i < reg.count; i++) total_bits += reg.items[i].param_count;
    // Cap is a bug-catcher, not the termination argument (see design doc):
    // the interprocedural lattice is finite and strictly monotone (only
    // false->true), so at most total_bits + reg.count individual fields can
    // ever flip; one full pass with no flips means we're done. Comfortable
    // margin added on top.
    size_t cap = total_bits + reg.count + 8;

    bool changed = true;
    size_t iterations = 0;
    while (changed) {
        changed = false;
        iterations++;
        bool fail_closed = iterations > cap;

        for (size_t fi = 0; fi < reg.count; fi++) {
            FuncInfo* f = &reg.items[fi];
            bool* local_escapes = f->param_count ? calloc(f->param_count, sizeof(bool)) : NULL;
            if (f->param_count && !local_escapes) {
                registry_free(&reg);
                return NULL;
            }
            bool local_return_escapes = false;

            if (fail_closed) {
                // Cap hit: fail CLOSED — mark every remaining param of every
                // function escaping, never open. This should be
                // unreachable for a correct implementation; see the
                // monotone-bound argument above.
                for (size_t i = 0; i < f->param_count; i++) local_escapes[i] = true;
                local_return_escapes = true;
            } else {
                analyze_function_body(&reg, f, local_escapes, &local_return_escapes);
            }

            for (size_t i = 0; i < f->param_count; i++) {
                if (local_escapes[i] && !f->escapes[i]) {
                    f->escapes[i] = true;
                    changed = true;
                }
            }
            if (local_return_escapes && !f->return_escapes) {
                f->return_escapes = true;
                changed = true;
            }

            free(local_escapes);
        }

        if (fail_closed) break;
    }

    ParamEscapeResult* result = xmalloc(sizeof(ParamEscapeResult));
    if (!result) {
        registry_free(&reg);
        return NULL;
    }
    result->count = reg.count;
    result->summaries = NULL;
    if (reg.count > 0) {
        result->summaries = calloc(reg.count, sizeof(ParamEscapeSummary));
        if (!result->summaries) {
            free(result);
            registry_free(&reg);
            return NULL;
        }
    }

    for (size_t i = 0; i < reg.count; i++) {
        ParamEscapeSummary* s = &result->summaries[i];
        s->function_name = strdup(reg.items[i].name);
        s->param_count = reg.items[i].param_count;
        s->return_escapes = reg.items[i].return_escapes;
        s->escapes = NULL;
        if (s->param_count > 0) {
            s->escapes = calloc(s->param_count, sizeof(bool));
            if (s->escapes) {
                memcpy(s->escapes, reg.items[i].escapes, s->param_count * sizeof(bool));
            }
        }
        if (!s->function_name || (s->param_count > 0 && !s->escapes)) {
            // Allocation failure partway through: free what we have and bail.
            param_escape_result_free(result);
            registry_free(&reg);
            return NULL;
        }
    }

    registry_free(&reg);
    return result;
}

void param_escape_result_free(ParamEscapeResult* result) {
    if (!result) return;
    for (size_t i = 0; i < result->count; i++) {
        free(result->summaries[i].function_name);
        free(result->summaries[i].escapes);
    }
    free(result->summaries);
    free(result);
}

const ParamEscapeSummary* param_escape_lookup(const ParamEscapeResult* result, const char* fn) {
    if (!result || !fn) return NULL;
    for (size_t i = 0; i < result->count; i++) {
        if (result->summaries[i].function_name && strcmp(result->summaries[i].function_name, fn) == 0) {
            return &result->summaries[i];
        }
    }
    return NULL;
}

bool param_escape_param_escapes(const ParamEscapeResult* result, const char* fn, size_t param_idx) {
    const ParamEscapeSummary* s = param_escape_lookup(result, fn);
    if (!s) return true;              // unknown function: conservative miss
    if (param_idx >= s->param_count) return true; // out of range: conservative miss
    return s->escapes[param_idx];
}
