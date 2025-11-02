#include "ast_safety.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

// Global tracking for allocated nodes
static SafeASTNode** allocated_nodes = NULL;
static size_t allocated_count = 0;
static size_t allocated_capacity = 0;

// Add node to global tracking
static void track_node(SafeASTNode* node) {
    if (allocated_count >= allocated_capacity) {
        allocated_capacity = allocated_capacity ? allocated_capacity * 2 : 16;
        allocated_nodes = realloc(allocated_nodes, allocated_capacity * sizeof(SafeASTNode*));
    }
    allocated_nodes[allocated_count++] = node;
}

// Remove node from global tracking
static void untrack_node(SafeASTNode* node) {
    for (size_t i = 0; i < allocated_count; i++) {
        if (allocated_nodes[i] == node) {
            memmove(&allocated_nodes[i], &allocated_nodes[i + 1], 
                   (allocated_count - i - 1) * sizeof(SafeASTNode*));
            allocated_count--;
            return;
        }
    }
}

SafeASTNode* safe_ast_node_new(ASTNodeType type, Position pos, const char* file, int line) {
    SafeASTNode* node = malloc(sizeof(SafeASTNode));
    if (!node) {
        fprintf(stderr, "MEMORY ERROR: Failed to allocate SafeASTNode at %s:%d\n", file, line);
        return NULL;
    }
    
    // Initialize magic numbers and safety data
    node->magic_start = AST_MAGIC_START;
    node->magic_end = AST_MAGIC_END;
    node->ref_count = 1;
    node->alloc_size = sizeof(SafeASTNode);
    node->created_file = file;
    node->created_line = line;
    
    // Initialize base AST node
    node->base.type = type;
    node->base.pos = pos;
    node->base.node_type = NULL;
    node->base.next = NULL;
    
    // Track this allocation
    track_node(node);
    
    printf("SAFE_AST: Created node type %d at %s:%d (ptr=%p)\n", 
           type, file, line, (void*)node);
    
    return node;
}

int safe_ast_node_validate(const SafeASTNode* node) {
    if (!node) {
        fprintf(stderr, "VALIDATION ERROR: NULL node pointer\n");
        return 0;
    }
    
    if (node->magic_start != AST_MAGIC_START) {
        fprintf(stderr, "VALIDATION ERROR: Invalid start magic (0x%x, expected 0x%x) at %p\n", 
                node->magic_start, AST_MAGIC_START, (void*)node);
        return 0;
    }
    
    if (node->magic_end != AST_MAGIC_END) {
        fprintf(stderr, "VALIDATION ERROR: Invalid end magic (0x%x, expected 0x%x) at %p\n", 
                node->magic_end, AST_MAGIC_END, (void*)node);
        return 0;
    }
    
    if (node->ref_count == 0) {
        fprintf(stderr, "VALIDATION ERROR: Zero reference count at %p\n", (void*)node);
        return 0;
    }
    
    if (node->base.type >= AST_NODE_COUNT || node->base.type < 0) {
        fprintf(stderr, "VALIDATION ERROR: Invalid AST node type %d at %p\n", 
                node->base.type, (void*)node);
        return 0;
    }
    
    return 1;
}

void safe_ast_node_add_ref(SafeASTNode* node) {
    if (!safe_ast_node_validate(node)) {
        fprintf(stderr, "REFERENCE ERROR: Cannot add reference to invalid node\n");
        return;
    }
    
    node->ref_count++;
    printf("SAFE_AST: Added reference to node at %p (refs=%d)\n", 
           (void*)node, node->ref_count);
}

void safe_ast_node_release(SafeASTNode* node) {
    if (!safe_ast_node_validate(node)) {
        fprintf(stderr, "REFERENCE ERROR: Cannot release invalid node\n");
        return;
    }
    
    node->ref_count--;
    printf("SAFE_AST: Released reference to node at %p (refs=%d)\n", 
           (void*)node, node->ref_count);
    
    if (node->ref_count == 0) {
        safe_ast_node_free(node);
    }
}

void safe_ast_node_free(SafeASTNode* node) {
    if (!node) return;
    
    if (!safe_ast_node_validate(node)) {
        fprintf(stderr, "FREE ERROR: Attempting to free invalid node at %p\n", (void*)node);
        return;
    }
    
    printf("SAFE_AST: Freeing node type %d created at %s:%d (ptr=%p)\n", 
           node->base.type, node->created_file, node->created_line, (void*)node);
    
    // Remove from tracking
    untrack_node(node);
    
    // Clear magic numbers to detect use-after-free
    node->magic_start = 0xDEAD0000;
    node->magic_end = 0x0000BEEF;
    
    free(node);
}

SafeASTNode* safe_ast_literal_new(TokenType type, const char* value, Position pos, 
                                  const char* file, int line) {
    // Allocate space for SafeASTNode + LiteralNode data
    size_t total_size = sizeof(SafeASTNode) + sizeof(LiteralNode) - sizeof(ASTNode);
    SafeASTNode* safe_node = malloc(total_size);
    if (!safe_node) {
        fprintf(stderr, "MEMORY ERROR: Failed to allocate SafeLiteralNode at %s:%d\n", file, line);
        return NULL;
    }
    
    // Initialize safety wrapper
    safe_node->magic_start = AST_MAGIC_START;
    safe_node->magic_end = AST_MAGIC_END;
    safe_node->ref_count = 1;
    safe_node->alloc_size = total_size;
    safe_node->created_file = file;
    safe_node->created_line = line;
    
    // Initialize base as literal
    safe_node->base.type = AST_LITERAL;
    safe_node->base.pos = pos;
    safe_node->base.node_type = NULL;
    safe_node->base.next = NULL;
    
    // Initialize literal-specific data
    LiteralNode* literal = (LiteralNode*)safe_node;
    literal->literal_type = type;
    literal->value = value ? strdup(value) : NULL;
    
    // Track this allocation
    track_node(safe_node);
    
    printf("SAFE_AST: Created literal node with type %d at %s:%d:%d, value='%s' (ptr=%p)\n", 
           AST_LITERAL, pos.filename ? pos.filename : "unknown", pos.line, pos.column, 
           value ? value : "null", (void*)safe_node);
    
    return safe_node;
}

void ast_memory_report(void) {
    printf("=== AST Memory Report ===\n");
    printf("Allocated nodes: %zu\n", allocated_count);
    
    for (size_t i = 0; i < allocated_count; i++) {
        SafeASTNode* node = allocated_nodes[i];
        if (safe_ast_node_validate(node)) {
            printf("  Node %zu: type=%d, refs=%d, created at %s:%d\n", 
                   i, node->base.type, node->ref_count, 
                   node->created_file, node->created_line);
        } else {
            printf("  Node %zu: CORRUPTED!\n", i);
        }
    }
    printf("========================\n");
}

int ast_memory_check_all(void) {
    int errors = 0;
    
    for (size_t i = 0; i < allocated_count; i++) {
        if (!safe_ast_node_validate(allocated_nodes[i])) {
            errors++;
        }
    }
    
    if (errors > 0) {
        printf("MEMORY CHECK: Found %d corrupted nodes out of %zu total\n", 
               errors, allocated_count);
    } else {
        printf("MEMORY CHECK: All %zu nodes are valid\n", allocated_count);
    }
    
    return errors;
}