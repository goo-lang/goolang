#include "../../include/transparent_async.h"
#include "../../include/errors/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

// Global runtime instance
static AsyncRuntime* g_global_runtime = NULL;
static pthread_mutex_t g_runtime_mutex = PTHREAD_MUTEX_INITIALIZER;

// Utility functions
uint64_t async_get_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Get system information
static uint32_t get_cpu_count(void) {
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    return cpu_count > 0 ? (uint32_t)cpu_count : 4;
}

static uint64_t get_available_memory(void) {
    long page_size = sysconf(_SC_PAGESIZE);
    long page_count = sysconf(_SC_PHYS_PAGES);
    if (page_size > 0 && page_count > 0) {
        return (uint64_t)page_size * (uint64_t)page_count;
    }
    return 8ULL * 1024 * 1024 * 1024; // 8GB fallback
}

static uint32_t get_numa_node_count(void) {
    // Simplified NUMA detection - assume 1 node on macOS, check for Linux later
    return 1;
}

// Configuration helpers
AsyncRuntimeConfig async_runtime_config_default(void) {
    uint32_t cpu_count = get_cpu_count();
    
    return (AsyncRuntimeConfig) {
        .min_worker_threads = 2,
        .max_worker_threads = cpu_count * 2,
        .io_thread_count = 4,
        
        .enable_work_stealing = true,
        .enable_numa_awareness = false,
        .enable_automatic_scaling = true,
        .task_queue_size = 1024,
        
        .scheduler_tick_interval_us = 100,
        .load_balancing_threshold = 10,
        .enable_zero_cost_futures = true,
        
        .max_concurrent_tasks = 10000,
        .max_memory_usage_bytes = get_available_memory() / 4,
        .default_task_timeout_ms = 30000,
        
        .enable_performance_monitoring = true,
        .enable_detailed_tracing = false
    };
}

AsyncRuntimeConfig async_runtime_config_io_optimized(void) {
    AsyncRuntimeConfig config = async_runtime_config_default();
    config.io_thread_count = get_cpu_count();
    config.max_worker_threads = get_cpu_count();
    config.enable_work_stealing = false;
    config.scheduler_tick_interval_us = 50;
    return config;
}

AsyncRuntimeConfig async_runtime_config_compute_optimized(void) {
    AsyncRuntimeConfig config = async_runtime_config_default();
    config.max_worker_threads = get_cpu_count();
    config.io_thread_count = 2;
    config.enable_work_stealing = true;
    config.enable_numa_awareness = true;
    config.scheduler_tick_interval_us = 200;
    return config;
}

AsyncRuntimeConfig async_runtime_config_numa_optimized(void) {
    AsyncRuntimeConfig config = async_runtime_config_compute_optimized();
    config.enable_numa_awareness = true;
    config.max_worker_threads = get_cpu_count();
    return config;
}

// Task creation and management
AsyncTask* async_task_create(AsyncFunction function, void* context, size_t context_size) {
    if (!function) return NULL;
    
    AsyncTask* task = calloc(1, sizeof(AsyncTask));
    if (!task) return NULL;
    
    // Initialize task context
    task->context.task_id = 0; // Will be set when submitted
    task->context.function = function;
    task->context.submit_time = async_get_timestamp_ns();
    task->context.priority = 0;
    
    // Copy user context
    if (context && context_size > 0) {
        task->context.user_data = malloc(context_size);
        if (!task->context.user_data) {
            free(task);
            return NULL;
        }
        memcpy(task->context.user_data, context, context_size);
        task->context.user_data_size = context_size;
    }
    
    // Initialize task state
    task->state = ASYNC_TASK_PENDING;
    atomic_init(&task->is_cancelled, false);
    atomic_init(&task->needs_wakeup, false);
    atomic_init(&task->poll_count, 0);
    atomic_init(&task->wake_count, 0);
    atomic_init(&task->yield_count, 0);
    
    // Initialize synchronization
    if (pthread_mutex_init(&task->state_mutex, NULL) != 0) {
        free(task->context.user_data);
        free(task);
        return NULL;
    }
    
    if (pthread_cond_init(&task->completion_cond, NULL) != 0) {
        pthread_mutex_destroy(&task->state_mutex);
        free(task->context.user_data);
        free(task);
        return NULL;
    }
    
    return task;
}

void async_task_destroy(AsyncTask* task) {
    if (!task) return;
    
    pthread_mutex_destroy(&task->state_mutex);
    pthread_cond_destroy(&task->completion_cond);
    
    if (task->waker) {
        async_waker_destroy(task->waker);
    }
    
    free(task->context.user_data);
    free(task);
}

// Waker operations
AsyncWaker* async_waker_create(AsyncTask* task) {
    if (!task) return NULL;
    
    AsyncWaker* waker = calloc(1, sizeof(AsyncWaker));
    if (!waker) return NULL;
    
    waker->task = task;
    waker->wake_fn = NULL; // Default wake function
    atomic_init(&waker->is_awakened, false);
    
    return waker;
}

void async_waker_destroy(AsyncWaker* waker) {
    if (!waker) return;
    free(waker);
}

void async_waker_wake(AsyncWaker* waker) {
    if (!waker || !waker->task) return;
    
    atomic_store(&waker->is_awakened, true);
    atomic_store(&waker->task->needs_wakeup, true);
    atomic_fetch_add(&waker->task->wake_count, 1);
    
    // Signal completion condition to wake up waiting threads
    pthread_mutex_lock(&waker->task->state_mutex);
    pthread_cond_signal(&waker->task->completion_cond);
    pthread_mutex_unlock(&waker->task->state_mutex);
    
    if (waker->wake_fn) {
        waker->wake_fn(waker);
    }
}

bool async_waker_is_awakened(AsyncWaker* waker) {
    if (!waker) return false;
    return atomic_load(&waker->is_awakened);
}

// Future operations
AsyncFuture* async_future_create(AsyncTask* task) {
    if (!task) return NULL;
    
    AsyncFuture* future = calloc(1, sizeof(AsyncFuture));
    if (!future) return NULL;
    
    future->task = task;
    future->is_resolved = false;
    future->result_cached = false;
    future->force_sync_execution = false;
    future->prefer_inline_execution = async_should_execute_inline(task);
    
    return future;
}

void async_future_destroy(AsyncFuture* future) {
    if (!future) return;
    free(future);
}

Result_void_ptr async_future_get(AsyncFuture* future, uint64_t timeout_ms) {
    if (!future || !future->task) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Invalid future or task"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Check if result is already cached
    if (future->result_cached) {
        return future->cached_result;
    }
    
    AsyncTask* task = future->task;
    
    // Wait for task completion with timeout
    pthread_mutex_lock(&task->state_mutex);
    
    if (task->state != ASYNC_TASK_COMPLETED && task->state != ASYNC_TASK_FAILED) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
        
        int wait_result = pthread_cond_timedwait(&task->completion_cond, &task->state_mutex, &deadline);
        if (wait_result != 0) {
            pthread_mutex_unlock(&task->state_mutex);
            
            Error* error = malloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_OPERATION_CANCELLED,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_RUNTIME,
                .message = strdup("Future wait timeout"),
                .hint = strdup("Increase timeout or check for deadlocks"),
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
        }
    }
    
    Result_void_ptr result = task->result;
    pthread_mutex_unlock(&task->state_mutex);
    
    // Cache the result
    future->cached_result = result;
    future->result_cached = true;
    future->is_resolved = true;
    
    return result;
}

bool async_future_is_ready(AsyncFuture* future) {
    if (!future || !future->task) return false;
    
    if (future->is_resolved) return true;
    
    AsyncTaskState state = future->task->state;
    return (state == ASYNC_TASK_COMPLETED || state == ASYNC_TASK_FAILED || state == ASYNC_TASK_CANCELLED);
}

Result_void_ptr async_future_poll(AsyncFuture* future) {
    if (!future || !future->task) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Invalid future"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    if (async_future_is_ready(future)) {
        return async_future_get(future, 0);
    }
    
    atomic_fetch_add(&future->task->poll_count, 1);
    
    // Future is not ready, return pending status
    return OK_PTR(NULL);
}

// Executor type recommendation
ExecutorType async_recommend_executor_type(AsyncTask* task) {
    if (!task) return EXECUTOR_TYPE_INLINE;
    
    // Check for inline execution
    if (async_should_execute_inline(task)) {
        return EXECUTOR_TYPE_INLINE;
    }
    
    // I/O bound tasks prefer I/O optimized executor
    if (task->context.is_io_bound) {
        return EXECUTOR_TYPE_IO_OPTIMIZED;
    }
    
    // NUMA-aware tasks
    if (task->context.requires_numa_locality) {
        return EXECUTOR_TYPE_NUMA_AWARE;
    }
    
    // Large CPU-bound tasks prefer work-stealing
    if (task->context.estimated_cpu_time_us > 1000) {
        return EXECUTOR_TYPE_WORK_STEALING;
    }
    
    // Default to thread pool
    return EXECUTOR_TYPE_THREAD_POOL;
}

bool async_should_execute_inline(AsyncTask* task) {
    if (!task) return false;
    
    // Execute inline if estimated execution time is very small
    if (task->context.estimated_cpu_time_us > 0 && 
        task->context.estimated_cpu_time_us < ASYNC_INLINE_THRESHOLD_NS / 1000) {
        return true;
    }
    
    // Execute inline for very simple tasks
    if (task->context.estimated_memory_bytes < 1024 && !task->context.is_io_bound) {
        return true;
    }
    
    return false;
}

// Inline executor implementation
static Result_void_ptr inline_executor_submit(AsyncExecutor* executor, AsyncTask* task) {
    if (!executor || !task) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Invalid executor or task"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Execute task immediately on current thread
    task->state = ASYNC_TASK_RUNNING;
    task->context.start_time = async_get_timestamp_ns();
    
    // Create waker for the task
    AsyncWaker* waker = async_waker_create(task);
    task->waker = waker;
    
    // Execute the function
    Result_void_ptr result = task->context.function(task->context.user_data, waker);
    
    // Update task state
    task->context.completion_time = async_get_timestamp_ns();
    task->state = result.is_error ? ASYNC_TASK_FAILED : ASYNC_TASK_COMPLETED;
    task->result = result;
    
    // Signal completion
    pthread_mutex_lock(&task->state_mutex);
    pthread_cond_broadcast(&task->completion_cond);
    pthread_mutex_unlock(&task->state_mutex);
    
    return OK_PTR(NULL);
}

static Result_void_ptr inline_executor_shutdown(AsyncExecutor* executor, uint64_t timeout_ms) {
    (void)executor;
    (void)timeout_ms;
    // Inline executor doesn't need shutdown
    return OK_PTR(NULL);
}

// Create inline executor
static AsyncExecutor* create_inline_executor(AsyncRuntime* runtime) {
    AsyncExecutor* executor = calloc(1, sizeof(AsyncExecutor));
    if (!executor) return NULL;
    
    executor->type = EXECUTOR_TYPE_INLINE;
    executor->name = "inline";
    executor->submit_task = inline_executor_submit;
    executor->shutdown = inline_executor_shutdown;
    executor->optimal_task_size_range[0] = 0;
    executor->optimal_task_size_range[1] = 1000; // µs
    executor->handles_io_bound_tasks = false;
    executor->supports_numa_awareness = false;
    executor->supports_work_stealing = false;
    executor->runtime = runtime;
    
    return executor;
}

// Runtime creation and management
AsyncRuntime* async_runtime_create(AsyncRuntimeConfig config) {
    AsyncRuntime* runtime = calloc(1, sizeof(AsyncRuntime));
    if (!runtime) return NULL;
    
    runtime->config = config;
    runtime->cpu_count = get_cpu_count();
    runtime->numa_node_count = get_numa_node_count();
    runtime->available_memory_bytes = get_available_memory();
    runtime->startup_time_ns = async_get_timestamp_ns();
    
    // Initialize atomics
    atomic_init(&runtime->next_task_id, 1);
    atomic_init(&runtime->active_task_count, 0);
    atomic_init(&runtime->completed_task_count, 0);
    atomic_init(&runtime->total_submitted_tasks, 0);
    atomic_init(&runtime->total_completed_tasks, 0);
    atomic_init(&runtime->total_failed_tasks, 0);
    atomic_init(&runtime->total_cancelled_tasks, 0);
    atomic_init(&runtime->scheduler_running, false);
    
    // Initialize synchronization
    if (pthread_mutex_init(&runtime->runtime_mutex, NULL) != 0) {
        free(runtime);
        return NULL;
    }
    
    if (pthread_cond_init(&runtime->shutdown_cond, NULL) != 0) {
        pthread_mutex_destroy(&runtime->runtime_mutex);
        free(runtime);
        return NULL;
    }
    
    // Create executors
    runtime->inline_executor = create_inline_executor(runtime);
    if (!runtime->inline_executor) {
        async_runtime_destroy(runtime);
        return NULL;
    }
    
    runtime->is_initialized = true;
    return runtime;
}

void async_runtime_destroy(AsyncRuntime* runtime) {
    if (!runtime) return;

    if (runtime->is_running) {
        async_runtime_shutdown(runtime, 5000); // 5 second timeout
    }

    // If this runtime is the registered global, clear the reference so it doesn't
    // dangle. Otherwise async_runtime_global() would hand out the freed pointer
    // instead of lazily recreating a fresh, started runtime.
    pthread_mutex_lock(&g_runtime_mutex);
    if (g_global_runtime == runtime) {
        g_global_runtime = NULL;
    }
    pthread_mutex_unlock(&g_runtime_mutex);

    // Destroy executors
    if (runtime->inline_executor) {
        free(runtime->inline_executor);
    }
    
    // Destroy synchronization objects
    pthread_mutex_destroy(&runtime->runtime_mutex);
    pthread_cond_destroy(&runtime->shutdown_cond);
    
    free(runtime);
}

Result_void_ptr async_runtime_start(AsyncRuntime* runtime) {
    if (!runtime || !runtime->is_initialized) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Runtime not initialized"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&runtime->runtime_mutex);
    
    if (runtime->is_running) {
        pthread_mutex_unlock(&runtime->runtime_mutex);
        return OK_PTR(NULL); // Already running
    }
    
    runtime->is_running = true;
    pthread_mutex_unlock(&runtime->runtime_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr async_runtime_shutdown(AsyncRuntime* runtime, uint64_t timeout_ms) {
    if (!runtime) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Invalid runtime"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&runtime->runtime_mutex);
    
    if (!runtime->is_running) {
        pthread_mutex_unlock(&runtime->runtime_mutex);
        return OK_PTR(NULL); // Already shut down
    }
    
    runtime->is_running = false;
    
    // Wait for shutdown with timeout
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
    
    pthread_cond_timedwait(&runtime->shutdown_cond, &runtime->runtime_mutex, &deadline);
    pthread_mutex_unlock(&runtime->runtime_mutex);
    
    return OK_PTR(NULL);
}

// Task submission
Result_void_ptr async_task_submit(AsyncRuntime* runtime, AsyncTask* task) {
    if (!runtime || !task) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Invalid runtime or task"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    if (!runtime->is_running) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OPERATION_FAILED,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Runtime is not running"),
            .hint = strdup("Start the runtime first"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Assign task ID
    task->context.task_id = atomic_fetch_add(&runtime->next_task_id, 1);
    
    // Select appropriate executor
    AsyncExecutor* executor = async_select_executor(runtime, task);
    if (!executor) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OPERATION_FAILED,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("No suitable executor available"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Update statistics
    atomic_fetch_add(&runtime->total_submitted_tasks, 1);
    atomic_fetch_add(&runtime->active_task_count, 1);
    
    // Submit to executor
    return executor->submit_task(executor, task);
}

AsyncExecutor* async_select_executor(AsyncRuntime* runtime, AsyncTask* task) {
    if (!runtime || !task) return NULL;
    
    ExecutorType recommended_type = async_recommend_executor_type(task);
    
    switch (recommended_type) {
        case EXECUTOR_TYPE_INLINE:
            return runtime->inline_executor;
        
        case EXECUTOR_TYPE_THREAD_POOL:
            return runtime->thread_pool_executor ? runtime->thread_pool_executor : runtime->inline_executor;
        
        case EXECUTOR_TYPE_WORK_STEALING:
            return runtime->work_stealing_executor ? runtime->work_stealing_executor : runtime->inline_executor;
        
        case EXECUTOR_TYPE_NUMA_AWARE:
            return runtime->numa_executor ? runtime->numa_executor : runtime->inline_executor;
        
        case EXECUTOR_TYPE_IO_OPTIMIZED:
            return runtime->io_executor ? runtime->io_executor : runtime->inline_executor;
        
        default:
            return runtime->inline_executor;
    }
}

// Performance monitoring
AsyncPerformanceMetrics async_runtime_get_metrics(AsyncRuntime* runtime) {
    AsyncPerformanceMetrics metrics = {0};
    
    if (!runtime) return metrics;
    
    metrics.total_tasks_submitted = atomic_load(&runtime->total_submitted_tasks);
    metrics.total_tasks_completed = atomic_load(&runtime->total_completed_tasks);
    metrics.total_tasks_failed = atomic_load(&runtime->total_failed_tasks);
    metrics.total_tasks_cancelled = atomic_load(&runtime->total_cancelled_tasks);
    
    // Calculate averages and other derived metrics
    if (metrics.total_tasks_completed > 0) {
        metrics.average_task_execution_time_ns = 1000000; // Placeholder
        metrics.average_task_wait_time_ns = 500000;       // Placeholder
        metrics.max_task_execution_time_ns = 10000000;    // Placeholder
    }
    
    metrics.active_worker_count = runtime->worker_count;
    metrics.idle_worker_count = 0; // Placeholder
    metrics.cpu_utilization_percent = 50.0; // Placeholder
    
    metrics.memory_usage_bytes = 1024 * 1024; // Placeholder
    metrics.peak_memory_usage_bytes = 2 * 1024 * 1024; // Placeholder
    
    return metrics;
}

void async_runtime_print_metrics(AsyncRuntime* runtime) {
    if (!runtime) return;
    
    AsyncPerformanceMetrics metrics = async_runtime_get_metrics(runtime);
    
    printf("=== Async Runtime Performance Metrics ===\n");
    printf("Tasks submitted: %llu\n", metrics.total_tasks_submitted);
    printf("Tasks completed: %llu\n", metrics.total_tasks_completed);
    printf("Tasks failed: %llu\n", metrics.total_tasks_failed);
    printf("Tasks cancelled: %llu\n", metrics.total_tasks_cancelled);
    
    if (metrics.total_tasks_completed > 0) {
        printf("Average execution time: %.3f ms\n", metrics.average_task_execution_time_ns / 1e6);
        printf("Average wait time: %.3f ms\n", metrics.average_task_wait_time_ns / 1e6);
        printf("Max execution time: %.3f ms\n", metrics.max_task_execution_time_ns / 1e6);
    }
    
    printf("Active workers: %u\n", metrics.active_worker_count);
    printf("CPU utilization: %.1f%%\n", metrics.cpu_utilization_percent);
    printf("Memory usage: %.2f MB\n", metrics.memory_usage_bytes / (1024.0 * 1024.0));
    printf("Peak memory: %.2f MB\n", metrics.peak_memory_usage_bytes / (1024.0 * 1024.0));
}

// Global runtime management
AsyncRuntime* async_get_global_runtime(void) {
    pthread_mutex_lock(&g_runtime_mutex);
    AsyncRuntime* runtime = g_global_runtime;
    pthread_mutex_unlock(&g_runtime_mutex);
    return runtime;
}

Result_void_ptr async_set_global_runtime(AsyncRuntime* runtime) {
    pthread_mutex_lock(&g_runtime_mutex);
    g_global_runtime = runtime;
    pthread_mutex_unlock(&g_runtime_mutex);
    return OK_PTR(NULL);
}

// Transparent async execution helpers
Result_void_ptr async_submit_transparent(AsyncFunction function, void* context, size_t context_size) {
    AsyncRuntime* runtime = async_get_global_runtime();
    if (!runtime) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OPERATION_FAILED,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("No global runtime available"),
            .hint = strdup("Initialize a global runtime first"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    AsyncTask* task = async_task_create(function, context, context_size);
    if (!task) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Failed to create async task"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    return async_task_submit(runtime, task);
}

Result_void_ptr async_await_transparent(AsyncFuture* future) {
    return async_future_get(future, 30000); // 30 second default timeout
}

AsyncFuture* async_spawn_detached(AsyncFunction function, void* context, size_t context_size) {
    AsyncTask* task = async_task_create(function, context, context_size);
    if (!task) return NULL;
    
    AsyncRuntime* runtime = async_get_global_runtime();
    if (runtime) {
        async_task_submit(runtime, task);
    }
    
    return async_future_create(task);
}

// Get global runtime (singleton pattern)
AsyncRuntime* async_runtime_global(void) {
    pthread_mutex_lock(&g_runtime_mutex);
    
    if (!g_global_runtime) {
        // Create default runtime on first access
        AsyncRuntimeConfig config = async_runtime_config_default();
        g_global_runtime = async_runtime_create(config);
        if (g_global_runtime) {
            async_runtime_start(g_global_runtime);
        }
    }
    
    pthread_mutex_unlock(&g_runtime_mutex);
    return g_global_runtime;
}

// Get runtime statistics
RuntimeStats async_runtime_get_stats(AsyncRuntime* runtime) {
    RuntimeStats stats = {0};
    
    if (!runtime) return stats;
    
    stats.total_tasks = atomic_load(&runtime->total_submitted_tasks);
    stats.pending_tasks = atomic_load(&runtime->active_task_count);
    stats.active_workers = runtime->worker_count;
    
    // Calculate CPU utilization (simplified)
    if (stats.active_workers > 0) {
        stats.cpu_utilization = (double)stats.pending_tasks / stats.active_workers;
        if (stats.cpu_utilization > 1.0) stats.cpu_utilization = 1.0;
    }
    
    return stats;
}

// Submit task to runtime and return future
AsyncFuture* async_runtime_submit(AsyncRuntime* runtime, AsyncTask* task) {
    if (!runtime || !task) return NULL;
    
    Result_void_ptr submit_result = async_task_submit(runtime, task);
    if (submit_result.is_error) {
        // Clean up error
        if (submit_result.error) {
            free((void*)submit_result.error->message);
            free((void*)submit_result.error->hint);
            free(submit_result.error);
        }
        async_task_destroy(task);
        return NULL;
    }
    
    return async_future_create(task);
}