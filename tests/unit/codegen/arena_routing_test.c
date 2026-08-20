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
#include "../goo_check.h"
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

// Four rows: the fixture sanity check, then (a), (b) and (c) below.
#define ARENA_ROUTING_ROWS 4

int main(void) {
    goo_check_expect(ARENA_ROUTING_ROWS);

    int parse_rc = parse_input(SRC, "test.goo");
    if (parse_rc != 0 || !ast_root) {
        printf("arena-routing-test: BROKEN -- parse_input failed (rc=%d, ast_root=%p).\n",
               parse_rc, (void*)ast_root);
        printf("  The fixture no longer parses, so nothing below tested codegen.\n");
        return 2;
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
        printf("arena-routing-test: BROKEN -- param_escape_analyze returned NULL.\n");
        if (checker) type_checker_free(checker);
        ast_node_free(ast_root);
        return 2;
    }

    BlockEscapeResult* be = block_escape_analyze(ast_root, pe);
    param_escape_result_free(pe);
    if (!be) {
        printf("arena-routing-test: BROKEN -- block_escape_analyze returned NULL.\n");
        if (checker) type_checker_free(checker);
        ast_node_free(ast_root);
        return 2;
    }

    // Sanity-check the fixture itself before trusting the 7c assertions
    // built on top of it: exactly 2 sites, keep (index 0) escaping, tmp
    // (index 1) not.
    goo_check_row(1, "fixture sanity: block_escape_analyze over the arena{} fixture");
    char ctx[256];
    snprintf(ctx, sizeof(ctx), "decision count == 2 (got %zu)", be->count);
    goo_check(be->count == 2, ctx);
    if (be->count == 2) {
        snprintf(ctx, sizeof(ctx), "decisions[0] (keep, returned) escapes_block == true (got %s)",
                 be->decisions[0].escapes_block ? "true" : "false");
        goo_check(be->decisions[0].escapes_block == true, ctx);
        snprintf(ctx, sizeof(ctx), "decisions[1] (tmp, dies in block) escapes_block == false (got %s)",
                 be->decisions[1].escapes_block ? "true" : "false");
        goo_check(be->decisions[1].escapes_block == false, ctx);
    }
    // Rows (a) to (c) all read this fixture's decisions. Against a fixture of
    // the wrong shape they measure nothing, so stop here and let the missing
    // rows report BROKEN rather than dress the fixture defect up as a codegen
    // failure.
    if (goo_check_failed()) {
        block_escape_result_free(be);
        if (checker) type_checker_free(checker);
        ast_node_free(ast_root);
        return goo_check_done("arena-routing-test");
    }

    // Lightweight CodeGenerator: calloc only, NO codegen_new (no LLVM
    // context/module/builder). codegen_arena_eligible must not dereference
    // any of those fields.
    CodeGenerator* cg = calloc(1, sizeof(CodeGenerator));
    if (!cg) {
        printf("arena-routing-test: BROKEN -- calloc(CodeGenerator) failed.\n");
        block_escape_result_free(be);
        if (checker) type_checker_free(checker);
        ast_node_free(ast_root);
        return 2;
    }
    cg->block_escape = be;

    // (a) Dummy arena pushed: the gate must mirror the analysis exactly --
    // non-escaping (false) -> arena-eligible (true); escaping (true) -> not
    // arena-eligible (false).
    goo_check_row(2, "(a) arena pushed: eligible == !escapes_block for every decision");
    cg->arena_stack[0] = (LLVMValueRef)0x1;
    cg->arena_depth = 1;
    for (size_t i = 0; i < be->count; i++) {
        bool eligible = codegen_arena_eligible(cg, be->decisions[i].site, ALLOC_KIND_DEFAULT);
        bool expected = !be->decisions[i].escapes_block;
        snprintf(ctx, sizeof(ctx),
                 "decisions[%zu]: codegen_arena_eligible == %s (escapes_block=%s)",
                 i, expected ? "true" : "false",
                 be->decisions[i].escapes_block ? "true" : "false");
        goo_check(eligible == expected, ctx);
    }

    // (b) Empty arena stack: nothing routes to the arena regardless of the
    // site's escape decision -- proves the inert-today property (the arena
    // stack really is empty for every program until Task 6 pushes one).
    goo_check_row(3, "(b) arena_depth == 0: eligible == false for every decision");
    cg->arena_depth = 0;
    for (size_t i = 0; i < be->count; i++) {
        bool eligible = codegen_arena_eligible(cg, be->decisions[i].site, ALLOC_KIND_DEFAULT);
        snprintf(ctx, sizeof(ctx), "decisions[%zu]: codegen_arena_eligible == false with no active arena", i);
        goo_check(eligible == false, ctx);
    }

    // (c) NULL site, with an arena active: block_escape_site_escapes's
    // conservative miss contract (TRUE on NULL) must make this ineligible
    // even though an arena is on the stack.
    goo_check_row(4, "(c) NULL alloc_site (arena active) -> eligible == false");
    cg->arena_stack[0] = (LLVMValueRef)0x1;
    cg->arena_depth = 1;
    goo_check(codegen_arena_eligible(cg, NULL, ALLOC_KIND_DEFAULT) == false,
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

    return goo_check_done("arena-routing-test");
}
