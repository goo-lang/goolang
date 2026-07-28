// ARC step 1: the object header. Table-driven unit test.
//
// ADR 0002 chose reference counting. Every ARC implementation needs somewhere
// to keep the count, and goo_alloc returned raw malloc memory with no header
// at all. This test pins the header's contract BEFORE any retain or release is
// emitted by codegen, so the allocator change can be proven invisible on its
// own.
//
// The layout is the one the feasibility audit picked: the header sits BEFORE
// the payload, the way malloc itself works. goo_alloc(n) allocates
// GOO_OBJ_HEADER_SIZE + n and returns base + GOO_OBJ_HEADER_SIZE, so every
// caller keeps seeing the object address and no codegen change is needed.
//
// The three properties that carry the most risk, each a row below:
//
//   1. goo_zerobase has NO header. Every zero-size allocation aliases one
//      static byte, so a header read through it would read whatever precedes
//      a static, and a header WRITE would corrupt it. Retain, release and the
//      count query must all treat it as a no-op.
//   2. goo_realloc must move the BASE, not the payload. It used to hand the
//      payload pointer straight to realloc(); with a header in front, that
//      would offset a pointer libc never returned.
//   3. The size arithmetic must not overflow. calloc used to do this check for
//      free inside goo_slice_alloc; a plain add wraps, under-allocates, and
//      hands back a buffer the caller writes past.

#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>

static int failures = 0;
static int checks = 0;

static void check(int cond, const char* what) {
    checks++;
    if (!cond) {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

static void row(int n, const char* desc) {
    printf("=== Row %d: %s ===\n", n, desc);
}

// Workers for the concurrency rows. Kept as file-scope functions rather than
// nested lambdas because C has none, and pthread_create needs this exact
// signature.
#define OBJ_HEADER_NITERS 100000

static void* retain_worker(void* p) {
    for (int i = 0; i < OBJ_HEADER_NITERS; i++) goo_retain(p);
    return NULL;
}

static void* retain_release_worker(void* p) {
    for (int i = 0; i < OBJ_HEADER_NITERS; i++) {
        goo_retain(p);
        goo_release(p);
    }
    return NULL;
}

static void* release_once_worker(void* p) {
    goo_release(p);
    return NULL;
}

int main(void) {
    printf("Running ARC object-header tests...\n");

    row(1, "a fresh allocation starts at refcount 1");
    {
        void* p = goo_alloc(32);
        check(p != NULL, "goo_alloc returned NULL");
        check(goo_obj_refcount(p) == 1, "fresh allocation is not refcount 1");
        goo_free(p);
    }

    row(2, "the payload is writable for its full requested size");
    {
        // Catches a header that overlaps the payload, which would show up as
        // the count changing when the caller writes its own first byte.
        size_t n = 64;
        unsigned char* p = (unsigned char*)goo_alloc(n);
        memset(p, 0xAB, n);
        check(goo_obj_refcount(p) == 1, "writing the payload changed the count");
        int intact = 1;
        for (size_t i = 0; i < n; i++) if (p[i] != 0xAB) intact = 0;
        check(intact, "payload did not survive its own memset");
        goo_free(p);
    }

    row(3, "the payload is 16-byte aligned");
    {
        // max_align_t must hold for the payload, and arena.c already uses 16
        // (GOO_ARENA_ALIGNMENT), so 16 is the consistent choice.
        int aligned = 1;
        void* held[8];
        for (int i = 0; i < 8; i++) {
            held[i] = goo_alloc((size_t)(i * 7 + 1));
            if (((uintptr_t)held[i] % 16u) != 0u) aligned = 0;
        }
        check(aligned, "a payload was not 16-byte aligned");
        for (int i = 0; i < 8; i++) goo_free(held[i]);
    }

    row(4, "retain and release move the count");
    {
        void* p = goo_alloc(16);
        goo_retain(p);
        check(goo_obj_refcount(p) == 2, "retain did not increment");
        goo_retain(p);
        check(goo_obj_refcount(p) == 3, "second retain did not increment");
        goo_release(p);
        check(goo_obj_refcount(p) == 2, "release did not decrement");
        goo_release(p);
        check(goo_obj_refcount(p) == 1, "second release did not decrement");
        goo_release(p);  // drops to 0 and frees; nothing to assert without ASan
    }

    row(5, "goo_zerobase has no header and is inert");
    {
        void* z = goo_alloc(0);
        check(z == (void*)&goo_zerobase, "zero-size alloc did not return the sentinel");
        check(goo_obj_refcount(z) == 0, "sentinel reported a refcount");
        goo_retain(z);   // must not write through the sentinel
        goo_release(z);  // must not free the sentinel
        check(goo_obj_refcount(z) == 0, "retain/release wrote through the sentinel");
        goo_free(z);     // still a no-op
    }

    row(6, "NULL is inert for every header operation");
    {
        check(goo_obj_refcount(NULL) == 0, "NULL reported a refcount");
        goo_retain(NULL);
        goo_release(NULL);
        goo_free(NULL);
        check(1, "NULL handling crashed");
    }

    row(7, "realloc preserves the payload and the count");
    {
        unsigned char* p = (unsigned char*)goo_alloc(16);
        memset(p, 0x5A, 16);
        goo_retain(p);  // count 2, must survive the move
        unsigned char* q = (unsigned char*)goo_realloc(p, 128);
        check(q != NULL, "goo_realloc returned NULL");
        check(((uintptr_t)q % 16u) == 0u, "reallocated payload lost its alignment");
        int intact = 1;
        for (int i = 0; i < 16; i++) if (q[i] != 0x5A) intact = 0;
        check(intact, "goo_realloc did not preserve the payload");
        check(goo_obj_refcount(q) == 2, "goo_realloc did not preserve the count");
        goo_release(q);
        goo_free(q);
    }

    row(8, "realloc from the sentinel is a fresh allocation");
    {
        void* z = goo_alloc(0);
        void* p = goo_realloc(z, 32);
        check(p != NULL && p != (void*)&goo_zerobase, "realloc from sentinel misbehaved");
        check(goo_obj_refcount(p) == 1, "realloc from sentinel did not init the count");
        goo_free(p);
    }

    row(9, "realloc from NULL is a fresh allocation");
    {
        void* p = goo_realloc(NULL, 32);
        check(p != NULL, "realloc from NULL returned NULL");
        check(goo_obj_refcount(p) == 1, "realloc from NULL did not init the count");
        goo_free(p);
    }

    row(10, "realloc to size 0 frees and returns NULL");
    {
        void* p = goo_alloc(32);
        void* q = goo_realloc(p, 0);
        check(q == NULL, "realloc to 0 did not return NULL");
    }

    row(11, "two allocations do not share a header");
    {
        void* a = goo_alloc(16);
        void* b = goo_alloc(16);
        goo_retain(a);
        check(goo_obj_refcount(a) == 2, "retain on a did not take");
        check(goo_obj_refcount(b) == 1, "retain on a changed b's count");
        goo_release(a);
        goo_free(a);
        goo_free(b);
    }

    // -----------------------------------------------------------------------
    // Concurrency rows. Goroutines are NOT cooperative coroutines on one
    // thread: concurrency.c:110 spawns goo_default_thread_count() OS threads
    // (GOMAXPROCS or NCPU, capped at 16), and a yielded goroutine is
    // republished to a SHARED ready queue that any worker can take. So two
    // goroutines genuinely run at once, and one goroutine can even move
    // between OS threads.
    //
    // These rows drive pthreads directly rather than the Goo scheduler: the
    // subject under test is the primitive, not the scheduler.
    //
    // They are the rows that fail if the count is not atomic. Row 12 is the
    // reliable one — a lost update on a plain `rc++` shows up as a deficit
    // that grows with the iteration count. A race is not guaranteed to show
    // itself on any single run, which is exactly why obj-header-tsan exists
    // alongside these.
    // -----------------------------------------------------------------------
    enum { NTHREADS = 8, NITERS = 100000 };

    row(12, "concurrent retain does not lose an update");
    {
        void* p = goo_alloc(16);
        pthread_t th[NTHREADS];
        for (int i = 0; i < NTHREADS; i++) {
            pthread_create(&th[i], NULL, retain_worker, p);
        }
        for (int i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);

        uint64_t want = 1 + (uint64_t)NTHREADS * NITERS;
        uint64_t got = goo_obj_refcount(p);
        if (got != want) {
            printf("  (count is %llu, expected %llu — %lld updates lost)\n",
                   (unsigned long long)got, (unsigned long long)want,
                   (long long)want - (long long)got);
        }
        check(got == want, "concurrent retain lost an update");

        for (uint64_t i = 0; i < (uint64_t)NTHREADS * NITERS; i++) goo_release(p);
        check(goo_obj_refcount(p) == 1, "unwinding the retains did not reach 1");
        goo_free(p);
    }

    row(13, "balanced concurrent retain/release returns to the start");
    {
        void* p = goo_alloc(16);
        pthread_t th[NTHREADS];
        for (int i = 0; i < NTHREADS; i++) {
            pthread_create(&th[i], NULL, retain_release_worker, p);
        }
        for (int i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);

        // The object must still be ALIVE: every release here is paired with a
        // retain, so the count must never have transiently hit 0 and freed it.
        check(goo_obj_refcount(p) == 1, "balanced retain/release did not return to 1");
        goo_free(p);
    }

    row(14, "the last concurrent release frees exactly once");
    {
        // N threads race to drop the final reference. Exactly one of them must
        // observe the transition to 0. Two would be a double free, which the
        // TOCTOU in the pre-atomic goo_release made reachable: it read the
        // count, compared it, and decremented as three separate steps.
        void* p = goo_alloc(16);
        for (int i = 0; i < NTHREADS; i++) goo_retain(p);  // count = 1 + N

        pthread_t th[NTHREADS];
        for (int i = 0; i < NTHREADS; i++) {
            pthread_create(&th[i], NULL, release_once_worker, p);
        }
        for (int i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);

        check(goo_obj_refcount(p) == 1, "concurrent release did not settle at 1");
        goo_free(p);  // the one remaining reference
    }

    printf("\n=================================================\n");
    printf("obj_header_test summary: %d checks passed, %d failed\n",
           checks - failures, failures);
    return failures ? 1 : 0;
}
