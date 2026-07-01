#include "import_resolver.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __linux__
#include <unistd.h>
#endif

// Per-file static strdup — house idiom (see ast_constructors.c, types.c,
// ide/*.c) rather than relying on POSIX strdup, to avoid -std=c23
// feature-macro friction over _POSIX_C_SOURCE/_DEFAULT_SOURCE.
static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        memcpy(dup, str, len + 1);
    }
    return dup;
}

// Mirrors goo_runtime_archive_path()'s precedence (src/codegen/codegen.c):
// env var -> exe-relative (via /proc/self/exe on Linux) -> cwd-relative
// fallback. Here the target is a directory (GOOROOT), not a single
// archive file, so the fallback stays a bare relative path ("./goostd")
// rather than a repo-root-relative one.
const char* goo_gooroot_dir(void) {
    static char buf[4096];
    const char* env = getenv("GOOROOT");
    if (env && env[0] != '\0') { snprintf(buf, sizeof(buf), "%s", env); return buf; }
#ifdef __linux__
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char* slash = strrchr(exe, '/');        // dir-of-exe
        if (slash) {
            *slash = '\0';
            snprintf(buf, sizeof(buf), "%s/../lib/goostd", exe);
            struct stat st;
            if (stat(buf, &st) == 0) return buf; // exists relative to the binary
        }
    }
#endif
    snprintf(buf, sizeof(buf), "./goostd");  // fallback (cwd-relative)
    return buf;
}

// qsort comparator for the collected *.go filenames — determinism, not
// filesystem readdir() order (which is unspecified).
static int cmp_str_ptr(const void* a, const void* b) {
    const char* const* sa = (const char* const*)a;
    const char* const* sb = (const char* const*)b;
    return strcmp(*sa, *sb);
}

static int has_suffix(const char* s, size_t s_len, const char* suffix, size_t suffix_len) {
    if (s_len < suffix_len) return 0;
    return memcmp(s + s_len - suffix_len, suffix, suffix_len) == 0;
}

static void free_str_array(char** arr, size_t count) {
    if (!arr) return;
    for (size_t i = 0; i < count; i++) free(arr[i]);
    free(arr);
}

int resolve_import(const char* import_path, PackageSource* out) {
    if (!out) return 1;
    out->files = NULL;
    out->file_count = 0;
    out->name = NULL;
    out->import_path = NULL;
    if (!import_path) return 1;

    char pkg_dir[4096];
    snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s", goo_gooroot_dir(), import_path);

    DIR* dir = opendir(pkg_dir);
    if (!dir) return 1; // package directory doesn't exist

    // Pass 1: collect candidate *.go filenames (excluding *_test.go).
    char** names = NULL;
    size_t count = 0;
    size_t cap = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* fname = entry->d_name;
        size_t len = strlen(fname);
        if (!has_suffix(fname, len, ".go", 3)) continue;
        if (has_suffix(fname, len, "_test.go", 8)) continue;

        if (count == cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            char** grown = realloc(names, new_cap * sizeof(char*));
            if (!grown) { free_str_array(names, count); closedir(dir); return 1; }
            names = grown;
            cap = new_cap;
        }
        names[count] = str_dup(fname);
        if (!names[count]) { free_str_array(names, count); closedir(dir); return 1; }
        count++;
    }
    closedir(dir);

    if (count == 0) {
        free_str_array(names, count);
        return 1; // no non-_test.go *.go files — nothing to compile
    }

    qsort(names, count, sizeof(char*), cmp_str_ptr);

    // Pass 2: turn each filename into a full (gooroot-relative) path.
    char** files = malloc(count * sizeof(char*));
    if (!files) { free_str_array(names, count); return 1; }
    for (size_t i = 0; i < count; i++) {
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", pkg_dir, names[i]);
        files[i] = str_dup(full_path);
        if (!files[i]) { free_str_array(names, count); free_str_array(files, i); return 1; }
    }
    free_str_array(names, count);

    const char* last_slash = strrchr(import_path, '/');
    const char* short_name = last_slash ? last_slash + 1 : import_path;

    out->files = files;
    out->file_count = count;
    out->name = str_dup(short_name);
    out->import_path = str_dup(import_path);
    if (!out->name || !out->import_path) {
        package_source_free(out);
        return 1;
    }
    return 0;
}

void package_source_free(PackageSource* p) {
    if (!p) return;
    free_str_array(p->files, p->file_count);
    free(p->name);
    free(p->import_path);
    p->files = NULL;
    p->file_count = 0;
    p->name = NULL;
    p->import_path = NULL;
}
