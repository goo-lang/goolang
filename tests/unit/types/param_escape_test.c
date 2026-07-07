// Task 7a: interprocedural param-escape summaries — table-driven unit test.
//
// Encodes all 15 rows of the test matrix in
// docs/superpowers/specs/2026-07-07-arena-7a-param-escape-summaries-design.md.
// Each row: a Goo source string -> parse_input -> (type_check_program, so
// FuncLitNode.captured_names is populated exactly the way the real compiler
// pipeline populates it, per row 7's requirement) -> param_escape_analyze ->
// assert escapes[]/return_escapes for the named function(s).
//
// Rows 2, 5, 7, 8, 10, 13, 14 are the load-bearing "must be true" cases (an
// unsound implementation shows up as a wrong `false` there). Rows 1, 6, 9,
// 11, 12 guard against trivially marking everything (which would be sound
// but useless).
//
// All 15 source strings verified to parse successfully against this
// front-end before this table was written (see task report). Row 2
// deliberately calls an unregistered/external function (fmt.Println) and so
// is expected to FAIL type-checking (undefined "fmt") — that's fine, we only
// need it to parse; param_escape_analyze operates on the AST regardless of
// type-check outcome, and the whole point of row 2 is exercising the
// external/unregistered-callee retain-all rule.

#include "parser.h"
#include "ast.h"
#include "types.h"
#include "param_escape.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_EXPECT_PARAMS 4
#define MAX_EXPECT_FUNCS 3

typedef struct {
    const char* fn_name;
    size_t      param_count;                     // expected flattened param count
    bool        expected_escapes[MAX_EXPECT_PARAMS];
    bool        check_return_escapes;
    bool        expected_return_escapes;
} FuncExpectation;

typedef struct {
    int             row;
    const char*     description;
    const char*     src;
    FuncExpectation expect[MAX_EXPECT_FUNCS];
    int             expect_count;
} TestRow;

static TestRow rows[] = {
    {
        1, "param unused -> false",
        "package main\n"
        "func f(p *int) {\n"
        "}\n",
        { { "f", 1, { false }, false, false } }, 1
    },
    {
        2, "param only read via EXTERNAL call -> true (external retains, pure-conservative)",
        "package main\n"
        "func f(p *int) {\n"
        "    fmt.Println(p)\n"
        "}\n",
        { { "f", 1, { true }, false, false } }, 1
    },
    {
        3, "param returned -> true, return_escapes true",
        "package main\n"
        "func f(p *int) *int {\n"
        "    return p\n"
        "}\n",
        { { "f", 1, { true }, true, true } }, 1
    },
    {
        4, "one of two returned -> a=false, b=true",
        "package main\n"
        "func f(a *int, b *int) *int {\n"
        "    return b\n"
        "}\n",
        { { "f", 2, { false, true }, true, true } }, 1
    },
    {
        5, "stored to global -> true",
        "package main\n"
        "var g *int\n"
        "func f(p *int) {\n"
        "    g = p\n"
        "}\n",
        { { "f", 1, { true }, false, false } }, 1
    },
    {
        6, "stored to plain local, never out -> false",
        "package main\n"
        "func f(p *int) {\n"
        "    x := p\n"
        "    _ = x\n"
        "}\n",
        { { "f", 1, { false }, false, false } }, 1
    },
    {
        7, "captured by closure -> true",
        "package main\n"
        "func use(p *int) {\n"
        "}\n"
        "func f(p *int) {\n"
        "    go func() {\n"
        "        use(p)\n"
        "    }()\n"
        "}\n",
        { { "f", 1, { true }, false, false } }, 1
    },
    {
        8, "passed to goroutine arg -> true (independent of callee's own summary)",
        "package main\n"
        "func g(x *int) {\n"
        "}\n"
        "func f(p *int) {\n"
        "    go g(p)\n"
        "}\n",
        {
            { "f", 1, { true }, false, false },
            { "g", 1, { false }, false, false }, // g itself doesn't retain x
        }, 2
    },
    {
        9, "passed to non-retaining user callee -> false",
        "package main\n"
        "func g(x *int) {\n"
        "}\n"
        "func f(p *int) {\n"
        "    g(p)\n"
        "}\n",
        {
            { "f", 1, { false }, false, false },
            { "g", 1, { false }, false, false },
        }, 2
    },
    {
        10, "passed to retaining user callee -> true (transitive)",
        "package main\n"
        "var g *int\n"
        "func stash(x *int) {\n"
        "    g = x\n"
        "}\n"
        "func f(p *int) {\n"
        "    stash(p)\n"
        "}\n",
        {
            { "f", 1, { true }, false, false },
            { "stash", 1, { true }, false, false },
        }, 2
    },
    {
        11, "recursion terminates -> p=false (self-call non-retaining)",
        "package main\n"
        "func f(p *int) {\n"
        "    f(p)\n"
        "}\n",
        { { "f", 1, { false }, false, false } }, 1
    },
    {
        12, "mutual recursion terminates -> both false",
        "package main\n"
        "func a(p *int) {\n"
        "    b(p)\n"
        "}\n"
        "func b(q *int) {\n"
        "    a(q)\n"
        "}\n",
        {
            { "a", 1, { false }, false, false },
            { "b", 1, { false }, false, false },
        }, 2
    },
    {
        13, "transitive through return -> true",
        "package main\n"
        "func id(x *int) *int {\n"
        "    return x\n"
        "}\n"
        "func f(p *int) *int {\n"
        "    return id(p)\n"
        "}\n",
        {
            { "f", 1, { true }, true, true },
            { "id", 1, { true }, true, true },
        }, 2
    },
    {
        14, "field store on param (out-param) -> true",
        "package main\n"
        "type Box struct {\n"
        "    next *Box\n"
        "}\n"
        "func f(p *Box) {\n"
        "    p.next = p\n"
        "}\n",
        { { "f", 1, { true }, false, false } }, 1
    },
    {
        15, "method receiver as param 0 -> true",
        "package main\n"
        "type T struct {\n"
        "    val int\n"
        "}\n"
        "var gT *T\n"
        "func (r *T) m() {\n"
        "    gT = r\n"
        "}\n",
        { { "m", 1, { true }, false, false } }, 1
    },
};

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* fmt_ctx) {
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("    FAIL: %s\n", fmt_ctx);
    }
}

int main(void) {
    size_t n = sizeof(rows) / sizeof(rows[0]);
    int row_failed_count = 0;

    for (size_t r = 0; r < n; r++) {
        TestRow* row = &rows[r];
        printf("=== Row %d: %s ===\n", row->row, row->description);

        int before_fail = g_fail;

        int parse_rc = parse_input(row->src, "test.goo");
        if (parse_rc != 0 || !ast_root) {
            printf("    FAIL: parse_input failed (rc=%d, ast_root=%p)\n", parse_rc, (void*)ast_root);
            g_fail++;
            if (ast_root) { ast_node_free(ast_root); ast_root = NULL; }
            printf("  Row %d: FAIL\n\n", row->row);
            row_failed_count++;
            continue;
        }

        // Run the real type checker so FuncLitNode.captured_names is
        // populated exactly the way the compiler pipeline populates it
        // (parse -> typecheck -> escape-analyze -> codegen). We deliberately
        // ignore the return code: row 2's source calls an unregistered
        // external function and is EXPECTED to fail type-checking, but that
        // must not stop param_escape_analyze from running on the resulting
        // (partially-checked) AST.
        TypeChecker* checker = type_checker_new();
        if (checker) {
            type_check_program(checker, ast_root);
        }

        ParamEscapeResult* result = param_escape_analyze(ast_root);
        if (!result) {
            printf("    FAIL: param_escape_analyze returned NULL (allocation failure)\n");
            g_fail++;
        } else {
            for (int e = 0; e < row->expect_count; e++) {
                FuncExpectation* fe = &row->expect[e];
                const ParamEscapeSummary* summary = param_escape_lookup(result, fe->fn_name);

                char ctxbuf[256];
                snprintf(ctxbuf, sizeof(ctxbuf), "row %d: summary for '%s' found", row->row, fe->fn_name);
                check(summary != NULL, ctxbuf);
                if (!summary) continue;

                snprintf(ctxbuf, sizeof(ctxbuf), "row %d: '%s' param_count == %zu (got %zu)",
                         row->row, fe->fn_name, fe->param_count, summary->param_count);
                check(summary->param_count == fe->param_count, ctxbuf);

                size_t check_n = fe->param_count < summary->param_count ? fe->param_count : summary->param_count;
                for (size_t i = 0; i < check_n; i++) {
                    snprintf(ctxbuf, sizeof(ctxbuf), "row %d: '%s'.escapes[%zu] == %s (got %s)",
                             row->row, fe->fn_name, i,
                             fe->expected_escapes[i] ? "true" : "false",
                             summary->escapes[i] ? "true" : "false");
                    check(summary->escapes[i] == fe->expected_escapes[i], ctxbuf);
                }

                if (fe->check_return_escapes) {
                    snprintf(ctxbuf, sizeof(ctxbuf), "row %d: '%s'.return_escapes == %s (got %s)",
                             row->row, fe->fn_name,
                             fe->expected_return_escapes ? "true" : "false",
                             summary->return_escapes ? "true" : "false");
                    check(summary->return_escapes == fe->expected_return_escapes, ctxbuf);
                }

                // Also exercise the param_escape_param_escapes lookup helper
                // for consistency with the summary struct's own array.
                for (size_t i = 0; i < check_n; i++) {
                    bool via_helper = param_escape_param_escapes(result, fe->fn_name, i);
                    snprintf(ctxbuf, sizeof(ctxbuf),
                             "row %d: param_escape_param_escapes('%s', %zu) matches summary->escapes[%zu]",
                             row->row, fe->fn_name, i, i);
                    check(via_helper == summary->escapes[i], ctxbuf);
                }
            }

            // Conservative-miss contract, checked once against a name/index
            // that can never be registered.
            {
                bool miss = param_escape_param_escapes(result, "__no_such_function__", 0);
                check(miss == true, "unknown function name conservatively returns true");
            }

            param_escape_result_free(result);
        }

        if (checker) type_checker_free(checker);
        ast_node_free(ast_root);
        ast_root = NULL;

        bool row_ok = (g_fail == before_fail);
        printf("  Row %d: %s\n\n", row->row, row_ok ? "PASS" : "FAIL");
        if (!row_ok) row_failed_count++;
    }

    printf("=================================================\n");
    printf("param_escape_test summary: %d assertions passed, %d failed, %d/%zu rows failed\n",
           g_pass, g_fail, row_failed_count, n);

    return g_fail ? 1 : 0;
}
