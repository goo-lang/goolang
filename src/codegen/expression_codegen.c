#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Expression code generation

#if LLVM_AVAILABLE
// Read-modify-write for a map-index target `m[k]`: read the old value via
// goo_map_get_sv, compute old <op> rhs (or old +/- 1 for postfix, when
// rhs_or_null is NULL), and write the result back via goo_map_set_sv.
// Returns a ValueInfo wrapping either the OLD value or the NEW value
// (rvalue, not addressable) depending on return_old_value — see that
// parameter's doc below — or NULL on error (a source-located error has
// already been emitted).
//
// Mirrors the `m[k] = v` fast path (this file, TOKEN_ASSIGN's AST_INDEX_EXPR
// arm, ~line 1110) for the map/key/value plumbing, and the plain read fast
// path (composite_codegen.c's codegen_generate_index_expr TYPE_MAP arm) for
// the get+slot_to_value half. This lets `m[k]++`/`m[k] += n` reuse the same
// runtime calls and slot packing instead of resolving an address — map
// values are never addressable (see codegen_emit_lvalue_address's
// AST_INDEX_EXPR arm, which still rejects `&m[k]` / `m[k].F = v`
// unconditionally; only whole-value RMW is new here).
//
// return_old_value: which value the caller gets back. The write-back
// (goo_map_set_sv) always stores `newv` regardless of this flag — only the
// RETURNED ValueInfo differs:
//   - true  (postfix m[k]++/m[k]--): C postfix semantics return the value
//     BEFORE the op, matching the pre-existing non-map postfix path
//     (AST_POSTFIX_EXPR below, `return value_info_new(NULL, loaded, ...)`
//     where `loaded` is read before the store).
//   - false (compound-assign m[k] += n / -= / *=): matches the non-map
//     compound-assign arm's `return newval;` (codegen_generate_binary_expr),
//     which is the value AFTER the op.
// A named bool is used instead of overloading rhs_or_null==NULL for this,
// since that already carries a distinct meaning (postfix's implicit +/-1
// vs. compound-assign's explicit RHS expression) and conflating the two
// would make call sites harder to read.
static ValueInfo* codegen_map_index_rmw(CodeGenerator* codegen, TypeChecker* checker,
                                        ASTNode* index_node, TokenType base_op,
                                        ASTNode* rhs_or_null, bool return_old_value) {
    IndexExprNode* idx = (IndexExprNode*)index_node;
    Type* base_t = type_check_expression(checker, idx->expr);
    if (!base_t || base_t->kind != TYPE_MAP) return NULL;
    Type* key_type = base_t->data.map.key_type;
    Type* val_type = base_t->data.map.value_type;

    LLVMValueRef get_fn = LLVMGetNamedFunction(codegen->module, "goo_map_get_sv");
    LLVMValueRef set_fn = LLVMGetNamedFunction(codegen->module, "goo_map_set_sv");
    if (!get_fn || !set_fn) {
        codegen_error(codegen, index_node->pos, "map get/set runtime symbols missing");
        return NULL;
    }

    ValueInfo* mv = codegen_generate_expression(codegen, checker, idx->expr);
    ValueInfo* kv = codegen_generate_expression(codegen, checker, idx->index);
    if (!mv || !kv) { value_info_free(mv); value_info_free(kv); return NULL; }
    // Box a concrete key into an interface-typed map key BEFORE slot-packing
    // (Task 2) — no-op for every non-interface-keyed map.
    if (!codegen_box_map_key_if_needed(codegen, checker, kv, key_type, index_node->pos)) {
        value_info_free(mv);
        value_info_free(kv);
        return NULL;
    }
    LLVMValueRef kslot = codegen_map_key_to_slot(codegen, checker, kv, key_type);

    // old = slot_to_value(goo_map_get_sv(m, kslot)) — a missing key returns a
    // zero slot from the runtime, so old_val is V's zero value, matching the
    // plain `m[k]` read fast-path's missing-key behavior.
    LLVMValueRef gargs[2] = { mv->llvm_value, kslot };
    LLVMValueRef old_slot = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(get_fn),
                                           get_fn, gargs, 2, "rmw_get");
    LLVMValueRef old_val = codegen_map_slot_to_value(codegen, old_slot, val_type);

    // rhs value: the RHS expr (loaded to a scalar if it's itself an lvalue,
    // same auto-load every other RHS site in this file performs), or a
    // constant 1 for postfix ++/--.
    LLVMValueRef rhs;
    if (rhs_or_null) {
        ValueInfo* rv = codegen_generate_expression(codegen, checker, rhs_or_null);
        if (!rv) { value_info_free(mv); value_info_free(kv); return NULL; }
        rhs = rv->is_lvalue && rv->goo_type
              ? LLVMBuildLoad2(codegen->builder, codegen_type_to_llvm(codegen, rv->goo_type), rv->llvm_value, "rmw_rhs")
              : rv->llvm_value;
        value_info_free(rv);
    } else {
        rhs = LLVMConstInt(codegen_type_to_llvm(codegen, val_type), 1, 0);
    }

    // new = old <op> rhs. v1 admits + - * on map values (mirrors the base_op
    // switch's TOKEN_PLUS/TOKEN_MINUS/TOKEN_MULTIPLY cases used by the
    // compound-assign desugar above); anything else is an explicit error,
    // not a silent miscompile.
    LLVMValueRef newv;
    switch (base_op) {
        case TOKEN_PLUS:     newv = LLVMBuildAdd(codegen->builder, old_val, rhs, "rmw_add"); break;
        case TOKEN_MINUS:    newv = LLVMBuildSub(codegen->builder, old_val, rhs, "rmw_sub"); break;
        case TOKEN_MULTIPLY: newv = LLVMBuildMul(codegen->builder, old_val, rhs, "rmw_mul"); break;
        default:
            codegen_error(codegen, index_node->pos,
                          "unsupported compound op on map value (v1: + - *)");
            value_info_free(mv); value_info_free(kv); return NULL;
    }

    // goo_map_set_sv(m, kslot, value_to_slot(new))
    LLVMValueRef nslot = codegen_map_value_to_slot(codegen, newv, val_type);
    LLVMValueRef sargs[3] = { mv->llvm_value, kslot, nslot };
    LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(set_fn), set_fn, sargs, 3, "");

    value_info_free(mv);
    value_info_free(kv);
    return value_info_new(NULL, return_old_value ? old_val : newv, val_type);
}
#endif

ValueInfo* codegen_generate_expression(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    if (!codegen || !checker || !expr) return NULL;
    
    switch (expr->type) {
        case AST_IDENTIFIER:
            return codegen_generate_identifier(codegen, checker, expr);
        case AST_LITERAL:
            return codegen_generate_literal(codegen, checker, expr);
        case AST_BINARY_EXPR:
            return codegen_generate_binary_expr(codegen, checker, expr);
        case AST_UNARY_EXPR:
            return codegen_generate_unary_expr(codegen, checker, expr);
        case AST_CALL_EXPR:
            return codegen_generate_call_expr(codegen, checker, expr);
        case AST_INDEX_EXPR:
            return codegen_generate_index_expr(codegen, checker, expr);
        case AST_SLICE_INDEX_EXPR:
            return codegen_generate_slice_index_expr(codegen, checker, expr);
        case AST_SELECTOR_EXPR:
            return codegen_generate_selector_expr(codegen, checker, expr);
        case AST_TRY_EXPR:
            return codegen_generate_try_expr(codegen, checker, expr);
        case AST_CATCH_EXPR:
            return codegen_generate_catch_expr(codegen, checker, expr);
        case AST_PTR_ARITHMETIC:
            return codegen_generate_ptr_arithmetic(codegen, checker, expr);
        case AST_PTR_DEREF:
            return codegen_generate_ptr_deref(codegen, checker, expr);
        case AST_ADDR_OF:
            return codegen_generate_addr_of(codegen, checker, expr);
        case AST_PORT_IO:
            return codegen_generate_port_io(codegen, checker, expr);
        case AST_MMIO_ACCESS:
            return codegen_generate_mmio_access(codegen, checker, expr);
        case AST_SLICE_EXPR:
            return codegen_generate_slice_lit(codegen, checker, expr);
        case AST_ARRAY_LITERAL:
            return codegen_generate_array_lit(codegen, checker, expr);
        case AST_STRUCT_LITERAL:
            return codegen_generate_struct_lit(codegen, checker, expr);
        case AST_MATCH_EXPR:
            return codegen_generate_match(codegen, checker, expr);
        case AST_FUNC_LIT:
            return codegen_generate_func_lit(codegen, checker, expr);
        case AST_SLICE_CONVERSION: {
            // []byte(s) (Task 2, stdlib unblocker). The checker has already
            // validated conv->operand as TYPE_STRING and stamped the []byte
            // Type on expr->node_type. Go copies on conversion:
            // goo_bytes_from_string allocates a fresh buffer and memcpy's,
            // so mutating the result slice never aliases the source string
            // (see the bytesconv_probe golden's mutation-independence
            // lines).
            SliceConvNode* conv = (SliceConvNode*)expr;
            ValueInfo* src = codegen_generate_expression(codegen, checker, conv->operand);
            if (!src) return NULL;
            LLVMValueRef sval = src->llvm_value;
            if (src->is_lvalue && src->goo_type) {
                LLVMTypeRef st = codegen_type_to_llvm(codegen, src->goo_type);
                if (st) sval = LLVMBuildLoad2(codegen->builder, st, sval, "conv_load");
            }
            // String rep is {i8* ptr, i64 len} — field 0/1.
            LLVMValueRef str_ptr = LLVMBuildExtractValue(codegen->builder, sval, 0, "bytesconv_str_ptr");
            LLVMValueRef str_len = LLVMBuildExtractValue(codegen->builder, sval, 1, "bytesconv_str_len");
            value_info_free(src);

            // goo_bytes_from_string is not registered by runtime_integration.c
            // (out of this task's file allowlist) — declare it lazily here on
            // first use, the same lazy-fallback pattern call_codegen.c's
            // string(x) arm uses for goo_string_from_rune.
            LLVMValueRef from_str_fn = LLVMGetNamedFunction(codegen->module, "goo_bytes_from_string");
            if (!from_str_fn) {
                LLVMTypeRef i8_ptr = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
                LLVMTypeRef i64_l = LLVMInt64TypeInContext(codegen->context);
                LLVMTypeRef params[] = { i8_ptr, i64_l };
                LLVMTypeRef fn_type = LLVMFunctionType(i8_ptr, params, 2, 0);
                from_str_fn = LLVMAddFunction(codegen->module, "goo_bytes_from_string", fn_type);
            }
            LLVMValueRef args[] = { str_ptr, str_len };
            LLVMValueRef data_ptr = LLVMBuildCall2(codegen->builder,
                LLVMGlobalGetValueType(from_str_fn), from_str_fn, args, 2, "bytesconv_data");

            Type* slice_type = expr->node_type;
            LLVMTypeRef slice_llvm = codegen_type_to_llvm(codegen, slice_type);
            if (!slice_llvm) {
                codegen_error(codegen, expr->pos, "[]byte(s): cannot lower slice type");
                return NULL;
            }
            // Build the {ptr, len, cap=len} slice aggregate. data_ptr's LLVM
            // type is already i8* (goo_bytes_from_string's declared return),
            // matching field 0's element-pointer type for []byte exactly.
            LLVMValueRef result = LLVMGetUndef(slice_llvm);
            result = LLVMBuildInsertValue(codegen->builder, result, data_ptr, 0, "bytesconv_sl_data");
            result = LLVMBuildInsertValue(codegen->builder, result, str_len, 1, "bytesconv_sl_len");
            result = LLVMBuildInsertValue(codegen->builder, result, str_len, 2, "bytesconv_sl_cap");
            return value_info_new(NULL, result, slice_type);
        }
        // Task 2 of type assertions: single-return `d2 := a.(Dog)` /
        // `_ = a.(T)` / any other single-value use of x.(T). The checker
        // already rejected a non-interface operand, an interface target, and
        // an impossible assertion — codegen only emits the runtime
        // vtable-pointer compare (codegen_interface_assert_match, shared
        // with the comma-ok arm in function_codegen.c and Task 3's type
        // switch) and picks load-on-match vs panic-on-miss. Unlike the
        // comma-ok form there is no phi/join: the panic block is
        // unreachable, so the match block IS the continuation and this
        // function can just return its loaded value directly.
        case AST_TYPE_ASSERT: {
            TypeAssertNode* ta = (TypeAssertNode*)expr;
            Type* iface_type = ta->expr->node_type;
            Type* target = expr->node_type;
            if (!iface_type || !target) {
                codegen_error(codegen, expr->pos,
                              "internal: type assertion missing resolved types");
                return NULL;
            }

            ValueInfo* iv = codegen_generate_expression(codegen, checker, ta->expr);
            if (!iv) return NULL;
            LLVMValueRef iface_val = iv->llvm_value;
            // A selector/index operand (e.g. `s.field.(T)`) is an lvalue
            // (an address) — load the {vtable, data} struct before
            // extracting, mirroring codegen_interface_dispatch's call site
            // (call_codegen.c ~1094). An identifier operand is already a
            // loaded rvalue.
            if (iv->is_lvalue) {
                LLVMTypeRef ity = codegen_type_to_llvm(codegen, iface_type);
                if (ity) {
                    iface_val = LLVMBuildLoad2(codegen->builder, ity, iface_val, "ta.operand");
                }
            }
            value_info_free(iv);

            LLVMValueRef data = NULL;
            LLVMValueRef match = codegen_interface_assert_match(codegen, checker, iface_val,
                                                                iface_type, target, &data);
            if (!match) {
                codegen_error(codegen, expr->pos,
                              "internal: cannot build type assertion vtable compare");
                return NULL;
            }

            LLVMValueRef panic_fn = LLVMGetNamedFunction(codegen->module, "goo_panic");
            if (!panic_fn) {
                codegen_error(codegen, expr->pos, "goo_panic not found in module");
                return NULL;
            }

            LLVMBasicBlockRef match_bb = codegen_create_block(codegen, "ta.ok");
            LLVMBasicBlockRef miss_bb  = codegen_create_block(codegen, "ta.panic");
            LLVMBuildCondBr(codegen->builder, match, match_bb, miss_bb);

            // Miss: panic with the STATIC type names only — no dynamic-type
            // name (v1 deviation: RTTI would be needed to name the actual
            // held type, and is out of scope) — then terminate the block.
            codegen_set_insert_point(codegen, miss_bb);
            const char* iname = iface_type->data.interface.name
                                     ? iface_type->data.interface.name : "interface";
            const char* tname = type_receiver_name(target);
            char msg_buf[256];
            snprintf(msg_buf, sizeof(msg_buf), "interface conversion: %s is not %s",
                     iname, tname ? tname : type_to_string(target));
            LLVMValueRef msg = LLVMBuildGlobalStringPtr(codegen->builder, msg_buf, "ta_panic_msg");
            LLVMValueRef panic_args[1] = { msg };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(panic_fn), panic_fn,
                           panic_args, 1, "");
            LLVMBuildUnreachable(codegen->builder);

            // Match: recover the concrete value and continue here.
            codegen_set_insert_point(codegen, match_bb);
            LLVMValueRef loaded = codegen_interface_assert_unbox(codegen, target, data);
            if (!loaded) {
                codegen_error(codegen, expr->pos,
                              "internal: cannot lower type assertion target type");
                return NULL;
            }
            return value_info_new(NULL, loaded, target);
        }
        case AST_POSTFIX_EXPR: {
            // `j++` / `j--` / `c.n++` / `a[i]++`: load operand, compute
            // load ± 1, store back, return the LOADED (pre-modification)
            // value. Postfix semantics. The operand's storage ADDRESS is
            // resolved via codegen_emit_lvalue_address — the SAME helper
            // `x = v` / `s.field = v` / `a[i] = v` assignment uses
            // (expression_codegen.c's TOKEN_ASSIGN arm) — so identifier,
            // selector, and index operands are all handled uniformly here
            // and `a[i]++` inherits whatever GEP/bounds behavior `a[i] = x`
            // already has (same code path, not a re-walk).
            PostfixExprNode* p = (PostfixExprNode*)expr;
            ValueInfo* target;
            LLVMTypeRef elem_llvm;
            LLVMValueRef loaded;
            LLVMValueRef one;
            LLVMValueRef updated;

            // `m[k]++` / `m[k]--`: map values are never addressable (see
            // codegen_emit_lvalue_address's AST_INDEX_EXPR arm below), so
            // route to the read-modify-write helper BEFORE resolving an
            // lvalue address — mirrors the `m[k] = v` fast path's map
            // detection (type_check_expression + TYPE_MAP) in the
            // TOKEN_ASSIGN arm of codegen_generate_binary_expr.
            if (p->operand->type == AST_INDEX_EXPR) {
                IndexExprNode* pidx = (IndexExprNode*)p->operand;
                Type* pbase_t = type_check_expression(checker, pidx->expr);
                if (pbase_t && pbase_t->kind == TYPE_MAP) {
                    // Postfix returns the PRE-increment (old) value — same
                    // rule as the addressable-lvalue postfix path just below
                    // (`loaded` is captured before the store). Without
                    // return_old_value=true here, `m[k]++` would silently
                    // diverge from `i++`'s semantics depending on whether
                    // the operand is a map index or a plain variable.
                    return codegen_map_index_rmw(codegen, checker, p->operand,
                                                 p->operator == TOKEN_INCREMENT ? TOKEN_PLUS : TOKEN_MINUS,
                                                 NULL, /*return_old_value=*/true);
                }
            }

            target = codegen_emit_lvalue_address(codegen, checker, p->operand);
            if (!target || !target->is_lvalue) {
                codegen_error(codegen, expr->pos,
                              "postfix ++/-- operand must be an addressable lvalue "
                              "(identifier, field, or index)");
                return NULL;
            }
            elem_llvm = target->goo_type
                ? codegen_type_to_llvm(codegen, target->goo_type)
                : LLVMInt32TypeInContext(codegen->context);
            loaded = LLVMBuildLoad2(codegen->builder, elem_llvm, target->llvm_value, "postfix_load");
            one = LLVMConstInt(elem_llvm, 1, 0);
            if (p->operator == TOKEN_INCREMENT) {
                updated = LLVMBuildAdd(codegen->builder, loaded, one, "postfix_inc");
            } else {
                updated = LLVMBuildSub(codegen->builder, loaded, one, "postfix_dec");
            }
            LLVMBuildStore(codegen->builder, updated, target->llvm_value);
            return value_info_new(NULL, loaded, target->goo_type);
        }
        case AST_PAREN_EXPR: {
            // MapLitNode — `map[K]V{ … }`. Lowers to:
            //   m = goo_map_new_sv(key_kind)
            //   for each (k,v): goo_map_set_sv(m, key_slot(k), slot(v))
            // where slot(v)/key_slot(k) pack the declared V/K into the
            // 8-byte i64 slot (codegen_map_value_to_slot /
            // codegen_map_key_to_slot — the latter never boxes strings).
            // Returns the GooMapSV* as a raw ptr-typed value.
            MapLitNode* lit = (MapLitNode*)expr;
            Type* key_type = (expr->node_type && expr->node_type->kind == TYPE_MAP)
                ? expr->node_type->data.map.key_type : NULL;
            Type* val_type = (expr->node_type && expr->node_type->kind == TYPE_MAP)
                ? expr->node_type->data.map.value_type : NULL;
            if (!val_type) {
                codegen_error(codegen, expr->pos, "map literal missing resolved value type");
                return NULL;
            }
            LLVMValueRef new_fn = LLVMGetNamedFunction(codegen->module, "goo_map_new_sv");
            LLVMValueRef set_fn = LLVMGetNamedFunction(codegen->module, "goo_map_set_sv");
            if (!new_fn || !set_fn) {
                codegen_error(codegen, expr->pos, "map runtime symbols missing");
                return NULL;
            }
            LLVMValueRef key_kind = LLVMConstInt(LLVMInt32TypeInContext(codegen->context),
                                                 codegen_map_key_kind(key_type), 0);
            // A struct key needs the synthesized per-field comparator so the
            // runtime's key_kind==STRUCT arm can call it (goo_map_key_eq,
            // runtime.c); an interface key needs the runtime's
            // goo_iface_key_eq (Task 2, key_kind==IFACE); every other key
            // kind passes NULL (unused by the runtime for STRING/INLINE).
            // Mirrors the make(map[K]V) site (call_codegen.c).
            LLVMValueRef keyeq_ptr;
            LLVMTypeRef keyeq_ptr_ty = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
            if (key_type && key_type->kind == TYPE_STRUCT) {
                LLVMValueRef cmp_fn = codegen_get_or_emit_struct_key_eq(codegen, checker, key_type);
                if (!cmp_fn) {
                    codegen_error(codegen, expr->pos,
                                  "map literal: failed to synthesize struct key comparator");
                    return NULL;
                }
                keyeq_ptr = LLVMBuildBitCast(codegen->builder, cmp_fn, keyeq_ptr_ty, "keyeq_ptr");
            } else if (key_type && key_type->kind == TYPE_INTERFACE) {
                LLVMValueRef cmp_fn = LLVMGetNamedFunction(codegen->module, "goo_iface_key_eq");
                if (!cmp_fn) {
                    codegen_error(codegen, expr->pos, "map literal: goo_iface_key_eq unavailable");
                    return NULL;
                }
                keyeq_ptr = LLVMBuildBitCast(codegen->builder, cmp_fn, keyeq_ptr_ty, "keyeq_ptr");
            } else {
                keyeq_ptr = LLVMConstPointerNull(keyeq_ptr_ty);
            }
            LLVMValueRef new_args[] = { key_kind, keyeq_ptr };
            LLVMValueRef m = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(new_fn),
                                            new_fn, new_args, 2, "map_new");
            ASTNode* k = lit->keys;
            ASTNode* v = lit->values;
            while (k && v) {
                ValueInfo* kv = codegen_generate_expression(codegen, checker, k);
                ValueInfo* vv = codegen_generate_expression(codegen, checker, v);
                if (!kv || !vv) return NULL;
                // Box a concrete key into the map's interface-typed key slot
                // BEFORE slot-packing (Task 2) — mirrors the value-boxing
                // step just below for the value half. No-op for every
                // non-interface-keyed map (codegen_box_map_key_if_needed).
                if (!codegen_box_map_key_if_needed(codegen, checker, kv, key_type, expr->pos)) {
                    value_info_free(kv);
                    value_info_free(vv);
                    return NULL;
                }
                LLVMValueRef kp = codegen_map_key_to_slot(codegen, checker, kv, key_type);
                // Box a concrete implementer into the map's interface-typed
                // value slot BEFORE slot-boxing — mirrors the plain-assignment
                // helper (function_codegen.c's var-decl init boxing / the
                // TOKEN_ASSIGN arm below) via the same codegen_interface_box.
                // Without this, codegen_coerce_to_type leaves an aggregate
                // unchanged (it only handles int/float widths), so the raw
                // concrete struct bits would be written into the slot's box
                // instead of a real {vtable,data} pair — reviewer finding I1.
                // interface→interface (already-boxed RHS, reviewer probe p13)
                // needs no re-box: same layout, falls through unchanged.
                if (val_type->kind == TYPE_INTERFACE &&
                    vv->goo_type && vv->goo_type->kind != TYPE_INTERFACE) {
                    LLVMValueRef boxed = codegen_interface_box(codegen, checker,
                                                               val_type,
                                                               vv->goo_type,
                                                               vv->llvm_value);
                    if (!boxed) {
                        codegen_error(codegen, expr->pos,
                                      "failed to box map literal value into interface");
                        value_info_free(kv);
                        value_info_free(vv);
                        return NULL;
                    }
                    vv->llvm_value = boxed;
                    vv->goo_type = val_type;
                }
                LLVMTypeRef want_vt = codegen_type_to_llvm(codegen, val_type);
                if (want_vt && !codegen_map_value_is_inline(val_type)) {
                    int src_signed = vv->goo_type &&
                        vv->goo_type->kind >= TYPE_INT8 &&
                        vv->goo_type->kind <= TYPE_INT64;
                    vv->llvm_value = codegen_coerce_to_type(
                        codegen, vv->llvm_value, src_signed, want_vt);
                }
                LLVMValueRef slot = codegen_map_value_to_slot(codegen, vv->llvm_value, val_type);
                LLVMValueRef args[3] = { m, kp, slot };
                LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(set_fn),
                              set_fn, args, 3, "");
                value_info_free(kv);
                value_info_free(vv);
                k = k->next;
                v = v->next;
            }
            return value_info_new(NULL, m, expr->node_type);
        }
        default:
            codegen_error(codegen, expr->pos, "Unknown expression type for code generation");
            return NULL;
    }
}

// Resolve a bare identifier to its module-level (non-local) function global,
// if any. Inside a package (current_package set) a package-local function is
// emitted under its mangled symbol goo_pkg__<pkg>__<name>, so an
// intra-package reference must resolve the mangled symbol FIRST; the bare
// name is the fallback for the main package and for runtime/shim symbols.
// (Without this, any package whose functions call each other — every real
// stdlib leaf — fails codegen with "Undefined identifier".)
//
// Shared by codegen_generate_identifier's value-position fallback below
// (which wraps a TYPE_FUNCTION result into the universal fat-pointer VALUE)
// and codegen_resolve_callee's direct-call bypass (call_codegen.c), which
// need the identical lookup but return it BARE, unwrapped.
LLVMValueRef codegen_lookup_global_function(CodeGenerator* codegen, TypeChecker* checker,
                                            const char* name) {
    LLVMValueRef func_val = NULL;
    char* pkg_sym = codegen_package_symbol_name(checker, name);
    if (pkg_sym) {
        func_val = LLVMGetNamedFunction(codegen->module, pkg_sym);
        free(pkg_sym);
    }
    if (!func_val) {
        func_val = LLVMGetNamedFunction(codegen->module, name);
    }
    return func_val;
}

ValueInfo* codegen_generate_identifier(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_IDENTIFIER) return NULL;

    IdentifierNode* ident = (IdentifierNode*)expr;

    // Look up the identifier in the symbol table
    ValueInfo* value_info = codegen_lookup_value(codegen, ident->name);
    if (!value_info) {
        // Fall back to module-level functions. User-defined functions
        // (`func add(...)`) are registered via LLVMAddFunction during
        // codegen_generate_function_decl but never make it into the
        // codegen value table, so any `add(2,3)` call would fail here.
        LLVMValueRef func_val = codegen_lookup_global_function(codegen, checker, ident->name);
        if (func_val) {
            Variable* func_var = type_checker_lookup_variable(checker, ident->name);
            Type* func_type = func_var ? func_var->type : NULL;

            // A bare named-function reference reached HERE is always a VALUE
            // use, never a direct-call callee: codegen_generate_call_expr's
            // generic call path and codegen_generate_go_stmt's callee
            // resolution both call codegen_resolve_callee FIRST
            // (call_codegen.c) and bypass this identifier arm entirely for
            // an unshadowed bare-identifier callee — see that function's
            // comment for why. Every function-typed VALUE is the universal
            // fat pointer `{ fn_ptr, env_ptr }` (env FIRST — a change-
            // together contract Branch B's closures build on unseen; see
            // docs/superpowers/specs/2026-07-03-closures-design.md
            // "Representation"). A named function captures nothing, so
            // get-or-create its thunk (mirrors PR #30's goroutine thunk
            // conventions: per-symbol, get-or-create, cached by name) and
            // wrap it as `{ thunk, NULL }`.
            if (func_type && func_type->kind == TYPE_FUNCTION) {
                LLVMValueRef thunk = codegen_get_func_thunk(codegen, checker, func_type,
                                                            func_val, ident->name);
                if (!thunk) {
                    codegen_error(codegen, expr->pos,
                                  "internal: failed to build value-thunk for '%s'",
                                  ident->name);
                    return NULL;
                }
                LLVMTypeRef pair_ty = codegen_get_funcval_pair_type(codegen);
                LLVMValueRef pair = LLVMConstNull(pair_ty);
                pair = LLVMBuildInsertValue(codegen->builder, pair, thunk, 0, "funcval");
                return value_info_new(ident->name, pair, func_type);
            }
            return value_info_new(ident->name, func_val, func_type);
        }
        codegen_error(codegen, expr->pos, "Undefined identifier '%s'", ident->name);
        return NULL;
    }
    
    // If it's an lvalue (variable), load the value. With LLVM 22 opaque
    // pointers, LLVMTypeOf(alloca) returns just `ptr`, not `ptr i32` —
    // we have to supply the load type from goo_type. Previously every
    // var read produced `load ptr, ptr %x` which fed `ptr` values into
    // `icmp sgt`/`add`, failing module verification.
    // A global const (codegen_const_decl creates one via LLVMAddGlobal) lives
    // in the value table as is_lvalue=0 but its LLVM value is the address, not
    // the contents. Reading it as an expression must load through that address
    // — otherwise `os.Exit(X)` passes `ptr @X` to a function expecting `i32`
    // and module verification rejects it (M9-const-ref-load).
    // A reference to a CONSTANT global (a folded const like `const two32 =
    // 1<<32`) resolves to its constant initializer, not a runtime load. This
    // keeps const-folding transitive: `const mask32 = two32 - 1` then lowers to
    // `sub <const>, 1`, which LLVM folds, so it passes the compile-time-constant
    // check for a subsequent const. (Loads are never constant expressions.)
    if (!value_info->is_lvalue && LLVMIsAGlobalVariable(value_info->llvm_value)
        && LLVMIsGlobalConstant(value_info->llvm_value)) {
        LLVMValueRef init = LLVMGetInitializer(value_info->llvm_value);
        if (init && LLVMIsConstant(init)) {
            return value_info_new(ident->name, init, value_info->goo_type);
        }
    }

    bool needs_load = value_info->is_lvalue
        || (LLVMIsAGlobalVariable(value_info->llvm_value) && value_info->goo_type);
    if (needs_load) {
        LLVMTypeRef load_type = value_info->goo_type
            ? codegen_type_to_llvm(codegen, value_info->goo_type)
            : LLVMTypeOf(value_info->llvm_value);
        LLVMValueRef loaded_value = LLVMBuildLoad2(codegen->builder, load_type, value_info->llvm_value, ident->name);
        return value_info_new(ident->name, loaded_value, value_info->goo_type);
    } else {
        // Direct value (function pointer, inlined literal, etc.)
        return value_info_new(ident->name, value_info->llvm_value, value_info->goo_type);
    }
#endif
}

#if LLVM_AVAILABLE
LLVMValueRef codegen_const_string_value(CodeGenerator* codegen, const char* bytes, size_t len) {
    // Emit the bytes as a private global constant array, then build a constant
    // { i8* data, i64 len } struct. Builder-free (no LLVMBuild*), so it is valid
    // at global scope where there is no basic block. The explicit length keeps
    // embedded NULs (e.g. the math/bits tables that start with "\x00").
    LLVMValueRef arr = LLVMConstStringInContext(codegen->context, bytes,
                                                (unsigned)len, /*DontNullTerminate=*/0);
    LLVMTypeRef arr_type = LLVMTypeOf(arr); // [len+1 x i8]
    LLVMValueRef global = LLVMAddGlobal(codegen->module, arr_type, "str");
    LLVMSetInitializer(global, arr);
    LLVMSetGlobalConstant(global, 1);
    LLVMSetLinkage(global, LLVMPrivateLinkage);
    LLVMSetUnnamedAddr(global, 1);

    LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0);
    LLVMValueRef idx[2] = { zero, zero };
    LLVMValueRef data_ptr = LLVMConstInBoundsGEP2(arr_type, global, idx, 2);

    LLVMValueRef len_val = LLVMConstInt(LLVMInt64TypeInContext(codegen->context), len, 0);
    LLVMValueRef fields[2] = { data_ptr, len_val };
    return LLVMConstStructInContext(codegen->context, fields, 2, /*packed=*/0);
}
#endif

ValueInfo* codegen_generate_literal(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_LITERAL) return NULL;
    
    LiteralNode* literal = (LiteralNode*)expr;
    LLVMValueRef llvm_value = NULL;
    Type* goo_type = NULL;
    
    switch (literal->literal_type) {
        case TOKEN_INT: {
            // strtoull with base 0 auto-detects the prefix (0x hex, 0o octal,
            // 0b binary, else decimal) and parses the FULL unsigned 64-bit range
            // — atoll couldn't do either (hex parsed as 0, and a value above
            // INT64_MAX clamped). Integer literals are non-negative (a leading
            // `-` is a separate unary op), so unsigned parsing is exact.
            unsigned long long value = strtoull(literal->value, NULL, 0);
            Type* nt = expr->node_type;
            // Cross-kind float adaptation (expression_checker.c's cross-kind
            // block in type_check_binary_expr): the `1` in `1 < g` (g
            // float32) is an untyped-int-literal-rooted operand that met a
            // float-kind operand, so the checker stamped this literal's
            // node_type FLOAT32/FLOAT64 instead of leaving it INT64. Emit a
            // float constant directly here (mirrors the TOKEN_FLOAT arm
            // below) rather than an int constant that codegen_generate_
            // binary_expr would later feed into an `fcmp`/`fadd` alongside a
            // real float value — that width/kind mismatch is exactly what
            // made the LLVM verifier abort with "Both operands to ICmp
            // instruction are not of the same type! icmp slt i64, float"
            // before this stamping existed.
            // `value` is parsed by strtoull as unsigned, so it is always
            // non-negative here — a negative literal like `-1` never reaches
            // this arm as a negative number; it arrives as a unary MINUS
            // node wrapping this literal (see is_untyped_int_rooted's
            // AST_UNARY_EXPR case), with the sign applied by the unary op's
            // own codegen above this literal. So casting the raw unsigned
            // `value` straight to double is exact for the full range of a
            // representable integer literal.
            if (nt && type_is_float(nt)) {
                LLVMTypeRef lt = codegen_type_to_llvm(codegen, nt);
                if (lt) {
                    llvm_value = LLVMConstReal(lt, (double)value);
                    goo_type = nt;
                    break;
                }
            }
            // Narrow integer-literal adaptation: when type-checking retyped this
            // literal to a specific integer type OTHER than the default int32
            // (e.g. a uint64 parameter/operand/return context), emit the
            // constant at THAT width and signedness so it matches with no later
            // coercion. int32-typed literals keep the magnitude-based path below
            // (which auto-promotes to i64 past INT32_MAX — an unadapted untyped
            // constant must preserve its full magnitude, e.g. 9000000000).
            if (nt && type_is_integer(nt) && nt->kind != TYPE_INT64) {
                LLVMTypeRef lt = codegen_type_to_llvm(codegen, nt);
                if (lt) {
                    llvm_value = LLVMConstInt(lt, value, type_is_signed(nt));
                    goo_type = nt;
                    break;
                }
            }
            // Default: an untyped integer constant is `int` (int64 here), which
            // holds the full magnitude of any literal.
            llvm_value = LLVMConstInt(LLVMInt64TypeInContext(codegen->context),
                                     value, 1);
            goo_type = type_checker_get_builtin(checker, TYPE_INT64);
            break;
        }
        
        case TOKEN_FLOAT: {
            // Parse float value from string
            double value = atof(literal->value);
            // Narrow float-literal adaptation (mirrors the TOKEN_INT arm
            // above): when type-checking retyped this literal to float32
            // (e.g. `g * 2.0` with g float32 — see
            // is_untyped_float_rooted/adapt_untyped_float_operand in
            // expression_checker.c), emit the constant at THAT width so
            // codegen's computed type matches what the checker stamped.
            // Unadapted literals default to float64 (Go's untyped-float
            // default type), the path below.
            Type* nt = expr->node_type;
            if (nt && nt->kind == TYPE_FLOAT32) {
                llvm_value = LLVMConstReal(LLVMFloatTypeInContext(codegen->context), value);
                goo_type = nt;
                break;
            }
            llvm_value = LLVMConstReal(LLVMDoubleTypeInContext(codegen->context), value);
            goo_type = type_checker_get_builtin(checker, TYPE_FLOAT64);
            break;
        }
        
        case TOKEN_STRING: {
            // Builder-free constant { i8* data, i64 len } (see
            // codegen_const_string_value). Works at global scope — the old
            // LLVMBuildGlobalStringPtr path dereferenced a NULL insert point
            // there and crashed. literal->length preserves embedded NULs.
            llvm_value = codegen_const_string_value(codegen, literal->value, literal->length);
            goo_type = type_checker_get_builtin(checker, TYPE_STRING);
            break;
        }
        
        case TOKEN_TRUE:
        case TOKEN_FALSE: {
            int bool_val = (literal->literal_type == TOKEN_TRUE) ? 1 : 0;
            llvm_value = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), bool_val, 0);
            goo_type = type_checker_get_builtin(checker, TYPE_BOOL);
            break;
        }
        
        case TOKEN_CHAR: {
            // Parse character value (for now, just take first character)
            char char_val = literal->value[0];
            llvm_value = LLVMConstInt(LLVMInt8TypeInContext(codegen->context), char_val, 0);
            goo_type = type_checker_get_builtin(checker, TYPE_CHAR);
            break;
        }

        case TOKEN_NIL:
            // nil literal: the var-decl initialiser path intercepts this before
            // reaching here when the declared type is ?T (see function_codegen.c).
            // For any other context fall back to a generic null i8* so the module
            // stays well-formed.
            return codegen_generate_null_literal(codegen, checker, NULL);

        default:
            codegen_error(codegen, expr->pos, "Unknown literal type");
            return NULL;
    }
    
    if (!llvm_value || !goo_type) {
        codegen_error(codegen, expr->pos, "Failed to generate literal");
        return NULL;
    }
    
    return value_info_new(NULL, llvm_value, goo_type);
#endif
}

#if LLVM_AVAILABLE
// GEP field `selector` out of an already-resolved struct address
// `struct_addr` (struct type `st`). Shared by the plain selector arm below
// and the map[K]*P auto-deref special-case (legal Go: m[k].F = v when the
// map's VALUE type is a POINTER -- no map-value ADDRESS is needed, unlike a
// struct-VALUE map, which stays rejected).
static ValueInfo* codegen_emit_struct_field_lvalue(CodeGenerator* codegen, Type* st,
                                                   LLVMValueRef struct_addr,
                                                   const char* selector) {
    int field_index = -1;
    StructField* field = NULL;
    for (size_t i = 0; i < st->data.struct_type.field_count; i++) {
        if (strcmp(st->data.struct_type.fields[i].name, selector) == 0) {
            field_index = (int)i;
            field = &st->data.struct_type.fields[i];
            break;
        }
    }
    if (field_index == -1) return NULL;

    LLVMValueRef indices[] = {
        LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0),
        LLVMConstInt(LLVMInt32TypeInContext(codegen->context), field_index, 0)
    };
    LLVMValueRef field_ptr = LLVMBuildGEP2(codegen->builder,
                                           codegen_type_to_llvm(codegen, st),
                                           struct_addr, indices, 2, selector);
    ValueInfo* out = value_info_new(selector, field_ptr, field->type);
    out->is_lvalue = 1;
    return out;
}

// GEP element `idx64` out of an already-resolved slice HEADER VALUE (not an
// address) -- the data pointer is field 0, bounds-checked against the
// header's own length (field 1). Shared by the plain slice arm below (header
// loaded from an addressable slice variable) and the map[K][]V special-case
// (header read as an RVALUE via the map-get fast path -- there is no
// map-value address, but the header's data pointer aliases the shared
// backing array, so writing through it is legal Go without one).
static ValueInfo* codegen_emit_slice_elem_lvalue(CodeGenerator* codegen, Type* slice_type,
                                                 LLVMValueRef slice_header_val,
                                                 LLVMValueRef idx64, ASTNode* expr) {
    Type* elem_type = slice_type->data.slice.element_type;
    LLVMValueRef data_ptr = LLVMBuildExtractValue(codegen->builder, slice_header_val, 0, "slice_ptr");
    LLVMValueRef slice_len = LLVMBuildExtractValue(codegen->builder, slice_header_val, 1, "slice_len");
    codegen_emit_bounds_check(codegen, idx64, slice_len, expr);
    LLVMValueRef elem_ptr = LLVMBuildGEP2(codegen->builder,
                                          codegen_type_to_llvm(codegen, elem_type),
                                          data_ptr, &idx64, 1, "slice_elem");
    ValueInfo* out = value_info_new(NULL, elem_ptr, elem_type);
    out->is_lvalue = 1;
    return out;
}

// Compute the ADDRESS of an assignable lvalue without loading its value.
// codegen_generate_expression auto-loads identifiers (and a selector spills a
// loaded struct base to a throwaway temp), which is correct for reads but loses
// stores. This helper keeps everything as addresses so `x = v`, `s.field = v`,
// and `a[i] = v` write to the real storage. Returns a ValueInfo whose
// llvm_value is a pointer to the storage (is_lvalue == 1), or NULL if the
// expression is not an addressable lvalue.
// Resolve the storage address of an addressable expression (identifier,
// selector, index, deref) without loading it. Shared with call_codegen.c so a
// pointer-receiver method call can auto-take the receiver's address (P2-3).
ValueInfo* codegen_emit_lvalue_address(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    if (!expr) return NULL;

    if (expr->type == AST_IDENTIFIER) {
        IdentifierNode* ident = (IdentifierNode*)expr;
        ValueInfo* v = codegen_lookup_value(codegen, ident->name);
        if (!v || !v->is_lvalue) return NULL;
        return v; // the variable's alloca
    }

    if (expr->type == AST_SELECTOR_EXPR) {
        SelectorExprNode* sel = (SelectorExprNode*)expr;

        // Legal Go: m[k].F = v when the map's VALUE type is a POINTER (to a
        // struct) -- Go auto-derefs through the pointer, so no map-value
        // ADDRESS is needed (maps don't have one; that's exactly what makes
        // a struct-VALUE map illegal below). Detected by: sel->expr (`m[k]`)
        // is itself a map-index expression (its base's type is TYPE_MAP) and
        // its OWN type (the map's value type, stamped on the node by
        // type_check_index_expr) is TYPE_POINTER->TYPE_STRUCT. Read `m[k]`
        // as an RVALUE (the map-get fast path returns the pointer directly --
        // pointer map values ride the inline i64 slot, see
        // codegen_map_value_is_inline), then GEP the field through the
        // pointee struct.
        if (sel->expr && sel->expr->type == AST_INDEX_EXPR) {
            IndexExprNode* inner = (IndexExprNode*)sel->expr;
            if (inner->expr && inner->expr->node_type &&
                inner->expr->node_type->kind == TYPE_MAP) {
                Type* map_val_type = sel->expr->node_type;
                if (map_val_type && map_val_type->kind == TYPE_POINTER &&
                    map_val_type->data.pointer.pointee_type &&
                    map_val_type->data.pointer.pointee_type->kind == TYPE_STRUCT) {
                    ValueInfo* ptr_val = codegen_generate_expression(codegen, checker, sel->expr);
                    if (!ptr_val) return NULL;
                    return codegen_emit_struct_field_lvalue(
                        codegen, map_val_type->data.pointer.pointee_type,
                        ptr_val->llvm_value, sel->selector);
                }
                // Struct-VALUE map (map[K]P, non-pointer): falls through to
                // the general path below, which recurses into
                // codegen_emit_lvalue_address(sel->expr) -> the AST_INDEX_EXPR
                // arm's map-base guard -> rejected. Go: map values are not
                // addressable, so a plain struct value can't be field-written
                // through a map index.
            }
        }

        ValueInfo* base = codegen_emit_lvalue_address(codegen, checker, sel->expr);
        if (!base) return NULL;

        // Resolve the struct's address. For a pointer-to-struct variable the
        // base alloca holds the pointer, so load it to reach the struct.
        Type* st = base->goo_type;
        LLVMValueRef struct_addr = base->llvm_value;
        if (st && st->kind == TYPE_POINTER && st->data.pointer.pointee_type &&
            st->data.pointer.pointee_type->kind == TYPE_STRUCT) {
            struct_addr = LLVMBuildLoad2(codegen->builder,
                                         LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),
                                         base->llvm_value, "struct_ptr");
            st = st->data.pointer.pointee_type;
        }
        if (!st || st->kind != TYPE_STRUCT) return NULL;

        return codegen_emit_struct_field_lvalue(codegen, st, struct_addr, sel->selector);
    }

    if (expr->type == AST_INDEX_EXPR) {
        IndexExprNode* ix = (IndexExprNode*)expr;

        // Legal Go: m[k][i] = v when the map's VALUE type is a SLICE. The
        // slice header aliases its backing array, so writing through the
        // header's data pointer mutates the shared storage -- no map-value
        // ADDRESS is needed (maps don't have one; a two-step `s := m[k];
        // s[i] = v` already relies on exactly this aliasing and works today).
        // Detected by: ix->expr (`m[k]`) is itself a map-index expression
        // (its base's type is TYPE_MAP) and its OWN type (the map's value
        // type, stamped on the node by type_check_index_expr) is TYPE_SLICE.
        // Read `m[k]` as an RVALUE (the map-get fast path in
        // composite_codegen.c), which yields the header {ptr,len,cap} by
        // value, and GEP+bounds-check off of THAT instead of an address.
        if (ix->expr && ix->expr->type == AST_INDEX_EXPR) {
            IndexExprNode* inner = (IndexExprNode*)ix->expr;
            if (inner->expr && inner->expr->node_type &&
                inner->expr->node_type->kind == TYPE_MAP) {
                Type* map_val_type = ix->expr->node_type;
                if (map_val_type && map_val_type->kind == TYPE_SLICE) {
                    ValueInfo* index_val = codegen_generate_expression(codegen, checker, ix->index);
                    if (!index_val) return NULL;
                    if (index_val->is_lvalue && index_val->goo_type) {
                        LLVMTypeRef it = codegen_type_to_llvm(codegen, index_val->goo_type);
                        if (it) {
                            index_val->llvm_value = LLVMBuildLoad2(codegen->builder, it, index_val->llvm_value, "idx");
                            index_val->is_lvalue = 0;
                        }
                    }
                    LLVMValueRef idx64 = codegen_widen_index(codegen, index_val);

                    ValueInfo* slice_val = codegen_generate_expression(codegen, checker, ix->expr);
                    if (!slice_val) return NULL;
                    return codegen_emit_slice_elem_lvalue(codegen, map_val_type,
                                                          slice_val->llvm_value, idx64, expr);
                }
                // Array-VALUE map (map[K][N]T, non-slice): falls through to
                // the general path below, which recurses into
                // codegen_emit_lvalue_address(ix->expr) -> the map-base guard
                // right below -> rejected. Go: an array map value has no
                // aliasing like a slice header, so it is not addressable
                // through the map.
            }
        }

        // Go semantics: a map index is never an lvalue address. Direct
        // `m[k] = v` is intercepted earlier (assignment fast path, above);
        // any request that reaches HERE is a partial write through a map
        // value (m[k].F = v, m[k][i] = v) or an address-of that slipped past
        // typecheck — all illegal. Fire ONLY on a map base — node_type is
        // stamped on ix->expr by typecheck (which always runs before
        // codegen), so this doesn't disturb the array/slice GEP paths below.
        if (ix->expr && ix->expr->node_type && ix->expr->node_type->kind == TYPE_MAP) {
            codegen_error(codegen, expr->pos,
                          "cannot assign through a map value (map values are "
                          "not addressable; assign the whole value: m[k] = v)");
            return NULL;
        }

        ValueInfo* base = codegen_emit_lvalue_address(codegen, checker, ix->expr);
        if (!base) return NULL;
        Type* base_type = base->goo_type;
        if (!base_type) return NULL;

        ValueInfo* index_val = codegen_generate_expression(codegen, checker, ix->index);
        if (!index_val) return NULL;
        if (index_val->is_lvalue && index_val->goo_type) {
            LLVMTypeRef it = codegen_type_to_llvm(codegen, index_val->goo_type);
            if (it) {
                index_val->llvm_value = LLVMBuildLoad2(codegen->builder, it, index_val->llvm_value, "idx");
                index_val->is_lvalue = 0;
            }
        }
        // Signed-correct 64-bit offset so a uint8 index (255) does not
        // sign-extend to -1 on the write path (matches the read path).
        LLVMValueRef idx64 = codegen_widen_index(codegen, index_val);

        if (base_type->kind == TYPE_ARRAY) {
            // base->llvm_value is a pointer to the array; GEP the element.
            // Bounds-check against the fixed length (static N) first — mirrors
            // the slice-write arm; arr[i]=x aborts on out-of-range.
            LLVMValueRef arr_len = LLVMConstInt(LLVMInt64TypeInContext(codegen->context),
                                                (unsigned long long)base_type->data.array.length, 0);
            codegen_emit_bounds_check(codegen, idx64, arr_len, expr);
            LLVMValueRef indices[] = {
                LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0),
                idx64
            };
            LLVMValueRef elem_ptr = LLVMBuildGEP2(codegen->builder,
                                                  codegen_type_to_llvm(codegen, base_type),
                                                  base->llvm_value, indices, 2, "array_elem");
            ValueInfo* out = value_info_new(NULL, elem_ptr, base_type->data.array.element_type);
            out->is_lvalue = 1;
            return out;
        }

        if (base_type->kind == TYPE_SLICE) {
            // base->llvm_value points to the slice struct { ptr, len, cap };
            // load it and let the shared helper extract the data pointer,
            // bounds-check against the length, and GEP the element — mirrors
            // the slice-READ arm in composite_codegen.c so s[i]=x aborts on
            // out-of-range instead of writing past the buffer.
            LLVMValueRef slice_val = LLVMBuildLoad2(codegen->builder,
                                                    codegen_type_to_llvm(codegen, base_type),
                                                    base->llvm_value, "slice_load");
            return codegen_emit_slice_elem_lvalue(codegen, base_type, slice_val, idx64, expr);
        }

        return NULL; // maps / pointers: not an addressable element lvalue here
    }

    if (expr->type == AST_UNARY_EXPR) {
        // Deref lvalue: `*p = v`. The store address is the pointer value
        // itself, so generate the operand (loading p to its pointer value) and
        // return that as the address. Element type is the pointee.
        UnaryExprNode* un = (UnaryExprNode*)expr;
        if (un->operator == TOKEN_MULTIPLY) {
            ValueInfo* ptr = codegen_generate_expression(codegen, checker, un->operand);
            if (!ptr || !ptr->goo_type || ptr->goo_type->kind != TYPE_POINTER) return NULL;
            ValueInfo* out = value_info_new(NULL, ptr->llvm_value,
                                            ptr->goo_type->data.pointer.pointee_type);
            out->is_lvalue = 1;
            return out;
        }
        return NULL;
    }

    return NULL;
}
#endif

// Widen (sign-extend) or truncate an integer value to a target LLVM type.
// Signed: Goo int literals are signed, so widening sign-extends.
static LLVMValueRef int_widen_or_trunc(CodeGenerator* codegen, LLVMValueRef v,
                                       LLVMTypeRef to_ty,
                                       unsigned from_bits, unsigned to_bits) {
    if (from_bits < to_bits)
        return LLVMBuildSExt(codegen->builder, v, to_ty, "litsext");
    if (from_bits > to_bits)
        return LLVMBuildTrunc(codegen->builder, v, to_ty, "littrunc");
    return v;
}

// Returns non-zero if `node` is a compile-time integer constant: either a
// bare AST_LITERAL or a unary minus applied to an AST_LITERAL (i.e. `-1`).
// The parser represents `-1` as AST_UNARY_EXPR(TOKEN_MINUS, AST_LITERAL),
// so both forms must be recognised as "untyped literal" for coercion.
static int is_int_literal_node(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_LITERAL) return 1;
    if (node->type == AST_UNARY_EXPR) {
        UnaryExprNode* u = (UnaryExprNode*)node;
        if (u->operator == TOKEN_MINUS && u->operand &&
            u->operand->type == AST_LITERAL)
            return 1;
    }
    return 0;
}

// If exactly one operand is an integer LITERAL and the operand widths differ,
// coerce the literal to the other operand's integer type. Go-faithful: only
// untyped literals adapt; two mismatched typed variables are left alone (that
// mismatch is a type error surfaced earlier, not silently coerced here).
static void coerce_int_literal_operand(CodeGenerator* codegen,
                                       ASTNode* left_ast, ValueInfo* left,
                                       ASTNode* right_ast, ValueInfo* right) {
    if (!left || !right || !left->goo_type || !right->goo_type) return;
    if (!type_is_integer(left->goo_type) || !type_is_integer(right->goo_type))
        return;
    LLVMTypeRef lt = LLVMTypeOf(left->llvm_value);
    LLVMTypeRef rt = LLVMTypeOf(right->llvm_value);
    if (LLVMGetTypeKind(lt) != LLVMIntegerTypeKind ||
        LLVMGetTypeKind(rt) != LLVMIntegerTypeKind) return;
    unsigned lw = LLVMGetIntTypeWidth(lt);
    unsigned rw = LLVMGetIntTypeWidth(rt);
    if (lw == rw) return;

    int left_lit  = is_int_literal_node(left_ast);
    int right_lit = is_int_literal_node(right_ast);
    if (left_lit == right_lit) return;  // neither, or both — leave alone

    if (left_lit) {
        left->llvm_value = int_widen_or_trunc(codegen, left->llvm_value, rt, lw, rw);
        left->goo_type   = right->goo_type;
    } else {
        right->llvm_value = int_widen_or_trunc(codegen, right->llvm_value, lt, rw, lw);
        right->goo_type   = left->goo_type;
    }
}

// Codegen-local duplicate of expression_checker.c's static
// is_untyped_int_rooted called with for_float_context=1 (checker-internal,
// not exported via a header edit per the task-2 brief — change them
// together). Is `node` an untyped-integer-constant-rooted AST operand,
// judged for a FLOAT adaptation target: a bare int literal, a unary -/+/^
// through to one, or a {+,-,*} binop where BOTH sides are (recursively)
// such? Needed by is_float_literal_node's binop leg below (task 2): an
// int-rooted operand mixed with a float-rooted sibling in +,-,*,/ is part
// of a float-kind constant expression as a whole (Go's kind-promotion
// rule), so it counts as "untyped" for the width-coercion backstop the same
// way a bare int literal already does. Two shapes are excluded, mirroring
// the checker's float-context exclusions exactly (see its
// is_untyped_int_rooted doc comment for the full rationale): NO shift leg
// (a shift's operands are integer-only, so a shift-rooted subtree must
// never be treated as float-adaptable), and NO / or % in the binop leg (an
// all-int division/modulo truncates as an INT constant in Go before any
// float promotion, which stamp-and-compute can't reproduce — the checker
// rejects those shapes in float contexts, so codegen must not classify
// them as float-adaptable either).
static int is_int_rooted_float_context(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_LITERAL)
        return ((LiteralNode*)node)->literal_type == TOKEN_INT;
    if (node->type == AST_UNARY_EXPR) {
        UnaryExprNode* u = (UnaryExprNode*)node;
        if (u->operator == TOKEN_MINUS || u->operator == TOKEN_PLUS ||
            u->operator == TOKEN_BIT_XOR)
            return is_int_rooted_float_context(u->operand);
    }
    if (node->type == AST_BINARY_EXPR) {
        BinaryExprNode* b = (BinaryExprNode*)node;
        if (b->operator == TOKEN_PLUS || b->operator == TOKEN_MINUS ||
            b->operator == TOKEN_MULTIPLY)
            return is_int_rooted_float_context(b->left) &&
                   is_int_rooted_float_context(b->right);
    }
    return 0;
}

// Float analogue of coerce_int_literal_operand: unify float32/float64 operand
// widths so LLVM float binary ops (fadd/fcmp/...) get two operands of the SAME
// LLVM FP type — required for the instruction to verify at all (a bare
// `g == 2.5` with g float32 built `fcmp oeq float %g, double 2.5` and failed
// verification before this fix).
//
// Go rule (deliberately NOT "always widen to the bigger type" like the int
// case above): an UNTYPED float constant adapts to the other, TYPED operand —
// so `g == 2.5` (g float32) compares at float32 precision (the 2.5 constant
// is narrowed), and `g * 2.0` yields a float32 product, not a float64 one.
// Only when NEITHER side is an untyped constant (two typed, differently-sized
// float variables/expressions) do we fall back to widening the narrower to
// the wider — real Go actually REJECTS mixing two distinctly-typed floats in
// a binary op without an explicit conversion (`d + g` with d float64, g
// float32 is a compile error in real Go); Goo's checker currently admits it
// (type_compatible treats any numeric-numeric pair as compatible), so
// extend-the-narrower is the least-surprising codegen behavior for the
// combination the checker lets through. See task-3-report.md for the
// resulting checker/codegen result-type disagreement this constant-adapts
// rule produces for arithmetic (not comparison) ops, reported rather than
// papered over.
//
// Codegen-local duplicate of expression_checker.c's static
// is_untyped_float_rooted (checker-internal, not exported via a header edit
// per the task-2 brief — cross-referenced by this comment rather than shared
// through include/types.h — change them together). Is `node` an untyped-
// float-constant-rooted AST operand: a bare float literal, a unary -/+
// through to one, or an arithmetic binop (+,-,*,/) where each side is
// float-rooted OR float-context int-rooted (is_int_rooted_float_context
// above — no shift, no / or % in the int subtree), with at least one side
// float-rooted (task 2 — see the checker's is_untyped_float_rooted for the
// full rationale, including why an all-int binop like `1+2` stays OUT of
// this predicate, and why `/` is kept HERE — a division containing a float
// literal is a float division in Go's constant arithmetic — while an
// all-int division subtree is excluded via the int side test)? This is
// the AST-untypedness test that replaced `LLVMIsConstant` below:
// LLVMIsConstant can't tell an untyped literal from a TYPED constant
// conversion (`float32(0.1)` also constant-folds to an LLVMConstReal),
// which made `float32(0.1) == 0.1` diverge from Go — the typed conversion
// was wrongly treated as "may adapt" and got widened to double instead of
// the untyped `0.1` narrowing to float32.
static int is_float_literal_node(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_LITERAL)
        return ((LiteralNode*)node)->literal_type == TOKEN_FLOAT;
    if (node->type == AST_UNARY_EXPR) {
        UnaryExprNode* u = (UnaryExprNode*)node;
        if (u->operator == TOKEN_MINUS || u->operator == TOKEN_PLUS)
            return is_float_literal_node(u->operand);
    }
    if (node->type == AST_BINARY_EXPR) {
        BinaryExprNode* b = (BinaryExprNode*)node;
        if (b->operator == TOKEN_PLUS || b->operator == TOKEN_MINUS ||
            b->operator == TOKEN_MULTIPLY || b->operator == TOKEN_DIVIDE) {
            int left_ok  = is_float_literal_node(b->left)  || is_int_rooted_float_context(b->left);
            int right_ok = is_float_literal_node(b->right) || is_int_rooted_float_context(b->right);
            return left_ok && right_ok &&
                   (is_float_literal_node(b->left) || is_float_literal_node(b->right));
        }
    }
    return 0;
}

// With the checker now adapting an untyped float literal's stamped type to
// match its typed operand (expression_checker.c's
// is_untyped_float_rooted/adapt_untyped_float_operand, run before this
// codegen pass), the TOKEN_FLOAT literal arm above already emits the
// literal at the RIGHT width in the common literal-vs-typed case — so by
// the time we get here `lk == rk` and this function returns early. What's
// left for this function to handle is the backstop case the checker does
// NOT adapt: two TYPED, differently-sized float operands (no literal on
// either side, e.g. `d + g` with d float64, g float32) — extend the
// narrower to the wider (documented Go-rejects-this-case above). The
// AST-untypedness predicate (vs. LLVMIsConstant) is what keeps a typed
// `float32(0.1)` conversion OUT of the "may adapt" path in that comparison.
static void coerce_float_operand_widths(CodeGenerator* codegen,
                                        ASTNode* left_ast, ValueInfo* left,
                                        ASTNode* right_ast, ValueInfo* right) {
    if (!left || !right || !left->goo_type || !right->goo_type) return;
    if (!type_is_float(left->goo_type) || !type_is_float(right->goo_type)) return;
    LLVMTypeRef lt = LLVMTypeOf(left->llvm_value);
    LLVMTypeRef rt = LLVMTypeOf(right->llvm_value);
    LLVMTypeKind lk = LLVMGetTypeKind(lt), rk = LLVMGetTypeKind(rt);
    if ((lk != LLVMFloatTypeKind && lk != LLVMDoubleTypeKind) ||
        (rk != LLVMFloatTypeKind && rk != LLVMDoubleTypeKind)) return;
    if (lk == rk) return;  // already the same LLVM FP width

    int left_untyped  = is_float_literal_node(left_ast);
    int right_untyped = is_float_literal_node(right_ast);

    if (left_untyped != right_untyped) {
        // Untyped-literal-vs-typed: coerce the UNTYPED LITERAL to the typed
        // operand's width (codegen_coerce_to_type FPExt/FPTrunc's either
        // direction; src_signed is irrelevant for FP->FP so pass 1). Backstop
        // only — the checker already narrows/widens the literal's node_type
        // itself, so this branch fires only if that adaptation didn't apply.
        if (left_untyped) {
            left->llvm_value = codegen_coerce_to_type(codegen, left->llvm_value, 1, rt);
        } else {
            right->llvm_value = codegen_coerce_to_type(codegen, right->llvm_value, 1, lt);
        }
        return;
    }

    // Both untyped-literal-rooted, or both typed and differently sized:
    // extend the narrower to the wider (documented Go-rejects-this case
    // above).
    if (lk == LLVMFloatTypeKind) {
        left->llvm_value = codegen_coerce_to_type(codegen, left->llvm_value, 1, rt);
    } else {
        right->llvm_value = codegen_coerce_to_type(codegen, right->llvm_value, 1, lt);
    }
}

#if LLVM_AVAILABLE
// P1-1: lower a string `==`/`!=` to a goo_string_eq call plus an i1 conversion.
// goo_string_eq returns i32 (0/1); `want_equal` selects `== `(true iff equal,
// eqi != 0) vs `!=` (true iff not equal, eqi == 0). Returns NULL (after a
// source-located error) if the runtime symbol is missing.
static LLVMValueRef codegen_string_eq_to_i1(CodeGenerator* codegen,
                                            LLVMValueRef a, LLVMValueRef b,
                                            int want_equal, ASTNode* expr) {
    LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_string_eq");
    if (!fn) {
        codegen_error(codegen, expr->pos, "goo_string_eq not found in module");
        return NULL;
    }
    LLVMValueRef args[2] = { a, b };
    LLVMValueRef eqi = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn),
                                      fn, args, 2, "streq");
    LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0);
    return LLVMBuildICmp(codegen->builder, want_equal ? LLVMIntNE : LLVMIntEQ,
                         eqi, zero, want_equal ? "streq_b" : "strne_b");
}

// P1-2: lower a string ordering comparison to goo_string_cmp (returns
// -1/0/1) followed by `<pred> 0`, where pred is SLT/SLE/SGT/SGE for
// < / <= / > / >= respectively.
static LLVMValueRef codegen_string_cmp_to_i1(CodeGenerator* codegen,
                                             LLVMValueRef a, LLVMValueRef b,
                                             LLVMIntPredicate pred, ASTNode* expr) {
    LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_string_cmp");
    if (!fn) {
        codegen_error(codegen, expr->pos, "goo_string_cmp not found in module");
        return NULL;
    }
    LLVMValueRef args[2] = { a, b };
    LLVMValueRef cmp = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn),
                                      fn, args, 2, "strcmp");
    LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0);
    return LLVMBuildICmp(codegen->builder, pred, cmp, zero, "strcmp_b");
}

// P1-7: emit a runtime divide-by-zero guard before an integer div/rem. If the
// divisor is 0, call goo_panic('integer divide by zero') (which aborts) and
// mark the block unreachable; otherwise fall through to `divzero.cont`, where
// the builder is left positioned so the division is emitted on the safe path.
static void codegen_emit_divzero_check(CodeGenerator* codegen, LLVMValueRef divisor,
                                       ASTNode* expr) {
    (void)expr;
    LLVMValueRef panic_fn = LLVMGetNamedFunction(codegen->module, "goo_panic");
    if (!panic_fn) return;  // no panic symbol: emit the div unguarded
    LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(divisor), 0, 0);
    LLVMValueRef iszero = LLVMBuildICmp(codegen->builder, LLVMIntEQ, divisor, zero, "divzero");
    LLVMBasicBlockRef panic_bb = codegen_create_block(codegen, "divzero.panic");
    LLVMBasicBlockRef cont_bb = codegen_create_block(codegen, "divzero.cont");
    LLVMBuildCondBr(codegen->builder, iszero, panic_bb, cont_bb);

    codegen_set_insert_point(codegen, panic_bb);
    LLVMValueRef msg = LLVMBuildGlobalStringPtr(codegen->builder,
                                                "integer divide by zero", "divzero_msg");
    LLVMValueRef args[1] = { msg };
    LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(panic_fn), panic_fn, args, 1, "");
    LLVMBuildUnreachable(codegen->builder);

    codegen_set_insert_point(codegen, cont_bb);
}
#endif

ValueInfo* codegen_generate_binary_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_BINARY_EXPR) return NULL;
    
    BinaryExprNode* binary = (BinaryExprNode*)expr;

    // Compound assignment (x += e, x &= e, ...): lower to load-op-store. Resolve
    // the target address, then temporarily retype this node to the BASE operator
    // and recurse — that reuses the full binary-op codegen (operand width
    // coercion, signedness) to compute `x <op> e`, loading x from the same
    // lvalue. Store the result back, coerced to the target's width like a plain
    // assignment.
    {
        TokenType base_op = TOKEN_UNKNOWN;
        switch (binary->operator) {
            case TOKEN_PLUS_ASSIGN:   base_op = TOKEN_PLUS; break;
            case TOKEN_MINUS_ASSIGN:  base_op = TOKEN_MINUS; break;
            case TOKEN_MUL_ASSIGN:    base_op = TOKEN_MULTIPLY; break;
            case TOKEN_DIV_ASSIGN:    base_op = TOKEN_DIVIDE; break;
            case TOKEN_MOD_ASSIGN:    base_op = TOKEN_MODULO; break;
            case TOKEN_AND_ASSIGN:    base_op = TOKEN_BIT_AND; break;
            case TOKEN_OR_ASSIGN:     base_op = TOKEN_BIT_OR; break;
            case TOKEN_XOR_ASSIGN:    base_op = TOKEN_BIT_XOR; break;
            case TOKEN_LSHIFT_ASSIGN: base_op = TOKEN_LSHIFT; break;
            case TOKEN_RSHIFT_ASSIGN: base_op = TOKEN_RSHIFT; break;
            default: break;
        }
        if (base_op != TOKEN_UNKNOWN) {
            // `m[k] += n` (and m[k]-=, m[k]*=, ...): map values are never
            // addressable (see codegen_emit_lvalue_address's AST_INDEX_EXPR
            // arm below, which still rejects &m[k] / m[k].F = v
            // unconditionally), so route to the read-modify-write helper
            // BEFORE resolving an lvalue address — mirrors the `m[k] = v`
            // fast path's map detection (type_check_expression + TYPE_MAP)
            // a few dozen lines down in this same function's TOKEN_ASSIGN
            // arm. Unsupported ops (anything but + - *) are rejected inside
            // the helper, not here — v1 admits only + - * on map values.
            if (binary->left->type == AST_INDEX_EXPR) {
                IndexExprNode* cidx = (IndexExprNode*)binary->left;
                Type* cbase_t = type_check_expression(checker, cidx->expr);
                if (cbase_t && cbase_t->kind == TYPE_MAP) {
                    // Compound-assign returns the POST-op (new) value — matches
                    // the non-map compound-assign arm's `return newval;` a few
                    // dozen lines down, which evaluates `x <op> e` and hands
                    // back the result already stored into x.
                    return codegen_map_index_rmw(codegen, checker, binary->left, base_op,
                                                 binary->right, /*return_old_value=*/false);
                }
            }

            ValueInfo* target = codegen_emit_lvalue_address(codegen, checker, binary->left);
            if (!target || !target->is_lvalue) {
                codegen_error(codegen, expr->pos,
                              "Compound-assignment target must be an addressable lvalue");
                return NULL;
            }
            binary->operator = base_op;
            ValueInfo* newval = codegen_generate_binary_expr(codegen, checker, expr);
            binary->operator = (base_op == TOKEN_PLUS)     ? TOKEN_PLUS_ASSIGN
                             : (base_op == TOKEN_MINUS)    ? TOKEN_MINUS_ASSIGN
                             : (base_op == TOKEN_MULTIPLY) ? TOKEN_MUL_ASSIGN
                             : (base_op == TOKEN_DIVIDE)   ? TOKEN_DIV_ASSIGN
                             : (base_op == TOKEN_MODULO)   ? TOKEN_MOD_ASSIGN
                             : (base_op == TOKEN_BIT_AND)  ? TOKEN_AND_ASSIGN
                             : (base_op == TOKEN_BIT_OR)   ? TOKEN_OR_ASSIGN
                             : (base_op == TOKEN_BIT_XOR)  ? TOKEN_XOR_ASSIGN
                             : (base_op == TOKEN_LSHIFT)   ? TOKEN_LSHIFT_ASSIGN
                                                           : TOKEN_RSHIFT_ASSIGN;
            if (!newval) return NULL;
            LLVMValueRef sval = newval->llvm_value;
            if (target->goo_type &&
                LLVMGetTypeKind(LLVMTypeOf(sval)) == LLVMIntegerTypeKind) {
                LLVMTypeRef tt = codegen_type_to_llvm(codegen, target->goo_type);
                if (tt && LLVMGetTypeKind(tt) == LLVMIntegerTypeKind) {
                    unsigned sw = LLVMGetIntTypeWidth(LLVMTypeOf(sval));
                    unsigned tw = LLVMGetIntTypeWidth(tt);
                    if (sw > tw)
                        sval = LLVMBuildTrunc(codegen->builder, sval, tt, "casn.trunc");
                    else if (sw < tw)
                        sval = (newval->goo_type && type_is_signed(newval->goo_type))
                            ? LLVMBuildSExt(codegen->builder, sval, tt, "casn.sext")
                            : LLVMBuildZExt(codegen->builder, sval, tt, "casn.zext");
                }
            }
            LLVMBuildStore(codegen->builder, sval, target->llvm_value);
            return newval;
        }
    }

    // Special handling for assignment
    if (binary->operator == TOKEN_ASSIGN) {
        // Blank identifier `_` as a plain-assignment target (F1): `_ = rhs`
        // evaluates the RHS for its side effects but stores nowhere. Intercept
        // before lvalue resolution (which has no storage for `_`).
        if (binary->left && binary->left->type == AST_IDENTIFIER &&
            strcmp(((IdentifierNode*)binary->left)->name, "_") == 0) {
            ValueInfo* value = codegen_generate_expression(codegen, checker, binary->right);
            if (!value) return NULL;
            if (value->is_lvalue && value->goo_type) {
                LLVMTypeRef vt = codegen_type_to_llvm(codegen, value->goo_type);
                if (vt) {
                    value->llvm_value = LLVMBuildLoad2(codegen->builder, vt, value->llvm_value, "rval");
                    value->is_lvalue = 0;
                }
            }
            return value;  // discarded — nothing is stored
        }

        // Map element assignment `m[k] = v` has no addressable lvalue — it
        // lowers to a goo_map_set_sv call. Intercept before lvalue resolution
        // (which rejects map index targets). Only the map case is handled
        // here; array/slice index assignment falls through to the GEP path.
        if (binary->left->type == AST_INDEX_EXPR) {
            IndexExprNode* idx = (IndexExprNode*)binary->left;
            Type* base_t = type_check_expression(checker, idx->expr);
            if (base_t && base_t->kind == TYPE_MAP) {
                Type* key_type = base_t->data.map.key_type;
                Type* val_type = base_t->data.map.value_type;
                LLVMValueRef set_fn = LLVMGetNamedFunction(codegen->module, "goo_map_set_sv");
                if (!set_fn) {
                    codegen_error(codegen, expr->pos, "goo_map_set_sv missing");
                    return NULL;
                }
                ValueInfo* mv = codegen_generate_expression(codegen, checker, idx->expr);
                ValueInfo* kv = codegen_generate_expression(codegen, checker, idx->index);
                if (!mv || !kv) { value_info_free(mv); value_info_free(kv); return NULL; }
                // Box a concrete key into an interface-typed map key BEFORE
                // slot-packing (Task 2) — mirrors the value-boxing step
                // below for the RHS value; no-op for non-interface keys.
                if (!codegen_box_map_key_if_needed(codegen, checker, kv, key_type, expr->pos)) {
                    value_info_free(mv);
                    value_info_free(kv);
                    return NULL;
                }
                LLVMValueRef kp = codegen_map_key_to_slot(codegen, checker, kv, key_type);
                ValueInfo* vv = codegen_generate_expression(codegen, checker, binary->right);
                if (!vv) { value_info_free(mv); value_info_free(kv); return NULL; }
                if (vv->is_lvalue && vv->goo_type) {
                    LLVMTypeRef vt = codegen_type_to_llvm(codegen, vv->goo_type);
                    if (vt) {
                        vv->llvm_value = LLVMBuildLoad2(codegen->builder, vt, vv->llvm_value, "rval");
                        vv->is_lvalue = 0;
                    }
                }
                // Box a concrete implementer into the map's interface-typed
                // value slot BEFORE slot-boxing/coercion — same helper the
                // TOKEN_ASSIGN arm below uses for `x = Sq{...}` into an
                // interface-typed lvalue (codegen_interface_box). Without
                // this, codegen_coerce_to_type leaves the aggregate unchanged
                // (it only handles int/float widths), so the raw concrete
                // struct bits would be written into the slot's box instead of
                // a real {vtable,data} pair — reviewer finding I1 (read-back
                // treats those bytes as a vtable pointer -> SIGSEGV).
                // interface->interface (already-boxed RHS, reviewer probe
                // p13) needs no re-box: same layout, falls through unchanged.
                // `stored_val` (not vv->llvm_value/vv->goo_type) carries the
                // boxed representation into the slot, so the returned `vv`
                // keeps reporting the concrete RHS type/value — mirrors the
                // general TOKEN_ASSIGN arm below, which returns the unboxed
                // `value` even though `sval` (boxed) is what gets stored.
                LLVMValueRef stored_val = vv->llvm_value;
                if (val_type->kind == TYPE_INTERFACE &&
                    vv->goo_type && vv->goo_type->kind != TYPE_INTERFACE) {
                    LLVMValueRef boxed = codegen_interface_box(codegen, checker,
                                                               val_type,
                                                               vv->goo_type,
                                                               vv->llvm_value);
                    if (!boxed) {
                        codegen_error(codegen, expr->pos,
                                      "failed to box value into interface map value");
                        value_info_free(mv);
                        value_info_free(kv);
                        return NULL;
                    }
                    stored_val = boxed;
                }
                LLVMTypeRef want_vt = codegen_type_to_llvm(codegen, val_type);
                if (want_vt && !codegen_map_value_is_inline(val_type)) {
                    int src_signed = vv->goo_type &&
                        vv->goo_type->kind >= TYPE_INT8 &&
                        vv->goo_type->kind <= TYPE_INT64;
                    stored_val = codegen_coerce_to_type(
                        codegen, stored_val, src_signed, want_vt);
                }
                LLVMValueRef slot = codegen_map_value_to_slot(codegen, stored_val, val_type);
                LLVMValueRef args[3] = { mv->llvm_value, kp, slot };
                LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(set_fn), set_fn, args, 3, "");
                value_info_free(mv);
                value_info_free(kv);
                return vv;  // assignment evaluates to the stored value
            }
        }

        // Resolve the assignment target's address. Three lvalue forms are
        // supported; each yields a ValueInfo whose llvm_value is a pointer to
        // the storage location (is_lvalue == 1):
        //   - identifier  -> the variable's alloca (codegen_lookup_value)
        //   - field s.x   -> a struct GEP (codegen_generate_selector_expr)
        //   - index a[i]  -> an element GEP (codegen_generate_index_expr)
        // The selector/index read paths already compute and return these GEPs
        // unloaded, so assignment reuses them directly.
        ValueInfo* target = codegen_emit_lvalue_address(codegen, checker, binary->left);
        if (!target || !target->is_lvalue) {
            codegen_error(codegen, expr->pos,
                          "Assignment target must be an addressable lvalue "
                          "(identifier, field, or index)");
            return NULL;
        }

        // Reassignment to a `?T` lvalue (e.g. `b = nil`, `c = 7`). The raw
        // store path below would write an i8* null (for nil) or a bare scalar
        // (for a value) into the {i1,T} nullable slot, corrupting the tag.
        // Route both cases through the shared nullable helpers instead.
        if (target->goo_type && target->goo_type->kind == TYPE_NULLABLE) {
            // nil literal: intercept BEFORE evaluating the RHS, since the
            // generic nil path emits an i8* null of the wrong LLVM type.
            // NOTE: for an identifier target, `target` is the live value-table
            // entry (codegen_emit_lvalue_address returns it directly), so it
            // must NOT be freed here — mirroring the non-nullable store path
            // below, which also never frees `target`.
            if (binary->right->type == AST_LITERAL &&
                ((LiteralNode*)binary->right)->literal_type == TOKEN_NIL) {
                ValueInfo* nil_val = codegen_generate_null_literal(codegen, checker, target->goo_type);
                if (!nil_val) return NULL;
                LLVMBuildStore(codegen->builder, nil_val->llvm_value, target->llvm_value);
                ValueInfo* ret = value_info_new(NULL, nil_val->llvm_value, target->goo_type);
                value_info_free(nil_val);
                return ret;
            }
            // Non-nil RHS: evaluate, load if lvalue, then auto-wrap to the
            // target's nullable type via the shared assignment helper.
            ValueInfo* value = codegen_generate_expression(codegen, checker, binary->right);
            if (!value) return NULL;
            if (value->is_lvalue && value->goo_type) {
                LLVMTypeRef vt = codegen_type_to_llvm(codegen, value->goo_type);
                if (vt) {
                    value->llvm_value = LLVMBuildLoad2(codegen->builder, vt, value->llvm_value, "rval");
                    value->is_lvalue = 0;
                }
            }
            codegen_generate_nullable_assignment(codegen, checker, target->llvm_value,
                                                 value->llvm_value, target->goo_type,
                                                 value->goo_type, expr->pos);
            return value;
        }

        // Generate the right side value. If it is itself an lvalue (e.g.
        // `a[i] = b[j]` or `x = s.y`), dereference it to the scalar value
        // before storing — mirroring the auto-load consumers do elsewhere.
        ValueInfo* value = codegen_generate_expression(codegen, checker, binary->right);
        if (!value) return NULL;
        if (value->is_lvalue && value->goo_type) {
            LLVMTypeRef vt = codegen_type_to_llvm(codegen, value->goo_type);
            if (vt) {
                value->llvm_value = LLVMBuildLoad2(codegen->builder, vt, value->llvm_value, "rval");
                value->is_lvalue = 0;
            }
        }

        // Box a concrete implementer into an interface-typed lvalue's
        // {vtable, data} value (mirrors var-decl init / call-arg boxing).
        // interface→interface needs no box — same layout, store directly.
        LLVMValueRef sval = value->llvm_value;
        if (target->goo_type && target->goo_type->kind == TYPE_INTERFACE &&
            value->goo_type && value->goo_type->kind != TYPE_INTERFACE) {
            LLVMValueRef boxed = codegen_interface_box(codegen, checker,
                                                       target->goo_type,
                                                       value->goo_type,
                                                       value->llvm_value);
            if (!boxed) {
                codegen_error(codegen, expr->pos,
                              "failed to box value into interface on assignment");
                return NULL;
            }
            sval = boxed;
        }

        // Coerce a mismatched-width RHS (int or float) to the target
        // variable's width before storing. A mixed-width binary op widens to
        // its larger operand, so `x = x & int64Const` (x uint32) yields an
        // i64 — storing that into the i32 slot would write 8 bytes over a
        // 4-byte alloca and corrupt the stack. The float counterpart: `g :=
        // float32(2.5); var y float64; y = g * 2.0` — the constant-adapts
        // binop rule (Task 3) keeps the product at float32, so storing it
        // into the float64 slot needs an FPExt. Before this fix the block
        // was int-only, so the float case silently wrote a 4-byte float
        // pattern into an 8-byte double slot (a failure-mode downgrade from
        // the binop fix: it used to be a loud verifier type mismatch).
        // Delegate to the shared width-coercion helper (int<->int,
        // int->float, float<->float); it no-ops on kinds it doesn't handle
        // (aggregates, pointers, already-matching types), so it's safe to
        // call unconditionally here. Assignments are always statements, so
        // the builder is guaranteed positioned.
        if (target->goo_type && value->goo_type) {
            LLVMTypeRef tt = codegen_type_to_llvm(codegen, target->goo_type);
            if (tt) {
                sval = codegen_coerce_to_type(codegen, sval,
                                              type_is_signed(value->goo_type), tt);
            }
        }

        // Store the value into the target's address.
        LLVMBuildStore(codegen->builder, sval, target->llvm_value);

        // Return the stored value
        return value;
    }
    
    // Special handling for channel send (<-)
    if (binary->operator == TOKEN_ARROW) {
        return codegen_generate_channel_send(codegen, checker, expr);
    }

    // Special handling for nullable == nil / nil == nullable.
    // We must NOT evaluate the nil side: codegen_generate_null_literal
    // without a context type emits an i8* null, which is the wrong LLVM
    // type to compare against a { i1, T } nullable struct. Instead, detect
    // the pattern here, evaluate ONLY the nullable operand, then read its
    // is_null flag directly.
    if (binary->operator == TOKEN_EQ || binary->operator == TOKEN_NE) {
        ASTNode* nullable_node = NULL;

        // Determine which side is the nil literal and which is nullable.
        bool right_is_nil = (binary->right->type == AST_LITERAL &&
                             ((LiteralNode*)binary->right)->literal_type == TOKEN_NIL);
        bool left_is_nil  = (binary->left->type == AST_LITERAL &&
                             ((LiteralNode*)binary->left)->literal_type == TOKEN_NIL);

        if (right_is_nil) {
            // Run the type checker on the left to confirm it is ?T.
            Type* lt = type_check_expression(checker, binary->left);
            if (lt && type_is_nullable(lt)) {
                nullable_node = binary->left;
            }
        } else if (left_is_nil) {
            // nil == ?T form
            Type* rt = type_check_expression(checker, binary->right);
            if (rt && type_is_nullable(rt)) {
                nullable_node = binary->right;
            }
        }

        if (nullable_node) {
            // Evaluate only the nullable operand.
            ValueInfo* nv = codegen_generate_expression(codegen, checker, nullable_node);
            if (!nv) return NULL;

            // Auto-load if the result is an lvalue (e.g. identifier/selector).
            if (nv->is_lvalue && nv->goo_type) {
                LLVMTypeRef nt = codegen_type_to_llvm(codegen, nv->goo_type);
                if (nt) {
                    nv->llvm_value = LLVMBuildLoad2(codegen->builder, nt, nv->llvm_value, "nullable_load");
                    nv->is_lvalue = 0;
                }
            }

            // Read the is_null flag (struct index 0) via the header-declared helper.
            LLVMValueRef is_null = codegen_check_nullable_null(codegen, nv->llvm_value);
            LLVMValueRef result;

            if (binary->operator == TOKEN_EQ) {
                // a == nil  →  is_null
                result = is_null;
            } else {
                // a != nil  →  !is_null
                result = LLVMBuildNot(codegen->builder, is_null, "is_not_null");
            }

            Type* bool_type = type_checker_get_builtin(checker, TYPE_BOOL);
            value_info_free(nv);
            return value_info_new(NULL, result, bool_type);
        }
    }

    // funcval == nil / nil == funcval (queue #3). A function value is the fat
    // pointer { i8* fn_ptr, i8* env }; its zero value has a null fn_ptr. Mirror
    // the nullable path above: do NOT evaluate the nil side (a bare i8* null is
    // the wrong shape to compare against the pair struct) — evaluate only the
    // funcval operand and compare its fn-ptr word (field 0) against null. The
    // env word is deliberately ignored (a nil funcval has no closure).
    if (binary->operator == TOKEN_EQ || binary->operator == TOKEN_NE) {
        bool right_is_nil = (binary->right->type == AST_LITERAL &&
                             ((LiteralNode*)binary->right)->literal_type == TOKEN_NIL);
        bool left_is_nil  = (binary->left->type == AST_LITERAL &&
                             ((LiteralNode*)binary->left)->literal_type == TOKEN_NIL);

        ASTNode* funcval_node = NULL;
        if (right_is_nil) {
            Type* lt = type_check_expression(checker, binary->left);
            if (lt && lt->kind == TYPE_FUNCTION) funcval_node = binary->left;
        } else if (left_is_nil) {
            Type* rt = type_check_expression(checker, binary->right);
            if (rt && rt->kind == TYPE_FUNCTION) funcval_node = binary->right;
        }

        if (funcval_node) {
            ValueInfo* fv = codegen_generate_expression(codegen, checker, funcval_node);
            if (!fv) return NULL;

            // Auto-load if the operand is an lvalue (e.g. a plain identifier),
            // yielding the { i8*, i8* } pair value itself.
            if (fv->is_lvalue && fv->goo_type) {
                LLVMTypeRef ft = codegen_type_to_llvm(codegen, fv->goo_type);
                if (ft) {
                    fv->llvm_value = LLVMBuildLoad2(codegen->builder, ft, fv->llvm_value, "funcval_load");
                    fv->is_lvalue = 0;
                }
            }

            LLVMValueRef fn_ptr = LLVMBuildExtractValue(codegen->builder, fv->llvm_value, 0, "funcval_fnptr");
            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
            LLVMValueRef null_ptr = LLVMConstPointerNull(i8ptr);
            LLVMValueRef result = LLVMBuildICmp(codegen->builder,
                binary->operator == TOKEN_EQ ? LLVMIntEQ : LLVMIntNE,
                fn_ptr, null_ptr, "funcval_nilcmp");

            Type* bool_type = type_checker_get_builtin(checker, TYPE_BOOL);
            value_info_free(fv);
            return value_info_new(NULL, result, bool_type);
        }
    }

    // iface == nil / nil == iface (RTTI follow-up). An interface value is the
    // pair { ptr vtable, ptr data }; its nil (zero) value has both words null.
    // Mirror the nullable/funcval paths: do NOT evaluate the nil side (a bare
    // i8* null is the wrong shape to compare against the pair) — evaluate only
    // the interface operand and test BOTH words against null, matching the
    // type-switch `case nil:` test (statement_codegen.c) so `x == nil` and
    // `switch x.(type){case nil:}` agree.
    if (binary->operator == TOKEN_EQ || binary->operator == TOKEN_NE) {
        bool right_is_nil = (binary->right->type == AST_LITERAL &&
                             ((LiteralNode*)binary->right)->literal_type == TOKEN_NIL);
        bool left_is_nil  = (binary->left->type == AST_LITERAL &&
                             ((LiteralNode*)binary->left)->literal_type == TOKEN_NIL);

        ASTNode* iface_node = NULL;
        if (right_is_nil) {
            Type* lt = type_check_expression(checker, binary->left);
            if (lt && lt->kind == TYPE_INTERFACE) iface_node = binary->left;
        } else if (left_is_nil) {
            Type* rt = type_check_expression(checker, binary->right);
            if (rt && rt->kind == TYPE_INTERFACE) iface_node = binary->right;
        }

        if (iface_node) {
            ValueInfo* iv = codegen_generate_expression(codegen, checker, iface_node);
            if (!iv) return NULL;

            // Auto-load if the operand is an lvalue (e.g. a plain identifier),
            // yielding the { ptr, ptr } pair value itself.
            if (iv->is_lvalue && iv->goo_type) {
                LLVMTypeRef it = codegen_type_to_llvm(codegen, iv->goo_type);
                if (it) {
                    iv->llvm_value = LLVMBuildLoad2(codegen->builder, it, iv->llvm_value, "iface_load");
                    iv->is_lvalue = 0;
                }
            }

            LLVMValueRef vt   = LLVMBuildExtractValue(codegen->builder, iv->llvm_value, 0, "iface_vt");
            LLVMValueRef data = LLVMBuildExtractValue(codegen->builder, iv->llvm_value, 1, "iface_data");
            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
            LLVMValueRef null_ptr = LLVMConstPointerNull(i8ptr);
            LLVMIntPredicate pred = (binary->operator == TOKEN_EQ) ? LLVMIntEQ : LLVMIntNE;
            LLVMValueRef vt_cmp   = LLVMBuildICmp(codegen->builder, pred, vt, null_ptr, "iface_vtcmp");
            LLVMValueRef data_cmp = LLVMBuildICmp(codegen->builder, pred, data, null_ptr, "iface_datacmp");
            // == nil: both words null (AND). != nil: either non-null (OR) — De
            // Morgan of the == form, so the two operators stay consistent.
            LLVMValueRef result = (binary->operator == TOKEN_EQ)
                ? LLVMBuildAnd(codegen->builder, vt_cmp, data_cmp, "iface_niseq")
                : LLVMBuildOr(codegen->builder, vt_cmp, data_cmp, "iface_nisne");

            Type* bool_type = type_checker_get_builtin(checker, TYPE_BOOL);
            value_info_free(iv);
            return value_info_new(NULL, result, bool_type);
        }
    }

    // P1-5: short-circuit && / ||. Must run BEFORE the eager operand
    // generation below — the whole point is that the right operand is only
    // evaluated when the result isn't already determined by the left.
    //   a && b  ≡  a ? b : false
    //   a || b  ≡  a ? true : b
    if (binary->operator == TOKEN_AND || binary->operator == TOKEN_OR) {
        int is_and = (binary->operator == TOKEN_AND);

        // Evaluate the left operand to an i1.
        ValueInfo* lv = codegen_generate_expression(codegen, checker, binary->left);
        if (!lv) return NULL;
        if (lv->is_lvalue && lv->goo_type) {
            LLVMTypeRef lt = codegen_type_to_llvm(codegen, lv->goo_type);
            if (lt) {
                lv->llvm_value = LLVMBuildLoad2(codegen->builder, lt, lv->llvm_value, "lval");
                lv->is_lvalue = 0;
            }
        }
        LLVMValueRef lbool = lv->llvm_value;
        value_info_free(lv);

        // Capture the block the conditional branch lives in: it is the
        // PHI predecessor for the short-circuit value.
        LLVMBasicBlockRef entry_bb = LLVMGetInsertBlock(codegen->builder);
        LLVMBasicBlockRef rhs_bb = codegen_create_block(codegen, is_and ? "and.rhs" : "or.rhs");
        LLVMBasicBlockRef merge_bb = codegen_create_block(codegen, is_and ? "and.merge" : "or.merge");

        // && : left true -> evaluate rhs; left false -> merge (result false).
        // || : left true -> merge (result true); left false -> evaluate rhs.
        if (is_and) LLVMBuildCondBr(codegen->builder, lbool, rhs_bb, merge_bb);
        else        LLVMBuildCondBr(codegen->builder, lbool, merge_bb, rhs_bb);

        // rhs block: evaluate the right operand (only reached when needed).
        codegen_set_insert_point(codegen, rhs_bb);
        ValueInfo* rv = codegen_generate_expression(codegen, checker, binary->right);
        if (!rv) return NULL;
        if (rv->is_lvalue && rv->goo_type) {
            LLVMTypeRef rt = codegen_type_to_llvm(codegen, rv->goo_type);
            if (rt) {
                rv->llvm_value = LLVMBuildLoad2(codegen->builder, rt, rv->llvm_value, "rval");
                rv->is_lvalue = 0;
            }
        }
        LLVMValueRef rbool = rv->llvm_value;
        value_info_free(rv);
        // The rhs may itself have introduced blocks (nested &&/||, calls);
        // the PHI predecessor is wherever rhs evaluation actually ended.
        LLVMBasicBlockRef rhs_end_bb = LLVMGetInsertBlock(codegen->builder);
        LLVMBuildBr(codegen->builder, merge_bb);

        // merge: phi over the short-circuit constant and the rhs value.
        codegen_set_insert_point(codegen, merge_bb);
        LLVMTypeRef i1 = LLVMInt1TypeInContext(codegen->context);
        LLVMValueRef phi = LLVMBuildPhi(codegen->builder, i1, is_and ? "and.sc" : "or.sc");
        LLVMValueRef sc_const = LLVMConstInt(i1, is_and ? 0 : 1, 0);
        LLVMValueRef in_vals[2] = { sc_const, rbool };
        LLVMBasicBlockRef in_blocks[2] = { entry_bb, rhs_end_bb };
        LLVMAddIncoming(phi, in_vals, in_blocks, 2);

        return value_info_new(NULL, phi, type_checker_get_builtin(checker, TYPE_BOOL));
    }

    // Generate left and right operands
    ValueInfo* left_val = codegen_generate_expression(codegen, checker, binary->left);
    if (!left_val) return NULL;

    // Auto-load any lvalue (selector / index / parameter / var read)
    // before using it as a value. codegen_generate_identifier loads
    // automatically; codegen_generate_selector_expr returns the field
    // address as an lvalue so it can also serve assignment targets,
    // which means binary_expr (and similar consumers) have to
    // dereference it themselves to get the actual scalar value.
    if (left_val->is_lvalue && left_val->goo_type) {
        LLVMTypeRef lt = codegen_type_to_llvm(codegen, left_val->goo_type);
        if (lt) {
            left_val->llvm_value = LLVMBuildLoad2(codegen->builder, lt, left_val->llvm_value, "lval");
            left_val->is_lvalue = 0;
        }
    }

    ValueInfo* right_val = codegen_generate_expression(codegen, checker, binary->right);
    if (!right_val) {
        value_info_free(left_val);
        return NULL;
    }

    // Auto-load the right operand too: a selector/index lvalue (e.g. the `p.y`
    // in `p.x + p.y`) returns the field address, so it must be dereferenced to
    // the scalar value before the binary op — mirroring the left operand above.
    if (right_val->is_lvalue && right_val->goo_type) {
        LLVMTypeRef rt = codegen_type_to_llvm(codegen, right_val->goo_type);
        if (rt) {
            right_val->llvm_value = LLVMBuildLoad2(codegen->builder, rt, right_val->llvm_value, "rval");
            right_val->is_lvalue = 0;
        }
    }

    // M3: reconcile an int literal operand's width to the other operand's
    // type so e.g. `int64_var == -1` compares i64-to-i64, not i64-to-i32.
    coerce_int_literal_operand(codegen, binary->left, left_val,
                               binary->right, right_val);

    // Float analogue: reconcile float32/float64 operand widths (untyped
    // literal adapts to typed operand; typed-vs-typed extends the narrower)
    // so e.g. `g == 2.5` (g float32) compares float-to-float, not
    // float-to-double.
    coerce_float_operand_widths(codegen, binary->left, left_val,
                                binary->right, right_val);

    // CHANGE-TOGETHER (task 2, checker/codegen hygiene): read the checker's
    // recording instead of re-invoking it here. This USED TO be `Type*
    // result_type = type_check_binary_expr(checker, expr);` — a full
    // re-check, called AFTER left_val/right_val above were already
    // generated. That ordering was the bug: type_check_literal
    // unconditionally overwrites a literal's node_type to its untyped
    // default (INT64/FLOAT64) on every call, so re-checking `expr`'s
    // children from scratch here clobbered any outer-context adaptation the
    // FIRST (real) checker pass had stamped on them — e.g. the `1+2` in
    // `(1+2) * g` (g float32): the outer `*` node's cross-kind adaptation
    // stamps `1+2` and its leaves FLOAT32 during the one true pass, but a
    // later isolated re-check of `1+2` alone (no visibility of sibling `g`)
    // re-derives plain INT64 and stomps that stamp back down (see #101/#102
    // review history, and expression_checker.c's invariant comment on
    // type_check_binary_expr). The checker always runs before codegen, so a
    // NULL recording here means a successful-return path there forgot to
    // set node_type — a compiler bug, not a user-facing error.
    Type* result_type = expr->node_type;
    if (!result_type) {
        codegen_error(codegen, expr->pos,
                      "internal: binary expression not typed by checker "
                      "(missing node_type) -- compiler bug");
        value_info_free(left_val);
        value_info_free(right_val);
        return NULL;
    }
    
    LLVMValueRef result = NULL;
    LLVMValueRef left_llvm = left_val->llvm_value;
    LLVMValueRef right_llvm = right_val->llvm_value;

    // A shift requires both operands to share an LLVM integer width, but Go
    // allows the count to have any integer type (and Goo's uint is 32-bit while
    // the shifted value is often 64-bit, e.g. bits.RotateLeft64's `x >> s`).
    // Coerce the count to the shifted value's width — zero-extend or truncate,
    // a shift count is non-negative — so the shift verifies.
    if ((binary->operator == TOKEN_LSHIFT || binary->operator == TOKEN_RSHIFT) &&
        LLVMGetTypeKind(LLVMTypeOf(left_llvm)) == LLVMIntegerTypeKind &&
        LLVMGetTypeKind(LLVMTypeOf(right_llvm)) == LLVMIntegerTypeKind) {
        unsigned lw = LLVMGetIntTypeWidth(LLVMTypeOf(left_llvm));
        unsigned rw = LLVMGetIntTypeWidth(LLVMTypeOf(right_llvm));
        if (rw < lw) {
            right_llvm = LLVMBuildZExt(codegen->builder, right_llvm,
                                       LLVMTypeOf(left_llvm), "shcnt.zext");
        } else if (rw > lw) {
            right_llvm = LLVMBuildTrunc(codegen->builder, right_llvm,
                                        LLVMTypeOf(left_llvm), "shcnt.trunc");
        }
    }
    // Other integer binary ops (arithmetic, bitwise, comparison) require both
    // operands at the same LLVM width. A mixed-width integer expression can
    // survive type checking (the checker's result type is the wider operand) —
    // e.g. `x & (m3 & m)` with a uint32 x and an int64 const mask in
    // bits.ReverseBytes32. Widen the narrower operand to the wider, extending by
    // ITS OWN signedness (sext signed / zext unsigned). String `+` is untouched:
    // a goo_string is a struct, not an integer LLVM type, so the guard skips it.
    else if (LLVMGetTypeKind(LLVMTypeOf(left_llvm)) == LLVMIntegerTypeKind &&
             LLVMGetTypeKind(LLVMTypeOf(right_llvm)) == LLVMIntegerTypeKind) {
        unsigned lw = LLVMGetIntTypeWidth(LLVMTypeOf(left_llvm));
        unsigned rw = LLVMGetIntTypeWidth(LLVMTypeOf(right_llvm));
        // For arithmetic/bitwise ops, reconcile BOTH operands to the RESULT
        // type's width, so the operation happens at the width Go computes it at.
        // This is load-bearing for multiply/subtract where a 64-bit vs 32-bit op
        // differs: the deBruijn hash `uint32(x&-x) * deBruijn32` must wrap mod
        // 2^32 even though the untyped const deBruijn32 is int64 here (the type
        // checker already types the result uint32). Comparisons are excluded —
        // their result is bool, so they keep the widen-narrower-to-wider rule.
        int is_cmp = (binary->operator == TOKEN_EQ || binary->operator == TOKEN_NE ||
                      binary->operator == TOKEN_LT || binary->operator == TOKEN_LE ||
                      binary->operator == TOKEN_GT || binary->operator == TOKEN_GE);
        unsigned target_w = 0;
        LLVMTypeRef target_t = NULL;
        if (!is_cmp && result_type && type_is_integer(result_type)) {
            target_t = codegen_type_to_llvm(codegen, result_type);
            if (target_t && LLVMGetTypeKind(target_t) == LLVMIntegerTypeKind)
                target_w = LLVMGetIntTypeWidth(target_t);
        }
        if (target_w == 0) { // comparison / non-integer result: widen to the wider
            target_w = (lw > rw) ? lw : rw;
            target_t = (lw > rw) ? LLVMTypeOf(left_llvm) : LLVMTypeOf(right_llvm);
        }
        if (lw != target_w) {
            int sgn = left_val->goo_type && type_is_signed(left_val->goo_type);
            left_llvm = (lw > target_w)
                ? LLVMBuildTrunc(codegen->builder, left_llvm, target_t, "binl.trunc")
                : (sgn ? LLVMBuildSExt(codegen->builder, left_llvm, target_t, "binl.sext")
                       : LLVMBuildZExt(codegen->builder, left_llvm, target_t, "binl.zext"));
        }
        if (rw != target_w) {
            int sgn = right_val->goo_type && type_is_signed(right_val->goo_type);
            right_llvm = (rw > target_w)
                ? LLVMBuildTrunc(codegen->builder, right_llvm, target_t, "binr.trunc")
                : (sgn ? LLVMBuildSExt(codegen->builder, right_llvm, target_t, "binr.sext")
                       : LLVMBuildZExt(codegen->builder, right_llvm, target_t, "binr.zext"));
        }
    }

    // Generate operation based on operator and types
    switch (binary->operator) {
        // Arithmetic operators
        case TOKEN_PLUS:
            if (left_val->goo_type && left_val->goo_type->kind == TYPE_STRING) {
                // String concatenation (P3-3): both operands are goo_string_t
                // values (16-byte {data, len} structs passed by value); call
                // the runtime fn and return its goo_string_t result.
                LLVMValueRef concat_fn = LLVMGetNamedFunction(codegen->module, "goo_string_concat");
                if (!concat_fn) {
                    codegen_error(codegen, expr->pos, "goo_string_concat not found in module");
                    value_info_free(left_val);
                    value_info_free(right_val);
                    return NULL;
                }
                LLVMValueRef cargs[] = { left_llvm, right_llvm };
                result = LLVMBuildCall2(codegen->builder,
                                        LLVMGlobalGetValueType(concat_fn), concat_fn,
                                        cargs, 2, "strconcat");
            } else if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildAdd(codegen->builder, left_llvm, right_llvm, "add");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFAdd(codegen->builder, left_llvm, right_llvm, "fadd");
            }
            break;
            
        case TOKEN_MINUS:
            if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildSub(codegen->builder, left_llvm, right_llvm, "sub");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFSub(codegen->builder, left_llvm, right_llvm, "fsub");
            }
            break;
            
        case TOKEN_MULTIPLY:
            if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildMul(codegen->builder, left_llvm, right_llvm, "mul");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFMul(codegen->builder, left_llvm, right_llvm, "fmul");
            }
            break;
            
        case TOKEN_DIVIDE:
            if (type_is_integer(left_val->goo_type)) {
                codegen_emit_divzero_check(codegen, right_llvm, expr); // P1-7
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildSDiv(codegen->builder, left_llvm, right_llvm, "sdiv");
                } else {
                    result = LLVMBuildUDiv(codegen->builder, left_llvm, right_llvm, "udiv");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFDiv(codegen->builder, left_llvm, right_llvm, "fdiv");
            }
            break;
            
        case TOKEN_MODULO:
            if (type_is_integer(left_val->goo_type)) {
                codegen_emit_divzero_check(codegen, right_llvm, expr); // P1-7
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildSRem(codegen->builder, left_llvm, right_llvm, "srem");
                } else {
                    result = LLVMBuildURem(codegen->builder, left_llvm, right_llvm, "urem");
                }
            }
            break;
            
        // Comparison operators
        case TOKEN_EQ:
            if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildICmp(codegen->builder, LLVMIntEQ, left_llvm, right_llvm, "eq");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOEQ, left_llvm, right_llvm, "feq");
            } else if (left_val->goo_type && left_val->goo_type->kind == TYPE_BOOL) {
                // P1-3: bools are i1 — integer-equality is the right lowering.
                result = LLVMBuildICmp(codegen->builder, LLVMIntEQ, left_llvm, right_llvm, "booleq");
            } else if (left_val->goo_type && left_val->goo_type->kind == TYPE_STRING) {
                // P1-1: string value equality via goo_string_eq (returns i32
                // 0/1); convert to i1 with `!= 0` (true iff equal).
                result = codegen_string_eq_to_i1(codegen, left_llvm, right_llvm,
                                                 /*want_equal=*/1, expr);
                if (!result) { value_info_free(left_val); value_info_free(right_val); return NULL; }
            }
            break;

        case TOKEN_NE:
            if (type_is_integer(left_val->goo_type)) {
                result = LLVMBuildICmp(codegen->builder, LLVMIntNE, left_llvm, right_llvm, "ne");
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealONE, left_llvm, right_llvm, "fne");
            } else if (left_val->goo_type && left_val->goo_type->kind == TYPE_BOOL) {
                // P1-3: bool inequality.
                result = LLVMBuildICmp(codegen->builder, LLVMIntNE, left_llvm, right_llvm, "boolne");
            } else if (left_val->goo_type && left_val->goo_type->kind == TYPE_STRING) {
                // P1-1: string inequality — true iff goo_string_eq == 0.
                result = codegen_string_eq_to_i1(codegen, left_llvm, right_llvm,
                                                 /*want_equal=*/0, expr);
                if (!result) { value_info_free(left_val); value_info_free(right_val); return NULL; }
            }
            break;
            
        case TOKEN_LT:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntSLT, left_llvm, right_llvm, "slt");
                } else {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntULT, left_llvm, right_llvm, "ult");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOLT, left_llvm, right_llvm, "flt");
            } else if (left_val->goo_type && left_val->goo_type->kind == TYPE_STRING) {
                result = codegen_string_cmp_to_i1(codegen, left_llvm, right_llvm, LLVMIntSLT, expr);
                if (!result) { value_info_free(left_val); value_info_free(right_val); return NULL; }
            }
            break;
            
        case TOKEN_LE:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntSLE, left_llvm, right_llvm, "sle");
                } else {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntULE, left_llvm, right_llvm, "ule");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOLE, left_llvm, right_llvm, "fle");
            } else if (left_val->goo_type && left_val->goo_type->kind == TYPE_STRING) {
                result = codegen_string_cmp_to_i1(codegen, left_llvm, right_llvm, LLVMIntSLE, expr);
                if (!result) { value_info_free(left_val); value_info_free(right_val); return NULL; }
            }
            break;
            
        case TOKEN_GT:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntSGT, left_llvm, right_llvm, "sgt");
                } else {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntUGT, left_llvm, right_llvm, "ugt");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOGT, left_llvm, right_llvm, "fgt");
            } else if (left_val->goo_type && left_val->goo_type->kind == TYPE_STRING) {
                result = codegen_string_cmp_to_i1(codegen, left_llvm, right_llvm, LLVMIntSGT, expr);
                if (!result) { value_info_free(left_val); value_info_free(right_val); return NULL; }
            }
            break;
            
        case TOKEN_GE:
            if (type_is_integer(left_val->goo_type)) {
                if (type_is_signed(left_val->goo_type)) {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntSGE, left_llvm, right_llvm, "sge");
                } else {
                    result = LLVMBuildICmp(codegen->builder, LLVMIntUGE, left_llvm, right_llvm, "uge");
                }
            } else if (type_is_float(left_val->goo_type)) {
                result = LLVMBuildFCmp(codegen->builder, LLVMRealOGE, left_llvm, right_llvm, "fge");
            } else if (left_val->goo_type && left_val->goo_type->kind == TYPE_STRING) {
                result = codegen_string_cmp_to_i1(codegen, left_llvm, right_llvm, LLVMIntSGE, expr);
                if (!result) { value_info_free(left_val); value_info_free(right_val); return NULL; }
            }
            break;
            
        // Logical operators: handled by the short-circuit path above, which
        // returns before reaching this switch. These cases are unreachable;
        // keep them defensive so a future refactor can't silently fall back
        // to eager (non-short-circuiting) And/Or.
        case TOKEN_AND:
        case TOKEN_OR:
            codegen_error(codegen, expr->pos,
                          "internal: && / || must be lowered by the short-circuit path");
            value_info_free(left_val);
            value_info_free(right_val);
            return NULL;
            
        // Bitwise operators
        case TOKEN_BIT_AND:
            result = LLVMBuildAnd(codegen->builder, left_llvm, right_llvm, "bitand");
            break;

        case TOKEN_AND_NOT: {
            // Go bit-clear `a &^ b` == `a & ~b`.
            LLVMValueRef notr = LLVMBuildNot(codegen->builder, right_llvm, "andnot_not");
            result = LLVMBuildAnd(codegen->builder, left_llvm, notr, "andnot");
            break;
        }

        case TOKEN_BIT_OR:
            result = LLVMBuildOr(codegen->builder, left_llvm, right_llvm, "bitor");
            break;
            
        case TOKEN_BIT_XOR:
            result = LLVMBuildXor(codegen->builder, left_llvm, right_llvm, "xor");
            break;
            
        case TOKEN_LSHIFT:
            result = LLVMBuildShl(codegen->builder, left_llvm, right_llvm, "shl");
            break;
            
        case TOKEN_RSHIFT:
            if (type_is_signed(left_val->goo_type)) {
                result = LLVMBuildAShr(codegen->builder, left_llvm, right_llvm, "ashr");
            } else {
                result = LLVMBuildLShr(codegen->builder, left_llvm, right_llvm, "lshr");
            }
            break;
            
        default:
            codegen_error(codegen, expr->pos, "Unsupported binary operator");
            value_info_free(left_val);
            value_info_free(right_val);
            return NULL;
    }
    
    value_info_free(left_val);
    value_info_free(right_val);

    if (!result) {
        codegen_error(codegen, expr->pos, "Failed to generate binary operation");
        return NULL;
    }

    // CHANGE-TOGETHER (task 2, checker/codegen hygiene): this USED TO be a
    // silent "composite cross-kind self-correction" that overwrote
    // `result_type`'s CATEGORY (int vs float32 vs float64) from the actual
    // LLVM value's type kind whenever they disagreed. It existed because the
    // old `result_type` came from a REDUNDANT re-check of `expr` (see the
    // block above, before the task-2 swap) that ran AFTER operands were
    // already built — for a fully-int-rooted sub-expression like the `1+2`
    // in `(1+2) * g` (g float32), that isolated re-check lost the outer
    // cross-kind context and re-derived plain INT64 for a node the first,
    // real checker pass had stamped FLOAT32, while `result` itself (built
    // from operands read with the FLOAT32 stamp before the re-check ran)
    // genuinely was a float register — a category-only mismatch between
    // `result_type` and `result`.
    //
    // With that re-check gone, `result_type` above IS `expr->node_type` —
    // the SAME single-pass recording that drove codegen_generate_literal's
    // operand emission a few lines up — so this mismatch has no known
    // mechanism left to produce it: verified empirically (temporarily
    // reinstrumented, locally, not committed) that this branch fires ZERO
    // times post-swap across the full 188-probe golden suite, including the
    // exact #102 shapes (`(1+2)*g`, `(1+1)+0.5`) that used to trip it twice
    // per binop_stamp_probe run under the old (re-check) mechanism. Demoted
    // from a silent correction to a loud defensive check: reaching this
    // branch now means the checker's recorded category and the LLVM value
    // codegen actually built have diverged — an invariant violation (e.g. a
    // future regression reintroducing a re-check, or a bug in the
    // untyped-operand adapters) that deserves a compiler-bug error, not a
    // silent second guess.
    if (result_type && !type_is_float(result_type)) {
        LLVMTypeKind result_kind = LLVMGetTypeKind(LLVMTypeOf(result));
        if (result_kind == LLVMFloatTypeKind || result_kind == LLVMDoubleTypeKind) {
            codegen_error(codegen, expr->pos,
                          "internal: checker recorded a non-float result type "
                          "(%s) but codegen built a %s value -- compiler bug",
                          type_to_string(result_type),
                          result_kind == LLVMFloatTypeKind ? "float32" : "float64");
            return NULL;
        }
    }

    return value_info_new(NULL, result, result_type);
#endif
}

ValueInfo* codegen_generate_unary_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_UNARY_EXPR) return NULL;
    
    UnaryExprNode* unary = (UnaryExprNode*)expr;
    
    // Special handling for channel receive
    if (unary->operator == TOKEN_ARROW) {
        return codegen_generate_channel_recv(codegen, checker, expr);
    }
    
    // Generate operand
    ValueInfo* operand = codegen_generate_expression(codegen, checker, unary->operand);
    if (!operand) return NULL;
    
    // Get result type
    Type* result_type = type_check_unary_expr(checker, expr);
    if (!result_type) {
        value_info_free(operand);
        return NULL;
    }
    
    LLVMValueRef result = NULL;
    LLVMValueRef operand_llvm = operand->llvm_value;
    // Auto-load an lvalue operand (bare field selector / index result) for
    // value-consuming operators: -x, !x, ^x, *x all need the VALUE. The
    // address-of case is excluded — TOKEN_BIT_AND works on the unloaded
    // operand (it resolves storage itself via codegen_emit_lvalue_address,
    // and its struct-literal case loads explicitly).
    if (unary->operator != TOKEN_BIT_AND && operand->is_lvalue && operand->goo_type) {
        LLVMTypeRef ot = codegen_type_to_llvm(codegen, operand->goo_type);
        if (ot) operand_llvm = LLVMBuildLoad2(codegen->builder, ot, operand_llvm, "unary_load");
    }

    switch (unary->operator) {
        case TOKEN_PLUS:
            // Unary plus is a no-op
            result = operand_llvm;
            break;
            
        case TOKEN_MINUS:
            if (type_is_integer(operand->goo_type)) {
                result = LLVMBuildNeg(codegen->builder, operand_llvm, "neg");
            } else if (type_is_float(operand->goo_type)) {
                result = LLVMBuildFNeg(codegen->builder, operand_llvm, "fneg");
            }
            break;
            
        case TOKEN_NOT:
            result = LLVMBuildNot(codegen->builder, operand_llvm, "not");
            break;

        case TOKEN_BIT_XOR:   // ^x  (Go bitwise complement)
        case TOKEN_BIT_NOT:   // ~x
            result = LLVMBuildNot(codegen->builder, operand_llvm, "bitnot");
            break;
            
        case TOKEN_MULTIPLY:
            // Dereference pointer. operand_llvm is the pointer value; the
            // pointee LLVM type comes from the goo type (LLVMGetElementType is
            // unusable under opaque pointers).
            if (operand->goo_type->kind == TYPE_POINTER) {
                LLVMTypeRef pointee = codegen_type_to_llvm(codegen, operand->goo_type->data.pointer.pointee_type);
                result = LLVMBuildLoad2(codegen->builder, pointee, operand_llvm, "deref");
            } else {
                codegen_error(codegen, expr->pos, "Cannot dereference non-pointer type");
                value_info_free(operand);
                return NULL;
            }
            break;

        case TOKEN_BIT_AND: {
            // &StructType{...}: Go's addressable-rvalue special case. The
            // literal has no storage — give it leaked heap storage
            // (goo_alloc, the same lifetime model as escaping locals in
            // function_codegen.c) and yield the pointer. The generic
            // operand generation above already produced the aggregate
            // value; reuse it rather than re-generating (re-generation
            // would double-evaluate side effects in field expressions).
            if (unary->operand->type == AST_STRUCT_LITERAL &&
                operand->goo_type && operand->goo_type->kind == TYPE_STRUCT) {
                LLVMTypeRef struct_llvm = codegen_type_to_llvm(codegen, operand->goo_type);
                if (!struct_llvm) {
                    codegen_error(codegen, expr->pos, "&literal: cannot lower struct type");
                    value_info_free(operand);
                    return NULL;
                }
                LLVMValueRef lit_val = operand_llvm;
                if (operand->is_lvalue) {
                    lit_val = LLVMBuildLoad2(codegen->builder, struct_llvm,
                                             lit_val, "addr_lit_load");
                }
                LLVMValueRef alloc_fn = LLVMGetNamedFunction(codegen->module, "goo_alloc");
                if (!alloc_fn) {
                    codegen_error(codegen, expr->pos, "&literal: goo_alloc unavailable");
                    value_info_free(operand);
                    return NULL;
                }
                LLVMValueRef size = LLVMSizeOf(struct_llvm);
                LLVMValueRef heap_ptr = LLVMBuildCall2(codegen->builder,
                                                       LLVMGlobalGetValueType(alloc_fn),
                                                       alloc_fn, &size, 1, "addr_lit");
                LLVMBuildStore(codegen->builder, lit_val, heap_ptr);
                result = heap_ptr;
                result_type = type_pointer(operand->goo_type);
                break;
            }
            // Address-of (`&x`). The generic operand above was loaded (an
            // identifier auto-loads), so use the lvalue-address helper to get
            // the operand's storage address rather than its value.
            ValueInfo* addr = codegen_emit_lvalue_address(codegen, checker, unary->operand);
            if (!addr || !addr->is_lvalue) {
                codegen_error(codegen, expr->pos, "Cannot take address of non-lvalue");
                value_info_free(operand);
                return NULL;
            }
            result = addr->llvm_value;
            result_type = type_pointer(addr->goo_type);
            break;
        }
            
        default:
            codegen_error(codegen, expr->pos, "Unsupported unary operator");
            value_info_free(operand);
            return NULL;
    }
    
    value_info_free(operand);
    
    if (!result) {
        codegen_error(codegen, expr->pos, "Failed to generate unary operation");
        return NULL;
    }
    
    return value_info_new(NULL, result, result_type);
#endif
}

// Goo extension expressions

// Forward declarations for error union functions
ValueInfo* codegen_generate_try_expr_impl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
ValueInfo* codegen_generate_catch_expr_impl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

ValueInfo* codegen_generate_try_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    return codegen_generate_try_expr_impl(codegen, checker, expr);
}

ValueInfo* codegen_generate_catch_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    return codegen_generate_catch_expr_impl(codegen, checker, expr);
}

