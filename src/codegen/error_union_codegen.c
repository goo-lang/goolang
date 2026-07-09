#include "codegen.h"
#include "value_scope.h"
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

    // Propagation block: re-wrap the operand's ERROR into the ENCLOSING
    // function's error-union type and return THAT. `try` propagates only the
    // error (not the operand's value), so the operand and the enclosing function
    // may have different VALUE types — the headline cross-value-type pattern,
    // e.g. an `!string` operand propagated out of an `!int` function. Ret-ing
    // the WHOLE operand would be an ABI mismatch in that case ("Function return
    // type does not match operand type"); instead we extract the error (the
    // error slot is always a string in Phase 1) and build a fresh error union of
    // the enclosing return type. The type checker (type_check_try_expr)
    // guarantees the enclosing function returns `!T` and the error types are
    // compatible, so the else-branch is defensive (clean codegen_error rather
    // than garbage IR if the gate ever regresses).
    codegen_set_insert_point(codegen, propagate_block);
    FunctionInfo* cur = codegen->current_function_info;
    if (cur && cur->goo_type && type_is_error_union(cur->goo_type)) {
        LLVMValueRef err_val =
            codegen_error_union_get_error(codegen, operand_info->llvm_value);
        LLVMTypeRef enclosing_union = codegen_type_to_llvm(codegen, cur->goo_type);
        LLVMValueRef rewrapped =
            codegen_create_error_union_error(codegen, enclosing_union, err_val);
        LLVMBuildRet(codegen->builder, rewrapped);
    } else {
        codegen_error(codegen, expr->pos,
                      "try used outside an error-union function "
                      "(should have been rejected by the type checker)");
        // Terminate the block so the (rejected) module is still structurally
        // valid; error_count>0 already fails the build before emission.
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
// Generate the catch handler block on the error path. Statements run for their
// side effects; if the block's final statement is a value-producing expression
// (non-void) its value is returned via *out_value (coerced to the value type's
// LLVM type) to serve as the recovery result. *out_value is left untouched when
// the handler is side-effect-only (void trailing expr or a non-expression
// terminator) or when the block already terminated (e.g. `return`). Returns 0
// on a codegen failure. Mirrors codegen_generate_block's value-table scope
// teardown so inner `x := ...` bindings do not leak past the handler.
static int generate_catch_body_value(CodeGenerator* codegen, TypeChecker* checker,
                                     ASTNode* body, Type* value_type,
                                     LLVMValueRef* out_value) {
    // Grammar guarantees a block; fall back to a plain statement for safety.
    if (!body || body->type != AST_BLOCK_STMT) {
        return codegen_generate_statement(codegen, checker, body);
    }

    ASTNode* trailing = ast_block_trailing_expr(body);
    BlockStmtNode* block = (BlockStmtNode*)body;
    size_t pre_block_vt_size = vscope_enter(codegen);

    for (ASTNode* s = block->statements; s; s = s->next) {
        // Once the block has a terminator, appending more would be invalid.
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)))
            break;

        // The final statement supplies the recovery value when it is a
        // value-producing expression (non-void). A void trailing call is a
        // side-effect-only handler and recovers with the zero value of T.
        int is_trailing_value =
            s->next == NULL && s->type == AST_EXPR_STMT &&
            ((ExprStmtNode*)s)->expr == trailing &&
            trailing->node_type && trailing->node_type->kind != TYPE_VOID;

        if (is_trailing_value) {
            ValueInfo* vi = codegen_generate_expression(codegen, checker, trailing);
            if (!vi) {
                vscope_exit(codegen, pre_block_vt_size);
                return 0;
            }
            // A handler that ends in a named location (field selector, indexed
            // element) yields an lvalue whose llvm_value is the storage pointer,
            // not the scalar. Load it first so the PHI receives a value of T's
            // type, not a pointer (mirrors the if-let load in statement_codegen.c).
            if (vi->is_lvalue && vi->goo_type) {
                LLVMTypeRef lt = codegen_type_to_llvm(codegen, vi->goo_type);
                if (lt) {
                    vi->llvm_value = LLVMBuildLoad2(codegen->builder, lt,
                                                    vi->llvm_value, "catch_trail_load");
                    vi->is_lvalue = 0;
                }
            }
            LLVMTypeRef from = LLVMTypeOf(vi->llvm_value);
            LLVMTypeRef to = codegen_type_to_llvm(codegen, value_type);
            *out_value = (from == to)
                ? vi->llvm_value
                : codegen_convert_value(codegen, vi->llvm_value, from, to);
            value_info_free(vi);
        } else if (!codegen_generate_statement(codegen, checker, s)) {
            vscope_exit(codegen, pre_block_vt_size);
            return 0;
        }
    }

    vscope_exit(codegen, pre_block_vt_size);
    return 1;
}

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

    // Mirror the type-checker scope that type_check_catch_expr pushes around
    // the error variable + catch body (expression_checker.c). Without this,
    // a re-entrant type_check_expression call from inside body codegen (e.g.
    // call_codegen.c's `e.Error()` method-call special case, which re-derives
    // the receiver's type) can't resolve `e` and reports "Undefined variable"
    // — same requirement composite_codegen.c's match-arm binding documents.
    scope_push(checker);

    // Bind the error variable in the codegen value table so that uses of it
    // inside the catch body (e.g. `fmt.Println(e)`, `e.Error()`) resolve
    // correctly.
    //
    // P2-7: the type checker now binds catch_expr->error_var as the real
    // `error` interface (type_checker_error_type — the nullable {i1 is_null,
    // i8* handle} pointer, see expression_checker.c's type_check_catch_expr),
    // matching the n,err destructure path. Codegen must box the union's raw
    // error arm into that same shape via goo_error_from_string, mirroring
    // function_codegen.c:1690-1761 exactly (including its documented
    // non-string-arm degradation), so extraction later matches the working
    // call_codegen.c:1119-1159 .Error()/fmt.Println read.
    if (catch_expr->error_var) {
        LLVMValueRef error_raw = codegen_error_union_get_error(codegen, operand_info->llvm_value);
        Type* err_arm_type = operand_info->goo_type->data.error_union.error_type;

        Type* error_type = type_checker_error_type(checker);
        LLVMTypeRef error_llvm = codegen_type_to_llvm(codegen, error_type);
        LLVMTypeRef i8pt = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);

        if (error_raw && error_llvm) {
            // Only a string-shaped arm (the default NULL arm, or an explicit
            // TYPE_STRING arm — both are goo_string_t, see function_codegen.c:
            // 1705-1720) is something goo_error_from_string can box. A
            // genuinely non-string explicit error arm isn't a goo_string;
            // boxing it would build invalid IR, so it degrades to a non-null
            // marker instead — `e != nil` still holds, e.Error() yields ""
            // (no message), same tradeoff the destructure path documents.
            int default_arm = (err_arm_type == NULL) || (err_arm_type->kind == TYPE_STRING);

            LLVMValueRef handle;
            if (default_arm) {
                LLVMValueRef from_str = LLVMGetNamedFunction(codegen->module, "goo_error_from_string");
                if (!from_str) {
                    codegen_error(codegen, expr->pos, "goo_error_from_string not found in module");
                    value_info_free(operand_info);
                    scope_pop(checker);
                    return NULL;
                }
                LLVMValueRef fargs[] = { error_raw };
                handle = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(from_str),
                                        from_str, fargs, 1, "catch.err.boxed");
            } else {
                handle = LLVMBuildIntToPtr(codegen->builder,
                    LLVMConstInt(LLVMInt64TypeInContext(codegen->context), 1, 0),
                    i8pt, "catch.err.marker");
            }

            // The catch error block only runs when is_error was true, so the
            // bound variable's nullable is_null is unconditionally false here
            // (unlike the destructure path, which PHIs across both branches
            // because it binds unconditionally at the call site).
            LLVMValueRef is_null = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), 0, 0);
            LLVMValueRef error_val = LLVMGetUndef(error_llvm);
            error_val = LLVMBuildInsertValue(codegen->builder, error_val, is_null, 0, "catch.err.is_null");
            error_val = LLVMBuildInsertValue(codegen->builder, error_val, handle, 1, "catch.err.ptr");

            LLVMValueRef error_alloca = codegen_alloc_local(
                codegen, error_llvm, catch_expr->error_var);
            LLVMBuildStore(codegen->builder, error_val, error_alloca);
            ValueInfo* error_vi = value_info_new(catch_expr->error_var,
                                                 error_alloca, error_type);
            error_vi->is_lvalue = 1;
            error_vi->is_initialized = 1;
            vscope_add(codegen, error_vi);

            // Mirror into the type-checker scope so any re-entrant
            // type_check_* calls inside the catch body can resolve `e`.
            Variable* error_tv = variable_new(catch_expr->error_var, error_type, expr->pos);
            if (error_tv) {
                error_tv->is_initialized = 1;
                scope_add_variable(checker->current_scope, error_tv);
            }
        }
    }

    // Generate the catch body (grammar guarantees a block). A value-producing
    // handler (final statement is a non-void expression) supplies the recovery
    // value via body_value; a side-effect-only handler leaves it NULL. Track
    // whether the body emits a terminator (e.g. `return`) so we only add a PHI
    // incoming from the error side when the block falls through.
    LLVMValueRef catch_value = NULL;
    LLVMValueRef body_value = NULL;
    LLVMBasicBlockRef error_exit_block = NULL;
    Type* vtype = operand_info->goo_type->data.error_union.value_type;

    if (catch_expr->catch_body) {
        if (!generate_catch_body_value(codegen, checker, catch_expr->catch_body,
                                       vtype, &body_value)) {
            value_info_free(operand_info);
            scope_pop(checker);
            return NULL;
        }
    }

    // Restore the type-checker scope pushed above the error variable binding.
    scope_pop(checker);

    // Recovery merge: if the error block fell through (the body did not end in
    // a terminator such as `return`, or there was no body), the catch recovers
    // and the merged result is the value-producing handler's value when present,
    // otherwise the zero value of the union's value type T. A body that already
    // terminated contributes no incoming to the merge PHI.
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
        LLVMTypeRef vllvm = codegen_type_to_llvm(codegen, vtype);
        catch_value = body_value ? body_value : LLVMConstNull(vllvm);
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

    // P2-3: an !T method is registered by the type checker — and called — under
    // its mangled name "T__m", with the receiver spliced in as params[0]. Use
    // that mangled name for the type-info lookup and the emitted symbol (mirrors
    // function_codegen.c:218-228); without it the bare-name lookup misses, the
    // param/receiver binding loop never runs, and the body fails with an
    // "Undefined identifier" for the first parameter. func_decl->name stays the
    // bare method name for diagnostics.
    char* mangled = NULL;
    const char* emit_name = func_decl->name;
    if (func_decl->receiver) {
        VarDeclNode* recv = (VarDeclNode*)func_decl->receiver;
        Type* recv_type = recv->type ? type_from_ast(checker, recv->type) : NULL;
        const char* tn = type_receiver_name(recv_type);
        if (tn) {
            mangled = type_method_mangled_name(tn, func_decl->name);
            if (mangled) emit_name = mangled;
        }
    }

    // Get function type info from type checker
    Variable* func_var = type_checker_lookup_variable(checker, emit_name);
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

    // stdlib Phase 0 (Task 4): mirror function_codegen.c — a non-main package's
    // error-union functions (and methods) must ALSO be emitted under the
    // package-mangled symbol `goo_pkg__<pkg>__<base>`, not the bare `emit_name`.
    // Without this a package `func TryParse(x int) !int` would emit `@TryParse`,
    // colliding with a same-named main symbol and diverging from pkg->exports /
    // Task 5's mangled-name call resolution. `emit_name` (bare, or method `T__m`)
    // stays the type-checker lookup key; only the LLVM symbol is prefixed. For
    // the main package the helper returns NULL, so the symbol stays bare.
    const char* symbol_name = emit_name;
    char* pkg_mangled = codegen_package_symbol_name(checker, emit_name);
    if (pkg_mangled) symbol_name = pkg_mangled;

    // Create the function under its emitted (possibly mangled) name so method
    // call sites, which emit a call to "T__m", resolve to this definition.
    LLVMValueRef function = LLVMAddFunction(codegen->module, symbol_name, function_type);

    // Create function info
    FunctionInfo* func_info = function_info_new(symbol_name, function, return_type);
    if (!func_info) {
        codegen_error(codegen, func_decl->base.pos, "Failed to create function info");
        if (param_types) free(param_types);
        free(mangled);
        free(pkg_mangled);
        return 0;
    }
    free(mangled);       // emit_name was copied by LLVMAddFunction / function_info_new
    free(pkg_mangled);   // symbol_name likewise copied; safe to free the mangled buffer

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

                LLVMValueRef param_alloca = codegen_alloc_local(codegen, param_types[param_index], param_name);
                LLVMBuildStore(codegen->builder, param_value, param_alloca);

                ValueInfo* param_info = value_info_new(param_name, param_alloca,
                                                      func_type_info->data.function.param_types[param_index]);
                param_info->is_lvalue = 1;
                param_info->is_initialized = 1;
                vscope_add(codegen, param_info);

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