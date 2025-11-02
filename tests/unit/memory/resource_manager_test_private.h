#ifndef RESOURCE_MANAGER_TEST_PRIVATE_H
#define RESOURCE_MANAGER_TEST_PRIVATE_H

#include "memory_safety.h"
#include "types.h"
#include "ast.h"

// Resource information
typedef struct ResourceInfo {
    char* name;                      // Variable name holding the resource
    ResourceType type;               // Type of resource
    ResourceContext context;         // How it was acquired
    CleanupMethod cleanup_method;    // How it should be cleaned up
    char* cleanup_function;          // Function to call for cleanup
    ASTNode* acquisition_site;       // Where it was acquired
    ASTNode* cleanup_site;           // Where cleanup should happen
    Position acquisition_pos;        // Source position of acquisition
    
    // Scope information
    size_t scope_depth;              // Nesting depth where acquired
    size_t scope_id;                 // Unique scope identifier
    struct LifetimeScope* scope;     // Associated lifetime scope
    
    // State tracking
    int is_acquired;                 // Currently holds the resource
    int is_moved;                    // Resource has been moved
    int is_borrowed;                 // Resource is currently borrowed
    int needs_cleanup;               // Requires cleanup when scope ends
    int cleanup_generated;           // Cleanup code has been generated
    
    // Error handling
    int is_error_resource;           // Resource acquired in error path
    int cleanup_can_fail;            // Cleanup operation can fail
    
    struct ResourceInfo* next;       // For linked lists
} ResourceInfo;

// Defer statement information
typedef struct DeferInfo {
    ASTNode* defer_stmt;             // The defer statement
    ASTNode* deferred_call;          // Function call to defer
    size_t scope_depth;              // Scope where defer was declared
    Position defer_pos;              // Source position
    int is_processed;                // Has been processed for cleanup
    
    struct DeferInfo* next;          // For linked lists
} DeferInfo;

// Scope cleanup tracker
typedef struct ScopeCleanup {
    size_t scope_id;                 // Unique scope identifier
    size_t scope_depth;              // Nesting depth
    ASTNode* scope_node;             // AST node for the scope
    
    ResourceInfo** resources;        // Resources in this scope
    size_t resource_count;
    size_t resource_capacity;
    
    DeferInfo** defers;              // Defer statements in this scope
    size_t defer_count;
    size_t defer_capacity;
    
    // Scope metadata
    int has_early_returns;           // Scope has return statements
    int has_error_handling;          // Scope has error handling
    int has_loops;                   // Scope contains loops
    int is_function_scope;           // This is a function scope
    
    struct ScopeCleanup* parent;     // Parent scope
    struct ScopeCleanup* next;       // For linked lists
} ScopeCleanup;

// Main resource manager
typedef struct ResourceManager {
    // Current analysis state
    TypeChecker* type_checker;       // Associated type checker
    FlowSensitiveAnalyzer* flow_analyzer; // Flow analysis for integration
    
    // Scope management
    ScopeCleanup* current_scope;     // Currently active scope
    ScopeCleanup* global_scope;      // Global scope for the program
    size_t next_scope_id;            // Next scope ID to assign
    size_t current_depth;            // Current nesting depth
    
    // Resource tracking
    ResourceInfo** all_resources;    // All tracked resources
    size_t resource_count;
    size_t resource_capacity;
    
    // Configuration
    int enable_raii;                 // Enable RAII-style management
    int enable_defer;                // Enable defer statement processing
    int strict_cleanup_order;        // Enforce strict LIFO cleanup order
    int generate_cleanup_comments;   // Add comments to generated cleanup
    int enable_error_cleanup;        // Cleanup in error paths
    
    // Statistics
    struct {
        size_t resources_tracked;
        size_t cleanups_generated;
        size_t scopes_processed;
        size_t defers_processed;
        size_t errors_detected;
    } statistics;
    
    // Error tracking
    int error_count;
    int warning_count;
} ResourceManager;

ResourceInfo* resource_info_new(const char* name, ResourceType type, Position pos);

#endif // RESOURCE_MANAGER_TEST_PRIVATE_H
