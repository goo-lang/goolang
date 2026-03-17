#include "package_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } \
} while(0)
#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))
#define EXPECT_EQ(a, b) do { \
    if ((a) != (b)) { printf("  FAIL: %s != %s (line %d)\n", #a, #b, __LINE__); return 1; } \
} while(0)
#define EXPECT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { printf("  FAIL: \"%s\" != \"%s\" (line %d)\n", (a), (b), __LINE__); return 1; } \
} while(0)
#define EXPECT_NOT_NULL(p) do { \
    if ((p) == NULL) { printf("  FAIL: %s is NULL (line %d)\n", #p, __LINE__); return 1; } \
} while(0)

// =============================================================================
// SemVer Tests
// =============================================================================

static int test_semver_parse(void) {
    SemVer v = semver_parse("1.2.3");
    EXPECT_EQ(v.major, 1);
    EXPECT_EQ(v.minor, 2);
    EXPECT_EQ(v.patch, 3);
    return 0;
}

static int test_semver_parse_prefix(void) {
    SemVer v = semver_parse("v2.0.1");
    EXPECT_EQ(v.major, 2);
    EXPECT_EQ(v.minor, 0);
    EXPECT_EQ(v.patch, 1);
    return 0;
}

static int test_semver_parse_prerelease(void) {
    SemVer v = semver_parse("1.0.0-beta.1");
    EXPECT_EQ(v.major, 1);
    EXPECT_EQ(v.minor, 0);
    EXPECT_EQ(v.patch, 0);
    EXPECT_NOT_NULL(v.prerelease);
    EXPECT_STR_EQ(v.prerelease, "beta.1");
    free((void*)v.prerelease);
    return 0;
}

static int test_semver_compare(void) {
    SemVer a = {1, 2, 3, NULL};
    SemVer b = {1, 2, 4, NULL};
    EXPECT_TRUE(semver_compare(a, b) < 0);
    EXPECT_TRUE(semver_compare(b, a) > 0);
    EXPECT_EQ(semver_compare(a, a), 0);
    return 0;
}

static int test_semver_compare_major(void) {
    SemVer a = {1, 0, 0, NULL};
    SemVer b = {2, 0, 0, NULL};
    EXPECT_TRUE(semver_compare(a, b) < 0);
    return 0;
}

static int test_semver_compatible(void) {
    SemVer required = {1, 2, 0, NULL};
    SemVer good = {1, 3, 0, NULL};
    SemVer bad_major = {2, 0, 0, NULL};
    SemVer too_old = {1, 1, 0, NULL};

    EXPECT_TRUE(semver_compatible(required, good));
    EXPECT_FALSE(semver_compatible(required, bad_major));
    EXPECT_FALSE(semver_compatible(required, too_old));
    return 0;
}

static int test_semver_to_string(void) {
    SemVer v = {1, 2, 3, NULL};
    char* s = semver_to_string(v);
    EXPECT_STR_EQ(s, "1.2.3");
    free(s);
    return 0;
}

// =============================================================================
// Manifest Tests
// =============================================================================

static int test_manifest_lifecycle(void) {
    SemVer v = {0, 1, 0, NULL};
    PackageManifest* m = manifest_new("my_project", v);
    EXPECT_NOT_NULL(m);
    EXPECT_STR_EQ(m->name, "my_project");
    EXPECT_EQ(m->version.major, 0);
    EXPECT_EQ(m->version.minor, 1);

    manifest_free(m);
    return 0;
}

static int test_manifest_add_deps(void) {
    SemVer v = {1, 0, 0, NULL};
    PackageManifest* m = manifest_new("app", v);

    manifest_add_dependency(m, "json", "1.2.0", DEP_STRATEGY_COMPATIBLE);
    manifest_add_dependency(m, "http", "2.0.0", DEP_STRATEGY_LATEST);

    EXPECT_EQ(m->dependency_count, (size_t)2);

    manifest_free(m);
    return 0;
}

// =============================================================================
// Package Manager Tests
// =============================================================================

static int test_package_manager_new(void) {
    PackageManager* pm = package_manager_new("/tmp/cache");
    EXPECT_NOT_NULL(pm);
    EXPECT_STR_EQ(pm->cache_dir, "/tmp/cache");
    EXPECT_TRUE(pm->block_known_vulnerable);

    package_manager_free(pm);
    return 0;
}

static int test_add_registry(void) {
    PackageManager* pm = package_manager_new(NULL);
    package_manager_add_registry(pm, "https://packages.goo.dev");
    package_manager_add_registry(pm, "https://mirror.goo.dev");

    EXPECT_EQ(pm->registry_count, (size_t)2);

    package_manager_free(pm);
    return 0;
}

// =============================================================================
// Resolution Tests
// =============================================================================

static int test_resolve_basic(void) {
    PackageManager* pm = package_manager_new(NULL);
    SemVer v = {1, 0, 0, NULL};
    PackageManifest* m = manifest_new("app", v);

    manifest_add_dependency(m, "json", "1.0.0", DEP_STRATEGY_COMPATIBLE);
    manifest_add_dependency(m, "http", "2.0.0", DEP_STRATEGY_EXACT);

    ResolveResult* result = resolve_dependencies(pm, m);
    EXPECT_NOT_NULL(result);
    EXPECT_EQ(result->status, RESOLVE_OK);
    EXPECT_EQ(result->resolved_count, (size_t)2);

    resolve_result_free(result);
    manifest_free(m);
    package_manager_free(pm);
    return 0;
}

static int test_resolve_security_block(void) {
    PackageManager* pm = package_manager_new(NULL);
    SemVer v = {1, 0, 0, NULL};
    PackageManifest* m = manifest_new("app", v);

    manifest_add_dependency(m, "vulnerable_pkg", "1.0.0", DEP_STRATEGY_EXACT);
    // Mark as vulnerable
    m->dependencies->has_security_advisory = true;
    m->dependencies->advisory_id = strdup("CVE-2024-1234");

    ResolveResult* result = resolve_dependencies(pm, m);
    EXPECT_NOT_NULL(result);
    EXPECT_EQ(result->status, RESOLVE_SECURITY_BLOCKED);

    resolve_result_free(result);
    manifest_free(m);
    package_manager_free(pm);
    return 0;
}

// =============================================================================
// Strategy Parsing Tests
// =============================================================================

static int test_parse_strategy_exact(void) {
    DependencyStrategy s;
    EXPECT_TRUE(parse_dependency_strategy("exact", &s));
    EXPECT_EQ(s, DEP_STRATEGY_EXACT);
    return 0;
}

static int test_parse_strategy_performance(void) {
    DependencyStrategy s;
    EXPECT_TRUE(parse_dependency_strategy("performance_optimized", &s));
    EXPECT_EQ(s, DEP_STRATEGY_PERFORMANCE);
    return 0;
}

static int test_parse_strategy_invalid(void) {
    DependencyStrategy s;
    EXPECT_FALSE(parse_dependency_strategy("nonexistent", &s));
    return 0;
}

// =============================================================================
// Test Runner
// =============================================================================

typedef struct { const char* name; int (*func)(void); } TestCase;

int main(void) {
    TestCase tests[] = {
        {"semver_parse",            test_semver_parse},
        {"semver_parse_prefix",     test_semver_parse_prefix},
        {"semver_parse_prerelease", test_semver_parse_prerelease},
        {"semver_compare",          test_semver_compare},
        {"semver_compare_major",    test_semver_compare_major},
        {"semver_compatible",       test_semver_compatible},
        {"semver_to_string",        test_semver_to_string},
        {"manifest_lifecycle",      test_manifest_lifecycle},
        {"manifest_add_deps",       test_manifest_add_deps},
        {"package_manager_new",     test_package_manager_new},
        {"add_registry",            test_add_registry},
        {"resolve_basic",           test_resolve_basic},
        {"resolve_security_block",  test_resolve_security_block},
        {"parse_strategy_exact",    test_parse_strategy_exact},
        {"parse_strategy_performance", test_parse_strategy_performance},
        {"parse_strategy_invalid",  test_parse_strategy_invalid},
    };

    size_t total = sizeof(tests) / sizeof(tests[0]);
    size_t passed = 0;

    printf("Running %zu package manager tests...\n\n", total);

    for (size_t i = 0; i < total; i++) {
        printf("[ RUN      ] pkg.%s\n", tests[i].name);
        int result = tests[i].func();
        if (result == 0) {
            printf("[\033[32m       OK\033[0m ] pkg.%s\n", tests[i].name);
            passed++;
        } else {
            printf("[\033[31m  FAILED  \033[0m ] pkg.%s\n", tests[i].name);
        }
    }

    printf("\nTests run: %zu\n\033[32mPassed: %zu\033[0m\n", total, passed);
    if (passed < total) printf("\033[31mFailed: %zu\033[0m\n", total - passed);

    return passed < total ? 1 : 0;
}
