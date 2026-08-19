#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include "../../include/reactive_programming.h"

// Test helper variables
static int test_event_processed_count = 0;

// Test event processor
EVENT_PROCESSOR(test_event_processor) {
    (void)context; // Suppress unused parameter warning
    
    if (!event) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid event"));
    }
    
    test_event_processed_count++;
    printf("⚡ Event processed: %s (ID: %llu, Count: %d)\n", 
           event->name, event->id, test_event_processed_count);
    
    return OK_PTR(event);
}

// Test event filter
EVENT_FILTER(test_event_filter) {
    (void)context; // Suppress unused parameter warning
    
    if (!event) return false;
    
    // Filter events based on name containing "important"
    return strstr(event->name, "important") != NULL;
}

// Test event stream creation and publishing
void test_event_streams(void) {
    printf("\n📡 Test: Event Streams\n");
    printf("======================\n");
    
    // Reset test counter
    test_event_processed_count = 0;
    
    // Create event stream
    EventStream* stream = event_stream_create("test_stream", EVENT_TYPE_USER_ACTION);
    assert(stream != NULL);
    assert(strcmp(stream->name, "test_stream") == 0);
    assert(stream->event_type == EVENT_TYPE_USER_ACTION);
    assert(stream->is_active);
    assert(!stream->is_closed);
    
    // Create test events
    char* event_data1 = "Test Event 1";
    char* event_data2 = "Important Test Event 2";
    char* event_data3 = "Test Event 3";
    
    Event* event1 = event_create(EVENT_TYPE_USER_ACTION, "test_event_1", 
                                event_data1, strlen(event_data1) + 1);
    Event* event2 = event_create(EVENT_TYPE_USER_ACTION, "important_event_2", 
                                event_data2, strlen(event_data2) + 1);
    Event* event3 = event_create(EVENT_TYPE_USER_ACTION, "test_event_3", 
                                event_data3, strlen(event_data3) + 1);
    
    assert(event1 != NULL);
    assert(event2 != NULL);
    assert(event3 != NULL);
    
    // Publish events
    Result_void_ptr pub1 = event_stream_publish(stream, event1);
    Result_void_ptr pub2 = event_stream_publish(stream, event2);
    Result_void_ptr pub3 = event_stream_publish(stream, event3);
    
    assert(!pub1.is_error);
    assert(!pub2.is_error);
    assert(!pub3.is_error);
    
    // Verify stream state
    assert(stream->event_count == 3);
    assert(stream->head_event != NULL);
    assert(stream->tail_event != NULL);
    
    // Test event sequencing
    assert(event1->sequence_number == 0);
    assert(event2->sequence_number == 1);
    assert(event3->sequence_number == 2);
    
    // Close stream
    Result_void_ptr close_result = event_stream_close(stream);
    assert(!close_result.is_error);
    assert(stream->is_closed);
    
    // Cleanup
    event_unref(event1);
    event_unref(event2);
    event_unref(event3);
    event_stream_destroy(stream);
    
    printf("✅ Event streams test passed\n");
}

// Test reactive components (minimal test without actor system)
void test_reactive_components(void) {
    printf("\n⚡ Test: Reactive Components\n");
    printf("============================\n");
    
    // Create reactive component config
    ReactiveComponentConfig config = {
        .name = "test_component",
        .auto_start = true,
        .enable_persistence = false,
        .enable_metrics = true,
        .processing_timeout_ms = 5000,
        .max_event_buffer_size = 1000
    };
    
    // Create reactive component
    ReactiveComponent* component = reactive_component_create("test_component", &config);
    assert(component != NULL);
    assert(strcmp(component->name, "test_component") == 0);
    assert(component->config.auto_start == true);
    assert(component->config.enable_metrics == true);
    assert(!component->is_active);
    
    // Create input and output streams
    EventStream* input_stream = event_stream_create("input_stream", EVENT_TYPE_USER_ACTION);
    EventStream* output_stream = event_stream_create("output_stream", EVENT_TYPE_STATE_CHANGE);
    
    assert(input_stream != NULL);
    assert(output_stream != NULL);
    
    // Add streams to component (using placeholder functions)
    Result_void_ptr add_input = reactive_component_add_input_stream(component, input_stream);
    Result_void_ptr add_output = reactive_component_add_output_stream(component, output_stream);
    
    // These are placeholder implementations, so they should succeed
    assert(!add_input.is_error);
    assert(!add_output.is_error);
    
    // Start component
    Result_void_ptr start_result = reactive_component_start(component);
    assert(!start_result.is_error);
    assert(component->is_active);
    
    // Stop component
    Result_void_ptr stop_result = reactive_component_stop(component);
    assert(!stop_result.is_error);
    assert(!component->is_active);
    
    // Cleanup
    reactive_component_destroy(component);
    event_stream_destroy(input_stream);
    event_stream_destroy(output_stream);
    
    printf("✅ Reactive components test passed\n");
}

// Test event reference counting
void test_event_lifecycle(void) {
    printf("\n⚡ Test: Event Lifecycle\n");
    printf("========================\n");
    
    // Create event
    char* test_data = "Test event data";
    Event* event = event_create(EVENT_TYPE_USER_ACTION, "test_event", 
                               test_data, strlen(test_data) + 1);
    assert(event != NULL);
    assert(event->type == EVENT_TYPE_USER_ACTION);
    assert(strcmp(event->name, "test_event") == 0);
    assert(event->data_size == strlen(test_data) + 1);
    assert(strcmp((char*)event->data, test_data) == 0);
    assert(atomic_load(&event->ref_count) == 1);
    
    // Test reference counting
    Event* ref1 = event_ref(event);
    assert(ref1 == event);
    assert(atomic_load(&event->ref_count) == 2);
    
    Event* ref2 = event_ref(event);
    assert(ref2 == event);
    assert(atomic_load(&event->ref_count) == 3);
    
    // Test unreferencing
    event_unref(ref1);
    assert(atomic_load(&event->ref_count) == 2);
    
    event_unref(ref2);
    assert(atomic_load(&event->ref_count) == 1);
    
    // Final unreference should destroy the event
    event_unref(event);
    // Event is now destroyed, can't check ref_count
    
    printf("✅ Event lifecycle test passed\n");
}

// Test filter functionality
void test_event_filtering(void) {
    printf("\n🔍 Test: Event Filtering\n");
    printf("=========================\n");
    
    // Create test events
    Event* regular_event = event_create(EVENT_TYPE_USER_ACTION, "regular_event", NULL, 0);
    Event* important_event = event_create(EVENT_TYPE_USER_ACTION, "important_notification", NULL, 0);
    Event* another_important = event_create(EVENT_TYPE_USER_ACTION, "another_important_one", NULL, 0);
    
    assert(regular_event != NULL);
    assert(important_event != NULL);
    assert(another_important != NULL);
    
    // Test filter function
    bool regular_passes = test_event_filter(regular_event, NULL);
    bool important_passes = test_event_filter(important_event, NULL);
    bool another_important_passes = test_event_filter(another_important, NULL);
    
    // Verify filtering results
    assert(!regular_passes);       // Should not pass filter
    assert(important_passes);      // Should pass filter
    assert(another_important_passes); // Should pass filter
    
    printf("📊 Filter test results:\n");
    printf("   regular_event: %s\n", regular_passes ? "PASSED" : "FILTERED");
    printf("   important_notification: %s\n", important_passes ? "PASSED" : "FILTERED");
    printf("   another_important_one: %s\n", another_important_passes ? "PASSED" : "FILTERED");
    
    // Cleanup
    event_unref(regular_event);
    event_unref(important_event);
    event_unref(another_important);
    
    printf("✅ Event filtering test passed\n");
}

// Test basic stream statistics
void test_stream_statistics(void) {
    printf("\n📊 Test: Stream Statistics\n");
    printf("===========================\n");
    
    // Create stream
    EventStream* stream = event_stream_create("stats_stream", EVENT_TYPE_CUSTOM);
    assert(stream != NULL);
    
    // Publish some events
    for (int i = 0; i < 5; i++) {
        char event_name[32];
        char event_data[64];
        snprintf(event_name, sizeof(event_name), "stats_event_%d", i + 1);
        snprintf(event_data, sizeof(event_data), "Event data %d", i + 1);
        
        Event* event = event_create(EVENT_TYPE_CUSTOM, event_name, event_data, strlen(event_data) + 1);
        event_stream_publish(stream, event);
        event_unref(event);
    }
    
    // Verify statistics
    assert(stream->event_count == 5);
    assert(atomic_load(&stream->sequence_counter) == 5);
    
    // Print statistics
    event_stream_print_stats(stream);
    
    // Cleanup
    event_stream_destroy(stream);
    
    printf("✅ Stream statistics test passed\n");
}

int main(void) {
    printf("⚡ Reactive Programming System Testing (Minimal)\n");
    printf("================================================\n");
    printf("Testing core reactive programming functionality without\n");
    printf("dependencies on the full actor system.\n\n");
    
    // Run all tests
    test_event_streams();
    test_reactive_components();
    test_event_lifecycle();
    test_event_filtering();
    test_stream_statistics();
    
    printf("\n🎉 All reactive programming tests passed!\n");
    printf("💡 Key features tested:\n");
    printf("   • Event stream creation and publishing\n");
    printf("   • Event reference counting and lifecycle\n");
    printf("   • Reactive component management\n");
    printf("   • Event filtering functionality\n");
    printf("   • Stream statistics and monitoring\n");
    printf("   • Event sequence numbering\n");
    
    return 0;
}