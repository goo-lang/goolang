#ifndef RELEASE_DECISION_H
#define RELEASE_DECISION_H

// T4: which locals may codegen emit `goo_release` for at function exit?
//
// FOURTH module in the ARC leg, and the FIRST that a consumer acts on by FREEING
// memory. param_escape, block_escape and local_escape all answer "may this value
// outlive a boundary". None of them answers whether a release is SAFE, and
// include/local_escape.h says so explicitly:
//
//     "This module answers 'does the value outlive F', NOT 'does the local own
//      the value'. A release consumer needs BOTH."
//
// This module is that consumer's decision procedure. It lives in src/types/ and
// not in codegen for the same reason its three siblings do: it is pure, it needs
// no LLVM, and it is therefore table-testable. Codegen only READS a verdict, the
// way it already reads block_escape's.
//
// =============================================================================
// FOUR CONDITIONS, ALL OF WHICH MUST HOLD
// =============================================================================
//
// Every one of these was established by MEASUREMENT, not by reasoning. See
// docs/adr/0002-measurements/t4_condition2_findings.md.
//
//   1. NON-ESCAPING. local_escape says the value does not outlive the function.
//
//   2. OWNED. The local holds a value it owns, not a borrowed view. Measured:
//      `b := borrowView(s)` where borrowView returns `s[1:]` reads as
//      non-escaping, and releasing it frees the CALLER's buffer. This is decided
//      at the BINDING SITE, per the table below.
//
//      NOTE what condition 2 is NOT. ADR 0004 proposed "origin-emptiness" -- an
//      empty TaintSet meaning the value aliases nothing. That is CONSTANT-FALSE
//      here: local_escape's slots ARE local indices and a local keeps its own
//      bit for life, so no local's taint is ever empty. Worse, a local borrows
//      FROM a parameter, and parameters are not in that slot space at all.
//
//   3. NOT ARENA-ROUTED. A local declared inside `arena { }` can hold an arena
//      pointer, which carries no object header, so `goo_release` would compute
//      `ptr - GOO_OBJ_HEADER_SIZE` on an interior pointer and hand it to free().
//      local_escape's boundary is the FUNCTION, so such a local reads as
//      non-escaping and nothing else stops it.
//
//   4. BOUND ONCE. A local rebound after its declaration holds a different
//      object at exit than the one condition 2 classified.
//
//      THE LOOP HALF OF THIS CONDITION IS RELAXED, and condition 6 replaces it.
//      It used to refuse every local declared inside a loop, because a release
//      at FUNCTION exit frees the value held at that instant -- one of N -- and
//      leaks the rest. That is a placement defect, not an ownership one, and
//      codegen now releases such a local at ITERATION end instead
//      (codegen_emit_loop_scope_releases, statement_codegen.c). What remains is
//      the question condition 6 answers.
//
//      "Bound once" also rules out RE-ASSIGNMENT, which is a separate hazard and
//      not merely a lost optimisation. `a := new(int)` followed by
//      `a = t.borrowedField` leaves `a` holding a value it does not own, so a
//      release at exit frees someone else's memory even though the DECLARATION
//      site was a clean allocation. Condition 2 reads the binding site, so only
//      counting bindings catches this.
//
//      This condition used to be why T4 did NOT target `err`. Measured on
//      bench/daemon/daemon.goo: `n, err := strconv.Atoi(f)` is inside handle's
//      loop, and at FUNCTION exit `err` is nil for the benchmark's input, so a
//      release there frees ZERO bytes rather than the 5.8% originally
//      projected. Released per ITERATION it frees a real error every time the
//      parse fails, which is the reclamation condition 6 unblocks.
//
//   6. DOES NOT OUTLIVE ITS ITERATION. Applies ONLY to a local whose
//      declaration is inside a loop, and it is the one genuinely new soundness
//      question in the loop-scoped release. See the block below.
//
// A FIFTH CONDITION EXISTED AND IS RETIRED. It refused any local declared inside
// a conditional block, because codegen_alloc_local_promoted hoists the alloca to
// the entry block while the initialising store stays at the DECLARATION SITE --
// so an unexecuted declaration left the slot undef, and goo_release read, and
// through __atomic_fetch_sub WROTE, through garbage. That was a LIVE BUG,
// reachable through a plain `if` (PR #265).
//
// codegen_arc_zero_slot (src/codegen/statement_codegen.c) now stores NULL into
// every release candidate's slot immediately after its alloca, and goo_release is
// a no-op on NULL, so the premise is gone. The guarantee is FAIL-CLOSED: codegen
// records no release site at all unless that store was emitted.
//
// =============================================================================
// CONDITION 6 -- the loop-carried store, and why conditions 1-3 do not cover it
// =============================================================================
//
// Established by MEASUREMENT before a line of it was written. See
// docs/adr/0002-measurements/loop_carried_store_findings.md, which recorded
// that the HAZARD and the CONTROL were indistinguishable: `last = s` with
// `last` declared outside the loop, and a loop-local nothing retains, BOTH read
// RELEASE_NO_LOOP_SCOPE. Condition 4 refused both on a syntactic property they
// share, so relaxing it alone frees `s` while `last` still points at the buffer.
//
// WHAT THE OTHER CONDITIONS ALREADY COVER, so this one does not repeat them:
//
//   passed to a callee that retains it   condition 1, via param_escape
//   returned                             condition 1
//   captured by go / defer / a closure   condition 1
//   stored into a map or a global        condition 1 -- measured: the daemon's
//                                        `f` reads RELEASE_NO_ESCAPES because
//                                        `counts[f]` makes it escape
//
// THE ONE GAP is a store into a name whose lifetime is longer than the
// iteration but SHORTER than the function, because local_escape's boundary is
// the function and it therefore answers, correctly and uselessly, that nothing
// escaped. `last = s` is that shape.
//
// THE RULE, and it is deliberately a MENTION test rather than a value-flow one:
// a local declared at loop depth D is refused when its name appears anywhere in
// the right-hand side of an assignment or declaration whose target lives longer
// than depth D. A target lives longer when it is a local declared at a
// shallower loop depth, or when it is not a plain identifier at all -- a field,
// an index or any other store this module cannot resolve.
//
// SOUND BY CONSTRUCTION, which a blacklist of sink shapes would not be. The
// walk is TOTAL: a statement kind it does not recognise sets `unreadable` and
// refuses the whole function, so enumerating the assignment forms it does
// recognise enumerates every store a readable function can perform.
//
// THE PRECISION IT GIVES UP, named so the next reader does not rediscover it:
// `n = n + len(s)` refuses `s`, even though `len` propagates nothing. A mention
// is not a flow. Refining that needs the reverse of condition 2's binding table
// -- does this expression PROPAGATE its operand, or merely READ it -- and the
// loop-carried probe is what makes such a refinement safe to attempt.
//
// =============================================================================
// CONDITION 2 -- the binding-site table
// =============================================================================
//
//   Bound to                                  Owned?
//   ----------------------------------------  -------------------------------
//   new(T), &T{}                              YES, the site is an allocation
//   a struct/slice/array/map literal          YES, a fresh allocation
//   a call to a Goo function                  iff its ParamEscapeSummary
//                                             .return_escapes is FALSE
//   a call to a C shim                        iff shim_signature_is_non_retaining
//   a slice, index or selector expression      NO -- a view into something else
//   another local, or any identifier           NO -- an alias; one owner only
//   anything not listed                        NO -- conservative
//
// `return_escapes` is documented as "does F return a value derived from one of
// its own params?", which IS the borrowed-result relation. The shim column is
// justified in src/types/shim_signatures.c against each runtime body, and that
// audit is explicit that every whitelisted `strings` entry COPIES -- the table
// even notes TrimSpace copies where Go's returns a slice of its argument.
// errors.Unwrap is 0 precisely because it returns a pointer INTO its argument.
//
// =============================================================================
// SOUNDNESS INVARIANT -- asymmetric, and the opposite way round from its siblings
// =============================================================================
//
// In the three escape passes, `true` (escapes) is the safe answer. Here the safe
// answer is `false` (do not release). A wrong `true` FREES LIVE MEMORY. Every
// construct this module does not precisely understand must therefore return
// false, with a reason that says why.

#include "ast.h"
#include <stdbool.h>
#include <stddef.h>

// Why a local was refused. Carried so a caller -- and a test row -- can assert
// the CAUSE and not merely the verdict. A row that passes for the wrong reason
// measures nothing, which this arc has now been bitten by twice.
typedef enum {
    RELEASE_OK = 0,             // all four conditions hold
    RELEASE_NO_ESCAPES,         // condition 1: may outlive the function
    RELEASE_NO_NOT_OWNED,       // condition 2: a borrowed view, or an alias
    RELEASE_NO_ARENA,           // condition 3: declared inside `arena { }`
    RELEASE_NO_LOOP_SCOPE,      // condition 4: declared inside a loop body
    RELEASE_NO_REBOUND,         // condition 4: assigned again after declaration
    RELEASE_NO_ALIASED,         // condition 7: another local names this one's value
    RELEASE_NO_BLOCK_ESCAPE,    // condition 6: stored into something outliving the iteration
    RELEASE_NO_NO_BINDING,      // no binding site found for the name
    RELEASE_NO_UNKNOWN,         // a statement kind this module cannot read
} ReleaseVerdict;

typedef struct ReleaseDecision {
    char*           local_name;  // owned
    ReleaseVerdict  verdict;

    // Does this local's slice own the values stored in it? True only when every
    // value that reached its slots was a fresh temporary by condition 2's
    // table. One borrowed element makes it false, because the release is a
    // single walk of the whole buffer and cannot skip an entry.
    //
    // TWO SOURCES, both gated by that all-or-nothing rule:
    //   STORED   -- a slice literal's elements, or an append's arguments.
    //   ARRIVED  -- the declaration bound the local to a SHIM CALL whose table
    //               row carries returns_owned_elems, meaning the runtime body
    //               allocated every element. `fields := strings.Split(s, ",")`
    //               stores nothing, so before this it owned nothing and its
    //               parts leaked -- 273,982 bytes per 2,000 requests on
    //               bench/daemon/daemon.goo, the largest item left after #274.
    //
    // A Goo callee cannot supply the ARRIVED half. param_escape's
    // return_escapes describes the returned VALUE, not its contents, so there
    // is no summary to read and the answer stays false.
    //
    // Independent of `verdict`. A local that refuses to release still gets a
    // meaningful answer here, and the caller must check BOTH -- there is no
    // element release without a buffer release to hang it on.
    bool            owns_elems;
} ReleaseDecision;

typedef struct ReleasePlanFunction {
    char*             function_name;  // owned
    ReleaseDecision*  decisions;      // owned, one per local of this function
    size_t            count;

    // Key expressions of index assignments that are FRESH TEMPORARIES, so a map
    // written through one may take ownership of the key. Borrowed AST pointers
    // into the same tree codegen walks, never freed here.
    //
    // Condition 2's table, applied to a key instead of a binding. Nothing else
    // ever held the value, so exactly one owner exists and the map may be it.
    ASTNode**         owned_keys;     // owned array, borrowed elements
    size_t            owned_key_count;

    // Operands of a `+` that are FRESH TEMPORARIES, so the concat that consumes
    // one may release it. Borrowed AST pointers, same as owned_keys.
    //
    // Condition 2's table again, applied to an operand. goo_string_concat COPIES
    // both operands into fresh memory, so an operand nothing else names is
    // unreachable the instant the call returns.
    ASTNode**         owned_concat_operands;   // owned array, borrowed elements
    size_t            owned_concat_count;
} ReleasePlanFunction;

typedef struct ReleasePlan {
    ReleasePlanFunction* functions;   // one per user-defined function/method
    size_t               count;
} ReleasePlan;

// Analyze every function reachable from `program`'s top-level declarations.
// Runs param_escape and local_escape internally, so a caller needs neither.
// Pure: does not mutate `program`. Returns NULL only on allocation failure; a
// NULL or malformed `program` yields a valid, empty (count == 0) plan.
ReleasePlan* release_plan_analyze(ASTNode* program);
void         release_plan_free(ReleasePlan* plan);

// True ONLY when all four conditions hold. Conservative on every miss -- an
// unknown function or an unknown local returns false, because the caller frees
// on a true.
bool release_plan_should_release(const ReleasePlan* plan, const char* fn, const char* local);

// The verdict, for a plan dump or a test that asserts the cause. Returns
// RELEASE_NO_NO_BINDING on an unknown function or local.
ReleaseVerdict release_plan_verdict(const ReleasePlan* plan, const char* fn, const char* local);

// Stable, human-readable name for a verdict. Never NULL.
const char* release_verdict_name(ReleaseVerdict v);

// May a map take ownership of the key written by this index assignment?
//
// `key_expr` is the INDEX node of an assignment target — the `k` of `m[k] = v`.
// True only when that expression is a fresh temporary by condition 2's table,
// so no other name ever held it and the map can be its single owner.
//
// HALF THE GUARD, exactly like condition 2 itself. This module holds no type
// information, so it cannot tell a map write from a slice write, and it cannot
// tell a pointer key from an inline one. The caller must confirm BOTH before it
// emits goo_map_set_sv_owning. Codegen has the type and does that.
//
// Conservative on every miss: an unknown function or an unrecorded node returns
// false, and false means the key stays borrowed, which is what every program
// did before key ownership existed.
bool release_plan_key_is_owned(const ReleasePlan* plan, const char* fn,
                               const ASTNode* key_expr);

// Does this local's slice own its ELEMENTS, so releasing the buffer should
// release each of them first?
//
// True only when the local has stored elements and every one was a fresh
// temporary. HALF THE GUARD, as always: this module holds no types, so the
// caller must confirm the local really is a slice and that its element shape
// puts a releasable pointer at offset 0. Codegen does that.
//
// Conservative on every miss. False means the elements stay, which is what
// every program did before this existed.
bool release_plan_slice_owns_elems(const ReleasePlan* plan, const char* fn,
                                   const char* local);

// May the concat that consumes this operand release it?
//
// `operand` is one side of a `+` node. True only when that expression is a fresh
// temporary by condition 2's table, so no other name ever held it and this
// concat is its single consumer. `goo_string_concat` copies both operands into
// fresh memory, so the result aliases neither and the release is safe the
// instant the call returns.
//
// HALF THE GUARD, as always: this module holds no type information and cannot
// tell a string `+` from an integer one. Codegen has the type and asks only at a
// string-concat site.
//
// Conservative on every miss. False leaves the operand borrowed, which is what
// every program did before this existed.
// Is `rhs` the shape `target = append(target, ...)`?
//
// FOR CODEGEN'S RELEASE-BEFORE-STORE, which must NOT fire here. A self-append
// does not rebind the local to a different object -- it GROWS the existing one,
// and goo_slice_append's realloc frees the old base ITSELF. A release emitted at
// such a store reads a pointer the RHS already freed.
//
// Exported rather than reimplemented: this is the same predicate the rebind rule
// uses, and the two must agree or one of them frees live memory.
bool release_decision_is_self_append(const char* target, ASTNode* rhs);

bool release_plan_concat_operand_is_owned(const ReleasePlan* plan, const char* fn,
                                          const ASTNode* operand);

#endif // RELEASE_DECISION_H
