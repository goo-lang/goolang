#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Function and declaration code generation

// Forward declaration for error union function generation
int codegen_generate_error_union_function(CodeGenerator* codegen, TypeChecker* checker, 
                                         FuncDeclNode* func_decl, Type* return_type);

int codegen_generate_function_decl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, decl->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !decl || decl->type != AST_FUNC_DECL) return 0;
    
    FuncDeclNode* func_decl = (FuncDeclNode*)decl;
    
    // Get function type from AST
    Type* return_type = NULL;
    if (func_decl->return_type) {
        return_type = type_from_ast(checker, func_decl->return_type);
    } else {
        return_type = type_checker_get_builtin(checker, TYPE_VOID);
    }
    
    if (!return_type) {
        codegen_error(codegen, decl->pos, "Failed to determine function return type");
        return 0;
    }
    
    // Check if this is an error union function
    if (type_is_error_union(return_type)) {
        return codegen_generate_error_union_function(codegen, checker, func_decl, return_type);
    }
    
    // Generate LLVM return type
    LLVMTypeRef llvm_return_type = codegen_type_to_llvm(codegen, return_type);
    if (!llvm_return_type) {
        codegen_error(codegen, decl->pos, "Failed to generate LLVM return type");
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
                codegen_error(codegen, decl->pos, "Failed to generate LLVM type for parameter %d", i);
                free(param_types);
                return 0;
            }
        }
    }
    
    LLVMTypeRef function_type = LLVMFunctionType(llvm_return_type, param_types, param_count, 0);
    
    // Create the function
    LLVMValueRef function = LLVMAddFunction(codegen->module, func_decl->name, function_type);
    
    // Handle WebAssembly exports/imports based on function attributes
    if (codegen_is_wasm_target(codegen)) {
        // Check for export/import annotations in function name or comments
        // For now, export main function and any function starting with "export_"
        if (strcmp(func_decl->name, "main") == 0) {
            codegen_add_wasm_export(codegen, function, "main");
        } else if (strncmp(func_decl->name, "export_", 7) == 0) {
            // Export with the name without the prefix
            codegen_add_wasm_export(codegen, function, func_decl->name + 7);
        } else if (strncmp(func_decl->name, "import_", 7) == 0) {
            // Import function - mark as external
            LLVMSetLinkage(function, LLVMExternalLinkage);
            // TODO: Add proper import module/name parsing
            codegen_add_wasm_import(codegen, function, "env", func_decl->name + 7);
        }
        
        // Add WebAssembly-specific function attributes
        if (return_type && return_type->kind == TYPE_VOID) {
            // Add no-return attribute for void functions if they don't return
            LLVMAttributeRef no_return_attr = LLVMCreateEnumAttribute(codegen->context, 
                                                                     LLVMGetEnumAttributeKindForName("noreturn", 8), 0);
            // Only add if function actually doesn't return (TODO: analyze control flow)
        }
    }
    
    // Create function info
    FunctionInfo* func_info = function_info_new(func_decl->name, function, return_type);
    if (!func_info) {
        codegen_error(codegen, decl->pos, "Failed to create function info");
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
            if (param->type == AST_IDENTIFIER) {
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
    
    // Add return if missing
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
        if (LLVMGetTypeKind(llvm_return_type) == LLVMVoidTypeKind) {
            LLVMBuildRetVoid(codegen->builder);
        } else {
            // Return zero/null for non-void functions without explicit return
            LLVMValueRef zero_val = LLVMConstNull(llvm_return_type);
            LLVMBuildRet(codegen->builder, zero_val);
        }
    }
    
    // Exit function scope
    codegen_exit_function(codegen);
    
    // Clean up function info
    function_info_free(func_info);
    
    return result;
#endif
}

int codegen_generate_var_decl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, decl->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !decl || decl->type != AST_VAR_DECL) return 0;
    
    VarDeclNode* var_decl = (VarDeclNode*)decl;
    
    // Get type from AST node (set during type checking)
    Type* var_type = decl->node_type;
    if (!var_type) {
        codegen_error(codegen, decl->pos, "Variable declaration has no type information");
        return 0;
    }
    
    // Generate code for each variable
    for (size_t i = 0; i < var_decl->name_count; i++) {
        const char* var_name = var_decl->names[i];
        
        // Convert type to LLVM type
        LLVMTypeRef llvm_type = codegen_type_to_llvm(codegen, var_type);
        if (!llvm_type) {
            codegen_error(codegen, decl->pos, "Failed to convert type for variable '%s'", var_name);
            return 0;
        }
        
        // Create alloca for the variable
        LLVMValueRef alloca_inst;
        if (codegen->current_function) {
            // Local variable
            alloca_inst = codegen_create_entry_alloca(codegen, llvm_type, var_name);
        } else {
            // Global variable
            alloca_inst = LLVMAddGlobal(codegen->module, llvm_type, var_name);
            LLVMSetInitializer(alloca_inst, LLVMConstNull(llvm_type));
        }
        
        if (!alloca_inst) {
            codegen_error(codegen, decl->pos, "Failed to create storage for variable '%s'", var_name);
            return 0;
        }
        
        // Generate initializer if present
        if (var_decl->values) {
            ValueInfo* init_value = codegen_generate_expression(codegen, checker, var_decl->values);
            if (!init_value) {
                codegen_error(codegen, decl->pos, "Failed to generate initializer for variable '%s'", var_name);
                return 0;
            }
            
            // Store the initial value
            if (codegen->current_function) {
                LLVMBuildStore(codegen->builder, init_value->llvm_value, alloca_inst);
            } else {
                // Global initializer
                if (LLVMIsConstant(init_value->llvm_value)) {
                    LLVMSetInitializer(alloca_inst, init_value->llvm_value);
                } else {
                    codegen_error(codegen, decl->pos, "Global variable '%s' requires constant initializer", var_name);
                    value_info_free(init_value);
                    return 0;
                }
            }
            
            value_info_free(init_value);
        }
        
        // Add to symbol table
        ValueInfo* value_info = value_info_new(var_name, alloca_inst, var_type);
        if (!value_info) {
            codegen_error(codegen, decl->pos, "Failed to create value info for variable '%s'", var_name);
            return 0;
        }
        
        value_info->is_lvalue = 1;
        value_info->is_initialized = (var_decl->values != NULL);
        
        if (!codegen_add_value(codegen, value_info)) {
            codegen_error(codegen, decl->pos, "Failed to add variable '%s' to symbol table", var_name);
            value_info_free(value_info);
            return 0;
        }
    }
    
    return 1;
#endif
}

int codegen_generate_const_decl(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decl) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, decl->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !decl || decl->type != AST_CONST_DECL) return 0;
    
    ConstDeclNode* const_decl = (ConstDeclNode*)decl;
    
    // Constants must have initializers
    if (!const_decl->values) {
        codegen_error(codegen, decl->pos, "Constant declaration must have initializer");
        return 0;
    }
    
    // Generate the constant value
    ValueInfo* const_value = codegen_generate_expression(codegen, checker, const_decl->values);
    if (!const_value) {
        codegen_error(codegen, decl->pos, "Failed to generate constant value");
        return 0;
    }
    
    // Constants must be compile-time constants
    if (!LLVMIsConstant(const_value->llvm_value)) {
        codegen_error(codegen, decl->pos, "Constant value must be compile-time constant");
        value_info_free(const_value);
        return 0;
    }
    
    // Generate code for each constant
    for (size_t i = 0; i < const_decl->name_count; i++) {
        const char* const_name = const_decl->names[i];
        
        // Get type from type checker
        Variable* var = type_checker_lookup_variable(checker, const_name);
        if (!var) {
            codegen_error(codegen, decl->pos, "Constant '%s' not found in type checker", const_name);
            value_info_free(const_value);
            return 0;
        }
        
        // Convert type to LLVM type
        LLVMTypeRef llvm_type = codegen_type_to_llvm(codegen, var->type);
        if (!llvm_type) {
            codegen_error(codegen, decl->pos, "Failed to convert type for constant '%s'", const_name);
            value_info_free(const_value);
            return 0;
        }
        
        // Create global constant
        LLVMValueRef global_const = LLVMAddGlobal(codegen->module, llvm_type, const_name);
        LLVMSetInitializer(global_const, const_value->llvm_value);
        LLVMSetGlobalConstant(global_const, 1);  // Mark as constant
        
        // Add to symbol table
        ValueInfo* value_info = value_info_new(const_name, global_const, var->type);
        if (!value_info) {
            codegen_error(codegen, decl->pos, "Failed to create value info for constant '%s'", const_name);
            value_info_free(const_value);
            return 0;
        }
        
        value_info->is_lvalue = 0;  // Constants are not lvalues
        value_info->is_initialized = 1;
        
        if (!codegen_add_value(codegen, value_info)) {
            codegen_error(codegen, decl->pos, "Failed to add constant '%s' to symbol table", const_name);
            value_info_free(value_info);
            value_info_free(const_value);
            return 0;
        }
    }
    
    value_info_free(const_value);
    return 1;
#endif
}

// Statement generation

int codegen_generate_statement(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
    if (!codegen || !checker || !stmt) return 0;
    
    switch (stmt->type) {
        case AST_BLOCK_STMT:
            return codegen_generate_block_stmt(codegen, checker, stmt);
        case AST_EXPR_STMT:
            return codegen_generate_expr_stmt(codegen, checker, stmt);
        case AST_VAR_DECL:
            return codegen_generate_var_decl(codegen, checker, stmt);
        case AST_IF_STMT:
            return codegen_generate_if_stmt(codegen, checker, stmt);
        case AST_FOR_STMT:
            return codegen_generate_for_stmt(codegen, checker, stmt);
        case AST_RETURN_STMT:
            return codegen_generate_return_stmt(codegen, checker, stmt);
        case AST_GO_STMT:
            return codegen_generate_go_stmt(codegen, checker, stmt);
        case AST_DEFER_STMT:
            return codegen_generate_defer_stmt(codegen, checker, stmt);
        case AST_SELECT_STMT:
            return codegen_generate_select_stmt(codegen, checker, stmt);
        case AST_UNSAFE_STMT:
            return codegen_generate_unsafe_stmt(codegen, checker, stmt);
        case AST_ASM_STMT:
            return codegen_generate_asm_stmt(codegen, checker, stmt);
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
            // TODO: Implement break/continue
            return 1;
        default:
            codegen_error(codegen, stmt->pos, "Unknown statement type for code generation");
            return 0;
    }
}

int codegen_generate_block_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_BLOCK_STMT) return 0;
    
    BlockStmtNode* block = (BlockStmtNode*)stmt;
    
    // Generate code for each statement in the block
    ASTNode* current = block->statements;
    while (current) {
        if (!codegen_generate_statement(codegen, checker, current)) {
            return 0;
        }
        current = current->next;
    }
    
    return 1;
#endif
}

int codegen_generate_expr_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_EXPR_STMT) return 0;
    
    ExprStmtNode* expr_stmt = (ExprStmtNode*)stmt;
    
    // Generate the expression (result is discarded)
    ValueInfo* result = codegen_generate_expression(codegen, checker, expr_stmt->expr);
    if (!result) {
        return 0;
    }
    
    value_info_free(result);
    return 1;
#endif
}

int codegen_generate_if_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_IF_STMT) return 0;
    
    IfStmtNode* if_stmt = (IfStmtNode*)stmt;
    
    // Generate condition
    ValueInfo* condition = codegen_generate_expression(codegen, checker, if_stmt->condition);
    if (!condition) {
        return 0;
    }
    
    // Create basic blocks
    LLVMBasicBlockRef then_block = codegen_create_block(codegen, "if.then");
    LLVMBasicBlockRef else_block = if_stmt->else_stmt ? codegen_create_block(codegen, "if.else") : NULL;
    LLVMBasicBlockRef merge_block = codegen_create_block(codegen, "if.merge");
    
    // Generate conditional branch
    LLVMValueRef cond_val = condition->llvm_value;
    value_info_free(condition);
    
    LLVMBuildCondBr(codegen->builder, cond_val, then_block, else_block ? else_block : merge_block);
    
    // Generate then block
    codegen_set_insert_point(codegen, then_block);
    if (!codegen_generate_statement(codegen, checker, if_stmt->then_stmt)) {
        return 0;
    }
    
    // Branch to merge block if no terminator
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
        LLVMBuildBr(codegen->builder, merge_block);
    }
    
    // Generate else block if present
    if (else_block) {
        codegen_set_insert_point(codegen, else_block);
        if (!codegen_generate_statement(codegen, checker, if_stmt->else_stmt)) {
            return 0;
        }
        
        // Branch to merge block if no terminator
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
            LLVMBuildBr(codegen->builder, merge_block);
        }
    }
    
    // Continue with merge block
    codegen_set_insert_point(codegen, merge_block);
    
    return 1;
#endif
}

int codegen_generate_for_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_FOR_STMT) return 0;
    
    ForStmtNode* for_stmt = (ForStmtNode*)stmt;
    
    // Create basic blocks
    LLVMBasicBlockRef init_block = codegen_create_block(codegen, "for.init");
    LLVMBasicBlockRef cond_block = codegen_create_block(codegen, "for.cond");
    LLVMBasicBlockRef body_block = codegen_create_block(codegen, "for.body");
    LLVMBasicBlockRef post_block = codegen_create_block(codegen, "for.post");
    LLVMBasicBlockRef exit_block = codegen_create_block(codegen, "for.exit");
    
    // Jump to init block
    LLVMBuildBr(codegen->builder, init_block);
    
    // Generate init block
    codegen_set_insert_point(codegen, init_block);
    if (for_stmt->init) {
        if (!codegen_generate_statement(codegen, checker, for_stmt->init)) {
            return 0;
        }
    }
    LLVMBuildBr(codegen->builder, cond_block);
    
    // Generate condition block
    codegen_set_insert_point(codegen, cond_block);
    if (for_stmt->condition) {
        ValueInfo* condition = codegen_generate_expression(codegen, checker, for_stmt->condition);
        if (!condition) {
            return 0;
        }
        
        LLVMBuildCondBr(codegen->builder, condition->llvm_value, body_block, exit_block);
        value_info_free(condition);
    } else {
        // Infinite loop
        LLVMBuildBr(codegen->builder, body_block);
    }
    
    // Generate body block
    codegen_set_insert_point(codegen, body_block);
    if (for_stmt->body) {
        if (!codegen_generate_statement(codegen, checker, for_stmt->body)) {
            return 0;
        }
    }
    LLVMBuildBr(codegen->builder, post_block);
    
    // Generate post block
    codegen_set_insert_point(codegen, post_block);
    if (for_stmt->post) {
        if (!codegen_generate_statement(codegen, checker, for_stmt->post)) {
            return 0;
        }
    }
    LLVMBuildBr(codegen->builder, cond_block);
    
    // Continue with exit block
    codegen_set_insert_point(codegen, exit_block);
    
    return 1;
#endif
}

#if LLVM_AVAILABLE
// Forward declaration for error return generation
LLVMValueRef codegen_generate_error_return(CodeGenerator* codegen, LLVMValueRef return_value, 
                                         Type* return_type, Type* function_return_type);
#endif

int codegen_generate_return_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_RETURN_STMT) return 0;
    
    ReturnStmtNode* return_stmt = (ReturnStmtNode*)stmt;
    
    if (return_stmt->values) {
        // Generate return value
        ValueInfo* return_value = codegen_generate_expression(codegen, checker, return_stmt->values);
        if (!return_value) {
            return 0;
        }
        
        // Get function return type for error union handling
        Type* function_return_type = NULL;
        if (codegen->current_function_info && codegen->current_function_info->goo_type) {
            function_return_type = codegen->current_function_info->goo_type;
        }
        
        // Handle error union returns
#if LLVM_AVAILABLE
        LLVMValueRef final_return_value = return_value->llvm_value;
        if (function_return_type) {
            final_return_value = codegen_generate_error_return(codegen, return_value->llvm_value, 
                                                             return_value->goo_type, function_return_type);
        }
        LLVMBuildRet(codegen->builder, final_return_value);
#else
        // Stub implementation when LLVM is not available
        codegen_generate_error_return(codegen, return_value->llvm_value, 
                                    return_value->goo_type, function_return_type);
#endif
        value_info_free(return_value);
    } else {
        // Void return
        LLVMBuildRetVoid(codegen->builder);
    }
    
    return 1;
#endif
}

int codegen_generate_go_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available for go statements");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_GO_STMT) return 0;
    
    GoStmtNode* go_stmt = (GoStmtNode*)stmt;
    
    // Handle WebAssembly-specific concurrency
    if (codegen_is_wasm_target(codegen)) {
        // In single-threaded WASM, transform goroutines to async/await
        // Generate JavaScript Promise-based execution
        
        if (go_stmt->call->type != AST_CALL_EXPR) {
            codegen_error(codegen, stmt->pos, "Go statement must contain a function call in WASM target");
            return 0;
        }
        
        CallExprNode* call = (CallExprNode*)go_stmt->call;
        
        // Create promise wrapper function
        LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
        LLVMValueRef create_promise_func = codegen_get_runtime_function(codegen, "js_create_promise");
        
        if (create_promise_func) {
            // Generate the function call as a promise
            ValueInfo* func_val = codegen_generate_expression(codegen, checker, call->function);
            if (!func_val) return 0;
            
            // Create promise with the function as executor
            LLVMValueRef args[] = { func_val->llvm_value };
            LLVMBuildCall2(codegen->builder, LLVMGetElementType(LLVMTypeOf(create_promise_func)),
                          create_promise_func, args, 1, "async_call");
            
            value_info_free(func_val);
            return 1;
        }
    }
    
    // Standard goroutine implementation for native targets
    // For go statements, we need to call goo_go with the function and arguments
    // This is a simplified implementation that assumes the call is a simple function call
    
    if (go_stmt->call->type != AST_CALL_EXPR) {
        codegen_error(codegen, stmt->pos, "Go statement must contain a function call");
        return 0;
    }
    
    CallExprNode* call = (CallExprNode*)go_stmt->call;
    
    // Generate the function address
    ValueInfo* func_val = codegen_generate_expression(codegen, checker, call->function);
    if (!func_val) return 0;
    
    // For simplicity, we'll use NULL as the argument for now
    // In a complete implementation, we'd need to package the arguments properly
    LLVMValueRef null_arg = LLVMConstNull(LLVMPointerType(LLVMInt8Type(), 0));
    
    // Get the goo_go function
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
    LLVMTypeRef func_ptr_type = LLVMPointerType(LLVMFunctionType(LLVMVoidType(), &void_ptr_type, 1, 0), 0);
    LLVMTypeRef param_types[] = { func_ptr_type, void_ptr_type };
    LLVMTypeRef goo_go_type = LLVMFunctionType(void_ptr_type, param_types, 2, 0);
    
    LLVMValueRef goo_go_func = LLVMGetNamedFunction(codegen->module, "goo_go");
    if (!goo_go_func) {
        // Declare goo_go if not already declared
        goo_go_func = LLVMAddFunction(codegen->module, "goo_go", goo_go_type);
    }
    
    // Call goo_go(func, arg)
    LLVMValueRef args[] = { func_val->llvm_value, null_arg };
    LLVMBuildCall2(codegen->builder, goo_go_type, goo_go_func, args, 2, "");
    
    value_info_free(func_val);
    return 1;
#endif
}

int codegen_generate_defer_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available for defer statements");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_DEFER_STMT) return 0;
    
    // TODO: Implement defer statements
    // For now, just treat it as a regular function call for compilation purposes
    DeferStmtNode* defer_stmt = (DeferStmtNode*)stmt;
    
    // Generate the deferred call as a regular expression for now
    ValueInfo* result = codegen_generate_expression(codegen, checker, defer_stmt->call);
    if (result) {
        value_info_free(result);
    }
    
    return 1;
#endif
}

int codegen_generate_select_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available for select statements");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_SELECT_STMT) return 0;
    
    SelectStmtNode* select_stmt = (SelectStmtNode*)stmt;
    
    // Count the number of cases
    size_t case_count = 0;
    ASTNode* case_node = select_stmt->cases;
    while (case_node) {
        case_count++;
        case_node = case_node->next;
    }
    
    if (case_count == 0) {
        codegen_error(codegen, stmt->pos, "Select statement must have at least one case");
        return 0;
    }
    
    // Create array of select cases
    LLVMTypeRef select_case_type = codegen_get_select_case_type(codegen);
    LLVMValueRef cases_array = LLVMBuildArrayAlloca(codegen->builder, select_case_type, 
                                                   LLVMConstInt(LLVMInt64Type(), case_count, 0), 
                                                   "select_cases");
    
    // Create basic blocks for each case and the end
    LLVMBasicBlockRef* case_blocks = malloc(sizeof(LLVMBasicBlockRef) * case_count);
    LLVMBasicBlockRef default_block = NULL;
    LLVMBasicBlockRef end_block = LLVMAppendBasicBlock(codegen->current_function, "select_end");
    
    // Generate case blocks
    case_node = select_stmt->cases;
    size_t case_index = 0;
    int has_default = 0;
    
    while (case_node && case_index < case_count) {
        SelectCaseNode* select_case = (SelectCaseNode*)case_node;
        
        if (select_case->comm == NULL) {
            // Default case
            if (has_default) {
                codegen_error(codegen, case_node->pos, "Select statement can only have one default case");
                free(case_blocks);
                return 0;
            }
            has_default = 1;
            default_block = LLVMAppendBasicBlock(codegen->current_function, "select_default");
            case_blocks[case_index] = default_block;
        } else {
            // Regular case
            char case_name[32];
            snprintf(case_name, sizeof(case_name), "select_case_%zu", case_index);
            case_blocks[case_index] = LLVMAppendBasicBlock(codegen->current_function, case_name);
            
            // Setup select case data
            if (!codegen_setup_select_case(codegen, checker, cases_array, case_index, select_case)) {
                free(case_blocks);
                return 0;
            }
        }
        
        case_node = case_node->next;
        case_index++;
    }
    
    // Call goo_select to determine which case is ready
    LLVMValueRef select_func = codegen_get_select_function(codegen);
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
    LLVMTypeRef param_types[] = {
        void_ptr_type,   // goo_select_case_t* cases
        LLVMInt64Type(), // size_t num_cases
        LLVMInt64Type()  // int64_t timeout_ns
    };
    LLVMTypeRef func_type = LLVMFunctionType(LLVMInt32Type(), param_types, 3, 0);
    
    LLVMValueRef case_count_val = LLVMConstInt(LLVMInt64Type(), case_count - (has_default ? 1 : 0), 0);
    LLVMValueRef timeout_val = LLVMConstInt(LLVMInt64Type(), -1, 0); // No timeout
    
    LLVMValueRef args[] = { cases_array, case_count_val, timeout_val };
    LLVMValueRef selected_case = LLVMBuildCall2(codegen->builder, func_type, select_func, args, 3, "selected_case");
    
    // Create switch based on the result
    LLVMValueRef switch_inst = LLVMBuildSwitch(codegen->builder, selected_case, 
                                               has_default ? default_block : end_block, 
                                               (unsigned)(case_count - (has_default ? 1 : 0)));
    
    // Add cases to switch
    case_index = 0;
    case_node = select_stmt->cases;
    while (case_node && case_index < case_count) {
        SelectCaseNode* select_case = (SelectCaseNode*)case_node;
        
        if (select_case->comm != NULL) {
            // Regular case - add to switch
            LLVMValueRef case_val = LLVMConstInt(LLVMInt32Type(), case_index, 0);
            LLVMAddCase(switch_inst, case_val, case_blocks[case_index]);
        }
        
        case_node = case_node->next;
        case_index++;
    }
    
    // Generate code for each case block
    case_index = 0;
    case_node = select_stmt->cases;
    while (case_node && case_index < case_count) {
        SelectCaseNode* select_case = (SelectCaseNode*)case_node;
        
        LLVMPositionBuilderAtEnd(codegen->builder, case_blocks[case_index]);
        
        // Generate case body
        if (select_case->body) {
            if (!codegen_generate_statement(codegen, checker, select_case->body)) {
                free(case_blocks);
                return 0;
            }
        }
        
        // Branch to end
        LLVMBuildBr(codegen->builder, end_block);
        
        case_node = case_node->next;
        case_index++;
    }
    
    // Position builder at end block
    LLVMPositionBuilderAtEnd(codegen->builder, end_block);
    
    free(case_blocks);
    return 1;
#endif
}

#if LLVM_AVAILABLE
// Helper function to get select case type
LLVMTypeRef codegen_get_select_case_type(CodeGenerator* codegen __attribute__((unused))) {
    // Create struct type for goo_select_case_t
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
    LLVMTypeRef field_types[] = {
        void_ptr_type,  // goo_channel_t* channel
        void_ptr_type,  // void* data
        LLVMInt32Type(), // int is_send
        LLVMInt32Type()  // int ready
    };
    return LLVMStructType(field_types, 4, 0);
}
#endif

#if LLVM_AVAILABLE
// Helper function to get goo_select function
LLVMValueRef codegen_get_select_function(CodeGenerator* codegen) {
    LLVMValueRef select_func = LLVMGetNamedFunction(codegen->module, "goo_select");
    if (!select_func) {
        // Declare goo_select if not already declared
        LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
        LLVMTypeRef param_types[] = {
            void_ptr_type,   // goo_select_case_t* cases
            LLVMInt64Type(), // size_t num_cases
            LLVMInt64Type()  // int64_t timeout_ns
        };
        LLVMTypeRef func_type = LLVMFunctionType(LLVMInt32Type(), param_types, 3, 0);
        select_func = LLVMAddFunction(codegen->module, "goo_select", func_type);
    }
    return select_func;
}
#endif

#if LLVM_AVAILABLE
// Helper function to setup select case data
int codegen_setup_select_case(CodeGenerator* codegen, TypeChecker* checker, 
                              LLVMValueRef cases_array, size_t case_index, 
                              SelectCaseNode* select_case) {
    // Get pointer to the case struct in the array
    LLVMTypeRef select_case_type = codegen_get_select_case_type(codegen);
    LLVMValueRef indices[] = {
        LLVMConstInt(LLVMInt32Type(), 0, 0),           // Array index
        LLVMConstInt(LLVMInt32Type(), case_index, 0)   // Case index
    };
    LLVMValueRef case_ptr = LLVMBuildGEP2(codegen->builder, select_case_type, cases_array, indices, 2, "case_ptr");
    
    // Parse the communication operation
    if (!select_case->comm) return 0;
    
    LLVMValueRef channel = NULL;
    LLVMValueRef data_ptr = NULL;
    int is_send = 0;
    
    // Determine if this is a send or receive operation
    if (select_case->comm->type == AST_BINARY_EXPR) {
        // Channel send: ch <- value
        BinaryExprNode* binary = (BinaryExprNode*)select_case->comm;
        if (binary->operator == TOKEN_ARROW) {
            is_send = 1;
            
            // Generate channel and value
            ValueInfo* channel_val = codegen_generate_expression(codegen, checker, binary->left);
            if (!channel_val) return 0;
            channel = channel_val->llvm_value;
            
            ValueInfo* value_val = codegen_generate_expression(codegen, checker, binary->right);
            if (!value_val) return 0;
            
            // Get pointer to the value
            if (value_val->is_lvalue) {
                data_ptr = value_val->llvm_value;
            } else {
                // Store value temporarily
                LLVMValueRef temp_alloca = LLVMBuildAlloca(codegen->builder, 
                                                          LLVMTypeOf(value_val->llvm_value), 
                                                          "temp_send_value");
                LLVMBuildStore(codegen->builder, value_val->llvm_value, temp_alloca);
                data_ptr = temp_alloca;
            }
            
            value_info_free(channel_val);
            value_info_free(value_val);
        }
    } else if (select_case->comm->type == AST_UNARY_EXPR) {
        // Channel receive: <-ch
        UnaryExprNode* unary = (UnaryExprNode*)select_case->comm;
        if (unary->operator == TOKEN_ARROW) {
            is_send = 0;
            
            // Generate channel
            ValueInfo* channel_val = codegen_generate_expression(codegen, checker, unary->operand);
            if (!channel_val) return 0;
            channel = channel_val->llvm_value;
            
            // Allocate space for received value
            data_ptr = LLVMBuildAlloca(codegen->builder, LLVMInt32Type(), "recv_space");
            
            value_info_free(channel_val);
        }
    }
    
    if (!channel || !data_ptr) {
        codegen_error(codegen, select_case->comm->pos, "Invalid channel operation in select case");
        return 0;
    }
    
    // Cast pointers to void*
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
    channel = LLVMBuildBitCast(codegen->builder, channel, void_ptr_type, "channel_void_ptr");
    data_ptr = LLVMBuildBitCast(codegen->builder, data_ptr, void_ptr_type, "data_void_ptr");
    
    // Set the case fields
    // case_ptr->channel = channel
    LLVMValueRef channel_field_ptr = LLVMBuildStructGEP2(codegen->builder, select_case_type, case_ptr, 0, "channel_field");
    LLVMBuildStore(codegen->builder, channel, channel_field_ptr);
    
    // case_ptr->data = data_ptr
    LLVMValueRef data_field_ptr = LLVMBuildStructGEP2(codegen->builder, select_case_type, case_ptr, 1, "data_field");
    LLVMBuildStore(codegen->builder, data_ptr, data_field_ptr);
    
    // case_ptr->is_send = is_send
    LLVMValueRef is_send_field_ptr = LLVMBuildStructGEP2(codegen->builder, select_case_type, case_ptr, 2, "is_send_field");
    LLVMValueRef is_send_val = LLVMConstInt(LLVMInt1Type(), is_send, 0);
    LLVMBuildStore(codegen->builder, is_send_val, is_send_field_ptr);
    
    return 1;
}

// Unsafe statement generation
int codegen_generate_unsafe_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_UNSAFE_STMT) return 0;
    
    UnsafeStmtNode* unsafe_stmt = (UnsafeStmtNode*)stmt;
    
    // For now, unsafe blocks are just transparent - they contain the actual unsafe operations
    // In the future, we might want to add runtime checks or metadata here
    
    // Generate the body of the unsafe block
    return codegen_generate_statement(codegen, checker, unsafe_stmt->body);
#endif
}

// Inline assembly statement generation
int codegen_generate_asm_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_ASM_STMT) return 0;
    
    AsmStmtNode* asm_stmt = (AsmStmtNode*)stmt;
    
    // Create inline assembly function type (void -> void for now)
    LLVMTypeRef func_type = LLVMFunctionType(LLVMVoidType(), NULL, 0, 0);
    
    // Create inline assembly with the provided assembly code
    const char* constraints = "~{dirflag},~{fpsr},~{flags}"; // Basic x86 clobbers
    
    LLVMValueRef inline_asm = LLVMGetInlineAsm(func_type, 
                                               asm_stmt->assembly_code, strlen(asm_stmt->assembly_code),
                                               (char*)constraints, strlen(constraints),
                                               1, 1, LLVMInlineAsmDialectIntel, 0);
    
    // Call the inline assembly
    LLVMBuildCall2(codegen->builder, func_type, inline_asm, NULL, 0, "inline_asm");
    
    return 1;
#endif
}
#endif