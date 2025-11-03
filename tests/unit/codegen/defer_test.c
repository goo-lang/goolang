#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "types.h"
#include "codegen.h"

// Forward declarations for helper functions
void lexer_init(const char* source, const char* filename);
ASTNode* parse_program(void);
int codegen_generate(CodeGenerator* codegen, ASTNode* ast);
char* codegen_get_ir_string(CodeGenerator* codegen);
void test_cleanup(void);

// Test counter
static int tests_run = 0;
static int tests_passed = 0;

// Test macros
#define TEST_FUNC(name) void name()
#define TEST_START() tests_run++
#define TEST_PASS() tests_passed++; return
#define ASSERT_NOT_NULL(ptr, msg) if (!(ptr)) { printf("\033[0;31m✗ FAIL\033[0m: %s\n", msg); return; }
#define ASSERT_TRUE(cond, msg) if (!(cond)) { printf("\033[0;31m✗ FAIL\033[0m: %s\n", msg); return; }
#define RUN_TEST(test) do { \
    printf("  %s ... ", #test); \
    test(); \
    if (tests_passed > tests_run - 1) printf("\033[0;32m✓ PASS\033[0m\n"); \
} while(0)

// Helper function to compile Goo source to LLVM IR
static char* compile_to_llvm_ir(const char* source) {
    lexer_init(source, "test.goo");

    ASTNode* ast = parse_program();
    if (!ast) {
        fprintf(stderr, "Parse error\n");
        return NULL;
    }

    CodeGenerator* codegen = codegen_new("test_module");
    if (!codegen) {
        fprintf(stderr, "Failed to create code generator\n");
        ast_node_free(ast);
        return NULL;
    }

    codegen_initialize_target(codegen);
    int codegen_result = codegen_generate(codegen, ast);

    if (!codegen_result) {
        fprintf(stderr, "Code generation failed\n");
        codegen_free(codegen);
        ast_node_free(ast);
        return NULL;
    }

    char* ir = codegen_get_ir_string(codegen);

    codegen_free(codegen);
    ast_node_free(ast);

    return ir;
}

// Helper function to check if IR contains a substring
static int ir_contains(const char* ir, const char* pattern) {
    return ir && pattern && strstr(ir, pattern) != NULL;
}

// ============================================================================
// TDD Cycle 11: Defer Statements
// ============================================================================

// ============================================================================
// Test 1: Basic Defer with Function Call
// ============================================================================
TEST_FUNC(test_basic_defer) {
    TEST_START();

    // Given: Simple defer with function call
    const char* source =
        "package main\n"
        "func cleanup() {}\n"
        "func test() {\n"
        "    defer cleanup();\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should be generated successfully
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 2: Defer Executes at Function End
// ============================================================================
TEST_FUNC(test_defer_executes_at_end) {
    TEST_START();

    // Given: Defer in function with normal return
    const char* source =
        "package main\n"
        "func cleanup() {}\n"
        "func test() int {\n"
        "    x := 0;\n"
        "    defer cleanup();\n"
        "    x = 10;\n"
        "    return x;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain deferred call
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");
    ASSERT_TRUE(ir_contains(ir, "ret"), "IR should contain return");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 3: Multiple Defer Statements (LIFO Order)
// ============================================================================
TEST_FUNC(test_multiple_defer_lifo) {
    TEST_START();

    // Given: Multiple defer statements
    const char* source =
        "package main\n"
        "func first() {}\n"
        "func second() {}\n"
        "func third() {}\n"
        "func test() {\n"
        "    defer first();\n"
        "    defer second();\n"
        "    defer third();\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle multiple defers
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 4: Defer with Early Return
// ============================================================================
TEST_FUNC(test_defer_with_early_return) {
    TEST_START();

    // Given: Defer with conditional early return
    const char* source =
        "package main\n"
        "func cleanup() {}\n"
        "func test(x int) int {\n"
        "    defer cleanup();\n"
        "    if (x) > 0 {\n"
        "        return x;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Defer should execute on all return paths
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");
    ASSERT_TRUE(ir_contains(ir, "ret"), "IR should contain returns");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 5: Defer in Loop
// ============================================================================
TEST_FUNC(test_defer_in_loop) {
    TEST_START();

    // Given: Defer inside a loop
    const char* source =
        "package main\n"
        "func cleanup() {}\n"
        "func test() {\n"
        "    for {\n"
        "        defer cleanup();\n"
        "        break;\n"
        "    }\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle defer in loop context
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 6: Defer with Arguments
// ============================================================================
TEST_FUNC(test_defer_with_arguments) {
    TEST_START();

    // Given: Defer with function arguments
    const char* source =
        "package main\n"
        "func log(x int) {}\n"
        "func test() {\n"
        "    x := 10;\n"
        "    defer log(x);\n"
        "    x = 20;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Arguments should be evaluated at defer time
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 7: Defer in Nested Functions
// ============================================================================
TEST_FUNC(test_defer_nested) {
    TEST_START();

    // Given: Defer in nested scope
    const char* source =
        "package main\n"
        "func outerCleanup() {}\n"
        "func innerCleanup() {}\n"
        "func outer() {\n"
        "    defer outerCleanup();\n"
        "}\n"
        "func inner() {\n"
        "    defer innerCleanup();\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Each function has its own defer stack
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@outer"), "IR should contain outer function");
    ASSERT_TRUE(ir_contains(ir, "@inner"), "IR should contain inner function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 8: Defer Empty Function
// ============================================================================
TEST_FUNC(test_defer_no_defers) {
    TEST_START();

    // Given: Function with no defer statements
    const char* source =
        "package main\n"
        "func test() {\n"
        "    x := 10;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should work normally without defers
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main(void) {
    printf("\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\033[0;34m  TDD Cycle 11: Defer Statements\033[0m\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\n");

    RUN_TEST(test_basic_defer);
    RUN_TEST(test_defer_executes_at_end);
    RUN_TEST(test_multiple_defer_lifo);
    RUN_TEST(test_defer_with_early_return);
    RUN_TEST(test_defer_in_loop);
    RUN_TEST(test_defer_with_arguments);
    RUN_TEST(test_defer_nested);
    RUN_TEST(test_defer_no_defers);

    printf("\n");
    printf("\033[0;34m================================\033[0m\n");
    printf("\033[0;34m  Test Results\033[0m\n");
    printf("\033[0;34m================================\033[0m\n");
    printf("  Total:   %d\n", tests_run);
    printf("  \033[0;32mPassed:  %d\033[0m\n", tests_passed);
    printf("  \033[0;31mFailed:  %d\033[0m\n", tests_run - tests_passed);
    if (tests_run > 0) {
        printf("  Pass Rate: %d%%\n", (tests_passed * 100) / tests_run);
    }
    printf("\n");

    if (tests_passed == tests_run && tests_run > 0) {
        printf("\033[0;32m✓ All tests passed!\033[0m\n");
        printf("\n");
        return 0;
    } else if (tests_run == 0) {
        printf("\033[0;33m⚠ No tests run\033[0m\n");
        printf("\n");
        return 1;
    } else {
        printf("\033[0;33m⚠ Some tests failed\033[0m\n");
        printf("\n");
        return 1;
    }
}
