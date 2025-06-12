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
    goroutine->context.uc_link = &g_scheduler->main_context;
    
    makecontext(&goroutine->context, goroutine_wrapper, 0);
#pragma clang diagnostic pop
#endif
    
    // Add to scheduler
    scheduler_add_goroutine(goroutine);
    
    return goroutine;
}

void goo_yield(void) {
    if (!g_scheduler || !g_scheduler->current_goroutine) {
        return;
    }
    
    goo_goroutine_t* current = g_scheduler->current_goroutine;
    current->state = GOO_GOROUTINE_READY;
    
    // Add back to ready queue
    scheduler_add_goroutine(current);
    
    // Switch to scheduler
#ifdef GOO_PLATFORM_UNIX
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (swapcontext(&current->context, &g_scheduler->main_context) == -1) {
        goo_panic("Failed to yield goroutine");
    }
#pragma clang diagnostic pop
#endif
}

void goo_goroutine_exit(void) {
    if (!g_scheduler || !g_scheduler->current_goroutine) {
        return;
    }
    
    goo_goroutine_t* current = g_scheduler->current_goroutine;
    current->state = GOO_GOROUTINE_DONE;
    
    goo_mutex_lock(g_scheduler->scheduler_mutex);
    g_scheduler->stats.num_goroutines--;
    goo_mutex_unlock(g_scheduler->scheduler_mutex);
    
    // Free goroutine resources
    goo_free(current->stack);
    goo_free(current);
    
    g_scheduler->current_goroutine = NULL;
    
    // Return to scheduler
#ifdef GOO_PLATFORM_UNIX
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    setcontext(&g_scheduler->main_context);
#pragma clang diagnostic pop
#endif
}

// Goroutine wrapper function
static void goroutine_wrapper(void) {
    goo_goroutine_t* current = g_scheduler->current_goroutine;
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
            g_scheduler->current_goroutine = goroutine;
            goroutine->state = GOO_GOROUTINE_RUNNING;
            
            goo_mutex_lock(g_scheduler->scheduler_mutex);
            g_scheduler->stats.context_switches++;
            goo_mutex_unlock(g_scheduler->scheduler_mutex);
            
#ifdef GOO_PLATFORM_UNIX
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            if (swapcontext(&g_scheduler->main_context, &goroutine->context) == -1) {
                goo_panic("Failed to switch to goroutine");
            }
#pragma clang diagnostic pop
#endif
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