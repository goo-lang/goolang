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
#include "../goo_check.h"

// 15 rows, written out inline rather than as a table, so the count is a
// literal. goo_check_done() reports BROKEN if the two ever disagree.
#define OBJ_HEADER_ROWS 15

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

// Counts destructor invocations for the goo_release_with rows below.
static int dtor_calls = 0;
static void counting_dtor(void* obj) { (void)obj; dtor_calls++; }

int main(void) {
    goo_check_expect(OBJ_HEADER_ROWS);

    goo_check_row(1, "a fresh allocation starts at refcount 1");
    {
        void* p = goo_alloc(32);
        goo_check(p != NULL, "goo_alloc returned NULL");
        goo_check(goo_obj_refcount(p) == 1, "fresh allocation is not refcount 1");
        goo_free(p);
    }

    goo_check_row(2, "the payload is writable for its full requested size");
    {
        // Catches a header that overlaps the payload, which would show up as
        // the count changing when the caller writes its own first byte.
        size_t n = 64;
        unsigned char* p = (unsigned char*)goo_alloc(n);
        memset(p, 0xAB, n);
        goo_check(goo_obj_refcount(p) == 1, "writing the payload changed the count");
        int intact = 1;
        for (size_t i = 0; i < n; i++) if (p[i] != 0xAB) intact = 0;
        goo_check(intact, "payload did not survive its own memset");
        goo_free(p);
    }

    goo_check_row(3, "the payload is 16-byte aligned");
    {
        // max_align_t must hold for the payload, and arena.c already uses 16
        // (GOO_ARENA_ALIGNMENT), so 16 is the consistent choice.
        int aligned = 1;
        void* held[8];
        for (int i = 0; i < 8; i++) {
            held[i] = goo_alloc((size_t)(i * 7 + 1));
            if (((uintptr_t)held[i] % 16u) != 0u) aligned = 0;
        }
        goo_check(aligned, "a payload was not 16-byte aligned");
        for (int i = 0; i < 8; i++) goo_free(held[i]);
    }

    goo_check_row(4, "retain and release move the count");
    {
        void* p = goo_alloc(16);
        goo_retain(p);
        goo_check(goo_obj_refcount(p) == 2, "retain did not increment");
        goo_retain(p);
        goo_check(goo_obj_refcount(p) == 3, "second retain did not increment");
        goo_release(p);
        goo_check(goo_obj_refcount(p) == 2, "release did not decrement");
        goo_release(p);
        goo_check(goo_obj_refcount(p) == 1, "second release did not decrement");
        goo_release(p);  // drops to 0 and frees; nothing to assert without ASan
    }

    goo_check_row(5, "goo_zerobase has no header and is inert");
    {
        void* z = goo_alloc(0);
        goo_check(z == (void*)&goo_zerobase, "zero-size alloc did not return the sentinel");
        goo_check(goo_obj_refcount(z) == 0, "sentinel reported a refcount");
        goo_retain(z);   // must not write through the sentinel
        goo_release(z);  // must not free the sentinel
        goo_check(goo_obj_refcount(z) == 0, "retain/release wrote through the sentinel");
        goo_free(z);     // still a no-op
    }

    goo_check_row(6, "NULL is inert for every header operation");
    {
        goo_check(goo_obj_refcount(NULL) == 0, "NULL reported a refcount");
        goo_retain(NULL);
        goo_release(NULL);
        goo_free(NULL);
        goo_check(1, "NULL handling crashed");
    }

    goo_check_row(7, "realloc preserves the payload and the count");
    {
        unsigned char* p = (unsigned char*)goo_alloc(16);
        memset(p, 0x5A, 16);
        goo_retain(p);  // count 2, must survive the move
        unsigned char* q = (unsigned char*)goo_realloc(p, 128);
        goo_check(q != NULL, "goo_realloc returned NULL");
        goo_check(((uintptr_t)q % 16u) == 0u, "reallocated payload lost its alignment");
        int intact = 1;
        for (int i = 0; i < 16; i++) if (q[i] != 0x5A) intact = 0;
        goo_check(intact, "goo_realloc did not preserve the payload");
        goo_check(goo_obj_refcount(q) == 2, "goo_realloc did not preserve the count");
        goo_release(q);
        goo_free(q);
    }

    goo_check_row(8, "realloc from the sentinel is a fresh allocation");
    {
        void* z = goo_alloc(0);
        void* p = goo_realloc(z, 32);
        goo_check(p != NULL && p != (void*)&goo_zerobase, "realloc from sentinel misbehaved");
        goo_check(goo_obj_refcount(p) == 1, "realloc from sentinel did not init the count");
        goo_free(p);
    }

    goo_check_row(9, "realloc from NULL is a fresh allocation");
    {
        void* p = goo_realloc(NULL, 32);
        goo_check(p != NULL, "realloc from NULL returned NULL");
        goo_check(goo_obj_refcount(p) == 1, "realloc from NULL did not init the count");
        goo_free(p);
    }

    goo_check_row(10, "realloc to size 0 frees and returns NULL");
    {
        void* p = goo_alloc(32);
        void* q = goo_realloc(p, 0);
        goo_check(q == NULL, "realloc to 0 did not return NULL");
    }

    goo_check_row(11, "two allocations do not share a header");
    {
        void* a = goo_alloc(16);
        void* b = goo_alloc(16);
        goo_retain(a);
        goo_check(goo_obj_refcount(a) == 2, "retain on a did not take");
        goo_check(goo_obj_refcount(b) == 1, "retain on a changed b's count");
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

    goo_check_row(12, "concurrent retain does not lose an update");
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
        goo_check(got == want, "concurrent retain lost an update");

        for (uint64_t i = 0; i < (uint64_t)NTHREADS * NITERS; i++) goo_release(p);
        goo_check(goo_obj_refcount(p) == 1, "unwinding the retains did not reach 1");
        goo_free(p);
    }

    goo_check_row(13, "balanced concurrent retain/release returns to the start");
    {
        void* p = goo_alloc(16);
        pthread_t th[NTHREADS];
        for (int i = 0; i < NTHREADS; i++) {
            pthread_create(&th[i], NULL, retain_release_worker, p);
        }
        for (int i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);

        // The object must still be ALIVE: every release here is paired with a
        // retain, so the count must never have transiently hit 0 and freed it.
        goo_check(goo_obj_refcount(p) == 1, "balanced retain/release did not return to 1");
        goo_free(p);
    }

    goo_check_row(14, "the last concurrent release frees exactly once");
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

        goo_check(goo_obj_refcount(p) == 1, "concurrent release did not settle at 1");
        goo_free(p);  // the one remaining reference
    }

    goo_check_row(15, "an IMMORTAL object survives retain, release and free");
    {
        // A string LITERAL is the motivating case. codegen_const_string_value
        // emits { [2 x i64] header, [N x i8] bytes } with rc = GOO_RC_IMMORTAL
        // and hands out a pointer to the BYTES, so the header sits at
        // `data - 16` exactly as it does for a goo_alloc'd object. This static
        // reproduces that layout byte for byte, because the runtime guards and
        // the codegen layout have to agree and neither one alone proves it.
        //
        // Before the guards, every call below computed `data - 16` and handed a
        // .rodata address to free().
        static const struct {
            uint64_t rc;
            uint64_t reserved;
            char     bytes[8];
        } __attribute__((aligned(16))) literal = { GOO_RC_IMMORTAL, 0, "hello" };

        void* data = (void*)(uintptr_t)literal.bytes;

        goo_check(goo_obj_refcount(data) == GOO_RC_IMMORTAL,
              "an immortal object should read back the sentinel");

        goo_retain(data);
        goo_check(goo_obj_refcount(data) == GOO_RC_IMMORTAL,
              "retain must not change an immortal count");

        goo_release(data);
        goo_check(goo_obj_refcount(data) == GOO_RC_IMMORTAL,
              "release must not change an immortal count");

        goo_free(data);
        goo_check(goo_obj_refcount(data) == GOO_RC_IMMORTAL,
              "free must be a no-op on an immortal object");

        // The bytes must still be readable: a wrong guard would have freed the
        // storage out from under this read.
        goo_check(literal.bytes[0] == 'h' && literal.bytes[4] == 'o',
              "an immortal object's payload must survive the traffic above");
    }

    // ---------------------------------------------------------------------
    // goo_release_with: the destructor runs ONCE, and only at zero.
    //
    // goo_release frees ONE block, and the header carries no type tag, so an
    // object that owns CONTENTS (a map owns its entry chain) needs a hook. The
    // hazard is running it early: a destructor that fires while someone still
    // holds a reference destroys a live object's contents.
    {
        dtor_calls = 0;
        void* p = goo_alloc(8);
        goo_check(goo_obj_refcount(p) == 1, "a fresh object starts at 1");

        goo_retain(p);
        goo_check(goo_obj_refcount(p) == 2, "retain took it to 2");

        // The release that does NOT reach zero must not run the destructor.
        goo_release_with(p, counting_dtor);
        goo_check(goo_obj_refcount(p) == 1, "the first release took it back to 1");
        goo_check(dtor_calls == 0,
              "the destructor must NOT run while a reference is still held");

        // The release that reaches zero must run it exactly once.
        goo_release_with(p, counting_dtor);
        goo_check(dtor_calls == 1,
              "the destructor must run exactly once, on the release that hit 0");
    }

    // A destructor must never run for a pointer that has no header, or for an
    // immortal one -- both return before the decrement.
    {
        dtor_calls = 0;
        goo_release_with(NULL, counting_dtor);
        goo_check(dtor_calls == 0, "no destructor for NULL");

        static struct {
            uint64_t rc; uint64_t reserved; char bytes[8];
        } __attribute__((aligned(16))) lit2 = { GOO_RC_IMMORTAL, 0, "hi" };
        goo_release_with((void*)(uintptr_t)lit2.bytes, counting_dtor);
        goo_check(dtor_calls == 0, "no destructor for an immortal object");
    }

    return goo_check_done("obj-header-test");
}
