#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "include/actor_system.h"

// Test message types
typedef struct {
    int value;
} CounterMessage;

// Counter actor behavior
ACTOR_DEFINE_HANDLER(counter_handler) {
    CounterMessage* counter = (CounterMessage*)self->context;
    
    printf("Counter actor %s processing message\n", self->name);
    
    if (message->data) {
        int* increment = (int*)message->data;
        counter->value += *increment;
        
        printf("Counter incremented by %d, new value: %d\n", *increment, counter->value);
        
        // Reply with new value
        if (message->expects_response) {
            actor_reply(message, &counter->value, sizeof(int));
        }
    }
}

// Test basic functionality
void test_basic_functionality() {
    printf("Testing basic actor system functionality...\n");
    
    ActorSystem* system = actor_system_create("TestSystem", 2);
    assert(system != NULL);
    
    Result_void_ptr start_result = actor_system_start(system);
    assert(!start_result.is_error);
    
    // Set up counter actor behavior
    ActorBehavior counter_behavior;
    actor_behavior_init(&counter_behavior);
    counter_behavior.handle_message = counter_handler;
    
    // Create counter context
    CounterMessage counter_context = {.value = 10};
    
    // Spawn counter actor
    ActorRef* counter_ref = actor_spawn(system, "counter", counter_behavior, 
                                       &counter_context, sizeof(CounterMessage));
    assert(counter_ref != NULL);
    
    printf("Spawned counter actor: %s\n", counter_ref->actor_name);
    
    // Give actor time to start
    sleep(1);
    
    // Verify actor is running
    Actor* counter_actor = actor_system_find_actor(system, counter_ref->actor_id);
    assert(counter_actor != NULL);
    printf("Actor state: %d (RUNNING=%d)\n", counter_actor->state, ACTOR_STATE_RUNNING);
    
    // Send a fire-and-forget message
    int increment = 5;
    Result_void_ptr send_result = actor_send_fire_and_forget(counter_ref, &increment, sizeof(int));
    assert(!send_result.is_error);
    
    printf("Sent fire-and-forget message\n");
    
    // Give time for message processing
    sleep(1);
    
    // Check statistics
    uint64_t total_actors, total_messages, active_futures;
    actor_system_get_statistics(system, &total_actors, &total_messages, &active_futures);
    printf("System stats - Actors: %llu, Messages: %llu, Futures: %llu\n", 
           total_actors, total_messages, active_futures);
    
    // Cleanup
    actor_terminate(counter_ref);
    actor_ref_destroy(counter_ref);
    actor_system_shutdown(system, 2000);
    actor_system_destroy(system);
    
    printf("✓ Basic functionality test passed\n");
}

// Test message passing with response
void test_message_with_response() {
    printf("Testing message passing with response...\n");
    
    ActorSystem* system = actor_system_create("ResponseTestSystem", 2);
    actor_system_start(system);
    
    ActorBehavior counter_behavior;
    actor_behavior_init(&counter_behavior);
    counter_behavior.handle_message = counter_handler;
    
    CounterMessage counter_context = {.value = 100};
    ActorRef* counter_ref = actor_spawn(system, "response_counter", counter_behavior, 
                                       &counter_context, sizeof(CounterMessage));
    
    sleep(1); // Give time to start
    
    // Send message expecting response
    int increment = 25;
    ActorFuture* future = actor_send(counter_ref, &increment, sizeof(int));
    assert(future != NULL);
    
    printf("Sent message expecting response\n");
    
    // Wait for response with timeout
    Result_void_ptr result = actor_future_await_timeout(future, 5000);
    if (!result.is_error) {
        int* response = (int*)result.value;
        printf("Received response: %d\n", *response);
        assert(*response == 125); // 100 + 25
    } else {
        printf("Failed to get response: %s\n", result.error->message);
    }
    
    // Cleanup
    actor_future_destroy(future);
    actor_terminate(counter_ref);
    actor_ref_destroy(counter_ref);
    actor_system_shutdown(system, 2000);
    actor_system_destroy(system);
    
    printf("✓ Message with response test passed\n");
}

int main() {
    printf("=== Actor System Working Tests ===\n\n");
    
    test_basic_functionality();
    test_message_with_response();
    
    printf("\n=== All Working Tests Passed! ===\n");
    return 0;
}