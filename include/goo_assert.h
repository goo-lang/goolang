#ifndef GOO_ASSERT_H
#define GOO_ASSERT_H

// Executable comments.
//
// WHY THIS EXISTS. A word-boundary grep on 2026-08-08 found ZERO runtime
// assert() calls in 134,608 lines of src/. The compiler states its invariants
// in prose — include/escape_core.h and include/release_decision.h are dense
// with them — and nothing checks that the code still agrees with the prose. A
// comment and the code beside it can disagree silently. An assert that survives
// a test run cannot.
//
// SQLite ships over 7,500 asserts in a quarter of this line count, at a
// measured cost there of roughly four times slower, which is why SQLite
// keeps them behind an opt-in flag. This header kept that same opt-in
// default until 2026-09-02, when a serial, low-noise re-measurement (Task
// 24, docs/misra/README.md) found no cost here: median wall time on the
// 495-fixture golden corpus was very slightly LOWER with asserts on (27.90 s)
// than off (27.96 s), well inside the run-to-run range either side. With the
// cost gone, Holzmann's Power of Ten rules (rule 5: an assertion belongs in
// the code that flies, not only in the debug build that tested it) settle
// the default below: ON.
//
// THE FOUR BUILDS. The same construct behaves four ways on purpose:
//
//   construct        coverage        debug        default       GOO_ASSERT_OFF
//                    (GOO_COVERAGE)  (GOO_DEBUG)   (no flag)
//   GOO_ASSERT(x)    no-op           abort if      abort if      no-op
//                                    false         false
//   GOO_NEVER(x)     constant 0      abort if      abort if      (x)
//                                    true          true
//   GOO_ALWAYS(x)    constant 1      abort if      abort if      (x)
//                                    false         false
//
// GOO_ASSERT_OFF is the KILL SWITCH, not the default. Define it to recover
// the pre-2026-09-02 contract (no-op / pass-through) -- for example to bisect
// whether an assert itself, rather than the bug it caught, explains a
// regression. GOO_DEBUG still forces the checking arm on regardless of
// GOO_ASSERT_OFF, exactly as before.
//
// The coverage row is the load-bearing one, and it is the part that is easy to
// get wrong. GOO_NEVER and GOO_ALWAYS mark a DEFENSIVE branch — one the author
// believes is unreachable, kept as a last line of defence. Such a branch pins
// the coverage number below 100% forever, because no test can reach it. In the
// coverage build they collapse to a constant, the branch disappears, and the
// denominator stops counting something no test could ever cover. Under
// GOO_ASSERT_OFF the condition is passed through unchanged, so a wrong belief
// degrades into a handled case instead of undefined behaviour -- this is the
// pre-2026-09-02 default, kept alive only as the kill switch.
//
// The rule that goes with them: do NOT delete a defensive branch to make the
// coverage number green. Mark it and keep it.
//
// NAMING. Hipp's macros are the bare NEVER and ALWAYS. These carry a GOO_
// prefix because this header is force-included into EVERY translation unit
// (see -include in the Makefile), next to the LLVM 22 headers. A header that
// cannot be un-included should not claim two names that generic. The 8 existing
// occurrences of NEVER/ALWAYS in src/ are all inside comments, so the bare
// names were free on 2026-08-08 — the prefix is about the next header, not
// this one.
//
// TRADE-OFF, STATED PLAINLY. This is four builds of one source, which Go's
// design explicitly rejects and which SQLite's author defends. `make
// verify-core` runs the DEFAULT build, so the asserts DO gate it now: a
// crash there is a genuine invariant firing on real input, not a decoration,
// and the fix is to read the assertion's file and line, never to weaken or
// remove it. GOO_ASSERT_OFF exists for the rare case that genuinely needs
// the pre-2026-09-02 behaviour; it is a kill switch, not a knob any gate
// should turn by default. The alternative — keeping one build — gives up an
// honest coverage number on defensive branches, which is the thing item 1 of
// this arc just spent the effort to obtain.

#include <stdio.h>
#include <stdlib.h>

// Not static inline: taking its address in the macro below in every TU would
// otherwise emit an unused-function warning in the TUs that never assert.
// __attribute__((noreturn)) lets the optimiser see that the false arm ends the
// program, so an assert costs nothing on the taken path.
__attribute__((noreturn, unused))
static void goo_assert_fail(const char* kind, const char* expr,
                            const char* file, int line, const char* fn) {
    fprintf(stderr, "goo: %s FAILED: %s\n  at %s:%d in %s()\n",
            kind, expr, file, line, fn);
    fflush(stderr);
    abort();
}

#if defined(GOO_COVERAGE)
   // Coverage carve-out, unchanged by the 2026-09-02 flip: an assert that
   // never fails in a test run must not register as an uncovered branch, so
   // it stays a plain no-op here -- see the GOO_NEVER/GOO_ALWAYS coverage arm
   // below for the same reasoning against the same denominator.
#  define GOO_ASSERT(x) ((void)0)
#elif defined(GOO_DEBUG) || !defined(GOO_ASSERT_OFF)
#  define GOO_ASSERT(x) \
     ((x) ? (void)0 : goo_assert_fail("ASSERT", #x, __FILE__, __LINE__, __func__))
#else
   // GOO_ASSERT_OFF only: the kill switch, not the default. (void)0 rather
   // than empty, so `if (c) GOO_ASSERT(x); else ...` still parses.
#  define GOO_ASSERT(x) ((void)0)
#endif

#if defined(GOO_COVERAGE)
   // The branch vanishes, so it stops padding the denominator. `(void)sizeof`
   // keeps the expression type-checked, so a GOO_NEVER whose argument stopped
   // compiling cannot hide in a coverage-only build.
#  define GOO_NEVER(x)  ((void)sizeof(x), 0)
#  define GOO_ALWAYS(x) ((void)sizeof(x), 1)
#elif defined(GOO_DEBUG) || !defined(GOO_ASSERT_OFF)
#  define GOO_NEVER(x) \
     ((x) ? (goo_assert_fail("NEVER", #x, __FILE__, __LINE__, __func__), 1) : 0)
#  define GOO_ALWAYS(x) \
     ((x) ? 1 : (goo_assert_fail("ALWAYS", #x, __FILE__, __LINE__, __func__), 0))
#else
   // GOO_ASSERT_OFF only: the defensive branch stays, and it is the last
   // line of defence -- the pre-2026-09-02 default, kept as the kill switch.
#  define GOO_NEVER(x)  (x)
#  define GOO_ALWAYS(x) (x)
#endif

// ---------------------------------------------------------------------------
// GOO_TESTCASE — a boundary marker.
//
// `GOO_TESTCASE(x)` is a CLAIM by the author: this boundary matters, and some
// test must drive `x` both true and false. SQLite carries 1,184 of them, and
// that is the mechanism by which it HOLDS 100% MC/DC rather than merely
// measuring it (https://sqlite.org/testing.html).
//
// WHY THIS IS GATED WHEN COVERAGE IS NOT. scripts/coverage_corpus.sh refuses
// to be a gate on purpose, and its reason is sound: "a coverage target invites
// tests that raise the number instead of tests that find bugs". That is true
// of a PERCENTAGE. It is false of a MARKER. A percentage can be inflated by
// shallow tests; "this specific boundary was driven both ways" cannot be --
// either a test reached it or none did.
//
// In a coverage build the marker is a real branch, so gcov records each
// direction; scripts/testcase_report.sh reads those counters back. The write
// to a volatile is what stops the optimiser folding the branch away at any
// -O level, which would erase the very record being asked for.
//
// Everywhere else it expands to `(void)sizeof(x)`: no code, but the expression
// stays TYPE-CHECKED, so a marker whose expression rotted cannot hide in the
// build where it does nothing.
#if defined(GOO_COVERAGE)
static volatile int goo_testcase_sink;
static inline void goo_testcase_hit(const char* file, int line) {
    (void)file;
    goo_testcase_sink = line;
}
#  define GOO_TESTCASE(x) ((void)((x) ? (goo_testcase_hit(__FILE__, __LINE__), 1) : 0))
#else
#  define GOO_TESTCASE(x) ((void)sizeof(x))
#endif

#endif // GOO_ASSERT_H
