#include "types.h"
#include "comptime.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// =============================================================================
// Compile-Time Type Context Management
// =============================================================================

ComptimeTypeContext* comptime_type_context_new(ComptimeContext* comptime_ctx) {
    ComptimeTypeContext* ctx = malloc(sizeof(ComptimeTypeContext));
    if (!ctx) return NULL;
    
    ctx->comptime_ctx = comptime_ctx;
    ctx->type_functions = NULL;
    ctx->computed_types = NULL;
    ctx->computed_type_count = 0;
    ctx->computed_type_capacity = 0;
    
    return ctx;
}

void comptime_type_context_free(ComptimeTypeContext* ctx) {
    if (!ctx) return;
    
    // Free type functions
    TypeFunction* func = ctx->type_functions;
    while (func) {
        TypeFunction* next = func->next;
        type_function_free(func);
        func = next;
    }
    
    // Free computed types cache
    for (size_t i = 0; i < ctx->computed_type_count; i++) {
        type_free(ctx->computed_types[i]);
    }
    free(ctx->computed_types);
    
    free(ctx);
}

// =============================================================================
// Type-Level Function Management
// =============================================================================

TypeFunction* type_function_new(const char* name, Type** param_types, size_t param_count, 
                                Type* return_type, ASTNode* body) {
    TypeFunction* func = malloc(sizeof(TypeFunction));
    if (!func) return NULL;
    
    func->name = strdup(name);
    func->param_count = param_count;
    func->return_type = return_type;
    func->body = body;
    func->is_comptime_only = 1;  // Default to compile-time only
    func->next = NULL;
    
    if (param_count > 0) {
        func->param_types = malloc(sizeof(Type*) * param_count);
        if (!func->param_types) {
            free(func->name);
            free(func);
            return NULL;
        }
        memcpy(func->param_types, param_types, sizeof(Type*) * param_count);
    } else {
        func->param_types = NULL;
    }
    
    return func;
}

void type_function_free(TypeFunction* func) {
    if (!func) return;
    
    free(func->name);
    free(func->param_types);
    // Note: We don't free return_type or body as they may be shared
    free(func);
}

int comptime_type_register_function(ComptimeTypeContext* ctx, TypeFunction* func) {
    if (!ctx || !func) return 0;
    
    // Add to linked list
    func->next = ctx->type_functions;
    ctx->type_functions = func;
    
    return 1;
}

TypeFunction* comptime_type_lookup_function(ComptimeTypeContext* ctx, const char* name) {
    if (!ctx || !name) return NULL;
    
    TypeFunction* func = ctx->type_functions;
    while (func) {
        if (strcmp(func->name, name) == 0) {
            return func;
        }
        func = func->next;
    }
    
    return NULL;
}

// =============================================================================
// Compile-Time Type Computation
// =============================================================================

ComptimeTypeResult* comptime_type_result_new(Type* type, ComptimeValue* value) {
    ComptimeTypeResult* result = malloc(sizeof(ComptimeTypeResult));
    if (!result) return NULL;
    
    result->type = type;
    result->value = value;
    result->is_valid = (type != NULL);
    result->error_message = NULL;
    
    return result;
}

void comptime_type_result_free(ComptimeTypeResult* result) {
    if (!result) return;
    
    // Note: We don't free type or value as they may be managed elsewhere
    free(result->error_message);
    free(result);
}

ComptimeTypeResult* comptime_type_evaluate(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || !checker->comptime_type_ctx) {
        return NULL;
    }
    
    // Evaluate the expression at compile time
    ComptimeContext* ctx = checker->comptime_type_ctx->comptime_ctx;
    ComptimeResult* result = comptime_evaluate_expression(ctx, expr);
    
    if (!result || result->type != COMPTIME_RESULT_VALUE) {
        ComptimeTypeResult* type_result = comptime_type_result_new(NULL, NULL);
        if (type_result && result && result->type == COMPTIME_RESULT_ERROR) {
            type_result->error_message = strdup(result->data.error.message);
        }
        comptime_result_free(result);
        return type_result;
    }
    
    // Convert the compile-time value to a type
    Type* computed_type = comptime_type_from_value(&result->data.value);
    ComptimeTypeResult* type_result = comptime_type_result_new(computed_type, &result->data.value);
    
    // Don't free the result yet as we're sharing the value
    return type_result;
}

Type* comptime_type_from_value(ComptimeValue* value) {
    if (!value) return NULL;
    
    switch (value->type) {
        case COMPTIME_VALUE_INT:
            // For integer values, we can create array types with compile-time sizes
            return type_int(64, 1);  // Default to int64
            
        case COMPTIME_VALUE_FLOAT:
            return type_float(64);  // Default to float64
            
        case COMPTIME_VALUE_STRING: {
            // String values can represent type names
            const char* type_name = value->string_value;
            if (strcmp(type_name, "int") == 0) return type_int(32, 1);
            if (strcmp(type_name, "int64") == 0) return type_int(64, 1);
            if (strcmp(type_name, "float") == 0) return type_float(32);
            if (strcmp(type_name, "float64") == 0) return type_float(64);
            if (strcmp(type_name, "bool") == 0) return type_bool();
            if (strcmp(type_name, "string") == 0) return type_string_type();
            // For unknown type names, return NULL
            return NULL;
        }
        
        case COMPTIME_VALUE_BOOL:
            return type_bool();
            
        case COMPTIME_VALUE_ARRAY: {
            // Array values can be used to create array types
            // For now, create an array of the element type
            if (value->array_value.count > 0) {
                Type* element_type = comptime_type_from_value(value->array_value.elements[0]);
                if (element_type) {
                    return type_array(element_type, value->array_value.count);
                }
            }
            return NULL;
        }
        
        default:
            return NULL;
    }
}

Type* comptime_type_call_function(TypeChecker* checker, const char* func_name, 
                                 ComptimeValue** args, size_t arg_count) {
    if (!checker || !func_name || !checker->comptime_type_ctx) {
        return NULL;
    }
    
    TypeFunction* func = comptime_type_lookup_function(checker->comptime_type_ctx, func_name);
    if (!func) {
        return NULL;
    }
    
    // For now, implement some built-in type functions
    if (strcmp(func_name, "Array") == 0 && arg_count == 2) {
        // Array(T, N) -> [N]T
        Type* element_type = comptime_type_from_value(args[0]);
        if (element_type && args[1]->type == COMPTIME_VALUE_INT) {
            return type_array(element_type, (size_t)args[1]->int_value);
        }
    }
    
    if (strcmp(func_name, "Slice") == 0 && arg_count == 1) {
        // Slice(T) -> []T
        Type* element_type = comptime_type_from_value(args[0]);
        if (element_type) {
            return type_slice(element_type);
        }
    }
    
    if (strcmp(func_name, "Pointer") == 0 && arg_count == 1) {
        // Pointer(T) -> *T
        Type* pointee_type = comptime_type_from_value(args[0]);
        if (pointee_type) {
            return type_pointer(pointee_type);
        }
    }
    
    return NULL;
}

// =============================================================================
// Type Checking with Compile-Time Support
// =============================================================================

Type* type_check_comptime_expr(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr) return NULL;
    
    // First, try regular type checking
    Type* regular_type = type_check_expression(checker, expr);
    if (!regular_type) return NULL;
    
    // If we have compile-time context, try to evaluate at compile time
    if (checker->comptime_type_ctx) {
        ComptimeTypeResult* comptime_result = comptime_type_evaluate(checker, expr);
        if (comptime_result && comptime_result->is_valid && comptime_result->type) {
            // Use the compile-time computed type if available
            Type* comptime_type = comptime_result->type;
            comptime_type_result_free(comptime_result);
            return comptime_type;
        }
        comptime_type_result_free(comptime_result);
    }
    
    return regular_type;
}

int type_check_comptime_block(TypeChecker* checker, ASTNode* block) {
    if (!checker || !block) return 0;
    
    if (block->type != AST_COMPTIME_BLOCK) {
        return type_check_statement(checker, block);
    }
    
    ComptimeBlockNode* comptime_block = &block->data.comptime_block;
    
    // Evaluate the comptime block
    if (checker->comptime_type_ctx) {
        ComptimeContext* ctx = checker->comptime_type_ctx->comptime_ctx;
        ComptimeResult* result = comptime_evaluate_statement(ctx, comptime_block->statement);
        
        if (result && result->type == COMPTIME_RESULT_ERROR) {
            // Report compile-time error
            type_error(checker, block->position, "Compile-time error: %s", 
                      result->data.error.message);
            comptime_result_free(result);
            return 0;
        }
        
        comptime_result_free(result);
    }
    
    return 1;
}

// =============================================================================
// Dependent Type Support
// =============================================================================

Type* type_create_dependent(Type* base_type, ComptimeValue* constraint_value) {
    if (!base_type || !constraint_value) return NULL;
    
    // For now, create a qualified type with the constraint as metadata
    // In a full implementation, this would create a proper dependent type
    return type_qualified(base_type, OWNERSHIP_OWNED, MUTABILITY_IMMUTABLE);
}

int type_validate_dependent_constraint(Type* dependent_type, ComptimeValue* value) {
    if (!dependent_type || !value) return 0;
    
    // For now, just return true
    // In a full implementation, this would validate the constraint
    return 1;
}

// =============================================================================
// Built-in Type-Level Functions
// =============================================================================

void comptime_type_register_builtins(ComptimeTypeContext* ctx) {
    if (!ctx) return;
    
    // Register built-in type functions
    
    // Array(T, N) -> [N]T
    TypeFunction* array_func = type_function_new("Array", NULL, 2, NULL, NULL);
    if (array_func) {
        comptime_type_register_function(ctx, array_func);
    }
    
    // Slice(T) -> []T  
    TypeFunction* slice_func = type_function_new("Slice", NULL, 1, NULL, NULL);
    if (slice_func) {
        comptime_type_register_function(ctx, slice_func);
    }
    
    // Pointer(T) -> *T
    TypeFunction* pointer_func = type_function_new("Pointer", NULL, 1, NULL, NULL);
    if (pointer_func) {
        comptime_type_register_function(ctx, pointer_func);
    }
    
    // Map(K, V) -> map[K]V
    TypeFunction* map_func = type_function_new("Map", NULL, 2, NULL, NULL);
    if (map_func) {
        comptime_type_register_function(ctx, map_func);
    }
    
    // Channel(T) -> chan T
    TypeFunction* channel_func = type_function_new("Channel", NULL, 1, NULL, NULL);
    if (channel_func) {
        comptime_type_register_function(ctx, channel_func);
    }
    
    // SizeOf(T) -> int (compile-time)
    TypeFunction* sizeof_func = type_function_new("SizeOf", NULL, 1, type_int(64, 0), NULL);
    if (sizeof_func) {
        comptime_type_register_function(ctx, sizeof_func);
    }
    
    // AlignOf(T) -> int (compile-time)
    TypeFunction* alignof_func = type_function_new("AlignOf", NULL, 1, type_int(64, 0), NULL);
    if (alignof_func) {
        comptime_type_register_function(ctx, alignof_func);
    }
}

// =============================================================================
// TypeChecker Integration
// =============================================================================

// Initialize compile-time type support in a type checker
int type_checker_init_comptime(TypeChecker* checker, ComptimeContext* comptime_ctx) {
    if (!checker || !comptime_ctx) return 0;
    
    checker->comptime_type_ctx = comptime_type_context_new(comptime_ctx);
    if (!checker->comptime_type_ctx) return 0;
    
    // Register built-in type functions
    comptime_type_register_builtins(checker->comptime_type_ctx);
    
    return 1;
}

// Clean up compile-time type support in a type checker
void type_checker_cleanup_comptime(TypeChecker* checker) {
    if (!checker) return;
    
    comptime_type_context_free(checker->comptime_type_ctx);
    checker->comptime_type_ctx = NULL;
}
