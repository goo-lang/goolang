// fanin-stress: N producer goroutines each send once on a shared UNBUFFERED
// (rendezvous) channel; main receives N values. This is the lost-wakeup
// shape the 2026-07-10 P3 sub-A review reproduced hanging on main within
// ~200k iterations: `not_full` is waited on by TWO predicate classes — a
// sender waiting for the rendezvous slot to free (goo_chan_send's first
// loop) and a sender waiting for its parked value to be consumed (the
// second loop) — but the receiver's post-take wakeup was a single
// pthread_cond_signal, which could wake the already-satisfied second-loop
// sender instead of the slot-waiter. The slot-waiter then slept forever
// with main blocked on not_empty (undetectable: the main-blocked-with-
// blocked-goroutines detector gap). Fixed by broadcasting not_full.
// Expected GREEN post-fix; a timeout here is the lost-wakeup regressing.
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>

static goo_channel_t* g_ch;

static void producer(void* arg) {
    (void)arg;
    int v = 3;
    goo_chan_send(g_ch, &v);
}

int main(void) {
    goo_scheduler_init(4);
    // 2 senders reproduced the hang reliably in review; 3 intermittently.
    // Run both shapes, many batches — each batch is a fresh channel so a
    // lost wakeup cannot be rescued by later traffic.
#ifdef GOO_SIM_BUILD
    // Under the deterministic simulator (tests/sim, build/sim/% sets
    // GOO_SIM_BUILD) one seed covers FEWER batches than production: the
    // simulator explores interleavings by seed, so more seeds add more than
    // more batches in one seed. Sized from a measurement (Task 7 fix round 2,
    // 2026-11-08, the 4000-batch program, seeds 1..64, default model): the
    // worst seed took 138.7 steps and 3.879 ms of virtual time per batch, and
    // the virtual clock binds first. The bar is 10% of each default run
    // limit: 100,000 steps and 1 s. 1 s / 3.879 ms = 257 batches; half of
    // that, for the spread of a short run, is 128 = 2 shapes x 64:
    // about 0.50 s and 17,800 steps.
    const int BATCHES = 64;
#else
    const int BATCHES = 4000;
#endif
    for (int senders = 2; senders <= 3; senders++) {
        for (int b = 0; b < BATCHES; b++) {
            g_ch = goo_make_chan(sizeof(int), 0);   // UNBUFFERED: rendezvous path
            for (int i = 0; i < senders; i++) {
                goo_go(producer, NULL);
            }
            long sum = 0;
            for (int i = 0; i < senders; i++) {
                int got = 0;
                goo_chan_recv(g_ch, &got);
                sum += got;
            }
            goo_scheduler_wait();
            if (sum != (long)senders * 3) {
                fprintf(stderr, "senders=%d batch %d: sum=%ld expected %d\n",
                        senders, b, sum, senders * 3);
                return 1;
            }
        }
    }
    printf("fanin-stress OK (2- and 3-sender rendezvous fan-in x %d batches @ 4 threads)\n",
           BATCHES);
    return 0;
}
