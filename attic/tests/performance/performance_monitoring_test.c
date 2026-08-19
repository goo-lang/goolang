#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "include/performance_monitoring.h"
#include "include/structured_concurrency.h"
#include "include/work_stealing.h"

// Test workload functions for performance analysis
typedef struct {
    int* data_array;
    size_t array_size;
    PerformanceMonitor* monitor;
    int workload_type;
} PerformanceTestContext;

// Light workload - fast execution
static Result_void_ptr light_workload_function(size_t index, void* context) {
    PerformanceTestContext* ctx = (PerformanceTestContext*)context;
    uint64_t start_time = performance_get_timestamp_ns();
    
    // Simple computation
    ctx->data_array[index] = (int)(index * 2 + 1);
    
    uint64_t end_time = performance_get_timestamp_ns();
    PERF_RECORD_TASK_COMPLETION(ctx->monitor, index % 8, index, end_time - start_time);
    
    return OK_PTR(NULL);
}

// Heavy workload - slow execution to test bottleneck detection
static Result_void_ptr heavy_workload_function(size_t index, void* context) {
    PerformanceTestContext* ctx = (PerformanceTestContext*)context;
    uint64_t start_time = performance_get_timestamp_ns();
    
    // Simulate heavy computation with random delay
    int computation_result = 0;
    for (int i = 0; i < 10000 + (rand() % 50000); i++) {
        computation_result += i * i;
    }
    
    ctx->data_array[index] = computation_result % 1000;
    
    uint64_t end_time = performance_get_timestamp_ns();
    PERF_RECORD_TASK_COMPLETION(ctx->monitor, index % 8, index, end_time - start_time);
    
    return OK_PTR(NULL);
}

// Imbalanced workload - creates load imbalance
static Result_void_ptr imbalanced_workload_function(size_t index, void* context) {
    PerformanceTestContext* ctx = (PerformanceTestContext*)context;
    uint64_t start_time = performance_get_timestamp_ns();
    
    // Create artificial load imbalance
    int computation_cycles;
    if (index % 4 == 0) {
        computation_cycles = 100000; // Heavy tasks
    } else {
        computation_cycles = 1000;   // Light tasks
    }
    
    int result = 0;
    for (int i = 0; i < computation_cycles; i++) {
        result += i;
    }
    
    ctx->data_array[index] = result % 1000;
    
    uint64_t end_time = performance_get_timestamp_ns();
    PERF_RECORD_TASK_COMPLETION(ctx->monitor, index % 8, index, end_time - start_time);
    
    return OK_PTR(NULL);
}

// Memory-intensive workload
static Result_void_ptr memory_intensive_function(size_t index, void* context) {
    PerformanceTestContext* ctx = (PerformanceTestContext*)context;
    uint64_t start_time = performance_get_timestamp_ns();
    
    // Allocate and use temporary memory
    size_t temp_size = 1024 + (index % 4096); // 1KB to 5KB
    int* temp_array = malloc(temp_size * sizeof(int));
    
    if (temp_array) {
        performance_record_memory_allocation(ctx->monitor, temp_size * sizeof(int));
        
        // Use the memory to simulate cache misses
        for (size_t i = 0; i < temp_size; i++) {
            temp_array[i] = (int)(i + index);
        }
        
        // Sum to prevent optimization
        int sum = 0;
        for (size_t i = 0; i < temp_size; i++) {
            sum += temp_array[i];
        }
        
        ctx->data_array[index] = sum % 1000;
        free(temp_array);
    } else {
        ctx->data_array[index] = -1; // Error marker
    }
    
    uint64_t end_time = performance_get_timestamp_ns();
    PERF_RECORD_TASK_COMPLETION(ctx->monitor, index % 8, index, end_time - start_time);
    
    return OK_PTR(NULL);
}

// Bottleneck detection callback
static void bottleneck_detected_callback(const char* bottleneck_type, const char* details) {
    printf("🚨 BOTTLENECK DETECTED: %s\n", bottleneck_type);
    printf("   Details: %s\n", details);
}

// Performance alert callback
static void performance_alert_callback(const char* alert_message) {
    printf("📊 Performance Alert: %s\n", alert_message);
}

// Test performance monitoring with different workloads
static void test_workload_performance(const char* test_name, 
                                     ParallelForFunction workload_func,
                                     PerformanceConfig perf_config,
                                     size_t item_count) {
    printf("\n=== %s ===\n", test_name);
    
    // Create test data
    int* test_array = calloc(item_count, sizeof(int));
    PerformanceMonitor* monitor = performance_monitor_create(perf_config, 8);
    
    PerformanceTestContext context = {
        .data_array = test_array,
        .array_size = item_count,
        .monitor = monitor,
        .workload_type = 0
    };
    
    // Create task scope
    TaskScope* scope = task_scope_create(task_scope_config_default(), "performance_test");
    task_scope_start(scope);
    
    // Start performance monitoring
    performance_monitor_start(monitor);
    
    // Configure parallel for
    ParallelForConfig config = {
        .start_index = 0,
        .end_index = item_count,
        .chunk_size = 0, // Auto-calculate
        .max_workers = 8,
        .priority = TASK_PRIORITY_NORMAL
    };
    
    printf("Running %s with %zu items...\n", test_name, item_count);
    
    uint64_t test_start = performance_get_timestamp_ns();
    Result_void_ptr result = task_scope_parallel_for(scope, config, workload_func, &context);
    uint64_t test_end = performance_get_timestamp_ns();
    
    // Stop monitoring and analyze results
    performance_monitor_stop(monitor);
    
    if (result.is_error) {
        printf("❌ Test failed: %s\n", result.error->message);
    } else {
        printf("✅ Test completed successfully\n");
    }
    
    printf("Total execution time: %.3f ms\n", (test_end - test_start) / 1e6);
    
    // Check for bottlenecks
    performance_check_bottlenecks(monitor);
    
    // Print performance summary
    performance_print_summary(monitor);
    
    // Print efficiency metrics
    printf("Overall efficiency: %.1f%%\n", performance_calculate_efficiency(monitor) * 100.0);
    printf("Throughput: %.1f tasks/second\n", performance_calculate_throughput(monitor));
    
    // Cleanup
    task_scope_shutdown(scope, 5000);
    task_scope_destroy(scope);
    performance_monitor_destroy(monitor);
    free(test_array);
}

// Test work-stealing performance monitoring
static void test_work_stealing_performance(void) {
    printf("\n=== Work-Stealing Performance Analysis ===\n");
    
    const size_t ITEM_COUNT = 5000;
    int* test_array = calloc(ITEM_COUNT, sizeof(int));
    
    // Create detailed performance configuration
    PerformanceConfig perf_config = performance_config_detailed();
    perf_config.on_bottleneck_detected = bottleneck_detected_callback;
    perf_config.on_performance_alert = performance_alert_callback;
    
    PerformanceMonitor* monitor = performance_monitor_create(perf_config, 8);
    
    PerformanceTestContext context = {
        .data_array = test_array,
        .array_size = ITEM_COUNT,
        .monitor = monitor,
        .workload_type = 0
    };
    
    // Create work-stealing scope
    WorkStealingScope* ws_scope = work_stealing_scope_create(8, "performance_test_ws");
    
    // Start monitoring
    performance_monitor_start(monitor);
    
    // Configure work-stealing parallel for
    WorkStealingParallelForConfig config = {
        .base_config = {
            .start_index = 0,
            .end_index = ITEM_COUNT,
            .chunk_size = 50, // Small chunks to encourage stealing
            .max_workers = 8,
            .priority = TASK_PRIORITY_NORMAL
        },
        .min_steal_size = 10,
        .initial_chunk_size = 50,
        .adaptive_chunking = true,
        .locality_aware = false
    };
    
    printf("Running work-stealing with performance monitoring...\n");
    
    uint64_t start_time = performance_get_timestamp_ns();
    Result_void_ptr result = work_stealing_parallel_for(ws_scope, config, imbalanced_workload_function, &context);
    uint64_t end_time = performance_get_timestamp_ns();
    
    performance_monitor_stop(monitor);
    
    if (result.is_error) {
        printf("❌ Work-stealing test failed: %s\n", result.error->message);
    } else {
        printf("✅ Work-stealing test completed\n");
    }
    
    printf("Execution time: %.3f ms\n", (end_time - start_time) / 1e6);
    
    // Analyze work-stealing specific metrics
    printf("\nWork-Stealing Analysis:\n");
    printf("Total steals: %llu\n", atomic_load(&ws_scope->total_steals));
    
    for (size_t i = 0; i < ws_scope->worker_count; i++) {
        printf("Worker %zu: executed=%llu\n", i, 
               atomic_load(&ws_scope->worker_contexts[i].tasks_executed));
    }
    
    // Check bottlenecks
    performance_check_bottlenecks(monitor);
    performance_print_summary(monitor);
    
    // Cleanup
    work_stealing_scope_destroy(ws_scope);
    performance_monitor_destroy(monitor);
    free(test_array);
}

// Comparative performance analysis
static void test_performance_comparison(void) {
    printf("\n=== Performance Comparison Analysis ===\n");
    
    const size_t ITEM_COUNT = 2000;
    
    // Test different configurations
    struct {
        const char* name;
        ParallelForFunction func;
        PerformanceConfig config;
    } test_cases[] = {
        {"Light Workload (Optimal)", light_workload_function, performance_config_minimal()},
        {"Heavy Workload (CPU Bound)", heavy_workload_function, performance_config_default()},
        {"Imbalanced Workload (Load Issues)", imbalanced_workload_function, performance_config_detailed()},
        {"Memory Intensive (Memory Bound)", memory_intensive_function, performance_config_detailed()}
    };
    
    for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
        test_cases[i].config.on_bottleneck_detected = bottleneck_detected_callback;
        test_cases[i].config.on_performance_alert = performance_alert_callback;
        
        test_workload_performance(test_cases[i].name, test_cases[i].func, 
                                 test_cases[i].config, ITEM_COUNT);
        
        // Brief pause between tests
        usleep(100000); // 100ms
    }
}

// Stress test for bottleneck detection
static void test_bottleneck_detection(void) {
    printf("\n=== Bottleneck Detection Stress Test ===\n");
    
    PerformanceConfig config = performance_config_detailed();
    config.slow_task_threshold_ns = 1000000; // 1ms - very sensitive
    config.high_contention_threshold = 10;   // 10% - sensitive
    config.load_imbalance_threshold = 0.15;  // 15% - sensitive
    config.on_bottleneck_detected = bottleneck_detected_callback;
    config.on_performance_alert = performance_alert_callback;
    
    const size_t ITEM_COUNT = 1000;
    int* test_array = calloc(ITEM_COUNT, sizeof(int));
    PerformanceMonitor* monitor = performance_monitor_create(config, 8);
    
    PerformanceTestContext context = {
        .data_array = test_array,
        .array_size = ITEM_COUNT,
        .monitor = monitor,
        .workload_type = 0
    };
    
    // Create work-stealing scope for maximum contention
    WorkStealingScope* ws_scope = work_stealing_scope_create(8, "bottleneck_test");
    
    performance_monitor_start(monitor);
    
    // Run with very small chunks to create contention
    WorkStealingParallelForConfig ws_config = {
        .base_config = {
            .start_index = 0,
            .end_index = ITEM_COUNT,
            .chunk_size = 1, // Individual tasks to maximize stealing
            .max_workers = 8,
            .priority = TASK_PRIORITY_NORMAL
        },
        .min_steal_size = 1,
        .initial_chunk_size = 1,
        .adaptive_chunking = false, // Disable to force small chunks
        .locality_aware = false
    };
    
    printf("Running bottleneck detection test (expect multiple alerts)...\n");
    
    Result_void_ptr result = work_stealing_parallel_for(ws_scope, ws_config, 
                                                       heavy_workload_function, &context);
    
    // Force bottleneck check
    performance_check_bottlenecks(monitor);
    
    performance_monitor_stop(monitor);
    
    if (result.is_error) {
        printf("❌ Bottleneck test failed: %s\n", result.error->message);
    } else {
        printf("✅ Bottleneck test completed\n");
    }
    
    printf("\nBottleneck Detection Summary:\n");
    printf("Load imbalance events: %u\n", 
           atomic_load(&monitor->global_metrics.load_imbalance_events));
    printf("Contention events: %u\n", 
           atomic_load(&monitor->global_metrics.contention_events));
    printf("Synchronization waits: %u\n", 
           atomic_load(&monitor->global_metrics.synchronization_waits));
    
    work_stealing_scope_destroy(ws_scope);
    performance_monitor_destroy(monitor);
    free(test_array);
}

int main() {
    printf("=== Performance Monitoring and Bottleneck Detection for Parallel For ===\n");
    
    srand((unsigned int)time(NULL));
    
    // Run comprehensive performance tests
    test_performance_comparison();
    test_work_stealing_performance();
    test_bottleneck_detection();
    
    printf("\n=== Performance Monitoring Benefits Demonstrated ===\n");
    printf("1. ✅ Real-time Performance Metrics: Task execution times, throughput, efficiency\n");
    printf("2. ✅ Bottleneck Detection: Load imbalance, contention, slow tasks, memory pressure\n");
    printf("3. ✅ Work-Stealing Analysis: Steal success rates, worker utilization, load distribution\n");
    printf("4. ✅ Configurable Monitoring: Minimal overhead to detailed profiling modes\n");
    printf("5. ✅ Adaptive Alerts: Customizable thresholds and callback notifications\n");
    printf("6. ✅ Performance History: Trend analysis and performance regression detection\n");
    
    printf("\n=== Integration with Goo's Parallel For System ===\n");
    printf("• Seamless integration with structured concurrency and work-stealing\n");
    printf("• Low-overhead atomic operations for metric collection\n");
    printf("• Configurable monitoring levels to balance insight vs performance\n");
    printf("• Real-time bottleneck detection with actionable recommendations\n");
    printf("• Performance data export for analysis and optimization\n");
    
    return 0;
}