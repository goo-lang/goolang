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
    
    printf("⚡ Created reactive system extension: %s (ID: %llu)\n", reactive_system->name, reactive_system->id);
    return reactive_system;
}

void reactive_system_destroy(ReactiveSystemExtension* reactive_system) {
    if (!reactive_system) return;
    
    printf("🗑️ Destroying reactive system extension: %s (ID: %llu)\n", reactive_system->name, reactive_system->id);
    
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
    
    free(reactive_system);
}

Result_void_ptr reactive_system_start(ReactiveSystemExtension* reactive_system) {
    if (!reactive_system) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid reactive system"));
    }
    
    printf("🚀 Starting reactive system: %s\n", reactive_system->name);
    reactive_system->processor_running = true;
    
    return OK_PTR(reactive_system);
}

Result_void_ptr reactive_system_shutdown(ReactiveSystemExtension* reactive_system) {
    if (!reactive_system) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid reactive system"));
    }
    
    printf("🛑 Shutting down reactive system: %s\n", reactive_system->name);
    reactive_system->processor_running = false;
    
    return OK_PTR(reactive_system);
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
    
    pthread_mutex_unlock(&component->component_mutex);
    
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
    
    pthread_mutex_unlock(&component->component_mutex);
    
    return OK_PTR(component);
}

// Statistics and monitoring functions

ReactiveSystemStats reactive_system_get_stats(ReactiveSystemExtension* reactive_system) {
    ReactiveSystemStats stats = {0};
    
    if (!reactive_system) return stats;
    
    pthread_mutex_lock(&reactive_system->reactive_mutex);
    
    stats.total_events_emitted = reactive_system->total_events_processed;
    stats.total_events_processed = reactive_system->total_events_processed;
    stats.active_streams = reactive_system->system_stream_count;
    stats.active_components = reactive_system->component_count;
    stats.active_subscriptions = reactive_system->total_subscriptions_created;
    
    pthread_mutex_unlock(&reactive_system->reactive_mutex);
    
    return stats;
}

void reactive_system_print_stats(ReactiveSystemExtension* reactive_system) {
    if (!reactive_system) return;
    
    ReactiveSystemStats stats = reactive_system_get_stats(reactive_system);
    
    printf("\n📊 Reactive System Statistics: %s\n", reactive_system->name);
    printf("=====================================\n");
    printf("Total Events Emitted: %llu\n", stats.total_events_emitted);
    printf("Total Events Processed: %llu\n", stats.total_events_processed);
    printf("Active Subscriptions: %llu\n", stats.active_subscriptions);
    printf("Active Streams: %llu\n", stats.active_streams);
    printf("Active Components: %llu\n", stats.active_components);
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

Result_void_ptr reactive_system_integrate_async(ReactiveSystemExtension* reactive_system, void* async_context) {
    if (!reactive_system) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid reactive system"));
    }
    
    printf("🔗 Integrated reactive system with async context: %s\n", reactive_system->name);
    return OK_PTR(reactive_system);
}

Result_void_ptr event_stream_integrate_async(EventStream* stream, void* async_context) {
    if (!stream) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid event stream"));
    }
    
    // Integration would involve connecting the stream to the async runtime
    // for efficient event processing and backpressure handling
    
    return OK_PTR(stream);
}

// Placeholder implementations for other functions to avoid linking errors

EventSubscription* event_subscription_create(EventStream* stream, const char* name, 
                                           SubscriptionConfig* config) {
    (void)stream; (void)name; (void)config;
    printf("📋 event_subscription_create - placeholder implementation\n");
    return NULL;
}

void event_subscription_destroy(EventSubscription* subscription) {
    (void)subscription;
    printf("🗑️ event_subscription_destroy - placeholder implementation\n");
}

Result_void_ptr reactive_component_add_input_stream(ReactiveComponent* component, EventStream* stream) {
    (void)component; (void)stream;
    printf("⚡ reactive_component_add_input_stream - placeholder implementation\n");
    return OK_PTR(component);
}

Result_void_ptr reactive_component_add_output_stream(ReactiveComponent* component, EventStream* stream) {
    (void)component; (void)stream;
    printf("⚡ reactive_component_add_output_stream - placeholder implementation\n");
    return OK_PTR(component);
}

Actor* reactive_actor_create(ReactiveSystemExtension* reactive_system, const char* name, 
                           ReactiveMessageHandler handler, void* initial_state, size_t state_size) {
    (void)reactive_system; (void)name; (void)handler; (void)initial_state; (void)state_size;
    printf("🎭 reactive_actor_create - placeholder implementation\n");
    return NULL;
}

Result_void_ptr reactive_send_event_message(Actor* sender, Actor* recipient, Event* event) {
    (void)sender; (void)recipient; (void)event;
    printf("📬 reactive_send_event_message - placeholder implementation\n");
    return OK_PTR(sender);
}

Result_void_ptr reactive_broadcast_event(ReactiveSystemExtension* reactive_system, Event* event) {
    (void)reactive_system; (void)event;
    printf("📢 reactive_broadcast_event - placeholder implementation\n");
    return OK_PTR(reactive_system);
}