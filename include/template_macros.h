#ifndef TEMPLATE_MACROS_H
#define TEMPLATE_MACROS_H

#include "advanced_macro_system.h"
#include "ast.h"
#include "types.h"

// Template macro types
typedef enum {
    TEMPLATE_FUNCTION,      // Generate functions from templates
    TEMPLATE_STRUCT,        // Generate struct definitions
    TEMPLATE_INTERFACE,     // Generate interface definitions
    TEMPLATE_IMPL,          // Generate implementation blocks
    TEMPLATE_CRUD,          // Generate CRUD operations
    TEMPLATE_API_CLIENT,    // Generate API client code
    TEMPLATE_DSL,           // Domain-specific language support
    TEMPLATE_COUNT
} TemplateMacroType;

// Template parameter types
typedef enum {
    TEMPLATE_PARAM_TYPE,        // Type parameter (T, U, etc.)
    TEMPLATE_PARAM_VALUE,       // Value parameter (N, size, etc.)
    TEMPLATE_PARAM_EXPR,        // Expression parameter
    TEMPLATE_PARAM_PATTERN,     // Pattern parameter
    TEMPLATE_PARAM_BLOCK,       // Code block parameter
    TEMPLATE_PARAM_LIST,        // List of parameters
    TEMPLATE_PARAM_MAP          // Key-value mapping
} TemplateParamType;

// Template parameter
typedef struct {
    char* name;                 // Parameter name
    TemplateParamType type;     // Parameter type
    ComptimeValue* value;       // Parameter value
    char* constraint;           // Type constraint (optional)
    char* default_value;        // Default value (optional)
    bool is_variadic;           // Whether parameter accepts multiple values
} TemplateParameter;

// Template filter/transformation functions
typedef enum {
    FILTER_LOWERCASE,           // {{name | lowercase}}
    FILTER_UPPERCASE,           // {{name | uppercase}}
    FILTER_CAPITALIZE,          // {{name | capitalize}}
    FILTER_SNAKE_CASE,          // {{name | snake_case}}
    FILTER_CAMEL_CASE,          // {{name | camel_case}}
    FILTER_PASCAL_CASE,         // {{name | pascal_case}}
    FILTER_KEBAB_CASE,          // {{name | kebab_case}}
    FILTER_PLURAL,              // {{name | plural}}
    FILTER_SINGULAR,            // {{name | singular}}
    FILTER_ESCAPE,              // {{string | escape}}
    FILTER_QUOTE,               // {{string | quote}}
    FILTER_TYPE_NAME,           // {{type | type_name}}
    FILTER_FIELD_NAMES,         // {{struct | field_names}}
    FILTER_FIELD_TYPES,         // {{struct | field_types}}
    FILTER_COUNT
} TemplateFilter;

// Template context for code generation
typedef struct {
    TemplateParameter* parameters;  // Template parameters
    size_t param_count;            // Number of parameters
    
    char* template_code;           // Template source code
    char* generated_code;          // Generated output code
    
    // Code generation settings
    bool preserve_whitespace;      // Whether to preserve whitespace
    bool generate_comments;        // Whether to add generation comments
    char* output_namespace;        // Target namespace/module
    
    // Error handling
    char* error_message;           // Error message if generation fails
    bool has_error;                // Whether an error occurred
    
    // Debug information
    char* expansion_trace;         // Trace of template expansion
    int recursion_depth;           // Current recursion depth
} TemplateContext;

// Template expansion result
typedef struct {
    char* code;                    // Generated code
    char** additional_files;       // Additional files to generate
    size_t file_count;             // Number of additional files
    bool success;                  // Whether expansion succeeded
    char* error_message;           // Error message if failed
    
    // Metadata
    char** generated_functions;    // List of generated function names
    size_t function_count;
    char** generated_types;        // List of generated type names
    size_t type_count;
} TemplateExpansionResult;

// Core template macro functions
bool register_template_macros(MacroRegistry* registry);
TemplateContext* create_template_context(const char* template_code);
void destroy_template_context(TemplateContext* ctx);
void destroy_template_expansion_result(TemplateExpansionResult* result);

// Template parameter management
bool add_template_parameter(TemplateContext* ctx, const char* name, 
                           TemplateParamType type, ComptimeValue* value);
TemplateParameter* find_template_parameter(TemplateContext* ctx, const char* name);
bool set_template_parameter_constraint(TemplateContext* ctx, const char* name, const char* constraint);

// Template processing
TemplateExpansionResult* expand_template(TemplateContext* ctx);
char* process_template_string(const char* template_str, TemplateContext* ctx);
char* substitute_parameters(const char* template_str, TemplateContext* ctx);

// Template filters
char* apply_template_filter(const char* input, TemplateFilter filter);
char* apply_filter_chain(const char* input, const char* filter_chain);
TemplateFilter parse_filter_name(const char* filter_str);

// Built-in template macros
ComptimeValue* template_macro_evaluator(MacroContext* ctx, ComptimeValue** args);
TemplateExpansionResult* generate_crud_template(TemplateContext* ctx);
TemplateExpansionResult* generate_api_client_template(TemplateContext* ctx);
TemplateExpansionResult* generate_struct_template(TemplateContext* ctx);

// Template introspection and analysis
bool analyze_template_dependencies(const char* template_str, char*** deps, size_t* dep_count);
bool validate_template_syntax(const char* template_str);
char* get_template_signature(TemplateContext* ctx);

// Error handling
void template_error(TemplateContext* ctx, const char* format, ...);
void print_template_expansion_trace(TemplateExpansionResult* result);

// String transformation utilities
char* to_lowercase(const char* str);
char* to_uppercase(const char* str);
char* to_capitalize(const char* str);
char* to_snake_case(const char* str);
char* to_camel_case(const char* str);
char* to_pascal_case(const char* str);
char* to_kebab_case(const char* str);
char* to_plural(const char* str);
char* to_singular(const char* str);
char* escape_string(const char* str);
char* quote_string(const char* str);

// Type analysis utilities
char* extract_type_name(Type* type);
char** extract_struct_field_names(Type* struct_type, size_t* count);
Type** extract_struct_field_types(Type* struct_type, size_t* count);

// Debug and introspection
void print_template_context(TemplateContext* ctx);
void print_template_parameters(TemplateContext* ctx);
char* get_template_info(TemplateContext* ctx);

#endif // TEMPLATE_MACROS_H