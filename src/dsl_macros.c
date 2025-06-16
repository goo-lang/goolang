#include "../include/dsl_macros.h"
#include "../include/errors/error.h"
#include "../include/template_macros.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

// Simple implementation of strncasecmp for systems that don't have it
static int my_strncasecmp(const char* s1, const char* s2, size_t n) {
    if (n == 0) return 0;
    
    while (n-- > 0) {
        int c1 = tolower((unsigned char)*s1++);
        int c2 = tolower((unsigned char)*s2++);
        
        if (c1 != c2) return c1 - c2;
        if (c1 == 0) break;
    }
    return 0;
}

// Simple implementation of strcasecmp for systems that don't have it
static int my_strcasecmp(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1++);
        int c2 = tolower((unsigned char)*s2++);
        
        if (c1 != c2) return c1 - c2;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

// Simple implementation of strcasestr for systems that don't have it
static char* my_strcasestr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return (char*)haystack;
    
    for (const char* p = haystack; *p; p++) {
        if (my_strncasecmp(p, needle, needle_len) == 0) {
            return (char*)p;
        }
    }
    return NULL;
}

// Forward declarations for helper functions
static bool parse_sql_content(DSLContext* ctx);
static bool parse_http_content(DSLContext* ctx);
static bool parse_json_content(DSLContext* ctx);
static bool parse_regex_content(DSLContext* ctx);
static bool parse_html_content(DSLContext* ctx);
static bool parse_openapi_content(DSLContext* ctx);
static bool parse_config_content(DSLContext* ctx);
static bool parse_infrastructure_content(DSLContext* ctx);

static char* generate_sql_code(DSLContext* ctx);
static char* generate_http_code(DSLContext* ctx);
static char* generate_json_code(DSLContext* ctx);
static char* generate_regex_code(DSLContext* ctx);
static char* generate_html_code(DSLContext* ctx);
static char* generate_openapi_code(DSLContext* ctx);
static char* generate_config_code(DSLContext* ctx);
static char* generate_infrastructure_code(DSLContext* ctx);

// DSL type names
static const char* dsl_type_names[] = {
    [DSL_SQL] = "sql",
    [DSL_HTTP] = "http",
    [DSL_JSON] = "json",
    [DSL_REGEX] = "regex",
    [DSL_HTML] = "html",
    [DSL_CSS] = "css",
    [DSL_GRAPHQL] = "graphql",
    [DSL_YAML] = "yaml",
    [DSL_TOML] = "toml",
    [DSL_DOCKERFILE] = "dockerfile",
    [DSL_KUBERNETES] = "kubernetes",
    [DSL_TERRAFORM] = "terraform",
    [DSL_OPENAPI] = "openapi"
};

// Create DSL context
DSLContext* create_dsl_context(DSLType type, const char* dsl_content) {
    if (!dsl_content || type >= DSL_COUNT) return NULL;
    
    DSLContext* ctx = (DSLContext*)calloc(1, sizeof(DSLContext));
    if (!ctx) return NULL;
    
    ctx->type = type;
    ctx->dsl_content = strdup(dsl_content);
    ctx->state = DSL_STATE_INITIAL;
    ctx->strict_mode = true;
    ctx->generate_validators = true;
    ctx->optimize_queries = true;
    ctx->error_line = -1;
    ctx->error_column = -1;
    
    return ctx;
}

// Destroy DSL context
void destroy_dsl_context(DSLContext* ctx) {
    if (!ctx) return;
    
    free(ctx->dsl_content);
    free(ctx->generated_code);
    free(ctx->target_namespace);
    free(ctx->error_message);
    
    // Free dependencies
    for (size_t i = 0; i < ctx->dependency_count; i++) {
        free(ctx->dependencies[i]);
    }
    free(ctx->dependencies);
    
    // Free exports
    for (size_t i = 0; i < ctx->export_count; i++) {
        free(ctx->exports[i]);
    }
    free(ctx->exports);
    
    free(ctx);
}

// Register DSL macros with the macro registry
bool register_dsl_macros(MacroRegistry* registry) {
    if (!registry) return false;
    
    // Register SQL DSL macro
    MacroTemplate* sql_macro = create_macro_template("sql", MACRO_DSL);
    if (!sql_macro) return false;
    sql_macro->evaluator = dsl_macro_evaluator;
    add_macro_parameter(sql_macro, "query", MACRO_PARAM_STRING);
    if (!register_macro(registry, sql_macro)) return false;
    
    // Register HTTP DSL macro
    MacroTemplate* http_macro = create_macro_template("http", MACRO_DSL);
    if (!http_macro) return false;
    http_macro->evaluator = dsl_macro_evaluator;
    add_macro_parameter(http_macro, "endpoint_spec", MACRO_PARAM_STRING);
    if (!register_macro(registry, http_macro)) return false;
    
    // Register JSON DSL macro
    MacroTemplate* json_macro = create_macro_template("json", MACRO_DSL);
    if (!json_macro) return false;
    json_macro->evaluator = dsl_macro_evaluator;
    add_macro_parameter(json_macro, "schema", MACRO_PARAM_STRING);
    if (!register_macro(registry, json_macro)) return false;
    
    // Register OpenAPI DSL macro
    MacroTemplate* openapi_macro = create_macro_template("openapi", MACRO_DSL);
    if (!openapi_macro) return false;
    openapi_macro->evaluator = dsl_macro_evaluator;
    add_macro_parameter(openapi_macro, "spec", MACRO_PARAM_STRING);
    if (!register_macro(registry, openapi_macro)) return false;
    
    return true;
}

// Parse DSL content based on type
bool parse_dsl_content(DSLContext* ctx) {
    if (!ctx || !ctx->dsl_content) return false;
    
    ctx->state = DSL_STATE_PARSING;
    
    switch (ctx->type) {
        case DSL_SQL:
            return parse_sql_content(ctx);
        case DSL_HTTP:
            return parse_http_content(ctx);
        case DSL_JSON:
            return parse_json_content(ctx);
        case DSL_REGEX:
            return parse_regex_content(ctx);
        case DSL_HTML:
            return parse_html_content(ctx);
        case DSL_OPENAPI:
            return parse_openapi_content(ctx);
        case DSL_YAML:
        case DSL_TOML:
            return parse_config_content(ctx);
        case DSL_DOCKERFILE:
        case DSL_KUBERNETES:
        case DSL_TERRAFORM:
            return parse_infrastructure_content(ctx);
        default:
            dsl_error(ctx, "Unsupported DSL type: %s", dsl_type_to_string(ctx->type));
            return false;
    }
}

// Generate code from parsed DSL
char* generate_code_from_dsl(DSLContext* ctx) {
    if (!ctx || ctx->state != DSL_STATE_COMPLETE) return NULL;
    
    switch (ctx->type) {
        case DSL_SQL:
            return generate_sql_code(ctx);
        case DSL_HTTP:
            return generate_http_code(ctx);
        case DSL_JSON:
            return generate_json_code(ctx);
        case DSL_REGEX:
            return generate_regex_code(ctx);
        case DSL_HTML:
            return generate_html_code(ctx);
        case DSL_OPENAPI:
            return generate_openapi_code(ctx);
        case DSL_YAML:
        case DSL_TOML:
            return generate_config_code(ctx);
        case DSL_DOCKERFILE:
        case DSL_KUBERNETES:
        case DSL_TERRAFORM:
            return generate_infrastructure_code(ctx);
        default:
            return NULL;
    }
}

// Validate DSL syntax
bool validate_dsl_syntax(DSLContext* ctx) {
    if (!ctx || !ctx->dsl_content) return false;
    
    // Basic syntax validation - check for balanced braces/brackets
    int brace_count = 0;
    int bracket_count = 0;
    int paren_count = 0;
    
    for (const char* p = ctx->dsl_content; *p; p++) {
        switch (*p) {
            case '{': brace_count++; break;
            case '}': brace_count--; break;
            case '[': bracket_count++; break;
            case ']': bracket_count--; break;
            case '(': paren_count++; break;
            case ')': paren_count--; break;
        }
    }
    
    if (brace_count != 0 || bracket_count != 0 || paren_count != 0) {
        dsl_error(ctx, "Unbalanced delimiters in DSL content");
        return false;
    }
    
    return true;
}

// SQL DSL support
SQLQuery* parse_sql_query(const char* sql_content) {
    if (!sql_content) return NULL;
    
    SQLQuery* query = (SQLQuery*)calloc(1, sizeof(SQLQuery));
    if (!query) return NULL;
    
    // Simple SQL parsing - look for SELECT statements
    char* sql_copy = strdup(sql_content);
    char* token = strtok(sql_copy, " \t\n");
    
    if (token && my_strcasecmp(token, "SELECT") == 0) {
        // Parse columns until FROM
        query->columns = (char**)malloc(10 * sizeof(char*));
        query->column_count = 0;
        
        while ((token = strtok(NULL, " \t\n")) != NULL) {
            if (my_strcasecmp(token, "FROM") == 0) {
                token = strtok(NULL, " \t\n");
                if (token) {
                    query->table_name = strdup(token);
                }
                break;
            } else {
                // Add column (simplified - remove commas)
                char* clean_col = strdup(token);
                char* comma = strchr(clean_col, ',');
                if (comma) *comma = '\0';
                if (query->column_count < 10) {
                    query->columns[query->column_count++] = clean_col;
                } else {
                    free(clean_col);
                }
            }
        }
        
        // Look for WHERE clause
        char* where_pos = my_strcasestr(sql_content, "WHERE");
        if (where_pos) {
            char* order_pos = my_strcasestr(where_pos, "ORDER BY");
            char* limit_pos = my_strcasestr(where_pos, "LIMIT");
            
            // Extract WHERE clause
            char* where_end = order_pos ? order_pos : (limit_pos ? limit_pos : where_pos + strlen(where_pos));
            size_t where_len = where_end - where_pos - 5; // Skip "WHERE"
            query->where_clause = (char*)malloc(where_len + 1);
            strncpy(query->where_clause, where_pos + 5, where_len);
            query->where_clause[where_len] = '\0';
        }
    }
    
    query->limit_value = -1;
    query->offset_value = -1;
    
    free(sql_copy);
    return query;
}

// Generate SQL function
char* generate_sql_function(SQLQuery* query, const char* func_name) {
    if (!query || !func_name) return NULL;
    
    char* code = (char*)malloc(2048);
    if (!code) return NULL;
    
    snprintf(code, 2048,
        "// Auto-generated SQL function\n"
        "func %s(db *Database) ![]Row {\n"
        "    query := \"SELECT %s FROM %s",
        func_name,
        query->columns ? "specified columns" : "*",
        query->table_name ? query->table_name : "table");
    
    if (query->where_clause) {
        strncat(code, " WHERE ", 2048 - strlen(code) - 1);
        strncat(code, query->where_clause, 2048 - strlen(code) - 1);
    }
    
    strncat(code, "\"\n    return db.query(query)\n}\n", 2048 - strlen(code) - 1);
    
    return code;
}

// Destroy SQL query
void destroy_sql_query(SQLQuery* query) {
    if (!query) return;
    
    free(query->table_name);
    free(query->where_clause);
    free(query->order_clause);
    free(query->group_clause);
    free(query->having_clause);
    
    for (size_t i = 0; i < query->column_count; i++) {
        free(query->columns[i]);
    }
    free(query->columns);
    
    free(query);
}

// HTTP endpoint parsing
HTTPEndpoint* parse_http_endpoint(const char* http_content) {
    if (!http_content) return NULL;
    
    HTTPEndpoint* endpoint = (HTTPEndpoint*)calloc(1, sizeof(HTTPEndpoint));
    if (!endpoint) return NULL;
    
    // Simple HTTP endpoint parsing
    // Format: "GET /api/users/{id} -> User"
    char* content_copy = strdup(http_content);
    char* token = strtok(content_copy, " \t\n");
    
    if (token) {
        endpoint->method = strdup(token);
        
        token = strtok(NULL, " \t\n");
        if (token) {
            endpoint->path = strdup(token);
            
            // Look for response type after "->"
            char* arrow = strstr(http_content, "->");
            if (arrow) {
                char* response_type = arrow + 2;
                while (*response_type == ' ') response_type++;
                char* end = response_type;
                while (*end && *end != ' ' && *end != '\n') end++;
                
                size_t type_len = end - response_type;
                endpoint->response_type = (char*)malloc(type_len + 1);
                strncpy(endpoint->response_type, response_type, type_len);
                endpoint->response_type[type_len] = '\0';
            }
        }
    }
    
    free(content_copy);
    return endpoint;
}

// Generate HTTP client function
char* generate_http_client_function(HTTPEndpoint* endpoint) {
    if (!endpoint) return NULL;
    
    char* code = (char*)malloc(1024);
    if (!code) return NULL;
    
    char* func_name = to_lowercase(endpoint->method);
    char* path_name = strdup(endpoint->path);
    
    // Convert path to function name
    for (char* p = path_name; *p; p++) {
        if (*p == '/' || *p == '{' || *p == '}' || *p == '-') {
            *p = '_';
        }
    }
    
    snprintf(code, 1024,
        "// Auto-generated HTTP client function\n"
        "func %s_%s(client *HTTPClient) !%s {\n"
        "    url := client.base_url + \"%s\"\n"
        "    response := try client.request(\"%s\", url, nil)\n"
        "    return try response.decode<%s>()\n"
        "}\n",
        func_name, path_name,
        endpoint->response_type ? endpoint->response_type : "Response",
        endpoint->path,
        endpoint->method,
        endpoint->response_type ? endpoint->response_type : "Response");
    
    free(func_name);
    free(path_name);
    return code;
}

// Destroy HTTP endpoint
void destroy_http_endpoint(HTTPEndpoint* endpoint) {
    if (!endpoint) return;
    
    free(endpoint->method);
    free(endpoint->path);
    free(endpoint->request_body_type);
    free(endpoint->response_type);
    
    for (size_t i = 0; i < endpoint->param_count; i++) {
        free(endpoint->parameters[i]);
    }
    free(endpoint->parameters);
    
    for (size_t i = 0; i < endpoint->header_count; i++) {
        free(endpoint->headers[i]);
    }
    free(endpoint->headers);
    
    free(endpoint);
}

// OpenAPI specification parsing
OpenAPISpec* parse_openapi_spec(const char* openapi_content) {
    if (!openapi_content) return NULL;
    
    OpenAPISpec* spec = (OpenAPISpec*)calloc(1, sizeof(OpenAPISpec));
    if (!spec) return NULL;
    
    // Simplified OpenAPI parsing - look for basic info
    char* title_pos = strstr(openapi_content, "title:");
    if (title_pos) {
        char* title_start = title_pos + 6;
        while (*title_start == ' ' || *title_start == '"') title_start++;
        char* title_end = title_start;
        while (*title_end && *title_end != '\n' && *title_end != '"') title_end++;
        
        size_t title_len = title_end - title_start;
        spec->title = (char*)malloc(title_len + 1);
        strncpy(spec->title, title_start, title_len);
        spec->title[title_len] = '\0';
    }
    
    char* version_pos = strstr(openapi_content, "version:");
    if (version_pos) {
        char* version_start = version_pos + 8;
        while (*version_start == ' ' || *version_start == '"') version_start++;
        char* version_end = version_start;
        while (*version_end && *version_end != '\n' && *version_end != '"') version_end++;
        
        size_t version_len = version_end - version_start;
        spec->version = (char*)malloc(version_len + 1);
        strncpy(spec->version, version_start, version_len);
        spec->version[version_len] = '\0';
    }
    
    return spec;
}

// Generate API client from OpenAPI spec
char* generate_api_client_from_spec(OpenAPISpec* spec) {
    if (!spec) return NULL;
    
    char* code = (char*)malloc(2048);
    if (!code) return NULL;
    
    char* client_name = spec->title ? to_pascal_case(spec->title) : strdup("API");
    
    snprintf(code, 2048,
        "// Auto-generated API client for %s v%s\n\n"
        "struct %sClient {\n"
        "    base_url: string\n"
        "    api_key: string\n"
        "    timeout: Duration\n"
        "}\n\n"
        "func new_%s_client(base_url string, api_key string) %sClient {\n"
        "    return %sClient{\n"
        "        base_url: base_url,\n"
        "        api_key: api_key,\n"
        "        timeout: Duration.seconds(30)\n"
        "    }\n"
        "}\n\n"
        "func (client *%sClient) request(method string, path string, body any) !Response {\n"
        "    headers := map[string]string{\n"
        "        \"Authorization\": \"Bearer \" + client.api_key,\n"
        "        \"Content-Type\": \"application/json\"\n"
        "    }\n"
        "    \n"
        "    url := client.base_url + path\n"
        "    return http.request(method, url, body, headers, client.timeout)\n"
        "}\n",
        spec->title ? spec->title : "API",
        spec->version ? spec->version : "1.0.0",
        client_name,
        to_lowercase(client_name), client_name, client_name,
        client_name);
    
    free(client_name);
    return code;
}

// Destroy OpenAPI spec
void destroy_openapi_spec(OpenAPISpec* spec) {
    if (!spec) return;
    
    free(spec->title);
    free(spec->version);
    free(spec->base_url);
    
    for (size_t i = 0; i < spec->endpoint_count; i++) {
        free(spec->endpoints[i]);
    }
    free(spec->endpoints);
    
    for (size_t i = 0; i < spec->model_count; i++) {
        free(spec->models[i]);
    }
    free(spec->models);
    
    free(spec);
}

// JSON DSL support
char* generate_json_parser(const char* json_schema) {
    if (!json_schema) return NULL;
    
    char* code = (char*)malloc(1024);
    if (!code) return NULL;
    
    snprintf(code, 1024,
        "// Auto-generated JSON parser\n"
        "func parse_json(data []byte) !JSONValue {\n"
        "    parser := json.new_parser(data)\n"
        "    return try parser.parse()\n"
        "}\n\n"
        "func validate_json_schema(value JSONValue) !bool {\n"
        "    // Schema validation logic would go here\n"
        "    return true\n"
        "}\n");
    
    return code;
}

// JSON serializer generation
char* generate_json_serializer(const char* json_schema) {
    if (!json_schema) return NULL;
    
    char* code = (char*)malloc(1024);
    if (!code) return NULL;
    
    snprintf(code, 1024,
        "// Auto-generated JSON serializer\n"
        "func serialize_to_json(value any) !string {\n"
        "    serializer := json.new_serializer()\n"
        "    return try serializer.serialize(value)\n"
        "}\n");
    
    return code;
}

// Regex DSL support
char* generate_regex_matcher(const char* regex_pattern, const char* func_name) {
    if (!regex_pattern || !func_name) return NULL;
    
    char* code = (char*)malloc(1024);
    if (!code) return NULL;
    
    char* escaped_pattern = escape_string(regex_pattern);
    
    snprintf(code, 1024,
        "// Auto-generated regex matcher\n"
        "func %s(input string) !bool {\n"
        "    pattern := regex.compile(\"%s\")\n"
        "    return pattern.matches(input)\n"
        "}\n\n"
        "func %s_find_all(input string) ![]string {\n"
        "    pattern := regex.compile(\"%s\")\n"
        "    return pattern.find_all(input)\n"
        "}\n",
        func_name, escaped_pattern, func_name, escaped_pattern);
    
    free(escaped_pattern);
    return code;
}

// Regex validator generation
char* generate_regex_validator(const char* regex_pattern, const char* func_name) {
    if (!regex_pattern || !func_name) return NULL;
    
    char* code = (char*)malloc(1024);
    if (!code) return NULL;
    
    char* escaped_pattern = escape_string(regex_pattern);
    
    snprintf(code, 1024,
        "// Auto-generated regex validator\n"
        "func %s_validate(input string) !ValidationResult {\n"
        "    pattern := regex.compile(\"%s\")\n"
        "    if pattern.matches(input) {\n"
        "        return ValidationResult{valid: true}\n"
        "    } else {\n"
        "        return ValidationResult{valid: false, error: \"Input does not match pattern\"}\n"
        "    }\n"
        "}\n",
        func_name, escaped_pattern);
    
    free(escaped_pattern);
    return code;
}

// HTML template support
char* generate_html_template_function(const char* html_template, const char* func_name) {
    if (!html_template || !func_name) return NULL;
    
    char* code = (char*)malloc(2048);
    if (!code) return NULL;
    
    snprintf(code, 2048,
        "// Auto-generated HTML template function\n"
        "func %s(context map[string]any) !string {\n"
        "    template_str := `%s`\n"
        "    renderer := html.new_template_renderer()\n"
        "    return try renderer.render(template_str, context)\n"
        "}\n",
        func_name, html_template);
    
    return code;
}

// Configuration DSL support
char* generate_config_parser(const char* config_schema, DSLType config_type) {
    if (!config_schema) return NULL;
    
    char* code = (char*)malloc(1024);
    if (!code) return NULL;
    
    const char* parser_type = (config_type == DSL_YAML) ? "yaml" : "toml";
    
    snprintf(code, 1024,
        "// Auto-generated %s config parser\n"
        "func parse_%s_config(data []byte) !Config {\n"
        "    parser := %s.new_parser()\n"
        "    return try parser.parse(data)\n"
        "}\n",
        parser_type, parser_type, parser_type);
    
    return code;
}

// Configuration validator generation
char* generate_config_validator(const char* config_schema, DSLType config_type) {
    if (!config_schema) return NULL;
    
    char* code = (char*)malloc(1024);
    if (!code) return NULL;
    
    const char* config_name = (config_type == DSL_YAML) ? "yaml" : "toml";
    
    snprintf(code, 1024,
        "// Auto-generated %s config validator\n"
        "func validate_%s_config(config Config) !bool {\n"
        "    // Configuration validation logic\n"
        "    return true\n"
        "}\n",
        config_name, config_name);
    
    return code;
}

// Infrastructure DSL support
char* generate_dockerfile_from_spec(const char* docker_spec) {
    if (!docker_spec) return NULL;
    
    char* code = (char*)malloc(1024);
    if (!code) return NULL;
    
    snprintf(code, 1024,
        "// Auto-generated Dockerfile builder\n"
        "func build_dockerfile(spec DockerSpec) !string {\n"
        "    builder := docker.new_file_builder()\n"
        "    return try builder.generate(spec)\n"
        "}\n");
    
    return code;
}

// Kubernetes manifest generation
char* generate_kubernetes_manifest(const char* k8s_spec) {
    if (!k8s_spec) return NULL;
    
    char* code = (char*)malloc(1024);
    if (!code) return NULL;
    
    snprintf(code, 1024,
        "// Auto-generated Kubernetes manifest builder\n"
        "func build_k8s_manifest(spec K8sSpec) !string {\n"
        "    builder := k8s.new_manifest_builder()\n"
        "    return try builder.generate(spec)\n"
        "}\n");
    
    return code;
}

// Terraform configuration generation
char* generate_terraform_config(const char* tf_spec) {
    if (!tf_spec) return NULL;
    
    char* code = (char*)malloc(1024);
    if (!code) return NULL;
    
    snprintf(code, 1024,
        "// Auto-generated Terraform configuration builder\n"
        "func build_terraform_config(spec TerraformSpec) !string {\n"
        "    builder := terraform.new_config_builder()\n"
        "    return try builder.generate(spec)\n"
        "}\n");
    
    return code;
}

// Compile-time API generation
ComptimeValue* dsl_macro_evaluator(MacroContext* ctx, ComptimeValue** args) {
    if (!ctx || !args || ctx->arg_count < 1) return NULL;
    
    // Get DSL content from first argument
    ComptimeValue* dsl_content = args[0];
    if (dsl_content->type != COMPTIME_VALUE_STRING) {
        return NULL;
    }
    
    // Determine DSL type from macro name
    DSLType dsl_type = parse_dsl_type(ctx->macro ? ctx->macro->name : "unknown");
    if (dsl_type == DSL_COUNT) {
        return NULL;
    }
    
    // Create DSL context and generate code
    DSLContext* dsl_ctx = create_dsl_context(dsl_type, dsl_content->string_value);
    if (!dsl_ctx) return NULL;
    
    // Parse and generate code
    if (parse_dsl_content(dsl_ctx)) {
        dsl_ctx->state = DSL_STATE_COMPLETE;
        char* generated = generate_code_from_dsl(dsl_ctx);
        
        ComptimeValue* result = NULL;
        if (generated) {
            result = create_comptime_string(generated);
            free(generated);
        }
        
        destroy_dsl_context(dsl_ctx);
        return result;
    }
    
    destroy_dsl_context(dsl_ctx);
    return NULL;
}

// Generate compile-time API
char* generate_compile_time_api(const char* spec_content, DSLType spec_type) {
    if (!spec_content) return NULL;
    
    DSLContext* ctx = create_dsl_context(spec_type, spec_content);
    if (!ctx) return NULL;
    
    if (parse_dsl_content(ctx)) {
        ctx->state = DSL_STATE_COMPLETE;
        char* code = generate_code_from_dsl(ctx);
        destroy_dsl_context(ctx);
        return code;
    }
    
    destroy_dsl_context(ctx);
    return NULL;
}

// DSL optimization and analysis
bool optimize_dsl_code(DSLContext* ctx) {
    if (!ctx || !ctx->generated_code) return false;
    
    // Simple optimization - remove redundant whitespace
    char* optimized = (char*)malloc(strlen(ctx->generated_code) + 1);
    if (!optimized) return false;
    
    char* dst = optimized;
    char* src = ctx->generated_code;
    bool in_whitespace = false;
    
    while (*src) {
        if (isspace(*src)) {
            if (!in_whitespace) {
                *dst++ = ' ';
                in_whitespace = true;
            }
        } else {
            *dst++ = *src;
            in_whitespace = false;
        }
        src++;
    }
    *dst = '\0';
    
    free(ctx->generated_code);
    ctx->generated_code = optimized;
    
    return true;
}

// Analyze DSL dependencies
char** analyze_dsl_dependencies(DSLContext* ctx, size_t* dep_count) {
    if (!ctx || !dep_count) return NULL;
    
    *dep_count = 0;
    
    // Simple dependency analysis based on DSL type
    char** deps = (char**)malloc(5 * sizeof(char*));
    if (!deps) return NULL;
    
    switch (ctx->type) {
        case DSL_SQL:
            deps[(*dep_count)++] = strdup("database");
            break;
        case DSL_HTTP:
            deps[(*dep_count)++] = strdup("http");
            deps[(*dep_count)++] = strdup("json");
            break;
        case DSL_JSON:
            deps[(*dep_count)++] = strdup("json");
            break;
        case DSL_REGEX:
            deps[(*dep_count)++] = strdup("regex");
            break;
        case DSL_HTML:
            deps[(*dep_count)++] = strdup("html");
            break;
        default:
            break;
    }
    
    return deps;
}

// Validate DSL semantics
bool validate_dsl_semantics(DSLContext* ctx) {
    if (!ctx) return false;
    
    // Basic semantic validation
    return ctx->state == DSL_STATE_COMPLETE && ctx->generated_code != NULL;
}

// Code generation utilities
char* generate_function_wrapper(const char* func_name, const char* params, 
                               const char* return_type, const char* body) {
    if (!func_name || !params || !return_type || !body) return NULL;
    
    char* code = (char*)malloc(strlen(func_name) + strlen(params) + 
                              strlen(return_type) + strlen(body) + 100);
    if (!code) return NULL;
    
    sprintf(code, "func %s(%s) %s {\n%s\n}\n", func_name, params, return_type, body);
    return code;
}

// Generate struct definition
char* generate_struct_definition(const char* struct_name, char** fields, size_t field_count) {
    if (!struct_name || !fields) return NULL;
    
    char* code = (char*)malloc(2048);
    if (!code) return NULL;
    
    sprintf(code, "struct %s {\n", struct_name);
    
    for (size_t i = 0; i < field_count; i++) {
        strncat(code, "    ", 2048 - strlen(code) - 1);
        strncat(code, fields[i], 2048 - strlen(code) - 1);
        strncat(code, "\n", 2048 - strlen(code) - 1);
    }
    
    strncat(code, "}\n", 2048 - strlen(code) - 1);
    return code;
}

// Generate interface implementation
char* generate_interface_implementation(const char* interface_name, char** methods, size_t method_count) {
    if (!interface_name || !methods) return NULL;
    
    char* code = (char*)malloc(2048);
    if (!code) return NULL;
    
    sprintf(code, "// Implementation of %s interface\n\n", interface_name);
    
    for (size_t i = 0; i < method_count; i++) {
        strncat(code, methods[i], 2048 - strlen(code) - 1);
        strncat(code, "\n\n", 2048 - strlen(code) - 1);
    }
    
    return code;
}

// Error handling
void dsl_error(DSLContext* ctx, const char* format, ...) {
    if (!ctx || !format) return;
    
    va_list args;
    va_start(args, format);
    
    ctx->state = DSL_STATE_ERROR;
    free(ctx->error_message);
    ctx->error_message = (char*)malloc(1024);
    if (ctx->error_message) {
        vsnprintf(ctx->error_message, 1024, format, args);
    }
    
    va_end(args);
    
    fprintf(stderr, "DSL Error: %s\n", ctx->error_message);
}

// Print DSL parsing trace
void print_dsl_parsing_trace(DSLContext* ctx) {
    if (!ctx) return;
    
    printf("DSL Parsing Trace:\n");
    printf("  Type: %s\n", dsl_type_to_string(ctx->type));
    printf("  State: %d\n", ctx->state);
    printf("  Content Length: %zu\n", ctx->dsl_content ? strlen(ctx->dsl_content) : 0);
    printf("  Generated Code Length: %zu\n", ctx->generated_code ? strlen(ctx->generated_code) : 0);
    
    if (ctx->error_message) {
        printf("  Error: %s\n", ctx->error_message);
    }
}

// Get DSL error context
char* get_dsl_error_context(DSLContext* ctx, int line, int column) {
    if (!ctx || !ctx->dsl_content) return NULL;
    
    char* context = (char*)malloc(512);
    if (!context) return NULL;
    
    snprintf(context, 512, "Error at line %d, column %d in %s DSL",
             line, column, dsl_type_to_string(ctx->type));
    
    return context;
}

// Utility functions
DSLType parse_dsl_type(const char* type_name) {
    if (!type_name) return DSL_COUNT;
    
    for (int i = 0; i < DSL_COUNT; i++) {
        if (strcmp(dsl_type_names[i], type_name) == 0) {
            return (DSLType)i;
        }
    }
    return DSL_COUNT;
}

// DSL type to string
const char* dsl_type_to_string(DSLType type) {
    if (type >= DSL_COUNT) return "unknown";
    return dsl_type_names[type];
}

// Check if DSL type is supported
bool is_dsl_type_supported(DSLType type) {
    return type < DSL_COUNT;
}

// Debug and introspection
void print_dsl_context(DSLContext* ctx) {
    if (!ctx) return;
    
    printf("DSL Context:\n");
    printf("  Type: %s\n", dsl_type_to_string(ctx->type));
    printf("  State: %d\n", ctx->state);
    printf("  Strict Mode: %s\n", ctx->strict_mode ? "Yes" : "No");
    printf("  Generate Validators: %s\n", ctx->generate_validators ? "Yes" : "No");
    printf("  Optimize Queries: %s\n", ctx->optimize_queries ? "Yes" : "No");
    
    if (ctx->target_namespace) {
        printf("  Target Namespace: %s\n", ctx->target_namespace);
    }
    
    if (ctx->error_message) {
        printf("  Error: %s\n", ctx->error_message);
    }
    
    printf("  Dependencies: %zu\n", ctx->dependency_count);
    printf("  Exports: %zu\n", ctx->export_count);
}

// Get DSL info
char* get_dsl_info(DSLContext* ctx) {
    if (!ctx) return NULL;
    
    char* info = (char*)malloc(1024);
    if (!info) return NULL;
    
    snprintf(info, 1024,
        "DSL Info:\n"
        "  Type: %s\n"
        "  State: %d\n"
        "  Content Size: %zu bytes\n"
        "  Generated Code Size: %zu bytes\n"
        "  Dependencies: %zu\n"
        "  Exports: %zu",
        dsl_type_to_string(ctx->type),
        ctx->state,
        ctx->dsl_content ? strlen(ctx->dsl_content) : 0,
        ctx->generated_code ? strlen(ctx->generated_code) : 0,
        ctx->dependency_count,
        ctx->export_count);
    
    return info;
}

// Debug DSL generation
void debug_dsl_generation(DSLContext* ctx) {
    if (!ctx) return;
    
    printf("=== DSL Generation Debug ===\n");
    print_dsl_context(ctx);
    
    if (ctx->dsl_content) {
        printf("\nDSL Content (first 200 chars):\n%.200s%s\n",
               ctx->dsl_content,
               strlen(ctx->dsl_content) > 200 ? "..." : "");
    }
    
    if (ctx->generated_code) {
        printf("\nGenerated Code (first 300 chars):\n%.300s%s\n",
               ctx->generated_code,
               strlen(ctx->generated_code) > 300 ? "..." : "");
    }
    
    printf("===========================\n");
}

// Helper parsing functions for different DSL types
static bool parse_sql_content(DSLContext* ctx) {
    if (!validate_dsl_syntax(ctx)) return false;
    
    // SQL-specific parsing logic
    SQLQuery* query = parse_sql_query(ctx->dsl_content);
    if (query) {
        ctx->generated_code = generate_sql_function(query, "generated_query");
        destroy_sql_query(query);
        ctx->state = DSL_STATE_COMPLETE;
        return true;
    }
    
    return false;
}

static bool parse_http_content(DSLContext* ctx) {
    if (!validate_dsl_syntax(ctx)) return false;
    
    // HTTP-specific parsing logic
    HTTPEndpoint* endpoint = parse_http_endpoint(ctx->dsl_content);
    if (endpoint) {
        ctx->generated_code = generate_http_client_function(endpoint);
        destroy_http_endpoint(endpoint);
        ctx->state = DSL_STATE_COMPLETE;
        return true;
    }
    
    return false;
}

static bool parse_json_content(DSLContext* ctx) {
    if (!validate_dsl_syntax(ctx)) return false;
    
    // JSON-specific parsing logic
    ctx->generated_code = generate_json_parser(ctx->dsl_content);
    ctx->state = DSL_STATE_COMPLETE;
    return ctx->generated_code != NULL;
}

static bool parse_regex_content(DSLContext* ctx) {
    if (!validate_dsl_syntax(ctx)) return false;
    
    // Regex-specific parsing logic
    ctx->generated_code = generate_regex_matcher(ctx->dsl_content, "generated_matcher");
    ctx->state = DSL_STATE_COMPLETE;
    return ctx->generated_code != NULL;
}

static bool parse_html_content(DSLContext* ctx) {
    if (!validate_dsl_syntax(ctx)) return false;
    
    // HTML-specific parsing logic
    ctx->generated_code = generate_html_template_function(ctx->dsl_content, "generated_template");
    ctx->state = DSL_STATE_COMPLETE;
    return ctx->generated_code != NULL;
}

static bool parse_openapi_content(DSLContext* ctx) {
    if (!validate_dsl_syntax(ctx)) return false;
    
    // OpenAPI-specific parsing logic
    OpenAPISpec* spec = parse_openapi_spec(ctx->dsl_content);
    if (spec) {
        ctx->generated_code = generate_api_client_from_spec(spec);
        destroy_openapi_spec(spec);
        ctx->state = DSL_STATE_COMPLETE;
        return true;
    }
    
    return false;
}

static bool parse_config_content(DSLContext* ctx) {
    if (!validate_dsl_syntax(ctx)) return false;
    
    // Configuration-specific parsing logic
    ctx->generated_code = generate_config_parser(ctx->dsl_content, ctx->type);
    ctx->state = DSL_STATE_COMPLETE;
    return ctx->generated_code != NULL;
}

static bool parse_infrastructure_content(DSLContext* ctx) {
    if (!validate_dsl_syntax(ctx)) return false;
    
    // Infrastructure-specific parsing logic
    switch (ctx->type) {
        case DSL_DOCKERFILE:
            ctx->generated_code = generate_dockerfile_from_spec(ctx->dsl_content);
            break;
        case DSL_KUBERNETES:
            ctx->generated_code = generate_kubernetes_manifest(ctx->dsl_content);
            break;
        case DSL_TERRAFORM:
            ctx->generated_code = generate_terraform_config(ctx->dsl_content);
            break;
        default:
            return false;
    }
    
    ctx->state = DSL_STATE_COMPLETE;
    return ctx->generated_code != NULL;
}

// Code generation helpers for different DSL types
static char* generate_sql_code(DSLContext* ctx) {
    return strdup(ctx->generated_code);
}

static char* generate_http_code(DSLContext* ctx) {
    return strdup(ctx->generated_code);
}

static char* generate_json_code(DSLContext* ctx) {
    return strdup(ctx->generated_code);
}

static char* generate_regex_code(DSLContext* ctx) {
    return strdup(ctx->generated_code);
}

static char* generate_html_code(DSLContext* ctx) {
    return strdup(ctx->generated_code);
}

static char* generate_openapi_code(DSLContext* ctx) {
    return strdup(ctx->generated_code);
}

static char* generate_config_code(DSLContext* ctx) {
    return strdup(ctx->generated_code);
}

static char* generate_infrastructure_code(DSLContext* ctx) {
    return strdup(ctx->generated_code);
}

