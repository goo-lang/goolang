#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Error union code generation implementation

#if LLVM_AVAILABLE

// Generate code for creating an error union value from a success value
LLVMValueRef codegen_create_error_union_success(CodeGenerator* codegen, LLVMTypeRef union_type, 
                                               LLVMValueRef value, Type* value_type __attribute__((unused))) {
    if (!codegen || !union_type || !value) return NULL;
    
    // Error union structure: { i1 is_error, union { value, error } }
    LLVMValueRef error_union = LLVMGetUndef(union_type);
    
    // Set is_error flag to false (0)
    LLVMValueRef is_error_false = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), 0, 0);
    error_union = LLVMBuildInsertValue(codegen->builder, error_union, is_error_false, 0, "error_union.is_error");
    
    // Get the data union type (index 1 in the error union struct)
    LLVMTypeRef data_union_type = LLVMStructGetTypeAtIndex(union_type, 1);
    LLVMValueRef data_union = LLVMGetUndef(data_union_type);
    
    // Insert the value into slot 0 of the data union (success value)
    data_union = LLVMBuildInsertValue(codegen->builder, data_union, value, 0, "data_union.value");
    
    // Insert the data union into the error union
    error_union = LLVMBuildInsertValue(codegen->builder, error_union, data_union, 1, "error_union.data");
    
    return error_union;
}

// Generate code for creating an error union value from an error
LLVMValueRef codegen_create_error_union_error(CodeGenerator* codegen, LLVMTypeRef union_type, 
                                             LLVMValueRef error_value) {
    if (!codegen || !union_type || !error_value) return NULL;
    
    // Error union structure: { i1 is_error, union { value, error } }
    LLVMValueRef error_union = LLVMGetUndef(union_type);
    
    // Set is_error flag to true (1)
    LLVMValueRef is_error_true = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), 1, 0);
    error_union = LLVMBuildInsertValue(codegen->builder, error_union, is_error_true, 0, "error_union.is_error");
    
    // Get the data union type (index 1 in the error union struct)
    LLVMTypeRef data_union_type = LLVMStructGetTypeAtIndex(union_type, 1);
    LLVMValueRef data_union = LLVMGetUndef(data_union_type);
    
    // Insert the error into slot 1 of the data union (error value)
    data_union = LLVMBuildInsertValue(codegen->builder, data_union, error_value, 1, "data_union.error");
    
    // Insert the data union into the error union
    error_union = LLVMBuildInsertValue(codegen->builder, error_union, data_union, 1, "error_union.data");
    
    return error_union;
}

// Generate code to check if an error union contains an error
LLVMValueRef codegen_error_union_is_error(CodeGenerator* codegen, LLVMValueRef error_union) {
    if (!codegen || !error_union) return NULL;
    
    // Extract the is_error flag (index 0)
    return LLVMBuildExtractValue(codegen->builder, error_union, 0, "is_error_check");
}

// Generate code to extract the success value from an error union (assumes no error)
LLVMValueRef codegen_error_union_get_value(CodeGenerator* codegen, LLVMValueRef error_union) {
    if (!codegen || !error_union) return NULL;
    
    // Extract the data union (index 1)
    LLVMValueRef data_union = LLVMBuildExtractValue(codegen->builder, error_union, 1, "data_union");
    
    // Extract the value (index 0 in data union)
    return LLVMBuildExtractValue(codegen->builder, data_union, 0, "success_value");
}

// Generate code to extract the error value from an error union (assumes error)
LLVMValueRef codegen_error_union_get_error(CodeGenerator* codegen, LLVMValueRef error_union) {
    if (!codegen || !error_union) return NULL;
    
    // Extract the data union (index 1)
    LLVMValueRef data_union = LLVMBuildExtractValue(codegen->builder, error_union, 1, "data_union");
    
    // Extract the error (index 1 in data union)
    return LLVMBuildExtractValue(codegen->builder, data_union, 1, "error_value");
}

// Generate code for try expression: propagate error or unwrap value
ValueInfo* codegen_generate_try_expr_impl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    if (!codegen || !checker || !expr || expr->type != AST_TRY_EXPR) return NULL;
    
    TryExprNode* try_expr = (TryExprNode*)expr;
    
    // Generate the expression that should be an error union
    ValueInfo* operand_info = codegen_generate_expression(codegen, checker, try_expr->expr);
    if (!operand_info) return NULL;
    
    // Check that the operand is actually an error union type
    if (!type_is_error_union(operand_info->goo_type)) {
        codegen_error(codegen, expr->pos, "Try operator can only be applied to error union types");
        value_info_free(operand_info);
        return NULL;
    }
    
    // Check if the operand contains an error
    LLVMValueRef is_error = codegen_error_union_is_error(codegen, operand_info->llvm_value);
    
    // Create basic blocks for error and success cases
    LLVMBasicBlockRef error_block = codegen_create_block(codegen, "try.error");
    LLVMBasicBlockRef success_block = codegen_create_block(codegen, "try.success");
    LLVMBasicBlockRef merge_block = codegen_create_block(codegen, "try.merge");
    
    // Branch based on error status
    LLVMBuildCondBr(codegen->builder, is_error, error_block, success_block);
    
    // Error block: propagate the error by returning it
    codegen_set_insert_point(codegen, error_block);
    
    // For now, we'll just return the error union as-is (error propagation)
    // In a full implementation, this would return from the current function
    LLVMValueRef error_result = operand_info->llvm_value;
    LLVMBuildBr(codegen->builder, merge_block);
    LLVMBasicBlockRef error_exit_block = LLVMGetInsertBlock(codegen->builder);
    
    // Success block: extract and return the value
    codegen_set_insert_point(codegen, success_block);
    LLVMValueRef success_value = codegen_error_union_get_value(codegen, operand_info->llvm_value);
    LLVMBuildBr(codegen->builder, merge_block);
    LLVMBasicBlockRef success_exit_block = LLVMGetInsertBlock(codegen->builder);
    
    // Merge block: use PHI to select the result
    codegen_set_insert_point(codegen, merge_block);
    
    // Get the value type (unwrapped from error union)
    Type* value_type = operand_info->goo_type->data.error_union.value_type;
    LLVMTypeRef value_llvm_type = codegen_type_to_llvm(codegen, value_type);
    
    // Create PHI node to merge the two possible values
    LLVMValueRef phi = LLVMBuildPhi(codegen->builder, value_llvm_type, "try_result");
    
    // Add incoming values to PHI
    LLVMValueRef incoming_values[] = { error_result, success_value };
    LLVMBasicBlockRef incoming_blocks[] = { error_exit_block, success_exit_block };
    LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);
    
    value_info_free(operand_info);
    return value_info_new(NULL, phi, value_type);
}

// Generate code for catch expression: handle error or use value
ValueInfo* codegen_generate_catch_expr_impl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    if (!codegen || !checker || !expr || expr->type != AST_CATCH_EXPR) return NULL;
    
    CatchExprNode* catch_expr = (CatchExprNode*)expr;
    
    // Generate the expression that should be an error union
    ValueInfo* operand_info = codegen_generate_expression(codegen, checker, catch_expr->expr);
    if (!operand_info) return NULL;
    
    // Check that the operand is actually an error union type
    if (!type_is_error_union(operand_info->goo_type)) {
        codegen_error(codegen, expr->pos, "Catch operator can only be applied to error union types");
        value_info_free(operand_info);
        return NULL;
    }
    
    // Check if the operand contains an error
    LLVMValueRef is_error = codegen_error_union_is_error(codegen, operand_info->llvm_value);
    
    // Create basic blocks for error and success cases
    LLVMBasicBlockRef error_block = codegen_create_block(codegen, "catch.error");
    LLVMBasicBlockRef success_block = codegen_create_block(codegen, "catch.success");
    LLVMBasicBlockRef merge_block = codegen_create_block(codegen, "catch.merge");
    
    // Branch based on error status
    LLVMBuildCondBr(codegen->builder, is_error, error_block, success_block);
    
    // Error block: execute catch body
    codegen_set_insert_point(codegen, error_block);
    
    // If there's an error variable name, create a binding for it
    if (catch_expr->error_var) {
        LLVMValueRef error_value __attribute__((unused)) = codegen_error_union_get_error(codegen, operand_info->llvm_value);
        
        // TODO: Add error variable to scope
        // For now, we'll just generate the catch body without the error binding
    }
    
    // Generate the catch body
    ValueInfo* catch_result = NULL;
    if (catch_expr->catch_body) {
        catch_result = codegen_generate_expression(codegen, checker, catch_expr->catch_body);
    } else {
        // Default catch behavior: return a default value
        Type* value_type = operand_info->goo_type->data.error_union.value_type;
        LLVMTypeRef value_llvm_type = codegen_type_to_llvm(codegen, value_type);
        LLVMValueRef default_value = LLVMConstNull(value_llvm_type);
        catch_result = value_info_new(NULL, default_value, value_type);
    }
    
    if (!catch_result) {
        value_info_free(operand_info);
        return NULL;
    }
    
    LLVMValueRef catch_value = catch_result->llvm_value;
    LLVMBuildBr(codegen->builder, merge_block);
    LLVMBasicBlockRef error_exit_block = LLVMGetInsertBlock(codegen->builder);
    
    // Success block: extract and use the success value
    codegen_set_insert_point(codegen, success_block);
    LLVMValueRef success_value = codegen_error_union_get_value(codegen, operand_info->llvm_value);
    LLVMBuildBr(codegen->builder, merge_block);
    LLVMBasicBlockRef success_exit_block = LLVMGetInsertBlock(codegen->builder);
    
    // Merge block: use PHI to select the result
    codegen_set_insert_point(codegen, merge_block);
    
    // Get the value type (unwrapped from error union)
    Type* value_type = operand_info->goo_type->data.error_union.value_type;
    LLVMTypeRef value_llvm_type = codegen_type_to_llvm(codegen, value_type);
    
    // Create PHI node to merge the two possible values
    LLVMValueRef phi = LLVMBuildPhi(codegen->builder, value_llvm_type, "catch_result");
    
    // Add incoming values to PHI
    LLVMValueRef incoming_values[] = { catch_value, success_value };
    LLVMBasicBlockRef incoming_blocks[] = { error_exit_block, success_exit_block };
    LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);
    
    value_info_free(operand_info);
    value_info_free(catch_result);
    return value_info_new(NULL, phi, value_type);
}

// Generate code for function that returns an error union
int codegen_generate_error_union_function(CodeGenerator* codegen, TypeChecker* checker, 
                                         FuncDeclNode* func_decl, Type* return_type) {
    if (!codegen || !checker || !func_decl || !return_type) return 0;
    
    if (!type_is_error_union(return_type)) {
        codegen_error(codegen, func_decl->base.pos, "Function does not return error union type");
        return 0;
    }
    
    // Generate LLVM function type with error union return
    LLVMTypeRef error_union_type = codegen_type_to_llvm(codegen, return_type);
    if (!error_union_type) {
        codegen_error(codegen, func_decl->base.pos, "Failed to generate LLVM type for error union");
        return 0;
    }
    
    // Get function type info from type checker
    Variable* func_var = type_checker_lookup_variable(checker, func_decl->name);
    Type* func_type_info = NULL;
    if (func_var && func_var->type->kind == TYPE_FUNCTION) {
        func_type_info = func_var->type;
    }
    
    // Handle function parameters
    LLVMTypeRef* param_types = NULL;
    int param_count = 0;
    
    if (func_type_info && func_type_info->data.function.param_count > 0) {
        param_count = func_type_info->data.function.param_count;
        param_types = malloc(sizeof(LLVMTypeRef) * param_count);
        
        for (int i = 0; i < param_count; i++) {
            param_types[i] = codegen_type_to_llvm(codegen, func_type_info->data.function.param_types[i]);
            if (!param_types[i]) {
                codegen_error(codegen, func_decl->base.pos, "Failed to generate LLVM type for parameter %d", i);
                free(param_types);
                return 0;
            }
        }
    }
    
    LLVMTypeRef function_type = LLVMFunctionType(error_union_type, param_types, param_count, 0);
    
    // Create the function
    LLVMValueRef function = LLVMAddFunction(codegen->module, func_decl->name, function_type);
    
    // Create function info
    FunctionInfo* func_info = function_info_new(func_decl->name, function, return_type);
    if (!func_info) {
        codegen_error(codegen, func_decl->base.pos, "Failed to create function info");
        if (param_types) free(param_types);
        return 0;
    }
    
    // Create entry basic block
    func_info->entry_block = LLVMAppendBasicBlockInContext(codegen->context, function, "entry");
    
    // Enter function scope
    codegen_enter_function(codegen, func_info);
    codegen_set_insert_point(codegen, func_info->entry_block);
    
    // Generate function parameters as local variables
    if (func_decl->params && param_count > 0) {
        ASTNode* param = func_decl->params;
        int param_index = 0;

        while (param && param_index < param_count) {
            // Parameters are AST_VAR_DECL nodes created by the parser
            if (param->type == AST_VAR_DECL) {
                VarDeclNode* param_decl = (VarDeclNode*)param;

                // Handle each name in the parameter declaration
                for (size_t i = 0; i < param_decl->name_count && param_index < param_count; i++) {
                    const char* param_name = param_decl->names[i];
                    LLVMValueRef param_value = LLVMGetParam(function, param_index);

                    // Create alloca for parameter
                    LLVMValueRef param_alloca = codegen_create_entry_alloca(codegen, param_types[param_index], param_name);
                    LLVMBuildStore(codegen->builder, param_value, param_alloca);

                    // Add to value table
                    ValueInfo* param_info = value_info_new(param_name, param_alloca,
                                                          func_type_info->data.function.param_types[param_index]);
                    param_info->is_lvalue = 1;
                    param_info->is_initialized = 1;
                    codegen_add_value(codegen, param_info);

                    param_index++;
                }
            } else if (param->type == AST_IDENTIFIER) {
                // Fallback for old-style identifier parameters (if any)
                IdentifierNode* param_ident = (IdentifierNode*)param;
                LLVMValueRef param_value = LLVMGetParam(function, param_index);

                // Create alloca for parameter
                LLVMValueRef param_alloca = codegen_create_entry_alloca(codegen, param_types[param_index], param_ident->name);
                LLVMBuildStore(codegen->builder, param_value, param_alloca);

                // Add to value table
                ValueInfo* param_info = value_info_new(param_ident->name, param_alloca,
                                                      func_type_info->data.function.param_types[param_index]);
                param_info->is_lvalue = 1;
                param_info->is_initialized = 1;
                codegen_add_value(codegen, param_info);

                param_index++;
            }
            param = param->next;
        }
    }
    
    if (param_types) free(param_types);
    
    // Generate function body
    int result = 1;
    if (func_decl->body) {
        result = codegen_generate_statement(codegen, checker, func_decl->body);
    }
    
    // Add default return if missing (return error)
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
        // Create a default error value
        LLVMValueRef error_str = LLVMBuildGlobalStringPtr(codegen->builder, "function did not return", "default_error");
        LLVMValueRef error_union = codegen_create_error_union_error(codegen, error_union_type, error_str);
        LLVMBuildRet(codegen->builder, error_union);
    }
    
    // Exit function scope
    codegen_exit_function(codegen);
    
    // Clean up function info
    function_info_free(func_info);
    
    return result;
}

// Generate code for error propagation in return statements
LLVMValueRef codegen_generate_error_return(CodeGenerator* codegen, LLVMValueRef return_value, 
                                         Type* return_type, Type* function_return_type) {
    if (!codegen || !return_value || !return_type || !function_return_type) return NULL;
    
    // If function returns error union but value is not error union, wrap it
    if (type_is_error_union(function_return_type) && !type_is_error_union(return_type)) {
        LLVMTypeRef error_union_type = codegen_type_to_llvm(codegen, function_return_type);
        return codegen_create_error_union_success(codegen, error_union_type, return_value, return_type);
    }
    
    // If both are error unions, just return as-is
    return return_value;
}

#endif

// Stub implementations for when LLVM is not available
#if !LLVM_AVAILABLE

ValueInfo* codegen_generate_try_expr_impl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    (void)checker; // Suppress unused parameter warning
    codegen_error(codegen, expr->pos, "LLVM support not available for try expressions");
    return NULL;
}

ValueInfo* codegen_generate_catch_expr_impl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    (void)checker; // Suppress unused parameter warning
    codegen_error(codegen, expr->pos, "LLVM support not available for catch expressions");
    return NULL;
}

int codegen_generate_error_union_function(CodeGenerator* codegen, TypeChecker* checker, 
                                         FuncDeclNode* func_decl, Type* return_type) {
    (void)checker;     // Suppress unused parameter warning
    (void)return_type; // Suppress unused parameter warning
    codegen_error(codegen, func_decl->base.pos, "LLVM support not available for error union functions");
    return 0;
}

// Stub function when LLVM is not available - returns void*
void* codegen_generate_error_return_stub(CodeGenerator* codegen, void* return_value, 
                                        Type* return_type, Type* function_return_type) {
    (void)codegen;               // Suppress unused parameter warning
    (void)return_value;          // Suppress unused parameter warning
    (void)return_type;           // Suppress unused parameter warning
    (void)function_return_type;  // Suppress unused parameter warning
    return NULL;
}

#endif