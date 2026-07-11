#ifndef GOO_XALLOC_H
#define GOO_XALLOC_H

// Checked allocators. A fixed-size struct/object allocation in the compiler
// cannot meaningfully recover from OOM, so these fail fast with a clear message
// instead of returning NULL — which the (historically unchecked) call sites
// would then dereference. Made available in every translation unit via
// `-include include/xalloc.h` in the Makefile, mirroring ccomp_shim.h.
//
// Only fixed-size allocations (malloc(sizeof(T)), calloc(1, sizeof(T))) are
// swept to these; variable/input-sized allocations keep explicit NULL handling
// where recovery or graceful degradation matters.

#include <stdio.h>
#include <stdlib.h>

static inline void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) {
        fprintf(stderr, "goo: out of memory allocating %zu bytes\n", size);
        exit(1);
    }
    return p;
}

static inline void *xcalloc(size_t nmemb, size_t size) {
    void *p = calloc(nmemb, size);
    if (!p) {
        fprintf(stderr, "goo: out of memory allocating %zu x %zu bytes\n", nmemb, size);
        exit(1);
    }
    return p;
}

static inline void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (!p) {
        fprintf(stderr, "goo: out of memory reallocating %zu bytes\n", size);
        exit(1);
    }
    return p;
}

#endif // GOO_XALLOC_H
