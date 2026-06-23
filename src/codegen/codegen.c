#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Code generator initialization and cleanup

// WebAssembly target configuration
#if LLVM_AVAILABLE
static int codegen_configure_wasm_target(CodeGenerator* codegen) {
    if (!codegen) return 0;
    
    // Configure WebAssembly-specific settings
    // For example, set the correct calling convention, enable SIMD, etc.
    // This is a placeholder for actual WebAssembly target configuration
    Position pos = {0, 0, 0, "codegen"};
    codegen_warning(codegen, pos, "Configuring WebAssembly target - this is a placeholder");
    return 1;
}
#endif

CodeGenerator* codegen_new(const char* module_name __attribute__((unused))) {
#if LLVM_AVAILABLE
    CodeGenerator* codegen = malloc(sizeof(CodeGenerator));
    if (!codegen) return NULL;
    
    // Initialize LLVM core
    codegen->context = LLVMContextCreate();
    codegen->module = LLVMModuleCreateWithNameInContext(module_name, codegen->context);
    codegen->builder = LLVMCreateBuilderInContext(codegen->context);
    
    codegen->target_machine = NULL;
    codegen->current_function = NULL;
    codegen->current_function_info = NULL;
    
    // Initialize symbol tables
    codegen->value_table = NULL;
    codegen->value_table_size = 0;
    codegen->value_table_capacity = 0;
    
    codegen->type_cache = NULL;
    codegen->type_cache_size = 0;
    codegen->type_cache_capacity = 0;
    
    // Error reporting
    codegen->current_file = NULL;
    codegen->error_count = 0;
    codegen->warning_count = 0;
    
    // Target information
    codegen->target_triple = NULL;
    codegen->target_cpu = NULL;
    codegen->target_features = NULL;
    
    // WebAssembly configuration
    codegen->wasm_configured = 0;
    codegen->is_wasm_target = 0;
    
    // Declare runtime functions
    codegen_declare_runtime_functions(codegen);
    
    // Configure WebAssembly-specific features if targeting WASM
    if (codegen->target_triple && strstr(codegen->target_triple, "wasm32")) {
        codegen->is_wasm_target = 1;
        codegen_declare_wasm_runtime_functions(codegen);
        codegen_configure_wasm_concurrency(codegen);
    }
    
    return codegen;
#else
    CodeGenerator* codegen = malloc(sizeof(CodeGenerator));
    if (!codegen) return NULL;
    
    codegen->error_message = strdup("LLVM support not available in this build");
    codegen->llvm_unavailable = 1;
    codegen->current_file = NULL;
    codegen->error_count = 0;
    codegen->warning_count = 0;
    return codegen;
#endif
}

void codegen_free(CodeGenerator* codegen) {
    if (!codegen) return;
    
#if LLVM_AVAILABLE
    if (codegen->builder) {
        LLVMDisposeBuilder(codegen->builder);
    }
    if (codegen->module) {
        LLVMDisposeModule(codegen->module);
    }
    if (codegen->context) {
        LLVMContextDispose(codegen->context);
    }
    if (codegen->target_machine) {
        LLVMDisposeTargetMachine(codegen->target_machine);
    }
    
    // Free symbol tables
    if (codegen->value_table) {
        for (size_t i = 0; i < codegen->value_table_size; i++) {
            if (codegen->value_table[i]) {
                value_info_free(codegen->value_table[i]);
            }
        }
        free(codegen->value_table);
    }
    
    if (codegen->type_cache) {
        free(codegen->type_cache);
    }
    
    free(codegen->current_file);
    free(codegen->target_triple);
    free(codegen->target_cpu);
    free(codegen->target_features);
#else
    free(codegen->error_message);
    free(codegen->current_file);
#endif
    
    free(codegen);
}

int codegen_set_target(CodeGenerator* codegen __attribute__((unused)), const char* triple __attribute__((unused)), const char* cpu __attribute__((unused)), const char* features __attribute__((unused))) {
    if (!codegen) return 0;
    
#if LLVM_AVAILABLE
    if (triple) {
        free(codegen->target_triple);
        codegen->target_triple = strdup(triple);
    }
    if (cpu) {
        free(codegen->target_cpu);
        codegen->target_cpu = strdup(cpu);
    }
    if (features) {
        free(codegen->target_features);
        codegen->target_features = strdup(features);
    }
    return 1;
#else
    return 0;
#endif
}

int codegen_initialize_target(CodeGenerator* codegen) {
    if (!codegen) return 0;
    
#if LLVM_AVAILABLE
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    
    // Initialize WebAssembly target
    LLVMInitializeWebAssemblyTarget();
    LLVMInitializeWebAssemblyTargetInfo();
    LLVMInitializeWebAssemblyTargetMC();
    LLVMInitializeWebAssemblyAsmPrinter();
    
    // Set target triple for module
    if (codegen->target_triple) {
        LLVMSetTarget(codegen->module, codegen->target_triple);
        
        // Configure WebAssembly-specific features
        if (strstr(codegen->target_triple, "wasm32")) {
            codegen_configure_wasm_target(codegen);
        }
    } else {
        char* default_triple = LLVMGetDefaultTargetTriple();
        LLVMSetTarget(codegen->module, default_triple);
        LLVMDisposeMessage(default_triple);
    }
    
    return 1;
#else
    return 0;
#endif
}

// Main code generation entry point
int codegen_generate_program(CodeGenerator* codegen, TypeChecker* checker, ASTNode* program) {
    if (!codegen || !checker || !program) return 0;
    
#if !LLVM_AVAILABLE
    codegen_error(codegen, program->pos, "LLVM support not available");
    return 0;
#endif
    
    if (program->type != AST_PROGRAM) {
        codegen_error(codegen, program->pos, "Expected program node");
        return 0;
    }
    
    // Initialize target
    if (!codegen_initialize_target(codegen)) {
        codegen_error(codegen, program->pos, "Failed to initialize target");
        return 0;
    }
    
    ProgramNode* prog = (ProgramNode*)program;
    
    // Generate imports (TODO)
    if (prog->imports) {
        ASTNode* import = prog->imports;
        while (import) {
            // TODO: Handle imports
            import = import->next;
        }
    }
    
    // Generate declarations
    if (prog->decls) {
        ASTNode* decl = prog->decls;
        while (decl) {
            if (!codegen_generate_declaration(codegen, checker, decl)) {
                return 0;
            }
            decl = decl->next;
        }
    }
    
    return codegen->error_count == 0;
}

int codegen_generate_declaration(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl) {
    if (!codegen || !checker || !decl) return 0;
    
    switch (decl->type) {
        case AST_FUNC_DECL:
            return codegen_generate_function_decl(codegen, checker, decl);
        case AST_VAR_DECL:
            return codegen_generate_var_decl(codegen, checker, decl);
        case AST_CONST_DECL:
            return codegen_generate_const_decl(codegen, checker, decl);
        case AST_CONCEPT_DECL:
            // Concepts are compile-time only and don't generate runtime code
            return 1;
        case AST_TYPE_DECL:
            // Type declarations (e.g. `type Point struct { x int }`)
            // are compile-time only — the named type is registered with
            // the type checker so subsequent references resolve. No
            // runtime artifact is emitted.
            return 1;
        default:
            codegen_error(codegen, decl->pos, "Unknown declaration type for code generation");
            return 0;
    }
}

// Error reporting
void codegen_error(CodeGenerator* codegen, Position pos, const char* format, ...) {
    if (!codegen) return;
    
    fprintf(stderr, "Error at %s:%d:%d: ", pos.filename ? pos.filename : "<unknown>", pos.line, pos.column);
    
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    
    fprintf(stderr, "\n");
    codegen->error_count++;
}

void codegen_warning(CodeGenerator* codegen, Position pos, const char* format, ...) {
    if (!codegen) return;
    
    fprintf(stderr, "Warning at %s:%d:%d: ", pos.filename ? pos.filename : "<unknown>", pos.line, pos.column);
    
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    
    fprintf(stderr, "\n");
    codegen->warning_count++;
}

#if LLVM_AVAILABLE

// Value management
ValueInfo* value_info_new(const char* name, LLVMValueRef llvm_value, Type* goo_type) {
    ValueInfo* info = malloc(sizeof(ValueInfo));
    if (!info) return NULL;
    
    info->name = name ? strdup(name) : NULL;
    info->llvm_value = llvm_value;
    info->goo_type = goo_type;
    info->is_lvalue = 0;
    info->is_moved = 0;
    info->is_initialized = 1;
    
    return info;
}

void value_info_free(ValueInfo* info) {
    if (!info) return;
    free(info->name);
    free(info);
}

ValueInfo* codegen_lookup_value(CodeGenerator* codegen, const char* name) {
    if (!codegen || !name) return NULL;
    
    for (size_t i = 0; i < codegen->value_table_size; i++) {
        ValueInfo* info = codegen->value_table[i];
        if (info && info->name && strcmp(info->name, name) == 0) {
            return info;
        }
    }
    
    return NULL;
}

int codegen_add_value(CodeGenerator* codegen, ValueInfo* info) {
    if (!codegen || !info) return 0;
    
    // Resize table if needed
    if (codegen->value_table_size >= codegen->value_table_capacity) {
        size_t new_capacity = codegen->value_table_capacity == 0 ? 16 : codegen->value_table_capacity * 2;
        ValueInfo** new_table = realloc(codegen->value_table, sizeof(ValueInfo*) * new_capacity);
        if (!new_table) return 0;
        
        codegen->value_table = new_table;
        codegen->value_table_capacity = new_capacity;
    }
    
    codegen->value_table[codegen->value_table_size++] = info;
    return 1;
}

// Function management  
FunctionInfo* function_info_new(const char* name, LLVMValueRef function, Type* goo_type) {
    FunctionInfo* info = malloc(sizeof(FunctionInfo));
    if (!info) return NULL;
    
    info->name = name ? strdup(name) : NULL;
    info->function = function;
    info->function_type = LLVMGetElementType(LLVMTypeOf(function));
    info->goo_type = goo_type;
    
    info->entry_block = NULL;
    info->exit_block = NULL;
    info->return_value = NULL;
    
    info->locals = NULL;
    info->local_count = 0;
    info->local_capacity = 0;
    
    return info;
}

void function_info_free(FunctionInfo* info) {
    if (!info) return;
    
    free(info->name);
    
    if (info->locals) {
        for (size_t i = 0; i < info->local_count; i++) {
            if (info->locals[i]) {
                value_info_free(info->locals[i]);
            }
        }
        free(info->locals);
    }
    
    free(info);
}

int codegen_enter_function(CodeGenerator* codegen, FunctionInfo* func_info) {
    if (!codegen || !func_info) return 0;

    codegen->current_function = func_info->function;
    codegen->current_function_info = func_info;
    // Capture the current value table position — anything added past
    // this point belongs to this function and gets cleared on exit.
    codegen->value_table_function_start = codegen->value_table_size;

    return 1;
}

void codegen_exit_function(CodeGenerator* codegen) {
    if (!codegen) return;

    // Truncate the value table back to its pre-function size so this
    // function's locals don't leak into the next function's lookups.
    // Per-info free isn't done here because value_info_free's call
    // pattern in this codebase is inconsistent — the entries stay
    // logically dead and will be overwritten by future adds.
    if (codegen->value_table_size > codegen->value_table_function_start) {
        codegen->value_table_size = codegen->value_table_function_start;
    }

    codegen->current_function = NULL;
    codegen->current_function_info = NULL;
}

// Helper functions
LLVMValueRef codegen_create_alloca(CodeGenerator* codegen, LLVMTypeRef type, const char* name) {
    if (!codegen || !type) return NULL;
    
    return LLVMBuildAlloca(codegen->builder, type, name);
}

LLVMValueRef codegen_create_entry_alloca(CodeGenerator* codegen, LLVMTypeRef type, const char* name) {
    if (!codegen || !type || !codegen->current_function_info) return NULL;
    
    // Save current position
    LLVMBasicBlockRef current_block = LLVMGetInsertBlock(codegen->builder);
    
    // Move to beginning of entry block
    LLVMBasicBlockRef entry = codegen->current_function_info->entry_block;
    if (!entry) {
        entry = LLVMGetEntryBasicBlock(codegen->current_function);
    }
    
    LLVMValueRef first_instruction = LLVMGetFirstInstruction(entry);
    if (first_instruction) {
        LLVMPositionBuilderBefore(codegen->builder, first_instruction);
    } else {
        LLVMPositionBuilderAtEnd(codegen->builder, entry);
    }
    
    // Create alloca
    LLVMValueRef alloca = LLVMBuildAlloca(codegen->builder, type, name);
    
    // Restore position
    if (current_block) {
        LLVMPositionBuilderAtEnd(codegen->builder, current_block);
    }
    
    return alloca;
}

LLVMBasicBlockRef codegen_create_block(CodeGenerator* codegen, const char* name) {
    if (!codegen || !codegen->current_function) return NULL;
    
    return LLVMAppendBasicBlockInContext(codegen->context, codegen->current_function, name);
}

void codegen_set_insert_point(CodeGenerator* codegen, LLVMBasicBlockRef block) {
    if (!codegen || !block) return;
    
    LLVMPositionBuilderAtEnd(codegen->builder, block);
}

#endif

// Output generation stubs (to be implemented)
int codegen_emit_llvm_ir(CodeGenerator* codegen __attribute__((unused)), const char* filename __attribute__((unused))) {
#if LLVM_AVAILABLE
    if (!codegen || !filename) return 0;
    
    char* error_message = NULL;
    if (LLVMPrintModuleToFile(codegen->module, filename, &error_message)) {
        fprintf(stderr, "Error writing LLVM IR: %s\n", error_message);
        LLVMDisposeMessage(error_message);
        return 0;
    }
    
    return 1;
#else
    return 0;
#endif
}

int codegen_emit_object_file(CodeGenerator* codegen, const char* filename) {
#if LLVM_AVAILABLE
    if (!codegen || !filename) return 0;
    
    // Verify module first
    if (!codegen_verify_module(codegen)) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Module verification failed");
        return 0;
    }
    
    // Initialize target if not already done
    if (!codegen->target_machine) {
        char* error_message = NULL;
        
        // Get target triple
        char* target_triple = codegen->target_triple;
        if (!target_triple) {
            target_triple = LLVMGetDefaultTargetTriple();
        }
        
        // Get target from triple
        LLVMTargetRef target;
        if (LLVMGetTargetFromTriple(target_triple, &target, &error_message)) {
            codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Failed to get target: %s", error_message);
            LLVMDisposeMessage(error_message);
            if (!codegen->target_triple) LLVMDisposeMessage(target_triple);
            return 0;
        }
        
        // Create target machine
        const char* cpu = codegen->target_cpu ? codegen->target_cpu : "generic";
        const char* features = codegen->target_features ? codegen->target_features : "";
        
        codegen->target_machine = LLVMCreateTargetMachine(
            target, target_triple, cpu, features,
            LLVMCodeGenLevelDefault, LLVMRelocDefault, LLVMCodeModelDefault
        );
        
        if (!codegen->target_machine) {
            codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Failed to create target machine");
            if (!codegen->target_triple) LLVMDisposeMessage(target_triple);
            return 0;
        }
        
        // Set target data layout
        LLVMTargetDataRef target_data = LLVMCreateTargetDataLayout(codegen->target_machine);
        char* data_layout = LLVMCopyStringRepOfTargetData(target_data);
        LLVMSetDataLayout(codegen->module, data_layout);
        LLVMDisposeMessage(data_layout);
        LLVMDisposeTargetData(target_data);
        
        if (!codegen->target_triple) {
            LLVMDisposeMessage(target_triple);
        }
    }
    
    // Emit object file
    char* error_message = NULL;
    if (LLVMTargetMachineEmitToFile(codegen->target_machine, codegen->module, 
                                   (char*)filename, LLVMObjectFile, &error_message)) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Failed to emit object file: %s", error_message);
        LLVMDisposeMessage(error_message);
        return 0;
    }
    
    return 1;
#else
    return 0;
#endif
}

int codegen_emit_executable(CodeGenerator* codegen, const char* filename) {
#if LLVM_AVAILABLE
    if (!codegen || !filename) return 0;
    
    // Verify module first
    if (!codegen_verify_module(codegen)) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Module verification failed");
        return 0;
    }
    
    // Initialize target if not already done
    if (!codegen->target_machine) {
        char* error_message = NULL;
        
        // Get target triple
        char* target_triple = codegen->target_triple;
        if (!target_triple) {
            target_triple = LLVMGetDefaultTargetTriple();
        }
        
        // Get target from triple
        LLVMTargetRef target;
        if (LLVMGetTargetFromTriple(target_triple, &target, &error_message)) {
            codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Failed to get target: %s", error_message);
            LLVMDisposeMessage(error_message);
            if (!codegen->target_triple) LLVMDisposeMessage(target_triple);
            return 0;
        }
        
        // Create target machine
        const char* cpu = codegen->target_cpu ? codegen->target_cpu : "generic";
        const char* features = codegen->target_features ? codegen->target_features : "";
        
        codegen->target_machine = LLVMCreateTargetMachine(
            target, target_triple, cpu, features,
            LLVMCodeGenLevelDefault, LLVMRelocDefault, LLVMCodeModelDefault
        );
        
        if (!codegen->target_machine) {
            codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Failed to create target machine");
            if (!codegen->target_triple) LLVMDisposeMessage(target_triple);
            return 0;
        }
        
        // Set target data layout
        LLVMTargetDataRef target_data = LLVMCreateTargetDataLayout(codegen->target_machine);
        char* data_layout = LLVMCopyStringRepOfTargetData(target_data);
        LLVMSetDataLayout(codegen->module, data_layout);
        LLVMDisposeMessage(data_layout);
        LLVMDisposeTargetData(target_data);
        
        if (!codegen->target_triple) {
            LLVMDisposeMessage(target_triple);
        }
    }
    
    // Generate object file first
    char* object_filename = malloc(strlen(filename) + 3); // +2 for ".o", +1 for null terminator
    if (!object_filename) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Memory allocation failed");
        return 0;
    }
    sprintf(object_filename, "%s.o", filename);
    
    char* error_message = NULL;
    if (LLVMTargetMachineEmitToFile(codegen->target_machine, codegen->module, 
                                   object_filename, LLVMObjectFile, &error_message)) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"}, "Failed to emit object file: %s", error_message);
        LLVMDisposeMessage(error_message);
        free(object_filename);
        return 0;
    }
    
    // Link object file to create executable
    // Use system linker (ld on Unix, link.exe on Windows)
    char link_command[2048];
    
#ifdef __APPLE__
    // macOS linking with runtime library using clang
    char* target_triple = codegen->target_triple;
    if (!target_triple) {
        target_triple = LLVMGetDefaultTargetTriple();
    }
    snprintf(link_command, sizeof(link_command),
             "clang -target %s -o %s %s lib/libgoo_runtime.a -lm -lpthread",
             target_triple, filename, object_filename);
    if (!codegen->target_triple) {
        LLVMDisposeMessage(target_triple);
    }
#elif defined(__linux__)
    // Linux linking with runtime library using gcc. -lm/-lpthread must follow
    // the archive (the runtime uses libm and pthreads); newer binutils enforces
    // this ordering and otherwise fails with undefined references.
    snprintf(link_command, sizeof(link_command),
             "gcc -o %s %s lib/libgoo_runtime.a -lm -lpthread",
             filename, object_filename);
#elif defined(_WIN32)
    // Windows linking with runtime library
    snprintf(link_command, sizeof(link_command),
             "link.exe /OUT:%s %s lib\\libgoo_runtime.a /ENTRY:main /SUBSYSTEM:CONSOLE",
             filename, object_filename);
#else
    // Generic Unix linking with runtime library using gcc
    snprintf(link_command, sizeof(link_command),
             "gcc -o %s %s lib/libgoo_runtime.a -lm -lpthread",
             filename, object_filename);
#endif
    
    int link_result = system(link_command);
    if (link_result != 0) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"}, 
                     "Linking failed with command: %s", link_command);
        free(object_filename);
        return 0;
    }
    
    // Clean up object file
    remove(object_filename);
    free(object_filename);
    
    return 1;
#else
    return 0;
#endif
}

int codegen_optimize(CodeGenerator* codegen __attribute__((unused)), int level __attribute__((unused))) {
    // TODO: Implement optimization passes
    return 1;
}

int codegen_verify_module(CodeGenerator* codegen __attribute__((unused))) {
#if LLVM_AVAILABLE
    if (!codegen) return 0;
    
    char* error_message = NULL;
    if (LLVMVerifyModule(codegen->module, LLVMReturnStatusAction, &error_message)) {
        fprintf(stderr, "Module verification failed: %s\n", error_message);
        LLVMDisposeMessage(error_message);
        return 0;
    }
    
    return 1;
#else
    return 0;
#endif
}

// Check if target is WebAssembly
int codegen_is_wasm_target(CodeGenerator* codegen) {
    if (!codegen || !codegen->target_triple) return 0;
    return strstr(codegen->target_triple, "wasm32") != NULL;
}

// Generate WebAssembly export attribute
int codegen_add_wasm_export(CodeGenerator* codegen, LLVMValueRef function, const char* export_name) {
    if (!codegen || !function || !export_name) return 0;
    if (!codegen_is_wasm_target(codegen)) return 1; // No-op for non-WASM targets
    
    // Add export attribute to function
    LLVMAttributeRef export_attr = LLVMCreateStringAttribute(codegen->context, 
                                                            "wasm-export-name", 16,
                                                            export_name, strlen(export_name));
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, export_attr);
    
    // Make function externally visible
    LLVMSetLinkage(function, LLVMExternalLinkage);
    
    return 1;
}

// Generate WebAssembly import attribute  
int codegen_add_wasm_import(CodeGenerator* codegen, LLVMValueRef function, 
                           const char* module_name, const char* import_name) {
    if (!codegen || !function || !module_name || !import_name) return 0;
    if (!codegen_is_wasm_target(codegen)) return 1; // No-op for non-WASM targets
    
    // Add import module attribute
    LLVMAttributeRef module_attr = LLVMCreateStringAttribute(codegen->context,
                                                           "wasm-import-module", 18,
                                                           module_name, strlen(module_name));
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, module_attr);
    
    // Add import name attribute
    LLVMAttributeRef name_attr = LLVMCreateStringAttribute(codegen->context,
                                                         "wasm-import-name", 16,
                                                         import_name, strlen(import_name));
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, name_attr);
    
    return 1;
}

// Generate WASM memory management functions
int codegen_declare_wasm_runtime_functions(CodeGenerator* codegen) {
    if (!codegen || !codegen_is_wasm_target(codegen)) return 1;
    
    // WebAssembly memory.grow and memory.size intrinsics
    LLVMTypeRef i32_type = LLVMInt32TypeInContext(codegen->context);
    
    // i32 @llvm.wasm.memory.grow.i32(i32 %memory_index, i32 %delta)
    LLVMTypeRef grow_params[] = { i32_type, i32_type };
    LLVMTypeRef grow_type = LLVMFunctionType(i32_type, grow_params, 2, 0);
    LLVMValueRef grow_func = LLVMAddFunction(codegen->module, "llvm.wasm.memory.grow.i32", grow_type);
    
    // i32 @llvm.wasm.memory.size.i32(i32 %memory_index)
    LLVMTypeRef size_params[] = { i32_type };
    LLVMTypeRef size_type = LLVMFunctionType(i32_type, size_params, 1, 0);
    LLVMValueRef size_func = LLVMAddFunction(codegen->module, "llvm.wasm.memory.size.i32", size_type);
    
    return 1;
}

// Configure concurrency for WebAssembly environment
#if LLVM_AVAILABLE
int codegen_configure_wasm_concurrency(CodeGenerator* codegen) {
    if (!codegen || !codegen_is_wasm_target(codegen)) return 1;
    
    // For single-threaded WASM, transform goroutines to async/await
    // This will be used during function generation to emit different code
    // for go statements in WASM vs native targets
    
    // Add JavaScript async/await helper imports
    LLVMTypeRef void_type = LLVMVoidTypeInContext(codegen->context);
    LLVMTypeRef ptr_type = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
    
    // Import JavaScript Promise handling functions
    LLVMTypeRef promise_params[] = { ptr_type };
    LLVMTypeRef promise_type = LLVMFunctionType(ptr_type, promise_params, 1, 0);
    LLVMValueRef create_promise = LLVMAddFunction(codegen->module, "js_create_promise", promise_type);
    LLVMValueRef resolve_promise = LLVMAddFunction(codegen->module, "js_resolve_promise", promise_type);
    
    // Add import attributes
    codegen_add_wasm_import(codegen, create_promise, "js", "create_promise");
    codegen_add_wasm_import(codegen, resolve_promise, "js", "resolve_promise");
    
    return 1;
}
#endif