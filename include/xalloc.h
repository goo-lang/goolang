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
#include <string.h>

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

// strdup is variable-sized, so the rule above would exempt it — but it is the
// one variable-sized allocation that belongs here anyway. A strdup result in
// this tree is an identifier, a type name or a diagnostic string, and NOT ONE
// of the 570 call sites swept in this commit checked the result: a NULL went
// straight into a struct field, to be dereferenced later with no context left
// to report. There is no recovery to preserve, so fail fast like the three
// above.
//
// Implemented with malloc + memcpy rather than by calling strdup, so that
// scripts/alloc_doors_probe.sh can ban the bare name across src/ without
// needing an exemption for this header.
//
// NULL INPUT remains the caller's responsibility, exactly as with libc strdup.
// This wrapper handles allocation failure and nothing else. Accepting NULL and
// returning NULL would give a defined result to a different bug, and would put
// back the unchecked-NULL-in-a-struct-field shape this exists to remove.
static inline char *xstrdup(const char *s) {
    size_t len = strlen(s) + 1;
    // Cast is redundant in C but keeps this header usable from a C++ TU, which
    // matters because it is force-included into EVERY translation unit.
    char *p = (char *)malloc(len);
    if (!p) {
        fprintf(stderr, "goo: out of memory duplicating %zu bytes\n", len);
        exit(1);
    }
    memcpy(p, s, len);
    return p;
}

#endif // GOO_XALLOC_H
