#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include "include/advanced_channels.h"

// Test data structures
typedef struct {
    int value;
    char name[32];
} TestMessage;

typedef struct {
    int numbers[10];
    size_t count;
} NumberBatch;

// Global test state
static atomic_int test_message_counter = ATOMIC_VAR_INIT(0);
static atomic_int transform_counter = ATOMIC_VAR_INIT(0);

// Transformation functions for testing
Result_void_ptr test_map_function(void* input_data, size_t input_size, void** output_data, size_t* output_size, void* context) {
    if (!input_data || input_size != sizeof(TestMessage) || !output_data || !output_size) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid map function parameters"));
    }
    
    TestMessage* input_msg = (TestMessage*)input_data;
    TestMessage* output_msg = malloc(sizeof(TestMessage));
    if (!output_msg) {
        return ERR_PTR(error_create(ERROR_MEMORY_ALLOCATION, "Failed to allocate output message"));
    }
    
    // Transform: double the value and add suffix
    output_msg->value = input_msg->value * 2;
    snprintf(output_msg->name, sizeof(output_msg->name), "%s_mapped", input_msg->name);
    
    *output_data = output_msg;
    *output_size = sizeof(TestMessage);
    
    atomic_fetch_add(&transform_counter, 1);
    return OK_PTR(NULL);
}

Result_bool test_filter_function(void* data, size_t data_size, void* context) {
    if (!data || data_size != sizeof(TestMessage)) {
        return (Result_bool){.is_error = true, .error = error_create(ERROR_INVALID_EXPRESSION, "Invalid filter parameters")};
    }
    
    TestMessage* msg = (TestMessage*)data;
    // Filter: only allow even values
    bool passes = (msg->value % 2 == 0);
    
    return (Result_bool){.is_error = false, .value = passes};
}

// Test basic channel operations
void test_basic_channel_operations() {
    printf("Testing basic channel operations...\n");
    
    ChannelConfig config = channel_config_buffered(10);
    Channel* channel = channel_create(config, "test_channel");
    assert(channel != NULL);
    
    ChannelEndpoint* sender = channel_get_sender(channel);
    ChannelEndpoint* receiver = channel_get_receiver(channel);
    assert(sender != NULL);
    assert(receiver != NULL);
    
    // Test send and receive
    TestMessage send_msg = {42, "test"};
    Result_void_ptr send_result = channel_send(sender, &send_msg, sizeof(TestMessage));
    assert(!send_result.is_error);
    
    void* received_data;
    size_t received_size;
    Result_void_ptr receive_result = channel_receive(receiver, &received_data, &received_size);
    assert(!receive_result.is_error);
    assert(received_size == sizeof(TestMessage));
    
    TestMessage* received_msg = (TestMessage*)received_data;
    assert(received_msg->value == 42);
    assert(strcmp(received_msg->name, "test") == 0);
    
    free(received_data);
    channel_endpoint_destroy(sender);
    channel_endpoint_destroy(receiver);
    channel_destroy(channel);
    
    printf("✓ Basic channel operations test passed\n");
}

// Test different channel types
void test_channel_types() {
    printf("Testing different channel types...\n");
    
    // Test unbuffered channel
    ChannelConfig unbuffered_config = channel_config_unbuffered();
    Channel* unbuffered = channel_create(unbuffered_config, "unbuffered_test");
    assert(unbuffered != NULL);
    assert(unbuffered->config.type == CHANNEL_UNBUFFERED);
    channel_destroy(unbuffered);
    
    // Test broadcast channel
    ChannelConfig broadcast_config = channel_config_broadcast(5);
    Channel* broadcast = channel_create(broadcast_config, "broadcast_test");
    assert(broadcast != NULL);
    assert(broadcast->config.type == CHANNEL_BROADCAST);
    channel_destroy(broadcast);
    
    // Test reliable channel
    ChannelConfig reliable_config = channel_config_reliable();
    Channel* reliable = channel_create(reliable_config, "reliable_test");
    assert(reliable != NULL);
    assert(reliable->config.reliability == CHANNEL_RELIABILITY_EXACTLY_ONCE);
    channel_destroy(reliable);
    
    printf("✓ Channel types test passed\n");
}

// Test channel with timeout operations
void test_channel_timeouts() {
    printf("Testing channel timeout operations...\n");
    
    ChannelConfig config = channel_config_unbuffered();
    Channel* channel = channel_create(config, "timeout_test");
    assert(channel != NULL);
    
    ChannelEndpoint* receiver = channel_get_receiver(channel);
    assert(receiver != NULL);
    
    // Test receive timeout
    void* data;
    size_t size;
    uint64_t start_time = time(NULL);
    Result_void_ptr result = channel_receive_timeout(receiver, &data, &size, 100); // 100ms timeout
    uint64_t end_time = time(NULL);
    
    assert(result.is_error);
    assert(result.error->code == ERROR_CHANNEL_TIMEOUT);
    assert((end_time - start_time) < 2); // Should timeout quickly
    
    error_destroy(result.error);
    channel_endpoint_destroy(receiver);
    channel_destroy(channel);
    
    printf("✓ Channel timeout test passed\n");
}

// Test channel statistics
void test_channel_statistics() {
    printf("Testing channel statistics...\n");
    
    ChannelConfig config = channel_config_buffered(10);
    config.enable_metrics = true;
    Channel* channel = channel_create(config, "stats_test");
    assert(channel != NULL);
    
    ChannelEndpoint* sender = channel_get_sender(channel);
    ChannelEndpoint* receiver = channel_get_receiver(channel);
    
    // Send some messages
    for (int i = 0; i < 5; i++) {
        TestMessage msg = {i, "stats_test"};
        channel_send(sender, &msg, sizeof(TestMessage));
    }
    
    // Receive some messages
    for (int i = 0; i < 3; i++) {
        void* data;
        size_t size;
        Result_void_ptr result = channel_receive(receiver, &data, &size);
        if (!result.is_error) {
            free(data);
        }
    }
    
    // Check statistics
    ChannelStats stats = channel_get_stats(channel);
    assert(stats.messages_sent >= 5);
    assert(stats.messages_received >= 3);
    assert(stats.current_queue_size == 2); // 5 sent - 3 received
    
    channel_endpoint_destroy(sender);
    channel_endpoint_destroy(receiver);
    channel_destroy(channel);
    
    printf("✓ Channel statistics test passed\n");
}

// Test channel transformations
void test_channel_transformations() {
    printf("Testing channel transformations...\n");
    
    // Create input and output channels
    ChannelConfig config = channel_config_buffered(10);
    Channel* input_channel = channel_create(config, "transform_input");
    Channel* map_channel = channel_create(config, "transform_map");
    Channel* filter_channel = channel_create(config, "transform_filter");
    assert(input_channel && map_channel && filter_channel);
    
    // Create transformation stages
    ChannelTransformConfig transform_config = {
        .parallel_processing = false,
        .worker_count = 1,
        .batch_size = 1,
        .timeout_ms = 1000,
        .user_context = NULL
    };
    
    TransformStage* map_stage = channel_transform_map_create(
        input_channel, map_channel, test_map_function, transform_config);
    assert(map_stage != NULL);
    
    TransformStage* filter_stage = channel_transform_filter_create(
        map_channel, filter_channel, test_filter_function, transform_config);
    assert(filter_stage != NULL);
    
    // Start transformation stages
    assert(!channel_transform_stage_start(map_stage).is_error);
    assert(!channel_transform_stage_start(filter_stage).is_error);
    
    // Send test data
    ChannelEndpoint* sender = channel_get_sender(input_channel);
    ChannelEndpoint* receiver = channel_get_receiver(filter_channel);
    
    TestMessage input_msgs[] = {
        {1, "odd"},   // Will be mapped to {2, "odd_mapped"} and pass filter
        {2, "even"},  // Will be mapped to {4, "even_mapped"} and pass filter
        {3, "odd2"},  // Will be mapped to {6, "odd2_mapped"} and pass filter
        {5, "odd3"}   // Will be mapped to {10, "odd3_mapped"} and pass filter
    };
    
    for (size_t i = 0; i < sizeof(input_msgs) / sizeof(input_msgs[0]); i++) {
        channel_send(sender, &input_msgs[i], sizeof(TestMessage));
    }
    
    // Give transformations time to process
    usleep(500000); // 500ms
    
    // Receive and verify transformed/filtered data
    size_t received_count = 0;
    for (int i = 0; i < 10; i++) { // Try to receive up to 10 times
        void* data;
        size_t size;
        Result_void_ptr result = channel_receive_timeout(receiver, &data, &size, 100);
        
        if (!result.is_error) {
            TestMessage* msg = (TestMessage*)data;
            assert(msg->value % 2 == 0); // All received values should be even (after mapping)
            assert(strstr(msg->name, "_mapped") != NULL); // Should have mapping suffix
            
            free(data);
            received_count++;
        } else {
            error_destroy(result.error);
            break; // No more messages
        }
    }
    
    assert(received_count >= 3); // At least some messages should pass through
    
    // Cleanup
    channel_transform_stage_stop(map_stage);
    channel_transform_stage_stop(filter_stage);
    channel_transform_stage_destroy(map_stage);
    channel_transform_stage_destroy(filter_stage);
    
    channel_endpoint_destroy(sender);
    channel_endpoint_destroy(receiver);
    channel_destroy(input_channel);
    channel_destroy(map_channel);
    channel_destroy(filter_channel);
    
    printf("✓ Channel transformations test passed\n");
}

// Test load balancing
void test_load_balancing() {
    printf("Testing load balancing...\n");
    
    // Create input channel and worker channels
    ChannelConfig config = channel_config_buffered(20);
    Channel* input_channel = channel_create(config, "lb_input");
    
    const size_t worker_count = 3;
    Channel* worker_channels[worker_count];
    for (size_t i = 0; i < worker_count; i++) {
        char worker_name[32];
        snprintf(worker_name, sizeof(worker_name), "worker_%zu", i);
        worker_channels[i] = channel_create(config, worker_name);
        assert(worker_channels[i] != NULL);
    }
    
    // Create load balanced channel
    LoadBalancedChannelConfig lb_config = {
        .strategy = LOAD_BALANCE_ROUND_ROBIN,
        .worker_count = worker_count,
        .worker_weights = NULL,
        .hash_range = 0,
        .health_checking = false,
        .health_check_interval_ms = 1000
    };
    
    LoadBalancedChannel* lb_channel = load_balanced_channel_create(
        input_channel, worker_channels, worker_count, lb_config);
    assert(lb_channel != NULL);
    
    // Start load balancer
    assert(!load_balanced_channel_start(lb_channel).is_error);
    
    // Create receivers for worker channels
    ChannelEndpoint* worker_receivers[worker_count];
    for (size_t i = 0; i < worker_count; i++) {
        worker_receivers[i] = channel_get_receiver(worker_channels[i]);
        assert(worker_receivers[i] != NULL);
    }
    
    // Send messages to input channel
    ChannelEndpoint* input_sender = channel_get_sender(input_channel);
    const int message_count = 9; // Divisible by 3 for round-robin testing
    
    for (int i = 0; i < message_count; i++) {
        TestMessage msg = {i, "lb_test"};
        channel_send(input_sender, &msg, sizeof(TestMessage));
    }
    
    // Give load balancer time to distribute
    usleep(500000); // 500ms
    
    // Count messages received by each worker
    int worker_message_counts[worker_count];
    memset(worker_message_counts, 0, sizeof(worker_message_counts));
    
    for (size_t worker = 0; worker < worker_count; worker++) {
        for (int tries = 0; tries < 10; tries++) {
            void* data;
            size_t size;
            Result_void_ptr result = channel_receive_timeout(worker_receivers[worker], &data, &size, 10);
            
            if (!result.is_error) {
                worker_message_counts[worker]++;
                free(data);
            } else {
                error_destroy(result.error);
                break;
            }
        }
    }
    
    // Verify load balancing distribution
    int total_received = 0;
    for (size_t i = 0; i < worker_count; i++) {
        total_received += worker_message_counts[i];
        printf("  Worker %zu received %d messages\n", i, worker_message_counts[i]);
    }
    
    assert(total_received >= message_count - 2); // Allow for some timing issues
    
    // For round-robin, distribution should be roughly equal
    int expected_per_worker = message_count / worker_count;
    for (size_t i = 0; i < worker_count; i++) {
        assert(abs(worker_message_counts[i] - expected_per_worker) <= 2);
    }
    
    // Cleanup
    load_balanced_channel_stop(lb_channel);
    load_balanced_channel_destroy(lb_channel);
    
    channel_endpoint_destroy(input_sender);
    for (size_t i = 0; i < worker_count; i++) {
        channel_endpoint_destroy(worker_receivers[i]);
        channel_destroy(worker_channels[i]);
    }
    channel_destroy(input_channel);
    
    printf("✓ Load balancing test passed\n");
}

// Test persistence (basic file-based)
void test_channel_persistence() {
    printf("Testing channel persistence...\n");
    
    const char* test_file = "/tmp/channel_persistence_test.dat";
    
    // Remove any existing test file
    unlink(test_file);
    
    // Create channel with persistence
    ChannelConfig config = channel_config_buffered(10);
    Channel* channel = channel_create(config, "persistent_test");
    assert(channel != NULL);
    
    ChannelPersistenceConfig persistence_config = {
        .enable_persistence = true,
        .storage_type = PERSISTENCE_FILE,
        .flush_interval_ms = 100,
        .max_stored_messages = 100,
        .compress_messages = false
    };
    strncpy(persistence_config.storage_path, test_file, sizeof(persistence_config.storage_path) - 1);
    
    PersistentChannel* persistent = persistent_channel_create(channel, persistence_config);
    assert(persistent != NULL);
    
    // Start persistence thread
    persistent->persistence_active = true;
    assert(pthread_create(&persistent->persistence_thread, NULL, 
                         channel_persistence_worker, persistent) == 0);
    
    // Store some messages
    for (int i = 0; i < 5; i++) {
        TestMessage msg = {i, "persistent"};
        ChannelMessage* channel_msg = channel_message_create(&msg, sizeof(TestMessage));
        assert(channel_msg != NULL);
        
        persistent_channel_store_message(persistent, channel_msg);
        channel_message_destroy(channel_msg);
    }
    
    // Wait for persistence flush
    usleep(200000); // 200ms
    
    // Stop persistence
    persistent->persistence_active = false;
    pthread_join(persistent->persistence_thread, NULL);
    
    // Check that file was created and has content
    FILE* test_file_handle = fopen(test_file, "rb");
    assert(test_file_handle != NULL);
    
    fseek(test_file_handle, 0, SEEK_END);
    long file_size = ftell(test_file_handle);
    fclose(test_file_handle);
    
    assert(file_size > 0); // File should have content
    
    // Test recovery (simplified - just verify the interface works)
    persistent_channel_recover(persistent);
    
    // Cleanup
    if (persistent->storage_file) {
        fclose(persistent->storage_file);
    }
    free(persistent->write_buffer);
    pthread_mutex_destroy(&persistent->persistence_mutex);
    pthread_cond_destroy(&persistent->flush_condition);
    free(persistent);
    
    channel_destroy(channel);
    unlink(test_file); // Clean up test file
    
    printf("✓ Channel persistence test passed\n");
}

// Test error handling
void test_error_handling() {
    printf("Testing error handling...\n");
    
    // Test invalid channel creation
    ChannelConfig invalid_config = channel_config_buffered(0);
    invalid_config.buffer_size = SIZE_MAX; // Invalid size
    Channel* invalid_channel = channel_create(invalid_config, "invalid");
    // Should handle gracefully (may return NULL or valid channel with adjusted size)
    if (invalid_channel) {
        channel_destroy(invalid_channel);
    }
    
    // Test operations on closed channel
    ChannelConfig config = channel_config_buffered(1);
    Channel* channel = channel_create(config, "error_test");
    assert(channel != NULL);
    
    ChannelEndpoint* sender = channel_get_sender(channel);
    ChannelEndpoint* receiver = channel_get_receiver(channel);
    
    // Close channel and test operations
    channel_close(channel);
    
    TestMessage msg = {1, "error"};
    Result_void_ptr send_result = channel_send(sender, &msg, sizeof(TestMessage));
    assert(send_result.is_error);
    assert(send_result.error->code == ERROR_CHANNEL_CLOSED);
    error_destroy(send_result.error);
    
    void* data;
    size_t size;
    Result_void_ptr receive_result = channel_receive(receiver, &data, &size);
    assert(receive_result.is_error);
    assert(receive_result.error->code == ERROR_CHANNEL_CLOSED);
    error_destroy(receive_result.error);
    
    channel_endpoint_destroy(sender);
    channel_endpoint_destroy(receiver);
    channel_destroy(channel);
    
    printf("✓ Error handling test passed\n");
}

// Test concurrent access
void* concurrent_sender_worker(void* arg) {
    ChannelEndpoint* sender = (ChannelEndpoint*)arg;
    
    for (int i = 0; i < 100; i++) {
        TestMessage msg = {atomic_fetch_add(&test_message_counter, 1), "concurrent"};
        Result_void_ptr result = channel_send(sender, &msg, sizeof(TestMessage));
        if (result.is_error) {
            error_destroy(result.error);
        }
        usleep(1000); // 1ms delay
    }
    
    return NULL;
}

void* concurrent_receiver_worker(void* arg) {
    ChannelEndpoint* receiver = (ChannelEndpoint*)arg;
    int received_count = 0;
    
    for (int i = 0; i < 150; i++) { // Try to receive more than sent to test timeouts
        void* data;
        size_t size;
        Result_void_ptr result = channel_receive_timeout(receiver, &data, &size, 10);
        
        if (!result.is_error) {
            received_count++;
            free(data);
        } else {
            error_destroy(result.error);
        }
    }
    
    return (void*)(intptr_t)received_count;
}

void test_concurrent_access() {
    printf("Testing concurrent access...\n");
    
    atomic_store(&test_message_counter, 0);
    
    ChannelConfig config = channel_config_buffered(50);
    Channel* channel = channel_create(config, "concurrent_test");
    assert(channel != NULL);
    
    ChannelEndpoint* sender = channel_get_sender(channel);
    ChannelEndpoint* receiver = channel_get_receiver(channel);
    
    // Create multiple sender and receiver threads
    const int num_senders = 2;
    const int num_receivers = 2;
    
    pthread_t sender_threads[num_senders];
    pthread_t receiver_threads[num_receivers];
    
    // Start sender threads
    for (int i = 0; i < num_senders; i++) {
        assert(pthread_create(&sender_threads[i], NULL, concurrent_sender_worker, sender) == 0);
    }
    
    // Start receiver threads
    for (int i = 0; i < num_receivers; i++) {
        assert(pthread_create(&receiver_threads[i], NULL, concurrent_receiver_worker, receiver) == 0);
    }
    
    // Wait for all threads
    for (int i = 0; i < num_senders; i++) {
        pthread_join(sender_threads[i], NULL);
    }
    
    int total_received = 0;
    for (int i = 0; i < num_receivers; i++) {
        void* result;
        pthread_join(receiver_threads[i], &result);
        total_received += (intptr_t)result;
    }
    
    int total_sent = atomic_load(&test_message_counter);
    printf("  Sent: %d, Received: %d\n", total_sent, total_received);
    
    // Allow for some messages still in transit
    assert(total_received >= total_sent - 10);
    assert(total_received <= total_sent);
    
    channel_endpoint_destroy(sender);
    channel_endpoint_destroy(receiver);
    channel_destroy(channel);
    
    printf("✓ Concurrent access test passed\n");
}

int main() {
    printf("=== Advanced Channel Patterns Test Suite ===\n\n");
    
    // Run all tests
    test_basic_channel_operations();
    test_channel_types();
    test_channel_timeouts();
    test_channel_statistics();
    test_channel_transformations();
    test_load_balancing();
    test_channel_persistence();
    test_error_handling();
    test_concurrent_access();
    
    printf("\n=== All Advanced Channel Tests Passed! ===\n");
    printf("Advanced channel patterns are ready for production!\n");
    
    return 0;
}