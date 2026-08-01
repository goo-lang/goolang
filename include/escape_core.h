#ifndef ESCAPE_CORE_H
#define ESCAPE_CORE_H

// The taint-propagation engine shared by the three escape analyses.
//
//   module          source                        boundary
//   --------------  ----------------------------  ----------------
//   param_escape    a function parameter          the function
//   block_escape    an alloc site in `arena {}`    the arena block
//   local_escape    a LOCAL VARIABLE               the function
//
// All three ask the same question of the same AST with the source and the
// boundary moved, so all three ran the same walk. Until this module existed
// they ran THREE HAND-MAINTAINED COPIES of it, and every header said a third
// consumer would justify extracting the shared core. local_escape was that
// third consumer.
//
// THE COPIES HAD ALREADY DRIFTED, TWICE, AND BOTH DEFECTS SHIPPED:
//
//   1. `AST_POSTFIX_EXPR` had an arm in block_escape and the copy into
//      local_escape lost it, so the default arm marked EVERY local escaping.
//      `for i := 0; i < n; i++` is the most common loop in Go, so the omission
//      cost the analysis nearly all of its precision. Found by a test row that
//      passed with `i = i + 1` and failed with `i++`.
//   2. local_escape's `handle_defer_call` carries block_escape's comment
//      verbatim — "runs at the enclosing FUNCTION's exit, which always happens
//      AFTER this arena block has closed" — in a module that has no block. By
//      that comment's own reasoning local_escape should use param_escape's
//      treatment. The behaviour is conservative and therefore SOUND, so it is
//      preserved here rather than fixed silently. See `defer_is_like_go`.
//
// Neither defect is the kind a reviewer catches by reading one file. That is
// the argument for this module.
//
// =============================================================================
// SOUNDNESS INVARIANT — identical in all three passes, and asymmetric
// =============================================================================
//
// `true` (escapes) is ALWAYS a safe answer. The worst case is a lost
// optimisation: keep heap-allocating, keep leaking, exactly as today.
//
// `false` asserts "provably does not outlive the boundary", and a consumer
// FREES on it. Under-marking is therefore the ONLY bug class that can dangle a
// pointer. Every construct this engine does not precisely understand must fall
// to the default arm, which marks everything.
//
// A change here lands in all three passes at once. That is the point, and it
// is also the risk: run `make param-escape-test block-escape-test
// local-escape-test` (20 + 31 + 14 rows) plus the arena valgrind probes before
// trusting any edit to this file.

#include "ast.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// "No slot for this node/name." Every pass treats a miss as conservative.
#define ESCAPE_NO_SLOT ((size_t)-1)

// =============================================================================
// EscapeReasons — WHY a slot escaped, not merely THAT it did
// =============================================================================
// ADR 0005. The accumulator held one bit per slot, and two separate pieces of
// work stalled because that bit conflates causes needing opposite actions:
// `m[s] = 1` and `return s` both set it, but a map may own the first buffer and
// must never own the second.
//
// THE BOOLEAN IS NOT GONE, IT IS DERIVED. `escapes` is `reasons != 0`, so a
// consumer that only wants "did it escape at all" is unchanged. Every public
// lookup in the three passes still returns a bool and still fails CLOSED.
//
// THE SOUNDNESS INVARIANT IS UNCHANGED AND STILL ASYMMETRIC, with one addition
// that is easy to get wrong. Setting MORE reasons is always safe. Setting NO
// reason is an under-mark, and under-marking is the only bug class that dangles
// a pointer — so a mark with an empty set is treated as UNCLASSIFIED rather
// than as "did not escape". A caller that forgets a reason therefore loses
// precision instead of freeing live memory.
//
// The dangerous edit is a reason DROPPED, not a reason renamed. Renaming moves
// a slot between causes and the consumer's test simply stops matching.
// Dropping removes the mark, and the value is then freed while it is live.
typedef uint32_t EscapeReasons;

#define ESCAPE_REASON_NONE          ((EscapeReasons)0)

// THE CONSERVATIVE CATCH-ALL, and it must never stop being raised. Three kinds
// of site use it, and only the first is a "we do not know" answer:
//
//   the two DEFAULT arms   an unrecognised expression or statement kind. The
//                          engine cannot see what happens to the value, so
//                          everything escapes.
//   assign_to_lvalue's     a store with no target node at all, and a
//   two defensive arms     non-identifier target of a SHORT declaration, which
//                          Go's grammar does not produce.
//
// A new sink added without a reason lands here by way of escape_mark's empty-set
// guard, which costs precision and never safety.
#define ESCAPE_REASON_UNCLASSIFIED    ((EscapeReasons)1u << 0)

// `return x` — the value leaves the FUNCTION. The reason the map-key consumer
// exists to exclude: a map may own a key, but never one the caller still holds.
#define ESCAPE_REASON_RETURN          ((EscapeReasons)1u << 1)

// A store to a name this unit does not own. Membership of LocalEnv IS the
// engine's definition of "a plain local here", so an absent name is a package
// global OR a name from an enclosing scope, and the two are not told apart.
#define ESCAPE_REASON_GLOBAL_STORE    ((EscapeReasons)1u << 2)

// The VALUE side of a store through a non-identifier lvalue: `m[k] = v`,
// `obj.f = v`, `*p = v`. The container decides how long v lives, and the
// container's own fate is decided elsewhere.
#define ESCAPE_REASON_CONTAINER_STORE ((EscapeReasons)1u << 3)

// The KEY side of the same store — the subscript, not the value. `m[k] = v`
// stores k as well, because goo_map_set_sv keeps the key pointer verbatim.
//
// THIS IS THE REASON ADR 0005 WAS WRITTEN FOR. A local reaching this sink and
// NO OTHER is a local the map may take ownership of, worth a measured 180,000
// bytes on bench/daemon. Note the name says SUBSCRIPT and not MAP_KEY: this
// engine is purely syntactic and cannot tell a map index from a slice index.
// A consumer that acts on it must supply the type knowledge itself.
#define ESCAPE_REASON_SUBSCRIPT_STORE ((EscapeReasons)1u << 4)

// An argument to a call whose callee RETAINS it, per the callee summary. An
// unresolved or external callee retains everything, which is the conservative
// pair the hook contract already specifies.
#define ESCAPE_REASON_CALL_RETAIN     ((EscapeReasons)1u << 5)

// The CALLEE EXPRESSION's own taint, for a non-identifier callee. `p.m()`
// marks `p`, because a method's receiver is not a member of call->args and
// nothing else would reach it.
//
// THIS IS A MEASURED CEILING ON ARC RATHER THAN A DEFECT. It makes every local
// with a method set unreleasable — sync.Mutex, bytes.Buffer, os.File, any user
// struct with methods. It is load-bearing: discarding this taint left `p.m()`
// inside an arena block arena-eligible and a method storing its receiver then
// dangled. Naming it does not lift the ceiling. It makes the ceiling
// measurable, which is the first thing a fix would need.
#define ESCAPE_REASON_CALLEE_VALUE    ((EscapeReasons)1u << 6)

// `go f(x)` — the goroutine outlives every boundary the three passes have.
#define ESCAPE_REASON_GO_ARG          ((EscapeReasons)1u << 7)

// `defer f(x)`, and ONLY where the pass sets defer_is_like_go. At FUNCTION
// granularity a defer runs inside the frame and is an ordinary call, so
// param_escape never raises this. block_escape and local_escape do.
#define ESCAPE_REASON_DEFER_ARG       ((EscapeReasons)1u << 8)

// `ch <- x` — the value is handed to whoever receives it. A `<-ch` RECEIVE is
// not a sink and does not appear here.
#define ESCAPE_REASON_CHAN_SEND       ((EscapeReasons)1u << 9)

// A cell captured by a function literal. The literal may outlive the boundary,
// so everything it closes over may too.
#define ESCAPE_REASON_CLOSURE_CAPTURE ((EscapeReasons)1u << 10)

// Every reason. The value a lookup returns when it cannot answer — see
// local_escape.h's fail-closed contract. Returning this rather than zero is
// what stops a future "escapes ONLY via X" test passing on a miss.
#define ESCAPE_REASON_ALL             ((EscapeReasons)((1u << 11) - 1u))

// =============================================================================
// TaintSet — "which of this unit's slots may this value alias"
// =============================================================================
// Sized to the CURRENT unit's slot count and never resized, so every taint set
// within one unit's analysis shares that width. A "slot" is a parameter, an
// allocation site, or a local, depending on the pass.

typedef struct {
    bool*  bits;
    size_t n;
} TaintSet;

TaintSet escape_taint_new(size_t n);
void     escape_taint_free(TaintSet* t);
bool     escape_taint_empty(const TaintSet* t);
TaintSet escape_taint_copy(const TaintSet* src);
TaintSet escape_taint_all(size_t n);

// Unions `src` into `dst`. Returns true if that GREW dst's information (a bit
// flipped false->true), which is what the fixpoint loops test for termination.
bool escape_taint_union_into(TaintSet* dst, const TaintSet* src);

// =============================================================================
// LocalEnv — name -> current taint, for one unit's walk
// =============================================================================
// Membership in this map IS the engine's definition of "a plain local of this
// unit". A name that is absent — a package global, or a name from an enclosing
// scope — is conservatively a store-escape target for sink #2.

typedef struct {
    char*    name;   // owned
    TaintSet taint;
} LocalVar;

typedef struct {
    LocalVar* vars;
    size_t    count;
    size_t    capacity;
} LocalEnv;

void      escape_env_free(LocalEnv* env);
LocalVar* escape_env_find(LocalEnv* env, const char* name);

// Adds `name` seeded with a COPY of `value` if absent, else unions into the
// existing entry. Returns true if this grew the map's information. Returns
// false on allocation failure, failing CLOSED by simply not registering the
// local — an unregistered name is then a sink target, which is the safe side.
bool escape_env_add_or_union(LocalEnv* env, const char* name, const TaintSet* value);

// =============================================================================
// EscapeCtx — the walk context, and the six hooks the passes differ by
// =============================================================================

typedef struct EscapeCtx EscapeCtx;

typedef struct EscapeHooks {
    // Bind `name` to `value`. Returns true if the environment grew.
    //
    // param_escape and block_escape pass escape_env_add_or_union straight
    // through. local_escape needs more: a LOCAL is its source, so binding a
    // name must also register it and set its OWN bit. Making this a hook is
    // what removed five near-identical call sites from walk_stmt.
    bool (*bind)(EscapeCtx* ctx, const char* name, const TaintSet* value);

    // Is `expr` ITSELF a source slot of this pass? Writes the slot index and
    // returns true if so. Only block_escape ever answers true: its sources are
    // allocation sites (`&T{}`, `new(T)`, a closure environment), which are
    // expressions. A parameter and a local are both bound to NAMES instead, so
    // param_escape and local_escape leave this NULL.
    bool (*expr_source_slot)(EscapeCtx* ctx, ASTNode* expr, size_t* out_slot);

    // A `return` statement's value taint, already marked as escaping by the
    // caller. param_escape uses this to record `return_escapes`, its
    // interprocedural signal. The other two leave it NULL.
    void (*on_return)(EscapeCtx* ctx, const TaintSet* value_taint);

    // The callee's summary: per-parameter retention, plus whether the callee
    // may give back a value derived from one of its own parameters. Returns
    // false when no summary exists, which every pass treats as both retaining
    // and return-escaping — the conservative pair.
    //
    // param_escape reads its own in-progress Registry, because it is COMPUTING
    // these summaries and must see the current iterate. block_escape and
    // local_escape read a finished ParamEscapeResult. Same shape, two sources.
    bool (*callee_retention)(EscapeCtx* ctx, const char* name,
                             const bool** out_escapes, size_t* out_count,
                             bool* out_return_escapes);

    // Does a `defer f(x)` mark x escaping the way `go f(x)` does?
    //
    // FALSE at FUNCTION granularity (param_escape): a deferred call runs as
    // part of the function's own teardown, before the frame is gone, so it is
    // an ordinary call.
    //
    // TRUE at BLOCK granularity (block_escape): the defer fires at the
    // enclosing function's exit, which is always AFTER the arena block closed
    // and freed its arena, so a deferred argument outlives the block exactly
    // the way a goroutine argument does.
    //
    // local_escape sets TRUE, and that is CONSERVATIVE RATHER THAN CORRECT.
    // Its boundary is the function, so param_escape's answer is the precise
    // one. It is left true because a release consumer's ordering against
    // deferred calls is not yet decided: if codegen emits releases BEFORE the
    // defer block runs, the precise answer dangles a pointer. Tighten this
    // only together with that ordering, and with a row that pins it.
    bool defer_is_like_go;
} EscapeHooks;

struct EscapeCtx {
    LocalEnv* env;
    size_t    slot_count;  // params / alloc sites / locals, per pass
    // Accumulator, length slot_count. Bits are only ever ADDED, which is what
    // makes the fixpoint loops monotone and therefore terminating.
    EscapeReasons* reasons;

    const EscapeHooks* hooks;
    void* owner;           // the pass's own context; hooks cast this back
};

// Mark every slot in `t` as escaping FOR REASON `why`. Bounded by slot_count,
// because a taint set may be wider than the accumulator in the pass that built
// it. An empty `why` is raised to ESCAPE_REASON_UNCLASSIFIED — a mark that
// records nothing would read as "does not escape", which is the one direction
// that dangles a pointer.
void escape_mark(EscapeCtx* ctx, const TaintSet* t, EscapeReasons why);

// Mark EVERY slot escaping. The default arm of an unrecognised construct.
void escape_mark_all(EscapeCtx* ctx, EscapeReasons why);

// =============================================================================
// The walk
// =============================================================================

// Taint of an expression. Recursive, allocates its result.
TaintSet escape_expr_taint(EscapeCtx* ctx, ASTNode* expr);

// Walk one statement, applying every sink. Sets `*env_changed` when a binding
// grew, which drives the caller's local fixpoint loop.
void escape_walk_stmt(EscapeCtx* ctx, ASTNode* stmt, bool* env_changed);

// Seed `names` from the parallel `values` list, the shape `a, b := f(), g()`
// and `var a, b = x, y` share. Exposed because the per-pass drivers seed
// parameters and results through it before the body walk starts.
void escape_seed_names_from_values(EscapeCtx* ctx, char** names, size_t name_count,
                                   ASTNode* values, bool* env_changed);

#endif // ESCAPE_CORE_H
