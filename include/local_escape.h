#ifndef LOCAL_ESCAPE_H
#define LOCAL_ESCAPE_H

// ARC leg: per-LOCAL escape summaries.
//
// THIRD SOUNDNESS SIBLING of include/param_escape.h and
// include/block_escape.h. All three run the same taint-propagation engine
// with the source and the boundary moved:
//
//   module          source                        boundary
//   --------------  ----------------------------  ----------------
//   param_escape    a function parameter          the function
//   block_escape    an alloc site in `arena {}`    the arena block
//   local_escape    a LOCAL VARIABLE               the function
//
// A soundness fix to that shared shape in ANY of the three MUST be mirrored
// in the other two. Both existing headers say a THIRD consumer justifies
// extracting a shared core — this module is that third consumer, and the
// extraction is owed. It is deliberately NOT done in the same change that
// introduces this module: refactoring the machinery that prevents
// use-after-free is safer with three green row-tables as the net than with
// two. See .handoff.md's ledger.
//
// Why this module exists, when the other two already run: NOTHING today can
// say when an ordinary local dies. block_escape only classifies allocation
// sites lexically inside an `arena {}` block, so outside an arena no value
// has a known lifetime, and ARC therefore cannot release anything. Measured
// on bench/daemon: its 733.9 MB at 400k requests lives in runtime-helper
// allocations bound to LOCALS (`fields := strings.Split(...)`,
// `f := strings.TrimSpace(...)`, `parts` grown by append), and the whole
// program contains exactly ONE codegen-emitted allocation site. Releasing
// codegen sites would therefore move that number by nothing; releasing
// locals is what moves it.
//
// Pure static analysis: for every user-defined function F, computes one
// boolean per LOCAL VARIABLE name declared anywhere in F's body:
//
//     escapes[i] == true  =>  the value held by local i MAY outlive F
//
// The boundary is the FUNCTION, not the enclosing block. A local that
// outlives an inner block but dies with the function is reported as NOT
// escaping, which is exactly what a release-at-function-exit consumer needs,
// and is why `handle` in bench/daemon is the shape this targets: it is
// called once per request and everything it builds dies at its return.
//
// SOUNDNESS INVARIANT, identical to both siblings and asymmetric: `true` is
// always a safe answer (worst case: keep leaking it, as today). `false`
// asserts "provably does not outlive F", and a consumer will free on it —
// so every construct this module does not precisely understand must default
// to `true`. Under-marking is the ONLY bug class that can dangle a pointer.
//
// Explicitly NOT this module's job: no codegen change, no release emission,
// no ownership decision about which locals actually HOLD an owned reference
// (a local bound to a borrowed substring is not this module's problem — see
// the aliasing note below). This module does not mutate the AST.
//
// ALIASING IS NOT ESCAPE, and a consumer must not confuse them. `TrimPrefix`
// returns `s[len(prefix):]`, a view into its argument's buffer
// (goostd/strings/strings.go). A local bound to that value does not own it,
// and releasing it would free the caller's string. This module answers
// "does the value outlive F", NOT "does the local own the value". A release
// consumer needs BOTH.

#include "ast.h"
#include "escape_core.h"
#include "param_escape.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct LocalEscapeSummary {
    char*  function_name;  // owned
    char** local_names;    // owned array of owned strings, length local_count
    // Length local_count. Non-zero = may outlive F, and the SET says why.
    // ADR 0005: this was a bool array. `escapes` is now `reasons != 0`, which
    // is what local_escape_local_escapes returns, so a consumer that only
    // wants the boolean is unchanged.
    EscapeReasons* reasons;
    size_t local_count;
} LocalEscapeSummary;

typedef struct LocalEscapeResult {
    LocalEscapeSummary* summaries;  // one per user-defined function/method
    size_t count;
} LocalEscapeResult;

// Analyze every AST_FUNC_DECL reachable from `program`'s top-level
// declarations (methods included). Pure: does not mutate `program`. Returns
// NULL ONLY on allocation failure; a NULL/malformed `program` yields a
// valid, empty (count == 0) result.
//
// `summaries` (from param_escape_analyze) answers "does this callee retain
// that argument". It may be NULL, and every call is then treated as
// external and retaining — still SOUND, just maximally conservative, which
// costs every local passed to any function. Pass real summaries or ARC
// reclaims almost nothing.
LocalEscapeResult* local_escape_analyze(ASTNode* program,
                                        const ParamEscapeResult* summaries);
void local_escape_result_free(LocalEscapeResult* result);

// Lookup. Conservative on a miss, per the soundness contract: an unknown
// function name, or an unknown local name, must never be treated as
// "does not escape" by any consumer, so both report true.
bool local_escape_local_escapes(const LocalEscapeResult* result,
                                const char* fn, const char* local);

// The same lookup, answering WHY rather than WHETHER. `escapes` is
// `local_escape_local_reasons(...) != ESCAPE_REASON_NONE`, and the boolean
// lookup above is now written that way.
//
// CONSERVATIVE ON A MISS MEANS ESCAPE_REASON_ALL, NOT ZERO. The boolean form
// fails closed by returning `true`, and the only value that fails closed the
// same way here is the FULL set: a consumer asks "does this escape ONLY via
// X", and every wrong answer must make that test fail. Returning zero would
// read as "escapes for no reason", which is both self-contradictory and the
// one answer that frees live memory.
//
// A REASON IS A SOUND OVER-APPROXIMATION, EXACTLY LIKE THE BIT IT REPLACED.
// The set may name a cause that a more precise engine would drop, and a
// consumer must treat an EXTRA reason as a refusal rather than a defect. The
// measured example is the daemon: `counts[f] = counts[f] + 1` gives `f` both
// SUBSCRIPT_STORE and CONTAINER_STORE, because reading `counts[f]` carries
// f's taint into the right-hand side. Nothing about f's buffer reaches the
// map's value slot.
EscapeReasons local_escape_local_reasons(const LocalEscapeResult* result,
                                         const char* fn, const char* local);

#endif // LOCAL_ESCAPE_H
