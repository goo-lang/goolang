#include "comptime.h"
#include "types.h"
#include "ast.h"
#include "token.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>


// Comptime intrinsics (@emit, @typeof, @sizeof, @generate, @for,
// @fields, @format) and the comptime codegen pipeline. Split from
// comptime.c (refactor, no behavior change).
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
// The one-argument @format path feeds a user-supplied format string straight to
// snprintf. Because that string comes from the program being compiled, an
// unvalidated one is a compile-time format-string vulnerability (CWE-134): %n is
// an arbitrary-write primitive, extra specifiers read past the single vararg, and
// a type-mismatched specifier (e.g. %s against an integer) dereferences the
// argument as the wrong type. Validate that the format carries exactly one
// conversion, that it is not %n, and that its type class matches the argument.
// %% is a literal, not a conversion. (The zero- and multi-argument paths copy the
// format verbatim and never reach snprintf, so they need no check.)
typedef enum { FMT_ARG_INT, FMT_ARG_STR } FmtArgClass;

static bool comptime_format_one_arg_safe(const char* fmt, FmtArgClass cls) {
    size_t conversions = 0;
    for (size_t i = 0; fmt[i]; i++) {
        if (fmt[i] != '%') {
            continue;
        }
        i++;
        if (fmt[i] == '%') { // literal %%
            continue;
        }
        // Skip flags, width, precision, and length modifiers to the conversion char.
        while (fmt[i] && strchr("-+ #0'123456789.hljztLqI*", fmt[i])) {
            i++;
        }
        char c = fmt[i];
        if (c == '\0' || c == 'n') { // truncated specifier, or the %n write primitive
            return false;
        }
        if (++conversions > 1) { // more specifiers than the single argument
            return false;
        }
        bool matches = (cls == FMT_ARG_INT) ? (strchr("diouxXc", c) != NULL)
                                            : (c == 's');
        if (!matches) {
            return false;
        }
    }
    return true;
}

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
        FmtArgClass cls = (args[0]->type == COMPTIME_VALUE_INT) ? FMT_ARG_INT : FMT_ARG_STR;
        if (!comptime_format_one_arg_safe(fmt, cls)) {
            free(result);
            return comptime_result_new(NULL, comptime_error_new(
                "@format: unsafe or unsupported format string (expected a single %-specifier matching the argument type; %n and extra specifiers are rejected)",
                (Position){0}), NULL);
        }
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
    CodeGenPipeline* pipeline = xmalloc(sizeof(CodeGenPipeline));
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
