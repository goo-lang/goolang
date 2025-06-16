#include "../include/advanced_macro_system.h"
#include "../include/comptime.h"
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

// Test macro registry creation and destruction
void test_macro_registry() {
    TEST("macro registry creation and destruction");
    
    MacroRegistry* registry = create_macro_registry();
    ASSERT_NOT_NULL(registry);
    ASSERT_TRUE(registry->enable_hygiene);
    ASSERT_FALSE(registry->debug_expansions);
    ASSERT_EQ(registry->max_expansion_depth, 100);
    ASSERT_EQ(registry->macro_count, 0);
    
    destroy_macro_registry(registry);
    PASS();
}

// Test macro template creation
void test_macro_template_creation() {
    TEST("macro template creation");
    
    MacroTemplate* macro = create_macro_template("test_macro", MACRO_FUNCTION);
    ASSERT_NOT_NULL(macro);
    ASSERT_STR_EQ(macro->name, "test_macro");
    ASSERT_EQ(macro->type, MACRO_FUNCTION);
    ASSERT_EQ(macro->hygiene, HYGIENE_SEMANTIC);
    ASSERT_EQ(macro->max_recursion, 10);
    ASSERT_TRUE(macro->generate_debug_info);
    ASSERT_EQ(macro->param_count, 0);
    
    destroy_macro_template(macro);
    PASS();
}

// Test macro parameter management
void test_macro_parameters() {
    TEST("macro parameter management");
    
    MacroTemplate* macro = create_macro_template("param_test", MACRO_FUNCTION);
    ASSERT_NOT_NULL(macro);
    
    // Add parameters
    ASSERT_TRUE(add_macro_parameter(macro, "expr", MACRO_PARAM_EXPR));
    ASSERT_TRUE(add_macro_parameter(macro, "type", MACRO_PARAM_TYPE));
    ASSERT_TRUE(add_macro_parameter(macro, "ident", MACRO_PARAM_IDENT));
    
    ASSERT_EQ(macro->param_count, 3);
    ASSERT_STR_EQ(macro->parameters[0].name, "expr");
    ASSERT_EQ(macro->parameters[0].type, MACRO_PARAM_EXPR);
    ASSERT_FALSE(macro->parameters[0].is_optional);
    
    // Set constraints
    ASSERT_TRUE(set_parameter_constraint(macro, "type", "numeric"));
    ASSERT_STR_EQ(macro->parameters[1].constraint, "numeric");
    
    // Set default value
    ComptimeValue* default_val = create_comptime_string("default");
    ASSERT_TRUE(set_parameter_default(macro, "ident", default_val));
    ASSERT_TRUE(macro->parameters[2].is_optional);
    ASSERT_EQ(macro->parameters[2].default_value, default_val);
    
    destroy_macro_template(macro);
    PASS();
}

// Test macro registration
void test_macro_registration() {
    TEST("macro registration");
    
    MacroRegistry* registry = create_macro_registry();
    ASSERT_NOT_NULL(registry);
    
    MacroTemplate* macro1 = create_macro_template("macro1", MACRO_FUNCTION);
    MacroTemplate* macro2 = create_macro_template("macro2", MACRO_TEMPLATE);
    ASSERT_NOT_NULL(macro1);
    ASSERT_NOT_NULL(macro2);
    
    // Register macros
    ASSERT_TRUE(register_macro(registry, macro1));
    ASSERT_TRUE(register_macro(registry, macro2));
    ASSERT_EQ(registry->macro_count, 2);
    
    // Try to register duplicate
    MacroTemplate* duplicate = create_macro_template("macro1", MACRO_FUNCTION);
    ASSERT_FALSE(register_macro(registry, duplicate));
    destroy_macro_template(duplicate);
    
    // Find macros
    MacroTemplate* found1 = find_macro(registry, "macro1");
    MacroTemplate* found2 = find_macro(registry, "macro2");
    MacroTemplate* not_found = find_macro(registry, "nonexistent");
    
    ASSERT_EQ(found1, macro1);
    ASSERT_EQ(found2, macro2);
    ASSERT_NULL(not_found);
    
    destroy_macro_registry(registry);
    PASS();
}

// Test built-in macros
void test_builtin_macros() {
    TEST("built-in macros");
    
    MacroRegistry* registry = create_macro_registry();
    ASSERT_NOT_NULL(registry);
    
    // Check built-in macros are registered
    MacroTemplate* assert_macro = find_macro(registry, "assert!");
    MacroTemplate* debug_print = find_macro(registry, "debug_print!");
    MacroTemplate* typeof_macro = find_macro(registry, "typeof!");
    MacroTemplate* stringify = find_macro(registry, "stringify!");
    
    ASSERT_NOT_NULL(assert_macro);
    ASSERT_NOT_NULL(debug_print);
    ASSERT_NOT_NULL(typeof_macro);
    ASSERT_NOT_NULL(stringify);
    
    // Check assert! macro parameters
    ASSERT_EQ(assert_macro->param_count, 2);
    ASSERT_STR_EQ(assert_macro->parameters[0].name, "condition");
    ASSERT_EQ(assert_macro->parameters[0].type, MACRO_PARAM_EXPR);
    ASSERT_STR_EQ(assert_macro->parameters[1].name, "message");
    ASSERT_EQ(assert_macro->parameters[1].type, MACRO_PARAM_LITERAL);
    ASSERT_TRUE(assert_macro->parameters[1].is_optional);
    
    destroy_macro_registry(registry);
    PASS();
}

// Test macro context creation
void test_macro_context() {
    TEST("macro context creation");
    
    MacroTemplate* macro = create_macro_template("context_test", MACRO_FUNCTION);
    ASSERT_NOT_NULL(macro);
    
    ComptimeValue* arg1 = create_comptime_string("hello");
    ComptimeValue* arg2 = create_comptime_string("world");
    ComptimeValue* args[] = { arg1, arg2 };
    
    MacroContext* context = create_macro_context(macro, args, 2);
    ASSERT_NOT_NULL(context);
    ASSERT_EQ(context->macro, macro);
    ASSERT_EQ(context->arguments, args);
    ASSERT_EQ(context->arg_count, 2);
    ASSERT_FALSE(context->has_error);
    ASSERT_EQ(context->recursion_depth, 0);
    
    destroy_macro_context(context);
    destroy_macro_template(macro);
    PASS();
}

// Test template processing
void test_template_processing() {
    TEST("template processing");
    
    MacroTemplate* macro = create_macro_template("template_test", MACRO_TEMPLATE);
    ASSERT_NOT_NULL(macro);
    
    add_macro_parameter(macro, "name", MACRO_PARAM_IDENT);
    add_macro_parameter(macro, "type", MACRO_PARAM_TYPE);
    
    ComptimeValue* name_arg = create_comptime_string("foo");
    ComptimeValue* type_arg = create_comptime_string("int");
    ComptimeValue* args[] = { name_arg, type_arg };
    
    MacroContext* context = create_macro_context(macro, args, 2);
    ASSERT_NOT_NULL(context);
    
    const char* template_str = "func {{name}}() {{type}} { return 42; }";
    char* result = process_template(template_str, context);
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result, "func foo() int { return 42; }");
    
    free(result);
    destroy_macro_context(context);
    destroy_macro_template(macro);
    PASS();
}

// Test hygiene system
void test_hygiene_system() {
    TEST("hygiene system");
    
    MacroTemplate* macro = create_macro_template("hygiene_test", MACRO_FUNCTION);
    ASSERT_NOT_NULL(macro);
    macro->hygiene = HYGIENE_SEMANTIC;
    
    MacroContext* context = create_macro_context(macro, NULL, 0);
    ASSERT_NOT_NULL(context);
    
    // Test hygiene name generation
    char* hygiene_name = generate_hygiene_name(context, "var");
    ASSERT_NOT_NULL(hygiene_name);
    ASSERT_TRUE(strstr(hygiene_name, "var__macro_") != NULL);
    
    // Test hygiene conflict detection
    context->hygiene_scope = (char**)malloc(sizeof(char*));
    context->hygiene_scope[0] = strdup("existing_var");
    context->scope_depth = 1;
    
    ASSERT_TRUE(check_hygiene_conflicts(context, "existing_var"));
    ASSERT_FALSE(check_hygiene_conflicts(context, "new_var"));
    
    free(hygiene_name);
    // hygiene_scope is freed by destroy_macro_context
    destroy_macro_context(context);
    destroy_macro_template(macro);
    PASS();
}

// Test macro expansion validation
void test_macro_validation() {
    TEST("macro argument validation");
    
    MacroTemplate* macro = create_macro_template("validation_test", MACRO_FUNCTION);
    ASSERT_NOT_NULL(macro);
    
    add_macro_parameter(macro, "required", MACRO_PARAM_EXPR);
    add_macro_parameter(macro, "optional", MACRO_PARAM_LITERAL);
    set_parameter_default(macro, "optional", create_comptime_string("default"));
    
    ComptimeValue* arg = create_comptime_string("test");
    ComptimeValue* args1[] = { arg };
    ComptimeValue* args2[] = { arg, arg };
    ComptimeValue* args3[] = { arg, arg, arg };
    
    // Valid cases
    ASSERT_TRUE(validate_macro_arguments(macro, args1, 1)); // Required only
    ASSERT_TRUE(validate_macro_arguments(macro, args2, 2)); // Both parameters
    
    // Invalid cases
    ASSERT_FALSE(validate_macro_arguments(macro, NULL, 0));  // Missing required
    ASSERT_FALSE(validate_macro_arguments(macro, args3, 3)); // Too many args
    
    destroy_macro_template(macro);
    PASS();
}

// Test macro error handling
void test_macro_errors() {
    TEST("macro error handling");
    
    MacroTemplate* macro = create_macro_template("error_test", MACRO_FUNCTION);
    ASSERT_NOT_NULL(macro);
    
    MacroContext* context = create_macro_context(macro, NULL, 0);
    ASSERT_NOT_NULL(context);
    
    ASSERT_FALSE(context->has_error);
    ASSERT_NULL(context->error_message);
    
    macro_error(context, "Test error: %s", "something went wrong");
    
    ASSERT_TRUE(context->has_error);
    ASSERT_NOT_NULL(context->error_message);
    ASSERT_TRUE(strstr(context->error_message, "Test error: something went wrong") != NULL);
    
    destroy_macro_context(context);
    destroy_macro_template(macro);
    PASS();
}

// Test macro expansion result
void test_macro_expansion() {
    TEST("macro expansion");
    
    MacroRegistry* registry = create_macro_registry();
    ASSERT_NOT_NULL(registry);
    
    // Create a simple macro
    MacroTemplate* macro = create_macro_template("simple_macro", MACRO_TEMPLATE);
    ASSERT_NOT_NULL(macro);
    macro->code_template = strdup("generated_code");
    register_macro(registry, macro);
    
    // Expand macro
    MacroExpansion* expansion = expand_macro(registry, "simple_macro", NULL, 0, NULL);
    ASSERT_NOT_NULL(expansion);
    ASSERT_TRUE(expansion->success);
    ASSERT_NOT_NULL(expansion->expanded_code);
    ASSERT_STR_EQ(expansion->expanded_code, "generated_code");
    
    // Test non-existent macro
    MacroExpansion* fail_expansion = expand_macro(registry, "nonexistent", NULL, 0, NULL);
    ASSERT_NOT_NULL(fail_expansion);
    ASSERT_FALSE(fail_expansion->success);
    ASSERT_NOT_NULL(fail_expansion->error_message);
    
    free(expansion->expanded_code);
    free(expansion);
    free(fail_expansion->error_message);
    free(fail_expansion);
    destroy_macro_registry(registry);
    PASS();
}

// Test introspection functions
void test_introspection() {
    TEST("macro introspection");
    
    MacroRegistry* registry = create_macro_registry();
    ASSERT_NOT_NULL(registry);
    
    MacroTemplate* macro = create_macro_template("intro_test", MACRO_FUNCTION);
    ASSERT_NOT_NULL(macro);
    add_macro_parameter(macro, "param1", MACRO_PARAM_EXPR);
    register_macro(registry, macro);
    
    // Test get_macro_info
    char* info = get_macro_info(macro);
    ASSERT_NOT_NULL(info);
    ASSERT_TRUE(strstr(info, "intro_test") != NULL);
    ASSERT_TRUE(strstr(info, "Parameters: 1") != NULL);
    
    // Test print functions (just ensure they don't crash)
    print_macro_registry(registry);
    
    free(info);
    destroy_macro_registry(registry);
    PASS();
}

// Main test runner
int main() {
    printf("=== Advanced Macro System Tests ===\n\n");
    
    // No error system initialization needed
    
    // Run tests
    test_macro_registry();
    test_macro_template_creation();
    test_macro_parameters();
    test_macro_registration();
    test_builtin_macros();
    test_macro_context();
    test_template_processing();
    test_hygiene_system();
    test_macro_validation();
    test_macro_errors();
    test_macro_expansion();
    test_introspection();
    
    // Print summary
    printf("\n=== Test Summary ===\n");
    printf("Total tests: %d\n", test_count);
    printf("Passed: %d\n", pass_count);
    printf("Failed: %d\n", fail_count);
    
    // No error system cleanup needed
    
    return fail_count > 0 ? 1 : 0;
}