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
#define TEST_MAIN memory_safety_integration_test_main
#endif

// Mock functions for testing
static TypeChecker* create_test_type_checker() {
    TypeChecker* tc = type_checker_new();
    return tc;
}

static void cleanup_test_type_checker(TypeChecker* tc) {
    type_checker_free(tc);
}

// Test memory safety integration initialization
int test_memory_safety_integration_init() {
    TypeChecker* tc = create_test_type_checker();
    assert(tc != NULL);
    
    // Initialize memory safety integration
    int result = integrate_memory_safety_with_type_checker(tc);
    assert(result == 1);
    
    // Check that context was created
    MemorySafetyContext* ctx = get_memory_safety_context();
    assert(ctx != NULL);
    
    // Check that all features are enabled by default
    assert(memory_safety_is_feature_enabled("null_safety") == 1);
    assert(memory_safety_is_feature_enabled("ownership_tracking") == 1);
    assert(memory_safety_is_feature_enabled("resource_management") == 1);
    assert(memory_safety_is_feature_enabled("escape_analysis") == 1);
    assert(memory_safety_is_feature_enabled("flow_analysis") == 1);
    
    // Test disabling features
    memory_safety_enable_feature("null_safety", 0);
    assert(memory_safety_is_feature_enabled("null_safety") == 0);
    
    memory_safety_enable_feature("null_safety", 1);
    assert(memory_safety_is_feature_enabled("null_safety") == 1);
    
    // Cleanup
    cleanup_memory_safety_integration();
    cleanup_test_type_checker(tc);
    
    return 0;
}

// Test basic memory safety checking
int test_basic_memory_safety_checking() {
    TypeChecker* tc = create_test_type_checker();
    integrate_memory_safety_with_type_checker(tc);
    
    // Create a simple identifier expression
    IdentifierNode* id = ast_identifier_new("test_var", (Position){1, 1, 0, "test.goo"});
    
    // Add the variable to the type checker's scope
    Type* int_type = type_int(32, 1);
    Variable* var = variable_new("test_var", int_type, (Position){1, 1, 0, "test.goo"});
    var->is_initialized = 1;
    scope_add_variable(tc->current_scope, var);
    
    // Test memory-safe expression checking
    Type* result_type = memory_safe_type_check_expression(tc, (ASTNode*)id);
    assert(result_type != NULL);
    assert(result_type->kind == TYPE_INT32);
    
    // Test with moved variable
    var->is_moved = 1;
    result_type = memory_safe_type_check_expression(tc, (ASTNode*)id);
    // Should return NULL due to use-after-move error
    assert(result_type == NULL);
    assert(tc->error_count > 0);
    
    // Cleanup
    ast_node_free((ASTNode*)id);
    cleanup_memory_safety_integration();
    cleanup_test_type_checker(tc);
    
    return 0;
}

// Test nullable type safety
int test_nullable_type_safety() {
    TypeChecker* tc = create_test_type_checker();
    integrate_memory_safety_with_type_checker(tc);
    
    // Create a nullable type
    Type* int_type = type_int(32, 1);
    Type* nullable_int = type_nullable(int_type);
    
    // Create a variable with nullable type
    Variable* var = variable_new("nullable_var", nullable_int, (Position){1, 1, 0, "test.goo"});
    var->is_initialized = 1;
    scope_add_variable(tc->current_scope, var);
    
    // Create identifier expression
    IdentifierNode* id = ast_identifier_new("nullable_var", (Position){1, 1, 0, "test.goo"});
    
    // Test accessing nullable without null check - should fail
    Type* result_type = memory_safe_type_check_expression(tc, (ASTNode*)id);
    assert(result_type == NULL); // Should fail null safety check
    assert(tc->error_count > 0);
    
    // Cleanup
    ast_node_free((ASTNode*)id);
    cleanup_memory_safety_integration();
    cleanup_test_type_checker(tc);
    
    return 0;
}

// Test resource management integration
int test_resource_management_integration() {
    TypeChecker* tc = create_test_type_checker();
    integrate_memory_safety_with_type_checker(tc);
    
    // Create a variable declaration that allocates a file resource
    VarDeclNode* var_decl = malloc(sizeof(VarDeclNode));
    var_decl->base.type = AST_VAR_DECL;
    var_decl->base.pos = (Position){1, 1, 0, "test.goo"};
    
    var_decl->names = malloc(sizeof(char*));
    var_decl->names[0] = strdup("file_handle");
    var_decl->name_count = 1;
    var_decl->is_short_decl = 0;
    var_decl->ownership = OWNERSHIP_OWNED;
    
    // Create function call to open()
    CallExprNode* call = malloc(sizeof(CallExprNode));
    call->base.type = AST_CALL_EXPR;
    call->base.pos = (Position){1, 1, 0, "test.goo"};
    
    IdentifierNode* func_id = ast_identifier_new("open", (Position){1, 1, 0, "test.goo"});
    call->function = (ASTNode*)func_id;
    call->args = NULL;
    
    var_decl->values = (ASTNode*)call;
    var_decl->type = NULL;
    
    // Test memory-safe statement checking
    int result = memory_safe_type_check_statement(tc, (ASTNode*)var_decl);
    
    // Cleanup
    free(var_decl->names[0]);
    free(var_decl->names);
    ast_node_free((ASTNode*)func_id);
    free(call);
    free(var_decl);
    cleanup_memory_safety_integration();
    cleanup_test_type_checker(tc);
    
    return 0;
}

// Test if-let null safety
int test_if_let_null_safety() {
    TypeChecker* tc = create_test_type_checker();
    integrate_memory_safety_with_type_checker(tc);
    
    // Create nullable variable
    Type* int_type = type_int(32, 1);
    Type* nullable_int = type_nullable(int_type);
    Variable* var = variable_new("maybe_value", nullable_int, (Position){1, 1, 0, "test.goo"});
    var->is_initialized = 1;
    scope_add_variable(tc->current_scope, var);
    
    // Create if-let statement
    IdentifierNode* nullable_expr = ast_identifier_new("maybe_value", (Position){1, 1, 0, "test.goo"});
    BlockStmtNode* then_stmt = ast_block_stmt_new((Position){2, 1, 0, "test.goo"});
    
    IfLetStmtNode* if_let = ast_if_let_stmt_new("value", (ASTNode*)nullable_expr, 
                                               (ASTNode*)then_stmt, NULL, (Position){1, 1, 0, "test.goo"});
    
    // Test if-let checking
    int result = memory_safe_type_check_statement(tc, (ASTNode*)if_let);
    assert(result == 1); // Should succeed
    
    // Cleanup
    ast_node_free((ASTNode*)if_let);
    cleanup_memory_safety_integration();
    cleanup_test_type_checker(tc);
    
    return 0;
}

// Test field access safety
int test_field_access_safety() {
    TypeChecker* tc = create_test_type_checker();
    integrate_memory_safety_with_type_checker(tc);
    
    // Create a moved variable
    Type* int_type = type_int(32, 1);
    Variable* var = variable_new("obj", int_type, (Position){1, 1, 0, "test.goo"});
    var->is_initialized = 1;
    var->is_moved = 1; // Mark as moved
    scope_add_variable(tc->current_scope, var);
    
    // Create field access expression
    IdentifierNode* obj_id = ast_identifier_new("obj", (Position){1, 1, 0, "test.goo"});
    SelectorExprNode* field_access = malloc(sizeof(SelectorExprNode));
    field_access->base.type = AST_SELECTOR_EXPR;
    field_access->base.pos = (Position){1, 1, 0, "test.goo"};
    field_access->expr = (ASTNode*)obj_id;
    field_access->selector = strdup("field");
    
    // Test field access on moved value - should fail
    Type* result_type = memory_safe_type_check_expression(tc, (ASTNode*)field_access);
    assert(result_type == NULL); // Should fail due to use-after-move
    assert(tc->error_count > 0);
    
    // Cleanup
    free(field_access->selector);
    free(field_access);
    // obj_id is freed by field_access cleanup
    cleanup_memory_safety_integration();
    cleanup_test_type_checker(tc);
    
    return 0;
}

// Test statistics reporting
int test_statistics_reporting() {
    TypeChecker* tc = create_test_type_checker();
    integrate_memory_safety_with_type_checker(tc);
    
    // Test that statistics don't crash
    printf("--- Memory Safety Statistics ---\n");
    memory_safety_print_statistics();
    printf("--- End Statistics ---\n");
    
    // Cleanup
    cleanup_memory_safety_integration();
    cleanup_test_type_checker(tc);
    
    return 0;
}

// Test feature configuration
int test_feature_configuration() {
    TypeChecker* tc = create_test_type_checker();
    integrate_memory_safety_with_type_checker(tc);
    
    // Test enabling/disabling features
    const char* features[] = {
        "null_safety",
        "ownership_tracking", 
        "resource_management",
        "escape_analysis",
        "flow_analysis"
    };
    
    for (int i = 0; i < 5; i++) {
        const char* feature = features[i];
        
        // Should be enabled by default
        assert(memory_safety_is_feature_enabled(feature) == 1);
        
        // Disable and check
        memory_safety_enable_feature(feature, 0);
        assert(memory_safety_is_feature_enabled(feature) == 0);
        
        // Re-enable and check
        memory_safety_enable_feature(feature, 1);
        assert(memory_safety_is_feature_enabled(feature) == 1);
    }
    
    // Test unknown feature
    assert(memory_safety_is_feature_enabled("unknown_feature") == 0);
    
    // Cleanup
    cleanup_memory_safety_integration();
    cleanup_test_type_checker(tc);
    
    return 0;
}

#ifndef STANDALONE_TEST
// Register tests with the framework
void register_memory_safety_integration_tests(void) {
    test_framework_register_test("memory_safety_integration_init", test_memory_safety_integration_init);
    test_framework_register_test("basic_memory_safety_checking", test_basic_memory_safety_checking);
    test_framework_register_test("nullable_type_safety", test_nullable_type_safety);
    test_framework_register_test("resource_management_integration", test_resource_management_integration);
    test_framework_register_test("if_let_null_safety", test_if_let_null_safety);
    test_framework_register_test("field_access_safety", test_field_access_safety);
    test_framework_register_test("statistics_reporting", test_statistics_reporting);
    test_framework_register_test("feature_configuration", test_feature_configuration);
}
#endif