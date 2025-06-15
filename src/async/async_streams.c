#include "../../include/async_streams.h"
#include "../../include/errors/error.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

// Global statistics
static AsyncStreamStats g_global_stats = {0};
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// Utility functions
static uint64_t get_current_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t generate_id(void) {
    static atomic_uint_fast64_t counter = 1;
    return atomic_fetch_add(&counter, 1);
}

// Stream item operations
StreamItem* stream_item_create(void* data, size_t size) {
    return stream_item_create_with_destructor(data, size, free);
}

StreamItem* stream_item_create_with_destructor(void* data, size_t size, void (*destructor)(void*)) {
    StreamItem* item = calloc(1, sizeof(StreamItem));
    if (!item) return NULL;
    
    if (data && size > 0) {
        item->data = malloc(size);
        if (!item->data) {
            free(item);
            return NULL;
        }
        memcpy(item->data, data, size);
        item->size = size;
    } else {
        item->data = data;
        item->size = size;
    }
    
    item->destructor = destructor;
    item->sequence_number = generate_id();
    item->timestamp_ns = get_current_time_ns();
    item->is_end_marker = false;
    item->error = NULL;
    atomic_init(&item->ref_count, 1);
    
    return item;
}

StreamItem* stream_item_ref(StreamItem* item) {
    if (!item) return NULL;
    atomic_fetch_add(&item->ref_count, 1);
    return item;
}

void stream_item_unref(StreamItem* item) {
    if (!item) return;
    
    int ref_count = atomic_fetch_sub(&item->ref_count, 1);
    if (ref_count == 1) {
        // Last reference - clean up
        if (item->data && item->destructor) {
            item->destructor(item->data);
        }
        if (item->error) {
            free((void*)item->error->message);
            free((void*)item->error->hint);
            free(item->error);
        }
        free(item);
    }
}

StreamItem* stream_item_end_marker(void) {
    StreamItem* item = calloc(1, sizeof(StreamItem));
    if (!item) return NULL;
    
    item->is_end_marker = true;
    item->sequence_number = generate_id();
    item->timestamp_ns = get_current_time_ns();
    atomic_init(&item->ref_count, 1);
    
    return item;
}

StreamItem* stream_item_error(Error* error) {
    StreamItem* item = calloc(1, sizeof(StreamItem));
    if (!item) return NULL;
    
    item->error = error;
    item->sequence_number = generate_id();
    item->timestamp_ns = get_current_time_ns();
    atomic_init(&item->ref_count, 1);
    
    return item;
}

// Async iterator operations
AsyncIterator* async_iterator_create(const char* name, 
    AsyncIteratorNext next_fn, AsyncIteratorHasNext has_next_fn, 
    AsyncIteratorCleanup cleanup_fn, void* context, size_t context_size) {
    
    if (!next_fn) return NULL;
    
    AsyncIterator* iter = calloc(1, sizeof(AsyncIterator));
    if (!iter) return NULL;
    
    iter->id = generate_id();
    if (name) {
        strncpy(iter->name, name, sizeof(iter->name) - 1);
        iter->name[sizeof(iter->name) - 1] = '\0';
    } else {
        snprintf(iter->name, sizeof(iter->name), "iter_%llu", iter->id);
    }
    
    iter->state = ASYNC_ITER_CREATED;
    iter->next_fn = next_fn;
    iter->has_next_fn = has_next_fn;
    iter->cleanup_fn = cleanup_fn;
    
    // Copy context if provided
    if (context && context_size > 0) {
        iter->context = malloc(context_size);
        if (!iter->context) {
            free(iter);
            return NULL;
        }
        memcpy(iter->context, context, context_size);
        iter->context_size = context_size;
    }
    
    iter->current_position = 0;
    iter->total_items = 0;
    iter->is_infinite = true; // Assume infinite until told otherwise
    
    // Initialize buffer
    iter->buffer_capacity = 16; // Start with small buffer
    iter->item_buffer = calloc(iter->buffer_capacity, sizeof(StreamItem*));
    if (!iter->item_buffer) {
        free(iter->context);
        free(iter);
        return NULL;
    }
    
    // Initialize synchronization
    if (pthread_mutex_init(&iter->iter_mutex, NULL) != 0) {
        free(iter->item_buffer);
        free(iter->context);
        free(iter);
        return NULL;
    }
    
    if (pthread_cond_init(&iter->item_available, NULL) != 0) {
        pthread_mutex_destroy(&iter->iter_mutex);
        free(iter->item_buffer);
        free(iter->context);
        free(iter);
        return NULL;
    }
    
    if (pthread_cond_init(&iter->buffer_space, NULL) != 0) {
        pthread_cond_destroy(&iter->item_available);
        pthread_mutex_destroy(&iter->iter_mutex);
        free(iter->item_buffer);
        free(iter->context);
        free(iter);
        return NULL;
    }
    
    iter->cancel_token = cancellation_token_create();
    if (!iter->cancel_token) {
        pthread_cond_destroy(&iter->buffer_space);
        pthread_cond_destroy(&iter->item_available);
        pthread_mutex_destroy(&iter->iter_mutex);
        free(iter->item_buffer);
        free(iter->context);
        free(iter);
        return NULL;
    }
    
    return iter;
}

void async_iterator_destroy(AsyncIterator* iter) {
    if (!iter) return;
    
    // Cancel any ongoing operations
    async_iterator_cancel(iter);
    
    // Clean up buffer
    pthread_mutex_lock(&iter->iter_mutex);
    for (size_t i = 0; i < iter->buffer_size; i++) {
        size_t index = (iter->buffer_head + i) % iter->buffer_capacity;
        if (iter->item_buffer[index]) {
            stream_item_unref(iter->item_buffer[index]);
        }
    }
    pthread_mutex_unlock(&iter->iter_mutex);
    
    free(iter->item_buffer);
    
    // Call cleanup function if provided
    if (iter->cleanup_fn) {
        iter->cleanup_fn(iter);
    }
    
    free(iter->context);
    
    if (iter->cancel_token) {
        cancellation_token_destroy(iter->cancel_token);
    }
    
    // Destroy synchronization primitives
    pthread_cond_destroy(&iter->buffer_space);
    pthread_cond_destroy(&iter->item_available);
    pthread_mutex_destroy(&iter->iter_mutex);
    
    free(iter);
}

Result_void_ptr async_iterator_start(AsyncIterator* iter) {
    if (!iter) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid iterator"));
    }
    
    pthread_mutex_lock(&iter->iter_mutex);
    
    if (iter->state != ASYNC_ITER_CREATED) {
        pthread_mutex_unlock(&iter->iter_mutex);
        return ERR_PTR(error_create(ERROR_INVALID_STATE, "Iterator already started"));
    }
    
    iter->state = ASYNC_ITER_ACTIVE;
    pthread_cond_broadcast(&iter->item_available);
    
    pthread_mutex_unlock(&iter->iter_mutex);
    
    return OK_PTR(iter);
}

Result_void_ptr async_iterator_next(AsyncIterator* iter, StreamItem** item) {
    if (!iter || !item) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid iterator or item pointer"));
    }
    
    *item = NULL;
    
    pthread_mutex_lock(&iter->iter_mutex);
    
    // Check if iterator is in valid state
    if (iter->state == ASYNC_ITER_COMPLETED) {
        pthread_mutex_unlock(&iter->iter_mutex);
        return ERR_PTR(error_create(ERROR_ITERATOR_EXHAUSTED, "Iterator is exhausted"));
    }
    
    if (iter->state == ASYNC_ITER_ERROR) {
        Error* error = iter->last_error;
        pthread_mutex_unlock(&iter->iter_mutex);
        return ERR_PTR(error);
    }
    
    if (iter->state == ASYNC_ITER_CANCELLED) {
        pthread_mutex_unlock(&iter->iter_mutex);
        return ERR_PTR(error_create(ERROR_OPERATION_CANCELLED, "Iterator was cancelled"));
    }
    
    // Check if there are buffered items
    if (iter->buffer_size > 0) {
        *item = iter->item_buffer[iter->buffer_head];
        iter->item_buffer[iter->buffer_head] = NULL;
        iter->buffer_head = (iter->buffer_head + 1) % iter->buffer_capacity;
        iter->buffer_size--;
        iter->items_consumed++;
        
        pthread_cond_signal(&iter->buffer_space);
        pthread_mutex_unlock(&iter->iter_mutex);
        
        // Check if this is an end marker
        if (*item && (*item)->is_end_marker) {
            iter->state = ASYNC_ITER_COMPLETED;
        }
        
        return OK_PTR(*item);
    }
    
    // No buffered items - call the next function
    pthread_mutex_unlock(&iter->iter_mutex);
    
    uint64_t start_time = get_current_time_ns();
    Result_void_ptr result = iter->next_fn(iter, item);
    uint64_t end_time = get_current_time_ns();
    
    pthread_mutex_lock(&iter->iter_mutex);
    iter->total_yield_time_ns += (end_time - start_time);
    
    if (result.is_error) {
        iter->state = ASYNC_ITER_ERROR;
        iter->last_error = result.error;
        pthread_mutex_unlock(&iter->iter_mutex);
        return result;
    }
    
    if (*item) {
        iter->items_produced++;
        iter->current_position++;
        
        // Check if this is an end marker
        if ((*item)->is_end_marker) {
            iter->state = ASYNC_ITER_COMPLETED;
            iter->total_items = iter->current_position;
            iter->is_infinite = false;
        }
    }
    
    pthread_mutex_unlock(&iter->iter_mutex);
    
    return result;
}

bool async_iterator_has_next(AsyncIterator* iter) {
    if (!iter) return false;
    
    pthread_mutex_lock(&iter->iter_mutex);
    
    // Check state
    if (iter->state == ASYNC_ITER_COMPLETED || 
        iter->state == ASYNC_ITER_ERROR || 
        iter->state == ASYNC_ITER_CANCELLED) {
        pthread_mutex_unlock(&iter->iter_mutex);
        return false;
    }
    
    // If we have buffered items, we definitely have next
    if (iter->buffer_size > 0) {
        pthread_mutex_unlock(&iter->iter_mutex);
        return true;
    }
    
    // If we have a has_next function, use it
    if (iter->has_next_fn) {
        bool has_next = iter->has_next_fn(iter);
        pthread_mutex_unlock(&iter->iter_mutex);
        return has_next;
    }
    
    // For infinite iterators, assume there's always a next item
    bool result = iter->is_infinite || (iter->current_position < iter->total_items);
    pthread_mutex_unlock(&iter->iter_mutex);
    
    return result;
}

Result_void_ptr async_iterator_cancel(AsyncIterator* iter) {
    if (!iter) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid iterator"));
    }
    
    pthread_mutex_lock(&iter->iter_mutex);
    
    if (iter->state != ASYNC_ITER_CANCELLED) {
        iter->state = ASYNC_ITER_CANCELLED;
        
        if (iter->cancel_token) {
            cancellation_token_cancel(iter->cancel_token);
        }
        
        pthread_cond_broadcast(&iter->item_available);
        pthread_cond_broadcast(&iter->buffer_space);
    }
    
    pthread_mutex_unlock(&iter->iter_mutex);
    
    return OK_PTR(iter);
}

// Stream buffer operations
StreamBuffer* stream_buffer_create(size_t capacity, BackpressureStrategy strategy) {
    if (capacity == 0) capacity = 1024; // Default capacity
    
    StreamBuffer* buffer = calloc(1, sizeof(StreamBuffer));
    if (!buffer) return NULL;
    
    buffer->items = calloc(capacity, sizeof(StreamItem*));
    if (!buffer->items) {
        free(buffer);
        return NULL;
    }
    
    buffer->capacity = capacity;
    buffer->size = 0;
    buffer->head = 0;
    buffer->tail = 0;
    buffer->is_closed = false;
    atomic_init(&buffer->has_error, false);
    
    // Create backpressure controller
    buffer->backpressure = backpressure_controller_create(strategy, capacity);
    if (!buffer->backpressure) {
        free(buffer->items);
        free(buffer);
        return NULL;
    }
    
    // Initialize synchronization
    if (pthread_mutex_init(&buffer->buffer_mutex, NULL) != 0) {
        backpressure_controller_destroy(buffer->backpressure);
        free(buffer->items);
        free(buffer);
        return NULL;
    }
    
    if (pthread_cond_init(&buffer->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&buffer->buffer_mutex);
        backpressure_controller_destroy(buffer->backpressure);
        free(buffer->items);
        free(buffer);
        return NULL;
    }
    
    if (pthread_cond_init(&buffer->not_full, NULL) != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->buffer_mutex);
        backpressure_controller_destroy(buffer->backpressure);
        free(buffer->items);
        free(buffer);
        return NULL;
    }
    
    return buffer;
}

void stream_buffer_destroy(StreamBuffer* buffer) {
    if (!buffer) return;
    
    // Close buffer first
    stream_buffer_close(buffer);
    
    // Clean up remaining items
    pthread_mutex_lock(&buffer->buffer_mutex);
    for (size_t i = 0; i < buffer->size; i++) {
        size_t index = (buffer->head + i) % buffer->capacity;
        if (buffer->items[index]) {
            stream_item_unref(buffer->items[index]);
        }
    }
    pthread_mutex_unlock(&buffer->buffer_mutex);
    
    free(buffer->items);
    
    if (buffer->backpressure) {
        backpressure_controller_destroy(buffer->backpressure);
    }
    
    // Destroy synchronization primitives
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->buffer_mutex);
    
    free(buffer);
}

Result_void_ptr stream_buffer_put(StreamBuffer* buffer, StreamItem* item, uint64_t timeout_ms) {
    if (!buffer || !item) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid buffer or item"));
    }
    
    pthread_mutex_lock(&buffer->buffer_mutex);
    
    if (buffer->is_closed) {
        pthread_mutex_unlock(&buffer->buffer_mutex);
        return ERR_PTR(error_create(ERROR_BUFFER_CLOSED, "Buffer is closed"));
    }
    
    // Check backpressure
    Result_void_ptr backpressure_result = backpressure_controller_check(buffer->backpressure, buffer->size);
    if (backpressure_result.is_error) {
        pthread_mutex_unlock(&buffer->buffer_mutex);
        return backpressure_result;
    }
    
    // Wait for space if buffer is full
    struct timespec deadline;
    if (timeout_ms != UINT64_MAX) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }
    
    while (buffer->size >= buffer->capacity && !buffer->is_closed) {
        int wait_result;
        if (timeout_ms == UINT64_MAX) {
            wait_result = pthread_cond_wait(&buffer->not_full, &buffer->buffer_mutex);
        } else {
            wait_result = pthread_cond_timedwait(&buffer->not_full, &buffer->buffer_mutex, &deadline);
        }
        
        if (wait_result == ETIMEDOUT) {
            pthread_mutex_unlock(&buffer->buffer_mutex);
            return ERR_PTR(error_create(ERROR_OPERATION_TIMEOUT, "Buffer put operation timed out"));
        }
    }
    
    if (buffer->is_closed) {
        pthread_mutex_unlock(&buffer->buffer_mutex);
        return ERR_PTR(error_create(ERROR_BUFFER_CLOSED, "Buffer was closed during put operation"));
    }
    
    // Add item to buffer
    buffer->items[buffer->tail] = stream_item_ref(item);
    buffer->tail = (buffer->tail + 1) % buffer->capacity;
    buffer->size++;
    buffer->total_items_added++;
    
    if (buffer->size > buffer->peak_size) {
        buffer->peak_size = buffer->size;
    }
    
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->buffer_mutex);
    
    return OK_PTR(buffer);
}

Result_void_ptr stream_buffer_get(StreamBuffer* buffer, StreamItem** item, uint64_t timeout_ms) {
    if (!buffer || !item) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid buffer or item pointer"));
    }
    
    *item = NULL;
    
    pthread_mutex_lock(&buffer->buffer_mutex);
    
    // Calculate deadline for timeout
    struct timespec deadline;
    if (timeout_ms != UINT64_MAX) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }
    
    // Wait for items if buffer is empty
    while (buffer->size == 0 && !buffer->is_closed) {
        int wait_result;
        if (timeout_ms == UINT64_MAX) {
            wait_result = pthread_cond_wait(&buffer->not_empty, &buffer->buffer_mutex);
        } else {
            wait_result = pthread_cond_timedwait(&buffer->not_empty, &buffer->buffer_mutex, &deadline);
        }
        
        if (wait_result == ETIMEDOUT) {
            pthread_mutex_unlock(&buffer->buffer_mutex);
            return ERR_PTR(error_create(ERROR_OPERATION_TIMEOUT, "Buffer get operation timed out"));
        }
    }
    
    // Check if buffer is closed and empty
    if (buffer->is_closed && buffer->size == 0) {
        pthread_mutex_unlock(&buffer->buffer_mutex);
        return ERR_PTR(error_create(ERROR_BUFFER_CLOSED, "Buffer is closed and empty"));
    }
    
    // Get item from buffer
    if (buffer->size > 0) {
        *item = buffer->items[buffer->head];
        buffer->items[buffer->head] = NULL;
        buffer->head = (buffer->head + 1) % buffer->capacity;
        buffer->size--;
        buffer->total_items_removed++;
        
        pthread_cond_signal(&buffer->not_full);
    }
    
    pthread_mutex_unlock(&buffer->buffer_mutex);
    
    return OK_PTR(*item);
}

Result_void_ptr stream_buffer_close(StreamBuffer* buffer) {
    if (!buffer) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid buffer"));
    }
    
    pthread_mutex_lock(&buffer->buffer_mutex);
    
    if (!buffer->is_closed) {
        buffer->is_closed = true;
        pthread_cond_broadcast(&buffer->not_empty);
        pthread_cond_broadcast(&buffer->not_full);
    }
    
    pthread_mutex_unlock(&buffer->buffer_mutex);
    
    return OK_PTR(buffer);
}

bool stream_buffer_is_empty(StreamBuffer* buffer) {
    if (!buffer) return true;
    
    pthread_mutex_lock(&buffer->buffer_mutex);
    bool is_empty = (buffer->size == 0);
    pthread_mutex_unlock(&buffer->buffer_mutex);
    
    return is_empty;
}

bool stream_buffer_is_full(StreamBuffer* buffer) {
    if (!buffer) return false;
    
    pthread_mutex_lock(&buffer->buffer_mutex);
    bool is_full = (buffer->size >= buffer->capacity);
    pthread_mutex_unlock(&buffer->buffer_mutex);
    
    return is_full;
}

// Backpressure controller operations
BackpressureController* backpressure_controller_create(BackpressureStrategy strategy, size_t max_buffer_size) {
    BackpressureController* controller = calloc(1, sizeof(BackpressureController));
    if (!controller) return NULL;
    
    controller->strategy = strategy;
    controller->max_buffer_size = max_buffer_size;
    controller->current_buffer_size = 0;
    atomic_init(&controller->pending_items, 0);
    
    controller->target_latency_ms = 100.0; // 100ms target latency
    controller->current_latency_ms = 0.0;
    controller->throughput_items_per_sec = 0.0;
    
    controller->max_items_per_second = 0; // No throttling by default
    controller->items_this_second = 0;
    controller->last_second_timestamp = get_current_time_ns() / 1000000000ULL;
    
    if (pthread_mutex_init(&controller->backpressure_mutex, NULL) != 0) {
        free(controller);
        return NULL;
    }
    
    return controller;
}

void backpressure_controller_destroy(BackpressureController* controller) {
    if (!controller) return;
    
    pthread_mutex_destroy(&controller->backpressure_mutex);
    free(controller);
}

Result_void_ptr backpressure_controller_check(BackpressureController* controller, size_t pending_items) {
    if (!controller) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid backpressure controller"));
    }
    
    pthread_mutex_lock(&controller->backpressure_mutex);
    
    controller->current_buffer_size = pending_items;
    atomic_store(&controller->pending_items, pending_items);
    
    // Check if we exceed the buffer limit
    if (pending_items >= controller->max_buffer_size) {
        controller->backpressure_events++;
        
        switch (controller->strategy) {
            case BACKPRESSURE_BLOCK:
                pthread_mutex_unlock(&controller->backpressure_mutex);
                // Caller should block until space is available
                return OK_PTR(controller);
                
            case BACKPRESSURE_DROP_OLD:
                // Allow new items, old items will be dropped by buffer
                controller->total_dropped_items++;
                break;
                
            case BACKPRESSURE_DROP_NEW:
                // Drop the new item
                controller->total_dropped_items++;
                pthread_mutex_unlock(&controller->backpressure_mutex);
                return ERR_PTR(error_create(ERROR_BACKPRESSURE_DROP, "Item dropped due to backpressure"));
                
            case BACKPRESSURE_ERROR:
                pthread_mutex_unlock(&controller->backpressure_mutex);
                return ERR_PTR(error_create(ERROR_BACKPRESSURE_ERROR, "Backpressure limit exceeded"));
        }
    }
    
    // Check throttling
    if (controller->max_items_per_second > 0) {
        uint64_t current_second = get_current_time_ns() / 1000000000ULL;
        
        if (current_second != controller->last_second_timestamp) {
            controller->items_this_second = 0;
            controller->last_second_timestamp = current_second;
        }
        
        if (controller->items_this_second >= controller->max_items_per_second) {
            pthread_mutex_unlock(&controller->backpressure_mutex);
            return ERR_PTR(error_create(ERROR_RATE_LIMITED, "Rate limit exceeded"));
        }
        
        controller->items_this_second++;
    }
    
    pthread_mutex_unlock(&controller->backpressure_mutex);
    
    return OK_PTR(controller);
}

void backpressure_controller_update_metrics(BackpressureController* controller, 
    double latency_ms, double throughput) {
    
    if (!controller) return;
    
    pthread_mutex_lock(&controller->backpressure_mutex);
    
    controller->current_latency_ms = latency_ms;
    controller->throughput_items_per_sec = throughput;
    
    // Adaptive backpressure adjustment
    if (latency_ms > controller->target_latency_ms * 2.0) {
        // Latency is too high - reduce buffer size
        if (controller->max_buffer_size > 64) {
            controller->max_buffer_size = controller->max_buffer_size * 0.9;
        }
    } else if (latency_ms < controller->target_latency_ms * 0.5) {
        // Latency is low - can increase buffer size
        if (controller->max_buffer_size < 10000) {
            controller->max_buffer_size = controller->max_buffer_size * 1.1;
        }
    }
    
    pthread_mutex_unlock(&controller->backpressure_mutex);
}

// Stream pipeline operations
StreamPipeline* stream_pipeline_create(const char* name, AsyncIterator* source) {
    if (!source) return NULL;
    
    StreamPipeline* pipeline = calloc(1, sizeof(StreamPipeline));
    if (!pipeline) return NULL;
    
    pipeline->id = generate_id();
    if (name) {
        strncpy(pipeline->name, name, sizeof(pipeline->name) - 1);
        pipeline->name[sizeof(pipeline->name) - 1] = '\0';
    } else {
        snprintf(pipeline->name, sizeof(pipeline->name), "pipeline_%llu", pipeline->id);
    }
    
    pipeline->source_iterator = source;
    pipeline->state = PIPELINE_CREATED;
    pipeline->default_buffer_size = 1024;
    pipeline->auto_start = false;
    pipeline->stop_on_error = true;
    
    // Create global backpressure controller
    pipeline->global_backpressure = backpressure_controller_create(BACKPRESSURE_BLOCK, 10000);
    if (!pipeline->global_backpressure) {
        free(pipeline);
        return NULL;
    }
    
    // Create output buffer
    pipeline->output_buffer = stream_buffer_create(pipeline->default_buffer_size, BACKPRESSURE_BLOCK);
    if (!pipeline->output_buffer) {
        backpressure_controller_destroy(pipeline->global_backpressure);
        free(pipeline);
        return NULL;
    }
    
    // Initialize synchronization
    if (pthread_mutex_init(&pipeline->pipeline_mutex, NULL) != 0) {
        stream_buffer_destroy(pipeline->output_buffer);
        backpressure_controller_destroy(pipeline->global_backpressure);
        free(pipeline);
        return NULL;
    }
    
    if (pthread_cond_init(&pipeline->state_changed, NULL) != 0) {
        pthread_mutex_destroy(&pipeline->pipeline_mutex);
        stream_buffer_destroy(pipeline->output_buffer);
        backpressure_controller_destroy(pipeline->global_backpressure);
        free(pipeline);
        return NULL;
    }
    
    // Get global scheduler
    // pipeline->scheduler = structured_get_global_scheduler();
    
    return pipeline;
}

void stream_pipeline_destroy(StreamPipeline* pipeline) {
    if (!pipeline) return;
    
    // Stop pipeline if running
    stream_pipeline_stop(pipeline, 5000);
    
    // Clean up operations
    StreamOperation* op = pipeline->first_operation;
    while (op) {
        StreamOperation* next = op->next;
        
        if (op->input_buffer) {
            stream_buffer_destroy(op->input_buffer);
        }
        if (op->output_buffer) {
            stream_buffer_destroy(op->output_buffer);
        }
        if (op->execution_block) {
            concurrent_block_destroy(op->execution_block);
        }
        
        free(op);
        op = next;
    }
    
    if (pipeline->output_buffer) {
        stream_buffer_destroy(pipeline->output_buffer);
    }
    
    if (pipeline->global_backpressure) {
        backpressure_controller_destroy(pipeline->global_backpressure);
    }
    
    if (pipeline->pipeline_block) {
        concurrent_block_destroy(pipeline->pipeline_block);
    }
    
    free(pipeline->worker_threads);
    
    // Destroy synchronization primitives
    pthread_cond_destroy(&pipeline->state_changed);
    pthread_mutex_destroy(&pipeline->pipeline_mutex);
    
    free(pipeline);
}

// Pipeline operation worker function
static Result_void_ptr stream_operation_worker(void* args, AsyncContext* async_ctx) {
    StreamOperation* operation = (StreamOperation*)args;
    
    if (!operation || !operation->input_buffer || !operation->output_buffer) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid stream operation"));
    }
    
    printf("🔄 Starting stream operation: %s (type: %d)\n", operation->name, operation->type);
    
    uint64_t items_processed = 0;
    uint64_t start_time = get_current_time_ns();
    
    while (true) {
        // Check for cancellation
        // if (async_ctx && atomic_load(async_ctx->is_cancelled)) {
        //     break;
        // }
        
        // Get item from input buffer
        StreamItem* input_item = NULL;
        Result_void_ptr get_result = stream_buffer_get(operation->input_buffer, &input_item, 1000);
        
        if (get_result.is_error) {
            if (get_result.error->code == ERROR_OPERATION_TIMEOUT) {
                continue; // Timeout is normal, keep trying
            } else if (get_result.error->code == ERROR_BUFFER_CLOSED) {
                break; // Input buffer closed, stop processing
            } else {
                printf("❌ Error getting item from input buffer: %s\n", get_result.error->message);
                break;
            }
        }
        
        if (!input_item) continue;
        
        // Check for end marker
        if (input_item->is_end_marker) {
            // Propagate end marker to output
            stream_buffer_put(operation->output_buffer, input_item, UINT64_MAX);
            stream_item_unref(input_item);
            break;
        }
        
        // Check for error item
        if (input_item->error) {
            // Propagate error to output
            stream_buffer_put(operation->output_buffer, input_item, UINT64_MAX);
            stream_item_unref(input_item);
            operation->error_count++;
            continue;
        }
        
        // Process the item based on operation type
        StreamItem* output_item = NULL;
        Result_void_ptr process_result = OK_PTR(NULL);
        
        switch (operation->type) {
            case STREAM_OP_MAP: {
                process_result = operation->config.map_op.map_fn(input_item, &output_item, 
                    operation->config.map_op.context);
                break;
            }
            
            case STREAM_OP_FILTER: {
                bool should_include = operation->config.filter_op.filter_fn(input_item, 
                    operation->config.filter_op.context);
                if (should_include) {
                    output_item = stream_item_ref(input_item); // Pass through
                }
                process_result = OK_PTR(output_item);
                break;
            }
            
            case STREAM_OP_BUFFER: {
                // Simple pass-through for buffer operations
                output_item = stream_item_ref(input_item);
                process_result = OK_PTR(output_item);
                break;
            }
            
            default:
                printf("⚠️ Unsupported operation type: %d\n", operation->type);
                output_item = stream_item_ref(input_item); // Pass through
                process_result = OK_PTR(output_item);
                break;
        }
        
        stream_item_unref(input_item);
        
        if (process_result.is_error) {
            printf("❌ Error processing item in operation %s: %s\n", 
                   operation->name, process_result.error->message);
            operation->error_count++;
            
            // Create error item and propagate
            StreamItem* error_item = stream_item_error(process_result.error);
            stream_buffer_put(operation->output_buffer, error_item, UINT64_MAX);
            stream_item_unref(error_item);
            continue;
        }
        
        // Put processed item to output buffer (if any)
        if (output_item) {
            Result_void_ptr put_result = stream_buffer_put(operation->output_buffer, output_item, UINT64_MAX);
            if (put_result.is_error) {
                printf("❌ Error putting item to output buffer: %s\n", put_result.error->message);
                stream_item_unref(output_item);
                break;
            }
            stream_item_unref(output_item);
        }
        
        items_processed++;
        operation->items_processed++;
    }
    
    uint64_t end_time = get_current_time_ns();
    operation->processing_time_ns += (end_time - start_time);
    
    printf("✅ Stream operation %s completed: %llu items processed\n", 
           operation->name, items_processed);
    
    return OK_PTR(operation);
}

Result_void_ptr stream_pipeline_start(StreamPipeline* pipeline) {
    if (!pipeline) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid pipeline"));
    }
    
    pthread_mutex_lock(&pipeline->pipeline_mutex);
    
    if (pipeline->state != PIPELINE_CREATED && pipeline->state != PIPELINE_STOPPED) {
        pthread_mutex_unlock(&pipeline->pipeline_mutex);
        return ERR_PTR(error_create(ERROR_INVALID_STATE, "Pipeline already started or in invalid state"));
    }
    
    pipeline->state = PIPELINE_STARTING;
    pipeline->pipeline_start_time = get_current_time_ns();
    
    // Create concurrent block for pipeline execution
    ConcurrentBlockConfig config = concurrent_block_config_io_intensive();
    config.max_concurrent_tasks = pipeline->operation_count + 2; // +1 for source, +1 for sink
    config.fail_fast = pipeline->stop_on_error;
    
    pipeline->pipeline_block = concurrent_block_create(pipeline->name, config);
    if (!pipeline->pipeline_block) {
        pipeline->state = PIPELINE_ERROR;
        pthread_mutex_unlock(&pipeline->pipeline_mutex);
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create pipeline block"));
    }
    
    // Start the source iterator
    Result_void_ptr start_result = async_iterator_start(pipeline->source_iterator);
    if (start_result.is_error) {
        pipeline->state = PIPELINE_ERROR;
        pthread_mutex_unlock(&pipeline->pipeline_mutex);
        return start_result;
    }
    
    // Create source worker that feeds the first operation
    if (pipeline->first_operation) {
        // Source worker function
        ConcurrentFunction source_worker = (ConcurrentFunction)^Result_void_ptr(void* args, AsyncContext* async_ctx) {
            StreamPipeline* p = (StreamPipeline*)args;
            printf("🎯 Starting source worker for pipeline: %s\n", p->name);
            
            while (true) {
                // if (async_ctx && atomic_load(async_ctx->is_cancelled)) {
                //     break;
                // }
                
                StreamItem* item = NULL;
                Result_void_ptr next_result = async_iterator_next(p->source_iterator, &item);
                
                if (next_result.is_error) {
                    printf("❌ Source iterator error: %s\n", next_result.error->message);
                    // Create error item and send to first operation
                    StreamItem* error_item = stream_item_error(next_result.error);
                    stream_buffer_put(p->first_operation->input_buffer, error_item, UINT64_MAX);
                    stream_item_unref(error_item);
                    break;
                }
                
                if (!item) continue;
                
                // Send item to first operation
                Result_void_ptr put_result = stream_buffer_put(p->first_operation->input_buffer, item, UINT64_MAX);
                if (put_result.is_error) {
                    printf("❌ Error feeding first operation: %s\n", put_result.error->message);
                    stream_item_unref(item);
                    break;
                }
                
                stream_item_unref(item);
                
                // Check if item was end marker
                if (item && item->is_end_marker) {
                    break;
                }
                
                p->total_items_processed++;
            }
            
            printf("✅ Source worker completed for pipeline: %s\n", p->name);
            return OK_PTR(p);
        };
        
        ConcurrentExpression* source_expr = concurrent_expression_create("source_worker", 
            source_worker, pipeline, sizeof(StreamPipeline));
        if (!source_expr) {
            pipeline->state = PIPELINE_ERROR;
            pthread_mutex_unlock(&pipeline->pipeline_mutex);
            return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create source expression"));
        }
        
        concurrent_block_add_expression(pipeline->pipeline_block, source_expr);
    }
    
    // Start all operation workers
    StreamOperation* op = pipeline->first_operation;
    while (op) {
        ConcurrentExpression* op_expr = concurrent_expression_create(op->name, 
            (ConcurrentFunction)stream_operation_worker, op, sizeof(StreamOperation));
        if (!op_expr) {
            pipeline->state = PIPELINE_ERROR;
            pthread_mutex_unlock(&pipeline->pipeline_mutex);
            return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create operation expression"));
        }
        
        concurrent_block_add_expression(pipeline->pipeline_block, op_expr);
        op = op->next;
    }
    
    // Execute the pipeline block
    Result_void_ptr exec_result = concurrent_block_execute(pipeline->pipeline_block);
    if (exec_result.is_error) {
        pipeline->state = PIPELINE_ERROR;
        pthread_mutex_unlock(&pipeline->pipeline_mutex);
        return exec_result;
    }
    
    pipeline->state = PIPELINE_RUNNING;
    pthread_cond_broadcast(&pipeline->state_changed);
    
    pthread_mutex_unlock(&pipeline->pipeline_mutex);
    
    printf("🚀 Pipeline %s started successfully with %zu operations\n", 
           pipeline->name, pipeline->operation_count);
    
    return OK_PTR(pipeline);
}

Result_void_ptr stream_pipeline_stop(StreamPipeline* pipeline, uint64_t timeout_ms) {
    if (!pipeline) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid pipeline"));
    }
    
    pthread_mutex_lock(&pipeline->pipeline_mutex);
    
    if (pipeline->state != PIPELINE_RUNNING && pipeline->state != PIPELINE_PAUSED) {
        pthread_mutex_unlock(&pipeline->pipeline_mutex);
        return OK_PTR(pipeline); // Already stopped
    }
    
    pipeline->state = PIPELINE_STOPPING;
    
    // Cancel all operations
    if (pipeline->pipeline_block) {
        concurrent_block_cancel(pipeline->pipeline_block);
    }
    
    // Close all buffers to signal workers to stop
    StreamOperation* op = pipeline->first_operation;
    while (op) {
        if (op->input_buffer) {
            stream_buffer_close(op->input_buffer);
        }
        if (op->output_buffer) {
            stream_buffer_close(op->output_buffer);
        }
        op = op->next;
    }
    
    if (pipeline->output_buffer) {
        stream_buffer_close(pipeline->output_buffer);
    }
    
    pthread_mutex_unlock(&pipeline->pipeline_mutex);
    
    // Wait for pipeline block to complete
    if (pipeline->pipeline_block) {
        Result_void_ptr wait_result = concurrent_block_wait(pipeline->pipeline_block, timeout_ms);
        if (wait_result.is_error) {
            printf("⚠️ Pipeline stop timeout or error: %s\n", wait_result.error->message);
        }
    }
    
    pthread_mutex_lock(&pipeline->pipeline_mutex);
    pipeline->state = PIPELINE_STOPPED;
    pipeline->pipeline_end_time = get_current_time_ns();
    
    // Calculate final statistics
    if (pipeline->pipeline_end_time > pipeline->pipeline_start_time) {
        uint64_t total_time_ns = pipeline->pipeline_end_time - pipeline->pipeline_start_time;
        double total_time_s = total_time_ns / 1000000000.0;
        pipeline->throughput_items_per_sec = pipeline->total_items_processed / total_time_s;
    }
    
    pthread_cond_broadcast(&pipeline->state_changed);
    pthread_mutex_unlock(&pipeline->pipeline_mutex);
    
    printf("🛑 Pipeline %s stopped. Processed %llu items at %.2f items/sec\n", 
           pipeline->name, pipeline->total_items_processed, pipeline->throughput_items_per_sec);
    
    return OK_PTR(pipeline);
}

// Pipeline operation builders
Result_void_ptr stream_pipeline_map(StreamPipeline* pipeline, const char* name,
    StreamMapFunction map_fn, void* context, size_t context_size) {
    
    if (!pipeline || !map_fn) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid pipeline or map function"));
    }
    
    StreamOperation* operation = calloc(1, sizeof(StreamOperation));
    if (!operation) {
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate stream operation"));
    }
    
    operation->id = generate_id();
    operation->type = STREAM_OP_MAP;
    strncpy(operation->name, name ? name : "map", sizeof(operation->name) - 1);
    
    // Set up map operation
    operation->config.map_op.map_fn = map_fn;
    if (context && context_size > 0) {
        operation->config.map_op.context = malloc(context_size);
        if (!operation->config.map_op.context) {
            free(operation);
            return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate operation context"));
        }
        memcpy(operation->config.map_op.context, context, context_size);
        operation->config.map_op.context_size = context_size;
    }
    
    // Create buffers
    operation->input_buffer = stream_buffer_create(pipeline->default_buffer_size, BACKPRESSURE_BLOCK);
    operation->output_buffer = stream_buffer_create(pipeline->default_buffer_size, BACKPRESSURE_BLOCK);
    
    if (!operation->input_buffer || !operation->output_buffer) {
        if (operation->input_buffer) stream_buffer_destroy(operation->input_buffer);
        if (operation->output_buffer) stream_buffer_destroy(operation->output_buffer);
        free(operation->config.map_op.context);
        free(operation);
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create operation buffers"));
    }
    
    // Add to pipeline and connect operations
    pthread_mutex_lock(&pipeline->pipeline_mutex);
    
    if (!pipeline->first_operation) {
        pipeline->first_operation = operation;
        pipeline->last_operation = operation;
    } else {
        // Connect previous operation's output to this operation's input
        // by making them share the same buffer
        stream_buffer_destroy(operation->input_buffer);
        operation->input_buffer = pipeline->last_operation->output_buffer;
        
        pipeline->last_operation->next = operation;
        pipeline->last_operation = operation;
    }
    
    pipeline->operation_count++;
    
    pthread_mutex_unlock(&pipeline->pipeline_mutex);
    
    return OK_PTR(operation);
}

Result_void_ptr stream_pipeline_filter(StreamPipeline* pipeline, const char* name,
    StreamFilterFunction filter_fn, void* context, size_t context_size) {
    
    if (!pipeline || !filter_fn) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid pipeline or filter function"));
    }
    
    StreamOperation* operation = calloc(1, sizeof(StreamOperation));
    if (!operation) {
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate stream operation"));
    }
    
    operation->id = generate_id();
    operation->type = STREAM_OP_FILTER;
    strncpy(operation->name, name ? name : "filter", sizeof(operation->name) - 1);
    
    // Set up filter operation
    operation->config.filter_op.filter_fn = filter_fn;
    if (context && context_size > 0) {
        operation->config.filter_op.context = malloc(context_size);
        if (!operation->config.filter_op.context) {
            free(operation);
            return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate operation context"));
        }
        memcpy(operation->config.filter_op.context, context, context_size);
        operation->config.filter_op.context_size = context_size;
    }
    
    // Create buffers
    operation->input_buffer = stream_buffer_create(pipeline->default_buffer_size, BACKPRESSURE_BLOCK);
    operation->output_buffer = stream_buffer_create(pipeline->default_buffer_size, BACKPRESSURE_BLOCK);
    
    if (!operation->input_buffer || !operation->output_buffer) {
        if (operation->input_buffer) stream_buffer_destroy(operation->input_buffer);
        if (operation->output_buffer) stream_buffer_destroy(operation->output_buffer);
        free(operation->config.filter_op.context);
        free(operation);
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create operation buffers"));
    }
    
    // Add to pipeline and connect operations
    pthread_mutex_lock(&pipeline->pipeline_mutex);
    
    if (!pipeline->first_operation) {
        pipeline->first_operation = operation;
        pipeline->last_operation = operation;
    } else {
        // Connect previous operation's output to this operation's input
        // by making them share the same buffer
        stream_buffer_destroy(operation->input_buffer);
        operation->input_buffer = pipeline->last_operation->output_buffer;
        
        pipeline->last_operation->next = operation;
        pipeline->last_operation = operation;
    }
    
    pipeline->operation_count++;
    
    pthread_mutex_unlock(&pipeline->pipeline_mutex);
    
    return OK_PTR(operation);
}

Result_void_ptr stream_pipeline_batch(StreamPipeline* pipeline, const char* name,
    size_t batch_size, uint64_t timeout_ms, StreamBatchFunction batch_fn, void* context) {
    
    if (!pipeline || !batch_fn || batch_size == 0) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid pipeline, batch function, or batch size"));
    }
    
    StreamOperation* operation = calloc(1, sizeof(StreamOperation));
    if (!operation) {
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate stream operation"));
    }
    
    operation->id = generate_id();
    operation->type = STREAM_OP_BATCH;
    strncpy(operation->name, name ? name : "batch", sizeof(operation->name) - 1);
    
    // Set up batch operation
    operation->config.batch_op.batch_size = batch_size;
    operation->config.batch_op.batch_timeout_ms = timeout_ms;
    operation->config.batch_op.batch_fn = batch_fn;
    operation->config.batch_op.context = context;
    
    // Create buffers
    operation->input_buffer = stream_buffer_create(pipeline->default_buffer_size, BACKPRESSURE_BLOCK);
    operation->output_buffer = stream_buffer_create(pipeline->default_buffer_size, BACKPRESSURE_BLOCK);
    
    if (!operation->input_buffer || !operation->output_buffer) {
        if (operation->input_buffer) stream_buffer_destroy(operation->input_buffer);
        if (operation->output_buffer) stream_buffer_destroy(operation->output_buffer);
        free(operation);
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create operation buffers"));
    }
    
    // Add to pipeline and connect operations
    pthread_mutex_lock(&pipeline->pipeline_mutex);
    
    if (!pipeline->first_operation) {
        pipeline->first_operation = operation;
        pipeline->last_operation = operation;
    } else {
        // Connect previous operation's output to this operation's input
        // by making them share the same buffer
        stream_buffer_destroy(operation->input_buffer);
        operation->input_buffer = pipeline->last_operation->output_buffer;
        
        pipeline->last_operation->next = operation;
        pipeline->last_operation = operation;
    }
    
    pipeline->operation_count++;
    
    pthread_mutex_unlock(&pipeline->pipeline_mutex);
    
    return OK_PTR(operation);
}

Result_void_ptr stream_pipeline_parallel(StreamPipeline* pipeline, const char* name,
    size_t parallel_degree, ConcurrentBlockConfig config) {
    
    if (!pipeline || parallel_degree == 0) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid pipeline or parallel degree"));
    }
    
    StreamOperation* operation = calloc(1, sizeof(StreamOperation));
    if (!operation) {
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate stream operation"));
    }
    
    operation->id = generate_id();
    operation->type = STREAM_OP_PARALLEL;
    strncpy(operation->name, name ? name : "parallel", sizeof(operation->name) - 1);
    
    // Set up parallel operation
    operation->config.parallel_op.parallel_degree = parallel_degree;
    operation->config.parallel_op.concurrent_config = config;
    
    // Create buffers
    operation->input_buffer = stream_buffer_create(pipeline->default_buffer_size, BACKPRESSURE_BLOCK);
    operation->output_buffer = stream_buffer_create(pipeline->default_buffer_size, BACKPRESSURE_BLOCK);
    
    if (!operation->input_buffer || !operation->output_buffer) {
        if (operation->input_buffer) stream_buffer_destroy(operation->input_buffer);
        if (operation->output_buffer) stream_buffer_destroy(operation->output_buffer);
        free(operation);
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create operation buffers"));
    }
    
    // Add to pipeline and connect operations
    pthread_mutex_lock(&pipeline->pipeline_mutex);
    
    if (!pipeline->first_operation) {
        pipeline->first_operation = operation;
        pipeline->last_operation = operation;
    } else {
        // Connect previous operation's output to this operation's input
        // by making them share the same buffer
        stream_buffer_destroy(operation->input_buffer);
        operation->input_buffer = pipeline->last_operation->output_buffer;
        
        pipeline->last_operation->next = operation;
        pipeline->last_operation = operation;
    }
    
    pipeline->operation_count++;
    
    pthread_mutex_unlock(&pipeline->pipeline_mutex);
    
    return OK_PTR(operation);
}

Result_void_ptr stream_pipeline_buffer(StreamPipeline* pipeline, const char* name,
    size_t buffer_size, BackpressureStrategy strategy) {
    
    if (!pipeline) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid pipeline"));
    }
    
    StreamOperation* operation = calloc(1, sizeof(StreamOperation));
    if (!operation) {
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate stream operation"));
    }
    
    operation->id = generate_id();
    operation->type = STREAM_OP_BUFFER;
    strncpy(operation->name, name ? name : "buffer", sizeof(operation->name) - 1);
    
    // Set up buffer operation
    operation->config.buffer_op.buffer_size = buffer_size;
    operation->config.buffer_op.strategy = strategy;
    
    // Create buffers with specified size and strategy
    operation->input_buffer = stream_buffer_create(pipeline->default_buffer_size, BACKPRESSURE_BLOCK);
    operation->output_buffer = stream_buffer_create(buffer_size, strategy);
    
    if (!operation->input_buffer || !operation->output_buffer) {
        if (operation->input_buffer) stream_buffer_destroy(operation->input_buffer);
        if (operation->output_buffer) stream_buffer_destroy(operation->output_buffer);
        free(operation);
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create operation buffers"));
    }
    
    // Add to pipeline and connect operations
    pthread_mutex_lock(&pipeline->pipeline_mutex);
    
    if (!pipeline->first_operation) {
        pipeline->first_operation = operation;
        pipeline->last_operation = operation;
    } else {
        // Connect previous operation's output to this operation's input
        // by making them share the same buffer
        stream_buffer_destroy(operation->input_buffer);
        operation->input_buffer = pipeline->last_operation->output_buffer;
        
        pipeline->last_operation->next = operation;
        pipeline->last_operation = operation;
    }
    
    pipeline->operation_count++;
    
    pthread_mutex_unlock(&pipeline->pipeline_mutex);
    
    return OK_PTR(operation);
}

Result_void_ptr stream_pipeline_throttle(StreamPipeline* pipeline, const char* name,
    uint64_t max_items_per_second, uint64_t burst_size) {
    
    if (!pipeline || max_items_per_second == 0) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid pipeline or throttle rate"));
    }
    
    StreamOperation* operation = calloc(1, sizeof(StreamOperation));
    if (!operation) {
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate stream operation"));
    }
    
    operation->id = generate_id();
    operation->type = STREAM_OP_THROTTLE;
    strncpy(operation->name, name ? name : "throttle", sizeof(operation->name) - 1);
    
    // Set up throttle operation
    operation->config.throttle_op.max_items_per_second = max_items_per_second;
    operation->config.throttle_op.burst_size = burst_size;
    
    // Create buffers
    operation->input_buffer = stream_buffer_create(pipeline->default_buffer_size, BACKPRESSURE_BLOCK);
    operation->output_buffer = stream_buffer_create(pipeline->default_buffer_size, BACKPRESSURE_BLOCK);
    
    if (!operation->input_buffer || !operation->output_buffer) {
        if (operation->input_buffer) stream_buffer_destroy(operation->input_buffer);
        if (operation->output_buffer) stream_buffer_destroy(operation->output_buffer);
        free(operation);
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create operation buffers"));
    }
    
    // Add to pipeline and connect operations
    pthread_mutex_lock(&pipeline->pipeline_mutex);
    
    if (!pipeline->first_operation) {
        pipeline->first_operation = operation;
        pipeline->last_operation = operation;
    } else {
        // Connect previous operation's output to this operation's input
        // by making them share the same buffer
        stream_buffer_destroy(operation->input_buffer);
        operation->input_buffer = pipeline->last_operation->output_buffer;
        
        pipeline->last_operation->next = operation;
        pipeline->last_operation = operation;
    }
    
    pipeline->operation_count++;
    
    pthread_mutex_unlock(&pipeline->pipeline_mutex);
    
    return OK_PTR(operation);
}

// Global statistics
AsyncStreamStats async_stream_get_global_stats(void) {
    pthread_mutex_lock(&g_stats_mutex);
    AsyncStreamStats stats = g_global_stats;
    pthread_mutex_unlock(&g_stats_mutex);
    return stats;
}

void async_stream_reset_global_stats(void) {
    pthread_mutex_lock(&g_stats_mutex);
    memset(&g_global_stats, 0, sizeof(AsyncStreamStats));
    pthread_mutex_unlock(&g_stats_mutex);
}

// Pipeline execution functions
static void* stream_operation_processor(void* args) {
    StreamOperation* operation = (StreamOperation*)args;
    if (!operation) return NULL;
    
    printf("🔄 Starting operation processor: %s\n", operation->name);
    
    while (true) {
        StreamItem* input_item = NULL;
        Result_void_ptr get_result = stream_buffer_get(operation->input_buffer, &input_item, 1000);
        
        if (get_result.is_error) {
            if (get_result.error->code == ERROR_OPERATION_TIMEOUT) {
                continue;
            }
            if (get_result.error->code == ERROR_BUFFER_CLOSED) {
                printf("📥 Input buffer closed for operation: %s\n", operation->name);
                break;
            }
            printf("❌ Error getting item in operation %s: %s\n", operation->name, get_result.error->message);
            continue;
        }
        
        if (!input_item) continue;
        
        // Handle end marker
        if (input_item->is_end_marker) {
            printf("🏁 End marker received in operation: %s\n", operation->name);
            stream_buffer_put(operation->output_buffer, input_item, 1000);
            stream_item_unref(input_item);
            break;
        }
        
        // Handle error items
        if (input_item->error) {
            printf("❌ Error item received in operation: %s\n", operation->name);
            stream_buffer_put(operation->output_buffer, input_item, 1000);
            stream_item_unref(input_item);
            continue;
        }
        
        // Process item based on operation type
        StreamItem* output_item = NULL;
        bool should_output = true;
        
        uint64_t start_time = get_current_time_ns();
        
        switch (operation->type) {
            case STREAM_OP_MAP: {
                Result_void_ptr map_result = operation->config.map_op.map_fn(
                    input_item, &output_item, operation->config.map_op.context);
                
                if (map_result.is_error) {
                    output_item = stream_item_error(map_result.error);
                }
                break;
            }
            
            case STREAM_OP_FILTER: {
                bool passes_filter = operation->config.filter_op.filter_fn(
                    input_item, operation->config.filter_op.context);
                
                if (passes_filter) {
                    output_item = stream_item_ref(input_item);
                } else {
                    should_output = false;
                }
                break;
            }
            
            case STREAM_OP_BUFFER: {
                output_item = stream_item_ref(input_item);
                break;
            }
            
            case STREAM_OP_THROTTLE: {
                output_item = stream_item_ref(input_item);
                break;
            }
            
            default:
                printf("⚠️ Unsupported operation type: %d\n", operation->type);
                output_item = stream_item_ref(input_item);
                break;
        }
        
        uint64_t end_time = get_current_time_ns();
        operation->processing_time_ns += (end_time - start_time);
        operation->items_processed++;
        
        if (should_output && output_item) {
            Result_void_ptr put_result = stream_buffer_put(operation->output_buffer, output_item, 1000);
            if (put_result.is_error) {
                printf("❌ Error putting item in operation %s: %s\n", operation->name, put_result.error->message);
                operation->error_count++;
            }
            stream_item_unref(output_item);
        }
        
        stream_item_unref(input_item);
    }
    
    stream_buffer_close(operation->output_buffer);
    printf("✅ Operation processor finished: %s\n", operation->name);
    
    return NULL;
}

// Producer thread function
static void* stream_producer_thread(void* args) {
    StreamPipeline* pipeline = (StreamPipeline*)args;
    if (!pipeline || !pipeline->source_iterator || !pipeline->first_operation) {
        return NULL;
    }
    
    printf("📦 Starting producer thread for pipeline: %s\n", pipeline->name);
    
    while (async_iterator_has_next(pipeline->source_iterator)) {
        StreamItem* item = NULL;
        Result_void_ptr next_result = async_iterator_next(pipeline->source_iterator, &item);
        
        if (next_result.is_error) {
            printf("❌ Producer error: %s\n", next_result.error->message);
            break;
        }
        
        if (!item) continue;
        
        Result_void_ptr put_result = stream_buffer_put(pipeline->first_operation->input_buffer, item, 1000);
        if (put_result.is_error) {
            printf("❌ Producer put error: %s\n", put_result.error->message);
            stream_item_unref(item);
            break;
        }
        
        pipeline->total_items_processed++;
        stream_item_unref(item);
        
        if (item->is_end_marker) {
            printf("🏁 Producer encountered end marker\n");
            break;
        }
    }
    
    StreamItem* end_marker = stream_item_end_marker();
    stream_buffer_put(pipeline->first_operation->input_buffer, end_marker, 1000);
    stream_item_unref(end_marker);
    
    stream_buffer_close(pipeline->first_operation->input_buffer);
    
    printf("✅ Producer thread finished for pipeline: %s\n", pipeline->name);
    return NULL;
}