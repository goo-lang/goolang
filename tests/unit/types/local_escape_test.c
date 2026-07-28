// ARC leg: per-LOCAL escape summaries — table-driven unit test.
//
// Same shape as param_escape_test.c and block_escape_test.c: a Goo source
// string -> parse_input -> type_check_program (ignore rc; needed to populate
// FuncLitNode.captured_names the way the real pipeline does, per the
// closure-capture row) -> local_escape_analyze -> assert one boolean per
// named local.
//
// The SOUNDNESS rows are the load-bearing ones: an unsound implementation
// shows up as a wrong `false` there, and a consumer will free on a false.
// The PRECISION rows are what stop a lazy mark-everything implementation
// from passing — without them, `return true;` satisfies the whole table and
// ARC reclaims nothing.

#include "parser.h"
#include "ast.h"
#include "types.h"
#include "local_escape.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_EXPECT_LOCALS 4

typedef struct {
    const char* name;
    bool        expected_escapes;
} LocalExpectation;

typedef struct {
    int              row;
    const char*      description;
    const char*      src;
    const char*      fn;                          // function whose locals we assert
    LocalExpectation expect[MAX_EXPECT_LOCALS];
    int              expect_count;
} TestRow;

static TestRow rows[] = {
    // ---------------- PRECISION: must be false ----------------
    {
        1, "local never used past its own scope -> false",
        "package main\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    _ = x\n"
        "}\n",
        "f", { { "x", false } }, 1
    },
    {
        2, "local passed to a NON-retaining callee -> false",
        "package main\n"
        "func sink(p *int) {\n"
        "}\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    sink(x)\n"
        "}\n",
        "f", { { "x", false } }, 1
    },
    {
        3, "two locals, neither leaves the function -> false, false",
        "package main\n"
        "func f() {\n"
        "    a := new(int)\n"
        "    b := a\n"
        "    _ = b\n"
        "}\n",
        "f", { { "a", false }, { "b", false } }, 2
    },
    {
        4, "local declared inside a loop, dies each iteration -> false",
        "package main\n"
        "func f() {\n"
        "    for i := 0; i < 3; i++ {\n"
        "        t := new(int)\n"
        "        _ = t\n"
        "    }\n"
        "}\n",
        "f", { { "t", false } }, 1
    },

    // ---------------- SOUNDNESS: must be true ----------------
    {
        5, "local returned -> true",
        "package main\n"
        "func f() *int {\n"
        "    x := new(int)\n"
        "    return x\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        6, "local stored into a package global -> true",
        "package main\n"
        "var g *int\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    g = x\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        7, "local passed to a RETAINING callee -> true",
        "package main\n"
        "var g *int\n"
        "func stash(p *int) {\n"
        "    g = p\n"
        "}\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    stash(x)\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        8, "local captured by a closure -> true",
        "package main\n"
        "var h func()\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    h = func() { _ = x }\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        9, "local passed as a goroutine argument -> true",
        "package main\n"
        "func work(p *int) {\n"
        "}\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    go work(x)\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        10, "local used as a METHOD-CALL RECEIVER -> true (unresolved callee)",
        // The receiver rule fixed in PR #248: a selector callee resolves to
        // no summary, and a receiver is not a member of call->args, so the
        // retain-all rule for an unresolved callee never covered it. This
        // module must not reintroduce that hole.
        "package main\n"
        "type T struct { x int }\n"
        "var g *T\n"
        "func (t *T) stash() {\n"
        "    g = t\n"
        "}\n"
        "func f() {\n"
        "    p := &T{x: 1}\n"
        "    p.stash()\n"
        "}\n",
        "f", { { "p", true } }, 1
    },
    {
        11, "transitive: local copied into another that is returned -> true, true",
        "package main\n"
        "func f() *int {\n"
        "    a := new(int)\n"
        "    b := a\n"
        "    return b\n"
        "}\n",
        "f", { { "a", true }, { "b", true } }, 2
    },
    {
        12, "local passed to an EXTERNAL/unregistered callee -> true",
        "package main\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    unknownExternal(x)\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        13, "local stored through a pointer deref -> true",
        "package main\n"
        "func f(out **int) {\n"
        "    x := new(int)\n"
        "    *out = x\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        14, "local sent on a channel -> true",
        "package main\n"
        "func f(ch chan *int) {\n"
        "    x := new(int)\n"
        "    ch <- x\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        // SOUNDNESS. A map KEY is a stored reference, and nothing used to say
        // so: assign_to_lvalue marks only the RIGHT-hand taint, so the INDEX
        // expression of a non-identifier lvalue was never tainted.
        //
        // Measured before the fix: `m` ESCAPES and `k` did NOT. But
        // goo_map_set_sv (src/runtime/runtime.c) stores the key pointer
        // VERBATIM and never frees it, so the returned map genuinely holds
        // `k`. An ARC release consumer acting on that verdict frees a buffer
        // a live, returned map still points at.
        //
        // Found by docs/superpowers/specs/
        // 2026-07-28-daemon-alloc-attribution-findings.md. The slice
        // equivalent (row 16) was already sound, because append is an
        // ordinary call and the call sink covers it.
        15, "local used as a MAP KEY, map escapes -> true (key is a stored ref)",
        "package main\n"
        "func f(s string) map[string]int {\n"
        "    m := map[string]int{}\n"
        "    k := s + \"x\"\n"
        "    m[k] = 1\n"
        "    return m\n"
        "}\n",
        "f", { { "m", true }, { "k", true } }, 2
    },
    {
        // The slice counterpart of row 15, kept as the CONTRAST: this one was
        // already true before the map-key fix, and it must stay true.
        16, "local appended to a slice that escapes -> true",
        "package main\n"
        "func f(s string) []string {\n"
        "    parts := []string{}\n"
        "    k := s + \"x\"\n"
        "    parts = append(parts, k)\n"
        "    return parts\n"
        "}\n",
        "f", { { "parts", true }, { "k", true } }, 2
    },

    // ---------------- SOUNDNESS: one row per unguarded engine arm ----------
    //
    // docs/adr/0002-measurements/escape_arm_coverage.md mutated all 17 arms of
    // escape_expr_taint in BOTH directions and measured which suite notices.
    // Eleven of the 16 testable arms had NO soundness coverage in ANY of the
    // three suites: nothing detected the arm claiming its expression aliases
    // nothing, and under-marking is the one bug class that can dangle a
    // pointer once T4 emits a release.
    //
    // Every row below routes the taint THROUGH one arm with NO OTHER PATH to a
    // sink. That is deliberate and it is why row 15 does not already cover the
    // binary arm: row 15 contains `k := s + "x"`, but `k`'s verdict comes from
    // sink #2b (the map key), so under-marking the binary arm leaves row 15
    // green. A row that passes for a second reason measures nothing.
    //
    // Each row was verified to FAIL under
    // `scripts/escape_arm_coverage.sh <ARM> under` and to pass with the arm
    // restored.
    {
        // AST_BINARY_EXPR. `s` is bound to a LITERAL, so it carries only its
        // own bit and no call sink touches it. The concatenation is the only
        // route to the global.
        17, "local reaches a global THROUGH a binary expression -> true",
        "package main\n"
        "var g string\n"
        "func f() {\n"
        "    s := \"abc\"\n"
        "    g = s + \"def\"\n"
        "}\n",
        "f", { { "s", true } }, 1
    },
    {
        // AST_STRUCT_LITERAL. The arm unions over field_values, which is also
        // why AST_KEYED_ELEMENT never runs: the parser hands the values list
        // directly and no keyed-element node is walked.
        18, "local carried into a global inside a STRUCT LITERAL -> true",
        "package main\n"
        "type T struct { p *int }\n"
        "var g T\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    g = T{p: x}\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        // AST_SLICE_EXPR (a SliceLitNode, not a slice-index). This is the
        // analysis-side guard for the "slices of pointers" ledger item:
        // goo_slice_append copies raw bytes, so nothing counts an appended
        // pointer at RUNTIME, but the analysis must at least mark it.
        19, "local carried into a global inside a SLICE LITERAL -> true",
        "package main\n"
        "var g []*int\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    g = []*int{x}\n"
        "}\n",
        "f", { { "x", true } }, 1
    },
    {
        // AST_PAREN_EXPR is the MAP LITERAL (MapLitNode). The arm walks the
        // keys list AND the values list, so this row taints one of each: `k`
        // proves the key loop, `x` proves the value loop. One row, two loops.
        // The map is 23.2% of the daemon's retained bytes.
        20, "locals carried into a global inside a MAP LITERAL -> true, true",
        "package main\n"
        "var g map[string]*int\n"
        "func f() {\n"
        "    k := \"a\"\n"
        "    x := new(int)\n"
        "    g = map[string]*int{k: x}\n"
        "}\n",
        "f", { { "k", true }, { "x", true } }, 2
    },
    {
        // AST_INDEX_EXPR, and this is its FIRST fixture in any of the three
        // suites. `--reach` measured 0 hits across all 70 rows before this one.
        //
        // That zero looked wrong against PR #255, whose headline was the
        // map-key sink, so it was checked: assign_to_lvalue handles the WRITE
        // `m[k] = v` through mark_lvalue_subscripts (sink #2b) and never routes
        // through this arm. This arm is the READ, `v := xs[i]`, and nothing
        // exercised it.
        //
        // What it must assert: an element read OUT of a container aliases the
        // container. `xs[0]` can be a pointer the slice's buffer holds, so if
        // the element reaches a global the slice has to be kept alive too.
        // Under-marking here would let T4 release a buffer that a live global
        // still points into.
        21, "local read by INDEX into a global -> true (the element aliases the base)",
        "package main\n"
        "var g *int\n"
        "func f() {\n"
        "    xs := []*int{}\n"
        "    g = xs[0]\n"
        "}\n",
        "f", { { "xs", true } }, 1
    },
    {
        // PRECISION COST of the arm above, pinned deliberately rather than left
        // to drift -- the same treatment block-escape row 31 gets.
        //
        // AST_INDEX_EXPR unions the taint of the BASE and of the INDEX. On a
        // WRITE that is required (sink #2b: goo_map_set_sv keeps the key
        // pointer verbatim). On a READ it is over-conservative, because
        // goo_map_get stores nothing -- yet `k` still reads as escaping here,
        // and a local used only as a lookup key can therefore never be
        // released.
        //
        // MEASURED before deciding to keep it: the daemon's string local stays
        // escaping because the map holds it as a key in a WRITE, not a read
        // (docs/superpowers/specs/
        // 2026-07-28-daemon-alloc-attribution-findings.md). So tightening this
        // to mark only the base would reclaim ~0% of the daemon, and it would
        // be a soundness-relevant change to the shared engine. Not worth it
        // until a measurement says otherwise.
        //
        // If T4 ever does tighten it, this row is the one that must change, and
        // changing it should be an argument, not an accident.
        22, "local used only as a map READ key -> true (conservative, see comment)",
        "package main\n"
        "var g *int\n"
        "func f(m map[string]*int) {\n"
        "    k := \"a\"\n"
        "    g = m[k]\n"
        "}\n",
        "f", { { "k", true } }, 1
    },
};

static int failures = 0;
static int checks = 0;

int main(void) {
    printf("Running per-local escape summary tests...\n");
    size_t nrows = sizeof(rows) / sizeof(rows[0]);

    for (size_t r = 0; r < nrows; r++) {
        TestRow* row = &rows[r];
        printf("=== Row %d: %s ===\n", row->row, row->description);

        int parse_rc = parse_input(row->src, "test.goo");
        if (parse_rc != 0 || !ast_root) {
            printf("  FAIL: parse error (rc=%d)\n", parse_rc);
            failures++;
            if (ast_root) { ast_node_free(ast_root); ast_root = NULL; }
            continue;
        }

        // Run the real type checker so FuncLitNode.captured_names is
        // populated the way the compiler pipeline populates it (row 8's
        // closure-capture case needs it). The return code is ignored on
        // purpose: row 12 calls an unregistered external and is EXPECTED to
        // fail type-checking, which must not stop the analysis.
        TypeChecker* checker = type_checker_new();
        if (checker) {
            type_check_program(checker, ast_root);
        }

        ParamEscapeResult* summaries = param_escape_analyze(ast_root);
        LocalEscapeResult* result = local_escape_analyze(ast_root, summaries);
        if (!result) {
            printf("  FAIL: local_escape_analyze returned NULL\n");
            failures++;
            continue;
        }

        int row_failed = 0;
        for (int i = 0; i < row->expect_count; i++) {
            checks++;
            bool got = local_escape_local_escapes(result, row->fn, row->expect[i].name);
            if (got != row->expect[i].expected_escapes) {
                printf("  FAIL: local '%s' escapes=%d, expected %d\n",
                       row->expect[i].name, (int)got,
                       (int)row->expect[i].expected_escapes);
                failures++;
                row_failed = 1;
            }
        }
        printf("  Row %d: %s\n", row->row, row_failed ? "FAIL" : "PASS");

        local_escape_result_free(result);
        param_escape_result_free(summaries);
        if (checker) type_checker_free(checker);
        if (ast_root) { ast_node_free(ast_root); ast_root = NULL; }
    }

    printf("\n=================================================\n");
    printf("local_escape_test summary: %d assertions passed, %d failed\n",
           checks - failures, failures);
    return failures ? 1 : 0;
}
