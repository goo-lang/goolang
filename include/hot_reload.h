#ifndef HOT_RELOAD_H
#define HOT_RELOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "ast.h"
#include "types.h"

// Forward declarations
typedef struct HotReloadContext HotReloadContext;
typedef struct HotReloadableFunction HotReloadableFunction;
typedef struct HotReloadableType HotReloadableType;
typedef struct StatePreservation StatePreservation;
typedef struct FileWatcher FileWatcher;
typedef struct IncrementalCompiler IncrementalCompiler;

// Hot reload status
typedef enum {
    HOT_RELOAD_SUCCESS = 0,
    HOT_RELOAD_COMPILE_ERROR,
    HOT_RELOAD_LINK_ERROR,
    HOT_RELOAD_STATE_MIGRATION_ERROR,
    HOT_RELOAD_VERSION_MISMATCH,
    HOT_RELOAD_NOT_SUPPORTED,
    HOT_RELOAD_IN_PROGRESS,
    HOT_RELOAD_FAILED
} HotReloadStatus;

// Hot reload capabilities
typedef enum {
    HOT_RELOAD_CAP_FUNCTION = 1 << 0,
    HOT_RELOAD_CAP_TYPE = 1 << 1,
    HOT_RELOAD_CAP_GLOBAL = 1 << 2,
    HOT_RELOAD_CAP_CONSTANT = 1 << 3,
    HOT_RELOAD_CAP_MODULE = 1 << 4,
    HOT_RELOAD_CAP_STATE_MIGRATION = 1 << 5,
    HOT_RELOAD_CAP_ASYNC = 1 << 6,
    HOT_RELOAD_CAP_SAFE_POINT = 1 << 7
} HotReloadCapability;

// State migration strategy
typedef enum {
    MIGRATION_STRATEGY_DEFAULT = 0,
    MIGRATION_STRATEGY_CUSTOM,
    MIGRATION_STRATEGY_SERIALIZE,
    MIGRATION_STRATEGY_TRANSFORM,
    MIGRATION_STRATEGY_RESET
} MigrationStrategy;

// Hot reloadable function
struct HotReloadableFunction {
    char* name;
    char* module_path;
    FuncDeclNode* ast_node;
    void* current_impl;
    void* new_impl;
    size_t version;
    HotReloadCapability capabilities;
    bool is_reloading;
    time_t last_reload;
    size_t reload_count;
    
    // Performance tracking
    double avg_reload_time;
    size_t failed_reloads;
    
    struct HotReloadableFunction* next;
};

// Hot reloadable type
struct HotReloadableType {
    char* name;
    char* module_path;
    Type* type_info;
    size_t version;
    size_t instance_count;
    MigrationStrategy migration_strategy;
    
    // Migration callbacks
    void* (*serialize_fn)(void* instance, size_t* size);
    void* (*deserialize_fn)(void* data, size_t size);
    bool (*migrate_fn)(void* old_instance, void* new_instance);
    
    struct HotReloadableType* next;
};

// State preservation
struct StatePreservation {
    void* state_data;
    size_t state_size;
    size_t version;
    char* type_signature;
    time_t timestamp;
    
    // Metadata for migration
    char** field_names;
    Type** field_types;
    size_t field_count;
    
    struct StatePreservation* next;
};

// File watcher for detecting changes
struct FileWatcher {
    char* watched_path;
    time_t last_modified;
    bool is_directory;
    
    // Platform-specific handle
    void* platform_handle;
    
    // Callback for changes
    void (*on_change)(const char* path, void* context);
    void* callback_context;
    
    struct FileWatcher* next;
};

// Incremental compiler
struct IncrementalCompiler {
    char* project_root;
    char* build_dir;
    
    // Compilation state
    bool is_compiling;
    size_t compile_count;
    double avg_compile_time;
    
    // Dependency tracking
    char** source_files;
    size_t source_count;
    time_t* last_compiled;
    
    // Error reporting
    char* last_error;
    size_t error_count;
};

// Main hot reload context
struct HotReloadContext {
    // Configuration
    bool enabled;
    HotReloadCapability capabilities;
    size_t max_preserved_states;
    double reload_timeout;
    
    // Components
    FileWatcher* file_watchers;
    IncrementalCompiler* compiler;
    
    // Registered items
    HotReloadableFunction* functions;
    HotReloadableType* types;
    StatePreservation* preserved_states;
    
    // Statistics
    size_t total_reloads;
    size_t successful_reloads;
    size_t failed_reloads;
    double total_reload_time;
    
    // Thread safety
    void* mutex;
};

// Context management
HotReloadContext* hot_reload_context_new(void);
void hot_reload_context_free(HotReloadContext* ctx);
int hot_reload_enable(HotReloadContext* ctx, HotReloadCapability capabilities);
int hot_reload_disable(HotReloadContext* ctx);

// Registration
int hot_reload_register_function(HotReloadContext* ctx, const char* name, 
                                FuncDeclNode* ast, HotReloadCapability caps);
int hot_reload_register_type(HotReloadContext* ctx, const char* name,
                            Type* type, MigrationStrategy strategy);
int hot_reload_unregister_function(HotReloadContext* ctx, const char* name);
int hot_reload_unregister_type(HotReloadContext* ctx, const char* name);

// File watching
int hot_reload_watch_file(HotReloadContext* ctx, const char* path,
                         void (*on_change)(const char*, void*), void* context);
int hot_reload_watch_directory(HotReloadContext* ctx, const char* path,
                              void (*on_change)(const char*, void*), void* context);
int hot_reload_unwatch(HotReloadContext* ctx, const char* path);

// State preservation
int hot_reload_preserve_state(HotReloadContext* ctx, const char* type_name,
                             void* instance, size_t size);
void* hot_reload_restore_state(HotReloadContext* ctx, const char* type_name,
                              size_t* size);
int hot_reload_clear_preserved_states(HotReloadContext* ctx);

// Migration
int hot_reload_set_migration_callback(HotReloadContext* ctx, const char* type_name,
                                     bool (*migrate_fn)(void*, void*));
int hot_reload_set_serialization_callbacks(HotReloadContext* ctx, const char* type_name,
                                          void* (*serialize)(void*, size_t*),
                                          void* (*deserialize)(void*, size_t));

// Reload operations
HotReloadStatus hot_reload_function(HotReloadContext* ctx, const char* name);
HotReloadStatus hot_reload_type(HotReloadContext* ctx, const char* name);
HotReloadStatus hot_reload_module(HotReloadContext* ctx, const char* module_path);
HotReloadStatus hot_reload_all(HotReloadContext* ctx);

// Incremental compilation
int hot_reload_compile_file(HotReloadContext* ctx, const char* file_path);
int hot_reload_compile_changed(HotReloadContext* ctx);
int hot_reload_link_module(HotReloadContext* ctx, const char* module_path);

// Safe points
int hot_reload_enter_safe_point(HotReloadContext* ctx);
int hot_reload_exit_safe_point(HotReloadContext* ctx);
bool hot_reload_is_safe_to_reload(HotReloadContext* ctx);

// Utilities
const char* hot_reload_status_to_string(HotReloadStatus status);
void hot_reload_print_statistics(HotReloadContext* ctx);
int hot_reload_validate_compatibility(HotReloadContext* ctx, const char* name);

// Platform-specific implementations
int hot_reload_platform_init(void);
void hot_reload_platform_cleanup(void);
void* hot_reload_platform_watch_file(const char* path);
void hot_reload_platform_unwatch_file(void* handle);
bool hot_reload_platform_check_modified(void* handle, time_t* last_modified);

#endif // HOT_RELOAD_H