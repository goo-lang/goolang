#include "package_manager.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static char* str_dup(const char* s) {
    if (!s) return NULL;
    char* d = malloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

// =============================================================================
// Semantic Version
// =============================================================================

SemVer semver_parse(const char* version_string) {
    SemVer v = {0};
    if (!version_string) return v;

    // Skip leading 'v' or '='
    const char* p = version_string;
    if (*p == 'v' || *p == '=' || *p == '^' || *p == '~') p++;

    sscanf(p, "%d.%d.%d", &v.major, &v.minor, &v.patch);

    // Check for prerelease
    const char* dash = strchr(p, '-');
    if (dash) v.prerelease = str_dup(dash + 1);

    return v;
}

int semver_compare(SemVer a, SemVer b) {
    if (a.major != b.major) return a.major - b.major;
    if (a.minor != b.minor) return a.minor - b.minor;
    if (a.patch != b.patch) return a.patch - b.patch;

    // Prerelease has lower precedence than release
    if (a.prerelease && !b.prerelease) return -1;
    if (!a.prerelease && b.prerelease) return 1;
    if (a.prerelease && b.prerelease) return strcmp(a.prerelease, b.prerelease);

    return 0;
}

bool semver_compatible(SemVer required, SemVer candidate) {
    // Compatible if same major version and candidate >= required
    if (required.major != candidate.major) return false;
    return semver_compare(candidate, required) >= 0;
}

char* semver_to_string(SemVer v) {
    char buf[64];
    if (v.prerelease) {
        snprintf(buf, sizeof(buf), "%d.%d.%d-%s", v.major, v.minor, v.patch, v.prerelease);
    } else {
        snprintf(buf, sizeof(buf), "%d.%d.%d", v.major, v.minor, v.patch);
    }
    return str_dup(buf);
}

// =============================================================================
// Manifest Operations
// =============================================================================

PackageManifest* manifest_new(const char* name, SemVer version) {
    PackageManifest* m = calloc(1, sizeof(PackageManifest));
    if (!m) return NULL;
    m->name = str_dup(name);
    m->version = version;
    return m;
}

void manifest_free(PackageManifest* manifest) {
    if (!manifest) return;

    free(manifest->name);
    free(manifest->description);
    free(manifest->author);
    free(manifest->license);
    free(manifest->repository);
    free(manifest->min_goo_version);

    PackageDependency* dep = manifest->dependencies;
    while (dep) {
        PackageDependency* next = dep->next;
        free(dep->name);
        free(dep->path);
        free(dep->resolved_path);
        free(dep->advisory_id);
        free(dep->target_condition);
        free((void*)dep->required_version.prerelease);
        free((void*)dep->resolved_version.prerelease);
        free(dep);
        dep = next;
    }

    for (size_t i = 0; i < manifest->build_tag_count; i++) {
        free(manifest->build_tags[i]);
    }
    free(manifest->build_tags);

    free(manifest);
}

void manifest_add_dependency(PackageManifest* manifest, const char* name,
                             const char* version, DependencyStrategy strategy) {
    if (!manifest || !name) return;

    PackageDependency* dep = calloc(1, sizeof(PackageDependency));
    if (!dep) return;

    dep->name = str_dup(name);
    dep->required_version = semver_parse(version);
    dep->strategy = strategy;
    dep->next = manifest->dependencies;
    manifest->dependencies = dep;
    manifest->dependency_count++;
}

// =============================================================================
// Dependency Resolution
// =============================================================================

ResolveResult* resolve_dependencies(PackageManager* pm, PackageManifest* manifest) {
    if (!pm || !manifest) return NULL;

    ResolveResult* result = calloc(1, sizeof(ResolveResult));
    if (!result) return NULL;

    // Allocate space for resolved deps
    result->resolved = calloc(manifest->dependency_count, sizeof(PackageDependency*));
    if (!result->resolved && manifest->dependency_count > 0) {
        free(result);
        return NULL;
    }

    // Resolve each dependency
    size_t resolved = 0;
    for (PackageDependency* dep = manifest->dependencies; dep; dep = dep->next) {
        // Check security blocklist
        if (pm->block_known_vulnerable && dep->has_security_advisory) {
            result->status = RESOLVE_SECURITY_BLOCKED;
            char msg[512];
            snprintf(msg, sizeof(msg), "Package '%s' has security advisory: %s",
                     dep->name, dep->advisory_id ? dep->advisory_id : "unknown");
            result->error_message = str_dup(msg);
            pm->stats.security_advisories++;
            return result;
        }

        // Simulate resolution (in production, this would check the registry)
        dep->is_resolved = true;
        dep->resolved_version = dep->required_version;

        result->resolved[resolved++] = dep;
        pm->stats.packages_resolved++;
    }

    result->resolved_count = resolved;
    result->status = RESOLVE_OK;

    return result;
}

void resolve_result_free(ResolveResult* result) {
    if (!result) return;
    free(result->resolved);
    free(result->error_message);
    free(result);
}

// =============================================================================
// Package Manager Lifecycle
// =============================================================================

PackageManager* package_manager_new(const char* cache_dir) {
    PackageManager* pm = calloc(1, sizeof(PackageManager));
    if (!pm) return NULL;

    pm->cache_dir = str_dup(cache_dir ? cache_dir : ".goo/cache");
    pm->block_known_vulnerable = true;
    pm->auto_patch_security = true;

    return pm;
}

void package_manager_free(PackageManager* pm) {
    if (!pm) return;

    free(pm->cache_dir);
    for (size_t i = 0; i < pm->registry_count; i++) {
        free(pm->registry_urls[i]);
    }
    free(pm->registry_urls);
    manifest_free(pm->manifest);
    free(pm);
}

void package_manager_add_registry(PackageManager* pm, const char* url) {
    if (!pm || !url) return;

    char** tmp = realloc(pm->registry_urls, (pm->registry_count + 1) * sizeof(char*));
    if (!tmp) return;
    pm->registry_urls = tmp;
    pm->registry_urls[pm->registry_count++] = str_dup(url);
}

// =============================================================================
// Strategy Parsing
// =============================================================================

bool parse_dependency_strategy(const char* strategy_str, DependencyStrategy* out) {
    if (!strategy_str || !out) return false;

    if (strcmp(strategy_str, "exact") == 0)             { *out = DEP_STRATEGY_EXACT; return true; }
    if (strcmp(strategy_str, "compatible") == 0)         { *out = DEP_STRATEGY_COMPATIBLE; return true; }
    if (strcmp(strategy_str, "latest") == 0)             { *out = DEP_STRATEGY_LATEST; return true; }
    if (strcmp(strategy_str, "latest_compatible") == 0)  { *out = DEP_STRATEGY_COMPATIBLE; return true; }
    if (strcmp(strategy_str, "performance") == 0)        { *out = DEP_STRATEGY_PERFORMANCE; return true; }
    if (strcmp(strategy_str, "performance_optimized") == 0) { *out = DEP_STRATEGY_PERFORMANCE; return true; }
    if (strcmp(strategy_str, "minimal") == 0)            { *out = DEP_STRATEGY_MINIMAL; return true; }
    if (strcmp(strategy_str, "minimal_dependencies") == 0)  { *out = DEP_STRATEGY_MINIMAL; return true; }
    if (strcmp(strategy_str, "security") == 0)           { *out = DEP_STRATEGY_SECURITY; return true; }

    return false;
}
