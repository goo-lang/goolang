// The runtime defer stack: LIFO order, growth, reuse, and the zeroed frame.
//
// 43 lines, two exported functions, and nothing exercised them until the
// runtime package was split. This suite names //src/runtime:defer.
//
// WHY THE ORDER IS THE POINT. goo_defer_run unwinds last-pushed-first, which
// is what makes `defer` mean what Go says it means. An implementation that ran
// them forwards would pass any test that only counted calls, so every order
// row below records the SEQUENCE and compares it, never the count alone.
//
// THE FREE IS ASSERTED BY VALGRIND, NOT BY A CHECK. goo_defer_run frees each
// entry's env and then the entries array. Nothing in this process can observe
// that from the outside without an allocation-interception harness this repo
// does not have. So the suite allocates every env with goo_alloc, and the
// BUILD comment records that it is run under --config=valgrind: a run that
// stopped freeing would show up there as a leak, and here as nothing at all.
// Do not read a green run of this file alone as evidence that the frees happen.

#include "runtime.h"
#include "../goo_check.h"
#include <string.h>

#define MAX_SEEN 32

static int seen[MAX_SEEN];
static int seen_n;

static void record(void* env) {
    if (seen_n < MAX_SEEN) {
        seen[seen_n++] = env ? *(int*)env : -1;
    }
}

// A defer whose function pointer is NULL must be skipped, not called.
static void never_called(void* env) {
    (void)env;
    if (seen_n < MAX_SEEN) {
        seen[seen_n++] = 999;
    }
}

// Push one entry carrying `value`, with the env allocated the way codegen
// allocates it, so goo_defer_run's goo_free is operating on a real ARC block.
static void push_value(goo_defer_frame_t* f, int value) {
    int* env = goo_alloc(sizeof(int));
    *env = value;
    goo_defer_push(f, record, env);
}

static int seq_is(const int* want, int n) {
    if (seen_n != n) return 0;
    for (int i = 0; i < n; i++) {
        if (seen[i] != want[i]) return 0;
    }
    return 1;
}

int main(void) {
    goo_check_expect(7);

    // ---------------------------------------------------------------------
    goo_check_row(0, "NULL is tolerated by both entry points");
    goo_defer_push(NULL, record, NULL);
    goo_defer_run(NULL);
    goo_check(1, "push(NULL, ...) and run(NULL) returned");

    // ---------------------------------------------------------------------
    // runtime.h states this outright: "goo_defer_run is safe on a never-pushed
    // (zeroed) frame". Codegen emits the run on every exit path, including
    // paths where no defer ever executed.
    goo_check_row(1, "a zeroed frame runs safely and stays zeroed");
    goo_defer_frame_t f;
    memset(&f, 0, sizeof f);
    seen_n = 0;
    goo_defer_run(&f);
    goo_check(seen_n == 0, "no deferred function ran");
    goo_check(f.entries == NULL && f.len == 0 && f.cap == 0, "the frame is still zeroed");

    // ---------------------------------------------------------------------
    // THE LOAD-BEARING ROW.
    goo_check_row(2, "entries unwind last-pushed-first");
    memset(&f, 0, sizeof f);
    seen_n = 0;
    for (int i = 1; i <= 3; i++) {
        push_value(&f, i);
    }
    goo_defer_run(&f);
    {
        const int want[] = {3, 2, 1};
        goo_check(seq_is(want, 3), "ran 3, 2, 1 -- not 1, 2, 3");
    }

    // ---------------------------------------------------------------------
    // cap goes 0 -> 4 -> 8, so nine entries force two grows. Order must hold
    // across the goo_realloc that moves the array.
    goo_check_row(3, "order survives the growth that reallocates the array");
    memset(&f, 0, sizeof f);
    seen_n = 0;
    for (int i = 1; i <= 9; i++) {
        push_value(&f, i);
    }
    goo_check(f.len == 9, "nine entries were pushed");
    goo_check(f.cap >= 9, "capacity grew to hold them");
    goo_defer_run(&f);
    {
        const int want[] = {9, 8, 7, 6, 5, 4, 3, 2, 1};
        goo_check(seq_is(want, 9), "all nine unwound in reverse");
    }

    // ---------------------------------------------------------------------
    goo_check_row(4, "run resets the frame, so it can be used again");
    goo_check(f.entries == NULL && f.len == 0 && f.cap == 0, "the frame is zeroed after run");
    seen_n = 0;
    push_value(&f, 42);
    goo_defer_run(&f);
    {
        const int want[] = {42};
        goo_check(seq_is(want, 1), "a fresh push on the reused frame ran");
    }

    // ---------------------------------------------------------------------
    // The env of a zero-argument defer is NULL, and goo_free is a documented
    // no-op on it. The entry must still run.
    goo_check_row(5, "an entry with a NULL env still runs");
    memset(&f, 0, sizeof f);
    seen_n = 0;
    goo_defer_push(&f, record, NULL);
    goo_defer_run(&f);
    {
        const int want[] = {-1};
        goo_check(seq_is(want, 1), "the entry ran and reported a NULL env");
    }

    // ---------------------------------------------------------------------
    goo_check_row(6, "an entry with a NULL function is skipped, not called");
    memset(&f, 0, sizeof f);
    seen_n = 0;
    push_value(&f, 7);
    goo_defer_push(&f, NULL, NULL);
    goo_defer_push(&f, never_called, NULL);
    // Overwrite the last entry's fn, so the frame carries a NULL fn in the
    // middle of a run rather than only at its end.
    f.entries[2].fn = NULL;
    goo_defer_run(&f);
    {
        const int want[] = {7};
        goo_check(seq_is(want, 1), "only the entry with a function ran");
    }

    return goo_check_done("defer");
}
