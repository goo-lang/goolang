#include "types.h"
#include "comptime.h"  // comptime_context_lookup_func: order-independent func registry
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
        case AST_SLICE_INDEX_EXPR:
            return type_check_slice_index_expr(checker, expr);
        case AST_SELECTOR_EXPR:
            return type_check_selector_expr(checker, expr);
        case AST_TRY_EXPR:
            return type_check_try_expr(checker, expr);
        case AST_CATCH_EXPR:
            return type_check_catch_expr(checker, expr);
        case AST_POSTFIX_EXPR: {
            // `j++` / `j--` — type is the operand's type. Operand must
            // be an integer-valued lvalue; codegen does the actual
            // load/inc/store.
            PostfixExprNode* p = (PostfixExprNode*)expr;
            Type* t = type_check_expression(checker, p->operand);
            if (!t) return NULL;
            expr->node_type = t;
            return t;
        }
        case AST_PAREN_EXPR: {
            // MapLitNode — `map[K]V{ … }`. Type is just TYPE_MAP(K,V).
            MapLitNode* lit = (MapLitNode*)expr;
            Type* mt = type_from_ast(checker, lit->map_type);
            if (!mt || mt->kind != TYPE_MAP) return NULL;
            // Check every key against the declared key type K and every value
            // against the declared value type V, so a wrong-typed entry (e.g.
            // map[string]int{"a": "notint"}) is rejected here with a clean
            // type error instead of leaking to an opaque LLVM failure. (P3-1)
            Type* want_key = mt->data.map.key_type;
            Type* want_val = mt->data.map.value_type;
            size_t ki = 0;
            for (ASTNode* k = lit->keys; k; k = k->next, ki++) {
                Type* kt = type_check_expression(checker, k);
                if (!kt) return NULL;
                if (!type_compatible(kt, want_key)) {
                    type_error(checker, k->pos,
                               "Map literal key %zu type '%s' is not compatible "
                               "with declared key type '%s'",
                               ki, type_to_string(kt), type_to_string(want_key));
                    return NULL;
                }
            }
            size_t vi = 0;
            for (ASTNode* v = lit->values; v; v = v->next, vi++) {
                Type* vt = type_check_expression(checker, v);
                if (!vt) return NULL;
                if (!type_compatible(vt, want_val)) {
                    type_error(checker, v->pos,
                               "Map literal value %zu type '%s' is not compatible "
                               "with declared value type '%s'",
                               vi, type_to_string(vt), type_to_string(want_val));
                    return NULL;
                }
            }
            expr->node_type = mt;
            return mt;
        }
        case AST_SLICE_EXPR: {
            SliceLitNode* lit = (SliceLitNode*)expr;

            // Go-standard typed literal `[]T{...}`: the declared slice type is
            // stored on the node. Check every element against the declared
            // element type T and stamp the literal with the declared type —
            // this is the type-check the form is named for, and it makes the
            // inferred type correct even for an empty `[]T{}` (which would
            // otherwise default to []int32). (P3-1)
            if (lit->elem_type) {
                Type* declared = type_from_ast(checker, lit->elem_type);
                if (!declared || declared->kind != TYPE_SLICE) return NULL;
                Type* want = declared->data.slice.element_type;
                if (!want) return NULL;

                // The lowering (codegen_generate_slice_lit) now coerces each
                // element to the declared element width (SExt/Trunc/SIToFP via
                // slice_coerce_elem), so the general []T{} case lowers —
                // int64/uint/float64/bool, not just the natural-width
                // i32/string forms. Element compatibility is still enforced
                // per-element below (incl. the lossy float->int rejection).
                size_t i = 0;
                for (ASTNode* e = lit->elements; e; e = e->next, i++) {
                    Type* et = type_check_expression(checker, e);
                    if (!et) return NULL;
                    // A float element in an integer slice would silently
                    // truncate (`[]int{1, 2.5, 3}` -> 1 0 3) — type_compatible
                    // wrongly permits it as a numeric conversion. Reject the
                    // lossy float->int case explicitly so it can't miscompile.
                    if (type_is_integer(want)
                        && (et->kind == TYPE_FLOAT32 || et->kind == TYPE_FLOAT64)) {
                        type_error(checker, e->pos,
                                   "Slice literal element %zu: cannot use float "
                                   "value in a '%s' slice (would truncate)",
                                   i, type_to_string(want));
                        return NULL;
                    }
                    // An interface element type accepts any concrete implementer
                    // (boxed at codegen); check_interface_assign emits its own
                    // "does not implement" diagnostic.
                    if (want->kind == TYPE_INTERFACE) {
                        if (!check_interface_assign(checker, et, want, e->pos)) {
                            return NULL;
                        }
                    } else if (!type_compatible(et, want)) {
                        // type_compatible permits numeric widening (so
                        // []int64{1, 2} is fine) but rejects e.g. string vs int.
                        type_error(checker, e->pos,
                                   "Slice literal element %zu type '%s' is not "
                                   "compatible with declared element type '%s'",
                                   i, type_to_string(et), type_to_string(want));
                        return NULL;
                    }
                }
                expr->node_type = declared;
                return declared;
            }

            // Goo-native untyped form `[1, 2, 3]`: element type inferred from
            // the first element; subsequent elements must be compatible.
            if (!lit->elements) {
                // Empty untyped slice — element type defaults to int32 (no
                // declared type and no elements to infer from).
                Type* def = type_checker_get_builtin(checker, TYPE_INT32);
                Type* st = type_slice(def);
                expr->node_type = st;
                return st;
            }
            Type* elem_type = NULL;
            size_t i = 0;
            for (ASTNode* e = lit->elements; e; e = e->next, i++) {
                Type* et = type_check_expression(checker, e);
                if (!et) return NULL;
                if (!elem_type) {
                    elem_type = et;
                } else if (!type_compatible(et, elem_type)) {
                    type_error(checker, e->pos,
                               "Slice literal element %zu type '%s' is not "
                               "compatible with element type '%s'",
                               i, type_to_string(et), type_to_string(elem_type));
                    return NULL;
                }
            }
            Type* st = type_slice(elem_type);
            expr->node_type = st;
            return st;
        }
        case AST_STRUCT_LITERAL:
            return type_check_struct_literal(checker, expr);
        case AST_MATCH_EXPR:
            return type_check_match_expr(checker, expr);
        default:
            type_error(checker, expr->pos, "Unknown expression type");
            return NULL;
    }
}

// `Point{x: 3, y: 4}` / `Point{3, 4}`. Omitted keyed fields take their
// zero value (Go semantics — matches the zero-initializing alloca that
// `var p Point` already gets in codegen); positional form must cover
// every declared field.
Type* type_check_struct_literal(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_STRUCT_LITERAL) return NULL;

    StructLiteralNode* lit = (StructLiteralNode*)expr;

    // Named types are registered in the variable scope by
    // type_check_type_decl (the Variable's type IS the named Type).
    Variable* named = type_checker_lookup_variable(checker, lit->type_name);
    if (!named || !named->type) {
        type_error(checker, expr->pos, "Unknown type '%s' in struct literal",
                   lit->type_name);
        return NULL;
    }
    Type* named_type = named->type;

    // For enum variant construction (e.g. `Circle{radius: 5}` where Circle is
    // a variant of enum Shape): resolve the variant and use its payload struct
    // for the field-checking block below. The returned/stamped type is the ENUM,
    // not the payload, so the literal is assignable to an enum-typed variable.
    // Codegen recovers the variant by searching node_type->data.enum_type.variants
    // for lit->type_name.
    Type* enum_type = NULL;
    if (named_type->kind == TYPE_ENUM) {
        EnumVariant* variant = NULL;
        for (size_t i = 0; i < named_type->data.enum_type.variant_count; i++) {
            if (strcmp(named_type->data.enum_type.variants[i].name, lit->type_name) == 0) {
                variant = &named_type->data.enum_type.variants[i];
                break;
            }
        }
        if (!variant) {
            type_error(checker, expr->pos, "'%s' is not a variant of enum '%s'",
                       lit->type_name, named_type->data.enum_type.name);
            return NULL;
        }
        // Redirect struct_type to the variant's payload so the existing
        // keyed/positional checking block runs against the payload fields.
        enum_type = named_type;
        named_type = variant->payload;
    }

    Type* struct_type = named_type;

    // Named slice composite literal: `type IntSlice []int; IntSlice{3, 1, 2}`
    // Validates field_values as elements of the underlying element type and
    // stamps the named TYPE_SLICE as the expression's type. Codegen handles
    // lowering via the slice path (see codegen_generate_struct_lit).
    if (struct_type->kind == TYPE_SLICE) {
        Type* want = struct_type->data.slice.element_type;
        if (!want) {
            type_error(checker, expr->pos,
                       "Named slice type '%s' missing element type", lit->type_name);
            return NULL;
        }
        size_t i = 0;
        for (ASTNode* e = lit->field_values; e; e = e->next, i++) {
            Type* et = type_check_expression(checker, e);
            if (!et) return NULL;
            // Reject the lossy float→int case explicitly (same guard as []T{}).
            if (type_is_integer(want) &&
                (et->kind == TYPE_FLOAT32 || et->kind == TYPE_FLOAT64)) {
                type_error(checker, e->pos,
                           "Slice literal element %zu: cannot use float "
                           "value in a '%s' slice (would truncate)",
                           i, type_to_string(want));
                return NULL;
            }
            if (want->kind == TYPE_INTERFACE) {
                if (!check_interface_assign(checker, et, want, e->pos)) return NULL;
            } else if (!type_compatible(et, want)) {
                type_error(checker, e->pos,
                           "Slice literal element %zu type '%s' is not "
                           "compatible with declared element type '%s'",
                           i, type_to_string(et), type_to_string(want));
                return NULL;
            }
        }
        expr->node_type = struct_type;
        return struct_type;
    }

    if (struct_type->kind != TYPE_STRUCT) {
        type_error(checker, expr->pos,
                   "'%s' is not a struct type, cannot use composite literal",
                   lit->type_name);
        return NULL;
    }

    size_t decl_count = struct_type->data.struct_type.field_count;
    StructField* fields = struct_type->data.struct_type.fields;

    if (lit->is_keyed) {
        size_t i = 0;
        for (ASTNode* v = lit->field_values; v; v = v->next, i++) {
            const char* name = lit->field_names[i];
            if (!name) {
                type_error(checker, v->pos,
                           "Cannot mix keyed and positional initializers in '%s' literal",
                           lit->type_name);
                return NULL;
            }
            StructField* field = NULL;
            for (size_t j = 0; j < decl_count; j++) {
                if (fields[j].name && strcmp(fields[j].name, name) == 0) {
                    field = &fields[j];
                    break;
                }
            }
            if (!field) {
                type_error(checker, v->pos, "Struct '%s' has no field '%s'",
                           lit->type_name, name);
                return NULL;
            }
            for (size_t j = 0; j < i; j++) {
                if (lit->field_names[j] && strcmp(lit->field_names[j], name) == 0) {
                    type_error(checker, v->pos,
                               "Duplicate field '%s' in '%s' literal",
                               name, lit->type_name);
                    return NULL;
                }
            }
            Type* vt = type_check_expression(checker, v);
            if (!vt) return NULL;
            if (field->type && field->type->kind == TYPE_INTERFACE) {
                if (!check_interface_assign(checker, vt, field->type, v->pos)) {
                    return NULL;
                }
            } else if (!type_compatible(vt, field->type)) {
                type_error(checker, v->pos,
                           "Cannot use %s as field '%s' of type %s",
                           type_to_string(vt), name, type_to_string(field->type));
                return NULL;
            }
        }
    } else if (lit->field_count > 0) {
        if (lit->field_count != decl_count) {
            type_error(checker, expr->pos,
                       "Wrong number of initializers for '%s': got %zu, want %zu",
                       lit->type_name, lit->field_count, decl_count);
            return NULL;
        }
        size_t i = 0;
        for (ASTNode* v = lit->field_values; v; v = v->next, i++) {
            Type* vt = type_check_expression(checker, v);
            if (!vt) return NULL;
            if (fields[i].type && fields[i].type->kind == TYPE_INTERFACE) {
                if (!check_interface_assign(checker, vt, fields[i].type, v->pos)) {
                    return NULL;
                }
            } else if (!type_compatible(vt, fields[i].type)) {
                type_error(checker, v->pos,
                           "Cannot use %s as field '%s' of type %s",
                           type_to_string(vt), fields[i].name,
                           type_to_string(fields[i].type));
                return NULL;
            }
        }
    }
    // Empty literal `Point{}` — all fields zero-valued, nothing to check.

    // For enum variants, stamp the ENUM type (not the payload) so the literal
    // is assignable to an enum-typed variable. Plain struct path is unchanged.
    Type* result_type = enum_type ? enum_type : struct_type;
    expr->node_type = result_type;
    return result_type;
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

    // Blank identifier `_` as a plain-assignment target (F1): `_ = rhs`
    // discards the value. The LHS `_` is not a real variable, so type-checking
    // it as an expression would wrongly report "Undefined variable '_'".
    // Skip the LHS lookup, type-check the RHS for its side effects/validity,
    // and yield the RHS type as the assignment's result.
    if (binary->operator == TOKEN_ASSIGN &&
        binary->left && binary->left->type == AST_IDENTIFIER &&
        strcmp(((IdentifierNode*)binary->left)->name, "_") == 0) {
        Type* rhs_type = type_check_expression(checker, binary->right);
        if (!rhs_type) return NULL;
        expr->node_type = rhs_type;
        return rhs_type;
    }

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
            // Address-of yields a pointer. (A borrow/reference type has no LLVM
            // mapping and the codegen + deref paths reason in TYPE_POINTER, so
            // produce a pointer here to keep typecheck and codegen in agreement.)
            result_type = type_pointer(operand_type);
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

    // Second argument is the channel capacity, and is optional as of M8:
    // make_chan(T) and make_chan(T, 0) both produce an unbuffered (rendezvous)
    // channel where send blocks until a receiver takes the value. A positive
    // capacity produces a buffered channel.
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

// Builtin numeric/char type-conversion target (F2). Returns the named
// builtin Type for a conversion `T(x)`, or NULL if `name` is not a
// *supported* conversion type. Mirrors the type-name table in
// type_from_ast() but scoped to the numeric kinds a value conversion can
// produce. `string`/`bool` are deliberately NOT here — string conversions
// need byte/rune lowering (deferred) and bool has no numeric conversion in
// Go — but they ARE recognized as conversion *names* (see
// name_is_builtin_conv_name) so the call gate can reject them with a clean
// conversion-specific diagnostic instead of letting them fall through to
// ordinary identifier resolution (a misleading "Undefined variable 'string'"
// plus a follow-on cascade).
// Does a user-declared symbol shadow the predeclared type `name`? Go permits
// shadowing predeclared identifiers, so a value or function named `int`/`byte`
// makes `int(x)` an ordinary reference/call, not a conversion. Two sources:
//   1. scope_lookup_variable — a variable, or a top-level function declared
//      *before* this use (functions are registered in scope as decls are
//      processed in order).
//   2. comptime_context_lookup_func — every top-level function, bound in the
//      program pre-pass regardless of source order, so a *forward*-declared
//      `func int(...)` is honored too. Methods (receiver != NULL) belong to a
//      type's method set and do NOT shadow the conversion, so they are
//      excluded — otherwise a method coincidentally named `int` would
//      over-reject legitimate `int(x)` conversions.
// Codegen mirrors this via type_checker_lookup_variable so the two stages agree
// (avoids the silent miscompile where the checker calls through but codegen
// still converts).
static int name_is_user_shadowed(TypeChecker* checker, const char* name) {
    if (!checker || !name) return 0;
    if (scope_lookup_variable(checker->current_scope, name)) return 1;
    if (checker->comptime_type_ctx && checker->comptime_type_ctx->comptime_ctx) {
        ASTNode* fn = comptime_context_lookup_func(
            checker->comptime_type_ctx->comptime_ctx, name);
        if (fn && fn->type == AST_FUNC_DECL && !((FuncDeclNode*)fn)->receiver)
            return 1;
    }
    return 0;
}

static Type* builtin_conversion_target(TypeChecker* checker, const char* name) {
    if (!name) return NULL;
    if (strcmp(name, "int") == 0)     return type_checker_get_builtin(checker, TYPE_INT32);
    if (strcmp(name, "int8") == 0)    return type_checker_get_builtin(checker, TYPE_INT8);
    if (strcmp(name, "int16") == 0)   return type_checker_get_builtin(checker, TYPE_INT16);
    if (strcmp(name, "int32") == 0)   return type_checker_get_builtin(checker, TYPE_INT32);
    if (strcmp(name, "int64") == 0)   return type_checker_get_builtin(checker, TYPE_INT64);
    if (strcmp(name, "uint") == 0)    return type_checker_get_builtin(checker, TYPE_UINT32);
    if (strcmp(name, "uint8") == 0)   return type_checker_get_builtin(checker, TYPE_UINT8);
    if (strcmp(name, "uint16") == 0)  return type_checker_get_builtin(checker, TYPE_UINT16);
    if (strcmp(name, "uint32") == 0)  return type_checker_get_builtin(checker, TYPE_UINT32);
    if (strcmp(name, "uint64") == 0)  return type_checker_get_builtin(checker, TYPE_UINT64);
    if (strcmp(name, "byte") == 0)    return type_checker_get_builtin(checker, TYPE_UINT8);
    if (strcmp(name, "float32") == 0) return type_checker_get_builtin(checker, TYPE_FLOAT32);
    if (strcmp(name, "float64") == 0) return type_checker_get_builtin(checker, TYPE_FLOAT64);
    return NULL;
}

// Is `name` a builtin type-conversion name `T(x)` recognizes (F2)? This is the
// FULL recognized set the plan (F2 Step 3) lists — the numeric kinds plus
// `string`/`bool`. It is a superset of builtin_conversion_target(): numeric
// names produce a value conversion; `string`/`bool` are recognized only so the
// call gate rejects them cleanly (unsupported in v1) rather than mis-resolving
// them as undefined variables. Used solely to route a call onto the conversion
// gate; the gate then asks builtin_conversion_target() what to actually do.
static int name_is_builtin_conv_name(const char* name) {
    if (!name) return 0;
    static const char* names[] = {
        "int", "int8", "int16", "int32", "int64",
        "uint", "uint8", "uint16", "uint32", "uint64",
        "byte", "float32", "float64", "string", "bool",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i]) == 0) return 1;
    }
    return 0;
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
        // Builtin type conversion `T(x)` (F2): a call whose callee names a
        // builtin conversion type is a conversion, not a function call. Gate on
        // the name NOT being shadowed by a user variable OR function (Go
        // permits shadowing predeclared identifiers), so a user `func int` /
        // `var int` still calls/references through. Type names are not
        // registered in scope, so an unshadowed name takes the conversion path,
        // where numeric targets convert and `string`/`bool` are rejected cleanly.
        if (name_is_builtin_conv_name(func_ident->name) &&
            !name_is_user_shadowed(checker, func_ident->name)) {
            {
                Type* conv_target = builtin_conversion_target(checker, func_ident->name);
                // Recognized-but-unsupported conversion target (`string`/`bool`):
                // reject cleanly here with a conversion-specific diagnostic. The
                // name is no longer left to fall through to identifier resolution,
                // which emitted a misleading "Undefined variable '<name>'" plus a
                // follow-on cascade. v1 supports numeric conversions only.
                if (!conv_target) {
                    type_error(checker, expr->pos,
                               "cannot convert to %s (only numeric conversions "
                               "are supported in v1)",
                               func_ident->name);
                    return NULL;
                }
                if (!call->args || call->args->next) {
                    type_error(checker, expr->pos,
                               "conversion %s() expects exactly one argument",
                               func_ident->name);
                    return NULL;
                }
                Type* src = type_check_expression(checker, call->args);
                if (!src) return NULL;
                // Only numeric/char sources are convertible in v1. char (rune)
                // is an integer value, so it converts like one. string/bool
                // and aggregate sources are rejected cleanly here rather than
                // miscompiling at the LLVM verifier.
                if (!type_is_numeric(src) && src->kind != TYPE_CHAR) {
                    type_error(checker, expr->pos,
                               "cannot convert %s to %s (only numeric conversions "
                               "are supported in v1)",
                               type_to_string(src), func_ident->name);
                    return NULL;
                }
                expr->node_type = conv_target;
                return conv_target;
            }
        }
        // new(T) -> *T. The sole argument is a type name (e.g. `new(int)`),
        // resolved as a type rather than typechecked as a value expression.
        if (strcmp(func_ident->name, "new") == 0) {
            if (!call->args || call->args->next) {
                type_error(checker, expr->pos, "new expects exactly one type argument");
                return NULL;
            }
            Type* elem = type_from_ast(checker, call->args);
            if (!elem) return NULL; // type_from_ast reports the error
            Type* ptr = type_pointer(elem);
            expr->node_type = ptr;
            return ptr;
        }
        // append(slice, elem) -> slice. The result type is the first arg's
        // slice type (dynamic), so it can't ride the generic builtin path;
        // codegen lowers it to goo_slice_append (in-place amortized grow).
        if (strcmp(func_ident->name, "append") == 0) {
            if (!call->args || !call->args->next || call->args->next->next) {
                type_error(checker, expr->pos, "append expects exactly two arguments (slice, element)");
                return NULL;
            }
            Type* slice_t = type_check_expression(checker, call->args);
            if (!slice_t) return NULL;
            if (slice_t->kind != TYPE_SLICE) {
                type_error(checker, expr->pos,
                           "append: first argument must be a slice, got %s", type_to_string(slice_t));
                return NULL;
            }
            Type* elem_t = type_check_expression(checker, call->args->next);
            if (!elem_t) return NULL;
            // The element must be assignable to the slice's element type:
            // codegen sizes the copy from the slice element type, so a
            // mismatch (e.g. append([]int, "s")) would otherwise miscompile.
            if (!type_compatible(elem_t, slice_t->data.slice.element_type)) {
                type_error(checker, expr->pos,
                           "append: cannot use %s as element of %s",
                           type_to_string(elem_t), type_to_string(slice_t));
                return NULL;
            }
            expr->node_type = slice_t;
            return slice_t;
        }
        // cap(slice) -> int. The slice's capacity (header field 2).
        if (strcmp(func_ident->name, "cap") == 0) {
            if (!call->args || call->args->next) {
                type_error(checker, expr->pos, "cap expects exactly one argument");
                return NULL;
            }
            Type* slice_t = type_check_expression(checker, call->args);
            if (!slice_t) return NULL;
            if (slice_t->kind != TYPE_SLICE) {
                type_error(checker, expr->pos,
                           "cap: argument must be a slice, got %s", type_to_string(slice_t));
                return NULL;
            }
            expr->node_type = checker->builtin_types[TYPE_INT32];
            return checker->builtin_types[TYPE_INT32];
        }
        // error(msg) -> !T. Constructs the error case of the enclosing function's
        // return type. The argument must be a string; the call is only valid inside
        // a function whose return type is an error union (!T).
        //
        // Gate on the predeclared `error` builtin actually resolving in scope (it
        // is registered in type_checker.c alongside len/cap/append). This makes the
        // registration load-bearing and keeps `error` Go-faithfully shadowable: a
        // user-declared local `error` (is_builtin == 0, or absent from scope) falls
        // through to ordinary identifier resolution instead of being hijacked here.
        Variable* error_builtin = scope_lookup_variable(checker->current_scope, "error");
        if (strcmp(func_ident->name, "error") == 0 && error_builtin && error_builtin->is_builtin) {
            if (!call->args || call->args->next) {
                type_error(checker, expr->pos, "error expects exactly one string argument");
                return NULL;
            }
            Type* arg_t = type_check_expression(checker, call->args);
            if (!arg_t) return NULL;
            if (arg_t->kind != TYPE_STRING) {
                type_error(checker, expr->pos,
                           "error: argument must be a string, got %s",
                           type_to_string(arg_t));
                return NULL;
            }
            Type* ret = checker->current_return_type;
            if (!ret || !type_is_error_union(ret)) {
                type_error(checker, expr->pos,
                           "error() can only be used inside a function returning !T");
                return NULL;
            }
            expr->node_type = ret;
            return ret;
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
    
    // Decide whether to validate arity/arg types against the callee's
    // declared parameter list. We do so for ordinary *user* functions called
    // by their bare name, AND for user *method* calls (selectors that resolve
    // to a method). Two wrinkles:
    //   - Builtins (println/print/len/cap/append/error/make_chan/new) carry
    //     param_types=NULL/param_count=0 and are variadic or special-cased,
    //     so checking them against their stub signature would false-reject
    //     (e.g. len("x")). They are flagged is_builtin — skip them.
    //   - A method's func_type carries the spliced receiver as params[0]
    //     while the call's arg list omits it. So for methods we offset every
    //     comparison by recv_offset=1: arg i is matched against params[i+1]
    //     and the expected user-visible arity is (param_count - 1).
    //   - Variadic user functions have no fixed arity to check.
    // Package functions (fmt.Println) are also selectors but resolve through
    // TYPE_PACKAGE, not a struct receiver, so the method branch below skips
    // them; their variadic flag would skip them regardless.
    int check_signature = 0;
    size_t recv_offset = 0;
    const char* callee_name = NULL;
    if (call->function && call->function->type == AST_IDENTIFIER
        && !func_type->data.function.is_variadic) {
        IdentifierNode* callee_ident = (IdentifierNode*)call->function;
        Variable* callee = type_checker_lookup_variable(checker, callee_ident->name);
        if (callee && !callee->is_builtin) {
            check_signature = 1;
            callee_name = callee_ident->name;
        }
    } else if (call->function && call->function->type == AST_SELECTOR_EXPR
               && !func_type->data.function.is_variadic) {
        // Confirm the selector names a METHOD (receiver spliced into
        // params[0]), not a struct field that happens to hold a function
        // value, by re-resolving the mangled method name exactly as the
        // selector checker does and demanding it yields THIS func_type. This
        // precision avoids over-rejecting a function-valued field call (which
        // has no receiver to offset) and skips package functions cleanly.
        SelectorExprNode* sel = (SelectorExprNode*)call->function;
        Type* recv_t = sel->expr ? sel->expr->node_type : NULL;
        if (recv_t) {
            Type* st = recv_t;
            if (st->kind == TYPE_POINTER &&
                st->data.pointer.pointee_type &&
                st->data.pointer.pointee_type->kind == TYPE_STRUCT) {
                st = st->data.pointer.pointee_type;
            }
            if (st->kind == TYPE_STRUCT) {
                const char* tn = type_receiver_name(st);
                if (tn) {
                    char* mangled = type_method_mangled_name(tn, sel->selector);
                    Variable* m = mangled
                        ? type_checker_lookup_variable(checker, mangled) : NULL;
                    free(mangled);
                    if (m && m->type == func_type && !m->is_builtin) {
                        check_signature = 1;
                        recv_offset = 1;
                        callee_name = sel->selector;
                    }
                }
            } else if (st->kind == TYPE_INTERFACE) {
                // Interface method call (P4-4): the method type carries no
                // receiver, so check args directly (recv_offset = 0).
                for (InterfaceMethod* im = st->data.interface.methods; im; im = im->next) {
                    if (im->name && strcmp(im->name, sel->selector) == 0 &&
                        im->type == func_type) {
                        check_signature = 1;
                        recv_offset = 0;
                        callee_name = sel->selector;
                        break;
                    }
                }
            }
        }
    }

    // Check arguments
    ASTNode* arg = call->args;
    size_t arg_count = 0;
    size_t param_count = func_type->data.function.param_count;
    Type** param_types = func_type->data.function.param_types;
    while (arg) {
        Type* arg_type = type_check_expression(checker, arg);
        if (!arg_type) return NULL;

        // Argument type compatibility: position-named so the diagnostic
        // points at the offending argument rather than the LLVM verifier.
        // For methods, recv_offset=1 skips the spliced receiver in params[0].
        if (check_signature && (arg_count + recv_offset) < param_count && param_types) {
            Type* param_type = param_types[arg_count + recv_offset];

            // type_compatible() permits ANY numeric->numeric pair (it allows
            // implicit conversions), but call_codegen passes each argument to
            // the callee with NO trunc/ext/fptosi inserted (unlike the return
            // path, codegen never coerces a numeric arg). So a numeric arg
            // whose machine representation differs from the declared parameter
            // — a wider/narrower integer (int64 into an int param) or a float
            // into an integer (e.g. `add(1.5, 2)`) — slips past type_compatible
            // and crashes the LLVM verifier with "Call parameter type does not
            // match function signature!". Reject those here, mirroring P2-1's
            // return guard. No int-constant-widen exemption applies: codegen
            // does not widen arguments, so even an untyped literal into a wider
            // param would reach the verifier as invalid IR.
            if (param_type && type_is_numeric(arg_type) && type_is_numeric(param_type)) {
                int same_kind  = (type_is_float(arg_type) == type_is_float(param_type));
                int same_width = (type_size(arg_type) == type_size(param_type));
                if (!same_kind || !same_width) {
                    type_error(checker, arg->pos,
                               "argument %zu: cannot use %s as %s",
                               arg_count + 1,
                               type_to_string(arg_type), type_to_string(param_type));
                    return NULL;
                }
            }

            // Interface parameter (P4-3/P4-5): a concrete implementer may be
            // passed where an interface is expected. Check satisfaction here so
            // `f(Sq{})` into `func f(s Shape)` is accepted; codegen boxes it.
            if (param_type && param_type->kind == TYPE_INTERFACE &&
                arg_type && arg_type->kind != TYPE_INTERFACE) {
                const char* method = NULL;
                const char* reason = NULL;
                if (!type_interface_satisfied(checker, param_type, arg_type,
                                              &method, &reason)) {
                    const char* iname = param_type->data.interface.name
                                            ? param_type->data.interface.name : "interface";
                    const char* cname = type_receiver_name(arg_type);
                    type_error(checker, arg->pos,
                               "argument %zu: %s does not implement %s (%s method %s)",
                               arg_count + 1, cname ? cname : type_to_string(arg_type),
                               iname, reason ? reason : "missing", method ? method : "?");
                    return NULL;
                }
            } else if (param_type && !type_compatible(arg_type, param_type)) {
                type_error(checker, arg->pos,
                           "argument %zu: cannot use %s as %s",
                           arg_count + 1,
                           type_to_string(arg_type), type_to_string(param_type));
                return NULL;
            }
        }

        arg_count++;
        arg = arg->next;
    }

    // Argument count against the declared parameter list (minus the spliced
    // receiver for methods: param_count >= 1 whenever recv_offset == 1, so the
    // subtraction never underflows).
    if (check_signature && arg_count != param_count - recv_offset) {
        type_error(checker, expr->pos,
                   "call to %s: wrong number of arguments (have %zu, want %zu)",
                   callee_name, arg_count, param_count - recv_offset);
        return NULL;
    }

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

// F5: `base[low:high]` slice/substring. Result keeps the base's type — a
// substring is a string, a reslice is the same slice type. Array slicing
// (which yields a slice in Go) is deferred: the by-value array codegen needs
// the array materialised to a pointer first, so it is rejected cleanly here
// rather than accepted and then failing in codegen.
Type* type_check_slice_index_expr(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_SLICE_INDEX_EXPR) return NULL;

    SliceIndexExprNode* slice = (SliceIndexExprNode*)expr;

    Type* base_type = type_check_expression(checker, slice->expr);
    Type* low_type = type_check_expression(checker, slice->low);
    Type* high_type = type_check_expression(checker, slice->high);
    if (!base_type || !low_type || !high_type) return NULL;

    if (!type_is_integer(low_type)) {
        type_error(checker, slice->low->pos,
                   "Slice low bound must be integer, got %s", type_to_string(low_type));
        return NULL;
    }
    if (!type_is_integer(high_type)) {
        type_error(checker, slice->high->pos,
                   "Slice high bound must be integer, got %s", type_to_string(high_type));
        return NULL;
    }

    switch (base_type->kind) {
        case TYPE_STRING:   // substring shares the byte buffer
        case TYPE_SLICE:    // reslice shares the backing array
            expr->node_type = base_type;
            return base_type;
        default:
            type_error(checker, slice->expr->pos,
                       "Cannot slice type %s (v1 supports string and slice)",
                       type_to_string(base_type));
            return NULL;
    }
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

    // os.Getenv(string) -> string
    if (strcmp(package, "os") == 0 && strcmp(name, "Getenv") == 0) {
        Type* string_t = type_checker_get_builtin(checker, TYPE_STRING);
        return type_function(NULL, 0, string_t);
    }

    // File I/O (M1): scalar signatures, all returning int (bytes written /
    // byte value / size, or a negative value on error).
    //   os.WriteFile(path string, data string) -> int
    //   os.ReadByte(path string, offset int)   -> int
    //   os.FileSize(path string)               -> int
    if (strcmp(package, "os") == 0 &&
        (strcmp(name, "WriteFile") == 0 || strcmp(name, "ReadByte") == 0 ||
         strcmp(name, "FileSize") == 0)) {
        Type* int_t = type_checker_get_builtin(checker, TYPE_INT32);
        return type_function(NULL, 0, int_t);
    }

    // math.Pi -> float64. A package VALUE member, not a call — the
    // returned type is the value's type, no type_function wrapper.
    if (strcmp(package, "math") == 0 && strcmp(name, "Pi") == 0) {
        return type_checker_get_builtin(checker, TYPE_FLOAT64);
    }

    // math.Sqrt/Pow/Abs/Min/Max(float64...) -> float64
    if (strcmp(package, "math") == 0 &&
        (strcmp(name, "Sqrt") == 0 || strcmp(name, "Pow") == 0 ||
         strcmp(name, "Abs") == 0 || strcmp(name, "Min") == 0 ||
         strcmp(name, "Max") == 0)) {
        Type* float_t = type_checker_get_builtin(checker, TYPE_FLOAT64);
        return type_function(NULL, 0, float_t);
    }

    // strings.Contains(string, string) -> bool
    if (strcmp(package, "strings") == 0 && strcmp(name, "Contains") == 0) {
        Type* bool_t = type_checker_get_builtin(checker, TYPE_BOOL);
        return type_function(NULL, 0, bool_t);
    }

    // strings.ToUpper/ToLower/TrimSpace(string) -> string
    if (strcmp(package, "strings") == 0 &&
        (strcmp(name, "ToUpper") == 0 || strcmp(name, "ToLower") == 0 ||
         strcmp(name, "TrimSpace") == 0)) {
        Type* string_t = type_checker_get_builtin(checker, TYPE_STRING);
        return type_function(NULL, 0, string_t);
    }

    // strings.Split(string, string) -> []string
    if (strcmp(package, "strings") == 0 && strcmp(name, "Split") == 0) {
        Type* string_t = type_checker_get_builtin(checker, TYPE_STRING);
        return type_function(NULL, 0, type_slice(string_t));
    }

    // strings.Join([]string, string) -> string
    if (strcmp(package, "strings") == 0 && strcmp(name, "Join") == 0) {
        Type* string_t = type_checker_get_builtin(checker, TYPE_STRING);
        return type_function(NULL, 0, string_t);
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
        // Not a field — try a method `T__selector`. Methods are registered
        // as ordinary functions under their mangled name, so a plain
        // variable lookup resolves them. Returns the method's function type;
        // the call expression then yields its return type.
        const char* tn = type_receiver_name(struct_type);
        if (tn) {
            char* mangled = type_method_mangled_name(tn, selector->selector);
            Variable* m = mangled ? type_checker_lookup_variable(checker, mangled) : NULL;
            free(mangled);
            if (m && m->type && m->type->kind == TYPE_FUNCTION) {
                expr->node_type = m->type;
                return m->type;
            }
        }
        type_error(checker, expr->pos, "Struct has no field or method '%s'", selector->selector);
        return NULL;
    }

    // Interface method access (P4-4): resolve the selector in the interface's
    // method set. The method's function type carries NO receiver (unlike a
    // struct method's mangled function), so a call `a.M(args)` checks its args
    // directly against the method signature.
    if (expr_type->kind == TYPE_INTERFACE) {
        for (InterfaceMethod* im = expr_type->data.interface.methods; im; im = im->next) {
            if (im->name && strcmp(im->name, selector->selector) == 0) {
                expr->node_type = im->type;
                return im->type;
            }
        }
        type_error(checker, expr->pos, "%s has no method '%s'",
                   expr_type->data.interface.name ? expr_type->data.interface.name
                                                  : "interface",
                   selector->selector);
        return NULL;
    }

    // Named non-struct type (e.g. `type IntSlice []int`) method call: resolve
    // `Name__selector` exactly like the struct method path above (1199-1208).
    if (expr_type->name) {
        char* mangled = type_method_mangled_name(expr_type->name, selector->selector);
        Variable* m = mangled ? type_checker_lookup_variable(checker, mangled) : NULL;
        free(mangled);
        if (m && m->type && m->type->kind == TYPE_FUNCTION) {
            expr->node_type = m->type;
            return m->type;
        }
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

    // `try` propagates the error out of the ENCLOSING function on the error
    // path, so that function must itself return an error union (!T). Rejecting
    // this here keeps the codegen propagation path (LLVMBuildRet operand) total
    // — before this check a `try` in a non-!T function silently emitted
    // `unreachable` (garbage IR, no diagnostic).
    Type* enclosing = checker->current_return_type;
    if (!enclosing || !type_is_error_union(enclosing)) {
        type_error(checker, expr->pos,
                  "try can only be used inside a function that returns an error union (!T)");
        return NULL;
    }

    // Error-union-ness of the enclosing function is NECESSARY. The VALUE types
    // need NOT match: `try` propagates the operand's ERROR (not its value) out
    // of the enclosing function, and codegen re-wraps that error into the
    // enclosing function's error-union type. This enables the headline
    // cross-value-type propagation pattern — e.g. `name := try getName()` where
    // getName() is `!string`, used inside a `process() !int` function: the
    // unwrapped string is consumed locally while any error propagates up as the
    // function's own `!int`. Only the ERROR types must be compatible; in Phase 1
    // the error slot is always a string (default error type, represented as a
    // NULL error_type), so any two `!T`s are compatible. Reject only an explicit
    // error-type mismatch, which has no faithful re-wrap today.
    Type* operand_err = expr_type->data.error_union.error_type;
    Type* enclosing_err = enclosing->data.error_union.error_type;
    if (operand_err && enclosing_err && !type_equals(operand_err, enclosing_err)) {
        type_error(checker, expr->pos,
                  "try operand error type %s does not match the enclosing "
                  "function's error type %s",
                  type_to_string(operand_err), type_to_string(enclosing_err));
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
    
    // Type-check the catch body as a STATEMENT (the grammar always produces a
    // block: `expression CATCH identifier block`). Calling type_check_expression
    // on an AST_BLOCK_STMT hits the default "Unknown expression type" error.
    if (catch_expr->catch_body) {
        scope_push(checker);

        // Add error variable to scope so the catch body can reference it.
        if (catch_expr->error_var) {
            Type* error_type = expr_type->data.error_union.error_type;
            if (!error_type) {
                error_type = type_checker_get_builtin(checker, TYPE_STRING);
            }

            Variable* error_var = variable_new(catch_expr->error_var, error_type, expr->pos);
            if (error_var) {
                error_var->is_initialized = 1;
                scope_add_variable(checker->current_scope, error_var);
            }
        }

        type_check_statement(checker, catch_expr->catch_body);
        scope_pop(checker);
    }

    // The type of a catch expression is the value type of the error union.
    Type* value_type = expr_type->data.error_union.value_type;

    // P2-1: a value-producing handler (one whose final statement is an
    // expression) recovers with that expression's value on the error path, so
    // its type must be assignable to the value type T. A void trailing
    // expression (e.g. `fmt.Println(e)`) is a side-effect-only handler that
    // recovers with the zero value of T — that is allowed and not checked here.
    ASTNode* trailing = ast_block_trailing_expr(catch_expr->catch_body);
    if (trailing && trailing->node_type &&
        trailing->node_type->kind != TYPE_VOID &&
        !type_compatible(trailing->node_type, value_type)) {
        type_error(checker, trailing->pos,
                   "catch handler value of type %s is not assignable to %s",
                   type_to_string(trailing->node_type),
                   type_to_string(value_type));
        return NULL;
    }

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

// Typecheck a match expression (statement-style, yields void).
// For each arm: open a per-arm scope, bind positional payload names to
// variant field types (PATTERN_DESTRUCTURE over TYPE_ENUM), typecheck
// the guard and body, close the scope.  Enforces exhaustiveness when
// the scrutinee is a TYPE_ENUM and no wildcard arm is present.
Type* type_check_match_expr(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr) return NULL;

    MatchExprNode* m = (MatchExprNode*)expr;
    Type* scrut = type_check_expression(checker, m->expr);
    if (!scrut) return NULL;

    int is_enum = (scrut->kind == TYPE_ENUM);
    size_t vcount = is_enum ? scrut->data.enum_type.variant_count : 0;
    // covered[i] tracks whether variant i has an arm; calloc zeroes it.
    int* covered = vcount ? calloc(vcount, sizeof(int)) : NULL;
    int has_default = 0;

    for (ASTNode* c = m->cases; c; c = c->next) {
        MatchCaseNode* mc = (MatchCaseNode*)c;
        PatternNode* p = (PatternNode*)mc->pattern;

        // Per-arm scope so payload bindings don't leak to siblings or caller.
        scope_push(checker);

        if (p->pattern_type == PATTERN_WILDCARD) {
            if (has_default) {
                type_error(checker, c->pos,
                    "duplicate default arm in match");
                scope_pop(checker);
                free(covered);
                return NULL;
            }
            has_default = 1;
        } else if (is_enum && p->pattern_type == PATTERN_DESTRUCTURE) {
            const char* vn = p->data.destructure.type_name;
            EnumVariant* variant = NULL;
            int vidx = -1;
            for (size_t i = 0; i < vcount; i++) {
                if (strcmp(scrut->data.enum_type.variants[i].name, vn) == 0) {
                    variant = &scrut->data.enum_type.variants[i];
                    vidx = (int)i;
                    break;
                }
            }
            if (!variant) {
                type_error(checker, c->pos,
                    "'%s' is not a variant of enum '%s'",
                    vn, scrut->data.enum_type.name);
                scope_pop(checker);
                free(covered);
                return NULL;
            }
            if (covered[vidx]) {
                type_error(checker, c->pos,
                    "Duplicate match arm for variant '%s'", vn);
                scope_pop(checker);
                free(covered);
                return NULL;
            }
            covered[vidx] = 1;

            // Bind each positional identifier in data.destructure.fields to
            // the corresponding variant payload field type.
            Type* payload = variant->payload;
            size_t field_count = payload ? payload->data.struct_type.field_count : 0;
            size_t fi = 0;
            for (ASTNode* b = p->data.destructure.fields; b; b = b->next, fi++) {
                if (fi >= field_count) {
                    type_error(checker, c->pos,
                        "Too many bindings for variant '%s' (has %zu field(s))",
                        vn, field_count);
                    scope_pop(checker);
                    free(covered);
                    return NULL;
                }
                // Only bind AST_IDENTIFIER nodes; ignore wildcards named `_`.
                if (b->type != AST_IDENTIFIER) continue;
                IdentifierNode* bind = (IdentifierNode*)b;
                if (strcmp(bind->name, "_") == 0) continue;
                Variable* var = variable_new(bind->name,
                    payload->data.struct_type.fields[fi].type, c->pos);
                if (var) {
                    var->is_initialized = 1;
                    scope_add_variable(checker->current_scope, var);
                }
            }
        }
        // Non-enum destructure / literal / identifier patterns: no enum-specific
        // bookkeeping; existing value-match semantics apply.

        // Typecheck the optional guard in the arm's scope.
        if (mc->guard) {
            type_check_expression(checker,
                ((GuardConditionNode*)mc->guard)->condition);
        }

        // Typecheck body statements in the arm's scope.
        for (ASTNode* s = mc->body; s; s = s->next)
            type_check_statement(checker, s);

        scope_pop(checker);
    }

    // Exhaustiveness: every variant must be covered when there is no default.
    if (is_enum && !has_default) {
        for (size_t i = 0; i < vcount; i++) {
            if (!covered[i]) {
                type_error(checker, expr->pos,
                    "Non-exhaustive match: variant '%s' not handled "
                    "(add a case or `default:`)",
                    scrut->data.enum_type.variants[i].name);
                free(covered);
                return NULL;
            }
        }
    }

    free(covered);
    expr->node_type = type_checker_get_builtin(checker, TYPE_VOID);
    return expr->node_type;
}

