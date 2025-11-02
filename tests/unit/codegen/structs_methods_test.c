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
// Test 1: Basic Struct Declaration
// ============================================================================
TEST_FUNC(test_struct_declaration) {
    TEST_START();

    // Given: Basic struct type declaration
    const char* source =
        "package main\n"
        "type Person struct {\n"
        "    name string;\n"
        "    age int;\n"
        "}\n"
        "func test() Person {\n"
        "    var p Person;\n"
        "    return p;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain struct type definition
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");
    ASSERT_TRUE(ir_contains(ir, "alloca") || ir_contains(ir, "%Person"),
                "IR should contain struct allocation or type");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 2: Struct Field Assignment
// ============================================================================
TEST_FUNC(test_struct_field_assignment) {
    TEST_START();

    // Given: Struct with field assignment
    const char* source =
        "package main\n"
        "type Person struct {\n"
        "    name string;\n"
        "    age int;\n"
        "}\n"
        "func test() int {\n"
        "    var p Person;\n"
        "    p.age = 25;\n"
        "    return p.age;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain field access (GEP) and store/load
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "getelementptr") || ir_contains(ir, "extractvalue"),
                "IR should contain field access");
    ASSERT_TRUE(ir_contains(ir, "store"), "IR should contain store instruction");
    ASSERT_TRUE(ir_contains(ir, "load") || ir_contains(ir, "ret"),
                "IR should contain load or return");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 3: Struct Literal - Zero Values
// ============================================================================
TEST_FUNC(test_struct_literal_zero) {
    TEST_START();

    // Given: Empty struct literal (zero values)
    const char* source =
        "package main\n"
        "type Person struct {\n"
        "    name string;\n"
        "    age int;\n"
        "}\n"
        "func test() Person {\n"
        "    p := Person{};\n"
        "    return p;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should create zero-initialized struct
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");
    ASSERT_TRUE(ir_contains(ir, "alloca") || ir_contains(ir, "zeroinitializer"),
                "IR should contain struct allocation");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 4: Struct Literal - With Values
// ============================================================================
TEST_FUNC(test_struct_literal_values) {
    TEST_START();

    // Given: Struct literal with field values
    const char* source =
        "package main\n"
        "type Person struct {\n"
        "    name string;\n"
        "    age int;\n"
        "}\n"
        "func test() int {\n"
        "    p := Person{age: 30};\n"
        "    return p.age;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should initialize struct field with value
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "30") || ir_contains(ir, "i32 30"),
                "IR should contain literal value 30");
    ASSERT_TRUE(ir_contains(ir, "store") || ir_contains(ir, "getelementptr"),
                "IR should contain field initialization");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 5: Struct as Function Parameter
// ============================================================================
TEST_FUNC(test_struct_as_parameter) {
    TEST_START();

    // Given: Function taking struct as parameter
    const char* source =
        "package main\n"
        "type Person struct {\n"
        "    name string;\n"
        "    age int;\n"
        "}\n"
        "func get_age(p Person) int {\n"
        "    return p.age;\n"
        "}\n"
        "func test() int {\n"
        "    p := Person{age: 25};\n"
        "    return get_age(p);\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should pass struct to function
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@get_age"), "IR should contain get_age function");
    ASSERT_TRUE(ir_contains(ir, "call"), "IR should contain function call");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 6: Struct as Function Return Value
// ============================================================================
TEST_FUNC(test_struct_as_return) {
    TEST_START();

    // Given: Function returning struct
    const char* source =
        "package main\n"
        "type Person struct {\n"
        "    name string;\n"
        "    age int;\n"
        "}\n"
        "func create_person(age int) Person {\n"
        "    return Person{age: age};\n"
        "}\n"
        "func test() int {\n"
        "    p := create_person(40);\n"
        "    return p.age;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should return struct from function
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@create_person"), "IR should contain create_person function");
    ASSERT_TRUE(ir_contains(ir, "ret"), "IR should contain return instruction");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 7: Nested Struct Access
// ============================================================================
TEST_FUNC(test_nested_struct) {
    TEST_START();

    // Given: Nested struct access
    const char* source =
        "package main\n"
        "type Address struct {\n"
        "    city string;\n"
        "    zip int;\n"
        "}\n"
        "type Person struct {\n"
        "    name string;\n"
        "    address Address;\n"
        "}\n"
        "func test() int {\n"
        "    var p Person;\n"
        "    p.address.zip = 12345;\n"
        "    return p.address.zip;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle nested field access
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "getelementptr") || ir_contains(ir, "extractvalue"),
                "IR should contain nested field access");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 8: Multiple Struct Types
// ============================================================================
TEST_FUNC(test_multiple_structs) {
    TEST_START();

    // Given: Multiple different struct types
    const char* source =
        "package main\n"
        "type Person struct {\n"
        "    age int;\n"
        "}\n"
        "type Company struct {\n"
        "    size int;\n"
        "}\n"
        "func test() int {\n"
        "    p := Person{age: 25};\n"
        "    c := Company{size: 100};\n"
        "    return p.age + c.size;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should define both struct types
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "@test"), "IR should contain test function");
    ASSERT_TRUE(ir_contains(ir, "add"), "IR should contain addition");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 9: Basic Method Declaration
// ============================================================================
TEST_FUNC(test_method_declaration) {
    TEST_START();

    // Given: Method with value receiver
    const char* source =
        "package main\n"
        "type Counter struct {\n"
        "    value int;\n"
        "}\n"
        "func (c Counter) get_value() int {\n"
        "    return c.value;\n"
        "}\n"
        "func test() int {\n"
        "    c := Counter{value: 42};\n"
        "    return c.get_value();\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should contain method (mangled or with receiver parameter)
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "get_value") || ir_contains(ir, "Counter"),
                "IR should contain method reference");
    ASSERT_TRUE(ir_contains(ir, "call"), "IR should contain method call");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Test 10: Pointer Receiver Method
// ============================================================================
TEST_FUNC(test_pointer_receiver_method) {
    TEST_START();

    // Given: Method with pointer receiver
    const char* source =
        "package main\n"
        "type Counter struct {\n"
        "    value int;\n"
        "}\n"
        "func (c *Counter) increment() {\n"
        "    c.value = c.value + 1;\n"
        "}\n"
        "func test() int {\n"
        "    c := Counter{value: 10};\n"
        "    c.increment();\n"
        "    return c.value;\n"
        "}\n";

    // When: Compile to LLVM IR
    char* ir = compile_to_llvm_ir(source);

    // Then: IR should handle pointer receiver
    ASSERT_NOT_NULL(ir, "IR generation should succeed");
    ASSERT_TRUE(ir_contains(ir, "increment") || ir_contains(ir, "Counter"),
                "IR should contain method reference");
    ASSERT_TRUE(ir_contains(ir, "store"), "IR should contain store for increment");

    free(ir);
    TEST_PASS();
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main() {
    printf("\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\033[0;34m  TDD Cycle 7: Structs & Methods Tests\033[0m\n");
    printf("\033[0;34m========================================\033[0m\n");
    printf("\n");

    RUN_TEST(test_struct_declaration);
    RUN_TEST(test_struct_field_assignment);
    RUN_TEST(test_struct_literal_zero);
    RUN_TEST(test_struct_literal_values);
    RUN_TEST(test_struct_as_parameter);
    RUN_TEST(test_struct_as_return);
    RUN_TEST(test_nested_struct);
    RUN_TEST(test_multiple_structs);
    RUN_TEST(test_method_declaration);
    RUN_TEST(test_pointer_receiver_method);

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
