#include "panic_free.h"
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
#define TEST_MAIN bounds_verifier_test_main
#endif

// Helper functions for creating test AST nodes
static ASTNode* create_test_array_access(const char* array_name, int index_value) {
    IndexExprNode* index_expr = malloc(sizeof(IndexExprNode));
    index_expr->base.type = AST_INDEX_EXPR;
    index_expr->base.pos = (Position){1, 1, 0, "test.goo"};
    
    // Create array identifier
    IdentifierNode* array_id = ast_identifier_new(array_name, (Position){1, 1, 0, "test.goo"});
    index_expr->expr = (ASTNode*)array_id;
    
    // Create index literal
    LiteralNode* index_lit = ast_literal_new(TOKEN_INT, "42", (Position){1, 1, 0, "test.goo"});
    char index_str[32];
    snprintf(index_str, sizeof(index_str), "%d", index_value);
    free(index_lit->value);
    index_lit->value = strdup(index_str);
    index_expr->index = (ASTNode*)index_lit;
    
    return (ASTNode*)index_expr;
}

static void cleanup_test_array_access(ASTNode* node) {
    if (!node) return;
    
    IndexExprNode* index_expr = (IndexExprNode*)node;
    ast_node_free(index_expr->expr);
    ast_node_free(index_expr->index);
    free(index_expr);
}

// Test bounds verifier creation and basic functionality
int test_bounds_verifier_creation() {
    printf("Testing bounds verifier creation...\n");
    
    TypeChecker* tc = type_checker_new();
    assert(tc != NULL);
    
    BoundsVerifier* verifier = bounds_verifier_new(tc);
    assert(verifier != NULL);
    assert(verifier->context != NULL);
    assert(verifier->context->type_checker == tc);
    
    // Test configuration
    assert(verifier->enable_invariant_inference == 1);
    assert(verifier->enable_path_sensitivity == 1);
    assert(verifier->max_unroll_depth == 3);
    
    // Test mode setting
    bounds_verifier_set_mode(verifier, BOUNDS_CHECK_OPTIMIZE_OUT);
    assert(verifier->context->default_mode == BOUNDS_CHECK_OPTIMIZE_OUT);
    
    // Test feature toggling
    bounds_verifier_enable_feature(verifier, "smt_solver", 1);
    assert(verifier->enable_smt_solver == 1);
    
    bounds_verifier_enable_feature(verifier, "invariant_inference", 0);
    assert(verifier->enable_invariant_inference == 0);
    
    bounds_verifier_free(verifier);
    type_checker_free(tc);
    
    printf("✓ Bounds verifier creation test passed\n");
    return 1;
}

// Test symbolic expression creation and manipulation
int test_symbolic_expressions() {
    printf("Testing symbolic expressions...\n");
    
    // Test constant creation
    SymbolicExpression* const_expr = symbolic_constant(42);
    assert(const_expr != NULL);
    assert(const_expr->type == SYMBOLIC_CONSTANT);
    assert(const_expr->data.constant.value == 42);
    
    // Test variable creation
    SymbolicExpression* var_expr = symbolic_variable("x", NULL);
    assert(var_expr != NULL);
    assert(var_expr->type == SYMBOLIC_VARIABLE);
    assert(strcmp(var_expr->data.variable.name, "x") == 0);
    
    // Test binary operation
    SymbolicExpression* binary_expr = symbolic_binary_op(var_expr, TOKEN_PLUS, const_expr);
    assert(binary_expr != NULL);
    assert(binary_expr->type == SYMBOLIC_BINARY_OP);
    assert(binary_expr->data.binary_op.operator == TOKEN_PLUS);
    
    // Test expression to string
    char* expr_str = symbolic_expression_to_string(binary_expr);
    assert(expr_str != NULL);
    printf("Expression: %s\n", expr_str);
    free(expr_str);
    
    // Test simplification - create 2 + 3
    SymbolicExpression* two = symbolic_constant(2);
    SymbolicExpression* three = symbolic_constant(3);
    SymbolicExpression* sum = symbolic_binary_op(two, TOKEN_PLUS, three);
    
    SymbolicExpression* simplified = symbolic_expression_simplify(sum);
    assert(simplified != NULL);
    assert(simplified->type == SYMBOLIC_CONSTANT);
    assert(simplified->data.constant.value == 5);
    
    symbolic_expression_free(binary_expr);
    symbolic_expression_free(simplified);
    
    printf("✓ Symbolic expressions test passed\n");
    return 1;
}

// Test bounds constraint creation and manipulation
int test_bounds_constraints() {
    printf("Testing bounds constraints...\n");
    
    // Create symbolic expressions for testing
    SymbolicExpression* index = symbolic_variable("i", NULL);
    SymbolicExpression* bound = symbolic_constant(10);
    
    // Test comparison constraint
    BoundsConstraint* constraint = bounds_constraint_comparison(CONSTRAINT_LESS_THAN, index, bound);
    assert(constraint != NULL);
    assert(constraint->type == CONSTRAINT_LESS_THAN);
    
    // Test constraint to string
    char* constraint_str = bounds_constraint_to_string(constraint);
    assert(constraint_str != NULL);
    printf("Constraint: %s\n", constraint_str);
    free(constraint_str);
    
    bounds_constraint_free(constraint);
    
    printf("✓ Bounds constraints test passed\n");
    return 1;
}

// Test array access verification
int test_array_access_verification() {
    printf("Testing array access verification...\n");
    
    TypeChecker* tc = type_checker_new();
    BoundsVerifier* verifier = bounds_verifier_new(tc);
    
    // Create a test array variable in the type checker
    Type* int_array_type = type_array(type_int(32, 1), 10);
    Variable* array_var = variable_new("test_array", int_array_type, (Position){1, 1, 0, "test.goo"});
    array_var->is_initialized = 1;
    scope_add_variable(tc->current_scope, array_var);
    
    // Create array access node: test_array[5]
    ASTNode* array_access = create_test_array_access("test_array", 5);
    
    // Verify the array access
    BoundsProof* proof = verify_array_access(verifier, array_access);
    assert(proof != NULL);
    assert(proof->target_node == array_access);
    assert(proof->index_expr != NULL);
    assert(proof->bound_expr != NULL);
    
    printf("Proof status: %s\n", proof_status_to_string(proof->status));
    
    // Print the proof for debugging
    bounds_verifier_print_proof(proof);
    
    // Cleanup
    bounds_proof_free(proof);
    cleanup_test_array_access(array_access);
    bounds_verifier_free(verifier);
    type_checker_free(tc);
    
    printf("✓ Array access verification test passed\n");
    return 1;
}

// Test AST to symbolic expression conversion
int test_ast_to_symbolic_conversion() {
    printf("Testing AST to symbolic expression conversion...\n");
    
    TypeChecker* tc = type_checker_new();
    VerificationContext* context = verification_context_new(tc);
    
    // Add a variable to the type checker
    Type* int_type = type_int(32, 1);
    Variable* var = variable_new("x", int_type, (Position){1, 1, 0, "test.goo"});
    var->is_initialized = 1;
    scope_add_variable(tc->current_scope, var);
    
    // Test literal conversion
    LiteralNode* lit = ast_literal_new(TOKEN_INT, "42", (Position){1, 1, 0, "test.goo"});
    SymbolicExpression* sym_lit = ast_to_symbolic_expression((ASTNode*)lit, context);
    assert(sym_lit != NULL);
    assert(sym_lit->type == SYMBOLIC_CONSTANT);
    assert(sym_lit->data.constant.value == 42);
    
    // Test identifier conversion
    IdentifierNode* id = ast_identifier_new("x", (Position){1, 1, 0, "test.goo"});
    SymbolicExpression* sym_id = ast_to_symbolic_expression((ASTNode*)id, context);
    assert(sym_id != NULL);
    assert(sym_id->type == SYMBOLIC_VARIABLE);
    assert(strcmp(sym_id->data.variable.name, "x") == 0);
    
    // Test binary expression conversion: x + 42
    BinaryExprNode* binary = ast_binary_expr_new((ASTNode*)id, TOKEN_PLUS, (ASTNode*)lit, (Position){1, 1, 0, "test.goo"});
    SymbolicExpression* sym_binary = ast_to_symbolic_expression((ASTNode*)binary, context);
    assert(sym_binary != NULL);
    assert(sym_binary->type == SYMBOLIC_BINARY_OP);
    assert(sym_binary->data.binary_op.operator == TOKEN_PLUS);
    
    // Cleanup
    symbolic_expression_free(sym_lit);
    symbolic_expression_free(sym_id);
    symbolic_expression_free(sym_binary);
    ast_node_free((ASTNode*)binary); // This should clean up id and lit too
    verification_context_free(context);
    type_checker_free(tc);
    
    printf("✓ AST to symbolic conversion test passed\n");
    return 1;
}

// Test bounds verification with different scenarios
int test_verification_scenarios() {
    printf("Testing different verification scenarios...\n");
    
    TypeChecker* tc = type_checker_new();
    BoundsVerifier* verifier = bounds_verifier_new(tc);
    
    // Create array variable
    Type* int_array_type = type_array(type_int(32, 1), 10);
    Variable* array_var = variable_new("arr", int_array_type, (Position){1, 1, 0, "test.goo"});
    array_var->is_initialized = 1;
    scope_add_variable(tc->current_scope, array_var);
    
    // Test scenario 1: Constant index within bounds
    ASTNode* safe_access = create_test_array_access("arr", 5);
    BoundsProof* safe_proof = verify_array_access(verifier, safe_access);
    assert(safe_proof != NULL);
    printf("Safe access proof status: %s\n", proof_status_to_string(safe_proof->status));
    
    // Test scenario 2: Constant index at boundary
    ASTNode* boundary_access = create_test_array_access("arr", 9);
    BoundsProof* boundary_proof = verify_array_access(verifier, boundary_access);
    assert(boundary_proof != NULL);
    printf("Boundary access proof status: %s\n", proof_status_to_string(boundary_proof->status));
    
    // Test scenario 3: Constant index out of bounds
    ASTNode* unsafe_access = create_test_array_access("arr", 15);
    BoundsProof* unsafe_proof = verify_array_access(verifier, unsafe_access);
    assert(unsafe_proof != NULL);
    printf("Unsafe access proof status: %s\n", proof_status_to_string(unsafe_proof->status));
    
    // Print statistics
    bounds_verifier_print_statistics(verifier);
    
    // Cleanup
    bounds_proof_free(safe_proof);
    bounds_proof_free(boundary_proof);
    bounds_proof_free(unsafe_proof);
    cleanup_test_array_access(safe_access);
    cleanup_test_array_access(boundary_access);
    cleanup_test_array_access(unsafe_access);
    bounds_verifier_free(verifier);
    type_checker_free(tc);
    
    printf("✓ Verification scenarios test passed\n");
    return 1;
}

// Test utility functions
int test_utility_functions() {
    printf("Testing utility functions...\n");
    
    // Test enum to string conversions
    assert(strcmp(bounds_check_mode_to_string(BOUNDS_CHECK_PROVE_SAFE), "prove_safe") == 0);
    assert(strcmp(bounds_check_mode_to_string(BOUNDS_CHECK_OPTIMIZE_OUT), "optimize_out") == 0);
    
    assert(strcmp(proof_status_to_string(PROOF_STATUS_SAFE), "safe") == 0);
    assert(strcmp(proof_status_to_string(PROOF_STATUS_UNSAFE), "unsafe") == 0);
    assert(strcmp(proof_status_to_string(PROOF_STATUS_CONDITIONAL), "conditional") == 0);
    
    printf("✓ Utility functions test passed\n");
    return 1;
}

// Test performance and statistics
int test_performance_and_statistics() {
    printf("Testing performance and statistics...\n");
    
    TypeChecker* tc = type_checker_new();
    BoundsVerifier* verifier = bounds_verifier_new(tc);
    
    // Create array variable
    Type* int_array_type = type_array(type_int(32, 1), 100);
    Variable* array_var = variable_new("big_array", int_array_type, (Position){1, 1, 0, "test.goo"});
    array_var->is_initialized = 1;
    scope_add_variable(tc->current_scope, array_var);
    
    // Perform multiple verifications to test statistics
    for (int i = 0; i < 10; i++) {
        ASTNode* access = create_test_array_access("big_array", i * 5);
        BoundsProof* proof = verify_array_access(verifier, access);
        
        if (proof) {
            bounds_proof_free(proof);
        }
        cleanup_test_array_access(access);
    }
    
    // Check statistics
    assert(verifier->total_array_accesses == 10);
    
    // Print final statistics
    bounds_verifier_print_statistics(verifier);
    
    bounds_verifier_free(verifier);
    type_checker_free(tc);
    
    printf("✓ Performance and statistics test passed\n");
    return 1;
}

// Run all tests
int TEST_MAIN() {
    printf("=== Bounds Verifier Tests ===\n\n");
    
    int tests_passed = 0;
    int total_tests = 7;
    
    tests_passed += test_bounds_verifier_creation();
    tests_passed += test_symbolic_expressions();
    tests_passed += test_bounds_constraints();
    tests_passed += test_array_access_verification();
    tests_passed += test_ast_to_symbolic_conversion();
    tests_passed += test_verification_scenarios();
    tests_passed += test_utility_functions();
    tests_passed += test_performance_and_statistics();
    
    printf("\n=== Test Results ===\n");
    printf("Tests passed: %d/%d\n", tests_passed, total_tests);
    
    if (tests_passed == total_tests) {
        printf("✓ All bounds verifier tests passed!\n");
        return 0;
    } else {
        printf("✗ Some tests failed!\n");
        return 1;
    }
}

#ifndef STANDALONE_TEST
// Register tests with the framework
void register_bounds_verifier_tests(void) {
    test_framework_register_test("bounds_verifier_creation", test_bounds_verifier_creation);
    test_framework_register_test("symbolic_expressions", test_symbolic_expressions);
    test_framework_register_test("bounds_constraints", test_bounds_constraints);
    test_framework_register_test("array_access_verification", test_array_access_verification);
    test_framework_register_test("ast_to_symbolic_conversion", test_ast_to_symbolic_conversion);
    test_framework_register_test("verification_scenarios", test_verification_scenarios);
    test_framework_register_test("utility_functions", test_utility_functions);
    test_framework_register_test("performance_and_statistics", test_performance_and_statistics);
}
#endif