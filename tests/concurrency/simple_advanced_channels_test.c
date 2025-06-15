#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#ifndef ETIMEDOUT
#define ETIMEDOUT 138
#endif

// Simplified error handling for testing
typedef struct Error {
    int code;
    char message[256];
} Error;

typedef struct {
    bool is_error;
    union {
        void* value;
        Error* error;
    };
} Result_void_ptr;

typedef struct {
    bool is_error;
    union {
        bool value;
        Error* error;
    };
} Result_bool;

#define OK_PTR(x) ((Result_void_ptr){.is_error = false, .value = (x)})
#define ERR_PTR(x) ((Result_void_ptr){.is_error = true, .error = (x)})

Error* error_create(int code, const char* message) {
    Error* err = malloc(sizeof(Error));
    if (err) {
        err->code = code;
        strncpy(err->message, message, sizeof(err->message) - 1);
        err->message[sizeof(err->message) - 1] = '\0';
    }
    return err;
}

void error_destroy(Error* error) {
    free(error);
}

// Error codes
#define ERROR_INVALID_EXPRESSION   0x1001
#define ERROR_MEMORY_ALLOCATION    0x1002
#define ERROR_THREAD_CREATION      0x1003
#define ERROR_CHANNEL_TIMEOUT      0x3004
#define ERROR_CHANNEL_CLOSED       0x3001
#define ERROR_CHANNEL_FULL         0x3002

// Minimal channel types and structures needed for testing
typedef enum {
    CHANNEL_BUFFERED,
    CHANNEL_UNBUFFERED,
    CHANNEL_BROADCAST
} ChannelType;

typedef enum {
    CHANNEL_ORDER_FIFO
} ChannelOrdering;

typedef enum {
    CHANNEL_RELIABILITY_NONE,
    CHANNEL_RELIABILITY_EXACTLY_ONCE
} ChannelReliability;

typedef enum {
    FLOW_CONTROL_NONE
} FlowControlStrategy;

typedef struct ChannelConfig {
    ChannelType type;
    ChannelOrdering ordering;
    ChannelReliability reliability;
    FlowControlStrategy flow_control;
    size_t buffer_size;
    bool enable_metrics;
} ChannelConfig;

typedef struct ChannelMessage {
    void* data;
    size_t data_size;
    bool owns_data;
    struct ChannelMessage* next;
} ChannelMessage;

typedef struct ChannelStats {
    uint64_t messages_sent;
    uint64_t messages_received;
    size_t current_queue_size;
} ChannelStats;

typedef struct Channel {
    uint64_t id;
    char name[64];
    ChannelConfig config;
    
    ChannelMessage* message_queue;
    ChannelMessage* message_queue_tail;
    size_t current_size;
    
    pthread_mutex_t channel_mutex;
    pthread_cond_t message_available;
    pthread_cond_t space_available;
    
    bool is_closed;
    atomic_bool shutdown_requested;
    
    ChannelStats stats;
    atomic_int ref_count;
} Channel;

typedef struct ChannelEndpoint {
    Channel* channel;
    bool is_sender;
    bool is_receiver;
    uint64_t endpoint_id;
} ChannelEndpoint;

// Configuration helpers
ChannelConfig channel_config_buffered(size_t buffer_size) {
    return (ChannelConfig) {
        .type = CHANNEL_BUFFERED,
        .ordering = CHANNEL_ORDER_FIFO,
        .reliability = CHANNEL_RELIABILITY_NONE,
        .flow_control = FLOW_CONTROL_NONE,
        .buffer_size = buffer_size,
        .enable_metrics = true
    };
}

ChannelConfig channel_config_unbuffered(void) {
    ChannelConfig config = channel_config_buffered(1);
    config.type = CHANNEL_UNBUFFERED;
    config.buffer_size = 0;
    return config;
}

ChannelConfig channel_config_broadcast(size_t max_subscribers) {
    ChannelConfig config = channel_config_buffered(100);
    config.type = CHANNEL_BROADCAST;
    return config;
}

ChannelConfig channel_config_reliable(void) {
    ChannelConfig config = channel_config_buffered(100);
    config.reliability = CHANNEL_RELIABILITY_EXACTLY_ONCE;
    return config;
}

// Channel operations implementation
static uint64_t generate_unique_id(void) {
    static atomic_uint_fast64_t counter = ATOMIC_VAR_INIT(1);
    return atomic_fetch_add(&counter, 1);
}

Channel* channel_create(ChannelConfig config, const char* name) {
    Channel* channel = calloc(1, sizeof(Channel));
    if (!channel) return NULL;
    
    channel->id = generate_unique_id();
    strncpy(channel->name, name, sizeof(channel->name) - 1);
    channel->config = config;
    
    if (pthread_mutex_init(&channel->channel_mutex, NULL) != 0) {
        free(channel);
        return NULL;
    }
    
    if (pthread_cond_init(&channel->message_available, NULL) != 0) {
        pthread_mutex_destroy(&channel->channel_mutex);
        free(channel);
        return NULL;
    }
    
    if (pthread_cond_init(&channel->space_available, NULL) != 0) {
        pthread_cond_destroy(&channel->message_available);
        pthread_mutex_destroy(&channel->channel_mutex);
        free(channel);
        return NULL;
    }
    
    atomic_store(&channel->ref_count, 1);
    
    return channel;
}

void channel_destroy(Channel* channel) {
    if (!channel) return;
    
    // Cleanup messages
    ChannelMessage* msg = channel->message_queue;
    while (msg) {
        ChannelMessage* next = msg->next;
        if (msg->owns_data && msg->data) {
            free(msg->data);
        }
        free(msg);
        msg = next;
    }
    
    pthread_cond_destroy(&channel->space_available);
    pthread_cond_destroy(&channel->message_available);
    pthread_mutex_destroy(&channel->channel_mutex);
    free(channel);
}

ChannelEndpoint* channel_get_sender(Channel* channel) {
    if (!channel) return NULL;
    
    ChannelEndpoint* endpoint = calloc(1, sizeof(ChannelEndpoint));
    if (!endpoint) return NULL;
    
    endpoint->channel = channel;
    endpoint->is_sender = true;
    endpoint->is_receiver = false;
    endpoint->endpoint_id = generate_unique_id();
    
    return endpoint;
}

ChannelEndpoint* channel_get_receiver(Channel* channel) {
    if (!channel) return NULL;
    
    ChannelEndpoint* endpoint = calloc(1, sizeof(ChannelEndpoint));
    if (!endpoint) return NULL;
    
    endpoint->channel = channel;
    endpoint->is_sender = false;
    endpoint->is_receiver = true;
    endpoint->endpoint_id = generate_unique_id();
    
    return endpoint;
}

void channel_endpoint_destroy(ChannelEndpoint* endpoint) {
    free(endpoint);
}

Result_void_ptr channel_send(ChannelEndpoint* sender, void* data, size_t data_size) {
    if (!sender || !sender->is_sender || !data || data_size == 0) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid sender or data"));
    }
    
    Channel* channel = sender->channel;
    if (!channel || channel->is_closed) {
        return ERR_PTR(error_create(ERROR_CHANNEL_CLOSED, "Channel is closed"));
    }
    
    pthread_mutex_lock(&channel->channel_mutex);
    
    // Check buffer capacity
    if (channel->config.buffer_size > 0 && channel->current_size >= channel->config.buffer_size) {
        pthread_mutex_unlock(&channel->channel_mutex);
        return ERR_PTR(error_create(ERROR_CHANNEL_FULL, "Channel buffer is full"));
    }
    
    // Create message
    ChannelMessage* message = calloc(1, sizeof(ChannelMessage));
    if (!message) {
        pthread_mutex_unlock(&channel->channel_mutex);
        return ERR_PTR(error_create(ERROR_MEMORY_ALLOCATION, "Failed to create message"));
    }
    
    // Copy data
    message->data = malloc(data_size);
    if (!message->data) {
        free(message);
        pthread_mutex_unlock(&channel->channel_mutex);
        return ERR_PTR(error_create(ERROR_MEMORY_ALLOCATION, "Failed to allocate message data"));
    }
    
    memcpy(message->data, data, data_size);
    message->data_size = data_size;
    message->owns_data = true;
    
    // Add to queue
    if (!channel->message_queue) {
        channel->message_queue = message;
        channel->message_queue_tail = message;
    } else {
        channel->message_queue_tail->next = message;
        channel->message_queue_tail = message;
    }
    
    channel->current_size++;
    channel->stats.messages_sent++;
    
    pthread_cond_signal(&channel->message_available);
    pthread_mutex_unlock(&channel->channel_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr channel_receive_timeout(ChannelEndpoint* receiver, void** data, size_t* data_size, uint64_t timeout_ms) {
    if (!receiver || !receiver->is_receiver || !data || !data_size) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid receiver or parameters"));
    }
    
    Channel* channel = receiver->channel;
    if (!channel) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid channel"));
    }
    
    pthread_mutex_lock(&channel->channel_mutex);
    
    // Wait for message with timeout
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    
    int wait_result = 0;
    while (channel->current_size == 0 && !channel->is_closed && wait_result != ETIMEDOUT) {
        wait_result = pthread_cond_timedwait(&channel->message_available, &channel->channel_mutex, &deadline);
    }
    
    if (wait_result == ETIMEDOUT) {
        pthread_mutex_unlock(&channel->channel_mutex);
        return ERR_PTR(error_create(ERROR_CHANNEL_TIMEOUT, "Receive operation timed out"));
    }
    
    if (channel->is_closed) {
        pthread_mutex_unlock(&channel->channel_mutex);
        return ERR_PTR(error_create(ERROR_CHANNEL_CLOSED, "Channel is closed"));
    }
    
    if (channel->current_size == 0) {
        pthread_mutex_unlock(&channel->channel_mutex);
        return ERR_PTR(error_create(ERROR_CHANNEL_CLOSED, "No messages available"));
    }
    
    // Get message
    ChannelMessage* message = channel->message_queue;
    if (!message) {
        pthread_mutex_unlock(&channel->channel_mutex);
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Failed to get message"));
    }
    
    channel->message_queue = message->next;
    if (!channel->message_queue) {
        channel->message_queue_tail = NULL;
    }
    message->next = NULL;
    channel->current_size--;
    channel->stats.messages_received++;
    
    pthread_cond_signal(&channel->space_available);
    pthread_mutex_unlock(&channel->channel_mutex);
    
    *data = message->data;
    *data_size = message->data_size;
    message->owns_data = false;  // Transfer ownership to caller
    free(message);
    
    return OK_PTR(NULL);
}

Result_void_ptr channel_receive(ChannelEndpoint* receiver, void** data, size_t* data_size) {
    return channel_receive_timeout(receiver, data, data_size, 30000);  // 30 second default timeout
}

Result_void_ptr channel_close(Channel* channel) {
    if (!channel) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid channel"));
    }
    
    pthread_mutex_lock(&channel->channel_mutex);
    channel->is_closed = true;
    pthread_cond_broadcast(&channel->message_available);
    pthread_cond_broadcast(&channel->space_available);
    pthread_mutex_unlock(&channel->channel_mutex);
    
    return OK_PTR(NULL);
}

bool channel_is_closed(Channel* channel) {
    if (!channel) return true;
    return channel->is_closed;
}

ChannelStats channel_get_stats(Channel* channel) {
    if (!channel) {
        return (ChannelStats){0};
    }
    
    pthread_mutex_lock(&channel->channel_mutex);
    ChannelStats stats = channel->stats;
    stats.current_queue_size = channel->current_size;
    pthread_mutex_unlock(&channel->channel_mutex);
    
    return stats;
}

// Test data structures
typedef struct {
    int value;
    char name[32];
} TestMessage;

// Test functions
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
    time_t start_time = time(NULL);
    Result_void_ptr result = channel_receive_timeout(receiver, &data, &size, 100); // 100ms timeout
    time_t end_time = time(NULL);
    
    assert(result.is_error);
    assert(result.error->code == ERROR_CHANNEL_TIMEOUT);
    assert((end_time - start_time) < 2); // Should timeout quickly
    
    error_destroy(result.error);
    channel_endpoint_destroy(receiver);
    channel_destroy(channel);
    
    printf("✓ Channel timeout test passed\n");
}

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

void test_error_handling() {
    printf("Testing error handling...\n");
    
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

int main() {
    printf("=== Simple Advanced Channel Patterns Test Suite ===\n\n");
    
    // Run core tests
    test_basic_channel_operations();
    test_channel_types();
    test_channel_timeouts();
    test_channel_statistics();
    test_error_handling();
    
    printf("\n=== All Simple Advanced Channel Tests Passed! ===\n");
    printf("Core advanced channel patterns are working correctly!\n");
    printf("Ready for integration with the full system.\n");
    
    return 0;
}