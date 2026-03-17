#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include "hot_reload.h"
#include "panic_free.h"

// =============================================================================
// Helper Functions
// =============================================================================

static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

static HotReloadableFunction* find_function(HotReloadContext* ctx, const char* name) {
    HotReloadableFunction* func = ctx->functions;
    while (func) {
        if (strcmp(func->name, name) == 0) {
            return func;
        }
        func = func->next;
    }
    return NULL;
}

static HotReloadableType* find_type(HotReloadContext* ctx, const char* name) {
    HotReloadableType* type = ctx->types;
    while (type) {
        if (strcmp(type->name, name) == 0) {
            return type;
        }
        type = type->next;
    }
    return NULL;
}

static FileWatcher* find_watcher(HotReloadContext* ctx, const char* path) {
    FileWatcher* watcher = ctx->file_watchers;
    while (watcher) {
        if (strcmp(watcher->watched_path, path) == 0) {
            return watcher;
        }
        watcher = watcher->next;
    }
    return NULL;
}

// =============================================================================
// Context Management
// =============================================================================

HotReloadContext* hot_reload_context_new(void) {
    HotReloadContext* ctx = calloc(1, sizeof(HotReloadContext));
    if (!ctx) return NULL;
    
    // Initialize mutex
    ctx->mutex = malloc(sizeof(pthread_mutex_t));
    if (!ctx->mutex) {
        free(ctx);
        return NULL;
    }
    pthread_mutex_init((pthread_mutex_t*)ctx->mutex, NULL);
    
    // Set defaults
    ctx->max_preserved_states = 10;
    ctx->reload_timeout = 5.0; // 5 seconds
    
    // Initialize compiler
    ctx->compiler = calloc(1, sizeof(IncrementalCompiler));
    if (ctx->compiler) {
        ctx->compiler->project_root = str_dup(".");
        ctx->compiler->build_dir = str_dup("build/hot_reload");
    }
    
    // Initialize platform-specific components
    hot_reload_platform_init();
    
    return ctx;
}

void hot_reload_context_free(HotReloadContext* ctx) {
    if (!ctx) return;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    // Free functions
    HotReloadableFunction* func = ctx->functions;
    while (func) {
        HotReloadableFunction* next = func->next;
        free(func->name);
        free(func->module_path);
        // Note: Don't free AST nodes or implementations - they're owned elsewhere
        free(func);
        func = next;
    }
    
    // Free types
    HotReloadableType* type = ctx->types;
    while (type) {
        HotReloadableType* next = type->next;
        free(type->name);
        free(type->module_path);
        free(type);
        type = next;
    }
    
    // Free preserved states
    StatePreservation* state = ctx->preserved_states;
    while (state) {
        StatePreservation* next = state->next;
        free(state->state_data);
        free(state->type_signature);
        if (state->field_names) {
            for (size_t i = 0; i < state->field_count; i++) {
                free(state->field_names[i]);
            }
            free(state->field_names);
        }
        free(state->field_types);
        free(state);
        state = next;
    }
    
    // Free file watchers
    FileWatcher* watcher = ctx->file_watchers;
    while (watcher) {
        FileWatcher* next = watcher->next;
        if (watcher->platform_handle) {
            hot_reload_platform_unwatch_file(watcher->platform_handle);
        }
        free(watcher->watched_path);
        free(watcher);
        watcher = next;
    }
    
    // Free compiler
    if (ctx->compiler) {
        free(ctx->compiler->project_root);
        free(ctx->compiler->build_dir);
        free(ctx->compiler->last_error);
        if (ctx->compiler->source_files) {
            for (size_t i = 0; i < ctx->compiler->source_count; i++) {
                free(ctx->compiler->source_files[i]);
            }
            free(ctx->compiler->source_files);
        }
        free(ctx->compiler->last_compiled);
        free(ctx->compiler);
    }
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    pthread_mutex_destroy((pthread_mutex_t*)ctx->mutex);
    free(ctx->mutex);
    
    // Cleanup platform-specific components
    hot_reload_platform_cleanup();
    
    free(ctx);
}

int hot_reload_enable(HotReloadContext* ctx, HotReloadCapability capabilities) {
    if (!ctx) return -1;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    ctx->enabled = true;
    ctx->capabilities = capabilities;
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    
    return 0;
}

int hot_reload_disable(HotReloadContext* ctx) {
    if (!ctx) return -1;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    ctx->enabled = false;
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    
    return 0;
}

// =============================================================================
// Registration
// =============================================================================

int hot_reload_register_function(HotReloadContext* ctx, const char* name,
                                FuncDeclNode* ast, HotReloadCapability caps) {
    if (!ctx || !name || !ast) return -1;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    // Check if already registered
    if (find_function(ctx, name)) {
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return -1;
    }
    
    HotReloadableFunction* func = calloc(1, sizeof(HotReloadableFunction));
    if (!func) {
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return -1;
    }
    
    func->name = str_dup(name);
    func->ast_node = ast;
    func->capabilities = caps;
    func->version = 1;
    
    // Add to list
    func->next = ctx->functions;
    ctx->functions = func;
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    return 0;
}

int hot_reload_register_type(HotReloadContext* ctx, const char* name,
                            Type* type, MigrationStrategy strategy) {
    if (!ctx || !name || !type) return -1;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    // Check if already registered
    if (find_type(ctx, name)) {
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return -1;
    }
    
    HotReloadableType* hr_type = calloc(1, sizeof(HotReloadableType));
    if (!hr_type) {
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return -1;
    }
    
    hr_type->name = str_dup(name);
    hr_type->type_info = type;
    hr_type->migration_strategy = strategy;
    hr_type->version = 1;
    
    // Add to list
    hr_type->next = ctx->types;
    ctx->types = hr_type;
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    return 0;
}

// =============================================================================
// File Watching
// =============================================================================

int hot_reload_watch_file(HotReloadContext* ctx, const char* path,
                         void (*on_change)(const char*, void*), void* context) {
    if (!ctx || !path) return -1;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    // Check if already watching
    if (find_watcher(ctx, path)) {
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return -1;
    }
    
    FileWatcher* watcher = calloc(1, sizeof(FileWatcher));
    if (!watcher) {
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return -1;
    }
    
    watcher->watched_path = str_dup(path);
    watcher->on_change = on_change;
    watcher->callback_context = context;
    watcher->is_directory = false;
    
    // Get initial modification time
    struct stat st;
    if (stat(path, &st) == 0) {
        watcher->last_modified = st.st_mtime;
    }
    
    // Set up platform-specific watching
    watcher->platform_handle = hot_reload_platform_watch_file(path);
    
    // Add to list
    watcher->next = ctx->file_watchers;
    ctx->file_watchers = watcher;
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    return 0;
}

// =============================================================================
// State Preservation
// =============================================================================

int hot_reload_preserve_state(HotReloadContext* ctx, const char* type_name,
                             void* instance, size_t size) {
    if (!ctx || !type_name || !instance || size == 0) return -1;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    StatePreservation* state = calloc(1, sizeof(StatePreservation));
    if (!state) {
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return -1;
    }
    
    // Copy state data
    state->state_data = malloc(size);
    if (!state->state_data) {
        free(state);
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return -1;
    }
    memcpy(state->state_data, instance, size);
    
    state->state_size = size;
    state->type_signature = str_dup(type_name);
    state->timestamp = time(NULL);
    state->version = 1;
    
    // Limit preserved states
    size_t count = 0;
    StatePreservation* s = ctx->preserved_states;
    while (s) {
        count++;
        s = s->next;
    }
    
    if (count >= ctx->max_preserved_states) {
        // Remove oldest
        if (ctx->preserved_states) {
            StatePreservation* oldest = ctx->preserved_states;
            StatePreservation* prev = NULL;
            s = ctx->preserved_states;
            
            while (s->next) {
                if (s->next->timestamp < oldest->timestamp) {
                    prev = s;
                    oldest = s->next;
                }
                s = s->next;
            }
            
            if (prev) {
                prev->next = oldest->next;
            } else {
                ctx->preserved_states = oldest->next;
            }
            
            free(oldest->state_data);
            free(oldest->type_signature);
            free(oldest);
        }
    }
    
    // Add new state
    state->next = ctx->preserved_states;
    ctx->preserved_states = state;
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    return 0;
}

// =============================================================================
// Reload Operations
// =============================================================================

HotReloadStatus hot_reload_function(HotReloadContext* ctx, const char* name) {
    if (!ctx || !name) return HOT_RELOAD_FAILED;
    
    if (!ctx->enabled) return HOT_RELOAD_NOT_SUPPORTED;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    HotReloadableFunction* func = find_function(ctx, name);
    if (!func) {
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return HOT_RELOAD_FAILED;
    }
    
    if (func->is_reloading) {
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return HOT_RELOAD_IN_PROGRESS;
    }
    
    func->is_reloading = true;
    clock_t start = clock();
    
    // TODO: Implement actual reloading logic
    // For now, simulate success
    HotReloadStatus status = HOT_RELOAD_SUCCESS;
    
    // Update statistics
    double reload_time = (double)(clock() - start) / CLOCKS_PER_SEC;
    func->reload_count++;
    func->avg_reload_time = (func->avg_reload_time * (func->reload_count - 1) + reload_time) / func->reload_count;
    func->last_reload = time(NULL);
    func->version++;
    
    if (status == HOT_RELOAD_SUCCESS) {
        ctx->successful_reloads++;
    } else {
        ctx->failed_reloads++;
        func->failed_reloads++;
    }
    
    ctx->total_reloads++;
    ctx->total_reload_time += reload_time;
    
    func->is_reloading = false;
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    
    return status;
}

// =============================================================================
// Missing Implementations
// =============================================================================

int hot_reload_unregister_function(HotReloadContext* ctx, const char* name) {
    if (!ctx || !name) return -1;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    HotReloadableFunction* prev = NULL;
    HotReloadableFunction* func = ctx->functions;
    
    while (func) {
        if (strcmp(func->name, name) == 0) {
            if (prev) {
                prev->next = func->next;
            } else {
                ctx->functions = func->next;
            }
            free(func->name);
            free(func->module_path);
            free(func);
            pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
            return 0;
        }
        prev = func;
        func = func->next;
    }
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    return -1;
}

int hot_reload_unregister_type(HotReloadContext* ctx, const char* name) {
    if (!ctx || !name) return -1;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    HotReloadableType* prev = NULL;
    HotReloadableType* type = ctx->types;
    
    while (type) {
        if (strcmp(type->name, name) == 0) {
            if (prev) {
                prev->next = type->next;
            } else {
                ctx->types = type->next;
            }
            free(type->name);
            free(type->module_path);
            free(type);
            pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
            return 0;
        }
        prev = type;
        type = type->next;
    }
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    return -1;
}

int hot_reload_watch_directory(HotReloadContext* ctx, const char* path,
                              void (*on_change)(const char*, void*), void* context) {
    if (!ctx || !path) return -1;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    // Check if already watching
    if (find_watcher(ctx, path)) {
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return -1;
    }
    
    FileWatcher* watcher = calloc(1, sizeof(FileWatcher));
    if (!watcher) {
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return -1;
    }
    
    watcher->watched_path = str_dup(path);
    watcher->on_change = on_change;
    watcher->callback_context = context;
    watcher->is_directory = true;
    
    // Get initial modification time
    struct stat st;
    if (stat(path, &st) == 0) {
        watcher->last_modified = st.st_mtime;
    }
    
    // Set up platform-specific watching
    watcher->platform_handle = hot_reload_platform_watch_file(path);
    
    // Add to list
    watcher->next = ctx->file_watchers;
    ctx->file_watchers = watcher;
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    return 0;
}

int hot_reload_unwatch(HotReloadContext* ctx, const char* path) {
    if (!ctx || !path) return -1;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    FileWatcher* prev = NULL;
    FileWatcher* watcher = ctx->file_watchers;
    
    while (watcher) {
        if (strcmp(watcher->watched_path, path) == 0) {
            if (prev) {
                prev->next = watcher->next;
            } else {
                ctx->file_watchers = watcher->next;
            }
            
            if (watcher->platform_handle) {
                hot_reload_platform_unwatch_file(watcher->platform_handle);
            }
            free(watcher->watched_path);
            free(watcher);
            
            pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
            return 0;
        }
        prev = watcher;
        watcher = watcher->next;
    }
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    return -1;
}

void* hot_reload_restore_state(HotReloadContext* ctx, const char* type_name,
                              size_t* size) {
    if (!ctx || !type_name) return NULL;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    StatePreservation* state = ctx->preserved_states;
    while (state) {
        if (strcmp(state->type_signature, type_name) == 0) {
            void* data = malloc(state->state_size);
            if (data) {
                memcpy(data, state->state_data, state->state_size);
                if (size) *size = state->state_size;
            }
            pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
            return data;
        }
        state = state->next;
    }
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    return NULL;
}

int hot_reload_clear_preserved_states(HotReloadContext* ctx) {
    if (!ctx) return -1;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    StatePreservation* state = ctx->preserved_states;
    while (state) {
        StatePreservation* next = state->next;
        free(state->state_data);
        free(state->type_signature);
        if (state->field_names) {
            for (size_t i = 0; i < state->field_count; i++) {
                free(state->field_names[i]);
            }
            free(state->field_names);
        }
        free(state->field_types);
        free(state);
        state = next;
    }
    
    ctx->preserved_states = NULL;
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    return 0;
}

int hot_reload_set_migration_callback(HotReloadContext* ctx, const char* type_name,
                                     bool (*migrate_fn)(void*, void*)) {
    if (!ctx || !type_name || !migrate_fn) return -1;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    HotReloadableType* type = find_type(ctx, type_name);
    if (type) {
        type->migrate_fn = migrate_fn;
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return 0;
    }
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    return -1;
}

int hot_reload_set_serialization_callbacks(HotReloadContext* ctx, const char* type_name,
                                          void* (*serialize)(void*, size_t*),
                                          void* (*deserialize)(void*, size_t)) {
    if (!ctx || !type_name) return -1;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    HotReloadableType* type = find_type(ctx, type_name);
    if (type) {
        type->serialize_fn = serialize;
        type->deserialize_fn = deserialize;
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return 0;
    }
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    return -1;
}

HotReloadStatus hot_reload_type(HotReloadContext* ctx, const char* name) {
    if (!ctx || !name) return HOT_RELOAD_FAILED;
    
    if (!ctx->enabled) return HOT_RELOAD_NOT_SUPPORTED;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    HotReloadableType* type = find_type(ctx, name);
    if (!type) {
        pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
        return HOT_RELOAD_FAILED;
    }
    
    // TODO: Implement actual type reloading logic
    HotReloadStatus status = HOT_RELOAD_SUCCESS;
    
    if (status == HOT_RELOAD_SUCCESS) {
        type->version++;
        ctx->successful_reloads++;
    } else {
        ctx->failed_reloads++;
    }
    
    ctx->total_reloads++;
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    return status;
}

HotReloadStatus hot_reload_module(HotReloadContext* ctx, const char* module_path) {
    if (!ctx || !module_path) return HOT_RELOAD_FAILED;
    
    if (!ctx->enabled) return HOT_RELOAD_NOT_SUPPORTED;
    
    // TODO: Implement module reloading
    return HOT_RELOAD_SUCCESS;
}

HotReloadStatus hot_reload_all(HotReloadContext* ctx) {
    if (!ctx) return HOT_RELOAD_FAILED;
    
    if (!ctx->enabled) return HOT_RELOAD_NOT_SUPPORTED;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    HotReloadStatus overall_status = HOT_RELOAD_SUCCESS;
    
    // Reload all functions
    HotReloadableFunction* func = ctx->functions;
    while (func) {
        // TODO: Actually reload
        func = func->next;
    }
    
    // Reload all types
    HotReloadableType* type = ctx->types;
    while (type) {
        // TODO: Actually reload
        type = type->next;
    }
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
    return overall_status;
}

int hot_reload_compile_file(HotReloadContext* ctx, const char* file_path) {
    if (!ctx || !file_path) return -1;
    
    // TODO: Implement incremental compilation
    return 0;
}

int hot_reload_compile_changed(HotReloadContext* ctx) {
    if (!ctx) return -1;
    
    // TODO: Implement compilation of changed files
    return 0;
}

int hot_reload_link_module(HotReloadContext* ctx, const char* module_path) {
    if (!ctx || !module_path) return -1;
    
    // TODO: Implement module linking
    return 0;
}

int hot_reload_enter_safe_point(HotReloadContext* ctx) {
    if (!ctx) return -1;
    
    // TODO: Implement safe point logic
    return 0;
}

int hot_reload_exit_safe_point(HotReloadContext* ctx) {
    if (!ctx) return -1;
    
    // TODO: Implement safe point logic
    return 0;
}

bool hot_reload_is_safe_to_reload(HotReloadContext* ctx) {
    if (!ctx) return false;
    
    // TODO: Implement actual safety check
    return true;
}

int hot_reload_validate_compatibility(HotReloadContext* ctx, const char* name) {
    if (!ctx || !name) return -1;
    
    // TODO: Implement compatibility validation
    return 0;
}

// =============================================================================
// Utilities
// =============================================================================

const char* hot_reload_status_to_string(HotReloadStatus status) {
    switch (status) {
        case HOT_RELOAD_SUCCESS: return "Success";
        case HOT_RELOAD_COMPILE_ERROR: return "Compile Error";
        case HOT_RELOAD_LINK_ERROR: return "Link Error";
        case HOT_RELOAD_STATE_MIGRATION_ERROR: return "State Migration Error";
        case HOT_RELOAD_VERSION_MISMATCH: return "Version Mismatch";
        case HOT_RELOAD_NOT_SUPPORTED: return "Not Supported";
        case HOT_RELOAD_IN_PROGRESS: return "In Progress";
        case HOT_RELOAD_FAILED: return "Failed";
        default: return "Unknown";
    }
}

void hot_reload_print_statistics(HotReloadContext* ctx) {
    if (!ctx) return;
    
    pthread_mutex_lock((pthread_mutex_t*)ctx->mutex);
    
    printf("=== Hot Reload Statistics ===\n");
    printf("Enabled: %s\n", ctx->enabled ? "Yes" : "No");
    printf("Total reloads: %zu\n", ctx->total_reloads);
    printf("Successful: %zu\n", ctx->successful_reloads);
    printf("Failed: %zu\n", ctx->failed_reloads);
    
    if (ctx->total_reloads > 0) {
        printf("Success rate: %.1f%%\n", 
               (double)ctx->successful_reloads / ctx->total_reloads * 100);
        printf("Average reload time: %.3f ms\n",
               ctx->total_reload_time / ctx->total_reloads * 1000);
    }
    
    // Count registered items
    size_t func_count = 0;
    HotReloadableFunction* func = ctx->functions;
    while (func) {
        func_count++;
        func = func->next;
    }
    
    size_t type_count = 0;
    HotReloadableType* type = ctx->types;
    while (type) {
        type_count++;
        type = type->next;
    }
    
    printf("Registered functions: %zu\n", func_count);
    printf("Registered types: %zu\n", type_count);
    printf("Preserved states: %zu\n", func_count); // TODO: Actually count states
    
    pthread_mutex_unlock((pthread_mutex_t*)ctx->mutex);
}

// =============================================================================
// Platform-Specific Implementations (Unix/Linux)
// =============================================================================

#ifdef __unix__

#include <fcntl.h>
#include <sys/inotify.h>

static int inotify_fd = -1;

int hot_reload_platform_init(void) {
    inotify_fd = inotify_init1(IN_NONBLOCK);
    return inotify_fd >= 0 ? 0 : -1;
}

void hot_reload_platform_cleanup(void) {
    if (inotify_fd >= 0) {
        close(inotify_fd);
        inotify_fd = -1;
    }
}

void* hot_reload_platform_watch_file(const char* path) {
    if (inotify_fd < 0) return NULL;
    
    int wd = inotify_add_watch(inotify_fd, path, IN_MODIFY | IN_CREATE | IN_DELETE);
    if (wd < 0) return NULL;
    
    return (void*)(intptr_t)wd;
}

void hot_reload_platform_unwatch_file(void* handle) {
    if (inotify_fd < 0 || !handle) return;
    
    int wd = (int)(intptr_t)handle;
    inotify_rm_watch(inotify_fd, wd);
}

bool hot_reload_platform_check_modified(void* handle, time_t* last_modified) {
    (void)handle;
    (void)last_modified;
    // TODO: Implement proper inotify event checking
    return false;
}

#else

// Fallback implementation for non-Unix platforms

int hot_reload_platform_init(void) {
    return 0;
}

void hot_reload_platform_cleanup(void) {
}

void* hot_reload_platform_watch_file(const char* path) {
    return (void*)1; // Dummy handle
}

void hot_reload_platform_unwatch_file(void* handle) {
}

bool hot_reload_platform_check_modified(void* handle, time_t* last_modified) {
    return false;
}

#endif