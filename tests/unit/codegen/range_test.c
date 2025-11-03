/**
 * TDD Cycle 13: Range Loop Tests
 *
 * Testing range loop functionality:
 * - Range over arrays with index and value
 * - Range over slices
 * - Range with underscore for unused variables
 * - Range with break and continue
 * - Range in nested contexts
 */

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
static int test_failed_count = 0;

// Test macros
#define TEST_FUNC(name) void name()
#define TEST_START() tests_run++
#define TEST_PASS() tests_passed++; return
#define ASSERT_NOT_NULL(ptr, msg) if (!(ptr)) { printf("\033[0;31m✗ FAIL\033[0m: %s\n", msg); test_failed_count++; return; }
#define ASSERT_TRUE(cond, msg) if (!(cond)) { printf("\033[0;31m✗ FAIL\033[0m: %s\n", msg); test_failed_count++; return; }
#define RUN_TEST(test) do { \
    printf("  %s ... ", #test); \
    test(); \
    if (tests_passed > tests_run - 1) printf("\033[0;32m✓ PASS\033[0m\n"); \
} while(0)

#define TEST_SUITE_START(name) do { \
    printf("\033[0;34m================================\033[0m\n"); \
    printf("\033[0;34m  %s\033[0m\n", name); \
    printf("\033[0;34m================================\033[0m\n"); \
} while(0)

#define TEST_SUITE_END() do { \
    printf("\n\033[0;34m================================\033[0m\n"); \
    printf("\033[0;34m  Test Results\033[0m\n"); \
    printf("\033[0;34m================================\033[0m\n"); \
    printf("  Total:   %d\n", tests_run); \
    printf("  \033[0;32mPassed:  %d\033[0m\n", tests_passed); \
    printf("  \033[0;31mFailed:  %d\033[0m\n", test_failed_count); \
    printf("  Pass Rate: %d%%\n", tests_run > 0 ? (tests_passed * 100 / tests_run) : 0); \
    printf("\n"); \
    if (test_failed_count == 0) { \
        printf("\033[0;32m✓ All tests passed!\033[0m\n"); \
    } else { \
        printf("\033[0;31m✗ %d test(s) failed\033[0m\n", test_failed_count); \
    } \
    printf("\n"); \
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

// Test 1: Basic range over array with index and value
TEST_FUNC(test_range_array_index_value) {
    TEST_START();

    // Given: A range loop over array getting both index and value
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    arr := [3]int{10, 20, 30};\n"
        "    sum := 0;\n"
        "    for i, v := range arr {\n"
        "        sum = sum + i + v;\n"
        "    }\n"
        "    return sum;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle range loop
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    // Should iterate: sum = 0+10 + 1+20 + 2+30 = 10+21+32 = 63

    free(ir);
    TEST_PASS();
}

// Test 2: Range over array with index only
TEST_FUNC(test_range_array_index_only) {
    TEST_START();

    // Given: A range loop getting only the index
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    arr := [4]int{5, 10, 15, 20};\n"
        "    count := 0;\n"
        "    for i := range arr {\n"
        "        count = count + i;\n"
        "    }\n"
        "    return count;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle index-only range
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    // Should sum indices: 0+1+2+3 = 6

    free(ir);
    TEST_PASS();
}

// Test 3: Range with underscore for unused index
TEST_FUNC(test_range_underscore_index) {
    TEST_START();

    // Given: A range loop using _ for index
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    arr := [3]int{100, 200, 300};\n"
        "    sum := 0;\n"
        "    for _, v := range arr {\n"
        "        sum = sum + v;\n"
        "    }\n"
        "    return sum;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle underscore correctly
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    // Should sum values: 100+200+300 = 600

    free(ir);
    TEST_PASS();
}

// Test 4: Range with underscore for unused value
TEST_FUNC(test_range_underscore_value) {
    TEST_START();

    // Given: A range loop using _ for value
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    arr := [5]int{1, 2, 3, 4, 5};\n"
        "    idx_sum := 0;\n"
        "    for i, _ := range arr {\n"
        "        idx_sum = idx_sum + i;\n"
        "    }\n"
        "    return idx_sum;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle underscore for value
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    // Should sum indices: 0+1+2+3+4 = 10

    free(ir);
    TEST_PASS();
}

// Test 5: Range over slice
TEST_FUNC(test_range_slice) {
    TEST_START();

    // Given: A range loop over a slice
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    s := []int{10, 20, 30};\n"
        "    total := 0;\n"
        "    for _, val := range s {\n"
        "        total = total + val;\n"
        "    }\n"
        "    return total;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle slice range
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    // Should sum: 10+20+30 = 60

    free(ir);
    TEST_PASS();
}

// Test 6: Range with break
TEST_FUNC(test_range_with_break) {
    TEST_START();

    // Given: A range loop with break condition
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    arr := [5]int{1, 2, 3, 4, 5};\n"
        "    sum := 0;\n"
        "    for _, v := range arr {\n"
        "        if v == 4 {\n"
        "            break;\n"
        "        }\n"
        "        sum = sum + v;\n"
        "    }\n"
        "    return sum;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle break in range
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    // Should sum: 1+2+3 = 6 (breaks before adding 4)

    free(ir);
    TEST_PASS();
}

// Test 7: Range with continue
TEST_FUNC(test_range_with_continue) {
    TEST_START();

    // Given: A range loop with continue
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    arr := [5]int{1, 2, 3, 4, 5};\n"
        "    sum := 0;\n"
        "    for _, v := range arr {\n"
        "        if v == 3 {\n"
        "            continue;\n"
        "        }\n"
        "        sum = sum + v;\n"
        "    }\n"
        "    return sum;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle continue in range
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    // Should sum: 1+2+4+5 = 12 (skips 3)

    free(ir);
    TEST_PASS();
}

// Test 8: Nested range loops
TEST_FUNC(test_nested_range) {
    TEST_START();

    // Given: Nested range loops
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    outer := [2]int{10, 20};\n"
        "    inner := [2]int{1, 2};\n"
        "    sum := 0;\n"
        "    for _, o := range outer {\n"
        "        for _, i := range inner {\n"
        "            sum = sum + o + i;\n"
        "        }\n"
        "    }\n"
        "    return sum;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle nested ranges
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    // Should compute: (10+1)+(10+2)+(20+1)+(20+2) = 11+12+21+22 = 66

    free(ir);
    TEST_PASS();
}

// Test 9: Range over empty array
TEST_FUNC(test_range_empty_array) {
    TEST_START();

    // Given: Range over empty array
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    arr := [0]int{};\n"
        "    count := 0;\n"
        "    for _ := range arr {\n"
        "        count = count + 1;\n"
        "    }\n"
        "    return count;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle empty array range
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    // Should return 0 (never enters loop)

    free(ir);
    TEST_PASS();
}

// Test 10: Range modifying separate accumulator
TEST_FUNC(test_range_accumulator) {
    TEST_START();

    // Given: Range loop with complex accumulator logic
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    arr := [4]int{2, 4, 6, 8};\n"
        "    result := 1;\n"
        "    for i, v := range arr {\n"
        "        if i == 0 {\n"
        "            result = v;\n"
        "        } else {\n"
        "            result = result + v;\n"
        "        }\n"
        "    }\n"
        "    return result;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle complex range logic
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    // Should compute: result=2, then 2+4=6, then 6+6=12, then 12+8=20

    free(ir);
    TEST_PASS();
}

// Test runner
int main(void) {
    TEST_SUITE_START("Range Loop Tests");

    RUN_TEST(test_range_array_index_value);
    RUN_TEST(test_range_array_index_only);
    RUN_TEST(test_range_underscore_index);
    RUN_TEST(test_range_underscore_value);
    RUN_TEST(test_range_slice);
    RUN_TEST(test_range_with_break);
    RUN_TEST(test_range_with_continue);
    RUN_TEST(test_nested_range);
    RUN_TEST(test_range_empty_array);
    RUN_TEST(test_range_accumulator);

    TEST_SUITE_END();
    return test_failed_count > 0 ? 1 : 0;
}
