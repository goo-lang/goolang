/* tests/sim/sim_os.c — the seam (include/goo_os.h) over the simulator's
 * scheduler (spec sections 5-7). It replaces src/runtime/goo_os_posix.c in
 * the sim archive; the two never link together.
 *
 * Every blocking or ordering call ends in sim_yield_point(), the one place
 * where the seed picks the next coroutine. Four calls deliberately do NOT
 * yield: the clock reads and goo_os_tls(), which order nothing, and the
 * constructors goo_mutex_new/goo_cond_new. The constructors must not:
 * src/runtime/sync_shim.c calls goo_mutex_new while it holds a REAL pthread
 * mutex, and a switch there would let a second coroutine on the same real
 * thread take that mutex again, which deadlocks the process for real.
 *
 * Every seam call first asserts, through sim_current(), that it runs on a
 * coroutine: on the real thread there is no simulated thread to own a
 * mutex, sleep or wait.
 *
 * The checks of spec section 7 end the run with the verdict `invariant`:
 * a recursive lock, an unlock that is not held or not the caller's, a wait
 * without the mutex, the scheduler mutex held while another mutex is taken
 * (lock order: a channel mutex comes BEFORE the scheduler mutex), a thread
 * that exits holding a mutex, and a free of a held mutex or of a condvar
 * with waiters.
 *
 * Tiger Style (spec section 7): the tables are static and sized by sim.h;
 * the mutex and condvar OBJECTS come from goo_alloc, as production makes
 * them, because the runtime owns their lifetime. */
#include "sim.h"

#include "runtime.h"

#include <string.h>

/* concurrency.c's scheduler. Read, never written, to name the scheduler
 * mutex for the lock-order check: the program, not sim_main, decides when
 * goo_scheduler_init runs, so the mutex is looked up at each lock. */
extern goo_scheduler_t* g_scheduler;

typedef struct sim_mutex_slot {
    const goo_mutex_t* mutex;        /* nullptr = a free slot */
    uint32_t           owner;        /* a thread id; meaningful only while held */
    uint8_t            held;
} sim_mutex_slot_t;

/* One handle per thread id. Slots are never reused, like sim_sched.c's. */
struct goo_thread {
    goo_thread_func_t func;          /* nullptr = never created through the seam */
    void*             arg;
    uint32_t          id;
    uint8_t           exited;        /* func returned; a join may return */
    uint8_t           detached;
};

static sim_mutex_slot_t  g_mutexes[SIM_MUTEXES_MAX];
static struct goo_thread g_handles[SIM_THREADS_MAX];
static uint32_t          g_held_count[SIM_THREADS_MAX];   /* mutexes each thread holds now */
static bool              g_fault_spurious_wake;
static bool              g_fault_thread_create_eagain;
static bool              g_faults_set;

/* Once per process, before the first seam call: a fault that changed in
 * the middle of a run would make the seed no longer name the run. */
void sim_os_set_faults(bool spurious_wake, bool thread_create_eagain) {
    GOO_ASSERT(!g_faults_set);
    GOO_ASSERT(g_handles[0].func == nullptr);  /* no thread made through the seam yet */
    g_fault_spurious_wake = spurious_wake;
    g_fault_thread_create_eagain = thread_create_eagain;
    g_faults_set = true;
}

/* sim_current() asserts that the caller is a coroutine, not the real thread. */
static uint32_t self_id(void) {
    const sim_thread_t* self = sim_current();
    GOO_ASSERT(self->id < SIM_THREADS_MAX);
    GOO_ASSERT(self->state == SIM_RUNNABLE);   /* a parked thread makes no call */
    return self->id;
}

/* ---- the mutex table ---- */

static sim_mutex_slot_t* slot_find(const goo_mutex_t* mutex) {
    GOO_ASSERT(mutex != nullptr);
    for (uint32_t i = 0; i < SIM_MUTEXES_MAX; i++) {
        if (g_mutexes[i].mutex == mutex) return &g_mutexes[i];
    }
    return nullptr;
}

static sim_mutex_slot_t* slot_of(const goo_mutex_t* mutex) {
    sim_mutex_slot_t* slot = slot_find(mutex);
    if (slot == nullptr) sim_exit_with(SIM_INVARIANT, "unknown-mutex");
    GOO_ASSERT(slot->mutex == mutex);
    return slot;
}

static void slot_register(const goo_mutex_t* mutex) {
    GOO_ASSERT(mutex != nullptr);
    GOO_ASSERT(slot_find(mutex) == nullptr);   /* goo_alloc never returns a live mutex twice */
    for (uint32_t i = 0; i < SIM_MUTEXES_MAX; i++) {
        if (g_mutexes[i].mutex == nullptr) {
            g_mutexes[i] = (sim_mutex_slot_t){ .mutex = mutex, .owner = 0, .held = 0 };
            return;
        }
    }
    sim_exit_with(SIM_LIMIT, "mutexes");
}

static const goo_mutex_t* scheduler_mutex(void) {
    if (g_scheduler == nullptr) return nullptr;
    return g_scheduler->scheduler_mutex;   /* nullptr while goo_scheduler_init runs */
}

/* Lock order: a thread that holds the scheduler mutex takes no other. */
static void lock_order_check(const goo_mutex_t* mutex, uint32_t id) {
    GOO_ASSERT(mutex != nullptr);
    GOO_ASSERT(id < SIM_THREADS_MAX);
    const goo_mutex_t* sched = scheduler_mutex();
    if (sched == nullptr) return;
    if (sched == mutex) return;
    const sim_mutex_slot_t* slot = slot_of(sched);
    if (slot->held == 0) return;
    if (slot->owner != id) return;
    sim_exit_with(SIM_INVARIANT, "lock-order");
}

static void slot_take(sim_mutex_slot_t* slot, goo_mutex_t* mutex, uint32_t id) {
    GOO_ASSERT(slot->held == 0);
    GOO_ASSERT(g_held_count[id] < SIM_MUTEXES_MAX);
    slot->held = 1;
    slot->owner = id;
    mutex->locked = 1;                        /* sync_shim.c reads it, as in production */
    g_held_count[id]++;
}

/* The owner and held checks of unlock and wait. */
static void slot_check_owner(const sim_mutex_slot_t* slot, uint32_t id, const char* not_held) {
    GOO_ASSERT(not_held != nullptr);
    if (slot->held == 0) sim_exit_with(SIM_INVARIANT, not_held);
    if (slot->owner != id) sim_exit_with(SIM_INVARIANT, "unlock-not-owner");
    GOO_ASSERT(g_held_count[id] > 0);
}

/* Releases with no yield point of its own; the caller yields. */
static void slot_release(sim_mutex_slot_t* slot, goo_mutex_t* mutex, uint32_t id) {
    GOO_ASSERT(slot->held == 1);
    GOO_ASSERT(slot->owner == id);
    slot->held = 0;
    mutex->locked = 0;
    g_held_count[id]--;
    (void)sim_wake_one(mutex, SIM_WAIT_MUTEX);
}

/* ---- mutexes ---- */

goo_mutex_t* goo_mutex_new(void) {
    (void)self_id();
    goo_mutex_t* mutex = goo_alloc(sizeof(goo_mutex_t));
    GOO_ASSERT(mutex != nullptr);
    memset(mutex, 0, sizeof *mutex);
    slot_register(mutex);
    GOO_ASSERT(slot_find(mutex) != nullptr);  /* found by the search that lock and unlock use */
    return mutex;
}

void goo_mutex_free(goo_mutex_t* mutex) {
    (void)self_id();
    if (mutex == nullptr) return;              /* production accepts NULL */
    sim_mutex_slot_t* slot = slot_of(mutex);
    if (slot->held != 0) sim_exit_with(SIM_INVARIANT, "free-held-mutex");
    GOO_ASSERT(mutex->locked == 0);            /* the mirror agrees with the table */
    slot->mutex = nullptr;
    GOO_ASSERT(slot_find(mutex) == nullptr);
    goo_free(mutex);
}

void goo_mutex_lock(goo_mutex_t* mutex) {
    uint32_t id = self_id();
    if (mutex == nullptr) {                    /* production returns at once */
        sim_yield_point("lock");
        return;
    }
    sim_mutex_slot_t* slot = slot_of(mutex);
    if (slot->held != 0 && slot->owner == id) sim_exit_with(SIM_INVARIANT, "recursive-lock");
    lock_order_check(mutex, id);
    /* Each pass parks once, so GOO_SIM_STEP_LIMIT ends a starved waiter
     * before this bound; the bound only makes the loop finite on its face. */
    for (uint32_t parks = 0; slot->held != 0; parks++) {
        GOO_ASSERT(parks < UINT32_MAX);
        sim_block(SIM_WAIT_MUTEX, mutex);
    }
    slot_take(slot, mutex, id);
    sim_yield_point("lock");
    GOO_ASSERT(slot->owner == id);
    GOO_ASSERT(mutex->locked == 1);
}

void goo_mutex_unlock(goo_mutex_t* mutex) {
    uint32_t id = self_id();
    if (mutex == nullptr) {
        sim_yield_point("unlock");
        return;
    }
    sim_mutex_slot_t* slot = slot_of(mutex);
    slot_check_owner(slot, id, "unlock-not-held");
    GOO_ASSERT(mutex->locked == 1);            /* the mirror agrees with the table */
    slot_release(slot, mutex, id);
    GOO_ASSERT(g_held_count[id] < SIM_MUTEXES_MAX);   /* no wrap below zero */
    sim_yield_point("unlock");
}

/* Never waits. A held mutex, the caller's own included, is a failure, as
 * pthread_mutex_trylock reports EBUSY on a normal mutex. */
int goo_mutex_try_lock(goo_mutex_t* mutex) {
    uint32_t id = self_id();
    if (mutex == nullptr) {
        sim_yield_point("try_lock");
        return 0;
    }
    sim_mutex_slot_t* slot = slot_of(mutex);
    lock_order_check(mutex, id);
    int taken = 0;
    if (slot->held == 0) {
        slot_take(slot, mutex, id);
        taken = 1;
    }
    sim_yield_point("try_lock");
    if (taken == 1) GOO_ASSERT(slot->owner == id);
    return taken;
}

/* ---- condvars ---- */

goo_cond_t* goo_cond_new(void) {
    (void)self_id();
    goo_cond_t* cond = goo_alloc(sizeof(goo_cond_t));
    GOO_ASSERT(cond != nullptr);
    memset(cond, 0, sizeof *cond);
    return cond;
}

void goo_cond_free(goo_cond_t* cond) {
    GOO_ASSERT(cond != nullptr);
    (void)self_id();
    if (sim_wake_all(cond, SIM_WAIT_COND) != 0) sim_exit_with(SIM_INVARIANT, "free-cond-with-waiters");
    goo_free(cond);
}

/* Release and park with no switch between them, so no signal is lost;
 * then reacquire through goo_mutex_lock, with its yield point. With the
 * spurious-wake fault on, the seed returns one wait in eight without a
 * signal, which POSIX permits. */
void goo_cond_wait(goo_cond_t* cond, goo_mutex_t* mutex) {
    GOO_ASSERT(cond != nullptr);
    GOO_ASSERT(mutex != nullptr);
    uint32_t id = self_id();
    sim_mutex_slot_t* slot = slot_of(mutex);
    if (slot->held == 0) sim_exit_with(SIM_INVARIANT, "wait-without-mutex");
    if (slot->owner != id) sim_exit_with(SIM_INVARIANT, "wait-without-mutex");
    slot_release(slot, mutex, id);
    if (g_fault_spurious_wake && sim_random_below(8) == 0) {
        sim_yield_point("spurious");
    } else {
        sim_block(SIM_WAIT_COND, cond);
    }
    goo_mutex_lock(mutex);
    GOO_ASSERT(slot->owner == id);
}

void goo_cond_signal(goo_cond_t* cond) {
    GOO_ASSERT(cond != nullptr);
    (void)self_id();
    uint32_t woken = sim_wake_one(cond, SIM_WAIT_COND);
    GOO_ASSERT(woken <= 1);
    sim_yield_point("signal");
}

void goo_cond_broadcast(goo_cond_t* cond) {
    GOO_ASSERT(cond != nullptr);
    (void)self_id();
    uint32_t woken = sim_wake_all(cond, SIM_WAIT_COND);
    GOO_ASSERT(woken < SIM_THREADS_MAX);      /* the caller is not a waiter */
    sim_yield_point("broadcast");
}

/* ---- threads ---- */

/* Every thread made through the seam starts here, on its own coroutine. */
static void* thread_entry(void* arg) {
    GOO_ASSERT(arg == nullptr);
    uint32_t id = self_id();
    struct goo_thread* handle = &g_handles[id];
    GOO_ASSERT(handle->func != nullptr);
    void* result = handle->func(handle->arg);
    GOO_ASSERT(self_id() == id);               /* the coroutine ends as the thread it began as */
    if (g_held_count[id] != 0) sim_exit_with(SIM_INVARIANT, "exit-holding-mutex");
    handle->exited = 1;
    (void)sim_wake_all(handle, SIM_WAIT_JOIN);
    return result;
}

/* With the EAGAIN fault on, the seed fails one create in sixteen, as
 * pthread_create may; the caller must handle the NULL. */
goo_thread_t* goo_thread_create(goo_thread_func_t func, void* arg) {
    GOO_ASSERT(func != nullptr);
    (void)self_id();
    if (g_fault_thread_create_eagain && sim_random_below(16) == 0) {
        sim_yield_point("create_fail");
        return nullptr;
    }
    uint32_t id = sim_thread_spawn(thread_entry, nullptr);
    GOO_ASSERT(id != 0);                       /* coroutine 0 is the program's main (sim_main.c) */
    struct goo_thread* handle = &g_handles[id];
    GOO_ASSERT(handle->func == nullptr);      /* a thread id is never reused */
    *handle = (struct goo_thread){ .func = func, .arg = arg, .id = id, .exited = 0, .detached = 0 };
    sim_yield_point("create");
    return handle;
}

void goo_thread_join(goo_thread_t* thread) {
    uint32_t id = self_id();
    if (thread == nullptr) {                   /* production returns at once */
        sim_yield_point("join");
        return;
    }
    GOO_ASSERT(thread == &g_handles[thread->id]);
    GOO_ASSERT(thread->func != nullptr);       /* a handle goo_thread_create returned */
    if (thread->id == id) sim_exit_with(SIM_INVARIANT, "join-self");
    if (thread->detached != 0) sim_exit_with(SIM_INVARIANT, "join-detached");
    /* Each pass parks once; the thread's exit wakes every joiner. */
    for (uint32_t parks = 0; thread->exited == 0; parks++) {
        GOO_ASSERT(parks < UINT32_MAX);
        sim_block(SIM_WAIT_JOIN, thread);
    }
    sim_yield_point("join");
}

void goo_thread_detach(goo_thread_t* thread) {
    (void)self_id();
    if (thread == nullptr) {
        sim_yield_point("detach");
        return;
    }
    GOO_ASSERT(thread == &g_handles[thread->id]);
    GOO_ASSERT(thread->func != nullptr);
    thread->detached = 1;
    sim_yield_point("detach");
}

/* ---- the clock ---- */

uint64_t goo_platform_time_ns(void) {
    (void)self_id();
    return sim_now_ns();
}

uint64_t goo_platform_wall_time_ns(void) {
    (void)self_id();
    return sim_now_ns();
}

/* A deadline past UINT64_MAX saturates; the time limit fires first. */
void goo_platform_sleep_ns(uint64_t ns) {
    (void)self_id();
    uint64_t now_ns = sim_now_ns();
    uint64_t wake_at_ns = (ns > UINT64_MAX - now_ns) ? UINT64_MAX : now_ns + ns;
    sim_sleep_until(wake_at_ns);
    GOO_ASSERT(sim_now_ns() >= wake_at_ns);   /* the virtual clock never wakes a sleeper early */
}

/* ---- per-worker state ---- */

goo_worker_tls_t* goo_os_tls(void) {
    sim_thread_t* self = sim_current();
    GOO_ASSERT(self->id < SIM_THREADS_MAX);
    return &self->tls;
}
