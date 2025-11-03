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
// TDD Cycle 10: Switch Statements
// ============================================================================

// ============================================================================
// Test 1: Basic Switch with Integer
// ============================================================================
TEST_FUNC(test_basic_switch_int) {
    TEST_START();

    // Given: Basic switch with integer cases
    const char* source =
        "package main\n"
        "func test(x int) int {\n"
        "    switch x {\n"
        "    case 1:\n"
        "        return 10;\n"
        "    case 2:\n"
        "        return 20;\n"
        "    case 3:\n"
        "        return 30;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain switch or branch instructions
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");
    ASSERT_TRUE(ir_contains(ir, "switch") || ir_contains(ir, "icmp"),
                "IR should contain switch or comparison instructions");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 2: Switch with Multiple Values per Case
// ============================================================================
TEST_FUNC(test_switch_multiple_values) {
    TEST_START();

    // Given: Switch with multiple values in one case
    const char* source =
        "package main\n"
        "func test(x int) int {\n"
        "    switch x {\n"
        "    case 1, 2, 3:\n"
        "        return 100;\n"
        "    case 4, 5:\n"
        "        return 200;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle multiple case values
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 3: Switch with Default Case
// ============================================================================
TEST_FUNC(test_switch_default) {
    TEST_START();

    // Given: Switch with default case
    const char* source =
        "package main\n"
        "func test(x int) int {\n"
        "    switch x {\n"
        "    case 1:\n"
        "        return 10;\n"
        "    case 2:\n"
        "        return 20;\n"
        "    default:\n"
        "        return 99;\n"
        "    }\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle default case
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 4: Switch with No Condition (Type Switch Pattern)
// ============================================================================
TEST_FUNC(test_switch_no_condition) {
    TEST_START();

    // Given: Switch without condition (like if-else chain)
    const char* source =
        "package main\n"
        "func test(x int, y int) int {\n"
        "    switch {\n"
        "    case x > 10:\n"
        "        return 1;\n"
        "    case y > 20:\n"
        "        return 2;\n"
        "    default:\n"
        "        return 0;\n"
        "    }\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle boolean cases
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");
    ASSERT_TRUE(ir_contains(ir, "icmp"), "IR should contain comparisons");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 5: Switch with Break Statement
// ============================================================================
TEST_FUNC(test_switch_break) {
    TEST_START();

    // Given: Switch with explicit break
    const char* source =
        "package main\n"
        "func test(x int) int {\n"
        "    var result int = 0;\n"
        "    switch x {\n"
        "    case 1:\n"
        "        result = 10;\n"
        "        break;\n"
        "    case 2:\n"
        "        result = 20;\n"
        "        break;\n"
        "    }\n"
        "    return result;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle break properly
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");
    ASSERT_TRUE(ir_contains(ir, "br"), "IR should contain branch instructions");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 6: Nested Switch Statements
// ============================================================================
TEST_FUNC(test_nested_switch) {
    TEST_START();

    // Given: Nested switch statements
    const char* source =
        "package main\n"
        "func test(x int, y int) int {\n"
        "    switch x {\n"
        "    case 1:\n"
        "        switch y {\n"
        "        case 10:\n"
        "            return 100;\n"
        "        case 20:\n"
        "            return 200;\n"
        "        }\n"
        "        return 0;\n"
        "    case 2:\n"
        "        return 300;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle nested switches
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 7: Switch with Variable Assignment
// ============================================================================
TEST_FUNC(test_switch_variable_assignment) {
    TEST_START();

    // Given: Switch with variable assignments in cases
    const char* source =
        "package main\n"
        "func test(x int) int {\n"
        "    var result int = 0;\n"
        "    switch x {\n"
        "    case 1:\n"
        "        result = result + 10;\n"
        "    case 2:\n"
        "        result = result + 20;\n"
        "    case 3:\n"
        "        result = result + 30;\n"
        "    }\n"
        "    return result;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle variable assignments
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");
    ASSERT_TRUE(ir_contains(ir, "store") || ir_contains(ir, "add"),
                "IR should contain store or add instructions");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 8: Switch with Expression Cases
// ============================================================================
TEST_FUNC(test_switch_expression_cases) {
    TEST_START();

    // Given: Switch with expression in tag
    const char* source =
        "package main\n"
        "func test(x int, y int) int {\n"
        "    switch x + y {\n"
        "    case 10:\n"
        "        return 1;\n"
        "    case 20:\n"
        "        return 2;\n"
        "    default:\n"
        "        return 0;\n"
        "    }\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should evaluate expression
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");
    ASSERT_TRUE(ir_contains(ir, "add"), "IR should contain add instruction");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 9: Switch Returns in All Cases
// ============================================================================
TEST_FUNC(test_switch_all_return) {
    TEST_START();

    // Given: Switch where all cases return
    const char* source =
        "package main\n"
        "func test(x int) int {\n"
        "    switch x {\n"
        "    case 1:\n"
        "        return 10;\n"
        "    case 2:\n"
        "        return 20;\n"
        "    default:\n"
        "        return 0;\n"
        "    }\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle all-return switch
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");
    ASSERT_TRUE(ir_contains(ir, "ret"), "IR should contain return instructions");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 10: Empty Switch
// ============================================================================
TEST_FUNC(test_switch_empty) {
    TEST_START();

    // Given: Empty switch (should compile but do nothing)
    const char* source =
        "package main\n"
        "func test(x int) int {\n"
        "    switch x {\n"
        "    }\n"
        "    return 0;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle empty switch
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
    printf("\033[0;34m  TDD Cycle 10: Switch Statements\033[0m\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\n");

    RUN_TEST(test_basic_switch_int);
    RUN_TEST(test_switch_multiple_values);
    RUN_TEST(test_switch_default);
    RUN_TEST(test_switch_no_condition);
    RUN_TEST(test_switch_break);
    RUN_TEST(test_nested_switch);
    RUN_TEST(test_switch_variable_assignment);
    RUN_TEST(test_switch_expression_cases);
    RUN_TEST(test_switch_all_return);
    RUN_TEST(test_switch_empty);

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
