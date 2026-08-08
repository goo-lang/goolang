// ast_node_free() must free the WHOLE tree, not only the spine.
//
// WHY THIS EXISTS. The parser fuzzer (tests/fuzz/) found on 2026-08-08 that
// every assignment statement leaked 188 bytes, linear in the statement count.
// ast_node_free() had no `case AST_EXPR_STMT`, so the node fell to `default:`
// and nothing freed its `expr` child. src/ast/ast.c already carried a comment
// admitting that some node types leak their subtrees this way; this driver is
// what turns that admission into a number a gate can read.
//
// WHY IT IS A SEPARATE DRIVER rather than a check inside the compiler: the
// compiler is a batch process that exits, so it never has to free anything and
// a leak there is invisible. Only a caller that parses AND frees in one process
// can see the difference. The fuzz harness does exactly that, which is how the
// leak surfaced, but it needs clang and libFuzzer. This driver needs neither,
// so scripts/ast_free_leak_probe.sh can run it under valgrind with the ordinary
// gcc build and belong in verify-core.
//
// The driver parses and frees in a LOOP. One iteration would show the leak too,
// but a loop makes the arithmetic unambiguous: a per-statement leak scales with
// ITERATIONS * statements, while a one-off allocation the parser interns does
// not. That distinction is what stops this probe from failing on a fixed cost.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "parser.h"

extern ASTNode* ast_root;

#define ITERATIONS 50

// Six assignment statements. Each one is an `expression ASSIGN expression`
// reduction (src/parser/parser.y), which is the shape that leaked.
static const char* kSource =
    "package main\n"
    "func main() {\n"
    "\tx := 1\n"
    "\ty := 2\n"
    "\t_ = x\n"
    "\t_ = y\n"
    "\tx = y\n"
    "\ty = x\n"
    "\t_ = x\n"
    "\t_ = y\n"
    "}\n";

int main(void) {
    int parsed = 0;

    for (int i = 0; i < ITERATIONS; i++) {
        char* buf = strdup(kSource);
        if (!buf) {
            fprintf(stderr, "ast-free-leak: out of memory\n");
            return 2;
        }

        if (parse_input(buf, "ast_free_leak_test.goo") == 0 && ast_root) {
            parsed++;
            ast_node_free(ast_root);
        } else if (ast_root) {
            // Free it on the error path too. A leak that only appears when the
            // parse fails is still a leak, and the reject corpus is 156 files.
            ast_node_free(ast_root);
        }
        ast_root = NULL;
        free(buf);
    }

    // Instrument check. If the fixture stopped parsing -- a grammar change, a
    // renamed builtin -- every iteration would free nothing, valgrind would
    // report a clean run, and this probe would pass while measuring NOTHING.
    if (parsed != ITERATIONS) {
        fprintf(stderr,
                "ast-free-leak: BROKEN — parsed %d of %d iterations.\n"
                "  The fixture no longer parses, so a clean valgrind result\n"
                "  here would mean the tree was never built, not that it was\n"
                "  freed.\n",
                parsed, ITERATIONS);
        return 2;
    }

    printf("ast-free-leak: parsed and freed %d trees, %d assignment statements each\n",
           parsed, 6);
    return 0;
}
