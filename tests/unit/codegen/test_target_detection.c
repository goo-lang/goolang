#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Unit tests for target architecture detection in codegen
#include "../../../include/codegen.h"
#include "../../../include/types.h"

#if LLVM_AVAILABLE
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#endif

void test_target_detection() {
    printf("Testing target architecture detection...\n");
    
#if LLVM_AVAILABLE
    // Initialize LLVM
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    
    // Get default target triple
    char* default_triple = LLVMGetDefaultTargetTriple();
    printf("Default target triple: %s\n", default_triple);
    
    // Check if we're on the expected architecture
    if (strstr(default_triple, "arm64") || strstr(default_triple, "aarch64")) {
        printf("✓ Detected ARM64 architecture\n");
    } else if (strstr(default_triple, "x86_64")) {
        printf("✓ Detected x86_64 architecture\n");
    } else {
        printf("? Unknown architecture: %s\n", default_triple);
    }
    
    // Test target creation
    LLVMTargetRef target;
    char* error_message = NULL;
    
    if (LLVMGetTargetFromTriple(default_triple, &target, &error_message)) {
        printf("❌ Failed to get target: %s\n", error_message);
        LLVMDisposeMessage(error_message);
    } else {
        printf("✓ Successfully created target\n");
        
        // Create target machine
        LLVMTargetMachineRef target_machine = LLVMCreateTargetMachine(
            target, default_triple, "generic", "",
            LLVMCodeGenLevelDefault, LLVMRelocDefault, LLVMCodeModelDefault
        );
        
        if (target_machine) {
            printf("✓ Successfully created target machine\n");
            
            // Get data layout
            LLVMTargetDataRef target_data = LLVMCreateTargetDataLayout(target_machine);
            char* data_layout = LLVMCopyStringRepOfTargetData(target_data);
            printf("Data layout: %s\n", data_layout);
            
            LLVMDisposeMessage(data_layout);
            LLVMDisposeTargetData(target_data);
            LLVMDisposeTargetMachine(target_machine);
        } else {
            printf("❌ Failed to create target machine\n");
        }
    }
    
    LLVMDisposeMessage(default_triple);
#else
    printf("⚠️ LLVM not available - skipping target detection test\n");
#endif
}

void test_codegen_initialization() {
    printf("\nTesting codegen initialization...\n");
    
    CodeGenerator* codegen = codegen_new("test_module");
    if (!codegen) {
        printf("❌ Failed to create code generator\n");
        return;
    }
    
    printf("✓ Code generator created successfully\n");
    
    // Test target initialization
    if (codegen_initialize_target(codegen)) {
        printf("✓ Target initialized successfully\n");
    } else {
        printf("❌ Target initialization failed\n");
    }
    
    codegen_free(codegen);
}

void test_object_generation() {
    printf("\nTesting object file generation...\n");
    
    CodeGenerator* codegen = codegen_new("test_module");
    if (!codegen) {
        printf("❌ Failed to create code generator\n");
        return;
    }
    
    if (!codegen_initialize_target(codegen)) {
        printf("❌ Target initialization failed\n");
        codegen_free(codegen);
        return;
    }
    
#if LLVM_AVAILABLE
    // Create a simple function for testing
    LLVMTypeRef void_type = LLVMVoidTypeInContext(codegen->context);
    LLVMTypeRef func_type = LLVMFunctionType(void_type, NULL, 0, 0);
    LLVMValueRef main_func = LLVMAddFunction(codegen->module, "main", func_type);
    
    // Create entry block
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(codegen->context, main_func, "entry");
    LLVMPositionBuilderAtEnd(codegen->builder, entry);
    LLVMBuildRetVoid(codegen->builder);
    
    // Verify module
    if (codegen_verify_module(codegen)) {
        printf("✓ Module verification passed\n");
        
        // Try to generate object file
        if (codegen_emit_object_file(codegen, "tests/test_output.o")) {
            printf("✓ Object file generation succeeded\n");
            
            // Check if file exists
            FILE* f = fopen("tests/test_output.o", "r");
            if (f) {
                printf("✓ Object file created successfully\n");
                fclose(f);
                remove("tests/test_output.o");
            } else {
                printf("❌ Object file not found\n");
            }
        } else {
            printf("❌ Object file generation failed\n");
        }
    } else {
        printf("❌ Module verification failed\n");
    }
#endif
    
    codegen_free(codegen);
}

int main() {
    printf("=== Code Generation Target Tests ===\n");
    
    test_target_detection();
    test_codegen_initialization();
    test_object_generation();
    
    printf("\n=== Target Detection Tests Complete ===\n");
    return 0;
}