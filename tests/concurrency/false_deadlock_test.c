// false-deadlock-probe: the deadlock detector must NOT abort a live program.
//
// WHAT THIS PINS. Before 2026-09-02 a goroutine counted as blocked from
// goo_sched_block_begin until it returned from pthread_cond_wait AND ran
// goo_sched_block_end. The interval between the wake and that return is
// scheduler latency: the OS owes no bound on it. So for EVERY finite poll
// threshold there is a live program the detector aborts, which is what the
// SPIN model in tests/models/ proves (tests/models/README.md, "The C-level
// fix"). The fix moves the unregister to the WAKE SITE, before the wake, so a
// woken goroutine leaves the blocked set at the instant its waker decides to
// wake it, not when it is next scheduled.
//
// HOW THE WINDOW IS FORCED OPEN, deterministically, on the rendezvous path:
//
//   1. The receiver goroutine parks on not_empty      -> blocked = 1 of 2.
//   2. The sender sleeps 1 ms, so step 1 always wins the race. It then parks
//      the value, wakes not_empty, and parks itself on not_full waiting for
//      the value to be consumed        -> blocked = 2 of 2, ready queue empty.
//   3. main, inside goo_scheduler_wait, now reads
//      blocked_goroutines == num_goroutines with nothing runnable. Under the
//      OLD accounting that is TRUE while the receiver is awake and holding
//      the channel mutex. Under the NEW accounting the sender unregistered
//      the receiver before it broadcast, so the count never reaches 2.
//   4. GOO_TEST_WAKE_DELAY_NS holds step 3 open for as long as the test asks.
//
// THE HOOK. goo_sched_block_end sleeps GOO_TEST_WAKE_DELAY_NS nanoseconds
// before it does anything else, when that variable is set and non-zero. That
// is exactly the wake-to-unregister interval, made long enough to measure.
// The test REFUSES to run without it: with no delay the window is microseconds
// wide and a green result would mean nothing.
//
// RED (runtime before the fix, 100 ms delay): 100 polls of ~0.5 ms elapse
// inside one window, so the program dies with
//   fatal error: all goroutines are asleep - deadlock!
// and exit 2, with both goroutines alive and making progress.
// GREEN (runtime after the fix): exit 0 and the OK line below.
#define _GNU_SOURCE
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// Each handover opens the window once. Four is enough to make a missed first
// window (main may not be in the poll loop yet) irrelevant, and keeps the
// green run near 4 x 2 x delay.
#define HANDOVERS 4

static goo_channel_t* g_ch;
static atomic_long g_sum;

// Long enough that the receiver is always parked on not_empty before the
// sender parks the value; far shorter than the wake delay, so it never
// dominates the run.
static void nap_ms(long ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void sender(void* arg) {
    (void)arg;
    for (int i = 1; i <= HANDOVERS; i++) {
        nap_ms(1);            // let the receiver reach cond_wait first
        int v = i;
        goo_chan_send(g_ch, &v);
    }
}

static void receiver(void* arg) {
    (void)arg;
    for (int i = 0; i < HANDOVERS; i++) {
        int got = 0;
        goo_chan_recv(g_ch, &got);
        atomic_fetch_add(&g_sum, got);
    }
}

int main(void) {
    const char* delay = getenv("GOO_TEST_WAKE_DELAY_NS");
    if (!delay || !*delay || atoll(delay) <= 0) {
        fprintf(stderr,
                "false_deadlock_test: GOO_TEST_WAKE_DELAY_NS must be set to a "
                "positive nanosecond count.\n"
                "Without it the wake-to-unregister window is microseconds wide "
                "and a pass proves nothing.\n");
        return 1;
    }

    goo_scheduler_init(4);
    g_ch = goo_make_chan(sizeof(int), 0);   // UNBUFFERED: the rendezvous path
    atomic_store(&g_sum, 0);

    goo_go(receiver, NULL);
    goo_go(sender, NULL);
    goo_scheduler_wait();                   // the detector's poll loop lives here

    long expect = (long)HANDOVERS * (HANDOVERS + 1) / 2;
    long got = atomic_load(&g_sum);
    if (got != expect) {
        fprintf(stderr, "false_deadlock_test: sum=%ld expected %ld\n", got, expect);
        return 1;
    }
    printf("false-deadlock-probe OK (%d rendezvous handovers, wake delay %s ns, "
           "no abort)\n", HANDOVERS, delay);
    return 0;
}
