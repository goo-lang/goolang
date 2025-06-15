#include "../types/type_level_programming.c"
#include <stdio.h>
#include <assert.h>

int main() {
    printf("Testing Type-Level Programming Implementation...\n");
    
    // Test 1: Type-level natural numbers
    printf("1. Testing type-level natural numbers...\n");
    TypeLevelNat* zero = type_level_nat_zero();
    assert(zero != NULL);
    assert(zero->kind == NAT_ZERO);
    assert(zero->value == 0);
    
    TypeLevelNat* one = type_level_nat_succ(zero);
    assert(one != NULL);
    assert(one->kind == NAT_SUCC);
    assert(one->value == 1);
    
    TypeLevelNat* two = type_level_nat_succ(one);
    assert(two != NULL);
    assert(two->value == 2);
    
    // Test addition
    TypeLevelNat* three = type_level_nat_add(one, two);
    assert(three != NULL);
    assert(three->value == 3);
    printf("   ✓ Natural numbers working correctly\n");
    
    // Test 2: Type families
    printf("2. Testing type families...\n");
    TypeFamily* add_family = create_add_type_family();
    assert(add_family != NULL);
    assert(strcmp(add_family->name, "Add") == 0);
    assert(add_family->parameter_count == 2);
    
    TypeFamily* mul_family = create_mul_type_family();
    assert(mul_family != NULL);
    assert(strcmp(mul_family->name, "Mul") == 0);
    
    TypeFamily* equal_family = create_equal_type_family();
    assert(equal_family != NULL);
    assert(strcmp(equal_family->name, "Equal") == 0);
    printf("   ✓ Type families created successfully\n");
    
    // Test 3: Patterns
    printf("3. Testing type patterns...\n");
    TypePattern* wildcard = type_pattern_new(TYPE_PATTERN_WILDCARD, NULL);
    assert(wildcard != NULL);
    assert(wildcard->kind == TYPE_PATTERN_WILDCARD);
    
    TypePattern* variable = type_pattern_new(TYPE_PATTERN_VARIABLE, "T");
    assert(variable != NULL);
    assert(variable->kind == TYPE_PATTERN_VARIABLE);
    assert(strcmp(variable->name, "T") == 0);
    
    TypePattern* constructor = type_pattern_new(TYPE_PATTERN_CONSTRUCTOR, "Zero");
    assert(constructor != NULL);
    assert(constructor->kind == TYPE_PATTERN_CONSTRUCTOR);
    assert(strcmp(constructor->name, "Zero") == 0);
    printf("   ✓ Type patterns working correctly\n");
    
    // Test 4: Dependent types
    printf("4. Testing dependent types...\n");
    Type* int_type = type_new(TYPE_INT32);
    assert(int_type != NULL);
    
    DependentType* dep_type = dependent_type_new(DEP_SIZE_DEPENDENT, "TestDep", int_type);
    assert(dep_type != NULL);
    assert(dep_type->kind == DEP_SIZE_DEPENDENT);
    assert(strcmp(dep_type->name, "TestDep") == 0);
    assert(dep_type->base_type != NULL);
    
    // Test compile-time array
    Type* array_type = create_compile_time_array_type(int_type, two);
    assert(array_type != NULL);
    assert(array_type->kind == TYPE_ARRAY);
    assert(array_type->data.array.length == 2);
    
    // Test proof type
    Type* proof_type = create_proof_type("x > 0");
    assert(proof_type != NULL);
    assert(proof_type->size == 0); // Phantom types have zero size
    printf("   ✓ Dependent types working correctly\n");
    
    // Test 5: Type-level computations
    printf("5. Testing type-level computations...\n");
    TypeLevelComputation* const_comp = type_level_computation_new(TYPE_LEVEL_CONST, "TestConst");
    assert(const_comp != NULL);
    assert(const_comp->kind == TYPE_LEVEL_CONST);
    assert(strcmp(const_comp->name, "TestConst") == 0);
    assert(const_comp->is_const_evaluable == 1);
    
    TypeLevelComputation* func_comp = type_level_computation_new(TYPE_LEVEL_FUNCTION, "TestFunc");
    assert(func_comp != NULL);
    assert(func_comp->kind == TYPE_LEVEL_FUNCTION);
    
    TypeLevelComputation* dep_comp = type_level_computation_new(TYPE_LEVEL_DEPENDENT, "TestDep");
    assert(dep_comp != NULL);
    assert(dep_comp->kind == TYPE_LEVEL_DEPENDENT);
    assert(dep_comp->is_const_evaluable == 0); // Dependent types not const-evaluable by default
    printf("   ✓ Type-level computations working correctly\n");
    
    // Test 6: Matrix types
    printf("6. Testing matrix types...\n");
    Type* float_type = type_new(TYPE_FLOAT32);
    Type* matrix_type = create_compile_time_matrix_type(float_type, two, three);
    assert(matrix_type != NULL);
    assert(matrix_type->kind == TYPE_STRUCT);
    assert(matrix_type->data.struct_type.field_count == 1);
    
    StructField* data_field = &matrix_type->data.struct_type.fields[0];
    assert(strcmp(data_field->name, "data") == 0);
    assert(data_field->type->kind == TYPE_ARRAY);
    assert(data_field->type->data.array.length == 6); // 2 * 3 = 6
    printf("   ✓ Matrix types working correctly\n");
    
    // Test 7: Evaluation context
    printf("7. Testing evaluation context...\n");
    TypeChecker* checker = malloc(sizeof(TypeChecker));
    memset(checker, 0, sizeof(TypeChecker));
    
    TypeEvalContext* ctx = type_eval_context_new(checker);
    assert(ctx != NULL);
    assert(ctx->variable_bindings != NULL);
    assert(ctx->evaluation_depth == 0);
    assert(ctx->max_evaluation_depth == 100);
    
    int builtin_success = type_eval_context_init_builtins(ctx);
    assert(builtin_success);
    assert(ctx->family_count >= 3); // Should have Add, Mul, Equal families
    
    // Test natural number caching
    TypeLevelNat* cached_zero = type_eval_context_get_nat(ctx, 0);
    TypeLevelNat* cached_one = type_eval_context_get_nat(ctx, 1);
    assert(cached_zero != NULL);
    assert(cached_one != NULL);
    assert(cached_zero->value == 0);
    assert(cached_one->value == 1);
    
    // Test that requesting the same number returns cached version
    TypeLevelNat* cached_one_again = type_eval_context_get_nat(ctx, 1);
    assert(cached_one == cached_one_again);
    printf("   ✓ Evaluation context working correctly\n");
    
    // Cleanup
    type_level_nat_free(zero);
    type_level_nat_free(one);
    type_level_nat_free(two);
    type_level_nat_free(three);
    
    type_family_free(add_family);
    type_family_free(mul_family);
    type_family_free(equal_family);
    
    type_pattern_free(wildcard);
    type_pattern_free(variable);
    type_pattern_free(constructor);
    
    dependent_type_free(dep_type);
    type_free(array_type);
    type_free(proof_type);
    type_free(matrix_type);
    
    type_level_computation_free(const_comp);
    type_level_computation_free(func_comp);
    type_level_computation_free(dep_comp);
    
    type_eval_context_free(ctx);
    free(checker);
    
    printf("\n✅ All type-level programming tests passed!\n");
    return 0;
}