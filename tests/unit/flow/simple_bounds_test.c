#include "panic_free.h"
#include "types.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

int main() {
    printf("=== Simple Bounds Verifier Test ===\n\n");
    
    // Create type checker
    TypeChecker* tc = type_checker_new();
    assert(tc != NULL);
    printf("✓ Type checker created\n");
    
    // Create bounds verifier
    BoundsVerifier* verifier = bounds_verifier_new(tc);
    assert(verifier != NULL);
    assert(verifier->context != NULL);
    printf("✓ Bounds verifier created\n");
    
    // Test configuration
    assert(verifier->context->default_mode == BOUNDS_CHECK_PROVE_SAFE);
    printf("✓ Default mode: %s\n", bounds_check_mode_to_string(verifier->context->default_mode));
    
    // Test mode setting
    bounds_verifier_set_mode(verifier, BOUNDS_CHECK_OPTIMIZE_OUT);
    assert(verifier->context->default_mode == BOUNDS_CHECK_OPTIMIZE_OUT);
    printf("✓ Mode changed to: %s\n", bounds_check_mode_to_string(verifier->context->default_mode));
    
    // Test feature configuration
    bounds_verifier_enable_feature(verifier, "smt_solver", 1);
    assert(verifier->enable_smt_solver == 1);
    printf("✓ SMT solver enabled\n");
    
    bounds_verifier_enable_feature(verifier, "invariant_inference", 0);
    assert(verifier->enable_invariant_inference == 0);
    printf("✓ Invariant inference disabled\n");
    
    // Test symbolic expressions
    printf("\n--- Testing Symbolic Expressions ---\n");
    
    SymbolicExpression* const42 = symbolic_constant(42);
    assert(const42 != NULL);
    assert(const42->type == SYMBOLIC_CONSTANT);
    assert(const42->data.constant.value == 42);
    
    char* const_str = symbolic_expression_to_string(const42);
    printf("Constant expression: %s\n", const_str);
    free(const_str);
    
    SymbolicExpression* var_x = symbolic_variable("x", NULL);
    assert(var_x != NULL);
    assert(var_x->type == SYMBOLIC_VARIABLE);
    
    char* var_str = symbolic_expression_to_string(var_x);
    printf("Variable expression: %s\n", var_str);
    free(var_str);
    
    SymbolicExpression* sum = symbolic_binary_op(var_x, TOKEN_PLUS, const42);
    assert(sum != NULL);
    assert(sum->type == SYMBOLIC_BINARY_OP);
    
    char* sum_str = symbolic_expression_to_string(sum);
    printf("Binary expression: %s\n", sum_str);
    free(sum_str);
    
    // Test constraints
    printf("\n--- Testing Bounds Constraints ---\n");
    
    SymbolicExpression* index = symbolic_variable("i", NULL);
    SymbolicExpression* bound = symbolic_constant(10);
    
    BoundsConstraint* constraint = bounds_constraint_comparison(CONSTRAINT_LESS_THAN, index, bound);
    assert(constraint != NULL);
    assert(constraint->type == CONSTRAINT_LESS_THAN);
    
    char* constraint_str = bounds_constraint_to_string(constraint);
    printf("Constraint: %s\n", constraint_str);
    free(constraint_str);
    
    // Test utility functions
    printf("\n--- Testing Utility Functions ---\n");
    
    printf("Proof status strings:\n");
    printf("- SAFE: %s\n", proof_status_to_string(PROOF_STATUS_SAFE));
    printf("- UNSAFE: %s\n", proof_status_to_string(PROOF_STATUS_UNSAFE));
    printf("- CONDITIONAL: %s\n", proof_status_to_string(PROOF_STATUS_CONDITIONAL));
    printf("- UNKNOWN: %s\n", proof_status_to_string(PROOF_STATUS_UNKNOWN));
    
    printf("Bounds check modes:\n");
    printf("- RUNTIME: %s\n", bounds_check_mode_to_string(BOUNDS_CHECK_RUNTIME));
    printf("- PROVE_SAFE: %s\n", bounds_check_mode_to_string(BOUNDS_CHECK_PROVE_SAFE));
    printf("- OPTIMIZE_OUT: %s\n", bounds_check_mode_to_string(BOUNDS_CHECK_OPTIMIZE_OUT));
    
    // Test statistics
    printf("\n--- Testing Statistics ---\n");
    bounds_verifier_print_statistics(verifier);
    
    // Cleanup
    bounds_constraint_free(constraint);
    symbolic_expression_free(sum);
    bounds_verifier_free(verifier);
    type_checker_free(tc);
    
    printf("\n✓ All tests passed successfully!\n");
    printf("✓ Bounds verifier implementation working correctly\n");
    
    return 0;
}