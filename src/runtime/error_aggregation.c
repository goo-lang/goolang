#define _POSIX_C_SOURCE 200809L
#include "error_aggregation.h"
#include "error_context.h"
#include "error_recovery.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>

// =============================================================================
// Global Error Aggregation State
// =============================================================================

static ErrorAggregationConfig g_aggregation_config = {
    .enabled = true,
    .enable_parallel_collection = true,
    .default_max_errors = 100,
    .default_collect_warnings = true,
    .default_thread_safe = false,
    .composite_error_initial_capacity = 10,
    .max_composite_errors = 1000,
    .parallel_thread_pool_size = 4,
    .enable_error_caching = false
};

static ErrorAggregationStats g_aggregation_stats = {0};
static pthread_mutex_t g_aggregation_mutex = PTHREAD_MUTEX_INITIALIZER;

// =============================================================================
// Utility Functions
// =============================================================================

static uint64_t get_current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static char* duplicate_string(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

static void categorize_error(const goo_error_t* error, CompositeError* composite) {
    if (!error || !composite) return;
    
    // Categorize based on error code ranges
    if (error->code >= 5000 && error->code < 6000) {
        composite->error_counts.network_errors++;
    } else if (error->code >= 2000 && error->code < 3000) {
        composite->error_counts.validation_errors++;
    } else if (error->code >= 1000 && error->code < 2000) {
        composite->error_counts.file_errors++;
    } else if (error->code >= 9000) {
        composite->error_counts.fatal_errors++;
    } else {
        composite->error_counts.warning_errors++;
    }
}

// =============================================================================
// Error Aggregation System Lifecycle
// =============================================================================

void error_aggregation_system_init(void) {
    pthread_mutex_lock(&g_aggregation_mutex);
    
    if (!g_aggregation_config.enabled) {
        pthread_mutex_unlock(&g_aggregation_mutex);
        return;
    }
    
    // Reset statistics
    memset(&g_aggregation_stats, 0, sizeof(g_aggregation_stats));
    
    pthread_mutex_unlock(&g_aggregation_mutex);
    
    printf("🔄 Error aggregation system initialized\n");
}

void error_aggregation_system_shutdown(void) {
    pthread_mutex_lock(&g_aggregation_mutex);
    
    if (!g_aggregation_config.enabled) {
        pthread_mutex_unlock(&g_aggregation_mutex);
        return;
    }
    
    // Print final statistics
    print_error_aggregation_stats();
    
    pthread_mutex_unlock(&g_aggregation_mutex);
    
    printf("🔄 Error aggregation system shutdown\n");
}

// =============================================================================
// Composite Error Implementation
// =============================================================================

CompositeError* composite_error_new(const char* operation_name) {
    return composite_error_new_with_capacity(operation_name, 
                                            g_aggregation_config.composite_error_initial_capacity);
}

CompositeError* composite_error_new_with_capacity(const char* operation_name, int capacity) {
    CompositeError* composite = calloc(1, sizeof(CompositeError));
    if (!composite) return NULL;
    
    composite->operation_name = duplicate_string(operation_name);
    composite->error_capacity = capacity > 0 ? capacity : 10;
    composite->errors = calloc(composite->error_capacity, sizeof(goo_error_t*));
    composite->error_count = 0;
    composite->owns_errors = true;
    composite->created_time_ms = get_current_time_ms();
    
    if (!composite->errors) {
        free((void*)composite->operation_name);
        free(composite);
        return NULL;
    }
    
    pthread_mutex_lock(&g_aggregation_mutex);
    g_aggregation_stats.total_composite_errors_created++;
    g_aggregation_stats.active_composite_errors++;
    pthread_mutex_unlock(&g_aggregation_mutex);
    
    return composite;
}

void composite_error_free(CompositeError* composite) {
    if (!composite) return;
    
    if (composite->owns_errors) {
        for (int i = 0; i < composite->error_count; i++) {
            if (composite->errors[i]) {
                goo_error_free(composite->errors[i]);
            }
        }
    }
    
    free(composite->errors);
    free((void*)composite->operation_name);
    free((void*)composite->message);
    free((void*)composite->source_file);
    
    if (composite->destructor) {
        composite->destructor(composite);
    }
    
    pthread_mutex_lock(&g_aggregation_mutex);
    g_aggregation_stats.active_composite_errors--;
    pthread_mutex_unlock(&g_aggregation_mutex);
    
    free(composite);
}

void composite_error_add(CompositeError* composite, goo_error_t* error) {
    composite_error_add_with_context(composite, error, NULL);
}

void composite_error_add_with_context(CompositeError* composite,
                                     goo_error_t* error,
                                     const char* context) {
    if (!composite || !error) return;
    
    // Expand capacity if needed
    if (composite->error_count >= composite->error_capacity) {
        int new_capacity = composite->error_capacity * 2;
        goo_error_t** new_errors = realloc(composite->errors, 
                                          new_capacity * sizeof(goo_error_t*));
        if (!new_errors) return; // Failed to expand
        
        composite->errors = new_errors;
        composite->error_capacity = new_capacity;
        
        // Initialize new slots
        for (int i = composite->error_count; i < new_capacity; i++) {
            composite->errors[i] = NULL;
        }
    }
    
    // Add the error
    composite->errors[composite->error_count] = error;
    composite->error_count++;
    
    // Update timing
    uint64_t now = get_current_time_ms();
    if (composite->error_count == 1) {
        composite->first_error_time_ms = now;
    }
    composite->last_error_time_ms = now;
    
    // Categorize the error
    categorize_error(error, composite);
    
    // Add context if provided
    if (context && error->cause == NULL) {
        // Create a context error to chain
        goo_error_t* context_error = goo_new_error(context);
        if (context_error) {
            error->cause = context_error;
        }
    }
    
    pthread_mutex_lock(&g_aggregation_mutex);
    g_aggregation_stats.total_errors_collected++;
    pthread_mutex_unlock(&g_aggregation_mutex);
}

void composite_error_merge(CompositeError* dest, CompositeError* src) {
    if (!dest || !src) return;
    
    for (int i = 0; i < src->error_count; i++) {
        if (src->errors[i]) {
            // Clone the error if source owns it
            goo_error_t* error_to_add;
            if (src->owns_errors) {
                error_to_add = goo_new_error_with_code(src->errors[i]->message, 
                                                      src->errors[i]->code);
                if (src->errors[i]->cause) {
                    error_to_add->cause = goo_new_error(src->errors[i]->cause->message);
                }
            } else {
                error_to_add = src->errors[i];
            }
            
            composite_error_add(dest, error_to_add);
        }
    }
}

bool composite_error_has_fatal_errors(const CompositeError* composite) {
    return composite && composite->error_counts.fatal_errors > 0;
}

int composite_error_get_error_count(const CompositeError* composite) {
    return composite ? composite->error_count : 0;
}

goo_error_t* composite_error_get_first_error(const CompositeError* composite) {
    return (composite && composite->error_count > 0) ? composite->errors[0] : NULL;
}

goo_error_t* composite_error_get_last_error(const CompositeError* composite) {
    return (composite && composite->error_count > 0) ? 
           composite->errors[composite->error_count - 1] : NULL;
}

goo_error_t* composite_error_to_goo_error(const CompositeError* composite) {
    if (!composite || composite->error_count == 0) {
        return goo_new_error("Empty composite error");
    }
    
    // Create a summary error message
    char* summary = composite_error_to_string(composite);
    goo_error_t* result = goo_new_error(summary);
    free(summary);
    
    // Chain the first error as the cause
    if (composite->error_count > 0 && composite->errors[0]) {
        result->cause = goo_new_error_with_code(composite->errors[0]->message,
                                               composite->errors[0]->code);
    }
    
    return result;
}

char* composite_error_to_string(const CompositeError* composite) {
    if (!composite) return duplicate_string("Invalid composite error");
    
    if (composite->error_count == 0) {
        return duplicate_string("No errors in composite");
    }
    
    // Calculate needed buffer size
    size_t total_size = 256; // Base size for header
    for (int i = 0; i < composite->error_count; i++) {
        if (composite->errors[i] && composite->errors[i]->message) {
            total_size += strlen(composite->errors[i]->message) + 50; // Extra for formatting
        }
    }
    
    char* result = malloc(total_size);
    if (!result) return NULL;
    
    // Build the summary message
    if (composite->operation_name) {
        snprintf(result, total_size, "Operation '%s' failed with %d error(s):\n",
                composite->operation_name, composite->error_count);
    } else {
        snprintf(result, total_size, "Composite error with %d error(s):\n",
                composite->error_count);
    }
    
    // Add first few errors (limit to prevent huge messages)
    int max_errors_to_show = composite->error_count > 5 ? 5 : composite->error_count;
    for (int i = 0; i < max_errors_to_show; i++) {
        if (composite->errors[i] && composite->errors[i]->message) {
            char error_line[256];
            snprintf(error_line, sizeof(error_line), "  %d. [%d] %s\n", 
                    i + 1, composite->errors[i]->code, composite->errors[i]->message);
            strncat(result, error_line, total_size - strlen(result) - 1);
        }
    }
    
    // Add "and X more..." if there are more errors
    if (composite->error_count > max_errors_to_show) {
        char more_line[64];
        snprintf(more_line, sizeof(more_line), "  ... and %d more error(s)\n",
                composite->error_count - max_errors_to_show);
        strncat(result, more_line, total_size - strlen(result) - 1);
    }
    
    return result;
}

char* composite_error_to_detailed_string(const CompositeError* composite) {
    if (!composite) return duplicate_string("Invalid composite error");
    
    // Calculate needed buffer size (be generous)
    size_t total_size = 1024; // Base size for header and statistics
    for (int i = 0; i < composite->error_count; i++) {
        if (composite->errors[i] && composite->errors[i]->message) {
            total_size += strlen(composite->errors[i]->message) + 200; // Extra for detailed formatting
        }
    }
    
    char* result = malloc(total_size);
    if (!result) return NULL;
    
    // Build detailed header
    snprintf(result, total_size,
        "=== Composite Error Report ===\n"
        "Operation: %s\n"
        "Total Errors: %d\n"
        "Created: %lu ms ago\n"
        "Error Breakdown:\n"
        "  - Fatal: %d\n"
        "  - Validation: %d\n"
        "  - Network: %d\n"
        "  - File I/O: %d\n"
        "  - Warnings: %d\n\n"
        "Individual Errors:\n",
        composite->operation_name ? composite->operation_name : "Unknown",
        composite->error_count,
        get_current_time_ms() - composite->created_time_ms,
        composite->error_counts.fatal_errors,
        composite->error_counts.validation_errors,
        composite->error_counts.network_errors,
        composite->error_counts.file_errors,
        composite->error_counts.warning_errors);
    
    // Add all errors with details
    for (int i = 0; i < composite->error_count; i++) {
        if (composite->errors[i]) {
            char error_detail[512];
            snprintf(error_detail, sizeof(error_detail),
                    "%d. Error Code: %d\n"
                    "   Message: %s\n"
                    "   Cause: %s\n\n",
                    i + 1,
                    composite->errors[i]->code,
                    composite->errors[i]->message ? composite->errors[i]->message : "No message",
                    (composite->errors[i]->cause && composite->errors[i]->cause->message) ?
                        composite->errors[i]->cause->message : "None");
            strncat(result, error_detail, total_size - strlen(result) - 1);
        }
    }
    
    return result;
}

// =============================================================================
// Error Aggregator Implementation
// =============================================================================

ErrorAggregator* error_aggregator_new(AggregatorMode mode) {
    return error_aggregator_new_with_config(mode, 
                                          g_aggregation_config.default_max_errors,
                                          g_aggregation_config.default_thread_safe);
}

ErrorAggregator* error_aggregator_new_with_config(AggregatorMode mode, 
                                               int max_errors, 
                                               bool thread_safe) {
    ErrorAggregator* aggregator = calloc(1, sizeof(ErrorAggregator));
    if (!aggregator) return NULL;
    
    aggregator->mode = mode;
    aggregator->max_errors = max_errors;
    aggregator->collect_warnings = g_aggregation_config.default_collect_warnings;
    aggregator->collect_context = true;
    aggregator->thread_safe = thread_safe;
    aggregator->successful_operations = 0;
    aggregator->total_operations = 0;
    aggregator->has_fatal_errors = false;
    aggregator->collection_stopped = false;
    
    // Initialize thread safety
    if (thread_safe) {
        if (pthread_mutex_init(&aggregator->mutex, NULL) != 0) {
            free(aggregator);
            return NULL;
        }
    }
    
    // Create the composite error
    aggregator->composite = composite_error_new("Error Collection");
    if (!aggregator->composite) {
        if (thread_safe) {
            pthread_mutex_destroy(&aggregator->mutex);
        }
        free(aggregator);
        return NULL;
    }
    
    pthread_mutex_lock(&g_aggregation_mutex);
    g_aggregation_stats.total_aggregators_created++;
    g_aggregation_stats.active_aggregators++;
    pthread_mutex_unlock(&g_aggregation_mutex);
    
    return aggregator;
}

void error_aggregator_free(ErrorAggregator* aggregator) {
    if (!aggregator) return;
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    composite_error_free(aggregator->composite);
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
        pthread_mutex_destroy(&aggregator->mutex);
    }
    
    pthread_mutex_lock(&g_aggregation_mutex);
    g_aggregation_stats.active_aggregators--;
    pthread_mutex_unlock(&g_aggregation_mutex);
    
    free(aggregator);
}

// Configuration methods
void error_aggregator_set_max_errors(ErrorAggregator* aggregator, int max_errors) {
    if (!aggregator) return;
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    aggregator->max_errors = max_errors;
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
    }
}

void error_aggregator_set_collect_warnings(ErrorAggregator* aggregator, bool collect) {
    if (!aggregator) return;
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    aggregator->collect_warnings = collect;
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
    }
}

void error_aggregator_set_collect_context(ErrorAggregator* aggregator, bool collect) {
    if (!aggregator) return;
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    aggregator->collect_context = collect;
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
    }
}

void error_aggregator_set_operation_name(ErrorAggregator* aggregator, const char* name) {
    if (!aggregator) return;
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    free((void*)aggregator->composite->operation_name);
    aggregator->composite->operation_name = duplicate_string(name);
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
    }
}

// Hook methods
void error_aggregator_set_error_hook(ErrorAggregator* aggregator,
                                   void (*hook)(ErrorAggregator*, const goo_error_t*, void*),
                                   void* context) {
    if (!aggregator) return;
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    aggregator->on_error_collected = hook;
    aggregator->hook_context = context;
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
    }
}

void error_aggregator_set_limit_hook(ErrorAggregator* aggregator,
                                   void (*hook)(ErrorAggregator*, int, void*),
                                   void* context) {
    if (!aggregator) return;
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    aggregator->on_limit_reached = hook;
    aggregator->hook_context = context;
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
    }
}

void error_aggregator_set_completion_hook(ErrorAggregator* aggregator,
                                        void (*hook)(ErrorAggregator*, bool, void*),
                                        void* context) {
    if (!aggregator) return;
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    aggregator->on_collection_finished = hook;
    aggregator->hook_context = context;
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
    }
}

// Error recovery integration
void error_aggregator_set_retry(ErrorAggregator* aggregator, RetryAnnotation* retry) {
    if (!aggregator) return;
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    aggregator->retry_config = retry;
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
    }
}

void error_aggregator_set_circuit_breaker(ErrorAggregator* aggregator, 
                                        CircuitBreakerAnnotation* circuit_breaker) {
    if (!aggregator) return;
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    aggregator->circuit_breaker = circuit_breaker;
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
    }
}

// =============================================================================
// Core Collection Operations
// =============================================================================

bool error_aggregator_try(ErrorAggregator* aggregator, goo_error_union_t* result) {
    return error_aggregator_try_with_name(aggregator, result, NULL);
}

bool error_aggregator_try_with_name(ErrorAggregator* aggregator,
                                  goo_error_union_t* result,
                                  const char* operation_name) {
    if (!aggregator || !result) return false;
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    // Check if collection is stopped
    if (aggregator->collection_stopped) {
        if (aggregator->thread_safe) {
            pthread_mutex_unlock(&aggregator->mutex);
        }
        return false;
    }
    
    aggregator->total_operations++;
    
    // Update global statistics
    pthread_mutex_lock(&g_aggregation_mutex);
    g_aggregation_stats.total_operations_attempted++;
    pthread_mutex_unlock(&g_aggregation_mutex);
    
    bool success = goo_error_union_is_value(result);
    
    if (success) {
        aggregator->successful_operations++;
        
        pthread_mutex_lock(&g_aggregation_mutex);
        g_aggregation_stats.total_successful_operations++;
        pthread_mutex_unlock(&g_aggregation_mutex);
        
        if (aggregator->thread_safe) {
            pthread_mutex_unlock(&aggregator->mutex);
        }
        return true;
    }
    
    // Handle error case
    goo_error_t* error = goo_error_union_get_error(result);
    if (!error) {
        if (aggregator->thread_safe) {
            pthread_mutex_unlock(&aggregator->mutex);
        }
        return false;
    }
    
    // Check collection mode
    if (aggregator->mode == AGGREGATOR_FAIL_FAST) {
        aggregator->collection_stopped = true;
        composite_error_add_with_context(aggregator->composite, 
                                       goo_new_error_with_code(error->message, error->code),
                                       operation_name);
        aggregator->has_fatal_errors = true;
        
        if (aggregator->thread_safe) {
            pthread_mutex_unlock(&aggregator->mutex);
        }
        return false;
    }
    
    // Check if this is a warning in WARN_ONLY mode
    bool is_warning = (error->code < 3000); // Simple heuristic for warnings
    if (aggregator->mode == AGGREGATOR_WARN_ONLY && !is_warning) {
        aggregator->collection_stopped = true;
        aggregator->has_fatal_errors = true;
        composite_error_add_with_context(aggregator->composite,
                                       goo_new_error_with_code(error->message, error->code),
                                       operation_name);
        
        if (aggregator->thread_safe) {
            pthread_mutex_unlock(&aggregator->mutex);
        }
        return false;
    }
    
    // Check if we should collect this error
    bool should_collect = true;
    if (is_warning && !aggregator->collect_warnings) {
        should_collect = false;
    }
    
    if (should_collect) {
        // Check max errors limit
        if (aggregator->max_errors > 0 && 
            aggregator->composite->error_count >= aggregator->max_errors) {
            
            if (aggregator->on_limit_reached) {
                aggregator->on_limit_reached(aggregator, aggregator->max_errors, 
                                          aggregator->hook_context);
            }
            
            aggregator->collection_stopped = true;
            
            if (aggregator->thread_safe) {
                pthread_mutex_unlock(&aggregator->mutex);
            }
            return false;
        }
        
        // Add the error to the composite
        composite_error_add_with_context(aggregator->composite,
                                       goo_new_error_with_code(error->message, error->code),
                                       operation_name);
        
        // Check if this is a fatal error
        if (error->code >= 9000) {
            aggregator->has_fatal_errors = true;
        }
        
        // Call error hook
        if (aggregator->on_error_collected) {
            aggregator->on_error_collected(aggregator, error, aggregator->hook_context);
        }
    }
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
    }
    
    return false; // Operation failed, but collection continues
}

bool error_aggregator_try_operation(ErrorAggregator* aggregator,
                                  CollectableOperation operation,
                                  void* args) {
    if (!aggregator || !operation) return false;
    
    goo_error_union_t* result = operation(args);
    bool success = error_aggregator_try(aggregator, result);
    goo_error_union_free(result);
    
    return success;
}

int error_aggregator_try_batch(ErrorAggregator* aggregator,
                             CollectableOperation* operations,
                             void** args_array,
                             int operation_count) {
    if (!aggregator || !operations || operation_count <= 0) return 0;
    
    int successful_count = 0;
    
    for (int i = 0; i < operation_count; i++) {
        if (aggregator->collection_stopped) break;
        
        void* args = args_array ? args_array[i] : NULL;
        if (error_aggregator_try_operation(aggregator, operations[i], args)) {
            successful_count++;
        }
    }
    
    return successful_count;
}

int error_aggregator_try_batch_named(ErrorAggregator* aggregator,
                                   CollectableOperation* operations,
                                   void** args_array,
                                   const char** operation_names,
                                   int operation_count) {
    if (!aggregator || !operations || operation_count <= 0) return 0;
    
    int successful_count = 0;
    
    for (int i = 0; i < operation_count; i++) {
        if (aggregator->collection_stopped) break;
        
        void* args = args_array ? args_array[i] : NULL;
        const char* name = operation_names ? operation_names[i] : NULL;
        
        goo_error_union_t* result = operations[i](args);
        if (error_aggregator_try_with_name(aggregator, result, name)) {
            successful_count++;
        }
        goo_error_union_free(result);
    }
    
    return successful_count;
}

void error_aggregator_add_error(ErrorAggregator* aggregator, goo_error_t* error) {
    error_aggregator_add_error_with_context(aggregator, error, NULL);
}

void error_aggregator_add_error_with_context(ErrorAggregator* aggregator,
                                           goo_error_t* error,
                                           const char* context) {
    if (!aggregator || !error) return;
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    composite_error_add_with_context(aggregator->composite, error, context);
    
    if (error->code >= 9000) {
        aggregator->has_fatal_errors = true;
    }
    
    if (aggregator->on_error_collected) {
        aggregator->on_error_collected(aggregator, error, aggregator->hook_context);
    }
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
    }
}

// Collection control
void error_aggregator_stop(ErrorAggregator* aggregator) {
    if (!aggregator) return;
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    aggregator->collection_stopped = true;
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
    }
}

void error_aggregator_reset(ErrorAggregator* aggregator) {
    if (!aggregator) return;
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    // Free current composite and create new one
    const char* operation_name = duplicate_string(aggregator->composite->operation_name);
    composite_error_free(aggregator->composite);
    aggregator->composite = composite_error_new(operation_name);
    free((void*)operation_name);
    
    // Reset state
    aggregator->successful_operations = 0;
    aggregator->total_operations = 0;
    aggregator->has_fatal_errors = false;
    aggregator->collection_stopped = false;
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
    }
}

bool error_aggregator_is_stopped(const ErrorAggregator* aggregator) {
    return aggregator ? aggregator->collection_stopped : true;
}

// =============================================================================
// Error Collection Results
// =============================================================================

goo_error_union_t* error_aggregator_finish(ErrorAggregator* aggregator) {
    if (!aggregator) {
        return goo_error_union_new_error(goo_new_error("Invalid error collector"));
    }
    
    if (aggregator->thread_safe) {
        pthread_mutex_lock(&aggregator->mutex);
    }
    
    // Call completion hook
    bool success = (aggregator->composite->error_count == 0 && !aggregator->has_fatal_errors);
    if (aggregator->on_collection_finished) {
        aggregator->on_collection_finished(aggregator, success, aggregator->hook_context);
    }
    
    goo_error_union_t* result;
    
    if (success) {
        // Create a success result (could be a summary or just success indicator)
        char* success_message = malloc(256);
        snprintf(success_message, 256, "All %d operations succeeded", 
                aggregator->successful_operations);
        result = goo_error_union_new_value(success_message, strlen(success_message), free);
    } else {
        // Return the composite error
        goo_error_t* composite_error = composite_error_to_goo_error(aggregator->composite);
        result = goo_error_union_new_error(composite_error);
    }
    
    if (aggregator->thread_safe) {
        pthread_mutex_unlock(&aggregator->mutex);
    }
    
    return result;
}

CompositeError* error_aggregator_get_composite_error(ErrorAggregator* aggregator) {
    return aggregator ? aggregator->composite : NULL;
}

bool error_aggregator_has_errors(const ErrorAggregator* aggregator) {
    return aggregator && aggregator->composite && aggregator->composite->error_count > 0;
}

bool error_aggregator_has_fatal_errors(const ErrorAggregator* aggregator) {
    return aggregator && aggregator->has_fatal_errors;
}

int error_aggregator_get_error_count(const ErrorAggregator* aggregator) {
    return aggregator && aggregator->composite ? aggregator->composite->error_count : 0;
}

int error_aggregator_get_success_count(const ErrorAggregator* aggregator) {
    return aggregator ? aggregator->successful_operations : 0;
}

double error_aggregator_get_success_rate(const ErrorAggregator* aggregator) {
    if (!aggregator || aggregator->total_operations == 0) return 0.0;
    return (double)aggregator->successful_operations / aggregator->total_operations;
}

bool error_aggregator_was_successful(const ErrorAggregator* aggregator) {
    return aggregator && 
           aggregator->composite->error_count == 0 && 
           !aggregator->has_fatal_errors &&
           aggregator->successful_operations > 0;
}

// =============================================================================
// Configuration and Statistics
// =============================================================================

void configure_error_aggregation(const ErrorAggregationConfig* config) {
    if (!config) return;
    
    pthread_mutex_lock(&g_aggregation_mutex);
    g_aggregation_config = *config;
    pthread_mutex_unlock(&g_aggregation_mutex);
}

ErrorAggregationConfig* get_error_aggregation_config(void) {
    return &g_aggregation_config;
}

ErrorAggregationStats get_error_aggregation_stats(void) {
    pthread_mutex_lock(&g_aggregation_mutex);
    ErrorAggregationStats stats = g_aggregation_stats;
    
    // Calculate derived statistics
    if (stats.total_operations_attempted > 0) {
        stats.average_collection_time_ms = 
            (double)stats.total_collection_time_ms / stats.total_operations_attempted;
    }
    
    pthread_mutex_unlock(&g_aggregation_mutex);
    return stats;
}

void reset_error_aggregation_stats(void) {
    pthread_mutex_lock(&g_aggregation_mutex);
    memset(&g_aggregation_stats, 0, sizeof(g_aggregation_stats));
    pthread_mutex_unlock(&g_aggregation_mutex);
}

void print_error_aggregation_stats(void) {
    ErrorAggregationStats stats = get_error_aggregation_stats();
    
    printf("🔄 Error Aggregation Statistics\n");
    printf("═══════════════════════════════\n");
    printf("Collection Operations:\n");
    printf("  Total Aggregators Created: %lu\n", stats.total_aggregators_created);
    printf("  Active Collectors:        %d\n", stats.active_aggregators);
    printf("  Total Composite Errors:   %lu\n", stats.total_composite_errors_created);
    printf("  Active Composite Errors:  %d\n", stats.active_composite_errors);
    printf("\nOperation Statistics:\n");
    printf("  Total Operations Attempted: %lu\n", stats.total_operations_attempted);
    printf("  Successful Operations:      %lu\n", stats.total_successful_operations);
    printf("  Total Errors Collected:     %lu\n", stats.total_errors_collected);
    printf("  Success Rate:               %.1f%%\n", 
           stats.total_operations_attempted > 0 ? 
           (double)stats.total_successful_operations / stats.total_operations_attempted * 100 : 0);
    printf("\nPerformance:\n");
    printf("  Total Collection Time:    %lu ms\n", stats.total_collection_time_ms);
    printf("  Average Collection Time:  %.2f ms\n", stats.average_collection_time_ms);
    printf("  Parallel Collections:     %lu\n", stats.total_parallel_collections);
    printf("\nMemory Usage:\n");
    printf("  Current Memory:  %lu bytes\n", stats.current_memory_usage_bytes);
    printf("  Peak Memory:     %lu bytes\n", stats.peak_memory_usage_bytes);
    printf("═══════════════════════════════\n");
}

// =============================================================================
// Batch Validation Helpers
// =============================================================================

bool validate_required_field(const char* field_name, const void* value, ErrorAggregator* aggregator) {
    if (!field_name || !aggregator) return false;
    
    if (!value) {
        goo_error_t* error = goo_new_error_with_code(
            "Required field is missing", 3001);
        error_aggregator_add_error_with_context(aggregator, error, field_name);
        return false;
    }
    
    return true;
}

bool validate_string_length(const char* field_name, const char* value, 
                           int min_length, int max_length, ErrorAggregator* aggregator) {
    if (!field_name || !aggregator) return false;
    
    if (!value) {
        goo_error_t* error = goo_new_error_with_code(
            "String field is null", 3002);
        error_aggregator_add_error_with_context(aggregator, error, field_name);
        return false;
    }
    
    int length = strlen(value);
    
    if (length < min_length) {
        char* message = malloc(256);
        snprintf(message, 256, "String is too short (min: %d, actual: %d)", 
                min_length, length);
        goo_error_t* error = goo_new_error_with_code(message, 3003);
        error_aggregator_add_error_with_context(aggregator, error, field_name);
        free(message);
        return false;
    }
    
    if (max_length > 0 && length > max_length) {
        char* message = malloc(256);
        snprintf(message, 256, "String is too long (max: %d, actual: %d)", 
                max_length, length);
        goo_error_t* error = goo_new_error_with_code(message, 3004);
        error_aggregator_add_error_with_context(aggregator, error, field_name);
        free(message);
        return false;
    }
    
    return true;
}

bool validate_numeric_range(const char* field_name, double value,
                           double min_value, double max_value, ErrorAggregator* aggregator) {
    if (!field_name || !aggregator) return false;
    
    if (value < min_value) {
        char* message = malloc(256);
        snprintf(message, 256, "Value is too small (min: %.2f, actual: %.2f)", 
                min_value, value);
        goo_error_t* error = goo_new_error_with_code(message, 3005);
        error_aggregator_add_error_with_context(aggregator, error, field_name);
        free(message);
        return false;
    }
    
    if (value > max_value) {
        char* message = malloc(256);
        snprintf(message, 256, "Value is too large (max: %.2f, actual: %.2f)", 
                max_value, value);
        goo_error_t* error = goo_new_error_with_code(message, 3006);
        error_aggregator_add_error_with_context(aggregator, error, field_name);
        free(message);
        return false;
    }
    
    return true;
}

bool validate_email_format(const char* field_name, const char* email, ErrorAggregator* aggregator) {
    if (!field_name || !aggregator) return false;
    
    if (!email) {
        goo_error_t* error = goo_new_error_with_code(
            "Email field is null", 3007);
        error_aggregator_add_error_with_context(aggregator, error, field_name);
        return false;
    }
    
    // Simple email validation (for demonstration)
    const char* at_sign = strchr(email, '@');
    if (!at_sign) {
        goo_error_t* error = goo_new_error_with_code(
            "Email is missing @ symbol", 3008);
        error_aggregator_add_error_with_context(aggregator, error, field_name);
        return false;
    }
    
    const char* dot = strchr(at_sign, '.');
    if (!dot) {
        goo_error_t* error = goo_new_error_with_code(
            "Email is missing domain extension", 3009);
        error_aggregator_add_error_with_context(aggregator, error, field_name);
        return false;
    }
    
    // Check basic structure
    if (at_sign == email || at_sign == email + strlen(email) - 1) {
        goo_error_t* error = goo_new_error_with_code(
            "Email has invalid format", 3010);
        error_aggregator_add_error_with_context(aggregator, error, field_name);
        return false;
    }
    
    return true;
}

int error_aggregator_validate_all(ErrorAggregator* aggregator,
                                ValidationFunction* validators,
                                const void** data_items,
                                int item_count) {
    if (!aggregator || !validators || !data_items || item_count <= 0) return 0;
    
    int successful_validations = 0;
    
    for (int i = 0; i < item_count; i++) {
        if (aggregator->collection_stopped) break;
        
        if (validators[i] && validators[i](data_items[i], aggregator)) {
            successful_validations++;
        }
    }
    
    return successful_validations;
}

// =============================================================================
// Error Recovery Integration
// =============================================================================

goo_error_union_t* error_aggregator_execute_with_recovery(ErrorAggregator* aggregator,
                                                        CollectableOperation operation,
                                                        void* args,
                                                        RetryAnnotation* retry,
                                                        CircuitBreakerAnnotation* circuit_breaker) {
    if (!aggregator || !operation) {
        return goo_error_union_new_error(goo_new_error("Invalid parameters"));
    }
    
    // Create a recovery context
    ErrorRecoveryContext* recovery_context = error_recovery_context_create(
        "error_aggregator_operation", __FILE__, __LINE__);
    
    if (!recovery_context) {
        return goo_error_union_new_error(goo_new_error("Failed to create recovery context"));
    }
    
    // Set recovery patterns
    if (retry) {
        error_recovery_context_set_retry(recovery_context, retry);
    }
    if (circuit_breaker) {
        error_recovery_context_set_circuit_breaker(recovery_context, circuit_breaker);
    }
    
    // Execute with recovery patterns
    goo_error_union_t* result = execute_with_recovery_patterns(recovery_context, operation, args);
    
    // Add result to collector
    error_aggregator_try(aggregator, result);
    
    // Clean up
    error_recovery_context_free(recovery_context);
    
    return result;
}

int error_aggregator_execute_batch_with_recovery(ErrorAggregator* aggregator,
                                               CollectableOperation* operations,
                                               void** args_array,
                                               int operation_count,
                                               RetryAnnotation* retry,
                                               CircuitBreakerAnnotation* circuit_breaker) {
    if (!aggregator || !operations || operation_count <= 0) return 0;
    
    int successful_count = 0;
    
    for (int i = 0; i < operation_count; i++) {
        if (aggregator->collection_stopped) break;
        
        void* args = args_array ? args_array[i] : NULL;
        
        goo_error_union_t* result = error_aggregator_execute_with_recovery(
            aggregator, operations[i], args, retry, circuit_breaker);
        
        if (goo_error_union_is_value(result)) {
            successful_count++;
        }
        
        goo_error_union_free(result);
    }
    
    return successful_count;
}