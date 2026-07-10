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

// Mirrors goo_runtime_archive_path()'s precedence (src/codegen/codegen.c),
// with one extra tier: env var -> exe-relative installed layout -> exe-
// relative dev-tree layout -> cwd-relative fallback. Here the target is a
// directory (GOOROOT), not a single archive file, so the fallback stays a
// bare relative path ("./goostd") rather than a repo-root-relative one.
//
// GOOROOT semantics: the directory CONTAINING goostd/ (i.e. GOOROOT/goostd
// must exist) — either the repo root in a dev checkout or the install
// prefix's lib/ dir, never goostd itself. The two exe-relative tiers below
// exist because the archive's installed layout (<exe-dir>/../lib/) and the
// dev tree's layout (<exe-dir>/../, i.e. repo root, since bin/ and goostd/
// are siblings there — unlike lib/, goostd was never nested under lib/ in
// the dev tree) don't coincide, so a plain `bin/goo` built from a fresh
// checkout couldn't find ../lib/goostd and fell back to cwd, breaking any
// invocation outside the repo root.
const char* goo_gooroot_dir(void) {
    static char buf[4096];
    const char* env = getenv("GOOROOT");
    // GOOROOT is the goostd PARENT (see contract above); every tier here
    // returns the goostd directory itself, since resolve_import() joins the
    // return value directly with the import path ("%s/%s") without an
    // extra "goostd" segment — so the env branch must append it too.
    if (env && env[0] != '\0') { snprintf(buf, sizeof(buf), "%s/goostd", env); return buf; }
#ifdef __linux__
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char* slash = strrchr(exe, '/');        // dir-of-exe
        if (slash) {
            *slash = '\0';
            struct stat st;
            snprintf(buf, sizeof(buf), "%s/../lib/goostd", exe);
            if (stat(buf, &st) == 0) return buf; // installed layout: <exe-dir>/../lib/goostd
            snprintf(buf, sizeof(buf), "%s/../goostd", exe);
            if (stat(buf, &st) == 0) return buf; // dev-tree layout: <exe-dir>/../goostd (repo root)
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

// P4.4 (import path aliases): a small table mapping a Go-style nested import
// spelling (as Go's real stdlib names it, e.g. "unicode/utf8") to the flat
// directory name goostd actually vendors it under (e.g. "utf8" —
// goostd/utf8/utf8.go). This is a SPELLING equivalence only, not Go's
// `import alias "path"` syntax: the flat spelling keeps working unchanged
// (existing probes), and both spellings resolve to the identical package
// (same short name, same exports, same mangled symbols) since
// PackageSource.name is independently derived from the last path segment
// (see below) and never depends on which spelling was used.
//
// Single choke point: this is the ONLY table of aliases in the codebase.
// Every site that compares or resolves a raw import-path string against a
// known set of names funnels through normalize_import_path() first (here,
// and defensively in goo.c's is_stdlib_shim_import) so a future alias entry
// never needs a second copy of this list.
typedef struct {
    const char* nested;      // Go-style nested spelling, as written in source
    const char* flat;        // goostd's actual flat directory/package name
} ImportPathAlias;

static const ImportPathAlias import_path_aliases[] = {
    {"unicode/utf8", "utf8"},
    {"math/bits", "bits"},
};

const char* normalize_import_path(const char* import_path) {
    if (!import_path) return import_path;
    for (size_t i = 0; i < sizeof(import_path_aliases) / sizeof(import_path_aliases[0]); i++) {
        if (strcmp(import_path, import_path_aliases[i].nested) == 0) {
            return import_path_aliases[i].flat;
        }
    }
    return import_path; // flat/unaliased spellings pass through unchanged
}

// Scan an already-resolved directory `pkg_dir` for non-_test.go *.go files
// and populate `out` on success. `import_path` is kept purely to derive
// out->name (last path segment) and out->import_path (recorded verbatim) —
// it plays no part in the filesystem lookup itself, which is entirely
// driven by `pkg_dir`. Returns 0 on success, 1 if the directory doesn't
// exist or holds no buildable *.go files (leaving `out` untouched, since
// every failure return below is before `out` is written — callers rely on
// this to retry a second tier with the same `out`).
static int resolve_package_dir(const char* pkg_dir, const char* import_path, PackageSource* out) {
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

    // Pass 2: turn each filename into a full (pkg_dir-relative) path.
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

int resolve_import(const char* import_path, const char* source_dir, PackageSource* out) {
    if (!out) return 1;
    out->files = NULL;
    out->file_count = 0;
    out->name = NULL;
    out->import_path = NULL;
    if (!import_path) return 1;

    // P4.5 (source-dir-relative imports): "./name" is the EXPLICIT local
    // spelling — resolved against source_dir ONLY, never GOOROOT. No
    // fallback: a typo'd "./name" should fail cleanly, not silently
    // resolve to an unrelated stdlib package of the same short name.
    if (import_path[0] == '.' && import_path[1] == '/') {
        if (!source_dir) return 1; // no source-dir context available (e.g. a resolver_probe-style direct caller) — unresolvable
        const char* rel = import_path + 2;
        if (rel[0] == '\0') return 1; // "./" with nothing after it
        char pkg_dir[4096];
        snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s", source_dir, rel);
        return resolve_package_dir(pkg_dir, import_path, out);
    }

    // Bare name: GOOROOT tiers first (unchanged precedence — see the design
    // doc's deliberate deviation note: source-dir-first would let a local
    // directory silently shadow a same-named stdlib package). Normalize
    // BEFORE the GOOROOT join only — out->import_path/out->name (derived
    // inside resolve_package_dir) keep the caller's original spelling;
    // out->name already equals the flat short name for every alias table
    // entry via its own last-path-segment derivation (e.g.
    // strrchr("unicode/utf8", '/') -> "utf8"), so no separate normalization
    // is needed there.
    const char* resolved_path = normalize_import_path(import_path);
    char gooroot_pkg_dir[4096];
    snprintf(gooroot_pkg_dir, sizeof(gooroot_pkg_dir), "%s/%s", goo_gooroot_dir(), resolved_path);
    if (resolve_package_dir(gooroot_pkg_dir, import_path, out) == 0) return 0;

    // LAST tier: the main .goo file's own directory, bare-name fallback.
    // Deliberately tried only after GOOROOT has already failed.
    if (source_dir) {
        char local_pkg_dir[4096];
        snprintf(local_pkg_dir, sizeof(local_pkg_dir), "%s/%s", source_dir, import_path);
        if (resolve_package_dir(local_pkg_dir, import_path, out) == 0) return 0;
    }

    return 1; // unresolvable in any tier
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
