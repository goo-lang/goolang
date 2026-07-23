#include "error_recovery.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

// Global recovery registry
RecoveryRegistry* global_recovery_registry = NULL;

// Utility function to get current time in milliseconds
static uint64_t get_current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// Recovery configuration builders
RecoveryConfig* recovery_config_retry(int max_attempts, double initial_delay_ms, double backoff_factor) {
    RecoveryConfig* config = xcalloc(1, sizeof(RecoveryConfig));
    if (!config) return NULL;
    
    config->pattern_type = RECOVERY_RETRY;
    config->config.retry = (RetryConfig){
        .max_attempts = max_attempts,
        .initial_delay_ms = initial_delay_ms,
        .backoff_factor = backoff_factor,
        .max_delay_ms = 30000.0, // 30 seconds max
        .jitter_enabled = true,
        .should_retry = retry_should_retry_default,
        .calculate_delay = retry_calculate_delay_exponential
    };
    config->enable_metrics = true;
    
    return config;
}

RecoveryConfig* recovery_config_circuit_breaker(int failure_threshold, uint64_t timeout_ms) {
    RecoveryConfig* config = xcalloc(1, sizeof(RecoveryConfig));
    if (!config) return NULL;
    
    config->pattern_type = RECOVERY_CIRCUIT_BREAKER;
    config->config.circuit_breaker = (CircuitBreakerConfig){
        .failure_threshold = failure_threshold,
        .timeout_ms = timeout_ms,
        .recovery_timeout_ms = timeout_ms / 2,
        .state = CIRCUIT_CLOSED,
        .failure_count = 0,
        .last_failure_time = 0,
        .last_success_time = 0,
        .on_open = NULL,
        .on_close = NULL,
        .on_half_open = NULL,
        .user_data = NULL
    };
    config->enable_metrics = true;
    
    return config;
}

RecoveryConfig* recovery_config_timeout(uint64_t timeout_ms) {
    RecoveryConfig* config = xcalloc(1, sizeof(RecoveryConfig));
    if (!config) return NULL;
    
    config->pattern_type = RECOVERY_TIMEOUT;
    config->config.timeout = (TimeoutConfig){
        .timeout_ms = timeout_ms,
        .cancel_on_timeout = true,
        .on_timeout = NULL,
        .timeout_context = NULL
    };
    config->enable_metrics = true;
    
    return config;
}

RecoveryConfig* recovery_config_fallback(void* (*fallback_func)(const Error*, void*), void* context) {
    RecoveryConfig* config = xcalloc(1, sizeof(RecoveryConfig));
    if (!config) return NULL;
    
    config->pattern_type = RECOVERY_FALLBACK;
    config->config.fallback = (FallbackConfig){
        .fallback_func = fallback_func,
        .fallback_context = context,
        .transform_error = NULL,
        .on_fallback = NULL
    };
    config->enable_metrics = true;
    
    return config;
}

RecoveryConfig* recovery_config_rate_limit(int max_requests, uint64_t window_ms) {
    RecoveryConfig* config = xcalloc(1, sizeof(RecoveryConfig));
    if (!config) return NULL;
    
    config->pattern_type = RECOVERY_RATE_LIMIT;
    config->config.rate_limit = (RateLimitConfig){
        .max_requests_per_window = max_requests,
        .window_size_ms = window_ms,
        .burst_size = max_requests,
        .requests_in_window = 0,
        .window_start_time = get_current_time_ms(),
        .burst_tokens = max_requests,
        .last_token_refill = get_current_time_ms(),
        .on_rate_limit_exceeded = NULL,
        .context = NULL
    };
    config->enable_metrics = true;
    
    return config;
}

// Recovery context management
RecoveryContext* recovery_context_new(RecoveryConfig* config, ErgoErrorContext* error_context) {
    RecoveryContext* context = xcalloc(1, sizeof(RecoveryContext));
    if (!context) return NULL;
    
    context->config = config;
    context->error_context = error_context;
    context->current_attempt = 0;
    context->operation_start_time = get_current_time_ms();
    context->last_attempt_time = 0;
    
    // Initialize error history
    context->error_capacity = 10;
    context->attempt_errors = malloc(sizeof(Error*) * context->error_capacity);
    if (!context->attempt_errors) {
        free(context);
        return NULL;
    }
    
    context->recovery_in_progress = false;
    context->operation_cancelled = false;
    context->next = NULL;
    
    return context;
}

void recovery_context_free(RecoveryContext* context) {
    if (!context) return;
    
    // Free error history
    if (context->attempt_errors) {
        for (size_t i = 0; i < context->error_count; i++) {
            if (context->attempt_errors[i]) {
                free((void*)context->attempt_errors[i]->message);
                free((void*)context->attempt_errors[i]->hint);
                free(context->attempt_errors[i]);
            }
        }
        free(context->attempt_errors);
    }
    
    free(context);
}

// Default retry logic
bool retry_should_retry_default(const Error* error) {
    if (!error) return false;
    
    // Don't retry fatal errors or certain types
    if (error->severity == ERROR_SEVERITY_FATAL) return false;
    
    switch (error->code) {
        case ERROR_OUT_OF_MEMORY:
        case ERROR_STACK_OVERFLOW:
        case ERROR_INVALID_CAST:
            return false; // Don't retry these
        
        case ERROR_CODEGEN_FAILED:
        case ERROR_LLVM_ERROR:
        case ERROR_TYPE_MISMATCH:
            return true; // Might be transient
        
        default:
            return true; // Retry by default
    }
}

// Delay calculation strategies
double retry_calculate_delay_exponential(int attempt, double base_delay) {
    if (attempt <= 0) return base_delay;
    
    double delay = base_delay * pow(2.0, attempt - 1);
    
    // Add jitter (±25%)
    double jitter = ((double)rand() / RAND_MAX - 0.5) * 0.5;
    delay *= (1.0 + jitter);
    
    return delay;
}

double retry_calculate_delay_linear(int attempt, double base_delay) {
    return base_delay * attempt;
}

double retry_calculate_delay_fibonacci(int attempt, double base_delay) {
    if (attempt <= 1) return base_delay;
    
    int fib_a = 1, fib_b = 1;
    for (int i = 2; i < attempt; i++) {
        int temp = fib_b;
        fib_b = fib_a + fib_b;
        fib_a = temp;
    }
    
    return base_delay * fib_b;
}

// Circuit breaker operations
void circuit_breaker_record_success(CircuitBreakerConfig* config) {
    if (!config) return;
    
    config->last_success_time = get_current_time_ms();
    
    if (config->state == CIRCUIT_HALF_OPEN) {
        // Successful call in half-open state closes the circuit
        config->state = CIRCUIT_CLOSED;
        config->failure_count = 0;
        
        if (config->on_close) {
            config->on_close(config->user_data);
        }
    } else if (config->state == CIRCUIT_CLOSED) {
        // Reset failure count on success
        config->failure_count = 0;
    }
}

void circuit_breaker_record_failure(CircuitBreakerConfig* config) {
    if (!config) return;
    
    config->last_failure_time = get_current_time_ms();
    config->failure_count++;
    
    if (config->state == CIRCUIT_CLOSED && 
        config->failure_count >= config->failure_threshold) {
        // Open the circuit
        config->state = CIRCUIT_OPEN;
        
        if (config->on_open) {
            config->on_open(config->user_data);
        }
    } else if (config->state == CIRCUIT_HALF_OPEN) {
        // Failure in half-open state opens the circuit again
        config->state = CIRCUIT_OPEN;
        
        if (config->on_open) {
            config->on_open(config->user_data);
        }
    }
}

bool circuit_breaker_should_allow_request(CircuitBreakerConfig* config) {
    if (!config) return true;
    
    uint64_t current_time = get_current_time_ms();
    
    switch (config->state) {
        case CIRCUIT_CLOSED:
            return true;
        
        case CIRCUIT_OPEN:
            // Check if timeout has passed
            if (current_time - config->last_failure_time >= config->timeout_ms) {
                // Move to half-open state
                config->state = CIRCUIT_HALF_OPEN;
                
                if (config->on_half_open) {
                    config->on_half_open(config->user_data);
                }
                
                return true;
            }
            return false;
        
        case CIRCUIT_HALF_OPEN:
            return true;
        
        default:
            return false;
    }
}

void circuit_breaker_reset(CircuitBreakerConfig* config) {
    if (!config) return;
    
    config->state = CIRCUIT_CLOSED;
    config->failure_count = 0;
    config->last_failure_time = 0;
    config->last_success_time = 0;
}

// Rate limiting operations
bool rate_limiter_allow_request(RateLimitConfig* config) {
    if (!config) return true;
    
    uint64_t current_time = get_current_time_ms();
    
    // Check if we need to reset the window
    if (current_time - config->window_start_time >= config->window_size_ms) {
        config->window_start_time = current_time;
        config->requests_in_window = 0;
    }
    
    // Refill burst tokens
    uint64_t time_since_refill = current_time - config->last_token_refill;
    if (time_since_refill >= 1000) { // Refill every second
        int tokens_to_add = (int)(time_since_refill / 1000);
        config->burst_tokens += tokens_to_add;
        if (config->burst_tokens > (int)config->burst_size) {
            config->burst_tokens = config->burst_size;
        }
        config->last_token_refill = current_time;
    }
    
    // Check rate limit
    if (config->requests_in_window < config->max_requests_per_window && 
        config->burst_tokens > 0) {
        config->requests_in_window++;
        config->burst_tokens--;
        return true;
    }
    
    return false;
}

void rate_limiter_reset(RateLimitConfig* config) {
    if (!config) return;
    
    config->requests_in_window = 0;
    config->window_start_time = get_current_time_ms();
    config->burst_tokens = config->burst_size;
    config->last_token_refill = get_current_time_ms();
}

uint64_t rate_limiter_get_reset_time(const RateLimitConfig* config) {
    if (!config) return 0;
    
    uint64_t current_time = get_current_time_ms();
    uint64_t window_end = config->window_start_time + config->window_size_ms;
    
    return (window_end > current_time) ? (window_end - current_time) : 0;
}

// Recovery execution for integer results
RecoveryResult recovery_execute_int(RecoveryContext* context, 
                                   Result_int (*func)(void*), void* args) {
    RecoveryResult result = {0};
    
    if (!context || !func) {
        result.is_error = true;
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid recovery context or function"),
            .hint = NULL,
            .location = empty_source_location(),
            .next = NULL
        };
        result.error = error;
        return result;
    }
    
    uint64_t start_time = get_current_time_ms();
    context->recovery_in_progress = true;
    
    for (int attempt = 1; attempt <= context->config->config.retry.max_attempts; attempt++) {
        context->current_attempt = attempt;
        context->last_attempt_time = get_current_time_ms();
        
        // Execute the function
        Result_int func_result = func(args);
        
        if (!func_result.is_error) {
            // Success!
            result.is_error = false;
            result.int_result = func_result.value;
            result.attempts_made = attempt;
            result.total_time_ms = get_current_time_ms() - start_time;
            
            context->recovery_in_progress = false;
            context->config->successful_recoveries++;
            
            return result;
        }
        
        // Store error for history
        if (context->error_count < context->error_capacity) {
            Error* error_copy = xmalloc(sizeof(Error));
            *error_copy = *func_result.error;
            error_copy->message = func_result.error->message ? strdup(func_result.error->message) : NULL;
            error_copy->hint = func_result.error->hint ? strdup(func_result.error->hint) : NULL;
            error_copy->next = NULL;
            
            context->attempt_errors[context->error_count++] = error_copy;
        }
        
        // Check if we should retry
        if (attempt < context->config->config.retry.max_attempts) {
            if (context->config->config.retry.should_retry(func_result.error)) {
                // Calculate delay
                double delay = context->config->config.retry.calculate_delay(
                    attempt, context->config->config.retry.initial_delay_ms);
                
                if (delay > context->config->config.retry.max_delay_ms) {
                    delay = context->config->config.retry.max_delay_ms;
                }
                
                // Sleep for delay
                usleep((useconds_t)(delay * 1000)); // Convert ms to microseconds
            } else {
                // Error not retryable
                break;
            }
        }
    }
    
    // All attempts failed
    result.is_error = true;
    result.attempts_made = context->current_attempt;
    result.total_time_ms = get_current_time_ms() - start_time;
    
    // Create aggregated error
    Error* aggregated_error = xmalloc(sizeof(Error));
    *aggregated_error = (Error){
        .code = ERROR_INTERNAL,
        .severity = ERROR_SEVERITY_ERROR,
        .category = ERROR_CATEGORY_INTERNAL,
        .location = empty_source_location(),
        .next = NULL
    };
    
    char* message = malloc(512);
    snprintf(message, 512, "Operation failed after %d attempts", context->current_attempt);
    aggregated_error->message = message;
    aggregated_error->hint = strdup("Review individual attempt errors for details");
    
    result.error = aggregated_error;
    
    context->recovery_in_progress = false;
    context->config->failed_recoveries++;
    
    return result;
}

// Recovery registry management
RecoveryRegistry* recovery_registry_new(void) {
    RecoveryRegistry* registry = xcalloc(1, sizeof(RecoveryRegistry));
    if (!registry) return NULL;
    
    registry->capacity = 100;
    registry->annotations = malloc(sizeof(RecoveryAnnotation) * registry->capacity);
    if (!registry->annotations) {
        free(registry);
        return NULL;
    }
    
    registry->recovery_enabled = true;
    registry->max_concurrent_recoveries = 10;
    registry->current_recoveries = 0;
    
    return registry;
}

void recovery_registry_free(RecoveryRegistry* registry) {
    if (!registry) return;
    
    if (registry->annotations) {
        for (size_t i = 0; i < registry->count; i++) {
            free((void*)registry->annotations[i].function_name);
        }
        free(registry->annotations);
    }
    
    free(registry);
}

// System initialization
void recovery_system_init(void) {
    if (!global_recovery_registry) {
        global_recovery_registry = recovery_registry_new();
    }
}

void recovery_system_shutdown(void) {
    if (global_recovery_registry) {
        recovery_registry_free(global_recovery_registry);
        global_recovery_registry = NULL;
    }
}