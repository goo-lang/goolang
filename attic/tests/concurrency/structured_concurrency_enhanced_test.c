#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include "include/structured_concurrency_enhanced.h"

// Test context structures
typedef struct TestContext {
    int value;
    char message[256];
    bool should_fail;
    uint64_t sleep_duration_ms;
    int* shared_counter;
} TestContext;

// Simple async function for testing
Result_void_ptr simple_async_function(void* args, AsyncContext* async_ctx) {
    TestContext* ctx = (TestContext*)args;
    
    printf("  Async function called with value: %d\n", ctx->value);
    
    // Check for cancellation
    if (async_ctx && atomic_load(async_ctx->is_cancelled)) {
        return ERR_PTR(error_create(ERROR_OPERATION_CANCELLED, "Operation was cancelled"));
    }
    
    // Simulate work with sleep
    if (ctx->sleep_duration_ms > 0) {
        usleep(ctx->sleep_duration_ms * 1000);
    }
    
    // Check for cancellation after work
    if (async_ctx && atomic_load(async_ctx->is_cancelled)) {
        return ERR_PTR(error_create(ERROR_OPERATION_CANCELLED, "Operation was cancelled"));
    }
    
    if (ctx->should_fail) {
        return ERR_PTR(error_create(ERROR_OPERATION_FAILED, "Simulated failure"));
    }
    
    ctx->value *= 2;
    if (ctx->shared_counter) {
        (*ctx->shared_counter)++;
    }
    
    printf("  Async function completed with value: %d\n", ctx->value);
    return OK_PTR(ctx);
}

// CPU-intensive async function
Result_void_ptr cpu_intensive_function(void* args, AsyncContext* async_ctx) {
    TestContext* ctx = (TestContext*)args;
    
    printf("  CPU-intensive function called with value: %d\n", ctx->value);
    
    // Simulate CPU-intensive work with periodic cancellation checks
    for (int i = 0; i < 1000; i++) {
        if (async_ctx && atomic_load(async_ctx->is_cancelled)) {
            return ERR_PTR(error_create(ERROR_OPERATION_CANCELLED, "Operation was cancelled"));
        }
        
        ctx->value = (ctx->value * 31 + 17) % 10000;
        
        if (i % 100 == 0) {
            usleep(1000); // 1ms pause every 100 iterations
        }
    }
    
    if (ctx->shared_counter) {
        (*ctx->shared_counter)++;
    }
    
    printf("  CPU-intensive function completed with value: %d\n", ctx->value);
    return OK_PTR(ctx);
}

// I/O-bound async function
Result_void_ptr io_bound_function(void* args, AsyncContext* async_ctx) {
    TestContext* ctx = (TestContext*)args;
    
    printf("  I/O-bound function called with value: %d\n", ctx->value);
    
    // Simulate I/O operations
    for (int i = 0; i < 5; i++) {
        if (async_ctx && atomic_load(async_ctx->is_cancelled)) {
            return ERR_PTR(error_create(ERROR_OPERATION_CANCELLED, "Operation was cancelled"));
        }
        
        usleep(ctx->sleep_duration_ms * 1000 / 5); // Split sleep into chunks
        ctx->value += 10;
    }
    
    if (ctx->shared_counter) {
        (*ctx->shared_counter)++;
    }
    
    printf("  I/O-bound function completed with value: %d\n", ctx->value);
    return OK_PTR(ctx);
}

// The two callbacks below were originally GCC nested functions inside their
// tests. clang — required for the blocks-using concurrency sources this test
// links — does not support nested functions, so they live at file scope.
// cleanup_function reported completion by capturing a local; it now uses a
// file-scope flag instead.
static bool g_cleanup_called = false;

static void cleanup_function(void* resource) {
    printf("Cleaning up resource with value: %d\n", *(int*)resource);
    free(resource);
    g_cleanup_called = true;
}

// for_each_func only uses its parameters (the counter comes via context), so it
// needs no closure.
static Result_void_ptr for_each_func(void* item, size_t index, void* context) {
    int* value = (int*)item;
    int* counter = (int*)context;

    printf("  Processing item %zu with value: %d\n", index, *value);
    (*value) *= 2; // Double the value
    (*counter)++;

    return OK_PTR(item);
}

// Test basic concurrent block execution
void test_basic_concurrent_block(void) {
    printf("\n=== Testing Basic Concurrent Block Execution ===\n");
    
    // Initialize scheduler
    StructuredScheduler* scheduler = structured_scheduler_create(NULL);
    assert(scheduler != NULL);
    
    Result_void_ptr start_result = structured_scheduler_start(scheduler);
    assert(!start_result.is_error);
    
    structured_set_global_scheduler(scheduler);
    
    // Create concurrent block
    ConcurrentBlockConfig config = concurrent_block_config_default();
    config.max_concurrent_tasks = 4;
    config.timeout_ms = 5000;
    
    ConcurrentBlock* block = concurrent_block_create("test_block", config);
    assert(block != NULL);
    
    // Create test contexts
    int shared_counter = 0;
    TestContext contexts[3] = {
        {.value = 10, .should_fail = false, .sleep_duration_ms = 10, .shared_counter = &shared_counter},
        {.value = 20, .should_fail = false, .sleep_duration_ms = 20, .shared_counter = &shared_counter},
        {.value = 30, .should_fail = false, .sleep_duration_ms = 15, .shared_counter = &shared_counter}
    };
    
    // Add expressions to block
    for (int i = 0; i < 3; i++) {
        char expr_name[32];
        snprintf(expr_name, sizeof(expr_name), "expr_%d", i);
        
        ConcurrentExpression* expr = concurrent_expression_create(expr_name, simple_async_function, 
                                                                &contexts[i], sizeof(TestContext));
        assert(expr != NULL);
        
        Result_void_ptr add_result = concurrent_block_add_expression(block, expr);
        assert(!add_result.is_error);
    }
    
    // Execute block
    Result_void_ptr exec_result = concurrent_block_execute(block);
    assert(!exec_result.is_error);
    
    // Wait for completion
    Result_void_ptr wait_result = concurrent_block_wait(block, 10000);
    assert(!wait_result.is_error);
    
    // Check results
    assert(shared_counter == 3);
    printf("All 3 expressions completed successfully, shared counter: %d\n", shared_counter);
    
    // Cleanup
    concurrent_block_destroy(block);
    structured_scheduler_shutdown(scheduler, 5000);
    structured_scheduler_destroy(scheduler);
    
    printf("✓ Basic concurrent block test passed\n");
}

// Test fail-fast error handling
void test_fail_fast_error_handling(void) {
    printf("\n=== Testing Fail-Fast Error Handling ===\n");
    
    // Initialize scheduler
    StructuredScheduler* scheduler = structured_scheduler_create(NULL);
    assert(scheduler != NULL);
    structured_scheduler_start(scheduler);
    structured_set_global_scheduler(scheduler);
    
    // Create fail-fast concurrent block
    ConcurrentBlockConfig config = concurrent_block_config_fail_fast();
    config.timeout_ms = 5000;
    
    ConcurrentBlock* block = concurrent_block_create("fail_fast_block", config);
    assert(block != NULL);
    
    // Create test contexts - one will fail
    int shared_counter = 0;
    TestContext contexts[4] = {
        {.value = 10, .should_fail = false, .sleep_duration_ms = 50, .shared_counter = &shared_counter},
        {.value = 20, .should_fail = true,  .sleep_duration_ms = 10, .shared_counter = &shared_counter}, // This will fail
        {.value = 30, .should_fail = false, .sleep_duration_ms = 100, .shared_counter = &shared_counter}, // Should be cancelled
        {.value = 40, .should_fail = false, .sleep_duration_ms = 100, .shared_counter = &shared_counter}  // Should be cancelled
    };
    
    // Add expressions to block
    for (int i = 0; i < 4; i++) {
        char expr_name[32];
        snprintf(expr_name, sizeof(expr_name), "expr_%d", i);
        
        ConcurrentExpression* expr = concurrent_expression_create(expr_name, simple_async_function, 
                                                                &contexts[i], sizeof(TestContext));
        assert(expr != NULL);
        concurrent_block_add_expression(block, expr);
    }
    
    // Execute block
    concurrent_block_execute(block);
    
    // Wait for completion - should get error due to fail-fast
    Result_void_ptr wait_result = concurrent_block_wait(block, 10000);
    assert(wait_result.is_error);
    
    printf("Fail-fast correctly triggered error: %s\n", wait_result.error->message);
    printf("Shared counter: %d (should be less than 4 due to cancellation)\n", shared_counter);
    
    // Cleanup
    concurrent_block_destroy(block);
    structured_scheduler_shutdown(scheduler, 5000);
    structured_scheduler_destroy(scheduler);
    
    printf("✓ Fail-fast error handling test passed\n");
}

// Test timeout handling
void test_timeout_handling(void) {
    printf("\n=== Testing Timeout Handling ===\n");
    
    // Initialize scheduler
    StructuredScheduler* scheduler = structured_scheduler_create(NULL);
    assert(scheduler != NULL);
    structured_scheduler_start(scheduler);
    structured_set_global_scheduler(scheduler);
    
    // Create concurrent block with short timeout
    ConcurrentBlockConfig config = concurrent_block_config_default();
    config.timeout_ms = 100; // Very short timeout
    
    ConcurrentBlock* block = concurrent_block_create("timeout_block", config);
    assert(block != NULL);
    
    // Create test context with long sleep
    TestContext context = {
        .value = 10, 
        .should_fail = false, 
        .sleep_duration_ms = 500, // Longer than timeout
        .shared_counter = NULL
    };
    
    ConcurrentExpression* expr = concurrent_expression_create("slow_expr", simple_async_function, 
                                                            &context, sizeof(TestContext));
    assert(expr != NULL);
    concurrent_block_add_expression(block, expr);
    
    // Execute block
    concurrent_block_execute(block);
    
    // Wait for completion - should timeout
    Result_void_ptr wait_result = concurrent_block_wait(block, config.timeout_ms);
    assert(wait_result.is_error);
    
    printf("Timeout correctly triggered: %s\n", wait_result.error->message);
    
    // Cleanup
    concurrent_block_destroy(block);
    structured_scheduler_shutdown(scheduler, 5000);
    structured_scheduler_destroy(scheduler);
    
    printf("✓ Timeout handling test passed\n");
}

// Test timeout decorator
void test_timeout_decorator(void) {
    printf("\n=== Testing Timeout Decorator ===\n");
    
    // Initialize transparent execution system
    transparent_execution_init();
    
    // Register a slow function
    FunctionRegistry* registry = function_registry_global();
    function_registry_register(registry, "slow_function", NULL, simple_async_function, FUNC_TYPE_ASYNC_NATIVE);
    
    TransparentFunction* slow_func = function_registry_find(registry, "slow_function");
    assert(slow_func != NULL);
    
    // Create timeout decorator
    TimeoutDecorator* decorator = timeout_decorator_create(50, slow_func); // 50ms timeout
    assert(decorator != NULL);
    
    // Test with fast operation (should succeed)
    TestContext fast_context = {.value = 10, .should_fail = false, .sleep_duration_ms = 10};
    Result_void_ptr fast_result = timeout_decorator_execute(decorator, &fast_context, sizeof(TestContext));
    printf("Fast operation result: %s\n", fast_result.is_error ? "ERROR" : "SUCCESS");
    
    // Test with slow operation (should timeout)
    TestContext slow_context = {.value = 20, .should_fail = false, .sleep_duration_ms = 200};
    Result_void_ptr slow_result = timeout_decorator_execute(decorator, &slow_context, sizeof(TestContext));
    printf("Slow operation result: %s\n", slow_result.is_error ? "TIMEOUT" : "SUCCESS");
    assert(slow_result.is_error);
    
    // Cleanup
    timeout_decorator_destroy(decorator);
    transparent_execution_shutdown();
    
    printf("✓ Timeout decorator test passed\n");
}

// Test retry decorator
void test_retry_decorator(void) {
    printf("\n=== Testing Retry Decorator ===\n");
    
    // Initialize transparent execution system
    transparent_execution_init();
    
    // Register a function that sometimes fails
    FunctionRegistry* registry = function_registry_global();
    function_registry_register(registry, "flaky_function", NULL, simple_async_function, FUNC_TYPE_ASYNC_NATIVE);
    
    TransparentFunction* flaky_func = function_registry_find(registry, "flaky_function");
    assert(flaky_func != NULL);
    
    // Create retry decorator
    RetryDecorator* decorator = retry_decorator_create(3, 10, flaky_func); // 3 attempts, 10ms delay
    assert(decorator != NULL);
    
    // Test with function that always fails
    TestContext fail_context = {.value = 10, .should_fail = true, .sleep_duration_ms = 5};
    Result_void_ptr fail_result = retry_decorator_execute(decorator, &fail_context, sizeof(TestContext));
    printf("Always-failing operation result: %s (retries: %llu)\n", 
           fail_result.is_error ? "FAILED" : "SUCCESS", decorator->retry_count);
    assert(fail_result.is_error);
    assert(decorator->retry_count > 0);
    
    // Test with function that succeeds
    TestContext success_context = {.value = 20, .should_fail = false, .sleep_duration_ms = 5};
    Result_void_ptr success_result = retry_decorator_execute(decorator, &success_context, sizeof(TestContext));
    printf("Successful operation result: %s\n", success_result.is_error ? "FAILED" : "SUCCESS");
    assert(!success_result.is_error);
    
    // Cleanup
    retry_decorator_destroy(decorator);
    transparent_execution_shutdown();
    
    printf("✓ Retry decorator test passed\n");
}

// Test dependency handling
void test_dependency_handling(void) {
    printf("\n=== Testing Dependency Handling ===\n");
    
    // Initialize scheduler
    StructuredScheduler* scheduler = structured_scheduler_create(NULL);
    assert(scheduler != NULL);
    structured_scheduler_start(scheduler);
    structured_set_global_scheduler(scheduler);
    
    // Create concurrent block
    ConcurrentBlockConfig config = concurrent_block_config_default();
    ConcurrentBlock* block = concurrent_block_create("dependency_block", config);
    assert(block != NULL);
    
    // Create test contexts
    int shared_counter = 0;
    TestContext contexts[3] = {
        {.value = 10, .should_fail = false, .sleep_duration_ms = 20, .shared_counter = &shared_counter},
        {.value = 20, .should_fail = false, .sleep_duration_ms = 10, .shared_counter = &shared_counter},
        {.value = 30, .should_fail = false, .sleep_duration_ms = 5,  .shared_counter = &shared_counter}
    };
    
    // Create expressions with dependencies: expr2 depends on expr1, expr3 depends on expr2
    ConcurrentExpression* expr1 = concurrent_expression_create("expr1", simple_async_function, 
                                                             &contexts[0], sizeof(TestContext));
    ConcurrentExpression* expr2 = concurrent_expression_create("expr2", simple_async_function, 
                                                             &contexts[1], sizeof(TestContext));
    ConcurrentExpression* expr3 = concurrent_expression_create("expr3", simple_async_function, 
                                                             &contexts[2], sizeof(TestContext));
    
    assert(expr1 && expr2 && expr3);
    
    // Set up dependencies
    concurrent_expression_add_dependency(expr2, expr1);
    concurrent_expression_add_dependency(expr3, expr2);
    
    // Add to block
    concurrent_block_add_expression(block, expr1);
    concurrent_block_add_expression(block, expr2);
    concurrent_block_add_expression(block, expr3);
    
    // Execute and wait
    concurrent_block_execute(block);
    Result_void_ptr wait_result = concurrent_block_wait(block, 10000);
    assert(!wait_result.is_error);
    
    assert(shared_counter == 3);
    printf("All 3 dependent expressions completed successfully\n");
    
    // Cleanup
    concurrent_block_destroy(block);
    structured_scheduler_shutdown(scheduler, 5000);
    structured_scheduler_destroy(scheduler);
    
    printf("✓ Dependency handling test passed\n");
}

// Test resource management
void test_resource_management(void) {
    printf("\n=== Testing Resource Management ===\n");
    
    // Create a test resource
    int* test_resource = malloc(sizeof(int));
    *test_resource = 42;

    g_cleanup_called = false;

    // Create concurrent resource (cleanup_function is defined at file scope)
    ConcurrentResource* resource = concurrent_resource_create(test_resource, cleanup_function);
    assert(resource != NULL);
    assert(resource->is_acquired == true);
    
    // Destroy resource (should call cleanup)
    concurrent_resource_destroy(resource);
    assert(g_cleanup_called == true);
    
    printf("✓ Resource management test passed\n");
}

// Test for-each parallel execution
void test_for_each_parallel(void) {
    printf("\n=== Testing For-Each Parallel Execution ===\n");
    
    // Initialize scheduler
    StructuredScheduler* scheduler = structured_scheduler_create(NULL);
    assert(scheduler != NULL);
    structured_scheduler_start(scheduler);
    structured_set_global_scheduler(scheduler);
    
    // Create test data
    int values[5] = {1, 2, 3, 4, 5};
    void* items[5];
    for (int i = 0; i < 5; i++) {
        items[i] = &values[i];
    }
    
    int total_processed = 0;

    // Execute for-each (for_each_func is defined at file scope)
    ConcurrentBlockConfig config = concurrent_block_config_default();
    Result_void_ptr result = concurrent_for_each(items, 5, sizeof(int), for_each_func, &total_processed, config);
    assert(!result.is_error);
    
    // Check results
    assert(total_processed == 5);
    for (int i = 0; i < 5; i++) {
        printf("  Item %d: %d (should be %d)\n", i, values[i], (i + 1) * 2);
        assert(values[i] == (i + 1) * 2);
    }
    
    // Cleanup
    structured_scheduler_shutdown(scheduler, 5000);
    structured_scheduler_destroy(scheduler);
    
    printf("✓ For-each parallel execution test passed\n");
}

int main(void) {
    printf("Starting Enhanced Structured Concurrency Tests\n");
    printf("==============================================\n");
    
    // Run all tests
    test_basic_concurrent_block();
    test_fail_fast_error_handling();
    test_timeout_handling();
    test_timeout_decorator();
    test_retry_decorator();
    test_dependency_handling();
    test_resource_management();
    test_for_each_parallel();
    
    printf("\n✅ All enhanced structured concurrency tests passed!\n");
    return 0;
}