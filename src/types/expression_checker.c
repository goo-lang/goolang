#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Expression type checking implementation

Type* type_check_expression(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr) return NULL;
    
    switch (expr->type) {
        case AST_IDENTIFIER:
            return type_check_identifier(checker, expr);
        case AST_LITERAL:
            return type_check_literal(checker, expr);
        case AST_BINARY_EXPR:
            return type_check_binary_expr(checker, expr);
        case AST_UNARY_EXPR:
            return type_check_unary_expr(checker, expr);
        case AST_CALL_EXPR:
            return type_check_call_expr(checker, expr);
        case AST_INDEX_EXPR:
            return type_check_index_expr(checker, expr);
        case AST_SELECTOR_EXPR:
            return type_check_selector_expr(checker, expr);
        case AST_TRY_EXPR:
            return type_check_try_expr(checker, expr);
        case AST_CATCH_EXPR:
            return type_check_catch_expr(checker, expr);
        case AST_PAREN_EXPR: {
            // MapLitNode — `map[K]V{ … }`. Type is just TYPE_MAP(K,V).
            MapLitNode* lit = (MapLitNode*)expr;
            Type* mt = type_from_ast(checker, lit->map_type);
            if (!mt) return NULL;
            // Type-check each key + value, but don't strict-check —
            // M8-map-literal only formally validates the declared
            // map type. Runtime is string→int specifically.
            for (ASTNode* k = lit->keys; k; k = k->next) {
                if (!type_check_expression(checker, k)) return NULL;
            }
            for (ASTNode* v = lit->values; v; v = v->next) {
                if (!type_check_expression(checker, v)) return NULL;
            }
            expr->node_type = mt;
            return mt;
        }
        case AST_SLICE_EXPR: {
            // SliceLitNode — `[1, 2, 3]`. Element type inferred from
            // the first element; subsequent elements must match.
            SliceLitNode* lit = (SliceLitNode*)expr;
            if (!lit->elements) {
                // Empty slice — element type defaults to int32 until
                // context-based inference is wired up. Suitable for
                // M8-scope probe coverage.
                Type* def = type_checker_get_builtin(checker, TYPE_INT32);
                Type* st = type_slice(def);
                expr->node_type = st;
                return st;
            }
            Type* elem_type = NULL;
            for (ASTNode* e = lit->elements; e; e = e->next) {
                Type* et = type_check_expression(checker, e);
                if (!et) return NULL;
                if (!elem_type) elem_type = et;
                // (Skip strict element-type check for now — Goo's
                //  type_compatible interactions with int literals
                //  need more care; mismatches will surface in codegen
                //  if they're real.)
            }
            Type* st = type_slice(elem_type);
            expr->node_type = st;
            return st;
        }
        default:
            type_error(checker, expr->pos, "Unknown expression type");
            return NULL;
    }
}

Type* type_check_identifier(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_IDENTIFIER) return NULL;

    IdentifierNode* ident = (IdentifierNode*)expr;
    Variable* var = type_checker_lookup_variable(checker, ident->name);

    if (!var) {
        type_error(checker, expr->pos, "Undefined variable '%s'", ident->name);
        return NULL;
    }
    
    // Check if variable has been moved (ownership tracking)
    if (var->is_moved) {
        type_error(checker, expr->pos, 
                  "Use of moved variable '%s' (moved at %s:%d:%d)",
                  ident->name,
                  var->declared_pos.filename ? var->declared_pos.filename : "<unknown>",
                  var->declared_pos.line, var->declared_pos.column);
        return NULL;
    }
    
    // Check if variable is initialized
    if (!var->is_initialized) {
        type_error(checker, expr->pos, "Use of uninitialized variable '%s'", ident->name);
        return NULL;
    }
    
    // Store the resolved type in the AST node for later use
    expr->node_type = var->type;
    
    return var->type;
}

Type* type_check_literal(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_LITERAL) return NULL;
    
    LiteralNode* lit = (LiteralNode*)expr;
    Type* type = NULL;
    
    switch (lit->literal_type) {
        case TOKEN_INT:
            // TODO: Better integer type inference based on value
            type = type_checker_get_builtin(checker, TYPE_INT32);
            break;
        case TOKEN_FLOAT:
            type = type_checker_get_builtin(checker, TYPE_FLOAT64);
            break;
        case TOKEN_STRING:
            type = type_checker_get_builtin(checker, TYPE_STRING);
            break;
        case TOKEN_CHAR:
            type = type_checker_get_builtin(checker, TYPE_CHAR);
            break;
        case TOKEN_TRUE:
        case TOKEN_FALSE:
            type = type_checker_get_builtin(checker, TYPE_BOOL);
            break;
        case TOKEN_NIL:
            // nil has special type that can be assigned to any nullable type
            type = type_new(TYPE_UNKNOWN);  // Special nil type
            if (type) {
                type->name = strdup("nil");
            }
            break;
        default:
            type_error(checker, expr->pos, "Unknown literal type");
            return NULL;
    }
    
    expr->node_type = type;
    return type;
}

Type* type_check_binary_expr(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_BINARY_EXPR) return NULL;
    
    BinaryExprNode* binary = (BinaryExprNode*)expr;
    
    Type* left_type = type_check_expression(checker, binary->left);
    Type* right_type = type_check_expression(checker, binary->right);
    
    if (!left_type || !right_type) return NULL;
    
    Type* result_type = NULL;
    
    switch (binary->operator) {
        // Arithmetic operators
        case TOKEN_PLUS:
        case TOKEN_MINUS:
        case TOKEN_MULTIPLY:
        case TOKEN_DIVIDE:
        case TOKEN_MODULO:
            result_type = type_check_arithmetic_op(checker, left_type, right_type, binary->operator, expr->pos);
            break;
            
        // Comparison operators
        case TOKEN_EQ:
        case TOKEN_NE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_GT:
        case TOKEN_GE:
            result_type = type_check_comparison_op(checker, left_type, right_type, binary->operator, expr->pos);
            break;
            
        // Logical operators
        case TOKEN_AND:
        case TOKEN_OR:
            result_type = type_check_logical_op(checker, left_type, right_type, binary->operator, expr->pos);
            break;
            
        // Bitwise operators
        case TOKEN_BIT_AND:
        case TOKEN_BIT_OR:
        case TOKEN_BIT_XOR:
        case TOKEN_LSHIFT:
        case TOKEN_RSHIFT:
            result_type = type_check_bitwise_op(checker, left_type, right_type, binary->operator, expr->pos);
            break;
            
        // Assignment operators
        case TOKEN_ASSIGN:
            result_type = type_check_assignment_op(checker, binary->left, left_type, right_type, expr->pos);
            break;
            
        // Channel send operator
        case TOKEN_ARROW:  // ch <- value
            result_type = type_check_channel_send_op(checker, left_type, right_type, expr->pos);
            break;
            
        default:
            type_error(checker, expr->pos, "Unknown binary operator");
            return NULL;
    }
    
    expr->node_type = result_type;
    return result_type;
}

Type* type_check_unary_expr(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_UNARY_EXPR) return NULL;
    
    UnaryExprNode* unary = (UnaryExprNode*)expr;
    Type* operand_type = type_check_expression(checker, unary->operand);
    
    if (!operand_type) return NULL;
    
    Type* result_type = NULL;
    
    switch (unary->operator) {
        case TOKEN_MINUS:
        case TOKEN_PLUS:
            if (!type_is_numeric(operand_type)) {
                type_error(checker, expr->pos, 
                          "Unary %s requires numeric type, got %s",
                          (unary->operator == TOKEN_MINUS) ? "-" : "+",
                          type_to_string(operand_type));
                return NULL;
            }
            result_type = operand_type;
            break;
            
        case TOKEN_NOT:
            if (operand_type->kind != TYPE_BOOL) {
                type_error(checker, expr->pos, 
                          "Logical not requires boolean type, got %s",
                          type_to_string(operand_type));
                return NULL;
            }
            result_type = operand_type;
            break;
            
        case TOKEN_BIT_NOT:
            if (!type_is_integer(operand_type)) {
                type_error(checker, expr->pos, 
                          "Bitwise not requires integer type, got %s",
                          type_to_string(operand_type));
                return NULL;
            }
            result_type = operand_type;
            break;
            
        case TOKEN_BIT_AND:  // & - take reference/borrow
            // Check if we can borrow this value
            if (unary->operand->type == AST_IDENTIFIER) {
                IdentifierNode* ident = (IdentifierNode*)unary->operand;
                Variable* var = type_checker_lookup_variable(checker, ident->name);
                if (var) {
                    // Check if variable is moved
                    if (var->is_moved) {
                        type_error(checker, expr->pos,
                                  "Cannot borrow moved variable '%s'", ident->name);
                        return NULL;
                    }
                    // Mark as borrowed
                    var->is_borrowed = 1;
                    var->borrow_count++;
                }
            }
            result_type = type_reference(operand_type, 0);  // immutable reference by default
            break;
            
        case TOKEN_MULTIPLY:  // * - dereference
            if (operand_type->kind == TYPE_POINTER) {
                result_type = operand_type->data.pointer.pointee_type;
            } else if (operand_type->kind == TYPE_REFERENCE) {
                result_type = operand_type->data.reference.referenced_type;
            } else {
                type_error(checker, expr->pos,
                          "Cannot dereference non-pointer/reference type %s",
                          type_to_string(operand_type));
                return NULL;
            }
            break;
            
        case TOKEN_ARROW:  // <-ch (channel receive)
            result_type = type_check_channel_receive_op(checker, operand_type, expr->pos);
            break;
            
        default:
            type_error(checker, expr->pos, "Unknown unary operator");
            return NULL;
    }
    
    expr->node_type = result_type;
    return result_type;
}

Type* type_check_make_chan_call(TypeChecker* checker, CallExprNode* call, ASTNode* expr) {
    if (!checker || !call || !call->args) {
        type_error(checker, expr->pos, "make_chan requires type and capacity arguments");
        return NULL;
    }
    
    // First argument should be a type
    ASTNode* type_arg = call->args;
    Type* element_type = type_from_ast(checker, type_arg);
    if (!element_type) {
        type_error(checker, type_arg->pos, "Invalid type in make_chan");
        return NULL;
    }
    
    // Second argument should be capacity (optional)
    if (type_arg->next) {
        Type* capacity_type = type_check_expression(checker, type_arg->next);
        if (!capacity_type || !type_is_integer(capacity_type)) {
            type_error(checker, type_arg->next->pos, "Channel capacity must be an integer");
            return NULL;
        }
    }
    
    // Create and return channel type
    Type* chan_type = type_channel(element_type, CHAN_PATTERN_BASIC);
    expr->node_type = chan_type;
    return chan_type;
}

Type* type_check_call_expr(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_CALL_EXPR) return NULL;
    
    CallExprNode* call = (CallExprNode*)expr;
    
    // Special handling for make_chan
    if (call->function && call->function->type == AST_IDENTIFIER) {
        IdentifierNode* func_ident = (IdentifierNode*)call->function;
        if (strcmp(func_ident->name, "make_chan") == 0) {
            return type_check_make_chan_call(checker, call, expr);
        }
    }
    
    // Check function expression
    Type* func_type = type_check_expression(checker, call->function);
    if (!func_type) return NULL;
    
    if (func_type->kind != TYPE_FUNCTION) {
        type_error(checker, expr->pos, 
                  "Cannot call non-function type %s", type_to_string(func_type));
        return NULL;
    }
    
    // Check arguments
    ASTNode* arg = call->args;
    size_t arg_count __attribute__((unused)) = 0;
    while (arg) {
        Type* arg_type = type_check_expression(checker, arg);
        if (!arg_type) return NULL;
        
        // TODO: Check argument type compatibility with function parameters
        
        arg_count++;
        arg = arg->next;
    }
    
    // TODO: Check argument count against function parameters
    
    expr->node_type = func_type->data.function.return_type;
    return func_type->data.function.return_type;
}

Type* type_check_index_expr(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_INDEX_EXPR) return NULL;
    
    IndexExprNode* index = (IndexExprNode*)expr;
    
    Type* expr_type = type_check_expression(checker, index->expr);
    Type* index_type = type_check_expression(checker, index->index);
    
    if (!expr_type || !index_type) return NULL;
    
    // Check index type — integer for array/slice; key type for map.
    if (expr_type->kind == TYPE_MAP) {
        // Trust the key matches the declared map key type. Strict
        // validation can come later (string is the only key type
        // the M8 runtime supports anyway).
        Type* vt = expr_type->data.map.value_type;
        expr->node_type = vt;
        return vt;
    }
    if (!type_is_integer(index_type)) {
        type_error(checker, index->index->pos,
                  "Array index must be integer, got %s", type_to_string(index_type));
        return NULL;
    }
    
    Type* element_type = NULL;
    
    switch (expr_type->kind) {
        case TYPE_ARRAY:
            element_type = expr_type->data.array.element_type;
            break;
        case TYPE_SLICE:
            element_type = expr_type->data.slice.element_type;
            break;
        case TYPE_MAP:
            // For maps, check if index type is compatible with key type
            if (!type_compatible(index_type, expr_type->data.map.key_type)) {
                type_error(checker, index->index->pos,
                          "Map key type mismatch: expected %s, got %s",
                          type_to_string(expr_type->data.map.key_type),
                          type_to_string(index_type));
                return NULL;
            }
            element_type = expr_type->data.map.value_type;
            break;
        default:
            type_error(checker, index->expr->pos, 
                      "Cannot index type %s", type_to_string(expr_type));
            return NULL;
    }
    
    expr->node_type = element_type;
    return element_type;
}

// stdlib_package_lookup returns a function Type for a (package, name) pair
// drawn from the four hardcoded stdlib packages. Returns NULL if the pair
// isn't known. This is a deliberate shortcut for M7-stdlib-expansion: the
// type checker doesn't yet load stdlib/*.goo files, so we hand it the
// minimum surface needed to type-check fmt.Println etc.
static Type* stdlib_package_lookup(TypeChecker* checker,
                                   const char* package,
                                   const char* name) {
    if (!checker || !package || !name) return NULL;
    Type* void_t = type_checker_get_builtin(checker, TYPE_VOID);

    // fmt.Println(string) -> void  (one-arg-only stub; full variadic comes later)
    if (strcmp(package, "fmt") == 0 && strcmp(name, "Println") == 0) {
        return type_function(NULL, 0, void_t);
    }

    // os.Exit(int) -> void
    if (strcmp(package, "os") == 0 && strcmp(name, "Exit") == 0) {
        return type_function(NULL, 0, void_t);
    }

    // math.Sqrt(float64) -> float64
    if (strcmp(package, "math") == 0 && strcmp(name, "Sqrt") == 0) {
        Type* float_t = type_checker_get_builtin(checker, TYPE_FLOAT64);
        return type_function(NULL, 0, float_t);
    }

    // strings.Contains(string, string) -> bool
    if (strcmp(package, "strings") == 0 && strcmp(name, "Contains") == 0) {
        Type* bool_t = type_checker_get_builtin(checker, TYPE_BOOL);
        return type_function(NULL, 0, bool_t);
    }

    return NULL;
}

Type* type_check_selector_expr(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_SELECTOR_EXPR) return NULL;

    SelectorExprNode* selector = (SelectorExprNode*)expr;

    Type* expr_type = type_check_expression(checker, selector->expr);
    if (!expr_type) return NULL;

    // Package member access: when the left side is an imported package
    // identifier, resolve the selector against the stdlib symbol table.
    if (expr_type->kind == TYPE_PACKAGE && selector->expr->type == AST_IDENTIFIER) {
        IdentifierNode* pkg_ident = (IdentifierNode*)selector->expr;
        Type* fn_type = stdlib_package_lookup(checker, pkg_ident->name, selector->selector);
        if (fn_type) {
            expr->node_type = fn_type;
            return fn_type;
        }
        type_error(checker, expr->pos, "Package '%s' has no member '%s'",
                   pkg_ident->name, selector->selector);
        return NULL;
    }

    // Struct field access (also covers *Struct via the codegen layer
    // which dereferences pointers automatically). Walk the fields of
    // the resolved struct Type until we find a matching name.
    Type* struct_type = expr_type;
    if (struct_type->kind == TYPE_POINTER &&
        struct_type->data.pointer.pointee_type &&
        struct_type->data.pointer.pointee_type->kind == TYPE_STRUCT) {
        struct_type = struct_type->data.pointer.pointee_type;
    }
    if (struct_type->kind == TYPE_STRUCT) {
        for (size_t i = 0; i < struct_type->data.struct_type.field_count; i++) {
            StructField* f = &struct_type->data.struct_type.fields[i];
            if (f->name && strcmp(f->name, selector->selector) == 0) {
                expr->node_type = f->type;
                return f->type;
            }
        }
        type_error(checker, expr->pos, "Struct has no field '%s'", selector->selector);
        return NULL;
    }

    type_error(checker, expr->pos, "Selector on non-struct, non-package type");
    return NULL;
}

Type* type_check_try_expr(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_TRY_EXPR) return NULL;
    
    TryExprNode* try_expr = (TryExprNode*)expr;
    
    Type* expr_type = type_check_expression(checker, try_expr->expr);
    if (!expr_type) return NULL;
    
    // Expression must be an error union
    if (!type_is_error_union(expr_type)) {
        type_error(checker, expr->pos, 
                  "try can only be used with error union types, got %s",
                  type_to_string(expr_type));
        return NULL;
    }
    
    // try extracts the value type from the error union
    Type* value_type = expr_type->data.error_union.value_type;
    expr->node_type = value_type;
    return value_type;
}

Type* type_check_catch_expr(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_CATCH_EXPR) return NULL;
    
    CatchExprNode* catch_expr = (CatchExprNode*)expr;
    
    Type* expr_type = type_check_expression(checker, catch_expr->expr);
    if (!expr_type) return NULL;
    
    // Expression must be an error union
    if (!type_is_error_union(expr_type)) {
        type_error(checker, expr->pos,
                  "catch can only be used with error union types, got %s",
                  type_to_string(expr_type));
        return NULL;
    }
    
    // TODO: Add error variable to scope and check catch body
    Type* catch_body_type __attribute__((unused)) = NULL;
    if (catch_expr->catch_body) {
        scope_push(checker);
        
        // Add error variable to scope
        if (catch_expr->error_var) {
            Type* error_type = expr_type->data.error_union.error_type;
            if (!error_type) {
                error_type = type_checker_get_builtin(checker, TYPE_STRING);  // Default error type
            }
            
            Variable* error_var = variable_new(catch_expr->error_var, error_type, expr->pos);
            if (error_var) {
                scope_add_variable(checker->current_scope, error_var);
            }
        }
        
        catch_body_type = type_check_expression(checker, catch_expr->catch_body);
        scope_pop(checker);
    }
    
    // The type of a catch expression is the value type of the error union
    Type* value_type = expr_type->data.error_union.value_type;
    expr->node_type = value_type;
    return value_type;
}

// Channel operation type checking
Type* type_check_channel_send_op(TypeChecker* checker, Type* channel_type, Type* value_type, Position pos) {
    if (!checker || !channel_type || !value_type) return NULL;
    
    // Left operand must be a channel
    if (channel_type->kind != TYPE_CHANNEL) {
        type_error(checker, pos, "Cannot send to non-channel type %s", type_to_string(channel_type));
        return NULL;
    }
    
    // Check if value type is compatible with channel element type
    Type* element_type = channel_type->data.channel.element_type;
    if (!type_compatible(value_type, element_type)) {
        type_error(checker, pos, "Cannot send %s to channel of %s", 
                  type_to_string(value_type), type_to_string(element_type));
        return NULL;
    }
    
    // Channel send operation returns void
    return type_checker_get_builtin(checker, TYPE_VOID);
}

Type* type_check_channel_receive_op(TypeChecker* checker, Type* channel_type, Position pos) {
    if (!checker || !channel_type) return NULL;
    
    // Operand must be a channel
    if (channel_type->kind != TYPE_CHANNEL) {
        type_error(checker, pos, "Cannot receive from non-channel type %s", type_to_string(channel_type));
        return NULL;
    }
    
    // Channel receive operation returns the element type
    return channel_type->data.channel.element_type;
}

