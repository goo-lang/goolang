#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Composite-data lowering: index expressions (array/slice/map),
// struct field selectors, struct literals, and slice literals.
// Split from expression_codegen.c (refactor, no behavior change).

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
        LLVMValueRef kp = index_val->llvm_value;
        if (index_val->goo_type && index_val->goo_type->kind == TYPE_STRING) {
            kp = LLVMBuildExtractValue(codegen->builder, kp, 0, "k_ptr");
        }
        LLVMValueRef args[2] = { base_val->llvm_value, kp };
        LLVMValueRef slot = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(get_fn),
                                           get_fn, args, 2, "map_get");
        Type* val_type = base_type->data.map.value_type;
        result = codegen_map_slot_to_value(codegen, slot, val_type);
        value_info_free(base_val);
        value_info_free(index_val);
        return value_info_new(NULL, result, val_type);
    }

    // Handle different indexed types
    switch (base_type->kind) {
        case TYPE_ARRAY: {
            element_type = base_type->data.array.element_type;
            
            // For arrays, generate GEP
            LLVMValueRef indices[] = {
                LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0),  // Array base
                index_val->llvm_value  // Array index
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
            
            // Slices are structs with { ptr, len, cap }
            // Extract the data pointer (field 0)
            LLVMValueRef slice_ptr;
            if (base_val->is_lvalue) {
                // Load the slice struct
                LLVMValueRef slice_val = LLVMBuildLoad2(codegen->builder,
                                                       codegen_type_to_llvm(codegen, base_type),
                                                       base_val->llvm_value, "slice_load");
                slice_ptr = LLVMBuildExtractValue(codegen->builder, slice_val, 0, "slice_ptr");
            } else {
                slice_ptr = LLVMBuildExtractValue(codegen->builder, base_val->llvm_value, 0, "slice_ptr");
            }
            
            // Generate bounds check if in safe mode
            // TODO: Add runtime bounds checking
            
            // Index into the slice data
            result = LLVMBuildGEP2(codegen->builder,
                                  codegen_type_to_llvm(codegen, element_type),
                                  slice_ptr, &index_val->llvm_value, 1, "slice_elem");
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
                                  string_ptr, &index_val->llvm_value, 1, "string_char");
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
                                  ptr_val, &index_val->llvm_value, 1, "ptr_elem");
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
// Bounds checking is deferred (matching the existing index path's TODO).
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
    ValueInfo* low_val = codegen_generate_expression(codegen, checker, sl->low);
    ValueInfo* high_val = codegen_generate_expression(codegen, checker, sl->high);
    if (!low_val || !high_val) {
        codegen_error(codegen, expr->pos, "Failed to generate slice bounds");
        value_info_free(base_val);
        if (low_val) value_info_free(low_val);
        if (high_val) value_info_free(high_val);
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

    // Widen both bounds to i64 (header fields are size_t). Bounds are
    // integer-typed (the checker enforces it), so width is well-defined.
    LLVMValueRef low64 = low_val->llvm_value;
    LLVMValueRef high64 = high_val->llvm_value;
    unsigned low_w = LLVMGetIntTypeWidth(LLVMTypeOf(low64));
    unsigned high_w = LLVMGetIntTypeWidth(LLVMTypeOf(high64));
    if (low_w < 64)  low64  = LLVMBuildSExt(codegen->builder, low64, i64, "low64");
    else if (low_w > 64)  low64  = LLVMBuildTrunc(codegen->builder, low64, i64, "low64");
    if (high_w < 64) high64 = LLVMBuildSExt(codegen->builder, high64, i64, "high64");
    else if (high_w > 64) high64 = LLVMBuildTrunc(codegen->builder, high64, i64, "high64");

    LLVMValueRef data_ptr = LLVMBuildExtractValue(codegen->builder, base_struct, 0, "base_data");
    LLVMValueRef new_len = LLVMBuildSub(codegen->builder, high64, low64, "new_len");
    LLVMValueRef result = LLVMGetUndef(struct_ty);

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
        LLVMValueRef old_cap = LLVMBuildExtractValue(codegen->builder, base_struct, 2, "old_cap");
        LLVMValueRef new_cap = LLVMBuildSub(codegen->builder, old_cap, low64, "new_cap");
        result = LLVMBuildInsertValue(codegen->builder, result, new_data, 0, "sl_data");
        result = LLVMBuildInsertValue(codegen->builder, result, new_len, 1, "sl_len");
        result = LLVMBuildInsertValue(codegen->builder, result, new_cap, 2, "sl_cap");
    }

    ValueInfo* out = value_info_new(NULL, result, base_type);
    out->is_lvalue = 0;  // a fresh header value (rvalue)
    value_info_free(base_val);
    value_info_free(low_val);
    value_info_free(high_val);
    return out;
#endif
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
    }

    // Generate code for the base expression
    ValueInfo* base_val = codegen_generate_expression(codegen, checker, selector->expr);
    if (!base_val) {
        codegen_error(codegen, expr->pos, "Failed to generate base expression for selector");
        return NULL;
    }
    
    // Get the type of the base expression
    Type* base_type = base_val->goo_type;
    
    // Handle pointer to struct
    if (base_type->kind == TYPE_POINTER && base_type->data.pointer.pointee_type->kind == TYPE_STRUCT) {
        base_type = base_type->data.pointer.pointee_type;
        // Load through the pointer if needed
        if (!base_val->is_lvalue) {
            // If it's not an lvalue, we need to load it first
            LLVMValueRef loaded = LLVMBuildLoad2(codegen->builder, 
                                                codegen_type_to_llvm(codegen, base_type),
                                                base_val->llvm_value, "ptr_load");
            base_val->llvm_value = loaded;
        }
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
//   - We also snapshot value_table_size before each arm and truncate after,
//     so a binding from arm N is not visible during arm N+1.
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
        size_t pre_arm_vt_size = codegen->value_table_size;
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
                    codegen_add_value(codegen, vi);

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
                LLVMBuildCondBr(codegen->builder, g->llvm_value,
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
        codegen->value_table_size = pre_arm_vt_size;
    }

    LLVMPositionBuilderAtEnd(codegen->builder, merge);
    return value_info_new(NULL, NULL, type_checker_get_builtin(checker, TYPE_VOID));
#endif
}

// Coerce a slice-literal element value to the declared element LLVM type
// before it is stored into the backing array. Mirrors the struct-literal field
// coercion: int->int widens with SExt (Goo ints are signed) / narrows with
// Trunc, int->float uses SIToFP. Matching widths and other kinds (bool, string)
// pass through unchanged. This is what lets the general []T{} case lower
// (int64/uint/float), not just the natural-width i32/string forms.
static LLVMValueRef slice_coerce_elem(CodeGenerator* codegen, LLVMValueRef v, LLVMTypeRef to) {
    LLVMTypeRef from = LLVMTypeOf(v);
    if (from == to) return v;
    LLVMTypeKind fk = LLVMGetTypeKind(from), tk = LLVMGetTypeKind(to);
    if (fk == LLVMIntegerTypeKind && tk == LLVMIntegerTypeKind) {
        unsigned fb = LLVMGetIntTypeWidth(from), tb = LLVMGetIntTypeWidth(to);
        if (fb < tb) return LLVMBuildSExt(codegen->builder, v, to, "elem_sext");
        if (fb > tb) return LLVMBuildTrunc(codegen->builder, v, to, "elem_trunc");
        return v;
    }
    if (fk == LLVMIntegerTypeKind && (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind))
        return LLVMBuildSIToFP(codegen->builder, v, to, "elem_sitofp");
    return v;
}

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
    Type* elem_type = slice_type->data.slice.element_type;
    LLVMTypeRef llvm_elem = codegen_type_to_llvm(codegen, elem_type);
    if (!llvm_elem) {
        codegen_error(codegen, expr->pos, "Cannot lower slice element type");
        return NULL;
    }

    // Count + evaluate elements.
    size_t count = 0;
    for (ASTNode* e = lit->elements; e; e = e->next) count++;

    LLVMValueRef* elem_vals = count ? calloc(count, sizeof(LLVMValueRef)) : NULL;
    size_t idx = 0;
    for (ASTNode* e = lit->elements; e; e = e->next, idx++) {
        ValueInfo* v = codegen_generate_expression(codegen, checker, e);
        if (!v) { free(elem_vals); return NULL; }
        elem_vals[idx] = v->llvm_value;
        value_info_free(v);
    }

    // Are all elements LLVM constants? Variable elements (e.g.
    // `[]int{a, b, 30}`, the common Go form) produce non-constant loads;
    // LLVMConstArray would embed those illegally and crash the verifier
    // ("Use of instruction is not an instruction!"). So the const path is
    // valid only when every element is constant.
    int all_const = 1;
    for (size_t i = 0; i < count; i++) {
        if (!LLVMIsConstant(elem_vals[i])) { all_const = 0; break; }
    }

    LLVMTypeRef arr_type = LLVMArrayType(llvm_elem, count);

    // Heap-allocate writable backing for the literal so the slice can be
    // mutated (e.g. `sl[i] = v`) and stays valid if it escapes the current
    // frame. A read-only global would segfault on store; a stack alloca would
    // dangle on escape. Each element is stored individually via GEP — this
    // serves BOTH constant and VARIABLE elements (a runtime load can't go in a
    // const array). The backing is not reclaimed — consistent with the
    // prototype's current allocate-and-leak memory model. Falls back to a
    // read-only global only when the runtime allocator is unavailable, which
    // requires all-constant elements.
    //
    // The EMPTY literal `[]T{}` (count == 0) must also take this runtime path,
    // not the const-global fallback: a zero-cap slice backed by a read-only
    // global aborts on the first `append`, because goo_slice_append grows
    // (len 0 >= cap 0) and reallocs a non-heap pointer. goo_alloc(0) returns
    // NULL, the store loop runs zero times, and the slice is {NULL, 0, 0} —
    // a heap-owned (nil-backed) empty slice that append can realloc(NULL→…)
    // safely. The common idiom `xs := []int{}; xs = append(xs, v)` then works.
    LLVMValueRef data_ptr;
    LLVMValueRef alloc_fn = LLVMGetNamedFunction(codegen->module, "goo_alloc");
    if (alloc_fn) {
        LLVMValueRef size = LLVMSizeOf(arr_type);
        data_ptr = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(alloc_fn),
                                  alloc_fn, &size, 1, "slice_backing");
        LLVMTypeRef i32ty = LLVMInt32TypeInContext(codegen->context);
        LLVMValueRef zero = LLVMConstInt(i32ty, 0, 0);
        for (size_t i = 0; i < count; i++) {
            LLVMValueRef indices[2] = { zero, LLVMConstInt(i32ty, i, 0) };
            LLVMValueRef ep = LLVMBuildGEP2(codegen->builder, arr_type, data_ptr,
                                            indices, 2, "slice_elem");
            // Coerce the element to the declared element LLVM type before the
            // store (mirrors the struct-literal field coercion). An i32 int
            // literal stored into a []int64 backing, or into a []float64, would
            // otherwise be a store-operand type mismatch / invalid IR. Lets the
            // general []T{} case (int64/uint/float/bool) lower, not just
            // i32/string. Goo ints are signed -> SExt to widen.
            elem_vals[i] = slice_coerce_elem(codegen, elem_vals[i], llvm_elem);
            LLVMBuildStore(codegen->builder, elem_vals[i], ep);
        }
    } else if (all_const) {
        LLVMValueRef arr_const = LLVMConstArray(llvm_elem, elem_vals, (unsigned)count);
        LLVMValueRef global = LLVMAddGlobal(codegen->module, arr_type, "slice_lit");
        LLVMSetInitializer(global, arr_const);
        LLVMSetLinkage(global, LLVMPrivateLinkage);
        LLVMSetGlobalConstant(global, 1);
        data_ptr = global;
    } else {
        free(elem_vals);
        codegen_error(codegen, expr->pos,
            "slice literal with non-constant elements requires the runtime "
            "allocator (goo_alloc), which is unavailable in this build");
        return NULL;
    }
    free(elem_vals);

    // Build the slice struct { ptr, i64 len, i64 cap }. A fresh literal's
    // capacity equals its length — the backing buffer is sized exactly to
    // `count`, so append() will grow on the first insert past the end.
    LLVMTypeRef slice_llvm = codegen_type_to_llvm(codegen, slice_type);
    LLVMValueRef slice_val = LLVMGetUndef(slice_llvm);
    slice_val = LLVMBuildInsertValue(codegen->builder, slice_val, data_ptr, 0, "slice_ptr");
    LLVMValueRef len_val = LLVMConstInt(LLVMInt64TypeInContext(codegen->context), count, 0);
    slice_val = LLVMBuildInsertValue(codegen->builder, slice_val, len_val, 1, "slice_len");
    slice_val = LLVMBuildInsertValue(codegen->builder, slice_val, len_val, 2, "slice_cap");

    return value_info_new(NULL, slice_val, slice_type);
#endif
}

