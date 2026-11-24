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
#include "escape_core.h"
#include "nonretaining.h"
#include "local_escape.h"
#include "shim_signatures.h"
#include "types.h"
#include "token.h"
#include <stdlib.h>
#include <string.h>

// TaintSet, LocalEnv and the whole body walk now live in
// src/types/escape_core.c. This module keeps only what is its own: the
// function Registry, the interprocedural fixpoint, and the hooks that tell the
// shared engine that a PARAMETER is the source and the FUNCTION is the
// boundary. A slot here is a parameter index.

// are touched — in practice both always share the same n within one
// function's analysis). Returns true if dst changed.

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
    info->name = xstrdup(decl->name);
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
                // TWO WALKS OF ONE LIST, and nothing states they must agree.
                // The loop above sized param_names by counting; this one fills
                // it by counting again. If the two ever disagree -- a filter
                // added to one and not the other, an AST mutated in between --
                // this write goes past the end of a calloc'd array. A heap
                // overflow, silent, and shaped like nothing in particular.
                GOO_ASSERT(idx < count);
                info->param_names[idx] = xstrdup(vd->names[i]);
                if (!info->param_names[idx]) return false;
                idx++;
            }
        }
        // The postcondition of the pair: both walks reached the same total.
        // Short is as wrong as long -- it leaves NULL names that every later
        // strcmp reads through.
        GOO_ASSERT(idx == count);
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
// Hooks: what makes this pass param_escape rather than one of its siblings
// =============================================================================

// Carried through EscapeCtx.owner. The Registry is here rather than in the
// shared context because only this pass has one.
typedef struct {
    Registry* reg;
    bool*     return_escapes;  // accumulator, single bool, only ever set true
} ParamOwner;

// A parameter is bound to a NAME, so an ordinary environment union is the
// whole job. local_escape is the pass that needs more here.
static bool param_bind(EscapeCtx* ctx, const char* name, const TaintSet* value) {
    return escape_env_add_or_union(ctx->env, name, value);
}

// The interprocedural signal: F returns a value derived from one of its own
// parameters. This is the ONE line by which this pass's walk_stmt differed
// from its two siblings' before the extraction.
static void param_on_return(EscapeCtx* ctx, const TaintSet* value_taint) {
    ParamOwner* own = (ParamOwner*)ctx->owner;
    if (!escape_taint_empty(value_taint)) *own->return_escapes = true;
}

// Read the callee's summary out of the IN-PROGRESS Registry, not out of a
// finished ParamEscapeResult. This pass is computing those summaries, so it
// must see the current iterate of the outer fixpoint — which is exactly why
// this is a hook and not a shared lookup.
static bool param_callee_retention(EscapeCtx* ctx, const char* name,
                                   const bool** out_escapes, size_t* out_count,
                                   bool* out_return_escapes) {
    ParamOwner* own = (ParamOwner*)ctx->owner;
    FuncInfo* callee = registry_find(own->reg, name);
    if (!callee) return false;
    *out_escapes = callee->escapes;
    *out_count = callee->param_count;
    *out_return_escapes = callee->return_escapes;
    return true;
}

static const EscapeHooks PARAM_HOOKS = {
    .bind = param_bind,
    // No expression is a source here: the sources are parameter NAMES, seeded
    // into the environment before the walk starts.
    .expr_source_slot = NULL,
    .on_return = param_on_return,
    .callee_retention = param_callee_retention,
    // At FUNCTION granularity a defer'd call runs as part of F's own teardown,
    // before the frame is gone, so it is an ordinary call rather than a
    // handover to a possibly-outlasting context. block_escape must answer
    // differently because its boundary closes first.
    .defer_is_like_go = false,
};

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

// Sink #5 (retaining call argument) + the call-result taint rule. Also
// applies to a call reached as a bare expression statement (result
// discarded) since the sink fires purely from argument passing.

// Sink #4 (goroutine). Every argument of the launched call escapes
// unconditionally, independent of the callee's own summary. A func-literal
// callee's OWN captures still sink via escape_expr_taint's AST_FUNC_LIT case
// (sink #3) when we evaluate call->function below — that is a distinct
// mechanism from "every argument of this call" and both can fire together.

// A defer'd call executes synchronously as part of F's own teardown, before
// F's frame is gone — unlike `go`, it does not hand the value to a
// possibly-outlasting concurrent context. Not explicitly enumerated as its
// own sink kind in the design; treated the same as an ordinary call
// expression statement (sink #5 only, not the unconditional sink #4
// treatment `go` gets). Documented judgment call — see task report.

// Seeds/updates locals for a VarDeclNode or ConstDeclNode-shaped
// names/name_count/values triple (var_decl, short_var_decl, and const_decl
// all share this shape). A value-list shorter than the name list (grouped
// no-initializer decl, or a single multi-return call destructured across
// several names) is handled by giving every name the UNION of all provided
// values' taint — imprecise when names don't line up 1:1 with values, but
// safe (never under-marks).

// Borrowed pointers into the declaration's AST: the array is owned by the
// caller, the strings are not. Returns the count, and NULL in *out when 0.
// Synthetic `_N` names are the parser's placeholders for UNNAMED results, and
// `_` is a discard, so neither is a variable a bare `return` can read.
static size_t collect_result_names(const FuncDeclNode* decl, char*** out) {
    *out = NULL;
    if (!decl || !decl->return_type || decl->return_type->type != AST_STRUCT_TYPE) return 0;
    StructTypeNode* st = (StructTypeNode*)decl->return_type;
    size_t cap = 0;
    for (ASTNode* fld = st->fields; fld; fld = fld->next) {
        if (fld->type == AST_VAR_DECL) cap += ((VarDeclNode*)fld)->name_count;
    }
    if (cap == 0) return 0;
    char** names = calloc(cap, sizeof(char*));
    // Fail closed is not available here: without the names, a bare return is
    // blind. Abort rather than under-mark.
    GOO_ASSERT(names != NULL);
    size_t n = 0;
    for (ASTNode* fld = st->fields; fld; fld = fld->next) {
        if (fld->type != AST_VAR_DECL) continue;
        VarDeclNode* vd = (VarDeclNode*)fld;
        for (size_t i = 0; vd->names && i < vd->name_count; i++) {
            const char* nm = vd->names[i];
            if (!nm || strcmp(nm, "_") == 0 || is_synthetic_result_name(nm)) continue;
            names[n++] = (char*)nm;
        }
    }
    if (n == 0) {
        free(names);
        return 0;
    }
    *out = names;
    return n;
}

// Runs F's intraprocedural taint analysis to a LOCAL fixpoint (the taint map
// only grows, so repeating the whole-body walk until it stops changing is
// sound — this also handles for-loop back-edges, since a second pass
// re-encounters the loop body with the previous pass's taint already
// applied). Sinks accumulate monotonically into local_reasons/
// local_return_escapes across passes; re-firing an already-fired sink is
// harmless.
static void analyze_function_body(Registry* reg, FuncInfo* f, EscapeReasons* local_reasons, bool* local_return_escapes) {
    LocalEnv env = {0};
    for (size_t i = 0; i < f->param_count; i++) {
        TaintSet seed = escape_taint_new(f->param_count);
        // escape_taint_new uses a bare calloc and can hand back a NULL bits
        // with a non-zero n. param_count is > 0 here, because this loop ran.
        GOO_ASSERT(seed.bits != NULL);
        seed.bits[i] = true;
        escape_env_add_or_union(&env, f->param_names[i], &seed);
        escape_taint_free(&seed);
    }

    // Named results are locals that a bare `return` reads back. Seeding them
    // with an empty taint keeps `r = p` a propagation into `r` instead of a
    // global store, so the taint is still there for the bare return to report.
    char** result_names = NULL;
    size_t result_count = collect_result_names(f->decl, &result_names);
    for (size_t i = 0; i < result_count; i++) {
        TaintSet empty = escape_taint_new(f->param_count);
        escape_env_add_or_union(&env, result_names[i], &empty);
        escape_taint_free(&empty);
    }

    ParamOwner own = {
        .reg = reg,
        .return_escapes = local_return_escapes,
    };
    EscapeCtx ctx = {
        .env = &env,
        .slot_count = f->param_count,
        .reasons = local_reasons,
        .hooks = &PARAM_HOOKS,
        .owner = &own,
        .result_names = result_names,
        .result_count = result_count,
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
            for (size_t i = 0; i < f->param_count; i++) local_reasons[i] = ESCAPE_REASON_UNCLASSIFIED;
            *local_return_escapes = true;
            break;
        }
        if (f->decl->body) escape_walk_stmt(&ctx, f->decl->body, &changed);
    }

    free(result_names);
    escape_env_free(&env);
}

static void compute_returns_fresh(Registry* reg, ParamEscapeResult* result,
                                  ASTNode* program);

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
    bool hit_cap = false;
    while (changed) {
        changed = false;
        iterations++;
        bool fail_closed = iterations > cap;
        if (fail_closed) hit_cap = true;

        for (size_t fi = 0; fi < reg.count; fi++) {
            FuncInfo* f = &reg.items[fi];
            EscapeReasons* local_reasons = f->param_count ? calloc(f->param_count, sizeof(EscapeReasons)) : NULL;
            if (f->param_count && !local_reasons) {
                registry_free(&reg);
                return NULL;
            }
            bool local_return_escapes = false;

            if (fail_closed) {
                // Cap hit: fail CLOSED — mark every remaining param of every
                // function escaping, never open. This should be
                // unreachable for a correct implementation; see the
                // monotone-bound argument above.
                for (size_t i = 0; i < f->param_count; i++) local_reasons[i] = ESCAPE_REASON_UNCLASSIFIED;
                local_return_escapes = true;
            } else {
                analyze_function_body(&reg, f, local_reasons, &local_return_escapes);
            }

            for (size_t i = 0; i < f->param_count; i++) {
                if (local_reasons[i] != ESCAPE_REASON_NONE && !f->escapes[i]) {
                    f->escapes[i] = true;
                    changed = true;
                }
            }
            if (local_return_escapes && !f->return_escapes) {
                f->return_escapes = true;
                changed = true;
            }

            free(local_reasons);
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
        s->function_name = xstrdup(reg.items[i].name);
        s->param_count = reg.items[i].param_count;
        s->return_escapes = reg.items[i].return_escapes;
        s->is_method = reg.items[i].decl->receiver != NULL;
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

    // returns_fresh stays false everywhere (calloc) when the cap was hit: the
    // summaries above are then a fail-closed guess, and false only leaks.
    if (!hit_cap) compute_returns_fresh(&reg, result, program);

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

// =============================================================================
// returns_fresh: does the caller OWN what F returns? (finding C2)
// =============================================================================
//
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
// It runs AFTER the escape fixpoint above, not inside it. The local rule needs
// local_escape's reasons, and local_escape needs finished parameter summaries.
// Nothing in that fixpoint reads returns_fresh, so the order changes no answer.

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
        *arr = xrealloc(*arr, *cap * sizeof(**arr));
    }
    (*arr)[(*len)++] = name;
}

static void scan_declare(FreshScan* s, const char* name) {
    if (!name || strcmp(name, "_") == 0) return;
    scan_push_name(&s->scope, &s->scope_len, &s->scope_cap, name);
}

static void scan_bind(FreshScan* s, const char* name, ASTNode* value) {
    if (!name || strcmp(name, "_") == 0) return;
    if (s->binding_count >= s->binding_cap) {
        s->binding_cap = s->binding_cap ? s->binding_cap * 2 : 8;
        s->bindings = xrealloc(s->bindings, s->binding_cap * sizeof(*s->bindings));
    }
    s->bindings[s->binding_count++] = (FreshBinding){ .name = name, .value = value };
}

static bool scan_in_scope(const FreshScan* s, const char* name) {
    for (size_t i = s->scope_len; i > 0; i--) {
        if (strcmp(s->scope[i - 1], name) == 0) return true;
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
    if (!e) return;
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
            if (op && op->type == AST_IDENTIFIER) scan_bind(s, ((IdentifierNode*)op)->name, NULL);
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
            for (ASTNode* a = call->args; a; a = a->next) scan_expr(s, a);
            return;
        }
        case AST_FUNC_LIT: {
            // A closure holds its captures by reference, which is an address
            // taken. The body is not walked: its returns are not F's.
            FuncLitNode* lit = (FuncLitNode*)e;
            for (size_t i = 0; i < lit->captured_count; i++) {
                scan_push_name(&s->addr_taken, &s->addr_count, &s->addr_cap,
                               lit->captured_names[i]);
            }
            return;
        }
        case AST_STRUCT_LITERAL:
            for (ASTNode* v = ((StructLiteralNode*)e)->field_values; v; v = v->next) scan_expr(s, v);
            return;
        case AST_SLICE_EXPR:   // slice literal (SliceLitNode)
            for (ASTNode* v = ((SliceLitNode*)e)->elements; v; v = v->next) scan_expr(s, v);
            return;
        case AST_ARRAY_LITERAL:
            for (ASTNode* v = ((ArrayLitNode*)e)->elements; v; v = v->next) scan_expr(s, v);
            return;
        case AST_KEYED_ELEMENT:
            scan_expr(s, ((KeyedElementNode*)e)->key);
            scan_expr(s, ((KeyedElementNode*)e)->value);
            return;
        case AST_PAREN_EXPR: { // map literal (MapLitNode)
            MapLitNode* ml = (MapLitNode*)e;
            for (ASTNode* k = ml->keys; k; k = k->next) scan_expr(s, k);
            for (ASTNode* v = ml->values; v; v = v->next) scan_expr(s, v);
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
    if (value_count != 1 || values->type != AST_CALL_EXPR) return NULL;
    ASTNode* fn = ((CallExprNode*)values)->function;
    return (fn && fn->type == AST_SELECTOR_EXPR) ? values : NULL;
}

static void scan_bind_names(FreshScan* s, char** names, size_t name_count, ASTNode* values,
                            bool declare) {
    size_t value_count = 0;
    for (ASTNode* v = values; v; v = v->next) value_count++;
    ASTNode* v = values;
    for (size_t i = 0; i < name_count; i++) {
        ASTNode* bound = (value_count == name_count) ? v
                       : multi_result_binding(values, value_count);
        if (declare) scan_declare(s, names[i]);
        scan_bind(s, names[i], bound);
        if (v) v = v->next;
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
    if (!e || e->type != AST_BINARY_EXPR) return false;
    BinaryExprNode* b = (BinaryExprNode*)e;
    if (!fresh_is_assign_token(b->operator)) return false;
    if (b->left && b->left->type == AST_IDENTIFIER) {
        const char* name = ((IdentifierNode*)b->left)->name;
        if (b->operator == TOKEN_SHORT_ASSIGN) scan_declare(s, name);
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
                if (!scan_assignment(s, e)) scan_expr(s, e);
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
                    if (has_results) s->bare_return = true;
                    break;
                }
                for (ASTNode* v = n->values; v; v = v->next) {
                    scan_expr(s, v);
                    if (s->return_count >= s->return_cap) {
                        s->return_cap = s->return_cap ? s->return_cap * 2 : 4;
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
                for (ASTNode* v = n->values; v; v = v->next) scan_expr(s, v);
                scan_bind_names(s, n->names, n->name_count, n->values, true);
                break;
            }

            case AST_CONST_DECL: {
                ConstDeclNode* n = (ConstDeclNode*)stmt;
                for (ASTNode* v = n->values; v; v = v->next) scan_expr(s, v);
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
                        if (n->is_short_decl) scan_declare(s, name);
                        // A mismatch is a multi-result call: see scan_bind_names.
                        scan_bind(s, name, (tcount == vcount) ? v
                                           : multi_result_binding(n->values, vcount));
                    }
                    if (v) v = v->next;
                }
                break;
            }

            case AST_SWITCH_STMT: {
                SwitchStmtNode* n = (SwitchStmtNode*)stmt;
                scan_expr(s, n->tag);
                for (ASTNode* c = n->cases; c; c = c->next) {
                    if (c->type != AST_CASE_CLAUSE) { s->unreadable = true; continue; }
                    CaseClauseNode* cc = (CaseClauseNode*)c;
                    for (ASTNode* e = cc->exprs; e; e = e->next) scan_expr(s, e);
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
                    if (!scan_assignment(s, sc->comm)) scan_expr(s, sc->comm);
                    // A received value came from someone else.
                    if (sc->bind_name) {
                        if (sc->is_declare == 1) scan_declare(s, sc->bind_name);
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

// What one function's return paths need: either a path that is borrowed
// whatever the callees say, or the Goo callees whose results it returns.
typedef struct {
    bool         borrowed;
    const char** callees;   // borrowed from the AST
    size_t       callee_count, callee_cap;
} FreshFacts;

typedef struct {
    const FuncInfo*           f;
    const Registry*           reg;
    const LocalEscapeSummary* locals;   // NULL when local_escape has none for f
    FreshScan*                scan;
    FreshFacts*               facts;
} FreshClassify;

static bool name_in(const char* const* arr, size_t n, const char* name) {
    for (size_t i = 0; i < n; i++) {
        if (arr[i] && strcmp(arr[i], name) == 0) return true;
    }
    return false;
}

// A parameter or a named result. Either one holds a value from outside F's own
// allocations: the caller's argument, or a value the walk has no binding for.
static bool is_param_or_result(const FuncInfo* f, const char* name) {
    if (name_in((const char* const*)f->param_names, f->param_count, name)) return true;
    char** results = NULL;
    size_t n = collect_result_names(f->decl, &results);
    bool hit = name_in((const char* const*)results, n, name);
    free(results);
    return hit;
}

// Any name that is not a package-level one inside F: a callee spelled this way
// is a closure or a method value, not the function the registry knows.
static bool names_a_variable(const FreshClassify* c, const char* name) {
    if (is_param_or_result(c->f, name)) return true;
    for (size_t i = 0; i < c->scan->binding_count; i++) {
        if (strcmp(c->scan->bindings[i].name, name) == 0) return true;
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
static void classify_value(FreshClassify* c, ASTNode* v) {
    if (!v) { c->facts->borrowed = true; return; }
    if (is_fresh_composite(v)) return;
    // A literal is not an allocation, but no release of it can free anything:
    // a string literal's header is GOO_RC_IMMORTAL, nil is NULL, and a scalar
    // has no release slot. release_decision's value_is_release_safe accepts it
    // for the same reason, and a `return ""` error path must not make every
    // other path of the function leak.
    if (v->type == AST_LITERAL) return;
    if (v->type == AST_UNARY_EXPR) {
        UnaryExprNode* u = (UnaryExprNode*)v;
        if (u->operator == TOKEN_BIT_AND && u->operand && is_fresh_composite(u->operand)) return;
        c->facts->borrowed = true;
        return;
    }
    // goo_string_concat copies both operands into new memory. An integer `+`
    // lands here too, and codegen keeps no release slot for an integer.
    if (v->type == AST_BINARY_EXPR && ((BinaryExprNode*)v)->operator == TOKEN_PLUS) return;
    if (v->type != AST_CALL_EXPR) { c->facts->borrowed = true; return; }

    ASTNode* fn = ((CallExprNode*)v)->function;
    if (fn && fn->type == AST_IDENTIFIER) {
        const char* name = ((IdentifierNode*)fn)->name;
        if (names_a_variable(c, name)) { c->facts->borrowed = true; return; }
        bool builtin_alloc = strcmp(name, "new") == 0 || strcmp(name, "make") == 0;
        if (builtin_alloc && !registry_find((Registry*)c->reg, name)) return;
        facts_add_callee(c->facts, name);
        return;
    }
    if (fn && fn->type == AST_SELECTOR_EXPR) {
        SelectorExprNode* sel = (SelectorExprNode*)fn;
        // A base that is a variable makes this a method call, not a package one.
        if (sel->expr && sel->expr->type == AST_IDENTIFIER && sel->selector
            && !names_a_variable(c, ((IdentifierNode*)sel->expr)->name)
            && shim_signature_is_non_retaining(((IdentifierNode*)sel->expr)->name,
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
    const char* name = ((IdentifierNode*)r->value)->name;
    if (!r->local_in_scope || is_param_or_result(c->f, name)) { c->facts->borrowed = true; return; }
    if (name_in(c->scan->addr_taken, c->scan->addr_count, name)) { c->facts->borrowed = true; return; }

    EscapeReasons why = ESCAPE_REASON_ALL;
    for (size_t i = 0; c->locals && i < c->locals->local_count; i++) {
        if (strcmp(c->locals->local_names[i], name) == 0) { why = c->locals->reasons[i]; break; }
    }
    if ((why & ~ESCAPE_REASON_RETURN) != 0) { c->facts->borrowed = true; return; }

    bool any = false;
    for (size_t i = 0; i < c->scan->binding_count; i++) {
        const FreshBinding* b = &c->scan->bindings[i];
        if (strcmp(b->name, name) != 0) continue;
        any = true;
        // classify_value answers borrowed for NULL and for an identifier: the
        // other name may still hold the value, so it is an alias, not an owner.
        classify_value(c, b->value);
    }
    if (!any) c->facts->borrowed = true;
}

// The number of result slots. Several results come as an AST_STRUCT_TYPE of
// VarDecl fields, and so does one NAMED result. An anonymous struct result
// type has the same shape and can count as several, which only refuses more.
static size_t result_slot_count(const FuncDeclNode* decl) {
    if (!decl->return_type) return 0;
    if (decl->return_type->type != AST_STRUCT_TYPE) return 1;
    size_t n = 0;
    for (ASTNode* fld = ((StructTypeNode*)decl->return_type)->fields; fld; fld = fld->next) {
        n += (fld->type == AST_VAR_DECL) ? ((VarDeclNode*)fld)->name_count : 1;
    }
    return n;
}

static void collect_fresh_facts(const Registry* reg, const FuncInfo* f,
                                const LocalEscapeSummary* locals, FreshFacts* facts) {
    // No body means nothing to prove from.
    if (!f->decl->body) { facts->borrowed = true; return; }
    // Each result can be fresh on its own while two of them are one object
    // (`return b, b`, or `return b, &Holder{p: b}`). A caller that owned both
    // would free the object twice, so no multi-result function is fresh.
    if (result_slot_count(f->decl) > 1) { facts->borrowed = true; return; }

    FreshScan scan = {0};
    scan_stmts(&scan, f->decl->body, f->decl->return_type != NULL);
    if (scan.unreadable || scan.bare_return) {
        // A bare return gives the named results, which hold values from no
        // binding this walk records: their zero values, or a parameter.
        facts->borrowed = true;
        scan_free(&scan);
        return;
    }

    FreshClassify c = { .f = f, .reg = reg, .locals = locals, .scan = &scan, .facts = facts };
    for (size_t i = 0; i < scan.return_count && !facts->borrowed; i++) {
        const FreshReturn* r = &scan.returns[i];
        if (r->value->type == AST_IDENTIFIER) classify_local(&c, r);
        else classify_value(&c, r->value);
    }
    scan_free(&scan);
}

// local_escape keeps one summary per function WITH A BODY, in declaration
// order, and the registry keeps one per function. Pair them by walking both,
// not by name: two methods can share a name. The name of each pair is checked,
// and the first mismatch ends the pairing.
static const LocalEscapeSummary** pair_local_summaries(const Registry* reg,
                                                       const LocalEscapeResult* le) {
    const LocalEscapeSummary** out = xcalloc(reg->count, sizeof(*out));   // count > 0: the caller returns early on 0
    size_t li = 0;
    for (size_t fi = 0; le && fi < reg->count; fi++) {
        if (!reg->items[fi].decl->body) continue;
        if (li >= le->count) break;
        const LocalEscapeSummary* ls = &le->summaries[li++];
        // Once one pair disagrees, every later pair may be shifted, and a
        // shifted pair can still match by name when two methods share one.
        // Leave this entry and all later ones NULL: those functions prove
        // nothing about their locals.
        if (!ls->function_name || strcmp(ls->function_name, reg->items[fi].name) != 0) break;
        out[fi] = ls;
    }
    return out;
}

static void compute_returns_fresh(Registry* reg, ParamEscapeResult* result,
                                  ASTNode* program) {
    if (reg->count == 0) return;
    LocalEscapeResult* le = local_escape_analyze(program, result);
    // No local summaries: leave every returns_fresh false.
    if (!le) return;

    const LocalEscapeSummary** locals = pair_local_summaries(reg, le);
    FreshFacts* facts = xcalloc(reg->count, sizeof(*facts));
    for (size_t fi = 0; fi < reg->count; fi++) {
        collect_fresh_facts(reg, &reg->items[fi], locals[fi], &facts[fi]);
    }

    // may_borrow only rises, so this stops after at most reg->count + 1 passes.
    bool* may_borrow = xcalloc(reg->count, sizeof(bool));
    for (size_t fi = 0; fi < reg->count; fi++) may_borrow[fi] = facts[fi].borrowed;
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t fi = 0; fi < reg->count; fi++) {
            if (may_borrow[fi]) continue;
            for (size_t k = 0; k < facts[fi].callee_count; k++) {
                const char* callee = facts[fi].callees[k];
                // A bare call never calls a method, so only a function with no
                // receiver can match. EVERY such function with that name must
                // be fresh, and no match at all is an unknown callee: a func
                // variable, or a name this registry never saw.
                bool found = false, borrowed = false;
                for (size_t gj = 0; gj < reg->count; gj++) {
                    if (reg->items[gj].decl->receiver) continue;
                    if (strcmp(reg->items[gj].name, callee) != 0) continue;
                    found = true;
                    if (may_borrow[gj]) borrowed = true;
                }
                if (!found || borrowed) {
                    may_borrow[fi] = true;
                    changed = true;
                    break;
                }
            }
        }
    }

    for (size_t fi = 0; fi < reg->count && fi < result->count; fi++) {
        result->summaries[fi].returns_fresh = !may_borrow[fi];
    }

    free(may_borrow);
    for (size_t fi = 0; fi < reg->count; fi++) free(facts[fi].callees);
    free(facts);
    free(locals);
    local_escape_result_free(le);
}

// Every summary a bare call `fn()` could mean must pass `want_owned`'s test.
static bool every_bare_callee(const ParamEscapeResult* result, const char* fn, bool want_owned) {
    if (!result || !fn) return false;
    bool found = false;
    for (size_t i = 0; i < result->count; i++) {
        const ParamEscapeSummary* s = &result->summaries[i];
        if (s->is_method) continue;
        if (!s->function_name || strcmp(s->function_name, fn) != 0) continue;
        if (!s->returns_fresh) return false;
        if (want_owned && s->return_escapes) return false;
        found = true;
    }
    return found;
}

bool param_escape_returns_fresh(const ParamEscapeResult* result, const char* fn) {
    return every_bare_callee(result, fn, false);
}

bool param_escape_call_result_is_owned(const ParamEscapeResult* result, const char* fn) {
    return every_bare_callee(result, fn, true);
}
