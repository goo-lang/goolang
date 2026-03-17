#include "async_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Simple test macros
#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } \
} while(0)
#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))
#define EXPECT_EQ(a, b) do { \
    if ((a) != (b)) { printf("  FAIL: %s != %s (line %d)\n", #a, #b, __LINE__); return 1; } \
} while(0)
#define EXPECT_NOT_NULL(p) do { \
    if ((p) == NULL) { printf("  FAIL: %s is NULL (line %d)\n", #p, __LINE__); return 1; } \
} while(0)

// =============================================================================
// Test Helpers
// =============================================================================

static void* return_42(void* arg) {
    (void)arg;
    int* val = malloc(sizeof(int));
    *val = 42;
    return val;
}

static void* slow_task(void* arg) {
    (void)arg;
    usleep(50000); // 50ms
    int* val = malloc(sizeof(int));
    *val = 99;
    return val;
}

static void* failing_task(void* arg) {
    (void)arg;
    return (void*)-1; // Convention for failure
}

// =============================================================================
// Future Tests
// =============================================================================

static int test_future_lifecycle(void) {
    GooFuture* f = goo_future_new();
    EXPECT_NOT_NULL(f);
    EXPECT_EQ(goo_future_state(f), FUTURE_PENDING);
    EXPECT_FALSE(goo_future_is_done(f));

    int val = 42;
    goo_future_complete(f, &val, sizeof(int));

    EXPECT_TRUE(goo_future_is_done(f));
    EXPECT_EQ(goo_future_state(f), FUTURE_COMPLETED);

    int* result = (int*)goo_future_await(f);
    EXPECT_NOT_NULL(result);
    EXPECT_EQ(*result, 42);

    goo_future_release(f);
    return 0;
}

static int test_future_fail(void) {
    GooFuture* f = goo_future_new();
    goo_future_fail(f, "something broke");

    EXPECT_TRUE(goo_future_is_done(f));
    EXPECT_EQ(goo_future_state(f), FUTURE_FAILED);
    EXPECT_TRUE(strcmp(goo_future_error(f), "something broke") == 0);

    void* result = goo_future_await(f);
    EXPECT_TRUE(result == NULL);

    goo_future_release(f);
    return 0;
}

static int test_future_cancel(void) {
    GooFuture* f = goo_future_new();
    goo_future_cancel(f);

    EXPECT_EQ(goo_future_state(f), FUTURE_CANCELLED);
    EXPECT_TRUE(goo_future_is_done(f));

    goo_future_release(f);
    return 0;
}

static int test_future_double_complete(void) {
    GooFuture* f = goo_future_new();
    int val1 = 10, val2 = 20;
    goo_future_complete(f, &val1, sizeof(int));
    goo_future_complete(f, &val2, sizeof(int)); // Should be ignored

    int* result = (int*)goo_future_await(f);
    EXPECT_EQ(*result, 10); // First value wins

    goo_future_release(f);
    return 0;
}

// =============================================================================
// Executor Tests
// =============================================================================

static int test_executor_submit(void) {
    AsyncExecutor* exec = async_executor_new(2);
    EXPECT_NOT_NULL(exec);

    GooFuture* f = async_submit(exec, return_42, NULL);
    EXPECT_NOT_NULL(f);

    void* result = goo_future_await(f);
    EXPECT_NOT_NULL(result);
    // Result is a pointer stored via memcpy of sizeof(void*)
    void* actual = *(void**)result;
    EXPECT_NOT_NULL(actual);

    goo_future_release(f);
    async_executor_free(exec);
    return 0;
}

static int test_executor_multiple_tasks(void) {
    AsyncExecutor* exec = async_executor_new(4);

    GooFuture* futures[10];
    for (int i = 0; i < 10; i++) {
        futures[i] = async_submit(exec, return_42, NULL);
        EXPECT_NOT_NULL(futures[i]);
    }

    // All should complete
    for (int i = 0; i < 10; i++) {
        goo_future_await(futures[i]);
        EXPECT_EQ(goo_future_state(futures[i]), FUTURE_COMPLETED);
        goo_future_release(futures[i]);
    }

    EXPECT_EQ(exec->stats.tasks_submitted, (uint64_t)10);
    EXPECT_EQ(exec->stats.tasks_completed, (uint64_t)10);

    async_executor_free(exec);
    return 0;
}

static int test_executor_failing_task(void) {
    AsyncExecutor* exec = async_executor_new(1);

    GooFuture* f = async_submit(exec, failing_task, NULL);
    goo_future_await(f);

    EXPECT_EQ(goo_future_state(f), FUTURE_FAILED);
    EXPECT_EQ(exec->stats.tasks_failed, (uint64_t)1);

    goo_future_release(f);
    async_executor_free(exec);
    return 0;
}

// =============================================================================
// Concurrent Block Tests
// =============================================================================

static int test_concurrent_block_basic(void) {
    AsyncExecutor* exec = async_executor_new(4);
    ConcurrentBlock* block = concurrent_block_new(false, 0);
    EXPECT_NOT_NULL(block);

    concurrent_block_add(block, exec, return_42, NULL);
    concurrent_block_add(block, exec, return_42, NULL);
    concurrent_block_add(block, exec, return_42, NULL);

    size_t count = 0;
    void** results = concurrent_block_await_all(block, &count);

    EXPECT_EQ(count, (size_t)3);
    EXPECT_NOT_NULL(results);
    EXPECT_FALSE(concurrent_block_has_errors(block));

    free(results);
    concurrent_block_free(block);
    async_executor_free(exec);
    return 0;
}

static int test_concurrent_block_fail_fast(void) {
    AsyncExecutor* exec = async_executor_new(2);
    ConcurrentBlock* block = concurrent_block_new(true, 0);

    concurrent_block_add(block, exec, failing_task, NULL);
    concurrent_block_add(block, exec, slow_task, NULL);

    size_t count = 0;
    void** results = concurrent_block_await_all(block, &count);

    EXPECT_TRUE(concurrent_block_has_errors(block));
    EXPECT_NOT_NULL(concurrent_block_first_error(block));

    free(results);
    concurrent_block_free(block);
    async_executor_free(exec);
    return 0;
}

// =============================================================================
// Test Runner
// =============================================================================

typedef struct { const char* name; int (*func)(void); } TestCase;

int main(void) {
    TestCase tests[] = {
        {"future_lifecycle",          test_future_lifecycle},
        {"future_fail",               test_future_fail},
        {"future_cancel",             test_future_cancel},
        {"future_double_complete",    test_future_double_complete},
        {"executor_submit",           test_executor_submit},
        {"executor_multiple_tasks",   test_executor_multiple_tasks},
        {"executor_failing_task",     test_executor_failing_task},
        {"concurrent_block_basic",    test_concurrent_block_basic},
        {"concurrent_block_fail_fast", test_concurrent_block_fail_fast},
    };

    size_t total = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;

    printf("Running %zu async runtime tests...\n\n", total);

    for (size_t i = 0; i < total; i++) {
        printf("[ RUN      ] async.%s\n", tests[i].name);
        int result = tests[i].func();
        if (result == 0) {
            printf("[\033[32m       OK\033[0m ] async.%s\n", tests[i].name);
            passed++;
        } else {
            printf("[\033[31m  FAILED  \033[0m ] async.%s\n", tests[i].name);
        }
    }

    printf("\nTests run: %zu\n\033[32mPassed: %zu\033[0m\n", total, passed);
    if (passed < total) printf("\033[31mFailed: %zu\033[0m\n", total - passed);

    return passed < total ? 1 : 0;
}
