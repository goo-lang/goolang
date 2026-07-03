#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>

// stdlib Phase 0 (Task 4): compute the LLVM symbol name for a top-level symbol
// emitted while codegenning a non-main package. For the main package
// (checker->current_package == NULL) this returns NULL and callers keep the
// BARE name, so the no-import path is byte-identical. Otherwise it returns a
// malloc'd `goo_pkg__<pkg>__<base>` (caller frees). `base` is the type-checker
// lookup key — the bare function name for a plain/top-level function, or the
// method-mangled `T__m` for a method — so BOTH plain functions and methods (and
// any future top-level symbol) get the package prefix and never collide with
// main's bare symbols. This is the single source of truth for cross-package
// mangling, shared by function_codegen.c and error_union_codegen.c.
char* codegen_package_symbol_name(TypeChecker* checker, const char* base) {
    if (!checker || !checker->current_package
                 || !checker->current_package->name || !base) {
        return NULL;
    }
    const char* pkg = checker->current_package->name;
    size_t need = strlen("goo_pkg__") + strlen(pkg) + strlen("__")
                + strlen(base) + 1;
    char* out = malloc(need);
    if (!out) return NULL;
    snprintf(out, need, "goo_pkg__%s__%s", pkg, base);
    return out;
}

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

    codegen->struct_cache_keys = NULL;
    codegen->struct_cache_vals = NULL;
    codegen->struct_cache_size = 0;
    codegen->struct_cache_cap = 0;

    // Loop-context stack (break/continue targets) starts empty.
    codegen->loop_depth = 0;

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

    // Deferred global initializers (Task 2 / var-init cluster) — empty
    // until codegen_generate_var_decl's module-scope path defers one.
    codegen->deferred_global_inits = NULL;
    codegen->deferred_global_init_count = 0;
    codegen->deferred_global_init_capacity = 0;

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

    free(codegen->struct_cache_keys);
    free(codegen->struct_cache_vals);

    // The deferred-init array holds borrowed pointers (globals owned by the
    // module, expressions by the AST, types by the type system) — free only
    // the array itself.
    free(codegen->deferred_global_inits);

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
    
    // Forward-reference pre-pass: declare all plain-function prototypes before
    // emitting any body, so a call to a function defined later in the file
    // resolves (call sites use LLVMGetNamedFunction). Mirrors the type checker's
    // hoist_function_signatures.
    if (!codegen_predeclare_functions(codegen, checker, prog->decls)) {
        return 0;
    }

    // Task 2 / var-init cluster: pre-create goo.global_init's prototype
    // (no body yet) BEFORE any function body is generated. Without this, a
    // package-level `var` that ends up deferred but appears textually AFTER
    // `func main` would mean the call inserted into main's prologue (see
    // codegen_generate_function_decl's is_entry_main branch) can't find the
    // symbol yet — LLVMGetNamedFunction would return NULL at that point,
    // since the actual body-filling pass (codegen_generate_global_init_
    // function below) only runs after every declaration, including main
    // itself, has been generated. The prototype-now/body-later split lets
    // main call it regardless of declaration order.
    if (codegen_program_needs_global_init(prog->decls)) {
        LLVMTypeRef void_ty = LLVMVoidTypeInContext(codegen->context);
        LLVMTypeRef fn_ty = LLVMFunctionType(void_ty, NULL, 0, 0);
        LLVMAddFunction(codegen->module, "goo.global_init", fn_ty);
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

    // Fill in goo.global_init's body from whatever was deferred while
    // generating the declarations above. Must run after the loop: every
    // global and every function this initializer might call (e.g. `var w =
    // double(x)`) needs to already exist in the module.
    if (!codegen_generate_global_init_function(codegen, checker)) {
        return 0;
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

    // Search from the end so the most-recently added binding shadows earlier
    // ones.  This gives correct LIFO (innermost-scope-wins) semantics without
    // a full scope stack.  It also fixes the multiple-catch-block bug where
    // each `catch e {}` adds a new entry for `e`; FIFO would always return the
    // first entry (wrong alloca), whereas LIFO correctly returns the latest.
    for (size_t i = codegen->value_table_size; i > 0; i--) {
        ValueInfo* info = codegen->value_table[i - 1];
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

    info->named_result_names = NULL;
    info->named_result_count = 0;

    info->deferred_calls = NULL;
    info->deferred_count = 0;
    info->deferred_capacity = 0;

    return info;
}

void function_info_free(FunctionInfo* info) {
    if (!info) return;
    
    free(info->name);

    if (info->named_result_names) {
        for (size_t i = 0; i < info->named_result_count; i++)
            free(info->named_result_names[i]);
        free(info->named_result_names);
    }

    // The deferred-call array holds borrowed AST node pointers (owned by the
    // parse tree), so free only the array, not the nodes.
    free(info->deferred_calls);

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

LLVMValueRef codegen_map_value_to_slot(CodeGenerator* codegen, LLVMValueRef value, Type* value_type) {
    if (!codegen || !value || !value_type) return NULL;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(codegen->context);
    if (value_type->kind == TYPE_POINTER) {
        return LLVMBuildPtrToInt(codegen->builder, value, i64, "map_slot");
    }
    // Sign-extend a SIGNED integer when widening into the slot, so a negative
    // value narrower than the slot keeps its sign (e.g. the i32 literal -1
    // into a map[string]int64 must read back as -1, not 4294967295). Integer
    // literals are always emitted i32, so this widening is real. Unsigned,
    // bool, and char zero-extend. The read truncates back to V's width.
    LLVMBool is_signed = (value_type->kind >= TYPE_INT8 && value_type->kind <= TYPE_INT64);
    return LLVMBuildIntCast2(codegen->builder, value, i64, is_signed, "map_slot");
}

LLVMValueRef codegen_map_slot_to_value(CodeGenerator* codegen, LLVMValueRef slot, Type* value_type) {
    if (!codegen || !slot || !value_type) return NULL;
    LLVMTypeRef vt = codegen_type_to_llvm(codegen, value_type);
    if (!vt) return NULL;
    if (value_type->kind == TYPE_POINTER) {
        return LLVMBuildIntToPtr(codegen->builder, slot, vt, "map_val");
    }
    return LLVMBuildIntCast2(codegen->builder, slot, vt, /*isSigned=*/0, "map_val");
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

// Resolve the runtime archive path independent of cwd so the compiler works
// when invoked from any directory. Order: $GOO_RUNTIME -> <exe-dir>/../lib ->
// cwd-relative fallback. Returns a pointer into a static buffer (single-threaded
// codegen; not reentrant — acceptable here).
static const char* goo_runtime_archive_path(void) {
    static char buf[4096];
    const char* env = getenv("GOO_RUNTIME");
    if (env && env[0] != '\0') { snprintf(buf, sizeof(buf), "%s", env); return buf; }
#ifdef __linux__
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char* slash = strrchr(exe, '/');        // dir-of-exe
        if (slash) {
            *slash = '\0';
            snprintf(buf, sizeof(buf), "%s/../lib/libgoo_runtime.a", exe);
            struct stat st;
            if (stat(buf, &st) == 0) return buf; // exists relative to the binary
        }
    }
#endif
    snprintf(buf, sizeof(buf), "lib/libgoo_runtime.a");  // fallback (repo-root cwd)
    return buf;
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
             "clang -target %s -o %s %s %s -lm -lpthread",
             target_triple, filename, object_filename, goo_runtime_archive_path());
    if (!codegen->target_triple) {
        LLVMDisposeMessage(target_triple);
    }
#elif defined(__linux__)
    // Linux linking with runtime library using gcc. -lm/-lpthread must follow
    // the archive (the runtime uses libm and pthreads); newer binutils enforces
    // this ordering and otherwise fails with undefined references.
    // -no-pie: the LLVM backend emits non-PIC objects (R_X86_64_32S relocs),
    // but distros like Ubuntu default gcc to -pie, which rejects them with
    // "relocation ... can not be used when making a PIE object". Force a
    // non-PIE link to match the object model.
    snprintf(link_command, sizeof(link_command),
             "gcc -no-pie -o %s %s %s -lm -lpthread",
             filename, object_filename, goo_runtime_archive_path());
#elif defined(_WIN32)
    // Windows linking with runtime library
    snprintf(link_command, sizeof(link_command),
             "link.exe /OUT:%s %s %s /ENTRY:main /SUBSYSTEM:CONSOLE",
             filename, object_filename, goo_runtime_archive_path());
#else
    // Generic Unix linking with runtime library using gcc (see -no-pie note above)
    snprintf(link_command, sizeof(link_command),
             "gcc -no-pie -o %s %s %s -lm -lpthread",
             filename, object_filename, goo_runtime_archive_path());
#endif
    
    int link_result = system(link_command);
    if (link_result != 0) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"},
                     "Linking failed with command: %s", link_command);
        remove(object_filename);   // don't leave a stray object behind
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

// Coerce a VALUE to the target LLVM type using the source type's
// signedness — the single home for the width-coercion rule that was
// previously inlined (and repeatedly re-broken) at the var-decl,
// literal-element, append, and channel-send sites:
//   int -> int      : SExt/ZExt by src_signed when widening, Trunc when narrowing
//   int -> float    : SIToFP/UIToFP by src_signed
//   float -> float  : FPExt widening, FPTrunc narrowing
// Anything else (matching types, aggregates, pointers) returns v unchanged.
// REQUIRES a positioned builder — callers on constant/global paths must keep
// their LLVMConstInt/LLVMConstReal rebuilds instead.
#if LLVM_AVAILABLE
LLVMValueRef codegen_coerce_to_type(CodeGenerator* codegen, LLVMValueRef v,
                                    int src_signed, LLVMTypeRef to) {
    LLVMTypeRef from = LLVMTypeOf(v);
    if (from == to) return v;
    LLVMTypeKind fk = LLVMGetTypeKind(from), tk = LLVMGetTypeKind(to);
    int f_is_fp = (fk == LLVMFloatTypeKind || fk == LLVMDoubleTypeKind);
    int t_is_fp = (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind);
    if (fk == LLVMIntegerTypeKind && tk == LLVMIntegerTypeKind) {
        unsigned fb = LLVMGetIntTypeWidth(from), tb = LLVMGetIntTypeWidth(to);
        if (fb < tb) return src_signed
            ? LLVMBuildSExt(codegen->builder, v, to, "coerce_sext")
            : LLVMBuildZExt(codegen->builder, v, to, "coerce_zext");
        if (fb > tb) return LLVMBuildTrunc(codegen->builder, v, to, "coerce_trunc");
        return v;
    }
    if (fk == LLVMIntegerTypeKind && t_is_fp) return src_signed
        ? LLVMBuildSIToFP(codegen->builder, v, to, "coerce_sitofp")
        : LLVMBuildUIToFP(codegen->builder, v, to, "coerce_uitofp");
    if (f_is_fp && t_is_fp) {
        // float32<->float64 only; kind order: Float < Double.
        if (fk == LLVMFloatTypeKind && tk == LLVMDoubleTypeKind)
            return LLVMBuildFPExt(codegen->builder, v, to, "coerce_fpext");
        if (fk == LLVMDoubleTypeKind && tk == LLVMFloatTypeKind)
            return LLVMBuildFPTrunc(codegen->builder, v, to, "coerce_fptrunc");
    }
    return v;
}
#endif
