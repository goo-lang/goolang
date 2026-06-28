#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>

extern goo_scheduler_t* g_scheduler;

// Deadlock detection implementation

int goo_deadlock_init(void) {
    if (!g_scheduler) return 0;

    g_scheduler->deadlock_detector.enabled = 1;
    g_scheduler->deadlock_detector.last_check_time = 0;
    g_scheduler->deadlock_detector.check_interval_ns = 1000000000;  // 1 second
    g_scheduler->deadlock_detector.detected_deadlock = 0;
    g_scheduler->deadlock_detector.blocked_goroutines = 0;
    g_scheduler->deadlock_detector.main_in_wait = 0;

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

void goo_deadlock_abort(void) {
    fprintf(stderr, "fatal error: all goroutines are asleep - deadlock!\n");
    fflush(stderr);
    exit(2);
}
