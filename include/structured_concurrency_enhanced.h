#ifndef GOO_STRUCTURED_CONCURRENCY_ENHANCED_H
#define GOO_STRUCTURED_CONCURRENCY_ENHANCED_H

#include "structured_concurrency.h"
#include "transparent_async.h"
#include "transparent_execution.h"
#include <setjmp.h>

// Enhanced structured concurrency for transparent async system
// This provides high-level language features like concurrent blocks,
// timeout decorators, and automatic error cancellation.

// Forward declarations
typedef struct ConcurrentBlock ConcurrentBlock;
typedef struct TimeoutDecorator TimeoutDecorator;
typedef struct RetryDecorator RetryDecorator;
typedef struct ConcurrentExpression ConcurrentExpression;
typedef struct StructuredScheduler StructuredScheduler;

// Concurrent block configuration
typedef struct ConcurrentBlockConfig {
    size_t max_concurrent_tasks;
    uint64_t timeout_ms;
    bool fail_fast;              // Cancel all on first error
    bool collect_all_results;    // Collect results even if some fail
    TaskPriority default_priority;
    
    // Resource limits
    size_t max_memory_per_task;
    size_t max_total_memory;
    
    // Integration with transparent async
    bool use_transparent_execution;
    AsyncRuntime* async_runtime;
} ConcurrentBlockConfig;

// Concurrent expression - represents a single async computation
typedef struct ConcurrentExpression {
    uint64_t id;
    char name[64];
    
    // Expression details
    TransparentFunction* function;
    void* arguments;
    size_t args_size;
    
    // Dependencies
    ConcurrentExpression** dependencies;
    size_t dependency_count;
    
    // Result
    AsyncFuture* future;
    void* result;
    size_t result_size;
    Error* error;
    
    // State
    enum {
        EXPR_STATE_CREATED,
        EXPR_STATE_SCHEDULED,
        EXPR_STATE_RUNNING,
        EXPR_STATE_COMPLETED,
        EXPR_STATE_FAILED,
        EXPR_STATE_CANCELLED
    } state;
    
    // Cancellation
    CancellationToken* cancel_token;
    
    struct ConcurrentExpression* next;
} ConcurrentExpression;

// Concurrent block - represents a collection of concurrent expressions
typedef struct ConcurrentBlock {
    uint64_t id;
    char name[64];
    ConcurrentBlockConfig config;
    
    // Expressions in this block
    ConcurrentExpression* first_expression;
    ConcurrentExpression* last_expression;
    size_t expression_count;
    
    // Task scope for execution
    TaskScope* task_scope;
    
    // Results collection
    void** results;
    size_t* result_sizes;
    Error** errors;
    
    // Synchronization
    pthread_mutex_t block_mutex;
    pthread_cond_t all_completed;
    
    // Statistics
    uint64_t start_time_ns;
    uint64_t end_time_ns;
    size_t completed_expressions;
    size_t failed_expressions;
    
    // Parent block for nesting
    struct ConcurrentBlock* parent_block;
    
    // Cancellation propagation
    CancellationToken* block_cancel_token;
    bool is_cancelled;
} ConcurrentBlock;

// Timeout decorator for async functions
typedef struct TimeoutDecorator {
    uint64_t timeout_ms;
    TransparentFunction* wrapped_function;
    
    // Timeout behavior
    enum {
        TIMEOUT_ACTION_CANCEL,      // Cancel the operation
        TIMEOUT_ACTION_RETURN_NULL, // Return null result
        TIMEOUT_ACTION_THROW_ERROR  // Throw timeout error
    } timeout_action;
    
    // Statistics
    uint64_t total_calls;
    uint64_t timeout_count;
    uint64_t avg_execution_time_ms;
} TimeoutDecorator;

// Retry decorator for async functions
typedef struct RetryDecorator {
    size_t max_attempts;
    uint64_t base_delay_ms;
    double backoff_multiplier;
    uint64_t max_delay_ms;
    
    TransparentFunction* wrapped_function;
    
    // Retry conditions
    bool (*should_retry)(Error* error, size_t attempt, void* context);
    void* retry_context;
    
    // Statistics
    uint64_t total_calls;
    uint64_t retry_count;
    uint64_t success_after_retry_count;
} RetryDecorator;

// Structured scheduler for managing concurrent blocks
typedef struct StructuredScheduler {
    uint64_t id;
    
    // Active blocks
    ConcurrentBlock** active_blocks;
    size_t active_count;
    size_t active_capacity;
    
    // Global configuration
    TaskScope* global_scope;
    AsyncRuntime* async_runtime;
    
    // Resource tracking
    atomic_size_t total_memory_used;
    atomic_size_t total_active_tasks;
    
    // Statistics
    uint64_t total_blocks_created;
    uint64_t total_expressions_executed;
    uint64_t total_timeout_events;
    uint64_t total_retry_events;
    
    pthread_mutex_t scheduler_mutex;
} StructuredScheduler;

// Function type for concurrent expressions
typedef Result_void_ptr (*ConcurrentFunction)(void* args, AsyncContext* async_ctx);

// Enhanced structured concurrency API

// Scheduler management
StructuredScheduler* structured_scheduler_create(AsyncRuntime* runtime);
void structured_scheduler_destroy(StructuredScheduler* scheduler);
Result_void_ptr structured_scheduler_start(StructuredScheduler* scheduler);
Result_void_ptr structured_scheduler_shutdown(StructuredScheduler* scheduler, uint64_t timeout_ms);

// Global scheduler functions
void structured_set_global_scheduler(StructuredScheduler* scheduler);
StructuredScheduler* structured_get_global_scheduler(void);

// Configuration helpers
ConcurrentBlockConfig concurrent_block_config_default(void);
ConcurrentBlockConfig concurrent_block_config_cpu_intensive(void);
ConcurrentBlockConfig concurrent_block_config_io_intensive(void);
ConcurrentBlockConfig concurrent_block_config_fail_fast(void);

// Concurrent block operations
ConcurrentBlock* concurrent_block_create(const char* name, ConcurrentBlockConfig config);
void concurrent_block_destroy(ConcurrentBlock* block);

// Concurrent expression operations
ConcurrentExpression* concurrent_expression_create(const char* name, 
    ConcurrentFunction function, void* args, size_t args_size);
Result_void_ptr concurrent_expression_add_dependency(ConcurrentExpression* expr, 
    ConcurrentExpression* dependency);
void concurrent_expression_destroy(ConcurrentExpression* expr);

// Block execution
Result_void_ptr concurrent_block_add_expression(ConcurrentBlock* block, ConcurrentExpression* expr);
Result_void_ptr concurrent_block_execute(ConcurrentBlock* block);
Result_void_ptr concurrent_block_wait(ConcurrentBlock* block, uint64_t timeout_ms);
Result_void_ptr concurrent_block_cancel(ConcurrentBlock* block);

// Timeout decorator operations
TimeoutDecorator* timeout_decorator_create(uint64_t timeout_ms, TransparentFunction* function);
void timeout_decorator_destroy(TimeoutDecorator* decorator);
Result_void_ptr timeout_decorator_execute(TimeoutDecorator* decorator, void* args, size_t args_size);

// Retry decorator operations  
RetryDecorator* retry_decorator_create(size_t max_attempts, uint64_t base_delay_ms, 
    TransparentFunction* function);
void retry_decorator_destroy(RetryDecorator* decorator);
Result_void_ptr retry_decorator_execute(RetryDecorator* decorator, void* args, size_t args_size);
void retry_decorator_set_condition(RetryDecorator* decorator, 
    bool (*should_retry)(Error* error, size_t attempt, void* context), void* context);

// High-level convenience macros for structured concurrency

// Concurrent block with automatic cleanup
#define CONCURRENT_BLOCK(name) \
    ConcurrentBlockConfig __config = concurrent_block_config_default(); \
    ConcurrentBlock* name = concurrent_block_create(#name, __config); \
    if (!name) { \
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create concurrent block")); \
    }

#define CONCURRENT_BLOCK_WITH_CONFIG(name, config) \
    ConcurrentBlock* name = concurrent_block_create(#name, config); \
    if (!name) { \
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create concurrent block")); \
    }

#define CONCURRENT_BLOCK_END(block) \
    do { \
        Result_void_ptr __wait_result = concurrent_block_wait(block, UINT64_MAX); \
        concurrent_block_destroy(block); \
        if (__wait_result.is_error) return __wait_result; \
    } while(0)

// Concurrent expression creation
#define CONCURRENT_EXPR(block, name, function, args) \
    do { \
        ConcurrentExpression* __expr = concurrent_expression_create(#name, \
            (ConcurrentFunction)function, args, sizeof(*args)); \
        if (__expr) { \
            concurrent_block_add_expression(block, __expr); \
        } \
    } while(0)

// Timeout decorator macro
#define TIMEOUT(timeout_ms) \
    TimeoutDecorator* __timeout_decorator = NULL; \
    __timeout_decorator = timeout_decorator_create(timeout_ms, NULL)

#define WITH_TIMEOUT(timeout_ms, function, args) \
    ({ \
        TimeoutDecorator* __decorator = timeout_decorator_create(timeout_ms, function); \
        Result_void_ptr __result = timeout_decorator_execute(__decorator, args, sizeof(*args)); \
        timeout_decorator_destroy(__decorator); \
        __result; \
    })

// Retry decorator macro
#define RETRY(max_attempts) \
    RetryDecorator* __retry_decorator = NULL; \
    __retry_decorator = retry_decorator_create(max_attempts, 1000, NULL)

#define WITH_RETRY(max_attempts, function, args) \
    ({ \
        RetryDecorator* __decorator = retry_decorator_create(max_attempts, 1000, function); \
        Result_void_ptr __result = retry_decorator_execute(__decorator, args, sizeof(*args)); \
        retry_decorator_destroy(__decorator); \
        __result; \
    })

// Structured error handling
#define CONCURRENT_TRY(block) \
    do { \
        Result_void_ptr __exec_result = concurrent_block_execute(block); \
        if (__exec_result.is_error) { \
            concurrent_block_destroy(block); \
            return __exec_result; \
        } \
    } while(0)

#define CONCURRENT_CATCH(block, error_handler) \
    do { \
        Result_void_ptr __wait_result = concurrent_block_wait(block, UINT64_MAX); \
        if (__wait_result.is_error) { \
            error_handler(__wait_result.error); \
        } \
        concurrent_block_destroy(block); \
    } while(0)

// Resource management with automatic cleanup
typedef struct ConcurrentResource {
    void* resource;
    void (*cleanup_function)(void* resource);
    bool is_acquired;
    pthread_mutex_t resource_mutex;
} ConcurrentResource;

ConcurrentResource* concurrent_resource_create(void* resource, void (*cleanup_fn)(void*));
void concurrent_resource_destroy(ConcurrentResource* resource);

#define CONCURRENT_RESOURCE(name, resource, cleanup_fn) \
    ConcurrentResource* name = concurrent_resource_create(resource, cleanup_fn); \
    if (!name) { \
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create concurrent resource")); \
    }

#define CONCURRENT_RESOURCE_END(resource) \
    do { \
        concurrent_resource_destroy(resource); \
    } while(0)

// Statistics and monitoring
typedef struct StructuredConcurrencyStats {
    uint64_t total_blocks_created;
    uint64_t total_expressions_executed;
    uint64_t total_expressions_completed;
    uint64_t total_expressions_failed;
    uint64_t total_expressions_cancelled;
    
    // Timing statistics
    uint64_t avg_block_execution_time_ns;
    uint64_t avg_expression_execution_time_ns;
    
    // Resource usage
    size_t current_memory_usage;
    size_t peak_memory_usage;
    size_t active_blocks;
    size_t active_expressions;
    
    // Error statistics
    uint64_t timeout_events;
    uint64_t retry_events;
    uint64_t cancellation_events;
    
    // Success rates
    double block_success_rate;
    double expression_success_rate;
} StructuredConcurrencyStats;

StructuredConcurrencyStats structured_concurrency_get_stats(StructuredScheduler* scheduler);
void structured_concurrency_reset_stats(StructuredScheduler* scheduler);

// Advanced patterns

// For-each parallel execution
typedef Result_void_ptr (*ForEachFunction)(void* item, size_t index, void* context);

Result_void_ptr concurrent_for_each(void** items, size_t item_count, size_t item_size,
    ForEachFunction function, void* context, ConcurrentBlockConfig config);

// Pipeline processing
typedef struct ConcurrentPipeline {
    ConcurrentBlock* block;
    ConcurrentExpression** stages;
    size_t stage_count;
    
    // Pipeline state
    bool is_streaming;
    size_t current_batch_size;
    void** input_queue;
    void** output_queue;
    size_t queue_capacity;
    
    pthread_mutex_t pipeline_mutex;
} ConcurrentPipeline;

ConcurrentPipeline* concurrent_pipeline_create(size_t stage_count, ConcurrentFunction* stages,
    ConcurrentBlockConfig config);
Result_void_ptr concurrent_pipeline_process(ConcurrentPipeline* pipeline, 
    void** inputs, size_t input_count, void*** outputs, size_t* output_count);
void concurrent_pipeline_destroy(ConcurrentPipeline* pipeline);

// Batch processing
Result_void_ptr concurrent_batch_process(void** items, size_t item_count, size_t batch_size,
    ConcurrentFunction processor, void* context, ConcurrentBlockConfig config,
    void*** results, size_t* result_count);

#endif // GOO_STRUCTURED_CONCURRENCY_ENHANCED_H