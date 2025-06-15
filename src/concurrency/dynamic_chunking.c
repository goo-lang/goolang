#include "../../include/dynamic_chunking.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>

// Create dynamic chunking context
DynamicChunkingContext* dynamic_chunking_create(DynamicChunkingConfig config, size_t worker_count) {
    DynamicChunkingContext* ctx = calloc(1, sizeof(DynamicChunkingContext));
    if (!ctx) return NULL;
    
    ctx->config = config;
    ctx->worker_count = worker_count;
    ctx->current_chunk_size = config.min_chunk_size;
    ctx->history_size = 100; // Keep last 100 adaptations
    
    // Initialize workload profile
    atomic_init(&ctx->profile.total_execution_time_ns, 0);
    atomic_init(&ctx->profile.total_tasks_completed, 0);
    atomic_init(&ctx->profile.min_task_time_ns, UINT64_MAX);
    atomic_init(&ctx->profile.max_task_time_ns, 0);
    atomic_init(&ctx->profile.cache_misses, 0);
    atomic_init(&ctx->profile.memory_accesses, 0);
    atomic_init(&ctx->profile.steal_attempts, 0);
    atomic_init(&ctx->profile.successful_steals, 0);
    atomic_init(&ctx->profile.idle_worker_cycles, 0);
    
    ctx->profile.optimal_chunk_size = config.min_chunk_size;
    ctx->profile.min_chunk_size = config.min_chunk_size;
    ctx->profile.max_chunk_size = config.max_chunk_size;
    
    // Allocate history arrays
    ctx->chunk_size_history = calloc(ctx->history_size, sizeof(size_t));
    ctx->performance_history = calloc(ctx->history_size, sizeof(double));
    if (!ctx->chunk_size_history || !ctx->performance_history) {
        free(ctx->chunk_size_history);
        free(ctx->performance_history);
        free(ctx);
        return NULL;
    }
    
    // Allocate worker-specific metrics
    ctx->worker_execution_times = calloc(worker_count, sizeof(atomic_uint_fast64_t));
    ctx->worker_task_counts = calloc(worker_count, sizeof(atomic_uint_fast32_t));
    if (!ctx->worker_execution_times || !ctx->worker_task_counts) {
        free(ctx->chunk_size_history);
        free(ctx->performance_history);
        free(ctx->worker_execution_times);
        free(ctx->worker_task_counts);
        free(ctx);
        return NULL;
    }
    
    // Initialize worker metrics
    for (size_t i = 0; i < worker_count; i++) {
        atomic_init(&ctx->worker_execution_times[i], 0);
        atomic_init(&ctx->worker_task_counts[i], 0);
    }
    
    // Initialize mutex
    if (pthread_mutex_init(&ctx->adaptation_mutex, NULL) != 0) {
        free(ctx->chunk_size_history);
        free(ctx->performance_history);
        free(ctx->worker_execution_times);
        free(ctx->worker_task_counts);
        free(ctx);
        return NULL;
    }
    
    return ctx;
}

void dynamic_chunking_destroy(DynamicChunkingContext* ctx) {
    if (!ctx) return;
    
    free(ctx->chunk_size_history);
    free(ctx->performance_history);
    free(ctx->worker_execution_times);
    free(ctx->worker_task_counts);
    pthread_mutex_destroy(&ctx->adaptation_mutex);
    free(ctx);
}

// Update metrics with new task execution data
void dynamic_chunking_update_metrics(DynamicChunkingContext* ctx, size_t worker_id, 
                                    uint64_t execution_time_ns, size_t chunk_size) {
    if (!ctx || worker_id >= ctx->worker_count) return;
    
    // Update worker-specific metrics
    atomic_fetch_add(&ctx->worker_execution_times[worker_id], execution_time_ns);
    atomic_fetch_add(&ctx->worker_task_counts[worker_id], 1);
    
    // Update global profile
    atomic_fetch_add(&ctx->profile.total_execution_time_ns, execution_time_ns);
    atomic_fetch_add(&ctx->profile.total_tasks_completed, 1);
    
    // Update min/max times
    uint64_t current_min = atomic_load(&ctx->profile.min_task_time_ns);
    while (execution_time_ns < current_min) {
        if (atomic_compare_exchange_weak(&ctx->profile.min_task_time_ns, &current_min, execution_time_ns)) {
            break;
        }
    }
    
    uint64_t current_max = atomic_load(&ctx->profile.max_task_time_ns);
    while (execution_time_ns > current_max) {
        if (atomic_compare_exchange_weak(&ctx->profile.max_task_time_ns, &current_max, execution_time_ns)) {
            break;
        }
    }
    
    ctx->tasks_since_adaptation++;
    
    // Check if adaptation is needed
    if (should_adapt_chunk_size(ctx)) {
        pthread_mutex_lock(&ctx->adaptation_mutex);
        
        // Re-check under lock to avoid race conditions
        if (should_adapt_chunk_size(ctx)) {
            analyze_workload_variance(&ctx->profile);
            
            size_t new_chunk_size = dynamic_chunking_calculate_size(ctx, SIZE_MAX);
            
            // Record adaptation in history
            ctx->chunk_size_history[ctx->history_index] = ctx->current_chunk_size;
            ctx->performance_history[ctx->history_index] = 
                (double)atomic_load(&ctx->profile.total_execution_time_ns) / 
                atomic_load(&ctx->profile.total_tasks_completed);
            
            ctx->history_index = (ctx->history_index + 1) % ctx->history_size;
            ctx->current_chunk_size = new_chunk_size;
            ctx->tasks_since_adaptation = 0;
            
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ctx->last_adaptation_time = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        }
        
        pthread_mutex_unlock(&ctx->adaptation_mutex);
    }
}

// Calculate optimal chunk size based on current strategy
size_t dynamic_chunking_calculate_size(DynamicChunkingContext* ctx, size_t remaining_items) {
    if (!ctx) return 1;
    
    switch (ctx->config.strategy) {
        case CHUNK_STRATEGY_FIXED:
            return ctx->current_chunk_size;
            
        case CHUNK_STRATEGY_ADAPTIVE:
            return adaptive_chunk_size(&ctx->profile, remaining_items, &ctx->config);
            
        case CHUNK_STRATEGY_CACHE_AWARE:
            return cache_aware_chunk_size(&ctx->profile, remaining_items, &ctx->config);
            
        case CHUNK_STRATEGY_LOAD_BALANCED:
            return load_balanced_chunk_size(&ctx->profile, remaining_items, &ctx->config);
            
        case CHUNK_STRATEGY_HYBRID:
            // Combine multiple strategies
            size_t adaptive_size = adaptive_chunk_size(&ctx->profile, remaining_items, &ctx->config);
            size_t cache_size = cache_aware_chunk_size(&ctx->profile, remaining_items, &ctx->config);
            size_t balanced_size = load_balanced_chunk_size(&ctx->profile, remaining_items, &ctx->config);
            
            // Weighted average based on workload characteristics
            double cv = ctx->profile.coefficient_of_variation;
            double imbalance = ctx->profile.load_balance_factor;
            
            size_t result = (size_t)(
                adaptive_size * (cv > 0.5 ? 0.5 : 0.2) +
                cache_size * 0.3 +
                balanced_size * (imbalance > 0.3 ? 0.5 : 0.2)
            );
            
            return result < ctx->config.min_chunk_size ? ctx->config.min_chunk_size :
                   result > ctx->config.max_chunk_size ? ctx->config.max_chunk_size : result;
            
        default:
            return ctx->current_chunk_size;
    }
}

// Adaptive chunk sizing based on execution time variance
size_t adaptive_chunk_size(const WorkloadProfile* profile, size_t remaining_items, 
                          const DynamicChunkingConfig* config) {
    uint64_t total_tasks = atomic_load(&profile->total_tasks_completed);
    if (total_tasks == 0) {
        return config->min_chunk_size;
    }
    
    uint64_t avg_time_ns = atomic_load(&profile->total_execution_time_ns) / total_tasks;
    
    // If tasks are highly variable, use smaller chunks
    double cv = profile->coefficient_of_variation;
    double variance_factor = 1.0 - (cv > 1.0 ? 1.0 : cv);
    
    // Calculate target chunk size based on desired task duration
    size_t target_chunk_size = (size_t)(config->target_task_duration_ns / (double)avg_time_ns);
    
    // Apply variance adjustment
    target_chunk_size = (size_t)(target_chunk_size * variance_factor);
    
    // Consider remaining items
    if (remaining_items != SIZE_MAX && target_chunk_size > remaining_items / 4) {
        target_chunk_size = remaining_items / 4;
    }
    
    // Clamp to bounds
    if (target_chunk_size < config->min_chunk_size) target_chunk_size = config->min_chunk_size;
    if (target_chunk_size > config->max_chunk_size) target_chunk_size = config->max_chunk_size;
    
    return target_chunk_size;
}

// Cache-aware chunk sizing
size_t cache_aware_chunk_size(const WorkloadProfile* profile, size_t remaining_items,
                             const DynamicChunkingConfig* config) {
    (void)profile; // Unused for now
    
    // Calculate chunk size that fits in L1 cache
    size_t l1_optimal = calculate_cache_optimal_chunk_size(
        config->estimated_item_size, config->l1_cache_size, config->cache_line_size);
    
    // Calculate chunk size that fits in L2 cache
    size_t l2_optimal = calculate_cache_optimal_chunk_size(
        config->estimated_item_size, config->l2_cache_size, config->cache_line_size);
    
    // Prefer L1 cache size for better performance
    size_t cache_optimal = l1_optimal;
    
    // If chunk is too small compared to scheduling overhead, use L2 size
    if (cache_optimal < config->min_chunk_size) {
        cache_optimal = l2_optimal < config->min_chunk_size ? config->min_chunk_size : l2_optimal;
    }
    
    // Consider remaining items
    if (remaining_items != SIZE_MAX && cache_optimal > remaining_items / 2) {
        cache_optimal = remaining_items / 2;
        if (cache_optimal < config->min_chunk_size) {
            cache_optimal = remaining_items;
        }
    }
    
    // Clamp to bounds
    if (cache_optimal > config->max_chunk_size) cache_optimal = config->max_chunk_size;
    
    return cache_optimal;
}

// Load-balanced chunk sizing
size_t load_balanced_chunk_size(const WorkloadProfile* profile, size_t remaining_items,
                               const DynamicChunkingConfig* config) {
    // If load imbalance is high, use smaller chunks for better distribution
    double imbalance = profile->load_balance_factor;
    
    // Base chunk size on remaining work and desired granularity
    size_t base_chunk_size = config->min_chunk_size;
    
    if (remaining_items != SIZE_MAX) {
        // Calculate chunk size for roughly 4x more chunks than workers
        base_chunk_size = remaining_items / (config->max_chunk_size > 0 ? 
                                           (remaining_items / config->max_chunk_size) * 4 : 16);
    }
    
    // Adjust based on load imbalance
    if (imbalance > config->max_load_imbalance) {
        // High imbalance - use smaller chunks
        base_chunk_size = (size_t)(base_chunk_size * (1.0 - imbalance * 0.5));
    }
    
    // Adjust based on steal success rate
    uint32_t steals = atomic_load(&profile->successful_steals);
    uint32_t attempts = atomic_load(&profile->steal_attempts);
    
    if (attempts > 0) {
        double steal_rate = (double)steals / attempts;
        if (steal_rate < config->steal_threshold) {
            // Low steal success - make chunks smaller for better redistribution
            base_chunk_size = (size_t)(base_chunk_size * 0.7);
        }
    }
    
    // Clamp to bounds
    if (base_chunk_size < config->min_chunk_size) base_chunk_size = config->min_chunk_size;
    if (base_chunk_size > config->max_chunk_size) base_chunk_size = config->max_chunk_size;
    
    return base_chunk_size;
}

// Analyze workload variance for adaptation decisions
void analyze_workload_variance(WorkloadProfile* profile) {
    uint64_t total_tasks = atomic_load(&profile->total_tasks_completed);
    if (total_tasks < 10) return; // Need sufficient data
    
    uint64_t total_time = atomic_load(&profile->total_execution_time_ns);
    uint64_t min_time = atomic_load(&profile->min_task_time_ns);
    uint64_t max_time = atomic_load(&profile->max_task_time_ns);
    
    double mean_time = (double)total_time / total_tasks;
    
    // Simple variance estimation using min/max range
    double range = (double)(max_time - min_time);
    double estimated_stddev = range / 4.0; // Approximate standard deviation
    
    profile->execution_time_variance = estimated_stddev * estimated_stddev;
    profile->coefficient_of_variation = mean_time > 0 ? estimated_stddev / mean_time : 0.0;
}

// Calculate load imbalance across workers
double calculate_load_imbalance(const atomic_uint_fast64_t* worker_times, size_t worker_count) {
    if (!worker_times || worker_count == 0) return 0.0;
    
    uint64_t total_time = 0;
    uint64_t min_time = UINT64_MAX;
    uint64_t max_time = 0;
    
    for (size_t i = 0; i < worker_count; i++) {
        uint64_t time = atomic_load(&worker_times[i]);
        total_time += time;
        if (time < min_time) min_time = time;
        if (time > max_time) max_time = time;
    }
    
    if (total_time == 0) return 0.0;
    
    double avg_time = (double)total_time / worker_count;
    return avg_time > 0 ? (max_time - min_time) / avg_time : 0.0;
}

// Check if chunk size adaptation should occur
bool should_adapt_chunk_size(const DynamicChunkingContext* ctx) {
    return ctx->tasks_since_adaptation >= ctx->config.adaptation_window;
}

// Cache-optimal chunk size calculation
size_t calculate_cache_optimal_chunk_size(size_t item_size, size_t cache_size, size_t cache_line_size) {
    if (item_size == 0) item_size = sizeof(void*); // Default pointer size
    
    // Items per cache line
    size_t items_per_line = cache_line_size / item_size;
    if (items_per_line == 0) items_per_line = 1;
    
    // Total items that fit in cache
    size_t total_items = cache_size / item_size;
    
    // Use 75% of cache to account for other data and avoid eviction
    return (total_items * 3) / 4;
}

// Estimate working set size
size_t estimate_working_set_size(const WorkloadProfile* profile, size_t chunk_size) {
    (void)profile; // Unused for simple estimation
    
    // Simple heuristic: assume each item needs at least one cache line
    return chunk_size * get_cache_line_size();
}

// Default configurations
DynamicChunkingConfig dynamic_chunking_config_cpu_bound(void) {
    return (DynamicChunkingConfig) {
        .strategy = CHUNK_STRATEGY_ADAPTIVE,
        .adaptation_rate = 0.3,
        .adaptation_window = 50,
        .min_chunk_size = 10,
        .max_chunk_size = 1000,
        .l1_cache_size = get_l1_cache_size(),
        .l2_cache_size = get_l2_cache_size(),
        .cache_line_size = get_cache_line_size(),
        .estimated_item_size = sizeof(void*),
        .max_load_imbalance = 0.2,
        .steal_threshold = 0.3,
        .target_task_duration_ns = 100000, // 100 microseconds
        .max_overhead_ratio = 0.1
    };
}

DynamicChunkingConfig dynamic_chunking_config_memory_bound(void) {
    return (DynamicChunkingConfig) {
        .strategy = CHUNK_STRATEGY_CACHE_AWARE,
        .adaptation_rate = 0.2,
        .adaptation_window = 30,
        .min_chunk_size = 32,
        .max_chunk_size = 2048,
        .l1_cache_size = get_l1_cache_size(),
        .l2_cache_size = get_l2_cache_size(),
        .cache_line_size = get_cache_line_size(),
        .estimated_item_size = 64, // Larger items for memory-bound
        .max_load_imbalance = 0.15,
        .steal_threshold = 0.4,
        .target_task_duration_ns = 500000, // 500 microseconds
        .max_overhead_ratio = 0.05
    };
}

DynamicChunkingConfig dynamic_chunking_config_mixed_workload(void) {
    return (DynamicChunkingConfig) {
        .strategy = CHUNK_STRATEGY_HYBRID,
        .adaptation_rate = 0.25,
        .adaptation_window = 40,
        .min_chunk_size = 16,
        .max_chunk_size = 1024,
        .l1_cache_size = get_l1_cache_size(),
        .l2_cache_size = get_l2_cache_size(),
        .cache_line_size = get_cache_line_size(),
        .estimated_item_size = sizeof(void*) * 2,
        .max_load_imbalance = 0.25,
        .steal_threshold = 0.35,
        .target_task_duration_ns = 200000, // 200 microseconds
        .max_overhead_ratio = 0.08
    };
}