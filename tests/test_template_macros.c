#include "../include/template_macros.h"
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

// Test template context creation
void test_template_context_creation() {
    TEST("template context creation");
    
    const char* template_code = "func {{name}}() {{return_type}} { return {{default_value}}; }";
    TemplateContext* ctx = create_template_context(template_code);
    
    ASSERT_NOT_NULL(ctx);
    ASSERT_NOT_NULL(ctx->template_code);
    ASSERT_STR_EQ(ctx->template_code, template_code);
    ASSERT_TRUE(ctx->preserve_whitespace);
    ASSERT_TRUE(ctx->generate_comments);
    ASSERT_EQ(ctx->recursion_depth, 0);
    ASSERT_EQ(ctx->param_count, 0);
    
    destroy_template_context(ctx);
    PASS();
}

// Test template parameter management
void test_template_parameters() {
    TEST("template parameter management");
    
    TemplateContext* ctx = create_template_context("test template");
    ASSERT_NOT_NULL(ctx);
    
    // Add parameters
    ComptimeValue* name_val = create_comptime_string("test_func");
    ComptimeValue* type_val = create_comptime_string("int");
    ComptimeValue* default_val = create_comptime_string("42");
    
    ASSERT_TRUE(add_template_parameter(ctx, "name", TEMPLATE_PARAM_VALUE, name_val));
    ASSERT_TRUE(add_template_parameter(ctx, "return_type", TEMPLATE_PARAM_TYPE, type_val));
    ASSERT_TRUE(add_template_parameter(ctx, "default_value", TEMPLATE_PARAM_VALUE, default_val));
    
    ASSERT_EQ(ctx->param_count, 3);
    
    // Find parameters
    TemplateParameter* param = find_template_parameter(ctx, "name");
    ASSERT_NOT_NULL(param);
    ASSERT_STR_EQ(param->name, "name");
    ASSERT_EQ(param->type, TEMPLATE_PARAM_VALUE);
    
    param = find_template_parameter(ctx, "nonexistent");
    ASSERT_NULL(param);
    
    destroy_template_context(ctx);
    PASS();
}

// Test string transformation utilities
void test_string_transformations() {
    TEST("string transformation utilities");
    
    // Test lowercase
    char* result = to_lowercase("HELLO World");
    ASSERT_STR_EQ(result, "hello world");
    free(result);
    
    // Test uppercase
    result = to_uppercase("hello World");
    ASSERT_STR_EQ(result, "HELLO WORLD");
    free(result);
    
    // Test capitalize
    result = to_capitalize("hello WORLD");
    ASSERT_STR_EQ(result, "Hello world");
    free(result);
    
    // Test snake_case
    result = to_snake_case("HelloWorld");
    ASSERT_STR_EQ(result, "hello_world");
    free(result);
    
    result = to_snake_case("XMLHttpRequest");
    ASSERT_CONTAINS(result, "_");
    free(result);
    
    // Test camel_case
    result = to_camel_case("hello_world");
    ASSERT_STR_EQ(result, "helloWorld");
    free(result);
    
    result = to_camel_case("test-case-example");
    ASSERT_STR_EQ(result, "testCaseExample");
    free(result);
    
    // Test pascal_case
    result = to_pascal_case("hello_world");
    ASSERT_STR_EQ(result, "HelloWorld");
    free(result);
    
    // Test kebab_case
    result = to_kebab_case("HelloWorld");
    ASSERT_CONTAINS(result, "-");
    free(result);
    
    // Test plural
    result = to_plural("user");
    ASSERT_STR_EQ(result, "users");
    free(result);
    
    result = to_plural("box");
    ASSERT_STR_EQ(result, "boxes");
    free(result);
    
    result = to_plural("baby");
    ASSERT_STR_EQ(result, "babies");
    free(result);
    
    // Test singular
    result = to_singular("users");
    ASSERT_STR_EQ(result, "user");
    free(result);
    
    result = to_singular("boxes");
    ASSERT_STR_EQ(result, "box");
    free(result);
    
    // Test escape_string
    result = escape_string("Hello \"World\"\nNew line");
    ASSERT_CONTAINS(result, "\\\"");
    ASSERT_CONTAINS(result, "\\n");
    free(result);
    
    // Test quote_string
    result = quote_string("Hello World");
    ASSERT_STR_EQ(result, "\"Hello World\"");
    free(result);
    
    PASS();
}

// Test template filter parsing
void test_filter_parsing() {
    TEST("template filter parsing");
    
    ASSERT_EQ(parse_filter_name("lowercase"), FILTER_LOWERCASE);
    ASSERT_EQ(parse_filter_name("uppercase"), FILTER_UPPERCASE);
    ASSERT_EQ(parse_filter_name("snake_case"), FILTER_SNAKE_CASE);
    ASSERT_EQ(parse_filter_name("camel_case"), FILTER_CAMEL_CASE);
    ASSERT_EQ(parse_filter_name("plural"), FILTER_PLURAL);
    ASSERT_EQ(parse_filter_name("singular"), FILTER_SINGULAR);
    ASSERT_EQ(parse_filter_name("invalid_filter"), FILTER_COUNT);
    
    // Test apply_template_filter
    char* result = apply_template_filter("HelloWorld", FILTER_LOWERCASE);
    ASSERT_STR_EQ(result, "helloworld");
    free(result);
    
    result = apply_template_filter("user", FILTER_PLURAL);
    ASSERT_STR_EQ(result, "users");
    free(result);
    
    PASS();
}

// Test simple template processing
void test_simple_template_processing() {
    TEST("simple template processing");
    
    TemplateContext* ctx = create_template_context("Hello {{name}}, welcome to {{place}}!");
    ASSERT_NOT_NULL(ctx);
    
    // Add parameters
    ComptimeValue* name_val = create_comptime_string("Alice");
    ComptimeValue* place_val = create_comptime_string("Wonderland");
    
    add_template_parameter(ctx, "name", TEMPLATE_PARAM_VALUE, name_val);
    add_template_parameter(ctx, "place", TEMPLATE_PARAM_VALUE, place_val);
    
    // Process template
    char* result = process_template_string(ctx->template_code, ctx);
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result, "Hello Alice, welcome to Wonderland!");
    
    free(result);
    destroy_template_context(ctx);
    PASS();
}

// Test template processing with filters
void test_template_with_filters() {
    TEST("template processing with filters");
    
    TemplateContext* ctx = create_template_context("func {{name | lowercase}}() {{return_type | uppercase}} {");
    ASSERT_NOT_NULL(ctx);
    
    // Add parameters
    ComptimeValue* name_val = create_comptime_string("MyFunction");
    ComptimeValue* type_val = create_comptime_string("string");
    
    add_template_parameter(ctx, "name", TEMPLATE_PARAM_VALUE, name_val);
    add_template_parameter(ctx, "return_type", TEMPLATE_PARAM_VALUE, type_val);
    
    // Process template
    char* result = process_template_string(ctx->template_code, ctx);
    ASSERT_NOT_NULL(result);
    ASSERT_CONTAINS(result, "myfunction");
    ASSERT_CONTAINS(result, "STRING");
    
    free(result);
    destroy_template_context(ctx);
    PASS();
}

// Test template expansion
void test_template_expansion() {
    TEST("template expansion");
    
    TemplateContext* ctx = create_template_context("func {{func_name}}() { return {{value}}; }");
    ASSERT_NOT_NULL(ctx);
    
    // Add parameters
    ComptimeValue* name_val = create_comptime_string("test_function");
    ComptimeValue* value_val = create_comptime_string("42");
    
    add_template_parameter(ctx, "func_name", TEMPLATE_PARAM_VALUE, name_val);
    add_template_parameter(ctx, "value", TEMPLATE_PARAM_VALUE, value_val);
    
    // Expand template
    TemplateExpansionResult* result = expand_template(ctx);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    ASSERT_NOT_NULL(result->code);
    
    // Check generated code
    ASSERT_CONTAINS(result->code, "func test_function()");
    ASSERT_CONTAINS(result->code, "return 42;");
    ASSERT_CONTAINS(result->code, "Auto-generated code");
    
    destroy_template_expansion_result(result);
    destroy_template_context(ctx);
    PASS();
}

// Test CRUD template generation
void test_crud_template() {
    TEST("CRUD template generation");
    
    TemplateContext* ctx = create_template_context("placeholder");
    ASSERT_NOT_NULL(ctx);
    
    // Add parameters for CRUD template
    ComptimeValue* type_val = create_comptime_string("User");
    ComptimeValue* id_type_val = create_comptime_string("int64");
    
    add_template_parameter(ctx, "type_name", TEMPLATE_PARAM_VALUE, type_val);
    add_template_parameter(ctx, "id_type", TEMPLATE_PARAM_VALUE, id_type_val);
    
    // Generate CRUD template
    TemplateExpansionResult* result = generate_crud_template(ctx);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    ASSERT_NOT_NULL(result->code);
    
    // Check generated CRUD operations
    ASSERT_CONTAINS(result->code, "create_user");
    ASSERT_CONTAINS(result->code, "get_user");
    ASSERT_CONTAINS(result->code, "update_user");
    ASSERT_CONTAINS(result->code, "delete_user");
    ASSERT_CONTAINS(result->code, "list_users");
    ASSERT_CONTAINS(result->code, "database.insert");
    ASSERT_CONTAINS(result->code, "database.find_by_id");
    
    destroy_template_expansion_result(result);
    destroy_template_context(ctx);
    PASS();
}

// Test API client template generation
void test_api_client_template() {
    TEST("API client template generation");
    
    TemplateContext* ctx = create_template_context("placeholder");
    ASSERT_NOT_NULL(ctx);
    
    // Add parameters for API client template
    ComptimeValue* service_val = create_comptime_string("GitHub");
    
    add_template_parameter(ctx, "service_name", TEMPLATE_PARAM_VALUE, service_val);
    
    // Generate API client template
    TemplateExpansionResult* result = generate_api_client_template(ctx);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    ASSERT_NOT_NULL(result->code);
    
    // Check generated API client
    ASSERT_CONTAINS(result->code, "GitHubClient");
    ASSERT_CONTAINS(result->code, "new_github_client");
    ASSERT_CONTAINS(result->code, "base_url");
    ASSERT_CONTAINS(result->code, "api_key");
    ASSERT_CONTAINS(result->code, "Authorization");
    ASSERT_CONTAINS(result->code, "http.request");
    
    destroy_template_expansion_result(result);
    destroy_template_context(ctx);
    PASS();
}

// Test template macro evaluator
void test_template_macro_evaluator() {
    TEST("template macro evaluator");
    
    // Create mock macro context
    MacroTemplate* mock_macro = create_macro_template("template_test", MACRO_TEMPLATE);
    MacroContext* ctx = create_macro_context(mock_macro, NULL, 0);
    
    // Create arguments
    ComptimeValue* template_code = create_comptime_string("Hello {{param1}}!");
    ComptimeValue* param_value = create_comptime_string("World");
    ComptimeValue* args[] = { template_code, param_value };
    
    ctx->arguments = args;
    ctx->arg_count = 2;
    
    // Evaluate template macro
    ComptimeValue* result = template_macro_evaluator(ctx, args);
    ASSERT_NOT_NULL(result);
    ASSERT_EQ(result->type, COMPTIME_VALUE_STRING);
    ASSERT_CONTAINS(result->string_value, "Hello World!");
    
    destroy_macro_context(ctx);
    destroy_macro_template(mock_macro);
    PASS();
}

// Test error handling
void test_error_handling() {
    TEST("error handling");
    
    // Test with invalid inputs
    TemplateContext* ctx = create_template_context(NULL);
    ASSERT_NULL(ctx);
    
    TemplateExpansionResult* result = expand_template(NULL);
    ASSERT_NULL(result);
    
    // Test filter parsing errors
    ASSERT_EQ(parse_filter_name(NULL), FILTER_COUNT);
    ASSERT_EQ(parse_filter_name("invalid"), FILTER_COUNT);
    
    PASS();
}

// Test debug and introspection functions
void test_debug_functions() {
    TEST("debug and introspection functions");
    
    TemplateContext* ctx = create_template_context("test template {{param}}");
    ASSERT_NOT_NULL(ctx);
    
    ComptimeValue* param_val = create_comptime_string("value");
    add_template_parameter(ctx, "param", TEMPLATE_PARAM_VALUE, param_val);
    
    // Test debug functions (should not crash)
    print_template_context(ctx);
    print_template_parameters(ctx);
    
    char* info = get_template_info(ctx);
    ASSERT_NOT_NULL(info);
    ASSERT_CONTAINS(info, "Parameters: 1");
    free(info);
    
    destroy_template_context(ctx);
    PASS();
}

// Test edge cases and complex templates
void test_edge_cases() {
    TEST("edge cases and complex templates");
    
    // Test empty template
    TemplateContext* ctx = create_template_context("");
    ASSERT_NOT_NULL(ctx);
    
    TemplateExpansionResult* result = expand_template(ctx);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    
    destroy_template_expansion_result(result);
    destroy_template_context(ctx);
    
    // Test template with no parameters
    ctx = create_template_context("static content only");
    ASSERT_NOT_NULL(ctx);
    
    result = expand_template(ctx);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    ASSERT_CONTAINS(result->code, "static content only");
    
    destroy_template_expansion_result(result);
    destroy_template_context(ctx);
    
    // Test template with missing parameters
    ctx = create_template_context("Hello {{missing_param}}!");
    ASSERT_NOT_NULL(ctx);
    
    result = expand_template(ctx);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result->success);
    ASSERT_CONTAINS(result->code, "{{missing_param}}"); // Should keep original
    
    destroy_template_expansion_result(result);
    destroy_template_context(ctx);
    
    PASS();
}

// Main test runner
int main() {
    printf("=== Template Macro System Tests ===\n\n");
    
    // Run tests
    test_template_context_creation();
    test_template_parameters();
    test_string_transformations();
    test_filter_parsing();
    test_simple_template_processing();
    test_template_with_filters();
    test_template_expansion();
    test_crud_template();
    test_api_client_template();
    test_template_macro_evaluator();
    test_error_handling();
    test_debug_functions();
    test_edge_cases();
    
    // Print summary
    printf("\n=== Test Summary ===\n");
    printf("Total tests: %d\n", test_count);
    printf("Passed: %d\n", pass_count);
    printf("Failed: %d\n", fail_count);
    
    return fail_count > 0 ? 1 : 0;
}