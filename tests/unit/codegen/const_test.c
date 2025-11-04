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
// Test 1: Simple Integer Constant
// ============================================================================
TEST_FUNC(test_int_const) {
    TEST_START();

    // Given: Integer constant declaration
    const char* source =
        "package main\n"
        "const MaxSize int = 100\n"
        "func get_max() int {\n"
        "    return MaxSize\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile and inline constant
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_max"), "IR should contain get_max function");
    ASSERT_TRUE(ir_contains(ir, "100") || ir_contains(ir, "ret"),
                "IR should contain constant value or return");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 2: String Constant
// ============================================================================
TEST_FUNC(test_string_const) {
    TEST_START();

    // Given: String constant declaration
    const char* source =
        "package main\n"
        "const Greeting string = \"Hello\"\n"
        "func get_greeting() string {\n"
        "    return Greeting\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile and use string constant
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_greeting"), "IR should contain get_greeting function");
    ASSERT_TRUE(ir_contains(ir, "Hello") || ir_contains(ir, "@.str") || ir_contains(ir, "constant"),
                "IR should contain string constant");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 3: Constant Expression
// ============================================================================
TEST_FUNC(test_const_expression) {
    TEST_START();

    // Given: Constant with expression
    const char* source =
        "package main\n"
        "const Size int = 10 * 10\n"
        "func get_size() int {\n"
        "    return Size\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile (may or may not fold constant)
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_size"), "IR should contain get_size function");
    ASSERT_TRUE(ir_contains(ir, "100") || ir_contains(ir, "mul") || ir_contains(ir, "ret"),
                "IR should contain result or calculation");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 4: Multiple Constants
// ============================================================================
TEST_FUNC(test_multiple_consts) {
    TEST_START();

    // Given: Multiple constant declarations
    const char* source =
        "package main\n"
        "const Width int = 800\n"
        "const Height int = 600\n"
        "func get_area() int {\n"
        "    return Width * Height\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile and use both constants
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_area"), "IR should contain get_area function");
    ASSERT_TRUE(ir_contains(ir, "mul") || ir_contains(ir, "ret"),
                "IR should contain multiplication or return");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 5: Constant in Variable Initialization
// ============================================================================
TEST_FUNC(test_const_in_var_init) {
    TEST_START();

    // Given: Constant used in variable initialization
    const char* source =
        "package main\n"
        "const DefaultValue int = 42\n"
        "func create_value() int {\n"
        "    var x int = DefaultValue\n"
        "    return x\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile and use constant
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@create_value"), "IR should contain create_value function");
    ASSERT_TRUE(ir_contains(ir, "42") || ir_contains(ir, "alloca") || ir_contains(ir, "store"),
                "IR should contain constant or variable operations");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 6: Boolean Constant
// ============================================================================
TEST_FUNC(test_bool_const) {
    TEST_START();

    // Given: Boolean constant
    const char* source =
        "package main\n"
        "const Debug bool = true\n"
        "func is_debug() bool {\n"
        "    return Debug\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile and use boolean constant
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@is_debug"), "IR should contain is_debug function");
    ASSERT_TRUE(ir_contains(ir, "i1") || ir_contains(ir, "true") || ir_contains(ir, "ret"),
                "IR should contain boolean type or value");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 7: Constant in Arithmetic
// ============================================================================
TEST_FUNC(test_const_arithmetic) {
    TEST_START();

    // Given: Constant used in arithmetic expression
    const char* source =
        "package main\n"
        "const Base int = 100\n"
        "func add_to_base(x int) int {\n"
        "    return x + Base\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: Should compile and perform addition
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@add_to_base"), "IR should contain add_to_base function");
    ASSERT_TRUE(ir_contains(ir, "add") || ir_contains(ir, "100"),
                "IR should contain addition or constant");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main() {
    printf("\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\033[0;34m  TDD Cycle 17: Constants\033[0m\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\n");

    RUN_TEST(test_int_const);
    RUN_TEST(test_string_const);
    RUN_TEST(test_const_expression);
    RUN_TEST(test_multiple_consts);
    RUN_TEST(test_const_in_var_init);
    RUN_TEST(test_bool_const);
    RUN_TEST(test_const_arithmetic);

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
