#include "optimizer.h"
#include "ast.h"
#include "token.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } \
} while(0)
#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))
#define EXPECT_EQ(a, b) do { \
    if ((a) != (b)) { printf("  FAIL: %s != %s (line %d)\n", #a, #b, __LINE__); return 1; } \
} while(0)
#define EXPECT_NOT_NULL(p) do { \
    if ((p) == NULL) { printf("  FAIL: %s is NULL (line %d)\n", #p, __LINE__); return 1; } \
} while(0)

static Position pos0 = {0};

// =============================================================================
// Target Detection Tests
// =============================================================================

static int test_detect_host_target(void) {
    TargetInfo target = optimizer_detect_host_target();
    EXPECT_NOT_NULL(target.arch);
    EXPECT_NOT_NULL(target.os);
    EXPECT_TRUE(target.pointer_size == 4 || target.pointer_size == 8);
    EXPECT_EQ(target.cache_line_size, (size_t)64);
    EXPECT_TRUE(target.num_cores >= 1);
    return 0;
}

static int test_target_has_features(void) {
    TargetInfo target = optimizer_detect_host_target();

    // On x86_64 linux, we should have at least SSE
#if defined(__x86_64__)
    EXPECT_TRUE(comptime_target_has(&target, "sse"));
    EXPECT_TRUE(comptime_target_has(&target, "sse2"));
#endif

    // Unknown features should return false
    EXPECT_FALSE(comptime_target_has(&target, "quantum_compute"));
    EXPECT_FALSE(comptime_target_has(NULL, "sse"));

    return 0;
}

// =============================================================================
// Optimizer Lifecycle Tests
// =============================================================================

static int test_optimizer_new(void) {
    Optimizer* opt = optimizer_new(OPT_GOAL_THROUGHPUT);
    EXPECT_NOT_NULL(opt);
    EXPECT_EQ(opt->goals, (OptimizationGoal)OPT_GOAL_THROUGHPUT);
    EXPECT_NOT_NULL(opt->comptime);
    EXPECT_NOT_NULL(opt->passes);

    optimizer_free(opt);
    return 0;
}

static int test_optimizer_default_passes(void) {
    Optimizer* opt = optimizer_new(OPT_GOAL_THROUGHPUT);
    optimizer_add_default_passes(opt);

    // Should have several passes for throughput goal
    EXPECT_TRUE(opt->pass_count >= 4);

    // Check that specific passes are present
    bool has_constant_fold = false;
    bool has_inline = false;
    for (size_t i = 0; i < opt->pass_count; i++) {
        if (opt->passes[i].kind == OPT_PASS_CONSTANT_FOLD) has_constant_fold = true;
        if (opt->passes[i].kind == OPT_PASS_INLINE_SMALL) has_inline = true;
    }
    EXPECT_TRUE(has_constant_fold);
    EXPECT_TRUE(has_inline);

    optimizer_free(opt);
    return 0;
}

static int test_optimizer_memory_passes(void) {
    Optimizer* opt = optimizer_new(OPT_GOAL_MEMORY);
    optimizer_add_default_passes(opt);

    bool has_escape = false;
    for (size_t i = 0; i < opt->pass_count; i++) {
        if (opt->passes[i].kind == OPT_PASS_ESCAPE_BASED_ALLOC) has_escape = true;
    }
    EXPECT_TRUE(has_escape);

    optimizer_free(opt);
    return 0;
}

// =============================================================================
// Constant Folding Tests
// =============================================================================

static int test_constant_fold_binary(void) {
    Optimizer* opt = optimizer_new(OPT_GOAL_DEFAULT);

    // Create: 10 + 32
    LiteralNode* left = ast_literal_new(TOKEN_INT, "10", pos0);
    LiteralNode* right = ast_literal_new(TOKEN_INT, "32", pos0);
    BinaryExprNode* bin = ast_binary_expr_new((ASTNode*)left, TOKEN_PLUS, (ASTNode*)right, pos0);

    size_t folded = opt_constant_fold((ASTNode*)bin, opt->comptime);
    EXPECT_EQ(folded, (size_t)1);
    EXPECT_EQ(opt->comptime->stats.expressions_evaluated, (size_t)3); // 2 literals + 1 binary

    ast_node_free((ASTNode*)bin);
    optimizer_free(opt);
    return 0;
}

// =============================================================================
// Dead Code Elimination Tests
// =============================================================================

static int test_dce_true_condition(void) {
    // if (true) { then } else { else_branch }
    // -> else branch should be eliminated
    LiteralNode* cond = ast_literal_new(TOKEN_TRUE, "true", pos0);
    ASTNode* then_stmt = ast_node_new(AST_BLOCK_STMT, pos0);
    ASTNode* else_stmt = ast_node_new(AST_BLOCK_STMT, pos0);
    IfStmtNode* if_stmt = ast_if_stmt_new((ASTNode*)cond, then_stmt, else_stmt, pos0);

    size_t eliminated = opt_dead_code_eliminate((ASTNode*)if_stmt);
    EXPECT_EQ(eliminated, (size_t)1);
    EXPECT_TRUE(if_stmt->else_stmt == NULL);

    ast_node_free((ASTNode*)if_stmt);
    return 0;
}

// =============================================================================
// Annotation Parsing Tests
// =============================================================================

static int test_parse_optimize_for_throughput(void) {
    OptimizationGoal goal;
    EXPECT_TRUE(optimizer_parse_optimize_for("throughput", &goal));
    EXPECT_EQ(goal, OPT_GOAL_THROUGHPUT);
    return 0;
}

static int test_parse_optimize_for_memory(void) {
    OptimizationGoal goal;
    EXPECT_TRUE(optimizer_parse_optimize_for("memory", &goal));
    EXPECT_EQ(goal, OPT_GOAL_MEMORY);
    return 0;
}

static int test_parse_optimize_for_invalid(void) {
    OptimizationGoal goal;
    EXPECT_FALSE(optimizer_parse_optimize_for("invalid", &goal));
    return 0;
}

// =============================================================================
// Type Intrinsics Tests
// =============================================================================

static int test_type_intrinsics(void) {
    EXPECT_EQ(comptime_sizeof_type(NULL), (size_t)0);
    EXPECT_EQ(comptime_alignof_type(NULL), (size_t)0);
    EXPECT_FALSE(comptime_type_is_numeric(NULL));
    EXPECT_FALSE(comptime_type_is_vectorizable(NULL));
    return 0;
}

// =============================================================================
// Test Runner
// =============================================================================

typedef struct { const char* name; int (*func)(void); } TestCase;

int main(void) {
    TestCase tests[] = {
        {"detect_host_target",          test_detect_host_target},
        {"target_has_features",         test_target_has_features},
        {"optimizer_new",               test_optimizer_new},
        {"optimizer_default_passes",    test_optimizer_default_passes},
        {"optimizer_memory_passes",     test_optimizer_memory_passes},
        {"constant_fold_binary",        test_constant_fold_binary},
        {"dce_true_condition",          test_dce_true_condition},
        {"parse_optimize_for_throughput", test_parse_optimize_for_throughput},
        {"parse_optimize_for_memory",   test_parse_optimize_for_memory},
        {"parse_optimize_for_invalid",  test_parse_optimize_for_invalid},
        {"type_intrinsics",             test_type_intrinsics},
    };

    size_t total = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;

    printf("Running %zu optimizer tests...\n\n", total);

    for (size_t i = 0; i < total; i++) {
        printf("[ RUN      ] optimizer.%s\n", tests[i].name);
        int result = tests[i].func();
        if (result == 0) {
            printf("[\033[32m       OK\033[0m ] optimizer.%s\n", tests[i].name);
            passed++;
        } else {
            printf("[\033[31m  FAILED  \033[0m ] optimizer.%s\n", tests[i].name);
        }
    }

    printf("\nTests run: %zu\n\033[32mPassed: %zu\033[0m\n", total, passed);
    if (passed < total) printf("\033[31mFailed: %zu\033[0m\n", total - passed);

    return passed < total ? 1 : 0;
}
