// P2.4 missing-return analysis: the terminating-statement predicate, alone.
//
// This is the first suite in this directory that does NOT link
// //src/types:frontend. stmt_is_terminating is a pure structural walk over the
// AST -- its own header says so -- so it needs the parser to build a tree and
// nothing else. The dependency list in tests/unit/types/BUILD is the assertion:
// if someone gives the predicate a TypeChecker dependency, this target stops
// linking, and that is the intended alarm.
//
// The fixtures are parsed and NEVER type-checked, deliberately. A body like
// `func f() int { }` is a type error and a perfectly good parse, and the
// predicate is what the type checker consults to decide that. Running the
// checker first would make the fixture's own diagnostic the thing under test.
//
// ROW 8 IS THE LOAD-BEARING ONE. terminating_stmt.c's header records that
// Go's break-freedom clause applies to for/switch/type-switch/select, that the
// design doc omitted it for switch, and that omitting it is UNSOUND. A `for {}`
// containing a `break` does not terminate. Delete row 8 and an implementation
// that ignores break passes the rest of this table.

#include "parser.h"
#include "ast.h"
#include "types.h"
#include "../goo_check.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    int         row;
    const char* description;
    const char* src;
    const char* fn;          // the function whose body is judged
    int         terminates;  // expected stmt_is_terminating result
} TestRow;

static TestRow rows[] = {
    {
        1, "a bare return terminates",
        "package main\n"
        "func f() int {\n"
        "    return 0\n"
        "}\n",
        "f", 1
    },
    {
        2, "a body with no return does not terminate",
        "package main\n"
        "func f() int {\n"
        "    x := 0\n"
        "    _ = x\n"
        "}\n",
        "f", 0
    },
    {
        3, "an empty body does not terminate",
        "package main\n"
        "func f() int {\n"
        "}\n",
        "f", 0
    },
    {
        4, "if/else terminates when BOTH arms do",
        "package main\n"
        "func f(c bool) int {\n"
        "    if c {\n"
        "        return 1\n"
        "    } else {\n"
        "        return 2\n"
        "    }\n"
        "}\n",
        "f", 1
    },
    {
        5, "if with no else does not terminate, however the arm ends",
        "package main\n"
        "func f(c bool) int {\n"
        "    if c {\n"
        "        return 1\n"
        "    }\n"
        "}\n",
        "f", 0
    },
    {
        6, "a for with no condition terminates",
        "package main\n"
        "func f() int {\n"
        "    for {\n"
        "        x := 1\n"
        "        _ = x\n"
        "    }\n"
        "}\n",
        "f", 1
    },
    {
        7, "a for WITH a condition does not terminate",
        "package main\n"
        "func f() int {\n"
        "    for i := 0; i < 3; i = i + 1 {\n"
        "        _ = i\n"
        "    }\n"
        "}\n",
        "f", 0
    },
    {
        // The soundness row. See the file header.
        8, "a for containing a break does NOT terminate",
        "package main\n"
        "func f() int {\n"
        "    for {\n"
        "        break\n"
        "    }\n"
        "}\n",
        "f", 0
    },
    {
        9, "a switch with a default whose every clause returns terminates",
        "package main\n"
        "func f(x int) int {\n"
        "    switch x {\n"
        "    case 1:\n"
        "        return 1\n"
        "    default:\n"
        "        return 2\n"
        "    }\n"
        "}\n",
        "f", 1
    },
    {
        10, "a switch with NO default does not terminate",
        "package main\n"
        "func f(x int) int {\n"
        "    switch x {\n"
        "    case 1:\n"
        "        return 1\n"
        "    }\n"
        "}\n",
        "f", 0
    },
    {
        // The same soundness clause as row 8, on the construct the design doc
        // left it off. A clause that ends in return but can also break out.
        11, "a switch whose clause can break out does NOT terminate",
        "package main\n"
        "func f(x int) int {\n"
        "    switch x {\n"
        "    case 1:\n"
        "        if x > 0 {\n"
        "            break\n"
        "        }\n"
        "        return 1\n"
        "    default:\n"
        "        return 2\n"
        "    }\n"
        "}\n",
        "f", 0
    },
    {
        12, "a panic call terminates",
        "package main\n"
        "func f() int {\n"
        "    panic(\"boom\")\n"
        "}\n",
        "f", 1
    },
    {
        13, "a nested block ending in return terminates",
        "package main\n"
        "func f() int {\n"
        "    {\n"
        "        return 1\n"
        "    }\n"
        "}\n",
        "f", 1
    },
    {
        // The relaxed rule: without a goto in the body, ANY terminating
        // statement in the list is enough, not only the last one. Row 14 and
        // terminating_stmt.c's block_terminates comment are the pair.
        14, "dead code after a return still terminates, with no goto present",
        "package main\n"
        "func f() int {\n"
        "    return 1\n"
        "    x := 2\n"
        "    _ = x\n"
        "}\n",
        "f", 1
    },
};

// The function body named by `fn`, or NULL when the program has no such
// function. Returning NULL rather than asserting keeps the "fixture is wrong"
// case separable from the "predicate is wrong" case.
static ASTNode* body_of(ASTNode* root, const char* fn) {
    if (!root || root->type != AST_PROGRAM) return NULL;
    ProgramNode* prog = (ProgramNode*)root;
    for (ASTNode* d = prog->decls; d; d = d->next) {
        if (d->type != AST_FUNC_DECL) continue;
        FuncDeclNode* f = (FuncDeclNode*)d;
        if (f->name && strcmp(f->name, fn) == 0) return f->body;
    }
    return NULL;
}

int main(void) {
    size_t nrows = sizeof(rows) / sizeof(rows[0]);
    goo_check_expect((int)nrows);

    for (size_t r = 0; r < nrows; r++) {
        TestRow* row = &rows[r];
        goo_check_row(row->row, row->description);

        char ctx[320];
        int parse_rc = parse_input(row->src, "test.goo");
        snprintf(ctx, sizeof(ctx), "row %d: the fixture parses (rc=%d)", row->row, parse_rc);
        goo_check(parse_rc == 0 && ast_root != NULL, ctx);
        if (parse_rc != 0 || !ast_root) {
            if (ast_root) { ast_node_free(ast_root); ast_root = NULL; }
            continue;
        }

        ASTNode* body = body_of(ast_root, row->fn);
        snprintf(ctx, sizeof(ctx), "row %d: the fixture declares func %s", row->row, row->fn);
        goo_check(body != NULL, ctx);

        if (body) {
            int got = stmt_is_terminating(body);
            snprintf(ctx, sizeof(ctx),
                     "row %d: stmt_is_terminating=%d, expected %d -- %s",
                     row->row, got, row->terminates, row->description);
            goo_check(got == row->terminates, ctx);
        }

        ast_node_free(ast_root);
        ast_root = NULL;
    }

    return goo_check_done("terminating_stmt");
}
