#include "memory_safety.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Core flow-sensitive analysis algorithms

// Main function analysis entry point
int flow_analyze_function(FlowSensitiveAnalyzer* analyzer, ASTNode* function) {
    if (!analyzer || !function) return 0;
    
    // Build control flow graph
    ASTNode* body = NULL;
    if (function->type == AST_FUNC_DECL) {
        FuncDeclNode* func_decl = (FuncDeclNode*)function;
        body = func_decl->body;
    } else {
        flow_error(analyzer, function->pos, "Expected function declaration");
        return 0;
    }
    
    if (!body) return 1;  // Empty function is valid
    
    analyzer->cfg = cfg_build(body);
    if (!analyzer->cfg) {
        flow_error(analyzer, function->pos, "Failed to build control flow graph");
        return 0;
    }
    
    // Initialize block states
    analyzer->block_states = malloc(sizeof(OwnershipState*) * analyzer->cfg->block_count);
    if (!analyzer->block_states) return 0;
    
    for (size_t i = 0; i < analyzer->cfg->block_count; i++) {
        analyzer->block_states[i] = ownership_state_new(NULL);
    }
    
    // Initialize worklist with all blocks
    analyzer->worklist = malloc(sizeof(int) * analyzer->cfg->block_count);
    analyzer->worklist_size = analyzer->cfg->block_count;
    for (size_t i = 0; i < analyzer->cfg->block_count; i++) {
        analyzer->worklist[i] = i;
    }
    
    // Iterative dataflow analysis
    while (analyzer->worklist_size > 0) {
        // Pop block from worklist
        int block_id = analyzer->worklist[--analyzer->worklist_size];
        BasicBlock* block = analyzer->cfg->blocks[block_id];
        
        if (!flow_analyze_block(analyzer, block)) {
            return 0;
        }
    }
    
    // Perform post-analysis optimizations
    if (analyzer->aggressive_optimization) {
        optimize_move_operations(analyzer, function);
    }
    
    // Analyze usage patterns
    analyze_usage_patterns(analyzer, function);
    
    analyzer->total_values_analyzed += analyzer->cfg->block_count;
    return 1;
}

// Analyze a single basic block
int flow_analyze_block(FlowSensitiveAnalyzer* analyzer, BasicBlock* block) {
    if (!analyzer || !block) return 0;
    
    OwnershipState* state = analyzer->block_states[block->id];
    if (!state) return 0;
    
    // Merge state from predecessors
    for (size_t i = 0; i < block->predecessor_count; i++) {
        BasicBlock* pred = block->predecessors[i];
        OwnershipState* pred_state = analyzer->block_states[pred->id];
        if (pred_state) {
            ownership_state_merge(state, pred_state);
        }
    }
    
    // Analyze each statement in the block
    for (size_t i = 0; i < block->statement_count; i++) {
        if (!flow_analyze_statement(analyzer, block->statements[i], state)) {
            return 0;
        }
    }
    
    // Update exit state
    if (block->exit_state) {
        ownership_state_free(block->exit_state);
    }
    block->exit_state = ownership_state_copy(state);
    
    return 1;
}

// Analyze a single statement
int flow_analyze_statement(FlowSensitiveAnalyzer* analyzer, ASTNode* stmt, OwnershipState* state) {
    if (!analyzer || !stmt || !state) return 0;
    
    switch (stmt->type) {
        case AST_VAR_DECL: {
            VarDeclNode* var_decl = (VarDeclNode*)stmt;
            
            // Track new variables
            if (var_decl->names && var_decl->name_count > 0) {
                for (size_t i = 0; i < var_decl->name_count; i++) {
                    if (var_decl->names[i]) {
                        ValueState* new_value = value_state_new(var_decl->names[i], VALUE_STATE_UNINITIALIZED);
                        if (new_value) {
                            ownership_state_add_value(state, new_value);
                        }
                    }
                }
            }
            
            // Analyze initializer if present
            if (var_decl->values) {
                return flow_analyze_expression(analyzer, var_decl->values, state);
            }
            
            return 1;
        }
        
        case AST_EXPR_STMT: {
            ExprStmtNode* expr_stmt = (ExprStmtNode*)stmt;
            return flow_analyze_expression(analyzer, expr_stmt->expr, state);
        }
        
        case AST_RETURN_STMT: {
            ReturnStmtNode* ret_stmt = (ReturnStmtNode*)stmt;
            
            if (ret_stmt->values) {
                // Analyze returned expressions
                if (!flow_analyze_expression(analyzer, ret_stmt->values, state)) {
                    return 0;
                }
                
                // Mark returned values as potentially moved
                if (ret_stmt->values->type == AST_IDENTIFIER) {
                    IdentifierNode* ident = (IdentifierNode*)ret_stmt->values;
                    ValueState* value = ownership_state_lookup(state, ident->name);
                    if (value) {
                        // Infer transfer operation for return
                        TransferKind transfer = infer_transfer_operation(analyzer, ident->name, 
                                                                       ret_stmt->values, state);
                        if (transfer == TRANSFER_KIND_MOVE) {
                            value->state = VALUE_STATE_MOVED;
                            value->escape_status = ESCAPE_FUNCTION;
                            analyzer->moves_inferred++;
                        } else if (transfer == TRANSFER_KIND_COPY) {
                            analyzer->copies_required++;
                        }
                    }
                }
            }
            
            return 1;
        }
        
        case AST_IF_STMT: {
            IfStmtNode* if_stmt = (IfStmtNode*)stmt;
            
            // Analyze condition
            if (!flow_analyze_expression(analyzer, if_stmt->condition, state)) {
                return 0;
            }
            
            // Handle conditional moves
            return analyze_conditional_moves(analyzer, (ASTNode*)if_stmt, state);
        }
        
        case AST_FOR_STMT: {
            ForStmtNode* for_stmt = (ForStmtNode*)stmt;
            
            // Analyze init, condition, post
            if (for_stmt->init && !flow_analyze_statement(analyzer, for_stmt->init, state)) {
                return 0;
            }
            if (for_stmt->condition && !flow_analyze_expression(analyzer, for_stmt->condition, state)) {
                return 0;
            }
            if (for_stmt->post && !flow_analyze_statement(analyzer, for_stmt->post, state)) {
                return 0;
            }
            
            // Analyze body - may need multiple iterations for convergence
            if (for_stmt->body) {
                return flow_analyze_statement(analyzer, for_stmt->body, state);
            }
            
            return 1;
        }
        
        case AST_BLOCK_STMT: {
            BlockStmtNode* block = (BlockStmtNode*)stmt;
            ASTNode* current = block->statements;
            
            while (current) {
                if (!flow_analyze_statement(analyzer, current, state)) {
                    return 0;
                }
                current = current->next;
            }
            
            return 1;
        }
        
        default:
            // For unhandled statement types, assume they're safe
            return 1;
    }
}

// Analyze an expression
int flow_analyze_expression(FlowSensitiveAnalyzer* analyzer, ASTNode* expr, OwnershipState* state) {
    if (!analyzer || !expr || !state) return 0;
    
    switch (expr->type) {
        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)expr;
            ValueState* value = ownership_state_lookup(state, ident->name);
            
            if (!value) {
                // Variable not found in current state - might be a parameter or global
                value = value_state_new(ident->name, VALUE_STATE_INITIALIZED);
                if (value) {
                    ownership_state_add_value(state, value);
                }
            }
            
            // Check if variable is in a valid state for use
            if (value->state == VALUE_STATE_MOVED) {
                flow_error(analyzer, expr->pos, "Use of moved variable '%s'", ident->name);
                analyzer->unsafe_patterns_found++;
                return 0;
            }
            
            if (value->state == VALUE_STATE_UNINITIALIZED) {
                flow_error(analyzer, expr->pos, "Use of uninitialized variable '%s'", ident->name);
                analyzer->unsafe_patterns_found++;
                return 0;
            }
            
            // Update last use position
            value->last_use_position = expr->pos.line * 1000 + expr->pos.column;
            
            return 1;
        }
        
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)expr;
            
            // Handle assignment specially
            if (binary->operator == TOKEN_ASSIGN) {
                // Analyze right-hand side first
                if (!flow_analyze_expression(analyzer, binary->right, state)) {
                    return 0;
                }
                
                // Handle assignment to left-hand side
                if (binary->left->type == AST_IDENTIFIER) {
                    IdentifierNode* ident = (IdentifierNode*)binary->left;
                    
                    // Infer transfer operation
                    TransferKind transfer = infer_transfer_operation(analyzer, ident->name, 
                                                                   binary->right, state);
                    
                    // Update state based on transfer
                    if (binary->right->type == AST_IDENTIFIER) {
                        IdentifierNode* rhs_ident = (IdentifierNode*)binary->right;
                        ValueState* rhs_value = ownership_state_lookup(state, rhs_ident->name);
                        
                        if (rhs_value && transfer == TRANSFER_KIND_MOVE) {
                            rhs_value->state = VALUE_STATE_MOVED;
                            analyzer->moves_inferred++;
                        } else if (transfer == TRANSFER_KIND_COPY) {
                            analyzer->copies_required++;
                        } else if (transfer == TRANSFER_KIND_BORROW_IMMUTABLE || 
                                 transfer == TRANSFER_KIND_BORROW_MUTABLE) {
                            if (rhs_value) {
                                rhs_value->ref_count++;
                            }
                            analyzer->borrows_inferred++;
                        }
                    }
                    
                    // Update or create left-hand side value state
                    ownership_state_update(state, ident->name, VALUE_STATE_INITIALIZED);
                }
                
                return 1;
            }
            
            // For other binary operations, analyze both operands
            return flow_analyze_expression(analyzer, binary->left, state) &&
                   flow_analyze_expression(analyzer, binary->right, state);
        }
        
        case AST_UNARY_EXPR: {
            UnaryExprNode* unary = (UnaryExprNode*)expr;
            
            // Handle address-of operator (borrowing)
            if (unary->operator == TOKEN_BIT_AND) {
                if (unary->operand->type == AST_IDENTIFIER) {
                    IdentifierNode* ident = (IdentifierNode*)unary->operand;
                    ValueState* value = ownership_state_lookup(state, ident->name);
                    if (value) {
                        value->ref_count++;
                        value->state = VALUE_STATE_BORROWED_IMMUTABLE;
                        analyzer->borrows_inferred++;
                    }
                }
            }
            
            return flow_analyze_expression(analyzer, unary->operand, state);
        }
        
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            
            // Analyze function expression
            if (!flow_analyze_expression(analyzer, call->function, state)) {
                return 0;
            }
            
            // Analyze arguments - may involve moves or borrows
            ASTNode* arg = call->args;
            while (arg) {
                if (!flow_analyze_expression(analyzer, arg, state)) {
                    return 0;
                }
                
                // Determine if argument is moved to function
                if (arg->type == AST_IDENTIFIER) {
                    IdentifierNode* ident = (IdentifierNode*)arg;
                    ValueState* value = ownership_state_lookup(state, ident->name);
                    
                    if (value) {
                        // For now, assume arguments are moved unless they're primitive types
                        // This is a simplification - real implementation would check function signatures
                        TransferKind transfer = infer_transfer_operation(analyzer, ident->name, arg, state);
                        
                        if (transfer == TRANSFER_KIND_MOVE) {
                            value->state = VALUE_STATE_MOVED;
                            analyzer->moves_inferred++;
                        } else if (transfer == TRANSFER_KIND_BORROW_IMMUTABLE || 
                                 transfer == TRANSFER_KIND_BORROW_MUTABLE) {
                            value->ref_count++;
                            analyzer->borrows_inferred++;
                        }
                    }
                }
                
                arg = arg->next;
            }
            
            return 1;
        }
        
        case AST_INDEX_EXPR: {
            IndexExprNode* index = (IndexExprNode*)expr;
            return flow_analyze_expression(analyzer, index->expr, state) &&
                   flow_analyze_expression(analyzer, index->index, state);
        }
        
        case AST_SELECTOR_EXPR: {
            SelectorExprNode* selector = (SelectorExprNode*)expr;
            return flow_analyze_expression(analyzer, selector->expr, state);
        }
        
        case AST_LITERAL:
            // Literals don't affect ownership state
            return 1;
            
        default:
            // For unhandled expression types, assume they're safe
            return 1;
    }
}

// Infer the best transfer operation for a given context
TransferKind infer_transfer_operation(FlowSensitiveAnalyzer* analyzer, 
                                     const char* var_name, 
                                     ASTNode* usage_context,
                                     OwnershipState* state) {
    if (!analyzer || !var_name || !usage_context || !state) {
        return TRANSFER_KIND_AUTO_INFERRED;
    }
    
    ValueState* value = ownership_state_lookup(state, var_name);
    if (!value) {
        return TRANSFER_KIND_AUTO_INFERRED;
    }
    
    // If already moved, can't do anything
    if (value->state == VALUE_STATE_MOVED) {
        return TRANSFER_KIND_AUTO_INFERRED;  // Error case
    }
    
    // Check if this is the last use of the variable
    size_t current_position = usage_context->pos.line * 1000 + usage_context->pos.column;
    
    // Simple heuristic: if no future uses detected, prefer move
    if (!has_future_uses(analyzer, var_name, current_position, NULL)) {
        return TRANSFER_KIND_MOVE;
    }
    
    // If variable is borrowed, can only borrow
    if (value->ref_count > 0) {
        return TRANSFER_KIND_BORROW_IMMUTABLE;
    }
    
    // Check escape analysis
    if (value->escape_status == ESCAPE_FUNCTION || value->escape_status == ESCAPE_THREAD) {
        return TRANSFER_KIND_MOVE;
    }
    
    // Default to copy for safety
    return TRANSFER_KIND_COPY;
}

// Simple future use analysis
int has_future_uses(FlowSensitiveAnalyzer* analyzer, const char* var_name, 
                   size_t position __attribute__((unused)), BasicBlock* current_block __attribute__((unused))) {
    if (!analyzer || !var_name) return 0;
    
    // This is a simplified implementation
    // Real implementation would do proper reaching definitions analysis
    
    // For now, assume variables are used again unless proven otherwise
    return 1;
}

// Analyze conditional moves (if statements)
int analyze_conditional_moves(FlowSensitiveAnalyzer* analyzer, ASTNode* if_stmt, 
                             OwnershipState* state) {
    if (!analyzer || !if_stmt || !state) return 0;
    
    IfStmtNode* if_node = (IfStmtNode*)if_stmt;
    
    // Create copies of state for then and else branches
    OwnershipState* then_state = ownership_state_copy(state);
    OwnershipState* else_state = ownership_state_copy(state);
    
    if (!then_state || !else_state) {
        ownership_state_free(then_state);
        ownership_state_free(else_state);
        return 0;
    }
    
    // Analyze then branch
    int then_result = 1;
    if (if_node->then_stmt) {
        then_result = flow_analyze_statement(analyzer, if_node->then_stmt, then_state);
    }
    
    // Analyze else branch
    int else_result = 1;
    if (if_node->else_stmt) {
        else_result = flow_analyze_statement(analyzer, if_node->else_stmt, else_state);
    }
    
    // Merge the states back into the main state
    if (then_result && else_result) {
        merge_conditional_states(then_state, else_state, state);
    }
    
    ownership_state_free(then_state);
    ownership_state_free(else_state);
    
    return then_result && else_result;
}

// Merge states from conditional branches
int merge_conditional_states(OwnershipState* then_state, OwnershipState* else_state, 
                            OwnershipState* result_state) {
    if (!then_state || !else_state || !result_state) return 0;
    
    // Clear the result state first
    for (size_t i = 0; i < result_state->value_count; i++) {
        value_state_free(result_state->values[i]);
    }
    result_state->value_count = 0;
    
    // Merge all values from both branches
    for (size_t i = 0; i < then_state->value_count; i++) {
        ValueState* then_value = then_state->values[i];
        ValueState* else_value = ownership_state_lookup(else_state, then_value->name);
        
        ValueState* merged_value = value_state_copy(then_value);
        if (merged_value) {
            if (else_value) {
                value_state_merge(merged_value, else_value);
            }
            ownership_state_add_value(result_state, merged_value);
        }
    }
    
    // Add values that only exist in else branch
    for (size_t i = 0; i < else_state->value_count; i++) {
        ValueState* else_value = else_state->values[i];
        if (!ownership_state_lookup(result_state, else_value->name)) {
            ValueState* new_value = value_state_copy(else_value);
            if (new_value) {
                ownership_state_add_value(result_state, new_value);
            }
        }
    }
    
    return 1;
}

// Move operation optimization
int optimize_move_operations(FlowSensitiveAnalyzer* analyzer, ASTNode* function) {
    if (!analyzer || !function) return 0;
    
    // This is a placeholder for move optimization
    // Real implementation would perform sophisticated analysis to maximize moves
    
    return 1;
}

// Usage pattern analysis
int analyze_usage_patterns(FlowSensitiveAnalyzer* analyzer, ASTNode* function) {
    if (!analyzer || !function) return 0;
    
    // Analyze each value's usage pattern
    for (size_t i = 0; i < analyzer->cfg->block_count; i++) {
        OwnershipState* state = analyzer->block_states[i];
        if (!state) continue;
        
        for (size_t j = 0; j < state->value_count; j++) {
            ValueState* value = state->values[j];
            
            // Detect common patterns
            detect_copy_patterns(analyzer, value->name, state);
            detect_move_patterns(analyzer, value->name, state);
            detect_borrow_patterns(analyzer, value->name, state);
        }
    }
    
    return 1;
}

// Pattern detection functions (simplified implementations)
int detect_copy_patterns(FlowSensitiveAnalyzer* analyzer, const char* var_name, OwnershipState* state) {
    if (!analyzer || !var_name || !state) return 0;
    
    ValueState* value = ownership_state_lookup(state, var_name);
    if (!value) return 0;
    
    // Detect if variable is frequently copied
    if (value->ref_count > 3) {  // Heuristic threshold
        flow_warning(analyzer, (Position){0}, 
                    "Variable '%s' is copied frequently, consider using references", var_name);
    }
    
    return 1;
}

int detect_move_patterns(FlowSensitiveAnalyzer* analyzer, const char* var_name, OwnershipState* state) {
    if (!analyzer || !var_name || !state) return 0;
    
    ValueState* value = ownership_state_lookup(state, var_name);
    if (!value) return 0;
    
    // Detect if variable could be moved instead of copied
    if (value->escape_status == ESCAPE_FUNCTION && value->ref_count == 0) {
        if (value->recommended_transfer == TRANSFER_KIND_COPY) {
            value->recommended_transfer = TRANSFER_KIND_MOVE;
            flow_warning(analyzer, (Position){0}, 
                        "Variable '%s' could be moved instead of copied", var_name);
        }
    }
    
    return 1;
}

int detect_borrow_patterns(FlowSensitiveAnalyzer* analyzer, const char* var_name, OwnershipState* state) {
    if (!analyzer || !var_name || !state) return 0;
    
    ValueState* value = ownership_state_lookup(state, var_name);
    if (!value) return 0;
    
    // Detect if variable is only read (could use immutable borrow)
    if (value->state == VALUE_STATE_INITIALIZED && value->ref_count == 0) {
        value->recommended_transfer = TRANSFER_KIND_BORROW_IMMUTABLE;
    }
    
    return 1;
}

// Integration with type checker
int integrate_flow_analysis(TypeChecker* type_checker, ASTNode* function) {
    if (!type_checker || !function) return 0;
    
    FlowSensitiveAnalyzer* analyzer = flow_analyzer_new(type_checker);
    if (!analyzer) return 0;
    
    int result = flow_analyze_function(analyzer, function);
    
    if (result && analyzer->debug_mode) {
        flow_analyzer_print_statistics(analyzer);
    }
    
    // Apply decisions to type checker
    apply_ownership_decisions(type_checker, analyzer);
    
    flow_analyzer_free(analyzer);
    return result;
}

void apply_ownership_decisions(TypeChecker* type_checker, FlowSensitiveAnalyzer* analyzer) {
    if (!type_checker || !analyzer) return;
    
    // This would integrate the analysis results back into the type checker
    // For now, this is a placeholder
    
    // Real implementation would:
    // 1. Update variable ownership information in type checker
    // 2. Insert move/copy operations in the AST
    // 3. Generate appropriate LLVM IR for optimized memory operations
}
