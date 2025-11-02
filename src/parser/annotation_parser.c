#include "error_recovery.h"
#include "ast.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

// =============================================================================
// Annotation Parser Implementation
// =============================================================================

// Helper functions for parsing
static void skip_whitespace(AnnotationParser* parser);
static bool consume_token(AnnotationParser* parser, const char* token);
static bool consume_char(AnnotationParser* parser, char ch);
static char* parse_string_literal(AnnotationParser* parser);
static long parse_integer(AnnotationParser* parser);
static double parse_double(AnnotationParser* parser);
static bool parse_boolean(AnnotationParser* parser);
static char* parse_identifier(AnnotationParser* parser);

// =============================================================================
// Parser Lifecycle
// =============================================================================

AnnotationParser* annotation_parser_new(const char* source_text, size_t length) {
    if (!source_text) return NULL;
    
    AnnotationParser* parser = calloc(1, sizeof(AnnotationParser));
    if (!parser) return NULL;
    
    parser->source_text = source_text;
    parser->length = length;
    parser->position = 0;
    parser->error_message = NULL;
    
    return parser;
}

void annotation_parser_free(AnnotationParser* parser) {
    if (!parser) return;
    
    free(parser->error_message);
    free(parser);
}

// =============================================================================
// Parsing Utilities
// =============================================================================

static void skip_whitespace(AnnotationParser* parser) {
    while (parser->position < parser->length && 
           isspace(parser->source_text[parser->position])) {
        parser->position++;
    }
}

static bool consume_token(AnnotationParser* parser, const char* token) {
    skip_whitespace(parser);
    
    size_t token_len = strlen(token);
    if (parser->position + token_len > parser->length) {
        return false;
    }
    
    if (strncmp(&parser->source_text[parser->position], token, token_len) == 0) {
        parser->position += token_len;
        return true;
    }
    
    return false;
}

static bool consume_char(AnnotationParser* parser, char ch) {
    skip_whitespace(parser);
    
    if (parser->position < parser->length && 
        parser->source_text[parser->position] == ch) {
        parser->position++;
        return true;
    }
    
    return false;
}

static char* parse_string_literal(AnnotationParser* parser) {
    skip_whitespace(parser);
    
    if (parser->position >= parser->length || 
        parser->source_text[parser->position] != '"') {
        return NULL;
    }
    
    parser->position++; // Skip opening quote
    size_t start = parser->position;
    
    // Find closing quote
    while (parser->position < parser->length) {
        if (parser->source_text[parser->position] == '"') {
            // Found closing quote
            size_t length = parser->position - start;
            char* result = malloc(length + 1);
            if (result) {
                strncpy(result, &parser->source_text[start], length);
                result[length] = '\0';
            }
            parser->position++; // Skip closing quote
            return result;
        } else if (parser->source_text[parser->position] == '\\') {
            // Skip escaped character
            parser->position += 2;
        } else {
            parser->position++;
        }
    }
    
    // Unterminated string
    return NULL;
}

static long parse_integer(AnnotationParser* parser) {
    skip_whitespace(parser);
    
    size_t start = parser->position;
    
    // Handle negative numbers
    if (parser->position < parser->length && 
        parser->source_text[parser->position] == '-') {
        parser->position++;
    }
    
    // Parse digits
    while (parser->position < parser->length && 
           isdigit(parser->source_text[parser->position])) {
        parser->position++;
    }
    
    if (parser->position == start || 
        (parser->position == start + 1 && parser->source_text[start] == '-')) {
        return 0; // No digits found
    }
    
    // Extract number string
    size_t length = parser->position - start;
    char* num_str = malloc(length + 1);
    if (!num_str) return 0;
    
    strncpy(num_str, &parser->source_text[start], length);
    num_str[length] = '\0';
    
    long result = strtol(num_str, NULL, 10);
    free(num_str);
    
    return result;
}

static double parse_double(AnnotationParser* parser) {
    skip_whitespace(parser);
    
    size_t start = parser->position;
    
    // Handle negative numbers
    if (parser->position < parser->length && 
        parser->source_text[parser->position] == '-') {
        parser->position++;
    }
    
    // Parse digits before decimal point
    while (parser->position < parser->length && 
           isdigit(parser->source_text[parser->position])) {
        parser->position++;
    }
    
    // Parse decimal point and digits after
    if (parser->position < parser->length && 
        parser->source_text[parser->position] == '.') {
        parser->position++;
        while (parser->position < parser->length && 
               isdigit(parser->source_text[parser->position])) {
            parser->position++;
        }
    }
    
    if (parser->position == start || 
        (parser->position == start + 1 && parser->source_text[start] == '-')) {
        return 0.0; // No digits found
    }
    
    // Extract number string
    size_t length = parser->position - start;
    char* num_str = malloc(length + 1);
    if (!num_str) return 0.0;
    
    strncpy(num_str, &parser->source_text[start], length);
    num_str[length] = '\0';
    
    double result = strtod(num_str, NULL);
    free(num_str);
    
    return result;
}

static bool parse_boolean(AnnotationParser* parser) {
    if (consume_token(parser, "true")) {
        return true;
    } else if (consume_token(parser, "false")) {
        return false;
    }
    
    return false; // Default to false if not found
}

static char* parse_identifier(AnnotationParser* parser) {
    skip_whitespace(parser);
    
    size_t start = parser->position;
    
    // First character must be letter or underscore
    if (parser->position >= parser->length || 
        (!isalpha(parser->source_text[parser->position]) && 
         parser->source_text[parser->position] != '_')) {
        return NULL;
    }
    
    // Rest can be letters, digits, or underscores
    while (parser->position < parser->length) {
        char ch = parser->source_text[parser->position];
        if (isalnum(ch) || ch == '_') {
            parser->position++;
        } else {
            break;
        }
    }
    
    size_t length = parser->position - start;
    char* result = malloc(length + 1);
    if (result) {
        strncpy(result, &parser->source_text[start], length);
        result[length] = '\0';
    }
    
    return result;
}

// =============================================================================
// Retry Annotation Parsing
// =============================================================================

RetryAnnotation* parse_retry_annotation(AnnotationParser* parser) {
    if (!parser) return NULL;
    
    // Expect @retry
    if (!consume_token(parser, "@retry")) {
        return NULL;
    }
    
    RetryAnnotation* retry = retry_annotation_create_default();
    if (!retry) return NULL;
    
    // Check for parameter list
    if (!consume_char(parser, '(')) {
        // No parameters, use defaults
        return retry;
    }
    
    // Parse parameters
    while (parser->position < parser->length) {
        skip_whitespace(parser);
        
        // Check for end of parameters
        if (consume_char(parser, ')')) {
            break;
        }
        
        // Parse parameter name
        char* param_name = parse_identifier(parser);
        if (!param_name) {
            parser->error_message = strdup("Expected parameter name");
            retry_annotation_free(retry);
            return NULL;
        }
        
        // Expect '='
        if (!consume_char(parser, '=')) {
            parser->error_message = strdup("Expected '=' after parameter name");
            free(param_name);
            retry_annotation_free(retry);
            return NULL;
        }
        
        // Parse parameter value based on name
        if (strcmp(param_name, "max_attempts") == 0) {
            retry->max_attempts = (int)parse_integer(parser);
        } else if (strcmp(param_name, "backoff") == 0) {
            char* backoff_str = parse_string_literal(parser);
            if (backoff_str) {
                if (strcmp(backoff_str, "constant") == 0) {
                    retry->backoff.strategy = BACKOFF_CONSTANT;
                } else if (strcmp(backoff_str, "linear") == 0) {
                    retry->backoff.strategy = BACKOFF_LINEAR;
                } else if (strcmp(backoff_str, "exponential") == 0) {
                    retry->backoff.strategy = BACKOFF_EXPONENTIAL;
                } else if (strcmp(backoff_str, "jittered") == 0) {
                    retry->backoff.strategy = BACKOFF_JITTERED;
                }
                free(backoff_str);
            }
        } else if (strcmp(param_name, "initial_delay_ms") == 0) {
            retry->backoff.initial_delay_ms = (uint64_t)parse_integer(parser);
        } else if (strcmp(param_name, "max_delay_ms") == 0) {
            retry->backoff.max_delay_ms = (uint64_t)parse_integer(parser);
        } else if (strcmp(param_name, "multiplier") == 0) {
            retry->backoff.multiplier = parse_double(parser);
        } else if (strcmp(param_name, "jitter_factor") == 0) {
            retry->backoff.jitter_factor = parse_double(parser);
        } else if (strcmp(param_name, "total_timeout_ms") == 0) {
            retry->total_timeout_ms = (uint64_t)parse_integer(parser);
        } else if (strcmp(param_name, "per_attempt_timeout_ms") == 0) {
            retry->per_attempt_timeout_ms = (uint64_t)parse_integer(parser);
        } else if (strcmp(param_name, "reset_on_success") == 0) {
            retry->backoff.reset_on_success = parse_boolean(parser);
        }
        
        free(param_name);
        
        // Check for comma or end
        skip_whitespace(parser);
        if (parser->position < parser->length && 
            parser->source_text[parser->position] == ',') {
            parser->position++;
        }
    }
    
    return retry;
}

// =============================================================================
// Circuit Breaker Annotation Parsing
// =============================================================================

CircuitBreakerAnnotation* parse_circuit_breaker_annotation(AnnotationParser* parser) {
    if (!parser) return NULL;
    
    // Expect @circuit_breaker
    if (!consume_token(parser, "@circuit_breaker")) {
        return NULL;
    }
    
    CircuitBreakerAnnotation* cb = circuit_breaker_annotation_create_default();
    if (!cb) return NULL;
    
    // Check for parameter list
    if (!consume_char(parser, '(')) {
        // No parameters, use defaults
        return cb;
    }
    
    // Parse parameters
    while (parser->position < parser->length) {
        skip_whitespace(parser);
        
        // Check for end of parameters
        if (consume_char(parser, ')')) {
            break;
        }
        
        // Parse parameter name
        char* param_name = parse_identifier(parser);
        if (!param_name) {
            parser->error_message = strdup("Expected parameter name");
            circuit_breaker_annotation_free(cb);
            return NULL;
        }
        
        // Expect '='
        if (!consume_char(parser, '=')) {
            parser->error_message = strdup("Expected '=' after parameter name");
            free(param_name);
            circuit_breaker_annotation_free(cb);
            return NULL;
        }
        
        // Parse parameter value based on name
        if (strcmp(param_name, "failure_threshold") == 0) {
            cb->failure_threshold = (int)parse_integer(parser);
        } else if (strcmp(param_name, "failure_window_ms") == 0) {
            cb->failure_window_ms = (uint64_t)parse_integer(parser);
        } else if (strcmp(param_name, "failure_rate_threshold") == 0) {
            cb->failure_rate_threshold = parse_double(parser);
        } else if (strcmp(param_name, "open_timeout_ms") == 0) {
            cb->open_timeout_ms = (uint64_t)parse_integer(parser);
        } else if (strcmp(param_name, "half_open_max_requests") == 0) {
            cb->half_open_max_requests = (int)parse_integer(parser);
        } else if (strcmp(param_name, "recovery_threshold") == 0) {
            cb->recovery_threshold = (int)parse_integer(parser);
        }
        
        free(param_name);
        
        // Check for comma or end
        skip_whitespace(parser);
        if (parser->position < parser->length && 
            parser->source_text[parser->position] == ',') {
            parser->position++;
        }
    }
    
    return cb;
}

// =============================================================================
// Function Analysis and Code Generation
// =============================================================================

bool function_has_retry_annotation(const char* function_source) {
    if (!function_source) return false;
    
    // Simple search for @retry annotation
    return strstr(function_source, "@retry") != NULL;
}

bool function_has_circuit_breaker_annotation(const char* function_source) {
    if (!function_source) return false;
    
    // Simple search for @circuit_breaker annotation
    return strstr(function_source, "@circuit_breaker") != NULL;
}

char* generate_retry_wrapper_code(const RetryAnnotation* retry,
                                 const char* function_name,
                                 const char* function_signature) {
    if (!retry || !function_name || !function_signature) return NULL;
    
    size_t code_size = 2048;
    char* code = malloc(code_size);
    if (!code) return NULL;
    
    // Generate wrapper function code
    snprintf(code, code_size,
        "// Generated retry wrapper for %s\n"
        "static goo_error_union_t* __retry_wrapper_%s(void* args) {\n"
        "    return __original_%s(args);\n"
        "}\n"
        "\n"
        "%s {\n"
        "    // Setup retry annotation\n"
        "    RetryAnnotation* __retry = retry_annotation_create_default();\n"
        "    __retry->max_attempts = %d;\n"
        "    __retry->backoff.strategy = %d;\n"
        "    __retry->backoff.initial_delay_ms = %lu;\n"
        "    __retry->backoff.max_delay_ms = %lu;\n"
        "    __retry->backoff.multiplier = %f;\n"
        "    __retry->backoff.jitter_factor = %f;\n"
        "    __retry->total_timeout_ms = %lu;\n"
        "    __retry->per_attempt_timeout_ms = %lu;\n"
        "    \n"
        "    // Execute with retry\n"
        "    ErrorRecoveryContext* __context = error_recovery_context_create(\n"
        "        __func__, __FILE__, __LINE__);\n"
        "    error_recovery_context_set_retry(__context, __retry);\n"
        "    \n"
        "    goo_error_union_t* __result = execute_with_recovery_patterns(\n"
        "        __context, __retry_wrapper_%s, args);\n"
        "    \n"
        "    // Cleanup\n"
        "    error_recovery_context_free(__context);\n"
        "    retry_annotation_free(__retry);\n"
        "    \n"
        "    return __result;\n"
        "}\n",
        function_name,
        function_name,
        function_name,
        function_signature,
        retry->max_attempts,
        (int)retry->backoff.strategy,
        retry->backoff.initial_delay_ms,
        retry->backoff.max_delay_ms,
        retry->backoff.multiplier,
        retry->backoff.jitter_factor,
        retry->total_timeout_ms,
        retry->per_attempt_timeout_ms,
        function_name
    );
    
    return code;
}

char* generate_circuit_breaker_wrapper_code(const CircuitBreakerAnnotation* circuit_breaker,
                                           const char* function_name,
                                           const char* function_signature) {
    if (!circuit_breaker || !function_name || !function_signature) return NULL;
    
    size_t code_size = 2048;
    char* code = malloc(code_size);
    if (!code) return NULL;
    
    // Generate wrapper function code
    snprintf(code, code_size,
        "// Generated circuit breaker wrapper for %s\n"
        "static goo_error_union_t* __cb_wrapper_%s(void* args) {\n"
        "    return __original_%s(args);\n"
        "}\n"
        "\n"
        "%s {\n"
        "    // Setup circuit breaker annotation\n"
        "    CircuitBreakerAnnotation* __cb = circuit_breaker_annotation_create_default();\n"
        "    __cb->failure_threshold = %d;\n"
        "    __cb->failure_window_ms = %lu;\n"
        "    __cb->failure_rate_threshold = %f;\n"
        "    __cb->open_timeout_ms = %lu;\n"
        "    __cb->half_open_max_requests = %d;\n"
        "    __cb->recovery_threshold = %d;\n"
        "    \n"
        "    // Execute with circuit breaker\n"
        "    ErrorRecoveryContext* __context = error_recovery_context_create(\n"
        "        __func__, __FILE__, __LINE__);\n"
        "    error_recovery_context_set_circuit_breaker(__context, __cb);\n"
        "    \n"
        "    goo_error_union_t* __result = execute_with_recovery_patterns(\n"
        "        __context, __cb_wrapper_%s, args);\n"
        "    \n"
        "    // Cleanup\n"
        "    error_recovery_context_free(__context);\n"
        "    circuit_breaker_annotation_free(__cb);\n"
        "    \n"
        "    return __result;\n"
        "}\n",
        function_name,
        function_name,
        function_name,
        function_signature,
        circuit_breaker->failure_threshold,
        circuit_breaker->failure_window_ms,
        circuit_breaker->failure_rate_threshold,
        circuit_breaker->open_timeout_ms,
        circuit_breaker->half_open_max_requests,
        circuit_breaker->recovery_threshold,
        function_name
    );
    
    return code;
}

// =============================================================================
// Integration with Existing Parser
// =============================================================================

// Parse annotation from comment or attribute
RetryAnnotation* parse_retry_from_comment(const char* comment_text) {
    if (!comment_text) return NULL;
    
    // Look for @retry in comment
    const char* retry_pos = strstr(comment_text, "@retry");
    if (!retry_pos) return NULL;
    
    // Find the rest of the annotation
    const char* start = retry_pos;
    const char* end = strchr(start, '\n');
    if (!end) end = start + strlen(start);
    
    size_t annotation_len = end - start;
    char* annotation_text = malloc(annotation_len + 1);
    if (!annotation_text) return NULL;
    
    strncpy(annotation_text, start, annotation_len);
    annotation_text[annotation_len] = '\0';
    
    AnnotationParser* parser = annotation_parser_new(annotation_text, annotation_len);
    RetryAnnotation* retry = parse_retry_annotation(parser);
    
    annotation_parser_free(parser);
    free(annotation_text);
    
    return retry;
}

CircuitBreakerAnnotation* parse_circuit_breaker_from_comment(const char* comment_text) {
    if (!comment_text) return NULL;
    
    // Look for @circuit_breaker in comment
    const char* cb_pos = strstr(comment_text, "@circuit_breaker");
    if (!cb_pos) return NULL;
    
    // Find the rest of the annotation
    const char* start = cb_pos;
    const char* end = strchr(start, '\n');
    if (!end) end = start + strlen(start);
    
    size_t annotation_len = end - start;
    char* annotation_text = malloc(annotation_len + 1);
    if (!annotation_text) return NULL;
    
    strncpy(annotation_text, start, annotation_len);
    annotation_text[annotation_len] = '\0';
    
    AnnotationParser* parser = annotation_parser_new(annotation_text, annotation_len);
    CircuitBreakerAnnotation* cb = parse_circuit_breaker_annotation(parser);
    
    annotation_parser_free(parser);
    free(annotation_text);
    
    return cb;
}