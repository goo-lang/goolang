#include "comptime.h"
#include "types.h"
#include "ast.h"
#include "token.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>


// Comptime value & context plumbing: ComptimeContext lifecycle and
// var/func binding, ComptimeValue constructors/copy/free/converters,
// error and result types. Split from comptime.c (refactor, no
// behavior change). The eval engine stays in comptime.c; intrinsics
// and the codegen pipeline live in comptime_intrinsics.c.
// asprintf may not be available on all systems
#ifndef asprintf
int asprintf(char **strp, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int size = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    
    if (size < 0) return -1;
    
    *strp = malloc(size + 1);
    if (!*strp) return -1;
    
    va_start(args, fmt);
    vsnprintf(*strp, size + 1, fmt, args);
    va_end(args);
    
    return size;
}
#endif

// Create a new compile-time context
ComptimeContext* comptime_context_new(ComptimeContext* parent) {
    ComptimeContext* ctx = xmalloc(sizeof(ComptimeContext));
    if (!ctx) return NULL;
    
    // Initialize variable bindings
    ctx->var_names = NULL;
    ctx->var_values = NULL;
    ctx->var_count = 0;
    ctx->var_capacity = 0;
    
    // Initialize function bindings
    ctx->func_names = NULL;
    ctx->func_nodes = NULL;
    ctx->func_count = 0;
    ctx->func_capacity = 0;
    
    // Initialize type bindings
    ctx->type_names = NULL;
    ctx->type_values = NULL;
    ctx->type_count = 0;
    ctx->type_capacity = 0;
    
    // Set parent context
    ctx->parent = parent;
    
    // Initialize error tracking
    ctx->errors = NULL;
    
    // Set execution limits
    ctx->max_recursion_depth = 100;
    ctx->current_recursion_depth = 0;
    ctx->max_iterations = 10000;
    ctx->iteration_count = 0;
    
    // Initialize generated code buffer
    ctx->generated_code = NULL;
    ctx->generated_code_size = 0;
    ctx->generated_code_capacity = 0;
    
    return ctx;
}

// Free a compile-time context
void comptime_context_free(ComptimeContext* ctx) {
    if (!ctx) return;
    
    // Free variable bindings
    for (size_t i = 0; i < ctx->var_count; i++) {
        free(ctx->var_names[i]);
        comptime_value_free(ctx->var_values[i]);
    }
    free(ctx->var_names);
    free(ctx->var_values);
    
    // Free function bindings (names only, AST nodes are owned elsewhere)
    for (size_t i = 0; i < ctx->func_count; i++) {
        free(ctx->func_names[i]);
    }
    free(ctx->func_names);
    free(ctx->func_nodes);
    
    // Free type bindings (names only, types are owned elsewhere)
    for (size_t i = 0; i < ctx->type_count; i++) {
        free(ctx->type_names[i]);
    }
    free(ctx->type_names);
    free(ctx->type_values);
    
    // Free errors
    ComptimeError* error = ctx->errors;
    while (error) {
        ComptimeError* next = error->next;
        comptime_error_free(error);
        error = next;
    }
    
    // Free generated code buffer
    free(ctx->generated_code);
    
    free(ctx);
}

// Create a new compile-time value
ComptimeValue* comptime_value_new(ComptimeValueType type) {
    ComptimeValue* value = xmalloc(sizeof(ComptimeValue));
    if (!value) return NULL;
    
    value->type = type;
    
    // Initialize based on type
    switch (type) {
        case COMPTIME_VALUE_INT:
            value->int_value = 0;
            break;
        case COMPTIME_VALUE_FLOAT:
            value->float_value = 0.0;
            break;
        case COMPTIME_VALUE_BOOL:
            value->bool_value = false;
            break;
        case COMPTIME_VALUE_STRING:
            value->string_value = NULL;
            break;
        case COMPTIME_VALUE_ARRAY:
            value->array_value.elements = NULL;
            value->array_value.count = 0;
            value->array_value.capacity = 0;
            break;
        case COMPTIME_VALUE_STRUCT:
            value->struct_value.field_names = NULL;
            value->struct_value.field_values = NULL;
            value->struct_value.field_count = 0;
            break;
        case COMPTIME_VALUE_FUNCTION:
            value->function_value.function_node = NULL;
            value->function_value.closure = NULL;
            break;
        case COMPTIME_VALUE_TYPE:
            value->type_value = NULL;
            break;
        case COMPTIME_VALUE_NULL:
        case COMPTIME_VALUE_UNDEFINED:
            // No initialization needed
            break;
    }
    
    return value;
}

// Copy a compile-time value
ComptimeValue* comptime_value_copy(const ComptimeValue* value) {
    if (!value) return NULL;
    
    ComptimeValue* copy = comptime_value_new(value->type);
    if (!copy) return NULL;
    
    switch (value->type) {
        case COMPTIME_VALUE_INT:
            copy->int_value = value->int_value;
            break;
        case COMPTIME_VALUE_FLOAT:
            copy->float_value = value->float_value;
            break;
        case COMPTIME_VALUE_BOOL:
            copy->bool_value = value->bool_value;
            break;
        case COMPTIME_VALUE_STRING:
            if (value->string_value) {
                copy->string_value = strdup(value->string_value);
            }
            break;
        case COMPTIME_VALUE_ARRAY:
            copy->array_value.count = value->array_value.count;
            copy->array_value.capacity = value->array_value.capacity;
            if (value->array_value.elements) {
                copy->array_value.elements = malloc(sizeof(ComptimeValue*) * copy->array_value.capacity);
                for (size_t i = 0; i < copy->array_value.count; i++) {
                    copy->array_value.elements[i] = comptime_value_copy(value->array_value.elements[i]);
                }
            }
            break;
        case COMPTIME_VALUE_STRUCT:
            copy->struct_value.field_count = value->struct_value.field_count;
            if (value->struct_value.field_names && value->struct_value.field_values) {
                copy->struct_value.field_names = malloc(sizeof(char*) * copy->struct_value.field_count);
                copy->struct_value.field_values = malloc(sizeof(ComptimeValue*) * copy->struct_value.field_count);
                for (size_t i = 0; i < copy->struct_value.field_count; i++) {
                    copy->struct_value.field_names[i] = strdup(value->struct_value.field_names[i]);
                    copy->struct_value.field_values[i] = comptime_value_copy(value->struct_value.field_values[i]);
                }
            }
            break;
        case COMPTIME_VALUE_FUNCTION:
            copy->function_value.function_node = value->function_value.function_node;
            copy->function_value.closure = value->function_value.closure; // Shallow copy
            break;
        case COMPTIME_VALUE_TYPE:
            copy->type_value = value->type_value; // Shallow copy
            break;
        case COMPTIME_VALUE_NULL:
        case COMPTIME_VALUE_UNDEFINED:
            // No copying needed
            break;
    }
    
    return copy;
}

// Free a compile-time value
void comptime_value_free(ComptimeValue* value) {
    if (!value) return;
    
    switch (value->type) {
        case COMPTIME_VALUE_STRING:
            free(value->string_value);
            break;
        case COMPTIME_VALUE_ARRAY:
            for (size_t i = 0; i < value->array_value.count; i++) {
                comptime_value_free(value->array_value.elements[i]);
            }
            free(value->array_value.elements);
            break;
        case COMPTIME_VALUE_STRUCT:
            for (size_t i = 0; i < value->struct_value.field_count; i++) {
                free(value->struct_value.field_names[i]);
                comptime_value_free(value->struct_value.field_values[i]);
            }
            free(value->struct_value.field_names);
            free(value->struct_value.field_values);
            break;
        default:
            // Other types don't need special cleanup
            break;
    }
    
    free(value);
}

// Bind a variable in the context
bool comptime_context_bind_var(ComptimeContext* ctx, const char* name, ComptimeValue* value) {
    if (!ctx || !name || !value) return false;
    
    // Check if we need to expand the arrays
    if (ctx->var_count >= ctx->var_capacity) {
        size_t new_capacity = ctx->var_capacity == 0 ? 8 : ctx->var_capacity * 2;
        
        char** new_names = realloc(ctx->var_names, sizeof(char*) * new_capacity);
        if (!new_names) return false;
        
        ComptimeValue** new_values = realloc(ctx->var_values, sizeof(ComptimeValue*) * new_capacity);
        if (!new_values) {
            free(new_names);
            return false;
        }
        
        ctx->var_names = new_names;
        ctx->var_values = new_values;
        ctx->var_capacity = new_capacity;
    }
    
    // Add the binding
    ctx->var_names[ctx->var_count] = strdup(name);
    ctx->var_values[ctx->var_count] = value;
    ctx->var_count++;
    
    return true;
}

// Lookup a variable in the context
ComptimeValue* comptime_context_lookup_var(ComptimeContext* ctx, const char* name) {
    if (!ctx || !name) return NULL;
    
    // Search in current context
    for (size_t i = 0; i < ctx->var_count; i++) {
        if (strcmp(ctx->var_names[i], name) == 0) {
            return ctx->var_values[i];
        }
    }
    
    // Search in parent context
    if (ctx->parent) {
        return comptime_context_lookup_var(ctx->parent, name);
    }
    
    return NULL;
}

// Bind a function in the context
bool comptime_context_bind_func(ComptimeContext* ctx, const char* name, ASTNode* func_node) {
    if (!ctx || !name || !func_node) return false;
    
    // Check if we need to expand the arrays
    if (ctx->func_count >= ctx->func_capacity) {
        size_t new_capacity = ctx->func_capacity == 0 ? 8 : ctx->func_capacity * 2;
        
        char** new_names = realloc(ctx->func_names, sizeof(char*) * new_capacity);
        if (!new_names) return false;
        
        ASTNode** new_nodes = realloc(ctx->func_nodes, sizeof(ASTNode*) * new_capacity);
        if (!new_nodes) {
            free(new_names);
            return false;
        }
        
        ctx->func_names = new_names;
        ctx->func_nodes = new_nodes;
        ctx->func_capacity = new_capacity;
    }
    
    // Add the binding
    ctx->func_names[ctx->func_count] = strdup(name);
    ctx->func_nodes[ctx->func_count] = func_node;
    ctx->func_count++;
    
    return true;
}

// Lookup a function in the context
ASTNode* comptime_context_lookup_func(ComptimeContext* ctx, const char* name) {
    if (!ctx || !name) return NULL;
    
    // Search in current context
    for (size_t i = 0; i < ctx->func_count; i++) {
        if (strcmp(ctx->func_names[i], name) == 0) {
            return ctx->func_nodes[i];
        }
    }
    
    // Search in parent context
    if (ctx->parent) {
        return comptime_context_lookup_func(ctx->parent, name);
    }
    
    return NULL;
}

// Create compile-time values from basic types
ComptimeValue* comptime_value_from_int(int64_t value) {
    ComptimeValue* val = comptime_value_new(COMPTIME_VALUE_INT);
    if (val) {
        val->int_value = value;
    }
    return val;
}

ComptimeValue* comptime_value_from_float(double value) {
    ComptimeValue* val = comptime_value_new(COMPTIME_VALUE_FLOAT);
    if (val) {
        val->float_value = value;
    }
    return val;
}

ComptimeValue* comptime_value_from_bool(bool value) {
    ComptimeValue* val = comptime_value_new(COMPTIME_VALUE_BOOL);
    if (val) {
        val->bool_value = value;
    }
    return val;
}

ComptimeValue* comptime_value_from_string(const char* value) {
    ComptimeValue* val = comptime_value_new(COMPTIME_VALUE_STRING);
    if (val && value) {
        val->string_value = strdup(value);
    }
    return val;
}

// Create a compile-time value from a literal AST node
ComptimeValue* comptime_value_from_literal(ASTNode* literal) {
    if (!literal || literal->type != AST_LITERAL) return NULL;
    
    LiteralNode* lit = (LiteralNode*)literal;
    
    switch (lit->literal_type) {
        case TOKEN_INT:
            return comptime_value_from_int(atoll(lit->value));
        case TOKEN_FLOAT:
            return comptime_value_from_float(atof(lit->value));
        case TOKEN_TRUE:
            return comptime_value_from_bool(true);
        case TOKEN_FALSE:
            return comptime_value_from_bool(false);
        case TOKEN_STRING:
            return comptime_value_from_string(lit->value);
        case TOKEN_NIL:
            return comptime_value_new(COMPTIME_VALUE_NULL);
        default:
            return NULL;
    }
}

// Check if a value is truthy
bool comptime_value_is_truthy(const ComptimeValue* value) {
    if (!value) return false;
    
    switch (value->type) {
        case COMPTIME_VALUE_INT:
            return value->int_value != 0;
        case COMPTIME_VALUE_FLOAT:
            return value->float_value != 0.0;
        case COMPTIME_VALUE_BOOL:
            return value->bool_value;
        case COMPTIME_VALUE_STRING:
            return value->string_value && strlen(value->string_value) > 0;
        case COMPTIME_VALUE_ARRAY:
            return value->array_value.count > 0;
        case COMPTIME_VALUE_NULL:
        case COMPTIME_VALUE_UNDEFINED:
            return false;
        default:
            return true; // Functions, structs, types are truthy
    }
}

// Convert a value to string representation
char* comptime_value_to_string(const ComptimeValue* value) {
    if (!value) return strdup("null");
    
    char* result = NULL;
    
    switch (value->type) {
        case COMPTIME_VALUE_INT:
            asprintf(&result, "%lld", (long long)value->int_value);
            break;
        case COMPTIME_VALUE_FLOAT:
            asprintf(&result, "%g", value->float_value);
            break;
        case COMPTIME_VALUE_BOOL:
            result = strdup(value->bool_value ? "true" : "false");
            break;
        case COMPTIME_VALUE_STRING:
            result = strdup(value->string_value ? value->string_value : "");
            break;
        case COMPTIME_VALUE_NULL:
            result = strdup("null");
            break;
        case COMPTIME_VALUE_UNDEFINED:
            result = strdup("undefined");
            break;
        case COMPTIME_VALUE_ARRAY:
            // TODO: Implement array string representation
            result = strdup("[array]");
            break;
        case COMPTIME_VALUE_STRUCT:
            // TODO: Implement struct string representation
            result = strdup("{struct}");
            break;
        case COMPTIME_VALUE_FUNCTION:
            result = strdup("<function>");
            break;
        case COMPTIME_VALUE_TYPE:
            result = strdup("<type>");
            break;
    }
    
    return result ? result : strdup("error");
}

// Create a new compile-time error
ComptimeError* comptime_error_new(const char* message, Position pos) {
    ComptimeError* error = xmalloc(sizeof(ComptimeError));
    if (!error) return NULL;
    
    error->message = strdup(message);
    error->position = pos;
    error->next = NULL;
    
    return error;
}

// Free a compile-time error
void comptime_error_free(ComptimeError* error) {
    if (!error) return;
    
    free(error->message);
    free(error);
}

// Add an error to the context
void comptime_context_add_error(ComptimeContext* ctx, ComptimeError* error) {
    if (!ctx || !error) return;
    
    // Add to the front of the error list
    error->next = ctx->errors;
    ctx->errors = error;
}

// Create a new compile-time result
ComptimeResult* comptime_result_new(ComptimeValue* value, ComptimeError* error, char* generated_code) {
    ComptimeResult* result = xmalloc(sizeof(ComptimeResult));
    if (!result) return NULL;
    
    result->value = value;
    result->error = error;
    result->generated_code = generated_code;
    result->is_return = false;

    return result;
}

// Free a compile-time result
void comptime_result_free(ComptimeResult* result) {
    if (!result) return;
    
    comptime_value_free(result->value);
    comptime_error_free(result->error);
    free(result->generated_code);
    free(result);
}

// Forward declarations for evaluation functions
// Additional utility functions for macro system
ComptimeValue* create_comptime_string(const char* str) {
    return comptime_value_from_string(str);
}

ComptimeValue* create_comptime_void(void) {
    ComptimeValue* value = comptime_value_new(COMPTIME_VALUE_NULL);
    return value;
}

ComptimeValue* create_comptime_type(Type* type) {
    ComptimeValue* value = comptime_value_new(COMPTIME_VALUE_TYPE);
    if (value) {
        value->type_value = type;
    }
    return value;
}

bool comptime_value_to_bool(const ComptimeValue* value) {
    return comptime_value_is_truthy(value);
}

ASTNode* comptime_value_to_ast(const ComptimeValue* value) {
    if (!value) return NULL;
    
    Position pos = {0, 0, 0};
    
    switch (value->type) {
        case COMPTIME_VALUE_INT: {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%lld", value->int_value);
            return (ASTNode*)ast_literal_new(TOKEN_INT, buffer, pos);
        }
            
        case COMPTIME_VALUE_FLOAT: {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%f", value->float_value);
            return (ASTNode*)ast_literal_new(TOKEN_FLOAT, buffer, pos);
        }
            
        case COMPTIME_VALUE_BOOL:
            return (ASTNode*)ast_literal_new(TOKEN_IDENT,
                                 value->bool_value ? "true" : "false", pos);
            
        case COMPTIME_VALUE_STRING:
            return (ASTNode*)ast_literal_new(TOKEN_STRING,
                                 value->string_value, pos);
            
        default:
            return NULL;
    }
}

void print_comptime_value(const ComptimeValue* value) {
    if (!value) {
        printf("null");
        return;
    }
    
    char* str = comptime_value_to_string(value);
    printf("%s", str);
    free(str);
}

Type* comptime_value_get_type(const ComptimeValue* value) {
    if (!value) return NULL;
    
    switch (value->type) {
        case COMPTIME_VALUE_INT:
            return type_new(TYPE_INT64);
            
        case COMPTIME_VALUE_FLOAT:
            return type_new(TYPE_FLOAT64);
            
        case COMPTIME_VALUE_BOOL:
            return type_new(TYPE_BOOL);
            
        case COMPTIME_VALUE_STRING:
            return type_new(TYPE_STRING);
            
        case COMPTIME_VALUE_TYPE:
            return value->type_value;
            
        default:
            return NULL;
    }
}
