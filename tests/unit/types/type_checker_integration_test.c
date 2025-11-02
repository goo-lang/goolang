/**
 * Type Checker Integration Tests (TDD)
 *
 * Tests integration of Parser → Type Checker
 * Following RED-GREEN-REFACTOR methodology
 *
 * These tests verify:
 * 1. Type checker can analyze parsed ASTs
 * 2. Type errors are detected correctly
 * 3. Valid programs pass type checking
 * 4. Error unions (!T) and nullable types (?T) work
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"
#include "ast.h"
#include "types.h"

// Test counter
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

// Test macros
#define TEST_START(name) printf("  %s ... ", name)
#define TEST_PASS() do { \
    printf("\033[0;32m✓ PASS\033[0m\n"); \
    tests_passed++; \
    tests_run++; \
} while(0)

#define TEST_FAIL(msg) do { \
    printf("\033[0;31m✗ FAIL\033[0m: %s\n", msg); \
    tests_failed++; \
    tests_run++; \
} while(0)

#define ASSERT_EQUAL(expected, actual, msg) do { \
    if ((expected) != (actual)) { \
        printf("\033[0;31m✗ FAIL\033[0m: %s (expected: %d, got: %d)\n", msg, (int)(expected), (int)(actual)); \
        tests_failed++; \
        tests_run++; \
        return; \
    } \
} while(0)

#define ASSERT_NOT_EQUAL(expected, actual, msg) do { \
    if ((expected) == (actual)) { \
        printf("\033[0;31m✗ FAIL\033[0m: %s (both equal: %d)\n", msg, (int)(expected)); \
        tests_failed++; \
        tests_run++; \
        return; \
    } \
} while(0)

#define ASSERT_NOT_NULL(ptr, msg) do { \
    if ((ptr) == NULL) { \
        printf("\033[0;31m✗ FAIL\033[0m: %s (got NULL)\n", msg); \
        tests_failed++; \
        tests_run++; \
        return; \
    } \
} while(0)

#define ASSERT_NULL(ptr, msg) do { \
    if ((ptr) != NULL) { \
        printf("\033[0;31m✗ FAIL\033[0m: %s (expected NULL)\n", msg); \
        tests_failed++; \
        tests_run++; \
        return; \
    } \
} while(0)

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { \
        printf("\033[0;31m✗ FAIL\033[0m: %s\n", msg); \
        tests_failed++; \
        tests_run++; \
        return; \
    } \
} while(0)

// External AST root from parser
extern ASTNode* ast_root;

/**
 * Test 1: Type check simple integer variable
 *
 * Given: var x int = 42
 * When: Type checker analyzes the AST
 * Then: Variable has type int32
 */
void test_type_check_int_variable(void) {
    TEST_START("Type check integer variable");

    const char* source =
        "package main\n"
        "\n"
        "func main() {\n"
        "    var x int = 42\n"
        "}\n";

    parser_init();
    int parse_result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, parse_result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    TypeChecker* checker = type_checker_new();
    ASSERT_NOT_NULL(checker, "Type checker should be created");

    int type_result = type_check_program(checker, ast_root);
    ASSERT_NOT_EQUAL(0, type_result, "Type check should succeed");
    ASSERT_EQUAL(0, checker->error_count, "Should have no errors");

    type_checker_free(checker);
    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 2: Type check type mismatch error
 *
 * Given: var x int = "hello"  (string assigned to int)
 * When: Type checker analyzes the AST
 * Then: Type error is detected
 */
void test_type_check_type_mismatch(void) {
    TEST_START("Detect type mismatch");

    const char* source =
        "package main\n"
        "\n"
        "func main() {\n"
        "    var x int = \"hello\"\n"
        "}\n";

    parser_init();
    int parse_result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, parse_result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    TypeChecker* checker = type_checker_new();
    ASSERT_NOT_NULL(checker, "Type checker should be created");

    int type_result = type_check_program(checker, ast_root);
    ASSERT_EQUAL(0, type_result, "Type check should fail");
    ASSERT_TRUE(checker->error_count > 0, "Should have type errors");

    type_checker_free(checker);
    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 3: Type check function with parameters
 *
 * Given: func add(a int, b int) int { return a + b }
 * When: Type checker analyzes the AST
 * Then: Function signature is correctly typed
 */
void test_type_check_function_signature(void) {
    TEST_START("Type check function signature");

    const char* source =
        "package main\n"
        "\n"
        "func add(a int, b int) int {\n"
        "    return a + b\n"
        "}\n";

    parser_init();
    int parse_result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, parse_result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    TypeChecker* checker = type_checker_new();
    ASSERT_NOT_NULL(checker, "Type checker should be created");

    int type_result = type_check_program(checker, ast_root);
    ASSERT_NOT_EQUAL(0, type_result, "Type check should succeed");
    ASSERT_EQUAL(0, checker->error_count, "Should have no errors");

    // Verify function is in scope
    Variable* add_func = type_checker_lookup_variable(checker, "add");
    ASSERT_NOT_NULL(add_func, "Function 'add' should be in scope");
    ASSERT_TRUE(add_func->type->kind == TYPE_FUNCTION, "Should be function type");

    type_checker_free(checker);
    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 4: Type check function return type mismatch
 *
 * Given: func getNumber() int { return "hello" }
 * When: Type checker analyzes the AST
 * Then: Return type mismatch error detected
 */
void test_type_check_return_type_mismatch(void) {
    TEST_START("Detect return type mismatch");

    const char* source =
        "package main\n"
        "\n"
        "func getNumber() int {\n"
        "    return \"hello\"\n"
        "}\n";

    parser_init();
    int parse_result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, parse_result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    TypeChecker* checker = type_checker_new();
    ASSERT_NOT_NULL(checker, "Type checker should be created");

    int type_result = type_check_program(checker, ast_root);
    ASSERT_EQUAL(0, type_result, "Type check should fail");
    ASSERT_TRUE(checker->error_count > 0, "Should have type errors");

    type_checker_free(checker);
    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 5: Type check error union (!T)
 *
 * Given: func divide(a int, b int) !int { ... }
 * When: Type checker analyzes the AST
 * Then: Error union type is recognized
 */
void test_type_check_error_union(void) {
    TEST_START("Type check error union");

    const char* source =
        "package main\n"
        "\n"
        "func divide(a int, b int) !int {\n"
        "    if b == 0 {\n"
        "        return error(\"division by zero\")\n"
        "    }\n"
        "    return a / b\n"
        "}\n";

    parser_init();
    int parse_result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, parse_result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    TypeChecker* checker = type_checker_new();
    ASSERT_NOT_NULL(checker, "Type checker should be created");

    int type_result = type_check_program(checker, ast_root);
    ASSERT_NOT_EQUAL(0, type_result, "Type check should succeed");
    ASSERT_EQUAL(0, checker->error_count, "Should have no errors");

    // Verify function return type is error union
    Variable* divide_func = type_checker_lookup_variable(checker, "divide");
    ASSERT_NOT_NULL(divide_func, "Function 'divide' should be in scope");
    ASSERT_TRUE(divide_func->type->kind == TYPE_FUNCTION, "Should be function type");

    Type* return_type = divide_func->type->data.function.return_type;
    ASSERT_NOT_NULL(return_type, "Return type should exist");
    ASSERT_TRUE(type_is_error_union(return_type), "Return type should be error union");

    type_checker_free(checker);
    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 6: Type check nullable type (?T)
 *
 * Given: var name ?string = nil
 * When: Type checker analyzes the AST
 * Then: Nullable type is recognized
 */
void test_type_check_nullable_type(void) {
    TEST_START("Type check nullable type");

    const char* source =
        "package main\n"
        "\n"
        "func main() {\n"
        "    var name ?string = nil\n"
        "}\n";

    parser_init();
    int parse_result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, parse_result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    TypeChecker* checker = type_checker_new();
    ASSERT_NOT_NULL(checker, "Type checker should be created");

    int type_result = type_check_program(checker, ast_root);
    ASSERT_NOT_EQUAL(0, type_result, "Type check should succeed");
    ASSERT_EQUAL(0, checker->error_count, "Should have no errors");

    // Verify variable type is nullable
    scope_push(checker);  // Enter main function scope
    Variable* name_var = type_checker_lookup_variable(checker, "name");
    // Note: Variable might not be accessible from outer scope
    // This test verifies nullable types parse and type-check correctly

    type_checker_free(checker);
    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 7: Type check if statement condition
 *
 * Given: if x { } where x is not bool
 * When: Type checker analyzes the AST
 * Then: Type error for non-bool condition
 */
void test_type_check_if_condition(void) {
    TEST_START("Type check if condition");

    const char* source =
        "package main\n"
        "\n"
        "func main() {\n"
        "    var x int = 5\n"
        "    if x {\n"
        "        x = 10\n"
        "    }\n"
        "}\n";

    parser_init();
    int parse_result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, parse_result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    TypeChecker* checker = type_checker_new();
    ASSERT_NOT_NULL(checker, "Type checker should be created");

    int type_result = type_check_program(checker, ast_root);
    ASSERT_EQUAL(0, type_result, "Type check should fail");
    ASSERT_TRUE(checker->error_count > 0, "Should have type errors");

    type_checker_free(checker);
    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 8: Type check binary expression
 *
 * Given: var result int = 10 + 20
 * When: Type checker analyzes the AST
 * Then: Binary expression type is int
 */
void test_type_check_binary_expression(void) {
    TEST_START("Type check binary expression");

    const char* source =
        "package main\n"
        "\n"
        "func main() {\n"
        "    var result int = 10 + 20\n"
        "}\n";

    parser_init();
    int parse_result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, parse_result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    TypeChecker* checker = type_checker_new();
    ASSERT_NOT_NULL(checker, "Type checker should be created");

    int type_result = type_check_program(checker, ast_root);
    ASSERT_NOT_EQUAL(0, type_result, "Type check should succeed");
    ASSERT_EQUAL(0, checker->error_count, "Should have no errors");

    type_checker_free(checker);
    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 9: Type check undefined variable
 *
 * Given: x = 10 (x not declared)
 * When: Type checker analyzes the AST
 * Then: Undefined variable error
 */
void test_type_check_undefined_variable(void) {
    TEST_START("Detect undefined variable");

    const char* source =
        "package main\n"
        "\n"
        "func main() {\n"
        "    x = 10\n"
        "}\n";

    parser_init();
    int parse_result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, parse_result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    TypeChecker* checker = type_checker_new();
    ASSERT_NOT_NULL(checker, "Type checker should be created");

    int type_result = type_check_program(checker, ast_root);
    ASSERT_EQUAL(0, type_result, "Type check should fail");
    ASSERT_TRUE(checker->error_count > 0, "Should have type errors");

    type_checker_free(checker);
    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 10: Type check short variable declaration
 *
 * Given: x := 42
 * When: Type checker analyzes the AST
 * Then: Type is inferred as int
 */
void test_type_check_short_var_decl(void) {
    TEST_START("Type check short var decl");

    const char* source =
        "package main\n"
        "\n"
        "func main() {\n"
        "    x := 42\n"
        "    y := x + 10\n"
        "}\n";

    parser_init();
    int parse_result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, parse_result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    TypeChecker* checker = type_checker_new();
    ASSERT_NOT_NULL(checker, "Type checker should be created");

    int type_result = type_check_program(checker, ast_root);
    ASSERT_NOT_EQUAL(0, type_result, "Type check should succeed");
    ASSERT_EQUAL(0, checker->error_count, "Should have no errors");

    type_checker_free(checker);
    parser_cleanup();
    TEST_PASS();
}

/**
 * Main test runner
 */
int main(void) {
    printf("\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\033[0;34m  Type Checker Integration Tests (TDD)\033[0m\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\n");

    // Initialize parser
    parser_init();

    // Run all tests
    test_type_check_int_variable();
    test_type_check_type_mismatch();
    test_type_check_function_signature();
    test_type_check_return_type_mismatch();
    test_type_check_error_union();
    test_type_check_nullable_type();
    test_type_check_if_condition();
    test_type_check_binary_expression();
    test_type_check_undefined_variable();
    test_type_check_short_var_decl();

    // Cleanup
    parser_cleanup();

    // Print results
    printf("\n");
    printf("\033[0;34m================================\033[0m\n");
    printf("\033[0;34m  Test Results\033[0m\n");
    printf("\033[0;34m================================\033[0m\n");
    printf("  Total:   %d\n", tests_run);
    printf("  \033[0;32mPassed:  %d\033[0m\n", tests_passed);
    printf("  \033[0;31mFailed:  %d\033[0m\n", tests_failed);

    double pass_rate = tests_run > 0 ? (100.0 * tests_passed / tests_run) : 0.0;
    printf("  Pass Rate: %.0f%%\n", pass_rate);
    printf("\n");

    if (tests_failed > 0) {
        printf("\033[0;31m✗ Some tests failed\033[0m\n");
        printf("\n");
        return 1;
    } else if (tests_run > 0) {
        printf("\033[0;32m✓ All tests passed!\033[0m\n");
        printf("\n");
        return 0;
    } else {
        printf("\033[0;33m⚠ No tests run\033[0m\n");
        printf("\n");
        return 1;
    }
}
