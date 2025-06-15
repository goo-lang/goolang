#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include "include/work_stealing.h"

// Forward declaration of parallel_for_task_wrapper
extern Result_void_ptr parallel_for_task_wrapper(TaskContext* task_ctx, void* args);

// Simulate variable workload - some iterations take longer
typedef struct {
    atomic_int* sum;
    atomic_int* heavy_count;
    atomic_int* light_count;
} VariableWorkContext;

static double simulate_work(size_t index) {
    // Some indices have heavy computation
    if (index % 7 == 0 || index % 11 == 0) {
        // Heavy work - compute expensive function
        double result = 0.0;
        for (int i = 0; i < 10000; i++) {
            result += sin(index * 0.001 + i * 0.0001);
            result += cos(index * 0.002 - i * 0.0002);
        }
        return result;
    } else {
        // Light work
        return index * 2.5;
    }
}

static Result_void_ptr variable_work_function(size_t index, void* context) {
    VariableWorkContext* ctx = (VariableWorkContext*)context;
    
    // Simulate work - discard result since we're just testing scheduling
    (void)simulate_work(index);
    
    // Track workload distribution
    if (index % 7 == 0 || index % 11 == 0) {
        atomic_fetch_add(ctx->heavy_count, 1);
    } else {
        atomic_fetch_add(ctx->light_count, 1);
    }
    
    // Add to sum (simplified - just use index)
    atomic_fetch_add(ctx->sum, (int)(index + 1));
    
    return OK_PTR(NULL);
}

static void print_worker_stats(WorkStealingScope* scope) {
    printf("\n=== Work-Stealing Statistics ===\n");
    printf("Total steals: %llu\n", atomic_load(&scope->total_steals));
    printf("Total overflow operations: %llu\n", atomic_load(&scope->total_overflow_ops));
    
    printf("\nPer-Worker Statistics:\n");
    for (size_t i = 0; i < scope->worker_count; i++) {
        WorkerContext* worker = &scope->worker_contexts[i];
        printf("Worker %zu:\n", i);
        printf("  Tasks executed: %llu\n", atomic_load(&worker->tasks_executed));
        printf("  Tasks stolen: %llu\n", atomic_load(&worker->tasks_stolen));
        printf("  Steal attempts: %llu\n", atomic_load(&worker->steal_attempts));
        printf("  Failed steals: %llu\n", atomic_load(&worker->failed_steals));
        
        double steal_success_rate = 0.0;
        uint64_t attempts = atomic_load(&worker->steal_attempts);
        if (attempts > 0) {
            steal_success_rate = (double)atomic_load(&worker->tasks_stolen) / attempts * 100.0;
        }
        printf("  Steal success rate: %.1f%%\n", steal_success_rate);
    }
}

int main() {
    printf("=== Work-Stealing Parallel For Test ===\n");
    
    // Get CPU count
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    printf("System has %ld CPUs\n", cpu_count);
    
    // Create work-stealing scope
    size_t worker_count = cpu_count;
    WorkStealingScope* ws_scope = work_stealing_scope_create(worker_count, "work_stealing_test");
    if (!ws_scope) {
        fprintf(stderr, "Failed to create work-stealing scope\n");
        return 1;
    }
    
    printf("Created work-stealing scope with %zu workers\n", worker_count);
    
    // Test with variable workload
    const size_t ITEMS = 10000;
    atomic_int sum = 0;
    atomic_int heavy_count = 0;
    atomic_int light_count = 0;
    
    VariableWorkContext context = {
        .sum = &sum,
        .heavy_count = &heavy_count,
        .light_count = &light_count
    };
    
    WorkStealingParallelForConfig config = {
        .base_config = {
            .start_index = 0,
            .end_index = ITEMS,
            .chunk_size = 0,  // Auto-determine
            .max_workers = worker_count,
            .priority = TASK_PRIORITY_NORMAL
        },
        .min_steal_size = 10,
        .initial_chunk_size = 0,  // Auto-determine
        .adaptive_chunking = true,
        .locality_aware = false
    };
    
    printf("\nRunning parallel for with %zu items (variable workload)...\n", ITEMS);
    
    clock_t start_time = clock();
    
    Result_void_ptr result = work_stealing_parallel_for(
        ws_scope, config, variable_work_function, &context
    );
    
    clock_t end_time = clock();
    double cpu_time_used = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    
    if (result.is_error) {
        fprintf(stderr, "Parallel for failed: %s\n", result.error->message);
        work_stealing_scope_destroy(ws_scope);
        return 1;
    }
    
    // Verify results
    int expected_sum = ITEMS * (ITEMS + 1) / 2;
    int actual_sum = atomic_load(&sum);
    
    printf("\n=== Results ===\n");
    printf("Expected sum: %d\n", expected_sum);
    printf("Actual sum: %d\n", actual_sum);
    printf("Heavy tasks: %d\n", atomic_load(&heavy_count));
    printf("Light tasks: %d\n", atomic_load(&light_count));
    printf("Execution time: %.3f seconds\n", cpu_time_used);
    
    if (actual_sum == expected_sum) {
        printf("✓ Test PASSED - sum is correct\n");
    } else {
        printf("✗ Test FAILED - sum mismatch\n");
    }
    
    // Print work-stealing statistics
    print_worker_stats(ws_scope);
    
    // Test 2: Highly imbalanced workload
    printf("\n\n=== Test 2: Highly Imbalanced Workload ===\n");
    
    atomic_store(&sum, 0);
    atomic_store(&heavy_count, 0);
    atomic_store(&light_count, 0);
    
    // Make chunk size smaller to test more stealing
    config.initial_chunk_size = 50;
    
    start_time = clock();
    
    result = work_stealing_parallel_for(
        ws_scope, config, variable_work_function, &context
    );
    
    end_time = clock();
    cpu_time_used = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    
    if (result.is_error) {
        fprintf(stderr, "Parallel for failed: %s\n", result.error->message);
    } else {
        actual_sum = atomic_load(&sum);
        printf("Sum: %d (expected %d)\n", actual_sum, expected_sum);
        printf("Execution time: %.3f seconds\n", cpu_time_used);
        
        if (actual_sum == expected_sum) {
            printf("✓ Test 2 PASSED\n");
        } else {
            printf("✗ Test 2 FAILED\n");
        }
    }
    
    print_worker_stats(ws_scope);
    
    // Cleanup
    work_stealing_scope_destroy(ws_scope);
    
    printf("\n=== All tests completed ===\n");
    
    return 0;
}