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
                info->param_names[idx] = xstrdup(vd->names[i]);
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
        seed.bits[i] = true;
        escape_env_add_or_union(&env, f->param_names[i], &seed);
        escape_taint_free(&seed);
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

    escape_env_free(&env);
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
