#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "runtime.h"

// Test deadlock detection with channel patterns

// Test 1: Simple deadlock - two goroutines waiting on each other
void* goroutine1_func(void* arg) {
    goo_channel_t** channels = (goo_channel_t**)arg;
    goo_channel_t* ch1 = channels[0];
    goo_channel_t* ch2 = channels[1];
    
    printf("Goroutine 1: Sending to channel 1\n");
    int data = 1;
    goo_chan_send(ch1, &data);
    
    printf("Goroutine 1: Trying to receive from channel 2\n");
    int received;
    goo_chan_recv(ch2, &received);
    
    printf("Goroutine 1: Received %d from channel 2\n", received);
    return NULL;
}

void* goroutine2_func(void* arg) {
    goo_channel_t** channels = (goo_channel_t**)arg;
    goo_channel_t* ch1 = channels[0];
    goo_channel_t* ch2 = channels[1];
    
    printf("Goroutine 2: Sending to channel 2\n");
    int data = 2;
    goo_chan_send(ch2, &data);
    
    printf("Goroutine 2: Trying to receive from channel 1\n");
    int received;
    goo_chan_recv(ch1, &received);
    
    printf("Goroutine 2: Received %d from channel 1\n", received);
    return NULL;
}

void test_simple_deadlock() {
    printf("=== Testing Simple Deadlock Detection ===\n");
    
    // Create unbuffered channels (deadlock prone)
    goo_channel_t* ch1 = goo_make_chan(sizeof(int), 0);
    goo_channel_t* ch2 = goo_make_chan(sizeof(int), 0);
    
    goo_channel_t* channels[2] = {ch1, ch2};
    
    printf("Created two unbuffered channels\n");
    printf("Each goroutine will send to one channel and wait for the other\n");
    
    // Create pthread threads to simulate goroutines
    pthread_t thread1, thread2;
    
    printf("Starting goroutines...\n");
    pthread_create(&thread1, NULL, goroutine1_func, channels);
    pthread_create(&thread2, NULL, goroutine2_func, channels);
    
    // Give threads time to start and potentially deadlock
    printf("Waiting for potential deadlock...\n");
    sleep(2);
    
    // Check if deadlock was detected
    if (goo_deadlock_detected()) {
        printf("SUCCESS: Deadlock was detected by the runtime!\n");
    } else {
        printf("Deadlock detection did not trigger (threads may have completed)\n");
    }
    
    // Wait a bit more to see if deadlock is detected
    printf("Waiting for deadlock detection...\n");
    sleep(1);
    
    // Check deadlock detection again
    if (goo_deadlock_detected()) {
        printf("SUCCESS: Deadlock was detected by the runtime!\n");
        pthread_cancel(thread1);
        pthread_cancel(thread2);
    } else {
        printf("Attempting to join threads (may hang if deadlocked)...\n");
        void* result1, *result2;
        pthread_join(thread1, &result1);
        pthread_join(thread2, &result2);
        printf("Threads completed successfully\n");
    }
    
    // Cleanup
    goo_chan_free(ch1);
    goo_chan_free(ch2);
    
    printf("Simple deadlock test completed\n\n");
}

void test_no_deadlock() {
    printf("=== Testing No Deadlock (Control Test) ===\n");
    
    // Create buffered channels (no deadlock)
    goo_channel_t* ch1 = goo_make_chan(sizeof(int), 2);
    goo_channel_t* ch2 = goo_make_chan(sizeof(int), 2);
    
    printf("Created buffered channels (capacity 2)\n");
    
    // Send some data
    int data1 = 10, data2 = 20;
    printf("Sending data to both channels\n");
    goo_chan_send(ch1, &data1);
    goo_chan_send(ch2, &data2);
    
    // Receive the data
    int received1, received2;
    printf("Receiving data from both channels\n");
    goo_chan_recv(ch1, &received1);
    goo_chan_recv(ch2, &received2);
    
    printf("Received: %d and %d\n", received1, received2);
    
    // Check deadlock status
    if (goo_deadlock_detected()) {
        printf("UNEXPECTED: Deadlock detected in non-deadlock scenario\n");
    } else {
        printf("SUCCESS: No deadlock detected (as expected)\n");
    }
    
    // Cleanup
    goo_chan_free(ch1);
    goo_chan_free(ch2);
    
    printf("No deadlock test completed\n\n");
}

void test_deadlock_with_patterns() {
    printf("=== Testing Deadlock Detection with Channel Patterns ===\n");
    
    // Create pub/sub pattern that could deadlock if misconfigured
    goo_channel_t* pub = goo_make_pattern_chan(GOO_CHANNEL_PUB, sizeof(int), NULL);
    goo_channel_t* sub1 = goo_make_chan(sizeof(int), 0);  // Unbuffered subscriber
    goo_channel_t* sub2 = goo_make_chan(sizeof(int), 0);  // Unbuffered subscriber
    sub1->pattern = GOO_CHANNEL_SUB;
    sub2->pattern = GOO_CHANNEL_SUB;
    
    printf("Created pub/sub pattern with unbuffered subscribers\n");
    
    // Subscribe
    goo_chan_subscribe(pub, sub1);
    goo_chan_subscribe(pub, sub2);
    
    // Try to publish (may deadlock if subscribers aren't reading)
    printf("Publishing message...\n");
    int message = 42;
    if (goo_chan_send(pub, &message)) {
        printf("Message published successfully\n");
        
        // Try to read from subscribers
        int received1, received2;
        if (goo_chan_try_recv(sub1, &received1)) {
            printf("Subscriber 1 received: %d\n", received1);
        }
        if (goo_chan_try_recv(sub2, &received2)) {
            printf("Subscriber 2 received: %d\n", received2);
        }
    } else {
        printf("Failed to publish message\n");
    }
    
    // Check deadlock status
    if (goo_deadlock_detected()) {
        printf("Deadlock detected in pattern scenario\n");
    } else {
        printf("No deadlock detected in pattern scenario\n");
    }
    
    // Cleanup
    goo_chan_unsubscribe(pub, sub1);
    goo_chan_unsubscribe(pub, sub2);
    goo_chan_free(pub);
    goo_chan_free(sub1);
    goo_chan_free(sub2);
    
    printf("Pattern deadlock test completed\n\n");
}

int main() {
    printf("Deadlock Detection Test Suite\n");
    printf("=============================\n\n");
    
    // Initialize runtime with deadlock detection
    goo_init(0, NULL);
    printf("Runtime initialized with deadlock detection enabled\n\n");
    
    test_no_deadlock();
    test_deadlock_with_patterns();
    test_simple_deadlock();
    
    printf("All deadlock detection tests completed!\n");
    
    // Cleanup runtime
    goo_exit(0);
    
    return 0;
}