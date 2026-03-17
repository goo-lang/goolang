#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "shared_variables.h"
#include "error_hierarchies.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sched.h>

// =============================================================================
// Global State and Utilities
// =============================================================================

// Global shared variable system
static SharedVarSystem* g_shared_var_system = NULL;
static pthread_mutex_t g_system_mutex = PTHREAD_MUTEX_INITIALIZER;

// Get current timestamp in microseconds
static uint64_t get_current_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

// Generate unique ID with atomics
static uint64_t generate_var_id(SharedVarRegistry* registry) {
    return atomic_fetch_add(&registry->next_var_id, 1);
}

static uint64_t generate_watcher_id(SharedVarRegistry* registry) {
    return atomic_fetch_add(&registry->next_watcher_id, 1);
}

static uint64_t generate_event_id(SharedVarRegistry* registry) {
    return atomic_fetch_add(&registry->next_event_id, 1);
}

// Safe string duplication
static char* safe_strdup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* copy = malloc(len + 1);
    if (copy) {
        memcpy(copy, str, len + 1);
    }
    return copy;
}

// =============================================================================
// Shared Variable Registry Implementation
// =============================================================================

static SharedVarRegistry* shared_var_registry_create(void) {
    SharedVarRegistry* registry = calloc(1, sizeof(SharedVarRegistry));
    if (!registry) return NULL;
    
    registry->var_capacity = 16;
    registry->variables = calloc(registry->var_capacity, sizeof(SharedVar*));
    if (!registry->variables) {
        free(registry);
        return NULL;
    }
    
    if (pthread_rwlock_init(&registry->registry_lock, NULL) != 0) {
        free(registry->variables);
        free(registry);
        return NULL;
    }
    
    // Initialize atomic counters
    atomic_init(&registry->next_var_id, 1);
    atomic_init(&registry->next_watcher_id, 1);
    atomic_init(&registry->next_event_id, 1);
    
    return registry;
}

static void shared_var_registry_destroy(SharedVarRegistry* registry) {
    if (!registry) return;
    
    pthread_rwlock_wrlock(&registry->registry_lock);
    
    // Clean up all variables
    for (int i = 0; i < registry->var_count; i++) {
        if (registry->variables[i]) {
            shared_var_destroy(registry->variables[i]);
        }
    }
    
    pthread_rwlock_unlock(&registry->registry_lock);
    pthread_rwlock_destroy(&registry->registry_lock);
    
    free(registry->variables);
    free(registry);
}

static bool shared_var_registry_add(SharedVarRegistry* registry, SharedVar* var) {
    if (!registry || !var) return false;
    
    pthread_rwlock_wrlock(&registry->registry_lock);
    
    // Resize if needed
    if (registry->var_count >= registry->var_capacity) {
        int new_capacity = registry->var_capacity * 2;
        SharedVar** new_vars = realloc(registry->variables, 
                                      new_capacity * sizeof(SharedVar*));
        if (!new_vars) {
            pthread_rwlock_unlock(&registry->registry_lock);
            return false;
        }
        registry->variables = new_vars;
        registry->var_capacity = new_capacity;
    }
    
    registry->variables[registry->var_count++] = var;
    registry->global_stats.variables_created++;
    
    pthread_rwlock_unlock(&registry->registry_lock);
    return true;
}

// =============================================================================
// Change Event System Implementation
// =============================================================================

static ChangeEvent* change_event_create(SharedVar* var, ChangeEventType type) {
    if (!var) return NULL;
    
    ChangeEvent* event = calloc(1, sizeof(ChangeEvent));
    if (!event) return NULL;
    
    SharedVarSystem* system = get_global_shared_var_system();
    if (system && system->registry) {
        event->event_id = generate_event_id(system->registry);
    }
    
    event->variable = var;
    event->type = type;
    event->timestamp = get_current_time_us();
    event->thread_id = (uint64_t)pthread_self();
    
    return event;
}

static void change_event_destroy(ChangeEvent* event) {
    if (!event) return;
    
    free(event->old_value);
    free(event->new_value);
    free(event);
}

static void shared_var_notify_watchers_internal(SharedVar* var, ChangeEvent* event) {
    if (!var || !event) return;
    
    pthread_mutex_lock(&var->watcher_mutex);
    
    for (int i = 0; i < var->watcher_count; i++) {
        SharedVarWatcher* watcher = var->watchers[i];
        if (watcher && watcher->is_active && 
            (watcher->event_mask & event->type)) {
            watcher->callback(event, watcher->context);
        }
    }
    
    pthread_mutex_unlock(&var->watcher_mutex);
}

// =============================================================================
// Shared Variable Core Implementation
// =============================================================================

SharedVar* shared_var_create(const char* name, SharedVarType type, const void* initial_value) {
    SharedVarConfig config = {
        .name = name,
        .type = type,
        .size = 0,
        .default_order = MEMORY_ORDER_SEQ_CST,
        .enable_watching = true,
        .enable_statistics = true,
        .cache_line_padding = 64
    };
    
    return shared_var_create_with_config(&config, initial_value);
}

SharedVar* shared_var_create_with_config(const SharedVarConfig* config, const void* initial_value) {
    if (!config) return NULL;
    
    SharedVar* var = calloc(1, sizeof(SharedVar));
    if (!var) return NULL;
    
    SharedVarSystem* system = get_global_shared_var_system();
    if (system && system->registry) {
        var->var_id = generate_var_id(system->registry);
    }
    
    var->name = safe_strdup(config->name);
    var->type = config->type;
    var->config = *config;
    
    // Initialize storage based on type
    switch (var->type) {
        case SHARED_TYPE_INT8:
            atomic_init(&var->storage.int8_val, initial_value ? *(int8_t*)initial_value : 0);
            break;
        case SHARED_TYPE_INT16:
            atomic_init(&var->storage.int16_val, initial_value ? *(int16_t*)initial_value : 0);
            break;
        case SHARED_TYPE_INT32:
            atomic_init(&var->storage.int32_val, initial_value ? *(int32_t*)initial_value : 0);
            break;
        case SHARED_TYPE_INT64:
            atomic_init(&var->storage.int64_val, initial_value ? *(int64_t*)initial_value : 0);
            break;
        case SHARED_TYPE_UINT8:
            atomic_init(&var->storage.uint8_val, initial_value ? *(uint8_t*)initial_value : 0);
            break;
        case SHARED_TYPE_UINT16:
            atomic_init(&var->storage.uint16_val, initial_value ? *(uint16_t*)initial_value : 0);
            break;
        case SHARED_TYPE_UINT32:
            atomic_init(&var->storage.uint32_val, initial_value ? *(uint32_t*)initial_value : 0);
            break;
        case SHARED_TYPE_UINT64:
            atomic_init(&var->storage.uint64_val, initial_value ? *(uint64_t*)initial_value : 0);
            break;
        case SHARED_TYPE_BOOL:
            atomic_init(&var->storage.bool_val, initial_value ? *(bool*)initial_value : false);
            break;
        case SHARED_TYPE_POINTER:
            atomic_init(&var->storage.ptr_val, initial_value ? (uintptr_t)*(void**)initial_value : 0);
            break;
        case SHARED_TYPE_CUSTOM:
            if (initial_value && config->size > 0) {
                var->storage.custom.data = malloc(config->size);
                if (var->storage.custom.data) {
                    memcpy(var->storage.custom.data, initial_value, config->size);
                    var->storage.custom.size = config->size;
                }
            }
            if (pthread_rwlock_init(&var->storage.custom.rwlock, NULL) != 0) {
                free(var->storage.custom.data);
                free((void*)var->name);
                free(var);
                return NULL;
            }
            break;
        default:
            free((void*)var->name);
            free(var);
            return NULL;
    }
    
    // Initialize watcher system
    if (config->enable_watching) {
        var->watcher_capacity = 4;
        var->watchers = calloc(var->watcher_capacity, sizeof(SharedVarWatcher*));
        if (!var->watchers) {
            shared_var_destroy(var);
            return NULL;
        }
        
        if (pthread_mutex_init(&var->watcher_mutex, NULL) != 0) {
            shared_var_destroy(var);
            return NULL;
        }
    }
    
    // Initialize statistics
    if (config->enable_statistics) {
        if (pthread_mutex_init(&var->stats_mutex, NULL) != 0) {
            shared_var_destroy(var);
            return NULL;
        }
    }
    
    // Create variable arena
    var->var_arena = goo_arena_new(4096, var->name ? var->name : "shared_var");
    
    // Register with system
    if (system && system->registry) {
        shared_var_registry_add(system->registry, var);
    }
    
    return var;
}

void shared_var_destroy(SharedVar* var) {
    if (!var) return;
    
    // Notify watchers of destruction
    if (var->config.enable_watching && var->watchers) {
        ChangeEvent* event = change_event_create(var, CHANGE_EVENT_DESTROYED);
        if (event) {
            shared_var_notify_watchers_internal(var, event);
            change_event_destroy(event);
        }
        
        // Clean up watchers
        pthread_mutex_lock(&var->watcher_mutex);
        for (int i = 0; i < var->watcher_count; i++) {
            free(var->watchers[i]);
        }
        free(var->watchers);
        pthread_mutex_unlock(&var->watcher_mutex);
        pthread_mutex_destroy(&var->watcher_mutex);
    }
    
    // Clean up custom type storage
    if (var->type == SHARED_TYPE_CUSTOM) {
        pthread_rwlock_destroy(&var->storage.custom.rwlock);
        free(var->storage.custom.data);
    }
    
    // Clean up statistics
    if (var->config.enable_statistics) {
        pthread_mutex_destroy(&var->stats_mutex);
    }
    
    // Clean up arena
    goo_arena_free(var->var_arena);
    
    free((void*)var->name);
    free(var);
}

// =============================================================================
// Basic Operations Implementation
// =============================================================================

void* shared_var_read(SharedVar* var, void* dest, MemoryOrder order) {
    if (!var || !dest) return NULL;
    
    // Update statistics
    if (var->config.enable_statistics) {
        pthread_mutex_lock(&var->stats_mutex);
        var->stats.reads++;
        pthread_mutex_unlock(&var->stats_mutex);
    }
    
    switch (var->type) {
        case SHARED_TYPE_INT8:
            *(int8_t*)dest = atomic_load_explicit(&var->storage.int8_val, order);
            break;
        case SHARED_TYPE_INT16:
            *(int16_t*)dest = atomic_load_explicit(&var->storage.int16_val, order);
            break;
        case SHARED_TYPE_INT32:
            *(int32_t*)dest = atomic_load_explicit(&var->storage.int32_val, order);
            break;
        case SHARED_TYPE_INT64:
            *(int64_t*)dest = atomic_load_explicit(&var->storage.int64_val, order);
            break;
        case SHARED_TYPE_UINT8:
            *(uint8_t*)dest = atomic_load_explicit(&var->storage.uint8_val, order);
            break;
        case SHARED_TYPE_UINT16:
            *(uint16_t*)dest = atomic_load_explicit(&var->storage.uint16_val, order);
            break;
        case SHARED_TYPE_UINT32:
            *(uint32_t*)dest = atomic_load_explicit(&var->storage.uint32_val, order);
            break;
        case SHARED_TYPE_UINT64:
            *(uint64_t*)dest = atomic_load_explicit(&var->storage.uint64_val, order);
            break;
        case SHARED_TYPE_BOOL:
            *(bool*)dest = atomic_load_explicit(&var->storage.bool_val, order);
            break;
        case SHARED_TYPE_POINTER:
            *(void**)dest = (void*)atomic_load_explicit(&var->storage.ptr_val, order);
            break;
        case SHARED_TYPE_CUSTOM:
            if (var->custom_ops.read_func) {
                return var->custom_ops.read_func(var, dest, order);
            } else {
                pthread_rwlock_rdlock(&var->storage.custom.rwlock);
                if (var->storage.custom.data && var->storage.custom.size > 0) {
                    memcpy(dest, var->storage.custom.data, var->storage.custom.size);
                }
                pthread_rwlock_unlock(&var->storage.custom.rwlock);
            }
            break;
        default:
            return NULL;
    }
    
    return dest;
}

bool shared_var_write(SharedVar* var, const void* src, MemoryOrder order) {
    if (!var || !src) return false;
    
    void* old_value = NULL;
    
    // Capture old value for notifications
    if (var->config.enable_watching && var->watchers) {
        switch (var->type) {
            case SHARED_TYPE_INT64:
                old_value = malloc(sizeof(int64_t));
                if (old_value) {
                    *(int64_t*)old_value = atomic_load(&var->storage.int64_val);
                }
                break;
            case SHARED_TYPE_UINT64:
                old_value = malloc(sizeof(uint64_t));
                if (old_value) {
                    *(uint64_t*)old_value = atomic_load(&var->storage.uint64_val);
                }
                break;
            case SHARED_TYPE_BOOL:
                old_value = malloc(sizeof(bool));
                if (old_value) {
                    *(bool*)old_value = atomic_load(&var->storage.bool_val);
                }
                break;
            default:
                break;
        }
    }
    
    // Perform the write
    switch (var->type) {
        case SHARED_TYPE_INT8:
            atomic_store_explicit(&var->storage.int8_val, *(int8_t*)src, order);
            break;
        case SHARED_TYPE_INT16:
            atomic_store_explicit(&var->storage.int16_val, *(int16_t*)src, order);
            break;
        case SHARED_TYPE_INT32:
            atomic_store_explicit(&var->storage.int32_val, *(int32_t*)src, order);
            break;
        case SHARED_TYPE_INT64:
            atomic_store_explicit(&var->storage.int64_val, *(int64_t*)src, order);
            break;
        case SHARED_TYPE_UINT8:
            atomic_store_explicit(&var->storage.uint8_val, *(uint8_t*)src, order);
            break;
        case SHARED_TYPE_UINT16:
            atomic_store_explicit(&var->storage.uint16_val, *(uint16_t*)src, order);
            break;
        case SHARED_TYPE_UINT32:
            atomic_store_explicit(&var->storage.uint32_val, *(uint32_t*)src, order);
            break;
        case SHARED_TYPE_UINT64:
            atomic_store_explicit(&var->storage.uint64_val, *(uint64_t*)src, order);
            break;
        case SHARED_TYPE_BOOL:
            atomic_store_explicit(&var->storage.bool_val, *(bool*)src, order);
            break;
        case SHARED_TYPE_POINTER:
            atomic_store_explicit(&var->storage.ptr_val, (uintptr_t)*(void**)src, order);
            break;
        case SHARED_TYPE_CUSTOM:
            if (var->custom_ops.write_func) {
                return var->custom_ops.write_func(var, src, order);
            } else {
                pthread_rwlock_wrlock(&var->storage.custom.rwlock);
                if (var->storage.custom.data && var->storage.custom.size > 0) {
                    memcpy(var->storage.custom.data, src, var->storage.custom.size);
                }
                pthread_rwlock_unlock(&var->storage.custom.rwlock);
            }
            break;
        default:
            free(old_value);
            return false;
    }
    
    // Update statistics
    if (var->config.enable_statistics) {
        pthread_mutex_lock(&var->stats_mutex);
        var->stats.writes++;
        var->stats.last_modified = get_current_time_us();
        pthread_mutex_unlock(&var->stats_mutex);
    }
    
    // Notify watchers
    if (var->config.enable_watching && var->watchers) {
        ChangeEvent* event = change_event_create(var, CHANGE_EVENT_WRITE);
        if (event) {
            event->old_value = old_value;
            
            // Copy new value
            switch (var->type) {
                case SHARED_TYPE_INT64:
                    event->new_value = malloc(sizeof(int64_t));
                    if (event->new_value) {
                        *(int64_t*)event->new_value = *(int64_t*)src;
                        event->value_size = sizeof(int64_t);
                    }
                    break;
                default:
                    break;
            }
            
            event->memory_order = order;
            shared_var_notify_watchers_internal(var, event);
            change_event_destroy(event);
        }
    } else {
        free(old_value);
    }
    
    return true;
}

bool shared_var_compare_swap(SharedVar* var, void* expected, const void* desired, MemoryOrder order) {
    if (!var || !expected || !desired) return false;
    
    bool result = false;
    
    switch (var->type) {
        case SHARED_TYPE_INT64:
            result = atomic_compare_exchange_strong_explicit(
                &var->storage.int64_val, (int64_t*)expected, *(int64_t*)desired,
                order, order);
            break;
        case SHARED_TYPE_UINT64:
            result = atomic_compare_exchange_strong_explicit(
                &var->storage.uint64_val, (uint64_t*)expected, *(uint64_t*)desired,
                order, order);
            break;
        case SHARED_TYPE_BOOL:
            result = atomic_compare_exchange_strong_explicit(
                &var->storage.bool_val, (bool*)expected, *(bool*)desired,
                order, order);
            break;
        case SHARED_TYPE_POINTER:
            result = atomic_compare_exchange_strong_explicit(
                &var->storage.ptr_val, (uintptr_t*)expected, (uintptr_t)*(void**)desired,
                order, order);
            break;
        case SHARED_TYPE_CUSTOM:
            if (var->custom_ops.compare_swap_func) {
                result = var->custom_ops.compare_swap_func(var, expected, desired, order);
            }
            break;
        default:
            return false;
    }
    
    // Update statistics and notify watchers if successful
    if (result) {
        if (var->config.enable_statistics) {
            pthread_mutex_lock(&var->stats_mutex);
            var->stats.atomic_ops++;
            pthread_mutex_unlock(&var->stats_mutex);
        }
        
        if (var->config.enable_watching && var->watchers) {
            ChangeEvent* event = change_event_create(var, CHANGE_EVENT_ATOMIC_OP);
            if (event) {
                event->atomic_op = ATOMIC_OP_COMPARE_SWAP;
                event->memory_order = order;
                shared_var_notify_watchers_internal(var, event);
                change_event_destroy(event);
            }
        }
    }
    
    return result;
}

// =============================================================================
// Atomic Arithmetic Operations
// =============================================================================

int64_t shared_var_atomic_add_int(SharedVar* var, int64_t value, MemoryOrder order) {
    if (!var || var->type != SHARED_TYPE_INT64) return 0;
    
    int64_t result = atomic_fetch_add_explicit(&var->storage.int64_val, value, order);
    
    if (var->config.enable_statistics) {
        pthread_mutex_lock(&var->stats_mutex);
        var->stats.atomic_ops++;
        pthread_mutex_unlock(&var->stats_mutex);
    }
    
    return result;
}

int64_t shared_var_atomic_sub_int(SharedVar* var, int64_t value, MemoryOrder order) {
    if (!var || var->type != SHARED_TYPE_INT64) return 0;
    
    int64_t result = atomic_fetch_sub_explicit(&var->storage.int64_val, value, order);
    
    if (var->config.enable_statistics) {
        pthread_mutex_lock(&var->stats_mutex);
        var->stats.atomic_ops++;
        pthread_mutex_unlock(&var->stats_mutex);
    }
    
    return result;
}

int64_t shared_var_atomic_inc_int(SharedVar* var, MemoryOrder order) {
    return shared_var_atomic_add_int(var, 1, order);
}

int64_t shared_var_atomic_dec_int(SharedVar* var, MemoryOrder order) {
    return shared_var_atomic_sub_int(var, 1, order);
}

// =============================================================================
// Atomic Bitwise Operations
// =============================================================================

uint64_t shared_var_atomic_and_uint(SharedVar* var, uint64_t value, MemoryOrder order) {
    if (!var || var->type != SHARED_TYPE_UINT64) return 0;
    
    uint64_t result = atomic_fetch_and_explicit(&var->storage.uint64_val, value, order);
    
    if (var->config.enable_statistics) {
        pthread_mutex_lock(&var->stats_mutex);
        var->stats.atomic_ops++;
        pthread_mutex_unlock(&var->stats_mutex);
    }
    
    return result;
}

uint64_t shared_var_atomic_or_uint(SharedVar* var, uint64_t value, MemoryOrder order) {
    if (!var || var->type != SHARED_TYPE_UINT64) return 0;
    
    uint64_t result = atomic_fetch_or_explicit(&var->storage.uint64_val, value, order);
    
    if (var->config.enable_statistics) {
        pthread_mutex_lock(&var->stats_mutex);
        var->stats.atomic_ops++;
        pthread_mutex_unlock(&var->stats_mutex);
    }
    
    return result;
}

uint64_t shared_var_atomic_xor_uint(SharedVar* var, uint64_t value, MemoryOrder order) {
    if (!var || var->type != SHARED_TYPE_UINT64) return 0;
    
    uint64_t result = atomic_fetch_xor_explicit(&var->storage.uint64_val, value, order);
    
    if (var->config.enable_statistics) {
        pthread_mutex_lock(&var->stats_mutex);
        var->stats.atomic_ops++;
        pthread_mutex_unlock(&var->stats_mutex);
    }
    
    return result;
}

// =============================================================================
// Watcher Management Implementation
// =============================================================================

SharedVarWatcher* shared_var_add_watcher(SharedVar* var, SharedVarWatcherCallback callback, 
                                        void* context, ChangeEventType event_mask) {
    if (!var || !callback || !var->config.enable_watching) return NULL;
    
    SharedVarWatcher* watcher = calloc(1, sizeof(SharedVarWatcher));
    if (!watcher) return NULL;
    
    SharedVarSystem* system = get_global_shared_var_system();
    if (system && system->registry) {
        watcher->watcher_id = generate_watcher_id(system->registry);
    }
    
    watcher->callback = callback;
    watcher->context = context;
    watcher->event_mask = event_mask;
    watcher->is_active = true;
    
    pthread_mutex_lock(&var->watcher_mutex);
    
    // Resize watcher array if needed
    if (var->watcher_count >= var->watcher_capacity) {
        int new_capacity = var->watcher_capacity * 2;
        SharedVarWatcher** new_watchers = realloc(var->watchers, 
                                                 new_capacity * sizeof(SharedVarWatcher*));
        if (!new_watchers) {
            pthread_mutex_unlock(&var->watcher_mutex);
            free(watcher);
            return NULL;
        }
        var->watchers = new_watchers;
        var->watcher_capacity = new_capacity;
    }
    
    var->watchers[var->watcher_count++] = watcher;
    
    pthread_mutex_unlock(&var->watcher_mutex);
    
    // Update registry statistics
    if (system && system->registry) {
        system->registry->global_stats.watchers_registered++;
    }
    
    return watcher;
}

void shared_var_remove_watcher(SharedVar* var, SharedVarWatcher* watcher) {
    if (!var || !watcher || !var->config.enable_watching) return;
    
    pthread_mutex_lock(&var->watcher_mutex);
    
    // Find and remove watcher
    for (int i = 0; i < var->watcher_count; i++) {
        if (var->watchers[i] == watcher) {
            // Shift remaining watchers
            for (int j = i; j < var->watcher_count - 1; j++) {
                var->watchers[j] = var->watchers[j + 1];
            }
            var->watcher_count--;
            break;
        }
    }
    
    pthread_mutex_unlock(&var->watcher_mutex);
    
    free(watcher);
}

// =============================================================================
// Parallel Processing Framework Implementation
// =============================================================================

static void* parallel_worker_thread(void* arg) {
    ParallelContext* context = (ParallelContext*)arg;
    if (!context) return NULL;
    
    // Wait for all threads to be ready
    pthread_barrier_wait(&context->start_barrier);
    
    uint64_t start_time = get_current_time_us();
    
    // Determine work range for this thread
    int thread_id = 0; // Would need proper thread ID assignment
    int64_t total_work = context->end - context->start;
    int64_t work_per_thread = total_work / context->thread_count;
    int64_t thread_start = context->start + thread_id * work_per_thread;
    int64_t thread_end = (thread_id == context->thread_count - 1) ? 
                         context->end : thread_start + work_per_thread;
    
    // Execute work based on context type
    switch (context->type) {
        case PARALLEL_FOR:
            // Would execute parallel for work
            break;
        case PARALLEL_FOREACH:
            // Would execute parallel foreach work
            break;
        default:
            break;
    }
    
    uint64_t end_time = get_current_time_us();
    
    // Update statistics
    context->stats.total_operations += (thread_end - thread_start);
    
    // Wait for all threads to complete
    pthread_barrier_wait(&context->end_barrier);
    
    return NULL;
}

ParallelContext* parallel_context_create(ParallelLoopType type, int64_t start, int64_t end) {
    ParallelContext* context = calloc(1, sizeof(ParallelContext));
    if (!context) return NULL;
    
    context->type = type;
    context->start = start;
    context->end = end;
    context->step = 1;
    context->thread_count = sysconf(_SC_NPROCESSORS_ONLN);
    context->use_work_stealing = true;
    
    // Initialize barriers
    if (pthread_barrier_init(&context->start_barrier, NULL, context->thread_count) != 0 ||
        pthread_barrier_init(&context->end_barrier, NULL, context->thread_count) != 0 ||
        pthread_mutex_init(&context->error_mutex, NULL) != 0) {
        parallel_context_destroy(context);
        return NULL;
    }
    
    return context;
}

void parallel_context_add_shared_var(ParallelContext* context, SharedVar* var) {
    if (!context || !var) return;
    
    // Resize shared vars array if needed
    if (context->shared_var_count >= 16) return; // Simple limit
    
    if (!context->shared_vars) {
        context->shared_vars = calloc(16, sizeof(SharedVar*));
        if (!context->shared_vars) return;
    }
    
    context->shared_vars[context->shared_var_count++] = var;
}

void parallel_context_execute(ParallelContext* context, void* func, void* user_context) {
    if (!context || !func) return;
    
    context->stats.start_time = get_current_time_us();
    
    // Create worker threads
    pthread_t* threads = calloc(context->thread_count, sizeof(pthread_t));
    if (!threads) return;
    
    for (int i = 0; i < context->thread_count; i++) {
        if (pthread_create(&threads[i], NULL, parallel_worker_thread, context) != 0) {
            // Handle error
            break;
        }
    }
    
    // Wait for completion
    for (int i = 0; i < context->thread_count; i++) {
        pthread_join(threads[i], NULL);
    }
    
    context->stats.end_time = get_current_time_us();
    free(threads);
}

void parallel_context_destroy(ParallelContext* context) {
    if (!context) return;
    
    pthread_barrier_destroy(&context->start_barrier);
    pthread_barrier_destroy(&context->end_barrier);
    pthread_mutex_destroy(&context->error_mutex);
    
    free(context->shared_vars);
    free(context->errors);
    free(context);
}

// =============================================================================
// Simple Parallel Loop Implementations
// =============================================================================

void parallel_for(int64_t start, int64_t end, ParallelForFunc func, void* context) {
    parallel_for_with_threads(start, end, sysconf(_SC_NPROCESSORS_ONLN), func, context);
}

void parallel_for_with_threads(int64_t start, int64_t end, int thread_count, 
                              ParallelForFunc func, void* context) {
    if (!func || start >= end) return;
    
    ParallelContext* par_context = parallel_context_create(PARALLEL_FOR, start, end);
    if (!par_context) return;
    
    par_context->thread_count = thread_count;
    parallel_context_execute(par_context, (void*)func, context);
    parallel_context_destroy(par_context);
}

// =============================================================================
// System Management Implementation
// =============================================================================

SharedVarSystem* shared_var_system_create(const char* name) {
    SharedVarSystem* system = calloc(1, sizeof(SharedVarSystem));
    if (!system) return NULL;
    
    system->system_name = safe_strdup(name ? name : "default");
    system->registry = shared_var_registry_create();
    if (!system->registry) {
        free((void*)system->system_name);
        free(system);
        return NULL;
    }
    
    // Set default configuration
    system->config.default_thread_count = sysconf(_SC_NPROCESSORS_ONLN);
    system->config.enable_work_stealing = true;
    system->config.numa_aware = false;
    system->config.cache_line_size = 64;
    system->config.enable_statistics = true;
    
    // Initialize work queue
    system->queue_capacity = 64;
    system->work_queue = calloc(system->queue_capacity, sizeof(ParallelContext*));
    if (!system->work_queue) {
        shared_var_registry_destroy(system->registry);
        free((void*)system->system_name);
        free(system);
        return NULL;
    }
    
    if (pthread_mutex_init(&system->queue_mutex, NULL) != 0 ||
        pthread_cond_init(&system->work_available, NULL) != 0) {
        free(system->work_queue);
        shared_var_registry_destroy(system->registry);
        free((void*)system->system_name);
        free(system);
        return NULL;
    }
    
    // Create system arena
    system->system_arena = goo_arena_new(16384, system->system_name);
    
    return system;
}

void shared_var_system_destroy(SharedVarSystem* system) {
    if (!system) return;
    
    // Stop workers if running
    shared_var_system_stop(system);
    
    // Clean up work queue
    pthread_mutex_lock(&system->queue_mutex);
    for (int i = 0; i < system->queue_size; i++) {
        parallel_context_destroy(system->work_queue[i]);
    }
    free(system->work_queue);
    pthread_mutex_unlock(&system->queue_mutex);
    
    pthread_cond_destroy(&system->work_available);
    pthread_mutex_destroy(&system->queue_mutex);
    
    // Clean up registry
    shared_var_registry_destroy(system->registry);
    
    // Clean up arena
    goo_arena_free(system->system_arena);
    
    free(system->worker_threads);
    free((void*)system->system_name);
    free(system);
}

bool shared_var_system_start(SharedVarSystem* system) {
    if (!system || system->workers_active) return false;
    
    system->worker_count = system->config.default_thread_count;
    system->worker_threads = calloc(system->worker_count, sizeof(pthread_t));
    if (!system->worker_threads) return false;
    
    system->workers_active = true;
    
    // Start worker threads (placeholder - would implement full worker system)
    for (int i = 0; i < system->worker_count; i++) {
        // pthread_create(&system->worker_threads[i], NULL, worker_thread, system);
    }
    
    return true;
}

void shared_var_system_stop(SharedVarSystem* system) {
    if (!system || !system->workers_active) return;
    
    system->workers_active = false;
    
    // Signal all workers to stop
    pthread_cond_broadcast(&system->work_available);
    
    // Wait for workers to finish
    for (int i = 0; i < system->worker_count; i++) {
        if (system->worker_threads[i]) {
            pthread_join(system->worker_threads[i], NULL);
        }
    }
    
    free(system->worker_threads);
    system->worker_threads = NULL;
    system->worker_count = 0;
}

SharedVarSystem* get_global_shared_var_system(void) {
    pthread_mutex_lock(&g_system_mutex);
    
    if (!g_shared_var_system) {
        g_shared_var_system = shared_var_system_create("global");
        if (g_shared_var_system) {
            shared_var_system_start(g_shared_var_system);
        }
    }
    
    pthread_mutex_unlock(&g_system_mutex);
    return g_shared_var_system;
}

// =============================================================================
// Statistics and Monitoring Implementation
// =============================================================================

SharedVarStats shared_var_get_stats(SharedVar* var) {
    SharedVarStats stats = {0};
    if (var && var->config.enable_statistics) {
        pthread_mutex_lock(&var->stats_mutex);
        stats = var->stats;
        pthread_mutex_unlock(&var->stats_mutex);
    }
    return stats;
}

void shared_var_reset_stats(SharedVar* var) {
    if (var && var->config.enable_statistics) {
        pthread_mutex_lock(&var->stats_mutex);
        memset(&var->stats, 0, sizeof(SharedVarStats));
        pthread_mutex_unlock(&var->stats_mutex);
    }
}

void shared_var_system_print_stats(SharedVarSystem* system) {
    if (!system) return;
    
    printf("=== Shared Variable System Statistics ===\n");
    printf("System: %s\n", system->system_name);
    printf("Registry Variables: %d\n", system->registry->var_count);
    printf("Global Stats:\n");
    printf("  Variables Created: %lu\n", system->registry->global_stats.variables_created);
    printf("  Variables Destroyed: %lu\n", system->registry->global_stats.variables_destroyed);
    printf("  Watchers Registered: %lu\n", system->registry->global_stats.watchers_registered);
    printf("  Events Generated: %lu\n", system->registry->global_stats.events_generated);
}

// =============================================================================
// Debugging and Integration Functions (Stubs)
// =============================================================================

void shared_var_dump_info(SharedVar* var) {
    if (!var) return;
    
    printf("=== Shared Variable Info ===\n");
    printf("ID: %lu\n", var->var_id);
    printf("Name: %s\n", var->name ? var->name : "unnamed");
    printf("Type: %d\n", var->type);
    printf("Watchers: %d/%d\n", var->watcher_count, var->watcher_capacity);
    
    if (var->config.enable_statistics) {
        SharedVarStats stats = shared_var_get_stats(var);
        printf("Statistics:\n");
        printf("  Reads: %lu\n", stats.reads);
        printf("  Writes: %lu\n", stats.writes);
        printf("  Atomic Ops: %lu\n", stats.atomic_ops);
    }
}

// Placeholder implementations for integration functions
void shared_var_register_with_actor(SharedVar* var, ActorRef* actor) {
    // Would implement actor integration
}

void shared_var_notify_actor_on_change(SharedVar* var, ActorRef* actor, const char* message) {
    // Would implement actor notification system
}

SharedVar* actor_create_shared_var(ActorRef* actor, const char* name, SharedVarType type, 
                                  const void* initial_value) {
    // Would create shared variable in actor context
    return shared_var_create(name, type, initial_value);
}

void actor_watch_shared_var(ActorRef* actor, SharedVar* var, const char* handler_name) {
    // Would register actor watcher for shared variable
}