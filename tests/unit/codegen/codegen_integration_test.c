#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "types.h"
#include "codegen.h"

// Forward declarations for helper functions (implemented in test_codegen_helpers.c)
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

// ============================================================================
// Code Generation Integration Tests (TDD Cycle 5)
// ============================================================================
// These tests verify that the compiler can generate valid LLVM IR from
// Goo source code after successful parsing and type checking.
//
// Test Strategy:
// 1. Parse source code → AST
// 2. Type check AST → validated semantics
// 3. Generate LLVM IR → output
// 4. Verify IR contains expected patterns
// ============================================================================

// Helper function to compile Goo source to LLVM IR
static char* compile_to_llvm_ir(const char* source) {
    // Initialize lexer
    lexer_init(source, "test.goo");

    // Parse source code
    ASTNode* ast = parse_program();
    if (!ast) {
        fprintf(stderr, "Parse error\n");
        return NULL;
    }

    // Generate LLVM IR (codegen_generate will do type checking internally)
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

    // Get IR as string
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
// Test 1: Integer Literal Codegen
// ============================================================================
TEST_FUNC(test_codegen_integer_literal) {
    TEST_START();

    // Given: Source code with integer literal
    const char* source =
        "package main\n"
        "var x int = 42\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain constant 42
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "42"), "IR should contain integer literal 42");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 2: Binary Arithmetic Codegen
// ============================================================================
TEST_FUNC(test_codegen_binary_arithmetic) {
    TEST_START();

    // Given: Source code with arithmetic (using parameter to prevent constant folding)
    const char* source =
        "package main\n"
        "func calculate(x int) int {\n"
        "    return x + 5;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain add instruction
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "add"), "IR should contain add instruction");
    ASSERT_TRUE(ir_contains(ir, "i32"), "IR should use i32 type for int");
    ASSERT_TRUE(ir_contains(ir, "@calculate"), "IR should contain calculate function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 3: Simple Function Codegen
// ============================================================================
TEST_FUNC(test_codegen_simple_function) {
    TEST_START();

    // Given: Function definition
    const char* source =
        "package main\n"
        "func add(a int, b int) int {\n"
        "    return a + b;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain function definition
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "define"), "IR should contain function definition");
    ASSERT_TRUE(ir_contains(ir, "@add"), "IR should contain function name 'add'");
    ASSERT_TRUE(ir_contains(ir, "ret"), "IR should contain return statement");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 4: Function with Parameters
// ============================================================================
TEST_FUNC(test_codegen_function_parameters) {
    TEST_START();

    // Given: Function with multiple parameters
    const char* source =
        "package main\n"
        "func multiply(x int, y int) int {\n"
        "    return x * y;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain parameters
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "%x") || ir_contains(ir, "%0"),
                "IR should contain parameter x");
    ASSERT_TRUE(ir_contains(ir, "mul"), "IR should contain multiply instruction");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 5: Variable Declaration and Usage
// ============================================================================
TEST_FUNC(test_codegen_variable_declaration) {
    TEST_START();

    // Given: Variable declaration and usage
    const char* source =
        "package main\n"
        "func test() int {\n"
        "    var result int = 100;\n"
        "    return result;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain alloca and store
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "alloca") || ir_contains(ir, "store"),
                "IR should contain variable storage");
    ASSERT_TRUE(ir_contains(ir, "100"), "IR should contain initial value");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 6: If Statement Codegen
// ============================================================================
TEST_FUNC(test_codegen_if_statement) {
    TEST_START();

    // Given: If statement
    const char* source =
        "package main\n"
        "func test(x int) int {\n"
        "    if x > 0 {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain branch instructions
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "icmp") || ir_contains(ir, "cmp"),
                "IR should contain comparison");
    ASSERT_TRUE(ir_contains(ir, "br"), "IR should contain branch instruction");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 7: Boolean Expression Codegen
// ============================================================================
TEST_FUNC(test_codegen_boolean_expression) {
    TEST_START();

    // Given: Boolean expression
    const char* source =
        "package main\n"
        "func isPositive(x int) bool {\n"
        "    return x > 0;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain comparison
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "icmp"), "IR should contain integer comparison");
    ASSERT_TRUE(ir_contains(ir, "i1") || ir_contains(ir, "i8"),
                "IR should use boolean type");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 8: String Literal Codegen
// ============================================================================
TEST_FUNC(test_codegen_string_literal) {
    TEST_START();

    // Given: String literal
    const char* source =
        "package main\n"
        "var message string = \"hello\"\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain string constant
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "hello") || ir_contains(ir, "constant"),
                "IR should contain string literal");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 9: Multiple Functions
// ============================================================================
TEST_FUNC(test_codegen_multiple_functions) {
    TEST_START();

    // Given: Multiple function definitions
    const char* source =
        "package main\n"
        "func add(a int, b int) int {\n"
        "    return a + b;\n"
        "}\n"
        "func subtract(a int, b int) int {\n"
        "    return a - b;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain both functions
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@add"), "IR should contain 'add' function");
    ASSERT_TRUE(ir_contains(ir, "@subtract"), "IR should contain 'subtract' function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 10: Error Union Type (Advanced)
// ============================================================================
TEST_FUNC(test_codegen_error_union) {
    TEST_START();

    // Given: Function with error union return type (simplified - just return success case)
    const char* source =
        "package main\n"
        "func divide(a int, b int) !int {\n"
        "    return a / b;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain error union function
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@divide"), "IR should contain divide function");
    ASSERT_TRUE(ir_contains(ir, "define"), "IR should contain function definition");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main() {
    printf("\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\033[0;34m  Code Generation Integration Tests (TDD)\033[0m\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\n");

    int passed = 0;
    int total = 0;

    RUN_TEST(test_codegen_integer_literal);
    RUN_TEST(test_codegen_binary_arithmetic);
    RUN_TEST(test_codegen_simple_function);
    RUN_TEST(test_codegen_function_parameters);
    RUN_TEST(test_codegen_variable_declaration);
    RUN_TEST(test_codegen_if_statement);
    RUN_TEST(test_codegen_boolean_expression);
    RUN_TEST(test_codegen_string_literal);
    RUN_TEST(test_codegen_multiple_functions);
    RUN_TEST(test_codegen_error_union);

    printf("\n");
    printf("\033[0;34m================================\033[0m\n");
    printf("\033[0;34m  Test Results\033[0m\n");
    printf("\033[0;34m================================\033[0m\n");
    printf("  Total:   %d\n", total);
    printf("  \033[0;32mPassed:  %d\033[0m\n", passed);
    printf("  \033[0;31mFailed:  %d\033[0m\n", total - passed);
    if (total > 0) {
        printf("  Pass Rate: %d%%\n", (passed * 100) / total);
    }
    printf("\n");

    if (passed == total && total > 0) {
        printf("\033[0;32m✓ All tests passed!\033[0m\n");
        printf("\n");
        return 0;
    } else {
        printf("\033[0;33m⚠ No tests run\033[0m\n");
        printf("\n");
        return 1;
    }
}
