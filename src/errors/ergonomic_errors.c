#include "ergonomic_errors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <stdarg.h>

// Performance timing utilities
static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Default configuration
ErgoErrorConfig ergo_default_config = {
    .enable_auto_context = true,
    .enable_stack_traces = true,
    .enable_suggestions = true,
    .enable_recovery_patterns = true,
    .enable_error_aggregation = true,
    .max_context_depth = 50,
    .max_error_collection_size = 100,
    .default_retry_backoff = 1.5,
    .default_max_retries = 3,
    .track_performance = true,
    .lazy_context_generation = true
};

// Create new ergonomic error context
ErgoErrorContext* ergo_error_context_new(void) {
    ErgoErrorContext* ctx = xcalloc(1, sizeof(ErgoErrorContext));
    if (!ctx) return NULL;
    
    ctx->base_context = error_context_new();
    if (!ctx->base_context) {
        free(ctx);
        return NULL;
    }
    
    // Apply default configuration
    ergo_apply_config(ctx, &ergo_default_config);
    
    // Initialize error collection
    ctx->error_collection_capacity = 16;
    ctx->error_collection = malloc(sizeof(Error*) * ctx->error_collection_capacity);
    if (!ctx->error_collection) {
        error_context_free(ctx->base_context);
        free(ctx);
        return NULL;
    }
    
    return ctx;
}

// Free ergonomic error context
void ergo_error_context_free(ErgoErrorContext* ctx) {
    if (!ctx) return;
    
    // Pop all context frames
    while (ctx->current_frame) {
        ergo_pop_context_frame(ctx);
    }
    
    // Free error collection
    if (ctx->error_collection) {
        for (size_t i = 0; i < ctx->error_collection_size; i++) {
            if (ctx->error_collection[i]) {
                free((void*)ctx->error_collection[i]->message);
                free((void*)ctx->error_collection[i]->hint);
                free(ctx->error_collection[i]);
            }
        }
        free(ctx->error_collection);
    }
    
    // Free base context
    error_context_free(ctx->base_context);
    free(ctx);
}

// Push context frame for automatic error context
ErrorContextFrame* ergo_push_context_frame(ErgoErrorContext* ctx, 
                                          const char* function_name,
                                          const char* operation_description) {
    if (!ctx || ctx->current_depth >= ctx->max_context_depth) {
        return NULL;
    }
    
    ErrorContextFrame* frame = xcalloc(1, sizeof(ErrorContextFrame));
    if (!frame) return NULL;
    
    frame->function_name = function_name ? strdup(function_name) : NULL;
    frame->operation_description = operation_description ? strdup(operation_description) : NULL;
    frame->parent = ctx->current_frame;
    
    ctx->current_frame = frame;
    ctx->current_depth++;
    
    return frame;
}

// Pop context frame
void ergo_pop_context_frame(ErgoErrorContext* ctx) {
    if (!ctx || !ctx->current_frame) return;
    
    ErrorContextFrame* frame = ctx->current_frame;
    ctx->current_frame = frame->parent;
    ctx->current_depth--;
    
    // Call cleanup function if set
    if (frame->cleanup) {
        frame->cleanup(frame);
    }
    
    // Free frame data
    free((void*)frame->function_name);
    free((void*)frame->file_name);
    free((void*)frame->operation_description);
    free((void*)frame->current_file_being_processed);
    free((void*)frame->current_function_being_analyzed);
    free(frame);
}

// Automatic error propagation (called by TRY macro)
void _ergo_propagate_error(Error* error, const char* file, size_t line, 
                          const char* function, const char* expression) {
    if (!error) return;
    
    uint64_t start_time = get_time_ns();
    
    // Create a detailed error message with context
    char context_buffer[1024];
    snprintf(context_buffer, sizeof(context_buffer),
             "Error propagated from %s:%zu in %s() while evaluating: %s",
             file, line, function, expression);
    
    // If this error doesn't have a hint, add our context as hint
    if (!error->hint) {
        error->hint = strdup(context_buffer);
    } else {
        // Append to existing hint
        char* new_hint = malloc(strlen(error->hint) + strlen(context_buffer) + 10);
        if (new_hint) {
            snprintf(new_hint, strlen(error->hint) + strlen(context_buffer) + 10,
                    "%s\n%s", error->hint, context_buffer);
            free((void*)error->hint);
            error->hint = new_hint;
        }
    }
    
    // Update performance tracking
    uint64_t end_time = get_time_ns();
    // Note: In a real implementation, we'd get the context from thread-local storage
    // For now, we'll just track conceptually
    (void)start_time;
    (void)end_time;
}

// Add context to error
void _ergo_add_context(Error* error, const char* context) {
    if (!error || !context) return;
    
    if (!error->hint) {
        error->hint = strdup(context);
    } else {
        char* new_hint = malloc(strlen(error->hint) + strlen(context) + 4);
        if (new_hint) {
            snprintf(new_hint, strlen(error->hint) + strlen(context) + 4,
                    "%s; %s", error->hint, context);
            free((void*)error->hint);
            error->hint = new_hint;
        }
    }
}

// Enrich error with automatic context information
void ergo_enrich_error_with_context(Error* error, const ErrorContextFrame* frame) {
    if (!error || !frame) return;
    
    char enrichment[512];
    snprintf(enrichment, sizeof(enrichment),
             "In %s() while %s", 
             frame->function_name ? frame->function_name : "unknown",
             frame->operation_description ? frame->operation_description : "performing operation");
    
    _ergo_add_context(error, enrichment);
}

// Suggest fixes for common errors
void ergo_suggest_fix_for_error(Error* error) {
    if (!error) return;
    
    const char* suggestion = NULL;
    
    switch (error->code) {
        case ERROR_TYPE_MISMATCH:
            suggestion = "Try explicit type casting or check variable types";
            break;
        case ERROR_UNDEFINED_VARIABLE:
            suggestion = "Check variable name spelling or ensure it's declared in scope";
            break;
        case ERROR_UNDEFINED_TYPE:
            suggestion = "Import the required module or check type name spelling";
            break;
        case ERROR_MISSING_SEMICOLON:
            suggestion = "Add a semicolon ';' at the end of the statement";
            break;
        case ERROR_MISSING_CLOSING_PAREN:
            suggestion = "Add closing parenthesis ')' to match opening parenthesis";
            break;
        case ERROR_MISSING_CLOSING_BRACE:
            suggestion = "Add closing brace '}' to match opening brace";
            break;
        case ERROR_INVALID_EXPRESSION:
            suggestion = "Check expression syntax and operator precedence";
            break;
        default:
            // Try to infer suggestion from error message
            if (error->message) {
                if (strstr(error->message, "not found")) {
                    suggestion = "Check spelling and imports";
                } else if (strstr(error->message, "expected")) {
                    suggestion = "Review syntax requirements";
                }
            }
            break;
    }
    
    if (suggestion) {
        _ergo_add_context(error, suggestion);
    }
}

// Add stack trace information to error
void ergo_add_stack_trace_to_error(Error* error, const ErgoErrorContext* ctx) {
    if (!error || !ctx || !ctx->preserve_call_stack) return;
    
    char stack_trace[2048] = "Call stack:\n";
    const ErrorContextFrame* frame = ctx->current_frame;
    int depth = 0;
    
    while (frame && depth < 10) { // Limit stack trace depth
        char frame_info[256];
        snprintf(frame_info, sizeof(frame_info), "  %d. %s() - %s\n",
                depth + 1,
                frame->function_name ? frame->function_name : "unknown",
                frame->operation_description ? frame->operation_description : "unknown operation");
        
        if (strlen(stack_trace) + strlen(frame_info) < sizeof(stack_trace) - 1) {
            strcat(stack_trace, frame_info);
        }
        
        frame = frame->parent;
        depth++;
    }
    
    if (depth > 0) {
        _ergo_add_context(error, stack_trace);
    }
}

// Error collector implementation
ErrorCollector* error_collector_new(ErgoErrorContext* ctx) {
    ErrorCollector* collector = xcalloc(1, sizeof(ErrorCollector));
    if (!collector) return NULL;
    
    collector->ctx = ctx;
    collector->capacity = 16;
    collector->errors = malloc(sizeof(Error*) * collector->capacity);
    if (!collector->errors) {
        free(collector);
        return NULL;
    }
    
    collector->stop_on_first_error = false;
    collector->deduplicate_errors = true;
    collector->max_errors = 100;
    
    return collector;
}

void error_collector_free(ErrorCollector* collector) {
    if (!collector) return;
    
    if (collector->errors) {
        for (size_t i = 0; i < collector->count; i++) {
            if (collector->errors[i]) {
                free((void*)collector->errors[i]->message);
                free((void*)collector->errors[i]->hint);
                free(collector->errors[i]);
            }
        }
        free(collector->errors);
    }
    
    free(collector);
}

// Add error to collector
bool error_collector_try(ErrorCollector* collector, Error* error) {
    if (!collector || !error) return false;
    
    // Check if should stop on first error
    if (collector->stop_on_first_error && collector->count > 0) {
        return false;
    }
    
    // Check if reached max errors
    if (collector->count >= collector->max_errors) {
        return false;
    }
    
    // Check for duplicates if deduplication is enabled
    if (collector->deduplicate_errors) {
        for (size_t i = 0; i < collector->count; i++) {
            if (collector->errors[i]->code == error->code &&
                collector->errors[i]->location.line == error->location.line &&
                collector->errors[i]->location.filename == error->location.filename) {
                // Duplicate found, don't add
                return true; // Continue processing
            }
        }
    }
    
    // Expand array if needed
    if (collector->count >= collector->capacity) {
        collector->capacity *= 2;
        collector->errors = realloc(collector->errors, 
                                   sizeof(Error*) * collector->capacity);
        if (!collector->errors) return false;
    }
    
    // Clone the error
    Error* cloned_error = xmalloc(sizeof(Error));
    if (!cloned_error) return false;
    
    *cloned_error = *error;
    cloned_error->message = error->message ? strdup(error->message) : NULL;
    cloned_error->hint = error->hint ? strdup(error->hint) : NULL;
    cloned_error->next = NULL;
    
    collector->errors[collector->count++] = cloned_error;
    
    // Update statistics
    if (error->severity == ERROR_SEVERITY_FATAL || 
        error->severity == ERROR_SEVERITY_ERROR) {
        collector->total_errors++;
        if (error->severity == ERROR_SEVERITY_FATAL) {
            collector->has_fatal_errors = true;
        }
    } else if (error->severity == ERROR_SEVERITY_WARNING) {
        collector->total_warnings++;
    }
    
    return true; // Continue processing
}

// Finish error collection and return aggregated result
Error* error_collector_finish(ErrorCollector* collector) {
    if (!collector || collector->count == 0) return NULL;
    
    if (collector->count == 1) {
        // Single error, return it directly
        Error* error = collector->errors[0];
        collector->errors[0] = NULL; // Don't free it
        return error;
    }
    
    // Multiple errors, create an aggregated error
    Error* aggregated = xmalloc(sizeof(Error));
    if (!aggregated) return NULL;
    
    aggregated->code = ERROR_INTERNAL; // Use generic code for aggregated errors
    aggregated->severity = collector->has_fatal_errors ? ERROR_SEVERITY_FATAL : ERROR_SEVERITY_ERROR;
    aggregated->category = ERROR_CATEGORY_INTERNAL;
    aggregated->location = empty_source_location();
    aggregated->next = NULL;
    
    // Build aggregated message
    char* message = malloc(4096);
    if (!message) {
        free(aggregated);
        return NULL;
    }
    
    snprintf(message, 4096, "Multiple errors occurred (%zu errors, %zu warnings):",
             collector->total_errors, collector->total_warnings);
    
    // Add individual error messages
    for (size_t i = 0; i < collector->count && i < 10; i++) { // Limit to first 10
        char error_line[256];
        snprintf(error_line, sizeof(error_line), "\n  %zu. %s",
                i + 1, collector->errors[i]->message ? collector->errors[i]->message : "Unknown error");
        
        if (strlen(message) + strlen(error_line) < 4095) {
            strcat(message, error_line);
        }
    }
    
    if (collector->count > 10) {
        char more_msg[64];
        snprintf(more_msg, sizeof(more_msg), "\n  ... and %zu more errors", 
                collector->count - 10);
        if (strlen(message) + strlen(more_msg) < 4095) {
            strcat(message, more_msg);
        }
    }
    
    aggregated->message = message;
    aggregated->hint = strdup("Review each error individually and fix them one by one");
    
    return aggregated;
}

// Structured error implementation
StructuredError* structured_error_new(StructuredErrorType type, 
                                     const char* domain,
                                     const char* component) {
    StructuredError* error = xcalloc(1, sizeof(StructuredError));
    if (!error) return NULL;
    
    error->base.code = (ErrorCode)type;
    error->base.severity = ERROR_SEVERITY_ERROR;
    error->base.category = ERROR_CATEGORY_INTERNAL;
    error->base.location = empty_source_location();
    
    error->error_type = type;
    error->domain = domain ? strdup(domain) : NULL;
    error->component = component ? strdup(component) : NULL;
    
    return error;
}

void structured_error_free(StructuredError* error) {
    if (!error) return;
    
    free((void*)error->base.message);
    free((void*)error->base.hint);
    free((void*)error->domain);
    free((void*)error->component);
    free((void*)error->error_id);
    free((void*)error->help_url);
    free((void*)error->suggested_action);
    free((void*)error->message_key);
    
    // Free context arrays
    if (error->context_keys) {
        for (size_t i = 0; i < error->context_count; i++) {
            free(error->context_keys[i]);
            free(error->context_values[i]);
        }
        free(error->context_keys);
        free(error->context_values);
    }
    
    // Free message parameters
    if (error->message_params) {
        for (size_t i = 0; i < error->param_count; i++) {
            free(error->message_params[i]);
        }
        free(error->message_params);
    }
    
    free(error);
}

// Add context to structured error
void structured_error_add_context(StructuredError* error, 
                                 const char* key, const char* value) {
    if (!error || !key || !value) return;
    
    // Expand arrays if needed
    if (error->context_count >= 16) return; // Arbitrary limit
    
    if (!error->context_keys) {
        error->context_keys = malloc(sizeof(char*) * 16);
        error->context_values = malloc(sizeof(char*) * 16);
        if (!error->context_keys || !error->context_values) return;
    }
    
    error->context_keys[error->context_count] = strdup(key);
    error->context_values[error->context_count] = strdup(value);
    error->context_count++;
}

// Set help information
void structured_error_set_help(StructuredError* error, 
                              const char* help_url, 
                              const char* suggested_action) {
    if (!error) return;
    
    free((void*)error->help_url);
    free((void*)error->suggested_action);
    
    error->help_url = help_url ? strdup(help_url) : NULL;
    error->suggested_action = suggested_action ? strdup(suggested_action) : NULL;
}

// Error transformer implementation
ErrorTransformer* error_transformer_new(void) {
    ErrorTransformer* transformer = xcalloc(1, sizeof(ErrorTransformer));
    if (!transformer) return NULL;
    
    transformer->auto_transform_enabled = true;
    transformer->add_suggestions = true;
    transformer->add_help_urls = true;
    transformer->localize_messages = false;
    
    return transformer;
}

void error_transformer_free(ErrorTransformer* transformer) {
    if (!transformer) return;
    
    ErrorTransformRule* rule = transformer->rules;
    while (rule) {
        ErrorTransformRule* next = rule->next;
        free((void*)rule->context_pattern);
        free(rule);
        rule = next;
    }
    
    free(transformer);
}

// Integration functions
void ergo_integrate_with_error_context(ErgoErrorContext* ergo_ctx, ErrorContext* base_ctx) {
    if (!ergo_ctx || !base_ctx) return;
    
    // Replace the base context
    if (ergo_ctx->base_context) {
        error_context_free(ergo_ctx->base_context);
    }
    ergo_ctx->base_context = base_ctx;
}

ErrorContext* ergo_get_base_context(ErgoErrorContext* ergo_ctx) {
    return ergo_ctx ? ergo_ctx->base_context : NULL;
}

// Statistics
ErrorHandlingStats ergo_get_stats(const ErgoErrorContext* ctx) {
    ErrorHandlingStats stats = {0};
    
    if (ctx) {
        stats.total_propagations = ctx->total_error_propagations;
        stats.avg_propagation_time_ns = ctx->error_handling_overhead_ns / 
                                       (ctx->total_error_propagations + 1);
        stats.max_context_depth = ctx->current_depth;
        stats.memory_used_bytes = sizeof(ErgoErrorContext) + 
                                 (ctx->error_collection_capacity * sizeof(Error*));
    }
    
    return stats;
}

void ergo_reset_stats(ErgoErrorContext* ctx) {
    if (!ctx) return;
    
    ctx->error_handling_overhead_ns = 0;
    ctx->total_error_propagations = 0;
}

// Apply configuration
void ergo_apply_config(ErgoErrorContext* ctx, const ErgoErrorConfig* config) {
    if (!ctx || !config) return;
    
    ctx->auto_context_enabled = config->enable_auto_context;
    ctx->preserve_call_stack = config->enable_stack_traces;
    ctx->max_context_depth = config->max_context_depth;
    ctx->auto_retry_enabled = config->enable_recovery_patterns;
    ctx->max_retry_attempts = config->default_max_retries;
    ctx->retry_backoff_factor = config->default_retry_backoff;
}