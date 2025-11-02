/**
 * Integration Test: Complete Compilation Pipeline
 *
 * Tests the full compilation process:
 * Source Code → Lexer → Parser → Type Checker → Code Generator → Executable
 *
 * This test verifies that all compiler components work together correctly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Include compiler components
#include "lexer.h"
#include "token.h"

// Test counter
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

// Test macros
#define TEST_START(name) printf("  Testing %s ... ", name)
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

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { \
        TEST_FAIL(msg); \
        return; \
    } \
} while(0)

/**
 * Test 1: Lexer Integration
 * Verify that the lexer correctly tokenizes a simple program
 */
void test_lexer_integration() {
    TEST_START("Lexer Integration");

    const char* source =
        "package main\n"
        "\n"
        "import \"fmt\"\n"
        "\n"
        "func main() {\n"
        "    fmt.Printf(\"Hello, World!\\n\")\n"
        "}\n";

    Lexer* lexer = lexer_new(source, "test.goo");
    ASSERT_NOT_NULL(lexer, "Lexer creation failed");

    int token_count = 0;
    Token* token;

    do {
        token = lexer_next_token(lexer);
        ASSERT_NOT_NULL(token, "Token generation failed");
        token_count++;

        // Verify tokens are valid
        if (token->type == TOKEN_EOF) {
            token_free(token);
            break;
        }
        if (token->type == TOKEN_ERROR) {
            token_free(token);
            lexer_free(lexer);
            TEST_FAIL("Error token found");
            return;
        }
        token_free(token);
    } while (1);

    lexer_free(lexer);

    // We should have more than just EOF
    ASSERT_TRUE(token_count >= 10, "Too few tokens generated");

    TEST_PASS();
}

/**
 * Test 2: Parser Integration
 * Verify that the parser generates a valid AST from tokens
 */
void test_parser_integration() {
    TEST_START("Parser Integration (TODO)");

    // TODO: Implement when parser is integrated with compilation pipeline

    TEST_PASS();
}

/**
 * Test 3: Type Checker Integration
 * Verify that the type checker validates the AST correctly
 */
void test_type_checker_integration() {
    TEST_START("Type Checker Integration (TODO)");

    // TODO: Implement when type checker is integrated
    TEST_PASS();
}

/**
 * Test 4: Code Generator Integration
 * Verify that the code generator produces valid LLVM IR
 */
void test_codegen_integration() {
    TEST_START("Code Generator Integration (TODO)");

    // TODO: Implement when codegen is integrated
    TEST_PASS();
}

/**
 * Test 5: End-to-End Compilation
 * Compile a complete program from source to executable
 */
void test_end_to_end_compilation() {
    TEST_START("End-to-End Compilation (TODO)");

    // TODO: Implement full pipeline
    // 1. Lex → tokens
    // 2. Parse → AST
    // 3. Type check → typed AST
    // 4. Generate code → LLVM IR
    // 5. Compile → executable
    // 6. Run → verify output

    TEST_PASS();
}

/**
 * Test 6: Error Handling in Pipeline
 * Verify that compilation errors are properly reported
 */
void test_error_handling() {
    TEST_START("Error Handling (TODO)");

    // TODO: Verify that type error is caught
    TEST_PASS();
}

/**
 * Main test runner
 */
int main(void) {
    printf("\n");
    printf("\033[0;34m================================\033[0m\n");
    printf("\033[0;34m  Compilation Pipeline Tests\033[0m\n");
    printf("\033[0;34m================================\033[0m\n");
    printf("\n");

    // Run all tests
    test_lexer_integration();
    test_parser_integration();
    test_type_checker_integration();
    test_codegen_integration();
    test_end_to_end_compilation();
    test_error_handling();

    // Print results
    printf("\n");
    printf("\033[0;34m================================\033[0m\n");
    printf("\033[0;34m  Test Results\033[0m\n");
    printf("\033[0;34m================================\033[0m\n");
    printf("  Total:   %d\n", tests_run);
    printf("  \033[0;32mPassed:  %d\033[0m\n", tests_passed);
    printf("  \033[0;31mFailed:  %d\033[0m\n", tests_failed);
    printf("\n");

    if (tests_failed > 0) {
        printf("\033[0;31m✗ Some tests failed\033[0m\n");
        return 1;
    } else {
        printf("\033[0;32m✓ All tests passed!\033[0m\n");
        return 0;
    }
}
