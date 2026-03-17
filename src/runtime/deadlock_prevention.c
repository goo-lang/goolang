#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "deadlock_prevention.h"
#include "error_hierarchies.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sched.h>
#if defined(NUMA_AVAILABLE) && NUMA_AVAILABLE == 1
#include <numa.h>
#endif

// =============================================================================
// Global State
// =============================================================================

static DeadlockDetector* g_global_detector = NULL;
static WorkStealingScheduler* g_global_scheduler = NULL;
static NUMAManager* g_global_numa_manager = NULL;
static PerformanceMonitor* g_global_monitor = NULL;
static pthread_mutex_t g_global_mutex = PTHREAD_MUTEX_INITIALIZER;

// ID counters
static atomic_uint_least64_t g_next_resource_id = 1;
static atomic_uint_least64_t g_next_task_id = 1;

// =============================================================================
// Utility Functions
// =============================================================================

static uint64_t get_current_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000UL + (uint64_t)ts.tv_nsec;
}


// =============================================================================
// Resource Descriptor Implementation
// =============================================================================

ResourceDescriptor* resource_descriptor_create(uint64_t id, ResourceType type, 
                                              void* resource, const char* name) {
    if (id == 0) {
        id = atomic_fetch_add(&g_next_resource_id, 1);
    }
    
    ResourceDescriptor* desc = calloc(1, sizeof(ResourceDescriptor));
    if (!desc) return NULL;
    
    desc->resource_id = id;
    desc->type = type;
    desc->resource_ptr = resource;
    desc->name = name ? strdup(name) : NULL;
    desc->hierarchy_level = 0;
    desc->creation_time = get_current_time_ns();
    desc->priority = 50; // Default priority
    desc->compare_func = NULL;
    
    atomic_init(&desc->acquisition_count, 0);
    atomic_init(&desc->contention_count, 0);
    desc->avg_hold_time_ns = 0.0;
    
    return desc;
}

void resource_descriptor_destroy(ResourceDescriptor* desc) {
    if (!desc) return;
    
    free((void*)desc->name);
    free(desc);
}

// =============================================================================
// Lock Graph Implementation
// =============================================================================

static LockGraph* lock_graph_create(size_t initial_capacity) {
    LockGraph* graph = calloc(1, sizeof(LockGraph));
    if (!graph) return NULL;
    
    graph->node_capacity = initial_capacity > 0 ? initial_capacity : 64;
    graph->nodes = calloc(graph->node_capacity, sizeof(LockNode*));
    if (!graph->nodes) {
        free(graph);
        return NULL;
    }
    
    if (pthread_rwlock_init(&graph->graph_lock, NULL) != 0) {
        free(graph->nodes);
        free(graph);
        return NULL;
    }
    
    // Initialize cycle detection cache
    graph->cache_size = graph->node_capacity;
    graph->visited = calloc(graph->cache_size, sizeof(bool));
    graph->recursion_stack = calloc(graph->cache_size, sizeof(bool));
    
    graph->cycle_detection_count = 0;
    graph->deadlocks_prevented = 0;
    graph->avg_detection_time_ns = 0.0;
    
    return graph;
}

static void lock_graph_destroy(LockGraph* graph) {
    if (!graph) return;
    
    // Clean up nodes
    for (size_t i = 0; i < graph->node_count; i++) {
        if (graph->nodes[i]) {
            free(graph->nodes[i]->dependencies);
            free(graph->nodes[i]->dependents);
            free(graph->nodes[i]);
        }
    }
    
    free(graph->nodes);
    free(graph->visited);
    free(graph->recursion_stack);
    
    pthread_rwlock_destroy(&graph->graph_lock);
    free(graph);
}


static bool lock_graph_has_cycle_dfs(LockGraph* graph, size_t node_index) {
    if (node_index >= graph->node_count) return false;
    
    graph->visited[node_index] = true;
    graph->recursion_stack[node_index] = true;
    
    LockNode* node = graph->nodes[node_index];
    if (!node) return false;
    
    // Check all dependencies
    for (size_t i = 0; i < node->dependency_count; i++) {
        if (!node->dependencies[i]) continue;
        
        // Find the index of the dependency node
        size_t dep_index = SIZE_MAX;
        for (size_t j = 0; j < graph->node_count; j++) {
            if (graph->nodes[j] == node->dependencies[i]) {
                dep_index = j;
                break;
            }
        }
        
        if (dep_index == SIZE_MAX) continue;
        
        if (graph->recursion_stack[dep_index]) {
            return true; // Back edge found - cycle detected
        }
        
        if (!graph->visited[dep_index] && 
            lock_graph_has_cycle_dfs(graph, dep_index)) {
            return true;
        }
    }
    
    graph->recursion_stack[node_index] = false;
    return false;
}

static bool lock_graph_detect_cycle(LockGraph* graph) {
    uint64_t start_time = get_current_time_ns();
    
    // Reset visited arrays
    memset(graph->visited, false, graph->cache_size * sizeof(bool));
    memset(graph->recursion_stack, false, graph->cache_size * sizeof(bool));
    
    // Check for cycles using DFS
    bool has_cycle = false;
    for (size_t i = 0; i < graph->node_count && !has_cycle; i++) {
        if (!graph->visited[i]) {
            has_cycle = lock_graph_has_cycle_dfs(graph, i);
        }
    }
    
    // Update statistics
    graph->cycle_detection_count++;
    uint64_t end_time = get_current_time_ns();
    double detection_time = (double)(end_time - start_time);
    graph->avg_detection_time_ns = (graph->avg_detection_time_ns * 0.9) + 
                                  (detection_time * 0.1);
    
    if (has_cycle) {
        graph->deadlocks_prevented++;
    }
    
    return has_cycle;
}

// =============================================================================
// Deadlock Detector Implementation
// =============================================================================

DeadlockDetector* deadlock_detector_create(LockOrderPolicy policy) {
    DeadlockDetector* detector = calloc(1, sizeof(DeadlockDetector));
    if (!detector) return NULL;
    
    detector->resource_graph = lock_graph_create(128);
    if (!detector->resource_graph) {
        free(detector);
        return NULL;
    }
    
    detector->order_policy = policy;
    detector->enable_static_analysis = true;
    detector->enable_runtime_detection = true;
    detector->detection_interval_ms = 100; // 100ms detection interval
    detector->timeout_threshold_ms = 5000; // 5 second timeout threshold
    
    // Initialize recovery settings
    detector->recovery.enable_timeout_based = true;
    detector->recovery.enable_preemption = true;
    detector->recovery.enable_restart = false;
    detector->recovery.recovery_timeout_ms = 1000;
    
    // Initialize statistics
    atomic_init(&detector->stats.deadlocks_detected, 0);
    atomic_init(&detector->stats.deadlocks_prevented, 0);
    atomic_init(&detector->stats.false_positives, 0);
    atomic_init(&detector->stats.recovery_attempts, 0);
    atomic_init(&detector->stats.successful_recoveries, 0);
    
    atomic_init(&detector->detector_active, false);
    
    return detector;
}

void deadlock_detector_destroy(DeadlockDetector* detector) {
    if (!detector) return;
    
    deadlock_detector_stop(detector);
    lock_graph_destroy(detector->resource_graph);
    free(detector);
}

// Deadlock detection thread
static void* deadlock_detection_thread(void* arg) {
    DeadlockDetector* detector = (DeadlockDetector*)arg;
    
    while (atomic_load(&detector->detector_active)) {
        pthread_rwlock_rdlock(&detector->resource_graph->graph_lock);
        
        if (lock_graph_detect_cycle(detector->resource_graph)) {
            atomic_fetch_add(&detector->stats.deadlocks_detected, 1);
            
            // TODO: Implement deadlock recovery mechanisms
            printf("Deadlock detected! Implementing recovery...\n");
        }
        
        pthread_rwlock_unlock(&detector->resource_graph->graph_lock);
        
        // Sleep for detection interval
        struct timespec sleep_time;
        sleep_time.tv_sec = detector->detection_interval_ms / 1000;
        sleep_time.tv_nsec = (detector->detection_interval_ms % 1000) * 1000000;
        nanosleep(&sleep_time, NULL);
    }
    
    return NULL;
}

bool deadlock_detector_start(DeadlockDetector* detector) {
    if (!detector || atomic_load(&detector->detector_active)) {
        return false;
    }
    
    atomic_store(&detector->detector_active, true);
    
    if (detector->enable_runtime_detection) {
        if (pthread_create(&detector->detector_thread, NULL, 
                          deadlock_detection_thread, detector) != 0) {
            atomic_store(&detector->detector_active, false);
            return false;
        }
    }
    
    return true;
}

void deadlock_detector_stop(DeadlockDetector* detector) {
    if (!detector || !atomic_load(&detector->detector_active)) {
        return;
    }
    
    atomic_store(&detector->detector_active, false);
    
    if (detector->enable_runtime_detection) {
        pthread_join(detector->detector_thread, NULL);
    }
}

// =============================================================================
// Lock Ordering Implementation
// =============================================================================

static int compare_resources_by_id(const void* a, const void* b) {
    const LockRequest* req_a = (const LockRequest*)a;
    const LockRequest* req_b = (const LockRequest*)b;
    
    uint64_t id_a = req_a->resource->resource_id;
    uint64_t id_b = req_b->resource->resource_id;
    
    return (id_a < id_b) ? -1 : (id_a > id_b) ? 1 : 0;
}

static int compare_resources_by_address(const void* a, const void* b) {
    const LockRequest* req_a = (const LockRequest*)a;
    const LockRequest* req_b = (const LockRequest*)b;
    
    uintptr_t addr_a = (uintptr_t)req_a->resource->resource_ptr;
    uintptr_t addr_b = (uintptr_t)req_b->resource->resource_ptr;
    
    return (addr_a < addr_b) ? -1 : (addr_a > addr_b) ? 1 : 0;
}

static int compare_resources_by_hierarchy(const void* a, const void* b) {
    const LockRequest* req_a = (const LockRequest*)a;
    const LockRequest* req_b = (const LockRequest*)b;
    
    uint32_t level_a = req_a->resource->hierarchy_level;
    uint32_t level_b = req_b->resource->hierarchy_level;
    
    return (level_a < level_b) ? -1 : (level_a > level_b) ? 1 : 0;
}

static bool order_lock_requests(DeadlockDetector* detector, 
                               LockRequest* requests, size_t count) {
    if (!detector || !requests || count == 0) return false;
    
    // Choose comparison function based on ordering policy
    int (*compare_func)(const void*, const void*) = NULL;
    
    switch (detector->order_policy) {
        case LOCK_ORDER_RESOURCE_ID:
            compare_func = compare_resources_by_id;
            break;
        case LOCK_ORDER_ADDRESS:
            compare_func = compare_resources_by_address;
            break;
        case LOCK_ORDER_HIERARCHY:
            compare_func = compare_resources_by_hierarchy;
            break;
        case LOCK_ORDER_TIMESTAMP:
            // TODO: Implement timestamp-based ordering
            compare_func = compare_resources_by_id;
            break;
        case LOCK_ORDER_PRIORITY:
            // TODO: Implement priority-based ordering
            compare_func = compare_resources_by_id;
            break;
        case LOCK_ORDER_CUSTOM:
            // TODO: Use custom comparison function
            compare_func = compare_resources_by_id;
            break;
        default:
            compare_func = compare_resources_by_id;
            break;
    }
    
    // Sort requests to ensure consistent lock ordering
    qsort(requests, count, sizeof(LockRequest), compare_func);
    
    return true;
}

bool acquire_lock_ordered(DeadlockDetector* detector, LockRequest* request) {
    if (!detector || !request || !request->resource) return false;
    
    // For single lock, just acquire it
    return try_acquire_multiple_locks(detector, request, 1);
}

bool try_acquire_multiple_locks(DeadlockDetector* detector, 
                               LockRequest* requests, size_t count) {
    if (!detector || !requests || count == 0) return false;
    
    // Order the lock requests to prevent deadlocks
    if (!order_lock_requests(detector, requests, count)) {
        return false;
    }
    
    uint64_t start_time = get_current_time_ns();
    bool all_acquired = true;
    
    // Try to acquire all locks in order
    for (size_t i = 0; i < count; i++) {
        LockRequest* req = &requests[i];
        
        // Check timeout
        if (req->timeout_ns > 0) {
            uint64_t elapsed = get_current_time_ns() - start_time;
            if (elapsed >= req->timeout_ns) {
                all_acquired = false;
                break;
            }
        }
        
        // Update statistics
        atomic_fetch_add(&req->resource->acquisition_count, 1);
        
        // TODO: Implement actual lock acquisition based on resource type
        // This is a simplified version that just simulates lock acquisition
        
        if (req->acquired_callback) {
            req->acquired_callback(req, true);
        }
    }
    
    // If not all locks were acquired, release the ones we got
    if (!all_acquired) {
        for (size_t i = 0; i < count; i++) {
            // TODO: Release locks that were acquired
        }
        
        atomic_fetch_add(&detector->stats.deadlocks_prevented, 1);
    }
    
    return all_acquired;
}

bool release_lock_ordered(DeadlockDetector* detector, uint64_t resource_id, 
                         uint64_t thread_id) {
    if (!detector) return false;
    
    // TODO: Implement lock release with graph update
    (void)resource_id;
    (void)thread_id;
    
    return true;
}

// =============================================================================
// Work-Stealing Scheduler Implementation
// =============================================================================

WorkTask* work_task_create(void (*function)(void*), void* context, uint32_t priority) {
    if (!function) return NULL;
    
    WorkTask* task = calloc(1, sizeof(WorkTask));
    if (!task) return NULL;
    
    task->task_id = atomic_fetch_add(&g_next_task_id, 1);
    task->function = function;
    task->context = context;
    task->priority = priority;
    task->is_parallel = true;
    task->estimated_work = 1;
    
    task->dependencies = NULL;
    task->dependency_count = 0;
    atomic_init(&task->pending_deps, 0);
    
    atomic_init(&task->completed, false);
    task->completion_callback = NULL;
    task->completion_context = NULL;
    
    task->created_time = get_current_time_ns();
    task->started_time = 0;
    task->completed_time = 0;
    task->steal_count = 0;
    
    return task;
}

void work_task_destroy(WorkTask* task) {
    if (!task) return;
    
    free(task->dependencies);
    free(task);
}

static WorkDeque* work_deque_create(size_t capacity) {
    WorkDeque* deque = calloc(1, sizeof(WorkDeque));
    if (!deque) return NULL;
    
    deque->capacity = capacity > 0 ? capacity : 1024;
    deque->tasks = calloc(deque->capacity, sizeof(WorkTask*));
    if (!deque->tasks) {
        free(deque);
        return NULL;
    }
    
    atomic_init(&deque->top, 0);
    atomic_init(&deque->bottom, 0);
    
    if (pthread_mutex_init(&deque->steal_mutex, NULL) != 0) {
        free(deque->tasks);
        free(deque);
        return NULL;
    }
    
    atomic_init(&deque->tasks_added, 0);
    atomic_init(&deque->tasks_stolen, 0);
    atomic_init(&deque->steal_attempts, 0);
    
    return deque;
}

static void work_deque_destroy(WorkDeque* deque) {
    if (!deque) return;
    
    free(deque->tasks);
    pthread_mutex_destroy(&deque->steal_mutex);
    free(deque);
}

static bool work_deque_push_bottom(WorkDeque* deque, WorkTask* task) {
    if (!deque || !task) return false;
    
    size_t bottom = atomic_load(&deque->bottom);
    size_t top = atomic_load(&deque->top);
    
    // Check if deque is full
    if (bottom - top >= deque->capacity) {
        return false;
    }
    
    deque->tasks[bottom % deque->capacity] = task;
    atomic_store(&deque->bottom, bottom + 1);
    atomic_fetch_add(&deque->tasks_added, 1);
    
    return true;
}

static WorkTask* work_deque_pop_bottom(WorkDeque* deque) {
    if (!deque) return NULL;
    
    size_t bottom = atomic_load(&deque->bottom);
    if (bottom == 0) return NULL;
    
    bottom--;
    atomic_store(&deque->bottom, bottom);
    
    size_t top = atomic_load(&deque->top);
    if (top <= bottom) {
        WorkTask* task = deque->tasks[bottom % deque->capacity];
        
        if (top == bottom) {
            // Last task - use CAS to avoid race with steal
            if (!atomic_compare_exchange_strong(&deque->top, &top, top + 1)) {
                atomic_store(&deque->bottom, bottom + 1);
                return NULL;
            }
        }
        
        return task;
    } else {
        atomic_store(&deque->bottom, bottom + 1);
        return NULL;
    }
}

static WorkTask* work_deque_steal_top(WorkDeque* deque) {
    if (!deque) return NULL;
    
    pthread_mutex_lock(&deque->steal_mutex);
    atomic_fetch_add(&deque->steal_attempts, 1);
    
    size_t top = atomic_load(&deque->top);
    size_t bottom = atomic_load(&deque->bottom);
    
    if (top < bottom) {
        WorkTask* task = deque->tasks[top % deque->capacity];
        if (atomic_compare_exchange_strong(&deque->top, &top, top + 1)) {
            atomic_fetch_add(&deque->tasks_stolen, 1);
            task->steal_count++;
            pthread_mutex_unlock(&deque->steal_mutex);
            return task;
        }
    }
    
    pthread_mutex_unlock(&deque->steal_mutex);
    return NULL;
}

// Worker thread implementation
static void* worker_thread_function(void* arg) {
    WorkerThread* worker = (WorkerThread*)arg;
    WorkStealingScheduler* scheduler = (WorkStealingScheduler*)worker; // This is a hack - fix this
    
    while (atomic_load(&worker->active)) {
        WorkTask* task = NULL;
        
        // Try to get task from local deque
        task = work_deque_pop_bottom(worker->deque);
        
        // If no local task, try to steal from other workers
        if (!task && scheduler) {
            for (size_t i = 0; i < scheduler->worker_count && !task; i++) {
                if (i != worker->worker_id) {
                    task = work_deque_steal_top(scheduler->workers[i].deque);
                    if (task) {
                        atomic_fetch_add(&worker->stats.tasks_stolen, 1);
                    }
                }
            }
        }
        
        // If still no task, check global queue
        if (!task && scheduler) {
            pthread_mutex_lock(&scheduler->global_mutex);
            if (atomic_load(&scheduler->global_head) != atomic_load(&scheduler->global_tail)) {
                size_t head = atomic_load(&scheduler->global_head);
                task = scheduler->global_queue[head % scheduler->global_capacity];
                atomic_store(&scheduler->global_head, head + 1);
            }
            pthread_mutex_unlock(&scheduler->global_mutex);
        }
        
        if (task) {
            // Execute task
            task->started_time = get_current_time_ns();
            task->function(task->context);
            task->completed_time = get_current_time_ns();
            atomic_store(&task->completed, true);
            
            atomic_fetch_add(&worker->stats.tasks_executed, 1);
            
            if (task->completion_callback) {
                task->completion_callback(task, true);
            }
        } else {
            // No work available - sleep briefly
            atomic_store(&worker->sleeping, true);
            usleep(1000); // Sleep for 1ms
            atomic_store(&worker->sleeping, false);
        }
    }
    
    return NULL;
}

WorkStealingScheduler* work_stealing_scheduler_create(size_t worker_count) {
    if (worker_count == 0) {
        worker_count = sysconf(_SC_NPROCESSORS_ONLN);
    }
    
    WorkStealingScheduler* scheduler = calloc(1, sizeof(WorkStealingScheduler));
    if (!scheduler) return NULL;
    
    scheduler->worker_count = worker_count;
    scheduler->workers = calloc(worker_count, sizeof(WorkerThread));
    if (!scheduler->workers) {
        free(scheduler);
        return NULL;
    }
    
    // Initialize workers
    for (size_t i = 0; i < worker_count; i++) {
        WorkerThread* worker = &scheduler->workers[i];
        worker->worker_id = i;
        worker->deque = work_deque_create(1024);
        if (!worker->deque) {
            // Clean up
            for (size_t j = 0; j < i; j++) {
                work_deque_destroy(scheduler->workers[j].deque);
            }
            free(scheduler->workers);
            free(scheduler);
            return NULL;
        }
        
        atomic_init(&worker->active, false);
        atomic_init(&worker->sleeping, false);
        worker->cpu_affinity = i % sysconf(_SC_NPROCESSORS_ONLN);
        worker->numa_node = 0; // Default NUMA node
        worker->random_state = get_current_time_ns() ^ i;
        
        // Initialize statistics
        atomic_init(&worker->stats.tasks_executed, 0);
        atomic_init(&worker->stats.tasks_stolen, 0);
        atomic_init(&worker->stats.steal_attempts, 0);
        atomic_init(&worker->stats.failed_steals, 0);
        worker->stats.cpu_utilization = 0.0;
        worker->stats.idle_time_ns = 0;
    }
    
    // Initialize global queue
    scheduler->global_capacity = 1024;
    scheduler->global_queue = calloc(scheduler->global_capacity, sizeof(WorkTask*));
    if (!scheduler->global_queue) {
        for (size_t i = 0; i < worker_count; i++) {
            work_deque_destroy(scheduler->workers[i].deque);
        }
        free(scheduler->workers);
        free(scheduler);
        return NULL;
    }
    
    atomic_init(&scheduler->global_head, 0);
    atomic_init(&scheduler->global_tail, 0);
    
    if (pthread_mutex_init(&scheduler->global_mutex, NULL) != 0 ||
        pthread_cond_init(&scheduler->global_not_empty, NULL) != 0) {
        free(scheduler->global_queue);
        for (size_t i = 0; i < worker_count; i++) {
            work_deque_destroy(scheduler->workers[i].deque);
        }
        free(scheduler->workers);
        free(scheduler);
        return NULL;
    }
    
    // Configuration
    scheduler->enable_work_stealing = true;
    scheduler->enable_load_balancing = true;
    scheduler->steal_batch_size = 1;
    scheduler->idle_sleep_ns = 1000000; // 1ms
    
    scheduler->worker_loads = calloc(worker_count, sizeof(atomic_uint_least32_t));
    scheduler->load_balance_interval_ms = 100;
    
    atomic_init(&scheduler->scheduler_active, false);
    atomic_init(&scheduler->shutdown_requested, false);
    
    return scheduler;
}

void work_stealing_scheduler_destroy(WorkStealingScheduler* scheduler) {
    if (!scheduler) return;
    
    work_stealing_scheduler_stop(scheduler);
    
    // Clean up workers
    for (size_t i = 0; i < scheduler->worker_count; i++) {
        work_deque_destroy(scheduler->workers[i].deque);
    }
    free(scheduler->workers);
    
    // Clean up global queue
    free(scheduler->global_queue);
    pthread_mutex_destroy(&scheduler->global_mutex);
    pthread_cond_destroy(&scheduler->global_not_empty);
    
    free(scheduler->worker_loads);
    free(scheduler);
}

bool work_stealing_scheduler_start(WorkStealingScheduler* scheduler) {
    if (!scheduler || atomic_load(&scheduler->scheduler_active)) {
        return false;
    }
    
    atomic_store(&scheduler->scheduler_active, true);
    atomic_store(&scheduler->shutdown_requested, false);
    
    // Start worker threads
    for (size_t i = 0; i < scheduler->worker_count; i++) {
        WorkerThread* worker = &scheduler->workers[i];
        atomic_store(&worker->active, true);
        
        if (pthread_create(&worker->thread, NULL, worker_thread_function, worker) != 0) {
            // Failed to create thread - clean up
            atomic_store(&worker->active, false);
            atomic_store(&scheduler->scheduler_active, false);
            
            // Stop already started threads
            for (size_t j = 0; j < i; j++) {
                atomic_store(&scheduler->workers[j].active, false);
                pthread_join(scheduler->workers[j].thread, NULL);
            }
            
            return false;
        }
        
        // Set CPU affinity
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(worker->cpu_affinity, &cpuset);
        pthread_setaffinity_np(worker->thread, sizeof(cpu_set_t), &cpuset);
    }
    
    return true;
}

void work_stealing_scheduler_stop(WorkStealingScheduler* scheduler) {
    if (!scheduler || !atomic_load(&scheduler->scheduler_active)) {
        return;
    }
    
    atomic_store(&scheduler->shutdown_requested, true);
    
    // Stop worker threads
    for (size_t i = 0; i < scheduler->worker_count; i++) {
        atomic_store(&scheduler->workers[i].active, false);
    }
    
    // Wait for worker threads to finish
    for (size_t i = 0; i < scheduler->worker_count; i++) {
        pthread_join(scheduler->workers[i].thread, NULL);
    }
    
    atomic_store(&scheduler->scheduler_active, false);
}

bool work_task_submit(WorkStealingScheduler* scheduler, WorkTask* task) {
    if (!scheduler || !task || !atomic_load(&scheduler->scheduler_active)) {
        return false;
    }
    
    // Try to submit to least loaded worker
    size_t best_worker = 0;
    uint32_t min_load = atomic_load(&scheduler->worker_loads[0]);
    
    for (size_t i = 1; i < scheduler->worker_count; i++) {
        uint32_t load = atomic_load(&scheduler->worker_loads[i]);
        if (load < min_load) {
            min_load = load;
            best_worker = i;
        }
    }
    
    // Try to add to worker's deque
    if (work_deque_push_bottom(scheduler->workers[best_worker].deque, task)) {
        atomic_fetch_add(&scheduler->worker_loads[best_worker], 1);
        return true;
    }
    
    // If worker deque is full, add to global queue
    pthread_mutex_lock(&scheduler->global_mutex);
    size_t tail = atomic_load(&scheduler->global_tail);
    size_t head = atomic_load(&scheduler->global_head);
    
    if (tail - head < scheduler->global_capacity) {
        scheduler->global_queue[tail % scheduler->global_capacity] = task;
        atomic_store(&scheduler->global_tail, tail + 1);
        pthread_cond_signal(&scheduler->global_not_empty);
        pthread_mutex_unlock(&scheduler->global_mutex);
        return true;
    }
    
    pthread_mutex_unlock(&scheduler->global_mutex);
    return false; // All queues full
}

bool work_task_wait(WorkTask* task, uint64_t timeout_ms) {
    if (!task) return false;
    
    uint64_t start_time = get_current_time_ns();
    uint64_t timeout_ns = timeout_ms * 1000000;
    
    while (!atomic_load(&task->completed)) {
        if (timeout_ms > 0) {
            uint64_t elapsed = get_current_time_ns() - start_time;
            if (elapsed >= timeout_ns) {
                return false; // Timeout
            }
        }
        
        usleep(1000); // Sleep for 1ms
    }
    
    return true;
}

// =============================================================================
// Lock-Free Data Structures (Simplified Implementations)
// =============================================================================

LockFreeStack* lockfree_stack_create(void) {
    LockFreeStack* stack = calloc(1, sizeof(LockFreeStack));
    if (!stack) return NULL;
    
    stack->head = NULL;
    atomic_init(&stack->push_count, 0);
    atomic_init(&stack->pop_count, 0);
    
    return stack;
}

void lockfree_stack_destroy(LockFreeStack* stack) {
    if (!stack) return;
    
    // Pop all remaining items
    while (lockfree_stack_pop(stack) != NULL) {
        // Continue popping
    }
    
    free(stack);
}

bool lockfree_stack_push(LockFreeStack* stack, void* data) {
    if (!stack) return false;
    
    struct StackNode* node = malloc(sizeof(struct StackNode));
    if (!node) return false;
    
    node->data = data;
    
    struct StackNode* old_head;
    do {
        old_head = stack->head;
        node->next = old_head;
    } while (!__sync_bool_compare_and_swap(&stack->head, old_head, node));
    
    atomic_fetch_add(&stack->push_count, 1);
    return true;
}

void* lockfree_stack_pop(LockFreeStack* stack) {
    if (!stack) return NULL;
    
    struct StackNode* old_head;
    struct StackNode* new_head;
    
    do {
        old_head = stack->head;
        if (!old_head) return NULL;
        new_head = old_head->next;
    } while (!__sync_bool_compare_and_swap(&stack->head, old_head, new_head));
    
    void* data = old_head->data;
    free(old_head);
    
    atomic_fetch_add(&stack->pop_count, 1);
    return data;
}

// =============================================================================
// NUMA Management (Simplified Implementation)
// =============================================================================

NUMAManager* numa_manager_create(void) {
    NUMAManager* manager = calloc(1, sizeof(NUMAManager));
    if (!manager) return NULL;
    
    // Check if NUMA is available
#if defined(NUMA_AVAILABLE) && NUMA_AVAILABLE == 1
    manager->numa_available = (numa_available() >= 0);
#else
    manager->numa_available = false;
#endif
    
    if (manager->numa_available) {
#if defined(NUMA_AVAILABLE) && NUMA_AVAILABLE == 1
        manager->node_count = numa_max_node() + 1;
#else
        manager->node_count = 1;
#endif
        manager->nodes = calloc(manager->node_count, sizeof(NUMANode));
        if (!manager->nodes) {
            free(manager);
            return NULL;
        }
        
        // Initialize NUMA nodes
        for (size_t i = 0; i < manager->node_count; i++) {
            NUMANode* node = &manager->nodes[i];
            node->node_id = i;
#if defined(NUMA_AVAILABLE) && NUMA_AVAILABLE == 1
            node->memory_size = numa_node_size64(i, &node->free_memory);
#else
            node->memory_size = 1024 * 1024 * 1024; // 1GB fallback
            node->free_memory = node->memory_size;
#endif
            node->cpu_count = 0; // Would be populated by CPU topology detection
            node->cpu_list = NULL;
            
            char arena_name[64];
            snprintf(arena_name, sizeof(arena_name), "numa_node_%u", (unsigned)i);
            node->memory_arena = goo_arena_new(1024 * 1024, arena_name);
            
            node->memory_utilization = 0.0;
            node->cpu_utilization = 0.0;
            node->allocation_count = 0;
            node->deallocation_count = 0;
        }
    } else {
        manager->node_count = 1;
        manager->nodes = calloc(1, sizeof(NUMANode));
        if (!manager->nodes) {
            free(manager);
            return NULL;
        }
        
        // Single node configuration
        manager->nodes[0].node_id = 0;
        manager->nodes[0].memory_arena = goo_arena_new(1024 * 1024, "numa_fallback");
    }
    
    manager->allocation_policy = NUMA_POLICY_LOCAL;
    manager->thread_policy = THREAD_POLICY_ROUND_ROBIN;
    manager->preferred_node = 0;
    
    return manager;
}

void numa_manager_destroy(NUMAManager* manager) {
    if (!manager) return;
    
    for (size_t i = 0; i < manager->node_count; i++) {
        NUMANode* node = &manager->nodes[i];
        goo_arena_free(node->memory_arena);
        free(node->cpu_list);
        free(node->threads);
        free(node->free_blocks);
    }
    
    free(manager->nodes);
    free(manager);
}

void* numa_alloc(NUMAManager* manager, size_t size, uint32_t preferred_node) {
    if (!manager || size == 0) return NULL;
    
    if (preferred_node >= manager->node_count) {
        preferred_node = 0;
    }
    
    NUMANode* node = &manager->nodes[preferred_node];
    void* ptr = goo_arena_alloc(node->memory_arena, size);
    
    if (ptr) {
        node->allocation_count++;
        manager->stats.local_allocations++;
    } else {
        // Fallback to system malloc
        ptr = malloc(size);
        if (ptr) {
            manager->stats.remote_allocations++;
        }
    }
    
    return ptr;
}

void numa_free(NUMAManager* manager, void* ptr, size_t size) {
    if (!manager || !ptr) return;
    
    // For simplicity, just use system free 
    // In a full implementation, we would track which arena owns the memory
    free(ptr);
    
    // Update statistics for the preferred node
    if (manager->node_count > 0) {
        manager->nodes[0].deallocation_count++;
    }
    
    (void)size; // Unused in this implementation
}

bool numa_bind_thread(NUMAManager* manager, pthread_t thread, uint32_t node) {
    if (!manager || node >= manager->node_count) return false;
    
    if (manager->numa_available) {
#if defined(NUMA_AVAILABLE) && NUMA_AVAILABLE == 1
        struct bitmask* nodemask = numa_allocate_nodemask();
        numa_bitmask_clearall(nodemask);
        numa_bitmask_setbit(nodemask, node);
        
        int result = numa_run_on_node_mask(nodemask);
        numa_free_nodemask(nodemask);
        
        if (result == 0) {
            manager->stats.thread_migrations++;
            return true;
        }
#endif
    }
    
    (void)thread; // Unused in this implementation
    return false;
}

// =============================================================================
// Performance Monitoring (Simplified Implementation)
// =============================================================================

PerformanceMonitor* performance_monitor_create(uint64_t sample_interval_ms) {
    PerformanceMonitor* monitor = calloc(1, sizeof(PerformanceMonitor));
    if (!monitor) return NULL;
    
    monitor->sample_interval_ms = sample_interval_ms > 0 ? sample_interval_ms : 100;
    monitor->enable_detailed_metrics = true;
    monitor->enable_profiling = false;
    
    monitor->history_capacity = 1000; // Keep 1000 samples
    monitor->history = calloc(monitor->history_capacity, sizeof(PerformanceMetrics));
    if (!monitor->history) {
        free(monitor);
        return NULL;
    }
    
    // Initialize thresholds
    monitor->triggers.cpu_threshold = 0.8;
    monitor->triggers.memory_threshold = 0.9;
    monitor->triggers.contention_threshold = 0.5;
    monitor->triggers.optimization_callback = NULL;
    
    // Initialize adaptive settings
    monitor->adaptive.enable_auto_scaling = true;
    monitor->adaptive.enable_load_balancing = true;
    monitor->adaptive.enable_memory_tuning = false;
    monitor->adaptive.adaptation_interval_ms = 1000;
    
    atomic_init(&monitor->monitor_active, false);
    
    return monitor;
}

void performance_monitor_destroy(PerformanceMonitor* monitor) {
    if (!monitor) return;
    
    performance_monitor_stop(monitor);
    free(monitor->history);
    free(monitor);
}

// Performance monitoring thread (simplified)
static void* performance_monitor_thread(void* arg) {
    PerformanceMonitor* monitor = (PerformanceMonitor*)arg;
    
    while (atomic_load(&monitor->monitor_active)) {
        // Collect current metrics (simplified)
        monitor->current.cpu_utilization = 0.5; // Placeholder
        monitor->current.memory_usage = 1024 * 1024; // Placeholder
        monitor->current.active_threads = 4; // Placeholder
        
        // Store in history
        if (monitor->history_size < monitor->history_capacity) {
            monitor->history[monitor->history_size] = monitor->current;
            monitor->history_size++;
        } else {
            // Circular buffer - overwrite oldest
            size_t index = monitor->history_size % monitor->history_capacity;
            monitor->history[index] = monitor->current;
            monitor->history_size++;
        }
        
        // Check optimization triggers
        if (monitor->triggers.optimization_callback) {
            if (monitor->current.cpu_utilization > monitor->triggers.cpu_threshold ||
                monitor->current.memory_fragmentation > monitor->triggers.memory_threshold) {
                monitor->triggers.optimization_callback(monitor);
            }
        }
        
        // Sleep for sample interval
        struct timespec sleep_time;
        sleep_time.tv_sec = monitor->sample_interval_ms / 1000;
        sleep_time.tv_nsec = (monitor->sample_interval_ms % 1000) * 1000000;
        nanosleep(&sleep_time, NULL);
    }
    
    return NULL;
}

bool performance_monitor_start(PerformanceMonitor* monitor) {
    if (!monitor || atomic_load(&monitor->monitor_active)) {
        return false;
    }
    
    atomic_store(&monitor->monitor_active, true);
    
    if (pthread_create(&monitor->monitor_thread, NULL, 
                      performance_monitor_thread, monitor) != 0) {
        atomic_store(&monitor->monitor_active, false);
        return false;
    }
    
    return true;
}

void performance_monitor_stop(PerformanceMonitor* monitor) {
    if (!monitor || !atomic_load(&monitor->monitor_active)) {
        return;
    }
    
    atomic_store(&monitor->monitor_active, false);
    pthread_join(monitor->monitor_thread, NULL);
}

PerformanceMetrics performance_monitor_get_current(PerformanceMonitor* monitor) {
    PerformanceMetrics empty = {0};
    if (!monitor) return empty;
    
    return monitor->current;
}

// =============================================================================
// Global System Management
// =============================================================================

bool deadlock_prevention_init(const DeadlockPreventionConfig* config) {
    pthread_mutex_lock(&g_global_mutex);
    
    if (g_global_detector || g_global_scheduler || g_global_numa_manager || g_global_monitor) {
        pthread_mutex_unlock(&g_global_mutex);
        return false; // Already initialized
    }
    
    // Create deadlock detector
    LockOrderPolicy policy = config ? config->lock_order_policy : LOCK_ORDER_RESOURCE_ID;
    g_global_detector = deadlock_detector_create(policy);
    if (!g_global_detector) {
        pthread_mutex_unlock(&g_global_mutex);
        return false;
    }
    
    // Create work-stealing scheduler
    size_t worker_count = config ? config->worker_thread_count : 0;
    g_global_scheduler = work_stealing_scheduler_create(worker_count);
    if (!g_global_scheduler) {
        deadlock_detector_destroy(g_global_detector);
        g_global_detector = NULL;
        pthread_mutex_unlock(&g_global_mutex);
        return false;
    }
    
    // Create NUMA manager
    if (!config || config->enable_numa_awareness) {
        g_global_numa_manager = numa_manager_create();
        // Continue even if NUMA manager creation fails
    }
    
    // Create performance monitor
    if (!config || config->enable_performance_monitoring) {
        uint64_t interval = config ? config->monitor_sample_interval_ms : 100;
        g_global_monitor = performance_monitor_create(interval);
        // Continue even if monitor creation fails
    }
    
    // Start systems
    if (config && config->enable_runtime_detection) {
        deadlock_detector_start(g_global_detector);
    }
    
    if (config && config->enable_work_stealing) {
        work_stealing_scheduler_start(g_global_scheduler);
    }
    
    if (g_global_monitor && (!config || config->enable_performance_monitoring)) {
        performance_monitor_start(g_global_monitor);
    }
    
    pthread_mutex_unlock(&g_global_mutex);
    return true;
}

void deadlock_prevention_shutdown(void) {
    pthread_mutex_lock(&g_global_mutex);
    
    if (g_global_monitor) {
        performance_monitor_destroy(g_global_monitor);
        g_global_monitor = NULL;
    }
    
    if (g_global_numa_manager) {
        numa_manager_destroy(g_global_numa_manager);
        g_global_numa_manager = NULL;
    }
    
    if (g_global_scheduler) {
        work_stealing_scheduler_destroy(g_global_scheduler);
        g_global_scheduler = NULL;
    }
    
    if (g_global_detector) {
        deadlock_detector_destroy(g_global_detector);
        g_global_detector = NULL;
    }
    
    pthread_mutex_unlock(&g_global_mutex);
}

DeadlockDetector* get_global_deadlock_detector(void) {
    return g_global_detector;
}

WorkStealingScheduler* get_global_scheduler(void) {
    return g_global_scheduler;
}

NUMAManager* get_global_numa_manager(void) {
    return g_global_numa_manager;
}

PerformanceMonitor* get_global_performance_monitor(void) {
    return g_global_monitor;
}

// =============================================================================
// Debugging and Diagnostics
// =============================================================================

void deadlock_detector_dump_graph(DeadlockDetector* detector) {
    if (!detector) return;
    
    printf("=== Deadlock Detector Graph ===\n");
    printf("Resources: %zu\n", detector->resource_graph->node_count);
    printf("Cycle detections: %lu\n", detector->resource_graph->cycle_detection_count);
    printf("Deadlocks prevented: %lu\n", detector->resource_graph->deadlocks_prevented);
    printf("Avg detection time: %.2f ns\n", detector->resource_graph->avg_detection_time_ns);
    
    // TODO: Dump detailed graph structure
}

void work_stealing_scheduler_dump_stats(WorkStealingScheduler* scheduler) {
    if (!scheduler) return;
    
    printf("=== Work-Stealing Scheduler Stats ===\n");
    printf("Workers: %zu\n", scheduler->worker_count);
    printf("Active: %s\n", atomic_load(&scheduler->scheduler_active) ? "Yes" : "No");
    
    for (size_t i = 0; i < scheduler->worker_count; i++) {
        WorkerThread* worker = &scheduler->workers[i];
        printf("Worker %zu:\n", i);
        printf("  Tasks executed: %lu\n", atomic_load(&worker->stats.tasks_executed));
        printf("  Tasks stolen: %lu\n", atomic_load(&worker->stats.tasks_stolen));
        printf("  Steal attempts: %lu\n", atomic_load(&worker->stats.steal_attempts));
    }
}

void numa_manager_dump_topology(NUMAManager* manager) {
    if (!manager) return;
    
    printf("=== NUMA Topology ===\n");
    printf("NUMA available: %s\n", manager->numa_available ? "Yes" : "No");
    printf("Nodes: %zu\n", manager->node_count);
    
    for (size_t i = 0; i < manager->node_count; i++) {
        NUMANode* node = &manager->nodes[i];
        printf("Node %u:\n", node->node_id);
        printf("  Memory: %zu bytes (free: %zu)\n", node->memory_size, node->free_memory);
        printf("  CPUs: %u\n", node->cpu_count);
        printf("  Allocations: %lu\n", node->allocation_count);
    }
}

void performance_monitor_dump_metrics(PerformanceMonitor* monitor) {
    if (!monitor) return;
    
    printf("=== Performance Metrics ===\n");
    printf("CPU utilization: %.1f%%\n", monitor->current.cpu_utilization * 100.0);
    printf("Memory usage: %zu bytes\n", monitor->current.memory_usage);
    printf("Active threads: %u\n", monitor->current.active_threads);
    printf("Active locks: %u\n", monitor->current.active_locks);
    printf("Lock contention: %.1f%%\n", monitor->current.lock_contention_ratio * 100.0);
    printf("Messages/sec: %lu\n", monitor->current.messages_per_second);
    printf("Avg message latency: %.2f ns\n", monitor->current.avg_message_latency_ns);
}

// =============================================================================
// Stub implementations for missing functions
// =============================================================================

bool resource_register(DeadlockDetector* detector, ResourceDescriptor* resource) {
    (void)detector; (void)resource;
    return true; // Stub
}

bool resource_unregister(DeadlockDetector* detector, uint64_t resource_id) {
    (void)detector; (void)resource_id;
    return true; // Stub
}

bool deadlock_detector_register_scope(DeadlockDetector* detector, ConcurrencyScope* scope) {
    (void)detector; (void)scope;
    return true; // Stub
}

bool deadlock_detector_unregister_scope(DeadlockDetector* detector, ConcurrencyScope* scope) {
    (void)detector; (void)scope;
    return true; // Stub
}

bool deadlock_detector_register_channel(DeadlockDetector* detector, AdvancedChannel* channel) {
    (void)detector; (void)channel;
    return true; // Stub
}

bool deadlock_detector_unregister_channel(DeadlockDetector* detector, AdvancedChannel* channel) {
    (void)detector; (void)channel;
    return true; // Stub
}

bool deadlock_detector_register_actor(DeadlockDetector* detector, ActorRef* actor) {
    (void)detector; (void)actor;
    return true; // Stub
}

bool deadlock_detector_unregister_actor(DeadlockDetector* detector, ActorRef* actor) {
    (void)detector; (void)actor;
    return true; // Stub
}

// More lock-free data structure stubs
LockFreeQueue* lockfree_queue_create(void) {
    return NULL; // Stub
}

void lockfree_queue_destroy(LockFreeQueue* queue) {
    (void)queue; // Stub
}

bool lockfree_queue_enqueue(LockFreeQueue* queue, void* data) {
    (void)queue; (void)data;
    return false; // Stub
}

void* lockfree_queue_dequeue(LockFreeQueue* queue) {
    (void)queue;
    return NULL; // Stub
}

LockFreeHashTable* lockfree_hashtable_create(size_t bucket_count,
                                             uint32_t (*hash_func)(const void*, size_t),
                                             bool (*key_cmp)(const void*, const void*, size_t)) {
    (void)bucket_count; (void)hash_func; (void)key_cmp;
    return NULL; // Stub
}

void lockfree_hashtable_destroy(LockFreeHashTable* table) {
    (void)table; // Stub
}

bool lockfree_hashtable_insert(LockFreeHashTable* table, const void* key, size_t key_size, void* value) {
    (void)table; (void)key; (void)key_size; (void)value;
    return false; // Stub
}

void* lockfree_hashtable_lookup(LockFreeHashTable* table, const void* key, size_t key_size) {
    (void)table; (void)key; (void)key_size;
    return NULL; // Stub
}

bool lockfree_hashtable_delete(LockFreeHashTable* table, const void* key, size_t key_size) {
    (void)table; (void)key; (void)key_size;
    return false; // Stub
}