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
#define TEST_MAIN resource_manager_test_main
#endif

// Mock type checker for testing
static TypeChecker* create_mock_type_checker() {
    TypeChecker* tc = malloc(sizeof(TypeChecker));
    if (tc) {
        memset(tc, 0, sizeof(TypeChecker));
    }
    return tc;
}

static void cleanup_mock_type_checker(TypeChecker* tc) {
    free(tc);
}

// Mock AST nodes for testing
static ASTNode* create_mock_var_decl(const char* name, const char* init_func) {
    VarDeclNode* var_decl = malloc(sizeof(VarDeclNode));
    var_decl->base.type = AST_VAR_DECL;
    var_decl->base.pos = (Position){1, 1, 0, "test.goo"};
    
    // Set up variable name
    var_decl->names = malloc(sizeof(char*));
    var_decl->names[0] = strdup(name);
    var_decl->name_count = 1;
    
    // Create initialization expression (function call)
    if (init_func) {
        CallExprNode* call = malloc(sizeof(CallExprNode));
        call->base.type = AST_CALL_EXPR;
        call->base.pos = (Position){1, 1, 0, "test.goo"};
        
        IdentifierNode* func_name = malloc(sizeof(IdentifierNode));
        func_name->base.type = AST_IDENTIFIER;
        func_name->base.pos = (Position){1, 1, 0, "test.goo"};
        func_name->name = strdup(init_func);
        
        call->function = (ASTNode*)func_name;
        call->args = NULL;
        
        var_decl->values = (ASTNode*)call;
    } else {
        var_decl->values = NULL;
    }
    
    var_decl->type = NULL;
    var_decl->ownership = 0;
    var_decl->is_short_decl = 0;
    
    return (ASTNode*)var_decl;
}

static ASTNode* create_mock_defer_stmt(const char* func_name) {
    DeferStmtNode* defer_stmt = malloc(sizeof(DeferStmtNode));
    defer_stmt->base.type = AST_DEFER_STMT;
    defer_stmt->base.pos = (Position){1, 1, 0, "test.goo"};
    
    // Create deferred function call
    CallExprNode* call = malloc(sizeof(CallExprNode));
    call->base.type = AST_CALL_EXPR;
    call->base.pos = (Position){1, 1, 0, "test.goo"};
    
    IdentifierNode* func_id = malloc(sizeof(IdentifierNode));
    func_id->base.type = AST_IDENTIFIER;
    func_id->base.pos = (Position){1, 1, 0, "test.goo"};
    func_id->name = strdup(func_name);
    
    call->function = (ASTNode*)func_id;
    call->args = NULL;
    
    defer_stmt->call = (ASTNode*)call;
    
    return (ASTNode*)defer_stmt;
}

static ASTNode* create_mock_block_stmt() {
    BlockStmtNode* block = malloc(sizeof(BlockStmtNode));
    block->base.type = AST_BLOCK_STMT;
    block->base.pos = (Position){1, 1, 0, "test.goo"};
    block->statements = NULL;
    
    return (ASTNode*)block;
}

static void cleanup_mock_var_decl(ASTNode* node) {
    if (!node) return;
    
    VarDeclNode* var_decl = (VarDeclNode*)node;
    
    // Clean up names
    for (size_t i = 0; i < var_decl->name_count; i++) {
        free(var_decl->names[i]);
    }
    free(var_decl->names);
    
    // Clean up initialization expression
    if (var_decl->values && var_decl->values->type == AST_CALL_EXPR) {
        CallExprNode* call = (CallExprNode*)var_decl->values;
        if (call->function && call->function->type == AST_IDENTIFIER) {
            IdentifierNode* func_name = (IdentifierNode*)call->function;
            free(func_name->name);
            free(func_name);
        }
        free(call);
    }
    
    free(var_decl);
}

static void cleanup_mock_defer_stmt(ASTNode* node) {
    if (!node) return;
    
    DeferStmtNode* defer_stmt = (DeferStmtNode*)node;
    
    if (defer_stmt->call && defer_stmt->call->type == AST_CALL_EXPR) {
        CallExprNode* call = (CallExprNode*)defer_stmt->call;
        if (call->function && call->function->type == AST_IDENTIFIER) {
            IdentifierNode* func_name = (IdentifierNode*)call->function;
            free(func_name->name);
            free(func_name);
        }
        free(call);
    }
    
    free(defer_stmt);
}

// Test resource manager creation and cleanup
int test_resource_manager_creation() {
    printf("Testing resource manager creation...\n");
    
    TypeChecker* tc = create_mock_type_checker();
    ResourceManager* rm = resource_manager_new(tc);
    
    assert(rm != NULL);
    assert(rm->type_checker == tc);
    assert(rm->resource_count == 0);
    assert(rm->pending_count == 0);
    assert(rm->current_scope == NULL);
    assert(rm->enable_raii == 1);
    assert(rm->enable_defer == 1);
    
    resource_manager_free(rm);
    cleanup_mock_type_checker(tc);
    
    printf("✓ Resource manager creation test passed\n");
    return 1;
}

// Test resource type detection
int test_resource_type_detection() {
    printf("Testing resource type detection...\n");
    
    // Test function name to resource type mapping
    assert(get_resource_type_for_function("open") == RESOURCE_TYPE_FILE);
    assert(get_resource_type_for_function("fopen") == RESOURCE_TYPE_FILE);
    assert(get_resource_type_for_function("socket") == RESOURCE_TYPE_NETWORK);
    assert(get_resource_type_for_function("malloc") == RESOURCE_TYPE_MEMORY);
    assert(get_resource_type_for_function("mutex_new") == RESOURCE_TYPE_MUTEX);
    assert(get_resource_type_for_function("thread_create") == RESOURCE_TYPE_THREAD);
    assert(get_resource_type_for_function("gpu_alloc") == RESOURCE_TYPE_GPU_BUFFER);
    assert(get_resource_type_for_function("unknown_func") == RESOURCE_TYPE_UNKNOWN);
    
    printf("✓ Resource type detection test passed\n");
    return 1;
}

// Test resource tracking
int test_resource_tracking() {
    printf("Testing resource tracking...\n");
    
    TypeChecker* tc = create_mock_type_checker();
    ResourceManager* rm = resource_manager_new(tc);
    
    Position pos = {1, 1, 0, "test.goo"};
    
    // Track a file resource
    int result = resource_manager_track_resource(rm, "file_handle", RESOURCE_TYPE_FILE, NULL, pos);
    assert(result == 1);
    assert(rm->resource_count == 1);
    assert(rm->resources_tracked == 1);
    
    // Find the resource
    ResourceInfo* res = resource_manager_find_resource(rm, "file_handle");
    assert(res != NULL);
    assert(strcmp(res->name, "file_handle") == 0);
    assert(res->type == RESOURCE_TYPE_FILE);
    assert(res->is_acquired == 1);
    assert(res->needs_cleanup == 1);
    assert(strcmp(res->cleanup_function, "close") == 0);
    
    // Track a memory resource
    result = resource_manager_track_resource(rm, "buffer", RESOURCE_TYPE_MEMORY, NULL, pos);
    assert(result == 1);
    assert(rm->resource_count == 2);
    
    // Find the memory resource
    ResourceInfo* mem_res = resource_manager_find_resource(rm, "buffer");
    assert(mem_res != NULL);
    assert(mem_res->type == RESOURCE_TYPE_MEMORY);
    assert(strcmp(mem_res->cleanup_function, "free") == 0);
    
    // Test resource not found
    ResourceInfo* not_found = resource_manager_find_resource(rm, "nonexistent");
    assert(not_found == NULL);
    
    resource_manager_free(rm);
    cleanup_mock_type_checker(tc);
    
    printf("✓ Resource tracking test passed\n");
    return 1;
}

// Test scope management
int test_scope_management() {
    printf("Testing scope management...\n");
    
    TypeChecker* tc = create_mock_type_checker();
    ResourceManager* rm = resource_manager_new(tc);
    
    ASTNode* block1 = create_mock_block_stmt();
    ASTNode* block2 = create_mock_block_stmt();
    
    // Initially no scope
    assert(rm->current_scope == NULL);
    assert(rm->current_depth == 0);
    
    // Enter first scope
    ScopeCleanup* scope1 = resource_manager_enter_scope(rm, block1);
    assert(scope1 != NULL);
    assert(rm->current_scope == scope1);
    assert(rm->current_depth == 1);
    assert(scope1->parent == NULL);
    
    // Enter nested scope
    ScopeCleanup* scope2 = resource_manager_enter_scope(rm, block2);
    assert(scope2 != NULL);
    assert(rm->current_scope == scope2);
    assert(rm->current_depth == 2);
    assert(scope2->parent == scope1);
    
    // Exit nested scope
    resource_manager_exit_scope(rm);
    assert(rm->current_scope == scope1);
    assert(rm->current_depth == 1);
    
    // Exit first scope
    resource_manager_exit_scope(rm);
    assert(rm->current_scope == NULL);
    assert(rm->current_depth == 0);
    
    free(block1);
    free(block2);
    resource_manager_free(rm);
    cleanup_mock_type_checker(tc);
    
    printf("✓ Scope management test passed\n");
    return 1;
}

// Test defer statement processing
int test_defer_processing() {
    printf("Testing defer statement processing...\n");
    
    TypeChecker* tc = create_mock_type_checker();
    ResourceManager* rm = resource_manager_new(tc);
    
    ASTNode* block = create_mock_block_stmt();
    ASTNode* defer_stmt = create_mock_defer_stmt("cleanup_func");
    Position pos = {1, 1, 0, "test.goo"};
    
    // Enter scope
    ScopeCleanup* scope = resource_manager_enter_scope(rm, block);
    assert(scope != NULL);
    assert(scope->defer_count == 0);
    
    // Process defer statement
    int result = resource_manager_process_defer(rm, defer_stmt, pos);
    assert(result == 1);
    assert(scope->defer_count == 1);
    assert(rm->defers_processed == 1);
    
    // Check defer info
    DeferInfo* defer_info = scope->defers[0];
    assert(defer_info != NULL);
    assert(defer_info->defer_stmt == defer_stmt);
    assert(defer_info->scope_depth == 1);
    assert(defer_info->is_processed == 0);
    
    resource_manager_exit_scope(rm);
    
    cleanup_mock_defer_stmt(defer_stmt);
    free(block);
    resource_manager_free(rm);
    cleanup_mock_type_checker(tc);
    
    printf("✓ Defer processing test passed\n");
    return 1;
}

// Test resource state changes
int test_resource_state_changes() {
    printf("Testing resource state changes...\n");
    
    TypeChecker* tc = create_mock_type_checker();
    ResourceManager* rm = resource_manager_new(tc);
    
    Position pos = {1, 1, 0, "test.goo"};
    
    // Track a resource
    resource_manager_track_resource(rm, "resource", RESOURCE_TYPE_FILE, NULL, pos);
    ResourceInfo* res = resource_manager_find_resource(rm, "resource");
    
    // Initial state
    assert(res->is_acquired == 1);
    assert(res->is_moved == 0);
    assert(res->is_borrowed == 0);
    assert(res->needs_cleanup == 1);
    
    // Mark as borrowed
    int result = resource_manager_mark_resource_borrowed(rm, "resource");
    assert(result == 1);
    assert(res->is_borrowed == 1);
    assert(res->needs_cleanup == 1); // Still needs cleanup by owner
    
    // Mark as moved
    result = resource_manager_mark_resource_moved(rm, "resource");
    assert(result == 1);
    assert(res->is_moved == 1);
    assert(res->is_acquired == 0);
    assert(res->needs_cleanup == 0); // No longer needs cleanup
    
    resource_manager_free(rm);
    cleanup_mock_type_checker(tc);
    
    printf("✓ Resource state changes test passed\n");
    return 1;
}

// Test cleanup code generation
int test_cleanup_code_generation() {
    printf("Testing cleanup code generation...\n");
    
    TypeChecker* tc = create_mock_type_checker();
    ResourceManager* rm = resource_manager_new(tc);
    
    Position pos = {1, 1, 0, "test.goo"};
    
    // Create a resource
    ResourceInfo* res = resource_info_new("test_file", RESOURCE_TYPE_FILE, pos);
    res->cleanup_function = strdup("close");
    
    // Generate cleanup code
    char* cleanup_code = generate_cleanup_code(res, CLEANUP_METHOD_FUNCTION_CALL);
    assert(cleanup_code != NULL);
    assert(strstr(cleanup_code, "close(test_file)") != NULL);
    
    free(cleanup_code);
    
    // Test different cleanup methods
    cleanup_code = generate_cleanup_code(res, CLEANUP_METHOD_DESTRUCTOR);
    assert(cleanup_code != NULL);
    assert(strstr(cleanup_code, "test_file.~test_file()") != NULL);
    
    free(cleanup_code);
    
    cleanup_code = generate_cleanup_code(res, CLEANUP_METHOD_DEFER);
    assert(cleanup_code != NULL);
    assert(strstr(cleanup_code, "Defer cleanup") != NULL);
    
    free(cleanup_code);
    free(res->name);
    free(res->cleanup_function);
    free(res);
    resource_manager_free(rm);
    cleanup_mock_type_checker(tc);
    
    printf("✓ Cleanup code generation test passed\n");
    return 1;
}

// Test variable declaration analysis
int test_variable_declaration_analysis() {
    printf("Testing variable declaration analysis...\n");
    
    TypeChecker* tc = create_mock_type_checker();
    ResourceManager* rm = resource_manager_new(tc);
    
    // Create variable declaration with resource allocation
    ASTNode* var_decl = create_mock_var_decl("file", "open");
    
    // Enter scope and analyze
    ASTNode* block = create_mock_block_stmt();
    resource_manager_enter_scope(rm, block);
    
    int result = resource_manager_analyze_statement(rm, var_decl);
    assert(result == 1);
    
    // Check if resource was tracked
    ResourceInfo* res = resource_manager_find_resource(rm, "file");
    assert(res != NULL);
    assert(res->type == RESOURCE_TYPE_FILE);
    assert(strcmp(res->cleanup_function, "close") == 0);
    
    resource_manager_exit_scope(rm);
    
    cleanup_mock_var_decl(var_decl);
    free(block);
    resource_manager_free(rm);
    cleanup_mock_type_checker(tc);
    
    printf("✓ Variable declaration analysis test passed\n");
    return 1;
}

// Test utility functions
int test_utility_functions() {
    printf("Testing utility functions...\n");
    
    // Test string conversion functions
    assert(strcmp(resource_type_to_string(RESOURCE_TYPE_FILE), "file") == 0);
    assert(strcmp(resource_type_to_string(RESOURCE_TYPE_MEMORY), "memory") == 0);
    assert(strcmp(resource_type_to_string(RESOURCE_TYPE_UNKNOWN), "unknown") == 0);
    
    assert(strcmp(resource_context_to_string(RESOURCE_CONTEXT_DIRECT), "direct") == 0);
    assert(strcmp(resource_context_to_string(RESOURCE_CONTEXT_FUNCTION_CALL), "function_call") == 0);
    
    assert(strcmp(cleanup_method_to_string(CLEANUP_METHOD_FUNCTION_CALL), "function_call") == 0);
    assert(strcmp(cleanup_method_to_string(CLEANUP_METHOD_RAII), "raii") == 0);
    assert(strcmp(cleanup_method_to_string(CLEANUP_METHOD_DEFER), "defer") == 0);
    
    printf("✓ Utility functions test passed\n");
    return 1;
}

// Test statistics reporting
int test_statistics_reporting() {
    printf("Testing statistics reporting...\n");
    
    TypeChecker* tc = create_mock_type_checker();
    ResourceManager* rm = resource_manager_new(tc);
    
    Position pos = {1, 1, 0, "test.goo"};
    
    // Add some resources and process some operations
    resource_manager_track_resource(rm, "file1", RESOURCE_TYPE_FILE, NULL, pos);
    resource_manager_track_resource(rm, "mem1", RESOURCE_TYPE_MEMORY, NULL, pos);
    
    ASTNode* defer_stmt = create_mock_defer_stmt("cleanup");
    ASTNode* block = create_mock_block_stmt();
    
    resource_manager_enter_scope(rm, block);
    resource_manager_process_defer(rm, defer_stmt, pos);
    resource_manager_exit_scope(rm);
    
    // Test that statistics are reasonable
    assert(rm->resources_tracked == 2);
    assert(rm->scopes_processed == 1);
    assert(rm->defers_processed == 1);
    
    // Test statistics printing (just verify it doesn't crash)
    printf("--- Statistics Output ---\n");
    resource_manager_print_statistics(rm);
    printf("--- End Statistics ---\n");
    
    // Test resource info printing
    printf("--- Resource Info Output ---\n");
    resource_manager_print_resource_info(rm, "file1");
    printf("--- End Resource Info ---\n");
    
    cleanup_mock_defer_stmt(defer_stmt);
    free(block);
    resource_manager_free(rm);
    cleanup_mock_type_checker(tc);
    
    printf("✓ Statistics reporting test passed\n");
    return 1;
}

// Test comprehensive workflow
int test_comprehensive_workflow() {
    printf("Testing comprehensive workflow...\n");
    
    TypeChecker* tc = create_mock_type_checker();
    ResourceManager* rm = resource_manager_new(tc);
    
    ASTNode* function_block = create_mock_block_stmt();
    
    // Enter function scope
    ScopeCleanup* func_scope = resource_manager_enter_scope(rm, function_block);
    func_scope->is_function_scope = 1;
    
    // Create and analyze variable declarations
    ASTNode* file_var = create_mock_var_decl("file", "open");
    ASTNode* mem_var = create_mock_var_decl("buffer", "malloc");
    
    resource_manager_analyze_statement(rm, file_var);
    resource_manager_analyze_statement(rm, mem_var);
    
    // Process defer statements
    ASTNode* defer1 = create_mock_defer_stmt("log_cleanup");
    ASTNode* defer2 = create_mock_defer_stmt("finalize");
    
    resource_manager_process_defer(rm, defer1, (Position){2, 1, 0, "test.goo"});
    resource_manager_process_defer(rm, defer2, (Position){3, 1, 0, "test.goo"});
    
    // Enter nested scope
    ASTNode* nested_block = create_mock_block_stmt();
    resource_manager_enter_scope(rm, nested_block);
    
    ASTNode* temp_var = create_mock_var_decl("temp", "socket");
    resource_manager_analyze_statement(rm, temp_var);
    
    // Exit nested scope (should generate cleanup for temp)
    resource_manager_exit_scope(rm);
    
    // Exit function scope (should generate cleanup for all remaining resources)
    resource_manager_exit_scope(rm);
    
    // Verify final state
    assert(rm->resources_tracked == 3);
    assert(rm->defers_processed == 2);
    assert(rm->scopes_processed == 2);
    
    // Verify resources were tracked correctly
    ResourceInfo* file_res = resource_manager_find_resource(rm, "file");
    ResourceInfo* mem_res = resource_manager_find_resource(rm, "buffer");
    ResourceInfo* net_res = resource_manager_find_resource(rm, "temp");
    
    assert(file_res != NULL && file_res->type == RESOURCE_TYPE_FILE);
    assert(mem_res != NULL && mem_res->type == RESOURCE_TYPE_MEMORY);
    assert(net_res != NULL && net_res->type == RESOURCE_TYPE_NETWORK);
    
    // Cleanup
    cleanup_mock_var_decl(file_var);
    cleanup_mock_var_decl(mem_var);
    cleanup_mock_var_decl(temp_var);
    cleanup_mock_defer_stmt(defer1);
    cleanup_mock_defer_stmt(defer2);
    free(function_block);
    free(nested_block);
    resource_manager_free(rm);
    cleanup_mock_type_checker(tc);
    
    printf("✓ Comprehensive workflow test passed\n");
    return 1;
}

// Run all tests
int TEST_MAIN() {
    printf("=== Automatic Resource Management Tests ===\n\n");
    
    int tests_passed = 0;
    int total_tests = 10;
    
    tests_passed += test_resource_manager_creation();
    tests_passed += test_resource_type_detection();
    tests_passed += test_resource_tracking();
    tests_passed += test_scope_management();
    tests_passed += test_defer_processing();
    tests_passed += test_resource_state_changes();
    tests_passed += test_cleanup_code_generation();
    tests_passed += test_variable_declaration_analysis();
    tests_passed += test_utility_functions();
    tests_passed += test_statistics_reporting();
    tests_passed += test_comprehensive_workflow();
    
    printf("\n=== Test Results ===\n");
    printf("Tests passed: %d/%d\n", tests_passed, total_tests);
    
    if (tests_passed == total_tests) {
        printf("✓ All resource management tests passed!\n");
        return 0;
    } else {
        printf("✗ Some tests failed!\n");
        return 1;
    }
}

#ifndef STANDALONE_TEST
// Register tests with the framework
void register_resource_manager_tests() {
    test_framework_register_test("resource_manager_creation", test_resource_manager_creation);
    test_framework_register_test("resource_type_detection", test_resource_type_detection);
    test_framework_register_test("resource_tracking", test_resource_tracking);
    test_framework_register_test("scope_management", test_scope_management);
    test_framework_register_test("defer_processing", test_defer_processing);
    test_framework_register_test("resource_state_changes", test_resource_state_changes);
    test_framework_register_test("cleanup_code_generation", test_cleanup_code_generation);
    test_framework_register_test("variable_declaration_analysis", test_variable_declaration_analysis);
    test_framework_register_test("utility_functions", test_utility_functions);
    test_framework_register_test("statistics_reporting", test_statistics_reporting);
    test_framework_register_test("comprehensive_workflow", test_comprehensive_workflow);
}
#endif