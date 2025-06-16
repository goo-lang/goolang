#ifndef PACKAGE_MANAGER_H
#define PACKAGE_MANAGER_H

#include "module.h"
#include <stdbool.h>

// Forward declarations
typedef struct PackageManager PackageManager;
typedef struct ResolvedDependency ResolvedDependency;
typedef struct DependencyGraph DependencyGraph;
typedef struct InstallContext InstallContext;

// Package resolution result
typedef enum {
    RESOLVE_SUCCESS,
    RESOLVE_NOT_FOUND,
    RESOLVE_VERSION_CONFLICT,
    RESOLVE_CIRCULAR_DEPENDENCY,
    RESOLVE_NETWORK_ERROR,
    RESOLVE_AUTH_ERROR,
    RESOLVE_CHECKSUM_ERROR
} ResolveResult;

// Resolved dependency with concrete version
typedef struct ResolvedDependency {
    char* name;
    Version* resolved_version;
    char* source_url;
    char* local_path;
    bool is_cached;
    Module* module;
    struct ResolvedDependency** dependencies;
    size_t dependency_count;
    struct ResolvedDependency* next;
} ResolvedDependency;

// Dependency resolution graph
typedef struct DependencyGraph {
    ResolvedDependency* root;
    ResolvedDependency** nodes;
    size_t node_count;
    size_t node_capacity;
} DependencyGraph;

// Installation context
typedef struct InstallContext {
    char* workspace_root;
    char* cache_dir;
    bool force_update;
    bool offline_mode;
    bool verify_checksums;
    char** selected_features;
    size_t feature_count;
} InstallContext;

// Package manager instance
typedef struct PackageManager {
    PackageConfig* config;
    Module* current_module;
    DependencyGraph* dependency_graph;
    char* workspace_root;
    char* lock_file_path;
} PackageManager;

// Lock file structure for reproducible builds
typedef struct LockFile {
    char* version;                 // Lock file format version
    Module* root_module;           // Root module information
    ResolvedDependency* packages;  // Locked package versions
    char* checksum;                // Overall lock file checksum
    time_t generated_at;           // Generation timestamp
} LockFile;

// Function declarations

// Package manager lifecycle
PackageManager* package_manager_create(const char* workspace_root);
void package_manager_free(PackageManager* pm);
bool package_manager_initialize(PackageManager* pm);
bool package_manager_load_module(PackageManager* pm, const char* module_path);

// Dependency resolution
ResolveResult package_manager_resolve_dependencies(PackageManager* pm, DependencyGraph** graph);
ResolvedDependency* resolve_dependency(PackageManager* pm, const Dependency* dep, DependencyGraph* graph);
bool check_version_conflicts(const DependencyGraph* graph, char** error_message);
bool detect_circular_dependencies(const DependencyGraph* graph, char** error_message);

// Package installation
bool package_manager_install(PackageManager* pm, const InstallContext* context);
bool package_manager_install_dependency(PackageManager* pm, const char* name, const char* constraint);
bool package_manager_update(PackageManager* pm, const char* package_name);
bool package_manager_uninstall(PackageManager* pm, const char* package_name);

// Package fetching and caching
bool fetch_package(PackageManager* pm, const ResolvedDependency* dep, const char* cache_dir);
bool fetch_from_registry(PackageManager* pm, const ResolvedDependency* dep, const char* cache_dir);
bool fetch_from_git(PackageManager* pm, const ResolvedDependency* dep, const char* cache_dir);
bool fetch_from_local(PackageManager* pm, const ResolvedDependency* dep, const char* cache_dir);
char* get_package_cache_path(PackageManager* pm, const ResolvedDependency* dep);

// Registry operations
bool registry_search(const ModuleRegistry* registry, const char* query, Module*** results, size_t* count);
bool registry_get_versions(const ModuleRegistry* registry, const char* package_name, Version*** versions, size_t* count);
Module* registry_get_package_info(const ModuleRegistry* registry, const char* package_name, const Version* version);
bool registry_publish(const ModuleRegistry* registry, const Module* module, const char* auth_token);

// Lock file operations
LockFile* lock_file_create(const Module* root_module, const DependencyGraph* graph);
void lock_file_free(LockFile* lock_file);
LockFile* lock_file_load(const char* filepath);
bool lock_file_save(const LockFile* lock_file, const char* filepath);
bool lock_file_validate(const LockFile* lock_file, const Module* current_module);

// Workspace operations
bool package_manager_init_workspace(const char* path, const char* name);
bool package_manager_add_workspace_member(PackageManager* pm, const char* member_path);
bool package_manager_remove_workspace_member(PackageManager* pm, const char* member_path);
Module** package_manager_get_workspace_members(PackageManager* pm, size_t* count);

// Build integration
bool package_manager_prepare_build(PackageManager* pm, char*** include_paths, size_t* path_count);
bool package_manager_link_dependencies(PackageManager* pm, char*** library_paths, size_t* path_count);
char* package_manager_get_build_script_path(PackageManager* pm, const char* package_name);

// Dependency graph operations
DependencyGraph* dependency_graph_create(void);
void dependency_graph_free(DependencyGraph* graph);
bool dependency_graph_add_node(DependencyGraph* graph, ResolvedDependency* dep);
ResolvedDependency* dependency_graph_find_node(const DependencyGraph* graph, const char* name);
bool dependency_graph_add_edge(DependencyGraph* graph, const char* from, const char* to);
bool dependency_graph_topological_sort(const DependencyGraph* graph, ResolvedDependency*** sorted, size_t* count);

// Resolved dependency operations
ResolvedDependency* resolved_dependency_create(const char* name, const Version* version);
void resolved_dependency_free(ResolvedDependency* dep);
bool resolved_dependency_add_child(ResolvedDependency* parent, ResolvedDependency* child);

// Package verification
bool verify_package_checksum(const char* package_path, const char* expected_checksum);
bool verify_package_signature(const char* package_path, const char* signature);
char* calculate_package_checksum(const char* package_path);

// Feature management
bool feature_is_enabled(const Module* module, const char* feature);
char** get_enabled_features(const Module* module, const InstallContext* context, size_t* count);
bool validate_feature_dependencies(const Module* module, const char** features, size_t feature_count);

// Command-line interface support
typedef enum {
    CMD_INIT,
    CMD_INSTALL,
    CMD_UPDATE,
    CMD_UNINSTALL,
    CMD_LIST,
    CMD_SEARCH,
    CMD_PUBLISH,
    CMD_WORKSPACE,
    CMD_CLEAN,
    CMD_VERSION
} PackageCommand;

typedef struct CommandArgs {
    PackageCommand command;
    char** args;
    size_t arg_count;
    bool force;
    bool offline;
    bool verbose;
    char* workspace;
    char* registry;
} CommandArgs;

// CLI functions
int package_manager_main(int argc, char** argv);
CommandArgs* parse_command_args(int argc, char** argv);
void command_args_free(CommandArgs* args);
int execute_command(PackageManager* pm, const CommandArgs* args);

// Utility functions
char* package_manager_get_default_registry_url(void);
char* package_manager_get_cache_dir(void);
char* package_manager_get_config_dir(void);
bool package_manager_is_workspace_root(const char* path);
char* package_manager_find_workspace_root(const char* start_path);

// Error handling
typedef enum {
    PKG_ERR_SUCCESS = 0,
    PKG_ERR_INVALID_MODULE,
    PKG_ERR_DEPENDENCY_NOT_FOUND,
    PKG_ERR_VERSION_CONFLICT,
    PKG_ERR_CIRCULAR_DEPENDENCY,
    PKG_ERR_NETWORK_ERROR,
    PKG_ERR_AUTH_ERROR,
    PKG_ERR_CHECKSUM_MISMATCH,
    PKG_ERR_WORKSPACE_ERROR,
    PKG_ERR_LOCK_FILE_ERROR,
    PKG_ERR_REGISTRY_ERROR,
    PKG_ERR_INTERNAL_ERROR
} PackageError;

const char* package_error_string(PackageError error);
void package_manager_set_error(PackageManager* pm, PackageError error, const char* message);
PackageError package_manager_get_last_error(const PackageManager* pm, char** message);

#endif // PACKAGE_MANAGER_H