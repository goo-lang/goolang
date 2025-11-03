#include "types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Forward declarations
Type* type_check_composite_lit(TypeChecker* checker, ASTNode* expr);

// Expression type checking implementation

Type* type_check_expression(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr) return NULL;
    
    // Debug: show what node type we received
    printf("DEBUG: Type checking expression with type %d at %s:%d:%d\n", 
           expr->type, expr->pos.filename ? expr->pos.filename : "unknown", 
           expr->pos.line, expr->pos.column);
    
    switch (expr->type) {
        case AST_IDENTIFIER:
            return type_check_identifier(checker, expr);
        case AST_LITERAL:
            return type_check_literal(checker, expr);
        case AST_COMPOSITE_LIT:
            return type_check_composite_lit(checker, expr);
        case AST_BINARY_EXPR:
            return type_check_binary_expr(checker, expr);
        case AST_UNARY_EXPR:
            return type_check_unary_expr(checker, expr);
        case AST_POSTFIX_EXPR:
            // For now, treat postfix expressions as unary expressions
            return type_check_unary_expr(checker, expr);
        case AST_CALL_EXPR:
            return type_check_call_expr(checker, expr);
        case AST_INDEX_EXPR:
            return type_check_index_expr(checker, expr);
        case AST_SELECTOR_EXPR:
            return type_check_selector_expr(checker, expr);
        case AST_SLICE_EXPR:
            // TODO: Implement proper slice expression type checking
            type_error(checker, expr->pos, "Slice expressions not yet implemented");
            return NULL;
        case AST_TYPE_ASSERT_EXPR:
            // TODO: Implement proper type assertion checking
            type_error(checker, expr->pos, "Type assertions not yet implemented");
            return NULL;
        case AST_PAREN_EXPR:
            // TODO: Implement parenthesized expressions
            type_error(checker, expr->pos, "Parenthesized expressions not yet implemented");
            return NULL;
        case AST_TRY_EXPR:
            return type_check_try_expr(checker, expr);
        case AST_CATCH_EXPR:
            return type_check_catch_expr(checker, expr);
        default:
            type_error(checker, expr->pos, "Unknown expression type %d (at %s:%d:%d)", 
                      expr->type, expr->pos.filename, expr->pos.line, expr->pos.column);
            return NULL;
    }
}

Type* type_check_identifier(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_IDENTIFIER) return NULL;

    IdentifierNode* ident = (IdentifierNode*)expr;

    // Check for builtin functions - these don't need to be declared
    if (strcmp(ident->name, "make") == 0 ||
        strcmp(ident->name, "len") == 0 ||
        strcmp(ident->name, "cap") == 0 ||
        strcmp(ident->name, "append") == 0 ||
        strcmp(ident->name, "make_chan") == 0 ||
        strcmp(ident->name, "goo_printf") == 0) {
        // Return a function type for builtins (generic function)
        expr->node_type = type_checker_get_builtin(checker, TYPE_FUNCTION);
        return expr->node_type;
    }

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
    
    // Special handling for built-in functions
    if (call->function && call->function->type == AST_IDENTIFIER) {
        IdentifierNode* func_ident = (IdentifierNode*)call->function;

        // Handle make_chan built-in
        if (strcmp(func_ident->name, "make_chan") == 0) {
            return type_check_make_chan_call(checker, call, expr);
        }

        // Handle len() built-in
        if (strcmp(func_ident->name, "len") == 0) {
            // len() takes one argument (array, slice, string, or map)
            if (!call->args) {
                type_error(checker, expr->pos, "len() requires one argument");
                return NULL;
            }
            if (call->args->next) {
                type_error(checker, expr->pos, "len() takes exactly one argument");
                return NULL;
            }

            // Check the argument type
            Type* arg_type = type_check_expression(checker, call->args);
            if (!arg_type) return NULL;

            // len() works on arrays, slices, strings, and maps
            if (arg_type->kind != TYPE_ARRAY &&
                arg_type->kind != TYPE_SLICE &&
                arg_type->kind != TYPE_STRING &&
                arg_type->kind != TYPE_MAP) {
                type_error(checker, call->args->pos,
                          "len() argument must be array, slice, string, or map, got %s",
                          type_to_string(arg_type));
                return NULL;
            }

            // len() returns int
            Type* int_type = type_checker_get_builtin(checker, TYPE_INT32);
            expr->node_type = int_type;
            return int_type;
        }
    }

    // Special handling for method calls (selector.method())
    if (call->function && call->function->type == AST_SELECTOR_EXPR) {
        SelectorExprNode* selector = (SelectorExprNode*)call->function;

        // Get the type of the object
        Type* obj_type = type_check_expression(checker, selector->expr);
        if (!obj_type) return NULL;

        // Get the struct type name (handle both value and pointer receivers)
        const char* type_name = NULL;
        if (obj_type->kind == TYPE_STRUCT) {
            type_name = obj_type->data.struct_type.name;
        } else if (obj_type->kind == TYPE_POINTER &&
                   obj_type->data.pointer.pointee_type &&
                   obj_type->data.pointer.pointee_type->kind == TYPE_STRUCT) {
            type_name = obj_type->data.pointer.pointee_type->data.struct_type.name;
        }

        if (type_name) {
            // Build mangled method name (TypeName_methodName)
            size_t len = strlen(type_name) + strlen(selector->selector) + 2;
            char* mangled_name = malloc(len);
            snprintf(mangled_name, len, "%s_%s", type_name, selector->selector);

            // Look up the method
            Variable* method = type_checker_lookup_variable(checker, mangled_name);
            free(mangled_name);

            if (method && method->type && method->type->kind == TYPE_FUNCTION) {
                // It's a method call - return the return type
                // TODO: Validate argument types
                Type* return_type = method->type->data.function.return_type;
                expr->node_type = return_type;
                return return_type;
            }
        }
        // If not a method, fall through to regular selector type checking
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
    
    // Check index type
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

Type* type_check_selector_expr(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_SELECTOR_EXPR) return NULL;

    SelectorExprNode* selector = (SelectorExprNode*)expr;

    Type* expr_type = type_check_expression(checker, selector->expr);
    if (!expr_type) return NULL;

    // Handle struct field access and methods
    if (expr_type->kind == TYPE_STRUCT) {
        // First, look up the field in the struct
        for (size_t i = 0; i < expr_type->data.struct_type.field_count; i++) {
            if (strcmp(expr_type->data.struct_type.fields[i].name, selector->selector) == 0) {
                Type* field_type = expr_type->data.struct_type.fields[i].type;
                expr->node_type = field_type;
                return field_type;
            }
        }

        // If not a field, check for a method
        // Build mangled method name: TypeName_methodName
        const char* type_name = expr_type->data.struct_type.name;
        size_t mangled_len = strlen(type_name) + strlen(selector->selector) + 2;
        char* mangled_name = malloc(mangled_len);
        if (mangled_name) {
            snprintf(mangled_name, mangled_len, "%s_%s", type_name, selector->selector);
            Variable* method = type_checker_lookup_variable(checker, mangled_name);
            free(mangled_name);

            if (method && method->type && method->type->kind == TYPE_FUNCTION) {
                expr->node_type = method->type;
                return method->type;
            }
        }

        // Neither field nor method found
        type_error(checker, expr->pos,
                  "Field '%s' not found in struct",
                  selector->selector);
        return NULL;
    }

    // Handle pointer to struct (for pointer receiver methods)
    if (expr_type->kind == TYPE_POINTER &&
        expr_type->data.pointer.pointee_type &&
        expr_type->data.pointer.pointee_type->kind == TYPE_STRUCT) {

        Type* struct_type = expr_type->data.pointer.pointee_type;

        // First, look up the field in the struct (will auto-deref pointer)
        for (size_t i = 0; i < struct_type->data.struct_type.field_count; i++) {
            if (strcmp(struct_type->data.struct_type.fields[i].name, selector->selector) == 0) {
                Type* field_type = struct_type->data.struct_type.fields[i].type;
                expr->node_type = field_type;
                return field_type;
            }
        }

        // If not a field, check for a method (pointer receiver)
        const char* type_name = struct_type->data.struct_type.name;
        size_t mangled_len = strlen(type_name) + strlen(selector->selector) + 2;
        char* mangled_name = malloc(mangled_len);
        if (mangled_name) {
            snprintf(mangled_name, mangled_len, "%s_%s", type_name, selector->selector);
            Variable* method = type_checker_lookup_variable(checker, mangled_name);
            free(mangled_name);

            if (method && method->type && method->type->kind == TYPE_FUNCTION) {
                expr->node_type = method->type;
                return method->type;
            }
        }

        // Neither field nor method found
        type_error(checker, expr->pos,
                  "Field '%s' not found in struct",
                  selector->selector);
        return NULL;
    }

    // TODO: Handle package selectors (e.g., fmt.Println)
    // TODO: Handle interface method selectors

    type_error(checker, expr->pos,
              "Cannot access field '%s' on type %s",
              selector->selector,
              type_to_string(expr_type));
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

// Type check composite literal (struct literal)
Type* type_check_composite_lit(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_COMPOSITE_LIT) return NULL;

    CompositeLitNode* comp = (CompositeLitNode*)expr;

    // Get the struct type being initialized
    Type* struct_type = type_from_ast(checker, comp->type);
    if (!struct_type) {
        return NULL;
    }

    if (struct_type->kind != TYPE_STRUCT) {
        type_error(checker, expr->pos, "Composite literal can only be used with struct types");
        return NULL;
    }

    // Type check each field initialization
    for (size_t i = 0; i < comp->field_count; i++) {
        const char* field_name = comp->field_names[i];
        ASTNode* field_value = comp->field_values[i];

        // Find the field in the struct type
        StructField* field = NULL;
        for (size_t j = 0; j < struct_type->data.struct_type.field_count; j++) {
            if (strcmp(struct_type->data.struct_type.fields[j].name, field_name) == 0) {
                field = &struct_type->data.struct_type.fields[j];
                break;
            }
        }

        if (!field) {
            type_error(checker, expr->pos, "Struct %s has no field named %s",
                      struct_type->data.struct_type.name, field_name);
            return NULL;
        }

        // Type check the field value
        Type* value_type = type_check_expression(checker, field_value);
        if (!value_type) {
            return NULL;
        }

        // Check that the value type matches the field type
        if (!type_equals(value_type, field->type)) {
            type_error(checker, field_value->pos,
                      "Cannot assign value of type %s to field %s of type %s",
                      type_to_string(value_type), field_name, type_to_string(field->type));
            return NULL;
        }
    }

    expr->node_type = struct_type;
    return struct_type;
}
