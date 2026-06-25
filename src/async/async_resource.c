#include "../../include/async_resource.h"
#include "../../include/errors/error.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

// Global resource manager instance
static AsyncResourceManager* g_resource_manager = NULL;
static pthread_once_t g_manager_init_once = PTHREAD_ONCE_INIT;

// Global statistics
static AsyncResourceStats g_global_stats = {0};
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// Utility functions
static uint64_t get_current_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t generate_id(void) {
    static atomic_uint_fast64_t counter = 1;
    return atomic_fetch_add(&counter, 1);
}

// Initialize global resource manager
static void init_global_resource_manager(void) {
    g_resource_manager = calloc(1, sizeof(AsyncResourceManager));
    if (!g_resource_manager) return;
    
    g_resource_manager->resource_capacity = 64;
    g_resource_manager->all_resources = calloc(g_resource_manager->resource_capacity, sizeof(AsyncResource*));
    
    g_resource_manager->scope_capacity = 32;
    g_resource_manager->active_scopes = calloc(g_resource_manager->scope_capacity, sizeof(AsyncResourceScope*));
    
    g_resource_manager->default_timeout_ms = 10000; // 10 seconds default
    g_resource_manager->enable_resource_tracking = true;
    g_resource_manager->enable_leak_detection = true;
    g_resource_manager->auto_cleanup_on_shutdown = true;
    
    pthread_mutex_init(&g_resource_manager->manager_mutex, NULL);
    pthread_cond_init(&g_resource_manager->cleanup_requested, NULL);
    pthread_cond_init(&g_resource_manager->shutdown_complete, NULL);
    
    printf("🔧 Async resource manager initialized\n");
}

AsyncResourceManager* async_resource_manager_global(void) {
    pthread_once(&g_manager_init_once, init_global_resource_manager);
    return g_resource_manager;
}

// Core async resource operations
AsyncResource* async_resource_create(const char* name, AsyncResourceType type,
                                   AsyncResourceAcquireFn acquire_fn, AsyncResourceCleanupFn cleanup_fn,
                                   void* context, size_t context_size) {
    
    AsyncResource* resource = calloc(1, sizeof(AsyncResource));
    if (!resource) return NULL;
    
    resource->id = generate_id();
    strncpy(resource->name, name ? name : "unnamed", sizeof(resource->name) - 1);
    resource->type = type;
    resource->state = RESOURCE_STATE_CREATED;
    resource->priority = RESOURCE_PRIORITY_NORMAL;
    
    resource->acquire_fn = acquire_fn;
    resource->cleanup_fn = cleanup_fn;
    
    // Copy context if provided
    if (context && context_size > 0) {
        resource->context = malloc(context_size);
        if (!resource->context) {
            free(resource);
            return NULL;
        }
        memcpy(resource->context, context, context_size);
        resource->context_size = context_size;
    }
    
    resource->created_time_ns = get_current_time_ns();
    resource->acquisition_timeout_ms = 5000; // 5 second default
    atomic_init(&resource->ref_count, 1);
    
    // Initialize dependency tracking
    resource->dependency_capacity = 4;
    resource->dependencies = calloc(resource->dependency_capacity, sizeof(AsyncResource*));
    
    // Initialize synchronization
    if (pthread_mutex_init(&resource->resource_mutex, NULL) != 0 ||
        pthread_cond_init(&resource->state_changed, NULL) != 0) {
        free(resource->context);
        free(resource->dependencies);
        free(resource);
        return NULL;
    }
    
    // Register with global manager
    AsyncResourceManager* manager = async_resource_manager_global();
    if (manager) {
        pthread_mutex_lock(&manager->manager_mutex);
        
        // Expand capacity if needed
        if (manager->resource_count >= manager->resource_capacity) {
            size_t new_capacity = manager->resource_capacity * 2;
            AsyncResource** new_array = realloc(manager->all_resources, 
                                              new_capacity * sizeof(AsyncResource*));
            if (new_array) {
                manager->all_resources = new_array;
                manager->resource_capacity = new_capacity;
            }
        }
        
        // Add to registry
        if (manager->resource_count < manager->resource_capacity) {
            manager->all_resources[manager->resource_count++] = resource;
            manager->total_resources_created++;
        }
        
        pthread_mutex_unlock(&manager->manager_mutex);
    }
    
    // Update global statistics
    pthread_mutex_lock(&g_stats_mutex);
    g_global_stats.total_resources_created++;
    g_global_stats.currently_active_resources++;
    pthread_mutex_unlock(&g_stats_mutex);
    
    printf("📦 Created async resource: %s (ID: %llu, Type: %d)\n", resource->name, resource->id, resource->type);
    
    return resource;
}

void async_resource_destroy(AsyncResource* resource) {
    if (!resource) return;
    
    printf("🗑️ Destroying async resource: %s (ID: %llu)\n", resource->name, resource->id);
    
    // Ensure resource is released
    if (resource->state == RESOURCE_STATE_ACQUIRED) {
        async_resource_release(resource);
    }
    
    // Clean up dependencies
    if (resource->dependencies) {
        for (size_t i = 0; i < resource->dependency_count; i++) {
            if (resource->dependencies[i]) {
                async_resource_unref(resource->dependencies[i]);
            }
        }
        free(resource->dependencies);
    }
    
    // Clean up context
    free(resource->context);
    
    // Clean up error
    if (resource->last_error) {
        free((void*)resource->last_error->message);
        free((void*)resource->last_error->hint);
        free(resource->last_error);
    }
    
    // Destroy synchronization objects
    pthread_mutex_destroy(&resource->resource_mutex);
    pthread_cond_destroy(&resource->state_changed);
    
    // Remove from global manager
    AsyncResourceManager* manager = async_resource_manager_global();
    if (manager) {
        pthread_mutex_lock(&manager->manager_mutex);
        
        for (size_t i = 0; i < manager->resource_count; i++) {
            if (manager->all_resources[i] == resource) {
                // Move last element to this position
                manager->all_resources[i] = manager->all_resources[manager->resource_count - 1];
                manager->resource_count--;
                break;
            }
        }
        
        pthread_mutex_unlock(&manager->manager_mutex);
    }
    
    // Update global statistics
    pthread_mutex_lock(&g_stats_mutex);
    g_global_stats.currently_active_resources--;
    pthread_mutex_unlock(&g_stats_mutex);
    
    free(resource);
}

Result_void_ptr async_resource_acquire(AsyncResource* resource, uint64_t timeout_ms) {
    if (!resource) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid resource"));
    }
    
    pthread_mutex_lock(&resource->resource_mutex);
    
    if (resource->state != RESOURCE_STATE_CREATED && resource->state != RESOURCE_STATE_RELEASED) {
        pthread_mutex_unlock(&resource->resource_mutex);
        return ERR_PTR(error_create(ERROR_INVALID_STATE, "Resource is not in a state where it can be acquired"));
    }
    
    printf("🔓 Acquiring resource: %s (ID: %llu)\n", resource->name, resource->id);
    
    resource->state = RESOURCE_STATE_ACQUIRING;
    resource->acquire_attempts++;
    
    // Update global statistics
    pthread_mutex_lock(&g_stats_mutex);
    g_global_stats.total_acquisitions++;
    pthread_mutex_unlock(&g_stats_mutex);
    
    uint64_t start_time = get_current_time_ns();
    
    pthread_mutex_unlock(&resource->resource_mutex);
    
    // Wait for dependencies first
    Result_void_ptr dep_result = async_resource_wait_for_dependencies(resource, timeout_ms);
    if (dep_result.is_error) {
        pthread_mutex_lock(&resource->resource_mutex);
        resource->state = RESOURCE_STATE_ERROR;
        resource->last_error = dep_result.error;
        pthread_mutex_unlock(&resource->resource_mutex);
        return dep_result;
    }
    
    // Call acquire function if provided
    Result_void_ptr acquire_result = {.is_error = false, .value = resource};
    
    if (resource->acquire_fn) {
        acquire_result = resource->acquire_fn(resource->context, resource->waker);
    }
    
    pthread_mutex_lock(&resource->resource_mutex);
    
    if (acquire_result.is_error) {
        resource->state = RESOURCE_STATE_ERROR;
        resource->last_error = acquire_result.error;
        
        pthread_mutex_lock(&g_stats_mutex);
        g_global_stats.failed_acquisitions++;
        pthread_mutex_unlock(&g_stats_mutex);
        
        printf("❌ Failed to acquire resource: %s (ID: %llu)\n", resource->name, resource->id);
    } else {
        // Store the handle produced by the acquire function (e.g. the malloc'd
        // buffer or the opened FILE*) so resource_data is valid for callers and
        // for cleanup_fn, which frees/closes resource_data. Without this
        // write-back resource_data stays NULL while state is ACQUIRED, causing
        // NULL dereferences in users and silent leaks of the handle. Guarded on
        // acquire_fn so resources with no acquire function (acquire_result
        // defaults to {.value = resource}) are not handed a bogus self-pointer.
        if (resource->acquire_fn) {
            resource->resource_data = acquire_result.value;
        }
        resource->state = RESOURCE_STATE_ACQUIRED;
        resource->acquired_time_ns = get_current_time_ns();
        resource->successful_acquisitions++;
        
        pthread_mutex_lock(&g_stats_mutex);
        g_global_stats.successful_acquisitions++;
        uint64_t acquisition_time = resource->acquired_time_ns - start_time;
        g_global_stats.average_acquisition_time_ms = 
            (g_global_stats.average_acquisition_time_ms + (acquisition_time / 1e6)) / 2.0;
        pthread_mutex_unlock(&g_stats_mutex);
        
        printf("✅ Successfully acquired resource: %s (ID: %llu)\n", resource->name, resource->id);
    }
    
    pthread_cond_broadcast(&resource->state_changed);
    pthread_mutex_unlock(&resource->resource_mutex);
    
    return acquire_result;
}

Result_void_ptr async_resource_release(AsyncResource* resource) {
    if (!resource) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid resource"));
    }
    
    pthread_mutex_lock(&resource->resource_mutex);
    
    if (resource->state != RESOURCE_STATE_ACQUIRED) {
        pthread_mutex_unlock(&resource->resource_mutex);
        return OK_PTR(resource); // Already released or not acquired
    }
    
    printf("🔒 Releasing resource: %s (ID: %llu)\n", resource->name, resource->id);
    
    resource->state = RESOURCE_STATE_RELEASING;
    resource->released_time_ns = get_current_time_ns();
    
    pthread_mutex_unlock(&resource->resource_mutex);
    
    // Call cleanup function
    if (resource->cleanup_fn && resource->resource_data) {
        resource->cleanup_fn(resource->resource_data, resource->context);
        resource->resource_data = NULL; // cleanup_fn freed/closed it; don't leave a dangling handle
        resource->cleanup_count++;
    }

    pthread_mutex_lock(&resource->resource_mutex);
    resource->state = RESOURCE_STATE_RELEASED;
    
    // Update statistics
    pthread_mutex_lock(&g_stats_mutex);
    g_global_stats.total_releases++;
    if (resource->acquired_time_ns > 0 && resource->released_time_ns > 0) {
        double lifetime_ms = (resource->released_time_ns - resource->acquired_time_ns) / 1e6;
        g_global_stats.average_resource_lifetime_ms = 
            (g_global_stats.average_resource_lifetime_ms + lifetime_ms) / 2.0;
    }
    pthread_mutex_unlock(&g_stats_mutex);
    
    pthread_cond_broadcast(&resource->state_changed);
    pthread_mutex_unlock(&resource->resource_mutex);
    
    printf("✅ Successfully released resource: %s (ID: %llu)\n", resource->name, resource->id);
    
    return OK_PTR(resource);
}

Result_void_ptr async_resource_cancel(AsyncResource* resource) {
    if (!resource) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid resource"));
    }
    
    pthread_mutex_lock(&resource->resource_mutex);
    
    printf("🚫 Cancelling resource: %s (ID: %llu)\n", resource->name, resource->id);
    
    AsyncResourceState old_state = resource->state;
    resource->state = RESOURCE_STATE_CANCELLED;
    
    // Cancel the associated cancellation token if present
    if (resource->cancel_token) {
        // cancellation_token_cancel(resource->cancel_token);
    }
    
    // If resource was acquired, clean it up
    if (old_state == RESOURCE_STATE_ACQUIRED && resource->cleanup_fn && resource->resource_data) {
        pthread_mutex_unlock(&resource->resource_mutex);
        resource->cleanup_fn(resource->resource_data, resource->context);
        pthread_mutex_lock(&resource->resource_mutex);
        resource->resource_data = NULL; // cleanup_fn freed/closed it; don't leave a dangling handle
        resource->cleanup_count++;
    }
    
    pthread_cond_broadcast(&resource->state_changed);
    pthread_mutex_unlock(&resource->resource_mutex);
    
    return OK_PTR(resource);
}

// Resource state management
AsyncResourceState async_resource_get_state(AsyncResource* resource) {
    if (!resource) return RESOURCE_STATE_ERROR;
    
    pthread_mutex_lock(&resource->resource_mutex);
    AsyncResourceState state = resource->state;
    pthread_mutex_unlock(&resource->resource_mutex);
    
    return state;
}

bool async_resource_is_acquired(AsyncResource* resource) {
    return async_resource_get_state(resource) == RESOURCE_STATE_ACQUIRED;
}

bool async_resource_is_healthy(AsyncResource* resource) {
    if (!resource) return false;
    
    pthread_mutex_lock(&resource->resource_mutex);
    bool healthy = (resource->state == RESOURCE_STATE_ACQUIRED && 
                   resource->last_error == NULL);
    
    // Check status function if provided
    if (healthy && resource->status_fn && resource->resource_data) {
        healthy = resource->status_fn(resource->resource_data, resource->context);
    }
    
    pthread_mutex_unlock(&resource->resource_mutex);
    
    return healthy;
}

// Resource references and sharing
AsyncResource* async_resource_ref(AsyncResource* resource) {
    if (!resource) return NULL;
    atomic_fetch_add(&resource->ref_count, 1);
    return resource;
}

void async_resource_unref(AsyncResource* resource) {
    if (!resource) return;
    
    int ref_count = atomic_fetch_sub(&resource->ref_count, 1);
    if (ref_count == 1) {
        // Last reference - destroy the resource
        async_resource_destroy(resource);
    }
}

// Resource dependencies
Result_void_ptr async_resource_add_dependency(AsyncResource* resource, AsyncResource* dependency) {
    if (!resource || !dependency) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid resource or dependency"));
    }
    
    pthread_mutex_lock(&resource->resource_mutex);
    
    // Expand capacity if needed
    if (resource->dependency_count >= resource->dependency_capacity) {
        size_t new_capacity = resource->dependency_capacity * 2;
        AsyncResource** new_deps = realloc(resource->dependencies, 
                                         new_capacity * sizeof(AsyncResource*));
        if (!new_deps) {
            pthread_mutex_unlock(&resource->resource_mutex);
            return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to expand dependency array"));
        }
        resource->dependencies = new_deps;
        resource->dependency_capacity = new_capacity;
    }
    
    // Add dependency with reference
    resource->dependencies[resource->dependency_count++] = async_resource_ref(dependency);
    
    pthread_mutex_unlock(&resource->resource_mutex);
    
    printf("🔗 Added dependency: %s -> %s\n", resource->name, dependency->name);
    
    return OK_PTR(resource);
}

Result_void_ptr async_resource_remove_dependency(AsyncResource* resource, AsyncResource* dependency) {
    if (!resource || !dependency) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid resource or dependency"));
    }
    
    pthread_mutex_lock(&resource->resource_mutex);
    
    // Find and remove dependency
    for (size_t i = 0; i < resource->dependency_count; i++) {
        if (resource->dependencies[i] == dependency) {
            async_resource_unref(resource->dependencies[i]);
            
            // Move last element to this position
            resource->dependencies[i] = resource->dependencies[resource->dependency_count - 1];
            resource->dependency_count--;
            
            pthread_mutex_unlock(&resource->resource_mutex);
            
            printf("🔗 Removed dependency: %s -> %s\n", resource->name, dependency->name);
            return OK_PTR(resource);
        }
    }
    
    pthread_mutex_unlock(&resource->resource_mutex);
    
    return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Dependency not found"));
}

Result_void_ptr async_resource_wait_for_dependencies(AsyncResource* resource, uint64_t timeout_ms) {
    if (!resource) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid resource"));
    }
    
    pthread_mutex_lock(&resource->resource_mutex);
    
    if (resource->dependency_count == 0) {
        pthread_mutex_unlock(&resource->resource_mutex);
        return OK_PTR(resource);
    }
    
    printf("⏳ Waiting for %zu dependencies for resource: %s\n", 
           resource->dependency_count, resource->name);
    
    // Create a copy of dependencies to check
    size_t dep_count = resource->dependency_count;
    AsyncResource** deps = malloc(dep_count * sizeof(AsyncResource*));
    if (!deps) {
        pthread_mutex_unlock(&resource->resource_mutex);
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate dependency array"));
    }
    
    for (size_t i = 0; i < dep_count; i++) {
        deps[i] = async_resource_ref(resource->dependencies[i]);
    }
    
    pthread_mutex_unlock(&resource->resource_mutex);
    
    // Wait for each dependency to be acquired
    uint64_t start_time = get_current_time_ns();
    uint64_t deadline = start_time + (timeout_ms * 1000000ULL);
    
    for (size_t i = 0; i < dep_count; i++) {
        AsyncResource* dep = deps[i];
        
        while (async_resource_get_state(dep) != RESOURCE_STATE_ACQUIRED) {
            uint64_t current_time = get_current_time_ns();
            if (current_time >= deadline) {
                // Cleanup references
                for (size_t j = 0; j < dep_count; j++) {
                    async_resource_unref(deps[j]);
                }
                free(deps);
                return ERR_PTR(error_create(ERROR_OPERATION_TIMEOUT, "Dependency wait timeout"));
            }
            
            usleep(1000); // Sleep 1ms
        }
        
        async_resource_unref(dep);
    }
    
    free(deps);
    
    printf("✅ All dependencies ready for resource: %s\n", resource->name);
    return OK_PTR(resource);
}

// Statistics and monitoring
AsyncResourceStats async_resource_get_global_stats(void) {
    pthread_mutex_lock(&g_stats_mutex);
    AsyncResourceStats stats = g_global_stats;
    pthread_mutex_unlock(&g_stats_mutex);
    return stats;
}

void async_resource_reset_global_stats(void) {
    pthread_mutex_lock(&g_stats_mutex);
    memset(&g_global_stats, 0, sizeof(AsyncResourceStats));
    pthread_mutex_unlock(&g_stats_mutex);
}

void async_resource_print_stats(AsyncResource* resource) {
    if (!resource) return;
    
    pthread_mutex_lock(&resource->resource_mutex);
    
    printf("\n📊 Resource Statistics: %s (ID: %llu)\n", resource->name, resource->id);
    printf("================================\n");
    printf("Type: %d\n", resource->type);
    printf("State: %d\n", resource->state);
    printf("Priority: %d\n", resource->priority);
    printf("Reference Count: %d\n", atomic_load(&resource->ref_count));
    printf("Dependencies: %zu\n", resource->dependency_count);
    printf("Acquire Attempts: %llu\n", resource->acquire_attempts);
    printf("Successful Acquisitions: %llu\n", resource->successful_acquisitions);
    printf("Cleanup Count: %llu\n", resource->cleanup_count);
    
    if (resource->created_time_ns > 0) {
        double age_ms = (get_current_time_ns() - resource->created_time_ns) / 1e6;
        printf("Age: %.2f ms\n", age_ms);
    }
    
    if (resource->acquired_time_ns > 0 && resource->released_time_ns > 0) {
        double lifetime_ms = (resource->released_time_ns - resource->acquired_time_ns) / 1e6;
        printf("Last Lifetime: %.2f ms\n", lifetime_ms);
    }
    
    pthread_mutex_unlock(&resource->resource_mutex);
}

void async_resource_manager_print_global_stats(void) {
    AsyncResourceStats stats = async_resource_get_global_stats();
    
    printf("\n📈 Global Async Resource Statistics\n");
    printf("===================================\n");
    printf("Total Resources Created: %llu\n", stats.total_resources_created);
    printf("Currently Active: %llu\n", stats.currently_active_resources);
    printf("Total Acquisitions: %llu\n", stats.total_acquisitions);
    printf("Successful Acquisitions: %llu\n", stats.successful_acquisitions);
    printf("Failed Acquisitions: %llu\n", stats.failed_acquisitions);
    printf("Total Releases: %llu\n", stats.total_releases);
    printf("Automatic Cleanups: %llu\n", stats.automatic_cleanups);
    printf("Resource Leaks Detected: %llu\n", stats.resource_leaks_detected);
    printf("Deferred Actions Executed: %llu\n", stats.deferred_actions_executed);
    printf("Average Acquisition Time: %.2f ms\n", stats.average_acquisition_time_ms);
    printf("Average Resource Lifetime: %.2f ms\n", stats.average_resource_lifetime_ms);
    
    if (stats.total_acquisitions > 0) {
        double success_rate = (double)stats.successful_acquisitions / stats.total_acquisitions * 100.0;
        printf("Acquisition Success Rate: %.1f%%\n", success_rate);
    }
}

// Deferred action implementation
DeferredAction* deferred_action_create(const char* name, void (*action_fn)(void*, void*),
                                     void* data, void* context, size_t data_size) {
    DeferredAction* action = calloc(1, sizeof(DeferredAction));
    if (!action) return NULL;
    
    action->id = generate_id();
    strncpy(action->name, name ? name : "unnamed", sizeof(action->name) - 1);
    action->action_fn = action_fn;
    action->context = context;
    action->priority = RESOURCE_PRIORITY_NORMAL;
    action->scheduled_time_ns = get_current_time_ns();
    
    // Copy data if provided
    if (data && data_size > 0) {
        action->data = malloc(data_size);
        if (!action->data) {
            free(action);
            return NULL;
        }
        memcpy(action->data, data, data_size);
        action->data_size = data_size;
    } else {
        action->data = data;
        action->data_size = data_size;
    }
    
    // Default to execute on all conditions
    action->execute_on_success = true;
    action->execute_on_error = true;
    action->execute_on_cancel = true;
    
    printf("📋 Created deferred action: %s (ID: %llu)\n", action->name, action->id);
    
    return action;
}

void deferred_action_destroy(DeferredAction* action) {
    if (!action) return;
    
    printf("🗑️ Destroying deferred action: %s (ID: %llu)\n", action->name, action->id);
    
    // Free copied data
    if (action->data && action->data_size > 0) {
        free(action->data);
    }
    
    // Clean up execution error
    if (action->execution_error) {
        free((void*)action->execution_error->message);
        free((void*)action->execution_error->hint);
        free(action->execution_error);
    }
    
    free(action);
}

// Execute a single deferred action
static Result_void_ptr execute_single_deferred_action(DeferredAction* action, bool on_success, 
                                                     bool on_error, bool on_cancel) {
    if (!action || action->is_executed) {
        return OK_PTR(action);
    }
    
    // Check execution conditions
    bool should_execute = false;
    if (on_success && action->execute_on_success) should_execute = true;
    if (on_error && action->execute_on_error) should_execute = true;
    if (on_cancel && action->execute_on_cancel) should_execute = true;
    
    if (!should_execute) {
        return OK_PTR(action);
    }
    
    printf("▶️ Executing deferred action: %s\n", action->name);
    
    action->executed_time_ns = get_current_time_ns();
    action->is_executed = true;
    
    // Execute the action function
    if (action->action_fn) {
        // Note: In a real implementation, we might want to add signal handling
        action->action_fn(action->data, action->context);
        
        // Update global statistics
        pthread_mutex_lock(&g_stats_mutex);
        g_global_stats.deferred_actions_executed++;
        pthread_mutex_unlock(&g_stats_mutex);
        
        printf("✅ Deferred action completed: %s\n", action->name);
    }
    
    return OK_PTR(action);
}

// Resource scope implementation
AsyncResourceScope* async_resource_scope_create(const char* name, void* async_context) {
    AsyncResourceScope* scope = calloc(1, sizeof(AsyncResourceScope));
    if (!scope) return NULL;
    
    scope->id = generate_id();
    strncpy(scope->name, name ? name : "unnamed", sizeof(scope->name) - 1);
    scope->async_context = async_context;
    
    scope->resource_capacity = 16;
    scope->resources = calloc(scope->resource_capacity, sizeof(AsyncResource*));
    if (!scope->resources) {
        free(scope);
        return NULL;
    }
    
    scope->is_active = true;
    scope->auto_cleanup_on_exit = true;
    scope->cleanup_reverse_order = true;
    scope->created_time_ns = get_current_time_ns();
    
    // Initialize synchronization
    if (pthread_mutex_init(&scope->scope_mutex, NULL) != 0 ||
        pthread_cond_init(&scope->cleanup_complete, NULL) != 0) {
        free(scope->resources);
        free(scope);
        return NULL;
    }
    
    // Register with global manager
    AsyncResourceManager* manager = async_resource_manager_global();
    if (manager) {
        pthread_mutex_lock(&manager->manager_mutex);
        
        // Expand capacity if needed
        if (manager->scope_count >= manager->scope_capacity) {
            size_t new_capacity = manager->scope_capacity * 2;
            AsyncResourceScope** new_array = realloc(manager->active_scopes, 
                                                    new_capacity * sizeof(AsyncResourceScope*));
            if (new_array) {
                manager->active_scopes = new_array;
                manager->scope_capacity = new_capacity;
            }
        }
        
        // Add to registry
        if (manager->scope_count < manager->scope_capacity) {
            manager->active_scopes[manager->scope_count++] = scope;
        }
        
        pthread_mutex_unlock(&manager->manager_mutex);
    }
    
    printf("🎯 Created async resource scope: %s (ID: %llu)\n", scope->name, scope->id);
    
    return scope;
}

void async_resource_scope_destroy(AsyncResourceScope* scope) {
    if (!scope) return;
    
    printf("🗑️ Destroying async resource scope: %s (ID: %llu)\n", scope->name, scope->id);
    
    // Clean up if not already done
    if (scope->auto_cleanup_on_exit) {
        async_resource_scope_cleanup(scope);
    }
    
    // Clean up resources array
    free(scope->resources);
    
    // Clean up deferred actions
    DeferredAction* current = scope->first_deferred;
    while (current) {
        DeferredAction* next = current->next;
        deferred_action_destroy(current);
        current = next;
    }
    
    // Clean up error
    if (scope->scope_error) {
        free((void*)scope->scope_error->message);
        free((void*)scope->scope_error->hint);
        free(scope->scope_error);
    }
    
    // Destroy synchronization objects
    pthread_mutex_destroy(&scope->scope_mutex);
    pthread_cond_destroy(&scope->cleanup_complete);
    
    // Remove from global manager
    AsyncResourceManager* manager = async_resource_manager_global();
    if (manager) {
        pthread_mutex_lock(&manager->manager_mutex);
        
        for (size_t i = 0; i < manager->scope_count; i++) {
            if (manager->active_scopes[i] == scope) {
                // Move last element to this position
                manager->active_scopes[i] = manager->active_scopes[manager->scope_count - 1];
                manager->scope_count--;
                break;
            }
        }
        
        pthread_mutex_unlock(&manager->manager_mutex);
    }
    
    scope->destroyed_time_ns = get_current_time_ns();
    free(scope);
}

Result_void_ptr async_resource_scope_add(AsyncResourceScope* scope, AsyncResource* resource) {
    if (!scope || !resource) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid scope or resource"));
    }
    
    pthread_mutex_lock(&scope->scope_mutex);
    
    if (!scope->is_active) {
        pthread_mutex_unlock(&scope->scope_mutex);
        return ERR_PTR(error_create(ERROR_INVALID_STATE, "Scope is not active"));
    }
    
    // Expand capacity if needed
    if (scope->resource_count >= scope->resource_capacity) {
        size_t new_capacity = scope->resource_capacity * 2;
        AsyncResource** new_array = realloc(scope->resources, 
                                           new_capacity * sizeof(AsyncResource*));
        if (!new_array) {
            pthread_mutex_unlock(&scope->scope_mutex);
            return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to expand scope resources"));
        }
        scope->resources = new_array;
        scope->resource_capacity = new_capacity;
    }
    
    // Add resource with reference
    scope->resources[scope->resource_count++] = async_resource_ref(resource);
    resource->scope = scope;
    scope->total_resources_managed++;
    
    pthread_mutex_unlock(&scope->scope_mutex);
    
    printf("➕ Added resource to scope: %s -> %s\n", resource->name, scope->name);
    
    return OK_PTR(scope);
}

Result_void_ptr async_resource_scope_remove(AsyncResourceScope* scope, AsyncResource* resource) {
    if (!scope || !resource) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid scope or resource"));
    }
    
    pthread_mutex_lock(&scope->scope_mutex);
    
    // Find and remove resource
    for (size_t i = 0; i < scope->resource_count; i++) {
        if (scope->resources[i] == resource) {
            async_resource_unref(scope->resources[i]);
            resource->scope = NULL;
            
            // Move last element to this position
            scope->resources[i] = scope->resources[scope->resource_count - 1];
            scope->resource_count--;
            
            pthread_mutex_unlock(&scope->scope_mutex);
            
            printf("➖ Removed resource from scope: %s -> %s\n", resource->name, scope->name);
            return OK_PTR(scope);
        }
    }
    
    pthread_mutex_unlock(&scope->scope_mutex);
    
    return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Resource not found in scope"));
}

Result_void_ptr async_resource_scope_cleanup(AsyncResourceScope* scope) {
    if (!scope) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid scope"));
    }
    
    pthread_mutex_lock(&scope->scope_mutex);
    
    if (scope->cleanup_in_progress) {
        pthread_mutex_unlock(&scope->scope_mutex);
        return OK_PTR(scope);
    }
    
    printf("🧹 Starting scope cleanup: %s (ID: %llu)\n", scope->name, scope->id);
    
    scope->cleanup_in_progress = true;
    scope->is_active = false;
    
    // Determine cleanup order
    size_t resource_count = scope->resource_count;
    if (resource_count == 0) {
        scope->cleanup_in_progress = false;
        pthread_cond_broadcast(&scope->cleanup_complete);
        pthread_mutex_unlock(&scope->scope_mutex);
        return OK_PTR(scope);
    }
    
    // Create cleanup list
    AsyncResource** cleanup_list = malloc(resource_count * sizeof(AsyncResource*));
    if (!cleanup_list) {
        scope->cleanup_in_progress = false;
        pthread_mutex_unlock(&scope->scope_mutex);
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate cleanup list"));
    }
    
    // Copy resources and add references for cleanup
    for (size_t i = 0; i < resource_count; i++) {
        if (scope->cleanup_reverse_order) {
            cleanup_list[i] = async_resource_ref(scope->resources[resource_count - 1 - i]);
        } else {
            cleanup_list[i] = async_resource_ref(scope->resources[i]);
        }
    }
    
    pthread_mutex_unlock(&scope->scope_mutex);
    
    // Clean up resources in order
    size_t successful_cleanups = 0;
    size_t failed_cleanups = 0;
    
    for (size_t i = 0; i < resource_count; i++) {
        AsyncResource* resource = cleanup_list[i];
        
        printf("🧹 Cleaning up resource in scope: %s\n", resource->name);
        
        Result_void_ptr release_result = async_resource_release(resource);
        if (release_result.is_error) {
            printf("❌ Failed to clean up resource: %s\n", resource->name);
            failed_cleanups++;
        } else {
            successful_cleanups++;
        }
        
        async_resource_unref(resource);
    }
    
    free(cleanup_list);
    
    // Execute deferred actions
    execute_deferred_actions(scope, successful_cleanups == resource_count, 
                           failed_cleanups > 0, false);
    
    pthread_mutex_lock(&scope->scope_mutex);
    
    scope->successful_cleanups += successful_cleanups;
    scope->failed_cleanups += failed_cleanups;
    scope->cleanup_in_progress = false;
    
    // Update global statistics
    pthread_mutex_lock(&g_stats_mutex);
    g_global_stats.automatic_cleanups += successful_cleanups;
    pthread_mutex_unlock(&g_stats_mutex);
    
    pthread_cond_broadcast(&scope->cleanup_complete);
    pthread_mutex_unlock(&scope->scope_mutex);
    
    printf("✅ Scope cleanup completed: %s (Success: %zu, Failed: %zu)\n", 
           scope->name, successful_cleanups, failed_cleanups);
    
    return OK_PTR(scope);
}

Result_void_ptr async_resource_scope_defer(AsyncResourceScope* scope, DeferredAction* action) {
    if (!scope || !action) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid scope or action"));
    }
    
    pthread_mutex_lock(&scope->scope_mutex);
    
    // Add to deferred action list
    if (!scope->first_deferred) {
        scope->first_deferred = action;
        scope->last_deferred = action;
    } else {
        scope->last_deferred->next = action;
        scope->last_deferred = action;
    }
    
    scope->deferred_count++;
    
    pthread_mutex_unlock(&scope->scope_mutex);
    
    printf("📋 Deferred action added to scope: %s -> %s\n", action->name, scope->name);
    
    return OK_PTR(scope);
}

Result_void_ptr async_resource_defer(AsyncResource* resource, DeferredAction* action) {
    if (!resource || !action) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid resource or action"));
    }
    
    action->associated_resource = resource;
    
    if (resource->scope) {
        return async_resource_scope_defer(resource->scope, action);
    }
    
    // Resource not in a scope - execute immediately on resource cleanup
    printf("📋 Deferred action associated with resource: %s -> %s\n", action->name, resource->name);
    
    return OK_PTR(resource);
}

Result_void_ptr execute_deferred_actions(AsyncResourceScope* scope, bool on_success, 
                                       bool on_error, bool on_cancel) {
    if (!scope) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid scope"));
    }
    
    pthread_mutex_lock(&scope->scope_mutex);
    
    DeferredAction* current = scope->first_deferred;
    size_t executed_count = 0;
    size_t failed_count = 0;
    
    printf("⚙️ Executing deferred actions in scope: %s (Success: %s, Error: %s, Cancel: %s)\n", 
           scope->name, on_success ? "yes" : "no", on_error ? "yes" : "no", on_cancel ? "yes" : "no");
    
    pthread_mutex_unlock(&scope->scope_mutex);
    
    // Execute actions in order
    while (current) {
        Result_void_ptr exec_result = execute_single_deferred_action(current, on_success, on_error, on_cancel);
        
        if (exec_result.is_error) {
            failed_count++;
            if (scope->stop_on_error) {
                printf("🛑 Stopping deferred action execution due to error\n");
                break;
            }
        } else if (current->is_executed) {
            executed_count++;
        }
        
        current = current->next;
    }
    
    printf("✅ Deferred actions executed: %zu successful, %zu failed\n", executed_count, failed_count);
    
    return OK_PTR(scope);
}

void async_resource_scope_print_stats(AsyncResourceScope* scope) {
    if (!scope) return;
    
    pthread_mutex_lock(&scope->scope_mutex);
    
    printf("\n📊 Resource Scope Statistics: %s (ID: %llu)\n", scope->name, scope->id);
    printf("==========================================\n");
    printf("Active: %s\n", scope->is_active ? "Yes" : "No");
    printf("Current Resources: %zu\n", scope->resource_count);
    printf("Total Managed: %llu\n", scope->total_resources_managed);
    printf("Successful Cleanups: %llu\n", scope->successful_cleanups);
    printf("Failed Cleanups: %llu\n", scope->failed_cleanups);
    printf("Deferred Actions: %zu\n", scope->deferred_count);
    printf("Cleanup in Progress: %s\n", scope->cleanup_in_progress ? "Yes" : "No");
    
    if (scope->created_time_ns > 0) {
        double age_ms = (get_current_time_ns() - scope->created_time_ns) / 1e6;
        printf("Age: %.2f ms\n", age_ms);
    }
    
    pthread_mutex_unlock(&scope->scope_mutex);
}

// Resource manager operations
Result_void_ptr async_resource_manager_init(void) {
    AsyncResourceManager* manager = async_resource_manager_global();
    if (!manager) {
        return ERR_PTR(error_create(ERROR_INITIALIZATION_FAILED, "Failed to initialize resource manager"));
    }
    
    printf("✅ Async resource manager initialized successfully\n");
    return OK_PTR(manager);
}

void async_resource_manager_shutdown(void) {
    AsyncResourceManager* manager = async_resource_manager_global();
    if (!manager) return;
    
    printf("🛑 Shutting down async resource manager\n");
    
    pthread_mutex_lock(&manager->manager_mutex);
    manager->is_shutting_down = true;
    
    // Clean up all active resources if auto-cleanup is enabled
    if (manager->auto_cleanup_on_shutdown) {
        printf("🧹 Auto-cleaning %zu active resources\n", manager->resource_count);
        
        // Create a copy of resource list to avoid modification during iteration
        size_t resource_count = manager->resource_count;
        AsyncResource** resources = malloc(resource_count * sizeof(AsyncResource*));
        if (resources) {
            for (size_t i = 0; i < resource_count; i++) {
                resources[i] = async_resource_ref(manager->all_resources[i]);
            }
            
            pthread_mutex_unlock(&manager->manager_mutex);
            
            // Release all resources
            for (size_t i = 0; i < resource_count; i++) {
                async_resource_release(resources[i]);
                async_resource_unref(resources[i]);
            }
            
            free(resources);
            pthread_mutex_lock(&manager->manager_mutex);
        }
        
        // Clean up all active scopes
        printf("🧹 Auto-cleaning %zu active scopes\n", manager->scope_count);
        
        size_t scope_count = manager->scope_count;
        AsyncResourceScope** scopes = malloc(scope_count * sizeof(AsyncResourceScope*));
        if (scopes) {
            for (size_t i = 0; i < scope_count; i++) {
                scopes[i] = manager->active_scopes[i];
            }
            
            pthread_mutex_unlock(&manager->manager_mutex);
            
            // Clean up all scopes
            for (size_t i = 0; i < scope_count; i++) {
                async_resource_scope_cleanup(scopes[i]);
                async_resource_scope_destroy(scopes[i]);
            }
            
            free(scopes);
            pthread_mutex_lock(&manager->manager_mutex);
        }
    }
    
    // Signal shutdown complete
    pthread_cond_broadcast(&manager->shutdown_complete);
    pthread_mutex_unlock(&manager->manager_mutex);
    
    printf("✅ Async resource manager shutdown complete\n");
}

// Configuration functions
void async_resource_manager_set_default_timeout(uint64_t timeout_ms) {
    AsyncResourceManager* manager = async_resource_manager_global();
    if (manager) {
        pthread_mutex_lock(&manager->manager_mutex);
        manager->default_timeout_ms = timeout_ms;
        pthread_mutex_unlock(&manager->manager_mutex);
        printf("⚙️ Set default resource timeout: %llu ms\n", timeout_ms);
    }
}

void async_resource_manager_enable_tracking(bool enable) {
    AsyncResourceManager* manager = async_resource_manager_global();
    if (manager) {
        pthread_mutex_lock(&manager->manager_mutex);
        manager->enable_resource_tracking = enable;
        pthread_mutex_unlock(&manager->manager_mutex);
        printf("⚙️ Resource tracking: %s\n", enable ? "enabled" : "disabled");
    }
}

void async_resource_manager_enable_leak_detection(bool enable) {
    AsyncResourceManager* manager = async_resource_manager_global();
    if (manager) {
        pthread_mutex_lock(&manager->manager_mutex);
        manager->enable_leak_detection = enable;
        pthread_mutex_unlock(&manager->manager_mutex);
        printf("⚙️ Leak detection: %s\n", enable ? "enabled" : "disabled");
    }
}

// Resource leak detection
ResourceLeak* async_resource_detect_leaks(size_t* leak_count) {
    AsyncResourceManager* manager = async_resource_manager_global();
    if (!manager || !leak_count) {
        if (leak_count) *leak_count = 0;
        return NULL;
    }
    
    pthread_mutex_lock(&manager->manager_mutex);
    
    if (!manager->enable_leak_detection) {
        pthread_mutex_unlock(&manager->manager_mutex);
        *leak_count = 0;
        return NULL;
    }
    
    // Count potential leaks (resources in error state or very old)
    uint64_t current_time = get_current_time_ns();
    uint64_t leak_threshold_ns = 300000000000ULL; // 5 minutes
    
    size_t potential_leaks = 0;
    for (size_t i = 0; i < manager->resource_count; i++) {
        AsyncResource* resource = manager->all_resources[i];
        
        pthread_mutex_lock(&resource->resource_mutex);
        bool is_leak = false;
        
        // Check for leaked conditions
        if (resource->state == RESOURCE_STATE_ERROR ||
            (resource->state == RESOURCE_STATE_ACQUIRED && 
             (current_time - resource->acquired_time_ns) > leak_threshold_ns) ||
            (resource->state == RESOURCE_STATE_CREATED && 
             (current_time - resource->created_time_ns) > leak_threshold_ns)) {
            is_leak = true;
        }
        
        pthread_mutex_unlock(&resource->resource_mutex);
        
        if (is_leak) {
            potential_leaks++;
        }
    }
    
    if (potential_leaks == 0) {
        pthread_mutex_unlock(&manager->manager_mutex);
        *leak_count = 0;
        return NULL;
    }
    
    // Allocate leak report
    ResourceLeak* leaks = calloc(potential_leaks, sizeof(ResourceLeak));
    if (!leaks) {
        pthread_mutex_unlock(&manager->manager_mutex);
        *leak_count = 0;
        return NULL;
    }
    
    // Fill leak report
    size_t leak_index = 0;
    for (size_t i = 0; i < manager->resource_count && leak_index < potential_leaks; i++) {
        AsyncResource* resource = manager->all_resources[i];
        
        pthread_mutex_lock(&resource->resource_mutex);
        bool is_leak = false;
        
        if (resource->state == RESOURCE_STATE_ERROR ||
            (resource->state == RESOURCE_STATE_ACQUIRED && 
             (current_time - resource->acquired_time_ns) > leak_threshold_ns) ||
            (resource->state == RESOURCE_STATE_CREATED && 
             (current_time - resource->created_time_ns) > leak_threshold_ns)) {
            is_leak = true;
        }
        
        if (is_leak) {
            ResourceLeak* leak = &leaks[leak_index++];
            leak->resource_id = resource->id;
            strncpy(leak->resource_name, resource->name, sizeof(leak->resource_name) - 1);
            leak->type = resource->type;
            leak->created_time_ns = resource->created_time_ns;
            leak->leaked_time_ns = current_time;
            leak->creation_location = "unknown"; // Would need debug info in real implementation
        }
        
        pthread_mutex_unlock(&resource->resource_mutex);
    }
    
    manager->total_resources_leaked += leak_index;
    
    pthread_mutex_unlock(&manager->manager_mutex);
    
    *leak_count = leak_index;
    
    if (leak_index > 0) {
        printf("⚠️ Detected %zu resource leaks\n", leak_index);
        
        // Update global statistics
        pthread_mutex_lock(&g_stats_mutex);
        g_global_stats.resource_leaks_detected += leak_index;
        pthread_mutex_unlock(&g_stats_mutex);
    }
    
    return leaks;
}

void async_resource_free_leak_report(ResourceLeak* leaks, size_t count) {
    if (leaks) {
        printf("🗑️ Freed leak report for %zu resources\n", count);
        free(leaks);
    }
}

// Common resource implementations

// Memory resource implementation
static Result_void_ptr memory_resource_acquire(void* context, AsyncWaker* waker) {
    (void)waker; // Unused parameter
    
    size_t* size_ptr = (size_t*)context;
    if (!size_ptr) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid memory size context"));
    }
    
    size_t size = *size_ptr;
    void* memory = malloc(size);
    if (!memory) {
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate memory"));
    }
    
    printf("💾 Allocated %zu bytes of memory\n", size);
    return OK_PTR(memory);
}

static void memory_resource_cleanup(void* resource_data, void* context) {
    (void)context; // Unused parameter
    
    if (resource_data) {
        free(resource_data);
        printf("💾 Freed allocated memory\n");
    }
}

AsyncResource* async_memory_resource_create(const char* name, size_t size, size_t alignment) {
    (void)alignment; // TODO: Implement aligned allocation
    
    size_t* size_context = malloc(sizeof(size_t));
    if (!size_context) return NULL;
    
    *size_context = size;
    
    AsyncResource* resource = async_resource_create(name, RESOURCE_TYPE_MEMORY,
                                                   memory_resource_acquire, memory_resource_cleanup,
                                                   size_context, sizeof(size_t));
    
    if (!resource) {
        free(size_context);
        return NULL;
    }
    
    printf("💾 Created memory resource: %s (%zu bytes)\n", name, size);
    return resource;
}

Result_void_ptr async_memory_resource_resize(AsyncResource* resource, size_t new_size) {
    if (!resource || resource->type != RESOURCE_TYPE_MEMORY) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid memory resource"));
    }
    
    if (!async_resource_is_acquired(resource)) {
        return ERR_PTR(error_create(ERROR_INVALID_STATE, "Memory resource not acquired"));
    }
    
    void* new_memory = realloc(resource->resource_data, new_size);
    if (!new_memory && new_size > 0) {
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to resize memory"));
    }
    
    resource->resource_data = new_memory;
    
    // Update size in context
    if (resource->context) {
        *(size_t*)resource->context = new_size;
    }
    
    printf("💾 Resized memory resource: %s (new size: %zu bytes)\n", resource->name, new_size);
    return OK_PTR(new_memory);
}

// File resource implementation
static Result_void_ptr file_resource_acquire(void* context, AsyncWaker* waker) {
    (void)waker; // Unused parameter
    
    char* file_info = (char*)context;
    if (!file_info) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid file context"));
    }
    
    // Context format: "filename\0mode\0"
    char* filename = file_info;
    char* mode = filename + strlen(filename) + 1;
    
    FILE* file = fopen(filename, mode);
    if (!file) {
        return ERR_PTR(error_create(ERROR_OPERATION_FAILED, "Failed to open file"));
    }
    
    printf("📁 Opened file: %s (mode: %s)\n", filename, mode);
    return OK_PTR(file);
}

static void file_resource_cleanup(void* resource_data, void* context) {
    (void)context; // Unused parameter
    
    FILE* file = (FILE*)resource_data;
    if (file) {
        fclose(file);
        printf("📁 Closed file\n");
    }
}

AsyncResource* async_file_resource_create(const char* filename, const char* mode) {
    if (!filename || !mode) return NULL;
    
    // Create context with filename and mode
    size_t filename_len = strlen(filename);
    size_t mode_len = strlen(mode);
    size_t context_size = filename_len + 1 + mode_len + 1;
    
    char* context = malloc(context_size);
    if (!context) return NULL;
    
    strcpy(context, filename);
    strcpy(context + filename_len + 1, mode);
    
    AsyncResource* resource = async_resource_create(filename, RESOURCE_TYPE_FILE,
                                                   file_resource_acquire, file_resource_cleanup,
                                                   context, context_size);
    
    if (!resource) {
        free(context);
        return NULL;
    }
    
    printf("📁 Created file resource: %s (mode: %s)\n", filename, mode);
    return resource;
}

Result_void_ptr async_file_resource_read(AsyncResource* resource, void* buffer, size_t size, size_t* bytes_read) {
    if (!resource || resource->type != RESOURCE_TYPE_FILE || !buffer || !bytes_read) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid file resource or parameters"));
    }
    
    if (!async_resource_is_acquired(resource)) {
        return ERR_PTR(error_create(ERROR_INVALID_STATE, "File resource not acquired"));
    }
    
    FILE* file = (FILE*)resource->resource_data;
    if (!file) {
        return ERR_PTR(error_create(ERROR_INVALID_STATE, "File handle is NULL"));
    }
    
    *bytes_read = fread(buffer, 1, size, file);
    
    if (*bytes_read < size && ferror(file)) {
        return ERR_PTR(error_create(ERROR_OPERATION_FAILED, "File read error"));
    }
    
    return OK_PTR(buffer);
}

Result_void_ptr async_file_resource_write(AsyncResource* resource, const void* buffer, size_t size, size_t* bytes_written) {
    if (!resource || resource->type != RESOURCE_TYPE_FILE || !buffer || !bytes_written) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid file resource or parameters"));
    }
    
    if (!async_resource_is_acquired(resource)) {
        return ERR_PTR(error_create(ERROR_INVALID_STATE, "File resource not acquired"));
    }
    
    FILE* file = (FILE*)resource->resource_data;
    if (!file) {
        return ERR_PTR(error_create(ERROR_INVALID_STATE, "File handle is NULL"));
    }
    
    *bytes_written = fwrite(buffer, 1, size, file);
    
    if (*bytes_written < size) {
        return ERR_PTR(error_create(ERROR_OPERATION_FAILED, "File write error"));
    }
    
    fflush(file);
    return OK_PTR((void*)buffer);
}