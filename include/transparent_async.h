#ifndef TRANSPARENT_ASYNC_H
#define TRANSPARENT_ASYNC_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include "ergonomic_errors.h"
#include "fearless_concurrency.h"
#include "structured_concurrency.h"
#include "work_stealing.h"

// Forward declarations
typedef struct AsyncRuntime AsyncRuntime;
typedef struct AsyncTask AsyncTask;
typedef struct AsyncFuture AsyncFuture;
typedef struct AsyncExecutor AsyncExecutor;
typedef struct AsyncWaker AsyncWaker;

// Async task states
typedef enum {
    ASYNC_TASK_PENDING,     // Task is waiting to be executed
    ASYNC_TASK_READY,       // Task is ready to run
    ASYNC_TASK_RUNNING,     // Task is currently executing
    ASYNC_TASK_SUSPENDED,   // Task is waiting for I/O or other async operation
    ASYNC_TASK_COMPLETED,   // Task has finished successfully
    ASYNC_TASK_FAILED,      // Task failed with an error
    ASYNC_TASK_CANCELLED    // Task was cancelled
} AsyncTaskState;

// Executor types for automatic selection
typedef enum {
    EXECUTOR_TYPE_INLINE,        // Execute immediately on current thread
    EXECUTOR_TYPE_THREAD_POOL,   // Use shared thread pool
    EXECUTOR_TYPE_WORK_STEALING, // Use work-stealing scheduler
    EXECUTOR_TYPE_NUMA_AWARE,    // Use NUMA-aware scheduling
    EXECUTOR_TYPE_IO_OPTIMIZED   // Optimized for I/O heavy workloads
} ExecutorType;

// Async function signature
typedef Result_void_ptr (*AsyncFunction)(void* context, AsyncWaker* waker);

// Async task context
typedef struct AsyncTaskContext {
    uint64_t task_id;
    void* user_data;
    size_t user_data_size;
    AsyncFunction function;
    
    // Task scheduling info
    int priority;
    uint64_t submit_time;
    uint64_t start_time;
    uint64_t completion_time;
    
    // Resource requirements
    size_t estimated_cpu_time_us;
    size_t estimated_memory_bytes;
    bool is_io_bound;
    bool requires_numa_locality;
    
    // Parent task for structured concurrency
    AsyncTask* parent_task;
    uint32_t cancellation_token;
} AsyncTaskContext;

// Async task structure
typedef struct AsyncTask {
    AsyncTaskContext context;
    AsyncTaskState state;
    
    // Execution state
    atomic_bool is_cancelled;
    atomic_bool needs_wakeup;
    Result_void_ptr result;
    
    // Synchronization
    pthread_mutex_t state_mutex;
    pthread_cond_t completion_cond;
    
    // Linked list for scheduler queues
    struct AsyncTask* next;
    
    // Waker for resuming suspended tasks
    AsyncWaker* waker;
    
    // Statistics
    atomic_uint_fast64_t poll_count;
    atomic_uint_fast64_t wake_count;
    atomic_uint_fast64_t yield_count;
} AsyncTask;

// Future for transparent async execution
typedef struct AsyncFuture {
    AsyncTask* task;
    bool is_resolved;
    
    // Zero-cost optimization flags
    bool force_sync_execution;
    bool prefer_inline_execution;
    
    // Result caching
    Result_void_ptr cached_result;
    bool result_cached;
} AsyncFuture;

// Waker for resuming suspended tasks
typedef struct AsyncWaker {
    AsyncTask* task;
    void (*wake_fn)(AsyncWaker* waker);
    void* wake_data;
    atomic_bool is_awakened;
} AsyncWaker;

// Thread pool worker context
typedef struct AsyncWorker {
    uint32_t worker_id;
    pthread_t thread;
    bool is_running;
    
    // Work stealing queue
    WorkStealingDeque* local_queue;
    
    // Performance metrics
    atomic_uint_fast64_t tasks_executed;
    atomic_uint_fast64_t tasks_stolen;
    atomic_uint_fast64_t idle_time_ns;
    atomic_uint_fast64_t busy_time_ns;
    
    // NUMA affinity
    uint32_t numa_node;
    uint32_t cpu_affinity;
    
    AsyncRuntime* runtime;
} AsyncWorker;

// Async executor interface
typedef struct AsyncExecutor {
    ExecutorType type;
    const char* name;
    
    // Executor functions
    Result_void_ptr (*submit_task)(struct AsyncExecutor* executor, AsyncTask* task);
    Result_void_ptr (*shutdown)(struct AsyncExecutor* executor, uint64_t timeout_ms);
    
    // Performance characteristics
    uint32_t optimal_task_size_range[2];  // [min, max] task sizes this executor handles well
    bool handles_io_bound_tasks;
    bool supports_numa_awareness;
    bool supports_work_stealing;
    
    // Runtime reference
    AsyncRuntime* runtime;
    void* executor_data;
} AsyncExecutor;

// Async runtime configuration
typedef struct AsyncRuntimeConfig {
    // Thread pool settings
    size_t min_worker_threads;
    size_t max_worker_threads;
    size_t io_thread_count;
    
    // Scheduling parameters
    bool enable_work_stealing;
    bool enable_numa_awareness;
    bool enable_automatic_scaling;
    uint32_t task_queue_size;
    
    // Performance tuning
    uint64_t scheduler_tick_interval_us;
    uint32_t load_balancing_threshold;
    bool enable_zero_cost_futures;
    
    // Resource limits
    size_t max_concurrent_tasks;
    size_t max_memory_usage_bytes;
    uint64_t default_task_timeout_ms;
    
    // Monitoring
    bool enable_performance_monitoring;
    bool enable_detailed_tracing;
} AsyncRuntimeConfig;

// Performance metrics
typedef struct AsyncPerformanceMetrics {
    uint64_t total_tasks_submitted;
    uint64_t total_tasks_completed;
    uint64_t total_tasks_failed;
    uint64_t total_tasks_cancelled;
    
    uint64_t average_task_execution_time_ns;
    uint64_t average_task_wait_time_ns;
    uint64_t max_task_execution_time_ns;
    
    uint32_t active_worker_count;
    uint32_t idle_worker_count;
    double cpu_utilization_percent;
    
    size_t memory_usage_bytes;
    size_t peak_memory_usage_bytes;
    
    uint64_t work_steal_count;
    uint64_t context_switch_count;
} AsyncPerformanceMetrics;

// Runtime statistics
typedef struct RuntimeStats {
    uint64_t total_tasks;
    uint64_t pending_tasks;
    uint64_t active_workers;
    double cpu_utilization;
} RuntimeStats;

// Main async runtime
typedef struct AsyncRuntime {
    AsyncRuntimeConfig config;
    bool is_initialized;
    bool is_running;
    
    // Thread pool
    AsyncWorker* workers;
    size_t worker_count;
    
    // Executors
    AsyncExecutor* inline_executor;
    AsyncExecutor* thread_pool_executor;
    AsyncExecutor* work_stealing_executor;
    AsyncExecutor* numa_executor;
    AsyncExecutor* io_executor;
    
    // Global task queues
    WorkStealingDeque* global_queue;
    WorkStealingDeque* io_queue;
    
    // Task management
    atomic_uint_fast64_t next_task_id;
    atomic_uint_fast64_t active_task_count;
    atomic_uint_fast64_t completed_task_count;
    
    // Scheduler thread
    pthread_t scheduler_thread;
    atomic_bool scheduler_running;
    
    // Performance monitoring
    atomic_uint_fast64_t total_submitted_tasks;
    atomic_uint_fast64_t total_completed_tasks;
    atomic_uint_fast64_t total_failed_tasks;
    atomic_uint_fast64_t total_cancelled_tasks;
    
    // System resources
    uint32_t cpu_count;
    uint32_t numa_node_count;
    uint64_t available_memory_bytes;
    
    // Synchronization
    pthread_mutex_t runtime_mutex;
    pthread_cond_t shutdown_cond;
    
    // Startup time
    uint64_t startup_time_ns;
} AsyncRuntime;

// Runtime lifecycle
AsyncRuntime* async_runtime_create(AsyncRuntimeConfig config);
void async_runtime_destroy(AsyncRuntime* runtime);
Result_void_ptr async_runtime_start(AsyncRuntime* runtime);
Result_void_ptr async_runtime_shutdown(AsyncRuntime* runtime, uint64_t timeout_ms);

// Configuration helpers
AsyncRuntimeConfig async_runtime_config_default(void);
AsyncRuntimeConfig async_runtime_config_io_optimized(void);
AsyncRuntimeConfig async_runtime_config_compute_optimized(void);
AsyncRuntimeConfig async_runtime_config_numa_optimized(void);

// Task creation and management
AsyncTask* async_task_create(AsyncFunction function, void* context, size_t context_size);
void async_task_destroy(AsyncTask* task);
Result_void_ptr async_task_submit(AsyncRuntime* runtime, AsyncTask* task);
Result_void_ptr async_task_cancel(AsyncTask* task);

// Future operations
AsyncFuture* async_future_create(AsyncTask* task);
void async_future_destroy(AsyncFuture* future);
Result_void_ptr async_future_get(AsyncFuture* future, uint64_t timeout_ms);
bool async_future_is_ready(AsyncFuture* future);
Result_void_ptr async_future_poll(AsyncFuture* future);

// Waker operations
AsyncWaker* async_waker_create(AsyncTask* task);
void async_waker_destroy(AsyncWaker* waker);
void async_waker_wake(AsyncWaker* waker);
bool async_waker_is_awakened(AsyncWaker* waker);

// Executor selection and management
AsyncExecutor* async_select_executor(AsyncRuntime* runtime, AsyncTask* task);
Result_void_ptr async_executor_submit(AsyncExecutor* executor, AsyncTask* task);

// Performance monitoring

AsyncPerformanceMetrics async_runtime_get_metrics(AsyncRuntime* runtime);
void async_runtime_print_metrics(AsyncRuntime* runtime);
void async_runtime_reset_metrics(AsyncRuntime* runtime);

// Utility functions
uint64_t async_get_timestamp_ns(void);
ExecutorType async_recommend_executor_type(AsyncTask* task);
bool async_should_execute_inline(AsyncTask* task);
RuntimeStats async_runtime_get_stats(AsyncRuntime* runtime);

// Task submission via runtime
AsyncFuture* async_runtime_submit(AsyncRuntime* runtime, AsyncTask* task);

// Global runtime instance (singleton pattern)
AsyncRuntime* async_runtime_global(void);

// Integration with existing concurrency systems
Result_void_ptr async_integrate_with_goroutines(AsyncRuntime* runtime);
Result_void_ptr async_integrate_with_structured_concurrency(AsyncRuntime* runtime, TaskScope* scope);
Result_void_ptr async_integrate_with_work_stealing(AsyncRuntime* runtime, WorkStealingDeque* global_deque);

// Error handling for async contexts
typedef struct AsyncError {
    Error base_error;
    uint64_t task_id;
    AsyncTaskState task_state;
    const char* executor_name;
    uint64_t error_timestamp;
} AsyncError;

AsyncError* async_error_create(ErrorCode code, const char* message, uint64_t task_id);
void async_error_destroy(AsyncError* error);

// Transparent async macros for easy integration
#define ASYNC_RUNTIME_GLOBAL() async_get_global_runtime()
#define ASYNC_SUBMIT(func, ctx) async_submit_transparent(func, ctx, sizeof(*ctx))
#define ASYNC_AWAIT(future) async_await_transparent(future)
#define ASYNC_SPAWN(func, ctx) async_spawn_detached(func, ctx, sizeof(*ctx))

// Zero-cost future optimization
#define ASYNC_INLINE_THRESHOLD_NS 1000  // Execute inline if estimated time < 1µs
#define ASYNC_PREFER_SYNC_OPERATIONS true

// Global runtime access (singleton pattern)
AsyncRuntime* async_get_global_runtime(void);
Result_void_ptr async_set_global_runtime(AsyncRuntime* runtime);

// Transparent async execution helpers
Result_void_ptr async_submit_transparent(AsyncFunction function, void* context, size_t context_size);
Result_void_ptr async_await_transparent(AsyncFuture* future);
AsyncFuture* async_spawn_detached(AsyncFunction function, void* context, size_t context_size);

#endif // TRANSPARENT_ASYNC_H