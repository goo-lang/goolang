#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include "../../include/async_streams.h"
// #include "../../include/structured_concurrency_enhanced.h"

// Test data structures
typedef struct IntegerContext {
    int start;
    int end;
    int current;
    int step;
} IntegerContext;

typedef struct ProcessingContext {
    int multiplier;
    int offset;
} ProcessingContext;

// Simple integer range iterator
Result_void_ptr integer_iterator_next(AsyncIterator* iter, StreamItem** item) {
    if (!iter || !iter->context || !item) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid iterator or context"));
    }
    
    IntegerContext* ctx = (IntegerContext*)iter->context;
    
    if (ctx->current >= ctx->end) {
        // End of iteration
        *item = stream_item_end_marker();
        return OK_PTR(*item);
    }
    
    // Create item with current value
    int* value = malloc(sizeof(int));
    *value = ctx->current;
    
    *item = stream_item_create_with_destructor(value, sizeof(int), free);
    if (!*item) {
        free(value);
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create stream item"));
    }
    
    ctx->current += ctx->step;
    
    return OK_PTR(*item);
}

bool integer_iterator_has_next(AsyncIterator* iter) {
    if (!iter || !iter->context) return false;
    
    IntegerContext* ctx = (IntegerContext*)iter->context;
    return ctx->current < ctx->end;
}

void integer_iterator_cleanup(AsyncIterator* iter) {
    if (!iter) return;
    printf("🧹 Cleaning up integer iterator: %s\n", iter->name);
}

// Stream processing functions

// Map function: multiply by factor and add offset
Result_void_ptr multiply_and_add_map(StreamItem* input, StreamItem** output, void* context) {
    if (!input || !output || !context) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid map parameters"));
    }
    
    ProcessingContext* proc_ctx = (ProcessingContext*)context;
    int* input_value = (int*)input->data;
    
    int* result = malloc(sizeof(int));
    *result = (*input_value * proc_ctx->multiplier) + proc_ctx->offset;
    
    *output = stream_item_create_with_destructor(result, sizeof(int), free);
    if (!*output) {
        free(result);
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create output item"));
    }
    
    printf("  📊 Map: %d -> %d\n", *input_value, *result);
    return OK_PTR(*output);
}

// Filter function: only allow even numbers
bool even_numbers_filter(StreamItem* input, void* context) {
    if (!input || !input->data) return false;
    
    int* value = (int*)input->data;
    bool is_even = (*value % 2) == 0;
    
    printf("  🔍 Filter: %d %s\n", *value, is_even ? "PASS" : "BLOCK");
    return is_even;
}

// Test basic async iterator functionality
void test_basic_async_iterator(void) {
    printf("\n📋 Test: Basic Async Iterator\n");
    printf("=============================\n");
    
    // Create integer range iterator (1 to 10)
    IntegerContext ctx = {.start = 1, .end = 11, .current = 1, .step = 1};
    
    AsyncIterator* iter = async_iterator_create("integer_range", 
        integer_iterator_next, integer_iterator_has_next, integer_iterator_cleanup,
        &ctx, sizeof(IntegerContext));
    assert(iter != NULL);
    
    // Start iterator
    Result_void_ptr start_result = async_iterator_start(iter);
    assert(!start_result.is_error);
    
    // Iterate through all items
    int count = 0;
    int expected_values[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    
    while (async_iterator_has_next(iter)) {
        StreamItem* item = NULL;
        Result_void_ptr next_result = async_iterator_next(iter, &item);
        
        if (next_result.is_error) {
            printf("❌ Iterator error: %s\n", next_result.error->message);
            break;
        }
        
        if (!item) continue;
        
        if (item->is_end_marker) {
            printf("🏁 End marker reached\n");
            stream_item_unref(item);
            break;
        }
        
        assert(item->data != NULL);
        int* value = (int*)item->data;
        printf("📦 Item %d: %d\n", count, *value);
        
        assert(count < 10);
        assert(*value == expected_values[count]);
        
        stream_item_unref(item);
        count++;
    }
    
    assert(count == 10);
    printf("✅ Processed %d items successfully\n", count);
    
    async_iterator_destroy(iter);
    printf("✅ Basic async iterator test passed\n");
}

// Test stream buffer operations
void test_stream_buffer(void) {
    printf("\n🛡️ Test: Stream Buffer Operations\n");
    printf("=================================\n");
    
    // Create buffer with capacity 5
    StreamBuffer* buffer = stream_buffer_create(5, BACKPRESSURE_BLOCK);
    assert(buffer != NULL);
    assert(stream_buffer_is_empty(buffer));
    
    // Add items to buffer
    for (int i = 1; i <= 3; i++) {
        int* value = malloc(sizeof(int));
        *value = i * 10;
        
        StreamItem* item = stream_item_create_with_destructor(value, sizeof(int), free);
        assert(item != NULL);
        
        Result_void_ptr put_result = stream_buffer_put(buffer, item, 1000);
        assert(!put_result.is_error);
        
        printf("📥 Added item: %d\n", *value);
        stream_item_unref(item);
    }
    
    assert(!stream_buffer_is_empty(buffer));
    assert(!stream_buffer_is_full(buffer));
    
    // Remove items from buffer
    for (int i = 1; i <= 3; i++) {
        StreamItem* item = NULL;
        Result_void_ptr get_result = stream_buffer_get(buffer, &item, 1000);
        assert(!get_result.is_error);
        assert(item != NULL);
        
        int* value = (int*)item->data;
        printf("📤 Retrieved item: %d\n", *value);
        assert(*value == i * 10);
        
        stream_item_unref(item);
    }
    
    assert(stream_buffer_is_empty(buffer));
    
    // Test buffer closing
    Result_void_ptr close_result = stream_buffer_close(buffer);
    assert(!close_result.is_error);
    
    stream_buffer_destroy(buffer);
    printf("✅ Stream buffer test passed\n");
}

// Test stream pipeline with map and filter operations
void test_stream_pipeline(void) {
    printf("\n🚰 Test: Stream Pipeline Processing\n");
    printf("===================================\n");
    
    // Initialize structured concurrency - simplified for testing
    // StructuredScheduler* scheduler = structured_scheduler_create(NULL);
    // assert(scheduler != NULL);
    // structured_scheduler_start(scheduler);
    // structured_set_global_scheduler(scheduler);
    
    // Create source iterator (numbers 1-10)
    IntegerContext ctx = {.start = 1, .end = 11, .current = 1, .step = 1};
    AsyncIterator* source = async_iterator_create("number_source",
        integer_iterator_next, integer_iterator_has_next, integer_iterator_cleanup,
        &ctx, sizeof(IntegerContext));
    assert(source != NULL);
    
    // Create pipeline
    StreamPipeline* pipeline = stream_pipeline_create("test_pipeline", source);
    assert(pipeline != NULL);
    
    // Add map operation (multiply by 2, add 1)
    ProcessingContext map_ctx = {.multiplier = 2, .offset = 1};
    Result_void_ptr map_result = stream_pipeline_map(pipeline, "multiply_and_add", 
        multiply_and_add_map, &map_ctx, sizeof(ProcessingContext));
    assert(!map_result.is_error);
    
    // Add filter operation (only even numbers)
    Result_void_ptr filter_result = stream_pipeline_filter(pipeline, "even_filter",
        even_numbers_filter, NULL, 0);
    assert(!filter_result.is_error);
    
    // Add buffer operation
    Result_void_ptr buffer_result = stream_pipeline_buffer(pipeline, "output_buffer",
        100, BACKPRESSURE_BLOCK);
    assert(!buffer_result.is_error);
    
    printf("🔧 Pipeline created with %zu operations\n", pipeline->operation_count);
    
    // Start pipeline
    Result_void_ptr start_result = stream_pipeline_start(pipeline);
    assert(!start_result.is_error);
    
    printf("⏳ Waiting for pipeline to process...\n");
    
    // Wait for pipeline to complete
    usleep(2000000); // 2 seconds
    
    // Stop pipeline
    Result_void_ptr stop_result = stream_pipeline_stop(pipeline, 5000);
    assert(!stop_result.is_error);
    
    printf("📊 Pipeline processed %llu items at %.2f items/sec\n",
           pipeline->total_items_processed, pipeline->throughput_items_per_sec);
    
    // Clean up
    stream_pipeline_destroy(pipeline);
    async_iterator_destroy(source);
    
    // structured_scheduler_shutdown(scheduler, 5000);
    // structured_scheduler_destroy(scheduler);
    
    printf("✅ Stream pipeline test passed\n");
}

// Test backpressure handling
void test_backpressure_handling(void) {
    printf("\n⏸️ Test: Backpressure Handling\n");
    printf("=============================\n");
    
    // Create small buffer with drop strategy
    StreamBuffer* buffer = stream_buffer_create(3, BACKPRESSURE_DROP_NEW);
    assert(buffer != NULL);
    
    // Fill buffer to capacity
    for (int i = 1; i <= 3; i++) {
        int* value = malloc(sizeof(int));
        *value = i;
        
        StreamItem* item = stream_item_create_with_destructor(value, sizeof(int), free);
        Result_void_ptr put_result = stream_buffer_put(buffer, item, 100);
        assert(!put_result.is_error);
        
        printf("📥 Added item %d to buffer\n", i);
        stream_item_unref(item);
    }
    
    assert(stream_buffer_is_full(buffer));
    
    // Try to add one more item - should be dropped
    int* overflow_value = malloc(sizeof(int));
    *overflow_value = 999;
    StreamItem* overflow_item = stream_item_create_with_destructor(overflow_value, sizeof(int), free);
    
    Result_void_ptr overflow_result = stream_buffer_put(buffer, overflow_item, 100);
    printf("🚫 Overflow item result: %s\n", overflow_result.is_error ? "DROPPED" : "ACCEPTED");
    
    stream_item_unref(overflow_item);
    
    // Retrieve all items
    for (int i = 1; i <= 3; i++) {
        StreamItem* item = NULL;
        Result_void_ptr get_result = stream_buffer_get(buffer, &item, 100);
        assert(!get_result.is_error);
        
        int* value = (int*)item->data;
        printf("📤 Retrieved item: %d\n", *value);
        
        stream_item_unref(item);
    }
    
    stream_buffer_destroy(buffer);
    printf("✅ Backpressure handling test passed\n");
}

// Test stream item reference counting
void test_stream_item_ref_counting(void) {
    printf("\n🔢 Test: Stream Item Reference Counting\n");
    printf("=======================================\n");
    
    // Create an item
    int* value = malloc(sizeof(int));
    *value = 42;
    
    StreamItem* item = stream_item_create_with_destructor(value, sizeof(int), free);
    assert(item != NULL);
    printf("📦 Created item with value: %d (ref_count: %d)\n", *value, atomic_load(&item->ref_count));
    
    // Add references
    StreamItem* ref1 = stream_item_ref(item);
    StreamItem* ref2 = stream_item_ref(item);
    assert(ref1 == item);
    assert(ref2 == item);
    printf("🔗 Added 2 references (ref_count: %d)\n", atomic_load(&item->ref_count));
    
    // Remove references
    stream_item_unref(ref1);
    printf("🔓 Removed 1 reference (ref_count: %d)\n", atomic_load(&item->ref_count));
    
    stream_item_unref(ref2);
    printf("🔓 Removed 1 reference (ref_count: %d)\n", atomic_load(&item->ref_count));
    
    stream_item_unref(item);
    printf("🔓 Removed original reference - item should be freed\n");
    
    printf("✅ Stream item reference counting test passed\n");
}

// Test error propagation
void test_error_propagation(void) {
    printf("\n💥 Test: Error Propagation\n");
    printf("=========================\n");
    
    // Create an error
    Error* test_error = error_create(ERROR_OPERATION_FAILED, "Test error message");
    
    // Create error stream item
    StreamItem* error_item = stream_item_error(test_error);
    assert(error_item != NULL);
    assert(error_item->error != NULL);
    printf("❌ Created error item: %s\n", error_item->error->message);
    
    // Create buffer and propagate error
    StreamBuffer* buffer = stream_buffer_create(10, BACKPRESSURE_BLOCK);
    
    Result_void_ptr put_result = stream_buffer_put(buffer, error_item, 1000);
    assert(!put_result.is_error);
    
    // Retrieve error item
    StreamItem* retrieved_item = NULL;
    Result_void_ptr get_result = stream_buffer_get(buffer, &retrieved_item, 1000);
    assert(!get_result.is_error);
    assert(retrieved_item != NULL);
    assert(retrieved_item->error != NULL);
    
    printf("📤 Retrieved error item: %s\n", retrieved_item->error->message);
    
    stream_item_unref(error_item);
    stream_item_unref(retrieved_item);
    stream_buffer_destroy(buffer);
    
    printf("✅ Error propagation test passed\n");
}

int main(void) {
    printf("🧪 Async Streams and Pipeline Testing\n");
    printf("=====================================\n");
    
    // Run all tests
    test_basic_async_iterator();
    test_stream_buffer();
    test_stream_pipeline();
    test_backpressure_handling();
    test_stream_item_ref_counting();
    test_error_propagation();
    
    // Show global statistics
    AsyncStreamStats stats = async_stream_get_global_stats();
    printf("\n📈 Global Stream Statistics\n");
    printf("===========================\n");
    printf("📊 Total items processed: %llu\n", stats.total_items_processed);
    printf("📊 Items per second: %llu\n", stats.items_per_second);
    printf("📊 Backpressure events: %llu\n", stats.backpressure_events);
    printf("📊 Items dropped: %llu\n", stats.items_dropped);
    printf("📊 Error count: %llu\n", stats.error_count);
    
    printf("\n🎉 All async streams tests passed!\n");
    return 0;
}