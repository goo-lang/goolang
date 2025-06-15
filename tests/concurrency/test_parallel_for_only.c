#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdatomic.h>
#include "structured_concurrency.h"

// Test data structure for parallel sum
typedef struct {
    int* array;
    size_t size;
    atomic_int total_sum;
} ParallelSumContext;

// Function to add array element to total sum
Result_void_ptr parallel_sum_function(size_t index, void* context) {
    ParallelSumContext* sum_ctx = (ParallelSumContext*)context;
    
    if (index >= sum_ctx->size) {
        return OK_PTR(NULL);
    }
    
    int value = sum_ctx->array[index];
    int old_sum = atomic_fetch_add(&sum_ctx->total_sum, value);
    
    // Debug output for verification
    printf("    Adding index %zu (value %d), sum now: %d\n", index, value, old_sum + value);
    
    return OK_PTR(NULL);
}

// Test parallel for loops only
void test_parallel_for_only() {
    printf("Testing parallel for loops only...\n");
    
    TaskScopeConfig config = task_scope_config_cpu_intensive();
    TaskScope* scope = task_scope_create(config, "parallel_for_test");
    task_scope_start(scope);
    
    // Create test data
    const size_t array_size = 1000;
    int* test_array = malloc(array_size * sizeof(int));
    for (size_t i = 0; i < array_size; i++) {
        test_array[i] = i + 1; // 1, 2, 3, ..., 1000
    }
    
    ParallelSumContext sum_context = {
        .array = test_array,
        .size = array_size,
        .total_sum = 0
    };
    
    // Execute parallel for loop
    ParallelForConfig for_config = {
        .start_index = 0,
        .end_index = array_size,
        .chunk_size = 100,
        .max_workers = 4,
        .priority = TASK_PRIORITY_NORMAL
    };
    
    Result_void_ptr for_result = task_scope_parallel_for(scope, for_config, parallel_sum_function, &sum_context);
    if (for_result.is_error) {
        printf("  Error in parallel for: %s\n", for_result.error->message);
        assert(false);
    }
    
    // Verify the sum (1+2+...+1000 = 500500)
    printf("  Expected sum: 500500, Actual sum: %d\n", sum_context.total_sum);
    assert(sum_context.total_sum == 500500);
    
    printf("✓ Parallel for test passed\n");
    
    free(test_array);
    task_scope_shutdown(scope, 1000);
    task_scope_destroy(scope);
}

int main() {
    printf("=== Parallel For Only Test ===\n");
    test_parallel_for_only();
    printf("All tests passed!\n");
    return 0;
}
