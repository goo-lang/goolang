#ifndef COMPTIME_H
#define COMPTIME_H

#include "ast.h"
#include "types.h"
#include <stdbool.h>
#include <stdint.h>

// Forward declarations
typedef struct ComptimeContext ComptimeContext;
typedef struct ComptimeValue ComptimeValue;
typedef struct ComptimeError ComptimeError;

// Compile-time value types
typedef enum {
    COMPTIME_VALUE_INT,
    COMPTIME_VALUE_FLOAT, 
    COMPTIME_VALUE_BOOL,
    COMPTIME_VALUE_STRING,
    COMPTIME_VALUE_ARRAY,
    COMPTIME_VALUE_STRUCT,
    COMPTIME_VALUE_FUNCTION,
    COMPTIME_VALUE_TYPE,
    COMPTIME_VALUE_NULL,
    COMPTIME_VALUE_UNDEFINED
} ComptimeValueType;

// Compile-time value representation
typedef struct ComptimeValue {
    ComptimeValueType type;
    union {
        int64_t int_value;
        double float_value;
        bool bool_value;
        char* string_value;
        struct {
            ComptimeValue** elements;
            size_t count;
            size_t capacity;
        } array_value;
        struct {
            char** field_names;
            ComptimeValue** field_values;
            size_t field_count;
        } struct_value;
        struct {
            ASTNode* function_node;
            ComptimeContext* closure;
        } function_value;
        Type* type_value;
    };
} ComptimeValue;

// Compile-time error information
typedef struct ComptimeError {
    char* message;
    Position position;
    struct ComptimeError* next;
} ComptimeError;

// Compile-time execution context
typedef struct ComptimeContext {
    // Variable bindings
    char** var_names;
    ComptimeValue** var_values;
    size_t var_count;
    size_t var_capacity;
    
    // Function bindings
    char** func_names;
    ASTNode** func_nodes;
    size_t func_count;
    size_t func_capacity;
    
    // Type bindings
    char** type_names;
    Type** type_values;
    size_t type_count;
    size_t type_capacity;
    
    // Parent context for lexical scoping
    struct ComptimeContext* parent;
    
    // Error tracking
    ComptimeError* errors;
    
    // Execution limits
    size_t max_recursion_depth;
    size_t current_recursion_depth;
    size_t max_iterations;
    size_t iteration_count;
    
    // Generated code buffer
    char* generated_code;
    size_t generated_code_size;
    size_t generated_code_capacity;
} ComptimeContext;

// Compile-time interpreter result
typedef struct {
    ComptimeValue* value;
    ComptimeError* error;
    char* generated_code; // For @emit and code generation
    // True when this result carries a return-statement value. Block walkers
    // stop iterating and propagate up so subsequent statements in the same
    // block don't overwrite the function's return value.
    bool is_return;
} ComptimeResult;

// Core compile-time execution functions
ComptimeContext* comptime_context_new(ComptimeContext* parent);
void comptime_context_free(ComptimeContext* ctx);

ComptimeValue* comptime_value_new(ComptimeValueType type);
ComptimeValue* comptime_value_copy(const ComptimeValue* value);
void comptime_value_free(ComptimeValue* value);

// Variable and function binding
bool comptime_context_bind_var(ComptimeContext* ctx, const char* name, ComptimeValue* value);
bool comptime_context_bind_func(ComptimeContext* ctx, const char* name, ASTNode* func_node);
bool comptime_context_bind_type(ComptimeContext* ctx, const char* name, Type* type);

ComptimeValue* comptime_context_lookup_var(ComptimeContext* ctx, const char* name);
ASTNode* comptime_context_lookup_func(ComptimeContext* ctx, const char* name);
Type* comptime_context_lookup_type(ComptimeContext* ctx, const char* name);

// Compile-time evaluation
ComptimeResult* comptime_eval_block(ComptimeContext* ctx, ASTNode* block);
ComptimeResult* comptime_eval_expression(ComptimeContext* ctx, ASTNode* expr);
ComptimeResult* comptime_eval_statement(ComptimeContext* ctx, ASTNode* stmt);
ComptimeResult* comptime_eval_function_call(ComptimeContext* ctx, ASTNode* call);
// Enhanced variants (M11 recursion engine) — used by the intrinsics
// unit (comptime_eval_function_call_advanced delegates to enhanced).
ComptimeResult* comptime_eval_statement_enhanced(ComptimeContext* ctx, ASTNode* stmt);
ComptimeResult* comptime_eval_function_call_enhanced(ComptimeContext* ctx, ASTNode* call);

// Compile-time intrinsics
ComptimeResult* comptime_intrinsic_emit(ComptimeContext* ctx, ComptimeValue* code);
ComptimeResult* comptime_intrinsic_typeof(ComptimeContext* ctx, ComptimeValue* value);
ComptimeResult* comptime_intrinsic_sizeof(ComptimeContext* ctx, ComptimeValue* value);

// Code generation pipeline
typedef struct CodeGenPipeline {
    char** generated_functions;
    size_t function_count;
    size_t function_capacity;
    
    char** generated_types;
    size_t type_count;
    size_t type_capacity;
    
    char** generated_constants;
    size_t constant_count;
    size_t constant_capacity;
} CodeGenPipeline;

// Advanced code generation functions
ComptimeResult* comptime_intrinsic_generate_template(ComptimeContext* ctx, ComptimeValue* template, ComptimeValue** args, size_t arg_count);
ComptimeResult* comptime_intrinsic_generate_loop(ComptimeContext* ctx, ComptimeValue* count, ComptimeValue* template);
ComptimeResult* comptime_intrinsic_struct_fields(ComptimeContext* ctx, ComptimeValue* struct_type);
ComptimeResult* comptime_intrinsic_format(ComptimeContext* ctx, ComptimeValue* format_str, ComptimeValue** args, size_t arg_count);

// Code generation pipeline
CodeGenPipeline* comptime_codegen_pipeline_new(void);
void comptime_codegen_pipeline_free(CodeGenPipeline* pipeline);
bool comptime_codegen_pipeline_add_function(CodeGenPipeline* pipeline, const char* function_code);
char* comptime_codegen_pipeline_finalize(CodeGenPipeline* pipeline);

// Enhanced function evaluation
ComptimeResult* comptime_eval_function_call_advanced(ComptimeContext* ctx, ASTNode* call);

// Utility functions
ComptimeValue* comptime_value_from_literal(ASTNode* literal);
ComptimeValue* comptime_value_from_int(int64_t value);
ComptimeValue* comptime_value_from_float(double value);
ComptimeValue* comptime_value_from_bool(bool value);
ComptimeValue* comptime_value_from_string(const char* value);

bool comptime_value_is_truthy(const ComptimeValue* value);
int comptime_value_compare(const ComptimeValue* a, const ComptimeValue* b);
char* comptime_value_to_string(const ComptimeValue* value);

// Additional utility functions for macro system
ComptimeValue* create_comptime_string(const char* str);
ComptimeValue* create_comptime_void(void);
ComptimeValue* create_comptime_type(Type* type);
bool comptime_value_to_bool(const ComptimeValue* value);
ASTNode* comptime_value_to_ast(const ComptimeValue* value);
void print_comptime_value(const ComptimeValue* value);
Type* comptime_value_get_type(const ComptimeValue* value);

// Error handling
ComptimeError* comptime_error_new(const char* message, Position pos);
void comptime_error_free(ComptimeError* error);
void comptime_context_add_error(ComptimeContext* ctx, ComptimeError* error);

// Result management
ComptimeResult* comptime_result_new(ComptimeValue* value, ComptimeError* error, char* generated_code);
void comptime_result_free(ComptimeResult* result);

// Main entry point for compile-time execution
ComptimeResult* execute_comptime_block(ASTNode* comptime_block, ComptimeContext* global_ctx);

#endif // COMPTIME_H
