#include "../include/derive_macros.h"
#include "../include/errors/error.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test utilities
static int test_count = 0;
static int pass_count = 0;
static int fail_count = 0;

#define TEST(name) do { \
    printf("Testing %s... ", name); \
    fflush(stdout); \
    test_count++; \
} while(0)

#define PASS() do { \
    printf("PASSED\n"); \
    pass_count++; \
} while(0)

#define FAIL(msg) do { \
    printf("FAILED: %s\n", msg); \
    fail_count++; \
} while(0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        FAIL(#expr " is not true"); \
        return; \
    } \
} while(0)

#define ASSERT_FALSE(expr) do { \
    if (expr) { \
        FAIL(#expr " is not false"); \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        char msg[256]; \
        snprintf(msg, sizeof(msg), "%s != %s", #a, #b); \
        FAIL(msg); \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        char msg[256]; \
        snprintf(msg, sizeof(msg), "'%s' != '%s'", (a), (b)); \
        FAIL(msg); \
        return; \
    } \
} while(0)

#define ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        FAIL(#ptr " is NULL"); \
        return; \
    } \
} while(0)

#define ASSERT_NULL(ptr) do { \
    if ((ptr) != NULL) { \
        FAIL(#ptr " is not NULL"); \
        return; \
    } \
} while(0)

#define ASSERT_CONTAINS(str, substr) do { \
    if (!strstr((str), (substr))) { \
        char msg[512]; \
        snprintf(msg, sizeof(msg), "'%s' does not contain '%s'", (str), (substr)); \
        FAIL(msg); \
        return; \
    } \
} while(0)

// Test derive macro registry creation
void test_derive_registry_creation() {
    TEST("derive macro registry creation");
    
    MacroRegistry* registry = create_derive_macro_registry();
    ASSERT_NOT_NULL(registry);
    
    // Check that derive macro is registered
    MacroTemplate* derive_macro = find_macro(registry, "derive");
    ASSERT_NOT_NULL(derive_macro);
    ASSERT_EQ(derive_macro->type, MACRO_ATTRIBUTE);
    ASSERT_EQ(derive_macro->param_count, 2);
    
    destroy_macro_registry(registry);
    PASS();
}

// Test derive type parsing
void test_derive_type_parsing() {
    TEST("derive type parsing");
    
    // Test supported types
    ASSERT_TRUE(is_derive_type_supported("Debug"));
    ASSERT_TRUE(is_derive_type_supported("Clone"));
    ASSERT_TRUE(is_derive_type_supported("PartialEq"));
    ASSERT_TRUE(is_derive_type_supported("Hash"));
    ASSERT_TRUE(is_derive_type_supported("Default"));
    
    // Test unsupported types
    ASSERT_FALSE(is_derive_type_supported("Unknown"));
    ASSERT_FALSE(is_derive_type_supported("InvalidType"));
    ASSERT_FALSE(is_derive_type_supported(""));
    
    // Test parsing
    ASSERT_EQ(parse_derive_type("Debug"), DERIVE_DEBUG);
    ASSERT_EQ(parse_derive_type("Clone"), DERIVE_CLONE);
    ASSERT_EQ(parse_derive_type("PartialEq"), DERIVE_PARTIAL_EQ);
    ASSERT_EQ(parse_derive_type("Hash"), DERIVE_HASH);
    ASSERT_EQ(parse_derive_type("Unknown"), DERIVE_COUNT);
    
    // Test string conversion
    ASSERT_STR_EQ(derive_type_to_string(DERIVE_DEBUG), "Debug");
    ASSERT_STR_EQ(derive_type_to_string(DERIVE_CLONE), "Clone");
    ASSERT_STR_EQ(derive_type_to_string(DERIVE_PARTIAL_EQ), "PartialEq");
    
    PASS();
}

// Test derive context creation
void test_derive_context_creation() {
    TEST("derive context creation");
    
    // Create a mock struct node
    ASTNode* mock_struct = ast_node_new(AST_STRUCT_TYPE, (Position){1, 1, 0, "test.goo"});
    ASSERT_NOT_NULL(mock_struct);
    
    DeriveMacroContext* ctx = create_derive_context(mock_struct, DERIVE_DEBUG);
    ASSERT_NOT_NULL(ctx);
    ASSERT_EQ(ctx->derive_type, DERIVE_DEBUG);
    ASSERT_EQ(ctx->target_struct, mock_struct);
    ASSERT_TRUE(ctx->generate_comments);
    ASSERT_FALSE(ctx->optimize_for_size);
    
    // Check that fields were analyzed (mock data)
    ASSERT_EQ(ctx->field_count, 3);
    ASSERT_NOT_NULL(ctx->field_names);
    ASSERT_NOT_NULL(ctx->field_types);
    ASSERT_STR_EQ(ctx->field_names[0], "id");
    ASSERT_STR_EQ(ctx->field_names[1], "name");
    ASSERT_STR_EQ(ctx->field_names[2], "email");
    
    destroy_derive_context(ctx);
    ast_node_free(mock_struct);
    PASS();
}

// Test Debug derive implementation
void test_derive_debug() {
    TEST("Debug derive implementation");
    
    ASTNode* mock_struct = ast_node_new(AST_STRUCT_TYPE, (Position){1, 1, 0, "test.goo"});
    DeriveMacroContext* ctx = create_derive_context(mock_struct, DERIVE_DEBUG);
    ASSERT_NOT_NULL(ctx);
    
    DeriveResult* result = derive_debug(ctx);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    ASSERT_NOT_NULL(result->function_code);
    
    // Check that generated code contains expected elements
    ASSERT_CONTAINS(result->function_code, "debug()");
    ASSERT_CONTAINS(result->function_code, "User {");
    ASSERT_CONTAINS(result->function_code, "string_builder");
    ASSERT_CONTAINS(result->function_code, "self->id");
    ASSERT_CONTAINS(result->function_code, "self->name");
    ASSERT_CONTAINS(result->function_code, "self->email");
    
    destroy_derive_result(result);
    destroy_derive_context(ctx);
    ast_node_free(mock_struct);
    PASS();
}

// Test Clone derive implementation
void test_derive_clone() {
    TEST("Clone derive implementation");
    
    ASTNode* mock_struct = ast_node_new(AST_STRUCT_TYPE, (Position){1, 1, 0, "test.goo"});
    DeriveMacroContext* ctx = create_derive_context(mock_struct, DERIVE_CLONE);
    ASSERT_NOT_NULL(ctx);
    
    DeriveResult* result = derive_clone(ctx);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    ASSERT_NOT_NULL(result->function_code);
    
    // Check generated code
    ASSERT_CONTAINS(result->function_code, "clone()");
    ASSERT_CONTAINS(result->function_code, "User{");
    ASSERT_CONTAINS(result->function_code, "clone_value(self->id)");
    ASSERT_CONTAINS(result->function_code, "clone_value(self->name)");
    ASSERT_CONTAINS(result->function_code, "clone_value(self->email)");
    
    destroy_derive_result(result);
    destroy_derive_context(ctx);
    ast_node_free(mock_struct);
    PASS();
}

// Test PartialEq derive implementation
void test_derive_partial_eq() {
    TEST("PartialEq derive implementation");
    
    ASTNode* mock_struct = ast_node_new(AST_STRUCT_TYPE, (Position){1, 1, 0, "test.goo"});
    DeriveMacroContext* ctx = create_derive_context(mock_struct, DERIVE_PARTIAL_EQ);
    ASSERT_NOT_NULL(ctx);
    
    DeriveResult* result = derive_partial_eq(ctx);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    ASSERT_NOT_NULL(result->function_code);
    
    // Check generated code
    ASSERT_CONTAINS(result->function_code, "eq(other User)");
    ASSERT_CONTAINS(result->function_code, "return");
    ASSERT_CONTAINS(result->function_code, "eq_value(self->id, other.id)");
    ASSERT_CONTAINS(result->function_code, "eq_value(self->name, other.name)");
    ASSERT_CONTAINS(result->function_code, "&&");
    
    destroy_derive_result(result);
    destroy_derive_context(ctx);
    ast_node_free(mock_struct);
    PASS();
}

// Test Hash derive implementation
void test_derive_hash() {
    TEST("Hash derive implementation");
    
    ASTNode* mock_struct = ast_node_new(AST_STRUCT_TYPE, (Position){1, 1, 0, "test.goo"});
    DeriveMacroContext* ctx = create_derive_context(mock_struct, DERIVE_HASH);
    ASSERT_NOT_NULL(ctx);
    
    DeriveResult* result = derive_hash(ctx);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    ASSERT_NOT_NULL(result->function_code);
    
    // Check generated code
    ASSERT_CONTAINS(result->function_code, "hash() uint64");
    ASSERT_CONTAINS(result->function_code, "hasher_new()");
    ASSERT_CONTAINS(result->function_code, "hasher_write");
    ASSERT_CONTAINS(result->function_code, "hash_value(self->id)");
    ASSERT_CONTAINS(result->function_code, "hasher_finish");
    
    destroy_derive_result(result);
    destroy_derive_context(ctx);
    ast_node_free(mock_struct);
    PASS();
}

// Test Default derive implementation
void test_derive_default() {
    TEST("Default derive implementation");
    
    ASTNode* mock_struct = ast_node_new(AST_STRUCT_TYPE, (Position){1, 1, 0, "test.goo"});
    DeriveMacroContext* ctx = create_derive_context(mock_struct, DERIVE_DEFAULT);
    ASSERT_NOT_NULL(ctx);
    
    DeriveResult* result = derive_default(ctx);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    ASSERT_NOT_NULL(result->function_code);
    
    // Check generated code
    ASSERT_CONTAINS(result->function_code, "User_default() User");
    ASSERT_CONTAINS(result->function_code, "return User{");
    ASSERT_CONTAINS(result->function_code, "id: default_value");
    ASSERT_CONTAINS(result->function_code, "name: default_value");
    ASSERT_CONTAINS(result->function_code, "email: default_value");
    
    destroy_derive_result(result);
    destroy_derive_context(ctx);
    ast_node_free(mock_struct);
    PASS();
}

// Test utility functions
void test_utility_functions() {
    TEST("utility functions");
    
    // Test function signature generation
    char* sig = generate_function_signature("test_func", "TestStruct", "bool", "int x, string y");
    ASSERT_NOT_NULL(sig);
    ASSERT_CONTAINS(sig, "func (TestStruct* self) test_func(int x, string y) bool");
    free(sig);
    
    // Test without parameters
    sig = generate_function_signature("simple_func", "MyStruct", "void", "");
    ASSERT_NOT_NULL(sig);
    ASSERT_CONTAINS(sig, "func (MyStruct* self) simple_func() void");
    free(sig);
    
    // Test derive macro info
    char* info = get_derive_macro_info(DERIVE_DEBUG);
    ASSERT_NOT_NULL(info);
    ASSERT_CONTAINS(info, "Debug");
    ASSERT_CONTAINS(info, "automatic implementation");
    free(info);
    
    PASS();
}

// Test derive macro evaluator
void test_derive_evaluator() {
    TEST("derive macro evaluator");
    
    // Create mock context and arguments
    MacroTemplate* mock_macro = create_macro_template("derive_test", MACRO_ATTRIBUTE);
    MacroContext* ctx = create_macro_context(mock_macro, NULL, 0);
    
    ComptimeValue* traits = create_comptime_string("Debug,Clone");
    ComptimeValue* target = create_comptime_string("User");
    ComptimeValue* args[] = { traits, target };
    
    ctx->arguments = args;
    ctx->arg_count = 2;
    
    ComptimeValue* result = derive_macro_evaluator(ctx, args);
    ASSERT_NOT_NULL(result);
    ASSERT_EQ(result->type, COMPTIME_VALUE_STRING);
    ASSERT_STR_EQ(result->string_value, "derive_macro_generated");
    
    destroy_macro_context(ctx);
    destroy_macro_template(mock_macro);
    PASS();
}

// Test error handling
void test_error_handling() {
    TEST("error handling");
    
    // Test with invalid inputs
    DeriveMacroContext* ctx = create_derive_context(NULL, DERIVE_DEBUG);
    ASSERT_NULL(ctx);
    
    DeriveResult* result = derive_debug(NULL);
    ASSERT_NULL(result);
    
    // Test derive type parsing errors
    ASSERT_EQ(parse_derive_type(NULL), DERIVE_COUNT);
    ASSERT_EQ(parse_derive_type("InvalidType"), DERIVE_COUNT);
    
    PASS();
}

// Test debug and introspection
void test_debug_functions() {
    TEST("debug and introspection");
    
    ASTNode* mock_struct = ast_node_new(AST_STRUCT_TYPE, (Position){1, 1, 0, "test.goo"});
    DeriveMacroContext* ctx = create_derive_context(mock_struct, DERIVE_DEBUG);
    ASSERT_NOT_NULL(ctx);
    
    // Test print function (should not crash)
    print_derive_context(ctx);
    
    destroy_derive_context(ctx);
    ast_node_free(mock_struct);
    PASS();
}

// Main test runner
int main() {
    printf("=== Derive Macro System Tests ===\n\n");
    
    // Run tests
    test_derive_registry_creation();
    test_derive_type_parsing();
    test_derive_context_creation();
    test_derive_debug();
    test_derive_clone();
    test_derive_partial_eq();
    test_derive_hash();
    test_derive_default();
    test_utility_functions();
    test_derive_evaluator();
    test_error_handling();
    test_debug_functions();
    
    // Print summary
    printf("\n=== Test Summary ===\n");
    printf("Total tests: %d\n", test_count);
    printf("Passed: %d\n", pass_count);
    printf("Failed: %d\n", fail_count);
    
    return fail_count > 0 ? 1 : 0;
}