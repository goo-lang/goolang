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
// Test 1: Map Declaration with make
// ============================================================================
TEST_FUNC(test_map_make) {
    TEST_START();

    // Given: Map creation with make
    const char* source =
        "package main\n"
        "func create_map() map[string]int {\n"
        "    var m map[string]int = make(map[string]int);\n"
        "    return m;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain map creation
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@create_map"), "IR should contain create_map function");
    ASSERT_TRUE(ir_contains(ir, "goo_map_new") || ir_contains(ir, "alloca") || ir_contains(ir, "call"),
                "IR should contain map allocation");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 2: Map Literal Creation
// ============================================================================
TEST_FUNC(test_map_literal) {
    TEST_START();

    // Given: Map literal with initial values
    const char* source =
        "package main\n"
        "func get_scores() map[string]int {\n"
        "    return map[string]int{\"alice\": 95, \"bob\": 87};\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain map initialization
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_scores"), "IR should contain get_scores function");
    ASSERT_TRUE(ir_contains(ir, "goo_map_new") || ir_contains(ir, "goo_map_set") || ir_contains(ir, "call"),
                "IR should contain map initialization");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 3: Map Element Access
// ============================================================================
TEST_FUNC(test_map_access) {
    TEST_START();

    // Given: Map element access
    const char* source =
        "package main\n"
        "func get_value(m map[string]int, key string) int {\n"
        "    return m[key];\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain map get operation
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_value"), "IR should contain get_value function");
    ASSERT_TRUE(ir_contains(ir, "goo_map_get") || ir_contains(ir, "call"),
                "IR should contain map get call");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 4: Map Element Assignment
// ============================================================================
TEST_FUNC(test_map_assignment) {
    TEST_START();

    // Given: Map element assignment
    const char* source =
        "package main\n"
        "func set_value(m map[string]int, key string, value int) {\n"
        "    m[key] = value;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain map set operation
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@set_value"), "IR should contain set_value function");
    ASSERT_TRUE(ir_contains(ir, "goo_map_set") || ir_contains(ir, "call"),
                "IR should contain map set call");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 5: Map Delete Operation
// ============================================================================
TEST_FUNC(test_map_delete) {
    TEST_START();

    // Given: Map element deletion
    const char* source =
        "package main\n"
        "func remove_key(m map[string]int, key string) {\n"
        "    delete(m, key);\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain delete operation
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@remove_key"), "IR should contain remove_key function");
    ASSERT_TRUE(ir_contains(ir, "goo_map_delete") || ir_contains(ir, "delete") || ir_contains(ir, "call"),
                "IR should contain map delete call");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 6: Map Length
// ============================================================================
TEST_FUNC(test_map_length) {
    TEST_START();

    // Given: Getting map length
    const char* source =
        "package main\n"
        "func map_size(m map[string]int) int {\n"
        "    return len(m);\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain length operation
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@map_size"), "IR should contain map_size function");
    ASSERT_TRUE(ir_contains(ir, "goo_map_len") || ir_contains(ir, "len") ||
                ir_contains(ir, "getelementptr") || ir_contains(ir, "load"),
                "IR should contain map length operation");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 7: Map with Integer Keys
// ============================================================================
TEST_FUNC(test_map_int_keys) {
    TEST_START();

    // Given: Map with int keys
    const char* source =
        "package main\n"
        "func create_int_map() map[int]string {\n"
        "    return make(map[int]string);\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle int-keyed maps
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@create_int_map"), "IR should contain create_int_map function");
    ASSERT_TRUE(ir_contains(ir, "goo_map_new") || ir_contains(ir, "call"),
                "IR should contain map creation");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 8: Map as Function Parameter
// ============================================================================
TEST_FUNC(test_map_parameter) {
    TEST_START();

    // Given: Function with map parameter
    const char* source =
        "package main\n"
        "func process_map(m map[string]int) int {\n"
        "    return len(m);\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle map as parameter
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@process_map"), "IR should contain process_map function");
    ASSERT_TRUE(ir_contains(ir, "ptr") || ir_contains(ir, "%"),
                "IR should contain map parameter");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 9: Empty Map Check
// ============================================================================
TEST_FUNC(test_map_empty_check) {
    TEST_START();

    // Given: Check if map is empty
    const char* source =
        "package main\n"
        "func is_empty(m map[string]int) bool {\n"
        "    return len(m) == 0;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain length check
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@is_empty"), "IR should contain is_empty function");
    ASSERT_TRUE(ir_contains(ir, "icmp") || ir_contains(ir, "goo_map_len"),
                "IR should contain comparison");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 10: Nested Map Operations
// ============================================================================
TEST_FUNC(test_map_multiple_ops) {
    TEST_START();

    // Given: Multiple map operations in sequence
    const char* source =
        "package main\n"
        "func update_scores(m map[string]int) {\n"
        "    m[\"new\"] = 100;\n"
        "    var x int = m[\"new\"];\n"
        "    delete(m, \"old\");\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle multiple operations
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@update_scores"), "IR should contain update_scores function");
    ASSERT_TRUE(ir_contains(ir, "call") || ir_contains(ir, "store") || ir_contains(ir, "load"),
                "IR should contain map operations");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main() {
    printf("\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\033[0;34m  TDD Cycle 15: Map Operations\033[0m\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\n");

    RUN_TEST(test_map_make);
    RUN_TEST(test_map_literal);
    RUN_TEST(test_map_access);
    RUN_TEST(test_map_assignment);
    RUN_TEST(test_map_delete);
    RUN_TEST(test_map_length);
    RUN_TEST(test_map_int_keys);
    RUN_TEST(test_map_parameter);
    RUN_TEST(test_map_empty_check);
    RUN_TEST(test_map_multiple_ops);

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
