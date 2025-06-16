#ifndef DSL_MACROS_H
#define DSL_MACROS_H

#include "advanced_macro_system.h"
#include "template_macros.h"
#include "ast.h"
#include "types.h"

// DSL types supported
typedef enum {
    DSL_SQL,                // SQL query DSL
    DSL_HTTP,               // HTTP API DSL
    DSL_JSON,               // JSON manipulation DSL
    DSL_REGEX,              // Regular expression DSL
    DSL_HTML,               // HTML template DSL
    DSL_CSS,                // CSS styling DSL
    DSL_GRAPHQL,            // GraphQL query DSL
    DSL_YAML,               // YAML configuration DSL
    DSL_TOML,               // TOML configuration DSL
    DSL_DOCKERFILE,         // Docker container DSL
    DSL_KUBERNETES,         // Kubernetes manifest DSL
    DSL_TERRAFORM,          // Infrastructure DSL
    DSL_OPENAPI,            // OpenAPI specification DSL
    DSL_COUNT
} DSLType;

// DSL parsing state
typedef enum {
    DSL_STATE_INITIAL,      // Starting state
    DSL_STATE_PARSING,      // Currently parsing DSL content
    DSL_STATE_COMPLETE,     // DSL parsing complete
    DSL_STATE_ERROR         // Error in DSL parsing
} DSLParsingState;

// DSL context for parsing and code generation
typedef struct {
    DSLType type;                   // Type of DSL
    char* dsl_content;              // Raw DSL content
    char* generated_code;           // Generated Goo code
    DSLParsingState state;          // Current parsing state
    
    // Parser configuration
    bool strict_mode;               // Whether to use strict parsing
    bool generate_validators;       // Whether to generate validation code
    bool optimize_queries;          // Whether to optimize generated queries
    char* target_namespace;         // Target namespace for generated code
    
    // Error handling
    char* error_message;            // Error message if parsing fails
    int error_line;                 // Line number of error
    int error_column;               // Column number of error
    
    // Metadata
    char** dependencies;            // Required dependencies
    size_t dependency_count;        // Number of dependencies
    char** exports;                 // Exported functions/types
    size_t export_count;            // Number of exports
} DSLContext;

// OpenAPI specification structure
typedef struct {
    char* title;                    // API title
    char* version;                  // API version
    char* base_url;                 // Base URL
    char** endpoints;               // List of endpoints
    size_t endpoint_count;          // Number of endpoints
    char** models;                  // Data models
    size_t model_count;             // Number of models
} OpenAPISpec;

// SQL query components
typedef struct {
    char* table_name;               // Target table
    char** columns;                 // Selected columns
    size_t column_count;            // Number of columns
    char* where_clause;             // WHERE conditions
    char* order_clause;             // ORDER BY clause
    char* group_clause;             // GROUP BY clause
    char* having_clause;            // HAVING clause
    int limit_value;                // LIMIT value (-1 if none)
    int offset_value;               // OFFSET value (-1 if none)
} SQLQuery;

// HTTP endpoint definition
typedef struct {
    char* method;                   // HTTP method (GET, POST, etc.)
    char* path;                     // URL path
    char** parameters;              // Path/query parameters
    size_t param_count;             // Number of parameters
    char* request_body_type;        // Request body type
    char* response_type;            // Response type
    char** headers;                 // Required headers
    size_t header_count;            // Number of headers
} HTTPEndpoint;

// Core DSL functions
DSLContext* create_dsl_context(DSLType type, const char* dsl_content);
void destroy_dsl_context(DSLContext* ctx);
bool register_dsl_macros(MacroRegistry* registry);

// DSL parsing functions
bool parse_dsl_content(DSLContext* ctx);
char* generate_code_from_dsl(DSLContext* ctx);
bool validate_dsl_syntax(DSLContext* ctx);

// SQL DSL support
SQLQuery* parse_sql_query(const char* sql_content);
char* generate_sql_function(SQLQuery* query, const char* func_name);
void destroy_sql_query(SQLQuery* query);

// HTTP/API DSL support  
HTTPEndpoint* parse_http_endpoint(const char* http_content);
char* generate_http_client_function(HTTPEndpoint* endpoint);
void destroy_http_endpoint(HTTPEndpoint* endpoint);

// OpenAPI support
OpenAPISpec* parse_openapi_spec(const char* openapi_content);
char* generate_api_client_from_spec(OpenAPISpec* spec);
void destroy_openapi_spec(OpenAPISpec* spec);

// JSON DSL support
char* generate_json_parser(const char* json_schema);
char* generate_json_serializer(const char* json_schema);

// Regex DSL support
char* generate_regex_matcher(const char* regex_pattern, const char* func_name);
char* generate_regex_validator(const char* regex_pattern, const char* func_name);

// HTML template DSL support
char* generate_html_template_function(const char* html_template, const char* func_name);

// Configuration DSL support (YAML/TOML)
char* generate_config_parser(const char* config_schema, DSLType config_type);
char* generate_config_validator(const char* config_schema, DSLType config_type);

// Infrastructure DSL support
char* generate_dockerfile_from_spec(const char* docker_spec);
char* generate_kubernetes_manifest(const char* k8s_spec);
char* generate_terraform_config(const char* tf_spec);

// Compile-time API generation
ComptimeValue* dsl_macro_evaluator(MacroContext* ctx, ComptimeValue** args);
char* generate_compile_time_api(const char* spec_content, DSLType spec_type);

// DSL optimization and analysis
bool optimize_dsl_code(DSLContext* ctx);
char** analyze_dsl_dependencies(DSLContext* ctx, size_t* dep_count);
bool validate_dsl_semantics(DSLContext* ctx);

// Code generation utilities
char* generate_function_wrapper(const char* func_name, const char* params, 
                               const char* return_type, const char* body);
char* generate_struct_definition(const char* struct_name, char** fields, size_t field_count);
char* generate_interface_implementation(const char* interface_name, char** methods, size_t method_count);

// Error handling and debugging
void dsl_error(DSLContext* ctx, const char* format, ...);
void print_dsl_parsing_trace(DSLContext* ctx);
char* get_dsl_error_context(DSLContext* ctx, int line, int column);

// Utility functions
DSLType parse_dsl_type(const char* type_name);
const char* dsl_type_to_string(DSLType type);
bool is_dsl_type_supported(DSLType type);

// Debug and introspection
void print_dsl_context(DSLContext* ctx);
char* get_dsl_info(DSLContext* ctx);
void debug_dsl_generation(DSLContext* ctx);

#endif // DSL_MACROS_H