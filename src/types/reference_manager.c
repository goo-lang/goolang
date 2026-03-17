#include "memory_safety.h"
#include "errors/error.h"
#include "token.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Internal helper functions
static LifetimeScope* lifetime_scope_new(LifetimeScopeKind kind, size_t start_pos, LifetimeScope* parent);
static void lifetime_scope_free(LifetimeScope* scope);
static ReferenceInfo* reference_info_new(const char* name, const char* target, ReferenceKind kind, size_t position);
static void reference_info_free(ReferenceInfo* ref);
static BorrowTracker* borrow_tracker_new(const char* target_name);
static void borrow_tracker_free(BorrowTracker* tracker);
static BorrowTracker* find_borrow_tracker(ReferenceManager* mgr, const char* target_name);
static int check_borrow_conflicts(ReferenceManager* mgr, const char* target_name, ReferenceKind new_borrow_kind);
static void invalidate_references_to(ReferenceManager* mgr, const char* target_name, size_t position);
static int is_reference_valid_at(ReferenceInfo* ref, size_t position);

// Create a new reference manager
ReferenceManager* reference_manager_new(FlowSensitiveAnalyzer* flow_analyzer) {
    ReferenceManager* mgr = calloc(1, sizeof(ReferenceManager));
    if (!mgr) return NULL;
    
    mgr->flow_analyzer = flow_analyzer;
    mgr->next_scope_id = 1;
    
    // Create global scope
    mgr->global_scope = lifetime_scope_new(LIFETIME_SCOPE_GLOBAL, 0, NULL);
    mgr->current_scope = mgr->global_scope;
    
    // Initialize collections
    mgr->reference_capacity = 32;
    mgr->all_references = calloc(mgr->reference_capacity, sizeof(ReferenceInfo*));
    
    mgr->tracker_capacity = 16;
    mgr->borrow_trackers = calloc(mgr->tracker_capacity, sizeof(BorrowTracker*));
    
    // Default configuration
    mgr->enable_weak_references = 1;
    mgr->enable_smart_pointers = 1;
    mgr->strict_lifetime_checking = 1;
    
    return mgr;
}

// Free reference manager
void reference_manager_free(ReferenceManager* mgr) {
    if (!mgr) return;
    
    // Free all references
    for (size_t i = 0; i < mgr->reference_count; i++) {
        reference_info_free(mgr->all_references[i]);
    }
    free(mgr->all_references);
    
    // Free all borrow trackers
    for (size_t i = 0; i < mgr->tracker_count; i++) {
        borrow_tracker_free(mgr->borrow_trackers[i]);
    }
    free(mgr->borrow_trackers);
    
    // Free lifetime scopes (global scope will free all children)
    lifetime_scope_free(mgr->global_scope);
    
    free(mgr);
}

// Enter a new lifetime scope
LifetimeScope* reference_manager_enter_scope(ReferenceManager* mgr, LifetimeScopeKind kind, size_t start_pos) {
    LifetimeScope* new_scope = lifetime_scope_new(kind, start_pos, mgr->current_scope);
    if (!new_scope) return NULL;
    new_scope->scope_id = mgr->next_scope_id++;
    
    // Add to parent's children
    if (mgr->current_scope) {
        if (mgr->current_scope->child_count >= mgr->current_scope->child_capacity) {
            size_t new_cap = mgr->current_scope->child_capacity ?
                mgr->current_scope->child_capacity * 2 : 4;
            LifetimeScope** tmp = realloc(mgr->current_scope->children,
                new_cap * sizeof(LifetimeScope*));
            if (!tmp) {
                lifetime_scope_free(new_scope);
                return NULL;
            }
            mgr->current_scope->children = tmp;
            mgr->current_scope->child_capacity = new_cap;
        }
        mgr->current_scope->children[mgr->current_scope->child_count++] = new_scope;
    }
    
    mgr->current_scope = new_scope;
    return new_scope;
}

// Exit current lifetime scope
void reference_manager_exit_scope(ReferenceManager* mgr, size_t end_pos) {
    if (!mgr->current_scope || mgr->current_scope == mgr->global_scope) {
        return; // Can't exit global scope
    }
    
    mgr->current_scope->end_position = end_pos;
    
    // Invalidate all references that were created in this scope
    for (size_t i = 0; i < mgr->current_scope->reference_count; i++) {
        ReferenceInfo* ref = mgr->current_scope->references[i];
        ref->validity = REFERENCE_INVALIDATED;
        ref->last_use_position = end_pos;
        mgr->references_invalidated++;
    }
    
    mgr->current_scope = mgr->current_scope->parent;
}

// Create a new reference
ReferenceInfo* reference_manager_create_reference(ReferenceManager* mgr, 
                                                 const char* ref_name,
                                                 const char* target_name,
                                                 ReferenceKind kind,
                                                 size_t position) {
    // Check for borrow conflicts
    if (check_borrow_conflicts(mgr, target_name, kind)) {
        mgr->error_count++;
        return NULL;
    }
    
    // Create reference info
    ReferenceInfo* ref = reference_info_new(ref_name, target_name, kind, position);
    if (!ref) return NULL;
    
    ref->scope = mgr->current_scope;
    ref->validity = REFERENCE_VALID;
    
    // Add to global reference list
    if (mgr->reference_count >= mgr->reference_capacity) {
        size_t new_cap = mgr->reference_capacity * 2;
        ReferenceInfo** tmp = realloc(mgr->all_references,
            new_cap * sizeof(ReferenceInfo*));
        if (!tmp) { reference_info_free(ref); return NULL; }
        mgr->all_references = tmp;
        mgr->reference_capacity = new_cap;
    }
    mgr->all_references[mgr->reference_count++] = ref;

    // Add to current scope
    if (mgr->current_scope->reference_count >= mgr->current_scope->reference_capacity) {
        size_t new_cap = mgr->current_scope->reference_capacity ?
            mgr->current_scope->reference_capacity * 2 : 8;
        ReferenceInfo** tmp = realloc(mgr->current_scope->references,
            new_cap * sizeof(ReferenceInfo*));
        if (!tmp) return ref;  // Already in global list, just skip scope tracking
        mgr->current_scope->references = tmp;
        mgr->current_scope->reference_capacity = new_cap;
    }
    mgr->current_scope->references[mgr->current_scope->reference_count++] = ref;
    
    // Update borrow tracker
    BorrowTracker* tracker = find_borrow_tracker(mgr, target_name);
    if (!tracker) {
        tracker = borrow_tracker_new(target_name);
        if (mgr->tracker_count >= mgr->tracker_capacity) {
            size_t new_cap = mgr->tracker_capacity * 2;
            BorrowTracker** tmp = realloc(mgr->borrow_trackers,
                new_cap * sizeof(BorrowTracker*));
            if (!tmp) return ref;
            mgr->borrow_trackers = tmp;
            mgr->tracker_capacity = new_cap;
        }
        mgr->borrow_trackers[mgr->tracker_count++] = tracker;
    }
    
    // Add to borrower list
    if (tracker->borrower_count >= tracker->borrower_capacity) {
        size_t new_cap = tracker->borrower_capacity ?
            tracker->borrower_capacity * 2 : 4;
        ReferenceInfo** tmp = realloc(tracker->borrowers,
            new_cap * sizeof(ReferenceInfo*));
        if (!tmp) return ref;
        tracker->borrowers = tmp;
        tracker->borrower_capacity = new_cap;
    }
    tracker->borrowers[tracker->borrower_count++] = ref;
    
    // Update borrow counts
    if (kind == REFERENCE_KIND_MUTABLE) {
        tracker->has_mutable_borrow = 1;
    } else if (kind == REFERENCE_KIND_SHARED) {
        tracker->immutable_borrow_count++;
    }
    
    mgr->references_created++;
    return ref;
}

// Update reference usage position
void reference_manager_use_reference(ReferenceManager* mgr, const char* ref_name, size_t position) {
    // Find the reference
    for (size_t i = 0; i < mgr->reference_count; i++) {
        ReferenceInfo* ref = mgr->all_references[i];
        if (strcmp(ref->name, ref_name) == 0) {
            // Check if reference is still valid
            if (!is_reference_valid_at(ref, position)) {
                // Reference used after invalidation - error
                mgr->error_count++;
                // TODO: Report error through error system
                return;
            }
            ref->last_use_position = position;
            return;
        }
    }
}

// Invalidate references when target is moved or mutated
void reference_manager_invalidate_references(ReferenceManager* mgr, const char* target_name, size_t position) {
    invalidate_references_to(mgr, target_name, position);
}

// Check if a variable can be safely moved (no outstanding references)
int reference_manager_can_move(ReferenceManager* mgr, const char* var_name, size_t position) {
    BorrowTracker* tracker = find_borrow_tracker(mgr, var_name);
    if (!tracker) return 1; // No references, safe to move
    
    // Check if any references are still active
    for (size_t i = 0; i < tracker->borrower_count; i++) {
        ReferenceInfo* ref = tracker->borrowers[i];
        if (is_reference_valid_at(ref, position)) {
            return 0; // Has active references, cannot move
        }
    }
    
    return 1; // All references invalidated, safe to move
}

// Analyze reference patterns in an expression
int reference_manager_analyze_expression(ReferenceManager* mgr, ASTNode* expr, size_t position) {
    if (!expr) return 1;
    
    switch (expr->type) {
        case AST_IDENTIFIER: {
            // Check if this is a reference usage
            IdentifierNode* ident = (IdentifierNode*)expr;
            reference_manager_use_reference(mgr, ident->name, position);
            break;
        }
        
        case AST_ADDR_OF: {
            // Creating a reference: &variable 
            AddrOfNode* addr_of = (AddrOfNode*)expr;
            if (addr_of->operand) {
                // For now, treat all address-of as immutable references
                // TODO: Extend AST to distinguish &mut references
                ReferenceKind kind = REFERENCE_KIND_SHARED;
                
                // Generate a temporary name for the reference
                char ref_name[64];
                snprintf(ref_name, sizeof(ref_name), "__ref_%zu", position);
                
                const char* target_name = NULL;
                if (addr_of->operand->type == AST_IDENTIFIER) {
                    IdentifierNode* ident = (IdentifierNode*)addr_of->operand;
                    target_name = ident->name;
                }
                
                if (target_name) {
                    reference_manager_create_reference(mgr, ref_name, target_name, kind, position);
                }
            }
            break;
        }
        
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)expr;
            reference_manager_analyze_expression(mgr, binary->left, position);
            reference_manager_analyze_expression(mgr, binary->right, position);
            break;
        }
            
        case AST_UNARY_EXPR: {
            UnaryExprNode* unary = (UnaryExprNode*)expr;
            reference_manager_analyze_expression(mgr, unary->operand, position);
            break;
        }
        
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            // For now, just analyze the function being called
            // TODO: Iterate through argument list properly
            if (call->args) {
                reference_manager_analyze_expression(mgr, call->args, position);
            }
            break;
        }
            
        default:
            // For other expression types, we don't track references for now
            break;
    }
    
    return 1;
}

// Analyze reference patterns in a statement
int reference_manager_analyze_statement(ReferenceManager* mgr, ASTNode* stmt, size_t position) {
    if (!stmt) return 1;
    
    switch (stmt->type) {
        case AST_VAR_DECL: {
            // Check if initializer creates or uses references
            VarDeclNode* var_decl = (VarDeclNode*)stmt;
            if (var_decl->values) {
                reference_manager_analyze_expression(mgr, var_decl->values, position);
            }
            break;
        }
        
        case AST_BLOCK_STMT: {
            // Enter a new scope for the block
            (void)reference_manager_enter_scope(mgr, LIFETIME_SCOPE_BLOCK, position);

            BlockStmtNode* block = (BlockStmtNode*)stmt;
            // Analyze statements in the block (simple traversal for now)
            ASTNode* current = block->statements;
            size_t stmt_pos = position;
            while (current) {
                reference_manager_analyze_statement(mgr, current, stmt_pos++);
                current = current->next;
            }
            
            // Exit the block scope
            reference_manager_exit_scope(mgr, stmt_pos);
            break;
        }
        
        case AST_IF_STMT: {
            IfStmtNode* if_stmt = (IfStmtNode*)stmt;
            // Analyze condition
            reference_manager_analyze_expression(mgr, if_stmt->condition, position);
            
            // Create conditional scopes
            reference_manager_enter_scope(mgr, LIFETIME_SCOPE_CONDITIONAL, position);
            reference_manager_analyze_statement(mgr, if_stmt->then_stmt, position + 1);
            reference_manager_exit_scope(mgr, position + 2);
            
            if (if_stmt->else_stmt) {
                reference_manager_enter_scope(mgr, LIFETIME_SCOPE_CONDITIONAL, position);
                reference_manager_analyze_statement(mgr, if_stmt->else_stmt, position + 3);
                reference_manager_exit_scope(mgr, position + 4);
            }
            break;
        }
        
        case AST_EXPR_STMT: {
            ExprStmtNode* expr_stmt = (ExprStmtNode*)stmt;
            reference_manager_analyze_expression(mgr, expr_stmt->expr, position);
            break;
        }
            
        default:
            // For other statement types, we don't track references for now
            break;
    }
    
    return 1;
}

// Generate reference management code (for codegen integration)
int reference_manager_generate_cleanup_code(ReferenceManager* mgr, ASTNode* function) {
    (void)mgr;
    (void)function;
    // This would generate automatic cleanup code for references
    // For now, just return success
    return 1;
}

// Print reference manager statistics
void reference_manager_print_statistics(ReferenceManager* mgr) {
    printf("Reference Manager Statistics:\n");
    printf("  References created: %zu\n", mgr->references_created);
    printf("  References invalidated: %zu\n", mgr->references_invalidated);
    printf("  Borrow conflicts detected: %zu\n", mgr->borrow_conflicts_detected);
    printf("  Lifetime errors prevented: %zu\n", mgr->lifetime_errors_prevented);
    printf("  Errors: %d\n", mgr->error_count);
    printf("  Warnings: %d\n", mgr->warning_count);
}

// Internal helper implementations

static LifetimeScope* lifetime_scope_new(LifetimeScopeKind kind, size_t start_pos, LifetimeScope* parent) {
    LifetimeScope* scope = calloc(1, sizeof(LifetimeScope));
    if (!scope) return NULL;
    
    scope->kind = kind;
    scope->start_position = start_pos;
    scope->parent = parent;
    
    scope->reference_capacity = 8;
    scope->references = calloc(scope->reference_capacity, sizeof(ReferenceInfo*));
    
    scope->child_capacity = 4;
    scope->children = calloc(scope->child_capacity, sizeof(LifetimeScope*));
    
    return scope;
}

static void lifetime_scope_free(LifetimeScope* scope) {
    if (!scope) return;
    
    // Free children first
    for (size_t i = 0; i < scope->child_count; i++) {
        lifetime_scope_free(scope->children[i]);
    }
    free(scope->children);
    
    // References are owned by the reference manager, just free the array
    free(scope->references);
    
    free(scope);
}

static ReferenceInfo* reference_info_new(const char* name, const char* target, ReferenceKind kind, size_t position) {
    ReferenceInfo* ref = calloc(1, sizeof(ReferenceInfo));
    if (!ref) return NULL;
    
    ref->name = strdup(name);
    ref->target_name = strdup(target);
    ref->kind = kind;
    ref->creation_position = position;
    ref->last_use_position = position;
    ref->validity = REFERENCE_VALID;
    
    return ref;
}

static void reference_info_free(ReferenceInfo* ref) {
    if (!ref) return;
    
    free(ref->name);
    free(ref->target_name);
    free(ref);
}

static BorrowTracker* borrow_tracker_new(const char* target_name) {
    BorrowTracker* tracker = calloc(1, sizeof(BorrowTracker));
    if (!tracker) return NULL;
    
    tracker->target_name = strdup(target_name);
    tracker->borrower_capacity = 4;
    tracker->borrowers = calloc(tracker->borrower_capacity, sizeof(ReferenceInfo*));
    
    return tracker;
}

static void borrow_tracker_free(BorrowTracker* tracker) {
    if (!tracker) return;
    
    free(tracker->target_name);
    free(tracker->borrowers);
    free(tracker);
}

static BorrowTracker* find_borrow_tracker(ReferenceManager* mgr, const char* target_name) {
    for (size_t i = 0; i < mgr->tracker_count; i++) {
        if (strcmp(mgr->borrow_trackers[i]->target_name, target_name) == 0) {
            return mgr->borrow_trackers[i];
        }
    }
    return NULL;
}

static int check_borrow_conflicts(ReferenceManager* mgr, const char* target_name, ReferenceKind new_borrow_kind) {
    BorrowTracker* tracker = find_borrow_tracker(mgr, target_name);
    if (!tracker) return 0; // No existing borrows, no conflict
    
    // Mutable borrow conflicts with any existing borrow
    if (new_borrow_kind == REFERENCE_KIND_MUTABLE) {
        if (tracker->has_mutable_borrow || tracker->immutable_borrow_count > 0) {
            mgr->borrow_conflicts_detected++;
            return 1; // Conflict detected
        }
    }
    
    // Immutable borrow conflicts with existing mutable borrow
    if (new_borrow_kind == REFERENCE_KIND_SHARED) {
        if (tracker->has_mutable_borrow) {
            mgr->borrow_conflicts_detected++;
            return 1; // Conflict detected
        }
    }
    
    return 0; // No conflict
}

static void invalidate_references_to(ReferenceManager* mgr, const char* target_name, size_t position) {
    BorrowTracker* tracker = find_borrow_tracker(mgr, target_name);
    if (!tracker) return;
    
    // Invalidate all references to this target
    for (size_t i = 0; i < tracker->borrower_count; i++) {
        ReferenceInfo* ref = tracker->borrowers[i];
        if (ref->validity == REFERENCE_VALID) {
            ref->validity = REFERENCE_INVALIDATED;
            ref->last_use_position = position;
            mgr->references_invalidated++;
        }
    }
    
    // Reset borrow counts
    tracker->has_mutable_borrow = 0;
    tracker->immutable_borrow_count = 0;
    tracker->last_mutation_position = position;
}

static int is_reference_valid_at(ReferenceInfo* ref, size_t position) {
    if (ref->validity == REFERENCE_INVALIDATED) {
        return 0;
    }
    
    // Check if position is within the reference's lifetime scope
    if (ref->scope) {
        if (position < ref->scope->start_position || 
            (ref->scope->end_position > 0 && position > ref->scope->end_position)) {
            return 0;
        }
    }
    
    return 1;
}
