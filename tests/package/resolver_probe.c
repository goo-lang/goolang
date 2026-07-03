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
    if (resolve_import("greet", &ps) != 0) { printf("RESOLVER FAIL: resolve\n"); return 1; }
    if (strcmp(ps.name, "greet") != 0) { printf("RESOLVER FAIL: name=%s\n", ps.name); return 1; }
    if (ps.file_count != 1) { printf("RESOLVER FAIL: count=%zu (expected 1, _test.go must be excluded)\n", ps.file_count); return 1; }
    package_source_free(&ps);
    printf("RESOLVER OK\n");
    return 0;
}
