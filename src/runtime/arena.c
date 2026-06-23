#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

// =============================================================================
// Arena Allocation System Implementation
// =============================================================================

#define ARENA_DEFAULT_SIZE (64 * 1024)    // 64KB default arena size
#define ARENA_ALIGNMENT 8                 // 8-byte alignment for all allocations
#define MAX_ARENA_STACK_DEPTH 256         // Maximum nested arena depth

// Arena block structure
typedef struct ArenaBlock {
    void* memory;                         // Allocated memory block
    size_t size;                         // Total size of the block
    size_t used;                         // Current offset in the block
    struct ArenaBlock* next;             // Next block in the chain
} ArenaBlock;

// Arena allocator structure
typedef struct Arena {
    ArenaBlock* current_block;           // Current allocation block
    ArenaBlock* first_block;             // First block in the chain
    size_t default_block_size;           // Default size for new blocks
    size_t total_allocated;              // Total memory allocated
    size_t total_used;                   // Total memory actually used
    size_t allocation_count;             // Number of allocations made
    char* debug_name;                    // Arena name for debugging
    struct Arena* parent;                // Parent arena for nested scopes
    int scope_id;                        // Unique scope identifier
} Arena;

// Global arena stack for scope management
static Arena* g_arena_stack[MAX_ARENA_STACK_DEPTH];
static int g_arena_stack_depth = 0;
static int g_next_scope_id = 1;

// Statistics tracking
static struct {
    size_t total_arenas_created;
    size_t total_arenas_destroyed;
    size_t total_memory_allocated;
    size_t total_memory_freed;
    size_t peak_memory_usage;
    size_t current_memory_usage;
    size_t allocation_count;
    size_t deallocation_count;
} g_arena_stats = {0};

// =============================================================================
// Arena Block Management
// =============================================================================

static ArenaBlock* arena_block_new(size_t size) {
    ArenaBlock* block = malloc(sizeof(ArenaBlock));
    if (!block) return NULL;
    
    block->memory = malloc(size);
    if (!block->memory) {
        free(block);
        return NULL;
    }
    
    block->size = size;
    block->used = 0;
    block->next = NULL;
    
    g_arena_stats.total_memory_allocated += size;
    g_arena_stats.current_memory_usage += size;
    if (g_arena_stats.current_memory_usage > g_arena_stats.peak_memory_usage) {
        g_arena_stats.peak_memory_usage = g_arena_stats.current_memory_usage;
    }
    
    return block;
}

static void arena_block_free(ArenaBlock* block) {
    if (!block) return;
    
    g_arena_stats.total_memory_freed += block->size;
    g_arena_stats.current_memory_usage -= block->size;
    g_arena_stats.deallocation_count++;
    
    free(block->memory);
    free(block);
}

static void arena_block_free_chain(ArenaBlock* block) {
    while (block) {
        ArenaBlock* next = block->next;
        arena_block_free(block);
        block = next;
    }
}

// =============================================================================
// Arena Management
// =============================================================================

Arena* goo_arena_new(size_t initial_size, const char* debug_name) {
    Arena* arena = malloc(sizeof(Arena));
    if (!arena) return NULL;
    
    if (initial_size == 0) {
        initial_size = ARENA_DEFAULT_SIZE;
    }
    
    arena->current_block = arena_block_new(initial_size);
    if (!arena->current_block) {
        free(arena);
        return NULL;
    }
    
    arena->first_block = arena->current_block;
    arena->default_block_size = initial_size;
    arena->total_allocated = initial_size;
    arena->total_used = 0;
    arena->allocation_count = 0;
    arena->parent = NULL;
    arena->scope_id = g_next_scope_id++;
    
    // Copy debug name
    if (debug_name) {
        size_t name_len = strlen(debug_name);
        arena->debug_name = malloc(name_len + 1);
        if (arena->debug_name) {
            strcpy(arena->debug_name, debug_name);
        }
    } else {
        arena->debug_name = NULL;
    }
    
    g_arena_stats.total_arenas_created++;
    
    return arena;
}

void goo_arena_free(Arena* arena) {
    if (!arena) return;
    
    // Free all blocks
    arena_block_free_chain(arena->first_block);
    
    // Free debug name
    if (arena->debug_name) {
        free(arena->debug_name);
    }
    
    g_arena_stats.total_arenas_destroyed++;
    free(arena);
}

void goo_arena_reset(Arena* arena) {
    if (!arena) return;
    
    // Reset all blocks to unused
    ArenaBlock* block = arena->first_block;
    while (block) {
        block->used = 0;
        block = block->next;
    }
    
    arena->current_block = arena->first_block;
    arena->total_used = 0;
    arena->allocation_count = 0;
}

// =============================================================================
// Arena Allocation
// =============================================================================

static size_t align_size(size_t size) {
    return (size + ARENA_ALIGNMENT - 1) & ~(ARENA_ALIGNMENT - 1);
}

void* goo_arena_alloc(Arena* arena, size_t size) {
    if (!arena || size == 0) return NULL;
    
    size = align_size(size);
    
    // Check if current block has enough space
    ArenaBlock* block = arena->current_block;
    if (block->used + size > block->size) {
        // Need a new block
        size_t new_block_size = arena->default_block_size;
        if (size > new_block_size) {
            new_block_size = size * 2; // Ensure we have enough space
        }
        
        ArenaBlock* new_block = arena_block_new(new_block_size);
        if (!new_block) return NULL;
        
        // Link the new block
        block->next = new_block;
        arena->current_block = new_block;
        arena->total_allocated += new_block_size;
        block = new_block;
    }
    
    // Allocate from current block
    void* ptr = (char*)block->memory + block->used;
    block->used += size;
    arena->total_used += size;
    arena->allocation_count++;
    g_arena_stats.allocation_count++;
    
    return ptr;
}

void* goo_arena_alloc_zero(Arena* arena, size_t size) {
    void* ptr = goo_arena_alloc(arena, size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

char* goo_arena_strdup(Arena* arena, const char* str) {
    if (!arena || !str) return NULL;
    
    size_t len = strlen(str);
    char* copy = goo_arena_alloc(arena, len + 1);
    if (copy) {
        strcpy(copy, str);
    }
    return copy;
}

// =============================================================================
// Scope Management
// =============================================================================

Arena* goo_arena_push_scope(size_t initial_size, const char* debug_name) {
    if (g_arena_stack_depth >= MAX_ARENA_STACK_DEPTH) {
        fprintf(stderr, "Error: Arena stack overflow (max depth %d)\n", MAX_ARENA_STACK_DEPTH);
        return NULL;
    }
    
    Arena* arena = goo_arena_new(initial_size, debug_name);
    if (!arena) {
        fprintf(stderr, "Error: Failed to create arena for scope\n");
        return NULL;
    }
    
    // Set parent relationship
    if (g_arena_stack_depth > 0) {
        arena->parent = g_arena_stack[g_arena_stack_depth - 1];
    }
    
    g_arena_stack[g_arena_stack_depth++] = arena;
    return arena;
}

void goo_arena_pop_scope(void) {
    if (g_arena_stack_depth <= 0) {
        fprintf(stderr, "Error: Attempt to pop from empty arena stack\n");
        return;
    }
    
    Arena* arena = g_arena_stack[--g_arena_stack_depth];
    if (arena) {
        goo_arena_free(arena);
    }
}

Arena* goo_arena_current_scope(void) {
    if (g_arena_stack_depth <= 0) return NULL;
    return g_arena_stack[g_arena_stack_depth - 1];
}

int goo_arena_scope_depth(void) {
    return g_arena_stack_depth;
}

// =============================================================================
// Convenience Allocators
// =============================================================================

void* goo_arena_alloc_current(size_t size) {
    Arena* arena = goo_arena_current_scope();
    if (!arena) {
        fprintf(stderr, "Error: No current arena scope for allocation\n");
        return NULL;
    }
    return goo_arena_alloc(arena, size);
}

void* goo_arena_alloc_zero_current(size_t size) {
    Arena* arena = goo_arena_current_scope();
    if (!arena) {
        fprintf(stderr, "Error: No current arena scope for allocation\n");
        return NULL;
    }
    return goo_arena_alloc_zero(arena, size);
}

char* goo_arena_strdup_current(const char* str) {
    Arena* arena = goo_arena_current_scope();
    if (!arena) {
        fprintf(stderr, "Error: No current arena scope for allocation\n");
        return NULL;
    }
    return goo_arena_strdup(arena, str);
}

// =============================================================================
// Array Allocation Helpers
// =============================================================================

void* goo_arena_alloc_array(Arena* arena, size_t count, size_t element_size) {
    if (count == 0 || element_size == 0) return NULL;
    
    // Check for overflow
    if (count > SIZE_MAX / element_size) {
        fprintf(stderr, "Error: Array allocation would overflow\n");
        return NULL;
    }
    
    return goo_arena_alloc(arena, count * element_size);
}

void* goo_arena_alloc_array_zero(Arena* arena, size_t count, size_t element_size) {
    if (count == 0 || element_size == 0) return NULL;
    
    // Check for overflow
    if (count > SIZE_MAX / element_size) {
        fprintf(stderr, "Error: Array allocation would overflow\n");
        return NULL;
    }
    
    return goo_arena_alloc_zero(arena, count * element_size);
}

// =============================================================================
// Statistics and Debugging
// =============================================================================

void goo_arena_print_stats(const Arena* arena) {
    if (!arena) return;
    
    printf("=== Arena Statistics ===\n");
    printf("Arena scope ID: %d\n", arena->scope_id);
    printf("Debug name: %s\n", arena->debug_name ? arena->debug_name : "<unnamed>");
    printf("Total allocated: %zu bytes\n", arena->total_allocated);
    printf("Total used: %zu bytes\n", arena->total_used);
    printf("Allocation count: %zu\n", arena->allocation_count);
    printf("Efficiency: %.1f%%\n", 
           arena->total_allocated > 0 ? 
           (double)arena->total_used / arena->total_allocated * 100.0 : 0.0);
    
    // Count blocks
    int block_count = 0;
    ArenaBlock* block = arena->first_block;
    while (block) {
        block_count++;
        block = block->next;
    }
    printf("Block count: %d\n", block_count);
    printf("Default block size: %zu bytes\n", arena->default_block_size);
}

void goo_arena_print_global_stats(void) {
    printf("=== Global Arena Statistics ===\n");
    printf("Arenas created: %zu\n", g_arena_stats.total_arenas_created);
    printf("Arenas destroyed: %zu\n", g_arena_stats.total_arenas_destroyed);
    printf("Active arenas: %zu\n", 
           g_arena_stats.total_arenas_created - g_arena_stats.total_arenas_destroyed);
    printf("Total memory allocated: %zu bytes\n", g_arena_stats.total_memory_allocated);
    printf("Total memory freed: %zu bytes\n", g_arena_stats.total_memory_freed);
    printf("Current memory usage: %zu bytes\n", g_arena_stats.current_memory_usage);
    printf("Peak memory usage: %zu bytes\n", g_arena_stats.peak_memory_usage);
    printf("Total allocations: %zu\n", g_arena_stats.allocation_count);
    printf("Total deallocations: %zu\n", g_arena_stats.deallocation_count);
    printf("Current arena stack depth: %d\n", g_arena_stack_depth);
}

void goo_arena_print_scope_stack(void) {
    printf("=== Arena Scope Stack ===\n");
    printf("Stack depth: %d\n", g_arena_stack_depth);
    
    for (int i = 0; i < g_arena_stack_depth; i++) {
        Arena* arena = g_arena_stack[i];
        printf("  [%d] %s (ID: %d, Used: %zu/%zu bytes)\n", 
               i,
               arena->debug_name ? arena->debug_name : "<unnamed>",
               arena->scope_id,
               arena->total_used,
               arena->total_allocated);
    }
}

// =============================================================================
// Integration with Goo Runtime
// =============================================================================

void goo_arena_init_system(void) {
    g_arena_stack_depth = 0;
    g_next_scope_id = 1;
    memset(&g_arena_stats, 0, sizeof(g_arena_stats));

    if (goo_runtime_verbose()) {
        printf("🏟️  Arena allocation system initialized\n");
    }
}

void goo_arena_cleanup_system(void) {
    // Clean up any remaining arenas
    while (g_arena_stack_depth > 0) {
        goo_arena_pop_scope();
    }
    
    if (goo_runtime_verbose()) {
        printf("🏟️  Arena allocation system cleaned up\n");
        printf("📊 Final arena statistics:\n");
        goo_arena_print_global_stats();
    }
}

// =============================================================================
// Debugging and Validation
// =============================================================================

int goo_arena_validate(const Arena* arena) {
    if (!arena) return 0;
    
    size_t total_size = 0;
    size_t total_used = 0;
    int block_count = 0;
    
    ArenaBlock* block = arena->first_block;
    while (block) {
        // Validate block
        if (!block->memory) {
            fprintf(stderr, "Error: Arena block %d has NULL memory\n", block_count);
            return 0;
        }
        
        if (block->used > block->size) {
            fprintf(stderr, "Error: Arena block %d has used > size (%zu > %zu)\n", 
                   block_count, block->used, block->size);
            return 0;
        }
        
        total_size += block->size;
        total_used += block->used;
        block_count++;
        block = block->next;
    }
    
    if (total_size != arena->total_allocated) {
        fprintf(stderr, "Error: Arena total_allocated mismatch (%zu != %zu)\n",
               arena->total_allocated, total_size);
        return 0;
    }
    
    if (total_used != arena->total_used) {
        fprintf(stderr, "Error: Arena total_used mismatch (%zu != %zu)\n",
               arena->total_used, total_used);
        return 0;
    }
    
    return 1;
}

void goo_arena_dump_blocks(const Arena* arena) {
    if (!arena) return;
    
    printf("=== Arena Block Dump ===\n");
    printf("Arena: %s (ID: %d)\n", 
           arena->debug_name ? arena->debug_name : "<unnamed>",
           arena->scope_id);
    
    int block_num = 0;
    ArenaBlock* block = arena->first_block;
    while (block) {
        printf("Block %d: %p, Size: %zu, Used: %zu (%.1f%%)\n",
               block_num,
               block->memory,
               block->size,
               block->used,
               (double)block->used / block->size * 100.0);
        
        block_num++;
        block = block->next;
    }
}