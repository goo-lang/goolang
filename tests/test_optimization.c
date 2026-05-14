#include "optimization.h"
#include "comptime.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Test helper functions
void test_target_info_detection(void);
void test_optimization_context_creation(void);
void test_optimization_directives(void);
void test_hardware_feature_detection(void);
void test_algorithm_selection(void);
void test_performance_estimation(void);
void test_builtin_registration(void);

void test_target_info_detection(void) {
    printf("Testing target information detection...\n");
    
    // Test automatic target detection
    TargetInfo* info = target_info_detect();
    assert(info != NULL);
    assert(info->architecture != NULL);
    assert(info->cpu_model != NULL);
    assert(info->core_count > 0);
    assert(info->cache_line_size > 0);
    assert(info->l1_cache_size > 0);
    
    printf("Detected architecture: %s\n", info->architecture);
    printf("CPU model: %s\n", info->cpu_model);
    printf("Core count: %d\n", info->core_count);
    printf("Cache line size: %zu bytes\n", info->cache_line_size);
    
    // Test some features
    printf("SSE support: %s\n", target_has_feature(info, HW_FEATURE_SSE) ? "yes" : "no");
    printf("AVX2 support: %s\n", target_has_feature(info, HW_FEATURE_AVX2) ? "yes" : "no");
    printf("NEON support: %s\n", target_has_feature(info, HW_FEATURE_NEON) ? "yes" : "no");
    
    target_info_free(info);
    
    // Test target specification parsing
    TargetInfo* x86_info = target_info_from_string("x86_64-unknown-linux-gnu");
    assert(x86_info != NULL);
    assert(strcmp(x86_info->architecture, "x86_64") == 0);
    assert(target_has_feature(x86_info, HW_FEATURE_SSE));
    assert(target_has_feature(x86_info, HW_FEATURE_SSE2));
    target_info_free(x86_info);
    
    TargetInfo* arm_info = target_info_from_string("aarch64-unknown-linux-gnu");
    assert(arm_info != NULL);
    assert(strcmp(arm_info->architecture, "aarch64") == 0);
    assert(target_has_feature(arm_info, HW_FEATURE_NEON));
    target_info_free(arm_info);
    
    printf("✓ Target information detection tests passed!\n");
}

void test_optimization_context_creation(void) {
    printf("Testing optimization context creation...\n");
    
    // Create compile-time context first
    ComptimeContext* comptime_ctx = comptime_context_new(NULL);
    assert(comptime_ctx != NULL);
    
    // Create optimization context
    OptimizationContext* opt_ctx = comptime_optimization_context_new(comptime_ctx);
    assert(opt_ctx != NULL);
    assert(opt_ctx->comptime_ctx == comptime_ctx);
    assert(opt_ctx->target_info != NULL);
    assert(opt_ctx->conservative_mode == true);
    assert(opt_ctx->min_performance_improvement >= 1.0);
    assert(opt_ctx->benchmark_cache_dir != NULL);
    
    // Verify target info is populated
    assert(opt_ctx->target_info->architecture != NULL);
    assert(opt_ctx->target_info->core_count > 0);
    
    // Test directive management
    OptimizationDirective* directive = optimization_directive_new("@test_directive", OPT_GOAL_THROUGHPUT);
    assert(directive != NULL);
    assert(strcmp(directive->name, "@test_directive") == 0);
    assert(directive->goal == OPT_GOAL_THROUGHPUT);
    
    int result = optimization_context_add_directive(opt_ctx, directive);
    assert(result == 1);
    
    OptimizationDirective* found = optimization_context_find_directive(opt_ctx, "@test_directive");
    assert(found == directive);
    
    OptimizationDirective* not_found = optimization_context_find_directive(opt_ctx, "@nonexistent");
    assert(not_found == NULL);
    
    comptime_optimization_context_free(opt_ctx);
    comptime_context_free(comptime_ctx);
    
    printf("✓ Optimization context creation tests passed!\n");
}

void test_optimization_directives(void) {
    printf("Testing optimization directives...\n");
    
    ComptimeContext* comptime_ctx = comptime_context_new(NULL);
    OptimizationContext* opt_ctx = comptime_optimization_context_new(comptime_ctx);
    
    // Create a mock AST node for testing
    ASTNode* mock_target = malloc(sizeof(ASTNode));
    mock_target->type = AST_FUNC_DECL;
    mock_target->pos.line = 1;
    mock_target->pos.column = 1;
    
    // Test @optimize_for directive
    OptimizeForParams throughput_params = {
        .goal = OPT_GOAL_THROUGHPUT,
        .enable_auto_vectorization = true,
        .enable_auto_parallelization = true,
        .enable_cache_optimization = true,
        .enable_branch_optimization = false
    };
    
    ComptimeResult* result = comptime_directive_optimize_for(opt_ctx, &throughput_params, mock_target);
    assert(result != NULL);
    if (result->error != NULL) {
        printf("Error in optimization directive: %s\n", result->error->message);
    }
    assert(result->error == NULL);
    assert(result->value != NULL);
    assert(result->value->type == COMPTIME_VALUE_STRING);
    assert(strstr(result->value->string_value, "throughput") != NULL);
    
    printf("Generated optimization code:\n%s\n", result->generated_code);
    comptime_result_free(result);
    
    // Test @use_algorithm directive
    UseAlgorithmParams algorithm_params = {
        .algorithm_name = "quicksort",
        .min_speedup_required = 1.5,
        .fallback_to_default = true
    };
    
    result = comptime_directive_use_algorithm(opt_ctx, &algorithm_params, mock_target);
    assert(result != NULL);
    assert(result->error == NULL);
    assert(result->value != NULL);
    assert(strstr(result->value->string_value, "quicksort") != NULL);
    comptime_result_free(result);
    
    // Test @target_has directive
    TargetHasParams target_params = {
        .feature = HW_FEATURE_SSE2
    };
    
    result = comptime_directive_target_has(opt_ctx, &target_params);
    assert(result != NULL);
    assert(result->error == NULL);
    assert(result->value != NULL);
    assert(result->value->type == COMPTIME_VALUE_BOOL);
    
    printf("Target has SSE2: %s\n", result->value->bool_value ? "yes" : "no");
    comptime_result_free(result);
    
    // Test @profile_guided directive
    ProfileGuidedParams pgo_params = {
        .profile_data_file = "profile.data",
        .enable_speculative_optimization = true,
        .confidence_threshold = 0.8
    };
    
    result = comptime_directive_profile_guided(opt_ctx, &pgo_params, mock_target);
    assert(result != NULL);
    assert(result->error == NULL);
    assert(result->value != NULL);
    assert(strstr(result->value->string_value, "profile.data") != NULL);
    comptime_result_free(result);
    
    // Test @benchmark_and_select directive
    char* implementations[] = {"impl1", "simd_impl", "parallel_impl"};
    BenchmarkSelectParams benchmark_params = {
        .implementation_names = implementations,
        .implementation_count = 3,
        .test_data_spec = "test_data.bin",
        .benchmark_iterations = 100,
        .selection_criteria = OPT_GOAL_THROUGHPUT
    };
    
    result = comptime_directive_benchmark_and_select(opt_ctx, &benchmark_params);
    assert(result != NULL);
    assert(result->error == NULL);
    assert(result->value != NULL);
    assert(result->value->type == COMPTIME_VALUE_STRING);
    
    printf("Selected implementation: %s\n", result->value->string_value);
    comptime_result_free(result);
    
    // Clean up mock AST node
    free(mock_target);
    
    comptime_optimization_context_free(opt_ctx);
    comptime_context_free(comptime_ctx);
    
    printf("✓ Optimization directive tests passed!\n");
}

void test_hardware_feature_detection(void) {
    printf("Testing hardware feature detection...\n");
    
    // Test feature name mapping
    assert(strcmp(hardware_feature_name(HW_FEATURE_SSE), "sse") == 0);
    assert(strcmp(hardware_feature_name(HW_FEATURE_AVX2), "avx2") == 0);
    assert(strcmp(hardware_feature_name(HW_FEATURE_NEON), "neon") == 0);
    
    // Test current target info
    TargetInfo* current_info = get_current_target_info();
    assert(current_info != NULL);
    assert(current_info->architecture != NULL);
    
    printf("Current target: %s\n", current_info->architecture);
    
    // Test individual feature detection
    for (int feature = 0; feature < HW_FEATURE_COUNT; feature++) {
        bool has_feature = detect_hardware_feature((HardwareFeature)feature);
        printf("Feature %s: %s\n", 
               hardware_feature_name((HardwareFeature)feature),
               has_feature ? "supported" : "not supported");
    }
    
    target_info_free(current_info);
    
    printf("✓ Hardware feature detection tests passed!\n");
}

void test_algorithm_selection(void) {
    printf("Testing algorithm selection...\n");
    
    ComptimeContext* comptime_ctx = comptime_context_new(NULL);
    OptimizationContext* opt_ctx = comptime_optimization_context_new(comptime_ctx);
    
    // Register built-in algorithms
    register_builtin_algorithms(opt_ctx);
    
    assert(opt_ctx->algorithm_count > 0);
    assert(opt_ctx->available_algorithms != NULL);
    
    // Verify some algorithms are registered
    bool found_quicksort = false;
    bool found_simd_sort = false;
    
    for (size_t i = 0; i < opt_ctx->algorithm_count; i++) {
        printf("Registered algorithm: %s\n", opt_ctx->available_algorithms[i]);
        if (strcmp(opt_ctx->available_algorithms[i], "quicksort") == 0) {
            found_quicksort = true;
        }
        if (strcmp(opt_ctx->available_algorithms[i], "simd_sort") == 0) {
            found_simd_sort = true;
        }
    }
    
    assert(found_quicksort);
    assert(found_simd_sort);
    
    comptime_optimization_context_free(opt_ctx);
    comptime_context_free(comptime_ctx);
    
    printf("✓ Algorithm selection tests passed!\n");
}

void test_performance_estimation(void) {
    printf("Testing performance estimation...\n");
    
    ComptimeContext* comptime_ctx = comptime_context_new(NULL);
    OptimizationContext* opt_ctx = comptime_optimization_context_new(comptime_ctx);
    
    // Create a test directive
    OptimizationDirective* directive = optimization_directive_new("@test_perf", OPT_GOAL_THROUGHPUT);
    assert(directive != NULL);
    
    // Test performance estimation
    double improvement = estimate_performance_improvement(opt_ctx, directive, NULL);
    printf("Estimated performance improvement: %.2fx\n", improvement);
    assert(improvement >= 1.0);
    
    // Test benefit analysis
    bool beneficial = is_optimization_beneficial(opt_ctx, directive, NULL);
    printf("Optimization beneficial: %s\n", beneficial ? "yes" : "no");
    
    // Test with different goals
    directive->goal = OPT_GOAL_LATENCY;
    improvement = estimate_performance_improvement(opt_ctx, directive, NULL);
    printf("Latency optimization improvement: %.2fx\n", improvement);
    
    directive->goal = OPT_GOAL_MEMORY;
    improvement = estimate_performance_improvement(opt_ctx, directive, NULL);
    printf("Memory optimization improvement: %.2fx\n", improvement);
    
    optimization_directive_free(directive);
    comptime_optimization_context_free(opt_ctx);
    comptime_context_free(comptime_ctx);
    
    printf("✓ Performance estimation tests passed!\n");
}

void test_builtin_registration(void) {
    printf("Testing built-in registration...\n");
    
    ComptimeContext* comptime_ctx = comptime_context_new(NULL);
    OptimizationContext* opt_ctx = comptime_optimization_context_new(comptime_ctx);
    
    // Register built-in directives
    register_builtin_optimization_directives(opt_ctx);
    
    // Verify directives are registered
    assert(optimization_context_find_directive(opt_ctx, "@optimize_for") != NULL);
    assert(optimization_context_find_directive(opt_ctx, "@use_algorithm") != NULL);
    assert(optimization_context_find_directive(opt_ctx, "@target_has") != NULL);
    assert(optimization_context_find_directive(opt_ctx, "@profile_guided") != NULL);
    assert(optimization_context_find_directive(opt_ctx, "@benchmark_and_select") != NULL);
    
    // Register built-in algorithms
    register_builtin_algorithms(opt_ctx);
    assert(opt_ctx->algorithm_count > 0);
    
    // Test optimization goal and strategy name mappings
    assert(strcmp(optimization_goal_name(OPT_GOAL_THROUGHPUT), "throughput") == 0);
    assert(strcmp(optimization_goal_name(OPT_GOAL_LATENCY), "latency") == 0);
    assert(strcmp(optimization_goal_name(OPT_GOAL_MEMORY), "memory") == 0);
    
    assert(strcmp(optimization_strategy_name(OPT_STRATEGY_SIMD), "simd") == 0);
    assert(strcmp(optimization_strategy_name(OPT_STRATEGY_PARALLEL), "parallel") == 0);
    assert(strcmp(optimization_strategy_name(OPT_STRATEGY_CACHE_FRIENDLY), "cache_friendly") == 0);
    
    comptime_optimization_context_free(opt_ctx);
    comptime_context_free(comptime_ctx);
    
    printf("✓ Built-in registration tests passed!\n");
}

int main(void) {
    printf("Running optimization directives framework tests...\n\n");
    
    test_target_info_detection();
    test_optimization_context_creation();
    test_optimization_directives();
    test_hardware_feature_detection();
    test_algorithm_selection();
    test_performance_estimation();
    test_builtin_registration();
    
    printf("\n✅ All optimization directives framework tests passed!\n");
    return 0;
}
