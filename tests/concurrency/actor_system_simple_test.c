#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "include/actor_system.h"

// Simple echo handler
ACTOR_DEFINE_HANDLER(simple_echo_handler) {
    printf("Actor %s received message of size %zu\n", self->name, message->data_size);
    
    if (message->expects_response && message->data) {
        // Echo the message back
        actor_reply(message, message->data, message->data_size);
    }
}

int main() {
    printf("=== Simple Actor System Test ===\n");
    
    // Test 1: Actor system creation
    printf("1. Creating actor system...\n");
    ActorSystem* system = actor_system_create("SimpleTest", 2);
    assert(system != NULL);
    
    printf("2. Starting actor system...\n");
    Result_void_ptr start_result = actor_system_start(system);
    assert(!start_result.is_error);
    assert(system->is_running);
    
    // Test 2: Message creation
    printf("3. Testing message creation...\n");
    char test_data[] = "Hello World";
    ActorMessage* msg = actor_message_create(MESSAGE_TYPE_USER, test_data, strlen(test_data) + 1);
    assert(msg != NULL);
    printf("   Message ID: %llu\n", msg->message_id);
    
    // Test 3: Mailbox creation
    printf("4. Testing mailbox creation...\n");
    ActorMailbox* mailbox = actor_mailbox_create(10);
    assert(mailbox != NULL);
    
    // Test 4: Message enqueue/dequeue
    printf("5. Testing message queue...\n");
    Result_void_ptr enqueue_result = actor_mailbox_enqueue(mailbox, msg);
    assert(!enqueue_result.is_error);
    
    ActorMessage* dequeued = actor_mailbox_dequeue(mailbox, 1000);
    assert(dequeued != NULL);
    assert(dequeued == msg);
    
    printf("   Dequeued message data: %s\n", (char*)dequeued->data);
    
    // Test 5: Actor spawning (simplified)
    printf("6. Testing actor spawning...\n");
    ActorBehavior echo_behavior;
    actor_behavior_init(&echo_behavior);
    echo_behavior.handle_message = simple_echo_handler;
    
    ActorRef* actor_ref = actor_spawn(system, "simple_echo", echo_behavior, NULL, 0);
    assert(actor_ref != NULL);
    printf("   Spawned actor: %s (ID: %llu)\n", actor_ref->actor_name, actor_ref->actor_id);
    
    // Give actor time to start
    printf("7. Waiting for actor to start...\n");
    sleep(1);
    
    Actor* actor = actor_system_find_actor(system, actor_ref->actor_id);
    assert(actor != NULL);
    printf("   Actor state: %d (should be %d for RUNNING)\n", actor->state, ACTOR_STATE_RUNNING);
    
    // Cleanup
    printf("8. Cleaning up...\n");
    actor_message_destroy(dequeued);
    actor_mailbox_destroy(mailbox);
    actor_terminate(actor_ref);
    actor_ref_destroy(actor_ref);
    
    actor_system_shutdown(system, 2000);
    actor_system_destroy(system);
    
    printf("=== Simple Test Completed Successfully! ===\n");
    return 0;
}