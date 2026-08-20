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
#include "../goo_check.h"
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
        2, "param passed to a NON-whitelisted external -> true (pure-conservative retain)",
        "package main\n"
        "func f(p *int) {\n"
        "    stash(p)\n"
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
    {
        // A param sent on a channel is received by another goroutine or the
        // caller, so it outlives the function -> escapes. `ch <- p` is a
        // BinaryExprNode with the ARROW operator; before the fix walk_stmt
        // only handled assign operators, so the send was a discarded taint and
        // `p` was wrongly summarized non-retaining (which then let
        // block_escape keep a sent value in an arena -> use-after-free). `ch`
        // itself (param 0) is not sent, so it does not escape.
        16, "param sent on a channel -> true (ch does not, the sent value does)",
        "package main\n"
        "func send(ch chan *int, p *int) {\n"
        "    ch <- p\n"
        "}\n",
        { { "send", 2, { false, true }, false, false } }, 1
    },
    {
        // THE SAME SEND, INSIDE A SELECT CASE. Row 16 covers `ch <- p` as an
        // expression statement; this reaches the identical sink through the
        // select arm, where comm is an EXPRESSION and goes to
        // escape_walk_expr_stmt rather than escape_walk_stmt.
        //
        // Before that fix this passed because escape_walk_stmt dropped the
        // expression on `default:` and called escape_mark_all. The row exists so
        // the sink is pinned in each of the three passes and not in one.
        25, "param sent on a channel inside a SELECT case -> true",
        "package main\n"
        "func sendsel(ch chan *int, p *int) {\n"
        "    select {\n"
        "    case ch <- p:\n"
        "    }\n"
        "}\n",
        { { "sendsel", 2, { false, true }, false, false } }, 1
    },
    {
        // 7a' non-retaining whitelist: fmt.Println does not retain its args, so
        // a param only passed to it does NOT escape (was `true` pre-whitelist).
        17, "param passed to fmt.Println -> false (7a' whitelist, non-retaining)",
        "package main\n"
        "func f(p *int) {\n"
        "    fmt.Println(p)\n"
        "}\n",
        { { "f", 1, { false }, false, false } }, 1
    },
    {
        // A whitelisted builtin (len) likewise does not retain its argument.
        18, "param passed to len -> false (7a' whitelist)",
        "package main\n"
        "func f(p *int) {\n"
        "    _ = len(p)\n"
        "}\n",
        { { "f", 1, { false }, false, false } }, 1
    },
    {
        19, "param used as a method-call RECEIVER -> true (unresolved callee)",
        // Soundness row, mirroring block_escape row 30. A selector callee does
        // not resolve to a summary, and the receiver is not in call->args, so
        // the retain-all rule for an unresolved callee never covered it. The
        // receiver taint was computed and freed, so `stash` storing its
        // receiver in a global left f's param 0 marked non-escaping.
        "package main\n"
        "type T struct { x int }\n"
        "var g *T\n"
        "func (t *T) stash() {\n"
        "    g = t\n"
        "}\n"
        "func f(p *T) {\n"
        "    p.stash()\n"
        "}\n",
        { { "f", 1, { true }, false, false } }, 1
    },
    {
        20, "param stored into a NAMED RESULT, bare return -> true",
        // CHARACTERISATION pin, not a bug fix — this already holds, and the
        // reason is load-bearing enough to nail down.
        //
        // The return sink walks ReturnStmtNode->values, which a bare `return`
        // leaves empty, so it never fires and return_escapes stays false. The
        // param is still correctly marked, but by a DIFFERENT sink: LocalEnv
        // is seeded from f->param_names only (analyze_function_body), a named
        // result is not a parameter, so `p = q` is a store to a non-local and
        // fires the store-escape sink instead.
        //
        // Anyone who later "tidies" named results into LocalEnv silently turns
        // this into an under-mark, because the return sink will NOT catch it.
        // That combination is a use-after-free, so this row must stay red in
        // that world.
        "package main\n"
        "type T struct { x int }\n"
        "func id(q *T) (p *T) {\n"
        "    p = q\n"
        "    return\n"
        "}\n",
        { { "id", 1, { true }, false, false } }, 1
    },
    {
        21, "param passed to a non-retaining SHIM -> false (whitelist widened past fmt)",
        // Rows 17 and 18 pin the fmt/builtin half of the whitelist. This is
        // the half that used to be missing, and its absence dominated
        // everything else in the ARC leg.
        //
        // A C shim has no Goo body, so param_escape_lookup misses and
        // call_taint took its "external/unregistered: pure-conservative"
        // branch. Measured on bench/daemon: EVERY local passed to any stdlib
        // call was marked escaping for that reason alone, so no release
        // consumer could ever reclaim anything in a real program. See
        // docs/superpowers/specs/2026-07-28-daemon-alloc-attribution-findings.md.
        //
        // goo_strings_trim_space does goo_alloc + memcpy, so it neither keeps
        // the argument nor returns a view of it. NOTE that is the SHIM's
        // behaviour, not Go's: Go's strings.TrimSpace returns a slice of its
        // argument.
        "package main\n"
        "import \"strings\"\n"
        "func f(s string) string {\n"
        "    return strings.TrimSpace(s)\n"
        "}\n",
        { { "f", 1, { false }, false, false } }, 1
    },
    {
        22, "param passed to errors.Unwrap -> true (its result ALIASES the argument)",
        // SOUNDNESS row, and the counterweight to row 21. Widening the
        // whitelist to a whole table is only safe if the table says no where
        // it must. goo_error_unwrap returns `e->cause` (src/runtime/runtime.c)
        // — a pointer INTO the argument's own structure — so its row is 0 and
        // has to stay 0.
        "package main\n"
        "import \"errors\"\n"
        "func f(e error) error {\n"
        "    return errors.Unwrap(e)\n"
        "}\n",
        { { "f", 1, { true }, false, true } }, 1
    },
    {
        23, "shim name SHADOWED by a local -> true (the base is a variable, not a package)",
        // SOUNDNESS row. goo_callee_is_non_retaining answers by package name
        // and selector, so a local named `strings` would otherwise collect the
        // strings package's whitelist and `strings.Split(p)` would read as
        // non-retaining — while Split is really a method on the local that
        // stores p. selector_base_is_local (escape_core.c) is the guard, and
        // membership in the walk's own LocalEnv is what it tests.
        "package main\n"
        "type Splitter struct { held *int }\n"
        "var g *Splitter\n"
        "func (s *Splitter) Split(p *int) {\n"
        "    s.held = p\n"
        "    g = s\n"
        "}\n"
        "func f(p *int) {\n"
        "    strings := &Splitter{}\n"
        "    strings.Split(p)\n"
        "}\n",
        { { "f", 1, { true }, false, false } }, 1
    },
    {
        24, "param alive across a `for i := 0; i < 3; i++` loop -> false",
        // PRECISION row, and it closes the ledger item PR #255 opened.
        //
        // Deleting the AST_POSTFIX_EXPR arm from the shared engine
        // (src/types/escape_core.c) failed local-escape row 4 and left ALL 23
        // rows of this table green, so none of them covered `i++`. The measured
        // cause was not "undetected" but stronger: `--reach` counts ZERO
        // postfix nodes across every fixture in this file. No row here even
        // CONTAINED one.
        //
        // The teeth: without that arm the postfix falls to the default arm,
        // which marks every slot escaping on the spot, so `p` reads true and
        // this row fails. `for i := 0; i < 3; i = i + 1` would NOT fail, which
        // is exactly how the original defect was found.
        //
        // See docs/adr/0002-measurements/escape_arm_coverage.md.
        "package main\n"
        "func f(p *int) {\n"
        "    for i := 0; i < 3; i++ {\n"
        "        _ = p\n"
        "    }\n"
        "}\n",
        { { "f", 1, { false }, false, false } }, 1
    },
    {
        25, "param passed to a DEFER whose callee does not retain it -> false",
        // PRECISION row, and the guard on PARAM_HOOKS.defer_is_like_go = false.
        //
        // This is the field where this pass and block_escape must disagree. At
        // FUNCTION granularity a defer runs inside the frame, before it is
        // gone, so `false` is the PRECISE answer here; block_escape sets the
        // same field true because its boundary closes first. Nothing recorded
        // that disagreement as a test until scripts/escape_teeth.sh flipped the
        // field and all 24 rows above stayed green.
        //
        // The teeth: flipped to true, a deferred argument escapes
        // unconditionally (sink #4), so `p` reads true and this row fails.
        // `sink` deliberately does NOT retain its parameter, so sink #5 is
        // silent and the defer treatment is the only thing that can mark `p`.
        "package main\n"
        "func sink(q *int) {\n"
        "}\n"
        "func f(p *int) {\n"
        "    defer sink(p)\n"
        "}\n",
        { { "f", 1, { false }, false, false } }, 1
    },
    {
        26, "returning a param sets BOTH escapes[0] and return_escapes",
        // The COUPLING row. It looks trivial and it is not: it is the reason
        // scripts/escape_teeth.sh carries no `retention-return` entry for the
        // block and local tables, and that omission needs a fact to stand on.
        //
        // Sink #1 (AST_RETURN_STMT in the shared engine) marks the returned
        // value BEFORE on_return records the interprocedural signal. So a
        // callee whose return_escapes is true ALWAYS has escapes[i] true as
        // well -- the two cannot be set apart by any fixture.
        //
        // The consequence for the other two passes: at a call site `retains`
        // reads callee_escapes[i], which is already true, so the argument is
        // marked by sink #5 no matter what return_escapes says. Forcing
        // return_escapes false there changes no verdict, which is exactly what
        // the teeth measured before the entries were dropped.
        //
        // param_escape is the one pass where the field IS observable, because
        // it composes: `func a(p) { return b(p) }` marks a's param only if
        // b(p)'s result carries taint. That is the `retention-return` entry
        // this table's own teeth still keep.
        "package main\n"
        "func id(p *int) *int {\n"
        "    return p\n"
        "}\n",
        { { "id", 1, { true }, true, true } }, 1
    },
};



int main(void) {
    size_t n = sizeof(rows) / sizeof(rows[0]);
    goo_check_expect((int)n);

    for (size_t r = 0; r < n; r++) {
        TestRow* row = &rows[r];
        goo_check_row(row->row, row->description);

        char parsebuf[128];
        int parse_rc = parse_input(row->src, "test.goo");
        snprintf(parsebuf, sizeof(parsebuf),
                 "row %d: the fixture parses (rc=%d, ast_root=%p)",
                 row->row, parse_rc, (void*)ast_root);
        goo_check(parse_rc == 0 && ast_root != NULL, parsebuf);
        if (parse_rc != 0 || !ast_root) {
            if (ast_root) { ast_node_free(ast_root); ast_root = NULL; }
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
            goo_check(false, "param_escape_analyze returned NULL (allocation failure)");
        } else {
            for (int e = 0; e < row->expect_count; e++) {
                FuncExpectation* fe = &row->expect[e];
                const ParamEscapeSummary* summary = param_escape_lookup(result, fe->fn_name);

                char ctxbuf[256];
                snprintf(ctxbuf, sizeof(ctxbuf), "row %d: summary for '%s' found", row->row, fe->fn_name);
                goo_check(summary != NULL, ctxbuf);
                if (!summary) continue;

                snprintf(ctxbuf, sizeof(ctxbuf), "row %d: '%s' param_count == %zu (got %zu)",
                         row->row, fe->fn_name, fe->param_count, summary->param_count);
                goo_check(summary->param_count == fe->param_count, ctxbuf);

                size_t check_n = fe->param_count < summary->param_count ? fe->param_count : summary->param_count;
                for (size_t i = 0; i < check_n; i++) {
                    snprintf(ctxbuf, sizeof(ctxbuf), "row %d: '%s'.escapes[%zu] == %s (got %s)",
                             row->row, fe->fn_name, i,
                             fe->expected_escapes[i] ? "true" : "false",
                             summary->escapes[i] ? "true" : "false");
                    goo_check(summary->escapes[i] == fe->expected_escapes[i], ctxbuf);
                }

                if (fe->check_return_escapes) {
                    snprintf(ctxbuf, sizeof(ctxbuf), "row %d: '%s'.return_escapes == %s (got %s)",
                             row->row, fe->fn_name,
                             fe->expected_return_escapes ? "true" : "false",
                             summary->return_escapes ? "true" : "false");
                    goo_check(summary->return_escapes == fe->expected_return_escapes, ctxbuf);
                }

                // Also exercise the param_escape_param_escapes lookup helper
                // for consistency with the summary struct's own array.
                for (size_t i = 0; i < check_n; i++) {
                    bool via_helper = param_escape_param_escapes(result, fe->fn_name, i);
                    snprintf(ctxbuf, sizeof(ctxbuf),
                             "row %d: param_escape_param_escapes('%s', %zu) matches summary->escapes[%zu]",
                             row->row, fe->fn_name, i, i);
                    goo_check(via_helper == summary->escapes[i], ctxbuf);
                }
            }

            // Conservative-miss contract, checked once against a name/index
            // that can never be registered.
            {
                bool miss = param_escape_param_escapes(result, "__no_such_function__", 0);
                goo_check(miss == true, "unknown function name conservatively returns true");
            }

            param_escape_result_free(result);
        }

        if (checker) type_checker_free(checker);
        ast_node_free(ast_root);
        ast_root = NULL;

    }

    return goo_check_done("param-escape-test");
}
