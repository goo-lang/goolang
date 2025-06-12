#include "codegen.h"
#include <stdlib.h>
#include <string.h>

#if LLVM_AVAILABLE

// Type mapping from Goo types to LLVM types

LLVMTypeRef codegen_type_to_llvm(CodeGenerator* codegen, const Type* type) {
    if (!codegen || !type) return NULL;
    
    switch (type->kind) {
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
            // Slice is represented as { T*, i64 } (pointer + length)
            {
                LLVMTypeRef element_type = codegen_type_to_llvm(codegen, type->data.slice.element_type);
                if (!element_type) return NULL;
                
                return LLVMStructTypeInContext(codegen->context,
                    (LLVMTypeRef[]){
                        LLVMPointerType(element_type, 0),
                        LLVMInt64TypeInContext(codegen->context)
                    }, 2, 0);
            }
            
        case TYPE_MAP:
            // Map is represented as an opaque pointer for now
            return LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
            
        case TYPE_CHANNEL:
            return codegen_get_channel_type(codegen, type);
            
        case TYPE_FUNCTION:
            return codegen_get_function_type(codegen, type);
            
        case TYPE_POINTER:
            return codegen_get_pointer_type(codegen, type);
            
        case TYPE_STRUCT:
            return codegen_get_struct_type(codegen, type);
            
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
    
    // Check if we already have this struct type cached
    // For now, we'll generate it each time
    
    if (type->data.struct_type.field_count == 0) {
        // Empty struct
        return LLVMStructTypeInContext(codegen->context, NULL, 0, 0);
    }
    
    // Create array of field types
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
    
    // Create the struct type
    LLVMTypeRef struct_type = LLVMStructTypeInContext(codegen->context, field_types, 
                                                      (unsigned)type->data.struct_type.field_count, 0);
    
    free(field_types);
    return struct_type;
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
    
    LLVMTypeRef error_type = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0); // Generic error pointer
    if (type->data.error_union.error_type) {
        error_type = codegen_type_to_llvm(codegen, type->data.error_union.error_type);
        if (!error_type) {
            error_type = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
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
            // Sign extend or zero extend (assume zero extend for now)
            return LLVMBuildZExt(codegen->builder, value, to_type, "zext");
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