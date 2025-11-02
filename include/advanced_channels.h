#ifndef GOO_ADVANCED_CHANNELS_H
#define GOO_ADVANCED_CHANNELS_H

#include "runtime.h"
#include "actor_system.h"
#include "structured_concurrency.h"
#include "shared_variables.h"
#include "error_hierarchies.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

// =============================================================================
// Advanced Channel Patterns System
// =============================================================================

// Forward declarations
typedef struct AdvancedChannel AdvancedChannel;
typedef struct ChannelMessage ChannelMessage;
typedef struct ChannelNode ChannelNode;
typedef struct DistributedChannel DistributedChannel;
typedef struct LoadBalancer LoadBalancer;
typedef struct ChannelSelector ChannelSelector;
typedef struct ChannelMultiplexer ChannelMultiplexer;
typedef struct ChannelBroadcaster ChannelBroadcaster;
typedef struct ChannelPipeline ChannelPipeline;

// =============================================================================
// Channel Types and Configurations
// =============================================================================

// Types of advanced channels
typedef enum {
    CHANNEL_TYPE_BASIC,         // Basic FIFO channel
    CHANNEL_TYPE_PRIORITY,      // Priority-based channel
    CHANNEL_TYPE_DISTRIBUTED,   // Distributed across nodes
    CHANNEL_TYPE_LOAD_BALANCED, // Load-balanced delivery
    CHANNEL_TYPE_BROADCAST,     // Broadcast to all subscribers
    CHANNEL_TYPE_PIPELINE,      // Pipeline processing
    CHANNEL_TYPE_STREAM,        // Streaming data channel
    CHANNEL_TYPE_REQUEST_REPLY, // Request-reply pattern
    CHANNEL_TYPE_PUBSUB,        // Publish-subscribe
    CHANNEL_TYPE_RING,          // Ring buffer channel
    CHANNEL_TYPE_ADAPTIVE       // Adaptive behavior channel
} AdvancedChannelType;

// Channel consistency models for distributed channels
typedef enum {
    CONSISTENCY_EVENTUAL,       // Eventual consistency
    CONSISTENCY_STRONG,         // Strong consistency
    CONSISTENCY_BOUNDED_STALENESS, // Bounded staleness
    CONSISTENCY_SESSION,        // Session consistency
    CONSISTENCY_CAUSAL          // Causal consistency
} ConsistencyModel;

// Load balancing strategies
typedef enum {
    LOAD_BALANCE_ROUND_ROBIN,   // Round-robin distribution
    LOAD_BALANCE_LEAST_LOADED,  // Send to least loaded node
    LOAD_BALANCE_WEIGHTED,      // Weighted distribution
    LOAD_BALANCE_HASH,          // Hash-based routing
    LOAD_BALANCE_RANDOM,        // Random selection
    LOAD_BALANCE_LOCALITY,      // Locality-aware routing
    LOAD_BALANCE_ADAPTIVE       // Adaptive load balancing
} LoadBalanceStrategy;

// Channel ordering guarantees
typedef enum {
    ORDER_NONE,                 // No ordering guarantee
    ORDER_FIFO,                 // First-in-first-out
    ORDER_PRIORITY,             // Priority-based ordering
    ORDER_TIMESTAMP,            // Timestamp-based ordering
    ORDER_CAUSAL,               // Causal ordering
    ORDER_TOTAL                 // Total ordering
} OrderingGuarantee;

// Message delivery guarantees
typedef enum {
    DELIVERY_AT_MOST_ONCE,      // At most once delivery
    DELIVERY_AT_LEAST_ONCE,     // At least once delivery
    DELIVERY_EXACTLY_ONCE,      // Exactly once delivery
    DELIVERY_BEST_EFFORT        // Best effort delivery
} DeliveryGuarantee;

// =============================================================================
// Channel Message Structure
// =============================================================================

// Priority levels for messages
typedef enum {
    MSG_PRIORITY_CRITICAL = 0,  // Critical priority
    MSG_PRIORITY_HIGH = 25,     // High priority
    MSG_PRIORITY_NORMAL = 50,   // Normal priority
    MSG_PRIORITY_LOW = 75,      // Low priority
    MSG_PRIORITY_BACKGROUND = 100 // Background priority
} MessagePriority;

// Message routing information
typedef struct MessageRoute {
    uint64_t source_node_id;    // Source node ID
    uint64_t target_node_id;    // Target node ID
    const char* routing_key;    // Routing key for topic-based routing
    uint32_t hash_key;          // Hash key for hash-based routing
    double weight;              // Weight for weighted routing
    
    // Locality information
    const char* region;         // Geographic region
    const char* datacenter;     // Datacenter location
    const char* rack;           // Rack location
    
} MessageRoute;

// Message metadata for advanced features
typedef struct MessageMetadata {
    uint64_t sequence_number;   // Sequence number
    uint64_t timestamp_ns;      // Timestamp in nanoseconds
    uint64_t expiry_time_ns;    // Message expiry time
    uint32_t retry_count;       // Number of retries
    uint32_t max_retries;       // Maximum retries allowed
    
    // Tracing and debugging
    uint64_t trace_id;          // Distributed tracing ID
    uint64_t span_id;           // Span ID for tracing
    const char* correlation_id; // Correlation ID
    
    // Delivery tracking
    uint64_t delivery_attempt;  // Delivery attempt number
    uint64_t last_delivery_time; // Last delivery attempt time
    
} MessageMetadata;

// Advanced channel message
typedef struct ChannelMessage {
    uint64_t message_id;        // Unique message ID
    MessagePriority priority;   // Message priority
    
    // Message content
    void* data;                 // Message data
    size_t data_size;           // Data size in bytes
    void (*data_destructor)(void*); // Data cleanup function
    bool owns_data;             // Whether message owns the data
    
    // Type information
    const char* message_type;   // Message type identifier
    uint32_t type_hash;         // Type hash for fast comparison
    
    // Routing and metadata
    MessageRoute route;         // Routing information
    MessageMetadata metadata;   // Message metadata
    
    // Acknowledgment and callbacks
    void (*ack_callback)(struct ChannelMessage* msg, bool success, void* context);
    void* ack_context;          // Acknowledgment context
    
    // Reference counting for shared messages
    atomic_int ref_count;       // Reference count
    pthread_mutex_t ref_mutex;  // Reference count protection
    
    // Memory management
    Arena* message_arena;       // Message-specific arena
    
} ChannelMessage;

// =============================================================================
// Channel Configuration
// =============================================================================

// Basic channel configuration
typedef struct ChannelConfig {
    const char* name;           // Channel name
    AdvancedChannelType type;   // Channel type
    
    // Capacity and buffering
    size_t capacity;            // Channel capacity (0 = unbounded)
    size_t max_message_size;    // Maximum message size
    bool drop_on_overflow;      // Drop messages when full
    
    // Ordering and delivery
    OrderingGuarantee ordering; // Ordering guarantee
    DeliveryGuarantee delivery; // Delivery guarantee
    
    // Timeouts
    uint64_t send_timeout_ms;   // Send timeout
    uint64_t receive_timeout_ms; // Receive timeout
    uint64_t message_ttl_ms;    // Message time-to-live
    
    // Retry and error handling
    uint32_t max_retries;       // Maximum retry attempts
    uint64_t retry_delay_ms;    // Delay between retries
    bool exponential_backoff;   // Use exponential backoff
    
    // Performance tuning
    bool use_memory_pool;       // Use memory pool for messages
    size_t memory_pool_size;    // Memory pool size
    bool batch_operations;      // Batch send/receive operations
    size_t batch_size;          // Batch size
    
    // Monitoring
    bool enable_metrics;        // Enable performance metrics
    bool enable_tracing;        // Enable distributed tracing
    
} ChannelConfig;

// Distributed channel configuration
typedef struct DistributedChannelConfig {
    ChannelConfig base;         // Base channel config
    
    // Cluster configuration
    uint32_t replica_count;     // Number of replicas
    ConsistencyModel consistency; // Consistency model
    double replication_factor;  // Replication factor (0.0-1.0)
    
    // Load balancing
    LoadBalanceStrategy load_balance; // Load balancing strategy
    double* node_weights;       // Node weights for weighted balancing
    uint32_t node_count;        // Number of nodes
    
    // Failure handling
    bool auto_failover;         // Automatic failover
    uint64_t failure_detection_timeout_ms; // Failure detection timeout
    uint64_t recovery_timeout_ms; // Recovery timeout
    
    // Network configuration
    uint16_t base_port;         // Base port for communication
    const char* multicast_group; // Multicast group address
    bool use_compression;       // Use message compression
    const char* encryption_key; // Encryption key (if any)
    
} DistributedChannelConfig;

// =============================================================================
// Channel Statistics and Monitoring
// =============================================================================

// Channel performance statistics
typedef struct ChannelStats {
    // Message counts
    uint64_t messages_sent;     // Total messages sent
    uint64_t messages_received; // Total messages received
    uint64_t messages_dropped;  // Messages dropped
    uint64_t messages_expired;  // Messages that expired
    uint64_t messages_retried;  // Messages retried
    
    // Timing statistics
    double avg_send_time_ns;    // Average send time
    double avg_receive_time_ns; // Average receive time
    double avg_queue_time_ns;   // Average time in queue
    
    // Queue statistics
    size_t current_queue_size;  // Current queue size
    size_t peak_queue_size;     // Peak queue size
    double queue_utilization;   // Queue utilization (0.0-1.0)
    
    // Error statistics
    uint64_t send_failures;     // Send failures
    uint64_t receive_failures;  // Receive failures
    uint64_t timeout_errors;    // Timeout errors
    uint64_t overflow_errors;   // Buffer overflow errors
    
    // Network statistics (for distributed channels)
    uint64_t bytes_sent;        // Total bytes sent
    uint64_t bytes_received;    // Total bytes received
    uint64_t network_errors;    // Network errors
    double avg_latency_ms;      // Average network latency
    
} ChannelStats;

// Load balancer statistics
typedef struct LoadBalancerStats {
    double* node_loads;         // Current load per node
    uint64_t* messages_per_node; // Messages sent to each node
    uint64_t* errors_per_node;  // Errors per node
    double* response_times;     // Average response time per node
    uint32_t active_nodes;      // Number of active nodes
    uint32_t failed_nodes;      // Number of failed nodes
    
} LoadBalancerStats;

// =============================================================================
// Basic Advanced Channel Structure
// =============================================================================

// Core advanced channel structure
typedef struct AdvancedChannel {
    uint64_t channel_id;        // Unique channel ID
    const char* name;           // Channel name
    AdvancedChannelType type;   // Channel type
    
    // Configuration
    ChannelConfig config;       // Channel configuration
    
    // Message storage
    ChannelMessage** messages;  // Message buffer
    size_t capacity;            // Buffer capacity
    atomic_size_t head;         // Head pointer (for consumers)
    atomic_size_t tail;         // Tail pointer (for producers)
    atomic_size_t size;         // Current size
    
    // Priority queue (for priority channels)
    ChannelMessage** priority_heap; // Priority heap
    size_t heap_size;           // Heap size
    size_t heap_capacity;       // Heap capacity
    
    // Synchronization
    pthread_mutex_t send_mutex; // Send operation mutex
    pthread_mutex_t recv_mutex; // Receive operation mutex
    pthread_cond_t not_empty;   // Not empty condition
    pthread_cond_t not_full;    // Not full condition
    
    // State management
    atomic_bool is_closed;      // Channel closed flag
    atomic_int sender_count;    // Number of active senders
    atomic_int receiver_count;  // Number of active receivers
    
    // Subscribers and selectors
    struct ChannelSubscriber** subscribers; // Subscribers list
    int subscriber_count;       // Number of subscribers
    ChannelSelector** selectors; // Associated selectors
    int selector_count;         // Number of selectors
    
    // Statistics and monitoring
    ChannelStats stats;         // Performance statistics
    pthread_mutex_t stats_mutex; // Statistics protection
    
    // Integration
    ConcurrencyScope* parent_scope; // Parent concurrency scope
    ActorRef* owner_actor;      // Owner actor (if any)
    
    // Memory management
    Arena* channel_arena;       // Channel-specific memory
    
    // Extensions for specific channel types
    union {
        DistributedChannel* distributed; // Distributed channel data
        LoadBalancer* load_balancer;     // Load balancer data
        ChannelBroadcaster* broadcaster; // Broadcaster data
        ChannelPipeline* pipeline;       // Pipeline data
    } ext;
    
} AdvancedChannel;

// =============================================================================
// Distributed Channel Implementation
// =============================================================================

// Distributed channel node information
typedef struct ChannelNode {
    uint64_t node_id;           // Unique node ID
    const char* hostname;       // Node hostname
    uint16_t port;              // Node port
    
    // Node status
    enum {
        NODE_STATUS_ACTIVE,     // Node is active
        NODE_STATUS_INACTIVE,   // Node is inactive
        NODE_STATUS_FAILED,     // Node has failed
        NODE_STATUS_RECOVERING  // Node is recovering
    } status;
    
    // Load information
    double current_load;        // Current load (0.0-1.0)
    uint64_t message_count;     // Messages processed
    uint64_t error_count;       // Error count
    double avg_response_time_ms; // Average response time
    
    // Network information
    uint64_t last_heartbeat;    // Last heartbeat timestamp
    uint64_t bytes_sent;        // Bytes sent to this node
    uint64_t bytes_received;    // Bytes received from this node
    
    // Connection management
    int socket_fd;              // Socket file descriptor
    pthread_mutex_t send_mutex; // Send mutex for this node
    
} ChannelNode;

// Distributed channel structure
typedef struct DistributedChannel {
    AdvancedChannel* base;      // Base channel
    DistributedChannelConfig config; // Distributed config
    
    // Cluster management
    ChannelNode* nodes;         // Array of nodes
    uint32_t node_count;        // Number of nodes
    uint32_t active_nodes;      // Number of active nodes
    uint64_t local_node_id;     // This node's ID
    
    // Consensus and coordination
    uint64_t current_term;      // Current consensus term
    uint64_t commit_index;      // Commit index
    uint64_t last_applied;      // Last applied index
    ChannelNode* leader_node;   // Current leader node
    
    // Message replication
    struct ReplicationState {
        uint64_t* next_index;   // Next index to send to each node
        uint64_t* match_index;  // Highest index replicated on each node
        atomic_bool* replication_done; // Replication completion flags
    } replication;
    
    // Failure detection
    pthread_t heartbeat_thread; // Heartbeat thread
    bool heartbeat_active;      // Heartbeat thread active
    uint64_t failure_detection_interval_ms; // Failure detection interval
    
    // Load balancing
    LoadBalancer* load_balancer; // Load balancer instance
    
    // Network layer
    pthread_t listener_thread;  // Network listener thread
    bool listener_active;       // Listener thread active
    
    // Message routing
    struct MessageRouter {
        uint32_t (*hash_function)(const void* key, size_t key_len);
        bool (*routing_predicate)(ChannelMessage* msg, ChannelNode* node);
        void* routing_context;
    } router;
    
} DistributedChannel;

// =============================================================================
// Load Balancer Implementation
// =============================================================================

// Load balancer structure
typedef struct LoadBalancer {
    LoadBalanceStrategy strategy; // Load balancing strategy
    
    // Node management
    ChannelNode* nodes;         // Array of nodes
    uint32_t node_count;        // Number of nodes
    atomic_uint current_index;  // Current index for round-robin
    
    // Weighted balancing
    double* node_weights;       // Node weights
    double total_weight;        // Total weight
    
    // Hash-based routing
    uint32_t (*hash_function)(const void* data, size_t size);
    bool use_consistent_hashing; // Use consistent hashing
    
    // Adaptive balancing
    struct AdaptiveState {
        double* response_times;  // Response time history
        double* error_rates;     // Error rate per node
        uint64_t* last_update;   // Last update timestamp per node
        double learning_rate;    // Learning rate for adaptation
    } adaptive;
    
    // Statistics
    LoadBalancerStats stats;    // Load balancer statistics
    pthread_mutex_t stats_mutex; // Statistics protection
    
} LoadBalancer;

// =============================================================================
// Channel Selectors and Multiplexing
// =============================================================================

// Channel selection operation
typedef enum {
    SELECT_OP_SEND,             // Send operation
    SELECT_OP_RECEIVE,          // Receive operation
    SELECT_OP_SEND_OR_RECV      // Either send or receive
} SelectOperation;

// Channel selector case
typedef struct SelectCase {
    AdvancedChannel* channel;   // Channel to operate on
    SelectOperation operation;  // Operation type
    ChannelMessage* message;    // Message for send operations
    void** result;              // Result pointer for receive operations
    bool is_ready;              // Whether case is ready
    
} SelectCase;

// Channel selector for multiplexing operations
typedef struct ChannelSelector {
    uint64_t selector_id;       // Unique selector ID
    
    // Selection cases
    SelectCase* cases;          // Array of selection cases
    int case_count;             // Number of cases
    int selected_case;          // Selected case index (-1 if none)
    
    // Default case
    bool has_default;           // Whether there's a default case
    void (*default_handler)(void* context); // Default handler
    void* default_context;      // Default context
    
    // Timeout
    uint64_t timeout_ms;        // Selection timeout
    bool timed_out;             // Whether selection timed out
    
    // Synchronization
    pthread_mutex_t select_mutex; // Selection mutex
    pthread_cond_t ready_cond;  // Ready condition
    
    // Result
    bool selection_complete;    // Selection completed
    int selected_index;         // Index of selected case
    
} ChannelSelector;

// Channel multiplexer for fan-in/fan-out
typedef struct ChannelMultiplexer {
    uint64_t multiplexer_id;    // Unique multiplexer ID
    
    // Input channels (fan-in)
    AdvancedChannel** input_channels; // Input channels
    int input_count;            // Number of input channels
    
    // Output channels (fan-out)
    AdvancedChannel** output_channels; // Output channels
    int output_count;           // Number of output channels
    
    // Multiplexing strategy
    enum {
        MUX_ROUND_ROBIN,        // Round-robin distribution
        MUX_BROADCAST,          // Broadcast to all outputs
        MUX_HASH_BASED,         // Hash-based routing
        MUX_PRIORITY_BASED,     // Priority-based routing
        MUX_LOAD_BALANCED       // Load-balanced routing
    } strategy;
    
    // Processing function
    ChannelMessage* (*processor)(ChannelMessage* input, void* context);
    void* processor_context;    // Processor context
    
    // Worker threads
    pthread_t* worker_threads;  // Worker threads
    int worker_count;           // Number of workers
    bool workers_active;        // Workers active flag
    
    // Statistics
    struct {
        uint64_t messages_processed; // Messages processed
        uint64_t processing_errors;  // Processing errors
        double avg_processing_time_ns; // Average processing time
    } stats;
    
} ChannelMultiplexer;

// =============================================================================
// Broadcasting and Publishing
// =============================================================================

// Channel subscriber
typedef struct ChannelSubscriber {
    uint64_t subscriber_id;     // Unique subscriber ID
    const char* topic_filter;   // Topic filter (regex or glob)
    AdvancedChannel* delivery_channel; // Delivery channel
    
    // Subscriber configuration
    bool persistent;            // Persistent subscription
    uint64_t subscription_time; // Subscription timestamp
    MessagePriority min_priority; // Minimum message priority
    
    // Delivery options
    DeliveryGuarantee delivery_guarantee; // Delivery guarantee
    uint64_t max_queue_size;    // Maximum queue size for subscriber
    bool drop_on_overflow;      // Drop messages on overflow
    
    // Statistics
    uint64_t messages_delivered; // Messages delivered
    uint64_t messages_dropped;   // Messages dropped
    uint64_t last_delivery_time; // Last delivery time
    
} ChannelSubscriber;

// Channel broadcaster for pub-sub patterns
typedef struct ChannelBroadcaster {
    uint64_t broadcaster_id;    // Unique broadcaster ID
    
    // Topic management
    struct TopicTree* topics;   // Topic tree for routing
    pthread_rwlock_t topic_lock; // Topic tree protection
    
    // Subscribers
    ChannelSubscriber** subscribers; // Subscriber list
    int subscriber_count;       // Number of subscribers
    int subscriber_capacity;    // Subscriber array capacity
    pthread_rwlock_t subscriber_lock; // Subscriber list protection
    
    // Message retention
    ChannelMessage** retained_messages; // Retained messages
    int retention_count;        // Number of retained messages
    uint64_t retention_time_ms; // Message retention time
    
    // Worker threads for message delivery
    pthread_t* delivery_threads; // Delivery threads
    int delivery_thread_count;  // Number of delivery threads
    bool delivery_active;       // Delivery threads active
    
    // Message queue for broadcasting
    ChannelMessage** broadcast_queue; // Broadcast queue
    size_t queue_capacity;      // Queue capacity
    atomic_size_t queue_head;   // Queue head
    atomic_size_t queue_tail;   // Queue tail
    pthread_mutex_t queue_mutex; // Queue protection
    pthread_cond_t queue_not_empty; // Queue not empty condition
    
} ChannelBroadcaster;

// =============================================================================
// Pipeline Processing
// =============================================================================

// Pipeline stage
typedef struct PipelineStage {
    uint64_t stage_id;          // Unique stage ID
    const char* stage_name;     // Stage name
    
    // Processing function
    ChannelMessage* (*processor)(ChannelMessage* input, void* context);
    void* processor_context;    // Processor context
    
    // Error handling
    ChannelMessage* (*error_handler)(ChannelMessage* input, StructuredError* error, void* context);
    void* error_context;        // Error handler context
    
    // Stage configuration
    int parallelism;            // Number of parallel workers
    uint64_t timeout_ms;        // Processing timeout
    uint32_t max_retries;       // Maximum retries
    
    // Buffering
    AdvancedChannel* input_channel;  // Input channel
    AdvancedChannel* output_channel; // Output channel
    
    // Worker management
    pthread_t* workers;         // Worker threads
    bool workers_active;        // Workers active flag
    
    // Statistics
    struct {
        uint64_t messages_processed; // Messages processed
        uint64_t processing_errors;  // Processing errors
        uint64_t retries;           // Total retries
        double avg_processing_time_ns; // Average processing time
    } stats;
    
} PipelineStage;

// Channel pipeline for stream processing
typedef struct ChannelPipeline {
    uint64_t pipeline_id;       // Unique pipeline ID
    const char* pipeline_name;  // Pipeline name
    
    // Pipeline stages
    PipelineStage** stages;     // Pipeline stages
    int stage_count;            // Number of stages
    
    // Pipeline configuration
    bool ordered_processing;    // Maintain message order
    bool fault_tolerant;        // Fault tolerant processing
    uint64_t checkpoint_interval_ms; // Checkpointing interval
    
    // Input/Output
    AdvancedChannel* input_channel;  // Pipeline input
    AdvancedChannel* output_channel; // Pipeline output
    AdvancedChannel* error_channel;  // Error output
    
    // Pipeline control
    atomic_bool is_running;     // Pipeline running flag
    pthread_t coordinator_thread; // Pipeline coordinator
    
    // Checkpointing and recovery
    struct CheckpointState {
        uint64_t checkpoint_id; // Current checkpoint ID
        uint64_t processed_count; // Messages processed
        void* state_data;       // Pipeline state data
        size_t state_size;      // State data size
    } checkpoint;
    
} ChannelPipeline;

// =============================================================================
// Core API Functions
// =============================================================================

// Channel creation and management
AdvancedChannel* advanced_channel_create(const char* name, AdvancedChannelType type);
AdvancedChannel* advanced_channel_create_with_config(const ChannelConfig* config);
void advanced_channel_destroy(AdvancedChannel* channel);

// Basic channel operations
bool advanced_channel_send(AdvancedChannel* channel, ChannelMessage* message);
bool advanced_channel_send_timeout(AdvancedChannel* channel, ChannelMessage* message, uint64_t timeout_ms);
ChannelMessage* advanced_channel_receive(AdvancedChannel* channel);
ChannelMessage* advanced_channel_receive_timeout(AdvancedChannel* channel, uint64_t timeout_ms);

// Channel state management
bool advanced_channel_close(AdvancedChannel* channel);
bool advanced_channel_is_closed(AdvancedChannel* channel);
size_t advanced_channel_size(AdvancedChannel* channel);
size_t advanced_channel_capacity(AdvancedChannel* channel);

// Message creation and management
ChannelMessage* channel_message_create(const void* data, size_t data_size, const char* type);
ChannelMessage* channel_message_create_with_priority(const void* data, size_t data_size, 
                                                    const char* type, MessagePriority priority);
void channel_message_destroy(ChannelMessage* message);
void channel_message_retain(ChannelMessage* message);
void channel_message_release(ChannelMessage* message);

// =============================================================================
// Distributed Channel API
// =============================================================================

// Distributed channel management
DistributedChannel* distributed_channel_create(const char* name, const DistributedChannelConfig* config);
void distributed_channel_destroy(DistributedChannel* channel);
bool distributed_channel_add_node(DistributedChannel* channel, const char* hostname, uint16_t port);
bool distributed_channel_remove_node(DistributedChannel* channel, uint64_t node_id);

// Cluster operations
bool distributed_channel_start_cluster(DistributedChannel* channel);
void distributed_channel_stop_cluster(DistributedChannel* channel);
bool distributed_channel_is_leader(DistributedChannel* channel);
ChannelNode* distributed_channel_get_leader(DistributedChannel* channel);

// Replication and consistency
bool distributed_channel_replicate_message(DistributedChannel* channel, ChannelMessage* message);
bool distributed_channel_wait_for_consensus(DistributedChannel* channel, uint64_t message_id, uint64_t timeout_ms);

// =============================================================================
// Load Balancer API
// =============================================================================

// Load balancer management
LoadBalancer* load_balancer_create(LoadBalanceStrategy strategy, ChannelNode* nodes, uint32_t node_count);
void load_balancer_destroy(LoadBalancer* balancer);
ChannelNode* load_balancer_select_node(LoadBalancer* balancer, ChannelMessage* message);
void load_balancer_update_node_stats(LoadBalancer* balancer, uint64_t node_id, 
                                     double response_time, bool success);

// Load balancer configuration
void load_balancer_set_weights(LoadBalancer* balancer, double* weights);
void load_balancer_set_hash_function(LoadBalancer* balancer, 
                                     uint32_t (*hash_func)(const void*, size_t));

// =============================================================================
// Channel Selector API
// =============================================================================

// Selector management
ChannelSelector* channel_selector_create(void);
void channel_selector_destroy(ChannelSelector* selector);
bool channel_selector_add_send_case(ChannelSelector* selector, AdvancedChannel* channel, ChannelMessage* message);
bool channel_selector_add_receive_case(ChannelSelector* selector, AdvancedChannel* channel, void** result);
void channel_selector_set_default(ChannelSelector* selector, void (*handler)(void*), void* context);
void channel_selector_set_timeout(ChannelSelector* selector, uint64_t timeout_ms);

// Selection operations
int channel_selector_select(ChannelSelector* selector);
bool channel_selector_select_timeout(ChannelSelector* selector, uint64_t timeout_ms, int* selected_case);

// =============================================================================
// Broadcasting and Pub-Sub API
// =============================================================================

// Broadcaster management
ChannelBroadcaster* channel_broadcaster_create(const char* name);
void channel_broadcaster_destroy(ChannelBroadcaster* broadcaster);

// Subscription management
uint64_t channel_broadcaster_subscribe(ChannelBroadcaster* broadcaster, const char* topic_filter, 
                                      AdvancedChannel* delivery_channel);
bool channel_broadcaster_unsubscribe(ChannelBroadcaster* broadcaster, uint64_t subscriber_id);

// Publishing
bool channel_broadcaster_publish(ChannelBroadcaster* broadcaster, const char* topic, ChannelMessage* message);
bool channel_broadcaster_retain_message(ChannelBroadcaster* broadcaster, const char* topic, 
                                       ChannelMessage* message, uint64_t retention_time_ms);

// =============================================================================
// Pipeline Processing API
// =============================================================================

// Pipeline management
ChannelPipeline* channel_pipeline_create(const char* name);
void channel_pipeline_destroy(ChannelPipeline* pipeline);
bool channel_pipeline_add_stage(ChannelPipeline* pipeline, const char* stage_name,
                                ChannelMessage* (*processor)(ChannelMessage*, void*), void* context);
bool channel_pipeline_start(ChannelPipeline* pipeline);
void channel_pipeline_stop(ChannelPipeline* pipeline);

// Pipeline operations
bool channel_pipeline_process_message(ChannelPipeline* pipeline, ChannelMessage* message);
bool channel_pipeline_checkpoint(ChannelPipeline* pipeline);
bool channel_pipeline_restore_from_checkpoint(ChannelPipeline* pipeline, uint64_t checkpoint_id);

// =============================================================================
// Integration APIs
// =============================================================================

// Actor system integration
bool advanced_channel_register_with_actor(AdvancedChannel* channel, ActorRef* actor);
bool advanced_channel_send_to_actor(AdvancedChannel* channel, ActorRef* actor, ChannelMessage* message);

// Structured concurrency integration
bool advanced_channel_register_with_scope(AdvancedChannel* channel, ConcurrencyScope* scope);
bool advanced_channel_cancel_on_scope_cancel(AdvancedChannel* channel, ConcurrencyScope* scope);

// Shared variables integration
bool advanced_channel_watch_shared_variable(AdvancedChannel* channel, SharedVar* var, 
                                           const char* message_type);

// =============================================================================
// Statistics and Monitoring
// =============================================================================

// Statistics retrieval
ChannelStats advanced_channel_get_stats(AdvancedChannel* channel);
void advanced_channel_reset_stats(AdvancedChannel* channel);
LoadBalancerStats load_balancer_get_stats(LoadBalancer* balancer);

// Performance monitoring
typedef struct ChannelPerformanceReport {
    uint64_t total_channels;
    uint64_t total_messages_sent;
    uint64_t total_messages_received;
    double avg_message_latency_ms;
    double channel_utilization;
    uint64_t memory_usage_bytes;
} ChannelPerformanceReport;

ChannelPerformanceReport get_channel_performance_report(void);
void print_channel_performance_report(const ChannelPerformanceReport* report);

// =============================================================================
// Debugging and Utilities
// =============================================================================

// Debug information
void advanced_channel_dump_info(AdvancedChannel* channel);
void distributed_channel_dump_cluster_info(DistributedChannel* channel);
void load_balancer_dump_stats(LoadBalancer* balancer);

// Utility functions
uint32_t channel_hash_string(const char* str);
uint32_t channel_hash_data(const void* data, size_t size);
bool channel_match_topic_filter(const char* topic, const char* filter);

// =============================================================================
// High-Level Channel Patterns
// =============================================================================

// Convenience macros for common patterns
#define CHANNEL_SELECT(selector) \
    for (ChannelSelector* _sel = (selector); _sel && channel_selector_select(_sel) >= 0; _sel = NULL)

#define CHANNEL_CASE_SEND(channel, message) \
    channel_selector_add_send_case(get_current_selector(), (channel), (message))

#define CHANNEL_CASE_RECEIVE(channel, result) \
    channel_selector_add_receive_case(get_current_selector(), (channel), (void**)(result))

#define CHANNEL_DEFAULT(handler, context) \
    channel_selector_set_default(get_current_selector(), (handler), (context))

// Pattern implementations
typedef struct ChannelPatterns {
    // Request-Reply pattern
    ChannelMessage* (*request_reply)(AdvancedChannel* request_channel, 
                                    AdvancedChannel* reply_channel,
                                    ChannelMessage* request, uint64_t timeout_ms);
    
    // Fan-out pattern
    bool (*fan_out)(AdvancedChannel* input, AdvancedChannel** outputs, int output_count);
    
    // Fan-in pattern
    bool (*fan_in)(AdvancedChannel** inputs, int input_count, AdvancedChannel* output);
    
    // Worker pool pattern
    bool (*worker_pool)(AdvancedChannel* work_queue, int worker_count, 
                       void (*worker_func)(ChannelMessage*, void*), void* context);
    
} ChannelPatterns;

// Get global channel patterns
ChannelPatterns* get_channel_patterns(void);

#endif // GOO_ADVANCED_CHANNELS_H