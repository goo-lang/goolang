#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include "../include/reactive_programming.h"

// NOTE ON API MIGRATION
// ---------------------
// This demo originally targeted an older actor API (MESSAGE_TYPE_INIT/USER/STOP,
// Message.data/data_size, Actor.state_size, actor_create/actor_send_message/
// actor_destroy/actor_system_print_stats, single-arg actor_system_shutdown, ...).
// None of those symbols exist in the current tree. The reactive demo links only
// against reactive_programming_simple.c + async_resource.c + error.c, which provide
// the event-stream / reactive-component / async-resource API but NO actor runtime.
//
// The current message contract (include/fearless_concurrency.h) is:
//   - MessageType: MSG_TYPE_USER, MSG_TYPE_SYSTEM, MSG_TYPE_TERMINATE, ...
//   - struct Message: .type, .payload, .payload_size  (NOT .data/.data_size)
//   - ACTOR_MESSAGE_HANDLER(name) -> Result_void_ptr name(Actor*, Message*, void* state)
//   - per-actor state is passed to the handler as the `void* state` argument.
//
// Because no actor-system runtime is linkable here, the actor demos (1 and 4) drive
// the message handlers directly: they build Message structs and invoke the handlers,
// which faithfully demonstrates the message-passing/handler dispatch intent without
// inventing library functions. Demos 2, 3 and 5 use the real reactive/event-stream and
// async-resource APIs, which are fully implemented in the link set.

// Demo state structures
typedef struct UserProfile {
    uint64_t user_id;
    char username[32];
    char email[64];
    bool is_online;
    uint64_t last_activity_time;
} UserProfile;

typedef struct OrderData {
    uint64_t order_id;
    uint64_t user_id;
    double amount;
    char status[16];
    uint64_t created_time;
} OrderData;

typedef struct NotificationData {
    uint64_t notification_id;
    uint64_t target_user_id;
    char message[128];
    char type[16];
    bool is_sent;
} NotificationData;

// Handler signature matches ACTOR_MESSAGE_HANDLER expansion:
//   Result_void_ptr name(Actor* actor, Message* message, void* state)
typedef Result_void_ptr (*ServiceHandler)(Actor* actor, Message* message, void* state);

// Lightweight stand-in for a service actor: bundles a handler with its state.
// This lets the demo dispatch messages to "actors" without an actor runtime.
typedef struct ServiceActor {
    char name[64];
    ServiceHandler handler;
    void* state;
    size_t state_size;
    int restart_count;
} ServiceActor;

// Global state for demos
static int user_service_messages = 0;
static int order_service_messages = 0;
static int notification_service_messages = 0;
static int events_processed = 0;

// User Service Actor Handler
ACTOR_MESSAGE_HANDLER(user_service_handler) {
    (void)actor;
    user_service_messages++;

    printf("👤 UserService received message: Type %d (Total: %d)\n",
           message->type, user_service_messages);

    switch (message->type) {
        case MSG_TYPE_SYSTEM:
            printf("   🎬 UserService initialized\n");
            break;

        case MSG_TYPE_USER: {
            if (message->payload_size >= sizeof(UserProfile)) {
                UserProfile* profile = (UserProfile*)message->payload;
                printf("   📊 Processing user profile: %s (ID: %llu)\n",
                       profile->username, (unsigned long long)profile->user_id);

                // Simulate user processing
                usleep(10000); // 10ms processing time

                // Update user state in actor state
                if (state && message->payload_size >= sizeof(UserProfile)) {
                    memcpy(state, profile, sizeof(UserProfile));
                }
            }
            break;
        }

        case MSG_TYPE_TERMINATE:
            printf("   🛑 UserService shutting down\n");
            break;

        default:
            printf("   ❓ Unknown message type in UserService\n");
            break;
    }

    return OK_PTR(message);
}

// Order Service Actor Handler
ACTOR_MESSAGE_HANDLER(order_service_handler) {
    (void)actor;
    (void)state;
    order_service_messages++;

    printf("🛒 OrderService received message: Type %d (Total: %d)\n",
           message->type, order_service_messages);

    switch (message->type) {
        case MSG_TYPE_SYSTEM:
            printf("   🎬 OrderService initialized\n");
            break;

        case MSG_TYPE_USER: {
            if (message->payload_size >= sizeof(OrderData)) {
                OrderData* order = (OrderData*)message->payload;
                printf("   💰 Processing order: ID %llu, Amount $%.2f, Status: %s\n",
                       (unsigned long long)order->order_id, order->amount, order->status);

                // Simulate order processing
                usleep(15000); // 15ms processing time

                // Update order status
                if (strcmp(order->status, "pending") == 0) {
                    strcpy(order->status, "processing");
                    printf("   ⏳ Order status updated to: processing\n");
                }
            }
            break;
        }

        case MSG_TYPE_TERMINATE:
            printf("   🛑 OrderService shutting down\n");
            break;

        default:
            printf("   ❓ Unknown message type in OrderService\n");
            break;
    }

    return OK_PTR(message);
}

// Notification Service Actor Handler
ACTOR_MESSAGE_HANDLER(notification_service_handler) {
    (void)actor;
    (void)state;
    notification_service_messages++;

    printf("🔔 NotificationService received message: Type %d (Total: %d)\n",
           message->type, notification_service_messages);

    switch (message->type) {
        case MSG_TYPE_SYSTEM:
            printf("   🎬 NotificationService initialized\n");
            break;

        case MSG_TYPE_USER: {
            if (message->payload_size >= sizeof(NotificationData)) {
                NotificationData* notification = (NotificationData*)message->payload;
                printf("   📧 Sending notification: %s (Type: %s, User: %llu)\n",
                       notification->message, notification->type,
                       (unsigned long long)notification->target_user_id);

                // Simulate notification sending
                usleep(5000); // 5ms sending time
                notification->is_sent = true;
                printf("   ✅ Notification sent successfully\n");
            }
            break;
        }

        case MSG_TYPE_TERMINATE:
            printf("   🛑 NotificationService shutting down\n");
            break;

        default:
            printf("   ❓ Unknown message type in NotificationService\n");
            break;
    }

    return OK_PTR(message);
}

// Helper: dispatch a message to a ServiceActor by invoking its handler directly.
// This stands in for actor_send_message in an environment without an actor runtime.
static void service_actor_send(ServiceActor* actor, MessageType type,
                               void* payload, size_t payload_size) {
    if (!actor || !actor->handler) return;

    Message message = {0};
    message.type = type;
    message.message_id = (uint64_t)time(NULL);
    message.timestamp = (uint64_t)time(NULL);
    message.payload = payload;
    message.payload_size = payload_size;
    message.message_name = actor->name;

    actor->handler(NULL, &message, actor->state);
}

// Event processors for reactive components
EVENT_PROCESSOR(user_event_processor) {
    (void)context;
    events_processed++;

    printf("⚡ Processing user event: %s (ID: %llu, Count: %d)\n",
           event->name, (unsigned long long)event->id, events_processed);

    if (event->type == EVENT_TYPE_STATE_CHANGE && event->data) {
        UserProfile* profile = (UserProfile*)event->data;
        printf("   👤 User state changed: %s, Online: %s\n",
               profile->username, profile->is_online ? "Yes" : "No");
    }

    return OK_PTR(event);
}

EVENT_PROCESSOR(order_event_processor) {
    (void)context;
    events_processed++;

    printf("⚡ Processing order event: %s (ID: %llu, Count: %d)\n",
           event->name, (unsigned long long)event->id, events_processed);

    if (event->type == EVENT_TYPE_USER_ACTION && event->data) {
        OrderData* order = (OrderData*)event->data;
        printf("   🛒 Order event: ID %llu, Amount $%.2f, Status: %s\n",
               (unsigned long long)order->order_id, order->amount, order->status);
    }

    return OK_PTR(event);
}

// Event filters
EVENT_FILTER(high_value_order_filter) {
    (void)context;
    if (!event || event->type != EVENT_TYPE_USER_ACTION) return false;

    if (event->data && event->data_size >= sizeof(OrderData)) {
        OrderData* order = (OrderData*)event->data;
        return order->amount >= 100.0; // Filter for orders >= $100
    }

    return false;
}

EVENT_FILTER(online_user_filter) {
    (void)context;
    if (!event || event->type != EVENT_TYPE_STATE_CHANGE) return false;

    if (event->data && event->data_size >= sizeof(UserProfile)) {
        UserProfile* profile = (UserProfile*)event->data;
        return profile->is_online; // Filter for online users only
    }

    return false;
}

// Demo 1: Basic Actor System with Message Passing
void demo_basic_actor_system(void) {
    printf("\n🎭 Demo 1: Basic Actor System with Message Passing\n");
    printf("==================================================\n");

    // Create service actors with initial state.
    // (No actor runtime is linkable here, so we model actors as handler+state pairs
    //  and dispatch messages directly.)
    UserProfile user_state = {0};
    OrderData order_state = {0};
    NotificationData notification_state = {0};

    ServiceActor user_service = {
        .handler = user_service_handler,
        .state = &user_state,
        .state_size = sizeof(UserProfile)
    };
    strcpy(user_service.name, "user_service");

    ServiceActor order_service = {
        .handler = order_service_handler,
        .state = &order_state,
        .state_size = sizeof(OrderData)
    };
    strcpy(order_service.name, "order_service");

    ServiceActor notification_service = {
        .handler = notification_service_handler,
        .state = &notification_state,
        .state_size = sizeof(NotificationData)
    };
    strcpy(notification_service.name, "notification_service");

    // "Start" actors via a lifecycle (system) message.
    service_actor_send(&user_service, MSG_TYPE_SYSTEM, NULL, 0);
    service_actor_send(&order_service, MSG_TYPE_SYSTEM, NULL, 0);
    service_actor_send(&notification_service, MSG_TYPE_SYSTEM, NULL, 0);

    printf("✅ All service actors started\n");

    // Simulate user registration
    UserProfile new_user = {
        .user_id = 1001,
        .is_online = true,
        .last_activity_time = (uint64_t)time(NULL)
    };
    strcpy(new_user.username, "alice_smith");
    strcpy(new_user.email, "alice@example.com");

    printf("\n👤 Simulating user registration...\n");
    service_actor_send(&user_service, MSG_TYPE_USER, &new_user, sizeof(UserProfile));

    // Simulate order creation
    OrderData new_order = {
        .order_id = 2001,
        .user_id = 1001,
        .amount = 149.99,
        .created_time = (uint64_t)time(NULL)
    };
    strcpy(new_order.status, "pending");

    printf("\n🛒 Simulating order creation...\n");
    service_actor_send(&order_service, MSG_TYPE_USER, &new_order, sizeof(OrderData));

    // Simulate notification
    NotificationData notification = {
        .notification_id = 3001,
        .target_user_id = 1001,
        .is_sent = false
    };
    strcpy(notification.message, "Welcome to our e-commerce platform!");
    strcpy(notification.type, "welcome");

    printf("\n🔔 Simulating notification...\n");
    service_actor_send(&notification_service, MSG_TYPE_USER, &notification, sizeof(NotificationData));

    // Print message statistics
    printf("\n📊 Service Statistics:\n");
    printf("  user_service messages: %d\n", user_service_messages);
    printf("  order_service messages: %d\n", order_service_messages);
    printf("  notification_service messages: %d\n", notification_service_messages);

    // "Stop" actors via a terminate message.
    printf("\n🧹 Cleaning up actor system...\n");
    service_actor_send(&user_service, MSG_TYPE_TERMINATE, NULL, 0);
    service_actor_send(&order_service, MSG_TYPE_TERMINATE, NULL, 0);
    service_actor_send(&notification_service, MSG_TYPE_TERMINATE, NULL, 0);

    printf("✅ Demo 1 completed - Basic actor system demonstrated\n");
}

// Demo 2: Event Streams and Reactive Components
void demo_event_streams_and_reactive_components(void) {
    printf("\n📡 Demo 2: Event Streams and Reactive Components\n");
    printf("===============================================\n");

    // Create event streams for different types of events
    EventStream* user_events = event_stream_create("user_events", EVENT_TYPE_STATE_CHANGE);
    EventStream* order_events = event_stream_create("order_events", EVENT_TYPE_USER_ACTION);
    EventStream* notification_events = event_stream_create("notification_events", EVENT_TYPE_CUSTOM);

    assert(user_events != NULL);
    assert(order_events != NULL);
    assert(notification_events != NULL);

    printf("✅ Created event streams\n");

    // Create reactive components
    ReactiveComponentConfig user_component_config = {
        .auto_start = true,
        .enable_metrics = true,
        .processing_timeout_ms = 5000,
        .max_event_buffer_size = 1000
    };
    strcpy(user_component_config.name, "user_analytics");

    ReactiveComponentConfig order_component_config = {
        .auto_start = true,
        .enable_metrics = true,
        .processing_timeout_ms = 5000,
        .max_event_buffer_size = 1000
    };
    strcpy(order_component_config.name, "order_processor");

    ReactiveComponent* user_analytics = reactive_component_create("user_analytics", &user_component_config);
    ReactiveComponent* order_processor = reactive_component_create("order_processor", &order_component_config);

    assert(user_analytics != NULL);
    assert(order_processor != NULL);

    // Set event processors
    user_analytics->process_event = user_event_processor;
    order_processor->process_event = order_event_processor;

    // Start components
    reactive_component_start(user_analytics);
    reactive_component_start(order_processor);

    printf("✅ Created and started reactive components\n");

    // Create test users and orders
    UserProfile users[] = {
        {1001, "alice_smith", "alice@example.com", true, (uint64_t)time(NULL)},
        {1002, "bob_jones", "bob@example.com", false, (uint64_t)time(NULL) - 3600},
        {1003, "carol_wilson", "carol@example.com", true, (uint64_t)time(NULL) - 1800}
    };

    OrderData orders[] = {
        {2001, 1001, 149.99, "pending", (uint64_t)time(NULL)},
        {2002, 1002, 79.50, "pending", (uint64_t)time(NULL) - 300},
        {2003, 1003, 299.99, "pending", (uint64_t)time(NULL) - 600}
    };

    // Simulate user events. The reactive component's processor is invoked directly
    // for each published event (the simple reactive backend has no dispatch thread).
    printf("\n👥 Publishing user events...\n");
    for (int i = 0; i < 3; i++) {
        char event_name[64];
        snprintf(event_name, sizeof(event_name), "user_state_change_%d", i + 1);

        Event* user_event = event_create(EVENT_TYPE_STATE_CHANGE, event_name,
                                        &users[i], sizeof(UserProfile));
        event_stream_publish(user_events, user_event);
        if (user_analytics->process_event) {
            user_analytics->process_event(user_event, user_analytics->processor_context);
        }
        event_unref(user_event);
    }

    // Simulate order events
    printf("\n🛒 Publishing order events...\n");
    for (int i = 0; i < 3; i++) {
        char event_name[64];
        snprintf(event_name, sizeof(event_name), "order_created_%d", i + 1);

        Event* order_event = event_create(EVENT_TYPE_USER_ACTION, event_name,
                                         &orders[i], sizeof(OrderData));
        event_stream_publish(order_events, order_event);
        if (order_processor->process_event) {
            order_processor->process_event(order_event, order_processor->processor_context);
        }
        event_unref(order_event);
    }

    // Print stream statistics
    printf("\n📊 Event Stream Statistics:\n");
    event_stream_print_stats(user_events);
    event_stream_print_stats(order_events);

    // Cleanup
    printf("\n🧹 Cleaning up streams and components...\n");
    reactive_component_stop(user_analytics);
    reactive_component_stop(order_processor);
    reactive_component_destroy(user_analytics);
    reactive_component_destroy(order_processor);

    event_stream_close(user_events);
    event_stream_close(order_events);
    event_stream_close(notification_events);
    event_stream_destroy(user_events);
    event_stream_destroy(order_events);
    event_stream_destroy(notification_events);

    printf("✅ Demo 2 completed - Event streams and reactive components demonstrated\n");
}

// Demo 3: Complex Event Processing with Filters and Patterns
void demo_complex_event_processing(void) {
    printf("\n🎯 Demo 3: Complex Event Processing with Filters\n");
    printf("===============================================\n");

    // Create event streams
    EventStream* all_orders = event_stream_create("all_orders", EVENT_TYPE_USER_ACTION);
    EventStream* user_activity = event_stream_create("user_activity", EVENT_TYPE_STATE_CHANGE);

    assert(all_orders != NULL);
    assert(user_activity != NULL);

    // Set up filters
    all_orders->global_filter = high_value_order_filter;
    user_activity->global_filter = online_user_filter;

    printf("✅ Created filtered event streams\n");

    // Create test data
    OrderData test_orders[] = {
        {3001, 1001, 25.99, "pending", (uint64_t)time(NULL)},      // Low value - should be filtered
        {3002, 1002, 150.00, "pending", (uint64_t)time(NULL)},    // High value - should pass
        {3003, 1003, 89.99, "pending", (uint64_t)time(NULL)},     // Low value - should be filtered
        {3004, 1001, 299.99, "pending", (uint64_t)time(NULL)},   // High value - should pass
        {3005, 1002, 45.50, "pending", (uint64_t)time(NULL)}      // Low value - should be filtered
    };

    UserProfile test_users[] = {
        {1001, "active_user1", "user1@example.com", true, (uint64_t)time(NULL)},   // Online - should pass
        {1002, "inactive_user", "user2@example.com", false, (uint64_t)time(NULL)}, // Offline - should be filtered
        {1003, "active_user2", "user3@example.com", true, (uint64_t)time(NULL)},   // Online - should pass
        {1004, "away_user", "user4@example.com", false, (uint64_t)time(NULL)}      // Offline - should be filtered
    };

    // Publish orders (the global filter classifies high-value vs low-value orders).
    printf("\n💰 Publishing orders (filtering for high-value orders >= $100)...\n");
    for (int i = 0; i < 5; i++) {
        char event_name[64];
        snprintf(event_name, sizeof(event_name), "order_event_%d", i + 1);

        Event* order_event = event_create(EVENT_TYPE_USER_ACTION, event_name,
                                         &test_orders[i], sizeof(OrderData));

        printf("   📦 Publishing order: $%.2f ", test_orders[i].amount);

        bool passes = all_orders->global_filter
                          ? all_orders->global_filter(order_event, all_orders->filter_context)
                          : true;

        Result_void_ptr result = event_stream_publish(all_orders, order_event);
        if (!result.is_error) {
            if (passes) {
                printf("✅ (High value - processed)\n");
            } else {
                printf("🔍 (Low value - but published)\n");
            }
        }

        event_unref(order_event);
    }

    // Publish user activities (the global filter classifies online vs offline users).
    printf("\n👥 Publishing user activities (filtering for online users only)...\n");
    for (int i = 0; i < 4; i++) {
        char event_name[64];
        snprintf(event_name, sizeof(event_name), "user_activity_%d", i + 1);

        Event* user_event = event_create(EVENT_TYPE_STATE_CHANGE, event_name,
                                        &test_users[i], sizeof(UserProfile));

        printf("   👤 Publishing user: %s (Online: %s) ",
               test_users[i].username, test_users[i].is_online ? "Yes" : "No");

        bool passes = user_activity->global_filter
                          ? user_activity->global_filter(user_event, user_activity->filter_context)
                          : true;

        Result_void_ptr result = event_stream_publish(user_activity, user_event);
        if (!result.is_error) {
            if (passes) {
                printf("✅ (Online - processed)\n");
            } else {
                printf("🔍 (Offline - but published)\n");
            }
        }

        event_unref(user_event);
    }

    // Print stream statistics
    printf("\n📊 Filtered Stream Statistics:\n");
    printf("All Orders Stream:\n");
    printf("  Total events: %zu\n", all_orders->event_count);
    printf("  Events dropped: %llu\n", (unsigned long long)all_orders->dropped_events);

    printf("User Activity Stream:\n");
    printf("  Total events: %zu\n", user_activity->event_count);
    printf("  Events dropped: %llu\n", (unsigned long long)user_activity->dropped_events);

    // Cleanup
    printf("\n🧹 Cleaning up filtered streams...\n");
    event_stream_close(all_orders);
    event_stream_close(user_activity);
    event_stream_destroy(all_orders);
    event_stream_destroy(user_activity);

    printf("✅ Demo 3 completed - Complex event processing with filters demonstrated\n");
}

// Demo 4: Actor Hierarchy and Supervision
void demo_actor_hierarchy_and_supervision(void) {
    printf("\n👨‍👩‍👧‍👦 Demo 4: Actor Hierarchy and Supervision\n");
    printf("==========================================\n");

    // Model a supervisor and its child service actors as handler+state pairs.
    ServiceActor supervisor = { .handler = user_service_handler };
    strcpy(supervisor.name, "application_supervisor");

    ServiceActor user_service = { .handler = user_service_handler };
    strcpy(user_service.name, "user_service_child");
    ServiceActor order_service = { .handler = order_service_handler };
    strcpy(order_service.name, "order_service_child");
    ServiceActor notification_service = { .handler = notification_service_handler };
    strcpy(notification_service.name, "notification_service_child");

    printf("✅ Created supervisor and child actors\n");

    // "Start" the children via lifecycle messages.
    service_actor_send(&supervisor, MSG_TYPE_SYSTEM, NULL, 0);
    service_actor_send(&user_service, MSG_TYPE_SYSTEM, NULL, 0);
    service_actor_send(&order_service, MSG_TYPE_SYSTEM, NULL, 0);
    service_actor_send(&notification_service, MSG_TYPE_SYSTEM, NULL, 0);

    printf("🔗 Setting up actor hierarchy...\n");

    // Demonstrate actor restart with a bounded restart limit (max 2).
    const int max_restart_attempts = 2;
    printf("\n🔄 Demonstrating actor restart...\n");
    printf("Initial restart count for user_service: %d\n", user_service.restart_count);

    for (int attempt = 1; attempt <= 3; attempt++) {
        if (user_service.restart_count < max_restart_attempts) {
            user_service.restart_count++;
            printf("✅ Restart #%d successful. Count: %d\n", attempt, user_service.restart_count);
        } else {
            printf("❌ Restart #%d failed (limit exceeded). Count: %d\n",
                   attempt, user_service.restart_count);
        }
    }

    // Send messages to demonstrate continued operation
    printf("\n📬 Sending messages to demonstrate operation...\n");
    UserProfile test_user = {9001, "test_user", "test@example.com", true, (uint64_t)time(NULL)};
    service_actor_send(&user_service, MSG_TYPE_USER, &test_user, sizeof(UserProfile));

    OrderData test_order = {9002, 9001, 199.99, "pending", (uint64_t)time(NULL)};
    service_actor_send(&order_service, MSG_TYPE_USER, &test_order, sizeof(OrderData));

    // Print statistics
    printf("\n📊 Hierarchical System Statistics:\n");
    printf("  user_service messages: %d (restarts: %d)\n",
           user_service_messages, user_service.restart_count);
    printf("  order_service messages: %d\n", order_service_messages);

    // "Stop" actors via terminate messages.
    printf("\n🧹 Cleaning up hierarchical system...\n");
    service_actor_send(&user_service, MSG_TYPE_TERMINATE, NULL, 0);
    service_actor_send(&order_service, MSG_TYPE_TERMINATE, NULL, 0);
    service_actor_send(&notification_service, MSG_TYPE_TERMINATE, NULL, 0);
    service_actor_send(&supervisor, MSG_TYPE_TERMINATE, NULL, 0);

    printf("✅ Demo 4 completed - Actor hierarchy and supervision demonstrated\n");
}

// Demo 5: Integration with Async Resource Management
void demo_async_integration(void) {
    printf("\n🔗 Demo 5: Integration with Async Resource Management\n");
    printf("===================================================\n");

    // Initialize async resource management
    Result_void_ptr init_result = async_resource_manager_init();
    if (init_result.is_error) {
        printf("❌ Failed to initialize async resource manager\n");
        return;
    }

    // Create event streams with async integration
    EventStream* async_stream = event_stream_create("async_integrated_stream", EVENT_TYPE_CUSTOM);
    event_stream_integrate_async(async_stream, NULL);

    printf("✅ Created streams with async integration\n");

    // Model an async-integrated service actor.
    ServiceActor async_actor = { .handler = user_service_handler };
    strcpy(async_actor.name, "async_integrated_actor");
    service_actor_send(&async_actor, MSG_TYPE_SYSTEM, NULL, 0);

    // Demonstrate resource-aware processing
    WITH_ASYNC_RESOURCE_SCOPE(reactive_scope, NULL) {
        printf("📦 Operating within async resource scope...\n");

        // Create memory resource for event processing
        AsyncResource* event_buffer = async_memory_resource_create("event_buffer", 4096, 0);
        if (event_buffer) {
            async_resource_scope_add(reactive_scope, event_buffer);

            if (!async_resource_acquire(event_buffer, 1000).is_error) {
                printf("   💾 Acquired event buffer memory resource\n");

                // Simulate event processing with resource management
                char* test_event_data = "Async integrated event data";
                Event* async_event = event_create(EVENT_TYPE_CUSTOM, "async_event",
                                                 test_event_data, strlen(test_event_data) + 1);

                event_stream_publish(async_stream, async_event);
                printf("   📡 Published event with async resource management\n");

                event_unref(async_event);
            }

            async_resource_unref(event_buffer);
        }

        // Send message to async-integrated actor
        char* message_data = "Hello from async context!";
        service_actor_send(&async_actor, MSG_TYPE_USER,
                           message_data, strlen(message_data) + 1);

        printf("   📬 Sent message to async-integrated actor\n");

        printf("🔚 Exiting async resource scope - automatic cleanup\n");
    }

    // Cleanup
    printf("\n🧹 Cleaning up integrated system...\n");
    service_actor_send(&async_actor, MSG_TYPE_TERMINATE, NULL, 0);

    event_stream_close(async_stream);
    event_stream_destroy(async_stream);

    // Shutdown async resource manager
    async_resource_manager_shutdown();

    printf("✅ Demo 5 completed - Async integration demonstrated\n");
}

int main(void) {
    printf("🌟 Reactive Programming and Event Handling Demonstration\n");
    printf("========================================================\n");
    printf("This demo showcases the reactive programming model with actors,\n");
    printf("event streams, and complex event processing capabilities.\n");

    // Reset global counters
    user_service_messages = 0;
    order_service_messages = 0;
    notification_service_messages = 0;
    events_processed = 0;

    // Run all demos
    demo_basic_actor_system();
    demo_event_streams_and_reactive_components();
    demo_complex_event_processing();
    demo_actor_hierarchy_and_supervision();
    demo_async_integration();

    // Final statistics
    printf("\n📈 Overall Demo Statistics\n");
    printf("=========================\n");
    printf("User Service Messages Processed: %d\n", user_service_messages);
    printf("Order Service Messages Processed: %d\n", order_service_messages);
    printf("Notification Service Messages Processed: %d\n", notification_service_messages);
    printf("Total Events Processed: %d\n", events_processed);

    printf("\n🎉 All reactive programming demos completed successfully!\n");
    printf("💡 Key features demonstrated:\n");
    printf("   • Actor model with message passing between services\n");
    printf("   • Event streams for reactive data flow\n");
    printf("   • Reactive components for event processing\n");
    printf("   • Complex event processing with filters and patterns\n");
    printf("   • Actor hierarchy and supervision strategies\n");
    printf("   • Integration with async resource management\n");
    printf("   • Automatic resource cleanup and lifecycle management\n");
    printf("   • Error handling and restart mechanisms\n");
    printf("   • Real-time event processing and correlation\n");

    return 0;
}