#ifndef GOO_PACKAGE_MANAGER_H
#define GOO_PACKAGE_MANAGER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// Semantic Version
// =============================================================================

typedef struct SemVer {
    int major;
    int minor;
    int patch;
    const char* prerelease;        // e.g., "beta.1"
} SemVer;

// =============================================================================
// Package Dependency
// =============================================================================

typedef enum {
    DEP_STRATEGY_EXACT,            // =1.2.3
    DEP_STRATEGY_COMPATIBLE,       // ^1.2.3 (same major)
    DEP_STRATEGY_LATEST,           // latest stable
    DEP_STRATEGY_PERFORMANCE,      // benchmark and select fastest
    DEP_STRATEGY_MINIMAL,          // fewest transitive deps
    DEP_STRATEGY_SECURITY,         // most secure/audited
} DependencyStrategy;

typedef struct PackageDependency {
    char* name;                    // Package name (e.g., "json")
    char* path;                    // Import path (e.g., "crypto/aes")
    SemVer required_version;
    DependencyStrategy strategy;

    // Resolution result
    bool is_resolved;
    SemVer resolved_version;
    char* resolved_path;           // Filesystem path to resolved package

    // Security
    bool has_security_advisory;
    char* advisory_id;

    // Conditional dependency
    char* target_condition;        // e.g., "@target_has(\"avx2\")"

    struct PackageDependency* next;
} PackageDependency;

// =============================================================================
// Package Manifest (goo.toml)
// =============================================================================

typedef struct PackageManifest {
    char* name;
    char* description;
    SemVer version;
    char* author;
    char* license;
    char* repository;

    PackageDependency* dependencies;
    size_t dependency_count;

    // Build configuration
    char* min_goo_version;
    char** build_tags;
    size_t build_tag_count;
} PackageManifest;

// =============================================================================
// Dependency Resolution
// =============================================================================

typedef enum {
    RESOLVE_OK,
    RESOLVE_NOT_FOUND,
    RESOLVE_VERSION_CONFLICT,
    RESOLVE_CYCLE_DETECTED,
    RESOLVE_SECURITY_BLOCKED,
} ResolveStatus;

typedef struct ResolveResult {
    ResolveStatus status;
    PackageDependency** resolved;  // Ordered list of resolved deps
    size_t resolved_count;
    char* error_message;
} ResolveResult;

// =============================================================================
// Package Manager
// =============================================================================

typedef struct PackageManager {
    // Configuration
    char* cache_dir;               // Local package cache
    char** registry_urls;          // Package registries
    size_t registry_count;

    // Security policy
    bool auto_patch_security;
    bool block_known_vulnerable;

    // Loaded manifest
    PackageManifest* manifest;

    // Statistics
    struct {
        size_t packages_resolved;
        size_t packages_cached;
        size_t security_advisories;
        size_t version_conflicts_resolved;
    } stats;
} PackageManager;

// =============================================================================
// API
// =============================================================================

// Version operations
SemVer semver_parse(const char* version_string);
int semver_compare(SemVer a, SemVer b);
bool semver_compatible(SemVer required, SemVer candidate);
char* semver_to_string(SemVer v);

// Manifest operations
PackageManifest* manifest_new(const char* name, SemVer version);
void manifest_free(PackageManifest* manifest);
void manifest_add_dependency(PackageManifest* manifest, const char* name,
                             const char* version, DependencyStrategy strategy);

// Dependency resolution
ResolveResult* resolve_dependencies(PackageManager* pm, PackageManifest* manifest);
void resolve_result_free(ResolveResult* result);

// Package manager lifecycle
PackageManager* package_manager_new(const char* cache_dir);
void package_manager_free(PackageManager* pm);
void package_manager_add_registry(PackageManager* pm, const char* url);

// Dependency strategy parsing
bool parse_dependency_strategy(const char* strategy_str, DependencyStrategy* out);

#endif // GOO_PACKAGE_MANAGER_H
