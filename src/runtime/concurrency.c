#define _XOPEN_SOURCE 700  // For ucontext functions

#include "runtime.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#ifdef GOO_PLATFORM_UNIX
    #include <pthread.h>
    #include <unistd.h>
    #include <sys/mman.h>
    #include <ucontext.h>
#endif

// Goroutine stack size (64KB by default)
#define GOO_GOROUTINE_STACK_SIZE (64 * 1024)

// Maximum number of OS threads in scheduler
#define GOO_MAX_OS_THREADS 16

// Structure definitions are now in runtime.h

// Global scheduler instance
goo_scheduler_t* g_scheduler = NULL;

// M8c: per-worker scheduler state. Each OS worker thread has its own scheduler
// return context and "currently running goroutine". These supersede the shared
// g_scheduler->main_context / ->current_goroutine fields (which are left unused
// to avoid a header change). Sharing those single fields across N workers was
// the multi-thread corruption (stomped return context / wrong current).
//
// t_reap carries the disposition decision back to the worker WITHOUT
// dereferencing the goroutine after the swap: a goroutine that EXITS sets
// t_reap to itself (it is never re-enqueued, so only this worker references it,
// so this worker frees it); a goroutine that YIELDS leaves t_reap NULL (it
// re-enqueued itself and may already be running/freed on another worker, so
// this worker must not touch it). ucontext keeps the same OS thread across the
// switch, so the goroutine and its worker share these thread-locals.
static _Thread_local ucontext_t       t_sched_ctx;
static _Thread_local goo_goroutine_t* t_current = NULL;
static _Thread_local goo_goroutine_t* t_reap = NULL;
// M8d: a yielding goroutine hands itself here; the scheduler re-enqueues it
// AFTER swapcontext has saved its context (mirrors t_reap). Re-enqueuing from
// inside goo_yield (before the swap) would publish the goroutine to other
// workers before its context is written → race. Mutually exclusive with t_reap.
static _Thread_local goo_goroutine_t* t_requeue = NULL;

// Forward declarations
static void* scheduler_main_loop(void* arg);
static void goroutine_wrapper(void);
static goo_goroutine_t* scheduler_get_next_goroutine(void);
static void scheduler_add_goroutine(goo_goroutine_t* goroutine);

// Scheduler initialization
void goo_scheduler_init(int num_threads) {
    if (g_scheduler) {
        return;  // Already initialized
    }
    
    if (num_threads <= 0) {
        num_threads = 1;  // Default to single-threaded
    }
    if (num_threads > GOO_MAX_OS_THREADS) {
        num_threads = GOO_MAX_OS_THREADS;
    }
    
    g_scheduler = goo_alloc(sizeof(goo_scheduler_t));
    memset(g_scheduler, 0, sizeof(goo_scheduler_t));
    
    g_scheduler->num_threads = num_threads;
    g_scheduler->running = 1;
    g_scheduler->next_goroutine_id = 1;
    g_scheduler->next_channel_id = 1;
    g_scheduler->scheduler_mutex = goo_mutex_new();
    
#ifdef GOO_PLATFORM_UNIX
    // Create OS threads for the scheduler
    g_scheduler->os_threads = goo_alloc(sizeof(pthread_t) * num_threads);
    
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&g_scheduler->os_threads[i], NULL, 
                      scheduler_main_loop, 
                      (void*)(intptr_t)i);
    }
#endif
}

void goo_scheduler_shutdown(void) {
    if (!g_scheduler) {
        return;
    }
    
    g_scheduler->running = 0;
    
#ifdef GOO_PLATFORM_UNIX
    // Wait for all OS threads to finish
    for (int i = 0; i < g_scheduler->num_threads; i++) {
        pthread_join(g_scheduler->os_threads[i], NULL);
    }
    goo_free(g_scheduler->os_threads);
#endif
    
    goo_mutex_free(g_scheduler->scheduler_mutex);
    goo_free(g_scheduler);
    g_scheduler = NULL;
}

// Run-to-completion barrier: block the calling thread until all spawned
// goroutines have finished. Generated main() calls this before returning so
// goroutine side effects are observable. Polls under the scheduler mutex,
// matching the scheduler loop's existing sleep-based idiom. Returns immediately
// if no scheduler exists (no goroutine was ever spawned), and bails out if the
// scheduler has stopped (e.g. deadlock detector tripped) to avoid hanging.
void goo_scheduler_wait(void) {
    if (!g_scheduler) {
        return;  // No goroutines were ever started.
    }

    for (;;) {
        goo_mutex_lock(g_scheduler->scheduler_mutex);
        int done = (g_scheduler->stats.num_goroutines == 0 &&
                    g_scheduler->ready_queue == NULL);
        int stopped = !g_scheduler->running;
        goo_mutex_unlock(g_scheduler->scheduler_mutex);

        if (done || stopped) {
            break;
        }

        goo_platform_sleep_ns(500000);  // 0.5ms
    }
}

// Goroutine creation
goo_goroutine_t* goo_go(goo_goroutine_func_t func, void* arg) {
    if (!g_scheduler) {
        goo_scheduler_init(1);  // Initialize with default settings
    }
    
    goo_goroutine_t* goroutine = goo_alloc(sizeof(goo_goroutine_t));
    memset(goroutine, 0, sizeof(goo_goroutine_t));
    
    goroutine->state = GOO_GOROUTINE_READY;
    goroutine->function = func;
    goroutine->arg = arg;
    goroutine->stack_size = GOO_GOROUTINE_STACK_SIZE;
    goroutine->creation_time = goo_platform_time_ns();
    
    goo_mutex_lock(g_scheduler->scheduler_mutex);
    goroutine->id = g_scheduler->next_goroutine_id++;
    g_scheduler->stats.num_goroutines++;
    goo_mutex_unlock(g_scheduler->scheduler_mutex);
    
    // Allocate stack
    goroutine->stack = goo_alloc(goroutine->stack_size);
    
#ifdef GOO_PLATFORM_UNIX
    // Set up goroutine context
    // Note: ucontext functions are deprecated on macOS 10.6+ but still functional
    // 
    // Future alternatives to consider:
    // - Grand Central Dispatch (libdispatch) for task-based concurrency
    // - Custom pthread + setjmp/longjmp implementation
    // - Assembly-based context switching for performance
    // - Third-party libraries (libco, boost::context, libtask)
    //
    // The current implementation works on all Unix-like systems and provides
    // the necessary goroutine functionality for the Goo language runtime.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (getcontext(&goroutine->context) == -1) {
        goo_panic("Failed to get goroutine context");
    }
    
    goroutine->context.uc_stack.ss_sp = goroutine->stack;
    goroutine->context.uc_stack.ss_size = goroutine->stack_size;
    goroutine->context.uc_link = NULL;  // overridden by scheduler_main_loop before each swap
    
    makecontext(&goroutine->context, goroutine_wrapper, 0);
#pragma clang diagnostic pop
#endif
    
    // Add to scheduler
    scheduler_add_goroutine(goroutine);
    
    return goroutine;
}

void goo_yield(void) {
    goo_goroutine_t* current = t_current;
    if (!g_scheduler || !current) {
        return;
    }

    current->state = GOO_GOROUTINE_READY;
    t_requeue = current;   // scheduler re-enqueues AFTER the swap saves our context

#ifdef GOO_PLATFORM_UNIX
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (swapcontext(&current->context, &t_sched_ctx) == -1) {
        goo_panic("Failed to yield goroutine");
    }
#pragma clang diagnostic pop
#endif
}

void goo_goroutine_exit(void) {
    goo_goroutine_t* current = t_current;
    if (!g_scheduler || !current) {
        return;
    }

    current->state = GOO_GOROUTINE_DONE;

    goo_mutex_lock(g_scheduler->scheduler_mutex);
    g_scheduler->stats.num_goroutines--;
    goo_mutex_unlock(g_scheduler->scheduler_mutex);

    // Hand this goroutine to the worker for reaping. Do NOT free current->stack
    // or current here: this code is executing ON current->stack. Setting t_reap
    // (this OS thread's thread-local) tells the worker — after swapcontext
    // returns onto its own stack — to free us. A yielding goroutine leaves
    // t_reap NULL, so it is never freed from under another worker.
    t_reap = current;

    // Return control to this worker's scheduler context.
#ifdef GOO_PLATFORM_UNIX
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    setcontext(&t_sched_ctx);
#pragma clang diagnostic pop
#endif
}

// Goroutine wrapper function
static void goroutine_wrapper(void) {
    goo_goroutine_t* current = t_current;
    if (current && current->function) {
        current->function(current->arg);
    }
    goo_goroutine_exit();
}

// Scheduler internal functions
static void* scheduler_main_loop(void* arg) {
    int thread_id = (int)(intptr_t)arg;
    (void)thread_id;  // Suppress unused warning
    
    while (g_scheduler->running) {
        goo_goroutine_t* goroutine = scheduler_get_next_goroutine();
        
        if (goroutine) {
            t_current = goroutine;
            t_reap = NULL;                 // cleared each run; the goroutine sets it on exit
            t_requeue = NULL;              // cleared each run; the goroutine sets it on yield
            goroutine->state = GOO_GOROUTINE_RUNNING;

            goo_mutex_lock(g_scheduler->scheduler_mutex);
            g_scheduler->stats.context_switches++;
            goo_mutex_unlock(g_scheduler->scheduler_mutex);

#ifdef GOO_PLATFORM_UNIX
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            // Return to THIS worker's scheduler context when the goroutine
            // exits or yields.
            goroutine->context.uc_link = &t_sched_ctx;
            if (swapcontext(&t_sched_ctx, &goroutine->context) == -1) {
                goo_panic("Failed to switch to goroutine");
            }
#pragma clang diagnostic pop
#endif

            // Back on this worker's own stack. Reap ONLY via t_reap, which the
            // goroutine set (to itself) iff it EXITED. Do NOT read goroutine->*
            // here: if it yielded, it re-enqueued itself and another worker may
            // already have run and freed it — dereferencing it would be a UAF.
            // A DONE goroutine is never re-enqueued, so t_reap is ours alone.
            if (t_reap) {
                goo_free(t_reap->stack);
                goo_free(t_reap);
                t_reap = NULL;
            }
            // A yielded goroutine is published to the ready queue ONLY here —
            // after swapcontext has fully saved its context — so no other worker
            // can swap into a half-saved context.
            if (t_requeue) {
                scheduler_add_goroutine(t_requeue);
                t_requeue = NULL;
            }
            t_current = NULL;
        } else {
            // No goroutines ready, check for deadlock
            if (goo_deadlock_check()) {
                // Deadlock detected, stop scheduler
                break;
            }
            
            // Sleep briefly
            goo_platform_sleep_ns(1000000);  // 1ms
        }
        
        goo_mutex_lock(g_scheduler->scheduler_mutex);
        g_scheduler->stats.scheduler_cycles++;
        goo_mutex_unlock(g_scheduler->scheduler_mutex);
    }
    
    return NULL;
}

static goo_goroutine_t* scheduler_get_next_goroutine(void) {
    goo_mutex_lock(g_scheduler->scheduler_mutex);
    
    goo_goroutine_t* goroutine = g_scheduler->ready_queue;
    if (goroutine) {
        g_scheduler->ready_queue = goroutine->next;
        goroutine->next = NULL;
    }
    
    goo_mutex_unlock(g_scheduler->scheduler_mutex);
    return goroutine;
}

static void scheduler_add_goroutine(goo_goroutine_t* goroutine) {
    goo_mutex_lock(g_scheduler->scheduler_mutex);
    
    goroutine->next = g_scheduler->ready_queue;
    g_scheduler->ready_queue = goroutine;
    
    goo_mutex_unlock(g_scheduler->scheduler_mutex);
}