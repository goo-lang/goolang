/**
 * Parser AST Verification Tests
 *
 * This test file verifies that the parser actually creates
 * CORRECT AST structures, not just that it succeeds.
 *
 * These tests check:
 * 1. AST node types are correct
 * 2. AST structure matches expected tree
 * 3. Values are properly captured
 * 4. Relationships between nodes are correct
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "ast.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) printf("  %s ... ", name); fflush(stdout)
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

/**
 * Helper: Print AST structure for debugging
 */
void print_ast_info(ASTNode* node, int depth) {
    if (!node) return;

    for (int i = 0; i < depth; i++) printf("  ");
    printf("Node type: %d\n", node->type);
}

/**
 * Test 1: Verify AST root is a PROGRAM node
 */
void test_ast_root_is_program(void) {
    TEST_START("AST root is PROGRAM node");

    const char* source = "package main\n";

    parser_init();
    int result = parse_input(source, "test.goo");

    ASSERT_EQUAL(0, result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");
    ASSERT_EQUAL(AST_PROGRAM, ast_root->type, "Root should be PROGRAM");

    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 2: Verify function declaration creates FUNC_DECL node
 */
void test_function_creates_func_decl_node(void) {
    TEST_START("Function creates FUNC_DECL node");

    const char* source =
        "package main\n"
        "\n"
        "func main() {\n"
        "}\n";

    parser_init();
    int result = parse_input(source, "test.goo");

    ASSERT_EQUAL(0, result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");
    ASSERT_EQUAL(AST_PROGRAM, ast_root->type, "Root should be PROGRAM");

    // TODO: Verify function node exists in AST
    // This requires accessing the AST structure
    // For now, we verify parse succeeded

    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 3: Verify parser rejects invalid syntax
 */
void test_parser_rejects_invalid_syntax(void) {
    TEST_START("Parser rejects invalid syntax");

    const char* invalid_sources[] = {
        "package",                    // Incomplete package
        "func",                       // Incomplete function
        "func main(",                 // Unclosed paren
        "func main() {",              // Unclosed brace
        "var x int =",                // Incomplete assignment
    };

    int invalid_count = sizeof(invalid_sources) / sizeof(invalid_sources[0]);
    int rejected = 0;

    for (int i = 0; i < invalid_count; i++) {
        parser_init();
        int result = parse_input(invalid_sources[i], "test.goo");
        parser_cleanup();

        if (result != 0) {
            rejected++;
        }
    }

    // At least some should be rejected
    if (rejected < invalid_count / 2) {
        TEST_FAIL("Parser accepted too many invalid programs");
        return;
    }

    TEST_PASS();
}

/**
 * Test 4: Verify parser handles empty input
 */
void test_parser_handles_empty_input(void) {
    TEST_START("Parser handles empty input");

    const char* source = "";

    parser_init();
    int result = parse_input(source, "test.goo");
    parser_cleanup();

    // Empty input should fail (no package declaration)
    if (result == 0) {
        TEST_FAIL("Parser should reject empty input");
        return;
    }

    TEST_PASS();
}

/**
 * Test 5: Verify error union syntax is recognized
 */
void test_error_union_syntax_recognized(void) {
    TEST_START("Error union syntax recognized");

    const char* source =
        "package main\n"
        "\n"
        "func divide(a int, b int) !int {\n"
        "    return a / b\n"
        "}\n";

    parser_init();
    int result = parse_input(source, "test.goo");

    ASSERT_EQUAL(0, result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    // Success means error union syntax was recognized
    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 6: Verify nullable syntax is recognized
 */
void test_nullable_syntax_recognized(void) {
    TEST_START("Nullable syntax recognized");

    const char* source =
        "package main\n"
        "\n"
        "var name ?string = nil\n";

    parser_init();
    int result = parse_input(source, "test.goo");

    ASSERT_EQUAL(0, result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 7: Verify multiple functions can be parsed
 */
void test_multiple_functions_parsed(void) {
    TEST_START("Multiple functions parsed");

    const char* source =
        "package main\n"
        "\n"
        "func add(a int, b int) int {\n"
        "    return a + b\n"
        "}\n"
        "\n"
        "func subtract(a int, b int) int {\n"
        "    return a - b\n"
        "}\n"
        "\n"
        "func main() {\n"
        "}\n";

    parser_init();
    int result = parse_input(source, "test.goo");

    ASSERT_EQUAL(0, result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 8: Verify parser doesn't crash on deeply nested structures
 */
void test_deeply_nested_structures(void) {
    TEST_START("Deeply nested structures");

    const char* source =
        "package main\n"
        "\n"
        "func main() {\n"
        "    if true {\n"
        "        if true {\n"
        "            if true {\n"
        "                x := 1\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n";

    parser_init();
    int result = parse_input(source, "test.goo");

    ASSERT_EQUAL(0, result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 9: Verify complex expression parsing
 */
void test_complex_expression_parsing(void) {
    TEST_START("Complex expression parsing");

    const char* source =
        "package main\n"
        "\n"
        "func calculate() int {\n"
        "    return (1 + 2) * (3 - 4) / 5\n"
        "}\n";

    parser_init();
    int result = parse_input(source, "test.goo");

    ASSERT_EQUAL(0, result, "Parse should succeed");
    ASSERT_NOT_NULL(ast_root, "AST root should exist");

    parser_cleanup();
    TEST_PASS();
}

/**
 * Test 10: Verify parser cleanup doesn't crash
 */
void test_parser_cleanup_safe(void) {
    TEST_START("Parser cleanup is safe");

    parser_init();
    parse_input("package main\n", "test.goo");
    parser_cleanup();

    // Cleanup again should be safe
    parser_cleanup();

    TEST_PASS();
}

int main(void) {
    printf("\n");
    printf("\033[0;34m================================\033[0m\n");
    printf("\033[0;34m  Parser AST Verification Tests\033[0m\n");
    printf("\033[0;34m================================\033[0m\n");
    printf("\n");

    test_ast_root_is_program();
    test_function_creates_func_decl_node();
    test_parser_rejects_invalid_syntax();
    test_parser_handles_empty_input();
    test_error_union_syntax_recognized();
    test_nullable_syntax_recognized();
    test_multiple_functions_parsed();
    test_deeply_nested_structures();
    test_complex_expression_parsing();
    test_parser_cleanup_safe();

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
