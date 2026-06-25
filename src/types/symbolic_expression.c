// Leaf utilities for SymbolicExpression lifetime management.
//
// Extracted from bounds_verifier.c so that consumers needing only the
// symbolic-expression destructor (e.g. dependent_types.c's type_constraint_free)
// don't have to link the entire bounds verifier, which drags in the full
// type checker. The prototype lives in panic_free.h.
#include "panic_free.h"
#include <stdlib.h>

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
