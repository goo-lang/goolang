// chan-mt-stress: producer goroutines send over a shared buffered channel under
// 4 OS threads; main receives and checks the aggregate. Exercises channel
// send/recv under true parallelism (a gap left by mt-scheduler-stress) and the
// goo_current_goroutine() accessor. Expected GREEN on current code (regression
// guard); a failure here is a real channel-MT bug to fix.
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>

static goo_channel_t* g_ch;

static void producer(void* arg) {
    (void)arg;
    // accessor sanity: inside a goroutine this must be non-NULL.
    if (goo_current_goroutine() == NULL) {
        // signal failure by sending a sentinel the checker will catch
        int bad = -1000000;
        goo_chan_send(g_ch, &bad);
        return;
    }
    int v = 7;
    goo_chan_send(g_ch, &v);
}

int main(void) {
    goo_scheduler_init(4);
    const int BATCHES = 50, PER = 16;
    for (int b = 0; b < BATCHES; b++) {
        g_ch = goo_make_chan(sizeof(int), PER);   // buffered, capacity PER
        for (int i = 0; i < PER; i++) {
            goo_go(producer, NULL);
        }
        long sum = 0;
        for (int i = 0; i < PER; i++) {
            int got = 0;
            goo_chan_recv(g_ch, &got);
            sum += got;
        }
        goo_scheduler_wait();
        if (sum != (long)PER * 7) {
            fprintf(stderr, "batch %d: sum=%ld expected %d\n", b, sum, PER * 7);
            return 1;
        }
    }
    printf("chan-mt-stress OK (%d batches x %d producers @ 4 threads)\n", BATCHES, PER);
    return 0;
}
