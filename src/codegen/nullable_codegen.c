#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Nullable type code generation implementation

#if LLVM_AVAILABLE

// Generate code for creating a nullable value with a non-null value
LLVMValueRef codegen_create_nullable_with_value(CodeGenerator* codegen, LLVMTypeRef nullable_type, 
                                               LLVMValueRef value, Type* value_type) {
    if (!codegen || !nullable_type || !value) return NULL;
    
    // Nullable structure: { i1 is_null, value_type value }
    LLVMValueRef nullable_val = LLVMGetUndef(nullable_type);
    
    // Set is_null flag to false (0)
    LLVMValueRef is_null_false = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), 0, 0);
    nullable_val = LLVMBuildInsertValue(codegen->builder, nullable_val, is_null_false, 0, "nullable.is_null");
    
    // Coerce the value to match the expected element type of slot 1 before
    // inserting.  Without this, a value whose LLVM type differs from the
    // slot's yields either an "insertvalue operand type mismatch" (e.g. an
    // i32 literal wrapped into a ?int64 struct's i64 slot) or, for a
    // cross-kind mismatch (e.g. a typed int wrapped into a ?float64
    // struct's double slot), a hard LLVM verifier crash
    // ("insertvalue { i1, double } ..., i64"). Delegate to the shared
    // width/kind coercion helper (int<->int, int->float, float<->float)
    // instead of reimplementing the rule here — every wrap site that routes
    // through this function inherits the fix in one place. The helper
    // itself no-ops on kinds it doesn't handle (matching types, aggregates,
    // pointers), so it's safe to call unconditionally whenever the slot and
    // value types differ.  Signedness comes from the source Type when the
    // caller supplied one (typed values); default to signed when it
    // didn't (untyped literals), matching function_codegen.c's var-decl
    // convention.
    {
        LLVMTypeRef elems[2];
        LLVMGetStructElementTypes(nullable_type, elems);
        LLVMTypeRef slot_type = elems[1];
        if (LLVMTypeOf(value) != slot_type) {
            int src_signed = value_type ? type_is_signed(value_type) : 1;
            value = codegen_coerce_to_type(codegen, value, src_signed, slot_type);
        }
    }

    // Insert the value into slot 1
    nullable_val = LLVMBuildInsertValue(codegen->builder, nullable_val, value, 1, "nullable.value");
    
    return nullable_val;
}

// Generate code for creating a null nullable value
LLVMValueRef codegen_create_nullable_null(CodeGenerator* codegen, LLVMTypeRef nullable_type, Type* base_type) {
    if (!codegen || !nullable_type) return NULL;
    
    // Nullable structure: { i1 is_null, value_type value }
    LLVMValueRef nullable_val = LLVMGetUndef(nullable_type);
    
    // Set is_null flag to true (1)
    LLVMValueRef is_null_true = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), 1, 0);
    nullable_val = LLVMBuildInsertValue(codegen->builder, nullable_val, is_null_true, 0, "nullable.is_null");
    
    // Set a default value for the value field (even though it's null for memory layout consistency)
    LLVMTypeRef value_type = codegen_type_to_llvm(codegen, base_type);
    if (value_type) {
        LLVMValueRef default_value = LLVMConstNull(value_type);
        nullable_val = LLVMBuildInsertValue(codegen->builder, nullable_val, default_value, 1, "nullable.value");
    }
    
    return nullable_val;
}

// Generate code to check if a nullable value is null
LLVMValueRef codegen_nullable_is_null(CodeGenerator* codegen, LLVMValueRef nullable_value) {
    if (!codegen || !nullable_value) return NULL;
    
    // Extract the is_null flag (index 0)
    return LLVMBuildExtractValue(codegen->builder, nullable_value, 0, "is_null_check");
}

// Generate code to extract the value from a nullable (assumes not null)
LLVMValueRef codegen_nullable_get_value(CodeGenerator* codegen, LLVMValueRef nullable_value) {
    if (!codegen || !nullable_value) return NULL;
    
    // Extract the value (index 1)
    return LLVMBuildExtractValue(codegen->builder, nullable_value, 1, "unwrapped_value");
}

// Generate code for safe nullable access with runtime null check
ValueInfo* codegen_generate_nullable_access(CodeGenerator* codegen, TypeChecker* checker, 
                                           LLVMValueRef nullable_value, Type* nullable_type, 
                                           Position pos, const char* access_description) {
    if (!codegen || !nullable_value || !nullable_type || !type_is_nullable(nullable_type)) return NULL;
    
    // Check if the nullable value is null
    LLVMValueRef is_null = codegen_nullable_is_null(codegen, nullable_value);
    
    // Create basic blocks for null and non-null cases
    LLVMBasicBlockRef null_block = codegen_create_block(codegen, "nullable.null");
    LLVMBasicBlockRef non_null_block = codegen_create_block(codegen, "nullable.non_null");
    LLVMBasicBlockRef merge_block = codegen_create_block(codegen, "nullable.merge");
    
    // Branch based on null status
    LLVMBuildCondBr(codegen->builder, is_null, null_block, non_null_block);
    
    // Null block: generate panic/error
    codegen_set_insert_point(codegen, null_block);
    
    // Generate a null pointer access error
    LLVMValueRef error_message = LLVMBuildGlobalStringPtr(codegen->builder, 
        access_description ? access_description : "null pointer access", "null_error");
    
    // Call panic function (for now, just unreachable)
    // TODO: Implement proper panic/abort mechanism
    LLVMBuildUnreachable(codegen->builder);
    
    // Non-null block: extract and return the value
    codegen_set_insert_point(codegen, non_null_block);
    LLVMValueRef unwrapped_value = codegen_nullable_get_value(codegen, nullable_value);
    LLVMBuildBr(codegen->builder, merge_block);
    LLVMBasicBlockRef non_null_exit_block = LLVMGetInsertBlock(codegen->builder);
    
    // Merge block: continue execution
    codegen_set_insert_point(codegen, merge_block);
    
    // Get the base type (unwrapped from nullable)
    Type* base_type = nullable_type->data.nullable.base_type;
    LLVMTypeRef base_llvm_type = codegen_type_to_llvm(codegen, base_type);
    
    // Create PHI node for the unwrapped value
    LLVMValueRef phi = LLVMBuildPhi(codegen->builder, base_llvm_type, "nullable_result");
    
    // Add incoming value (only from non-null block since null block is unreachable)
    LLVMValueRef incoming_values[] = { unwrapped_value };
    LLVMBasicBlockRef incoming_blocks[] = { non_null_exit_block };
    LLVMAddIncoming(phi, incoming_values, incoming_blocks, 1);
    
    return value_info_new(NULL, phi, base_type);
}

// Generate code for if-let nullable unwrapping
ValueInfo* codegen_generate_if_let_nullable(CodeGenerator* codegen, TypeChecker* checker, 
                                           LLVMValueRef nullable_value, Type* nullable_type,
                                           ASTNode* then_stmt, ASTNode* else_stmt,
                                           const char* var_name, Position pos) {
    if (!codegen || !nullable_value || !nullable_type || !type_is_nullable(nullable_type)) return NULL;
    
    // Check if the nullable value is null
    LLVMValueRef is_null = codegen_nullable_is_null(codegen, nullable_value);
    
    // Create basic blocks
    LLVMBasicBlockRef non_null_block = codegen_create_block(codegen, "if_let.non_null");
    LLVMBasicBlockRef else_block = else_stmt ? codegen_create_block(codegen, "if_let.else") : NULL;
    LLVMBasicBlockRef merge_block = codegen_create_block(codegen, "if_let.merge");
    
    // Branch based on null status (note: branch to non_null when is_null is false)
    LLVMValueRef is_not_null = LLVMBuildNot(codegen->builder, is_null, "is_not_null");
    LLVMBuildCondBr(codegen->builder, is_not_null, non_null_block, else_block ? else_block : merge_block);
    
    // Non-null block: extract value and execute then statement
    codegen_set_insert_point(codegen, non_null_block);
    LLVMValueRef unwrapped_value = codegen_nullable_get_value(codegen, nullable_value);
    
    // TODO: Add unwrapped value to scope with var_name if provided
    // For now, just execute the then statement
    ValueInfo* then_result = NULL;
    if (then_stmt) {
        if (then_stmt->type == AST_EXPR_STMT || then_stmt->type == AST_LITERAL || 
            then_stmt->type == AST_IDENTIFIER || then_stmt->type == AST_BINARY_EXPR) {
            then_result = codegen_generate_expression(codegen, checker, then_stmt);
        } else {
            codegen_generate_statement(codegen, checker, then_stmt);
            // Create a dummy result for consistency
            Type* void_type = type_checker_get_builtin(checker, TYPE_VOID);
            LLVMValueRef void_val = LLVMGetUndef(LLVMVoidTypeInContext(codegen->context));
            then_result = value_info_new(NULL, void_val, void_type);
        }
    }
    
    LLVMBuildBr(codegen->builder, merge_block);
    LLVMBasicBlockRef then_exit_block = LLVMGetInsertBlock(codegen->builder);
    
    // Else block: execute else statement if present
    ValueInfo* else_result = NULL;
    LLVMBasicBlockRef else_exit_block = NULL;
    
    if (else_block) {
        codegen_set_insert_point(codegen, else_block);
        
        if (else_stmt) {
            if (else_stmt->type == AST_EXPR_STMT || else_stmt->type == AST_LITERAL || 
                else_stmt->type == AST_IDENTIFIER || else_stmt->type == AST_BINARY_EXPR) {
                else_result = codegen_generate_expression(codegen, checker, else_stmt);
            } else {
                codegen_generate_statement(codegen, checker, else_stmt);
                // Create a dummy result for consistency
                Type* void_type = type_checker_get_builtin(checker, TYPE_VOID);
                LLVMValueRef void_val = LLVMGetUndef(LLVMVoidTypeInContext(codegen->context));
                else_result = value_info_new(NULL, void_val, void_type);
            }
        }
        
        LLVMBuildBr(codegen->builder, merge_block);
        else_exit_block = LLVMGetInsertBlock(codegen->builder);
    }
    
    // Merge block
    codegen_set_insert_point(codegen, merge_block);
    
    // If both branches have results, create a PHI node
    if (then_result && else_result) {
        // Check if types are compatible
        if (then_result->goo_type == else_result->goo_type || 
            type_compatible(then_result->goo_type, else_result->goo_type)) {
            
            LLVMTypeRef result_type = codegen_type_to_llvm(codegen, then_result->goo_type);
            LLVMValueRef phi = LLVMBuildPhi(codegen->builder, result_type, "if_let_result");
            
            if (else_exit_block) {
                LLVMValueRef incoming_values[] = { then_result->llvm_value, else_result->llvm_value };
                LLVMBasicBlockRef incoming_blocks[] = { then_exit_block, else_exit_block };
                LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);
            } else {
                LLVMValueRef incoming_values[] = { then_result->llvm_value };
                LLVMBasicBlockRef incoming_blocks[] = { then_exit_block };
                LLVMAddIncoming(phi, incoming_values, incoming_blocks, 1);
            }
            
            Type* result_goo_type = then_result->goo_type;
            value_info_free(then_result);
            if (else_result) value_info_free(else_result);
            
            return value_info_new(NULL, phi, result_goo_type);
        }
    }
    
    // Clean up and return void result
    if (then_result) value_info_free(then_result);
    if (else_result) value_info_free(else_result);
    
    Type* void_type = type_checker_get_builtin(checker, TYPE_VOID);
    LLVMValueRef void_val = LLVMGetUndef(LLVMVoidTypeInContext(codegen->context));
    return value_info_new(NULL, void_val, void_type);
}

// Generate code for forced nullable unwrapping (panic if null)
ValueInfo* codegen_generate_nullable_force_unwrap(CodeGenerator* codegen, TypeChecker* checker, 
                                                 LLVMValueRef nullable_value, Type* nullable_type, 
                                                 Position pos) {
    if (!codegen || !nullable_value || !nullable_type || !type_is_nullable(nullable_type)) return NULL;
    
    return codegen_generate_nullable_access(codegen, checker, nullable_value, nullable_type, 
                                          pos, "forced unwrap of null value");
}

// Generate code for nullable assignment
int codegen_generate_nullable_assignment(CodeGenerator* codegen, TypeChecker* checker,
                                        LLVMValueRef nullable_target, LLVMValueRef source_value,
                                        Type* target_type, Type* source_type, Position pos) {
    if (!codegen || !nullable_target || !source_value) return 0;
    
    if (!type_is_nullable(target_type)) {
        codegen_error(codegen, pos, "Cannot assign to non-nullable type");
        return 0;
    }
    
    LLVMTypeRef nullable_llvm_type = codegen_type_to_llvm(codegen, target_type);
    if (!nullable_llvm_type) return 0;
    
    LLVMValueRef nullable_value;
    
    if (type_is_nullable(source_type)) {
        // Direct assignment of nullable to nullable
        nullable_value = source_value;
    } else {
        // Wrap non-nullable value in nullable
        nullable_value = codegen_create_nullable_with_value(codegen, nullable_llvm_type, 
                                                          source_value, source_type);
    }
    
    // Store the nullable value
    LLVMBuildStore(codegen->builder, nullable_value, nullable_target);
    return 1;
}

// Generate code for null literal
ValueInfo* codegen_generate_null_literal(CodeGenerator* codegen, TypeChecker* checker, Type* expected_type) {
    if (!codegen || !expected_type || !type_is_nullable(expected_type)) {
        // If no expected type or not nullable, create a generic null pointer
        LLVMValueRef null_ptr = LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0));
        Type* void_ptr_type = type_pointer(type_checker_get_builtin(checker, TYPE_VOID));
        return value_info_new(NULL, null_ptr, void_ptr_type);
    }
    
    LLVMTypeRef nullable_type = codegen_type_to_llvm(codegen, expected_type);
    if (!nullable_type) return NULL;
    
    LLVMValueRef null_value = codegen_create_nullable_null(codegen, nullable_type, 
                                                         expected_type->data.nullable.base_type);
    
    return value_info_new(NULL, null_value, expected_type);
}

#endif

// Stub implementations for when LLVM is not available
#if !LLVM_AVAILABLE

ValueInfo* codegen_generate_nullable_access(CodeGenerator* codegen, TypeChecker* checker, 
                                           void* nullable_value, Type* nullable_type, 
                                           Position pos, const char* access_description) {
    (void)checker; (void)nullable_value; (void)nullable_type; (void)access_description;
    codegen_error(codegen, pos, "LLVM support not available for nullable access");
    return NULL;
}

ValueInfo* codegen_generate_if_let_nullable(CodeGenerator* codegen, TypeChecker* checker, 
                                           void* nullable_value, Type* nullable_type,
                                           ASTNode* then_stmt, ASTNode* else_stmt,
                                           const char* var_name, Position pos) {
    (void)checker; (void)nullable_value; (void)nullable_type; (void)then_stmt; 
    (void)else_stmt; (void)var_name;
    codegen_error(codegen, pos, "LLVM support not available for if-let nullable");
    return NULL;
}

ValueInfo* codegen_generate_nullable_force_unwrap(CodeGenerator* codegen, TypeChecker* checker, 
                                                 void* nullable_value, Type* nullable_type, 
                                                 Position pos) {
    (void)checker; (void)nullable_value; (void)nullable_type;
    codegen_error(codegen, pos, "LLVM support not available for nullable force unwrap");
    return NULL;
}

int codegen_generate_nullable_assignment(CodeGenerator* codegen, TypeChecker* checker,
                                        void* nullable_target, void* source_value,
                                        Type* target_type, Type* source_type, Position pos) {
    (void)checker; (void)nullable_target; (void)source_value; (void)target_type; (void)source_type;
    codegen_error(codegen, pos, "LLVM support not available for nullable assignment");
    return 0;
}

ValueInfo* codegen_generate_null_literal(CodeGenerator* codegen, TypeChecker* checker, Type* expected_type) {
    (void)checker; (void)expected_type;
    Position pos = {0, 0, 0, "unknown"};
    codegen_error(codegen, pos, "LLVM support not available for null literal");
    return NULL;
}

#endif