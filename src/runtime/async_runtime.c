#include "async_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// =============================================================================
// Future Implementation
// =============================================================================

GooFuture* goo_future_new(void) {
    GooFuture* f = calloc(1, sizeof(GooFuture));
    if (!f) return NULL;

    f->state = FUTURE_PENDING;
    f->ref_count = 1;
    pthread_mutex_init(&f->mutex, NULL);
    pthread_cond_init(&f->completed, NULL);

    return f;
}

void goo_future_retain(GooFuture* future) {
    if (!future) return;
    pthread_mutex_lock(&future->mutex);
    future->ref_count++;
    pthread_mutex_unlock(&future->mutex);
}

void goo_future_release(GooFuture* future) {
    if (!future) return;

    pthread_mutex_lock(&future->mutex);
    future->ref_count--;
    int should_free = (future->ref_count <= 0);
    pthread_mutex_unlock(&future->mutex);

    if (should_free) {
        pthread_mutex_destroy(&future->mutex);
        pthread_cond_destroy(&future->completed);
        free(future->result);
        free(future->error_message);
        free(future);
    }
}

void goo_future_complete(GooFuture* future, void* result, size_t result_size) {
    if (!future) return;

    pthread_mutex_lock(&future->mutex);
    if (future->state != FUTURE_PENDING) {
        pthread_mutex_unlock(&future->mutex);
        return;
    }

    if (result && result_size > 0) {
        future->result = malloc(result_size);
        if (future->result) {
            memcpy(future->result, result, result_size);
            future->result_size = result_size;
        }
    } else {
        future->result = result;
        future->result_size = 0;
    }

    future->state = FUTURE_COMPLETED;
    FutureCallback cb = future->on_complete;
    void* ctx = future->callback_context;
    pthread_cond_broadcast(&future->completed);
    pthread_mutex_unlock(&future->mutex);

    if (cb) cb(future, ctx);
}

void goo_future_fail(GooFuture* future, const char* error) {
    if (!future) return;

    pthread_mutex_lock(&future->mutex);
    if (future->state != FUTURE_PENDING) {
        pthread_mutex_unlock(&future->mutex);
        return;
    }

    future->error_message = error ? strdup(error) : NULL;
    future->state = FUTURE_FAILED;
    FutureCallback cb = future->on_error;
    void* ctx = future->callback_context;
    pthread_cond_broadcast(&future->completed);
    pthread_mutex_unlock(&future->mutex);

    if (cb) cb(future, ctx);
}

void goo_future_cancel(GooFuture* future) {
    if (!future) return;

    pthread_mutex_lock(&future->mutex);
    if (future->state != FUTURE_PENDING) {
        pthread_mutex_unlock(&future->mutex);
        return;
    }

    future->state = FUTURE_CANCELLED;
    pthread_cond_broadcast(&future->completed);
    pthread_mutex_unlock(&future->mutex);
}

void* goo_future_await(GooFuture* future) {
    if (!future) return NULL;

    pthread_mutex_lock(&future->mutex);
    while (future->state == FUTURE_PENDING) {
        pthread_cond_wait(&future->completed, &future->mutex);
    }
    void* result = future->result;
    pthread_mutex_unlock(&future->mutex);

    return result;
}

void* goo_future_await_timeout(GooFuture* future, uint64_t timeout_ns) {
    if (!future) return NULL;

    pthread_mutex_lock(&future->mutex);

    if (future->state != FUTURE_PENDING) {
        void* result = future->result;
        pthread_mutex_unlock(&future->mutex);
        return result;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ns / 1000000000ULL;
    deadline.tv_nsec += timeout_ns % 1000000000ULL;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    while (future->state == FUTURE_PENDING) {
        int rc = pthread_cond_timedwait(&future->completed, &future->mutex, &deadline);
        if (rc != 0) break; // Timeout
    }

    void* result = (future->state == FUTURE_COMPLETED) ? future->result : NULL;
    pthread_mutex_unlock(&future->mutex);

    return result;
}

bool goo_future_is_done(GooFuture* future) {
    if (!future) return true;
    pthread_mutex_lock(&future->mutex);
    bool done = (future->state != FUTURE_PENDING);
    pthread_mutex_unlock(&future->mutex);
    return done;
}

FutureState goo_future_state(GooFuture* future) {
    if (!future) return FUTURE_FAILED;
    pthread_mutex_lock(&future->mutex);
    FutureState s = future->state;
    pthread_mutex_unlock(&future->mutex);
    return s;
}

const char* goo_future_error(GooFuture* future) {
    if (!future) return NULL;
    return future->error_message;
}

void goo_future_on_complete(GooFuture* future, FutureCallback cb, void* ctx) {
    if (!future) return;
    pthread_mutex_lock(&future->mutex);
    future->on_complete = cb;
    future->callback_context = ctx;
    // If already completed, fire immediately
    bool fire = (future->state == FUTURE_COMPLETED);
    pthread_mutex_unlock(&future->mutex);
    if (fire && cb) cb(future, ctx);
}

void goo_future_on_error(GooFuture* future, FutureCallback cb, void* ctx) {
    if (!future) return;
    pthread_mutex_lock(&future->mutex);
    future->on_error = cb;
    future->callback_context = ctx;
    bool fire = (future->state == FUTURE_FAILED);
    pthread_mutex_unlock(&future->mutex);
    if (fire && cb) cb(future, ctx);
}

// =============================================================================
// Async Executor — Worker Pool
// =============================================================================

static void* executor_worker(void* arg) {
    AsyncExecutor* executor = (AsyncExecutor*)arg;

    while (executor->running) {
        pthread_mutex_lock(&executor->queue_mutex);

        while (!executor->queue_head && executor->running) {
            pthread_cond_wait(&executor->queue_not_empty, &executor->queue_mutex);
        }

        if (!executor->running) {
            pthread_mutex_unlock(&executor->queue_mutex);
            break;
        }

        // Dequeue task
        AsyncTask* task = executor->queue_head;
        executor->queue_head = task->next;
        if (!executor->queue_head) executor->queue_tail = NULL;
        executor->queue_size--;

        pthread_mutex_unlock(&executor->queue_mutex);

        // Execute task
        if (task->cancel_token &&
            cancellation_token_is_cancelled(task->cancel_token)) {
            goo_future_cancel(task->future);
            executor->stats.tasks_cancelled++;
        } else {
            void* result = task->func(task->arg);
            if (result == (void*)-1) {
                goo_future_fail(task->future, "task failed");
                executor->stats.tasks_failed++;
            } else {
                // Store result pointer directly (zero-copy for pointers)
                goo_future_complete(task->future, &result, sizeof(void*));
                executor->stats.tasks_completed++;
            }
        }

        goo_future_release(task->future);
        free(task);
    }

    return NULL;
}

AsyncExecutor* async_executor_new(size_t worker_count) {
    if (worker_count == 0) worker_count = 4;

    AsyncExecutor* executor = calloc(1, sizeof(AsyncExecutor));
    if (!executor) return NULL;

    pthread_mutex_init(&executor->queue_mutex, NULL);
    pthread_cond_init(&executor->queue_not_empty, NULL);

    executor->running = true;
    executor->worker_count = worker_count;
    executor->workers = calloc(worker_count, sizeof(pthread_t));
    if (!executor->workers) {
        free(executor);
        return NULL;
    }

    for (size_t i = 0; i < worker_count; i++) {
        pthread_create(&executor->workers[i], NULL, executor_worker, executor);
    }

    return executor;
}

void async_executor_free(AsyncExecutor* executor) {
    if (!executor) return;

    // Signal workers to stop
    executor->running = false;
    pthread_cond_broadcast(&executor->queue_not_empty);

    // Join workers
    for (size_t i = 0; i < executor->worker_count; i++) {
        pthread_join(executor->workers[i], NULL);
    }

    // Drain remaining tasks
    AsyncTask* task = executor->queue_head;
    while (task) {
        AsyncTask* next = task->next;
        goo_future_cancel(task->future);
        goo_future_release(task->future);
        free(task);
        task = next;
    }

    pthread_mutex_destroy(&executor->queue_mutex);
    pthread_cond_destroy(&executor->queue_not_empty);
    free(executor->workers);
    free(executor);
}

GooFuture* async_submit(AsyncExecutor* executor, AsyncFunc func, void* arg) {
    return async_submit_with_timeout(executor, func, arg, 0);
}

GooFuture* async_submit_with_timeout(AsyncExecutor* executor, AsyncFunc func,
                                     void* arg, uint64_t timeout_ns) {
    if (!executor || !func) return NULL;

    GooFuture* future = goo_future_new();
    if (!future) return NULL;

    AsyncTask* task = calloc(1, sizeof(AsyncTask));
    if (!task) {
        goo_future_release(future);
        return NULL;
    }

    task->func = func;
    task->arg = arg;
    task->future = future;
    task->timeout_ns = timeout_ns;
    goo_future_retain(future); // Worker holds a ref

    // Enqueue
    pthread_mutex_lock(&executor->queue_mutex);
    if (executor->queue_tail) {
        executor->queue_tail->next = task;
    } else {
        executor->queue_head = task;
    }
    executor->queue_tail = task;
    executor->queue_size++;
    executor->stats.tasks_submitted++;
    pthread_cond_signal(&executor->queue_not_empty);
    pthread_mutex_unlock(&executor->queue_mutex);

    return future;
}

// =============================================================================
// Concurrent Block — parallel { expr1, expr2, ... }
// =============================================================================

ConcurrentBlock* concurrent_block_new(bool fail_fast, uint64_t timeout_ns) {
    ConcurrentBlock* block = calloc(1, sizeof(ConcurrentBlock));
    if (!block) return NULL;

    block->fail_fast = fail_fast;
    block->timeout_ns = timeout_ns;
    block->future_capacity = 8;
    block->futures = calloc(block->future_capacity, sizeof(GooFuture*));
    block->cancel_token = cancellation_token_create(NULL);

    if (!block->futures) {
        free(block);
        return NULL;
    }

    return block;
}

void concurrent_block_free(ConcurrentBlock* block) {
    if (!block) return;

    for (size_t i = 0; i < block->future_count; i++) {
        goo_future_release(block->futures[i]);
    }
    free(block->futures);
    if (block->cancel_token) {
        cancellation_token_destroy(block->cancel_token);
    }
    free(block);
}

void concurrent_block_add(ConcurrentBlock* block, AsyncExecutor* executor,
                          AsyncFunc func, void* arg) {
    if (!block || !executor || !func) return;

    // Grow array if needed
    if (block->future_count >= block->future_capacity) {
        size_t new_cap = block->future_capacity * 2;
        GooFuture** tmp = realloc(block->futures, new_cap * sizeof(GooFuture*));
        if (!tmp) return;
        block->futures = tmp;
        block->future_capacity = new_cap;
    }

    GooFuture* future = async_submit(executor, func, arg);
    if (future) {
        future->cancel_token = block->cancel_token;
        block->futures[block->future_count++] = future;
    }
}

void** concurrent_block_await_all(ConcurrentBlock* block, size_t* out_count) {
    if (!block || block->future_count == 0) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    void** results = calloc(block->future_count, sizeof(void*));
    if (!results) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    for (size_t i = 0; i < block->future_count; i++) {
        if (block->timeout_ns > 0) {
            results[i] = goo_future_await_timeout(block->futures[i], block->timeout_ns);
        } else {
            results[i] = goo_future_await(block->futures[i]);
        }

        // Fail-fast: if one fails, cancel the rest
        if (block->fail_fast && goo_future_state(block->futures[i]) == FUTURE_FAILED) {
            if (block->cancel_token) {
                cancellation_token_cancel(block->cancel_token, CANCEL_REASON_ERROR);
            }
            break;
        }
    }

    if (out_count) *out_count = block->future_count;
    return results;
}

bool concurrent_block_has_errors(ConcurrentBlock* block) {
    if (!block) return false;

    for (size_t i = 0; i < block->future_count; i++) {
        FutureState s = goo_future_state(block->futures[i]);
        if (s == FUTURE_FAILED || s == FUTURE_CANCELLED) return true;
    }
    return false;
}

const char* concurrent_block_first_error(ConcurrentBlock* block) {
    if (!block) return NULL;

    for (size_t i = 0; i < block->future_count; i++) {
        if (goo_future_state(block->futures[i]) == FUTURE_FAILED) {
            return goo_future_error(block->futures[i]);
        }
    }
    return NULL;
}
