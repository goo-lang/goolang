// Proves the prelude reaches a translation unit that includes neither header.
// Makefile:23 forces xalloc.h and goo_assert.h in with -include; this file is
// the Bazel-side check that goo_cc_library/goo_cc_test do the same.
//
// Three things are being asserted at once, and all three fail loudly under
// C23, which removed implicit function declarations:
//   xmalloc     -- comes from xalloc.h
//   GOO_ASSERT  -- comes from goo_assert.h
//   free        -- comes from <stdlib.h>, which xalloc.h includes, so this
//                  also pins that the prelude arrives with its own includes
//                  rather than as a bare declaration.
#include <stddef.h>

int main(void) {
    void *p = xmalloc(16);
    GOO_ASSERT(p != NULL);
    free(p);
    return 0;
}
