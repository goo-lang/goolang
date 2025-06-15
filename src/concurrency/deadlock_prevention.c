#include "../../include/deadlock_prevention.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

// Utility functions
static uint64_t get_monotonic_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

[[nodiscard]] static uint64_t generate_unique_id(void) {
    static atomic_uint_fast64_t counter = 1;
    return atomic_fetch_add(&counter, 1);
}

// Configuration helpers
DeadlockPreventionConfig deadlock_prevention_config_default(void) {
    return (DeadlockPreventionConfig) {
        .primary_strategy = LOCK_STRATEGY_TIMEOUT,
        .fallback_strategy = LOCK_STRATEGY_ORDERED,
        .detection_method = DETECTION_WAIT_FOR_GRAPH,
        .default_timeout_ms = 5000,          // 5 seconds
        .detection_interval_ms = 1000,       // 1 second
        .enable_wound_wait = false,
        .enable_wait_die = false,
        .enable_banker_algorithm = false,
        .enable_lock_ordering = true,
        .enable_timeout_based_prevention = true,
        .max_resources_per_entity = 64,
        .max_concurrent_acquisitions = 16,
        .enable_performance_optimization = true,
        .enable_adaptive_strategies = false,
        .enable_load_balancing = false
    };
}

DeadlockPreventionConfig deadlock_prevention_config_conservative(void) {
    DeadlockPreventionConfig config = deadlock_prevention_config_default();
    config.primary_strategy = LOCK_STRATEGY_ORDERED;
    config.fallback_strategy = LOCK_STRATEGY_BANKER;
    config.enable_banker_algorithm = true;
    config.enable_lock_ordering = true;
    config.default_timeout_ms = 10000;      // 10 seconds
    config.max_resources_per_entity = 32;
    config.max_concurrent_acquisitions = 8;
    return config;
}

DeadlockPreventionConfig deadlock_prevention_config_aggressive(void) {
    DeadlockPreventionConfig config = deadlock_prevention_config_default();
    config.primary_strategy = LOCK_STRATEGY_WOUND_WAIT;
    config.fallback_strategy = LOCK_STRATEGY_WAIT_DIE;
    config.detection_method = DETECTION_CYCLE_DETECTION;
    config.enable_wound_wait = true;
    config.enable_wait_die = true;
    config.default_timeout_ms = 1000;       // 1 second
    config.detection_interval_ms = 500;     // 500ms
    config.enable_adaptive_strategies = true;
    return config;
}

DeadlockPreventionConfig deadlock_prevention_config_performance_focused(void) {
    DeadlockPreventionConfig config = deadlock_prevention_config_default();
    config.primary_strategy = LOCK_STRATEGY_TIMEOUT;
    config.detection_method = DETECTION_TIMEOUT;
    config.default_timeout_ms = 2000;       // 2 seconds
    config.detection_interval_ms = 2000;    // 2 seconds
    config.max_resources_per_entity = 128;
    config.max_concurrent_acquisitions = 32;
    config.enable_performance_optimization = true;
    config.enable_load_balancing = true;
    return config;
}

// Resource operations
Resource* resource_create(ResourceManager* manager, ResourceType type, const char* name) {
    if (!manager) return NULL;
    
    Resource* resource = calloc(1, sizeof(Resource));
    if (!resource) return NULL;
    
    resource->id = generate_unique_id();
    if (name) {
        strncpy(resource->name, name, sizeof(resource->name) - 1);
        resource->name[sizeof(resource->name) - 1] = '\0';
    } else {
        snprintf(resource->name, sizeof(resource->name), "resource_%lu", resource->id);
    }
    
    resource->type = type;
    resource->is_allocated = false;
    resource->is_exclusive = false;
    resource->current_holder_id = 0;
    
    // Initialize holder array for shared resources
    resource->holder_capacity = 16;
    resource->holders = calloc(resource->holder_capacity, sizeof(uint64_t));
    if (!resource->holders) {
        free(resource);
        return NULL;
    }
    
    // Initialize synchronization
    if (pthread_mutex_init(&resource->resource_mutex, NULL) != 0) {
        free(resource->holders);
        free(resource);
        return NULL;
    }
    
    if (pthread_cond_init(&resource->resource_available, NULL) != 0) {
        pthread_mutex_destroy(&resource->resource_mutex);
        free(resource->holders);
        free(resource);
        return NULL;
    }
    
    resource->allocation_time = 0;
    resource->last_access_time = get_monotonic_time_ns();
    resource->total_acquisitions = 0;
    
    // Add to manager
    pthread_mutex_lock(&manager->registry_mutex);
    
    if (manager->resource_count >= manager->resource_capacity) {
        // Expand capacity
        size_t new_capacity = manager->resource_capacity * 2;
        Resource** new_resources = realloc(manager->resources, new_capacity * sizeof(Resource*));
        if (!new_resources) {
            pthread_mutex_unlock(&manager->registry_mutex);
            pthread_cond_destroy(&resource->resource_available);
            pthread_mutex_destroy(&resource->resource_mutex);
            free(resource->holders);
            free(resource);
            return NULL;
        }
        manager->resources = new_resources;
        manager->resource_capacity = new_capacity;
    }
    
    manager->resources[manager->resource_count++] = resource;
    
    pthread_mutex_unlock(&manager->registry_mutex);
    
    return resource;
}

void resource_destroy(Resource* resource) {
    if (!resource) return;
    
    // Clean up pending and granted requests
    ResourceRequest* req = resource->pending_requests;
    while (req) {
        ResourceRequest* next = req->next;
        resource_request_destroy(req);
        req = next;
    }
    
    req = resource->granted_requests;
    while (req) {
        ResourceRequest* next = req->next;
        resource_request_destroy(req);
        req = next;
    }
    
    // Clean up custom resource data
    if (resource->resource_data && resource->resource_destructor) {
        resource->resource_destructor(resource->resource_data);
    }
    
    free(resource->holders);
    
    pthread_mutex_destroy(&resource->resource_mutex);
    pthread_cond_destroy(&resource->resource_available);
    
    free(resource);
}

Resource* resource_find_by_id(ResourceManager* manager, uint64_t resource_id) {
    if (!manager) return NULL;
    
    pthread_mutex_lock(&manager->registry_mutex);
    
    for (size_t i = 0; i < manager->resource_count; i++) {
        if (manager->resources[i] && manager->resources[i]->id == resource_id) {
            Resource* found = manager->resources[i];
            pthread_mutex_unlock(&manager->registry_mutex);
            return found;
        }
    }
    
    pthread_mutex_unlock(&manager->registry_mutex);
    return NULL;
}

Resource* resource_find_by_name(ResourceManager* manager, const char* name) {
    if (!manager || !name) return NULL;
    
    pthread_mutex_lock(&manager->registry_mutex);
    
    for (size_t i = 0; i < manager->resource_count; i++) {
        if (manager->resources[i] && strcmp(manager->resources[i]->name, name) == 0) {
            Resource* found = manager->resources[i];
            pthread_mutex_unlock(&manager->registry_mutex);
            return found;
        }
    }
    
    pthread_mutex_unlock(&manager->registry_mutex);
    return NULL;
}

// Resource request operations
ResourceRequest* resource_request_create(uint64_t entity_id, ResourceType type, uint64_t resource_id, ResourceAccessMode mode) {
    ResourceRequest* request = calloc(1, sizeof(ResourceRequest));
    if (!request) return NULL;
    
    request->request_id = generate_unique_id();
    request->requesting_entity_id = entity_id;
    request->resource_type = type;
    request->resource_id = resource_id;
    request->access_mode = mode;
    request->request_time = get_monotonic_time_ns();
    request->timeout_ms = 5000;  // 5 second default
    request->priority = 0;
    request->is_granted = false;
    request->is_waiting = false;
    request->is_timed_out = false;
    request->grant_time = 0;
    
    return request;
}

void resource_request_destroy(ResourceRequest* request) {
    if (!request) return;
    
    // Clean up dependencies
    if (request->dependencies) {
        for (size_t i = 0; i < request->dependency_count; i++) {
            if (request->dependencies[i]) {
                resource_request_destroy(request->dependencies[i]);
            }
        }
        free(request->dependencies);
    }
    
    free(request);
}

// Resource manager operations
ResourceManager* resource_manager_create(void) {
    ResourceManager* manager = calloc(1, sizeof(ResourceManager));
    if (!manager) return NULL;
    
    manager->resource_capacity = 1000;
    manager->resources = calloc(manager->resource_capacity, sizeof(Resource*));
    if (!manager->resources) {
        free(manager);
        return NULL;
    }
    
    if (pthread_mutex_init(&manager->registry_mutex, NULL) != 0) {
        free(manager->resources);
        free(manager);
        return NULL;
    }
    
    manager->default_strategy = LOCK_STRATEGY_TIMEOUT;
    manager->default_timeout_ms = 5000;
    manager->max_concurrent_requests = 100;
    
    // Initialize pending requests array
    manager->pending_request_capacity = 10000;
    manager->pending_requests = calloc(manager->pending_request_capacity, sizeof(ResourceRequest*));
    if (!manager->pending_requests) {
        pthread_mutex_destroy(&manager->registry_mutex);
        free(manager->resources);
        free(manager);
        return NULL;
    }
    
    // Initialize resource pools
    manager->pool_count = 10;  // Support 10 different resource types
    manager->resource_pools = calloc(manager->pool_count, sizeof(*manager->resource_pools));
    if (!manager->resource_pools) {
        free(manager->pending_requests);
        pthread_mutex_destroy(&manager->registry_mutex);
        free(manager->resources);
        free(manager);
        return NULL;
    }
    
    // Initialize each resource pool
    for (size_t i = 0; i < manager->pool_count; i++) {
        manager->resource_pools[i].type = (ResourceType)i;
        manager->resource_pools[i].pool_capacity = 100;
        manager->resource_pools[i].pool = calloc(manager->resource_pools[i].pool_capacity, sizeof(Resource*));
        if (!manager->resource_pools[i].pool) {
            // Clean up already allocated pools
            for (size_t j = 0; j < i; j++) {
                free(manager->resource_pools[j].pool);
            }
            free(manager->resource_pools);
            free(manager->pending_requests);
            pthread_mutex_destroy(&manager->registry_mutex);
            free(manager->resources);
            free(manager);
            return NULL;
        }
    }
    
    return manager;
}

void resource_manager_destroy(ResourceManager* manager) {
    if (!manager) return;
    
    // Destroy all resources
    if (manager->resources) {
        for (size_t i = 0; i < manager->resource_count; i++) {
            if (manager->resources[i]) {
                resource_destroy(manager->resources[i]);
            }
        }
        free(manager->resources);
    }
    
    // Clean up pending requests
    if (manager->pending_requests) {
        for (size_t i = 0; i < manager->pending_request_count; i++) {
            if (manager->pending_requests[i]) {
                resource_request_destroy(manager->pending_requests[i]);
            }
        }
        free(manager->pending_requests);
    }
    
    // Clean up resource pools
    if (manager->resource_pools) {
        for (size_t i = 0; i < manager->pool_count; i++) {
            free(manager->resource_pools[i].pool);
        }
        free(manager->resource_pools);
    }
    
    // Destroy deadlock detector if present
    if (manager->deadlock_detector) {
        deadlock_detector_destroy(manager->deadlock_detector);
    }
    
    pthread_mutex_destroy(&manager->registry_mutex);
    free(manager);
}

// Core resource acquisition logic
static bool can_grant_request(Resource* resource, ResourceRequest* request) {
    switch (request->access_mode) {
        case ACCESS_MODE_EXCLUSIVE:
            return !resource->is_allocated;
            
        case ACCESS_MODE_SHARED:
        case ACCESS_MODE_READ:
            return !resource->is_exclusive;
            
        case ACCESS_MODE_WRITE:
            return !resource->is_allocated;
            
        default:
            return false;
    }
}

static Result_void_ptr grant_resource(Resource* resource, ResourceRequest* request) {
    pthread_mutex_lock(&resource->resource_mutex);
    
    if (!can_grant_request(resource, request)) {
        pthread_mutex_unlock(&resource->resource_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_RESOURCE_UNAVAILABLE,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Resource cannot be granted"),
            .hint = strdup("Resource is currently held in incompatible mode"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Grant the resource
    request->is_granted = true;
    request->grant_time = get_monotonic_time_ns();
    resource->is_allocated = true;
    resource->allocation_time = request->grant_time;
    resource->last_access_time = request->grant_time;
    resource->total_acquisitions++;
    
    switch (request->access_mode) {
        case ACCESS_MODE_EXCLUSIVE:
        case ACCESS_MODE_WRITE:
            resource->is_exclusive = true;
            resource->current_holder_id = request->requesting_entity_id;
            break;
            
        case ACCESS_MODE_SHARED:
        case ACCESS_MODE_READ:
            resource->is_exclusive = false;
            
            // Add to holders list
            if (resource->holder_count >= resource->holder_capacity) {
                // Expand holders array
                size_t new_capacity = resource->holder_capacity * 2;
                uint64_t* new_holders = realloc(resource->holders, new_capacity * sizeof(uint64_t));
                if (!new_holders) {
                    pthread_mutex_unlock(&resource->resource_mutex);
                    
                    Error* error = malloc(sizeof(Error));
                    *error = (Error){
                        .code = ERROR_OUT_OF_MEMORY,
                        .severity = ERROR_SEVERITY_ERROR,
                        .category = ERROR_CATEGORY_INTERNAL,
                        .message = strdup("Failed to expand holders array"),
                        .hint = NULL,
                        .location = (SourceLocation){0},
                        .next = NULL
                    };
                    return ERR_PTR(error);
                }
                resource->holders = new_holders;
                resource->holder_capacity = new_capacity;
            }
            
            resource->holders[resource->holder_count++] = request->requesting_entity_id;
            break;
    }
    
    // Move from pending to granted list
    if (resource->pending_requests == request) {
        resource->pending_requests = request->next;
    } else {
        ResourceRequest* prev = resource->pending_requests;
        while (prev && prev->next != request) {
            prev = prev->next;
        }
        if (prev) {
            prev->next = request->next;
        }
    }
    resource->pending_count--;
    
    // Add to granted list
    request->next = resource->granted_requests;
    resource->granted_requests = request;
    resource->granted_count++;
    
    pthread_cond_signal(&resource->resource_available);
    pthread_mutex_unlock(&resource->resource_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr resource_acquire(ResourceManager* manager, ResourceRequest* request) {
    return resource_acquire_timeout(manager, request, request->timeout_ms);
}

Result_void_ptr resource_acquire_timeout(ResourceManager* manager, ResourceRequest* request, uint64_t timeout_ms) {
    if (!manager || !request) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid manager or request"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    Resource* resource = resource_find_by_id(manager, request->resource_id);
    if (!resource) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Resource not found"),
            .hint = strdup("Ensure resource exists before requesting"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    uint64_t start_time = get_monotonic_time_ns();
    
    // Try immediate acquisition
    Result_void_ptr immediate_result = grant_resource(resource, request);
    if (!immediate_result.is_error) {
        manager->successful_allocations++;
        return immediate_result;
    }
    
    // Resource not immediately available - add to pending queue
    pthread_mutex_lock(&resource->resource_mutex);
    
    request->is_waiting = true;
    request->next = resource->pending_requests;
    resource->pending_requests = request;
    resource->pending_count++;
    
    // Wait with timeout
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    
    int wait_result = 0;
    while (!request->is_granted && !request->is_timed_out && wait_result != ETIMEDOUT) {
        if (timeout_ms == UINT64_MAX) {
            wait_result = pthread_cond_wait(&resource->resource_available, &resource->resource_mutex);
        } else {
            wait_result = pthread_cond_timedwait(&resource->resource_available, &resource->resource_mutex, &deadline);
        }
        
        // Try to grant again after waking up
        if (!request->is_granted && can_grant_request(resource, request)) {
            pthread_mutex_unlock(&resource->resource_mutex);
            Result_void_ptr grant_result = grant_resource(resource, request);
            if (!grant_result.is_error) {
                manager->successful_allocations++;
                return grant_result;
            }
            pthread_mutex_lock(&resource->resource_mutex);
        }
    }
    
    if (wait_result == ETIMEDOUT || request->is_timed_out) {
        // Remove from pending queue
        if (resource->pending_requests == request) {
            resource->pending_requests = request->next;
        } else {
            ResourceRequest* prev = resource->pending_requests;
            while (prev && prev->next != request) {
                prev = prev->next;
            }
            if (prev) {
                prev->next = request->next;
            }
        }
        resource->pending_count--;
        
        request->is_timed_out = true;
        manager->timeouts++;
        
        pthread_mutex_unlock(&resource->resource_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TIMEOUT_EXCEEDED,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Resource acquisition timed out"),
            .hint = strdup("Increase timeout or check for deadlocks"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_unlock(&resource->resource_mutex);
    
    if (request->is_granted) {
        manager->successful_allocations++;
        return OK_PTR(NULL);
    } else {
        manager->failed_allocations++;
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_RESOURCE_UNAVAILABLE,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to acquire resource"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
}

Result_void_ptr resource_release(ResourceManager* manager, uint64_t entity_id, uint64_t resource_id) {
    if (!manager) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid manager"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    Resource* resource = resource_find_by_id(manager, resource_id);
    if (!resource) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Resource not found"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&resource->resource_mutex);
    
    // Find and remove the request from granted list
    ResourceRequest* request = resource->granted_requests;
    ResourceRequest* prev = NULL;
    
    while (request) {
        if (request->requesting_entity_id == entity_id) {
            // Found the request to release
            if (prev) {
                prev->next = request->next;
            } else {
                resource->granted_requests = request->next;
            }
            resource->granted_count--;
            break;
        }
        prev = request;
        request = request->next;
    }
    
    if (!request) {
        pthread_mutex_unlock(&resource->resource_mutex);
        
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Entity does not hold this resource"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Remove from holders list
    if (resource->is_exclusive) {
        resource->current_holder_id = 0;
        resource->is_exclusive = false;
    } else {
        // Remove from shared holders
        for (size_t i = 0; i < resource->holder_count; i++) {
            if (resource->holders[i] == entity_id) {
                // Shift remaining holders
                for (size_t j = i; j < resource->holder_count - 1; j++) {
                    resource->holders[j] = resource->holders[j + 1];
                }
                resource->holder_count--;
                break;
            }
        }
    }
    
    // If no more holders, mark resource as unallocated
    if (resource->holder_count == 0 && resource->current_holder_id == 0) {
        resource->is_allocated = false;
        resource->allocation_time = 0;
    }
    
    // Signal waiting threads
    pthread_cond_signal(&resource->resource_available);
    
    pthread_mutex_unlock(&resource->resource_mutex);
    
    // Clean up the request
    resource_request_destroy(request);
    
    return OK_PTR(NULL);
}

Result_void_ptr resource_release_all(ResourceManager* manager, uint64_t entity_id) {
    if (!manager) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid manager"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&manager->registry_mutex);
    
    // Release resources from all resources held by this entity
    for (size_t i = 0; i < manager->resource_count; i++) {
        Resource* resource = manager->resources[i];
        if (!resource) continue;
        
        // Try to release this resource (ignore errors for resources not held)
        resource_release(manager, entity_id, resource->id);
    }
    
    pthread_mutex_unlock(&manager->registry_mutex);
    
    return OK_PTR(NULL);
}

// Banker's algorithm implementation
BankerState* banker_state_create(size_t process_count, size_t resource_type_count) {
    BankerState* state = calloc(1, sizeof(BankerState));
    if (!state) return NULL;
    
    state->process_count = process_count;
    state->resource_type_count = resource_type_count;
    
    // Allocate matrices
    state->allocation = malloc(process_count * sizeof(int*));
    state->max_need = malloc(process_count * sizeof(int*));
    state->need = malloc(process_count * sizeof(int*));
    
    if (!state->allocation || !state->max_need || !state->need) {
        free(state->allocation);
        free(state->max_need);
        free(state->need);
        free(state);
        return NULL;
    }
    
    for (size_t i = 0; i < process_count; i++) {
        state->allocation[i] = calloc(resource_type_count, sizeof(int));
        state->max_need[i] = calloc(resource_type_count, sizeof(int));
        state->need[i] = calloc(resource_type_count, sizeof(int));
        
        if (!state->allocation[i] || !state->max_need[i] || !state->need[i]) {
            // Clean up allocated arrays
            for (size_t j = 0; j <= i; j++) {
                free(state->allocation[j]);
                free(state->max_need[j]);
                free(state->need[j]);
            }
            free(state->allocation);
            free(state->max_need);
            free(state->need);
            free(state);
            return NULL;
        }
    }
    
    state->available = calloc(resource_type_count, sizeof(int));
    if (!state->available) {
        for (size_t i = 0; i < process_count; i++) {
            free(state->allocation[i]);
            free(state->max_need[i]);
            free(state->need[i]);
        }
        free(state->allocation);
        free(state->max_need);
        free(state->need);
        free(state);
        return NULL;
    }
    
    if (pthread_mutex_init(&state->banker_mutex, NULL) != 0) {
        free(state->available);
        for (size_t i = 0; i < process_count; i++) {
            free(state->allocation[i]);
            free(state->max_need[i]);
            free(state->need[i]);
        }
        free(state->allocation);
        free(state->max_need);
        free(state->need);
        free(state);
        return NULL;
    }
    
    return state;
}

void banker_state_destroy(BankerState* state) {
    if (!state) return;
    
    for (size_t i = 0; i < state->process_count; i++) {
        free(state->allocation[i]);
        free(state->max_need[i]);
        free(state->need[i]);
    }
    
    free(state->allocation);
    free(state->max_need);
    free(state->need);
    free(state->available);
    
    pthread_mutex_destroy(&state->banker_mutex);
    free(state);
}

bool banker_is_safe_state(BankerState* state) {
    if (!state) return false;
    
    pthread_mutex_lock(&state->banker_mutex);
    
    // Create work and finish arrays
    int* work = malloc(state->resource_type_count * sizeof(int));
    bool* finish = malloc(state->process_count * sizeof(bool));
    
    if (!work || !finish) {
        free(work);
        free(finish);
        pthread_mutex_unlock(&state->banker_mutex);
        return false;
    }
    
    // Initialize work = available
    for (size_t i = 0; i < state->resource_type_count; i++) {
        work[i] = state->available[i];
    }
    
    // Initialize finish = false for all processes
    for (size_t i = 0; i < state->process_count; i++) {
        finish[i] = false;
    }
    
    // Find processes that can complete
    bool found = true;
    while (found) {
        found = false;
        
        for (size_t i = 0; i < state->process_count; i++) {
            if (!finish[i]) {
                // Check if process i can complete
                bool can_complete = true;
                for (size_t j = 0; j < state->resource_type_count; j++) {
                    if (state->need[i][j] > work[j]) {
                        can_complete = false;
                        break;
                    }
                }
                
                if (can_complete) {
                    // Process i can complete
                    finish[i] = true;
                    found = true;
                    
                    // Add allocated resources back to work
                    for (size_t j = 0; j < state->resource_type_count; j++) {
                        work[j] += state->allocation[i][j];
                    }
                }
            }
        }
    }
    
    // Check if all processes can complete
    bool is_safe = true;
    for (size_t i = 0; i < state->process_count; i++) {
        if (!finish[i]) {
            is_safe = false;
            break;
        }
    }
    
    free(work);
    free(finish);
    pthread_mutex_unlock(&state->banker_mutex);
    
    return is_safe;
}

bool banker_can_grant_request(BankerState* state, size_t process_id, int* request) {
    if (!state || !request || process_id >= state->process_count) {
        return false;
    }
    
    pthread_mutex_lock(&state->banker_mutex);
    
    // Check if request exceeds need
    for (size_t i = 0; i < state->resource_type_count; i++) {
        if (request[i] > state->need[process_id][i]) {
            pthread_mutex_unlock(&state->banker_mutex);
            return false;
        }
    }
    
    // Check if request exceeds available
    for (size_t i = 0; i < state->resource_type_count; i++) {
        if (request[i] > state->available[i]) {
            pthread_mutex_unlock(&state->banker_mutex);
            return false;
        }
    }
    
    // Simulate granting the request
    for (size_t i = 0; i < state->resource_type_count; i++) {
        state->available[i] -= request[i];
        state->allocation[process_id][i] += request[i];
        state->need[process_id][i] -= request[i];
    }
    
    // Check if resulting state is safe
    bool is_safe = banker_is_safe_state(state);
    
    // Restore original state
    for (size_t i = 0; i < state->resource_type_count; i++) {
        state->available[i] += request[i];
        state->allocation[process_id][i] -= request[i];
        state->need[process_id][i] += request[i];
    }
    
    pthread_mutex_unlock(&state->banker_mutex);
    
    return is_safe;
}

Result_void_ptr banker_grant_request(BankerState* state, size_t process_id, int* request) {
    if (!state || !request || process_id >= state->process_count) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid banker state or request"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    if (!banker_can_grant_request(state, process_id, request)) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_BANKER_UNSAFE_STATE,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Granting request would lead to unsafe state"),
            .hint = strdup("Request denied by banker's algorithm"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&state->banker_mutex);
    
    // Grant the request
    for (size_t i = 0; i < state->resource_type_count; i++) {
        state->available[i] -= request[i];
        state->allocation[process_id][i] += request[i];
        state->need[process_id][i] -= request[i];
    }
    
    pthread_mutex_unlock(&state->banker_mutex);
    
    return OK_PTR(NULL);
}

// Statistics and monitoring
DeadlockPreventionStats deadlock_prevention_get_stats(DeadlockPreventionSystem* system) {
    DeadlockPreventionStats stats = {0};
    
    if (!system) return stats;
    
    pthread_mutex_lock(&system->system_mutex);
    
    if (system->resource_manager) {
        stats.total_resources = system->resource_manager->resource_count;
        stats.total_requests = system->resource_manager->total_requests;
        stats.successful_acquisitions = system->resource_manager->successful_allocations;
        stats.failed_acquisitions = system->resource_manager->failed_allocations;
        stats.timeouts = system->resource_manager->timeouts;
        stats.deadlocks_prevented = system->resource_manager->deadlocks_prevented;
    }
    
    stats.deadlocks_detected = system->deadlocks_detected;
    stats.false_positives = system->false_positives;
    stats.successful_optimizations = system->successful_optimizations;
    
    // Calculate derived metrics
    if (stats.total_requests > 0) {
        stats.deadlock_prevention_effectiveness = 
            (double)stats.deadlocks_prevented / stats.total_requests;
    }
    
    pthread_mutex_unlock(&system->system_mutex);
    
    return stats;
}

void deadlock_prevention_reset_stats(DeadlockPreventionSystem* system) {
    if (!system) return;
    
    pthread_mutex_lock(&system->system_mutex);
    
    if (system->resource_manager) {
        system->resource_manager->total_requests = 0;
        system->resource_manager->successful_allocations = 0;
        system->resource_manager->failed_allocations = 0;
        system->resource_manager->timeouts = 0;
        system->resource_manager->deadlocks_prevented = 0;
    }
    
    system->deadlocks_detected = 0;
    system->false_positives = 0;
    system->successful_optimizations = 0;
    
    pthread_mutex_unlock(&system->system_mutex);
}

// High-level system operations
DeadlockPreventionSystem* deadlock_prevention_create(DeadlockPreventionConfig config) {
    DeadlockPreventionSystem* system = calloc(1, sizeof(DeadlockPreventionSystem));
    if (!system) return NULL;
    
    system->config = config;
    system->is_active = false;
    atomic_init(&system->shutdown_requested, false);
    
    if (pthread_mutex_init(&system->system_mutex, NULL) != 0) {
        free(system);
        return NULL;
    }
    
    // Create resource manager
    system->resource_manager = resource_manager_create();
    if (!system->resource_manager) {
        pthread_mutex_destroy(&system->system_mutex);
        free(system);
        return NULL;
    }
    
    system->resource_manager->default_strategy = config.primary_strategy;
    system->resource_manager->default_timeout_ms = config.default_timeout_ms;
    
    // Create deadlock detector if enabled
    if (config.detection_method != DETECTION_NONE) {
        DeadlockDetectorConfig detector_config = {
            .method = config.detection_method,
            .detection_interval_ms = config.detection_interval_ms,
            .max_wait_time_ms = config.default_timeout_ms,
            .enable_cycle_detection = true,
            .enable_phantom_deadlock_filtering = true,
            .max_graph_depth = 100,
            .enable_preemptive_detection = true,
            .enable_resource_prediction = false,
            .max_detection_threads = 2,
            .actor_system = config.actor_system,
            .task_scope = config.task_scope,
            .shared_var_manager = config.shared_var_manager,
            .channel_broker = config.channel_broker
        };
        
        system->deadlock_detector = deadlock_detector_create(detector_config);
        // Note: Not checking for failure to keep creation simple
    }
    
    // Create lock ordering system if enabled
    if (config.enable_lock_ordering) {
        system->lock_ordering = lock_ordering_create();
        // Note: Not checking for failure to keep creation simple
    }
    
    // Create banker state if enabled
    if (config.enable_banker_algorithm) {
        // Create with reasonable defaults
        system->banker_state = banker_state_create(100, 10);  // 100 processes, 10 resource types
        // Note: Not checking for failure to keep creation simple
    }
    
    return system;
}

void deadlock_prevention_destroy(DeadlockPreventionSystem* system) {
    if (!system) return;
    
    // Shutdown system if still active
    if (system->is_active) {
        deadlock_prevention_shutdown(system, 5000);  // 5 second timeout
    }
    
    // Destroy components
    if (system->resource_manager) {
        resource_manager_destroy(system->resource_manager);
    }
    
    if (system->deadlock_detector) {
        deadlock_detector_destroy(system->deadlock_detector);
    }
    
    if (system->lock_ordering) {
        lock_ordering_destroy(system->lock_ordering);
    }
    
    if (system->timeout_manager) {
        timeout_manager_destroy(system->timeout_manager);
    }
    
    if (system->performance_optimizer) {
        performance_optimizer_destroy(system->performance_optimizer);
    }
    
    if (system->banker_state) {
        banker_state_destroy(system->banker_state);
    }
    
    pthread_mutex_destroy(&system->system_mutex);
    free(system);
}

Result_void_ptr deadlock_prevention_start(DeadlockPreventionSystem* system) {
    if (!system) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid deadlock prevention system"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&system->system_mutex);
    
    if (system->is_active) {
        pthread_mutex_unlock(&system->system_mutex);
        return OK_PTR(NULL);  // Already started
    }
    
    system->is_active = true;
    atomic_store(&system->shutdown_requested, false);
    
    // Start resource manager
    if (system->resource_manager) {
        Result_void_ptr result = resource_manager_start(system->resource_manager);
        if (result.is_error) {
            system->is_active = false;
            pthread_mutex_unlock(&system->system_mutex);
            return result;
        }
    }
    
    // Start deadlock detector
    if (system->deadlock_detector) {
        Result_void_ptr result = deadlock_detector_start(system->deadlock_detector);
        if (result.is_error) {
            system->is_active = false;
            pthread_mutex_unlock(&system->system_mutex);
            return result;
        }
    }
    
    // Start timeout manager
    if (system->timeout_manager) {
        Result_void_ptr result = timeout_manager_start(system->timeout_manager);
        if (result.is_error) {
            system->is_active = false;
            pthread_mutex_unlock(&system->system_mutex);
            return result;
        }
    }
    
    // Start performance optimizer
    if (system->performance_optimizer) {
        Result_void_ptr result = performance_optimizer_start(system->performance_optimizer);
        if (result.is_error) {
            system->is_active = false;
            pthread_mutex_unlock(&system->system_mutex);
            return result;
        }
    }
    
    pthread_mutex_unlock(&system->system_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr deadlock_prevention_shutdown(DeadlockPreventionSystem* system, uint64_t timeout_ms) {
    if (!system) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid deadlock prevention system"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&system->system_mutex);
    
    if (!system->is_active) {
        pthread_mutex_unlock(&system->system_mutex);
        return OK_PTR(NULL);  // Already shut down
    }
    
    atomic_store(&system->shutdown_requested, true);
    
    // Stop components
    if (system->performance_optimizer) {
        performance_optimizer_stop(system->performance_optimizer);
    }
    
    if (system->timeout_manager) {
        timeout_manager_stop(system->timeout_manager);
    }
    
    if (system->deadlock_detector) {
        deadlock_detector_stop(system->deadlock_detector);
    }
    
    if (system->resource_manager) {
        resource_manager_shutdown(system->resource_manager);
    }
    
    system->is_active = false;
    
    pthread_mutex_unlock(&system->system_mutex);
    
    return OK_PTR(NULL);
}

// Stub implementations for components not fully implemented
DeadlockDetector* deadlock_detector_create(DeadlockDetectorConfig config) {
    DeadlockDetector* detector = calloc(1, sizeof(DeadlockDetector));
    if (!detector) return NULL;
    
    detector->config = config;
    detector->is_active = false;
    atomic_init(&detector->shutdown_requested, false);
    
    if (pthread_mutex_init(&detector->detector_mutex, NULL) != 0) {
        free(detector);
        return NULL;
    }
    
    return detector;
}

void deadlock_detector_destroy(DeadlockDetector* detector) {
    if (!detector) return;
    
    if (detector->is_active) {
        deadlock_detector_stop(detector);
    }
    
    pthread_mutex_destroy(&detector->detector_mutex);
    free(detector);
}

Result_void_ptr deadlock_detector_start(DeadlockDetector* detector) {
    if (!detector) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid deadlock detector"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&detector->detector_mutex);
    detector->is_active = true;
    atomic_store(&detector->shutdown_requested, false);
    pthread_mutex_unlock(&detector->detector_mutex);
    
    return OK_PTR(NULL);
}

Result_void_ptr deadlock_detector_stop(DeadlockDetector* detector) {
    if (!detector) return OK_PTR(NULL);
    
    pthread_mutex_lock(&detector->detector_mutex);
    detector->is_active = false;
    atomic_store(&detector->shutdown_requested, true);
    pthread_mutex_unlock(&detector->detector_mutex);
    
    return OK_PTR(NULL);
}

LockOrderingSystem* lock_ordering_create(void) {
    LockOrderingSystem* system = calloc(1, sizeof(LockOrderingSystem));
    if (!system) return NULL;
    
    if (pthread_mutex_init(&system->ordering_mutex, NULL) != 0) {
        free(system);
        return NULL;
    }
    
    return system;
}

void lock_ordering_destroy(LockOrderingSystem* system) {
    if (!system) return;
    
    pthread_mutex_destroy(&system->ordering_mutex);
    free(system);
}

TimeoutManager* timeout_manager_create(void) {
    TimeoutManager* manager = calloc(1, sizeof(TimeoutManager));
    if (!manager) return NULL;
    
    if (pthread_mutex_init(&manager->timeout_mutex, NULL) != 0) {
        free(manager);
        return NULL;
    }
    
    if (pthread_cond_init(&manager->timeout_changed, NULL) != 0) {
        pthread_mutex_destroy(&manager->timeout_mutex);
        free(manager);
        return NULL;
    }
    
    return manager;
}

void timeout_manager_destroy(TimeoutManager* manager) {
    if (!manager) return;
    
    pthread_mutex_destroy(&manager->timeout_mutex);
    pthread_cond_destroy(&manager->timeout_changed);
    free(manager);
}

Result_void_ptr timeout_manager_start(TimeoutManager* manager) {
    if (!manager) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid timeout manager"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    manager->is_active = true;
    atomic_store(&manager->shutdown_requested, false);
    
    return OK_PTR(NULL);
}

Result_void_ptr timeout_manager_stop(TimeoutManager* manager) {
    if (!manager) return OK_PTR(NULL);
    
    manager->is_active = false;
    atomic_store(&manager->shutdown_requested, true);
    
    return OK_PTR(NULL);
}

PerformanceOptimizer* performance_optimizer_create(void) {
    PerformanceOptimizer* optimizer = calloc(1, sizeof(PerformanceOptimizer));
    if (!optimizer) return NULL;
    
    if (pthread_mutex_init(&optimizer->optimizer_mutex, NULL) != 0) {
        free(optimizer);
        return NULL;
    }
    
    return optimizer;
}

void performance_optimizer_destroy(PerformanceOptimizer* optimizer) {
    if (!optimizer) return;
    
    pthread_mutex_destroy(&optimizer->optimizer_mutex);
    free(optimizer);
}

Result_void_ptr performance_optimizer_start(PerformanceOptimizer* optimizer) {
    if (!optimizer) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid performance optimizer"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    optimizer->is_active = true;
    
    return OK_PTR(NULL);
}

Result_void_ptr performance_optimizer_stop(PerformanceOptimizer* optimizer) {
    if (!optimizer) return OK_PTR(NULL);
    
    optimizer->is_active = false;
    
    return OK_PTR(NULL);
}

Result_void_ptr resource_manager_start(ResourceManager* manager) {
    if (!manager) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid resource manager"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Resource manager is ready to go
    return OK_PTR(NULL);
}

Result_void_ptr resource_manager_shutdown(ResourceManager* manager) {
    if (!manager) return OK_PTR(NULL);
    
    // Resource manager doesn't need explicit shutdown
    return OK_PTR(NULL);
}

// ============================================================================
// WORK-STEALING SCHEDULER FOR OPTIMAL CPU UTILIZATION
// ============================================================================

// Work-stealing queue for lock-free task distribution
typedef struct WorkStealingQueue {
    void** tasks;
    atomic_size_t top;    // Only modified by owner
    atomic_size_t bottom; // Only modified by owner
    size_t capacity;
    
    pthread_mutex_t resize_mutex;
} WorkStealingQueue;

// NUMA-aware worker thread
typedef struct NumaWorkerThread {
    pthread_t thread_id;
    int numa_node;
    int cpu_affinity;
    
    WorkStealingQueue* local_queue;
    atomic_bool is_active;
    atomic_uint64_t tasks_executed;
    atomic_uint64_t tasks_stolen;
    atomic_uint64_t idle_time_ns;
    
    // Performance metrics
    uint64_t last_activity_time;
    double cpu_utilization;
    size_t cache_misses;
    
    struct NumaWorkerThread* next;
} NumaWorkerThread;

// Work-stealing scheduler
typedef struct WorkStealingScheduler {
    NumaWorkerThread** workers;
    size_t worker_count;
    size_t numa_node_count;
    
    // Global task queue for overflow
    WorkStealingQueue* global_queue;
    
    // Load balancing
    atomic_bool enable_work_stealing;
    atomic_size_t steal_attempts;
    atomic_size_t successful_steals;
    
    // NUMA topology
    struct {
        int node_id;
        size_t worker_count;
        NumaWorkerThread** workers;
        size_t memory_size_mb;
        double load_factor;
    } *numa_nodes;
    
    // Scheduler state
    bool is_active;
    atomic_bool shutdown_requested;
    
    // Performance monitoring
    pthread_t monitor_thread;
    uint64_t monitor_interval_ms;
    
    // Statistics
    atomic_uint64_t total_tasks_scheduled;
    atomic_uint64_t total_tasks_completed;
    atomic_uint64_t total_steal_attempts;
    atomic_uint64_t successful_steals;
    atomic_uint64_t load_imbalance_events;
    
    pthread_mutex_t scheduler_mutex;
} WorkStealingScheduler;

// Task for the work-stealing scheduler
typedef struct ScheduledTask {
    void (*task_function)(void* context);
    void* task_context;
    int priority;
    uint64_t task_id;
    uint64_t creation_time;
    
    // NUMA preferences
    int preferred_numa_node;
    bool numa_sensitive;
    
    // Dependencies
    atomic_int dependency_count;
    struct ScheduledTask** dependencies;
    size_t dependency_capacity;
    
    // Completion tracking
    atomic_bool is_completed;
    pthread_cond_t completion_cond;
    pthread_mutex_t completion_mutex;
} ScheduledTask;

// Work-stealing queue operations
static WorkStealingQueue* work_stealing_queue_create(size_t initial_capacity) {
    WorkStealingQueue* queue = calloc(1, sizeof(WorkStealingQueue));
    if (!queue) return NULL;
    
    queue->capacity = initial_capacity;
    queue->tasks = calloc(queue->capacity, sizeof(void*));
    if (!queue->tasks) {
        free(queue);
        return NULL;
    }
    
    atomic_store(&queue->top, 0);
    atomic_store(&queue->bottom, 0);
    
    if (pthread_mutex_init(&queue->resize_mutex, NULL) != 0) {
        free(queue->tasks);
        free(queue);
        return NULL;
    }
    
    return queue;
}

static void work_stealing_queue_destroy(WorkStealingQueue* queue) {
    if (!queue) return;
    
    free(queue->tasks);
    pthread_mutex_destroy(&queue->resize_mutex);
    free(queue);
}

// Push task to bottom (only owner thread)
static bool work_stealing_queue_push(WorkStealingQueue* queue, void* task) {
    if (!queue || !task) return false;
    
    size_t bottom = atomic_load(&queue->bottom);
    size_t top = atomic_load(&queue->top);
    
    if (bottom - top >= queue->capacity - 1) {
        // Queue is full, need to resize
        pthread_mutex_lock(&queue->resize_mutex);
        
        size_t new_capacity = queue->capacity * 2;
        void** new_tasks = realloc(queue->tasks, new_capacity * sizeof(void*));
        if (!new_tasks) {
            pthread_mutex_unlock(&queue->resize_mutex);
            return false;
        }
        
        queue->tasks = new_tasks;
        queue->capacity = new_capacity;
        
        pthread_mutex_unlock(&queue->resize_mutex);
    }
    
    queue->tasks[bottom % queue->capacity] = task;
    atomic_store(&queue->bottom, bottom + 1);
    
    return true;
}

// Pop task from bottom (only owner thread)
static void* work_stealing_queue_pop(WorkStealingQueue* queue) {
    if (!queue) return NULL;
    
    size_t bottom = atomic_load(&queue->bottom);
    if (bottom == 0) return NULL;
    
    bottom--;
    atomic_store(&queue->bottom, bottom);
    
    size_t top = atomic_load(&queue->top);
    if (top <= bottom) {
        void* task = queue->tasks[bottom % queue->capacity];
        
        if (top == bottom) {
            // Last element, need to compete with stealers
            if (atomic_compare_exchange_strong(&queue->top, &top, top + 1)) {
                return task;
            } else {
                atomic_store(&queue->bottom, bottom + 1);
                return NULL;
            }
        }
        
        return task;
    } else {
        // Queue is empty
        atomic_store(&queue->bottom, bottom + 1);
        return NULL;
    }
}

// Steal task from top (thief threads)
static void* work_stealing_queue_steal(WorkStealingQueue* queue) {
    if (!queue) return NULL;
    
    size_t top = atomic_load(&queue->top);
    size_t bottom = atomic_load(&queue->bottom);
    
    if (top >= bottom) {
        return NULL; // Queue is empty
    }
    
    void* task = queue->tasks[top % queue->capacity];
    
    if (atomic_compare_exchange_strong(&queue->top, &top, top + 1)) {
        return task;
    }
    
    return NULL; // Failed to steal
}

// NUMA topology detection
static int get_numa_node_count(void) {
#ifdef __linux__
    // On Linux, read from /proc/sys/kernel/numa_nodes
    FILE* f = fopen("/proc/sys/kernel/numa_nodes", "r");
    if (f) {
        int node_count;
        if (fscanf(f, "%d", &node_count) == 1) {
            fclose(f);
            return node_count;
        }
        fclose(f);
    }
#endif
    
    // Fallback: assume single NUMA node
    return 1;
}

static int get_current_numa_node(void) {
#ifdef __linux__
    // On Linux, this would use numa_node_of_cpu(sched_getcpu())
    // For now, return 0 as a simple fallback
#endif
    return 0;
}

// Set CPU affinity for NUMA awareness
static bool set_cpu_affinity(pthread_t thread, int cpu_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    return pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset) == 0;
#else
    // Not supported on this platform
    return true;
#endif
}

// Worker thread function
static void* work_stealing_worker_thread(void* arg) {
    NumaWorkerThread* worker = (NumaWorkerThread*)arg;
    
    // Set CPU affinity if specified
    if (worker->cpu_affinity >= 0) {
        set_cpu_affinity(pthread_self(), worker->cpu_affinity);
    }
    
    uint64_t idle_start = 0;
    
    while (atomic_load(&worker->is_active)) {
        void* task = NULL;
        
        // Try to get task from local queue first
        task = work_stealing_queue_pop(worker->local_queue);
        
        if (!task) {
            // No local task, try to steal from other workers
            // TODO: Implement stealing strategy based on NUMA topology
            if (idle_start == 0) {
                idle_start = get_monotonic_time_ns();
            }
            
            usleep(1000); // 1ms backoff
            continue;
        }
        
        // Update idle time tracking
        if (idle_start > 0) {
            uint64_t idle_duration = get_monotonic_time_ns() - idle_start;
            atomic_fetch_add(&worker->idle_time_ns, idle_duration);
            idle_start = 0;
        }
        
        // Execute task
        ScheduledTask* scheduled_task = (ScheduledTask*)task;
        if (scheduled_task && scheduled_task->task_function) {
            worker->last_activity_time = get_monotonic_time_ns();
            
            scheduled_task->task_function(scheduled_task->task_context);
            
            atomic_store(&scheduled_task->is_completed, true);
            atomic_fetch_add(&worker->tasks_executed, 1);
            
            // Signal completion
            pthread_mutex_lock(&scheduled_task->completion_mutex);
            pthread_cond_broadcast(&scheduled_task->completion_cond);
            pthread_mutex_unlock(&scheduled_task->completion_mutex);
        }
    }
    
    return NULL;
}

// Create work-stealing scheduler
static WorkStealingScheduler* work_stealing_scheduler_create(size_t worker_count) {
    WorkStealingScheduler* scheduler = calloc(1, sizeof(WorkStealingScheduler));
    if (!scheduler) return NULL;
    
    scheduler->worker_count = worker_count;
    scheduler->numa_node_count = get_numa_node_count();
    
    // Create workers array
    scheduler->workers = calloc(worker_count, sizeof(NumaWorkerThread*));
    if (!scheduler->workers) {
        free(scheduler);
        return NULL;
    }
    
    // Create global overflow queue
    scheduler->global_queue = work_stealing_queue_create(1000);
    if (!scheduler->global_queue) {
        free(scheduler->workers);
        free(scheduler);
        return NULL;
    }
    
    // Initialize NUMA nodes
    scheduler->numa_nodes = calloc(scheduler->numa_node_count, sizeof(*scheduler->numa_nodes));
    if (!scheduler->numa_nodes) {
        work_stealing_queue_destroy(scheduler->global_queue);
        free(scheduler->workers);
        free(scheduler);
        return NULL;
    }
    
    for (size_t i = 0; i < scheduler->numa_node_count; i++) {
        scheduler->numa_nodes[i].node_id = i;
        scheduler->numa_nodes[i].worker_count = 0;
        scheduler->numa_nodes[i].workers = calloc(worker_count, sizeof(NumaWorkerThread*));
        scheduler->numa_nodes[i].memory_size_mb = 1024; // Default 1GB
        scheduler->numa_nodes[i].load_factor = 0.0;
    }
    
    // Create worker threads
    for (size_t i = 0; i < worker_count; i++) {
        NumaWorkerThread* worker = calloc(1, sizeof(NumaWorkerThread));
        if (!worker) continue;
        
        worker->numa_node = i % scheduler->numa_node_count;
        worker->cpu_affinity = i; // Simple CPU assignment
        worker->local_queue = work_stealing_queue_create(256);
        
        if (!worker->local_queue) {
            free(worker);
            continue;
        }
        
        atomic_store(&worker->is_active, true);
        atomic_store(&worker->tasks_executed, 0);
        atomic_store(&worker->tasks_stolen, 0);
        atomic_store(&worker->idle_time_ns, 0);
        
        if (pthread_create(&worker->thread_id, NULL, work_stealing_worker_thread, worker) != 0) {
            work_stealing_queue_destroy(worker->local_queue);
            free(worker);
            continue;
        }
        
        scheduler->workers[i] = worker;
        
        // Add to NUMA node
        int numa_node = worker->numa_node;
        scheduler->numa_nodes[numa_node].workers[scheduler->numa_nodes[numa_node].worker_count++] = worker;
    }
    
    atomic_store(&scheduler->enable_work_stealing, true);
    scheduler->monitor_interval_ms = 1000; // 1 second
    
    if (pthread_mutex_init(&scheduler->scheduler_mutex, NULL) != 0) {
        // Cleanup would be needed here
        return scheduler; // Return partial state for now
    }
    
    return scheduler;
}

// Schedule a task
static Result_void_ptr work_stealing_scheduler_schedule(WorkStealingScheduler* scheduler, ScheduledTask* task) {
    if (!scheduler || !task) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid scheduler or task"));
    }
    
    // Choose worker based on NUMA preference and load balancing
    size_t target_worker = 0;
    
    if (task->numa_sensitive && task->preferred_numa_node >= 0 && 
        task->preferred_numa_node < scheduler->numa_node_count) {
        
        // Find least loaded worker in preferred NUMA node
        int numa_node = task->preferred_numa_node;
        size_t min_queue_size = SIZE_MAX;
        
        for (size_t i = 0; i < scheduler->numa_nodes[numa_node].worker_count; i++) {
            NumaWorkerThread* worker = scheduler->numa_nodes[numa_node].workers[i];
            if (worker) {
                size_t queue_size = atomic_load(&worker->local_queue->bottom) - 
                                   atomic_load(&worker->local_queue->top);
                if (queue_size < min_queue_size) {
                    min_queue_size = queue_size;
                    target_worker = worker - scheduler->workers[0]; // Get index
                }
            }
        }
    } else {
        // Round-robin assignment
        static atomic_size_t round_robin_counter = ATOMIC_VAR_INIT(0);
        target_worker = atomic_fetch_add(&round_robin_counter, 1) % scheduler->worker_count;
    }
    
    // Schedule task
    NumaWorkerThread* worker = scheduler->workers[target_worker];
    if (worker && work_stealing_queue_push(worker->local_queue, task)) {
        atomic_fetch_add(&scheduler->total_tasks_scheduled, 1);
        return OK_PTR(NULL);
    } else {
        // Fallback to global queue
        if (work_stealing_queue_push(scheduler->global_queue, task)) {
            atomic_fetch_add(&scheduler->total_tasks_scheduled, 1);
            return OK_PTR(NULL);
        }
    }
    
    return ERR_PTR(error_create(ERROR_RESOURCE_EXHAUSTED, "Failed to schedule task"));
}

// ============================================================================
// ADAPTIVE THREAD POOL SIZING
// ============================================================================

// Thread pool with automatic sizing
typedef struct AdaptiveThreadPool {
    // Core configuration
    size_t min_threads;
    size_t max_threads;
    size_t current_threads;
    
    // Worker threads
    pthread_t* threads;
    atomic_bool* thread_active;
    
    // Task queue
    void** task_queue;
    size_t queue_capacity;
    atomic_size_t queue_head;
    atomic_size_t queue_tail;
    atomic_size_t queue_size;
    
    // Synchronization
    pthread_mutex_t pool_mutex;
    pthread_cond_t task_available;
    pthread_cond_t task_completed;
    
    // Load monitoring
    atomic_uint64_t tasks_queued;
    atomic_uint64_t tasks_completed;
    atomic_uint64_t total_execution_time_ns;
    
    // Adaptive sizing
    uint64_t last_resize_time;
    double cpu_utilization_threshold;
    double queue_length_threshold;
    uint64_t resize_interval_ms;
    
    // Performance metrics
    struct {
        uint64_t timestamp;
        size_t thread_count;
        size_t queue_length;
        double cpu_utilization;
        double throughput;
    } performance_history[60]; // Last 60 measurements
    size_t history_index;
    
    bool is_active;
    atomic_bool shutdown_requested;
} AdaptiveThreadPool;

// Create adaptive thread pool
static AdaptiveThreadPool* adaptive_thread_pool_create(size_t min_threads, size_t max_threads) {
    AdaptiveThreadPool* pool = calloc(1, sizeof(AdaptiveThreadPool));
    if (!pool) return NULL;
    
    pool->min_threads = min_threads;
    pool->max_threads = max_threads;
    pool->current_threads = min_threads;
    
    pool->queue_capacity = 1000;
    pool->task_queue = calloc(pool->queue_capacity, sizeof(void*));
    if (!pool->task_queue) {
        free(pool);
        return NULL;
    }
    
    pool->threads = calloc(max_threads, sizeof(pthread_t));
    pool->thread_active = calloc(max_threads, sizeof(atomic_bool));
    
    if (!pool->threads || !pool->thread_active) {
        free(pool->task_queue);
        free(pool->threads);
        free(pool->thread_active);
        free(pool);
        return NULL;
    }
    
    // Initialize synchronization
    if (pthread_mutex_init(&pool->pool_mutex, NULL) != 0 ||
        pthread_cond_init(&pool->task_available, NULL) != 0 ||
        pthread_cond_init(&pool->task_completed, NULL) != 0) {
        free(pool->task_queue);
        free(pool->threads);
        free(pool->thread_active);
        free(pool);
        return NULL;
    }
    
    // Initialize thresholds
    pool->cpu_utilization_threshold = 0.8;  // 80%
    pool->queue_length_threshold = 10.0;    // 10 tasks per thread
    pool->resize_interval_ms = 5000;        // 5 seconds
    
    atomic_store(&pool->queue_head, 0);
    atomic_store(&pool->queue_tail, 0);
    atomic_store(&pool->queue_size, 0);
    
    return pool;
}

// Monitor performance and adjust thread count
static void adaptive_thread_pool_resize(AdaptiveThreadPool* pool) {
    if (!pool) return;
    
    uint64_t current_time = get_monotonic_time_ns();
    if (current_time - pool->last_resize_time < pool->resize_interval_ms * 1000000) {
        return; // Too soon to resize
    }
    
    pthread_mutex_lock(&pool->pool_mutex);
    
    // Calculate current metrics
    size_t queue_length = atomic_load(&pool->queue_size);
    double queue_per_thread = (double)queue_length / pool->current_threads;
    
    // Simple heuristic for thread adjustment
    bool should_increase = false;
    bool should_decrease = false;
    
    if (queue_per_thread > pool->queue_length_threshold && 
        pool->current_threads < pool->max_threads) {
        should_increase = true;
    } else if (queue_per_thread < 1.0 && queue_length == 0 && 
               pool->current_threads > pool->min_threads) {
        should_decrease = true;
    }
    
    if (should_increase) {
        // Add one thread
        size_t new_thread_idx = pool->current_threads;
        atomic_store(&pool->thread_active[new_thread_idx], true);
        
        // Create worker thread (simplified)
        pool->current_threads++;
        
    } else if (should_decrease) {
        // Remove one thread
        size_t thread_to_remove = pool->current_threads - 1;
        atomic_store(&pool->thread_active[thread_to_remove], false);
        pool->current_threads--;
    }
    
    pool->last_resize_time = current_time;
    
    pthread_mutex_unlock(&pool->pool_mutex);
}

// ============================================================================
// LOCK-FREE DATA STRUCTURES
// ============================================================================

// Lock-free stack using CAS
typedef struct LockFreeStack {
    atomic_uintptr_t head;
    atomic_uint64_t size;
} LockFreeStack;

typedef struct LockFreeStackNode {
    void* data;
    struct LockFreeStackNode* next;
} LockFreeStackNode;

// Create lock-free stack
static LockFreeStack* lock_free_stack_create(void) {
    LockFreeStack* stack = calloc(1, sizeof(LockFreeStack));
    if (!stack) return NULL;
    
    atomic_store(&stack->head, 0);
    atomic_store(&stack->size, 0);
    
    return stack;
}

// Push to lock-free stack
static bool lock_free_stack_push(LockFreeStack* stack, void* data) {
    if (!stack || !data) return false;
    
    LockFreeStackNode* new_node = malloc(sizeof(LockFreeStackNode));
    if (!new_node) return false;
    
    new_node->data = data;
    
    uintptr_t old_head;
    do {
        old_head = atomic_load(&stack->head);
        new_node->next = (LockFreeStackNode*)old_head;
    } while (!atomic_compare_exchange_weak(&stack->head, &old_head, (uintptr_t)new_node));
    
    atomic_fetch_add(&stack->size, 1);
    return true;
}

// Pop from lock-free stack
static void* lock_free_stack_pop(LockFreeStack* stack) {
    if (!stack) return NULL;
    
    uintptr_t old_head;
    LockFreeStackNode* new_head;
    
    do {
        old_head = atomic_load(&stack->head);
        if (old_head == 0) return NULL; // Stack is empty
        
        new_head = ((LockFreeStackNode*)old_head)->next;
    } while (!atomic_compare_exchange_weak(&stack->head, &old_head, (uintptr_t)new_head));
    
    LockFreeStackNode* old_node = (LockFreeStackNode*)old_head;
    void* data = old_node->data;
    free(old_node);
    
    atomic_fetch_sub(&stack->size, 1);
    return data;
}

// Destroy lock-free stack
static void lock_free_stack_destroy(LockFreeStack* stack) {
    if (!stack) return;
    
    // Pop all remaining elements
    while (lock_free_stack_pop(stack) != NULL) {
        // Continue until empty
    }
    
    free(stack);
}