/*
 * A deliberate data race, to prove --config=tsan can report one.
 *
 * Two threads increment one unsynchronised long. Without the sanitizer this
 * exits 0 and merely loses updates.
 *
 * NOTE ON REACH. src/runtime opts out of instrumentation (see
 * src/runtime/BUILD), so tsan does not cover the goroutine scheduler today.
 * This defect proves the CONFIG works, not that the runtime is race-free.
 */
#include <pthread.h>

static long shared = 0;

static void *bump(void *arg) {
    int i;
    (void)arg;
    for (i = 0; i < 100000; i++) {
        shared++;   /* unsynchronised read-modify-write */
    }
    return NULL;
}

int main(void) {
    pthread_t a, b;

    if (pthread_create(&a, NULL, bump, NULL) != 0) return 1;
    if (pthread_create(&b, NULL, bump, NULL) != 0) return 1;
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    return shared == 0 ? 0 : 0;
}
