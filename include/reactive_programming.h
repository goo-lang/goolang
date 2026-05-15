#ifndef GOO_REACTIVE_PROGRAMMING_H
#define GOO_REACTIVE_PROGRAMMING_H

#include "transparent_async.h"
#include "async_resource.h"
#include "fearless_concurrency.h"
#include "errors/error.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "ccomp_shim.h"

// Forward declarations
typedef struct ReactiveComponent ReactiveComponent;
typedef struct EventStream EventStream;
typedef struct EventSubscription EventSubscription;
typedef struct EventPattern EventPattern;

// Additional message types for reactive programming (extending existing MessageType)
typedef enum {
    REACTIVE_MSG_EVENT = 100,   // Event message (starting from 100 to avoid conflicts)
    REACTIVE_MSG_SUBSCRIBE,     // Stream subscription message
    REACTIVE_MSG_UNSUBSCRIBE,   // Stream unsubscription message
    REACTIVE_MSG_PATTERN_MATCH, // Pattern matching message
    REACTIVE_MSG_AGGREGATE      // Event aggregation message
} ReactiveMessageType;

// Event types for reactive programming
typedef enum {
    EVENT_TYPE_STATE_CHANGE,    // State change event
    EVENT_TYPE_USER_ACTION,     // User interaction event
    EVENT_TYPE_TIMER,           // Timer-based event
    EVENT_TYPE_NETWORK,         // Network event
    EVENT_TYPE_FILE_SYSTEM,     // File system event
    EVENT_TYPE_CUSTOM           // Custom event type
} EventType;

// Use existing ActorState and Message from fearless_concurrency.h

// Use existing SupervisionStrategy from fearless_concurrency.h

// Reactive message handler function signature (specialized for reactive components)
typedef Result_void_ptr (*ReactiveMessageHandler)(Actor* actor, Message* message, void* state);

// Use existing Actor structure from fearless_concurrency.h

// Event structure for reactive programming
typedef struct Event {
    uint64_t id;
    EventType type;
    char name[64];
    
    // Event payload
    void* data;
    size_t data_size;
    void (*destructor)(void* data);
    
    // Event metadata
    uint64_t timestamp_ns;
    uint64_t sequence_number;
    char source[64];            // Event source identifier
    
    // Event correlation
    uint64_t correlation_id;
    char correlation_key[32];
    
    // Event properties
    bool is_aggregatable;
    bool is_filterable;
    uint32_t priority;
    
    // Reference counting
    atomic_int ref_count;
    
    struct Event* next;         // For event stream linked list
} Event;

// Event filter function signature
typedef bool (*EventFilter)(Event* event, void* filter_context);

// Event processor function signature  
typedef Result_void_ptr (*EventProcessor)(Event* event, void* processor_context);

// Event aggregator function signature
typedef Result_void_ptr (*EventAggregator)(Event** events, size_t event_count, void* aggregator_context);

// Event subscription configuration
typedef struct SubscriptionConfig {
    char name[64];
    EventFilter filter;
    void* filter_context;
    EventProcessor processor;
    void* processor_context;
    uint64_t buffer_size;
    uint64_t batch_timeout_ms;
    bool enable_batching;
    bool enable_ordering;
    bool enable_persistence;
} SubscriptionConfig;

// Event subscription
typedef struct EventSubscription {
    uint64_t id;
    char name[64];
    SubscriptionConfig config;
    
    // Subscription state
    bool is_active;
    bool is_paused;
    uint64_t created_time_ns;
    
    // Event processing
    Event** event_buffer;
    size_t buffer_size;
    size_t buffer_capacity;
    uint64_t events_processed;
    uint64_t events_filtered;
    
    // Associated stream
    EventStream* stream;
    
    // Synchronization
    pthread_mutex_t subscription_mutex;
    pthread_cond_t events_available;
    
    // Reference counting
    atomic_int ref_count;
    
    struct EventSubscription* next;
} EventSubscription;

// Event stream for reactive data flow
typedef struct EventStream {
    uint64_t id;
    char name[64];
    EventType event_type;
    
    // Stream state
    bool is_active;
    bool is_closed;
    uint64_t created_time_ns;
    
    // Event flow
    Event* head_event;
    Event* tail_event;
    size_t event_count;
    size_t max_buffer_size;
    atomic_uint_fast64_t sequence_counter;
    
    // Subscriptions
    EventSubscription* first_subscription;
    EventSubscription* last_subscription;
    size_t subscription_count;
    
    // Event processing patterns
    EventFilter global_filter;
    void* filter_context;
    EventAggregator aggregator;
    void* aggregator_context;
    uint64_t aggregation_window_ms;
    
    // Backpressure handling
    bool enable_backpressure;
    size_t backpressure_threshold;
    uint64_t dropped_events;
    
    // Synchronization
    pthread_mutex_t stream_mutex;
    pthread_cond_t event_published;
    
    // Reference counting
    atomic_int ref_count;
    
    struct EventStream* next;   // For stream registry linked list
} EventStream;

// Reactive component annotation and configuration
typedef struct ReactiveComponentConfig {
    char name[64];
    bool auto_start;
    bool enable_persistence;
    bool enable_metrics;
    uint64_t processing_timeout_ms;
    size_t max_event_buffer_size;
} ReactiveComponentConfig;

// Reactive component structure
typedef struct ReactiveComponent {
    uint64_t id;
    char name[64];
    ReactiveComponentConfig config;
    
    // Component state
    bool is_active;
    bool is_processing;
    void* component_state;
    size_t state_size;
    
    // Event streams (inputs and outputs)
    EventStream** input_streams;
    size_t input_stream_count;
    size_t input_stream_capacity;
    
    EventStream** output_streams;
    size_t output_stream_count;
    size_t output_stream_capacity;
    
    // Component processing function
    EventProcessor process_event;
    void* processor_context;
    
    // Actor integration (reactive components can be backed by actors)
    Actor* backing_actor;
    
    // Statistics
    uint64_t events_processed;
    uint64_t processing_errors;
    uint64_t average_processing_time_ns;
    
    // Synchronization
    pthread_mutex_t component_mutex;
    
    // Reference counting
    atomic_int ref_count;
    
    struct ReactiveComponent* next;
} ReactiveComponent;

// Reactive system extension for existing ActorSystem
typedef struct ReactiveSystemExtension {
    uint64_t id;
    char name[64];
    
    // Reference to existing actor system
    ActorSystem* actor_system;
    
    // System-wide event streams
    EventStream** system_streams;
    size_t system_stream_count;
    size_t system_stream_capacity;
    
    // Reactive components
    ReactiveComponent** components;
    size_t component_count;
    size_t component_capacity;
    
    // Event processing statistics
    uint64_t total_events_processed;
    uint64_t total_patterns_matched;
    uint64_t total_subscriptions_created;
    
    // Synchronization
    pthread_mutex_t reactive_mutex;
    
    // Background event processor thread
    pthread_t event_processor_thread;
    bool processor_running;
} ReactiveSystemExtension;

// Complex event processing patterns
typedef struct EventPattern {
    uint64_t id;
    char name[64];
    
    // Pattern definition
    EventFilter* filters;
    size_t filter_count;
    EventAggregator aggregator;
    void* aggregator_context;
    
    // Pattern timing
    uint64_t time_window_ms;
    uint64_t correlation_timeout_ms;
    
    // Pattern state
    bool is_active;
    Event** matched_events;
    size_t matched_count;
    size_t max_matches;
    
    // Pattern completion callback
    void (*on_pattern_match)(EventPattern* pattern, Event** events, size_t count, void* context);
    void* callback_context;
    
    struct EventPattern* next;
} EventPattern;

// Reactive system extension functions
ReactiveSystemExtension* reactive_system_create(const char* name, ActorSystem* actor_system);
void reactive_system_destroy(ReactiveSystemExtension* reactive_system);
Result_void_ptr reactive_system_start(ReactiveSystemExtension* reactive_system);
Result_void_ptr reactive_system_shutdown(ReactiveSystemExtension* reactive_system);

// Reactive actor creation (specialized wrapper around existing actor functions)
Actor* reactive_actor_create(ReactiveSystemExtension* reactive_system, const char* name, 
                            ReactiveMessageHandler handler, void* initial_state, size_t state_size);

// Reactive message sending (wrapper around existing message functions)
Result_void_ptr reactive_send_event_message(Actor* sender, Actor* recipient, Event* event);
Result_void_ptr reactive_broadcast_event(ReactiveSystemExtension* reactive_system, Event* event);

// Event stream management
EventStream* event_stream_create(const char* name, EventType type);
void event_stream_destroy(EventStream* stream);
Result_void_ptr event_stream_publish(EventStream* stream, Event* event);
Result_void_ptr event_stream_close(EventStream* stream);

// Event management
Event* event_create(EventType type, const char* name, void* data, size_t data_size);
void event_destroy(Event* event);
Event* event_ref(Event* event);
void event_unref(Event* event);

// Event subscription
EventSubscription* event_subscription_create(EventStream* stream, const char* name, 
                                           SubscriptionConfig* config);
void event_subscription_destroy(EventSubscription* subscription);
Result_void_ptr event_subscription_pause(EventSubscription* subscription);
Result_void_ptr event_subscription_resume(EventSubscription* subscription);

// Reactive component management
ReactiveComponent* reactive_component_create(const char* name, ReactiveComponentConfig* config);
void reactive_component_destroy(ReactiveComponent* component);
Result_void_ptr reactive_component_add_input_stream(ReactiveComponent* component, EventStream* stream);
Result_void_ptr reactive_component_add_output_stream(ReactiveComponent* component, EventStream* stream);
Result_void_ptr reactive_component_start(ReactiveComponent* component);
Result_void_ptr reactive_component_stop(ReactiveComponent* component);

// Complex event processing
EventPattern* event_pattern_create(const char* name, EventFilter* filters, size_t filter_count,
                                 EventAggregator aggregator, uint64_t time_window_ms);
void event_pattern_destroy(EventPattern* pattern);
Result_void_ptr event_pattern_register(EventStream* stream, EventPattern* pattern);
Result_void_ptr event_pattern_unregister(EventStream* stream, EventPattern* pattern);

// Reactive annotations and macros

// Reactive component annotation
#define REACTIVE __attribute__((annotate("reactive")))

// Actor message handler macro
#define ACTOR_MESSAGE_HANDLER(name) \
    Result_void_ptr name(Actor* actor, Message* message, void* state)

// Event processor macro
#define EVENT_PROCESSOR(name) \
    Result_void_ptr name(Event* event, void* context)

// Event filter macro
#define EVENT_FILTER(name) \
    bool name(Event* event, void* context)

// Event aggregator macro
#define EVENT_AGGREGATOR(name) \
    Result_void_ptr name(Event** events, size_t event_count, void* context)

// Reactive component with automatic setup
#define REACTIVE_COMPONENT(component_name, config) \
    ReactiveComponent* component_name = reactive_component_create(#component_name, config); \
    for (bool __component_active = true; __component_active; \
         __component_active = false, reactive_component_stop(component_name), \
         reactive_component_destroy(component_name))

// Actor with automatic lifecycle management
#define WITH_ACTOR(actor_name, system, handler, state, state_size) \
    Actor* actor_name = actor_create(system, #actor_name, handler, state, state_size); \
    for (bool __actor_active = true; __actor_active; \
         __actor_active = false, actor_stop(actor_name), actor_destroy(actor_name))

// Event stream with automatic cleanup
#define WITH_EVENT_STREAM(stream_name, name, type) \
    EventStream* stream_name = event_stream_create(name, type); \
    for (bool __stream_active = true; __stream_active; \
         __stream_active = false, event_stream_close(stream_name), \
         event_stream_destroy(stream_name))

// Convenience macros for message sending
#define SEND_MESSAGE(sender, recipient, type, data, size) \
    actor_send_message(sender, recipient, type, data, size)

#define SEND_MESSAGE_ASYNC(sender, recipient, type, data, size, waker) \
    actor_send_message_async(sender, recipient, type, data, size, waker)

#define BROADCAST_MESSAGE(sender, type, data, size) \
    actor_broadcast_message(sender, type, data, size)

// Event emission macros
#define EMIT_EVENT(stream, type, name, data, size) \
    do { \
        Event* __event = event_create(type, name, data, size); \
        if (__event) { \
            event_stream_publish(stream, __event); \
            event_unref(__event); \
        } \
    } while(0)

#define EMIT_STATE_CHANGE(stream, state_name, old_state, new_state) \
    do { \
        struct { void* old_val; void* new_val; } __state_change = {old_state, new_state}; \
        EMIT_EVENT(stream, EVENT_TYPE_STATE_CHANGE, state_name, &__state_change, sizeof(__state_change)); \
    } while(0)

// Pattern matching for complex event processing
#define PATTERN_MATCH(pattern_name, filters, filter_count, aggregator, window_ms) \
    EventPattern* pattern_name = event_pattern_create(#pattern_name, filters, filter_count, \
                                                    aggregator, window_ms)

// Statistics and monitoring functions
typedef struct ReactiveSystemStats {
    uint64_t total_actors_created;
    uint64_t currently_active_actors;
    uint64_t total_messages_sent;
    uint64_t total_messages_processed;
    uint64_t dead_letter_count;
    uint64_t actor_restarts;
    uint64_t total_events_emitted;
    uint64_t total_events_processed;
    uint64_t active_subscriptions;
    uint64_t active_streams;
    uint64_t active_components;
    double average_message_processing_time_ms;
    double average_event_processing_time_ms;
} ReactiveSystemStats;

ReactiveSystemStats reactive_system_get_stats(ReactiveSystemExtension* reactive_system);
void reactive_system_print_stats(ReactiveSystemExtension* reactive_system);
void event_stream_print_stats(EventStream* stream);
void reactive_component_print_stats(ReactiveComponent* component);

// Integration with transparent async system
Result_void_ptr reactive_system_integrate_async(ReactiveSystemExtension* reactive_system, void* async_context);
Result_void_ptr event_stream_integrate_async(EventStream* stream, void* async_context);

#endif // GOO_REACTIVE_PROGRAMMING_H