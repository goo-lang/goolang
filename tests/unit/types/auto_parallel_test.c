#include "auto_parallel.h"
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
// Lifecycle Tests
// =============================================================================

static int test_auto_parallel_new(void) {
    TargetInfo target = optimizer_detect_host_target();
    AutoParallelizer* ap = auto_parallel_new(&target);
    EXPECT_NOT_NULL(ap);
    EXPECT_EQ(ap->min_trip_count, (size_t)64);
    EXPECT_TRUE(ap->prefer_simd);

    auto_parallel_free(ap);
    return 0;
}

// =============================================================================
// Dependency Analysis Tests
// =============================================================================

static int test_no_dependencies(void) {
    // A simple literal has no variable writes -> no dependencies
    LiteralNode* val = ast_literal_new(TOKEN_INT, "2", pos0);

    DataDependency* deps = dependency_analyze_loop_body((ASTNode*)val, "i");
    EXPECT_TRUE(deps == NULL);
    EXPECT_FALSE(auto_parallel_has_loop_carried_deps(deps));

    ast_node_free((ASTNode*)val);
    return 0;
}

static int test_has_loop_carried_deps(void) {
    DataDependency dep = {
        .kind = DEP_FLOW,
        .variable = "sum",
        .is_loop_carried = true,
        .next = NULL,
    };

    EXPECT_TRUE(auto_parallel_has_loop_carried_deps(&dep));

    // Reduction dependency is not blocking
    dep.kind = DEP_REDUCTION;
    EXPECT_FALSE(auto_parallel_has_loop_carried_deps(&dep));
    return 0;
}

// =============================================================================
// Strategy Selection Tests
// =============================================================================

static int test_strategy_sequential(void) {
    LoopAnalysis analysis = {0};
    analysis.parallel_kind = LOOP_SEQUENTIAL;

    ParallelStrategy strategy = auto_parallel_choose_strategy(&analysis, NULL);
    EXPECT_EQ(strategy, PAR_STRATEGY_NONE);
    return 0;
}

static int test_strategy_parallel_for(void) {
    LoopAnalysis analysis = {0};
    analysis.parallel_kind = LOOP_PARALLEL;
    analysis.is_vectorizable = false;

    TargetInfo target = {0};
    ParallelStrategy strategy = auto_parallel_choose_strategy(&analysis, &target);
    EXPECT_EQ(strategy, PAR_STRATEGY_PARALLEL_FOR);
    return 0;
}

static int test_strategy_simd(void) {
    LoopAnalysis analysis = {0};
    analysis.parallel_kind = LOOP_PARALLEL;
    analysis.is_vectorizable = true;
    analysis.trip_count = 256;

    TargetInfo target = {0};
    target.has_avx2 = true;

    ParallelStrategy strategy = auto_parallel_choose_strategy(&analysis, &target);
    EXPECT_EQ(strategy, PAR_STRATEGY_SIMD);
    return 0;
}

static int test_strategy_reduction(void) {
    LoopAnalysis analysis = {0};
    analysis.parallel_kind = LOOP_REDUCTION;
    analysis.reduction = REDUCE_SUM;

    TargetInfo target = {0};
    ParallelStrategy strategy = auto_parallel_choose_strategy(&analysis, &target);
    EXPECT_EQ(strategy, PAR_STRATEGY_REDUCTION);
    return 0;
}

// =============================================================================
// Annotation Parsing Tests
// =============================================================================

static int test_parse_annotation_default(void) {
    size_t min_iters, chunk_size;
    EXPECT_TRUE(auto_parallel_parse_annotation("", &min_iters, &chunk_size));
    EXPECT_EQ(min_iters, (size_t)64);
    EXPECT_EQ(chunk_size, (size_t)0);
    return 0;
}

static int test_parse_annotation_custom(void) {
    size_t min_iters, chunk_size;
    EXPECT_TRUE(auto_parallel_parse_annotation("min_iters=128,chunk_size=32", &min_iters, &chunk_size));
    EXPECT_EQ(min_iters, (size_t)128);
    EXPECT_EQ(chunk_size, (size_t)32);
    return 0;
}

// =============================================================================
// Test Runner
// =============================================================================

typedef struct { const char* name; int (*func)(void); } TestCase;

int main(void) {
    TestCase tests[] = {
        {"auto_parallel_new",       test_auto_parallel_new},
        {"no_dependencies",         test_no_dependencies},
        {"has_loop_carried_deps",   test_has_loop_carried_deps},
        {"strategy_sequential",     test_strategy_sequential},
        {"strategy_parallel_for",   test_strategy_parallel_for},
        {"strategy_simd",           test_strategy_simd},
        {"strategy_reduction",      test_strategy_reduction},
        {"parse_annotation_default", test_parse_annotation_default},
        {"parse_annotation_custom", test_parse_annotation_custom},
    };

    size_t total = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;

    printf("Running %zu auto-parallel tests...\n\n", total);

    for (size_t i = 0; i < total; i++) {
        printf("[ RUN      ] parallel.%s\n", tests[i].name);
        int result = tests[i].func();
        if (result == 0) {
            printf("[\033[32m       OK\033[0m ] parallel.%s\n", tests[i].name);
            passed++;
        } else {
            printf("[\033[31m  FAILED  \033[0m ] parallel.%s\n", tests[i].name);
        }
    }

    printf("\nTests run: %zu\n\033[32mPassed: %zu\033[0m\n", total, passed);
    if (passed < total) printf("\033[31mFailed: %zu\033[0m\n", total - passed);

    return passed < total ? 1 : 0;
}
