#include "../include/dsl_macros.h"
#include "../include/advanced_macro_system.h"
#include "../include/comptime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Test helper functions
void test_assert(bool condition, const char* test_name) {
    if (condition) {
        printf("✓ %s\n", test_name);
    } else {
        printf("✗ %s\n", test_name);
        exit(1);
    }
}

// Test DSL context creation and destruction
void test_dsl_context_creation() {
    const char* sql_content = "SELECT * FROM users WHERE id = 1";
    DSLContext* ctx = create_dsl_context(DSL_SQL, sql_content);
    
    test_assert(ctx != NULL, "DSL context creation");
    test_assert(ctx->type == DSL_SQL, "DSL type set correctly");
    test_assert(strcmp(ctx->dsl_content, sql_content) == 0, "DSL content copied");
    test_assert(ctx->state == DSL_STATE_INITIAL, "Initial state set");
    test_assert(ctx->strict_mode == true, "Strict mode enabled by default");
    
    destroy_dsl_context(ctx);
}

// Test SQL DSL parsing
void test_sql_dsl_parsing() {
    const char* sql_content = "SELECT name, email FROM users WHERE active = true";
    SQLQuery* query = parse_sql_query(sql_content);
    
    test_assert(query != NULL, "SQL query parsing");
    if (query) {
        printf("Debug: table_name = %s\n", query->table_name ? query->table_name : "NULL");
        test_assert(query->table_name != NULL, "SQL table name extracted");
        if (query->table_name) {
            test_assert(strcmp(query->table_name, "users") == 0, "Correct table name");
        }
        test_assert(query->column_count >= 1, "SQL columns extracted");
        destroy_sql_query(query);
    }
}

// Test SQL function generation
void test_sql_function_generation() {
    const char* sql_content = "SELECT * FROM products WHERE category = 'electronics'";
    SQLQuery* query = parse_sql_query(sql_content);
    
    char* generated = generate_sql_function(query, "get_electronics");
    test_assert(generated != NULL, "SQL function generation");
    test_assert(strstr(generated, "func get_electronics") != NULL, "Function name in output");
    test_assert(strstr(generated, "Database") != NULL, "Database parameter in output");
    test_assert(strstr(generated, "products") != NULL, "Table name in query");
    
    free(generated);
    destroy_sql_query(query);
}

// Test HTTP endpoint parsing
void test_http_endpoint_parsing() {
    const char* http_content = "GET /api/users/{id} -> User";
    HTTPEndpoint* endpoint = parse_http_endpoint(http_content);
    
    test_assert(endpoint != NULL, "HTTP endpoint parsing");
    test_assert(strcmp(endpoint->method, "GET") == 0, "HTTP method extracted");
    test_assert(strcmp(endpoint->path, "/api/users/{id}") == 0, "HTTP path extracted");
    test_assert(endpoint->response_type != NULL, "Response type extracted");
    test_assert(strcmp(endpoint->response_type, "User") == 0, "Correct response type");
    
    destroy_http_endpoint(endpoint);
}

// Test HTTP client generation
void test_http_client_generation() {
    const char* http_content = "POST /api/orders -> Order";
    HTTPEndpoint* endpoint = parse_http_endpoint(http_content);
    
    char* generated = generate_http_client_function(endpoint);
    test_assert(generated != NULL, "HTTP client generation");
    test_assert(strstr(generated, "func post_") != NULL, "Function name contains method");
    test_assert(strstr(generated, "HTTPClient") != NULL, "Client parameter in output");
    test_assert(strstr(generated, "/api/orders") != NULL, "Path in generated code");
    test_assert(strstr(generated, "POST") != NULL, "Method in generated code");
    
    free(generated);
    destroy_http_endpoint(endpoint);
}

// Test OpenAPI specification parsing
void test_openapi_parsing() {
    const char* openapi_content = 
        "title: Pet Store API\n"
        "version: 1.0.0\n"
        "paths:\n"
        "  /pets:\n"
        "    get:\n"
        "      responses:\n"
        "        200:\n"
        "          description: List of pets";
    
    OpenAPISpec* spec = parse_openapi_spec(openapi_content);
    test_assert(spec != NULL, "OpenAPI spec parsing");
    test_assert(spec->title != NULL, "API title extracted");
    test_assert(strcmp(spec->title, "Pet Store API") == 0, "Correct API title");
    test_assert(spec->version != NULL, "API version extracted");
    test_assert(strcmp(spec->version, "1.0.0") == 0, "Correct API version");
    
    destroy_openapi_spec(spec);
}

// Test OpenAPI client generation
void test_openapi_client_generation() {
    const char* openapi_content = 
        "title: User Service\n"
        "version: 2.1.0";
    
    OpenAPISpec* spec = parse_openapi_spec(openapi_content);
    char* generated = generate_api_client_from_spec(spec);
    
    test_assert(generated != NULL, "OpenAPI client generation");
    test_assert(strstr(generated, "UserServiceClient") != NULL, "Client struct name");
    test_assert(strstr(generated, "new_userservice_client") != NULL, "Constructor function");
    test_assert(strstr(generated, "v2.1.0") != NULL, "Version in comment");
    test_assert(strstr(generated, "base_url") != NULL, "Base URL field");
    test_assert(strstr(generated, "api_key") != NULL, "API key field");
    
    free(generated);
    destroy_openapi_spec(spec);
}

// Test JSON parser generation
void test_json_parser_generation() {
    const char* schema = "{\"type\": \"object\", \"properties\": {\"name\": {\"type\": \"string\"}}}";
    char* generated = generate_json_parser(schema);
    
    test_assert(generated != NULL, "JSON parser generation");
    test_assert(strstr(generated, "parse_json") != NULL, "Parser function name");
    test_assert(strstr(generated, "JSONValue") != NULL, "JSON value type");
    test_assert(strstr(generated, "json.new_parser") != NULL, "Parser creation");
    
    free(generated);
}

// Test JSON serializer generation
void test_json_serializer_generation() {
    const char* schema = "{\"type\": \"object\"}";
    char* generated = generate_json_serializer(schema);
    
    test_assert(generated != NULL, "JSON serializer generation");
    test_assert(strstr(generated, "serialize_to_json") != NULL, "Serializer function name");
    test_assert(strstr(generated, "json.new_serializer") != NULL, "Serializer creation");
    
    free(generated);
}

// Test regex matcher generation
void test_regex_matcher_generation() {
    const char* pattern = "[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}";
    char* generated = generate_regex_matcher(pattern, "validate_email");
    
    test_assert(generated != NULL, "Regex matcher generation");
    test_assert(strstr(generated, "func validate_email") != NULL, "Function name");
    test_assert(strstr(generated, "regex.compile") != NULL, "Regex compilation");
    test_assert(strstr(generated, "pattern.matches") != NULL, "Pattern matching");
    test_assert(strstr(generated, "validate_email_find_all") != NULL, "Find all function");
    
    free(generated);
}

// Test regex validator generation
void test_regex_validator_generation() {
    const char* pattern = "^[0-9]{3}-[0-9]{2}-[0-9]{4}$";
    char* generated = generate_regex_validator(pattern, "ssn");
    
    test_assert(generated != NULL, "Regex validator generation");
    test_assert(strstr(generated, "func ssn_validate") != NULL, "Validator function name");
    test_assert(strstr(generated, "ValidationResult") != NULL, "Validation result type");
    test_assert(strstr(generated, "valid: true") != NULL, "Success case");
    test_assert(strstr(generated, "valid: false") != NULL, "Failure case");
    
    free(generated);
}

// Test HTML template generation
void test_html_template_generation() {
    const char* template = "<h1>{{title}}</h1><p>{{content}}</p>";
    char* generated = generate_html_template_function(template, "render_page");
    
    test_assert(generated != NULL, "HTML template generation");
    test_assert(strstr(generated, "func render_page") != NULL, "Template function name");
    test_assert(strstr(generated, "map[string]any") != NULL, "Context parameter type");
    test_assert(strstr(generated, "html.new_template_renderer") != NULL, "Renderer creation");
    test_assert(strstr(generated, template) != NULL, "Template content included");
    
    free(generated);
}

// Test config parser generation
void test_config_parser_generation() {
    const char* schema = "server:\n  host: string\n  port: int";
    char* yaml_generated = generate_config_parser(schema, DSL_YAML);
    char* toml_generated = generate_config_parser(schema, DSL_TOML);
    
    test_assert(yaml_generated != NULL, "YAML config parser generation");
    test_assert(strstr(yaml_generated, "parse_yaml_config") != NULL, "YAML parser function");
    test_assert(strstr(yaml_generated, "yaml.new_parser") != NULL, "YAML parser creation");
    
    test_assert(toml_generated != NULL, "TOML config parser generation");
    test_assert(strstr(toml_generated, "parse_toml_config") != NULL, "TOML parser function");
    test_assert(strstr(toml_generated, "toml.new_parser") != NULL, "TOML parser creation");
    
    free(yaml_generated);
    free(toml_generated);
}

// Test infrastructure DSL generation
void test_infrastructure_generation() {
    const char* docker_spec = "FROM node:16\nCOPY . /app\nWORKDIR /app\nRUN npm install";
    char* docker_generated = generate_dockerfile_from_spec(docker_spec);
    
    test_assert(docker_generated != NULL, "Dockerfile generation");
    test_assert(strstr(docker_generated, "build_dockerfile") != NULL, "Docker function name");
    test_assert(strstr(docker_generated, "DockerSpec") != NULL, "Docker spec type");
    
    const char* k8s_spec = "apiVersion: v1\nkind: Pod";
    char* k8s_generated = generate_kubernetes_manifest(k8s_spec);
    
    test_assert(k8s_generated != NULL, "Kubernetes generation");
    test_assert(strstr(k8s_generated, "build_k8s_manifest") != NULL, "K8s function name");
    test_assert(strstr(k8s_generated, "K8sSpec") != NULL, "K8s spec type");
    
    const char* tf_spec = "resource \"aws_instance\" \"web\" {}";
    char* tf_generated = generate_terraform_config(tf_spec);
    
    test_assert(tf_generated != NULL, "Terraform generation");
    test_assert(strstr(tf_generated, "build_terraform_config") != NULL, "Terraform function name");
    test_assert(strstr(tf_generated, "TerraformSpec") != NULL, "Terraform spec type");
    
    free(docker_generated);
    free(k8s_generated);
    free(tf_generated);
}

// Test DSL type parsing
void test_dsl_type_parsing() {
    test_assert(parse_dsl_type("sql") == DSL_SQL, "SQL type parsing");
    test_assert(parse_dsl_type("http") == DSL_HTTP, "HTTP type parsing");
    test_assert(parse_dsl_type("json") == DSL_JSON, "JSON type parsing");
    test_assert(parse_dsl_type("regex") == DSL_REGEX, "Regex type parsing");
    test_assert(parse_dsl_type("openapi") == DSL_OPENAPI, "OpenAPI type parsing");
    test_assert(parse_dsl_type("invalid") == DSL_COUNT, "Invalid type parsing");
    
    test_assert(strcmp(dsl_type_to_string(DSL_SQL), "sql") == 0, "SQL type to string");
    test_assert(strcmp(dsl_type_to_string(DSL_HTTP), "http") == 0, "HTTP type to string");
    test_assert(strcmp(dsl_type_to_string(DSL_COUNT), "unknown") == 0, "Invalid type to string");
}

// Test DSL syntax validation
void test_dsl_syntax_validation() {
    DSLContext* valid_ctx = create_dsl_context(DSL_JSON, "{\"key\": \"value\"}");
    test_assert(validate_dsl_syntax(valid_ctx), "Valid JSON syntax");
    destroy_dsl_context(valid_ctx);
    
    DSLContext* invalid_ctx = create_dsl_context(DSL_JSON, "{\"key\": \"value\"");
    test_assert(!validate_dsl_syntax(invalid_ctx), "Invalid JSON syntax detected");
    destroy_dsl_context(invalid_ctx);
    
    DSLContext* balanced_ctx = create_dsl_context(DSL_SQL, "SELECT * FROM users WHERE (active = true)");
    test_assert(validate_dsl_syntax(balanced_ctx), "Balanced parentheses");
    destroy_dsl_context(balanced_ctx);
}

// Test complete DSL workflow
void test_complete_dsl_workflow() {
    const char* sql_content = "SELECT id, name FROM customers WHERE region = 'US'";
    DSLContext* ctx = create_dsl_context(DSL_SQL, sql_content);
    
    test_assert(parse_dsl_content(ctx), "DSL content parsing");
    test_assert(ctx->state == DSL_STATE_COMPLETE, "DSL parsing completed");
    
    char* generated = generate_code_from_dsl(ctx);
    test_assert(generated != NULL, "Code generation from DSL");
    test_assert(strstr(generated, "customers") != NULL, "Table name in generated code");
    
    free(generated);
    destroy_dsl_context(ctx);
}

// Test code generation utilities
void test_code_generation_utilities() {
    char* func_wrapper = generate_function_wrapper(
        "test_func", "param1 int, param2 string", "!Result", 
        "    return Result{success: true}"
    );
    
    test_assert(func_wrapper != NULL, "Function wrapper generation");
    test_assert(strstr(func_wrapper, "func test_func") != NULL, "Function declaration");
    test_assert(strstr(func_wrapper, "param1 int") != NULL, "Parameters included");
    test_assert(strstr(func_wrapper, "!Result") != NULL, "Return type included");
    test_assert(strstr(func_wrapper, "return Result") != NULL, "Body included");
    
    free(func_wrapper);
    
    char* fields[] = {"id: int", "name: string", "active: bool"};
    char* struct_def = generate_struct_definition("User", fields, 3);
    
    test_assert(struct_def != NULL, "Struct definition generation");
    test_assert(strstr(struct_def, "struct User") != NULL, "Struct declaration");
    test_assert(strstr(struct_def, "id: int") != NULL, "Field included");
    test_assert(strstr(struct_def, "name: string") != NULL, "Second field included");
    test_assert(strstr(struct_def, "active: bool") != NULL, "Third field included");
    
    free(struct_def);
}

// Test error handling
void test_error_handling() {
    DSLContext* ctx = create_dsl_context(DSL_SQL, "SELECT * FROM");
    
    // Test error reporting
    dsl_error(ctx, "Test error: %s", "invalid SQL");
    test_assert(ctx->state == DSL_STATE_ERROR, "Error state set");
    test_assert(ctx->error_message != NULL, "Error message set");
    test_assert(strstr(ctx->error_message, "Test error") != NULL, "Error message content");
    
    char* error_context = get_dsl_error_context(ctx, 1, 10);
    test_assert(error_context != NULL, "Error context generation");
    test_assert(strstr(error_context, "line 1") != NULL, "Line number in context");
    test_assert(strstr(error_context, "column 10") != NULL, "Column number in context");
    test_assert(strstr(error_context, "sql DSL") != NULL, "DSL type in context");
    
    free(error_context);
    destroy_dsl_context(ctx);
}

// Test DSL macro evaluator
void test_dsl_macro_evaluator() {
    // Create a mock macro context with proper macro template
    MacroTemplate* sql_macro_template = (MacroTemplate*)calloc(1, sizeof(MacroTemplate));
    sql_macro_template->name = strdup("sql");
    sql_macro_template->type = MACRO_DSL;
    
    MacroContext* macro_ctx = (MacroContext*)calloc(1, sizeof(MacroContext));
    macro_ctx->macro = sql_macro_template;
    macro_ctx->arg_count = 1;
    
    // Create argument
    ComptimeValue* sql_arg = create_comptime_string("SELECT * FROM users");
    ComptimeValue* args[] = {sql_arg};
    
    ComptimeValue* result = dsl_macro_evaluator(macro_ctx, args);
    test_assert(result != NULL, "DSL macro evaluator");
    test_assert(result->type == COMPTIME_VALUE_STRING, "Result is string");
    test_assert(strstr(result->string_value, "func") != NULL, "Generated function code");
    
    comptime_value_free(sql_arg);
    comptime_value_free(result);
    free(sql_macro_template->name);
    free(sql_macro_template);
    free(macro_ctx);
}

// Test DSL registry
void test_dsl_registry() {
    MacroRegistry* registry = create_macro_registry();
    test_assert(register_dsl_macros(registry), "DSL macro registration");
    
    MacroTemplate* sql_macro = find_macro(registry, "sql");
    test_assert(sql_macro != NULL, "SQL macro found in registry");
    test_assert(sql_macro->type == MACRO_DSL, "SQL macro type correct");
    
    MacroTemplate* http_macro = find_macro(registry, "http");
    test_assert(http_macro != NULL, "HTTP macro found in registry");
    
    MacroTemplate* json_macro = find_macro(registry, "json");
    test_assert(json_macro != NULL, "JSON macro found in registry");
    
    MacroTemplate* openapi_macro = find_macro(registry, "openapi");
    test_assert(openapi_macro != NULL, "OpenAPI macro found in registry");
    
    destroy_macro_registry(registry);
}

int main() {
    printf("Running DSL Macro Tests...\n\n");
    
    test_dsl_context_creation();
    test_sql_dsl_parsing();
    test_sql_function_generation();
    test_http_endpoint_parsing();
    test_http_client_generation();
    test_openapi_parsing();
    test_openapi_client_generation();
    test_json_parser_generation();
    test_json_serializer_generation();
    test_regex_matcher_generation();
    test_regex_validator_generation();
    test_html_template_generation();
    test_config_parser_generation();
    test_infrastructure_generation();
    test_dsl_type_parsing();
    test_dsl_syntax_validation();
    test_complete_dsl_workflow();
    test_code_generation_utilities();
    test_error_handling();
    test_dsl_macro_evaluator();
    test_dsl_registry();
    
    printf("\n✓ All DSL macro tests passed!\n");
    return 0;
}