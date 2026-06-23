#define _POSIX_C_SOURCE 200809L
#include "error_recovery.h"
#include "error_context.h"
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
// Global Error Recovery State
// =============================================================================

static ErrorRecoveryConfig g_recovery_config = {
    .enabled = true,
    .enable_retry = true,
    .enable_circuit_breaker = true,
    .enable_statistics = true,
    .enable_detailed_logging = false,
    .max_concurrent_circuits = 1000,
    .max_failure_history_size = 100
};

static ErrorRecoveryStats g_recovery_stats = {0};
static pthread_mutex_t g_recovery_mutex = PTHREAD_MUTEX_INITIALIZER;

// Global circuit breaker registry
static struct {
    CircuitBreakerAnnotation** circuits;
    int count;
    int capacity;
} g_circuit_registry = {0};

// =============================================================================
// Utility Functions
// =============================================================================

static uint64_t get_current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static double random_double(void) {
    return (double)rand() / RAND_MAX;
}

static void sleep_ms(uint64_t ms) {
    if (ms == 0) return;
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000
    };
    nanosleep(&ts, NULL);
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

// =============================================================================
// Error Recovery System Lifecycle
// =============================================================================

void error_recovery_system_init(void) {
    pthread_mutex_lock(&g_recovery_mutex);
    
    if (!g_recovery_config.enabled) {
        pthread_mutex_unlock(&g_recovery_mutex);
        return;
    }
    
    // Initialize circuit registry
    g_circuit_registry.capacity = g_recovery_config.max_concurrent_circuits;
    g_circuit_registry.circuits = calloc(g_circuit_registry.capacity, 
                                        sizeof(CircuitBreakerAnnotation*));
    g_circuit_registry.count = 0;
    
    // Reset statistics
    memset(&g_recovery_stats, 0, sizeof(g_recovery_stats));
    
    // Seed random number generator for jittered backoff
    srand(time(NULL));
    
    pthread_mutex_unlock(&g_recovery_mutex);
    
    printf("🔄 Error recovery system initialized\n");
}

void error_recovery_system_shutdown(void) {
    pthread_mutex_lock(&g_recovery_mutex);
    
    if (!g_recovery_config.enabled) {
        pthread_mutex_unlock(&g_recovery_mutex);
        return;
    }
    
    // Clean up circuit registry
    for (int i = 0; i < g_circuit_registry.count; i++) {
        circuit_breaker_annotation_free(g_circuit_registry.circuits[i]);
    }
    free(g_circuit_registry.circuits);
    g_circuit_registry.circuits = NULL;
    g_circuit_registry.count = 0;
    g_circuit_registry.capacity = 0;
    
    // Print final statistics
    print_error_recovery_stats();
    
    pthread_mutex_unlock(&g_recovery_mutex);
    
    printf("🔄 Error recovery system shutdown\n");
}

// =============================================================================
// Retry Annotation Implementation
// =============================================================================

RetryAnnotation* retry_annotation_create_default(void) {
    RetryAnnotation* retry = calloc(1, sizeof(RetryAnnotation));
    if (!retry) return NULL;
    
    // Default retry configuration
    retry->max_attempts = 3;
    retry->backoff.strategy = BACKOFF_EXPONENTIAL;
    retry->backoff.initial_delay_ms = 100;
    retry->backoff.max_delay_ms = 30000; // 30 seconds max
    retry->backoff.multiplier = 2.0;
    retry->backoff.jitter_factor = 0.1; // 10% jitter
    retry->backoff.reset_on_success = true;
    retry->total_timeout_ms = 300000; // 5 minutes total
    retry->per_attempt_timeout_ms = 30000; // 30 seconds per attempt
    
    // Initialize runtime state
    retry->runtime_state.current_attempt = 0;
    retry->runtime_state.current_delay_ms = retry->backoff.initial_delay_ms;
    retry->runtime_state.exhausted = false;
    
    return retry;
}

void retry_annotation_free(RetryAnnotation* annotation) {
    if (!annotation) return;
    
    free(annotation->retryable_errors);
    free(annotation->fatal_errors);
    if (annotation->runtime_state.last_error) {
        goo_error_free(annotation->runtime_state.last_error);
    }
    free(annotation);
}

uint64_t calculate_backoff_delay(const BackoffConfig* config, int attempt) {
    if (!config || attempt <= 0) return 0;
    
    uint64_t delay = config->initial_delay_ms;
    
    switch (config->strategy) {
        case BACKOFF_CONSTANT:
            delay = config->initial_delay_ms;
            break;
            
        case BACKOFF_LINEAR:
            delay = config->initial_delay_ms * attempt;
            break;
            
        case BACKOFF_EXPONENTIAL:
            delay = config->initial_delay_ms * pow(config->multiplier, attempt - 1);
            break;
            
        case BACKOFF_JITTERED: {
            // Exponential with jitter
            double base_delay = config->initial_delay_ms * pow(config->multiplier, attempt - 1);
            double jitter = base_delay * config->jitter_factor * (random_double() * 2.0 - 1.0);
            delay = (uint64_t)(base_delay + jitter);
            break;
        }
    }
    
    // Apply maximum delay cap
    if (delay > config->max_delay_ms) {
        delay = config->max_delay_ms;
    }
    
    return delay;
}

void apply_backoff_delay(uint64_t delay_ms) {
    if (delay_ms > 0) {
        if (g_recovery_config.enable_detailed_logging) {
            printf("⏳ Applying backoff delay: %lu ms\n", delay_ms);
        }
        sleep_ms(delay_ms);
    }
}

bool should_retry_error(const RetryAnnotation* retry, const goo_error_t* error, int attempt) {
    if (!retry || !error) return false;
    
    // Check if we've exceeded max attempts
    if (attempt >= retry->max_attempts) {
        return false;
    }
    
    // Check custom retry logic
    if (retry->should_retry) {
        return retry->should_retry(error, attempt, retry->retry_context);
    }
    
    // Check fatal errors list
    if (retry->fatal_errors && retry->fatal_error_count > 0) {
        for (int i = 0; i < retry->fatal_error_count; i++) {
            if ((ErrorCode)error->code == retry->fatal_errors[i]) {
                return false; // Don't retry fatal errors
            }
        }
    }
    
    // Check retryable errors list
    if (retry->retryable_errors && retry->retryable_error_count > 0) {
        for (int i = 0; i < retry->retryable_error_count; i++) {
            if ((ErrorCode)error->code == retry->retryable_errors[i]) {
                return true; // Retry this error
            }
        }
        return false; // Error not in retryable list
    }
    
    // Default: retry most errors except internal errors
    return error->code < 9000; // Don't retry internal errors by default
}

bool is_retry_timeout_exceeded(const RetryAnnotation* retry, uint64_t start_time_ms) {
    if (!retry || retry->total_timeout_ms == 0) return false;
    
    uint64_t elapsed = get_current_time_ms() - start_time_ms;
    return elapsed >= retry->total_timeout_ms;
}

goo_error_union_t* execute_with_retry(RetryAnnotation* retry,
                                     RetryableFunction func,
                                     void* args,
                                     ErrorRecoveryContext* context) {
    if (!retry || !func) {
        return goo_error_union_new_error(goo_new_error("Invalid retry configuration"));
    }
    
    if (!g_recovery_config.enable_retry) {
        // Retry disabled, execute once
        return func(args);
    }
    
    pthread_mutex_lock(&g_recovery_mutex);
    g_recovery_stats.total_retry_attempts++;
    pthread_mutex_unlock(&g_recovery_mutex);
    
    retry->runtime_state.start_time_ms = get_current_time_ms();
    retry->runtime_state.current_attempt = 0;
    retry->runtime_state.current_delay_ms = retry->backoff.initial_delay_ms;
    
    goo_error_union_t* result = NULL;
    goo_error_t* last_error = NULL;
    
    for (int attempt = 1; attempt <= retry->max_attempts; attempt++) {
        retry->runtime_state.current_attempt = attempt;
        
        // Check for total timeout
        if (is_retry_timeout_exceeded(retry, retry->runtime_state.start_time_ms)) {
            if (g_recovery_config.enable_detailed_logging) {
                printf("⏰ Retry timeout exceeded after %d attempts\n", attempt - 1);
            }
            break;
        }
        
        // Execute the function
        uint64_t attempt_start = get_current_time_ms();
        result = func(args);
        uint64_t attempt_end = get_current_time_ms();
        
        if (context) {
            context->total_execution_time_ms += (attempt_end - attempt_start);
        }
        
        // Check if the result is successful
        if (goo_error_union_is_value(result)) {
            // Success!
            if (retry->on_success) {
                retry->on_success(attempt, retry->hook_context);
            }
            
            if (context) {
                context->successful_calls++;
                if (attempt > 1) {
                    context->retried_calls++;
                    pthread_mutex_lock(&g_recovery_mutex);
                    g_recovery_stats.successful_retries++;
                    pthread_mutex_unlock(&g_recovery_mutex);
                }
            }
            
            if (g_recovery_config.enable_detailed_logging && attempt > 1) {
                printf("✅ Retry succeeded on attempt %d\n", attempt);
            }
            
            // Reset backoff on success if configured
            if (retry->backoff.reset_on_success) {
                retry->runtime_state.current_delay_ms = retry->backoff.initial_delay_ms;
            }
            
            return result;
        }
        
        // Extract error for analysis
        goo_error_t* error = goo_error_union_get_error(result);
        if (last_error) {
            goo_error_free(last_error);
        }
        last_error = goo_new_error_with_code(error->message, error->code);
        
        // Check if we should retry this error
        if (!should_retry_error(retry, error, attempt)) {
            if (g_recovery_config.enable_detailed_logging) {
                printf("❌ Error not retryable: %s (code: %d)\n", error->message, error->code);
            }
            break;
        }
        
        // Don't delay after the last attempt
        if (attempt < retry->max_attempts) {
            // Calculate and apply backoff delay
            uint64_t delay = calculate_backoff_delay(&retry->backoff, attempt);
            retry->runtime_state.current_delay_ms = delay;
            
            if (retry->on_retry) {
                retry->on_retry(attempt, error, retry->hook_context);
            }
            
            if (g_recovery_config.enable_detailed_logging) {
                printf("🔄 Retrying attempt %d/%d after %lu ms (error: %s)\n",
                       attempt + 1, retry->max_attempts, delay, error->message);
            }
            
            pthread_mutex_lock(&g_recovery_mutex);
            g_recovery_stats.total_retry_delay_ms += delay;
            pthread_mutex_unlock(&g_recovery_mutex);
            
            apply_backoff_delay(delay);
        }
        
        // Free the current result before next attempt
        goo_error_union_free(result);
        result = NULL;
    }
    
    // All retries exhausted
    retry->runtime_state.exhausted = true;
    
    if (retry->on_exhausted) {
        retry->on_exhausted(retry->runtime_state.current_attempt, last_error, retry->hook_context);
    }
    
    if (context) {
        context->failed_calls++;
    }
    
    pthread_mutex_lock(&g_recovery_mutex);
    g_recovery_stats.exhausted_retries++;
    pthread_mutex_unlock(&g_recovery_mutex);
    
    if (g_recovery_config.enable_detailed_logging) {
        printf("💥 Retry exhausted after %d attempts\n", retry->runtime_state.current_attempt);
    }
    
    // Return the last error
    if (last_error) {
        result = goo_error_union_new_error(last_error);
    } else {
        result = goo_error_union_new_error(goo_new_error("Retry exhausted"));
    }
    
    return result;
}

// =============================================================================
// Circuit Breaker Implementation
// =============================================================================

CircuitBreakerAnnotation* circuit_breaker_annotation_create_default(void) {
    CircuitBreakerAnnotation* cb = calloc(1, sizeof(CircuitBreakerAnnotation));
    if (!cb) return NULL;
    
    // Default circuit breaker configuration  
    cb->failure_threshold = 5;
    cb->failure_window_ms = 60000; // 1 minute
    cb->failure_rate_threshold = 0.5; // 50% failure rate
    cb->open_timeout_ms = 30000; // 30 seconds
    cb->half_open_max_requests = 3;
    cb->recovery_threshold = 2;
    
    // Initialize runtime state
    cb->runtime_state.state = CIRCUIT_CLOSED;
    cb->runtime_state.state_changed_time_ms = get_current_time_ms();
    cb->runtime_state.failure_count = 0;
    cb->runtime_state.success_count = 0;
    cb->runtime_state.half_open_requests = 0;
    cb->runtime_state.failure_timestamp_capacity = g_recovery_config.max_failure_history_size;
    cb->runtime_state.failure_timestamps = calloc(cb->runtime_state.failure_timestamp_capacity,
                                                  sizeof(uint64_t));
    cb->runtime_state.failure_timestamp_count = 0;
    
    // Generate unique circuit ID
    static int circuit_counter = 0;
    char* circuit_id = malloc(32);
    snprintf(circuit_id, 32, "circuit_%d_%lu", ++circuit_counter, get_current_time_ms());
    cb->runtime_state.circuit_id = circuit_id;
    
    return cb;
}

void circuit_breaker_annotation_free(CircuitBreakerAnnotation* annotation) {
    if (!annotation) return;
    
    free(annotation->circuit_breaking_errors);
    free(annotation->runtime_state.failure_timestamps);
    free(annotation->runtime_state.circuit_id);
    free(annotation);
}

bool is_circuit_open(const CircuitBreakerAnnotation* circuit_breaker) {
    return circuit_breaker && circuit_breaker->runtime_state.state == CIRCUIT_OPEN;
}

double calculate_failure_rate(const CircuitBreakerAnnotation* circuit_breaker) {
    if (!circuit_breaker || circuit_breaker->runtime_state.failure_timestamp_count == 0) {
        return 0.0;
    }
    
    uint64_t current_time = get_current_time_ms();
    uint64_t window_start = current_time - circuit_breaker->failure_window_ms;
    
    int recent_failures = 0;
    for (int i = 0; i < circuit_breaker->runtime_state.failure_timestamp_count; i++) {
        if (circuit_breaker->runtime_state.failure_timestamps[i] >= window_start) {
            recent_failures++;
        }
    }
    
    // Calculate failure rate based on recent failures and window
    int total_requests = recent_failures + circuit_breaker->runtime_state.success_count;
    if (total_requests == 0) return 0.0;
    
    return (double)recent_failures / total_requests;
}

void record_failure(CircuitBreakerAnnotation* circuit_breaker, const goo_error_t* error) {
    if (!circuit_breaker) return;
    
    // Check if this error should trigger circuit breaking
    if (circuit_breaker->is_failure && !circuit_breaker->is_failure(error, circuit_breaker->failure_context)) {
        return; // Not considered a failure by custom logic
    }
    
    if (circuit_breaker->circuit_breaking_errors && circuit_breaker->circuit_breaking_error_count > 0) {
        bool should_break = false;
        for (int i = 0; i < circuit_breaker->circuit_breaking_error_count; i++) {
            if ((ErrorCode)error->code == circuit_breaker->circuit_breaking_errors[i]) {
                should_break = true;
                break;
            }
        }
        if (!should_break) return;
    }
    
    uint64_t current_time = get_current_time_ms();
    
    // Add failure timestamp
    if (circuit_breaker->runtime_state.failure_timestamp_count < 
        circuit_breaker->runtime_state.failure_timestamp_capacity) {
        circuit_breaker->runtime_state.failure_timestamps[
            circuit_breaker->runtime_state.failure_timestamp_count++] = current_time;
    } else {
        // Shift timestamps and add new one
        memmove(circuit_breaker->runtime_state.failure_timestamps,
                circuit_breaker->runtime_state.failure_timestamps + 1,
                (circuit_breaker->runtime_state.failure_timestamp_capacity - 1) * sizeof(uint64_t));
        circuit_breaker->runtime_state.failure_timestamps[
            circuit_breaker->runtime_state.failure_timestamp_capacity - 1] = current_time;
    }
    
    circuit_breaker->runtime_state.failure_count++;
    
    if (g_recovery_config.enable_detailed_logging) {
        printf("🔴 Circuit breaker recorded failure: %s\n", error->message);
    }
}

void record_success(CircuitBreakerAnnotation* circuit_breaker) {
    if (!circuit_breaker) return;
    
    circuit_breaker->runtime_state.success_count++;
    
    if (g_recovery_config.enable_detailed_logging) {
        printf("🟢 Circuit breaker recorded success\n");
    }
}

void update_circuit_state(CircuitBreakerAnnotation* circuit_breaker,
                         bool success,
                         const goo_error_t* error) {
    if (!circuit_breaker) return;
    
    CircuitState old_state = circuit_breaker->runtime_state.state;
    CircuitState new_state = old_state;
    uint64_t current_time = get_current_time_ms();
    
    if (success) {
        record_success(circuit_breaker);
        
        switch (old_state) {
            case CIRCUIT_HALF_OPEN:
                if (circuit_breaker->runtime_state.success_count >= circuit_breaker->recovery_threshold) {
                    new_state = CIRCUIT_CLOSED;
                    circuit_breaker->runtime_state.failure_count = 0;
                    circuit_breaker->runtime_state.success_count = 0;
                    circuit_breaker->runtime_state.half_open_requests = 0;
                    
                    if (circuit_breaker->on_circuit_close) {
                        circuit_breaker->on_circuit_close(circuit_breaker->hook_context);
                    }
                    
                    if (g_recovery_config.enable_detailed_logging) {
                        printf("🔵 Circuit closed after successful recovery\n");
                    }
                }
                break;
                
            case CIRCUIT_OPEN:
                // Shouldn't happen, but reset if it does
                new_state = CIRCUIT_CLOSED;
                break;
                
            case CIRCUIT_CLOSED:
                // Stay closed
                break;
        }
    } else {
        record_failure(circuit_breaker, error);
        
        switch (old_state) {
            case CIRCUIT_CLOSED:
                // Check if we should open the circuit
                if (circuit_breaker->runtime_state.failure_count >= circuit_breaker->failure_threshold ||
                    calculate_failure_rate(circuit_breaker) >= circuit_breaker->failure_rate_threshold) {
                    
                    new_state = CIRCUIT_OPEN;
                    
                    if (circuit_breaker->on_circuit_open) {
                        circuit_breaker->on_circuit_open("Failure threshold exceeded", 
                                                        circuit_breaker->hook_context);
                    }
                    
                    pthread_mutex_lock(&g_recovery_mutex);
                    g_recovery_stats.circuit_breaker_trips++;
                    pthread_mutex_unlock(&g_recovery_mutex);
                    
                    if (g_recovery_config.enable_detailed_logging) {
                        printf("🔴 Circuit opened due to failures (count: %d, rate: %.2f)\n",
                               circuit_breaker->runtime_state.failure_count,
                               calculate_failure_rate(circuit_breaker));
                    }
                }
                break;
                
            case CIRCUIT_HALF_OPEN:
                // Failure in half-open state, go back to open
                new_state = CIRCUIT_OPEN;
                circuit_breaker->runtime_state.half_open_requests = 0;
                
                pthread_mutex_lock(&g_recovery_mutex);
                g_recovery_stats.half_open_failures++;
                pthread_mutex_unlock(&g_recovery_mutex);
                
                if (g_recovery_config.enable_detailed_logging) {
                    printf("🔴 Circuit reopened after half-open failure\n");
                }
                break;
                
            case CIRCUIT_OPEN:
                // Stay open
                break;
        }
    }
    
    // Check if we should transition from OPEN to HALF_OPEN
    if (old_state == CIRCUIT_OPEN && new_state == CIRCUIT_OPEN) {
        if (current_time - circuit_breaker->runtime_state.state_changed_time_ms >= 
            circuit_breaker->open_timeout_ms) {
            new_state = CIRCUIT_HALF_OPEN;
            circuit_breaker->runtime_state.half_open_requests = 0;
            circuit_breaker->runtime_state.success_count = 0;
            
            if (g_recovery_config.enable_detailed_logging) {
                printf("🟡 Circuit transitioning to half-open for testing\n");
            }
        }
    }
    
    // Update state if changed
    if (new_state != old_state) {
        circuit_breaker->runtime_state.state = new_state;
        circuit_breaker->runtime_state.state_changed_time_ms = current_time;
        
        if (circuit_breaker->on_state_change) {
            circuit_breaker->on_state_change(old_state, new_state, circuit_breaker->hook_context);
        }
    }
}

bool can_execute_request(CircuitBreakerAnnotation* circuit_breaker) {
    if (!circuit_breaker) return true;
    
    switch (circuit_breaker->runtime_state.state) {
        case CIRCUIT_CLOSED:
            return true;
            
        case CIRCUIT_OPEN:
            pthread_mutex_lock(&g_recovery_mutex);
            g_recovery_stats.requests_blocked_by_circuit++;
            pthread_mutex_unlock(&g_recovery_mutex);
            return false;
            
        case CIRCUIT_HALF_OPEN:
            if (circuit_breaker->runtime_state.half_open_requests < 
                circuit_breaker->half_open_max_requests) {
                circuit_breaker->runtime_state.half_open_requests++;
                return true;
            }
            pthread_mutex_lock(&g_recovery_mutex);
            g_recovery_stats.requests_blocked_by_circuit++;
            pthread_mutex_unlock(&g_recovery_mutex);
            return false;
    }
    
    return true;
}

goo_error_union_t* execute_with_circuit_breaker(CircuitBreakerAnnotation* circuit_breaker,
                                               RetryableFunction func,
                                               void* args,
                                               ErrorRecoveryContext* context) {
    if (!circuit_breaker || !func) {
        return goo_error_union_new_error(goo_new_error("Invalid circuit breaker configuration"));
    }
    
    if (!g_recovery_config.enable_circuit_breaker) {
        // Circuit breaker disabled, execute normally
        return func(args);
    }
    
    // Check if circuit allows execution
    if (!can_execute_request(circuit_breaker)) {
        if (context) {
            context->circuit_breaker_trips++;
        }
        
        char* error_msg = malloc(256);
        snprintf(error_msg, 256, "Circuit breaker is %s - request blocked",
                circuit_breaker->runtime_state.state == CIRCUIT_OPEN ? "open" : "half-open (saturated)");
        goo_error_t* circuit_error = goo_new_error(error_msg);
        free(error_msg);
        
        return goo_error_union_new_error(circuit_error);
    }
    
    // Execute the function
    uint64_t start_time = get_current_time_ms();
    goo_error_union_t* result = func(args);
    uint64_t end_time = get_current_time_ms();
    
    if (context) {
        context->total_execution_time_ms += (end_time - start_time);
    }
    
    // Update circuit state based on result
    if (goo_error_union_is_value(result)) {
        update_circuit_state(circuit_breaker, true, NULL);
        if (context) {
            context->successful_calls++;
        }
    } else {
        goo_error_t* error = goo_error_union_get_error(result);
        update_circuit_state(circuit_breaker, false, error);
        if (context) {
            context->failed_calls++;
        }
    }
    
    return result;
}

// =============================================================================
// Error Recovery Context Management
// =============================================================================

ErrorRecoveryContext* error_recovery_context_create(const char* function_name,
                                                   const char* source_file,
                                                   int source_line) {
    ErrorRecoveryContext* context = calloc(1, sizeof(ErrorRecoveryContext));
    if (!context) return NULL;
    
    context->function_name = duplicate_string(function_name);
    context->source_file = duplicate_string(source_file);
    context->source_line = source_line;
    
    return context;
}

void error_recovery_context_free(ErrorRecoveryContext* context) {
    if (!context) return;
    
    free((void*)context->function_name);
    free((void*)context->source_file);
    free(context);
}

void error_recovery_context_set_retry(ErrorRecoveryContext* context, 
                                     RetryAnnotation* retry) {
    if (!context) return;
    context->has_retry = true;
    context->retry = retry;
}

void error_recovery_context_set_circuit_breaker(ErrorRecoveryContext* context,
                                               CircuitBreakerAnnotation* circuit_breaker) {
    if (!context) return;
    context->has_circuit_breaker = true;
    context->circuit_breaker = circuit_breaker;
}

// =============================================================================
// Combined Recovery Patterns
// =============================================================================

// Packed arguments that let retry logic run inside a circuit breaker via a
// file-scope wrapper instead of a nested-function trampoline. A trampoline
// closes over locals on the stack, which forces an executable stack on the
// whole translation unit (and a linker warning in every compiled program).
typedef struct {
    ErrorRecoveryContext* context;
    RetryableFunction func;
    void* args;
} RetryWrapperArgs;

static goo_error_union_t* retry_wrapper_fn(void* packed) {
    RetryWrapperArgs* w = (RetryWrapperArgs*)packed;
    return execute_with_retry(w->context->retry, w->func, w->args, w->context);
}

goo_error_union_t* execute_with_recovery_patterns(ErrorRecoveryContext* context,
                                                 RetryableFunction func,
                                                 void* args) {
    if (!context || !func) {
        return goo_error_union_new_error(goo_new_error("Invalid recovery context"));
    }
    
    pthread_mutex_lock(&g_recovery_mutex);
    g_recovery_stats.total_function_calls++;
    pthread_mutex_unlock(&g_recovery_mutex);
    
    goo_error_union_t* result = NULL;
    
    if (context->has_circuit_breaker && context->has_retry) {
        // Both circuit breaker and retry - circuit breaker wraps retry.
        // Pack the retry parameters so retry_wrapper_fn (file scope) can apply
        // retry logic without a nested-function trampoline. The circuit breaker
        // forwards these args verbatim to the wrapped function.
        RetryWrapperArgs wrapper_args = { context, func, args };
        result = execute_with_circuit_breaker(context->circuit_breaker,
                                            retry_wrapper_fn, &wrapper_args, context);
    } else if (context->has_retry) {
        // Only retry
        result = execute_with_retry(context->retry, func, args, context);
    } else if (context->has_circuit_breaker) {
        // Only circuit breaker
        result = execute_with_circuit_breaker(context->circuit_breaker, func, args, context);
    } else {
        // No recovery patterns, execute normally
        result = func(args);
    }
    
    // Update global statistics
    if (goo_error_union_is_value(result)) {
        pthread_mutex_lock(&g_recovery_mutex);
        g_recovery_stats.total_successful_calls++;
        pthread_mutex_unlock(&g_recovery_mutex);
    }
    
    return result;
}

goo_error_union_t* execute_with_full_error_recovery(const char* function_name,
                                                   const char* source_file,
                                                   int source_line,
                                                   RetryAnnotation* retry,
                                                   CircuitBreakerAnnotation* circuit_breaker,
                                                   RetryableFunction func,
                                                   void* args) {
    ErrorRecoveryContext* context = error_recovery_context_create(function_name, 
                                                                 source_file, 
                                                                 source_line);
    if (!context) {
        return goo_error_union_new_error(goo_new_error("Failed to create recovery context"));
    }
    
    if (retry) {
        error_recovery_context_set_retry(context, retry);
    }
    
    if (circuit_breaker) {
        error_recovery_context_set_circuit_breaker(context, circuit_breaker);
    }
    
    goo_error_union_t* result = execute_with_recovery_patterns(context, func, args);
    
    error_recovery_context_free(context);
    return result;
}

// =============================================================================
// Statistics and Monitoring
// =============================================================================

ErrorRecoveryStats get_error_recovery_stats(void) {
    pthread_mutex_lock(&g_recovery_mutex);
    ErrorRecoveryStats stats = g_recovery_stats;
    
    // Calculate derived statistics
    if (stats.total_function_calls > 0) {
        stats.success_rate = (double)stats.total_successful_calls / stats.total_function_calls;
    }
    
    if (stats.total_function_calls > 0 && stats.total_execution_time_ms > 0) {
        stats.average_execution_time_ms = (double)stats.total_execution_time_ms / stats.total_function_calls;
    }
    
    pthread_mutex_unlock(&g_recovery_mutex);
    return stats;
}

void reset_error_recovery_stats(void) {
    pthread_mutex_lock(&g_recovery_mutex);
    memset(&g_recovery_stats, 0, sizeof(g_recovery_stats));
    pthread_mutex_unlock(&g_recovery_mutex);
}

void print_error_recovery_stats(void) {
    ErrorRecoveryStats stats = get_error_recovery_stats();
    
    printf("🔄 Error Recovery Statistics\n");
    printf("═══════════════════════════\n");
    printf("Function Calls:\n");
    printf("  Total:      %lu\n", stats.total_function_calls);
    printf("  Successful: %lu (%.1f%%)\n", stats.total_successful_calls, stats.success_rate * 100);
    printf("  Failed:     %lu\n", stats.total_function_calls - stats.total_successful_calls);
    printf("\nRetry Statistics:\n");
    printf("  Total Attempts:     %lu\n", stats.total_retry_attempts);
    printf("  Successful Retries: %lu\n", stats.successful_retries);
    printf("  Exhausted Retries:  %lu\n", stats.exhausted_retries);
    printf("  Total Delay:        %lu ms\n", stats.total_retry_delay_ms);
    printf("\nCircuit Breaker Statistics:\n");
    printf("  Circuit Trips:         %lu\n", stats.circuit_breaker_trips);
    printf("  Recovery Events:       %lu\n", stats.circuit_recovery_events);
    printf("  Blocked Requests:      %lu\n", stats.requests_blocked_by_circuit);
    printf("  Half-open Successes:   %lu\n", stats.half_open_successes);
    printf("  Half-open Failures:    %lu\n", stats.half_open_failures);
    printf("\nPerformance:\n");
    printf("  Average Execution Time: %.2f ms\n", stats.average_execution_time_ms);
    printf("═══════════════════════════\n");
}

// =============================================================================
// Configuration
// =============================================================================

void configure_error_recovery(const ErrorRecoveryConfig* config) {
    if (!config) return;
    
    pthread_mutex_lock(&g_recovery_mutex);
    g_recovery_config = *config;
    pthread_mutex_unlock(&g_recovery_mutex);
}

ErrorRecoveryConfig* get_error_recovery_config(void) {
    return &g_recovery_config;
}