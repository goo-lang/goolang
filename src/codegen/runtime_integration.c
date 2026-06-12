#include "codegen.h"
#include <string.h>
#include <stdlib.h>

#if LLVM_AVAILABLE

// Runtime function metadata
typedef struct {
    const char* name;
    LLVMTypeRef return_type;
    LLVMTypeRef* param_types;
    unsigned param_count;
    LLVMValueRef function;
} RuntimeFunction;

// Runtime function table
static RuntimeFunction* runtime_functions = NULL;
static size_t runtime_function_count = 0;
static size_t runtime_function_capacity = 0;

// Helper to add a runtime function to the table
static void add_runtime_function(CodeGenerator* codegen, const char* name, 
                                LLVMTypeRef return_type, LLVMTypeRef* param_types, 
                                unsigned param_count) {
    if (runtime_function_count >= runtime_function_capacity) {
        runtime_function_capacity = runtime_function_capacity ? runtime_function_capacity * 2 : 16;
        runtime_functions = realloc(runtime_functions, 
                                   sizeof(RuntimeFunction) * runtime_function_capacity);
    }
    
    RuntimeFunction* func = &runtime_functions[runtime_function_count++];
    func->name = name;
    func->return_type = return_type;
    func->param_count = param_count;
    
    if (param_count > 0) {
        func->param_types = malloc(sizeof(LLVMTypeRef) * param_count);
        memcpy(func->param_types, param_types, sizeof(LLVMTypeRef) * param_count);
    } else {
        func->param_types = NULL;
    }
    
    // Create the function type and declare it
    LLVMTypeRef func_type = LLVMFunctionType(return_type, param_types, param_count, 0);
    func->function = LLVMAddFunction(codegen->module, name, func_type);
}

LLVMValueRef codegen_declare_runtime_functions(CodeGenerator* codegen) {
    if (!codegen) return NULL;
    
    // Basic types
    LLVMTypeRef void_type = LLVMVoidTypeInContext(codegen->context);
    LLVMTypeRef i8_type = LLVMInt8TypeInContext(codegen->context);
    LLVMTypeRef i32_type = LLVMInt32TypeInContext(codegen->context);
    LLVMTypeRef i64_type __attribute__((unused)) = LLVMInt64TypeInContext(codegen->context);
    LLVMTypeRef ptr_type = LLVMPointerType(i8_type, 0);
    LLVMTypeRef size_type = LLVMInt64TypeInContext(codegen->context);  // size_t
    
    // goo_string_t type (struct { char* data, size_t length })
    LLVMTypeRef string_types[] = { ptr_type, size_type };
    LLVMTypeRef string_type = LLVMStructTypeInContext(codegen->context, string_types, 2, 0);
    
    // goo_slice_t type (struct { void* data, size_t length, size_t capacity })
    // WARNING: this 3-field layout does NOT match codegen's TYPE_SLICE
    // ({T*, i64} — see src/codegen/type_mapping.c). Slice values that
    // flow between Goo code and the runtime must use the 2-field
    // str_slice_type below (matches goo_str_slice_t in runtime.h).
    LLVMTypeRef slice_types[] = { ptr_type, size_type, size_type };
    LLVMTypeRef slice_type = LLVMStructTypeInContext(codegen->context, slice_types, 3, 0);

    // goo_str_slice_t type (struct { goo_string_t* data, int64_t length })
    // — the codegen TYPE_SLICE layout.
    LLVMTypeRef str_slice_types[] = { ptr_type, size_type };
    LLVMTypeRef str_slice_type = LLVMStructTypeInContext(codegen->context, str_slice_types, 2, 0);
    
    // Program initialization
    // void goo_init(int argc, char** argv)
    {
        LLVMTypeRef params[] = { i32_type, LLVMPointerType(ptr_type, 0) };
        add_runtime_function(codegen, "goo_init", void_type, params, 2);
    }
    
    // void goo_exit(int code)
    {
        LLVMTypeRef params[] = { i32_type };
        add_runtime_function(codegen, "goo_exit", void_type, params, 1);
    }
    
    // Memory management
    // void* goo_alloc(size_t size)
    {
        LLVMTypeRef params[] = { size_type };
        add_runtime_function(codegen, "goo_alloc", ptr_type, params, 1);
    }
    
    // void* goo_realloc(void* ptr, size_t size)
    {
        LLVMTypeRef params[] = { ptr_type, size_type };
        add_runtime_function(codegen, "goo_realloc", ptr_type, params, 2);
    }
    
    // void goo_free(void* ptr)
    {
        LLVMTypeRef params[] = { ptr_type };
        add_runtime_function(codegen, "goo_free", void_type, params, 1);
    }
    
    // Error handling
    // void goo_panic(const char* message)
    {
        LLVMTypeRef params[] = { ptr_type };
        add_runtime_function(codegen, "goo_panic", void_type, params, 1);
    }
    
    // goo_error_t* goo_new_error(const char* message)
    {
        LLVMTypeRef params[] = { ptr_type };
        add_runtime_function(codegen, "goo_new_error", ptr_type, params, 1);
    }
    
    // I/O functions
    // void goo_print(const char* message)
    {
        LLVMTypeRef params[] = { ptr_type };
        add_runtime_function(codegen, "goo_print", void_type, params, 1);
    }
    
    // void goo_println(const char* message)
    {
        LLVMTypeRef params[] = { ptr_type };
        add_runtime_function(codegen, "goo_println", void_type, params, 1);
    }
    
    // void goo_print_string(goo_string_t str)
    {
        LLVMTypeRef params[] = { string_type };
        add_runtime_function(codegen, "goo_print_string", void_type, params, 1);
    }
    
    // void goo_println_string(goo_string_t str)
    {
        LLVMTypeRef params[] = { string_type };
        add_runtime_function(codegen, "goo_println_string", void_type, params, 1);
    }

    // void goo_println_int(int64_t value) — Println dispatch target for any
    // signed integer; codegen sign-extends narrower int types to i64.
    {
        LLVMTypeRef params[] = { LLVMInt64TypeInContext(codegen->context) };
        add_runtime_function(codegen, "goo_println_int", void_type, params, 1);
    }

    // void goo_println_bool(int value) — i1 zero-extended to i32 at the call site.
    {
        LLVMTypeRef params[] = { LLVMInt32TypeInContext(codegen->context) };
        add_runtime_function(codegen, "goo_println_bool", void_type, params, 1);
    }

    // void goo_println_float(double value) — f32 promoted to f64 at the call site.
    {
        LLVMTypeRef params[] = { LLVMDoubleTypeInContext(codegen->context) };
        add_runtime_function(codegen, "goo_println_float", void_type, params, 1);
    }

    // Print-without-newline variants. The variadic fmt.Println codegen
    // emits one print-no-newline per arg, then a single goo_println("")
    // for the trailing newline. Same width semantics as their println
    // siblings (sext to i64 for ints, zext to i32 for bools, fpext to
    // f64 for floats).
    {
        LLVMTypeRef params[] = { LLVMInt64TypeInContext(codegen->context) };
        add_runtime_function(codegen, "goo_print_int", void_type, params, 1);
    }
    {
        LLVMTypeRef params[] = { LLVMInt32TypeInContext(codegen->context) };
        add_runtime_function(codegen, "goo_print_bool", void_type, params, 1);
    }
    {
        LLVMTypeRef params[] = { LLVMDoubleTypeInContext(codegen->context) };
        add_runtime_function(codegen, "goo_print_float", void_type, params, 1);
    }
    
    // String operations
    // goo_string_t goo_string_new(const char* data)
    {
        LLVMTypeRef params[] = { ptr_type };
        add_runtime_function(codegen, "goo_string_new", string_type, params, 1);
    }
    
    // goo_string_t goo_string_concat(goo_string_t a, goo_string_t b)
    {
        LLVMTypeRef params[] = { string_type, string_type };
        add_runtime_function(codegen, "goo_string_concat", string_type, params, 2);
    }
    
    // void goo_string_free(goo_string_t str)
    {
        LLVMTypeRef params[] = { string_type };
        add_runtime_function(codegen, "goo_string_free", void_type, params, 1);
    }

    // Stdlib package backings
    // int goo_strings_contains(const char* haystack, const char* needle)
    {
        LLVMTypeRef params[] = { ptr_type, ptr_type };
        add_runtime_function(codegen, "goo_strings_contains",
                             LLVMInt32TypeInContext(codegen->context), params, 2);
    }

    // goo_string_t goo_strings_to_upper/to_lower/trim_space(const char* s)
    {
        LLVMTypeRef params[] = { ptr_type };
        add_runtime_function(codegen, "goo_strings_to_upper", string_type, params, 1);
        add_runtime_function(codegen, "goo_strings_to_lower", string_type, params, 1);
        add_runtime_function(codegen, "goo_strings_trim_space", string_type, params, 1);
    }

    // goo_str_slice_t goo_strings_split(const char* s, const char* sep)
    {
        LLVMTypeRef params[] = { ptr_type, ptr_type };
        add_runtime_function(codegen, "goo_strings_split", str_slice_type, params, 2);
    }

    // goo_string_t goo_strings_join(goo_str_slice_t parts, const char* sep)
    {
        LLVMTypeRef params[] = { str_slice_type, ptr_type };
        add_runtime_function(codegen, "goo_strings_join", string_type, params, 2);
    }

    // goo_string_t goo_os_getenv(const char* name)
    {
        LLVMTypeRef params[] = { ptr_type };
        add_runtime_function(codegen, "goo_os_getenv", string_type, params, 1);
    }

    // double goo_math_sqrt/abs(double), goo_math_pow/min/max(double, double)
    {
        LLVMTypeRef dbl = LLVMDoubleTypeInContext(codegen->context);
        LLVMTypeRef unary[] = { dbl };
        LLVMTypeRef binary[] = { dbl, dbl };
        add_runtime_function(codegen, "goo_math_sqrt", dbl, unary, 1);
        add_runtime_function(codegen, "goo_math_abs", dbl, unary, 1);
        add_runtime_function(codegen, "goo_math_pow", dbl, binary, 2);
        add_runtime_function(codegen, "goo_math_min", dbl, binary, 2);
        add_runtime_function(codegen, "goo_math_max", dbl, binary, 2);
    }

    // GooMapSI* goo_map_new_si(void)
    {
        add_runtime_function(codegen, "goo_map_new_si", ptr_type, NULL, 0);
    }
    // void goo_map_set_si(GooMapSI*, const char*, int)
    {
        LLVMTypeRef params[] = { ptr_type, ptr_type, LLVMInt32TypeInContext(codegen->context) };
        add_runtime_function(codegen, "goo_map_set_si", void_type, params, 3);
    }
    // int goo_map_get_si(GooMapSI*, const char*)
    {
        LLVMTypeRef params[] = { ptr_type, ptr_type };
        add_runtime_function(codegen, "goo_map_get_si",
                             LLVMInt32TypeInContext(codegen->context), params, 2);
    }

    // Slice operations
    // goo_slice_t goo_slice_new(size_t element_size, size_t capacity)
    {
        LLVMTypeRef params[] = { size_type, size_type };
        add_runtime_function(codegen, "goo_slice_new", slice_type, params, 2);
    }
    
    // void goo_slice_free(goo_slice_t slice)
    {
        LLVMTypeRef params[] = { slice_type };
        add_runtime_function(codegen, "goo_slice_free", void_type, params, 1);
    }
    
    // void* goo_slice_get(goo_slice_t slice, size_t index, size_t element_size)
    {
        LLVMTypeRef params[] = { slice_type, size_type, size_type };
        add_runtime_function(codegen, "goo_slice_get", ptr_type, params, 3);
    }
    
    // Bounds checking
    // void goo_bounds_check(size_t index, size_t length, const char* file, int line)
    {
        LLVMTypeRef params[] = { size_type, size_type, ptr_type, i32_type };
        add_runtime_function(codegen, "goo_bounds_check", void_type, params, 4);
    }
    
    // void goo_null_check(void* ptr, const char* file, int line)
    {
        LLVMTypeRef params[] = { ptr_type, ptr_type, i32_type };
        add_runtime_function(codegen, "goo_null_check", void_type, params, 3);
    }
    
    // int goo_check_bounds(size_t index, size_t length)
    {
        LLVMTypeRef params[] = { size_type, size_type };
        add_runtime_function(codegen, "goo_check_bounds", i32_type, params, 2);
    }
    
    // WebAssembly-specific runtime functions
    if (codegen->is_wasm_target) {
        // JavaScript interop functions
        // void js_console_log(const char* message)
        {
            LLVMTypeRef params[] = { ptr_type };
            add_runtime_function(codegen, "js_console_log", void_type, params, 1);
        }
        
        // void* js_create_object()
        {
            add_runtime_function(codegen, "js_create_object", ptr_type, NULL, 0);
        }
        
        // void js_set_property(void* obj, const char* key, void* value)
        {
            LLVMTypeRef params[] = { ptr_type, ptr_type, ptr_type };
            add_runtime_function(codegen, "js_set_property", void_type, params, 3);
        }
        
        // void* js_get_property(void* obj, const char* key)
        {
            LLVMTypeRef params[] = { ptr_type, ptr_type };
            add_runtime_function(codegen, "js_get_property", ptr_type, params, 2);
        }
        
        // DOM manipulation functions
        // void* dom_get_element_by_id(const char* id)
        {
            LLVMTypeRef params[] = { ptr_type };
            add_runtime_function(codegen, "dom_get_element_by_id", ptr_type, params, 1);
        }
        
        // void dom_add_event_listener(void* element, const char* event, void* callback)
        {
            LLVMTypeRef params[] = { ptr_type, ptr_type, ptr_type };
            add_runtime_function(codegen, "dom_add_event_listener", void_type, params, 3);
        }
        
        // WebAssembly memory management
        // i32 wasm_memory_grow(i32 delta)
        {
            LLVMTypeRef params[] = { i32_type };
            add_runtime_function(codegen, "wasm_memory_grow", i32_type, params, 1);
        }
        
        // i32 wasm_memory_size()
        {
            add_runtime_function(codegen, "wasm_memory_size", i32_type, NULL, 0);
        }
        
        // Async/await transformation helpers for single-threaded WASM
        // void* js_create_promise(void* executor)
        {
            LLVMTypeRef params[] = { ptr_type };
            add_runtime_function(codegen, "js_create_promise", ptr_type, params, 1);
        }
        
        // void js_resolve_promise(void* promise, void* value)
        {
            LLVMTypeRef params[] = { ptr_type, ptr_type };
            add_runtime_function(codegen, "js_resolve_promise", void_type, params, 2);
        }
        
        // void js_reject_promise(void* promise, void* error)
        {
            LLVMTypeRef params[] = { ptr_type, ptr_type };
            add_runtime_function(codegen, "js_reject_promise", void_type, params, 2);
        }
    }
    
    return NULL;
}

LLVMValueRef codegen_get_runtime_function(CodeGenerator* codegen, const char* name) {
    if (!codegen || !name) return NULL;
    
    for (size_t i = 0; i < runtime_function_count; i++) {
        if (strcmp(runtime_functions[i].name, name) == 0) {
            return runtime_functions[i].function;
        }
    }
    
    return NULL;
}

LLVMValueRef codegen_call_runtime_function(CodeGenerator* codegen, const char* name, 
                                          LLVMValueRef* args, unsigned arg_count) {
    if (!codegen || !name) return NULL;
    
    LLVMValueRef func = codegen_get_runtime_function(codegen, name);
    if (!func) {
        codegen_error(codegen, (Position){0}, "Runtime function '%s' not found", name);
        return NULL;
    }
    
    return LLVMBuildCall2(codegen->builder, LLVMGetElementType(LLVMTypeOf(func)), 
                         func, args, arg_count, "");
}

#else

// Stubs for when LLVM is not available
int codegen_declare_runtime_functions(CodeGenerator* codegen) {
    (void)codegen;
    return 0;
}

int codegen_get_runtime_function(CodeGenerator* codegen, const char* name) {
    (void)codegen;
    (void)name;
    return 0;
}

int codegen_call_runtime_function(CodeGenerator* codegen, const char* name, 
                                  void* args, unsigned arg_count) {
    (void)codegen;
    (void)name;
    (void)args;
    (void)arg_count;
    return 0;
}

#endif