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
//   - Discovery REUSES the engine: pass A runs escape_walk_stmt with zero-width
//     taint sets purely so seed_local registers every declared name. A
//     hand-written structural walker would have to know every declaration
//     shape and would drift the first time one was added.
//
// The sinks are untouched: return, store to a non-local, closure capture,
// goroutine argument, channel send, defer argument, and a retaining call
// argument (including a method-call RECEIVER, which is not a member of
// call->args and needed its own marking).

#include "local_escape.h"
#include "escape_core.h"
#include "nonretaining.h"
#include "token.h"
#include <stdlib.h>
#include <string.h>

#define BLOCK_ESCAPE_NO_UNIT ((size_t)-1)

// =============================================================================
// TaintSet, LocalEnv and the whole body walk now live in
// src/types/escape_core.c. This file used to carry a hand-maintained copy of
// them, and that copy is where BOTH recorded drift defects landed: the missing
// `AST_POSTFIX_EXPR` arm, and a defer comment describing block_escape's
// boundary rather than this module's. See include/escape_core.h.
//
// This module keeps what is its own: unit discovery, and the hooks that tell
// the shared engine a LOCAL is the source and the FUNCTION is the boundary.
// A slot here is a local index.

// =============================================================================
// Pass 1: unit discovery. A Unit is one FUNCTION body (block_escape's unit was
// one `arena {}` block). local_names[] holds every local this function
// declares, in first-seen order, and each name is one bit in the taint sets.
//
// DISCOVERY REUSES THE ENGINE rather than duplicating it. Every declaration
// form funnels through escape_env_add_or_union, so running escape_walk_stmt ONCE with
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
    u->fn_name = fn_name ? xstrdup(fn_name) : NULL;
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
    u->local_names[u->local_count] = xstrdup(name);
    if (!u->local_names[u->local_count]) return false;
    u->local_count++;
    return true;
}

// =============================================================================
// Pass 2: per-unit taint fixpoint. The engine itself is
// src/types/escape_core.c; what follows is only this pass's half of the
// contract — its owner struct and its hooks.
// =============================================================================

// Carried through EscapeCtx.owner.
typedef struct {
    const ParamEscapeResult* summaries; // may be NULL; borrowed
    char**    local_names;  // this unit's locals, borrowed, same order as escapes[]
    size_t    local_count;
    Unit*     unit;         // discovery target during pass A; NULL during pass B
} LocalOwner;

// A local's own bit, by NAME. block_escape matched an alloc-site AST node
// here; a local is identified by its name, because the same declaration can be
// re-walked across fixpoint passes and must land on the same bit.
static size_t find_local_index(EscapeCtx* ctx, const char* name) {
    LocalOwner* own = (LocalOwner*)ctx->owner;
    if (!name) return ESCAPE_NO_SLOT;
    for (size_t i = 0; i < own->local_count; i++) {
        if (own->local_names[i] && strcmp(own->local_names[i], name) == 0) return i;
    }
    return ESCAPE_NO_SLOT;
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
static bool seed_local(EscapeCtx* ctx, const char* name, const TaintSet* value) {
    LocalOwner* own = (LocalOwner*)ctx->owner;
    if (own->unit) {
        if (!unit_add_local(own->unit, name)) {
            // Allocation failure during discovery. Fail CLOSED: an unregistered
            // local has no bit, so it is never reported non-escaping, and the
            // public lookup answers `true` on a miss.
            return false;
        }
        return escape_env_add_or_union(ctx->env, name, value);
    }
    size_t idx = find_local_index(ctx, name);
    if (idx == ESCAPE_NO_SLOT) {
        // Not a discovered local (e.g. "_", or a shape discovery missed).
        // Seeding no own bit is safe: it simply never becomes a source.
        return escape_env_add_or_union(ctx->env, name, value);
    }
    TaintSet seeded = escape_taint_copy(value);
    if (idx < seeded.n) seeded.bits[idx] = true;
    bool changed = escape_env_add_or_union(ctx->env, name, &seeded);
    escape_taint_free(&seeded);
    return changed;
}

static bool local_callee_retention(EscapeCtx* ctx, const char* name,
                                   const bool** out_escapes, size_t* out_count,
                                   bool* out_return_escapes) {
    LocalOwner* own = (LocalOwner*)ctx->owner;
    const ParamEscapeSummary* callee = param_escape_lookup(own->summaries, name);
    if (!callee) return false;
    *out_escapes = callee->escapes;
    *out_count = callee->param_count;
    *out_return_escapes = callee->return_escapes;
    return true;
}

static const EscapeHooks LOCAL_HOOKS = {
    // seed_local, NOT a plain environment union — a LOCAL is this pass's
    // source, so binding a name must also register it and set its own bit.
    // Before the extraction this difference was five near-identical edits
    // scattered through a 200-line copy of walk_stmt.
    .bind = seed_local,
    // No expression is a source: a local is a NAME, seeded at its declaration.
    .expr_source_slot = NULL,
    .on_return = NULL,
    .callee_retention = local_callee_retention,
    // TRUE, and that is CONSERVATIVE RATHER THAN CORRECT — see
    // EscapeHooks.defer_is_like_go. This pass's boundary is the FUNCTION, so
    // param_escape's `false` is the precise answer. The value is preserved as
    // it was because a release consumer's ordering against deferred calls is
    // not yet decided: releases emitted BEFORE the defer block runs would
    // dangle. Tighten it only together with that ordering, and with a row.
    .defer_is_like_go = true,
};

// Sink #2 (store to a non-block-local location). Membership in ctx->env IS
// this module's definition of "a plain local of THIS unit" — see this
// file's header comment: it is seeded ONLY by var/:=/etc. encountered
// while walking this unit's own body, so a function parameter, an
// outer-scope (pre-block) local, or a package global is correctly absent
// and therefore a sink target. `_` is a pure discard: neither a sink nor a
// local. Any non-identifier lvalue (*p, obj.field, arr[k], ...) is
// unconditionally a sink, same conservative simplification param_escape.c
// documents for its own sink #2.

// Sink #5 (retaining call argument) + the call-result taint rule, using
// `summaries` (7a's ParamEscapeResult) in place of param_escape.c's own
// Registry lookup. param_escape_lookup/param_escape_param_escapes already
// return the conservative "true"/NULL on an unknown function or a NULL
// result, so "summaries == NULL" and "external/unregistered callee" fall
// out through the exact same code path here.

// Sink #4 (goroutine): every argument of the launched call escapes
// unconditionally, independent of the callee's own summary — identical to
// param_escape.c's handle_go_call.

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

// Seeds/updates locals for a VarDeclNode/ConstDeclNode-shaped
// names/name_count/values triple. Identical to param_escape.c's
// seed_names_from_values.

// Runs one unit's intraprocedural taint analysis to a LOCAL fixpoint (same
// termination argument as param_escape.c's analyze_function_body: the
// taint map only grows, so repeating the whole-body walk until nothing
// changes is sound and also handles for-loop back-edges). LocalEnv starts
// EMPTY here — unlike a function's params, an arena block has no
// pre-existing "parameters" to seed; every local comes from a var/:=/etc.
// encountered during the walk itself.
static void analyze_unit(const ParamEscapeResult* summaries, Unit* u, EscapeReasons* reasons) {
    // PASS A — discovery. Zero-width taint sets, so every sink marks nothing
    // and the walk exists only to drive seed_local, which registers each
    // declared name on the unit. This is why discovery cannot drift away from
    // the engine: it IS the engine.
    {
        LocalEnv env = {0};
        LocalOwner own = { .summaries = summaries, .local_names = NULL,
                           .local_count = 0, .unit = u };
        EscapeCtx ctx = {
            .env = &env,
            .slot_count = 0,
            .reasons = NULL,
            .hooks = &LOCAL_HOOKS,
            .owner = &own,
        };
        bool ignored = false;
        escape_walk_stmt(&ctx, u->body, &ignored);
        escape_env_free(&env);
    }

    if (u->local_count == 0) return;

    // PASS B — the real fixpoint, now that the bit width is known.
    LocalEnv env = {0};
    LocalOwner own = { .summaries = summaries, .local_names = u->local_names,
                       .local_count = u->local_count, .unit = NULL };
    EscapeCtx ctx = {
        .env = &env,
        .slot_count = u->local_count,
        .reasons = reasons,
        .hooks = &LOCAL_HOOKS,
        .owner = &own,
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
            for (size_t i = 0; i < u->local_count; i++) reasons[i] = ESCAPE_REASON_UNCLASSIFIED;
            break;
        }
        escape_walk_stmt(&ctx, u->body, &changed);
    }

    escape_env_free(&env);
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
        EscapeReasons* reasons = NULL;

        // Pass A runs inside analyze_unit and fills u->local_names, so the
        // accumulator cannot be sized until after it. analyze_unit therefore
        // allocates nothing and this loop owns the two-step.
        {
            LocalEnv env = {0};
            LocalOwner own = { .summaries = summaries, .local_names = NULL,
                               .local_count = 0, .unit = u };
            EscapeCtx ctx = { .env = &env, .slot_count = 0, .reasons = NULL,
                              .hooks = &LOCAL_HOOKS, .owner = &own };
            bool ignored = false;
            escape_walk_stmt(&ctx, u->body, &ignored);
            escape_env_free(&env);
        }
        if (u->local_count > 0) {
            reasons = calloc(u->local_count, sizeof(EscapeReasons));
            if (!reasons) {
                local_escape_result_free(result);
                unit_list_free(&units);
                return NULL;
            }
        }

        // analyze_unit re-runs pass A harmlessly (unit_add_local is idempotent
        // by name) and then runs the fixpoint.
        analyze_unit(summaries, u, reasons);

        LocalEscapeSummary* s = &result->summaries[result->count];
        s->function_name = u->fn_name;   // ownership MOVES out of the unit
        u->fn_name = NULL;
        s->local_names = u->local_names; // ownership MOVES out of the unit
        s->local_count = u->local_count;
        u->local_names = NULL;
        u->local_count = 0;
        s->reasons = reasons;
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
        free(s->reasons);
    }
    free(result->summaries);
    free(result);
}

// Conservative on EVERY miss, per the soundness contract: an unknown function
// or an unknown local reports `true`. A consumer frees on `false`, so a miss
// must never be mistaken for "provably dies here".
bool local_escape_local_escapes(const LocalEscapeResult* result,
                                const char* fn, const char* local) {
    return local_escape_local_reasons(result, fn, local) != ESCAPE_REASON_NONE;
}

// ONE lookup, not two. The boolean above is derived from this, so the two can
// never disagree about whether a local escapes -- two pieces of state that must
// agree and are maintained separately is the shape ADR 0005 rejected a second
// boolean for, and it is the shape of PR #278's use-after-free.
//
// Every miss returns ESCAPE_REASON_ALL, which is what `true` was.
EscapeReasons local_escape_local_reasons(const LocalEscapeResult* result,
                                         const char* fn, const char* local) {
    if (!result || !fn || !local) return ESCAPE_REASON_ALL;
    for (size_t i = 0; i < result->count; i++) {
        const LocalEscapeSummary* s = &result->summaries[i];
        if (!s->function_name || strcmp(s->function_name, fn) != 0) continue;
        for (size_t j = 0; j < s->local_count; j++) {
            if (s->local_names[j] && strcmp(s->local_names[j], local) == 0) {
                return s->reasons[j];
            }
        }
        return ESCAPE_REASON_ALL;  // known function, unknown local
    }
    return ESCAPE_REASON_ALL;      // unknown function
}
