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
