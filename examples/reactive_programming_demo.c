#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "../include/reactive_programming.h"

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

// Global state for demos
static int user_service_messages = 0;
static int order_service_messages = 0;
static int notification_service_messages = 0;
static int events_processed = 0;

// User Service Actor Handler
ACTOR_MESSAGE_HANDLER(user_service_handler) {
    user_service_messages++;
    
    printf("👤 UserService received message: Type %d (Total: %d)\n", 
           message->type, user_service_messages);
    
    switch (message->type) {
        case MESSAGE_TYPE_INIT:
            printf("   🎬 UserService initialized\n");
            break;
            
        case MESSAGE_TYPE_USER: {
            if (message->data_size >= sizeof(UserProfile)) {
                UserProfile* profile = (UserProfile*)message->data;
                printf("   📊 Processing user profile: %s (ID: %llu)\n", 
                       profile->username, profile->user_id);
                
                // Simulate user processing
                usleep(10000); // 10ms processing time
                
                // Update user state in actor state
                if (state && actor->state_size >= sizeof(UserProfile)) {
                    memcpy(state, profile, sizeof(UserProfile));
                }
            }
            break;
        }
        
        case MESSAGE_TYPE_STOP:
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
    order_service_messages++;
    
    printf("🛒 OrderService received message: Type %d (Total: %d)\n", 
           message->type, order_service_messages);
    
    switch (message->type) {
        case MESSAGE_TYPE_INIT:
            printf("   🎬 OrderService initialized\n");
            break;
            
        case MESSAGE_TYPE_USER: {
            if (message->data_size >= sizeof(OrderData)) {
                OrderData* order = (OrderData*)message->data;
                printf("   💰 Processing order: ID %llu, Amount $%.2f, Status: %s\n", 
                       order->order_id, order->amount, order->status);
                
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
        
        case MESSAGE_TYPE_STOP:
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
    notification_service_messages++;
    
    printf("🔔 NotificationService received message: Type %d (Total: %d)\n", 
           message->type, notification_service_messages);
    
    switch (message->type) {
        case MESSAGE_TYPE_INIT:
            printf("   🎬 NotificationService initialized\n");
            break;
            
        case MESSAGE_TYPE_USER: {
            if (message->data_size >= sizeof(NotificationData)) {
                NotificationData* notification = (NotificationData*)message->data;
                printf("   📧 Sending notification: %s (Type: %s, User: %llu)\n", 
                       notification->message, notification->type, notification->target_user_id);
                
                // Simulate notification sending
                usleep(5000); // 5ms sending time
                notification->is_sent = true;
                printf("   ✅ Notification sent successfully\n");
            }
            break;
        }
        
        case MESSAGE_TYPE_STOP:
            printf("   🛑 NotificationService shutting down\n");
            break;
            
        default:
            printf("   ❓ Unknown message type in NotificationService\n");
            break;
    }
    
    return OK_PTR(message);
}

// Event processors for reactive components
EVENT_PROCESSOR(user_event_processor) {
    events_processed++;
    
    printf("⚡ Processing user event: %s (ID: %llu, Count: %d)\n", 
           event->name, event->id, events_processed);
    
    if (event->type == EVENT_TYPE_STATE_CHANGE && event->data) {
        UserProfile* profile = (UserProfile*)event->data;
        printf("   👤 User state changed: %s, Online: %s\n", 
               profile->username, profile->is_online ? "Yes" : "No");
    }
    
    return OK_PTR(event);
}

EVENT_PROCESSOR(order_event_processor) {
    events_processed++;
    
    printf("⚡ Processing order event: %s (ID: %llu, Count: %d)\n", 
           event->name, event->id, events_processed);
    
    if (event->type == EVENT_TYPE_USER_ACTION && event->data) {
        OrderData* order = (OrderData*)event->data;
        printf("   🛒 Order event: ID %llu, Amount $%.2f, Status: %s\n", 
               order->order_id, order->amount, order->status);
    }
    
    return OK_PTR(event);
}

// Event filters
EVENT_FILTER(high_value_order_filter) {
    if (!event || event->type != EVENT_TYPE_USER_ACTION) return false;
    
    if (event->data && event->data_size >= sizeof(OrderData)) {
        OrderData* order = (OrderData*)event->data;
        return order->amount >= 100.0; // Filter for orders >= $100
    }
    
    return false;
}

EVENT_FILTER(online_user_filter) {
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
    
    // Create actor system
    ActorSystem* system = actor_system_create("e_commerce_system");
    assert(system != NULL);
    
    // Start the system
    actor_system_start(system);
    
    // Create service actors with initial state
    UserProfile initial_user_state = {0};
    OrderData initial_order_state = {0};
    NotificationData initial_notification_state = {0};
    
    Actor* user_service = actor_create(system, "user_service", user_service_handler,
                                      &initial_user_state, sizeof(UserProfile));
    Actor* order_service = actor_create(system, "order_service", order_service_handler,
                                       &initial_order_state, sizeof(OrderData));
    Actor* notification_service = actor_create(system, "notification_service", notification_service_handler,
                                              &initial_notification_state, sizeof(NotificationData));
    
    assert(user_service != NULL);
    assert(order_service != NULL);
    assert(notification_service != NULL);
    
    // Start all actors
    actor_start(user_service);
    actor_start(order_service);
    actor_start(notification_service);
    
    printf("✅ All service actors started\n");
    
    // Simulate user registration
    UserProfile new_user = {
        .user_id = 1001,
        .username = "alice_smith",
        .email = "alice@example.com",
        .is_online = true,
        .last_activity_time = (uint64_t)time(NULL)
    };
    strcpy(new_user.username, "alice_smith");
    strcpy(new_user.email, "alice@example.com");
    
    printf("\n👤 Simulating user registration...\n");
    actor_send_message(NULL, user_service, MESSAGE_TYPE_USER, &new_user, sizeof(UserProfile));
    
    // Simulate order creation
    OrderData new_order = {
        .order_id = 2001,
        .user_id = 1001,
        .amount = 149.99,
        .created_time = (uint64_t)time(NULL)
    };
    strcpy(new_order.status, "pending");
    
    printf("\n🛒 Simulating order creation...\n");
    actor_send_message(NULL, order_service, MESSAGE_TYPE_USER, &new_order, sizeof(OrderData));
    
    // Simulate notification
    NotificationData notification = {
        .notification_id = 3001,
        .target_user_id = 1001,
        .is_sent = false
    };
    strcpy(notification.message, "Welcome to our e-commerce platform!");
    strcpy(notification.type, "welcome");
    
    printf("\n🔔 Simulating notification...\n");
    actor_send_message(NULL, notification_service, MESSAGE_TYPE_USER, &notification, sizeof(NotificationData));
    
    // Wait for message processing
    printf("\n⏳ Waiting for message processing...\n");
    usleep(100000); // 100ms
    
    // Print system statistics
    printf("\n📊 System Statistics:\n");
    actor_system_print_stats(system);
    
    // Cleanup
    printf("\n🧹 Cleaning up actor system...\n");
    actor_stop(user_service);
    actor_stop(order_service);
    actor_stop(notification_service);
    
    actor_destroy(user_service);
    actor_destroy(order_service);
    actor_destroy(notification_service);
    
    actor_system_shutdown(system);
    actor_system_destroy(system);
    
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
    
    // Simulate user events
    printf("\n👥 Publishing user events...\n");
    for (int i = 0; i < 3; i++) {
        char event_name[64];
        snprintf(event_name, sizeof(event_name), "user_state_change_%d", i + 1);
        
        Event* user_event = event_create(EVENT_TYPE_STATE_CHANGE, event_name, 
                                        &users[i], sizeof(UserProfile));
        event_stream_publish(user_events, user_event);
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
        event_unref(order_event);
    }
    
    // Wait for event processing
    printf("\n⏳ Waiting for event processing...\n");
    usleep(100000); // 100ms
    
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
    
    // Publish orders (only high-value orders should be processed)
    printf("\n💰 Publishing orders (filtering for high-value orders >= $100)...\n");
    for (int i = 0; i < 5; i++) {
        char event_name[64];
        snprintf(event_name, sizeof(event_name), "order_event_%d", i + 1);
        
        Event* order_event = event_create(EVENT_TYPE_USER_ACTION, event_name, 
                                         &test_orders[i], sizeof(OrderData));
        
        printf("   📦 Publishing order: $%.2f ", test_orders[i].amount);
        
        Result_void_ptr result = event_stream_publish(all_orders, order_event);
        if (!result.is_error) {
            if (test_orders[i].amount >= 100.0) {
                printf("✅ (High value - processed)\n");
            } else {
                printf("🔍 (Low value - but published)\n");
            }
        }
        
        event_unref(order_event);
    }
    
    // Publish user activities (only online users should be processed)
    printf("\n👥 Publishing user activities (filtering for online users only)...\n");
    for (int i = 0; i < 4; i++) {
        char event_name[64];
        snprintf(event_name, sizeof(event_name), "user_activity_%d", i + 1);
        
        Event* user_event = event_create(EVENT_TYPE_STATE_CHANGE, event_name, 
                                        &test_users[i], sizeof(UserProfile));
        
        printf("   👤 Publishing user: %s (Online: %s) ", 
               test_users[i].username, test_users[i].is_online ? "Yes" : "No");
        
        Result_void_ptr result = event_stream_publish(user_activity, user_event);
        if (!result.is_error) {
            if (test_users[i].is_online) {
                printf("✅ (Online - processed)\n");
            } else {
                printf("🔍 (Offline - but published)\n");
            }
        }
        
        event_unref(user_event);
    }
    
    // Wait for processing
    printf("\n⏳ Waiting for event processing...\n");
    usleep(50000); // 50ms
    
    // Print stream statistics
    printf("\n📊 Filtered Stream Statistics:\n");
    printf("All Orders Stream:\n");
    printf("  Total events: %zu\n", all_orders->event_count);
    printf("  Events dropped: %llu\n", all_orders->dropped_events);
    
    printf("User Activity Stream:\n");
    printf("  Total events: %zu\n", user_activity->event_count);
    printf("  Events dropped: %llu\n", user_activity->dropped_events);
    
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
    
    // Create actor system
    ActorSystem* system = actor_system_create("hierarchical_system");
    actor_system_start(system);
    
    // Create supervisor actor (parent)
    Actor* supervisor = actor_create(system, "application_supervisor", user_service_handler, NULL, 0);
    actor_start(supervisor);
    
    // Create child service actors
    Actor* user_service = actor_create(system, "user_service_child", user_service_handler, NULL, 0);
    Actor* order_service = actor_create(system, "order_service_child", order_service_handler, NULL, 0);
    Actor* notification_service = actor_create(system, "notification_service_child", notification_service_handler, NULL, 0);
    
    // Set up supervision configuration
    supervisor->config.supervision_strategy = SUPERVISION_RESTART;
    user_service->config.supervision_strategy = SUPERVISION_RESTART;
    order_service->config.supervision_strategy = SUPERVISION_RESTART;
    notification_service->config.supervision_strategy = SUPERVISION_RESTART;
    
    // Reduce restart limits for demo
    user_service->config.max_restart_attempts = 2;
    order_service->config.max_restart_attempts = 2;
    notification_service->config.max_restart_attempts = 2;
    
    printf("✅ Created supervisor and child actors\n");
    
    // Start child actors
    actor_start(user_service);
    actor_start(order_service);
    actor_start(notification_service);
    
    // Add children to supervisor (conceptually - hierarchy setup)
    printf("🔗 Setting up actor hierarchy...\n");
    user_service->parent = supervisor;
    order_service->parent = supervisor;
    notification_service->parent = supervisor;
    
    // Demonstrate actor restart
    printf("\n🔄 Demonstrating actor restart...\n");
    printf("Initial restart count for user_service: %d\n", user_service->restart_count);
    
    Result_void_ptr restart1 = actor_restart(user_service);
    if (!restart1.is_error) {
        printf("✅ First restart successful. Count: %d\n", user_service->restart_count);
    }
    
    Result_void_ptr restart2 = actor_restart(user_service);
    if (!restart2.is_error) {
        printf("✅ Second restart successful. Count: %d\n", user_service->restart_count);
    }
    
    Result_void_ptr restart3 = actor_restart(user_service);
    if (restart3.is_error) {
        printf("❌ Third restart failed (limit exceeded). Count: %d\n", user_service->restart_count);
    }
    
    // Send messages to demonstrate continued operation
    printf("\n📬 Sending messages to demonstrate operation...\n");
    UserProfile test_user = {9001, "test_user", "test@example.com", true, (uint64_t)time(NULL)};
    actor_send_message(supervisor, user_service, MESSAGE_TYPE_USER, &test_user, sizeof(UserProfile));
    
    OrderData test_order = {9002, 9001, 199.99, "pending", (uint64_t)time(NULL)};
    actor_send_message(supervisor, order_service, MESSAGE_TYPE_USER, &test_order, sizeof(OrderData));
    
    // Wait for processing
    usleep(50000); // 50ms
    
    // Print system statistics
    printf("\n📊 Hierarchical System Statistics:\n");
    actor_system_print_stats(system);
    
    // Cleanup
    printf("\n🧹 Cleaning up hierarchical system...\n");
    actor_stop(user_service);
    actor_stop(order_service);
    actor_stop(notification_service);
    actor_stop(supervisor);
    
    actor_destroy(user_service);
    actor_destroy(order_service);
    actor_destroy(notification_service);
    actor_destroy(supervisor);
    
    actor_system_shutdown(system);
    actor_system_destroy(system);
    
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
    
    // Create actor system with resource scope
    ActorSystem* system = actor_system_create("integrated_system");
    actor_system_start(system);
    
    // Integrate with async context
    Result_void_ptr integrate_result = reactive_system_integrate_async(system, NULL);
    if (!integrate_result.is_error) {
        printf("✅ Successfully integrated reactive system with async context\n");
    }
    
    // Create actors with async integration
    Actor* async_actor = actor_create(system, "async_integrated_actor", user_service_handler, NULL, 0);
    actor_integrate_async(async_actor, NULL);
    actor_start(async_actor);
    
    // Create event streams with async integration
    EventStream* async_stream = event_stream_create("async_integrated_stream", EVENT_TYPE_CUSTOM);
    event_stream_integrate_async(async_stream, NULL);
    
    printf("✅ Created actors and streams with async integration\n");
    
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
        actor_send_message(NULL, async_actor, MESSAGE_TYPE_USER, 
                          message_data, strlen(message_data) + 1);
        
        printf("   📬 Sent message to async-integrated actor\n");
        
        // Wait for processing
        usleep(50000); // 50ms
        
        printf("🔚 Exiting async resource scope - automatic cleanup\n");
    }
    
    // Cleanup
    printf("\n🧹 Cleaning up integrated system...\n");
    actor_stop(async_actor);
    actor_destroy(async_actor);
    
    event_stream_close(async_stream);
    event_stream_destroy(async_stream);
    
    actor_system_shutdown(system);
    actor_system_destroy(system);
    
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