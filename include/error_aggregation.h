#ifndef GOO_ERROR_AGGREGATION_H
#define GOO_ERROR_AGGREGATION_H

#include "error_context.h"
#include "error_recovery.h"
#include "runtime.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

// =============================================================================
// Error Aggregation System
// =============================================================================

// Forward declarations
typedef struct ErrorAggregator ErrorAggregator;
typedef struct CompositeError CompositeError;
typedef struct ErrorGroup ErrorGroup;
typedef struct ParallelErrorAggregator ParallelErrorAggregator;

// =============================================================================
// Composite Error - Container for Multiple Errors
// =============================================================================

typedef struct CompositeError {
    // Basic error information
    const char* message;           // Summary message for the composite error
    int error_count;               // Number of individual errors collected
    goo_error_t** errors;          // Array of individual errors
    int error_capacity;            // Capacity of the errors array
    
    // Error categorization
    struct {
        int fatal_errors;          // Number of fatal errors
        int warning_errors;        // Number of warnings
        int validation_errors;     // Number of validation errors
        int network_errors;        // Number of network errors
        int file_errors;           // Number of file I/O errors
    } error_counts;
    
    // Context information
    const char* operation_name;    // Name of the operation that failed
    const char* source_file;       // Source file where aggregation started
    int source_line;               // Source line where aggregation started  
    
    // Metadata
    uint64_t created_time_ms;      // When the composite error was created
    uint64_t first_error_time_ms;  // When the first error was collected
    uint64_t last_error_time_ms;   // When the last error was collected
    
    // Memory management
    bool owns_errors;              // Whether this composite error owns the individual errors
    void (*destructor)(CompositeError* composite);  // Custom destructor
    
} CompositeError;

// =============================================================================
// Error Aggregator - Accumulates Multiple Errors
// =============================================================================

typedef enum {
    AGGREGATOR_CONTINUE,    // Continue collecting errors after failure
    AGGREGATOR_FAIL_FAST,   // Stop on first error (traditional behavior)
    AGGREGATOR_WARN_ONLY    // Collect only warnings, treat errors as fatal
} AggregatorMode;

typedef struct ErrorAggregator {
    // Configuration
    AggregatorMode mode;           // How the aggregator should behave
    int max_errors;                // Maximum number of errors to collect (0 = unlimited)
    bool collect_warnings;         // Whether to collect warnings
    bool collect_context;          // Whether to collect error context
    
    // State
    CompositeError* composite;     // The composite error being built
    int successful_operations;     // Number of successful operations
    int total_operations;          // Total number of operations attempted
    bool has_fatal_errors;         // Whether any fatal errors were collected
    bool collection_stopped;       // Whether collection was stopped due to limits
    
    // Thread safety
    pthread_mutex_t mutex;         // Mutex for thread-safe operation
    bool thread_safe;              // Whether this aggregator is thread-safe
    
    // Hooks and callbacks
    void (*on_error_collected)(ErrorAggregator* aggregator, const goo_error_t* error, void* context);
    void (*on_limit_reached)(ErrorAggregator* aggregator, int limit, void* context);
    void (*on_collection_finished)(ErrorAggregator* aggregator, bool success, void* context);
    void* hook_context;            // Context passed to hooks
    
    // Integration with error recovery
    RetryAnnotation* retry_config; // Optional retry configuration for operations
    CircuitBreakerAnnotation* circuit_breaker; // Optional circuit breaker
    
} ErrorAggregator;

// =============================================================================
// Error Group - Logical Grouping of Related Errors
// =============================================================================

typedef struct ErrorGroup {
    const char* group_name;        // Name of this error group (e.g., "validation", "network")
    const char* description;       // Human-readable description
    goo_error_t** errors;          // Errors in this group
    int error_count;               // Number of errors in this group
    int error_capacity;            // Capacity of errors array
    
    // Group metadata
    ErrorCategory category;        // Primary category for this group
    int priority;                  // Priority level (higher = more important)
    bool is_fatal;                 // Whether any error in this group is fatal
    
    // Statistics
    uint64_t first_error_time_ms;  // Time of first error in group
    uint64_t last_error_time_ms;   // Time of last error in group
    
} ErrorGroup;

// =============================================================================
// Parallel Error Aggregator - Thread-Safe Multi-Producer Collection
// =============================================================================

typedef struct ParallelErrorAggregator {
    ErrorAggregator* base_aggregator; // Base aggregator (always thread-safe)
    
    // Parallel execution support
    int active_threads;            // Number of active worker threads
    int max_threads;               // Maximum number of concurrent threads
    pthread_cond_t completion_cond; // Condition variable for completion
    
    // Work distribution
    struct {
        void** work_items;         // Array of work items to process
        int total_items;           // Total number of work items
        int current_item;          // Current work item index
        int completed_items;       // Number of completed items
        pthread_mutex_t work_mutex; // Mutex for work distribution
    } work_queue;
    
    // Results aggregation
    struct {
        void** results;            // Array of operation results
        bool* success_flags;       // Success flag for each operation
        pthread_mutex_t results_mutex; // Mutex for results access
    } results;
    
} ParallelErrorAggregator;

// =============================================================================
// Error Aggregator Lifecycle
// =============================================================================

// Create and configure error aggregators
ErrorAggregator* error_aggregator_new(AggregatorMode mode);
ErrorAggregator* error_aggregator_new_with_config(AggregatorMode mode, 
                                                  int max_errors, 
                                                  bool thread_safe);
void error_aggregator_free(ErrorAggregator* aggregator);

// Configure aggregator behavior
void error_aggregator_set_max_errors(ErrorAggregator* aggregator, int max_errors);
void error_aggregator_set_collect_warnings(ErrorAggregator* aggregator, bool collect);
void error_aggregator_set_collect_context(ErrorAggregator* aggregator, bool collect);
void error_aggregator_set_operation_name(ErrorAggregator* aggregator, const char* name);

// Set hooks and callbacks
void error_aggregator_set_error_hook(ErrorAggregator* aggregator,
                                     void (*hook)(ErrorAggregator*, const goo_error_t*, void*),
                                     void* context);
void error_aggregator_set_limit_hook(ErrorAggregator* aggregator,
                                     void (*hook)(ErrorAggregator*, int, void*),
                                     void* context);
void error_aggregator_set_completion_hook(ErrorAggregator* aggregator,
                                          void (*hook)(ErrorAggregator*, bool, void*),
                                          void* context);

// Attach error recovery patterns
void error_aggregator_set_retry(ErrorAggregator* aggregator, RetryAnnotation* retry);
void error_aggregator_set_circuit_breaker(ErrorAggregator* aggregator, 
                                          CircuitBreakerAnnotation* circuit_breaker);

// =============================================================================
// Error Collection Operations
// =============================================================================

// Core collection methods
typedef goo_error_union_t* (*CollectableOperation)(void* args);

bool error_aggregator_try(ErrorAggregator* aggregator, 
                          goo_error_union_t* result);
bool error_aggregator_try_operation(ErrorAggregator* aggregator,
                                    CollectableOperation operation,
                                    void* args);
bool error_aggregator_try_with_name(ErrorAggregator* aggregator,
                                    goo_error_union_t* result,
                                    const char* operation_name);

// Batch operations
int error_aggregator_try_batch(ErrorAggregator* aggregator,
                               CollectableOperation* operations,
                               void** args_array,
                               int operation_count);
int error_aggregator_try_batch_named(ErrorAggregator* aggregator,
                                     CollectableOperation* operations,
                                     void** args_array,
                                     const char** operation_names,
                                     int operation_count);

// Direct error collection
void error_aggregator_add_error(ErrorAggregator* aggregator, goo_error_t* error);
void error_aggregator_add_error_with_context(ErrorAggregator* aggregator,
                                             goo_error_t* error,
                                             const char* context);

// Collection control
void error_aggregator_stop(ErrorAggregator* aggregator);
void error_aggregator_reset(ErrorAggregator* aggregator);
bool error_aggregator_is_stopped(const ErrorAggregator* aggregator);

// =============================================================================
// Error Collection Results
// =============================================================================

// Finish collection and get results
goo_error_union_t* error_aggregator_finish(ErrorAggregator* aggregator);
CompositeError* error_aggregator_get_composite_error(ErrorAggregator* aggregator);
bool error_aggregator_has_errors(const ErrorAggregator* aggregator);
bool error_aggregator_has_fatal_errors(const ErrorAggregator* aggregator);

// Statistics and information
int error_aggregator_get_error_count(const ErrorAggregator* aggregator);
int error_aggregator_get_success_count(const ErrorAggregator* aggregator);
double error_aggregator_get_success_rate(const ErrorAggregator* aggregator);
bool error_aggregator_was_successful(const ErrorAggregator* aggregator);

// =============================================================================
// Composite Error Management
// =============================================================================

// Create and manage composite errors
CompositeError* composite_error_new(const char* operation_name);
CompositeError* composite_error_new_with_capacity(const char* operation_name, int capacity);
void composite_error_free(CompositeError* composite);

// Add errors to composite
void composite_error_add(CompositeError* composite, goo_error_t* error);
void composite_error_add_with_context(CompositeError* composite,
                                     goo_error_t* error,
                                     const char* context);
void composite_error_merge(CompositeError* dest, CompositeError* src);

// Composite error information
bool composite_error_has_fatal_errors(const CompositeError* composite);
int composite_error_get_error_count(const CompositeError* composite);
goo_error_t* composite_error_get_first_error(const CompositeError* composite);
goo_error_t* composite_error_get_last_error(const CompositeError* composite);

// Convert composite to regular error
goo_error_t* composite_error_to_goo_error(const CompositeError* composite);
char* composite_error_to_string(const CompositeError* composite);
char* composite_error_to_detailed_string(const CompositeError* composite);

// =============================================================================
// Error Grouping and Classification
// =============================================================================

// Create and manage error groups
ErrorGroup* error_group_new(const char* group_name, ErrorCategory category);
void error_group_free(ErrorGroup* group);

// Add errors to groups
void error_group_add_error(ErrorGroup* group, goo_error_t* error);
void error_group_set_fatal(ErrorGroup* group, bool is_fatal);
void error_group_set_priority(ErrorGroup* group, int priority);

// Group composite errors by category
ErrorGroup** composite_error_group_by_category(const CompositeError* composite, 
                                              int* group_count);
ErrorGroup** composite_error_group_by_severity(const CompositeError* composite,
                                              int* group_count);

// =============================================================================
// Parallel Error Collection
// =============================================================================

// Create parallel aggregators
ParallelErrorAggregator* parallel_error_aggregator_new(int max_threads);
void parallel_error_aggregator_free(ParallelErrorAggregator* aggregator);

// Configure parallel execution
void parallel_error_aggregator_set_work_items(ParallelErrorAggregator* aggregator,
                                              void** items,
                                              int item_count);

// Execute parallel operations
typedef goo_error_union_t* (*ParallelOperation)(void* work_item, int thread_id, void* context);

goo_error_union_t* parallel_error_aggregator_execute(ParallelErrorAggregator* aggregator,
                                                     ParallelOperation operation,
                                                     void* context);

// Get parallel results
int parallel_error_aggregator_get_successful_count(const ParallelErrorAggregator* aggregator);
int parallel_error_aggregator_get_failed_count(const ParallelErrorAggregator* aggregator);
void** parallel_error_aggregator_get_results(const ParallelErrorAggregator* aggregator);

// =============================================================================
// Integration with Error Recovery
// =============================================================================

// Execute operations with error collection and recovery
goo_error_union_t* error_aggregator_execute_with_recovery(ErrorAggregator* aggregator,
                                                          CollectableOperation operation,
                                                          void* args,
                                                          RetryAnnotation* retry,
                                                          CircuitBreakerAnnotation* circuit_breaker);

// Batch execution with error recovery
int error_aggregator_execute_batch_with_recovery(ErrorAggregator* aggregator,
                                                 CollectableOperation* operations,
                                                 void** args_array,
                                                 int operation_count,
                                                 RetryAnnotation* retry,
                                                 CircuitBreakerAnnotation* circuit_breaker);

// =============================================================================
// Utility Functions and Macros
// =============================================================================

// Convenience macros for common patterns
#define ERROR_AGGREGATOR_TRY(aggregator, expr) \
    error_aggregator_try((aggregator), (expr))

#define ERROR_AGGREGATOR_TRY_NAMED(aggregator, expr, name) \
    error_aggregator_try_with_name((aggregator), (expr), (name))

#define ERROR_AGGREGATOR_BATCH_VALIDATE(aggregator, validation_funcs, data_items, count) \
    error_aggregator_try_batch((aggregator), (validation_funcs), (data_items), (count))

// Helper functions for common validation patterns
bool validate_required_field(const char* field_name, const void* value, ErrorAggregator* aggregator);
bool validate_string_length(const char* field_name, const char* value, 
                           int min_length, int max_length, ErrorAggregator* aggregator);
bool validate_numeric_range(const char* field_name, double value,
                           double min_value, double max_value, ErrorAggregator* aggregator);
bool validate_email_format(const char* field_name, const char* email, ErrorAggregator* aggregator);

// Batch validation helpers
typedef bool (*ValidationFunction)(const void* data, ErrorAggregator* aggregator);

int error_aggregator_validate_all(ErrorAggregator* aggregator,
                                  ValidationFunction* validators,
                                  const void** data_items,
                                  int item_count);

// =============================================================================
// Configuration and Customization
// =============================================================================

// Global error aggregation configuration
typedef struct ErrorAggregationConfig {
    bool enabled;                  // Whether error aggregation is enabled
    bool enable_parallel_collection; // Whether parallel collection is supported
    int default_max_errors;        // Default maximum errors per aggregator
    bool default_collect_warnings; // Default warning collection setting
    bool default_thread_safe;      // Default thread safety setting
    
    // Memory management
    int composite_error_initial_capacity; // Initial capacity for composite errors
    int max_composite_errors;      // Maximum number of composite errors in memory
    
    // Performance tuning
    int parallel_thread_pool_size; // Size of thread pool for parallel operations
    bool enable_error_caching;     // Whether to cache composite errors
    
} ErrorAggregationConfig;

// Configure the error aggregation system
void configure_error_aggregation(const ErrorAggregationConfig* config);
ErrorAggregationConfig* get_error_aggregation_config(void);

// Initialize and shutdown the aggregation system
void error_aggregation_system_init(void);
void error_aggregation_system_shutdown(void);

// =============================================================================
// Statistics and Monitoring
// =============================================================================

typedef struct ErrorAggregationStats {
    // Collection statistics
    uint64_t total_aggregators_created;
    uint64_t total_composite_errors_created;
    uint64_t total_errors_collected;
    uint64_t total_operations_attempted;
    uint64_t total_successful_operations;
    
    // Performance metrics
    uint64_t total_collection_time_ms;
    double average_collection_time_ms;
    uint64_t total_parallel_collections;
    double parallel_speedup_factor;
    
    // Memory usage
    uint64_t current_memory_usage_bytes;
    uint64_t peak_memory_usage_bytes;
    int active_aggregators;
    int active_composite_errors;
    
} ErrorAggregationStats;

// Get aggregation statistics
ErrorAggregationStats get_error_aggregation_stats(void);
void reset_error_aggregation_stats(void);
void print_error_aggregation_stats(void);

#endif // GOO_ERROR_AGGREGATION_H