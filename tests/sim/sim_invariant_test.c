/* tests/sim/sim_invariant_test.c — the invariant teeth of scripts/sim_probe.sh
 * --self-test (Task 6 of the 2026-09-03 deterministic-simulation plan). It
 * builds like every program under test: -Dmain=goo_sim_program_main, linked
 * against sim_main.o and the sim archive.
 *
 * The case is unlock-not-owner, the one violation of tests/sim/sim_os.c that
 * tests/sim/sim_entry_test.c leaves to this file. main locks a sync.Mutex on
 * coroutine 0; a goroutine unlocks it on a worker coroutine. Go allows this,
 * because a Go sync.Mutex has no owner. Goo's does: goo_sync_mutex_lock
 * takes a goo_mutex_t, which is a PTHREAD_MUTEX_DEFAULT in production, and
 * an unlock by a thread that does not own it is undefined behaviour there.
 * So the verdict invariant, detail unlock-not-owner, is the truth about this
 * program, and the driver must read exactly that.
 *
 * If the check does not fire, the unlock goes through, the goroutine ends,
 * goo_scheduler_wait returns, and main returns MISSED_STATUS, which is no
 * verdict code: a missing check can never read as the verdict it wants.
 *
 * Usage: GOO_SIM_TRACE_DIR=<dir> build/sim/sim_invariant_test */
#include "runtime.h"
#include "goo_os.h"
#include "sim.h"

#include <stdio.h>

enum { WORKERS = 1, MISSED_STATUS = 1 };

/* The Goo-visible sync.Mutex: one opaque pointer, zero value NULL. */
static void* g_mutex_slot;

/* Runs on a worker coroutine, never on coroutine 0, so it is not the owner. */
static void unlocker(void* arg) {
    GOO_ASSERT(arg == nullptr);
    GOO_ASSERT(sim_current()->id != 0);
    goo_sync_mutex_unlock(&g_mutex_slot);
    fprintf(stderr, "sim_invariant_test: the unlock by a non-owner went through\n");
}

int main(void) {
    GOO_ASSERT(sim_current()->id == 0);
    GOO_ASSERT(g_mutex_slot == nullptr);
    goo_scheduler_init(WORKERS);
    goo_sync_mutex_lock(&g_mutex_slot);
    GOO_ASSERT(g_mutex_slot != nullptr);
    goo_go(unlocker, nullptr);
    goo_scheduler_wait();
    fprintf(stderr, "sim_invariant_test: the unlock-not-owner check did not fire\n");
    return MISSED_STATUS;
}
