#include "comptime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// =============================================================================
// Value Operations
// =============================================================================

ComptimeValue comptime_value_int(int64_t val) {
    return (ComptimeValue){.kind = COMPTIME_INT, .data.int_val = val};
}

ComptimeValue comptime_value_float(double val) {
    return (ComptimeValue){.kind = COMPTIME_FLOAT, .data.float_val = val};
}

ComptimeValue comptime_value_bool(bool val) {
    return (ComptimeValue){.kind = COMPTIME_BOOL, .data.bool_val = val};
}

ComptimeValue comptime_value_string(const char* val) {
    ComptimeValue v = {.kind = COMPTIME_STRING};
    v.data.string_val = val ? strdup(val) : NULL;
    return v;
}

ComptimeValue comptime_value_nil(void) {
    return (ComptimeValue){.kind = COMPTIME_NIL};
}

ComptimeValue comptime_value_error(const char* msg) {
    ComptimeValue v = {.kind = COMPTIME_ERROR};
    v.data.string_val = msg ? strdup(msg) : NULL;
    return v;
}

void comptime_value_free(ComptimeValue* val) {
    if (!val) return;
    if (val->kind == COMPTIME_STRING || val->kind == COMPTIME_ERROR) {
        free(val->data.string_val);
        val->data.string_val = NULL;
    }
    if (val->kind == COMPTIME_ARRAY) {
        for (size_t i = 0; i < val->data.array_val.length; i++) {
            comptime_value_free(val->data.array_val.elements[i]);
            free(val->data.array_val.elements[i]);
        }
        free(val->data.array_val.elements);
    }
}

bool comptime_value_is_truthy(const ComptimeValue* val) {
    if (!val) return false;
    switch (val->kind) {
        case COMPTIME_INT:    return val->data.int_val != 0;
        case COMPTIME_FLOAT:  return val->data.float_val != 0.0;
        case COMPTIME_BOOL:   return val->data.bool_val;
        case COMPTIME_STRING: return val->data.string_val && strlen(val->data.string_val) > 0;
        case COMPTIME_NIL:    return false;
        case COMPTIME_ARRAY:  return val->data.array_val.length > 0;
        case COMPTIME_ERROR:  return false;
    }
    return false;
}

char* comptime_value_to_string(const ComptimeValue* val) {
    if (!val) return strdup("nil");
    char buf[128];
    switch (val->kind) {
        case COMPTIME_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long)val->data.int_val);
            return strdup(buf);
        case COMPTIME_FLOAT:
            snprintf(buf, sizeof(buf), "%g", val->data.float_val);
            return strdup(buf);
        case COMPTIME_BOOL:
            return strdup(val->data.bool_val ? "true" : "false");
        case COMPTIME_STRING:
            return val->data.string_val ? strdup(val->data.string_val) : strdup("");
        case COMPTIME_NIL:
            return strdup("nil");
        case COMPTIME_ARRAY:
            snprintf(buf, sizeof(buf), "[array:%zu]", val->data.array_val.length);
            return strdup(buf);
        case COMPTIME_ERROR:
            snprintf(buf, sizeof(buf), "error: %s",
                     val->data.string_val ? val->data.string_val : "unknown");
            return strdup(buf);
    }
    return strdup("?");
}

// =============================================================================
// Interpreter Lifecycle
// =============================================================================

ComptimeInterpreter* comptime_interpreter_new(void) {
    ComptimeInterpreter* interp = calloc(1, sizeof(ComptimeInterpreter));
    if (!interp) return NULL;

    interp->max_iterations = 100000;
    interp->max_recursion = 256;

    // Create global scope
    interp->current_scope = calloc(1, sizeof(ComptimeScope));

    return interp;
}

void comptime_interpreter_free(ComptimeInterpreter* interp) {
    if (!interp) return;

    // Free scopes
    while (interp->current_scope) {
        comptime_pop_scope(interp);
    }

    // Free functions
    ComptimeFunc* func = interp->functions;
    while (func) {
        ComptimeFunc* next = func->next;
        free(func->name);
        free(func);
        func = next;
    }

    free(interp->error_message);
    free(interp);
}

// =============================================================================
// Scope Management
// =============================================================================

void comptime_push_scope(ComptimeInterpreter* interp) {
    if (!interp) return;
    ComptimeScope* scope = calloc(1, sizeof(ComptimeScope));
    if (!scope) return;
    scope->parent = interp->current_scope;
    interp->current_scope = scope;
}

void comptime_pop_scope(ComptimeInterpreter* interp) {
    if (!interp || !interp->current_scope) return;

    ComptimeScope* scope = interp->current_scope;
    interp->current_scope = scope->parent;

    // Free bindings
    ComptimeBinding* b = scope->bindings;
    while (b) {
        ComptimeBinding* next = b->next;
        comptime_value_free(&b->value);
        free(b->name);
        free(b);
        b = next;
    }
    free(scope);
}

bool comptime_set_variable(ComptimeInterpreter* interp, const char* name,
                           ComptimeValue value, bool is_const) {
    if (!interp || !name || !interp->current_scope) return false;

    // Check if already exists in current scope
    for (ComptimeBinding* b = interp->current_scope->bindings; b; b = b->next) {
        if (strcmp(b->name, name) == 0) {
            if (b->is_const) {
                interp->has_error = true;
                free(interp->error_message);
                char msg[256];
                snprintf(msg, sizeof(msg), "Cannot reassign constant '%s'", name);
                interp->error_message = strdup(msg);
                return false;
            }
            comptime_value_free(&b->value);
            b->value = value;
            return true;
        }
    }

    ComptimeBinding* b = calloc(1, sizeof(ComptimeBinding));
    if (!b) return false;
    b->name = strdup(name);
    b->value = value;
    b->is_const = is_const;
    b->next = interp->current_scope->bindings;
    interp->current_scope->bindings = b;
    return true;
}

ComptimeValue* comptime_lookup_variable(ComptimeInterpreter* interp, const char* name) {
    if (!interp || !name) return NULL;

    for (ComptimeScope* scope = interp->current_scope; scope; scope = scope->parent) {
        for (ComptimeBinding* b = scope->bindings; b; b = b->next) {
            if (strcmp(b->name, name) == 0) {
                return &b->value;
            }
        }
    }
    return NULL;
}

// =============================================================================
// Function Registration
// =============================================================================

void comptime_register_function(ComptimeInterpreter* interp, const char* name,
                                ASTNode* params, ASTNode* body) {
    if (!interp || !name) return;

    ComptimeFunc* func = calloc(1, sizeof(ComptimeFunc));
    if (!func) return;

    func->name = strdup(name);
    func->params = params;
    func->body = body;
    func->next = interp->functions;
    interp->functions = func;
}

static ComptimeFunc* comptime_find_function(ComptimeInterpreter* interp, const char* name) {
    for (ComptimeFunc* f = interp->functions; f; f = f->next) {
        if (strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}

// =============================================================================
// Error Helpers
// =============================================================================

static void comptime_set_error(ComptimeInterpreter* interp, const char* msg, Position pos) {
    interp->has_error = true;
    free(interp->error_message);
    interp->error_message = strdup(msg);
    interp->error_pos = pos;
}

bool comptime_has_error(ComptimeInterpreter* interp) {
    return interp && interp->has_error;
}

const char* comptime_get_error(ComptimeInterpreter* interp) {
    return interp ? interp->error_message : NULL;
}

// =============================================================================
// Expression Evaluation
// =============================================================================

static int64_t comptime_to_int(const ComptimeValue* v) {
    if (!v) return 0;
    switch (v->kind) {
        case COMPTIME_INT:   return v->data.int_val;
        case COMPTIME_FLOAT: return (int64_t)v->data.float_val;
        case COMPTIME_BOOL:  return v->data.bool_val ? 1 : 0;
        default:             return 0;
    }
}

static double comptime_to_float(const ComptimeValue* v) {
    if (!v) return 0.0;
    switch (v->kind) {
        case COMPTIME_INT:   return (double)v->data.int_val;
        case COMPTIME_FLOAT: return v->data.float_val;
        case COMPTIME_BOOL:  return v->data.bool_val ? 1.0 : 0.0;
        default:             return 0.0;
    }
}

static bool is_numeric(const ComptimeValue* v) {
    return v && (v->kind == COMPTIME_INT || v->kind == COMPTIME_FLOAT);
}

ComptimeValue comptime_eval_expression(ComptimeInterpreter* interp, ASTNode* expr) {
    if (!interp || !expr || interp->has_error) {
        return comptime_value_nil();
    }

    interp->stats.expressions_evaluated++;

    switch (expr->type) {
        case AST_LITERAL: {
            LiteralNode* lit = (LiteralNode*)expr;
            switch (lit->literal_type) {
                case TOKEN_INT:
                    return comptime_value_int(atoll(lit->value));
                case TOKEN_FLOAT:
                    return comptime_value_float(atof(lit->value));
                case TOKEN_STRING:
                    return comptime_value_string(lit->value);
                case TOKEN_TRUE:
                    return comptime_value_bool(true);
                case TOKEN_FALSE:
                    return comptime_value_bool(false);
                case TOKEN_NIL:
                    return comptime_value_nil();
                default:
                    return comptime_value_error("unsupported literal type");
            }
        }

        case AST_IDENTIFIER: {
            IdentifierNode* ident = (IdentifierNode*)expr;
            ComptimeValue* val = comptime_lookup_variable(interp, ident->name);
            if (!val) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Undefined variable '%s' in comptime context", ident->name);
                comptime_set_error(interp, msg, expr->pos);
                return comptime_value_error(msg);
            }
            // Return a copy
            if (val->kind == COMPTIME_STRING) {
                return comptime_value_string(val->data.string_val);
            }
            return *val;
        }

        case AST_BINARY_EXPR: {
            BinaryExprNode* bin = (BinaryExprNode*)expr;
            ComptimeValue left = comptime_eval_expression(interp, bin->left);
            if (interp->has_error) return comptime_value_nil();
            ComptimeValue right = comptime_eval_expression(interp, bin->right);
            if (interp->has_error) { comptime_value_free(&left); return comptime_value_nil(); }

            ComptimeValue result;
            bool use_float = (left.kind == COMPTIME_FLOAT || right.kind == COMPTIME_FLOAT);

            switch (bin->operator) {
                case TOKEN_PLUS:
                    if (left.kind == COMPTIME_STRING && right.kind == COMPTIME_STRING) {
                        size_t len = strlen(left.data.string_val) + strlen(right.data.string_val) + 1;
                        char* s = malloc(len);
                        snprintf(s, len, "%s%s", left.data.string_val, right.data.string_val);
                        result = (ComptimeValue){.kind = COMPTIME_STRING, .data.string_val = s};
                    } else if (use_float) {
                        result = comptime_value_float(comptime_to_float(&left) + comptime_to_float(&right));
                    } else {
                        result = comptime_value_int(comptime_to_int(&left) + comptime_to_int(&right));
                    }
                    break;
                case TOKEN_MINUS:
                    if (use_float) result = comptime_value_float(comptime_to_float(&left) - comptime_to_float(&right));
                    else result = comptime_value_int(comptime_to_int(&left) - comptime_to_int(&right));
                    break;
                case TOKEN_MULTIPLY:
                    if (use_float) result = comptime_value_float(comptime_to_float(&left) * comptime_to_float(&right));
                    else result = comptime_value_int(comptime_to_int(&left) * comptime_to_int(&right));
                    break;
                case TOKEN_DIVIDE:
                    if (use_float) {
                        double d = comptime_to_float(&right);
                        if (d == 0.0) { comptime_set_error(interp, "division by zero", expr->pos); result = comptime_value_nil(); }
                        else result = comptime_value_float(comptime_to_float(&left) / d);
                    } else {
                        int64_t d = comptime_to_int(&right);
                        if (d == 0) { comptime_set_error(interp, "division by zero", expr->pos); result = comptime_value_nil(); }
                        else result = comptime_value_int(comptime_to_int(&left) / d);
                    }
                    break;
                case TOKEN_MODULO:
                    if (is_numeric(&left) && is_numeric(&right)) {
                        int64_t d = comptime_to_int(&right);
                        if (d == 0) { comptime_set_error(interp, "modulo by zero", expr->pos); result = comptime_value_nil(); }
                        else result = comptime_value_int(comptime_to_int(&left) % d);
                    } else { result = comptime_value_error("modulo requires integers"); }
                    break;
                // Comparison operators
                case TOKEN_EQ:
                    if (use_float) result = comptime_value_bool(comptime_to_float(&left) == comptime_to_float(&right));
                    else result = comptime_value_bool(comptime_to_int(&left) == comptime_to_int(&right));
                    break;
                case TOKEN_NE:
                    if (use_float) result = comptime_value_bool(comptime_to_float(&left) != comptime_to_float(&right));
                    else result = comptime_value_bool(comptime_to_int(&left) != comptime_to_int(&right));
                    break;
                case TOKEN_LT:
                    if (use_float) result = comptime_value_bool(comptime_to_float(&left) < comptime_to_float(&right));
                    else result = comptime_value_bool(comptime_to_int(&left) < comptime_to_int(&right));
                    break;
                case TOKEN_LE:
                    if (use_float) result = comptime_value_bool(comptime_to_float(&left) <= comptime_to_float(&right));
                    else result = comptime_value_bool(comptime_to_int(&left) <= comptime_to_int(&right));
                    break;
                case TOKEN_GT:
                    if (use_float) result = comptime_value_bool(comptime_to_float(&left) > comptime_to_float(&right));
                    else result = comptime_value_bool(comptime_to_int(&left) > comptime_to_int(&right));
                    break;
                case TOKEN_GE:
                    if (use_float) result = comptime_value_bool(comptime_to_float(&left) >= comptime_to_float(&right));
                    else result = comptime_value_bool(comptime_to_int(&left) >= comptime_to_int(&right));
                    break;
                // Logical operators
                case TOKEN_AND:
                    result = comptime_value_bool(comptime_value_is_truthy(&left) && comptime_value_is_truthy(&right));
                    break;
                case TOKEN_OR:
                    result = comptime_value_bool(comptime_value_is_truthy(&left) || comptime_value_is_truthy(&right));
                    break;
                // Bitwise operators
                case TOKEN_BIT_AND:
                    result = comptime_value_int(comptime_to_int(&left) & comptime_to_int(&right));
                    break;
                case TOKEN_BIT_OR:
                    result = comptime_value_int(comptime_to_int(&left) | comptime_to_int(&right));
                    break;
                case TOKEN_BIT_XOR:
                    result = comptime_value_int(comptime_to_int(&left) ^ comptime_to_int(&right));
                    break;
                case TOKEN_LSHIFT:
                    result = comptime_value_int(comptime_to_int(&left) << comptime_to_int(&right));
                    break;
                case TOKEN_RSHIFT:
                    result = comptime_value_int(comptime_to_int(&left) >> comptime_to_int(&right));
                    break;
                default:
                    result = comptime_value_error("unsupported binary operator in comptime");
                    break;
            }

            comptime_value_free(&left);
            comptime_value_free(&right);
            return result;
        }

        case AST_UNARY_EXPR: {
            UnaryExprNode* unary = (UnaryExprNode*)expr;
            ComptimeValue operand = comptime_eval_expression(interp, unary->operand);
            if (interp->has_error) return comptime_value_nil();

            ComptimeValue result;
            switch (unary->operator) {
                case TOKEN_MINUS:
                    if (operand.kind == COMPTIME_FLOAT) result = comptime_value_float(-operand.data.float_val);
                    else result = comptime_value_int(-comptime_to_int(&operand));
                    break;
                case TOKEN_NOT:
                    result = comptime_value_bool(!comptime_value_is_truthy(&operand));
                    break;
                case TOKEN_BIT_NOT:
                    result = comptime_value_int(~comptime_to_int(&operand));
                    break;
                default:
                    result = comptime_value_error("unsupported unary operator in comptime");
                    break;
            }
            comptime_value_free(&operand);
            return result;
        }

        case AST_CALL_EXPR: {
            CallExprNode* call = (CallExprNode*)expr;
            if (!call->function || call->function->type != AST_IDENTIFIER) {
                comptime_set_error(interp, "comptime only supports direct function calls", expr->pos);
                return comptime_value_nil();
            }

            IdentifierNode* func_ident = (IdentifierNode*)call->function;
            ComptimeFunc* func = comptime_find_function(interp, func_ident->name);
            if (!func) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Unknown comptime function '%s'", func_ident->name);
                comptime_set_error(interp, msg, expr->pos);
                return comptime_value_nil();
            }

            // Check recursion limit
            if (interp->current_recursion >= interp->max_recursion) {
                comptime_set_error(interp, "comptime recursion limit exceeded", expr->pos);
                return comptime_value_nil();
            }

            // Evaluate arguments
            comptime_push_scope(interp);
            ASTNode* param = func->params;
            ASTNode* arg = call->args;
            while (param && arg) {
                VarDeclNode* var = (VarDeclNode*)param;
                ComptimeValue val = comptime_eval_expression(interp, arg);
                if (interp->has_error) { comptime_pop_scope(interp); return comptime_value_nil(); }
                comptime_set_variable(interp, var->names[0], val, false);
                param = param->next;
                arg = arg->next;
            }

            // Execute function body
            interp->current_recursion++;
            interp->stats.functions_called++;
            comptime_eval_block(interp, func->body);
            interp->current_recursion--;

            // Get return value (stored as special binding)
            ComptimeValue* ret = comptime_lookup_variable(interp, "__return__");
            ComptimeValue result = ret ? *ret : comptime_value_nil();
            if (ret && ret->kind == COMPTIME_STRING) {
                result = comptime_value_string(ret->data.string_val);
            }

            comptime_pop_scope(interp);
            return result;
        }

        default: {
            char msg[128];
            snprintf(msg, sizeof(msg), "Unsupported expression type %d in comptime", expr->type);
            comptime_set_error(interp, msg, expr->pos);
            return comptime_value_nil();
        }
    }
}

// =============================================================================
// Statement Evaluation
// =============================================================================

bool comptime_eval_statement(ComptimeInterpreter* interp, ASTNode* stmt) {
    if (!interp || !stmt || interp->has_error) return false;

    switch (stmt->type) {
        case AST_VAR_DECL: {
            VarDeclNode* var = (VarDeclNode*)stmt;
            ComptimeValue val = comptime_value_nil();
            if (var->values) {
                val = comptime_eval_expression(interp, var->values);
            }
            if (interp->has_error) return false;
            // In comptime blocks, all declarations are effectively const
            bool is_const = true;
            for (size_t i = 0; i < var->name_count; i++) {
                comptime_set_variable(interp, var->names[i], val, is_const);
            }
            interp->stats.constants_produced++;
            return true;
        }

        case AST_RETURN_STMT: {
            ReturnStmtNode* ret = (ReturnStmtNode*)stmt;
            if (ret->values) {
                ComptimeValue val = comptime_eval_expression(interp, ret->values);
                if (interp->has_error) return false;
                comptime_set_variable(interp, "__return__", val, false);
            }
            return true;
        }

        case AST_IF_STMT: {
            IfStmtNode* if_stmt = (IfStmtNode*)stmt;
            ComptimeValue cond = comptime_eval_expression(interp, if_stmt->condition);
            if (interp->has_error) return false;

            if (comptime_value_is_truthy(&cond)) {
                comptime_eval_statement(interp, if_stmt->then_stmt);
            } else if (if_stmt->else_stmt) {
                comptime_eval_statement(interp, if_stmt->else_stmt);
            }
            comptime_value_free(&cond);
            return !interp->has_error;
        }

        case AST_FOR_STMT: {
            ForStmtNode* for_stmt = (ForStmtNode*)stmt;
            comptime_push_scope(interp);

            // Init
            if (for_stmt->init) {
                comptime_eval_statement(interp, for_stmt->init);
            }

            size_t iterations = 0;
            while (!interp->has_error && iterations < interp->max_iterations) {
                // Condition
                if (for_stmt->condition) {
                    ComptimeValue cond = comptime_eval_expression(interp, for_stmt->condition);
                    if (!comptime_value_is_truthy(&cond)) {
                        comptime_value_free(&cond);
                        break;
                    }
                    comptime_value_free(&cond);
                }

                // Body
                comptime_eval_statement(interp, for_stmt->body);

                // Post
                if (for_stmt->post) {
                    comptime_eval_expression(interp, for_stmt->post);
                }

                iterations++;
            }

            if (iterations >= interp->max_iterations) {
                comptime_set_error(interp, "comptime loop iteration limit exceeded", stmt->pos);
            }

            comptime_pop_scope(interp);
            return !interp->has_error;
        }

        case AST_BLOCK_STMT: {
            return comptime_eval_block(interp, stmt);
        }

        case AST_EXPR_STMT: {
            ExprStmtNode* expr_stmt = (ExprStmtNode*)stmt;
            ComptimeValue val = comptime_eval_expression(interp, expr_stmt->expr);
            comptime_value_free(&val);
            return !interp->has_error;
        }

        default: {
            char msg[128];
            snprintf(msg, sizeof(msg), "Unsupported statement type %d in comptime", stmt->type);
            comptime_set_error(interp, msg, stmt->pos);
            return false;
        }
    }
}

bool comptime_eval_block(ComptimeInterpreter* interp, ASTNode* block) {
    if (!interp || !block || interp->has_error) return false;

    if (block->type != AST_BLOCK_STMT) {
        return comptime_eval_statement(interp, block);
    }

    BlockStmtNode* blk = (BlockStmtNode*)block;
    comptime_push_scope(interp);

    ASTNode* stmt = blk->statements;
    while (stmt && !interp->has_error) {
        comptime_eval_statement(interp, stmt);
        stmt = stmt->next;
    }

    comptime_pop_scope(interp);
    return !interp->has_error;
}

// =============================================================================
// Entry Point — Process a comptime block
// =============================================================================

bool comptime_process_block(ComptimeInterpreter* interp, ComptimeBlockNode* block) {
    if (!interp || !block) return false;
    return comptime_eval_block(interp, block->body);
}
