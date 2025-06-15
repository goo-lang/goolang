#include "codegen.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

// Forward declarations
#if LLVM_AVAILABLE
void test_wasm_codegen(void);
#else
void test_wasm_codegen(void);
#endif

// Simple test suite for the code generator

void test_codegen_creation(void) {
    printf("Testing code generator creation...\n");
    
    CodeGenerator* codegen = codegen_new("test_module");
    assert(codegen != NULL);
    
#if LLVM_AVAILABLE
    assert(codegen->context != NULL);
    assert(codegen->module != NULL);
    assert(codegen->builder != NULL);
    assert(codegen->error_count == 0);
#else
    assert(codegen->llvm_unavailable == 1);
#endif
    
    codegen_free(codegen);
    
    printf("Code generator creation test passed!\n");
}

void test_target_initialization(void) {
    printf("Testing target initialization...\n");
    
    CodeGenerator* codegen = codegen_new("test_module");
    assert(codegen != NULL);
    
    int result = codegen_initialize_target(codegen);
#if LLVM_AVAILABLE
    assert(result == 1);
#else
    assert(result == 0);
#endif
    
    codegen_free(codegen);
    
    printf("Target initialization test passed!\n");
}

#if LLVM_AVAILABLE
void test_basic_type_mapping(void) {
    printf("Testing basic type mapping...\n");
    
    CodeGenerator* codegen = codegen_new("test_module");
    assert(codegen != NULL);
    
    // Test basic types
    LLVMTypeRef void_type = codegen_get_basic_type(codegen, TYPE_VOID);
    assert(void_type != NULL);
    assert(LLVMGetTypeKind(void_type) == LLVMVoidTypeKind);
    
    LLVMTypeRef bool_type = codegen_get_basic_type(codegen, TYPE_BOOL);
    assert(bool_type != NULL);
    assert(LLVMGetTypeKind(bool_type) == LLVMIntegerTypeKind);
    assert(LLVMGetIntTypeWidth(bool_type) == 1);
    
    LLVMTypeRef int32_type = codegen_get_basic_type(codegen, TYPE_INT32);
    assert(int32_type != NULL);
    assert(LLVMGetTypeKind(int32_type) == LLVMIntegerTypeKind);
    assert(LLVMGetIntTypeWidth(int32_type) == 32);
    
    LLVMTypeRef float64_type = codegen_get_basic_type(codegen, TYPE_FLOAT64);
    assert(float64_type != NULL);
    assert(LLVMGetTypeKind(float64_type) == LLVMDoubleTypeKind);
    
    codegen_free(codegen);
    
    printf("Basic type mapping test passed!\n");
}

void test_value_management(void) {
    printf("Testing value management...\n");
    
    CodeGenerator* codegen = codegen_new("test_module");
    assert(codegen != NULL);
    
    // Create a test value
    LLVMTypeRef int_type = codegen_get_basic_type(codegen, TYPE_INT32);
    LLVMValueRef int_val = LLVMConstInt(int_type, 42, 0);
    
    ValueInfo* value_info = value_info_new("test_var", int_val, NULL);
    assert(value_info != NULL);
    assert(strcmp(value_info->name, "test_var") == 0);
    assert(value_info->llvm_value == int_val);
    
    // Add to code generator
    assert(codegen_add_value(codegen, value_info) == 1);
    
    // Look up the value
    ValueInfo* found = codegen_lookup_value(codegen, "test_var");
    assert(found == value_info);
    
    // Try to look up non-existent value
    ValueInfo* not_found = codegen_lookup_value(codegen, "nonexistent");
    assert(not_found == NULL);
    
    codegen_free(codegen);
    
    printf("Value management test passed!\n");
}
#endif

void run_codegen_tests(void) {
    printf("Running code generator tests...\n\n");
    
    test_codegen_creation();
    test_target_initialization();
    
#if LLVM_AVAILABLE
    test_basic_type_mapping();
    test_value_management();
    test_wasm_codegen();
#else
    printf("LLVM not available - skipping LLVM-specific tests\n");
#endif
    
    printf("\nAll code generator tests passed!\n");
}

#if LLVM_AVAILABLE
void test_wasm_codegen(void) {
    printf("Running WebAssembly code generation tests...\n");
    
    CodeGenerator* codegen = codegen_new("wasm_test_module");
    assert(codegen != NULL);
    
    // Set WebAssembly target
    int result = codegen_set_target(codegen, "wasm32-unknown-unknown", NULL, NULL);
    assert(result == 1);
    
    // Initialize target
    result = codegen_initialize_target(codegen);
    assert(result == 1);
    
    // Test WASM target detection
    assert(codegen_is_wasm_target(codegen) == 1);
    
    // Test WASM export function
    LLVMTypeRef void_type = LLVMVoidTypeInContext(codegen->context);
    LLVMTypeRef func_type = LLVMFunctionType(void_type, NULL, 0, 0);
    LLVMValueRef test_func = LLVMAddFunction(codegen->module, "test_export", func_type);
    
    result = codegen_add_wasm_export(codegen, test_func, "test_export");
    assert(result == 1);
    
    // Test WASM import function
    LLVMValueRef import_func = LLVMAddFunction(codegen->module, "test_import", func_type);
    result = codegen_add_wasm_import(codegen, import_func, "env", "test_import");
    assert(result == 1);
    
    // Test WASM runtime functions declaration
    result = codegen_declare_wasm_runtime_functions(codegen);
    assert(result == 1);
    
    // Test WASM concurrency configuration
    result = codegen_configure_wasm_concurrency(codegen);
    assert(result == 1);
    
    printf("WebAssembly code generation test passed!\n");
    
    codegen_free(codegen);
}
#else
void test_wasm_codegen(void) {
    printf("LLVM not available - skipping WebAssembly tests\n");
}
#endif

// Function to be called from main
int test_codegen(void) {
    run_codegen_tests();
    test_wasm_codegen();
    return 0;
}