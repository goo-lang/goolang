#ifndef GOO_ADVANCED_CHANNELS_H
#define GOO_ADVANCED_CHANNELS_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "ccomp_shim.h"
#include "ergonomic_errors.h"
#include "fearless_concurrency.h"
#include "shared_variables.h"
#include "structured_concurrency.h"

// Forward declarations
typedef struct Channel Channel;
typedef struct ChannelSelector ChannelSelector;
typedef struct ChannelBroker ChannelBroker;
typedef struct ChannelPool ChannelPool;

// Advanced channel types
typedef enum {
    CHANNEL_UNBUFFERED,     // Synchronous, zero-capacity
    CHANNEL_BUFFERED,       // Asynchronous with fixed buffer
    CHANNEL_UNBOUNDED,      // Asynchronous with dynamic buffer
    CHANNEL_PRIORITY,       // Priority-based message ordering
    CHANNEL_BROADCAST,      // One-to-many communication
    CHANNEL_MULTICAST,      // Selective broadcasting
    CHANNEL_REQUEST_REPLY,  // Synchronous request-response
    CHANNEL_PIPELINE,       // Pipelined processing
    CHANNEL_FANOUT,         // Work distribution
    CHANNEL_FANIN,          // Result aggregation
    CHANNEL_RATE_LIMITED,   // Rate-controlled communication
    CHANNEL_RELIABLE        // Guaranteed delivery with acks
} ChannelType;

// Channel ordering guarantees
typedef enum {
    CHANNEL_ORDER_NONE,     // No ordering guarantees
    CHANNEL_ORDER_FIFO,     // First-in, first-out
    CHANNEL_ORDER_LIFO,     // Last-in, first-out (stack-like)
    CHANNEL_ORDER_PRIORITY, // Priority-based ordering
    CHANNEL_ORDER_TIMESTAMP // Timestamp-based ordering
} ChannelOrdering;

// Channel reliability levels
typedef enum {
    CHANNEL_RELIABILITY_NONE,      // Best effort, no guarantees
    CHANNEL_RELIABILITY_AT_MOST_ONCE,    // No duplicates
    CHANNEL_RELIABILITY_AT_LEAST_ONCE,   // Guaranteed delivery, possible duplicates
    CHANNEL_RELIABILITY_EXACTLY_ONCE     // Guaranteed delivery, no duplicates
} ChannelReliability;

// Channel flow control strategies
typedef enum {
    FLOW_CONTROL_NONE,        // No flow control
    FLOW_CONTROL_STOP_WAIT,   // Stop and wait for ack
    FLOW_CONTROL_SLIDING_WINDOW, // Sliding window protocol
    FLOW_CONTROL_RATE_LIMIT,  // Rate limiting
    FLOW_CONTROL_BACKPRESSURE // Backpressure signaling
} FlowControlStrategy;

// Message metadata
typedef struct MessageMetadata {
    uint64_t sequence_number;
    uint64_t timestamp_ns;
    uint64_t sender_id;
    uint64_t correlation_id;
    int priority;
    uint64_t ttl_ms;           // Time to live
    bool requires_ack;
    size_t retry_count;
    
    // Routing information
    char topic[64];
    char* routing_key;
    
    // Custom headers
    struct {
        char* key;
        char* value;
    } headers[16];
    size_t header_count;
} MessageMetadata;

// Channel message
typedef struct ChannelMessage {
    void* data;
    size_t data_size;
    MessageMetadata metadata;
    
    // Internal management
    uint64_t message_id;
    bool owns_data;
    void (*data_destructor)(void* data);
    
    // Acknowledgment support
    bool ack_received;
    uint64_t ack_timeout_ms;
    pthread_cond_t ack_received_cond;
    pthread_mutex_t ack_mutex;
    
    struct ChannelMessage* next;
} ChannelMessage;

// Channel configuration
typedef struct ChannelConfig {
    ChannelType type;
    ChannelOrdering ordering;
    ChannelReliability reliability;
    FlowControlStrategy flow_control;
    
    // Capacity settings
    size_t buffer_size;
    size_t max_buffer_size;     // For unbounded channels
    size_t message_size_limit;
    
    // Timing settings
    uint64_t default_timeout_ms;
    uint64_t ack_timeout_ms;
    uint64_t message_ttl_ms;
    
    // Flow control settings
    size_t window_size;         // For sliding window
    size_t max_rate_per_second; // For rate limiting
    size_t burst_size;          // Rate limiting burst
    
    // Reliability settings
    size_t max_retries;
    uint64_t retry_interval_ms;
    bool enable_dead_letter_queue;
    
    // Broadcasting settings (for broadcast/multicast)
    size_t max_subscribers;
    bool persistent_subscription;
    
    // Performance settings
    bool use_lock_free_queue;
    bool enable_batching;
    size_t batch_size;
    uint64_t batch_timeout_ms;
    
    // Monitoring
    bool enable_metrics;
    bool enable_tracing;
    
    // Integration
    ActorSystem* actor_system;
    TaskScope* task_scope;
} ChannelConfig;

// Channel statistics
typedef struct ChannelStats {
    // Message counts
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t messages_dropped;
    uint64_t messages_expired;
    uint64_t messages_retried;
    
    // Acknowledgment stats
    uint64_t acks_sent;
    uint64_t acks_received;
    uint64_t ack_timeouts;
    
    // Timing statistics (in nanoseconds)
    uint64_t avg_send_time_ns;
    uint64_t avg_receive_time_ns;
    uint64_t avg_round_trip_time_ns;
    uint64_t max_latency_ns;
    
    // Queue statistics
    size_t current_queue_size;
    size_t peak_queue_size;
    double queue_utilization;
    
    // Flow control stats
    uint64_t flow_control_events;
    uint64_t backpressure_events;
    uint64_t rate_limit_events;
    
    // Error statistics
    uint64_t send_errors;
    uint64_t receive_errors;
    uint64_t serialization_errors;
    
    // Subscription stats (for broadcast channels)
    size_t active_subscribers;
    size_t total_subscribers;
} ChannelStats;

// Channel endpoint for sending/receiving
typedef struct ChannelEndpoint {
    Channel* channel;
    bool is_sender;
    bool is_receiver;
    uint64_t endpoint_id;
    
    // Endpoint-specific configuration
    int priority_level;
    char subscription_topic[64];
    bool auto_ack;
    
    // Statistics
    uint64_t messages_handled;
    uint64_t last_activity_time;
    
    struct ChannelEndpoint* next;
} ChannelEndpoint;

// Main channel structure
typedef struct Channel {
    uint64_t id;
    char name[64];
    ChannelConfig config;
    
    // Message storage
    ChannelMessage* message_queue;
    ChannelMessage* message_queue_tail;
    size_t current_size;
    
    // For priority channels
    ChannelMessage** priority_queues;
    size_t* priority_sizes;
    int max_priority;
    
    // Synchronization
    pthread_mutex_t channel_mutex;
    pthread_cond_t message_available;
    pthread_cond_t space_available;
    pthread_rwlock_t subscriber_lock; // For broadcast channels
    
    // Senders and receivers
    ChannelEndpoint* senders;
    ChannelEndpoint* receivers;
    size_t sender_count;
    size_t receiver_count;
    
    // Subscribers (for broadcast channels)
    ChannelEndpoint** subscribers;
    size_t subscriber_count;
    size_t subscriber_capacity;
    
    // State management
    bool is_closed;
    atomic_bool shutdown_requested;
    
    // Flow control state
    atomic_size_t window_current;
    atomic_size_t sequence_number;
    
    // Rate limiting state
    uint64_t rate_limit_window_start;
    size_t rate_limit_count;
    size_t rate_limit_tokens;
    
    // Reliability tracking
    ChannelMessage** pending_acks;
    size_t pending_ack_count;
    size_t pending_ack_capacity;
    
    // Dead letter queue
    Channel* dead_letter_queue;
    
    // Statistics
    ChannelStats stats;
    
    // Reference counting
    atomic_int ref_count;
    
    // Channel manager reference
    struct ChannelBroker* broker;
} Channel;

// Channel selector for multiplexing operations
typedef struct SelectCase {
    Channel* channel;
    ChannelEndpoint* endpoint;
    enum {
        SELECT_SEND,
        SELECT_RECEIVE,
        SELECT_DEFAULT
    } operation;
    
    // For send operations
    void* send_data;
    size_t send_size;
    
    // For receive operations
    void** receive_buffer;
    size_t* receive_size;
    
    // Result
    bool ready;
    bool executed;
    Error* error;
} SelectCase;

typedef struct ChannelSelector {
    SelectCase* cases;
    size_t case_count;
    size_t case_capacity;
    
    // Selection strategy
    enum {
        SELECT_FIRST_READY,
        SELECT_RANDOM,
        SELECT_ROUND_ROBIN,
        SELECT_PRIORITY
    } strategy;
    
    // Timeout settings
    uint64_t timeout_ms;
    bool has_default;
    
    // State
    size_t last_selected;
    atomic_bool is_selecting;
} ChannelSelector;

// Channel broker for managing multiple channels
typedef struct ChannelBroker {
    // Channel registry
    Channel** channels;
    size_t channel_count;
    size_t channel_capacity;
    pthread_mutex_t registry_mutex;
    
    // Named channels
    struct {
        char name[64];
        Channel* channel;
    } *named_channels;
    size_t named_channel_count;
    size_t named_channel_capacity;
    
    // Pattern matching for topics
    struct {
        char pattern[128];
        Channel* channel;
        bool is_regex;
    } *topic_routes;
    size_t topic_route_count;
    size_t topic_route_capacity;
    
    // Broker configuration
    bool enable_message_persistence;
    bool enable_clustering;
    size_t max_channels;
    
    // Message routing
    pthread_t routing_thread;
    bool routing_active;
    
    // Global statistics
    uint64_t total_messages_routed;
    uint64_t total_channels_created;
    uint64_t total_routing_errors;
    
    // Integration
    ActorSystem* actor_system;
    TaskScope* task_scope;
    SharedVarManager* shared_var_manager;
} ChannelBroker;

// Channel pool for efficient channel management
typedef struct ChannelPool {
    Channel** available_channels;
    size_t available_count;
    size_t pool_capacity;
    
    ChannelConfig template_config;
    char name_prefix[32];
    
    pthread_mutex_t pool_mutex;
    atomic_size_t next_channel_id;
    
    // Pool statistics
    size_t total_created;
    size_t total_acquired;
    size_t total_released;
    size_t peak_usage;
} ChannelPool;

// Core channel operations
Channel* channel_create(ChannelConfig config, const char* name);
void channel_destroy(Channel* channel);
ChannelEndpoint* channel_get_sender(Channel* channel);
ChannelEndpoint* channel_get_receiver(Channel* channel);
void channel_endpoint_destroy(ChannelEndpoint* endpoint);

// Message operations
ChannelMessage* channel_message_create(void* data, size_t data_size);
void channel_message_destroy(ChannelMessage* message);
Result_void_ptr channel_message_set_metadata(ChannelMessage* message, MessageMetadata metadata);

// Send/receive operations
Result_void_ptr channel_send(ChannelEndpoint* sender, void* data, size_t data_size);
Result_void_ptr channel_send_with_metadata(ChannelEndpoint* sender, void* data, size_t data_size, MessageMetadata metadata);
Result_void_ptr channel_send_timeout(ChannelEndpoint* sender, void* data, size_t data_size, uint64_t timeout_ms);
Result_void_ptr channel_receive(ChannelEndpoint* receiver, void** data, size_t* data_size);
Result_void_ptr channel_receive_timeout(ChannelEndpoint* receiver, void** data, size_t* data_size, uint64_t timeout_ms);
Result_void_ptr channel_receive_with_metadata(ChannelEndpoint* receiver, void** data, size_t* data_size, MessageMetadata* metadata);

// Non-blocking operations
Result_bool channel_try_send(ChannelEndpoint* sender, void* data, size_t data_size);
Result_bool channel_try_receive(ChannelEndpoint* receiver, void** data, size_t* data_size);

// Batch operations
typedef struct ChannelBatch {
    void** messages;
    size_t* message_sizes;
    MessageMetadata* metadata;
    size_t count;
    size_t capacity;
} ChannelBatch;

ChannelBatch* channel_batch_create(size_t capacity);
void channel_batch_destroy(ChannelBatch* batch);
Result_void_ptr channel_batch_add(ChannelBatch* batch, void* data, size_t data_size, MessageMetadata metadata);
Result_void_ptr channel_send_batch(ChannelEndpoint* sender, ChannelBatch* batch);
Result_void_ptr channel_receive_batch(ChannelEndpoint* receiver, ChannelBatch* batch, uint64_t timeout_ms);

// Acknowledgment operations
Result_void_ptr channel_ack_message(ChannelEndpoint* receiver, ChannelMessage* message);
Result_void_ptr channel_nack_message(ChannelEndpoint* receiver, ChannelMessage* message, bool requeue);
Result_void_ptr channel_wait_for_ack(ChannelMessage* message, uint64_t timeout_ms);

// Channel closing and cleanup
Result_void_ptr channel_close_sender(ChannelEndpoint* sender);
Result_void_ptr channel_close_receiver(ChannelEndpoint* receiver);
Result_void_ptr channel_close(Channel* channel);
bool channel_is_closed(Channel* channel);

// Channel selector operations
ChannelSelector* channel_selector_create(void);
void channel_selector_destroy(ChannelSelector* selector);
Result_void_ptr channel_selector_add_send(ChannelSelector* selector, ChannelEndpoint* sender, void* data, size_t data_size);
Result_void_ptr channel_selector_add_receive(ChannelSelector* selector, ChannelEndpoint* receiver, void** buffer, size_t* size);
Result_void_ptr channel_selector_add_default(ChannelSelector* selector);
Result_int channel_selector_select(ChannelSelector* selector, uint64_t timeout_ms);

// Broadcast and multicast operations
Result_void_ptr channel_subscribe(Channel* channel, ChannelEndpoint* subscriber, const char* topic);
Result_void_ptr channel_unsubscribe(Channel* channel, ChannelEndpoint* subscriber);
Result_void_ptr channel_publish(ChannelEndpoint* publisher, void* data, size_t data_size, const char* topic);
Result_void_ptr channel_multicast(ChannelEndpoint* sender, void* data, size_t data_size, const char** topics, size_t topic_count);

// Request-reply operations
typedef struct RequestReplyContext {
    uint64_t correlation_id;
    Channel* reply_channel;
    ChannelEndpoint* reply_endpoint;
    uint64_t timeout_ms;
    pthread_mutex_t reply_mutex;
    pthread_cond_t reply_received;
    bool reply_ready;
    void* reply_data;
    size_t reply_size;
} RequestReplyContext;

RequestReplyContext* request_reply_context_create(uint64_t timeout_ms);
void request_reply_context_destroy(RequestReplyContext* context);
Result_void_ptr channel_send_request(ChannelEndpoint* sender, void* request_data, size_t request_size, RequestReplyContext* context);
Result_void_ptr channel_send_reply(ChannelEndpoint* sender, void* reply_data, size_t reply_size, uint64_t correlation_id);
Result_void_ptr channel_wait_for_reply(RequestReplyContext* context);

// Channel broker operations
ChannelBroker* channel_broker_create(void);
void channel_broker_destroy(ChannelBroker* broker);
Result_void_ptr channel_broker_register(ChannelBroker* broker, Channel* channel);
Result_void_ptr channel_broker_register_named(ChannelBroker* broker, Channel* channel, const char* name);
Channel* channel_broker_find_by_name(ChannelBroker* broker, const char* name);
Result_void_ptr channel_broker_add_topic_route(ChannelBroker* broker, const char* pattern, Channel* channel);
Result_void_ptr channel_broker_route_message(ChannelBroker* broker, const char* topic, void* data, size_t data_size);

// Channel pool operations
ChannelPool* channel_pool_create(ChannelConfig template_config, size_t initial_size, const char* name_prefix);
void channel_pool_destroy(ChannelPool* pool);
Channel* channel_pool_acquire(ChannelPool* pool);
Result_void_ptr channel_pool_release(ChannelPool* pool, Channel* channel);

// Advanced patterns
typedef struct Pipeline {
    Channel** stages;
    size_t stage_count;
    ChannelEndpoint** stage_inputs;
    ChannelEndpoint** stage_outputs;
    
    // Pipeline workers
    ConcurrentTask** workers;
    TaskGroup* worker_group;
    
    bool is_running;
    pthread_mutex_t pipeline_mutex;
} Pipeline;

Pipeline* channel_pipeline_create(ChannelConfig* stage_configs, size_t stage_count, TaskScope* scope);
void channel_pipeline_destroy(Pipeline* pipeline);
Result_void_ptr channel_pipeline_start(Pipeline* pipeline);
Result_void_ptr channel_pipeline_stop(Pipeline* pipeline);
Result_void_ptr channel_pipeline_push(Pipeline* pipeline, void* data, size_t data_size);
Result_void_ptr channel_pipeline_pop(Pipeline* pipeline, void** data, size_t* data_size, uint64_t timeout_ms);

// Fan-out/Fan-in patterns
typedef struct FanOutConfig {
    size_t worker_count;
    ChannelConfig worker_channel_config;
    bool round_robin_distribution;
    bool load_balancing;
} FanOutConfig;

typedef struct FanOut {
    Channel* input_channel;
    Channel** worker_channels;
    ChannelEndpoint* input_receiver;
    ChannelEndpoint** worker_senders;
    size_t worker_count;
    
    // Distribution state
    atomic_size_t next_worker;
    atomic_size_t* worker_load;
    
    ConcurrentTask* distributor_task;
    bool is_active;
} FanOut;

FanOut* channel_fanout_create(Channel* input, FanOutConfig config, TaskScope* scope);
void channel_fanout_destroy(FanOut* fanout);
Result_void_ptr channel_fanout_start(FanOut* fanout);
Result_void_ptr channel_fanout_stop(FanOut* fanout);
Channel** channel_fanout_get_worker_channels(FanOut* fanout, size_t* count);

typedef struct FanIn {
    Channel** input_channels;
    Channel* output_channel;
    ChannelEndpoint** input_receivers;
    ChannelEndpoint* output_sender;
    size_t input_count;
    
    ConcurrentTask* aggregator_task;
    bool is_active;
} FanIn;

FanIn* channel_fanin_create(Channel** inputs, size_t input_count, Channel* output, TaskScope* scope);
void channel_fanin_destroy(FanIn* fanin);
Result_void_ptr channel_fanin_start(FanIn* fanin);
Result_void_ptr channel_fanin_stop(FanIn* fanin);

// Configuration helpers
ChannelConfig channel_config_default(void);
ChannelConfig channel_config_unbuffered(void);
ChannelConfig channel_config_buffered(size_t buffer_size);
ChannelConfig channel_config_broadcast(size_t max_subscribers);
ChannelConfig channel_config_reliable(void);
ChannelConfig channel_config_high_throughput(void);
ChannelConfig channel_config_low_latency(void);

// Statistics and monitoring
ChannelStats channel_get_stats(Channel* channel);
void channel_reset_stats(Channel* channel);

typedef struct ChannelBrokerStats {
    uint64_t total_channels;
    uint64_t active_channels;
    uint64_t total_messages_routed;
    uint64_t routing_errors;
    uint64_t avg_routing_time_ns;
    size_t active_subscriptions;
} ChannelBrokerStats;

ChannelBrokerStats channel_broker_get_stats(ChannelBroker* broker);
void channel_broker_reset_stats(ChannelBroker* broker);

// Integration with other systems
Result_void_ptr channel_integrate_with_actors(Channel* channel, ActorSystem* actor_system);
Result_void_ptr channel_integrate_with_tasks(Channel* channel, TaskScope* task_scope);
Result_void_ptr channel_integrate_with_shared_vars(Channel* channel, SharedVarManager* var_manager);

// Utility macros
#define CHANNEL_SEND(endpoint, data) \
    channel_send(endpoint, &(data), sizeof(data))

#define CHANNEL_RECEIVE(endpoint, data_ptr) \
    do { \
        void* __recv_data; \
        size_t __recv_size; \
        Result_void_ptr __recv_result = channel_receive(endpoint, &__recv_data, &__recv_size); \
        if (!__recv_result.is_error && __recv_size == sizeof(*(data_ptr))) { \
            *(data_ptr) = *(typeof(*(data_ptr))*)__recv_data; \
            free(__recv_data); \
        } \
    } while(0)

#define CHANNEL_SELECT_BEGIN(selector) \
    do { \
        ChannelSelector* __sel = selector; \
        int __selected_case = channel_selector_select(__sel, UINT64_MAX);

#define CHANNEL_SELECT_SEND(endpoint, data) \
        channel_selector_add_send(__sel, endpoint, &(data), sizeof(data));

#define CHANNEL_SELECT_RECEIVE(endpoint, data_ptr) \
        channel_selector_add_receive(__sel, endpoint, (void**)(data_ptr), NULL);

#define CHANNEL_SELECT_DEFAULT() \
        channel_selector_add_default(__sel);

#define CHANNEL_SELECT_END() \
    } while(0)

// Error codes specific to channels
#define ERROR_CHANNEL_CLOSED        0x3001
#define ERROR_CHANNEL_FULL          0x3002
#define ERROR_CHANNEL_EMPTY         0x3003
#define ERROR_CHANNEL_TIMEOUT       0x3004
#define ERROR_CHANNEL_INVALID_TYPE  0x3005
#define ERROR_CHANNEL_NO_SUBSCRIBERS 0x3006
#define ERROR_CHANNEL_ACK_TIMEOUT   0x3007

#endif // GOO_ADVANCED_CHANNELS_H