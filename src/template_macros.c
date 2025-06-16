#include "../include/template_macros.h"
#include "../include/errors/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

// Template filter names
static const char* filter_names[] = {
    [FILTER_LOWERCASE] = "lowercase",
    [FILTER_UPPERCASE] = "uppercase", 
    [FILTER_CAPITALIZE] = "capitalize",
    [FILTER_SNAKE_CASE] = "snake_case",
    [FILTER_CAMEL_CASE] = "camel_case",
    [FILTER_PASCAL_CASE] = "pascal_case",
    [FILTER_KEBAB_CASE] = "kebab_case",
    [FILTER_PLURAL] = "plural",
    [FILTER_SINGULAR] = "singular",
    [FILTER_ESCAPE] = "escape",
    [FILTER_QUOTE] = "quote",
    [FILTER_TYPE_NAME] = "type_name",
    [FILTER_FIELD_NAMES] = "field_names",
    [FILTER_FIELD_TYPES] = "field_types"
};

// Register template macros
bool register_template_macros(MacroRegistry* registry) {
    if (!registry) return false;
    
    // Create template macro
    MacroTemplate* template_macro = create_macro_template("template", MACRO_TEMPLATE);
    if (!template_macro) return false;
    
    template_macro->evaluator = template_macro_evaluator;
    add_macro_parameter(template_macro, "template_code", MACRO_PARAM_EXPR);
    add_macro_parameter(template_macro, "parameters", MACRO_PARAM_PATTERN);
    
    return register_macro(registry, template_macro);
}

// Create template context
TemplateContext* create_template_context(const char* template_code) {
    if (!template_code) return NULL;
    
    TemplateContext* ctx = (TemplateContext*)calloc(1, sizeof(TemplateContext));
    if (!ctx) return NULL;
    
    ctx->template_code = strdup(template_code);
    ctx->preserve_whitespace = true;
    ctx->generate_comments = true;
    ctx->recursion_depth = 0;
    
    return ctx;
}

// Destroy template context
void destroy_template_context(TemplateContext* ctx) {
    if (!ctx) return;
    
    // Free parameters
    for (size_t i = 0; i < ctx->param_count; i++) {
        free(ctx->parameters[i].name);
        free(ctx->parameters[i].constraint);
        free(ctx->parameters[i].default_value);
        if (ctx->parameters[i].value) {
            comptime_value_free(ctx->parameters[i].value);
        }
    }
    free(ctx->parameters);
    
    free(ctx->template_code);
    free(ctx->generated_code);
    free(ctx->output_namespace);
    free(ctx->error_message);
    free(ctx->expansion_trace);
    free(ctx);
}

// Destroy template expansion result
void destroy_template_expansion_result(TemplateExpansionResult* result) {
    if (!result) return;
    
    free(result->code);
    free(result->error_message);
    
    // Free additional files
    for (size_t i = 0; i < result->file_count; i++) {
        free(result->additional_files[i]);
    }
    free(result->additional_files);
    
    // Free generated functions
    for (size_t i = 0; i < result->function_count; i++) {
        free(result->generated_functions[i]);
    }
    free(result->generated_functions);
    
    // Free generated types
    for (size_t i = 0; i < result->type_count; i++) {
        free(result->generated_types[i]);
    }
    free(result->generated_types);
    
    free(result);
}

// Add template parameter
bool add_template_parameter(TemplateContext* ctx, const char* name,
                           TemplateParamType type, ComptimeValue* value) {
    if (!ctx || !name) return false;
    
    // Resize parameters array
    TemplateParameter* new_params = (TemplateParameter*)realloc(
        ctx->parameters,
        (ctx->param_count + 1) * sizeof(TemplateParameter)
    );
    if (!new_params) return false;
    
    ctx->parameters = new_params;
    TemplateParameter* param = &ctx->parameters[ctx->param_count];
    
    param->name = strdup(name);
    param->type = type;
    param->value = value;
    param->constraint = NULL;
    param->default_value = NULL;
    param->is_variadic = false;
    
    ctx->param_count++;
    return true;
}

// Find template parameter
TemplateParameter* find_template_parameter(TemplateContext* ctx, const char* name) {
    if (!ctx || !name) return NULL;
    
    for (size_t i = 0; i < ctx->param_count; i++) {
        if (strcmp(ctx->parameters[i].name, name) == 0) {
            return &ctx->parameters[i];
        }
    }
    return NULL;
}

// Process template string with parameter substitution
char* process_template_string(const char* template_str, TemplateContext* ctx) {
    if (!template_str || !ctx) return NULL;
    
    size_t result_size = strlen(template_str) * 3; // Allocate extra space
    char* result = (char*)malloc(result_size);
    if (!result) return NULL;
    
    char* out = result;
    const char* in = template_str;
    
    while (*in && (size_t)(out - result) < (result_size - 200)) {
        if (in[0] == '{' && in[1] == '{') {
            // Find the end of the template expression
            const char* start = in + 2;
            const char* end = strstr(start, "}}");
            if (!end) {
                *out++ = *in++;
                continue;
            }
            
            // Extract the expression
            size_t expr_len = end - start;
            char* expr = (char*)malloc(expr_len + 1);
            strncpy(expr, start, expr_len);
            expr[expr_len] = '\0';
            
            // Check for filters (param | filter)
            char* pipe = strchr(expr, '|');
            char* param_name = expr;
            char* filter_name = NULL;
            
            if (pipe) {
                *pipe = '\0';
                param_name = expr;
                filter_name = pipe + 1;
                
                // Trim whitespace
                while (*param_name == ' ') param_name++;
                while (*filter_name == ' ') filter_name++;
                
                char* end_param = param_name + strlen(param_name) - 1;
                while (end_param > param_name && *end_param == ' ') {
                    *end_param-- = '\0';
                }
            }
            
            // Find parameter value
            TemplateParameter* param = find_template_parameter(ctx, param_name);
            if (param && param->value) {
                char* value_str = comptime_value_to_string(param->value);
                if (value_str) {
                    // Apply filter if specified
                    if (filter_name) {
                        char* filtered = apply_filter_chain(value_str, filter_name);
                        if (filtered) {
                            size_t filtered_len = strlen(filtered);
                            if ((size_t)(out - result) + filtered_len < result_size - 1) {
                                strcpy(out, filtered);
                                out += filtered_len;
                            }
                            free(filtered);
                        }
                        free(value_str);
                    } else {
                        size_t value_len = strlen(value_str);
                        if ((size_t)(out - result) + value_len < result_size - 1) {
                            strcpy(out, value_str);
                            out += value_len;
                        }
                        free(value_str);
                    }
                }
            } else {
                // Parameter not found, keep the original expression
                sprintf(out, "{{%s}}", expr);
                out += strlen(out);
            }
            
            free(expr);
            in = end + 2;
        } else {
            *out++ = *in++;
        }
    }
    
    *out = '\0';
    return result;
}

// Expand template
TemplateExpansionResult* expand_template(TemplateContext* ctx) {
    if (!ctx) return NULL;
    
    TemplateExpansionResult* result = (TemplateExpansionResult*)calloc(1, sizeof(TemplateExpansionResult));
    if (!result) return NULL;
    
    // Process the template
    result->code = process_template_string(ctx->template_code, ctx);
    if (result->code) {
        result->success = true;
        
        // Add generation comment if requested
        if (ctx->generate_comments) {
            char* commented_code = (char*)malloc(strlen(result->code) + 200);
            if (commented_code) {
                sprintf(commented_code, 
                    "// Auto-generated code from template\n"
                    "// Template parameters: %zu\n\n%s",
                    ctx->param_count, result->code);
                free(result->code);
                result->code = commented_code;
            }
        }
    } else {
        result->success = false;
        result->error_message = strdup("Template expansion failed");
    }
    
    return result;
}

// Apply template filter
char* apply_template_filter(const char* input, TemplateFilter filter) {
    if (!input) return NULL;
    
    switch (filter) {
        case FILTER_LOWERCASE:
            return to_lowercase(input);
        case FILTER_UPPERCASE:
            return to_uppercase(input);
        case FILTER_CAPITALIZE:
            return to_capitalize(input);
        case FILTER_SNAKE_CASE:
            return to_snake_case(input);
        case FILTER_CAMEL_CASE:
            return to_camel_case(input);
        case FILTER_PASCAL_CASE:
            return to_pascal_case(input);
        case FILTER_KEBAB_CASE:
            return to_kebab_case(input);
        case FILTER_PLURAL:
            return to_plural(input);
        case FILTER_SINGULAR:
            return to_singular(input);
        case FILTER_ESCAPE:
            return escape_string(input);
        case FILTER_QUOTE:
            return quote_string(input);
        default:
            return strdup(input);
    }
}

// Apply filter chain
char* apply_filter_chain(const char* input, const char* filter_chain) {
    if (!input || !filter_chain) return strdup(input);
    
    // For now, support single filters only
    // In a full implementation, would parse chains like "lowercase | plural"
    TemplateFilter filter = parse_filter_name(filter_chain);
    return apply_template_filter(input, filter);
}

// Parse filter name
TemplateFilter parse_filter_name(const char* filter_str) {
    if (!filter_str) return FILTER_COUNT;
    
    for (int i = 0; i < FILTER_COUNT; i++) {
        if (strcmp(filter_names[i], filter_str) == 0) {
            return (TemplateFilter)i;
        }
    }
    return FILTER_COUNT;
}

// Generate CRUD template
TemplateExpansionResult* generate_crud_template(TemplateContext* ctx) {
    if (!ctx) return NULL;
    
    // Set up CRUD template
    const char* crud_template = 
        "// CRUD operations for {{type_name}}\n\n"
        "func create_{{type_name | lowercase}}(data {{type_name}}) !{{type_name}} {\n"
        "    return database.insert(data)\n"
        "}\n\n"
        "func get_{{type_name | lowercase}}(id {{id_type}}) !?{{type_name}} {\n"
        "    return database.find_by_id<{{type_name}}>(id)\n"
        "}\n\n"
        "func update_{{type_name | lowercase}}(id {{id_type}}, data {{type_name}}) !{{type_name}} {\n"
        "    return database.update(id, data)\n"
        "}\n\n"
        "func delete_{{type_name | lowercase}}(id {{id_type}}) !void {\n"
        "    return database.delete<{{type_name}}>(id)\n"
        "}\n\n"
        "func list_{{type_name | lowercase | plural}}(limit int, offset int) ![]{{type_name}} {\n"
        "    return database.list<{{type_name}}>(limit, offset)\n"
        "}\n";
    
    free(ctx->template_code);
    ctx->template_code = strdup(crud_template);
    
    return expand_template(ctx);
}

// Generate API client template
TemplateExpansionResult* generate_api_client_template(TemplateContext* ctx) {
    if (!ctx) return NULL;
    
    const char* api_template = 
        "// API client for {{service_name}}\n\n"
        "struct {{service_name}}Client {\n"
        "    base_url: string\n"
        "    api_key: string\n"
        "    timeout: Duration\n"
        "}\n\n"
        "func new_{{service_name | lowercase}}_client(base_url string, api_key string) {{service_name}}Client {\n"
        "    return {{service_name}}Client{\n"
        "        base_url: base_url,\n"
        "        api_key: api_key,\n"
        "        timeout: Duration.seconds(30)\n"
        "    }\n"
        "}\n\n"
        "func (client *{{service_name}}Client) request(method string, path string, body any) !Response {\n"
        "    headers := map[string]string{\n"
        "        \"Authorization\": \"Bearer \" + client.api_key,\n"
        "        \"Content-Type\": \"application/json\"\n"
        "    }\n"
        "    \n"
        "    url := client.base_url + path\n"
        "    return http.request(method, url, body, headers, client.timeout)\n"
        "}\n";
    
    free(ctx->template_code);
    ctx->template_code = strdup(api_template);
    
    return expand_template(ctx);
}

// Template macro evaluator
ComptimeValue* template_macro_evaluator(MacroContext* ctx, ComptimeValue** args) {
    if (!ctx || !args || ctx->arg_count < 1) return NULL;
    
    // Get template code
    ComptimeValue* template_value = args[0];
    if (template_value->type != COMPTIME_VALUE_STRING) {
        return NULL;
    }
    
    // Create template context
    TemplateContext* template_ctx = create_template_context(template_value->string_value);
    if (!template_ctx) return NULL;
    
    // Add parameters from remaining arguments
    for (size_t i = 1; i < ctx->arg_count; i++) {
        char param_name[32];
        sprintf(param_name, "param%zu", i);
        add_template_parameter(template_ctx, param_name, TEMPLATE_PARAM_VALUE, args[i]);
    }
    
    // Expand template
    TemplateExpansionResult* result = expand_template(template_ctx);
    ComptimeValue* return_value = NULL;
    
    if (result && result->success) {
        return_value = create_comptime_string(result->code);
    }
    
    destroy_template_expansion_result(result);
    destroy_template_context(template_ctx);
    
    return return_value;
}

// String transformation utilities
char* to_lowercase(const char* str) {
    if (!str) return NULL;
    
    char* result = strdup(str);
    if (!result) return NULL;
    
    for (char* p = result; *p; p++) {
        *p = tolower(*p);
    }
    return result;
}

char* to_uppercase(const char* str) {
    if (!str) return NULL;
    
    char* result = strdup(str);
    if (!result) return NULL;
    
    for (char* p = result; *p; p++) {
        *p = toupper(*p);
    }
    return result;
}

char* to_capitalize(const char* str) {
    if (!str) return NULL;
    
    char* result = strdup(str);
    if (!result) return NULL;
    
    if (result[0]) {
        result[0] = toupper(result[0]);
        for (char* p = result + 1; *p; p++) {
            *p = tolower(*p);
        }
    }
    return result;
}

char* to_snake_case(const char* str) {
    if (!str) return NULL;
    
    size_t len = strlen(str);
    char* result = (char*)malloc(len * 2 + 1); // Extra space for underscores
    if (!result) return NULL;
    
    char* out = result;
    bool prev_was_lower = false;
    
    for (const char* in = str; *in; in++) {
        if (isupper(*in) && prev_was_lower) {
            *out++ = '_';
        }
        *out++ = tolower(*in);
        prev_was_lower = islower(*in);
    }
    *out = '\0';
    
    return result;
}

char* to_camel_case(const char* str) {
    if (!str) return NULL;
    
    char* result = (char*)malloc(strlen(str) + 1);
    if (!result) return NULL;
    
    char* out = result;
    bool capitalize_next = false;
    bool first_char = true;
    
    for (const char* in = str; *in; in++) {
        if (*in == '_' || *in == '-' || *in == ' ') {
            capitalize_next = true;
        } else {
            if (first_char) {
                *out++ = tolower(*in);
                first_char = false;
            } else if (capitalize_next) {
                *out++ = toupper(*in);
                capitalize_next = false;
            } else {
                *out++ = tolower(*in);
            }
        }
    }
    *out = '\0';
    
    return result;
}

char* to_pascal_case(const char* str) {
    char* camel = to_camel_case(str);
    if (camel && camel[0]) {
        camel[0] = toupper(camel[0]);
    }
    return camel;
}

char* to_kebab_case(const char* str) {
    if (!str) return NULL;
    
    char* snake = to_snake_case(str);
    if (!snake) return NULL;
    
    // Replace underscores with hyphens
    for (char* p = snake; *p; p++) {
        if (*p == '_') *p = '-';
    }
    
    return snake;
}

char* to_plural(const char* str) {
    if (!str) return NULL;
    
    size_t len = strlen(str);
    char* result = (char*)malloc(len + 4); // Extra space for plural ending
    if (!result) return NULL;
    
    strcpy(result, str);
    
    // Simple pluralization rules
    if (len > 0) {
        char last = str[len - 1];
        char second_last = len > 1 ? str[len - 2] : '\0';
        
        if (last == 'y' && second_last && strchr("aeiou", second_last) == NULL) {
            result[len - 1] = 'i';
            strcat(result, "es");
        } else if (last == 's' || last == 'x' || last == 'z' ||
                  (len > 1 && ((last == 'h' && (second_last == 'c' || second_last == 's'))))) {
            strcat(result, "es");
        } else if (last == 'f') {
            result[len - 1] = 'v';
            strcat(result, "es");
        } else if (len > 1 && last == 'e' && second_last == 'f') {
            result[len - 2] = 'v';
            strcat(result, "s");
        } else {
            strcat(result, "s");
        }
    }
    
    return result;
}

char* to_singular(const char* str) {
    if (!str) return NULL;
    
    size_t len = strlen(str);
    char* result = strdup(str);
    if (!result) return NULL;
    
    // Simple singularization rules (reverse of pluralization)
    if (len > 3 && strcmp(result + len - 3, "ies") == 0) {
        result[len - 3] = 'y';
        result[len - 2] = '\0';
    } else if (len > 2 && strcmp(result + len - 2, "es") == 0) {
        result[len - 2] = '\0';
    } else if (len > 1 && result[len - 1] == 's') {
        result[len - 1] = '\0';
    }
    
    return result;
}

char* escape_string(const char* str) {
    if (!str) return NULL;
    
    size_t len = strlen(str);
    char* result = (char*)malloc(len * 2 + 1); // Extra space for escaping
    if (!result) return NULL;
    
    char* out = result;
    for (const char* in = str; *in; in++) {
        switch (*in) {
            case '"': *out++ = '\\'; *out++ = '"'; break;
            case '\\': *out++ = '\\'; *out++ = '\\'; break;
            case '\n': *out++ = '\\'; *out++ = 'n'; break;
            case '\t': *out++ = '\\'; *out++ = 't'; break;
            case '\r': *out++ = '\\'; *out++ = 'r'; break;
            default: *out++ = *in; break;
        }
    }
    *out = '\0';
    
    return result;
}

char* quote_string(const char* str) {
    if (!str) return NULL;
    
    char* escaped = escape_string(str);
    if (!escaped) return NULL;
    
    size_t len = strlen(escaped);
    char* result = (char*)malloc(len + 3); // quotes + null terminator
    if (!result) {
        free(escaped);
        return NULL;
    }
    
    sprintf(result, "\"%s\"", escaped);
    free(escaped);
    
    return result;
}

// Error handling
void template_error(TemplateContext* ctx, const char* format, ...) {
    if (!ctx || !format) return;
    
    va_list args;
    va_start(args, format);
    
    ctx->has_error = true;
    free(ctx->error_message);
    ctx->error_message = (char*)malloc(1024);
    if (ctx->error_message) {
        vsnprintf(ctx->error_message, 1024, format, args);
    }
    
    va_end(args);
    
    fprintf(stderr, "Template error: %s\n", ctx->error_message);
}

// Debug functions
void print_template_context(TemplateContext* ctx) {
    if (!ctx) return;
    
    printf("Template Context:\n");
    printf("  Parameters: %zu\n", ctx->param_count);
    printf("  Generate Comments: %s\n", ctx->generate_comments ? "Yes" : "No");
    printf("  Preserve Whitespace: %s\n", ctx->preserve_whitespace ? "Yes" : "No");
    
    if (ctx->output_namespace) {
        printf("  Output Namespace: %s\n", ctx->output_namespace);
    }
    
    if (ctx->template_code) {
        printf("  Template (first 100 chars): %.100s%s\n", 
               ctx->template_code, 
               strlen(ctx->template_code) > 100 ? "..." : "");
    }
}

void print_template_parameters(TemplateContext* ctx) {
    if (!ctx) return;
    
    printf("Template Parameters:\n");
    for (size_t i = 0; i < ctx->param_count; i++) {
        TemplateParameter* param = &ctx->parameters[i];
        printf("  %s: type=%d", param->name, param->type);
        if (param->value) {
            char* value_str = comptime_value_to_string(param->value);
            printf(", value=%s", value_str);
            free(value_str);
        }
        if (param->constraint) {
            printf(", constraint=%s", param->constraint);
        }
        printf("\n");
    }
}

char* get_template_info(TemplateContext* ctx) {
    if (!ctx) return NULL;
    
    char* info = (char*)malloc(512);
    if (!info) return NULL;
    
    snprintf(info, 512, 
        "Template Info:\n"
        "  Parameters: %zu\n"
        "  Has Error: %s\n"
        "  Recursion Depth: %d",
        ctx->param_count,
        ctx->has_error ? "Yes" : "No",
        ctx->recursion_depth);
    
    return info;
}

// Stub implementations for type analysis utilities
char* extract_type_name(Type* type) {
    // In a real implementation, would extract actual type name
    return strdup("MockType");
}

char** extract_struct_field_names(Type* struct_type, size_t* count) {
    // Mock implementation
    *count = 2;
    char** names = (char**)malloc(2 * sizeof(char*));
    names[0] = strdup("field1");
    names[1] = strdup("field2");
    return names;
}

Type** extract_struct_field_types(Type* struct_type, size_t* count) {
    // Mock implementation
    *count = 2;
    Type** types = (Type**)malloc(2 * sizeof(Type*));
    types[0] = type_new(TYPE_INT64);
    types[1] = type_new(TYPE_STRING);
    return types;
}