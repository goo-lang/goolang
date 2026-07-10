#include "runtime.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>

#ifdef GOO_PLATFORM_UNIX
    #include <pthread.h>
#endif

// External scheduler reference
extern goo_scheduler_t* g_scheduler;

// Mutex implementation
goo_mutex_t* goo_mutex_new(void) {
    goo_mutex_t* mutex = goo_alloc(sizeof(goo_mutex_t));
    mutex->locked = 0;
    
#ifdef GOO_PLATFORM_UNIX
    if (pthread_mutex_init(&mutex->mutex, NULL) != 0) {
        goo_free(mutex);
        return NULL;
    }
#endif
    
    return mutex;
}

void goo_mutex_free(goo_mutex_t* mutex) {
    if (!mutex) return;
    
#ifdef GOO_PLATFORM_UNIX
    pthread_mutex_destroy(&mutex->mutex);
#endif
    
    goo_free(mutex);
}

void goo_mutex_lock(goo_mutex_t* mutex) {
    if (!mutex) return;
    
#ifdef GOO_PLATFORM_UNIX
    pthread_mutex_lock(&mutex->mutex);
#endif
    
    mutex->locked = 1;
}

void goo_mutex_unlock(goo_mutex_t* mutex) {
    if (!mutex) return;
    
    mutex->locked = 0;
    
#ifdef GOO_PLATFORM_UNIX
    pthread_mutex_unlock(&mutex->mutex);
#endif
}

int goo_mutex_try_lock(goo_mutex_t* mutex) {
    if (!mutex) return 0;
    
#ifdef GOO_PLATFORM_UNIX
    if (pthread_mutex_trylock(&mutex->mutex) == 0) {
        mutex->locked = 1;
        return 1;  // Success
    }
#endif
    
    return 0;  // Failed to acquire
}

// Wait group implementation
goo_waitgroup_t* goo_waitgroup_new(void) {
    goo_waitgroup_t* wg = goo_alloc(sizeof(goo_waitgroup_t));
    wg->mutex = goo_mutex_new();
    wg->cond = goo_alloc(sizeof(goo_cond_t));
    wg->counter = 0;
    
#ifdef GOO_PLATFORM_UNIX
    pthread_cond_init(&wg->cond->cond, NULL);
#endif
    
    return wg;
}

void goo_waitgroup_free(goo_waitgroup_t* wg) {
    if (!wg) return;
    
#ifdef GOO_PLATFORM_UNIX
    pthread_cond_destroy(&wg->cond->cond);
#endif
    
    goo_free(wg->cond);
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
#ifdef GOO_PLATFORM_UNIX
        pthread_cond_broadcast(&wg->cond->cond);
#endif
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
#ifdef GOO_PLATFORM_UNIX
        pthread_cond_wait(&wg->cond->cond, &wg->mutex->mutex);
#endif
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