// P4.7 (packages-B, B3): sync.Mutex / sync.WaitGroup runtime shim.
//
// sync.Mutex and sync.WaitGroup are synthesized by the checker (goo.c's
// sync package seeding) as one-field structs whose sole field is an opaque
// pointer, zero-value NULL — so `var mu sync.Mutex` allocas 8 zeroed bytes
// and is immediately usable, matching Go's zero-value contract. Every sync
// method call compiles to a call into one of the wrappers below
// (call_codegen.c's method-call arm intercepts "sync"-owned receivers),
// passing the ADDRESS of that one field — "the slot" — as `void** slot`.
//
// goo_mutex_t requires a real pthread_mutex_init (a zero-filled
// pthread_mutex_t is not portably valid) and goo_waitgroup_t holds a heap
// goo_mutex_t*, so neither can simply BE the zero-initialized struct. Each
// wrapper here lazily allocates the real primitive into *slot on first use.
//
// Race safety: two goroutines racing the first Lock()/Add()/Wait() on the
// same zero-value object must not both win a naive NULL check and each
// pthread_mutex_init an independently-allocated goo_mutex_t (leaking one,
// and splitting subsequent callers across two distinct mutexes that no
// longer serialize each other — silent loss of the safety this package
// exists to provide). g_sync_init_lock serializes the allocate-and-publish
// step; the fast path (every call after the first, on a given object) is a
// single atomic acquire-load with no lock contention at all.
#include "runtime.h"
#include "platform.h"
#include <stdlib.h>

#ifdef GOO_PLATFORM_UNIX
    #include <pthread.h>
#endif

static pthread_mutex_t g_sync_init_lock = PTHREAD_MUTEX_INITIALIZER;

// Double-checked lazy init. `slot` holds NULL (never used) or a pointer to
// a heap-allocated goo_mutex_t, published exactly once. The fast-path read
// uses acquire semantics so a thread observing a non-NULL pointer also
// observes every store goo_mutex_new made to the pointee (its pthread_mutex_
// init) — guaranteed by the release store on the publishing side. The lock
// only ever guards the allocate step; it is never held during
// Lock/Unlock/Add/Done/Wait themselves, so steady-state contention is
// whatever the underlying pthread primitive itself provides, unchanged.
static goo_mutex_t* sync_mutex_ensure(void** slot) {
    void* p = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (p) return (goo_mutex_t*)p;

    pthread_mutex_lock(&g_sync_init_lock);
    p = __atomic_load_n(slot, __ATOMIC_ACQUIRE);  // re-check: lost the race?
    if (!p) {
        p = goo_mutex_new();
        __atomic_store_n(slot, p, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_sync_init_lock);
    return (goo_mutex_t*)p;
}

static goo_waitgroup_t* sync_wg_ensure(void** slot) {
    void* p = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (p) return (goo_waitgroup_t*)p;

    pthread_mutex_lock(&g_sync_init_lock);
    p = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (!p) {
        p = goo_waitgroup_new();
        __atomic_store_n(slot, p, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_sync_init_lock);
    return (goo_waitgroup_t*)p;
}

void goo_sync_mutex_lock(void** slot) {
    if (!slot) return;
    goo_mutex_lock(sync_mutex_ensure(slot));
}

// Go parity: Unlock() of a Mutex that isn't currently locked panics
// ("sync: unlock of unlocked mutex") rather than silently succeeding.
// goo_mutex_unlock itself (sync.c) has no such check and must not gain one
// — it is shared with the scheduler/channel internals, which maintain
// their own lock/unlock discipline unrelated to Go's sync.Mutex contract —
// so the check lives here instead, reading the mutex's own `locked` flag
// (set 1 by goo_mutex_lock, 0 by goo_mutex_unlock; see `struct goo_mutex`,
// runtime.h). This also covers Unlock() on a never-locked but now lazily-
// created zero-value Mutex for free: goo_mutex_new leaves `locked` 0, so
// the very first call here panics exactly like Go's zero-value Mutex does.
//
// The `locked` read is not itself synchronized. A genuine race — Unlock
// called concurrently with another Lock/Unlock on the same Mutex — is
// undefined behavior in Go too (sync.Mutex is not safe for that), so this
// is a best-effort diagnostic for the common sequential double-unlock bug,
// not a race detector.
void goo_sync_mutex_unlock(void** slot) {
    if (!slot) return;
    goo_mutex_t* m = sync_mutex_ensure(slot);
    if (!m->locked) {
        goo_panic("sync: unlock of unlocked mutex");
        return;  // unreachable: goo_panic is noreturn (exits/aborts)
    }
    goo_mutex_unlock(m);
}

void goo_sync_wg_add(void** slot, int64_t delta) {
    if (!slot) return;
    // goo_waitgroup_add takes a signed `int`; Goo's WaitGroup.Add(delta int)
    // is Goo's 64-bit `int`. Truncating narrows only if the caller passes a
    // delta outside `int` range, which is already a misuse no real program
    // hits (a WaitGroup counter never approaches 2^31).
    goo_waitgroup_add(sync_wg_ensure(slot), (int)delta);
}

void goo_sync_wg_done(void** slot) {
    if (!slot) return;
    goo_waitgroup_done(sync_wg_ensure(slot));
}

void goo_sync_wg_wait(void** slot) {
    if (!slot) return;
    // No prior Add(): lazy-init finds counter == 0 (goo_waitgroup_new's own
    // zero-init), so goo_waitgroup_wait's `while (counter > 0)` never
    // executes and this returns immediately — Go-correct.
    goo_waitgroup_wait(sync_wg_ensure(slot));
}
