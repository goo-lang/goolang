#include "types.h"
#include "embedding.h"
#include "shim_signatures.h"  // P4.1: declarative stdlib-shim call signatures
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
// Comptime value params (fix round 4): goo_expr_references_comptime_param
// now lives in expression_helpers.c, declared in types.h.
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

// Closures Branch B, Task 1: func literal `func(params) result { body }` in
// expression position. Builds the signature from the literal's OWN
// params/return_type — the exact same param-walk as type_from_ast's
// AST_FUNC_TYPE arm (type_checker.c), including the #105 variadic-last-
// param model (wrap in TYPE_SLICE, set is_variadic on the resulting
// function Type). Not shared code with that arm (it lives in a different
// file outside this task's allowlist) but deliberately kept in lockstep —
// see that arm's comment for the shape this mirrors.
//
// Closures Task 2 (capture): the literal's body scope now chains directly
// onto the ENCLOSING scope (no more T1's "root at package/global" stand-in —
// see the git history of this comment for that earlier behavior), with
// is_function_boundary=1 marking the scope pushed for the literal's own
// body. type_check_identifier's scope walk uses that marker to detect a
// capture: a resolution that has to leave this (or a nested literal's) own
// boundary scope to find its binding. Codegen's promotion pass
// (function_codegen.c) heap-allocates every captured variable's storage so
// the closure's env can hold a pointer to it that outlives the declaring
// frame — see "Capture" in docs/superpowers/specs/2026-07-03-closures-
// design.md. Package-level functions/vars/consts/imports still resolve
// exactly as before (never captures): they live in the root/package scope,
// which is never is_function_boundary, so type_check_identifier's "found in
// a FUNCTION scope" check excludes them.
static Type* type_check_func_lit(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_FUNC_LIT) return NULL;
    FuncLitNode* lit = (FuncLitNode*)expr;

    size_t param_count = 0;
    for (ASTNode* p = lit->params; p; p = p->next) {
        if (p->type != AST_VAR_DECL) continue;
        // Comptime-value params gap-fix: a comptime parameter demands a
        // concrete callee Variable with a func_decl_node (Task 2's
        // type_check_call_expr walks that back-reference to find
        // is_comptime_param) — a func literal is called through its
        // expression's Type alone, never resolving to such a Variable, so
        // the check silently never fires and `comptime n` behaves as a
        // plain runtime int. Reject it here, at the literal's own
        // signature-build time.
        if (((VarDeclNode*)p)->is_comptime_param) {
            type_error(checker, p->pos,
                "comptime parameters are only supported on named functions");
            return NULL;
        }
        param_count++;
    }

    Type** param_types = NULL;
    int is_variadic = 0;
    if (param_count > 0) {
        param_types = calloc(param_count, sizeof(Type*));
        if (!param_types) return NULL;
        size_t idx = 0;
        for (ASTNode* p = lit->params; p; p = p->next) {
            if (p->type != AST_VAR_DECL) continue;
            VarDeclNode* pd = (VarDeclNode*)p;
            Type* pt = pd->type ? type_from_ast(checker, pd->type)
                                : type_checker_get_builtin(checker, TYPE_INT32);
            if (!pt) { free(param_types); return NULL; }
            if (pd->is_variadic_param) {
                pt = type_slice(pt);
                is_variadic = 1;
            }
            param_types[idx++] = pt;
        }
    }

    Type* return_type = lit->return_type
        ? type_from_ast(checker, lit->return_type)
        : type_checker_get_builtin(checker, TYPE_VOID);
    if (lit->return_type && !return_type) { free(param_types); return NULL; }

    Type* func_type = type_function(param_types, param_count, return_type);
    if (func_type) func_type->data.function.is_variadic = is_variadic;
    free(param_types);  // type_function copies what it needs
    if (!func_type) return NULL;

    // Chain the literal's body scope onto the currently-active (enclosing)
    // scope — Closures Task 2 (see the doc comment above); T1 rooted this at
    // package/global instead.
    Scope* enclosing_scope = checker->current_scope;

    scope_push(checker);
    checker->current_scope->is_function_boundary = 1;

    // Codegen-hardening R1-TC: snapshot the WHOLE per-function scratch
    // struct (active_type_params, literal_stack, label registry, goto-label
    // registry, arena-nesting chain) in one assignment before this
    // literal's own body-check mutates any of it — mirrors cfctx_save's
    // role for codegen (function_codegen.c's codegen_generate_func_lit).
    // Captured BEFORE the literal-stack push just below, so tc_fctx_restore
    // at the end both restores the label/goto/arena namespaces AND pops
    // this literal's own stack entry in the same single assignment — no
    // separate literal_stack_len-restore line needed.
    TcFunctionContext saved_tcfctx;
    tc_fctx_save(&saved_tcfctx, &checker->tc_fctx);

    // Push this literal onto the checker's literal stack so
    // type_check_identifier can relay a transitive capture through every
    // currently-open literal (types.h's TcFunctionContext.literal_stack doc
    // comment). Degrades gracefully at GOO_CLOSURE_MAX_NESTING (push
    // silently skipped) instead of desyncing the stack — tc_fctx_restore
    // above still balances it either way.
    if (checker->tc_fctx.literal_stack_len < GOO_CLOSURE_MAX_NESTING) {
        checker->tc_fctx.literal_stack[checker->tc_fctx.literal_stack_len] = expr;
        checker->tc_fctx.literal_stack_len++;
    }

    // Track the return type so a `return` inside the literal's body checks
    // against the LITERAL's declared return, not whatever enclosing
    // function's return type was active (mirrors type_check_function_decl's
    // save/restore of this exact field, type_checker.c).
    Type* saved_return_type = checker->current_return_type;
    checker->current_return_type = return_type;

    // Codegen-hardening R1-TC: a func literal gets its own label AND
    // goto-label namespace, same rationale as type_check_function_decl's
    // own tc_fctx_reset (type_checker.c) — a label/goto inside the closure
    // must not collide with (or be visible to) the enclosing function's.
    // Unlike that boundary, arena_chain_depth ALSO needs an explicit reset
    // here (a closure's own arena-nesting path must be measured from ITS
    // OWN body, not offset by however many `arena{}` blocks happen to
    // lexically enclose the literal in the outer function) — inlined the
    // same way ControlFlowContext.loop_depth gets an extra explicit reset
    // in codegen_generate_func_lit beyond cfctx_reset's own minimal scope.
    tc_fctx_reset(&checker->tc_fctx);
    checker->tc_fctx.arena_chain_depth = 0;
    if (lit->body) {
        type_check_collect_goto_labels(checker, lit->body);
    }

    // gofmt-syntax-b Task 3: same independent-namespace save/restore as
    // the label/goto-label registries just above — `fallthrough` cannot
    // cross a func-literal boundary even when the literal is lexically
    // written inside a switch case's body (Go: the literal is its own
    // function). Kept outside TcFunctionContext (see that struct's own doc
    // comment, types.h).
    FallthroughContext saved_fallthrough_ctx = checker->fallthrough_ctx;
    checker->fallthrough_ctx = FALLTHROUGH_CTX_NONE;

    if (lit->params) {
        for (ASTNode* p = lit->params; p; p = p->next) {
            if (p->type != AST_VAR_DECL) continue;
            VarDeclNode* pd = (VarDeclNode*)p;
            Type* pt = pd->type ? type_from_ast(checker, pd->type)
                                : type_checker_get_builtin(checker, TYPE_INT32);
            if (pd->is_variadic_param && pt) pt = type_slice(pt);
            if (pt) {
                for (size_t i = 0; i < pd->name_count; i++) {
                    Variable* pv = variable_new(pd->names[i], pt, pd->base.pos);
                    if (pv) {
                        pv->is_initialized = 1;
                        // Closures Task 2: backref for capture stamping (see
                        // type_check_function_decl's identical convention).
                        pv->decl_node = (struct ASTNode*)pd;
                        scope_add_variable(checker->current_scope, pv);
                    }
                }
            }
        }
    }

    // Check the body statements — reuse type_check_statement exactly like
    // type_check_function_decl's own body traversal (type_checker.c) rather
    // than duplicating that logic.
    int ok = 1;
    if (lit->body) {
        ok = type_check_statement(checker, lit->body);
    }

    // P2.4: missing-return analysis — same rule and rationale as
    // type_check_function_decl's identical check (type_checker.c), applied
    // to a func literal's own declared return type/body instead of an
    // enclosing named function's.
    if (ok && lit->body && return_type && return_type->kind != TYPE_VOID) {
        if (!stmt_is_terminating(lit->body)) {
            type_error(checker, lit->body->pos, "missing return");
            ok = 0;
        }
    }

    checker->fallthrough_ctx = saved_fallthrough_ctx;
    tc_fctx_restore(&checker->tc_fctx, &saved_tcfctx);
    checker->current_return_type = saved_return_type;
    scope_pop(checker);
    checker->current_scope = enclosing_scope;

    if (!ok) return NULL;

    expr->node_type = func_type;
    return func_type;
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
                // An interface-typed map key (Task 2) accepts any concrete
                // implementer — route through check_interface_assign,
                // mirroring the value check below. It emits its own "does
                // not implement" diagnostic on failure.
                if (want_key->kind == TYPE_INTERFACE) {
                    if (!check_interface_assign(checker, kt, want_key, k->pos)) {
                        return NULL;
                    }
                } else {
                    // Arc 9 (i): keys take the SAME adaptation + range gate
                    // as values (adapt_field_init_value below) — an untyped
                    // `300` IS type_compatible with int8, so without this
                    // pass `map[int8]int{300: 1}` was accepted and the raw
                    // folded key (i64 300) reached the runtime: the map
                    // silently behaved as map[int64] (m[44] missed, m[300]
                    // hit), violating the declared key domain. Const-ident
                    // keys are judged by the shared core inside the adapter
                    // (arc 7). In-range keys are unaffected: codegen widens
                    // a narrow key to the runtime's i64 by its signedness,
                    // which equals the raw folded value for every
                    // representable key.
                    kt = adapt_field_init_value(checker, k, want_key, kt);
                    if (!kt) return NULL;
                    if (!type_compatible(kt, want_key)) {
                        type_error(checker, k->pos,
                                   "Map literal key %zu type '%s' is not compatible "
                                   "with declared key type '%s'",
                                   ki, type_to_string(kt), type_to_string(want_key));
                        return NULL;
                    }
                }
            }
            size_t vi = 0;
            for (ASTNode* v = lit->values; v; v = v->next, vi++) {
                // Elided composite VALUE `{...}` (Task 4, e.g. map[string]P{"p":
                // {X: 1}} or map[string][]int{"a": {1, 2}}): thread the map's
                // declared value type V into the literal so
                // type_check_struct_literal can resolve it — same pre-stamp
                // check_slice_elements does for slice/array elements. V may be
                // a struct type (struct field elision) or a slice type
                // (positional element elision); type_check_struct_literal
                // accepts both for an elided (type_name==NULL) literal.
                if (v->type == AST_STRUCT_LITERAL &&
                    ((StructLiteralNode*)v)->type_name == NULL) {
                    v->node_type = want_val;
                }
                Type* vt = type_check_expression(checker, v);
                if (!vt) return NULL;
                // Task 3b: same element adaptation + range check as the
                // slice/array sinks — `map[string]int8{"a": 300}` rejects
                // instead of silently truncating. Keys take the same pass
                // above (arc 9) — the old "a wrong-width key literal is a
                // compatibility mismatch" rationale was false for untyped
                // literals, which are compatible with every integer width.
                vt = adapt_field_init_value(checker, v, want_val, vt);
                if (!vt) return NULL;
                // An interface-typed map value accepts any concrete
                // implementer (boxed at codegen via codegen_interface_box —
                // see the AST_PAREN_EXPR map-literal codegen site). Plain
                // type_compatible rejects a concrete implementer outright
                // (it isn't the interface type itself), which used to fall
                // through to the generic mismatch error below with a NULL-ish
                // rendering of the interface type; route interface-typed
                // slots through check_interface_assign instead, mirroring the
                // slice-literal element check above. It emits its own "does
                // not implement" diagnostic on failure.
                if (want_val->kind == TYPE_INTERFACE) {
                    if (!check_interface_assign(checker, vt, want_val, v->pos)) {
                        return NULL;
                    }
                } else if (!type_compatible(vt, want_val)) {
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
            if (!at->length || !goo_fold_const_int_ctx(checker, at->length, &n)) {
                type_error(checker, expr->pos,
                           "array literal: length must be a constant expression");
                return NULL;
            }
            // Fix round 3 (minor 3): negative length — same clean rejection
            // as type_from_ast's AST_ARRAY_TYPE case (a wrapped size_t hung
            // the compiler downstream).
            if ((int64_t)n < 0) {
                type_error(checker, expr->pos,
                           "array length must be non-negative");
                return NULL;
            }
            // Fix round 3 (minor 1): when the length derives from a comptime
            // parameter (`[n]int{1, 2}` inside a comptime function's
            // TEMPLATE body), `n` here is Step 3's PLACEHOLDER — validating
            // the element count against it produced a placeholder-derived
            // rejection ("index 1 out of bounds for length 1") for a length
            // the user never wrote. Defer count-vs-length validation to
            // instance time: codegen's re-derivation
            // (codegen_generate_array_lit, composite_codegen.c) sizes the
            // instance's array from the REAL value, and its const fast path
            // zero-fills/places elements against that real length. Element
            // TYPE checking below is unaffected — only the index bound is
            // skipped for comptime-length literals.
            int comptime_len = goo_expr_references_comptime_param(checker, at->length);
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
                if (cur < 0 || (!comptime_len && (uint64_t)cur >= n)) {
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
            // Fix round 4: mark a comptime-length literal's type so
            // const-INDEX validation (type_check_index_expr) defers to
            // instance time too, exactly like the count-vs-length deferral
            // above — the flag rides the Type to every consumer. Round 6
            // (M-r5c): the shared marker also rewrites the display name so
            // diagnostics never show the placeholder length.
            if (arr && comptime_len) type_array_mark_comptime(arr, at->length);
            expr->node_type = arr;
            return arr;
        }
        case AST_MATCH_EXPR:
            return type_check_match_expr(checker, expr);
        case AST_FUNC_LIT:
            return type_check_func_lit(checker, expr);
        // Task 2 (stdlib unblocker): `[]byte(s)` conversion. v1 scope is
        // exactly []byte(string) — any other []T(expr) shape (e.g.
        // []int("x")) is rejected here with a v1-scoped diagnostic rather
        // than reaching codegen with no lowering.
        case AST_SLICE_CONVERSION: {
            SliceConvNode* conv = (SliceConvNode*)expr;
            // conv->slice_type is an AST_SLICE_TYPE node (a TYPE, not a
            // value expression) — resolved via type_from_ast, the
            // established convention for type-node resolution elsewhere in
            // this file (e.g. make(T)/new(T), slice-literal elem_type).
            // type_check_expression's switch has no AST_SLICE_TYPE case and
            // would always fail here.
            Type* target = type_from_ast(checker, conv->slice_type);
            Type* src = type_check_expression(checker, conv->operand);
            if (!target || !src) return NULL;
            if (target->kind != TYPE_SLICE ||
                target->data.slice.element_type->kind != TYPE_UINT8) {
                type_error(checker, expr->pos,
                    "[]T(x) conversion is only supported for []byte(string) in v1");
                return NULL;
            }
            if (src->kind != TYPE_STRING) {
                type_error(checker, expr->pos,
                    "[]byte(x) requires a string operand, got %s", type_to_string(src));
                return NULL;
            }
            expr->node_type = target;
            return target;
        }
        // Task 2 of type assertions: `x.(T)`. The comma-ok vs single-return
        // form is NOT decided here (mirrors the comma-ok map read) — this
        // always yields T, the single-value result type; the assignment
        // site (type_check_var_decl) synthesizes {T, bool} for a 2-name
        // short decl, exactly like the map-read comma-ok path.
        case AST_TYPE_ASSERT: {
            TypeAssertNode* ta = (TypeAssertNode*)expr;
            Type* operand_type = type_check_expression(checker, ta->expr);
            if (!operand_type) return NULL;
            if (operand_type->kind != TYPE_INTERFACE) {
                type_error(checker, expr->pos,
                    "invalid type assertion: operand is not an interface type");
                return NULL;
            }
            // ta->asserted_type is a TYPE node (like conv->slice_type above),
            // resolved via type_from_ast — not a value expression, so
            // type_check_expression has no case for it.
            Type* target_type = type_from_ast(checker, ta->asserted_type);
            if (!target_type) {
                type_error(checker, expr->pos,
                    "invalid type assertion: cannot resolve target type");
                return NULL;
            }
            if (target_type->kind == TYPE_INTERFACE) {
                // Interface target: a runtime-checked assertion (Go semantics).
                // Always well-formed on an interface operand; success is
                // decided at runtime by enumerating implementers in codegen
                // (codegen_interface_target_match). Do NOT require static
                // satisfaction — unlike a concrete target, there is no single
                // "the operand's static type must already satisfy I" check
                // that would make sense here (the OPERAND is itself some
                // interface type, e.g. `interface{}`, whose static method set
                // is unrelated to whether its dynamic value implements I).
                expr->node_type = target_type;
                return target_type;
            }
            const char* method = NULL;
            const char* reason = NULL;
            if (!type_interface_satisfied(checker, operand_type, target_type, &method, &reason)) {
                // Fix 2: the "comptime" sentinel gets the dedicated one-place
                // diagnostic — see report_comptime_method_not_satisfied's doc
                // comment (every type_interface_satisfied caller does this).
                if (reason && strcmp(reason, "comptime") == 0) {
                    report_comptime_method_not_satisfied(checker, expr->pos, method);
                    return NULL;
                }
                const char* iname = operand_type->data.interface.name
                                         ? operand_type->data.interface.name : "interface";
                const char* cname = type_receiver_name(target_type);
                type_error(checker, expr->pos,
                    "impossible type assertion: %s does not implement %s (%s method %s)",
                    cname ? cname : type_to_string(target_type), iname,
                    reason ? reason : "missing", method ? method : "?");
                return NULL;
            }
            expr->node_type = target_type;
            return target_type;
        }
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
    // Fix 2 (comptime-param functions are not first-class values): this is
    // the one sink every composite-literal value routes through (struct
    // field, slice element, array element, map value — see the Task 3b note
    // above), so the "cannot store a comptime-param function in a composite"
    // rule lives here instead of four copies at the call sites. Checked
    // BEFORE the numeric early-return below — a function type is never
    // numeric, so it would otherwise sail through untouched. NULL signals
    // rejection (error already emitted), exactly like the range-check
    // rejections below; every caller already propagates NULL.
    if (!reject_comptime_function_value(checker, v, vt, v->pos,
                                        "stored in a composite literal")) {
        return NULL;
    }
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
        // Arc 7 (n): a CONST IDENTIFIER is not a literal leaf, so ident-
        // bearing constant slot values (`S{x: a}`, `[]int8{a}`, `[2]int8{a,
        // 1}`, map values — every composite slot routes through here, Task
        // 3b) skipped the rooted adapter above and codegen's width coercion
        // truncated the store. Judge the FOLDED value via the shared core
        // (check_const_int_expr_fits — same admission and disjointness
        // argument as the arc-5 var-decl leg: is_untyped_int_rooted admits
        // no identifier leaf, so literal shapes keep the per-literal
        // adapter semantics bit-for-bit). Fit (1) falls through returning
        // `vt` unchanged — the caller's type_compatible laxness accepts and
        // codegen's coercion is exact for a representable value. Not-
        // applicable (0) — plain variables, comptime-param-tainted — stays
        // on the v1 laxness path unchanged.
        if (check_const_int_expr_fits(checker, v, field_type) < 0) return NULL;
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
    // type is inferred from context (the enclosing array/slice element type, or
    // — Task 4 — a map's declared value type V), which the caller pre-stamps on
    // expr->node_type before dispatching here. Resolve the target type from that
    // stamp instead of a by-name lookup. Struct element types are the common
    // table case; TYPE_SLICE is also accepted (e.g. `map[string][]int{"a": {1,
    // 2}}`) so it falls through to the "Named slice composite literal" block
    // below, the SAME machinery `type IntSlice []int; IntSlice{1, 2, 3}`
    // already uses — no new lowering path needed, just widening this gate.
    Type* named_type;
    if (lit->type_name == NULL) {
        named_type = expr->node_type;
        if (!named_type) {
            type_error(checker, expr->pos,
                       "elided composite literal '{...}' has no inferable type "
                       "in this context");
            return NULL;
        }
        if (named_type->kind != TYPE_STRUCT && named_type->kind != TYPE_SLICE) {
            type_error(checker, expr->pos,
                       "elided composite literal '{...}' requires a struct or "
                       "slice element type, got '%s'", type_to_string(named_type));
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
    // (or, Task 4, an ELIDED slice value — lit->type_name is NULL here, e.g.
    // `map[string][]int{"a": {1, 2}}` — reusing this same block; the error
    // messages below fall back to "(elided)" since there's no name to print).
    // Validates field_values as elements of the underlying element type and
    // stamps the named TYPE_SLICE as the expression's type. Codegen handles
    // lowering via the slice path (see codegen_generate_struct_lit).
    if (struct_type->kind == TYPE_SLICE) {
        // Keyed form (e.g. `IntSlice{x: 3}`) is invalid for slice types —
        // slices have no named fields.
        if (lit->is_keyed) {
            type_error(checker, expr->pos,
                       "cannot use keyed (field-name) elements with slice type '%s'",
                       lit->type_name ? lit->type_name : "(elided)");
            return NULL;
        }
        Type* want = struct_type->data.slice.element_type;
        if (!want) {
            type_error(checker, expr->pos,
                       "Named slice type '%s' missing element type",
                       lit->type_name ? lit->type_name : "(elided)");
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

// Closures Task 2: record a capture detected by type_check_identifier's
// scope walk. `depth` is the number of function/literal boundaries the walk
// crossed to reach `var`'s declaring scope — exactly the count of
// currently-open literals (innermost first, i.e. the TOP `depth` entries of
// checker->tc_fctx.literal_stack) that must ALSO carry this name through their own
// env, per the transitive-capture rule: an inner literal's env is populated
// correctly, but each INTERMEDIATE literal between the reference and the
// declaration must relay the slot pointer inward through its own env too
// (the classic nested-closure "missing middle env" bug).
//
// Marks var->is_captured and stamps the declaring VarDeclNode (via
// var->decl_node) so codegen's promotion pass (function_codegen.c) can find
// it, then appends `name` (deduped) to the captured_names of each of those
// `depth` literals.
//
// Returns 0 (after emitting a type_error) if var->decl_node is NULL or not
// an AST_VAR_DECL: this happens for a binding form that carries no
// VarDeclNode backref today — an if-let bind, a for-range loop variable, or
// a 2-value `a, b := x, y` short decl (a MultiAssignNode, not a VarDeclNode
// — see type_check_multi_assign). Silently allowing the capture would
// type-check successfully but leave the variable un-promotable at codegen
// time (nothing keys promotion off these forms), a silent miscompile once
// the closure outlives the frame. Rejecting cleanly here is the T2
// equivalent of T1's "Undefined variable" stand-in: a real, documented gap
// instead of a wrong answer.
static int type_checker_record_capture(TypeChecker* checker, Variable* var,
                                        const char* name, int depth, Position pos) {
    // Loop-variable capture: REJECT (checked before the decl_node guard —
    // it is the more specific diagnosis, and a range binding would otherwise
    // fall through to the generic "unsupported form" message below). Modern
    // Go (1.22+) gives each loop iteration its OWN variable, so `for i := 0;
    // i < 3; i = i + 1 { fs = append(fs, func() int { return i }) }` sums to
    // 3 there — but this compiler's promotion model has exactly ONE slot per
    // declaration and the for-init/range slot is minted once per loop ENTRY,
    // so it would silently compute the pre-Go-1.22 shared-slot answer (9).
    // Reject-rather-than-silently-deviate, per the #101 precedent;
    // per-iteration slots are the recorded follow-up. The suggested `i := i`
    // body-local copy works because an ordinary local's captured slot is
    // minted per EXECUTION of its declaration — once per iteration inside a
    // loop body (see codegen_alloc_local_promoted, function_codegen.c).
    if (var->is_loop_var) {
        type_error(checker, pos,
                   "cannot capture loop variable '%s' in a closure "
                   "(per-iteration capture semantics not yet supported; "
                   "copy it to a local first: %s := %s)",
                   name, name, name);
        return 0;
    }

    if (!var->decl_node || var->decl_node->type != AST_VAR_DECL) {
        type_error(checker, pos,
                   "cannot capture '%s' in a closure: declared via a form not "
                   "yet supported for capture (multi-value ':=', an if-let "
                   "binding, or a for-range loop variable) — bind it with a "
                   "plain 'name := value' first", name);
        return 0;
    }

    var->is_captured = 1;
    ((VarDeclNode*)var->decl_node)->is_captured = 1;

    if (depth > (int)checker->tc_fctx.literal_stack_len) depth = (int)checker->tc_fctx.literal_stack_len;
    for (int i = (int)checker->tc_fctx.literal_stack_len - depth; i < (int)checker->tc_fctx.literal_stack_len; i++) {
        FuncLitNode* lit = (FuncLitNode*)checker->tc_fctx.literal_stack[i];
        int already = 0;
        for (size_t j = 0; j < lit->captured_count; j++) {
            if (strcmp(lit->captured_names[j], name) == 0) { already = 1; break; }
        }
        if (already) continue;
        char** grown = realloc(lit->captured_names, sizeof(char*) * (lit->captured_count + 1));
        if (!grown) continue;  // defensive: leave this literal's env short rather than crash
        grown[lit->captured_count] = strdup(name);
        lit->captured_names = grown;
        lit->captured_count++;
    }
    return 1;
}

Type* type_check_identifier(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_IDENTIFIER) return NULL;

    IdentifierNode* ident = (IdentifierNode*)expr;

    // Closures Task 2: resolve via our own scope walk (mirrors
    // scope_lookup_variable's semantics exactly for the non-capture case —
    // same first-match-per-scope, then parent, traversal) instead of
    // calling type_checker_lookup_variable: that raw helper is ALSO used by
    // package/global lookups and other checker passes (see its call sites)
    // which must stay side-effect-free. Only THIS identifier arm may record
    // a capture. See "Capture" in docs/superpowers/specs/2026-07-03-
    // closures-design.md and the file-level note above type_check_func_lit.
    Variable* var = NULL;
    Scope* found_scope = NULL;
    int boundaries_crossed = 0;
    for (Scope* s = checker->current_scope; s; s = s->parent) {
        Variable* v = s->variables;
        while (v && strcmp(v->name, ident->name) != 0) v = v->next;
        if (v) { var = v; found_scope = s; break; }
        if (s->is_function_boundary) boundaries_crossed++;
    }

    if (!var) {
        type_error(checker, expr->pos, "Undefined variable '%s'", ident->name);
        return NULL;
    }

    // A capture is a resolution that (a) had to leave at least one
    // function/literal body scope to find the binding, AND (b) landed on a
    // binding that itself belongs to SOME function activation (a local or
    // param — not a global or package-level symbol, which resolve exactly
    // as before). For (b): every scope OTHER than the true root or a
    // package's top-level scope has a function-boundary scope somewhere in
    // its own ancestor-or-self chain, because every OTHER scope_push
    // happens while already nested inside one (if/for/switch/etc bodies,
    // or a literal's own body, are only ever pushed from inside a function
    // or literal). So walking from found_scope up to the true root and
    // finding no boundary means found_scope IS that top-level declaration
    // scope — never a capture, regardless of (a).
    if (boundaries_crossed > 0) {
        int found_in_function = 0;
        for (Scope* s = found_scope; s; s = s->parent) {
            if (s->is_function_boundary) { found_in_function = 1; break; }
        }
        if (found_in_function) {
            if (!type_checker_record_capture(checker, var, ident->name,
                                             boundaries_crossed, expr->pos)) {
                return NULL;  // capture of an unsupported binding form; error already emitted
            }
        }
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
static int adapt_untyped_int_rec(TypeChecker* checker, ASTNode* n, Type* target,
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
            if (!adapt_untyped_int_rec(checker, u->operand, target, !negated, checkable))
                return 0;
            n->node_type = target;
        } else if (u->operator == TOKEN_PLUS || u->operator == TOKEN_BIT_XOR) {
            // `^`-rooted literals are excluded from range checking (never
            // re-enabled once excluded, hence `checkable && ...` rather than
            // an unconditional 0) — see this function's doc comment and the
            // task-3 deviation note. The stamping side effect is unchanged
            // for `^`, matching pre-task-3 behavior exactly.
            int child_checkable = checkable && (u->operator != TOKEN_BIT_XOR);
            if (!adapt_untyped_int_rec(checker, u->operand, target, negated, child_checkable))
                return 0;
            n->node_type = target;
        }
    }
    if (n->type == AST_BINARY_EXPR) {
        BinaryExprNode* b = (BinaryExprNode*)n;
        if (b->operator == TOKEN_LSHIFT || b->operator == TOKEN_RSHIFT) {
            if (!adapt_untyped_int_rec(checker, b->left, target, negated, checkable)) // shift type = left type
                return 0;
            n->node_type = target;
        } else if (b->operator == TOKEN_PLUS || b->operator == TOKEN_MINUS ||
                   b->operator == TOKEN_MULTIPLY || b->operator == TOKEN_DIVIDE ||
                   b->operator == TOKEN_MODULO) {
            // Each side keeps ITS OWN negated/checkable state — a binary
            // MINUS does not flip either operand's sign for range-check
            // purposes (only a unary MINUS wrapping a leaf does that). The
            // per-leaf checks alone let `100 + 100` into int8 through
            // uncaught (each 100 individually fits) — the WHOLE-expression
            // folded value is judged once, at the top-level entry in the
            // adapt_untyped_int_operand wrapper below (arc 13).
            if (!adapt_untyped_int_rec(checker, b->left, target, negated, checkable))
                return 0;
            if (!adapt_untyped_int_rec(checker, b->right, target, negated, checkable)) // binop result = operand type
                return 0;
            n->node_type = target;
        }
    }
    return 1;
}

// Arc 13 (s): entry wrapper — fold-then-check the WHOLE compound against
// the target ONCE, then run the per-leaf adaptation. Each leaf of
// `100 + 100` individually fits int8, so the leaf checks alone silently
// truncated the folded 200 at every adapter sink (var decls, assignments,
// call args, composite slots, arc-9 map keys). Top-level only — judging
// interior nodes would false-reject `(100 + 100) - 100` (= 100, fits) on
// its inner subtree. Judged only when UNAMBIGUOUS: a negated caller
// context (the fold here is of the unnegated subtree — the wrong value
// to judge) or a fold in the modular window [2^63, 2^64) (where `0 - 1`
// and `1 << 63` are indistinguishable post-fold) skips the check and
// keeps the per-leaf laxness — so every newly rejected value is genuinely
// unrepresentable and no Go-legal shape gains a false reject. The
// returns/chan-send/const-decl gates judge the window via the
// negated-shape heuristic instead (their documented `0 - 1` deviation);
// this sink family deliberately stays lax there. goo_fold_const_int is
// the context-free folder: rooted shapes have no identifier leaves, and
// it returns 0 on anything it can't fold (e.g. division by zero),
// falling back to the per-leaf pass unchanged.
static int adapt_untyped_int_operand(TypeChecker* checker, ASTNode* n, Type* target,
                                      int negated, int checkable) {
    // Peel leading unary `+` (identity — never changes the value) so
    // `+(100 + 100)` reaches the same whole-fold gate as the bare
    // compound (arc-13 review find). `-` stays behind the negated logic
    // and `^` is the documented task-3 exclusion — neither is peeled.
    ASTNode* top = n;
    while (top && top->type == AST_UNARY_EXPR &&
           ((UnaryExprNode*)top)->operator == TOKEN_PLUS) {
        top = ((UnaryExprNode*)top)->operand;
    }
    if (top && top->type == AST_BINARY_EXPR && checkable && !negated &&
        type_is_integer(target)) {
        uint64_t folded;
        if (goo_fold_const_int(top, &folded) &&
            folded <= (uint64_t)INT64_MAX &&
            !int_const_fits_expected(folded, target, 0, 0)) {
            type_error(checker, top->pos, "constant %lld overflows %s",
                       (long long)(int64_t)folded, type_to_string(target));
            return 0;
        }
    }
    return adapt_untyped_int_rec(checker, n, target, negated, checkable);
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
    if (type_is_integer(target)) {
        if (is_untyped_int_rooted(value, 0))
            return adapt_untyped_int_operand(checker, value, target, 0, 1);
        // Arc 5 (h) sibling: a CONST IDENTIFIER is not a literal leaf, so
        // ident-bearing constant initializers (`const a = 300; var v int8 =
        // a`, or `a + 1`) skipped the per-literal adapter above entirely and
        // codegen's width-coerce step truncated the store (printed 44).
        // Judge the FOLDED value via the shared core (check_const_int_expr_
        // fits — same admission as the arc-4 chan-send gate and the arc-5
        // const-decl gate). The two branches are DISJOINT: is_untyped_int_
        // rooted admits no identifier leaf, so every pure-literal shape
        // keeps the adapter's semantics bit-for-bit (arc 13 closed the old
        // `100 + 100`-into-int8 per-leaf deviation with the whole-fold
        // wrapper) — only shapes the
        // adapter never handled gain a check. Not-applicable (0) — plain
        // variables, calls, comptime-param-tainted — stays accepted
        // unchanged (the v1 any-int laxness for non-constants).
        if (check_const_int_expr_fits(checker, value, target) < 0)
            return 0;
    }
    return 1;
}

// Arc 14 (f) bridge for type_checker.c's type_check_switch_stmt: a float-
// literal case expression compared against an INTEGER switch tag (`switch n
// { case 2.5: }`, n an int) had no adapter — the checker fell through to
// type_check_comparison_op, which accepts a float-vs-int comparison, and
// codegen_generate_switch_stmt's fallback LLVMBuildICmp(IntEQ) then took a
// float operand, crashing the LLVM verifier instead of reporting a clean
// diagnostic. Go's rule for a float CONSTANT meeting an integer context is
// the conversion rule (`int(2.5)`'s rule): magnitude must fit AND the value
// must be integral — reuse check_conversion_operand_range (the exact check
// a conversion already gets) instead of duplicating it, then stamp the case
// subtree to the tag's width so codegen emits an integer constant in place
// of a float one.
//
// Tri-state, mirroring check_const_int_expr_fits's contract (this file's
// other cross-TU bridge with the same "does this gate even apply" question):
// 0 when `case_expr` is not float-rooted (gate doesn't apply — caller falls
// back to its ordinary type_check_comparison_op path so a genuine kind
// mismatch like a string case still rejects there); 1 when it is float-
// rooted and fits (stamped); -1 when it is float-rooted but rejected (a
// positioned diagnostic was already emitted by check_conversion_operand_
// range, e.g. "constant 2.5 truncated to integer").
//
// Declared non-static and NOT forward-declared at the top of this file —
// same convention as adapt_var_decl_initializer just above: one external
// caller, in type_checker.c, which forward-declares its own `extern`
// prototype rather than a header change.
int adapt_switch_case_float_into_int(TypeChecker* checker, ASTNode* case_expr,
                                      Type* tag_type) {
    if (!case_expr || !tag_type || !is_untyped_float_rooted(case_expr)) return 0;
    if (!check_conversion_operand_range(checker, case_expr, tag_type, 0))
        return -1; // overflow or truncation — diagnostic already emitted
    stamp_int_const_expr_type(case_expr, tag_type);
    return 1;
}

// Arc 14 (g) bridge for type_checker.c's type_check_return_stmt: an untyped
// FLOAT-literal return value narrowing into (or widening to) a differently-
// sized float return type had no adapter — float_const_coerce (type_
// checker.c) only covers an untyped INT constant meeting a float target, so
// `return 3.9` from a `func() float32` defaulted to float64, failed the
// width check, and landed in the numeric-mismatch reject block: a Go-legal
// program (constant conversion with rounding) falsely rejected. Mirrors
// adapt_var_decl_initializer's float leg above, but for a return
// statement's single value expression instead of a var-decl initializer —
// same adapt_untyped_float_operand call, which range-checks (so `return
// 1e40` from a float32 function still rejects, "overflows float32") and
// recursively stamps the whole float-rooted subtree (so a mixed `return 1 +
// 0.5` computes at the target width, not just a leaf literal).
//
// Tri-state, same contract as adapt_switch_case_float_into_int just above:
// 0 when `value` is not float-rooted (gate doesn't apply — caller keeps its
// ordinary width-mismatch path); 1 when it is and range-checks clean
// (stamped to `expected`); -1 when it is float-rooted but out of range
// (diagnostic already emitted).
int adapt_return_float_literal(TypeChecker* checker, ASTNode* value, Type* expected) {
    if (!value || !expected || !type_is_float(expected) || !is_untyped_float_rooted(value))
        return 0;
    return adapt_untyped_float_operand(checker, value, expected, 0) ? 1 : -1;
}

// Go rejects a CONSTANT negative shift count at compile time ("invalid
// negative shift count"); a runtime-value count panics instead (codegen's
// shneg guard). Returns 0 (and reports) iff the count folds to a constant
// that is negative when read as signed. Reading the folded uint64 as int64
// also catches counts >= 2^63 — Go rejects those too (as "shift count too
// large"); collapsing both onto this one diagnostic is deliberate v1 scope.
static int check_const_shift_count(TypeChecker* checker, ASTNode* count_expr,
                                   Position pos) {
    uint64_t folded;
    if (goo_fold_const_int_ctx(checker, count_expr, &folded) &&
        (int64_t)folded < 0) {
        type_error(checker, pos, "invalid negative shift count: %lld",
                   (long long)(int64_t)folded);
        return 0;
    }
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

    // P2.8 T4.2 (cascade suppression): an operand bound to a previously
    // failed declaration (see register_declared_names_after_failure) must
    // not spawn a SECOND diagnostic here — propagate the poison silently,
    // before any operator-specific check below gets a chance to reject it
    // (e.g. "Arithmetic operation requires numeric operands"). Single choke
    // point: type_check_arithmetic_op has exactly one caller, right here, so
    // guarding this entry covers every binary operator uniformly.
    if (type_is_poison(left_type) || type_is_poison(right_type)) {
        Type* poison = type_is_poison(left_type) ? left_type : right_type;
        expr->node_type = poison;
        return poison;
    }

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
            result_type = type_check_bitwise_op(checker, left_type, right_type, binary->operator, expr->pos);
            break;

        // Shift operators: same bitwise checks as above, plus a compile-time
        // reject on a constant-folded negative count (Go: "invalid negative
        // shift count") — scoped to ONLY these two cases, not the shared
        // bitwise body above, since `binary->right` means "shift count" here
        // but "ordinary RHS operand" for &/&^/|/^ (a negative-when-signed
        // fold there, e.g. an all-ones mask constant, is not a shift count
        // and must not be rejected as one).
        case TOKEN_LSHIFT:
        case TOKEN_RSHIFT:
            if (!check_const_shift_count(checker, binary->right, expr->pos)) {
                result_type = NULL;
                break;
            }
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
            if ((binary->operator == TOKEN_LSHIFT_ASSIGN || binary->operator == TOKEN_RSHIFT_ASSIGN) &&
                !check_const_shift_count(checker, binary->right, expr->pos)) {
                result_type = NULL;
                break;
            }
            // Arc 7 (n): const-IDENT-bearing constant RHS representability.
            // The pre-switch literal adaptation range-checks literal-rooted
            // RHS shapes (`v = 300` into int8 rejects there), but a const
            // identifier is not rooted, so `v = a` / `v = a + 100` /
            // `s.x = a` fell through to type_check_assignment_op's any-int
            // laxness and codegen truncated the store. Judge the FOLDED
            // value against the assignment target via the shared core
            // (check_const_int_expr_fits) — left_type is the field/element
            // type for selector/index targets, so those are covered by the
            // same call. Compound arithmetic assigns convert the constant
            // to the target's type first (Go semantics), so the same rule
            // applies; SHIFT-assigns are excluded — their RHS is a shift
            // COUNT (validated above), not a value converting to the
            // target's type. The rooted guard keeps literal shapes on the
            // pre-switch adapter's path (disjoint, bit-for-bit unchanged).
            if (binary->operator != TOKEN_LSHIFT_ASSIGN &&
                binary->operator != TOKEN_RSHIFT_ASSIGN &&
                !is_untyped_int_rooted(binary->right, 0) &&
                check_const_int_expr_fits(checker, binary->right, left_type) < 0) {
                result_type = NULL;
                break;
            }
            result_type = type_check_assignment_op(checker, binary->left, left_type, right_type, binary->right, expr->pos);
            break;
            
        // Channel send operator
        case TOKEN_ARROW:  // ch <- value
            // Fix 2 (comptime-param functions are not first-class values):
            // `ch <- fill` transports fill's VALUE to a receiver with no
            // func_decl_node to check a later call against — the same alias
            // bypass as assignment. type_check_channel_send_op sees only
            // TYPES, so the expression-carrying gate lives here (the
            // select-statement send comm has its own sibling gate in
            // type_check_select_stmt).
            if (!reject_comptime_function_value(checker, binary->right, right_type,
                                                expr->pos, "sent on a channel")) {
                return NULL;
            }
            result_type = type_check_channel_send_op(checker, left_type, right_type, binary->right, expr->pos);
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

    // P2.8 FIX F1 (cascade-suppression completeness): an operand bound to a
    // previously failed declaration (see register_declared_names_after_
    // failure) must not spawn a SECOND diagnostic here — propagate the
    // poison silently, before any operator-specific check below gets a
    // chance to reject it (numeric, boolean, integer, dereference, ...).
    // Single choke point, mirroring type_check_binary_expr's guard: every
    // unary operator's operand check lives in the switch below, so guarding
    // this one entry covers all of them uniformly.
    if (type_is_poison(operand_type)) {
        expr->node_type = operand_type;
        return operand_type;
    }

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
            // Go semantics: map values are not addressable — &m[k] is illegal.
            // (The runtime slot may hold a heap box, but exposing it would
            // alias storage that overwrite replaces silently.) operand_type
            // above is the map's VALUE type (int, P, ...); re-derive the
            // INDEX EXPRESSION's base type via its already-stamped node_type
            // (type_check_expression on unary->operand, above, recursed into
            // ix->expr and stamped it) to see whether the base is a map.
            if (unary->operand->type == AST_INDEX_EXPR) {
                IndexExprNode* ix = (IndexExprNode*)unary->operand;
                if (ix->expr && ix->expr->node_type && ix->expr->node_type->kind == TYPE_MAP) {
                    type_error(checker, expr->pos,
                              "cannot take the address of a map value "
                              "(map values are not addressable)");
                    return NULL;
                }
            }
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

// Shared accept rule for append(dst, s...) and copy(dst, src): src_t is
// assignable into dst_t (a SLICE type, verified by the caller) when it's a
// slice with an identical (not merely compatible) element type, or — when
// dst's element is byte — a string.
static int slice_or_string_assignable(Type* src_t, Type* dst_t) {
    int byte_dst = dst_t->data.slice.element_type->kind == TYPE_UINT8; // byte kind (Task 2)
    return (src_t->kind == TYPE_SLICE &&
            type_compatible(src_t->data.slice.element_type,
                             dst_t->data.slice.element_type) &&
            src_t->data.slice.element_type->kind ==
            dst_t->data.slice.element_type->kind)
           || (byte_dst && src_t->kind == TYPE_STRING);
}

// Function generics Task 6: fully handles a call whose callee is a generic
// Variable (callee_var->is_generic), replacing the ordinary fixed-arity path
// in type_check_call_expr below for this call entirely — arity is checked
// against the generic signature's own param_count, every argument is
// type-checked here, and the substituted return type becomes the call's
// node_type. Callers must `return` this function's result directly rather
// than falling through, since the normal check_signature/param_types loop
// has no notion of TYPE_PARAM.
//
// Inference walks each (declared param type, checked arg type) pair through
// unify_types, which structurally matches the (possibly TYPE_PARAM-bearing)
// param against the concrete arg and writes newly-inferred bindings — Tier A
// scope only recurses through TYPE_SLICE/TYPE_POINTER/TYPE_FUNCTION (see its
// doc comment in types.c). A bare TYPE_PARAM parameter already bound to a
// DIFFERENT concrete type is unify_types' most common failure mode, so it is
// special-cased here (pre-checking bindings[idx] before calling unify_types)
// to produce a "conflicting types" diagnostic naming both types, rather than
// unify_types' generic 0 return folding into the same message as an
// unrelated structural mismatch (e.g. []int against *int).
// Tier B: find the bound (constraint interface) for type-param index `idx` by
// locating a TYPE_PARAM with that index anywhere in a generic signature's
// param types. Every type param appears in a parameter (Tier A invariant), so
// this finds it. Returns the constraint Type* (a TYPE_INTERFACE), or NULL.
static Type* generic_param_constraint(Type* t, int idx) {
    if (!t) return NULL;
    switch (t->kind) {
        case TYPE_PARAM:
            return t->data.type_param.index == idx ? t->data.type_param.constraint : NULL;
        case TYPE_SLICE:   return generic_param_constraint(t->data.slice.element_type, idx);
        case TYPE_POINTER: return generic_param_constraint(t->data.pointer.pointee_type, idx);
        case TYPE_FUNCTION: {
            for (size_t i = 0; i < t->data.function.param_count; i++) {
                Type* c = generic_param_constraint(t->data.function.param_types[i], idx);
                if (c) return c;
            }
            return generic_param_constraint(t->data.function.return_type, idx);
        }
        default: return NULL;
    }
}

// Tier B transitive-bound support: does the abstract constraint interface
// `have_iface` (a type parameter's OWN bound) structurally cover every
// method required by `want_iface` (the callee's bound)? Used when a
// generic call's inferred binding is itself an abstract TYPE_PARAM — e.g.
// `Inner(x)` called from inside `Outer`'s body, where `x`'s type is
// `Outer`'s own type parameter `T`, not a concrete type yet. There is no
// concrete method table to consult in that case (type_interface_satisfied
// looks up mangled "Concrete__method" names and would always report
// "missing" for an abstract T), so satisfaction is checked by comparing
// method names between the two interfaces instead. Name match is
// sufficient for Tier B. A NULL/non-interface `have_iface` (a bare `any`
// type parameter, i.e. no constraint) covers only an empty `want_iface`.
// True if two interface-method function types have identical signatures.
// Both carry NO receiver (unlike a registered concrete method, whose params[0]
// is the receiver — see type_interface_satisfied), so params line up directly.
// A missing return type and TYPE_VOID are treated as equivalent.
static int iface_method_sig_equals(Type* have_fn, Type* want_fn) {
    if (!have_fn || have_fn->kind != TYPE_FUNCTION ||
        !want_fn || want_fn->kind != TYPE_FUNCTION) return 0;
    if (have_fn->data.function.param_count != want_fn->data.function.param_count)
        return 0;
    for (size_t k = 0; k < want_fn->data.function.param_count; k++) {
        if (!type_equals(have_fn->data.function.param_types[k],
                         want_fn->data.function.param_types[k]))
            return 0;
    }
    Type* hr = have_fn->data.function.return_type;
    Type* wr = want_fn->data.function.return_type;
    int h_void = !hr || hr->kind == TYPE_VOID;
    int w_void = !wr || wr->kind == TYPE_VOID;
    if (h_void != w_void) return 0;
    if (!h_void && !type_equals(hr, wr)) return 0;
    return 1;
}

static int interface_covers(Type* have_iface, Type* want_iface) {
    if (!want_iface || want_iface->kind != TYPE_INTERFACE) return 0;
    if (want_iface->data.interface.method_count == 0) return 1;
    if (!have_iface || have_iface->kind != TYPE_INTERFACE) return 0;
    for (InterfaceMethod* wm = want_iface->data.interface.methods; wm; wm = wm->next) {
        int found = 0;
        for (InterfaceMethod* hm = have_iface->data.interface.methods; hm; hm = hm->next) {
            // A name match is not enough: the signatures must be identical, or a
            // caller-side bound could smuggle in a same-named method with a
            // different arity/type and miscompile at the concrete call site.
            if (hm->name && wm->name && strcmp(hm->name, wm->name) == 0 &&
                iface_method_sig_equals(hm->type, wm->type)) { found = 1; break; }
        }
        if (!found) return 0;
    }
    return 1;
}

// Fix round 7 (I-r6): does `t` contain a TYPE_PARAM anywhere in its
// structure? Checker-side sibling of the monomorphizer's static
// args_contain_typeparam (monomorphize.c) — same Tier-A shapes unify_types
// recurses, plus arrays/nullables for completeness. Used by
// type_check_generic_call to detect a parameter position that would BIND a
// type parameter from an argument (as opposed to a fully concrete position).
// Non-static (declared in types.h): sub-project 2 reuses it from
// type_checker.c's declare_function_signature for the `comptime n T`
// declaration wall — see that call site's comment for why.
int type_contains_type_param(const Type* t) {
    if (!t) return 0;
    switch (t->kind) {
        case TYPE_PARAM:
            return 1;
        case TYPE_SLICE:
            return type_contains_type_param(t->data.slice.element_type);
        case TYPE_POINTER:
            return type_contains_type_param(t->data.pointer.pointee_type);
        case TYPE_ARRAY:
            return type_contains_type_param(t->data.array.element_type);
        case TYPE_NULLABLE:
            return type_contains_type_param(t->data.nullable.base_type);
        case TYPE_FUNCTION: {
            for (size_t i = 0; i < t->data.function.param_count; i++) {
                if (type_contains_type_param(t->data.function.param_types[i]))
                    return 1;
            }
            return type_contains_type_param(t->data.function.return_type);
        }
        default:
            return 0;
    }
}

// Comptime value params Task 2: the VarDeclNode at parameter position `idx`
// in a FuncDeclNode's parameter list (skipping any non-VarDeclNode entries —
// none are expected today, but declare_function_signature's own param_types
// build loop applies the same guard). idx is 0-based over parameters only,
// matching func_type->data.function.param_types[idx] indexing. Returns NULL
// past the end of the list (e.g. a trailing variadic-packed argument beyond
// the fixed prefix, which has no per-argument VarDeclNode of its own).
//
// Moved above type_check_generic_call (sub-project 2): the generic-call loop
// now needs this same lookup to detect a comptime position, so it must be
// defined before its first use rather than only before type_check_call_expr's
// fixed-arity loop further down.
static VarDeclNode* func_decl_param_at(FuncDeclNode* fd, size_t idx) {
    if (!fd) return NULL;
    size_t i = 0;
    for (ASTNode* p = fd->params; p; p = p->next) {
        if (p->type != AST_VAR_DECL) continue;
        if (i == idx) return (VarDeclNode*)p;
        i++;
    }
    return NULL;
}

// Comptime value params Task 3 (sub-project 2: shared across
// type_check_call_expr's fixed-arity loop AND type_check_generic_call's
// generic loop — the "3rd near-copy avoided" the composition map calls out).
// Validates that `arg` is a compile-time-constant int via the same two-tier
// fold (goo_fold_const_int_ctx, then the comptime engine) sub-project 1
// established, and appends the resolved value to call->comptime_value_args,
// growing by one. `param_vd` is the comptime parameter's own VarDeclNode
// (used only for its name, in diagnostics). Preconditions the CALLER must
// establish before invoking this (not re-checked here): comptime_first_check
// holds for `call`'s CallExprNode, and param_vd->is_comptime_param is set —
// this helper trusts both and unconditionally attempts the capture.
//
// Returns 1 with the value appended on success. On failure it emits the
// diagnostic itself AND resets call->comptime_value_args/_arg_count to the
// clean (NULL,0) state (matching the entry-reset every re-visit would
// otherwise perform, without waiting for one) before returning 0 — every
// caller must treat 0 as "a type error was already reported; propagate
// NULL/failure up the stack without emitting a second diagnostic."
static int type_check_capture_comptime_arg(TypeChecker* checker,
                                            CallExprNode* call, ASTNode* arg,
                                            VarDeclNode* param_vd) {
    // Tier 1 — the checker-aware const folder, which resolves scope-
    // registered constants (`const K = 3`, package-level consts, `comptime
    // const M int = 2+2`) via each Variable's cached const_int_value; the
    // comptime ENGINE alone (tier 2) has no view of checker-scope constants.
    // Guarded by goo_expr_references_comptime_param: inside a comptime
    // template body the folder would resolve the enclosing comptime PARAM to
    // its placeholder — such an argument skips the fold and falls to the
    // engine, which rejects it (transitive comptime forwarding is a
    // documented restriction).
    // Tier 2 — the comptime engine, for everything the folder doesn't handle
    // (e.g. comptime block results). A runtime variable fails both tiers ->
    // clean rejection.
    int64_t comptime_arg_value = 0;
    int resolved = 0;
    uint64_t folded = 0;
    if (!goo_expr_references_comptime_param(checker, arg) &&
        goo_fold_const_int_ctx(checker, arg, &folded)) {
        comptime_arg_value = (int64_t)folded;
        resolved = 1;
    }
    if (!resolved) {
        ComptimeContext* raw_ctx = checker->comptime_type_ctx
            ? checker->comptime_type_ctx->comptime_ctx : NULL;
        ComptimeResult* res = raw_ctx
            ? comptime_eval_expression(raw_ctx, arg) : NULL;
        int ok = res && res->value && !res->error &&
                 res->value->type == COMPTIME_VALUE_INT;
        if (!ok) {
            if (res) comptime_result_free(res);
            type_error(checker, arg->pos,
                "argument to comptime parameter '%s' must be a compile-time constant",
                param_vd->names[0]);
            // Error-path hygiene: drop any values captured for EARLIER
            // comptime args of this same failing call so the node leaves
            // this function in the clean (NULL,0) state at the point of
            // failure (the entry-gate free would also catch it on a
            // re-visit; this just doesn't wait for one).
            free(call->comptime_value_args);
            call->comptime_value_args = NULL;
            call->comptime_value_arg_count = 0;
            return 0;
        }
        comptime_arg_value = res->value->int_value;
        comptime_result_free(res);
    }
    // Capture the resolved int for the monomorphizer, in parameter order
    // (ast.h's field doc comment) — a compact grow-by-one.
    int64_t* grown = realloc(call->comptime_value_args,
        (call->comptime_value_arg_count + 1) * sizeof(int64_t));
    if (!grown) {
        type_error(checker, arg->pos,
            "out of memory recording comptime argument '%s'",
            param_vd->names[0]);
        // Error-path hygiene: same clean-state teardown as the rejection
        // path above (realloc failure leaves the old allocation valid —
        // free it, don't leak it).
        free(call->comptime_value_args);
        call->comptime_value_args = NULL;
        call->comptime_value_arg_count = 0;
        return 0;
    }
    call->comptime_value_args = grown;
    call->comptime_value_args[call->comptime_value_arg_count++] = comptime_arg_value;
    return 1;
}

// Task C (explicit generic instantiation, f[T](...)): shared core behind
// BOTH type_check_generic_call (inference-only, `f(7, 8)`) and
// type_check_generic_call_explicit (`f[int](7, 8)`) below. `preseeded_
// bindings`, when non-NULL, is a caller-allocated `n`-slot array with one or
// more slots already bound (explicit instantiation's single type argument)
// — this function takes OWNERSHIP of it exactly as it already owns a
// freshly calloc'd array on the inference-only path (frees it on every
// error return, hands it off to call->type_args on success). Passing NULL
// reproduces type_check_generic_call's pre-Task-C behavior byte-for-byte.
static Type* type_check_generic_call_core(TypeChecker* checker, ASTNode* expr,
                                           CallExprNode* call, Variable* callee_var,
                                           const char* callee_name,
                                           Type** preseeded_bindings) {
    // Comptime+generic composition (sub-project 2): honor the same
    // first-visit contract type_check_call_expr enforces before dispatching
    // here (its own comptime_first_check/entry-reset, above the callee-kind
    // dispatch that forwards to this function) — codegen re-invokes
    // type_check_call_expr on the same CallExprNode (call_codegen.c), which
    // re-dispatches here for a generic callee every time. The caller's own
    // entry-reset already runs before every dispatch into this function (so
    // call->comptime_value_args is already (NULL,0) by the time a true
    // first visit reaches here), but recomputing the flag locally keeps the
    // per-arg capture loop below self-contained: it gates the capture
    // helper call directly on this function's own local reasoning rather
    // than trusting a value computed two stack frames away.
    int comptime_first_check = (expr->node_type == NULL);
    if (comptime_first_check) {
        free(call->comptime_value_args);
        call->comptime_value_args = NULL;
        call->comptime_value_arg_count = 0;
    }

    Type* gsig = callee_var->type;
    if (!gsig || gsig->kind != TYPE_FUNCTION) {
        type_error(checker, expr->pos,
                   "%s is marked generic but has no function signature",
                   callee_name);
        // Fix round (T-C review, Finding 1): ownership of preseeded_bindings
        // (the explicit-instantiation caller's calloc'd array — see the
        // function's own doc comment) doesn't transfer to the local
        // `bindings` variable until below; an early return before that point
        // must free the caller's array itself or it leaks (e.g.
        // `first[int](1)` with a defensive-only trip of this branch).
        free(preseeded_bindings);
        return NULL;
    }
    size_t n = callee_var->type_param_count;
    size_t pc = gsig->data.function.param_count;

    size_t argc = 0;
    for (ASTNode* a = call->args; a; a = a->next) argc++;

    if (argc != pc) {
        type_error(checker, expr->pos,
                   "wrong number of arguments to %s: expected %zu, got %zu",
                   callee_name, pc, argc);
        // Fix round (T-C review, Finding 1): same pre-ownership leak as
        // above — this is the ordinarily-reachable trip, e.g.
        // `first[int](1)` where first expects 2 args.
        free(preseeded_bindings);
        return NULL;
    }

    // Task C: an explicit-instantiation caller already allocated `bindings`
    // with the bracketed type argument bound in (see the doc comment above)
    // — reuse it instead of calloc'ing a fresh all-NULL array. `explicit_
    // inst` also gates the widening-adaptation step in the per-argument
    // loop below: it must fire ONLY for a binding that came from an
    // explicit type argument, never for one an earlier argument's own
    // checked type already inferred (see that step's doc comment for why).
    int explicit_inst = (preseeded_bindings != NULL);
    Type** bindings = explicit_inst ? preseeded_bindings
                                     : calloc(n ? n : 1, sizeof(Type*));
    if (!bindings) return NULL;

    size_t k = 0;
    for (ASTNode* a = call->args; a; a = a->next, k++) {
        Type* at = type_check_expression(checker, a);
        if (!at) { free(bindings); return NULL; }

        // Fix 2 (comptime-param functions are not first-class values): a
        // generic call bypasses type_check_call_expr's argument loop entirely
        // (this function is its replacement for generic callees), so it needs
        // its own copy of that loop's gate — `Apply(fill, 5)` into
        // `func Apply[T any](f func(int, int) int, x T)` captured fill's
        // VALUE into a func-typed parameter with zero diagnostics.
        if (!reject_comptime_function_value(checker, a, at, a->pos,
                                            "passed as an argument")) {
            free(bindings);
            return NULL;
        }

        Type* pt = gsig->data.function.param_types[k];

        // Comptime+generic composition (sub-project 2): a position whose
        // declared param is_comptime_param never binds a type parameter —
        // the declaration wall in declare_function_signature (`comptime n T`
        // rejected) guarantees `pt` here is always a plain concrete type for
        // a comptime position, never a bare/contained TYPE_PARAM. It
        // validates as that concrete type and captures its value via the
        // same helper type_check_call_expr's fixed-arity loop uses
        // (type_check_capture_comptime_arg), then is EXCLUDED from
        // unify_types entirely — unify_types would otherwise fall to its
        // `type_equals` default-case comparison of `pt` against `at` (the
        // argument's own static type, e.g. an untyped-literal's default
        // width), and a spurious kind/width mismatch there would surface as
        // "cannot infer type arguments", the wrong diagnostic for this
        // position; the capture helper's own "must be a compile-time
        // constant" is the correct rejection here instead. Gated on
        // comptime_first_check exactly like the non-generic loop: a
        // codegen-phase re-invocation must not re-capture (or re-evaluate,
        // which can even false-fail against defer's rewritten argument
        // nodes) values already captured on the first pass.
        VarDeclNode* param_vd = (callee_var->func_decl_node &&
                callee_var->func_decl_node->type == AST_FUNC_DECL)
            ? func_decl_param_at((FuncDeclNode*)callee_var->func_decl_node, k)
            : NULL;
        if (param_vd && param_vd->is_comptime_param) {
            if (comptime_first_check &&
                !type_check_capture_comptime_arg(checker, call, a, param_vd)) {
                free(bindings);
                return NULL;
            }
            continue;
        }

        // Fix round 7 (I-r6): a comptime-length array VALUE meeting the
        // generic axis. Two cases, split by whether this parameter position
        // binds a type parameter:
        // - BINDS one (`Id(a)` with a: [n]int into `x T`, or any pt shape
        //   containing a TYPE_PARAM): reject at template time. The generic
        //   instance would otherwise be stamped against the TEMPLATE's
        //   placeholder-length Type (the recorded binding) while the call
        //   site passes the instance-real array — the mismatch failed
        //   closed, but only at the LLVM verifier ("Call parameter type
        //   does not match function signature"), violating the
        //   clean-diagnostics bar. Per-value re-binding of generic
        //   instances is the composition follow-up (design doc,
        //   Scope/YAGNI).
        // - CONCRETE array position (`g(1, a)` into `arr [4]int`): the
        //   wave-6 length deferral applies — element types must match now,
        //   unify is skipped (it compares the placeholder length), and the
        //   call codegen's argument loop enforces the instance-real length
        //   (instance-named rejection on a genuine mismatch).
        if (goo_type_contains_comptime_array(at)) {
            if (type_contains_type_param(pt)) {
                type_error(checker, a->pos,
                    "comptime-length array cannot bind a generic type parameter (not yet supported)");
                free(bindings);
                return NULL;
            }
            if (pt && pt->kind == TYPE_ARRAY && at->kind == TYPE_ARRAY &&
                type_equals(pt->data.array.element_type,
                            at->data.array.element_type)) {
                continue; // length deferred to instance time (call codegen)
            }
        }

        if (pt && pt->kind == TYPE_PARAM) {
            int idx = pt->data.type_param.index;
            // Task C — explicit-instantiation widening: `f[float64](7)`
            // pre-binds T=float64 (idx 0) before this loop ever runs, but
            // the literal `7` was just checked (type_check_expression
            // above) at ITS OWN default type, int64 — inference alone never
            // needs this step (a binding there always came FROM an earlier
            // argument's own checked type, so a later mismatch is always a
            // genuine conflict), but an explicit binding is independent of
            // every argument and legitimately wider than an untyped
            // literal's default width/kind. Adapt the literal node in
            // place — the SAME is_untyped_int_rooted/adapt_untyped_int_
            // operand pair the ordinary fixed-arity call-arg loop uses
            // further down this file (type_check_call_expr) for a
            // non-generic parameter — so `at` reflects the adapted type
            // before the conflict check below runs. A non-literal argument
            // (a variable, a call result, ...) is never int-rooted and
            // falls through unchanged, keeping the ordinary
            // conflicting-types rejection for a genuine mismatch
            // (`f[int]("hello")`).
            if (explicit_inst && idx >= 0 && (size_t)idx < n && bindings[idx] &&
                !type_equals(bindings[idx], at)) {
                Type* bt = bindings[idx];
                if (type_is_integer(bt) && is_untyped_int_rooted(a, 0)) {
                    if (!adapt_untyped_int_operand(checker, a, bt, 0, 1)) {
                        free(bindings);
                        return NULL;
                    }
                    at = bt;
                } else if (type_is_float(bt) && is_untyped_int_rooted(a, 1)) {
                    if (!adapt_untyped_int_operand(checker, a, bt, 0, 1)) {
                        free(bindings);
                        return NULL;
                    }
                    at = bt;
                }
            }
            if (idx >= 0 && (size_t)idx < n && bindings[idx] &&
                !type_equals(bindings[idx], at)) {
                type_error(checker, expr->pos,
                           "cannot infer %s: conflicting types %s and %s",
                           pt->data.type_param.name ? pt->data.type_param.name : "type parameter",
                           type_to_string(bindings[idx]), type_to_string(at));
                free(bindings);
                return NULL;
            }
        }
        if (!unify_types(pt, at, bindings, n)) {
            type_error(checker, expr->pos,
                       "cannot infer type arguments for %s: argument %zu (%s) does not match",
                       callee_name, k + 1, type_to_string(at));
            free(bindings);
            return NULL;
        }
    }

    for (size_t i = 0; i < n; i++) {
        if (!bindings[i]) {
            type_error(checker, expr->pos,
                       "cannot infer type parameter %zu of %s", i, callee_name);
            free(bindings);
            return NULL;
        }
    }

    // Tier B: enforce interface-constraint bounds — each inferred concrete type
    // must satisfy its type param's bound. `any` / 0-method bounds are
    // satisfied by everything, so skip them.
    for (size_t i = 0; i < n; i++) {
        Type* bound = NULL;
        for (size_t p = 0; p < pc && !bound; p++)
            bound = generic_param_constraint(gsig->data.function.param_types[p], (int)i);
        if (bound && bound->kind == TYPE_INTERFACE &&
            bound->data.interface.method_count > 0) {
            if (bindings[i] && bindings[i]->kind == TYPE_PARAM) {
                // Transitive case: the inferred binding is itself an
                // abstract type parameter (e.g. calling Inner(x) from
                // inside Outer's body, where x : Outer's own T). Check that
                // T's own constraint structurally covers `bound` instead of
                // running the concrete-only type_interface_satisfied,
                // which would always report "missing" against an abstract
                // receiver name.
                Type* have = bindings[i]->data.type_param.constraint;
                if (!interface_covers(have, bound)) {
                    const char* pname = bindings[i]->data.type_param.name;
                    const char* hname = (have && have->kind == TYPE_INTERFACE &&
                                          have->data.interface.name)
                                             ? have->data.interface.name : "any";
                    type_error(checker, expr->pos,
                        "type parameter %s (constraint %s) does not satisfy %s",
                        pname ? pname : "T", hname,
                        bound->data.interface.name ? bound->data.interface.name : "interface");
                    free(bindings);
                    return NULL;
                }
            } else {
                const char* method = NULL; const char* reason = NULL;
                if (!type_interface_satisfied(checker, bound, bindings[i], &method, &reason)) {
                    // Fix 2: the "comptime" sentinel gets the dedicated
                    // one-place diagnostic — see
                    // report_comptime_method_not_satisfied's doc comment
                    // (every type_interface_satisfied caller does this).
                    if (reason && strcmp(reason, "comptime") == 0) {
                        report_comptime_method_not_satisfied(checker, expr->pos, method);
                        free(bindings);
                        return NULL;
                    }
                    const char* cn = type_receiver_name(bindings[i]);
                    type_error(checker, expr->pos,
                        "%s does not implement %s (%s method %s)",
                        cn ? cn : type_to_string(bindings[i]),
                        bound->data.interface.name ? bound->data.interface.name : "interface",
                        reason ? reason : "missing", method ? method : "?");
                    free(bindings);
                    return NULL;
                }
            }
        }
    }

    // Record the instantiation for the monomorphizer (Task 9). The recorder
    // gets its OWN copy of the bindings array rather than aliasing the one
    // handed to call->type_args below — the checker's instantiation list
    // (freed by type_checker_free) and this call node (freed by
    // ast_node_free) each then own an independent allocation, so neither
    // teardown path has to reason about which runs first or double-frees a
    // shared pointer.
    Type** rec_args = n ? malloc(n * sizeof(Type*)) : NULL;
    if (n && !rec_args) { free(bindings); return NULL; }
    if (rec_args) memcpy(rec_args, bindings, n * sizeof(Type*));

    // Comptime+generic composition (sub-project 2): copy this call's
    // captured comptime values (0/NULL for a generic-only function — the
    // per-arg loop above only grows call->comptime_value_arg_count for a
    // genuinely comptime position) into the same independent-copy
    // discipline rec_args uses just above, so the seed's second payload is
    // torn down by type_checker_free without ever aliasing back to this
    // CallExprNode's own comptime_value_args (ast_node_free's teardown).
    // Correct on a re-invocation too: call->comptime_value_args retains the
    // FIRST pass's captured values across a codegen re-check (the capture
    // loop above only re-runs the helper when comptime_first_check holds),
    // so this copy reads the right data whether this is a first visit or a
    // re-visit — matching call->type_args/bindings' own unconditional
    // rebuild-every-call pattern just above.
    int64_t* rec_comptime = call->comptime_value_arg_count
        ? malloc(call->comptime_value_arg_count * sizeof(int64_t)) : NULL;
    if (call->comptime_value_arg_count && !rec_comptime) {
        free(bindings);
        free(rec_args);
        return NULL;
    }
    if (rec_comptime) {
        memcpy(rec_comptime, call->comptime_value_args,
               call->comptime_value_arg_count * sizeof(int64_t));
    }
    type_check_record_instantiation(checker, callee_var, rec_args, n,
                                     rec_comptime, call->comptime_value_arg_count,
                                     expr);

    call->type_args = bindings;
    call->type_arg_count = n;

    Type* result = type_substitute(gsig->data.function.return_type, bindings, n);
    expr->node_type = result;
    return result;
}

// Function generics Task 6 (unchanged wrapper — see the shared-core doc
// comment above): ordinary inference-only call site, `f(7, 8)`. No
// pre-seeded bindings; every type parameter is inferred from the arguments.
static Type* type_check_generic_call(TypeChecker* checker, ASTNode* expr,
                                      CallExprNode* call, Variable* callee_var,
                                      const char* callee_name) {
    return type_check_generic_call_core(checker, expr, call, callee_var,
                                         callee_name, NULL);
}

// Task C: explicit instantiation `f[T](...)` — single type parameter only.
// The grammar's index_expr holds exactly one bracketed expression (`f[int,
// string](...)` does not parse — a pre-existing grammar limit, out of this
// task's scope; the caller's own gate never reaches here for it), which is
// why explicit instantiation is restricted to a generic callee declaring
// exactly one type parameter (checked below).
//
// `type_arg_node` is the bracketed AST node — resolved as a TYPE via
// type_from_ast, NOT type_check_expression, which would evaluate it as a
// VALUE and reject a bare type name like `int` with "Undefined variable
// 'int'" (the exact diagnostic this task replaces). The caller
// (type_check_call_expr) only reaches this function once its own
// disambiguation gate has already confirmed callee_var->is_generic, so a
// genuine index expression (`arr[i]()`) never routes through here — see
// that call site's doc comment for the full safety argument.
//
// Reuses every other line of type_check_generic_call_core's machinery —
// per-argument checking (including the widening adaptation gated on
// explicit_inst there), Tier-B constraint enforcement, monomorphizer
// recording, and result substitution — completely unchanged.
static Type* type_check_generic_call_explicit(TypeChecker* checker, ASTNode* expr,
                                               CallExprNode* call, Variable* callee_var,
                                               const char* callee_name,
                                               ASTNode* type_arg_node) {
    if (callee_var->type_param_count != 1) {
        type_error(checker, expr->pos,
                   "%s declares %zu type parameters; explicit instantiation "
                   "with a single bracketed type argument is only supported "
                   "for single-type-parameter generic functions",
                   callee_name, callee_var->type_param_count);
        return NULL;
    }

    Type* explicit_type = type_from_ast(checker, type_arg_node);
    if (!explicit_type) return NULL; // type_from_ast already reported the error

    Type** bindings = calloc(1, sizeof(Type*));
    if (!bindings) return NULL;
    bindings[0] = explicit_type;

    return type_check_generic_call_core(checker, expr, call, callee_var,
                                         callee_name, bindings);
}

// min(a, b, ...) / max(a, b, ...) -> the smallest/largest argument (Go
// 1.21). At least one argument required; every argument must be of an
// "ordered" type — Go restricts min/max to types supporting `<` (v1 has no
// user-defined ordered types beyond the three basic kinds, so: integer,
// float, or string). The result type follows the SAME untyped-constant
// rules as a chain of binary comparisons (type_check_binary_expr, above):
// an untyped-constant-rooted argument (bare literal, unary -/+/^ through
// to one, or a {+,-,*} subtree of those — is_untyped_int_rooted /
// is_untyped_float_rooted) adapts to the single concrete (non-rooted) type
// present among the OTHER arguments; two different concrete types is a
// hard reject ("mismatched types"), the same rule `int8var + int16var`
// gets. Reuses type_check_binary_expr's own adapters rather than inventing
// a separate constraint system, per the task design.
//
// Two passes, not a left-to-right fold: a fold only re-adapts the MOST
// RECENTLY visited node against a newly discovered concrete type, leaving
// earlier already-folded literal nodes stamped at a stale width — e.g.
// `min(1, 2, int32var)` would leave `1` stamped int64 while `2` and
// int32var end up int32, an LLVM width mismatch codegen cannot recover
// from. Pass 1 determines the target type without mutating any node; pass
// 2 adapts (or rejects) every node against that now-known-correct target.
//
// Documented deviation from full Go constant semantics (see the task
// report): Go treats an all-constant min/max call as itself a constant,
// usable anywhere a constant expression is required, with EXACT
// (arbitrary-precision) arithmetic. Goo folds the INTEGER case (see
// goo_fold_const_int/goo_fold_const_int_ctx's AST_CALL_EXPR arm in
// expression_helpers.c) so `[min(2,3)]int` and `var x int8 = min(1, 2)`
// both work like a genuine constant, including overflow rejection — this
// is the case the task's representability gates cover and must not
// silently diverge on. FLOAT and STRING constant-folding are NOT
// implemented (no goo_fold_const_float/string equivalent exists in this
// checker); an all-constant `min("a", "b")` or `min(1.0, 2.0)` still
// type-checks and runs correctly, just as an ordinary RUNTIME comparison
// chain rather than a compile-time constant — it cannot be used where Go
// requires a genuine constant (an array length, a case label, etc.).
static Type* type_check_minmax_call(TypeChecker* checker, ASTNode* expr,
                                     CallExprNode* call, const char* name) {
    if (!call->args) {
        type_error(checker, expr->pos, "%s expects at least one argument", name);
        return NULL;
    }

    size_t n = 0;
    for (ASTNode* a = call->args; a; a = a->next) n++;
    ASTNode** nodes = malloc(n * sizeof(ASTNode*));
    Type** types = malloc(n * sizeof(Type*));
    if (!nodes || !types) {
        free(nodes); free(types);
        type_error(checker, expr->pos, "out of memory checking %s call", name);
        return NULL;
    }

    size_t i = 0;
    for (ASTNode* a = call->args; a; a = a->next, i++) {
        Type* t = type_check_expression(checker, a);
        if (!t) { free(nodes); free(types); return NULL; }
        if (type_is_poison(t)) {
            // P2.8 cascade suppression, matching close()'s identical guard.
            free(nodes); free(types);
            expr->node_type = t;
            return t;
        }
        nodes[i] = a;
        types[i] = t;
    }

    // Pass 1: classify every argument and settle on the target type,
    // WITHOUT mutating any node yet (see the fold-order bug this avoids,
    // in the doc comment above).
    int any_string = 0, any_numeric = 0, any_rooted_float = 0;
    Type* concrete_float = NULL; // the single non-rooted float type seen, if any
    Type* concrete_int = NULL;   // the single non-rooted integer type seen, if any
    for (i = 0; i < n; i++) {
        Type* t = types[i];
        if (t->kind == TYPE_STRING) { any_string = 1; continue; }
        if (type_is_float(t)) {
            any_numeric = 1;
            if (is_untyped_float_rooted(nodes[i])) {
                any_rooted_float = 1;
            } else if (concrete_float && concrete_float->kind != t->kind) {
                type_error(checker, expr->pos, "%s: mismatched types %s and %s",
                           name, type_to_string(concrete_float), type_to_string(t));
                free(nodes); free(types);
                return NULL;
            } else if (!concrete_float) {
                concrete_float = t;
            }
            continue;
        }
        if (type_is_integer(t)) {
            any_numeric = 1;
            if (!is_untyped_int_rooted(nodes[i], 0)) {
                if (concrete_int && concrete_int->kind != t->kind) {
                    type_error(checker, expr->pos, "%s: mismatched types %s and %s",
                               name, type_to_string(concrete_int), type_to_string(t));
                    free(nodes); free(types);
                    return NULL;
                }
                if (!concrete_int) concrete_int = t;
            }
            continue;
        }
        type_error(checker, expr->pos,
                   "%s: argument of type %s is not ordered (integer, float, or string required)",
                   name, type_to_string(t));
        free(nodes); free(types);
        return NULL;
    }

    if (any_string && any_numeric) {
        type_error(checker, expr->pos, "%s: cannot mix string and numeric arguments", name);
        free(nodes); free(types);
        return NULL;
    }

    Type* target;
    if (any_string) {
        target = checker->builtin_types[TYPE_STRING];
    } else if (concrete_float) {
        // A concrete (non-rooted) INT argument can never adapt to a float
        // target — no implicit int->float conversion for a typed value,
        // same rule type_check_binary_expr's cross-kind block enforces.
        // Caught explicitly here rather than left to pass 2, whose
        // per-node adapt only knows how to reject a ROOTED mismatch.
        if (concrete_int) {
            type_error(checker, expr->pos, "%s: mismatched types %s and %s",
                       name, type_to_string(concrete_int), type_to_string(concrete_float));
            free(nodes); free(types);
            return NULL;
        }
        target = concrete_float;
    } else if (concrete_int) {
        target = concrete_int;
    } else {
        // Every numeric argument is untyped-constant-rooted (e.g. `min(1,
        // 2)`, `max(1.0, 2.0)`, or a mix like `min(1, 2.5)`) — Go's default
        // type for an untyped constant: float64 if ANY argument is
        // float-family (kind promotion, matching is_untyped_float_rooted's
        // own binop leg), else int (int64 here).
        target = any_rooted_float ? checker->builtin_types[TYPE_FLOAT64]
                                   : checker->builtin_types[TYPE_INT64];
    }

    // Pass 2: adapt every rooted node to `target` (or reject a genuine,
    // non-adaptable mismatch pass 1's classification didn't already catch
    // — e.g. a shift-rooted int meeting a float target, which is rooted
    // under is_untyped_int_rooted(_, 0) but NOT under the stricter
    // for_float_context=1 shape type_check_binary_expr's own cross-kind
    // block requires).
    for (i = 0; i < n; i++) {
        Type* t = types[i];
        ASTNode* node = nodes[i];
        if (t->kind == TYPE_STRING || t->kind == target->kind) continue;

        if (type_is_float(target)) {
            if (type_is_float(t) && is_untyped_float_rooted(node)) {
                if (!adapt_untyped_float_operand(checker, node, target, 0)) {
                    free(nodes); free(types);
                    return NULL;
                }
                continue;
            }
            if (type_is_integer(t) && is_untyped_int_rooted(node, 1)) {
                if (!adapt_untyped_int_operand(checker, node, target, 0, 1)) {
                    free(nodes); free(types);
                    return NULL;
                }
                continue;
            }
        } else if (type_is_integer(target)) {
            if (type_is_integer(t) && is_untyped_int_rooted(node, 0)) {
                if (!adapt_untyped_int_operand(checker, node, target, 0, 1)) {
                    free(nodes); free(types);
                    return NULL;
                }
                continue;
            }
        }
        type_error(checker, expr->pos, "%s: mismatched types %s and %s",
                   name, type_to_string(t), type_to_string(target));
        free(nodes); free(types);
        return NULL;
    }

    free(nodes);
    free(types);
    expr->node_type = target;
    return target;
}

// Comptime value params (fix round 2's I2 guard; promoted in fix round 4):
// the comptime-param-reference walk now lives in expression_helpers.c as
// goo_expr_references_comptime_param — type_from_ast (type_checker.c)
// became its third consumer for comptime_length stamping. At the
// comptime-argument capture site below, it guards the const-folding fast
// path: goo_fold_const_int_ctx resolves ANY Variable with
// has_const_int_value — which inside a comptime function's TEMPLATE body
// includes the comptime param itself, bound to a PLACEHOLDER (see
// type_check_function_decl's is_comptime_param binding). Folding
// `helper(n, seed)`'s `n` there would silently record the placeholder as
// the instance value — a miscompile — where the design instead makes
// transitive comptime forwarding a documented restriction (design doc,
// Scope/YAGNI): such an argument must fall through to the comptime engine,
// which cannot resolve the param and rejects with the standard
// "must be a compile-time constant" diagnostic.

Type* type_check_call_expr(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_CALL_EXPR) return NULL;

    CallExprNode* call = (CallExprNode*)expr;

    // Comptime value params: comptime_value_args/comptime_value_arg_count
    // are zeroed at every parser.y CallExprNode construction site (fix
    // round 3 — see ast.h's field doc comment for the ownership contract),
    // and this function owns them thereafter. It is NOT called exactly
    // once per call node: codegen re-invokes it on the same CallExprNode
    // (call_codegen.c's method-call return-type recomputation and defer
    // re-emission paths), and recapturing there would clobber the values
    // captured on the first pass with a re-evaluation against whatever
    // scope codegen currently has mirrored (fix round 1, finding 4).
    // `expr->node_type` is the first-visit discriminator: reliably NULL at
    // parse time (every construction site zeroes base.node_type) and set
    // at the end of this function's successful main path — so node_type !=
    // NULL means the values were already captured by an earlier full check
    // and the capture/record sites below (gated on this same flag) must
    // leave them untouched.
    int comptime_first_check = (expr->node_type == NULL);
    if (comptime_first_check) {
        // Fields are zeroed from birth, so this free is unconditionally
        // safe: a true first visit frees NULL (no-op); a re-attempt after
        // a FAILED first check (node_type still NULL) frees that attempt's
        // partial capture instead of orphaning it (closes fix round 2's
        // residual transient leak completely).
        free(call->comptime_value_args);
        call->comptime_value_args = NULL;
        call->comptime_value_arg_count = 0;
    }

    // Task C: explicit generic instantiation `f[T](...)` parses as
    // CallExpr(IndexExpr(f, T), args) — Go's own grammar shape, unchanged
    // here (no parser work; see the goo-grammar tripwire). Recognize it
    // BEFORE the ordinary callee check further down (`func_type =
    // type_check_expression(checker, call->function)`), which would
    // otherwise recurse into type_check_index_expr and type-check the
    // bracketed TYPE as a VALUE expression — a bare type name like `int`
    // has no Variable binding, so that path failed with "Undefined
    // variable 'int'" (the manifest-documented gap this task closes).
    //
    // Disambiguation safety: this block fires only when the index base
    // identifier resolves to a Variable that is a FUNCTION — an ordinary
    // index expression `arr[i]()` (calling an element of a function-slice)
    // can never satisfy that, since `arr` there is a TYPE_SLICE/TYPE_ARRAY/
    // TYPE_MAP Variable, not TYPE_FUNCTION — so this is a purely syntactic
    // dispatch with zero risk of misclassifying real indexing.
    // type_from_ast (inside type_check_generic_call_explicit) is only ever
    // invoked once this gate has already confirmed the base names a
    // function, so a genuine index expression never reaches it and never
    // risks a spurious "Unknown type" diagnostic on its index operand.
    if (call->function && call->function->type == AST_INDEX_EXPR) {
        IndexExprNode* idx_expr = (IndexExprNode*)call->function;
        if (idx_expr->expr && idx_expr->expr->type == AST_IDENTIFIER) {
            IdentifierNode* base_ident = (IdentifierNode*)idx_expr->expr;
            Variable* base_var = type_checker_lookup_variable(checker, base_ident->name);
            if (base_var && base_var->is_generic) {
                return type_check_generic_call_explicit(checker, expr, call,
                                                         base_var, base_ident->name,
                                                         idx_expr->index);
            }
            // A NON-generic function indexed (`notGeneric[int](...)`): Go's
            // own grammar shape again, but there is no generic
            // instantiation to perform. Reject cleanly here, with a single
            // diagnostic regardless of what the bracketed content happens
            // to be — rather than falling through to the ordinary
            // index-expr path, which gives an inconsistent diagnostic
            // depending on the bracket's content (a bare type name like
            // `int` fails as "Undefined variable"; a real in-scope value
            // fails later as "Cannot index type function(...)").
            if (base_var && base_var->type && base_var->type->kind == TYPE_FUNCTION) {
                type_error(checker, expr->pos,
                           "%s is not generic; it cannot be explicitly "
                           "instantiated with a type argument",
                           base_ident->name);
                return NULL;
            }
        }
    }

    // A type in call-argument position (map_type/slice_type/chan_type — the
    // grammar alternative added for `make(...)`) only means something when
    // the callee is the `make` builtin. Any other callee reaching here with
    // one (e.g. `foo(map[string]int)`, `foo(chan int)`) is rejected here
    // with a clean, specific diagnostic. Without this guard the argument
    // would fall through to the generic type_check_expression() call in the
    // argument-checking loop below, whose switch has no case for
    // AST_MAP_TYPE/AST_SLICE_TYPE/AST_CHAN_TYPE and would instead emit the
    // far less useful "Unknown expression type".
    if (call->args && (call->args->type == AST_MAP_TYPE || call->args->type == AST_SLICE_TYPE
                        || call->args->type == AST_CHAN_TYPE)) {
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
        // string(x) where x is ANY integer kind (Task 2, port unblocker):
        // Go-conformant rune/byte->string conversion — the result is x's
        // value interpreted as a Unicode code point and UTF-8-encoded (see
        // goo_string_from_rune's doc comment in runtime.h for the
        // invalid-code-point rule). Placed AHEAD of the generic
        // name_is_builtin_conv_name gate below, which still recognizes
        // "string" as a conversion NAME (so the shadowing/name-recognition
        // logic doesn't need duplicating) but only ever produces NULL for it
        // (builtin_conversion_target has no "string" case) and rejects with
        // "cannot convert to string ... only numeric conversions". This arm
        // intercepts "string" first so that generic rejection never fires.
        // Scope: rune/byte->string, AND (Task 2, stdlib unblocker)
        // string([]byte) — a []byte source is checked FIRST, below, ahead
        // of the integer/TYPE_CHAR check, since a slice is neither.
        if (strcmp(func_ident->name, "string") == 0 &&
            !name_is_user_shadowed(checker, func_ident->name)) {
            if (!call->args || call->args->next) {
                type_error(checker, expr->pos,
                           "conversion string() expects exactly one argument");
                return NULL;
            }
            Type* src = type_check_expression(checker, call->args);
            if (!src) return NULL;
            // Task 2: string(b) where b is []byte — Go copies on
            // conversion (see goo_cstr_from_bytes's doc comment in
            // runtime.h); the result never aliases the source slice.
            if (src->kind == TYPE_SLICE &&
                src->data.slice.element_type->kind == TYPE_UINT8) {
                Type* str_type = type_checker_get_builtin(checker, TYPE_STRING);
                expr->node_type = str_type;
                return str_type;
            }
            // Only integer-kind sources convert (Go: rune/byte/int.../uint...
            // and the char/rune literal kind TYPE_CHAR). Floats, bool,
            // string, and non-byte-slice aggregate sources are NOT
            // integer-to-string conversions in Go and are rejected here
            // rather than reaching codegen with no lowering.
            if (!type_is_integer(src) && src->kind != TYPE_CHAR) {
                type_error(checker, expr->pos,
                           "cannot convert %s to string (only integer-kind "
                           "conversions — rune/byte/int-family — or a "
                           "[]byte operand are supported)",
                           type_to_string(src));
                return NULL;
            }
            Type* str_type = type_checker_get_builtin(checker, TYPE_STRING);
            expr->node_type = str_type;
            return str_type;
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
                // type_from_ast() already rejects a non-comparable or
                // not-yet-supported map key type with its own clean error
                // before returning here (the AST_MAP_TYPE gate), so `made`
                // never carries a rejected key type at this point.
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
            if (made->kind == TYPE_CHANNEL) {
                // make(chan T[, capacity]): capacity optional, integer,
                // defaults to unbuffered (0) — same shape as make_chan(T[,
                // capacity])'s (type_check_make_chan_call above) except
                // `made` here is already the resolved channel type (the
                // grammar's chan_type alternative on call->args produced it
                // directly via type_from_ast), so it is NOT re-wrapped in
                // another type_channel() call the way make_chan's bare
                // element-type argument is.
                ASTNode* cap_arg = call->args->next;
                if (cap_arg && cap_arg->next) {
                    type_error(checker, expr->pos,
                               "make(chan T, capacity) takes at most two arguments");
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
            type_error(checker, expr->pos, "make() requires a map, slice, or channel type");
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
            // append(dst, s...) (Task 4): the second arg is a whole []E
            // (identical elem, not merely compatible) or, when dst's
            // element is byte, a string — instead of a bare element.
            // `has_spread` is set by the grammar for ANY trailing `expr...`
            // call argument (Task 3); append has its own dedicated arm
            // (this one), so it must read the flag itself rather than
            // relying on the generic variadic-pack path Task 3 modified.
            if (call->has_spread) {
                Type* src_t = type_check_expression(checker, call->args->next);
                if (!src_t) return NULL;
                int ok = slice_or_string_assignable(src_t, slice_t);
                if (!ok) {
                    type_error(checker, expr->pos,
                               "append: cannot spread %s into %s",
                               type_to_string(src_t), type_to_string(slice_t));
                    return NULL;
                }
                expr->node_type = slice_t;
                return slice_t;
            }
            Type* elem_t = type_check_expression(checker, call->args->next);
            if (!elem_t) return NULL;
            // Fix 2 (comptime-param functions are not first-class values):
            // append is special-cased ABOVE the generic argument loop (it
            // returns early), so the loop's per-argument gate never sees its
            // element — `append(s, fill)` stored fill into a slice with zero
            // diagnostics. The slice argument (arg 1) and a spread source
            // (`append(s, s2...)`) need no gate: both must be TYPE_SLICE
            // (rejected above otherwise), and a slice ELEMENT type is built
            // by type_from_ast from a type annotation, which never carries
            // has_comptime_params — only a named function's own declared
            // signature Type does, and every way of getting one INTO a slice
            // is gated (composite literal, this element path, index-assign
            // via the assignment gate).
            if (!reject_comptime_function_value(checker, call->args->next, elem_t,
                                                call->args->next->pos,
                                                "passed as an argument")) {
                return NULL;
            }
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
        // copy(dst, src) -> int (Go-exact: min(len(dst), len(src)) count).
        // dst must be a slice; src is a slice with an identical element type,
        // or (when dst's element is byte) a string — same acceptance rule
        // as append(dst, s...) above. Result type is always int, unlike
        // append, so this rides a fixed node_type rather than the first
        // arg's dynamic type.
        if (strcmp(func_ident->name, "copy") == 0) {
            if (!call->args || !call->args->next || call->args->next->next) {
                type_error(checker, expr->pos, "copy expects exactly two arguments (dst, src)");
                return NULL;
            }
            Type* dst_t = type_check_expression(checker, call->args);
            if (!dst_t) return NULL;
            if (dst_t->kind != TYPE_SLICE) {
                type_error(checker, expr->pos,
                           "copy: destination must be a slice, got %s", type_to_string(dst_t));
                return NULL;
            }
            Type* src_t = type_check_expression(checker, call->args->next);
            if (!src_t) return NULL;
            int ok = slice_or_string_assignable(src_t, dst_t);
            if (!ok) {
                type_error(checker, expr->pos,
                           "copy: cannot copy %s into %s", type_to_string(src_t), type_to_string(dst_t));
                return NULL;
            }
            expr->node_type = checker->builtin_types[TYPE_INT64]; // Go: copy -> int (64-bit)
            return expr->node_type;
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
            if (arg_t->kind != TYPE_SLICE && arg_t->kind != TYPE_STRING &&
                arg_t->kind != TYPE_MAP && arg_t->kind != TYPE_ARRAY) {
                type_error(checker, expr->pos,
                           "len() requires an array, slice, string, or map argument");
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
            if (slice_t->kind != TYPE_SLICE && slice_t->kind != TYPE_ARRAY) {
                type_error(checker, expr->pos,
                           "cap: argument must be a slice or array, got %s", type_to_string(slice_t));
                return NULL;
            }
            expr->node_type = checker->builtin_types[TYPE_INT64]; // Go: cap -> int (64-bit)
            return checker->builtin_types[TYPE_INT64];
        }
        // close(ch) -> void (P3.1). Statement-only builtin (no result value,
        // like delete/panic below); codegen lowers it to goo_chan_close.
        // Exactly one argument, which must be a channel — anything else
        // would hand codegen a non-pointer value to pass to goo_chan_close,
        // which unconditionally dereferences it.
        if (strcmp(func_ident->name, "close") == 0) {
            if (!call->args || call->args->next) {
                type_error(checker, expr->pos, "close expects exactly one argument (channel)");
                return NULL;
            }
            Type* chan_t = type_check_expression(checker, call->args);
            if (!chan_t) return NULL;
            // P2.8 cascade suppression: a poisoned argument already carries
            // its diagnostic; don't stringify it into a second one here.
            if (type_is_poison(chan_t)) return chan_t;
            if (chan_t->kind != TYPE_CHANNEL) {
                type_error(checker, expr->pos,
                           "close: argument must be a channel, got %s", type_to_string(chan_t));
                return NULL;
            }
            expr->node_type = checker->builtin_types[TYPE_VOID];
            return checker->builtin_types[TYPE_VOID];
        }
        // recover() — rejected in v1 (P3.5, user decision 2026-07-10:
        // minimum scope). The builtin is registered in scope purely so the
        // call reaches THIS message instead of "Undefined variable
        // 'recover'". Returning NULL routes `r := recover()` through the
        // failed-declaration path, which poisons r (P2.8) — no cascade.
        if (strcmp(func_ident->name, "recover") == 0) {
            type_error(checker, expr->pos,
                       "recover() is not supported in v1; panics terminate the program "
                       "(use !T error unions for recoverable errors)");
            return NULL;
        }
        // delete(m, k) -> void. Removes key k from map m (no-op if absent).
        // Exactly two args: the first must be a map, the second assignable
        // to its key type (any admitted key kind — see the AST_MAP_TYPE
        // comparability gate). Codegen lowers to goo_map_delete_sv, passing
        // the key packed into its i64 slot like every other map-op site.
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
            // Interface-typed key (Task 2): accept any concrete implementer,
            // mirroring type_check_index_expr's TYPE_MAP arm.
            if (map_t->data.map.key_type->kind == TYPE_INTERFACE) {
                if (!check_interface_assign(checker, key_t, map_t->data.map.key_type, expr->pos)) {
                    return NULL;
                }
            } else {
                // Arc 9 (i): constant keys gate against the declared key
                // width, same as the index-key choke point — an ungated
                // `delete(m, 300)` on map[int8]int passed the raw folded
                // i64 to the runtime and silently no-opped forever.
                Type* want_key = map_t->data.map.key_type;
                if (type_is_integer(want_key)) {
                    if (is_untyped_int_rooted(call->args->next, 0)) {
                        if (!adapt_untyped_int_operand(checker, call->args->next,
                                                       want_key, 0, 1))
                            return NULL;
                        key_t = want_key;
                    } else if (check_const_int_expr_fits(checker, call->args->next,
                                                         want_key) < 0) {
                        return NULL;
                    }
                }
                if (!type_compatible(key_t, want_key)) {
                    type_error(checker, expr->pos,
                               "delete: cannot use %s as key of %s",
                               type_to_string(key_t), type_to_string(map_t));
                    return NULL;
                }
            }
            expr->node_type = checker->builtin_types[TYPE_VOID];
            return checker->builtin_types[TYPE_VOID];
        }
        // clear(m) / clear(s) -> void (Go 1.21). Map: removes every entry
        // (equivalent to deleting every key, but codegen lowers it to one
        // dedicated goo_map_clear_sv pass instead of an entry-by-entry
        // delete loop). Slice: zeroes every element up to len — len and
        // cap are UNCHANGED (this is not a truncation; codegen memsets the
        // backing store, it never touches the header). Exactly one
        // argument, which must be a map or slice — anything else would
        // hand codegen a value with neither a GooMapSV* nor a {ptr,len,cap}
        // header to clear.
        if (strcmp(func_ident->name, "clear") == 0) {
            if (!call->args || call->args->next) {
                type_error(checker, expr->pos, "clear expects exactly one argument (map or slice)");
                return NULL;
            }
            Type* arg_t = type_check_expression(checker, call->args);
            if (!arg_t) return NULL;
            // P2.8 cascade suppression, matching close()'s identical guard.
            if (type_is_poison(arg_t)) return arg_t;
            if (arg_t->kind != TYPE_MAP && arg_t->kind != TYPE_SLICE) {
                type_error(checker, expr->pos,
                           "clear: argument must be a map or slice, got %s", type_to_string(arg_t));
                return NULL;
            }
            expr->node_type = checker->builtin_types[TYPE_VOID];
            return checker->builtin_types[TYPE_VOID];
        }
        // min(a, b, ...) / max(a, b, ...) -> the smallest/largest argument
        // (Go 1.21). Real checking (arity, ordered-type gate, untyped-
        // constant adaptation) lives in the shared type_check_minmax_call
        // helper above (name-parameterized — the two builtins differ only
        // in which comparison codegen emits, never in their TYPE rule).
        if (strcmp(func_ident->name, "min") == 0) {
            return type_check_minmax_call(checker, expr, call, "min");
        }
        if (strcmp(func_ident->name, "max") == 0) {
            return type_check_minmax_call(checker, expr, call, "max");
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

    // Check function expression.
    //
    // P3.6 (method values): if the callee is a selector expression, tell
    // type_check_selector_expr that ITS result feeds this call directly —
    // it must keep splicing the receiver into params[0] (the existing wire
    // format the recv_offset arity/arg-type logic below this point depends
    // on, unchanged). Every OTHER selector — a value position like
    // `f := c.get`, a function argument, a struct field initializer — never
    // sets this flag and gets the receiver-STRIPPED type instead (see
    // type_check_selector_expr's method-lookup arm). Cleared right after so
    // it doesn't leak into unrelated selector checks later in this
    // function's own argument loop (each argument is independently
    // call-position-neutral: an argument that is itself a method selector
    // in non-call position, e.g. `f(c.get)`, must also be stripped).
    int selector_callee = call->function && call->function->type == AST_SELECTOR_EXPR;
    if (selector_callee) checker->selector_call_position = 1;
    Type* func_type = type_check_expression(checker, call->function);
    if (selector_callee) checker->selector_call_position = 0;
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
        // P2.8 cascade suppression: a poisoned callee (bound by a failed
        // declaration, e.g. the rejected `f := i.m` interface method value)
        // already carries its diagnostic — propagate silently instead of
        // stringifying "<poisoned>" into a second one.
        if (type_is_poison(func_type)) return func_type;
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
    // Comptime value params Task 2: the resolved callee Variable, captured
    // whenever check_signature is set below (identifier, struct-method, or
    // source-package-function call), so the per-argument loop can walk its
    // func_decl_node (FuncDeclNode) to find each parameter's
    // is_comptime_param flag. Stays NULL for interface-method calls (no
    // concrete Variable to resolve to) — comptime params on an interface
    // method are simply not checked here.
    Variable* checked_callee = NULL;
    if (call->function && call->function->type == AST_IDENTIFIER
        && !skip_variadic_builtin) {
        IdentifierNode* callee_ident = (IdentifierNode*)call->function;
        Variable* callee = type_checker_lookup_variable(checker, callee_ident->name);
        // Function generics Task 6: a call through a generic Variable is
        // handled ENTIRELY by type_check_generic_call — arity, per-argument
        // type-checking, inference, and the result type all happen there,
        // bypassing the fixed-arity check_signature/param_types path below
        // (which knows nothing about TYPE_PARAM and would either false-reject
        // every call or, worse, silently pass one through with the wrong
        // result type). Must come before the `!callee->is_builtin` arm below:
        // a generic function is never is_builtin, but is also not meant to
        // fall into the ordinary check_signature path.
        if (callee && callee->is_generic) {
            return type_check_generic_call(checker, expr, call, callee,
                                            callee_ident->name);
        }
        if (callee && !callee->is_builtin) {
            check_signature = 1;
            callee_name = callee_ident->name;
            checked_callee = callee;
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
                    // P4.3: st may be a package-owned receiver type whose
                    // method Variable only lives in that package's exports
                    // scope (see type_checker_lookup_method's doc comment).
                    Variable* m = mangled
                        ? type_checker_lookup_method(checker, st, sel->selector, mangled)
                        : NULL;
                    free(mangled);
                    if (m && m->type == func_type && !m->is_builtin) {
                        check_signature = 1;
                        recv_offset = 1;
                        callee_name = sel->selector;
                        checked_callee = m;
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
                        checked_callee = exp;
                    }
                }
                // P4.1: no source export matched (true for every shim symbol —
                // the seeded shim Package carries an empty exports scope, per
                // the comment above) — fall back to the declarative shim
                // table. shim_signature_lookup already built func_type's real
                // param list (see stdlib_package_lookup), so this only needs
                // to confirm (package, name) names a known shim CALLABLE
                // before flipping check_signature on; no re-lookup of the
                // Type itself, and no Variable to record as checked_callee
                // (shim functions never have comptime params to walk).
                // Task B (alias imports): dispatch on the resolved package's
                // canonical import_path, matching type_check_selector_expr —
                // see that call site's comment.
                const char* dispatch_pkg = (pkg_marker && pkg_marker->package && pkg_marker->package->import_path)
                    ? pkg_marker->package->import_path : pkg_ident->name;
                if (!check_signature &&
                    shim_signature_is_known_call(dispatch_pkg, sel->selector)) {
                    check_signature = 1;
                    recv_offset = 0;
                    callee_name = sel->selector;
                }
            }
        }
    }

    // Task 3 (spread `f(s...)`): validated BEFORE the per-argument loop
    // below, which would otherwise misread the spread's slice-typed final
    // argument against the variadic slot's UNWRAPPED element type (producing
    // a confusing "cannot use []int as int" instead of the precise
    // diagnostics here). Go spread rules: the callee must be variadic, and
    // the call must supply exactly the fixed arguments then one slice — no
    // extra/missing args. The final argument's element-type match (no
    // coercion) is checked inside the loop below, at the variadic slot
    // position, once we know that position is reached exactly once.
    //
    // Both checks below are gated on `func_type->data.function.param_types`
    // (non-NULL), NOT on `check_signature`. check_signature exists to guard
    // the OLD per-arg diagnostics, whose name-based resolution (re-looking-up
    // the callee by mangled name/exports scope) can be unreliable for
    // non-identifier callees. These two spread checks don't need that: they
    // read param_count/param_types/is_variadic directly off func_type, the
    // callee EXPRESSION's own static type (already validated TYPE_FUNCTION
    // above), which is reliable regardless of how the callee resolved — the
    // unconditional `!callee_is_variadic` check just below is exactly this
    // pattern. Gating these on check_signature left a call through a
    // function-valued struct field (e.g. `o.Sum(s...)`, no name-based
    // resolution match, check_signature stays 0) completely unchecked: a
    // width/arity mismatch reached call_codegen's spread branch, which passes
    // the raw {ptr,len,cap} through with no per-element coercion — a latent
    // heap-OOB read in the callee, and for a missing fixed arg, a silent
    // codegen failure with zero diagnostic output.
    if (call->has_spread && !callee_is_variadic) {
        type_error(checker, expr->pos,
                   "spread argument requires a variadic function");
        return NULL;
    }
    if (call->has_spread && func_type->data.function.param_types) {
        size_t arg_total = 0;
        for (ASTNode* a = call->args; a; a = a->next) arg_total++;
        size_t declared_total = func_type->data.function.param_count - recv_offset;
        if (arg_total != declared_total) {
            type_error(checker, expr->pos,
                "spread call must supply exactly the fixed arguments then one slice (want %zu args, got %zu)",
                declared_total, arg_total);
            return NULL;
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

        // P2.8 FIX F1 (cascade-suppression completeness): an argument bound
        // to a previously failed declaration (see
        // register_declared_names_after_failure) must not spawn a SECOND
        // diagnostic here. Mirrors the return-statement guard's placement
        // (type_checker.c) and the binary-op choke point's (this file,
        // type_check_binary_expr) — skip every check below for this
        // argument (comptime-function-value, comptime-param capture, and
        // the type-compatibility comparisons that stringify arg_type) and
        // move on to the next one.
        if (type_is_poison(arg_type)) {
            arg_count++;
            arg = arg->next;
            continue;
        }

        // Fix 2 (comptime-param functions are not first-class values):
        // `takesFunc(fill)` would capture fill's VALUE into takesFunc's
        // func-typed parameter — a Variable with no func_decl_node on the
        // other side, the same bypass the var-decl/assignment/return guards
        // close, for the argument-passing channel. `arg` here is a VALUE
        // argument, never the callee itself (call->function is checked
        // separately, above this loop), so this cannot false-reject a direct
        // call's callee.
        if (!reject_comptime_function_value(checker, arg, arg_type, arg->pos,
                                            "passed as an argument")) {
            return NULL;
        }

        // Comptime value params Task 2: a `comptime name T` parameter demands
        // a compile-time-constant argument — evaluate it through the
        // comptime engine and require an int result. Independent of (and
        // checked before) the type-compatibility logic below: this is a
        // constness requirement, not a type mismatch, so it gets its own
        // diagnostic rather than falling through to "cannot use T as U".
        // Only reachable when checked_callee's real FuncDeclNode is known
        // (identifier/struct-method/source-package calls); an interface
        // method call has no concrete Variable to resolve is_comptime_param
        // from and is not checked here.
        //
        // Fix round 1 (finding 4): gated on comptime_first_check — on a
        // codegen-phase RE-invocation of this function over the same node,
        // the values were already validated and captured by the first pass;
        // re-appending would duplicate them, and re-EVALUATING can even
        // false-fail (defer's re-emission path rewrites argument nodes to
        // synthetic identifiers the comptime engine can't evaluate).
        if (comptime_first_check &&
            checked_callee && checked_callee->func_decl_node &&
            checked_callee->func_decl_node->type == AST_FUNC_DECL) {
            VarDeclNode* param_vd = func_decl_param_at(
                (FuncDeclNode*)checked_callee->func_decl_node, arg_count + recv_offset);
            if (param_vd && param_vd->is_comptime_param) {
                // Extracted (sub-project 2) into type_check_capture_comptime_arg,
                // shared with type_check_generic_call's per-arg loop — see its
                // doc comment above for the two-tier fold and the ownership/
                // error-path contract. Behavior here is unchanged: on failure
                // the helper has already emitted the diagnostic and reset
                // call->comptime_value_args to (NULL,0).
                if (!type_check_capture_comptime_arg(checker, call, arg, param_vd))
                    return NULL;
            }
        }

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
        // Reachable when check_signature is true (unchanged from before), OR
        // when call->has_spread is true regardless of check_signature — the
        // has_spread branch below (Task 3's strict element-identity check)
        // must run for a spread call through an unresolved-name callee (e.g.
        // a function-valued struct field) too; see the comment above the
        // pre-loop spread checks for why that's safe to trust off func_type
        // alone. The fixed-position sub-branch immediately below still
        // requires check_signature itself before indexing param_types — it
        // is the OLD per-arg diagnostic and stays exactly as reliable/
        // unreliable as before.
        if ((check_signature || call->has_spread) && param_types && callee_is_variadic && param_count > 0) {
            size_t last_idx = param_count - 1;
            if ((arg_count + recv_offset) < last_idx) {
                if (check_signature) {
                    param_type = param_types[arg_count + recv_offset];
                }
            } else if (call->has_spread) {
                // Task 3: the variadic slot's sole argument IS the spread
                // operand ([]E), not a single element — compare the WHOLE
                // slice type's element against the variadic slot's element
                // with strict identity (type_equals, no coercion: Go
                // requires E to match exactly, unlike the per-element path's
                // type_compatible below which would let []int32 slip into
                // ...int64). The pre-loop arg-count check above guarantees
                // this position is reached exactly once, by the final arg.
                Type* slice_t = param_types[last_idx];
                Type* elem_want = (slice_t && slice_t->kind == TYPE_SLICE)
                    ? slice_t->data.slice.element_type : NULL;
                if (arg_type->kind != TYPE_SLICE || !elem_want ||
                    !type_equals(arg_type->data.slice.element_type, elem_want)) {
                    type_error(checker, arg->pos,
                               "cannot spread %s into variadic parameter ...%s",
                               type_to_string(arg_type),
                               elem_want ? type_to_string(elem_want) : "?");
                    return NULL;
                }
                arg_count++;
                arg = arg->next;
                continue;
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
            } else if (arg && param_type && type_is_float(param_type) &&
                       is_untyped_int_rooted(arg, 1)) {
                // P4.1 finding (latent gap, not a shim-table error): this call-arg
                // path never adapted an untyped INT literal to a FLOAT parameter —
                // only the int-target branch above existed. It was never exercised
                // before shim signatures existed because check_signature never fired
                // for a package call (math.Sqrt/Pow/...), so `math.Pow(2, 10)`'s
                // literals stayed int64 and only codegen's separate SIToFP coercion
                // (codegen_generate_stdlib_call) made it work. Once math.Pow's real
                // float64 params are checked here, the SAME literals would otherwise
                // hit the numeric-width mismatch below ("cannot use int64 as
                // float64") for code that already compiled correctly. Mirrors the
                // for_float_context=1 adaptation struct fields/binary exprs already
                // do (see is_untyped_int_rooted's doc comment, which documents this
                // call-arg site as float-adapting even though, until now, it wasn't).
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
                // Arc 10 (o) rider: a SAME-width, differently-SIGNED pair
                // (uint64 const into an int64 param) slid past this guard
                // untouched and bit-reinterpreted — enter the gate for the
                // sign mismatch too, so a foldable constant is judged by
                // the shared core; a non-constant (fit == 0) sign-only
                // mismatch keeps the pre-existing acceptance (the plain-var
                // laxness wall) rather than becoming a new rejection.
                int sign_differs = type_is_integer(arg_type) &&
                                   type_is_integer(param_type) &&
                                   type_is_signed(arg_type) !=
                                       type_is_signed(param_type);
                if (!same_kind || !same_width || sign_differs) {
                    // Arc 10 (o): a const-IDENT-bearing constant argument is
                    // representable, not a width mismatch — `f(K)` with
                    // K = 5 into an int8 parameter must be accepted (Go),
                    // and K = 300 must reject as overflow, not as "cannot
                    // use int64 as int8". Literal-rooted shapes never reach
                    // here (adapted above, so same_kind/same_width hold);
                    // this leg sees only ident-bearing constants and
                    // non-constants. Fit (1): accept — call_codegen's arg
                    // loop (T4) coerces the value to the declared parameter
                    // width, exact for a representable constant. Reject
                    // (-1): the shared core already emitted the overflow
                    // diagnostic. Not applicable (0): plain variables keep
                    // this clean width mismatch.
                    int fit = type_is_integer(param_type)
                              ? check_const_int_expr_fits(checker, arg, param_type)
                              : 0;
                    if (fit < 0) return NULL;
                    if (fit > 0) {
                        arg_type = param_type;
                    } else if (!same_kind || !same_width) {
                        type_error(checker, arg->pos,
                                   "argument %zu: cannot use %s as %s",
                                   arg_count + 1,
                                   type_to_string(arg_type), type_to_string(param_type));
                        return NULL;
                    }
                    // fit == 0 with same kind and width (sign-only
                    // mismatch, non-constant): pre-existing acceptance.
                }
            }

            // Interface parameter (P4-3/P4-5): a concrete implementer may be
            // passed where an interface is expected. Check satisfaction here so
            // `f(Sq{})` into `func f(s Shape)` is accepted; codegen boxes it.
            //
            // F3 fix: this call-arg site duplicates check_interface_assign's
            // logic inline (to keep the "argument %zu:" message prefix)
            // rather than calling it, so it needs its own copy of the same
            // bare-nil short-circuit: `take(nil)` must not be routed into
            // type_interface_satisfied as if nil were a concrete type.
            if (param_type && param_type->kind == TYPE_INTERFACE &&
                arg_type && arg_type->kind == TYPE_UNKNOWN) {
                // accepted: nil is Go's sixth nilable kind for interfaces —
                // codegen_interface_box already boxes a TYPE_UNKNOWN concrete
                // to the zero {NULL,NULL} interface value.
            } else if (param_type && param_type->kind == TYPE_INTERFACE &&
                arg_type && arg_type->kind != TYPE_INTERFACE) {
                const char* method = NULL;
                const char* reason = NULL;
                if (!type_interface_satisfied(checker, param_type, arg_type,
                                              &method, &reason)) {
                    if (reason && strcmp(reason, "comptime") == 0) {
                        report_comptime_method_not_satisfied(checker, arg->pos, method);
                        return NULL;
                    }
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
                // Fix round 6 (M-r5a): a comptime-length array ARGUMENT
                // (`sum4(a)` with a: [n]int into a [4]int param) — the
                // template-time length here is the placeholder, so the
                // length comparison is meaningless until instance time.
                // Same deferral as assignment (type_check_assignment_op,
                // fix round 5): element types must still match now; the
                // call codegen's argument loop enforces the real
                // per-instance lengths (instance-named rejection on a
                // genuine mismatch). Ordinary array arguments reject here
                // exactly as before.
                int comptime_len_deferred =
                    arg_type && arg_type->kind == TYPE_ARRAY &&
                    param_type->kind == TYPE_ARRAY &&
                    (arg_type->data.array.comptime_length ||
                     param_type->data.array.comptime_length) &&
                    type_equals(arg_type->data.array.element_type,
                                param_type->data.array.element_type);
                if (!comptime_len_deferred) {
                    type_error(checker, arg->pos,
                               "argument %zu: cannot use %s as %s",
                               arg_count + 1,
                               type_to_string(arg_type), type_to_string(param_type));
                    return NULL;
                }
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
            // required. declared >= 1 normally holds (a variadic signature
            // carries at least the slice param), but the shim table's
            // zero-fixed-param variadic rows (fmt.Println family) encode
            // param_types=NULL/param_count=0 and rely on the
            // skip_variadic_builtin path never reaching here — an implicit
            // coupling (P4.1 review). Guard the subtraction so a future
            // table row that breaks that coupling degrades to min_args=0
            // instead of a size_t underflow that requires SIZE_MAX args.
            size_t min_args = declared > 0 ? declared - 1 : 0;
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

    // Comptime value params Task 3: record this call site as a
    // monomorphization seed once every argument (including each comptime
    // one, above) has validated — mirrors type_check_record_instantiation's
    // generic-axis recording (type_check_generic_call) but keyed on int64_t
    // comptime VALUES instead of concrete Types. Two callee shapes reach here
    // with comptime values: a plain identifier callee (a local comptime-param
    // function) and — since the P6 M1 wall lift — a package-function SELECTOR
    // (`pkg.Fill(4, ...)`), whose checked_callee is the surviving export copy.
    // A struct-method selector never does (declare_function_signature rejects
    // comptime params on methods), and an interface-method call has a NULL
    // checked_callee — so gating on checked_callee alone (rather than the
    // callee's AST shape) is exact. Gated on comptime_first_check so a
    // codegen-phase re-invocation doesn't append duplicate seeds after the
    // monomorphizer already ran (see the entry-reset comment at the top of
    // this function).
    if (comptime_first_check &&
        call->comptime_value_arg_count > 0 && call->function && checked_callee) {
        // P6 M1 (comptime-wall lift, front (b)): reject a SAME-package INTERNAL
        // comptime call. Its bare-name callee is the package's own inner-scope
        // Variable (owner_pkg NULL), which scope_pop frees right after the
        // package is codegen'd — before the main-pass monomorphizer consumes
        // the seed, a use-after-free. A cross-package `pkg.Fill(...)` call
        // instead binds the surviving EXPORT COPY (owner_pkg set); a top-level
        // (main) call runs with current_package == NULL. Only the intersection
        // (inside a package body AND callee has no owning package) is the
        // dangling case — rejected precisely here rather than by banning every
        // package-level comptime declaration.
        if (checker->current_package && !checked_callee->owner_pkg) {
            type_error(checker, expr->pos,
                "comptime call to '%s' from within a package is not yet supported",
                callee_name ? callee_name : "?");
            return NULL;
        }
        int64_t* rec_values = malloc(call->comptime_value_arg_count * sizeof(int64_t));
        if (!rec_values) {
            // Fix round 1 (finding 5): a silent skip here would surface much
            // later as an undefined-symbol failure at the (never-stamped)
            // instance's call site — fail loudly at the point of the actual
            // problem instead, mirroring the capture-site realloc error above.
            type_error(checker, expr->pos,
                "out of memory recording comptime instantiation of '%s'",
                callee_name ? callee_name : "?");
            return NULL;
        }
        memcpy(rec_values, call->comptime_value_args,
               call->comptime_value_arg_count * sizeof(int64_t));
        type_check_record_comptime_instantiation(checker, checked_callee,
            rec_values, call->comptime_value_arg_count, expr);
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
    
    // Check index type — integer for array/slice/string; a map's key can be
    // any of its admitted key kinds (string, integer, bool, char, pointer —
    // see the AST_MAP_TYPE comparability gate), so map indexing skips this
    // integer requirement and is checked against its own key type in the
    // switch below instead.
    if (expr_type->kind != TYPE_MAP && !type_is_integer(index_type)) {
        type_error(checker, index->index->pos,
                  "Array index must be integer, got %s", type_to_string(index_type));
        return NULL;
    }
    
    Type* element_type = NULL;
    
    switch (expr_type->kind) {
        case TYPE_ARRAY: {
            element_type = expr_type->data.array.element_type;
            // Go rejects a CONSTANT out-of-bounds array index at COMPILE time
            // (`arr[5]` on a [3]int is "index 5 out of bounds [0:3]"; a negative
            // constant folds to a huge unsigned value and so also fails). Fold a
            // constant index and check it against the static length; a
            // non-constant index falls through to the runtime bounds check
            // (goo_bounds_check) instead.
            uint64_t ci;
            if (goo_fold_const_int_ctx(checker, index->index, &ci)) {
                if ((int64_t)ci < 0) {
                    type_error(checker, index->index->pos,
                               "array index %lld must not be negative", (long long)ci);
                    return NULL;
                }
                // Fix round 4: a comptime-length array's `length` here is
                // the TEMPLATE placeholder — validating a const index
                // against it falsely rejected `buf[3]` on `[n]int` with
                // "out of bounds [0:1]", a bound the user never wrote.
                // Defer the upper-bound check to instance time (the codegen
                // index paths re-check against the instance's re-derived
                // REAL length and hard-fail a genuine violation); ordinary
                // arrays (comptime_length == 0) keep this check unchanged.
                // The negative-index rejection above stays unconditional —
                // invalid at every instance.
                if (!expr_type->data.array.comptime_length &&
                    ci >= (uint64_t)expr_type->data.array.length) {
                    type_error(checker, index->index->pos,
                               "array index %llu out of bounds [0:%zu]",
                               (unsigned long long)ci, expr_type->data.array.length);
                    return NULL;
                }
            }
            break;
        }
        case TYPE_SLICE:
            element_type = expr_type->data.slice.element_type;
            break;
        case TYPE_MAP: {
            // For maps, check if index type is compatible with key type. An
            // interface-typed key (Task 2, interface-typed map keys) accepts
            // any concrete implementer (boxed at codegen via
            // codegen_box_map_key_if_needed) — plain type_compatible rejects
            // a concrete implementer outright (it isn't the interface type
            // itself), so route interface-typed keys through
            // check_interface_assign instead, mirroring the map-literal
            // value check above (and this function's own key check). This
            // is the single type-check choke point for `m[k]` reads AND
            // `m[k] = v` assignments (the assignment's LHS is type-checked
            // as this same index expression) and comma-ok reads.
            Type* want_key = expr_type->data.map.key_type;
            if (want_key->kind == TYPE_INTERFACE) {
                if (!check_interface_assign(checker, index_type, want_key, index->index->pos)) {
                    return NULL;
                }
            } else {
                // Arc 9 (i): gate constant keys against the declared key
                // width — `m[300]` (read, write, or comma-ok; this arm is
                // the single choke point for all three) on a map[int8]int
                // must reject like Go instead of passing the raw folded
                // i64 to the runtime, where it can never equal any key
                // that is genuinely in the int8 domain. Literal-rooted
                // shapes adapt (stamping the key to the declared width —
                // codegen widens it back by signedness, exactly); const
                // identifiers are judged by the shared representability
                // core. Non-constant keys keep the plain compatibility
                // check — a typed narrow variable is in-domain by
                // construction.
                if (type_is_integer(want_key)) {
                    if (is_untyped_int_rooted(index->index, 0)) {
                        if (!adapt_untyped_int_operand(checker, index->index,
                                                       want_key, 0, 1))
                            return NULL;
                        index_type = want_key;
                    } else if (check_const_int_expr_fits(checker, index->index,
                                                         want_key) < 0) {
                        return NULL;
                    }
                }
                if (!type_compatible(index_type, want_key)) {
                    type_error(checker, index->index->pos,
                              "Map key type mismatch: expected %s, got %s",
                              type_to_string(want_key),
                              type_to_string(index_type));
                    return NULL;
                }
            }
            element_type = expr_type->data.map.value_type;
            break;
        }
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

// stdlib_package_lookup returns a Type for a (package, name) pair drawn from
// the stdlib shim surface. Returns NULL if the pair isn't known. Package
// VALUE members (not calls) are handled inline here, exactly as before —
// they carry no type_function wrapper and are outside shim_signatures.c's
// table (see that file's doc comment). Every CALLABLE member is now built
// from the declarative table (P4.1) via shim_signature_lookup, which
// supplies REAL parameter lists instead of the old param-less
// `type_function(NULL, 0, ret)` stubs — see shim_signatures.h for the full
// design and shim_signature_is_known_call for the call-checker hook this
// enables.
static Type* stdlib_package_lookup(TypeChecker* checker,
                                   const char* package,
                                   const char* name) {
    if (!checker || !package || !name) return NULL;

    // os.Args -> []string. A package VALUE member, not a call — mirrors
    // math.Pi below (no type_function wrapper). Real argv is captured
    // once at process entry (codegen's is_entry_main prologue calling
    // goo_os_args_init); argv[0] is always present for a real process,
    // so len(os.Args) >= 1 always holds even with no extra args.
    if (strcmp(package, "os") == 0 && strcmp(name, "Args") == 0) {
        Type* string_t = type_checker_get_builtin(checker, TYPE_STRING);
        return type_slice(string_t);
    }

    // math.Pi -> float64. A package VALUE member, not a call — the
    // returned type is the value's type, no type_function wrapper.
    if (strcmp(package, "math") == 0 && strcmp(name, "Pi") == 0) {
        return type_checker_get_builtin(checker, TYPE_FLOAT64);
    }

    return shim_signature_lookup(checker, package, name);
}

// Struct embedding desugar: rewrite `o.X` into `(o.Hop1.Hop2).X` in place, so
// every downstream consumer (lvalue addressing, method receiver auto-address,
// pointer auto-deref) sees only constructs that already ship. Promotion in Go
// is DEFINED as this sugar — the rewrite is the spec, executed.
static ASTNode* embed_wrap_base(ASTNode* base, const EmbedResult* r, Position pos) {
    for (size_t i = 0; i < r->len; i++) {
        SelectorExprNode* s = (SelectorExprNode*)xmalloc(sizeof(SelectorExprNode));
        s->base.type = AST_SELECTOR_EXPR;
        s->base.pos = pos;
        s->base.node_type = NULL;
        s->base.next = NULL;
        s->expr = base;
        s->selector = strdup(r->path[i]);
        base = (ASTNode*)s;
    }
    return base;
}

// P3.6 (method values): build the func type a method selector yields in
// VALUE position — the same signature `method_type` carries, minus the
// spliced receiver at params[0] (`type_check_function_decl` puts it there
// for every method; see the call comment above). type_function COPIES the
// param_types it's given (types.c), so handing it a pointer into the middle
// of method_type's OWN array is safe — the result owns an independent copy,
// and is_variadic is copied across explicitly since type_function always
// zero-initializes it fresh.
static Type* type_strip_receiver(Type* method_type) {
    size_t recv_count = method_type->data.function.param_count;
    if (recv_count == 0) return NULL;  // invariant violation: every method has a receiver
    size_t stripped_count = recv_count - 1;
    Type** stripped_params = stripped_count > 0
        ? &method_type->data.function.param_types[1] : NULL;
    Type* stripped = type_function(stripped_params, stripped_count,
                                   method_type->data.function.return_type);
    if (stripped) stripped->data.function.is_variadic = method_type->data.function.is_variadic;
    return stripped;
}

Type* type_check_selector_expr(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || expr->type != AST_SELECTOR_EXPR) return NULL;

    SelectorExprNode* selector = (SelectorExprNode*)expr;

    // P3.6 (method values): capture-and-clear the call-position flag the
    // caller (type_check_call_expr) set for us, immediately. Cleared before
    // the recursive base-expression check just below (selector->expr —
    // e.g. `c` in `c.get` — is never itself a call callee) so that check
    // can't inherit it; restored around the embedding re-invocation further
    // down (this function calling itself after embed_wrap_base rewrites the
    // AST), which resolves this SAME logical selector post-rewrite and must
    // see the original call-position verdict again.
    int is_call_callee = checker->selector_call_position;
    checker->selector_call_position = 0;

    Type* expr_type = type_check_expression(checker, selector->expr);
    if (!expr_type) return NULL;

    // P2.8 FIX F1 (cascade-suppression completeness): a poisoned base
    // expression (bound to a previously failed declaration — see
    // register_declared_names_after_failure) must not spawn a SECOND
    // diagnostic here. Single choke point, mirroring type_check_binary_
    // expr's guard: every struct/package/interface resolution below falls
    // through to "Selector on non-struct, non-package type" on a mismatch,
    // so guarding this one entry (e.g. a poisoned composite-literal
    // variable's `.field` access) covers all of them uniformly.
    if (type_is_poison(expr_type)) {
        expr->node_type = expr_type;
        return expr_type;
    }

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

        // Task B (alias imports): dispatch on the package's canonical
        // import_path (pkg_marker->package), not the use-site identifier —
        // `f.Println` after `import f "fmt"` must resolve exactly like
        // `fmt.Println`. pkg_marker was already resolved just above; a
        // shim package's marker always carries a Package* (seeded by
        // seed_imported_stdlib_markers), so this only falls back to the
        // raw identifier for a non-package selector base (already handled
        // by the TYPE_PACKAGE guard) or a defensive NULL.
        const char* dispatch_pkg = (pkg_marker && pkg_marker->package && pkg_marker->package->import_path)
            ? pkg_marker->package->import_path : pkg_ident->name;
        Type* fn_type = stdlib_package_lookup(checker, dispatch_pkg, selector->selector);
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
            // P4.3: struct_type may be a package-owned receiver type (e.g.
            // shapes.Point) — its method Variable only lives in that
            // package's exports scope, never in main's own scope chain (the
            // package's body scope is torn down right after codegen; see
            // type_checker_lookup_method's doc comment).
            Variable* m = mangled
                ? type_checker_lookup_method(checker, struct_type, selector->selector, mangled)
                : NULL;
            free(mangled);
            if (m && m->type && m->type->kind == TYPE_FUNCTION) {
                if (is_call_callee) {
                    // Existing method-CALL path, byte-for-byte unchanged:
                    // the receiver stays spliced as params[0] — see
                    // type_check_call_expr's recv_offset logic just above
                    // this function in the file.
                    expr->node_type = m->type;
                    return m->type;
                }
                // P4.7 (sync shim): reached only when !is_call_callee (the
                // is_call_callee branch above always returns). sync.Mutex /
                // sync.WaitGroup methods lower directly to goo_sync_*
                // runtime wrappers keyed off the CALL SITE's receiver
                // expression (call_codegen.c) — there is no goo_pkg__sync__
                // symbol for a bound thunk to close over (sync has no Goo
                // source body; see is_stdlib_shim_import). Reject method
                // VALUES here, mirroring the interface method-value scope
                // cut above, rather than let this reach codegen and either
                // crash or bind the wrong thing. Rejection (not a thunk
                // that also calls the wrapper) is the v1 choice — sync
                // method values are rare in practice and the thunk path
                // would need its own lazy-init-aware codegen with no reuse
                // from the direct-call path built for B3.
                {
                    Package* owner = type_receiver_owner_package(struct_type);
                    // import_path, not ->name: ->name is the call-site
                    // identifier (`import s "sync"` sets it to "s"), which
                    // would silently miss this check under an alias.
                    // import_path is the canonical path, alias-independent.
                    if (owner && owner->import_path && strcmp(owner->import_path, "sync") == 0) {
                        type_error(checker, expr->pos,
                                   "method values on sync.%s are not supported in v1 "
                                   "(call %s directly)",
                                   tn, selector->selector);
                        return NULL;
                    }
                }
                // P3.6: value position (`f := c.get`, a callback argument, a
                // struct field initializer, ...) — yield the receiver-
                // STRIPPED signature (params[1..]) so `f` type-checks and
                // calls as a plain 0-or-more-arg func value. The receiver
                // itself is bound into the func value's env cell at codegen
                // time (composite_codegen.c's method arm); nothing here
                // allocates or copies — this is a pure type-level view.
                Type* stripped = type_strip_receiver(m->type);
                if (!stripped) return NULL;
                expr->node_type = stripped;
                return stripped;
            }
        }
        EmbedResult er = embedding_resolve(checker, struct_type, selector->selector);
        if (er.kind == EMBED_FIELD || er.kind == EMBED_METHOD) {
            selector->expr = embed_wrap_base(selector->expr, &er, expr->pos);
            // Re-resolve: each inserted hop is a real (embedded) field, and
            // the leaf is now a direct member of its owner. Restore the
            // call-position verdict captured at entry — this recursive call
            // resolves the SAME logical selector (post-rewrite), so it must
            // see it again, not the cleared default.
            checker->selector_call_position = is_call_callee;
            return type_check_selector_expr(checker, expr);
        }
        if (er.kind == EMBED_AMBIGUOUS) {
            type_error(checker, expr->pos,
                       "ambiguous selector '%s' (found via %s and %s)",
                       selector->selector, er.ambig_a, er.ambig_b);
            return NULL;
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
                // P3.6 follow-up (sub-B review): interface METHOD VALUES
                // (`f := i.m`) are a v1 scope cut — binding needs vtable
                // dispatch through the bound thunk, which doesn't exist.
                // Reject HERE with an accurate positioned message; without
                // this the value passes typecheck and dies at codegen with
                // a misleading "Selector can only be applied to struct
                // types" attributed to the wrong line.
                if (!is_call_callee) {
                    type_error(checker, expr->pos,
                               "method values on interface types are not supported in v1 "
                               "(call the method directly, or bind from the concrete type)");
                    return NULL;
                }
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
    // P3.6: same call-vs-value fork as that path too — a named-int method in
    // value position (`f := n.double`) yields the receiver-stripped
    // signature, while a call callee keeps the spliced receiver for
    // type_check_call_expr's recv_offset logic. Only a BARE named type
    // reaches this arm: a pointer-to-named base (`p.double`, p *MyInt)
    // carries name "*MyInt" (type_pointer), mangles to a symbol that never
    // exists, and falls through to the rejection below — a pre-existing
    // limitation of the method-CALL path (verified 2026-07-10), unchanged
    // here, so the method-VALUE surface exactly tracks the callable surface.
    if (expr_type->name) {
        char* mangled = type_method_mangled_name(expr_type->name, selector->selector);
        // P4.3: same package-owned-receiver fallback as the struct arm above.
        Variable* m = mangled
            ? type_checker_lookup_method(checker, expr_type, selector->selector, mangled)
            : NULL;
        free(mangled);
        if (m && m->type && m->type->kind == TYPE_FUNCTION) {
            if (is_call_callee) {
                expr->node_type = m->type;
                return m->type;
            }
            Type* stripped = type_strip_receiver(m->type);
            if (!stripped) return NULL;
            expr->node_type = stripped;
            return stripped;
        }
    }

    // Function generics Tier B: a method call on a bounded type parameter.
    // `x.M()` where x : TYPE_PARAM resolves M against the bound interface's
    // method set (the checker sees the abstract T; monomorphization later
    // dispatches to the concrete type's M). An `any` (0-method) bound has no
    // methods, so an attempted method call correctly reaches the reject below.
    if (expr_type->kind == TYPE_PARAM &&
        expr_type->data.type_param.constraint &&
        expr_type->data.type_param.constraint->kind == TYPE_INTERFACE) {
        Type* bound = expr_type->data.type_param.constraint;
        for (InterfaceMethod* im = bound->data.interface.methods; im; im = im->next) {
            if (im->name && strcmp(im->name, selector->selector) == 0) {
                expr->node_type = im->type;
                return im->type;
            }
        }
        type_error(checker, expr->pos,
                   "type parameter %s (constraint %s) has no method '%s'",
                   expr_type->data.type_param.name ? expr_type->data.type_param.name : "T",
                   bound->data.interface.name ? bound->data.interface.name : "interface",
                   selector->selector);
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

    // P2.6 (T2): a user-declared (T, error) result tuple is accepted
    // alongside a genuine !T error union — see type_is_error_result_tuple's
    // doc comment (types.c) for why this is a structural (not flag-based)
    // check. Everything below keys off `is_tuple` to run the identical
    // control flow the !T path already established.
    int is_tuple = type_is_error_result_tuple(expr_type);

    // Expression must be an error union OR a (T, error) tuple
    if (!is_tuple && !type_is_error_union(expr_type)) {
        type_error(checker, expr->pos,
                  "try can only be used with error union types, got %s",
                  type_to_string(expr_type));
        return NULL;
    }

    // `try` propagates the error out of the ENCLOSING function on the error
    // path, so that function must itself return an error union (!T) —
    // regardless of whether the OPERAND is a !T or a (T,error) tuple. A
    // (T,error)-returning enclosing function is a distinct, out-of-scope
    // shape (design doc's Out of scope list): v1 `try` only propagates OUT
    // of !T-returning functions. Rejecting this here keeps the codegen
    // propagation path (LLVMBuildRet operand) total — before this check a
    // `try` in a non-!T function silently emitted `unreachable` (garbage
    // IR, no diagnostic).
    Type* enclosing = checker->current_return_type;
    if (!enclosing || !type_is_error_union(enclosing)) {
        type_error(checker, expr->pos,
                  "try requires the enclosing function to return an error union (!T)");
        return NULL;
    }

    if (is_tuple) {
        // The tuple's error field is always the boxed `error` interface
        // (type_is_error_result_tuple guarantees field 1 satisfies
        // type_is_error) — there is no distinct declared error ARM type to
        // compare against the enclosing union's, unlike the !T branch below.
        // try extracts the value type from field 0.
        Type* value_type = expr_type->data.struct_type.fields[0].type;
        expr->node_type = value_type;
        return value_type;
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

    // P2.6 (T2): a user-declared (T, error) result tuple is accepted
    // alongside a genuine !T error union — see type_is_error_result_tuple's
    // doc comment (types.c). Unlike try, catch has no enclosing-function
    // requirement: it handles the tuple locally, so the tuple path is a
    // pure sibling of the !T path from here on.
    int is_tuple = type_is_error_result_tuple(expr_type);

    // Expression must be an error union OR a (T, error) tuple
    if (!is_tuple && !type_is_error_union(expr_type)) {
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
        //
        // P2-7: bind the same `error` interface type the n,err destructure
        // path binds (type_checker.c:1969's type_checker_error_type call),
        // not the union's raw error arm (which defaults to plain
        // TYPE_STRING and has no method set — e.Error() failed with
        // "Selector on non-struct, non-package type"). This is unconditional
        // regardless of the union's declared error arm, mirroring the
        // destructure path exactly; codegen degrades a non-string arm
        // identically (function_codegen.c:1705-1734 / error_union_codegen.c).
        if (catch_expr->error_var) {
            Type* error_type = type_checker_error_type(checker);

            Variable* error_var = variable_new(catch_expr->error_var, error_type, expr->pos);
            if (error_var) {
                error_var->is_initialized = 1;
                scope_add_variable(checker->current_scope, error_var);
            }
        }

        type_check_statement(checker, catch_expr->catch_body);
        scope_pop(checker);
    }

    // The type of a catch expression is the value type of the error union,
    // or field 0 for a (T,error) tuple.
    Type* value_type = is_tuple
        ? expr_type->data.struct_type.fields[0].type
        : expr_type->data.error_union.value_type;

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
Type* type_check_channel_send_op(TypeChecker* checker, Type* channel_type, Type* value_type, ASTNode* value_expr, Position pos) {
    if (!checker || !channel_type || !value_type) return NULL;

    // Left operand must be a channel
    if (channel_type->kind != TYPE_CHANNEL) {
        type_error(checker, pos, "Cannot send to non-channel type %s", type_to_string(channel_type));
        return NULL;
    }

    Type* element_type = channel_type->data.channel.element_type;

    // Task 1 (chan-send representability, arc 3; const-identifier extension,
    // arc 4 item (j)): a compile-time integer constant — literal shape OR an
    // expression over cached const identifiers — sent into an integer-element
    // channel of a DIFFERENT kind (`ch <- 300`, `const k = 300; ch <- k` into
    // chan int8) used to fall straight through to the blanket type_compatible
    // check below, which treats any two integer kinds as compatible — so
    // codegen materialized the constant at its own width and the runtime
    // silently truncated on receive (300 -> 44). Gate through the shared
    // representability helper (chan_send_const_int_gate, type_checker.c —
    // see its doc comment for the case classes and the negated/bare_literal
    // reconstruction). Same-kind sends and non-constant values need no gate —
    // they flow through type_compatible below unchanged.
    int gate = chan_send_const_int_gate(checker, value_expr, value_type, element_type);
    if (gate < 0) return NULL;
    if (gate > 0) return type_checker_get_builtin(checker, TYPE_VOID);

    // Check if value type is compatible with channel element type
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

