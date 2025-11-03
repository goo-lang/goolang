#include "types.h"
#include <stdlib.h>
#include <string.h>

// Helper functions for expression type checking

Type* type_check_arithmetic_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op, Position pos) {
    if (!checker || !left_type || !right_type) return NULL;

    // Special case: string concatenation with + operator
    if (op == TOKEN_PLUS && left_type->kind == TYPE_STRING && right_type->kind == TYPE_STRING) {
        return type_checker_get_builtin(checker, TYPE_STRING);
    }

    // Both operands must be numeric
    if (!type_is_numeric(left_type) || !type_is_numeric(right_type)) {
        type_error(checker, pos, "Arithmetic operation requires numeric operands");
        return NULL;
    }

    // For now, return the "larger" type (simple promotion rules)
    if (type_is_float(left_type) || type_is_float(right_type)) {
        if (left_type->kind == TYPE_FLOAT64 || right_type->kind == TYPE_FLOAT64) {
            return type_checker_get_builtin(checker, TYPE_FLOAT64);
        } else {
            return type_checker_get_builtin(checker, TYPE_FLOAT32);
        }
    } else {
        // Integer arithmetic
        if (left_type->kind == TYPE_INT64 || right_type->kind == TYPE_INT64) {
            return type_checker_get_builtin(checker, TYPE_INT64);
        } else {
            return type_checker_get_builtin(checker, TYPE_INT32);
        }
    }
}

Type* type_check_comparison_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op __attribute__((unused)), Position pos) {
    if (!checker || !left_type || !right_type) return NULL;
    
    // Comparison operations return boolean
    Type* bool_type = type_checker_get_builtin(checker, TYPE_BOOL);
    
    // Check if types are compatible for comparison
    if (!type_compatible(left_type, right_type)) {
        type_error(checker, pos, "Cannot compare incompatible types %s and %s",
                  type_to_string(left_type), type_to_string(right_type));
        return NULL;
    }
    
    return bool_type;
}

Type* type_check_logical_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op __attribute__((unused)), Position pos) {
    if (!checker || !left_type || !right_type) return NULL;
    
    // Logical operations require boolean operands
    if (left_type->kind != TYPE_BOOL || right_type->kind != TYPE_BOOL) {
        type_error(checker, pos, "Logical operation requires boolean operands");
        return NULL;
    }
    
    return type_checker_get_builtin(checker, TYPE_BOOL);
}

Type* type_check_bitwise_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op __attribute__((unused)), Position pos) {
    if (!checker || !left_type || !right_type) return NULL;
    
    // Bitwise operations require integer operands
    if (!type_is_integer(left_type) || !type_is_integer(right_type)) {
        type_error(checker, pos, "Bitwise operation requires integer operands");
        return NULL;
    }
    
    // Return the "larger" integer type
    if (left_type->kind == TYPE_INT64 || right_type->kind == TYPE_INT64) {
        return type_checker_get_builtin(checker, TYPE_INT64);
    } else {
        return type_checker_get_builtin(checker, TYPE_INT32);
    }
}

Type* type_check_assignment_op(TypeChecker* checker, ASTNode* target, Type* target_type, Type* value_type, Position pos) {
    if (!checker || !target || !target_type || !value_type) return NULL;
    
    // Check that value is compatible with target
    if (!type_compatible(value_type, target_type)) {
        type_error(checker, pos, "Cannot assign %s to %s",
                  type_to_string(value_type), type_to_string(target_type));
        return NULL;
    }
    
    // TODO: Check that target is assignable (lvalue)
    
    return target_type;
}