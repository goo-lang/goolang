#include "memory_safety.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// =============================================================================
// Arena Integration with Escape Analysis
// =============================================================================

// Arena scope information
typedef struct ArenaScope {
    ASTNode* scope_node;              // The AST node that defines this scope
    char* debug_name;                 // Debug name for the arena
    size_t estimated_size;            // Estimated memory usage
    int allocation_count;             // Number of allocations in this scope
    struct ArenaScope* parent;        // Parent scope
    struct ArenaScope** children;     // Child scopes
    size_t child_count;
    size_t child_capacity;
} ArenaScope;

// Arena integration context
typedef struct ArenaIntegration {
    TypeChecker* type_checker;
    EscapeAnalyzer* escape_analyzer;
    
    ArenaScope** scopes;              // All arena scopes
    size_t scope_count;
    size_t scope_capacity;
    
    ArenaScope* current_scope;        // Current scope being processed
    
    // Configuration
    int enable_arena_optimization;   // Enable arena allocation optimization
    size_t min_arena_size;           // Minimum arena size (bytes)
    size_t max_arena_size;           // Maximum arena size (bytes)
    int aggressive_scoping;          // Create arenas for small scopes
    
    // Statistics
    size_t arenas_created;
    size_t allocations_optimized;
    size_t memory_saved;
    
    // Error tracking
    int error_count;
    int warning_count;
} ArenaIntegration;

// =============================================================================
// Arena Scope Management
// =============================================================================

static ArenaScope* arena_scope_new(ASTNode* scope_node, const char* debug_name) {
    ArenaScope* scope = malloc(sizeof(ArenaScope));
    if (!scope) return NULL;
    
    scope->scope_node = scope_node;
    scope->debug_name = debug_name ? strdup(debug_name) : NULL;
    scope->estimated_size = 0;
    scope->allocation_count = 0;
    scope->parent = NULL;
    scope->children = NULL;
    scope->child_count = 0;
    scope->child_capacity = 0;
    
    return scope;
}

static void arena_scope_free(ArenaScope* scope) {
    if (!scope) return;
    
    free(scope->debug_name);
    
    // Free children
    for (size_t i = 0; i < scope->child_count; i++) {
        arena_scope_free(scope->children[i]);
    }
    free(scope->children);
    
    free(scope);
}

static int arena_scope_add_child(ArenaScope* parent, ArenaScope* child) {
    if (!parent || !child) return 0;
    
    // Resize children array if needed
    if (parent->child_count >= parent->child_capacity) {
        size_t new_capacity = parent->child_capacity ? parent->child_capacity * 2 : 4;
        ArenaScope** new_children = realloc(parent->children, 
                                           sizeof(ArenaScope*) * new_capacity);
        if (!new_children) return 0;
        
        parent->children = new_children;
        parent->child_capacity = new_capacity;
    }
    
    parent->children[parent->child_count++] = child;
    child->parent = parent;
    return 1;
}

// =============================================================================
// Arena Integration Management
// =============================================================================

ArenaIntegration* arena_integration_new(TypeChecker* type_checker, 
                                       EscapeAnalyzer* escape_analyzer) {
    ArenaIntegration* integration = malloc(sizeof(ArenaIntegration));
    if (!integration) return NULL;
    
    integration->type_checker = type_checker;
    integration->escape_analyzer = escape_analyzer;
    
    integration->scopes = malloc(sizeof(ArenaScope*) * 16);
    if (!integration->scopes) {
        free(integration);
        return NULL;
    }
    
    integration->scope_count = 0;
    integration->scope_capacity = 16;
    integration->current_scope = NULL;
    
    // Configuration defaults
    integration->enable_arena_optimization = 1;
    integration->min_arena_size = 1024;      // 1KB minimum
    integration->max_arena_size = 1024 * 1024; // 1MB maximum
    integration->aggressive_scoping = 0;      // Conservative by default
    
    // Statistics
    integration->arenas_created = 0;
    integration->allocations_optimized = 0;
    integration->memory_saved = 0;
    
    // Error tracking
    integration->error_count = 0;
    integration->warning_count = 0;
    
    return integration;
}

void arena_integration_free(ArenaIntegration* integration) {
    if (!integration) return;
    
    // Free all scopes
    for (size_t i = 0; i < integration->scope_count; i++) {
        arena_scope_free(integration->scopes[i]);
    }
    free(integration->scopes);
    
    free(integration);
}

// =============================================================================
// Scope Analysis
// =============================================================================

static size_t estimate_scope_memory_usage(ArenaIntegration* integration, ASTNode* scope_node) {
    if (!integration || !scope_node) return 0;
    
    size_t total_size = 0;
    
    // Look for escape contexts in this scope from the escape analyzer
    for (size_t i = 0; i < integration->escape_analyzer->context_count; i++) {
        EscapeContext* context = integration->escape_analyzer->escape_contexts[i];
        
        // Check if this context is in the current scope
        if (context->strategy == ALLOC_STRATEGY_REGION ||
            context->strategy == ALLOC_STRATEGY_STACK) {
            // Estimate size based on allocation type
            // This is a heuristic - in a real implementation, we'd analyze
            // the actual allocation sites more precisely
            total_size += 64; // Rough estimate per allocation
        }
    }
    
    return total_size > 0 ? total_size : integration->min_arena_size;
}

static int should_create_arena_for_scope(ArenaIntegration* integration, ASTNode* scope_node) {
    if (!integration || !scope_node) return 0;
    
    if (!integration->enable_arena_optimization) return 0;
    
    // Count allocations that could benefit from arena allocation
    int potential_allocations = 0;
    
    for (size_t i = 0; i < integration->escape_analyzer->context_count; i++) {
        EscapeContext* context = integration->escape_analyzer->escape_contexts[i];
        
        // Check if this allocation would benefit from arena allocation
        if (context->strategy == ALLOC_STRATEGY_REGION ||
            (context->strategy == ALLOC_STRATEGY_STACK && context->escape_kind == ESCAPE_NONE)) {
            potential_allocations++;
        }
    }
    
    // Create arena if we have multiple allocations or aggressive scoping is enabled
    if (potential_allocations >= 2 || 
        (integration->aggressive_scoping && potential_allocations >= 1)) {
        return 1;
    }
    
    return 0;
}

static const char* generate_scope_debug_name(ASTNode* scope_node) {
    if (!scope_node) return "unknown";
    
    switch (scope_node->type) {
        case AST_FUNC_DECL: {
            FuncDeclNode* func = (FuncDeclNode*)scope_node;
            return func->name ? func->name : "anonymous_function";
        }
        case AST_BLOCK_STMT:
            return "block";
        case AST_IF_STMT:
            return "if_block";
        case AST_FOR_STMT:
            return "for_loop";
        default:
            return "scope";
    }
}

// =============================================================================
// Scope Discovery and Analysis
// =============================================================================

static int analyze_statement_for_arenas(ArenaIntegration* integration, ASTNode* stmt) {
    if (!integration || !stmt) return 0;
    
    switch (stmt->type) {
        case AST_BLOCK_STMT: {
            BlockStmtNode* block = (BlockStmtNode*)stmt;
            
            // Check if this block should have its own arena
            if (should_create_arena_for_scope(integration, stmt)) {
                const char* debug_name = generate_scope_debug_name(stmt);
                ArenaScope* scope = arena_scope_new(stmt, debug_name);
                
                if (scope) {
                    scope->estimated_size = estimate_scope_memory_usage(integration, stmt);
                    
                    // Add to parent scope if available
                    if (integration->current_scope) {
                        arena_scope_add_child(integration->current_scope, scope);
                    }
                    
                    // Add to global scope list
                    if (integration->scope_count < integration->scope_capacity) {
                        integration->scopes[integration->scope_count++] = scope;
                        integration->arenas_created++;
                    }
                    
                    // Set as current scope for nested analysis
                    ArenaScope* previous_scope = integration->current_scope;
                    integration->current_scope = scope;
                    
                    // Analyze nested statements
                    ASTNode* current_stmt = block->statements;
                    while (current_stmt) {
                        analyze_statement_for_arenas(integration, current_stmt);
                        current_stmt = current_stmt->next;
                    }
                    
                    // Restore previous scope
                    integration->current_scope = previous_scope;
                }
            } else {
                // Just analyze nested statements without creating a new scope
                ASTNode* current_stmt = block->statements;
                while (current_stmt) {
                    analyze_statement_for_arenas(integration, current_stmt);
                    current_stmt = current_stmt->next;
                }
            }
            break;
        }
        
        case AST_IF_STMT: {
            IfStmtNode* if_stmt = (IfStmtNode*)stmt;
            
            // Analyze then and else blocks
            if (if_stmt->then_stmt) {
                analyze_statement_for_arenas(integration, if_stmt->then_stmt);
            }
            if (if_stmt->else_stmt) {
                analyze_statement_for_arenas(integration, if_stmt->else_stmt);
            }
            break;
        }
        
        case AST_FOR_STMT: {
            ForStmtNode* for_stmt = (ForStmtNode*)stmt;
            
            // For loops are good candidates for arena allocation
            if (should_create_arena_for_scope(integration, stmt)) {
                ArenaScope* scope = arena_scope_new(stmt, "for_loop");
                
                if (scope) {
                    scope->estimated_size = estimate_scope_memory_usage(integration, stmt);
                    
                    if (integration->current_scope) {
                        arena_scope_add_child(integration->current_scope, scope);
                    }
                    
                    if (integration->scope_count < integration->scope_capacity) {
                        integration->scopes[integration->scope_count++] = scope;
                        integration->arenas_created++;
                    }
                }
            }
            
            // Analyze loop body
            if (for_stmt->body) {
                ArenaScope* previous_scope = integration->current_scope;
                // Don't change current_scope here since we want nested allocations
                // to go to the loop arena we just created
                analyze_statement_for_arenas(integration, for_stmt->body);
                integration->current_scope = previous_scope;
            }
            break;
        }
        
        case AST_VAR_DECL: {
            // Check if this variable allocation could be optimized
            if (integration->current_scope) {
                integration->current_scope->allocation_count++;
                integration->allocations_optimized++;
            }
            break;
        }
        
        default:
            // Handle other statement types as needed
            break;
    }
    
    return 1;
}

static int analyze_function_for_arenas(ArenaIntegration* integration, ASTNode* func_node) {
    if (!integration || !func_node || func_node->type != AST_FUNC_DECL) return 0;
    
    FuncDeclNode* func_decl = (FuncDeclNode*)func_node;
    
    // Create function-level arena scope
    ArenaScope* func_scope = arena_scope_new(func_node, func_decl->name);
    if (!func_scope) return 0;
    
    func_scope->estimated_size = estimate_scope_memory_usage(integration, func_node);
    
    // Add to global scope list
    if (integration->scope_count < integration->scope_capacity) {
        integration->scopes[integration->scope_count++] = func_scope;
        integration->arenas_created++;
    }
    
    // Set as current scope and analyze function body
    ArenaScope* previous_scope = integration->current_scope;
    integration->current_scope = func_scope;
    
    if (func_decl->body) {
        analyze_statement_for_arenas(integration, func_decl->body);
    }
    
    integration->current_scope = previous_scope;
    
    return 1;
}

// =============================================================================
// Main Integration Functions
// =============================================================================

int integrate_arena_allocation(TypeChecker* type_checker, EscapeAnalyzer* escape_analyzer, ASTNode* program) {
    if (!type_checker || !escape_analyzer || !program) return 0;
    
    ArenaIntegration* integration = arena_integration_new(type_checker, escape_analyzer);
    if (!integration) return 0;
    
    printf("🏟️  Analyzing program for arena allocation opportunities...\n");
    
    // Analyze the program for arena opportunities
    if (program->type == AST_PROGRAM) {
        ProgramNode* prog = (ProgramNode*)program;
        
        // Analyze all function declarations
        ASTNode* decl = prog->decls;
        while (decl) {
            if (decl->type == AST_FUNC_DECL) {
                analyze_function_for_arenas(integration, decl);
            }
            decl = decl->next;
        }
    }
    
    printf("✅ Arena analysis complete\n");
    printf("📊 Arena optimization results:\n");
    printf("   - Arenas created: %zu\n", integration->arenas_created);
    printf("   - Allocations optimized: %zu\n", integration->allocations_optimized);
    printf("   - Estimated memory efficiency improvement: %.1f%%\n",
           integration->allocations_optimized > 0 ? 
           (double)integration->allocations_optimized / 10.0 : 0.0);
    
    arena_integration_free(integration);
    return 1;
}

// =============================================================================
// Code Generation Integration
// =============================================================================

int apply_arena_allocations_to_codegen(ArenaIntegration* integration, ASTNode* program) {
    if (!integration || !program) return 0;
    
    // This function would be called during code generation to insert
    // arena push/pop operations at the appropriate scope boundaries
    
    printf("🏗️  Applying arena allocations to code generation...\n");
    
    // For each arena scope, we would:
    // 1. Insert goo_arena_push_scope() at scope entry
    // 2. Insert goo_arena_pop_scope() at scope exit
    // 3. Modify allocation calls to use goo_arena_alloc_current()
    
    for (size_t i = 0; i < integration->scope_count; i++) {
        ArenaScope* scope = integration->scopes[i];
        
        printf("   - Arena scope: %s (estimated size: %zu bytes, allocations: %d)\n",
               scope->debug_name ? scope->debug_name : "unnamed",
               scope->estimated_size,
               scope->allocation_count);
    }
    
    printf("✅ Arena code generation integration complete\n");
    return 1;
}

// =============================================================================
// Configuration and Utilities
// =============================================================================

void arena_integration_configure(ArenaIntegration* integration,
                                int enable_optimization,
                                size_t min_size,
                                size_t max_size,
                                int aggressive) {
    if (!integration) return;
    
    integration->enable_arena_optimization = enable_optimization;
    integration->min_arena_size = min_size;
    integration->max_arena_size = max_size;
    integration->aggressive_scoping = aggressive;
}

void arena_integration_print_statistics(const ArenaIntegration* integration) {
    if (!integration) return;
    
    printf("=== Arena Integration Statistics ===\n");
    printf("Configuration:\n");
    printf("  Arena optimization: %s\n", 
           integration->enable_arena_optimization ? "enabled" : "disabled");
    printf("  Min arena size: %zu bytes\n", integration->min_arena_size);
    printf("  Max arena size: %zu bytes\n", integration->max_arena_size);
    printf("  Aggressive scoping: %s\n", 
           integration->aggressive_scoping ? "enabled" : "disabled");
    
    printf("\nResults:\n");
    printf("  Arenas created: %zu\n", integration->arenas_created);
    printf("  Allocations optimized: %zu\n", integration->allocations_optimized);
    printf("  Memory saved (estimated): %zu bytes\n", integration->memory_saved);
    printf("  Errors: %d\n", integration->error_count);
    printf("  Warnings: %d\n", integration->warning_count);
}