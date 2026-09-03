#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

// Platform detection
#if defined(__APPLE__) && defined(__MACH__)
    #define GOO_PLATFORM_MACOS 1
    #define GOO_PLATFORM_UNIX 1
#elif defined(__linux__)
    #define GOO_PLATFORM_LINUX 1
    #define GOO_PLATFORM_UNIX 1
#elif defined(_WIN32) || defined(_WIN64)
    #define GOO_PLATFORM_WINDOWS 1
#else
    #define GOO_PLATFORM_UNKNOWN 1
    #define GOO_PLATFORM_UNIX 1  // Assume Unix-like
#endif

// Architecture detection
#if defined(__x86_64__) || defined(_M_X64)
    #define GOO_ARCH_X64 1
    #define GOO_POINTER_SIZE 8
#elif defined(__i386__) || defined(_M_IX86)
    #define GOO_ARCH_X86 1
    #define GOO_POINTER_SIZE 4
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define GOO_ARCH_ARM64 1
    #define GOO_POINTER_SIZE 8
#elif defined(__arm__) || defined(_M_ARM)
    #define GOO_ARCH_ARM 1
    #define GOO_POINTER_SIZE 4
#else
    #define GOO_ARCH_UNKNOWN 1
    #define GOO_POINTER_SIZE 8  // Assume 64-bit
#endif

// Platform-specific includes
#ifdef GOO_PLATFORM_UNIX
    #include <unistd.h>
    #include <sys/mman.h>
    #include <pthread.h>
#endif

#ifdef GOO_PLATFORM_WINDOWS
    #include <windows.h>
#endif

// Platform-specific memory functions
void* goo_platform_alloc(size_t size);
void goo_platform_free(void* ptr, size_t size);

// Thread, clock and sleep declarations (goo_thread_t, goo_thread_func_t,
// goo_thread_create/join/detach, goo_platform_time_ns/wall_time_ns/
// sleep_ns) moved to include/goo_os.h (deterministic-simulation task 1):
// they are the seam now, implemented by src/runtime/goo_os_posix.c. Pulled
// in here so existing includers of platform.h still see these names.
//
// P4.6 (packages-C, C1) on why goo_platform_wall_time_ns exists separately
// from goo_platform_time_ns: wall-clock nanoseconds since the Unix epoch
// (CLOCK_REALTIME) is distinct from goo_platform_time_ns's CLOCK_MONOTONIC
// — correct for measuring elapsed intervals, but a silent lie for anything
// claiming to be "UnixNano": a monotonic clock's epoch is arbitrary, usually
// boot time, not 1970-01-01. time.Now().UnixNano() needs the real epoch, so
// it gets its own primitive rather than reusing the monotonic one.
#include "goo_os.h"

#endif // PLATFORM_H