#define _POSIX_C_SOURCE 200809L
#include "actor_system.h"
#include "error_hierarchies.h"
#include "error_transformation.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

// =============================================================================
// Global State and Utilities
// =============================================================================

static uint64_t g_next_actor_id = 1;
static uint64_t g_next_message_id = 1;
static uint64_t g_next_future_id = 1;
static uint64_t g_next_supervisor_id = 1;
static pthread_mutex_t g_id_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread-local storage for current actor context
static __thread ActorContext* g_current_context = NULL;

// Get current timestamp in milliseconds
static uint64_t get_current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// Generate unique ID with thread safety
static uint64_t generate_unique_id(uint64_t* counter) {
    pthread_mutex_lock(&g_id_mutex);
    uint64_t id = (*counter)++;
    pthread_mutex_unlock(&g_id_mutex);
    return id;
}

// Safe string duplication
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
// Actor Message Implementation
// =============================================================================

ActorMessage* actor_message_create(const char* handler_name, void* payload, size_t payload_size) {
    if (!handler_name) return NULL;
    
    ActorMessage* msg = calloc(1, sizeof(ActorMessage));
    if (!msg) return NULL;
    
    msg->message_id = generate_unique_id(&g_next_message_id);
    msg->type = ACTOR_MSG_USER;
    msg->priority = ACTOR_PRIORITY_NORMAL;
    
    msg->handler_name = safe_strdup(handler_name);
    
    if (payload && payload_size > 0) {
        msg->payload = malloc(payload_size);
        if (msg->payload) {
            memcpy(msg->payload, payload, payload_size);
            msg->payload_size = payload_size;
            msg->owns_payload = true;
        }
    }
    
    msg->timestamp = get_current_time_ms();
    msg->timeout_ms = 30000; // 30 second default timeout
    msg->max_retries = 3;
    
    return msg;
}

ActorMessage* actor_message_create_system(ActorSystemMessageType type) {
    ActorMessage* msg = calloc(1, sizeof(ActorMessage));
    if (!msg) return NULL;
    
    msg->message_id = generate_unique_id(&g_next_message_id);
    msg->type = ACTOR_MSG_SYSTEM;
    msg->priority = ACTOR_PRIORITY_SYSTEM;
    msg->system_data.system_type = type;
    msg->timestamp = get_current_time_ms();
    
    return msg;
}

void actor_message_destroy(ActorMessage* message) {
    if (!message) return;
    
    if (message->owns_payload && message->payload) {
        if (message->payload_destructor) {
            message->payload_destructor(message->payload);
        } else {
            free(message->payload);
        }
    }
    
    free((void*)message->handler_name);
    
    if (message->type == ACTOR_MSG_ERROR && message->system_data.error_info.error) {
        structured_error_free(message->system_data.error_info.error);
        free((void*)message->system_data.error_info.error_context);
    }
    
    free(message);
}

// =============================================================================
// Actor Mailbox Implementation
// =============================================================================

ActorMailbox* actor_mailbox_create(ActorMailboxConfig* config) {
    ActorMailbox* mailbox = calloc(1, sizeof(ActorMailbox));
    if (!mailbox) return NULL;
    
    // Set default configuration
    if (config) {
        mailbox->config = *config;
    } else {
        mailbox->config.max_capacity = 1000;
        mailbox->config.drop_on_overflow = false;
        mailbox->config.priority_queue = false;
        mailbox->config.message_timeout_ms = 30000;
        mailbox->config.enable_metrics = true;
    }
    
    // Allocate message storage
    mailbox->capacity = mailbox->config.max_capacity;
    mailbox->messages = calloc(mailbox->capacity, sizeof(ActorMessage*));
    if (!mailbox->messages) {
        free(mailbox);
        return NULL;
    }
    
    // Initialize priority queue if enabled
    if (mailbox->config.priority_queue) {
        mailbox->priority_heap = calloc(mailbox->capacity, sizeof(ActorMessage*));
        if (!mailbox->priority_heap) {
            free(mailbox->messages);
            free(mailbox);
            return NULL;
        }
    }
    
    // Initialize synchronization primitives
    if (pthread_mutex_init(&mailbox->mutex, NULL) != 0 ||
        pthread_cond_init(&mailbox->not_empty, NULL) != 0 ||
        pthread_cond_init(&mailbox->not_full, NULL) != 0) {
        free(mailbox->priority_heap);
        free(mailbox->messages);
        free(mailbox);
        return NULL;
    }
    
    return mailbox;
}

void actor_mailbox_destroy(ActorMailbox* mailbox) {
    if (!mailbox) return;
    
    pthread_mutex_lock(&mailbox->mutex);
    
    // Clean up remaining messages
    for (size_t i = 0; i < mailbox->size; i++) {
        size_t index = (mailbox->head + i) % mailbox->capacity;
        if (mailbox->messages[index]) {
            actor_message_destroy(mailbox->messages[index]);
        }
    }
    
    // Clean up priority heap
    for (size_t i = 0; i < mailbox->heap_size; i++) {
        if (mailbox->priority_heap[i]) {
            actor_message_destroy(mailbox->priority_heap[i]);
        }
    }
    
    pthread_mutex_unlock(&mailbox->mutex);
    
    // Destroy synchronization primitives
    pthread_cond_destroy(&mailbox->not_empty);
    pthread_cond_destroy(&mailbox->not_full);
    pthread_mutex_destroy(&mailbox->mutex);
    
    free(mailbox->priority_heap);
    free(mailbox->messages);
    free(mailbox);
}

// Priority heap operations for mailbox
static void mailbox_heap_swap(ActorMessage** heap, size_t i, size_t j) {
    ActorMessage* temp = heap[i];
    heap[i] = heap[j];
    heap[j] = temp;
}

static void mailbox_heap_bubble_up(ActorMessage** heap, size_t index) {
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (heap[index]->priority <= heap[parent]->priority) break;
        mailbox_heap_swap(heap, index, parent);
        index = parent;
    }
}

static void mailbox_heap_bubble_down(ActorMessage** heap, size_t heap_size, size_t index) {
    while (true) {
        size_t largest = index;
        size_t left = 2 * index + 1;
        size_t right = 2 * index + 2;
        
        if (left < heap_size && heap[left]->priority > heap[largest]->priority) {
            largest = left;
        }
        
        if (right < heap_size && heap[right]->priority > heap[largest]->priority) {
            largest = right;
        }
        
        if (largest == index) break;
        
        mailbox_heap_swap(heap, index, largest);
        index = largest;
    }
}

bool actor_mailbox_send(ActorMailbox* mailbox, ActorMessage* message) {
    if (!mailbox || !message) return false;
    
    pthread_mutex_lock(&mailbox->mutex);
    
    // Check if mailbox is closed
    if (mailbox->is_closed) {
        pthread_mutex_unlock(&mailbox->mutex);
        return false;
    }
    
    // Check capacity
    if (mailbox->size >= mailbox->capacity) {
        if (mailbox->config.drop_on_overflow) {
            mailbox->stats.messages_dropped++;
            pthread_mutex_unlock(&mailbox->mutex);
            actor_message_destroy(message);
            return false;
        } else {
            // Wait for space
            while (mailbox->size >= mailbox->capacity && !mailbox->is_closed) {
                pthread_cond_wait(&mailbox->not_full, &mailbox->mutex);
            }
            
            if (mailbox->is_closed) {
                pthread_mutex_unlock(&mailbox->mutex);
                return false;
            }
        }
    }
    
    // Add message to mailbox
    if (mailbox->config.priority_queue) {
        // Add to priority heap
        mailbox->priority_heap[mailbox->heap_size] = message;
        mailbox_heap_bubble_up(mailbox->priority_heap, mailbox->heap_size);
        mailbox->heap_size++;
    } else {
        // Add to circular buffer
        mailbox->messages[mailbox->tail] = message;
        mailbox->tail = (mailbox->tail + 1) % mailbox->capacity;
    }
    
    mailbox->size++;
    mailbox->stats.messages_received++;
    
    // Signal waiting receivers
    pthread_cond_signal(&mailbox->not_empty);
    pthread_mutex_unlock(&mailbox->mutex);
    
    return true;
}

ActorMessage* actor_mailbox_receive(ActorMailbox* mailbox) {
    if (!mailbox) return NULL;
    
    pthread_mutex_lock(&mailbox->mutex);
    
    // Wait for message
    while (mailbox->size == 0 && !mailbox->is_closed) {
        pthread_cond_wait(&mailbox->not_empty, &mailbox->mutex);
    }
    
    if (mailbox->size == 0) {
        pthread_mutex_unlock(&mailbox->mutex);
        return NULL; // Mailbox closed
    }
    
    ActorMessage* message = NULL;
    
    if (mailbox->config.priority_queue) {
        // Remove from priority heap
        message = mailbox->priority_heap[0];
        mailbox->priority_heap[0] = mailbox->priority_heap[mailbox->heap_size - 1];
        mailbox->heap_size--;
        if (mailbox->heap_size > 0) {
            mailbox_heap_bubble_down(mailbox->priority_heap, mailbox->heap_size, 0);
        }
    } else {
        // Remove from circular buffer
        message = mailbox->messages[mailbox->head];
        mailbox->messages[mailbox->head] = NULL;
        mailbox->head = (mailbox->head + 1) % mailbox->capacity;
    }
    
    mailbox->size--;
    
    // Update peak size
    if (mailbox->size > mailbox->stats.peak_size) {
        mailbox->stats.peak_size = mailbox->size;
    }
    
    // Signal waiting senders
    pthread_cond_signal(&mailbox->not_full);
    pthread_mutex_unlock(&mailbox->mutex);
    
    return message;
}

ActorMessage* actor_mailbox_try_receive(ActorMailbox* mailbox) {
    if (!mailbox) return NULL;
    
    pthread_mutex_lock(&mailbox->mutex);
    
    if (mailbox->size == 0) {
        pthread_mutex_unlock(&mailbox->mutex);
        return NULL;
    }
    
    ActorMessage* message = NULL;
    
    if (mailbox->config.priority_queue) {
        // Remove from priority heap
        message = mailbox->priority_heap[0];
        mailbox->priority_heap[0] = mailbox->priority_heap[mailbox->heap_size - 1];
        mailbox->heap_size--;
        if (mailbox->heap_size > 0) {
            mailbox_heap_bubble_down(mailbox->priority_heap, mailbox->heap_size, 0);
        }
    } else {
        // Remove from circular buffer
        message = mailbox->messages[mailbox->head];
        mailbox->messages[mailbox->head] = NULL;
        mailbox->head = (mailbox->head + 1) % mailbox->capacity;
    }
    
    mailbox->size--;
    pthread_cond_signal(&mailbox->not_full);
    pthread_mutex_unlock(&mailbox->mutex);
    
    return message;
}

void actor_mailbox_close(ActorMailbox* mailbox) {
    if (!mailbox) return;
    
    pthread_mutex_lock(&mailbox->mutex);
    mailbox->is_closed = true;
    pthread_cond_broadcast(&mailbox->not_empty);
    pthread_cond_broadcast(&mailbox->not_full);
    pthread_mutex_unlock(&mailbox->mutex);
}

// =============================================================================
// Actor Future Implementation
// =============================================================================

ActorFuture* actor_future_create(void) {
    ActorFuture* future = calloc(1, sizeof(ActorFuture));
    if (!future) return NULL;
    
    future->future_id = generate_unique_id(&g_next_future_id);
    future->state = ACTOR_FUTURE_PENDING;
    future->created_at = get_current_time_ms();
    future->timeout_ms = 30000; // 30 second default
    future->ref_count = 1;
    
    if (pthread_mutex_init(&future->mutex, NULL) != 0 ||
        pthread_cond_init(&future->completed, NULL) != 0 ||
        pthread_mutex_init(&future->ref_mutex, NULL) != 0) {
        free(future);
        return NULL;
    }
    
    return future;
}

void actor_future_release(ActorFuture* future) {
    if (!future) return;
    
    pthread_mutex_lock(&future->ref_mutex);
    future->ref_count--;
    bool should_free = (future->ref_count <= 0);
    pthread_mutex_unlock(&future->ref_mutex);
    
    if (should_free) {
        // Clean up result
        if (future->result && future->result_destructor) {
            future->result_destructor(future->result);
        }
        
        // Clean up error
        if (future->error) {
            structured_error_free(future->error);
        }
        
        // Clean up chained futures
        for (int i = 0; i < future->chained_count; i++) {
            actor_future_release(future->chained_futures[i]);
        }
        free(future->chained_futures);
        
        // Destroy synchronization primitives
        pthread_cond_destroy(&future->completed);
        pthread_mutex_destroy(&future->mutex);
        pthread_mutex_destroy(&future->ref_mutex);
        
        free(future);
    }
}

static void actor_future_complete_internal(ActorFuture* future, void* result, size_t result_size) {
    pthread_mutex_lock(&future->mutex);
    
    if (future->state != ACTOR_FUTURE_PENDING) {
        pthread_mutex_unlock(&future->mutex);
        return;
    }
    
    future->state = ACTOR_FUTURE_COMPLETED;
    future->completed_at = get_current_time_ms();
    
    if (result && result_size > 0) {
        future->result = malloc(result_size);
        if (future->result) {
            memcpy(future->result, result, result_size);
            future->result_size = result_size;
        }
    }
    
    // Notify waiters
    pthread_cond_broadcast(&future->completed);
    pthread_mutex_unlock(&future->mutex);
    
    // Call completion callback
    if (future->on_complete) {
        future->on_complete(future, future->callback_context);
    }
    
    // Complete chained futures
    for (int i = 0; i < future->chained_count; i++) {
        if (future->chained_futures[i]->on_complete) {
            future->chained_futures[i]->on_complete(future, future->callback_context);
        }
    }
}

static void actor_future_fail_internal(ActorFuture* future, StructuredError* error) {
    pthread_mutex_lock(&future->mutex);
    
    if (future->state != ACTOR_FUTURE_PENDING) {
        pthread_mutex_unlock(&future->mutex);
        return;
    }
    
    future->state = ACTOR_FUTURE_FAILED;
    future->completed_at = get_current_time_ms();
    future->error = error;
    
    // Notify waiters
    pthread_cond_broadcast(&future->completed);
    pthread_mutex_unlock(&future->mutex);
    
    // Call error callback
    if (future->on_error) {
        future->on_error(future, error, future->callback_context);
    }
}

void* actor_future_await(ActorFuture* future) {
    if (!future) return NULL;
    
    pthread_mutex_lock(&future->mutex);
    
    // Wait for completion
    while (future->state == ACTOR_FUTURE_PENDING) {
        pthread_cond_wait(&future->completed, &future->mutex);
    }
    
    void* result = NULL;
    if (future->state == ACTOR_FUTURE_COMPLETED) {
        result = future->result;
    }
    
    pthread_mutex_unlock(&future->mutex);
    return result;
}

void* actor_future_await_timeout(ActorFuture* future, uint64_t timeout_ms) {
    if (!future) return NULL;
    
    pthread_mutex_lock(&future->mutex);
    
    if (future->state != ACTOR_FUTURE_PENDING) {
        void* result = (future->state == ACTOR_FUTURE_COMPLETED) ? future->result : NULL;
        pthread_mutex_unlock(&future->mutex);
        return result;
    }
    
    // Calculate timeout
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += timeout_ms / 1000;
    timeout.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (timeout.tv_nsec >= 1000000000) {
        timeout.tv_sec++;
        timeout.tv_nsec -= 1000000000;
    }
    
    // Wait with timeout
    int result = pthread_cond_timedwait(&future->completed, &future->mutex, &timeout);
    
    void* return_value = NULL;
    if (result == 0 && future->state == ACTOR_FUTURE_COMPLETED) {
        return_value = future->result;
    } else if (result == ETIMEDOUT) {
        future->state = ACTOR_FUTURE_TIMEOUT;
    }
    
    pthread_mutex_unlock(&future->mutex);
    return return_value;
}

bool actor_future_is_completed(ActorFuture* future) {
    if (!future) return false;
    
    pthread_mutex_lock(&future->mutex);
    bool completed = (future->state == ACTOR_FUTURE_COMPLETED);
    pthread_mutex_unlock(&future->mutex);
    
    return completed;
}

bool actor_future_is_failed(ActorFuture* future) {
    if (!future) return false;
    
    pthread_mutex_lock(&future->mutex);
    bool failed = (future->state == ACTOR_FUTURE_FAILED);
    pthread_mutex_unlock(&future->mutex);
    
    return failed;
}

StructuredError* actor_future_get_error(ActorFuture* future) {
    if (!future) return NULL;
    
    pthread_mutex_lock(&future->mutex);
    StructuredError* error = future->error;
    pthread_mutex_unlock(&future->mutex);
    
    return error;
}

void actor_future_set_callback(ActorFuture* future, ActorFutureCallback callback, void* context) {
    if (!future || !callback) return;
    
    pthread_mutex_lock(&future->mutex);
    future->on_complete = callback;
    future->callback_context = context;
    
    // If already completed, call callback immediately
    if (future->state == ACTOR_FUTURE_COMPLETED) {
        pthread_mutex_unlock(&future->mutex);
        callback(future, context);
    } else {
        pthread_mutex_unlock(&future->mutex);
    }
}

// =============================================================================
// Actor Behavior Implementation
// =============================================================================

ActorBehavior* actor_behavior_create(const char* name) {
    if (!name) return NULL;
    
    ActorBehavior* behavior = calloc(1, sizeof(ActorBehavior));
    if (!behavior) return NULL;
    
    behavior->behavior_name = safe_strdup(name);
    behavior->mailbox_capacity = 1000;
    behavior->message_timeout_ms = 30000;
    behavior->enable_supervision = true;
    
    return behavior;
}

void actor_behavior_destroy(ActorBehavior* behavior) {
    if (!behavior) return;
    
    free((void*)behavior->behavior_name);
    
    // Free handlers
    for (int i = 0; i < behavior->handler_count; i++) {
        free((void*)behavior->handlers[i].message_name);
    }
    free(behavior->handlers);
    
    free(behavior);
}

void actor_behavior_add_handler(ActorBehavior* behavior, const char* message_name, 
                               ActorHandler handler) {
    if (!behavior || !message_name || !handler) return;
    
    // Resize handlers array if needed  
    int current_capacity = behavior->handlers ? behavior->handler_count : 0;
    if (behavior->handler_count >= current_capacity) {
        int new_capacity = current_capacity == 0 ? 4 : current_capacity * 2;
        ActorHandlerEntry* new_handlers = realloc(behavior->handlers, 
                                                 new_capacity * sizeof(ActorHandlerEntry));
        if (!new_handlers) return;
        
        behavior->handlers = new_handlers;
    }
    
    // Add new handler
    ActorHandlerEntry* entry = &behavior->handlers[behavior->handler_count];
    entry->message_name = safe_strdup(message_name);
    entry->handler = handler;
    entry->is_async = true;
    entry->timeout_ms = behavior->message_timeout_ms;
    entry->priority = ACTOR_PRIORITY_NORMAL;
    
    behavior->handler_count++;
}

void actor_behavior_set_lifecycle_hooks(ActorBehavior* behavior,
                                       ActorHandler on_start,
                                       ActorHandler on_stop,
                                       ActorHandler on_error,
                                       ActorHandler on_restart) {
    if (!behavior) return;
    
    behavior->on_start = on_start;
    behavior->on_stop = on_stop;
    behavior->on_error = on_error;
    behavior->on_restart = on_restart;
}

// =============================================================================
// Actor Core Implementation
// =============================================================================

static ActorHandlerEntry* actor_find_handler(Actor* actor, const char* message_name) {
    if (!actor || !actor->behavior || !message_name) return NULL;
    
    for (int i = 0; i < actor->behavior->handler_count; i++) {
        if (strcmp(actor->behavior->handlers[i].message_name, message_name) == 0) {
            return &actor->behavior->handlers[i];
        }
    }
    
    return NULL;
}

static void* actor_main_loop(void* arg) {
    Actor* actor = (Actor*)arg;
    if (!actor) return NULL;
    
    ActorContext context = {0};
    context.self = actor;
    context.self_ref = actor->self_ref;
    context.start_time = get_current_time_ms();
    context.message_arena = goo_arena_new(4096, "message_arena");
    context.state_arena = goo_arena_new(8192, "state_arena");
    
    actor->context = &context;
    g_current_context = &context;
    
    // Call start hook
    if (actor->behavior->on_start) {
        ActorMessage* start_msg = actor_message_create_system(ACTOR_SYSTEM_START);
        ActorFuture* future = actor->behavior->on_start(actor, start_msg, &context);
        if (future) {
            actor_future_release(future);
        }
        actor_message_destroy(start_msg);
    }
    
    // Set actor state to running
    pthread_mutex_lock(&actor->state_mutex);
    actor->state = ACTOR_STATE_RUNNING;
    pthread_cond_broadcast(&actor->state_changed);
    pthread_mutex_unlock(&actor->state_mutex);
    
    // Main message processing loop
    while (actor->state == ACTOR_STATE_RUNNING) {
        // Receive message
        ActorMessage* message = actor_mailbox_receive(actor->mailbox);
        if (!message) {
            break; // Mailbox closed
        }
        
        context.current_message = message;
        uint64_t start_time = get_current_time_ms();
        
        // Handle system messages
        if (message->type == ACTOR_MSG_SYSTEM) {
            switch (message->system_data.system_type) {
                case ACTOR_SYSTEM_STOP:
                    pthread_mutex_lock(&actor->state_mutex);
                    actor->state = ACTOR_STATE_STOPPING;
                    pthread_mutex_unlock(&actor->state_mutex);
                    actor_message_destroy(message);
                    continue;
                    
                case ACTOR_SYSTEM_KILL:
                    pthread_mutex_lock(&actor->state_mutex);
                    actor->state = ACTOR_STATE_STOPPED;
                    pthread_mutex_unlock(&actor->state_mutex);
                    actor_message_destroy(message);
                    goto cleanup;
                    
                case ACTOR_SYSTEM_SUSPEND:
                    pthread_mutex_lock(&actor->state_mutex);
                    actor->state = ACTOR_STATE_SUSPENDED;
                    while (actor->state == ACTOR_STATE_SUSPENDED) {
                        pthread_cond_wait(&actor->state_changed, &actor->state_mutex);
                    }
                    pthread_mutex_unlock(&actor->state_mutex);
                    break;
                    
                case ACTOR_SYSTEM_RESUME:
                    pthread_mutex_lock(&actor->state_mutex);
                    if (actor->state == ACTOR_STATE_SUSPENDED) {
                        actor->state = ACTOR_STATE_RUNNING;
                        pthread_cond_broadcast(&actor->state_changed);
                    }
                    pthread_mutex_unlock(&actor->state_mutex);
                    break;
                    
                default:
                    break;
            }
        } else {
            // Handle user messages
            ActorHandlerEntry* handler = actor_find_handler(actor, message->handler_name);
            if (handler) {
                // Create future for message
                ActorFuture* future = actor_future_create();
                if (future) {
                    future->message_id = message->message_id;
                    context.current_future = future;
                    
                    // Call handler
                    ActorFuture* result_future = handler->handler(actor, message, &context);
                    
                    // Complete the future
                    if (result_future) {
                        // Handler returned a future, chain it
                        if (actor_future_is_completed(result_future)) {
                            void* result = actor_future_await(result_future);
                            actor_future_complete_internal(future, result, 0);
                        }
                        actor_future_release(result_future);
                    } else {
                        // Handler completed synchronously
                        actor_future_complete_internal(future, NULL, 0);
                    }
                    
                    actor_future_release(future);
                }
            }
        }
        
        // Update statistics
        uint64_t end_time = get_current_time_ms();
        double message_time = (double)(end_time - start_time);
        actor->stats.messages_processed++;
        
        // Update rolling average
        if (actor->stats.messages_processed == 1) {
            actor->stats.avg_message_time_ms = message_time;
        } else {
            actor->stats.avg_message_time_ms = actor->stats.avg_message_time_ms * 0.9 + message_time * 0.1;
        }
        
        // Clean up message
        actor_message_destroy(message);
        context.current_message = NULL;
        context.current_future = NULL;
        
        // Reset message arena
        goo_arena_reset(context.message_arena);
    }
    
cleanup:
    // Call stop hook
    if (actor->behavior->on_stop) {
        ActorMessage* stop_msg = actor_message_create_system(ACTOR_SYSTEM_STOP);
        ActorFuture* future = actor->behavior->on_stop(actor, stop_msg, &context);
        if (future) {
            actor_future_release(future);
        }
        actor_message_destroy(stop_msg);
    }
    
    // Clean up context
    goo_arena_free(context.message_arena);
    goo_arena_free(context.state_arena);
    
    // Set final state
    pthread_mutex_lock(&actor->state_mutex);
    actor->state = ACTOR_STATE_STOPPED;
    pthread_cond_broadcast(&actor->state_changed);
    pthread_mutex_unlock(&actor->state_mutex);
    
    g_current_context = NULL;
    return NULL;
}

Actor* actor_create(ActorSystem* system, const char* name, ActorBehavior* behavior) {
    if (!system || !name || !behavior) return NULL;
    
    Actor* actor = calloc(1, sizeof(Actor));
    if (!actor) return NULL;
    
    actor->actor_id = generate_unique_id(&g_next_actor_id);
    actor->actor_name = safe_strdup(name);
    actor->actor_type = safe_strdup(behavior->behavior_name);
    actor->state = ACTOR_STATE_CREATED;
    actor->behavior = behavior;
    
    // Create mailbox
    ActorMailboxConfig mailbox_config = {
        .max_capacity = behavior->mailbox_capacity,
        .drop_on_overflow = false,
        .priority_queue = true,
        .message_timeout_ms = behavior->message_timeout_ms,
        .enable_metrics = true
    };
    actor->mailbox = actor_mailbox_create(&mailbox_config);
    if (!actor->mailbox) {
        free((void*)actor->actor_name);
        free((void*)actor->actor_type);
        free(actor);
        return NULL;
    }
    
    // Initialize synchronization
    if (pthread_mutex_init(&actor->state_mutex, NULL) != 0 ||
        pthread_cond_init(&actor->state_changed, NULL) != 0) {
        actor_mailbox_destroy(actor->mailbox);
        free((void*)actor->actor_name);
        free((void*)actor->actor_type);
        free(actor);
        return NULL;
    }
    
    // Create actor arena
    actor->actor_arena = goo_arena_new(16384, actor->actor_name);
    
    // Initialize user state if behavior provides it
    if (behavior->create_state) {
        actor->user_state = behavior->create_state();
    }
    
    return actor;
}

void actor_destroy(Actor* actor) {
    if (!actor) return;
    
    // Stop actor if running
    if (actor->state == ACTOR_STATE_RUNNING) {
        actor_send_system_message(actor->self_ref, ACTOR_SYSTEM_STOP);
        
        // Wait for stop
        pthread_mutex_lock(&actor->state_mutex);
        while (actor->state != ACTOR_STATE_STOPPED) {
            pthread_cond_wait(&actor->state_changed, &actor->state_mutex);
        }
        pthread_mutex_unlock(&actor->state_mutex);
    }
    
    // Join thread if we own it
    if (actor->owns_thread && actor->thread) {
        pthread_join(actor->thread, NULL);
    }
    
    // Destroy user state
    if (actor->user_state && actor->behavior->destroy_state) {
        actor->behavior->destroy_state(actor->user_state);
    }
    
    // Clean up mailbox
    actor_mailbox_destroy(actor->mailbox);
    
    // Clean up arenas
    goo_arena_free(actor->actor_arena);
    
    // Clean up synchronization
    pthread_cond_destroy(&actor->state_changed);
    pthread_mutex_destroy(&actor->state_mutex);
    
    // Clean up children array
    free(actor->children);
    
    // Clean up strings
    free((void*)actor->actor_name);
    free((void*)actor->actor_type);
    
    free(actor);
}

// Start actor in its own thread
static bool actor_start_thread(Actor* actor) {
    if (!actor) return false;
    
    pthread_mutex_lock(&actor->state_mutex);
    if (actor->state != ACTOR_STATE_CREATED) {
        pthread_mutex_unlock(&actor->state_mutex);
        return false;
    }
    
    actor->state = ACTOR_STATE_STARTING;
    pthread_mutex_unlock(&actor->state_mutex);
    
    if (pthread_create(&actor->thread, NULL, actor_main_loop, actor) != 0) {
        pthread_mutex_lock(&actor->state_mutex);
        actor->state = ACTOR_STATE_FAILED;
        pthread_mutex_unlock(&actor->state_mutex);
        return false;
    }
    
    actor->owns_thread = true;
    return true;
}

// =============================================================================
// Actor Reference Implementation
// =============================================================================

ActorRef* actor_ref_create(ActorSystem* system, uint64_t actor_id) {
    if (!system) return NULL;
    
    ActorRef* ref = calloc(1, sizeof(ActorRef));
    if (!ref) return NULL;
    
    ref->actor_id = actor_id;
    ref->system = system;
    ref->is_valid = true;
    ref->version = 1;
    ref->ref_count = 1;
    
    if (pthread_mutex_init(&ref->ref_mutex, NULL) != 0) {
        free(ref);
        return NULL;
    }
    
    return ref;
}

void actor_ref_retain(ActorRef* ref) {
    if (!ref) return;
    
    pthread_mutex_lock(&ref->ref_mutex);
    ref->ref_count++;
    pthread_mutex_unlock(&ref->ref_mutex);
}

void actor_ref_release(ActorRef* ref) {
    if (!ref) return;
    
    pthread_mutex_lock(&ref->ref_mutex);
    ref->ref_count--;
    bool should_free = (ref->ref_count <= 0);
    pthread_mutex_unlock(&ref->ref_mutex);
    
    if (should_free) {
        pthread_mutex_destroy(&ref->ref_mutex);
        free(ref);
    }
}

bool actor_ref_is_valid(ActorRef* ref) {
    if (!ref) return false;
    
    // For now, just check the valid flag
    // In a full implementation, we'd validate against the actor system
    return ref->is_valid;
}

// =============================================================================
// Placeholder implementations for remaining functions
// =============================================================================

// These would be fully implemented in a complete system
ActorSystem* actor_system_create(const char* name, ActorSystemConfig* config) {
    // Placeholder - would create full actor system
    return NULL;
}

void actor_system_destroy(ActorSystem* system) {
    // Placeholder - would clean up actor system
}

ActorRef* actor_spawn(ActorSystem* system, const char* name, ActorBehavior* behavior) {
    if (!system || !name || !behavior) return NULL;
    
    Actor* actor = actor_create(system, name, behavior);
    if (!actor) return NULL;
    
    // Create reference
    ActorRef* ref = actor_ref_create(system, actor->actor_id);
    if (!ref) {
        actor_destroy(actor);
        return NULL;
    }
    
    actor->self_ref = ref;
    actor_ref_retain(ref);
    
    // Start actor
    if (!actor_start_thread(actor)) {
        actor_ref_release(ref);
        actor_destroy(actor);
        return NULL;
    }
    
    return ref;
}

ActorFuture* actor_send(ActorRef* to, const char* handler, void* payload, size_t payload_size) {
    // Placeholder - would implement message sending
    return NULL;
}

void actor_send_system_message(ActorRef* to, ActorSystemMessageType msg_type) {
    // Placeholder - would send system message
}

// =============================================================================
// Statistics and Monitoring
// =============================================================================

ActorMailboxStats actor_mailbox_get_stats(ActorMailbox* mailbox) {
    ActorMailboxStats stats = {0};
    if (mailbox) {
        pthread_mutex_lock(&mailbox->mutex);
        stats = mailbox->stats;
        stats.current_size = mailbox->size;
        pthread_mutex_unlock(&mailbox->mutex);
    }
    return stats;
}