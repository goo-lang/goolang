#include "codegen.h"
#include <stdlib.h>
#include <string.h>

#if LLVM_AVAILABLE

// Type mapping from Goo types to LLVM types

// Function-generics Task 8: resolve a TYPE_PARAM through the codegen's
// active substitution environment (Task 9/10 populate active_subst /
// active_subst_n around a monomorphized instantiation's codegen). Identity
// for any non-TYPE_PARAM type, and identity for a TYPE_PARAM when there is
// no active env, the index is out of range, or that slot is unbound
// (NULL) — callers detect "unbound" by comparing the result against `t`.
const Type* codegen_resolve_type(CodeGenerator* codegen, const Type* t) {
    if (t && t->kind == TYPE_PARAM && codegen->active_subst) {
        int i = t->data.type_param.index;
        if (i >= 0 && (size_t)i < codegen->active_subst_n && codegen->active_subst[i])
            return codegen->active_subst[i];
    }
    return t;
}

LLVMTypeRef codegen_type_to_llvm(CodeGenerator* codegen, const Type* type) {
    if (!codegen || !type) return NULL;

    switch (type->kind) {
        case TYPE_PARAM: {
            // Never occurs on the non-generic path (active_subst == NULL
            // there), so this branch is exercised starting with Task 9's
            // monomorphization worklist. Recursing on the resolved concrete
            // type lets it be anything codegen_type_to_llvm otherwise
            // handles, including another (bound) TYPE_PARAM one level in.
            const Type* r = codegen_resolve_type(codegen, type);
            if (r == type) return NULL; // unbound TYPE_PARAM at lowering = internal error
            return codegen_type_to_llvm(codegen, r);
        }

        case TYPE_VOID:
        case TYPE_BOOL:
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT32:
        case TYPE_INT64:
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64:
        case TYPE_FLOAT32:
        case TYPE_FLOAT64:
        case TYPE_CHAR:
            return codegen_get_basic_type(codegen, type->kind);
            
        case TYPE_STRING:
            // String is represented as { i8*, i64 } (pointer + length)
            return LLVMStructTypeInContext(codegen->context, 
                (LLVMTypeRef[]){
                    LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),
                    LLVMInt64TypeInContext(codegen->context)
                }, 2, 0);
            
        case TYPE_ARRAY:
            return codegen_get_array_type(codegen, type);
            
        case TYPE_SLICE:
            // Slice is represented as { T*, i64, i64 } (pointer + length +
            // capacity) to match the runtime goo_slice_t (include/runtime.h).
            // The capacity field lets append() grow in place; field order is
            // load-bearing: ptr=0, len=1, cap=2.
            {
                LLVMTypeRef element_type = codegen_type_to_llvm(codegen, type->data.slice.element_type);
                if (!element_type) return NULL;

                return LLVMStructTypeInContext(codegen->context,
                    (LLVMTypeRef[]){
                        LLVMPointerType(element_type, 0),
                        LLVMInt64TypeInContext(codegen->context),
                        LLVMInt64TypeInContext(codegen->context)
                    }, 3, 0);
            }
            
        case TYPE_MAP:
            // Map is represented as an opaque pointer for now
            return LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
            
        case TYPE_CHANNEL:
            return codegen_get_channel_type(codegen, type);
            
        case TYPE_FUNCTION:
            // Universal fat-pointer VALUE representation (Branch A of the
            // closures design — docs/superpowers/specs/2026-07-03-closures-
            // design.md "Representation"). This dispatcher case fires for
            // every VALUE use of a function type reached generically:
            // parameter, local, struct field, slice element, return type.
            // It is DELIBERATELY DISTINCT from codegen_get_function_type
            // (below), which stays the raw LLVM function type for SIGNATURE
            // uses — a function's own declaration, and the callee of a
            // direct call / go statement, both of which resolve their
            // callee's Goo type via codegen_resolve_callee /
            // codegen_lookup_global_function (call_codegen.c) and therefore
            // never reach this dispatcher case for the callee itself.
            return codegen_get_funcval_pair_type(codegen);
            
        case TYPE_POINTER:
            return codegen_get_pointer_type(codegen, type);
            
        case TYPE_STRUCT:
            return codegen_get_struct_type(codegen, type);

        case TYPE_ENUM:
            return codegen_get_enum_type(codegen, type);
            
        case TYPE_INTERFACE:
            // Interface is represented as { vtable*, data* }
            return LLVMStructTypeInContext(codegen->context,
                (LLVMTypeRef[]){
                    LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),  // vtable
                    LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0)   // data
                }, 2, 0);
            
        case TYPE_ERROR_UNION:
            return codegen_get_error_union_type(codegen, type);
            
        case TYPE_NULLABLE:
            return codegen_get_nullable_type(codegen, type);
            
        case TYPE_QUALIFIED:
            // For qualified types, just use the base type
            return codegen_type_to_llvm(codegen, type->data.qualified.base_type);
            
        default:
            return NULL;
    }
}

LLVMTypeRef codegen_get_basic_type(CodeGenerator* codegen, TypeKind kind) {
    if (!codegen) return NULL;
    
    switch (kind) {
        case TYPE_VOID:
            return LLVMVoidTypeInContext(codegen->context);
        case TYPE_BOOL:
            return LLVMInt1TypeInContext(codegen->context);
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_CHAR:
            return LLVMInt8TypeInContext(codegen->context);
        case TYPE_INT16:
        case TYPE_UINT16:
            return LLVMInt16TypeInContext(codegen->context);
        case TYPE_INT32:
        case TYPE_UINT32:
            return LLVMInt32TypeInContext(codegen->context);
        case TYPE_INT64:
        case TYPE_UINT64:
            return LLVMInt64TypeInContext(codegen->context);
        case TYPE_FLOAT32:
            return LLVMFloatTypeInContext(codegen->context);
        case TYPE_FLOAT64:
            return LLVMDoubleTypeInContext(codegen->context);
        case TYPE_STRING:
            // String is { i8*, i64 } — must match codegen_type_to_llvm.
            // codegen_get_basic_type used to return NULL here, causing
            // a SIGSEGV in string-literal codegen via LLVMGetUndef(NULL).
            return LLVMStructTypeInContext(codegen->context,
                (LLVMTypeRef[]){
                    LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),
                    LLVMInt64TypeInContext(codegen->context)
                }, 2, 0);
        default:
            return NULL;
    }
}

LLVMTypeRef codegen_get_array_type(CodeGenerator* codegen, const Type* type) {
    if (!codegen || !type || type->kind != TYPE_ARRAY) return NULL;
    
    LLVMTypeRef element_type = codegen_type_to_llvm(codegen, type->data.array.element_type);
    if (!element_type) return NULL;
    
    return LLVMArrayType(element_type, (unsigned)type->data.array.length);
}

LLVMTypeRef codegen_get_struct_type(CodeGenerator* codegen, const Type* type) {
    if (!codegen || !type || type->kind != TYPE_STRUCT) return NULL;

    // Anonymous structs (multi-return tuples and named-return result structs
    // have no user name) lower to a LITERAL struct type, which LLVM uniques by
    // structure: two independent type_from_ast() results with identical fields
    // yield the SAME LLVMTypeRef. This is what lets a forward-reference
    // prototype and the function body agree on a tuple return type — a named
    // struct would mint distinct `anon`/`anon.1` types keyed on the (differing)
    // Type* pointer and fail the verifier. Anonymous structs cannot be recursive
    // (no name to refer back to), so the opaque-cache cycle break is unneeded.
    if (!type->data.struct_type.name && type->data.struct_type.field_count > 0) {
        size_t fc = type->data.struct_type.field_count;
        LLVMTypeRef* fts = malloc(sizeof(LLVMTypeRef) * fc);
        if (!fts) return NULL;
        for (size_t i = 0; i < fc; i++) {
            fts[i] = codegen_type_to_llvm(codegen, type->data.struct_type.fields[i].type);
            if (!fts[i]) { free(fts); return NULL; }
        }
        LLVMTypeRef lit = LLVMStructTypeInContext(codegen->context, fts, (unsigned)fc, 0);
        free(fts);
        return lit;
    }

    // Consult the struct cache first. The cache stores (Type* -> LLVMTypeRef)
    // pairs. For recursive struct types (e.g. `type Node struct { next *Node }`)
    // the cache entry is inserted BEFORE resolving the field types, so that
    // when codegen_type_to_llvm is called recursively for `*Node` it finds the
    // opaque named struct in the cache and returns it immediately instead of
    // recursing infinitely.
    for (size_t i = 0; i < codegen->struct_cache_size; i++) {
        if (codegen->struct_cache_keys[i] == type)
            return codegen->struct_cache_vals[i];
    }

    // Create an opaque named struct and insert it into the cache BEFORE
    // resolving the body. This breaks potential cycles.
    const char* name = type->data.struct_type.name ? type->data.struct_type.name : "anon";
    LLVMTypeRef opaque = LLVMStructCreateNamed(codegen->context, name);
    if (!opaque) return NULL;

    // Grow the cache if needed.
    if (codegen->struct_cache_size == codegen->struct_cache_cap) {
        size_t new_cap = codegen->struct_cache_cap ? codegen->struct_cache_cap * 2 : 8;
        const Type** new_keys = realloc(codegen->struct_cache_keys, new_cap * sizeof(const Type*));
        if (!new_keys) return NULL;
        codegen->struct_cache_keys = new_keys;
        LLVMTypeRef* new_vals = realloc(codegen->struct_cache_vals, new_cap * sizeof(LLVMTypeRef));
        if (!new_vals) return NULL;
        codegen->struct_cache_vals = new_vals;
        codegen->struct_cache_cap = new_cap;
    }
    codegen->struct_cache_keys[codegen->struct_cache_size] = type;
    codegen->struct_cache_vals[codegen->struct_cache_size] = opaque;
    codegen->struct_cache_size++;

    if (type->data.struct_type.field_count == 0) {
        // Empty struct: leave the named struct body-less (zero fields).
        LLVMStructSetBody(opaque, NULL, 0, 0);
        return opaque;
    }

    // Resolve field types now that the opaque entry is in the cache.
    LLVMTypeRef* field_types = malloc(sizeof(LLVMTypeRef) * type->data.struct_type.field_count);
    if (!field_types) return NULL;

    for (size_t i = 0; i < type->data.struct_type.field_count; i++) {
        StructField* field = &type->data.struct_type.fields[i];
        field_types[i] = codegen_type_to_llvm(codegen, field->type);
        if (!field_types[i]) {
            free(field_types);
            return NULL;
        }
    }

    // Populate the opaque struct body.
    LLVMStructSetBody(opaque, field_types, (unsigned)type->data.struct_type.field_count, 0);
    free(field_types);
    return opaque;
}

LLVMTypeRef codegen_get_enum_type(CodeGenerator* codegen, const Type* type) {
    if (!codegen || !type || type->kind != TYPE_ENUM) return NULL;
    // { i32 tag, [N x i8] payload } where N covers the largest variant.
    size_t tag_slot = (type->align > 4) ? type->align : 4;
    size_t payload_bytes = (type->size > tag_slot) ? (type->size - tag_slot) : 0;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(codegen->context);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(codegen->context);
    LLVMTypeRef payload = LLVMArrayType(i8, (unsigned)payload_bytes);
    LLVMTypeRef members[2] = { i32, payload };
    return LLVMStructTypeInContext(codegen->context, members, 2, 0);
}

LLVMTypeRef codegen_get_function_type(CodeGenerator* codegen, const Type* type) {
    if (!codegen || !type || type->kind != TYPE_FUNCTION) return NULL;
    
    // Convert return type
    LLVMTypeRef return_type = codegen_type_to_llvm(codegen, type->data.function.return_type);
    if (!return_type) return NULL;
    
    // Convert parameter types
    LLVMTypeRef* param_types = NULL;
    if (type->data.function.param_count > 0) {
        param_types = malloc(sizeof(LLVMTypeRef) * type->data.function.param_count);
        if (!param_types) return NULL;
        
        for (size_t i = 0; i < type->data.function.param_count; i++) {
            param_types[i] = codegen_type_to_llvm(codegen, type->data.function.param_types[i]);
            if (!param_types[i]) {
                free(param_types);
                return NULL;
            }
        }
    }
    
    LLVMTypeRef func_type = LLVMFunctionType(return_type, param_types,
                                           (unsigned)type->data.function.param_count,
                                           type->data.function.is_variadic);

    free(param_types);
    return func_type;
}

// The universal function-VALUE representation: an anonymous (literal)
// `{ ptr fn, ptr env }` struct — 16 bytes, two SysV INTEGER-class eightbytes,
// passed BY VALUE (does not trigger the >16-byte by-pointer ABI rule the
// runtime slice/string structs follow — verified against goo_string_t, the
// existing {ptr,i64} 16-byte precedent passed by value throughout this
// codebase and across the C runtime boundary). env is FIRST in the pair AND
// first in every indirect call's argument list (`fn_ptr(env_ptr, args...)`,
// call_codegen.c) — this is a change-together contract Branch B (closures)
// builds on unseen: reordering either position breaks every closure
// literal's calling convention. See docs/superpowers/specs/
// 2026-07-03-closures-design.md "Representation".
//
// LITERAL (unnamed) struct types with identical field lists are
// automatically uniqued by LLVM within a context — every call site that
// builds this type (here; codegen_get_func_thunk, function_codegen.c; the
// indirect-call site in call_codegen.c, via codegen_get_funcval_call_type
// below) gets the SAME LLVMTypeRef with no caching of our own, exactly like
// TYPE_STRING's `{ptr,i64}` struct above.
LLVMTypeRef codegen_get_funcval_pair_type(CodeGenerator* codegen) {
    if (!codegen) return NULL;
    LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
    LLVMTypeRef fields[] = { ptr, ptr };
    return LLVMStructTypeInContext(codegen->context, fields, 2, 0);
}

// The LLVM function type for CALLING THROUGH a function value: `fn_type`'s
// signature with an i8* env parameter PREPENDED (env FIRST — see
// codegen_get_funcval_pair_type above). Shared by codegen_get_func_thunk
// (function_codegen.c, which uses it as the thunk's OWN definition type) and
// the indirect-call site in codegen_generate_call_expr (call_codegen.c,
// which uses it as the LLVMBuildCall2 call-through type for `fn_ptr`) — the
// two must agree byte-for-byte or an indirect call through a thunk-wrapped
// named function builds mismatched IR.
LLVMTypeRef codegen_get_funcval_call_type(CodeGenerator* codegen, const Type* fn_type) {
    if (!codegen || !fn_type || fn_type->kind != TYPE_FUNCTION) return NULL;

    LLVMTypeRef return_type = codegen_type_to_llvm(codegen, fn_type->data.function.return_type);
    if (!return_type) return NULL;

    size_t param_count = fn_type->data.function.param_count;
    LLVMTypeRef* param_types = malloc(sizeof(LLVMTypeRef) * (param_count + 1));
    if (!param_types) return NULL;
    param_types[0] = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);  // env — FIRST
    for (size_t i = 0; i < param_count; i++) {
        param_types[i + 1] = codegen_type_to_llvm(codegen, fn_type->data.function.param_types[i]);
        if (!param_types[i + 1]) {
            free(param_types);
            return NULL;
        }
    }

    LLVMTypeRef call_type = LLVMFunctionType(return_type, param_types,
                                             (unsigned)(param_count + 1),
                                             fn_type->data.function.is_variadic);
    free(param_types);
    return call_type;
}

LLVMTypeRef codegen_get_pointer_type(CodeGenerator* codegen, const Type* type) {
    if (!codegen || !type || type->kind != TYPE_POINTER) return NULL;
    
    LLVMTypeRef pointee_type = codegen_type_to_llvm(codegen, type->data.pointer.pointee_type);
    if (!pointee_type) return NULL;
    
    return LLVMPointerType(pointee_type, 0);
}

// Special Goo type mappings

LLVMTypeRef codegen_get_error_union_type(CodeGenerator* codegen, const Type* type) {
    if (!codegen || !type || type->kind != TYPE_ERROR_UNION) return NULL;
    
    // Error union is represented as { i1, value_type } where i1 indicates if it's an error
    LLVMTypeRef value_type = codegen_type_to_llvm(codegen, type->data.error_union.value_type);
    if (!value_type) return NULL;

    // We need to handle both the value and error cases
    // For simplicity, we'll use a tagged union: { i1 is_error, union { value, error } }

    // Default error type is goo_string_t {i8*, i64} so the error message carries
    // both pointer and length. This allows the catch error-var binding to use the
    // value directly without an extra strlen call. Explicit error types override.
    LLVMTypeRef error_type = codegen_get_basic_type(codegen, TYPE_STRING);
    if (type->data.error_union.error_type) {
        LLVMTypeRef explicit_type = codegen_type_to_llvm(codegen, type->data.error_union.error_type);
        if (explicit_type) {
            error_type = explicit_type;
        }
    }
    
    // Create union of value and error types
    LLVMTypeRef union_types[] = { value_type, error_type };
    LLVMTypeRef union_type = LLVMStructTypeInContext(codegen->context, union_types, 2, 0);
    
    // Create error union struct: { i1 is_error, union data }
    LLVMTypeRef error_union_types[] = {
        LLVMInt1TypeInContext(codegen->context),  // is_error flag
        union_type                                // data union
    };
    
    return LLVMStructTypeInContext(codegen->context, error_union_types, 2, 0);
}

LLVMTypeRef codegen_get_nullable_type(CodeGenerator* codegen, const Type* type) {
    if (!codegen || !type || type->kind != TYPE_NULLABLE) return NULL;
    
    // Nullable type is represented as { i1, value_type } where i1 indicates if it's null
    LLVMTypeRef base_type = codegen_type_to_llvm(codegen, type->data.nullable.base_type);
    if (!base_type) return NULL;
    
    LLVMTypeRef nullable_types[] = {
        LLVMInt1TypeInContext(codegen->context),  // is_null flag
        base_type                                 // actual value
    };
    
    return LLVMStructTypeInContext(codegen->context, nullable_types, 2, 0);
}

LLVMTypeRef codegen_get_channel_type(CodeGenerator* codegen, const Type* type) {
    if (!codegen || !type || type->kind != TYPE_CHANNEL) return NULL;
    
    // Channel is represented as an opaque pointer to runtime channel structure
    // The runtime will handle the actual channel implementation
    return LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
}

// Helper functions for error unions

LLVMValueRef codegen_create_error_union_value(CodeGenerator* codegen, LLVMTypeRef union_type,
                                            LLVMValueRef value, int is_error) {
    if (!codegen || !union_type || !value) return NULL;
    
    // Create an undef value of the union type
    LLVMValueRef union_val = LLVMGetUndef(union_type);
    
    // Set the is_error flag
    LLVMValueRef is_error_val = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), is_error ? 1 : 0, 0);
    union_val = LLVMBuildInsertValue(codegen->builder, union_val, is_error_val, 0, "");
    
    // Set the data value
    LLVMValueRef data_union = LLVMGetUndef(LLVMStructGetTypeAtIndex(union_type, 1));
    if (is_error) {
        data_union = LLVMBuildInsertValue(codegen->builder, data_union, value, 1, ""); // error in slot 1
    } else {
        data_union = LLVMBuildInsertValue(codegen->builder, data_union, value, 0, ""); // value in slot 0
    }
    union_val = LLVMBuildInsertValue(codegen->builder, union_val, data_union, 1, "");
    
    return union_val;
}

LLVMValueRef codegen_extract_error_union_value(CodeGenerator* codegen, LLVMValueRef union_value, int get_error) {
    if (!codegen || !union_value) return NULL;
    
    // Extract the data union
    LLVMValueRef data_union = LLVMBuildExtractValue(codegen->builder, union_value, 1, "");
    
    // Extract either the value (index 0) or error (index 1)
    return LLVMBuildExtractValue(codegen->builder, data_union, get_error ? 1 : 0, "");
}

LLVMValueRef codegen_check_error_union(CodeGenerator* codegen, LLVMValueRef union_value) {
    if (!codegen || !union_value) return NULL;
    
    // Extract the is_error flag
    return LLVMBuildExtractValue(codegen->builder, union_value, 0, "is_error");
}

// Helper functions for nullable types

LLVMValueRef codegen_create_nullable_value(CodeGenerator* codegen, LLVMTypeRef nullable_type, 
                                         LLVMValueRef value, int is_null) {
    if (!codegen || !nullable_type) return NULL;
    
    // Create an undef value of the nullable type
    LLVMValueRef nullable_val = LLVMGetUndef(nullable_type);
    
    // Set the is_null flag
    LLVMValueRef is_null_val = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), is_null ? 1 : 0, 0);
    nullable_val = LLVMBuildInsertValue(codegen->builder, nullable_val, is_null_val, 0, "");
    
    // Set the value (even if null, we still store something for memory layout consistency)
    if (value) {
        nullable_val = LLVMBuildInsertValue(codegen->builder, nullable_val, value, 1, "");
    }
    
    return nullable_val;
}

LLVMValueRef codegen_extract_nullable_value(CodeGenerator* codegen, LLVMValueRef nullable_value) {
    if (!codegen || !nullable_value) return NULL;
    
    // Extract the actual value (index 1)
    return LLVMBuildExtractValue(codegen->builder, nullable_value, 1, "value");
}

LLVMValueRef codegen_check_nullable_null(CodeGenerator* codegen, LLVMValueRef nullable_value) {
    if (!codegen || !nullable_value) return NULL;
    
    // Extract the is_null flag (index 0)
    return LLVMBuildExtractValue(codegen->builder, nullable_value, 0, "is_null");
}

// Conversion and casting
LLVMValueRef codegen_convert_value(CodeGenerator* codegen, LLVMValueRef value, 
                                 LLVMTypeRef from_type, LLVMTypeRef to_type) {
    if (!codegen || !value || !from_type || !to_type) return NULL;
    
    // If types are the same, no conversion needed
    if (from_type == to_type) return value;
    
    LLVMTypeKind from_kind = LLVMGetTypeKind(from_type);
    LLVMTypeKind to_kind = LLVMGetTypeKind(to_type);
    
    // Integer conversions
    if (from_kind == LLVMIntegerTypeKind && to_kind == LLVMIntegerTypeKind) {
        unsigned from_bits = LLVMGetIntTypeWidth(from_type);
        unsigned to_bits = LLVMGetIntTypeWidth(to_type);
        
        if (from_bits < to_bits) {
            // Sign-extend on widening: signed-bias convention, consistent with
            // this function's float branches (SIToFP/FPToSI) and every other
            // integer-widening site in codegen (erru_sext, ret_sext). Zero-
            // extending would turn a narrow negative (e.g. int32 -1) into a
            // large positive on widening to int64.
            return LLVMBuildSExt(codegen->builder, value, to_type, "sext");
        } else if (from_bits > to_bits) {
            // Truncate
            return LLVMBuildTrunc(codegen->builder, value, to_type, "trunc");
        }
        return value;
    }
    
    // Integer to float
    if (from_kind == LLVMIntegerTypeKind && (to_kind == LLVMFloatTypeKind || to_kind == LLVMDoubleTypeKind)) {
        // Assume signed conversion for now
        return LLVMBuildSIToFP(codegen->builder, value, to_type, "sitofp");
    }
    
    // Float to integer  
    if ((from_kind == LLVMFloatTypeKind || from_kind == LLVMDoubleTypeKind) && to_kind == LLVMIntegerTypeKind) {
        // Assume signed conversion for now
        return LLVMBuildFPToSI(codegen->builder, value, to_type, "fptosi");
    }
    
    // Float conversions
    if ((from_kind == LLVMFloatTypeKind || from_kind == LLVMDoubleTypeKind) && 
        (to_kind == LLVMFloatTypeKind || to_kind == LLVMDoubleTypeKind)) {
        if (from_kind == LLVMFloatTypeKind && to_kind == LLVMDoubleTypeKind) {
            return LLVMBuildFPExt(codegen->builder, value, to_type, "fpext");
        } else if (from_kind == LLVMDoubleTypeKind && to_kind == LLVMFloatTypeKind) {
            return LLVMBuildFPTrunc(codegen->builder, value, to_type, "fptrunc");
        }
        return value;
    }
    
    // TODO: Add more conversion cases as needed
    return value;
}

int codegen_types_compatible(LLVMTypeRef from, LLVMTypeRef to) {
    if (!from || !to) return 0;
    
    // Same type
    if (from == to) return 1;
    
    LLVMTypeKind from_kind = LLVMGetTypeKind(from);
    LLVMTypeKind to_kind = LLVMGetTypeKind(to);
    
    // Numeric types are generally compatible
    if ((from_kind == LLVMIntegerTypeKind || from_kind == LLVMFloatTypeKind || from_kind == LLVMDoubleTypeKind) &&
        (to_kind == LLVMIntegerTypeKind || to_kind == LLVMFloatTypeKind || to_kind == LLVMDoubleTypeKind)) {
        return 1;
    }
    
    // Pointers to compatible types
    if (from_kind == LLVMPointerTypeKind && to_kind == LLVMPointerTypeKind) {
        // For now, assume all pointers are compatible
        return 1;
    }
    
    return 0;
}

#endif