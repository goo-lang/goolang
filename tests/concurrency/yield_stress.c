// yield-stress: goroutines call goo_yield() repeatedly under 4 OS threads.
// RED before M8d Item 2: goo_yield re-enqueues the goroutine BEFORE swapcontext
// saves its context, so another worker can swap into a half-saved/duplicated
// context → crash/corruption. GREEN after: the scheduler re-enqueues via
// t_requeue only after the swap completes.
#define _GNU_SOURCE
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>

static atomic_int g_done;

static void yielder(void* arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        goo_yield();
    }
    atomic_fetch_add(&g_done, 1);
}

int main(void) {
    goo_scheduler_init(4);
    const int BATCHES = 50, PER = 16;
    for (int b = 0; b < BATCHES; b++) {
        atomic_store(&g_done, 0);
        for (int i = 0; i < PER; i++) {
            goo_go(yielder, NULL);
        }
        goo_scheduler_wait();
        int got = atomic_load(&g_done);
        if (got != PER) {
            fprintf(stderr, "batch %d: done=%d expected %d\n", b, got, PER);
            return 1;
        }
    }
    printf("yield-stress OK (%d batches x %d goroutines x 50 yields @ 4 threads)\n", BATCHES, PER);
    return 0;
}
