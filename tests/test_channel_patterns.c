#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "runtime.h"

void test_pub_sub() {
    printf("=== Testing Pub/Sub Pattern ===\n");
    
    // Create publisher and subscriber channels with buffers for testing
    goo_channel_t* pub = goo_make_pattern_chan(GOO_CHANNEL_PUB, sizeof(int), NULL);
    goo_channel_t* sub1 = goo_make_chan(sizeof(int), 5);  // Buffered for testing
    goo_channel_t* sub2 = goo_make_chan(sizeof(int), 5);  // Buffered for testing
    sub1->pattern = GOO_CHANNEL_SUB;
    sub2->pattern = GOO_CHANNEL_SUB;
    
    printf("Created publisher and 2 subscribers\n");
    
    // Subscribe channels to publisher
    if (goo_chan_subscribe(pub, sub1)) {
        printf("Subscriber 1 connected successfully\n");
    }
    if (goo_chan_subscribe(pub, sub2)) {
        printf("Subscriber 2 connected successfully\n");
    }
    
    // Publish a message
    int message = 42;
    printf("Publishing message: %d\n", message);
    if (goo_chan_send(pub, &message)) {
        printf("Message published successfully\n");
    }
    
    // Read from subscribers
    int received1, received2;
    if (goo_chan_try_recv(sub1, &received1)) {
        printf("Subscriber 1 received: %d\n", received1);
    }
    if (goo_chan_try_recv(sub2, &received2)) {
        printf("Subscriber 2 received: %d\n", received2);
    }
    
    // Cleanup
    goo_chan_unsubscribe(pub, sub1);
    goo_chan_unsubscribe(pub, sub2);
    goo_chan_free(pub);
    goo_chan_free(sub1);
    goo_chan_free(sub2);
    
    printf("Pub/Sub test completed\n\n");
}

void test_push_pull() {
    printf("=== Testing Push/Pull Pattern ===\n");
    
    // Create push and pull channels with buffers for testing
    goo_channel_t* push = goo_make_pattern_chan(GOO_CHANNEL_PUSH, sizeof(int), NULL);
    goo_channel_t* pull1 = goo_make_chan(sizeof(int), 5);  // Buffered for testing
    goo_channel_t* pull2 = goo_make_chan(sizeof(int), 5);  // Buffered for testing
    pull1->pattern = GOO_CHANNEL_PULL;
    pull2->pattern = GOO_CHANNEL_PULL;
    
    printf("Created push channel and 2 pull workers\n");
    
    // Add workers to push channel
    if (goo_chan_add_worker(push, pull1)) {
        printf("Worker 1 connected successfully\n");
    } else {
        printf("Failed to connect Worker 1\n");
    }
    if (goo_chan_add_worker(push, pull2)) {
        printf("Worker 2 connected successfully\n");
    } else {
        printf("Failed to connect Worker 2\n");
    }
    
    printf("Push channel now has %zu workers\n", push->pattern_data.push_pull_data.worker_count);
    
    // Send multiple messages (should be load-balanced)
    for (int i = 1; i <= 4; i++) {
        printf("Pushing message: %d\n", i);
        if (goo_chan_send(push, &i)) {
            printf("Message %d pushed successfully\n", i);
        } else {
            printf("Failed to push message %d\n", i);
        }
    }
    
    // Workers should receive messages in round-robin fashion
    int received;
    printf("Checking worker outputs:\n");
    printf("Worker 1 has %zu messages, Worker 2 has %zu messages\n", 
           goo_chan_len(pull1), goo_chan_len(pull2));
    
    // Check all messages in worker queues
    while (goo_chan_try_recv(pull1, &received)) {
        printf("Worker 1 received: %d\n", received);
    }
    while (goo_chan_try_recv(pull2, &received)) {
        printf("Worker 2 received: %d\n", received);
    }
    
    // Cleanup
    goo_chan_free(push);
    goo_chan_free(pull1);
    goo_chan_free(pull2);
    
    printf("Push/Pull test completed\n\n");
}

void test_req_rep() {
    printf("=== Testing Req/Rep Pattern ===\n");
    
    // Create request and reply channels with buffers for testing
    goo_channel_t* req = goo_make_pattern_chan(GOO_CHANNEL_REQ, sizeof(int), NULL);
    goo_channel_t* rep = goo_make_chan(sizeof(int), 5);  // Buffered for testing
    rep->pattern = GOO_CHANNEL_REP;
    
    printf("Created request and reply channels\n");
    
    // Pair the channels
    if (goo_chan_pair_req_rep(req, rep)) {
        printf("Channels paired successfully\n");
    }
    
    // Send a request
    int request = 100;
    printf("Sending request: %d\n", request);
    if (goo_chan_send(req, &request)) {
        printf("Request sent successfully\n");
    }
    
    // Receive request on reply channel
    int received_req;
    if (goo_chan_try_recv(rep, &received_req)) {
        printf("Received request: %d\n", received_req);
        
        // Send reply
        int reply = received_req * 2;
        printf("Sending reply: %d\n", reply);
        // Note: In a full implementation, reply would go back to req channel
        // For now, this demonstrates the pairing mechanism
    }
    
    // Cleanup
    goo_chan_free(req);
    goo_chan_free(rep);
    
    printf("Req/Rep test completed\n\n");
}

int main() {
    printf("Channel Patterns Test Suite\n");
    printf("===========================\n\n");
    
    // Initialize runtime
    goo_init(0, NULL);
    
    test_pub_sub();
    test_push_pull();
    test_req_rep();
    
    printf("All channel pattern tests completed!\n");
    
    // Cleanup runtime
    goo_exit(0);
    
    return 0;
}