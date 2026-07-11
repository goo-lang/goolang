#include "../../include/numa_scheduling.h"
#include "../../include/errors/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

// Platform-specific includes
#ifdef __linux__
#include <sched.h>
#include <sys/syscall.h>
#endif

// Global NUMA topology (discovered once)
static NumaTopology* g_numa_topology = NULL;
static bool g_numa_initialized = false;

// Discover NUMA topology
NumaTopology* numa_topology_discover(void) {
    if (g_numa_topology && g_numa_initialized) {
        return g_numa_topology;
    }
    
    NumaTopology* topology = xcalloc(1, sizeof(NumaTopology));
    if (!topology) return NULL;
    
#ifdef __linux__
    // Try to discover actual NUMA topology on Linux
    FILE* nodes_file = fopen("/sys/devices/system/node/possible", "r");
    if (nodes_file) {
        char buffer[256];
        if (fgets(buffer, sizeof(buffer), nodes_file)) {
            // Parse node range (e.g., "0-1" or "0")
            int first_node, last_node;
            if (sscanf(buffer, "%d-%d", &first_node, &last_node) == 2) {
                topology->node_count = last_node - first_node + 1;
            } else if (sscanf(buffer, "%d", &first_node) == 1) {
                topology->node_count = 1;
            }
        }
        fclose(nodes_file);
    }
    
    // Fallback if we couldn't read the file
    if (topology->node_count == 0) {
        topology->node_count = 1; // Assume single node
    }
    
    topology->numa_available = topology->node_count > 1;
#else
    // Non-Linux platforms: simulate single NUMA node
    topology->node_count = 1;
    topology->numa_available = false;
#endif
    
    // Get CPU count
    topology->total_cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (topology->total_cpu_count <= 0) {
        topology->total_cpu_count = 4; // Fallback
    }
    
    // Allocate nodes
    topology->nodes = calloc(topology->node_count, sizeof(NumaNode));
    topology->cpu_to_node_map = calloc(topology->total_cpu_count, sizeof(uint32_t));
    
    if (!topology->nodes || !topology->cpu_to_node_map) {
        numa_topology_destroy(topology);
        return NULL;
    }
    
    // Initialize nodes
    uint32_t cpus_per_node = topology->total_cpu_count / topology->node_count;
    for (uint32_t i = 0; i < topology->node_count; i++) {
        NumaNode* node = &topology->nodes[i];
        node->node_id = i;
        node->cpu_count = (i == topology->node_count - 1) ? 
                         topology->total_cpu_count - (i * cpus_per_node) : cpus_per_node;
        
        // Allocate CPU IDs for this node
        node->cpu_ids = calloc(node->cpu_count, sizeof(uint32_t));
        if (!node->cpu_ids) {
            numa_topology_destroy(topology);
            return NULL;
        }
        
        // Assign CPUs to this node
        for (uint32_t j = 0; j < node->cpu_count; j++) {
            uint32_t cpu_id = i * cpus_per_node + j;
            if (cpu_id < topology->total_cpu_count) {
                node->cpu_ids[j] = cpu_id;
                topology->cpu_to_node_map[cpu_id] = i;
            }
        }
        
        // Set default characteristics
        node->is_available = true;
        node->supports_interleaving = topology->numa_available;
        node->cache_line_size = 64;  // Common cache line size
        node->page_size = 4096;      // Common page size
        node->total_memory_bytes = 1ULL << 30; // 1GB default
        node->available_memory_bytes = node->total_memory_bytes;
        node->memory_bandwidth_gbps = 10.0; // 10 GB/s default
        node->memory_latency_ns = 100;       // 100ns default
        
        // Allocate distance matrix
        node->distance_to_nodes = calloc(topology->node_count, sizeof(uint32_t));
        if (!node->distance_to_nodes) {
            numa_topology_destroy(topology);
            return NULL;
        }
        
        // Initialize distances (simplified model)
        for (uint32_t k = 0; k < topology->node_count; k++) {
            if (i == k) {
                node->distance_to_nodes[k] = 10;  // Local access
            } else {
                node->distance_to_nodes[k] = 20;  // Remote access
            }
        }
    }
    
    // Set topology characteristics
    topology->max_distance = 20;
    topology->avg_bandwidth = 10.0;
    topology->uniform_topology = true;
    topology->can_set_affinity = true;
    topology->supports_migration = true;
    topology->supports_interleaving = topology->numa_available;
    
    g_numa_topology = topology;
    g_numa_initialized = true;
    
    return topology;
}

// Destroy NUMA topology
void numa_topology_destroy(NumaTopology* topology) {
    if (!topology) return;
    
    if (topology->nodes) {
        for (uint32_t i = 0; i < topology->node_count; i++) {
            free(topology->nodes[i].cpu_ids);
            free(topology->nodes[i].distance_to_nodes);
        }
        free(topology->nodes);
    }
    
    free(topology->cpu_to_node_map);

    // Clear the global BEFORE freeing: comparing topology after free(topology)
    // reads the value of a pointer whose lifetime has ended, which is undefined
    // behavior (C11 §6.2.4). The comparison is pointer identity only, so doing
    // it here (before the free) is equivalent and well-defined.
    if (topology == g_numa_topology) {
        g_numa_topology = NULL;
        g_numa_initialized = false;
    }

    free(topology);
}

// Check if NUMA is available
bool numa_topology_is_available(void) {
    NumaTopology* topology = numa_topology_discover();
    return topology && topology->numa_available;
}

// Print NUMA topology information
void numa_topology_print(const NumaTopology* topology) {
    if (!topology) {
        printf("NUMA topology not available\n");
        return;
    }
    
    printf("=== NUMA Topology ===\n");
    printf("NUMA available: %s\n", topology->numa_available ? "Yes" : "No");
    printf("Node count: %u\n", topology->node_count);
    printf("Total CPUs: %u\n", topology->total_cpu_count);
    printf("Max distance: %u\n", topology->max_distance);
    printf("Uniform topology: %s\n", topology->uniform_topology ? "Yes" : "No");
    
    for (uint32_t i = 0; i < topology->node_count; i++) {
        const NumaNode* node = &topology->nodes[i];
        printf("\nNode %u:\n", node->node_id);
        printf("  CPUs: %u (", node->cpu_count);
        for (uint32_t j = 0; j < node->cpu_count; j++) {
            printf("%u", node->cpu_ids[j]);
            if (j < node->cpu_count - 1) printf(", ");
        }
        printf(")\n");
        printf("  Memory: %llu MB available\n", node->available_memory_bytes / (1024 * 1024));
        printf("  Bandwidth: %.1f GB/s\n", node->memory_bandwidth_gbps);
        printf("  Latency: %u ns\n", node->memory_latency_ns);
    }
}

// Get current NUMA node
uint32_t numa_get_current_node(void) {
#ifdef __linux__
    int cpu = sched_getcpu();
    if (cpu >= 0) {
        NumaTopology* topology = numa_topology_discover();
        if (topology && cpu < (int)topology->total_cpu_count) {
            return topology->cpu_to_node_map[cpu];
        }
    }
#endif
    return 0; // Default to node 0
}

// Get NUMA node for a specific CPU
uint32_t numa_get_cpu_node(uint32_t cpu_id) {
    NumaTopology* topology = numa_topology_discover();
    if (topology && cpu_id < topology->total_cpu_count) {
        return topology->cpu_to_node_map[cpu_id];
    }
    return 0;
}

// Set CPU affinity
bool numa_set_cpu_affinity(uint32_t cpu_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    
    return sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0;
#else
    (void)cpu_id;
    return false; // Not supported on this platform
#endif
}

// Bind process to NUMA node
bool numa_bind_to_node(uint32_t node_id) {
    NumaTopology* topology = numa_topology_discover();
    if (!topology || node_id >= topology->node_count) {
        return false;
    }
    
#ifdef __linux__
    // Set affinity to all CPUs in the node
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    const NumaNode* node = &topology->nodes[node_id];
    for (uint32_t i = 0; i < node->cpu_count; i++) {
        CPU_SET(node->cpu_ids[i], &cpuset);
    }
    
    return sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0;
#else
    (void)node_id;
    return false;
#endif
}

// NUMA memory allocation (simplified for cross-platform compatibility)
void* numa_alloc_on_node(size_t size, uint32_t node_id) {
    // For now, use regular malloc (would use numa_alloc_onnode on Linux with libnuma)
    (void)node_id;
    return malloc(size);
}

void* numa_alloc_interleaved(size_t size, uint32_t* nodes, size_t node_count) {
    // For now, use regular malloc (would use numa_alloc_interleaved on Linux)
    (void)nodes;
    (void)node_count;
    return malloc(size);
}

bool numa_move_memory(void* addr, size_t size, uint32_t target_node) {
    // Memory migration not implemented in this simplified version
    (void)addr;
    (void)size;
    (void)target_node;
    return false;
}

void numa_free(void* ptr, size_t size) {
    (void)size;
    free(ptr);
}

// Memory region management
NumaMemoryRegion* numa_memory_region_create(void* base, size_t size, uint32_t preferred_node) {
    NumaMemoryRegion* region = xcalloc(1, sizeof(NumaMemoryRegion));
    if (!region) return NULL;
    
    region->base_address = base;
    region->size = size;
    region->preferred_node = preferred_node;
    region->affinity_policy = NUMA_POLICY_LOCAL;
    region->can_migrate = true;
    region->migration_cost = 50; // Arbitrary cost units
    region->access_frequency = 0;
    
    atomic_init(&region->local_accesses, 0);
    atomic_init(&region->remote_accesses, 0);
    atomic_init(&region->last_access_time, 0);
    
    return region;
}

void numa_memory_region_destroy(NumaMemoryRegion* region) {
    if (!region) return;
    free(region);
}

uint32_t numa_memory_region_get_node(const void* address) {
    // Simplified: just return current node
    // In a real implementation, this would query the kernel for the page's node
    (void)address;
    return numa_get_current_node();
}

bool numa_memory_region_is_local(const NumaMemoryRegion* region, uint32_t worker_node) {
    return region && region->preferred_node == worker_node;
}

// NUMA-aware task scope creation
NumaTaskScope* numa_task_scope_create(NumaParallelForConfig config, const char* name) {
    NumaTaskScope* scope = xcalloc(1, sizeof(NumaTaskScope));
    if (!scope) return NULL;
    
    // Initialize base task scope
    TaskScopeConfig base_config = task_scope_config_default();
    base_config.max_concurrent_tasks = config.base_config.max_workers;
    base_config.numa_aware_scheduling = true;
    
    TaskScope* base_scope = task_scope_create(base_config, name);
    if (!base_scope) {
        free(scope);
        return NULL;
    }
    
    scope->base_scope = *base_scope;
    scope->config = config;
    
    // Discover NUMA topology
    scope->topology = numa_topology_discover();
    if (!scope->topology) {
        free(scope);
        free(base_scope);
        return NULL;
    }
    
    // Allocate NUMA workers
    scope->numa_worker_count = config.base_config.max_workers;
    scope->numa_workers = calloc(scope->numa_worker_count, sizeof(NumaWorkerContext));
    
    // Allocate per-node load counters
    scope->node_load_counters = calloc(scope->topology->node_count, sizeof(atomic_uint_fast32_t));
    scope->node_queue_lengths = calloc(scope->topology->node_count, sizeof(uint32_t));
    
    if (!scope->numa_workers || !scope->node_load_counters || !scope->node_queue_lengths) {
        numa_task_scope_destroy(scope);
        return NULL;
    }
    
    // Initialize NUMA workers
    for (size_t i = 0; i < scope->numa_worker_count; i++) {
        NumaWorkerContext* worker = &scope->numa_workers[i];
        worker->worker_id = i;
        
        // Assign worker to NUMA node based on strategy
        if (config.placement_strategy == NUMA_PLACEMENT_ROUND_ROBIN) {
            worker->numa_node = i % scope->topology->node_count;
        } else {
            worker->numa_node = 0; // Default to node 0
        }
        
        // Assign CPU within the node
        const NumaNode* node = &scope->topology->nodes[worker->numa_node];
        if (node->cpu_count > 0) {
            worker->cpu_id = node->cpu_ids[i % node->cpu_count];
        } else {
            worker->cpu_id = i; // Fallback
        }
        
        worker->scope = &scope->base_scope;
        worker->is_active = true;
        worker->load_factor = 0;
        
        atomic_init(&worker->local_memory_accesses, 0);
        atomic_init(&worker->remote_memory_accesses, 0);
        atomic_init(&worker->cross_node_steals, 0);
        atomic_init(&worker->migration_events, 0);
        atomic_init(&worker->prefer_local_tasks, true);
    }
    
    // Initialize per-node counters
    for (uint32_t i = 0; i < scope->topology->node_count; i++) {
        atomic_init(&scope->node_load_counters[i], 0);
        scope->node_queue_lengths[i] = 0;
    }
    
    // Initialize global counters
    atomic_init(&scope->total_migrations, 0);
    atomic_init(&scope->cross_node_accesses, 0);
    atomic_init(&scope->local_accesses, 0);
    
    scope->numa_initialized = true;
    scope->enable_dynamic_balancing = config.enable_task_migration;
    
    free(base_scope); // We copied the contents
    return scope;
}

// Destroy NUMA task scope
void numa_task_scope_destroy(NumaTaskScope* scope) {
    if (!scope) return;
    
    free(scope->numa_workers);
    free(scope->node_load_counters);
    free(scope->node_queue_lengths);
    
    // Don't destroy the global topology
    
    free(scope);
}

// Start NUMA-aware task scope
Result_void_ptr numa_task_scope_start(NumaTaskScope* scope) {
    if (!scope || !scope->numa_initialized) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("NUMA task scope not properly initialized"),
            .hint = strdup("Ensure NUMA topology is available"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Set CPU affinity for workers if enabled
    if (scope->topology->can_set_affinity && scope->config.enable_numa_awareness) {
        for (size_t i = 0; i < scope->numa_worker_count; i++) {
            NumaWorkerContext* worker = &scope->numa_workers[i];
            numa_set_cpu_affinity(worker->cpu_id);
        }
    }
    
    return task_scope_start(&scope->base_scope);
}

// Select optimal NUMA node for task placement
uint32_t numa_select_optimal_node(const NumaTaskScope* scope, const void* data_address, size_t data_size) {
    if (!scope || !scope->numa_initialized) {
        return 0;
    }
    
    uint32_t optimal_node = 0;
    
    switch (scope->config.placement_strategy) {
        case NUMA_PLACEMENT_ROUND_ROBIN: {
            static atomic_uint_fast32_t counter = 0;
            optimal_node = atomic_fetch_add(&counter, 1) % scope->topology->node_count;
            break;
        }
        
        case NUMA_PLACEMENT_MEMORY_LOCAL:
            if (data_address) {
                optimal_node = numa_memory_region_get_node(data_address);
            } else {
                optimal_node = numa_get_current_node();
            }
            break;
            
        case NUMA_PLACEMENT_LOAD_BALANCED:
            optimal_node = numa_find_least_loaded_node(scope);
            break;
            
        case NUMA_PLACEMENT_BANDWIDTH_AWARE:
        case NUMA_PLACEMENT_LATENCY_OPTIMIZED:
        case NUMA_PLACEMENT_ADAPTIVE:
        default:
            // Find the node with the best characteristics for this workload
            optimal_node = numa_find_least_loaded_node(scope);
            break;
    }
    
    // Validate the selected node
    if (optimal_node >= scope->topology->node_count) {
        optimal_node = 0;
    }
    
    (void)data_size; // Unused in this simplified implementation
    
    return optimal_node;
}

// Find the least loaded NUMA node
uint32_t numa_find_least_loaded_node(const NumaTaskScope* scope) {
    if (!scope || !scope->numa_initialized) {
        return 0;
    }
    
    uint32_t least_loaded_node = 0;
    uint32_t min_load = UINT32_MAX;
    
    for (uint32_t i = 0; i < scope->topology->node_count; i++) {
        uint32_t load = atomic_load(&scope->node_load_counters[i]);
        if (load < min_load) {
            min_load = load;
            least_loaded_node = i;
        }
    }
    
    return least_loaded_node;
}

// Calculate memory locality ratio
double numa_calculate_locality_ratio(const NumaTaskScope* scope) {
    if (!scope) return 0.0;
    
    uint64_t local = atomic_load(&scope->local_accesses);
    uint64_t remote = atomic_load(&scope->cross_node_accesses);
    uint64_t total = local + remote;
    
    return total > 0 ? (double)local / total : 1.0;
}

// Print NUMA performance report
void numa_print_performance_report(const NumaTaskScope* scope) {
    if (!scope) return;
    
    printf("\n=== NUMA Performance Report ===\n");
    printf("NUMA awareness: %s\n", scope->config.enable_numa_awareness ? "enabled" : "disabled");
    printf("Placement strategy: %s\n", numa_placement_strategy_to_string(scope->config.placement_strategy));
    printf("Memory policy: %s\n", numa_affinity_policy_to_string(scope->config.memory_policy));
    
    printf("\nMemory Access Patterns:\n");
    printf("  Local accesses: %llu\n", atomic_load(&scope->local_accesses));
    printf("  Cross-node accesses: %llu\n", atomic_load(&scope->cross_node_accesses));
    printf("  Locality ratio: %.2f%%\n", numa_calculate_locality_ratio(scope) * 100.0);
    
    printf("\nMigration Statistics:\n");
    printf("  Total migrations: %llu\n", atomic_load(&scope->total_migrations));
    
    printf("\nPer-Node Load:\n");
    for (uint32_t i = 0; i < scope->topology->node_count; i++) {
        uint32_t load = atomic_load(&scope->node_load_counters[i]);
        printf("  Node %u: %u tasks\n", i, load);
    }
    
    printf("\nPer-Worker Statistics:\n");
    for (size_t i = 0; i < scope->numa_worker_count; i++) {
        const NumaWorkerContext* worker = &scope->numa_workers[i];
        printf("  Worker %u (Node %u, CPU %u):\n", worker->worker_id, worker->numa_node, worker->cpu_id);
        printf("    Local memory accesses: %llu\n", atomic_load(&worker->local_memory_accesses));
        printf("    Remote memory accesses: %llu\n", atomic_load(&worker->remote_memory_accesses));
        printf("    Cross-node steals: %llu\n", atomic_load(&worker->cross_node_steals));
        printf("    Migrations: %llu\n", atomic_load(&worker->migration_events));
    }
}

// Configuration presets
NumaParallelForConfig numa_config_default(void) {
    return (NumaParallelForConfig) {
        .base_config = {
            .start_index = 0,
            .end_index = 0,
            .chunk_size = 0,
            .max_workers = 8,
            .priority = TASK_PRIORITY_NORMAL
        },
        .placement_strategy = NUMA_PLACEMENT_LOAD_BALANCED,
        .memory_policy = NUMA_POLICY_LOCAL,
        .enable_numa_awareness = true,
        .enable_task_migration = false,
        .enable_memory_migration = false,
        .local_preference_weight = 80,
        .migration_threshold = 20,
        .memory_access_threshold = 10,
        .preferred_nodes = NULL,
        .preferred_node_count = 0,
        .excluded_nodes = NULL,
        .excluded_node_count = 0,
        .memory_regions = NULL,
        .memory_region_count = 0,
        .on_migration = NULL,
        .on_load_imbalance = NULL
    };
}

NumaParallelForConfig numa_config_memory_bound(void) {
    NumaParallelForConfig config = numa_config_default();
    config.placement_strategy = NUMA_PLACEMENT_MEMORY_LOCAL;
    config.memory_policy = NUMA_POLICY_BIND;
    config.local_preference_weight = 95;
    config.enable_memory_migration = true;
    return config;
}

NumaParallelForConfig numa_config_compute_bound(void) {
    NumaParallelForConfig config = numa_config_default();
    config.placement_strategy = NUMA_PLACEMENT_LOAD_BALANCED;
    config.memory_policy = NUMA_POLICY_INTERLEAVE;
    config.enable_task_migration = true;
    config.local_preference_weight = 60;
    return config;
}

// Utility functions
const char* numa_placement_strategy_to_string(NumaPlacementStrategy strategy) {
    switch (strategy) {
        case NUMA_PLACEMENT_ROUND_ROBIN: return "Round Robin";
        case NUMA_PLACEMENT_MEMORY_LOCAL: return "Memory Local";
        case NUMA_PLACEMENT_LOAD_BALANCED: return "Load Balanced";
        case NUMA_PLACEMENT_BANDWIDTH_AWARE: return "Bandwidth Aware";
        case NUMA_PLACEMENT_LATENCY_OPTIMIZED: return "Latency Optimized";
        case NUMA_PLACEMENT_ADAPTIVE: return "Adaptive";
        default: return "Unknown";
    }
}

const char* numa_affinity_policy_to_string(NumaAffinityPolicy policy) {
    switch (policy) {
        case NUMA_POLICY_NONE: return "None";
        case NUMA_POLICY_LOCAL: return "Local";
        case NUMA_POLICY_INTERLEAVE: return "Interleave";
        case NUMA_POLICY_BIND: return "Bind";
        case NUMA_POLICY_PREFERRED: return "Preferred";
        case NUMA_POLICY_BALANCED: return "Balanced";
        default: return "Unknown";
    }
}

// NUMA-aware parallel for execution (simplified implementation)
Result_void_ptr numa_parallel_for(
    NumaTaskScope* scope,
    NumaParallelForConfig config,
    ParallelForFunction function,
    void* context) {
    
    if (!scope || !function) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Invalid NUMA parallel for parameters"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // For this simplified implementation, delegate to the base parallel for
    // In a full implementation, this would:
    // 1. Analyze data placement
    // 2. Assign tasks to optimal NUMA nodes
    // 3. Set CPU affinity for workers
    // 4. Monitor and balance load across nodes
    // 5. Migrate tasks if beneficial
    
    ParallelForConfig base_config = config.base_config;
    return task_scope_parallel_for(&scope->base_scope, base_config, function, context);
}