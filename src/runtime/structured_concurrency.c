#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "structured_concurrency.h"
#include "error_hierarchies.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sched.h>

// =============================================================================
// Global State and Utilities
// =============================================================================

// Global scheduler and scope
static ConcurrencyScheduler* g_global_scheduler = NULL;
static pthread_mutex_t g_scheduler_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread-local storage for current scope and parallel block
static __thread ConcurrencyScope* g_current_scope = NULL;
static __thread ParallelBlock* g_current_parallel_block = NULL;

// Global counters
static atomic_uint_least64_t g_next_scope_id = 1;
static atomic_uint_least64_t g_next_task_id = 1; 
static atomic_uint_least64_t g_next_group_id = 1;
static atomic_uint_least64_t g_next_token_id = 1;

// Utility functions
static uint64_t get_current_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000UL + (uint64_t)ts.tv_nsec;
}


static char* safe_strdup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* copy = malloc(len + 1);
    if (copy) {
        memcpy(copy, str, len + 1);
    }
    return copy;
}

// =============================================================================
// Cancellation Token Implementation
// =============================================================================

CancellationToken* cancellation_token_create(CancellationToken* parent) {
    CancellationToken* token = calloc(1, sizeof(CancellationToken));
    if (!token) return NULL;
    
    token->token_id = atomic_fetch_add(&g_next_token_id, 1);
    atomic_init(&token->is_cancelled, false);
    token->reason = CANCEL_REASON_USER;
    token->parent = parent;
    
    // Initialize arrays
    token->child_capacity = 4;
    token->children = calloc(token->child_capacity, sizeof(CancellationToken*));
    if (!token->children) {
        free(token);
        return NULL;
    }
    
    token->callback_count = 0;
    token->callbacks = NULL;
    token->callback_contexts = NULL;
    
    // Initialize synchronization
    if (pthread_mutex_init(&token->mutex, NULL) != 0 ||
        pthread_cond_init(&token->cancelled_cond, NULL) != 0) {
        free(token->children);
        free(token);
        return NULL;
    }
    
    // Create arena
    token->token_arena = goo_arena_new(1024, "cancellation_token");
    
    // Add to parent's children if parent exists
    if (parent) {
        pthread_mutex_lock(&parent->mutex);
        if (parent->child_count >= parent->child_capacity) {
            int new_capacity = parent->child_capacity * 2;
            CancellationToken** new_children = realloc(parent->children, 
                                                      new_capacity * sizeof(CancellationToken*));
            if (new_children) {
                parent->children = new_children;
                parent->child_capacity = new_capacity;
            }
        }
        if (parent->child_count < parent->child_capacity) {
            parent->children[parent->child_count++] = token;
        }
        pthread_mutex_unlock(&parent->mutex);
    }
    
    return token;
}

void cancellation_token_destroy(CancellationToken* token) {
    if (!token) return;
    
    // Cancel if not already cancelled
    if (!atomic_load(&token->is_cancelled)) {
        cancellation_token_cancel(token, CANCEL_REASON_SHUTDOWN);
    }
    
    // Clean up children
    pthread_mutex_lock(&token->mutex);
    for (int i = 0; i < token->child_count; i++) {
        if (token->children[i]) {
            cancellation_token_destroy(token->children[i]);
        }
    }
    free(token->children);
    
    // Clean up callbacks
    free(token->callbacks);
    free(token->callback_contexts);
    
    pthread_mutex_unlock(&token->mutex);
    
    // Clean up synchronization
    pthread_cond_destroy(&token->cancelled_cond);
    pthread_mutex_destroy(&token->mutex);
    
    // Clean up arena
    goo_arena_free(token->token_arena);
    
    free(token);
}

bool cancellation_token_is_cancelled(CancellationToken* token) {
    if (!token) return false;
    return atomic_load(&token->is_cancelled);
}

void cancellation_token_cancel(CancellationToken* token, CancellationReason reason) {
    if (!token) return;
    
    pthread_mutex_lock(&token->mutex);
    
    // Already cancelled
    if (atomic_load(&token->is_cancelled)) {
        pthread_mutex_unlock(&token->mutex);
        return;
    }
    
    // Set cancellation
    token->reason = reason;
    token->cancelled_at = get_current_time_ns();
    atomic_store(&token->is_cancelled, true);
    
    // Notify waiters
    pthread_cond_broadcast(&token->cancelled_cond);
    
    // Call callbacks
    for (int i = 0; i < token->callback_count; i++) {
        if (token->callbacks[i]) {
            token->callbacks[i](NULL, reason, token->callback_contexts[i]);
        }
    }
    
    pthread_mutex_unlock(&token->mutex);
    
    // Cancel all children
    for (int i = 0; i < token->child_count; i++) {
        if (token->children[i]) {
            cancellation_token_cancel(token->children[i], CANCEL_REASON_PARENT);
        }
    }
}

bool cancellation_token_wait_for_cancellation(CancellationToken* token, uint64_t timeout_ms) {
    if (!token) return false;
    
    pthread_mutex_lock(&token->mutex);
    
    if (atomic_load(&token->is_cancelled)) {
        pthread_mutex_unlock(&token->mutex);
        return true;
    }
    
    bool result = false;
    if (timeout_ms == 0) {
        // Wait indefinitely
        while (!atomic_load(&token->is_cancelled)) {
            pthread_cond_wait(&token->cancelled_cond, &token->mutex);
        }
        result = true;
    } else {
        // Wait with timeout
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += timeout_ms / 1000;
        timeout.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec++;
            timeout.tv_nsec -= 1000000000;
        }
        
        while (!atomic_load(&token->is_cancelled)) {
            int wait_result = pthread_cond_timedwait(&token->cancelled_cond, &token->mutex, &timeout);
            if (wait_result == ETIMEDOUT) {
                break;
            }
        }
        result = atomic_load(&token->is_cancelled);
    }
    
    pthread_mutex_unlock(&token->mutex);
    return result;
}

// =============================================================================
// Concurrent Task Implementation
// =============================================================================

ConcurrentTask* concurrent_task_create(const char* name, TaskFunction function, void* context) {
    TaskConfig config = {
        .name = name,
        .type = TASK_TYPE_FUNCTION,
        .priority = 50,
        .timeout_ms = 30000,
        .deadline_ms = 0,
        .memory_limit = 0,
        .cpu_time_limit_ms = 0,
        .max_retries = 0,
        .schedule_policy = SCHEDULE_POLICY_FAIR,
        .cpu_affinity = -1,
        .numa_aware = false,
        .on_complete = NULL,
        .on_error = NULL,
        .on_progress = NULL,
        .callback_context = NULL
    };
    
    return concurrent_task_create_with_config(&config, function, context);
}

ConcurrentTask* concurrent_task_create_with_config(const TaskConfig* config, TaskFunction function, void* context) {
    if (!config || !function) return NULL;
    
    ConcurrentTask* task = calloc(1, sizeof(ConcurrentTask));
    if (!task) return NULL;
    
    task->task_id = atomic_fetch_add(&g_next_task_id, 1);
    task->name = safe_strdup(config->name);
    task->type = config->type;
    task->state = TASK_STATE_CREATED;
    
    task->function = function;
    task->context = context;
    task->result = NULL;
    task->result_size = 0;
    
    task->error = NULL;
    task->owns_error = false;
    
    // Copy configuration
    task->config = *config;
    if (config->name) {
        task->config.name = safe_strdup(config->name);
    }
    
    // Initialize statistics
    task->stats.created_at = get_current_time_ns();
    task->stats.progress = 0.0;
    
    // Create cancellation token
    task->cancellation_token = cancellation_token_create(NULL);
    task->is_cancellation_requested = false;
    
    // Initialize arrays
    task->dependencies = NULL;
    task->dependency_count = 0;
    task->dependents = NULL;
    task->dependent_count = 0;
    
    // Set relationships
    task->parent_scope = g_current_scope;
    task->task_group = NULL;
    
    // Initialize synchronization
    if (pthread_mutex_init(&task->task_mutex, NULL) != 0 ||
        pthread_cond_init(&task->state_changed, NULL) != 0) {
        concurrent_task_destroy(task);
        return NULL;
    }
    
    // Create future (stub for now)
    task->future = NULL; // Would be actor_future_create() in full implementation
    
    // Initialize scheduling info
    task->current_cpu = -1;
    task->quantum_start = 0;
    task->last_scheduled = 0;
    
    // Create task arena
    task->task_arena = goo_arena_new(4096, task->name ? task->name : "concurrent_task");
    
    return task;
}

void concurrent_task_destroy(ConcurrentTask* task) {
    if (!task) return;
    
    // Cancel if not already done
    if (task->state != TASK_STATE_COMPLETED && 
        task->state != TASK_STATE_FAILED && 
        task->state != TASK_STATE_CANCELLED) {
        concurrent_task_cancel(task, CANCEL_REASON_SHUTDOWN);
    }
    
    // Clean up result
    if (task->result && task->owns_error) {
        free(task->result);
    }
    
    // Clean up error
    if (task->error && task->owns_error) {
        structured_error_free(task->error);
    }
    
    // Clean up dependencies
    free(task->dependencies);
    free(task->dependents);
    
    // Clean up cancellation token
    cancellation_token_destroy(task->cancellation_token);
    
    // Clean up future
    if (task->future) {
        // Would be actor_future_release(task->future) in full implementation
    }
    
    // Clean up synchronization
    pthread_cond_destroy(&task->state_changed);
    pthread_mutex_destroy(&task->task_mutex);
    
    // Clean up arena
    goo_arena_free(task->task_arena);
    
    // Clean up strings
    free((void*)task->name);
    free((void*)task->config.name);
    
    free(task);
}

static void concurrent_task_set_state(ConcurrentTask* task, TaskState new_state) {
    if (!task) return;
    
    pthread_mutex_lock(&task->task_mutex);
    task->state = new_state;
    
    // Update timestamps
    uint64_t now = get_current_time_ns();
    switch (new_state) {
        case TASK_STATE_RUNNING:
            task->stats.started_at = now;
            break;
        case TASK_STATE_COMPLETED:
        case TASK_STATE_FAILED:
        case TASK_STATE_CANCELLED:
            task->stats.completed_at = now;
            task->stats.wall_time_ns = now - task->stats.created_at;
            break;
        default:
            break;
    }
    
    pthread_cond_broadcast(&task->state_changed);
    pthread_mutex_unlock(&task->task_mutex);
    
    // Call callbacks
    if (new_state == TASK_STATE_COMPLETED && task->config.on_complete) {
        task->config.on_complete(task, task->result, task->config.callback_context);
    } else if (new_state == TASK_STATE_FAILED && task->config.on_error) {
        task->config.on_error(task, task->error, task->config.callback_context);
    }
}

// Task execution wrapper
static void* task_execution_wrapper(void* arg) {
    ConcurrentTask* task = (ConcurrentTask*)arg;
    if (!task) return NULL;
    
    // Set running state
    concurrent_task_set_state(task, TASK_STATE_RUNNING);
    
    void* result = NULL;
    
    // Execute the task function
    if (task->function) {
        result = task->function(task->context, task->cancellation_token);
    }
    
    // Check if cancelled during execution
    if (cancellation_token_is_cancelled(task->cancellation_token)) {
        concurrent_task_set_state(task, TASK_STATE_CANCELLED);
        if (result) {
            free(result); // Clean up result if task was cancelled
            result = NULL;
        }
    } else {
        // Task completed successfully
        task->result = result;
        concurrent_task_set_state(task, TASK_STATE_COMPLETED);
        
        // Complete the future
        if (task->future) {
            // Note: This is a simplified future completion
            // In a full implementation, we'd properly handle the future
        }
    }
    
    return result;
}

bool concurrent_task_start(ConcurrentTask* task) {
    if (!task) return false;
    
    pthread_mutex_lock(&task->task_mutex);
    
    if (task->state != TASK_STATE_CREATED && task->state != TASK_STATE_READY) {
        pthread_mutex_unlock(&task->task_mutex);
        return false;
    }
    
    // Check dependencies
    bool dependencies_satisfied = true;
    for (int i = 0; i < task->dependency_count; i++) {
        if (task->dependencies[i] && !task->dependencies[i]->is_satisfied) {
            dependencies_satisfied = false;
            break;
        }
    }
    
    if (!dependencies_satisfied) {
        task->state = TASK_STATE_WAITING;
        pthread_mutex_unlock(&task->task_mutex);
        return false;
    }
    
    // Schedule with global scheduler
    ConcurrencyScheduler* scheduler = get_global_concurrency_scheduler();
    if (scheduler) {
        bool scheduled = concurrency_scheduler_schedule(scheduler, task);
        pthread_mutex_unlock(&task->task_mutex);
        return scheduled;
    } else {
        // No scheduler available, run directly
        pthread_mutex_unlock(&task->task_mutex);
        pthread_t thread;
        int result = pthread_create(&thread, NULL, task_execution_wrapper, task);
        if (result == 0) {
            pthread_detach(thread);
            return true;
        }
        return false;
    }
}

bool concurrent_task_cancel(ConcurrentTask* task, CancellationReason reason) {
    if (!task) return false;
    
    // Cancel the task's cancellation token
    cancellation_token_cancel(task->cancellation_token, reason);
    
    pthread_mutex_lock(&task->task_mutex);
    
    // If task hasn't started yet, mark as cancelled immediately
    if (task->state == TASK_STATE_CREATED || task->state == TASK_STATE_WAITING || 
        task->state == TASK_STATE_READY) {
        task->state = TASK_STATE_CANCELLED;
        pthread_cond_broadcast(&task->state_changed);
    }
    // If task is running, it will check the cancellation token and stop
    
    pthread_mutex_unlock(&task->task_mutex);
    return true;
}

void* concurrent_task_await(ConcurrentTask* task) {
    return concurrent_task_await_timeout(task, 0); // 0 means wait indefinitely
}

void* concurrent_task_await_timeout(ConcurrentTask* task, uint64_t timeout_ms) {
    if (!task) return NULL;
    
    pthread_mutex_lock(&task->task_mutex);
    
    // If already completed, return immediately
    if (task->state == TASK_STATE_COMPLETED || 
        task->state == TASK_STATE_FAILED || 
        task->state == TASK_STATE_CANCELLED) {
        void* result = (task->state == TASK_STATE_COMPLETED) ? task->result : NULL;
        pthread_mutex_unlock(&task->task_mutex);
        return result;
    }
    
    // Wait for completion
    bool timed_out = false;
    if (timeout_ms == 0) {
        // Wait indefinitely
        while (task->state != TASK_STATE_COMPLETED && 
               task->state != TASK_STATE_FAILED && 
               task->state != TASK_STATE_CANCELLED) {
            pthread_cond_wait(&task->state_changed, &task->task_mutex);
        }
    } else {
        // Wait with timeout
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += timeout_ms / 1000;
        timeout.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec++;
            timeout.tv_nsec -= 1000000000;
        }
        
        while (task->state != TASK_STATE_COMPLETED && 
               task->state != TASK_STATE_FAILED && 
               task->state != TASK_STATE_CANCELLED) {
            int wait_result = pthread_cond_timedwait(&task->state_changed, &task->task_mutex, &timeout);
            if (wait_result == ETIMEDOUT) {
                timed_out = true;
                break;
            }
        }
    }
    
    void* result = NULL;
    if (!timed_out && task->state == TASK_STATE_COMPLETED) {
        result = task->result;
    }
    
    pthread_mutex_unlock(&task->task_mutex);
    return result;
}

TaskState concurrent_task_get_state(ConcurrentTask* task) {
    if (!task) return TASK_STATE_FAILED;
    
    pthread_mutex_lock(&task->task_mutex);
    TaskState state = task->state;
    pthread_mutex_unlock(&task->task_mutex);
    
    return state;
}

bool concurrent_task_is_completed(ConcurrentTask* task) {
    TaskState state = concurrent_task_get_state(task);
    return (state == TASK_STATE_COMPLETED || 
            state == TASK_STATE_FAILED || 
            state == TASK_STATE_CANCELLED);
}

bool concurrent_task_is_cancelled(ConcurrentTask* task) {
    return concurrent_task_get_state(task) == TASK_STATE_CANCELLED;
}

StructuredError* concurrent_task_get_error(ConcurrentTask* task) {
    if (!task) return NULL;
    
    pthread_mutex_lock(&task->task_mutex);
    StructuredError* error = task->error;
    pthread_mutex_unlock(&task->task_mutex);
    
    return error;
}

// =============================================================================
// Concurrency Scope Implementation
// =============================================================================

ConcurrencyScope* concurrency_scope_create(const char* name, ConcurrencyScope* parent) {
    ConcurrencyScope* scope = calloc(1, sizeof(ConcurrencyScope));
    if (!scope) return NULL;
    
    scope->scope_id = atomic_fetch_add(&g_next_scope_id, 1);
    scope->name = safe_strdup(name);
    scope->parent = parent;
    
    // Initialize arrays
    scope->child_capacity = 4;
    scope->children = calloc(scope->child_capacity, sizeof(ConcurrencyScope*));
    if (!scope->children) {
        free(scope);
        return NULL;
    }
    
    scope->tasks = calloc(16, sizeof(ConcurrentTask*));
    scope->task_groups = calloc(8, sizeof(TaskGroup*));
    scope->parallel_blocks = calloc(8, sizeof(ParallelBlock*));
    
    if (!scope->tasks || !scope->task_groups || !scope->parallel_blocks) {
        free(scope->children);
        free(scope->tasks);
        free(scope->task_groups);
        free(scope->parallel_blocks);
        free(scope);
        return NULL;
    }
    
    // Create cancellation token
    scope->cancellation_token = cancellation_token_create(parent ? parent->cancellation_token : NULL);
    scope->auto_cancel_on_error = true;
    
    // Initialize limits
    scope->memory_limit = 0; // Unlimited by default
    scope->memory_used = 0;
    scope->thread_limit = 0; // Unlimited by default
    scope->active_threads = 0;
    
    // Initialize statistics
    scope->stats.created_at = get_current_time_ns();
    
    // Initialize synchronization
    if (pthread_mutex_init(&scope->scope_mutex, NULL) != 0 ||
        pthread_cond_init(&scope->empty_cond, NULL) != 0) {
        concurrency_scope_destroy(scope);
        return NULL;
    }
    
    // Create arena
    scope->scope_arena = goo_arena_new(8192, scope->name ? scope->name : "concurrency_scope");
    
    // Add to parent's children if parent exists
    if (parent) {
        pthread_mutex_lock(&parent->scope_mutex);
        if (parent->child_count >= parent->child_capacity) {
            int new_capacity = parent->child_capacity * 2;
            ConcurrencyScope** new_children = realloc(parent->children, 
                                                     new_capacity * sizeof(ConcurrencyScope*));
            if (new_children) {
                parent->children = new_children;
                parent->child_capacity = new_capacity;
            }
        }
        if (parent->child_count < parent->child_capacity) {
            parent->children[parent->child_count++] = scope;
        }
        pthread_mutex_unlock(&parent->scope_mutex);
    }
    
    return scope;
}

void concurrency_scope_destroy(ConcurrencyScope* scope) {
    if (!scope) return;
    
    // Cancel all tasks and wait for completion
    concurrency_scope_cancel_all(scope, CANCEL_REASON_SHUTDOWN);
    concurrency_scope_wait_for_completion(scope, 5000); // 5 second timeout
    
    // Clean up children
    pthread_mutex_lock(&scope->scope_mutex);
    for (int i = 0; i < scope->child_count; i++) {
        if (scope->children[i]) {
            concurrency_scope_destroy(scope->children[i]);
        }
    }
    free(scope->children);
    
    // Clean up tasks
    for (int i = 0; i < scope->task_count; i++) {
        if (scope->tasks[i]) {
            concurrent_task_destroy(scope->tasks[i]);
        }
    }
    free(scope->tasks);
    
    // Clean up task groups
    for (int i = 0; i < scope->group_count; i++) {
        if (scope->task_groups[i]) {
            task_group_destroy(scope->task_groups[i]);
        }
    }
    free(scope->task_groups);
    
    // Clean up parallel blocks
    for (int i = 0; i < scope->block_count; i++) {
        if (scope->parallel_blocks[i]) {
            parallel_block_destroy(scope->parallel_blocks[i]);
        }
    }
    free(scope->parallel_blocks);
    
    pthread_mutex_unlock(&scope->scope_mutex);
    
    // Clean up cancellation token
    cancellation_token_destroy(scope->cancellation_token);
    
    // Clean up synchronization
    pthread_cond_destroy(&scope->empty_cond);
    pthread_mutex_destroy(&scope->scope_mutex);
    
    // Clean up arena
    goo_arena_free(scope->scope_arena);
    
    // Clean up name
    free((void*)scope->name);
    
    free(scope);
}

bool concurrency_scope_wait_for_completion(ConcurrencyScope* scope, uint64_t timeout_ms) {
    if (!scope) return false;
    
    pthread_mutex_lock(&scope->scope_mutex);
    
    // Check if already empty
    bool is_empty = (scope->task_count == 0 && scope->child_count == 0);
    if (is_empty) {
        pthread_mutex_unlock(&scope->scope_mutex);
        return true;
    }
    
    bool result = false;
    if (timeout_ms == 0) {
        // Wait indefinitely
        while (scope->task_count > 0 || scope->child_count > 0) {
            pthread_cond_wait(&scope->empty_cond, &scope->scope_mutex);
        }
        result = true;
    } else {
        // Wait with timeout
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += timeout_ms / 1000;
        timeout.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec++;
            timeout.tv_nsec -= 1000000000;
        }
        
        while (scope->task_count > 0 || scope->child_count > 0) {
            int wait_result = pthread_cond_timedwait(&scope->empty_cond, &scope->scope_mutex, &timeout);
            if (wait_result == ETIMEDOUT) {
                break;
            }
        }
        result = (scope->task_count == 0 && scope->child_count == 0);
    }
    
    pthread_mutex_unlock(&scope->scope_mutex);
    return result;
}

void concurrency_scope_cancel_all(ConcurrencyScope* scope, CancellationReason reason) {
    if (!scope) return;
    
    // Cancel the scope's cancellation token (which will cancel all children)
    cancellation_token_cancel(scope->cancellation_token, reason);
    
    pthread_mutex_lock(&scope->scope_mutex);
    
    // Cancel all tasks
    for (int i = 0; i < scope->task_count; i++) {
        if (scope->tasks[i]) {
            concurrent_task_cancel(scope->tasks[i], reason);
        }
    }
    
    // Cancel all child scopes
    for (int i = 0; i < scope->child_count; i++) {
        if (scope->children[i]) {
            concurrency_scope_cancel_all(scope->children[i], reason);
        }
    }
    
    pthread_mutex_unlock(&scope->scope_mutex);
}

// =============================================================================
// Task Group Implementation
// =============================================================================

TaskGroup* task_group_create(const char* name, ConcurrencyScope* parent_scope) {
    TaskGroup* group = calloc(1, sizeof(TaskGroup));
    if (!group) return NULL;
    
    group->group_id = atomic_fetch_add(&g_next_group_id, 1);
    group->name = safe_strdup(name);
    
    // Initialize arrays
    group->task_capacity = 8;
    group->tasks = calloc(group->task_capacity, sizeof(ConcurrentTask*));
    if (!group->tasks) {
        free(group);
        return NULL;
    }
    
    // Set default behavior
    group->fail_fast = true;
    group->wait_for_all = true;
    group->required_success_count = 0; // Will be set to task_count when tasks are added
    
    // Initialize atomic counters
    atomic_init(&group->completed_count, 0);
    atomic_init(&group->failed_count, 0);
    atomic_init(&group->cancelled_count, 0);
    
    // Initialize result arrays
    group->results = NULL;
    group->errors = NULL;
    
    // Initialize synchronization
    if (pthread_mutex_init(&group->group_mutex, NULL) != 0 ||
        pthread_cond_init(&group->completion_cond, NULL) != 0) {
        free(group->tasks);
        free(group);
        return NULL;
    }
    
    // Set parent scope and create cancellation token
    group->parent_scope = parent_scope;
    group->cancellation_token = cancellation_token_create(
        parent_scope ? parent_scope->cancellation_token : NULL);
    
    return group;
}

void task_group_destroy(TaskGroup* group) {
    if (!group) return;
    
    // Wait for completion before destroying
    task_group_wait_for_completion(group, 1000); // 1 second timeout
    
    // Clean up tasks (but don't destroy them, they're owned by scope)
    free(group->tasks);
    
    // Clean up results and errors
    if (group->results) {
        for (int i = 0; i < group->task_count; i++) {
            // Results are owned by tasks, don't free them here
        }
        free(group->results);
    }
    
    if (group->errors) {
        for (int i = 0; i < group->task_count; i++) {
            // Errors are owned by tasks, don't free them here
        }
        free(group->errors);
    }
    
    // Clean up cancellation token
    cancellation_token_destroy(group->cancellation_token);
    
    // Clean up synchronization
    pthread_cond_destroy(&group->completion_cond);
    pthread_mutex_destroy(&group->group_mutex);
    
    // Clean up name
    free((void*)group->name);
    
    free(group);
}

bool task_group_add_task(TaskGroup* group, ConcurrentTask* task) {
    if (!group || !task) return false;
    
    pthread_mutex_lock(&group->group_mutex);
    
    // Resize array if needed
    if (group->task_count >= group->task_capacity) {
        int new_capacity = group->task_capacity * 2;
        ConcurrentTask** new_tasks = realloc(group->tasks, 
                                            new_capacity * sizeof(ConcurrentTask*));
        if (!new_tasks) {
            pthread_mutex_unlock(&group->group_mutex);
            return false;
        }
        group->tasks = new_tasks;
        group->task_capacity = new_capacity;
    }
    
    // Add task to group
    group->tasks[group->task_count++] = task;
    task->task_group = group;
    
    // Update required success count if needed
    if (group->required_success_count == 0) {
        group->required_success_count = group->task_count;
    }
    
    pthread_mutex_unlock(&group->group_mutex);
    return true;
}

bool task_group_wait_for_completion(TaskGroup* group, uint64_t timeout_ms) {
    if (!group) return false;
    
    pthread_mutex_lock(&group->group_mutex);
    
    bool result = false;
    
    // Define completion check function
    bool is_complete = false;
    do {
        int completed = atomic_load(&group->completed_count);
        int failed = atomic_load(&group->failed_count);
        int cancelled = atomic_load(&group->cancelled_count);
        int total_finished = completed + failed + cancelled;
        
        if (group->fail_fast && failed > 0) {
            is_complete = true; // Fail fast on first error
            break;
        }
        
        if (group->wait_for_all) {
            is_complete = (total_finished >= group->task_count);
        } else {
            is_complete = (completed >= group->required_success_count);
        }
        break; // Only check once initially
    } while(0);
    
    if (is_complete) {
        result = true;
    } else if (timeout_ms == 0) {
        // Wait indefinitely - recheck completion in loop
        while (!result) {
            pthread_cond_wait(&group->completion_cond, &group->group_mutex);
            
            // Recheck completion
            int completed = atomic_load(&group->completed_count);
            int failed = atomic_load(&group->failed_count);
            int cancelled = atomic_load(&group->cancelled_count);
            int total_finished = completed + failed + cancelled;
            
            if (group->fail_fast && failed > 0) {
                result = true;
            } else if (group->wait_for_all) {
                result = (total_finished >= group->task_count);
            } else {
                result = (completed >= group->required_success_count);
            }
        }
    } else {
        // Wait with timeout
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += timeout_ms / 1000;
        timeout.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec++;
            timeout.tv_nsec -= 1000000000;
        }
        
        while (!result) {
            int wait_result = pthread_cond_timedwait(&group->completion_cond, &group->group_mutex, &timeout);
            if (wait_result == ETIMEDOUT) {
                break;
            }
            
            // Recheck completion
            int completed = atomic_load(&group->completed_count);
            int failed = atomic_load(&group->failed_count);
            int cancelled = atomic_load(&group->cancelled_count);
            int total_finished = completed + failed + cancelled;
            
            if (group->fail_fast && failed > 0) {
                result = true;
            } else if (group->wait_for_all) {
                result = (total_finished >= group->task_count);
            } else {
                result = (completed >= group->required_success_count);
            }
        }
    }
    
    pthread_mutex_unlock(&group->group_mutex);
    return result;
}

void** task_group_get_results(TaskGroup* group, int* result_count) {
    if (!group || !result_count) return NULL;
    
    pthread_mutex_lock(&group->group_mutex);
    
    if (!group->results) {
        group->results = calloc(group->task_count, sizeof(void*));
        if (group->results) {
            for (int i = 0; i < group->task_count; i++) {
                if (group->tasks[i] && group->tasks[i]->state == TASK_STATE_COMPLETED) {
                    group->results[i] = group->tasks[i]->result;
                }
            }
        }
    }
    
    *result_count = group->task_count;
    void** results = group->results;
    
    pthread_mutex_unlock(&group->group_mutex);
    return results;
}

// =============================================================================
// Placeholder implementations for remaining functions
// =============================================================================

// These would be fully implemented in a complete system

ConcurrencyScheduler* get_global_concurrency_scheduler(void) {
    pthread_mutex_lock(&g_scheduler_mutex);
    
    if (!g_global_scheduler) {
        g_global_scheduler = concurrency_scheduler_create("global", sysconf(_SC_NPROCESSORS_ONLN));
        if (g_global_scheduler) {
            concurrency_scheduler_start(g_global_scheduler);
        }
    }
    
    pthread_mutex_unlock(&g_scheduler_mutex);
    return g_global_scheduler;
}

ConcurrencyScope* get_current_scope(void) {
    return g_current_scope;
}

ParallelBlock* get_current_parallel_block(void) {
    return g_current_parallel_block;
}

// Scheduler stubs
ConcurrencyScheduler* concurrency_scheduler_create(const char* name, int worker_count) {
    (void)name; (void)worker_count;
    return NULL; // Stub
}

void concurrency_scheduler_destroy(ConcurrencyScheduler* scheduler) {
    (void)scheduler; // Stub
}

bool concurrency_scheduler_start(ConcurrencyScheduler* scheduler) {
    (void)scheduler;
    return false; // Stub
}

void concurrency_scheduler_stop(ConcurrencyScheduler* scheduler) {
    (void)scheduler; // Stub
}

bool concurrency_scheduler_schedule(ConcurrencyScheduler* scheduler, ConcurrentTask* task) {
    (void)scheduler;
    if (!task) return false;
    
    // Simple direct execution for now
    pthread_t thread;
    int result = pthread_create(&thread, NULL, task_execution_wrapper, task);
    if (result == 0) {
        pthread_detach(thread);
        return true;
    }
    return false;
}

// Parallel block stubs
ParallelBlock* parallel_block_create(const char* name, ConcurrencyScope* parent_scope) {
    (void)name; (void)parent_scope;
    return NULL; // Stub
}

void parallel_block_destroy(ParallelBlock* block) {
    (void)block; // Stub
}

bool parallel_block_add_task(ParallelBlock* block, ConcurrentTask* task) {
    (void)block; (void)task;
    return false; // Stub
}

void** parallel_block_execute_all(ParallelBlock* block, uint64_t timeout_ms) {
    (void)block; (void)timeout_ms;
    return NULL; // Stub
}

// High-level constructs stubs
void** parallel_execute(ConcurrencyScope* scope, TaskFunction* functions, 
                        void** contexts, int task_count, uint64_t timeout_ms) {
    (void)scope; (void)functions; (void)contexts; (void)task_count; (void)timeout_ms;
    return NULL; // Stub
}

// Dependency stubs
TaskDependency* task_dependency_create(ConcurrentTask* source, ConcurrentTask* target, DependencyType type) {
    (void)source; (void)target; (void)type;
    return NULL; // Stub
}

void task_dependency_destroy(TaskDependency* dependency) {
    (void)dependency; // Stub
}

// Error handling stubs
ErrorSummary* error_summary_create_from_tasks(ConcurrentTask** tasks, int task_count) {
    (void)tasks; (void)task_count;
    return NULL; // Stub
}

void error_summary_destroy(ErrorSummary* summary) {
    (void)summary; // Stub
}

// Monitoring stubs
ConcurrencyMetrics get_concurrency_metrics(ConcurrencyScheduler* scheduler) {
    (void)scheduler;
    ConcurrencyMetrics metrics = {0};
    return metrics;
}

void print_concurrency_metrics(const ConcurrencyMetrics* metrics) {
    if (!metrics) return;
    
    printf("=== Concurrency Metrics ===\n");
    printf("Total tasks created: %lu\n", metrics->total_tasks_created);
    printf("Total tasks completed: %lu\n", metrics->total_tasks_completed);
    printf("Total tasks failed: %lu\n", metrics->total_tasks_failed);
    printf("Total tasks cancelled: %lu\n", metrics->total_tasks_cancelled);
    printf("Average task duration: %.2f ms\n", metrics->average_task_duration_ms);
    printf("CPU utilization: %.2f%%\n", metrics->cpu_utilization * 100.0);
    printf("Memory utilization: %.2f%%\n", metrics->memory_utilization * 100.0);
}

// Debug stubs
void dump_scope_info(ConcurrencyScope* scope) {
    if (!scope) return;
    
    printf("=== Scope Info ===\n");
    printf("ID: %lu\n", scope->scope_id);
    printf("Name: %s\n", scope->name ? scope->name : "unnamed");
    printf("Tasks: %d\n", scope->task_count);
    printf("Child scopes: %d\n", scope->child_count);
    printf("Memory used: %zu bytes\n", scope->memory_used);
    printf("Active threads: %d\n", scope->active_threads);
}

void dump_task_info(ConcurrentTask* task) {
    if (!task) return;
    
    printf("=== Task Info ===\n");
    printf("ID: %lu\n", task->task_id);
    printf("Name: %s\n", task->name ? task->name : "unnamed");
    printf("State: %d\n", task->state);
    printf("Type: %d\n", task->type);
    printf("Priority: %d\n", task->config.priority);
    printf("Progress: %.1f%%\n", task->stats.progress * 100.0);
}

void dump_scheduler_info(ConcurrencyScheduler* scheduler) {
    (void)scheduler; // Stub
    printf("=== Scheduler Info ===\n");
    printf("Status: Not implemented\n");
}