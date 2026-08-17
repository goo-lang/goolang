#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include "include/actor_system.h"

// Test message types
typedef struct {
    int value;
} CounterMessage;

typedef struct {
    int increment;
} IncrementMessage;

typedef struct {
    char text[256];
} EchoMessage;

// Counter actor behavior
ACTOR_DEFINE_HANDLER(counter_handler) {
    CounterMessage* counter = (CounterMessage*)self->context;
    
    if (message->data) {
        IncrementMessage* inc_msg = (IncrementMessage*)message->data;
        counter->value += inc_msg->increment;
        
        printf("Counter actor %s: incremented by %d, new value: %d\n", 
               self->name, inc_msg->increment, counter->value);
        
        // Reply with new value
        if (message->expects_response) {
            actor_reply(message, &counter->value, sizeof(int));
        }
    }
}

// Echo actor behavior
ACTOR_DEFINE_HANDLER(echo_handler) {
    if (message->data) {
        EchoMessage* echo_msg = (EchoMessage*)message->data;
        printf("Echo actor %s: received '%s'\n", self->name, echo_msg->text);
        
        // Reply with the same message
        if (message->expects_response) {
            actor_reply(message, echo_msg, sizeof(EchoMessage));
        }
    }
}

// Test basic actor system creation
void test_actor_system_creation() {
    printf("Testing actor system creation...\n");
    
    ActorSystem* system = actor_system_create("TestSystem", 4);
    assert(system != NULL);
    assert(strcmp(system->name, "TestSystem") == 0);
    assert(system->thread_pool_size == 4);
    assert(!system->is_running);
    
    Result_void_ptr start_result = actor_system_start(system);
    assert(!start_result.is_error);
    assert(system->is_running);
    
    Result_void_ptr shutdown_result = actor_system_shutdown(system, 1000);
    assert(!shutdown_result.is_error);
    assert(!system->is_running);
    
    actor_system_destroy(system);
    
    printf("✓ Actor system creation test passed\n");
}

// Test actor spawning
void test_actor_spawning() {
    printf("Testing actor spawning...\n");
    
    ActorSystem* system = actor_system_create("SpawnTestSystem", 2);
    actor_system_start(system);
    
    // Set up counter actor behavior
    ActorBehavior counter_behavior;
    actor_behavior_init(&counter_behavior);
    counter_behavior.handle_message = counter_handler;
    
    // Create counter context
    CounterMessage counter_context = {.value = 0};
    
    // Spawn counter actor
    ActorRef* counter_ref = actor_spawn(system, "counter", counter_behavior, 
                                       &counter_context, sizeof(CounterMessage));
    assert(counter_ref != NULL);
    assert(actor_ref_is_valid(counter_ref));
    assert(strcmp(counter_ref->actor_name, "counter") == 0);
    
    // Give actor time to start
    usleep(100000); // 100ms
    
    // Verify actor was added to system
    Actor* counter_actor = actor_system_find_actor(system, counter_ref->actor_id);
    assert(counter_actor != NULL);
    assert(counter_actor->state == ACTOR_STATE_RUNNING);
    
    // Test finding by name
    Actor* found_actor = actor_system_find_actor_by_name(system, "counter");
    assert(found_actor != NULL);
    assert(found_actor == counter_actor);
    
    // Cleanup
    actor_terminate(counter_ref);
    actor_ref_destroy(counter_ref);
    actor_system_shutdown(system, 1000);
    actor_system_destroy(system);
    
    printf("✓ Actor spawning test passed\n");
}

// Test message passing
void test_message_passing() {
    printf("Testing message passing...\n");
    
    ActorSystem* system = actor_system_create("MessageTestSystem", 2);
    actor_system_start(system);
    
    // Set up counter actor
    ActorBehavior counter_behavior;
    actor_behavior_init(&counter_behavior);
    counter_behavior.handle_message = counter_handler;
    
    CounterMessage counter_context = {.value = 10};
    ActorRef* counter_ref = actor_spawn(system, "counter", counter_behavior, 
                                       &counter_context, sizeof(CounterMessage));
    
    // Give actor time to start
    usleep(100000); // 100ms
    
    // Send increment message
    IncrementMessage inc_msg = {.increment = 5};
    ActorFuture* future = actor_send(counter_ref, &inc_msg, sizeof(IncrementMessage));
    assert(future != NULL);
    
    // Wait for response
    Result_void_ptr result = actor_future_await_timeout(future, 5000);
    assert(!result.is_error);
    
    int* response = (int*)result.value;
    assert(*response == 15); // 10 + 5
    
    printf("Received response: %d\n", *response);
    
    // Test fire and forget message
    IncrementMessage inc_msg2 = {.increment = 3};
    Result_void_ptr fire_result = actor_send_fire_and_forget(counter_ref, &inc_msg2, sizeof(IncrementMessage));
    assert(!fire_result.is_error);
    
    // Give time for processing
    usleep(100000);
    
    // Cleanup
    actor_future_destroy(future);
    actor_terminate(counter_ref);
    actor_ref_destroy(counter_ref);
    actor_system_shutdown(system, 1000);
    actor_system_destroy(system);
    
    printf("✓ Message passing test passed\n");
}

// Test echo actor
void test_echo_actor() {
    printf("Testing echo actor...\n");
    
    ActorSystem* system = actor_system_create("EchoTestSystem", 2);
    actor_system_start(system);
    
    // Set up echo actor
    ActorBehavior echo_behavior;
    actor_behavior_init(&echo_behavior);
    echo_behavior.handle_message = echo_handler;
    
    ActorRef* echo_ref = actor_spawn(system, "echo", echo_behavior, NULL, 0);
    assert(echo_ref != NULL);
    
    // Give actor time to start
    usleep(100000);
    
    // Send echo message
    EchoMessage echo_msg;
    strcpy(echo_msg.text, "Hello, Actor World!");
    
    ActorFuture* future = actor_send(echo_ref, &echo_msg, sizeof(EchoMessage));
    assert(future != NULL);
    
    // Wait for response
    Result_void_ptr result = actor_future_await_timeout(future, 5000);
    assert(!result.is_error);
    
    EchoMessage* response = (EchoMessage*)result.value;
    assert(strcmp(response->text, "Hello, Actor World!") == 0);
    
    printf("Echo response: '%s'\n", response->text);
    
    // Cleanup
    actor_future_destroy(future);
    actor_terminate(echo_ref);
    actor_ref_destroy(echo_ref);
    actor_system_shutdown(system, 1000);
    actor_system_destroy(system);
    
    printf("✓ Echo actor test passed\n");
}

// Test multiple actors
void test_multiple_actors() {
    printf("Testing multiple actors...\n");
    
    ActorSystem* system = actor_system_create("MultiActorTestSystem", 4);
    actor_system_start(system);
    
    // Create multiple counter actors
    ActorBehavior counter_behavior;
    actor_behavior_init(&counter_behavior);
    counter_behavior.handle_message = counter_handler;
    
    const int num_actors = 5;
    ActorRef* actors[num_actors];
    CounterMessage contexts[num_actors];
    
    // Spawn actors
    for (int i = 0; i < num_actors; i++) {
        contexts[i].value = i * 10;
        char name[64];
        snprintf(name, sizeof(name), "counter_%d", i);
        
        actors[i] = actor_spawn(system, name, counter_behavior, 
                               &contexts[i], sizeof(CounterMessage));
        assert(actors[i] != NULL);
    }
    
    // Give actors time to start
    usleep(200000);
    
    // Send messages to all actors
    ActorFuture* futures[num_actors];
    for (int i = 0; i < num_actors; i++) {
        IncrementMessage inc_msg = {.increment = i + 1};
        futures[i] = actor_send(actors[i], &inc_msg, sizeof(IncrementMessage));
        assert(futures[i] != NULL);
    }
    
    // Wait for all responses
    for (int i = 0; i < num_actors; i++) {
        Result_void_ptr result = actor_future_await_timeout(futures[i], 5000);
        assert(!result.is_error);
        
        int* response = (int*)result.value;
        int expected = (i * 10) + (i + 1);
        assert(*response == expected);
        
        printf("Actor %d response: %d (expected %d)\n", i, *response, expected);
    }
    
    // Check system statistics
    uint64_t total_actors, total_messages, active_futures;
    actor_system_get_statistics(system, &total_actors, &total_messages, &active_futures);
    
    printf("System stats - Actors: %llu, Messages: %llu, Futures: %llu\n", 
           total_actors, total_messages, active_futures);
    
    assert(total_actors == num_actors);
    assert(total_messages >= num_actors); // At least one message per actor
    
    // Cleanup
    for (int i = 0; i < num_actors; i++) {
        actor_future_destroy(futures[i]);
        actor_terminate(actors[i]);
        actor_ref_destroy(actors[i]);
    }
    
    actor_system_shutdown(system, 2000);
    actor_system_destroy(system);
    
    printf("✓ Multiple actors test passed\n");
}

// Test future operations
void test_future_operations() {
    printf("Testing future operations...\n");
    
    ActorSystem* system = actor_system_create("FutureTestSystem", 2);
    actor_system_start(system);
    
    // Set up counter actor
    ActorBehavior counter_behavior;
    actor_behavior_init(&counter_behavior);
    counter_behavior.handle_message = counter_handler;
    
    CounterMessage counter_context = {.value = 100};
    ActorRef* counter_ref = actor_spawn(system, "future_counter", counter_behavior, 
                                       &counter_context, sizeof(CounterMessage));
    
    usleep(100000);
    
    // Test future with timeout
    IncrementMessage inc_msg = {.increment = 25};
    ActorFuture* future = actor_send_with_timeout(counter_ref, &inc_msg, sizeof(IncrementMessage), 1000);
    assert(future != NULL);
    
    // Test future ready state
    assert(!actor_future_is_ready(future));
    
    // Wait for completion
    Result_void_ptr result = actor_future_await(future);
    assert(!result.is_error);
    assert(actor_future_is_ready(future));
    
    int* response = (int*)result.value;
    assert(*response == 125);
    
    printf("Future response: %d\n", *response);
    
    // Test timeout (with invalid actor to force timeout)
    ActorRef invalid_ref = {.actor_id = 99999, .is_valid = true};
    strcpy(invalid_ref.actor_name, "invalid");
    
    ActorFuture* timeout_future = actor_send_with_timeout(&invalid_ref, &inc_msg, sizeof(IncrementMessage), 100);
    // This should fail immediately since the actor doesn't exist
    assert(timeout_future == NULL);
    
    // Cleanup
    actor_future_destroy(future);
    actor_terminate(counter_ref);
    actor_ref_destroy(counter_ref);
    actor_system_shutdown(system, 1000);
    actor_system_destroy(system);
    
    printf("✓ Future operations test passed\n");
}

// Test actor lifecycle
void test_actor_lifecycle() {
    printf("Testing actor lifecycle...\n");
    
    ActorSystem* system = actor_system_create("LifecycleTestSystem", 2);
    actor_system_start(system);
    
    // Set up actor with lifecycle callbacks
    ActorBehavior lifecycle_behavior;
    actor_behavior_init(&lifecycle_behavior);
    lifecycle_behavior.handle_message = echo_handler;
    lifecycle_behavior.pre_start = NULL; // Would be set in real use
    lifecycle_behavior.post_stop = NULL; // Would be set in real use
    
    ActorRef* actor_ref = actor_spawn(system, "lifecycle_actor", lifecycle_behavior, NULL, 0);
    assert(actor_ref != NULL);
    assert(actor_ref_is_valid(actor_ref));
    
    usleep(100000);
    
    // Verify actor is running
    Actor* actor = actor_system_find_actor(system, actor_ref->actor_id);
    assert(actor != NULL);
    assert(actor->state == ACTOR_STATE_RUNNING);
    
    // Terminate actor
    Result_void_ptr term_result = actor_terminate(actor_ref);
    assert(!term_result.is_error);
    assert(!actor_ref_is_valid(actor_ref));
    
    // Give time for termination
    usleep(200000);
    
    actor_ref_destroy(actor_ref);
    actor_system_shutdown(system, 1000);
    actor_system_destroy(system);
    
    printf("✓ Actor lifecycle test passed\n");
}

// Test error handling
void test_error_handling() {
    printf("Testing error handling...\n");
    
    ActorSystem* system = actor_system_create("ErrorTestSystem", 2);
    actor_system_start(system);
    
    // Test sending to invalid actor
    ActorRef invalid_ref = {.actor_id = 99999, .is_valid = false};
    strcpy(invalid_ref.actor_name, "invalid");
    
    IncrementMessage msg = {.increment = 1};
    Result_void_ptr result = actor_send_fire_and_forget(&invalid_ref, &msg, sizeof(msg));
    assert(result.is_error);
    assert(result.error->code == ERROR_ACTOR_NOT_FOUND);
    
    // Test invalid parameters
    ActorFuture* null_future = actor_send(NULL, &msg, sizeof(msg));
    assert(null_future == NULL);
    
    // Test future operations on NULL
    Result_void_ptr null_result = actor_future_await(NULL);
    assert(null_result.is_error);
    
    actor_system_shutdown(system, 1000);
    actor_system_destroy(system);
    
    printf("✓ Error handling test passed\n");
}

// Performance test
void test_performance() {
    printf("Testing performance...\n");
    
    ActorSystem* system = actor_system_create("PerformanceTestSystem", 8);
    actor_system_start(system);
    
    // Create high-performance counter actor
    ActorBehavior counter_behavior;
    actor_behavior_init(&counter_behavior);
    counter_behavior.handle_message = counter_handler;
    
    CounterMessage counter_context = {.value = 0};
    ActorRef* counter_ref = actor_spawn(system, "perf_counter", counter_behavior, 
                                       &counter_context, sizeof(CounterMessage));
    
    usleep(100000);
    
    // Send many messages
    const int num_messages = 1000;
    clock_t start_time = clock();
    
    for (int i = 0; i < num_messages; i++) {
        IncrementMessage inc_msg = {.increment = 1};
        actor_send_fire_and_forget(counter_ref, &inc_msg, sizeof(IncrementMessage));
    }
    
    // Give time for processing
    usleep(1000000); // 1 second
    
    clock_t end_time = clock();
    double elapsed = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    double msgs_per_sec = num_messages / elapsed;
    
    printf("Sent %d messages in %.3f seconds (%.0f msgs/sec)\n", 
           num_messages, elapsed, msgs_per_sec);
    
    // Verify system statistics
    uint64_t total_actors, total_messages, active_futures;
    actor_system_get_statistics(system, &total_actors, &total_messages, &active_futures);
    
    printf("Final stats - Messages sent: %llu\n", total_messages);
    assert(total_messages >= num_messages);
    
    // Cleanup
    actor_terminate(counter_ref);
    actor_ref_destroy(counter_ref);
    actor_system_shutdown(system, 2000);
    actor_system_destroy(system);
    
    printf("✓ Performance test passed\n");
}

int main() {
    printf("=== Actor System Foundation Tests ===\n\n");
    
    test_actor_system_creation();
    test_actor_spawning();
    test_message_passing();
    test_echo_actor();
    test_multiple_actors();
    test_future_operations();
    test_actor_lifecycle();
    test_error_handling();
    test_performance();
    
    printf("\n=== All Actor System Tests Passed! ===\n");
    return 0;
}