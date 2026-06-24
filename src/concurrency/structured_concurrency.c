#include "../../include/structured_concurrency.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

// Forward declarations and types for parallel for
typedef struct ParallelForTaskArgs {
    size_t start;
    size_t end;
    ParallelForFunction func;
    void* user_context;
} ParallelForTaskArgs;

// Task function wrapper for parallel for
Result_void_ptr parallel_for_task_wrapper(TaskContext* task_ctx, void* args) {
    ParallelForTaskArgs* pf_args = (ParallelForTaskArgs*)args;
    
    // Validate arguments - just return success for invalid ranges to avoid error objects
    if (!pf_args || !pf_args->func || pf_args->start >= pf_args->end) {
        return OK_PTR(NULL);  // Skip invalid range without creating error objects
    }
    
    for (size_t i = pf_args->start; i < pf_args->end; i++) {
        CHECK_CANCELLATION(task_ctx);
        
        Result_void_ptr result = pf_args->func(i, pf_args->user_context);
        if (result.is_error) {
            return result;
        }
        
        // Update progress
        int progress = (int)((i - pf_args->start + 1) * 100 / (pf_args->end - pf_args->start));
        UPDATE_PROGRESS(task_ctx, progress, "Processing items");
    }
    
    return OK_PTR(NULL);
}

// Utility functions
static uint64_t get_monotonic_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t generate_unique_id(void) {
    static atomic_uint_fast64_t counter = 1;
    return atomic_fetch_add(&counter, 1);
}

// Configuration helpers
TaskScopeConfig task_scope_config_default(void) {
    return (TaskScopeConfig) {
        .type = SCOPE_TYPE_PARALLEL,
        .max_concurrent_tasks = 8,
        .max_total_tasks = 1000,
        .max_memory_per_task = 16 * 1024 * 1024,  // 16MB
        .max_total_memory = 256 * 1024 * 1024,    // 256MB
        .default_task_timeout_ms = 30000,         // 30 seconds
        .use_work_stealing = true,
        .numa_aware_scheduling = false,
        .default_priority = TASK_PRIORITY_NORMAL,
        .propagate_errors = true,
        .collect_all_errors = false,
        .default_cancel_strategy = CANCEL_GRACEFUL,
        .actor_system = NULL,
        .shared_var_manager = NULL
    };
}

TaskScopeConfig task_scope_config_cpu_intensive(void) {
    TaskScopeConfig config = task_scope_config_default();
    config.max_concurrent_tasks = 16;  // More threads for CPU work
    config.use_work_stealing = true;
    config.numa_aware_scheduling = true;
    config.default_priority = TASK_PRIORITY_HIGH;
    return config;
}

TaskScopeConfig task_scope_config_io_intensive(void) {
    TaskScopeConfig config = task_scope_config_default();
    config.max_concurrent_tasks = 64;  // Many threads for I/O waiting
    config.default_task_timeout_ms = 120000;  // 2 minutes for I/O
    config.use_work_stealing = false;  // Less useful for I/O
    return config;
}

TaskScopeConfig task_scope_config_memory_constrained(void) {
    TaskScopeConfig config = task_scope_config_default();
    config.max_concurrent_tasks = 4;   // Fewer concurrent tasks
    config.max_memory_per_task = 4 * 1024 * 1024;   // 4MB per task
    config.max_total_memory = 64 * 1024 * 1024;     // 64MB total
    return config;
}

// Worker thread function for task execution
static void* task_worker_thread(void* arg) {
    TaskScope* scope = (TaskScope*)arg;
    
    while (scope->is_active && !scope->shutdown_requested) {
        pthread_mutex_lock(&scope->scope_mutex);
        
        // Wait for tasks if queue is empty
        while (scope->queue_size == 0 && scope->is_active && !scope->shutdown_requested) {
            pthread_cond_wait(&scope->tasks_available, &scope->scope_mutex);
        }
        
        if (scope->shutdown_requested) {
            pthread_mutex_unlock(&scope->scope_mutex);
            break;
        }
        
        // Get task from queue
        ConcurrentTask* task = NULL;
        if (scope->queue_size > 0) {
            task = scope->task_queue[scope->queue_head];
            scope->queue_head = (scope->queue_head + 1) % scope->queue_capacity;
            scope->queue_size--;
        }
        
        pthread_mutex_unlock(&scope->scope_mutex);
        
        if (task) {
            // Execute task
            pthread_mutex_lock(&task->task_mutex);
            
            if (task->state == TASK_STATE_QUEUED && !atomic_load(&task->context.is_cancelled)) {
                task->state = TASK_STATE_RUNNING;
                task->context.start_time_ns = get_monotonic_time_ns();
                
                pthread_mutex_unlock(&task->task_mutex);
                
                // Call task function
                Result_void_ptr result = task->function(&task->context, task->arguments);
                
                pthread_mutex_lock(&task->task_mutex);
                
                task->context.end_time_ns = get_monotonic_time_ns();
                
                if (result.is_error) {
                    task->state = TASK_STATE_ERROR;
                    task->error = result.error;
                    
                    // Add to scope's error collection if enabled
                    if (scope->config.collect_all_errors) {
                        pthread_mutex_lock(&scope->scope_mutex);
                        if (scope->error_count < scope->error_capacity) {
                            scope->collected_errors[scope->error_count++] = result.error;
                        }
                        atomic_store(&scope->has_errors, true);
                        pthread_mutex_unlock(&scope->scope_mutex);
                    }
                } else {
                    task->state = TASK_STATE_COMPLETED;
                    task->result = result.value;
                }
                
                pthread_cond_broadcast(&task->state_changed);
            }
            
            pthread_mutex_unlock(&task->task_mutex);
            
            // Update scope statistics
            pthread_mutex_lock(&scope->scope_mutex);
            if (task->state == TASK_STATE_COMPLETED) {
                scope->total_tasks_completed++;
            } else if (task->state == TASK_STATE_ERROR) {
                scope->total_tasks_failed++;
            }
            pthread_mutex_unlock(&scope->scope_mutex);
        }
    }
    
    return NULL;
}

// Task scope management
TaskScope* task_scope_create(TaskScopeConfig config, const char* name) {
    TaskScope* scope = calloc(1, sizeof(TaskScope));
    if (!scope) return NULL;
    
    scope->id = generate_unique_id();
    if (name) {
        strncpy(scope->name, name, sizeof(scope->name) - 1);
        scope->name[sizeof(scope->name) - 1] = '\0';
    } else {
        snprintf(scope->name, sizeof(scope->name), "scope_%llu", scope->id);
    }
    
    scope->config = config;
    scope->is_active = false;
    scope->shutdown_requested = false;
    atomic_init(&scope->has_errors, false);
    atomic_init(&scope->current_memory_usage, 0);
    
    // Initialize synchronization
    if (pthread_mutex_init(&scope->scope_mutex, NULL) != 0) {
        free(scope);
        return NULL;
    }
    
    if (pthread_cond_init(&scope->tasks_available, NULL) != 0) {
        pthread_mutex_destroy(&scope->scope_mutex);
        free(scope);
        return NULL;
    }
    
    if (pthread_cond_init(&scope->scope_completed, NULL) != 0) {
        pthread_mutex_destroy(&scope->scope_mutex);
        pthread_cond_destroy(&scope->tasks_available);
        free(scope);
        return NULL;
    }
    
    // Allocate task groups
    scope->group_capacity = 100;
    scope->task_groups = calloc(scope->group_capacity, sizeof(TaskGroup*));
    if (!scope->task_groups) {
        pthread_mutex_destroy(&scope->scope_mutex);
        pthread_cond_destroy(&scope->tasks_available);
        pthread_cond_destroy(&scope->scope_completed);
        free(scope);
        return NULL;
    }
    
    // Allocate task queue
    scope->queue_capacity = config.max_total_tasks;
    scope->task_queue = calloc(scope->queue_capacity, sizeof(ConcurrentTask*));
    if (!scope->task_queue) {
        free(scope->task_groups);
        pthread_mutex_destroy(&scope->scope_mutex);
        pthread_cond_destroy(&scope->tasks_available);
        pthread_cond_destroy(&scope->scope_completed);
        free(scope);
        return NULL;
    }
    
    // Allocate worker threads
    scope->worker_count = config.max_concurrent_tasks;
    scope->worker_threads = calloc(scope->worker_count, sizeof(pthread_t));
    if (!scope->worker_threads) {
        free(scope->task_queue);
        free(scope->task_groups);
        pthread_mutex_destroy(&scope->scope_mutex);
        pthread_cond_destroy(&scope->tasks_available);
        pthread_cond_destroy(&scope->scope_completed);
        free(scope);
        return NULL;
    }
    
    // Allocate error collection array
    if (config.collect_all_errors) {
        scope->error_capacity = 1000;
        scope->collected_errors = calloc(scope->error_capacity, sizeof(Error*));
        if (!scope->collected_errors) {
            free(scope->worker_threads);
            free(scope->task_queue);
            free(scope->task_groups);
            pthread_mutex_destroy(&scope->scope_mutex);
            pthread_cond_destroy(&scope->tasks_available);
            pthread_cond_destroy(&scope->scope_completed);
            free(scope);
            return NULL;
        }
    }
    
    // Allocate child scopes array
    scope->child_capacity = 50;
    scope->child_scopes = calloc(scope->child_capacity, sizeof(TaskScope*));
    if (!scope->child_scopes) {
        free(scope->collected_errors);
        free(scope->worker_threads);
        free(scope->task_queue);
        free(scope->task_groups);
        pthread_mutex_destroy(&scope->scope_mutex);
        pthread_cond_destroy(&scope->tasks_available);
        pthread_cond_destroy(&scope->scope_completed);
        free(scope);
        return NULL;
    }
    
    return scope;
}

Result_void_ptr task_scope_start(TaskScope* scope) {
    if (!scope) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid task scope"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&scope->scope_mutex);
    
    if (scope->is_active) {
        pthread_mutex_unlock(&scope->scope_mutex);
        return OK_PTR(NULL);  // Already started
    }
    
    scope->is_active = true;
    scope->shutdown_requested = false;
    
    // Start worker threads
    for (size_t i = 0; i < scope->worker_count; i++) {
        if (pthread_create(&scope->worker_threads[i], NULL, task_worker_thread, scope) != 0) {
            // Clean up already started threads
            scope->shutdown_requested = true;
            pthread_cond_broadcast(&scope->tasks_available);
            
            for (size_t j = 0; j < i; j++) {
                pthread_join(scope->worker_threads[j], NULL);
            }
            
            scope->is_active = false;
            pthread_mutex_unlock(&scope->scope_mutex);
            
            Error* error = malloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_INTERNAL,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Failed to start worker threads"),
                .hint = strdup("Check system thread limits"),
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
        }
    }
    
    pthread_mutex_unlock(&scope->scope_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr task_scope_shutdown(TaskScope* scope, uint64_t timeout_ms) {
    if (!scope) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid task scope"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&scope->scope_mutex);
    
    if (!scope->is_active) {
        pthread_mutex_unlock(&scope->scope_mutex);
        return OK_PTR(NULL);  // Already shut down
    }
    
    scope->shutdown_requested = true;
    
    // Wake up all worker threads
    pthread_cond_broadcast(&scope->tasks_available);
    
    pthread_mutex_unlock(&scope->scope_mutex);
    
    // Wait for worker threads to finish
    for (size_t i = 0; i < scope->worker_count; i++) {
        pthread_join(scope->worker_threads[i], NULL);
    }
    
    pthread_mutex_lock(&scope->scope_mutex);
    scope->is_active = false;
    pthread_cond_broadcast(&scope->scope_completed);
    pthread_mutex_unlock(&scope->scope_mutex);
    
    return OK_PTR(NULL);
}

// Release every resource a scope owns WITHOUT freeing the scope struct itself.
// task_scope_destroy uses this for heap-allocated scopes; owners that embed a
// TaskScope by value (e.g. WorkStealingScope, whose base_scope is the first
// member) call it directly and free the enclosing block themselves — calling
// the full task_scope_destroy on an embedded scope would free(&base_scope),
// i.e. the wrapper, and then the wrapper's own free() double-frees it.
void task_scope_cleanup(TaskScope* scope) {
    if (!scope) return;

    // Ensure scope is shut down
    if (scope->is_active) {
        task_scope_shutdown(scope, 5000);  // 5 second timeout
    }

    // Clean up task groups
    if (scope->task_groups) {
        for (size_t i = 0; i < scope->group_count; i++) {
            if (scope->task_groups[i]) {
                task_group_destroy(scope->task_groups[i]);
            }
        }
        free(scope->task_groups);
    }
    
    // Clean up child scopes
    if (scope->child_scopes) {
        for (size_t i = 0; i < scope->child_count; i++) {
            if (scope->child_scopes[i]) {
                task_scope_destroy(scope->child_scopes[i]);
            }
        }
        free(scope->child_scopes);
    }
    
    // Clean up error collection
    if (scope->collected_errors) {
        for (size_t i = 0; i < scope->error_count; i++) {
            if (scope->collected_errors[i]) {
                free((void*)scope->collected_errors[i]->message);
                free((void*)scope->collected_errors[i]->hint);
                free(scope->collected_errors[i]);
            }
        }
        free(scope->collected_errors);
    }
    
    // Clean up remaining tasks in queue
    if (scope->task_queue) {
        for (size_t i = 0; i < scope->queue_size; i++) {
            size_t index = (scope->queue_head + i) % scope->queue_capacity;
            if (scope->task_queue[index]) {
                task_destroy(scope->task_queue[index]);
            }
        }
        free(scope->task_queue);
    }
    
    free(scope->worker_threads);
    
    // Destroy synchronization primitives
    pthread_mutex_destroy(&scope->scope_mutex);
    pthread_cond_destroy(&scope->tasks_available);
    pthread_cond_destroy(&scope->scope_completed);
}

void task_scope_destroy(TaskScope* scope) {
    if (!scope) return;
    task_scope_cleanup(scope);
    free(scope);
}

// Task management
ConcurrentTask* task_create(TaskScope* scope, TaskFunction function, void* args, size_t args_size, const char* name) {
    if (!scope || !function) return NULL;
    
    ConcurrentTask* task = calloc(1, sizeof(ConcurrentTask));
    if (!task) return NULL;
    
    task->id = generate_unique_id();
    if (name) {
        strncpy(task->name, name, sizeof(task->name) - 1);
        task->name[sizeof(task->name) - 1] = '\0';
    } else {
        snprintf(task->name, sizeof(task->name), "task_%llu", task->id);
    }
    
    task->function = function;
    task->state = TASK_STATE_CREATED;
    task->priority = scope->config.default_priority;
    
    // Copy arguments
    if (args && args_size > 0) {
        task->arguments = malloc(args_size);
        if (!task->arguments) {
            free(task);
            return NULL;
        }
        memcpy(task->arguments, args, args_size);
        task->args_size = args_size;
    }
    
    // Initialize task context
    task->context.task_id = task->id;
    task->context.scope = scope;
    task->context.cancel_strategy = scope->config.default_cancel_strategy;
    atomic_init(&task->context.is_cancelled, false);
    atomic_init(&task->context.cancel_requested, false);
    atomic_init(&task->context.progress_percentage, 0);
    task->context.max_memory_bytes = scope->config.max_memory_per_task;
    task->context.max_execution_time_ms = scope->config.default_task_timeout_ms;
    
    // Initialize synchronization
    if (pthread_mutex_init(&task->task_mutex, NULL) != 0) {
        free(task->arguments);
        free(task);
        return NULL;
    }
    
    if (pthread_cond_init(&task->state_changed, NULL) != 0) {
        pthread_mutex_destroy(&task->task_mutex);
        free(task->arguments);
        free(task);
        return NULL;
    }
    
    atomic_init(&task->ref_count, 1);
    
    return task;
}

Result_void_ptr task_submit(ConcurrentTask* task) {
    if (!task || !task->context.scope) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid task or scope"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    TaskScope* scope = task->context.scope;
    
    pthread_mutex_lock(&scope->scope_mutex);
    
    // Check if scope can accept more tasks
    if (scope->queue_size >= scope->queue_capacity) {
        pthread_mutex_unlock(&scope->scope_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INTERNAL,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Task queue is full"),
            .hint = strdup("Wait for tasks to complete or increase queue capacity"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Add task to queue
    size_t queue_tail = (scope->queue_head + scope->queue_size) % scope->queue_capacity;
    scope->task_queue[queue_tail] = task;
    scope->queue_size++;
    scope->total_tasks_created++;
    
    // Update task state
    pthread_mutex_lock(&task->task_mutex);
    task->state = TASK_STATE_QUEUED;
    pthread_cond_signal(&task->state_changed);
    pthread_mutex_unlock(&task->task_mutex);
    
    // Wake up a worker thread
    pthread_cond_signal(&scope->tasks_available);
    
    pthread_mutex_unlock(&scope->scope_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr task_wait(ConcurrentTask* task, uint64_t timeout_ms) {
    if (!task) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid task"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&task->task_mutex);
    
    // If task is already completed, return immediately
    if (task->state == TASK_STATE_COMPLETED || 
        task->state == TASK_STATE_ERROR || 
        task->state == TASK_STATE_CANCELLED) {
        
        TaskState final_state = task->state;
        Error* task_error = task->error;
        
        pthread_mutex_unlock(&task->task_mutex);
        
        if (final_state == TASK_STATE_ERROR && task_error) {
            return ERR_PTR(task_error);
        } else {
            // For completed or cancelled tasks, return success
            // The caller can check task->state to see if it was cancelled
            return OK_PTR(NULL);
        }
    }
    
    // Wait for completion with timeout
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    
    int wait_result = 0;
    while (task->state != TASK_STATE_COMPLETED && 
           task->state != TASK_STATE_ERROR && 
           task->state != TASK_STATE_CANCELLED &&
           wait_result != ETIMEDOUT) {
        
        if (timeout_ms == UINT64_MAX) {
            wait_result = pthread_cond_wait(&task->state_changed, &task->task_mutex);
        } else {
            wait_result = pthread_cond_timedwait(&task->state_changed, &task->task_mutex, &deadline);
        }
    }
    
    TaskState final_state = task->state;
    Error* task_error = task->error;
    
    pthread_mutex_unlock(&task->task_mutex);
    
    if (wait_result == ETIMEDOUT) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INTERNAL,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Task wait timed out"),
            .hint = strdup("Increase timeout or check task responsiveness"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    if (final_state == TASK_STATE_ERROR && task_error) {
        return ERR_PTR(task_error);
    } else {
        // For completed or cancelled tasks, return success
        // The caller can check task->state to see if it was cancelled
        return OK_PTR(NULL);
    }
}

Result_void_ptr task_cancel(ConcurrentTask* task, CancellationStrategy strategy) {
    if (!task) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid task"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&task->task_mutex);
    
    if (task->state == TASK_STATE_COMPLETED || 
        task->state == TASK_STATE_ERROR || 
        task->state == TASK_STATE_CANCELLED) {
        pthread_mutex_unlock(&task->task_mutex);
        return OK_PTR(NULL);  // Already finished
    }
    
    atomic_store(&task->context.cancel_requested, true);
    task->context.cancel_strategy = strategy;
    
    if (strategy == CANCEL_IMMEDIATE) {
        atomic_store(&task->context.is_cancelled, true);
        task->state = TASK_STATE_CANCELLED;
        pthread_cond_broadcast(&task->state_changed);
    }
    
    pthread_mutex_unlock(&task->task_mutex);
    
    return OK_PTR(NULL);
}

void task_destroy(ConcurrentTask* task) {
    if (!task) return;
    
    // Clean up dependencies
    TaskDependency* dep = task->dependencies;
    while (dep) {
        TaskDependency* next = dep->next;
        free(dep);
        dep = next;
    }
    
    // Clean up arguments and result (avoid double free if they point to same memory)
    free(task->arguments);
    if (task->result != task->arguments) {
        free(task->result);
    }
    
    // Destroy synchronization primitives
    pthread_mutex_destroy(&task->task_mutex);
    pthread_cond_destroy(&task->state_changed);
    
    free(task);
}

// Task groups
TaskGroup* task_group_create(TaskScope* scope, const char* name) {
    if (!scope) return NULL;
    
    TaskGroup* group = calloc(1, sizeof(TaskGroup));
    if (!group) return NULL;
    
    group->id = generate_unique_id();
    if (name) {
        strncpy(group->name, name, sizeof(group->name) - 1);
        group->name[sizeof(group->name) - 1] = '\0';
    } else {
        snprintf(group->name, sizeof(group->name), "group_%llu", group->id);
    }
    
    group->fail_fast = false;
    group->wait_for_all = true;
    group->timeout_ms = 60000;  // 1 minute default
    group->max_concurrent_tasks = scope->config.max_concurrent_tasks;
    
    if (pthread_mutex_init(&group->group_mutex, NULL) != 0) {
        free(group);
        return NULL;
    }
    
    if (pthread_cond_init(&group->all_completed, NULL) != 0) {
        pthread_mutex_destroy(&group->group_mutex);
        free(group);
        return NULL;
    }
    
    // Add to scope
    pthread_mutex_lock(&scope->scope_mutex);
    if (scope->group_count < scope->group_capacity) {
        scope->task_groups[scope->group_count++] = group;
    }
    pthread_mutex_unlock(&scope->scope_mutex);
    
    return group;
}

Result_void_ptr task_group_add_task(TaskGroup* group, ConcurrentTask* task) {
    if (!group || !task) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid task group or task"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&group->group_mutex);
    
    // Add task to linked list
    if (!group->first_task) {
        group->first_task = task;
        group->last_task = task;
        task->next = NULL;
        task->prev = NULL;
    } else {
        group->last_task->next = task;
        task->prev = group->last_task;
        task->next = NULL;
        group->last_task = task;
    }
    
    group->task_count++;
    
    pthread_mutex_unlock(&group->group_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr task_group_wait_all(TaskGroup* group, uint64_t timeout_ms) {
    if (!group) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid task group"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&group->group_mutex);
    
    printf("task_group_wait_all: Group has %zu tasks\n", group->task_count);
    
    // Wait for all tasks to complete
    ConcurrentTask* task = group->first_task;
    size_t task_index = 0;
    while (task) {
        printf("  Waiting for task %zu: %s (state=%d)\n", task_index, task->name, task->state);
        pthread_mutex_unlock(&group->group_mutex);
        
        Result_void_ptr wait_result = task_wait(task, timeout_ms);
        if (wait_result.is_error && group->fail_fast) {
            printf("  Task %zu failed with error\n", task_index);
            return wait_result;
        }
        
        pthread_mutex_lock(&group->group_mutex);
        
        // Update statistics
        if (wait_result.is_error) {
            group->failed_tasks++;
            printf("  Task %zu completed with error\n", task_index);
        } else {
            group->completed_tasks++;
            printf("  Task %zu completed successfully\n", task_index);
        }
        
        task = task->next;
        task_index++;
    }
    
    printf("task_group_wait_all: All %zu tasks completed\n", task_index);
    pthread_cond_broadcast(&group->all_completed);
    pthread_mutex_unlock(&group->group_mutex);
    
    return OK_PTR(NULL);
}

void task_group_destroy(TaskGroup* group) {
    if (!group) return;
    
    // Clean up tasks
    ConcurrentTask* task = group->first_task;
    while (task) {
        ConcurrentTask* next = task->next;
        task_destroy(task);
        task = next;
    }
    
    // Destroy synchronization primitives
    pthread_mutex_destroy(&group->group_mutex);
    pthread_cond_destroy(&group->all_completed);
    
    free(group);
}

// Parallel for implementation
Result_void_ptr task_scope_parallel_for(TaskScope* scope, ParallelForConfig config, 
                                       ParallelForFunction function, void* context) {
    // Comprehensive input validation
    if (!scope || !function) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid scope or function"),
            .hint = strdup("Ensure both TaskScope and ParallelForFunction are non-null"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Validate configuration parameters
    if (config.start_index >= config.end_index) {
        return OK_PTR(NULL);  // Nothing to do - valid case
    }
    
    if (config.max_workers == 0) {
        config.max_workers = 1; // Fallback to single worker
    }
    
    // Prevent integer overflow in calculations
    size_t total_items = config.end_index - config.start_index;
    if (total_items > SIZE_MAX / 2) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Range too large for parallel processing"),
            .hint = strdup("Consider processing smaller chunks sequentially"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Adaptive chunk sizing with safety bounds
    size_t chunk_size = config.chunk_size;
    if (chunk_size == 0) {
        chunk_size = (total_items + config.max_workers - 1) / config.max_workers;
        // Ensure minimum chunk size for efficiency
        if (chunk_size < 1) chunk_size = 1;
        // Ensure maximum chunk size to prevent memory issues
        if (chunk_size > 10000) chunk_size = 10000;
    }
    
    // Calculate actual number of tasks needed
    size_t task_count = 0;
    size_t max_tasks = (total_items + chunk_size - 1) / chunk_size;
    
    // Limit concurrent tasks for safety
    if (max_tasks > 1000) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_WARNING,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Too many concurrent tasks - consider larger chunk size"),
            .hint = strdup("Increase chunk_size or reduce range"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Allocate with bounds checking
    ParallelForTaskArgs* all_args = malloc(max_tasks * sizeof(ParallelForTaskArgs));
    if (!all_args) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to allocate task arguments"),
            .hint = strdup("Reduce max_workers or chunk_size"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Track task pointers for proper cleanup
    ConcurrentTask** tasks = malloc(max_tasks * sizeof(ConcurrentTask*));
    if (!tasks) {
        free(all_args);
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to allocate task array"),
            .hint = strdup("Reduce max_workers or chunk_size"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Initialize all task pointers to NULL for safe cleanup
    memset(tasks, 0, max_tasks * sizeof(ConcurrentTask*));
    
    // Create and submit tasks with proper error handling
    for (size_t start = config.start_index; start < config.end_index; start += chunk_size) {
        size_t end = start + chunk_size;
        if (end > config.end_index) end = config.end_index;
        
        // Initialize task arguments with bounds checking
        all_args[task_count] = (ParallelForTaskArgs){
            .start = start,
            .end = end,
            .func = function,
            .user_context = context
        };
        
        // Create task with descriptive name for debugging
        char task_name[64];
        snprintf(task_name, sizeof(task_name), "parallel_for_chunk_%zu_%zu", start, end);
        
        ConcurrentTask* task = task_create(scope, parallel_for_task_wrapper, 
                                         &all_args[task_count], sizeof(ParallelForTaskArgs), task_name);
        if (task) {
            task->priority = config.priority;
            tasks[task_count] = task;
            
            // Submit task and check for errors
            Result_void_ptr submit_result = task_submit(task);
            if (submit_result.is_error) {
                // Clean up on submission failure
                for (size_t cleanup_i = 0; cleanup_i < task_count; cleanup_i++) {
                    if (tasks[cleanup_i]) {
                        task_cancel(tasks[cleanup_i], CANCEL_GRACEFUL);
                    }
                }
                free(all_args);
                free(tasks);
                return submit_result;
            }
            task_count++;
        } else {
            // Task creation failed - clean up and return error
            for (size_t cleanup_i = 0; cleanup_i < task_count; cleanup_i++) {
                if (tasks[cleanup_i]) {
                    task_cancel(tasks[cleanup_i], CANCEL_GRACEFUL);
                }
            }
            free(all_args);
            free(tasks);
            
            Error* error = malloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_OUT_OF_MEMORY,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Failed to create parallel task"),
                .hint = strdup("System may be under memory pressure"),
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
        }
    }
    
    // Wait for all tasks with proper error aggregation
    Result_void_ptr first_error = OK_PTR(NULL);
    size_t completed_tasks = 0;
    
    for (size_t i = 0; i < task_count; i++) {
        if (tasks[i]) {
            Result_void_ptr wait_result = task_wait(tasks[i], UINT64_MAX);  // No timeout
            completed_tasks++;
            
            if (wait_result.is_error && first_error.is_error == false) {
                first_error = wait_result;
                // Continue waiting for other tasks to complete for proper cleanup
            }
        }
    }
    
    // Ensure all tasks have completed before cleanup
    if (completed_tasks < task_count) {
        usleep(100000); // 100ms grace period for remaining tasks
    }
    
    // Clean up resources safely
    free(all_args);
    free(tasks);
    
    return first_error;
}

// Statistics and monitoring
TaskScopeStats task_scope_get_stats(TaskScope* scope) {
    TaskScopeStats stats = {0};
    
    if (!scope) return stats;
    
    pthread_mutex_lock(&scope->scope_mutex);
    
    stats.total_tasks = scope->total_tasks_created;
    stats.completed_tasks = scope->total_tasks_completed;
    stats.failed_tasks = scope->total_tasks_failed;
    stats.cancelled_tasks = 0;  // TODO: track cancelled tasks
    
    stats.current_memory_usage = atomic_load(&scope->current_memory_usage);
    stats.peak_memory_usage = scope->peak_memory_usage;
    stats.active_workers = scope->worker_count;
    
    stats.current_queue_size = scope->queue_size;
    stats.peak_queue_size = scope->queue_capacity;  // TODO: track peak queue size
    
    if (scope->queue_capacity > 0) {
        stats.queue_utilization = (double)scope->queue_size / scope->queue_capacity;
    }
    
    stats.error_count = scope->error_count;
    
    if (stats.total_tasks > 0) {
        stats.task_success_rate = (double)stats.completed_tasks / stats.total_tasks;
    }
    
    pthread_mutex_unlock(&scope->scope_mutex);
    
    return stats;
}

void task_scope_reset_stats(TaskScope* scope) {
    if (!scope) return;
    
    pthread_mutex_lock(&scope->scope_mutex);
    
    scope->total_tasks_created = 0;
    scope->total_tasks_completed = 0;
    scope->total_tasks_failed = 0;
    scope->total_execution_time_ns = 0;
    scope->peak_memory_usage = 0;
    scope->error_count = 0;
    
    pthread_mutex_unlock(&scope->scope_mutex);
}

// Cancellation token operations
CancellationToken* cancellation_token_create(void) {
    CancellationToken* token = calloc(1, sizeof(CancellationToken));
    if (!token) return NULL;
    
    atomic_init(&token->is_cancelled, false);
    atomic_init(&token->cancel_requested, false);
    
    token->callback_capacity = 10;
    token->on_cancel_callbacks = calloc(token->callback_capacity, sizeof(void*));
    token->callback_contexts = calloc(token->callback_capacity, sizeof(void*));
    
    if (!token->on_cancel_callbacks || !token->callback_contexts) {
        free(token->on_cancel_callbacks);
        free(token->callback_contexts);
        free(token);
        return NULL;
    }
    
    if (pthread_mutex_init(&token->token_mutex, NULL) != 0) {
        free(token->on_cancel_callbacks);
        free(token->callback_contexts);
        free(token);
        return NULL;
    }
    
    return token;
}

void cancellation_token_cancel(CancellationToken* token) {
    if (!token) return;
    
    pthread_mutex_lock(&token->token_mutex);
    
    atomic_store(&token->cancel_requested, true);
    atomic_store(&token->is_cancelled, true);
    
    // Call all registered callbacks
    for (size_t i = 0; i < token->callback_count; i++) {
        if (token->on_cancel_callbacks[i]) {
            token->on_cancel_callbacks[i](token->callback_contexts[i]);
        }
    }
    
    pthread_mutex_unlock(&token->token_mutex);
}

bool cancellation_token_is_cancelled(CancellationToken* token) {
    if (!token) return false;
    return atomic_load(&token->is_cancelled);
}

void cancellation_token_destroy(CancellationToken* token) {
    if (!token) return;
    
    free(token->on_cancel_callbacks);
    free(token->callback_contexts);
    pthread_mutex_destroy(&token->token_mutex);
    free(token);
}

// Task dependency implementation
Result_void_ptr task_add_dependency(ConcurrentTask* task, ConcurrentTask* dependency, bool required, bool wait_completion) {
    if (!task || !dependency) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid task or dependency"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Create new dependency
    TaskDependency* dep = malloc(sizeof(TaskDependency));
    if (!dep) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to allocate dependency"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    dep->dependency_task_id = dependency->id;
    dep->is_required = required;
    dep->wait_for_completion = wait_completion;
    dep->next = task->dependencies;
    
    task->dependencies = dep;
    task->dependency_count++;
    
    return OK_PTR(task);
}

// Map-reduce implementation
Result_void_ptr task_scope_map_reduce(TaskScope* scope, MapReduceConfig config,
                                     MapFunction map_fn, ReduceFunction reduce_fn,
                                     void** result, size_t* result_size) {
    if (!scope || !map_fn || !reduce_fn || !result || !result_size) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid parameters for map-reduce"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    if (config.item_count == 0) {
        *result = NULL;
        *result_size = 0;
        return OK_PTR(NULL);
    }
    
    // Map phase
    TaskGroup* map_group = task_group_create(scope, "map_phase");
    if (!map_group) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to create map group"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Store map results
    void** map_results = calloc(config.item_count, sizeof(void*));
    size_t* map_result_sizes = calloc(config.item_count, sizeof(size_t));
    if (!map_results || !map_result_sizes) {
        free(map_results);
        free(map_result_sizes);
        task_group_destroy(map_group);
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to allocate map results"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Map task structure
    typedef struct {
        size_t index;
        void* input_item;
        MapFunction map_function;
        void** map_results;
        size_t* map_result_sizes;
    } MapTaskArgs;
    
    // Map task function
    auto map_task_function = ^Result_void_ptr(TaskContext* context, void* args) {
        MapTaskArgs* map_args = (MapTaskArgs*)args;
        
        Result_void_ptr map_result = map_args->map_function(
            map_args->input_item,
            &map_args->map_results[map_args->index],
            &map_args->map_result_sizes[map_args->index]
        );
        
        return map_result;
    };
    
    // Create map tasks
    MapTaskArgs* map_task_args = calloc(config.item_count, sizeof(MapTaskArgs));
    if (!map_task_args) {
        free(map_results);
        free(map_result_sizes);
        task_group_destroy(map_group);
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to allocate map task args"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    for (size_t i = 0; i < config.item_count; i++) {
        map_task_args[i] = (MapTaskArgs){
            .index = i,
            .input_item = config.input_items[i],
            .map_function = map_fn,
            .map_results = map_results,
            .map_result_sizes = map_result_sizes
        };
        
        ConcurrentTask* map_task = task_create(scope, (TaskFunction)map_task_function, 
                                             &map_task_args[i], sizeof(MapTaskArgs), "map_task");
        if (map_task) {
            task_group_add_task(map_group, map_task);
            task_submit(map_task);
        }
    }
    
    // Wait for all map tasks to complete
    Result_void_ptr map_wait_result = task_group_wait_all(map_group, UINT64_MAX);
    if (map_wait_result.is_error) {
        free(map_results);
        free(map_result_sizes);
        free(map_task_args);
        task_group_destroy(map_group);
        return map_wait_result;
    }
    
    // Reduce phase
    Result_void_ptr reduce_result = reduce_fn(map_results, config.item_count, result, result_size);
    
    // Cleanup
    for (size_t i = 0; i < config.item_count; i++) {
        free(map_results[i]);
    }
    free(map_results);
    free(map_result_sizes);
    free(map_task_args);
    task_group_destroy(map_group);
    
    return reduce_result;
}

// Pipeline implementation
Pipeline* pipeline_create(TaskScope* scope, const char* name, TaskFunction* stages, size_t stage_count) {
    if (!scope || !name || !stages || stage_count == 0) {
        return NULL;
    }
    
    Pipeline* pipeline = malloc(sizeof(Pipeline));
    if (!pipeline) return NULL;
    
    pipeline->id = generate_unique_id();
    strncpy(pipeline->name, name, sizeof(pipeline->name) - 1);
    pipeline->name[sizeof(pipeline->name) - 1] = '\0';
    
    pipeline->stage_count = stage_count;
    pipeline->stages = malloc(stage_count * sizeof(TaskFunction));
    pipeline->buffer_sizes = malloc(stage_count * sizeof(size_t));
    pipeline->worker_counts = malloc(stage_count * sizeof(size_t));
    pipeline->input_buffers = malloc(stage_count * sizeof(void*));
    pipeline->output_buffers = malloc(stage_count * sizeof(void*));
    pipeline->buffer_positions = malloc(stage_count * sizeof(atomic_size_t));
    
    if (!pipeline->stages || !pipeline->buffer_sizes || !pipeline->worker_counts ||
        !pipeline->input_buffers || !pipeline->output_buffers || !pipeline->buffer_positions) {
        free(pipeline->stages);
        free(pipeline->buffer_sizes);
        free(pipeline->worker_counts);
        free(pipeline->input_buffers);
        free(pipeline->output_buffers);
        free(pipeline->buffer_positions);
        free(pipeline);
        return NULL;
    }
    
    // Copy stages and initialize default configurations
    for (size_t i = 0; i < stage_count; i++) {
        pipeline->stages[i] = stages[i];
        pipeline->buffer_sizes[i] = 1000; // Default buffer size
        pipeline->worker_counts[i] = 1;   // Default worker count
        atomic_init(&pipeline->buffer_positions[i], 0);
    }
    
    pipeline->is_running = false;
    atomic_init(&pipeline->shutdown_requested, false);
    pipeline->scope = scope;
    
    if (pthread_mutex_init(&pipeline->pipeline_mutex, NULL) != 0) {
        free(pipeline->stages);
        free(pipeline->buffer_sizes);
        free(pipeline->worker_counts);
        free(pipeline->input_buffers);
        free(pipeline->output_buffers);
        free(pipeline->buffer_positions);
        free(pipeline);
        return NULL;
    }
    
    return pipeline;
}

Result_void_ptr pipeline_start(Pipeline* pipeline) {
    if (!pipeline) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid pipeline"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&pipeline->pipeline_mutex);
    
    if (pipeline->is_running) {
        pthread_mutex_unlock(&pipeline->pipeline_mutex);
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OPERATION_FAILED,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Pipeline is already running"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Allocate buffers for each stage
    for (size_t i = 0; i < pipeline->stage_count; i++) {
        pipeline->input_buffers[i] = malloc(pipeline->buffer_sizes[i] * sizeof(void*));
        pipeline->output_buffers[i] = malloc(pipeline->buffer_sizes[i] * sizeof(void*));
        
        if (!pipeline->input_buffers[i] || !pipeline->output_buffers[i]) {
            // Cleanup on failure
            for (size_t j = 0; j <= i; j++) {
                free(pipeline->input_buffers[j]);
                free(pipeline->output_buffers[j]);
            }
            pthread_mutex_unlock(&pipeline->pipeline_mutex);
            Error* error = malloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_OUT_OF_MEMORY,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Failed to allocate pipeline buffers"),
                .hint = NULL,
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
        }
    }
    
    pipeline->is_running = true;
    atomic_store(&pipeline->shutdown_requested, false);
    
    pthread_mutex_unlock(&pipeline->pipeline_mutex);
    
    return OK_PTR(pipeline);
}

Result_void_ptr pipeline_push_input(Pipeline* pipeline, void* item, size_t item_size) {
    if (!pipeline || !item) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid pipeline or item"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    if (!pipeline->is_running) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OPERATION_FAILED,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Pipeline is not running"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Add item to first stage input buffer
    // This is a simplified implementation - real implementation would need proper queuing
    void** input_buffer = (void**)pipeline->input_buffers[0];
    size_t pos = atomic_fetch_add(&pipeline->buffer_positions[0], 1);
    
    if (pos >= pipeline->buffer_sizes[0]) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_BUFFER_OVERFLOW,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Pipeline buffer overflow"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Copy the item
    input_buffer[pos] = malloc(item_size);
    if (!input_buffer[pos]) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to allocate item copy"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    memcpy(input_buffer[pos], item, item_size);
    
    return OK_PTR(pipeline);
}

Result_void_ptr pipeline_pop_output(Pipeline* pipeline, void** item, size_t* item_size, uint64_t timeout_ms) {
    if (!pipeline || !item || !item_size) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid parameters"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // This is a simplified implementation
    // Real implementation would wait for output from the last stage
    *item = NULL;
    *item_size = 0;
    
    return OK_PTR(pipeline);
}

Result_void_ptr pipeline_shutdown(Pipeline* pipeline) {
    if (!pipeline) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid pipeline"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&pipeline->pipeline_mutex);
    
    atomic_store(&pipeline->shutdown_requested, true);
    pipeline->is_running = false;
    
    // Free buffers
    for (size_t i = 0; i < pipeline->stage_count; i++) {
        if (pipeline->input_buffers[i]) {
            void** buffer = (void**)pipeline->input_buffers[i];
            for (size_t j = 0; j < atomic_load(&pipeline->buffer_positions[i]); j++) {
                free(buffer[j]);
            }
            free(pipeline->input_buffers[i]);
            pipeline->input_buffers[i] = NULL;
        }
        
        if (pipeline->output_buffers[i]) {
            free(pipeline->output_buffers[i]);
            pipeline->output_buffers[i] = NULL;
        }
    }
    
    pthread_mutex_unlock(&pipeline->pipeline_mutex);
    
    return OK_PTR(pipeline);
}

void pipeline_destroy(Pipeline* pipeline) {
    if (!pipeline) return;
    
    pipeline_shutdown(pipeline);
    
    free(pipeline->stages);
    free(pipeline->buffer_sizes);
    free(pipeline->worker_counts);
    free(pipeline->input_buffers);
    free(pipeline->output_buffers);
    free(pipeline->buffer_positions);
    pthread_mutex_destroy(&pipeline->pipeline_mutex);
    free(pipeline);
}