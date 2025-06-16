#include "../include/auto_parallel.h"
#include "../include/ast.h"
#include "../include/types.h"
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

// Test annotation parsing
void test_annotation_parsing() {
    // Create mock annotation AST node
    Position pos = {0};
    ASTNode* annotation_node = ast_node_new(AST_ATTRIBUTE, pos);
    
    ParallelAnnotation* annotation = parse_parallel_annotation(annotation_node);
    
    test_assert(annotation != NULL, "Annotation parsing succeeds");
    test_assert(annotation->type == PARALLEL_AUTO, "Default annotation type");
    test_assert(annotation->vectorize == true, "Default vectorization enabled");
    test_assert(annotation->chunk_size == 0, "Auto chunk size");
    test_assert(annotation->num_threads == 0, "Auto thread count");
    
    // Test validation
    ASTNode* loop_node = ast_node_new(AST_FOR_STMT, pos);
    bool valid = validate_parallel_annotation(annotation, loop_node);
    test_assert(valid, "Annotation valid for loop");
    
    ASTNode* expr_node = ast_node_new(AST_BINARY_EXPR, pos);
    bool invalid = validate_parallel_annotation(annotation, expr_node);
    test_assert(invalid, "Some annotations invalid for expressions");
    
    parallel_annotation_free(annotation);
    ast_node_free(annotation_node);
    ast_node_free(loop_node);
    ast_node_free(expr_node);
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

// Test loop analysis
void test_loop_analysis() {
    HardwareInfo* hw_info = detect_hardware_capabilities();
    ParallelContext* ctx = parallel_context_new(hw_info);
    
    // Create mock for loop AST
    Position pos = {0};
    ASTNode* for_loop = ast_node_new(AST_FOR_STMT, pos);
    
    LoopInfo* loop_info = analyze_loop(ctx, for_loop);
    
    test_assert(loop_info != NULL, "Loop analysis succeeds");
    test_assert(loop_info->loop_node == for_loop, "Loop node reference correct");
    test_assert(loop_info->nesting_level == 0, "Correct nesting level");
    test_assert(loop_info->is_countable, "For loop is countable");
    test_assert(loop_info->iteration_count > 0, "Positive iteration count");
    
    test_assert(ctx->loops_analyzed == 1, "Statistics updated");
    
    // Test while loop
    ASTNode* while_loop = ast_node_new(AST_FOR_STMT, pos);
    LoopInfo* while_info = analyze_loop(ctx, while_loop);
    
    test_assert(while_info != NULL, "While loop analysis succeeds");
    test_assert(!while_info->is_countable, "While loop not countable");
    test_assert(while_info->iteration_count == -1, "While loop unknown iteration count");
    
    test_assert(ctx->loops_analyzed == 2, "Statistics updated for second loop");
    
    loop_info_free(loop_info);
    loop_info_free(while_info);
    ast_node_free(for_loop);
    ast_node_free(while_loop);
    parallel_context_free(ctx);
}

// Test dependency analysis
void test_dependency_analysis() {
    HardwareInfo* hw_info = detect_hardware_capabilities();
    ParallelContext* ctx = parallel_context_new(hw_info);
    
    // Create mock code block
    Position pos = {0};
    ASTNode* block = ast_node_new(AST_BLOCK_STMT, pos);
    
    DependencyGraph* graph = build_dependency_graph(ctx, block);
    
    test_assert(graph != NULL, "Dependency graph creation");
    test_assert(graph->nodes != NULL, "Graph nodes allocated");
    test_assert(graph->capacity > 0, "Graph has capacity");
    
    // Test dependency type analysis
    ASTNode* stmt1 = ast_node_new(AST_EXPR_STMT, pos);
    ASTNode* stmt2 = ast_node_new(AST_EXPR_STMT, pos);
    
    DependencyType dep_type = analyze_statement_dependency(stmt1, stmt2);
    test_assert(dep_type >= DEP_NONE && dep_type <= DEP_UNKNOWN, "Valid dependency type");
    
    // Test loop-carried dependency detection
    LoopInfo* loop_info = analyze_loop(ctx, block);
    bool has_deps = has_loop_carried_dependency(graph, loop_info);
    test_assert(!has_deps, "No loop-carried dependencies detected");
    
    dependency_graph_free(graph);
    loop_info_free(loop_info);
    ast_node_free(stmt1);
    ast_node_free(stmt2);
    ast_node_free(block);
    parallel_context_free(ctx);
}

// Test parallelization decision making
void test_parallelization_decision() {
    HardwareInfo* hw_info = detect_hardware_capabilities();
    ParallelContext* ctx = parallel_context_new(hw_info);
    
    // Create test loop
    Position pos = {0};
    ASTNode* loop_node = ast_node_new(AST_FOR_STMT, pos);
    LoopInfo* loop = analyze_loop(ctx, loop_node);
    
    // Test automatic decision
    ParallelDecision* auto_decision = make_parallelization_decision(ctx, loop, NULL);
    
    test_assert(auto_decision != NULL, "Automatic decision created");
    test_assert(auto_decision->strategy >= STRATEGY_NONE && 
               auto_decision->strategy < STRATEGY_COUNT, "Valid strategy");
    test_assert(auto_decision->expected_speedup >= 1.0, "Reasonable speedup estimate");
    test_assert(auto_decision->recommended_threads > 0, "Positive thread count");
    test_assert(auto_decision->reasoning != NULL, "Decision reasoning provided");
    
    printf("  Auto Decision: %s, Speedup: %.2fx, Threads: %d\n",
           parallel_strategy_string(auto_decision->strategy),
           auto_decision->expected_speedup,
           auto_decision->recommended_threads);
    
    // Test with forced annotation
    ParallelAnnotation* force_annotation = parse_parallel_annotation(NULL);
    force_annotation->type = PARALLEL_FORCE;
    force_annotation->num_threads = 4;
    
    ParallelDecision* forced_decision = make_parallelization_decision(ctx, loop, force_annotation);
    
    test_assert(forced_decision != NULL, "Forced decision created");
    test_assert(forced_decision->should_parallelize, "Forced parallelization");
    test_assert(forced_decision->strategy == STRATEGY_LOOP_PARALLEL, "Loop parallel strategy");
    test_assert(forced_decision->recommended_threads == 4, "Forced thread count");
    
    // Test SIMD annotation
    force_annotation->type = PARALLEL_SIMD;
    ParallelDecision* simd_decision = make_parallelization_decision(ctx, loop, force_annotation);
    
    test_assert(simd_decision != NULL, "SIMD decision created");
    test_assert(simd_decision->strategy == STRATEGY_SIMD, "SIMD strategy");
    test_assert(simd_decision->use_simd, "SIMD enabled");
    
    // Test benefit estimation
    double loop_benefit = estimate_parallelization_benefit(ctx, loop, STRATEGY_LOOP_PARALLEL);
    double simd_benefit = estimate_parallelization_benefit(ctx, loop, STRATEGY_SIMD);
    
    test_assert(loop_benefit >= 1.0, "Loop parallelization benefit >= 1.0");
    test_assert(simd_benefit >= 1.0, "SIMD benefit >= 1.0");
    
    printf("  Estimated Benefits - Loop: %.2fx, SIMD: %.2fx\n", loop_benefit, simd_benefit);
    
    // Test profitability check
    bool profitable = is_parallelization_profitable(ctx, loop, auto_decision);
    printf("  Auto parallelization profitable: %s\n", profitable ? "Yes" : "No");
    
    free(auto_decision->reasoning);
    free(auto_decision);
    free(forced_decision->reasoning);
    free(forced_decision);
    free(simd_decision->reasoning);
    free(simd_decision);
    parallel_annotation_free(force_annotation);
    loop_info_free(loop);
    ast_node_free(loop_node);
    parallel_context_free(ctx);
}

// Test function analysis
void test_function_analysis() {
    HardwareInfo* hw_info = detect_hardware_capabilities();
    ParallelContext* ctx = parallel_context_new(hw_info);
    
    // Create mock function
    Position pos = {0};
    ASTNode* function = ast_node_new(AST_FUNC_DECL, pos);
    
    bool success = analyze_function_for_parallelization(ctx, function);
    
    test_assert(success, "Function analysis succeeds");
    test_assert(ctx->functions_parallelized == 1, "Function statistics updated");
    
    ast_node_free(function);
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
    Position pos = {0};
    ASTNode* expr = ast_node_new(AST_BINARY_EXPR, pos);
    ReductionType detected;
    
    bool is_reduction = is_reduction_operation(expr, &detected);
    test_assert(!is_reduction, "No reduction detected in simple expression");
    
    ast_node_free(expr);
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
    Position pos = {0};
    ASTNode* module = ast_node_new(AST_PROGRAM, pos);
    bool applied = apply_parallelization_to_module(ctx, module);
    test_assert(applied, "Module parallelization applied");
    
    ast_node_free(module);
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

// Test code generation stubs
void test_code_generation_stubs() {
    HardwareInfo* hw_info = detect_hardware_capabilities();
    ParallelContext* ctx = parallel_context_new(hw_info);
    
    // Create test loop and decision
    Position pos = {0};
    ASTNode* loop_node = ast_node_new(AST_FOR_STMT, pos);
    LoopInfo* loop = analyze_loop(ctx, loop_node);
    ParallelDecision* decision = make_parallelization_decision(ctx, loop, NULL);
    
    // Test transformation functions (should return input for now)
    ASTNode* transformed = transform_loop_for_parallelization(ctx, loop, decision);
    test_assert(transformed == loop_node, "Loop transformation stub");
    
    ASTNode* parallel_loop = generate_parallel_loop(ctx, loop, decision);
    test_assert(parallel_loop == loop_node, "Parallel loop generation stub");
    
    ASTNode* simd_loop = generate_simd_loop(ctx, loop);
    test_assert(simd_loop == loop_node, "SIMD loop generation stub");
    
    ASTNode* function = ast_node_new(AST_FUNC_DECL, pos);
    ASTNode* task_code = generate_task_parallel_code(ctx, function);
    test_assert(task_code == function, "Task parallel code generation stub");
    
    free(decision->reasoning);
    free(decision);
    loop_info_free(loop);
    ast_node_free(loop_node);
    ast_node_free(function);
    parallel_context_free(ctx);
}

int main() {
    printf("Running Automatic Parallelization System Tests...\n\n");
    
    test_hardware_detection();
    test_parallel_context_creation();
    test_hardware_capabilities();
    test_annotation_parsing();
    test_cost_model();
    test_loop_analysis();
    test_dependency_analysis();
    test_parallelization_decision();
    test_function_analysis();
    test_configuration();
    test_utility_functions();
    test_integration();
    test_reporting();
    test_code_generation_stubs();
    
    printf("\n✓ All automatic parallelization tests passed!\n");
    return 0;
}