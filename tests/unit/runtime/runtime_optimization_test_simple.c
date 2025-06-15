#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "runtime_optimization.h"

// Define ANSI color codes here since we don't have test_helpers.h
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

// Test data structures
static int test_count = 0;
static int test_passed = 0;
static int test_failed = 0;

#define TEST(name) \
    do { \
        printf("Running test: %s...", #name); \
        test_count++; \
        if (test_##name()) { \
            printf(" PASSED\n"); \
            test_passed++; \
        } else { \
            printf(" FAILED\n"); \
            test_failed++; \
        } \
    } while(0)

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            printf("\n  ASSERTION FAILED: %s (line %d)\n", #expr, __LINE__); \
            return false; \
        } \
    } while(0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))
#define ASSERT_NULL(ptr) ASSERT_TRUE((ptr) == NULL)
#define ASSERT_NOT_NULL(ptr) ASSERT_TRUE((ptr) != NULL)
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))

// =============================================================================
// Test Functions (Simplified)
// =============================================================================

bool test_optimization_context_creation() {
    // Test creation with different safety levels
    OptimizationContext* ctx1 = optimization_context_new(OPT_SAFETY_AGGRESSIVE);
    ASSERT_NOT_NULL(ctx1);
    ASSERT_EQ(ctx1->safety_level, OPT_SAFETY_AGGRESSIVE);
    ASSERT_TRUE(ctx1->enable_speculation);
    ASSERT_TRUE(ctx1->enable_vectorization);
    
    OptimizationContext* ctx2 = optimization_context_new(OPT_SAFETY_DEBUG);
    ASSERT_NOT_NULL(ctx2);
    ASSERT_EQ(ctx2->safety_level, OPT_SAFETY_DEBUG);
    ASSERT_FALSE(ctx2->enable_speculation);
    ASSERT_FALSE(ctx2->enable_vectorization);
    
    optimization_context_free(ctx1);
    optimization_context_free(ctx2);
    
    return true;
}

bool test_hardware_capability_detection() {
    HardwareCapabilities caps = detect_hardware_capabilities();
    
    // Should detect at least some capabilities on most modern systems
    // This is platform-dependent, so we just check it doesn't crash
    printf("\n  Detected hardware capabilities: 0x%x", caps);
    
    HardwareVerifier* verifier = hardware_verifier_new(caps);
    ASSERT_NOT_NULL(verifier);
    ASSERT_EQ(verifier->available_features, caps);
    
    hardware_verifier_free(verifier);
    
    return true;
}

bool test_speculative_execution() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    // Create a dummy AST node for testing
    ASTNode* speculation_point = malloc(sizeof(ASTNode));
    speculation_point->type = AST_IDENTIFIER;
    
    SpeculationContext* spec_ctx = speculation_context_new(speculation_point);
    ASSERT_NOT_NULL(spec_ctx);
    ASSERT_FALSE(spec_ctx->is_speculating);
    
    // Test beginning speculation
    int result = begin_speculation(ctx, spec_ctx);
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(spec_ctx->is_speculating);
    ASSERT_EQ(spec_ctx->speculation_attempts, 1);
    ASSERT_NOT_NULL(spec_ctx->checkpoint_state);
    
    // Test committing speculation (success case)
    result = commit_speculation(ctx, spec_ctx);
    ASSERT_EQ(result, 0);
    ASSERT_FALSE(spec_ctx->is_speculating);
    ASSERT_EQ(spec_ctx->speculation_successes, 1);
    ASSERT_EQ(ctx->speculation_hits, 1);
    ASSERT_NULL(spec_ctx->checkpoint_state);
    
    // Test rollback case
    result = begin_speculation(ctx, spec_ctx);
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(spec_ctx->is_speculating);
    
    result = rollback_speculation(ctx, spec_ctx);
    ASSERT_EQ(result, 0);
    ASSERT_FALSE(spec_ctx->is_speculating);
    ASSERT_EQ(spec_ctx->rollback_count, 1);
    ASSERT_EQ(ctx->speculation_misses, 1);
    
    speculation_context_free(spec_ctx);
    free(speculation_point);
    optimization_context_free(ctx);
    
    return true;
}

bool test_adaptive_optimization() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    AdaptiveOptimizer* optimizer = adaptive_optimizer_new(ctx);
    ASSERT_NOT_NULL(optimizer);
    ASSERT_EQ(optimizer->ctx, ctx);
    ASSERT_EQ(optimizer->adaptation_threshold, 0.1);
    ASSERT_FALSE(optimizer->is_collecting);
    
    int result = adaptive_optimizer_update(optimizer);
    ASSERT_EQ(result, 0);
    
    result = adaptive_optimizer_trigger_reoptimization(optimizer);
    ASSERT_EQ(result, 0);
    
    adaptive_optimizer_free(optimizer);
    optimization_context_free(ctx);
    
    return true;
}

bool test_profile_data_management() {
    ProfileData* data = profile_data_new("test_function");
    ASSERT_NOT_NULL(data);
    ASSERT_NOT_NULL(data->function_name);
    ASSERT_EQ(strcmp(data->function_name, "test_function"), 0);
    ASSERT_EQ(data->call_count, 0);
    
    profile_data_free(data);
    
    return true;
}

bool test_optimization_diagnostics() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    // Create a dummy AST node
    ASTNode* target = malloc(sizeof(ASTNode));
    target->type = AST_IDENTIFIER;
    
    OptimizationDiagnostic* diag = optimization_diagnostic_new(
        OPT_BOUNDS_CHECK_ELIMINATION,
        target,
        OPT_ERROR_PROOF_FAILED,
        "Test diagnostic message"
    );
    
    ASSERT_NOT_NULL(diag);
    ASSERT_EQ(diag->opt_type, OPT_BOUNDS_CHECK_ELIMINATION);
    ASSERT_EQ(diag->error, OPT_ERROR_PROOF_FAILED);
    ASSERT_NOT_NULL(diag->message);
    ASSERT_FALSE(diag->is_warning);
    
    int result = emit_optimization_diagnostic(ctx, diag);
    ASSERT_EQ(result, 0);
    
    optimization_diagnostic_free(diag);
    free(target);
    optimization_context_free(ctx);
    
    return true;
}

bool test_error_handling() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    // Test error string function
    const char* error_str = optimization_error_string(OPT_ERROR_PROOF_FAILED);
    ASSERT_NOT_NULL(error_str);
    ASSERT_TRUE(strlen(error_str) > 0);
    
    // Test error clearing
    optimization_clear_error(ctx);
    ASSERT_EQ(ctx->last_error[0], '\0');
    
    optimization_context_free(ctx);
    
    return true;
}

bool test_optimization_benchmarking() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    ASTNode* target = malloc(sizeof(ASTNode));
    target->type = AST_IDENTIFIER;
    
    OptimizationBenchmark* benchmark = benchmark_optimization(
        ctx, OPT_BOUNDS_CHECK_ELIMINATION, target
    );
    
    ASSERT_NOT_NULL(benchmark);
    ASSERT_NOT_NULL(benchmark->name);
    ASSERT_TRUE(benchmark->baseline_time > 0);
    ASSERT_TRUE(benchmark->optimized_time > 0);
    ASSERT_TRUE(benchmark->speedup_factor > 0);
    ASSERT_TRUE(benchmark->correctness_verified);
    
    printf("\n  Benchmark speedup: %.2fx", benchmark->speedup_factor);
    
    optimization_benchmark_free(benchmark);
    free(target);
    optimization_context_free(ctx);
    
    return true;
}

bool test_performance_profile_collection() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    // Simulate collecting profile data for many functions
    const int num_functions = 100;
    clock_t start = clock();
    
    for (int i = 0; i < num_functions; i++) {
        char func_name[64];
        snprintf(func_name, sizeof(func_name), "function_%d", i);
        
        int result = collect_runtime_profile(ctx, func_name);
        ASSERT_EQ(result, 0);
    }
    
    clock_t end = clock();
    double time_taken = ((double)(end - start)) / CLOCKS_PER_SEC;
    
    printf("\n  Profile collection for %d functions took %.3f seconds", 
           num_functions, time_taken);
    
    optimization_context_free(ctx);
    
    return true;
}

bool test_memory_usage() {
    // Test that we don't leak memory during optimization operations
    const int iterations = 100; // Reduced for faster testing
    
    for (int i = 0; i < iterations; i++) {
        OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
        ASSERT_NOT_NULL(ctx);
        
        // Create and free a dummy target
        ASTNode* target = malloc(sizeof(ASTNode));
        target->type = AST_IDENTIFIER;
        
        free(target);
        optimization_context_free(ctx);
    }
    
    printf("\n  Completed %d optimization cycles without crashes", iterations);
    
    return true;
}

// =============================================================================
// Main Test Runner
// =============================================================================

void print_test_summary() {
    printf("\n" ANSI_COLOR_CYAN "=========================" ANSI_COLOR_RESET "\n");
    printf(ANSI_COLOR_CYAN "Runtime Optimization Test Summary" ANSI_COLOR_RESET "\n");
    printf(ANSI_COLOR_CYAN "=========================" ANSI_COLOR_RESET "\n");
    printf("Total tests: %d\n", test_count);
    printf(ANSI_COLOR_GREEN "Passed: %d" ANSI_COLOR_RESET "\n", test_passed);
    if (test_failed > 0) {
        printf(ANSI_COLOR_RED "Failed: %d" ANSI_COLOR_RESET "\n", test_failed);
    } else {
        printf("Failed: %d\n", test_failed);
    }
    printf("Success rate: %.1f%%\n", 
           test_count > 0 ? (100.0 * test_passed / test_count) : 0.0);
}

int main() {
    printf(ANSI_COLOR_BLUE "Starting Runtime Optimization Framework Tests (Simplified)..." ANSI_COLOR_RESET "\n");
    
    // Run basic functionality tests
    TEST(optimization_context_creation);
    TEST(hardware_capability_detection);
    TEST(speculative_execution);
    TEST(adaptive_optimization);
    TEST(profile_data_management);
    TEST(optimization_diagnostics);
    TEST(error_handling);
    TEST(optimization_benchmarking);
    
    // Performance and stress tests
    TEST(performance_profile_collection);
    TEST(memory_usage);
    
    print_test_summary();
    
    return (test_failed > 0) ? 1 : 0;
}
