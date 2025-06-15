#ifndef WORK_STEALING_H
#define WORK_STEALING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include "structured_concurrency.h"
#include "dynamic_chunking.h"

// Work-stealing deque structure for each worker
typedef struct WorkStealingDeque {
    ConcurrentTask** tasks;
    atomic_int_fast64_t top;    // Modified by owner only
    atomic_int_fast64_t bottom; // Modified by owner, read by stealers
    size_t capacity;
    size_t mask;                // For fast modulo operations (capacity - 1)
} WorkStealingDeque;

// Per-worker context for work stealing
typedef struct WorkerContext {
    size_t worker_id;
    TaskScope* scope;
    WorkStealingDeque* local_deque;
    
    // Performance statistics
    atomic_uint_fast64_t tasks_executed;
    atomic_uint_fast64_t tasks_stolen;
    atomic_uint_fast64_t steal_attempts;
    atomic_uint_fast64_t failed_steals;
    
    // CPU affinity
    int preferred_cpu;
    int numa_node;
    
    // Random state for stealing
    uint32_t random_state;
} WorkerContext;

// Work-stealing task scope extension
typedef struct WorkStealingScope {
    TaskScope base_scope;
    
    // Worker contexts
    WorkerContext* worker_contexts;
    size_t worker_count;
    
    // Global task queue for overflow
    ConcurrentTask** overflow_queue;
    atomic_size_t overflow_head;
    atomic_size_t overflow_tail;
    size_t overflow_capacity;
    pthread_mutex_t overflow_mutex;
    
    // Load balancing configuration
    size_t steal_attempts_before_overflow;
    size_t min_tasks_before_stealing;
    bool enable_work_stealing;
    
    // Performance monitoring
    atomic_uint_fast64_t total_steals;
    atomic_uint_fast64_t total_overflow_ops;
    
    // Dynamic chunking integration
    DynamicChunkingContext* chunking_context;
    bool enable_dynamic_chunking;
} WorkStealingScope;

// Work-stealing operations
WorkStealingScope* work_stealing_scope_create(size_t worker_count, const char* name);
WorkStealingScope* work_stealing_scope_create_with_chunking(size_t worker_count, const char* name, 
                                                           DynamicChunkingConfig chunking_config);
void work_stealing_scope_destroy(WorkStealingScope* ws_scope);

// Deque operations (owner thread)
bool work_stealing_push_bottom(WorkStealingDeque* deque, ConcurrentTask* task);
ConcurrentTask* work_stealing_pop_bottom(WorkStealingDeque* deque);

// Stealing operations (thief threads)
ConcurrentTask* work_stealing_steal_top(WorkStealingDeque* deque);

// Worker thread functions
void* work_stealing_worker_thread(void* arg);
ConcurrentTask* work_stealing_find_task(WorkerContext* worker);

// Enhanced parallel for with work stealing
typedef struct WorkStealingParallelForConfig {
    ParallelForConfig base_config;
    
    // Work-stealing specific options
    size_t min_steal_size;      // Minimum items to steal
    size_t initial_chunk_size;  // Initial chunk size per worker
    bool adaptive_chunking;     // Enable dynamic chunk sizing
    bool locality_aware;        // Consider data locality
} WorkStealingParallelForConfig;

Result_void_ptr work_stealing_parallel_for(
    WorkStealingScope* scope,
    WorkStealingParallelForConfig config,
    ParallelForFunction function,
    void* context
);

// Utility functions
static inline uint32_t xorshift32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static inline size_t next_power_of_2(size_t n) {
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    n++;
    return n;
}

#endif // WORK_STEALING_H