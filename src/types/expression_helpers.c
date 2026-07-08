#include "types.h"
#include <stdlib.h>
#include <string.h>

// Helper functions for expression type checking

// Fold an integer constant expression to its low 64 bits and report success.
// Go's untyped constants have arbitrary precision, but the result is always
// truncated to the target's ≤64-bit type, so 64-bit modular arithmetic gives the
// correct answer PROVIDED a shift by ≥64 yields 0 (matching truncation): then
// `1<<64 - 1` folds as `0 - 1` = all-ones = 2^64-1, exactly Go's uint64 result.
// (uint64_t, not __int128 — the latter is not accepted by the CompCert ccomp
// build.) Handles integer literals and the integer binary/unary operators;
// returns 0 for anything that is not a compile-time integer constant
// (identifiers, calls, floats) so callers fall back to ordinary codegen.
// Pure-literal only — const-identifier references are not folded here yet.
int goo_fold_const_int(ASTNode* expr, uint64_t* out) {
    if (!expr || !out) return 0;
    switch (expr->type) {
        case AST_LITERAL: {
            LiteralNode* lit = (LiteralNode*)expr;
            if (lit->literal_type != TOKEN_INT) return 0;
            *out = strtoull(lit->value, NULL, 0);
            return 1;
        }
        case AST_UNARY_EXPR: {
            UnaryExprNode* u = (UnaryExprNode*)expr;
            uint64_t v;
            if (!goo_fold_const_int(u->operand, &v)) return 0;
            switch (u->operator) {
                case TOKEN_MINUS:   *out = (uint64_t)(-(int64_t)v); return 1;
                case TOKEN_PLUS:    *out = v; return 1;
                case TOKEN_BIT_XOR: *out = ~v; return 1; // ^x complement
                case TOKEN_BIT_NOT: *out = ~v; return 1; // ~x
                default: return 0;
            }
        }
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)expr;
            uint64_t l, r;
            if (!goo_fold_const_int(b->left, &l) || !goo_fold_const_int(b->right, &r))
                return 0;
            switch (b->operator) {
                case TOKEN_PLUS:     *out = l + r; return 1;
                case TOKEN_MINUS:    *out = l - r; return 1;
                case TOKEN_MULTIPLY: *out = l * r; return 1;
                case TOKEN_DIVIDE:   if (r == 0) return 0; *out = l / r; return 1;
                case TOKEN_MODULO:   if (r == 0) return 0; *out = l % r; return 1;
                case TOKEN_LSHIFT:   *out = (r >= 64) ? 0 : (l << r); return 1;
                case TOKEN_RSHIFT:   *out = (r >= 64) ? 0 : (l >> r); return 1;
                case TOKEN_BIT_AND:  *out = l & r; return 1;
                case TOKEN_AND_NOT:  *out = l & ~r; return 1; // &^ bit-clear
                case TOKEN_BIT_OR:   *out = l | r; return 1;
                case TOKEN_BIT_XOR:  *out = l ^ r; return 1;
                default: return 0;
            }
        }
        default:
            return 0;
    }
}

// Comptime value params Task 3 (fix round 2): see the header doc comment.
// Recurses through the wrapper kinds whose inner type a local declaration
// can nest an array under; every other kind is a no-array leaf. (Function
// types are deliberately not recursed: a comptime param cannot appear in a
// function TYPE's array lengths — declare_function_signature resolves
// signatures before any param binding exists, so such a length was already
// rejected at declaration.)
int goo_type_contains_array(const Type* t) {
    while (t) {
        switch (t->kind) {
            case TYPE_ARRAY:
                return 1;
            case TYPE_SLICE:
                t = t->data.slice.element_type;
                break;
            case TYPE_POINTER:
                t = t->data.pointer.pointee_type;
                break;
            case TYPE_NULLABLE:
                t = t->data.nullable.base_type;
                break;
            default:
                return 0;
        }
    }
    return 0;
}

// Checker-aware sibling of goo_fold_const_int (see header): additionally
// resolves AST_IDENTIFIER against checker's scope chain, using the constant's
// cached integer value (Variable->const_int_value, set by
// type_check_const_decl when its RHS folds), and recurses through
// unary/binary operators WITH the same checker context so a const-expression
// built on a const-identifier (`[N+1]int`) folds too. Anything else
// (literals, and anything goo_fold_const_int already rejects) falls through
// to that context-free folder unchanged.
int goo_fold_const_int_ctx(TypeChecker* checker, ASTNode* expr, uint64_t* out) {
    if (!expr || !out) return 0;
    switch (expr->type) {
        case AST_IDENTIFIER: {
            IdentifierNode* id = (IdentifierNode*)expr;
            Variable* var = checker ? type_checker_lookup_variable(checker, id->name) : NULL;
            if (var && var->has_const_int_value) {
                *out = var->const_int_value;
                return 1;
            }
            return 0;
        }
        case AST_UNARY_EXPR: {
            UnaryExprNode* u = (UnaryExprNode*)expr;
            uint64_t v;
            if (!goo_fold_const_int_ctx(checker, u->operand, &v)) return 0;
            switch (u->operator) {
                case TOKEN_MINUS:   *out = (uint64_t)(-(int64_t)v); return 1;
                case TOKEN_PLUS:    *out = v; return 1;
                case TOKEN_BIT_XOR: *out = ~v; return 1; // ^x complement
                case TOKEN_BIT_NOT: *out = ~v; return 1; // ~x
                default: return 0;
            }
        }
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)expr;
            uint64_t l, r;
            if (!goo_fold_const_int_ctx(checker, b->left, &l) ||
                !goo_fold_const_int_ctx(checker, b->right, &r))
                return 0;
            switch (b->operator) {
                case TOKEN_PLUS:     *out = l + r; return 1;
                case TOKEN_MINUS:    *out = l - r; return 1;
                case TOKEN_MULTIPLY: *out = l * r; return 1;
                case TOKEN_DIVIDE:   if (r == 0) return 0; *out = l / r; return 1;
                case TOKEN_MODULO:   if (r == 0) return 0; *out = l % r; return 1;
                case TOKEN_LSHIFT:   *out = (r >= 64) ? 0 : (l << r); return 1;
                case TOKEN_RSHIFT:   *out = (r >= 64) ? 0 : (l >> r); return 1;
                case TOKEN_BIT_AND:  *out = l & r; return 1;
                case TOKEN_AND_NOT:  *out = l & ~r; return 1; // &^ bit-clear
                case TOKEN_BIT_OR:   *out = l | r; return 1;
                case TOKEN_BIT_XOR:  *out = l ^ r; return 1;
                default: return 0;
            }
        }
        default:
            // AST_LITERAL and anything not special-cased above: delegate to
            // the context-free folder (handles TOKEN_INT literals; returns 0
            // for anything else, same as here).
            return goo_fold_const_int(expr, out);
    }
}

// Fold a compile-time string constant — string literals joined by `+` — into a
// freshly malloc'd byte buffer (see header). Recurses over `+` binary nodes and
// concatenates the decoded bytes, preserving embedded NULs via each literal's
// stored length. Returns 0 (and allocates nothing kept) for any non-string-
// literal-concatenation node, so callers fall back to ordinary codegen.
int goo_fold_const_string(ASTNode* expr, char** out, size_t* out_len) {
    if (!expr || !out || !out_len) return 0;
    switch (expr->type) {
        case AST_LITERAL: {
            LiteralNode* lit = (LiteralNode*)expr;
            if (lit->literal_type != TOKEN_STRING) return 0;
            char* buf = (char*)malloc(lit->length + 1);
            if (!buf) return 0;
            if (lit->length > 0) memcpy(buf, lit->value, lit->length);
            buf[lit->length] = '\0';
            *out = buf;
            *out_len = lit->length;
            return 1;
        }
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)expr;
            if (b->operator != TOKEN_PLUS) return 0; // only concatenation folds
            char *lbuf = NULL, *rbuf = NULL;
            size_t llen = 0, rlen = 0;
            if (!goo_fold_const_string(b->left, &lbuf, &llen)) return 0;
            if (!goo_fold_const_string(b->right, &rbuf, &rlen)) { free(lbuf); return 0; }
            char* buf = (char*)malloc(llen + rlen + 1);
            if (!buf) { free(lbuf); free(rbuf); return 0; }
            if (llen > 0) memcpy(buf, lbuf, llen);
            if (rlen > 0) memcpy(buf + llen, rbuf, rlen);
            buf[llen + rlen] = '\0';
            free(lbuf);
            free(rbuf);
            *out = buf;
            *out_len = llen + rlen;
            return 1;
        }
        default:
            return 0;
    }
}

// Result type of an integer binary operation. Go requires both operands to
// share a type (untyped constants adapt to the other operand); Goo approximates
// that adaptation and — crucially — preserves the operand's WIDTH and SIGNEDNESS
// rather than collapsing everything that isn't int64 to int32, which silently
// turned uint64/uint arithmetic into int32 and broke every unsigned stdlib
// function. For a shift, Go fixes the result type to the LEFT operand's type
// regardless of the (count) right operand.
static Type* integer_binop_result_type(Type* left, Type* right, int is_shift) {
    if (is_shift) return left;                    // Go: shift result = left type
    if (left->kind == right->kind) return left;
    // One side is int64 — the default type of an untyped integer constant (Go's
    // `int`). Adopt the other, sized operand, the common `uint64 op 1` /
    // `1 op uint64` case. (The shape-based literal adaptation in
    // type_check_binary_expr handles most of these earlier; this is the
    // fallback for a constant that reached here still int64-typed.)
    if (left->kind == TYPE_INT64) return right;
    if (right->kind == TYPE_INT64) return left;
    // Two distinct sized integer types (rare without an explicit conversion in
    // well-typed Go): widen to the larger; on a tie keep the left operand.
    return (type_size(right) > type_size(left)) ? right : left;
}

Type* type_check_arithmetic_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op, Position pos) {
    if (!checker || !left_type || !right_type) return NULL;

    // String concatenation: in Go, `+` is overloaded for strings. When both
    // operands are strings, the result is a string (lowered to a
    // goo_string_concat call in codegen). No other arithmetic op applies to
    // strings.
    if (op == TOKEN_PLUS &&
        left_type->kind == TYPE_STRING && right_type->kind == TYPE_STRING) {
        return type_checker_get_builtin(checker, TYPE_STRING);
    }

    // Both operands must be numeric
    if (!type_is_numeric(left_type) || !type_is_numeric(right_type)) {
        type_error(checker, pos, "Arithmetic operation requires numeric operands");
        return NULL;
    }

    // `%` is not defined on floats — Go rejects it ("invalid operation:
    // operator % not defined on g (variable of type float64)") and so must
    // Goo. The float-promotion branch below only decides which float WIDTH
    // wins; it never asks whether the operator applies to floats at all, so
    // a same-kind float `%` (e.g. `g % g`, `g % 2.0`, both operands already
    // float64) sailed through here as a well-typed float64 expression.
    // Codegen's TOKEN_MODULO arm is integer-only (no frem is ever emitted),
    // so the mistyped node reached codegen and crashed there with two
    // opaque, unactionable errors ("Failed to generate binary operation"
    // then "Failed to generate initializer") instead of a checker
    // diagnostic. A cross-KIND float/int `%` (e.g. `g % 2`) is already
    // caught earlier by type_check_binary_expr's cross-kind rejection block
    // (TOKEN_MODULO is excluded from that block's adaptation, so the
    // mismatch falls through to its own clean rejection) — this check is
    // what closes the remaining same-kind-float gap.
    if (op == TOKEN_MODULO && (type_is_float(left_type) || type_is_float(right_type))) {
        Type* float_operand = type_is_float(left_type) ? left_type : right_type;
        type_error(checker, pos, "operator %% not defined on %s", type_to_string(float_operand));
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
        // Integer arithmetic — preserve operand width AND signedness (uint64
        // arithmetic stays uint64, not collapsed to int32).
        return integer_binop_result_type(left_type, right_type, 0);
    }
}

Type* type_check_comparison_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op, Position pos) {
    if (!checker || !left_type || !right_type) return NULL;

    // Comparison operations return boolean
    Type* bool_type = type_checker_get_builtin(checker, TYPE_BOOL);

    // ?T == nil / nil == ?T: nil (TYPE_UNKNOWN) is always comparable to
    // any nullable type. Codegen reads the is_null flag directly — no
    // struct-to-nil LLVM comparison is emitted.
    if ((type_is_nullable(left_type) && right_type->kind == TYPE_UNKNOWN) ||
        (left_type->kind == TYPE_UNKNOWN && type_is_nullable(right_type))) {
        return bool_type;
    }

    // funcval == nil / nil == funcval (queue #3): a function value is only
    // comparable to nil, and only with == / != (Go forbids ordering and
    // funcval-to-funcval comparison). Codegen reads the fn-ptr word and
    // compares it to null — no struct-to-nil LLVM comparison is emitted.
    if ((op == TOKEN_EQ || op == TOKEN_NE) &&
        ((left_type->kind == TYPE_FUNCTION && right_type->kind == TYPE_UNKNOWN) ||
         (left_type->kind == TYPE_UNKNOWN && right_type->kind == TYPE_FUNCTION))) {
        return bool_type;
    }

    // iface == nil / nil == iface (RTTI follow-up): an interface value is
    // comparable to nil, with == / != only (Go forbids ordering; v1 also
    // defers interface-to-interface comparison). nil is TYPE_UNKNOWN. Codegen
    // reads the {vtable, data} words and tests both null, mirroring the
    // type-switch `case nil:` path so the two agree.
    if ((op == TOKEN_EQ || op == TOKEN_NE) &&
        ((left_type->kind == TYPE_INTERFACE && right_type->kind == TYPE_UNKNOWN) ||
         (left_type->kind == TYPE_UNKNOWN && right_type->kind == TYPE_INTERFACE))) {
        return bool_type;
    }

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

Type* type_check_bitwise_op(TypeChecker* checker, Type* left_type, Type* right_type, TokenType op, Position pos) {
    if (!checker || !left_type || !right_type) return NULL;

    // Bitwise operations require integer operands
    if (!type_is_integer(left_type) || !type_is_integer(right_type)) {
        type_error(checker, pos, "Bitwise operation requires integer operands");
        return NULL;
    }

    // A shift keeps the LEFT operand's type (Go: the count operand does not
    // affect the result type); other bitwise ops take the common integer type.
    // Both preserve unsigned width — `uint64 & uint64` is uint64, and
    // `x << s` for a uint64 x stays uint64 rather than collapsing to int32.
    int is_shift = (op == TOKEN_LSHIFT || op == TOKEN_RSHIFT);
    return integer_binop_result_type(left_type, right_type, is_shift);
}

Type* type_check_assignment_op(TypeChecker* checker, ASTNode* target, Type* target_type, Type* value_type, ASTNode* value_expr, Position pos) {
    if (!checker || !target || !target_type || !value_type) return NULL;

    // Fix 2 (comptime-param functions are not first-class values): `f = fill`
    // (f a func-typed variable) would rebind f to a Variable with no
    // func_decl_node — the SAME silent bypass adapt_var_decl_initializer's
    // sibling check guards against for `var f ... = fill`. Checked before the
    // ordinary compatibility logic below since a function type is never
    // numeric/interface and would otherwise just fall through the
    // type_compatible check unremarked.
    if (!reject_comptime_function_value(checker, value_expr, value_type, pos,
                                        "used as a value")) {
        return NULL;
    }

    // The grammar accepts any expression as an assignment LHS; enforce
    // addressability here. Lvalues are identifiers, index, selector, and deref
    // (`*p`, parsed as AST_UNARY_EXPR with TOKEN_MULTIPLY).
    if (target->type != AST_IDENTIFIER && target->type != AST_INDEX_EXPR &&
        target->type != AST_SELECTOR_EXPR &&
        !(target->type == AST_UNARY_EXPR &&
          ((UnaryExprNode*)target)->operator == TOKEN_MULTIPLY)) {
        type_error(checker, pos, "cannot assign to a non-addressable expression");
        return NULL;
    }

    // An interface-typed target accepts any concrete implementer and any
    // interface (check_interface_assign emits its own diagnostic). Mirrors the
    // var-decl init path so `s = Sq{}` / `t = s` behave like `var s Shape = …`.
    if (target_type->kind == TYPE_INTERFACE) {
        if (!check_interface_assign(checker, value_type, target_type, pos)) {
            return NULL;
        }
        return target_type;
    }

    // Check that value is compatible with target
    if (!type_compatible(value_type, target_type)) {
        type_error(checker, pos, "Cannot assign %s to %s",
                  type_to_string(value_type), type_to_string(target_type));
        return NULL;
    }
    
    // TODO: Check that target is assignable (lvalue)
    
    return target_type;
}