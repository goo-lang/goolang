#include "memory_safety.h"
#include "types.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Test framework integration
#ifdef STANDALONE_TEST
#define TEST_MAIN main
#else
extern void test_framework_register_test(const char* name, int (*test_func)(void));
#define TEST_MAIN escape_analysis_test_main
#endif

// Mock AST nodes for testing
static ASTNode* create_mock_function(const char* name) {
    FuncDeclNode* func = malloc(sizeof(FuncDeclNode));
    func->base.type = AST_FUNC_DECL;
    func->base.pos = (Position){1, 1, 0, "test.goo"};
    
    func->name = strdup(name);
    func->params = NULL;
    func->return_type = NULL;
    func->body = NULL;
    
    return (ASTNode*)func;
}

static void cleanup_mock_function(ASTNode* func) {
    if (!func) return;
    
    FuncDeclNode* func_decl = (FuncDeclNode*)func;
    free(func_decl->name);
    free(func_decl);
}

// Test escape analyzer creation and cleanup
int test_escape_analyzer_creation() {
    printf("Testing escape analyzer creation...\n");
    
    EscapeAnalyzer* analyzer = escape_analyzer_new(NULL);
    assert(analyzer != NULL);
    assert(analyzer->function_count == 0);
    assert(analyzer->call_site_count == 0);
    assert(analyzer->context_count == 0);
    assert(analyzer->enable_region_analysis == 1);
    assert(analyzer->aggressive_stack_allocation == 1);
    
    escape_analyzer_free(analyzer);
    
    printf("✓ Escape analyzer creation test passed\n");
    return 1;
}

// Test function registration
int test_function_registration() {
    printf("Testing function registration...\n");
    
    EscapeAnalyzer* analyzer = escape_analyzer_new(NULL);
    assert(analyzer != NULL);
    
    // Create mock functions
    ASTNode* func1 = create_mock_function("test_function_1");
    ASTNode* func2 = create_mock_function("test_function_2");
    
    // Register functions
    int result1 = escape_analyzer_register_function(analyzer, "test_function_1", func1);
    int result2 = escape_analyzer_register_function(analyzer, "test_function_2", func2);
    
    assert(result1 == 1);
    assert(result2 == 1);
    assert(analyzer->function_count == 2);
    
    // Test function lookup
    FunctionEscapeInfo* info1 = escape_analyzer_find_function(analyzer, "test_function_1");
    FunctionEscapeInfo* info2 = escape_analyzer_find_function(analyzer, "test_function_2");
    FunctionEscapeInfo* info3 = escape_analyzer_find_function(analyzer, "nonexistent");
    
    assert(info1 != NULL);
    assert(info2 != NULL);
    assert(info3 == NULL);
    assert(strcmp(info1->function_name, "test_function_1") == 0);
    assert(strcmp(info2->function_name, "test_function_2") == 0);
    
    // Cleanup
    cleanup_mock_function(func1);
    cleanup_mock_function(func2);
    escape_analyzer_free(analyzer);
    
    printf("✓ Function registration test passed\n");
    return 1;
}

// Test escape context creation
int test_escape_context_creation() {
    printf("Testing escape context creation...\n");
    
    ASTNode* mock_site = create_mock_function("test_site");
    ASTNode* mock_target = create_mock_function("test_target");
    
    EscapeContext* context = escape_context_new(mock_site, mock_target, ESCAPE_FUNCTION, 1);
    assert(context != NULL);
    assert(context->escape_site == mock_site);
    assert(context->target_function == mock_target);
    assert(context->escape_kind == ESCAPE_FUNCTION);
    assert(context->call_depth == 1);
    assert(context->is_conditional == 0);
    assert(context->escape_probability == 1.0);
    
    escape_context_free(context);
    cleanup_mock_function(mock_site);
    cleanup_mock_function(mock_target);
    
    printf("✓ Escape context creation test passed\n");
    return 1;
}

// Test allocation strategy determination
int test_allocation_strategy_determination() {
    printf("Testing allocation strategy determination...\n");
    
    EscapeAnalyzer* analyzer = escape_analyzer_new(NULL);
    assert(analyzer != NULL);
    
    ASTNode* mock_site = create_mock_function("test_alloc");
    
    // Test different escape kinds
    AllocationStrategy strategy1 = determine_allocation_strategy(analyzer, mock_site, ESCAPE_NONE);
    AllocationStrategy strategy2 = determine_allocation_strategy(analyzer, mock_site, ESCAPE_FUNCTION);
    AllocationStrategy strategy3 = determine_allocation_strategy(analyzer, mock_site, ESCAPE_GLOBAL);
    AllocationStrategy strategy4 = determine_allocation_strategy(analyzer, mock_site, ESCAPE_THREAD);
    
    assert(strategy1 == ALLOC_STRATEGY_STACK);
    assert(strategy2 == ALLOC_STRATEGY_REGION); // With region analysis enabled
    assert(strategy3 == ALLOC_STRATEGY_GLOBAL);
    assert(strategy4 == ALLOC_STRATEGY_THREAD_LOCAL);
    
    // Test with region analysis disabled
    analyzer->enable_region_analysis = 0;
    AllocationStrategy strategy5 = determine_allocation_strategy(analyzer, mock_site, ESCAPE_FUNCTION);
    assert(strategy5 == ALLOC_STRATEGY_HEAP);
    
    cleanup_mock_function(mock_site);
    escape_analyzer_free(analyzer);
    
    printf("✓ Allocation strategy determination test passed\n");
    return 1;
}

// Test object lifetime determination
int test_object_lifetime_determination() {
    printf("Testing object lifetime determination...\n");
    
    EscapeAnalyzer* analyzer = escape_analyzer_new(NULL);
    assert(analyzer != NULL);
    
    ASTNode* mock_site = create_mock_function("test_lifetime");
    
    // Test different escape kinds
    ObjectLifetime lifetime1 = determine_object_lifetime(analyzer, mock_site, ESCAPE_NONE);
    ObjectLifetime lifetime2 = determine_object_lifetime(analyzer, mock_site, ESCAPE_FUNCTION);
    ObjectLifetime lifetime3 = determine_object_lifetime(analyzer, mock_site, ESCAPE_CLOSURE);
    ObjectLifetime lifetime4 = determine_object_lifetime(analyzer, mock_site, ESCAPE_GLOBAL);
    
    assert(lifetime1 == LIFETIME_LOCAL);
    assert(lifetime2 == LIFETIME_RETURN);
    assert(lifetime3 == LIFETIME_CLOSURE_CAPTURE);
    assert(lifetime4 == LIFETIME_GLOBAL);
    
    cleanup_mock_function(mock_site);
    escape_analyzer_free(analyzer);
    
    printf("✓ Object lifetime determination test passed\n");
    return 1;
}

// Test utility functions
int test_utility_functions() {
    printf("Testing utility functions...\n");
    
    // Test string conversion functions
    const char* lifetime_str = object_lifetime_to_string(LIFETIME_LOCAL);
    const char* alloc_str = allocation_strategy_to_string(ALLOC_STRATEGY_STACK);
    const char* escape_str = escape_kind_to_string(ESCAPE_FUNCTION);
    
    assert(strcmp(lifetime_str, "local") == 0);
    assert(strcmp(alloc_str, "stack") == 0);
    assert(strcmp(escape_str, "function") == 0);
    
    printf("✓ Utility functions test passed\n");
    return 1;
}

// Test statistics and reporting
int test_statistics_reporting() {
    printf("Testing statistics and reporting...\n");
    
    EscapeAnalyzer* analyzer = escape_analyzer_new(NULL);
    assert(analyzer != NULL);
    
    // Initialize some statistics
    analyzer->functions_analyzed = 5;
    analyzer->stack_allocations_recommended = 10;
    analyzer->heap_allocations_required = 3;
    analyzer->region_allocations_created = 2;
    
    // Test statistics printing (just verify it doesn't crash)
    printf("--- Statistics Output ---\n");
    escape_analyzer_print_statistics(analyzer);
    printf("--- End Statistics ---\n");
    
    escape_analyzer_free(analyzer);
    
    printf("✓ Statistics reporting test passed\n");
    return 1;
}

// Test comprehensive escape analysis workflow
int test_escape_analysis_workflow() {
    printf("Testing escape analysis workflow...\n");
    
    EscapeAnalyzer* analyzer = escape_analyzer_new(NULL);
    assert(analyzer != NULL);
    
    // Create mock functions
    ASTNode* func1 = create_mock_function("main");
    ASTNode* func2 = create_mock_function("helper");
    
    // Register functions
    escape_analyzer_register_function(analyzer, "main", func1);
    escape_analyzer_register_function(analyzer, "helper", func2);
    
    // Find and analyze a function
    FunctionEscapeInfo* main_info = escape_analyzer_find_function(analyzer, "main");
    assert(main_info != NULL);
    
    int analysis_result = analyze_function_escape(analyzer, main_info);
    assert(analysis_result == 1);
    assert(main_info->is_analyzed == 1);
    assert(analyzer->functions_analyzed == 1);
    
    // Print function information
    printf("--- Function Info Output ---\n");
    escape_analyzer_print_function_info(analyzer, "main");
    printf("--- End Function Info ---\n");
    
    cleanup_mock_function(func1);
    cleanup_mock_function(func2);
    escape_analyzer_free(analyzer);
    
    printf("✓ Escape analysis workflow test passed\n");
    return 1;
}

// Run all tests
int TEST_MAIN() {
    printf("=== Interprocedural Escape Analysis Tests ===\n\n");
    
    int tests_passed = 0;
    int total_tests = 8;
    
    tests_passed += test_escape_analyzer_creation();
    tests_passed += test_function_registration();
    tests_passed += test_escape_context_creation();
    tests_passed += test_allocation_strategy_determination();
    tests_passed += test_object_lifetime_determination();
    tests_passed += test_utility_functions();
    tests_passed += test_statistics_reporting();
    tests_passed += test_escape_analysis_workflow();
    
    printf("\n=== Test Results ===\n");
    printf("Tests passed: %d/%d\n", tests_passed, total_tests);
    
    if (tests_passed == total_tests) {
        printf("✓ All escape analysis tests passed!\n");
        return 0;
    } else {
        printf("✗ Some tests failed!\n");
        return 1;
    }
}

#ifndef STANDALONE_TEST
// Register tests with the framework
void register_escape_analysis_tests() {
    test_framework_register_test("escape_analyzer_creation", test_escape_analyzer_creation);
    test_framework_register_test("function_registration", test_function_registration);
    test_framework_register_test("escape_context_creation", test_escape_context_creation);
    test_framework_register_test("allocation_strategy_determination", test_allocation_strategy_determination);
    test_framework_register_test("object_lifetime_determination", test_object_lifetime_determination);
    test_framework_register_test("utility_functions", test_utility_functions);
    test_framework_register_test("statistics_reporting", test_statistics_reporting);
    test_framework_register_test("escape_analysis_workflow", test_escape_analysis_workflow);
}
#endif