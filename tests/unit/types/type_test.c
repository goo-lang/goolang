#include "types.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

// Simple test suite for the type system

void test_basic_types(void) {
    printf("Testing basic types...\n");
    
    Type* int_type = type_int(32, 1);
    assert(int_type != NULL);
    assert(int_type->kind == TYPE_INT32);
    assert(int_type->size == 4);
    assert(type_is_integer(int_type));
    assert(type_is_signed(int_type));
    type_free(int_type);
    
    Type* float_type = type_float(64);
    assert(float_type != NULL);
    assert(float_type->kind == TYPE_FLOAT64);
    assert(float_type->size == 8);
    assert(type_is_float(float_type));
    assert(type_is_numeric(float_type));
    type_free(float_type);
    
    Type* bool_type = type_bool();
    assert(bool_type != NULL);
    assert(bool_type->kind == TYPE_BOOL);
    assert(bool_type->size == 1);
    type_free(bool_type);
    
    printf("Basic types test passed!\n");
}

void test_compound_types(void) {
    printf("Testing compound types...\n");
    
    Type* int_type = type_int(32, 1);
    Type* array_type = type_array(int_type, 10);
    assert(array_type != NULL);
    assert(array_type->kind == TYPE_ARRAY);
    assert(array_type->data.array.length == 10);
    assert(array_type->data.array.element_type == int_type);
    assert(array_type->size == 40);  // 10 * 4 bytes
    type_free(array_type);
    
    Type* slice_type = type_slice(int_type);
    assert(slice_type != NULL);
    assert(slice_type->kind == TYPE_SLICE);
    assert(slice_type->data.slice.element_type == int_type);
    type_free(slice_type);
    
    Type* string_type = type_string_type();
    Type* map_type = type_map(string_type, int_type);
    assert(map_type != NULL);
    assert(map_type->kind == TYPE_MAP);
    assert(map_type->data.map.key_type == string_type);
    assert(map_type->data.map.value_type == int_type);
    type_free(map_type);
    
    printf("Compound types test passed!\n");
}

void test_goo_extensions(void) {
    printf("Testing Goo extension types...\n");
    
    Type* int_type = type_int(32, 1);
    
    // Test error union type
    Type* error_union = type_error_union(int_type, NULL);
    assert(error_union != NULL);
    assert(error_union->kind == TYPE_ERROR_UNION);
    assert(error_union->data.error_union.value_type == int_type);
    assert(type_is_error_union(error_union));
    type_free(error_union);
    
    // Test nullable type
    Type* nullable = type_nullable(int_type);
    assert(nullable != NULL);
    assert(nullable->kind == TYPE_NULLABLE);
    assert(nullable->data.nullable.base_type == int_type);
    assert(type_is_nullable(nullable));
    type_free(nullable);
    
    // Test qualified type
    Type* qualified = type_qualified(int_type, OWNERSHIP_OWNED, MUTABILITY_MUTABLE);
    assert(qualified != NULL);
    assert(qualified->kind == TYPE_QUALIFIED);
    assert(qualified->data.qualified.base_type == int_type);
    assert(qualified->data.qualified.ownership == OWNERSHIP_OWNED);
    assert(qualified->data.qualified.mutability == MUTABILITY_MUTABLE);
    type_free(qualified);
    
    // Test channel type
    Type* channel = type_channel(int_type, CHAN_PATTERN_PUB);
    assert(channel != NULL);
    assert(channel->kind == TYPE_CHANNEL);
    assert(channel->data.channel.element_type == int_type);
    assert(channel->data.channel.pattern == CHAN_PATTERN_PUB);
    type_free(channel);
    
    printf("Goo extension types test passed!\n");
}

void test_type_compatibility(void) {
    printf("Testing type compatibility...\n");
    
    Type* int32 = type_int(32, 1);
    Type* int64 = type_int(64, 1);
    Type* float32 = type_float(32);
    
    // Same types should be equal
    assert(type_equals(int32, int32));
    
    // Different types should not be equal
    assert(!type_equals(int32, int64));
    assert(!type_equals(int32, float32));
    
    // Numeric types should be compatible
    assert(type_compatible(int32, int64));
    assert(type_compatible(int32, float32));
    
    type_free(int32);
    type_free(int64);
    type_free(float32);
    
    printf("Type compatibility test passed!\n");
}

void test_type_checker_creation(void) {
    printf("Testing type checker creation...\n");
    
    TypeChecker* checker = type_checker_new();
    assert(checker != NULL);
    assert(checker->current_scope != NULL);
    assert(checker->builtin_types != NULL);
    assert(checker->error_count == 0);
    
    // Test builtin types
    Type* int_type = type_checker_get_builtin(checker, TYPE_INT32);
    assert(int_type != NULL);
    assert(int_type->kind == TYPE_INT32);
    
    Type* bool_type = type_checker_get_builtin(checker, TYPE_BOOL);
    assert(bool_type != NULL);
    assert(bool_type->kind == TYPE_BOOL);
    
    type_checker_free(checker);
    
    printf("Type checker creation test passed!\n");
}

void test_variable_management(void) {
    printf("Testing variable management...\n");
    
    Position pos = {1, 1, 0, "test.goo"};
    Type* int_type = type_int(32, 1);
    
    Variable* var = variable_new("x", int_type, pos);
    assert(var != NULL);
    assert(strcmp(var->name, "x") == 0);
    assert(var->type == int_type);
    assert(var->ownership == OWNERSHIP_OWNED);
    assert(!var->is_moved);
    assert(!var->is_initialized);
    
    Scope* scope = scope_new(NULL);
    assert(scope != NULL);
    
    // Add variable to scope
    assert(scope_add_variable(scope, var));
    
    // Look up variable
    Variable* found = scope_lookup_variable(scope, "x");
    assert(found == var);
    
    // Try to add duplicate
    Variable* duplicate = variable_new("x", int_type, pos);
    assert(!scope_add_variable(scope, duplicate));
    variable_free(duplicate);
    
    scope_free(scope);
    type_free(int_type);
    
    printf("Variable management test passed!\n");
}

void run_type_system_tests(void) {
    printf("Running type system tests...\n\n");
    
    test_basic_types();
    test_compound_types();
    test_goo_extensions();
    test_type_compatibility();
    test_type_checker_creation();
    test_variable_management();
    
    printf("\nAll type system tests passed!\n");
}

// Function to be called from main
int test_type_system(void) {
    run_type_system_tests();
    return 0;
}