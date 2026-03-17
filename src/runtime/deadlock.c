#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern goo_scheduler_t* g_scheduler;

// Deadlock detection implementation

int goo_deadlock_init(void) {
    if (!g_scheduler) return 0;
    
    g_scheduler->deadlock_detector.enabled = 1;
    g_scheduler->deadlock_detector.last_check_time = 0;
    g_scheduler->deadlock_detector.check_interval_ns = 1000000000;  // 1 second
    g_scheduler->deadlock_detector.detected_deadlock = 0;
    
    return 1;
}

void goo_deadlock_shutdown(void) {
    if (!g_scheduler) return;
    
    g_scheduler->deadlock_detector.enabled = 0;
    g_scheduler->deadlock_detector.detected_deadlock = 0;
}

void goo_deadlock_enable(int enable) {
    if (!g_scheduler) return;
    
    g_scheduler->deadlock_detector.enabled = enable;
    if (!enable) {
        g_scheduler->deadlock_detector.detected_deadlock = 0;
    }
}

int goo_deadlock_detected(void) {
    if (!g_scheduler) return 0;
    
    return g_scheduler->deadlock_detector.detected_deadlock;
}

// Get current time in nanoseconds
static uint64_t get_current_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Check if a goroutine depends on another through channel operations
static int goroutine_depends_on(goo_goroutine_t* g1, goo_goroutine_t* g2) {
    if (!g1 || !g2 || !g1->waiting_on_channel) {
        return 0;
    }
    
    goo_channel_t* channel = g1->waiting_on_channel;
    
    if (g1->waiting_for_send) {
        // g1 is waiting to send on channel
        // Check if g2 is waiting to receive on the same channel
        goo_goroutine_t* waiter = channel->recv_waiters;
        while (waiter) {
            if (waiter == g2) {
#ifndef NDEBUG
                printf("DEBUG: g%llu depends on g%llu (g%llu sending to ch%llu, g%llu receiving)\n",
                       (unsigned long long)g1->id, (unsigned long long)g2->id,
                       (unsigned long long)g1->id, (unsigned long long)channel->id,
                       (unsigned long long)g2->id);
#endif
                return 1;
            }
            waiter = waiter->next;
        }
    } else {
        // g1 is waiting to receive from channel
        // Check if g2 is waiting to send on the same channel
        goo_goroutine_t* waiter = channel->send_waiters;
        while (waiter) {
            if (waiter == g2) {
#ifndef NDEBUG
                printf("DEBUG: g%llu depends on g%llu (g%llu receiving from ch%llu, g%llu sending)\n",
                       (unsigned long long)g1->id, (unsigned long long)g2->id,
                       (unsigned long long)g1->id, (unsigned long long)channel->id,
                       (unsigned long long)g2->id);
#endif
                return 1;
            }
            waiter = waiter->next;
        }
    }
    
    return 0;
}

// Detect cycles in the dependency graph using DFS
static int detect_cycle_dfs(goo_goroutine_t* start, goo_goroutine_t* current, 
                           goo_goroutine_t** visited, int* visited_count, 
                           goo_goroutine_t** path, int path_len) {
    
    // Check if we've returned to the start (cycle found)
    if (current == start && path_len > 0) {
        printf("DEADLOCK DETECTED: Cycle found involving %d goroutines:\n", path_len + 1);
        for (int i = 0; i < path_len; i++) {
            printf("  Goroutine %llu waiting on channel %llu\n", 
                   (unsigned long long)path[i]->id, 
                   (unsigned long long)(path[i]->waiting_on_channel ? path[i]->waiting_on_channel->id : 0));
        }
        printf("  Goroutine %llu waiting on channel %llu\n", 
               (unsigned long long)current->id,
               (unsigned long long)(current->waiting_on_channel ? current->waiting_on_channel->id : 0));
        return 1;
    }
    
    // Check if we've already visited this goroutine in this path
    for (int i = 0; i < path_len; i++) {
        if (path[i] == current) {
            return 0;  // Already in path, but not a cycle to start
        }
    }
    
    // Check if we've visited this goroutine in any path
    for (int i = 0; i < *visited_count; i++) {
        if (visited[i] == current) {
            return 0;  // Already fully explored
        }
    }
    
    // Add to current path
    if (path_len >= 100) {  // Prevent stack overflow
        return 0;
    }
    path[path_len] = current;
    
    // Find all goroutines that current depends on
    goo_goroutine_t* g = g_scheduler->ready_queue;
    while (g) {
        if (g != current && goroutine_depends_on(current, g)) {
            if (detect_cycle_dfs(start, g, visited, visited_count, path, path_len + 1)) {
                return 1;
            }
        }
        g = g->next;
    }
    
    // Mark as fully visited
    visited[*visited_count] = current;
    (*visited_count)++;
    
    return 0;
}

// Main deadlock detection algorithm
static int detect_deadlock(void) {
    if (!g_scheduler) return 0;
    
    // Count blocked goroutines
    int blocked_count = 0;
    int total_count = 0;
    
    goo_goroutine_t* g = g_scheduler->ready_queue;
    while (g) {
        total_count++;
        if (g->state == GOO_GOROUTINE_BLOCKED && g->waiting_on_channel) {
            blocked_count++;
        }
        g = g->next;
    }
    
    // If no blocked goroutines, no deadlock
    if (blocked_count == 0) {
        return 0;
    }
    
    // If all goroutines are blocked, potential deadlock
    // (excluding the main goroutine which might be running the scheduler)
    if (blocked_count >= total_count - 1) {
        printf("WARNING: All goroutines appear to be blocked (%d/%d)\n", 
               blocked_count, total_count);
    }
    
    // Use cycle detection algorithm
    goo_goroutine_t* visited[1000];  // Max 1000 goroutines for simplicity
    int visited_count = 0;
    goo_goroutine_t* path[100];      // Max path length 100
    
    // Check each blocked goroutine as a potential cycle start
    g = g_scheduler->ready_queue;
    while (g) {
        if (g->state == GOO_GOROUTINE_BLOCKED && g->waiting_on_channel) {
            // Reset for new search
            visited_count = 0;
            
            if (detect_cycle_dfs(g, g, visited, &visited_count, path, 0)) {
                return 1;  // Deadlock found
            }
        }
        g = g->next;
    }
    
    return 0;  // No deadlock detected
}

int goo_deadlock_check(void) {
    if (!g_scheduler || !g_scheduler->deadlock_detector.enabled) {
        return 0;
    }
    
    uint64_t current_time = get_current_time_ns();
    
    // Check if enough time has passed since last check
    if (current_time - g_scheduler->deadlock_detector.last_check_time < 
        g_scheduler->deadlock_detector.check_interval_ns) {
        return 0;
    }
    
    g_scheduler->deadlock_detector.last_check_time = current_time;
    
    // Run deadlock detection
    if (detect_deadlock()) {
        g_scheduler->deadlock_detector.detected_deadlock = 1;
        printf("FATAL: Deadlock detected! Stopping program.\n");
        return 1;
    }
    
    return 0;
}