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
        return NULL;
    }

    CodeGenerator* codegen = codegen_new("test_module");
    if (!codegen) {
        ast_node_free(ast);
        return NULL;
    }

    codegen_initialize_target(codegen);
    int codegen_result = codegen_generate(codegen, ast);

    if (!codegen_result) {
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
// Test 1: Simple Variadic Function
// ============================================================================
TEST_FUNC(test_simple_variadic) {
    TEST_START();

    // Given: Simple variadic function
    const char* source =
        "package main\n"
        "func sum(nums ...int) int {\n"
        "    return 0\n"
        "}\n"
        "func main() {\n"
        "    var x int = sum(1, 2, 3)\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile successfully
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@sum"), "IR should contain sum function");
    ASSERT_TRUE(ir_contains(ir, "@main"), "IR should contain main function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 2: Variadic with Fixed Parameters
// ============================================================================
TEST_FUNC(test_variadic_with_fixed) {
    TEST_START();

    // Given: Variadic function with fixed and variadic parameters
    const char* source =
        "package main\n"
        "func printf_like(format string, args ...int) int {\n"
        "    return 0\n"
        "}\n"
        "func main() {\n"
        "    var n int = printf_like(\"test\", 1, 2, 3)\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile successfully
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@printf_like"), "IR should contain printf_like function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 3: Empty Variadic Arguments
// ============================================================================
TEST_FUNC(test_empty_variadic) {
    TEST_START();

    // Given: Variadic function called with no variadic arguments
    const char* source =
        "package main\n"
        "func count(nums ...int) int {\n"
        "    return 0\n"
        "}\n"
        "func main() {\n"
        "    var n int = count()\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile successfully
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@count"), "IR should contain count function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 4: Variadic Function with Single Argument
// ============================================================================
TEST_FUNC(test_single_variadic_arg) {
    TEST_START();

    // Given: Variadic function called with single argument
    const char* source =
        "package main\n"
        "func get_first(nums ...int) int {\n"
        "    return 0\n"
        "}\n"
        "func main() {\n"
        "    var x int = get_first(42)\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile successfully
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_first"), "IR should contain get_first function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 5: Multiple Variadic Calls
// ============================================================================
TEST_FUNC(test_multiple_variadic_calls) {
    TEST_START();

    // Given: Multiple calls to variadic function with different arg counts
    const char* source =
        "package main\n"
        "func max_int(nums ...int) int {\n"
        "    return 0\n"
        "}\n"
        "func main() {\n"
        "    var a int = max_int(1, 2)\n"
        "    var b int = max_int(5, 3, 9, 1)\n"
        "    var c int = max_int()\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile successfully
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@max_int"), "IR should contain max_int function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main() {
    printf("\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\033[0;34m  TDD Cycle 18: Variadic Functions\033[0m\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\n");

    RUN_TEST(test_simple_variadic);
    RUN_TEST(test_variadic_with_fixed);
    RUN_TEST(test_empty_variadic);
    RUN_TEST(test_single_variadic_arg);
    RUN_TEST(test_multiple_variadic_calls);

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
