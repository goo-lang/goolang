#ifndef GOO_ASYNC_RESOURCE_H
#define GOO_ASYNC_RESOURCE_H

#include "transparent_async.h"
#include "errors/error.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>

// Forward declarations
typedef struct AsyncResource AsyncResource;
typedef struct AsyncResourceManager AsyncResourceManager;
typedef struct AsyncResourceScope AsyncResourceScope;
typedef struct DeferredAction DeferredAction;

// Resource states for lifecycle management
typedef enum {
    RESOURCE_STATE_CREATED,     // Resource has been created but not acquired
    RESOURCE_STATE_ACQUIRING,   // Resource is being acquired asynchronously
    RESOURCE_STATE_ACQUIRED,    // Resource is acquired and ready for use
    RESOURCE_STATE_RELEASING,   // Resource is being released
    RESOURCE_STATE_RELEASED,    // Resource has been released
    RESOURCE_STATE_ERROR,       // Resource is in an error state
    RESOURCE_STATE_CANCELLED    // Resource acquisition was cancelled
} AsyncResourceState;

// Resource types for proper handling
typedef enum {
    RESOURCE_TYPE_MEMORY,       // Memory allocations
    RESOURCE_TYPE_FILE,         // File handles
    RESOURCE_TYPE_NETWORK,      // Network connections
    RESOURCE_TYPE_LOCK,         // Mutexes, semaphores, etc.
    RESOURCE_TYPE_THREAD,       // Thread handles
    RESOURCE_TYPE_TIMER,        // Timer handles
    RESOURCE_TYPE_CUSTOM        // User-defined resources
} AsyncResourceType;

// Resource priority for cleanup ordering
typedef enum {
    RESOURCE_PRIORITY_LOW = 0,
    RESOURCE_PRIORITY_NORMAL = 1,
    RESOURCE_PRIORITY_HIGH = 2,
    RESOURCE_PRIORITY_CRITICAL = 3
} AsyncResourcePriority;

// Resource acquisition function signature
typedef Result_void_ptr (*AsyncResourceAcquireFn)(void* context, AsyncWaker* waker);

// Resource cleanup function signature
typedef void (*AsyncResourceCleanupFn)(void* resource_data, void* context);

// Resource status checker function signature
typedef bool (*AsyncResourceStatusFn)(void* resource_data, void* context);

// Core async resource structure
typedef struct AsyncResource {
    uint64_t id;
    char name[64];
    AsyncResourceType type;
    AsyncResourceState state;
    AsyncResourcePriority priority;
    
    // Resource data and context
    void* resource_data;
    void* context;
    size_t context_size;
    
    // Function pointers for resource lifecycle
    AsyncResourceAcquireFn acquire_fn;
    AsyncResourceCleanupFn cleanup_fn;
    AsyncResourceStatusFn status_fn;
    
    // Async execution context
    void* async_context; // AsyncContext* async_context;
    CancellationToken* cancel_token;
    AsyncWaker* waker;
    
    // Timing and lifecycle tracking
    uint64_t created_time_ns;
    uint64_t acquired_time_ns;
    uint64_t released_time_ns;
    uint64_t acquisition_timeout_ms;
    
    // Reference counting for shared resources
    atomic_int ref_count;
    
    // Dependency tracking
    AsyncResource** dependencies;
    size_t dependency_count;
    size_t dependency_capacity;
    
    // Error handling
    Error* last_error;
    
    // Resource hierarchy
    AsyncResourceScope* scope;
    struct AsyncResource* parent;
    struct AsyncResource* first_child;
    struct AsyncResource* next_sibling;
    
    // Synchronization
    pthread_mutex_t resource_mutex;
    pthread_cond_t state_changed;
    
    // Statistics
    uint64_t acquire_attempts;
    uint64_t successful_acquisitions;
    uint64_t cleanup_count;
} AsyncResource;

// Deferred action for async-safe cleanup
typedef struct DeferredAction {
    uint64_t id;
    char name[32];
    
    // Action function and data
    void (*action_fn)(void* data, void* context);
    void* data;
    void* context;
    size_t data_size;
    
    // Execution conditions
    bool execute_on_success;
    bool execute_on_error;
    bool execute_on_cancel;
    
    // Priority for execution order
    AsyncResourcePriority priority;
    
    // Associated resource (optional)
    AsyncResource* associated_resource;
    
    // Timing
    uint64_t scheduled_time_ns;
    uint64_t executed_time_ns;
    
    // State
    bool is_executed;
    Error* execution_error;
    
    struct DeferredAction* next;
} DeferredAction;

// Resource scope for managing multiple resources
typedef struct AsyncResourceScope {
    uint64_t id;
    char name[64];
    
    // Resources in this scope
    AsyncResource** resources;
    size_t resource_count;
    size_t resource_capacity;
    
    // Deferred actions in this scope
    DeferredAction* first_deferred;
    DeferredAction* last_deferred;
    size_t deferred_count;
    
    // Scope hierarchy
    struct AsyncResourceScope* parent;
    struct AsyncResourceScope* first_child;
    struct AsyncResourceScope* next_sibling;
    
    // Async context
    void* async_context; // AsyncContext* async_context;
    CancellationToken* cancel_token;
    
    // Scope state
    bool is_active;
    bool cleanup_in_progress;
    bool auto_cleanup_on_exit;
    
    // Cleanup ordering
    bool cleanup_reverse_order; // Clean up in reverse acquisition order
    
    // Error handling
    Error* scope_error;
    bool stop_on_error;
    
    // Timing
    uint64_t created_time_ns;
    uint64_t destroyed_time_ns;
    
    // Synchronization
    pthread_mutex_t scope_mutex;
    pthread_cond_t cleanup_complete;
    
    // Statistics
    uint64_t total_resources_managed;
    uint64_t successful_cleanups;
    uint64_t failed_cleanups;
} AsyncResourceScope;

// Global resource manager
typedef struct AsyncResourceManager {
    // Global resource registry
    AsyncResource** all_resources;
    size_t resource_count;
    size_t resource_capacity;
    
    // Active scopes
    AsyncResourceScope** active_scopes;
    size_t scope_count;
    size_t scope_capacity;
    
    // Configuration
    size_t default_timeout_ms;
    bool enable_resource_tracking;
    bool enable_leak_detection;
    bool auto_cleanup_on_shutdown;
    
    // Global synchronization
    pthread_mutex_t manager_mutex;
    
    // Background cleanup thread
    pthread_t cleanup_thread;
    bool cleanup_thread_running;
    pthread_cond_t cleanup_requested;
    
    // Statistics
    uint64_t total_resources_created;
    uint64_t total_resources_leaked;
    uint64_t total_cleanup_failures;
    
    // Shutdown coordination
    bool is_shutting_down;
    pthread_cond_t shutdown_complete;
} AsyncResourceManager;

// Core resource management functions
AsyncResource* async_resource_create(const char* name, AsyncResourceType type, 
                                   AsyncResourceAcquireFn acquire_fn, AsyncResourceCleanupFn cleanup_fn,
                                   void* context, size_t context_size);

void async_resource_destroy(AsyncResource* resource);

Result_void_ptr async_resource_acquire(AsyncResource* resource, uint64_t timeout_ms);
Result_void_ptr async_resource_release(AsyncResource* resource);
Result_void_ptr async_resource_cancel(AsyncResource* resource);

// Resource state management
AsyncResourceState async_resource_get_state(AsyncResource* resource);
bool async_resource_is_acquired(AsyncResource* resource);
bool async_resource_is_healthy(AsyncResource* resource);

// Resource references and sharing
AsyncResource* async_resource_ref(AsyncResource* resource);
void async_resource_unref(AsyncResource* resource);

// Resource dependencies
Result_void_ptr async_resource_add_dependency(AsyncResource* resource, AsyncResource* dependency);
Result_void_ptr async_resource_remove_dependency(AsyncResource* resource, AsyncResource* dependency);
Result_void_ptr async_resource_wait_for_dependencies(AsyncResource* resource, uint64_t timeout_ms);

// Resource scope management
AsyncResourceScope* async_resource_scope_create(const char* name, void* async_context);
void async_resource_scope_destroy(AsyncResourceScope* scope);

Result_void_ptr async_resource_scope_add(AsyncResourceScope* scope, AsyncResource* resource);
Result_void_ptr async_resource_scope_remove(AsyncResourceScope* scope, AsyncResource* resource);
Result_void_ptr async_resource_scope_cleanup(AsyncResourceScope* scope);

// Deferred actions
DeferredAction* deferred_action_create(const char* name, void (*action_fn)(void*, void*),
                                     void* data, void* context, size_t data_size);
void deferred_action_destroy(DeferredAction* action);

Result_void_ptr async_resource_scope_defer(AsyncResourceScope* scope, DeferredAction* action);
Result_void_ptr async_resource_defer(AsyncResource* resource, DeferredAction* action);

// Execute deferred actions based on condition
Result_void_ptr execute_deferred_actions(AsyncResourceScope* scope, bool on_success, 
                                       bool on_error, bool on_cancel);

// Convenience macros for resource management

// Async resource annotation
#define ASYNC_RESOURCE __attribute__((annotate("async_resource")))

// Defer action in current scope
#define DEFER(scope, action_fn, data) \
    do { \
        DeferredAction* __defer_action = deferred_action_create(#action_fn, \
            (void(*)(void*, void*))action_fn, data, NULL, 0); \
        if (__defer_action) { \
            __defer_action->execute_on_success = true; \
            __defer_action->execute_on_error = true; \
            __defer_action->execute_on_cancel = true; \
            async_resource_scope_defer(scope, __defer_action); \
        } \
    } while(0)

// Defer cleanup on success only
#define DEFER_ON_SUCCESS(scope, action_fn, data) \
    do { \
        DeferredAction* __defer_action = deferred_action_create(#action_fn, \
            (void(*)(void*, void*))action_fn, data, NULL, 0); \
        if (__defer_action) { \
            __defer_action->execute_on_success = true; \
            async_resource_scope_defer(scope, __defer_action); \
        } \
    } while(0)

// Defer cleanup on error/cancel only
#define DEFER_ON_ERROR(scope, action_fn, data) \
    do { \
        DeferredAction* __defer_action = deferred_action_create(#action_fn, \
            (void(*)(void*, void*))action_fn, data, NULL, 0); \
        if (__defer_action) { \
            __defer_action->execute_on_error = true; \
            __defer_action->execute_on_cancel = true; \
            async_resource_scope_defer(scope, __defer_action); \
        } \
    } while(0)

// Scoped resource management
#define WITH_ASYNC_RESOURCE_SCOPE(scope_name, context) \
    AsyncResourceScope* scope_name = async_resource_scope_create(#scope_name, context); \
    for (bool __scope_active = true; __scope_active; \
         __scope_active = false, async_resource_scope_cleanup(scope_name), \
         async_resource_scope_destroy(scope_name))

// Auto-cleanup resource wrapper
#define AUTO_CLEANUP_RESOURCE(resource_var, resource_val) \
    AsyncResource* resource_var __attribute__((cleanup(async_resource_cleanup_wrapper))) = resource_val

// Resource acquisition with timeout
#define ACQUIRE_RESOURCE_WITH_TIMEOUT(resource, timeout, result_var) \
    Result_void_ptr result_var = async_resource_acquire(resource, timeout)

// Safe resource usage pattern
#define USE_RESOURCE(resource, timeout, block) \
    do { \
        ACQUIRE_RESOURCE_WITH_TIMEOUT(resource, timeout, __acquire_result); \
        if (!__acquire_result.is_error) { \
            block \
            async_resource_release(resource); \
        } else { \
            /* Handle acquisition error */ \
        } \
    } while(0)

// Global resource manager operations
AsyncResourceManager* async_resource_manager_global(void);
Result_void_ptr async_resource_manager_init(void);
void async_resource_manager_shutdown(void);

// Resource manager configuration
void async_resource_manager_set_default_timeout(uint64_t timeout_ms);
void async_resource_manager_enable_tracking(bool enable);
void async_resource_manager_enable_leak_detection(bool enable);

// Resource manager statistics and monitoring
typedef struct AsyncResourceStats {
    uint64_t total_resources_created;
    uint64_t currently_active_resources;
    uint64_t total_acquisitions;
    uint64_t successful_acquisitions;
    uint64_t failed_acquisitions;
    uint64_t total_releases;
    uint64_t automatic_cleanups;
    uint64_t resource_leaks_detected;
    uint64_t deferred_actions_executed;
    double average_acquisition_time_ms;
    double average_resource_lifetime_ms;
} AsyncResourceStats;

AsyncResourceStats async_resource_get_global_stats(void);
void async_resource_reset_global_stats(void);

// Resource debugging and monitoring
void async_resource_print_stats(AsyncResource* resource);
void async_resource_scope_print_stats(AsyncResourceScope* scope);
void async_resource_manager_print_global_stats(void);

// Resource leak detection
typedef struct ResourceLeak {
    uint64_t resource_id;
    char resource_name[64];
    AsyncResourceType type;
    uint64_t created_time_ns;
    uint64_t leaked_time_ns;
    const char* creation_location; // File:line where resource was created
} ResourceLeak;

ResourceLeak* async_resource_detect_leaks(size_t* leak_count);
void async_resource_free_leak_report(ResourceLeak* leaks, size_t count);

// Utility functions for resource cleanup
static inline void async_resource_cleanup_wrapper(AsyncResource** resource) {
    if (resource && *resource) {
        async_resource_release(*resource);
        async_resource_unref(*resource);
    }
}

// Common resource implementations

// Memory resource
AsyncResource* async_memory_resource_create(const char* name, size_t size, size_t alignment);
Result_void_ptr async_memory_resource_resize(AsyncResource* resource, size_t new_size);

// File resource  
AsyncResource* async_file_resource_create(const char* filename, const char* mode);
Result_void_ptr async_file_resource_read(AsyncResource* resource, void* buffer, size_t size, size_t* bytes_read);
Result_void_ptr async_file_resource_write(AsyncResource* resource, const void* buffer, size_t size, size_t* bytes_written);

// Network resource
AsyncResource* async_network_resource_create(const char* address, uint16_t port, bool is_server);
Result_void_ptr async_network_resource_connect(AsyncResource* resource, uint64_t timeout_ms);
Result_void_ptr async_network_resource_send(AsyncResource* resource, const void* data, size_t size);
Result_void_ptr async_network_resource_receive(AsyncResource* resource, void* buffer, size_t buffer_size, size_t* received);

// Lock resource
AsyncResource* async_lock_resource_create(const char* name, bool is_recursive);
Result_void_ptr async_lock_resource_try_acquire(AsyncResource* resource, uint64_t timeout_ms);

#endif // GOO_ASYNC_RESOURCE_H