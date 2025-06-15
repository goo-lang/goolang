#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "runtime.h"

// Test deadlock detection logic directly
extern goo_scheduler_t* g_scheduler;

void test_deadlock_detection_logic() {
    printf("=== Testing Deadlock Detection Logic ===\n");
    
    // Initialize runtime
    goo_init(0, NULL);
    
    // Initialize scheduler
    goo_scheduler_init(1);
    
    // Manually set up a deadlock scenario for testing
    if (!g_scheduler) {
        printf("ERROR: Scheduler not initialized\n");
        return;
    }
    
    // Create two channels
    goo_channel_t* ch1 = goo_make_chan(sizeof(int), 0);
    goo_channel_t* ch2 = goo_make_chan(sizeof(int), 0);
    
    // Create mock goroutines
    goo_goroutine_t g1 = {
        .id = 1,
        .state = GOO_GOROUTINE_BLOCKED,
        .waiting_on_channel = ch1,
        .waiting_for_send = 1,
        .next = NULL
    };
    
    goo_goroutine_t g2 = {
        .id = 2,
        .state = GOO_GOROUTINE_BLOCKED,
        .waiting_on_channel = ch2,
        .waiting_for_send = 1,
        .next = &g1
    };
    
    // Set up the ready queue to include our mock goroutines
    g_scheduler->ready_queue = &g2;
    
    // Set up waiting lists to create a dependency
    ch1->recv_waiters = &g2;  // g2 waiting to receive from ch1
    ch2->recv_waiters = &g1;  // g1 waiting to receive from ch2
    
    printf("Created deadlock scenario:\n");
    printf("  Goroutine 1: sending to ch1, needs receiver\n");
    printf("  Goroutine 2: sending to ch2, needs receiver\n");
    printf("  But g1 is waiting to receive from ch2, g2 is waiting to receive from ch1\n");
    
    // Enable deadlock detection
    goo_deadlock_enable(1);
    
    // Force a deadlock check
    printf("Running deadlock detection...\n");
    printf("Debug: g1.id=%llu, g1.waiting_on_channel=%p\n", 
           (unsigned long long)g1.id, (void*)g1.waiting_on_channel);
    printf("Debug: g2.id=%llu, g2.waiting_on_channel=%p\n", 
           (unsigned long long)g2.id, (void*)g2.waiting_on_channel);
    printf("Debug: ch1=%p, ch1.recv_waiters=%p\n", (void*)ch1, (void*)ch1->recv_waiters);
    printf("Debug: ch2=%p, ch2.recv_waiters=%p\n", (void*)ch2, (void*)ch2->recv_waiters);
    
    int deadlock_found = goo_deadlock_check();
    
    if (deadlock_found) {
        printf("SUCCESS: Deadlock detected!\n");
    } else {
        printf("No deadlock detected\n");
    }
    
    // Clean up
    g_scheduler->ready_queue = NULL;
    ch1->recv_waiters = NULL;
    ch2->recv_waiters = NULL;
    
    goo_chan_free(ch1);
    goo_chan_free(ch2);
    
    printf("Deadlock detection logic test completed\n\n");
}

void test_deadlock_detection_disabled() {
    printf("=== Testing Deadlock Detection Disabled ===\n");
    
    // Disable deadlock detection
    goo_deadlock_enable(0);
    
    printf("Deadlock detection disabled\n");
    
    // Run deadlock check
    int deadlock_found = goo_deadlock_check();
    
    if (deadlock_found) {
        printf("UNEXPECTED: Deadlock detected when disabled\n");
    } else {
        printf("SUCCESS: No deadlock detected when disabled\n");
    }
    
    // Re-enable for next tests
    goo_deadlock_enable(1);
    
    printf("Deadlock detection disabled test completed\n\n");
}

void test_no_goroutines() {
    printf("=== Testing No Blocked Goroutines ===\n");
    
    // Ensure no goroutines in scheduler
    if (g_scheduler) {
        g_scheduler->ready_queue = NULL;
    }
    
    printf("No goroutines in scheduler\n");
    
    // Run deadlock check
    int deadlock_found = goo_deadlock_check();
    
    if (deadlock_found) {
        printf("UNEXPECTED: Deadlock detected with no goroutines\n");
    } else {
        printf("SUCCESS: No deadlock detected with no goroutines\n");
    }
    
    printf("No goroutines test completed\n\n");
}

int main() {
    printf("Deadlock Detection Logic Test Suite\n");
    printf("===================================\n\n");
    
    test_deadlock_detection_disabled();
    test_no_goroutines();
    test_deadlock_detection_logic();
    
    printf("All deadlock detection logic tests completed!\n");
    
    // Cleanup runtime
    goo_exit(0);
    
    return 0;
}