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
 * them. The runtime decides WHEN to free one; this file decides whether the
 * storage returns to malloc, and for a mutex it does not (see FREED STORAGE
 * IS NEVER REUSED below). */
#include "sim.h"

#include "runtime.h"

#include <stddef.h>
#include <string.h>

/* concurrency.c's scheduler. Read, never written, to name the scheduler
 * mutex for the lock-order check: the program, not sim_main, decides when
 * goo_scheduler_init runs, so the mutex is looked up at each lock. */
extern goo_scheduler_t* g_scheduler;

typedef struct sim_mutex_slot {
    const goo_mutex_t* mutex;        /* nullptr = a free slot */
    uint32_t           owner;        /* a thread id; meaningful only while held */
    uint32_t           generation;   /* changes at each free of this slot */
    uint32_t           next_free;    /* the free list; meaningful only while free */
    uint8_t            held;
} sim_mutex_slot_t;

/* THE HANDLE (Task 7 fix round 1). A goo_mutex_t carries its slot index and
 * the slot's generation, so a lookup is O(1): the table has 16,384 slots,
 * and a search of it at every lock would cost 256 times what the 64-slot
 * search cost. struct goo_mutex is the runtime's type and does not change;
 * the tag lives in the bytes of its pthread_mutex_t, which only
 * goo_os_posix.c uses, and the sim archive never links that file. A handle
 * the seam did not make (a stranger, a copy, freed bytes, stale bytes at a
 * reused address) fails one of the checks in slot_of and reads
 * unknown-mutex.
 *
 * FREED STORAGE IS NEVER REUSED (Task 7 fix round 2). goo_mutex_free clears
 * the tag and keeps the object: it does not return it to malloc. Two reasons.
 * A read of a freed object is undefined in C, and slot_of reads the tag of
 * whatever handle it gets. And with reuse, malloc could put a new mutex at
 * the freed address: the stale pointer would then BE the new live handle,
 * with its valid tag, and a use after free would lock the wrong mutex with
 * no verdict. Kept, the storage holds magic 0 until the process ends, so
 * every freed handle reads unknown-mutex, exactly. The cost is the size of
 * a goo_mutex_t for each free; the runtime frees a mutex only at scheduler
 * shutdown, goo_chan_free and a wait-group free, and no program under test
 * reaches them in a loop. Should one do so, the count of frees is bounded by
 * SIM_QUARANTINE_MAX, and the next free ends the run with the verdict
 * limit-freed-mutexes: memory use then stays bounded, and the run says why
 * it stopped. The generation still decides when the bytes of a
 * freed mutex are written into the new mutex that takes its slot (the free
 * list is last in, first out). */
typedef struct sim_mutex_tag {
    uint64_t magic;                  /* SIM_MUTEX_MAGIC while the seam owns the mutex */
    uint32_t slot;
    uint32_t generation;
} sim_mutex_tag_t;

static_assert(offsetof(goo_mutex_t, locked) >= sizeof(sim_mutex_tag_t),
              "the tag must fit in the bytes before goo_mutex_t.locked");
#define SIM_MUTEX_MAGIC 0x53494d4d55544558ull   /* "SIMMUTEX" */
#define SIM_SLOT_NONE   UINT32_MAX

/* One handle per thread id. Slots are never reused, like sim_sched.c's. */
struct goo_thread {
    goo_thread_func_t func;          /* nullptr = never created through the seam */
    void*             arg;
    uint32_t          id;
    uint8_t           exited;        /* func returned; a join may return */
    uint8_t           detached;
};

static sim_mutex_slot_t  g_mutexes[SIM_MUTEXES_MAX];
/* Slots below g_slots_touched were used at least once; a freed one is on
 * the free list. Taking a slot is O(1): the free list, else the next
 * untouched slot. Zero is "empty", so no loop at start-up walks the table. */
static uint32_t          g_slots_touched;
static uint32_t          g_free_head_plus_one;            /* 0 = the free list is empty */
/* Mutexes freed so far. Their storage is kept (see FREED STORAGE above), so
 * this is the quarantine's size, and SIM_QUARANTINE_MAX bounds it. */
static uint32_t          g_quarantined;
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

static sim_mutex_tag_t tag_read(const goo_mutex_t* mutex) {
    GOO_ASSERT(mutex != nullptr);
    sim_mutex_tag_t tag;
    memcpy(&tag, mutex, sizeof tag);
    return tag;
}

static void tag_write(goo_mutex_t* mutex, sim_mutex_tag_t tag) {
    GOO_ASSERT(mutex != nullptr);
    GOO_ASSERT(tag.slot < SIM_MUTEXES_MAX || tag.magic == 0);
    memcpy(mutex, &tag, sizeof tag);
}

/* The slot a handle names, or the verdict unknown-mutex. Each check is its
 * own line, so the one that fails is the one a debugger shows. */
static sim_mutex_slot_t* slot_of(const goo_mutex_t* mutex) {
    GOO_ASSERT(mutex != nullptr);
    sim_mutex_tag_t tag = tag_read(mutex);
    if (tag.magic != SIM_MUTEX_MAGIC) sim_exit_with(SIM_INVARIANT, "unknown-mutex");
    if (tag.slot >= SIM_MUTEXES_MAX) sim_exit_with(SIM_INVARIANT, "unknown-mutex");
    sim_mutex_slot_t* slot = &g_mutexes[tag.slot];
    if (slot->mutex != mutex) sim_exit_with(SIM_INVARIANT, "unknown-mutex");   /* free, or another mutex */
    if (slot->generation != tag.generation) sim_exit_with(SIM_INVARIANT, "unknown-mutex");
    return slot;
}

/* O(1): the head of the free list, else the next untouched slot. */
static uint32_t slot_take_free(void) {
    if (g_free_head_plus_one != 0) {
        uint32_t index = g_free_head_plus_one - 1;
        GOO_ASSERT(index < g_slots_touched);
        GOO_ASSERT(g_mutexes[index].mutex == nullptr);
        uint32_t next = g_mutexes[index].next_free;
        g_free_head_plus_one = next == SIM_SLOT_NONE ? 0 : next + 1;
        return index;
    }
    if (g_slots_touched == SIM_MUTEXES_MAX) sim_exit_with(SIM_LIMIT, "mutexes");
    GOO_ASSERT(g_mutexes[g_slots_touched].mutex == nullptr);
    return g_slots_touched++;
}

static void slot_register(goo_mutex_t* mutex) {
    GOO_ASSERT(mutex != nullptr);
    uint32_t index = slot_take_free();
    sim_mutex_slot_t* slot = &g_mutexes[index];
    slot->mutex = mutex;
    slot->owner = 0;
    slot->held = 0;
    slot->next_free = SIM_SLOT_NONE;
    tag_write(mutex, (sim_mutex_tag_t){ .magic = SIM_MUTEX_MAGIC, .slot = index,
                                        .generation = slot->generation });
}

/* The slot goes back on the free list with a new generation, so a handle
 * that still names it (stale bytes at a reused address) no longer matches. */
static void slot_unregister(goo_mutex_t* mutex, sim_mutex_slot_t* slot) {
    GOO_ASSERT(slot->mutex == mutex);
    GOO_ASSERT(slot->held == 0);
    GOO_ASSERT(slot->generation < UINT32_MAX);   /* a wrap would revive old handles */
    uint32_t index = (uint32_t)(slot - g_mutexes);
    GOO_ASSERT(index < g_slots_touched);
    slot->mutex = nullptr;
    slot->generation++;
    slot->next_free = g_free_head_plus_one == 0 ? SIM_SLOT_NONE : g_free_head_plus_one - 1;
    g_free_head_plus_one = index + 1;
    tag_write(mutex, (sim_mutex_tag_t){ .magic = 0, .slot = 0, .generation = 0 });
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
    GOO_ASSERT(slot_of(mutex)->mutex == mutex);   /* found by the lookup that lock and unlock use */
    return mutex;
}

void goo_mutex_free(goo_mutex_t* mutex) {
    (void)self_id();
    if (mutex == nullptr) return;              /* production accepts NULL */
    sim_mutex_slot_t* slot = slot_of(mutex);
    if (slot->held != 0) sim_exit_with(SIM_INVARIANT, "free-held-mutex");
    GOO_ASSERT(mutex->locked == 0);            /* the mirror agrees with the table */
    if (g_quarantined == SIM_QUARANTINE_MAX) sim_exit_with(SIM_LIMIT, "freed-mutexes");
    g_quarantined++;
    slot_unregister(mutex, slot);
    GOO_ASSERT(slot->mutex == nullptr);
    GOO_ASSERT(tag_read(mutex).magic == 0);
    /* No goo_free: see FREED STORAGE IS NEVER REUSED above. */
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
