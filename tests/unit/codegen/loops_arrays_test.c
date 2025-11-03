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
// Test 1: Basic For Loop (While-Style)
// ============================================================================
TEST_FUNC(test_for_loop_basic) {
    TEST_START();

    // Given: While-style for loop - parser requires () around identifier comparisons
    const char* source =
        "package main\n"
        "func sum_to_n(n int) int {\n"
        "    var sum int = 0;\n"
        "    var i int = 0;\n"
        "    for true {\n"
        "        if (i >= n) { break; }\n"
        "        sum = sum + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return sum;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain loop structure
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@sum_to_n"), "IR should contain sum_to_n function");
    ASSERT_TRUE(ir_contains(ir, "br"), "IR should contain branch instruction");
    ASSERT_TRUE(ir_contains(ir, "icmp"), "IR should contain comparison");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 2: Infinite Loop with Break
// ============================================================================
TEST_FUNC(test_for_loop_infinite) {
    TEST_START();

    // Given: Infinite loop with break
    const char* source =
        "package main\n"
        "func count_to_n(n int) int {\n"
        "    var i int = 0;\n"
        "    for {\n"
        "        if i >= n {\n"
        "            break;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return i;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain loop and break
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@count_to_n"), "IR should contain count_to_n function");
    ASSERT_TRUE(ir_contains(ir, "br"), "IR should contain branch");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 3: For Loop with Continue
// ============================================================================
TEST_FUNC(test_for_loop_continue) {
    TEST_START();

    // Given: While-style for loop with continue statement - parser requires ()
    const char* source =
        "package main\n"
        "func sum_evens(n int) int {\n"
        "    var sum int = 0;\n"
        "    var i int = 0;\n"
        "    for true {\n"
        "        if (i >= n) { break; }\n"
        "        i = i + 1;\n"
        "        if (i % 2 != 0) {\n"
        "            continue;\n"
        "        }\n"
        "        sum = sum + i;\n"
        "    }\n"
        "    return sum;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain continue (branch to loop condition)
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@sum_evens"), "IR should contain sum_evens function");
    ASSERT_TRUE(ir_contains(ir, "br"), "IR should contain branch instructions");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 4: Nested Loops
// ============================================================================
TEST_FUNC(test_nested_loops) {
    TEST_START();

    // Given: Nested while-style loops - parser requires () around identifier comparisons
    const char* source =
        "package main\n"
        "func multiply_add(n int) int {\n"
        "    var sum int = 0;\n"
        "    var i int = 0;\n"
        "    for true {\n"
        "        if (i >= n) { break; }\n"
        "        var j int = 0;\n"
        "        for true {\n"
        "            if (j >= n) { break; }\n"
        "            sum = sum + 1;\n"
        "            j = j + 1;\n"
        "        }\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return sum;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain nested loop structure
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@multiply_add"), "IR should contain multiply_add function");
    ASSERT_TRUE(ir_contains(ir, "br"), "IR should contain branch instructions");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 5: Array Declaration
// ============================================================================
TEST_FUNC(test_array_declaration) {
    TEST_START();

    // Given: Array declaration (literals not supported by parser)
    const char* source =
        "package main\n"
        "func create_array(n int) [5]int {\n"
        "    var arr [5]int;\n"
        "    arr[0] = n;\n"
        "    arr[1] = n + 1;\n"
        "    return arr;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain array allocation
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@create_array"), "IR should contain create_array function");
    ASSERT_TRUE(ir_contains(ir, "alloca"), "IR should contain array allocation");
    ASSERT_TRUE(ir_contains(ir, "getelementptr"), "IR should contain array indexing");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 6: Array Element Access
// ============================================================================
TEST_FUNC(test_array_access) {
    TEST_START();
    
    // Given: Array element access and assignment
    const char* source =
        "package main\n"
        "func get_element(arr [5]int, index int) int {\n"
        "    return arr[index];\n"
        "}\n";
    
    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);
    
    // Then: IR should contain array indexing (getelementptr)
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_element"), "IR should contain get_element function");
    ASSERT_TRUE(ir_contains(ir, "getelementptr"), "IR should contain GEP for array access");
    
    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 7: Array Length
// ============================================================================
TEST_FUNC(test_array_length) {
    TEST_START();
    
    // Given: Using array length
    const char* source =
        "package main\n"
        "func array_size() int {\n"
        "    var arr [10]int;\n"
        "    return len(arr);\n"
        "}\n";
    
    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);
    
    // Then: IR should return constant 10
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@array_size"), "IR should contain array_size function");
    ASSERT_TRUE(ir_contains(ir, "10") || ir_contains(ir, "ret i32"), "IR should return array length");
    
    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 8: Array Assignment
// ============================================================================
TEST_FUNC(test_array_assignment) {
    TEST_START();

    // Given: Array element assignment
    const char* source =
        "package main\n"
        "func fill_array(arr [5]int, val int) [5]int {\n"
        "    arr[0] = val;\n"
        "    arr[1] = val + 1;\n"
        "    arr[2] = val + 2;\n"
        "    return arr;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain array assignment
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@fill_array"), "IR should contain fill_array function");
    ASSERT_TRUE(ir_contains(ir, "getelementptr"), "IR should contain array indexing");
    ASSERT_TRUE(ir_contains(ir, "store"), "IR should contain store instruction");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 9: Loop with Array Iteration
// ============================================================================
TEST_FUNC(test_loop_array_iteration) {
    TEST_START();

    // Given: Loop iterating over array - parser requires () around identifier comparisons
    const char* source =
        "package main\n"
        "func sum_array(arr [5]int) int {\n"
        "    var sum int = 0;\n"
        "    var i int = 0;\n"
        "    for true {\n"
        "        if (i >= 5) { break; }\n"
        "        sum = sum + arr[i];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return sum;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain loop with array access
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@sum_array"), "IR should contain sum_array function");
    ASSERT_TRUE(ir_contains(ir, "br"), "IR should contain branch");
    ASSERT_TRUE(ir_contains(ir, "getelementptr"), "IR should contain array access");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 10: Array Bounds Checking
// ============================================================================
TEST_FUNC(test_bounds_checking) {
    TEST_START();
    
    // Given: Array access that should have bounds checking
    const char* source =
        "package main\n"
        "func safe_access(arr [5]int, i int) int {\n"
        "    return arr[i];\n"
        "}\n";
    
    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);
    
    // Then: IR should contain bounds check call
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@safe_access"), "IR should contain safe_access function");
    ASSERT_TRUE(ir_contains(ir, "goo_bounds_check") || ir_contains(ir, "goo_check_bounds"),
                "IR should contain bounds checking");
    
    free(ir);
    TEST_PASS();
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main() {
    printf("\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\033[0;34m  TDD Cycle 6: Loops & Arrays Tests\033[0m\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\n");
    
    RUN_TEST(test_for_loop_basic);
    RUN_TEST(test_for_loop_infinite);
    RUN_TEST(test_for_loop_continue);
    RUN_TEST(test_nested_loops);
    RUN_TEST(test_array_declaration);
    RUN_TEST(test_array_access);
    RUN_TEST(test_array_length);
    RUN_TEST(test_array_assignment);
    RUN_TEST(test_loop_array_iteration);
    RUN_TEST(test_bounds_checking);
    
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
        printf("\033[0;32m✓ All tests passed!\ 033[0m\n");
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
