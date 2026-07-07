// Bump/arena allocator: a growable block-list bump allocator that later
// arena-region tasks route allocations into. Blocks are never resized in
// place; when the current block can't satisfy a request we append a new
// one, so pointers returned by goo_arena_alloc stay valid for the life of
// the arena (until goo_arena_free).
#include "runtime.h"
#include <stdint.h>
#include <stdlib.h>

// Used when the caller-supplied initial_size is zero or small, and as the
// minimum size of every block added on growth.
#define GOO_ARENA_DEFAULT_BLOCK_SIZE ((size_t)(64 * 1024))
#define GOO_ARENA_ALIGNMENT ((size_t)16)

typedef struct GooArenaBlock {
    struct GooArenaBlock* next;
    unsigned char* data;   // malloc'd separately so its alignment doesn't
                            // depend on this struct's layout
    size_t capacity;
    size_t used;
} GooArenaBlock;

struct GooArena {
    GooArenaBlock* first;    // kept across goo_arena_reset; freed last
    GooArenaBlock* current;  // block bump-allocation is currently drawing from
    size_t default_block_size;
};

// Allocates a block with room for at least `capacity` bytes. Panics on OOM.
static GooArenaBlock* goo_arena_block_new(size_t capacity) {
    GooArenaBlock* block = malloc(sizeof(GooArenaBlock));
    if (!block) {
        goo_panic("Out of memory");
    }

    block->data = malloc(capacity);
    if (!block->data) {
        goo_panic("Out of memory");
    }

    block->next = NULL;
    block->capacity = capacity;
    block->used = 0;

    return block;
}

GooArena* goo_arena_new(size_t initial_size) {
    size_t block_size = initial_size < GOO_ARENA_DEFAULT_BLOCK_SIZE
        ? GOO_ARENA_DEFAULT_BLOCK_SIZE
        : initial_size;
    // Alignment slack: goo_arena_alloc may need to round the first pointer
    // it hands out up to GOO_ARENA_ALIGNMENT, which would overflow a block
    // sized exactly block_size.
    block_size += GOO_ARENA_ALIGNMENT;

    GooArena* arena = malloc(sizeof(GooArena));
    if (!arena) {
        goo_panic("Out of memory");
    }

    arena->first = goo_arena_block_new(block_size);
    arena->current = arena->first;
    arena->default_block_size = GOO_ARENA_DEFAULT_BLOCK_SIZE;

    return arena;
}

void* goo_arena_alloc(GooArena* a, size_t size) {
    if (!a || size == 0) {
        return NULL;
    }

    GooArenaBlock* block = a->current;
    uintptr_t cursor = (uintptr_t)block->data + block->used;
    uintptr_t aligned = (cursor + (GOO_ARENA_ALIGNMENT - 1)) & ~(uintptr_t)(GOO_ARENA_ALIGNMENT - 1);
    size_t padding = (size_t)(aligned - cursor);

    if (padding + size <= block->capacity - block->used) {
        block->used += padding + size;
        return (void*)aligned;
    }

    // Current block can't satisfy this request: grow. Reuse a retained
    // block from a prior goo_arena_reset if one is already big enough,
    // otherwise append a freshly allocated one right after `block`.
    size_t needed = size + GOO_ARENA_ALIGNMENT;  // slack for alignment
    GooArenaBlock* next = block->next;
    if (!next || next->capacity < needed) {
        size_t new_capacity = needed > a->default_block_size ? needed : a->default_block_size;
        next = goo_arena_block_new(new_capacity);
        next->next = block->next;
        block->next = next;
    }
    a->current = next;

    return goo_arena_alloc(a, size);
}

void goo_arena_reset(GooArena* a) {
    if (!a) {
        return;
    }

    for (GooArenaBlock* block = a->first; block != NULL; block = block->next) {
        block->used = 0;
    }
    a->current = a->first;
}

void goo_arena_free(GooArena* a) {
    if (!a) {
        return;
    }

    GooArenaBlock* block = a->first;
    while (block) {
        GooArenaBlock* next = block->next;
        free(block->data);
        free(block);
        block = next;
    }

    free(a);
}
