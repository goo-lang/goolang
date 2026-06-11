#include "comptime.h"
#include "types.h"
#include "ast.h"
#include "token.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>

static ComptimeResult* comptime_eval_binary_expr(ComptimeContext* ctx, ASTNode* expr);
static ComptimeResult* comptime_eval_unary_expr(ComptimeContext* ctx, ASTNode* expr);
static ComptimeResult* comptime_eval_identifier(ComptimeContext* ctx, ASTNode* expr);
// Non-static — also called from the bridge (comptime_types.c, type_checker glue).
// Forward-declared here because the block walker at line ~1085 calls it before
// its definition further down the file.
ComptimeResult* comptime_eval_statement_enhanced(ComptimeContext* ctx, ASTNode* stmt);
ComptimeResult* comptime_eval_function_call_enhanced(ComptimeContext* ctx, ASTNode* call);

// Evaluate an expression at compile time
ComptimeResult* comptime_eval_expression(ComptimeContext* ctx, ASTNode* expr) {
    if (!ctx || !expr) {
        return comptime_result_new(NULL, comptime_error_new("Invalid context or expression", (Position){0}), NULL);
    }
    
    switch (expr->type) {
        case AST_LITERAL: {
            ComptimeValue* value = comptime_value_from_literal(expr);
            return comptime_result_new(value, NULL, NULL);
        }
        
        case AST_IDENTIFIER: {
            return comptime_eval_identifier(ctx, expr);
        }
        
        case AST_BINARY_EXPR: {
            return comptime_eval_binary_expr(ctx, expr);
        }
        
        case AST_UNARY_EXPR: {
            return comptime_eval_unary_expr(ctx, expr);
        }
        
        case AST_CALL_EXPR: {
            // Route through _enhanced so user-defined function calls work.
            // The original handler still has a TODO at line ~867 that
            // short-circuits with "User-defined function calls not yet
            // implemented"; _enhanced dispatches to comptime_call_user_function.
            return comptime_eval_function_call_enhanced(ctx, expr);
        }
        
        default: {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "Unsupported expression type in comptime evaluation: %d", expr->type);
            return comptime_result_new(NULL, comptime_error_new(error_msg, expr->pos), NULL);
        }
    }
}

// Evaluate an identifier
static ComptimeResult* comptime_eval_identifier(ComptimeContext* ctx, ASTNode* expr) {
    IdentifierNode* ident = (IdentifierNode*)expr;
    ComptimeValue* value = comptime_context_lookup_var(ctx, ident->name);
    
    if (!value) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Undefined variable in comptime context: %s", ident->name);
        return comptime_result_new(NULL, comptime_error_new(error_msg, expr->pos), NULL);
    }
    
    return comptime_result_new(comptime_value_copy(value), NULL, NULL);
}

// Evaluate a binary expression
static ComptimeResult* comptime_eval_binary_expr(ComptimeContext* ctx, ASTNode* expr) {
    BinaryExprNode* binary = (BinaryExprNode*)expr;
    
    // Evaluate left operand
    ComptimeResult* left_result = comptime_eval_expression(ctx, binary->left);
    if (left_result->error) {
        return left_result;
    }
    
    // Evaluate right operand
    ComptimeResult* right_result = comptime_eval_expression(ctx, binary->right);
    if (right_result->error) {
        comptime_result_free(left_result);
        return right_result;
    }
    
    ComptimeValue* left_val = left_result->value;
    ComptimeValue* right_val = right_result->value;
    ComptimeValue* result_val = NULL;
    
    // Perform the operation based on operator type
    switch (binary->operator) {
        case TOKEN_PLUS:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_int(left_val->int_value + right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_float(left_f + right_f);
            } else if (left_val->type == COMPTIME_VALUE_STRING || right_val->type == COMPTIME_VALUE_STRING) {
                // String concatenation
                char* left_str = comptime_value_to_string(left_val);
                char* right_str = comptime_value_to_string(right_val);
                char* concat = malloc(strlen(left_str) + strlen(right_str) + 1);
                strcpy(concat, left_str);
                strcat(concat, right_str);
                result_val = comptime_value_from_string(concat);
                free(left_str);
                free(right_str);
                free(concat);
            }
            break;
            
        case TOKEN_MINUS:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_int(left_val->int_value - right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_float(left_f - right_f);
            }
            break;
            
        case TOKEN_MULTIPLY:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_int(left_val->int_value * right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_float(left_f * right_f);
            }
            break;
            
        case TOKEN_DIVIDE:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                if (right_val->int_value == 0) {
                    comptime_result_free(left_result);
                    comptime_result_free(right_result);
                    return comptime_result_new(NULL, comptime_error_new("Division by zero in comptime evaluation", expr->pos), NULL);
                }
                result_val = comptime_value_from_int(left_val->int_value / right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                if (right_f == 0.0) {
                    comptime_result_free(left_result);
                    comptime_result_free(right_result);
                    return comptime_result_new(NULL, comptime_error_new("Division by zero in comptime evaluation", expr->pos), NULL);
                }
                result_val = comptime_value_from_float(left_f / right_f);
            }
            break;
            
        case TOKEN_EQ:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_bool(left_val->int_value == right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_bool(left_f == right_f);
            } else if (left_val->type == COMPTIME_VALUE_BOOL && right_val->type == COMPTIME_VALUE_BOOL) {
                result_val = comptime_value_from_bool(left_val->bool_value == right_val->bool_value);
            }
            break;
            
        case TOKEN_NE:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_bool(left_val->int_value != right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_bool(left_f != right_f);
            } else if (left_val->type == COMPTIME_VALUE_BOOL && right_val->type == COMPTIME_VALUE_BOOL) {
                result_val = comptime_value_from_bool(left_val->bool_value != right_val->bool_value);
            }
            break;
            
        case TOKEN_LT:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_bool(left_val->int_value < right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_bool(left_f < right_f);
            }
            break;
            
        case TOKEN_LE:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_bool(left_val->int_value <= right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_bool(left_f <= right_f);
            }
            break;
            
        case TOKEN_GT:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_bool(left_val->int_value > right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_bool(left_f > right_f);
            }
            break;
            
        case TOKEN_GE:
            if (left_val->type == COMPTIME_VALUE_INT && right_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_bool(left_val->int_value >= right_val->int_value);
            } else if (left_val->type == COMPTIME_VALUE_FLOAT || right_val->type == COMPTIME_VALUE_FLOAT) {
                double left_f = (left_val->type == COMPTIME_VALUE_FLOAT) ? left_val->float_value : (double)left_val->int_value;
                double right_f = (right_val->type == COMPTIME_VALUE_FLOAT) ? right_val->float_value : (double)right_val->int_value;
                result_val = comptime_value_from_bool(left_f >= right_f);
            }
            break;
            
        case TOKEN_AND:
            result_val = comptime_value_from_bool(comptime_value_is_truthy(left_val) && comptime_value_is_truthy(right_val));
            break;
            
        case TOKEN_OR:
            result_val = comptime_value_from_bool(comptime_value_is_truthy(left_val) || comptime_value_is_truthy(right_val));
            break;
            
        default: {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "Unsupported binary operator in comptime evaluation: %d", binary->operator);
            comptime_result_free(left_result);
            comptime_result_free(right_result);
            return comptime_result_new(NULL, comptime_error_new(error_msg, expr->pos), NULL);
        }
    }
    
    comptime_result_free(left_result);
    comptime_result_free(right_result);
    
    if (!result_val) {
        return comptime_result_new(NULL, comptime_error_new("Type error in binary operation", expr->pos), NULL);
    }
    
    return comptime_result_new(result_val, NULL, NULL);
}

// Evaluate a unary expression
static ComptimeResult* comptime_eval_unary_expr(ComptimeContext* ctx, ASTNode* expr) {
    UnaryExprNode* unary = (UnaryExprNode*)expr;
    
    ComptimeResult* operand_result = comptime_eval_expression(ctx, unary->operand);
    if (operand_result->error) {
        return operand_result;
    }
    
    ComptimeValue* operand_val = operand_result->value;
    ComptimeValue* result_val = NULL;
    
    switch (unary->operator) {
        case TOKEN_MINUS:
            if (operand_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_int(-operand_val->int_value);
            } else if (operand_val->type == COMPTIME_VALUE_FLOAT) {
                result_val = comptime_value_from_float(-operand_val->float_value);
            }
            break;
            
        case TOKEN_PLUS:
            if (operand_val->type == COMPTIME_VALUE_INT) {
                result_val = comptime_value_from_int(operand_val->int_value);
            } else if (operand_val->type == COMPTIME_VALUE_FLOAT) {
                result_val = comptime_value_from_float(operand_val->float_value);
            }
            break;
            
        case TOKEN_NOT:
            result_val = comptime_value_from_bool(!comptime_value_is_truthy(operand_val));
            break;
            
        default: {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "Unsupported unary operator in comptime evaluation: %d", unary->operator);
            comptime_result_free(operand_result);
            return comptime_result_new(NULL, comptime_error_new(error_msg, expr->pos), NULL);
        }
    }
    
    comptime_result_free(operand_result);
    
    if (!result_val) {
        return comptime_result_new(NULL, comptime_error_new("Type error in unary operation", expr->pos), NULL);
    }
    
    return comptime_result_new(result_val, NULL, NULL);
}

// Evaluate a function call
ComptimeResult* comptime_eval_function_call(ComptimeContext* ctx, ASTNode* call) {
    if (!ctx || !call || call->type != AST_CALL_EXPR) {
        return comptime_result_new(NULL, comptime_error_new("Invalid function call", (Position){0}), NULL);
    }
    
    CallExprNode* call_node = (CallExprNode*)call;
    
    // Check if it's a built-in intrinsic function (like @emit)
    if (call_node->function && call_node->function->type == AST_IDENTIFIER) {
        IdentifierNode* func_ident = (IdentifierNode*)call_node->function;
        
        if (strcmp(func_ident->name, "@emit") == 0) {
            // Handle @emit intrinsic
            // For now, assume args is a single argument node
            if (!call_node->args) {
                return comptime_result_new(NULL, comptime_error_new("@emit requires exactly one argument", call->pos), NULL);
            }
            
            ComptimeResult* arg_result = comptime_eval_expression(ctx, call_node->args);
            if (arg_result->error) {
                return arg_result;
            }
            
            return comptime_intrinsic_emit(ctx, arg_result->value);
        }
        
        if (strcmp(func_ident->name, "@typeof") == 0) {
            // Handle @typeof intrinsic
            if (!call_node->args) {
                return comptime_result_new(NULL, comptime_error_new("@typeof requires exactly one argument", call->pos), NULL);
            }
            
            ComptimeResult* arg_result = comptime_eval_expression(ctx, call_node->args);
            if (arg_result->error) {
                return arg_result;
            }
            
            return comptime_intrinsic_typeof(ctx, arg_result->value);
        }
        
        if (strcmp(func_ident->name, "@sizeof") == 0) {
            // Handle @sizeof intrinsic
            if (!call_node->args) {
                return comptime_result_new(NULL, comptime_error_new("@sizeof requires exactly one argument", call->pos), NULL);
            }
            
            ComptimeResult* arg_result = comptime_eval_expression(ctx, call_node->args);
            if (arg_result->error) {
                return arg_result;
            }
            
            return comptime_intrinsic_sizeof(ctx, arg_result->value);
        }

        // Non-intrinsic names land in the fallback below. User-defined
        // function calls dispatch through comptime_eval_function_call_enhanced
        // (see comptime_eval_expression's AST_CALL_EXPR arm), so this branch
        // only fires if the original handler is invoked directly with a
        // non-@-prefixed name — an API misuse rather than a comptime error.
    }
    
    return comptime_result_new(NULL, comptime_error_new("Complex function calls not supported in comptime evaluation", call->pos), NULL);
}

// Evaluate a statement at compile time
ComptimeResult* comptime_eval_statement(ComptimeContext* ctx, ASTNode* stmt) {
    if (!ctx || !stmt) {
        return comptime_result_new(NULL, comptime_error_new("Invalid context or statement", (Position){0}), NULL);
    }
    
    switch (stmt->type) {
        case AST_EXPR_STMT: {
            ExprStmtNode* expr_stmt = (ExprStmtNode*)stmt;
            return comptime_eval_expression(ctx, expr_stmt->expr);
        }
        
        case AST_CONST_DECL: {
            ConstDeclNode* const_decl = (ConstDeclNode*)stmt;
            
            // For simplicity, handle single constant declaration
            if (const_decl->name_count != 1) {
                return comptime_result_new(NULL, comptime_error_new("Multiple constants not supported in comptime", stmt->pos), NULL);
            }
            
            // Evaluate the initializer expression
            ComptimeResult* init_result = comptime_eval_expression(ctx, const_decl->values);
            if (init_result->error) {
                return init_result;
            }
            
            // Bind the constant in the context
            if (!comptime_context_bind_var(ctx, const_decl->names[0], comptime_value_copy(init_result->value))) {
                comptime_result_free(init_result);
                return comptime_result_new(NULL, comptime_error_new("Failed to bind constant", stmt->pos), NULL);
            }
            
            return init_result;
        }
        
        case AST_VAR_DECL: {
            VarDeclNode* var_decl = (VarDeclNode*)stmt;
            
            // For simplicity, handle single variable declaration
            if (var_decl->name_count != 1) {
                return comptime_result_new(NULL, comptime_error_new("Multiple variables not supported in comptime", stmt->pos), NULL);
            }
            
            ComptimeValue* init_val = NULL;
            if (var_decl->values) {
                ComptimeResult* init_result = comptime_eval_expression(ctx, var_decl->values);
                if (init_result->error) {
                    return init_result;
                }
                init_val = init_result->value;
                init_result->value = NULL; // Transfer ownership
                comptime_result_free(init_result);
            } else {
                init_val = comptime_value_new(COMPTIME_VALUE_UNDEFINED);
            }
            
            // Bind the variable in the context
            if (!comptime_context_bind_var(ctx, var_decl->names[0], init_val)) {
                comptime_value_free(init_val);
                return comptime_result_new(NULL, comptime_error_new("Failed to bind variable", stmt->pos), NULL);
            }
            
            return comptime_result_new(comptime_value_copy(init_val), NULL, NULL);
        }
        
        default: {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "Unsupported statement type in comptime evaluation: %d", stmt->type);
            return comptime_result_new(NULL, comptime_error_new(error_msg, stmt->pos), NULL);
        }
    }
}

// Evaluate a block at compile time
ComptimeResult* comptime_eval_block(ComptimeContext* ctx, ASTNode* block) {
    if (!ctx || !block || block->type != AST_BLOCK_STMT) {
        return comptime_result_new(NULL, comptime_error_new("Invalid context or block", (Position){0}), NULL);
    }
    
    BlockStmtNode* block_node = (BlockStmtNode*)block;
    ComptimeResult* last_result = NULL;
    
    // Create a new context for the block scope
    ComptimeContext* block_ctx = comptime_context_new(ctx);
    if (!block_ctx) {
        return comptime_result_new(NULL, comptime_error_new("Failed to create block context", block->pos), NULL);
    }
    
    // Walk the statement linked list. Dispatch via _enhanced so IF/FOR/RETURN
    // statements are evaluated (the non-enhanced switch only knows EXPR/CONST/VAR).
    // Stop and propagate immediately on error or on a return-statement result so
    // the function's return value isn't overwritten by a trailing statement.
    for (ASTNode* stmt = block_node->statements; stmt; stmt = stmt->next) {
        comptime_result_free(last_result);
        last_result = comptime_eval_statement_enhanced(block_ctx, stmt);

        if (!last_result) break;
        if (last_result->error || last_result->is_return) {
            comptime_context_free(block_ctx);
            return last_result;
        }
    }
    
    // Copy generated code from block context to parent
    if (block_ctx->generated_code) {
        // TODO: Merge generated code properly
    }
    
    comptime_context_free(block_ctx);
    
    if (!last_result) {
        last_result = comptime_result_new(comptime_value_new(COMPTIME_VALUE_NULL), NULL, NULL);
    }
    
    return last_result;
}

// Main entry point for compile-time execution
ComptimeResult* execute_comptime_block(ASTNode* comptime_block, ComptimeContext* global_ctx) {
    if (!comptime_block || comptime_block->type != AST_COMPTIME_BLOCK) {
        return comptime_result_new(NULL, comptime_error_new("Invalid comptime block", (Position){0}), NULL);
    }
    
    ComptimeBlockNode* block = (ComptimeBlockNode*)comptime_block;
    
    // Create execution context if not provided
    ComptimeContext* ctx = global_ctx;
    bool created_context = false;
    if (!ctx) {
        ctx = comptime_context_new(NULL);
        if (!ctx) {
            return comptime_result_new(NULL, comptime_error_new("Failed to create comptime context", comptime_block->pos), NULL);
        }
        created_context = true;
    }
    
    // Execute the block body
    ComptimeResult* result = comptime_eval_block(ctx, block->body);
    
    // Copy generated code from context to result
    if (ctx->generated_code && !result->generated_code) {
        result->generated_code = strdup(ctx->generated_code);
    }
    
    if (created_context) {
        comptime_context_free(ctx);
    }
    
    return result;
}

// Forward declarations for control flow
static ComptimeResult* comptime_eval_if_stmt(ComptimeContext* ctx, ASTNode* stmt);
static ComptimeResult* comptime_eval_for_stmt(ComptimeContext* ctx, ASTNode* stmt);
static ComptimeResult* comptime_eval_return_stmt(ComptimeContext* ctx, ASTNode* stmt);

// Execute a user-defined function
ComptimeResult* comptime_call_user_function(ComptimeContext* ctx, ASTNode* func_node, 
                                           ComptimeValue** args, size_t arg_count) {
    if (!ctx || !func_node || func_node->type != AST_FUNC_DECL) {
        return comptime_result_new(NULL, comptime_error_new("Invalid function for comptime call", (Position){0}), NULL);
    }
    
    FuncDeclNode* func = (FuncDeclNode*)func_node;
    
    // Check recursion depth
    if (ctx->current_recursion_depth >= ctx->max_recursion_depth) {
        return comptime_result_new(NULL, comptime_error_new("Maximum recursion depth exceeded in comptime function", func_node->pos), NULL);
    }
    
    // Create new context for function scope
    ComptimeContext* func_ctx = comptime_context_new(ctx);
    if (!func_ctx) {
        return comptime_result_new(NULL, comptime_error_new("Failed to create function context", func_node->pos), NULL);
    }
    
    func_ctx->current_recursion_depth = ctx->current_recursion_depth + 1;
    
    // Bind arguments to parameters using the parameters' real names from
    // func->params. Each param decl is an AST_VAR_DECL with names[]/name_count,
    // so a single decl may declare several params (e.g. `a, b int`). Walk both
    // axes in lockstep with the flat args[] array.
    size_t arg_idx = 0;
    for (ASTNode* p = func->params; p && arg_idx < arg_count; p = p->next) {
        if (p->type != AST_VAR_DECL) continue;
        VarDeclNode* pd = (VarDeclNode*)p;
        for (size_t n = 0; n < pd->name_count && arg_idx < arg_count; n++) {
            if (!comptime_context_bind_var(func_ctx, pd->names[n], comptime_value_copy(args[arg_idx]))) {
                comptime_context_free(func_ctx);
                return comptime_result_new(NULL, comptime_error_new("Failed to bind function parameter", func_node->pos), NULL);
            }
            arg_idx++;
        }
    }
    
    // Execute function body
    ComptimeResult* result = comptime_eval_block(func_ctx, func->body);
    
    // Copy generated code from function context
    if (func_ctx->generated_code && result && !result->generated_code) {
        result->generated_code = strdup(func_ctx->generated_code);
    }
    
    comptime_context_free(func_ctx);
    return result;
}

// Enhanced function call evaluation with user-defined functions
ComptimeResult* comptime_eval_function_call_enhanced(ComptimeContext* ctx, ASTNode* call) {
    if (!ctx || !call || call->type != AST_CALL_EXPR) {
        return comptime_result_new(NULL, comptime_error_new("Invalid function call", (Position){0}), NULL);
    }
    
    CallExprNode* call_node = (CallExprNode*)call;
    
    // Check if it's a built-in intrinsic function
    if (call_node->function && call_node->function->type == AST_IDENTIFIER) {
        IdentifierNode* func_ident = (IdentifierNode*)call_node->function;
        
        // Handle built-in intrinsics first
        if (func_ident->name[0] == '@') {
            // This is handled in the original function
            return comptime_eval_function_call(ctx, call);
        }
        
        // Look up user-defined function
        ASTNode* func_node = comptime_context_lookup_func(ctx, func_ident->name);
        if (func_node) {
            // Evaluate each argument in the call site's context, threading the
            // resulting ComptimeValue* into a flat args[] array for the callee.
            // Args are a linked list (CallExprNode.args -> next). On any
            // argument-evaluation error we free everything evaluated so far
            // and bubble the error up.
            ComptimeValue* args[16];
            size_t actual_arg_count = 0;
            for (ASTNode* a = call_node->args; a && actual_arg_count < 16; a = a->next) {
                ComptimeResult* arg_res = comptime_eval_expression(ctx, a);
                if (!arg_res || arg_res->error || !arg_res->value) {
                    for (size_t i = 0; i < actual_arg_count; i++) {
                        comptime_value_free(args[i]);
                    }
                    if (arg_res) return arg_res;
                    return comptime_result_new(NULL, comptime_error_new("Argument evaluation failed", call->pos), NULL);
                }
                args[actual_arg_count++] = arg_res->value;
                arg_res->value = NULL; // ownership transferred into args[]
                comptime_result_free(arg_res);
            }

            ComptimeResult* call_res = comptime_call_user_function(ctx, func_node, args, actual_arg_count);

            // The callee copied each arg into its scope (comptime_value_copy in
            // the bind loop), so the originals here are ours to free.
            for (size_t i = 0; i < actual_arg_count; i++) {
                comptime_value_free(args[i]);
            }

            // A return-statement-carrying result should propagate the *value*
            // up but not the is_return flag — the call expression is itself a
            // value-producing expression, not a return inside the caller.
            if (call_res) call_res->is_return = false;
            return call_res;
        }
        
        // Function not found
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Undefined function in comptime context: %s", func_ident->name);
        return comptime_result_new(NULL, comptime_error_new(error_msg, call->pos), NULL);
    }
    
    return comptime_result_new(NULL, comptime_error_new("Complex function calls not supported in comptime evaluation", call->pos), NULL);
}

// Evaluate an if statement
static ComptimeResult* comptime_eval_if_stmt(ComptimeContext* ctx, ASTNode* stmt) {
    if (!ctx || !stmt || stmt->type != AST_IF_STMT) {
        return comptime_result_new(NULL, comptime_error_new("Invalid if statement", (Position){0}), NULL);
    }
    
    IfStmtNode* if_stmt = (IfStmtNode*)stmt;
    
    // Evaluate condition
    ComptimeResult* cond_result = comptime_eval_expression(ctx, if_stmt->condition);
    if (cond_result->error) {
        return cond_result;
    }
    
    bool condition_true = comptime_value_is_truthy(cond_result->value);
    comptime_result_free(cond_result);
    
    // Execute appropriate branch via the enhanced dispatcher so block bodies
    // (the common case — `if cond { return n }`) and nested if/for/return
    // statements are handled. The non-enhanced dispatcher only knows
    // EXPR/CONST/VAR_DECL.
    if (condition_true) {
        return comptime_eval_statement_enhanced(ctx, if_stmt->then_stmt);
    } else if (if_stmt->else_stmt) {
        return comptime_eval_statement_enhanced(ctx, if_stmt->else_stmt);
    } else {
        // No else branch, return null
        return comptime_result_new(comptime_value_new(COMPTIME_VALUE_NULL), NULL, NULL);
    }
}

// Evaluate a for statement (simplified)
static ComptimeResult* comptime_eval_for_stmt(ComptimeContext* ctx, ASTNode* stmt) {
    if (!ctx || !stmt || stmt->type != AST_FOR_STMT) {
        return comptime_result_new(NULL, comptime_error_new("Invalid for statement", (Position){0}), NULL);
    }
    
    ForStmtNode* for_stmt = (ForStmtNode*)stmt;
    
    // Create new scope for the loop
    ComptimeContext* loop_ctx = comptime_context_new(ctx);
    if (!loop_ctx) {
        return comptime_result_new(NULL, comptime_error_new("Failed to create loop context", stmt->pos), NULL);
    }
    
    ComptimeResult* last_result = NULL;
    
    // Execute initialization if present
    if (for_stmt->init) {
        last_result = comptime_eval_statement(loop_ctx, for_stmt->init);
        if (last_result->error) {
            comptime_context_free(loop_ctx);
            return last_result;
        }
        comptime_result_free(last_result);
    }
    
    // Loop execution with iteration limit
    size_t max_iterations = 1000; // Prevent infinite loops
    size_t iteration_count = 0;
    
    while (iteration_count < max_iterations) {
        // Check condition if present
        if (for_stmt->condition) {
            ComptimeResult* cond_result = comptime_eval_expression(loop_ctx, for_stmt->condition);
            if (cond_result->error) {
                comptime_context_free(loop_ctx);
                return cond_result;
            }
            
            bool should_continue = comptime_value_is_truthy(cond_result->value);
            comptime_result_free(cond_result);
            
            if (!should_continue) {
                break;
            }
        }
        
        // Execute body
        comptime_result_free(last_result);
        last_result = comptime_eval_statement(loop_ctx, for_stmt->body);
        if (last_result->error) {
            comptime_context_free(loop_ctx);
            return last_result;
        }
        
        // Execute post statement if present
        if (for_stmt->post) {
            ComptimeResult* post_result = comptime_eval_statement(loop_ctx, for_stmt->post);
            if (post_result->error) {
                comptime_result_free(last_result);
                comptime_context_free(loop_ctx);
                return post_result;
            }
            comptime_result_free(post_result);
        }
        
        iteration_count++;
    }
    
    if (iteration_count >= max_iterations) {
        comptime_result_free(last_result);
        comptime_context_free(loop_ctx);
        return comptime_result_new(NULL, comptime_error_new("Loop iteration limit exceeded in comptime evaluation", stmt->pos), NULL);
    }
    
    // Copy generated code from loop context
    if (loop_ctx->generated_code && last_result && !last_result->generated_code) {
        last_result->generated_code = strdup(loop_ctx->generated_code);
    }
    
    comptime_context_free(loop_ctx);
    
    if (!last_result) {
        last_result = comptime_result_new(comptime_value_new(COMPTIME_VALUE_NULL), NULL, NULL);
    }
    
    return last_result;
}

// Evaluate a return statement
static ComptimeResult* comptime_eval_return_stmt(ComptimeContext* ctx, ASTNode* stmt) {
    if (!ctx || !stmt || stmt->type != AST_RETURN_STMT) {
        return comptime_result_new(NULL, comptime_error_new("Invalid return statement", (Position){0}), NULL);
    }
    
    ReturnStmtNode* return_stmt = (ReturnStmtNode*)stmt;
    
    if (return_stmt->values) {
        // Evaluate return expression
        ComptimeResult* expr_result = comptime_eval_expression(ctx, return_stmt->values);
        if (expr_result->error) {
            return expr_result;
        }

        expr_result->is_return = true;
        return expr_result;
    } else {
        ComptimeResult* res = comptime_result_new(comptime_value_new(COMPTIME_VALUE_NULL), NULL, NULL);
        if (res) res->is_return = true;
        return res;
    }
}

// Enhanced statement evaluation with control flow
ComptimeResult* comptime_eval_statement_enhanced(ComptimeContext* ctx, ASTNode* stmt) {
    if (!ctx || !stmt) {
        return comptime_result_new(NULL, comptime_error_new("Invalid context or statement", (Position){0}), NULL);
    }
    
    switch (stmt->type) {
        case AST_BLOCK_STMT:
            // Blocks (e.g. an if/for body) recurse into comptime_eval_block,
            // which walks all statements and propagates is_return up.
            return comptime_eval_block(ctx, stmt);

        case AST_IF_STMT:
            return comptime_eval_if_stmt(ctx, stmt);

        case AST_FOR_STMT:
            return comptime_eval_for_stmt(ctx, stmt);

        case AST_RETURN_STMT:
            return comptime_eval_return_stmt(ctx, stmt);

        case AST_FUNC_DECL: {
            // Register function in context
            FuncDeclNode* func_decl = (FuncDeclNode*)stmt;
            
            if (!comptime_context_bind_func(ctx, func_decl->name, stmt)) {
                return comptime_result_new(NULL, comptime_error_new("Failed to bind function", stmt->pos), NULL);
            }
            
            return comptime_result_new(comptime_value_new(COMPTIME_VALUE_NULL), NULL, NULL);
        }
        
        default:
            // Fall back to original implementation
            return comptime_eval_statement(ctx, stmt);
    }
}

// Constant folding optimization
ComptimeValue* comptime_constant_fold(ComptimeContext* ctx, ASTNode* expr) {
    if (!ctx || !expr) return NULL;
    
    // Only fold simple expressions that don't have side effects
    switch (expr->type) {
        case AST_LITERAL:
            return comptime_value_from_literal(expr);
            
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)expr;
            
            // Try to fold both operands
            ComptimeValue* left = comptime_constant_fold(ctx, binary->left);
            ComptimeValue* right = comptime_constant_fold(ctx, binary->right);
            
            if (left && right) {
                // Both operands are constants, perform the operation
                ComptimeResult* result = comptime_eval_expression(ctx, expr);
                if (result && !result->error) {
                    ComptimeValue* folded = result->value;
                    result->value = NULL; // Transfer ownership
                    comptime_result_free(result);
                    comptime_value_free(left);
                    comptime_value_free(right);
                    return folded;
                }
                comptime_result_free(result);
            }
            
            comptime_value_free(left);
            comptime_value_free(right);
            return NULL;
        }
        
        case AST_UNARY_EXPR: {
            UnaryExprNode* unary = (UnaryExprNode*)expr;
            
            ComptimeValue* operand = comptime_constant_fold(ctx, unary->operand);
            if (operand) {
                ComptimeResult* result = comptime_eval_expression(ctx, expr);
                if (result && !result->error) {
                    ComptimeValue* folded = result->value;
                    result->value = NULL;
                    comptime_result_free(result);
                    comptime_value_free(operand);
                    return folded;
                }
                comptime_result_free(result);
            }
            
            comptime_value_free(operand);
            return NULL;
        }
        
        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)expr;
            ComptimeValue* value = comptime_context_lookup_var(ctx, ident->name);
            
            // Only fold if it's a constant (we'd need additional metadata to track this)
            if (value) {
                return comptime_value_copy(value);
            }
            return NULL;
        }
        
        default:
            return NULL; // Cannot fold
    }
}

// Check if an expression can be evaluated at compile time
bool comptime_is_evaluable(ComptimeContext* ctx, ASTNode* expr) {
    if (!ctx || !expr) return false;
    
    switch (expr->type) {
        case AST_LITERAL:
            return true;
            
        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)expr;
            ComptimeValue* value = comptime_context_lookup_var(ctx, ident->name);
            return value != NULL;
        }
        
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)expr;
            return comptime_is_evaluable(ctx, binary->left) && 
                   comptime_is_evaluable(ctx, binary->right);
        }
        
        case AST_UNARY_EXPR: {
            UnaryExprNode* unary = (UnaryExprNode*)expr;
            return comptime_is_evaluable(ctx, unary->operand);
        }
        
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            
            // Check if it's a comptime intrinsic
            if (call->function && call->function->type == AST_IDENTIFIER) {
                IdentifierNode* func_ident = (IdentifierNode*)call->function;
                if (func_ident->name[0] == '@') {
                    return true; // Intrinsics are always evaluable
                }
                
                // Check if it's a user-defined function
                ASTNode* func_node = comptime_context_lookup_func(ctx, func_ident->name);
                return func_node != NULL;
            }
            
            return false;
        }
        
        default:
            return false;
    }
}
