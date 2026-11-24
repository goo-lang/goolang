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
    {
        27, "a bare return returns the named results (C3)",
        // `r = p; return` has no return VALUES, so the engine saw no flow from
        // p to the result: return_escapes stayed false and the caller's ARC
        // freed a live argument. `q` is never stored in a result, so it must
        // stay non-escaping, which proves the named results are not simply
        // marked wholesale. `keep` reaches its result through a local.
        "package main\n"
        "func pick(p *int, q *int) (r *int) {\n"
        "    r = p\n"
        "    return\n"
        "}\n"
        "func keep(p *int) (r *int) {\n"
        "    x := p\n"
        "    r = x\n"
        "    return\n"
        "}\n",
        { { "pick", 2, { true, false }, true, true },
          { "keep", 1, { true }, true, true } }, 2
    },
};

// returns_fresh rows (finding C2). A table of their own, because they assert
// nothing about escapes[] and the rows above assert nothing about ownership.
#define MAX_FRESH_FUNCS 6

typedef struct {
    const char* fn_name;
    bool        expected;
    // The name belongs to more than one function, so param_escape_lookup's
    // first match is not THE summary. Only the all-matches helper is checked.
    bool        name_is_shared;
} FreshExpectation;

typedef struct {
    int              row;
    const char*      description;
    const char*      src;
    FreshExpectation expect[MAX_FRESH_FUNCS];
    int              expect_count;
} FreshRow;

static FreshRow fresh_rows[] = {
    {
        28, "returns_fresh needs a positive proof on every return path (C2)",
        // return_escapes is false for all four of getS, mkReg and mk, because
        // none of them returns a value derived from a PARAMETER. ARC read that
        // as "the caller owns the result" and freed GS[0] and r.items[0].
        //
        // mk is the recursion row: its second path returns mk's own result, so
        // a fixpoint that starts every function at "fresh is false" and only
        // raises it can never prove mk. The pass computes the dual instead --
        // "may return a borrowed value", which starts false and only rises.
        //
        // pick is Task 3.1's bare return of a named result that holds a param.
        "package main\n"
        "type Box struct{ v int }\n"
        "type Reg struct{ items []*Box }\n"
        "var GS []*Box\n"
        "func getS() *Box {\n"
        "    return GS[0]\n"
        "}\n"
        "func mkReg(r *Reg) *Box {\n"
        "    b := &Box{v: 30}\n"
        "    r.items = append(r.items, b)\n"
        "    return b\n"
        "}\n"
        "func mk(n int) *Box {\n"
        "    if n == 0 {\n"
        "        return &Box{v: 7}\n"
        "    }\n"
        "    return mk(n - 1)\n"
        "}\n"
        "func pick(p *Box) (r *Box) {\n"
        "    r = p\n"
        "    return\n"
        "}\n",
        { { "getS", false, false }, { "mkReg", false, false },
          { "mk", true, false },    { "pick", false, false } }, 4
    },
    {
        29, "returns_fresh: the local rule, and the shapes it must refuse",
        // viaLocal is the precision half: a local bound only to a fresh value
        // and escaping only by return is fresh.
        //
        // orNil is precision too: a literal, nil included, frees nothing when
        // released, so an error path that returns one must not make the
        // function's other paths leak.
        //
        // The other three are the soundness half. Each holds a value the
        // caller does not own on SOME path, and each is a way the local rule
        // could be fooled: a write through the local's address, a same-named
        // local in an inner scope that is not the one returned, and a rebind
        // to a global.
        "package main\n"
        "type Box struct{ v int }\n"
        "var G *Box\n"
        "func viaLocal() *Box {\n"
        "    b := &Box{v: 1}\n"
        "    return b\n"
        "}\n"
        "func addrTaken() *Box {\n"
        "    b := &Box{v: 1}\n"
        "    p := &b\n"
        "    *p = G\n"
        "    return b\n"
        "}\n"
        "func shadowGlobal(c bool) *Box {\n"
        "    if c {\n"
        "        G := &Box{v: 2}\n"
        "        _ = G\n"
        "    }\n"
        "    return G\n"
        "}\n"
        "func rebound(c bool) *Box {\n"
        "    b := &Box{v: 3}\n"
        "    if c {\n"
        "        b = G\n"
        "    }\n"
        "    return b\n"
        "}\n"
        "func orNil(c bool) *Box {\n"
        "    if c {\n"
        "        return &Box{v: 4}\n"
        "    }\n"
        "    return nil\n"
        "}\n",
        { { "viaLocal", true, false },      { "addrTaken", false, false },
          { "shadowGlobal", false, false }, { "rebound", false, false },
          { "orNil", true, false } }, 5
    },
    {
        30, "returns_fresh: a name shared by two functions needs EVERY match fresh (R13)",
        // Summaries are found by bare name, and a method can share its name
        // with a function. The function get comes first and is fresh; the
        // method B.get returns a global. Neither the ownership lookup nor the
        // fixpoint can tell which one a call means, so both must say no:
        // get itself, and viaGet, which returns get's result.
        "package main\n"
        "type Box struct{ v int }\n"
        "type B struct{}\n"
        "var G *Box\n"
        "func get() *Box {\n"
        "    return &Box{v: 1}\n"
        "}\n"
        "func (b *B) get() *Box {\n"
        "    return G\n"
        "}\n"
        "func viaGet() *Box {\n"
        "    return get()\n"
        "}\n"
        "func only() *Box {\n"
        "    return &Box{v: 2}\n"
        "}\n",
        { { "get", false, true }, { "viaGet", false, false }, { "only", true, false } }, 3
    },
};

static void run_fresh_row(const FreshRow* row) {
    goo_check_row(row->row, row->description);

    char buf[256];
    int parse_rc = parse_input(row->src, "test.goo");
    snprintf(buf, sizeof(buf), "row %d: the fixture parses (rc=%d)", row->row, parse_rc);
    goo_check(parse_rc == 0 && ast_root != NULL, buf);
    if (parse_rc != 0 || !ast_root) {
        if (ast_root) { ast_node_free(ast_root); ast_root = NULL; }
        return;
    }
    TypeChecker* checker = type_checker_new();
    if (checker) type_check_program(checker, ast_root);

    ParamEscapeResult* result = param_escape_analyze(ast_root);
    goo_check(result != NULL, "param_escape_analyze returned a result");
    for (int e = 0; result && e < row->expect_count; e++) {
        const FreshExpectation* fe = &row->expect[e];
        if (!fe->name_is_shared) {
            const ParamEscapeSummary* summary = param_escape_lookup(result, fe->fn_name);
            snprintf(buf, sizeof(buf), "row %d: summary for '%s' found", row->row, fe->fn_name);
            goo_check(summary != NULL, buf);
            if (summary) {
                snprintf(buf, sizeof(buf), "row %d: '%s'.returns_fresh == %s (got %s)",
                         row->row, fe->fn_name, fe->expected ? "true" : "false",
                         summary->returns_fresh ? "true" : "false");
                goo_check(summary->returns_fresh == fe->expected, buf);
            }
        }
        bool via_helper = param_escape_returns_fresh(result, fe->fn_name);
        snprintf(buf, sizeof(buf), "row %d: param_escape_returns_fresh('%s') == %s (got %s)",
                 row->row, fe->fn_name, fe->expected ? "true" : "false",
                 via_helper ? "true" : "false");
        goo_check(via_helper == fe->expected, buf);
    }
    if (result) {
        goo_check(!param_escape_returns_fresh(result, "__no_such_function__"),
                  "unknown function name is conservatively not fresh");
        param_escape_result_free(result);
    }
    if (checker) type_checker_free(checker);
    ast_node_free(ast_root);
    ast_root = NULL;
}

int main(void) {
    size_t n = sizeof(rows) / sizeof(rows[0]);
    size_t n_fresh = sizeof(fresh_rows) / sizeof(fresh_rows[0]);
    goo_check_expect((int)(n + n_fresh));

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

    for (size_t r = 0; r < n_fresh; r++) run_fresh_row(&fresh_rows[r]);

    return goo_check_done("param-escape-test");
}
