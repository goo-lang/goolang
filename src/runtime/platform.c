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

// Thread support (struct goo_thread, thread_wrapper, goo_thread_create/
// join/detach) and clock/sleep support (goo_platform_time_ns/
// wall_time_ns/sleep_ns) moved to src/runtime/goo_os_posix.c
// (deterministic-simulation task 1): all are now seam calls, declared in
// include/goo_os.h.