#include "include/auto_parallel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Test helper functions
void test_assert(bool condition, const char* test_name) {
    if (condition) {
        printf("✓ %s\n", test_name);
    } else {
        printf("✗ %s\n", test_name);
        exit(1);
    }
}

// Test hardware detection
void test_hardware_detection() {
    HardwareInfo* hw_info = detect_hardware_capabilities();
    
    test_assert(hw_info != NULL, "Hardware detection succeeds");
    test_assert(hw_info->num_cores > 0, "Number of cores detected");
    test_assert(hw_info->num_threads > 0, "Number of threads detected");
    test_assert(hw_info->cache_line_size > 0, "Cache line size detected");
    test_assert(hw_info->simd_width > 0, "SIMD width detected");
    
    printf("  Detected: %d cores, %d threads, %d-byte SIMD\n", 
           hw_info->num_cores, hw_info->num_threads, (int)hw_info->simd_width);
    
    hardware_info_free(hw_info);
}

// Test parallel context creation
void test_parallel_context_creation() {
    HardwareInfo* hw_info = detect_hardware_capabilities();
    ParallelContext* ctx = parallel_context_new(hw_info);
    
    test_assert(ctx != NULL, "Parallel context creation");
    test_assert(ctx->hw_info != NULL, "Hardware info attached");
    test_assert(ctx->threshold_benefit > 1.0, "Reasonable threshold benefit");
    test_assert(ctx->max_threads > 0, "Max threads set");
    test_assert(ctx->loops_analyzed == 0, "Initial statistics zeroed");
    
    parallel_context_free(ctx);
}

// Test hardware capability checking
void test_hardware_capabilities() {
    HardwareInfo* hw_info = detect_hardware_capabilities();
    
    // Test capability checking functions
    bool has_multicore = hardware_supports_capability(hw_info, HW_CAP_MULTICORE);
    bool has_simd = hardware_supports_capability(hw_info, HW_CAP_SSE) ||
                    hardware_supports_capability(hw_info, HW_CAP_AVX) ||
                    hardware_supports_capability(hw_info, HW_CAP_NEON);
    
    test_assert(has_multicore || hw_info->num_cores == 1, "Multicore capability consistent");
    printf("  SIMD Support: %s\n", has_simd ? "Yes" : "No");
    
    if (hardware_supports_capability(hw_info, HW_CAP_AVX512)) {
        test_assert(hw_info->simd_width >= 64, "AVX-512 implies wide SIMD");
        printf("  AVX-512 detected\n");
    } else if (hardware_supports_capability(hw_info, HW_CAP_AVX2)) {
        test_assert(hw_info->simd_width >= 32, "AVX2 implies 256-bit SIMD");
        printf("  AVX2 detected\n");
    } else if (hardware_supports_capability(hw_info, HW_CAP_NEON)) {
        test_assert(hw_info->simd_width >= 16, "NEON implies 128-bit SIMD");
        printf("  ARM NEON detected\n");
    }
    
    hardware_info_free(hw_info);
}

// Test annotation parsing (without AST dependency)
void test_annotation_parsing() {
    ParallelAnnotation* annotation = parse_parallel_annotation(NULL);
    
    test_assert(annotation != NULL, "Annotation parsing succeeds");
    test_assert(annotation->type == PARALLEL_AUTO, "Default annotation type");
    test_assert(annotation->vectorize == true, "Default vectorization enabled");
    test_assert(annotation->chunk_size == 0, "Auto chunk size");
    test_assert(annotation->num_threads == 0, "Auto thread count");
    
    parallel_annotation_free(annotation);
}

// Test cost model creation
void test_cost_model() {
    CostModel* default_model = create_default_cost_model();
    
    test_assert(default_model != NULL, "Default cost model creation");
    test_assert(default_model->computation_cost > 0, "Positive computation cost");
    test_assert(default_model->memory_cost > 0, "Positive memory cost");
    test_assert(default_model->threshold_size > 0, "Positive threshold size");
    test_assert(default_model->parallel_efficiency > 0 && 
               default_model->parallel_efficiency <= 1.0, "Valid parallel efficiency");
    
    HardwareInfo* hw_info = detect_hardware_capabilities();
    CostModel* adaptive_model = create_adaptive_cost_model(hw_info);
    
    test_assert(adaptive_model != NULL, "Adaptive cost model creation");
    
    // Test cost calculation
    double seq_cost = calculate_parallel_cost(default_model, 10000, 1);
    double par_cost = calculate_parallel_cost(default_model, 10000, 4);
    
    test_assert(seq_cost > 0, "Sequential cost calculation");
    test_assert(par_cost > 0, "Parallel cost calculation");
    
    printf("  Cost ratio (4 threads): %.2fx\n", seq_cost / par_cost);
    
    cost_model_free(default_model);
    cost_model_free(adaptive_model);
    hardware_info_free(hw_info);
}

// Test dependency analysis (without AST)
void test_dependency_analysis() {
    HardwareInfo* hw_info = detect_hardware_capabilities();
    ParallelContext* ctx = parallel_context_new(hw_info);
    
    DependencyGraph* graph = build_dependency_graph(ctx, NULL);
    
    test_assert(graph != NULL, "Dependency graph creation");
    test_assert(graph->nodes != NULL, "Graph nodes allocated");
    test_assert(graph->capacity > 0, "Graph has capacity");
    
    // Test dependency type analysis
    DependencyType dep_type = analyze_statement_dependency(NULL, NULL);
    test_assert(dep_type >= DEP_NONE && dep_type <= DEP_UNKNOWN, "Valid dependency type");
    
    dependency_graph_free(graph);
    parallel_context_free(ctx);
}

// Test configuration functions
void test_configuration() {
    HardwareInfo* hw_info = detect_hardware_capabilities();
    ParallelContext* ctx = parallel_context_new(hw_info);
    
    // Test aggressiveness setting
    double original_threshold = ctx->threshold_benefit;
    
    set_parallelization_aggressiveness(ctx, 0.9); // Aggressive
    test_assert(ctx->aggressive_mode, "Aggressive mode enabled");
    test_assert(!ctx->conservative_mode, "Conservative mode disabled");
    test_assert(ctx->threshold_benefit < original_threshold, "Lower threshold in aggressive mode");
    
    set_parallelization_aggressiveness(ctx, 0.2); // Conservative
    test_assert(!ctx->aggressive_mode, "Aggressive mode disabled");
    test_assert(ctx->conservative_mode, "Conservative mode enabled");
    test_assert(ctx->threshold_benefit > original_threshold, "Higher threshold in conservative mode");
    
    set_parallelization_aggressiveness(ctx, 0.5); // Balanced
    test_assert(!ctx->aggressive_mode, "Balanced: aggressive mode disabled");
    test_assert(!ctx->conservative_mode, "Balanced: conservative mode disabled");
    
    // Test thread limit
    int original_max = ctx->max_threads;
    set_thread_limit(ctx, 2);
    test_assert(ctx->max_threads == 2, "Thread limit set correctly");
    
    set_thread_limit(ctx, original_max);
    test_assert(ctx->max_threads == original_max, "Thread limit restored");
    
    // Test profiling mode
    enable_profiling_mode(ctx, true); // Should not crash
    enable_profiling_mode(ctx, false);
    
    parallel_context_free(ctx);
}

// Test utility functions
void test_utility_functions() {
    // Test string conversion functions
    const char* strategy_str = parallel_strategy_string(STRATEGY_SIMD);
    test_assert(strcmp(strategy_str, "SIMD") == 0, "Strategy string conversion");
    
    const char* dep_str = dependency_type_string(DEP_TRUE);
    test_assert(strstr(dep_str, "RAW") != NULL, "Dependency string conversion");
    
    const char* red_str = reduction_type_string(REDUCTION_SUM);
    test_assert(strcmp(red_str, "Sum") == 0, "Reduction string conversion");
    
    // Test reduction detection
    ReductionType detected;
    bool is_reduction = is_reduction_operation(NULL, &detected);
    test_assert(!is_reduction, "No reduction detected in NULL expression");
}

// Test integration functions
void test_integration() {
    HardwareInfo* hw_info = detect_hardware_capabilities();
    ParallelContext* ctx = parallel_context_new(hw_info);
    
    // Test compiler pipeline integration
    bool integrated = integrate_with_compiler_pipeline(ctx);
    test_assert(integrated, "Compiler pipeline integration");
    
    // Test optimization registration
    register_parallel_optimizations(ctx); // Should not crash
    
    // Test module parallelization
    bool applied = apply_parallelization_to_module(ctx, NULL);
    test_assert(applied, "Module parallelization applied");
    
    parallel_context_free(ctx);
}

// Test reporting functions
void test_reporting() {
    HardwareInfo* hw_info = detect_hardware_capabilities();
    ParallelContext* ctx = parallel_context_new(hw_info);
    
    // Add some statistics
    ctx->loops_analyzed = 10;
    ctx->loops_parallelized = 7;
    ctx->functions_parallelized = 3;
    
    // Test summary generation
    char* summary = get_parallelization_summary(ctx);
    test_assert(summary != NULL, "Summary generation");
    test_assert(strstr(summary, "7/10") != NULL, "Summary contains statistics");
    test_assert(strstr(summary, "70.0%") != NULL, "Summary contains percentage");
    
    printf("  %s\n", summary);
    
    // Test report printing (should not crash)
    printf("\n--- Parallelization Report ---\n");
    print_parallelization_report(ctx);
    printf("--- End Report ---\n");
    
    free(summary);
    parallel_context_free(ctx);
}

// Test benefit estimation without actual loops
void test_benefit_estimation() {
    HardwareInfo* hw_info = detect_hardware_capabilities();
    ParallelContext* ctx = parallel_context_new(hw_info);
    
    // Create a mock loop info for testing
    LoopInfo mock_loop = {0};
    mock_loop.is_countable = true;
    mock_loop.iteration_count = 10000;
    mock_loop.has_dependencies = false;
    mock_loop.is_vectorizable = true;
    mock_loop.is_parallelizable = true;
    
    // Test benefit estimation
    double loop_benefit = estimate_parallelization_benefit(ctx, &mock_loop, STRATEGY_LOOP_PARALLEL);
    double simd_benefit = estimate_parallelization_benefit(ctx, &mock_loop, STRATEGY_SIMD);
    double task_benefit = estimate_parallelization_benefit(ctx, &mock_loop, STRATEGY_TASK_PARALLEL);
    
    test_assert(loop_benefit >= 1.0, "Loop parallelization benefit >= 1.0");
    test_assert(simd_benefit >= 1.0, "SIMD benefit >= 1.0");
    test_assert(task_benefit >= 1.0, "Task parallelization benefit >= 1.0");
    
    printf("  Estimated Benefits - Loop: %.2fx, SIMD: %.2fx, Task: %.2fx\n", 
           loop_benefit, simd_benefit, task_benefit);
    
    parallel_context_free(ctx);
}

int main() {
    printf("Running Simplified Automatic Parallelization System Tests...\n\n");
    
    test_hardware_detection();
    test_parallel_context_creation();
    test_hardware_capabilities();
    test_annotation_parsing();
    test_cost_model();
    test_dependency_analysis();
    test_configuration();
    test_utility_functions();
    test_integration();
    test_reporting();
    test_benefit_estimation();
    
    printf("\n✓ All simplified automatic parallelization tests passed!\n");
    return 0;
}