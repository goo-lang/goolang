#ifndef GOO_ERROR_RECOVERY_H
#define GOO_ERROR_RECOVERY_H

#include "ergonomic_errors.h"
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// Recovery pattern types
typedef enum {
    RECOVERY_RETRY,           // Simple retry with backoff
    RECOVERY_CIRCUIT_BREAKER, // Circuit breaker pattern
    RECOVERY_TIMEOUT,         // Timeout-based recovery
    RECOVERY_FALLBACK,        // Fallback to alternative
    RECOVERY_BULKHEAD,        // Resource isolation
    RECOVERY_RATE_LIMIT,      // Rate limiting
    RECOVERY_CUSTOM           // Custom recovery logic
} RecoveryPatternType;

// Retry configuration
typedef struct {
    int max_attempts;
    double initial_delay_ms;
    double backoff_factor;
    double max_delay_ms;
    bool jitter_enabled;
    
    // Conditional retry
    bool (*should_retry)(const Error* error);
    
    // Custom delay calculation
    double (*calculate_delay)(int attempt, double base_delay);
} RetryConfig;

// Circuit breaker configuration
typedef struct {
    int failure_threshold;     // Number of failures to open circuit
    uint64_t timeout_ms;       // How long to keep circuit open
    uint64_t recovery_timeout_ms; // Time to try half-open state
    
    // Circuit breaker state
    enum {
        CIRCUIT_CLOSED,   // Normal operation
        CIRCUIT_OPEN,     // Failing fast
        CIRCUIT_HALF_OPEN // Testing recovery
    } state;
    
    int failure_count;
    uint64_t last_failure_time;
    uint64_t last_success_time;
    
    // Callbacks
    void (*on_open)(void* user_data);
    void (*on_close)(void* user_data);
    void (*on_half_open)(void* user_data);
    void* user_data;
} CircuitBreakerConfig;

// Timeout configuration
typedef struct {
    uint64_t timeout_ms;
    bool cancel_on_timeout;
    
    // Custom timeout handler
    Error* (*on_timeout)(uint64_t elapsed_ms, void* context);
    void* timeout_context;
} TimeoutConfig;

// Fallback configuration
typedef struct {
    // Fallback function that provides alternative result
    void* (*fallback_func)(const Error* original_error, void* context);
    void* fallback_context;
    
    // Error transformation for fallback
    Error* (*transform_error)(const Error* original, void* context);
    
    // Logging/monitoring
    void (*on_fallback)(const Error* original, void* result, void* context);
} FallbackConfig;

// Rate limiting configuration
typedef struct {
    int max_requests_per_window;
    uint64_t window_size_ms;
    uint64_t burst_size;
    
    // Current state
    int requests_in_window;
    uint64_t window_start_time;
    int burst_tokens;
    uint64_t last_token_refill;
    
    // Rate limit exceeded handler
    Error* (*on_rate_limit_exceeded)(void* context);
    void* context;
} RateLimitConfig;

// Combined recovery configuration
typedef struct {
    RecoveryPatternType pattern_type;
    
    union {
        RetryConfig retry;
        CircuitBreakerConfig circuit_breaker;
        TimeoutConfig timeout;
        FallbackConfig fallback;
        RateLimitConfig rate_limit;
    } config;
    
    // Monitoring and metrics
    bool enable_metrics;
    uint64_t total_attempts;
    uint64_t successful_recoveries;
    uint64_t failed_recoveries;
    uint64_t avg_recovery_time_ms;
} RecoveryConfig;

// Recovery context for tracking active recovery operations
typedef struct RecoveryContext {
    RecoveryConfig* config;
    ErgoErrorContext* error_context;
    
    // Current operation state
    int current_attempt;
    uint64_t operation_start_time;
    uint64_t last_attempt_time;
    
    // Error history
    Error** attempt_errors;
    size_t error_count;
    size_t error_capacity;
    
    // Recovery state
    bool recovery_in_progress;
    bool operation_cancelled;
    
    struct RecoveryContext* next; // For chaining multiple recovery patterns
} RecoveryContext;

// Recovery annotation system
typedef struct {
    const char* function_name;
    RecoveryConfig recovery_config;
    
    // Function pointer to original function
    void* original_function;
    
    // Wrapper function that applies recovery
    void* wrapped_function;
    
    // Statistics
    uint64_t total_calls;
    uint64_t successful_calls;
    uint64_t recovered_calls;
    uint64_t failed_calls;
} RecoveryAnnotation;

// Recovery registry for managing annotated functions
typedef struct {
    RecoveryAnnotation* annotations;
    size_t count;
    size_t capacity;
    
    // Global recovery settings
    bool recovery_enabled;
    int max_concurrent_recoveries;
    int current_recoveries;
    
    // Monitoring
    uint64_t total_recovery_time_ms;
    uint64_t successful_recoveries;
    uint64_t failed_recoveries;
} RecoveryRegistry;

// Recovery configuration builders
RecoveryConfig* recovery_config_retry(int max_attempts, double initial_delay_ms, double backoff_factor);
RecoveryConfig* recovery_config_circuit_breaker(int failure_threshold, uint64_t timeout_ms);
RecoveryConfig* recovery_config_timeout(uint64_t timeout_ms);
RecoveryConfig* recovery_config_fallback(void* (*fallback_func)(const Error*, void*), void* context);
RecoveryConfig* recovery_config_rate_limit(int max_requests, uint64_t window_ms);

// Recovery context management
RecoveryContext* recovery_context_new(RecoveryConfig* config, ErgoErrorContext* error_context);
void recovery_context_free(RecoveryContext* context);

// Recovery execution
typedef struct {
    union {
        int int_result;
        double double_result;
        void* ptr_result;
        bool bool_result;
    };
    bool is_error;
    Error* error;
    int attempts_made;
    uint64_t total_time_ms;
} RecoveryResult;

// Execute function with recovery patterns
RecoveryResult recovery_execute_int(RecoveryContext* context, 
                                   Result_int (*func)(void*), void* args);
RecoveryResult recovery_execute_ptr(RecoveryContext* context, 
                                   Result_void_ptr (*func)(void*), void* args);

// Annotation macros for easy use
#define RETRY_ANNOTATION(max_attempts, initial_delay, backoff) \
    __attribute__((annotate("retry:" #max_attempts ":" #initial_delay ":" #backoff)))

#define CIRCUIT_BREAKER_ANNOTATION(threshold, timeout) \
    __attribute__((annotate("circuit_breaker:" #threshold ":" #timeout)))

#define TIMEOUT_ANNOTATION(timeout_ms) \
    __attribute__((annotate("timeout:" #timeout_ms)))

#define FALLBACK_ANNOTATION(fallback_func) \
    __attribute__((annotate("fallback:" #fallback_func)))

// Recovery pattern implementations
bool retry_should_retry_default(const Error* error);
double retry_calculate_delay_exponential(int attempt, double base_delay);
double retry_calculate_delay_linear(int attempt, double base_delay);
double retry_calculate_delay_fibonacci(int attempt, double base_delay);

// Circuit breaker operations
void circuit_breaker_record_success(CircuitBreakerConfig* config);
void circuit_breaker_record_failure(CircuitBreakerConfig* config);
bool circuit_breaker_should_allow_request(CircuitBreakerConfig* config);
void circuit_breaker_reset(CircuitBreakerConfig* config);

// Rate limiting operations
bool rate_limiter_allow_request(RateLimitConfig* config);
void rate_limiter_reset(RateLimitConfig* config);
uint64_t rate_limiter_get_reset_time(const RateLimitConfig* config);

// Recovery registry management
RecoveryRegistry* recovery_registry_new(void);
void recovery_registry_free(RecoveryRegistry* registry);

// Register function with recovery pattern
void recovery_registry_register(RecoveryRegistry* registry, 
                               const char* function_name,
                               RecoveryConfig* config,
                               void* original_function);

// Get recovery configuration for function
RecoveryConfig* recovery_registry_get_config(RecoveryRegistry* registry, 
                                           const char* function_name);

// Recovery metrics and monitoring
typedef struct {
    uint64_t total_operations;
    uint64_t successful_operations;
    uint64_t recovered_operations;
    uint64_t failed_operations;
    
    double success_rate;
    double recovery_rate;
    uint64_t avg_response_time_ms;
    uint64_t avg_recovery_time_ms;
    
    // Pattern-specific metrics
    struct {
        uint64_t total_retries;
        uint64_t successful_retries;
        double avg_attempts_per_operation;
    } retry_metrics;
    
    struct {
        uint64_t circuit_opens;
        uint64_t circuit_closes;
        uint64_t requests_blocked;
        uint64_t current_circuit_state; // 0=closed, 1=open, 2=half-open
    } circuit_breaker_metrics;
    
    struct {
        uint64_t timeouts;
        uint64_t avg_timeout_recovery_ms;
    } timeout_metrics;
    
    struct {
        uint64_t fallback_activations;
        uint64_t fallback_successes;
    } fallback_metrics;
    
    struct {
        uint64_t requests_rate_limited;
        double current_request_rate;
    } rate_limit_metrics;
} RecoveryMetrics;

RecoveryMetrics recovery_get_metrics(const RecoveryRegistry* registry);
void recovery_reset_metrics(RecoveryRegistry* registry);

// Advanced recovery patterns
typedef struct {
    RecoveryConfig* primary_config;
    RecoveryConfig* secondary_config;
    
    // Escalation rules
    int primary_max_attempts;
    bool escalate_on_timeout;
    bool escalate_on_circuit_open;
    
    // Current state
    bool using_secondary;
    int primary_failures;
} EscalatingRecoveryConfig;

// Composite recovery pattern (chain multiple patterns)
typedef struct {
    RecoveryConfig** patterns;
    size_t pattern_count;
    
    // Execution strategy
    enum {
        COMPOSITE_SEQUENTIAL,  // Try patterns in order
        COMPOSITE_PARALLEL,    // Try all patterns simultaneously
        COMPOSITE_FASTEST,     // Use whichever responds first
        COMPOSITE_CONSENSUS    // Require multiple patterns to agree
    } strategy;
    
    // Consensus requirements (for COMPOSITE_CONSENSUS)
    int required_successes;
    double consensus_timeout_ms;
} CompositeRecoveryConfig;

// Global recovery settings
extern RecoveryRegistry* global_recovery_registry;

// Initialization
void recovery_system_init(void);
void recovery_system_shutdown(void);

// Convenience macros for common patterns
#define WITH_RETRY(max_attempts, func, args) \
    do { \
        RecoveryConfig* __config = recovery_config_retry(max_attempts, 100.0, 2.0); \
        RecoveryContext* __ctx = recovery_context_new(__config, NULL); \
        RecoveryResult __result = recovery_execute_ptr(__ctx, func, args); \
        recovery_context_free(__ctx); \
        free(__config); \
        if (__result.is_error) return ERR_PTR(__result.error); \
        return OK_PTR(__result.ptr_result); \
    } while(0)

#define WITH_CIRCUIT_BREAKER(threshold, timeout, func, args) \
    do { \
        RecoveryConfig* __config = recovery_config_circuit_breaker(threshold, timeout); \
        RecoveryContext* __ctx = recovery_context_new(__config, NULL); \
        RecoveryResult __result = recovery_execute_ptr(__ctx, func, args); \
        recovery_context_free(__ctx); \
        free(__config); \
        if (__result.is_error) return ERR_PTR(__result.error); \
        return OK_PTR(__result.ptr_result); \
    } while(0)

#define WITH_TIMEOUT(timeout_ms, func, args) \
    do { \
        RecoveryConfig* __config = recovery_config_timeout(timeout_ms); \
        RecoveryContext* __ctx = recovery_context_new(__config, NULL); \
        RecoveryResult __result = recovery_execute_ptr(__ctx, func, args); \
        recovery_context_free(__ctx); \
        free(__config); \
        if (__result.is_error) return ERR_PTR(__result.error); \
        return OK_PTR(__result.ptr_result); \
    } while(0)

#endif // GOO_ERROR_RECOVERY_H