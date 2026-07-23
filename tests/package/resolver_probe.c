#include "import_resolver.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void) {
    // GOOROOT is the directory CONTAINING goostd/ (repo root or install
    // prefix), not goostd itself — see goo_gooroot_dir()'s contract in
    // src/package/import_resolver.c. The fixture package lives at
    // tests/fixtures/goostd/greet, so GOOROOT is its parent.
    setenv("GOOROOT", "tests/fixtures", 1);
    PackageSource ps;
    if (resolve_import("greet", NULL, &ps) != 0) { printf("RESOLVER FAIL: resolve\n"); return 1; }
    if (strcmp(ps.name, "greet") != 0) { printf("RESOLVER FAIL: name=%s\n", ps.name); return 1; }
    if (ps.file_count != 1) { printf("RESOLVER FAIL: count=%zu (expected 1, _test.go must be excluded)\n", ps.file_count); return 1; }
    package_source_free(&ps);

    // P4.5 (source-dir-relative imports). Fixtures live under
    // tests/fixtures/localsrc/*, disjoint from tests/fixtures/goostd/* except
    // for the deliberately-duplicated "greet" name used by the shadowing
    // assertion below.
    const char* SRC_DIR = "tests/fixtures/localsrc";

    // "./name" resolves ONLY against source_dir, never GOOROOT.
    if (resolve_import("./relonly", SRC_DIR, &ps) != 0) { printf("RESOLVER FAIL: ./relonly should resolve via source_dir\n"); return 1; }
    if (strcmp(ps.name, "relonly") != 0) { printf("RESOLVER FAIL: ./relonly name=%s\n", ps.name); return 1; }
    package_source_free(&ps);

    // "./name" with no source_dir context is unresolvable (no GOOROOT fallback).
    if (resolve_import("./relonly", NULL, &ps) == 0) { printf("RESOLVER FAIL: ./relonly should NOT resolve without a source_dir\n"); return 1; }

    // Bare name: GOOROOT tiers first, source_dir as a LAST-tier fallback.
    // "onlylocal" exists only under SRC_DIR, not under GOOROOT.
    if (resolve_import("onlylocal", SRC_DIR, &ps) != 0) { printf("RESOLVER FAIL: bare onlylocal should fall back to source_dir\n"); return 1; }
    if (strcmp(ps.name, "onlylocal") != 0) { printf("RESOLVER FAIL: onlylocal name=%s\n", ps.name); return 1; }
    package_source_free(&ps);

    // Bare name with no source_dir and no GOOROOT match: unresolvable.
    if (resolve_import("onlylocal", NULL, &ps) == 0) { printf("RESOLVER FAIL: bare onlylocal should NOT resolve without a source_dir fallback\n"); return 1; }

    // Shadowing prevention: "greet" exists under BOTH GOOROOT
    // (tests/fixtures/goostd/greet) and SRC_DIR (tests/fixtures/localsrc/greet)
    // with different file counts (2 vs 1) — GOOROOT must win for a bare name.
    if (resolve_import("greet", SRC_DIR, &ps) != 0) { printf("RESOLVER FAIL: bare greet (shadow case) should resolve\n"); return 1; }
    if (ps.file_count != 1) { printf("RESOLVER FAIL: bare greet should resolve to the GOOROOT copy (file_count=1), got %zu — local copy shadowed the stdlib\n", ps.file_count); return 1; }
    package_source_free(&ps);

    printf("RESOLVER OK\n");
    return 0;
}
