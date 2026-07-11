#include "memory_safety.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Memory safety integration with type system
// This file implements the integration of memory safety features with the type checker

// Memory safety context for type checking
typedef struct MemorySafetyContext {
    TypeChecker* type_checker;
    FlowSensitiveAnalyzer* flow_analyzer;
    ReferenceManager* reference_manager;
    EscapeAnalyzer* escape_analyzer;
    ResourceManager* resource_manager;
    
    // Safety checking flags
    int enable_null_safety;
    int enable_ownership_tracking;
    int enable_resource_management;
    int enable_escape_analysis;
    int enable_flow_analysis;
    
    // Error prevention statistics
    size_t null_errors_prevented;
    size_t use_after_free_prevented;
    size_t double_free_prevented;
    size_t resource_leaks_prevented;
    
    // Integration state
    int is_initialized;
    int error_count;
    int warning_count;
} MemorySafetyContext;

// Global memory safety context
static MemorySafetyContext* g_memory_safety_ctx = NULL;

// Memory safety context management
MemorySafetyContext* memory_safety_context_new(TypeChecker* type_checker) {
    if (!type_checker) return NULL;
    
    MemorySafetyContext* ctx = xmalloc(sizeof(MemorySafetyContext));
    if (!ctx) return NULL;
    
    memset(ctx, 0, sizeof(MemorySafetyContext));
    
    ctx->type_checker = type_checker;
    
    // Initialize memory safety components
    ctx->flow_analyzer = flow_analyzer_new(type_checker);
    if (!ctx->flow_analyzer) {
        free(ctx);
        return NULL;
    }
    
    ctx->reference_manager = reference_manager_new(ctx->flow_analyzer);
    if (!ctx->reference_manager) {
        flow_analyzer_free(ctx->flow_analyzer);
        free(ctx);
        return NULL;
    }
    
    ctx->escape_analyzer = escape_analyzer_new(ctx->reference_manager);
    if (!ctx->escape_analyzer) {
        reference_manager_free(ctx->reference_manager);
        flow_analyzer_free(ctx->flow_analyzer);
        free(ctx);
        return NULL;
    }
    
    ctx->resource_manager = resource_manager_new(type_checker);
    if (!ctx->resource_manager) {
        escape_analyzer_free(ctx->escape_analyzer);
        reference_manager_free(ctx->reference_manager);
        flow_analyzer_free(ctx->flow_analyzer);
        free(ctx);
        return NULL;
    }
    
    // Enable all safety features by default
    ctx->enable_null_safety = 1;
    ctx->enable_ownership_tracking = 1;
    ctx->enable_resource_management = 1;
    ctx->enable_escape_analysis = 1;
    ctx->enable_flow_analysis = 1;
    
    ctx->is_initialized = 1;
    
    return ctx;
}

void memory_safety_context_free(MemorySafetyContext* ctx) {
    if (!ctx) return;
    
    if (ctx->resource_manager) {
        resource_manager_free(ctx->resource_manager);
    }
    if (ctx->escape_analyzer) {
        escape_analyzer_free(ctx->escape_analyzer);
    }
    if (ctx->reference_manager) {
        reference_manager_free(ctx->reference_manager);
    }
    if (ctx->flow_analyzer) {
        flow_analyzer_free(ctx->flow_analyzer);
    }
    
    free(ctx);
}

// Initialize memory safety integration for a type checker
int integrate_memory_safety_with_type_checker(TypeChecker* type_checker) {
    if (!type_checker) return 0;
    
    // Create memory safety context
    MemorySafetyContext* ctx = memory_safety_context_new(type_checker);
    if (!ctx) return 0;
    
    // Set global context
    if (g_memory_safety_ctx) {
        memory_safety_context_free(g_memory_safety_ctx);
    }
    g_memory_safety_ctx = ctx;
    
    printf("Memory safety integration initialized successfully\n");
    return 1;
}

// Cleanup memory safety integration
void cleanup_memory_safety_integration(void) {
    if (g_memory_safety_ctx) {
        memory_safety_context_free(g_memory_safety_ctx);
        g_memory_safety_ctx = NULL;
    }
}

// Get current memory safety context
MemorySafetyContext* get_memory_safety_context(void) {
    return g_memory_safety_ctx;
}

// Enhanced type checking with memory safety
Type* memory_safe_type_check_expression(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || !g_memory_safety_ctx) {
        return type_check_expression(checker, expr);
    }
    
    MemorySafetyContext* ctx = g_memory_safety_ctx;
    
    // First perform standard type checking
    Type* expr_type = type_check_expression(checker, expr);
    if (!expr_type) return NULL;
    
    // Apply memory safety checks based on expression type
    switch (expr->type) {
        case AST_IDENTIFIER:
            return memory_safe_check_identifier_access(ctx, expr, expr_type);
            
        case AST_SELECTOR_EXPR:
            return memory_safe_check_field_access(ctx, expr, expr_type);
            
        case AST_INDEX_EXPR:
            return memory_safe_check_array_access(ctx, expr, expr_type);
            
        case AST_CALL_EXPR:
            return memory_safe_check_function_call(ctx, expr, expr_type);
            
        case AST_UNARY_EXPR:
            return memory_safe_check_unary_operation(ctx, expr, expr_type);
            
        case AST_BINARY_EXPR:
            return memory_safe_check_binary_operation(ctx, expr, expr_type);
            
        case AST_TRY_EXPR:
            return memory_safe_check_try_expression(ctx, expr, expr_type);
            
        default:
            // For other expressions, just return the type
            return expr_type;
    }
}

// Check identifier access for memory safety
Type* memory_safe_check_identifier_access(MemorySafetyContext* ctx, ASTNode* expr, Type* expr_type) {
    if (!ctx || !expr || !expr_type) return expr_type;
    
    IdentifierNode* id = (IdentifierNode*)expr;
    
    // Check if variable is moved
    Variable* var = type_checker_lookup_variable(ctx->type_checker, id->name);
    if (var && var->is_moved) {
        type_error(ctx->type_checker, expr->pos, 
                  "Use of moved variable '%s'", id->name);
        ctx->use_after_free_prevented++;
        return NULL;
    }
    
    // Check if accessing nullable type without null check
    if (ctx->enable_null_safety && type_is_nullable(expr_type)) {
        if (!is_null_checked_context(ctx, id->name, expr->pos)) {
            type_error(ctx->type_checker, expr->pos,
                      "Nullable variable '%s' must be checked for null before use", id->name);
            ctx->null_errors_prevented++;
            return NULL;
        }
    }
    
    // Track reference usage if enabled
    if (ctx->enable_ownership_tracking && ctx->reference_manager) {
        reference_manager_use_reference(ctx->reference_manager, id->name, expr->pos.offset);
    }
    
    return expr_type;
}

// Check field access for memory safety
Type* memory_safe_check_field_access(MemorySafetyContext* ctx, ASTNode* expr, Type* expr_type) {
    if (!ctx || !expr || !expr_type) return expr_type;
    
    SelectorExprNode* sel = (SelectorExprNode*)expr;
    
    // First check the base expression
    Type* base_type = memory_safe_type_check_expression(ctx->type_checker, sel->expr);
    if (!base_type) return NULL;
    
    // Check if base is nullable and needs null check
    if (ctx->enable_null_safety && type_is_nullable(base_type)) {
        type_error(ctx->type_checker, expr->pos,
                  "Cannot access field '%s' on nullable type without null check", sel->selector);
        ctx->null_errors_prevented++;
        return NULL;
    }
    
    // Check if base is a moved value
    if (sel->expr->type == AST_IDENTIFIER) {
        IdentifierNode* base_id = (IdentifierNode*)sel->expr;
        Variable* base_var = type_checker_lookup_variable(ctx->type_checker, base_id->name);
        if (base_var && base_var->is_moved) {
            type_error(ctx->type_checker, expr->pos,
                      "Cannot access field '%s' on moved value '%s'", sel->selector, base_id->name);
            ctx->use_after_free_prevented++;
            return NULL;
        }
    }
    
    return expr_type;
}

// Check array access for memory safety
Type* memory_safe_check_array_access(MemorySafetyContext* ctx, ASTNode* expr, Type* expr_type) {
    if (!ctx || !expr || !expr_type) return expr_type;
    
    IndexExprNode* idx = (IndexExprNode*)expr;
    
    // Check the array expression
    Type* array_type = memory_safe_type_check_expression(ctx->type_checker, idx->expr);
    if (!array_type) return NULL;
    
    // Check if array is nullable
    if (ctx->enable_null_safety && type_is_nullable(array_type)) {
        type_error(ctx->type_checker, expr->pos,
                  "Cannot index into nullable array without null check");
        ctx->null_errors_prevented++;
        return NULL;
    }
    
    // Check if array is a moved value
    if (idx->expr->type == AST_IDENTIFIER) {
        IdentifierNode* array_id = (IdentifierNode*)idx->expr;
        Variable* array_var = type_checker_lookup_variable(ctx->type_checker, array_id->name);
        if (array_var && array_var->is_moved) {
            type_error(ctx->type_checker, expr->pos,
                      "Cannot index into moved array '%s'", array_id->name);
            ctx->use_after_free_prevented++;
            return NULL;
        }
    }
    
    return expr_type;
}

// Check function call for memory safety
Type* memory_safe_check_function_call(MemorySafetyContext* ctx, ASTNode* expr, Type* expr_type) {
    if (!ctx || !expr || !expr_type) return expr_type;
    
    CallExprNode* call = (CallExprNode*)expr;
    
    // Check if calling function on moved value
    if (call->function->type == AST_SELECTOR_EXPR) {
        SelectorExprNode* sel = (SelectorExprNode*)call->function;
        if (sel->expr->type == AST_IDENTIFIER) {
            IdentifierNode* receiver = (IdentifierNode*)sel->expr;
            Variable* receiver_var = type_checker_lookup_variable(ctx->type_checker, receiver->name);
            if (receiver_var && receiver_var->is_moved) {
                type_error(ctx->type_checker, expr->pos,
                          "Cannot call method '%s' on moved value '%s'", sel->selector, receiver->name);
                ctx->use_after_free_prevented++;
                return NULL;
            }
        }
    }
    
    // Check resource allocation functions
    if (ctx->enable_resource_management && call->function->type == AST_IDENTIFIER) {
        IdentifierNode* func_id = (IdentifierNode*)call->function;
        ResourceType res_type = get_resource_type_for_function(func_id->name);
        
        if (res_type != RESOURCE_TYPE_UNKNOWN && ctx->resource_manager) {
            // This is a resource allocation - track it
            // Note: We'll need the variable name from the assignment context
            printf("Resource allocation detected: %s\n", func_id->name);
        }
    }
    
    return expr_type;
}

// Check unary operation for memory safety
Type* memory_safe_check_unary_operation(MemorySafetyContext* ctx, ASTNode* expr, Type* expr_type) {
    if (!ctx || !expr || !expr_type) return expr_type;
    
    UnaryExprNode* unary = (UnaryExprNode*)expr;
    
    // Check for forced unwrap of nullable (postfix !)
    if (unary->operator == TOKEN_BANG) {
        Type* operand_type = memory_safe_type_check_expression(ctx->type_checker, unary->operand);
        if (operand_type && type_is_nullable(operand_type)) {
            // This is a forced unwrap - check if it's safe
            if (unary->operand->type == AST_IDENTIFIER) {
                IdentifierNode* id = (IdentifierNode*)unary->operand;
                if (!is_guaranteed_non_null(ctx, id->name, expr->pos)) {
                    type_warning(ctx->type_checker, expr->pos,
                               "Forced unwrap of '%s' may panic if null", id->name);
                }
            }
        }
    }
    
    return expr_type;
}

// Check binary operation for memory safety
Type* memory_safe_check_binary_operation(MemorySafetyContext* ctx, ASTNode* expr, Type* expr_type) {
    if (!ctx || !expr || !expr_type) return expr_type;
    
    BinaryExprNode* binary = (BinaryExprNode*)expr;
    
    // Check assignment operations for move semantics
    if (binary->operator == TOKEN_ASSIGN) {
        return memory_safe_check_assignment(ctx, binary, expr_type);
    }
    
    return expr_type;
}

// Check assignment for memory safety
Type* memory_safe_check_assignment(MemorySafetyContext* ctx, BinaryExprNode* assign, Type* expr_type) {
    if (!ctx || !assign) return expr_type;
    
    // Check if we're assigning a resource-creating function call
    if (assign->right->type == AST_CALL_EXPR && assign->left->type == AST_IDENTIFIER) {
        CallExprNode* call = (CallExprNode*)assign->right;
        IdentifierNode* var = (IdentifierNode*)assign->left;
        
        if (call->function->type == AST_IDENTIFIER) {
            IdentifierNode* func = (IdentifierNode*)call->function;
            ResourceType res_type = get_resource_type_for_function(func->name);
            
            if (res_type != RESOURCE_TYPE_UNKNOWN && ctx->resource_manager) {
                // Track this resource assignment
                Position pos = assign->base.pos;
                resource_manager_track_resource(ctx->resource_manager, var->name, res_type, assign->right, pos);
                printf("Tracked resource assignment: %s = %s()\n", var->name, func->name);
            }
        }
    }
    
    // Check for move vs copy semantics
    if (ctx->enable_ownership_tracking && assign->right->type == AST_IDENTIFIER) {
        IdentifierNode* source = (IdentifierNode*)assign->right;
        IdentifierNode* target = (IdentifierNode*)assign->left;
        
        Variable* source_var = type_checker_lookup_variable(ctx->type_checker, source->name);
        if (source_var && should_move_value(ctx, source_var, assign->base.pos)) {
            // Mark source as moved
            source_var->is_moved = 1;
            printf("Moved value from '%s' to '%s'\n", source->name, target->name);
        }
    }
    
    return expr_type;
}

// Check try expression for memory safety
Type* memory_safe_check_try_expression(MemorySafetyContext* ctx, ASTNode* expr, Type* expr_type) {
    if (!ctx || !expr || !expr_type) return expr_type;
    
    TryExprNode* try_expr = (TryExprNode*)expr;
    
    // Check the inner expression
    Type* inner_type = memory_safe_type_check_expression(ctx->type_checker, try_expr->expr);
    if (!inner_type) return NULL;
    
    // Verify it's actually an error union type
    if (!type_is_error_union(inner_type)) {
        type_error(ctx->type_checker, expr->pos,
                  "Try operator can only be used with error union types");
        return NULL;
    }
    
    return expr_type;
}

// Helper functions for null safety checking
int is_null_checked_context(MemorySafetyContext* ctx, const char* var_name, Position pos) {
    // TODO: Implement flow-sensitive null checking
    // For now, assume variables are checked if they're in an if-let context
    return 0; // Conservative: require explicit null checks
}

int is_guaranteed_non_null(MemorySafetyContext* ctx, const char* var_name, Position pos) {
    // TODO: Implement flow-sensitive analysis to determine if variable is guaranteed non-null
    return 0; // Conservative: no guarantees without explicit checks
}

int should_move_value(MemorySafetyContext* ctx, Variable* var, Position pos) {
    // Simple heuristic: move if this is the last use of the variable
    // TODO: Implement proper last-use analysis
    return 0; // Conservative: don't move unless explicitly requested
}

// Enhanced statement checking with memory safety
int memory_safe_type_check_statement(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt || !g_memory_safety_ctx) {
        return type_check_statement(checker, stmt);
    }
    
    MemorySafetyContext* ctx = g_memory_safety_ctx;
    
    // First perform standard type checking
    int result = type_check_statement(checker, stmt);
    if (!result) return 0;
    
    // Apply memory safety checks based on statement type
    switch (stmt->type) {
        case AST_VAR_DECL:
            return memory_safe_check_variable_declaration(ctx, stmt);
            
        case AST_DEFER_STMT:
            return memory_safe_check_defer_statement(ctx, stmt);
            
        case AST_BLOCK_STMT:
            return memory_safe_check_block_statement(ctx, stmt);
            
        case AST_IF_LET_STMT:
            return memory_safe_check_if_let_statement(ctx, stmt);
            
        default:
            return result;
    }
}

// Check variable declaration for memory safety
int memory_safe_check_variable_declaration(MemorySafetyContext* ctx, ASTNode* stmt) {
    if (!ctx || !stmt) return 1;
    
    VarDeclNode* var_decl = (VarDeclNode*)stmt;
    
    // Analyze each variable declaration
    for (size_t i = 0; i < var_decl->name_count; i++) {
        const char* var_name = var_decl->names[i];
        
        // Check if this is a resource allocation
        if (ctx->enable_resource_management && var_decl->values && ctx->resource_manager) {
            resource_manager_analyze_statement(ctx->resource_manager, stmt);
        }
        
        // Track in flow analysis if enabled
        if (ctx->enable_flow_analysis && ctx->flow_analyzer) {
            // TODO: Add to flow analysis tracking
        }
    }
    
    return 1;
}

// Check defer statement for memory safety
int memory_safe_check_defer_statement(MemorySafetyContext* ctx, ASTNode* stmt) {
    if (!ctx || !stmt) return 1;
    
    if (ctx->enable_resource_management && ctx->resource_manager) {
        DeferStmtNode* defer_stmt = (DeferStmtNode*)stmt;
        resource_manager_process_defer(ctx->resource_manager, stmt, stmt->pos);
    }
    
    return 1;
}

// Check block statement for memory safety
int memory_safe_check_block_statement(MemorySafetyContext* ctx, ASTNode* stmt) {
    if (!ctx || !stmt) return 1;
    
    // Enter scope for resource management
    if (ctx->enable_resource_management && ctx->resource_manager) {
        resource_manager_enter_scope(ctx->resource_manager, stmt);
    }
    
    // Enter scope for reference management
    if (ctx->enable_ownership_tracking && ctx->reference_manager) {
        reference_manager_enter_scope(ctx->reference_manager, LIFETIME_SCOPE_BLOCK, stmt->pos.offset);
    }
    
    // Process block contents
    BlockStmtNode* block = (BlockStmtNode*)stmt;
    ASTNode* current = block->statements;
    while (current) {
        memory_safe_type_check_statement(ctx->type_checker, current);
        current = current->next;
    }
    
    // Exit scopes
    if (ctx->enable_ownership_tracking && ctx->reference_manager) {
        reference_manager_exit_scope(ctx->reference_manager, stmt->pos.offset + 1);
    }
    
    if (ctx->enable_resource_management && ctx->resource_manager) {
        resource_manager_exit_scope(ctx->resource_manager);
    }
    
    return 1;
}

// Check if-let statement for null safety
int memory_safe_check_if_let_statement(MemorySafetyContext* ctx, ASTNode* stmt) {
    if (!ctx || !stmt || !ctx->enable_null_safety) return 1;
    
    IfLetStmtNode* if_let = (IfLetStmtNode*)stmt;
    
    // Check that the expression is nullable
    Type* expr_type = memory_safe_type_check_expression(ctx->type_checker, if_let->nullable_expr);
    if (!expr_type || !type_is_nullable(expr_type)) {
        type_error(ctx->type_checker, stmt->pos,
                  "if-let can only be used with nullable types");
        return 0;
    }
    
    // In the then branch, the variable is guaranteed non-null
    // TODO: Add this information to the flow-sensitive analysis
    
    return 1;
}

// Statistics and reporting
void memory_safety_print_statistics(void) {
    if (!g_memory_safety_ctx) {
        printf("Memory safety integration not initialized\n");
        return;
    }
    
    MemorySafetyContext* ctx = g_memory_safety_ctx;
    
    printf("=== Memory Safety Statistics ===\n");
    printf("Null pointer errors prevented: %zu\n", ctx->null_errors_prevented);
    printf("Use-after-free errors prevented: %zu\n", ctx->use_after_free_prevented);
    printf("Double-free errors prevented: %zu\n", ctx->double_free_prevented);
    printf("Resource leaks prevented: %zu\n", ctx->resource_leaks_prevented);
    printf("Total errors prevented: %zu\n", 
           ctx->null_errors_prevented + ctx->use_after_free_prevented + 
           ctx->double_free_prevented + ctx->resource_leaks_prevented);
    
    printf("\nEnabled safety features:\n");
    printf("- Null safety: %s\n", ctx->enable_null_safety ? "enabled" : "disabled");
    printf("- Ownership tracking: %s\n", ctx->enable_ownership_tracking ? "enabled" : "disabled");
    printf("- Resource management: %s\n", ctx->enable_resource_management ? "enabled" : "disabled");
    printf("- Escape analysis: %s\n", ctx->enable_escape_analysis ? "enabled" : "disabled");
    printf("- Flow analysis: %s\n", ctx->enable_flow_analysis ? "enabled" : "disabled");
    
    // Print subsystem statistics
    if (ctx->resource_manager) {
        printf("\n--- Resource Manager Statistics ---\n");
        resource_manager_print_statistics(ctx->resource_manager);
    }
    
    if (ctx->reference_manager) {
        printf("\n--- Reference Manager Statistics ---\n");
        reference_manager_print_statistics(ctx->reference_manager);
    }
    
    if (ctx->flow_analyzer) {
        printf("\n--- Flow Analyzer Statistics ---\n");
        flow_analyzer_print_statistics(ctx->flow_analyzer);
    }
    
    if (ctx->escape_analyzer) {
        printf("\n--- Escape Analyzer Statistics ---\n");
        escape_analyzer_print_statistics(ctx->escape_analyzer);
    }
}

// Configuration functions
void memory_safety_enable_feature(const char* feature, int enable) {
    if (!g_memory_safety_ctx) return;
    
    MemorySafetyContext* ctx = g_memory_safety_ctx;
    
    if (strcmp(feature, "null_safety") == 0) {
        ctx->enable_null_safety = enable;
    } else if (strcmp(feature, "ownership_tracking") == 0) {
        ctx->enable_ownership_tracking = enable;
    } else if (strcmp(feature, "resource_management") == 0) {
        ctx->enable_resource_management = enable;
    } else if (strcmp(feature, "escape_analysis") == 0) {
        ctx->enable_escape_analysis = enable;
    } else if (strcmp(feature, "flow_analysis") == 0) {
        ctx->enable_flow_analysis = enable;
    }
}

int memory_safety_is_feature_enabled(const char* feature) {
    if (!g_memory_safety_ctx) return 0;
    
    MemorySafetyContext* ctx = g_memory_safety_ctx;
    
    if (strcmp(feature, "null_safety") == 0) {
        return ctx->enable_null_safety;
    } else if (strcmp(feature, "ownership_tracking") == 0) {
        return ctx->enable_ownership_tracking;
    } else if (strcmp(feature, "resource_management") == 0) {
        return ctx->enable_resource_management;
    } else if (strcmp(feature, "escape_analysis") == 0) {
        return ctx->enable_escape_analysis;
    } else if (strcmp(feature, "flow_analysis") == 0) {
        return ctx->enable_flow_analysis;
    }
    
    return 0;
}