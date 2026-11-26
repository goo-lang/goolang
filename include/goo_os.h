/* include/goo_os.h — the seam. Production: src/runtime/goo_os_posix.c.
 * Simulator: tests/sim/sim_os.c. The two never link together. */
#ifndef GOO_OS_H
#define GOO_OS_H
#include <stdint.h>
#include <ucontext.h>
#include "runtime.h"        /* goo_mutex_t, goo_cond_t, goo_goroutine_t,
                              * goo_thread_t (forward-declared in runtime.h) */

typedef void* (*goo_thread_func_t)(void* arg);

/* Mutex: names unchanged, definitions move from sync.c. */
goo_mutex_t* goo_mutex_new(void);
void         goo_mutex_free(goo_mutex_t* mutex);
void         goo_mutex_lock(goo_mutex_t* mutex);
void         goo_mutex_unlock(goo_mutex_t* mutex);
int          goo_mutex_try_lock(goo_mutex_t* mutex);

/* Condvar: new. Replaces every pthread_cond_* in channels.c and sync.c. */
goo_cond_t*  goo_cond_new(void);
void         goo_cond_free(goo_cond_t* cond);
void         goo_cond_wait(goo_cond_t* cond, goo_mutex_t* mutex);
void         goo_cond_signal(goo_cond_t* cond);
void         goo_cond_broadcast(goo_cond_t* cond);

/* Threads: names unchanged, definitions move from platform.c. */
goo_thread_t* goo_thread_create(goo_thread_func_t func, void* arg);
void          goo_thread_join(goo_thread_t* thread);
void          goo_thread_detach(goo_thread_t* thread);

/* Clock and sleep: names unchanged, definitions move from platform.c. */
uint64_t goo_platform_time_ns(void);
uint64_t goo_platform_wall_time_ns(void);
void     goo_platform_sleep_ns(uint64_t ns);

/* The per-worker state that concurrency.c kept in four _Thread_local
 * variables. One real thread per worker in production; one coroutine per
 * worker in the simulator, each with its own copy. */
typedef struct goo_worker_tls {
    ucontext_t       sched_ctx;
    goo_goroutine_t* current;
    goo_goroutine_t* reap;
    goo_goroutine_t* requeue;
} goo_worker_tls_t;
goo_worker_tls_t* goo_os_tls(void);
#endif
