#include "runtime.h"
#include "platform.h"
#include "goo_os.h"
#include <stdlib.h>
#include <string.h>

// External scheduler reference
extern goo_scheduler_t* g_scheduler;

// Mutex implementation moved to src/runtime/goo_os_posix.c (deterministic-
// simulation task 1): goo_mutex_new/free/lock/unlock/try_lock are now seam
// calls, declared in include/goo_os.h.

// Wait group implementation, rewritten over the seam's mutex and condvar
// (deterministic-simulation task 1): this file makes no direct OS thread
// library call any more.
goo_waitgroup_t* goo_waitgroup_new(void) {
    goo_waitgroup_t* wg = goo_alloc(sizeof(goo_waitgroup_t));
    wg->mutex = goo_mutex_new();
    wg->cond = goo_cond_new();
    wg->counter = 0;

    return wg;
}

void goo_waitgroup_free(goo_waitgroup_t* wg) {
    if (!wg) return;

    goo_cond_free(wg->cond);
    goo_mutex_free(wg->mutex);
    goo_free(wg);
}

void goo_waitgroup_add(goo_waitgroup_t* wg, int delta) {
    if (!wg) return;

    goo_mutex_lock(wg->mutex);
    wg->counter += delta;

    if (wg->counter < 0) {
        // Go's exact message (sync/waitgroup.go) — review parity fix: the
        // earlier "WaitGroup counter went negative" wording diverged from
        // upstream. Pinned by sync_wg_negative (golden, exit 2 + stderr).
        goo_panic("sync: negative WaitGroup counter");
    }

    if (wg->counter == 0) {
        // Wake up all waiting goroutines
        goo_cond_broadcast(wg->cond);
    }

    goo_mutex_unlock(wg->mutex);
}

void goo_waitgroup_done(goo_waitgroup_t* wg) {
    goo_waitgroup_add(wg, -1);
}

void goo_waitgroup_wait(goo_waitgroup_t* wg) {
    if (!wg) return;

    goo_mutex_lock(wg->mutex);

    while (wg->counter > 0) {
        goo_cond_wait(wg->cond, wg->mutex);
    }

    goo_mutex_unlock(wg->mutex);
}

// Runtime statistics
goo_runtime_stats_t goo_get_runtime_stats(void) {
    goo_runtime_stats_t stats = {0};
    
    if (g_scheduler) {
        goo_mutex_lock(g_scheduler->scheduler_mutex);
        stats = g_scheduler->stats;
        goo_mutex_unlock(g_scheduler->scheduler_mutex);
    }
    
    return stats;
}
