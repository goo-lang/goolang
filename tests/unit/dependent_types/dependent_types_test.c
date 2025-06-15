#include "dependent_types.h"
#include "types.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Utility for strdup if not available
char* str_dup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

int main() {
    printf("=== Dependent and Refinement Type System Test ===\n\n");
    
    // Create type checker and dependent type context
    TypeChecker* tc = type_checker_new();
    assert(tc != NULL);
    printf("✓ Type checker created\n");
    
    DependentTypeContext* context = dependent_type_context_new(tc);
    assert(context != NULL);
    assert(context->solver != NULL);
    printf("✓ Dependent type context created\n");
    
    // Test 1: Bounded Vector Types
    printf("\n--- Testing Bounded Vector Types ---\n");
    
    DependentType* bounded_vec_10 = create_bounded_vec_type(NULL, 10);
    assert(bounded_vec_10 != NULL);
    assert(bounded_vec_10->kind == DEPENDENT_BOUNDED_VEC);
    assert(bounded_vec_10->data.bounded_vec.capacity == 10);
    assert(!bounded_vec_10->data.bounded_vec.is_capacity_dynamic);
    
    char* vec_str = dependent_type_to_string(bounded_vec_10);
    printf("Static bounded vector: %s\n", vec_str);
    free(vec_str);
    
    DependentType* dynamic_vec = create_dynamic_bounded_vec_type(NULL, "N");
    assert(dynamic_vec != NULL);
    assert(dynamic_vec->data.bounded_vec.is_capacity_dynamic);
    assert(strcmp(dynamic_vec->data.bounded_vec.capacity_param, "N") == 0);
    
    char* dyn_vec_str = dependent_type_to_string(dynamic_vec);
    printf("Dynamic bounded vector: %s\n", dyn_vec_str);
    free(dyn_vec_str);
    
    // Test 2: Bounded Integer Types
    printf("\n--- Testing Bounded Integer Types ---\n");
    
    DependentType* bounded_int_0_255 = create_bounded_int_type(0, 255);
    assert(bounded_int_0_255 != NULL);
    assert(bounded_int_0_255->kind == DEPENDENT_BOUNDED_INT);
    assert(bounded_int_0_255->data.bounded_int.min_value == 0);
    assert(bounded_int_0_255->data.bounded_int.max_value == 255);
    
    char* int_str = dependent_type_to_string(bounded_int_0_255);
    printf("Bounded integer (0-255): %s\n", int_str);
    free(int_str);
    
    DependentType* dynamic_int = create_dynamic_bounded_int_type("Min", "Max");
    assert(dynamic_int != NULL);
    assert(dynamic_int->data.bounded_int.is_min_dynamic);
    assert(dynamic_int->data.bounded_int.is_max_dynamic);
    
    char* dyn_int_str = dependent_type_to_string(dynamic_int);
    printf("Dynamic bounded integer: %s\n", dyn_int_str);
    free(dyn_int_str);
    
    // Test 3: Sized Array Types
    printf("\n--- Testing Sized Array Types ---\n");
    
    DependentType* sized_array_20 = create_sized_array_type(NULL, 20);
    assert(sized_array_20 != NULL);
    assert(sized_array_20->kind == DEPENDENT_SIZED_ARRAY);
    assert(sized_array_20->data.sized_array.size == 20);
    
    char* array_str = dependent_type_to_string(sized_array_20);
    printf("Sized array: %s\n", array_str);
    free(array_str);
    
    // Test 4: Refinement Types
    printf("\n--- Testing Refinement Types ---\n");
    
    RefinementType* non_zero = create_non_zero_int_type();
    assert(non_zero != NULL);
    assert(non_zero->constraint != NULL);
    assert(non_zero->constraint->type == DEP_CONSTRAINT_NON_ZERO);
    printf("NonZeroInt refinement type created\n");
    
    RefinementType* positive = create_positive_int_type();
    assert(positive != NULL);
    assert(positive->constraint->type == DEP_CONSTRAINT_POSITIVE);
    printf("PositiveInt refinement type created\n");
    
    RefinementType* even = create_even_int_type();
    assert(even != NULL);
    assert(even->constraint->type == DEP_CONSTRAINT_EVEN);
    printf("EvenInt refinement type created\n");
    
    RefinementType* valid_index = create_valid_index_type("my_array");
    assert(valid_index != NULL);
    assert(valid_index->constraint->type == DEP_CONSTRAINT_VALID_INDEX);
    printf("ValidIndex refinement type created\n");
    
    // Test 5: Type Constraints
    printf("\n--- Testing Type Constraints ---\n");
    
    TypeConstraint* range_constraint = create_range_constraint(1, 100);
    assert(range_constraint != NULL);
    assert(range_constraint->type == DEP_CONSTRAINT_RANGE);
    assert(range_constraint->data.range.min_value == 1);
    assert(range_constraint->data.range.max_value == 100);
    
    char* constraint_str = type_constraint_to_string(range_constraint);
    printf("Range constraint: %s\n", constraint_str);
    free(constraint_str);
    
    TypeConstraint* size_constraint = create_size_constraint(DEP_CONSTRAINT_SIZE_LE, 50);
    assert(size_constraint != NULL);
    assert(size_constraint->type == DEP_CONSTRAINT_SIZE_LE);
    assert(size_constraint->data.size.size == 50);
    
    char* size_str = type_constraint_to_string(size_constraint);
    printf("Size constraint: %s\n", size_str);
    free(size_str);
    
    // Test 6: Constraint Solver
    printf("\n--- Testing Constraint Solver ---\n");
    
    ConstraintSolver* solver = context->solver;
    assert(solver != NULL);
    
    // Create a literal AST node for testing
    LiteralNode* lit_42 = malloc(sizeof(LiteralNode));
    memset(lit_42, 0, sizeof(LiteralNode));
    lit_42->base.type = AST_LITERAL;
    lit_42->literal_type = TOKEN_INT;
    lit_42->value = str_dup("42");
    
    // Test non-zero constraint with 42 (should be satisfied)
    SolverResult result = solve_constraint(solver, non_zero->constraint, NULL, (ASTNode*)lit_42);
    assert(result == SOLVER_RESULT_SATISFIED);
    printf("NonZero constraint with 42: %s\n", solver_result_to_string(result));
    
    // Test positive constraint with 42 (should be satisfied)
    result = solve_constraint(solver, positive->constraint, NULL, (ASTNode*)lit_42);
    assert(result == SOLVER_RESULT_SATISFIED);
    printf("Positive constraint with 42: %s\n", solver_result_to_string(result));
    
    // Test range constraint [1,100] with 42 (should be satisfied)
    result = solve_constraint(solver, range_constraint, NULL, (ASTNode*)lit_42);
    assert(result == SOLVER_RESULT_SATISFIED);
    printf("Range [1,100] constraint with 42: %s\n", solver_result_to_string(result));
    
    // Test even constraint with 42 (should be satisfied)
    result = solve_constraint(solver, even->constraint, NULL, (ASTNode*)lit_42);
    assert(result == SOLVER_RESULT_SATISFIED);
    printf("Even constraint with 42: %s\n", solver_result_to_string(result));
    
    // Test with zero value
    LiteralNode* lit_0 = malloc(sizeof(LiteralNode));
    memset(lit_0, 0, sizeof(LiteralNode));
    lit_0->base.type = AST_LITERAL;
    lit_0->literal_type = TOKEN_INT;
    lit_0->value = str_dup("0");
    
    // Test non-zero constraint with 0 (should be unsatisfied)
    result = solve_constraint(solver, non_zero->constraint, NULL, (ASTNode*)lit_0);
    assert(result == SOLVER_RESULT_UNSATISFIED);
    printf("NonZero constraint with 0: %s\n", solver_result_to_string(result));
    
    // Test positive constraint with 0 (should be unsatisfied)
    result = solve_constraint(solver, positive->constraint, NULL, (ASTNode*)lit_0);
    assert(result == SOLVER_RESULT_UNSATISFIED);
    printf("Positive constraint with 0: %s\n", solver_result_to_string(result));
    
    // Test 7: Type Parameter System
    printf("\n--- Testing Type Parameter System ---\n");
    
    TypeParameter* type_param = type_parameter_new(TYPE_PARAM_TYPE, "T");
    assert(type_param != NULL);
    assert(type_param->kind == TYPE_PARAM_TYPE);
    assert(strcmp(type_param->name, "T") == 0);
    printf("Type parameter T created\n");
    
    TypeParameter* value_param = type_parameter_new(TYPE_PARAM_VALUE, "N");
    assert(value_param != NULL);
    assert(value_param->kind == TYPE_PARAM_VALUE);
    assert(strcmp(value_param->name, "N") == 0);
    
    int bind_result = bind_value_parameter(context, value_param, 10);
    assert(bind_result == 1);
    assert(value_param->data.value_param.value == 10);
    assert(value_param->data.value_param.is_resolved == 1);
    printf("Value parameter N bound to 10\n");
    
    // Test 8: Built-in Types
    printf("\n--- Testing Built-in Types ---\n");
    
    // Check that built-in types were registered
    assert(context->dependent_type_count > 0);
    printf("Built-in dependent types registered: %zu\n", context->dependent_type_count);
    
    // Check refinement types
    RefinementType* builtin_non_zero = lookup_refinement_type(context, "NonZeroInt");
    assert(builtin_non_zero != NULL);
    printf("Found built-in NonZeroInt refinement type\n");
    
    RefinementType* builtin_positive = lookup_refinement_type(context, "PositiveInt");
    assert(builtin_positive != NULL);
    printf("Found built-in PositiveInt refinement type\n");
    
    // Test 9: Utility Functions
    printf("\n--- Testing Utility Functions ---\n");
    
    printf("Constraint types:\n");
    printf("- RANGE: %s\n", dependent_constraint_type_to_string(DEP_CONSTRAINT_RANGE));
    printf("- NON_ZERO: %s\n", dependent_constraint_type_to_string(DEP_CONSTRAINT_NON_ZERO));
    printf("- POSITIVE: %s\n", dependent_constraint_type_to_string(DEP_CONSTRAINT_POSITIVE));
    printf("- EVEN: %s\n", dependent_constraint_type_to_string(DEP_CONSTRAINT_EVEN));
    
    printf("\nDependent type kinds:\n");
    printf("- BOUNDED_VEC: %s\n", dependent_type_kind_to_string(DEPENDENT_BOUNDED_VEC));
    printf("- BOUNDED_INT: %s\n", dependent_type_kind_to_string(DEPENDENT_BOUNDED_INT));
    printf("- SIZED_ARRAY: %s\n", dependent_type_kind_to_string(DEPENDENT_SIZED_ARRAY));
    
    printf("\nSolver results:\n");
    printf("- SATISFIED: %s\n", solver_result_to_string(SOLVER_RESULT_SATISFIED));
    printf("- UNSATISFIED: %s\n", solver_result_to_string(SOLVER_RESULT_UNSATISFIED));
    printf("- UNKNOWN: %s\n", solver_result_to_string(SOLVER_RESULT_UNKNOWN));
    
    // Test 10: Statistics
    printf("\n--- Testing Statistics ---\n");
    dependent_type_context_print_statistics(context);
    printf("\n");
    constraint_solver_print_statistics(solver);
    
    // Test 11: Feature Configuration
    printf("\n--- Testing Feature Configuration ---\n");
    
    dependent_type_context_enable_feature(context, "strict_checking", 0);
    assert(context->strict_constraint_checking == 0);
    printf("Strict checking disabled\n");
    
    dependent_type_context_enable_feature(context, "constraint_inference", 0);
    assert(context->enable_constraint_inference == 0);
    printf("Constraint inference disabled\n");
    
    // Cleanup
    type_constraint_free(range_constraint);
    type_constraint_free(size_constraint);
    
    refinement_type_free(non_zero);
    refinement_type_free(positive);
    refinement_type_free(even);
    refinement_type_free(valid_index);
    
    dependent_type_free(bounded_vec_10);
    dependent_type_free(dynamic_vec);
    dependent_type_free(bounded_int_0_255);
    dependent_type_free(dynamic_int);
    dependent_type_free(sized_array_20);
    
    type_parameter_free(type_param);
    type_parameter_free(value_param);
    
    free(lit_42->value);
    free(lit_42);
    free(lit_0->value);
    free(lit_0);
    
    dependent_type_context_free(context);
    type_checker_free(tc);
    
    printf("\n✓ All tests passed successfully!\n");
    printf("✓ Dependent and refinement type system working correctly\n");
    
    return 0;
}