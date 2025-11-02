#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "advanced_channels.h"
#include "error_hierarchies.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fnmatch.h>

// =============================================================================
// Global State and Utilities
// =============================================================================

// Global counters
static atomic_uint_least64_t g_next_channel_id = 1;
static atomic_uint_least64_t g_next_message_id = 1;
static atomic_uint_least64_t g_next_node_id = 1;

// Thread-local selector for channel operations
static __thread ChannelSelector* g_current_selector = NULL;

// Global performance tracking
static ChannelPerformanceReport g_global_performance = {0};
static pthread_mutex_t g_performance_mutex = PTHREAD_MUTEX_INITIALIZER;

// Utility functions
static uint64_t get_current_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000UL + (uint64_t)ts.tv_nsec;
}

static uint64_t get_current_time_ms(void) {
    return get_current_time_ns() / 1000000;
}

static char* safe_strdup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* copy = malloc(len + 1);
    if (copy) {
        memcpy(copy, str, len + 1);
    }
    return copy;
}

// =============================================================================
// Message Management Implementation
// =============================================================================

ChannelMessage* channel_message_create(const void* data, size_t data_size, const char* type) {
    return channel_message_create_with_priority(data, data_size, type, MSG_PRIORITY_NORMAL);
}

ChannelMessage* channel_message_create_with_priority(const void* data, size_t data_size, 
                                                    const char* type, MessagePriority priority) {
    if (!data && data_size > 0) return NULL;
    
    ChannelMessage* message = calloc(1, sizeof(ChannelMessage));
    if (!message) return NULL;
    
    message->message_id = atomic_fetch_add(&g_next_message_id, 1);
    message->priority = priority;
    
    // Copy message data
    if (data && data_size > 0) {
        message->data = malloc(data_size);
        if (!message->data) {
            free(message);
            return NULL;
        }
        memcpy(message->data, data, data_size);
        message->data_size = data_size;
        message->owns_data = true;
    }
    
    // Set message type
    message->message_type = safe_strdup(type);
    message->type_hash = channel_hash_string(type);
    
    // Initialize metadata
    message->metadata.timestamp_ns = get_current_time_ns();
    message->metadata.sequence_number = message->message_id;
    message->metadata.expiry_time_ns = 0; // No expiry by default
    message->metadata.retry_count = 0;
    message->metadata.max_retries = 3;
    
    // Initialize routing
    message->route.source_node_id = 0;
    message->route.target_node_id = 0;
    message->route.routing_key = NULL;
    message->route.hash_key = 0;
    message->route.weight = 1.0;
    
    // Initialize reference counting
    atomic_init(&message->ref_count, 1);
    if (pthread_mutex_init(&message->ref_mutex, NULL) != 0) {
        free(message->data);
        free((void*)message->message_type);
        free(message);
        return NULL;
    }
    
    // Create message arena
    message->message_arena = goo_arena_new(1024, "channel_message");
    
    return message;
}

void channel_message_retain(ChannelMessage* message) {
    if (!message) return;
    
    pthread_mutex_lock(&message->ref_mutex);
    atomic_fetch_add(&message->ref_count, 1);
    pthread_mutex_unlock(&message->ref_mutex);
}

void channel_message_release(ChannelMessage* message) {
    if (!message) return;
    
    pthread_mutex_lock(&message->ref_mutex);
    int old_count = atomic_fetch_sub(&message->ref_count, 1);
    pthread_mutex_unlock(&message->ref_mutex);
    
    if (old_count <= 1) {
        channel_message_destroy(message);
    }
}

void channel_message_destroy(ChannelMessage* message) {
    if (!message) return;
    
    // Clean up data
    if (message->owns_data && message->data) {
        if (message->data_destructor) {
            message->data_destructor(message->data);
        } else {
            free(message->data);
        }
    }
    
    // Clean up strings
    free((void*)message->message_type);
    free((void*)message->route.routing_key);
    free((void*)message->route.region);
    free((void*)message->route.datacenter);
    free((void*)message->route.rack);
    free((void*)message->metadata.correlation_id);
    
    // Clean up synchronization
    pthread_mutex_destroy(&message->ref_mutex);
    
    // Clean up arena
    goo_arena_free(message->message_arena);
    
    free(message);
}

// =============================================================================
// Priority Queue Operations for Channel Messages
// =============================================================================

static void message_heap_swap(ChannelMessage** heap, size_t i, size_t j) {
    ChannelMessage* temp = heap[i];
    heap[i] = heap[j];
    heap[j] = temp;
}

static void message_heap_bubble_up(ChannelMessage** heap, size_t index) {
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (heap[index]->priority >= heap[parent]->priority) break;
        message_heap_swap(heap, index, parent);
        index = parent;
    }
}

static void message_heap_bubble_down(ChannelMessage** heap, size_t heap_size, size_t index) {
    while (true) {
        size_t smallest = index;
        size_t left = 2 * index + 1;
        size_t right = 2 * index + 2;
        
        if (left < heap_size && heap[left]->priority < heap[smallest]->priority) {
            smallest = left;
        }
        
        if (right < heap_size && heap[right]->priority < heap[smallest]->priority) {
            smallest = right;
        }
        
        if (smallest == index) break;
        
        message_heap_swap(heap, index, smallest);
        index = smallest;
    }
}

// =============================================================================
// Advanced Channel Core Implementation
// =============================================================================

AdvancedChannel* advanced_channel_create(const char* name, AdvancedChannelType type) {
    ChannelConfig config = {
        .name = name,
        .type = type,
        .capacity = 1000,
        .max_message_size = 1024 * 1024, // 1MB
        .drop_on_overflow = false,
        .ordering = ORDER_FIFO,
        .delivery = DELIVERY_AT_LEAST_ONCE,
        .send_timeout_ms = 30000,
        .receive_timeout_ms = 30000,
        .message_ttl_ms = 0, // No TTL by default
        .max_retries = 3,
        .retry_delay_ms = 1000,
        .exponential_backoff = true,
        .use_memory_pool = true,
        .memory_pool_size = 1024 * 1024, // 1MB
        .batch_operations = false,
        .batch_size = 10,
        .enable_metrics = true,
        .enable_tracing = false
    };
    
    return advanced_channel_create_with_config(&config);
}

AdvancedChannel* advanced_channel_create_with_config(const ChannelConfig* config) {
    if (!config) return NULL;
    
    AdvancedChannel* channel = calloc(1, sizeof(AdvancedChannel));
    if (!channel) return NULL;
    
    channel->channel_id = atomic_fetch_add(&g_next_channel_id, 1);
    channel->name = safe_strdup(config->name);
    channel->type = config->type;
    channel->config = *config;
    
    // Initialize capacity and allocate message buffer
    channel->capacity = config->capacity > 0 ? config->capacity : 1000;
    channel->messages = calloc(channel->capacity, sizeof(ChannelMessage*));
    if (!channel->messages) {
        advanced_channel_destroy(channel);
        return NULL;
    }
    
    // Initialize priority heap for priority channels
    if (config->type == CHANNEL_TYPE_PRIORITY) {
        channel->heap_capacity = channel->capacity;
        channel->priority_heap = calloc(channel->heap_capacity, sizeof(ChannelMessage*));
        if (!channel->priority_heap) {
            advanced_channel_destroy(channel);
            return NULL;
        }
    }
    
    // Initialize atomic variables
    atomic_init(&channel->head, 0);
    atomic_init(&channel->tail, 0);
    atomic_init(&channel->size, 0);
    atomic_init(&channel->is_closed, false);
    atomic_init(&channel->sender_count, 0);
    atomic_init(&channel->receiver_count, 0);
    
    // Initialize synchronization
    if (pthread_mutex_init(&channel->send_mutex, NULL) != 0 ||
        pthread_mutex_init(&channel->recv_mutex, NULL) != 0 ||
        pthread_cond_init(&channel->not_empty, NULL) != 0 ||
        pthread_cond_init(&channel->not_full, NULL) != 0 ||
        pthread_mutex_init(&channel->stats_mutex, NULL) != 0) {
        advanced_channel_destroy(channel);
        return NULL;
    }
    
    // Initialize arrays
    channel->subscribers = NULL;
    channel->subscriber_count = 0;
    channel->selectors = NULL;
    channel->selector_count = 0;
    
    // Initialize statistics
    memset(&channel->stats, 0, sizeof(ChannelStats));
    
    // Create channel arena
    channel->channel_arena = goo_arena_new(8192, channel->name ? channel->name : "advanced_channel");
    
    // Initialize type-specific extensions
    switch (config->type) {
        case CHANNEL_TYPE_DISTRIBUTED:
            // Would initialize distributed channel data
            break;
        case CHANNEL_TYPE_LOAD_BALANCED:
            // Would initialize load balancer
            break;
        case CHANNEL_TYPE_BROADCAST:
            // Would initialize broadcaster
            break;
        case CHANNEL_TYPE_PIPELINE:
            // Would initialize pipeline
            break;
        default:
            break;
    }
    
    // Update global performance tracking
    pthread_mutex_lock(&g_performance_mutex);
    g_global_performance.total_channels++;
    pthread_mutex_unlock(&g_performance_mutex);
    
    return channel;
}

void advanced_channel_destroy(AdvancedChannel* channel) {
    if (!channel) return;
    
    // Close channel if not already closed
    if (!atomic_load(&channel->is_closed)) {
        advanced_channel_close(channel);
    }
    
    // Clean up remaining messages
    pthread_mutex_lock(&channel->send_mutex);
    pthread_mutex_lock(&channel->recv_mutex);
    
    size_t current_size = atomic_load(&channel->size);
    for (size_t i = 0; i < current_size; i++) {
        size_t index = (atomic_load(&channel->head) + i) % channel->capacity;
        if (channel->messages[index]) {
            channel_message_release(channel->messages[index]);
        }
    }
    
    // Clean up priority heap
    for (size_t i = 0; i < channel->heap_size; i++) {
        if (channel->priority_heap[i]) {
            channel_message_release(channel->priority_heap[i]);
        }
    }
    
    pthread_mutex_unlock(&channel->recv_mutex);
    pthread_mutex_unlock(&channel->send_mutex);
    
    // Clean up memory
    free(channel->messages);
    free(channel->priority_heap);
    free(channel->subscribers);
    free(channel->selectors);
    
    // Clean up type-specific extensions
    switch (channel->type) {
        case CHANNEL_TYPE_DISTRIBUTED:
            if (channel->ext.distributed) {
                distributed_channel_destroy(channel->ext.distributed);
            }
            break;
        case CHANNEL_TYPE_LOAD_BALANCED:
            if (channel->ext.load_balancer) {
                load_balancer_destroy(channel->ext.load_balancer);
            }
            break;
        case CHANNEL_TYPE_BROADCAST:
            if (channel->ext.broadcaster) {
                channel_broadcaster_destroy(channel->ext.broadcaster);
            }
            break;
        case CHANNEL_TYPE_PIPELINE:
            if (channel->ext.pipeline) {
                channel_pipeline_destroy(channel->ext.pipeline);
            }
            break;
        default:
            break;
    }
    
    // Clean up synchronization
    pthread_cond_destroy(&channel->not_full);
    pthread_cond_destroy(&channel->not_empty);
    pthread_mutex_destroy(&channel->stats_mutex);
    pthread_mutex_destroy(&channel->recv_mutex);
    pthread_mutex_destroy(&channel->send_mutex);
    
    // Clean up arena
    goo_arena_free(channel->channel_arena);
    
    // Clean up name
    free((void*)channel->name);
    
    // Update global performance tracking
    pthread_mutex_lock(&g_performance_mutex);
    g_global_performance.total_channels--;
    pthread_mutex_unlock(&g_performance_mutex);
    
    free(channel);
}

bool advanced_channel_send(AdvancedChannel* channel, ChannelMessage* message) {
    return advanced_channel_send_timeout(channel, message, channel->config.send_timeout_ms);
}

bool advanced_channel_send_timeout(AdvancedChannel* channel, ChannelMessage* message, uint64_t timeout_ms) {
    if (!channel || !message) return false;
    
    // Check if channel is closed
    if (atomic_load(&channel->is_closed)) {
        return false;
    }
    
    // Increment sender count
    atomic_fetch_add(&channel->sender_count, 1);
    
    pthread_mutex_lock(&channel->send_mutex);
    
    bool result = false;
    uint64_t start_time = get_current_time_ns();
    
    // Check message size limit
    if (channel->config.max_message_size > 0 && 
        message->data_size > channel->config.max_message_size) {
        pthread_mutex_unlock(&channel->send_mutex);
        atomic_fetch_sub(&channel->sender_count, 1);
        return false;
    }
    
    // Check message TTL
    if (channel->config.message_ttl_ms > 0) {
        uint64_t now = get_current_time_ns();
        if (message->metadata.expiry_time_ns == 0) {
            message->metadata.expiry_time_ns = now + (channel->config.message_ttl_ms * 1000000);
        } else if (now > message->metadata.expiry_time_ns) {
            // Message has expired
            pthread_mutex_unlock(&channel->send_mutex);
            atomic_fetch_sub(&channel->sender_count, 1);
            return false;
        }
    }
    
    // Wait for space in channel
    size_t current_size = atomic_load(&channel->size);
    while (current_size >= channel->capacity && !atomic_load(&channel->is_closed)) {
        if (channel->config.drop_on_overflow) {
            // Drop message and update statistics
            pthread_mutex_lock(&channel->stats_mutex);
            channel->stats.messages_dropped++;
            pthread_mutex_unlock(&channel->stats_mutex);
            
            pthread_mutex_unlock(&channel->send_mutex);
            atomic_fetch_sub(&channel->sender_count, 1);
            return false;
        }
        
        // Wait with timeout
        if (timeout_ms > 0) {
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += timeout_ms / 1000;
            timeout.tv_nsec += (timeout_ms % 1000) * 1000000;
            if (timeout.tv_nsec >= 1000000000) {
                timeout.tv_sec++;
                timeout.tv_nsec -= 1000000000;
            }
            
            int wait_result = pthread_cond_timedwait(&channel->not_full, &channel->send_mutex, &timeout);
            if (wait_result == ETIMEDOUT) {
                pthread_mutex_unlock(&channel->send_mutex);
                atomic_fetch_sub(&channel->sender_count, 1);
                return false;
            }
        } else {
            pthread_cond_wait(&channel->not_full, &channel->send_mutex);
        }
        
        current_size = atomic_load(&channel->size);
    }
    
    if (atomic_load(&channel->is_closed)) {
        pthread_mutex_unlock(&channel->send_mutex);
        atomic_fetch_sub(&channel->sender_count, 1);
        return false;
    }
    
    // Add message to channel
    channel_message_retain(message); // Retain for channel storage
    
    if (channel->type == CHANNEL_TYPE_PRIORITY && channel->priority_heap) {
        // Add to priority heap
        if (channel->heap_size < channel->heap_capacity) {
            channel->priority_heap[channel->heap_size] = message;
            message_heap_bubble_up(channel->priority_heap, channel->heap_size);
            channel->heap_size++;
            result = true;
        }
    } else {
        // Add to circular buffer
        size_t tail = atomic_load(&channel->tail);
        channel->messages[tail] = message;
        atomic_store(&channel->tail, (tail + 1) % channel->capacity);
        result = true;
    }
    
    if (result) {
        atomic_fetch_add(&channel->size, 1);
        
        // Update statistics
        pthread_mutex_lock(&channel->stats_mutex);
        channel->stats.messages_sent++;
        uint64_t end_time = get_current_time_ns();
        double send_time = (double)(end_time - start_time);
        channel->stats.avg_send_time_ns = (channel->stats.avg_send_time_ns * 0.9) + (send_time * 0.1);
        
        size_t new_size = atomic_load(&channel->size);
        if (new_size > channel->stats.peak_queue_size) {
            channel->stats.peak_queue_size = new_size;
        }
        pthread_mutex_unlock(&channel->stats_mutex);
        
        // Update global statistics
        pthread_mutex_lock(&g_performance_mutex);
        g_global_performance.total_messages_sent++;
        pthread_mutex_unlock(&g_performance_mutex);
        
        // Signal waiting receivers
        pthread_cond_signal(&channel->not_empty);
    } else {
        channel_message_release(message); // Release if not added
    }
    
    pthread_mutex_unlock(&channel->send_mutex);
    atomic_fetch_sub(&channel->sender_count, 1);
    
    return result;
}

ChannelMessage* advanced_channel_receive(AdvancedChannel* channel) {
    return advanced_channel_receive_timeout(channel, channel->config.receive_timeout_ms);
}

ChannelMessage* advanced_channel_receive_timeout(AdvancedChannel* channel, uint64_t timeout_ms) {
    if (!channel) return NULL;
    
    // Increment receiver count
    atomic_fetch_add(&channel->receiver_count, 1);
    
    pthread_mutex_lock(&channel->recv_mutex);
    
    ChannelMessage* message = NULL;
    uint64_t start_time = get_current_time_ns();
    
    // Wait for message
    size_t current_size = atomic_load(&channel->size);
    while (current_size == 0 && !atomic_load(&channel->is_closed)) {
        if (timeout_ms > 0) {
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += timeout_ms / 1000;
            timeout.tv_nsec += (timeout_ms % 1000) * 1000000;
            if (timeout.tv_nsec >= 1000000000) {
                timeout.tv_sec++;
                timeout.tv_nsec -= 1000000000;
            }
            
            int wait_result = pthread_cond_timedwait(&channel->not_empty, &channel->recv_mutex, &timeout);
            if (wait_result == ETIMEDOUT) {
                pthread_mutex_unlock(&channel->recv_mutex);
                atomic_fetch_sub(&channel->receiver_count, 1);
                return NULL;
            }
        } else {
            pthread_cond_wait(&channel->not_empty, &channel->recv_mutex);
        }
        
        current_size = atomic_load(&channel->size);
    }
    
    if (current_size > 0) {
        if (channel->type == CHANNEL_TYPE_PRIORITY && channel->priority_heap && channel->heap_size > 0) {
            // Remove from priority heap
            message = channel->priority_heap[0];
            channel->priority_heap[0] = channel->priority_heap[channel->heap_size - 1];
            channel->heap_size--;
            if (channel->heap_size > 0) {
                message_heap_bubble_down(channel->priority_heap, channel->heap_size, 0);
            }
        } else {
            // Remove from circular buffer
            size_t head = atomic_load(&channel->head);
            message = channel->messages[head];
            channel->messages[head] = NULL;
            atomic_store(&channel->head, (head + 1) % channel->capacity);
        }
        
        if (message) {
            atomic_fetch_sub(&channel->size, 1);
            
            // Check message expiry
            if (channel->config.message_ttl_ms > 0 && message->metadata.expiry_time_ns > 0) {
                uint64_t now = get_current_time_ns();
                if (now > message->metadata.expiry_time_ns) {
                    // Message has expired, discard it
                    channel_message_release(message);
                    message = NULL;
                    
                    pthread_mutex_lock(&channel->stats_mutex);
                    channel->stats.messages_expired++;
                    pthread_mutex_unlock(&channel->stats_mutex);
                }
            }
            
            if (message) {
                // Update statistics
                pthread_mutex_lock(&channel->stats_mutex);
                channel->stats.messages_received++;
                uint64_t end_time = get_current_time_ns();
                double receive_time = (double)(end_time - start_time);
                channel->stats.avg_receive_time_ns = (channel->stats.avg_receive_time_ns * 0.9) + (receive_time * 0.1);
                
                // Calculate queue time
                double queue_time = (double)(end_time - message->metadata.timestamp_ns);
                channel->stats.avg_queue_time_ns = (channel->stats.avg_queue_time_ns * 0.9) + (queue_time * 0.1);
                pthread_mutex_unlock(&channel->stats_mutex);
                
                // Update global statistics
                pthread_mutex_lock(&g_performance_mutex);
                g_global_performance.total_messages_received++;
                pthread_mutex_unlock(&g_performance_mutex);
                
                // Signal waiting senders
                pthread_cond_signal(&channel->not_full);
            }
        }
    }
    
    pthread_mutex_unlock(&channel->recv_mutex);
    atomic_fetch_sub(&channel->receiver_count, 1);
    
    return message;
}

bool advanced_channel_close(AdvancedChannel* channel) {
    if (!channel) return false;
    
    atomic_store(&channel->is_closed, true);
    
    // Wake up all waiting threads
    pthread_cond_broadcast(&channel->not_empty);
    pthread_cond_broadcast(&channel->not_full);
    
    return true;
}

bool advanced_channel_is_closed(AdvancedChannel* channel) {
    if (!channel) return true;
    return atomic_load(&channel->is_closed);
}

size_t advanced_channel_size(AdvancedChannel* channel) {
    if (!channel) return 0;
    return atomic_load(&channel->size);
}

size_t advanced_channel_capacity(AdvancedChannel* channel) {
    if (!channel) return 0;
    return channel->capacity;
}

// =============================================================================
// Channel Selector Implementation
// =============================================================================

ChannelSelector* channel_selector_create(void) {
    ChannelSelector* selector = calloc(1, sizeof(ChannelSelector));
    if (!selector) return NULL;
    
    selector->selector_id = atomic_fetch_add(&g_next_channel_id, 1); // Reuse channel ID counter
    selector->cases = NULL;
    selector->case_count = 0;
    selector->selected_case = -1;
    selector->has_default = false;
    selector->timeout_ms = 0;
    selector->timed_out = false;
    selector->selection_complete = false;
    selector->selected_index = -1;
    
    if (pthread_mutex_init(&selector->select_mutex, NULL) != 0 ||
        pthread_cond_init(&selector->ready_cond, NULL) != 0) {
        free(selector);
        return NULL;
    }
    
    return selector;
}

void channel_selector_destroy(ChannelSelector* selector) {
    if (!selector) return;
    
    free(selector->cases);
    
    pthread_cond_destroy(&selector->ready_cond);
    pthread_mutex_destroy(&selector->select_mutex);
    
    free(selector);
}

bool channel_selector_add_send_case(ChannelSelector* selector, AdvancedChannel* channel, ChannelMessage* message) {
    if (!selector || !channel || !message) return false;
    
    // Resize cases array
    SelectCase* new_cases = realloc(selector->cases, (selector->case_count + 1) * sizeof(SelectCase));
    if (!new_cases) return false;
    
    selector->cases = new_cases;
    SelectCase* case_ptr = &selector->cases[selector->case_count];
    
    case_ptr->channel = channel;
    case_ptr->operation = SELECT_OP_SEND;
    case_ptr->message = message;
    case_ptr->result = NULL;
    case_ptr->is_ready = false;
    
    selector->case_count++;
    return true;
}

bool channel_selector_add_receive_case(ChannelSelector* selector, AdvancedChannel* channel, void** result) {
    if (!selector || !channel || !result) return false;
    
    // Resize cases array
    SelectCase* new_cases = realloc(selector->cases, (selector->case_count + 1) * sizeof(SelectCase));
    if (!new_cases) return false;
    
    selector->cases = new_cases;
    SelectCase* case_ptr = &selector->cases[selector->case_count];
    
    case_ptr->channel = channel;
    case_ptr->operation = SELECT_OP_RECEIVE;
    case_ptr->message = NULL;
    case_ptr->result = result;
    case_ptr->is_ready = false;
    
    selector->case_count++;
    return true;
}

void channel_selector_set_default(ChannelSelector* selector, void (*handler)(void*), void* context) {
    if (!selector) return;
    
    selector->has_default = true;
    selector->default_handler = handler;
    selector->default_context = context;
}

void channel_selector_set_timeout(ChannelSelector* selector, uint64_t timeout_ms) {
    if (!selector) return;
    selector->timeout_ms = timeout_ms;
}

int channel_selector_select(ChannelSelector* selector) {
    int selected_case = -1;
    bool success = channel_selector_select_timeout(selector, selector->timeout_ms, &selected_case);
    return success ? selected_case : -1;
}

bool channel_selector_select_timeout(ChannelSelector* selector, uint64_t timeout_ms, int* selected_case) {
    if (!selector || !selected_case) return false;
    
    pthread_mutex_lock(&selector->select_mutex);
    
    *selected_case = -1;
    selector->selection_complete = false;
    selector->timed_out = false;
    
    // Check if any cases are immediately ready
    for (int i = 0; i < selector->case_count; i++) {
        SelectCase* case_ptr = &selector->cases[i];
        
        if (case_ptr->operation == SELECT_OP_SEND) {
            // Check if channel has space
            if (atomic_load(&case_ptr->channel->size) < case_ptr->channel->capacity &&
                !atomic_load(&case_ptr->channel->is_closed)) {
                // Try to send immediately
                if (advanced_channel_send_timeout(case_ptr->channel, case_ptr->message, 0)) {
                    *selected_case = i;
                    selector->selection_complete = true;
                    pthread_mutex_unlock(&selector->select_mutex);
                    return true;
                }
            }
        } else if (case_ptr->operation == SELECT_OP_RECEIVE) {
            // Check if channel has messages
            if (atomic_load(&case_ptr->channel->size) > 0) {
                // Try to receive immediately
                ChannelMessage* msg = advanced_channel_receive_timeout(case_ptr->channel, 0);
                if (msg) {
                    *case_ptr->result = msg;
                    *selected_case = i;
                    selector->selection_complete = true;
                    pthread_mutex_unlock(&selector->select_mutex);
                    return true;
                }
            }
        }
    }
    
    // No cases ready immediately
    if (timeout_ms == 0) {
        // Non-blocking operation
        if (selector->has_default) {
            selector->default_handler(selector->default_context);
            pthread_mutex_unlock(&selector->select_mutex);
            return true;
        } else {
            pthread_mutex_unlock(&selector->select_mutex);
            return false;
        }
    }
    
    // Wait for a case to become ready or timeout
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += timeout_ms / 1000;
    timeout.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (timeout.tv_nsec >= 1000000000) {
        timeout.tv_sec++;
        timeout.tv_nsec -= 1000000000;
    }
    
    // Simplified implementation - in a full implementation, we would register
    // with each channel to be notified when they become ready
    int wait_result = pthread_cond_timedwait(&selector->ready_cond, &selector->select_mutex, &timeout);
    
    if (wait_result == ETIMEDOUT) {
        selector->timed_out = true;
        if (selector->has_default) {
            selector->default_handler(selector->default_context);
            pthread_mutex_unlock(&selector->select_mutex);
            return true;
        }
    }
    
    pthread_mutex_unlock(&selector->select_mutex);
    return false;
}

// =============================================================================
// Utility Functions Implementation
// =============================================================================

uint32_t channel_hash_string(const char* str) {
    if (!str) return 0;
    
    uint32_t hash = 5381;
    int c;
    
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    
    return hash;
}

uint32_t channel_hash_data(const void* data, size_t size) {
    if (!data || size == 0) return 0;
    
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t hash = 5381;
    
    for (size_t i = 0; i < size; i++) {
        hash = ((hash << 5) + hash) + bytes[i];
    }
    
    return hash;
}

bool channel_match_topic_filter(const char* topic, const char* filter) {
    if (!topic || !filter) return false;
    
    // Simple glob pattern matching
    return fnmatch(filter, topic, 0) == 0;
}

// =============================================================================
// Statistics and Monitoring
// =============================================================================

ChannelStats advanced_channel_get_stats(AdvancedChannel* channel) {
    ChannelStats stats = {0};
    if (!channel) return stats;
    
    pthread_mutex_lock(&channel->stats_mutex);
    stats = channel->stats;
    stats.current_queue_size = atomic_load(&channel->size);
    stats.queue_utilization = (double)stats.current_queue_size / channel->capacity;
    pthread_mutex_unlock(&channel->stats_mutex);
    
    return stats;
}

void advanced_channel_reset_stats(AdvancedChannel* channel) {
    if (!channel) return;
    
    pthread_mutex_lock(&channel->stats_mutex);
    memset(&channel->stats, 0, sizeof(ChannelStats));
    pthread_mutex_unlock(&channel->stats_mutex);
}

ChannelPerformanceReport get_channel_performance_report(void) {
    pthread_mutex_lock(&g_performance_mutex);
    ChannelPerformanceReport report = g_global_performance;
    pthread_mutex_unlock(&g_performance_mutex);
    
    // Calculate derived metrics
    if (report.total_messages_sent > 0 && report.total_messages_received > 0) {
        report.avg_message_latency_ms = 0.0; // Would be calculated from timing data
        report.channel_utilization = 0.0;    // Would be calculated from capacity data
        report.memory_usage_bytes = 0;       // Would be tracked separately
    }
    
    return report;
}

void print_channel_performance_report(const ChannelPerformanceReport* report) {
    if (!report) return;
    
    printf("=== Channel Performance Report ===\n");
    printf("Total channels: %lu\n", report->total_channels);
    printf("Messages sent: %lu\n", report->total_messages_sent);
    printf("Messages received: %lu\n", report->total_messages_received);
    printf("Average latency: %.2f ms\n", report->avg_message_latency_ms);
    printf("Channel utilization: %.1f%%\n", report->channel_utilization * 100.0);
    printf("Memory usage: %lu bytes\n", report->memory_usage_bytes);
}

// =============================================================================
// Debug Functions
// =============================================================================

void advanced_channel_dump_info(AdvancedChannel* channel) {
    if (!channel) return;
    
    printf("=== Advanced Channel Info ===\n");
    printf("ID: %lu\n", channel->channel_id);
    printf("Name: %s\n", channel->name ? channel->name : "unnamed");
    printf("Type: %d\n", channel->type);
    printf("Capacity: %zu\n", channel->capacity);
    printf("Current size: %zu\n", atomic_load(&channel->size));
    printf("Closed: %s\n", atomic_load(&channel->is_closed) ? "Yes" : "No");
    printf("Senders: %d\n", atomic_load(&channel->sender_count));
    printf("Receivers: %d\n", atomic_load(&channel->receiver_count));
    
    ChannelStats stats = advanced_channel_get_stats(channel);
    printf("Statistics:\n");
    printf("  Messages sent: %lu\n", stats.messages_sent);
    printf("  Messages received: %lu\n", stats.messages_received);
    printf("  Messages dropped: %lu\n", stats.messages_dropped);
    printf("  Peak queue size: %zu\n", stats.peak_queue_size);
    printf("  Queue utilization: %.1f%%\n", stats.queue_utilization * 100.0);
}

// =============================================================================
// Stub Implementations for Advanced Features
// =============================================================================

// These would be fully implemented in a complete system

// Distributed channel stubs
DistributedChannel* distributed_channel_create(const char* name, const DistributedChannelConfig* config) {
    (void)name; (void)config;
    return NULL; // Stub
}

void distributed_channel_destroy(DistributedChannel* channel) {
    (void)channel; // Stub
}

// Load balancer stubs
LoadBalancer* load_balancer_create(LoadBalanceStrategy strategy, ChannelNode* nodes, uint32_t node_count) {
    (void)strategy; (void)nodes; (void)node_count;
    return NULL; // Stub
}

void load_balancer_destroy(LoadBalancer* balancer) {
    (void)balancer; // Stub
}

LoadBalancerStats load_balancer_get_stats(LoadBalancer* balancer) {
    (void)balancer;
    LoadBalancerStats stats = {0};
    return stats;
}

// Broadcaster stubs
ChannelBroadcaster* channel_broadcaster_create(const char* name) {
    (void)name;
    return NULL; // Stub
}

void channel_broadcaster_destroy(ChannelBroadcaster* broadcaster) {
    (void)broadcaster; // Stub
}

// Pipeline stubs
ChannelPipeline* channel_pipeline_create(const char* name) {
    (void)name;
    return NULL; // Stub
}

void channel_pipeline_destroy(ChannelPipeline* pipeline) {
    (void)pipeline; // Stub
}

// Integration stubs
bool advanced_channel_register_with_actor(AdvancedChannel* channel, ActorRef* actor) {
    (void)channel; (void)actor;
    return false; // Stub
}

bool advanced_channel_register_with_scope(AdvancedChannel* channel, ConcurrencyScope* scope) {
    (void)channel; (void)scope;
    return false; // Stub
}

// Global accessor for thread-local selector
ChannelSelector* get_current_selector(void) {
    return g_current_selector;
}