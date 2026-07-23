#ifndef IMPORT_RESOLVER_H
#define IMPORT_RESOLVER_H

#include <stddef.h>

// A resolved Go-style package: a directory of *.go source files (Go
// semantics — a package is a directory, not a single file; *_test.go
// siblings are excluded from the build set).
typedef struct {
    char** files;        // sorted, strdup'd absolute/relative paths to each *.go file
    size_t file_count;
    char* name;           // package short name (last path segment of import_path)
    char* import_path;    // strdup'd copy of the import path passed to resolve_import
} PackageSource;

// Resolve `import_path` (e.g. "greet" or "encoding/json") to its source
// files. `source_dir` is the directory containing the main .goo file being
// compiled (NULL if unavailable/not applicable, e.g. a caller with no source
// file of its own). Resolution (P4.5):
//   - "./name" (leading dot-slash): resolved against source_dir ONLY. No
//     GOOROOT fallback — this is the explicit "local package" spelling.
//     Fails (returns non-0) if source_dir is NULL.
//   - bare "name": GOOROOT tiers first (unchanged precedence — a local
//     directory must not silently shadow a same-named stdlib package),
//     then source_dir as a LAST-tier fallback if GOOROOT resolution fails.
// Returns 0 on success with `out` fully populated; returns non-0 if no tier
// resolves the path, or its directory contains no non-_test.go *.go files.
// `out` is left zeroed on failure.
int resolve_import(const char* import_path, const char* source_dir, PackageSource* out);

// P4.4: map a Go-style nested import spelling (e.g. "unicode/utf8") to the
// flat name goostd actually vendors it under (e.g. "utf8"). Returns
// `import_path` unchanged for any spelling not in the alias table (including
// every flat spelling already in use). The single choke point for import-
// path-spelling comparisons; see the table + comment in import_resolver.c.
const char* normalize_import_path(const char* import_path);

// Free everything owned by a PackageSource populated by resolve_import
// (the file path array + each string, `name`, `import_path`). Safe to
// call on a zeroed/already-freed PackageSource or a NULL pointer.
void package_source_free(PackageSource* p);

// Resolve the GOOROOT directory (the root under which package import
// paths are looked up), in precedence order:
//   1. $GOOROOT environment variable, if set and non-empty.
//   2. <dir-of-the-running-executable>/../lib/goostd (via /proc/self/exe
//      on Linux), if that directory exists — mirrors
//      goo_runtime_archive_path()'s exe-relative resolution in
//      src/codegen/codegen.c.
//   3. "./goostd" (cwd-relative fallback).
// Returns a pointer to an internal static buffer — not thread-safe,
// callers must copy the result before a subsequent call overwrites it.
const char* goo_gooroot_dir(void);

#endif // IMPORT_RESOLVER_H
