#include "contracts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// C23 compatibility check. Under CompCert the underlying-type
// specifier on `typedef enum : unsigned char` is stripped (see
// ccomp_shim.h's GOO_ENUM_U8) and the enum widens to int, so the
// 1-byte expectation no longer holds. The assertion still verifies
// the intended layout on the regular C23 build.
#ifndef __COMPCERT__
_Static_assert(sizeof(ContractType) == 1, "ContractType should be 1 byte");
#endif

// =============================================================================
// Contract Creation Functions using C23 features
// =============================================================================

ContractExpression* contract_expression_create(
    ContractType type,
    struct ASTNode* condition,
    const char* description
) {
    ContractExpression* expr = xmalloc(sizeof(ContractExpression));
    if (!expr) return NULL;
    
    // Use C23 designated initializers
    *expr = (ContractExpression) {
        .type = type,
        .condition = condition,
        .message = NULL,
        .line = condition ? condition->pos.line : 0,
        .column = condition ? condition->pos.column : 0,
        .filename = (condition && condition->pos.filename) ? strdup(condition->pos.filename) : NULL,
        .description = description ? strdup(description) : NULL,
        .is_compile_time = false,
        .next = NULL
    };
    
    return expr;
}

FunctionContract* function_contract_create(const char* function_name) {
    if (!function_name) return NULL;
    
    FunctionContract* contract = xmalloc(sizeof(FunctionContract));
    if (!contract) return NULL;
    
    contract->function_name = strdup(function_name);
    contract->preconditions = NULL;
    contract->postconditions = NULL;
    contract->invariants = NULL;
    contract->return_type = NULL;
    contract->parameters = NULL;
    contract->parameter_count = 0;
    contract->next = NULL;
    
    return contract;
}

LoopContract* loop_contract_create(struct ASTNode* loop_node) {
    if (!loop_node) return NULL;
    
    LoopContract* contract = xmalloc(sizeof(LoopContract));
    if (!contract) return NULL;
    
    contract->loop_node = loop_node;
    contract->invariants = NULL;
    contract->variant = NULL;
    contract->next = NULL;
    
    return contract;
}

ContractContext* contract_context_create(void) {
    ContractContext* ctx = xmalloc(sizeof(ContractContext));
    if (!ctx) return NULL;
    
    ctx->function_contracts = NULL;
    ctx->loop_contracts = NULL;
    ctx->global_invariants = NULL;
    ctx->verification_enabled = 1;  // Enable by default
    ctx->optimization_level = 1;    // Moderate optimization
    
    return ctx;
}

// =============================================================================
// Contract Management Functions
// =============================================================================

int function_contract_add_precondition(
    FunctionContract* contract,
    struct ASTNode* condition,
    const char* description
) {
    if (!contract) return 0;
    
    ContractExpression* expr = contract_expression_create(
        CONTRACT_PRECONDITION, condition, description
    );
    if (!expr) return 0;
    
    // Add to the front of the list
    expr->next = contract->preconditions;
    contract->preconditions = expr;
    
    return 1;
}

int function_contract_add_postcondition(
    FunctionContract* contract,
    struct ASTNode* condition,
    const char* description
) {
    if (!contract) return 0;
    
    ContractExpression* expr = contract_expression_create(
        CONTRACT_POSTCONDITION, condition, description
    );
    if (!expr) return 0;
    
    // Add to the front of the list
    expr->next = contract->postconditions;
    contract->postconditions = expr;
    
    return 1;
}

int loop_contract_add_invariant(
    LoopContract* contract,
    struct ASTNode* condition,
    const char* description
) {
    if (!contract || !condition) return 0;
    
    ContractExpression* expr = contract_expression_create(
        CONTRACT_INVARIANT, condition, description
    );
    if (!expr) return 0;
    
    // Add to the front of the list
    expr->next = contract->invariants;
    contract->invariants = expr;
    
    return 1;
}

int contract_context_add_function(ContractContext* ctx, FunctionContract* contract) {
    if (!ctx || !contract) return 0;
    
    // Add to the front of the list
    contract->next = ctx->function_contracts;
    ctx->function_contracts = contract;
    
    return 1;
}

int contract_context_add_loop(ContractContext* ctx, LoopContract* contract) {
    if (!ctx || !contract) return 0;
    
    // Add to the front of the list
    contract->next = ctx->loop_contracts;
    ctx->loop_contracts = contract;
    
    return 1;
}

// =============================================================================
// Contract Verification Functions
// =============================================================================

ContractVerificationInfo* verify_contract_expression(
    ContractExpression* expr,
    DependentTypeContext* type_ctx,
    struct ASTNode* current_scope
) {
    if (!expr) return NULL;
    
    ContractVerificationInfo* info = xmalloc(sizeof(ContractVerificationInfo));
    if (!info) return NULL;
    
    info->result = CONTRACT_UNKNOWN;
    info->error_message = NULL;
    info->can_optimize_away = 0;
    info->witness = NULL;
    
    // Basic analysis of the contract condition
    if (!expr->condition) {
        info->result = CONTRACT_VIOLATED;
        info->error_message = strdup("Contract condition is null");
        return info;
    }
    
    // Check if condition is a compile-time constant
    if (expr->condition->type == AST_LITERAL) {
        LiteralNode* literal = (LiteralNode*)expr->condition;
        if (literal->literal_type == TOKEN_TRUE || literal->literal_type == TOKEN_FALSE) {
            if (literal->literal_type == TOKEN_TRUE) {
                info->result = CONTRACT_VERIFIED;
                info->can_optimize_away = 1;
            } else {
                info->result = CONTRACT_VIOLATED;
                info->error_message = strdup("Contract condition is statically false");
            }
            return info;
        }
    }
    
    // For more complex expressions, we'd need full symbolic execution
    // For now, mark as needing runtime check
    info->result = CONTRACT_RUNTIME_CHECK;
    info->can_optimize_away = 0;
    
    return info;
}

int verify_function_contracts(
    FunctionContract* contract,
    struct ASTNode* function_body,
    DependentTypeContext* type_ctx
) {
    if (!contract) return 1;  // No contracts = trivially satisfied
    
    int all_verified = 1;
    
    // Verify preconditions
    ContractExpression* expr = contract->preconditions;
    while (expr) {
        ContractVerificationInfo* info = verify_contract_expression(
            expr, type_ctx, function_body
        );
        
        if (info && info->result == CONTRACT_VIOLATED) {
            printf("Precondition violation in function %s: %s\n",
                   contract->function_name,
                   info->error_message ? info->error_message : "Unknown error");
            all_verified = 0;
        }
        
        if (info) contract_verification_info_free(info);
        expr = expr->next;
    }
    
    // Verify postconditions
    expr = contract->postconditions;
    while (expr) {
        ContractVerificationInfo* info = verify_contract_expression(
            expr, type_ctx, function_body
        );
        
        if (info && info->result == CONTRACT_VIOLATED) {
            printf("Postcondition violation in function %s: %s\n",
                   contract->function_name,
                   info->error_message ? info->error_message : "Unknown error");
            all_verified = 0;
        }
        
        if (info) contract_verification_info_free(info);
        expr = expr->next;
    }
    
    return all_verified;
}

int verify_loop_contracts(
    LoopContract* contract,
    DependentTypeContext* type_ctx
) {
    if (!contract) return 1;  // No contracts = trivially satisfied
    
    int all_verified = 1;
    
    // Verify loop invariants
    ContractExpression* expr = contract->invariants;
    while (expr) {
        ContractVerificationInfo* info = verify_contract_expression(
            expr, type_ctx, contract->loop_node
        );
        
        if (info && info->result == CONTRACT_VIOLATED) {
            printf("Loop invariant violation at line %d: %s\n",
                   expr->line,
                   info->error_message ? info->error_message : "Unknown error");
            all_verified = 0;
        }
        
        if (info) contract_verification_info_free(info);
        expr = expr->next;
    }
    
    return all_verified;
}

int verify_all_contracts(ContractContext* ctx, DependentTypeContext* type_ctx) {
    if (!ctx) return 1;
    
    int all_verified = 1;
    
    // Verify function contracts
    FunctionContract* func_contract = ctx->function_contracts;
    while (func_contract) {
        if (!verify_function_contracts(func_contract, NULL, type_ctx)) {
            all_verified = 0;
        }
        func_contract = func_contract->next;
    }
    
    // Verify loop contracts
    LoopContract* loop_contract = ctx->loop_contracts;
    while (loop_contract) {
        if (!verify_loop_contracts(loop_contract, type_ctx)) {
            all_verified = 0;
        }
        loop_contract = loop_contract->next;
    }
    
    return all_verified;
}

// =============================================================================
// Contract-based Optimization Functions
// =============================================================================

int optimize_with_contracts(
    ContractContext* ctx,
    struct ASTNode* ast,
    DependentTypeContext* type_ctx
) {
    if (!ctx || !ast) return 0;
    
    int optimizations_applied = 0;
    
    // Example optimization: eliminate null checks when we have NonNull contracts
    // Example optimization: eliminate bounds checks when we have range contracts
    // This would require deeper integration with the optimizer
    
    // For now, just demonstrate the concept
    if (ctx->optimization_level > 0) {
        optimizations_applied += eliminate_bounds_checks(ctx, ast);
    }
    
    return optimizations_applied;
}

int eliminate_bounds_checks(
    ContractContext* ctx,
    struct ASTNode* ast
) {
    if (!ctx || !ast) return 0;
    
    // This would analyze the AST to find array accesses and check if
    // contracts guarantee they're within bounds
    // For now, just return 0 (no optimizations)
    
    return 0;
}

// =============================================================================
// Integration with Dependent Types
// =============================================================================

DependentType* contract_to_dependent_constraint(
    ContractExpression* expr,
    struct Type* base_type
) {
    if (!expr || !base_type) return NULL;
    
    // Convert contract expression to dependent type constraint
    // This requires mapping contract conditions to constraint types
    
    DependentType* dep_type = dependent_type_new(DEPENDENT_BOUNDED_VEC, "contract_type");
    if (!dep_type) return NULL;
    
    // For now, create a generic constraint
    // In a full implementation, we'd parse the contract condition
    // and create appropriate constraints
    
    return dep_type;
}

int verify_contract_with_dependent_types(
    ContractExpression* expr,
    DependentType* dep_type,
    DependentTypeContext* type_ctx
) {
    if (!expr || !dep_type) return 0;
    
    // Use the dependent type system to verify the contract
    // This would involve constraint solving
    
    return 1;  // Assume verified for now
}

// =============================================================================
// Error Handling and Reporting
// =============================================================================

void report_contract_violation(
    ContractViolation* violation
) {
    if (!violation) return;
    
    printf("Contract violation: %s\n", violation->violation_message);
    printf("  Function: %s\n", violation->function_name ? violation->function_name : "unknown");
    printf("  Location: line %d, column %d\n", violation->line, violation->column);
    
    const char* contract_type_str;
    switch (violation->contract_type) {
        case CONTRACT_PRECONDITION:
            contract_type_str = "precondition";
            break;
        case CONTRACT_POSTCONDITION:
            contract_type_str = "postcondition";
            break;
        case CONTRACT_INVARIANT:
            contract_type_str = "invariant";
            break;
        case CONTRACT_ASSERTION:
            contract_type_str = "assertion";
            break;
        case CONTRACT_ASSUMPTION:
            contract_type_str = "assumption";
            break;
        default:
            contract_type_str = "unknown";
            break;
    }
    printf("  Type: %s\n", contract_type_str);
}

char* generate_contract_error_message(
    ContractExpression* expr,
    struct ASTNode* context
) {
    if (!expr) return NULL;
    
    const char* base_message;
    switch (expr->type) {
        case CONTRACT_PRECONDITION:
            base_message = "Precondition violated";
            break;
        case CONTRACT_POSTCONDITION:
            base_message = "Postcondition violated";
            break;
        case CONTRACT_INVARIANT:
            base_message = "Invariant violated";
            break;
        case CONTRACT_ASSERTION:
            base_message = "Assertion failed";
            break;
        case CONTRACT_ASSUMPTION:
            base_message = "Assumption violated";
            break;
        default:
            base_message = "Contract violated";
            break;
    }
    
    if (expr->description) {
        size_t len = strlen(base_message) + strlen(expr->description) + 10;
        char* message = malloc(len);
        if (message) {
            snprintf(message, len, "%s: %s", base_message, expr->description);
        }
        return message;
    }
    
    return strdup(base_message);
}

// =============================================================================
// Memory Management
// =============================================================================

void contract_expression_free(ContractExpression* expr) {
    while (expr) {
        ContractExpression* next = expr->next;
        
        if (expr->description) free(expr->description);
        // Note: condition and message are AST nodes, managed elsewhere
        
        free(expr);
        expr = next;
    }
}

void function_contract_free(FunctionContract* contract) {
    while (contract) {
        FunctionContract* next = contract->next;
        
        if (contract->function_name) free(contract->function_name);
        contract_expression_free(contract->preconditions);
        contract_expression_free(contract->postconditions);
        contract_expression_free(contract->invariants);
        // Note: return_type and parameters are managed elsewhere
        
        free(contract);
        contract = next;
    }
}

void loop_contract_free(LoopContract* contract) {
    while (contract) {
        LoopContract* next = contract->next;
        
        contract_expression_free(contract->invariants);
        // Note: loop_node and variant are AST nodes, managed elsewhere
        
        free(contract);
        contract = next;
    }
}

void contract_context_free(ContractContext* ctx) {
    if (!ctx) return;
    
    function_contract_free(ctx->function_contracts);
    loop_contract_free(ctx->loop_contracts);
    contract_expression_free(ctx->global_invariants);
    
    free(ctx);
}

void contract_verification_info_free(ContractVerificationInfo* info) {
    if (!info) return;
    
    if (info->error_message) free(info->error_message);
    // Note: witness is an AST node, managed elsewhere
    
    free(info);
}

void contract_violation_free(ContractViolation* violation) {
    while (violation) {
        ContractViolation* next = violation->next;
        
        if (violation->function_name) free(violation->function_name);
        if (violation->violation_message) free(violation->violation_message);
        // Note: violating_expression is an AST node, managed elsewhere
        
        free(violation);
        violation = next;
    }
}

// =============================================================================
// Utility Functions
// =============================================================================

int is_valid_contract_condition(struct ASTNode* expr) {
    if (!expr) return 0;
    
    // Contract conditions must be boolean expressions
    // They should also be side-effect free
    
    switch (expr->type) {
        case AST_LITERAL: {
            LiteralNode* literal = (LiteralNode*)expr;
            return literal->literal_type == TOKEN_TRUE || literal->literal_type == TOKEN_FALSE;
        }
            
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)expr;
            // Comparison operators are valid
            switch (binary->operator) {
                case TOKEN_EQ:
                case TOKEN_NE:
                case TOKEN_LT:
                case TOKEN_LE:
                case TOKEN_GT:
                case TOKEN_GE:
                case TOKEN_AND:
                case TOKEN_OR:
                    return is_valid_contract_condition(binary->left) &&
                           is_valid_contract_condition(binary->right);
                default:
                    return 0;
            }
            
        case AST_UNARY_EXPR: {
            UnaryExprNode* unary = (UnaryExprNode*)expr;
            if (unary->operator == TOKEN_NOT) {
                return is_valid_contract_condition(unary->operand);
            }
            return 0;
        }
            
        case AST_IDENTIFIER:
        case AST_SELECTOR_EXPR:
        case AST_INDEX_EXPR:
            // Variable references are valid in contracts
            return 1;
            
        case AST_CALL_EXPR:
            // Function calls are only valid if they're pure functions
            // For now, we'll be conservative and disallow them
            return 0;
            
        default:
            return 0;
    }
}
}

struct ASTNode* simplify_contract_expression(
    struct ASTNode* expr,
    DependentTypeContext* type_ctx
) {
    if (!expr) return NULL;
    
    // This would perform algebraic simplification of contract expressions
    // For now, just return the original expression
    return expr;
}

char* contract_to_string(ContractExpression* expr) {
    if (!expr) return NULL;
    
    const char* prefix;
    switch (expr->type) {
        case CONTRACT_PRECONDITION:
            prefix = "requires";
            break;
        case CONTRACT_POSTCONDITION:
            prefix = "ensures";
            break;
        case CONTRACT_INVARIANT:
            prefix = "invariant";
            break;
        case CONTRACT_ASSERTION:
            prefix = "assert";
            break;
        case CONTRACT_ASSUMPTION:
            prefix = "assume";
            break;
        default:
            prefix = "contract";
            break;
    }
    
    if (expr->description) {
        size_t len = strlen(prefix) + strlen(expr->description) + 10;
        char* result = malloc(len);
        if (result) {
            snprintf(result, len, "%s %s", prefix, expr->description);
        }
        return result;
    }
    
    return strdup(prefix);
}

int is_contract_compatible(
    ContractExpression* expr,
    struct Type* function_type
) {
    if (!expr || !function_type) return 0;
    
    // Check if the contract expression is compatible with the function signature
    // This would involve checking that all referenced variables are in scope
    // For now, just return true
    
    return 1;
}
