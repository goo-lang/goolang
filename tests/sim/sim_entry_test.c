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
 * Usage: SIM_ENTRY_CASE=<case> GOO_SIM_TRACE_DIR=<dir> build/sim/sim_entry_test */
#include "runtime.h"
#include "goo_os.h"
#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NONZERO_STATUS = 3, WORKERS = 2 };
static const uint64_t HANDLER_SLEEP_NS = 1000;

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

int main(void) {
    const char* which = getenv("SIM_ENTRY_CASE");
    if (which == nullptr) {
        fprintf(stderr, "sim_entry_test: set SIM_ENTRY_CASE\n");
        return 1;
    }
    if (strcmp(which, "coroutine0") == 0) return case_coroutine0();
    if (strcmp(which, "nonzero") == 0) return case_nonzero();
    if (strcmp(which, "verdict-output") == 0) return case_verdict_output();
    fprintf(stderr, "sim_entry_test: unknown SIM_ENTRY_CASE '%s'\n", which);
    return 1;
}
