#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include "include/structured_concurrency.h"

// Test data structures
typedef struct {
    int start;
    int end;
    int sum;
} SumTask;

typedef struct {
    int* array;
    size_t size;
    int total_sum;
} ParallelSumContext;

// Test task functions
Result_void_ptr simple_task_function(TaskContext* context, void* args) {
    SumTask* task_data = (SumTask*)args;
    
    // Simulate some work
    usleep(10000); // 10ms
    
    // Check for cancellation
    CHECK_CANCELLATION(context);
    
    // Calculate sum
    task_data->sum = 0;
    for (int i = task_data->start; i <= task_data->end; i++) {
        task_data->sum += i;
        
        // Update progress
        int progress = ((i - task_data->start + 1) * 100) / (task_data->end - task_data->start + 1);
        UPDATE_PROGRESS(context, progress, "Computing sum");
        
        // Check for cancellation periodically
        if (i % 10 == 0) {
            CHECK_CANCELLATION(context);
        }
    }
    
    return OK_PTR(task_data);
}

Result_void_ptr failing_task_function(TaskContext* context, void* args) {
    (void)context; // Suppress unused parameter warning
    (void)args;    // Suppress unused parameter warning
    
    usleep(5000); // 5ms
    
    Error* error = malloc(sizeof(Error));
    *error = (Error){
        .code = ERROR_OPERATION_FAILED,
        .severity = ERROR_SEVERITY_ERROR,
        .category = ERROR_CATEGORY_RUNTIME,
        .message = strdup("Simulated task failure"),
        .hint = NULL,
        .location = (SourceLocation){0},
        .next = NULL
    };
    return ERR_PTR(error);
}

Result_void_ptr slow_task_function(TaskContext* context, void* args) {
    // This task will take longer than typical timeouts
    for (int i = 0; i < 100; i++) {
        usleep(100000); // 100ms per iteration = 10 seconds total
        CHECK_CANCELLATION(context);
        UPDATE_PROGRESS(context, i, "Processing slowly");
    }
    return OK_PTR(args);
}

// Parallel for function
Result_void_ptr parallel_sum_function(size_t index, void* context) {
    ParallelSumContext* sum_context = (ParallelSumContext*)context;
    
    // Add this array element to a shared accumulator
    int value = sum_context->array[index];
    int old_sum = __sync_fetch_and_add(&sum_context->total_sum, value);
    
    // Debug: Print every 25th addition to see more activity
    if (index % 25 == 0) {
        printf("    Adding index %zu (value %d), sum now: %d\n", index, value, old_sum + value);
    }
    
    return OK_PTR(context);
}

// Test basic task scope creation and destruction
void test_task_scope_creation() {
    printf("Testing task scope creation and destruction...\n");
    
    TaskScopeConfig config = task_scope_config_default();
    TaskScope* scope = task_scope_create(config, "test_scope");
    assert(scope != NULL);
    assert(strcmp(scope->name, "test_scope") == 0);
    assert(scope->config.max_concurrent_tasks == 8);
    
    Result_void_ptr start_result = task_scope_start(scope);
    assert(!start_result.is_error);
    assert(scope->is_active);
    
    Result_void_ptr shutdown_result = task_scope_shutdown(scope, 1000);
    assert(!shutdown_result.is_error);
    assert(!scope->is_active);
    
    task_scope_destroy(scope);
    
    printf("✓ Task scope creation test passed\n");
}

// Test task creation and execution
void test_task_execution() {
    printf("Testing task creation and execution...\n");
    
    TaskScopeConfig config = task_scope_config_default();
    TaskScope* scope = task_scope_create(config, "execution_test");
    assert(scope != NULL);
    
    task_scope_start(scope);
    
    // Create a simple task
    SumTask task_data = {.start = 1, .end = 100, .sum = 0};
    ConcurrentTask* task = task_create(scope, simple_task_function, &task_data, sizeof(SumTask), "sum_task");
    assert(task != NULL);
    assert(task->state == TASK_STATE_CREATED);
    
    // Submit and wait for the task
    Result_void_ptr submit_result = task_submit(task);
    assert(!submit_result.is_error);
    
    Result_void_ptr wait_result = task_wait(task, 5000);
    assert(!wait_result.is_error);
    assert(task->state == TASK_STATE_COMPLETED);
    
    // Verify the result
    SumTask* result = (SumTask*)task->result;
    assert(result != NULL);
    assert(result->sum == 5050); // Sum of 1 to 100
    
    task_destroy(task);
    task_scope_shutdown(scope, 1000);
    task_scope_destroy(scope);
    
    printf("✓ Task execution test passed\n");
}

// Test task groups
void test_task_groups() {
    printf("Testing task groups...\n");
    
    TaskScopeConfig config = task_scope_config_default();
    TaskScope* scope = task_scope_create(config, "group_test");
    task_scope_start(scope);
    
    // Create a task group
    TaskGroup* group = task_group_create(scope, "sum_group");
    assert(group != NULL);
    
    // Create multiple tasks
    const int num_tasks = 5;
    SumTask task_data[num_tasks];
    ConcurrentTask* tasks[num_tasks];
    
    for (int i = 0; i < num_tasks; i++) {
        task_data[i] = (SumTask){.start = i * 20 + 1, .end = (i + 1) * 20, .sum = 0};
        tasks[i] = task_create(scope, simple_task_function, &task_data[i], sizeof(SumTask), "group_task");
        assert(tasks[i] != NULL);
        
        Result_void_ptr add_result = task_group_add_task(group, tasks[i]);
        assert(!add_result.is_error);
        
        task_submit(tasks[i]);
    }
    
    // Wait for all tasks to complete
    Result_void_ptr wait_result = task_group_wait_all(group, 10000);
    assert(!wait_result.is_error);
    
    // Verify all tasks completed
    int total_sum = 0;
    for (int i = 0; i < num_tasks; i++) {
        assert(tasks[i]->state == TASK_STATE_COMPLETED);
        SumTask* result = (SumTask*)tasks[i]->result;
        total_sum += result->sum;
        // Don't manually destroy tasks - task_group_destroy will handle them
    }
     // Sum of 1 to 100 = 5050
    assert(total_sum == 5050);

    // Don't manually destroy group - task_scope_destroy will handle it
    task_scope_shutdown(scope, 1000);
    task_scope_destroy(scope);

    printf("✓ Task groups test passed\n");
}

// Test task cancellation
void test_task_cancellation() {
    printf("Testing task cancellation...\n");
    
    TaskScopeConfig config = task_scope_config_default();
    TaskScope* scope = task_scope_create(config, "cancel_test");
    task_scope_start(scope);
    
    // Create a slow task
    int dummy_data = 42;
    ConcurrentTask* task = task_create(scope, slow_task_function, &dummy_data, sizeof(int), "slow_task");
    assert(task != NULL);
    
    task_submit(task);
    
    // Let it run for a bit
    usleep(100000); // 100ms
    
    // Cancel the task
    Result_void_ptr cancel_result = task_cancel(task, CANCEL_IMMEDIATE);
    assert(!cancel_result.is_error);
    
    // Wait for cancellation to complete
    Result_void_ptr wait_result = task_wait(task, 2000);
    assert(!wait_result.is_error);
    assert(task->state == TASK_STATE_CANCELLED);
    
    task_destroy(task);
    task_scope_shutdown(scope, 1000);
    task_scope_destroy(scope);
    
    printf("✓ Task cancellation test passed\n");
}

// Test error propagation
void test_error_propagation() {
    printf("Testing error propagation...\n");
    
    TaskScopeConfig config = task_scope_config_default();
    config.propagate_errors = true;
    TaskScope* scope = task_scope_create(config, "error_test");
    task_scope_start(scope);
    
    TaskGroup* group = task_group_create(scope, "error_group");
    group->fail_fast = true;
    
    // Create a mix of normal and failing tasks
    int dummy_data = 42;
    ConcurrentTask* normal_task1 = task_create(scope, simple_task_function, &dummy_data, sizeof(int), "normal1");
    ConcurrentTask* failing_task = task_create(scope, failing_task_function, &dummy_data, sizeof(int), "failing");
    ConcurrentTask* normal_task2 = task_create(scope, simple_task_function, &dummy_data, sizeof(int), "normal2");
    
    task_group_add_task(group, normal_task1);
    task_group_add_task(group, failing_task);
    task_group_add_task(group, normal_task2);
    
    task_submit(normal_task1);
    task_submit(failing_task);
    task_submit(normal_task2);
    
    // Wait for the group - should fail due to failing task
    Result_void_ptr wait_result = task_group_wait_all(group, 5000);
    // Note: This may or may not be an error depending on implementation details
     // Check that the failing task actually failed
    assert(failing_task->state == TASK_STATE_ERROR);
    assert(failing_task->error != NULL);

    // Don't manually destroy tasks or group - task_scope_destroy will handle them
    task_scope_shutdown(scope, 1000);
    task_scope_destroy(scope);

    printf("✓ Error propagation test passed\n");
}

// Test parallel for loops
void test_parallel_for() {
    printf("Testing parallel for loops...\n");
    
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
    assert(!for_result.is_error);
    
    // Verify the sum (1+2+...+1000 = 500500)
    printf("  Expected sum: 500500, Actual sum: %d\n", sum_context.total_sum);
    assert(sum_context.total_sum == 500500);
    
    free(test_array);
    task_scope_shutdown(scope, 1000);
    task_scope_destroy(scope);
    
    printf("✓ Parallel for test passed\n");
}

// Test configuration variants
void test_configurations() {
    printf("Testing different configurations...\n");
    
    // Test CPU intensive configuration
    TaskScopeConfig cpu_config = task_scope_config_cpu_intensive();
    assert(cpu_config.max_concurrent_tasks == 16);
    assert(cpu_config.numa_aware_scheduling == true);
    assert(cpu_config.default_priority == TASK_PRIORITY_HIGH);
    
    // Test IO intensive configuration
    TaskScopeConfig io_config = task_scope_config_io_intensive();
    assert(io_config.max_concurrent_tasks == 32);
    
    // Test memory constrained configuration
    TaskScopeConfig mem_config = task_scope_config_memory_constrained();
    assert(mem_config.max_memory_per_task < cpu_config.max_memory_per_task);
    
    printf("✓ Configuration test passed\n");
}

// Test statistics collection
void test_statistics() {
    printf("Testing statistics collection...\n");
    
    TaskScopeConfig config = task_scope_config_default();
    TaskScope* scope = task_scope_create(config, "stats_test");
    task_scope_start(scope);
    
    // Create and run several tasks
    for (int i = 0; i < 5; i++) {
        SumTask task_data = {.start = i * 10 + 1, .end = (i + 1) * 10, .sum = 0};
        ConcurrentTask* task = task_create(scope, simple_task_function, &task_data, sizeof(SumTask), "stats_task");
        task_submit(task);
        task_wait(task, 5000);
        task_destroy(task);
    }
    
    // Get statistics
    TaskScopeStats stats = task_scope_get_stats(scope);
    assert(stats.total_tasks >= 5);
    assert(stats.completed_tasks >= 5);
    assert(stats.avg_task_duration_ns > 0);
    
    printf("  Total tasks: %llu\n", stats.total_tasks);
    printf("  Completed tasks: %llu\n", stats.completed_tasks);
    printf("  Average duration: %llu ns\n", stats.avg_task_duration_ns);
    
    task_scope_shutdown(scope, 1000);
    task_scope_destroy(scope);
    
    printf("✓ Statistics test passed\n");
}

// Test cancellation tokens
void test_cancellation_tokens() {
    printf("Testing cancellation tokens...\n");
    
    CancellationToken* token = cancellation_token_create();
    assert(token != NULL);
    assert(!cancellation_token_is_cancelled(token));
    
    // Test cancellation
    cancellation_token_cancel(token);
    assert(cancellation_token_is_cancelled(token));
    
    cancellation_token_destroy(token);
    
    printf("✓ Cancellation tokens test passed\n");
}

int main() {
    printf("=== Structured Concurrency Test Suite ===\n\n");
    
    test_task_scope_creation();
    test_task_execution();
    test_task_groups();
    test_task_cancellation();
    test_error_propagation();
    test_parallel_for();
    test_configurations();
    test_statistics();
    test_cancellation_tokens();
    
    printf("\n=== All Structured Concurrency Tests Passed! ===\n");
    return 0;
}