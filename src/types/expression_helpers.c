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
                // A negative constant shift count must NOT fold (Go rejects it
                // at compile time; the checker's shift arm reports it) — bail
                // so no consumer sees a bogus folded value. r >= 64 (checked
                // on the reinterpreted-unsigned value AFTER the sign check)
                // still folds to 0, matching Go's arbitrary-precision shift
                // truncated to 64 bits.
                case TOKEN_LSHIFT:
                    if ((int64_t)r < 0) return 0;
                    *out = (r >= 64) ? 0 : (l << r); return 1;
                case TOKEN_RSHIFT:
                    if ((int64_t)r < 0) return 0;
                    *out = (r >= 64) ? 0 : (l >> r); return 1;
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

// Comptime value params (fix round 6, C-r5): see the header doc comment.
// Unlike goo_type_contains_array above, this walk must descend into array
// ELEMENTS — `[2][n]int` carries the flag only on the INNER array type.
int goo_type_contains_comptime_array(const Type* t) {
    while (t) {
        switch (t->kind) {
            case TYPE_ARRAY:
                if (t->data.array.comptime_length) return 1;
                t = t->data.array.element_type;
                break;
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

// Comptime value params (fix round 4): see the header doc comment. Promoted
// from expression_checker.c's file-local helper (fix round 2's I2 guard)
// when type_from_ast (type_checker.c) became its third consumer — the
// comptime_length stamping on array types.
int goo_expr_references_comptime_param(TypeChecker* checker, ASTNode* expr) {
    if (!expr) return 0;
    switch (expr->type) {
        case AST_IDENTIFIER: {
            Variable* v = type_checker_lookup_variable(checker,
                ((IdentifierNode*)expr)->name);
            return v && v->decl_node && v->decl_node->type == AST_VAR_DECL &&
                   ((VarDeclNode*)v->decl_node)->is_comptime_param;
        }
        case AST_UNARY_EXPR:
            return goo_expr_references_comptime_param(checker,
                ((UnaryExprNode*)expr)->operand);
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)expr;
            return goo_expr_references_comptime_param(checker, b->left) ||
                   goo_expr_references_comptime_param(checker, b->right);
        }
        default:
            return 0;
    }
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
                // A negative constant shift count must NOT fold (Go rejects it
                // at compile time; the checker's shift arm reports it) — bail
                // so no consumer sees a bogus folded value. r >= 64 (checked
                // on the reinterpreted-unsigned value AFTER the sign check)
                // still folds to 0, matching Go's arbitrary-precision shift
                // truncated to 64 bits.
                case TOKEN_LSHIFT:
                    if ((int64_t)r < 0) return 0;
                    *out = (r >= 64) ? 0 : (l << r); return 1;
                case TOKEN_RSHIFT:
                    if ((int64_t)r < 0) return 0;
                    *out = (r >= 64) ? 0 : (l >> r); return 1;
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
        // try-precedence hint (P2.8 T4.4, diagnostic only — precedence is
        // unchanged, see the design doc's out-of-scope list): TRY is %right
        // and binds looser than arithmetic (parser.y), so `try f() + 1`
        // parses as `try (f() + 1)` and the arithmetic check sees the raw
        // !T operand and dies here with no clue why. When either operand is
        // an error union, append a hint toward the fix instead of leaving
        // the user to rediscover TRY's precedence from first principles.
        if (type_is_error_union(left_type) || type_is_error_union(right_type)) {
            type_error(checker, pos,
                       "Arithmetic operation requires numeric operands — "
                       "error unions must be unwrapped before arithmetic — "
                       "did you mean (try f()) + 1?");
        } else {
            type_error(checker, pos, "Arithmetic operation requires numeric operands");
        }
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

// T3 (P2.5): payload kinds with an existing equality lowering — int/float/
// bool/string/pointer — the exact restriction the design settles on
// ("restrict v1 to payload types that already have == "). Anything else
// (struct being the concrete acceptance case) is rejected by the NULLABLE
// comparison arms below with a positioned diagnostic instead of being
// silently accepted by type_equals' kind-only struct fallback (a known,
// out-of-scope gap documented at type_switch_case_type_same in
// type_checker.c) and left to crash codegen with "Failed to generate
// binary operation".
static int type_nullable_payload_supports_equality(const Type* t) {
    if (!t) return 0;
    return type_is_integer(t) || type_is_float(t) ||
           t->kind == TYPE_BOOL || t->kind == TYPE_STRING || t->kind == TYPE_POINTER;
}

// T3: are two nullable payload types the kind of "compatible" that permits
// comparing them? Deliberately NOT type_compatible — that predicate was
// written for ASSIGNMENT direction (its TYPE_NULLABLE arm only recurses
// into the base type when NULLABLE is the `to` argument), which is exactly
// what produced the direction-sensitive bug this task fixes (`T == ?T`
// accepted, `?T == T` rejected). A same-CATEGORY pair (both integer, or
// both float) is comparable at any width — codegen widens the narrower
// payload to match, mirroring codegen_create_nullable_with_value's
// sign/zero-extend precedent for wrapping a narrow literal into a wide
// nullable slot. Anything else (bool/string/pointer, or a genuine kind
// mismatch) falls back to exact type_equals — no int<->float promotion,
// which codegen has no mixed-kind lowering for.
static int type_nullable_bases_comparable(const Type* a, const Type* b) {
    if (type_is_integer(a) && type_is_integer(b)) return 1;
    if (type_is_float(a) && type_is_float(b)) return 1;
    return type_equals(a, b);
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

    // pointer/slice/map/chan == nil / nil == ... (P2.2 option A): the
    // remaining four Go-compatible bare-nil kinds (func handled above).
    // == / != only — Go forbids ordering pointer/slice/map/chan against
    // anything, nil included. Codegen reads the pointer word directly (a
    // slice extracts its backing-ptr field first) — no struct-to-nil LLVM
    // comparison beyond that single icmp is emitted.
    if ((op == TOKEN_EQ || op == TOKEN_NE) &&
        (((left_type->kind == TYPE_POINTER || left_type->kind == TYPE_SLICE ||
           left_type->kind == TYPE_MAP || left_type->kind == TYPE_CHANNEL) &&
          right_type->kind == TYPE_UNKNOWN) ||
         (left_type->kind == TYPE_UNKNOWN &&
          (right_type->kind == TYPE_POINTER || right_type->kind == TYPE_SLICE ||
           right_type->kind == TYPE_MAP || right_type->kind == TYPE_CHANNEL)))) {
        return bool_type;
    }

    // slice == slice / map == map / func == func, NEITHER operand nil (F2
    // follow-up to P2.2): Go allows these three kinds to compare ONLY to
    // nil, never to another value of their own kind (unlike pointer/chan,
    // which support full identity comparison — see the arm just below).
    // The nil-literal arms above already handle <kind> vs nil, so by the
    // time we reach here neither operand is TYPE_UNKNOWN; any same-kind
    // pair is genuinely two non-nil values. Without this explicit reject,
    // the generic type_compatible fallback further down would silently
    // ACCEPT identically-typed slice/map/func operands (type_equals treats
    // matching element/signature types as compatible) and hand codegen an
    // expression it cannot lower — a checker-accepts/codegen-dies split
    // (the accidental "Failed to generate binary operation" every one of
    // these produced before this arm existed). Message shape mirrors Go's
    // own: "invalid operation: ... (slice can only be compared to nil)".
    if ((op == TOKEN_EQ || op == TOKEN_NE) &&
        left_type->kind == right_type->kind &&
        (left_type->kind == TYPE_SLICE || left_type->kind == TYPE_MAP ||
         left_type->kind == TYPE_FUNCTION)) {
        const char* kind_name = left_type->kind == TYPE_SLICE ? "slice"
                               : left_type->kind == TYPE_MAP ? "map" : "func";
        type_error(checker, pos,
                  "invalid operation: %s %s %s (%s can only be compared to nil)",
                  type_to_string(left_type), op == TOKEN_EQ ? "==" : "!=",
                  type_to_string(right_type), kind_name);
        return NULL;
    }

    // struct == struct where the struct is NON-comparable (has a slice/map/
    // func/interface/array field): Go rejects this statically ("invalid
    // operation: ... (struct containing []T cannot be compared)"). Without
    // this arm the generic type_compatible fallback below ACCEPTS it (matching
    // struct types are compatible) and hands codegen a comparison it can only
    // lower via the value comparator — which, for a non-comparable struct,
    // would emit an illegal icmp over an aggregate field (the same defect
    // class the boxing path now routes to a runtime panic). A COMPARABLE
    // struct `==` is Go-legal but not yet lowered in v1; it deliberately does
    // NOT match here and falls through to the existing codegen limitation,
    // keeping "statically illegal" distinct from "valid but unimplemented".
    if ((op == TOKEN_EQ || op == TOKEN_NE) &&
        left_type->kind == TYPE_STRUCT && right_type->kind == TYPE_STRUCT &&
        type_equals(left_type, right_type) &&
        !type_struct_fields_comparable(left_type)) {
        type_error(checker, pos,
                  "invalid operation: %s %s %s (struct is not comparable: has a slice/map/func/interface field)",
                  type_to_string(left_type), op == TOKEN_EQ ? "==" : "!=",
                  type_to_string(right_type));
        return NULL;
    }

    // pointer == pointer / chan == chan, NEITHER operand nil (F2 follow-up
    // to P2.2): Go allows two pointers to compare by address (identity) and
    // two channels to compare by identity — DISTINCT from the slice/map/
    // func reject just above, which Go restricts to nil-only. The generic
    // type_compatible fallback further down would already accept this
    // (same pointee/element type via type_equals), so this arm exists to
    // DOCUMENT that ptr/chan identity comparison is intentional, not an
    // accidental byproduct — codegen has an explicit lowering for exactly
    // this case (icmp eq on the pointer word; see
    // codegen_generate_binary_expr's TOKEN_EQ/TOKEN_NE cases). A
    // mismatched pointee/element type (e.g. *int vs *string) does not
    // match this arm (left_type->kind == right_type->kind still holds, but
    // type_compatible fails) and falls through to the generic
    // "incompatible types" rejection below, same as before.
    if ((op == TOKEN_EQ || op == TOKEN_NE) &&
        left_type->kind == right_type->kind &&
        (left_type->kind == TYPE_POINTER || left_type->kind == TYPE_CHANNEL) &&
        type_compatible(left_type, right_type)) {
        return bool_type;
    }

    // Ordered comparison (< <= > >=) against nil is never valid for any of
    // the five reference-like kinds — Go: "operator < not defined on
    // pointer". Without this explicit gate, `nil < p` would slip through
    // the generic type_compatible fallback below: type_compatible is an
    // ASSIGNMENT-compatibility predicate (now accepts nil for these five
    // kinds per P2.2 option A) that doesn't know which operator is being
    // checked, so TYPE_UNKNOWN reads as compatible regardless of op. ==/!=
    // are the dedicated arms above (func's and this one's) and are
    // unaffected by this guard.
    if (op != TOKEN_EQ && op != TOKEN_NE) {
        if ((type_is_nilable_ref_kind(left_type) && right_type->kind == TYPE_UNKNOWN) ||
            (left_type->kind == TYPE_UNKNOWN && type_is_nilable_ref_kind(right_type))) {
            type_error(checker, pos, "Cannot compare incompatible types %s and %s",
                      type_to_string(left_type), type_to_string(right_type));
            return NULL;
        }
    }

    // ?a == ?b / T == ?b / ?a == T (T3, P2.5): tag-aware nullable equality,
    // symmetric in both mixed-operand orders. REPLACES the accidental
    // asymmetric acceptance the generic type_compatible fallback below
    // produced for `T == ?T` while rejecting the flipped `?T == T` (see
    // type_nullable_bases_comparable's comment) — this arm must run before
    // that fallback so neither shape ever reaches it. Reached only when
    // neither operand is the bare nil literal (that shape returned via the
    // ?T==nil arm at the top of this function).
    if (type_is_nullable(left_type) || type_is_nullable(right_type)) {
        // Ordered comparisons on any nullable operand are never valid —
        // without this guard `?int < ?int` would fall through to the
        // direction/operator-blind type_compatible fallback below, get
        // ACCEPTED (matching bases), and crash codegen with "Failed to
        // generate binary operation" (no ordering lowering exists for a
        // {i1,T} struct).
        if (op != TOKEN_EQ && op != TOKEN_NE) {
            const char* op_str = op == TOKEN_LT ? "<" : op == TOKEN_LE ? "<="
                                : op == TOKEN_GT ? ">" : ">=";
            Type* nullable_operand = type_is_nullable(left_type) ? left_type : right_type;
            type_error(checker, pos, "operator %s not defined on nullable type %s",
                      op_str, type_to_string(nullable_operand));
            return NULL;
        }

        Type* payload_type;
        if (type_is_nullable(left_type) && type_is_nullable(right_type)) {
            // ?a == ?b: both nullable.
            Type* lb = left_type->data.nullable.base_type;
            Type* rb = right_type->data.nullable.base_type;
            if (!type_nullable_bases_comparable(lb, rb)) {
                type_error(checker, pos, "Cannot compare incompatible types %s and %s",
                          type_to_string(left_type), type_to_string(right_type));
                return NULL;
            }
            payload_type = lb;
        } else {
            // T == ?b / ?a == T: exactly one operand nullable.
            Type* nullable_side = type_is_nullable(left_type) ? left_type : right_type;
            Type* plain_side = type_is_nullable(left_type) ? right_type : left_type;
            payload_type = nullable_side->data.nullable.base_type;
            if (!type_nullable_bases_comparable(plain_side, payload_type)) {
                type_error(checker, pos, "Cannot compare incompatible types %s and %s",
                          type_to_string(left_type), type_to_string(right_type));
                return NULL;
            }
        }

        // ?Struct == ?Struct (and any other unsupported payload kind) is
        // rejected here — a positioned diagnostic instead of an opaque
        // codegen crash. Struct equality is its own future feature.
        if (!type_nullable_payload_supports_equality(payload_type)) {
            type_error(checker, pos,
                      "invalid operation: %s %s %s (nullable payload type %s does not support equality)",
                      type_to_string(left_type), op == TOKEN_EQ ? "==" : "!=",
                      type_to_string(right_type), type_to_string(payload_type));
            return NULL;
        }

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
        // Fix round 5 (M-r4): whole-array assignment where EITHER side is a
        // comptime-length array (`b = a` with a: [n]int, or a2 = b) — the
        // template-time length here is the placeholder, so the length
        // comparison is meaningless until instance time; `[1]int64 vs
        // [4]int64` falsely rejected a valid program. Defer exactly the
        // LENGTH: element types must still match now (type_equals — Go's
        // array-assignment rule), and codegen's assignment path enforces
        // the real per-instance lengths (instance-named rejection on a
        // genuine mismatch — see codegen_generate_assignment's array arm).
        // Ordinary array assignments (no comptime_length on either side)
        // reject here exactly as before.
        int comptime_len_deferred =
            value_type && target_type &&
            value_type->kind == TYPE_ARRAY && target_type->kind == TYPE_ARRAY &&
            (value_type->data.array.comptime_length ||
             target_type->data.array.comptime_length) &&
            type_equals(value_type->data.array.element_type,
                        target_type->data.array.element_type);
        if (!comptime_len_deferred) {
            type_error(checker, pos, "Cannot assign %s to %s",
                      type_to_string(value_type), type_to_string(target_type));
            return NULL;
        }
    }
    
    // TODO: Check that target is assignable (lvalue)
    
    return target_type;
}