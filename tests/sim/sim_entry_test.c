/* tests/sim/sim_entry_test.c — a program under the simulator that checks
 * the process entry (Task 5 of the 2026-09-03 deterministic-simulation
 * plan). It builds like every program under test: -Dmain=goo_sim_program_main,
 * linked against sim_main.o and the sim archive. tests/sim/sim_entry_test.sh
 * runs it once per case and judges the exit status, the output and the
 * trace header.
 *
 * SIM_ENTRY_CASE selects the case:
 *   coroutine0      main runs as coroutine 0 (spec section 6), and the exit
 *                   path -- an atexit handler that makes seam calls while
 *                   the runtime's workers are live -- runs on coroutine 0
 *                   too, never on the real thread, where sim_current()
 *                   would abort.
 *   nonzero         main returns 3: the exit status passes through unchanged.
 *   verdict-output  a line goes to stdout, then a sim-deadlock: the line must
 *                   survive the verdict's _exit.
 *
 * The violation cases (spec section 7: "a self-test that drives each
 * invariant to a violation on purpose"). Each one breaks ONE rule of
 * tests/sim/sim_os.c; the driver reads the exit status (11, or 15 for the
 * table limit) AND the detail in the trace header, so it knows that this
 * check fired and not another. If no check fires, the case returns
 * MISSED_STATUS, which is no verdict code.
 *   lock-order, recursive-lock, unlock-not-held, wait-without-mutex,
 *   wait-other-owner, exit-holding-mutex, free-held-mutex,
 *   free-cond-with-waiters, join-self, join-detached, unknown-mutex,
 *   limit-mutexes.
 * unlock-not-owner is Task 6's case (tests/sim/sim_invariant_test.c).
 *
 * Usage: SIM_ENTRY_CASE=<case> GOO_SIM_TRACE_DIR=<dir> build/sim/sim_entry_test */
#include "runtime.h"
#include "goo_os.h"
#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NONZERO_STATUS = 3, WORKERS = 2, MISSED_STATUS = 1, WAIT_POLLS_MAX = 100 };
static const uint64_t HANDLER_SLEEP_NS = 1000;
static const uint64_t NAP_NS = 1000;

/* concurrency.c's scheduler: the lock-order case takes its mutex. */
extern goo_scheduler_t* g_scheduler;

/* Every call here is a seam call. Each one asserts through sim_current()
 * that it runs on a coroutine, so on the real thread this would abort. */
static void exit_handler(void) {
    uint32_t id = sim_current()->id;
    GOO_ASSERT(id < SIM_THREADS_MAX);
    goo_mutex_t* mutex = goo_mutex_new();
    GOO_ASSERT(mutex != nullptr);
    goo_mutex_lock(mutex);
    goo_platform_sleep_ns(HANDLER_SLEEP_NS);   /* lets the workers run */
    goo_mutex_unlock(mutex);
    goo_mutex_free(mutex);
    GOO_ASSERT(sim_current()->id == id);
    printf("atexit-coroutine=%u\n", sim_current()->id);
}

static int case_coroutine0(void) {
    uint32_t id = sim_current()->id;
    printf("main-coroutine=%u\n", id);
    goo_scheduler_init(WORKERS);               /* live workers during the exit path */
    GOO_ASSERT(sim_current()->id == id);
    int rc = atexit(exit_handler);
    GOO_ASSERT(rc == 0);
    return 0;
}

static int case_nonzero(void) {
    printf("nonzero-returns=%d\n", NONZERO_STATUS);
    GOO_ASSERT(sim_current()->id == 0);
    return NONZERO_STATUS;
}

/* A wait that no thread can end: nothing runnable and nothing sleeping is
 * the verdict sim-deadlock, which ends the process with _exit. */
static int case_verdict_output(void) {
    printf("output-before-verdict\n");         /* buffered: stdout is a file */
    goo_mutex_t* mutex = goo_mutex_new();
    goo_cond_t* cond = goo_cond_new();
    GOO_ASSERT(mutex != nullptr);
    GOO_ASSERT(cond != nullptr);
    goo_mutex_lock(mutex);
    goo_cond_wait(cond, mutex);
    fprintf(stderr, "sim_entry_test: the wait returned; the deadlock verdict did not fire\n");
    return 1;
}

/* ---- the violation cases ---- */

static int missed(const char* check) {
    GOO_ASSERT(check != nullptr);
    fprintf(stderr, "sim_entry_test: the %s check did not fire\n", check);
    return MISSED_STATUS;
}

/* The scheduler mutex, then a channel-like mutex: the reverse of the order
 * the runtime keeps (channel mutex first). The first lock may wait for a
 * worker, which takes the same mutex in its loop. */
static int case_lock_order(void) {
    goo_scheduler_init(1);
    GOO_ASSERT(g_scheduler != nullptr);
    GOO_ASSERT(g_scheduler->scheduler_mutex != nullptr);
    goo_mutex_t* other = goo_mutex_new();
    goo_mutex_lock(g_scheduler->scheduler_mutex);
    goo_mutex_lock(other);
    return missed("lock-order");
}

static int case_recursive_lock(void) {
    goo_mutex_t* mutex = goo_mutex_new();
    goo_mutex_lock(mutex);
    goo_mutex_lock(mutex);
    return missed("recursive-lock");
}

static int case_unlock_not_held(void) {
    goo_mutex_t* mutex = goo_mutex_new();
    goo_mutex_lock(mutex);
    goo_mutex_unlock(mutex);
    goo_mutex_unlock(mutex);                   /* the double unlock */
    return missed("unlock-not-held");
}

static int case_wait_without_mutex(void) {
    goo_mutex_t* mutex = goo_mutex_new();
    goo_cond_t* cond = goo_cond_new();
    goo_cond_wait(cond, mutex);                /* the mutex is not held */
    return missed("wait-without-mutex");
}

static goo_mutex_t* g_shared_mutex;
static goo_cond_t* g_shared_cond;
static goo_thread_t* volatile g_self_handle;

/* Holds g_shared_mutex and never lets it go. */
static void* hold_forever(void* arg) {
    goo_mutex_lock(g_shared_mutex);
    for (uint32_t i = 0; i < WAIT_POLLS_MAX; i++) {
        goo_platform_sleep_ns(NAP_NS);
    }
    goo_mutex_unlock(g_shared_mutex);
    return arg;
}

/* The mutex is held, but by the other thread. */
static int case_wait_other_owner(void) {
    g_shared_mutex = goo_mutex_new();
    goo_cond_t* cond = goo_cond_new();
    goo_thread_t* holder = goo_thread_create(hold_forever, nullptr);
    GOO_ASSERT(holder != nullptr);
    goo_platform_sleep_ns(NAP_NS);             /* the holder runs and takes it */
    goo_cond_wait(cond, g_shared_mutex);
    return missed("wait-without-mutex (other owner)");
}

static void* lock_and_return(void* arg) {
    goo_mutex_lock(g_shared_mutex);
    return arg;                                /* exits with the mutex held */
}

static int case_exit_holding_mutex(void) {
    g_shared_mutex = goo_mutex_new();
    goo_thread_t* thread = goo_thread_create(lock_and_return, nullptr);
    GOO_ASSERT(thread != nullptr);
    goo_thread_join(thread);
    return missed("exit-holding-mutex");
}

static int case_free_held_mutex(void) {
    goo_mutex_t* mutex = goo_mutex_new();
    goo_mutex_lock(mutex);
    goo_mutex_free(mutex);
    return missed("free-held-mutex");
}

static void* wait_on_shared_cond(void* arg) {
    goo_mutex_lock(g_shared_mutex);
    goo_cond_wait(g_shared_cond, g_shared_mutex);
    goo_mutex_unlock(g_shared_mutex);
    return arg;
}

/* While main sleeps, the waiter is the only runnable thread, so it parks
 * on the condvar before main wakes, on every seed. */
static int case_free_cond_with_waiters(void) {
    g_shared_mutex = goo_mutex_new();
    g_shared_cond = goo_cond_new();
    goo_thread_t* waiter = goo_thread_create(wait_on_shared_cond, nullptr);
    GOO_ASSERT(waiter != nullptr);
    goo_platform_sleep_ns(NAP_NS);
    goo_cond_free(g_shared_cond);
    return missed("free-cond-with-waiters");
}

/* Waits until main publishes this thread's own handle, then joins it. */
static void* join_own_handle(void* arg) {
    for (uint32_t i = 0; i < WAIT_POLLS_MAX && g_self_handle == nullptr; i++) {
        goo_platform_sleep_ns(NAP_NS);
    }
    GOO_ASSERT(g_self_handle != nullptr);
    goo_thread_join(g_self_handle);
    return arg;
}

static int case_join_self(void) {
    goo_thread_t* thread = goo_thread_create(join_own_handle, nullptr);
    GOO_ASSERT(thread != nullptr);
    g_self_handle = thread;
    goo_thread_join(thread);
    return missed("join-self");
}

static void* return_at_once(void* arg) { return arg; }

static int case_join_detached(void) {
    goo_thread_t* thread = goo_thread_create(return_at_once, nullptr);
    GOO_ASSERT(thread != nullptr);
    goo_thread_detach(thread);
    goo_thread_join(thread);
    return missed("join-detached");
}

/* A mutex the seam never made: goo_mutex_new did not register it. */
static int case_unknown_mutex(void) {
    static goo_mutex_t stranger;
    goo_mutex_lock(&stranger);
    return missed("unknown-mutex");
}

/* One more live mutex than the table holds. */
static int case_limit_mutexes(void) {
    for (uint32_t i = 0; i <= SIM_MUTEXES_MAX; i++) {
        GOO_ASSERT(goo_mutex_new() != nullptr);
    }
    return missed("limit-mutexes");
}

typedef struct entry_case {
    const char* name;
    int (*run)(void);
} entry_case_t;

static const entry_case_t CASES[] = {
    { "coroutine0", case_coroutine0 },
    { "nonzero", case_nonzero },
    { "verdict-output", case_verdict_output },
    { "lock-order", case_lock_order },
    { "recursive-lock", case_recursive_lock },
    { "unlock-not-held", case_unlock_not_held },
    { "wait-without-mutex", case_wait_without_mutex },
    { "wait-other-owner", case_wait_other_owner },
    { "exit-holding-mutex", case_exit_holding_mutex },
    { "free-held-mutex", case_free_held_mutex },
    { "free-cond-with-waiters", case_free_cond_with_waiters },
    { "join-self", case_join_self },
    { "join-detached", case_join_detached },
    { "unknown-mutex", case_unknown_mutex },
    { "limit-mutexes", case_limit_mutexes },
};

int main(void) {
    const char* which = getenv("SIM_ENTRY_CASE");
    if (which == nullptr) {
        fprintf(stderr, "sim_entry_test: set SIM_ENTRY_CASE\n");
        return 1;
    }
    for (uint32_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        if (strcmp(which, CASES[i].name) == 0) return CASES[i].run();
    }
    fprintf(stderr, "sim_entry_test: unknown SIM_ENTRY_CASE '%s'\n", which);
    return 1;
}
