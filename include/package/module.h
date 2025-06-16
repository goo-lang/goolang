#ifndef MODULE_H
#define MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// Forward declarations
typedef struct Module Module;
typedef struct Dependency Dependency;
typedef struct Version Version;
typedef struct ModuleRegistry ModuleRegistry;

// Semantic versioning structure
typedef struct Version {
    int major;
    int minor;
    int patch;
    char* pre_release;    // Optional pre-release identifier
    char* build_metadata; // Optional build metadata
} Version;

// Version constraint types
typedef enum {
    VERSION_EXACT,        // "1.2.3"
    VERSION_COMPATIBLE,   // "^1.2.3" (compatible within major version)
    VERSION_TILDE,        // "~1.2.3" (compatible within minor version)
    VERSION_RANGE,        // ">=1.2.0 <2.0.0"
    VERSION_LATEST,       // "latest"
    VERSION_ANY           // "*"
} VersionConstraintType;

typedef struct VersionConstraint {
    VersionConstraintType type;
    Version* min_version;
    Version* max_version;
    bool include_pre_release;
} VersionConstraint;

// Dependency specification
typedef struct Dependency {
    char* name;                    // Package name
    char* source;                  // Source URL or registry name
    VersionConstraint* constraint; // Version constraint
    bool optional;                 // Optional dependency
    bool dev_only;                 // Development-only dependency
    char** features;               // Optional features to enable
    size_t feature_count;
    struct Dependency* next;       // Linked list of dependencies
} Dependency;

// Module/package definition
typedef struct Module {
    // Basic information
    char* name;                    // Module name
    Version* version;              // Module version
    char* description;             // Short description
    char* homepage;                // Homepage URL
    char* repository;              // Repository URL
    char* license;                 // License identifier
    char** authors;                // Array of author strings
    size_t author_count;
    char** keywords;               // Array of keywords
    size_t keyword_count;
    
    // Dependencies
    Dependency* dependencies;      // Runtime dependencies
    Dependency* dev_dependencies;  // Development dependencies
    Dependency* build_dependencies; // Build-time dependencies
    
    // Build configuration
    char* build_script;            // Build script path
    char** source_dirs;            // Source directories
    size_t source_dir_count;
    char** exclude_patterns;       // Exclude patterns
    size_t exclude_pattern_count;
    
    // Package configuration
    char* main_file;               // Main entry point
    char** public_exports;         // Public API exports
    size_t export_count;
    char** private_symbols;        // Private symbols
    size_t private_symbol_count;
    
    // Features and optional functionality
    char** features;               // Available features
    size_t feature_count;
    char** default_features;       // Default enabled features
    size_t default_feature_count;
    
    // Metadata
    char* minimum_goo_version;     // Minimum Goo compiler version
    time_t created_at;             // Creation timestamp
    time_t updated_at;             // Last update timestamp
    char* checksum;                // Package integrity checksum
    
    // Runtime information
    char* root_path;               // Module root directory
    char* cache_path;              // Cache directory
    bool is_workspace;             // Is this a workspace root
    Module** workspace_members;    // Workspace member modules
    size_t workspace_member_count;
} Module;

// Module registry for package resolution
typedef struct ModuleRegistry {
    char* name;                    // Registry name
    char* url;                     // Registry URL
    char* auth_token;              // Authentication token
    bool is_default;               // Is default registry
    struct ModuleRegistry* next;   // Linked list
} ModuleRegistry;

// Package manager configuration
typedef struct PackageConfig {
    ModuleRegistry* registries;    // Available registries
    char* cache_dir;               // Global cache directory
    char* config_dir;              // Configuration directory
    bool offline_mode;             // Offline-only mode
    bool verify_checksums;         // Verify package checksums
    int network_timeout;           // Network timeout in seconds
    char* proxy_url;               // HTTP proxy URL
} PackageConfig;

// Function declarations

// Version operations
Version* version_parse(const char* version_str);
void version_free(Version* version);
char* version_to_string(const Version* version);
int version_compare(const Version* a, const Version* b);
bool version_satisfies(const Version* version, const VersionConstraint* constraint);

// Version constraint operations
VersionConstraint* version_constraint_parse(const char* constraint_str);
void version_constraint_free(VersionConstraint* constraint);
char* version_constraint_to_string(const VersionConstraint* constraint);

// Dependency operations
Dependency* dependency_create(const char* name, const char* constraint_str);
void dependency_free(Dependency* dep);
Dependency* dependency_list_add(Dependency* list, Dependency* dep);
Dependency* dependency_list_find(Dependency* list, const char* name);
void dependency_list_free(Dependency* list);

// Module operations
Module* module_create(const char* name, const char* version);
void module_free(Module* module);
Module* module_load_from_file(const char* filepath);
bool module_save_to_file(const Module* module, const char* filepath);
Module* module_load_from_directory(const char* directory);
bool module_validate(const Module* module, char** error_message);

// Module parsing and serialization
Module* module_parse_toml(const char* toml_content);
char* module_to_toml(const Module* module);
Module* module_parse_json(const char* json_content);
char* module_to_json(const Module* module);

// Registry operations
ModuleRegistry* registry_create(const char* name, const char* url);
void registry_free(ModuleRegistry* registry);
ModuleRegistry* registry_list_add(ModuleRegistry* list, ModuleRegistry* registry);
ModuleRegistry* registry_list_find(ModuleRegistry* list, const char* name);
void registry_list_free(ModuleRegistry* list);

// Package configuration
PackageConfig* package_config_load(const char* config_dir);
void package_config_free(PackageConfig* config);
bool package_config_save(const PackageConfig* config, const char* config_dir);
PackageConfig* package_config_default(void);

// Utility functions
char* module_resolve_path(const Module* module, const char* relative_path);
bool module_is_local(const Module* module);
bool module_is_git(const Module* module);
bool module_is_registry(const Module* module);
char* module_get_cache_key(const Module* module);

// File operations
bool file_exists(const char* path);
bool directory_exists(const char* path);
bool create_directory(const char* path);
char* read_file_contents(const char* path);
bool write_file_contents(const char* path, const char* content);

// String utilities
char* string_duplicate(const char* str);
char** string_array_create(size_t count);
void string_array_free(char** array, size_t count);
char** string_split(const char* str, const char* delimiter, size_t* count);

#endif // MODULE_H