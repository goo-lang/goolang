#include "../../include/work_stealing.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#ifdef __linux__
#include <sched.h>
#endif

// Forward declarations
extern Result_void_ptr parallel_for_task_wrapper(TaskContext* task_ctx, void* args);

// Task arguments structure (matches the one in structured_concurrency.c)
typedef struct ParallelForTaskArgs {
    size_t start;
    size_t end;
    ParallelForFunction func;
    void* user_context;
} ParallelForTaskArgs;

// Helper to get monotonic time (from structured_concurrency.c)
static uint64_t get_monotonic_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Create a work-stealing deque
static WorkStealingDeque* work_stealing_deque_create(size_t capacity) {
    // Ensure capacity is power of 2 for fast modulo
    capacity = next_power_of_2(capacity);
    
    WorkStealingDeque* deque = malloc(sizeof(WorkStealingDeque));
    if (!deque) return NULL;
    
    deque->tasks = calloc(capacity, sizeof(ConcurrentTask*));
    if (!deque->tasks) {
        free(deque);
        return NULL;
    }
    
    deque->capacity = capacity;
    deque->mask = capacity - 1;
    atomic_init(&deque->top, 0);
    atomic_init(&deque->bottom, 0);
    
    return deque;
}

static void work_stealing_deque_destroy(WorkStealingDeque* deque) {
    if (!deque) return;
    free(deque->tasks);
    free(deque);
}

// Push task to bottom (owner only)
bool work_stealing_push_bottom(WorkStealingDeque* deque, ConcurrentTask* task) {
    int64_t b = atomic_load_explicit(&deque->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&deque->top, memory_order_acquire);
    
    if (b - t >= (int64_t)deque->capacity) {
        // Deque is full
        return false;
    }
    
    deque->tasks[b & deque->mask] = task;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&deque->bottom, b + 1, memory_order_relaxed);
    
    return true;
}

// Pop task from bottom (owner only)
ConcurrentTask* work_stealing_pop_bottom(WorkStealingDeque* deque) {
    int64_t b = atomic_load_explicit(&deque->bottom, memory_order_relaxed) - 1;
    atomic_store_explicit(&deque->bottom, b, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    
    int64_t t = atomic_load_explicit(&deque->top, memory_order_relaxed);
    
    if (t > b) {
        // Empty
        atomic_store_explicit(&deque->bottom, t, memory_order_relaxed);
        return NULL;
    }
    
    ConcurrentTask* task = deque->tasks[b & deque->mask];
    
    if (t == b) {
        // Last task - compete with stealers
        if (!atomic_compare_exchange_strong_explicit(
                &deque->top, &t, t + 1,
                memory_order_seq_cst, memory_order_relaxed)) {
            // Lost race
            task = NULL;
        }
        atomic_store_explicit(&deque->bottom, t + 1, memory_order_relaxed);
    }
    
    return task;
}

// Steal task from top (thief only)
ConcurrentTask* work_stealing_steal_top(WorkStealingDeque* deque) {
    int64_t t = atomic_load_explicit(&deque->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    int64_t b = atomic_load_explicit(&deque->bottom, memory_order_acquire);
    
    if (t >= b) {
        return NULL; // Empty
    }
    
    ConcurrentTask* task = deque->tasks[t & deque->mask];
    
    if (!atomic_compare_exchange_strong_explicit(
            &deque->top, &t, t + 1,
            memory_order_seq_cst, memory_order_relaxed)) {
        // Failed to steal
        return NULL;
    }
    
    return task;
}

// Find a task to execute using work stealing
ConcurrentTask* work_stealing_find_task(WorkerContext* worker) {
    WorkStealingScope* ws_scope = (WorkStealingScope*)worker->scope;
    
    // First, try to pop from own deque
    ConcurrentTask* task = work_stealing_pop_bottom(worker->local_deque);
    if (task) return task;
    
    // Try stealing from other workers
    size_t steal_attempts = 0;
    size_t victim_id = xorshift32(&worker->random_state) % ws_scope->worker_count;
    
    while (steal_attempts < ws_scope->steal_attempts_before_overflow) {
        if (victim_id != worker->worker_id) {
            WorkStealingDeque* victim_deque = ws_scope->worker_contexts[victim_id].local_deque;
            task = work_stealing_steal_top(victim_deque);
            
            if (task) {
                atomic_fetch_add(&worker->tasks_stolen, 1);
                atomic_fetch_add(&ws_scope->total_steals, 1);
                return task;
            }
            
            atomic_fetch_add(&worker->failed_steals, 1);
        }
        
        victim_id = (victim_id + 1) % ws_scope->worker_count;
        steal_attempts++;
        atomic_fetch_add(&worker->steal_attempts, 1);
    }
    
    // Last resort: check overflow queue
    pthread_mutex_lock(&ws_scope->overflow_mutex);
    
    size_t head = atomic_load(&ws_scope->overflow_head);
    size_t tail = atomic_load(&ws_scope->overflow_tail);
    
    if (head != tail) {
        task = ws_scope->overflow_queue[head % ws_scope->overflow_capacity];
        atomic_store(&ws_scope->overflow_head, head + 1);
        atomic_fetch_add(&ws_scope->total_overflow_ops, 1);
    }
    
    pthread_mutex_unlock(&ws_scope->overflow_mutex);
    
    return task;
}

// Worker thread function with work stealing
void* work_stealing_worker_thread(void* arg) {
    WorkerContext* worker = (WorkerContext*)arg;
    WorkStealingScope* ws_scope = (WorkStealingScope*)worker->scope;
    
    // Set CPU affinity if configured (Linux only)
#ifdef __linux__
    if (worker->preferred_cpu >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(worker->preferred_cpu, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }
#endif
    
    while (ws_scope->base_scope.is_active && !ws_scope->base_scope.shutdown_requested) {
        ConcurrentTask* task = work_stealing_find_task(worker);
        
        if (task) {
            // Execute task
            pthread_mutex_lock(&task->task_mutex);
            
            if (task->state == TASK_STATE_QUEUED && !atomic_load(&task->context.is_cancelled)) {
                task->state = TASK_STATE_RUNNING;
                uint64_t start_time = get_monotonic_time_ns();
                task->context.start_time_ns = start_time;
                
                pthread_mutex_unlock(&task->task_mutex);
                
                // Execute task function
                Result_void_ptr result = task->function(&task->context, task->arguments);
                
                pthread_mutex_lock(&task->task_mutex);
                
                uint64_t end_time = get_monotonic_time_ns();
                task->context.end_time_ns = end_time;
                
                if (result.is_error) {
                    task->state = TASK_STATE_ERROR;
                    task->error = result.error;
                } else {
                    task->state = TASK_STATE_COMPLETED;
                    task->result = result.value;
                }
                
                pthread_cond_broadcast(&task->state_changed);
                atomic_fetch_add(&worker->tasks_executed, 1);
                
                // Update dynamic chunking metrics if enabled
                if (ws_scope->enable_dynamic_chunking && ws_scope->chunking_context) {
                    uint64_t execution_time = end_time - start_time;
                    dynamic_chunking_update_metrics(ws_scope->chunking_context, 
                                                   worker->worker_id, execution_time, 1);
                }
            }
            
            pthread_mutex_unlock(&task->task_mutex);
        } else {
            // No work available - brief sleep
            usleep(1000); // 1ms
        }
    }
    
    return NULL;
}

// Create work-stealing scope with dynamic chunking
WorkStealingScope* work_stealing_scope_create_with_chunking(size_t worker_count, const char* name, 
                                                           DynamicChunkingConfig chunking_config) {
    WorkStealingScope* ws_scope = work_stealing_scope_create(worker_count, name);
    if (!ws_scope) return NULL;
    
    // Enable dynamic chunking
    ws_scope->chunking_context = dynamic_chunking_create(chunking_config, worker_count);
    if (!ws_scope->chunking_context) {
        work_stealing_scope_destroy(ws_scope);
        return NULL;
    }
    
    ws_scope->enable_dynamic_chunking = true;
    return ws_scope;
}

// Create work-stealing scope
WorkStealingScope* work_stealing_scope_create(size_t worker_count, const char* name) {
    if (worker_count == 0) worker_count = sysconf(_SC_NPROCESSORS_ONLN);
    
    WorkStealingScope* ws_scope = calloc(1, sizeof(WorkStealingScope));
    if (!ws_scope) return NULL;
    
    // Initialize base scope
    TaskScopeConfig config = task_scope_config_default();
    config.max_concurrent_tasks = worker_count;
    config.use_work_stealing = true;
    
    TaskScope* base = task_scope_create(config, name);
    if (!base) {
        free(ws_scope);
        return NULL;
    }
    
    // Copy base scope fields
    memcpy(&ws_scope->base_scope, base, sizeof(TaskScope));
    free(base); // Free the temporary base scope
    
    // Initialize work-stealing specific fields
    ws_scope->worker_count = worker_count;
    ws_scope->steal_attempts_before_overflow = 3;
    ws_scope->min_tasks_before_stealing = 2;
    ws_scope->enable_work_stealing = true;
    
    // Create worker contexts
    ws_scope->worker_contexts = calloc(worker_count, sizeof(WorkerContext));
    if (!ws_scope->worker_contexts) {
        task_scope_destroy(&ws_scope->base_scope);
        free(ws_scope);
        return NULL;
    }
    
    // Initialize each worker
    for (size_t i = 0; i < worker_count; i++) {
        WorkerContext* worker = &ws_scope->worker_contexts[i];
        worker->worker_id = i;
        worker->scope = &ws_scope->base_scope;
        worker->random_state = (uint32_t)(i + 1) * 0x9e3779b9; // Golden ratio hash
        worker->preferred_cpu = i % sysconf(_SC_NPROCESSORS_ONLN);
        worker->numa_node = -1; // TODO: Detect NUMA topology
        
        // Create local deque
        worker->local_deque = work_stealing_deque_create(1024);
        if (!worker->local_deque) {
            // Cleanup on failure
            for (size_t j = 0; j < i; j++) {
                work_stealing_deque_destroy(ws_scope->worker_contexts[j].local_deque);
            }
            free(ws_scope->worker_contexts);
            task_scope_destroy(&ws_scope->base_scope);
            free(ws_scope);
            return NULL;
        }
        
        atomic_init(&worker->tasks_executed, 0);
        atomic_init(&worker->tasks_stolen, 0);
        atomic_init(&worker->steal_attempts, 0);
        atomic_init(&worker->failed_steals, 0);
    }
    
    // Initialize overflow queue
    ws_scope->overflow_capacity = 4096;
    ws_scope->overflow_queue = calloc(ws_scope->overflow_capacity, sizeof(ConcurrentTask*));
    if (!ws_scope->overflow_queue) {
        for (size_t i = 0; i < worker_count; i++) {
            work_stealing_deque_destroy(ws_scope->worker_contexts[i].local_deque);
        }
        free(ws_scope->worker_contexts);
        task_scope_destroy(&ws_scope->base_scope);
        free(ws_scope);
        return NULL;
    }
    
    atomic_init(&ws_scope->overflow_head, 0);
    atomic_init(&ws_scope->overflow_tail, 0);
    pthread_mutex_init(&ws_scope->overflow_mutex, NULL);
    
    atomic_init(&ws_scope->total_steals, 0);
    atomic_init(&ws_scope->total_overflow_ops, 0);
    
    return ws_scope;
}

// Enhanced parallel for with work stealing
Result_void_ptr work_stealing_parallel_for(
    WorkStealingScope* scope,
    WorkStealingParallelForConfig config,
    ParallelForFunction function,
    void* context) {
    
    if (!scope || !function) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid scope or function"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    size_t total_items = config.base_config.end_index - config.base_config.start_index;
    if (total_items == 0) return OK_PTR(NULL);
    
    // Start worker threads if not already running
    if (!scope->base_scope.is_active) {
        scope->base_scope.is_active = true;
        
        // Create worker threads using our work-stealing function
        for (size_t i = 0; i < scope->worker_count; i++) {
            if (pthread_create(&scope->base_scope.worker_threads[i], NULL, 
                             work_stealing_worker_thread, &scope->worker_contexts[i]) != 0) {
                scope->base_scope.is_active = false;
                Error* error = malloc(sizeof(Error));
                *error = (Error){
                    .code = ERROR_INTERNAL,
                    .severity = ERROR_SEVERITY_ERROR,
                    .category = ERROR_CATEGORY_INTERNAL,
                    .message = strdup("Failed to create worker threads"),
                    .hint = NULL,
                    .location = (SourceLocation){0},
                    .next = NULL
                };
                return ERR_PTR(error);
            }
        }
    }
    
    // Determine initial chunk size
    size_t chunk_size = config.initial_chunk_size;
    if (chunk_size == 0) {
        chunk_size = (total_items + scope->worker_count - 1) / scope->worker_count;
        if (chunk_size < config.min_steal_size) {
            chunk_size = config.min_steal_size;
        }
    }
    
    // Create tasks and distribute to workers
    size_t worker_id = 0;
    size_t tasks_created = 0;
    
    // Track all tasks for proper waiting
    ConcurrentTask** all_tasks = malloc(((total_items + chunk_size - 1) / chunk_size) * sizeof(ConcurrentTask*));
    if (!all_tasks) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to allocate task tracking array"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    for (size_t start = config.base_config.start_index; 
         start < config.base_config.end_index; 
         start += chunk_size) {
        
        size_t end = start + chunk_size;
        if (end > config.base_config.end_index) {
            end = config.base_config.end_index;
        }
        
        // Create task arguments
        ParallelForTaskArgs* args = malloc(sizeof(ParallelForTaskArgs));
        if (!args) continue;
        
        *args = (ParallelForTaskArgs){
            .start = start,
            .end = end,
            .func = function,
            .user_context = context
        };
        
        // Create task
        char task_name[64];
        snprintf(task_name, sizeof(task_name), "ws_parallel_for_%zu_%zu", start, end);
        
        ConcurrentTask* task = task_create(&scope->base_scope, parallel_for_task_wrapper,
                                         args, sizeof(ParallelForTaskArgs), task_name);
        if (!task) {
            free(args);
            continue;
        }
        
        // Track the task
        all_tasks[tasks_created] = task;
        
        // Set task state to QUEUED
        pthread_mutex_lock(&task->task_mutex);
        task->state = TASK_STATE_QUEUED;
        pthread_mutex_unlock(&task->task_mutex);
        
        // Push to worker's local deque
        WorkerContext* worker = &scope->worker_contexts[worker_id];
        if (!work_stealing_push_bottom(worker->local_deque, task)) {
            // Deque full - push to overflow queue
            pthread_mutex_lock(&scope->overflow_mutex);
            
            size_t tail = atomic_load(&scope->overflow_tail);
            scope->overflow_queue[tail % scope->overflow_capacity] = task;
            atomic_store(&scope->overflow_tail, tail + 1);
            
            pthread_mutex_unlock(&scope->overflow_mutex);
        }
        
        tasks_created++;
        worker_id = (worker_id + 1) % scope->worker_count;
        
        // Adaptive chunking: adjust chunk size based on progress
        if (config.adaptive_chunking && tasks_created % scope->worker_count == 0) {
            // Simple heuristic: if we're creating too many tasks, increase chunk size
            if (tasks_created > scope->worker_count * 10) {
                chunk_size = chunk_size * 3 / 2;
            }
        }
    }
    
    // Wait for all tasks to complete
    Result_void_ptr first_error = OK_PTR(NULL);
    
    for (size_t i = 0; i < tasks_created; i++) {
        Result_void_ptr wait_result = task_wait(all_tasks[i], UINT64_MAX);
        if (wait_result.is_error && first_error.is_error == false) {
            first_error = wait_result;
        }
    }
    
    // Clean up task tracking
    free(all_tasks);
    
    return first_error;
}

// Cleanup
void work_stealing_scope_destroy(WorkStealingScope* ws_scope) {
    if (!ws_scope) return;
    
    // Shutdown workers
    ws_scope->base_scope.shutdown_requested = true;
    
    // Wait for workers to finish
    for (size_t i = 0; i < ws_scope->worker_count; i++) {
        pthread_join(ws_scope->base_scope.worker_threads[i], NULL);
    }
    
    // Clean up worker contexts
    for (size_t i = 0; i < ws_scope->worker_count; i++) {
        work_stealing_deque_destroy(ws_scope->worker_contexts[i].local_deque);
    }
    free(ws_scope->worker_contexts);
    
    // Clean up overflow queue
    free(ws_scope->overflow_queue);
    pthread_mutex_destroy(&ws_scope->overflow_mutex);
    
    // Clean up base scope
    task_scope_destroy(&ws_scope->base_scope);
    
    free(ws_scope);
}

