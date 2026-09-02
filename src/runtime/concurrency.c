#define _XOPEN_SOURCE 700  // For ucontext functions

#include "runtime.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdatomic.h>

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
// The worker decides a goroutine's disposition after the swap WITHOUT
// dereferencing it, via two mutually-exclusive thread-locals: a goroutine that
// EXITS sets t_reap to itself (it is never re-enqueued, so only this worker
// references it → this worker frees it); a goroutine that YIELDS sets t_requeue
// to itself and leaves t_reap NULL (the worker re-enqueues it below, AFTER the
// swap has saved its context — never before, or another worker could swap into
// a half-saved context). Either way the worker must not touch goroutine->* after
// the swap until the t_reap/t_requeue branches run. ucontext keeps the same OS
// thread across the switch, so the goroutine and its worker share these
// thread-locals.
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

// Resolve the default OS-thread count for lazy scheduler init.
// Policy (Go-faithful): GOMAXPROCS env if it parses to an integer >= 1 (honored
// even above NCPU), else the online CPU count; clamped to [1, GOO_MAX_OS_THREADS].
// Side-effect-free. Exposed (non-static) so the resolver is unit-testable.
int goo_default_thread_count(void) {
    const char* env = getenv("GOMAXPROCS");
    if (env && env[0] != '\0') {
        char* end = NULL;
        long n = strtol(env, &end, 10);
        // Whole string must be a valid integer >= 1; otherwise fall through.
        if (end != env && *end == '\0' && n >= 1) {
            return (n > GOO_MAX_OS_THREADS) ? GOO_MAX_OS_THREADS : (int)n;
        }
        // invalid / < 1 -> NCPU (Go ignores an invalid GOMAXPROCS)
    }
#ifdef GOO_PLATFORM_UNIX
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;                 // sysconf failure -> safe floor
    return (ncpu > GOO_MAX_OS_THREADS) ? GOO_MAX_OS_THREADS : (int)ncpu;
#else
    return 1;
#endif
}

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

    // THE DEADLOCK DETECTOR. `blocked_goroutines` is the size of a SET, and a
    // goroutine leaves that set at the instant its waker decides to wake it
    // (goo_sched_wake_waiters), never when it is next scheduled. So
    // (blocked_goroutines == num_goroutines) with an empty ready queue means
    // every live goroutine is parked and NO waker has released any of them:
    // nothing in the process can change that state, and it is a real deadlock.
    //
    // WHAT THIS REPLACES, and why the old numbers are gone. Until 2026-09-02 a
    // goroutine counted as blocked from goo_sched_block_begin until it returned
    // from pthread_cond_wait and ran goo_sched_block_end. That interval is
    // scheduler latency, which the OS does not bound, so the equality was
    // transiently true for LIVE programs and the threshold was a bet on how
    // long a wake takes: 3 polls first, then 100 after 3 proved too few under 4
    // workers. SPIN proved the bet cannot be won — for every finite threshold
    // there is a live program the detector aborts — and proved the protocol
    // above correct at a threshold of ONE. tests/models/README.md carries both
    // results, the model, and the three rulings this code implements.
    //
    // WHY 3 AND NOT 1. The model says a threshold of 1 is enough, and 1 is what
    // correctness needs. The README recommends a small streak anyway, for a
    // reason outside the model: a mistake in the new accounting then shows up
    // as an abort 1.5 ms late instead of a wrong abort, and 1.5 ms is not a
    // human-visible delay on a program that is already dead. It buys nothing
    // the count does not already give, and it costs 3 polls.
    //
    // THE DECISION AND THE ABORT ARE BOTH UNDER THE MUTEX (ruling 3). The old
    // loop dropped the mutex between reading the three conjuncts and calling
    // goo_deadlock_abort, so the state could move in between. goo_deadlock_abort
    // is one fprintf, one fflush and exit(2); it takes no lock, so it cannot
    // deadlock against the mutex it holds, and the process is ending anyway.
    // The `done || stopped` break stays outside: it makes no claim about a
    // state that must not move.
    const int GOO_DEADLOCK_POLL_STREAK = 3;
    int asleep_streak = 0;

    for (;;) {
        goo_platform_sleep_ns(500000);  // 0.5ms

        goo_mutex_lock(g_scheduler->scheduler_mutex);
        int done = (g_scheduler->stats.num_goroutines == 0 &&
                    g_scheduler->ready_queue == NULL);
        int stopped = !g_scheduler->running;
        // Every live goroutine is parked, none is runnable, and no waker has
        // released any of them.
        int all_asleep = (g_scheduler->stats.num_goroutines > 0) &&
                         (g_scheduler->deadlock_detector.blocked_goroutines ==
                          (int)g_scheduler->stats.num_goroutines) &&
                         (g_scheduler->ready_queue == NULL);
        if (all_asleep) {
            asleep_streak++;
        } else {
            asleep_streak = 0;
        }
        if (asleep_streak >= GOO_DEADLOCK_POLL_STREAK) {
            goo_deadlock_abort();   // noreturn: exit(2) with the mutex held
        }
        goo_mutex_unlock(g_scheduler->scheduler_mutex);

        if (done || stopped) {
            break;
        }
    }
}

// Goroutine creation
goo_goroutine_t* goo_go(goo_goroutine_func_t func, void* arg) {
    if (!g_scheduler) {
        goo_scheduler_init(goo_default_thread_count());  // Use GOMAXPROCS/NCPU worker count
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

goo_goroutine_t* goo_current_goroutine(void) {
    return t_current;
}

// Remove one goroutine from a condvar's waiter list. Both mutexes are held.
// Returns 1 if it was on the list, 0 if it was not — the caller asserts on 0,
// because a registered goroutine is always on the list it registered with.
static int waiter_list_remove(goo_goroutine_t** head, goo_goroutine_t* g) {
    for (goo_goroutine_t** p = head; *p; p = &(*p)->next_waiter) {
        if (*p == g) {
            *p = g->next_waiter;
            g->next_waiter = NULL;
            return 1;
        }
    }
    return 0;
}

// M9: called by a participant immediately before it blocks on a channel
// (cond_wait), while holding that channel's mutex. Registers the block and, for
// the main-thread path, immediately detects deadlocks where no goroutine can
// ever signal main.
//
// 2026-09-02: this is a SET INSERTION, not an increment. `waiters` is the
// waiter list of the condition variable this goroutine is about to wait on;
// goo_sched_wake_waiters walks that same list and removes every member before
// it broadcasts. The membership bit makes the insertion idempotent, which is
// what lets goo_sched_block_end be an unconditional caller.
//
// `waiters` is NULL only for a wait no wake site can ever reach
// (goo_chan_nil_block). Such a goroutine registers and never leaves the set,
// which is the truth about it.
//
// KNOWN LIMITATION: if main blocks on a channel mid-body WHILE one or more
// goroutines are themselves all blocked (num_goroutines > 0), that deadlock is
// NOT detected here (the instant check can't safely decide: another goroutine
// might be runnable and about to signal). The poll loop would detect it, but
// main is not in goo_scheduler_wait yet. Go avoids this because main is itself
// a goroutine; Goo's main is an OS thread, so this gap is structural.
void goo_sched_block_begin(goo_goroutine_t** waiters) {
    if (!g_scheduler) {
        // No scheduler means no goroutines were ever spawned.  If we reach
        // here, main itself is blocking on a channel with nobody to wake it.
        goo_deadlock_abort();
    }
    goo_goroutine_t* self = goo_current_goroutine();

    goo_mutex_lock(g_scheduler->scheduler_mutex);
    if (self) {
        // The wait loop unregisters before it re-registers, on the woken path
        // (the waker cleared the bit) and on the spurious path alike
        // (goo_sched_block_end cleared it), so the bit is always clear here.
        // GOO_NEVER keeps the guard under GOO_ASSERT_OFF: a wrong belief
        // degrades into a no-op rather than a double count.
        if (!GOO_NEVER(self->counted_blocked)) {
            self->counted_blocked = 1;
            if (waiters) {
                self->next_waiter = *waiters;
                *waiters = self;
            }
            g_scheduler->deadlock_detector.blocked_goroutines++;
        }
        GOO_ASSERT(g_scheduler->deadlock_detector.blocked_goroutines > 0);
        goo_mutex_unlock(g_scheduler->scheduler_mutex);
        return;
    }

    // Main-thread path: deadlock iff there is no goroutine that could ever
    // signal us (none exists and none is runnable). main is never a member of
    // the set and never joins a waiter list; the broadcast reaches it anyway.
    int deadlock = (g_scheduler->stats.num_goroutines == 0 &&
                    g_scheduler->ready_queue == NULL);
    goo_mutex_unlock(g_scheduler->scheduler_mutex);

    if (deadlock) {
        goo_deadlock_abort();
    }
}

// 2026-09-02: called immediately BEFORE the pthread_cond_broadcast that wakes
// this condition variable, with the channel mutex held. It removes EVERY
// goroutine on that condvar's waiter list from the blocked set, so the set the
// waker empties is exactly the set the broadcast wakes.
//
// Why every wake is a broadcast. pthread_cond_signal gives the caller no way
// to learn which waiter the OS woke, so a signal site cannot pair itself with
// "clear the bit of the one waiter this wakes": clear X's bit, the OS wakes Y,
// and Y runs while still counted. That is the defect this whole change fixes.
// The three former signal sites in channels.c are broadcasts now. The cost is
// a thundering herd on a condvar with many waiters; the alternative is a
// detector that aborts live programs. See tests/models/README.md, "Ruling 1".
//
// Why before, and not after, the broadcast. Between the removal and the
// broadcast a waiter is parked but out of the set, so the count is LOWER than
// the truth, never higher, and (blocked_goroutines == num_goroutines) cannot
// hold: that waiter is itself live and uncounted. The window can only PREVENT
// an abort. The other order gives back the defect at a smaller scale.
void goo_sched_wake_waiters(goo_goroutine_t** waiters) {
    if (!g_scheduler || !waiters) return;

    goo_mutex_lock(g_scheduler->scheduler_mutex);
    int n = 0;
    for (goo_goroutine_t* w = *waiters; w; ) {
        goo_goroutine_t* next = w->next_waiter;
        // The list holds registered goroutines only. This is the local form of
        // "the list length equals the count of registered goroutines": the
        // global form would need a registry of every live channel, which the
        // runtime does not keep.
        GOO_ASSERT(w->counted_blocked);
        w->counted_blocked = 0;
        w->next_waiter = NULL;
        n++;
        w = next;
    }
    *waiters = NULL;
    GOO_ASSERT(g_scheduler->deadlock_detector.blocked_goroutines >= n);
    g_scheduler->deadlock_detector.blocked_goroutines -= n;
    GOO_ASSERT(g_scheduler->deadlock_detector.blocked_goroutines >= 0);
    goo_mutex_unlock(g_scheduler->scheduler_mutex);
}

// TEST-ONLY hook. GOO_TEST_WAKE_DELAY_NS, when it is set to a positive integer,
// makes every goroutine sleep that many nanoseconds at the START of
// goo_sched_block_end — that is, inside the interval between the wake and the
// unregister. That interval is scheduler latency, so no test can widen it any
// other way, and it is the exact interval the detector used to mis-read.
// tests/concurrency/false_deadlock_test.c holds it open past the poll streak
// and requires the program to survive. Unset (the only state any gate but that
// probe runs in) the hook is one relaxed atomic load per wake.
//
// The cached value is read at most once per thread and never written after the
// first store, so two threads racing here compute the same number from the same
// environment; getenv is only ever read, never written, by this runtime.
static uint64_t goo_test_wake_delay_ns(void) {
    static _Atomic long cached = -1;   // -1 = not read yet
    long v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v < 0) {
        const char* s = getenv("GOO_TEST_WAKE_DELAY_NS");
        long parsed = (s && *s) ? strtol(s, NULL, 10) : 0;
        v = (parsed > 0) ? parsed : 0;
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return (uint64_t)v;
}

// M9: called by a goroutine immediately after it wakes from a channel
// cond_wait, with the channel mutex re-acquired by that cond_wait.
//
// 2026-09-02: an IDEMPOTENT UNREGISTER, not a decrement. On the woken path the
// waker already removed this goroutine, so this is a no-op — and the model
// proves it with a full search: assert(counted[_pid] == 0) holds on every
// reachable path with no spurious wakeup.
//
// It is NOT dead code, and a plain decrement here would drive the count below
// zero (that is the model's DOUBLE_DEC mutant, which fails at N = 2 in 139
// states). It is what stops a SPURIOUS return from pthread_cond_wait leaking a
// set member. POSIX takes a thread off the condvar queue the moment the wait
// returns, so no later broadcast can reach it; if its predicate has since
// stopped holding, it leaves the `while` loop and would carry its bit forever,
// and blocked_goroutines would be one too high for the life of the program.
// The model cannot show that leak — it has no condvar queue — so the argument
// is a C-level one; tests/models/README.md, "The leak is a C-level argument",
// carries it with the line numbers.
void goo_sched_block_end(goo_goroutine_t** waiters) {
    uint64_t delay = goo_test_wake_delay_ns();
    if (delay) {
        goo_platform_sleep_ns(delay);
    }
    if (!g_scheduler) return;
    goo_goroutine_t* self = goo_current_goroutine();
    if (!self) return;                            // main is not counted

    goo_mutex_lock(g_scheduler->scheduler_mutex);
    if (self->counted_blocked) {
        self->counted_blocked = 0;
        if (waiters) {
            int removed = waiter_list_remove(waiters, self);
            GOO_ASSERT(removed);                  // registered => on its list
            (void)removed;
        }
        GOO_ASSERT(g_scheduler->deadlock_detector.blocked_goroutines > 0);
        g_scheduler->deadlock_detector.blocked_goroutines--;
    } else {
        // The waker removed it, and removal unlinks as well as clearing.
        GOO_ASSERT(self->next_waiter == NULL);
    }
    GOO_ASSERT(g_scheduler->deadlock_detector.blocked_goroutines >= 0);
    goo_mutex_unlock(g_scheduler->scheduler_mutex);
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
            // goroutine set (to itself) iff it EXITED; a yielded goroutine set
            // t_requeue instead (handled just below) and leaves t_reap NULL.
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
            // No goroutine ready; idle briefly. (Deadlock detection happens at
            // channel block points and in goo_scheduler_wait, not here.)
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