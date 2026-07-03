#include "types.h"
#include "comptime.h"  // comptime_context_lookup_func: order-independent func registry
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>   // literal_fits_type: ERANGE from strtoull (defensive branch)

// Forward declarations: the untyped-constant rootedness predicates and
// adapters are defined below (near type_check_binary_expr, their primary
// call site — see the doc comments there for the full design), but
// type_check_struct_literal's field-init sink (task 3) needs them earlier
// in this file, at struct-literal-loop scope.
//
// Task 3 (constant representability): the adapters now RANGE-CHECK a
// literal against its stamp target before stamping it (see
// literal_fits_type's doc comment below), so they can fail — return 0 (a
// type_error was already emitted) instead of silently truncating/wrapping.
// Every caller below must propagate that failure (return NULL/0) rather
// than proceed to codegen with a partially-adapted tree. `negated` tracks
// whether the node being adapted sits under an odd number of enclosing
// unary MINUS operators (threaded through the SAME recursion legs #101
// threaded for_float_context through — flipped only at the unary-MINUS leg;
// PLUS/binop legs pass it through unchanged, since they never change a
// literal's effective sign). `checkable` (int adapter only) disables the
// range check — but NOT the stamping — for an entire `^`-rooted subtree:
// Go folds `^` on the CONSTANT before overflow-checking it, which this
// stamp-and-compute checker cannot reproduce, so checking the RAW literal
// under a `^` would be outright wrong rather than merely imprecise (see the
// task-3 deviation note). Top-level callers always start with negated=0,
// checkable=1 — the recursion updates both as it descends.
static int is_untyped_int_rooted(ASTNode* n, int for_float_context);
static int adapt_untyped_int_operand(TypeChecker* checker, ASTNode* n, Type* target,
                                      int negated, int checkable);
static int is_untyped_float_rooted(ASTNode* n);
static int adapt_untyped_float_operand(TypeChecker* checker, ASTNode* n, Type* target,
                                        int negated);
// Task 3b: the composite-value adaptation helper (defined below, near
// type_check_struct_literal, its original #101 sink) is now ALSO the
// element-value hook for slice literals (check_slice_elements), array
// literals, and map literal values — all four sinks share the exact same
// "adapt an untyped numeric-rooted value to the declared slot type, with
// the task-3 range check" shape, so they share the one function instead of
// growing four copies. Forward-declared because the element sinks appear
// earlier in this file than the definition.
static Type* adapt_field_init_value(TypeChecker* checker, ASTNode* v, Type* field_type, Type* vt);

// Validate each element of a slice composite literal against the declared
// element type. Returns 1 on success; emits a type_error and returns 0 on the
// first incompatible element. Shared by the anonymous []T{} path and the named
// slice-type path so the element rules live in one place.
static int check_slice_elements(TypeChecker* checker, ASTNode* elements,
                                Type* want_elem, Position pos) {
    (void)pos; // reserved; per-element errors use e->pos for precision
    size_t i = 0;
    for (ASTNode* e = elements; e; e = e->next, i++) {
        // Keyed elements (`index: value`) are supported for array literals but
        // not yet for slices (a slice's backing length would grow to max
        // index + 1). Reject cleanly rather than fall through to a confusing
        // "Unknown expression type".
        if (e->type == AST_KEYED_ELEMENT) {
            type_error(checker, e->pos,
                       "keyed elements in slice literals are not yet supported "
                       "(use an array literal `[N]T{...}`)");
            return 0;
        }
        // Elided composite element `{...}`: thread the declared element type
        // into the literal so type_check_struct_literal can resolve it.
        if (e->type == AST_STRUCT_LITERAL &&
            ((StructLiteralNode*)e)->type_name == NULL) {
            e->node_type = want_elem;
        }
        Type* et = type_check_expression(checker, e);
        if (!et) return 0;
        // Task 3b: adapt an untyped numeric-rooted element to the declared
        // element type BEFORE the compat checks below — carries the task-3
        // range check, so `[]int8{300}` is rejected ("constant 300 overflows
        // int8", Go-conformant) instead of silently truncating to 44 at
        // codegen's width coercion. NULL means that rejection fired (error
        // already emitted). A float-rooted element meeting an integer
        // element type is deliberately NOT adapted (adapt_field_init_value's
        // #100 asymmetry), so it still falls through to the explicit
        // float->int rejection just below.
        et = adapt_field_init_value(checker, e, want_elem, et);
        if (!et) return 0;
        // A float element in an integer slice would silently truncate
        // (`[]int{1, 2.5, 3}` -> 1 0 3) — type_compatible wrongly permits it
        // as a numeric conversion. Reject the lossy float->int case explicitly.
        if (type_is_integer(want_elem)
            && (et->kind == TYPE_FLOAT32 || et->kind == TYPE_FLOAT64)) {
            type_error(checker, e->pos,
                       "Slice literal element %zu: cannot use float "
                       "value in a '%s' slice (would truncate)",
                       i, type_to_string(want_elem));
            return 0;
        }
        // An interface element type accepts any concrete implementer
        // (boxed at codegen); check_interface_assign emits its own
        // "does not implement" diagnostic.
        if (want_elem->kind == TYPE_INTERFACE) {
            if (!check_interface_assign(checker, et, want_elem, e->pos)) {
                return 0;
            }
        } else if (!type_compatible(et, want_elem)) {
            // type_compatible permits numeric widening (so []int64{1, 2} is
            // fine) but rejects e.g. string vs int.
            type_error(checker, e->pos,
                       "Slice literal element %zu type '%s' is not "
                       "compatible with declared element type '%s'",
                       i, type_to_string(et), type_to_string(want_elem));
            return 0;
        }
    }
    return 1;
}

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
                // Task 3b: same element adaptation + range check as the
                // slice/array sinks — `map[string]int8{"a": 300}` rejects
                // instead of silently truncating (keys are not adapted; the
                // runtime map is string-keyed, so a numeric key never gets
                // here).
                vt = adapt_field_init_value(checker, v, want_val, vt);
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
                // per-element (incl. the lossy float->int rejection).
                if (!check_slice_elements(checker, lit->elements, want, expr->pos))
                    return NULL;
                expr->node_type = declared;
                return declared;
            }

            // Goo-native untyped form `[1, 2, 3]`: element type inferred from
            // the first element; subsequent elements must be compatible.
            if (!lit->elements) {
                // Empty untyped slice — element type defaults to int (int64,
                // Go's default integer type) with no elements to infer from.
                Type* def = type_checker_get_builtin(checker, TYPE_INT64);
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
        case AST_ARRAY_LITERAL: {
            // `[N]T{e...}`: element type T from the declared array type, length
            // N const-folded; each element must be compatible with T (codegen
            // coerces widths), and at most N elements (Go zero-fills the rest).
            ArrayLitNode* lit = (ArrayLitNode*)expr;
            ArrayTypeNode* at = (ArrayTypeNode*)lit->array_type;
            if (!at || at->base.type != AST_ARRAY_TYPE) return NULL;
            Type* want = type_from_ast(checker, at->element_type);
            if (!want) {
                type_error(checker, expr->pos, "array literal: unknown element type");
                return NULL;
            }
            uint64_t n = 0;
            if (!at->length || !goo_fold_const_int(at->length, &n)) {
                type_error(checker, expr->pos,
                           "array literal: length must be a constant expression");
                return NULL;
            }
            // Elements may be keyed (`index: value`, a sparse Go table like
            // utf8 acceptRanges) or bare. A keyed element places its value at
            // the const index; an unkeyed element continues at previous + 1
            // (Go semantics). Gaps are zero-filled by codegen.
            int64_t cur = -1;               // last assigned index
            uint64_t max_idx_plus1 = 0;     // highest index + 1 seen
            for (ASTNode* e = lit->elements; e; e = e->next) {
                ASTNode* value = e;
                if (e->type == AST_KEYED_ELEMENT) {
                    KeyedElementNode* ke = (KeyedElementNode*)e;
                    uint64_t k = 0;
                    if (!goo_fold_const_int(ke->key, &k)) {
                        type_error(checker, e->pos,
                                   "array literal: element index must be a "
                                   "constant integer expression");
                        return NULL;
                    }
                    cur = (int64_t)k;
                    value = ke->value;
                } else {
                    cur += 1;
                }
                if (cur < 0 || (uint64_t)cur >= n) {
                    type_error(checker, e->pos,
                               "array literal: index %lld out of bounds for "
                               "length %llu",
                               (long long)cur, (unsigned long long)n);
                    return NULL;
                }
                if ((uint64_t)cur + 1 > max_idx_plus1) max_idx_plus1 = (uint64_t)cur + 1;

                // Elided composite value `{...}`: thread the declared array
                // element type in so the struct-literal checker can resolve it.
                if (value->type == AST_STRUCT_LITERAL &&
                    ((StructLiteralNode*)value)->type_name == NULL) {
                    value->node_type = want;
                }
                Type* et = type_check_expression(checker, value);
                if (!et) return NULL;
                // Task 3b: same element adaptation + range check as
                // check_slice_elements — this loop is a separate path (keyed/
                // sparse array support) but the identical one-hook shape, so
                // `[2]int8{300, 5}` rejects like `[]int8{300}` does.
                et = adapt_field_init_value(checker, value, want, et);
                if (!et) return NULL;
                if (!type_compatible(et, want)) {
                    type_error(checker, e->pos,
                               "array literal element at index %lld: cannot use "
                               "%s as %s",
                               (long long)cur, type_to_string(et), type_to_string(want));
                    return NULL;
                }
            }
            Type* arr = type_array(want, (size_t)n);
            expr->node_type = arr;
            return arr;
        }
        case AST_MATCH_EXPR:
            return type_check_match_expr(checker, expr);
        default:
            type_error(checker, expr->pos, "Unknown expression type");
            return NULL;
    }
}

// Composite field-init sink (task 3): adapt an untyped numeric-rooted field
// value (a bare literal, unary -/+/^, or arithmetic binop composed of those
// — see is_untyped_int_rooted / is_untyped_float_rooted's doc comments) to
// the struct field's declared type, mirroring type_check_binary_expr's
// int-int / float-float / cross-kind blocks but for a single field-value
// node rather than two binop operands. Without this, `Q{F: 2.5}` (F
// float32) reaches codegen with the literal still stamped FLOAT64 (its
// checker default) and codegen_build_struct_value only width-coerces
// INTEGER-kind field mismatches (SExt/Trunc) — a float-kind or width
// mismatch InsertValues the wrong-shaped constant, silently storing the
// double's raw bit pattern into the float32 slot (verified via
// --emit-llvm: `store %Q { double 2.5, ... }` into a `{float, double,
// i32}` %Q — the printed "double 2.5" bytes land where a 4-byte float is
// read back, producing 0; an int literal into a float64 field is worse,
// landing raw integer bits that read back as a denormal near-zero double).
//
// Returns the field's type if adaptation happened (the compat check below
// must use this instead of the pre-adaptation `vt`, same as `left_type =
// right_type` in the binop blocks), or `vt` unchanged otherwise (value
// isn't rooted — a typed variable, call, or conversion — or the field
// isn't numeric).
//
// Asymmetry preserved from #100 (float->int assignment rejection): an
// int-rooted value adapts to ANY numeric field (int or float), but a
// float-rooted value adapts ONLY to a float field — float->int is never
// adapted here, so `Q{N: 2.5}` (N int32) falls through to the existing
// type_compatible rejection below instead of silently truncating.
//
// Task 3: returns NULL (distinct from `vt`, which is never NULL when this
// is called — every call site already bailed on a NULL vt) when the
// adapter's range check rejects an out-of-range literal (e.g. `Q{N: 300}`,
// N int8) — the type_error was already emitted by the adapter. Callers must
// check for NULL and return NULL themselves.
//
// Task 3b: no longer struct-field-only — this is now the shared element
// sink for slice literals (check_slice_elements), array literals, and map
// literal values too (see the forward declaration's comment at the top of
// this file). "field_type" reads as "declared slot type" at those call
// sites; the semantics (including the #100 float->int asymmetry and the
// task-3 range check) are identical for all four.
static Type* adapt_field_init_value(TypeChecker* checker, ASTNode* v, Type* field_type, Type* vt) {
    if (!field_type || !type_is_numeric(field_type)) return vt;
    if (type_is_float(field_type)) {
        if (is_untyped_float_rooted(v)) {
            if (!adapt_untyped_float_operand(checker, v, field_type, 0)) return NULL;
            return field_type;
        }
        // for_float_context=1 on the int-rooted check: no shift, and no /
        // or % anywhere in the subtree may stamp float (see
        // is_untyped_int_rooted's doc comment) — `D: 1` (a bare int
        // literal) takes this leg; `F: 1 + 0.5` (mixed rooted) is already
        // float-rooted as a whole and takes the leg above it instead.
        if (is_untyped_int_rooted(v, 1)) {
            if (!adapt_untyped_int_operand(checker, v, field_type, 0, 1)) return NULL;
            return field_type;
        }
    } else if (type_is_integer(field_type)) {
        // for_float_context=0: an integer target, so shifts and /,% are
        // all safe to adapt (see is_untyped_int_rooted's doc comment).
        if (is_untyped_int_rooted(v, 0)) {
            if (!adapt_untyped_int_operand(checker, v, field_type, 0, 1)) return NULL;
            return field_type;
        }
    }
    return vt;
}

// `Point{x: 3, y: 4}` / `Point{3, 4}`. Omitted keyed fields take their
// zero value (Go semantics — matches the zero-initializing alloca that
// `var p Point` already gets in codegen); positional form must cover
// every declared field.
Type* type_check_struct_literal(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_STRUCT_LITERAL) return NULL;

    StructLiteralNode* lit = (StructLiteralNode*)expr;

    // Elided composite literal `{...}`: the type name is omitted and the target
    // type is inferred from context (the enclosing array/slice element type),
    // which the caller pre-stamps on expr->node_type before dispatching here.
    // Resolve the target struct type from that stamp instead of a by-name
    // lookup. Only struct element types are supported (the common table case).
    Type* named_type;
    if (lit->type_name == NULL) {
        named_type = expr->node_type;
        if (!named_type) {
            type_error(checker, expr->pos,
                       "elided composite literal '{...}' has no inferable type "
                       "in this context");
            return NULL;
        }
        if (named_type->kind != TYPE_STRUCT) {
            type_error(checker, expr->pos,
                       "elided composite literal '{...}' requires a struct "
                       "element type, got '%s'", type_to_string(named_type));
            return NULL;
        }
    } else {
        // Named types are registered in the variable scope by
        // type_check_type_decl (the Variable's type IS the named Type).
        Variable* named = type_checker_lookup_variable(checker, lit->type_name);
        if (!named || !named->type) {
            type_error(checker, expr->pos, "Unknown type '%s' in struct literal",
                       lit->type_name);
            return NULL;
        }
        named_type = named->type;
    }

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
        // Keyed form (e.g. `IntSlice{x: 3}`) is invalid for slice types —
        // slices have no named fields.
        if (lit->is_keyed) {
            type_error(checker, expr->pos,
                       "cannot use keyed (field-name) elements with slice type '%s'",
                       lit->type_name);
            return NULL;
        }
        Type* want = struct_type->data.slice.element_type;
        if (!want) {
            type_error(checker, expr->pos,
                       "Named slice type '%s' missing element type", lit->type_name);
            return NULL;
        }
        if (!check_slice_elements(checker, lit->field_values, want, expr->pos))
            return NULL;
        expr->node_type = struct_type;
        return struct_type;
    }

    // TODO(follow-up): named map/array composite literals (e.g. `type M map[K]V;
    // M{k: v}`) fall through to this rejection — supporting them is a deliberate
    // deferral until the map/array composite-literal lowering path is wired up.
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
            // Task 3: adapt an untyped numeric-rooted value (literal or
            // rooted expression) to the field's declared type BEFORE the
            // compat check — see adapt_field_init_value's doc comment. A
            // NULL return means a range check rejected an out-of-range
            // literal (error already emitted).
            vt = adapt_field_init_value(checker, v, field->type, vt);
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
            // Task 3: same field-value adaptation as the keyed loop above.
            vt = adapt_field_init_value(checker, v, fields[i].type, vt);
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
            // Go: an untyped integer constant's default type is `int` (64-bit
            // here). The shape-based literal adaptation retypes it to a sized
            // context (param/operand) where one applies; this is the fallback.
            type = type_checker_get_builtin(checker, TYPE_INT64);
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

// Is `n` an untyped-integer-constant-rooted operand — a bare int literal, a
// unary -/+/^ through to one, a shift whose (recursively) left operand is one
// (integer contexts only), or an arithmetic binop where BOTH sides are
// (recursively) int-rooted? A shift's Go result type is its LEFT operand's
// type, so `1<<32` is untyped-rooted through the `1`; an arithmetic binop's
// result type is the promoted operand type, so `(1+2)*3` is untyped-rooted
// through ALL its literals, not just one — this is what lets a
// parenthesized/chained untyped constant expression adapt to a sized context
// AS A UNIT (task 2), rather than only a single leaf literal. Used to decide
// whether an operand can adapt to the other, sized operand's type.
//
// for_float_context selects which shapes are safe for the ADAPTATION TARGET
// kind, and each leg passes it through unchanged to every recursive call, so
// an offending node buried anywhere in the subtree (e.g. the `1<<2` in
// `(1<<2)+3`, or the `1/2` in `(1/2)+3`) excludes the WHOLE expression:
//
//   for_float_context=0 (integer target: the int-int width block and the
//   call-arg adaptation pass, both below): shifts allowed, arithmetic ops
//   {+,-,*,/,%} allowed — everything stays integer-typed, so every shape
//   Go permits in an untyped int constant is safe to stamp.
//
//   for_float_context=1 (float target: the cross-kind block in
//   type_check_binary_expr and is_untyped_float_rooted's binop leg, both
//   below): NO shift leg — a shift must NEVER be stamped to a float type
//   (a shift's operands are integer-only, LLVM `shl`/`ashr` have no float
//   form, and Go itself doesn't let a shift float-adapt in a mixed
//   comparison either, so `1<<2 > g`, g float32, stays rejected) — and the
//   arithmetic leg shrinks to {+,-,*}, EXCLUDING / and %. Rationale (task-2
//   review adjudication): Go resolves an untyped INT constant division by
//   TRUNCATION before any later float promotion (`(1/2)*g` is exactly 0 in
//   Go), but this checker doesn't constant-fold — stamping `1/2`'s literals
//   float would compute an exact float division (0.5) instead, a silently
//   wrong VALUE on Go-legal code; and a float-stamped `%` node dies in
//   codegen (TOKEN_MODULO is integer-only, no frem is emitted). +,-,* have
//   no such divergence: integer and float arithmetic agree exactly for
//   values representable in both domains. Excluded shapes fall through to
//   the operator's own check (a clean "incompatible types"-class rejection
//   — stricter than Go for the / and % cases, wrong-value-proof).
//
// This one function replaces what used to be two near-identical ones
// (is_untyped_int_rooted / is_untyped_int_rooted_non_shift) merged via this
// flag — both were static, same-file helpers, so there was never a header-
// edit reason to keep them apart. expression_codegen.c's
// is_int_rooted_float_context duplicates the for_float_context=1 shape of
// this function (cross-referenced by comment, not shared through a header
// edit) — change them together.
static int is_untyped_int_rooted(ASTNode* n, int for_float_context) {
    if (!n) return 0;
    if (n->type == AST_LITERAL && ((LiteralNode*)n)->literal_type == TOKEN_INT)
        return 1;
    if (n->type == AST_UNARY_EXPR) {
        // Unary -/+/^ result type is the operand's type, so `-1`, `^0` are
        // untyped-rooted through the operand (lets a negative literal arg like
        // `f(-1)` adapt to an int32/rune parameter).
        UnaryExprNode* u = (UnaryExprNode*)n;
        if (u->operator == TOKEN_MINUS || u->operator == TOKEN_PLUS ||
            u->operator == TOKEN_BIT_XOR)
            return is_untyped_int_rooted(u->operand, for_float_context);
    }
    if (n->type == AST_BINARY_EXPR) {
        BinaryExprNode* b = (BinaryExprNode*)n;
        if (!for_float_context &&
            (b->operator == TOKEN_LSHIFT || b->operator == TOKEN_RSHIFT))
            return is_untyped_int_rooted(b->left, for_float_context);
        if (b->operator == TOKEN_PLUS || b->operator == TOKEN_MINUS ||
            b->operator == TOKEN_MULTIPLY ||
            (!for_float_context &&
             (b->operator == TOKEN_DIVIDE || b->operator == TOKEN_MODULO)))
            return is_untyped_int_rooted(b->left, for_float_context) &&
                   is_untyped_int_rooted(b->right, for_float_context);
    }
    return 0;
}

// Is `f` finite (not +-inf, not NaN)? Equivalent to the standard `isfinite`
// macro for a float, but implemented as a raw IEEE-754 bit test instead of
// calling it: glibc's <math.h> declares isfinite's helper prototypes
// (bits/mathcalls-helper-functions.h) with a `long double` overload
// unconditionally, and CompCert (this project's second, verified-C build
// target — see `make ccomp-link`) rejects `long double` outright, so merely
// including <math.h> for isfinite broke the ccomp build. A float's exponent
// field is all-ones (0xFF) iff it's +-inf or NaN; strtod followed by a
// (float) narrowing cast never produces NaN from finite input, so in
// practice this only ever distinguishes overflow-to-infinity from a finite
// result — exactly what literal_fits_type's float32 arm needs, with the
// same rounding boundary the (float)v cast itself uses (no separate FLT_MAX
// threshold to keep in sync).
static bool float32_is_finite(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

// 64-bit sibling of float32_is_finite (same ccomp-safe bit-test technique,
// same <math.h> avoidance rationale — see that function's doc comment): a
// double's exponent field is all-ones (0x7FF) iff it's +-inf or NaN.
// Needed because float64 overflow does NOT reach literal_fits_type as an
// out-of-range NUMERIC STRING: the lexer bridge converts every float
// literal to a C double with atof (lexer_bridge.c's TOKEN_FLOAT arm), which
// saturates `1e309` to +inf, and parser.y's FLOAT_LITERAL rule then
// re-serializes that double via snprintf("%f") — so the literal TEXT the
// checker receives is literally "inf", which strtod parses back to +inf
// with NO ERANGE. An errno-based overflow check therefore never fires for
// float64 (it shipped dead in the first task-3 commit); the finiteness of
// the parsed VALUE is the reliable signal. "inf" text can only arise from
// that lexer-side saturation — Goo has no infinity literal syntax — so a
// non-finite parse result here always means the source constant overflowed
// float64.
static bool float64_is_finite(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    return (bits & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
}

// Task 3 (constant representability): does literal `lit`'s value, under
// `negated` (an odd number of enclosing unary MINUS — see
// adapt_untyped_int_operand's doc comment), fit representably in `target`?
// Pure predicate — emits nothing; check_literal_range (below) is the sole
// caller and owns the "constant %s overflows %s" diagnostic, so every
// rejection is worded identically regardless of which call site triggered it.
//
// INT literal: `lit->value` is DECIMALIZED text, never the raw source
// spelling — the lexer bridge parses the source text (including 0x/0o/0b
// prefixes) with strtoull into a long long (lexer_bridge.c's TOKEN_INT arm;
// a magnitude beyond UINT64_MAX clamps there via ERANGE), and parser.y's
// INT_LITERAL rule re-serializes that value with snprintf("%lld"). So by
// the time text reaches this checker: no prefix ever survives (base 0 on
// the strtoull below is kept to mirror codegen_generate_literal's TOKEN_INT
// arm, which shares this parse, and as defense — it never actually sees a
// prefixed string), and the ERANGE branch below cannot fire in practice
// (every %lld-printed value re-parses in range) — it is defensive only, do
// not treat it as live overflow handling. A user-written negative literal
// like `-128` never reaches this text as "-128" either; it arrives as THIS
// SAME node under a unary-MINUS wrapper, with `negated` carrying the sign
// (the one way a leading `-` CAN appear in the text is %lld printing a
// bit-pattern above INT64_MAX, e.g. source 0xFFFFFFFFFFFFFFFF becomes
// "-1" — strtoull's well-defined negation wrap re-parses it to the same
// 64-bit pattern, so the unsigned-max comparisons below still see the
// correct value). An int literal meeting a FLOAT target always fits — Go
// converts a constant integer to float with rounding, never rejecting for
// magnitude (float64 exactly represents any int literal up to 2^53; Go
// itself doesn't reject beyond that either). A signed integer target fits
// iff v <= INT_MAX(target) (!negated), or v <= (uint64_t)INT_MAX(target)+1
// (negated — two's complement has exactly one more negative value than
// positive, which is the whole reason `-128` fits int8 though the raw
// magnitude 128 alone does not: INT8_MAX is 127, but negated allows one
// more). An unsigned target rejects any negation except literal 0 (`-0` is
// vacuously representable as 0), and otherwise fits iff v <= UINT_MAX(target).
//
// FLOAT literal: the text is likewise re-serialized (lexer bridge atof ->
// double -> parser.y's shortest-round-tripping %g loop, task 1), so a
// float64-overflowing source constant like 1e309 arrives as the TEXT "inf"
// (atof saturated it), which strtod parses back to +inf with NO ERANGE —
// see float64_is_finite's doc comment for why the finiteness of the parsed
// VALUE, not errno, is the overflow signal for both float widths. A
// float64 target fits iff the parsed double is finite. A float32 target
// fits iff the value survives a (float) narrowing cast finite — overflow
// produces +-inf, which is the rejection; underflow-to-zero is NOT an
// error (Go permits a constant that rounds to zero, only overflow
// rejects). `negated` is unused for the two float-target arms (sign is
// irrelevant to over/underflow there).
//
// FLOAT literal into an INTEGER target (task 2, e.g. `int8(200.0)`): this
// is the MAGNITUDE half of the check — does the value fit the target's
// numeric range at all — kept here so "constant 200 overflows int8" uses
// the same diagnostic (check_literal_range, below) as every other
// overflow case. The separate INTEGRALITY half (`int(3.5)` — value fits
// but has a fractional part) lives in check_literal_integral, called from
// check_conversion_operand_range ONLY after this function accepts, so an
// integral-but-overflowing value like 200.0 into int8 gets the overflow
// message, never the truncation one (Go agrees — see that function's doc
// comment for the ordering rationale). `negated` DOES matter here (unlike
// the float-target arms above): the parsed magnitude is sign-applied
// before the range comparison, mirroring the INT-literal arm above. Every
// bound below is exactly double-representable except the default
// (int64/uint64) case's magnitude limit, which is deliberately pinned at
// +-2^63 (itself exact) rather than the true INT64_MAX (2^63-1, NOT
// exactly representable — it rounds UP to 2^63 in a double, which would
// wrongly admit one out-of-range value) — this is also the exact safety
// margin check_literal_integral's `(long long)` truncation cast needs to
// stay defined (UB outside +-2^63). For UINT64 this is narrower than the
// type's true [0, UINT64_MAX] range (deliberately — same +-2^63 safety
// margin, no wider cast available); a float constant in [2^63, UINT64_MAX]
// is legal Go but is out of scope here, same documented-deviation shape as
// the INT-literal pipeline's LLONG_MAX clamp (task-3 report). No probe
// exercises that extreme.
static bool literal_fits_type(const LiteralNode* lit, const Type* target, bool negated) {
    if (!lit || !target) return true; // defensive; callers only invoke on numeric targets

    if (lit->literal_type == TOKEN_INT) {
        errno = 0;
        unsigned long long v = strtoull(lit->value, NULL, 0);
        if (errno == ERANGE) return false;
        if (type_is_float(target)) return true; // int-into-float: never rejected for magnitude
        if (!type_is_integer(target)) return true; // defensive

        if (type_is_signed(target)) {
            unsigned long long max;
            switch (target->kind) {
                case TYPE_INT8:  max = (unsigned long long)INT8_MAX;  break;
                case TYPE_INT16: max = (unsigned long long)INT16_MAX; break;
                case TYPE_INT32: max = (unsigned long long)INT32_MAX; break;
                default:         max = (unsigned long long)INT64_MAX; break; // int / int64
            }
            return negated ? v <= max + 1 : v <= max;
        }
        if (negated) return v == 0; // "-0" only; any other negative magnitude can't be unsigned
        unsigned long long max;
        switch (target->kind) {
            case TYPE_UINT8:  max = (unsigned long long)UINT8_MAX;  break;
            case TYPE_UINT16: max = (unsigned long long)UINT16_MAX; break;
            case TYPE_UINT32: max = (unsigned long long)UINT32_MAX; break;
            default:          max = UINT64_MAX; break; // uint / uint64
        }
        return v <= max;
    }

    if (lit->literal_type == TOKEN_FLOAT) {
        double v = strtod(lit->value, NULL);
        if (target->kind == TYPE_FLOAT32) return float32_is_finite((float)v);
        // Finiteness, NOT errno: the actual overflow shape here is the text
        // "inf" (parser-side saturation, no ERANGE) — see float64_is_finite's
        // doc comment. A hypothetical direct out-of-range numeric string
        // would ALSO land here as strtod's +-HUGE_VAL (= +-inf), so the one
        // test covers both; strtod underflow returns a finite value and is
        // correctly accepted (Go allows constants that round to zero).
        if (target->kind == TYPE_FLOAT64) return float64_is_finite(v);
        if (!type_is_integer(target)) return true; // defensive

        // Task 2: FLOAT literal meeting an INTEGER conversion target — see
        // this function's doc comment above for the magnitude-vs-
        // integrality split and the +-2^63 bound rationale.
        if (!float64_is_finite(v)) return false; // saturated "inf" fits no integer target
        double signed_v = negated ? -v : v;
        if (type_is_signed(target)) {
            switch (target->kind) {
                case TYPE_INT8:  return signed_v >= INT8_MIN  && signed_v <= INT8_MAX;
                case TYPE_INT16: return signed_v >= INT16_MIN && signed_v <= INT16_MAX;
                case TYPE_INT32: return signed_v >= INT32_MIN && signed_v <= INT32_MAX;
                default: // int / int64 — exact +-2^63 bound (INT64_MAX itself
                         // isn't exactly representable as a double; see the
                         // doc comment above)
                    return signed_v >= (double)INT64_MIN && signed_v < -(double)INT64_MIN;
            }
        }
        // Any strictly negative value can't be unsigned. "-0.0" is fine but
        // never reaches this return: IEEE -0.0 == 0.0, so `-0.0 < 0.0` is
        // false and it falls through to the max comparisons below (contrast
        // the INT-literal arm's `v == 0` check, whose integer negation has
        // no signed zero to exploit).
        if (signed_v < 0.0) return false;
        switch (target->kind) {
            case TYPE_UINT8:  return signed_v <= UINT8_MAX;
            case TYPE_UINT16: return signed_v <= UINT16_MAX;
            case TYPE_UINT32: return signed_v <= UINT32_MAX;
            default: // uint / uint64 — deliberately narrower than the true
                     // range; see the doc comment above
                return signed_v < -(double)INT64_MIN;
        }
    }

    return true; // other literal kinds (string/char/bool/nil) never reach this helper
}

// Emits "constant <text> overflows <target>" (mirroring Go's wording; a `-`
// prefix on the literal text when negated, so the message matches the
// actual source spelling — `-128`, not `128`) and returns 0 when
// literal_fits_type rejects; returns 1 (no error) when it fits. The single
// diagnostic site for every adapter/bridge call below, so "constant 300
// overflows int8" reads identically regardless of whether it came from a
// var-decl, a struct field, a call argument, or a binary-expr operand.
static int check_literal_range(TypeChecker* checker, const LiteralNode* lit,
                                const Type* target, bool negated, Position pos) {
    if (literal_fits_type(lit, target, negated)) return 1;
    type_error(checker, pos, "constant %s%s overflows %s",
               negated ? "-" : "", lit->value, type_to_string(target));
    return 0;
}

// Task 2 (float-literal-fidelity): does FLOAT literal `lit`'s value, under
// `negated`, have no fractional part? Pure predicate, sibling to
// literal_fits_type — that function's extended TOKEN_FLOAT-into-integer-
// target arm is the MAGNITUDE half of "does this float constant convert to
// this integer type"; this is the INTEGRALITY half.
// check_conversion_operand_range (below) calls this ONLY after
// literal_fits_type has already accepted, so the SIGN-APPLIED value is
// always finite AND within [-2^63, 2^63) here (that function's doc comment
// derives the bound) — which is exactly what makes the `(long long)`
// truncation cast below well-defined (it's UB outside +-2^63). That's why
// `negated` MUST be threaded in and applied before the cast even though
// sign can't change whether a value is integral (-3.5 is exactly as
// non-integral as 3.5): the fits check bounds the SIGN-APPLIED value, and
// at the one asymmetric boundary — `int64(-9223372036854775808.0)`,
// exactly INT64_MIN, which Go accepts — the raw MAGNITUDE is +2^63, one
// past LLONG_MAX, so casting it un-negated would be UB (x86 cvttsd2si
// saturates to LLONG_MAX, failing the round-trip compare and rejecting a
// legal Go program). No <math.h> trunc()/floor() needed either, consistent
// with this file's ccomp <math.h> avoidance (see float64_is_finite's doc
// comment for why).
static bool literal_is_integral(const LiteralNode* lit, bool negated) {
    double v = strtod(lit->value, NULL);
    double sv = negated ? -v : v;
    return (double)(long long)sv == sv;
}

// Emits "constant <text> truncated to integer" (mirroring Go's own
// rejection of e.g. `int(3.5)`; a `-` prefix on the literal text when
// negated, matching check_literal_range's convention) and returns 0 when
// literal_is_integral rejects; returns 1 (no error) when the value is
// integral.
static int check_literal_integral(TypeChecker* checker, const LiteralNode* lit,
                                   bool negated, Position pos) {
    if (literal_is_integral(lit, negated)) return 1;
    type_error(checker, pos, "constant %s%s truncated to integer",
               negated ? "-" : "", lit->value);
    return 0;
}

// Task 3b (review follow-up): range-check a CONSTANT conversion operand —
// `int8(300)` must reject ("constant 300 overflows int8", Go-conformant)
// while `int8(x)` (x a runtime value) stays legal truncation; that
// asymmetry is the whole point, and it's why this walks the operand's AST
// SHAPE instead of just asking for its type. Checked shapes: a bare
// INT/FLOAT literal, or one under unary -/+ (negation threading identical
// to adapt_untyped_int_operand's — flipped at MINUS, unchanged at PLUS).
// A `^`-rooted operand is excluded per the task-3 deviation (Go folds `^`
// on the constant; checking the raw literal under it would be wrong, not
// just imprecise). Everything else — identifiers, calls, nested
// conversions, index/selector expressions, and constant BINOPS like
// `int8(0 - 5)` (the append_coerce/elem_coerce goldens' idiom; also
// consistent with the task-3 "no constant folding" per-literal deviation
// for `100 + 100`) — is deliberately not checked: runtime conversions
// truncate legally, and folded expressions are out of this checker's
// stamp-and-compute reach.
//
// CHECK-ONLY, no stamping: unlike the adapters, this never writes
// node_type. Conversion codegen (call_codegen.c's codegen_numeric_convert)
// evaluates the operand at its own stamped type and casts — re-stamping
// the literal to the conversion target here would silently change which
// codegen path emits the constant, for zero benefit (the conversion's
// result type is already carried on the call node itself).
//
// Task 2 (float-literal-fidelity): a FLOAT literal converting to an
// INTEGER target must additionally be INTEGRAL — `int(3.5)` rejects
// ("constant 3.5 truncated to integer", Go-conformant) while `int(2.0)`
// stays legal (integral value) and a runtime `int(x)` (x float) stays
// legal truncation, same asymmetry as the overflow check above. ORDER
// MATTERS: check_literal_range (overflow/magnitude) runs FIRST and
// short-circuits on failure, so an integral-but-overflowing value like
// `int8(200.0)` gets the OVERFLOW message, never the truncation one (Go
// agrees) — and it's also what keeps check_literal_integral's `(long
// long)` truncation cast safe (literal_fits_type's extended float arm
// guarantees the SIGN-APPLIED value is within [-2^63, 2^63) by the time
// integrality is tested — the predicate applies `negated` before casting
// for exactly that reason; see both functions' doc comments).
//
// Rounding-boundary carve-out of the "Go-conformant" claim: integrality
// is judged on the lexer-rounded double, not the source decimal. A
// source-non-integral literal that ROUNDS to an integral double —
// `int64(9007199254740993.5)` rounds to 9007199254740994.0 — is accepted
// here where Go (arbitrary-precision constants) rejects it. Pre-existing
// consequence of the atof-at-lex constant pipeline, shared by every
// consumer of the literal text; recorded with the stamp-and-compute
// deviation. An arbitrary-precision-constants task inherits this case.
static int check_conversion_operand_range(TypeChecker* checker, ASTNode* n,
                                          Type* target, bool negated) {
    if (!n || !target || !type_is_numeric(target)) return 1;
    if (n->type == AST_LITERAL) {
        LiteralNode* lit = (LiteralNode*)n;
        if (lit->literal_type == TOKEN_INT || lit->literal_type == TOKEN_FLOAT) {
            if (!check_literal_range(checker, lit, target, negated, n->pos))
                return 0; // overflow — reported; ORDER: overflow before integrality
            if (lit->literal_type == TOKEN_FLOAT && type_is_integer(target))
                return check_literal_integral(checker, lit, negated, n->pos);
            return 1;
        }
        return 1; // char literal etc. — not range-checked here
    }
    if (n->type == AST_UNARY_EXPR) {
        UnaryExprNode* u = (UnaryExprNode*)n;
        if (u->operator == TOKEN_MINUS)
            return check_conversion_operand_range(checker, u->operand, target, !negated);
        if (u->operator == TOKEN_PLUS)
            return check_conversion_operand_range(checker, u->operand, target, negated);
        return 1; // `^`-rooted excluded (task-3 deviation); other unaries aren't constants
    }
    return 1; // runtime value / binop / nested conversion: legal truncation
}

// Retype an untyped-int-rooted operand — and, for a shift, the shift node and
// its left operand recursively, or for an arithmetic binop (+,-,*,/,%), the
// binop node and BOTH sides recursively — to `target`. This makes `1<<32` in
// `x >= 1<<32` (x uint64) compute at 64 bits instead of overflowing an int32
// shift to 0, and makes `(1+2)*g` (g float32) stamp `1`, `2`, AND the `1+2`
// node itself to float32 (task 2) so the whole parenthesized sub-expression
// computes at that width, not just a leaf literal feeding a still-int add.
// No for_float_context parameter here (unlike is_untyped_int_rooted): this
// is only ever called after that predicate confirmed the shape is adaptable
// FOR THE CALLER'S CONTEXT, so it just recurses through whatever shift/binop
// structure is actually there — in a float context the predicate already
// guaranteed no shift, /, or % exists anywhere in the subtree, so those
// arms simply never fire on a float target.
//
// Task 3: a literal leaf is now range-checked (check_literal_range) against
// `target` before being stamped, honoring `negated`/`checkable` (see this
// file's top-of-file doc comment on the forward declarations for both
// parameters' semantics). Returns 0 the moment a checked literal doesn't
// fit (the type_error was already emitted); every recursive call and every
// external caller must check this return and propagate failure rather than
// treat the tree as fully adapted.
static int adapt_untyped_int_operand(TypeChecker* checker, ASTNode* n, Type* target,
                                      int negated, int checkable) {
    if (!n) return 1;
    if (n->type == AST_LITERAL && ((LiteralNode*)n)->literal_type == TOKEN_INT) {
        if (checkable &&
            !check_literal_range(checker, (LiteralNode*)n, target, negated, n->pos))
            return 0;
        n->node_type = target;
        return 1;
    }
    if (n->type == AST_UNARY_EXPR) {
        UnaryExprNode* u = (UnaryExprNode*)n;
        if (u->operator == TOKEN_MINUS) {
            // Sign flips for a range check ONLY at unary MINUS — PLUS and
            // BIT_XOR (below) never change a literal's effective magnitude
            // the way MINUS does.
            if (!adapt_untyped_int_operand(checker, u->operand, target, !negated, checkable))
                return 0;
            n->node_type = target;
        } else if (u->operator == TOKEN_PLUS || u->operator == TOKEN_BIT_XOR) {
            // `^`-rooted literals are excluded from range checking (never
            // re-enabled once excluded, hence `checkable && ...` rather than
            // an unconditional 0) — see this function's doc comment and the
            // task-3 deviation note. The stamping side effect is unchanged
            // for `^`, matching pre-task-3 behavior exactly.
            int child_checkable = checkable && (u->operator != TOKEN_BIT_XOR);
            if (!adapt_untyped_int_operand(checker, u->operand, target, negated, child_checkable))
                return 0;
            n->node_type = target;
        }
    }
    if (n->type == AST_BINARY_EXPR) {
        BinaryExprNode* b = (BinaryExprNode*)n;
        if (b->operator == TOKEN_LSHIFT || b->operator == TOKEN_RSHIFT) {
            if (!adapt_untyped_int_operand(checker, b->left, target, negated, checkable)) // shift type = left type
                return 0;
            n->node_type = target;
        } else if (b->operator == TOKEN_PLUS || b->operator == TOKEN_MINUS ||
                   b->operator == TOKEN_MULTIPLY || b->operator == TOKEN_DIVIDE ||
                   b->operator == TOKEN_MODULO) {
            // Each side keeps ITS OWN negated/checkable state — a binary
            // MINUS does not flip either operand's sign for range-check
            // purposes (only a unary MINUS wrapping a leaf does that); this
            // is exactly the per-literal (no constant-folding) design that
            // lets `100 + 100` into int8 through uncaught (each 100
            // individually fits) — see the task-3 deviation note.
            if (!adapt_untyped_int_operand(checker, b->left, target, negated, checkable))
                return 0;
            if (!adapt_untyped_int_operand(checker, b->right, target, negated, checkable)) // binop result = operand type
                return 0;
            n->node_type = target;
        }
    }
    return 1;
}

// Float analogue of is_untyped_int_rooted: is `n` an untyped-float-constant-
// rooted operand — a bare float literal, a unary -/+ through to one, or an
// arithmetic binop (+,-,*,/ — floats have no `%`) where EACH side is float-
// rooted OR int-rooted for a float context (is_untyped_int_rooted(side, 1):
// no shift, and no / or % anywhere in the int subtree — see that function's
// doc comment for both exclusions), with AT LEAST ONE side float-rooted? An
// all-int binop like `1+2` stays int-rooted only (see is_untyped_int_rooted)
// — this leg is for a MIXED int+float constant expression, which Go's kind-
// promotion rule makes float overall (e.g. `1 + 0.5`, or `1` meeting an
// already-float-rooted `(2.0*3.0)` sibling). Floats have no shift/^
// operator, so those legs of the int version don't apply here.
//
// Note the asymmetry for `/`: THIS function's own binop leg keeps `/` —
// a division that itself contains a float literal (`0.5 / 2`) is a FLOAT
// division in Go's constant arithmetic (kind promotion happens before the
// divide), exact-or-precision-class, so it may adapt — while an all-INT
// division subtree (`1/2` in `0.5 * (1/2)`) is truncating in Go and is
// excluded via the int-rooted side test above, falling through to a clean
// rejection instead of a silently wrong value.
//
// expression_codegen.c's is_float_literal_node duplicates this exact shape
// (cross-referenced by comment rather than shared through a header edit —
// see task-2 brief for the rationale) — change them together.
static int is_untyped_float_rooted(ASTNode* n) {
    if (!n) return 0;
    if (n->type == AST_LITERAL && ((LiteralNode*)n)->literal_type == TOKEN_FLOAT)
        return 1;
    if (n->type == AST_UNARY_EXPR) {
        // Unary -/+ result type is the operand's type, so `-0.1`, `+0.1` are
        // untyped-rooted through the operand.
        UnaryExprNode* u = (UnaryExprNode*)n;
        if (u->operator == TOKEN_MINUS || u->operator == TOKEN_PLUS)
            return is_untyped_float_rooted(u->operand);
    }
    if (n->type == AST_BINARY_EXPR) {
        BinaryExprNode* b = (BinaryExprNode*)n;
        if (b->operator == TOKEN_PLUS || b->operator == TOKEN_MINUS ||
            b->operator == TOKEN_MULTIPLY || b->operator == TOKEN_DIVIDE) {
            int left_ok  = is_untyped_float_rooted(b->left)  || is_untyped_int_rooted(b->left, 1);
            int right_ok = is_untyped_float_rooted(b->right) || is_untyped_int_rooted(b->right, 1);
            return left_ok && right_ok &&
                   (is_untyped_float_rooted(b->left) || is_untyped_float_rooted(b->right));
        }
    }
    return 0;
}

// Retype an untyped-float-rooted operand — and, for unary -/+, the unary node
// and its operand recursively, or for an arithmetic binop (+,-,*,/), the
// binop node and BOTH sides recursively — to `target`. Mirrors
// adapt_untyped_int_operand. Only ever called after is_untyped_float_rooted(n)
// confirmed true, so a binop's sides are each guaranteed float-rooted or
// float-context int-rooted (no shift, /, or %) by that precondition; a
// float-rooted side recurses here, an int-rooted side dispatches to
// adapt_untyped_int_operand (which
// stamps node_type = target regardless of target's own kind, so handing it a
// float target is exactly how an int leg of a mixed `1 + 0.5` gets stamped
// float here).
//
// Task 3: `negated` mirrors adapt_untyped_int_operand's (flipped only at
// unary MINUS, unchanged at PLUS/binop legs); a float literal leaf is
// range-checked via check_literal_range before stamping (float has no `^`,
// so there is no `checkable` parameter to thread here — every float literal
// this function reaches is always checkable). An int-rooted binop side
// dispatches to adapt_untyped_int_operand with checkable=1 (that function's
// own `^` handling takes over from there if the side happens to be
// `^`-rooted). Returns 0 the moment a checked literal doesn't fit (error
// already emitted); callers must propagate the failure.
static int adapt_untyped_float_operand(TypeChecker* checker, ASTNode* n, Type* target,
                                        int negated) {
    if (!n) return 1;
    if (n->type == AST_LITERAL && ((LiteralNode*)n)->literal_type == TOKEN_FLOAT) {
        if (!check_literal_range(checker, (LiteralNode*)n, target, negated, n->pos))
            return 0;
        n->node_type = target;
        return 1;
    }
    if (n->type == AST_UNARY_EXPR) {
        UnaryExprNode* u = (UnaryExprNode*)n;
        if (u->operator == TOKEN_MINUS || u->operator == TOKEN_PLUS) {
            int child_negated = (u->operator == TOKEN_MINUS) ? !negated : negated;
            if (!adapt_untyped_float_operand(checker, u->operand, target, child_negated)) // unary result = operand type
                return 0;
            n->node_type = target;
        }
    }
    if (n->type == AST_BINARY_EXPR) {
        BinaryExprNode* b = (BinaryExprNode*)n;
        if (b->operator == TOKEN_PLUS || b->operator == TOKEN_MINUS ||
            b->operator == TOKEN_MULTIPLY || b->operator == TOKEN_DIVIDE) {
            int ok = is_untyped_float_rooted(b->left)
                ? adapt_untyped_float_operand(checker, b->left, target, negated)
                : adapt_untyped_int_operand(checker, b->left, target, negated, 1);
            if (!ok) return 0;
            ok = is_untyped_float_rooted(b->right)
                ? adapt_untyped_float_operand(checker, b->right, target, negated)
                : adapt_untyped_int_operand(checker, b->right, target, negated, 1);
            if (!ok) return 0;
            n->node_type = target; // binop result = operand type
        }
    }
    return 1;
}

// Task 3 bridge for type_checker.c's type_check_var_decl (:800) — a var-decl
// initializer is the ONE typed sink in this checker that does NOT already
// route through the adapters above: type_check_var_decl calls
// type_check_expression on the initializer and leaves it at its checker-
// default INT64/FLOAT64 stamp, relying on codegen_apply_local_init_pipeline's
// later width-coerce step to narrow it to the declared type — which
// TRUNCATES/WRAPS instead of rejecting (`var b int8 = 300` -> 44, the
// original task-3 bug). Mirrors adapt_field_init_value's int/float/
// cross-kind dispatch shape (same is_untyped_int_rooted/is_untyped_float_
// rooted gating), but for a var-decl's single initializer expression against
// its declared type, and shares the exact SAME range-checking/stamping code
// (adapt_untyped_int_operand/adapt_untyped_float_operand) rather than
// duplicating it.
//
// Declared non-static and NOT forward-declared at the top of this file
// (unlike the struct-literal-loop predicates/adapters) because it has
// exactly one external caller, in a different translation unit:
// type_checker.c forward-declares its own `extern` prototype locally rather
// than pulling in a header change, per this task's "no header/parser
// changes" constraint — see the task-3 report for the full rationale.
//
// Returns 1 on success (adapted-and-in-range, or nothing adaptable — a
// typed variable/call/conversion RHS, or a non-numeric declared type, is
// left untouched exactly as before), 0 on a range violation (the adapter
// already emitted "constant ... overflows ...").
//
// Review fix: a NULLABLE declared type (`?int8`) wraps a numeric base at
// the type level, but is itself TYPE_NULLABLE, not numeric — so
// `type_is_numeric(declared)` used to bail immediately and `var gz ?int8 =
// 300` skipped range-checking entirely, deferring to codegen's width-coerce
// step, which truncates/wraps instead of rejecting (printed 44). Unwrap to
// the base type FIRST and range-check/stamp against THAT: the literal gets
// stamped as the plain (non-nullable) base type, exactly as it would for a
// non-nullable `var b int8 = 300`. Codegen's nullable auto-wrap
// (codegen_create_nullable_with_value) then receives an already-valid,
// already-narrowed value and just inserts it into the { i1, T } struct's
// slot 1 — it never needs to know a nullable declared type was involved.
int adapt_var_decl_initializer(TypeChecker* checker, ASTNode* value, Type* declared) {
    if (!value || !declared) return 1;
    Type* target = (declared->kind == TYPE_NULLABLE) ? declared->data.nullable.base_type
                                                       : declared;
    if (!target || !type_is_numeric(target)) return 1;
    if (type_is_float(target)) {
        if (is_untyped_float_rooted(value))
            return adapt_untyped_float_operand(checker, value, target, 0);
        // for_float_context=1: no shift, and no / or % anywhere in the
        // subtree may stamp float (is_untyped_int_rooted's doc comment) —
        // mirrors adapt_field_init_value's identical gate.
        if (is_untyped_int_rooted(value, 1))
            return adapt_untyped_int_operand(checker, value, target, 0, 1);
        return 1;
    }
    if (type_is_integer(target) && is_untyped_int_rooted(value, 0))
        return adapt_untyped_int_operand(checker, value, target, 0, 1);
    return 1;
}

// CHANGE-TOGETHER (task 2, checker/codegen hygiene): this function is the
// SOLE place that computes a binary expression's result type, and it is the
// SOLE place that RECORDS it, on `expr->node_type`, at each of its two
// successful returns (the `_ = rhs` short-circuit just below, and the
// switch-based exit at the bottom). That establishes the invariant codegen
// now relies on: after a successful checker pass, every AST_BINARY_EXPR
// carries its result type in `node_type` — codegen (expression_codegen.c,
// codegen_generate_binary_expr) reads that recording instead of re-invoking
// this function mid-codegen. If a new successful-return path is ever added
// here, it MUST set `expr->node_type` before returning, or codegen's
// NULL-recording check trips a loud "compiler bug" error instead of a
// silent misdispatch. A failing return (NULL) is exempt — codegen never
// reaches a node the checker rejected.
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
        expr->node_type = rhs_type;  // records — see the invariant comment above
        return rhs_type;
    }

    Type* left_type = type_check_expression(checker, binary->left);
    Type* right_type = type_check_expression(checker, binary->right);

    if (!left_type || !right_type) return NULL;

    // Narrow integer-literal adaptation for binary ops: if exactly one operand
    // is an untyped integer literal and the other is a differently-sized integer,
    // retype the literal to the other operand's type. LLVM binary ops (and
    // shifts) require both operands to share a type, and codegen emits a retyped
    // literal at that width (see codegen_generate_literal). This is what lets
    // `x + 1`, `x >> 8`, `x & m` compute in uint64 rather than mixing widths.
    if (type_is_integer(left_type) && type_is_integer(right_type) &&
        left_type->kind != right_type->kind) {
        // An operand adapts if it is untyped-integer-constant-rooted: a bare int
        // literal, a shift through to one (`1<<32`), or an arithmetic binop
        // through to two (`(1+2)<<32`). for_float_context=0: this is an
        // all-integer context, so shifts and /,% are all safe to adapt (see
        // is_untyped_int_rooted's doc comment). Adapt it to the other, sized
        // operand's type so both compute at the same width — this is what
        // stops `1<<32` in `x >= 1<<32` from overflowing an int32 shift to 0.
        int left_adaptable  = is_untyped_int_rooted(binary->left, 0);
        int right_adaptable = is_untyped_int_rooted(binary->right, 0);
        if (right_adaptable && !left_adaptable) {
            if (!adapt_untyped_int_operand(checker, binary->right, left_type, 0, 1)) return NULL;
            right_type = left_type;
        } else if (left_adaptable && !right_adaptable) {
            if (!adapt_untyped_int_operand(checker, binary->left, right_type, 0, 1)) return NULL;
            left_type = right_type;
        }
    }

    // Float analogue: an untyped float literal (e.g. `2.0` in `g * 2.0`, g
    // float32) is checker-stamped FLOAT64 by type_check_literal. Without this
    // adaptation the literal's stamped type never narrows, so
    // type_check_arithmetic_op's "either side FLOAT64 -> FLOAT64" rule always
    // wins and the checker stamps a float64 result for an expression codegen
    // computes at float32 — the checker/codegen disagreement this task fixes.
    // Adapting BEFORE result-type computation (mirroring the int block above)
    // makes the result come out float32. Applies to both arithmetic and
    // comparison operators (this runs before the switch on binary->operator).
    //
    // is_untyped_float_rooted's binop leg (task 2) is what makes this block
    // handle a CHAINED product like `2.0 * 3.0 * g` (parsed `(2.0*3.0) * g`):
    // the `2.0*3.0` sub-expression is itself float-rooted now, so it adapts
    // here as a unit, recursing adapt_untyped_float_operand down into its own
    // `2.0` and `3.0` leaves — not just a single leaf literal sitting
    // directly next to `g`.
    if (type_is_float(left_type) && type_is_float(right_type) &&
        left_type->kind != right_type->kind) {
        // An operand adapts if it is untyped-float-constant-rooted: a bare
        // float literal, a unary -/+ through to one, or an arithmetic binop
        // through to some mix of those (see is_untyped_float_rooted). A typed
        // conversion like `float32(0.1)` is NOT literal-rooted (it's a call
        // expression) and never adapts — only the untyped literal side does.
        int left_adaptable  = is_untyped_float_rooted(binary->left);
        int right_adaptable = is_untyped_float_rooted(binary->right);
        if (right_adaptable && !left_adaptable) {
            if (!adapt_untyped_float_operand(checker, binary->right, left_type, 0)) return NULL;
            right_type = left_type;
        } else if (left_adaptable && !right_adaptable) {
            if (!adapt_untyped_float_operand(checker, binary->left, right_type, 0)) return NULL;
            left_type = right_type;
        }
    }

    // Cross-kind adaptation: an untyped-int-rooted operand (a bare int
    // literal, unary -/+/^ through to one, or a {+,-,*} binop through to
    // some mix of those — for_float_context=1: NO shift and NO / or %
    // anywhere in the subtree, see is_untyped_int_rooted's doc comment for
    // both exclusions) meeting a float-kind operand (a typed float variable,
    // or the other side already float-stamped by one of the two blocks
    // above) adapts to that float type. Without this, `1 < g` (g float32)
    // reaches codegen as (INT64, FLOAT32) and codegen_generate_binary_expr
    // emits `icmp slt i64, float`, which the LLVM verifier rejects; `g > 1`
    // was already a clean type_error before this task (stricter than Go,
    // which permits both orders). The binop leg (task 2) extends this the
    // same way: `(1+2) * g` reaches here with binary->left = the whole
    // `1+2` node, which is now int-rooted as a unit, so it adapts (and
    // recurses into its own `1`/`2` leaves) instead of computing an integer
    // add that a float multiply can't consume. An int subtree containing
    // / or % (e.g. `(1/2) * g`, `(1%2) * g`) is NOT rooted here and falls
    // through to the operator's own clean rejection — Go truncates that
    // division/modulo as an INT constant before promotion, which a
    // stamp-and-compute checker can't reproduce; rejecting beats silently
    // computing 0.5 where Go gets 0. Reuses adapt_untyped_int_operand — it
    // never sees a shift, `/`, or `%` node here because
    // is_untyped_int_rooted(_, 1) excludes those shapes.
    //
    // An int VARIABLE (not int-rooted) meeting a float is left alone — the
    // mismatch falls through to whatever the operator's own check does (e.g.
    // type_check_comparison_op's "incompatible types" rejection), matching
    // Go (no implicit int-to-float conversion for typed values).
    //
    // Untyped×untyped cross-kind (task 2, e.g. `0.1 * 10`): left_type is
    // FLOAT64 (from type_check_literal on `0.1`) and right_type is INT64
    // (from `10`) BEFORE this block runs, so it already lands in the
    // type_is_float(left)&&type_is_integer(right) arm below with no
    // additional code — a bare int literal is trivially int-rooted. Verified
    // this composes correctly as-is; nothing further was needed here.
    //
    // Gated against TOKEN_MODULO as the CURRENT operator (final-sweep fix):
    // the exclusion above only looks at the operand's OWN subtree (does IT
    // contain a shift/`/`/`%`), so a bare `1` in `1 % g` (g float32) is
    // trivially int-rooted and would otherwise adapt here, leaving
    // type_check_arithmetic_op to face a float×float `%` — a pre-existing
    // hole where it returns FLOAT32 for a modulo codegen can't emit (its
    // TOKEN_MODULO arm is integer-only), crashing with an opaque "Failed to
    // generate binary operation" instead of a diagnostic. Go itself rejects
    // `1 % g` (mismatched types), so skipping adaptation when `%` is the
    // operator directly combining these two operands lets the mismatch fall
    // through unchanged to the rejection switch below, which already has a
    // clean TOKEN_MODULO case. `/` is deliberately NOT gated here — `1 / g`
    // must keep adapting and computing (see the constdiv/constmod probes for
    // the DIFFERENT, subtree-shaped exclusion that governs `(1%2) * g`).
    if (binary->operator != TOKEN_MODULO) {
        if (type_is_integer(left_type) && type_is_float(right_type) &&
            is_untyped_int_rooted(binary->left, 1)) {
            if (!adapt_untyped_int_operand(checker, binary->left, right_type, 0, 1)) return NULL;
            left_type = right_type;
        } else if (type_is_float(left_type) && type_is_integer(right_type) &&
                   is_untyped_int_rooted(binary->right, 1)) {
            if (!adapt_untyped_int_operand(checker, binary->right, left_type, 0, 1)) return NULL;
            right_type = left_type;
        }
    }

    // Cross-kind mix that did NOT adapt above — an int VARIABLE meeting a
    // float (`x > g`), a shift-rooted operand (`1<<2 > g`), or an all-int
    // /,% subtree in a float context (`(1/2) * g`, `(1%2) * g`,
    // `0.5 * (1/2)`) — must be REJECTED here for arithmetic and comparison
    // operators, not allowed through to codegen. Before this check, only
    // the float-LEFT comparison order got a clean rejection
    // (type_check_comparison_op via type_compatible, which is asymmetric:
    // float->int rejected, int->float permitted); the int-left comparison
    // order and EVERY arithmetic mix sailed through
    // (type_check_arithmetic_op happily returns the promoted float type)
    // and crashed the LLVM verifier with mismatched-operand-type errors
    // (`mul i64 0, float %g`, `icmp sgt i64 4, float %g`). Go rejects all
    // of these shapes ("invalid operation: mismatched types") except the
    // /,% constant cases, which Go computes by exact constant arithmetic
    // (truncating int division BEFORE promotion) — a stamp-and-compute
    // checker can't reproduce that, so rejecting beats silently computing
    // 0.5 where Go gets 0 (task-2 review adjudication). Only arithmetic and
    // comparison operators are gated: assignment ops keep their own
    // type_check_assignment_op/type_compatible path (int->float assignment
    // stays permitted, codegen coerces), and bitwise/shift/logical ops
    // already reject non-integer/non-bool operands in their own checks.
    if ((type_is_integer(left_type) && type_is_float(right_type)) ||
        (type_is_float(left_type) && type_is_integer(right_type))) {
        switch (binary->operator) {
            case TOKEN_EQ:
            case TOKEN_NE:
            case TOKEN_LT:
            case TOKEN_LE:
            case TOKEN_GT:
            case TOKEN_GE:
                // Same wording type_check_comparison_op produces for the
                // float-left order, so the diagnostic is consistent across
                // operand orders.
                type_error(checker, expr->pos,
                          "Cannot compare incompatible types %s and %s",
                          type_to_string(left_type), type_to_string(right_type));
                return NULL;
            case TOKEN_PLUS:
            case TOKEN_MINUS:
            case TOKEN_MULTIPLY:
            case TOKEN_DIVIDE:
            case TOKEN_MODULO:
                type_error(checker, expr->pos,
                          "Invalid operation: mismatched types %s and %s "
                          "(no implicit int/float conversion; use an explicit conversion)",
                          type_to_string(left_type), type_to_string(right_type));
                return NULL;
            default:
                break; // other operators keep their own checks below
        }
    }

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
        case TOKEN_AND_NOT:   // &^  (bit-clear: a & ~b)
        case TOKEN_BIT_OR:
        case TOKEN_BIT_XOR:
        case TOKEN_LSHIFT:
        case TOKEN_RSHIFT:
            result_type = type_check_bitwise_op(checker, left_type, right_type, binary->operator, expr->pos);
            break;
            
        // Assignment operators
        // Plain and compound assignment. A compound assign `x op= e` is checked
        // like `x = e` (target addressable, e compatible with the target's
        // type); codegen lowers it to `x = x op e`. The narrow literal
        // adaptation above already retyped an int literal RHS to the target.
        case TOKEN_ASSIGN:
        case TOKEN_PLUS_ASSIGN:
        case TOKEN_MINUS_ASSIGN:
        case TOKEN_MUL_ASSIGN:
        case TOKEN_DIV_ASSIGN:
        case TOKEN_MOD_ASSIGN:
        case TOKEN_AND_ASSIGN:
        case TOKEN_OR_ASSIGN:
        case TOKEN_XOR_ASSIGN:
        case TOKEN_LSHIFT_ASSIGN:
        case TOKEN_RSHIFT_ASSIGN:
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

    // Records — see the invariant comment at this function's top. This is
    // the exit every arithmetic/comparison/logical/bitwise/assignment/
    // channel-send case above falls through to (each sets `result_type` and
    // `break`s); `result_type` may legitimately be NULL here if the helper
    // it called already emitted a type_error, which correctly propagates as
    // an unset (NULL) node_type — codegen never reaches a rejected node.
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
            
        case TOKEN_BIT_NOT:   // ~x
        case TOKEN_BIT_XOR:   // ^x  (Go bitwise complement; same token as binary XOR)
            if (!type_is_integer(operand_type)) {
                type_error(checker, expr->pos,
                          "Bitwise not requires integer type, got %s",
                          type_to_string(operand_type));
                return NULL;
            }
            result_type = operand_type;
            break;
            
        case TOKEN_BIT_AND:  // & - take reference/borrow
            // Go allows & on any composite literal; Goo supports only the
            // struct case (heap-allocated by codegen). Reject the other
            // literal kinds here with a specific error — without this they
            // pass typecheck (this arm makes a pointer to ANY operand) and die in
            // codegen with the unhelpful "Cannot take address of non-lvalue".
            if (unary->operand->type == AST_SLICE_EXPR ||
                unary->operand->type == AST_ARRAY_LITERAL ||
                unary->operand->type == AST_PAREN_EXPR) {
                // AST_PAREN_EXPR is the map-literal tag (grouping parens
                // parse as identity and never produce a node — ast.h:638).
                const char* kind = unary->operand->type == AST_SLICE_EXPR ? "slice"
                                 : unary->operand->type == AST_ARRAY_LITERAL ? "array"
                                 : "map";
                type_error(checker, expr->pos,
                          "cannot take the address of a %s literal (only struct literals are supported)",
                          kind);
                return NULL;
            }
            if (unary->operand->type == AST_STRUCT_LITERAL &&
                operand_type->kind != TYPE_STRUCT) {
                // A named-slice/array composite literal parses as
                // AST_STRUCT_LITERAL and resolves to its underlying kind —
                // it must not reach the struct-only codegen path.
                // type_to_string can hand back a NULL name for unnamed
                // types (e.g. an enum-variant literal) — %s on NULL is UB.
                const char* tn = type_to_string(operand_type);
                type_error(checker, expr->pos,
                          "cannot take the address of a %s composite literal (only struct literals are supported)",
                          tn ? tn : "non-struct");
                return NULL;
            }
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
    if (strcmp(name, "int") == 0)     return type_checker_get_builtin(checker, TYPE_INT64);
    if (strcmp(name, "int8") == 0)    return type_checker_get_builtin(checker, TYPE_INT8);
    if (strcmp(name, "int16") == 0)   return type_checker_get_builtin(checker, TYPE_INT16);
    if (strcmp(name, "int32") == 0)   return type_checker_get_builtin(checker, TYPE_INT32);
    if (strcmp(name, "rune") == 0)    return type_checker_get_builtin(checker, TYPE_INT32); // Go: rune = int32
    if (strcmp(name, "int64") == 0)   return type_checker_get_builtin(checker, TYPE_INT64);
    if (strcmp(name, "uint") == 0)    return type_checker_get_builtin(checker, TYPE_UINT64);
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
        "byte", "rune", "float32", "float64", "string", "bool",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i]) == 0) return 1;
    }
    return 0;
}

Type* type_check_call_expr(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_CALL_EXPR) return NULL;

    CallExprNode* call = (CallExprNode*)expr;

    // A type in call-argument position (map_type/slice_type — the grammar
    // alternative added for `make(...)`) only means something when the
    // callee is the `make` builtin. Any other callee reaching here with one
    // (e.g. `foo(map[string]int)`) is rejected here with a clean, specific
    // diagnostic. Without this guard the argument would fall through to the
    // generic type_check_expression() call in the argument-checking loop
    // below, whose switch has no case for AST_MAP_TYPE/AST_SLICE_TYPE and
    // would instead emit the far less useful "Unknown expression type".
    if (call->args && (call->args->type == AST_MAP_TYPE || call->args->type == AST_SLICE_TYPE)) {
        int callee_is_make = call->function && call->function->type == AST_IDENTIFIER &&
                              strcmp(((IdentifierNode*)call->function)->name, "make") == 0;
        if (!callee_is_make) {
            type_error(checker, expr->pos, "type used as value");
            return NULL;
        }
    }

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
                // Task 3b: reject an out-of-range CONSTANT conversion
                // (`int8(300)`, `byte(300)`, `float32(1e40)`) — Go-conformant
                // — while a runtime-value conversion (`int8(x)`) stays legal
                // truncation. This was the task-3 report's recorded deferral;
                // wiring it required first migrating examples/conv_probe.goo's
                // two deliberately-truncating constant-conversion lines
                // (`int8(200)`, `byte(300)`) to runtime-variable form, which
                // preserves their SExt/ZExt/Trunc codegen coverage (same
                // instructions, same output) without tripping this check.
                // See check_conversion_operand_range's doc comment for the
                // exact checked shapes and the `^`/binop exclusions.
                if (!check_conversion_operand_range(checker, call->args,
                                                    conv_target, false)) {
                    return NULL; // error already emitted
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
        // make(map[K]V[, hint]) -> map value; make([]T, n[, cap]) -> slice
        // value. arg1 is a type — either the map_type/slice_type grammar
        // alternative above, or an identifier naming a type — resolved via
        // type_from_ast rather than typechecked as a value expression, same
        // as `new`.
        if (strcmp(func_ident->name, "make") == 0) {
            if (!call->args) {
                type_error(checker, expr->pos, "make() requires a map or slice type");
                return NULL;
            }
            Type* made = type_from_ast(checker, call->args);
            if (!made) return NULL; // type_from_ast reports the error
            if (made->kind == TYPE_MAP) {
                // type_from_ast() already rejects a non-string map key type
                // (the runtime map is string-keyed) with its own clean
                // error before returning here, so `made` never carries a
                // non-string key at this point.
                size_t hint_count = 0;
                for (ASTNode* a = call->args->next; a; a = a->next) hint_count++;
                if (hint_count > 1) {
                    type_error(checker, expr->pos,
                               "make(map[K]V, ...) takes at most one size hint argument");
                    return NULL;
                }
                if (call->args->next) {
                    // Size hint: type-checked and required to be an integer,
                    // but otherwise ignored — the list-backed map runtime
                    // has no notion of pre-sizing.
                    Type* hint_t = type_check_expression(checker, call->args->next);
                    if (!hint_t) return NULL;
                    if (!type_is_integer(hint_t)) {
                        type_error(checker, call->args->next->pos,
                                   "make: size hint must be an integer, got %s",
                                   type_to_string(hint_t));
                        return NULL;
                    }
                }
                expr->node_type = made;
                return made;
            }
            if (made->kind == TYPE_SLICE) {
                // make([]T, n[, cap]): length required, capacity optional,
                // both integers. No compile-time len<=cap relation is
                // enforced (Go checks it at runtime; a runtime check here
                // is a noted follow-up — no panic-with-format infra yet).
                ASTNode* len_arg = call->args->next;
                if (!len_arg) {
                    type_error(checker, expr->pos,
                               "make([]T) requires a length argument");
                    return NULL;
                }
                ASTNode* cap_arg = len_arg->next;
                if (cap_arg && cap_arg->next) {
                    type_error(checker, expr->pos,
                               "make([]T, len, cap) takes at most three arguments");
                    return NULL;
                }
                Type* len_t = type_check_expression(checker, len_arg);
                if (!len_t) return NULL;
                if (!type_is_integer(len_t)) {
                    type_error(checker, len_arg->pos,
                               "make: length must be an integer, got %s",
                               type_to_string(len_t));
                    return NULL;
                }
                if (cap_arg) {
                    Type* cap_t = type_check_expression(checker, cap_arg);
                    if (!cap_t) return NULL;
                    if (!type_is_integer(cap_t)) {
                        type_error(checker, cap_arg->pos,
                                   "make: capacity must be an integer, got %s",
                                   type_to_string(cap_t));
                        return NULL;
                    }
                }
                expr->node_type = made;
                return made;
            }
            type_error(checker, expr->pos, "make() requires a map or slice type");
            return NULL;
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
        // len(slice|string|map) -> int. Codegen dispatches on the arg's
        // TypeKind (map routes through goo_map_len_sv; slice/string extract
        // the header's length field) — gate the accepted kinds here so an
        // unsupported argument (e.g. an array) is a clean error instead of
        // reaching codegen's ExtractValue path, which assumes an aggregate
        // and segfaults on anything else.
        if (strcmp(func_ident->name, "len") == 0) {
            if (!call->args || call->args->next) {
                type_error(checker, expr->pos, "len expects exactly one argument");
                return NULL;
            }
            Type* arg_t = type_check_expression(checker, call->args);
            if (!arg_t) return NULL;
            if (arg_t->kind != TYPE_SLICE && arg_t->kind != TYPE_STRING && arg_t->kind != TYPE_MAP) {
                type_error(checker, expr->pos,
                           "len() requires a slice, string, or map argument");
                return NULL;
            }
            expr->node_type = checker->builtin_types[TYPE_INT64]; // Go: len -> int (64-bit)
            return checker->builtin_types[TYPE_INT64];
        }
        // cap(slice) -> int. The slice's capacity (header field 2). TYPE_MAP
        // falls into the `!= TYPE_SLICE` branch below, which already rejects
        // it cleanly — maps lower to an opaque i8* with no capacity field,
        // so codegen's ExtractValue would segfault if this let one through.
        if (strcmp(func_ident->name, "cap") == 0) {
            if (!call->args || call->args->next) {
                type_error(checker, expr->pos, "cap expects exactly one argument");
                return NULL;
            }
            Type* slice_t = type_check_expression(checker, call->args);
            if (!slice_t) return NULL;
            if (slice_t->kind == TYPE_MAP) {
                type_error(checker, expr->pos, "cap() is not defined for maps");
                return NULL;
            }
            if (slice_t->kind != TYPE_SLICE) {
                type_error(checker, expr->pos,
                           "cap: argument must be a slice, got %s", type_to_string(slice_t));
                return NULL;
            }
            expr->node_type = checker->builtin_types[TYPE_INT64]; // Go: cap -> int (64-bit)
            return checker->builtin_types[TYPE_INT64];
        }
        // delete(m, k) -> void. Removes key k from map m (no-op if absent).
        // Exactly two args: the first must be a map, the second assignable
        // to its key type (string only, today — the runtime map is
        // string-keyed). Codegen lowers to goo_map_delete_sv, passing the
        // key's data pointer like the map-write path does.
        if (strcmp(func_ident->name, "delete") == 0) {
            if (!call->args || !call->args->next || call->args->next->next) {
                type_error(checker, expr->pos, "delete expects exactly two arguments (map, key)");
                return NULL;
            }
            Type* map_t = type_check_expression(checker, call->args);
            if (!map_t) return NULL;
            if (map_t->kind != TYPE_MAP) {
                type_error(checker, expr->pos,
                           "delete() requires a map as its first argument");
                return NULL;
            }
            Type* key_t = type_check_expression(checker, call->args->next);
            if (!key_t) return NULL;
            if (!type_compatible(key_t, map_t->data.map.key_type)) {
                type_error(checker, expr->pos,
                           "delete: cannot use %s as key of %s",
                           type_to_string(key_t), type_to_string(map_t));
                return NULL;
            }
            expr->node_type = checker->builtin_types[TYPE_VOID];
            return checker->builtin_types[TYPE_VOID];
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

    // error.Error() -> string (Phase 6 Task 3). `e.Error` resolves (via the
    // selector special case) to TYPE_STRING, so the generic TYPE_FUNCTION
    // check below would reject the call. Recognize it here using the receiver
    // type already computed during the call->function check above (no
    // re-evaluation — avoids double type-checking/double-diagnostics).
    if (call->function->type == AST_SELECTOR_EXPR && func_type->kind == TYPE_STRING) {
        SelectorExprNode* esel = (SelectorExprNode*)call->function;
        Type* erecv_t = esel->expr->node_type;
        if (type_is_error(erecv_t) &&
            strcmp(esel->selector, "Error") == 0) {
            if (call->args) {
                type_error(checker, expr->pos, "error.Error() takes no arguments");
                return NULL;
            }
            expr->node_type = func_type; // string
            return func_type;
        }
    }

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
    // Task 2: a user-defined variadic function (param_types non-NULL — its
    // last entry is the TYPE_SLICE the checker built in
    // declare_function_signature) DOES get its signature checked below, just
    // with variadic-aware arity/element-type rules. Only the NULL-param
    // builtins (println/print/panic, which are separately excluded via
    // is_builtin below) rely on skipping this entirely — gating on
    // param_types instead of a blanket is_variadic exclusion preserves that
    // special case while covering the new feature.
    int callee_is_variadic = func_type->data.function.is_variadic;
    int skip_variadic_builtin = callee_is_variadic && !func_type->data.function.param_types;
    int check_signature = 0;
    size_t recv_offset = 0;
    const char* callee_name = NULL;
    if (call->function && call->function->type == AST_IDENTIFIER
        && !skip_variadic_builtin) {
        IdentifierNode* callee_ident = (IdentifierNode*)call->function;
        Variable* callee = type_checker_lookup_variable(checker, callee_ident->name);
        if (callee && !callee->is_builtin) {
            check_signature = 1;
            callee_name = callee_ident->name;
        }
    } else if (call->function && call->function->type == AST_SELECTOR_EXPR
               && !skip_variadic_builtin) {
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
            } else if (st->kind == TYPE_PACKAGE &&
                       sel->expr->type == AST_IDENTIFIER) {
                // Source-compiled package function call (stdlib Phase 1):
                // `pkg.Fn(args)`. Unlike the hardcoded stdlib shims — whose
                // markers carry no Package* (or an empty exports scope) and
                // whose func_type is a param-less stub that must NOT be
                // arity-checked (else `fmt.Println("x")` would false-reject) —
                // a source-package export carries a real signature. Without
                // this branch a `pkg.Fn` selector matches neither the struct-
                // method nor interface-method case above, so its args slip past
                // type checking and a width mismatch (int32 arg into an int64
                // param) reaches the LLVM verifier as invalid IR. Enable
                // checking only when the selector resolves in the package's
                // exports scope to THIS func_type (identity) — true for source
                // exports, false for shim lookups (which are not in exports).
                // recv_offset=0: package functions carry no spliced receiver.
                IdentifierNode* pkg_ident = (IdentifierNode*)sel->expr;
                Variable* pkg_marker =
                    type_checker_lookup_variable(checker, pkg_ident->name);
                if (pkg_marker && pkg_marker->package) {
                    Variable* exp = scope_lookup_variable(
                        pkg_marker->package->exports, sel->selector);
                    if (exp && exp->type == func_type) {
                        check_signature = 1;
                        recv_offset = 0;
                        callee_name = sel->selector;
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
        //
        // Task 2 (variadic): once the argument index reaches the variadic
        // param's position (param_count - 1 — the LAST entry, guaranteed by
        // declare_function_signature's "must be final" check), every
        // remaining trailing argument checks/adapts against that param's
        // ELEMENT type instead of indexing further into param_types (which
        // has no per-arg entries beyond the slice itself). This is what lets
        // `small(300)` into `func small(bs ...int8)` hit the same
        // is_untyped_int_rooted/adapt_untyped_int_operand path below that
        // "constant 300 overflows int8" already comes from for non-variadic
        // narrow params.
        Type* param_type = NULL;
        if (check_signature && param_types && callee_is_variadic && param_count > 0) {
            size_t last_idx = param_count - 1;
            if ((arg_count + recv_offset) < last_idx) {
                param_type = param_types[arg_count + recv_offset];
            } else {
                Type* slice_t = param_types[last_idx];
                param_type = (slice_t && slice_t->kind == TYPE_SLICE)
                    ? slice_t->data.slice.element_type : NULL;
            }
        } else if (check_signature && (arg_count + recv_offset) < param_count && param_types) {
            param_type = param_types[arg_count + recv_offset];
        }
        if (param_type) {
            // Narrow integer-literal adaptation: an untyped integer literal
            // adapts to an integer parameter's type (Go's untyped-constant rule,
            // restricted to literals). Retype the literal node so codegen emits
            // it at the parameter's width and signedness (see the node_type-
            // honoring path in codegen_generate_literal); the width guard below
            // then sees matching types. This is what lets `RotateLeft64(1, 4)`
            // pass `1` to a uint64 parameter.
            if (arg && param_type && type_is_integer(param_type) &&
                is_untyped_int_rooted(arg, 0)) { // for_float_context=0: integer param, shifts and /,% stay valid
                if (!adapt_untyped_int_operand(checker, arg, param_type, 0, 1)) return NULL;
                arg_type = param_type;
            }

            // type_compatible() no longer permits ANY numeric->numeric pair —
            // it now rejects float->int specifically (T3's asymmetric fix for
            // the silent bit-store), while still permitting int<->int width
            // mismatches and int->float. call_codegen's user-call arg loop
            // (T4) now coerces a numeric argument to the callee's declared
            // parameter width/signedness before the call, so a mismatch that
            // slips past this checker (check_signature false above, e.g. a
            // call through a function-valued struct field) no longer crashes
            // the LLVM verifier. That codegen coercion doesn't relax THIS
            // diagnostic, though: for a call we CAN verify by signature
            // identity (check_signature true, this block), a clean type error
            // beats silently reinterpreting bits at a different width. Reject
            // those here, mirroring P2-1's return guard. (An integer LITERAL
            // is exempt — it was just adapted to the parameter type above,
            // and codegen emits it at that width.)
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
    if (check_signature) {
        size_t declared = param_count - recv_offset;
        if (callee_is_variadic) {
            // The variadic param itself (declared's last slot) has no fixed
            // arity — zero or more trailing args pack into it (`sum()` is
            // valid: an empty slice). Only the FIXED prefix before it is
            // required; declared >= 1 always holds here because a variadic
            // signature always carries at least the slice param itself.
            size_t min_args = declared - 1;
            if (arg_count < min_args) {
                type_error(checker, expr->pos,
                           "call to %s: not enough arguments (have %zu, want at least %zu)",
                           callee_name, arg_count, min_args);
                return NULL;
            }
        } else if (arg_count != declared) {
            type_error(checker, expr->pos,
                       "call to %s: wrong number of arguments (have %zu, want %zu)",
                       callee_name, arg_count, declared);
            return NULL;
        }
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
        case TYPE_STRING:
            // Go: s[i] yields the i-th byte (type byte == uint8). The result
            // is a value, not an addressable lvalue — strings are immutable, so
            // `s[i] = x` is rejected separately in the assignment checker. The
            // matching codegen lives in codegen_generate_index_expr (TYPE_STRING).
            element_type = type_checker_get_builtin(checker, TYPE_UINT8);
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
    if (!base_type) return NULL;

    // Bounds are optional (open-ended slices `s[low:]`, `s[:high]`, `s[:]`);
    // codegen defaults an omitted low to 0 and an omitted high to len.
    if (slice->low) {
        Type* low_type = type_check_expression(checker, slice->low);
        if (!low_type) return NULL;
        if (!type_is_integer(low_type)) {
            type_error(checker, slice->low->pos,
                       "Slice low bound must be integer, got %s", type_to_string(low_type));
            return NULL;
        }
    }
    if (slice->high) {
        Type* high_type = type_check_expression(checker, slice->high);
        if (!high_type) return NULL;
        if (!type_is_integer(high_type)) {
            type_error(checker, slice->high->pos,
                       "Slice high bound must be integer, got %s", type_to_string(high_type));
            return NULL;
        }
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

    // fmt.Printf(format string, args...) -> void  (compile-time format walker)
    if (strcmp(package, "fmt") == 0 && strcmp(name, "Printf") == 0) {
        return type_function(NULL, 0, void_t);
    }

    // fmt.Sprintf(format string, args...) -> string  (compile-time format builder)
    if (strcmp(package, "fmt") == 0 && strcmp(name, "Sprintf") == 0) {
        Type* string_t = type_checker_get_builtin(checker, TYPE_STRING);
        return type_function(NULL, 0, string_t);
    }

    // fmt.Errorf(format string, args...) -> error  (Sprintf + box)
    if (strcmp(package, "fmt") == 0 && strcmp(name, "Errorf") == 0) {
        return type_function(NULL, 0, type_checker_error_type(checker));
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

    // strconv.Itoa(int) -> string
    if (strcmp(package, "strconv") == 0 && strcmp(name, "Itoa") == 0) {
        Type* string_t = type_checker_get_builtin(checker, TYPE_STRING);
        return type_function(NULL, 0, string_t);
    }

    // strconv.Atoi(string) -> !int  (error union: success=int, error=string).
    // The value arm is the language's `int`, which is now int64 (Go: int is
    // 64-bit here). The bridge binds `n` from this value type, and `n` then
    // round-trips through an `int`-typed slot (e.g. `func parse(s) (int, error)`);
    // both are int64, so the tuple slot matches.
    if (strcmp(package, "strconv") == 0 && strcmp(name, "Atoi") == 0) {
        Type* int_t = type_checker_get_builtin(checker, TYPE_INT64);
        Type* err_t = type_checker_get_builtin(checker, TYPE_STRING);
        return type_function(NULL, 0, type_error_union(int_t, err_t));
    }

    // errors.New(string) -> error  (?*int8 — the nullable error type)
    // For v1, the returned error is a non-nil marker; message storage is
    // deferred to Phase 6 (.Error() method / runtime error struct).
    if (strcmp(package, "errors") == 0 && strcmp(name, "New") == 0) {
        Type* err_t = type_checker_error_type(checker);
        return type_function(NULL, 0, err_t);
    }

    // errors.Unwrap(error) -> error  (returns the wrapped cause, or nil)
    if (strcmp(package, "errors") == 0 && strcmp(name, "Unwrap") == 0) {
        return type_function(NULL, 0, type_checker_error_type(checker));
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

        // stdlib Phase 0 (Task 5): for a source-compiled package the marker
        // Variable carries a real Package* whose `exports` scope holds fresh
        // copies of its A-Z top-level symbols with their real signatures. Resolve
        // the selector against those FIRST, so `mypkg.Double(21)` type-checks
        // against `func Double(int) int` and gets real argument checking. The
        // hardcoded stdlib shim (below) stays the per-symbol FALLBACK for shim
        // packages, whose markers carry an empty exports scope.
        Variable* pkg_marker = type_checker_lookup_variable(checker, pkg_ident->name);
        if (pkg_marker && pkg_marker->package) {
            Variable* exp = scope_lookup_variable(pkg_marker->package->exports,
                                                  selector->selector);
            if (exp && exp->type) {
                expr->node_type = exp->type;
                return exp->type;
            }
        }

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

    // error.Error() -> string (Phase 6 Task 3). The error type is the tagged
    // nullable handle (name=="error"); it carries no method set, so it must
    // be special-cased here BEFORE the named-type method lookup below (which
    // would look for a nonexistent "error__Error" function and fall through
    // to the generic rejection).
    if (type_is_error(expr_type) &&
        strcmp(selector->selector, "Error") == 0) {
        Type* ret = type_checker_get_builtin(checker, TYPE_STRING);
        expr->node_type = ret;
        return ret;
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

