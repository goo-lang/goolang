#ifndef GOO_ERROR_RECOVERY_H
#define GOO_ERROR_RECOVERY_H

#include "error_context.h"
#include "runtime.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// =============================================================================
// Error Recovery Pattern Annotations System
// =============================================================================

// Forward declarations
typedef struct RetryAnnotation RetryAnnotation;
typedef struct CircuitBreakerAnnotation CircuitBreakerAnnotation;
typedef struct ErrorRecoveryContext ErrorRecoveryContext;

// =============================================================================
// Backoff Strategies
// =============================================================================

typedef enum {
    BACKOFF_CONSTANT,    // Fixed delay between retries
    BACKOFF_LINEAR,      // Linearly increasing delay
    BACKOFF_EXPONENTIAL, // Exponentially increasing delay
    BACKOFF_JITTERED     // Exponential with random jitter
} BackoffStrategy;

// Backoff configuration
typedef struct BackoffConfig {
    BackoffStrategy strategy;
    uint64_t initial_delay_ms;   // Initial delay in milliseconds
    uint64_t max_delay_ms;       // Maximum delay cap
    double multiplier;           // Backoff multiplier (for exponential/linear)
    double jitter_factor;        // Jitter amount (0.0-1.0) for randomization
    bool reset_on_success;       // Reset backoff on successful retry
} BackoffConfig;

// =============================================================================
// Retry Annotation
// =============================================================================

typedef struct RetryAnnotation {
    // Basic retry parameters
    int max_attempts;            // Maximum number of retry attempts
    BackoffConfig backoff;       // Backoff strategy configuration
    
    // Conditional retry logic
    bool (*should_retry)(const goo_error_t* error, int attempt, void* context);
    void* retry_context;         // User-provided context for retry decisions
    
    // Error filtering
    ErrorCode* retryable_errors; // Array of error codes that should trigger retry
    int retryable_error_count;   // Number of retryable error codes
    ErrorCode* fatal_errors;     // Array of error codes that should NOT be retried
    int fatal_error_count;       // Number of fatal error codes
    
    // Timeout settings
    uint64_t total_timeout_ms;   // Total timeout for all retry attempts
    uint64_t per_attempt_timeout_ms; // Timeout per individual attempt
    
    // Hooks
    void (*on_retry)(int attempt, const goo_error_t* error, void* context);
    void (*on_exhausted)(int attempts, const goo_error_t* last_error, void* context);
    void (*on_success)(int attempts, void* context);
    void* hook_context;
    
    // Runtime state (not part of annotation, managed by runtime)
    struct {
        int current_attempt;
        uint64_t start_time_ms;
        uint64_t last_attempt_time_ms;
        uint64_t current_delay_ms;
        goo_error_t* last_error;
        bool exhausted;
    } runtime_state;
    
} RetryAnnotation;

// =============================================================================
// Circuit Breaker Annotation
// =============================================================================

typedef enum {
    CIRCUIT_CLOSED,   // Normal operation, requests pass through
    CIRCUIT_OPEN,     // Circuit is open, requests fail immediately
    CIRCUIT_HALF_OPEN // Testing mode, limited requests allowed
} CircuitState;

typedef struct CircuitBreakerAnnotation {
    // Failure detection
    int failure_threshold;       // Number of failures before opening circuit
    uint64_t failure_window_ms;  // Time window for counting failures
    double failure_rate_threshold; // Failure rate threshold (0.0-1.0)
    
    // Recovery settings
    uint64_t open_timeout_ms;    // How long to keep circuit open
    int half_open_max_requests;  // Max requests allowed in half-open state
    int recovery_threshold;      // Successful requests needed to fully close
    
    // Error classification
    bool (*is_failure)(const goo_error_t* error, void* context);
    void* failure_context;
    ErrorCode* circuit_breaking_errors; // Errors that should open the circuit
    int circuit_breaking_error_count;
    
    // Hooks
    void (*on_state_change)(CircuitState old_state, CircuitState new_state, void* context);
    void (*on_circuit_open)(const char* reason, void* context);
    void (*on_circuit_close)(void* context);
    void* hook_context;
    
    // Runtime state (managed by runtime)
    struct {
        CircuitState state;
        uint64_t state_changed_time_ms;
        int failure_count;
        int success_count;
        int half_open_requests;
        uint64_t* failure_timestamps;
        int failure_timestamp_capacity;
        int failure_timestamp_count;
        char* circuit_id; // Unique identifier for the circuit
    } runtime_state;
    
} CircuitBreakerAnnotation;

// =============================================================================
// Composite Error Recovery Context
// =============================================================================

typedef struct ErrorRecoveryContext {
    bool has_retry;
    bool has_circuit_breaker;
    RetryAnnotation* retry;
    CircuitBreakerAnnotation* circuit_breaker;
    
    // Function metadata
    const char* function_name;
    const char* source_file;
    int source_line;
    
    // Integration with error context system
    ErrorStackFrame* error_frame;
    
    // Performance monitoring
    uint64_t total_execution_time_ms;
    uint64_t successful_calls;
    uint64_t failed_calls;
    uint64_t retried_calls;
    uint64_t circuit_breaker_trips;
    
} ErrorRecoveryContext;

// =============================================================================
// Annotation Parsing and Management
// =============================================================================

// Annotation parser for compiler integration
typedef struct AnnotationParser {
    const char* source_text;
    size_t position;
    size_t length;
    char* error_message;
} AnnotationParser;

// Create and destroy annotation parsers
AnnotationParser* annotation_parser_new(const char* source_text, size_t length);
void annotation_parser_free(AnnotationParser* parser);

// Parse specific annotation types
RetryAnnotation* parse_retry_annotation(AnnotationParser* parser);
CircuitBreakerAnnotation* parse_circuit_breaker_annotation(AnnotationParser* parser);

// Create default configurations
RetryAnnotation* retry_annotation_create_default(void);
CircuitBreakerAnnotation* circuit_breaker_annotation_create_default(void);

// Free annotation structures
void retry_annotation_free(RetryAnnotation* annotation);
void circuit_breaker_annotation_free(CircuitBreakerAnnotation* annotation);

// =============================================================================
// Runtime Support Functions
// =============================================================================

// Initialize the error recovery system
void error_recovery_system_init(void);
void error_recovery_system_shutdown(void);

// Create and manage recovery contexts
ErrorRecoveryContext* error_recovery_context_create(const char* function_name,
                                                   const char* source_file,
                                                   int source_line);
void error_recovery_context_free(ErrorRecoveryContext* context);

// Attach annotations to contexts
void error_recovery_context_set_retry(ErrorRecoveryContext* context, 
                                     RetryAnnotation* retry);
void error_recovery_context_set_circuit_breaker(ErrorRecoveryContext* context,
                                               CircuitBreakerAnnotation* circuit_breaker);

// =============================================================================
// Retry Mechanism Implementation
// =============================================================================

// Execute function with retry logic
typedef goo_error_union_t* (*RetryableFunction)(void* args);

goo_error_union_t* execute_with_retry(RetryAnnotation* retry,
                                     RetryableFunction func,
                                     void* args,
                                     ErrorRecoveryContext* context);

// Backoff delay calculation
uint64_t calculate_backoff_delay(const BackoffConfig* config, int attempt);
void apply_backoff_delay(uint64_t delay_ms);

// Retry decision logic
bool should_retry_error(const RetryAnnotation* retry, const goo_error_t* error, int attempt);
bool is_retry_timeout_exceeded(const RetryAnnotation* retry, uint64_t start_time_ms);

// =============================================================================
// Circuit Breaker Implementation
// =============================================================================

// Execute function with circuit breaker protection
goo_error_union_t* execute_with_circuit_breaker(CircuitBreakerAnnotation* circuit_breaker,
                                               RetryableFunction func,
                                               void* args,
                                               ErrorRecoveryContext* context);

// Circuit state management
void update_circuit_state(CircuitBreakerAnnotation* circuit_breaker,
                         bool success,
                         const goo_error_t* error);
bool is_circuit_open(const CircuitBreakerAnnotation* circuit_breaker);
bool can_execute_request(CircuitBreakerAnnotation* circuit_breaker);

// Failure tracking
void record_failure(CircuitBreakerAnnotation* circuit_breaker, const goo_error_t* error);
void record_success(CircuitBreakerAnnotation* circuit_breaker);
double calculate_failure_rate(const CircuitBreakerAnnotation* circuit_breaker);

// =============================================================================
// Combined Recovery Patterns
// =============================================================================

// Execute function with both retry and circuit breaker
goo_error_union_t* execute_with_recovery_patterns(ErrorRecoveryContext* context,
                                                 RetryableFunction func,
                                                 void* args);

// Integration with automatic error context
goo_error_union_t* execute_with_full_error_recovery(const char* function_name,
                                                   const char* source_file,
                                                   int source_line,
                                                   RetryAnnotation* retry,
                                                   CircuitBreakerAnnotation* circuit_breaker,
                                                   RetryableFunction func,
                                                   void* args);

// =============================================================================
// Compiler Integration Functions
// =============================================================================

// Function attribute analysis
bool function_has_retry_annotation(const char* function_source);
bool function_has_circuit_breaker_annotation(const char* function_source);

// Code generation helpers
char* generate_retry_wrapper_code(const RetryAnnotation* retry,
                                 const char* function_name,
                                 const char* function_signature);
char* generate_circuit_breaker_wrapper_code(const CircuitBreakerAnnotation* circuit_breaker,
                                           const char* function_name,
                                           const char* function_signature);

// AST transformation functions
struct ASTNode* transform_function_with_retry(struct ASTNode* function_node,
                                            RetryAnnotation* retry);
struct ASTNode* transform_function_with_circuit_breaker(struct ASTNode* function_node,
                                                      CircuitBreakerAnnotation* circuit_breaker);

// =============================================================================
// Statistics and Monitoring
// =============================================================================

typedef struct ErrorRecoveryStats {
    // Retry statistics
    uint64_t total_retry_attempts;
    uint64_t successful_retries;
    uint64_t exhausted_retries;
    uint64_t total_retry_delay_ms;
    
    // Circuit breaker statistics
    uint64_t circuit_breaker_trips;
    uint64_t circuit_recovery_events;
    uint64_t requests_blocked_by_circuit;
    uint64_t half_open_successes;
    uint64_t half_open_failures;
    
    // Performance metrics
    uint64_t total_execution_time_ms;
    double average_execution_time_ms;
    double success_rate;
    uint64_t total_function_calls;
    uint64_t total_successful_calls;
    
} ErrorRecoveryStats;

// Get global recovery statistics
ErrorRecoveryStats get_error_recovery_stats(void);
void reset_error_recovery_stats(void);
void print_error_recovery_stats(void);

// Per-function statistics
ErrorRecoveryStats get_function_recovery_stats(const char* function_name);

// =============================================================================
// Configuration and Customization
// =============================================================================

// Global configuration
typedef struct ErrorRecoveryConfig {
    bool enabled;
    bool enable_retry;
    bool enable_circuit_breaker;
    bool enable_statistics;
    bool enable_detailed_logging;
    
    // Default settings
    RetryAnnotation default_retry;
    CircuitBreakerAnnotation default_circuit_breaker;
    
    // Memory limits
    int max_concurrent_circuits;
    int max_failure_history_size;
    
} ErrorRecoveryConfig;

// Configure the error recovery system
void configure_error_recovery(const ErrorRecoveryConfig* config);
ErrorRecoveryConfig* get_error_recovery_config(void);

// =============================================================================
// Utility Macros
// =============================================================================

// Convenience macros for common retry patterns
#define RETRY_DEFAULT() retry_annotation_create_default()

#define RETRY_SIMPLE(attempts) ({ \
    RetryAnnotation* _retry = retry_annotation_create_default(); \
    _retry->max_attempts = (attempts); \
    _retry; \
})

#define RETRY_WITH_BACKOFF(attempts, strategy, initial_delay) ({ \
    RetryAnnotation* _retry = retry_annotation_create_default(); \
    _retry->max_attempts = (attempts); \
    _retry->backoff.strategy = (strategy); \
    _retry->backoff.initial_delay_ms = (initial_delay); \
    _retry; \
})

// Convenience macros for circuit breaker patterns
#define CIRCUIT_BREAKER_DEFAULT() circuit_breaker_annotation_create_default()

#define CIRCUIT_BREAKER_SIMPLE(threshold, timeout) ({ \
    CircuitBreakerAnnotation* _cb = circuit_breaker_annotation_create_default(); \
    _cb->failure_threshold = (threshold); \
    _cb->open_timeout_ms = (timeout); \
    _cb; \
})

// Combined pattern macro
#define WITH_ERROR_RECOVERY(func_name, retry_config, circuit_config, func, args) \
    execute_with_full_error_recovery(__func__, __FILE__, __LINE__, \
                                    (retry_config), (circuit_config), (func), (args))

#endif // GOO_ERROR_RECOVERY_H