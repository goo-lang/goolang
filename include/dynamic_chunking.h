#ifndef DYNAMIC_CHUNKING_H
#define DYNAMIC_CHUNKING_H

#include <stdint.h>
#include "ccomp_shim.h"
#include <stdbool.h>
#include "structured_concurrency.h"

// Workload characteristics for adaptive chunking
typedef struct WorkloadProfile {
    // Timing statistics
    atomic_uint_fast64_t total_execution_time_ns;
    atomic_uint_fast64_t total_tasks_completed;
    atomic_uint_fast64_t min_task_time_ns;
    atomic_uint_fast64_t max_task_time_ns;
    
    // Variance and distribution
    double execution_time_variance;
    double coefficient_of_variation;  // stddev / mean
    
    // Cache and memory characteristics
    atomic_size_t cache_misses;
    atomic_size_t memory_accesses;
    size_t estimated_working_set_size;
    
    // Load balancing metrics
    atomic_uint_fast32_t steal_attempts;
    atomic_uint_fast32_t successful_steals;
    atomic_uint_fast32_t idle_worker_cycles;
    
    // Adaptive parameters
    size_t optimal_chunk_size;
    size_t min_chunk_size;
    size_t max_chunk_size;
    double load_balance_factor;  // 0.0 = perfectly balanced, 1.0 = completely imbalanced
} WorkloadProfile;

// Chunking strategy types
typedef enum {
    CHUNK_STRATEGY_FIXED,           // Fixed size chunks
    CHUNK_STRATEGY_ADAPTIVE,        // Adapt based on execution time variance
    CHUNK_STRATEGY_CACHE_AWARE,     // Optimize for cache locality
    CHUNK_STRATEGY_LOAD_BALANCED,   // Minimize worker idle time
    CHUNK_STRATEGY_HYBRID           // Combination of multiple strategies
} ChunkingStrategy;

// Dynamic chunking configuration
typedef struct DynamicChunkingConfig {
    ChunkingStrategy strategy;
    
    // Adaptation parameters
    double adaptation_rate;           // How quickly to adapt (0.0-1.0)
    size_t adaptation_window;         // Number of tasks to observe before adapting
    size_t min_chunk_size;
    size_t max_chunk_size;
    
    // Cache awareness
    size_t l1_cache_size;
    size_t l2_cache_size;
    size_t cache_line_size;
    size_t estimated_item_size;
    
    // Load balancing thresholds
    double max_load_imbalance;        // Maximum acceptable load imbalance
    double steal_threshold;           // When to trigger more aggressive chunking
    
    // Performance targets
    uint64_t target_task_duration_ns; // Optimal task duration
    double max_overhead_ratio;        // Maximum scheduling overhead
} DynamicChunkingConfig;

// Dynamic chunking context for runtime adaptation
typedef struct DynamicChunkingContext {
    DynamicChunkingConfig config;
    WorkloadProfile profile;
    
    // Current state
    size_t current_chunk_size;
    size_t tasks_since_adaptation;
    uint64_t last_adaptation_time;
    
    // History for trend analysis
    size_t* chunk_size_history;
    double* performance_history;
    size_t history_size;
    size_t history_index;
    
    // Worker-specific metrics
    atomic_uint_fast64_t* worker_execution_times;
    atomic_uint_fast32_t* worker_task_counts;
    size_t worker_count;
    
    // Synchronization
    pthread_mutex_t adaptation_mutex;
} DynamicChunkingContext;

// Function prototypes
DynamicChunkingContext* dynamic_chunking_create(DynamicChunkingConfig config, size_t worker_count);
void dynamic_chunking_destroy(DynamicChunkingContext* ctx);

// Chunk size calculation
size_t dynamic_chunking_calculate_size(DynamicChunkingContext* ctx, size_t remaining_items);
void dynamic_chunking_update_metrics(DynamicChunkingContext* ctx, size_t worker_id, 
                                    uint64_t execution_time_ns, size_t chunk_size);

// Strategy implementations
size_t adaptive_chunk_size(const WorkloadProfile* profile, size_t remaining_items, 
                          const DynamicChunkingConfig* config);
size_t cache_aware_chunk_size(const WorkloadProfile* profile, size_t remaining_items,
                             const DynamicChunkingConfig* config);
size_t load_balanced_chunk_size(const WorkloadProfile* profile, size_t remaining_items,
                               const DynamicChunkingConfig* config);

// Workload analysis
void analyze_workload_variance(WorkloadProfile* profile);
double calculate_load_imbalance(const atomic_uint_fast64_t* worker_times, size_t worker_count);
bool should_adapt_chunk_size(const DynamicChunkingContext* ctx);

// Cache-aware calculations
size_t calculate_cache_optimal_chunk_size(size_t item_size, size_t cache_size, size_t cache_line_size);
size_t estimate_working_set_size(const WorkloadProfile* profile, size_t chunk_size);

// Performance prediction
double predict_execution_time(const DynamicChunkingContext* ctx, size_t chunk_size);
double calculate_scheduling_overhead(size_t total_items, size_t chunk_size, size_t worker_count);

// Default configurations
DynamicChunkingConfig dynamic_chunking_config_cpu_bound(void);
DynamicChunkingConfig dynamic_chunking_config_memory_bound(void);
DynamicChunkingConfig dynamic_chunking_config_mixed_workload(void);

// Utility functions
static inline uint64_t get_cpu_frequency_hz(void) {
    // Platform-specific implementation would go here
    return 2400000000ULL; // 2.4 GHz default estimate
}

static inline size_t get_cache_line_size(void) {
    return 64; // Common cache line size
}

static inline size_t get_l1_cache_size(void) {
    return 32 * 1024; // 32KB typical L1 cache
}

static inline size_t get_l2_cache_size(void) {
    return 256 * 1024; // 256KB typical L2 cache
}

#endif // DYNAMIC_CHUNKING_H