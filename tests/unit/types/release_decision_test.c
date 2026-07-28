// T4: the release decision — table-driven unit test.
//
// This is the FIRST decision in the ARC leg that a consumer acts on by FREEING
// memory, so the soundness invariant runs the OPPOSITE way from its three escape
// siblings: there, `escapes = true` is the safe answer; here, `release = false`
// is. A wrong `true` frees live memory.
//
// EVERY ROW ASSERTS THE VERDICT, NOT JUST THE BOOLEAN. A row that refuses a
// local for the wrong reason would still pass a boolean check, and this arc has
// now been bitten twice by a check that passed for an unrelated cause (a
// function extractor that matched forward declarations, and a local_escape run
// whose imports had not resolved). Asserting the cause is what stops it.
//
// IMPORT-FREE ON PURPOSE. `.handoff.md` records a local_escape table that looked
// confident and was conservative for an unrelated reason: the imports had not
// resolved, and the tell was `i`, a plain int loop counter, reading as escaping.
// The shim half of condition 2 therefore cannot be covered here — it needs a
// resolved `strings` import — and is covered by an integration probe instead.

#include "parser.h"
#include "ast.h"
#include "types.h"
#include "release_decision.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_EXPECT_LOCALS 4

typedef struct {
    const char*    local;
    ReleaseVerdict verdict;
} LocalExpectation;

typedef struct {
    int              row;
    const char*      description;
    const char*      src;
    const char*      fn;
    LocalExpectation expect[MAX_EXPECT_LOCALS];
    int              expect_count;
} TestRow;

// A callee that returns a VIEW of its argument, so its ParamEscapeSummary
// carries return_escapes = true. This is the TrimPrefix shape that
// include/local_escape.h names as the hole, written without an import.
#define BORROW_HELPER \
    "func borrowView(s string) string {\n" \
    "    return s[1:]\n" \
    "}\n"

// A callee that returns a FRESH allocation: return_escapes = false.
#define OWNED_HELPER \
    "func makeOwned() *int {\n" \
    "    return new(int)\n" \
    "}\n"

static TestRow rows[] = {
    // ---------------- RELEASE: all four conditions hold ----------------
    {
        1, "new(int) at function scope, dies here -> RELEASE",
        "package main\n"
        "func f() {\n"
        "    a := new(int)\n"
        "    _ = a\n"
        "}\n",
        "f", { { "a", RELEASE_OK } }, 1
    },
    {
        2, "call to a Goo function with return_escapes false -> RELEASE",
        "package main\n"
        OWNED_HELPER
        "func f() {\n"
        "    a := makeOwned()\n"
        "    _ = a\n"
        "}\n",
        "f", { { "a", RELEASE_OK } }, 1
    },
    {
        3, "a composite literal is a fresh allocation -> RELEASE",
        "package main\n"
        "type T struct { x int }\n"
        "func f() {\n"
        "    p := &T{x: 1}\n"
        "    _ = p\n"
        "}\n",
        "f", { { "p", RELEASE_OK } }, 1
    },

    // ---------------- CONDITION 1: escapes ----------------
    {
        4, "returned -> refuse, ESCAPES",
        "package main\n"
        "func f() *int {\n"
        "    a := new(int)\n"
        "    return a\n"
        "}\n",
        "f", { { "a", RELEASE_NO_ESCAPES } }, 1
    },
    {
        5, "stored to a global -> refuse, ESCAPES",
        "package main\n"
        "var g *int\n"
        "func f() {\n"
        "    a := new(int)\n"
        "    g = a\n"
        "}\n",
        "f", { { "a", RELEASE_NO_ESCAPES } }, 1
    },

    // ---------------- CONDITION 2: not owned ----------------
    //
    // THE ROW THAT MATTERS MOST. `b` does not outlive f, so condition 1 passes
    // and local_escape alone would say "release". borrowView returns `s[1:]`,
    // a view into the CALLER's buffer, so a release frees the caller's string.
    {
        6, "bound to a callee that returns a VIEW of its arg -> refuse, NOT_OWNED",
        "package main\n"
        BORROW_HELPER
        "func f(s string) {\n"
        "    b := borrowView(s)\n"
        "    _ = b\n"
        "}\n",
        "f", { { "b", RELEASE_NO_NOT_OWNED } }, 1
    },
    {
        7, "bound to a slice expression directly -> refuse, NOT_OWNED",
        "package main\n"
        "func f(s string) {\n"
        "    c := s[1:]\n"
        "    _ = c\n"
        "}\n",
        "f", { { "c", RELEASE_NO_NOT_OWNED } }, 1
    },
    {
        8, "an ALIAS of another local -> refuse, NOT_OWNED (one owner only)",
        "package main\n"
        "func f() {\n"
        "    a := new(int)\n"
        "    d := a\n"
        "    _ = d\n"
        "}\n",
        // `a` still releases: it is the owner. `d` must not, or the buffer is
        // freed twice.
        "f", { { "a", RELEASE_OK }, { "d", RELEASE_NO_NOT_OWNED } }, 2
    },
    {
        9, "bound to an INDEX read -> refuse, NOT_OWNED (aliases the container)",
        "package main\n"
        "func f(xs []*int) {\n"
        "    e := xs[0]\n"
        "    _ = e\n"
        "}\n",
        "f", { { "e", RELEASE_NO_NOT_OWNED } }, 1
    },
    {
        10, "bound to a SELECTOR -> refuse, NOT_OWNED (a field of something else)",
        "package main\n"
        "type T struct { p *int }\n"
        "func f(t *T) {\n"
        "    q := t.p\n"
        "    _ = q\n"
        "}\n",
        "f", { { "q", RELEASE_NO_NOT_OWNED } }, 1
    },

    // ---------------- CONDITION 3: arena-routed ----------------
    //
    // An arena pointer has NO object header, so goo_release would compute
    // `ptr - GOO_OBJ_HEADER_SIZE` on an interior pointer and free() it.
    // local_escape's boundary is the FUNCTION, so `z` reads as non-escaping and
    // nothing else refuses it.
    {
        11, "declared inside `arena { }` -> refuse, ARENA",
        "package main\n"
        "func f() {\n"
        "    arena {\n"
        "        z := new(int)\n"
        "        _ = z\n"
        "    }\n"
        "}\n",
        "f", { { "z", RELEASE_NO_ARENA } }, 1
    },

    // ---------------- CONDITION 4: loop scope ----------------
    //
    // THE ROW THAT RETARGETED T4. `inner` is rebound on every iteration, so a release
    // at function exit frees one of N. `outer` is bound once and does release,
    // which is the contrast that proves the rule is about SCOPE and not about
    // the presence of a loop in the function.
    {
        12, "declared inside a loop -> refuse, LOOP_SCOPE; outer still releases",
        "package main\n"
        "func f(n int) {\n"
        "    outer := new(int)\n"
        "    for i := 0; i < n; i++ {\n"
        "        inner := new(int)\n"
        "        _ = inner\n"
        "    }\n"
        "    _ = outer\n"
        "}\n",
        "f", { { "outer", RELEASE_OK }, { "inner", RELEASE_NO_LOOP_SCOPE } }, 2
    },
    {
        13, "the daemon's shape: err bound inside the loop -> refuse, LOOP_SCOPE",
        "package main\n"
        OWNED_HELPER
        "func f(n int) {\n"
        "    for i := 0; i < n; i++ {\n"
        "        err := makeOwned()\n"
        "        _ = err\n"
        "    }\n"
        "}\n",
        "f", { { "err", RELEASE_NO_LOOP_SCOPE } }, 1
    },

    {
        // CONDITION 4, the re-assignment half, and it is a SOUNDNESS row rather
        // than a precision one. The DECLARATION site is a clean allocation, so
        // condition 2 reads `a` as owned. The later `a = t.p` leaves it holding a
        // field of someone else's struct, and a release at exit would free that.
        // Only counting bindings catches this.
        //
        // Added because scripts/release_decision_teeth.sh reported this condition
        // UNGUARDED: deleting it from decide() left all 15 original rows green.
        16, "declared owned, then RE-ASSIGNED to a borrowed value -> refuse, REBOUND",
        "package main\n"
        "type T struct { p *int }\n"
        "func f(t *T) {\n"
        "    a := new(int)\n"
        "    a = t.p\n"
        "    _ = a\n"
        "}\n",
        "f", { { "a", RELEASE_NO_REBOUND } }, 1
    },

    // ---------------- conservative defaults ----------------
    {
        14, "a scalar literal owns nothing to release -> refuse, NOT_OWNED",
        "package main\n"
        "func f() {\n"
        "    k := 1\n"
        "    _ = k\n"
        "}\n",
        "f", { { "k", RELEASE_NO_NOT_OWNED } }, 1
    },
    {
        15, "an UNRESOLVED callee -> refuse (conservative), NOT_OWNED",
        "package main\n"
        "func f() {\n"
        "    u := unknownExternal()\n"
        "    _ = u\n"
        "}\n",
        "f", { { "u", RELEASE_NO_NOT_OWNED } }, 1
    },
    {
        // THE UNREADABLE-FUNCTION REFUSAL, and it pins a real PRECISION CLIFF
        // rather than hiding it. The walk in release_decision.c does not read
        // `switch`, `type switch`, `select` or `if let`, so any function
        // containing one is refused ENTIRELY -- `a` here is an obvious release
        // candidate and still gets refused.
        //
        // That is deliberate for this first cut and it is the SAFE direction: an
        // unread statement might assign to a local, and a missed assignment would
        // let a rebound local be released. Refusing the function is sound;
        // guessing is not.
        //
        // It is also a narrow limitation to lift -- those arms only need
        // recursion into their bodies -- and lifting it is a change with its own
        // rows, not a silent widening of this one. Recorded in the ledger.
        //
        // Added because scripts/release_decision_teeth.sh reported this condition
        // UNGUARDED: deleting it from decide() left all 15 original rows green.
        17, "a function containing a SWITCH is refused entirely -> UNKNOWN",
        "package main\n"
        "func f(n int) {\n"
        "    a := new(int)\n"
        "    switch n {\n"
        "    case 1:\n"
        "        _ = a\n"
        "    }\n"
        "}\n",
        "f", { { "a", RELEASE_NO_UNKNOWN } }, 1
    },
};

static int failures = 0;
static int checks = 0;

int main(void) {
    printf("Running T4 release-decision tests...\n");
    size_t nrows = sizeof(rows) / sizeof(rows[0]);

    for (size_t i = 0; i < nrows; i++) {
        TestRow* row = &rows[i];
        printf("\n=== Row %d: %s ===\n", row->row, row->description);

        if (parse_input(row->src, "row.goo") != 0 || !ast_root) {
            printf("  FAIL: parse failed\n");
            failures++;
            continue;
        }
        TypeChecker* checker = type_checker_new();
        if (checker) type_check_program(checker, ast_root);  // rc ignored, as the siblings do

        ReleasePlan* plan = release_plan_analyze(ast_root);
        if (!plan) {
            printf("  FAIL: release_plan_analyze returned NULL\n");
            failures++;
            if (checker) type_checker_free(checker);
            ast_node_free(ast_root);
            ast_root = NULL;
            continue;
        }

        int row_failed = 0;
        for (int j = 0; j < row->expect_count; j++) {
            ReleaseVerdict got = release_plan_verdict(plan, row->fn, row->expect[j].local);
            ReleaseVerdict want = row->expect[j].verdict;
            checks++;
            if (got != want) {
                printf("  FAIL: local '%s' verdict=%s, expected %s\n",
                       row->expect[j].local,
                       release_verdict_name(got), release_verdict_name(want));
                failures++;
                row_failed = 1;
            }
            // The boolean and the verdict must never disagree, or a caller and a
            // test could read different answers from the same plan.
            bool should = release_plan_should_release(plan, row->fn, row->expect[j].local);
            checks++;
            if (should != (want == RELEASE_OK)) {
                printf("  FAIL: local '%s' should_release=%d disagrees with verdict %s\n",
                       row->expect[j].local, (int)should, release_verdict_name(want));
                failures++;
                row_failed = 1;
            }
        }

        // A miss must be conservative, checked once per row rather than assumed.
        checks++;
        if (release_plan_should_release(plan, "__no_such_function__", "x")) {
            printf("  FAIL: unknown function returned should_release=true\n");
            failures++;
            row_failed = 1;
        }

        printf("  Row %d: %s\n", row->row, row_failed ? "FAIL" : "PASS");

        release_plan_free(plan);
        if (checker) type_checker_free(checker);
        ast_node_free(ast_root);
        ast_root = NULL;
    }

    printf("\n=================================================\n");
    printf("release_decision_test summary: %d assertions passed, %d failed\n",
           checks - failures, failures);
    return failures ? 1 : 0;
}
