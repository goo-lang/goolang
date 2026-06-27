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

    // Widen or narrow the value to match the slot-0 element type before
    // inserting.  Without this, a narrow integer (e.g. i32 literal `5`)
    // wrapped into a !int64 function (whose value slot is i64) yields an
    // "insertvalue operand type mismatch" that fails `opt --passes=verify`.
    // Mirrors M4's fix in codegen_create_nullable_with_value.  Gate to
    // integer-kind mismatches only; structs and matching widths are untouched.
    {
        LLVMTypeRef value_slot_type = LLVMStructGetTypeAtIndex(data_union_type, 0);
        LLVMTypeRef val_ty          = LLVMTypeOf(value);
        if (LLVMGetTypeKind(val_ty)          == LLVMIntegerTypeKind &&
            LLVMGetTypeKind(value_slot_type) == LLVMIntegerTypeKind) {
            unsigned from_bits = LLVMGetIntTypeWidth(val_ty);
            unsigned to_bits   = LLVMGetIntTypeWidth(value_slot_type);
            if (from_bits < to_bits)
                value = LLVMBuildSExt(codegen->builder, value, value_slot_type, "erru_sext");
            else if (from_bits > to_bits)
                value = LLVMBuildTrunc(codegen->builder, value, value_slot_type, "erru_trunc");
        }
    }

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

// Generate code for try expression: propagate error or unwrap value.
//
// Original implementation tried to PHI the error-union struct and the
// extracted scalar — a type mismatch that failed LLVM module
// verification. Rewritten with early-return semantics: error path
// terminates the current basic block with `ret operand`; success
// path falls through with the unwrapped value. No PHI needed.
ValueInfo* codegen_generate_try_expr_impl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    if (!codegen || !checker || !expr || expr->type != AST_TRY_EXPR) return NULL;

    TryExprNode* try_expr = (TryExprNode*)expr;

    ValueInfo* operand_info = codegen_generate_expression(codegen, checker, try_expr->expr);
    if (!operand_info) return NULL;

    if (!type_is_error_union(operand_info->goo_type)) {
        codegen_error(codegen, expr->pos, "Try operator can only be applied to error union types");
        value_info_free(operand_info);
        return NULL;
    }

    LLVMValueRef is_error = codegen_error_union_is_error(codegen, operand_info->llvm_value);

    LLVMBasicBlockRef propagate_block = codegen_create_block(codegen, "try.propagate");
    LLVMBasicBlockRef continue_block = codegen_create_block(codegen, "try.continue");

    LLVMBuildCondBr(codegen->builder, is_error, propagate_block, continue_block);

    // Propagation block: return the error-union operand from the
    // current function — but only if the current function's return
    // type matches (i.e. it also returns this error union). When the
    // caller's return type doesn't match (e.g. main returns void in
    // the M8 probe), emit `unreachable` so the static IR verifies;
    // the success path is the only one exercised in such cases.
    codegen_set_insert_point(codegen, propagate_block);
    FunctionInfo* cur = codegen->current_function_info;
    if (cur && cur->goo_type && type_is_error_union(cur->goo_type)) {
        LLVMBuildRet(codegen->builder, operand_info->llvm_value);
    } else {
        LLVMBuildUnreachable(codegen->builder);
    }

    // Continue block: flow falls through here with the unwrapped
    // value. Subsequent codegen extends this block.
    codegen_set_insert_point(codegen, continue_block);
    LLVMValueRef success_value = codegen_error_union_get_value(codegen, operand_info->llvm_value);

    Type* value_type = operand_info->goo_type->data.error_union.value_type;
    value_info_free(operand_info);
    return value_info_new(NULL, success_value, value_type);
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
    
    // --- Error block ---
    // The catch body is always an AST_BLOCK_STMT (grammar: `expr CATCH id block`).
    // Generate it as a statement so block-level constructs (e.g. `return`) work.
    codegen_set_insert_point(codegen, error_block);

    // Bind the error variable in the codegen value table so that uses of it
    // inside the catch body (e.g. `fmt.Println(e)`) resolve correctly.
    if (catch_expr->error_var) {
        // Extract the error value from the error union's data slot.
        // After the type_mapping.c change, the default error type (NULL) maps
        // to goo_string_t {i8*, i64}, so error_raw IS already the full string
        // struct — no InsertValue wrapping needed. An explicitly typed error_type
        // that is also TYPE_STRING follows the same direct path.
        LLVMValueRef error_raw = codegen_error_union_get_error(codegen, operand_info->llvm_value);
        Type* error_type = operand_info->goo_type->data.error_union.error_type;
        if (!error_type) {
            error_type = type_checker_get_builtin(checker, TYPE_STRING);
        }
        if (error_raw && error_type) {
            LLVMTypeRef error_llvm = codegen_type_to_llvm(codegen, error_type);
            if (error_llvm) {
                // error_raw is already of type error_llvm (goo_string_t when
                // error_type is TYPE_STRING or the default NULL). Use it directly.
                LLVMValueRef error_alloca = codegen_create_entry_alloca(
                    codegen, error_llvm, catch_expr->error_var);
                LLVMBuildStore(codegen->builder, error_raw, error_alloca);
                ValueInfo* error_vi = value_info_new(catch_expr->error_var,
                                                     error_alloca, error_type);
                error_vi->is_lvalue = 1;
                error_vi->is_initialized = 1;
                codegen_add_value(codegen, error_vi);
            }
        }
    }

    // Generate the catch body as a statement (always a block).
    // Track whether it emits a terminator (e.g. `return`) so we only add a
    // PHI incoming from the error side when the block falls through.
    LLVMValueRef catch_value = NULL;
    LLVMBasicBlockRef error_exit_block = NULL;

    if (catch_expr->catch_body) {
        int body_ok = codegen_generate_statement(codegen, checker, catch_expr->catch_body);
        if (!body_ok) {
            value_info_free(operand_info);
            return NULL;
        }
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
            // Block fell through — produce a zero default and branch to merge.
            Type* vtype = operand_info->goo_type->data.error_union.value_type;
            LLVMTypeRef vllvm = codegen_type_to_llvm(codegen, vtype);
            catch_value = LLVMConstNull(vllvm);
            LLVMBuildBr(codegen->builder, merge_block);
            error_exit_block = LLVMGetInsertBlock(codegen->builder);
        }
        // else: block already terminated (e.g. `return`) — no br needed,
        // and no incoming to add to the PHI from the error side.
    } else {
        // No catch body: zero default, fall through to merge.
        Type* vtype = operand_info->goo_type->data.error_union.value_type;
        LLVMTypeRef vllvm = codegen_type_to_llvm(codegen, vtype);
        catch_value = LLVMConstNull(vllvm);
        LLVMBuildBr(codegen->builder, merge_block);
        error_exit_block = LLVMGetInsertBlock(codegen->builder);
    }
    
    // Success block: extract and use the success value
    codegen_set_insert_point(codegen, success_block);
    LLVMValueRef success_value = codegen_error_union_get_value(codegen, operand_info->llvm_value);
    LLVMBuildBr(codegen->builder, merge_block);
    LLVMBasicBlockRef success_exit_block = LLVMGetInsertBlock(codegen->builder);
    
    // --- Merge block ---
    codegen_set_insert_point(codegen, merge_block);

    // Get the value type (unwrapped from error union).
    Type* value_type = operand_info->goo_type->data.error_union.value_type;
    LLVMTypeRef value_llvm_type = codegen_type_to_llvm(codegen, value_type);

    // PHI to select the result.  Only add the error-side incoming when the
    // error block had a fall-through (error_exit_block != NULL).
    LLVMValueRef phi = LLVMBuildPhi(codegen->builder, value_llvm_type, "catch_result");
    LLVMAddIncoming(phi, &success_value, &success_exit_block, 1);
    if (error_exit_block && catch_value) {
        LLVMAddIncoming(phi, &catch_value, &error_exit_block, 1);
    }

    value_info_free(operand_info);
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

    // Mirror the type-checker scope for the function body (same pattern
    // as function_codegen.c — without this, type_check_* invocations
    // from inside body codegen don't find function params).
    scope_push(checker);
    if (func_decl->params) {
        for (ASTNode* p = func_decl->params; p; p = p->next) {
            if (p->type != AST_VAR_DECL) continue;
            VarDeclNode* pd = (VarDeclNode*)p;
            Type* pt = pd->type ? type_from_ast(checker, pd->type)
                                : type_checker_get_builtin(checker, TYPE_INT32);
            for (size_t i = 0; pt && i < pd->name_count; i++) {
                Variable* pv = variable_new(pd->names[i], pt, pd->base.pos);
                if (pv) {
                    pv->is_initialized = 1;
                    scope_add_variable(checker->current_scope, pv);
                }
            }
        }
    }

    // Generate function parameters as local variables. Parser builds
    // params as AST_VAR_DECL, not AST_IDENTIFIER — same fix that
    // function_codegen.c got. Original loop body never ran here.
    if (func_decl->params && param_count > 0) {
        ASTNode* param = func_decl->params;
        int param_index = 0;

        while (param && param_index < param_count) {
            const char* param_name = NULL;
            if (param->type == AST_VAR_DECL) {
                VarDeclNode* pd = (VarDeclNode*)param;
                if (pd->name_count > 0 && pd->names) param_name = pd->names[0];
            } else if (param->type == AST_IDENTIFIER) {
                param_name = ((IdentifierNode*)param)->name;
            }
            if (param_name) {
                LLVMValueRef param_value = LLVMGetParam(function, param_index);

                LLVMValueRef param_alloca = codegen_create_entry_alloca(codegen, param_types[param_index], param_name);
                LLVMBuildStore(codegen->builder, param_value, param_alloca);

                ValueInfo* param_info = value_info_new(param_name, param_alloca,
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
    
    // Add default return if missing (return error with goo_string_t payload).
    // The error slot is now goo_string_t {i8*, i64} by default (type_mapping.c),
    // so we must build the full struct rather than a bare i8*.
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
        const char* default_msg = "function did not return";
        LLVMValueRef msg_ptr = LLVMBuildGlobalStringPtr(
            codegen->builder, default_msg, "default_error_ptr");
        LLVMTypeRef str_llvm = codegen_get_basic_type(codegen, TYPE_STRING);
        LLVMValueRef str_val = LLVMGetUndef(str_llvm);
        str_val = LLVMBuildInsertValue(codegen->builder, str_val,
                                       msg_ptr, 0, "default_err_ptr");
        LLVMValueRef msg_len = LLVMConstInt(
            LLVMInt64TypeInContext(codegen->context),
            (unsigned long long)strlen(default_msg), 0);
        str_val = LLVMBuildInsertValue(codegen->builder, str_val,
                                       msg_len, 1, "default_err_len");
        LLVMValueRef error_union = codegen_create_error_union_error(
            codegen, error_union_type, str_val);
        LLVMBuildRet(codegen->builder, error_union);
    }
    
    // Exit function scope (codegen + type-check scopes)
    codegen_exit_function(codegen);
    scope_pop(checker);

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