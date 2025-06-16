#include "../../include/reactive_programming.h"
#include "../../include/errors/error.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

// Global reactive system registry for cleanup tracking
static ReactiveSystemExtension** g_reactive_systems = NULL;
static size_t g_reactive_system_count = 0;
static size_t g_reactive_system_capacity = 0;
static pthread_mutex_t g_reactive_system_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

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

// Reactive System Extension Implementation

ReactiveSystemExtension* reactive_system_create(const char* name, ActorSystem* actor_system) {
    if (!actor_system) return NULL;
    
    ReactiveSystemExtension* reactive_system = calloc(1, sizeof(ReactiveSystemExtension));
    if (!reactive_system) return NULL;
    
    reactive_system->id = generate_id();
    strncpy(reactive_system->name, name ? name : "reactive_system", sizeof(reactive_system->name) - 1);
    reactive_system->actor_system = actor_system;
    
    // Initialize event stream registry
    reactive_system->system_stream_capacity = 32;
    reactive_system->system_streams = calloc(reactive_system->system_stream_capacity, sizeof(EventStream*));
    if (!reactive_system->system_streams) {
        free(reactive_system);
        return NULL;
    }
    
    // Initialize reactive component registry
    reactive_system->component_capacity = 32;
    reactive_system->components = calloc(reactive_system->component_capacity, sizeof(ReactiveComponent*));
    if (!reactive_system->components) {
        free(reactive_system->system_streams);
        free(reactive_system);
        return NULL;
    }
    
    // Initialize synchronization
    if (pthread_mutex_init(&reactive_system->reactive_mutex, NULL) != 0) {
        free(reactive_system->components);
        free(reactive_system->system_streams);
        free(reactive_system);
        return NULL;
    }
    
    // Register system globally
    pthread_mutex_lock(&g_reactive_system_registry_mutex);
    if (g_reactive_system_count >= g_reactive_system_capacity) {
        size_t new_capacity = g_reactive_system_capacity == 0 ? 4 : g_reactive_system_capacity * 2;
        ReactiveSystemExtension** new_systems = realloc(g_reactive_systems, new_capacity * sizeof(ReactiveSystemExtension*));
        if (new_systems) {
            g_reactive_systems = new_systems;
            g_reactive_system_capacity = new_capacity;
        }
    }
    
    if (g_reactive_system_count < g_reactive_system_capacity) {
        g_reactive_systems[g_reactive_system_count++] = reactive_system;
    }
    pthread_mutex_unlock(&g_reactive_system_registry_mutex);
    
    printf("🎭 Created reactive system extension: %s (ID: %llu)\n", reactive_system->name, reactive_system->id);
    return reactive_system;
}

void reactive_system_destroy(ReactiveSystemExtension* reactive_system) {
    if (!reactive_system) return;
    
    printf("🗑️ Destroying reactive system extension: %s (ID: %llu)\n", reactive_system->name, reactive_system->id);
    
    // Shutdown system if still running
    if (reactive_system->processor_running) {
        reactive_system_shutdown(reactive_system);
    }
    
    pthread_mutex_lock(&reactive_system->reactive_mutex);
    
    // Destroy all event streams
    for (size_t i = 0; i < reactive_system->system_stream_count; i++) {
        if (reactive_system->system_streams[i]) {
            event_stream_destroy(reactive_system->system_streams[i]);
        }
    }
    free(reactive_system->system_streams);
    
    // Destroy all reactive components
    for (size_t i = 0; i < reactive_system->component_count; i++) {
        if (reactive_system->components[i]) {
            reactive_component_destroy(reactive_system->components[i]);
        }
    }
    free(reactive_system->components);
    
    pthread_mutex_unlock(&reactive_system->reactive_mutex);
    
    // Cleanup synchronization
    pthread_mutex_destroy(&reactive_system->reactive_mutex);
    
    // Remove from global registry
    pthread_mutex_lock(&g_reactive_system_registry_mutex);
    for (size_t i = 0; i < g_reactive_system_count; i++) {
        if (g_reactive_systems[i] == reactive_system) {
            memmove(&g_reactive_systems[i], &g_reactive_systems[i + 1], 
                   (g_reactive_system_count - i - 1) * sizeof(ReactiveSystemExtension*));
            g_reactive_system_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_reactive_system_registry_mutex);
    
    free(reactive_system);
}

Result_void_ptr actor_system_start(ActorSystem* system) {
    if (!system) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid actor system"));
    }
    
    pthread_mutex_lock(&system->system_mutex);
    
    if (system->is_running) {
        pthread_mutex_unlock(&system->system_mutex);
        return OK_PTR(system);
    }
    
    printf("🚀 Starting actor system: %s\n", system->name);
    
    // Create resource scope for the system
    system->resource_scope = async_resource_scope_create("actor_system_scope", system->async_context);
    if (!system->resource_scope) {
        pthread_mutex_unlock(&system->system_mutex);
        return ERR_PTR(error_create(ERROR_INITIALIZATION_FAILED, "Failed to create resource scope"));
    }
    
    system->is_running = true;
    system->started_time_ns = get_current_time_ns();
    
    // Start background threads
    system->dispatcher_running = true;
    system->processor_running = true;
    system->supervisor_running = true;
    
    // TODO: Create and start background threads for message dispatching, event processing, and supervision
    
    pthread_mutex_unlock(&system->system_mutex);
    
    printf("✅ Actor system started: %s\n", system->name);
    return OK_PTR(system);
}

Result_void_ptr actor_system_shutdown(ActorSystem* system) {
    if (!system) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid actor system"));
    }
    
    pthread_mutex_lock(&system->system_mutex);
    
    if (!system->is_running) {
        pthread_mutex_unlock(&system->system_mutex);
        return OK_PTR(system);
    }
    
    printf("🛑 Shutting down actor system: %s\n", system->name);
    
    system->is_shutting_down = true;
    
    // Stop all actors
    for (size_t i = 0; i < system->actor_count; i++) {
        if (system->actors[i]) {
            actor_stop(system->actors[i]);
        }
    }
    
    // Close all event streams
    for (size_t i = 0; i < system->system_stream_count; i++) {
        if (system->system_streams[i]) {
            event_stream_close(system->system_streams[i]);
        }
    }
    
    // Stop all reactive components
    for (size_t i = 0; i < system->component_count; i++) {
        if (system->components[i]) {
            reactive_component_stop(system->components[i]);
        }
    }
    
    // Stop background threads
    system->dispatcher_running = false;
    system->processor_running = false;
    system->supervisor_running = false;
    
    // TODO: Join background threads
    
    system->is_running = false;
    system->is_shutting_down = false;
    
    pthread_cond_broadcast(&system->shutdown_complete);
    pthread_mutex_unlock(&system->system_mutex);
    
    printf("✅ Actor system shutdown complete: %s\n", system->name);
    return OK_PTR(system);
}

// Actor Implementation

Actor* actor_create(ActorSystem* system, const char* name, ActorMessageHandler handler, 
                   void* initial_state, size_t state_size) {
    if (!system || !handler) return NULL;
    
    Actor* actor = calloc(1, sizeof(Actor));
    if (!actor) return NULL;
    
    actor->id = generate_id();
    strncpy(actor->name, name ? name : "actor", sizeof(actor->name) - 1);
    actor->state = ACTOR_STATE_CREATED;
    actor->message_handler = handler;
    actor->actor_system = system;
    
    // Copy initial state
    if (initial_state && state_size > 0) {
        actor->actor_state = malloc(state_size);
        if (!actor->actor_state) {
            free(actor);
            return NULL;
        }
        memcpy(actor->actor_state, initial_state, state_size);
        actor->state_size = state_size;
    }
    
    // Initialize child array
    actor->child_capacity = 8;
    actor->children = calloc(actor->child_capacity, sizeof(Actor*));
    if (!actor->children) {
        free(actor->actor_state);
        free(actor);
        return NULL;
    }
    
    // Set default configuration
    actor->config.mailbox_capacity = system->default_mailbox_capacity;
    actor->config.message_timeout_ms = system->default_message_timeout_ms;
    actor->config.supervision_strategy = system->default_supervision_strategy;
    actor->config.max_restart_attempts = 3;
    actor->config.restart_interval_ms = 1000;
    actor->config.enable_metrics = system->enable_actor_metrics;
    
    actor->created_time_ns = get_current_time_ns();
    atomic_init(&actor->ref_count, 1);
    atomic_init(&actor->messages_processed, 0);
    
    // Initialize synchronization
    if (pthread_mutex_init(&actor->actor_mutex, NULL) != 0 ||
        pthread_cond_init(&actor->message_available, NULL) != 0 ||
        pthread_cond_init(&actor->state_changed, NULL) != 0) {
        free(actor->children);
        free(actor->actor_state);
        free(actor);
        return NULL;
    }
    
    // Add actor to system
    pthread_mutex_lock(&system->system_mutex);
    if (system->actor_count >= system->actor_capacity) {
        size_t new_capacity = system->actor_capacity * 2;
        Actor** new_actors = realloc(system->actors, new_capacity * sizeof(Actor*));
        if (new_actors) {
            system->actors = new_actors;
            system->actor_capacity = new_capacity;
        } else {
            pthread_mutex_unlock(&system->system_mutex);
            pthread_mutex_destroy(&actor->actor_mutex);
            pthread_cond_destroy(&actor->message_available);
            pthread_cond_destroy(&actor->state_changed);
            free(actor->children);
            free(actor->actor_state);
            free(actor);
            return NULL;
        }
    }
    
    system->actors[system->actor_count++] = actor;
    pthread_mutex_unlock(&system->system_mutex);
    
    printf("🎭 Created actor: %s (ID: %llu) in system: %s\n", 
           actor->name, actor->id, system->name);
    
    return actor;
}

void actor_destroy(Actor* actor) {
    if (!actor) return;
    
    printf("🗑️ Destroying actor: %s (ID: %llu)\n", actor->name, actor->id);
    
    // Stop actor if still running
    if (actor->state == ACTOR_STATE_RUNNING) {
        actor_stop(actor);
    }
    
    pthread_mutex_lock(&actor->actor_mutex);
    
    // Destroy all child actors
    for (size_t i = 0; i < actor->child_count; i++) {
        if (actor->children[i]) {
            actor_destroy(actor->children[i]);
        }
    }
    free(actor->children);
    
    // Clean up mailbox
    Message* current = actor->mailbox_head;
    while (current) {
        Message* next = current->next;
        message_destroy(current);
        current = next;
    }
    
    // Clean up state
    free(actor->actor_state);
    
    // Clean up error
    if (actor->last_error) {
        // TODO: Implement error cleanup
    }
    
    pthread_mutex_unlock(&actor->actor_mutex);
    
    // Cleanup synchronization
    pthread_mutex_destroy(&actor->actor_mutex);
    pthread_cond_destroy(&actor->message_available);
    pthread_cond_destroy(&actor->state_changed);
    
    free(actor);
}

Result_void_ptr actor_start(Actor* actor) {
    if (!actor) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid actor"));
    }
    
    pthread_mutex_lock(&actor->actor_mutex);
    
    if (actor->state != ACTOR_STATE_CREATED) {
        pthread_mutex_unlock(&actor->actor_mutex);
        return ERR_PTR(error_create(ERROR_INVALID_STATE, "Actor is not in created state"));
    }
    
    printf("🚀 Starting actor: %s (ID: %llu)\n", actor->name, actor->id);
    
    actor->state = ACTOR_STATE_RUNNING;
    actor->started_time_ns = get_current_time_ns();
    actor->thread_running = true;
    
    // Send initialization message
    Message* init_msg = message_create(MESSAGE_TYPE_INIT, NULL, 0);
    if (init_msg) {
        init_msg->recipient = actor;
        init_msg->sender = actor; // Self-sent initialization
        
        // Add to mailbox
        if (!actor->mailbox_head) {
            actor->mailbox_head = actor->mailbox_tail = init_msg;
        } else {
            actor->mailbox_tail->next = init_msg;
            actor->mailbox_tail = init_msg;
        }
        actor->mailbox_size++;
        
        pthread_cond_signal(&actor->message_available);
    }
    
    pthread_cond_broadcast(&actor->state_changed);
    pthread_mutex_unlock(&actor->actor_mutex);
    
    printf("✅ Actor started: %s\n", actor->name);
    return OK_PTR(actor);
}

Result_void_ptr actor_stop(Actor* actor) {
    if (!actor) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid actor"));
    }
    
    pthread_mutex_lock(&actor->actor_mutex);
    
    if (actor->state != ACTOR_STATE_RUNNING) {
        pthread_mutex_unlock(&actor->actor_mutex);
        return OK_PTR(actor);
    }
    
    printf("🛑 Stopping actor: %s (ID: %llu)\n", actor->name, actor->id);
    
    actor->state = ACTOR_STATE_STOPPING;
    
    // Send stop message
    Message* stop_msg = message_create(MESSAGE_TYPE_STOP, NULL, 0);
    if (stop_msg) {
        stop_msg->recipient = actor;
        stop_msg->sender = actor; // Self-sent stop
        
        // Add to front of mailbox for priority
        stop_msg->next = actor->mailbox_head;
        actor->mailbox_head = stop_msg;
        if (!actor->mailbox_tail) {
            actor->mailbox_tail = stop_msg;
        }
        actor->mailbox_size++;
        
        pthread_cond_signal(&actor->message_available);
    }
    
    // Stop all child actors
    for (size_t i = 0; i < actor->child_count; i++) {
        if (actor->children[i]) {
            actor_stop(actor->children[i]);
        }
    }
    
    actor->thread_running = false;
    actor->state = ACTOR_STATE_STOPPED;
    actor->stopped_time_ns = get_current_time_ns();
    
    pthread_cond_broadcast(&actor->state_changed);
    pthread_mutex_unlock(&actor->actor_mutex);
    
    printf("✅ Actor stopped: %s\n", actor->name);
    return OK_PTR(actor);
}

Result_void_ptr actor_restart(Actor* actor) {
    if (!actor) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid actor"));
    }
    
    printf("🔄 Restarting actor: %s (ID: %llu)\n", actor->name, actor->id);
    
    pthread_mutex_lock(&actor->actor_mutex);
    
    actor->restart_count++;
    actor->last_restart_time_ns = get_current_time_ns();
    
    // Check restart limits
    if (actor->restart_count > actor->config.max_restart_attempts) {
        pthread_mutex_unlock(&actor->actor_mutex);
        printf("❌ Actor restart limit exceeded: %s\n", actor->name);
        return ERR_PTR(error_create(ERROR_OPERATION_FAILED, "Restart limit exceeded"));
    }
    
    pthread_mutex_unlock(&actor->actor_mutex);
    
    // Stop and start actor
    Result_void_ptr stop_result = actor_stop(actor);
    if (stop_result.is_error) {
        return stop_result;
    }
    
    // Wait briefly before restart
    usleep(actor->config.restart_interval_ms * 1000);
    
    pthread_mutex_lock(&actor->actor_mutex);
    actor->state = ACTOR_STATE_CREATED;
    pthread_mutex_unlock(&actor->actor_mutex);
    
    Result_void_ptr start_result = actor_start(actor);
    if (!start_result.is_error) {
        printf("✅ Actor restarted successfully: %s\n", actor->name);
    }
    
    return start_result;
}

// Message Implementation

Message* message_create(MessageType type, void* data, size_t data_size) {
    Message* message = calloc(1, sizeof(Message));
    if (!message) return NULL;
    
    message->id = generate_id();
    message->type = type;
    message->timestamp_ns = get_current_time_ns();
    message->priority = 1; // Normal priority
    
    // Copy data if provided
    if (data && data_size > 0) {
        message->data = malloc(data_size);
        if (!message->data) {
            free(message);
            return NULL;
        }
        memcpy(message->data, data, data_size);
        message->data_size = data_size;
    }
    
    atomic_init(&message->ref_count, 1);
    
    return message;
}

void message_destroy(Message* message) {
    if (!message) return;
    
    if (message->destructor && message->data) {
        message->destructor(message->data);
    } else {
        free(message->data);
    }
    
    free(message);
}

Message* message_ref(Message* message) {
    if (message) {
        atomic_fetch_add(&message->ref_count, 1);
    }
    return message;
}

void message_unref(Message* message) {
    if (message && atomic_fetch_sub(&message->ref_count, 1) == 1) {
        message_destroy(message);
    }
}

Result_void_ptr actor_send_message(Actor* sender, Actor* recipient, MessageType type, 
                                 void* data, size_t data_size) {
    if (!recipient) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid recipient actor"));
    }
    
    Message* message = message_create(type, data, data_size);
    if (!message) {
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create message"));
    }
    
    message->sender = sender;
    message->recipient = recipient;
    
    pthread_mutex_lock(&recipient->actor_mutex);
    
    // Check if recipient can accept messages
    if (recipient->state != ACTOR_STATE_RUNNING) {
        pthread_mutex_unlock(&recipient->actor_mutex);
        message_destroy(message);
        return ERR_PTR(error_create(ERROR_INVALID_STATE, "Recipient actor is not running"));
    }
    
    // Check mailbox capacity
    if (recipient->mailbox_size >= recipient->config.mailbox_capacity) {
        pthread_mutex_unlock(&recipient->actor_mutex);
        
        // Send to dead letter queue
        if (recipient->actor_system->enable_dead_letter_queue) {
            ActorSystem* system = recipient->actor_system;
            pthread_mutex_lock(&system->system_mutex);
            
            if (system->dead_letter_count < system->max_dead_letters) {
                if (!system->dead_letter_head) {
                    system->dead_letter_head = system->dead_letter_tail = message;
                } else {
                    system->dead_letter_tail->next = message;
                    system->dead_letter_tail = message;
                }
                system->dead_letter_count++;
                message = NULL; // Don't destroy - now in dead letter queue
            }
            
            pthread_mutex_unlock(&system->system_mutex);
        }
        
        if (message) {
            message_destroy(message);
        }
        
        return ERR_PTR(error_create(ERROR_OPERATION_FAILED, "Recipient mailbox full"));
    }
    
    // Add message to mailbox
    if (!recipient->mailbox_head) {
        recipient->mailbox_head = recipient->mailbox_tail = message;
    } else {
        recipient->mailbox_tail->next = message;
        recipient->mailbox_tail = message;
    }
    recipient->mailbox_size++;
    
    pthread_cond_signal(&recipient->message_available);
    pthread_mutex_unlock(&recipient->actor_mutex);
    
    printf("📬 Message sent: %s -> %s (Type: %d, ID: %llu)\n", 
           sender ? sender->name : "system", recipient->name, type, message->id);
    
    return OK_PTR(message);
}

// Event Stream Implementation

EventStream* event_stream_create(const char* name, EventType type) {
    EventStream* stream = calloc(1, sizeof(EventStream));
    if (!stream) return NULL;
    
    stream->id = generate_id();
    strncpy(stream->name, name ? name : "event_stream", sizeof(stream->name) - 1);
    stream->event_type = type;
    stream->is_active = true;
    stream->max_buffer_size = 10000; // Default buffer size
    stream->created_time_ns = get_current_time_ns();
    
    atomic_init(&stream->sequence_counter, 0);
    atomic_init(&stream->ref_count, 1);
    
    // Initialize synchronization
    if (pthread_mutex_init(&stream->stream_mutex, NULL) != 0 ||
        pthread_cond_init(&stream->event_published, NULL) != 0) {
        free(stream);
        return NULL;
    }
    
    printf("📡 Created event stream: %s (ID: %llu, Type: %d)\n", 
           stream->name, stream->id, type);
    
    return stream;
}

void event_stream_destroy(EventStream* stream) {
    if (!stream) return;
    
    printf("🗑️ Destroying event stream: %s (ID: %llu)\n", stream->name, stream->id);
    
    pthread_mutex_lock(&stream->stream_mutex);
    
    // Clean up all events
    Event* current = stream->head_event;
    while (current) {
        Event* next = current->next;
        event_destroy(current);
        current = next;
    }
    
    // Clean up all subscriptions
    EventSubscription* sub = stream->first_subscription;
    while (sub) {
        EventSubscription* next_sub = sub->next;
        event_subscription_destroy(sub);
        sub = next_sub;
    }
    
    stream->is_closed = true;
    pthread_cond_broadcast(&stream->event_published);
    pthread_mutex_unlock(&stream->stream_mutex);
    
    // Cleanup synchronization
    pthread_mutex_destroy(&stream->stream_mutex);
    pthread_cond_destroy(&stream->event_published);
    
    free(stream);
}

// Event Implementation

Event* event_create(EventType type, const char* name, void* data, size_t data_size) {
    Event* event = calloc(1, sizeof(Event));
    if (!event) return NULL;
    
    event->id = generate_id();
    event->type = type;
    strncpy(event->name, name ? name : "event", sizeof(event->name) - 1);
    event->timestamp_ns = get_current_time_ns();
    event->priority = 1; // Normal priority
    event->is_aggregatable = true;
    event->is_filterable = true;
    
    // Copy data if provided
    if (data && data_size > 0) {
        event->data = malloc(data_size);
        if (!event->data) {
            free(event);
            return NULL;
        }
        memcpy(event->data, data, data_size);
        event->data_size = data_size;
    }
    
    atomic_init(&event->ref_count, 1);
    
    return event;
}

void event_destroy(Event* event) {
    if (!event) return;
    
    if (event->destructor && event->data) {
        event->destructor(event->data);
    } else {
        free(event->data);
    }
    
    free(event);
}

Event* event_ref(Event* event) {
    if (event) {
        atomic_fetch_add(&event->ref_count, 1);
    }
    return event;
}

void event_unref(Event* event) {
    if (event && atomic_fetch_sub(&event->ref_count, 1) == 1) {
        event_destroy(event);
    }
}

Result_void_ptr event_stream_publish(EventStream* stream, Event* event) {
    if (!stream || !event) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid stream or event"));
    }
    
    pthread_mutex_lock(&stream->stream_mutex);
    
    if (!stream->is_active || stream->is_closed) {
        pthread_mutex_unlock(&stream->stream_mutex);
        return ERR_PTR(error_create(ERROR_INVALID_STATE, "Event stream is closed"));
    }
    
    // Check buffer limits
    if (stream->event_count >= stream->max_buffer_size) {
        if (stream->enable_backpressure) {
            // Drop oldest event
            if (stream->head_event) {
                Event* old_event = stream->head_event;
                stream->head_event = old_event->next;
                if (!stream->head_event) {
                    stream->tail_event = NULL;
                }
                event_unref(old_event);
                stream->event_count--;
                stream->dropped_events++;
            }
        } else {
            pthread_mutex_unlock(&stream->stream_mutex);
            return ERR_PTR(error_create(ERROR_OPERATION_FAILED, "Event stream buffer full"));
        }
    }
    
    // Set sequence number
    event->sequence_number = atomic_fetch_add(&stream->sequence_counter, 1);
    
    // Add event to stream
    event_ref(event); // Reference for the stream
    if (!stream->head_event) {
        stream->head_event = stream->tail_event = event;
    } else {
        stream->tail_event->next = event;
        stream->tail_event = event;
    }
    stream->event_count++;
    
    // Process subscriptions
    EventSubscription* sub = stream->first_subscription;
    while (sub) {
        if (sub->is_active && !sub->is_paused) {
            // Apply filter if present
            bool should_process = true;
            if (sub->config.filter) {
                should_process = sub->config.filter(event, sub->config.filter_context);
                if (!should_process) {
                    sub->events_filtered++;
                }
            }
            
            if (should_process) {
                // Add event to subscription buffer
                pthread_mutex_lock(&sub->subscription_mutex);
                
                if (sub->buffer_size < sub->buffer_capacity) {
                    sub->event_buffer[sub->buffer_size++] = event_ref(event);
                    pthread_cond_signal(&sub->events_available);
                }
                
                pthread_mutex_unlock(&sub->subscription_mutex);
            }
        }
        sub = sub->next;
    }
    
    pthread_cond_broadcast(&stream->event_published);
    pthread_mutex_unlock(&stream->stream_mutex);
    
    printf("📡 Event published: %s -> %s (ID: %llu, Seq: %llu)\n", 
           event->name, stream->name, event->id, event->sequence_number);
    
    return OK_PTR(event);
}

// Reactive Component Implementation

ReactiveComponent* reactive_component_create(const char* name, ReactiveComponentConfig* config) {
    ReactiveComponent* component = calloc(1, sizeof(ReactiveComponent));
    if (!component) return NULL;
    
    component->id = generate_id();
    strncpy(component->name, name ? name : "reactive_component", sizeof(component->name) - 1);
    
    if (config) {
        component->config = *config;
    } else {
        // Set defaults
        strncpy(component->config.name, component->name, sizeof(component->config.name) - 1);
        component->config.auto_start = true;
        component->config.enable_persistence = false;
        component->config.enable_metrics = true;
        component->config.processing_timeout_ms = 5000;
        component->config.max_event_buffer_size = 1000;
    }
    
    // Initialize stream arrays
    component->input_stream_capacity = 8;
    component->input_streams = calloc(component->input_stream_capacity, sizeof(EventStream*));
    if (!component->input_streams) {
        free(component);
        return NULL;
    }
    
    component->output_stream_capacity = 8;
    component->output_streams = calloc(component->output_stream_capacity, sizeof(EventStream*));
    if (!component->output_streams) {
        free(component->input_streams);
        free(component);
        return NULL;
    }
    
    atomic_init(&component->ref_count, 1);
    
    // Initialize synchronization
    if (pthread_mutex_init(&component->component_mutex, NULL) != 0) {
        free(component->output_streams);
        free(component->input_streams);
        free(component);
        return NULL;
    }
    
    printf("⚡ Created reactive component: %s (ID: %llu)\n", component->name, component->id);
    
    return component;
}

void reactive_component_destroy(ReactiveComponent* component) {
    if (!component) return;
    
    printf("🗑️ Destroying reactive component: %s (ID: %llu)\n", component->name, component->id);
    
    // Stop component if active
    if (component->is_active) {
        reactive_component_stop(component);
    }
    
    pthread_mutex_lock(&component->component_mutex);
    
    // Clean up streams (just remove references, don't destroy)
    free(component->input_streams);
    free(component->output_streams);
    
    // Clean up component state
    free(component->component_state);
    
    // Clean up backing actor if present
    if (component->backing_actor) {
        actor_destroy(component->backing_actor);
    }
    
    pthread_mutex_unlock(&component->component_mutex);
    
    // Cleanup synchronization
    pthread_mutex_destroy(&component->component_mutex);
    
    free(component);
}

Result_void_ptr reactive_component_start(ReactiveComponent* component) {
    if (!component) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid reactive component"));
    }
    
    pthread_mutex_lock(&component->component_mutex);
    
    if (component->is_active) {
        pthread_mutex_unlock(&component->component_mutex);
        return OK_PTR(component);
    }
    
    printf("🚀 Starting reactive component: %s\n", component->name);
    
    component->is_active = true;
    
    // Start backing actor if present
    if (component->backing_actor) {
        actor_start(component->backing_actor);
    }
    
    pthread_mutex_unlock(&component->component_mutex);
    
    printf("✅ Reactive component started: %s\n", component->name);
    return OK_PTR(component);
}

Result_void_ptr reactive_component_stop(ReactiveComponent* component) {
    if (!component) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid reactive component"));
    }
    
    pthread_mutex_lock(&component->component_mutex);
    
    if (!component->is_active) {
        pthread_mutex_unlock(&component->component_mutex);
        return OK_PTR(component);
    }
    
    printf("🛑 Stopping reactive component: %s\n", component->name);
    
    component->is_active = false;
    component->is_processing = false;
    
    // Stop backing actor if present
    if (component->backing_actor) {
        actor_stop(component->backing_actor);
    }
    
    pthread_mutex_unlock(&component->component_mutex);
    
    printf("✅ Reactive component stopped: %s\n", component->name);
    return OK_PTR(component);
}

// Statistics and monitoring functions

ReactiveSystemStats actor_system_get_stats(ActorSystem* system) {
    ReactiveSystemStats stats = {0};
    
    if (!system) return stats;
    
    pthread_mutex_lock(&system->system_mutex);
    
    stats.total_actors_created = system->actor_count;
    stats.total_messages_sent = system->total_messages_processed;
    stats.total_messages_processed = system->total_messages_processed;
    stats.dead_letter_count = system->dead_letter_count;
    stats.actor_restarts = system->total_actor_restarts;
    stats.total_events_emitted = system->total_events_processed;
    stats.total_events_processed = system->total_events_processed;
    stats.active_streams = system->system_stream_count;
    stats.active_components = system->component_count;
    
    // Count currently active actors
    for (size_t i = 0; i < system->actor_count; i++) {
        if (system->actors[i] && system->actors[i]->state == ACTOR_STATE_RUNNING) {
            stats.currently_active_actors++;
        }
    }
    
    // Count active subscriptions
    for (size_t i = 0; i < system->system_stream_count; i++) {
        if (system->system_streams[i]) {
            stats.active_subscriptions += system->system_streams[i]->subscription_count;
        }
    }
    
    pthread_mutex_unlock(&system->system_mutex);
    
    return stats;
}

void actor_system_print_stats(ActorSystem* system) {
    if (!system) return;
    
    ReactiveSystemStats stats = actor_system_get_stats(system);
    
    printf("\n📊 Actor System Statistics: %s\n", system->name);
    printf("=====================================\n");
    printf("Total Actors Created: %llu\n", stats.total_actors_created);
    printf("Currently Active Actors: %llu\n", stats.currently_active_actors);
    printf("Total Messages Sent: %llu\n", stats.total_messages_sent);
    printf("Total Messages Processed: %llu\n", stats.total_messages_processed);
    printf("Dead Letter Count: %llu\n", stats.dead_letter_count);
    printf("Actor Restarts: %llu\n", stats.actor_restarts);
    printf("Total Events Emitted: %llu\n", stats.total_events_emitted);
    printf("Total Events Processed: %llu\n", stats.total_events_processed);
    printf("Active Subscriptions: %llu\n", stats.active_subscriptions);
    printf("Active Streams: %llu\n", stats.active_streams);
    printf("Active Components: %llu\n", stats.active_components);
    printf("Avg Message Processing Time: %.2f ms\n", stats.average_message_processing_time_ms);
    printf("Avg Event Processing Time: %.2f ms\n", stats.average_event_processing_time_ms);
}

// Additional missing function implementations

Result_void_ptr actor_add_child(Actor* parent, Actor* child) {
    if (!parent || !child) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid parent or child actor"));
    }
    
    pthread_mutex_lock(&parent->actor_mutex);
    
    // Expand child array if needed
    if (parent->child_count >= parent->child_capacity) {
        size_t new_capacity = parent->child_capacity * 2;
        Actor** new_children = realloc(parent->children, new_capacity * sizeof(Actor*));
        if (!new_children) {
            pthread_mutex_unlock(&parent->actor_mutex);
            return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to expand child array"));
        }
        parent->children = new_children;
        parent->child_capacity = new_capacity;
    }
    
    // Add child
    parent->children[parent->child_count++] = child;
    child->parent = parent;
    
    pthread_mutex_unlock(&parent->actor_mutex);
    
    printf("👨‍👩‍👧‍👦 Added child actor: %s -> %s\n", child->name, parent->name);
    return OK_PTR(parent);
}

Result_void_ptr actor_remove_child(Actor* parent, Actor* child) {
    if (!parent || !child) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid parent or child actor"));
    }
    
    pthread_mutex_lock(&parent->actor_mutex);
    
    // Find and remove child
    for (size_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child) {
            // Move last element to this position
            parent->children[i] = parent->children[parent->child_count - 1];
            parent->child_count--;
            child->parent = NULL;
            
            pthread_mutex_unlock(&parent->actor_mutex);
            printf("👨‍👩‍👧‍👦 Removed child actor: %s from %s\n", child->name, parent->name);
            return OK_PTR(parent);
        }
    }
    
    pthread_mutex_unlock(&parent->actor_mutex);
    return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Child not found"));
}

Result_void_ptr actor_supervise(Actor* supervisor, Actor* supervised, SupervisionStrategy strategy) {
    if (!supervisor || !supervised) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid supervisor or supervised actor"));
    }
    
    supervised->config.supervision_strategy = strategy;
    return actor_add_child(supervisor, supervised);
}

Result_void_ptr actor_send_message_async(Actor* sender, Actor* recipient, MessageType type,
                                        void* data, size_t data_size, AsyncWaker* waker) {
    // For now, just delegate to regular send_message
    // In a full implementation, this would integrate with the async runtime
    (void)waker; // Unused parameter
    return actor_send_message(sender, recipient, type, data, data_size);
}

Result_void_ptr actor_broadcast_message(Actor* sender, MessageType type, void* data, size_t data_size) {
    if (!sender || !sender->actor_system) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid sender actor"));
    }
    
    ActorSystem* system = sender->actor_system;
    pthread_mutex_lock(&system->system_mutex);
    
    printf("📢 Broadcasting message from %s to %zu actors\n", sender->name, system->actor_count);
    
    // Send message to all actors in the system
    for (size_t i = 0; i < system->actor_count; i++) {
        if (system->actors[i] && system->actors[i] != sender) {
            actor_send_message(sender, system->actors[i], type, data, data_size);
        }
    }
    
    pthread_mutex_unlock(&system->system_mutex);
    return OK_PTR(sender);
}

Result_void_ptr event_stream_close(EventStream* stream) {
    if (!stream) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid event stream"));
    }
    
    pthread_mutex_lock(&stream->stream_mutex);
    
    if (stream->is_closed) {
        pthread_mutex_unlock(&stream->stream_mutex);
        return OK_PTR(stream);
    }
    
    printf("🔒 Closing event stream: %s\n", stream->name);
    
    stream->is_active = false;
    stream->is_closed = true;
    
    // Notify all waiting subscribers
    pthread_cond_broadcast(&stream->event_published);
    
    pthread_mutex_unlock(&stream->stream_mutex);
    
    return OK_PTR(stream);
}

EventSubscription* event_subscription_create(EventStream* stream, const char* name, 
                                           SubscriptionConfig* config) {
    if (!stream || !config) return NULL;
    
    EventSubscription* subscription = calloc(1, sizeof(EventSubscription));
    if (!subscription) return NULL;
    
    subscription->id = generate_id();
    strncpy(subscription->name, name ? name : "subscription", sizeof(subscription->name) - 1);
    subscription->config = *config;
    subscription->is_active = true;
    subscription->created_time_ns = get_current_time_ns();
    
    // Initialize event buffer
    subscription->buffer_capacity = config->buffer_size > 0 ? config->buffer_size : 100;
    subscription->event_buffer = calloc(subscription->buffer_capacity, sizeof(Event*));
    if (!subscription->event_buffer) {
        free(subscription);
        return NULL;
    }
    
    atomic_init(&subscription->ref_count, 1);
    
    // Initialize synchronization
    if (pthread_mutex_init(&subscription->subscription_mutex, NULL) != 0 ||
        pthread_cond_init(&subscription->events_available, NULL) != 0) {
        free(subscription->event_buffer);
        free(subscription);
        return NULL;
    }
    
    // Add to stream
    pthread_mutex_lock(&stream->stream_mutex);
    subscription->stream = stream;
    
    if (!stream->first_subscription) {
        stream->first_subscription = stream->last_subscription = subscription;
    } else {
        stream->last_subscription->next = subscription;
        stream->last_subscription = subscription;
    }
    stream->subscription_count++;
    
    pthread_mutex_unlock(&stream->stream_mutex);
    
    printf("📋 Created subscription: %s for stream: %s\n", subscription->name, stream->name);
    return subscription;
}

void event_subscription_destroy(EventSubscription* subscription) {
    if (!subscription) return;
    
    printf("🗑️ Destroying subscription: %s\n", subscription->name);
    
    pthread_mutex_lock(&subscription->subscription_mutex);
    
    // Clean up event buffer
    for (size_t i = 0; i < subscription->buffer_size; i++) {
        if (subscription->event_buffer[i]) {
            event_unref(subscription->event_buffer[i]);
        }
    }
    free(subscription->event_buffer);
    
    subscription->is_active = false;
    pthread_mutex_unlock(&subscription->subscription_mutex);
    
    // Cleanup synchronization
    pthread_mutex_destroy(&subscription->subscription_mutex);
    pthread_cond_destroy(&subscription->events_available);
    
    free(subscription);
}

Result_void_ptr reactive_component_add_input_stream(ReactiveComponent* component, EventStream* stream) {
    if (!component || !stream) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid component or stream"));
    }
    
    pthread_mutex_lock(&component->component_mutex);
    
    // Expand input stream array if needed
    if (component->input_stream_count >= component->input_stream_capacity) {
        size_t new_capacity = component->input_stream_capacity * 2;
        EventStream** new_streams = realloc(component->input_streams, 
                                           new_capacity * sizeof(EventStream*));
        if (!new_streams) {
            pthread_mutex_unlock(&component->component_mutex);
            return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to expand input stream array"));
        }
        component->input_streams = new_streams;
        component->input_stream_capacity = new_capacity;
    }
    
    // Add stream
    component->input_streams[component->input_stream_count++] = stream;
    
    pthread_mutex_unlock(&component->component_mutex);
    
    printf("⚡ Added input stream: %s -> %s\n", stream->name, component->name);
    return OK_PTR(component);
}

Result_void_ptr reactive_component_add_output_stream(ReactiveComponent* component, EventStream* stream) {
    if (!component || !stream) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid component or stream"));
    }
    
    pthread_mutex_lock(&component->component_mutex);
    
    // Expand output stream array if needed
    if (component->output_stream_count >= component->output_stream_capacity) {
        size_t new_capacity = component->output_stream_capacity * 2;
        EventStream** new_streams = realloc(component->output_streams, 
                                           new_capacity * sizeof(EventStream*));
        if (!new_streams) {
            pthread_mutex_unlock(&component->component_mutex);
            return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to expand output stream array"));
        }
        component->output_streams = new_streams;
        component->output_stream_capacity = new_capacity;
    }
    
    // Add stream
    component->output_streams[component->output_stream_count++] = stream;
    
    pthread_mutex_unlock(&component->component_mutex);
    
    printf("⚡ Added output stream: %s -> %s\n", stream->name, component->name);
    return OK_PTR(component);
}

// Event pattern processing functions
EventPattern* event_pattern_create(const char* name, EventFilter* filters, size_t filter_count,
                                 EventAggregator aggregator, uint64_t time_window_ms) {
    EventPattern* pattern = calloc(1, sizeof(EventPattern));
    if (!pattern) return NULL;
    
    pattern->id = generate_id();
    strncpy(pattern->name, name ? name : "pattern", sizeof(pattern->name) - 1);
    pattern->time_window_ms = time_window_ms;
    pattern->aggregator = aggregator;
    pattern->is_active = true;
    
    // Copy filters
    if (filters && filter_count > 0) {
        pattern->filters = malloc(filter_count * sizeof(EventFilter));
        if (pattern->filters) {
            memcpy(pattern->filters, filters, filter_count * sizeof(EventFilter));
            pattern->filter_count = filter_count;
        }
    }
    
    // Initialize match array
    pattern->max_matches = 100; // Default
    pattern->matched_events = calloc(pattern->max_matches, sizeof(Event*));
    if (!pattern->matched_events) {
        free(pattern->filters);
        free(pattern);
        return NULL;
    }
    
    return pattern;
}

void event_pattern_destroy(EventPattern* pattern) {
    if (!pattern) return;
    
    // Clean up matched events
    for (size_t i = 0; i < pattern->matched_count; i++) {
        if (pattern->matched_events[i]) {
            event_unref(pattern->matched_events[i]);
        }
    }
    
    free(pattern->matched_events);
    free(pattern->filters);
    free(pattern);
}

// Statistics printing functions
void actor_print_stats(Actor* actor) {
    if (!actor) return;
    
    printf("\n🎭 Actor Statistics: %s (ID: %llu)\n", actor->name, actor->id);
    printf("State: %d\n", actor->state);
    printf("Messages Processed: %llu\n", atomic_load(&actor->messages_processed));
    printf("Mailbox Size: %zu / %zu\n", actor->mailbox_size, actor->config.mailbox_capacity);
    printf("Restart Count: %d\n", actor->restart_count);
    printf("Child Count: %zu\n", actor->child_count);
}

void event_stream_print_stats(EventStream* stream) {
    if (!stream) return;
    
    printf("\n📡 Event Stream Statistics: %s (ID: %llu)\n", stream->name, stream->id);
    printf("Event Type: %d\n", stream->event_type);
    printf("Event Count: %zu\n", stream->event_count);
    printf("Subscription Count: %zu\n", stream->subscription_count);
    printf("Dropped Events: %llu\n", stream->dropped_events);
    printf("Is Active: %s\n", stream->is_active ? "Yes" : "No");
    printf("Is Closed: %s\n", stream->is_closed ? "Yes" : "No");
}

void reactive_component_print_stats(ReactiveComponent* component) {
    if (!component) return;
    
    printf("\n⚡ Reactive Component Statistics: %s (ID: %llu)\n", component->name, component->id);
    printf("Is Active: %s\n", component->is_active ? "Yes" : "No");
    printf("Events Processed: %llu\n", component->events_processed);
    printf("Processing Errors: %llu\n", component->processing_errors);
    printf("Input Streams: %zu\n", component->input_stream_count);
    printf("Output Streams: %zu\n", component->output_stream_count);
}

// Integration functions

Result_void_ptr reactive_system_integrate_async(ActorSystem* system, void* async_context) {
    if (!system) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid actor system"));
    }
    
    pthread_mutex_lock(&system->system_mutex);
    system->async_context = async_context;
    pthread_mutex_unlock(&system->system_mutex);
    
    printf("🔗 Integrated actor system with async context: %s\n", system->name);
    return OK_PTR(system);
}

Result_void_ptr actor_integrate_async(Actor* actor, void* async_context) {
    if (!actor) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid actor"));
    }
    
    pthread_mutex_lock(&actor->actor_mutex);
    actor->async_context = async_context;
    pthread_mutex_unlock(&actor->actor_mutex);
    
    return OK_PTR(actor);
}

Result_void_ptr event_stream_integrate_async(EventStream* stream, void* async_context) {
    if (!stream) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid event stream"));
    }
    
    // Integration would involve connecting the stream to the async runtime
    // for efficient event processing and backpressure handling
    
    return OK_PTR(stream);
}

// Dead letter queue management
void actor_system_process_dead_letters(ActorSystem* system) {
    if (!system) return;
    
    pthread_mutex_lock(&system->system_mutex);
    
    printf("📬 Processing %zu dead letters\n", system->dead_letter_count);
    
    Message* current = system->dead_letter_head;
    while (current) {
        printf("💀 Dead letter: Type %d, ID %llu\n", current->type, current->id);
        current = current->next;
    }
    
    pthread_mutex_unlock(&system->system_mutex);
}

Message* actor_system_get_dead_letter(ActorSystem* system) {
    if (!system) return NULL;
    
    pthread_mutex_lock(&system->system_mutex);
    
    Message* dead_letter = system->dead_letter_head;
    if (dead_letter) {
        system->dead_letter_head = dead_letter->next;
        if (!system->dead_letter_head) {
            system->dead_letter_tail = NULL;
        }
        system->dead_letter_count--;
        dead_letter->next = NULL;
    }
    
    pthread_mutex_unlock(&system->system_mutex);
    
    return dead_letter;
}

void actor_system_clear_dead_letters(ActorSystem* system) {
    if (!system) return;
    
    pthread_mutex_lock(&system->system_mutex);
    
    Message* current = system->dead_letter_head;
    while (current) {
        Message* next = current->next;
        message_destroy(current);
        current = next;
    }
    
    system->dead_letter_head = system->dead_letter_tail = NULL;
    system->dead_letter_count = 0;
    
    pthread_mutex_unlock(&system->system_mutex);
    
    printf("🧹 Cleared all dead letters\n");
}