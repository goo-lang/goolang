// deadlock-poll-probe: a real deadlock must abort THROUGH THE POLL LOOP.
//
// WHY THIS EXISTS, and what no other gate reaches. The 2026-09-02 review of
// the detector rewrite (task 8) found that nothing in the tree exercised
// goo_scheduler_wait's poll loop, its `all_asleep` conjunction, or its
// GOO_DEADLOCK_POLL_STREAK threshold:
//
//   - deadlock-probe (examples/deadlock_probe.goo) blocks MAIN on an empty
//     channel with no goroutine alive, so it aborts inside
//     goo_sched_block_begin's main-thread branch. The poll loop never runs.
//   - false-deadlock-probe requires the absence of an abort, so it proves the
//     count can stay BELOW num_goroutines. It cannot show the abort fires.
//
// So the positive control for the poll loop was missing, and the threshold of
// 3 that task 8 chose was unmeasured in C. This test is that control.
//
// THE SHAPE. Two goroutines park on the recv side of an unbuffered channel
// that nobody will ever send to. main spawns them and calls
// goo_scheduler_wait. Once both are parked:
//
//   blocked_goroutines == 2 == num_goroutines, and ready_queue == NULL
//
// and no waker exists, so the equality is permanent. The streak reaches
// GOO_DEADLOCK_POLL_STREAK and goo_deadlock_abort() runs: exit 2 with Go's
// message on stderr.
//
// WHICH ABORT SITE. main never blocks on a channel, so it never enters
// goo_sched_block_begin at all; the two goroutines take the `self` branch
// there, which has no abort. goo_scheduler_wait holds the only abort this
// program can reach. scripts/deadlock_poll_probe.sh --self-test proves that
// claim rather than asserting it: with the threshold raised past the timeout
// the SAME program hangs, which no other abort site would allow.
//
// GREEN: exit 2 and "fatal error: all goroutines are asleep - deadlock!".
// RED (threshold unreachable, or the count wrong): goo_scheduler_wait spins
// for ever and the probe's timeout fires.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include <stdio.h>

#define PARKERS 2

static goo_channel_t* g_ch;

// Parks on not_empty for ever: the channel is unbuffered and nothing sends.
static void parker(void* arg) {
    (void)arg;
    int got = 0;
    goo_chan_recv(g_ch, &got);
    // Unreachable in a correct run. If a lost wakeup ever delivers here, say
    // so on stdout: the probe asserts stdout is empty.
    printf("deadlock_poll_test: a parked goroutine woke with %d\n", got);
}

int main(void) {
    goo_scheduler_init(4);
    g_ch = goo_make_chan(sizeof(int), 0);   // UNBUFFERED, and no sender exists

    for (int i = 0; i < PARKERS; i++) {
        goo_go(parker, NULL);
    }

    goo_scheduler_wait();   // the poll loop lives here, and must not return

    fprintf(stderr,
            "deadlock_poll_test: goo_scheduler_wait returned with %d parked "
            "goroutines. The detector did not fire.\n", PARKERS);
    return 1;
}
