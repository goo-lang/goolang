#include "runtime.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    GooMapSV* m = goo_map_new_sv();
    goo_map_set_sv(m, "a", 42);

    int64_t v = 999; int found = 7;
    goo_map_get_sv_ok(m, "a", &v, &found);
    assert(found == 1 && v == 42);

    v = 999; found = 7;
    goo_map_get_sv_ok(m, "missing", &v, &found);
    assert(found == 0 && v == 0);

    printf("test_map_get_ok: PASS\n");
    return 0;
}
