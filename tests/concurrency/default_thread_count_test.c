// Unit test for goo_default_thread_count(): GOMAXPROCS/NCPU resolution policy.
// Pure function, no threads — deterministic. Mirrors the spec's resolution table.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Resolver under test (external linkage in src/runtime/concurrency.c).
extern int goo_default_thread_count(void);

#define CAP 16

// Expected NCPU-path result: online CPU count, floored to 1, clamped to CAP.
static int expected_ncpu(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > CAP) n = CAP;
    return (int)n;
}

static int fails = 0;
static void check(const char* name, int got, int want) {
    if (got != want) { printf("FAIL %s: got %d want %d\n", name, got, want); fails++; }
    else { printf("ok %s = %d\n", name, got); }
}

int main(void) {
    setenv("GOMAXPROCS", "1", 1);
    check("gomaxprocs=1", goo_default_thread_count(), 1);

    setenv("GOMAXPROCS", "8", 1);          // honored even if NCPU < 8 (Go-faithful)
    check("gomaxprocs=8", goo_default_thread_count(), 8);

    setenv("GOMAXPROCS", "99", 1);         // above cap -> clamp to 16
    check("gomaxprocs=99->clamp", goo_default_thread_count(), CAP);

    setenv("GOMAXPROCS", "abc", 1);        // non-numeric -> NCPU
    check("gomaxprocs=garbage->ncpu", goo_default_thread_count(), expected_ncpu());

    setenv("GOMAXPROCS", "0", 1);          // < 1 -> NCPU
    check("gomaxprocs=0->ncpu", goo_default_thread_count(), expected_ncpu());

    unsetenv("GOMAXPROCS");                 // unset -> NCPU
    check("unset->ncpu", goo_default_thread_count(), expected_ncpu());

    if (fails) { printf("default-thread-count-test: FAIL (%d)\n", fails); return 1; }
    printf("default-thread-count-test: PASS\n");
    return 0;
}
