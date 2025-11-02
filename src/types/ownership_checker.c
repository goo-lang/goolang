#include "types.h"
#include "memory_safety.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Ownership tracking and null safety implementation

// Check if an expression represents a move operation
int is_move_operation(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr) return 0;
    
    // Simple heuristic: assignment of owned variables is a move
    if (expr->type == AST_IDENTIFIER) {
        IdentifierNode* ident = (IdentifierNode*)expr;
        Variable* var = type_checker_lookup_variable(checker, ident->name);
        if (var && var->ownership == OWNERSHIP_OWNED) {
            return 1;
        }
    }
    
    return 0;
}

// Mark a variable as moved
void mark_variable_moved(TypeChecker* checker, const char* name, Position pos) {
    if (!checker || !name) return;
    
    Variable* var = type_checker_lookup_variable(checker, name);
    if (var) {
        var->is_moved = 1;
        var->declared_pos = pos;  // Update position for error reporting
    }
}

// Check ownership rules for assignment
int check_ownership_assignment(TypeChecker* checker, ASTNode* lvalue, ASTNode* rvalue, Position pos) {
    if (!checker || !lvalue || !rvalue) return 0;
    
    // If rvalue is a move operation, mark the source as moved
    if (rvalue->type == AST_IDENTIFIER && is_move_operation(checker, rvalue)) {
        IdentifierNode* ident = (IdentifierNode*)rvalue;
        Variable* var = type_checker_lookup_variable(checker, ident->name);
        
        if (var) {
            // Check if variable is already moved
            if (var->is_moved) {
                type_error(checker, pos,
                          "Use of moved variable '%s'", ident->name);
                return 0;
            }
            
            // Mark as moved
            mark_variable_moved(checker, ident->name, pos);
        }
    }
    
    return 1;
}

// Check if a type can be null
int type_can_be_null(const Type* type) {
    if (!type) return 0;
    
    return type->kind == TYPE_NULLABLE ||
           type_is_pointer_like(type) ||
           type->kind == TYPE_UNKNOWN;  // nil type
}

// Check null safety for dereference operations
int check_null_safety_dereference(TypeChecker* checker, ASTNode* expr __attribute__((unused)), Type* expr_type, Position pos) {
    if (!checker || !expr_type) return 0;
    
    // If type is nullable, require explicit null check
    if (type_is_nullable(expr_type)) {
        type_error(checker, pos,
                  "Cannot dereference nullable type %s without null check",
                  type_to_string(expr_type));
        return 0;
    }
    
    return 1;
}

// Check null safety for assignment
int check_null_safety_assignment(TypeChecker* checker, Type* target_type, Type* source_type, Position pos) {
    if (!checker || !target_type || !source_type) return 0;
    
    // Assigning nil to non-nullable type
    if (source_type->kind == TYPE_UNKNOWN && target_type->kind != TYPE_NULLABLE) {
        type_error(checker, pos,
                  "Cannot assign nil to non-nullable type %s",
                  type_to_string(target_type));
        return 0;
    }
    
    // Assigning nullable to non-nullable without explicit check
    if (type_is_nullable(source_type) && !type_is_nullable(target_type)) {
        type_error(checker, pos,
                  "Cannot assign nullable type %s to non-nullable type %s without explicit unwrapping",
                  type_to_string(source_type), type_to_string(target_type));
        return 0;
    }
    
    return 1;
}

// Enhanced assignment type checking with ownership and null safety
Type* type_check_assignment_with_safety(TypeChecker* checker, ASTNode* lvalue, ASTNode* rvalue, Position pos) {
    if (!checker || !lvalue || !rvalue) return NULL;
    
    Type* left_type = type_check_expression(checker, lvalue);
    Type* right_type = type_check_expression(checker, rvalue);
    
    if (!left_type || !right_type) return NULL;
    
    // Check ownership rules
    if (!check_ownership_assignment(checker, lvalue, rvalue, pos)) {
        return NULL;
    }
    
    // Check null safety
    if (!check_null_safety_assignment(checker, left_type, right_type, pos)) {
        return NULL;
    }
    
    // Check basic type compatibility
    if (!type_compatible(right_type, left_type)) {
        type_error(checker, pos,
                  "Cannot assign %s to %s",
                  type_to_string(right_type), type_to_string(left_type));
        return NULL;
    }
    
    return left_type;
}

// Check if an expression is safe to use (not moved, not null)
int is_expression_safe_to_use(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr) return 0;
    
    switch (expr->type) {
        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)expr;
            Variable* var = type_checker_lookup_variable(checker, ident->name);
            
            if (!var) {
                return 0;  // Undefined variable
            }
            
            if (var->is_moved) {
                return 0;  // Variable has been moved
            }
            
            if (!var->is_initialized) {
                return 0;  // Uninitialized variable
            }
            
            return 1;
        }
        
        case AST_LITERAL:
            return 1;  // Literals are always safe
            
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)expr;
            return is_expression_safe_to_use(checker, binary->left) &&
                   is_expression_safe_to_use(checker, binary->right);
        }
        
        case AST_UNARY_EXPR: {
            UnaryExprNode* unary = (UnaryExprNode*)expr;
            return is_expression_safe_to_use(checker, unary->operand);
        }
        
        case AST_INDEX_EXPR: {
            IndexExprNode* index = (IndexExprNode*)expr;
            if (!is_expression_safe_to_use(checker, index->expr) ||
                !is_expression_safe_to_use(checker, index->index)) {
                return 0;
            }
            
            // Additional null safety check for array/slice access
            Type* expr_type = type_check_expression(checker, index->expr);
            if (expr_type && type_is_nullable(expr_type)) {
                return 0;  // Cannot index nullable types without unwrapping
            }
            
            return 1;
        }
        
        case AST_SELECTOR_EXPR: {
            SelectorExprNode* selector = (SelectorExprNode*)expr;
            if (!is_expression_safe_to_use(checker, selector->expr)) {
                return 0;
            }
            
            // Additional null safety check for field access
            Type* expr_type = type_check_expression(checker, selector->expr);
            if (expr_type && type_is_nullable(expr_type)) {
                return 0;  // Cannot access fields of nullable types without unwrapping
            }
            
            return 1;
        }
        
        default:
            return 1;  // Assume other expressions are safe for now
    }
}

// Perform ownership and null safety analysis on a statement
int analyze_statement_safety(TypeChecker* checker, ASTNode* stmt) {
    if (!checker || !stmt) return 0;
    
    switch (stmt->type) {
        case AST_EXPR_STMT: {
            ExprStmtNode* expr_stmt = (ExprStmtNode*)stmt;
            return is_expression_safe_to_use(checker, expr_stmt->expr);
        }
        
        case AST_RETURN_STMT: {
            ReturnStmtNode* ret_stmt = (ReturnStmtNode*)stmt;
            if (ret_stmt->values) {
                return is_expression_safe_to_use(checker, ret_stmt->values);
            }
            return 1;
        }
        
        case AST_IF_STMT: {
            IfStmtNode* if_stmt = (IfStmtNode*)stmt;
            
            if (!is_expression_safe_to_use(checker, if_stmt->condition)) {
                return 0;
            }
            
            // TODO: Handle control flow analysis for null safety
            // If condition checks for null, variables should be considered non-null in then branch
            
            return analyze_statement_safety(checker, if_stmt->then_stmt) &&
                   (!if_stmt->else_stmt || analyze_statement_safety(checker, if_stmt->else_stmt));
        }
        
        case AST_BLOCK_STMT: {
            BlockStmtNode* block = (BlockStmtNode*)stmt;
            ASTNode* current = block->statements;
            
            while (current) {
                if (!analyze_statement_safety(checker, current)) {
                    return 0;
                }
                current = current->next;
            }
            
            return 1;
        }
        
        default:
            return 1;  // Assume other statements are safe for now
    }
}

// Ownership transfer for function calls
int check_function_call_ownership(TypeChecker* checker, CallExprNode* call, Position pos) {
    if (!checker || !call) return 0;
    
    // Check if arguments are safe to pass
    ASTNode* arg = call->args;
    while (arg) {
        if (!is_expression_safe_to_use(checker, arg)) {
            type_error(checker, pos,
                      "Cannot pass moved or uninitialized value as function argument");
            return 0;
        }
        
        // If argument is an owned variable, it may be moved depending on parameter type
        if (arg->type == AST_IDENTIFIER) {
            IdentifierNode* ident = (IdentifierNode*)arg;
            Variable* var = type_checker_lookup_variable(checker, ident->name);
            
            if (var && var->ownership == OWNERSHIP_OWNED) {
                // TODO: Check function parameter ownership requirements
                // For now, assume owned parameters take ownership
                mark_variable_moved(checker, ident->name, pos);
            }
        }
        
        arg = arg->next;
    }
    
    return 1;
}

// Check if a variable can be safely accessed in current scope
int check_variable_lifetime(TypeChecker* checker, const char* name, Position pos) {
    Variable* var = type_checker_lookup_variable(checker, name);
    if (!var) {
        type_error(checker, pos, "Undefined variable '%s'", name);
        return 0;
    }
    
    // Check if variable has been moved
    if (var->is_moved) {
        type_error(checker, pos,
                  "Use of moved variable '%s' (moved at %s:%d:%d)",
                  name,
                  var->declared_pos.filename ? var->declared_pos.filename : "<unknown>",
                  var->declared_pos.line, var->declared_pos.column);
        return 0;
    }
    
    // Check if variable is initialized
    if (!var->is_initialized) {
        type_error(checker, pos, "Use of uninitialized variable '%s'", name);
        return 0;
    }
    
    return 1;
}

// Enhanced ownership analysis integration
int perform_ownership_analysis(TypeChecker* checker, ASTNode* program) {
    if (!checker || !program) return 0;
    
    // Create flow-sensitive analyzer
    FlowSensitiveAnalyzer* flow_analyzer = flow_analyzer_new(checker);
    if (!flow_analyzer) {
        return 0;
    }
    
    // Create reference manager
    ReferenceManager* ref_manager = reference_manager_new(flow_analyzer);
    if (!ref_manager) {
        flow_analyzer_free(flow_analyzer);
        return 0;
    }
    
    // Create escape analyzer
    EscapeAnalyzer* escape_analyzer = escape_analyzer_new(ref_manager);
    if (!escape_analyzer) {
        reference_manager_free(ref_manager);
        flow_analyzer_free(flow_analyzer);
        return 0;
    }
    
    int result = 1;
    
    // Perform interprocedural escape analysis
    if (!escape_analyzer_analyze_program(escape_analyzer, program)) {
        type_error(checker, (Position){0}, "Escape analysis failed");
        result = 0;
    }
    
    // Integrate results with type checker
    if (result && !integrate_flow_analysis(checker, program)) {
        type_error(checker, (Position){0}, "Flow analysis integration failed");
        result = 0;
    }
    
    // Apply ownership decisions to types
    if (result) {
        apply_ownership_decisions(checker, flow_analyzer);
    }
    
    // Integrate arena allocation based on escape analysis
    if (result) {
        printf("🏟️  Integrating arena allocation system...\n");
        if (integrate_arena_allocation(checker, escape_analyzer, program)) {
            printf("✅ Arena allocation integration complete\n");
        } else {
            printf("⚠️  Arena allocation integration completed with warnings\n");
        }
    }
    
    // Integrate channel runtime and goroutine analysis
    if (result) {
        printf("📡 Integrating channel runtime system...\n");
        if (integrate_channel_runtime(checker, program)) {
            printf("✅ Channel runtime integration complete\n");
        } else {
            printf("⚠️  Channel runtime integration completed with warnings\n");
        }
    }
    
    // Print statistics if successful
    if (result) {
        printf("=== Ownership Analysis Results ===\n");
        escape_analyzer_print_statistics(escape_analyzer);
        flow_analyzer_print_statistics(flow_analyzer);
        reference_manager_print_statistics(ref_manager);
    }
    
    // Cleanup
    escape_analyzer_free(escape_analyzer);
    reference_manager_free(ref_manager);
    flow_analyzer_free(flow_analyzer);
    
    return result;
}

// Determine transfer operation based on ownership analysis
TransferKind determine_transfer_operation(TypeChecker* checker, ASTNode* expr, Variable* var) {
    if (!checker || !expr || !var) return TRANSFER_KIND_COPY;
    
    // Check if variable escapes
    if (var->ownership == OWNERSHIP_OWNED) {
        // Check if this is the last use
        // TODO: Implement more sophisticated last-use analysis
        
        // For now, use simple heuristics
        if (expr->type == AST_RETURN_STMT) {
            return TRANSFER_KIND_MOVE;  // Move when returning
        }
        
        if (expr->type == AST_CALL_EXPR) {
            return TRANSFER_KIND_MOVE;  // Move when passing to function
        }
        
        return TRANSFER_KIND_BORROW_IMMUTABLE;  // Default to borrow
    }
    
    return TRANSFER_KIND_COPY;
}

// Check memory safety with ownership rules
int check_memory_safety(TypeChecker* checker, ASTNode* expr, Position pos) {
    if (!checker || !expr) return 0;
    
    switch (expr->type) {
        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)expr;
            Variable* var = type_checker_lookup_variable(checker, ident->name);
            
            if (!var) {
                type_error(checker, pos, "Undefined variable '%s'", ident->name);
                return 0;
            }
            
            // Check if variable was moved
            if (var->is_moved) {
                type_error(checker, pos,
                          "Use of moved variable '%s' (ownership was transferred)",
                          ident->name);
                return 0;
            }
            
            // Check if variable is initialized
            if (!var->is_initialized) {
                type_error(checker, pos, "Use of uninitialized variable '%s'", ident->name);
                return 0;
            }
            
            return 1;
        }
        
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)expr;
            
            // Special handling for assignment
            if (binary->operator == TOKEN_ASSIGN) {
                if (!check_memory_safety(checker, binary->right, pos)) {
                    return 0;
                }
                
                // Check if we're moving the value
                if (binary->right->type == AST_IDENTIFIER) {
                    IdentifierNode* ident = (IdentifierNode*)binary->right;
                    Variable* var = type_checker_lookup_variable(checker, ident->name);
                    
                    if (var && var->ownership == OWNERSHIP_OWNED) {
                        TransferKind transfer = determine_transfer_operation(checker, expr, var);
                        
                        if (transfer == TRANSFER_KIND_MOVE) {
                            // Mark variable as moved
                            mark_variable_moved(checker, ident->name, pos);
                        }
                    }
                }
                
                return 1;
            }
            
            return check_memory_safety(checker, binary->left, pos) &&
                   check_memory_safety(checker, binary->right, pos);
        }
        
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            
            // Check arguments
            ASTNode* arg = call->args;
            while (arg) {
                if (!check_memory_safety(checker, arg, pos)) {
                    return 0;
                }
                arg = arg->next;
            }
            
            return check_function_call_ownership(checker, call, pos);
        }
        
        case AST_INDEX_EXPR: {
            IndexExprNode* index = (IndexExprNode*)expr;
            
            if (!check_memory_safety(checker, index->expr, pos) ||
                !check_memory_safety(checker, index->index, pos)) {
                return 0;
            }
            
            // Check for null pointer dereference
            Type* expr_type = type_check_expression(checker, index->expr);
            if (expr_type && type_is_nullable(expr_type)) {
                type_error(checker, pos,
                          "Cannot index nullable type without null check");
                return 0;
            }
            
            return 1;
        }
        
        case AST_SELECTOR_EXPR: {
            SelectorExprNode* selector = (SelectorExprNode*)expr;
            
            if (!check_memory_safety(checker, selector->expr, pos)) {
                return 0;
            }
            
            // Check for null pointer dereference
            Type* expr_type = type_check_expression(checker, selector->expr);
            if (expr_type && type_is_nullable(expr_type)) {
                type_error(checker, pos,
                          "Cannot access field of nullable type without null check");
                return 0;
            }
            
            return 1;
        }
        
        default:
            return 1;  // Other expressions assumed safe
    }
}

// Simplified borrow checker - checks basic ownership violations
int check_borrow_rules(TypeChecker* checker, ASTNode* expr, Position pos) {
    // Delegate to the enhanced memory safety checker
    return check_memory_safety(checker, expr, pos);
}