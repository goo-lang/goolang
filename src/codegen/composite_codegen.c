#include "codegen.h"
#include "value_scope.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Composite-data lowering: index expressions (array/slice/map),
// struct field selectors, struct literals, and slice literals.
// Split from expression_codegen.c (refactor, no behavior change).

#if LLVM_AVAILABLE
// P1-6: emit a goo_bounds_check(index, length, file, line) call. The runtime
// fn panics if index >= length (negative indices SExt to a huge size_t and so
// also fail), aborting before any out-of-range read/write. The bounds test is
// inside the runtime fn, so no IR branching is emitted here.
void codegen_emit_bounds_check(CodeGenerator* codegen, LLVMValueRef index,
                               LLVMValueRef length, ASTNode* expr) {
    LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_bounds_check");
    if (!fn) return;  // no symbol: index unguarded (best-effort)
    LLVMTypeRef i64 = LLVMInt64TypeInContext(codegen->context);
    LLVMValueRef idx64 = index;
    unsigned iw = LLVMGetIntTypeWidth(LLVMTypeOf(index));
    if (iw < 64)      idx64 = LLVMBuildSExt(codegen->builder, index, i64, "bc_idx");
    else if (iw > 64) idx64 = LLVMBuildTrunc(codegen->builder, index, i64, "bc_idx");
    LLVMValueRef file = LLVMBuildGlobalStringPtr(codegen->builder,
        expr->pos.filename ? expr->pos.filename : "<input>", "bc_file");
    LLVMValueRef line = LLVMConstInt(LLVMInt32TypeInContext(codegen->context),
                                     (unsigned long long)expr->pos.line, 0);
    LLVMValueRef args[4] = { idx64, length, file, line };
    LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn), fn, args, 4, "");
}

// Widen an integer index to a 64-bit offset with the correct signedness. Go
// array/string/slice indices are non-negative offsets, so an UNSIGNED narrow
// index (e.g. a uint8 of 255, as in the math/bits table lookup len8tab[x]) must
// be ZERO-extended. LLVM's GEP default sign-extends, turning 255 into -1 and
// reading before the buffer — the silent bug the table probe exposed. Signed
// indices sign-extend (a genuinely negative index is a bounds panic, not a
// wrap). The result feeds every element GEP and the bounds check, so they agree.
LLVMValueRef codegen_widen_index(CodeGenerator* codegen, ValueInfo* idx) {
    LLVMValueRef v = idx->llvm_value;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(codegen->context);
    LLVMTypeRef vt = LLVMTypeOf(v);
    if (vt == i64) return v;
    unsigned w = LLVMGetIntTypeWidth(vt);
    if (w > 64) return LLVMBuildTrunc(codegen->builder, v, i64, "idx.trunc");
    if (w == 64) return v;
    int is_unsigned = idx->goo_type && !type_is_signed(idx->goo_type);
    return is_unsigned ? LLVMBuildZExt(codegen->builder, v, i64, "idx.zext")
                       : LLVMBuildSExt(codegen->builder, v, i64, "idx.sext");
}
#endif

ValueInfo* codegen_generate_index_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_INDEX_EXPR) return NULL;
    
    IndexExprNode* index_expr = (IndexExprNode*)expr;
    
    // Generate code for the base expression (array/slice/map)
    ValueInfo* base_val = codegen_generate_expression(codegen, checker, index_expr->expr);
    if (!base_val) {
        codegen_error(codegen, expr->pos, "Failed to generate base expression for index");
        return NULL;
    }
    
    // Generate code for the index expression
    ValueInfo* index_val = codegen_generate_expression(codegen, checker, index_expr->index);
    if (!index_val) {
        codegen_error(codegen, expr->pos, "Failed to generate index expression");
        value_info_free(base_val);
        return NULL;
    }
    
    Type* base_type = base_val->goo_type;
    Type* element_type = NULL;
    LLVMValueRef result = NULL;

    // Map indexing fast-path: lower `m[k]` to goo_map_get_sv, then cast the
    // 8-byte i64 slot back to the declared value type V. On a missing key the
    // runtime returns 0 (V's zero value); comma-ok presence is future work.
    if (base_type && base_type->kind == TYPE_MAP) {
        LLVMValueRef get_fn = LLVMGetNamedFunction(codegen->module, "goo_map_get_sv");
        if (!get_fn) {
            codegen_error(codegen, expr->pos, "goo_map_get_sv missing");
            value_info_free(base_val); value_info_free(index_val);
            return NULL;
        }
        Type* key_type = base_type->data.map.key_type;
        // Box a concrete key into an interface-typed map key BEFORE
        // slot-packing (Task 2) — no-op for every non-interface-keyed map.
        if (!codegen_box_map_key_if_needed(codegen, checker, index_val, key_type, expr->pos)) {
            value_info_free(base_val); value_info_free(index_val);
            return NULL;
        }
        LLVMValueRef kp = codegen_map_key_to_slot(codegen, checker, index_val, key_type);
        LLVMValueRef args[2] = { base_val->llvm_value, kp };
        LLVMValueRef slot = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(get_fn),
                                           get_fn, args, 2, "map_get");
        Type* val_type = base_type->data.map.value_type;
        result = codegen_map_slot_to_value(codegen, slot, val_type);
        value_info_free(base_val);
        value_info_free(index_val);
        return value_info_new(NULL, result, val_type);
    }

    // Normalize the index to a signed-correct 64-bit offset before any element
    // GEP or bounds check (see codegen_widen_index). Done after the map
    // fast-path, which returned above — map keys are strings, not integers.
    LLVMValueRef idx64 = codegen_widen_index(codegen, index_val);

    // Handle different indexed types
    switch (base_type->kind) {
        case TYPE_ARRAY: {
            element_type = base_type->data.array.element_type;

            // Fix round 4: instance-time enforcement of the const-index
            // upper bound the checker DEFERRED for a comptime-length array
            // (comptime_length flag — see type_check_index_expr's skip).
            // base_type here is the instance's RE-DERIVED type (real
            // length), so a genuinely out-of-range const index at THIS
            // instance is a clean compile failure — named with the
            // instance symbol (symbol_override is installed for the
            // duration of instance stamping) — instead of a runtime
            // bounds panic. Ordinary arrays never carry the flag; their
            // const OOB indices were already rejected at type-check.
            if (base_type->data.array.comptime_length) {
                uint64_t ci;
                if (goo_fold_const_int_ctx(checker, index_expr->index, &ci) &&
                    ci >= (uint64_t)base_type->data.array.length) {
                    codegen_error(codegen, index_expr->index->pos,
                        "array index %llu out of bounds [0:%zu] in comptime instance '%s'",
                        (unsigned long long)ci, base_type->data.array.length,
                        codegen->symbol_override ? codegen->symbol_override : "?");
                    value_info_free(base_val);
                    value_info_free(index_val);
                    return NULL;
                }
            }

            // Bounds-check the read against the fixed length (static N) before
            // the element GEP — arrays previously skipped this (only slices
            // checked), so arr[i] could read past the array.
            LLVMValueRef arr_len = LLVMConstInt(LLVMInt64TypeInContext(codegen->context),
                                                (unsigned long long)base_type->data.array.length, 0);
            codegen_emit_bounds_check(codegen, idx64, arr_len, expr);

            // For arrays, generate GEP
            LLVMValueRef indices[] = {
                LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0),  // Array base
                idx64  // Array index
            };
            
            if (base_val->is_lvalue) {
                // Base is a pointer to array
                result = LLVMBuildGEP2(codegen->builder,
                                      codegen_type_to_llvm(codegen, base_type),
                                      base_val->llvm_value, indices, 2, "array_elem");
            } else {
                // Base is array value, need to create alloca
                LLVMValueRef array_alloca = codegen_create_alloca(codegen,
                                                                 codegen_type_to_llvm(codegen, base_type),
                                                                 "array_tmp");
                LLVMBuildStore(codegen->builder, base_val->llvm_value, array_alloca);
                result = LLVMBuildGEP2(codegen->builder,
                                      codegen_type_to_llvm(codegen, base_type),
                                      array_alloca, indices, 2, "array_elem");
            }
            break;
        }
        
        case TYPE_SLICE: {
            element_type = base_type->data.slice.element_type;

            // Slices are structs with { ptr, len, cap }. Extract the data
            // pointer (field 0) and the length (field 1).
            LLVMValueRef slice_ptr;
            LLVMValueRef slice_len;
            if (base_val->is_lvalue) {
                // Load the slice struct
                LLVMValueRef slice_val = LLVMBuildLoad2(codegen->builder,
                                                       codegen_type_to_llvm(codegen, base_type),
                                                       base_val->llvm_value, "slice_load");
                slice_ptr = LLVMBuildExtractValue(codegen->builder, slice_val, 0, "slice_ptr");
                slice_len = LLVMBuildExtractValue(codegen->builder, slice_val, 1, "slice_len");
            } else {
                slice_ptr = LLVMBuildExtractValue(codegen->builder, base_val->llvm_value, 0, "slice_ptr");
                slice_len = LLVMBuildExtractValue(codegen->builder, base_val->llvm_value, 1, "slice_len");
            }

            // P1-6: runtime bounds check before the element GEP. goo_bounds_check
            // panics ("bounds check failed") if index >= length, so out-of-range
            // access aborts instead of reading/writing past the buffer. The
            // comparison lives in the runtime fn, so no IR branching here.
            codegen_emit_bounds_check(codegen, idx64, slice_len, expr);

            // Index into the slice data
            result = LLVMBuildGEP2(codegen->builder,
                                  codegen_type_to_llvm(codegen, element_type),
                                  slice_ptr, &idx64, 1, "slice_elem");
            break;
        }
        
        case TYPE_STRING: {
            // Strings are like slices with byte elements
            element_type = type_checker_get_builtin(checker, TYPE_UINT8);
            
            // Extract the data pointer from string struct
            LLVMValueRef string_ptr;
            if (base_val->is_lvalue) {
                LLVMValueRef string_val = LLVMBuildLoad2(codegen->builder,
                                                        codegen_type_to_llvm(codegen, base_type),
                                                        base_val->llvm_value, "string_load");
                string_ptr = LLVMBuildExtractValue(codegen->builder, string_val, 0, "string_ptr");
            } else {
                string_ptr = LLVMBuildExtractValue(codegen->builder, base_val->llvm_value, 0, "string_ptr");
            }
            
            // Index into the string data
            result = LLVMBuildGEP2(codegen->builder,
                                  LLVMInt8TypeInContext(codegen->context),
                                  string_ptr, &idx64, 1, "string_char");
            break;
        }
        
        case TYPE_MAP: {
            // Unreachable: the map fast-path at the top of this function
            // handles all TYPE_MAP indexing and returns before the switch.
            codegen_error(codegen, expr->pos, "internal: map index reached switch fallthrough");
            value_info_free(base_val);
            value_info_free(index_val);
            return NULL;
        }
        
        case TYPE_POINTER: {
            // Pointer indexing (pointer arithmetic)
            element_type = base_type->data.pointer.pointee_type;
            
            LLVMValueRef ptr_val = base_val->llvm_value;
            if (base_val->is_lvalue) {
                ptr_val = LLVMBuildLoad2(codegen->builder,
                                       codegen_type_to_llvm(codegen, base_type),
                                       base_val->llvm_value, "ptr_load");
            }
            
            result = LLVMBuildGEP2(codegen->builder,
                                  codegen_type_to_llvm(codegen, element_type),
                                  ptr_val, &idx64, 1, "ptr_elem");
            break;
        }
        
        default:
            codegen_error(codegen, expr->pos, "Type cannot be indexed");
            value_info_free(base_val);
            value_info_free(index_val);
            return NULL;
    }
    
    // Create value info for the result
    ValueInfo* result_val = value_info_new(NULL, result, element_type);
    result_val->is_lvalue = 1;  // Indexing returns an lvalue
    
    value_info_free(base_val);
    value_info_free(index_val);
    
    return result_val;
#endif
}

// F5: `base[low:high]` slice/substring. Produces a fresh header value that
// shares the base's backing storage — no copy:
//   string -> goo_string { data+low,       high-low }
//   slice  -> goo_slice  { data+low*esize, high-low, cap-low }
// The element GEP scales `low` by the element size for slices (i8 for
// strings). Bounds are widened to i64 to match the size_t header fields.
// Bounds-checked via goo_slice_bounds_check before the new header is built:
// 0 <= low <= high <= max, where max is cap(base) for a slice (high may
// exceed len but not cap) and len(base) for a string (no cap field).
ValueInfo* codegen_generate_slice_index_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_SLICE_INDEX_EXPR) return NULL;

    SliceIndexExprNode* sl = (SliceIndexExprNode*)expr;

    ValueInfo* base_val = codegen_generate_expression(codegen, checker, sl->expr);
    if (!base_val) {
        codegen_error(codegen, expr->pos, "Failed to generate slice base");
        return NULL;
    }
    Type* base_type = base_val->goo_type;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(codegen->context);
    LLVMTypeRef struct_ty = codegen_type_to_llvm(codegen, base_type);

    // Load the base struct (string/slice) value if the base is an lvalue,
    // mirroring the index path.
    LLVMValueRef base_struct = base_val->llvm_value;
    if (base_val->is_lvalue) {
        base_struct = LLVMBuildLoad2(codegen->builder, struct_ty,
                                     base_val->llvm_value, "slice_base_load");
    }
    // Length is field 1 in both the string and slice headers — the default for
    // an omitted high bound (`s[low:]`).
    LLVMValueRef base_len = LLVMBuildExtractValue(codegen->builder, base_struct, 1, "base_len");

    // Bounds, widened to i64. An omitted low defaults to 0; an omitted high to
    // the length (open-ended slices `s[low:]`, `s[:high]`, `s[:]`).
    LLVMValueRef low64, high64;
    if (sl->low) {
        ValueInfo* lv = codegen_generate_expression(codegen, checker, sl->low);
        if (!lv) { value_info_free(base_val); return NULL; }
        low64 = lv->llvm_value;
        unsigned w = LLVMGetIntTypeWidth(LLVMTypeOf(low64));
        if (w < 64) low64 = LLVMBuildSExt(codegen->builder, low64, i64, "low64");
        else if (w > 64) low64 = LLVMBuildTrunc(codegen->builder, low64, i64, "low64");
        value_info_free(lv);
    } else {
        low64 = LLVMConstInt(i64, 0, 0);
    }
    if (sl->high) {
        ValueInfo* hv = codegen_generate_expression(codegen, checker, sl->high);
        if (!hv) { value_info_free(base_val); return NULL; }
        high64 = hv->llvm_value;
        unsigned w = LLVMGetIntTypeWidth(LLVMTypeOf(high64));
        if (w < 64) high64 = LLVMBuildSExt(codegen->builder, high64, i64, "high64");
        else if (w > 64) high64 = LLVMBuildTrunc(codegen->builder, high64, i64, "high64");
        value_info_free(hv);
    } else {
        high64 = base_len;
    }

    LLVMValueRef data_ptr = LLVMBuildExtractValue(codegen->builder, base_struct, 0, "base_data");
    LLVMValueRef new_len = LLVMBuildSub(codegen->builder, high64, low64, "new_len");
    LLVMValueRef result = LLVMGetUndef(struct_ty);

    // MAX for the bounds check: cap(base) for a slice (high may run up to
    // cap, past len — that's what lets a reslice recover previously-truncated
    // capacity), len(base) for a string (strings have no cap field at all).
    LLVMValueRef old_cap = NULL;
    LLVMValueRef max64 = base_len;
    if (base_type->kind != TYPE_STRING) {
        old_cap = LLVMBuildExtractValue(codegen->builder, base_struct, 2, "old_cap");
        max64 = old_cap;
    }
    {
        LLVMValueRef bc_fn = LLVMGetNamedFunction(codegen->module, "goo_slice_bounds_check");
        if (bc_fn) {
            LLVMValueRef file = LLVMBuildGlobalStringPtr(codegen->builder,
                expr->pos.filename ? expr->pos.filename : "<input>", "sbc_file");
            LLVMValueRef line = LLVMConstInt(LLVMInt32TypeInContext(codegen->context),
                                             (unsigned long long)expr->pos.line, 0);
            LLVMValueRef bc_args[5] = { low64, high64, max64, file, line };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(bc_fn), bc_fn, bc_args, 5, "");
        }
        // no symbol: bounds unguarded (best-effort), matching codegen_emit_bounds_check
    }

    if (base_type->kind == TYPE_STRING) {
        // Byte-addressed: new_data = data_ptr + low.
        LLVMValueRef new_data = LLVMBuildGEP2(codegen->builder,
                                              LLVMInt8TypeInContext(codegen->context),
                                              data_ptr, &low64, 1, "sub_data");
        result = LLVMBuildInsertValue(codegen->builder, result, new_data, 0, "str_data");
        result = LLVMBuildInsertValue(codegen->builder, result, new_len, 1, "str_len");
    } else {
        // TYPE_SLICE: element-scaled GEP; cap shrinks by low.
        Type* elem = base_type->data.slice.element_type;
        LLVMTypeRef elem_ty = codegen_type_to_llvm(codegen, elem);
        LLVMValueRef new_data = LLVMBuildGEP2(codegen->builder, elem_ty,
                                              data_ptr, &low64, 1, "resl_data");
        LLVMValueRef new_cap = LLVMBuildSub(codegen->builder, old_cap, low64, "new_cap");
        result = LLVMBuildInsertValue(codegen->builder, result, new_data, 0, "sl_data");
        result = LLVMBuildInsertValue(codegen->builder, result, new_len, 1, "sl_len");
        result = LLVMBuildInsertValue(codegen->builder, result, new_cap, 2, "sl_cap");
    }

    ValueInfo* out = value_info_new(NULL, result, base_type);
    out->is_lvalue = 0;  // a fresh header value (rvalue)
    value_info_free(base_val);
    return out;
#endif
}

// P3.6 (method values): `f := c.get` in non-call position. The checker has
// already stamped `expr->node_type` with the receiver-STRIPPED signature
// (type_check_selector_expr's value-position arm, expression_checker.c) —
// this builds the runtime VALUE for it: `{ bound_thunk, env }`, the same
// universal fat-pointer shape every func value uses (type_mapping.c's
// codegen_get_funcval_pair_type). `env` is a fresh goo_alloc'd heap cell
// holding the bound receiver, typed EXACTLY as the method's own receiver
// parameter — a struct COPY for a value receiver (later mutation of the
// source invisible through the bound value, Go copy semantics) or the
// pointer itself for a pointer receiver (mutation visible, aliased state).
//
// The four receiver-binding cases below mirror call_codegen.c's method-CALL
// receiver branch (~codegen_generate_call_expr's method fast path) exactly,
// so a bound method value observes the identical auto-&/auto-deref rules a
// direct call would; embedding-promoted methods are covered for free since
// the checker already rewrites a promoted selector's AST to name the owning
// type directly (embed_wrap_base) before codegen ever sees it — no
// promotion-hop walking is needed here (unlike call_codegen.c's `promo`,
// which exists only for the function-generics Tier B bounded-T fallback,
// irrelevant to this ordinary struct-method path).
static ValueInfo* codegen_generate_method_value(CodeGenerator* codegen, TypeChecker* checker,
                                                 ASTNode* expr, SelectorExprNode* selector,
                                                 Type* recv_static_type, Variable* method_var,
                                                 const char* mangled_name) {
    LLVMValueRef named_fn = LLVMGetNamedFunction(codegen->module, mangled_name);
    if (!named_fn) {
        codegen_error(codegen, expr->pos,
                      "internal: method '%s' not found in module", selector->selector);
        return NULL;
    }

    Type* method_fn_type = method_var->type;   // receiver spliced as params[0]
    Type* stripped_fn_type = expr->node_type;  // the checker already stripped it (P3.6)
    if (!stripped_fn_type || stripped_fn_type->kind != TYPE_FUNCTION ||
        method_fn_type->kind != TYPE_FUNCTION || method_fn_type->data.function.param_count == 0) {
        codegen_error(codegen, expr->pos,
                      "internal: method value missing resolved signature for '%s'",
                      selector->selector);
        return NULL;
    }

    Type* recv_param_type = method_fn_type->data.function.param_types[0];
    int ptr_recv = recv_param_type->kind == TYPE_POINTER;
    int recv_is_ptr_value = recv_static_type && recv_static_type->kind == TYPE_POINTER;

    LLVMValueRef thunk = codegen_get_method_bound_thunk(codegen, stripped_fn_type, method_fn_type,
                                                         named_fn, mangled_name);
    if (!thunk) {
        codegen_error(codegen, expr->pos,
                      "internal: failed to build bound thunk for method '%s'", selector->selector);
        return NULL;
    }

    LLVMTypeRef recv_llvm = codegen_type_to_llvm(codegen, recv_param_type);
    if (!recv_llvm) {
        codegen_error(codegen, expr->pos,
                      "cannot lower receiver type for method '%s'", selector->selector);
        return NULL;
    }

    // recv_val: a value of LLVM type recv_llvm — the exact bytes snapshotted
    // into the env cell below. Which of the four cases applies is decided by
    // (declared receiver kind) x (this call site's static receiver type),
    // same 2x2 the method-CALL path decides on (call_codegen.c).
    LLVMValueRef recv_val;
    if (ptr_recv && !recv_is_ptr_value) {
        // Auto-address-of: `c.ptrMethod`, c an addressable value. Binding a
        // pointer-receiver method demands the same addressability a direct
        // call would (P2.1 method-set rules) — reject cleanly otherwise,
        // matching call_codegen.c's "non-addressable value" diagnostic.
        ValueInfo* addr = codegen_emit_lvalue_address(codegen, checker, selector->expr);
        if (!addr || !addr->is_lvalue) {
            codegen_error(codegen, expr->pos,
                "cannot bind pointer-receiver method '%s' on non-addressable value",
                selector->selector);
            return NULL;
        }
        recv_val = addr->llvm_value;
    } else if (!ptr_recv && recv_is_ptr_value) {
        // Auto-deref: `p.valMethod`, p is *T — snapshot *p AT BIND TIME (a
        // later mutation of *p must not be visible through the bound value).
        ValueInfo* pv = codegen_generate_expression(codegen, checker, selector->expr);
        if (!pv) return NULL;
        LLVMValueRef ptr = pv->llvm_value;
        if (pv->is_lvalue && pv->goo_type) {
            LLVMTypeRef pt = codegen_type_to_llvm(codegen, pv->goo_type);
            if (pt) ptr = LLVMBuildLoad2(codegen->builder, pt, ptr, "mval_ptr_load");
        }
        value_info_free(pv);
        recv_val = LLVMBuildLoad2(codegen->builder, recv_llvm, ptr, "mval_recv_deref");
    } else {
        // Pointer receiver on an already-pointer value (bind the pointer
        // itself), or value receiver on a value (snapshot it). Either way:
        // generate the receiver expression and load through any lvalue
        // ADDRESS a chained selector/index receiver returns (an identifier
        // receiver is already auto-loaded to a value by
        // codegen_generate_identifier and needs no extra load) — mirrors
        // call_codegen.c's matching branch and its is_lvalue-load comment.
        ValueInfo* rv = codegen_generate_expression(codegen, checker, selector->expr);
        if (!rv) return NULL;
        recv_val = rv->llvm_value;
        if (rv->is_lvalue && rv->goo_type) {
            LLVMTypeRef rt = codegen_type_to_llvm(codegen, rv->goo_type);
            if (rt) recv_val = LLVMBuildLoad2(codegen->builder, rt, recv_val, "mval_recv_load");
        }
        value_info_free(rv);
    }

    // Snapshot into a fresh heap cell (goo_alloc — same allocator closures'
    // env construction uses, function_codegen.c's codegen_generate_func_lit)
    // sized to the receiver's own LLVM type: sizeof(T) for a value receiver
    // (the whole struct is copied in), sizeof(ptr) for a pointer receiver
    // (just the pointer). ALLOC_KIND_DEFAULT + a NULL alloc_site is the same
    // unconditional-heap choice every other env/box allocation in this
    // codebase makes; arena-eligibility for closure/method-value envs is out
    // of this task's scope.
    LLVMValueRef cell_size = LLVMSizeOf(recv_llvm);
    LLVMValueRef env_cell = codegen_emit_alloc(codegen, cell_size, ALLOC_KIND_DEFAULT, NULL);
    if (!env_cell) {
        codegen_error(codegen, expr->pos,
                      "internal: failed to allocate receiver cell for method '%s'",
                      selector->selector);
        return NULL;
    }
    LLVMBuildStore(codegen->builder, recv_val, env_cell);

    LLVMTypeRef pair_ty = codegen_get_funcval_pair_type(codegen);
    LLVMValueRef pair = LLVMConstNull(pair_ty);
    pair = LLVMBuildInsertValue(codegen->builder, pair, thunk, 0, "mval_thunk");
    pair = LLVMBuildInsertValue(codegen->builder, pair, env_cell, 1, "mval_env");
    return value_info_new(NULL, pair, stripped_fn_type);
}

ValueInfo* codegen_generate_selector_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_SELECTOR_EXPR) return NULL;

    SelectorExprNode* selector = (SelectorExprNode*)expr;

    // Package value-members (math.Pi, later os.Args): the base is a bare
    // package identifier, which has no value to generate — intercept
    // before the general base-expression path.
    if (selector->expr && selector->expr->type == AST_IDENTIFIER) {
        IdentifierNode* pkg = (IdentifierNode*)selector->expr;
        if (strcmp(pkg->name, "math") == 0 && strcmp(selector->selector, "Pi") == 0) {
            LLVMValueRef pi = LLVMConstReal(LLVMDoubleTypeInContext(codegen->context),
                                            3.14159265358979323846);
            return value_info_new(NULL, pi,
                                  type_checker_get_builtin(checker, TYPE_FLOAT64));
        }
        if (strcmp(pkg->name, "os") == 0 && strcmp(selector->selector, "Args") == 0) {
            // os.Args ([]string): the slice header is >16 bytes (SysV MEMORY
            // class, m12/ABI rule), so it can't cross the codegen<->C
            // boundary by value — mirrors strings.Split's goo_slice_t* out
            // pattern (call_codegen.c): spill an entry-block alloca, call
            // goo_os_args(&out), then load the populated header.
            Type* args_type = expr->node_type;
            if (!args_type || args_type->kind != TYPE_SLICE) {
                codegen_error(codegen, expr->pos, "os.Args: expected []string result");
                return NULL;
            }
            LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_os_args");
            if (!fn) {
                codegen_error(codegen, expr->pos, "goo_os_args not found in module");
                return NULL;
            }
            LLVMTypeRef slice_llvm = codegen_type_to_llvm(codegen, args_type);
            LLVMValueRef out = codegen_create_entry_alloca(codegen, slice_llvm, "os_args_out");
            LLVMValueRef call_args[] = { out };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn), fn, call_args, 1, "");
            LLVMValueRef slice_val = LLVMBuildLoad2(codegen->builder, slice_llvm, out, "os_args_slice");
            return value_info_new(NULL, slice_val, args_type);
        }
        // P4.6 (packages-C, C1): time.{Nanosecond,Microsecond,Millisecond,
        // Second} — Duration-typed constants, same math.Pi-style intercept
        // (there is no general codegen path for an arbitrary package-level
        // exported VALUE member; see that comment above). Unlike math.Pi,
        // reuse expr->node_type instead of re-deriving a builtin type: the
        // checker's generic package-export lookup (expression_checker.c)
        // already stamped the real Duration Type here (goo.c's
        // seed_time_package_exports); re-deriving a fresh builtin int64
        // instead would work too (same kind/width) but discards that Type's
        // name and owner_package for no benefit.
        if (strcmp(pkg->name, "time") == 0) {
            int64_t nanos = 0;
            int matched = 1;
            if (strcmp(selector->selector, "Nanosecond") == 0) nanos = 1LL;
            else if (strcmp(selector->selector, "Microsecond") == 0) nanos = 1000LL;
            else if (strcmp(selector->selector, "Millisecond") == 0) nanos = 1000000LL;
            else if (strcmp(selector->selector, "Second") == 0) nanos = 1000000000LL;
            else matched = 0;
            if (matched) {
                Type* duration_type = expr->node_type;
                if (!duration_type) {
                    codegen_error(codegen, expr->pos,
                                  "internal: time.%s missing resolved Duration type",
                                  selector->selector);
                    return NULL;
                }
                LLVMValueRef v = LLVMConstInt(LLVMInt64TypeInContext(codegen->context),
                                              (unsigned long long)nanos, 1);
                return value_info_new(NULL, v, duration_type);
            }
        }
    }

    // P3.6 (method values): decide field-vs-method BEFORE generating any
    // code for the base expression — a method receiver's codegen shape
    // depends on the method's declared receiver kind (see
    // codegen_generate_method_value's four-way branch above), which is a
    // DIFFERENT shape than the base_val generated below for field access.
    // Deciding first (and diverting to the method path without ever
    // touching base_val) keeps the base expression evaluated EXACTLY ONCE
    // either way — generating it here first and re-deriving a receiver
    // address/value again inside the method arm would double-evaluate any
    // receiver expression with a side effect (e.g. a call in the chain).
    // type_check_expression on an already-checked node is a pure type-cache
    // read, not a re-emission — the same pattern the method-CALL fast path
    // uses to get the receiver's type (call_codegen.c's
    // codegen_generate_call_expr, ~"Type* recv_type =
    // type_check_expression(checker, msel->expr);").
    Type* recv_static_type = type_check_expression(checker, selector->expr);
    if (!recv_static_type) {
        codegen_error(codegen, expr->pos, "Failed to resolve selector base type");
        return NULL;
    }
    Type* lookup_type = recv_static_type;
    if (lookup_type->kind == TYPE_POINTER && lookup_type->data.pointer.pointee_type &&
        lookup_type->data.pointer.pointee_type->kind == TYPE_STRUCT) {
        lookup_type = lookup_type->data.pointer.pointee_type;
    }
    int try_method = 0;
    if (lookup_type->kind == TYPE_STRUCT) {
        int is_field = 0;
        for (size_t i = 0; i < lookup_type->data.struct_type.field_count; i++) {
            if (strcmp(lookup_type->data.struct_type.fields[i].name, selector->selector) == 0) {
                is_field = 1;
                break;
            }
        }
        try_method = !is_field;
    } else if (lookup_type->name && lookup_type->kind != TYPE_PACKAGE &&
               lookup_type->kind != TYPE_INTERFACE && !type_is_error(lookup_type)) {
        // Named non-struct receiver (`type MyInt int` — named_int_method_
        // probe territory): no fields exist to shadow the name, so any
        // selector the checker accepted on it IS a method — mirror the
        // checker's named non-struct arm (type_check_selector_expr). The
        // three excludes are shapes whose ->name would false-positive a
        // mangled lookup while their selectors resolve through entirely
        // different mechanisms: package members via the math.Pi/os.Args
        // intercepts above and call_codegen.c's package arms, interface
        // methods via vtable dispatch (no bound-thunk mechanism in v1), and
        // the error handle's `.Error` via its checker special-case (typed
        // string, never a method value).
        try_method = 1;
    }
    if (try_method) {
        const char* tn = type_receiver_name(lookup_type);
        char* mangled = tn ? type_method_mangled_name(tn, selector->selector) : NULL;
        // P4.3: lookup_type may be a package-owned receiver type — its
        // method Variable only lives in that package's exports scope (see
        // type_checker_lookup_method's doc comment).
        Variable* mvar = mangled
            ? type_checker_lookup_method(checker, lookup_type, selector->selector, mangled)
            : NULL;
        if (mvar && mvar->type && mvar->type->kind == TYPE_FUNCTION) {
            // P4.3: package-owned receiver methods are DEFINED under a
            // goo_pkg__<pkg>__ prefixed symbol (function_codegen.c). Pass
            // that prefixed name — not the bare one — into
            // codegen_generate_method_value: it both looks up the
            // DEFINITION under this name (LLVMGetNamedFunction) and derives
            // the bound thunk's own cache key from it
            // (codegen_get_method_bound_thunk's "<mangled_name>.
            // __bound_thunk"). Using the bare name here would let a
            // same-named main-package method's thunk collide with this
            // one under an identical bare cache key.
            char* emit_name = mangled;
            char* pkg_emit_name = NULL;
            Package* owner_pkg = type_receiver_owner_package(lookup_type);
            if (owner_pkg) {
                pkg_emit_name = codegen_pkg_mangled_symbol(owner_pkg->name, mangled);
                if (!pkg_emit_name) {
                    // Review-fix (CRITICAL hardening): never degrade a
                    // package-owned bind to the bare name — a same-named
                    // main method would hijack it. Fail cleanly instead.
                    codegen_error(codegen, expr->pos,
                                  "internal: cannot mangle package method symbol for '%s'",
                                  selector->selector);
                    free(mangled);
                    return NULL;
                }
                emit_name = pkg_emit_name;
            }
            ValueInfo* mv = codegen_generate_method_value(codegen, checker, expr, selector,
                                                           recv_static_type, mvar, emit_name);
            free(pkg_emit_name);
            free(mangled);
            return mv;
        }
        free(mangled);
        // Not a resolvable method: fall through to the existing code below,
        // which re-derives the base type via base_val and reports its own
        // "Field not found"/"Selector can only be applied to struct types"
        // diagnostics unchanged (an embedded/promoted name was already
        // rewritten into a direct member access by the checker's
        // embed_wrap_base before codegen ever saw this AST, so it never
        // reaches here).
    }

    // Generate code for the base expression
    ValueInfo* base_val = codegen_generate_expression(codegen, checker, selector->expr);
    if (!base_val) {
        codegen_error(codegen, expr->pos, "Failed to generate base expression for selector");
        return NULL;
    }

    // Get the type of the base expression
    Type* base_type = base_val->goo_type;
    
    // Handle pointer to struct: resolve to the struct's address, then GEP.
    // Two shapes arrive here:
    //   - lvalue: llvm_value is the address of the POINTER SLOT (a chained
    //     selector like o.P, or any pointer field) — load the pointer out
    //     of the slot; the loaded value is the struct's address.
    //   - rvalue: llvm_value IS the pointer (an auto-loaded identifier like
    //     p) — it is already the struct's address; no load.
    // Either way the result is the struct's address, so mark it lvalue and
    // let the GEP fork below index the real struct storage directly. (The
    // old code had this inverted — it skipped the load for lvalues, GEPing
    // into the pointer slot itself and reading garbage, and for rvalues
    // loaded the whole struct and spilled it to a temp copy.)
    if (base_type->kind == TYPE_POINTER && base_type->data.pointer.pointee_type->kind == TYPE_STRUCT) {
        if (base_val->is_lvalue) {
            base_val->llvm_value = LLVMBuildLoad2(codegen->builder,
                                                  LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),
                                                  base_val->llvm_value, "struct_ptr");
        }
        base_type = base_type->data.pointer.pointee_type;
        base_val->is_lvalue = 1;
    }
    
    if (base_type->kind != TYPE_STRUCT) {
        codegen_error(codegen, expr->pos, "Selector can only be applied to struct types");
        value_info_free(base_val);
        return NULL;
    }
    
    // Find the field in the struct
    int field_index = -1;
    StructField* field = NULL;
    for (size_t i = 0; i < base_type->data.struct_type.field_count; i++) {
        if (strcmp(base_type->data.struct_type.fields[i].name, selector->selector) == 0) {
            field_index = (int)i;
            field = &base_type->data.struct_type.fields[i];
            break;
        }
    }
    
    if (field_index == -1) {
        codegen_error(codegen, expr->pos, "Field '%s' not found in struct", selector->selector);
        value_info_free(base_val);
        return NULL;
    }
    
    // Generate GEP instruction to get pointer to field
    LLVMValueRef indices[] = {
        LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0),  // Deref struct pointer
        LLVMConstInt(LLVMInt32TypeInContext(codegen->context), field_index, 0)  // Field index
    };
    
    LLVMValueRef field_ptr;
    if (base_val->is_lvalue) {
        // Base is already a pointer to the struct
        field_ptr = LLVMBuildGEP2(codegen->builder, 
                                 codegen_type_to_llvm(codegen, base_type),
                                 base_val->llvm_value, indices, 2, selector->selector);
    } else {
        // Base is a struct value, need to create alloca first
        LLVMValueRef struct_alloca = codegen_create_alloca(codegen, 
                                                          codegen_type_to_llvm(codegen, base_type), 
                                                          "struct_tmp");
        LLVMBuildStore(codegen->builder, base_val->llvm_value, struct_alloca);
        field_ptr = LLVMBuildGEP2(codegen->builder, 
                                 codegen_type_to_llvm(codegen, base_type),
                                 struct_alloca, indices, 2, selector->selector);
    }
    
    // Create value info for the field
    ValueInfo* field_val = value_info_new(selector->selector, field_ptr, field->type);
    field_val->is_lvalue = 1;  // Field access returns an lvalue
    
    value_info_free(base_val);
    return field_val;
#endif
}
// codegen_build_struct_value builds an aggregate LLVMValueRef for a struct
// literal, inserting each field into the zero struct value via InsertValue.
// Extracted from codegen_generate_struct_lit so both the plain struct path
// and the enum-variant payload path can reuse it (DRY).
// Returns the aggregate on success, NULL on error.
static LLVMValueRef codegen_build_struct_value(CodeGenerator* codegen, TypeChecker* checker,
                                               StructLiteralNode* lit, Type* struct_type) {
    LLVMTypeRef llvm_struct = codegen_type_to_llvm(codegen, struct_type);
    if (!llvm_struct) return NULL;

    StructField* fields = struct_type->data.struct_type.fields;
    size_t decl_count = struct_type->data.struct_type.field_count;

    LLVMValueRef agg = LLVMConstNull(llvm_struct);
    size_t i = 0;
    for (ASTNode* v = lit->field_values; v; v = v->next, i++) {
        // Declared index: positional inits map 1:1; keyed inits resolve
        // by name (type check already guaranteed the name exists).
        size_t field_index = i;
        if (lit->is_keyed) {
            for (field_index = 0; field_index < decl_count; field_index++) {
                if (fields[field_index].name &&
                    strcmp(fields[field_index].name, lit->field_names[i]) == 0) break;
            }
            if (field_index == decl_count) {
                codegen_error(codegen, v->pos, "Field '%s' not found in struct '%s'",
                              lit->field_names[i], lit->type_name);
                return NULL;
            }
        }

        Type* field_type = fields[field_index].type;

        // P2-4: a `?T` field initialized with the nil literal builds the field's
        // null-nullable directly, mirroring the call-arg nullable path. Without
        // this the raw nil would be InsertValue'd into the {i1,T} slot.
        // P2.2 option A widened this to also cover a BARE (non-?T) pointer/
        // slice/map/channel/function field (e.g. `Node{next: nil}` with
        // `next *Node`): without the widened gate, the fallthrough
        // codegen_generate_expression call below would evaluate nil with no
        // expected-type context and, for a slice/function field, build a
        // scalar null pointer where an InsertValue expects an aggregate —
        // an LLVM verifier failure, not just a wrong value.
        if (field_type &&
            (field_type->kind == TYPE_NULLABLE || type_is_nilable_ref_kind(field_type)) &&
            v->type == AST_LITERAL &&
            ((LiteralNode*)v)->literal_type == TOKEN_NIL) {
            ValueInfo* nil_val = codegen_generate_null_literal(codegen, checker, field_type);
            if (!nil_val) {
                codegen_error(codegen, v->pos, "Failed to generate nil struct literal field");
                return NULL;
            }
            agg = LLVMBuildInsertValue(codegen->builder, agg, nil_val->llvm_value,
                                       (unsigned)field_index,
                                       fields[field_index].name ? fields[field_index].name : "field");
            value_info_free(nil_val);
            continue;
        }

        ValueInfo* val = codegen_generate_expression(codegen, checker, v);
        if (!val) {
            codegen_error(codegen, v->pos, "Failed to generate struct literal field");
            return NULL;
        }
        // Selector/index lvalues carry the field address — load the value.
        // A NULL lowering here must fail loudly: skipping the load would
        // InsertValue the address instead of the field value.
        if (val->is_lvalue && val->goo_type) {
            LLVMTypeRef vt = codegen_type_to_llvm(codegen, val->goo_type);
            if (!vt) {
                codegen_error(codegen, v->pos,
                              "Failed to lower type of struct literal field value");
                value_info_free(val);
                return NULL;
            }
            val->llvm_value = LLVMBuildLoad2(codegen->builder, vt, val->llvm_value, "fieldval");
            val->is_lvalue = 0;
        }

        // P2-4: auto-wrap a bare `T` value into a `?T` field's {i1,T} struct.
        // A value that is already nullable (e.g. another `?int`) is left as-is.
        // After wrapping, the integer-width block below is a no-op (the value is
        // now an aggregate, not an integer).
        if (field_type && field_type->kind == TYPE_NULLABLE &&
            val->goo_type && val->goo_type->kind != TYPE_NULLABLE) {
            LLVMTypeRef nullable_llvm = codegen_type_to_llvm(codegen, field_type);
            if (nullable_llvm) {
                val->llvm_value = codegen_create_nullable_with_value(
                    codegen, nullable_llvm, val->llvm_value, val->goo_type);
                val->goo_type = field_type;
            }
        }

        // Box a concrete implementer into an interface-typed field's
        // {vtable, data} value (mirrors the nullable wrap above and the
        // assignment/call-arg boxing). interface→interface needs no box.
        if (field_type && field_type->kind == TYPE_INTERFACE &&
            val->goo_type && val->goo_type->kind != TYPE_INTERFACE) {
            LLVMValueRef boxed = codegen_interface_box(codegen, checker, field_type,
                                                       val->goo_type, val->llvm_value);
            if (!boxed) {
                codegen_error(codegen, v->pos,
                              "failed to box value into interface field");
                value_info_free(val);
                return NULL;
            }
            val->llvm_value = boxed;
            val->goo_type = field_type;
        }
        // Widen or narrow the field value to match the declared LLVM field
        // type before InsertValue.  Without this, an i32 integer literal (the
        // compiler's default width) stored into an int64 struct field produces
        // an "insertvalue operand type mismatch" that fails `opt --passes=verify`.
        // Mirrors M4's fix in codegen_create_nullable_with_value.  Gate to
        // integer-kind mismatches only; structs and matching widths are
        // untouched.  Goo integers are signed, so widening uses SExt.
        {
            LLVMTypeRef expected_ty = LLVMStructGetTypeAtIndex(llvm_struct, (unsigned)field_index);
            LLVMTypeRef actual_ty   = LLVMTypeOf(val->llvm_value);
            if (LLVMGetTypeKind(actual_ty)   == LLVMIntegerTypeKind &&
                LLVMGetTypeKind(expected_ty) == LLVMIntegerTypeKind) {
                unsigned from_bits = LLVMGetIntTypeWidth(actual_ty);
                unsigned to_bits   = LLVMGetIntTypeWidth(expected_ty);
                if (from_bits < to_bits)
                    val->llvm_value = LLVMBuildSExt(codegen->builder, val->llvm_value,
                                                    expected_ty, "field_sext");
                else if (from_bits > to_bits)
                    val->llvm_value = LLVMBuildTrunc(codegen->builder, val->llvm_value,
                                                     expected_ty, "field_trunc");
            }
        }
        agg = LLVMBuildInsertValue(codegen->builder, agg, val->llvm_value,
                                   (unsigned)field_index,
                                   fields[field_index].name ? fields[field_index].name : "field");
        value_info_free(val);
    }

    return agg;
}

// `Point{x: 3, y: 4}` / `Point{3, 4}` — build the struct value as an
// rvalue aggregate: start from the zero value (so omitted keyed fields
// get Go zero-value semantics for free, matching the zero-initializing
// alloca `var p Point` already gets) and InsertValue each provided
// field at its declared index. Selector codegen already handles rvalue
// struct bases by spilling to a temporary alloca.
//
// Also handles enum variant construction: `Circle{radius: 5}` when the
// Forward declaration: defined after codegen_generate_slice_lit, used here
// for named-slice composite literal lowering (TYPE_SLICE via struct literal),
// and (Task 2) from call_codegen.c for variadic call-site arg packing — see
// its non-static prototype in codegen.h.
#if LLVM_AVAILABLE
ValueInfo* codegen_build_slice_from_elems(CodeGenerator* codegen,
                                           TypeChecker* checker,
                                           ASTNode* first_elem,
                                           Type* slice_type,
                                           Position pos);
#endif

// declared type is a TYPE_ENUM. Builds the payload aggregate against the
// variant's payload TYPE_STRUCT, then stores {tag, payload} through a
// stack alloca and loads the whole enum value.
ValueInfo* codegen_generate_struct_lit(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_STRUCT_LITERAL) return NULL;

    StructLiteralNode* lit = (StructLiteralNode*)expr;
    Type* struct_type = expr->node_type;
    if (!struct_type) {
        codegen_error(codegen, expr->pos,
                      "Struct literal '%s' missing type info from type check",
                      lit->type_name);
        return NULL;
    }

    // Enum variant construction: `Circle{radius: 5}` where the declared type
    // is a TYPE_ENUM. Find the matching variant, build its payload, then
    // write {tag, payload} into an alloca and load the resulting enum value.
    if (struct_type->kind == TYPE_ENUM) {
        EnumVariant* variant = NULL;
        for (size_t i = 0; i < struct_type->data.enum_type.variant_count; i++) {
            if (strcmp(struct_type->data.enum_type.variants[i].name, lit->type_name) == 0) {
                variant = &struct_type->data.enum_type.variants[i];
                break;
            }
        }
        if (!variant) {
            codegen_error(codegen, expr->pos, "No variant '%s' in enum", lit->type_name);
            return NULL;
        }

        LLVMTypeRef enum_ty = codegen_type_to_llvm(codegen, struct_type);
        LLVMTypeRef payload_ty = codegen_type_to_llvm(codegen, variant->payload);
        if (!enum_ty || !payload_ty) {
            codegen_error(codegen, expr->pos,
                          "Failed to lower enum or payload type for variant '%s'",
                          lit->type_name);
            return NULL;
        }

        // Build the payload aggregate via the shared helper.
        LLVMValueRef payload_val =
            codegen_build_struct_value(codegen, checker, lit, variant->payload);
        if (!payload_val) return NULL;

        // alloca the whole enum; store tag into field 0; bitcast field-1
        // slot to payload-struct-pointer; store payload; load the enum.
        LLVMValueRef tmp = codegen_create_alloca(codegen, enum_ty, "enum_tmp");
        LLVMValueRef tag_ptr =
            LLVMBuildStructGEP2(codegen->builder, enum_ty, tmp, 0, "tag_ptr");
        LLVMBuildStore(codegen->builder,
            LLVMConstInt(LLVMInt32TypeInContext(codegen->context),
                         (unsigned long long)variant->tag, 0),
            tag_ptr);
        LLVMValueRef pslot =
            LLVMBuildStructGEP2(codegen->builder, enum_ty, tmp, 1, "payload_slot");
        LLVMValueRef pcast =
            LLVMBuildBitCast(codegen->builder, pslot,
                             LLVMPointerType(payload_ty, 0), "payload_cast");
        LLVMBuildStore(codegen->builder, payload_val, pcast);
        LLVMValueRef loaded =
            LLVMBuildLoad2(codegen->builder, enum_ty, tmp, "enum_val");
        return value_info_new(NULL, loaded, struct_type);
    }

    // Named slice composite literal: `type IntSlice []int; IntSlice{3, 1, 2}`
    // field_values holds the elements (parallel to StructLiteralNode->field_names
    // but without names for positional form). Lower via the shared slice helper.
    if (struct_type->kind == TYPE_SLICE) {
        return codegen_build_slice_from_elems(codegen, checker,
                                              lit->field_values, struct_type, expr->pos);
    }

    if (struct_type->kind != TYPE_STRUCT) {
        codegen_error(codegen, expr->pos,
                      "Struct literal '%s' missing type info from type check",
                      lit->type_name);
        return NULL;
    }

    LLVMTypeRef llvm_struct = codegen_type_to_llvm(codegen, struct_type);
    if (!llvm_struct) {
        codegen_error(codegen, expr->pos,
                      "Failed to lower struct type '%s'", lit->type_name);
        return NULL;
    }

    LLVMValueRef agg = codegen_build_struct_value(codegen, checker, lit, struct_type);
    if (!agg) return NULL;

    return value_info_new(NULL, agg, struct_type);
#endif
}
// codegen_generate_match lowers a `match expr { case Variant{fields}: body }` to
// LLVM IR: load the discriminant tag, switch on it, in each arm bind the
// positional payload fields as named locals, emit the body, and branch to a
// merge block. Statement-style: no result value is returned.
//
// Scope notes:
//   - The type-checker already ran scope_push/pop per arm during type-check.
//     During codegen we replicate that so any re-entrant type-checking (e.g.
//     from binary-expr lowering) can resolve arm-local names.
//   - We also snapshot the value-table high-water mark before each arm
//     (vscope_enter) and truncate after (vscope_exit), so a binding from
//     arm N is not visible during arm N+1.
ValueInfo* codegen_generate_match(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_MATCH_EXPR) return NULL;

    MatchExprNode* m = (MatchExprNode*)expr;

    // Generate the scrutinee. `match *e` yields a loaded value (not a pointer).
    // We need the enum in memory to GEP into the tag and payload slots.
    ValueInfo* scrut = codegen_generate_expression(codegen, checker, m->expr);
    if (!scrut) return NULL;

    Type* enum_type = scrut->goo_type;
    if (!enum_type || enum_type->kind != TYPE_ENUM) {
        codegen_error(codegen, expr->pos, "match scrutinee is not an enum");
        value_info_free(scrut);
        return NULL;
    }

    LLVMTypeRef enum_ty = codegen_type_to_llvm(codegen, enum_type);
    LLVMValueRef scrut_ptr;
    if (scrut->is_lvalue) {
        // Scrutinee is already a pointer (lvalue) — use it directly.
        scrut_ptr = scrut->llvm_value;
    } else {
        // Scrutinee is a value (e.g. result of a pointer deref). Spill to
        // a temporary alloca so we can GEP into the tag and payload fields.
        scrut_ptr = codegen_create_alloca(codegen, enum_ty, "match_scrut");
        LLVMBuildStore(codegen->builder, scrut->llvm_value, scrut_ptr);
    }
    value_info_free(scrut);

    // Load the tag (field 0) for the switch dispatch.
    LLVMValueRef tag_ptr = LLVMBuildStructGEP2(codegen->builder, enum_ty,
                                               scrut_ptr, 0, "tag_ptr");
    LLVMValueRef tag = LLVMBuildLoad2(codegen->builder,
                                      LLVMInt32TypeInContext(codegen->context),
                                      tag_ptr, "tag");

    LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(codegen->builder));
    LLVMBasicBlockRef merge = LLVMAppendBasicBlockInContext(codegen->context, fn, "match_end");

    // Pre-count non-wildcard arms and detect the wildcard arm.
    unsigned narms = 0;
    LLVMBasicBlockRef default_bb = merge;  // switch falls to merge if no wildcard
    for (ASTNode* c = m->cases; c; c = c->next) {
        PatternNode* p = (PatternNode*)((MatchCaseNode*)c)->pattern;
        if (p->pattern_type == PATTERN_WILDCARD) {
            default_bb = LLVMAppendBasicBlockInContext(codegen->context, fn, "match_default");
        } else {
            narms++;
        }
    }

    LLVMValueRef sw = LLVMBuildSwitch(codegen->builder, tag, default_bb, narms);

    // A failed guard branches here: the wildcard arm if present, else the merge
    // block (which exits the match).  Resolved once, before the per-arm loop, so
    // every arm sees the same target regardless of iteration order.
    LLVMBasicBlockRef guard_fallback_bb = default_bb;

    for (ASTNode* c = m->cases; c; c = c->next) {
        MatchCaseNode* mc = (MatchCaseNode*)c;
        PatternNode* p = (PatternNode*)mc->pattern;

        // Snapshot value-table depth so arm-local bindings don't escape.
        size_t pre_arm_vt_size = vscope_enter(codegen);
        scope_push(checker);

        LLVMBasicBlockRef arm;
        if (p->pattern_type == PATTERN_WILDCARD) {
            arm = default_bb;
        } else {
            // Resolve the variant by name and get its tag.
            const char* vn = p->data.destructure.type_name;
            EnumVariant* variant = NULL;
            for (size_t i = 0; i < enum_type->data.enum_type.variant_count; i++) {
                if (strcmp(enum_type->data.enum_type.variants[i].name, vn) == 0) {
                    variant = &enum_type->data.enum_type.variants[i];
                    break;
                }
            }
            if (!variant) {
                codegen_error(codegen, c->pos,
                    "match: variant '%s' not found in enum", vn);
                scope_pop(checker);
                return NULL;
            }

            arm = LLVMAppendBasicBlockInContext(codegen->context, fn, vn);
            LLVMAddCase(sw,
                LLVMConstInt(LLVMInt32TypeInContext(codegen->context),
                             (unsigned long long)variant->tag, 0),
                arm);
            LLVMPositionBuilderAtEnd(codegen->builder, arm);

            // Bind positional payload fields as arm locals.
            // Payload is stored in field 1 of the enum, bitcast to the
            // variant's struct type pointer so individual fields are accessible.
            if (variant->payload && p->data.destructure.fields) {
                LLVMTypeRef payload_ty = codegen_type_to_llvm(codegen, variant->payload);
                LLVMValueRef pslot = LLVMBuildStructGEP2(codegen->builder, enum_ty,
                                                          scrut_ptr, 1, "pslot");
                LLVMValueRef pptr = LLVMBuildBitCast(codegen->builder, pslot,
                                                      LLVMPointerType(payload_ty, 0), "pptr");
                size_t fi = 0;
                for (ASTNode* b = p->data.destructure.fields; b; b = b->next, fi++) {
                    if (b->type != AST_IDENTIFIER) continue;
                    IdentifierNode* bind = (IdentifierNode*)b;
                    if (strcmp(bind->name, "_") == 0) continue;

                    StructField* f = &variant->payload->data.struct_type.fields[fi];
                    LLVMTypeRef field_ty = codegen_type_to_llvm(codegen, f->type);
                    LLVMValueRef fptr = LLVMBuildStructGEP2(codegen->builder, payload_ty,
                                                             pptr, (unsigned)fi, bind->name);
                    // Load the field value and expose it as an lvalue alloca so
                    // that assignments to it in the arm body work correctly.
                    LLVMValueRef fval = LLVMBuildLoad2(codegen->builder,
                                                        field_ty, fptr, bind->name);
                    LLVMValueRef slot = codegen_alloc_local(codegen, field_ty, bind->name);
                    LLVMBuildStore(codegen->builder, fval, slot);

                    ValueInfo* vi = value_info_new(bind->name, slot, f->type);
                    vi->is_lvalue = 1;
                    vi->is_initialized = 1;
                    vscope_add(codegen, vi);

                    // Mirror into the type-checker scope so any re-entrant
                    // type_check_* calls inside the arm body can resolve the name.
                    Variable* tv = variable_new(bind->name, f->type, c->pos);
                    if (tv) {
                        tv->is_initialized = 1;
                        scope_add_variable(checker->current_scope, tv);
                    }
                }
            }
        }

        // Position the builder into the arm block (wildcard uses default_bb).
        LLVMPositionBuilderAtEnd(codegen->builder, arm);

        // M3: evaluate the guard; on false, branch to the fallback block
        // (the wildcard arm, or the merge block when there is none).
        // Payload fields are already in scope above, so the guard expression
        // can reference bound names like `n`.  Guardless arms are unchanged.
        if (mc->guard) {
            ValueInfo* g = codegen_generate_expression(
                codegen, checker, ((GuardConditionNode*)mc->guard)->condition);
            if (g && g->llvm_value) {
                // For the wildcard arm, a false guard must fall to merge (not
                // back to itself via default_bb, which would loop infinitely).
                LLVMBasicBlockRef arm_fallback =
                    (p->pattern_type == PATTERN_WILDCARD) ? merge : guard_fallback_bb;
                LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(
                    codegen->context, fn, "guard_body");
                // Auto-load lvalue guard conditions (bare field selectors) —
                // same as the if/for paths.
                LLVMValueRef cond_val = g->llvm_value;
                if (g->is_lvalue && g->goo_type) {
                    LLVMTypeRef ct = codegen_type_to_llvm(codegen, g->goo_type);
                    if (ct) cond_val = LLVMBuildLoad2(codegen->builder, ct, cond_val, "cond_load");
                }
                LLVMBuildCondBr(codegen->builder, cond_val,
                                body_bb, arm_fallback);
                LLVMPositionBuilderAtEnd(codegen->builder, body_bb);
                value_info_free(g);
            } else {
                if (g) value_info_free(g);
                codegen_error(codegen, mc->guard->pos,
                    "match guard condition failed to generate");
                scope_pop(checker);
                return NULL;
            }
        }

        // Emit arm body statements.
        for (ASTNode* s = mc->body; s; s = s->next)
            codegen_generate_statement(codegen, checker, s);

        // Fall through to merge unless the arm already terminated (e.g. return).
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)))
            LLVMBuildBr(codegen->builder, merge);

        // Restore type-checker scope and codegen value table.
        scope_pop(checker);
        vscope_exit(codegen, pre_arm_vt_size);
    }

    LLVMPositionBuilderAtEnd(codegen->builder, merge);
    return value_info_new(NULL, NULL, type_checker_get_builtin(checker, TYPE_VOID));
#endif
}

// Coerce a slice-literal element value to the declared element LLVM type
// before it is stored into the backing array. Mirrors the struct-literal field
// coercion: int->int widens with SExt or ZExt / narrows with Trunc, int->float
// uses SIToFP or UIToFP, float->float uses FPExt/FPTrunc. Matching widths and
// other kinds (bool, string) pass through unchanged. This is what lets the
// general []T{} case lower (int64/uint/float32/float64), not just the
// natural-width i32/string forms.
//
// Widening MUST follow the SOURCE type's signedness, not an assumed-signed
// default: a non-constant unsigned element (`x := uint8(200); []int64{x}`)
// zero-extends, not sign-extends — SExt on 200 (0xC8) would sign-bit-smear
// it into -56. Same rule as the var-decl initializer rebuild
// (function_codegen.c) and the constant-rebuild fixes in this file (the
// array fast-path and the slice collection-loop constant normalization
// above) — this completes the signedness family for the builder element
// path, where the caller passes src_signed from the still-live source
// ValueInfo/flag (see codegen_build_slice_from_elems's elem_signed[] and
// codegen_generate_array_lit's ev->goo_type).
//
// This is a thin wrapper (keeps the static signature callers already use)
// around the shared codegen_coerce_to_type helper in codegen.c, which is the
// single home for this rule now that six call sites needed the identical
// int/float arms.
static LLVMValueRef slice_coerce_elem(CodeGenerator* codegen, LLVMValueRef v, LLVMTypeRef to, int src_signed) {
    return codegen_coerce_to_type(codegen, v, src_signed, to);
}

// Shared core: build a slice struct { ptr, i64 len, i64 cap } from a
// next-chained ASTNode* element list and a resolved TYPE_SLICE type.
// Called by both codegen_generate_slice_lit (with SliceLitNode->elements)
// and codegen_generate_struct_lit (with StructLiteralNode->field_values)
// so the two surface forms share a single lowering path (DRY).
#if LLVM_AVAILABLE
ValueInfo* codegen_build_slice_from_elems(CodeGenerator* codegen,
                                           TypeChecker* checker,
                                           ASTNode* first_elem,
                                           Type* slice_type,
                                           Position pos) {
    Type* elem_type = slice_type->data.slice.element_type;
    LLVMTypeRef llvm_elem = codegen_type_to_llvm(codegen, elem_type);
    if (!llvm_elem) {
        codegen_error(codegen, pos, "Cannot lower slice element type");
        return NULL;
    }

    size_t count = 0;
    for (ASTNode* e = first_elem; e; e = e->next) count++;

    int elem_is_nullable = (elem_type && elem_type->kind == TYPE_NULLABLE);
    // P2.2 option A: a bare (non-?T) pointer/slice/map/channel/function
    // element also needs the expected-type-aware nil intercept below (e.g.
    // `[]*Node{a, nil, b}`) — kept as a SEPARATE flag from elem_is_nullable
    // (used below for the ?T-specific auto-wrap of a non-nil element, which
    // must NOT fire for these bare kinds).
    int elem_is_nilable_bare = type_is_nilable_ref_kind(elem_type);

    LLVMValueRef* elem_vals = count ? calloc(count, sizeof(LLVMValueRef)) : NULL;
    // Per-element source signedness, captured at collection time (this loop is
    // the only place the source type — v->goo_type — is still in scope; it is
    // freed via value_info_free(v) before the builder coercion loop below
    // runs). Consumed by slice_coerce_elem via the :1065-ish call so a
    // non-constant unsigned element (uint8 200) zero-extends instead of
    // sign-extending into a negative value. See slice_coerce_elem's comment.
    int* elem_signed = count ? calloc(count, sizeof(int)) : NULL;
    size_t idx = 0;
    for (ASTNode* e = first_elem; e; e = e->next, idx++) {
        if ((elem_is_nullable || elem_is_nilable_bare) && e->type == AST_LITERAL &&
            ((LiteralNode*)e)->literal_type == TOKEN_NIL) {
            ValueInfo* nil_val = codegen_generate_null_literal(codegen, checker, elem_type);
            if (!nil_val) { free(elem_vals); free(elem_signed); return NULL; }
            elem_vals[idx] = nil_val->llvm_value;
            value_info_free(nil_val);
            continue;
        }

        ValueInfo* v = codegen_generate_expression(codegen, checker, e);
        if (!v) { free(elem_vals); free(elem_signed); return NULL; }

        if (elem_is_nullable && v->goo_type && v->goo_type->kind != TYPE_NULLABLE) {
            if (v->is_lvalue) {
                LLVMTypeRef vt = codegen_type_to_llvm(codegen, v->goo_type);
                if (vt) {
                    v->llvm_value = LLVMBuildLoad2(codegen->builder, vt, v->llvm_value, "elemld");
                    v->is_lvalue = 0;
                }
            }
            v->llvm_value = codegen_create_nullable_with_value(
                codegen, llvm_elem, v->llvm_value, v->goo_type);
        }

        if (elem_type && elem_type->kind == TYPE_INTERFACE &&
            v->goo_type && v->goo_type->kind != TYPE_INTERFACE) {
            if (v->is_lvalue) {
                LLVMTypeRef vt = codegen_type_to_llvm(codegen, v->goo_type);
                if (vt) {
                    v->llvm_value = LLVMBuildLoad2(codegen->builder, vt, v->llvm_value, "elemld");
                    v->is_lvalue = 0;
                }
            }
            LLVMValueRef boxed = codegen_interface_box(codegen, checker, elem_type,
                                                       v->goo_type, v->llvm_value);
            if (!boxed) {
                codegen_error(codegen, e->pos,
                              "failed to box value into interface slice element");
                value_info_free(v);
                free(elem_vals);
                free(elem_signed);
                return NULL;
            }
            v->llvm_value = boxed;
        }

        // Load a scalar lvalue element (e.g. `xs[0]`, a slice/array index)
        // before use. The nullable/interface branches above already load
        // internally when THEY fire; this is the general fallthrough for a
        // plain element that used to fall straight through to the
        // elem_vals[idx] assignment below with its address still in
        // llvm_value — `[]int{xs[0], xs[1]}` silently packed each element's
        // storage ADDRESS instead of its value (a pointer bit pattern
        // reinterpreted as an int). Task 2 (variadic call-site packing)
        // reuses this helper for `sum(nums[0], nums[1])`-style trailing
        // args, which is what surfaced this — but the bug already existed
        // for ordinary slice literals. Mirrors the identical general-
        // fallback load in call_codegen.c's user-call argument loop.
        if (v->is_lvalue && v->goo_type) {
            LLVMTypeRef vt = codegen_type_to_llvm(codegen, v->goo_type);
            if (vt) {
                v->llvm_value = LLVMBuildLoad2(codegen->builder, vt, v->llvm_value, "elemld");
                v->is_lvalue = 0;
            }
        }

        if (v->llvm_value && LLVMIsConstant(v->llvm_value) && !v->is_lvalue &&
            LLVMGetTypeKind(LLVMTypeOf(v->llvm_value)) == LLVMIntegerTypeKind &&
            LLVMGetTypeKind(llvm_elem) == LLVMIntegerTypeKind &&
            LLVMTypeOf(v->llvm_value) != llvm_elem) {
            // Normalize constant-int elements to the slice's element width
            // here, at collection time — this is the only place the source
            // type (v->goo_type) is still in scope. The global path's late
            // rebuild below runs after this loop on bare LLVMValueRefs with
            // no source type available. Extraction must follow the SOURCE
            // type's signedness (see the array fast-path rebuild / the
            // var-decl rebuild in function_codegen.c): zero-extracting a
            // signed-negative narrow constant loses the sign (int32 -5 became
            // 4294967291 — the signed flag on LLVMConstInt does not re-extend
            // an already-widened raw value).
            int src_signed = v->goo_type ? type_is_signed(v->goo_type)
                                          : type_is_signed(elem_type);
            unsigned long long raw = src_signed
                ? (unsigned long long)LLVMConstIntGetSExtValue(v->llvm_value)
                : LLVMConstIntGetZExtValue(v->llvm_value);
            v->llvm_value = LLVMConstInt(llvm_elem, raw, src_signed);
        } else if (v->llvm_value && LLVMIsConstant(v->llvm_value) && !v->is_lvalue) {
            // Float counterpart of the int-constant normalization above:
            // `[]float32{1.5}` — the untyped literal folds to a double
            // constant, which must be rebuilt at the element's width before
            // reaching the global fast path or the builder store below.
            // Without this, `var gs = []float32{1.5}` puts an 8-byte double
            // constant into the private backing global's 4-byte float slots
            // (a constant-initializer type mismatch caught by the LLVM 22
            // module verifier: "Global variable initializer type does not
            // match global variable type!").
            LLVMTypeKind vk = LLVMGetTypeKind(LLVMTypeOf(v->llvm_value));
            LLVMTypeKind ek = LLVMGetTypeKind(llvm_elem);
            int v_is_fp = (vk == LLVMFloatTypeKind || vk == LLVMDoubleTypeKind);
            int e_is_fp = (ek == LLVMFloatTypeKind || ek == LLVMDoubleTypeKind);
            if (v_is_fp && e_is_fp && LLVMTypeOf(v->llvm_value) != llvm_elem) {
                LLVMBool loses_info;
                double d = LLVMConstRealGetDouble(v->llvm_value, &loses_info);
                v->llvm_value = LLVMConstReal(llvm_elem, d);
            } else if (vk == LLVMIntegerTypeKind && e_is_fp) {
                // Int-constant element into a float slice: `[]float64{1, 2.5}`
                // — the untyped `1` folds to an i64 constant, which neither
                // the int->int arm above (element isn't integer kind) nor the
                // FP->FP case catches, so it silently landed as an integer
                // bit pattern in the float slot (gm[0] == 1.0 was false).
                // Same rule as the var-decl global int->FP rebuild in
                // function_codegen.c: extract by SOURCE signedness, route
                // unsigned through unsigned long long so a large value's top
                // bit isn't reinterpreted as a sign, rebuild via ConstReal.
                int src_signed = v->goo_type ? type_is_signed(v->goo_type) : 1;
                double d = src_signed
                    ? (double)LLVMConstIntGetSExtValue(v->llvm_value)
                    : (double)(unsigned long long)LLVMConstIntGetZExtValue(v->llvm_value);
                v->llvm_value = LLVMConstReal(llvm_elem, d);
            }
        }

        elem_vals[idx] = v->llvm_value;
        elem_signed[idx] = v->goo_type ? type_is_signed(v->goo_type) : 1;
        value_info_free(v);
    }

    LLVMTypeRef arr_type = LLVMArrayType(llvm_elem, count);

    // Global scope (package-level `var s = []T{...}`): there is no active
    // builder insertion point (codegen->current_function is NULL), so every
    // builder call below would dereference an unpositioned builder and crash
    // (this was a segfault on `var n = []int{...}`). Instead produce a fully
    // CONSTANT slice value — a { ptr, len, cap } struct whose ptr field points
    // at a private constant global backing array. Mirrors the array-literal
    // const fast path. Elements must be LLVM constants at global scope.
    if (codegen->current_function == NULL) {
        for (size_t i = 0; i < count; i++) {
            if (!elem_vals[i] || !LLVMIsConstant(elem_vals[i])) {
                free(elem_vals);
                free(elem_signed);
                codegen_error(codegen, pos,
                    "global slice literal requires constant elements");
                return NULL;
            }
            // Integer- and float-constant elements are already normalized to
            // llvm_elem's width (integers with correct SOURCE-signedness
            // extraction) by the collection loop above, where v->goo_type was
            // still in scope. No rebuild needed here — redoing the integer
            // case from a bare LLVMValueRef with no source type would have to
            // guess signedness from the (possibly wider) DESTINATION type and
            // could re-mangle an already-corrected value.
        }
        LLVMValueRef backing;
        if (count > 0) {
            LLVMValueRef arr_const = LLVMConstArray(llvm_elem, elem_vals, (unsigned)count);
            backing = LLVMAddGlobal(codegen->module, arr_type, "slice_lit");
            LLVMSetInitializer(backing, arr_const);
            LLVMSetLinkage(backing, LLVMPrivateLinkage);
            LLVMSetGlobalConstant(backing, 1);
        } else {
            // Empty slice literal: Go's `[]T{}` is empty-but-non-nil (len 0,
            // but the backing pointer must NOT compare == nil — see the
            // slice==nil lowering in expression_codegen.c). This global/
            // const-scope path builds an LLVM constant directly and never
            // calls goo_alloc, so it can't pick up goo_alloc(0)'s zero-size
            // sentinel the way the local-scope path below does. Instead
            // reference that SAME sentinel directly as an external symbol
            // (defined once, in runtime.c) so every zero-size allocation —
            // literal or runtime — shares one non-nil, never-written,
            // never-freed byte. Under LLVM's opaque pointers the declared
            // pointee type (i8) is irrelevant — every pointer value has the
            // single generic `ptr` type, so this needs no cast to line up
            // with the slice struct's ptr field.
            LLVMTypeRef i8ty = LLVMInt8TypeInContext(codegen->context);
            LLVMValueRef zerobase = LLVMGetNamedGlobal(codegen->module, "goo_zerobase");
            if (!zerobase) {
                zerobase = LLVMAddGlobal(codegen->module, i8ty, "goo_zerobase");
                LLVMSetLinkage(zerobase, LLVMExternalLinkage);
            }
            backing = zerobase;
        }
        free(elem_vals);
        free(elem_signed);
        LLVMTypeRef slice_llvm = codegen_type_to_llvm(codegen, slice_type);
        LLVMValueRef len_c = LLVMConstInt(LLVMInt64TypeInContext(codegen->context), count, 0);
        LLVMValueRef fields[3] = { backing, len_c, len_c };
        LLVMValueRef slice_const = LLVMConstNamedStruct(slice_llvm, fields, 3);
        return value_info_new(NULL, slice_const, slice_type);
    }

    LLVMValueRef data_ptr;
    if (LLVMGetNamedFunction(codegen->module, "goo_alloc")) {
        LLVMValueRef size = LLVMSizeOf(arr_type);
        data_ptr = codegen_emit_alloc(codegen, size, ALLOC_KIND_DEFAULT, NULL);
        LLVMTypeRef i32ty = LLVMInt32TypeInContext(codegen->context);
        LLVMValueRef zero = LLVMConstInt(i32ty, 0, 0);
        for (size_t i = 0; i < count; i++) {
            LLVMValueRef indices[2] = { zero, LLVMConstInt(i32ty, i, 0) };
            LLVMValueRef ep = LLVMBuildGEP2(codegen->builder, arr_type, data_ptr,
                                            indices, 2, "slice_elem");
            elem_vals[i] = slice_coerce_elem(codegen, elem_vals[i], llvm_elem, elem_signed[i]);
            LLVMBuildStore(codegen->builder, elem_vals[i], ep);
        }
    } else {
        int all_const = 1;
        for (size_t i = 0; i < count; i++) {
            if (!LLVMIsConstant(elem_vals[i])) { all_const = 0; break; }
        }
        if (all_const && count > 0) {
            LLVMValueRef arr_const = LLVMConstArray(llvm_elem, elem_vals, (unsigned)count);
            LLVMValueRef global = LLVMAddGlobal(codegen->module, arr_type, "slice_lit");
            LLVMSetInitializer(global, arr_const);
            LLVMSetLinkage(global, LLVMPrivateLinkage);
            LLVMSetGlobalConstant(global, 1);
            data_ptr = global;
        } else {
            free(elem_vals);
            free(elem_signed);
            codegen_error(codegen, pos,
                "slice literal with non-constant elements requires the runtime "
                "allocator (goo_alloc), which is unavailable in this build");
            return NULL;
        }
    }
    free(elem_vals);
    free(elem_signed);

    LLVMTypeRef slice_llvm = codegen_type_to_llvm(codegen, slice_type);
    LLVMValueRef slice_val = LLVMGetUndef(slice_llvm);
    slice_val = LLVMBuildInsertValue(codegen->builder, slice_val, data_ptr, 0, "slice_ptr");
    LLVMValueRef len_val = LLVMConstInt(LLVMInt64TypeInContext(codegen->context), count, 0);
    slice_val = LLVMBuildInsertValue(codegen->builder, slice_val, len_val, 1, "slice_len");
    slice_val = LLVMBuildInsertValue(codegen->builder, slice_val, len_val, 2, "slice_cap");

    return value_info_new(NULL, slice_val, slice_type);
}
#endif

// codegen_generate_slice_lit lowers a slice literal to a slice struct
// { ptr, i64 len, i64 cap } backed by a heap-allocated array. Element
// type is taken from the type-check pass (node_type is already
// TYPE_SLICE).
//
// This single path serves BOTH surface forms (P3-2): the Goo-native
// `[1, 2, 3]` literal and the Go-standard typed composite literal
// `[]int{1, 2, 3}`. The P3-1 parser routes both to AST_SLICE_EXPR /
// SliceLitNode, so index, range, len, and append over a `[]T{}` literal
// reuse this lowering unchanged — no separate codegen is needed.
//
// Elements may be constants OR runtime values: `[]int{a, b, 30}` (the
// common Go form) lowers by allocating the backing buffer and storing each
// element individually, so a non-constant element no longer crashes the
// LLVM verifier. (The element WIDTH is still bounded by the type-checker's
// P3-1 guard to int32/string, which match their natural codegen repr; wider
// declared widths are rejected at type-check, not here.)
ValueInfo* codegen_generate_slice_lit(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_SLICE_EXPR) return NULL;

    SliceLitNode* lit = (SliceLitNode*)expr;
    Type* slice_type = expr->node_type;  // set by type checker
    if (!slice_type || slice_type->kind != TYPE_SLICE) {
        codegen_error(codegen, expr->pos, "Slice literal missing TYPE_SLICE node_type");
        return NULL;
    }
    // Delegate to the shared helper that also serves named-slice composite
    // literals (StructLiteralNode->field_values routed here from
    // codegen_generate_struct_lit when the resolved type is TYPE_SLICE).
    return codegen_build_slice_from_elems(codegen, checker,
                                          lit->elements, slice_type, expr->pos);
#endif
}

// Array composite literal `[N]T{e...}`. Lowers to an alloca of [N x T], stores
// each element (coerced to T's width) at its index, and zero-fills the omitted
// trailing elements (Go semantics). Returns the array BY VALUE (a load of the
// alloca); the index/len paths handle a by-value array. Package-level array
// globals (constant initializers) are a follow-up — this covers local literals.
ValueInfo* codegen_generate_array_lit(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_ARRAY_LITERAL) return NULL;

    ArrayLitNode* lit = (ArrayLitNode*)expr;
    Type* arr_type = expr->node_type;  // TYPE_ARRAY, stamped by the type checker
    if (!arr_type || arr_type->kind != TYPE_ARRAY) {
        codegen_error(codegen, expr->pos, "array literal missing TYPE_ARRAY node_type");
        return NULL;
    }

    // Comptime value params Task 3 (fix round 2): inside a monomorphized
    // comptime instance, the checker-stamped node_type was resolved ONCE at
    // template body-check time with the comptime param bound to a
    // placeholder — `buf := [n]int{}`'s literal carried the placeholder
    // length permanently, compiling clean and bounds-panicking at runtime.
    // Re-derive the literal's type fresh from its own AST type node under
    // the instance binding (the mirror Variable for `n` carries this
    // instance's value), exactly like codegen_generate_var_decl's
    // re-derivation (function_codegen.c — see the long comment there).
    // Local replacement only; expr->node_type stays untouched (the template
    // node is shared across instances). Gated (fix round 6, C-r5) on the
    // cached type carrying a comptime_length-flagged array — an unflagged
    // literal's template type was never placeholder-tainted (e.g. its
    // length names a block-local const SHADOWING the param), and
    // re-deriving it against the mirror scope would resolve the PARAM
    // instead of the shadow. See codegen_generate_var_decl's identical
    // gate rationale (function_codegen.c).
    if (codegen->active_comptime_value_n > 0 && lit->array_type &&
        goo_type_contains_comptime_array(arr_type)) {
        Type* fresh = type_from_ast(checker, lit->array_type);
        // Fix round 3 (minor 3): a failed re-derivation is a HARD codegen
        // failure — see codegen_generate_var_decl's identical branch
        // (function_codegen.c) for why falling back to the placeholder
        // type let an error-bearing compile still emit a binary.
        if (!fresh || fresh->kind != TYPE_ARRAY) {
            codegen_error(codegen, expr->pos,
                "cannot instantiate array literal type for this comptime instance");
            return NULL;
        }
        arr_type = fresh;
    }
    Type* elem_type = arr_type->data.array.element_type;
    size_t n = arr_type->data.array.length;
    LLVMTypeRef llvm_elem = codegen_type_to_llvm(codegen, elem_type);
    if (!llvm_elem) { codegen_error(codegen, expr->pos, "array literal: bad element type"); return NULL; }
    LLVMTypeRef arr_llvm = LLVMArrayType(llvm_elem, (unsigned)n);

    // All-constant fast path: build a constant [N x T] with LLVMConst* casts,
    // which is BUILDER-FREE and therefore valid at global scope (a package-level
    // `var tab = [256]byte{...}`, the table shape). Falls back to the alloca
    // path below if any element is not an LLVM constant (local literals only).
    {
        LLVMValueRef* consts = (LLVMValueRef*)malloc(sizeof(LLVMValueRef) * (n ? n : 1));
        if (consts) {
            // Pre-fill every slot with the zero value; keyed elements leave
            // gaps that are NOT just trailing (Go zero-fills them).
            for (size_t z = 0; z < n; z++) consts[z] = LLVMConstNull(llvm_elem);
            int all_const = 1;
            unsigned ew = (LLVMGetTypeKind(llvm_elem) == LLVMIntegerTypeKind)
                        ? LLVMGetIntTypeWidth(llvm_elem) : 0;
            int64_t cur = -1;  // effective index: keyed sets it, unkeyed is prev+1
            for (ASTNode* e = lit->elements; e; e = e->next) {
                ASTNode* value = e;
                if (e->type == AST_KEYED_ELEMENT) {
                    KeyedElementNode* ke = (KeyedElementNode*)e;
                    uint64_t k = 0;
                    if (!goo_fold_const_int(ke->key, &k)) { all_const = 0; break; }
                    cur = (int64_t)k;
                    value = ke->value;
                } else {
                    cur += 1;
                }
                if (cur < 0 || (uint64_t)cur >= n) { all_const = 0; break; }
                ValueInfo* ev = codegen_generate_expression(codegen, checker, value);
                if (!ev) { all_const = 0; break; }
                LLVMValueRef v = ev->llvm_value;
                int is_c = v && LLVMIsConstant(v) && !ev->is_lvalue;
                if (is_c && ew && LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMIntegerTypeKind
                          && LLVMTypeOf(v) != llvm_elem) {
                    // Rebuild the constant at the element width (LLVM 22 dropped
                    // the LLVMConst{ZExt,IntCast} const-expr casts). Extraction
                    // must follow the SOURCE type's signedness: zero-extracting
                    // a signed-negative narrow constant loses the sign (int32 -5
                    // became 4294967291 — the signed flag on LLVMConstInt does
                    // not re-extend an already-widened raw value). Same rule as
                    // the var-decl initializer rebuild in function_codegen.c.
                    int src_signed = ev->goo_type ? type_is_signed(ev->goo_type)
                                                  : type_is_signed(elem_type);
                    unsigned long long raw = src_signed
                        ? (unsigned long long)LLVMConstIntGetSExtValue(v)
                        : LLVMConstIntGetZExtValue(v);
                    v = LLVMConstInt(llvm_elem, raw, src_signed);
                } else if (is_c && LLVMTypeOf(v) != llvm_elem) {
                    // Float counterpart of the int-constant rebuild above:
                    // `[2]float32{1.5, 0.25}` — the untyped literals fold to
                    // double constants, which must be rebuilt at the
                    // element's width before landing in the [N x float]
                    // initializer below. Without this the const array holds
                    // 8-byte double bit patterns in 4-byte float slots, so
                    // every float32 element compares false at runtime.
                    // Mirrors the slice-collection-loop rebuild (Task 2).
                    LLVMTypeKind vk = LLVMGetTypeKind(LLVMTypeOf(v));
                    LLVMTypeKind ek = LLVMGetTypeKind(llvm_elem);
                    int v_is_fp = (vk == LLVMFloatTypeKind || vk == LLVMDoubleTypeKind);
                    int e_is_fp = (ek == LLVMFloatTypeKind || ek == LLVMDoubleTypeKind);
                    if (v_is_fp && e_is_fp) {
                        LLVMBool loses_info;
                        double d = LLVMConstRealGetDouble(v, &loses_info);
                        v = LLVMConstReal(llvm_elem, d);
                    } else if (vk == LLVMIntegerTypeKind && e_is_fp) {
                        // Int-constant element into a float array:
                        // `[2]float64{1, 2.5}` — the untyped `1` folds to an
                        // i64 constant; the int->int arm above requires an
                        // integer element (ew != 0) and the FP->FP case above
                        // requires an FP source, so it silently landed as an
                        // integer bit pattern in the float slot (b[0] == 1.0
                        // was false). Same rule as the var-decl global
                        // int->FP rebuild in function_codegen.c: extract by
                        // SOURCE signedness, route unsigned through unsigned
                        // long long so a large value's top bit isn't
                        // reinterpreted as a sign, rebuild via ConstReal.
                        int src_signed = ev->goo_type ? type_is_signed(ev->goo_type) : 1;
                        double d = src_signed
                            ? (double)LLVMConstIntGetSExtValue(v)
                            : (double)(unsigned long long)LLVMConstIntGetZExtValue(v);
                        v = LLVMConstReal(llvm_elem, d);
                    }
                }
                value_info_free(ev);
                if (!is_c) { all_const = 0; break; }
                consts[cur] = v;
            }
            if (all_const) {
                LLVMValueRef arr = LLVMConstArray(llvm_elem, consts, (unsigned)n);
                free(consts);
                ValueInfo* out = value_info_new(NULL, arr, arr_type);
                if (out) out->is_lvalue = 0;
                return out;
            }
            free(consts);
        }
    }

    LLVMValueRef arr_alloca = codegen_create_alloca(codegen, arr_llvm, "arraylit");
    LLVMValueRef zero = LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0);

    // Zero the whole array first: with keyed elements the assigned indices are
    // sparse, so unset slots (not just trailing ones) must be zero-valued.
    for (size_t z = 0; z < n; z++) {
        LLVMValueRef idx[2] = { zero, LLVMConstInt(LLVMInt32TypeInContext(codegen->context), z, 0) };
        LLVMValueRef gep = LLVMBuildGEP2(codegen->builder, arr_llvm, arr_alloca, idx, 2, "arr_zero");
        LLVMBuildStore(codegen->builder, LLVMConstNull(llvm_elem), gep);
    }

    int64_t cur = -1;  // effective index: keyed sets it, unkeyed is prev+1
    for (ASTNode* e = lit->elements; e; e = e->next) {
        ASTNode* value = e;
        if (e->type == AST_KEYED_ELEMENT) {
            KeyedElementNode* ke = (KeyedElementNode*)e;
            uint64_t k = 0;
            if (!goo_fold_const_int(ke->key, &k)) {
                codegen_error(codegen, e->pos, "array literal: non-constant element index");
                return NULL;
            }
            cur = (int64_t)k;
            value = ke->value;
        } else {
            cur += 1;
        }
        if (cur < 0 || (uint64_t)cur >= n) {
            codegen_error(codegen, e->pos, "array literal: element index out of bounds");
            return NULL;
        }
        ValueInfo* ev = codegen_generate_expression(codegen, checker, value);
        if (!ev) return NULL;
        LLVMValueRef v = ev->llvm_value;
        if (ev->is_lvalue && ev->goo_type) {
            LLVMTypeRef et = codegen_type_to_llvm(codegen, ev->goo_type);
            if (et) v = LLVMBuildLoad2(codegen->builder, et, v, "elem_load");
        }
        v = slice_coerce_elem(codegen, v, llvm_elem,
                              ev->goo_type ? type_is_signed(ev->goo_type) : 1);
        LLVMValueRef idx[2] = { zero, LLVMConstInt(LLVMInt32TypeInContext(codegen->context), (unsigned long long)cur, 0) };
        LLVMValueRef gep = LLVMBuildGEP2(codegen->builder, arr_llvm, arr_alloca, idx, 2, "arr_elem");
        LLVMBuildStore(codegen->builder, v, gep);
        value_info_free(ev);
    }

    LLVMValueRef arr_val = LLVMBuildLoad2(codegen->builder, arr_llvm, arr_alloca, "arrval");
    ValueInfo* out = value_info_new(NULL, arr_val, arr_type);
    if (out) out->is_lvalue = 0;
    return out;
#endif
}

