#include "../types/type_level_programming.c"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test type-level natural numbers
int test_type_level_nat_basic() {
    TEST_DESCRIPTION("Test basic type-level natural number operations");
    
    // Test Zero
    TypeLevelNat* zero = type_level_nat_zero();
    assert_not_null(zero, "Failed to create Zero natural number");
    assert_equal(zero->kind, NAT_ZERO, "Zero should have NAT_ZERO kind");
    assert_equal(zero->value, 0, "Zero should have value 0");
    assert_null(zero->predecessor, "Zero should have no predecessor");
    
    // Test Succ(Zero) = 1
    TypeLevelNat* one = type_level_nat_succ(zero);
    assert_not_null(one, "Failed to create Succ(Zero)");
    assert_equal(one->kind, NAT_SUCC, "Succ should have NAT_SUCC kind");
    assert_equal(one->value, 1, "Succ(Zero) should have value 1");
    assert_equal(one->predecessor, zero, "Succ(Zero) predecessor should be Zero");
    
    // Test Succ(Succ(Zero)) = 2
    TypeLevelNat* two = type_level_nat_succ(one);
    assert_not_null(two, "Failed to create Succ(Succ(Zero))");
    assert_equal(two->value, 2, "Succ(Succ(Zero)) should have value 2");
    
    // Test addition: Add(1, 2) = 3
    TypeLevelNat* three = type_level_nat_add(one, two);
    assert_not_null(three, "Failed to compute Add(1, 2)");
    assert_equal(three->value, 3, "Add(1, 2) should equal 3");
    
    // Test addition identity: Add(Zero, N) = N
    TypeLevelNat* identity_result = type_level_nat_add(zero, two);
    assert_not_null(identity_result, "Failed to compute Add(Zero, 2)");
    assert_equal(identity_result->value, 2, "Add(Zero, 2) should equal 2");
    
    type_level_nat_free(zero);
    type_level_nat_free(one);
    type_level_nat_free(two);
    type_level_nat_free(three);
    type_level_nat_free(identity_result);
    
    return 1;
}

// Test type families
int test_type_families() {
    TEST_DESCRIPTION("Test type family pattern matching and evaluation");
    
    // Create Add type family
    TypeFamily* add_family = create_add_type_family();
    assert_not_null(add_family, "Failed to create Add type family");
    assert_string_equal(add_family->name, "Add", "Add family should have correct name");
    assert_equal(add_family->parameter_count, 2, "Add family should have 2 parameters");
    assert_not_null(add_family->cases, "Add family should have cases");
    
    // Create Equal type family
    TypeFamily* equal_family = create_equal_type_family();
    assert_not_null(equal_family, "Failed to create Equal type family");
    assert_string_equal(equal_family->name, "Equal", "Equal family should have correct name");
    
    // Create Mul type family
    TypeFamily* mul_family = create_mul_type_family();
    assert_not_null(mul_family, "Failed to create Mul type family");
    assert_string_equal(mul_family->name, "Mul", "Mul family should have correct name");
    
    type_family_free(add_family);
    type_family_free(equal_family);
    type_family_free(mul_family);
    
    return 1;
}

// Test dependent types
int test_dependent_types() {
    TEST_DESCRIPTION("Test dependent type creation and constraints");
    
    // Create base type
    Type* int_type = type_new(TYPE_INT32);
    assert_not_null(int_type, "Failed to create int type");
    
    // Create dependent type
    DependentType* dep_type = dependent_type_new(0, "TestDepType", int_type);
    assert_not_null(dep_type, "Failed to create dependent type");
    assert_string_equal(dep_type->name, "TestDepType", "Dependent type should have correct name");
    assert_not_null(dep_type->base_type, "Dependent type should have base type");
    assert_equal(dep_type->is_compile_time, 1, "Dependent type should default to compile-time");
    
    // Test compile-time array type
    TypeLevelNat* size = type_level_nat_succ(type_level_nat_succ(type_level_nat_zero())); // 2
    Type* array_type = create_compile_time_array_type(int_type, size);
    assert_not_null(array_type, "Failed to create compile-time array type");
    assert_equal(array_type->kind, TYPE_ARRAY, "Should create array type");
    assert_equal(array_type->data.array.length, 2, "Array should have correct size");
    
    // Test proof type
    Type* proof_type = create_proof_type("x > 0");
    assert_not_null(proof_type, "Failed to create proof type");
    assert_not_null(proof_type->name, "Proof type should have name");
    assert_equal(proof_type->size, 0, "Proof types should have zero runtime size");
    
    dependent_type_free(dep_type);
    type_free(array_type);
    type_free(proof_type);
    type_level_nat_free(size);
    
    return 1;
}

// Test compile-time evaluation engine
int test_compile_time_evaluation() {
    TEST_DESCRIPTION("Test compile-time evaluation of type expressions");
    
    // Create type checker (mock)
    TypeChecker* checker = malloc(sizeof(TypeChecker));
    memset(checker, 0, sizeof(TypeChecker));
    
    // Create evaluation context
    TypeEvalContext* ctx = type_eval_context_new(checker);
    assert_not_null(ctx, "Failed to create type evaluation context");
    assert_not_null(ctx->variable_bindings, "Context should have variable bindings");
    assert_equal(ctx->evaluation_depth, 0, "Initial evaluation depth should be 0");
    assert_equal(ctx->max_evaluation_depth, 100, "Max evaluation depth should be 100");
    
    // Initialize built-in type families
    int builtin_success = type_eval_context_init_builtins(ctx);
    assert_true(builtin_success, "Failed to initialize built-in type families");
    assert_true(ctx->family_count >= 3, "Should have at least 3 built-in families");
    
    // Test natural number caching
    TypeLevelNat* nat0 = type_eval_context_get_nat(ctx, 0);
    TypeLevelNat* nat1 = type_eval_context_get_nat(ctx, 1);
    TypeLevelNat* nat2 = type_eval_context_get_nat(ctx, 2);
    
    assert_not_null(nat0, "Failed to get nat 0");
    assert_not_null(nat1, "Failed to get nat 1");
    assert_not_null(nat2, "Failed to get nat 2");
    assert_equal(nat0->value, 0, "Nat 0 should have value 0");
    assert_equal(nat1->value, 1, "Nat 1 should have value 1");
    assert_equal(nat2->value, 2, "Nat 2 should have value 2");
    
    // Test that requesting the same natural number returns cached version
    TypeLevelNat* nat1_again = type_eval_context_get_nat(ctx, 1);
    assert_equal(nat1, nat1_again, "Should return cached natural number");
    
    type_eval_context_free(ctx);
    free(checker);
    
    return 1;
}

// Test type-level computation evaluation
int test_type_level_computations() {
    TEST_DESCRIPTION("Test type-level computation creation and evaluation");
    
    // Create type checker (mock)
    TypeChecker* checker = malloc(sizeof(TypeChecker));
    memset(checker, 0, sizeof(TypeChecker));
    
    // Test constant computation
    TypeLevelComputation* const_comp = type_level_computation_new(TYPE_LEVEL_CONST, "TestConst");
    assert_not_null(const_comp, "Failed to create constant computation");
    assert_string_equal(const_comp->name, "TestConst", "Computation should have correct name");
    assert_equal(const_comp->kind, TYPE_LEVEL_CONST, "Should have correct kind");
    assert_equal(const_comp->is_const_evaluable, 1, "Constants should be const-evaluable");
    
    // Test function computation
    TypeLevelComputation* func_comp = type_level_computation_new(TYPE_LEVEL_FUNCTION, "TestFunc");
    assert_not_null(func_comp, "Failed to create function computation");
    assert_equal(func_comp->kind, TYPE_LEVEL_FUNCTION, "Should have correct kind");
    
    // Test dependent computation
    TypeLevelComputation* dep_comp = type_level_computation_new(TYPE_LEVEL_DEPENDENT, "TestDep");
    assert_not_null(dep_comp, "Failed to create dependent computation");
    assert_equal(dep_comp->kind, TYPE_LEVEL_DEPENDENT, "Should have correct kind");
    assert_equal(dep_comp->is_const_evaluable, 0, "Dependent types should not be const-evaluable by default");
    
    // Test const-evaluability check
    assert_true(type_level_computation_is_const_evaluable(const_comp), "Constant should be const-evaluable");
    assert_true(type_level_computation_is_const_evaluable(func_comp), "Function should be const-evaluable by default");
    assert_false(type_level_computation_is_const_evaluable(dep_comp), "Dependent should not be const-evaluable");
    
    type_level_computation_free(const_comp);
    type_level_computation_free(func_comp);
    type_level_computation_free(dep_comp);
    free(checker);
    
    return 1;
}

// Test matrix type creation
int test_matrix_types() {
    TEST_DESCRIPTION("Test compile-time matrix type creation");
    
    // Create element type
    Type* float_type = type_new(TYPE_FLOAT32);
    assert_not_null(float_type, "Failed to create float type");
    
    // Create matrix dimensions
    TypeLevelNat* rows = type_level_nat_succ(type_level_nat_succ(type_level_nat_zero())); // 2
    TypeLevelNat* cols = type_level_nat_succ(type_level_nat_succ(type_level_nat_succ(type_level_nat_zero()))); // 3
    
    // Create matrix type
    Type* matrix_type = create_compile_time_matrix_type(float_type, rows, cols);
    assert_not_null(matrix_type, "Failed to create matrix type");
    assert_equal(matrix_type->kind, TYPE_STRUCT, "Matrix should be a struct type");
    assert_not_null(matrix_type->name, "Matrix should have a name");
    assert_equal(matrix_type->data.struct_type.field_count, 1, "Matrix should have one field");
    
    // Check the data field
    StructField* data_field = &matrix_type->data.struct_type.fields[0];
    assert_string_equal(data_field->name, "data", "Field should be named 'data'");
    assert_equal(data_field->type->kind, TYPE_ARRAY, "Data field should be an array");
    assert_equal(data_field->type->data.array.length, 6, "Array should have 2*3=6 elements");
    
    type_free(matrix_type);
    type_level_nat_free(rows);
    type_level_nat_free(cols);
    
    return 1;
}

// Test pattern matching
int test_pattern_matching() {
    TEST_DESCRIPTION("Test type pattern creation and matching");
    
    // Create patterns
    TypePattern* wildcard = type_pattern_new(TYPE_PATTERN_WILDCARD, NULL);
    TypePattern* variable = type_pattern_new(TYPE_PATTERN_VARIABLE, "T");
    TypePattern* constructor = type_pattern_new(TYPE_PATTERN_CONSTRUCTOR, "Zero");
    
    assert_not_null(wildcard, "Failed to create wildcard pattern");
    assert_not_null(variable, "Failed to create variable pattern");
    assert_not_null(constructor, "Failed to create constructor pattern");
    
    assert_equal(wildcard->kind, TYPE_PATTERN_WILDCARD, "Should have wildcard kind");
    assert_equal(variable->kind, TYPE_PATTERN_VARIABLE, "Should have variable kind");
    assert_equal(constructor->kind, TYPE_PATTERN_CONSTRUCTOR, "Should have constructor kind");
    
    assert_string_equal(variable->name, "T", "Variable should have correct name");
    assert_string_equal(constructor->name, "Zero", "Constructor should have correct name");
    
    // Test subpattern addition
    TypePattern* sub_pattern = type_pattern_new(TYPE_PATTERN_VARIABLE, "A");
    int add_result = type_pattern_add_subpattern(constructor, sub_pattern);
    assert_true(add_result, "Should successfully add subpattern");
    assert_equal(constructor->subpattern_count, 1, "Should have one subpattern");
    assert_equal(constructor->subpatterns[0], sub_pattern, "Should store correct subpattern");
    
    type_pattern_free(wildcard);
    type_pattern_free(variable);
    type_pattern_free(constructor);
    
    return 1;
}

// Run all type-level programming tests
void run_type_level_programming_tests() {
    printf("\n=== Type-Level Programming Tests ===\n");
    
    RUN_TEST(test_type_level_nat_basic);
    RUN_TEST(test_type_families);
    RUN_TEST(test_dependent_types);
    RUN_TEST(test_compile_time_evaluation);
    RUN_TEST(test_type_level_computations);
    RUN_TEST(test_matrix_types);
    RUN_TEST(test_pattern_matching);
    
    printf("\n=== Type-Level Programming Tests Complete ===\n");
}

#ifdef STANDALONE_TEST
int main(void) {
    run_type_level_programming_tests();
    return 0;
}
#endif