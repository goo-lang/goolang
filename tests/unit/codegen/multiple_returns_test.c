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
// Test 1: Basic Multiple Return Declaration
// ============================================================================
TEST_FUNC(test_multiple_return_declaration) {
    TEST_START();

    // Given: Function with multiple return values
    const char* source =
        "package main\n"
        "func divide(a int, b int) (int, bool) {\n"
        "    if b == 0 {\n"
        "        return 0, false;\n"
        "    }\n"
        "    return a / b, true;\n"
        "}\n"
        "func test() int {\n"
        "    return 42;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain divide function with struct return type
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@divide"), "IR should contain divide function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 2: Multiple Assignment
// ============================================================================
TEST_FUNC(test_multiple_assignment) {
    TEST_START();

    // Given: Function call with multiple assignment
    const char* source =
        "package main\n"
        "func get_values() (int, int) {\n"
        "    return 10, 20;\n"
        "}\n"
        "func test() int {\n"
        "    a, b := get_values();\n"
        "    return a + b;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle multiple assignment
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_values"), "IR should contain get_values function");
    ASSERT_TRUE(ir_contains(ir, "add"), "IR should contain addition");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 3: Error Handling Pattern
// ============================================================================
TEST_FUNC(test_error_handling_pattern) {
    TEST_START();

    // Given: Go-style error handling pattern
    const char* source =
        "package main\n"
        "func divide(a int, b int) (int, bool) {\n"
        "    if b == 0 {\n"
        "        return 0, false;\n"
        "    }\n"
        "    return a / b, true;\n"
        "}\n"
        "func test() int {\n"
        "    result, ok := divide(10, 2);\n"
        "    if ok {\n"
        "        return result;\n"
        "    }\n"
        "    return -1;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle error checking
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@divide"), "IR should contain divide function");
    ASSERT_TRUE(ir_contains(ir, "br"), "IR should contain conditional branch");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 4: Underscore for Unused Returns
// ============================================================================
TEST_FUNC(test_underscore_unused_return) {
    TEST_START();

    // Given: Using underscore to ignore return value
    const char* source =
        "package main\n"
        "func get_values() (int, int) {\n"
        "    return 10, 20;\n"
        "}\n"
        "func test() int {\n"
        "    _, b := get_values();\n"
        "    return b;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should ignore first return value
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_values"), "IR should contain get_values function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 5: Multiple Returns with Different Types
// ============================================================================
TEST_FUNC(test_multiple_returns_different_types) {
    TEST_START();

    // Given: Function returning different types
    const char* source =
        "package main\n"
        "func get_info() (int, bool, int) {\n"
        "    return 42, true, 100;\n"
        "}\n"
        "func test() int {\n"
        "    a, b, c := get_info();\n"
        "    if b {\n"
        "        return a + c;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle three return values
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_info"), "IR should contain get_info function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 6: Named Return Parameters
// ============================================================================
TEST_FUNC(test_named_return_parameters) {
    TEST_START();

    // Given: Function with named return parameters
    const char* source =
        "package main\n"
        "func divide(a int, b int) (result int, ok bool) {\n"
        "    if b == 0 {\n"
        "        result = 0;\n"
        "        ok = false;\n"
        "        return result, ok;\n"
        "    }\n"
        "    result = a / b;\n"
        "    ok = true;\n"
        "    return result, ok;\n"
        "}\n"
        "func test() int {\n"
        "    r, _ := divide(10, 2);\n"
        "    return r;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle named returns
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@divide"), "IR should contain divide function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 7: Returning from Multiple Paths
// ============================================================================
TEST_FUNC(test_multiple_return_paths) {
    TEST_START();

    // Given: Multiple return statements in different branches
    const char* source =
        "package main\n"
        "func check_value(x int) (int, bool) {\n"
        "    if x > 0 {\n"
        "        return x, true;\n"
        "    }\n"
        "    if x < 0 {\n"
        "        return -x, false;\n"
        "    }\n"
        "    return 0, false;\n"
        "}\n"
        "func test() int {\n"
        "    v, _ := check_value(5);\n"
        "    return v;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle all return paths
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@check_value"), "IR should contain check_value function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 8: Passing Multiple Returns to Another Function
// ============================================================================
TEST_FUNC(test_passing_multiple_returns) {
    TEST_START();

    // Given: Using multiple returns as function arguments
    const char* source =
        "package main\n"
        "func get_pair() (int, int) {\n"
        "    return 3, 4;\n"
        "}\n"
        "func add(a int, b int) int {\n"
        "    return a + b;\n"
        "}\n"
        "func test() int {\n"
        "    x, y := get_pair();\n"
        "    return add(x, y);\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should extract and pass values
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_pair"), "IR should contain get_pair function");
    ASSERT_TRUE(ir_contains(ir, "@add"), "IR should contain add function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 9: Simple Two Value Return
// ============================================================================
TEST_FUNC(test_simple_two_value_return) {
    TEST_START();

    // Given: Simplest multiple return case
    const char* source =
        "package main\n"
        "func swap(a int, b int) (int, int) {\n"
        "    return b, a;\n"
        "}\n"
        "func test() int {\n"
        "    x, y := swap(1, 2);\n"
        "    return x + y;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should swap values
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@swap"), "IR should contain swap function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 10: Multiple Returns in Expression
// ============================================================================
TEST_FUNC(test_multiple_returns_in_expression) {
    TEST_START();

    // Given: Using multiple returns directly
    const char* source =
        "package main\n"
        "func get_values() (int, int) {\n"
        "    return 5, 10;\n"
        "}\n"
        "func test() int {\n"
        "    a, b := get_values();\n"
        "    return a * b;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle expression with multiple values
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_values"), "IR should contain get_values function");
    ASSERT_TRUE(ir_contains(ir, "mul"), "IR should contain multiplication");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main() {
    printf("\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\033[0;34m  TDD Cycle 8: Multiple Returns Tests\033[0m\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\n");

    RUN_TEST(test_multiple_return_declaration);
    RUN_TEST(test_multiple_assignment);
    RUN_TEST(test_error_handling_pattern);
    RUN_TEST(test_underscore_unused_return);
    RUN_TEST(test_multiple_returns_different_types);
    RUN_TEST(test_named_return_parameters);
    RUN_TEST(test_multiple_return_paths);
    RUN_TEST(test_passing_multiple_returns);
    RUN_TEST(test_simple_two_value_return);
    RUN_TEST(test_multiple_returns_in_expression);

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
