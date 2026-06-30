#include "codegen.h"
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
    char thunk_name[256];
    snprintf(thunk_name, sizeof(thunk_name), "goo.thunk.%s.%s.%s",
             concrete_name, iface_name, im->name);
    LLVMValueRef existing = LLVMGetNamedFunction(codegen->module, thunk_name);
    if (existing) return existing;

    LLVMTypeRef ret_llvm = NULL;
    LLVMTypeRef fnty = thunk_fn_type(codegen, im->type, &ret_llvm);
    if (!fnty) return NULL;
    LLVMValueRef thunk = LLVMAddFunction(codegen->module, thunk_name, fnty);

    // Resolve the real method T__m and its registered receiver kind.
    char* mangled = type_method_mangled_name(concrete_name, im->name);
    LLVMValueRef real_fn = mangled ? LLVMGetNamedFunction(codegen->module, mangled) : NULL;
    Variable* mvar = mangled ? type_checker_lookup_variable(checker, mangled) : NULL;
    free(mangled);
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

    // Receiver: a pointer receiver takes the data pointer; a value receiver
    // loads the concrete value from it.
    if (ptr_recv) {
        call_args[0] = data;
    } else {
        LLVMTypeRef llvm_T = codegen_type_to_llvm(codegen, concrete);
        call_args[0] = llvm_T ? LLVMBuildLoad2(codegen->builder, llvm_T, data, "recv")
                              : data;
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

// Build (or reuse) the vtable global for boxing `concrete` into `iface`. Returns
// the global (a ptr to the [N x ptr] thunk array), or NULL on failure.
LLVMValueRef codegen_interface_vtable(CodeGenerator* codegen, TypeChecker* checker,
                                      Type* iface, Type* concrete) {
    if (!iface || iface->kind != TYPE_INTERFACE) return NULL;
    const char* cname = type_receiver_name(concrete);
    const char* iname = iface->data.interface.name ? iface->data.interface.name : "iface";
    if (!cname) {
        codegen_error(codegen, (Position){0},
                      "internal: cannot name concrete type for interface vtable");
        return NULL;
    }

    char gname[256];
    snprintf(gname, sizeof(gname), "goo.vtable.%s.%s", cname, iname);
    LLVMValueRef existing = LLVMGetNamedGlobal(codegen->module, gname);
    if (existing) return existing;

    size_t n = iface->data.interface.method_count;
    LLVMTypeRef ptrty = iface_ptr_type(codegen);
    LLVMValueRef* slots = n ? malloc(n * sizeof(LLVMValueRef)) : NULL;
    size_t i = 0;
    for (InterfaceMethod* im = iface->data.interface.methods; im; im = im->next, i++) {
        LLVMValueRef thunk = build_thunk(codegen, checker, concrete, cname, iname, im);
        if (!thunk) { free(slots); return NULL; }
        slots[i] = thunk;  // a function value is a ptr constant
    }

    LLVMTypeRef arrty = LLVMArrayType(ptrty, (unsigned)n);
    LLVMValueRef init = LLVMConstArray(ptrty, slots, (unsigned)n);
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
    LLVMValueRef vt = codegen_interface_vtable(codegen, checker, iface, concrete);
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

    // Load the thunk pointer from vtable slot `idx` (array of ptr).
    LLVMTypeRef ptrty = iface_ptr_type(codegen);
    LLVMValueRef gep_idx = LLVMConstInt(LLVMInt64TypeInContext(codegen->context), idx, 0);
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

#endif // LLVM_AVAILABLE
