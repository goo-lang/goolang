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
// Test 1: Simple Function Literal
// ============================================================================
TEST_FUNC(test_simple_function_literal) {
    TEST_START();

    // Given: Simple function literal assigned to variable
    const char* source =
        "package main\n"
        "func main() {\n"
        "    var add func(int, int) int = func(a int, b int) int {\n"
        "        return a + b;\n"
        "    };\n"
        "    var result int = add(2, 3);\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile successfully
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@main"), "IR should contain main function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 2: Function Literal as Argument
// ============================================================================
TEST_FUNC(test_function_literal_as_argument) {
    TEST_START();

    // Given: Function literal passed as argument
    const char* source =
        "package main\n"
        "func apply(f func(int) int, x int) int {\n"
        "    return f(x);\n"
        "}\n"
        "func main() {\n"
        "    var result int = apply(func(n int) int {\n"
        "        return n * 2;\n"
        "    }, 5);\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile successfully
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@apply"), "IR should contain apply function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 3: Function Literal with Closure
// ============================================================================
TEST_FUNC(test_function_literal_with_closure) {
    TEST_START();

    // Given: Function literal that captures outer variable
    const char* source =
        "package main\n"
        "func makeAdder(x int) func(int) int {\n"
        "    return func(y int) int {\n"
        "        return x + y;\n"
        "    };\n"
        "}\n"
        "func main() {\n"
        "    var add5 func(int) int = makeAdder(5);\n"
        "    var result int = add5(3);\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile successfully
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@makeAdder"), "IR should contain makeAdder function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 4: Immediately Invoked Function Literal
// ============================================================================
TEST_FUNC(test_immediately_invoked_function_literal) {
    TEST_START();

    // Given: Function literal invoked immediately
    const char* source =
        "package main\n"
        "func main() {\n"
        "    var result int = func() int {\n"
        "        return 42;\n"
        "    }();\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile successfully
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@main"), "IR should contain main function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 5: Function Literal Returning Function
// ============================================================================
TEST_FUNC(test_function_literal_returning_function) {
    TEST_START();

    // Given: Function returning another function literal
    const char* source =
        "package main\n"
        "func multiplier(factor int) func(int) int {\n"
        "    return func(x int) int {\n"
        "        return x * factor;\n"
        "    };\n"
        "}\n"
        "func main() {\n"
        "    var double func(int) int = multiplier(2);\n"
        "    var result int = double(21);\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile successfully
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@multiplier"), "IR should contain multiplier function");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main() {
    printf("\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\033[0;34m  TDD Cycle 19: Function Literals/Closures\033[0m\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\n");

    RUN_TEST(test_simple_function_literal);
    RUN_TEST(test_function_literal_as_argument);
    RUN_TEST(test_function_literal_with_closure);
    RUN_TEST(test_immediately_invoked_function_literal);
    RUN_TEST(test_function_literal_returning_function);

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
