#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "runtime_optimization.h"
#include "memory_safety.h"

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
// Helper Functions for Creating Test AST Nodes
// =============================================================================

ASTNode* create_test_identifier(const char* name) {
    IdentifierNode* node = malloc(sizeof(IdentifierNode));
    node->base.type = AST_IDENTIFIER;
    node->name = strdup(name);
    return (ASTNode*)node;
}

ASTNode* create_test_literal_int(int value) {
    LiteralNode* node = malloc(sizeof(LiteralNode));
    node->base.type = AST_LITERAL;
    node->literal_type = TOKEN_INT;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    node->value = strdup(buf);
    return (ASTNode*)node;
}

ASTNode* create_test_index_expr(ASTNode* object, ASTNode* index) {
    IndexExprNode* node = malloc(sizeof(IndexExprNode));
    node->base.type = AST_INDEX_EXPR;
    node->expr = object;
    node->index = index;
    return (ASTNode*)node;
}

ASTNode* create_test_for_loop(ASTNode* init, ASTNode* condition, ASTNode* update, ASTNode* body) {
    ForStmtNode* node = malloc(sizeof(ForStmtNode));
    node->base.type = AST_FOR_STMT;
    node->init = init;
    node->condition = condition;
    node->post = update;
    node->body = body;
    return (ASTNode*)node;
}

ASTNode* create_test_if_stmt(ASTNode* condition, ASTNode* then_stmt, ASTNode* else_stmt) {
    IfStmtNode* node = malloc(sizeof(IfStmtNode));
    node->base.type = AST_IF_STMT;
    node->condition = condition;
    node->then_stmt = then_stmt;
    node->else_stmt = else_stmt;
    return (ASTNode*)node;
}

void free_test_ast_node(ASTNode* node) {
    if (!node) return;
    
    switch (node->type) {
        case AST_IDENTIFIER: {
            IdentifierNode* id = (IdentifierNode*)node;
            free(id->name);
            break;
        }
        case AST_LITERAL: {
            LiteralNode* lit = (LiteralNode*)node;
            free(lit->value);
            break;
        }
        case AST_INDEX_EXPR: {
            IndexExprNode* idx = (IndexExprNode*)node;
            free_test_ast_node(idx->expr);
            free_test_ast_node(idx->index);
            break;
        }
        case AST_FOR_STMT: {
            ForStmtNode* for_stmt = (ForStmtNode*)node;
            free_test_ast_node(for_stmt->init);
            free_test_ast_node(for_stmt->condition);
            free_test_ast_node(for_stmt->post);
            free_test_ast_node(for_stmt->body);
            break;
        }
        case AST_IF_STMT: {
            IfStmtNode* if_stmt = (IfStmtNode*)node;
            free_test_ast_node(if_stmt->condition);
            free_test_ast_node(if_stmt->then_stmt);
            free_test_ast_node(if_stmt->else_stmt);
            break;
        }
        default:
            break;
    }
    
    free(node);
}

// =============================================================================
// Test Functions
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

bool test_bounds_check_elimination_analysis() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    // Create test AST: arr[5] (constant index)
    ASTNode* arr = create_test_identifier("arr");
    ASTNode* index = create_test_literal_int(5);
    ASTNode* index_expr = create_test_index_expr(arr, index);
    
    BoundsCheckInfo* info = analyze_bounds_check(ctx, index_expr);
    ASSERT_NOT_NULL(info);
    ASSERT_NOT_NULL(info->elimination_reason);
    
    // Test elimination decision
    bool can_eliminate = can_eliminate_runtime_bounds_check(ctx, info);
    printf("\n  Can eliminate bounds check: %s", can_eliminate ? "yes" : "no");
    printf("\n  Reason: %s", info->elimination_reason);
    
    bounds_check_info_free(info);
    free_test_ast_node(index_expr);
    optimization_context_free(ctx);
    
    return true;
}

bool test_bounds_check_elimination_safety_levels() {
    // Test that debug mode never eliminates bounds checks
    OptimizationContext* debug_ctx = optimization_context_new(OPT_SAFETY_DEBUG);
    ASSERT_NOT_NULL(debug_ctx);
    
    ASTNode* arr = create_test_identifier("arr");
    ASTNode* index = create_test_literal_int(0);
    ASTNode* index_expr = create_test_index_expr(arr, index);
    
    BoundsCheckInfo* info = analyze_bounds_check(debug_ctx, index_expr);
    ASSERT_NOT_NULL(info);
    
    // Debug mode should never eliminate
    bool can_eliminate_debug = can_eliminate_runtime_bounds_check(debug_ctx, info);
    ASSERT_FALSE(can_eliminate_debug);
    
    bounds_check_info_free(info);
    optimization_context_free(debug_ctx);
    
    // Test that aggressive mode is more permissive
    OptimizationContext* aggressive_ctx = optimization_context_new(OPT_SAFETY_AGGRESSIVE);
    ASSERT_NOT_NULL(aggressive_ctx);
    
    info = analyze_bounds_check(aggressive_ctx, index_expr);
    ASSERT_NOT_NULL(info);
    
    bool can_eliminate_aggressive = can_eliminate_runtime_bounds_check(aggressive_ctx, info);
    printf("\n  Debug can eliminate: %s, Aggressive can eliminate: %s",
           can_eliminate_debug ? "yes" : "no",
           can_eliminate_aggressive ? "yes" : "no");
    
    bounds_check_info_free(info);
    free_test_ast_node(index_expr);
    optimization_context_free(aggressive_ctx);
    
    return true;
}

bool test_null_check_elimination() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    // Create test AST: *ptr (pointer dereference)
    ASTNode* ptr = create_test_identifier("ptr");
    UnaryExprNode* deref = malloc(sizeof(UnaryExprNode));
    deref->base.type = AST_UNARY_EXPR;
    deref->operator = TOKEN_MULTIPLY;
    deref->operand = ptr;
    
    bool can_eliminate = can_eliminate_null_check(ctx, (ASTNode*)deref);
    printf("\n  Can eliminate null check: %s", can_eliminate ? "yes" : "no");
    
    if (can_eliminate) {
        int result = eliminate_null_check(ctx, (ASTNode*)deref);
        ASSERT_EQ(result, 0);
        ASSERT_EQ(ctx->null_checks_eliminated, 1);
    }
    
    free_test_ast_node((ASTNode*)deref);
    optimization_context_free(ctx);
    
    return true;
}

bool test_branch_prediction_analysis() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    // Create test AST: if (ptr == null)
    ASTNode* ptr = create_test_identifier("ptr");
    LiteralNode* null_lit = malloc(sizeof(LiteralNode));
    null_lit->base.type = AST_LITERAL;
    null_lit->literal_type = TOKEN_NIL;
    null_lit->value = strdup("nil");
    
    BinaryExprNode* condition = malloc(sizeof(BinaryExprNode));
    condition->base.type = AST_BINARY_EXPR;
    condition->operator = TOKEN_EQ;
    condition->left = ptr;
    condition->right = (ASTNode*)null_lit;
    
    ASTNode* then_stmt = create_test_identifier("error_handler");
    ASTNode* if_stmt = create_test_if_stmt((ASTNode*)condition, then_stmt, NULL);
    
    BranchInfo* info = analyze_branch(ctx, if_stmt);
    ASSERT_NOT_NULL(info);
    
    printf("\n  Branch predictable: %s", info->is_predictable ? "yes" : "no");
    printf("\n  Predicted probability: %.2f", info->predicted_probability);
    
    if (info->is_predictable) {
        int result = optimize_branch_prediction(ctx, info);
        ASSERT_EQ(result, 0);
        ASSERT_EQ(ctx->branches_optimized, 1);
    }
    
    branch_info_free(info);
    free_test_ast_node(if_stmt);
    optimization_context_free(ctx);
    
    return true;
}

bool test_loop_optimization_analysis() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    // Create test AST: for (i = 0; i < n; i++)
    ASTNode* init = create_test_identifier("i_init");
    ASTNode* condition = create_test_identifier("i_condition");
    ASTNode* update = create_test_identifier("i_update");
    ASTNode* body = create_test_identifier("loop_body");
    
    ASTNode* for_loop = create_test_for_loop(init, condition, update, body);
    
    LoopOptInfo* info = analyze_loop(ctx, for_loop);
    ASSERT_NOT_NULL(info);
    
    printf("\n  Can vectorize: %s", info->can_vectorize ? "yes" : "no");
    printf("\n  Can unroll: %s", info->can_unroll ? "yes" : "no");
    printf("\n  Can prefetch: %s", info->can_prefetch ? "yes" : "no");
    printf("\n  Vector width: %zu", info->vector_width);
    printf("\n  Unroll factor: %zu", info->unroll_factor);
    
    int result = optimize_loop(ctx, info);
    if (result == 0) {
        ASSERT_EQ(ctx->loops_optimized, 1);
    }
    
    loop_opt_info_free(info);
    free_test_ast_node(for_loop);
    optimization_context_free(ctx);
    
    return true;
}

bool test_speculative_execution() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    ASTNode* speculation_point = create_test_identifier("speculation_point");
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
    free_test_ast_node(speculation_point);
    optimization_context_free(ctx);
    
    return true;
}

bool test_vectorization_capability() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    // Test with vectorization enabled
    ctx->enable_vectorization = true;
    ctx->hw_capabilities |= HW_CAP_AVX512; // Simulate AVX-512 support
    
    ASTNode* for_loop = create_test_for_loop(
        create_test_identifier("init"),
        create_test_identifier("condition"),
        create_test_identifier("update"),
        create_test_identifier("body")
    );
    
    bool can_vectorize = can_vectorize_loop(ctx, for_loop);
    ASSERT_TRUE(can_vectorize);
    
    int result = vectorize_loop(ctx, for_loop, 16);
    ASSERT_EQ(result, 0);
    
    // Test with vectorization disabled
    ctx->enable_vectorization = false;
    bool cannot_vectorize = can_vectorize_loop(ctx, for_loop);
    ASSERT_FALSE(cannot_vectorize);
    
    free_test_ast_node(for_loop);
    optimization_context_free(ctx);
    
    return true;
}

bool test_memory_prefetch() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    ctx->enable_prefetch = true;
    ctx->hw_capabilities |= HW_CAP_PREFETCH;
    
    ASTNode* memory_access = create_test_index_expr(
        create_test_identifier("array"),
        create_test_identifier("index")
    );
    
    int result = enable_memory_prefetch(ctx, memory_access);
    ASSERT_EQ(result, 0);
    
    // Test with prefetch disabled
    ctx->enable_prefetch = false;
    result = enable_memory_prefetch(ctx, memory_access);
    ASSERT_NE(result, 0);
    
    free_test_ast_node(memory_access);
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
    
    ASTNode* target = create_test_identifier("target");
    
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
    free_test_ast_node(target);
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
    
    ASTNode* target = create_test_identifier("benchmark_target");
    
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
    free_test_ast_node(target);
    optimization_context_free(ctx);
    
    return true;
}

bool test_integration_with_proof_system() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    // Create a mock proof context (in practice, this would be real)
    struct ProofGenerationContext* proof_ctx = NULL; // Would be initialized properly
    
    int result = optimization_context_set_proofs(ctx, proof_ctx);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(ctx->proof_ctx, proof_ctx);
    
    // Test proof generation for optimization
    ASTNode* target = create_test_identifier("optimization_target");
    struct ProofReport* proof = generate_optimization_safety_proof(
        ctx, OPT_BOUNDS_CHECK_ELIMINATION, target
    );
    
    // Note: This will be NULL since we don't have a real proof context
    // In a full implementation, this would return a real proof
    if (proof) {
        ASSERT_TRUE(proof->total_proofs_generated > 0);
        free(proof);
    }
    
    free_test_ast_node(target);
    optimization_context_free(ctx);
    
    return true;
}

bool test_integration_with_contract_system() {
    OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
    ASSERT_NOT_NULL(ctx);
    
    // Create a mock contract context
    struct ContractContext* contract_ctx = NULL; // Would be initialized properly
    
    int result = optimization_context_set_contracts(ctx, contract_ctx);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(ctx->contract_ctx, contract_ctx);
    
    // Test contract creation for optimization
    ASTNode* target = create_test_identifier("contract_target");
    ContractExpression* contract = create_optimization_contract(
        ctx, OPT_BOUNDS_CHECK_ELIMINATION, target
    );
    
    ASSERT_NOT_NULL(contract);
    ASSERT_EQ(contract->type, CONTRACT_ASSUMPTION);
    ASSERT_TRUE(contract->is_compile_time);
    
    free(contract);
    free_test_ast_node(target);
    optimization_context_free(ctx);
    
    return true;
}

// =============================================================================
// Performance and Stress Tests
// =============================================================================

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
    const int iterations = 1000;
    
    for (int i = 0; i < iterations; i++) {
        OptimizationContext* ctx = optimization_context_new(OPT_SAFETY_BALANCED);
        ASSERT_NOT_NULL(ctx);
        
        // Perform various operations
        ASTNode* target = create_test_identifier("test_target");
        BoundsCheckInfo* bounds_info = analyze_bounds_check(ctx, 
            create_test_index_expr(target, create_test_literal_int(0)));
        
        if (bounds_info) {
            bounds_check_info_free(bounds_info);
        }
        
        free_test_ast_node(target);
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
    printf(ANSI_COLOR_BLUE "Starting Runtime Optimization Framework Tests..." ANSI_COLOR_RESET "\n");
    
    // Initialize memory safety system (commented out for now)
    // if (memory_safety_init() != 0) {
    //     printf(ANSI_COLOR_RED "Failed to initialize memory safety system" ANSI_COLOR_RESET "\n");
    //     return 1;
    // }
    
    // Run basic functionality tests
    TEST(optimization_context_creation);
    TEST(hardware_capability_detection);
    TEST(bounds_check_elimination_analysis);
    TEST(bounds_check_elimination_safety_levels);
    TEST(null_check_elimination);
    TEST(branch_prediction_analysis);
    TEST(loop_optimization_analysis);
    TEST(speculative_execution);
    TEST(vectorization_capability);
    TEST(memory_prefetch);
    TEST(adaptive_optimization);
    TEST(profile_data_management);
    TEST(optimization_diagnostics);
    TEST(error_handling);
    TEST(optimization_benchmarking);
    
    // Integration tests
    TEST(integration_with_proof_system);
    TEST(integration_with_contract_system);
    
    // Performance and stress tests
    TEST(performance_profile_collection);
    TEST(memory_usage);
    
    print_test_summary();
    
    // Cleanup (commented out for now)
    // memory_safety_cleanup();
    
    return (test_failed > 0) ? 1 : 0;
}
