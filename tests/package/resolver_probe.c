#include "import_resolver.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void) {
    setenv("GOOROOT", "tests/fixtures/goostd", 1);
    PackageSource ps;
    if (resolve_import("greet", &ps) != 0) { printf("RESOLVER FAIL: resolve\n"); return 1; }
    if (strcmp(ps.name, "greet") != 0) { printf("RESOLVER FAIL: name=%s\n", ps.name); return 1; }
    if (ps.file_count != 1) { printf("RESOLVER FAIL: count=%zu (expected 1, _test.go must be excluded)\n", ps.file_count); return 1; }
    package_source_free(&ps);
    printf("RESOLVER OK\n");
    return 0;
}
