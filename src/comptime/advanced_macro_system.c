#include "advanced_macro_system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

// =============================================================================
// Registry Management
// =============================================================================

MacroRegistry* create_macro_registry(void) {
    MacroRegistry* registry = xmalloc(sizeof(MacroRegistry));
    if (!registry) return NULL;
    
    registry->macros = NULL;
    registry->macro_count = 0;
    registry->builtin_macros = NULL;
    registry->enable_hygiene = true;
    registry->debug_expansions = false;
    registry->max_expansion_depth = 64;
    
    // Register built-in macros
    register_builtin_macros(registry);
    
    return registry;
}

void destroy_macro_registry(MacroRegistry* registry) {
    if (!registry) return;
    
    // Destroy all macros
    MacroTemplate* current = registry->macros;
    while (current) {
        MacroTemplate* next = current->next;
        destroy_macro_template(current);
        current = next;
    }
    
    // Destroy built-in macros
    current = registry->builtin_macros;
    while (current) {
        MacroTemplate* next = current->next;
        destroy_macro_template(current);
        current = next;
    }
    
    free(registry);
}

// =============================================================================
// Macro Template Management
// =============================================================================

MacroTemplate* create_macro_template(const char* name, MacroType type) {
    if (!name) return NULL;
    
    MacroTemplate* macro = xmalloc(sizeof(MacroTemplate));
    if (!macro) return NULL;
    
    macro->name = strdup(name);
    macro->type = type;
    macro->hygiene = HYGIENE_LEXICAL; // Default hygiene level
    macro->parameters = NULL;
    macro->param_count = 0;
    macro->body = NULL;
    macro->code_template = NULL;
    macro->is_recursive = false;
    macro->max_recursion = 10;
    macro->generate_debug_info = true;
    macro->evaluator = NULL;
    macro->next = NULL;
    
    return macro;
}

void destroy_macro_template(MacroTemplate* macro) {
    if (!macro) return;
    
    if (macro->name) free(macro->name);
    if (macro->code_template) free(macro->code_template);
    
    // Clean up parameters
    for (size_t i = 0; i < macro->param_count; i++) {
        MacroParameter* param = &macro->parameters[i];
        if (param->name) free(param->name);
        if (param->constraint) free(param->constraint);
        // Note: default_value should be managed by comptime system
    }
    if (macro->parameters) free(macro->parameters);
    
    free(macro);
}

bool register_macro(MacroRegistry* registry, MacroTemplate* macro) {
    if (!registry || !macro) return false;
    
    // Check for name conflicts
    if (find_macro(registry, macro->name)) {
        return false; // Macro already exists
    }
    
    // Add to linked list
    macro->next = registry->macros;
    registry->macros = macro;
    registry->macro_count++;
    
    return true;
}

MacroTemplate* find_macro(MacroRegistry* registry, const char* name) {
    if (!registry || !name) return NULL;
    
    // Search user-defined macros first
    MacroTemplate* current = registry->macros;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    
    // Search built-in macros
    current = registry->builtin_macros;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

// =============================================================================
// Parameter Management
// =============================================================================

bool add_macro_parameter(MacroTemplate* macro, const char* name, MacroParamType type) {
    if (!macro || !name) return false;
    
    // Resize parameter array
    MacroParameter* new_params = realloc(macro->parameters, 
                                        (macro->param_count + 1) * sizeof(MacroParameter));
    if (!new_params) return false;
    
    macro->parameters = new_params;
    
    // Initialize new parameter
    MacroParameter* param = &macro->parameters[macro->param_count];
    param->name = strdup(name);
    param->type = type;
    param->is_optional = false;
    param->default_value = NULL;
    param->constraint = NULL;
    
    macro->param_count++;
    return true;
}

bool set_parameter_constraint(MacroTemplate* macro, const char* param_name, const char* constraint) {
    if (!macro || !param_name || !constraint) return false;
    
    for (size_t i = 0; i < macro->param_count; i++) {
        MacroParameter* param = &macro->parameters[i];
        if (strcmp(param->name, param_name) == 0) {
            if (param->constraint) free(param->constraint);
            param->constraint = strdup(constraint);
            return true;
        }
    }
    
    return false; // Parameter not found
}

bool set_parameter_default(MacroTemplate* macro, const char* param_name, ComptimeValue* default_val) {
    if (!macro || !param_name) return false;
    
    for (size_t i = 0; i < macro->param_count; i++) {
        MacroParameter* param = &macro->parameters[i];
        if (strcmp(param->name, param_name) == 0) {
            param->default_value = default_val;
            param->is_optional = true;
            return true;
        }
    }
    
    return false; // Parameter not found
}

// =============================================================================
// Context Management
// =============================================================================

MacroContext* create_macro_context(MacroTemplate* macro, ComptimeValue** args, size_t arg_count) {
    if (!macro) return NULL;
    
    MacroContext* context = xmalloc(sizeof(MacroContext));
    if (!context) return NULL;
    
    context->macro = macro;
    context->arguments = args;
    context->arg_count = arg_count;
    context->call_site = NULL;
    context->source_file = NULL;
    context->line = 0;
    context->column = 0;
    context->hygiene_scope = NULL;
    context->scope_depth = 0;
    context->recursion_depth = 0;
    context->parent = NULL;
    context->error_message = NULL;
    context->has_error = false;
    
    return context;
}

void destroy_macro_context(MacroContext* context) {
    if (!context) return;
    
    if (context->source_file) free(context->source_file);
    if (context->error_message) free(context->error_message);
    
    // Clean up hygiene scope
    if (context->hygiene_scope) {
        for (size_t i = 0; i < context->scope_depth; i++) {
            if (context->hygiene_scope[i]) {
                free(context->hygiene_scope[i]);
            }
        }
        free(context->hygiene_scope);
    }
    
    free(context);
}

// =============================================================================
// Macro Expansion
// =============================================================================

MacroExpansion* expand_macro(MacroRegistry* registry, const char* macro_name, 
                           ComptimeValue** args, size_t arg_count, ASTNode* call_site) {
    if (!registry || !macro_name) return NULL;
    
    MacroExpansion* expansion = xmalloc(sizeof(MacroExpansion));
    if (!expansion) return NULL;
    
    expansion->expanded_ast = NULL;
    expansion->expanded_code = NULL;
    expansion->success = false;
    expansion->error_message = NULL;
    expansion->expansion_trace = NULL;
    expansion->context = NULL;
    
    // Find the macro
    MacroTemplate* macro = find_macro(registry, macro_name);
    if (!macro) {
        expansion->error_message = strdup("Macro not found");
        return expansion;
    }
    
    // Validate arguments
    if (!validate_macro_arguments(macro, args, arg_count)) {
        expansion->error_message = strdup("Invalid macro arguments");
        return expansion;
    }
    
    // Create expansion context
    MacroContext* context = create_macro_context(macro, args, arg_count);
    if (!context) {
        expansion->error_message = strdup("Failed to create macro context");
        return expansion;
    }
    
    context->call_site = call_site;
    expansion->context = context;
    
    // Perform expansion based on macro type
    switch (macro->type) {
        case MACRO_FUNCTION:
        case MACRO_PROCEDURAL:
            if (macro->evaluator) {
                ComptimeValue* result = macro->evaluator(context, args);
                if (result && result->type == COMPTIME_VALUE_STRING) {
                    expansion->expanded_code = strdup(result->string_value);
                    expansion->success = true;
                }
            } else if (macro->code_template) {
                expansion->expanded_code = process_template(macro->code_template, context);
                expansion->success = (expansion->expanded_code != NULL);
            }
            break;
            
        case MACRO_COMPILE_TIME:
            if (macro->evaluator) {
                ComptimeValue* result = evaluate_macro_at_compile_time(context);
                if (result) {
                    expansion->success = true;
                }
            }
            break;
            
        default:
            expansion->error_message = strdup("Unsupported macro type");
            break;
    }
    
    if (context->has_error) {
        expansion->success = false;
        expansion->error_message = strdup(context->error_message ? context->error_message : "Unknown error");
    }
    
    return expansion;
}

MacroExpansion* expand_macro_ast(MacroRegistry* registry, ASTNode* macro_call) {
    // This would parse the macro call AST and extract arguments
    // For now, return a simple expansion
    return expand_macro(registry, "example_macro", NULL, 0, macro_call);
}

// =============================================================================
// Template Processing
// =============================================================================

char* process_template(const char* template_str, MacroContext* context) {
    if (!template_str || !context) return NULL;
    
    // Simple template substitution for demonstration
    // In a real implementation, this would be much more sophisticated
    size_t result_len = strlen(template_str) + 1024; // Extra space for substitutions
    char* result = malloc(result_len);
    if (!result) return NULL;
    
    strcpy(result, template_str);
    
    // Replace parameter placeholders
    for (size_t i = 0; i < context->arg_count && i < context->macro->param_count; i++) {
        MacroParameter* param = &context->macro->parameters[i];
        ComptimeValue* arg = context->arguments[i];
        
        if (arg && arg->type == COMPTIME_VALUE_STRING) {
            // Simple string replacement (very basic)
            char placeholder[64];
            snprintf(placeholder, sizeof(placeholder), "$%s", param->name);
            
            char* pos = strstr(result, placeholder);
            if (pos) {
                // This is a simplified replacement - real implementation would be more robust
                size_t new_len = strlen(result) + strlen(arg->string_value) + 1;
                char* new_result = malloc(new_len);
                if (new_result) {
                    size_t prefix_len = pos - result;
                    strncpy(new_result, result, prefix_len);
                    new_result[prefix_len] = '\0';
                    strcat(new_result, arg->string_value);
                    strcat(new_result, pos + strlen(placeholder));
                    free(result);
                    result = new_result;
                }
            }
        }
    }
    
    return result;
}

// =============================================================================
// Built-in Macros
// =============================================================================

void register_builtin_macros(MacroRegistry* registry) {
    if (!registry) return;
    
    // Register assert! macro
    MacroTemplate* assert_macro = create_macro_template("assert", MACRO_FUNCTION);
    if (assert_macro) {
        add_macro_parameter(assert_macro, "condition", MACRO_PARAM_EXPR);
        add_macro_parameter(assert_macro, "message", MACRO_PARAM_STRING);
        assert_macro->evaluator = builtin_macro_assert;
        assert_macro->next = registry->builtin_macros;
        registry->builtin_macros = assert_macro;
    }
    
    // Register debug_print! macro
    MacroTemplate* debug_macro = create_macro_template("debug_print", MACRO_FUNCTION);
    if (debug_macro) {
        add_macro_parameter(debug_macro, "expr", MACRO_PARAM_EXPR);
        debug_macro->evaluator = builtin_macro_debug_print;
        debug_macro->next = registry->builtin_macros;
        registry->builtin_macros = debug_macro;
    }
    
    // Register typeof! macro
    MacroTemplate* typeof_macro = create_macro_template("typeof", MACRO_COMPILE_TIME);
    if (typeof_macro) {
        add_macro_parameter(typeof_macro, "expr", MACRO_PARAM_EXPR);
        typeof_macro->evaluator = builtin_macro_typeof;
        typeof_macro->next = registry->builtin_macros;
        registry->builtin_macros = typeof_macro;
    }
    
    // Register stringify! macro
    MacroTemplate* stringify_macro = create_macro_template("stringify", MACRO_FUNCTION);
    if (stringify_macro) {
        add_macro_parameter(stringify_macro, "expr", MACRO_PARAM_EXPR);
        stringify_macro->evaluator = builtin_macro_stringify;
        stringify_macro->next = registry->builtin_macros;
        registry->builtin_macros = stringify_macro;
    }
}

ComptimeValue* builtin_macro_assert(MacroContext* ctx, ComptimeValue** args) {
    if (!ctx || !args || ctx->arg_count < 1) return NULL;
    
    ComptimeValue* result = xmalloc(sizeof(ComptimeValue));
    if (!result) return NULL;
    
    result->type = COMPTIME_VALUE_STRING;
    
    // Generate assertion code
    char* assert_code = malloc(256);
    if (!assert_code) {
        free(result);
        return NULL;
    }
    
    if (ctx->arg_count >= 2 && args[1] && args[1]->type == COMPTIME_VALUE_STRING) {
        snprintf(assert_code, 256, "if (!(%s)) { panic(\"%s\"); }", 
                args[0]->string_value ? args[0]->string_value : "condition",
                args[1]->string_value);
    } else {
        snprintf(assert_code, 256, "if (!(%s)) { panic(\"Assertion failed\"); }", 
                args[0]->string_value ? args[0]->string_value : "condition");
    }
    
    result->string_value = assert_code;
    return result;
}

ComptimeValue* builtin_macro_debug_print(MacroContext* ctx, ComptimeValue** args) {
    if (!ctx || !args || ctx->arg_count < 1) return NULL;
    
    ComptimeValue* result = xmalloc(sizeof(ComptimeValue));
    if (!result) return NULL;
    
    result->type = COMPTIME_VALUE_STRING;
    
    char* debug_code = malloc(256);
    if (!debug_code) {
        free(result);
        return NULL;
    }
    
    snprintf(debug_code, 256, "printf(\"DEBUG: %s = %%s\\n\", stringify(%s));", 
            args[0]->string_value ? args[0]->string_value : "expr",
            args[0]->string_value ? args[0]->string_value : "expr");
    
    result->string_value = debug_code;
    return result;
}

ComptimeValue* builtin_macro_compile_time_if(MacroContext* ctx, ComptimeValue** args) {
    if (!ctx || !args || ctx->arg_count < 2) return NULL;
    
    ComptimeValue* result = xmalloc(sizeof(ComptimeValue));
    if (!result) return NULL;
    
    result->type = COMPTIME_VALUE_STRING;
    
    // Evaluate condition at compile time
    bool condition = false;
    if (args[0] && args[0]->type == COMPTIME_VALUE_BOOL) {
        condition = args[0]->bool_value;
    }
    
    if (condition && args[1] && args[1]->type == COMPTIME_VALUE_STRING) {
        result->string_value = strdup(args[1]->string_value);
    } else if (!condition && ctx->arg_count >= 3 && args[2] && args[2]->type == COMPTIME_VALUE_STRING) {
        result->string_value = strdup(args[2]->string_value);
    } else {
        result->string_value = strdup(""); // Empty expansion
    }
    
    return result;
}

ComptimeValue* builtin_macro_typeof(MacroContext* ctx, ComptimeValue** args) {
    if (!ctx || !args || ctx->arg_count < 1) return NULL;
    
    ComptimeValue* result = xmalloc(sizeof(ComptimeValue));
    if (!result) return NULL;
    
    result->type = COMPTIME_VALUE_TYPE;
    // In a real implementation, this would analyze the expression and return its type
    result->type_value = NULL; // Placeholder
    
    return result;
}

ComptimeValue* builtin_macro_stringify(MacroContext* ctx, ComptimeValue** args) {
    if (!ctx || !args || ctx->arg_count < 1) return NULL;
    
    ComptimeValue* result = xmalloc(sizeof(ComptimeValue));
    if (!result) return NULL;
    
    result->type = COMPTIME_VALUE_STRING;
    
    // Convert expression to string
    if (args[0] && args[0]->string_value) {
        result->string_value = malloc(strlen(args[0]->string_value) + 3);
        if (result->string_value) {
            sprintf(result->string_value, "\"%s\"", args[0]->string_value);
        }
    } else {
        result->string_value = strdup("\"\"");
    }
    
    return result;
}

// =============================================================================
// Hygiene System
// =============================================================================

char* generate_hygiene_name(MacroContext* context, const char* original_name) {
    if (!context || !original_name) return NULL;
    
    // Generate a unique name based on macro context
    char* hygiene_name = malloc(strlen(original_name) + 64);
    if (!hygiene_name) return NULL;
    
    snprintf(hygiene_name, strlen(original_name) + 64, "__%s_%p_%d", 
             original_name, (void*)context, context->recursion_depth);
    
    return hygiene_name;
}

bool preserve_hygiene(MacroContext* context, ASTNode* node) {
    if (!context || !node) return false;
    
    // Hygiene preservation logic would go here
    // This is a placeholder implementation
    return true;
}

bool check_hygiene_conflicts(MacroContext* context, const char* name) {
    if (!context || !name) return false;
    
    // Check for hygiene conflicts in the current scope
    // This is a placeholder implementation
    return false; // No conflicts
}

// =============================================================================
// Validation and Error Handling
// =============================================================================

bool validate_macro_arguments(MacroTemplate* macro, ComptimeValue** args, size_t arg_count) {
    if (!macro) return false;
    
    // Check required parameter count
    size_t required_params = 0;
    for (size_t i = 0; i < macro->param_count; i++) {
        if (!macro->parameters[i].is_optional) {
            required_params++;
        }
    }
    
    if (arg_count < required_params) {
        return false; // Not enough arguments
    }
    
    if (arg_count > macro->param_count) {
        return false; // Too many arguments
    }
    
    // Type checking would go here
    return true;
}

void macro_error(MacroContext* context, const char* format, ...) {
    if (!context || !format) return;
    
    va_list args;
    va_start(args, format);
    
    size_t msg_len = 256;
    context->error_message = malloc(msg_len);
    if (context->error_message) {
        vsnprintf(context->error_message, msg_len, format, args);
        context->has_error = true;
    }
    
    va_end(args);
}

ComptimeValue* evaluate_macro_at_compile_time(MacroContext* context) {
    if (!context || !context->macro || !context->macro->evaluator) return NULL;
    
    return context->macro->evaluator(context, context->arguments);
}

// =============================================================================
// Debugging and Introspection
// =============================================================================

void print_macro_registry(MacroRegistry* registry) {
    if (!registry) return;
    
    printf("\n=== Macro Registry ===\n");
    printf("Total macros: %zu\n", registry->macro_count);
    printf("Hygiene enabled: %s\n", registry->enable_hygiene ? "yes" : "no");
    printf("Debug expansions: %s\n", registry->debug_expansions ? "yes" : "no");
    printf("Max expansion depth: %d\n", registry->max_expansion_depth);
    
    printf("\nUser-defined macros:\n");
    MacroTemplate* current = registry->macros;
    while (current) {
        printf("  - %s (type: %d, params: %zu)\n", 
               current->name, current->type, current->param_count);
        current = current->next;
    }
    
    printf("\nBuilt-in macros:\n");
    current = registry->builtin_macros;
    while (current) {
        printf("  - %s (type: %d, params: %zu)\n", 
               current->name, current->type, current->param_count);
        current = current->next;
    }
    
    printf("=== End Registry ===\n\n");
}

char* get_macro_info(MacroTemplate* macro) {
    if (!macro) return NULL;
    
    char* info = malloc(512);
    if (!info) return NULL;
    
    snprintf(info, 512, 
             "Macro: %s\n"
             "Type: %d\n"
             "Hygiene: %d\n"
             "Parameters: %zu\n"
             "Recursive: %s\n"
             "Max recursion: %d\n",
             macro->name, macro->type, macro->hygiene, macro->param_count,
             macro->is_recursive ? "yes" : "no", macro->max_recursion);
    
    return info;
}

void print_macro_expansion_trace(MacroExpansion* expansion) {
    if (!expansion) return;
    
    printf("\n=== Macro Expansion Trace ===\n");
    printf("Success: %s\n", expansion->success ? "yes" : "no");
    
    if (expansion->error_message) {
        printf("Error: %s\n", expansion->error_message);
    }
    
    if (expansion->expanded_code) {
        printf("Expanded code:\n%s\n", expansion->expanded_code);
    }
    
    if (expansion->context && expansion->context->macro) {
        printf("Macro: %s\n", expansion->context->macro->name);
        printf("Arguments: %zu\n", expansion->context->arg_count);
    }
    
    printf("=== End Trace ===\n\n");
}

void debug_macro_expansion(MacroExpansion* expansion) {
    print_macro_expansion_trace(expansion);
}
