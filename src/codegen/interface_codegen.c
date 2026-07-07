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
// Shared pointer-identity comparator `i32 @goo.ptreq(i64 a, i64 b) { ret a==b }`
// used as the descriptor's eq_fn field for pointer-boxed interfaces (the data
// word is the pointer; equality is identity). Get-or-emit by name — no per-type
// cache needed, one instance suffices module-wide.
static LLVMValueRef iface_ptr_eq_fn(CodeGenerator* codegen) {
    LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo.ptreq");
    if (fn) return fn;
    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
    LLVMTypeRef params[2] = { i64, i64 };
    fn = LLVMAddFunction(codegen->module, "goo.ptreq", LLVMFunctionType(i32, params, 2, 0));
    LLVMSetLinkage(fn, LLVMPrivateLinkage);
    LLVMBasicBlockRef save = LLVMGetInsertBlock(codegen->builder);
    LLVMBasicBlockRef bb = LLVMAppendBasicBlockInContext(ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(codegen->builder, bb);
    LLVMValueRef eq = LLVMBuildICmp(codegen->builder, LLVMIntEQ,
                                    LLVMGetParam(fn, 0), LLVMGetParam(fn, 1), "ptreq");
    LLVMBuildRet(codegen->builder, LLVMBuildZExt(codegen->builder, eq, i32, "ptreq.i32"));
    if (save) LLVMPositionBuilderAtEnd(codegen->builder, save);
    return fn;
}

// Per-type %v formatter reached via the descriptor's fmt_fn field (field
// index 2, codegen_get_or_emit_type_desc below). Emits (or reuses, by
// name) `goo.fmt.<T>` / `goo.fmt.$ptr$<T>` of LLVM type `goo_string(ptr)`:
// loads the concrete value from the `data` param and returns its %v
// string via the matching goo_*_to_string runtime helper.
//
// v1 scalar kinds only (int/uint widths, bool, float32/64, string); a
// pointer_form concrete or any other kind (struct, slice, map, ...) falls
// back to a goo_string copy of the type name — the same "T" / "*T" string
// codegen_get_or_emit_type_desc computes for its type_name field. This is
// computed independently here (not fetched from the descriptor) rather
// than calling codegen_get_or_emit_type_desc: that function calls INTO
// this one to fill its fmt_fn field BEFORE the descriptor global exists
// (LLVMGetNamedGlobal wouldn't find it yet), so a fallback-path call back
// into codegen_get_or_emit_type_desc for the same (concrete, pointer_form)
// would recurse forever instead of hitting its dedup cache.
//
// Mirrors build_thunk's function-creation + save/restore-builder pattern
// (this file, above): a NEW function is created and its body emitted
// while codegen may be mid-emitting another function (descriptor
// synthesis happens during interface boxing), so the caller's insert
// point must survive the call.
LLVMValueRef codegen_get_or_emit_type_fmt(CodeGenerator* codegen, TypeChecker* checker,
                                          Type* concrete, int pointer_form) {
    (void)checker;
    if (!codegen || !concrete) return NULL;

    const char* cname = type_receiver_name(concrete);
    if (!cname) cname = type_to_string(concrete);
    char fname[256];
    if (pointer_form) snprintf(fname, sizeof(fname), "goo.fmt.$ptr$%s", cname);
    else              snprintf(fname, sizeof(fname), "goo.fmt.%s", cname);
    LLVMValueRef existing = LLVMGetNamedFunction(codegen->module, fname);
    if (existing) return existing;

    LLVMTypeRef ptrty = iface_ptr_type(codegen);
    LLVMTypeRef string_ty = codegen_get_basic_type(codegen, TYPE_STRING);
    if (!string_ty) return NULL;
    LLVMTypeRef param_types[1] = { ptrty };
    LLVMTypeRef fnty = LLVMFunctionType(string_ty, param_types, 1, 0);
    LLVMValueRef fn = LLVMAddFunction(codegen->module, fname, fnty);
    if (!fn) return NULL;

    // Save/restore the outer insert point (see comment above).
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(codegen->builder);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(codegen->context, fn, "entry");
    LLVMPositionBuilderAtEnd(codegen->builder, entry);

    LLVMValueRef data = LLVMGetParam(fn, 0);
    LLVMValueRef result = NULL;
    LLVMContextRef ctx = codegen->context;

    int is_sint = !pointer_form && (concrete->kind == TYPE_INT8 || concrete->kind == TYPE_INT16 ||
                                    concrete->kind == TYPE_INT32 || concrete->kind == TYPE_INT64);
    int is_uint = !pointer_form && (concrete->kind == TYPE_UINT8 || concrete->kind == TYPE_UINT16 ||
                                    concrete->kind == TYPE_UINT32 || concrete->kind == TYPE_UINT64);
    int is_bool = !pointer_form && concrete->kind == TYPE_BOOL;
    int is_float = !pointer_form && (concrete->kind == TYPE_FLOAT32 || concrete->kind == TYPE_FLOAT64);
    int is_string = !pointer_form && concrete->kind == TYPE_STRING;

    if (is_sint) {
        LLVMTypeRef ty = codegen_type_to_llvm(codegen, concrete);
        LLVMValueRef v = LLVMBuildLoad2(codegen->builder, ty, data, "fmt.load");
        LLVMValueRef w = LLVMBuildSExt(codegen->builder, v, LLVMInt64TypeInContext(ctx), "fmt.sext");
        LLVMValueRef i2s = LLVMGetNamedFunction(codegen->module, "goo_int_to_string");
        if (!i2s) { LLVMPositionBuilderAtEnd(codegen->builder, saved); return NULL; }
        LLVMValueRef args[1] = { w };
        result = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(i2s), i2s, args, 1, "fmt.s");
    } else if (is_uint) {
        LLVMTypeRef ty = codegen_type_to_llvm(codegen, concrete);
        LLVMValueRef v = LLVMBuildLoad2(codegen->builder, ty, data, "fmt.load");
        LLVMValueRef w = LLVMBuildZExt(codegen->builder, v, LLVMInt64TypeInContext(ctx), "fmt.zext");
        LLVMValueRef u2s = LLVMGetNamedFunction(codegen->module, "goo_uint_to_string");
        if (!u2s) { LLVMPositionBuilderAtEnd(codegen->builder, saved); return NULL; }
        LLVMValueRef args[1] = { w };
        result = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(u2s), u2s, args, 1, "fmt.s");
    } else if (is_bool) {
        LLVMTypeRef ty = codegen_type_to_llvm(codegen, concrete);  // i1
        LLVMValueRef v = LLVMBuildLoad2(codegen->builder, ty, data, "fmt.load");
        LLVMValueRef w = LLVMBuildZExt(codegen->builder, v, LLVMInt32TypeInContext(ctx), "fmt.zext");
        LLVMValueRef b2s = LLVMGetNamedFunction(codegen->module, "goo_bool_to_string");
        if (!b2s) { LLVMPositionBuilderAtEnd(codegen->builder, saved); return NULL; }
        LLVMValueRef args[1] = { w };
        result = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(b2s), b2s, args, 1, "fmt.s");
    } else if (is_float) {
        LLVMTypeRef ty = codegen_type_to_llvm(codegen, concrete);
        LLVMValueRef v = LLVMBuildLoad2(codegen->builder, ty, data, "fmt.load");
        LLVMValueRef w = (concrete->kind == TYPE_FLOAT32)
            ? LLVMBuildFPExt(codegen->builder, v, LLVMDoubleTypeInContext(ctx), "fmt.fpext")
            : v;
        LLVMValueRef f2s = LLVMGetNamedFunction(codegen->module, "goo_float_to_string");
        if (!f2s) { LLVMPositionBuilderAtEnd(codegen->builder, saved); return NULL; }
        LLVMValueRef args[1] = { w };
        result = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(f2s), f2s, args, 1, "fmt.s");
    } else if (is_string) {
        // `data` points at a heap goo_string_t (value-boxed) — load and
        // return it directly (a shallow {ptr,len} copy; the backing bytes
        // are shared, same as every other %v path returning a fresh
        // struct without deep-copying storage).
        LLVMTypeRef ty = codegen_type_to_llvm(codegen, concrete);
        result = LLVMBuildLoad2(codegen->builder, ty, data, "fmt.str");
    } else {
        // pointer_form, or any non-scalar concrete kind not yet supported
        // in v1 (struct/slice/map/...): bounded fallback — a goo_string
        // copy of this type's own name ("T", or "*T" for pointer_form),
        // matching what codegen_get_or_emit_type_desc's type_name field
        // holds for the same (concrete, pointer_form).
        char tname[260];
        if (pointer_form) snprintf(tname, sizeof(tname), "*%s", cname);
        else              snprintf(tname, sizeof(tname), "%s", cname);
        LLVMValueRef name_ptr = LLVMBuildGlobalStringPtr(codegen->builder, tname, "fmt.tname");
        LLVMValueRef newfn = LLVMGetNamedFunction(codegen->module, "goo_string_new");
        if (!newfn) { LLVMPositionBuilderAtEnd(codegen->builder, saved); return NULL; }
        LLVMValueRef args[1] = { name_ptr };
        result = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(newfn), newfn, args, 1, "fmt.name");
    }

    if (!result) { LLVMPositionBuilderAtEnd(codegen->builder, saved); return NULL; }
    LLVMBuildRet(codegen->builder, result);
    LLVMPositionBuilderAtEnd(codegen->builder, saved);
    return fn;
}

// Per-concrete-type descriptor reached behind interface vtable slot 0
// (Go's itab->_type shape). Layout: { ptr eq_fn, ptr type_name, ptr fmt_fn }.
// eq_fn is FIRST so goo_iface_key_eq's slot-0 hop is a single extra deref.
// fmt_fn is codegen_get_or_emit_type_fmt's per-type %v formatter (above).
// Name-deduped by concrete type, like the vtable globals.
LLVMValueRef codegen_get_or_emit_type_desc(CodeGenerator* codegen, TypeChecker* checker,
                                           Type* concrete, int pointer_form) {
    char gname[256];
    const char* cname = type_receiver_name(concrete);
    if (!cname) cname = type_to_string(concrete);
    if (pointer_form) snprintf(gname, sizeof(gname), "goo.typedesc.$ptr$%s", cname);
    else              snprintf(gname, sizeof(gname), "goo.typedesc.%s", cname);
    LLVMValueRef existing = LLVMGetNamedGlobal(codegen->module, gname);
    if (existing) return existing;

    LLVMTypeRef ptrty = iface_ptr_type(codegen);

    // eq_fn: pointer-boxed form uses pointer identity; value form uses the
    // per-type value comparator — same choice codegen_interface_vtable made.
    LLVMValueRef eq_fn = pointer_form
        ? iface_ptr_eq_fn(codegen)
        : codegen_get_or_emit_type_eq(codegen, checker, concrete);
    if (!eq_fn) return NULL;

    // type_name: a private constant C string. For the pointer form, prefix '*'.
    char tname[256];
    if (pointer_form) snprintf(tname, sizeof(tname), "*%s", cname);
    else              snprintf(tname, sizeof(tname), "%s", cname);
    LLVMValueRef name_str = LLVMBuildGlobalStringPtr(codegen->builder, tname, "typename");
    // NOTE: LLVMBuildGlobalStringPtr needs an insertion point. The descriptor
    // is always emitted while boxing (inside a function), so builder has one.
    // If a builder-free path ever calls this, switch to a module-level constant
    // string global (see codegen_const_string_value in composite_codegen.c).

    LLVMValueRef fmt_fn = codegen_get_or_emit_type_fmt(codegen, checker, concrete, pointer_form);
    if (!fmt_fn) return NULL;

    LLVMValueRef fields[3] = { eq_fn, name_str, fmt_fn };
    LLVMTypeRef descty = LLVMStructTypeInContext(codegen->context,
                                                 (LLVMTypeRef[]){ptrty, ptrty, ptrty}, 3, 0);
    LLVMValueRef init = LLVMConstNamedStruct(descty, fields, 3);
    LLVMValueRef g = LLVMAddGlobal(codegen->module, descty, gname);
    LLVMSetInitializer(g, init);
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetGlobalConstant(g, 1);
    return g;
}

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
    // carries n+1 slots — slot 0 is the per-concrete-type descriptor
    // (codegen_get_or_emit_type_desc; its field 0 is the value-equality
    // comparator), slots 1..n are the method thunks in their original,
    // unchanged order. codegen_interface_dispatch
    // shifts its method GEP index by +1 to match (interface_codegen.c,
    // below); this is the ONLY other site that indexes a vtable by a raw
    // slot number (confirmed by grepping every `goo.vtable`/vtable-indexing
    // site in the codebase — codegen_interface_assert_match only ever
    // compares whole vtable-pointer identity, never indexes into one).
    size_t n = iface->data.interface.method_count;
    LLVMTypeRef ptrty = iface_ptr_type(codegen);
    LLVMValueRef* slots = malloc((n + 1) * sizeof(LLVMValueRef));
    if (!slots) return NULL;

    // slot-0 is now the type descriptor pointer (eq fn lives inside it). For
    // a POINTER-boxed interface (pointer_form) the data word IS the pointer,
    // so equality is POINTER IDENTITY — NOT the pointee's value comparator.
    // Without this, two distinct pointers to equal-content values would
    // compare equal as interface values / map keys (they'd run the pointee's
    // structeq), diverging from Go. `concrete` here is the pointee type (the
    // #114 normalization), so codegen_get_or_emit_type_desc(concrete) would
    // wrongly synthesize the pointee comparator if pointer_form weren't
    // threaded through — it is.
    LLVMValueRef desc = codegen_get_or_emit_type_desc(codegen, checker, concrete, pointer_form);
    if (!desc) { free(slots); return NULL; }
    // Slot 0 is the per-concrete-type descriptor pointer (was the eq fn before
    // the Task-1 refactor). A global's LLVM type is already `ptr` (opaque
    // pointers), same as every thunk placed below — no bitcast needed to
    // satisfy LLVMConstArray(ptrty, ...).
    slots[0] = desc;

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
    // A nil literal boxes to the ZERO interface value {NULL vtable, NULL data}
    // — the nil interface — NOT a heap box of a null. codegen_generate_null_
    // literal types a bare `nil` as *void (or TYPE_UNKNOWN); either would
    // otherwise fall through to the general path below and build a real
    // vtable + goo_alloc, so `f(nil)` produced a NON-nil interface (it compared
    // != nil and disagreed with `switch nil.(type){case nil:}`). `void` has no
    // first-class values, so a *void reaching boxing is always nil. Handling it
    // here covers every boxing site uniformly (call args, struct/slice
    // elements, return values, map values).
    if (!concrete || concrete->kind == TYPE_UNKNOWN ||
        (concrete->kind == TYPE_POINTER && concrete->data.pointer.pointee_type &&
         concrete->data.pointer.pointee_type->kind == TYPE_VOID)) {
        LLVMTypeRef ifacety = codegen_type_to_llvm(codegen, iface);
        if (!ifacety) return NULL;
        return LLVMConstNull(ifacety);  // { NULL, NULL }
    }

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
    LLVMValueRef size = LLVMSizeOf(llvm_T);
    LLVMValueRef data = codegen_emit_alloc(codegen, size, ALLOC_KIND_DEFAULT);
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
    // 0 is the per-concrete-type descriptor pointer (its field 0 is the eq
    // comparator; codegen_interface_vtable above), so method thunks shifted
    // right by one.
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

// Interface-target RTTI, Task 1: enumerate every concrete STRUCT type
// declared at package (top) scope that implements `iface`, for the
// closed-world runtime chain `x.(I)` builds (codegen_interface_target_match,
// below). "Declared at top scope" is detected the same way the type checker
// itself distinguishes a type declaration from an ordinary variable binding:
// `type Point struct{}` registers a Variable named "Point" whose ->type is
// the very struct Type it names (v->type->data.struct_type.name ==
// v->name); an ordinary `var p Point` registers a DIFFERENT variable ("p")
// whose ->type is that same struct Type, so name != struct_type.name for
// it. Walking to the ROOT scope (top-level declarations only) means a
// struct type declared inside a function body is never a candidate — Goo
// (like Go) only allows `type` declarations at package scope in practice,
// so this is not a limitation in v1.
//
// Returns the count and writes a malloc'd array of the matching Type*
// pointers to *out (NULL/0 on failure or no matches) — the caller owns the
// array (a flat list of borrowed Type* — the Types themselves are owned by
// the type checker) and must free() it.
size_t codegen_collect_iface_implementers(TypeChecker* checker, Type* iface, Type*** out) {
    if (out) *out = NULL;
    if (!checker || !iface || iface->kind != TYPE_INTERFACE || !out) return 0;
    if (!checker->current_scope) return 0;

    Scope* s = checker->current_scope;
    while (s->parent) s = s->parent;

    size_t count = 0, cap = 0;
    Type** arr = NULL;
    for (Variable* v = s->variables; v; v = v->next) {
        if (!v->type || v->type->kind != TYPE_STRUCT) continue;
        if (!v->type->data.struct_type.name) continue;
        if (!v->name || strcmp(v->name, v->type->data.struct_type.name) != 0) continue;

        const char* method = NULL;
        const char* reason = NULL;
        if (!type_interface_satisfied(checker, iface, v->type, &method, &reason)) continue;

        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 4;
            Type** narr = realloc(arr, ncap * sizeof(Type*));
            if (!narr) { free(arr); *out = NULL; return 0; }
            arr = narr;
            cap = ncap;
        }
        arr[count++] = v->type;
    }
    *out = arr;
    return count;
}

// Interface-target RTTI, Task 1: the shared match/build primitive for
// `x.(I)` where I is itself an INTERFACE (as opposed to
// codegen_interface_assert_match's concrete-target vtable-pointer compare).
// Closed-world: every concrete implementer of I is known at compile time
// (codegen_collect_iface_implementers above), so this builds a straight-
// line chain comparing `iface_val`'s dynamic-type descriptor (vtable slot 0)
// against each implementer's descriptor, `select`-ing the matching (T,I)
// vtable + reusing the SAME data word into a built I-value as it goes — no
// per-candidate branching, since building a candidate value is side-effect-
// free (mirrors codegen_interface_box's insertvalue boxing shape). Only the
// nil-vtable guard branches (mirrors the type switch's `case nil:` null-
// guard, statement_codegen.c). `iface_val` must already be the LOADED
// {vtable, data} struct value (same is_lvalue-load contract as
// codegen_interface_assert_match's caller). Writes the built target-
// interface value to *built_out (the zero interface value on a miss or a
// nil operand) and returns the i1 match bit. Self-contained: creates its
// own nil-guard branch + join and leaves the builder positioned at the join
// block on return. Returns NULL (and leaves *built_out unset) only on a
// hard internal failure (e.g. `target_iface`'s LLVM type can't be
// resolved) — should not happen for a type-checked interface-target
// assertion.
LLVMValueRef codegen_interface_target_match(CodeGenerator* codegen, TypeChecker* checker,
                                            LLVMValueRef iface_val, Type* target_iface,
                                            LLVMValueRef* built_out) {
    if (!codegen || !checker || !target_iface || target_iface->kind != TYPE_INTERFACE ||
        !built_out) {
        return NULL;
    }

    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef i1ty = LLVMInt1TypeInContext(ctx);
    LLVMTypeRef ptrty = iface_ptr_type(codegen);
    LLVMTypeRef target_llvm = codegen_type_to_llvm(codegen, target_iface);
    if (!target_llvm) return NULL;
    LLVMValueRef zero_built = LLVMConstNull(target_llvm);

    LLVMValueRef vtab = LLVMBuildExtractValue(codegen->builder, iface_val, 0, "itm.vt");
    LLVMValueRef data = LLVMBuildExtractValue(codegen->builder, iface_val, 1, "itm.data");
    LLVMValueRef null_vt = LLVMConstNull(ptrty);
    LLVMValueRef is_null = LLVMBuildICmp(codegen->builder, LLVMIntEQ, vtab, null_vt, "itm.isnull");

    LLVMBasicBlockRef entry_bb   = LLVMGetInsertBlock(codegen->builder);
    LLVMBasicBlockRef nonnull_bb = codegen_create_block(codegen, "itm.nonnull");
    LLVMBasicBlockRef join_bb    = codegen_create_block(codegen, "itm.join");
    if (!nonnull_bb || !join_bb) return NULL;
    LLVMBuildCondBr(codegen->builder, is_null, join_bb, nonnull_bb);

    codegen_set_insert_point(codegen, nonnull_bb);
    // desc_have: GEP element 0 of the vtable array, then load the ptr stored
    // there — vtable slot 0 is the per-concrete-type descriptor pointer
    // (codegen_get_or_emit_type_desc), which IS the dynamic-type identity.
    LLVMValueRef zero_idx = LLVMConstInt(LLVMInt64TypeInContext(ctx), 0, 0);
    LLVMValueRef slot0 = LLVMBuildGEP2(codegen->builder, ptrty, vtab, &zero_idx, 1, "itm.slot0");
    LLVMValueRef desc_have = LLVMBuildLoad2(codegen->builder, ptrty, slot0, "itm.desc");

    LLVMValueRef match_acc = LLVMConstInt(i1ty, 0, 0);
    LLVMValueRef built_acc = zero_built;

    if (target_iface->data.interface.method_count == 0) {
        // Method-less-target short-circuit (empty-interface RTTI fix):
        // `interface{}`/`any` (or any named interface with zero methods) is
        // satisfied by EVERY non-nil interface value, including ones whose
        // dynamic type is NOT a declared struct (int, string, float,
        // pointer, slice, ...). The enumeration loop below only ever walks
        // declared TYPE_STRUCT types (codegen_collect_iface_implementers),
        // so for a method-less target it would enumerate nothing and every
        // non-struct dynamic value would wrongly miss — running it here
        // would be at best a no-op and at worst wrong, so skip it entirely.
        // Reuse the operand's OWN {vtab, data} as the built value rather
        // than looking up a (T, target) vtable: T is unknown for non-struct
        // dynamic types, so no such vtable could even be found. This is
        // still correct because a method-less target never dispatches
        // through its vtable — the only thing callers rely on is
        // built.vtable[0], the dynamic-type descriptor, which `vtab`
        // already carries unchanged.
        match_acc = LLVMConstInt(i1ty, 1, 0);
        LLVMValueRef iv_any = LLVMGetUndef(target_llvm);
        iv_any = LLVMBuildInsertValue(codegen->builder, iv_any, vtab, 0, "itm.anyvt");
        iv_any = LLVMBuildInsertValue(codegen->builder, iv_any, data, 1, "itm.anydata");
        built_acc = iv_any;
    } else {
        Type** impls = NULL;
        size_t n = codegen_collect_iface_implementers(checker, target_iface, &impls);
        for (size_t i = 0; i < n; i++) {
            Type* T = impls[i];
            // base_of(T) mirrors codegen_interface_assert_match's pointer-
            // candidate normalization (interface_codegen.c, above):
            // codegen_collect_iface_implementers only ever yields TYPE_STRUCT
            // candidates today, so this is a no-op in practice — kept so a
            // future pointer-receiver-aware collector needs no change here.
            Type* base = T;
            if (base->kind == TYPE_POINTER && base->data.pointer.pointee_type &&
                type_receiver_name(base->data.pointer.pointee_type)) {
                base = base->data.pointer.pointee_type;
            }

            // Task 4 fix: try BOTH the value-form and pointer-form descriptor/
            // vtable for every candidate, not just value-form. `T`'s presence in
            // `impls` means type_interface_satisfied found a matching method —
            // but that check is permissive about receiver kind (it mangles on
            // the struct's name alone, never inspecting whether the method's
            // receiver is `T` or `*T`; see codegen_collect_iface_implementers's
            // caller, type_interface_satisfied, type_checker.c ~821). So a
            // pointer-receiver implementer like `func (c *C) Get()` satisfies
            // the interface in the checker's eyes regardless of whether the
            // runtime value in hand is value-boxed (`T{...}`) or pointer-boxed
            // (`&T{...}`) — those two boxings carry DISTINCT descriptor
            // identities (codegen_interface_box's two branches; Task 5's
            // `$ptr$` naming). Emitting only the value-form candidate (as
            // before) silently misses every pointer-boxed implementer — exactly
            // examples/iface_target_ptr.goo's `&C{n: 5}` shape. A given runtime
            // descriptor matches at most one of the two forms, so trying both is
            // safe: exactly one `eq` (or neither, on a genuine miss) is true.
            for (int form = 0; form <= 1; form++) {
                LLVMValueRef desc_T = codegen_get_or_emit_type_desc(codegen, checker, base, form);
                LLVMValueRef vt_TI = codegen_interface_vtable(codegen, checker, target_iface, base, form);
                if (!desc_T || !vt_TI) continue;

                LLVMValueRef eq = LLVMBuildICmp(codegen->builder, LLVMIntEQ, desc_have, desc_T, "itm.eq");

                // iv_T = {vt_TI, data} — side-effect-free (insertvalue only), so it's
                // safe to build for every candidate unconditionally and `select` the
                // winner, avoiding a basic block per implementer. `data` is reused
                // AS-IS — it's the same data word the concrete boxing site produced,
                // and I's thunks read it exactly that way (no re-box/copy).
                LLVMValueRef iv_T = LLVMGetUndef(target_llvm);
                iv_T = LLVMBuildInsertValue(codegen->builder, iv_T, vt_TI, 0, "itm.ivvt");
                iv_T = LLVMBuildInsertValue(codegen->builder, iv_T, data, 1, "itm.ivdata");

                built_acc = LLVMBuildSelect(codegen->builder, eq, iv_T, built_acc, "itm.built");
                match_acc = LLVMBuildOr(codegen->builder, match_acc, eq, "itm.match");
            }
        }
        free(impls);
    }

    LLVMBasicBlockRef nonnull_exit_bb = LLVMGetInsertBlock(codegen->builder);
    LLVMBuildBr(codegen->builder, join_bb);

    codegen_set_insert_point(codegen, join_bb);
    LLVMValueRef match_phi = LLVMBuildPhi(codegen->builder, i1ty, "itm.match.phi");
    LLVMValueRef match_vals[2]        = { LLVMConstInt(i1ty, 0, 0), match_acc };
    LLVMBasicBlockRef match_blocks[2] = { entry_bb, nonnull_exit_bb };
    LLVMAddIncoming(match_phi, match_vals, match_blocks, 2);

    LLVMValueRef built_phi = LLVMBuildPhi(codegen->builder, target_llvm, "itm.built.phi");
    LLVMValueRef built_vals[2]        = { zero_built, built_acc };
    LLVMBasicBlockRef built_blocks[2] = { entry_bb, nonnull_exit_bb };
    LLVMAddIncoming(built_phi, built_vals, built_blocks, 2);

    *built_out = built_phi;
    return match_phi;
}

#endif // LLVM_AVAILABLE
