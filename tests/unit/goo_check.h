#ifndef GOO_CHECK_H
#define GOO_CHECK_H

// The shared assertion and verdict surface for the C unit suites.
//
// Six suites hand-rolled this before it existed, and they had already drifted:
// four separate copies of check(), two counter conventions (g_pass/g_fail in
// param_escape, block_escape and arena_routing, checks/failures in obj_header,
// local_escape and release_decision), two indent widths, and one suite that
// printed a PASS line per check while the other five stayed quiet. Nothing was
// wrong with any single copy. The cost was that no reader and no script could
// tell one suite's verdict from another's.
//
// WHAT THIS DELIBERATELY DOES NOT DO. tests/framework/test_framework.c is a
// full xUnit clone in this same repo -- TEST()/TEST_F(), constructor-based
// self-registration, fixtures, JUnit output -- and it is unused on purpose.
// Auto-discovery finds tests you never listed, which is the property that let
// 77 tests pass on 2026-08-17 against frameworks that never linked into
// bin/goo. Every suite here keeps its own main() and its own readable table.
// This header owns the counting and the exit status, and nothing else.
//
// THREE OUTCOMES, and the third is why this file exists:
//
//   PASS   0  every check passed, and the row count matches the declaration
//   FAIL   1  a check failed
//   BROKEN 2  the suite and its declared row count disagree
//
// A table whose rows stop executing prints nothing and exits 0, which reads
// exactly like success. That is not hypothetical in this tree: ast_free_leak
// _test.c hand-rolled its own "parsed %d of %d" check and its own exit 2 for
// this reason, and scripts/safety-baseline.txt reached 139 dead entries out of
// 218 before anyone noticed. goo_check_expect() is the general form. It is
// MANDATORY -- a suite that never declares a count is BROKEN, because opting
// out would otherwise cost one omitted line and be invisible in review.
//
// Gated by scripts/goo_check_probe.sh in make verify-core: it asserts this
// contract against fixtures, and its --self-test mutates this file one line at
// a time to prove each rule above can still go red. No count is quoted here on
// purpose -- a number in a comment is the thing goo_check_expect() exists to
// stop trusting.

#include <stdbool.h>
#include <stdio.h>

static int goo_check_checks = 0;
static int goo_check_failures = 0;
static int goo_check_rows = 0;
static int goo_check_rows_expected = -1;

// Declare how many rows the suite runs. Call it once, before the first row.
// The number is the table length -- sizeof(rows)/sizeof(rows[0]) for a
// table-driven suite, or the literal count of goo_check_row() calls for one
// that writes its rows out inline.
static inline void goo_check_expect(int rows) {
    goo_check_rows_expected = rows;
}

// Announce a row. Prints unconditionally: the row headers are the map a
// reader needs to place a FAIL line, and they are what makes a stopped table
// visible in the log as well as in the exit status.
static inline void goo_check_row(int number, const char* description) {
    goo_check_rows++;
    printf("=== Row %d: %s ===\n", number, description);
}

// One assertion. Silent when it holds -- 213 gates share one stdout, and a
// PASS line per check buries the one line that matters. `label` should carry
// the observed value, not just the expectation, because this line is often
// the only evidence a reader gets.
static inline void goo_check(bool condition, const char* label) {
    goo_check_checks++;
    if (!condition) {
        goo_check_failures++;
        printf("  FAIL: %s\n", label);
    }
}

// Has anything failed so far? For the one legitimate reason to stop early:
// a suite whose later rows are only meaningful if the fixture has the shape
// they assume. Running them against a fixture that already failed produces
// noise, not evidence. The abandoned rows then make the row count disagree
// with the declaration, so the suite reports BROKEN rather than FAIL -- which
// is the honest verdict, because a bad fixture says nothing about the code.
static inline bool goo_check_failed(void) {
    return goo_check_failures > 0;
}

// The verdict. Return it from main() directly.
//
// `artifact` names what was tested, not what ran the test. A PASS about a
// different artifact than the one you edited reads identically to one about
// the right artifact, so the name belongs in the line itself.
//
// BROKEN outranks FAIL on purpose. If the table did not run as declared, the
// failure count is not trustworthy evidence about the code under test, and
// reporting it as an ordinary failure would send a reader to debug the
// compiler when the instrument is what moved.
static inline int goo_check_done(const char* artifact) {
    if (goo_check_rows_expected < 0) {
        printf("\n%s: BROKEN -- the suite never declared a row count.\n", artifact);
        printf("  Call goo_check_expect(n) before the first row. Without it\n");
        printf("  a table that stops early still exits 0.\n");
        return 2;
    }
    if (goo_check_rows != goo_check_rows_expected) {
        printf("\n%s: BROKEN -- %d of %d rows ran.\n",
               artifact, goo_check_rows, goo_check_rows_expected);
        printf("  A green result here would mean the table stopped or the\n");
        printf("  declaration went stale, not that the code is correct.\n");
        return 2;
    }
    if (goo_check_failures > 0) {
        printf("\n%s: FAIL (%d of %d checks, %d rows)\n",
               artifact, goo_check_failures, goo_check_checks, goo_check_rows);
        return 1;
    }
    printf("\n%s: PASS (%d checks, %d rows)\n",
           artifact, goo_check_checks, goo_check_rows);
    return 0;
}

#endif /* GOO_CHECK_H */
