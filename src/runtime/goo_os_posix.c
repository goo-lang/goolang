// The POSIX/Windows seam implementation (deterministic-simulation task 1).
//
// Everything below moved here verbatim from sync.c (the five mutex
// functions) and platform.c (struct goo_thread, thread_wrapper, the three
// thread functions, and the three clock/sleep functions). The condvar
// functions and goo_os_tls() are new: they replace every direct
// pthread_cond_* call in channels.c and sync.c. See include/goo_os.h for
// the seam this file implements, and tests/sim/sim_os.c for the simulator's
// implementation of the same header.
// clock_gettime and nanosleep need POSIX visibility. No explicit
// _POSIX_C_SOURCE define here: the Makefile's global -D_GNU_SOURCE already
// implies it, and runtime.h (included first) defines _XOPEN_SOURCE 700
// under a guard for a build without it. platform.c keeps its own guarded
// _POSIX_C_SOURCE define, which no longer warns.
#include "runtime.h"
#include "goo_os.h"
#include "platform.h"
#include <stdlib.h>
#include <time.h>

#ifdef GOO_PLATFORM_UNIX
    #include <pthread.h>
#endif

// ---------------------------------------------------------------------------
// Mutex implementation (moved verbatim from sync.c:14-70)
// ---------------------------------------------------------------------------
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

// A condvar call must succeed. This check replaces a GOO_ASSERT(rc == 0),
// which compiles away under GOO_ASSERT_OFF: that left a pthread_cond_*
// failure unchecked and rc unused (a warning, and the build uses -Werror).
// The kill switch turns off beliefs about the program, not error returns
// from the OS, so it does not reach this check. GOO_COVERAGE keeps
// goo_assert.h's carve-out: a check that never fails in a test run must not
// count as an uncovered branch, so that build only reads rc.
#if defined(GOO_COVERAGE)
#  define GOO_OS_REQUIRE_ZERO(rc) ((void)(rc))
#else
#  define GOO_OS_REQUIRE_ZERO(rc) \
     ((rc) != 0 ? goo_assert_fail("OS CALL", #rc " == 0", __FILE__, __LINE__, __func__) : (void)0)
#endif

// ---------------------------------------------------------------------------
// Condvar implementation (new: replaces pthread_cond_* in channels.c/sync.c)
// ---------------------------------------------------------------------------
goo_cond_t* goo_cond_new(void) {
    goo_cond_t* cond = goo_alloc(sizeof(goo_cond_t));
    GOO_ASSERT(cond != NULL);
#ifdef GOO_PLATFORM_UNIX
    int rc = pthread_cond_init(&cond->cond, NULL);
    GOO_OS_REQUIRE_ZERO(rc);
#endif
    return cond;
}
void goo_cond_free(goo_cond_t* cond) {
    GOO_ASSERT(cond != NULL);
#ifdef GOO_PLATFORM_UNIX
    int rc = pthread_cond_destroy(&cond->cond);
    GOO_OS_REQUIRE_ZERO(rc);
#endif
    goo_free(cond);
}
void goo_cond_wait(goo_cond_t* cond, goo_mutex_t* mutex) {
    GOO_ASSERT(cond != NULL);
    GOO_ASSERT(mutex != NULL);
#ifdef GOO_PLATFORM_UNIX
    int rc = pthread_cond_wait(&cond->cond, &mutex->mutex);
    GOO_OS_REQUIRE_ZERO(rc);
#endif
}
void goo_cond_signal(goo_cond_t* cond) {
    GOO_ASSERT(cond != NULL);
#ifdef GOO_PLATFORM_UNIX
    int rc = pthread_cond_signal(&cond->cond);
    GOO_OS_REQUIRE_ZERO(rc);
#endif
}
void goo_cond_broadcast(goo_cond_t* cond) {
    GOO_ASSERT(cond != NULL);
#ifdef GOO_PLATFORM_UNIX
    int rc = pthread_cond_broadcast(&cond->cond);
    GOO_OS_REQUIRE_ZERO(rc);
#endif
}

// ---------------------------------------------------------------------------
// Thread support (moved verbatim from platform.c)
// ---------------------------------------------------------------------------

// Thread support structure
struct goo_thread {
#ifdef GOO_PLATFORM_UNIX
    pthread_t handle;
#elif defined(GOO_PLATFORM_WINDOWS)
    HANDLE handle;
    DWORD thread_id;
#endif
    goo_thread_func_t func;
    void* arg;
};

// Thread wrapper function
#ifdef GOO_PLATFORM_UNIX
static void* thread_wrapper(void* arg) {
    goo_thread_t* thread = (goo_thread_t*)arg;
    return thread->func(thread->arg);
}
#elif defined(GOO_PLATFORM_WINDOWS)
static DWORD WINAPI thread_wrapper(LPVOID arg) {
    goo_thread_t* thread = (goo_thread_t*)arg;
    thread->func(thread->arg);
    return 0;
}
#endif

// Thread functions
goo_thread_t* goo_thread_create(goo_thread_func_t func, void* arg) {
    goo_thread_t* thread = goo_alloc(sizeof(goo_thread_t));
    thread->func = func;
    thread->arg = arg;

#ifdef GOO_PLATFORM_UNIX
    int result = pthread_create(&thread->handle, NULL, thread_wrapper, thread);
    if (result != 0) {
        goo_free(thread);
        return NULL;
    }
#elif defined(GOO_PLATFORM_WINDOWS)
    thread->handle = CreateThread(NULL, 0, thread_wrapper, thread, 0, &thread->thread_id);
    if (thread->handle == NULL) {
        goo_free(thread);
        return NULL;
    }
#endif

    return thread;
}

void goo_thread_join(goo_thread_t* thread) {
    if (!thread) return;

#ifdef GOO_PLATFORM_UNIX
    pthread_join(thread->handle, NULL);
#elif defined(GOO_PLATFORM_WINDOWS)
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
#endif

    goo_free(thread);
}

void goo_thread_detach(goo_thread_t* thread) {
    if (!thread) return;

#ifdef GOO_PLATFORM_UNIX
    pthread_detach(thread->handle);
#elif defined(GOO_PLATFORM_WINDOWS)
    CloseHandle(thread->handle);
#endif

    goo_free(thread);
}

// ---------------------------------------------------------------------------
// Clock and sleep (moved verbatim from platform.c)
// ---------------------------------------------------------------------------

// Time functions
uint64_t goo_platform_time_ns(void) {
#ifdef GOO_PLATFORM_UNIX
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#elif defined(GOO_PLATFORM_WINDOWS)
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000000000ULL / frequency.QuadPart);
#else
    // Fallback to standard clock
    return (uint64_t)clock() * (1000000000ULL / CLOCKS_PER_SEC);
#endif
}

// P4.6 (packages-C, C1): wall-clock (CLOCK_REALTIME) counterpart to
// goo_platform_time_ns's CLOCK_MONOTONIC — see platform.h's doc comment on
// why UnixNano needs this instead. Mirrors goo_platform_time_ns's per-
// platform structure exactly, just a different clock source.
uint64_t goo_platform_wall_time_ns(void) {
#ifdef GOO_PLATFORM_UNIX
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#elif defined(GOO_PLATFORM_WINDOWS)
    // FILETIME ticks are 100ns intervals since 1601-01-01; convert to
    // nanoseconds since 1970-01-01 (the Unix epoch offset in 100ns ticks is
    // the well-known constant 116444736000000000ULL).
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (ticks - 116444736000000000ULL) * 100ULL;
#else
    // Fallback: no wall-clock source available, so report the epoch. Better
    // than a garbage value; callers on this fallback path already accepted a
    // degraded goo_platform_time_ns above.
    return 0;
#endif
}

void goo_platform_sleep_ns(uint64_t ns) {
#ifdef GOO_PLATFORM_UNIX
    struct timespec ts;
    ts.tv_sec = ns / 1000000000ULL;
    ts.tv_nsec = ns % 1000000000ULL;
    nanosleep(&ts, NULL);
#elif defined(GOO_PLATFORM_WINDOWS)
    // Windows Sleep takes milliseconds
    DWORD ms = (DWORD)(ns / 1000000ULL);
    if (ms == 0 && ns > 0) ms = 1;  // Minimum 1ms
    Sleep(ms);
#else
    // Fallback using standard library
    struct timespec ts;
    ts.tv_sec = ns / 1000000000ULL;
    ts.tv_nsec = ns % 1000000000ULL;
    nanosleep(&ts, NULL);
#endif
}

// ---------------------------------------------------------------------------
// Per-worker TLS accessor (new: replaces the four _Thread_local variables in
// concurrency.c, wired up when task 2 routes concurrency.c through the seam)
// ---------------------------------------------------------------------------
static _Thread_local goo_worker_tls_t t_worker;
goo_worker_tls_t* goo_os_tls(void) {
    return &t_worker;
}
