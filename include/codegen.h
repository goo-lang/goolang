#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include "types.h"
#include "runtime.h"
#include <stddef.h>

// LLVM C API includes (only if LLVM is available)
#ifdef __has_include
#if __has_include(<llvm-c/Core.h>)
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#define LLVM_AVAILABLE 1
#else
#define LLVM_AVAILABLE 0
#endif
#else
// Fallback for older compilers
#define LLVM_AVAILABLE 0
#endif

// Forward declarations
typedef struct CodeGenerator CodeGenerator;
typedef struct FunctionInfo FunctionInfo;
typedef struct ValueInfo ValueInfo;

#if LLVM_AVAILABLE
// LLVM-based code generator
struct CodeGenerator {
    LLVMContextRef context;
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    LLVMTargetMachineRef target_machine;
    
    // Current function being generated
    LLVMValueRef current_function;
    FunctionInfo* current_function_info;
    
    // Symbol tables
    ValueInfo** value_table;     // Maps variable names to LLVM values
    size_t value_table_size;
    size_t value_table_capacity;
    // High-water mark captured on codegen_enter_function. Used by
    // codegen_exit_function to truncate the value table back to its
    // pre-function size so per-function locals don't leak across
    // function boundaries and produce "Referring to an instruction
    // in another function" verifier errors on later lookups.
    size_t value_table_function_start;
    
    // Type cache for LLVM types
    LLVMTypeRef* type_cache;
    size_t type_cache_size;
    size_t type_cache_capacity;

    // Struct-type cache: maps Goo Type* -> LLVMTypeRef for named struct
    // types. Pre-populated with an opaque struct before resolving fields so
    // that recursive pointer fields (`next *Node`) can reference the opaque
    // type without infinite recursion in codegen_get_struct_type.
    const Type** struct_cache_keys;
    LLVMTypeRef* struct_cache_vals;
    size_t struct_cache_size;
    size_t struct_cache_cap;

    // Loop-context stack for break/continue targets (depth-bounded; nesting
    // deeper than 32 is rejected with a codegen error).
    LLVMBasicBlockRef loop_break_bb[32];
    LLVMBasicBlockRef loop_continue_bb[32];
    int loop_depth;

    // Error reporting
    char* current_file;
    int error_count;
    int warning_count;
    
    // Target information
    char* target_triple;
    char* target_cpu;
    char* target_features;
    
    // WebAssembly-specific configuration
    int wasm_configured;
    int is_wasm_target;
};

// Function information for code generation
struct FunctionInfo {
    char* name;
    LLVMValueRef function;
    LLVMTypeRef function_type;
    Type* goo_type;
    
    // Basic blocks
    LLVMBasicBlockRef entry_block;
    LLVMBasicBlockRef exit_block;
    LLVMValueRef return_value;  // Alloca for return value
    
    // Local variables
    ValueInfo** locals;
    size_t local_count;
    size_t local_capacity;

    // Named return parameters (P3-5). When the function declares
    // `(x int, y int)` results, these hold the result names in field
    // order; a bare `return` loads each named-result local and builds the
    // aggregate return value from them. NULL / 0 for ordinary functions.
    char** named_result_names;
    size_t named_result_count;

    // Deferred calls (P3-4). Each `defer <call>` pushes its call AST node
    // here in source order; at every function-exit path the calls are
    // emitted in reverse (LIFO) order immediately before the `ret`. MVP:
    // arguments are evaluated at exit time, which matches Go's defer-time
    // evaluation for the literal/simple-arg cases the probe covers.
    ASTNode** deferred_calls;
    size_t deferred_count;
    size_t deferred_capacity;
};

// Value information for variables and expressions
struct ValueInfo {
    char* name;
    LLVMValueRef llvm_value;
    Type* goo_type;
    int is_lvalue;          // Can be assigned to
    int is_moved;           // For ownership tracking
    int is_initialized;     // For null safety
};

#else
// Stub implementation when LLVM is not available
struct CodeGenerator {
    char* error_message;
    int llvm_unavailable;
    
    // Error reporting (needed for compatibility)
    char* current_file;
    int error_count;
    int warning_count;
};

struct FunctionInfo {
    char* name;
};

struct ValueInfo {
    char* name;
};
#endif

// Code generator creation and destruction
CodeGenerator* codegen_new(const char* module_name);
void codegen_free(CodeGenerator* codegen);

// Target configuration
int codegen_set_target(CodeGenerator* codegen, const char* triple, const char* cpu, const char* features);
int codegen_initialize_target(CodeGenerator* codegen);

// stdlib Phase 0 (Task 4): cross-package symbol mangling. Returns a malloc'd
// `goo_pkg__<pkg>__<base>` when codegenning a non-main package, or NULL for the
// main package (callers keep the bare `base`). Single source of truth shared by
// the plain-function and error-union codegen paths. Caller frees the result.
char* codegen_package_symbol_name(TypeChecker* checker, const char* base);

// Code generation entry points
int codegen_generate_program(CodeGenerator* codegen, TypeChecker* checker, ASTNode* program);
int codegen_generate_declaration(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl);
int codegen_generate_statement(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
ValueInfo* codegen_generate_expression(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

// Declaration generation
// Forward-reference pre-pass: declare every plain function's LLVM prototype in
// the module before any body is emitted, so a call to a function defined later
// resolves. Mirrors the type checker's hoist_function_signatures. Call once,
// before the body-emitting declaration loop. Returns 1 on success, 0 on failure.
int codegen_predeclare_functions(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decls);
int codegen_generate_function_decl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl);
int codegen_generate_var_decl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl);
int codegen_generate_multi_assign(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
// Address of an assignable lvalue (identifier/field/index), unloaded.
ValueInfo* codegen_emit_lvalue_address(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
int codegen_generate_const_decl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl);

// Statement generation
int codegen_generate_block_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_expr_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_if_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_for_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_return_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_go_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_defer_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
// Emit the current function's registered defers in LIFO order before a `ret`.
void codegen_emit_deferred_calls(CodeGenerator* codegen, TypeChecker* checker);
int codegen_generate_select_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_switch_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_unsafe_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_asm_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);

// Select statement helper functions
#if LLVM_AVAILABLE
LLVMTypeRef codegen_get_select_case_type(CodeGenerator* codegen);
LLVMValueRef codegen_get_select_function(CodeGenerator* codegen);
int codegen_setup_select_case(CodeGenerator* codegen, TypeChecker* checker, 
                              LLVMValueRef cases_array, size_t case_index, 
                              SelectCaseNode* select_case);
#endif

// Expression generation
ValueInfo* codegen_generate_identifier(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_literal(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
// Build a *constant* goo_string { i8* data, i64 len } value from `len` raw bytes
// (embedded NULs preserved). Builder-free, so it is valid at global scope — used
// by both string-literal codegen and folded const-string tables. The bytes are
// copied into a private unnamed_addr global constant array.
LLVMValueRef codegen_const_string_value(CodeGenerator* codegen, const char* bytes, size_t len);
ValueInfo* codegen_generate_binary_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_unary_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_call_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_index_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
// Widen an integer index value to a signed-correct i64 offset (zero-extend for
// unsigned index types, sign-extend for signed). Prevents a narrow unsigned
// index (e.g. uint8 255) from sign-extending to -1 in an element GEP. Used by
// both the index read path and the index-assignment lvalue path.
LLVMValueRef codegen_widen_index(CodeGenerator* codegen, ValueInfo* idx);
ValueInfo* codegen_generate_slice_index_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_selector_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_struct_lit(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_slice_lit(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_match(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

// Interface codegen (P4-5): vtable construction, boxing, dynamic dispatch.
LLVMValueRef codegen_interface_vtable(CodeGenerator* codegen, TypeChecker* checker,
                                      Type* iface, Type* concrete);
LLVMValueRef codegen_interface_box(CodeGenerator* codegen, TypeChecker* checker,
                                   Type* iface, Type* concrete, LLVMValueRef value);
ValueInfo* codegen_interface_dispatch(CodeGenerator* codegen, TypeChecker* checker,
                                      LLVMValueRef iface_val, Type* iface_type,
                                      const char* method_name,
                                      LLVMValueRef* args, size_t argc);

// Goo extension expression generation
ValueInfo* codegen_generate_try_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_catch_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_channel_send(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_channel_recv(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

// Unsafe operation expression generation
ValueInfo* codegen_generate_ptr_arithmetic(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_ptr_deref(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_addr_of(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_port_io(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_mmio_access(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

#if LLVM_AVAILABLE
// Type mapping functions
LLVMTypeRef codegen_type_to_llvm(CodeGenerator* codegen, const Type* type);
LLVMTypeRef codegen_get_basic_type(CodeGenerator* codegen, TypeKind kind);
LLVMTypeRef codegen_get_array_type(CodeGenerator* codegen, const Type* type);
LLVMTypeRef codegen_get_struct_type(CodeGenerator* codegen, const Type* type);
LLVMTypeRef codegen_get_function_type(CodeGenerator* codegen, const Type* type);
LLVMTypeRef codegen_get_pointer_type(CodeGenerator* codegen, const Type* type);

// Special Goo type mappings
LLVMTypeRef codegen_get_enum_type(CodeGenerator* codegen, const Type* type);
LLVMTypeRef codegen_get_error_union_type(CodeGenerator* codegen, const Type* type);
LLVMTypeRef codegen_get_nullable_type(CodeGenerator* codegen, const Type* type);
LLVMTypeRef codegen_get_channel_type(CodeGenerator* codegen, const Type* type);

// Value management
ValueInfo* value_info_new(const char* name, LLVMValueRef llvm_value, Type* goo_type);
void value_info_free(ValueInfo* info);
ValueInfo* codegen_lookup_value(CodeGenerator* codegen, const char* name);
int codegen_add_value(CodeGenerator* codegen, ValueInfo* info);

// Function management
FunctionInfo* function_info_new(const char* name, LLVMValueRef function, Type* goo_type);
void function_info_free(FunctionInfo* info);
int codegen_enter_function(CodeGenerator* codegen, FunctionInfo* func_info);
void codegen_exit_function(CodeGenerator* codegen);

// Helper functions
LLVMValueRef codegen_create_alloca(CodeGenerator* codegen, LLVMTypeRef type, const char* name);
LLVMValueRef codegen_create_entry_alloca(CodeGenerator* codegen, LLVMTypeRef type, const char* name);
LLVMValueRef codegen_alloc_local(CodeGenerator* codegen, LLVMTypeRef type, const char* name);

// Map values ride an 8-byte runtime slot (i64). Convert a value of the
// declared map value-type V to the slot (ptrtoint / zext-or-trunc) and back
// (inttoptr / trunc-or-ext). The type checker guarantees V fits the slot
// (integer/bool/char/pointer).
LLVMValueRef codegen_map_value_to_slot(CodeGenerator* codegen, LLVMValueRef value, Type* value_type);
LLVMValueRef codegen_map_slot_to_value(CodeGenerator* codegen, LLVMValueRef slot, Type* value_type);
LLVMBasicBlockRef codegen_create_block(CodeGenerator* codegen, const char* name);
void codegen_set_insert_point(CodeGenerator* codegen, LLVMBasicBlockRef block);

// Conversion and casting
LLVMValueRef codegen_convert_value(CodeGenerator* codegen, LLVMValueRef value, 
                                 LLVMTypeRef from_type, LLVMTypeRef to_type);
int codegen_types_compatible(LLVMTypeRef from, LLVMTypeRef to);

// Error union helpers
LLVMValueRef codegen_create_error_union_value(CodeGenerator* codegen, LLVMTypeRef union_type,
                                            LLVMValueRef value, int is_error);
LLVMValueRef codegen_extract_error_union_value(CodeGenerator* codegen, LLVMValueRef union_value, int get_error);
LLVMValueRef codegen_check_error_union(CodeGenerator* codegen, LLVMValueRef union_value);

// Nullable type helpers
LLVMValueRef codegen_create_nullable_value(CodeGenerator* codegen, LLVMTypeRef nullable_type,
                                         LLVMValueRef value, int is_null);
LLVMValueRef codegen_extract_nullable_value(CodeGenerator* codegen, LLVMValueRef nullable_value);
LLVMValueRef codegen_check_nullable_null(CodeGenerator* codegen, LLVMValueRef nullable_value);

// Nullable codegen — used across expression_codegen.c, function_codegen.c,
// and call_codegen.c (default-nil locals/globals, reassignment, ?T arg wrap).
ValueInfo* codegen_generate_null_literal(CodeGenerator* codegen, TypeChecker* checker, Type* expected_type);
LLVMValueRef codegen_create_nullable_null(CodeGenerator* codegen, LLVMTypeRef nullable_type, Type* base_type);
LLVMValueRef codegen_create_nullable_with_value(CodeGenerator* codegen, LLVMTypeRef nullable_type,
                                               LLVMValueRef value, Type* value_type);
int codegen_generate_nullable_assignment(CodeGenerator* codegen, TypeChecker* checker,
                                        LLVMValueRef nullable_target, LLVMValueRef source_value,
                                        Type* target_type, Type* source_type, Position pos);

// Error union helpers
LLVMValueRef codegen_create_error_union_success(CodeGenerator* codegen, LLVMTypeRef union_type, 
                                               LLVMValueRef value, Type* value_type);
LLVMValueRef codegen_create_error_union_error(CodeGenerator* codegen, LLVMTypeRef union_type, 
                                             LLVMValueRef error_value);
LLVMValueRef codegen_error_union_is_error(CodeGenerator* codegen, LLVMValueRef error_union);
LLVMValueRef codegen_error_union_get_value(CodeGenerator* codegen, LLVMValueRef error_union);
LLVMValueRef codegen_error_union_get_error(CodeGenerator* codegen, LLVMValueRef error_union);

// Runtime function declarations
#if LLVM_AVAILABLE
LLVMValueRef codegen_declare_runtime_functions(CodeGenerator* codegen);
LLVMValueRef codegen_get_runtime_function(CodeGenerator* codegen, const char* name);
LLVMValueRef codegen_call_runtime_function(CodeGenerator* codegen, const char* name, 
                                          LLVMValueRef* args, unsigned arg_count);
#else
int codegen_declare_runtime_functions(CodeGenerator* codegen);
int codegen_get_runtime_function(CodeGenerator* codegen, const char* name);
int codegen_call_runtime_function(CodeGenerator* codegen, const char* name, 
                                  void* args, unsigned arg_count);
#endif

// Channel operation helpers
LLVMValueRef codegen_generate_channel_send_call(CodeGenerator* codegen, LLVMValueRef channel, LLVMValueRef value);
LLVMValueRef codegen_generate_channel_recv_call(CodeGenerator* codegen, LLVMValueRef channel);
ValueInfo* codegen_generate_make_chan_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

// Built-in function helpers
ValueInfo* codegen_generate_println_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_print_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

// Error return helper - works with LLVM types
LLVMValueRef codegen_generate_error_return(CodeGenerator* codegen, LLVMValueRef return_value, 
                                         Type* return_type, Type* function_return_type);

#else

// Stub version for when LLVM is not available
void* codegen_generate_error_return_stub(CodeGenerator* codegen, void* return_value, 
                                        Type* return_type, Type* function_return_type);

// Map the function name to the stub
#define codegen_generate_error_return codegen_generate_error_return_stub

#endif

// Output generation
int codegen_emit_llvm_ir(CodeGenerator* codegen, const char* filename);
int codegen_emit_object_file(CodeGenerator* codegen, const char* filename);
int codegen_emit_executable(CodeGenerator* codegen, const char* filename);
int codegen_optimize(CodeGenerator* codegen, int level);
int codegen_verify_module(CodeGenerator* codegen);

// WebAssembly target support
#if LLVM_AVAILABLE
int codegen_is_wasm_target(CodeGenerator* codegen);
int codegen_add_wasm_export(CodeGenerator* codegen, LLVMValueRef function, const char* export_name);
int codegen_add_wasm_import(CodeGenerator* codegen, LLVMValueRef function, 
                           const char* module_name, const char* import_name);
int codegen_declare_wasm_runtime_functions(CodeGenerator* codegen);
int codegen_configure_wasm_concurrency(CodeGenerator* codegen);
#endif

// Error reporting
void codegen_error(CodeGenerator* codegen, Position pos, const char* format, ...);
void codegen_warning(CodeGenerator* codegen, Position pos, const char* format, ...);

#endif // CODEGEN_H