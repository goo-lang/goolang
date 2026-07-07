// Arena leg — Task 7c: codegen_arena_eligible predicate test.
//
// 7c wires the 7b block-escape decision into the codegen_emit_alloc choke
// point via one new public predicate, codegen_arena_eligible(cg, alloc_site,
// kind). This is the testable seam: it touches only cg->arena_stack /
// cg->arena_depth / cg->block_escape — never the LLVM builder/module — so it
// can be exercised against a lightweight CodeGenerator built with a bare
// calloc(1, sizeof(CodeGenerator)) instead of the full codegen_new (which
// would require a real LLVM context/module/builder this test has no use
// for).
//
// Pipeline: parse -> type_check_program (so FuncLitNode.captured_names is
// populated the way the real compiler populates it, matching 7a/7b's own
// test setup) -> param_escape_analyze (7a) -> block_escape_analyze (7b) ->
// the assertions below against codegen_arena_eligible (7c).
//
// The source below is 7b's row 10 shape ("two sites, one returned") renamed
// to match the design doc's own worked example: `keep` is returned (escapes
// its arena block -> must stay off the arena, i.e. NOT arena-eligible) and
// `tmp` dies inside the block (arena-eligible). Table-driven per decision so
// this test would also cover a future richer fixture without restructuring.

#include "parser.h"
#include "ast.h"
#include "types.h"
#include "param_escape.h"
#include "block_escape.h"
#include "codegen.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char* SRC =
    "package main\n"
    "func f() *int {\n"
    "    arena {\n"
    "        keep := new(int)\n"
    "        tmp := new(int)\n"
    "        _ = tmp\n"
    "        return keep\n"
    "    }\n"
    "}\n";

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* label) {
    if (cond) {
        g_pass++;
        printf("    PASS: %s\n", label);
    } else {
        g_fail++;
        printf("    FAIL: %s\n", label);
    }
}

int main(void) {
    int parse_rc = parse_input(SRC, "test.goo");
    if (parse_rc != 0 || !ast_root) {
        printf("FAIL: parse_input failed (rc=%d, ast_root=%p)\n", parse_rc, (void*)ast_root);
        return 1;
    }

    // Real type checker so FuncLitNode.captured_names is populated exactly
    // like the compiler pipeline populates it (7a/7b precondition mirrored
    // here even though this fixture has no closures).
    TypeChecker* checker = type_checker_new();
    if (checker) {
        type_check_program(checker, ast_root);
    }

    ParamEscapeResult* pe = param_escape_analyze(ast_root);
    if (!pe) {
        printf("FAIL: param_escape_analyze returned NULL (allocation failure)\n");
        if (checker) type_checker_free(checker);
        ast_node_free(ast_root);
        return 1;
    }

    BlockEscapeResult* be = block_escape_analyze(ast_root, pe);
    param_escape_result_free(pe);
    if (!be) {
        printf("FAIL: block_escape_analyze returned NULL (allocation failure)\n");
        if (checker) type_checker_free(checker);
        ast_node_free(ast_root);
        return 1;
    }

    // Sanity-check the fixture itself before trusting the 7c assertions
    // built on top of it: exactly 2 sites, keep (index 0) escaping, tmp
    // (index 1) not.
    printf("=== Fixture sanity: block_escape_analyze over the arena{} fixture ===\n");
    char ctx[256];
    snprintf(ctx, sizeof(ctx), "decision count == 2 (got %zu)", be->count);
    check(be->count == 2, ctx);
    if (be->count == 2) {
        snprintf(ctx, sizeof(ctx), "decisions[0] (keep, returned) escapes_block == true (got %s)",
                 be->decisions[0].escapes_block ? "true" : "false");
        check(be->decisions[0].escapes_block == true, ctx);
        snprintf(ctx, sizeof(ctx), "decisions[1] (tmp, dies in block) escapes_block == false (got %s)",
                 be->decisions[1].escapes_block ? "true" : "false");
        check(be->decisions[1].escapes_block == false, ctx);
    }
    if (g_fail > 0) {
        printf("FAIL: fixture does not have the expected shape -- aborting before 7c assertions\n");
        block_escape_result_free(be);
        if (checker) type_checker_free(checker);
        ast_node_free(ast_root);
        printf("=================================================\n");
        printf("arena_routing_test summary: %d assertions passed, %d failed\n", g_pass, g_fail);
        return 1;
    }

    // Lightweight CodeGenerator: calloc only, NO codegen_new (no LLVM
    // context/module/builder). codegen_arena_eligible must not dereference
    // any of those fields.
    CodeGenerator* cg = calloc(1, sizeof(CodeGenerator));
    if (!cg) {
        printf("FAIL: calloc(CodeGenerator) failed\n");
        block_escape_result_free(be);
        if (checker) type_checker_free(checker);
        ast_node_free(ast_root);
        return 1;
    }
    cg->block_escape = be;

    // (a) Dummy arena pushed: the gate must mirror the analysis exactly --
    // non-escaping (false) -> arena-eligible (true); escaping (true) -> not
    // arena-eligible (false).
    printf("=== (a) arena pushed: eligible == !escapes_block for every decision ===\n");
    cg->arena_stack[0] = (LLVMValueRef)0x1;
    cg->arena_depth = 1;
    for (size_t i = 0; i < be->count; i++) {
        bool eligible = codegen_arena_eligible(cg, be->decisions[i].site, ALLOC_KIND_DEFAULT);
        bool expected = !be->decisions[i].escapes_block;
        snprintf(ctx, sizeof(ctx),
                 "decisions[%zu]: codegen_arena_eligible == %s (escapes_block=%s)",
                 i, expected ? "true" : "false",
                 be->decisions[i].escapes_block ? "true" : "false");
        check(eligible == expected, ctx);
    }

    // (b) Empty arena stack: nothing routes to the arena regardless of the
    // site's escape decision -- proves the inert-today property (the arena
    // stack really is empty for every program until Task 6 pushes one).
    printf("=== (b) arena_depth == 0: eligible == false for every decision ===\n");
    cg->arena_depth = 0;
    for (size_t i = 0; i < be->count; i++) {
        bool eligible = codegen_arena_eligible(cg, be->decisions[i].site, ALLOC_KIND_DEFAULT);
        snprintf(ctx, sizeof(ctx), "decisions[%zu]: codegen_arena_eligible == false with no active arena", i);
        check(eligible == false, ctx);
    }

    // (c) NULL site, with an arena active: block_escape_site_escapes's
    // conservative miss contract (TRUE on NULL) must make this ineligible
    // even though an arena is on the stack.
    printf("=== (c) NULL alloc_site (arena active) -> eligible == false ===\n");
    cg->arena_stack[0] = (LLVMValueRef)0x1;
    cg->arena_depth = 1;
    check(codegen_arena_eligible(cg, NULL, ALLOC_KIND_DEFAULT) == false,
          "codegen_arena_eligible(cg, NULL, ALLOC_KIND_DEFAULT) == false");

    // AllocKind has exactly one member (ALLOC_KIND_DEFAULT) today, so the
    // "non-default kind -> ineligible" case from the design doc has no
    // second kind to construct -- skipped per the doc's own "otherwise
    // skip" clause.

    free(cg);
    block_escape_result_free(be);
    if (checker) type_checker_free(checker);
    ast_node_free(ast_root);
    ast_root = NULL;

    printf("=================================================\n");
    printf("arena_routing_test summary: %d assertions passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
