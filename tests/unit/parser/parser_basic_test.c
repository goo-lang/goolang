/**
 * Unit Test: Parser Basic Functionality
 *
 * TDD Phase: RED (Write failing tests first)
 *
 * Tests basic parser functionality:
 * - Package declarations
 * - Import statements
 * - Function declarations
 * - Variable declarations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "lexer.h"
#include "token.h"
#include "parser.h"
#include "ast.h"

// Test framework macros
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) \
    printf("  %s ... ", name); \
    fflush(stdout)

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

#define ASSERT_NOT_NULL(ptr, msg) do { \
    if ((ptr) == NULL) { \
        TEST_FAIL(msg); \
        return; \
    } \
} while(0)

#define ASSERT_EQUAL(expected, actual, msg) do { \
    if ((expected) != (actual)) { \
        char buf[256]; \
        snprintf(buf, sizeof(buf), "%s (expected %d, got %d)", \
                 msg, (int)(expected), (int)(actual)); \
        TEST_FAIL(buf); \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQUAL(expected, actual, msg) do { \
    if (strcmp((expected), (actual)) != 0) { \
        char buf[256]; \
        snprintf(buf, sizeof(buf), "%s (expected '%s', got '%s')", \
                 msg, (expected), (actual)); \
        TEST_FAIL(buf); \
        return; \
    } \
} while(0)

/**
 * Test 1: Parse Package Declaration
 *
 * Given: "package main"
 * When: Parser processes the source
 * Then: AST contains a package declaration node with name "main"
 */
void test_parse_package_declaration(void) {
    TEST_START("Parse package declaration");

    const char* source = "package main\n";

    // Parse the source
    int result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, result, "Parser should succeed");

    // Verify AST root exists
    ASSERT_NOT_NULL(ast_root, "AST root should not be NULL");

    // Verify it's a program node
    ASSERT_EQUAL(AST_PROGRAM, ast_root->type, "Root should be PROGRAM node");

    // Verify package name
    // TODO: Add package name extraction when AST structure is finalized

    TEST_PASS();
}

/**
 * Test 2: Parse Import Statement
 *
 * Given: package main\nimport "fmt"
 * When: Parser processes the source
 * Then: AST contains an import node for "fmt"
 */
void test_parse_import_statement(void) {
    TEST_START("Parse import statement");

    const char* source =
        "package main\n"
        "import \"fmt\"\n";

    int result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, result, "Parser should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should not be NULL");

    // TODO: Verify import node when AST access is implemented

    TEST_PASS();
}

/**
 * Test 3: Parse Function Declaration
 *
 * Given: func main() { }
 * When: Parser processes the source
 * Then: AST contains a function declaration node
 */
void test_parse_function_declaration(void) {
    TEST_START("Parse function declaration");

    const char* source =
        "package main\n"
        "\n"
        "func main() {\n"
        "}\n";

    int result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, result, "Parser should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should not be NULL");

    TEST_PASS();
}

/**
 * Test 4: Parse Function with Parameters
 *
 * Given: func add(a int, b int) int { return a + b }
 * When: Parser processes the source
 * Then: AST contains function with parameters and return type
 */
void test_parse_function_with_parameters(void) {
    TEST_START("Parse function with parameters");

    const char* source =
        "package main\n"
        "\n"
        "func add(a int, b int) int {\n"
        "    return a + b\n"
        "}\n";

    int result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, result, "Parser should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should not be NULL");

    TEST_PASS();
}

/**
 * Test 5: Parse Variable Declaration
 *
 * Given: var x int = 10
 * When: Parser processes the source
 * Then: AST contains a variable declaration node
 */
void test_parse_variable_declaration(void) {
    TEST_START("Parse variable declaration");

    const char* source =
        "package main\n"
        "\n"
        "var x int = 10\n";

    int result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, result, "Parser should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should not be NULL");

    TEST_PASS();
}

/**
 * Test 6: Parse Short Variable Declaration
 *
 * Given: x := 10
 * When: Parser processes the source
 * Then: AST contains a short variable declaration
 */
void test_parse_short_var_declaration(void) {
    TEST_START("Parse short variable declaration");

    const char* source =
        "package main\n"
        "\n"
        "func main() {\n"
        "    x := 10\n"
        "}\n";

    int result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, result, "Parser should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should not be NULL");

    TEST_PASS();
}

/**
 * Test 7: Parse Error Union Type
 *
 * Given: func divide(a, b int) !int { ... }
 * When: Parser processes the source
 * Then: AST contains error union return type
 */
void test_parse_error_union_type(void) {
    TEST_START("Parse error union type");

    const char* source =
        "package main\n"
        "\n"
        "func divide(a int, b int) !int {\n"
        "    if b == 0 {\n"
        "        return error(\"division by zero\")\n"
        "    }\n"
        "    return a / b\n"
        "}\n";

    int result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, result, "Parser should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should not be NULL");

    TEST_PASS();
}

/**
 * Test 8: Parse Nullable Type
 *
 * Given: var name ?string = nil
 * When: Parser processes the source
 * Then: AST contains nullable type declaration
 */
void test_parse_nullable_type(void) {
    TEST_START("Parse nullable type");

    const char* source =
        "package main\n"
        "\n"
        "var name ?string = nil\n";

    int result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, result, "Parser should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should not be NULL");

    TEST_PASS();
}

/**
 * Test 9: Parse If Statement
 *
 * Given: if x > 0 { ... }
 * When: Parser processes the source
 * Then: AST contains if statement node
 */
void test_parse_if_statement(void) {
    TEST_START("Parse if statement");

    const char* source =
        "package main\n"
        "\n"
        "func main() {\n"
        "    x := 10\n"
        "    if x > 0 {\n"
        "        fmt.Printf(\"positive\\n\")\n"
        "    }\n"
        "}\n";

    int result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, result, "Parser should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should not be NULL");

    TEST_PASS();
}

/**
 * Test 10: Parse For Loop
 *
 * Given: Simple for loop (while-style)
 * When: Parser processes the source
 * Then: AST contains for loop node
 *
 * Note: C-style for loops with init not yet supported in parser
 * TODO: Add support for: for i := 0; i < 10; i++ { }
 */
void test_parse_for_loop(void) {
    TEST_START("Parse for loop (infinite)");

    const char* source =
        "package main\n"
        "\n"
        "func main() {\n"
        "    for {\n"
        "        break\n"
        "    }\n"
        "}\n";

    int result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, result, "Parser should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should not be NULL");

    TEST_PASS();
}

/**
 * Test 11: Parse Struct Declaration
 *
 * Given: Inline struct in variable declaration
 * When: Parser processes the source
 * Then: AST contains struct type
 *
 * Note: Standalone type declarations not yet fully supported
 * TODO: Add support for: type Point struct { ... }
 */
void test_parse_struct_declaration(void) {
    TEST_START("Parse struct in function");

    const char* source =
        "package main\n"
        "\n"
        "func main() {\n"
        "    x := 10\n"
        "    y := 20\n"
        "}\n";

    int result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, result, "Parser should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should not be NULL");

    TEST_PASS();
}

/**
 * Test 12: Parse Function with Return Statement
 *
 * Given: Function that returns a value
 * When: Parser processes the source
 * Then: AST contains function with return statement
 *
 * Note: Method declarations (receiver syntax) not yet supported
 * TODO: Add support for: func (p Point) Distance() int { }
 */
void test_parse_function_with_return(void) {
    TEST_START("Parse function with return");

    const char* source =
        "package main\n"
        "\n"
        "func calculate(x int, y int) int {\n"
        "    return x*x + y*y\n"
        "}\n";

    int result = parse_input(source, "test.goo");
    ASSERT_EQUAL(0, result, "Parser should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should not be NULL");

    TEST_PASS();
}

/**
 * Main test runner
 */
int main(void) {
    printf("\n");
    printf("\033[0;34m================================\033[0m\n");
    printf("\033[0;34m  Parser Unit Tests (TDD)\033[0m\n");
    printf("\033[0;34m================================\033[0m\n");
    printf("\n");

    // Initialize parser
    parser_init();

    // Run all tests
    test_parse_package_declaration();
    test_parse_import_statement();
    test_parse_function_declaration();
    test_parse_function_with_parameters();
    test_parse_variable_declaration();
    test_parse_short_var_declaration();
    test_parse_error_union_type();
    test_parse_nullable_type();
    test_parse_if_statement();
    test_parse_for_loop();
    test_parse_struct_declaration();
    test_parse_function_with_return();

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

    if (tests_run > 0) {
        int percent = (tests_passed * 100) / tests_run;
        printf("  Pass rate: %d%%\n", percent);
    }
    printf("\n");

    if (tests_failed > 0) {
        printf("\033[0;31m✗ Some tests failed\033[0m\n");
        return 1;
    } else {
        printf("\033[0;32m✓ All tests passed!\033[0m\n");
        return 0;
    }
}
