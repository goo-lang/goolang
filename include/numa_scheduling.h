#ifndef NUMA_SCHEDULING_H
#define NUMA_SCHEDULING_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "structured_concurrency.h"
#include "work_stealing.h"

// NUMA topology information
typedef struct NumaNode {
    uint32_t node_id;
    uint32_t cpu_count;
    uint32_t* cpu_ids;              // Array of CPU IDs in this node
    
    // Memory information
    uint64_t total_memory_bytes;
    uint64_t available_memory_bytes;
    double memory_bandwidth_gbps;   // Peak memory bandwidth
    uint32_t memory_latency_ns;     // Local memory access latency
    
    // Distance matrix (latency to other nodes)
    uint32_t* distance_to_nodes;   // Array indexed by node_id
    
    // Node characteristics
    bool is_available;
    bool supports_interleaving;
    uint32_t cache_line_size;
    uint32_t page_size;
} NumaNode;

// NUMA topology structure
typedef struct NumaTopology {
    uint32_t node_count;
    NumaNode* nodes;
    
    // Global CPU mapping
    uint32_t total_cpu_count;
    uint32_t* cpu_to_node_map;      // Maps CPU ID to NUMA node ID
    
    // Topology metrics
    uint32_t max_distance;          // Maximum inter-node distance
    double avg_bandwidth;           // Average memory bandwidth
    bool uniform_topology;          // True if all distances are equal
    
    // System capabilities
    bool numa_available;
    bool can_set_affinity;
    bool supports_migration;
    bool supports_interleaving;
} NumaTopology;

// Memory affinity policies
typedef enum NumaAffinityPolicy {
    NUMA_POLICY_NONE = 0,           // No specific policy
    NUMA_POLICY_LOCAL,              // Prefer local node memory
    NUMA_POLICY_INTERLEAVE,         // Interleave across all nodes
    NUMA_POLICY_BIND,               // Bind to specific nodes
    NUMA_POLICY_PREFERRED,          // Prefer specific nodes, fallback allowed
    NUMA_POLICY_BALANCED            // Dynamic balancing based on load
} NumaAffinityPolicy;

// NUMA-aware task placement strategy
typedef enum NumaPlacementStrategy {
    NUMA_PLACEMENT_ROUND_ROBIN = 0,  // Simple round-robin across nodes
    NUMA_PLACEMENT_MEMORY_LOCAL,     // Place tasks near their data
    NUMA_PLACEMENT_LOAD_BALANCED,    // Balance load across nodes
    NUMA_PLACEMENT_BANDWIDTH_AWARE,  // Consider memory bandwidth
    NUMA_PLACEMENT_LATENCY_OPTIMIZED, // Minimize memory access latency
    NUMA_PLACEMENT_ADAPTIVE          // Adapt based on runtime characteristics
} NumaPlacementStrategy;

// Memory region with NUMA affinity
typedef struct NumaMemoryRegion {
    void* base_address;
    size_t size;
    uint32_t preferred_node;        // Preferred NUMA node for this memory
    NumaAffinityPolicy affinity_policy;
    
    // Access patterns
    atomic_uint_fast64_t local_accesses;
    atomic_uint_fast64_t remote_accesses;
    atomic_uint_fast64_t last_access_time;
    
    // Migration hints
    bool can_migrate;
    uint32_t migration_cost;
    uint32_t access_frequency;
    
    struct NumaMemoryRegion* next;
} NumaMemoryRegion;

// NUMA-aware worker context
typedef struct NumaWorkerContext {
    uint32_t worker_id;
    uint32_t numa_node;
    uint32_t cpu_id;
    
    // Task affinity
    TaskScope* scope;
    WorkStealingDeque* local_deque;
    
    // NUMA-specific statistics
    atomic_uint_fast64_t local_memory_accesses;
    atomic_uint_fast64_t remote_memory_accesses;
    atomic_uint_fast64_t cross_node_steals;
    atomic_uint_fast64_t migration_events;
    
    // Performance metrics
    uint64_t total_execution_time_ns;
    uint64_t memory_stall_time_ns;
    double cache_miss_rate;
    
    // Worker state
    bool is_active;
    atomic_bool prefer_local_tasks;
    uint32_t load_factor;           // Current load (0-100)
} NumaWorkerContext;

// NUMA-aware parallel for configuration
typedef struct NumaParallelForConfig {
    ParallelForConfig base_config;
    
    // NUMA settings
    NumaPlacementStrategy placement_strategy;
    NumaAffinityPolicy memory_policy;
    bool enable_numa_awareness;
    bool enable_task_migration;
    bool enable_memory_migration;
    
    // Performance tuning
    uint32_t local_preference_weight;    // 0-100, preference for local tasks
    uint32_t migration_threshold;        // Load imbalance threshold for migration
    uint32_t memory_access_threshold;    // Remote access threshold for migration
    
    // Node selection
    uint32_t* preferred_nodes;          // Array of preferred NUMA nodes
    size_t preferred_node_count;
    uint32_t* excluded_nodes;           // Array of excluded NUMA nodes  
    size_t excluded_node_count;
    
    // Memory regions
    NumaMemoryRegion** memory_regions;
    size_t memory_region_count;
    
    // Callbacks
    void (*on_migration)(uint32_t worker_id, uint32_t from_node, uint32_t to_node);
    void (*on_load_imbalance)(uint32_t overloaded_node, uint32_t underloaded_node);
} NumaParallelForConfig;

// NUMA-aware task scope
typedef struct NumaTaskScope {
    TaskScope base_scope;
    
    // NUMA topology
    NumaTopology* topology;
    
    // Worker management
    NumaWorkerContext* numa_workers;
    size_t numa_worker_count;
    
    // Load balancing
    atomic_uint_fast32_t* node_load_counters;  // Load per NUMA node
    uint32_t* node_queue_lengths;              // Queue length per node
    
    // Memory management
    NumaMemoryRegion* memory_regions;
    uint32_t active_memory_regions;
    
    // Performance monitoring
    atomic_uint_fast64_t total_migrations;
    atomic_uint_fast64_t cross_node_accesses;
    atomic_uint_fast64_t local_accesses;
    
    // Configuration
    NumaParallelForConfig config;
    bool numa_initialized;
    bool enable_dynamic_balancing;
} NumaTaskScope;

// NUMA topology discovery and management
NumaTopology* numa_topology_discover(void);
void numa_topology_destroy(NumaTopology* topology);
bool numa_topology_is_available(void);
void numa_topology_print(const NumaTopology* topology);

// NUMA node operations
uint32_t numa_get_current_node(void);
uint32_t numa_get_cpu_node(uint32_t cpu_id);
bool numa_set_cpu_affinity(uint32_t cpu_id);
bool numa_bind_to_node(uint32_t node_id);

// Memory operations
void* numa_alloc_on_node(size_t size, uint32_t node_id);
void* numa_alloc_interleaved(size_t size, uint32_t* nodes, size_t node_count);
bool numa_move_memory(void* addr, size_t size, uint32_t target_node);
void numa_free(void* ptr, size_t size);

// Memory region management
NumaMemoryRegion* numa_memory_region_create(void* base, size_t size, uint32_t preferred_node);
void numa_memory_region_destroy(NumaMemoryRegion* region);
uint32_t numa_memory_region_get_node(const void* address);
bool numa_memory_region_is_local(const NumaMemoryRegion* region, uint32_t worker_node);

// NUMA-aware task scope operations
NumaTaskScope* numa_task_scope_create(NumaParallelForConfig config, const char* name);
void numa_task_scope_destroy(NumaTaskScope* scope);
Result_void_ptr numa_task_scope_start(NumaTaskScope* scope);
Result_void_ptr numa_task_scope_shutdown(NumaTaskScope* scope, uint64_t timeout_ms);

// NUMA-aware parallel for execution
Result_void_ptr numa_parallel_for(
    NumaTaskScope* scope,
    NumaParallelForConfig config,
    ParallelForFunction function,
    void* context
);

// Task placement and migration
uint32_t numa_select_optimal_node(const NumaTaskScope* scope, const void* data_address, size_t data_size);
bool numa_migrate_task(NumaTaskScope* scope, uint32_t task_id, uint32_t target_node);
void numa_balance_load(NumaTaskScope* scope);
uint32_t numa_find_least_loaded_node(const NumaTaskScope* scope);

// Performance analysis
void numa_analyze_memory_access_patterns(const NumaTaskScope* scope);
double numa_calculate_locality_ratio(const NumaTaskScope* scope);
void numa_print_performance_report(const NumaTaskScope* scope);
void numa_optimize_placement(NumaTaskScope* scope);

// Configuration helpers
NumaParallelForConfig numa_config_default(void);
NumaParallelForConfig numa_config_memory_bound(void);
NumaParallelForConfig numa_config_compute_bound(void);
NumaParallelForConfig numa_config_adaptive(void);

// Utility functions
const char* numa_placement_strategy_to_string(NumaPlacementStrategy strategy);
const char* numa_affinity_policy_to_string(NumaAffinityPolicy policy);
uint32_t numa_distance_between_nodes(const NumaTopology* topology, uint32_t node1, uint32_t node2);
bool numa_nodes_are_local(const NumaTopology* topology, uint32_t node1, uint32_t node2);

// Integration with existing systems
Result_void_ptr numa_work_stealing_parallel_for(
    NumaTaskScope* scope,
    NumaParallelForConfig config,
    ParallelForFunction function,
    void* context
);

// NUMA awareness macros for easy integration
#define NUMA_LOCAL_ALLOC(size, node) numa_alloc_on_node(size, node)
#define NUMA_CHECK_LOCALITY(region, worker_node) numa_memory_region_is_local(region, worker_node)
#define NUMA_MIGRATE_IF_BENEFICIAL(scope, task_id, data_addr) \
    do { \
        uint32_t optimal_node = numa_select_optimal_node(scope, data_addr, 0); \
        if (optimal_node != numa_get_current_node()) { \
            numa_migrate_task(scope, task_id, optimal_node); \
        } \
    } while(0)

// Platform-specific NUMA support
#ifdef __linux__
#include <numa.h>
#include <numaif.h>
#define NUMA_PLATFORM_SUPPORTED 1
#else
#define NUMA_PLATFORM_SUPPORTED 0
// Provide stub implementations for non-Linux platforms
#endif

// Error codes for NUMA operations
typedef enum NumaError {
    NUMA_SUCCESS = 0,
    NUMA_ERROR_NOT_AVAILABLE,
    NUMA_ERROR_INVALID_NODE,
    NUMA_ERROR_AFFINITY_FAILED,
    NUMA_ERROR_ALLOCATION_FAILED,
    NUMA_ERROR_MIGRATION_FAILED,
    NUMA_ERROR_TOPOLOGY_DISCOVERY_FAILED
} NumaError;

#endif // NUMA_SCHEDULING_H