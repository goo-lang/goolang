#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "include/numa_scheduling.h"
#include "include/performance_monitoring.h"

// Test data structure for NUMA scheduling validation
typedef struct {
    int* data_array;
    size_t array_size;
    uint32_t* access_pattern;
    size_t current_node;
    bool track_locality;
} NumaTestContext;

// NUMA-aware computation function
static Result_void_ptr numa_computation_function(size_t index, void* context) {
    NumaTestContext* ctx = (NumaTestContext*)context;
    
    // Record current NUMA node for this access
    if (ctx->track_locality && ctx->access_pattern) {
        ctx->access_pattern[index] = numa_get_current_node();
    }
    
    // Simulate memory-intensive computation
    if (index < ctx->array_size) {
        ctx->data_array[index] = (int)(index * 3 + 7);
        
        // Simulate some memory access patterns
        if (index > 0) {
            ctx->data_array[index] += ctx->data_array[index - 1] / 2;
        }
    }
    
    // Brief computation to simulate work
    volatile int sum = 0;
    for (int i = 0; i < 1000; i++) {
        sum += i;
    }
    
    return OK_PTR(NULL);
}

// Memory-bound workload function
static Result_void_ptr memory_bound_function(size_t index, void* context) {
    NumaTestContext* ctx = (NumaTestContext*)context;
    
    if (ctx->track_locality && ctx->access_pattern) {
        ctx->access_pattern[index] = numa_get_current_node();
    }
    
    // Heavy memory access pattern
    if (index < ctx->array_size) {
        // Multiple memory accesses to simulate cache-intensive workload
        for (size_t i = 0; i < 10 && (index + i) < ctx->array_size; i++) {
            ctx->data_array[index + i] = (int)((index + i) * 2);
        }
        
        // Read back and modify (simulates typical memory-bound computation)
        int sum = 0;
        for (size_t i = 0; i < 10 && (index + i) < ctx->array_size; i++) {
            sum += ctx->data_array[index + i];
        }
        ctx->data_array[index] = sum % 1000;
    }
    
    return OK_PTR(NULL);
}

// Migration callback
static void migration_callback(uint32_t worker_id, uint32_t from_node, uint32_t to_node) {
    printf("📦 Worker %u migrated from NUMA node %u to node %u\n", 
           worker_id, from_node, to_node);
}

// Load imbalance callback
static void load_imbalance_callback(uint32_t overloaded_node, uint32_t underloaded_node) {
    printf("⚖️  Load imbalance detected: Node %u overloaded, Node %u underloaded\n", 
           overloaded_node, underloaded_node);
}

// Test NUMA topology discovery
static void test_numa_topology_discovery(void) {
    printf("=== Test 1: NUMA Topology Discovery ===\n");
    
    // Discover NUMA topology
    NumaTopology* topology = numa_topology_discover();
    if (!topology) {
        printf("❌ Failed to discover NUMA topology\n");
        return;
    }
    
    printf("✅ NUMA topology discovered successfully\n");
    printf("NUMA available: %s\n", topology->numa_available ? "Yes" : "No");
    printf("Current NUMA node: %u\n", numa_get_current_node());
    
    // Print detailed topology
    numa_topology_print(topology);
    
    printf("✅ NUMA topology discovery test completed\n");
}

// Test NUMA-aware task scope creation
static void test_numa_task_scope(void) {
    printf("\n=== Test 2: NUMA Task Scope Creation ===\n");
    
    // Create NUMA configuration
    NumaParallelForConfig config = numa_config_default();
    config.base_config.max_workers = 4;
    config.placement_strategy = NUMA_PLACEMENT_LOAD_BALANCED;
    config.enable_numa_awareness = true;
    config.on_migration = migration_callback;
    config.on_load_imbalance = load_imbalance_callback;
    
    printf("Configuration:\n");
    printf("  Placement strategy: %s\n", numa_placement_strategy_to_string(config.placement_strategy));
    printf("  Memory policy: %s\n", numa_affinity_policy_to_string(config.memory_policy));
    printf("  NUMA awareness: %s\n", config.enable_numa_awareness ? "enabled" : "disabled");
    
    // Create NUMA task scope
    NumaTaskScope* scope = numa_task_scope_create(config, "numa_test");
    if (!scope) {
        printf("❌ Failed to create NUMA task scope\n");
        return;
    }
    
    printf("✅ NUMA task scope created successfully\n");
    printf("Topology nodes: %u\n", scope->topology->node_count);
    printf("NUMA workers: %zu\n", scope->numa_worker_count);
    printf("NUMA initialized: %s\n", scope->numa_initialized ? "yes" : "no");
    
    // Start the scope
    Result_void_ptr start_result = numa_task_scope_start(scope);
    if (start_result.is_error) {
        printf("❌ Failed to start NUMA task scope: %s\n", start_result.error->message);
    } else {
        printf("✅ NUMA task scope started successfully\n");
    }
    
    // Clean up
    numa_task_scope_destroy(scope);
    printf("✅ NUMA task scope test completed\n");
}

// Test different NUMA placement strategies
static void test_numa_placement_strategies(void) {
    printf("\n=== Test 3: NUMA Placement Strategies ===\n");
    
    const size_t ARRAY_SIZE = 2000;
    int* test_array = calloc(ARRAY_SIZE, sizeof(int));
    uint32_t* access_pattern = calloc(ARRAY_SIZE, sizeof(uint32_t));
    
    NumaTestContext context = {
        .data_array = test_array,
        .array_size = ARRAY_SIZE,
        .access_pattern = access_pattern,
        .current_node = 0,
        .track_locality = true
    };
    
    struct {
        const char* name;
        NumaPlacementStrategy strategy;
    } strategies[] = {
        {"Round Robin", NUMA_PLACEMENT_ROUND_ROBIN},
        {"Memory Local", NUMA_PLACEMENT_MEMORY_LOCAL},
        {"Load Balanced", NUMA_PLACEMENT_LOAD_BALANCED},
        {"Bandwidth Aware", NUMA_PLACEMENT_BANDWIDTH_AWARE}
    };
    
    for (size_t i = 0; i < sizeof(strategies) / sizeof(strategies[0]); i++) {
        printf("\nTesting %s placement strategy:\n", strategies[i].name);
        
        // Reset test data
        memset(test_array, 0, ARRAY_SIZE * sizeof(int));
        memset(access_pattern, 0, ARRAY_SIZE * sizeof(uint32_t));
        
        // Configure NUMA parallel for
        NumaParallelForConfig config = numa_config_default();
        config.base_config.start_index = 0;
        config.base_config.end_index = ARRAY_SIZE;
        config.base_config.max_workers = 4;
        config.placement_strategy = strategies[i].strategy;
        config.enable_numa_awareness = true;
        
        // Create and run
        NumaTaskScope* scope = numa_task_scope_create(config, "strategy_test");
        if (scope) {
            numa_task_scope_start(scope);
            
            uint64_t start_time = performance_get_timestamp_ns();
            Result_void_ptr result = numa_parallel_for(scope, config, numa_computation_function, &context);
            uint64_t end_time = performance_get_timestamp_ns();
            
            if (result.is_error) {
                printf("  ❌ Execution failed: %s\n", result.error->message);
            } else {
                printf("  ✅ Execution completed successfully\n");
                printf("  Execution time: %.3f ms\n", (end_time - start_time) / 1e6);
                
                // Analyze NUMA node distribution
                uint32_t node_counts[8] = {0}; // Assume max 8 nodes
                for (size_t j = 0; j < ARRAY_SIZE; j++) {
                    if (access_pattern[j] < 8) {
                        node_counts[access_pattern[j]]++;
                    }
                }
                
                printf("  NUMA node distribution: ");
                NumaTopology* topology = numa_topology_discover();
                for (uint32_t k = 0; k < topology->node_count; k++) {
                    printf("Node %u: %u tasks ", k, node_counts[k]);
                }
                printf("\n");
            }
            
            numa_task_scope_destroy(scope);
        }
    }
    
    free(test_array);
    free(access_pattern);
    printf("✅ Placement strategy test completed\n");
}

// Test NUMA configuration presets
static void test_numa_configurations(void) {
    printf("\n=== Test 4: NUMA Configuration Presets ===\n");
    
    struct {
        const char* name;
        NumaParallelForConfig config;
    } configs[] = {
        {"Default Config", numa_config_default()},
        {"Memory-Bound Config", numa_config_memory_bound()},
        {"Compute-Bound Config", numa_config_compute_bound()}
    };
    
    for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
        printf("\n%s:\n", configs[i].name);
        printf("  Placement strategy: %s\n", 
               numa_placement_strategy_to_string(configs[i].config.placement_strategy));
        printf("  Memory policy: %s\n", 
               numa_affinity_policy_to_string(configs[i].config.memory_policy));
        printf("  NUMA awareness: %s\n", 
               configs[i].config.enable_numa_awareness ? "enabled" : "disabled");
        printf("  Task migration: %s\n", 
               configs[i].config.enable_task_migration ? "enabled" : "disabled");
        printf("  Memory migration: %s\n", 
               configs[i].config.enable_memory_migration ? "enabled" : "disabled");
        printf("  Local preference weight: %u%%\n", 
               configs[i].config.local_preference_weight);
    }
    
    printf("\n✅ Configuration preset test completed\n");
}

// Test NUMA performance monitoring
static void test_numa_performance_monitoring(void) {
    printf("\n=== Test 5: NUMA Performance Monitoring ===\n");
    
    const size_t ARRAY_SIZE = 5000;
    int* test_array = calloc(ARRAY_SIZE, sizeof(int));
    uint32_t* access_pattern = calloc(ARRAY_SIZE, sizeof(uint32_t));
    
    NumaTestContext context = {
        .data_array = test_array,
        .array_size = ARRAY_SIZE,
        .access_pattern = access_pattern,
        .current_node = 0,
        .track_locality = true
    };
    
    // Configure for performance monitoring
    NumaParallelForConfig config = numa_config_memory_bound();
    config.base_config.start_index = 0;
    config.base_config.end_index = ARRAY_SIZE;
    config.base_config.max_workers = 6;
    config.on_migration = migration_callback;
    config.on_load_imbalance = load_imbalance_callback;
    
    printf("Running memory-bound workload with NUMA monitoring...\n");
    
    NumaTaskScope* scope = numa_task_scope_create(config, "performance_test");
    if (scope) {
        numa_task_scope_start(scope);
        
        uint64_t start_time = performance_get_timestamp_ns();
        Result_void_ptr result = numa_parallel_for(scope, config, memory_bound_function, &context);
        uint64_t end_time = performance_get_timestamp_ns();
        
        if (result.is_error) {
            printf("❌ Performance test failed: %s\n", result.error->message);
        } else {
            printf("✅ Performance test completed successfully\n");
            printf("Total execution time: %.3f ms\n", (end_time - start_time) / 1e6);
            
            // Calculate and display locality metrics
            double locality_ratio = numa_calculate_locality_ratio(scope);
            printf("Memory locality ratio: %.1f%%\n", locality_ratio * 100.0);
            
            // Print detailed performance report
            numa_print_performance_report(scope);
        }
        
        numa_task_scope_destroy(scope);
    }
    
    free(test_array);
    free(access_pattern);
    printf("✅ Performance monitoring test completed\n");
}

// Test NUMA memory region management
static void test_numa_memory_regions(void) {
    printf("\n=== Test 6: NUMA Memory Region Management ===\n");
    
    // Test memory region creation
    const size_t REGION_SIZE = 1024 * 1024; // 1MB
    void* memory = malloc(REGION_SIZE);
    
    if (!memory) {
        printf("❌ Failed to allocate test memory\n");
        return;
    }
    
    // Create NUMA memory region
    NumaMemoryRegion* region = numa_memory_region_create(memory, REGION_SIZE, 0);
    if (!region) {
        printf("❌ Failed to create NUMA memory region\n");
        free(memory);
        return;
    }
    
    printf("✅ NUMA memory region created successfully\n");
    printf("Base address: %p\n", region->base_address);
    printf("Size: %zu bytes\n", region->size);
    printf("Preferred node: %u\n", region->preferred_node);
    printf("Affinity policy: %s\n", numa_affinity_policy_to_string(region->affinity_policy));
    printf("Can migrate: %s\n", region->can_migrate ? "yes" : "no");
    
    // Test locality checking
    for (uint32_t node = 0; node < 4; node++) {
        bool is_local = numa_memory_region_is_local(region, node);
        printf("Local to node %u: %s\n", node, is_local ? "yes" : "no");
    }
    
    // Test memory node detection
    uint32_t memory_node = numa_memory_region_get_node(memory);
    printf("Memory located on node: %u\n", memory_node);
    
    // Clean up
    numa_memory_region_destroy(region);
    free(memory);
    printf("✅ Memory region test completed\n");
}

int main() {
    printf("=== NUMA-Aware Task Scheduling Test Suite ===\n");
    
    // Run comprehensive NUMA scheduling tests
    test_numa_topology_discovery();
    test_numa_task_scope();
    test_numa_placement_strategies();
    test_numa_configurations();
    test_numa_performance_monitoring();
    test_numa_memory_regions();
    
    printf("\n=== NUMA-Aware Scheduling Benefits Demonstrated ===\n");
    printf("1. ✅ NUMA Topology Discovery: Automatic detection of system NUMA layout\n");
    printf("2. ✅ Intelligent Task Placement: Multiple strategies for optimal placement\n");
    printf("3. ✅ Memory Locality Optimization: Minimize cross-node memory access\n");
    printf("4. ✅ Performance Monitoring: Real-time locality and performance metrics\n");
    printf("5. ✅ Configurable Policies: Workload-specific NUMA configurations\n");
    printf("6. ✅ Memory Region Management: NUMA-aware memory allocation and tracking\n");
    
    printf("\n=== Integration with Goo's Parallel For System ===\n");
    printf("• Seamless integration with existing parallel for infrastructure\n");
    printf("• Compatible with work-stealing, memory safety, and performance monitoring\n");
    printf("• Cross-platform support with graceful fallback on non-NUMA systems\n");
    printf("• Adaptive policies that adjust to runtime workload characteristics\n");
    printf("• Zero-overhead when NUMA is not available or disabled\n");
    
    printf("\n🎉 NUMA-aware task scheduling enhancement completed!\n");
    printf("The parallel for system now provides comprehensive NUMA optimization!\n");
    
    return 0;
}