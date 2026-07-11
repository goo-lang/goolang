#include "../../include/shared_variables.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

// Helper functions
static size_t min(size_t a, size_t b) {
    return a < b ? a : b;
}
static uint64_t get_monotonic_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t generate_unique_id(void) {
    static atomic_uint_fast64_t counter = 1;
    return atomic_fetch_add(&counter, 1);
}

// Configuration helpers
SharedVarConfig shared_var_config_default(const char* name, SharedVarType type) {
    return (SharedVarConfig) {
        .name = name,
        .type = type,
        .sync_mode = SYNC_MODE_ADAPTIVE,
        .access_pattern = ACCESS_PATTERN_UNKNOWN,
        .consistency = CONSISTENCY_SEQUENTIAL,
        .initial_capacity = 0,
        .enable_statistics = true,
        .enable_contention_detection = true,
        .use_memory_pool = true,
        .alignment_requirement = 0,
        .custom_ops = NULL,
        .custom_sync_state = NULL,
        .validate_value = NULL,
        .transform_on_write = NULL,
        .transform_context = NULL
    };
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
    config.consistency = CONSISTENCY_LINEARIZABLE;
    return config;
}

SharedVarConfig shared_var_config_high_contention(const char* name, SharedVarType type) {
    SharedVarConfig config = shared_var_config_default(name, type);
    config.sync_mode = SYNC_MODE_ADAPTIVE;
    config.access_pattern = ACCESS_PATTERN_BALANCED;
    config.consistency = CONSISTENCY_SEQUENTIAL;
    config.enable_contention_detection = true;
    return config;
}

// Optimization thread for adaptive synchronization
static void* optimization_thread(void* arg) {
    SharedVarManager* manager = (SharedVarManager*)arg;
    
    while (manager->optimization_enabled) {
        usleep(manager->optimization_interval_ms * 1000);
        
        pthread_mutex_lock(&manager->registry_mutex);
        
        // Analyze each variable for optimization opportunities
        for (size_t i = 0; i < manager->variable_count; i++) {
            SharedVariable* var = manager->variables[i];
            if (!var) continue;
            
            SharedVarStats* stats = &var->stats;
            
            // Check if we should switch synchronization mode
            if (var->config.sync_mode == SYNC_MODE_ADAPTIVE) {
                SyncMode new_mode = var->sync_state.adaptive.current_mode;
                
                // High contention -> try mutex or rwlock
                if (stats->contention_events > 100) {
                    if (stats->total_reads > stats->total_writes * 3) {
                        new_mode = SYNC_MODE_RW_LOCK;
                    } else {
                        new_mode = SYNC_MODE_MUTEX;
                    }
                }
                // Low contention -> try atomic
                else if (stats->contention_events < 10) {
                    new_mode = SYNC_MODE_ATOMIC;
                }
                
                // Switch mode if needed
                if (new_mode != var->sync_state.adaptive.current_mode) {
                    var->sync_state.adaptive.current_mode = new_mode;
                    var->sync_state.adaptive.last_adaptation_time = get_monotonic_time_ns();
                    stats->adaptive_mode_switches++;
                    manager->total_optimizations++;
                }
            }
        }
        
        pthread_mutex_unlock(&manager->registry_mutex);
    }
    
    return NULL;
}

// Manager operations
SharedVarManager* shared_var_manager_create(size_t max_variables) {
    SharedVarManager* manager = xcalloc(1, sizeof(SharedVarManager));
    if (!manager) return NULL;
    
    manager->max_variables = max_variables;
    manager->enable_global_statistics = true;
    manager->enable_automatic_optimization = true;
    manager->optimization_interval_ms = 1000; // 1 second
    
    // Initialize registry
    manager->variable_capacity = max_variables;
    manager->variables = calloc(manager->variable_capacity, sizeof(SharedVariable*));
    if (!manager->variables) {
        free(manager);
        return NULL;
    }
    
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
    
    // Initialize memory pool
    manager->pool_size = 1024 * 1024; // 1MB default
    manager->memory_pool = malloc(manager->pool_size);
    if (!manager->memory_pool) {
        free(manager->sync_groups);
        pthread_mutex_destroy(&manager->registry_mutex);
        free(manager->variables);
        free(manager);
        return NULL;
    }
    
    if (pthread_mutex_init(&manager->pool_mutex, NULL) != 0) {
        free(manager->memory_pool);
        free(manager->sync_groups);
        pthread_mutex_destroy(&manager->registry_mutex);
        free(manager->variables);
        free(manager);
        return NULL;
    }
    
    return manager;
}

Result_void_ptr shared_var_manager_start_optimization(SharedVarManager* manager) {
    if (!manager) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid shared variable manager"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    if (manager->optimization_enabled) {
        return OK_PTR(NULL); // Already running
    }
    
    manager->optimization_enabled = true;
    
    if (pthread_create(&manager->optimizer_thread, NULL, optimization_thread, manager) != 0) {
        manager->optimization_enabled = false;
        
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INTERNAL,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Failed to start optimization thread"),
            .hint = strdup("Check system thread limits"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    return OK_PTR(NULL);
}

Result_void_ptr shared_var_manager_stop_optimization(SharedVarManager* manager) {
    if (!manager || !manager->optimization_enabled) {
        return OK_PTR(NULL);
    }
    
    manager->optimization_enabled = false;
    pthread_join(manager->optimizer_thread, NULL);
    
    return OK_PTR(NULL);
}

void shared_var_manager_destroy(SharedVarManager* manager) {
    if (!manager) return;
    
    // Stop optimization thread
    shared_var_manager_stop_optimization(manager);
    
    // Destroy all variables
    if (manager->variables) {
        for (size_t i = 0; i < manager->variable_count; i++) {
            if (manager->variables[i]) {
                shared_var_destroy(manager->variables[i]);
            }
        }
        free(manager->variables);
    }
    
    // Destroy all sync groups
    if (manager->sync_groups) {
        for (size_t i = 0; i < manager->sync_group_count; i++) {
            if (manager->sync_groups[i]) {
                sync_group_destroy(manager->sync_groups[i]);
            }
        }
        free(manager->sync_groups);
    }
    
    // Clean up memory pool
    free(manager->memory_pool);
    
    // Destroy mutexes
    pthread_mutex_destroy(&manager->registry_mutex);
    pthread_mutex_destroy(&manager->pool_mutex);
    
    free(manager);
}

// Shared variable creation
static Result_void_ptr init_sync_state(SharedVariable* var) {
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
            switch (var->config.type) {
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
                    goto unsupported_atomic;
            }
            break;
            
        unsupported_atomic:
        case SYNC_MODE_MUTEX:
            if (pthread_mutex_init(&var->sync_state.mutex.mutex, NULL) != 0) {
                Error* error = xmalloc(sizeof(Error));
                *error = (Error){
                    .code = ERROR_INTERNAL,
                    .severity = ERROR_SEVERITY_ERROR,
                    .category = ERROR_CATEGORY_INTERNAL,
                    .message = strdup("Failed to initialize mutex"),
                    .hint = NULL,
                    .location = (SourceLocation){0},
                    .next = NULL
                };
                return ERR_PTR(error);
            }
            
            // Allocate value storage
            var->sync_state.mutex.value = calloc(1, var->value_size);
            if (!var->sync_state.mutex.value) {
                pthread_mutex_destroy(&var->sync_state.mutex.mutex);
                Error* error = xmalloc(sizeof(Error));
                *error = (Error){
                    .code = ERROR_OUT_OF_MEMORY,
                    .severity = ERROR_SEVERITY_ERROR,
                    .category = ERROR_CATEGORY_INTERNAL,
                    .message = strdup("Failed to allocate value storage"),
                    .hint = NULL,
                    .location = (SourceLocation){0},
                    .next = NULL
                };
                return ERR_PTR(error);
            }
            break;
            
        case SYNC_MODE_RW_LOCK:
            if (pthread_rwlock_init(&var->sync_state.rwlock.rwlock, NULL) != 0) {
                Error* error = xmalloc(sizeof(Error));
                *error = (Error){
                    .code = ERROR_INTERNAL,
                    .severity = ERROR_SEVERITY_ERROR,
                    .category = ERROR_CATEGORY_INTERNAL,
                    .message = strdup("Failed to initialize rwlock"),
                    .hint = NULL,
                    .location = (SourceLocation){0},
                    .next = NULL
                };
                return ERR_PTR(error);
            }
            
            var->sync_state.rwlock.value = calloc(1, var->value_size);
            if (!var->sync_state.rwlock.value) {
                pthread_rwlock_destroy(&var->sync_state.rwlock.rwlock);
                Error* error = xmalloc(sizeof(Error));
                *error = (Error){
                    .code = ERROR_OUT_OF_MEMORY,
                    .severity = ERROR_SEVERITY_ERROR,
                    .category = ERROR_CATEGORY_INTERNAL,
                    .message = strdup("Failed to allocate value storage"),
                    .hint = NULL,
                    .location = (SourceLocation){0},
                    .next = NULL
                };
                return ERR_PTR(error);
            }
            break;
            
        case SYNC_MODE_SPIN_LOCK:
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            var->sync_state.spinlock.value = calloc(1, var->value_size);
            if (!var->sync_state.spinlock.value) {
                Error* error = xmalloc(sizeof(Error));
                *error = (Error){
                    .code = ERROR_OUT_OF_MEMORY,
                    .severity = ERROR_SEVERITY_ERROR,
                    .category = ERROR_CATEGORY_INTERNAL,
                    .message = strdup("Failed to allocate value storage"),
                    .hint = NULL,
                    .location = (SourceLocation){0},
                    .next = NULL
                };
                return ERR_PTR(error);
            }
            break;
            
        case SYNC_MODE_ADAPTIVE:
            var->sync_state.adaptive.current_mode = SYNC_MODE_ATOMIC;
            var->sync_state.adaptive.contention_count = 0;
            var->sync_state.adaptive.last_adaptation_time = get_monotonic_time_ns();
            
            // Initialize with atomic mode first
            switch (var->config.type) {
                case SHARED_TYPE_INT32:
                case SHARED_TYPE_INT64:
                case SHARED_TYPE_UINT32:
                case SHARED_TYPE_UINT64:
                case SHARED_TYPE_BOOL:
                    atomic_init(&var->sync_state.adaptive.sync.atomic_value, 0);
                    break;
                default:
                    // Fall back to mutex for complex types
                    var->sync_state.adaptive.current_mode = SYNC_MODE_MUTEX;
                    if (pthread_mutex_init(&var->sync_state.adaptive.sync.mutex, NULL) != 0) {
                        Error* error = xmalloc(sizeof(Error));
                        *error = (Error){
                            .code = ERROR_INTERNAL,
                            .severity = ERROR_SEVERITY_ERROR,
                            .category = ERROR_CATEGORY_INTERNAL,
                            .message = strdup("Failed to initialize adaptive mutex"),
                            .hint = NULL,
                            .location = (SourceLocation){0},
                            .next = NULL
                        };
                        return ERR_PTR(error);
                    }
                    var->sync_state.adaptive.value = calloc(1, var->value_size);
                    if (!var->sync_state.adaptive.value) {
                        pthread_mutex_destroy(&var->sync_state.adaptive.sync.mutex);
                        Error* error = xmalloc(sizeof(Error));
                        *error = (Error){
                            .code = ERROR_OUT_OF_MEMORY,
                            .severity = ERROR_SEVERITY_ERROR,
                            .category = ERROR_CATEGORY_INTERNAL,
                            .message = strdup("Failed to allocate adaptive value storage"),
                            .hint = NULL,
                            .location = (SourceLocation){0},
                            .next = NULL
                        };
                        return ERR_PTR(error);
                    }
                    break;
            }
            break;
            
        case SYNC_MODE_CUSTOM:
            if (!var->config.custom_ops) {
                Error* error = xmalloc(sizeof(Error));
                *error = (Error){
                    .code = ERROR_INVALID_EXPRESSION,
                    .severity = ERROR_SEVERITY_ERROR,
                    .category = ERROR_CATEGORY_INTERNAL,
                    .message = strdup("Custom sync mode requires custom operations"),
                    .hint = strdup("Provide CustomSyncOps in configuration"),
                    .location = (SourceLocation){0},
                    .next = NULL
                };
                return ERR_PTR(error);
            }
            
            var->sync_state.custom.sync_state = var->config.custom_sync_state;
            var->sync_state.custom.ops = var->config.custom_ops;
            var->sync_state.custom.value = calloc(1, var->value_size);
            if (!var->sync_state.custom.value) {
                Error* error = xmalloc(sizeof(Error));
                *error = (Error){
                    .code = ERROR_OUT_OF_MEMORY,
                    .severity = ERROR_SEVERITY_ERROR,
                    .category = ERROR_CATEGORY_INTERNAL,
                    .message = strdup("Failed to allocate custom value storage"),
                    .hint = NULL,
                    .location = (SourceLocation){0},
                    .next = NULL
                };
                return ERR_PTR(error);
            }
            break;
            
        default:
            Error* error = xmalloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_INVALID_EXPRESSION,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Unsupported synchronization mode"),
                .hint = NULL,
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
    }
    
    return OK_PTR(NULL);
}

SharedVariable* shared_var_create(SharedVarManager* manager, SharedVarConfig config) {
    if (!manager || !config.name) return NULL;
    
    pthread_mutex_lock(&manager->registry_mutex);
    
    if (manager->variable_count >= manager->variable_capacity) {
        pthread_mutex_unlock(&manager->registry_mutex);
        return NULL;
    }
    
    SharedVariable* var = xcalloc(1, sizeof(SharedVariable));
    if (!var) {
        pthread_mutex_unlock(&manager->registry_mutex);
        return NULL;
    }
    
    // Initialize basic properties
    var->id = generate_unique_id();
    strncpy(var->name, config.name, sizeof(var->name) - 1);
    var->name[sizeof(var->name) - 1] = '\0';
    var->config = config;
    var->manager = manager;
    
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
            var->value_size = sizeof(char*);
            break;
        case SHARED_TYPE_CUSTOM:
            var->value_size = config.initial_capacity > 0 ? config.initial_capacity : 64;
            break;
    }
    
    var->version = 1;
    var->creation_time = get_monotonic_time_ns();
    var->last_access_time = var->creation_time;
    atomic_init(&var->ref_count, 1);
    
    // Initialize synchronization state
    Result_void_ptr init_result = init_sync_state(var);
    if (init_result.is_error) {
        free(var);
        pthread_mutex_unlock(&manager->registry_mutex);
        return NULL;
    }
    
    // Add to manager
    manager->variables[manager->variable_count++] = var;
    
    pthread_mutex_unlock(&manager->registry_mutex);
    
    return var;
}

void shared_var_destroy(SharedVariable* var) {
    if (!var) return;
    
    // Cleanup synchronization state
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
                free(var->sync_state.adaptive.value);
            } else if (var->sync_state.adaptive.current_mode == SYNC_MODE_RW_LOCK) {
                pthread_rwlock_destroy(&var->sync_state.adaptive.sync.rwlock);
                free(var->sync_state.adaptive.value);
            }
            break;
            
        case SYNC_MODE_CUSTOM:
            if (var->config.custom_ops && var->config.custom_ops->destroy) {
                var->config.custom_ops->destroy(var->sync_state.custom.sync_state);
            }
            free(var->sync_state.custom.value);
            break;
            
        case SYNC_MODE_ATOMIC:
        case SYNC_MODE_WAIT_FREE:
            // No cleanup needed for atomic operations
            break;
    }
    
    free(var);
}

SharedVariable* shared_var_find_by_name(SharedVarManager* manager, const char* name) {
    if (!manager || !name) return NULL;
    
    pthread_mutex_lock(&manager->registry_mutex);
    
    for (size_t i = 0; i < manager->variable_count; i++) {
        if (manager->variables[i] && strcmp(manager->variables[i]->name, name) == 0) {
            SharedVariable* found = manager->variables[i];
            pthread_mutex_unlock(&manager->registry_mutex);
            return found;
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
            SharedVariable* found = manager->variables[i];
            pthread_mutex_unlock(&manager->registry_mutex);
            return found;
        }
    }
    
    pthread_mutex_unlock(&manager->registry_mutex);
    return NULL;
}

// Synchronization groups
SyncGroup* sync_group_create(SharedVarManager* manager, const char* name, ConsistencyLevel consistency) {
    if (!manager || !name) return NULL;
    
    pthread_mutex_lock(&manager->registry_mutex);
    
    if (manager->sync_group_count >= manager->sync_group_capacity) {
        pthread_mutex_unlock(&manager->registry_mutex);
        return NULL;
    }
    
    SyncGroup* group = xcalloc(1, sizeof(SyncGroup));
    if (!group) {
        pthread_mutex_unlock(&manager->registry_mutex);
        return NULL;
    }
    
    group->id = generate_unique_id();
    strncpy(group->name, name, sizeof(group->name) - 1);
    group->name[sizeof(group->name) - 1] = '\0';
    group->consistency_level = consistency;
    group->supports_transactions = true;
    
    if (pthread_mutex_init(&group->group_mutex, NULL) != 0) {
        free(group);
        pthread_mutex_unlock(&manager->registry_mutex);
        return NULL;
    }
    
    group->variable_capacity = 50;
    group->variables = calloc(group->variable_capacity, sizeof(SharedVariable*));
    if (!group->variables) {
        pthread_mutex_destroy(&group->group_mutex);
        free(group);
        pthread_mutex_unlock(&manager->registry_mutex);
        return NULL;
    }
    
    manager->sync_groups[manager->sync_group_count++] = group;
    
    pthread_mutex_unlock(&manager->registry_mutex);
    
    return group;
}

void sync_group_destroy(SyncGroup* group) {
    if (!group) return;
    
    pthread_mutex_destroy(&group->group_mutex);
    free(group->variables);
    free(group);
}

Result_void_ptr sync_group_add_variable(SyncGroup* group, SharedVariable* var) {
    if (!group || !var) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid sync group or variable"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&group->group_mutex);
    
    if (group->variable_count >= group->variable_capacity) {
        pthread_mutex_unlock(&group->group_mutex);
        
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INTERNAL,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Sync group is at capacity"),
            .hint = strdup("Create a new sync group or increase capacity"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    group->variables[group->variable_count++] = var;
    
    pthread_mutex_unlock(&group->group_mutex);
    
    return OK_PTR(NULL);
}

// Atomic operations implementation
Result_int32_t shared_var_get_int32(SharedVariable* var) {
    if (!var || var->config.type != SHARED_TYPE_INT32) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable or type mismatch"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_INT32"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return (Result_int32_t){.is_error = true, .error = error};
    }
    
    uint64_t start_time = get_monotonic_time_ns();
    var->last_access_time = start_time;
    var->stats.total_reads++;
    
    int32_t value;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
            value = atomic_load(&var->sync_state.atomic.value_i32);
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
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock)) {
                // Spin
            }
            value = *(int32_t*)var->sync_state.spinlock.value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                value = (int32_t)atomic_load(&var->sync_state.adaptive.sync.atomic_value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                value = *(int32_t*)var->sync_state.adaptive.value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        default:
            Error* error = xmalloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_INVALID_EXPRESSION,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Unsupported synchronization mode for int32"),
                .hint = NULL,
                .location = (SourceLocation){0},
                .next = NULL
            };
            return (Result_int32_t){.is_error = true, .error = error};
    }
    
    uint64_t end_time = get_monotonic_time_ns();
    var->stats.avg_read_time_ns = (var->stats.avg_read_time_ns + (end_time - start_time)) / 2;
    
    return (Result_int32_t){.is_error = false, .value = value};
}

Result_void_ptr shared_var_set_int32(SharedVariable* var, int32_t value) {
    if (!var || var->config.type != SHARED_TYPE_INT32) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable or type mismatch"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_INT32"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    uint64_t start_time = get_monotonic_time_ns();
    var->last_access_time = start_time;
    var->stats.total_writes++;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
            atomic_store(&var->sync_state.atomic.value_i32, value);
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
            while (atomic_flag_test_and_set(&var->sync_state.spinlock.spinlock)) {
                // Spin
            }
            *(int32_t*)var->sync_state.spinlock.value = value;
            atomic_flag_clear(&var->sync_state.spinlock.spinlock);
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
            
        default:
            Error* error = xmalloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_INVALID_EXPRESSION,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Unsupported synchronization mode for int32"),
                .hint = NULL,
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
    }
    
    var->version++;
    
    uint64_t end_time = get_monotonic_time_ns();
    var->stats.avg_write_time_ns = (var->stats.avg_write_time_ns + (end_time - start_time)) / 2;
    
    return OK_PTR(NULL);
}

Result_bool shared_var_cas_int32(SharedVariable* var, int32_t expected, int32_t desired) {
    if (!var || var->config.type != SHARED_TYPE_INT32) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable or type mismatch"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_INT32"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return (Result_bool){.is_error = true, .error = error};
    }
    
    var->stats.total_cas_attempts++;
    bool success = false;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
            success = atomic_compare_exchange_strong(&var->sync_state.atomic.value_i32, &expected, desired);
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            if (*(int32_t*)var->sync_state.mutex.value == expected) {
                *(int32_t*)var->sync_state.mutex.value = desired;
                success = true;
            }
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                int64_t expected_64 = expected;
                success = atomic_compare_exchange_strong(&var->sync_state.adaptive.sync.atomic_value, &expected_64, desired);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                if (*(int32_t*)var->sync_state.adaptive.value == expected) {
                    *(int32_t*)var->sync_state.adaptive.value = desired;
                    success = true;
                }
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        default:
            Error* error = xmalloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_INVALID_EXPRESSION,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("CAS not supported for this synchronization mode"),
                .hint = strdup("Use atomic or adaptive mode for CAS operations"),
                .location = (SourceLocation){0},
                .next = NULL
            };
            return (Result_bool){.is_error = true, .error = error};
    }
    
    if (success) {
        var->stats.successful_cas++;
        var->stats.total_writes++; // CAS is also a write operation when successful
        var->version++;
    }
    
    return (Result_bool){.is_error = false, .value = success};
}

Result_int32_t shared_var_fetch_add_int32(SharedVariable* var, int32_t value) {
    if (!var || var->config.type != SHARED_TYPE_INT32) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable or type mismatch"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_INT32"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return (Result_int32_t){.is_error = true, .error = error};
    }
    
    int32_t old_value;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
            old_value = atomic_fetch_add(&var->sync_state.atomic.value_i32, value);
            break;
            
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            old_value = *(int32_t*)var->sync_state.mutex.value;
            *(int32_t*)var->sync_state.mutex.value += value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
            
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                old_value = (int32_t)atomic_fetch_add(&var->sync_state.adaptive.sync.atomic_value, value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                old_value = *(int32_t*)var->sync_state.adaptive.value;
                *(int32_t*)var->sync_state.adaptive.value += value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
            
        default:
            Error* error = xmalloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_INVALID_EXPRESSION,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Fetch-add not supported for this synchronization mode"),
                .hint = strdup("Use atomic or adaptive mode for fetch-add operations"),
                .location = (SourceLocation){0},
                .next = NULL
            };
            return (Result_int32_t){.is_error = true, .error = error};
    }
    
    var->version++;
    var->stats.total_writes++;
    
    return (Result_int32_t){.is_error = false, .value = old_value};
}

// Additional typed read/write operations for other data types

Result_int64_t shared_var_get_int64(SharedVariable* var) {
    if (!var || var->config.type != SHARED_TYPE_INT64) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable or type mismatch"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_INT64"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return (Result_int64_t){.is_error = true, .error = error};
    }
    
    var->stats.total_reads++;
    int64_t value;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
            value = atomic_load(&var->sync_state.atomic.value_i64);
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
        default:
            value = 0; // Default fallback
            break;
    }
    
    return (Result_int64_t){.is_error = false, .value = value};
}

Result_void_ptr shared_var_set_int64(SharedVariable* var, int64_t value) {
    if (!var || var->config.type != SHARED_TYPE_INT64) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable or type mismatch"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_INT64"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    var->stats.total_writes++;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
            atomic_store(&var->sync_state.atomic.value_i64, value);
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
        default:
            break;
    }
    
    var->version++;
    return OK_PTR(NULL);
}

Result_uint32_t shared_var_get_uint32(SharedVariable* var) {
    if (!var || var->config.type != SHARED_TYPE_UINT32) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable or type mismatch"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_UINT32"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return (Result_uint32_t){.is_error = true, .error = error};
    }
    
    var->stats.total_reads++;
    uint32_t value;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
            value = atomic_load(&var->sync_state.atomic.value_u32);
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
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                value = (uint32_t)atomic_load(&var->sync_state.adaptive.sync.atomic_value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                value = *(uint32_t*)var->sync_state.adaptive.value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
        default:
            value = 0;
            break;
    }
    
    return (Result_uint32_t){.is_error = false, .value = value};
}

Result_void_ptr shared_var_set_uint32(SharedVariable* var, uint32_t value) {
    if (!var || var->config.type != SHARED_TYPE_UINT32) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable or type mismatch"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_UINT32"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    var->stats.total_writes++;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
            atomic_store(&var->sync_state.atomic.value_u32, value);
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
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                atomic_store(&var->sync_state.adaptive.sync.atomic_value, value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                *(uint32_t*)var->sync_state.adaptive.value = value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
        default:
            break;
    }
    
    var->version++;
    return OK_PTR(NULL);
}

Result_bool shared_var_get_bool(SharedVariable* var) {
    if (!var || var->config.type != SHARED_TYPE_BOOL) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable or type mismatch"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_BOOL"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return (Result_bool){.is_error = true, .error = error};
    }
    
    var->stats.total_reads++;
    bool value;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
            value = atomic_load(&var->sync_state.atomic.value_bool);
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
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                value = (bool)atomic_load(&var->sync_state.adaptive.sync.atomic_value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                value = *(bool*)var->sync_state.adaptive.value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
        default:
            value = false;
            break;
    }
    
    return (Result_bool){.is_error = false, .value = value};
}

Result_void_ptr shared_var_set_bool(SharedVariable* var, bool value) {
    if (!var || var->config.type != SHARED_TYPE_BOOL) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable or type mismatch"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_BOOL"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    var->stats.total_writes++;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
            atomic_store(&var->sync_state.atomic.value_bool, value);
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
        case SYNC_MODE_ADAPTIVE:
            if (var->sync_state.adaptive.current_mode == SYNC_MODE_ATOMIC) {
                atomic_store(&var->sync_state.adaptive.sync.atomic_value, value);
            } else {
                pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
                *(bool*)var->sync_state.adaptive.value = value;
                pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            }
            break;
        default:
            break;
    }
    
    var->version++;
    return OK_PTR(NULL);
}

// String operations
Result_void_ptr shared_var_get_string(SharedVariable* var, char* buffer, size_t buffer_size) {
    if (!var || var->config.type != SHARED_TYPE_STRING || !buffer) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable, type mismatch, or null buffer"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_STRING and buffer is not null"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    var->stats.total_reads++;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            if (var->sync_state.mutex.value) {
                strncpy(buffer, (char*)var->sync_state.mutex.value, buffer_size - 1);
                buffer[buffer_size - 1] = '\0';
            } else {
                buffer[0] = '\0';
            }
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_rdlock(&var->sync_state.rwlock.rwlock);
            if (var->sync_state.rwlock.value) {
                strncpy(buffer, (char*)var->sync_state.rwlock.value, buffer_size - 1);
                buffer[buffer_size - 1] = '\0';
            } else {
                buffer[0] = '\0';
            }
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
        case SYNC_MODE_ADAPTIVE:
            pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
            if (var->sync_state.adaptive.value) {
                strncpy(buffer, (char*)var->sync_state.adaptive.value, buffer_size - 1);
                buffer[buffer_size - 1] = '\0';
            } else {
                buffer[0] = '\0';
            }
            pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            break;
        default:
            buffer[0] = '\0';
            break;
    }
    
    return OK_PTR(NULL);
}

Result_void_ptr shared_var_set_string(SharedVariable* var, const char* value) {
    if (!var || var->config.type != SHARED_TYPE_STRING) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable or type mismatch"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_STRING"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    var->stats.total_writes++;
    
    // Handle NULL value as empty string
    const char* safe_value = value ? value : "";
    size_t len = strlen(safe_value) + 1;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            free(var->sync_state.mutex.value);
            var->sync_state.mutex.value = malloc(len);
            if (var->sync_state.mutex.value) {
                strcpy((char*)var->sync_state.mutex.value, safe_value);
            }
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
        case SYNC_MODE_RW_LOCK:
            pthread_rwlock_wrlock(&var->sync_state.rwlock.rwlock);
            free(var->sync_state.rwlock.value);
            var->sync_state.rwlock.value = malloc(len);
            if (var->sync_state.rwlock.value) {
                strcpy((char*)var->sync_state.rwlock.value, safe_value);
            }
            pthread_rwlock_unlock(&var->sync_state.rwlock.rwlock);
            break;
        case SYNC_MODE_ADAPTIVE:
            // Use mutex for adaptive mode - strings don't work well with atomics
            pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
            free(var->sync_state.adaptive.value);
            var->sync_state.adaptive.value = malloc(len);
            if (var->sync_state.adaptive.value) {
                strcpy((char*)var->sync_state.adaptive.value, safe_value);
            }
            pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            break;
        default:
            break;
    }
    
    var->version++;
    return OK_PTR(NULL);
}

// Additional fetch operations
Result_int64_t shared_var_fetch_add_int64(SharedVariable* var, int64_t value) {
    if (!var || var->config.type != SHARED_TYPE_INT64) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable or type mismatch"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_INT64"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return (Result_int64_t){.is_error = true, .error = error};
    }
    
    int64_t old_value;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_ATOMIC:
            old_value = atomic_fetch_add(&var->sync_state.atomic.value_i64, value);
            break;
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            old_value = *(int64_t*)var->sync_state.mutex.value;
            *(int64_t*)var->sync_state.mutex.value += value;
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
        default:
            old_value = 0;
            break;
    }
    
    var->version++;
    var->stats.total_writes++;
    
    return (Result_int64_t){.is_error = false, .value = old_value};
}

// Custom type operations
Result_void_ptr shared_var_get_custom(SharedVariable* var, void* buffer, size_t buffer_size) {
    if (!var || var->config.type != SHARED_TYPE_CUSTOM || !buffer) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable, type mismatch, or null buffer"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_CUSTOM and buffer is not null"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    var->stats.total_reads++;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_CUSTOM:
            if (var->config.custom_ops && var->config.custom_ops->read_begin) {
                var->config.custom_ops->read_begin(var->sync_state.custom.sync_state);
                memcpy(buffer, var->sync_state.custom.value, min(buffer_size, var->value_size));
                var->config.custom_ops->read_end(var->sync_state.custom.sync_state);
            }
            break;
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            memcpy(buffer, var->sync_state.mutex.value, min(buffer_size, var->value_size));
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
        case SYNC_MODE_ADAPTIVE:
            // Use mutex for adaptive mode - custom types need mutex protection
            pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
            memcpy(buffer, var->sync_state.adaptive.value, min(buffer_size, var->value_size));
            pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            break;
        default:
            memset(buffer, 0, buffer_size);
            break;
    }
    
    return OK_PTR(NULL);
}

Result_void_ptr shared_var_set_custom(SharedVariable* var, const void* value, size_t value_size) {
    if (!var || var->config.type != SHARED_TYPE_CUSTOM || !value) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable, type mismatch, or null value"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_CUSTOM and value is not null"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    var->stats.total_writes++;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_CUSTOM:
            if (var->config.custom_ops && var->config.custom_ops->write_begin) {
                var->config.custom_ops->write_begin(var->sync_state.custom.sync_state);
                memcpy(var->sync_state.custom.value, value, min(value_size, var->value_size));
                var->config.custom_ops->write_end(var->sync_state.custom.sync_state);
            }
            break;
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            memcpy(var->sync_state.mutex.value, value, min(value_size, var->value_size));
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
        case SYNC_MODE_ADAPTIVE:
            // Use mutex for adaptive mode - custom types need mutex protection
            pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
            memcpy(var->sync_state.adaptive.value, value, min(value_size, var->value_size));
            pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            break;
        default:
            break;
    }
    
    var->version++;
    return OK_PTR(NULL);
}

Result_bool shared_var_cas_custom(SharedVariable* var, const void* expected, const void* desired, size_t size) {
    if (!var || var->config.type != SHARED_TYPE_CUSTOM || !expected || !desired) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_TYPE_MISMATCH,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid variable, type mismatch, or null values"),
            .hint = strdup("Ensure variable is of type SHARED_TYPE_CUSTOM and values are not null"),
            .location = (SourceLocation){0},
            .next = NULL
        };
        return (Result_bool){.is_error = true, .error = error};
    }
    
    var->stats.total_cas_attempts++;
    bool success = false;
    
    switch (var->config.sync_mode) {
        case SYNC_MODE_MUTEX:
            pthread_mutex_lock(&var->sync_state.mutex.mutex);
            if (memcmp(var->sync_state.mutex.value, expected, size) == 0) {
                memcpy(var->sync_state.mutex.value, desired, size);
                success = true;
            }
            pthread_mutex_unlock(&var->sync_state.mutex.mutex);
            break;
        case SYNC_MODE_ADAPTIVE:
            pthread_mutex_lock(&var->sync_state.adaptive.sync.mutex);
            if (memcmp(var->sync_state.adaptive.value, expected, size) == 0) {
                memcpy(var->sync_state.adaptive.value, desired, size);
                success = true;
            }
            pthread_mutex_unlock(&var->sync_state.adaptive.sync.mutex);
            break;
        default:
            break;
    }
    
    if (success) {
        var->stats.successful_cas++;
        var->version++;
    }
    
    return (Result_bool){.is_error = false, .value = success};
}

// Software Transactional Memory (STM) - Basic implementation
typedef struct STMTransaction {
    bool is_active;
    SharedVariable** variables;
    void** read_values;
    void** write_values;
    uint64_t* read_versions;   // Track versions of read variables for conflict detection
    size_t variable_count;
    size_t read_count;         // Separate count for read variables
    size_t capacity;
} STMTransaction;

STMTransaction* stm_begin_transaction(void) {
    STMTransaction* tx = xcalloc(1, sizeof(STMTransaction));
    if (!tx) return NULL;
    
    tx->is_active = true;
    tx->capacity = 10;
    tx->variables = calloc(tx->capacity, sizeof(SharedVariable*));
    tx->read_values = calloc(tx->capacity, sizeof(void*));
    tx->write_values = calloc(tx->capacity, sizeof(void*));
    tx->read_versions = calloc(tx->capacity, sizeof(uint64_t));
    
    if (!tx->variables || !tx->read_values || !tx->write_values || !tx->read_versions) {
        free(tx->variables);
        free(tx->read_values);
        free(tx->write_values);
        free(tx->read_versions);
        free(tx);
        return NULL;
    }
    
    return tx;
}

Result_void_ptr stm_read(STMTransaction* tx, SharedVariable* var, void* buffer, size_t buffer_size) {
    if (!tx || !var || !buffer || !tx->is_active) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid transaction or parameters"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Suppress unused parameter warning
    (void)buffer_size;
    
    // Record the variable and its version for conflict detection
    if (tx->read_count < tx->capacity) {
        // Check if we already read this variable
        bool already_read = false;
        for (size_t i = 0; i < tx->read_count; i++) {
            if (tx->variables[i] == var) {
                already_read = true;
                break;
            }
        }
        
        if (!already_read) {
            tx->variables[tx->read_count] = var;
            tx->read_versions[tx->read_count] = var->version;
            tx->read_count++;
        }
    }
    
    // Simple STM read - just read current value
    switch (var->config.type) {
        case SHARED_TYPE_INT32: {
            Result_int32_t result = shared_var_get_int32(var);
            if (!result.is_error) {
                *(int32_t*)buffer = result.value;
            }
            break;
        }
        case SHARED_TYPE_INT64: {
            Result_int64_t result = shared_var_get_int64(var);
            if (!result.is_error) {
                *(int64_t*)buffer = result.value;
            }
            break;
        }
        default:
            break;
    }
    
    return OK_PTR(NULL);
}

Result_void_ptr stm_write(STMTransaction* tx, SharedVariable* var, const void* value, size_t value_size) {
    if (!tx || !var || !value || !tx->is_active) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid transaction or parameters"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Simple STM write - defer until commit
    // Note: we're reusing the variables array beyond read_count for writes
    if (tx->variable_count < tx->capacity) {
        tx->variables[tx->variable_count] = var;
        tx->write_values[tx->variable_count] = malloc(value_size);
        if (tx->write_values[tx->variable_count]) {
            memcpy(tx->write_values[tx->variable_count], value, value_size);
            tx->variable_count++;
        }
    }
    
    return OK_PTR(NULL);
}

Result_void_ptr stm_commit(STMTransaction* tx) {
    if (!tx || !tx->is_active) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_INTERNAL,
            .message = strdup("Invalid or inactive transaction"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Check for conflicts - verify that read variables haven't changed
    for (size_t i = 0; i < tx->read_count; i++) {
        SharedVariable* var = tx->variables[i];
        uint64_t read_version = tx->read_versions[i];
        
        if (var && var->version != read_version) {
            // Conflict detected - transaction must be aborted
            Error* error = xmalloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_INVALID_EXPRESSION,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_INTERNAL,
                .message = strdup("Transaction conflict detected - variable modified by another transaction"),
                .hint = strdup("Retry the transaction"),
                .location = (SourceLocation){0},
                .next = NULL
            };
            tx->is_active = false;
            return ERR_PTR(error);
        }
    }
    
    // No conflicts, apply all writes
    for (size_t i = 0; i < tx->variable_count; i++) {
        SharedVariable* var = tx->variables[i];
        void* value = tx->write_values[i];
        
        if (var && value) {
            switch (var->config.type) {
                case SHARED_TYPE_INT32:
                    shared_var_set_int32(var, *(int32_t*)value);
                    break;
                case SHARED_TYPE_INT64:
                    shared_var_set_int64(var, *(int64_t*)value);
                    break;
                default:
                    break;
            }
        }
    }
    
    tx->is_active = false;
    return OK_PTR(NULL);
}

void stm_destroy_transaction(STMTransaction* tx) {
    if (!tx) return;
    
    for (size_t i = 0; i < tx->variable_count; i++) {
        free(tx->write_values[i]);
        free(tx->read_values[i]);
    }
    
    free(tx->variables);
    free(tx->read_values);
    free(tx->write_values);
    free(tx->read_versions);
    free(tx);
}

GlobalSharedVarStats shared_var_manager_get_stats(SharedVarManager* manager) {
    GlobalSharedVarStats stats = {0};
    
    if (!manager) return stats;
    
    pthread_mutex_lock(&manager->registry_mutex);
    
    stats.total_variables = manager->variable_count;
    
    for (size_t i = 0; i < manager->variable_count; i++) {
        SharedVariable* var = manager->variables[i];
        if (!var) continue;
        
        if (var->config.sync_mode != SYNC_MODE_CUSTOM) {
            stats.active_variables++;
        }
        
        stats.total_operations += var->stats.total_reads + var->stats.total_writes;
        stats.total_contentions += var->stats.contention_events;
        
        // Count by synchronization mode
        switch (var->config.sync_mode) {
            case SYNC_MODE_ATOMIC:
                stats.atomic_operations += var->stats.total_reads + var->stats.total_writes;
                break;
            case SYNC_MODE_MUTEX:
                stats.mutex_operations += var->stats.total_reads + var->stats.total_writes;
                break;
            case SYNC_MODE_RW_LOCK:
                stats.rwlock_operations += var->stats.total_reads + var->stats.total_writes;
                break;
            case SYNC_MODE_ADAPTIVE:
                stats.adaptive_operations += var->stats.total_reads + var->stats.total_writes;
                break;
            default:
                break;
        }
        
        stats.memory_usage_bytes += var->stats.current_memory_usage;
    }
    
    stats.optimization_events = manager->total_optimizations;
    
    if (stats.total_operations > 0) {
        stats.contention_rate = (double)stats.total_contentions / stats.total_operations;
    }
    
    pthread_mutex_unlock(&manager->registry_mutex);
    
    return stats;
}

void shared_var_manager_reset_stats(SharedVarManager* manager) {
    if (!manager) return;
    
    pthread_mutex_lock(&manager->registry_mutex);
    
    for (size_t i = 0; i < manager->variable_count; i++) {
        if (manager->variables[i]) {
            shared_var_reset_stats(manager->variables[i]);
        }
    }
    
    manager->total_operations = 0;
    manager->total_contentions = 0;
    manager->total_optimizations = 0;
    
    pthread_mutex_unlock(&manager->registry_mutex);
}

// Statistics and monitoring
SharedVarStats shared_var_get_stats(SharedVariable* var) {
    if (!var) {
        return (SharedVarStats){0};
    }
    
    return var->stats;
}

void shared_var_reset_stats(SharedVariable* var) {
    if (!var) return;
    
    memset(&var->stats, 0, sizeof(SharedVarStats));
}