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

// Code generation entry points
int codegen_generate_program(CodeGenerator* codegen, TypeChecker* checker, ASTNode* program);
int codegen_generate_declaration(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl);
int codegen_generate_statement(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
ValueInfo* codegen_generate_expression(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

// Declaration generation
int codegen_generate_function_decl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl);
int codegen_generate_var_decl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl);
int codegen_generate_const_decl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl);

// Statement generation
int codegen_generate_block_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_expr_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_if_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_for_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_return_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_go_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
int codegen_generate_defer_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt);
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
ValueInfo* codegen_generate_binary_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_unary_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_call_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_index_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_selector_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_struct_lit(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_slice_lit(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

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