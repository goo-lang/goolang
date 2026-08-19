#include "memory_safety.h"
#include "types.h"
#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// =============================================================================
// 19.4: Automatic Resource Management Implementation
// =============================================================================

// Helper function for string duplication
static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

// =============================================================================
// Resource Type Definitions (enums defined in memory_safety.h)
// =============================================================================

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

// Cleanup action that needs to be performed
typedef struct CleanupAction {
    ResourceInfo* resource;          // Resource to clean up
    CleanupMethod method;            // How to clean up
    char* cleanup_code;              // Generated cleanup code
    ASTNode* cleanup_location;       // Where to insert cleanup
    int priority;                    // Cleanup order priority
    int is_conditional;              // Cleanup only in certain conditions
    
    struct CleanupAction* next;      // For linked lists
} CleanupAction;

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
    
    CleanupAction** cleanup_actions; // Actions to perform at scope exit
    size_t cleanup_count;
    size_t cleanup_capacity;
    
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
    
    // Cleanup management
    CleanupAction** pending_cleanups; // Cleanups to be generated
    size_t pending_count;
    size_t pending_capacity;
    
    // Configuration
    int enable_raii;                 // Enable RAII-style management
    int enable_defer;                // Enable defer statement processing
    int strict_cleanup_order;        // Enforce strict LIFO cleanup order
    int generate_cleanup_comments;   // Add comments to generated cleanup
    int enable_error_cleanup;        // Cleanup in error paths
    
    // Statistics
    size_t resources_tracked;
    size_t cleanups_generated;
    size_t scopes_processed;
    size_t defers_processed;
    size_t errors_detected;
    
    // Error tracking
    int error_count;
    int warning_count;
} ResourceManager;

// =============================================================================
// Resource Manager Core Functions
// =============================================================================

ResourceManager* resource_manager_new(TypeChecker* type_checker) {
    ResourceManager* rm = xmalloc(sizeof(ResourceManager));
    if (!rm) return NULL;
    
    rm->type_checker = type_checker;
    rm->flow_analyzer = NULL; // Will be set during integration
    
    // Initialize scope management
    rm->current_scope = NULL;
    rm->global_scope = NULL;
    rm->next_scope_id = 1;
    rm->current_depth = 0;
    
    // Initialize resource tracking
    rm->all_resources = malloc(sizeof(ResourceInfo*) * 16);
    if (!rm->all_resources) {
        free(rm);
        return NULL;
    }
    rm->resource_count = 0;
    rm->resource_capacity = 16;
    
    // Initialize cleanup management
    rm->pending_cleanups = malloc(sizeof(CleanupAction*) * 16);
    if (!rm->pending_cleanups) {
        free(rm->all_resources);
        free(rm);
        return NULL;
    }
    rm->pending_count = 0;
    rm->pending_capacity = 16;
    
    // Default configuration
    rm->enable_raii = 1;
    rm->enable_defer = 1;
    rm->strict_cleanup_order = 1;
    rm->generate_cleanup_comments = 1;
    rm->enable_error_cleanup = 1;
    
    // Initialize statistics
    rm->resources_tracked = 0;
    rm->cleanups_generated = 0;
    rm->scopes_processed = 0;
    rm->defers_processed = 0;
    rm->errors_detected = 0;
    rm->error_count = 0;
    rm->warning_count = 0;
    
    return rm;
}

void resource_manager_free(ResourceManager* rm) {
    if (!rm) return;
    
    // Free all resources
    for (size_t i = 0; i < rm->resource_count; i++) {
        ResourceInfo* res = rm->all_resources[i];
        if (res) {
            free(res->name);
            free(res->cleanup_function);
            free(res);
        }
    }
    free(rm->all_resources);
    
    // Free all pending cleanups
    for (size_t i = 0; i < rm->pending_count; i++) {
        CleanupAction* action = rm->pending_cleanups[i];
        if (action) {
            free(action->cleanup_code);
            free(action);
        }
    }
    free(rm->pending_cleanups);
    
    // Free scopes (simplified - would need recursive cleanup)
    // TODO: Implement proper scope cleanup
    
    free(rm);
}

// =============================================================================
// Scope Management Functions
// =============================================================================

ScopeCleanup* scope_cleanup_new(size_t scope_id, size_t depth, ASTNode* scope_node) {
    ScopeCleanup* scope = xmalloc(sizeof(ScopeCleanup));
    if (!scope) return NULL;
    
    scope->scope_id = scope_id;
    scope->scope_depth = depth;
    scope->scope_node = scope_node;
    
    // Initialize resource arrays
    scope->resources = malloc(sizeof(ResourceInfo*) * 8);
    if (!scope->resources) {
        free(scope);
        return NULL;
    }
    scope->resource_count = 0;
    scope->resource_capacity = 8;
    
    // Initialize defer arrays
    scope->defers = malloc(sizeof(DeferInfo*) * 4);
    if (!scope->defers) {
        free(scope->resources);
        free(scope);
        return NULL;
    }
    scope->defer_count = 0;
    scope->defer_capacity = 4;
    
    // Initialize cleanup arrays
    scope->cleanup_actions = malloc(sizeof(CleanupAction*) * 8);
    if (!scope->cleanup_actions) {
        free(scope->resources);
        free(scope->defers);
        free(scope);
        return NULL;
    }
    scope->cleanup_count = 0;
    scope->cleanup_capacity = 8;
    
    // Initialize metadata
    scope->has_early_returns = 0;
    scope->has_error_handling = 0;
    scope->has_loops = 0;
    scope->is_function_scope = 0;
    scope->parent = NULL;
    scope->next = NULL;
    
    return scope;
}

void scope_cleanup_free(ScopeCleanup* scope) {
    if (!scope) return;
    
    // Free defer infos
    for (size_t i = 0; i < scope->defer_count; i++) {
        free(scope->defers[i]);
    }
    free(scope->defers);
    
    // Free cleanup actions
    for (size_t i = 0; i < scope->cleanup_count; i++) {
        CleanupAction* action = scope->cleanup_actions[i];
        if (action) {
            free(action->cleanup_code);
            free(action);
        }
    }
    free(scope->cleanup_actions);
    
    free(scope->resources);
    free(scope);
}

ScopeCleanup* resource_manager_enter_scope(ResourceManager* rm, ASTNode* scope_node) {
    if (!rm) return NULL;
    
    size_t scope_id = rm->next_scope_id++;
    size_t depth = rm->current_depth++;
    
    ScopeCleanup* new_scope = scope_cleanup_new(scope_id, depth, scope_node);
    if (!new_scope) return NULL;
    
    // Set parent relationship
    new_scope->parent = rm->current_scope;
    rm->current_scope = new_scope;
    
    // Determine scope type
    if (scope_node && scope_node->type == AST_FUNC_DECL) {
        new_scope->is_function_scope = 1;
    }
    
    rm->scopes_processed++;
    return new_scope;
}

void resource_manager_exit_scope(ResourceManager* rm) {
    if (!rm || !rm->current_scope) return;
    
    ScopeCleanup* current = rm->current_scope;
    
    // Generate cleanup actions for this scope
    resource_manager_generate_scope_cleanup(rm, current);
    
    // Move to parent scope
    rm->current_scope = current->parent;
    rm->current_depth--;
    
    // Note: We don't free the scope here as it may be referenced later
    // for code generation. It should be freed when the entire analysis is done.
}

// =============================================================================
// Resource Tracking Functions
// =============================================================================

ResourceInfo* resource_info_new(const char* name, ResourceType type, Position pos) {
    ResourceInfo* res = xmalloc(sizeof(ResourceInfo));
    if (!res) return NULL;
    
    res->name = str_dup(name);
    res->type = type;
    res->context = RESOURCE_CONTEXT_DIRECT;
    res->cleanup_method = CLEANUP_METHOD_FUNCTION_CALL;
    res->cleanup_function = NULL;
    res->acquisition_site = NULL;
    res->cleanup_site = NULL;
    res->acquisition_pos = pos;
    
    res->scope_depth = 0;
    res->scope_id = 0;
    res->scope = NULL;
    
    res->is_acquired = 1;
    res->is_moved = 0;
    res->is_borrowed = 0;
    res->needs_cleanup = 1;
    res->cleanup_generated = 0;
    
    res->is_error_resource = 0;
    res->cleanup_can_fail = 0;
    
    res->next = NULL;
    
    return res;
}

int resource_manager_track_resource(ResourceManager* rm, const char* name, 
                                   ResourceType type, ASTNode* acquisition_site, Position pos) {
    if (!rm || !name) return 0;
    
    // Check if we need to resize
    if (rm->resource_count >= rm->resource_capacity) {
        size_t new_capacity = rm->resource_capacity * 2;
        ResourceInfo** new_resources = realloc(rm->all_resources,
            sizeof(ResourceInfo*) * new_capacity);
        if (!new_resources) return 0;
        
        rm->all_resources = new_resources;
        rm->resource_capacity = new_capacity;
    }
    
    ResourceInfo* resource = resource_info_new(name, type, pos);
    if (!resource) return 0;
    
    resource->acquisition_site = acquisition_site;
    resource->scope_depth = rm->current_depth;
    resource->scope_id = rm->current_scope ? rm->current_scope->scope_id : 0;
    
    // Determine cleanup method based on resource type
    switch (type) {
        case RESOURCE_TYPE_FILE:
            resource->cleanup_function = str_dup("close");
            break;
        case RESOURCE_TYPE_NETWORK:
            resource->cleanup_function = str_dup("close_connection");
            break;
        case RESOURCE_TYPE_MUTEX:
            resource->cleanup_function = str_dup("unlock");
            break;
        case RESOURCE_TYPE_MEMORY:
            resource->cleanup_function = str_dup("free");
            break;
        case RESOURCE_TYPE_THREAD:
            resource->cleanup_function = str_dup("thread_join");
            break;
        case RESOURCE_TYPE_GPU_BUFFER:
            resource->cleanup_function = str_dup("gpu_buffer_free");
            break;
        default:
            resource->cleanup_function = str_dup("cleanup");
            break;
    }
    
    // Add to current scope
    if (rm->current_scope) {
        if (rm->current_scope->resource_count < rm->current_scope->resource_capacity) {
            rm->current_scope->resources[rm->current_scope->resource_count++] = resource;
        }
    }
    
    // Add to global resource list
    rm->all_resources[rm->resource_count++] = resource;
    rm->resources_tracked++;
    
    return 1;
}

ResourceInfo* resource_manager_find_resource(ResourceManager* rm, const char* name) {
    if (!rm || !name) return NULL;
    
    for (size_t i = 0; i < rm->resource_count; i++) {
        ResourceInfo* res = rm->all_resources[i];
        if (res && res->name && strcmp(res->name, name) == 0) {
            return res;
        }
    }
    return NULL;
}

int resource_manager_mark_resource_moved(ResourceManager* rm, const char* name) {
    ResourceInfo* res = resource_manager_find_resource(rm, name);
    if (!res) return 0;
    
    res->is_moved = 1;
    res->is_acquired = 0;
    res->needs_cleanup = 0;
    
    return 1;
}

int resource_manager_mark_resource_borrowed(ResourceManager* rm, const char* name) {
    ResourceInfo* res = resource_manager_find_resource(rm, name);
    if (!res) return 0;
    
    res->is_borrowed = 1;
    // Borrowed resources still need cleanup by original owner
    
    return 1;
}

// =============================================================================
// Defer Statement Processing
// =============================================================================

DeferInfo* defer_info_new(ASTNode* defer_stmt, size_t scope_depth, Position pos) {
    DeferInfo* defer = xmalloc(sizeof(DeferInfo));
    if (!defer) return NULL;
    
    defer->defer_stmt = defer_stmt;
    defer->deferred_call = NULL;
    defer->scope_depth = scope_depth;
    defer->defer_pos = pos;
    defer->is_processed = 0;
    defer->next = NULL;
    
    // Extract the deferred call from the defer statement
    if (defer_stmt && defer_stmt->type == AST_DEFER_STMT) {
        DeferStmtNode* defer_node = (DeferStmtNode*)defer_stmt;
        defer->deferred_call = defer_node->call;
    }
    
    return defer;
}

int resource_manager_process_defer(ResourceManager* rm, ASTNode* defer_stmt, Position pos) {
    if (!rm || !defer_stmt || defer_stmt->type != AST_DEFER_STMT) return 0;
    
    DeferInfo* defer = defer_info_new(defer_stmt, rm->current_depth, pos);
    if (!defer) return 0;
    
    // Add to current scope
    if (rm->current_scope) {
        if (rm->current_scope->defer_count >= rm->current_scope->defer_capacity) {
            size_t new_capacity = rm->current_scope->defer_capacity * 2;
            DeferInfo** new_defers = realloc(rm->current_scope->defers,
                sizeof(DeferInfo*) * new_capacity);
            if (!new_defers) {
                free(defer);
                return 0;
            }
            rm->current_scope->defers = new_defers;
            rm->current_scope->defer_capacity = new_capacity;
        }
        
        rm->current_scope->defers[rm->current_scope->defer_count++] = defer;
    }
    
    rm->defers_processed++;
    return 1;
}

// =============================================================================
// Cleanup Action Generation
// =============================================================================

CleanupAction* cleanup_action_new(ResourceInfo* resource, CleanupMethod method, int priority) {
    CleanupAction* action = xmalloc(sizeof(CleanupAction));
    if (!action) return NULL;
    
    action->resource = resource;
    action->method = method;
    action->cleanup_code = NULL;
    action->cleanup_location = NULL;
    action->priority = priority;
    action->is_conditional = 0;
    action->next = NULL;
    
    return action;
}

int resource_manager_add_cleanup_action(ResourceManager* rm, CleanupAction* action) {
    if (!rm || !action) return 0;
    
    // Check if we need to resize
    if (rm->pending_count >= rm->pending_capacity) {
        size_t new_capacity = rm->pending_capacity * 2;
        CleanupAction** new_actions = realloc(rm->pending_cleanups,
            sizeof(CleanupAction*) * new_capacity);
        if (!new_actions) return 0;
        
        rm->pending_cleanups = new_actions;
        rm->pending_capacity = new_capacity;
    }
    
    rm->pending_cleanups[rm->pending_count++] = action;
    return 1;
}

char* generate_cleanup_code(ResourceInfo* resource, CleanupMethod method) {
    if (!resource) return NULL;
    
    char* code = malloc(256); // Allocate enough space for cleanup code
    if (!code) return NULL;
    
    switch (method) {
        case CLEANUP_METHOD_FUNCTION_CALL:
            if (resource->cleanup_function) {
                snprintf(code, 256, "%s(%s);", resource->cleanup_function, resource->name);
            } else {
                snprintf(code, 256, "cleanup(%s);", resource->name);
            }
            break;
            
        case CLEANUP_METHOD_DESTRUCTOR:
            snprintf(code, 256, "%s.~%s();", resource->name, resource->name);
            break;
            
        case CLEANUP_METHOD_DEFER:
            snprintf(code, 256, "// Defer cleanup for %s", resource->name);
            break;
            
        case CLEANUP_METHOD_RAII:
            snprintf(code, 256, "// RAII cleanup for %s", resource->name);
            break;
            
        case CLEANUP_METHOD_CUSTOM:
            snprintf(code, 256, "custom_cleanup(%s);", resource->name);
            break;
            
        default:
            snprintf(code, 256, "// Unknown cleanup method for %s", resource->name);
            break;
    }
    
    return code;
}

int resource_manager_generate_scope_cleanup(ResourceManager* rm, ScopeCleanup* scope) {
    if (!rm || !scope) return 0;
    
    // Generate cleanup for resources in reverse order (LIFO)
    for (int i = (int)scope->resource_count - 1; i >= 0; i--) {
        ResourceInfo* resource = scope->resources[i];
        if (!resource || !resource->needs_cleanup || resource->cleanup_generated) {
            continue;
        }
        
        CleanupAction* action = cleanup_action_new(resource, resource->cleanup_method, i);
        if (!action) continue;
        
        action->cleanup_code = generate_cleanup_code(resource, resource->cleanup_method);
        resource->cleanup_generated = 1;
        
        // Add to scope's cleanup actions
        if (scope->cleanup_count < scope->cleanup_capacity) {
            scope->cleanup_actions[scope->cleanup_count++] = action;
        }
        
        rm->cleanups_generated++;
    }
    
    // Generate cleanup for defer statements in reverse order
    for (int i = (int)scope->defer_count - 1; i >= 0; i--) {
        DeferInfo* defer = scope->defers[i];
        if (!defer || defer->is_processed) continue;
        
        // Create cleanup action for defer
        CleanupAction* action = cleanup_action_new(NULL, CLEANUP_METHOD_DEFER, 
                                                  (int)scope->defer_count - i);
        if (!action) continue;
        
        // Generate code for deferred call
        if (defer->deferred_call) {
            action->cleanup_code = malloc(128);
            if (action->cleanup_code) {
                snprintf(action->cleanup_code, 128, "// Execute deferred call");
                // TODO: Generate actual call code based on AST
            }
        }
        
        defer->is_processed = 1;
        
        // Add to scope's cleanup actions
        if (scope->cleanup_count < scope->cleanup_capacity) {
            scope->cleanup_actions[scope->cleanup_count++] = action;
        }
    }
    
    return 1;
}

// =============================================================================
// AST Analysis Functions
// =============================================================================

int resource_manager_analyze_statement(ResourceManager* rm, ASTNode* stmt) {
    if (!rm || !stmt) return 0;
    
    switch (stmt->type) {
        case AST_VAR_DECL: {
            VarDeclNode* var_decl = (VarDeclNode*)stmt;
            
            // Check if this is a resource allocation
            if (var_decl->values) {
                // Analyze the initialization expression to detect resource allocation
                ResourceType type = detect_resource_type(rm, var_decl->values);
                if (type != RESOURCE_TYPE_UNKNOWN) {
                    for (size_t i = 0; i < var_decl->name_count; i++) {
                        resource_manager_track_resource(rm, var_decl->names[i], type,
                                                      var_decl->values, stmt->pos);
                    }
                }
            }
            break;
        }
        
        case AST_DEFER_STMT: {
            resource_manager_process_defer(rm, stmt, stmt->pos);
            break;
        }
        
        case AST_BLOCK_STMT: {
            BlockStmtNode* block = (BlockStmtNode*)stmt;
            
            // Enter new scope for block
            resource_manager_enter_scope(rm, stmt);
            
            // Analyze all statements in the block
            ASTNode* current_stmt = block->statements;
            while (current_stmt) {
                resource_manager_analyze_statement(rm, current_stmt);
                current_stmt = current_stmt->next;
            }
            
            // Exit scope and generate cleanup
            resource_manager_exit_scope(rm);
            break;
        }
        
        case AST_IF_STMT: {
            IfStmtNode* if_stmt = (IfStmtNode*)stmt;
            
            // Analyze condition
            resource_manager_analyze_expression(rm, if_stmt->condition);
            
            // Analyze branches - each gets its own scope
            if (if_stmt->then_stmt) {
                resource_manager_enter_scope(rm, if_stmt->then_stmt);
                resource_manager_analyze_statement(rm, if_stmt->then_stmt);
                resource_manager_exit_scope(rm);
            }
            
            if (if_stmt->else_stmt) {
                resource_manager_enter_scope(rm, if_stmt->else_stmt);
                resource_manager_analyze_statement(rm, if_stmt->else_stmt);
                resource_manager_exit_scope(rm);
            }
            break;
        }
        
        case AST_FOR_STMT: {
            ForStmtNode* for_stmt = (ForStmtNode*)stmt;
            
            // Enter scope for the entire for loop
            resource_manager_enter_scope(rm, stmt);
            if (rm->current_scope) {
                rm->current_scope->has_loops = 1;
            }
            
            // Analyze loop components
            if (for_stmt->init) {
                resource_manager_analyze_statement(rm, for_stmt->init);
            }
            if (for_stmt->condition) {
                resource_manager_analyze_expression(rm, for_stmt->condition);
            }
            if (for_stmt->post) {
                resource_manager_analyze_statement(rm, for_stmt->post);
            }
            if (for_stmt->body) {
                resource_manager_analyze_statement(rm, for_stmt->body);
            }
            
            resource_manager_exit_scope(rm);
            break;
        }
        
        case AST_RETURN_STMT: {
            ReturnStmtNode* ret = (ReturnStmtNode*)stmt;
            
            if (rm->current_scope) {
                rm->current_scope->has_early_returns = 1;
            }
            
            if (ret->values) {
                resource_manager_analyze_expression(rm, ret->values);
            }
            break;
        }
        
        case AST_EXPR_STMT: {
            ExprStmtNode* expr_stmt = (ExprStmtNode*)stmt;
            resource_manager_analyze_expression(rm, expr_stmt->expr);
            break;
        }
        
        default:
            // Handle other statement types as needed
            break;
    }
    
    return 1;
}

int resource_manager_analyze_expression(ResourceManager* rm, ASTNode* expr) {
    if (!rm || !expr) return 0;
    
    switch (expr->type) {
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            
            // Check if this is a resource allocation function
            if (call->function && call->function->type == AST_IDENTIFIER) {
                IdentifierNode* func_name = (IdentifierNode*)call->function;
                ResourceType type = get_resource_type_for_function(func_name->name);
                
                if (type != RESOURCE_TYPE_UNKNOWN) {
                    // This call allocates a resource
                    // The resource will be tracked when assigned to a variable
                }
            }
            
            // Analyze arguments
            if (call->args) {
                resource_manager_analyze_expression(rm, call->args);
            }
            break;
        }
        
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)expr;
            
            resource_manager_analyze_expression(rm, binary->left);
            resource_manager_analyze_expression(rm, binary->right);
            break;
        }
        
        case AST_UNARY_EXPR: {
            UnaryExprNode* unary = (UnaryExprNode*)expr;
            resource_manager_analyze_expression(rm, unary->operand);
            break;
        }
        
        default:
            // Handle other expression types as needed
            break;
    }
    
    return 1;
}

// =============================================================================
// Resource Type Detection
// =============================================================================

ResourceType detect_resource_type(ResourceManager* rm, ASTNode* expr) {
    if (!rm || !expr) return RESOURCE_TYPE_UNKNOWN;
    
    if (expr->type == AST_CALL_EXPR) {
        CallExprNode* call = (CallExprNode*)expr;
        if (call->function && call->function->type == AST_IDENTIFIER) {
            IdentifierNode* func_name = (IdentifierNode*)call->function;
            return get_resource_type_for_function(func_name->name);
        }
    }
    
    return RESOURCE_TYPE_UNKNOWN;
}

ResourceType get_resource_type_for_function(const char* func_name) {
    if (!func_name) return RESOURCE_TYPE_UNKNOWN;
    
    // File operations
    if (strcmp(func_name, "open") == 0 || strcmp(func_name, "fopen") == 0) {
        return RESOURCE_TYPE_FILE;
    }
    
    // Network operations
    if (strcmp(func_name, "socket") == 0 || strcmp(func_name, "connect") == 0) {
        return RESOURCE_TYPE_NETWORK;
    }
    
    // Memory operations
    if (strcmp(func_name, "malloc") == 0 || strcmp(func_name, "calloc") == 0 ||
        strcmp(func_name, "realloc") == 0) {
        return RESOURCE_TYPE_MEMORY;
    }
    
    // Mutex operations
    if (strcmp(func_name, "mutex_new") == 0 || strcmp(func_name, "lock") == 0) {
        return RESOURCE_TYPE_MUTEX;
    }
    
    // Thread operations
    if (strcmp(func_name, "thread_create") == 0 || strcmp(func_name, "spawn") == 0) {
        return RESOURCE_TYPE_THREAD;
    }
    
    // GPU operations
    if (strcmp(func_name, "gpu_alloc") == 0 || strcmp(func_name, "cuda_malloc") == 0) {
        return RESOURCE_TYPE_GPU_BUFFER;
    }
    
    return RESOURCE_TYPE_UNKNOWN;
}

// =============================================================================
// Function Analysis
// =============================================================================

int resource_manager_analyze_function(ResourceManager* rm, ASTNode* func_node) {
    if (!rm || !func_node || func_node->type != AST_FUNC_DECL) return 0;
    
    FuncDeclNode* func_decl = (FuncDeclNode*)func_node;
    
    // Enter function scope
    ScopeCleanup* func_scope = resource_manager_enter_scope(rm, func_node);
    if (func_scope) {
        func_scope->is_function_scope = 1;
    }
    
    // Analyze function body
    if (func_decl->body) {
        resource_manager_analyze_statement(rm, func_decl->body);
    }
    
    // Exit function scope
    resource_manager_exit_scope(rm);
    
    return 1;
}

// =============================================================================
// Statistics and Reporting
// =============================================================================

void resource_manager_print_statistics(ResourceManager* rm) {
    if (!rm) return;
    
    printf("=== Automatic Resource Management Statistics ===\n");
    printf("Resources tracked: %zu\n", rm->resources_tracked);
    printf("Cleanups generated: %zu\n", rm->cleanups_generated);
    printf("Scopes processed: %zu\n", rm->scopes_processed);
    printf("Defer statements processed: %zu\n", rm->defers_processed);
    printf("Errors detected: %zu\n", rm->errors_detected);
    printf("Warnings: %d\n", rm->warning_count);
    
    printf("\nConfiguration:\n");
    printf("RAII enabled: %s\n", rm->enable_raii ? "yes" : "no");
    printf("Defer enabled: %s\n", rm->enable_defer ? "yes" : "no");
    printf("Strict cleanup order: %s\n", rm->strict_cleanup_order ? "yes" : "no");
    printf("Error cleanup enabled: %s\n", rm->enable_error_cleanup ? "yes" : "no");
}

void resource_manager_print_resource_info(ResourceManager* rm, const char* resource_name) {
    if (!rm || !resource_name) return;
    
    ResourceInfo* res = resource_manager_find_resource(rm, resource_name);
    if (!res) {
        printf("Resource '%s' not found\n", resource_name);
        return;
    }
    
    printf("=== Resource Information: %s ===\n", resource_name);
    printf("Type: %s\n", resource_type_to_string(res->type));
    printf("Context: %s\n", resource_context_to_string(res->context));
    printf("Cleanup method: %s\n", cleanup_method_to_string(res->cleanup_method));
    printf("Cleanup function: %s\n", res->cleanup_function ? res->cleanup_function : "none");
    printf("Scope depth: %zu\n", res->scope_depth);
    printf("Is acquired: %s\n", res->is_acquired ? "yes" : "no");
    printf("Is moved: %s\n", res->is_moved ? "yes" : "no");
    printf("Needs cleanup: %s\n", res->needs_cleanup ? "yes" : "no");
    printf("Cleanup generated: %s\n", res->cleanup_generated ? "yes" : "no");
}

// =============================================================================
// Utility Functions
// =============================================================================

const char* resource_type_to_string(ResourceType type) {
    switch (type) {
        case RESOURCE_TYPE_FILE: return "file";
        case RESOURCE_TYPE_NETWORK: return "network";
        case RESOURCE_TYPE_MUTEX: return "mutex";
        case RESOURCE_TYPE_MEMORY: return "memory";
        case RESOURCE_TYPE_THREAD: return "thread";
        case RESOURCE_TYPE_GPU_BUFFER: return "gpu_buffer";
        case RESOURCE_TYPE_CUSTOM: return "custom";
        case RESOURCE_TYPE_UNKNOWN: return "unknown";
        default: return "invalid";
    }
}

const char* resource_context_to_string(ResourceContext context) {
    switch (context) {
        case RESOURCE_CONTEXT_DIRECT: return "direct";
        case RESOURCE_CONTEXT_FUNCTION_CALL: return "function_call";
        case RESOURCE_CONTEXT_CONSTRUCTOR: return "constructor";
        case RESOURCE_CONTEXT_ASSIGNMENT: return "assignment";
        case RESOURCE_CONTEXT_PARAMETER: return "parameter";
        default: return "invalid";
    }
}

const char* cleanup_method_to_string(CleanupMethod method) {
    switch (method) {
        case CLEANUP_METHOD_FUNCTION_CALL: return "function_call";
        case CLEANUP_METHOD_DESTRUCTOR: return "destructor";
        case CLEANUP_METHOD_DEFER: return "defer";
        case CLEANUP_METHOD_RAII: return "raii";
        case CLEANUP_METHOD_CUSTOM: return "custom";
        default: return "invalid";
    }
}

// Error reporting
void resource_manager_error(ResourceManager* rm, Position pos, const char* format, ...) {
    if (!rm) return;
    
    va_list args;
    va_start(args, format);
    
    fprintf(stderr, "Resource Manager Error at %s:%zu:%zu: ", 
            pos.filename, pos.line, pos.column);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    
    va_end(args);
    
    rm->error_count++;
    rm->errors_detected++;
}

void resource_manager_warning(ResourceManager* rm, Position pos, const char* format, ...) {
    if (!rm) return;
    
    va_list args;
    va_start(args, format);
    
    fprintf(stderr, "Resource Manager Warning at %s:%zu:%zu: ", 
            pos.filename, pos.line, pos.column);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    
    va_end(args);
    
    rm->warning_count++;
}

// =============================================================================
// Integration Functions
// =============================================================================

int integrate_resource_manager_with_type_checker(TypeChecker* type_checker, ResourceManager* rm) {
    if (!type_checker || !rm) return 0;
    
    // Set up bidirectional integration
    rm->type_checker = type_checker;
    
    // TODO: Add resource manager to type checker structure
    // This would require modifying the TypeChecker struct
    
    return 1;
}

int apply_resource_management_to_codegen(ResourceManager* rm, ASTNode* program) {
    if (!rm || !program) return 0;
    
    // This function would be called during code generation to insert
    // the cleanup code at appropriate locations
    
    for (size_t i = 0; i < rm->pending_count; i++) {
        CleanupAction* action = rm->pending_cleanups[i];
        
        // Generate appropriate cleanup code for each action
        // This would involve inserting cleanup calls into the generated code
        if (action && action->cleanup_code) {
            // TODO: Insert cleanup code into generated LLVM IR
            // This would typically involve:
            // 1. Finding the appropriate basic block
            // 2. Creating cleanup function calls
            // 3. Handling error paths and early returns
        }
    }
    
    return 1;
}