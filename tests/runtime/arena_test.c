// Stress test for the bump/arena allocator (src/runtime/arena.c). Not part
// of `make test` -- compiled and run directly under valgrind:
//
//   make runtime-lib
//   gcc -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/runtime/arena_test.c
//       lib/libgoo_runtime.a -lpthread -lm -o build/arena_test
//   valgrind --leak-check=full --error-exitcode=99 build/arena_test
//
// (line-wrapped above for readability; run it as one line, or drop the
// trailing backslashes -- a `//` comment can't span lines with them.)
#include "runtime.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NUM_ALLOCS 10000

int main(void) {
    GooArena* a = goo_arena_new(0);
    assert(a != NULL);

    void* ptrs[NUM_ALLOCS];
    size_t sizes[NUM_ALLOCS];

    // Allocate many objects of varying sizes, several times larger in total
    // than the default block size, to force multiple block growths.
    for (int i = 0; i < NUM_ALLOCS; i++) {
        size_t size = (size_t)((i % 251) + 1);  // 1..251 bytes
        void* p = goo_arena_alloc(a, size);
        assert(p != NULL);

        // Every pointer handed out must be 16-byte aligned.
        assert(((uintptr_t)p % 16) == 0);

        // Prove the memory is usable: write and read back a distinct
        // pattern per allocation.
        memset(p, (int)(i & 0xFF), size);

        ptrs[i] = p;
        sizes[i] = size;
    }

    for (int i = 0; i < NUM_ALLOCS; i++) {
        unsigned char* p = ptrs[i];
        unsigned char expected = (unsigned char)(i & 0xFF);
        for (size_t j = 0; j < sizes[i]; j++) {
            assert(p[j] == expected);
        }
    }

    // size == 0 mirrors goo_alloc's convention: NULL, no allocation.
    assert(goo_arena_alloc(a, 0) == NULL);

    // Reset must not free any block; the arena should be reusable
    // immediately with no extra malloc traffic required to satisfy the
    // same workload again.
    goo_arena_reset(a);

    for (int i = 0; i < NUM_ALLOCS; i++) {
        size_t size = (size_t)((i % 97) + 1);  // different pattern than before
        void* p = goo_arena_alloc(a, size);
        assert(p != NULL);
        assert(((uintptr_t)p % 16) == 0);
        memset(p, (int)((i + 1) & 0xFF), size);
    }

    goo_arena_free(a);

    printf("arena_test: PASS\n");
    return 0;
}
