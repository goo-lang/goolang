#include "../include/advanced_macro_system.h"
#include "../include/errors/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

// Registry management
MacroRegistry* create_macro_registry(void) {
    MacroRegistry* registry = (MacroRegistry*)xcalloc(1, sizeof(MacroRegistry));
    if (!registry) {
        // Memory allocation failure - return NULL
        return NULL;
    }
    
    registry->enable_hygiene = true;
    registry->debug_expansions = false;
    registry->max_expansion_depth = 100;
    
    // Register built-in macros
    register_builtin_macros(registry);
    
    return registry;
}

void destroy_macro_registry(MacroRegistry* registry) {
    if (!registry) return;
    
    // Free all registered macros
    MacroTemplate* macro = registry->macros;
    while (macro) {
        MacroTemplate* next = macro->next;
        destroy_macro_template(macro);
        macro = next;
    }
    
    free(registry);
}

// Macro template management
MacroTemplate* create_macro_template(const char* name, MacroType type) {
    MacroTemplate* macro = (MacroTemplate*)xcalloc(1, sizeof(MacroTemplate));
    if (!macro) {
        // Memory allocation failure - return NULL
        return NULL;
    }
    
    macro->name = strdup(name);
    macro->type = type;
    macro->hygiene = HYGIENE_SEMANTIC;  // Default to semantic hygiene
    macro->max_recursion = 10;
    macro->generate_debug_info = true;
    
    return macro;
}

void destroy_macro_template(MacroTemplate* macro) {
    if (!macro) return;
    
    free(macro->name);
    free(macro->code_template);
    
    // Free parameters
    for (size_t i = 0; i < macro->param_count; i++) {
        free(macro->parameters[i].name);
        free(macro->parameters[i].constraint);
    }
    free(macro->parameters);
    
    free(macro);
}

bool register_macro(MacroRegistry* registry, MacroTemplate* macro) {
    if (!registry || !macro) return false;
    
    // Check for duplicate macro
    if (find_macro(registry, macro->name)) {
        // Macro already registered
        return false;
    }
    
    // Add to registry
    macro->next = registry->macros;
    registry->macros = macro;
    registry->macro_count++;
    
    return true;
}

MacroTemplate* find_macro(MacroRegistry* registry, const char* name) {
    if (!registry || !name) return NULL;
    
    // Search in user-defined macros
    MacroTemplate* macro = registry->macros;
    while (macro) {
        if (strcmp(macro->name, name) == 0) {
            return macro;
        }
        macro = macro->next;
    }
    
    // Search in built-in macros
    macro = registry->builtin_macros;
    while (macro) {
        if (strcmp(macro->name, name) == 0) {
            return macro;
        }
        macro = macro->next;
    }
    
    return NULL;
}

// Parameter management
bool add_macro_parameter(MacroTemplate* macro, const char* name, MacroParamType type) {
    if (!macro || !name) return false;
    
    // Resize parameters array
    MacroParameter* new_params = (MacroParameter*)realloc(
        macro->parameters,
        (macro->param_count + 1) * sizeof(MacroParameter)
    );
    if (!new_params) {
        // Memory allocation failure - return false
        return false;
    }
    
    macro->parameters = new_params;
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
        if (strcmp(macro->parameters[i].name, param_name) == 0) {
            free(macro->parameters[i].constraint);
            macro->parameters[i].constraint = strdup(constraint);
            return true;
        }
    }
    
    return false;
}

bool set_parameter_default(MacroTemplate* macro, const char* param_name, ComptimeValue* default_val) {
    if (!macro || !param_name || !default_val) return false;
    
    for (size_t i = 0; i < macro->param_count; i++) {
        if (strcmp(macro->parameters[i].name, param_name) == 0) {
            macro->parameters[i].is_optional = true;
            macro->parameters[i].default_value = default_val;
            return true;
        }
    }
    
    return false;
}

// Context management
MacroContext* create_macro_context(MacroTemplate* macro, ComptimeValue** args, size_t arg_count) {
    MacroContext* context = (MacroContext*)xcalloc(1, sizeof(MacroContext));
    if (!context) {
        // Memory allocation failure - return NULL
        return NULL;
    }
    
    context->macro = macro;
    context->arguments = args;
    context->arg_count = arg_count;
    context->hygiene_scope = NULL;
    context->scope_depth = 0;
    context->recursion_depth = 0;
    context->has_error = false;
    
    return context;
}

void destroy_macro_context(MacroContext* context) {
    if (!context) return;
    
    // Free hygiene scope
    for (size_t i = 0; i < context->scope_depth; i++) {
        free(context->hygiene_scope[i]);
    }
    free(context->hygiene_scope);
    
    free(context->error_message);
    free(context->source_file);
    free(context);
}

// Macro expansion
MacroExpansion* expand_macro(MacroRegistry* registry, const char* macro_name,
                            ComptimeValue** args, size_t arg_count, ASTNode* call_site) {
    if (!registry || !macro_name) return NULL;
    
    MacroExpansion* expansion = (MacroExpansion*)xcalloc(1, sizeof(MacroExpansion));
    if (!expansion) {
        // Memory allocation failure - return NULL
        return NULL;
    }
    
    // Find macro
    MacroTemplate* macro = find_macro(registry, macro_name);
    if (!macro) {
        expansion->success = false;
        expansion->error_message = strdup("Macro not found");
        return expansion;
    }
    
    // Validate arguments
    if (!validate_macro_arguments(macro, args, arg_count)) {
        expansion->success = false;
        expansion->error_message = strdup("Invalid macro arguments");
        return expansion;
    }
    
    // Create context
    MacroContext* context = create_macro_context(macro, args, arg_count);
    if (!context) {
        expansion->success = false;
        expansion->error_message = strdup("Failed to create macro context");
        return expansion;
    }
    
    context->call_site = call_site;
    expansion->context = context;
    
    // Handle different macro types
    switch (macro->type) {
        case MACRO_FUNCTION:
            // Expand function-like macro
            if (macro->evaluator) {
                ComptimeValue* result = macro->evaluator(context, args);
                if (result) {
                    expansion->expanded_ast = comptime_value_to_ast(result);
                    expansion->success = true;
                }
            } else if (macro->code_template) {
                expansion->expanded_code = process_template(macro->code_template, context);
                expansion->success = expansion->expanded_code != NULL;
            }
            break;
            
        case MACRO_TEMPLATE:
            // Process template-based macro
            if (macro->body) {
                expansion->expanded_ast = substitute_template_ast(macro->body, context);
                expansion->success = expansion->expanded_ast != NULL;
            } else if (macro->code_template) {
                expansion->expanded_code = process_template(macro->code_template, context);
                expansion->success = expansion->expanded_code != NULL;
            }
            break;
            
        case MACRO_PROCEDURAL:
            // Handle procedural macro (requires custom evaluator)
            if (macro->evaluator) {
                ComptimeValue* result = macro->evaluator(context, args);
                if (result) {
                    expansion->expanded_ast = comptime_value_to_ast(result);
                    expansion->success = true;
                }
            }
            break;
            
        default:
            expansion->success = false;
            expansion->error_message = strdup("Unsupported macro type");
            break;
    }
    
    // Apply hygiene if enabled
    if (expansion->success && registry->enable_hygiene && macro->hygiene != HYGIENE_NONE) {
        preserve_hygiene(context, expansion->expanded_ast);
    }
    
    // Generate debug info if requested
    if (registry->debug_expansions && macro->generate_debug_info) {
        debug_macro_expansion(expansion);
    }
    
    return expansion;
}

MacroExpansion* expand_macro_ast(MacroRegistry* registry, ASTNode* macro_call) {
    if (!registry || !macro_call) return NULL;
    
    // Extract macro name and arguments from AST
    // This is simplified - real implementation would parse AST structure
    const char* macro_name = "unknown";
    if (macro_call->type == AST_IDENTIFIER) {
        macro_name = ((IdentifierNode*)macro_call)->name;
    }
    
    // Convert AST arguments to ComptimeValues
    ComptimeValue** args = NULL;
    size_t arg_count = 0;
    
    return expand_macro(registry, macro_name, args, arg_count, macro_call);
}

// Hygiene system
bool preserve_hygiene(MacroContext* context, ASTNode* node) {
    if (!context || !node) return false;
    
    // Traverse AST and rename identifiers to preserve hygiene
    switch (node->type) {
        case AST_IDENTIFIER:
            if (check_hygiene_conflicts(context, ((IdentifierNode*)node)->name)) {
                char* hygiene_name = generate_hygiene_name(context, ((IdentifierNode*)node)->name);
                free(((IdentifierNode*)node)->name);
                ((IdentifierNode*)node)->name = hygiene_name;
            }
            break;
            
        case AST_FUNC_DECL:
        case AST_VAR_DECL:
            // Add to hygiene scope
            context->hygiene_scope = (char**)realloc(
                context->hygiene_scope,
                (context->scope_depth + 1) * sizeof(char*)
            );
            // Add variable/function name to hygiene scope
            // This would need proper handling based on node structure
            break;
            
        default:
            // Recursively process children
            break;
    }
    
    return true;
}

char* generate_hygiene_name(MacroContext* context, const char* original_name) {
    if (!context || !original_name) return NULL;
    
    // Generate unique name based on context
    char* hygiene_name = (char*)malloc(strlen(original_name) + 32);
    sprintf(hygiene_name, "%s__macro_%p_%d", original_name, (void*)context, context->recursion_depth);
    
    return hygiene_name;
}

bool check_hygiene_conflicts(MacroContext* context, const char* name) {
    if (!context || !name) return false;
    
    // Check if name exists in outer scopes
    for (size_t i = 0; i < context->scope_depth; i++) {
        if (strcmp(context->hygiene_scope[i], name) == 0) {
            return true;
        }
    }
    
    return false;
}

// Built-in macros
void register_builtin_macros(MacroRegistry* registry) {
    if (!registry) return;
    
    // assert! macro
    MacroTemplate* assert_macro = create_macro_template("assert!", MACRO_FUNCTION);
    assert_macro->evaluator = builtin_macro_assert;
    add_macro_parameter(assert_macro, "condition", MACRO_PARAM_EXPR);
    add_macro_parameter(assert_macro, "message", MACRO_PARAM_LITERAL);
    set_parameter_default(assert_macro, "message", create_comptime_string("Assertion failed"));
    assert_macro->next = registry->builtin_macros;
    registry->builtin_macros = assert_macro;
    
    // debug_print! macro
    MacroTemplate* debug_print = create_macro_template("debug_print!", MACRO_FUNCTION);
    debug_print->evaluator = builtin_macro_debug_print;
    add_macro_parameter(debug_print, "value", MACRO_PARAM_EXPR);
    debug_print->next = registry->builtin_macros;
    registry->builtin_macros = debug_print;
    
    // typeof! macro
    MacroTemplate* typeof_macro = create_macro_template("typeof!", MACRO_FUNCTION);
    typeof_macro->evaluator = builtin_macro_typeof;
    add_macro_parameter(typeof_macro, "expr", MACRO_PARAM_EXPR);
    typeof_macro->next = registry->builtin_macros;
    registry->builtin_macros = typeof_macro;
    
    // stringify! macro
    MacroTemplate* stringify = create_macro_template("stringify!", MACRO_FUNCTION);
    stringify->evaluator = builtin_macro_stringify;
    add_macro_parameter(stringify, "expr", MACRO_PARAM_EXPR);
    stringify->next = registry->builtin_macros;
    registry->builtin_macros = stringify;
}

ComptimeValue* builtin_macro_assert(MacroContext* ctx, ComptimeValue** args) {
    if (!ctx || !args || ctx->arg_count < 1) return NULL;
    
    ComptimeValue* condition = args[0];
    if (!comptime_value_to_bool(condition)) {
        const char* message = "Assertion failed";
        if (ctx->arg_count > 1 && args[1]->type == COMPTIME_VALUE_STRING) {
            message = args[1]->string_value;
        }
        
        macro_error(ctx, "%s", message);
        return NULL;
    }
    
    // Return void on success
    return create_comptime_void();
}

ComptimeValue* builtin_macro_debug_print(MacroContext* ctx, ComptimeValue** args) {
    if (!ctx || !args || ctx->arg_count < 1) return NULL;
    
    // Print debug information
    printf("[DEBUG] ");
    print_comptime_value(args[0]);
    printf("\n");
    
    return create_comptime_void();
}

ComptimeValue* builtin_macro_typeof(MacroContext* ctx, ComptimeValue** args) {
    if (!ctx || !args || ctx->arg_count < 1) return NULL;
    
    ComptimeValue* expr = args[0];
    Type* type = comptime_value_get_type(expr);
    
    return create_comptime_type(type);
}

ComptimeValue* builtin_macro_stringify(MacroContext* ctx, ComptimeValue** args) {
    if (!ctx || !args || ctx->arg_count < 1) return NULL;
    
    char* str = comptime_value_to_string(args[0]);
    return create_comptime_string(str);
}

// Template processing
char* process_template(const char* template_str, MacroContext* context) {
    if (!template_str || !context) return NULL;
    
    // Simple template processing - replace {{param}} with values
    char* result = (char*)malloc(strlen(template_str) * 10);  // Allocate extra space
    char* out = result;
    const char* in = template_str;
    
    while (*in) {
        if (in[0] == '{' && in[1] == '{') {
            // Find parameter name
            const char* start = in + 2;
            const char* end = strstr(start, "}}");
            if (end) {
                size_t name_len = end - start;
                char* param_name = (char*)malloc(name_len + 1);
                if (!param_name) { // allocation failure: skip this {{...}} substitution
                    in = end + 2;
                    continue;
                }
                strncpy(param_name, start, name_len);
                param_name[name_len] = '\0';
                
                // Find parameter value
                for (size_t i = 0; i < context->macro->param_count && i < context->arg_count; i++) {
                    if (strcmp(context->macro->parameters[i].name, param_name) == 0) {
                        char* value_str = comptime_value_to_string(context->arguments[i]);
                        strcpy(out, value_str);
                        out += strlen(value_str);
                        free(value_str);
                        break;
                    }
                }
                
                free(param_name);
                in = end + 2;
                continue;
            }
        }
        
        *out++ = *in++;
    }
    
    *out = '\0';
    return result;
}

ASTNode* substitute_template_ast(ASTNode* template_ast, MacroContext* context) {
    if (!template_ast || !context) return NULL;

    // Deep copy AST and substitute parameters
    // This is a simplified implementation
    //
    // Was: ast_node_copy(template_ast). That helper has been deleted (it
    // allocated only sizeof(ASTNode) then wrote derived-struct fields past
    // the allocation — a latent heap overflow). There is no safe drop-in
    // replacement: a real fix means a typed-constructor deep copy over the
    // full node-kind switch (see ast_type_clone / clone_const_value for the
    // pattern), which is unwarranted here since this whole path is dead
    // code — expand_macro_ast (the only way to reach this function) has no
    // callers anywhere in the compiler. Substitute parameters in the copied
    // AST — real implementation would traverse and substitute.
    return NULL;
}

// Error handling
void macro_error(MacroContext* context, const char* format, ...) {
    if (!context) return;
    
    va_list args;
    va_start(args, format);
    
    context->has_error = true;
    context->error_message = (char*)malloc(1024);
    vsnprintf(context->error_message, 1024, format, args);
    
    va_end(args);
    
    // Report error with location info
    // In a real implementation, we would report to an error context
    // For now, just print the error
    fprintf(stderr, "Macro error at %s:%d:%d: %s\n", 
            context->source_file ? context->source_file : "<unknown>",
            context->line, context->column, context->error_message);
}

void print_macro_expansion_trace(MacroExpansion* expansion) {
    if (!expansion) return;
    
    printf("Macro Expansion Trace:\n");
    printf("  Success: %s\n", expansion->success ? "Yes" : "No");
    
    if (expansion->error_message) {
        printf("  Error: %s\n", expansion->error_message);
    }
    
    if (expansion->context) {
        printf("  Macro: %s\n", expansion->context->macro->name);
        printf("  Arguments: %zu\n", expansion->context->arg_count);
        printf("  Recursion Depth: %d\n", expansion->context->recursion_depth);
    }
    
    if (expansion->expanded_code) {
        printf("  Expanded Code:\n%s\n", expansion->expanded_code);
    }
}

// Debugging and introspection
void print_macro_registry(MacroRegistry* registry) {
    if (!registry) return;
    
    printf("Macro Registry:\n");
    printf("  Total Macros: %zu\n", registry->macro_count);
    printf("  Hygiene Enabled: %s\n", registry->enable_hygiene ? "Yes" : "No");
    printf("  Max Expansion Depth: %d\n", registry->max_expansion_depth);
    
    printf("\nRegistered Macros:\n");
    MacroTemplate* macro = registry->macros;
    while (macro) {
        printf("  - %s (type: %d, params: %zu)\n", 
               macro->name, macro->type, macro->param_count);
        macro = macro->next;
    }
    
    printf("\nBuilt-in Macros:\n");
    macro = registry->builtin_macros;
    while (macro) {
        printf("  - %s (type: %d, params: %zu)\n", 
               macro->name, macro->type, macro->param_count);
        macro = macro->next;
    }
}

char* get_macro_info(MacroTemplate* macro) {
    if (!macro) return NULL;
    
    char* info = (char*)malloc(1024);
    snprintf(info, 1024, "Macro: %s\nType: %d\nParameters: %zu\nHygiene: %d\nRecursive: %s",
             macro->name, macro->type, macro->param_count, macro->hygiene,
             macro->is_recursive ? "Yes" : "No");
    
    return info;
}

void debug_macro_expansion(MacroExpansion* expansion) {
    if (!expansion) return;
    
    print_macro_expansion_trace(expansion);
    
    if (expansion->expanded_ast) {
        printf("\nExpanded AST:\n");
        // Print AST structure (implementation depends on AST printing functions)
    }
}

// Compile-time macro evaluation
ComptimeValue* evaluate_macro_at_compile_time(MacroContext* context) {
    if (!context || !context->macro) return NULL;
    
    if (context->macro->evaluator) {
        return context->macro->evaluator(context, context->arguments);
    }
    
    return NULL;
}

bool validate_macro_arguments(MacroTemplate* macro, ComptimeValue** args, size_t arg_count) {
    if (!macro) return false;
    
    // Count required parameters
    size_t required_count = 0;
    for (size_t i = 0; i < macro->param_count; i++) {
        if (!macro->parameters[i].is_optional) {
            required_count++;
        }
    }
    
    // Check argument count
    if (arg_count < required_count || arg_count > macro->param_count) {
        return false;
    }
    
    // Validate argument types
    for (size_t i = 0; i < arg_count && i < macro->param_count; i++) {
        MacroParameter* param = &macro->parameters[i];
        
        // Type checking would go here based on param->type
        // For now, assume all arguments are valid
    }
    
    return true;
}