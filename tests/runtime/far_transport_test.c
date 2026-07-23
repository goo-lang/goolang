// far_transport unit test (M2-B1 T2). Pins the shim ABI, FIFO ordering,
// the buffering envelope (sends never block on remote progress for the
// protocol's in-flight counts), and the "far: closed" vs hard-failure
// error split the lanes.go pumps compare against.
#include "far_transport.h"
#include "runtime.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char url[256];

static int str_eq(goo_string_t s, const char* want) {
    return s.length == strlen(want) && memcmp(s.data, want, s.length) == 0;
}
static int str_starts(goo_string_t s, const char* pre) {
    return s.length >= strlen(pre) && memcmp(s.data, pre, strlen(pre)) == 0;
}

static void* closer_thread(void* arg) {
    int64_t h = *(int64_t*)arg;
    usleep(200 * 1000);
    goo_far_close(h);
    return NULL;
}

int main(void) {
    char tmpl[] = "/tmp/goo-far-test-XXXXXX";
    char* dir = mkdtemp(tmpl);
    assert(dir);
    snprintf(url, sizeof(url), "ipc://%s/sock", dir);

    goo_string_t err;
    int64_t a = -1, b = -1;

    // listen/dial roundtrip
    assert(goo_far_listen(url, &a, &err) == 1);
    assert(goo_far_dial(url, &b, &err) == 1);

    // Buffering envelope pin: 8 sends complete with NO receiver progress.
    // (Protocol in-flight max is per-sub-exchange 1 for halos and `count`
    // partials + 1 broadcast for collectives; 8 covers the probe shapes
    // with margin. far_transport.c sets SENDBUF/RECVBUF=128 to make this
    // a configured property, not a default-dependent accident.)
    for (int i = 0; i < 8; i++) goo_far_send_f64(a, 1.5 * i);

    // FIFO pin: values arrive in send order.
    for (int i = 0; i < 8; i++) {
        double v = -1.0;
        assert(goo_far_recv_f64(b, &v, &err) == 1);
        assert(v == 1.5 * i);
    }

    // Bidirectional (pair socket): b -> a works too.
    goo_far_send_f64(b, 42.0);
    double v = 0.0;
    assert(goo_far_recv_f64(a, &v, &err) == 1);
    assert(v == 42.0);

    // "far: closed": recv blocked on b unblocks when b is closed locally.
    pthread_t t;
    pthread_create(&t, NULL, closer_thread, &b);
    assert(goo_far_recv_f64(b, &v, &err) == 0);
    assert(str_eq(err, "far: closed"));
    pthread_join(t, NULL);

    // Hard failure branch: malformed URL.
    int64_t c = -1;
    assert(goo_far_listen("bogus://nope", &c, &err) == 0);
    assert(str_starts(err, "far: listen failed: "));

    goo_far_close(a);
    printf("far_transport_test: PASS\n");
    return 0;
}
