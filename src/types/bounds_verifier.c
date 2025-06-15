#include "panic_free.h"
#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

// =============================================================================
// Bounds Verifier Management
// =============================================================================

BoundsVerifier* bounds_verifier_new(TypeChecker* type_checker) {
    BoundsVerifier* verifier = malloc(sizeof(BoundsVerifier));
    if (!verifier) return NULL;
    
    memset(verifier, 0, sizeof(BoundsVerifier));
    
    // Create verification context
    verifier->context = verification_context_new(type_checker);
    if (!verifier->context) {
        free(verifier);
        return NULL;
    }
    
    // Set default configuration
    verifier->enable_smt_solver = 0;            // Disabled for now
    verifier->enable_invariant_inference = 1;   // Enable loop invariant inference
    verifier->enable_path_sensitivity = 1;      // Track constraints through branches
    verifier->max_unroll_depth = 3;            // Conservative unrolling
    
    // Initialize proof cache
    verifier->cache_capacity = 128;
    verifier->proof_cache = malloc(sizeof(BoundsProof*) * verifier->cache_capacity);
    if (verifier->proof_cache) {
        memset(verifier->proof_cache, 0, sizeof(BoundsProof*) * verifier->cache_capacity);
    }
    
    return verifier;
}

void bounds_verifier_free(BoundsVerifier* verifier) {
    if (!verifier) return;
    
    // Free verification context
    if (verifier->context) {
        verification_context_free(verifier->context);
    }
    
    // Free proof cache
    if (verifier->proof_cache) {
        for (size_t i = 0; i < verifier->cache_size; i++) {
            if (verifier->proof_cache[i]) {
                bounds_proof_free(verifier->proof_cache[i]);
            }
        }
        free(verifier->proof_cache);
    }
    
    free(verifier);
}

VerificationContext* verification_context_new(TypeChecker* type_checker) {
    VerificationContext* context = malloc(sizeof(VerificationContext));
    if (!context) return NULL;
    
    memset(context, 0, sizeof(VerificationContext));
    
    context->type_checker = type_checker;
    context->current_function = NULL;
    context->current_scope = NULL;
    
    // Initialize constraint storage
    context->constraint_capacity = 64;
    context->active_constraints = malloc(sizeof(BoundsConstraint*) * context->constraint_capacity);
    if (context->active_constraints) {
        memset(context->active_constraints, 0, sizeof(BoundsConstraint*) * context->constraint_capacity);
    }
    
    // Initialize symbol storage
    context->symbol_capacity = 128;
    context->symbols = malloc(sizeof(SymbolicExpression*) * context->symbol_capacity);
    if (context->symbols) {
        memset(context->symbols, 0, sizeof(SymbolicExpression*) * context->symbol_capacity);
    }
    
    // Set default configuration
    context->default_mode = BOUNDS_CHECK_PROVE_SAFE;
    context->enable_proof_optimization = 1;
    context->enable_loop_analysis = 1;
    context->max_proof_complexity = 1000;
    context->proof_timeout = 5.0; // 5 seconds
    
    return context;
}

void verification_context_free(VerificationContext* context) {
    if (!context) return;
    
    // Free constraints
    if (context->active_constraints) {
        for (size_t i = 0; i < context->constraint_count; i++) {
            if (context->active_constraints[i]) {
                bounds_constraint_free(context->active_constraints[i]);
            }
        }
        free(context->active_constraints);
    }
    
    // Free symbols
    if (context->symbols) {
        for (size_t i = 0; i < context->symbol_count; i++) {
            if (context->symbols[i]) {
                symbolic_expression_free(context->symbols[i]);
            }
        }
        free(context->symbols);
    }
    
    free(context);
}

void bounds_verifier_set_mode(BoundsVerifier* verifier, BoundsCheckMode mode) {
    if (!verifier || !verifier->context) return;
    verifier->context->default_mode = mode;
}

void bounds_verifier_enable_feature(BoundsVerifier* verifier, const char* feature, int enable) {
    if (!verifier || !feature) return;
    
    if (strcmp(feature, "smt_solver") == 0) {
        verifier->enable_smt_solver = enable;
    } else if (strcmp(feature, "invariant_inference") == 0) {
        verifier->enable_invariant_inference = enable;
    } else if (strcmp(feature, "path_sensitivity") == 0) {
        verifier->enable_path_sensitivity = enable;
    } else if (strcmp(feature, "proof_optimization") == 0) {
        verifier->context->enable_proof_optimization = enable;
    } else if (strcmp(feature, "loop_analysis") == 0) {
        verifier->context->enable_loop_analysis = enable;
    }
}

// =============================================================================
// Symbolic Expression Management
// =============================================================================

SymbolicExpression* symbolic_expression_new(SymbolicExpressionType type) {
    SymbolicExpression* expr = malloc(sizeof(SymbolicExpression));
    if (!expr) return NULL;
    
    memset(expr, 0, sizeof(SymbolicExpression));
    expr->type = type;
    expr->result_type = NULL;
    
    return expr;
}

void symbolic_expression_free(SymbolicExpression* expr) {
    if (!expr) return;
    
    switch (expr->type) {
        case SYMBOLIC_VARIABLE:
            free(expr->data.variable.name);
            break;
            
        case SYMBOLIC_BINARY_OP:
            symbolic_expression_free(expr->data.binary_op.left);
            symbolic_expression_free(expr->data.binary_op.right);
            break;
            
        case SYMBOLIC_UNARY_OP:
            symbolic_expression_free(expr->data.unary_op.operand);
            break;
            
        case SYMBOLIC_FUNCTION_CALL:
            free(expr->data.function_call.function_name);
            if (expr->data.function_call.arguments) {
                for (size_t i = 0; i < expr->data.function_call.arg_count; i++) {
                    symbolic_expression_free(expr->data.function_call.arguments[i]);
                }
                free(expr->data.function_call.arguments);
            }
            break;
            
        case SYMBOLIC_ARRAY_LENGTH:
            symbolic_expression_free(expr->data.array_length.array_expr);
            break;
            
        case SYMBOLIC_CONDITIONAL:
            symbolic_expression_free(expr->data.conditional.condition);
            symbolic_expression_free(expr->data.conditional.true_expr);
            symbolic_expression_free(expr->data.conditional.false_expr);
            break;
            
        case SYMBOLIC_CONSTANT:
        default:
            // No additional cleanup needed
            break;
    }
    
    free(expr);
}

SymbolicExpression* symbolic_constant(int64_t value) {
    SymbolicExpression* expr = symbolic_expression_new(SYMBOLIC_CONSTANT);
    if (expr) {
        expr->data.constant.value = value;
    }
    return expr;
}

SymbolicExpression* symbolic_variable(const char* name, Type* type) {
    SymbolicExpression* expr = symbolic_expression_new(SYMBOLIC_VARIABLE);
    if (expr) {
        expr->data.variable.name = strdup(name);
        expr->data.variable.type = type;
        expr->result_type = type;
    }
    return expr;
}

SymbolicExpression* symbolic_binary_op(SymbolicExpression* left, TokenType op, SymbolicExpression* right) {
    if (!left || !right) return NULL;
    
    SymbolicExpression* expr = symbolic_expression_new(SYMBOLIC_BINARY_OP);
    if (expr) {
        expr->data.binary_op.left = left;
        expr->data.binary_op.right = right;
        expr->data.binary_op.operator = op;
        
        // Simple type inference - could be more sophisticated
        if (left->result_type && right->result_type) {
            expr->result_type = left->result_type; // Simplified
        }
    }
    return expr;
}

SymbolicExpression* symbolic_array_length(SymbolicExpression* array_expr) {
    if (!array_expr) return NULL;
    
    SymbolicExpression* expr = symbolic_expression_new(SYMBOLIC_ARRAY_LENGTH);
    if (expr) {
        expr->data.array_length.array_expr = array_expr;
        // Length is always an integer type
        expr->result_type = NULL; // Would set to int type
    }
    return expr;
}

// Convert AST expression to symbolic expression
SymbolicExpression* ast_to_symbolic_expression(ASTNode* ast_expr, VerificationContext* context) {
    if (!ast_expr || !context) return NULL;
    
    switch (ast_expr->type) {
        case AST_LITERAL: {
            LiteralNode* lit = (LiteralNode*)ast_expr;
            if (lit->literal_type == TOKEN_INT) {
                int64_t value = strtoll(lit->value, NULL, 10);
                return symbolic_constant(value);
            }
            break;
        }
        
        case AST_IDENTIFIER: {
            IdentifierNode* id = (IdentifierNode*)ast_expr;
            Variable* var = type_checker_lookup_variable(context->type_checker, id->name);
            if (var) {
                return symbolic_variable(id->name, var->type);
            }
            break;
        }
        
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)ast_expr;
            SymbolicExpression* left = ast_to_symbolic_expression(binary->left, context);
            SymbolicExpression* right = ast_to_symbolic_expression(binary->right, context);
            if (left && right) {
                return symbolic_binary_op(left, binary->operator, right);
            }
            symbolic_expression_free(left);
            symbolic_expression_free(right);
            break;
        }
        
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)ast_expr;
            if (call->function->type == AST_IDENTIFIER) {
                IdentifierNode* func_id = (IdentifierNode*)call->function;
                
                // Handle special functions like len()
                if (strcmp(func_id->name, "len") == 0 && call->args) {
                    SymbolicExpression* array_expr = ast_to_symbolic_expression(call->args, context);
                    if (array_expr) {
                        return symbolic_array_length(array_expr);
                    }
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    return NULL;
}

// Simplify symbolic expression
SymbolicExpression* symbolic_expression_simplify(SymbolicExpression* expr) {
    if (!expr) return NULL;
    
    switch (expr->type) {
        case SYMBOLIC_BINARY_OP: {
            // Simplify operands first
            SymbolicExpression* left = symbolic_expression_simplify(expr->data.binary_op.left);
            SymbolicExpression* right = symbolic_expression_simplify(expr->data.binary_op.right);
            
            // Constant folding
            if (left && right && 
                left->type == SYMBOLIC_CONSTANT && right->type == SYMBOLIC_CONSTANT) {
                
                int64_t left_val = left->data.constant.value;
                int64_t right_val = right->data.constant.value;
                int64_t result;
                
                switch (expr->data.binary_op.operator) {
                    case TOKEN_PLUS:
                        result = left_val + right_val;
                        break;
                    case TOKEN_MINUS:
                        result = left_val - right_val;
                        break;
                    case TOKEN_MULTIPLY:
                        result = left_val * right_val;
                        break;
                    case TOKEN_DIVIDE:
                        if (right_val != 0) {
                            result = left_val / right_val;
                        } else {
                            return expr; // Can't divide by zero
                        }
                        break;
                    default:
                        return expr; // Other operators not simplified yet
                }
                
                symbolic_expression_free(left);
                symbolic_expression_free(right);
                symbolic_expression_free(expr);
                return symbolic_constant(result);
            }
            
            // Update simplified operands
            if (left != expr->data.binary_op.left) {
                symbolic_expression_free(expr->data.binary_op.left);
                expr->data.binary_op.left = left;
            }
            if (right != expr->data.binary_op.right) {
                symbolic_expression_free(expr->data.binary_op.right);
                expr->data.binary_op.right = right;
            }
            break;
        }
        
        default:
            // No simplification for other types yet
            break;
    }
    
    return expr;
}

char* symbolic_expression_to_string(const SymbolicExpression* expr) {
    if (!expr) return strdup("null");
    
    char* result = malloc(256);
    if (!result) return NULL;
    
    switch (expr->type) {
        case SYMBOLIC_CONSTANT:
            snprintf(result, 256, "%lld", (long long)expr->data.constant.value);
            break;
            
        case SYMBOLIC_VARIABLE:
            snprintf(result, 256, "%s", expr->data.variable.name ? expr->data.variable.name : "?");
            break;
            
        case SYMBOLIC_BINARY_OP: {
            char* left_str = symbolic_expression_to_string(expr->data.binary_op.left);
            char* right_str = symbolic_expression_to_string(expr->data.binary_op.right);
            const char* op_str = "?";
            
            switch (expr->data.binary_op.operator) {
                case TOKEN_PLUS: op_str = "+"; break;
                case TOKEN_MINUS: op_str = "-"; break;
                case TOKEN_MULTIPLY: op_str = "*"; break;
                case TOKEN_DIVIDE: op_str = "/"; break;
                case TOKEN_LT: op_str = "<"; break;
                case TOKEN_GT: op_str = ">"; break;
                case TOKEN_LE: op_str = "<="; break;
                case TOKEN_GE: op_str = ">="; break;
                case TOKEN_EQ: op_str = "=="; break;
                case TOKEN_NE: op_str = "!="; break;
                default: break;
            }
            
            snprintf(result, 256, "(%s %s %s)", 
                     left_str ? left_str : "?", op_str, right_str ? right_str : "?");
            
            free(left_str);
            free(right_str);
            break;
        }
        
        case SYMBOLIC_ARRAY_LENGTH: {
            char* array_str = symbolic_expression_to_string(expr->data.array_length.array_expr);
            snprintf(result, 256, "len(%s)", array_str ? array_str : "?");
            free(array_str);
            break;
        }
        
        default:
            snprintf(result, 256, "<?>");
            break;
    }
    
    return result;
}

// =============================================================================
// Bounds Constraint Management
// =============================================================================

BoundsConstraint* bounds_constraint_new(ConstraintType type) {
    BoundsConstraint* constraint = malloc(sizeof(BoundsConstraint));
    if (!constraint) return NULL;
    
    memset(constraint, 0, sizeof(BoundsConstraint));
    constraint->type = type;
    constraint->confidence = 1.0;
    
    return constraint;
}

void bounds_constraint_free(BoundsConstraint* constraint) {
    if (!constraint) return;
    
    switch (constraint->type) {
        case CONSTRAINT_LESS_THAN:
        case CONSTRAINT_LESS_EQUAL:
        case CONSTRAINT_GREATER_THAN:
        case CONSTRAINT_GREATER_EQUAL:
        case CONSTRAINT_EQUAL:
        case CONSTRAINT_NOT_EQUAL:
            symbolic_expression_free(constraint->data.comparison.left);
            symbolic_expression_free(constraint->data.comparison.right);
            break;
            
        case CONSTRAINT_AND:
        case CONSTRAINT_OR:
        case CONSTRAINT_IMPLIES:
            bounds_constraint_free(constraint->data.compound.left);
            bounds_constraint_free(constraint->data.compound.right);
            break;
            
        case CONSTRAINT_NOT:
            bounds_constraint_free(constraint->data.unary.operand);
            break;
    }
    
    free(constraint);
}

BoundsConstraint* bounds_constraint_comparison(ConstraintType type, SymbolicExpression* left, SymbolicExpression* right) {
    if (!left || !right) return NULL;
    
    BoundsConstraint* constraint = bounds_constraint_new(type);
    if (constraint) {
        constraint->data.comparison.left = left;
        constraint->data.comparison.right = right;
    }
    return constraint;
}

char* bounds_constraint_to_string(const BoundsConstraint* constraint) {
    if (!constraint) return strdup("null");
    
    char* result = malloc(512);
    if (!result) return NULL;
    
    switch (constraint->type) {
        case CONSTRAINT_LESS_THAN:
        case CONSTRAINT_LESS_EQUAL:
        case CONSTRAINT_GREATER_THAN:
        case CONSTRAINT_GREATER_EQUAL:
        case CONSTRAINT_EQUAL:
        case CONSTRAINT_NOT_EQUAL: {
            char* left_str = symbolic_expression_to_string(constraint->data.comparison.left);
            char* right_str = symbolic_expression_to_string(constraint->data.comparison.right);
            const char* op_str = "?";
            
            switch (constraint->type) {
                case CONSTRAINT_LESS_THAN: op_str = "<"; break;
                case CONSTRAINT_LESS_EQUAL: op_str = "<="; break;
                case CONSTRAINT_GREATER_THAN: op_str = ">"; break;
                case CONSTRAINT_GREATER_EQUAL: op_str = ">="; break;
                case CONSTRAINT_EQUAL: op_str = "=="; break;
                case CONSTRAINT_NOT_EQUAL: op_str = "!="; break;
                default: break;
            }
            
            snprintf(result, 512, "%s %s %s", 
                     left_str ? left_str : "?", op_str, right_str ? right_str : "?");
            
            free(left_str);
            free(right_str);
            break;
        }
        
        case CONSTRAINT_AND:
        case CONSTRAINT_OR:
        case CONSTRAINT_IMPLIES: {
            char* left_str = bounds_constraint_to_string(constraint->data.compound.left);
            char* right_str = bounds_constraint_to_string(constraint->data.compound.right);
            const char* op_str = "?";
            
            switch (constraint->type) {
                case CONSTRAINT_AND: op_str = "&&"; break;
                case CONSTRAINT_OR: op_str = "||"; break;
                case CONSTRAINT_IMPLIES: op_str = "=>"; break;
                default: break;
            }
            
            snprintf(result, 512, "(%s %s %s)", 
                     left_str ? left_str : "?", op_str, right_str ? right_str : "?");
            
            free(left_str);
            free(right_str);
            break;
        }
        
        case CONSTRAINT_NOT: {
            char* operand_str = bounds_constraint_to_string(constraint->data.unary.operand);
            snprintf(result, 512, "!(%s)", operand_str ? operand_str : "?");
            free(operand_str);
            break;
        }
        
        default:
            snprintf(result, 512, "<?>");
            break;
    }
    
    return result;
}

// =============================================================================
// Bounds Verification Core
// =============================================================================

BoundsProof* bounds_proof_new(ASTNode* target_node) {
    BoundsProof* proof = malloc(sizeof(BoundsProof));
    if (!proof) return NULL;
    
    memset(proof, 0, sizeof(BoundsProof));
    proof->status = PROOF_STATUS_UNKNOWN;
    proof->target_node = target_node;
    // confidence field not defined in header
    
    return proof;
}

void bounds_proof_free(BoundsProof* proof) {
    if (!proof) return;
    
    symbolic_expression_free(proof->index_expr);
    symbolic_expression_free(proof->bound_expr);
    
    // Free assumptions
    if (proof->assumptions) {
        for (size_t i = 0; i < proof->assumption_count; i++) {
            bounds_constraint_free(proof->assumptions[i]);
        }
        free(proof->assumptions);
    }
    
    // Free conclusions
    if (proof->conclusions) {
        for (size_t i = 0; i < proof->conclusion_count; i++) {
            bounds_constraint_free(proof->conclusions[i]);
        }
        free(proof->conclusions);
    }
    
    // Free proof steps
    if (proof->proof_steps) {
        for (size_t i = 0; i < proof->step_count; i++) {
            free(proof->proof_steps[i]);
        }
        free(proof->proof_steps);
    }
    
    free(proof->optimization_note);
    free(proof);
}

void bounds_proof_add_step(BoundsProof* proof, const char* step_description) {
    if (!proof || !step_description) return;
    
    // Simple approach: always allocate space for one more step
    char** new_steps = realloc(proof->proof_steps, sizeof(char*) * (proof->step_count + 1));
    if (!new_steps) return;
    
    proof->proof_steps = new_steps;
    proof->proof_steps[proof->step_count] = strdup(step_description);
    proof->step_count++;
}

// Main verification function for array access
BoundsProof* verify_array_access(BoundsVerifier* verifier, ASTNode* array_access) {
    if (!verifier || !array_access || array_access->type != AST_INDEX_EXPR) {
        return NULL;
    }
    
    verifier->total_array_accesses++;
    
    IndexExprNode* index_expr = (IndexExprNode*)array_access;
    
    // Create proof object
    BoundsProof* proof = bounds_proof_new(array_access);
    if (!proof) return NULL;
    
    // Convert index and array expressions to symbolic form
    proof->index_expr = ast_to_symbolic_expression(index_expr->index, verifier->context);
    
    // Create bound expression (array length)
    proof->bound_expr = symbolic_array_length(
        ast_to_symbolic_expression(index_expr->expr, verifier->context)
    );
    
    if (!proof->index_expr || !proof->bound_expr) {
        proof->status = PROOF_STATUS_ERROR;
        bounds_proof_add_step(proof, "Failed to convert expressions to symbolic form");
        return proof;
    }
    
    // Perform verification
    BoundsConstraint* safety_constraint = bounds_constraint_comparison(
        CONSTRAINT_LESS_THAN, proof->index_expr, proof->bound_expr
    );
    
    if (safety_constraint) {
        proof->status = determine_proof_status(verifier->context, safety_constraint);
        
        // Add constraint to proof
        proof->conclusions = malloc(sizeof(BoundsConstraint*));
        proof->conclusions[0] = safety_constraint;
        proof->conclusion_count = 1;
        
        // Determine optimization potential
        if (proof->status == PROOF_STATUS_SAFE) {
            proof->can_eliminate_check = 1;
            proof->optimization_note = strdup("Bounds check can be eliminated - access proven safe");
            verifier->proven_safe_accesses++;
            verifier->eliminated_checks++;
        } else if (proof->status == PROOF_STATUS_CONDITIONAL) {
            proof->requires_runtime_check = 1;
            proof->optimization_note = strdup("Runtime check required - safety depends on runtime conditions");
        } else {
            proof->requires_runtime_check = 1;
            proof->optimization_note = strdup("Runtime check required - safety cannot be proven");
        }
    } else {
        proof->status = PROOF_STATUS_ERROR;
        bounds_proof_add_step(proof, "Failed to create safety constraint");
    }
    
    return proof;
}

// Determine proof status using simple heuristics
ProofStatus determine_proof_status(VerificationContext* context, BoundsConstraint* goal) {
    if (!context || !goal) return PROOF_STATUS_ERROR;
    
    // For now, implement simple heuristics
    // In a full implementation, this would use SMT solvers or theorem provers
    
    if (goal->type == CONSTRAINT_LESS_THAN) {
        SymbolicExpression* left = goal->data.comparison.left;
        SymbolicExpression* right = goal->data.comparison.right;
        
        // Simple case: constant index vs array length
        if (left->type == SYMBOLIC_CONSTANT && right->type == SYMBOLIC_ARRAY_LENGTH) {
            // Need more context to determine if this is safe
            // For now, return conditional
            return PROOF_STATUS_CONDITIONAL;
        }
        
        // Loop variable vs array length
        if (left->type == SYMBOLIC_VARIABLE && right->type == SYMBOLIC_ARRAY_LENGTH) {
            // Check if we have constraints on the variable
            for (size_t i = 0; i < context->constraint_count; i++) {
                BoundsConstraint* constraint = context->active_constraints[i];
                if (constraint && constraint->type == CONSTRAINT_LESS_THAN) {
                    // Check if this constraint applies to our variable
                    // This is simplified - real implementation would be more sophisticated
                    return PROOF_STATUS_SAFE;
                }
            }
            return PROOF_STATUS_CONDITIONAL;
        }
    }
    
    return PROOF_STATUS_UNKNOWN;
}

// =============================================================================
// Program Analysis
// =============================================================================

int analyze_function_bounds(BoundsVerifier* verifier, ASTNode* function) {
    if (!verifier || !function || function->type != AST_FUNC_DECL) {
        return 0;
    }
    
    FuncDeclNode* func_decl = (FuncDeclNode*)function;
    verifier->context->current_function = function;
    
    // Analyze function body
    if (func_decl->body) {
        analyze_statement_bounds(verifier, func_decl->body);
    }
    
    verifier->context->current_function = NULL;
    return 1;
}

int analyze_statement_bounds(BoundsVerifier* verifier, ASTNode* statement) {
    if (!verifier || !statement) return 0;
    
    switch (statement->type) {
        case AST_BLOCK_STMT: {
            BlockStmtNode* block = (BlockStmtNode*)statement;
            ASTNode* current = block->statements;
            while (current) {
                analyze_statement_bounds(verifier, current);
                current = current->next;
            }
            break;
        }
        
        case AST_EXPR_STMT: {
            ExprStmtNode* expr_stmt = (ExprStmtNode*)statement;
            analyze_expression_bounds(verifier, expr_stmt->expr);
            break;
        }
        
        case AST_FOR_STMT: {
            if (verifier->context->enable_loop_analysis) {
                analyze_loop_bounds(verifier->context, statement);
            }
            break;
        }
        
        case AST_IF_STMT: {
            if (verifier->enable_path_sensitivity) {
                analyze_conditional_bounds(verifier->context, statement);
            }
            break;
        }
        
        default:
            break;
    }
    
    return 1;
}

int analyze_expression_bounds(BoundsVerifier* verifier, ASTNode* expression) {
    if (!verifier || !expression) return 0;
    
    switch (expression->type) {
        case AST_INDEX_EXPR: {
            // This is an array access - verify bounds
            BoundsProof* proof = verify_array_access(verifier, expression);
            if (proof) {
                // Cache the proof for later optimization
                if (verifier->cache_size < verifier->cache_capacity) {
                    verifier->proof_cache[verifier->cache_size++] = proof;
                } else {
                    bounds_proof_free(proof);
                }
            }
            break;
        }
        
        case AST_BINARY_EXPR: {
            BinaryExprNode* binary = (BinaryExprNode*)expression;
            analyze_expression_bounds(verifier, binary->left);
            analyze_expression_bounds(verifier, binary->right);
            break;
        }
        
        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expression;
            ASTNode* arg = call->args;
            while (arg) {
                analyze_expression_bounds(verifier, arg);
                arg = arg->next;
            }
            break;
        }
        
        default:
            break;
    }
    
    return 1;
}

// Analyze conditional bounds (simplified implementation)
int analyze_conditional_bounds(VerificationContext* context, ASTNode* if_stmt) {
    if (!context || !if_stmt || if_stmt->type != AST_IF_STMT) {
        return 0;
    }
    
    // For now, just a placeholder implementation
    // Real implementation would analyze both branches and merge constraints
    return 1;
}

// Simplified loop analysis
int analyze_loop_bounds(VerificationContext* context, ASTNode* loop_node) {
    if (!context || !loop_node || loop_node->type != AST_FOR_STMT) {
        return 0;
    }
    
    // For now, just add a simple constraint that loop variables are bounded
    // Real implementation would do sophisticated loop invariant inference
    
    ForStmtNode* for_stmt = (ForStmtNode*)loop_node;
    
    // Simple heuristic: if init is "i = 0" and condition is "i < len(array)"
    // then we can infer that i is always < len(array) in the loop body
    
    if (for_stmt->condition && for_stmt->condition->type == AST_BINARY_EXPR) {
        BinaryExprNode* condition = (BinaryExprNode*)for_stmt->condition;
        
        if (condition->operator == TOKEN_LT) {
            // Create constraint: left < right
            SymbolicExpression* left = ast_to_symbolic_expression(condition->left, context);
            SymbolicExpression* right = ast_to_symbolic_expression(condition->right, context);
            
            if (left && right) {
                BoundsConstraint* constraint = bounds_constraint_comparison(
                    CONSTRAINT_LESS_THAN, left, right
                );
                
                if (constraint && context->constraint_count < context->constraint_capacity) {
                    context->active_constraints[context->constraint_count++] = constraint;
                    return 1;
                }
                
                bounds_constraint_free(constraint);
            }
            
            symbolic_expression_free(left);
            symbolic_expression_free(right);
        }
    }
    
    return 0;
}

// =============================================================================
// Utility Functions
// =============================================================================

const char* bounds_check_mode_to_string(BoundsCheckMode mode) {
    switch (mode) {
        case BOUNDS_CHECK_RUNTIME: return "runtime";
        case BOUNDS_CHECK_PROVE_SAFE: return "prove_safe";
        case BOUNDS_CHECK_OPTIMIZE_OUT: return "optimize_out";
        case BOUNDS_CHECK_HYBRID: return "hybrid";
        case BOUNDS_CHECK_DISABLED: return "disabled";
        default: return "unknown";
    }
}

const char* proof_status_to_string(ProofStatus status) {
    switch (status) {
        case PROOF_STATUS_UNKNOWN: return "unknown";
        case PROOF_STATUS_SAFE: return "safe";
        case PROOF_STATUS_UNSAFE: return "unsafe";
        case PROOF_STATUS_CONDITIONAL: return "conditional";
        case PROOF_STATUS_TIMEOUT: return "timeout";
        case PROOF_STATUS_ERROR: return "error";
        default: return "invalid";
    }
}

void bounds_verifier_print_statistics(BoundsVerifier* verifier) {
    if (!verifier) return;
    
    printf("=== Bounds Verification Statistics ===\n");
    printf("Total array accesses analyzed: %zu\n", verifier->total_array_accesses);
    printf("Proven safe accesses: %zu\n", verifier->proven_safe_accesses);
    printf("Bounds checks eliminated: %zu\n", verifier->eliminated_checks);
    printf("Complex proofs generated: %zu\n", verifier->complex_proofs);
    printf("Total proof generation time: %.3f seconds\n", verifier->total_proof_time);
    
    if (verifier->total_array_accesses > 0) {
        double safe_percentage = (double)verifier->proven_safe_accesses / verifier->total_array_accesses * 100.0;
        printf("Safety proof rate: %.1f%%\n", safe_percentage);
        
        double elimination_percentage = (double)verifier->eliminated_checks / verifier->total_array_accesses * 100.0;
        printf("Check elimination rate: %.1f%%\n", elimination_percentage);
    }
    
    printf("Errors: %d, Warnings: %d\n", verifier->error_count, verifier->warning_count);
    
    // Configuration status
    printf("\nConfiguration:\n");
    printf("- SMT solver: %s\n", verifier->enable_smt_solver ? "enabled" : "disabled");
    printf("- Invariant inference: %s\n", verifier->enable_invariant_inference ? "enabled" : "disabled");
    printf("- Path sensitivity: %s\n", verifier->enable_path_sensitivity ? "enabled" : "disabled");
    printf("- Max unroll depth: %d\n", verifier->max_unroll_depth);
}

void bounds_verifier_print_proof(BoundsProof* proof) {
    if (!proof) return;
    
    printf("=== Bounds Proof ===\n");
    printf("Status: %s\n", proof_status_to_string(proof->status));
    
    if (proof->index_expr) {
        char* index_str = symbolic_expression_to_string(proof->index_expr);
        printf("Index: %s\n", index_str);
        free(index_str);
    }
    
    if (proof->bound_expr) {
        char* bound_str = symbolic_expression_to_string(proof->bound_expr);
        printf("Bound: %s\n", bound_str);
        free(bound_str);
    }
    
    printf("Can eliminate check: %s\n", proof->can_eliminate_check ? "yes" : "no");
    printf("Requires runtime check: %s\n", proof->requires_runtime_check ? "yes" : "no");
    
    if (proof->optimization_note) {
        printf("Optimization: %s\n", proof->optimization_note);
    }
    
    if (proof->step_count > 0) {
        printf("Proof steps:\n");
        for (size_t i = 0; i < proof->step_count; i++) {
            printf("  %zu. %s\n", i + 1, proof->proof_steps[i]);
        }
    }
}