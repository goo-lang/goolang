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
        7, "passed to an external/unregistered callee -> true",
        "package main\n"
        "func f() {\n"
        "    arena {\n"
        "        fmt.Println(new(int))\n"
        "    }\n"
        "}\n",
        1, { true }
    },
    {
        8, "captured by a closure -> true",
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
        1, { true }
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
            printf("    FAIL: param_escape_analyze returned NULL (allocation failure)\n");
            g_fail++;
        } else {
            BlockEscapeResult* result = block_escape_analyze(ast_root, summaries);
            if (!result) {
                printf("    FAIL: block_escape_analyze returned NULL (allocation failure)\n");
                g_fail++;
            } else {
                char ctxbuf[256];

                snprintf(ctxbuf, sizeof(ctxbuf), "row %d: decision count == %zu (got %zu)",
                         row->row, row->expected_count, result->count);
                check(result->count == row->expected_count, ctxbuf);

                size_t check_n = row->expected_count < result->count ? row->expected_count : result->count;
                for (size_t i = 0; i < check_n; i++) {
                    snprintf(ctxbuf, sizeof(ctxbuf),
                             "row %d: decisions[%zu].escapes_block == %s (got %s)",
                             row->row, i,
                             row->expected_escapes[i] ? "true" : "false",
                             result->decisions[i].escapes_block ? "true" : "false");
                    check(result->decisions[i].escapes_block == row->expected_escapes[i], ctxbuf);

                    // Cross-check the lookup helper agrees with the
                    // decisions array it was derived from.
                    bool via_helper = block_escape_site_escapes(result, result->decisions[i].site);
                    snprintf(ctxbuf, sizeof(ctxbuf),
                             "row %d: block_escape_site_escapes(decisions[%zu].site) matches decisions[%zu].escapes_block",
                             row->row, i, i);
                    check(via_helper == result->decisions[i].escapes_block, ctxbuf);
                }

                if (row->row == 14) {
                    // The load-bearing miss case: a real site node that
                    // exists in the AST but was never recorded because it
                    // lies outside every arena block.
                    ASTNode* outside_site = find_first_new_call(ast_root);
                    snprintf(ctxbuf, sizeof(ctxbuf), "row %d: found the out-of-arena new(int) node to probe", row->row);
                    check(outside_site != NULL, ctxbuf);
                    if (outside_site) {
                        bool escapes = block_escape_site_escapes(result, outside_site);
                        snprintf(ctxbuf, sizeof(ctxbuf),
                                 "row %d: block_escape_site_escapes(un-recorded out-of-arena site) == true (conservative miss)",
                                 row->row);
                        check(escapes == true, ctxbuf);
                    }
                }

                block_escape_result_free(result);
            }
            param_escape_result_free(summaries);
        }

        if (checker) type_checker_free(checker);
        ast_node_free(ast_root);
        ast_root = NULL;

        bool row_ok = (g_fail == before_fail);
        printf("  Row %d: %s\n\n", row->row, row_ok ? "PASS" : "FAIL");
        if (!row_ok) row_failed_count++;
    }

    // Conservative-miss contract for an unknown/NULL site, independent of
    // any specific row's AST (block_escape_analyze(NULL,...) yields a
    // valid, empty result per the header contract).
    {
        BlockEscapeResult* empty = block_escape_analyze(NULL, NULL);
        check(empty != NULL, "block_escape_analyze(NULL, NULL) returns a valid (non-NULL) empty result");
        if (empty) {
            check(empty->count == 0, "block_escape_analyze(NULL, NULL) result has count == 0");
            check(block_escape_site_escapes(empty, NULL) == true,
                  "block_escape_site_escapes(_, NULL) == true (conservative miss)");
            // A bogus/unknown site pointer (never produced by any
            // analysis) must also miss conservatively -- use the address
            // of a local as an arbitrary non-NULL, definitely-unregistered
            // "ASTNode*".
            ASTNode bogus;
            memset(&bogus, 0, sizeof(bogus));
            check(block_escape_site_escapes(empty, &bogus) == true,
                  "block_escape_site_escapes(_, <unknown node>) == true (conservative miss)");
            block_escape_result_free(empty);
        }
    }

    printf("=================================================\n");
    printf("block_escape_test summary: %d assertions passed, %d failed, %d/%zu rows failed\n",
           g_pass, g_fail, row_failed_count, n);

    return g_fail ? 1 : 0;
}
