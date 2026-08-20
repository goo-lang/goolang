// Task 7b: per-alloc-site block-escape decisions — table-driven unit test.
//
// Encodes all 15 rows of the test matrix in
// docs/superpowers/specs/2026-07-07-arena-7b-block-escape-decision-design.md.
// Each row: a Goo source string -> parse_input -> type_check_program
// (ignore rc; needed to populate FuncLitNode.captured_names exactly the
// way the real compiler pipeline does, per row 8's closure-capture case) ->
// param_escape_analyze (7a summaries) -> block_escape_analyze (7b, this
// task) -> assert decisions[i].escapes_block in source order.
//
// All 15 source strings were verified to parse successfully against this
// front-end before this table was written (see task report) — no rewrites
// were needed. Row 7 deliberately calls an unregistered/external function
// (fmt.Println) and so is expected to FAIL type-checking (undefined
// "fmt") — that's fine, we only need it to parse; the escape analyses
// operate on the AST regardless of type-check outcome, and the whole
// point of row 7 is exercising the external/unregistered-callee
// retain-all rule (sink #5).
//
// Rows 2, 3, 4, 6, 7, 8, 9, 11, 12, 15, and `b` in row 10 are the
// load-bearing "must be true" (soundness) rows — an unsound implementation
// shows up as a wrong `false` there. Rows 1, 5, 13, and `a` in row 10 are
// the "must be false" (precision) rows — a lazy mark-everything impl fails
// those; they prove the arena actually delivers a benefit.

#include "parser.h"
#include "ast.h"
#include "types.h"
#include "param_escape.h"
#include "block_escape.h"
#include "../goo_check.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_EXPECT_SITES 4

typedef struct {
    int         row;
    const char* description;
    const char* src;
    size_t      expected_count;                    // expected decisions produced, source order
    bool        expected_escapes[MAX_EXPECT_SITES];
} TestRow;

static TestRow rows[] = {
    {
        1, "dies in block (arena-eligible) -> false",
        "package main\n"
        "func f() {\n"
        "    arena {\n"
        "        x := new(int)\n"
        "        _ = x\n"
        "    }\n"
        "}\n",
        1, { false }
    },
    {
        2, "returned from the block's function -> true",
        "package main\n"
        "func f() *int {\n"
        "    arena {\n"
        "        return new(int)\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        3, "stored to a pre-block outer local -> true",
        "package main\n"
        "func f() {\n"
        "    var keep *int\n"
        "    arena {\n"
        "        keep = new(int)\n"
        "    }\n"
        "    _ = keep\n"
        "}\n",
        1, { true }
    },
    {
        4, "stored to a package global -> true",
        "package main\n"
        "var g *int\n"
        "func f() {\n"
        "    arena {\n"
        "        g = new(int)\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        5, "passed to a non-retaining callee -> false",
        "package main\n"
        "func sink(x *int) {\n"
        "}\n"
        "func f() {\n"
        "    arena {\n"
        "        p := new(int)\n"
        "        sink(p)\n"
        "    }\n"
        "}\n",
        1, { false }
    },
    {
        6, "passed to a retaining callee -> true",
        "package main\n"
        "var g *int\n"
        "func stash(x *int) {\n"
        "    g = x\n"
        "}\n"
        "func f() {\n"
        "    arena {\n"
        "        stash(new(int))\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        7, "passed to a non-whitelisted external/unregistered callee -> true",
        "package main\n"
        "func f() {\n"
        "    arena {\n"
        "        stash(new(int))\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        // TWO sites since closure environments became a site kind (phase 1a):
        // the `new(int)` (escapes by sink #3, capture) and the capturing
        // literal's own environment (escapes by sink #4, the `go`).
        8, "captured by a closure -> true (and the env escapes via go)",
        "package main\n"
        "func use(p *int) {\n"
        "}\n"
        "func f() {\n"
        "    arena {\n"
        "        p := new(int)\n"
        "        go func() {\n"
        "            use(p)\n"
        "        }()\n"
        "    }\n"
        "}\n",
        2, { true, true }
    },
    {
        9, "goroutine argument -> true (independent of callee's own summary)",
        "package main\n"
        "func g(x *int) {\n"
        "}\n"
        "func f() {\n"
        "    arena {\n"
        "        go g(new(int))\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        10, "two sites, one returned -> a=false, b=true",
        "package main\n"
        "func f() *int {\n"
        "    arena {\n"
        "        a := new(int)\n"
        "        b := new(int)\n"
        "        _ = a\n"
        "        return b\n"
        "    }\n"
        "}\n",
        2, { false, true }
    },
    {
        11, "escapes through a local alias -> true",
        "package main\n"
        "func f() *int {\n"
        "    arena {\n"
        "        x := new(int)\n"
        "        y := x\n"
        "        return y\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        12, "store through a function-param pointer -> true",
        "package main\n"
        "func f(out **int) {\n"
        "    arena {\n"
        "        *out = new(int)\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        13, "block-local composite literal sites, both die -> false, false",
        "package main\n"
        "type Node struct {\n"
        "    next *Node\n"
        "}\n"
        "func f() {\n"
        "    arena {\n"
        "        n := &Node{}\n"
        "        m := &Node{next: n}\n"
        "        _ = m\n"
        "    }\n"
        "}\n",
        2, { false, false }
    },
    {
        14, "site outside any arena block -> not recorded",
        "package main\n"
        "func f() {\n"
        "    x := new(int)\n"
        "    _ = x\n"
        "}\n",
        0, { false }
    },
    {
        15, "conditional store to an outer var -> true",
        "package main\n"
        "func f(cond bool) {\n"
        "    var keep *int\n"
        "    arena {\n"
        "        p := new(int)\n"
        "        if cond {\n"
        "            keep = p\n"
        "        }\n"
        "    }\n"
        "    _ = keep\n"
        "}\n",
        1, { true }
    },
    {
        // A `defer`'d call runs at the enclosing FUNCTION's exit, which is
        // AFTER this arena block frees its arena — so the deferred call's
        // arguments outlive the block and must escape, like a goroutine arg.
        // `sink` is genuinely non-retaining (it derefs q into the blank
        // discard, never stashing q), so the ONLY reason p escapes is the
        // defer treatment — this row fails if handle_defer_call regresses to
        // the ordinary-call (retention-based) handling that caused a
        // use-after-free (see examples/arena_defer_escape_probe.goo).
        16, "passed to a deferred call -> true (defer fires past the block)",
        "package main\n"
        "func sink(q *int) { _ = *q }\n"
        "func f() {\n"
        "    arena {\n"
        "        p := new(int)\n"
        "        defer sink(p)\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        // A value sent on a channel leaves the block — a receiver reads it
        // after the arena is freed. `ch <- p` is a BinaryExprNode with the
        // ARROW operator; before the fix walk_stmt only handled assign
        // operators here, so the send fell through to a discarded taint and
        // p was wrongly kept in the arena (a use-after-free — see
        // examples/arena_chan_send_probe.goo).
        17, "sent on a channel -> true (receiver reads it past the block)",
        "package main\n"
        "func f(ch chan *int) {\n"
        "    arena {\n"
        "        p := new(int)\n"
        "        ch <- p\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        // THE SAME SEND, INSIDE A SELECT CASE. Row 17 covers `ch <- p` as an
        // expression statement. This one covers it as a select case's comm
        // clause, which reaches the sink by a DIFFERENT path: comm is an
        // EXPRESSION, so the select arm routes it through escape_walk_expr_stmt
        // rather than escape_walk_stmt.
        //
        // Before that arm was fixed this row passed for the wrong reason --
        // escape_walk_stmt sent the expression to `default:`, which calls
        // escape_mark_all and marks everything. Pinning the send in its own row
        // is what stops a future narrowing of the select arm from silently
        // dropping the sink.
        33, "sent on a channel inside a SELECT case -> true (same sink, other path)",
        "package main\n"
        "func f(ch chan *int) {\n"
        "    arena {\n"
        "        p := new(int)\n"
        "        select {\n"
        "        case ch <- p:\n"
        "        }\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        // 7a' non-retaining whitelist: fmt.Println does not retain its args, so
        // an arena value passed ONLY to it does not escape the block and stays
        // arena-eligible (was `true` under the pure-conservative external rule).
        // Codegen-inert today (fmt.Println(*int) does not type-check yet), but
        // the analysis decision is exercised here.
        18, "passed only to fmt.Println -> false (7a' whitelist, arena-eligible)",
        "package main\n"
        "func f() {\n"
        "    arena {\n"
        "        p := new(int)\n"
        "        fmt.Println(p)\n"
        "    }\n"
        "}\n",
        1, { false }
    },
    {
        // The go-callee hole. handle_go_call marked a goroutine's ARGUMENTS
        // escaping but computed the CALLEE's taint and discarded it, so a
        // receiver reached by `go p.m()` stayed arena-eligible while the
        // goroutine held it. The call carries no arguments, so nothing else
        // could have saved it. Verified at the IR level before the fix: the
        // new(T) emitted goo_arena_alloc.
        19, "go p.m() receiver -> true (callee taint escapes, not just args)",
        "package main\n"
        "type T struct { x int }\n"
        "func (t *T) m() { _ = t.x }\n"
        "func f() {\n"
        "    arena {\n"
        "        p := new(T)\n"
        "        go p.m()\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        // Same hole through an IDENTIFIER callee, which the old code did not
        // even compute a taint for. Harmless while only new(T)/&composite are
        // sites, but it stops being harmless the moment closure environments
        // become sites — `go f()` on a local holding a closure takes exactly
        // this path. Pinned now so the fix cannot be reverted quietly.
        20, "go through a value reached from an arena alloc -> true",
        "package main\n"
        "type B struct { fn func() }\n"
        "func f() {\n"
        "    arena {\n"
        "        b := new(B)\n"
        "        b.fn = func() {}\n"
        "        go b.fn()\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        // Over-marking guard for the same fix: an ORDINARY call (no `go`) to a
        // whitelisted non-retaining function must stay arena-eligible. The
        // callee-taint change must not leak into the plain-call path.
        21, "plain call is unaffected by the go-callee fix -> false",
        "package main\n"
        "func f() {\n"
        "    arena {\n"
        "        p := new(int)\n"
        "        fmt.Println(p)\n"
        "    }\n"
        "}\n",
        1, { false }
    },

    // ---- Phase 1a: closure environments are an allocation site ------------
    //
    // A capturing func literal allocates an environment struct
    // (function_codegen.c's `env_ptr`). It escapes its arena block EXACTLY
    // when the closure value does, so the existing taint engine decides it
    // with no new concept. Rows 22-29 are the matrix
    // docs/superpowers/specs/2026-07-28-arena-site-widening-findings.md
    // requires: one precision row, one row per escape sink, a non-capturing
    // guard, and a row pinning that sink #3 was NOT relaxed.
    {
        // PRECISION. The environment is built, called, and dropped inside the
        // block. Calling a closure does not leak its environment — a body can
        // read the captured cells but cannot store the env itself — so
        // call_taint's discard of an identifier callee's taint is correct
        // here. A lazy mark-everything implementation fails this row, and so
        // does one that sets the self-bit BEFORE mark_escapes.
        22, "capturing closure dies in the block -> false (arena-eligible)",
        "package main\n"
        "func f() {\n"
        "    arena {\n"
        "        n := 1\n"
        "        c := func() { _ = n }\n"
        "        c()\n"
        "    }\n"
        "}\n",
        1, { false }
    },
    {
        // Sink #1: returned from the block's function.
        23, "capturing closure returned -> true",
        "package main\n"
        "func f() func() {\n"
        "    arena {\n"
        "        n := 1\n"
        "        return func() { _ = n }\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        // Sink #2: stored to a pre-block outer local, read after the block.
        24, "capturing closure stored to an outer local -> true",
        "package main\n"
        "func f() {\n"
        "    var keep func()\n"
        "    arena {\n"
        "        n := 1\n"
        "        keep = func() { _ = n }\n"
        "    }\n"
        "    keep()\n"
        "}\n",
        1, { true }
    },
    {
        // Sink #2 through a package global — a strictly longer-lived location
        // than the enclosing frame.
        25, "capturing closure stored to a global -> true",
        "package main\n"
        "var g func()\n"
        "func f() {\n"
        "    arena {\n"
        "        n := 1\n"
        "        g = func() { _ = n }\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        // Sink #4: the goroutine runs past the block. This is the exact path
        // row 20 pinned in advance — `go c()` on a LOCAL holding a closure
        // reaches the environment only through handle_go_call's callee taint,
        // because the call carries no arguments.
        26, "capturing closure launched with go -> true (callee taint)",
        "package main\n"
        "func f() {\n"
        "    arena {\n"
        "        n := 1\n"
        "        c := func() { _ = n }\n"
        "        go c()\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        // Sink #5: a call argument the callee retains.
        27, "capturing closure passed to a retaining callee -> true",
        "package main\n"
        "var g func()\n"
        "func stash(fn func()) {\n"
        "    g = fn\n"
        "}\n"
        "func f() {\n"
        "    arena {\n"
        "        n := 1\n"
        "        stash(func() { _ = n })\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        // A literal with NO captures allocates no environment, so registering
        // it as a site would record a decision with nothing behind it. Guards
        // the `captured_count > 0` condition in discover_expr.
        28, "non-capturing closure is not a site -> not recorded",
        "package main\n"
        "func f() {\n"
        "    arena {\n"
        "        c := func() { }\n"
        "        c()\n"
        "    }\n"
        "}\n",
        0, { false }
    },
    {
        // Sink #3 is NOT relaxed by this arc. `p` is captured, so `p` escapes
        // even though the closure holding it dies in the block. Both facts in
        // one row: decisions[0] (the new(int)) true, decisions[1] (the env)
        // false. Setting the environment's self-bit BEFORE its mark_escapes
        // call flips decisions[1] to true and fails here.
        29, "captured site escapes, its closure env does not -> true, false",
        "package main\n"
        "func f() {\n"
        "    arena {\n"
        "        p := new(int)\n"
        "        c := func() { _ = p }\n"
        "        c()\n"
        "    }\n"
        "}\n",
        2, { true, false }
    },
    {
        30, "method call on the site, receiver retained by the method -> true",
        // Soundness row. The callee is a SELECTOR expr, so the summary lookup
        // (which needs an AST_IDENTIFIER) misses and `callee` stays NULL. The
        // receiver is not an entry in call->args, so the retain-all rule for
        // an unresolved callee never reaches it. Before the fix the receiver
        // taint was computed and freed, leaving this site arena-eligible while
        // the method body stored it in a global: a use-after-free once the
        // block frees the arena. Same hole handle_go_call already closed for
        // `go p.m()`.
        "package main\n"
        "type T struct { x int }\n"
        "var g *T\n"
        "func (t *T) stash() {\n"
        "    g = t\n"
        "}\n"
        "func f() {\n"
        "    arena {\n"
        "        p := &T{x: 1}\n"
        "        p.stash()\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        31, "method call on the site, method does NOT retain -> still true",
        // Precision cost of the conservative fix, pinned deliberately rather
        // than left to drift. An unresolved callee cannot be proven
        // non-retaining, so ANY method call on a site marks it. Recovering
        // this row needs receiver-type-qualified summary keys: the registry
        // keys on the bare name and returns the first match, so resolving
        // `p.touch()` by name alone can find a different type's `touch`.
        "package main\n"
        "type T struct { x int }\n"
        "func (t *T) touch() {\n"
        "}\n"
        "func f() {\n"
        "    arena {\n"
        "        p := &T{x: 1}\n"
        "        p.touch()\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        32, "site allocated inside a `for i := 0; i < 3; i++` loop -> false",
        // PRECISION row, the block-granularity half of the ledger item PR #255
        // opened. Deleting the AST_POSTFIX_EXPR arm from the shared engine left
        // all 31 rows of this table green. `--reach` gives the stronger cause:
        // ZERO postfix nodes across every fixture in this file.
        //
        // This is also the commonest arena shape there is -- allocate per
        // iteration, let the block reclaim -- so its absence mattered more here
        // than the row count suggests.
        //
        // The teeth: without the arm the postfix falls to the default arm,
        // which marks every SITE escaping, so this reads true and the row
        // fails. See docs/adr/0002-measurements/escape_arm_coverage.md.
        "package main\n"
        "func f() {\n"
        "    arena {\n"
        "        for i := 0; i < 3; i++ {\n"
        "            x := new(int)\n"
        "            _ = x\n"
        "        }\n"
        "    }\n"
        "}\n",
        1, { false }
    },
    {
        33, "site escapes THROUGH a callee that returns its own parameter -> true",
        // SOUNDNESS. This row was written to isolate block_callee_retention's
        // `*out_return_escapes = callee->return_escapes`, and MEASUREMENT SHOWED
        // IT CANNOT. The record is kept here because the negative result is what
        // justifies scripts/escape_teeth.sh carrying no such entry.
        //
        // Why no fixture can isolate that field: sink #1 (AST_RETURN_STMT) marks
        // the returned value before on_return records the signal, so `id` comes
        // out with escapes[0] AND return_escapes both true -- param_escape_test
        // row 26 asserts exactly that. At the call site `retains` reads
        // escapes[0], already true, so `x` is marked by sink #5 whatever
        // return_escapes says. Forcing the field false leaves this row green.
        //
        // The row is still worth its place: it covers a site leaving the block
        // through a CALL RESULT rather than a direct store, and it fails with
        // the rest when the site-source or site-slot machinery breaks.
        "package main\n"
        "var g *int\n"
        "func id(p *int) *int {\n"
        "    return p\n"
        "}\n"
        "func f() {\n"
        "    arena {\n"
        "        x := new(int)\n"
        "        g = id(x)\n"
        "    }\n"
        "}\n",
        1, { true }
    },
};



// Recursively finds the first `new(...)` call expression reachable from a
// statement list — used only by row 14, to obtain a real (but
// un-recorded, since it lies outside every arena block) site node to
// exercise block_escape_site_escapes's miss contract against something
// more specific than a bare NULL. Deliberately independent of
// src/types/block_escape.c's own internal is_new_call (test should not
// rely on the module's internals to find its own blind spot).
static ASTNode* find_first_new_call(ASTNode* node) {
    if (!node) return NULL;
    for (ASTNode* n = node; n; n = n->next) {
        switch (n->type) {
            case AST_CALL_EXPR: {
                CallExprNode* call = (CallExprNode*)n;
                if (call->function && call->function->type == AST_IDENTIFIER &&
                    strcmp(((IdentifierNode*)call->function)->name, "new") == 0) {
                    return n;
                }
                break;
            }
            default:
                break;
        }
        // Descend into the shapes this test's own sources actually use:
        // function bodies, blocks, var decls, expr stmts.
        ASTNode* found = NULL;
        switch (n->type) {
            case AST_PROGRAM:
                found = find_first_new_call(((ProgramNode*)n)->decls);
                break;
            case AST_FUNC_DECL:
                found = find_first_new_call(((FuncDeclNode*)n)->body);
                break;
            case AST_BLOCK_STMT:
                found = find_first_new_call(((BlockStmtNode*)n)->statements);
                break;
            case AST_EXPR_STMT:
                found = find_first_new_call(((ExprStmtNode*)n)->expr);
                break;
            case AST_VAR_DECL:
                found = find_first_new_call(((VarDeclNode*)n)->values);
                break;
            default:
                break;
        }
        if (found) return found;
    }
    return NULL;
}

int main(void) {
    size_t n = sizeof(rows) / sizeof(rows[0]);
    goo_check_expect((int)n + 1);

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
        // populated exactly the way the compiler pipeline populates it.
        // We ignore the return code: row 7's source calls an unregistered
        // external function and is EXPECTED to fail type-checking, but
        // that must not stop either escape analysis from running on the
        // resulting (partially-checked) AST.
        TypeChecker* checker = type_checker_new();
        if (checker) {
            type_check_program(checker, ast_root);
        }

        ParamEscapeResult* summaries = param_escape_analyze(ast_root);
        if (!summaries) {
            goo_check(false, "param_escape_analyze returned NULL (allocation failure)");
        } else {
            BlockEscapeResult* result = block_escape_analyze(ast_root, summaries);
            if (!result) {
                goo_check(false, "block_escape_analyze returned NULL (allocation failure)");
            } else {
                char ctxbuf[256];

                snprintf(ctxbuf, sizeof(ctxbuf), "row %d: decision count == %zu (got %zu)",
                         row->row, row->expected_count, result->count);
                goo_check(result->count == row->expected_count, ctxbuf);

                size_t check_n = row->expected_count < result->count ? row->expected_count : result->count;
                for (size_t i = 0; i < check_n; i++) {
                    snprintf(ctxbuf, sizeof(ctxbuf),
                             "row %d: decisions[%zu].escapes_block == %s (got %s)",
                             row->row, i,
                             row->expected_escapes[i] ? "true" : "false",
                             result->decisions[i].escapes_block ? "true" : "false");
                    goo_check(result->decisions[i].escapes_block == row->expected_escapes[i], ctxbuf);

                    // Cross-check the lookup helper agrees with the
                    // decisions array it was derived from.
                    bool via_helper = block_escape_site_escapes(result, result->decisions[i].site);
                    snprintf(ctxbuf, sizeof(ctxbuf),
                             "row %d: block_escape_site_escapes(decisions[%zu].site) matches decisions[%zu].escapes_block",
                             row->row, i, i);
                    goo_check(via_helper == result->decisions[i].escapes_block, ctxbuf);
                }

                if (row->row == 14) {
                    // The load-bearing miss case: a real site node that
                    // exists in the AST but was never recorded because it
                    // lies outside every arena block.
                    ASTNode* outside_site = find_first_new_call(ast_root);
                    snprintf(ctxbuf, sizeof(ctxbuf), "row %d: found the out-of-arena new(int) node to probe", row->row);
                    goo_check(outside_site != NULL, ctxbuf);
                    if (outside_site) {
                        bool escapes = block_escape_site_escapes(result, outside_site);
                        snprintf(ctxbuf, sizeof(ctxbuf),
                                 "row %d: block_escape_site_escapes(un-recorded out-of-arena site) == true (conservative miss)",
                                 row->row);
                        goo_check(escapes == true, ctxbuf);
                    }
                }

                block_escape_result_free(result);
            }
            param_escape_result_free(summaries);
        }

        if (checker) type_checker_free(checker);
        ast_node_free(ast_root);
        ast_root = NULL;

    }

    // Conservative-miss contract for an unknown/NULL site, independent of
    // any specific row's AST (block_escape_analyze(NULL,...) yields a
    // valid, empty result per the header contract). Counted as one more row
    // than the table holds -- goo_check_expect() above says (int)n + 1 for
    // exactly this section, so dropping it reports BROKEN rather than a
    // quietly smaller test.
    goo_check_row((int)n + 1, "conservative-miss contract for an unknown or NULL site");
    {
        BlockEscapeResult* empty = block_escape_analyze(NULL, NULL);
        goo_check(empty != NULL, "block_escape_analyze(NULL, NULL) returns a valid (non-NULL) empty result");
        if (empty) {
            goo_check(empty->count == 0, "block_escape_analyze(NULL, NULL) result has count == 0");
            goo_check(block_escape_site_escapes(empty, NULL) == true,
                  "block_escape_site_escapes(_, NULL) == true (conservative miss)");
            // A bogus/unknown site pointer (never produced by any
            // analysis) must also miss conservatively -- use the address
            // of a local as an arbitrary non-NULL, definitely-unregistered
            // "ASTNode*".
            ASTNode bogus;
            memset(&bogus, 0, sizeof(bogus));
            goo_check(block_escape_site_escapes(empty, &bogus) == true,
                  "block_escape_site_escapes(_, <unknown node>) == true (conservative miss)");
            block_escape_result_free(empty);
        }
    }

    return goo_check_done("block-escape-test");
}
