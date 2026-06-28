// mt-scheduler-stress: exercises the M:N scheduler under 4 OS threads.
// RED before M8c (UAF on the goroutine's own stack + shared main_context/
// current_goroutine across workers) → crashes/corrupts in a meaningful
// fraction of the 100 batches. GREEN after M8c → 0 crashes, correct count.
#define _GNU_SOURCE
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>

static atomic_int g_counter;

static void worker(void* arg) {
    (void)arg;
    volatile long x = 0;
    for (int i = 0; i < 2000; i++) x += i;   // bounded work, forces real interleaving
    atomic_fetch_add(&g_counter, 1);
}

int main(void) {
    goo_scheduler_init(4);                    // explicit 4 workers; bypasses the lazy NCPU/GOMAXPROCS default
    const int BATCHES = 100;
    const int PER = 16;
    for (int b = 0; b < BATCHES; b++) {
        atomic_store(&g_counter, 0);
        for (int i = 0; i < PER; i++) {
            goo_go(worker, NULL);
        }
        goo_scheduler_wait();                 // block until this batch's goroutines finish
        int got = atomic_load(&g_counter);
        if (got != PER) {
            fprintf(stderr, "batch %d: counter=%d expected %d\n", b, got, PER);
            return 1;
        }
    }
    printf("mt-scheduler-stress OK (%d batches x %d goroutines @ 4 threads)\n", BATCHES, PER);
    return 0;
}
