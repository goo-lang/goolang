#ifndef GOO_STRUCTURED_CONCURRENCY_H
#define GOO_STRUCTURED_CONCURRENCY_H

#include "runtime.h"
#include "actor_system.h"
#include "shared_variables.h"
#include "error_hierarchies.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

// =============================================================================
// Structured Concurrency Framework
// =============================================================================

// Forward declarations
typedef struct ConcurrencyScope ConcurrencyScope;
typedef struct ConcurrentTask ConcurrentTask;
typedef struct ParallelBlock ParallelBlock;
typedef struct CancellationToken CancellationToken;
typedef struct TaskGroup TaskGroup;
typedef struct ConcurrencyScheduler ConcurrencyScheduler;
typedef struct TaskDependency TaskDependency;

// =============================================================================
// Task States and Types
// =============================================================================

// States a concurrent task can be in
typedef enum {
    TASK_STATE_CREATED,     // Task created but not started
    TASK_STATE_WAITING,     // Task waiting for dependencies
    TASK_STATE_READY,       // Task ready to execute
    TASK_STATE_RUNNING,     // Task currently executing
    TASK_STATE_SUSPENDED,   // Task suspended by scheduler
    TASK_STATE_COMPLETED,   // Task completed successfully
    TASK_STATE_FAILED,      // Task failed with error
    TASK_STATE_CANCELLED,   // Task was cancelled
    TASK_STATE_TIMEOUT      // Task timed out
} TaskState;

// Types of concurrent tasks
typedef enum {
    TASK_TYPE_FUNCTION,     // Simple function execution
    TASK_TYPE_PARALLEL,     // Part of parallel block
    TASK_TYPE_PIPELINE,     // Pipeline stage
    TASK_TYPE_STREAM,       // Stream processing
    TASK_TYPE_BACKGROUND,   // Background/daemon task
    TASK_TYPE_PERIODIC      // Periodic/scheduled task
} TaskType;

// Cancellation reasons
typedef enum {
    CANCEL_REASON_USER,         // User-requested cancellation
    CANCEL_REASON_TIMEOUT,      // Task timed out
    CANCEL_REASON_ERROR,        // Error in sibling task
    CANCEL_REASON_PARENT,       // Parent scope cancelled
    CANCEL_REASON_RESOURCE,     // Resource exhaustion
    CANCEL_REASON_SHUTDOWN      // System shutdown
} CancellationReason;

// Task scheduling policies
typedef enum {
    SCHEDULE_POLICY_FAIR,       // Fair scheduling
    SCHEDULE_POLICY_PRIORITY,   // Priority-based scheduling
    SCHEDULE_POLICY_DEADLINE,   // Deadline scheduling
    SCHEDULE_POLICY_AFFINITY,   // CPU affinity scheduling
    SCHEDULE_POLICY_NUMA        // NUMA-aware scheduling
} SchedulePolicy;

// =============================================================================
// Task Functions and Callbacks
// =============================================================================

// Task execution function signature
typedef void* (*TaskFunction)(void* context, CancellationToken* token);

// Task completion callback
typedef void (*TaskCompletionCallback)(ConcurrentTask* task, void* result, void* context);

// Task error callback
typedef void (*TaskErrorCallback)(ConcurrentTask* task, StructuredError* error, void* context);

// Task progress callback
typedef void (*TaskProgressCallback)(ConcurrentTask* task, double progress, void* context);

// Cancellation callback
typedef void (*CancellationCallback)(ConcurrentTask* task, CancellationReason reason, void* context);

// =============================================================================
// Cancellation Token System
// =============================================================================

// Cancellation token for cooperative cancellation
typedef struct CancellationToken {
    uint64_t token_id;              // Unique token ID
    atomic_bool is_cancelled;       // Cancellation flag
    CancellationReason reason;      // Reason for cancellation
    uint64_t cancelled_at;          // Timestamp of cancellation
    
    // Parent-child relationships
    CancellationToken* parent;      // Parent token
    CancellationToken** children;   // Child tokens
    int child_count;                // Number of children
    int child_capacity;             // Child array capacity
    
    // Callbacks
    CancellationCallback* callbacks; // Cancellation callbacks
    void** callback_contexts;       // Callback contexts
    int callback_count;             // Number of callbacks
    
    // Synchronization
    pthread_mutex_t mutex;          // Token protection
    pthread_cond_t cancelled_cond;  // Cancellation notification
    
    // Memory management
    Arena* token_arena;             // Token-specific memory
    
} CancellationToken;

// =============================================================================
// Task Configuration and Statistics
// =============================================================================

// Configuration for concurrent tasks
typedef struct TaskConfig {
    const char* name;               // Task name (for debugging)
    TaskType type;                  // Type of task
    int priority;                   // Task priority (0-100)
    uint64_t timeout_ms;            // Timeout in milliseconds
    uint64_t deadline_ms;           // Deadline for completion
    
    // Resource limits
    size_t memory_limit;            // Memory usage limit
    uint64_t cpu_time_limit_ms;     // CPU time limit
    int max_retries;                // Maximum retry attempts
    
    // Scheduling
    SchedulePolicy schedule_policy; // Scheduling policy
    int cpu_affinity;               // Preferred CPU core (-1 for any)
    bool numa_aware;                // NUMA-aware placement
    
    // Callbacks
    TaskCompletionCallback on_complete;  // Completion callback
    TaskErrorCallback on_error;          // Error callback
    TaskProgressCallback on_progress;    // Progress callback
    void* callback_context;              // Callback context
    
} TaskConfig;

// Task execution statistics
typedef struct TaskStats {
    uint64_t created_at;            // Task creation time
    uint64_t started_at;            // Task start time
    uint64_t completed_at;          // Task completion time
    uint64_t cpu_time_ns;           // CPU time consumed
    uint64_t wall_time_ns;          // Wall clock time
    
    size_t memory_peak;             // Peak memory usage
    size_t memory_current;          // Current memory usage
    
    int retry_count;                // Number of retries
    int preemption_count;           // Times preempted
    int migration_count;            // CPU migrations
    
    double progress;                // Current progress (0.0-1.0)
    
} TaskStats;

// =============================================================================
// Concurrent Task Structure
// =============================================================================

// Core concurrent task structure
typedef struct ConcurrentTask {
    // Identification
    uint64_t task_id;               // Unique task ID
    const char* name;               // Task name
    TaskType type;                  // Task type
    TaskState state;                // Current state
    
    // Execution
    TaskFunction function;          // Function to execute
    void* context;                  // Function context
    void* result;                   // Task result
    size_t result_size;             // Result size
    
    // Error handling
    StructuredError* error;         // Task error (if any)
    bool owns_error;                // Whether task owns error
    
    // Configuration and stats
    TaskConfig config;              // Task configuration
    TaskStats stats;                // Execution statistics
    
    // Cancellation
    CancellationToken* cancellation_token; // Cancellation token
    bool is_cancellation_requested; // Cancellation requested
    
    // Dependencies
    TaskDependency** dependencies;  // Task dependencies
    int dependency_count;           // Number of dependencies
    ConcurrentTask** dependents;    // Tasks depending on this one
    int dependent_count;            // Number of dependents
    
    // Parent-child relationships
    ConcurrencyScope* parent_scope; // Parent scope
    TaskGroup* task_group;          // Task group (if any)
    
    // Synchronization
    pthread_mutex_t task_mutex;     // Task state protection
    pthread_cond_t state_changed;   // State change notification
    ActorFuture* future;            // Future for async result
    
    // Scheduling
    int current_cpu;                // Current CPU core
    uint64_t quantum_start;         // Time quantum start
    uint64_t last_scheduled;        // Last scheduling time
    
    // Memory management
    Arena* task_arena;              // Task-specific memory
    
} ConcurrentTask;

// =============================================================================
// Task Dependencies
// =============================================================================

// Types of task dependencies
typedef enum {
    DEPENDENCY_SEQUENTIAL,      // Task must complete before dependent starts
    DEPENDENCY_DATA,           // Data dependency (result passed to dependent)
    DEPENDENCY_RESOURCE,       // Resource dependency (shared resource)
    DEPENDENCY_BARRIER,        // Barrier dependency (all must reach point)
    DEPENDENCY_CONDITIONAL     // Conditional dependency (depends on result)
} DependencyType;

// Task dependency structure
typedef struct TaskDependency {
    uint64_t dependency_id;         // Unique dependency ID
    DependencyType type;            // Type of dependency
    ConcurrentTask* source_task;    // Source task
    ConcurrentTask* target_task;    // Target task
    
    // Conditions
    bool (*condition_func)(void* source_result, void* context); // Condition function
    void* condition_context;        // Condition context
    
    // Data transformation
    void* (*transform_func)(void* source_result, void* context); // Transform function
    void* transform_context;        // Transform context
    
    bool is_satisfied;              // Whether dependency is satisfied
    uint64_t satisfied_at;          // When dependency was satisfied
    
} TaskDependency;

// =============================================================================
// Task Groups and Parallel Blocks
// =============================================================================

// Task group for managing related tasks
typedef struct TaskGroup {
    uint64_t group_id;              // Unique group ID
    const char* name;               // Group name
    
    // Tasks
    ConcurrentTask** tasks;         // Tasks in this group
    int task_count;                 // Number of tasks
    int task_capacity;              // Task array capacity
    
    // Group behavior
    bool fail_fast;                 // Cancel all on first failure
    bool wait_for_all;              // Wait for all tasks to complete
    int required_success_count;     // Minimum successful tasks needed
    
    // State tracking
    atomic_int completed_count;     // Completed tasks
    atomic_int failed_count;        // Failed tasks
    atomic_int cancelled_count;     // Cancelled tasks
    
    // Results aggregation
    void** results;                 // Task results
    StructuredError** errors;       // Task errors
    
    // Synchronization
    pthread_mutex_t group_mutex;    // Group protection
    pthread_cond_t completion_cond; // Completion notification
    
    // Parent scope and cancellation
    ConcurrencyScope* parent_scope; // Parent scope
    CancellationToken* cancellation_token; // Group cancellation
    
} TaskGroup;

// Parallel block for structured parallel execution
typedef struct ParallelBlock {
    uint64_t block_id;              // Unique block ID
    const char* name;               // Block name
    
    // Execution mode
    enum {
        PARALLEL_ALL,               // Execute all tasks in parallel
        PARALLEL_RACE,              // Stop on first completion
        PARALLEL_QUORUM,            // Stop when quorum reached
        PARALLEL_PIPELINE           // Pipeline execution
    } execution_mode;
    
    // Tasks and groups
    TaskGroup* task_group;          // Associated task group
    ConcurrentTask** tasks;         // Tasks in block
    int task_count;                 // Number of tasks
    
    // Configuration
    int max_parallelism;            // Maximum concurrent tasks
    uint64_t timeout_ms;            // Block timeout
    bool preserve_order;            // Preserve result order
    
    // Results
    void** results;                 // Block results
    int result_count;               // Number of results
    StructuredError* block_error;   // Block-level error
    
    // Parent scope
    ConcurrencyScope* parent_scope; // Parent scope
    
} ParallelBlock;

// =============================================================================
// Concurrency Scope
// =============================================================================

// Concurrency scope for structured concurrency
typedef struct ConcurrencyScope {
    uint64_t scope_id;              // Unique scope ID
    const char* name;               // Scope name
    
    // Hierarchical structure
    ConcurrencyScope* parent;       // Parent scope
    ConcurrencyScope** children;    // Child scopes
    int child_count;                // Number of children
    int child_capacity;             // Child array capacity
    
    // Tasks and groups
    ConcurrentTask** tasks;         // Tasks in this scope
    int task_count;                 // Number of tasks
    TaskGroup** task_groups;        // Task groups in scope
    int group_count;                // Number of groups
    ParallelBlock** parallel_blocks; // Parallel blocks
    int block_count;                // Number of blocks
    
    // Cancellation
    CancellationToken* cancellation_token; // Scope cancellation
    bool auto_cancel_on_error;      // Cancel all on first error
    
    // Resource management
    size_t memory_limit;            // Memory limit for scope
    size_t memory_used;             // Currently used memory
    int thread_limit;               // Thread limit for scope
    int active_threads;             // Currently active threads
    
    // Statistics
    struct {
        uint64_t created_at;
        uint64_t total_tasks_created;
        uint64_t total_tasks_completed;
        uint64_t total_tasks_failed;
        uint64_t total_tasks_cancelled;
        double success_rate;
        uint64_t average_task_duration_ns;
    } stats;
    
    // Synchronization
    pthread_mutex_t scope_mutex;    // Scope protection
    pthread_cond_t empty_cond;      // All tasks completed
    
    // Memory management
    Arena* scope_arena;             // Scope-specific memory
    
} ConcurrencyScope;

// =============================================================================
// Concurrency Scheduler
// =============================================================================

// Work-stealing scheduler for concurrent tasks
typedef struct ConcurrencyScheduler {
    const char* scheduler_name;     // Scheduler name
    
    // Worker threads
    pthread_t* worker_threads;      // Worker thread pool
    int worker_count;               // Number of workers
    bool workers_active;            // Whether workers are running
    
    // Task queues (per-worker)
    struct WorkerQueue {
        ConcurrentTask** tasks;     // Task queue
        int front, back;            // Queue pointers
        int capacity;               // Queue capacity
        pthread_mutex_t mutex;      // Queue protection
        pthread_cond_t not_empty;   // Queue not empty condition
        
        // Work stealing
        atomic_int steal_index;     // Steal index for work stealing
        uint64_t steals_performed;  // Number of steals performed
        uint64_t steals_received;   // Number of times stolen from
        
        // Statistics
        uint64_t tasks_executed;    // Tasks executed by this worker
        uint64_t idle_time_ns;      // Time spent idle
        uint64_t busy_time_ns;      // Time spent executing tasks
        
    } *worker_queues;
    
    // Global scheduling
    ConcurrentTask** global_queue;  // Global task queue
    int global_front, global_back;  // Global queue pointers
    int global_capacity;            // Global queue capacity
    pthread_mutex_t global_mutex;   // Global queue protection
    pthread_cond_t global_not_empty; // Global queue condition
    
    // Scheduling policies
    SchedulePolicy default_policy;  // Default scheduling policy
    bool work_stealing_enabled;     // Work stealing enabled
    bool numa_aware;                // NUMA-aware scheduling
    
    // Resource management
    int max_threads;                // Maximum threads
    size_t memory_limit;            // Memory limit
    size_t memory_used;             // Current memory usage
    
    // Statistics
    struct {
        uint64_t tasks_scheduled;
        uint64_t tasks_completed;
        uint64_t tasks_failed;
        uint64_t context_switches;
        uint64_t work_steals;
        double average_queue_length;
        double cpu_utilization;
    } stats;
    
    // Configuration
    struct {
        uint64_t quantum_ms;        // Time quantum for preemption
        uint64_t idle_timeout_ms;   // Worker idle timeout
        int steal_attempts;         // Max steal attempts
        bool adaptive_scheduling;   // Adaptive scheduling
    } config;
    
    // Memory management
    Arena* scheduler_arena;         // Scheduler memory
    
} ConcurrencyScheduler;

// =============================================================================
// Core API Functions
// =============================================================================

// Scope management
ConcurrencyScope* concurrency_scope_create(const char* name, ConcurrencyScope* parent);
void concurrency_scope_destroy(ConcurrencyScope* scope);
bool concurrency_scope_wait_for_completion(ConcurrencyScope* scope, uint64_t timeout_ms);
void concurrency_scope_cancel_all(ConcurrencyScope* scope, CancellationReason reason);

// Task creation and management
ConcurrentTask* concurrent_task_create(const char* name, TaskFunction function, void* context);
ConcurrentTask* concurrent_task_create_with_config(const TaskConfig* config, TaskFunction function, void* context);
void concurrent_task_destroy(ConcurrentTask* task);

bool concurrent_task_start(ConcurrentTask* task);
bool concurrent_task_cancel(ConcurrentTask* task, CancellationReason reason);
void* concurrent_task_await(ConcurrentTask* task);
void* concurrent_task_await_timeout(ConcurrentTask* task, uint64_t timeout_ms);

// Task state management
TaskState concurrent_task_get_state(ConcurrentTask* task);
bool concurrent_task_is_completed(ConcurrentTask* task);
bool concurrent_task_is_cancelled(ConcurrentTask* task);
StructuredError* concurrent_task_get_error(ConcurrentTask* task);

// Cancellation tokens
CancellationToken* cancellation_token_create(CancellationToken* parent);
void cancellation_token_destroy(CancellationToken* token);
bool cancellation_token_is_cancelled(CancellationToken* token);
void cancellation_token_cancel(CancellationToken* token, CancellationReason reason);
bool cancellation_token_wait_for_cancellation(CancellationToken* token, uint64_t timeout_ms);

// Task dependencies
TaskDependency* task_dependency_create(ConcurrentTask* source, ConcurrentTask* target, DependencyType type);
void task_dependency_destroy(TaskDependency* dependency);
bool task_dependency_add_condition(TaskDependency* dependency, 
                                  bool (*condition)(void*, void*), void* context);

// Task groups
TaskGroup* task_group_create(const char* name, ConcurrencyScope* parent_scope);
void task_group_destroy(TaskGroup* group);
bool task_group_add_task(TaskGroup* group, ConcurrentTask* task);
bool task_group_wait_for_completion(TaskGroup* group, uint64_t timeout_ms);
void** task_group_get_results(TaskGroup* group, int* result_count);

// Parallel blocks
ParallelBlock* parallel_block_create(const char* name, ConcurrencyScope* parent_scope);
void parallel_block_destroy(ParallelBlock* block);
bool parallel_block_add_task(ParallelBlock* block, ConcurrentTask* task);
void** parallel_block_execute_all(ParallelBlock* block, uint64_t timeout_ms);
void* parallel_block_execute_race(ParallelBlock* block, uint64_t timeout_ms);

// =============================================================================
// High-Level Parallel Constructs
// =============================================================================

// Parallel execution macros and functions
#define PARALLEL_BLOCK(name) \
    for (ParallelBlock* _pb = parallel_block_create(name, get_current_scope()); \
         _pb; parallel_block_destroy(_pb), _pb = NULL)

#define PARALLEL_TASK(name, func, ctx) \
    parallel_block_add_task(get_current_parallel_block(), \
                           concurrent_task_create(name, func, ctx))

// Structured parallel execution
void** parallel_execute(ConcurrencyScope* scope, TaskFunction* functions, 
                        void** contexts, int task_count, uint64_t timeout_ms);

// Parallel for loop
void parallel_for_structured(ConcurrencyScope* scope, int64_t start, int64_t end, 
                             void (*body)(int64_t index, void* context), void* context);

// Parallel reduce
void* parallel_reduce_structured(ConcurrencyScope* scope, void** items, int item_count,
                                void* initial_value, 
                                void* (*reduce_func)(void* acc, void* item, void* context),
                                void* context);

// Pipeline execution
typedef struct Pipeline Pipeline;
Pipeline* pipeline_create(const char* name, ConcurrencyScope* scope);
bool pipeline_add_stage(Pipeline* pipeline, TaskFunction stage_func, void* context);
void** pipeline_execute(Pipeline* pipeline, void** inputs, int input_count);
void pipeline_destroy(Pipeline* pipeline);

// =============================================================================
// Scheduler Management
// =============================================================================

// Scheduler creation and management
ConcurrencyScheduler* concurrency_scheduler_create(const char* name, int worker_count);
void concurrency_scheduler_destroy(ConcurrencyScheduler* scheduler);
bool concurrency_scheduler_start(ConcurrencyScheduler* scheduler);
void concurrency_scheduler_stop(ConcurrencyScheduler* scheduler);

// Task scheduling
bool concurrency_scheduler_schedule(ConcurrencyScheduler* scheduler, ConcurrentTask* task);
bool concurrency_scheduler_yield(ConcurrencyScheduler* scheduler);
void concurrency_scheduler_set_policy(ConcurrencyScheduler* scheduler, SchedulePolicy policy);

// Global scheduler access
ConcurrencyScheduler* get_global_concurrency_scheduler(void);
ConcurrencyScope* get_current_scope(void);
ParallelBlock* get_current_parallel_block(void);

// =============================================================================
// Error Handling and Recovery
// =============================================================================

// Error aggregation for parallel tasks
typedef struct ErrorSummary {
    StructuredError** errors;       // Array of errors
    int error_count;                // Number of errors
    int total_tasks;                // Total tasks that ran
    double failure_rate;            // Failure rate (0.0-1.0)
    StructuredError* primary_error; // Primary/first error
} ErrorSummary;

ErrorSummary* error_summary_create_from_tasks(ConcurrentTask** tasks, int task_count);
void error_summary_destroy(ErrorSummary* summary);

// Recovery strategies
typedef enum {
    RECOVERY_FAIL_FAST,             // Fail immediately on first error
    RECOVERY_COLLECT_ALL,           // Collect all errors before failing
    RECOVERY_RETRY,                 // Retry failed tasks
    RECOVERY_FALLBACK,              // Use fallback values
    RECOVERY_PARTIAL_SUCCESS        // Accept partial success
} RecoveryStrategy;

bool apply_recovery_strategy(TaskGroup* group, RecoveryStrategy strategy, void* recovery_context);

// =============================================================================
// Performance Monitoring and Debugging
// =============================================================================

// Performance metrics
typedef struct ConcurrencyMetrics {
    uint64_t total_tasks_created;
    uint64_t total_tasks_completed;
    uint64_t total_tasks_failed;
    uint64_t total_tasks_cancelled;
    
    double average_task_duration_ms;
    double average_queue_time_ms;
    double cpu_utilization;
    double memory_utilization;
    
    uint64_t context_switches;
    uint64_t work_steals;
    uint64_t cache_misses;
    
} ConcurrencyMetrics;

ConcurrencyMetrics get_concurrency_metrics(ConcurrencyScheduler* scheduler);
void print_concurrency_metrics(const ConcurrencyMetrics* metrics);

// Debugging support
void dump_scope_info(ConcurrencyScope* scope);
void dump_task_info(ConcurrentTask* task);
void dump_scheduler_info(ConcurrencyScheduler* scheduler);

// Tracing and profiling
typedef struct ConcurrencyTrace ConcurrencyTrace;
ConcurrencyTrace* concurrency_trace_start(const char* name);
void concurrency_trace_end(ConcurrencyTrace* trace);
void concurrency_trace_export(ConcurrencyTrace* trace, const char* filename);

// =============================================================================
// Integration with Actor System and Shared Variables
// =============================================================================

// Actor-based task execution
ConcurrentTask* concurrent_task_create_from_actor(ActorRef* actor, const char* handler_name, 
                                                  void* message, size_t message_size);

// Shared variable integration
bool concurrent_task_add_shared_variable(ConcurrentTask* task, SharedVar* var, const char* name);
SharedVar* concurrent_task_get_shared_variable(ConcurrentTask* task, const char* name);

// Channel integration for task communication
typedef struct TaskChannel TaskChannel;
TaskChannel* task_channel_create(const char* name, size_t capacity);
bool task_channel_send(TaskChannel* channel, ConcurrentTask* sender, void* data, size_t size);
void* task_channel_receive(TaskChannel* channel, ConcurrentTask* receiver, uint64_t timeout_ms);
void task_channel_destroy(TaskChannel* channel);

#endif // GOO_STRUCTURED_CONCURRENCY_H