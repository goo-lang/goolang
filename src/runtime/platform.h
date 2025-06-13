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

// Platform-specific thread functions (for future goroutine support)
typedef struct goo_thread goo_thread_t;
typedef void* (*goo_thread_func_t)(void* arg);

goo_thread_t* goo_thread_create(goo_thread_func_t func, void* arg);
void goo_thread_join(goo_thread_t* thread);
void goo_thread_detach(goo_thread_t* thread);

// Platform-specific time functions
uint64_t goo_platform_time_ns(void);
void goo_platform_sleep_ns(uint64_t ns);

#endif // PLATFORM_H