#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "runtime.h"
#include <stdlib.h>
#include <time.h>

#ifdef GOO_PLATFORM_UNIX
    #include <sys/time.h>
    #include <errno.h>
    #include <unistd.h>
    #include <sys/mman.h>
#endif

// Platform-specific memory allocation
void* goo_platform_alloc(size_t size) {
#ifdef GOO_PLATFORM_UNIX
    // Use mmap for large allocations for better control
    if (size >= 1024 * 1024) {  // 1MB threshold
#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#else
#define MAP_ANONYMOUS 0x20
#endif
#endif
        void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) {
            return NULL;
        }
        return ptr;
    }
#endif
    
    // Use standard malloc for smaller allocations
    return malloc(size);
}

void goo_platform_free(void* ptr, size_t size) {
    if (!ptr) return;
    
#ifdef GOO_PLATFORM_UNIX
    // If it was allocated with mmap, use munmap
    if (size >= 1024 * 1024) {
        munmap(ptr, size);
        return;
    }
#endif
    
    // Use standard free
    free(ptr);
}

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