// M11-types-const-spike: comptime engine reachability test.
//
// Goal: determine empirically whether src/comptime/'s evaluation engine
// (~6,200 LOC, currently unreachable from the normal compile pipeline)
// can evaluate the kinds of expressions M11's MVP demo needs. Four
// tiers of escalating difficulty isolate the engine's capability ceiling.
//
// Build (direct; the Makefile's comptime_test target at line 928 is
// broken — missing src/types/types.c — and a fix is intentionally
// deferred to M11-types-const-integrate or a Makefile-cleanup task):
//
//   gcc -std=c23 -Iinclude -DLLVM_AVAILABLE=0 \
//       -o /tmp/spike \
//       tests/unit/comptime/test_engine_reachability.c \
//       src/comptime/comptime.c \
//       src/comptime/comptime_types.c \
//       src/ast/ast.c \
//       src/lexer/lexer.c \
//       src/lexer/token.c \
//       src/parser/parser.tab.c \
//       src/parser/lexer_bridge.c \
//       src/parser/parser_errors.c \
//       src/types/types.c \
//       src/errors/error.c \
//       src/errors/ergonomic_errors.c
//
// (Iterate the link line as undefined-symbol errors appear; record
// the final form in docs/COMPTIME_AUDIT.md so the integrate task
// knows the engine's transitive dependency set.)
//
// Exits 0 iff all four tiers PASS. The Tier 4 result is the M11
// MVP blocker — if fib(10) doesn't evaluate, the MVP needs scope
// adjustment.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ast.h"
#include "comptime.h"
#include "parser.h"

// parser.h declares this as extern ASTNode* ast_root; defined in
// parser.tab.c via Bison.

static ConstDeclNode* find_first_const(ProgramNode* prog) {
    for (ASTNode* d = prog->decls; d; d = d->next) {
        if (d->type == AST_CONST_DECL) return (ConstDeclNode*)d;
    }
    return NULL;
}

static FuncDeclNode* find_func(ProgramNode* prog, const char* name) {
    for (ASTNode* d = prog->decls; d; d = d->next) {
        if (d->type == AST_FUNC_DECL) {
            FuncDeclNode* f = (FuncDeclNode*)d;
            if (f->name && strcmp(f->name, name) == 0) return f;
        }
    }
    return NULL;
}

static int run_tier(const char* label,
                    const char* source,
                    const char* const* fn_names,
                    size_t fn_count,
                    int64_t want) {
    printf("\n== %s ==\n", label);

    if (parse_input(source, "<spike>") != 0 || !ast_root) {
        printf("  FAIL: parse failed\n");
        return 0;
    }
    if (ast_root->type != AST_PROGRAM) {
        printf("  FAIL: ast_root type=%d (want AST_PROGRAM=%d)\n",
               ast_root->type, AST_PROGRAM);
        return 0;
    }
    ProgramNode* prog = (ProgramNode*)ast_root;

    ConstDeclNode* cdecl = find_first_const(prog);
    if (!cdecl || !cdecl->values) {
        printf("  FAIL: no comptime const found in AST\n");
        return 0;
    }
    printf("  parsed; comptime const RHS node type=%d\n", cdecl->values->type);

    ComptimeContext* ctx = comptime_context_new(NULL);
    if (!ctx) {
        printf("  FAIL: comptime_context_new returned NULL\n");
        return 0;
    }

    for (size_t i = 0; i < fn_count; i++) {
        FuncDeclNode* fn = find_func(prog, fn_names[i]);
        if (!fn) {
            printf("  FAIL: function %s not found in AST\n", fn_names[i]);
            comptime_context_free(ctx);
            return 0;
        }
        if (!comptime_context_bind_func(ctx, fn_names[i], (ASTNode*)fn)) {
            printf("  FAIL: comptime_context_bind_func(%s) returned false\n",
                   fn_names[i]);
            comptime_context_free(ctx);
            return 0;
        }
        printf("  bound function: %s\n", fn_names[i]);
    }

    ComptimeResult* res = comptime_eval_expression(ctx, cdecl->values);
    if (!res) {
        printf("  FAIL: comptime_eval_expression returned NULL\n");
        comptime_context_free(ctx);
        return 0;
    }
    if (!res->value) {
        printf("  FAIL: result has null value (likely engine error)\n");
        comptime_result_free(res);
        comptime_context_free(ctx);
        return 0;
    }
    if (res->value->type != COMPTIME_VALUE_INT) {
        printf("  FAIL: result type=%d (want COMPTIME_VALUE_INT=%d)\n",
               res->value->type, COMPTIME_VALUE_INT);
        comptime_result_free(res);
        comptime_context_free(ctx);
        return 0;
    }

    int64_t got = res->value->int_value;
    int ok = (got == want);
    printf("  %s: got=%lld want=%lld\n",
           ok ? "PASS" : "FAIL",
           (long long)got, (long long)want);

    comptime_result_free(res);
    comptime_context_free(ctx);
    return ok;
}

int main(void) {
    int passes = 0;
    const int total = 4;

    printf("=== M11-types-const-spike: comptime engine reachability ===\n");

    passes += run_tier(
        "Tier 1: literal 42",
        "package main\ncomptime const X int = 42\n",
        NULL, 0, 42);

    passes += run_tier(
        "Tier 2: arithmetic 1 + 2",
        "package main\ncomptime const X int = 1 + 2\n",
        NULL, 0, 3);

    const char* fib_only[] = {"fib"};

    passes += run_tier(
        "Tier 3: fib(2)",
        "package main\n"
        "func fib(n int) int {\n"
        "    if n < 2 { return n }\n"
        "    return fib(n-1) + fib(n-2)\n"
        "}\n"
        "comptime const X int = fib(2)\n",
        fib_only, 1, 1);

    passes += run_tier(
        "Tier 4: fib(10) — M11 MVP blocker",
        "package main\n"
        "func fib(n int) int {\n"
        "    if n < 2 { return n }\n"
        "    return fib(n-1) + fib(n-2)\n"
        "}\n"
        "comptime const X int = fib(10)\n",
        fib_only, 1, 55);

    printf("\n=== %d / %d tiers PASS ===\n", passes, total);
    return passes == total ? 0 : 1;
}
