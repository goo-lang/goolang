// The bump arena: alignment, growth, pointer stability, and reset.
//
// This is the first suite to name //src/runtime:arena. Before the runtime
// package was split, arena.c was one of twelve files inside a single target,
// and no test could name it -- which is why 134 lines of allocator with four
// exported functions had none.
//
// WHAT IS WORTH PINNING HERE. arena.c's own header makes one central promise:
// "pointers returned by goo_arena_alloc stay valid for the life of the arena".
// That is the property a bump allocator is easiest to get wrong, because the
// growth path is the only place it can break and the happy path never reaches
// it. Row 6 crosses a block boundary and then reads back every earlier
// allocation.
//
// CLAUDE.md records that arenas reclaim less than the syntax suggests -- an
// arena frees only what codegen allocates directly in the block, never what a
// runtime helper allocated. That is a codegen question and is NOT what this
// suite tests. This tests the allocator underneath, which is a smaller and
// answerable thing.
//
// A NOTE ON WHAT IS NOT ASSERTED. Nothing here checks how many malloc calls
// happen. The block-reuse path after a reset is observable only as an address
// coming back, which row 8 does check, and counting allocations would need the
// interception harness this repo does not have.

#include "runtime.h"
#include "../goo_check.h"
#include <stdint.h>
#include <string.h>

#define ALIGNMENT ((uintptr_t)16)

// The default block is 64 KiB (GOO_ARENA_DEFAULT_BLOCK_SIZE, arena.c). Kept as
// a local literal rather than exported: the test asserts behaviour ACROSS a
// block boundary, so it needs a size that certainly crosses one, not the exact
// constant. If the default shrinks, this still crosses it.
#define BIGGER_THAN_A_BLOCK ((size_t)(96 * 1024))

static int aligned16(const void* p) {
    return ((uintptr_t)p % ALIGNMENT) == 0;
}

int main(void) {
    goo_check_expect(9);

    // ---------------------------------------------------------------------
    goo_check_row(0, "NULL is tolerated by every entry point");
    goo_check(goo_arena_alloc(NULL, 8) == NULL, "alloc(NULL, 8) is NULL");
    goo_arena_reset(NULL);   // must not crash
    goo_arena_free(NULL);    // must not crash
    goo_check(1, "reset(NULL) and free(NULL) returned");

    // ---------------------------------------------------------------------
    goo_check_row(1, "a zero initial size still gives a usable arena");
    GooArena* a = goo_arena_new(0);
    goo_check(a != NULL, "arena_new(0) returned an arena");
    if (a == NULL) {
        return goo_check_done("arena");
    }
    void* first = goo_arena_alloc(a, 32);
    goo_check(first != NULL, "a 32-byte allocation succeeded");

    // ---------------------------------------------------------------------
    // Go requires a zero-size allocation to be non-nil. arena.c returns the
    // same sentinel goo_alloc uses, and says why at the return site.
    goo_check_row(2, "a zero-size allocation is the shared sentinel, not NULL");
    void* z1 = goo_arena_alloc(a, 0);
    void* z2 = goo_arena_alloc(a, 0);
    goo_check(z1 != NULL, "alloc(a, 0) is not NULL");
    goo_check(z1 == (void*)&goo_zerobase, "alloc(a, 0) is &goo_zerobase");
    goo_check(z1 == z2, "two zero-size allocations share one address");

    // ---------------------------------------------------------------------
    goo_check_row(3, "every allocation is 16-byte aligned");
    int all_aligned = aligned16(first);
    for (int i = 1; i <= 40; i++) {
        void* p = goo_arena_alloc(a, (size_t)i);   // deliberately odd sizes
        if (!aligned16(p)) {
            all_aligned = 0;
        }
    }
    goo_check(all_aligned, "41 allocations of assorted sizes were all aligned");

    // ---------------------------------------------------------------------
    goo_check_row(4, "distinct allocations do not overlap");
    unsigned char* p1 = goo_arena_alloc(a, 64);
    unsigned char* p2 = goo_arena_alloc(a, 64);
    goo_check(p1 != NULL && p2 != NULL, "both allocations succeeded");
    goo_check(p1 + 64 <= p2 || p2 + 64 <= p1, "the two 64-byte ranges are disjoint");

    // ---------------------------------------------------------------------
    goo_check_row(5, "an allocation is writable across its whole length");
    memset(p1, 0xAB, 64);
    memset(p2, 0xCD, 64);
    int intact = 1;
    for (int i = 0; i < 64; i++) {
        if (p1[i] != 0xAB || p2[i] != 0xCD) {
            intact = 0;
        }
    }
    goo_check(intact, "each byte held the value written to it");

    // ---------------------------------------------------------------------
    // THE LOAD-BEARING ROW. arena.c promises pointers stay valid for the life
    // of the arena, and the growth path is where that can break: it appends a
    // block rather than resizing, so an implementation that reallocated would
    // dangle every earlier pointer.
    goo_check_row(6, "pointers survive the growth that adds a new block");
    enum { KEEP = 24 };
    unsigned char* kept[KEEP];
    for (int i = 0; i < KEEP; i++) {
        kept[i] = goo_arena_alloc(a, 4096);
        if (kept[i]) {
            memset(kept[i], (unsigned char)(i + 1), 4096);
        }
    }
    int survived = 1;
    for (int i = 0; i < KEEP; i++) {
        if (!kept[i]) {
            survived = 0;
            continue;
        }
        for (int j = 0; j < 4096; j++) {
            if (kept[i][j] != (unsigned char)(i + 1)) {
                survived = 0;
                break;
            }
        }
    }
    goo_check(survived, "24 x 4096 bytes still read back after crossing blocks");
    goo_check(p1[0] == 0xAB && p2[0] == 0xCD, "the pre-growth allocations are intact too");

    // ---------------------------------------------------------------------
    goo_check_row(7, "a request larger than the default block size succeeds");
    unsigned char* big = goo_arena_alloc(a, BIGGER_THAN_A_BLOCK);
    goo_check(big != NULL, "a 96 KiB allocation succeeded");
    goo_check(aligned16(big), "the oversized allocation is aligned");
    if (big) {
        memset(big, 0x5A, BIGGER_THAN_A_BLOCK);
        goo_check(big[0] == 0x5A && big[BIGGER_THAN_A_BLOCK - 1] == 0x5A,
                  "its first and last byte are writable");
    }
    goo_arena_free(a);

    // ---------------------------------------------------------------------
    // reset rewinds to the first block and zeroes every block's used count, so
    // the next allocation must land where the first one did. That address
    // coming back is the only outside evidence that reset retains blocks
    // rather than freeing them.
    goo_check_row(8, "reset rewinds to the first address and reuses the blocks");
    GooArena* b = goo_arena_new(0);
    goo_check(b != NULL, "a second arena was created");
    if (b != NULL) {
        void* before = goo_arena_alloc(b, 128);
        for (int i = 0; i < 32; i++) {
            goo_arena_alloc(b, 4096);   // force at least one extra block
        }
        goo_arena_reset(b);
        void* after = goo_arena_alloc(b, 128);
        goo_check(before == after, "the first allocation after reset reuses the first address");

        unsigned char* q = goo_arena_alloc(b, 256);
        goo_check(q != NULL && aligned16(q), "allocation continues to work after reset");
        goo_arena_free(b);
    }

    return goo_check_done("arena");
}
