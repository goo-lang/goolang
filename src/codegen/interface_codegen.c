#include "codegen.h"
#include "embedding.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Interface codegen (P4-5): vtable construction, boxing a concrete value into an
// interface value {vtable, data}, and dynamic method dispatch through it.
//
// Layout (see type_mapping.c TYPE_INTERFACE): an interface value is { ptr vtable,
// ptr data }. `data` points to a heap copy of the concrete value; `vtable` points
// to a per-(concrete,interface) global array of THUNK function pointers, one per
// interface method in declaration order.
//
// A thunk gives every method a uniform call signature so dispatch can call
// through it without knowing the concrete type: `goo.thunk.<T>.<I>.<m>` has
// signature (ptr data, <interface-method-params>) -> <interface-method-ret>; it
// loads the receiver from `data` (a value receiver is loaded; a pointer receiver
// is passed as the data pointer itself) and tail-calls the real method `T__m`.

#if LLVM_AVAILABLE

static LLVMTypeRef iface_ptr_type(CodeGenerator* codegen) {
    return LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
}

// Build the (ptr data, <method params>) -> <ret> LLVM function type shared by a
// thunk definition and a dispatch call site. Writes the resolved return type to
// *ret_out. Returns NULL on failure.
static LLVMTypeRef thunk_fn_type(CodeGenerator* codegen, Type* method_type,
                                 LLVMTypeRef* ret_out) {
    size_t np = method_type ? method_type->data.function.param_count : 0;
    LLVMTypeRef* pt = malloc((np + 1) * sizeof(LLVMTypeRef));
    if (!pt) return NULL;
    pt[0] = iface_ptr_type(codegen);  // data pointer (receiver)
    for (size_t i = 0; i < np; i++) {
        pt[i + 1] = codegen_type_to_llvm(codegen, method_type->data.function.param_types[i]);
        if (!pt[i + 1]) { free(pt); return NULL; }
    }
    Type* ret = method_type ? method_type->data.function.return_type : NULL;
    LLVMTypeRef ret_llvm = (ret && ret->kind != TYPE_VOID)
                               ? codegen_type_to_llvm(codegen, ret)
                               : LLVMVoidTypeInContext(codegen->context);
    if (!ret_llvm) { free(pt); return NULL; }
    LLVMTypeRef fnty = LLVMFunctionType(ret_llvm, pt, (unsigned)(np + 1), 0);
    free(pt);
    if (ret_out) *ret_out = ret_llvm;
    return fnty;
}

// Emit (or reuse) the thunk wrapping concrete method `T__m` for interface method
// `im`. Returns the thunk function value (a ptr constant), or NULL on failure.
static LLVMValueRef build_thunk(CodeGenerator* codegen, TypeChecker* checker,
                                Type* concrete, const char* concrete_name,
                                const char* iface_name, InterfaceMethod* im) {
    // After the C-representation normalization in codegen_interface_box, a
    // pointer concrete must never reach the thunk builder — its thunks are
    // the pointee's. A future direct caller of codegen_interface_vtable with
    // a raw *T would otherwise re-create the #109 verifier failure.
    if (concrete && concrete->kind == TYPE_POINTER) {
        codegen_error(codegen, (Position){0},
                      "internal: pointer concrete reached thunk builder un-normalized");
        return NULL;
    }

    char thunk_name[256];
    snprintf(thunk_name, sizeof(thunk_name), "goo.thunk.%s.%s.%s",
             concrete_name, iface_name, im->name);
    LLVMValueRef existing = LLVMGetNamedFunction(codegen->module, thunk_name);
    if (existing) return existing;

    LLVMTypeRef ret_llvm = NULL;
    LLVMTypeRef fnty = thunk_fn_type(codegen, im->type, &ret_llvm);
    if (!fnty) return NULL;
    LLVMValueRef thunk = LLVMAddFunction(codegen->module, thunk_name, fnty);

    // Resolve the real method T__m and its registered receiver kind. If T
    // doesn't declare it directly, it is a PROMOTED method: resolve the
    // embedding path and re-mangle against the owning embedded type.
    EmbedResult epath;
    memset(&epath, 0, sizeof(epath));
    char* mangled = type_method_mangled_name(concrete_name, im->name);
    LLVMValueRef real_fn = mangled ? LLVMGetNamedFunction(codegen->module, mangled) : NULL;
    Variable* mvar = mangled ? type_checker_lookup_variable(checker, mangled) : NULL;
    free(mangled);
    if (!real_fn && concrete->kind == TYPE_STRUCT) {
        EmbedResult er = embedding_resolve(checker, concrete, im->name);
        if (er.kind == EMBED_METHOD) {
            epath = er;
            const char* otn = type_receiver_name(er.owner);
            char* om = otn ? type_method_mangled_name(otn, im->name) : NULL;
            real_fn = om ? LLVMGetNamedFunction(codegen->module, om) : NULL;
            mvar = om ? type_checker_lookup_variable(checker, om) : NULL;
            free(om);
        }
    }
    if (!real_fn) {
        codegen_error(codegen, (Position){0},
                      "internal: missing method implementation for interface thunk");
        return NULL;
    }
    Type* recv_param = (mvar && mvar->type && mvar->type->kind == TYPE_FUNCTION &&
                        mvar->type->data.function.param_count > 0)
                           ? mvar->type->data.function.param_types[0] : NULL;
    int ptr_recv = recv_param && recv_param->kind == TYPE_POINTER;

    // Emit the thunk body, saving/restoring the outer insert point.
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(codegen->builder);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(codegen->context, thunk, "entry");
    LLVMPositionBuilderAtEnd(codegen->builder, entry);

    LLVMValueRef data = LLVMGetParam(thunk, 0);
    size_t np = im->type ? im->type->data.function.param_count : 0;
    LLVMValueRef* call_args = malloc((np + 1) * sizeof(LLVMValueRef));
    if (!call_args) { LLVMPositionBuilderAtEnd(codegen->builder, saved); return NULL; }

    // Receiver: walk the embedding path (empty for direct methods) from the
    // boxed *concrete to a pointer to the method's owner, loading through
    // pointer-embedded hops. Then pointer receivers take that pointer, value
    // receivers load through it.
    LLVMValueRef recv_ptr = data;
    Type* cur = concrete;
    for (size_t h = 0; h < epath.len; h++) {
        unsigned fidx = 0;
        int found = 0;
        for (size_t fi = 0; fi < cur->data.struct_type.field_count; fi++) {
            if (strcmp(cur->data.struct_type.fields[fi].name, epath.path[h]) == 0) {
                fidx = (unsigned)fi;
                found = 1;
                break;
            }
        }
        if (!found) {
            codegen_error(codegen, (Position){0},
                          "internal: embedding hop '%s' not found building thunk",
                          epath.path[h]);
            LLVMPositionBuilderAtEnd(codegen->builder, saved);
            return NULL;
        }
        LLVMTypeRef cur_llvm = codegen_get_struct_type(codegen, cur);
        recv_ptr = LLVMBuildStructGEP2(codegen->builder, cur_llvm, recv_ptr, fidx, "embed.hop");
        Type* ft = cur->data.struct_type.fields[fidx].type;
        if (ft->kind == TYPE_POINTER) {
            recv_ptr = LLVMBuildLoad2(codegen->builder, iface_ptr_type(codegen),
                                      recv_ptr, "embed.load");
            cur = ft->data.pointer.pointee_type;
        } else {
            cur = ft;
        }
    }
    if (ptr_recv) {
        call_args[0] = recv_ptr;
    } else {
        LLVMTypeRef llvm_owner = codegen_type_to_llvm(codegen, cur);
        call_args[0] = llvm_owner
                           ? LLVMBuildLoad2(codegen->builder, llvm_owner, recv_ptr, "recv")
                           : recv_ptr;
    }
    for (size_t i = 0; i < np; i++) call_args[i + 1] = LLVMGetParam(thunk, (unsigned)(i + 1));

    LLVMValueRef call = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(real_fn),
                                       real_fn, call_args, (unsigned)(np + 1),
                                       ret_llvm == LLVMVoidTypeInContext(codegen->context) ? "" : "callr");
    free(call_args);

    if (ret_llvm == LLVMVoidTypeInContext(codegen->context)) {
        LLVMBuildRetVoid(codegen->builder);
    } else {
        LLVMBuildRet(codegen->builder, call);
    }

    LLVMPositionBuilderAtEnd(codegen->builder, saved);
    return thunk;
}

// Build (or reuse) the vtable global for boxing `concrete` into `iface`.
// `pointer_form` selects a distinctly-named global for the pointer-boxing
// shape (Task 5) so it never aliases the value-boxing shape's global, even
// though both build thunks against the same `concrete` and both globals hold
// identical slot contents. Returns the global (a ptr to the [N x ptr] thunk
// array), or NULL on failure.
LLVMValueRef codegen_interface_vtable(CodeGenerator* codegen, TypeChecker* checker,
                                      Type* iface, Type* concrete, int pointer_form) {
    if (!iface || iface->kind != TYPE_INTERFACE) return NULL;
    const char* cname = type_receiver_name(concrete);
    const char* iname = iface->data.interface.name ? iface->data.interface.name : "iface";
    if (!cname) {
        codegen_error(codegen, (Position){0},
                      "internal: cannot name concrete type for interface vtable");
        return NULL;
    }

    char gname[256];
    if (pointer_form) {
        snprintf(gname, sizeof(gname), "goo.vtable.$ptr$%s.%s", cname, iname);
    } else {
        snprintf(gname, sizeof(gname), "goo.vtable.%s.%s", cname, iname);
    }
    LLVMValueRef existing = LLVMGetNamedGlobal(codegen->module, gname);
    if (existing) return existing;

    // Interface-typed map keys, Task 1 (vtable ABI shift): the vtable now
    // carries n+1 slots — slot 0 is the concrete's per-type value-equality
    // comparator (codegen_get_or_emit_type_eq), slots 1..n are the method
    // thunks in their original, unchanged order. codegen_interface_dispatch
    // shifts its method GEP index by +1 to match (interface_codegen.c,
    // below); this is the ONLY other site that indexes a vtable by a raw
    // slot number (confirmed by grepping every `goo.vtable`/vtable-indexing
    // site in the codebase — codegen_interface_assert_match only ever
    // compares whole vtable-pointer identity, never indexes into one).
    size_t n = iface->data.interface.method_count;
    LLVMTypeRef ptrty = iface_ptr_type(codegen);
    LLVMValueRef* slots = malloc((n + 1) * sizeof(LLVMValueRef));
    if (!slots) return NULL;

    LLVMValueRef eq_fn = codegen_get_or_emit_type_eq(codegen, checker, concrete);
    if (!eq_fn) { free(slots); return NULL; }
    // A Function value's LLVM type is already `ptr` (opaque pointers), the
    // same as every thunk placed below without a cast — no bitcast needed
    // to satisfy LLVMConstArray(ptrty, ...).
    slots[0] = eq_fn;

    size_t i = 0;
    for (InterfaceMethod* im = iface->data.interface.methods; im; im = im->next, i++) {
        LLVMValueRef thunk = build_thunk(codegen, checker, concrete, cname, iname, im);
        if (!thunk) { free(slots); return NULL; }
        slots[i + 1] = thunk;  // a function value is a ptr constant
    }

    LLVMTypeRef arrty = LLVMArrayType(ptrty, (unsigned)(n + 1));
    LLVMValueRef init = LLVMConstArray(ptrty, slots, (unsigned)(n + 1));
    LLVMValueRef g = LLVMAddGlobal(codegen->module, arrty, gname);
    LLVMSetInitializer(g, init);
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetGlobalConstant(g, 1);
    free(slots);
    return g;
}

// Box a concrete value into an interface value { vtable, data }. `value` is the
// loaded concrete LLVM value. Returns the interface struct value, or NULL.
LLVMValueRef codegen_interface_box(CodeGenerator* codegen, TypeChecker* checker,
                                   Type* iface, Type* concrete, LLVMValueRef value) {
    // C-representation for pointer concretes (Go's own layout): the interface
    // data word IS the pointer. *T's thunks are built against the POINTEE
    // (same build_thunk as T's), because in both boxing shapes `data` ends up
    // pointing at a T (value-boxed: at the heap copy; pointer-boxed: at the
    // caller's object) — so the thunk bodies are identical. No heap box:
    // boxing a pointer must alias, and storing it in a box was the #109
    // miscompile (thunks treated the box as the pointee). See
    // docs/superpowers/specs/2026-07-04-ptr-iface-boxing-design.md.
    //
    // Task 5: despite identical thunks, the vtable GLOBAL must NOT be the
    // same one T's value-boxing uses — a value-boxed T and a pointer-boxed
    // *T both end up with `data` pointing at a T, but they are different
    // DYNAMIC TYPES in Go and an assertion/switch must be able to tell them
    // apart via the vtable-pointer identity check. pointer_form=1 requests
    // the distinctly-named `goo.vtable.$ptr$<T>.<I>` global (same slot
    // contents, different address) so it never aliases `goo.vtable.<T>.<I>`.
    if (concrete && concrete->kind == TYPE_POINTER &&
        concrete->data.pointer.pointee_type &&
        type_receiver_name(concrete->data.pointer.pointee_type)) {
        Type* pointee = concrete->data.pointer.pointee_type;
        LLVMValueRef pvt = codegen_interface_vtable(codegen, checker, iface, pointee, /*pointer_form=*/1);
        if (!pvt) return NULL;
        LLVMTypeRef pifacety = codegen_type_to_llvm(codegen, iface);
        if (!pifacety) return NULL;
        LLVMValueRef piv = LLVMGetUndef(pifacety);
        piv = LLVMBuildInsertValue(codegen->builder, piv, pvt, 0, "iface.vt");
        piv = LLVMBuildInsertValue(codegen->builder, piv, value, 1, "iface.data");
        return piv;
    }

    LLVMValueRef vt = codegen_interface_vtable(codegen, checker, iface, concrete, /*pointer_form=*/0);
    if (!vt) return NULL;

    LLVMTypeRef llvm_T = codegen_type_to_llvm(codegen, concrete);
    if (!llvm_T) return NULL;

    // Heap-box the concrete value so the interface can outlive the current frame.
    LLVMValueRef alloc_fn = LLVMGetNamedFunction(codegen->module, "goo_alloc");
    if (!alloc_fn) {
        codegen_error(codegen, (Position){0}, "goo_alloc missing for interface boxing");
        return NULL;
    }
    LLVMValueRef size = LLVMSizeOf(llvm_T);
    LLVMValueRef data = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(alloc_fn),
                                       alloc_fn, &size, 1, "iface_data");
    LLVMBuildStore(codegen->builder, value, data);

    LLVMTypeRef ifacety = codegen_type_to_llvm(codegen, iface);  // { ptr, ptr }
    LLVMValueRef iv = LLVMGetUndef(ifacety);
    iv = LLVMBuildInsertValue(codegen->builder, iv, vt, 0, "iface.vt");
    iv = LLVMBuildInsertValue(codegen->builder, iv, data, 1, "iface.data");
    return iv;
}

// Dispatch `method_name(args)` through a loaded interface value. `iface_val` is
// the { vtable, data } struct value; `args`/`argc` are the already-generated
// argument values (NOT including the receiver). Returns the call result, or NULL.
ValueInfo* codegen_interface_dispatch(CodeGenerator* codegen, TypeChecker* checker,
                                      LLVMValueRef iface_val, Type* iface_type,
                                      const char* method_name,
                                      LLVMValueRef* args, size_t argc) {
    (void)checker;
    if (!iface_type || iface_type->kind != TYPE_INTERFACE) return NULL;

    // Find the method's slot index and signature in declaration order.
    size_t idx = 0;
    InterfaceMethod* im = NULL;
    for (InterfaceMethod* m = iface_type->data.interface.methods; m; m = m->next, idx++) {
        if (m->name && strcmp(m->name, method_name) == 0) { im = m; break; }
    }
    if (!im) {
        codegen_error(codegen, (Position){0},
                      "internal: interface method '%s' not found for dispatch", method_name);
        return NULL;
    }

    LLVMValueRef vt = LLVMBuildExtractValue(codegen->builder, iface_val, 0, "vt");
    LLVMValueRef data = LLVMBuildExtractValue(codegen->builder, iface_val, 1, "data");

    // Load the thunk pointer from vtable slot `idx + 1` (array of ptr): slot
    // 0 is now the per-concrete-type eq comparator (Task 1, codegen_
    // interface_vtable above), so method thunks shifted right by one.
    LLVMTypeRef ptrty = iface_ptr_type(codegen);
    LLVMValueRef gep_idx = LLVMConstInt(LLVMInt64TypeInContext(codegen->context), idx + 1, 0);
    LLVMValueRef slot = LLVMBuildGEP2(codegen->builder, ptrty, vt, &gep_idx, 1, "vt.slot");
    LLVMValueRef thunk = LLVMBuildLoad2(codegen->builder, ptrty, slot, "thunk");

    LLVMTypeRef ret_llvm = NULL;
    LLVMTypeRef fnty = thunk_fn_type(codegen, im->type, &ret_llvm);
    if (!fnty) return NULL;

    LLVMValueRef* call_args = malloc((argc + 1) * sizeof(LLVMValueRef));
    if (!call_args) return NULL;
    call_args[0] = data;
    for (size_t i = 0; i < argc; i++) call_args[i + 1] = args[i];

    int is_void = (ret_llvm == LLVMVoidTypeInContext(codegen->context));
    LLVMValueRef result = LLVMBuildCall2(codegen->builder, fnty, thunk, call_args,
                                         (unsigned)(argc + 1), is_void ? "" : "ifc.call");
    free(call_args);

    Type* ret_type = im->type ? im->type->data.function.return_type : NULL;
    return value_info_new(NULL, result, ret_type);
}

// Task 2 (type assertions): the vtable-pointer identity compare shared by
// `x.(T)` (both comma-ok and single-return) and Task 3's type switch. Spec
// mechanism: the dynamic type held by an interface value is T iff
// `iface.vtable == &goo.vtable.T.I`, and is *T iff `iface.vtable ==
// &goo.vtable.$ptr$T.I` — a pointer compare, no RTTI. `iface_val` must
// already be the LOADED {vtable, data} struct value (mirror the
// is_lvalue-load idiom at codegen_interface_dispatch's call site,
// call_codegen.c ~1094 — a selector/index operand is an address and must be
// loaded before ExtractValue). `target`'s vtable is resolved via
// codegen_interface_vtable, requesting the SAME form (value vs. pointer) the
// boxing site would have used for that shape (see the form selection below)
// — it must already exist by the time any assertion against a live interface
// value runs, since boxing is what put the value there. Writes the raw
// (still-boxed) data pointer to *data_out unless NULL; pass it to
// codegen_interface_assert_unbox to recover the concrete value, but only
// inside a branch already gated on the returned match bit — the data
// pointer's pointee size/shape depends on the ACTUAL runtime type, so
// unboxing it as `target` before confirming the match would be unsound.
// Returns the i1 match value, or NULL if the vtable global couldn't be
// resolved (should not happen for a type-checked assertion — the checker's
// type_interface_satisfied gate already ruled out a non-implementing target).
LLVMValueRef codegen_interface_assert_match(CodeGenerator* codegen, TypeChecker* checker,
                                            LLVMValueRef iface_val, Type* iface_type,
                                            Type* target, LLVMValueRef* data_out) {
    if (!codegen || !iface_type || iface_type->kind != TYPE_INTERFACE || !target) return NULL;

    // Task 5 (replaces df41fb2's unwrap-to-pointee-and-use-value-form): a
    // value-boxed T and a pointer-boxed *T both end up with `data` pointing
    // at a T, but they are different Go dynamic types and must compare
    // unequal. Select the vtable FORM by the target's own shape, mirroring
    // codegen_interface_box's two branches exactly:
    //   - target is a pointer *T with a nameable pointee (box-site's
    //     C-representation branch) -> look up the POINTER-form vtable, built
    //     against the pointee, so `x.(*T)` only matches a pointer-boxed *T.
    //   - target is anything else (a value T, or a pointer without a
    //     nameable pointee) -> look up the VALUE-form vtable, so `x.(T)`
    //     only matches a value-boxed T.
    // Distinct globals -> distinct addresses -> the icmp below now
    // discriminates T from *T instead of aliasing them.
    Type* vt_target = target;
    int vt_pointer_form = 0;
    if (vt_target->kind == TYPE_POINTER && vt_target->data.pointer.pointee_type &&
        type_receiver_name(vt_target->data.pointer.pointee_type)) {
        vt_target = vt_target->data.pointer.pointee_type;
        vt_pointer_form = 1;
    }

    LLVMValueRef vt_want = codegen_interface_vtable(codegen, checker, iface_type, vt_target,
                                                    vt_pointer_form);
    if (!vt_want) return NULL;

    LLVMValueRef vt_have = LLVMBuildExtractValue(codegen->builder, iface_val, 0, "ta.vt");
    if (data_out) {
        *data_out = LLVMBuildExtractValue(codegen->builder, iface_val, 1, "ta.data");
    }
    return LLVMBuildICmp(codegen->builder, LLVMIntEQ, vt_have, vt_want, "ta.match");
}

// Recover the concrete value of Go type `target` from an interface's `data`
// pointer (as extracted by codegen_interface_assert_match). Must mirror
// codegen_interface_box's two boxing shapes exactly:
//   - a pointer concrete whose pointee has a nameable receiver (e.g. *Box)
//     is boxed WITHOUT a heap copy — `data` IS the pointer value itself
//     (see codegen_interface_box's C-representation branch) — so recovering
//     it is a no-op (identity), not a load. In this LLVM version pointers
//     are opaque (`ptr`), so no bitcast is needed either.
//   - every other concrete (a struct value, or a pointer without a
//     nameable pointee) is boxed as a heap COPY — `data` is a pointer TO a
//     `target`, and must be LOADED through.
// Returns NULL if `target`'s LLVM type cannot be resolved.
LLVMValueRef codegen_interface_assert_unbox(CodeGenerator* codegen, Type* target,
                                            LLVMValueRef data) {
    if (!codegen || !target || !data) return NULL;
    if (target->kind == TYPE_POINTER && target->data.pointer.pointee_type &&
        type_receiver_name(target->data.pointer.pointee_type)) {
        return data;
    }
    LLVMTypeRef target_llvm = codegen_type_to_llvm(codegen, target);
    if (!target_llvm) return NULL;
    return LLVMBuildLoad2(codegen->builder, target_llvm, data, "ta.val");
}

#endif // LLVM_AVAILABLE
