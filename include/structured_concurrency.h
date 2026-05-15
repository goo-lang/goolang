#ifndef GOO_STRUCTURED_CONCURRENCY_H
#define GOO_STRUCTURED_CONCURRENCY_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "ccomp_shim.h"
#include "ergonomic_errors.h"
#include "fearless_concurrency.h"
#include "shared_variables.h"

// Forward declarations
typedef struct TaskScope TaskScope;
typedef struct ConcurrentTask ConcurrentTask;
typedef struct TaskGroup TaskGroup;
typedef struct CoroutineScheduler CoroutineScheduler;
typedef struct Coroutine Coroutine;

// Task scope types for structured concurrency
typedef enum {
    SCOPE_TYPE_PARALLEL,        // All tasks run in parallel
    SCOPE_TYPE_SEQUENTIAL,      // Tasks run one after another
    SCOPE_TYPE_PIPELINE,        // Tasks form a pipeline
    SCOPE_TYPE_MAP_REDUCE,      // Map-reduce pattern
    SCOPE_TYPE_WORKER_POOL,     // Fixed pool of worker tasks
    SCOPE_TYPE_ASYNC_ITERATOR   // Asynchronous iteration
} TaskScopeType;

// Task priority levels
typedef enum {
    TASK_PRIORITY_LOW = 0,
    TASK_PRIORITY_NORMAL = 1,
    TASK_PRIORITY_HIGH = 2,
    TASK_PRIORITY_CRITICAL = 3
} TaskPriority;

// Task state tracking
typedef enum {
    TASK_STATE_CREATED,
    TASK_STATE_QUEUED,
    TASK_STATE_RUNNING,
    TASK_STATE_WAITING,
    TASK_STATE_COMPLETED,
    TASK_STATE_CANCELLED,
    TASK_STATE_ERROR
} TaskState;

// Cancellation strategy
typedef enum {
    CANCEL_GRACEFUL,      // Allow task to complete current operation
    CANCEL_IMMEDIATE,     // Cancel task immediately
    CANCEL_COOPERATIVE    // Task checks cancellation token
} CancellationStrategy;

// Task execution context
typedef struct TaskContext {
    uint64_t task_id;
    TaskScope* scope;
    CancellationStrategy cancel_strategy;
    
    // Cancellation token
    atomic_bool is_cancelled;
    atomic_bool cancel_requested;
    
    // Progress tracking
    atomic_int progress_percentage;
    char progress_message[256];
    
    // Resource limits
    uint64_t max_memory_bytes;
    uint64_t max_execution_time_ms;
    
    // Statistics
    uint64_t start_time_ns;
    uint64_t end_time_ns;
    uint64_t cpu_time_ns;
    size_t memory_used_bytes;
    
    // Error context integration
    ErgoErrorContext* error_context;
} TaskContext;

// Task function signature
typedef Result_void_ptr (*TaskFunction)(TaskContext* context, void* args);

// Task dependency specification
typedef struct TaskDependency {
    uint64_t dependency_task_id;
    bool is_required;           // If false, task can start even if dependency fails
    bool wait_for_completion;   // If false, task can start when dependency starts
    
    struct TaskDependency* next;
} TaskDependency;

// Concurrent task structure
typedef struct ConcurrentTask {
    uint64_t id;
    char name[64];
    
    // Task execution
    TaskFunction function;
    void* arguments;
    size_t args_size;
    
    // Task metadata
    TaskState state;
    TaskPriority priority;
    TaskContext context;
    
    // Dependencies
    TaskDependency* dependencies;
    size_t dependency_count;
    
    // Results
    void* result;
    size_t result_size;
    Error* error;
    
    // Synchronization
    pthread_mutex_t task_mutex;
    pthread_cond_t state_changed;
    
    // Reference counting
    atomic_int ref_count;
    
    // Linked list for task groups
    struct ConcurrentTask* next;
    struct ConcurrentTask* prev;
} ConcurrentTask;

// Task group for related tasks
typedef struct TaskGroup {
    uint64_t id;
    char name[64];
    
    // Tasks in this group
    ConcurrentTask* first_task;
    ConcurrentTask* last_task;
    size_t task_count;
    
    // Group behavior
    bool fail_fast;             // Stop all tasks if one fails
    bool wait_for_all;          // Wait for all tasks to complete
    uint64_t timeout_ms;        // Group timeout
    
    // Synchronization
    pthread_mutex_t group_mutex;
    pthread_cond_t all_completed;
    
    // Statistics
    size_t completed_tasks;
    size_t failed_tasks;
    size_t cancelled_tasks;
    
    // Resource management
    size_t max_concurrent_tasks;
    size_t active_tasks;
    
    struct TaskGroup* next;
} TaskGroup;

// Task scope configuration
typedef struct TaskScopeConfig {
    TaskScopeType type;
    size_t max_concurrent_tasks;
    size_t max_total_tasks;
    
    // Resource limits
    size_t max_memory_per_task;
    size_t max_total_memory;
    uint64_t default_task_timeout_ms;
    
    // Scheduling
    bool use_work_stealing;
    bool numa_aware_scheduling;
    TaskPriority default_priority;
    
    // Error handling
    bool propagate_errors;
    bool collect_all_errors;
    CancellationStrategy default_cancel_strategy;
    
    // Integration
    ActorSystem* actor_system;
    SharedVarManager* shared_var_manager;
} TaskScopeConfig;

// Task scope for structured concurrency
typedef struct TaskScope {
    uint64_t id;
    char name[64];
    TaskScopeConfig config;
    
    // Task management
    TaskGroup** task_groups;
    size_t group_count;
    size_t group_capacity;
    
    // Worker thread pool
    pthread_t* worker_threads;
    size_t worker_count;
    
    // Task queue
    ConcurrentTask** task_queue;
    size_t queue_head;
    size_t queue_tail;
    size_t queue_size;
    size_t queue_capacity;
    
    // Synchronization
    pthread_mutex_t scope_mutex;
    pthread_cond_t tasks_available;
    pthread_cond_t scope_completed;
    
    // Scope state
    bool is_active;
    bool shutdown_requested;
    atomic_bool has_errors;
    
    // Error collection
    Error** collected_errors;
    size_t error_count;
    size_t error_capacity;
    
    // Statistics
    uint64_t total_tasks_created;
    uint64_t total_tasks_completed;
    uint64_t total_tasks_failed;
    uint64_t total_execution_time_ns;
    
    // Resource tracking
    atomic_size_t current_memory_usage;
    size_t peak_memory_usage;
    
    // Parent scope for nesting
    struct TaskScope* parent_scope;
    struct TaskScope** child_scopes;
    size_t child_count;
    size_t child_capacity;
} TaskScope;

// Coroutine support for lightweight concurrency
typedef enum {
    COROUTINE_STATE_CREATED,
    COROUTINE_STATE_SUSPENDED,
    COROUTINE_STATE_RUNNING,
    COROUTINE_STATE_COMPLETED,
    COROUTINE_STATE_ERROR
} CoroutineState;

typedef struct CoroutineFrame {
    void* stack_base;
    void* stack_top;
    size_t stack_size;
    void* registers[16];  // Platform-specific register storage
    
    struct CoroutineFrame* previous;
} CoroutineFrame;

typedef struct Coroutine {
    uint64_t id;
    char name[64];
    
    // Execution state
    CoroutineState state;
    CoroutineFrame frame;
    
    // Function and arguments
    Result_void_ptr (*function)(struct Coroutine* self, void* args);
    void* arguments;
    
    // Yield values
    void* yielded_value;
    size_t yielded_size;
    
    // Result
    void* result;
    Error* error;
    
    // Scheduler
    CoroutineScheduler* scheduler;
    
    // Synchronization for cooperative multitasking
    bool is_yielded;
    uint64_t resume_time;
    
    struct Coroutine* next;
} Coroutine;

typedef struct CoroutineScheduler {
    Coroutine* active_coroutines;
    Coroutine* suspended_coroutines;
    Coroutine* current_coroutine;
    
    size_t active_count;
    size_t suspended_count;
    
    // Scheduling policy
    enum {
        CORO_SCHED_ROUND_ROBIN,
        CORO_SCHED_PRIORITY,
        CORO_SCHED_FAIR_SHARE
    } scheduling_policy;
    
    // Integration with task scope
    TaskScope* task_scope;
    
    pthread_mutex_t scheduler_mutex;
} CoroutineScheduler;

// Async/await support
typedef struct AsyncOperation {
    uint64_t id;
    
    // Operation state
    bool is_completed;
    void* result;
    Error* error;
    
    // Waiting coroutines
    Coroutine** waiting_coroutines;
    size_t waiting_count;
    size_t waiting_capacity;
    
    // Completion callback
    void (*on_completion)(struct AsyncOperation* op, void* result, Error* error);
    void* callback_context;
    
    pthread_mutex_t operation_mutex;
    pthread_cond_t completed;
} AsyncOperation;

// Structured concurrency operations
TaskScope* task_scope_create(TaskScopeConfig config, const char* name);
void task_scope_destroy(TaskScope* scope);
Result_void_ptr task_scope_start(TaskScope* scope);
Result_void_ptr task_scope_shutdown(TaskScope* scope, uint64_t timeout_ms);

// Task management
ConcurrentTask* task_create(TaskScope* scope, TaskFunction function, void* args, size_t args_size, const char* name);
Result_void_ptr task_add_dependency(ConcurrentTask* task, ConcurrentTask* dependency, bool required, bool wait_completion);
Result_void_ptr task_submit(ConcurrentTask* task);
Result_void_ptr task_cancel(ConcurrentTask* task, CancellationStrategy strategy);
Result_void_ptr task_wait(ConcurrentTask* task, uint64_t timeout_ms);
void task_destroy(ConcurrentTask* task);

// Task groups
TaskGroup* task_group_create(TaskScope* scope, const char* name);
Result_void_ptr task_group_add_task(TaskGroup* group, ConcurrentTask* task);
Result_void_ptr task_group_wait_all(TaskGroup* group, uint64_t timeout_ms);
Result_void_ptr task_group_cancel_all(TaskGroup* group, CancellationStrategy strategy);
void task_group_destroy(TaskGroup* group);

// Structured patterns
typedef struct ParallelForConfig {
    size_t start_index;
    size_t end_index;
    size_t chunk_size;
    size_t max_workers;
    TaskPriority priority;
} ParallelForConfig;

typedef Result_void_ptr (*ParallelForFunction)(size_t index, void* context);

Result_void_ptr task_scope_parallel_for(TaskScope* scope, ParallelForConfig config, 
                                       ParallelForFunction function, void* context);

typedef struct MapReduceConfig {
    void** input_items;
    size_t item_count;
    size_t max_mappers;
    size_t max_reducers;
} MapReduceConfig;

typedef Result_void_ptr (*MapFunction)(void* input, void** output, size_t* output_size);
typedef Result_void_ptr (*ReduceFunction)(void** inputs, size_t input_count, void** output, size_t* output_size);

Result_void_ptr task_scope_map_reduce(TaskScope* scope, MapReduceConfig config,
                                     MapFunction map_fn, ReduceFunction reduce_fn,
                                     void** result, size_t* result_size);

// Pipeline support
typedef struct Pipeline {
    uint64_t id;
    char name[64];
    
    // Pipeline stages
    TaskFunction* stages;
    size_t stage_count;
    
    // Stage configuration
    size_t* buffer_sizes;
    size_t* worker_counts;
    
    // Input/output channels
    void** input_buffers;
    void** output_buffers;
    atomic_size_t* buffer_positions;
    
    // Pipeline state
    bool is_running;
    atomic_bool shutdown_requested;
    
    TaskScope* scope;
    pthread_mutex_t pipeline_mutex;
} Pipeline;

Pipeline* pipeline_create(TaskScope* scope, const char* name, TaskFunction* stages, size_t stage_count);
Result_void_ptr pipeline_start(Pipeline* pipeline);
Result_void_ptr pipeline_push_input(Pipeline* pipeline, void* item, size_t item_size);
Result_void_ptr pipeline_pop_output(Pipeline* pipeline, void** item, size_t* item_size, uint64_t timeout_ms);
Result_void_ptr pipeline_shutdown(Pipeline* pipeline);
void pipeline_destroy(Pipeline* pipeline);

// Coroutine operations
CoroutineScheduler* coroutine_scheduler_create(TaskScope* scope);
void coroutine_scheduler_destroy(CoroutineScheduler* scheduler);

Coroutine* coroutine_create(CoroutineScheduler* scheduler, 
                           Result_void_ptr (*function)(Coroutine* self, void* args),
                           void* args, const char* name);
Result_void_ptr coroutine_start(Coroutine* coro);
Result_void_ptr coroutine_yield(Coroutine* coro, void* value, size_t value_size);
Result_void_ptr coroutine_resume(Coroutine* coro);
Result_void_ptr coroutine_wait(Coroutine* coro, uint64_t timeout_ms);
void coroutine_destroy(Coroutine* coro);

// Async/await operations
AsyncOperation* async_operation_create(void);
void async_operation_complete(AsyncOperation* op, void* result, Error* error);
Result_void_ptr async_operation_await(AsyncOperation* op, uint64_t timeout_ms);
void async_operation_destroy(AsyncOperation* op);

// Utility macros for structured concurrency
#define TASK_SCOPE_WITH_CONFIG(name, config) \
    TaskScope* name = task_scope_create(config, #name); \
    if (!name) { \
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create task scope")); \
    } \
    Result_void_ptr __scope_start_result = task_scope_start(name); \
    if (__scope_start_result.is_error) { \
        task_scope_destroy(name); \
        return __scope_start_result; \
    }

#define TASK_SCOPE(name) \
    TaskScopeConfig __config = task_scope_config_default(); \
    TASK_SCOPE_WITH_CONFIG(name, __config)

#define TASK_SCOPE_END(scope) \
    do { \
        task_scope_shutdown(scope, 30000); \
        task_scope_destroy(scope); \
    } while(0)

#define PARALLEL_TASK(scope, function, args, name) \
    do { \
        ConcurrentTask* __task = task_create(scope, function, args, sizeof(*args), name); \
        if (__task) task_submit(__task); \
    } while(0)

#define WAIT_FOR_TASK(task) \
    do { \
        if (task) { \
            Result_void_ptr __wait_result = task_wait(task, UINT64_MAX); \
            if (__wait_result.is_error) return __wait_result; \
        } \
    } while(0)

// Configuration helpers
TaskScopeConfig task_scope_config_default(void);
TaskScopeConfig task_scope_config_cpu_intensive(void);
TaskScopeConfig task_scope_config_io_intensive(void);
TaskScopeConfig task_scope_config_memory_constrained(void);

// Statistics and monitoring
typedef struct TaskScopeStats {
    uint64_t total_tasks;
    uint64_t completed_tasks;
    uint64_t failed_tasks;
    uint64_t cancelled_tasks;
    
    // Timing statistics
    uint64_t avg_task_duration_ns;
    uint64_t min_task_duration_ns;
    uint64_t max_task_duration_ns;
    
    // Resource usage
    size_t current_memory_usage;
    size_t peak_memory_usage;
    size_t active_workers;
    
    // Queue statistics
    size_t current_queue_size;
    size_t peak_queue_size;
    double queue_utilization;
    
    // Error statistics
    uint64_t error_count;
    double task_success_rate;
} TaskScopeStats;

TaskScopeStats task_scope_get_stats(TaskScope* scope);
void task_scope_reset_stats(TaskScope* scope);

// Integration with existing systems
Result_void_ptr task_scope_integrate_with_actors(TaskScope* scope, ActorSystem* actor_system);
Result_void_ptr task_scope_integrate_with_shared_vars(TaskScope* scope, SharedVarManager* var_manager);

// Cancellation token operations
typedef struct CancellationToken {
    atomic_bool is_cancelled;
    atomic_bool cancel_requested;
    
    // Callbacks for cancellation
    void (**on_cancel_callbacks)(void* context);
    void** callback_contexts;
    size_t callback_count;
    size_t callback_capacity;
    
    pthread_mutex_t token_mutex;
} CancellationToken;

CancellationToken* cancellation_token_create(void);
void cancellation_token_cancel(CancellationToken* token);
bool cancellation_token_is_cancelled(CancellationToken* token);
Result_void_ptr cancellation_token_add_callback(CancellationToken* token, 
                                               void (*callback)(void*), void* context);
void cancellation_token_destroy(CancellationToken* token);

// Helper macros for cancellation
#define CHECK_CANCELLATION(context) \
    do { \
        if (atomic_load(&(context)->is_cancelled)) { \
            Error* error = malloc(sizeof(Error)); \
            *error = (Error){ \
                .code = ERROR_OPERATION_CANCELLED, \
                .severity = ERROR_SEVERITY_ERROR, \
                .category = ERROR_CATEGORY_RUNTIME, \
                .message = strdup("Task was cancelled"), \
                .hint = NULL, \
                .location = (SourceLocation){0}, \
                .next = NULL \
            }; \
            return ERR_PTR(error); \
        } \
    } while(0)

#define UPDATE_PROGRESS(context, percentage, message) \
    do { \
        atomic_store(&(context)->progress_percentage, percentage); \
        strncpy((context)->progress_message, message, sizeof((context)->progress_message) - 1); \
        (context)->progress_message[sizeof((context)->progress_message) - 1] = '\0'; \
    } while(0)

#endif // GOO_STRUCTURED_CONCURRENCY_H