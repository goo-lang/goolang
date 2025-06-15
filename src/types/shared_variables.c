#include "../../include/shared_variables.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>

// Global shared variable manager
static SharedVarManager* global_manager = NULL;
static pthread_mutex_t global_manager_mutex = PTHREAD_MUTEX_INITIALIZER;

// Utility functions
static uint64_t get_current_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t generate_id(void) {
    static atomic_uint_fast64_t id_counter = ATOMIC_VAR_INIT(1);
    return atomic_fetch_add(&id_counter, 1);
}

// Shared variable manager implementation
SharedVarManager* shared_var_manager_create(size_t max_variables) {
    SharedVarManager* manager = malloc(sizeof(SharedVarManager));
    if (!manager) return NULL;
    
    manager->max_variables = max_variables > 0 ? max_variables : 1000;
    manager->enable_global_statistics = true;
    manager->enable_automatic_optimization = true;
    
    // Initialize variable registry
    manager->variables = calloc(manager->max_variables, sizeof(SharedVariable*));
    if (!manager->variables) {
        free(manager);
        return NULL;
    }
    
    manager->variable_count = 0;
    manager->variable_capacity = manager->max_variables;
    
    if (pthread_mutex_init(&manager->registry_mutex, NULL) != 0) {
        free(manager->variables);
        free(manager);
        return NULL;
    }
    
    // Initialize sync groups
    manager->sync_group_capacity = 100;
    manager->sync_groups = calloc(manager->sync_group_capacity, sizeof(SyncGroup*));
    if (!manager->sync_groups) {
        pthread_mutex_destroy(&manager->registry_mutex);
        free(manager->variables);
        free(manager);
        return NULL;
    }
    
    manager->sync_group_count = 0;
    
    // Initialize memory pool
    manager->pool_size = 1024 * 1024; // 1MB default pool
    manager->memory_pool = malloc(manager->pool_size);
    if (!manager->memory_pool) {
        free(manager->sync_groups);
        pthread_mutex_destroy(&manager->registry_mutex);
        free(manager->variables);
        free(manager);
        return NULL;
    }
    
    manager->pool_used = 0;
    if (pthread_mutex_init(&manager->pool_mutex, NULL) != 0) {
        free(manager->memory_pool);
        free(manager->sync_groups);
        pthread_mutex_destroy(&manager->registry_mutex);
        free(manager->variables);
        free(manager);
        return NULL;
    }
    
    // Initialize optimization engine
    manager->optimization_enabled = false;
    manager->optimization_interval_ms = 5000; // 5 seconds
    
    // Initialize statistics
    manager->total_operations = 0;
    manager->total_contentions = 0;
    manager->total_optimizations = 0;
    
    manager->actor_system = NULL;
    
    // Set as global manager if none exists
    pthread_mutex_lock(&global_manager_mutex);
    if (!global_manager) {
        global_manager = manager;
    }
    pthread_mutex_unlock(&global_manager_mutex);
    
    return manager;
}

void shared_var_manager_destroy(SharedVarManager* manager) {
    if (!manager) return;
    
    // Stop optimization thread if running
    if (manager->optimization_enabled) {
        shared_var_manager_stop_optimization(manager);
    }
    
    // Destroy all variables
    pthread_mutex_lock(&manager->registry_mutex);
    for (size_t i = 0; i < manager->variable_count; i++) {
        if (manager->variables[i]) {
            shared_var_destroy(manager->variables[i]);
        }
    }
    pthread_mutex_unlock(&manager->registry_mutex);
    
    // Destroy all sync groups
    for (size_t i = 0; i < manager->sync_group_count; i++) {
        if (manager->sync_groups[i]) {
            sync_group_destroy(manager->sync_groups[i]);
        }
    }
    
    // Clear global manager reference
    pthread_mutex_lock(&global_manager_mutex);
    if (global_manager == manager) {
        global_manager = NULL;
    }
    pthread_mutex_unlock(&global_manager_mutex);
    
    // Clean up resources
    pthread_mutex_destroy(&manager->registry_mutex);
    pthread_mutex_destroy(&manager->pool_mutex);
    free(manager->memory_pool);
    free(manager->sync_groups);
    free(manager->variables);
    free(manager);
}

// Configuration helpers
SharedVarConfig shared_var_config_default(const char* name, SharedVarType type) {
    SharedVarConfig config = {0};
    config.name = name;
    config.type = type;
    config.sync_mode = SYNC_MODE_ADAPTIVE;
    config.access_pattern = ACCESS_PATTERN_UNKNOWN;
    config.consistency = CONSISTENCY_SEQUENTIAL;
    config.initial_capacity = 64;
    config.enable_statistics = true;
    config.enable_contention_detection = true;
    config.use_memory_pool = true;
    config.alignment_requirement = sizeof(void*);
    config.custom_ops = NULL;
    config.custom_sync_state = NULL;
    config.validate_value = NULL;
    config.transform_on_write = NULL;
    config.transform_context = NULL;
    return config;
}

SharedVarConfig shared_var_config_atomic(const char* name, SharedVarType type) {
    SharedVarConfig config = shared_var_config_default(name, type);
    config.sync_mode = SYNC_MODE_ATOMIC;
    config.consistency = CONSISTENCY_LINEARIZABLE;
    return config;
}

SharedVarConfig shared_var_config_read_heavy(const char* name, SharedVarType type) {
    SharedVarConfig config = shared_var_config_default(name, type);
    config.sync_mode = SYNC_MODE_RW_LOCK;
    config.access_pattern = ACCESS_PATTERN_READ_HEAVY;
    config.consistency = CONSISTENCY_CAUSAL;
    return config;
}

SharedVarConfig shared_var_config_write_heavy(const char* name, SharedVarType type) {
    SharedVarConfig config = shared_var_config_default(name, type);
    config.sync_mode = SYNC_MODE_MUTEX;
    config.access_pattern = ACCESS_PATTERN_WRITE_HEAVY;
    return config;
}

SharedVarConfig shared_var_config_high_contention(const char* name, SharedVarType type) {
    SharedVarConfig config = shared_var_config_default(name, type);
    config.sync_mode = SYNC_MODE_ADAPTIVE;
    config.access_pattern = ACCESS_PATTERN_BALANCED;
    config.enable_contention_detection = true;
    return config;
}

// Shared variable creation
SharedVariable* shared_var_create(SharedVarManager* manager, SharedVarConfig config) {
    if (!manager) return NULL;
    
    pthread_mutex_lock(&manager->registry_mutex);
    
    if (manager->variable_count >= manager->max_variables) {
        pthread_mutex_unlock(&manager->registry_mutex);
        return NULL;
    }
    
    SharedVariable* var = malloc(sizeof(SharedVariable));
    if (!var) {
        pthread_mutex_unlock(&manager->registry_mutex);
        return NULL;
    }
    
    // Initialize basic properties
    var->id = generate_id();
    strncpy(var->name, config.name, sizeof(var->name) - 1);
    var->name[sizeof(var->name) - 1] = '\0';
    var->config = config;
    var->manager = manager;
    
    // Initialize metadata
    var->version = 1;
    var->creation_time = get_current_time_ns();
    var->last_access_time = var->creation_time;
    
    // Initialize statistics
    memset(&var->stats, 0, sizeof(SharedVarStats));
    
    // Initialize reference counting
    atomic_init(&var->ref_count, 1);
    
    // Set value size based on type
    switch (config.type) {
        case SHARED_TYPE_INT32:
        case SHARED_TYPE_UINT32:
        case SHARED_TYPE_FLOAT32:
            var->value_size = 4;
            break;
        case SHARED_TYPE_INT64:
        case SHARED_TYPE_UINT64:
        case SHARED_TYPE_FLOAT64:
        case SHARED_TYPE_PTR:
            var->value_size = 8;
            break;
        case SHARED_TYPE_BOOL:
            var->value_size = 1;
            break;
        case SHARED_TYPE_STRING:
            var->value_size = config.initial_capacity;
            break;
        case SHARED_TYPE_CUSTOM:
            var->value_size = config.initial_capacity;
            break;
    }
    
    // Initialize synchronization based on mode
    switch (config.sync_mode) {
        case SYNC_MODE_ATOMIC:
            // Initialize atomic values based on type
            switch (config.type) {
                case SHARED_TYPE_INT32:
                    atomic_init(&var->sync_state.atomic.value_i32, 0);
                    break;
                case SHARED_TYPE_INT64:
                    atomic_init(&var->sync_state.atomic.value_i64, 0);
                    break;
                case SHARED_TYPE_UINT32:
                    atomic_init(&var->sync_state.atomic.value_u32, 0);
                    break;
                case SHARED_TYPE_UINT64:
                    atomic_init(&var->sync_state.atomic.value_u64, 0);
                    break;
                case SHARED_TYPE_BOOL:
                    atomic_init(&var->sync_state.atomic.value_bool, false);
                    break;
                case SHARED_TYPE_PTR:
                    atomic_init(&var->sync_state.atomic.value_ptr, NULL);
                    break;
                default:
                    // For non-atomic types, fall back to mutex
                    if (pthread_mutex_init(&var->sync_state.mutex.mutex, NULL) != 0) {
                        free(var);
                        pthread_mutex_unlock(&manager->registry_mutex);
                        return NULL;
                    }
                    var->sync_state.mutex.value = malloc(var->value_size);
                    if (!var->sync_state.mutex.value) {
                        pthread_mutex_destroy(&var->sync_state.mutex.mutex);
                        free(var);
                        pthread_mutex_unlock(&manager->registry_mutex);
                        return NULL;
                    }
                    memset(var->sync_state.mutex.value, 0, var->value_size);
                    break;
            }
            break;
            
        case SYNC_MODE_MUTEX:
            if (pthread_mutex_init(&var->sync_state.mutex.mutex, NULL) != 0) {
                free(var);
                pthread_mutex_unlock(&manager->registry_mutex);
                return NULL;
            }
            var->sync_state.mutex.value = malloc(var->value_size);
            if (!var->sync_state.mutex.value) {
                pthread_mutex_destroy(&var->sync_state.mutex.mutex);
                free(var);
                pthread_mutex_unlock(&manager->registry_mutex);
                return NULL;
            }
            memset(var->sync_state.mutex.value, 0, var->value_size);
            break;
            
        case SYNC_MODE_RW_LOCK:
            if (pthread_rwlock_init(&var->sync_state.rwlock.rwlock, NULL) != 0) {
                free(var);
                pthread_mutex_unlock(&manager->registry_mutex);
                return NULL;
            }
            var->sync_state.rwlock.value = malloc(var->value_size);
            if (!var->sync_state.rwlock.value) {
                pthread_rwlock_destroy(&var->sync_state.rwlock.rwlock);
                free(var);
                pthread_mutex_unlock(&manager->registry_mutex);
                return NULL;
            }
            memset(var->sync_state.rwlock.value, 0, var->value_size);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            var->sync_state.spinlock.value = malloc(var->value_size);
            if (!var->sync_state.spinlock.value) {
                free(var);
                pthread_mutex_unlock(&manager->registry_mutex);
                return NULL;
            }
            memset(var->sync_state.spinlock.value, 0, var->value_size);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            var->sync_state.adaptive.current_mode = SYNC_MODE_ATOMIC;
            var->sync_state.adaptive.contention_count = 0;
            var->sync_state.adaptive.last_adaptation_time = get_current_time_ns();
            
            // Start with atomic if supported
            switch (config.type) {
                case SHARED_TYPE_INT32:
                case SHARED_TYPE_INT64:
                case SHARED_TYPE_UINT32:
                case SHARED_TYPE_UINT64:
                case SHARED_TYPE_BOOL:
                case SHARED_TYPE_PTR:
                    atomic_init(&var->sync_state.adaptive.sync.atomic_value, 0);
                    break;
                default:
                    // Use mutex for complex types
                    var->sync_state.adaptive.current_mode = SYNC_MODE_MUTEX;
                    if (pthread_mutex_init(&var->sync_state.adaptive.sync.mutex, NULL) != 0) {
                        free(var);
                        pthread_mutex_unlock(&manager->registry_mutex);
                        return NULL;
                    }
                    break;
            }
            
            var->sync_state.adaptive.value = malloc(var->value_size);
            if (!var->sync_state.adaptive.value) {
                if (var->sync_state.adaptive.current_mode == SYNC_MODE_MUTEX) {
                    pthread_mutex_destroy(&var->sync_state.adaptive.sync.mutex);
                }
                free(var);
                pthread_mutex_unlock(&manager->registry_mutex);
                return NULL;
            }
            memset(var->sync_state.adaptive.value, 0, var->value_size);
            break;
            
        case SYNC_MODE_WAIT_FREE:
            // For now, implement as atomic
            switch (config.type) {
                case SHARED_TYPE_INT32:
                    atomic_init(&var->sync_state.atomic.value_i32, 0);
                    break;
                case SHARED_TYPE_INT64:
                    atomic_init(&var->sync_state.atomic.value_i64, 0);
                    break;
                case SHARED_TYPE_UINT32:
                    atomic_init(&var->sync_state.atomic.value_u32, 0);
                    break;
                case SHARED_TYPE_UINT64:
                    atomic_init(&var->sync_state.atomic.value_u64, 0);
                    break;
                case SHARED_TYPE_BOOL:
                    atomic_init(&var->sync_state.atomic.value_bool, false);
                    break;
                case SHARED_TYPE_PTR:
                    atomic_init(&var->sync_state.atomic.value_ptr, NULL);
                    break;
                default:
                    // Fall back to mutex for complex types
                    if (pthread_mutex_init(&var->sync_state.mutex.mutex, NULL) != 0) {
                        free(var);
                        pthread_mutex_unlock(&manager->registry_mutex);
                        return NULL;
                    }
                    var->sync_state.mutex.value = malloc(var->value_size);
                    if (!var->sync_state.mutex.value) {
                        pthread_mutex_destroy(&var->sync_state.mutex.mutex);
                        free(var);
                        pthread_mutex_unlock(&manager->registry_mutex);
                        return NULL;
                    }
                    memset(var->sync_state.mutex.value, 0, var->value_size);
                    break;
            }
            break;
    }
    
    // Add to manager registry
    manager->variables[manager->variable_count] = var;
    manager->variable_count++;
    manager->total_shared_variables++;
    
    var->next = NULL;
    
    pthread_mutex_unlock(&manager->registry_mutex);
    
    return var;
}

void shared_var_destroy(SharedVariable* var) {
    if (!var) return;
    
    // Decrement reference count
    int old_count = atomic_fetch_sub(&var->ref_count, 1);
    if (old_count > 1) {
        return; // Still has references
    }
    
    // Clean up synchronization resources based on mode
    switch (var->config.sync_mode) {
        case SYNC_MODE_MUTEX:
            pthread_mutex_destroy(&var->sync_state.mutex.mutex);
            free(var->sync_state.mutex.value);
            break;
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_destroy(&var->sync_state.rwlock.rwlock);
            free(var->sync_state.rwlock.value);
            break;
        case SYNC_MODE_SPIN_LOCK:
            free(var->sync_state.spinlock.value);
            break;
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_MUTEX) {
                pthread_mutex_destroy(&var->sync_state.adaptive.sync.mutex);
            } else if (var->sync_state.adaptive.current_mode == SYNC_MODE_RW_LOCK) {
                pthread_rwlock_destroy(&var->sync_state.adaptive.sync.rwlock);
            }
            free(var->sync_state.adaptive.value);
            break;
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            // Atomic values don't need cleanup, but check for allocated value storage
            if (var->config.type == SHARED_TYPE_STRING || var->config.type == SHARED_TYPE_CUSTOM) {
                if (var->sync_state.mutex.value) {
                    free(var->sync_state.mutex.value);
                }
            }
            break;
    }
    
    free(var);
}

// Utility function to update statistics
static void update_read_stats(SharedVariable* var) {
    if (!var->config.enable_statistics) return;
    
    var->stats.total_reads++;
    var->last_access_time = get_current_time_ns();
}

static void update_write_stats(SharedVariable* var) {
    if (!var->config.enable_statistics) return;
    
    var->stats.total_writes++;
    var->last_access_time = get_current_time_ns();
    var->version++;
}

// Find functions
SharedVariable* shared_var_find_by_name(SharedVarManager* manager, const char* name) {
    if (!manager || !name) return NULL;
    
    pthread_mutex_lock(&manager->registry_mutex);
    
    for (size_t i = 0; i < manager->variable_count; i++) {
        if (manager->variables[i] && strcmp(manager->variables[i]->name, name) == 0) {
            SharedVariable* var = manager->variables[i];
            atomic_fetch_add(&var->ref_count, 1); // Increment ref count
            pthread_mutex_unlock(&manager->registry_mutex);
            return var;
        }
    }
    
    pthread_mutex_unlock(&manager->registry_mutex);
    return NULL;
}

SharedVariable* shared_var_find_by_id(SharedVarManager* manager, uint64_t id) {
    if (!manager) return NULL;
    
    pthread_mutex_lock(&manager->registry_mutex);
    
    for (size_t i = 0; i < manager->variable_count; i++) {
        if (manager->variables[i] && manager->variables[i]->id == id) {
            SharedVariable* var = manager->variables[i];
            atomic_fetch_add(&var->ref_count, 1); // Increment ref count
            pthread_mutex_unlock(&manager->registry_mutex);
            return var;
        }
    }
    
    pthread_mutex_unlock(&manager->registry_mutex);
    return NULL;
}

// Atomic read operations for primitives
Result_int32_t shared_var_get_int32(SharedVariable* var) {
    if (!var || var->config.type != SHARED_TYPE_INT32) {
        return (Result_int32_t){.is_error = true, .error = malloc(sizeof(Error))};
    }
    
    update_read_stats(var);
    
    int32_t value;
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            value = atomic_load(&var->sync_state.atomic.value_i32);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                value = atomic_load(&var->sync_state.adaptive.sync.atomic_value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                value = *(int32_t*)var->sync_state.adaptive.value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            value = *(int32_t*)var->sync_state.mutex.value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_rdlock(&var->sync_state.rwlock.rwlock);
            value = *(int32_t*)var->sync_state.rwlock.value;
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            value = *(int32_t*)var->sync_state.spinlock.value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    return (Result_int32_t){.is_error = false, .value = value};
}

Result_int64_t shared_var_get_int64(SharedVariable* var) {
    if (!var || var->config.type != SHARED_TYPE_INT64) {
        return (Result_int64_t){.is_error = true, .error = malloc(sizeof(Error))};
    }
    
    update_read_stats(var);
    
    int64_t value;
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            value = atomic_load(&var->sync_state.atomic.value_i64);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                value = atomic_load(&var->sync_state.adaptive.sync.atomic_value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                value = *(int64_t*)var->sync_state.adaptive.value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            value = *(int64_t*)var->sync_state.mutex.value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_rdlock(&var->sync_state.rwlock.rwlock);
            value = *(int64_t*)var->sync_state.rwlock.value;
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            value = *(int64_t*)var->sync_state.spinlock.value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    return (Result_int64_t){.is_error = false, .value = value};
}

Result_uint32_t shared_var_get_uint32(SharedVariable* var) {
    if (!var || var->config.type != SHARED_TYPE_UINT32) {
        return (Result_uint32_t){.is_error = true, .error = malloc(sizeof(Error))};
    }
    
    update_read_stats(var);
    
    uint32_t value;
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            value = atomic_load(&var->sync_state.atomic.value_u32);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                value = atomic_load(&var->sync_state.adaptive.sync.atomic_value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                value = *(uint32_t*)var->sync_state.adaptive.value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            value = *(uint32_t*)var->sync_state.mutex.value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_rdlock(&var->sync_state.rwlock.rwlock);
            value = *(uint32_t*)var->sync_state.rwlock.value;
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            value = *(uint32_t*)var->sync_state.spinlock.value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    return (Result_uint32_t){.is_error = false, .value = value};
}

Result_uint64_t shared_var_get_uint64(SharedVariable* var) {
    if (!var || var->config.type != SHARED_TYPE_UINT64) {
        return (Result_uint64_t){.is_error = true, .error = malloc(sizeof(Error))};
    }
    
    update_read_stats(var);
    
    uint64_t value;
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            value = atomic_load(&var->sync_state.atomic.value_u64);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                value = atomic_load(&var->sync_state.adaptive.sync.atomic_value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                value = *(uint64_t*)var->sync_state.adaptive.value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            value = *(uint64_t*)var->sync_state.mutex.value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_rdlock(&var->sync_state.rwlock.rwlock);
            value = *(uint64_t*)var->sync_state.rwlock.value;
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            value = *(uint64_t*)var->sync_state.spinlock.value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    return (Result_uint64_t){.is_error = false, .value = value};
}

Result_bool shared_var_get_bool(SharedVariable* var) {
    if (!var || var->config.type != SHARED_TYPE_BOOL) {
        return (Result_bool){.is_error = true, .error = malloc(sizeof(Error))};
    }
    
    update_read_stats(var);
    
    bool value;
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            value = atomic_load(&var->sync_state.atomic.value_bool);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                value = atomic_load(&var->sync_state.adaptive.sync.atomic_value) != 0;
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                value = *(bool*)var->sync_state.adaptive.value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            value = *(bool*)var->sync_state.mutex.value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_rdlock(&var->sync_state.rwlock.rwlock);
            value = *(bool*)var->sync_state.rwlock.value;
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            value = *(bool*)var->sync_state.spinlock.value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    return (Result_bool){.is_error = false, .value = value};
}

Result_void_ptr shared_var_get_ptr(SharedVariable* var) {
    if (!var || var->config.type != SHARED_TYPE_PTR) {
        return ERR_PTR(malloc(sizeof(Error)));
    }
    
    update_read_stats(var);
    
    void* value;
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            value = atomic_load(&var->sync_state.atomic.value_ptr);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                value = (void*)atomic_load(&var->sync_state.adaptive.sync.atomic_value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                value = *(void**)var->sync_state.adaptive.value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            value = *(void**)var->sync_state.mutex.value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_rdlock(&var->sync_state.rwlock.rwlock);
            value = *(void**)var->sync_state.rwlock.value;
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            value = *(void**)var->sync_state.spinlock.value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    return OK_PTR(value);
}

// Atomic write operations for primitives
Result_void_ptr shared_var_set_int32(SharedVariable* var, int32_t value) {
    if (!var || var->config.type != SHARED_TYPE_INT32) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid variable or type mismatch";
        }
        return ERR_PTR(err);
    }
    
    update_write_stats(var);
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            atomic_store(&var->sync_state.atomic.value_i32, value);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                atomic_store(&var->sync_state.adaptive.sync.atomic_value, value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                *(int32_t*)var->sync_state.adaptive.value = value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            *(int32_t*)var->sync_state.mutex.value = value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_wrlock(&var->sync_state.rwlock.rwlock);
            *(int32_t*)var->sync_state.rwlock.value = value;
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            *(int32_t*)var->sync_state.spinlock.value = value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    return OK_PTR(var);
}

Result_void_ptr shared_var_set_int64(SharedVariable* var, int64_t value) {
    if (!var || var->config.type != SHARED_TYPE_INT64) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid variable or type mismatch";
        }
        return ERR_PTR(err);
    }
    
    update_write_stats(var);
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            atomic_store(&var->sync_state.atomic.value_i64, value);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                atomic_store(&var->sync_state.adaptive.sync.atomic_value, value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                *(int64_t*)var->sync_state.adaptive.value = value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            *(int64_t*)var->sync_state.mutex.value = value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_wrlock(&var->sync_state.rwlock.rwlock);
            *(int64_t*)var->sync_state.rwlock.value = value;
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            *(int64_t*)var->sync_state.spinlock.value = value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    return OK_PTR(var);
}

Result_void_ptr shared_var_set_uint32(SharedVariable* var, uint32_t value) {
    if (!var || var->config.type != SHARED_TYPE_UINT32) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid variable or type mismatch";
        }
        return ERR_PTR(err);
    }
    
    update_write_stats(var);
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            atomic_store(&var->sync_state.atomic.value_u32, value);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                atomic_store(&var->sync_state.adaptive.sync.atomic_value, value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                *(uint32_t*)var->sync_state.adaptive.value = value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            *(uint32_t*)var->sync_state.mutex.value = value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_wrlock(&var->sync_state.rwlock.rwlock);
            *(uint32_t*)var->sync_state.rwlock.value = value;
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            *(uint32_t*)var->sync_state.spinlock.value = value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    return OK_PTR(var);
}

Result_void_ptr shared_var_set_uint64(SharedVariable* var, uint64_t value) {
    if (!var || var->config.type != SHARED_TYPE_UINT64) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid variable or type mismatch";
        }
        return ERR_PTR(err);
    }
    
    update_write_stats(var);
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            atomic_store(&var->sync_state.atomic.value_u64, value);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                atomic_store(&var->sync_state.adaptive.sync.atomic_value, value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                *(uint64_t*)var->sync_state.adaptive.value = value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            *(uint64_t*)var->sync_state.mutex.value = value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_wrlock(&var->sync_state.rwlock.rwlock);
            *(uint64_t*)var->sync_state.rwlock.value = value;
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            *(uint64_t*)var->sync_state.spinlock.value = value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    return OK_PTR(var);
}

Result_void_ptr shared_var_set_bool(SharedVariable* var, bool value) {
    if (!var || var->config.type != SHARED_TYPE_BOOL) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid variable or type mismatch";
        }
        return ERR_PTR(err);
    }
    
    update_write_stats(var);
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            atomic_store(&var->sync_state.atomic.value_bool, value);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                atomic_store(&var->sync_state.adaptive.sync.atomic_value, value ? 1 : 0);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                *(bool*)var->sync_state.adaptive.value = value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            *(bool*)var->sync_state.mutex.value = value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_wrlock(&var->sync_state.rwlock.rwlock);
            *(bool*)var->sync_state.rwlock.value = value;
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            *(bool*)var->sync_state.spinlock.value = value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    return OK_PTR(var);
}

Result_void_ptr shared_var_set_ptr(SharedVariable* var, void* value) {
    if (!var || var->config.type != SHARED_TYPE_PTR) {
        Error* err = malloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid variable or type mismatch";
        }
        return ERR_PTR(err);
    }
    
    update_write_stats(var);
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            atomic_store(&var->sync_state.atomic.value_ptr, value);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                atomic_store(&var->sync_state.adaptive.sync.atomic_value, (uint64_t)value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                *(void**)var->sync_state.adaptive.value = value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            *(void**)var->sync_state.mutex.value = value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_wrlock(&var->sync_state.rwlock.rwlock);
            *(void**)var->sync_state.rwlock.value = value;
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            *(void**)var->sync_state.spinlock.value = value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    return OK_PTR(var);
}

// Atomic arithmetic operations
Result_int32_t shared_var_fetch_add_int32(SharedVariable* var, int32_t value) {
    if (!var || var->config.type != SHARED_TYPE_INT32) {
        return (Result_int32_t){.is_error = true, .error = malloc(sizeof(Error))};
    }
    
    update_write_stats(var);
    
    int32_t old_value;
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            old_value = atomic_fetch_add(&var->sync_state.atomic.value_i32, value);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                old_value = atomic_fetch_add(&var->sync_state.adaptive.sync.atomic_value, value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                old_value = *(int32_t*)var->sync_state.adaptive.value;
                *(int32_t*)var->sync_state.adaptive.value += value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            old_value = *(int32_t*)var->sync_state.mutex.value;
            *(int32_t*)var->sync_state.mutex.value += value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_wrlock(&var->sync_state.rwlock.rwlock);
            old_value = *(int32_t*)var->sync_state.rwlock.value;
            *(int32_t*)var->sync_state.rwlock.value += value;
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            old_value = *(int32_t*)var->sync_state.spinlock.value;
            *(int32_t*)var->sync_state.spinlock.value += value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    return (Result_int32_t){.is_error = false, .value = old_value};
}

Result_int64_t shared_var_fetch_add_int64(SharedVariable* var, int64_t value) {
    if (!var || var->config.type != SHARED_TYPE_INT64) {
        return (Result_int64_t){.is_error = true, .error = malloc(sizeof(Error))};
    }
    
    update_write_stats(var);
    
    int64_t old_value;
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            old_value = atomic_fetch_add(&var->sync_state.atomic.value_i64, value);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                old_value = atomic_fetch_add(&var->sync_state.adaptive.sync.atomic_value, value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                old_value = *(int64_t*)var->sync_state.adaptive.value;
                *(int64_t*)var->sync_state.adaptive.value += value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            old_value = *(int64_t*)var->sync_state.mutex.value;
            *(int64_t*)var->sync_state.mutex.value += value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_wrlock(&var->sync_state.rwlock.rwlock);
            old_value = *(int64_t*)var->sync_state.rwlock.value;
            *(int64_t*)var->sync_state.rwlock.value += value;
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            old_value = *(int64_t*)var->sync_state.spinlock.value;
            *(int64_t*)var->sync_state.spinlock.value += value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    return (Result_int64_t){.is_error = false, .value = old_value};
}

// Compare and swap operations
Result_bool shared_var_cas_int32(SharedVariable* var, int32_t expected, int32_t desired) {
    if (!var || var->config.type != SHARED_TYPE_INT32) {
        return (Result_bool){.is_error = true, .error = malloc(sizeof(Error))};
    }
    
    update_write_stats(var);
    var->stats.total_cas_attempts++;
    
    bool success = false;
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            success = atomic_compare_exchange_strong(&var->sync_state.atomic.value_i32, &expected, desired);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                success = atomic_compare_exchange_strong(&var->sync_state.adaptive.sync.atomic_value, &expected, desired);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                if (*(int32_t*)var->sync_state.adaptive.value == expected) {
                    *(int32_t*)var->sync_state.adaptive.value = desired;
                    success = true;
                }
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            if (*(int32_t*)var->sync_state.mutex.value == expected) {
                *(int32_t*)var->sync_state.mutex.value = desired;
                success = true;
            }
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_wrlock(&var->sync_state.rwlock.rwlock);
            if (*(int32_t*)var->sync_state.rwlock.value == expected) {
                *(int32_t*)var->sync_state.rwlock.value = desired;
                success = true;
            }
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            if (*(int32_t*)var->sync_state.spinlock.value == expected) {
                *(int32_t*)var->sync_state.spinlock.value = desired;
                success = true;
            }
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
    }
    
    if (success) {
        var->stats.successful_cas++;
    }
    
    return (Result_bool){.is_error = false, .value = success};
}

// String operations with fine-grained locking
Result_void_ptr shared_var_get_string(SharedVariable* var, char* buffer, size_t buffer_size) {
    if (!var || var->config.type != SHARED_TYPE_STRING || !buffer) {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_INVALID_PARAMETER;
        snprintf(error->message, sizeof(error->message), "Invalid parameters for string get");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    update_read_stats(var);
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE: {
            // For strings, we need to use mutex even in atomic mode due to complexity
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            char* str_value = (char*)var->sync_state.mutex.value;
            if (str_value) {
                size_t len = strlen(str_value);
                if (len < buffer_size) {
                    strcpy(buffer, str_value);
                } else {
                    strncpy(buffer, str_value, buffer_size - 1);
                    buffer[buffer_size - 1] = '\0';
                }
            } else {
                buffer[0] = '\0';
            }
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
        }
        
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            char* str_value = (char*)var->sync_state.mutex.value;
            if (str_value) {
                size_t len = strlen(str_value);
                if (len < buffer_size) {
                    strcpy(buffer, str_value);
                } else {
                    strncpy(buffer, str_value, buffer_size - 1);
                    buffer[buffer_size - 1] = '\0';
                }
            } else {
                buffer[0] = '\0';
            }
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_rdlock(&var->sync_state.rwlock.rwlock);
            char* str_value = (char*)var->sync_state.rwlock.value;
            if (str_value) {
                size_t len = strlen(str_value);
                if (len < buffer_size) {
                    strcpy(buffer, str_value);
                } else {
                    strncpy(buffer, str_value, buffer_size - 1);
                    buffer[buffer_size - 1] = '\0';
                }
            } else {
                buffer[0] = '\0';
            }
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            char* str_value = (char*)var->sync_state.spinlock.value;
            if (str_value) {
                size_t len = strlen(str_value);
                if (len < buffer_size) {
                    strcpy(buffer, str_value);
                } else {
                    strncpy(buffer, str_value, buffer_size - 1);
                    buffer[buffer_size - 1] = '\0';
                }
            } else {
                buffer[0] = '\0';
            }
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
            char* str_value = (char*)var->sync_state.adaptive.value;
            if (str_value) {
                size_t len = strlen(str_value);
                if (len < buffer_size) {
                    strcpy(buffer, str_value);
                } else {
                    strncpy(buffer, str_value, buffer_size - 1);
                    buffer[buffer_size - 1] = '\0';
                }
            } else {
                buffer[0] = '\0';
            }
            pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            break;
    }
    
    return OK_PTR(buffer);
}

Result_void_ptr shared_var_set_string(SharedVariable* var, const char* value) {
    if (!var || var->config.type != SHARED_TYPE_STRING) {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_INVALID_PARAMETER;
        snprintf(error->message, sizeof(error->message), "Invalid parameters for string set");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    update_write_stats(var);
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            if (var->sync_state.mutex.value) {
                free(var->sync_state.mutex.value);
            }
            if (value) {
                var->sync_state.mutex.value = malloc(strlen(value) + 1);
                strcpy((char*)var->sync_state.mutex.value, value);
            } else {
                var->sync_state.mutex.value = NULL;
            }
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_wrlock(&var->sync_state.rwlock.rwlock);
            if (var->sync_state.rwlock.value) {
                free(var->sync_state.rwlock.value);
            }
            if (value) {
                var->sync_state.rwlock.value = malloc(strlen(value) + 1);
                strcpy((char*)var->sync_state.rwlock.value, value);
            } else {
                var->sync_state.rwlock.value = NULL;
            }
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock));
            if (var->sync_state.spinlock.value) {
                free(var->sync_state.spinlock.value);
            }
            if (value) {
                var->sync_state.spinlock.value = malloc(strlen(value) + 1);
                strcpy((char*)var->sync_state.spinlock.value, value);
            } else {
                var->sync_state.spinlock.value = NULL;
            }
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
            if (var->sync_state.adaptive.value) {
                free(var->sync_state.adaptive.value);
            }
            if (value) {
                var->sync_state.adaptive.value = malloc(strlen(value) + 1);
                strcpy((char*)var->sync_state.adaptive.value, value);
            } else {
                var->sync_state.adaptive.value = NULL;
            }
            pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            break;
    }
    
    return OK_PTR(var);
}

// Custom type operations with user-defined synchronization
Result_void_ptr shared_var_get_custom(SharedVariable* var, void* buffer, size_t buffer_size) {
    if (!var || var->config.type != SHARED_TYPE_CUSTOM || !buffer || !var->config.custom_ops) {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_INVALID_PARAMETER;
        snprintf(error->message, sizeof(error->message), "Invalid parameters for custom get");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    update_read_stats(var);
    
    CustomSyncOps* ops = var->config.custom_ops;
    
    // Use custom synchronization if available
    if (ops->read_begin && ops->read_end) {
        ops->read_begin(var->sync_state.custom.sync_state);
        
        // Copy the custom value using user-provided serialization
        if (ops->serialize) {
            size_t serialized_size = buffer_size;
            ops->serialize(var->sync_state.custom.value, buffer, &serialized_size);
        } else {
            // Simple memcpy fallback
            if (buffer_size >= var->value_size) {
                memcpy(buffer, var->sync_state.custom.value, var->value_size);
            }
        }
        
        ops->read_end(var->sync_state.custom.sync_state);
    } else if (ops->lock && ops->unlock) {
        // Use simple lock/unlock
        ops->lock(var->sync_state.custom.sync_state);
        
        if (ops->serialize) {
            size_t serialized_size = buffer_size;
            ops->serialize(var->sync_state.custom.value, buffer, &serialized_size);
        } else {
            if (buffer_size >= var->value_size) {
                memcpy(buffer, var->sync_state.custom.value, var->value_size);
            }
        }
        
        ops->unlock(var->sync_state.custom.sync_state);
    } else {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_INVALID_OPERATION;
        snprintf(error->message, sizeof(error->message), "No synchronization operations defined");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    return OK_PTR(buffer);
}

Result_void_ptr shared_var_set_custom(SharedVariable* var, const void* value, size_t value_size) {
    if (!var || var->config.type != SHARED_TYPE_CUSTOM || !value || !var->config.custom_ops) {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_INVALID_PARAMETER;
        snprintf(error->message, sizeof(error->message), "Invalid parameters for custom set");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    update_write_stats(var);
    
    CustomSyncOps* ops = var->config.custom_ops;
    
    // Use custom synchronization if available
    if (ops->write_begin && ops->write_end) {
        ops->write_begin(var->sync_state.custom.sync_state);
        
        // Update the custom value using user-provided deserialization
        if (ops->deserialize) {
            ops->deserialize(var->sync_state.custom.value, value, value_size);
        } else {
            // Simple memcpy fallback
            if (value_size <= var->value_size) {
                memcpy(var->sync_state.custom.value, value, value_size);
            }
        }
        
        ops->write_end(var->sync_state.custom.sync_state);
    } else if (ops->lock && ops->unlock) {
        // Use simple lock/unlock
        ops->lock(var->sync_state.custom.sync_state);
        
        if (ops->deserialize) {
            ops->deserialize(var->sync_state.custom.value, value, value_size);
        } else {
            if (value_size <= var->value_size) {
                memcpy(var->sync_state.custom.value, value, value_size);
            }
        }
        
        ops->unlock(var->sync_state.custom.sync_state);
    } else {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_INVALID_OPERATION;
        snprintf(error->message, sizeof(error->message), "No synchronization operations defined");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    return OK_PTR(var);
}

Result_bool shared_var_cas_custom(SharedVariable* var, const void* expected, const void* desired, size_t size) {
    if (!var || var->config.type != SHARED_TYPE_CUSTOM || !expected || !desired || !var->config.custom_ops) {
        return (Result_bool){.is_error = true, .error = malloc(sizeof(Error))};
    }
    
    update_write_stats(var);
    var->stats.total_cas_attempts++;
    
    CustomSyncOps* ops = var->config.custom_ops;
    bool success = false;
    
    // Use custom synchronization for compare-and-swap
    if (ops->write_begin && ops->write_end) {
        ops->write_begin(var->sync_state.custom.sync_state);
        
        // Compare current value with expected
        if (memcmp(var->sync_state.custom.value, expected, size) == 0) {
            // Values match, perform the swap
            if (ops->deserialize) {
                ops->deserialize(var->sync_state.custom.value, desired, size);
            } else {
                memcpy(var->sync_state.custom.value, desired, size);
            }
            success = true;
        }
        
        ops->write_end(var->sync_state.custom.sync_state);
    } else if (ops->lock && ops->unlock) {
        ops->lock(var->sync_state.custom.sync_state);
        
        if (memcmp(var->sync_state.custom.value, expected, size) == 0) {
            if (ops->deserialize) {
                ops->deserialize(var->sync_state.custom.value, desired, size);
            } else {
                memcpy(var->sync_state.custom.value, desired, size);
            }
            success = true;
        }
        
        ops->unlock(var->sync_state.custom.sync_state);
    }
    
    if (success) {
        var->stats.successful_cas++;
    }
    
    return (Result_bool){.is_error = false, .value = success};
}

// Software Transactional Memory (STM) Implementation

// STM Log entry for tracking read/write operations
typedef struct STMLogEntry {
    SharedVariable* var;
    enum {
        STM_READ,
        STM_WRITE
    } operation;
    
    union {
        struct {
            void* value;
            size_t size;
            uint64_t version;  // Version when read
        } read;
        
        struct {
            void* old_value;
            void* new_value;
            size_t size;
            uint64_t old_version;
        } write;
    } data;
    
    struct STMLogEntry* next;
} STMLogEntry;

// STM Transaction structure
typedef struct STMTransaction {
    uint64_t transaction_id;
    pthread_t thread_id;
    
    // Transaction state
    bool is_active;
    bool is_read_only;
    uint64_t start_time;
    
    // Read and write logs
    STMLogEntry* read_log;
    STMLogEntry* write_log;
    size_t read_count;
    size_t write_count;
    
    // Variables accessed in this transaction
    SharedVariable** accessed_vars;
    size_t accessed_var_count;
    size_t accessed_var_capacity;
    
    // Conflict detection
    bool has_conflict;
    SharedVariable* conflict_var;
    
    struct STMTransaction* next;
} STMTransaction;

// Global STM state
static pthread_mutex_t stm_global_mutex = PTHREAD_MUTEX_INITIALIZER;
static STMTransaction* active_transactions = NULL;
static atomic_uint_fast64_t stm_transaction_counter = ATOMIC_VAR_INIT(1);

// STM utility functions
static uint64_t stm_generate_transaction_id(void) {
    return atomic_fetch_add(&stm_transaction_counter, 1);
}

static STMLogEntry* stm_create_log_entry(SharedVariable* var) {
    STMLogEntry* entry = malloc(sizeof(STMLogEntry));
    if (!entry) return NULL;
    
    entry->var = var;
    entry->next = NULL;
    return entry;
}

static void stm_destroy_log_entry(STMLogEntry* entry) {
    if (!entry) return;
    
    if (entry->operation == STM_WRITE) {
        free(entry->data.write.old_value);
        free(entry->data.write.new_value);
    } else if (entry->operation == STM_READ) {
        free(entry->data.read.value);
    }
    
    free(entry);
}

static void stm_destroy_log(STMLogEntry* head) {
    while (head) {
        STMLogEntry* next = head->next;
        stm_destroy_log_entry(head);
        head = next;
    }
}

// STM Transaction operations
STMTransaction* stm_begin_transaction(void) {
    STMTransaction* tx = malloc(sizeof(STMTransaction));
    if (!tx) return NULL;
    
    tx->transaction_id = stm_generate_transaction_id();
    tx->thread_id = pthread_self();
    tx->is_active = true;
    tx->is_read_only = true;
    tx->start_time = get_current_time_ns();
    
    tx->read_log = NULL;
    tx->write_log = NULL;
    tx->read_count = 0;
    tx->write_count = 0;
    
    tx->accessed_var_capacity = 16;
    tx->accessed_vars = malloc(sizeof(SharedVariable*) * tx->accessed_var_capacity);
    tx->accessed_var_count = 0;
    
    tx->has_conflict = false;
    tx->conflict_var = NULL;
    tx->next = NULL;
    
    // Add to global transaction list
    pthread_mutex_lock(&stm_global_mutex);
    tx->next = active_transactions;
    active_transactions = tx;
    pthread_mutex_unlock(&stm_global_mutex);
    
    return tx;
}

static bool stm_add_accessed_var(STMTransaction* tx, SharedVariable* var) {
    // Check if already tracked
    for (size_t i = 0; i < tx->accessed_var_count; i++) {
        if (tx->accessed_vars[i] == var) {
            return true; // Already tracked
        }
    }
    
    // Expand capacity if needed
    if (tx->accessed_var_count >= tx->accessed_var_capacity) {
        size_t new_capacity = tx->accessed_var_capacity * 2;
        SharedVariable** new_vars = realloc(tx->accessed_vars, sizeof(SharedVariable*) * new_capacity);
        if (!new_vars) return false;
        
        tx->accessed_vars = new_vars;
        tx->accessed_var_capacity = new_capacity;
    }
    
    tx->accessed_vars[tx->accessed_var_count++] = var;
    return true;
}

Result_void_ptr stm_read(STMTransaction* tx, SharedVariable* var, void* buffer, size_t buffer_size) {
    if (!tx || !tx->is_active || !var || !buffer) {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_INVALID_PARAMETER;
        snprintf(error->message, sizeof(error->message), "Invalid STM read parameters");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    // Add to accessed variables
    if (!stm_add_accessed_var(tx, var)) {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_OUT_OF_MEMORY;
        snprintf(error->message, sizeof(error->message), "Failed to track accessed variable");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    // Create read log entry
    STMLogEntry* entry = stm_create_log_entry(var);
    if (!entry) {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_OUT_OF_MEMORY;
        snprintf(error->message, sizeof(error->message), "Failed to create STM log entry");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    entry->operation = STM_READ;
    entry->data.read.version = var->version;
    entry->data.read.size = buffer_size;
    entry->data.read.value = malloc(buffer_size);
    
    // Perform the actual read based on variable type
    Result_void_ptr read_result;
    switch (var->config.type) {
        case SHARED_TYPE_INT32: {
            Result_int32_t int_result = shared_var_get_int32(var);
            if (int_result.is_error) {
                stm_destroy_log_entry(entry);
                return (Result_void_ptr){.is_error = true, .error = int_result.error};
            }
            *(int32_t*)entry->data.read.value = int_result.value;
            *(int32_t*)buffer = int_result.value;
            read_result = OK_PTR(buffer);
            break;
        }
        case SHARED_TYPE_INT64: {
            Result_int64_t int_result = shared_var_get_int64(var);
            if (int_result.is_error) {
                stm_destroy_log_entry(entry);
                return (Result_void_ptr){.is_error = true, .error = int_result.error};
            }
            *(int64_t*)entry->data.read.value = int_result.value;
            *(int64_t*)buffer = int_result.value;
            read_result = OK_PTR(buffer);
            break;
        }
        case SHARED_TYPE_STRING: {
            Result_void_ptr str_result = shared_var_get_string(var, (char*)buffer, buffer_size);
            if (str_result.is_error) {
                stm_destroy_log_entry(entry);
                return str_result;
            }
            strcpy((char*)entry->data.read.value, (char*)buffer);
            read_result = str_result;
            break;
        }
        default: {
            Error* error = malloc(sizeof(Error));
            error->code = ERROR_UNSUPPORTED_OPERATION;
            snprintf(error->message, sizeof(error->message), "Unsupported type for STM read");
            stm_destroy_log_entry(entry);
            return (Result_void_ptr){.is_error = true, .error = error};
        }
    }
    
    // Add to read log
    entry->next = tx->read_log;
    tx->read_log = entry;
    tx->read_count++;
    
    return read_result;
}

Result_void_ptr stm_write(STMTransaction* tx, SharedVariable* var, const void* value, size_t value_size) {
    if (!tx || !tx->is_active || !var || !value) {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_INVALID_PARAMETER;
        snprintf(error->message, sizeof(error->message), "Invalid STM write parameters");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    tx->is_read_only = false;
    
    // Add to accessed variables
    if (!stm_add_accessed_var(tx, var)) {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_OUT_OF_MEMORY;
        snprintf(error->message, sizeof(error->message), "Failed to track accessed variable");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    // Create write log entry
    STMLogEntry* entry = stm_create_log_entry(var);
    if (!entry) {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_OUT_OF_MEMORY;
        snprintf(error->message, sizeof(error->message), "Failed to create STM log entry");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    entry->operation = STM_WRITE;
    entry->data.write.old_version = var->version;
    entry->data.write.size = value_size;
    entry->data.write.old_value = malloc(value_size);
    entry->data.write.new_value = malloc(value_size);
    
    // Store current value as old value and new value
    memcpy(entry->data.write.new_value, value, value_size);
    
    // Get current value for old value
    switch (var->config.type) {
        case SHARED_TYPE_INT32: {
            Result_int32_t int_result = shared_var_get_int32(var);
            if (int_result.is_error) {
                stm_destroy_log_entry(entry);
                return (Result_void_ptr){.is_error = true, .error = int_result.error};
            }
            *(int32_t*)entry->data.write.old_value = int_result.value;
            break;
        }
        case SHARED_TYPE_INT64: {
            Result_int64_t int_result = shared_var_get_int64(var);
            if (int_result.is_error) {
                stm_destroy_log_entry(entry);
                return (Result_void_ptr){.is_error = true, .error = int_result.error};
            }
            *(int64_t*)entry->data.write.old_value = int_result.value;
            break;
        }
        default: {
            memset(entry->data.write.old_value, 0, value_size);
            break;
        }
    }
    
    // Add to write log
    entry->next = tx->write_log;
    tx->write_log = entry;
    tx->write_count++;
    
    return OK_PTR((void*)value);
}

static bool stm_validate_transaction(STMTransaction* tx) {
    // Check all read operations for conflicts
    for (STMLogEntry* entry = tx->read_log; entry; entry = entry->next) {
        if (entry->var->version != entry->data.read.version) {
            tx->has_conflict = true;
            tx->conflict_var = entry->var;
            return false;
        }
    }
    
    // Check for write-write conflicts with other transactions
    pthread_mutex_lock(&stm_global_mutex);
    for (STMTransaction* other_tx = active_transactions; other_tx; other_tx = other_tx->next) {
        if (other_tx == tx || !other_tx->is_active) continue;
        
        // Check if this transaction's writes conflict with other transaction's writes
        for (STMLogEntry* our_write = tx->write_log; our_write; our_write = our_write->next) {
            for (STMLogEntry* other_write = other_tx->write_log; other_write; other_write = other_write->next) {
                if (our_write->var == other_write->var) {
                    pthread_mutex_unlock(&stm_global_mutex);
                    tx->has_conflict = true;
                    tx->conflict_var = our_write->var;
                    return false;
                }
            }
        }
    }
    pthread_mutex_unlock(&stm_global_mutex);
    
    return true;
}

Result_void_ptr stm_commit(STMTransaction* tx) {
    if (!tx || !tx->is_active) {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_INVALID_PARAMETER;
        snprintf(error->message, sizeof(error->message), "Invalid transaction for commit");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    // Validate transaction
    if (!stm_validate_transaction(tx)) {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_TRANSACTION_CONFLICT;
        snprintf(error->message, sizeof(error->message), "Transaction conflict detected");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    // Apply all writes
    for (STMLogEntry* entry = tx->write_log; entry; entry = entry->next) {
        SharedVariable* var = entry->var;
        
        // Apply the write based on variable type
        switch (var->config.type) {
            case SHARED_TYPE_INT32:
                shared_var_set_int32(var, *(int32_t*)entry->data.write.new_value);
                break;
            case SHARED_TYPE_INT64:
                shared_var_set_int64(var, *(int64_t*)entry->data.write.new_value);
                break;
            case SHARED_TYPE_STRING:
                shared_var_set_string(var, (char*)entry->data.write.new_value);
                break;
            default:
                break;
        }
        
        // Update version for conflict detection
        var->version++;
    }
    
    tx->is_active = false;
    return OK_PTR(tx);
}

void stm_abort(STMTransaction* tx) {
    if (!tx) return;
    
    tx->is_active = false;
    tx->has_conflict = true;
}

void stm_destroy_transaction(STMTransaction* tx) {
    if (!tx) return;
    
    // Remove from global list
    pthread_mutex_lock(&stm_global_mutex);
    if (active_transactions == tx) {
        active_transactions = tx->next;
    } else {
        for (STMTransaction* current = active_transactions; current; current = current->next) {
            if (current->next == tx) {
                current->next = tx->next;
                break;
            }
        }
    }
    pthread_mutex_unlock(&stm_global_mutex);
    
    // Clean up logs
    stm_destroy_log(tx->read_log);
    stm_destroy_log(tx->write_log);
    
    // Clean up accessed variables
    free(tx->accessed_vars);
    
    free(tx);
}

// Read/Write Distinction and Access Pattern Optimization

// Access pattern detection and optimization
static void analyze_access_pattern(SharedVariable* var) {
    if (!var->config.enable_statistics) return;
    
    uint64_t total_ops = var->stats.total_reads + var->stats.total_writes;
    if (total_ops < 100) return; // Not enough data yet
    
    double read_ratio = (double)var->stats.total_reads / total_ops;
    double write_ratio = (double)var->stats.total_writes / total_ops;
    
    AccessPattern old_pattern = var->config.access_pattern;
    AccessPattern new_pattern = ACCESS_PATTERN_BALANCED;
    
    if (read_ratio > 0.8) {
        new_pattern = ACCESS_PATTERN_READ_HEAVY;
    } else if (write_ratio > 0.8) {
        new_pattern = ACCESS_PATTERN_WRITE_HEAVY;
    } else if (read_ratio > 0.6) {
        new_pattern = ACCESS_PATTERN_READ_HEAVY;
    } else if (write_ratio > 0.6) {
        new_pattern = ACCESS_PATTERN_WRITE_HEAVY;
    }
    
    // Switch synchronization mode if pattern changed significantly
    if (old_pattern != new_pattern && var->config.sync_mode == SYNC_MODE_ADAPTIVE) {
        var->config.access_pattern = new_pattern;
        
        // Adjust synchronization strategy based on access pattern
        switch (new_pattern) {
            case ACCESS_PATTERN_READ_HEAVY:
                // Switch to reader-writer locks for read optimization
                if (var->sync_state.adaptive.current_mode != SYNC_MODE_RW_LOCK) {
                    pthread_rwlock_init(&var->sync_state.adaptive.sync.rwlock, NULL);
                    var->sync_state.adaptive.current_mode = SYNC_MODE_RW_LOCK;
                    var->stats.adaptive_mode_switches++;
                }
                break;
                
            case ACCESS_PATTERN_WRITE_HEAVY:
                // Switch to mutex for write optimization
                if (var->sync_state.adaptive.current_mode != SYNC_MODE_MUTEX) {
                    pthread_mutex_init(&var->sync_state.adaptive.sync.mutex, NULL);
                    var->sync_state.adaptive.current_mode = SYNC_MODE_MUTEX;
                    var->stats.adaptive_mode_switches++;
                }
                break;
                
            case ACCESS_PATTERN_BALANCED:
                // Use atomic operations for balanced workload
                if (var->sync_state.adaptive.current_mode != SYNC_MODE_ATOMIC) {
                    var->sync_state.adaptive.current_mode = SYNC_MODE_ATOMIC;
                    var->stats.adaptive_mode_switches++;
                }
                break;
                
            default:
                break;
        }
        
        var->sync_state.adaptive.last_adaptation_time = get_current_time_ns();
    }
}

// Optimized read operations for read-heavy patterns
Result_int32_t shared_var_read_optimized_int32(SharedVariable* var) {
    if (!var || var->config.type != SHARED_TYPE_INT32) {
        return (Result_int32_t){.is_error = true, .error = malloc(sizeof(Error))};
    }
    
    // For read-heavy patterns, use reader-writer locks preferentially
    if (var->config.access_pattern == ACCESS_PATTERN_READ_HEAVY && 
        var->config.sync_mode == SYNC_MODE_RW_LOCK) {
        
        update_read_stats(var);
        
        pthread_rwlock_rdlock(&var->sync_state.rwlock.rwlock);
        int32_t value = *(int32_t*)var->sync_state.rwlock.value;
        pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
        
        // Periodically analyze access pattern
        if (var->stats.total_reads % 50 == 0) {
            analyze_access_pattern(var);
        }
        
        return (Result_int32_t){.is_error = false, .value = value};
    }
    
    // Fall back to normal read operation
    return shared_var_get_int32(var);
}

Result_int64_t shared_var_read_optimized_int64(SharedVariable* var) {
    if (!var || var->config.type != SHARED_TYPE_INT64) {
        return (Result_int64_t){.is_error = true, .error = malloc(sizeof(Error))};
    }
    
    if (var->config.access_pattern == ACCESS_PATTERN_READ_HEAVY && 
        var->config.sync_mode == SYNC_MODE_RW_LOCK) {
        
        update_read_stats(var);
        
        pthread_rwlock_rdlock(&var->sync_state.rwlock.rwlock);
        int64_t value = *(int64_t*)var->sync_state.rwlock.value;
        pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
        
        if (var->stats.total_reads % 50 == 0) {
            analyze_access_pattern(var);
        }
        
        return (Result_int64_t){.is_error = false, .value = value};
    }
    
    return shared_var_get_int64(var);
}

// Bulk read operations for efficiency
typedef struct {
    SharedVariable* var;
    void* buffer;
    size_t buffer_size;
    bool success;
    Error* error;
} BulkReadOperation;

Result_void_ptr shared_var_bulk_read(BulkReadOperation* operations, size_t count) {
    if (!operations || count == 0) {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_INVALID_PARAMETER;
        snprintf(error->message, sizeof(error->message), "Invalid bulk read parameters");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    // Sort operations by synchronization type for optimal locking order
    // This is a simple implementation - in practice, would use more sophisticated sorting
    
    for (size_t i = 0; i < count; i++) {
        BulkReadOperation* op = &operations[i];
        op->success = false;
        op->error = NULL;
        
        if (!op->var || !op->buffer) {
            op->error = malloc(sizeof(Error));
            op->error->code = ERROR_INVALID_PARAMETER;
            snprintf(op->error->message, sizeof(op->error->message), "Invalid operation parameters");
            continue;
        }
        
        // Perform optimized read based on variable type and access pattern
        switch (op->var->config.type) {
            case SHARED_TYPE_INT32: {
                Result_int32_t result = shared_var_read_optimized_int32(op->var);
                if (result.is_error) {
                    op->error = result.error;
                } else {
                    *(int32_t*)op->buffer = result.value;
                    op->success = true;
                }
                break;
            }
            case SHARED_TYPE_INT64: {
                Result_int64_t result = shared_var_read_optimized_int64(op->var);
                if (result.is_error) {
                    op->error = result.error;
                } else {
                    *(int64_t*)op->buffer = result.value;
                    op->success = true;
                }
                break;
            }
            case SHARED_TYPE_STRING: {
                Result_void_ptr result = shared_var_get_string(op->var, (char*)op->buffer, op->buffer_size);
                if (result.is_error) {
                    op->error = result.error;
                } else {
                    op->success = true;
                }
                break;
            }
            default: {
                op->error = malloc(sizeof(Error));
                op->error->code = ERROR_UNSUPPORTED_OPERATION;
                snprintf(op->error->message, sizeof(op->error->message), "Unsupported type for bulk read");
                break;
            }
        }
    }
    
    return OK_PTR(operations);
}

// Read/write operation batching for complex access patterns
typedef struct {
    enum {
        RW_OP_READ,
        RW_OP_WRITE
    } operation_type;
    
    SharedVariable* var;
    
    union {
        struct {
            void* buffer;
            size_t buffer_size;
        } read;
        
        struct {
            const void* value;
            size_t value_size;
        } write;
    } params;
    
    bool success;
    Error* error;
} ReadWriteOperation;

Result_void_ptr shared_var_batch_read_write(ReadWriteOperation* operations, size_t count) {
    if (!operations || count == 0) {
        Error* error = malloc(sizeof(Error));
        error->code = ERROR_INVALID_PARAMETER;
        snprintf(error->message, sizeof(error->message), "Invalid batch operation parameters");
        return (Result_void_ptr){.is_error = true, .error = error};
    }
    
    // Process operations in order, optimizing for read/write patterns
    for (size_t i = 0; i < count; i++) {
        ReadWriteOperation* op = &operations[i];
        op->success = false;
        op->error = NULL;
        
        if (!op->var) {
            op->error = malloc(sizeof(Error));
            op->error->code = ERROR_INVALID_PARAMETER;
            snprintf(op->error->message, sizeof(op->error->message), "Invalid variable");
            continue;
        }
        
        if (op->operation_type == RW_OP_READ) {
            // Perform optimized read
            switch (op->var->config.type) {
                case SHARED_TYPE_INT32: {
                    Result_int32_t result = shared_var_read_optimized_int32(op->var);
                    if (result.is_error) {
                        op->error = result.error;
                    } else {
                        *(int32_t*)op->params.read.buffer = result.value;
                        op->success = true;
                    }
                    break;
                }
                case SHARED_TYPE_INT64: {
                    Result_int64_t result = shared_var_read_optimized_int64(op->var);
                    if (result.is_error) {
                        op->error = result.error;
                    } else {
                        *(int64_t*)op->params.read.buffer = result.value;
                        op->success = true;
                    }
                    break;
                }
                default: {
                    op->error = malloc(sizeof(Error));
                    op->error->code = ERROR_UNSUPPORTED_OPERATION;
                    snprintf(op->error->message, sizeof(op->error->message), "Unsupported type for batch read");
                    break;
                }
            }
        } else if (op->operation_type == RW_OP_WRITE) {
            // Perform write operation
            switch (op->var->config.type) {
                case SHARED_TYPE_INT32: {
                    Result_void_ptr result = shared_var_set_int32(op->var, *(int32_t*)op->params.write.value);
                    if (result.is_error) {
                        op->error = result.error;
                    } else {
                        op->success = true;
                        // Trigger access pattern analysis after writes
                        analyze_access_pattern(op->var);
                    }
                    break;
                }
                case SHARED_TYPE_INT64: {
                    Result_void_ptr result = shared_var_set_int64(op->var, *(int64_t*)op->params.write.value);
                    if (result.is_error) {
                        op->error = result.error;
                    } else {
                        op->success = true;
                        analyze_access_pattern(op->var);
                    }
                    break;
                }
                default: {
                    op->error = malloc(sizeof(Error));
                    op->error->code = ERROR_UNSUPPORTED_OPERATION;
                    snprintf(op->error->message, sizeof(op->error->message), "Unsupported type for batch write");
                    break;
                }
            }
        }
    }
    
    return OK_PTR(operations);
}