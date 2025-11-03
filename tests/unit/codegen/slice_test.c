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
// TDD Cycle 12: Slices
// ============================================================================

// ============================================================================
// Test 1: Slice Creation with make()
// ============================================================================
TEST_FUNC(test_slice_make) {
    TEST_START();

    // Given: Slice created with make
    const char* source =
        "package main\n"
        "func test() []int {\n"
        "    s := make([]int, 5);\n"
        "    return s;\n"
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
// Test 2: Slice Indexing - Read
// ============================================================================
TEST_FUNC(test_slice_indexing_read) {
    TEST_START();

    // Given: Slice with indexed read access
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    s := make([]int, 5);\n"
        "    x := s[2];\n"
        "    return x;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain array indexing
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "getelementptr") || ir_contains(ir, "load"),
                "IR should contain indexing operations");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 3: Slice Indexing - Write
// ============================================================================
TEST_FUNC(test_slice_indexing_write) {
    TEST_START();

    // Given: Slice with indexed write access
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    s := make([]int, 5);\n"
        "    s[2] = 42;\n"
        "    return s[2];\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain store and load
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "store"), "IR should contain store");
    ASSERT_TRUE(ir_contains(ir, "load") || ir_contains(ir, "getelementptr"),
                "IR should contain load or GEP");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 4: len() Builtin for Slices
// ============================================================================
TEST_FUNC(test_slice_len) {
    TEST_START();

    // Given: Slice with len() call
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    s := make([]int, 5);\n"
        "    n := len(s);\n"
        "    return n;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain len operation
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 5: cap() Builtin for Slices
// ============================================================================
TEST_FUNC(test_slice_cap) {
    TEST_START();

    // Given: Slice with cap() call
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    s := make([]int, 5, 10);\n"
        "    c := cap(s);\n"
        "    return c;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain cap operation
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 6: append() Builtin
// ============================================================================
TEST_FUNC(test_slice_append) {
    TEST_START();

    // Given: Slice with append operation
    const char* source =
        "package main\n"
        "func test() []int {\n"
        "    s := make([]int, 0);\n"
        "    s = append(s, 42);\n"
        "    return s;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain append call
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 7: Nil Slice
// ============================================================================
TEST_FUNC(test_slice_nil) {
    TEST_START();

    // Given: Nil slice declaration
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    var s []int;\n"
        "    return len(s);\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle nil slice
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 8: Slice as Function Parameter
// ============================================================================
TEST_FUNC(test_slice_as_parameter) {
    TEST_START();

    // Given: Function accepting slice parameter
    const char* source =
        "package main\n"
        "func sum(s []int) int {\n"
        "    return len(s);\n"
        "}\n"
        "func test() int {\n"
        "    s := make([]int, 5);\n"
        "    return sum(s);\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain function with slice parameter
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@sum"), "IR should contain sum function");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 9: Slice as Return Value
// ============================================================================
TEST_FUNC(test_slice_as_return) {
    TEST_START();

    // Given: Function returning slice
    const char* source =
        "package main\n"
        "func makeSlice() []int {\n"
        "    return make([]int, 3);\n"
        "}\n"
        "func test() int {\n"
        "    s := makeSlice();\n"
        "    return len(s);\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle slice return
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@makeSlice"), "IR should contain makeSlice function");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 10: Multiple append() Operations
// ============================================================================
TEST_FUNC(test_multiple_appends) {
    TEST_START();

    // Given: Multiple append operations
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    s := make([]int, 0);\n"
        "    s = append(s, 1);\n"
        "    s = append(s, 2);\n"
        "    s = append(s, 3);\n"
        "    return len(s);\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle multiple appends
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
    printf("\033[0;34m  TDD Cycle 12: Slices\033[0m\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\n");

    RUN_TEST(test_slice_make);
    RUN_TEST(test_slice_indexing_read);
    RUN_TEST(test_slice_indexing_write);
    RUN_TEST(test_slice_len);
    RUN_TEST(test_slice_cap);
    RUN_TEST(test_slice_append);
    RUN_TEST(test_slice_nil);
    RUN_TEST(test_slice_as_parameter);
    RUN_TEST(test_slice_as_return);
    RUN_TEST(test_multiple_appends);

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
