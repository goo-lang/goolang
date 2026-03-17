#ifndef GOO_ASYNC_RUNTIME_H
#define GOO_ASYNC_RUNTIME_H

#include "runtime.h"
#include "structured_concurrency.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

// =============================================================================
// General-Purpose Future — not tied to actors
// =============================================================================

typedef enum {
    FUTURE_PENDING,
    FUTURE_COMPLETED,
    FUTURE_FAILED,
    FUTURE_CANCELLED,
} FutureState;

typedef struct GooFuture GooFuture;
typedef void (*FutureCallback)(GooFuture* future, void* context);

struct GooFuture {
    FutureState state;
    void* result;
    size_t result_size;
    char* error_message;

    // Synchronization
    pthread_mutex_t mutex;
    pthread_cond_t completed;

    // Callbacks
    FutureCallback on_complete;
    FutureCallback on_error;
    void* callback_context;

    // Cancellation
    CancellationToken* cancel_token;

    // Reference counting
    int ref_count;
};

// =============================================================================
// Async Executor — maps async work to goroutines
// =============================================================================

typedef void* (*AsyncFunc)(void* arg);

typedef struct AsyncTask {
    AsyncFunc func;
    void* arg;
    GooFuture* future;
    CancellationToken* cancel_token;
    uint64_t timeout_ns;           // 0 = no timeout
    struct AsyncTask* next;
} AsyncTask;

typedef struct AsyncExecutor {
    // Task queue
    AsyncTask* queue_head;
    AsyncTask* queue_tail;
    size_t queue_size;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_not_empty;

    // State
    bool running;
    size_t worker_count;
    pthread_t* workers;

    // Statistics
    struct {
        uint64_t tasks_submitted;
        uint64_t tasks_completed;
        uint64_t tasks_failed;
        uint64_t tasks_cancelled;
        uint64_t tasks_timed_out;
    } stats;
} AsyncExecutor;

// =============================================================================
// Concurrent Block — runs N expressions in parallel, collects results
// =============================================================================

typedef struct ConcurrentBlock {
    GooFuture** futures;
    size_t future_count;
    size_t future_capacity;
    CancellationToken* cancel_token;
    bool fail_fast;                // Cancel all on first failure
    uint64_t timeout_ns;           // 0 = no timeout
} ConcurrentBlock;

// =============================================================================
// API — Future
// =============================================================================

GooFuture* goo_future_new(void);
void goo_future_retain(GooFuture* future);
void goo_future_release(GooFuture* future);

// Set result (called by producer)
void goo_future_complete(GooFuture* future, void* result, size_t result_size);
void goo_future_fail(GooFuture* future, const char* error);
void goo_future_cancel(GooFuture* future);

// Get result (called by consumer)
void* goo_future_await(GooFuture* future);
void* goo_future_await_timeout(GooFuture* future, uint64_t timeout_ns);
bool goo_future_is_done(GooFuture* future);
FutureState goo_future_state(GooFuture* future);
const char* goo_future_error(GooFuture* future);

// Callbacks
void goo_future_on_complete(GooFuture* future, FutureCallback cb, void* ctx);
void goo_future_on_error(GooFuture* future, FutureCallback cb, void* ctx);

// =============================================================================
// API — Executor
// =============================================================================

AsyncExecutor* async_executor_new(size_t worker_count);
void async_executor_free(AsyncExecutor* executor);

// Submit work — returns a future for the result
GooFuture* async_submit(AsyncExecutor* executor, AsyncFunc func, void* arg);
GooFuture* async_submit_with_timeout(AsyncExecutor* executor, AsyncFunc func,
                                     void* arg, uint64_t timeout_ns);

// =============================================================================
// API — Concurrent Block (parallel { expr1, expr2, ... })
// =============================================================================

ConcurrentBlock* concurrent_block_new(bool fail_fast, uint64_t timeout_ns);
void concurrent_block_free(ConcurrentBlock* block);

// Add an async function to the block
void concurrent_block_add(ConcurrentBlock* block, AsyncExecutor* executor,
                          AsyncFunc func, void* arg);

// Execute all and wait for completion — returns array of results
void** concurrent_block_await_all(ConcurrentBlock* block, size_t* out_count);

// Check if any task failed
bool concurrent_block_has_errors(ConcurrentBlock* block);
const char* concurrent_block_first_error(ConcurrentBlock* block);

#endif // GOO_ASYNC_RUNTIME_H
