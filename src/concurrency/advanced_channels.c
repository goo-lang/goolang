#include "../../include/advanced_channels.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <regex.h>

// Utility functions
static uint64_t get_monotonic_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t generate_unique_id(void) {
    static atomic_uint_fast64_t counter = ATOMIC_VAR_INIT(1);
    return atomic_fetch_add(&counter, 1);
}

static uint64_t generate_correlation_id(void) {
    static atomic_uint_fast64_t correlation_counter = ATOMIC_VAR_INIT(1);
    return atomic_fetch_add(&correlation_counter, 1);
}

// Configuration helpers
ChannelConfig channel_config_default(void) {
    return (ChannelConfig) {
        .type = CHANNEL_BUFFERED,
        .ordering = CHANNEL_ORDER_FIFO,
        .reliability = CHANNEL_RELIABILITY_NONE,
        .flow_control = FLOW_CONTROL_NONE,
        .buffer_size = 100,
        .max_buffer_size = 10000,
        .message_size_limit = 1024 * 1024,  // 1MB
        .default_timeout_ms = 30000,        // 30 seconds
        .ack_timeout_ms = 5000,             // 5 seconds
        .message_ttl_ms = 300000,           // 5 minutes
        .window_size = 64,
        .max_rate_per_second = 1000,
        .burst_size = 100,
        .max_retries = 3,
        .retry_interval_ms = 1000,
        .enable_dead_letter_queue = false,
        .max_subscribers = 100,
        .persistent_subscription = false,
        .use_lock_free_queue = false,
        .enable_batching = false,
        .batch_size = 10,
        .batch_timeout_ms = 100,
        .enable_metrics = true,
        .enable_tracing = false,
        .actor_system = NULL,
        .task_scope = NULL
    };
}

ChannelConfig channel_config_unbuffered(void) {
    ChannelConfig config = channel_config_default();
    config.type = CHANNEL_UNBUFFERED;
    config.buffer_size = 0;
    return config;
}

ChannelConfig channel_config_buffered(size_t buffer_size) {
    ChannelConfig config = channel_config_default();
    config.type = CHANNEL_BUFFERED;
    config.buffer_size = buffer_size;
    return config;
}

ChannelConfig channel_config_broadcast(size_t max_subscribers) {
    ChannelConfig config = channel_config_default();
    config.type = CHANNEL_BROADCAST;
    config.max_subscribers = max_subscribers;
    config.persistent_subscription = true;
    return config;
}

ChannelConfig channel_config_reliable(void) {
    ChannelConfig config = channel_config_default();
    config.reliability = CHANNEL_RELIABILITY_EXACTLY_ONCE;
    config.flow_control = FLOW_CONTROL_SLIDING_WINDOW;
    config.enable_dead_letter_queue = true;
    config.max_retries = 5;
    return config;
}

ChannelConfig channel_config_high_throughput(void) {
    ChannelConfig config = channel_config_default();
    config.buffer_size = 10000;
    config.use_lock_free_queue = true;
    config.enable_batching = true;
    config.batch_size = 100;
    config.max_rate_per_second = 100000;
    return config;
}

ChannelConfig channel_config_low_latency(void) {
    ChannelConfig config = channel_config_default();
    config.type = CHANNEL_UNBUFFERED;
    config.buffer_size = 1;
    config.use_lock_free_queue = true;
    config.enable_batching = false;
    return config;
}

// Rate limiting helper
static bool rate_limit_check(Channel* channel) {
    if (channel->config.flow_control != FLOW_CONTROL_RATE_LIMIT) {
        return true;  // No rate limiting
    }
    
    uint64_t current_time = get_monotonic_time_ns();
    uint64_t window_duration = 1000000000ULL;  // 1 second in nanoseconds
    
    // Reset window if needed
    if (current_time - channel->rate_limit_window_start >= window_duration) {
        channel->rate_limit_window_start = current_time;
        channel->rate_limit_count = 0;
        channel->rate_limit_tokens = channel->config.burst_size;
    }
    
    // Check rate limit
    if (channel->rate_limit_count >= channel->config.max_rate_per_second) {
        channel->stats.rate_limit_events++;
        return false;
    }
    
    // Check burst limit
    if (channel->rate_limit_tokens == 0) {
        channel->stats.rate_limit_events++;
        return false;
    }
    
    channel->rate_limit_count++;
    channel->rate_limit_tokens--;
    return true;
}

// Message operations
ChannelMessage* channel_message_create(void* data, size_t data_size) {
    ChannelMessage* message = calloc(1, sizeof(ChannelMessage));
    if (!message) return NULL;
    
    message->message_id = generate_unique_id();
    message->data = data;
    message->data_size = data_size;
    message->owns_data = false;  // Caller manages data by default
    
    // Initialize metadata
    message->metadata.sequence_number = 0;
    message->metadata.timestamp_ns = get_monotonic_time_ns();
    message->metadata.sender_id = 0;
    message->metadata.correlation_id = 0;
    message->metadata.priority = 0;
    message->metadata.ttl_ms = 300000;  // 5 minutes default
    message->metadata.requires_ack = false;
    message->metadata.retry_count = 0;
    
    // Initialize acknowledgment synchronization
    if (pthread_mutex_init(&message->ack_mutex, NULL) != 0) {
        free(message);
        return NULL;
    }
    
    if (pthread_cond_init(&message->ack_received_cond, NULL) != 0) {
        pthread_mutex_destroy(&message->ack_mutex);
        free(message);
        return NULL;
    }
    
    message->ack_received = false;
    message->ack_timeout_ms = 5000;  // 5 seconds default
    
    return message;
}

void channel_message_destroy(ChannelMessage* message) {
    if (!message) return;
    
    if (message->owns_data && message->data) {
        if (message->data_destructor) {
            message->data_destructor(message->data);
        } else {
            free(message->data);
        }
    }
    
    // Clean up custom headers
    for (size_t i = 0; i < message->metadata.header_count; i++) {
        free(message->metadata.headers[i].key);
        free(message->metadata.headers[i].value);
    }
    
    free(message->metadata.routing_key);
    
    pthread_mutex_destroy(&message->ack_mutex);
    pthread_cond_destroy(&message->ack_received_cond);
    
    free(message);
}

Result_void_ptr channel_message_set_metadata(ChannelMessage* message, MessageMetadata metadata) {
    if (!message) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid message"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Copy metadata, preserving some system-managed fields
    uint64_t original_timestamp = message->metadata.timestamp_ns;
    uint64_t original_sequence = message->metadata.sequence_number;
    
    message->metadata = metadata;
    
    // Restore system fields if they weren't set
    if (metadata.timestamp_ns == 0) {
        message->metadata.timestamp_ns = original_timestamp;
    }
    if (metadata.sequence_number == 0) {
        message->metadata.sequence_number = original_sequence;
    }
    
    // Copy routing key if provided
    if (metadata.routing_key) {
        message->metadata.routing_key = strdup(metadata.routing_key);
    }
    
    return OK_PTR(NULL);
}

// Channel creation and management
Channel* channel_create(ChannelConfig config, const char* name) {
    Channel* channel = calloc(1, sizeof(Channel));
    if (!channel) return NULL;
    
    channel->id = generate_unique_id();
    if (name) {
        strncpy(channel->name, name, sizeof(channel->name) - 1);
        channel->name[sizeof(channel->name) - 1] = '\0';
    } else {
        snprintf(channel->name, sizeof(channel->name), "channel_%lu", channel->id);
    }
    
    channel->config = config;
    channel->is_closed = false;
    atomic_init(&channel->shutdown_requested, false);
    atomic_init(&channel->sequence_number, 1);
    atomic_init(&channel->window_current, 0);
    atomic_init(&channel->ref_count, 1);
    
    // Initialize synchronization
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
        pthread_mutex_destroy(&channel->channel_mutex);
        pthread_cond_destroy(&channel->message_available);
        free(channel);
        return NULL;
    }
    
    if (pthread_rwlock_init(&channel->subscriber_lock, NULL) != 0) {
        pthread_mutex_destroy(&channel->channel_mutex);
        pthread_cond_destroy(&channel->message_available);
        pthread_cond_destroy(&channel->space_available);
        free(channel);
        return NULL;
    }
    
    // Initialize priority queues if needed
    if (config.type == CHANNEL_PRIORITY) {
        channel->max_priority = 10;  // Support priorities 0-9
        channel->priority_queues = calloc(channel->max_priority, sizeof(ChannelMessage*));
        channel->priority_sizes = calloc(channel->max_priority, sizeof(size_t));
        
        if (!channel->priority_queues || !channel->priority_sizes) {
            free(channel->priority_queues);
            free(channel->priority_sizes);
            pthread_rwlock_destroy(&channel->subscriber_lock);
            pthread_mutex_destroy(&channel->channel_mutex);
            pthread_cond_destroy(&channel->message_available);
            pthread_cond_destroy(&channel->space_available);
            free(channel);
            return NULL;
        }
    }
    
    // Initialize subscribers array for broadcast channels
    if (config.type == CHANNEL_BROADCAST || config.type == CHANNEL_MULTICAST) {
        channel->subscriber_capacity = config.max_subscribers;
        channel->subscribers = calloc(channel->subscriber_capacity, sizeof(ChannelEndpoint*));
        
        if (!channel->subscribers) {
            free(channel->priority_queues);
            free(channel->priority_sizes);
            pthread_rwlock_destroy(&channel->subscriber_lock);
            pthread_mutex_destroy(&channel->channel_mutex);
            pthread_cond_destroy(&channel->message_available);
            pthread_cond_destroy(&channel->space_available);
            free(channel);
            return NULL;
        }
    }
    
    // Initialize pending acks array for reliable channels
    if (config.reliability != CHANNEL_RELIABILITY_NONE) {
        channel->pending_ack_capacity = config.buffer_size * 2;
        channel->pending_acks = calloc(channel->pending_ack_capacity, sizeof(ChannelMessage*));
        
        if (!channel->pending_acks) {
            free(channel->subscribers);
            free(channel->priority_queues);
            free(channel->priority_sizes);
            pthread_rwlock_destroy(&channel->subscriber_lock);
            pthread_mutex_destroy(&channel->channel_mutex);
            pthread_cond_destroy(&channel->message_available);
            pthread_cond_destroy(&channel->space_available);
            free(channel);
            return NULL;
        }
    }
    
    // Create dead letter queue if enabled
    if (config.enable_dead_letter_queue) {
        ChannelConfig dlq_config = channel_config_buffered(1000);
        dlq_config.enable_dead_letter_queue = false;  // Prevent recursion
        
        char dlq_name[128];
        snprintf(dlq_name, sizeof(dlq_name), "%s_dlq", channel->name);
        
        channel->dead_letter_queue = channel_create(dlq_config, dlq_name);
        // Note: Not checking for failure here to keep channel creation simple
    }
    
    // Initialize rate limiting
    channel->rate_limit_window_start = get_monotonic_time_ns();
    channel->rate_limit_tokens = config.burst_size;
    
    return channel;
}

void channel_destroy(Channel* channel) {
    if (!channel) return;
    
    // Close channel first
    channel_close(channel);
    
    // Clean up message queue
    ChannelMessage* msg = channel->message_queue;
    while (msg) {
        ChannelMessage* next = msg->next;
        channel_message_destroy(msg);
        msg = next;
    }
    
    // Clean up priority queues
    if (channel->priority_queues) {
        for (int i = 0; i < channel->max_priority; i++) {
            ChannelMessage* prio_msg = channel->priority_queues[i];
            while (prio_msg) {
                ChannelMessage* next = prio_msg->next;
                channel_message_destroy(prio_msg);
                prio_msg = next;
            }
        }
        free(channel->priority_queues);
        free(channel->priority_sizes);
    }
    
    // Clean up pending acks
    if (channel->pending_acks) {
        for (size_t i = 0; i < channel->pending_ack_count; i++) {
            if (channel->pending_acks[i]) {
                channel_message_destroy(channel->pending_acks[i]);
            }
        }
        free(channel->pending_acks);
    }
    
    // Clean up subscribers array
    free(channel->subscribers);
    
    // Clean up dead letter queue
    if (channel->dead_letter_queue) {
        channel_destroy(channel->dead_letter_queue);
    }
    
    // Clean up endpoints
    ChannelEndpoint* endpoint = channel->senders;
    while (endpoint) {
        ChannelEndpoint* next = endpoint->next;
        channel_endpoint_destroy(endpoint);
        endpoint = next;
    }
    
    endpoint = channel->receivers;
    while (endpoint) {
        ChannelEndpoint* next = endpoint->next;
        channel_endpoint_destroy(endpoint);
        endpoint = next;
    }
    
    // Destroy synchronization primitives
    pthread_rwlock_destroy(&channel->subscriber_lock);
    pthread_mutex_destroy(&channel->channel_mutex);
    pthread_cond_destroy(&channel->message_available);
    pthread_cond_destroy(&channel->space_available);
    
    free(channel);
}

// Endpoint management
ChannelEndpoint* channel_get_sender(Channel* channel) {
    if (!channel) return NULL;
    
    ChannelEndpoint* endpoint = calloc(1, sizeof(ChannelEndpoint));
    if (!endpoint) return NULL;
    
    endpoint->channel = channel;
    endpoint->is_sender = true;
    endpoint->is_receiver = false;
    endpoint->endpoint_id = generate_unique_id();
    endpoint->auto_ack = false;
    endpoint->last_activity_time = get_monotonic_time_ns();
    
    // Add to channel's sender list
    pthread_mutex_lock(&channel->channel_mutex);
    endpoint->next = channel->senders;
    channel->senders = endpoint;
    channel->sender_count++;
    pthread_mutex_unlock(&channel->channel_mutex);
    
    // Increment channel reference count
    atomic_fetch_add(&channel->ref_count, 1);
    
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
    endpoint->auto_ack = true;  // Auto-ack by default for receivers
    endpoint->last_activity_time = get_monotonic_time_ns();
    
    // Add to channel's receiver list
    pthread_mutex_lock(&channel->channel_mutex);
    endpoint->next = channel->receivers;
    channel->receivers = endpoint;
    channel->receiver_count++;
    pthread_mutex_unlock(&channel->channel_mutex);
    
    // Increment channel reference count
    atomic_fetch_add(&channel->ref_count, 1);
    
    return endpoint;
}

void channel_endpoint_destroy(ChannelEndpoint* endpoint) {
    if (!endpoint) return;
    
    Channel* channel = endpoint->channel;
    if (channel) {
        pthread_mutex_lock(&channel->channel_mutex);
        
        // Remove from sender list
        if (endpoint->is_sender) {
            ChannelEndpoint** current = &channel->senders;
            while (*current) {
                if (*current == endpoint) {
                    *current = endpoint->next;
                    channel->sender_count--;
                    break;
                }
                current = &(*current)->next;
            }
        }
        
        // Remove from receiver list
        if (endpoint->is_receiver) {
            ChannelEndpoint** current = &channel->receivers;
            while (*current) {
                if (*current == endpoint) {
                    *current = endpoint->next;
                    channel->receiver_count--;
                    break;
                }
                current = &(*current)->next;
            }
        }
        
        pthread_mutex_unlock(&channel->channel_mutex);
        
        // Decrement channel reference count
        if (atomic_fetch_sub(&channel->ref_count, 1) == 1) {
            // Last reference, destroy channel
            channel_destroy(channel);
        }
    }
    
    free(endpoint);
}

// Core send/receive operations
Result_void_ptr channel_send(ChannelEndpoint* sender, void* data, size_t data_size) {
    MessageMetadata default_metadata = {0};
    return channel_send_with_metadata(sender, data, data_size, default_metadata);
}

Result_void_ptr channel_send_with_metadata(ChannelEndpoint* sender, void* data, size_t data_size, MessageMetadata metadata) {
    if (!sender || !sender->is_sender || !data) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid sender endpoint or data"),
            .hint = strdup("Ensure endpoint is a valid sender and data is not NULL"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    Channel* channel = sender->channel;
    if (!channel || channel->is_closed || atomic_load(&channel->shutdown_requested)) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_CHANNEL_CLOSED,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Channel is closed"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Check message size limit
    if (data_size > channel->config.message_size_limit) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Message exceeds size limit"),
            .hint = strdup("Reduce message size or increase channel limit"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    uint64_t start_time = get_monotonic_time_ns();
    
    pthread_mutex_lock(&channel->channel_mutex);
    
    // Check rate limiting
    if (!rate_limit_check(channel)) {
        pthread_mutex_unlock(&channel->channel_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_CHANNEL_TIMEOUT,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Rate limit exceeded"),
            .hint = strdup("Reduce send rate or increase rate limit"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Check flow control (sliding window)
    if (channel->config.flow_control == FLOW_CONTROL_SLIDING_WINDOW) {
        while (atomic_load(&channel->window_current) >= channel->config.window_size) {
            pthread_cond_wait(&channel->space_available, &channel->channel_mutex);
            
            if (channel->is_closed || atomic_load(&channel->shutdown_requested)) {
                pthread_mutex_unlock(&channel->channel_mutex);
                
                Error* error = malloc(sizeof(Error));
                *error = (Error){
                    .code = ERROR_CHANNEL_CLOSED,
                    .severity = ERROR_SEVERITY_ERROR,
                    .category = ERROR_CATEGORY_INTERNAL,
                    .message = strdup("Channel closed while waiting for window space"),
                    .hint = NULL,
                    .location = (SourceLocation){0},
                    .next = NULL
                };
                return ERR_PTR(error);
            }
        }
    }
    
    // For buffered channels, check if there's space
    if (channel->config.type == CHANNEL_BUFFERED && 
        channel->current_size >= channel->config.buffer_size) {
        
        pthread_mutex_unlock(&channel->channel_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_CHANNEL_FULL,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Channel buffer is full"),
            .hint = strdup("Wait for messages to be consumed or increase buffer size"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Create message
    ChannelMessage* message = channel_message_create(data, data_size);
    if (!message) {
        pthread_mutex_unlock(&channel->channel_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to create message"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Copy data to message
    message->data = malloc(data_size);
    if (!message->data) {
        channel_message_destroy(message);
        pthread_mutex_unlock(&channel->channel_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to allocate message data"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    memcpy(message->data, data, data_size);
    message->owns_data = true;
    
    // Set metadata
    channel_message_set_metadata(message, metadata);
    message->metadata.sender_id = sender->endpoint_id;
    message->metadata.sequence_number = atomic_fetch_add(&channel->sequence_number, 1);
    
    // Handle different channel types
    switch (channel->config.type) {
        case CHANNEL_UNBUFFERED:
            // Synchronous send - wait for receiver
            while (channel->receiver_count == 0 && !channel->is_closed) {
                pthread_cond_wait(&channel->message_available, &channel->channel_mutex);
            }
            
            if (channel->is_closed) {
                channel_message_destroy(message);
                pthread_mutex_unlock(&channel->channel_mutex);
                
                Error* error = malloc(sizeof(Error));
                *error = (Error){
                    .code = ERROR_CHANNEL_CLOSED,
                    .severity = ERROR_SEVERITY_ERROR,
                    .category = ERROR_CATEGORY_INTERNAL,
                    .message = strdup("Channel closed while waiting for receiver"),
                    .hint = NULL,
                    .location = (SourceLocation){0},
                    .next = NULL
                };
                return ERR_PTR(error);
            }
            
            // Add to queue and signal
            channel->message_queue = message;
            channel->current_size = 1;
            pthread_cond_signal(&channel->message_available);
            break;
            
        case CHANNEL_BUFFERED:
        case CHANNEL_UNBOUNDED:
            // Add to FIFO queue
            if (!channel->message_queue) {
                channel->message_queue = message;
                channel->message_queue_tail = message;
            } else {
                channel->message_queue_tail->next = message;
                channel->message_queue_tail = message;
            }
            channel->current_size++;
            pthread_cond_signal(&channel->message_available);
            break;
            
        case CHANNEL_PRIORITY:
            // Add to priority queue
            if (message->metadata.priority < 0) message->metadata.priority = 0;
            if (message->metadata.priority >= channel->max_priority) {
                message->metadata.priority = channel->max_priority - 1;
            }
            
            int priority = message->metadata.priority;
            if (!channel->priority_queues[priority]) {
                channel->priority_queues[priority] = message;
            } else {
                // Find tail of priority queue
                ChannelMessage* tail = channel->priority_queues[priority];
                while (tail->next) tail = tail->next;
                tail->next = message;
            }
            channel->priority_sizes[priority]++;
            channel->current_size++;
            pthread_cond_signal(&channel->message_available);
            break;
            
        case CHANNEL_BROADCAST:
            // Send to all subscribers
            pthread_rwlock_rdlock(&channel->subscriber_lock);
            
            for (size_t i = 0; i < channel->subscriber_count; i++) {
                ChannelEndpoint* subscriber = channel->subscribers[i];
                if (subscriber && subscriber->is_receiver) {
                    // Create a copy of the message for each subscriber
                    ChannelMessage* copy = channel_message_create(NULL, 0);
                    if (copy) {
                        copy->data = malloc(data_size);
                        if (copy->data) {
                            memcpy(copy->data, data, data_size);
                            copy->data_size = data_size;
                            copy->owns_data = true;
                            copy->metadata = message->metadata;
                            
                            // Add to subscriber's private queue (simplified)
                            // In a full implementation, each subscriber would have its own queue
                        }
                    }
                }
            }
            
            pthread_rwlock_unlock(&channel->subscriber_lock);
            channel_message_destroy(message);  // Original message not needed
            break;
            
        default:
            channel_message_destroy(message);
            pthread_mutex_unlock(&channel->channel_mutex);
            
            Error* error = malloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_CHANNEL_INVALID_TYPE,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Unsupported channel type"),
                .hint = NULL,
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
    }
    
    // Update statistics
    channel->stats.messages_sent++;
    sender->messages_handled++;
    sender->last_activity_time = get_monotonic_time_ns();
    
    uint64_t end_time = get_monotonic_time_ns();
    uint64_t send_time = end_time - start_time;
    channel->stats.avg_send_time_ns = (channel->stats.avg_send_time_ns + send_time) / 2;
    
    // Update flow control window
    if (channel->config.flow_control == FLOW_CONTROL_SLIDING_WINDOW) {
        atomic_fetch_add(&channel->window_current, 1);
    }
    
    pthread_mutex_unlock(&channel->channel_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr channel_receive(ChannelEndpoint* receiver, void** data, size_t* data_size) {
    return channel_receive_timeout(receiver, data, data_size, receiver->channel->config.default_timeout_ms);
}

Result_void_ptr channel_receive_timeout(ChannelEndpoint* receiver, void** data, size_t* data_size, uint64_t timeout_ms) {
    if (!receiver || !receiver->is_receiver || !data || !data_size) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid receiver endpoint or parameters"),
            .hint = strdup("Ensure endpoint is a valid receiver and parameters are not NULL"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    Channel* channel = receiver->channel;
    if (!channel) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid channel"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    uint64_t start_time = get_monotonic_time_ns();
    
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
    while (channel->current_size == 0 && !channel->is_closed && 
           !atomic_load(&channel->shutdown_requested) && wait_result != ETIMEDOUT) {
        
        if (timeout_ms == UINT64_MAX) {
            wait_result = pthread_cond_wait(&channel->message_available, &channel->channel_mutex);
        } else {
            wait_result = pthread_cond_timedwait(&channel->message_available, &channel->channel_mutex, &deadline);
        }
    }
    
    if (wait_result == ETIMEDOUT) {
        pthread_mutex_unlock(&channel->channel_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_CHANNEL_TIMEOUT,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Receive operation timed out"),
            .hint = strdup("Increase timeout or check if sender is active"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    if (channel->is_closed || atomic_load(&channel->shutdown_requested)) {
        pthread_mutex_unlock(&channel->channel_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_CHANNEL_CLOSED,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Channel is closed"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    if (channel->current_size == 0) {
        pthread_mutex_unlock(&channel->channel_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_CHANNEL_EMPTY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Channel is empty"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Get message based on channel type
    ChannelMessage* message = NULL;
    
    switch (channel->config.type) {
        case CHANNEL_UNBUFFERED:
        case CHANNEL_BUFFERED:
        case CHANNEL_UNBOUNDED:
            // FIFO order
            message = channel->message_queue;
            if (message) {
                channel->message_queue = message->next;
                if (!channel->message_queue) {
                    channel->message_queue_tail = NULL;
                }
                message->next = NULL;
                channel->current_size--;
            }
            break;
            
        case CHANNEL_PRIORITY:
            // Find highest priority non-empty queue
            for (int priority = channel->max_priority - 1; priority >= 0; priority--) {
                if (channel->priority_sizes[priority] > 0) {
                    message = channel->priority_queues[priority];
                    if (message) {
                        channel->priority_queues[priority] = message->next;
                        message->next = NULL;
                        channel->priority_sizes[priority]--;
                        channel->current_size--;
                    }
                    break;
                }
            }
            break;
            
        default:
            pthread_mutex_unlock(&channel->channel_mutex);
            
            Error* error = malloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_CHANNEL_INVALID_TYPE,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Unsupported channel type for receive"),
                .hint = NULL,
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
    }
    
    if (!message) {
        pthread_mutex_unlock(&channel->channel_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INTERNAL,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to retrieve message"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Check message TTL
    uint64_t current_time = get_monotonic_time_ns();
    uint64_t message_age = (current_time - message->metadata.timestamp_ns) / 1000000;  // Convert to ms
    
    if (message->metadata.ttl_ms > 0 && message_age > message->metadata.ttl_ms) {
        // Message expired
        channel->stats.messages_expired++;
        
        // Send to dead letter queue if enabled
        if (channel->dead_letter_queue) {
            ChannelEndpoint* dlq_sender = channel_get_sender(channel->dead_letter_queue);
            if (dlq_sender) {
                channel_send(dlq_sender, message->data, message->data_size);
                channel_endpoint_destroy(dlq_sender);
            }
        }
        
        channel_message_destroy(message);
        pthread_mutex_unlock(&channel->channel_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_CHANNEL_TIMEOUT,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Message expired (TTL exceeded)"),
            .hint = strdup("Increase message TTL or process messages faster"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Return message data
    *data = message->data;
    *data_size = message->data_size;
    
    // Transfer ownership of data to caller
    message->data = NULL;
    message->owns_data = false;
    
    // Update statistics
    channel->stats.messages_received++;
    receiver->messages_handled++;
    receiver->last_activity_time = current_time;
    
    uint64_t end_time = get_monotonic_time_ns();
    uint64_t receive_time = end_time - start_time;
    channel->stats.avg_receive_time_ns = (channel->stats.avg_receive_time_ns + receive_time) / 2;
    
    // Handle acknowledgments for reliable channels
    if (channel->config.reliability != CHANNEL_RELIABILITY_NONE && message->metadata.requires_ack) {
        if (receiver->auto_ack) {
            channel_ack_message(receiver, message);
        } else {
            // Add to pending acks for manual acknowledgment
            if (channel->pending_ack_count < channel->pending_ack_capacity) {
                channel->pending_acks[channel->pending_ack_count++] = message;
                message = NULL;  // Don't destroy message yet
            }
        }
    }
    
    // Signal space available for flow control
    if (channel->config.flow_control == FLOW_CONTROL_SLIDING_WINDOW) {
        atomic_fetch_sub(&channel->window_current, 1);
        pthread_cond_signal(&channel->space_available);
    }
    
    // Signal space available for buffered channels
    if (channel->config.type == CHANNEL_BUFFERED) {
        pthread_cond_signal(&channel->space_available);
    }
    
    pthread_mutex_unlock(&channel->channel_mutex);
    
    if (message) {
        channel_message_destroy(message);
    }
    
    return OK_PTR(NULL);
}

// Non-blocking operations
Result_bool channel_try_send(ChannelEndpoint* sender, void* data, size_t data_size) {
    if (!sender || !sender->is_sender || !data) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid sender endpoint or data"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return (Result_bool){.is_error = true, .error = error};
    }
    
    Channel* channel = sender->channel;
    if (!channel || channel->is_closed || atomic_load(&channel->shutdown_requested)) {
        return (Result_bool){.is_error = false, .value = false};
    }
    
    pthread_mutex_lock(&channel->channel_mutex);
    
    // Check if send would block
    bool can_send = false;
    
    switch (channel->config.type) {
        case CHANNEL_UNBUFFERED:
            can_send = (channel->receiver_count > 0);
            break;
            
        case CHANNEL_BUFFERED:
            can_send = (channel->current_size < channel->config.buffer_size);
            break;
            
        case CHANNEL_UNBOUNDED:
        case CHANNEL_PRIORITY:
        case CHANNEL_BROADCAST:
            can_send = true;  // These types generally don't block
            break;
    }
    
    // Check rate limiting
    if (can_send && !rate_limit_check(channel)) {
        can_send = false;
    }
    
    // Check flow control
    if (can_send && channel->config.flow_control == FLOW_CONTROL_SLIDING_WINDOW) {
        can_send = (atomic_load(&channel->window_current) < channel->config.window_size);
    }
    
    pthread_mutex_unlock(&channel->channel_mutex);
    
    if (can_send) {
        Result_void_ptr send_result = channel_send(sender, data, data_size);
        return (Result_bool){.is_error = send_result.is_error, .error = send_result.error, .value = !send_result.is_error};
    } else {
        return (Result_bool){.is_error = false, .value = false};
    }
}

Result_bool channel_try_receive(ChannelEndpoint* receiver, void** data, size_t* data_size) {
    if (!receiver || !receiver->is_receiver || !data || !data_size) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid receiver endpoint or parameters"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return (Result_bool){.is_error = true, .error = error};
    }
    
    Channel* channel = receiver->channel;
    if (!channel) {
        return (Result_bool){.is_error = false, .value = false};
    }
    
    pthread_mutex_lock(&channel->channel_mutex);
    
    bool can_receive = (channel->current_size > 0) && 
                      !channel->is_closed && 
                      !atomic_load(&channel->shutdown_requested);
    
    pthread_mutex_unlock(&channel->channel_mutex);
    
    if (can_receive) {
        Result_void_ptr receive_result = channel_receive_timeout(receiver, data, data_size, 0);  // No timeout
        return (Result_bool){.is_error = receive_result.is_error, .error = receive_result.error, .value = !receive_result.is_error};
    } else {
        return (Result_bool){.is_error = false, .value = false};
    }
}

// Channel management
Result_void_ptr channel_close(Channel* channel) {
    if (!channel) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid channel"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&channel->channel_mutex);
    
    if (channel->is_closed) {
        pthread_mutex_unlock(&channel->channel_mutex);
        return OK_PTR(NULL);  // Already closed
    }
    
    channel->is_closed = true;
    atomic_store(&channel->shutdown_requested, true);
    
    // Wake up all waiting threads
    pthread_cond_broadcast(&channel->message_available);
    pthread_cond_broadcast(&channel->space_available);
    
    pthread_mutex_unlock(&channel->channel_mutex);
    
    return OK_PTR(NULL);
}

bool channel_is_closed(Channel* channel) {
    if (!channel) return true;
    return channel->is_closed || atomic_load(&channel->shutdown_requested);
}

// Statistics
ChannelStats channel_get_stats(Channel* channel) {
    if (!channel) {
        return (ChannelStats){0};
    }
    
    pthread_mutex_lock(&channel->channel_mutex);
    ChannelStats stats = channel->stats;
    stats.current_queue_size = channel->current_size;
    // TODO: Track peak queue size
    stats.peak_queue_size = channel->config.buffer_size;
    
    if (channel->config.buffer_size > 0) {
        stats.queue_utilization = (double)channel->current_size / channel->config.buffer_size;
    }
    
    stats.active_subscribers = channel->subscriber_count;
    stats.total_subscribers = channel->subscriber_count;  // TODO: Track total over time
    
    pthread_mutex_unlock(&channel->channel_mutex);
    
    return stats;
}

void channel_reset_stats(Channel* channel) {
    if (!channel) return;
    
    pthread_mutex_lock(&channel->channel_mutex);
    memset(&channel->stats, 0, sizeof(ChannelStats));
    pthread_mutex_unlock(&channel->channel_mutex);
}

// ============================================================================
// DISTRIBUTED CHANNEL SUPPORT
// ============================================================================

// Distributed channel node structure
typedef struct DistributedNode {
    uint64_t node_id;
    char address[256];
    uint16_t port;
    bool is_active;
    uint64_t last_heartbeat;
    
    // Connection state
    int socket_fd;
    pthread_t communication_thread;
    bool thread_active;
    
    // Statistics
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t connection_failures;
    
    struct DistributedNode* next;
} DistributedNode;

// Distributed channel configuration
typedef struct DistributedChannelConfig {
    size_t replica_count;
    enum {
        CONSISTENCY_EVENTUAL,
        CONSISTENCY_STRONG,
        CONSISTENCY_WEAK
    } consistency_model;
    
    uint64_t heartbeat_interval_ms;
    uint64_t failure_timeout_ms;
    bool auto_failover;
    bool partition_tolerance;
    
    // Network configuration
    char bind_address[256];
    uint16_t base_port;
    size_t max_nodes;
    
    // Consensus configuration
    enum {
        CONSENSUS_NONE,
        CONSENSUS_RAFT,
        CONSENSUS_PAXOS
    } consensus_algorithm;
} DistributedChannelConfig;

// Distributed channel structure
typedef struct DistributedChannel {
    Channel* local_channel;
    DistributedChannelConfig config;
    
    // Node management
    DistributedNode* nodes;
    size_t node_count;
    uint64_t local_node_id;
    
    // Replication state
    uint64_t global_sequence_number;
    uint64_t last_committed_sequence;
    
    // Consensus state
    bool is_leader;
    uint64_t current_term;
    uint64_t voted_for;
    
    // Message replication queue
    ChannelMessage** replication_queue;
    size_t replication_queue_size;
    size_t replication_queue_capacity;
    
    pthread_mutex_t distributed_mutex;
    pthread_t heartbeat_thread;
    pthread_t replication_thread;
    bool threads_active;
} DistributedChannel;

// Distributed channel operations
static DistributedChannel* distributed_channel_create(Channel* local_channel, DistributedChannelConfig config) {
    DistributedChannel* dist_channel = calloc(1, sizeof(DistributedChannel));
    if (!dist_channel) return NULL;
    
    dist_channel->local_channel = local_channel;
    dist_channel->config = config;
    dist_channel->local_node_id = generate_unique_id();
    
    // Initialize replication queue
    dist_channel->replication_queue_capacity = 1000;
    dist_channel->replication_queue = calloc(dist_channel->replication_queue_capacity, sizeof(ChannelMessage*));
    if (!dist_channel->replication_queue) {
        free(dist_channel);
        return NULL;
    }
    
    if (pthread_mutex_init(&dist_channel->distributed_mutex, NULL) != 0) {
        free(dist_channel->replication_queue);
        free(dist_channel);
        return NULL;
    }
    
    return dist_channel;
}

static void distributed_channel_destroy(DistributedChannel* dist_channel) {
    if (!dist_channel) return;
    
    // Stop threads
    dist_channel->threads_active = false;
    if (dist_channel->heartbeat_thread) {
        pthread_join(dist_channel->heartbeat_thread, NULL);
    }
    if (dist_channel->replication_thread) {
        pthread_join(dist_channel->replication_thread, NULL);
    }
    
    // Cleanup nodes
    DistributedNode* node = dist_channel->nodes;
    while (node) {
        DistributedNode* next = node->next;
        if (node->socket_fd >= 0) {
            close(node->socket_fd);
        }
        if (node->thread_active) {
            pthread_join(node->communication_thread, NULL);
        }
        free(node);
        node = next;
    }
    
    // Cleanup replication queue
    for (size_t i = 0; i < dist_channel->replication_queue_size; i++) {
        channel_message_destroy(dist_channel->replication_queue[i]);
    }
    free(dist_channel->replication_queue);
    
    pthread_mutex_destroy(&dist_channel->distributed_mutex);
    free(dist_channel);
}

// Heartbeat mechanism
static void* distributed_heartbeat_worker(void* arg) {
    DistributedChannel* dist_channel = (DistributedChannel*)arg;
    
    while (dist_channel->threads_active) {
        pthread_mutex_lock(&dist_channel->distributed_mutex);
        
        uint64_t current_time = get_monotonic_time_ns();
        DistributedNode* node = dist_channel->nodes;
        
        while (node) {
            if (node->is_active) {
                // Send heartbeat
                // TODO: Implement actual network heartbeat
                node->last_heartbeat = current_time;
            }
            
            // Check for failures
            if (current_time - node->last_heartbeat > dist_channel->config.failure_timeout_ms * 1000000) {
                node->is_active = false;
                if (dist_channel->config.auto_failover) {
                    // TODO: Implement failover logic
                }
            }
            
            node = node->next;
        }
        
        pthread_mutex_unlock(&dist_channel->distributed_mutex);
        
        usleep(dist_channel->config.heartbeat_interval_ms * 1000);
    }
    
    return NULL;
}

// Message replication
static Result_void_ptr distributed_replicate_message(DistributedChannel* dist_channel, ChannelMessage* message) {
    if (!dist_channel || !message) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid distributed channel or message"));
    }
    
    pthread_mutex_lock(&dist_channel->distributed_mutex);
    
    // Add to replication queue
    if (dist_channel->replication_queue_size >= dist_channel->replication_queue_capacity) {
        // Queue full, resize or drop
        pthread_mutex_unlock(&dist_channel->distributed_mutex);
        return ERR_PTR(error_create(ERROR_CHANNEL_FULL, "Replication queue full"));
    }
    
    // Create a copy of the message for replication
    ChannelMessage* replica_message = channel_message_create(message->data, message->data_size);
    if (!replica_message) {
        pthread_mutex_unlock(&dist_channel->distributed_mutex);
        return ERR_PTR(error_create(ERROR_MEMORY_ALLOCATION, "Failed to create replica message"));
    }
    
    replica_message->metadata = message->metadata;
    replica_message->metadata.sequence_number = atomic_fetch_add(&dist_channel->global_sequence_number, 1);
    
    dist_channel->replication_queue[dist_channel->replication_queue_size++] = replica_message;
    
    pthread_mutex_unlock(&dist_channel->distributed_mutex);
    
    return OK_PTR(NULL);
}

// ============================================================================
// LOAD BALANCING ALGORITHMS
// ============================================================================

// Load balancing strategies
typedef enum {
    LOAD_BALANCE_ROUND_ROBIN,
    LOAD_BALANCE_LEAST_CONNECTIONS,
    LOAD_BALANCE_WEIGHTED_ROUND_ROBIN,
    LOAD_BALANCE_RANDOM,
    LOAD_BALANCE_HASH_BASED,
    LOAD_BALANCE_LEAST_RESPONSE_TIME
} LoadBalanceStrategy;

// Load balanced channel configuration
typedef struct LoadBalancedChannelConfig {
    LoadBalanceStrategy strategy;
    size_t worker_count;
    uint32_t* worker_weights;  // For weighted strategies
    size_t hash_range;         // For hash-based distribution
    bool health_checking;
    uint64_t health_check_interval_ms;
} LoadBalancedChannelConfig;

// Worker node for load balancing
typedef struct WorkerNode {
    uint64_t worker_id;
    Channel* worker_channel;
    ChannelEndpoint* sender;
    
    // Load metrics
    atomic_size_t active_connections;
    atomic_uint64_t total_processed;
    atomic_uint64_t total_response_time_ms;
    atomic_uint64_t last_response_time_ms;
    
    // Health status
    bool is_healthy;
    uint64_t last_health_check;
    uint32_t weight;
    
    struct WorkerNode* next;
} WorkerNode;

// Load balanced channel
typedef struct LoadBalancedChannel {
    Channel* input_channel;
    ChannelEndpoint* input_receiver;
    
    WorkerNode* workers;
    size_t worker_count;
    LoadBalancedChannelConfig config;
    
    // Load balancing state
    atomic_size_t round_robin_counter;
    atomic_size_t total_weight;
    
    // Distribution thread
    pthread_t distributor_thread;
    bool distributor_active;
    
    // Health checking
    pthread_t health_checker_thread;
    bool health_checker_active;
    
    pthread_mutex_t lb_mutex;
} LoadBalancedChannel;

// Round-robin load balancing
static WorkerNode* select_worker_round_robin(LoadBalancedChannel* lb_channel) {
    if (!lb_channel || lb_channel->worker_count == 0) return NULL;
    
    size_t index = atomic_fetch_add(&lb_channel->round_robin_counter, 1) % lb_channel->worker_count;
    
    WorkerNode* worker = lb_channel->workers;
    for (size_t i = 0; i < index && worker; i++) {
        worker = worker->next;
    }
    
    return worker && worker->is_healthy ? worker : NULL;
}

// Least connections load balancing
static WorkerNode* select_worker_least_connections(LoadBalancedChannel* lb_channel) {
    if (!lb_channel || lb_channel->worker_count == 0) return NULL;
    
    WorkerNode* selected = NULL;
    size_t min_connections = SIZE_MAX;
    
    WorkerNode* worker = lb_channel->workers;
    while (worker) {
        if (worker->is_healthy) {
            size_t connections = atomic_load(&worker->active_connections);
            if (connections < min_connections) {
                min_connections = connections;
                selected = worker;
            }
        }
        worker = worker->next;
    }
    
    return selected;
}

// Weighted round-robin load balancing
static WorkerNode* select_worker_weighted_round_robin(LoadBalancedChannel* lb_channel) {
    if (!lb_channel || lb_channel->worker_count == 0) return NULL;
    
    size_t total_weight = atomic_load(&lb_channel->total_weight);
    if (total_weight == 0) return select_worker_round_robin(lb_channel);
    
    size_t weight_index = atomic_fetch_add(&lb_channel->round_robin_counter, 1) % total_weight;
    size_t current_weight = 0;
    
    WorkerNode* worker = lb_channel->workers;
    while (worker) {
        if (worker->is_healthy) {
            current_weight += worker->weight;
            if (current_weight > weight_index) {
                return worker;
            }
        }
        worker = worker->next;
    }
    
    return NULL;
}

// Least response time load balancing
static WorkerNode* select_worker_least_response_time(LoadBalancedChannel* lb_channel) {
    if (!lb_channel || lb_channel->worker_count == 0) return NULL;
    
    WorkerNode* selected = NULL;
    uint64_t min_response_time = UINT64_MAX;
    
    WorkerNode* worker = lb_channel->workers;
    while (worker) {
        if (worker->is_healthy) {
            uint64_t avg_response_time = 0;
            uint64_t total_processed = atomic_load(&worker->total_processed);
            if (total_processed > 0) {
                avg_response_time = atomic_load(&worker->total_response_time_ms) / total_processed;
            }
            
            if (avg_response_time < min_response_time) {
                min_response_time = avg_response_time;
                selected = worker;
            }
        }
        worker = worker->next;
    }
    
    return selected;
}

// Worker selection dispatcher
static WorkerNode* select_worker(LoadBalancedChannel* lb_channel) {
    switch (lb_channel->config.strategy) {
        case LOAD_BALANCE_ROUND_ROBIN:
            return select_worker_round_robin(lb_channel);
        case LOAD_BALANCE_LEAST_CONNECTIONS:
            return select_worker_least_connections(lb_channel);
        case LOAD_BALANCE_WEIGHTED_ROUND_ROBIN:
            return select_worker_weighted_round_robin(lb_channel);
        case LOAD_BALANCE_LEAST_RESPONSE_TIME:
            return select_worker_least_response_time(lb_channel);
        case LOAD_BALANCE_RANDOM:
            if (lb_channel->worker_count > 0) {
                size_t index = rand() % lb_channel->worker_count;
                WorkerNode* worker = lb_channel->workers;
                for (size_t i = 0; i < index && worker; i++) {
                    worker = worker->next;
                }
                return worker && worker->is_healthy ? worker : NULL;
            }
            break;
        default:
            return select_worker_round_robin(lb_channel);
    }
    return NULL;
}

// Load balancer message distribution worker
static void* load_balancer_distributor_worker(void* arg) {
    LoadBalancedChannel* lb_channel = (LoadBalancedChannel*)arg;
    
    while (lb_channel->distributor_active) {
        void* data;
        size_t data_size;
        
        // Receive message from input channel
        Result_void_ptr receive_result = channel_receive_timeout(
            lb_channel->input_receiver, &data, &data_size, 1000);
        
        if (receive_result.is_error) {
            if (receive_result.error->code == ERROR_CHANNEL_TIMEOUT) {
                // Timeout is expected, continue
                error_destroy(receive_result.error);
                continue;
            } else if (receive_result.error->code == ERROR_CHANNEL_CLOSED) {
                // Channel closed, exit
                error_destroy(receive_result.error);
                break;
            } else {
                // Other error, log and continue
                error_destroy(receive_result.error);
                continue;
            }
        }
        
        // Select worker for load balancing
        WorkerNode* worker = select_worker(lb_channel);
        if (!worker) {
            // No healthy workers available
            free(data);
            continue;
        }
        
        // Forward message to selected worker
        atomic_fetch_add(&worker->active_connections, 1);
        uint64_t start_time = get_monotonic_time_ns();
        
        Result_void_ptr send_result = channel_send(worker->sender, data, data_size);
        
        uint64_t end_time = get_monotonic_time_ns();
        uint64_t response_time = (end_time - start_time) / 1000000; // Convert to ms
        
        atomic_fetch_add(&worker->total_processed, 1);
        atomic_fetch_add(&worker->total_response_time_ms, response_time);
        atomic_store(&worker->last_response_time_ms, response_time);
        atomic_fetch_sub(&worker->active_connections, 1);
        
        if (send_result.is_error) {
            // Worker failed, mark as unhealthy
            worker->is_healthy = false;
            error_destroy(send_result.error);
        }
        
        free(data);
    }
    
    return NULL;
}

// Health checker worker
static void* load_balancer_health_checker_worker(void* arg) {
    LoadBalancedChannel* lb_channel = (LoadBalancedChannel*)arg;
    
    while (lb_channel->health_checker_active) {
        uint64_t current_time = get_monotonic_time_ns();
        
        pthread_mutex_lock(&lb_channel->lb_mutex);
        
        WorkerNode* worker = lb_channel->workers;
        while (worker) {
            // Simple health check - if worker hasn't processed anything recently, mark unhealthy
            uint64_t time_since_activity = current_time - worker->last_health_check;
            
            if (time_since_activity > lb_channel->config.health_check_interval_ms * 1000000 * 5) {
                // TODO: Implement actual health check (e.g., ping message)
                // For now, assume worker is healthy if channel is not closed
                worker->is_healthy = !channel_is_closed(worker->worker_channel);
            }
            
            worker->last_health_check = current_time;
            worker = worker->next;
        }
        
        pthread_mutex_unlock(&lb_channel->lb_mutex);
        
        usleep(lb_channel->config.health_check_interval_ms * 1000);
    }
    
    return NULL;
}

// Create load balanced channel
static LoadBalancedChannel* load_balanced_channel_create(
    Channel* input_channel, 
    Channel** worker_channels, 
    size_t worker_count,
    LoadBalancedChannelConfig config) {
    
    if (!input_channel || !worker_channels || worker_count == 0) {
        return NULL;
    }
    
    LoadBalancedChannel* lb_channel = calloc(1, sizeof(LoadBalancedChannel));
    if (!lb_channel) return NULL;
    
    lb_channel->input_channel = input_channel;
    lb_channel->input_receiver = channel_get_receiver(input_channel);
    if (!lb_channel->input_receiver) {
        free(lb_channel);
        return NULL;
    }
    
    lb_channel->config = config;
    lb_channel->worker_count = worker_count;
    
    if (pthread_mutex_init(&lb_channel->lb_mutex, NULL) != 0) {
        channel_endpoint_destroy(lb_channel->input_receiver);
        free(lb_channel);
        return NULL;
    }
    
    // Create worker nodes
    WorkerNode* prev_worker = NULL;
    size_t total_weight = 0;
    
    for (size_t i = 0; i < worker_count; i++) {
        WorkerNode* worker = calloc(1, sizeof(WorkerNode));
        if (!worker) {
            // Cleanup on failure
            // TODO: Implement proper cleanup
            return NULL;
        }
        
        worker->worker_id = generate_unique_id();
        worker->worker_channel = worker_channels[i];
        worker->sender = channel_get_sender(worker_channels[i]);
        worker->is_healthy = true;
        worker->weight = config.worker_weights ? config.worker_weights[i] : 1;
        total_weight += worker->weight;
        
        if (prev_worker) {
            prev_worker->next = worker;
        } else {
            lb_channel->workers = worker;
        }
        prev_worker = worker;
    }
    
    atomic_store(&lb_channel->total_weight, total_weight);
    
    return lb_channel;
}

// Start load balanced channel
static Result_void_ptr load_balanced_channel_start(LoadBalancedChannel* lb_channel) {
    if (!lb_channel) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid load balanced channel"));
    }
    
    lb_channel->distributor_active = true;
    if (pthread_create(&lb_channel->distributor_thread, NULL, 
                       load_balancer_distributor_worker, lb_channel) != 0) {
        lb_channel->distributor_active = false;
        return ERR_PTR(error_create(ERROR_THREAD_CREATION, "Failed to create distributor thread"));
    }
    
    if (lb_channel->config.health_checking) {
        lb_channel->health_checker_active = true;
        if (pthread_create(&lb_channel->health_checker_thread, NULL,
                           load_balancer_health_checker_worker, lb_channel) != 0) {
            lb_channel->health_checker_active = false;
            // Continue without health checking
        }
    }
    
    return OK_PTR(NULL);
}

// Stop load balanced channel
static Result_void_ptr load_balanced_channel_stop(LoadBalancedChannel* lb_channel) {
    if (!lb_channel) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid load balanced channel"));
    }
    
    lb_channel->distributor_active = false;
    lb_channel->health_checker_active = false;
    
    if (lb_channel->distributor_thread) {
        pthread_join(lb_channel->distributor_thread, NULL);
    }
    
    if (lb_channel->health_checker_thread) {
        pthread_join(lb_channel->health_checker_thread, NULL);
    }
    
    return OK_PTR(NULL);
}

// Destroy load balanced channel
static void load_balanced_channel_destroy(LoadBalancedChannel* lb_channel) {
    if (!lb_channel) return;
    
    load_balanced_channel_stop(lb_channel);
    
    // Cleanup workers
    WorkerNode* worker = lb_channel->workers;
    while (worker) {
        WorkerNode* next = worker->next;
        channel_endpoint_destroy(worker->sender);
        free(worker);
        worker = next;
    }
    
    channel_endpoint_destroy(lb_channel->input_receiver);
    pthread_mutex_destroy(&lb_channel->lb_mutex);
    free(lb_channel);
}

// ============================================================================
// CHANNEL PERSISTENCE AND RECOVERY
// ============================================================================

// Persistence configuration
typedef struct ChannelPersistenceConfig {
    bool enable_persistence;
    char storage_path[256];
    size_t max_stored_messages;
    uint64_t flush_interval_ms;
    bool compress_messages;
    enum {
        PERSISTENCE_MEMORY,
        PERSISTENCE_FILE,
        PERSISTENCE_DATABASE
    } storage_type;
} ChannelPersistenceConfig;

// Persistent channel structure
typedef struct PersistentChannel {
    Channel* channel;
    ChannelPersistenceConfig config;
    
    // Storage state
    FILE* storage_file;
    void* storage_handle;  // For database connections
    
    // Message buffer for batching writes
    ChannelMessage** write_buffer;
    size_t write_buffer_size;
    size_t write_buffer_capacity;
    
    // Persistence worker thread
    pthread_t persistence_thread;
    bool persistence_active;
    
    pthread_mutex_t persistence_mutex;
    pthread_cond_t flush_condition;
} PersistentChannel;

// Create persistent channel
static PersistentChannel* persistent_channel_create(Channel* channel, ChannelPersistenceConfig config) {
    if (!channel) return NULL;
    
    PersistentChannel* persistent = calloc(1, sizeof(PersistentChannel));
    if (!persistent) return NULL;
    
    persistent->channel = channel;
    persistent->config = config;
    
    // Initialize write buffer
    persistent->write_buffer_capacity = 1000;
    persistent->write_buffer = calloc(persistent->write_buffer_capacity, sizeof(ChannelMessage*));
    if (!persistent->write_buffer) {
        free(persistent);
        return NULL;
    }
    
    if (pthread_mutex_init(&persistent->persistence_mutex, NULL) != 0) {
        free(persistent->write_buffer);
        free(persistent);
        return NULL;
    }
    
    if (pthread_cond_init(&persistent->flush_condition, NULL) != 0) {
        pthread_mutex_destroy(&persistent->persistence_mutex);
        free(persistent->write_buffer);
        free(persistent);
        return NULL;
    }
    
    // Open storage based on type
    if (config.storage_type == PERSISTENCE_FILE && config.enable_persistence) {
        persistent->storage_file = fopen(config.storage_path, "a+b");
        if (!persistent->storage_file) {
            pthread_cond_destroy(&persistent->flush_condition);
            pthread_mutex_destroy(&persistent->persistence_mutex);
            free(persistent->write_buffer);
            free(persistent);
            return NULL;
        }
    }
    
    return persistent;
}

// Persistence worker thread
static void* channel_persistence_worker(void* arg) {
    PersistentChannel* persistent = (PersistentChannel*)arg;
    
    while (persistent->persistence_active) {
        pthread_mutex_lock(&persistent->persistence_mutex);
        
        // Wait for flush signal or timeout
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += persistent->config.flush_interval_ms / 1000;
        timeout.tv_nsec += (persistent->config.flush_interval_ms % 1000) * 1000000;
        
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec++;
            timeout.tv_nsec -= 1000000000;
        }
        
        pthread_cond_timedwait(&persistent->flush_condition, &persistent->persistence_mutex, &timeout);
        
        // Flush write buffer to storage
        if (persistent->write_buffer_size > 0 && persistent->storage_file) {
            for (size_t i = 0; i < persistent->write_buffer_size; i++) {
                ChannelMessage* message = persistent->write_buffer[i];
                
                // Write message metadata
                fwrite(&message->metadata, sizeof(MessageMetadata), 1, persistent->storage_file);
                
                // Write message data size
                fwrite(&message->data_size, sizeof(size_t), 1, persistent->storage_file);
                
                // Write message data
                if (message->data && message->data_size > 0) {
                    fwrite(message->data, message->data_size, 1, persistent->storage_file);
                }
                
                // Cleanup
                channel_message_destroy(message);
            }
            
            fflush(persistent->storage_file);
            persistent->write_buffer_size = 0;
        }
        
        pthread_mutex_unlock(&persistent->persistence_mutex);
    }
    
    return NULL;
}

// Store message for persistence
static Result_void_ptr persistent_channel_store_message(PersistentChannel* persistent, ChannelMessage* message) {
    if (!persistent || !message || !persistent->config.enable_persistence) {
        return OK_PTR(NULL);  // Persistence disabled
    }
    
    pthread_mutex_lock(&persistent->persistence_mutex);
    
    // Check buffer capacity
    if (persistent->write_buffer_size >= persistent->write_buffer_capacity) {
        // Force flush or drop oldest message
        pthread_cond_signal(&persistent->flush_condition);
        pthread_mutex_unlock(&persistent->persistence_mutex);
        return ERR_PTR(error_create(ERROR_CHANNEL_FULL, "Persistence buffer full"));
    }
    
    // Create a copy of the message for persistence
    ChannelMessage* persistent_message = channel_message_create(message->data, message->data_size);
    if (!persistent_message) {
        pthread_mutex_unlock(&persistent->persistence_mutex);
        return ERR_PTR(error_create(ERROR_MEMORY_ALLOCATION, "Failed to create persistent message"));
    }
    
    persistent_message->metadata = message->metadata;
    persistent->write_buffer[persistent->write_buffer_size++] = persistent_message;
    
    pthread_mutex_unlock(&persistent->persistence_mutex);
    
    return OK_PTR(NULL);
}

// Recover messages from storage
static Result_void_ptr persistent_channel_recover(PersistentChannel* persistent) {
    if (!persistent || !persistent->config.enable_persistence || !persistent->storage_file) {
        return OK_PTR(NULL);
    }
    
    // Seek to beginning of file
    fseek(persistent->storage_file, 0, SEEK_SET);
    
    size_t recovered_count = 0;
    MessageMetadata metadata;
    size_t data_size;
    
    while (fread(&metadata, sizeof(MessageMetadata), 1, persistent->storage_file) == 1) {
        // Read data size
        if (fread(&data_size, sizeof(size_t), 1, persistent->storage_file) != 1) {
            break;
        }
        
        // Read message data
        void* data = NULL;
        if (data_size > 0) {
            data = malloc(data_size);
            if (!data || fread(data, data_size, 1, persistent->storage_file) != 1) {
                free(data);
                break;
            }
        }
        
        // Create message and add to channel
        ChannelMessage* message = channel_message_create(data, data_size);
        if (message) {
            message->metadata = metadata;
            message->owns_data = true;  // Message owns the data
            
            // Add to channel queue
            pthread_mutex_lock(&persistent->channel->channel_mutex);
            
            if (!persistent->channel->message_queue) {
                persistent->channel->message_queue = message;
                persistent->channel->message_queue_tail = message;
            } else {
                persistent->channel->message_queue_tail->next = message;
                persistent->channel->message_queue_tail = message;
            }
            persistent->channel->current_size++;
            
            pthread_mutex_unlock(&persistent->channel->channel_mutex);
            pthread_cond_signal(&persistent->channel->message_available);
            
            recovered_count++;
        }
    }
    
    return OK_PTR(NULL);
}

// ============================================================================
// CHANNEL TRANSFORMATION OPERATIONS
// ============================================================================

// Transformation function types
typedef Result_void_ptr (*ChannelMapFunction)(void* input_data, size_t input_size, void** output_data, size_t* output_size, void* context);
typedef Result_bool (*ChannelFilterFunction)(void* data, size_t data_size, void* context);
typedef Result_void_ptr (*ChannelReduceFunction)(void* accumulator, void* data, size_t data_size, void* context);

// Transformation configuration
typedef struct ChannelTransformConfig {
    bool parallel_processing;
    size_t worker_count;
    size_t batch_size;
    uint64_t timeout_ms;
    void* user_context;
} ChannelTransformConfig;

// Transformation pipeline stage
typedef struct TransformStage {
    enum {
        TRANSFORM_MAP,
        TRANSFORM_FILTER,
        TRANSFORM_REDUCE
    } type;
    
    union {
        ChannelMapFunction map_func;
        ChannelFilterFunction filter_func;
        ChannelReduceFunction reduce_func;
    } function;
    
    ChannelTransformConfig config;
    
    Channel* input_channel;
    Channel* output_channel;
    ChannelEndpoint* input_receiver;
    ChannelEndpoint* output_sender;
    
    pthread_t worker_thread;
    bool worker_active;
    
    struct TransformStage* next;
} TransformStage;

// Map transformation worker
static void* channel_map_worker(void* arg) {
    TransformStage* stage = (TransformStage*)arg;
    
    while (stage->worker_active) {
        void* input_data;
        size_t input_size;
        
        // Receive input message
        Result_void_ptr receive_result = channel_receive_timeout(
            stage->input_receiver, &input_data, &input_size, stage->config.timeout_ms);
        
        if (receive_result.is_error) {
            if (receive_result.error->code == ERROR_CHANNEL_TIMEOUT) {
                error_destroy(receive_result.error);
                continue;
            } else if (receive_result.error->code == ERROR_CHANNEL_CLOSED) {
                error_destroy(receive_result.error);
                break;
            } else {
                error_destroy(receive_result.error);
                continue;
            }
        }
        
        // Apply map function
        void* output_data;
        size_t output_size;
        
        Result_void_ptr map_result = stage->function.map_func(
            input_data, input_size, &output_data, &output_size, stage->config.user_context);
        
        if (!map_result.is_error) {
            // Send transformed message
            channel_send(stage->output_sender, output_data, output_size);
            free(output_data);
        } else {
            error_destroy(map_result.error);
        }
        
        free(input_data);
    }
    
    return NULL;
}

// Filter transformation worker
static void* channel_filter_worker(void* arg) {
    TransformStage* stage = (TransformStage*)arg;
    
    while (stage->worker_active) {
        void* input_data;
        size_t input_size;
        
        // Receive input message
        Result_void_ptr receive_result = channel_receive_timeout(
            stage->input_receiver, &input_data, &input_size, stage->config.timeout_ms);
        
        if (receive_result.is_error) {
            if (receive_result.error->code == ERROR_CHANNEL_TIMEOUT) {
                error_destroy(receive_result.error);
                continue;
            } else if (receive_result.error->code == ERROR_CHANNEL_CLOSED) {
                error_destroy(receive_result.error);
                break;
            } else {
                error_destroy(receive_result.error);
                continue;
            }
        }
        
        // Apply filter function
        Result_bool filter_result = stage->function.filter_func(
            input_data, input_size, stage->config.user_context);
        
        if (!filter_result.is_error && filter_result.value) {
            // Message passes filter, forward it
            channel_send(stage->output_sender, input_data, input_size);
        }
        
        if (filter_result.is_error) {
            error_destroy(filter_result.error);
        }
        
        free(input_data);
    }
    
    return NULL;
}

// Create map transformation stage
static TransformStage* channel_transform_map_create(
    Channel* input, Channel* output, ChannelMapFunction map_func, ChannelTransformConfig config) {
    
    if (!input || !output || !map_func) return NULL;
    
    TransformStage* stage = calloc(1, sizeof(TransformStage));
    if (!stage) return NULL;
    
    stage->type = TRANSFORM_MAP;
    stage->function.map_func = map_func;
    stage->config = config;
    stage->input_channel = input;
    stage->output_channel = output;
    
    stage->input_receiver = channel_get_receiver(input);
    stage->output_sender = channel_get_sender(output);
    
    if (!stage->input_receiver || !stage->output_sender) {
        channel_endpoint_destroy(stage->input_receiver);
        channel_endpoint_destroy(stage->output_sender);
        free(stage);
        return NULL;
    }
    
    return stage;
}

// Create filter transformation stage
static TransformStage* channel_transform_filter_create(
    Channel* input, Channel* output, ChannelFilterFunction filter_func, ChannelTransformConfig config) {
    
    if (!input || !output || !filter_func) return NULL;
    
    TransformStage* stage = calloc(1, sizeof(TransformStage));
    if (!stage) return NULL;
    
    stage->type = TRANSFORM_FILTER;
    stage->function.filter_func = filter_func;
    stage->config = config;
    stage->input_channel = input;
    stage->output_channel = output;
    
    stage->input_receiver = channel_get_receiver(input);
    stage->output_sender = channel_get_sender(output);
    
    if (!stage->input_receiver || !stage->output_sender) {
        channel_endpoint_destroy(stage->input_receiver);
        channel_endpoint_destroy(stage->output_sender);
        free(stage);
        return NULL;
    }
    
    return stage;
}

// Start transformation stage
static Result_void_ptr channel_transform_stage_start(TransformStage* stage) {
    if (!stage) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid transformation stage"));
    }
    
    stage->worker_active = true;
    
    void* (*worker_func)(void*) = NULL;
    switch (stage->type) {
        case TRANSFORM_MAP:
            worker_func = channel_map_worker;
            break;
        case TRANSFORM_FILTER:
            worker_func = channel_filter_worker;
            break;
        default:
            return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Unsupported transformation type"));
    }
    
    if (pthread_create(&stage->worker_thread, NULL, worker_func, stage) != 0) {
        stage->worker_active = false;
        return ERR_PTR(error_create(ERROR_THREAD_CREATION, "Failed to create transformation worker"));
    }
    
    return OK_PTR(NULL);
}

// Stop transformation stage
static Result_void_ptr channel_transform_stage_stop(TransformStage* stage) {
    if (!stage) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid transformation stage"));
    }
    
    stage->worker_active = false;
    
    if (stage->worker_thread) {
        pthread_join(stage->worker_thread, NULL);
    }
    
    return OK_PTR(NULL);
}

// Destroy transformation stage
static void channel_transform_stage_destroy(TransformStage* stage) {
    if (!stage) return;
    
    channel_transform_stage_stop(stage);
    
    channel_endpoint_destroy(stage->input_receiver);
    channel_endpoint_destroy(stage->output_sender);
    
    free(stage);
}