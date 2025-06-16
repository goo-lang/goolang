#include "comptime.h"
#include "types.h"
#include "ast.h"
#include "token.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>

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
    ComptimeContext* ctx = malloc(sizeof(ComptimeContext));
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
    ComptimeValue* value = malloc(sizeof(ComptimeValue));
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
    ComptimeError* error = malloc(sizeof(ComptimeError));
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
    ComptimeResult* result = malloc(sizeof(ComptimeResult));
    if (!result) return NULL;
    
    result->value = value;
    result->error = error;
    result->generated_code = generated_code;
    
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
static ComptimeResult* comptime_eval_binary_expr(ComptimeContext* ctx, ASTNode* expr);
static ComptimeResult* comptime_eval_unary_expr(ComptimeContext* ctx, ASTNode* expr);
static ComptimeResult* comptime_eval_identifier(ComptimeContext* ctx, ASTNode* expr);

// Evaluate an expression at compile time
ComptimeResult* comptime_eval_expression(ComptimeContext* ctx, ASTNode* expr) {
    if (!ctx || !expr) {
        return comptime_result_new(NULL, comptime_error_new("Invalid context or expression", (Position){0}), NULL);
    }
    
    switch (expr->type) {
        case AST_LITERAL: {
            ComptimeValue* value = comptime_value_from_literal(expr);
            return comptime_result_new(value, NULL, NULL);
        }
        
        case AST_IDENTIFIER: {
            return comptime_eval_identifier(ctx, expr);
        }
        
        case AST_BINARY_EXPR: {
            return comptime_eval_binary_expr(ctx, expr);
        }
        
        case AST_UNARY_EXPR: {
            return comptime_eval_unary_expr(ctx, expr);
        }
        
        case AST_CALL_EXPR: {
            return comptime_eval_function_call(ctx, expr);
        }
        
        default: {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "Unsupported expression type in comptime evaluation: %d", expr->type);
            return comptime_result_new(NULL, comptime_error_new(error_msg, expr->pos), NULL);
        }
    }
}

// Evaluate an identifier
static ComptimeResult* comptime_eval_identifier(ComptimeContext* ctx, ASTNode* expr) {
    IdentifierNode* ident = (IdentifierNode*)expr;
    ComptimeValue* value = comptime_context_lookup_var(ctx, ident->name);
    
    if (!value) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Undefined variable in comptime context: %s", ident->name);
        return comptime_result_new(NULL, comptime_error_new(error_msg, expr->pos), NULL);
    }
    
    return comptime_result_new(comptime_value_copy(value), NULL, NULL);
}

// Evaluate a binary expression
static ComptimeResult* comptime_eval_binary_expr(ComptimeContext* ctx, ASTNode* expr) {
    BinaryExprNode* binary = (BinaryExprNode*)expr;
    
    // Evaluate left operand
    ComptimeResult* left_result = comptime_eval_expression(ctx, binary->left);
    if (left_result->error) {
        return left_result;
    }
    
    // Evaluate right operand
    ComptimeResult* right_result = comptime_eval_expression(ctx, binary->right);
    if (right_result->error) {
        comptime_result_free(left_result);
        return right_result;
    }
    
    ComptimeValue* left_val = left_result->value;
    ComptimeValue* right_val = right_result->value;
    ComptimeValue* result_val = NULL;
    
    // Perform the operation based on operator type
    switch (binary->operator) {
        case TOKEN_PLUS:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_int(left_val->int_value + right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_float(left_f + right_f);
            } else if (left_val->type == COMPTIME_VALUE_STRING || right_val->type == COMPTIME_VALUE_STRING) {
                // String concatenation
                char* left_str = comptime_value_to_string(left_val);
                char* right_str = comptime_value_to_string(right_val);
                char* concat = malloc(strlen(left_str) + strlen(right_str) + 1);
                strcpy(concat, left_str);
                strcat(concat, right_str);
                result_val = comptime_value_from_string(concat);
                free(left_str);
                free(right_str);
                free(concat);
            }
            break;
            
        case TOKEN_MINUS:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_int(left_val->int_value - right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_float(left_f - right_f);
            }
            break;
            
        case TOKEN_MULTIPLY:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_int(left_val->int_value * right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_float(left_f * right_f);
            }
            break;
            
        case TOKEN_DIVIDE:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                if (right_val->int_value == 0) {
                    comptime_result_free(left_result);
                    comptime_result_free(right_result);
                    return comptime_result_new(NULL, comptime_error_new("Division by zero in comptime evaluation", expr->pos), NULL);
                }
                result_val = comptime_value_from_int(left_val->int_value / right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                if (right_f == 0.0) {
                    comptime_result_free(left_result);
                    comptime_result_free(right_result);
                    return comptime_result_new(NULL, comptime_error_new("Division by zero in comptime evaluation", expr->pos), NULL);
                }
                result_val = comptime_value_from_float(left_f / right_f);
            }
            break;
            
        case TOKEN_EQ:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_bool(left_val->int_value == right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_bool(left_f == right_f);
            } else if (left_val->type == COMPTIME_VALUE_BOOL && right_val->type == COMPTIME_VALUE_BOOL) {
                result_val = comptime_value_from_bool(left_val->bool_value == right_val->bool_value);
            }
            break;
            
        case TOKEN_NE:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_bool(left_val->int_value != right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_bool(left_f != right_f);
            } else if (left_val->type == COMPTIME_VALUE_BOOL && right_val->type == COMPTIME_VALUE_BOOL) {
                result_val = comptime_value_from_bool(left_val->bool_value != right_val->bool_value);
            }
            break;
            
        case TOKEN_LT:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_bool(left_val->int_value < right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_bool(left_f < right_f);
            }
            break;
            
        case TOKEN_LE:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_bool(left_val->int_value <= right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_bool(left_f <= right_f);
            }
            break;
            
        case TOKEN_GT:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_bool(left_val->int_value > right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_bool(left_f > right_f);
            }
            break;
            
        case TOKEN_GE:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_bool(left_val->int_value >= right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_bool(left_f >= right_f);
            }
            break;
            
        case TOKEN_AND:
            result_val = comptime_value_from_bool(comptime_value_is_truthy(left_val) && comptime_value_is_truthy(right_val));
            break;
            
        case TOKEN_OR:
            result_val = comptime_value_from_bool(comptime_value_is_truthy(left_val) || comptime_value_is_truthy(right_val));
            break;
            
        default: {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "Unsupported binary operator in comptime evaluation: %d", binary->operator);
            comptime_result_free(left_result);
            comptime_result_free(right_result);
            return comptime_result_new(NULL, comptime_error_new(error_msg, expr->pos), NULL);
        }
    }
    
    comptime_result_free(left_result);
    comptime_result_free(right_result);
    
    if (!result_val) {
        return comptime_result_new(NULL, comptime_error_new("Type error in binary operation", expr->pos), NULL);
    }
    
    return comptime_result_new(result_val, NULL, NULL);
}

// Evaluate a unary expression
static ComptimeResult* comptime_eval_unary_expr(ComptimeContext* ctx, ASTNode* expr) {
    UnaryExprNode* unary = (UnaryExprNode*)expr;
    
    ComptimeResult* operand_result = comptime_eval_expression(ctx, unary->operand);
    if (operand_result->error) {
        return operand_result;
    }
    
    ComptimeValue* operand_val = operand_result->value;
    ComptimeValue* result_val = NULL;
    
    switch (unary->operator) {
        case TOKEN_MINUS:
            if (operand_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_int(-operand_val->int_value);
            } else if (operand_val->type == COMPTIME_VALUE_FLOAT) {
                result_val = comptime_value_from_float(-operand_val->float_value);
            }
            break;
            
        case TOKEN_PLUS:
            if (operand_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_int(operand_val->int_value);
            } else if (operand_val->type == COMPTIME_VALUE_FLOAT) {
                result_val = comptime_value_from_float(operand_val->float_value);
            }
            break;
            
        case TOKEN_NOT:
            result_val = comptime_value_from_bool(!comptime_value_is_truthy(operand_val));
            break;
            
        default: {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "Unsupported unary operator in comptime evaluation: %d", unary->operator);
            comptime_result_free(operand_result);
            return comptime_result_new(NULL, comptime_error_new(error_msg, expr->pos), NULL);
        }
    }
    
    comptime_result_free(operand_result);
    
    if (!result_val) {
        return comptime_result_new(NULL, comptime_error_new("Type error in unary operation", expr->pos), NULL);
    }
    
    return comptime_result_new(result_val, NULL, NULL);
}

// Evaluate a function call
ComptimeResult* comptime_eval_function_call(ComptimeContext* ctx, ASTNode* call) {
    if (!ctx || !call || call->type != AST_CALL_EXPR) {
        return comptime_result_new(NULL, comptime_error_new("Invalid function call", (Position){0}), NULL);
    }
    
    CallExprNode* call_node = (CallExprNode*)call;
    
    // Check if it's a built-in intrinsic function (like @emit)
    if (call_node->function && call_node->function->type == AST_IDENTIFIER) {
        IdentifierNode* func_ident = (IdentifierNode*)call_node->function;
        
        if (strcmp(func_ident->name, "@emit") == 0) {
            // Handle @emit intrinsic
            // For now, assume args is a single argument node
            if (!call_node->args) {
                return comptime_result_new(NULL, comptime_error_new("@emit requires exactly one argument", call->pos), NULL);
            }
            
            ComptimeResult* arg_result = comptime_eval_expression(ctx, call_node->args);
            if (arg_result->error) {
                return arg_result;
            }
            
            return comptime_intrinsic_emit(ctx, arg_result->value);
        }
        
        if (strcmp(func_ident->name, "@typeof") == 0) {
            // Handle @typeof intrinsic
            if (!call_node->args) {
                return comptime_result_new(NULL, comptime_error_new("@typeof requires exactly one argument", call->pos), NULL);
            }
            
            ComptimeResult* arg_result = comptime_eval_expression(ctx, call_node->args);
            if (arg_result->error) {
                return arg_result;
            }
            
            return comptime_intrinsic_typeof(ctx, arg_result->value);
        }
        
        if (strcmp(func_ident->name, "@sizeof") == 0) {
            // Handle @sizeof intrinsic
            if (!call_node->args) {
                return comptime_result_new(NULL, comptime_error_new("@sizeof requires exactly one argument", call->pos), NULL);
            }
            
            ComptimeResult* arg_result = comptime_eval_expression(ctx, call_node->args);
            if (arg_result->error) {
                return arg_result;
            }
            
            return comptime_intrinsic_sizeof(ctx, arg_result->value);
        }
        
        // Look up user-defined function
        ASTNode* func_node = comptime_context_lookup_func(ctx, func_ident->name);
        if (!func_node) {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "Undefined function in comptime context: %s", func_ident->name);
            return comptime_result_new(NULL, comptime_error_new(error_msg, call->pos), NULL);
        }
        
        // TODO: Implement user-defined function calls
        return comptime_result_new(NULL, comptime_error_new("User-defined function calls not yet implemented", call->pos), NULL);
    }
    
    return comptime_result_new(NULL, comptime_error_new("Complex function calls not supported in comptime evaluation", call->pos), NULL);
}

// Built-in intrinsic: @emit
ComptimeResult* comptime_intrinsic_emit(ComptimeContext* ctx, ComptimeValue* code) {
    if (!ctx || !code) {
        return comptime_result_new(NULL, comptime_error_new("Invalid context or code for @emit", (Position){0}), NULL);
    }
    
    if (code->type != COMPTIME_VALUE_STRING) {
        return comptime_result_new(NULL, comptime_error_new("@emit requires a string argument", (Position){0}), NULL);
    }
    
    // Add the code to the generated code buffer
    const char* code_str = code->string_value;
    size_t code_len = strlen(code_str);
    
    // Expand buffer if needed or initialize if NULL
    if (ctx->generated_code == NULL || ctx->generated_code_size + code_len + 1 > ctx->generated_code_capacity) {
        size_t new_capacity = ctx->generated_code_capacity == 0 ? 1024 : ctx->generated_code_capacity * 2;
        while (new_capacity < ctx->generated_code_size + code_len + 1) {
            new_capacity *= 2;
        }
        
        char* new_buffer = realloc(ctx->generated_code, new_capacity);
        if (!new_buffer) {
            return comptime_result_new(NULL, comptime_error_new("Out of memory in @emit", (Position){0}), NULL);
        }
        
        ctx->generated_code = new_buffer;
        ctx->generated_code_capacity = new_capacity;
        
        // Initialize buffer if it was NULL
        if (ctx->generated_code_size == 0) {
            ctx->generated_code[0] = '\0';
        }
    }
    
    // Append the code
    if (ctx->generated_code_size == 0) {
        strcpy(ctx->generated_code, code_str);
    } else {
        strcat(ctx->generated_code, code_str);
    }
    ctx->generated_code_size += code_len;
    
    return comptime_result_new(comptime_value_new(COMPTIME_VALUE_NULL), NULL, NULL);
}

// Built-in intrinsic: @typeof
ComptimeResult* comptime_intrinsic_typeof(ComptimeContext* ctx, ComptimeValue* value) {
    if (!ctx || !value) {
        return comptime_result_new(NULL, comptime_error_new("Invalid context or value for @typeof", (Position){0}), NULL);
    }
    
    const char* type_name;
    switch (value->type) {
        case COMPTIME_VALUE_INT:
            type_name = "int";
            break;
        case COMPTIME_VALUE_FLOAT:
            type_name = "float";
            break;
        case COMPTIME_VALUE_BOOL:
            type_name = "bool";
            break;
        case COMPTIME_VALUE_STRING:
            type_name = "string";
            break;
        case COMPTIME_VALUE_ARRAY:
            type_name = "array";
            break;
        case COMPTIME_VALUE_STRUCT:
            type_name = "struct";
            break;
        case COMPTIME_VALUE_FUNCTION:
            type_name = "function";
            break;
        case COMPTIME_VALUE_TYPE:
            type_name = "type";
            break;
        case COMPTIME_VALUE_NULL:
            type_name = "null";
            break;
        case COMPTIME_VALUE_UNDEFINED:
            type_name = "undefined";
            break;
        default:
            type_name = "unknown";
            break;
    }
    
    return comptime_result_new(comptime_value_from_string(type_name), NULL, NULL);
}

// Built-in intrinsic: @sizeof
ComptimeResult* comptime_intrinsic_sizeof(ComptimeContext* ctx, ComptimeValue* value) {
    if (!ctx || !value) {
        return comptime_result_new(NULL, comptime_error_new("Invalid context or value for @sizeof", (Position){0}), NULL);
    }
    
    int64_t size;
    switch (value->type) {
        case COMPTIME_VALUE_INT:
            size = 8; // Assuming 64-bit integers
            break;
        case COMPTIME_VALUE_FLOAT:
            size = 8; // Assuming 64-bit floats
            break;
        case COMPTIME_VALUE_BOOL:
            size = 1;
            break;
        case COMPTIME_VALUE_STRING:
            size = value->string_value ? (int64_t)strlen(value->string_value) : 0;
            break;
        case COMPTIME_VALUE_ARRAY:
            size = (int64_t)value->array_value.count;
            break;
        default:
            return comptime_result_new(NULL, comptime_error_new("Cannot get size of this type", (Position){0}), NULL);
    }
    
    return comptime_result_new(comptime_value_from_int(size), NULL, NULL);
}

// Evaluate a statement at compile time
ComptimeResult* comptime_eval_statement(ComptimeContext* ctx, ASTNode* stmt) {
    if (!ctx || !stmt) {
        return comptime_result_new(NULL, comptime_error_new("Invalid context or statement", (Position){0}), NULL);
    }
    
    switch (stmt->type) {
        case AST_EXPR_STMT: {
            ExprStmtNode* expr_stmt = (ExprStmtNode*)stmt;
            return comptime_eval_expression(ctx, expr_stmt->expr);
        }
        
        case AST_CONST_DECL: {
            ConstDeclNode* const_decl = (ConstDeclNode*)stmt;
            
            // For simplicity, handle single constant declaration
            if (const_decl->name_count != 1) {
                return comptime_result_new(NULL, comptime_error_new("Multiple constants not supported in comptime", stmt->pos), NULL);
            }
            
            // Evaluate the initializer expression
            ComptimeResult* init_result = comptime_eval_expression(ctx, const_decl->values);
            if (init_result->error) {
                return init_result;
            }
            
            // Bind the constant in the context
            if (!comptime_context_bind_var(ctx, const_decl->names[0], comptime_value_copy(init_result->value))) {
                comptime_result_free(init_result);
                return comptime_result_new(NULL, comptime_error_new("Failed to bind constant", stmt->pos), NULL);
            }
            
            return init_result;
        }
        
        case AST_VAR_DECL: {
            VarDeclNode* var_decl = (VarDeclNode*)stmt;
            
            // For simplicity, handle single variable declaration
            if (var_decl->name_count != 1) {
                return comptime_result_new(NULL, comptime_error_new("Multiple variables not supported in comptime", stmt->pos), NULL);
            }
            
            ComptimeValue* init_val = NULL;
            if (var_decl->values) {
                ComptimeResult* init_result = comptime_eval_expression(ctx, var_decl->values);
                if (init_result->error) {
                    return init_result;
                }
                init_val = init_result->value;
                init_result->value = NULL; // Transfer ownership
                comptime_result_free(init_result);
            } else {
                init_val = comptime_value_new(COMPTIME_VALUE_UNDEFINED);
            }
            
            // Bind the variable in the context
            if (!comptime_context_bind_var(ctx, var_decl->names[0], init_val)) {
                comptime_value_free(init_val);
                return comptime_result_new(NULL, comptime_error_new("Failed to bind variable", stmt->pos), NULL);
            }
            
            return comptime_result_new(comptime_value_copy(init_val), NULL, NULL);
        }
        
        default: {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "Unsupported statement type in comptime evaluation: %d", stmt->type);
            return comptime_result_new(NULL, comptime_error_new(error_msg, stmt->pos), NULL);
        }
    }
}

// Evaluate a block at compile time
ComptimeResult* comptime_eval_block(ComptimeContext* ctx, ASTNode* block) {
    if (!ctx || !block || block->type != AST_BLOCK_STMT) {
        return comptime_result_new(NULL, comptime_error_new("Invalid context or block", (Position){0}), NULL);
    }
    
    BlockStmtNode* block_node = (BlockStmtNode*)block;
    ComptimeResult* last_result = NULL;
    
    // Create a new context for the block scope
    ComptimeContext* block_ctx = comptime_context_new(ctx);
    if (!block_ctx) {
        return comptime_result_new(NULL, comptime_error_new("Failed to create block context", block->pos), NULL);
    }
    
    // Execute each statement in the block
    // For now, we'll handle the statements as a linked list or single statement
    // This is a simplification - in practice you'd iterate through the statement list
    if (block_node->statements) {
        last_result = comptime_eval_statement(block_ctx, block_node->statements);
        
        if (last_result->error) {
            comptime_context_free(block_ctx);
            return last_result;
        }
    }
    
    // Copy generated code from block context to parent
    if (block_ctx->generated_code) {
        // TODO: Merge generated code properly
    }
    
    comptime_context_free(block_ctx);
    
    if (!last_result) {
        last_result = comptime_result_new(comptime_value_new(COMPTIME_VALUE_NULL), NULL, NULL);
    }
    
    return last_result;
}

// Main entry point for compile-time execution
ComptimeResult* execute_comptime_block(ASTNode* comptime_block, ComptimeContext* global_ctx) {
    if (!comptime_block || comptime_block->type != AST_COMPTIME_BLOCK) {
        return comptime_result_new(NULL, comptime_error_new("Invalid comptime block", (Position){0}), NULL);
    }
    
    ComptimeBlockNode* block = (ComptimeBlockNode*)comptime_block;
    
    // Create execution context if not provided
    ComptimeContext* ctx = global_ctx;
    bool created_context = false;
    if (!ctx) {
        ctx = comptime_context_new(NULL);
        if (!ctx) {
            return comptime_result_new(NULL, comptime_error_new("Failed to create comptime context", comptime_block->pos), NULL);
        }
        created_context = true;
    }
    
    // Execute the block body
    ComptimeResult* result = comptime_eval_block(ctx, block->body);
    
    // Copy generated code from context to result
    if (ctx->generated_code && !result->generated_code) {
        result->generated_code = strdup(ctx->generated_code);
    }
    
    if (created_context) {
        comptime_context_free(ctx);
    }
    
    return result;
}

// Forward declarations for control flow
static ComptimeResult* comptime_eval_if_stmt(ComptimeContext* ctx, ASTNode* stmt);
static ComptimeResult* comptime_eval_for_stmt(ComptimeContext* ctx, ASTNode* stmt);
static ComptimeResult* comptime_eval_return_stmt(ComptimeContext* ctx, ASTNode* stmt);

// Exception for early returns
typedef struct {
    ComptimeValue* return_value;
    bool is_return;
} ComptimeControlFlow;

static ComptimeControlFlow comptime_control_flow_none(void) {
    return (ComptimeControlFlow){NULL, false};
}

static ComptimeControlFlow comptime_control_flow_return(ComptimeValue* value) {
    return (ComptimeControlFlow){value, true};
}

// Execute a user-defined function
ComptimeResult* comptime_call_user_function(ComptimeContext* ctx, ASTNode* func_node, 
                                           ComptimeValue** args, size_t arg_count) {
    if (!ctx || !func_node || func_node->type != AST_FUNC_DECL) {
        return comptime_result_new(NULL, comptime_error_new("Invalid function for comptime call", (Position){0}), NULL);
    }
    
    FuncDeclNode* func = (FuncDeclNode*)func_node;
    
    // Check recursion depth
    if (ctx->current_recursion_depth >= ctx->max_recursion_depth) {
        return comptime_result_new(NULL, comptime_error_new("Maximum recursion depth exceeded in comptime function", func_node->pos), NULL);
    }
    
    // Create new context for function scope
    ComptimeContext* func_ctx = comptime_context_new(ctx);
    if (!func_ctx) {
        return comptime_result_new(NULL, comptime_error_new("Failed to create function context", func_node->pos), NULL);
    }
    
    func_ctx->current_recursion_depth = ctx->current_recursion_depth + 1;
    
    // Bind function parameters to arguments
    // Note: This is a simplified implementation. In a real implementation,
    // we'd need to properly parse the function signature and match parameters
    for (size_t i = 0; i < arg_count && i < 10; i++) { // Assuming max 10 params for now
        char param_name[32];
        snprintf(param_name, sizeof(param_name), "param_%zu", i);
        
        if (!comptime_context_bind_var(func_ctx, param_name, comptime_value_copy(args[i]))) {
            comptime_context_free(func_ctx);
            return comptime_result_new(NULL, comptime_error_new("Failed to bind function parameter", func_node->pos), NULL);
        }
    }
    
    // Execute function body
    ComptimeResult* result = comptime_eval_block(func_ctx, func->body);
    
    // Copy generated code from function context
    if (func_ctx->generated_code && result && !result->generated_code) {
        result->generated_code = strdup(func_ctx->generated_code);
    }
    
    comptime_context_free(func_ctx);
    return result;
}

// Enhanced function call evaluation with user-defined functions
ComptimeResult* comptime_eval_function_call_enhanced(ComptimeContext* ctx, ASTNode* call) {
    if (!ctx || !call || call->type != AST_CALL_EXPR) {
        return comptime_result_new(NULL, comptime_error_new("Invalid function call", (Position){0}), NULL);
    }
    
    CallExprNode* call_node = (CallExprNode*)call;
    
    // Check if it's a built-in intrinsic function
    if (call_node->function && call_node->function->type == AST_IDENTIFIER) {
        IdentifierNode* func_ident = (IdentifierNode*)call_node->function;
        
        // Handle built-in intrinsics first
        if (func_ident->name[0] == '@') {
            // This is handled in the original function
            return comptime_eval_function_call(ctx, call);
        }
        
        // Look up user-defined function
        ASTNode* func_node = comptime_context_lookup_func(ctx, func_ident->name);
        if (func_node) {
            // Evaluate arguments
            ComptimeValue* args[16]; // Max 16 arguments
            size_t actual_arg_count = 0;
            
            // For now, assume no arguments since we don't have proper argument parsing
            // In a real implementation, we'd parse the arguments from call_node
            
            return comptime_call_user_function(ctx, func_node, args, actual_arg_count);
        }
        
        // Function not found
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Undefined function in comptime context: %s", func_ident->name);
        return comptime_result_new(NULL, comptime_error_new(error_msg, call->pos), NULL);
    }
    
    return comptime_result_new(NULL, comptime_error_new("Complex function calls not supported in comptime evaluation", call->pos), NULL);
}

// Evaluate an if statement
static ComptimeResult* comptime_eval_if_stmt(ComptimeContext* ctx, ASTNode* stmt) {
    if (!ctx || !stmt || stmt->type != AST_IF_STMT) {
        return comptime_result_new(NULL, comptime_error_new("Invalid if statement", (Position){0}), NULL);
    }
    
    IfStmtNode* if_stmt = (IfStmtNode*)stmt;
    
    // Evaluate condition
    ComptimeResult* cond_result = comptime_eval_expression(ctx, if_stmt->condition);
    if (cond_result->error) {
        return cond_result;
    }
    
    bool condition_true = comptime_value_is_truthy(cond_result->value);
    comptime_result_free(cond_result);
    
    // Execute appropriate branch
    if (condition_true) {
        return comptime_eval_statement(ctx, if_stmt->then_stmt);
    } else if (if_stmt->else_stmt) {
        return comptime_eval_statement(ctx, if_stmt->else_stmt);
    } else {
        // No else branch, return null
        return comptime_result_new(comptime_value_new(COMPTIME_VALUE_NULL), NULL, NULL);
    }
}

// Evaluate a for statement (simplified)
static ComptimeResult* comptime_eval_for_stmt(ComptimeContext* ctx, ASTNode* stmt) {
    if (!ctx || !stmt || stmt->type != AST_FOR_STMT) {
        return comptime_result_new(NULL, comptime_error_new("Invalid for statement", (Position){0}), NULL);
    }
    
    ForStmtNode* for_stmt = (ForStmtNode*)stmt;
    
    // Create new scope for the loop
    ComptimeContext* loop_ctx = comptime_context_new(ctx);
    if (!loop_ctx) {
        return comptime_result_new(NULL, comptime_error_new("Failed to create loop context", stmt->pos), NULL);
    }
    
    ComptimeResult* last_result = NULL;
    
    // Execute initialization if present
    if (for_stmt->init) {
        last_result = comptime_eval_statement(loop_ctx, for_stmt->init);
        if (last_result->error) {
            comptime_context_free(loop_ctx);
            return last_result;
        }
        comptime_result_free(last_result);
    }
    
    // Loop execution with iteration limit
    size_t max_iterations = 1000; // Prevent infinite loops
    size_t iteration_count = 0;
    
    while (iteration_count < max_iterations) {
        // Check condition if present
        if (for_stmt->condition) {
            ComptimeResult* cond_result = comptime_eval_expression(loop_ctx, for_stmt->condition);
            if (cond_result->error) {
                comptime_context_free(loop_ctx);
                return cond_result;
            }
            
            bool should_continue = comptime_value_is_truthy(cond_result->value);
            comptime_result_free(cond_result);
            
            if (!should_continue) {
                break;
            }
        }
        
        // Execute body
        comptime_result_free(last_result);
        last_result = comptime_eval_statement(loop_ctx, for_stmt->body);
        if (last_result->error) {
            comptime_context_free(loop_ctx);
            return last_result;
        }
        
        // Execute post statement if present
        if (for_stmt->post) {
            ComptimeResult* post_result = comptime_eval_statement(loop_ctx, for_stmt->post);
            if (post_result->error) {
                comptime_result_free(last_result);
                comptime_context_free(loop_ctx);
                return post_result;
            }
            comptime_result_free(post_result);
        }
        
        iteration_count++;
    }
    
    if (iteration_count >= max_iterations) {
        comptime_result_free(last_result);
        comptime_context_free(loop_ctx);
        return comptime_result_new(NULL, comptime_error_new("Loop iteration limit exceeded in comptime evaluation", stmt->pos), NULL);
    }
    
    // Copy generated code from loop context
    if (loop_ctx->generated_code && last_result && !last_result->generated_code) {
        last_result->generated_code = strdup(loop_ctx->generated_code);
    }
    
    comptime_context_free(loop_ctx);
    
    if (!last_result) {
        last_result = comptime_result_new(comptime_value_new(COMPTIME_VALUE_NULL), NULL, NULL);
    }
    
    return last_result;
}

// Evaluate a return statement
static ComptimeResult* comptime_eval_return_stmt(ComptimeContext* ctx, ASTNode* stmt) {
    if (!ctx || !stmt || stmt->type != AST_RETURN_STMT) {
        return comptime_result_new(NULL, comptime_error_new("Invalid return statement", (Position){0}), NULL);
    }
    
    ReturnStmtNode* return_stmt = (ReturnStmtNode*)stmt;
    
    if (return_stmt->values) {
        // Evaluate return expression
        ComptimeResult* expr_result = comptime_eval_expression(ctx, return_stmt->values);
        if (expr_result->error) {
            return expr_result;
        }
        
        // Mark this result as a return value (we'd need to modify the result structure for this)
        return expr_result;
    } else {
        // Return without value
        return comptime_result_new(comptime_value_new(COMPTIME_VALUE_NULL), NULL, NULL);
    }
}

// Enhanced statement evaluation with control flow
ComptimeResult* comptime_eval_statement_enhanced(ComptimeContext* ctx, ASTNode* stmt) {
    if (!ctx || !stmt) {
        return comptime_result_new(NULL, comptime_error_new("Invalid context or statement", (Position){0}), NULL);
    }
    
    switch (stmt->type) {
        case AST_IF_STMT:
            return comptime_eval_if_stmt(ctx, stmt);
            
        case AST_FOR_STMT:
            return comptime_eval_for_stmt(ctx, stmt);
            
        case AST_RETURN_STMT:
            return comptime_eval_return_stmt(ctx, stmt);
            
        case AST_FUNC_DECL: {
            // Register function in context
            FuncDeclNode* func_decl = (FuncDeclNode*)stmt;
            
            if (!comptime_context_bind_func(ctx, func_decl->name, stmt)) {
                return comptime_result_new(NULL, comptime_error_new("Failed to bind function", stmt->pos), NULL);
            }
            
            return comptime_result_new(comptime_value_new(COMPTIME_VALUE_NULL), NULL, NULL);
        }
        
        default:
            // Fall back to original implementation
            return comptime_eval_statement(ctx, stmt);
    }
}

// Constant folding optimization
ComptimeValue* comptime_constant_fold(ComptimeContext* ctx, ASTNode* expr) {
    if (!ctx || !expr) return NULL;
    
    // Only fold simple expressions that don't have side effects
    switch (expr->type) {
        case AST_LITERAL:
            return comptime_value_from_literal(expr);
            
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)expr;
            
            // Try to fold both operands
            ComptimeValue* left = comptime_constant_fold(ctx, binary->left);
            ComptimeValue* right = comptime_constant_fold(ctx, binary->right);
            
            if (left && right) {
                // Both operands are constants, perform the operation
                ComptimeResult* result = comptime_eval_expression(ctx, expr);
                if (result && !result->error) {
                    ComptimeValue* folded = result->value;
                    result->value = NULL; // Transfer ownership
                    comptime_result_free(result);
                    comptime_value_free(left);
                    comptime_value_free(right);
                    return folded;
                }
                comptime_result_free(result);
            }
            
            comptime_value_free(left);
            comptime_value_free(right);
            return NULL;
        }
        
        case AST_UNARY_EXPR: {
            UnaryExprNode* unary = (UnaryExprNode*)expr;
            
            ComptimeValue* operand = comptime_constant_fold(ctx, unary->operand);
            if (operand) {
                ComptimeResult* result = comptime_eval_expression(ctx, expr);
                if (result && !result->error) {
                    ComptimeValue* folded = result->value;
                    result->value = NULL;
                    comptime_result_free(result);
                    comptime_value_free(operand);
                    return folded;
                }
                comptime_result_free(result);
            }
            
            comptime_value_free(operand);
            return NULL;
        }
        
        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)expr;
            ComptimeValue* value = comptime_context_lookup_var(ctx, ident->name);
            
            // Only fold if it's a constant (we'd need additional metadata to track this)
            if (value) {
                return comptime_value_copy(value);
            }
            return NULL;
        }
        
        default:
            return NULL; // Cannot fold
    }
}

// Check if an expression can be evaluated at compile time
bool comptime_is_evaluable(ComptimeContext* ctx, ASTNode* expr) {
    if (!ctx || !expr) return false;
    
    switch (expr->type) {
        case AST_LITERAL:
            return true;
            
        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)expr;
            ComptimeValue* value = comptime_context_lookup_var(ctx, ident->name);
            return value != NULL;
        }
        
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)expr;
            return comptime_is_evaluable(ctx, binary->left) && 
                   comptime_is_evaluable(ctx, binary->right);
        }
        
        case AST_UNARY_EXPR: {
            UnaryExprNode* unary = (UnaryExprNode*)expr;
            return comptime_is_evaluable(ctx, unary->operand);
        }
        
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            
            // Check if it's a comptime intrinsic
            if (call->function && call->function->type == AST_IDENTIFIER) {
                IdentifierNode* func_ident = (IdentifierNode*)call->function;
                if (func_ident->name[0] == '@') {
                    return true; // Intrinsics are always evaluable
                }
                
                // Check if it's a user-defined function
                ASTNode* func_node = comptime_context_lookup_func(ctx, func_ident->name);
                return func_node != NULL;
            }
            
            return false;
        }
        
        default:
            return false;
    }
}

// Advanced code generation features

// Template-based code generation
ComptimeResult* comptime_intrinsic_generate_template(ComptimeContext* ctx, ComptimeValue* template, ComptimeValue** args, size_t arg_count) {
    if (!ctx || !template || template->type != COMPTIME_VALUE_STRING) {
        return comptime_result_new(NULL, comptime_error_new("@generate requires a string template", (Position){0}), NULL);
    }
    
    const char* template_str = template->string_value;
    size_t template_len = strlen(template_str);
    
    // Allocate buffer for generated code (estimate size)
    size_t buffer_size = template_len * 2;
    char* generated = malloc(buffer_size);
    if (!generated) {
        return comptime_result_new(NULL, comptime_error_new("Out of memory in @generate", (Position){0}), NULL);
    }
    
    size_t output_pos = 0;
    size_t template_pos = 0;
    
    // Simple template processing: replace {{i}} with argument i
    while (template_pos < template_len) {
        if (template_pos + 1 < template_len && 
            template_str[template_pos] == '{' && 
            template_str[template_pos + 1] == '{') {
            
            // Find the closing }}
            size_t closing_pos = template_pos + 2;
            while (closing_pos + 1 < template_len && 
                   !(template_str[closing_pos] == '}' && template_str[closing_pos + 1] == '}')) {
                closing_pos++;
            }
            
            if (closing_pos + 1 < template_len) {
                // Extract the placeholder content
                size_t placeholder_len = closing_pos - (template_pos + 2);
                char placeholder[32];
                if (placeholder_len < sizeof(placeholder)) {
                    strncpy(placeholder, template_str + template_pos + 2, placeholder_len);
                    placeholder[placeholder_len] = '\0';
                    
                    // Check if it's a simple integer index
                    if (placeholder[0] >= '0' && placeholder[0] <= '9') {
                        int arg_index = atoi(placeholder);
                        if (arg_index >= 0 && (size_t)arg_index < arg_count) {
                            // Replace with the argument value
                            char* arg_str = comptime_value_to_string(args[arg_index]);
                            size_t arg_len = strlen(arg_str);
                            
                            // Expand buffer if needed
                            if (output_pos + arg_len >= buffer_size) {
                                buffer_size = (output_pos + arg_len + 1) * 2;
                                char* new_buffer = realloc(generated, buffer_size);
                                if (!new_buffer) {
                                    free(generated);
                                    free(arg_str);
                                    return comptime_result_new(NULL, comptime_error_new("Out of memory in @generate", (Position){0}), NULL);
                                }
                                generated = new_buffer;
                            }
                            
                            strcpy(generated + output_pos, arg_str);
                            output_pos += arg_len;
                            free(arg_str);
                        }
                    }
                }
                
                template_pos = closing_pos + 2;
            } else {
                // Malformed template, just copy the character
                generated[output_pos++] = template_str[template_pos++];
            }
        } else {
            // Regular character, copy it
            if (output_pos >= buffer_size - 1) {
                buffer_size *= 2;
                char* new_buffer = realloc(generated, buffer_size);
                if (!new_buffer) {
                    free(generated);
                    return comptime_result_new(NULL, comptime_error_new("Out of memory in @generate", (Position){0}), NULL);
                }
                generated = new_buffer;
            }
            generated[output_pos++] = template_str[template_pos++];
        }
    }
    
    generated[output_pos] = '\0';
    
    // Add the generated code to the context
    ComptimeValue* code_value = comptime_value_from_string(generated);
    ComptimeResult* emit_result = comptime_intrinsic_emit(ctx, code_value);
    comptime_value_free(code_value);
    free(generated);
    
    return emit_result;
}

// Loop-based code generation
ComptimeResult* comptime_intrinsic_generate_loop(ComptimeContext* ctx, ComptimeValue* count, ComptimeValue* template) {
    if (!ctx || !count || count->type != COMPTIME_VALUE_INT || !template || template->type != COMPTIME_VALUE_STRING) {
        return comptime_result_new(NULL, comptime_error_new("@generate_loop requires an integer count and string template", (Position){0}), NULL);
    }
    
    int64_t loop_count = count->int_value;
    if (loop_count < 0 || loop_count > 1000) { // Reasonable limit
        return comptime_result_new(NULL, comptime_error_new("@generate_loop count out of range", (Position){0}), NULL);
    }
    
    ComptimeResult* last_result = NULL;
    
    for (int64_t i = 0; i < loop_count; i++) {
        // Create argument for current iteration
        ComptimeValue* iter_value = comptime_value_from_int(i);
        ComptimeValue* args[] = {iter_value};
        
        comptime_result_free(last_result);
        last_result = comptime_intrinsic_generate_template(ctx, template, args, 1);
        
        comptime_value_free(iter_value);
        
        if (last_result->error) {
            return last_result;
        }
    }
    
    if (!last_result) {
        last_result = comptime_result_new(comptime_value_new(COMPTIME_VALUE_NULL), NULL, NULL);
    }
    
    return last_result;
}

// Reflection-based code generation helpers
ComptimeResult* comptime_intrinsic_struct_fields(ComptimeContext* ctx, ComptimeValue* struct_type) {
    if (!ctx || !struct_type) {
        return comptime_result_new(NULL, comptime_error_new("@struct_fields requires a struct type", (Position){0}), NULL);
    }
    
    // This would integrate with the type system to get actual struct fields
    // For now, return a mock array of field names
    ComptimeValue* fields_array = comptime_value_new(COMPTIME_VALUE_ARRAY);
    if (!fields_array) {
        return comptime_result_new(NULL, comptime_error_new("Out of memory", (Position){0}), NULL);
    }
    
    // Mock fields for demonstration
    const char* mock_fields[] = {"id", "name", "value"};
    size_t field_count = sizeof(mock_fields) / sizeof(mock_fields[0]);
    
    fields_array->array_value.capacity = field_count;
    fields_array->array_value.count = field_count;
    fields_array->array_value.elements = malloc(sizeof(ComptimeValue*) * field_count);
    
    if (!fields_array->array_value.elements) {
        comptime_value_free(fields_array);
        return comptime_result_new(NULL, comptime_error_new("Out of memory", (Position){0}), NULL);
    }
    
    for (size_t i = 0; i < field_count; i++) {
        fields_array->array_value.elements[i] = comptime_value_from_string(mock_fields[i]);
    }
    
    return comptime_result_new(fields_array, NULL, NULL);
}

// String formatting for code generation
ComptimeResult* comptime_intrinsic_format(ComptimeContext* ctx, ComptimeValue* format_str, ComptimeValue** args, size_t arg_count) {
    if (!ctx || !format_str || format_str->type != COMPTIME_VALUE_STRING) {
        return comptime_result_new(NULL, comptime_error_new("@format requires a format string", (Position){0}), NULL);
    }
    
    const char* fmt = format_str->string_value;
    size_t result_size = strlen(fmt) + 256; // Estimate
    char* result = malloc(result_size);
    if (!result) {
        return comptime_result_new(NULL, comptime_error_new("Out of memory in @format", (Position){0}), NULL);
    }
    
    // Simple sprintf-style formatting
    // This is a simplified implementation - a real one would be more robust
    if (arg_count == 0) {
        strcpy(result, fmt);
    } else if (arg_count == 1) {
        if (args[0]->type == COMPTIME_VALUE_INT) {
            snprintf(result, result_size, fmt, args[0]->int_value);
        } else if (args[0]->type == COMPTIME_VALUE_STRING) {
            snprintf(result, result_size, fmt, args[0]->string_value);
        } else {
            char* arg_str = comptime_value_to_string(args[0]);
            snprintf(result, result_size, fmt, arg_str);
            free(arg_str);
        }
    } else {
        // Multiple arguments - simplified handling
        strcpy(result, fmt);
    }
    
    ComptimeValue* result_value = comptime_value_from_string(result);
    free(result);
    
    return comptime_result_new(result_value, NULL, NULL);
}

// Enhanced function call handling with more intrinsics
ComptimeResult* comptime_eval_function_call_advanced(ComptimeContext* ctx, ASTNode* call) {
    if (!ctx || !call || call->type != AST_CALL_EXPR) {
        return comptime_result_new(NULL, comptime_error_new("Invalid function call", (Position){0}), NULL);
    }
    
    CallExprNode* call_node = (CallExprNode*)call;
    
    if (call_node->function && call_node->function->type == AST_IDENTIFIER) {
        IdentifierNode* func_ident = (IdentifierNode*)call_node->function;
        
        // Handle extended intrinsics
        if (strcmp(func_ident->name, "@generate") == 0) {
            // @generate(template, args...)
            // For simplicity, assume 2 arguments: template and one replacement value
            if (call_node->args && call_node->args->type == AST_IDENTIFIER) {
                // Simplified: just emit the template directly
                ComptimeValue* template = comptime_value_from_string("func generated_{{0}}() { return {{0}}; }\n");
                ComptimeValue* index = comptime_value_from_int(42);
                ComptimeValue* args[] = {index};
                ComptimeResult* result = comptime_intrinsic_generate_template(ctx, template, args, 1);
                comptime_value_free(template);
                comptime_value_free(index);
                return result;
            }
        }
        
        if (strcmp(func_ident->name, "@generate_loop") == 0) {
            ComptimeValue* count = comptime_value_from_int(3);
            ComptimeValue* template = comptime_value_from_string("func process_{{0}}() { return {{0}}; }\n");
            ComptimeResult* result = comptime_intrinsic_generate_loop(ctx, count, template);
            comptime_value_free(count);
            comptime_value_free(template);
            return result;
        }
        
        if (strcmp(func_ident->name, "@struct_fields") == 0) {
            ComptimeValue* mock_struct = comptime_value_new(COMPTIME_VALUE_STRUCT);
            ComptimeResult* result = comptime_intrinsic_struct_fields(ctx, mock_struct);
            comptime_value_free(mock_struct);
            return result;
        }
        
        if (strcmp(func_ident->name, "@format") == 0) {
            ComptimeValue* format_str = comptime_value_from_string("Value: %d");
            ComptimeValue* value = comptime_value_from_int(123);
            ComptimeValue* args[] = {value};
            ComptimeResult* result = comptime_intrinsic_format(ctx, format_str, args, 1);
            comptime_value_free(format_str);
            comptime_value_free(value);
            return result;
        }
    }
    
    // Fall back to the original implementation
    return comptime_eval_function_call_enhanced(ctx, call);
}

// Code generation pipeline integration
CodeGenPipeline* comptime_codegen_pipeline_new(void) {
    CodeGenPipeline* pipeline = malloc(sizeof(CodeGenPipeline));
    if (!pipeline) return NULL;
    
    pipeline->generated_functions = NULL;
    pipeline->function_count = 0;
    pipeline->function_capacity = 0;
    
    pipeline->generated_types = NULL;
    pipeline->type_count = 0;
    pipeline->type_capacity = 0;
    
    pipeline->generated_constants = NULL;
    pipeline->constant_count = 0;
    pipeline->constant_capacity = 0;
    
    return pipeline;
}

void comptime_codegen_pipeline_free(CodeGenPipeline* pipeline) {
    if (!pipeline) return;
    
    for (size_t i = 0; i < pipeline->function_count; i++) {
        free(pipeline->generated_functions[i]);
    }
    free(pipeline->generated_functions);
    
    for (size_t i = 0; i < pipeline->type_count; i++) {
        free(pipeline->generated_types[i]);
    }
    free(pipeline->generated_types);
    
    for (size_t i = 0; i < pipeline->constant_count; i++) {
        free(pipeline->generated_constants[i]);
    }
    free(pipeline->generated_constants);
    
    free(pipeline);
}

bool comptime_codegen_pipeline_add_function(CodeGenPipeline* pipeline, const char* function_code) {
    if (!pipeline || !function_code) return false;
    
    if (pipeline->function_count >= pipeline->function_capacity) {
        size_t new_capacity = pipeline->function_capacity == 0 ? 8 : pipeline->function_capacity * 2;
        char** new_functions = realloc(pipeline->generated_functions, sizeof(char*) * new_capacity);
        if (!new_functions) return false;
        
        pipeline->generated_functions = new_functions;
        pipeline->function_capacity = new_capacity;
    }
    
    pipeline->generated_functions[pipeline->function_count] = strdup(function_code);
    pipeline->function_count++;
    
    return true;
}

char* comptime_codegen_pipeline_finalize(CodeGenPipeline* pipeline) {
    if (!pipeline) return NULL;
    
    size_t total_size = 0;
    
    // Calculate total size needed
    for (size_t i = 0; i < pipeline->function_count; i++) {
        total_size += strlen(pipeline->generated_functions[i]) + 1;
    }
    for (size_t i = 0; i < pipeline->type_count; i++) {
        total_size += strlen(pipeline->generated_types[i]) + 1;
    }
    for (size_t i = 0; i < pipeline->constant_count; i++) {
        total_size += strlen(pipeline->generated_constants[i]) + 1;
    }
    
    if (total_size == 0) return strdup("");
    
    char* result = malloc(total_size + 1);
    if (!result) return NULL;
    
    result[0] = '\0';
    
    // Concatenate all generated code
    for (size_t i = 0; i < pipeline->constant_count; i++) {
        strcat(result, pipeline->generated_constants[i]);
        strcat(result, "\n");
    }
    
    for (size_t i = 0; i < pipeline->type_count; i++) {
        strcat(result, pipeline->generated_types[i]);
        strcat(result, "\n");
    }
    
    for (size_t i = 0; i < pipeline->function_count; i++) {
        strcat(result, pipeline->generated_functions[i]);
        strcat(result, "\n");
    }
    
    return result;
}

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
            return ast_literal_new(TOKEN_INT, buffer, pos);
        }
            
        case COMPTIME_VALUE_FLOAT: {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%f", value->float_value);
            return ast_literal_new(TOKEN_FLOAT, buffer, pos);
        }
            
        case COMPTIME_VALUE_BOOL:
            return ast_literal_new(TOKEN_IDENT,
                                 value->bool_value ? "true" : "false", pos);
            
        case COMPTIME_VALUE_STRING:
            return ast_literal_new(TOKEN_STRING,
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
