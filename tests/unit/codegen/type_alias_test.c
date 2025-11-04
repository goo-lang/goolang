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
// Test 1: Simple Type Alias
// ============================================================================
TEST_FUNC(test_simple_alias) {
    TEST_START();

    // Given: Simple type definition (Go doesn't distinguish at runtime)
    const char* source =
        "package main\n"
        "type MyInt int\n"
        "func get_value() MyInt {\n"
        "    var x MyInt = 42\n"
        "    return x\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile and use underlying type
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_value"), "IR should contain get_value function");
    ASSERT_TRUE(ir_contains(ir, "i32") || ir_contains(ir, "i64"),
                "IR should use integer type");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 2: Function Type Alias
// ============================================================================
TEST_FUNC(test_function_alias) {
    TEST_START();

    // Given: Function type definition
    const char* source =
        "package main\n"
        "type Handler func(int) int\n"
        "func apply(h Handler, x int) int {\n"
        "    return h(x)\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should handle function type
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@apply"), "IR should contain apply function");
    ASSERT_TRUE(ir_contains(ir, "call") || ir_contains(ir, "ptr"),
                "IR should handle function pointer");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 3: Alias Usage in Parameters
// ============================================================================
TEST_FUNC(test_alias_parameter) {
    TEST_START();

    // Given: Type definition used in parameters
    const char* source =
        "package main\n"
        "type Counter int\n"
        "func increment(c Counter) Counter {\n"
        "    return c + 1\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile successfully
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@increment"), "IR should contain increment function");
    ASSERT_TRUE(ir_contains(ir, "add"), "IR should contain addition");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main() {
    printf("\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\033[0;34m  TDD Cycle 16: Type Aliases\033[0m\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\n");

    RUN_TEST(test_simple_alias);
    RUN_TEST(test_function_alias);
    RUN_TEST(test_alias_parameter);

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
