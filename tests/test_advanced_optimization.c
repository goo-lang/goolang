#include "advanced_optimization.h"
#include "comptime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Mock AST node creation for testing
static ASTNode* create_mock_loop_node(void) {
    ASTNode* node = malloc(sizeof(ASTNode));
    if (!node) return NULL;
    
    node->type = AST_FOR_STMT;
    node->pos.line = 10;
    node->pos.column = 5;
    node->pos.offset = 100;
    node->pos.filename = "test.goo";
    node->node_type = NULL;
    node->next = NULL;
    
    return node;
}

static ASTNode* create_mock_function_node(void) {
    ASTNode* node = malloc(sizeof(ASTNode));
    if (!node) return NULL;
    
    node->type = AST_FUNC_DECL;
    node->pos.line = 20;
    node->pos.column = 1;
    node->pos.offset = 200;
    node->pos.filename = "test.goo";
    node->node_type = NULL;
    node->next = NULL;
    
    return node;
}

static ASTNode* create_mock_allocation_node(void) {
    ASTNode* node = malloc(sizeof(ASTNode));
    if (!node) return NULL;
    
    node->type = AST_VAR_DECL;
    node->pos.line = 30;
    node->pos.column = 8;
    node->pos.offset = 300;
    node->pos.filename = "test.goo";
    node->node_type = NULL;
    node->next = NULL;
    
    return node;
}

static void cleanup_mock_node(ASTNode* node) {
    if (node) {
        free(node);
    }
}

// Test strategy manager
void test_strategy_manager(void) {
    printf("Testing strategy manager...\n");
    
    // Create and destroy empty manager
    StrategyManager* manager = create_strategy_manager();
    assert(manager != NULL);
    assert(manager->strategy_count == 0);
    assert(manager->capacity == 0);
    assert(manager->pgo_data == NULL);
    
    destroy_strategy_manager(manager);
    
    printf("✓ Strategy manager tests passed!\n");
}

// Test vectorization analysis
void test_vectorization_analysis(void) {
    printf("Testing vectorization analysis...\n");
    
    ASTNode* loop_node = create_mock_loop_node();
    assert(loop_node != NULL);
    
    AdvancedStrategy* strategy = analyze_vectorization(loop_node);
    assert(strategy != NULL);
    assert(strategy->type == STRATEGY_VECTORIZATION);
    assert(strategy->strategy_name != NULL);
    assert(strcmp(strategy->strategy_name, "Auto-Vectorization Analysis") == 0);
    assert(strategy->confidence_score > 0.0);
    
    VectorizationAnalysis* vec = &strategy->strategy_data.vectorization;
    assert(vec->can_vectorize == true);
    assert(vec->vector_width == 4);
    assert(vec->requires_alignment == true);
    assert(vec->has_dependencies == false);
    assert(vec->dependency_info != NULL);
    
    // Test application
    bool applied = apply_vectorization_strategy(loop_node, vec);
    assert(applied == true);
    
    // Cleanup
    free(strategy->strategy_name);
    free(strategy->analysis_data);
    free(vec->dependency_info);
    free(strategy);
    cleanup_mock_node(loop_node);
    
    printf("✓ Vectorization analysis tests passed!\n");
}

// Test inlining analysis
void test_inlining_analysis(void) {
    printf("Testing inlining analysis...\n");
    
    ASTNode* function_node = create_mock_function_node();
    assert(function_node != NULL);
    
    // Test hot function (should inline)
    AdvancedStrategy* hot_strategy = analyze_inlining(function_node, 150);
    assert(hot_strategy != NULL);
    assert(hot_strategy->type == STRATEGY_INLINING);
    
    InliningAnalysis* hot_inline = &hot_strategy->strategy_data.inlining;
    assert(hot_inline->call_frequency == 150);
    assert(hot_inline->should_inline == true);
    assert(hot_inline->inline_reason != NULL);
    
    // Test application
    bool applied = apply_inlining_strategy(function_node, hot_inline);
    assert(applied == true);
    
    // Test cold function (should not inline)
    AdvancedStrategy* cold_strategy = analyze_inlining(function_node, 10);
    assert(cold_strategy != NULL);
    
    InliningAnalysis* cold_inline = &cold_strategy->strategy_data.inlining;
    assert(cold_inline->call_frequency == 10);
    assert(cold_inline->should_inline == false);
    
    // Cleanup
    free(hot_strategy->strategy_name);
    free(hot_strategy->analysis_data);
    free(hot_inline->inline_reason);
    free(hot_strategy);
    
    free(cold_strategy->strategy_name);
    free(cold_strategy->analysis_data);
    free(cold_inline->inline_reason);
    free(cold_strategy);
    
    cleanup_mock_node(function_node);
    
    printf("✓ Inlining analysis tests passed!\n");
}

// Test escape analysis
void test_escape_analysis(void) {
    printf("Testing escape analysis...\n");
    
    ASTNode* allocation_node = create_mock_allocation_node();
    assert(allocation_node != NULL);
    
    AdvancedStrategy* strategy = analyze_escape(allocation_node);
    assert(strategy != NULL);
    assert(strategy->type == STRATEGY_ESCAPE_ANALYSIS);
    
    EscapeAnalysis* escape = &strategy->strategy_data.escape;
    assert(escape->escapes == false);
    assert(escape->stack_allocatable == true);
    assert(escape->needs_heap == false);
    assert(escape->escape_path != NULL);
    
    // Test application
    bool applied = apply_escape_strategy(allocation_node, escape);
    assert(applied == true);
    
    // Cleanup
    free(strategy->strategy_name);
    free(strategy->analysis_data);
    free(escape->escape_path);
    free(strategy);
    cleanup_mock_node(allocation_node);
    
    printf("✓ Escape analysis tests passed!\n");
}

// Test loop optimization
void test_loop_optimization(void) {
    printf("Testing loop optimization...\n");
    
    ASTNode* loop_node = create_mock_loop_node();
    assert(loop_node != NULL);
    
    AdvancedStrategy* strategy = analyze_loop_optimization(loop_node);
    assert(strategy != NULL);
    assert(strategy->type == STRATEGY_LOOP_UNROLLING);
    
    LoopOptimization* loop = &strategy->strategy_data.loop;
    assert(loop->can_unroll == true);
    assert(loop->unroll_factor == 4);
    assert(loop->vectorizable == true);
    assert(loop->has_invariants == true);
    assert(loop->optimization_notes != NULL);
    
    // Test application
    bool applied = apply_loop_strategy(loop_node, loop);
    assert(applied == true);
    
    // Cleanup
    free(strategy->strategy_name);
    free(strategy->analysis_data);
    free(loop->optimization_notes);
    free(strategy);
    cleanup_mock_node(loop_node);
    
    printf("✓ Loop optimization tests passed!\n");
}

// Test PGO integration
void test_pgo_integration(void) {
    printf("Testing PGO integration...\n");
    
    StrategyManager* manager = create_strategy_manager();
    assert(manager != NULL);
    
    // Create mock profile data (simplified for test)
    ProfileData* profile = malloc(sizeof(ProfileData));
    assert(profile != NULL);
    profile->source_file = strdup("test_integration.pgo");
    profile->total_samples = 5000;
    profile->collection_time = 2.5;
    profile->hot_functions = NULL;
    profile->hot_function_count = 0;
    profile->cold_functions = NULL;
    profile->cold_function_count = 0;
    profile->functions = NULL;
    profile->loops = NULL;
    profile->call_sites = NULL;
    
    // Integrate PGO data
    integrate_pgo_data(manager, profile);
    assert(manager->pgo_data == profile);
    
    // Update strategies from profile
    update_strategies_from_profile(manager, profile);
    
    // Test integration with optimization directives (simplified)
    printf("Integration with optimization directives simulated\n");
    
    // Cleanup
    destroy_strategy_manager(manager);
    free(profile->source_file);
    free(profile);
    
    printf("✓ PGO integration tests passed!\n");
}

// Test compile-time intrinsics
void test_comptime_intrinsics(void) {
    printf("Testing compile-time intrinsics...\n");
    
    // Create mock compile-time values
    ComptimeValue* target = malloc(sizeof(ComptimeValue));
    target->type = COMPTIME_VALUE_STRING;
    target->string_value = strdup("test_function");
    
    ComptimeValue* strategy_type = malloc(sizeof(ComptimeValue));
    strategy_type->type = COMPTIME_VALUE_STRING;
    strategy_type->string_value = strdup("vectorization");
    
    ComptimeValue* vector_width = malloc(sizeof(ComptimeValue));
    vector_width->type = COMPTIME_VALUE_INT;
    vector_width->int_value = 8;
    
    // Test intrinsics
    ComptimeValue* strategy_result = comptime_get_optimization_strategy(target, strategy_type);
    assert(strategy_result != NULL);
    assert(strategy_result->type == COMPTIME_VALUE_STRING);
    assert(strcmp(strategy_result->string_value, "vectorization_recommended") == 0);
    
    ComptimeValue* vec_result = comptime_force_vectorization(target, vector_width);
    assert(vec_result != NULL);
    assert(vec_result->type == COMPTIME_VALUE_BOOL);
    assert(vec_result->bool_value == true);
    
    ComptimeValue* inline_result = comptime_inline_aggressively(target);
    assert(inline_result != NULL);
    assert(inline_result->type == COMPTIME_VALUE_BOOL);
    assert(inline_result->bool_value == true);
    
    ComptimeValue* stack_result = comptime_stack_allocate(target);
    assert(stack_result != NULL);
    assert(stack_result->type == COMPTIME_VALUE_BOOL);
    assert(stack_result->bool_value == true);
    
    // Cleanup
    free(target->string_value);
    free(target);
    free(strategy_type->string_value);
    free(strategy_type);
    free(vector_width);
    free(strategy_result->string_value);
    free(strategy_result);
    free(vec_result);
    free(inline_result);
    free(stack_result);
    
    printf("✓ Compile-time intrinsics tests passed!\n");
}

// Test reporting functions
void test_reporting(void) {
    printf("Testing reporting functions...\n");
    
    StrategyManager* manager = create_strategy_manager();
    assert(manager != NULL);
    
    // Create some test strategies
    ASTNode* loop_node = create_mock_loop_node();
    ASTNode* function_node = create_mock_function_node();
    
    AdvancedStrategy* vec_strategy = analyze_vectorization(loop_node);
    AdvancedStrategy* inline_strategy = analyze_inlining(function_node, 200);
    
    // Manually add strategies to manager for testing
    manager->strategies = malloc(2 * sizeof(AdvancedStrategy));
    manager->strategies[0] = *vec_strategy;
    manager->strategies[1] = *inline_strategy;
    manager->strategy_count = 2;
    manager->capacity = 2;
    
    // Test reporting
    print_strategy_report(manager);
    
    char* summary = get_strategy_summary(manager);
    assert(summary != NULL);
    assert(strstr(summary, "2 strategies") != NULL);
    
    // Cleanup
    free(summary);
    
    // Don't free the strategies themselves since they're now owned by manager
    free(vec_strategy);
    free(inline_strategy);
    cleanup_mock_node(loop_node);
    cleanup_mock_node(function_node);
    
    destroy_strategy_manager(manager);
    
    printf("✓ Reporting tests passed!\n");
}

int main(void) {
    printf("Running Advanced Optimization Strategy tests...\n\n");
    
    test_strategy_manager();
    test_vectorization_analysis();
    test_inlining_analysis();
    test_escape_analysis();
    test_loop_optimization();
    test_pgo_integration();
    test_comptime_intrinsics();
    test_reporting();
    
    printf("\n✅ All Advanced Optimization Strategy tests passed!\n");
    return 0;
}
