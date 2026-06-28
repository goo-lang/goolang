#include "codegen.h"
#include "comptime.h"
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

    // A method is emitted as an ordinary function under its mangled name
    // "T__m" (matching the type checker's registration). The receiver is
    // params[0] (spliced by the parser), so the param-binding loop below
    // handles it with no special-casing. `emit_name` is the LLVM/symbol
    // name; func_decl->name stays the bare method name for diagnostics.
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

    // The Goo `main` is the C program entry point: lower a void main to
    // `i32 @main` so it returns 0 on normal completion. Otherwise main emitted
    // `ret void`, leaving the process exit code as a garbage register value.
    int is_entry_main = (!func_decl->receiver &&
                         strcmp(func_decl->name, "main") == 0 &&
                         return_type->kind == TYPE_VOID);
    if (is_entry_main) {
        llvm_return_type = LLVMInt32TypeInContext(codegen->context);
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
                codegen_error(codegen, decl->pos, "Failed to generate LLVM type for parameter %d", i);
                free(param_types);
                return 0;
            }
        }
    }
    
    LLVMTypeRef function_type = LLVMFunctionType(llvm_return_type, param_types, param_count, 0);
    
    // Create the function
    LLVMValueRef function = LLVMAddFunction(codegen->module, emit_name, function_type);
    
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
    FunctionInfo* func_info = function_info_new(emit_name, function, return_type);
    free(mangled);  // emit_name was copied by LLVMAddFunction and function_info_new
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

    // Mirror the type-checker scope so re-invocations of type_check_*
    // from inside codegen (e.g. type_check_binary_expr at
    // expression_codegen.c:208) can resolve `a` and `b` inside the
    // function body. Without this the type-checker scope is whatever
    // was last left around (usually global) and any identifier lookup
    // from codegen fails. Mirror only the params; the body's nested
    // blocks will push their own scopes the same way the type-check
    // pass did.
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
    
    // Generate function parameters as local variables. The parser builds
    // params as AST_VAR_DECL nodes (see parser.y::func_param creating
    // VarDeclNode with names[0]/name_count=1), not AST_IDENTIFIER as
    // this loop previously assumed. The mismatched check meant the loop
    // body never ran and `a` / `b` in any function body resolved to
    // "Undefined identifier" — every user function with parameters was
    // broken end-to-end.
    if (func_decl->params && param_count > 0) {
        ASTNode* param = func_decl->params;
        int param_index = 0;

        while (param && param_index < param_count) {
            const char* param_name = NULL;
            if (param->type == AST_VAR_DECL) {
                VarDeclNode* pd = (VarDeclNode*)param;
                if (pd->name_count > 0 && pd->names) param_name = pd->names[0];
            } else if (param->type == AST_IDENTIFIER) {
                // Defensive: keep the old path working for any path that
                // builds params as bare identifiers.
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
    
    // Add return if missing
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
        if (is_entry_main) {
            // Run-to-completion barrier: block until every goroutine spawned via
            // `go` has finished, so their side effects are observable before the
            // process exits. The scheduler is lazily created by the first
            // goo_go(); goo_scheduler_wait() is a no-op when none ran. (Programs
            // with an explicit `return` in main bypass this — out of M8 scope.)
            LLVMTypeRef wait_ty = LLVMFunctionType(LLVMVoidTypeInContext(codegen->context), NULL, 0, 0);
            LLVMValueRef wait_fn = LLVMGetNamedFunction(codegen->module, "goo_scheduler_wait");
            if (!wait_fn) wait_fn = LLVMAddFunction(codegen->module, "goo_scheduler_wait", wait_ty);
            LLVMBuildCall2(codegen->builder, wait_ty, wait_fn, NULL, 0, "");
        }
        if (LLVMGetTypeKind(llvm_return_type) == LLVMVoidTypeKind) {
            LLVMBuildRetVoid(codegen->builder);
        } else {
            // Return zero/null for non-void functions without explicit return
            LLVMValueRef zero_val = LLVMConstNull(llvm_return_type);
            LLVMBuildRet(codegen->builder, zero_val);
        }
    }
    
    // Exit function scope (codegen value table and mirrored type-check scope)
    codegen_exit_function(codegen);
    scope_pop(checker);

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

    // Multi-LHS short var decl `a, b := f()` — evaluate RHS once,
    // destructure via ExtractValue. Per-name types come from the
    // struct's fields. Codepath returns early after handling.
    if (var_decl->name_count > 1 && var_type->kind == TYPE_STRUCT &&
        var_decl->values && var_type->data.struct_type.field_count >= var_decl->name_count) {
        ValueInfo* rhs = NULL;

        // comma-ok map read: `v, ok := m[k]` — call goo_map_get_sv_ok to
        // get both the value slot and a found flag, then pack them into a
        // {V, i1} aggregate so the generic ExtractValue loop below can bind
        // name0→V and name1→ok without any special per-name logic.
        if (var_decl->name_count == 2 && var_decl->is_short_decl &&
            var_decl->values->type == AST_INDEX_EXPR) {
            IndexExprNode* idx_expr = (IndexExprNode*)var_decl->values;
            if (idx_expr->expr && idx_expr->expr->node_type &&
                idx_expr->expr->node_type->kind == TYPE_MAP) {
                Type* val_type = idx_expr->expr->node_type->data.map.value_type;

                // Evaluate the map pointer and the key expression.
                ValueInfo* map_val = codegen_generate_expression(codegen, checker, idx_expr->expr);
                ValueInfo* key_val = codegen_generate_expression(codegen, checker, idx_expr->index);
                if (!map_val || !key_val) {
                    codegen_error(codegen, decl->pos, "Failed to evaluate comma-ok map operands");
                    value_info_free(map_val);
                    value_info_free(key_val);
                    return 0;
                }

                // Obtain goo_map_get_sv_ok; it is pre-declared by runtime_integration.c.
                LLVMValueRef ok_fn = LLVMGetNamedFunction(codegen->module, "goo_map_get_sv_ok");
                if (!ok_fn) {
                    codegen_error(codegen, decl->pos, "goo_map_get_sv_ok missing from module");
                    value_info_free(map_val);
                    value_info_free(key_val);
                    return 0;
                }

                // Alloca output slots in the entry block (mem2reg-friendly).
                LLVMTypeRef i64t = LLVMInt64TypeInContext(codegen->context);
                LLVMTypeRef i32t = LLVMInt32TypeInContext(codegen->context);
                LLVMValueRef out_slot   = codegen_create_entry_alloca(codegen, i64t, "commaok_out");
                LLVMValueRef found_slot = codegen_create_entry_alloca(codegen, i32t, "commaok_found");

                // String key: extract the raw char* from the {i8*, i64} struct.
                LLVMValueRef kp = key_val->llvm_value;
                if (key_val->goo_type && key_val->goo_type->kind == TYPE_STRING) {
                    kp = LLVMBuildExtractValue(codegen->builder, kp, 0, "k_ptr");
                }

                // Call goo_map_get_sv_ok(map, key, &out, &found).
                LLVMValueRef call_args[4] = { map_val->llvm_value, kp, out_slot, found_slot };
                LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(ok_fn),
                               ok_fn, call_args, 4, "");

                // Load the raw i64 slot and convert to V (handles sign/zero-ext).
                LLVMValueRef raw_slot = LLVMBuildLoad2(codegen->builder, i64t, out_slot, "commaok_slot");
                LLVMValueRef val = codegen_map_slot_to_value(codegen, raw_slot, val_type);

                // Load found (i32) and truncate to i1 for the bool field.
                LLVMValueRef found_i32 = LLVMBuildLoad2(codegen->builder, i32t, found_slot, "commaok_fi");
                LLVMTypeRef  i1t       = LLVMInt1TypeInContext(codegen->context);
                LLVMValueRef ok_bit    = LLVMBuildTrunc(codegen->builder, found_i32, i1t, "commaok_ok");

                // Pack into {V, i1} struct; ExtractValue loop below will unpack.
                LLVMTypeRef val_llvm = codegen_type_to_llvm(codegen, val_type);
                LLVMTypeRef agg_fields[2] = { val_llvm, i1t };
                LLVMTypeRef agg_type = LLVMStructTypeInContext(codegen->context, agg_fields, 2, 0);
                LLVMValueRef agg = LLVMGetUndef(agg_type);
                agg = LLVMBuildInsertValue(codegen->builder, agg, val,    0, "commaok_v");
                agg = LLVMBuildInsertValue(codegen->builder, agg, ok_bit, 1, "commaok_agg");

                value_info_free(map_val);
                value_info_free(key_val);
                rhs = value_info_new(NULL, agg, var_type);
            }
        }

        // Non-map-ok path: generic struct-return destructure (e.g. `a, b := f()`).
        if (!rhs) {
            rhs = codegen_generate_expression(codegen, checker, var_decl->values);
        }
        if (!rhs) {
            codegen_error(codegen, decl->pos, "Failed to generate multi-LHS RHS");
            return 0;
        }
        for (size_t i = 0; i < var_decl->name_count; i++) {
            const char* nm = var_decl->names[i];
            Type* field_type = var_type->data.struct_type.fields[i].type;
            LLVMTypeRef field_llvm = codegen_type_to_llvm(codegen, field_type);
            LLVMValueRef field_val = LLVMBuildExtractValue(codegen->builder, rhs->llvm_value, (unsigned)i, nm);
            LLVMValueRef field_alloca = codegen_create_entry_alloca(codegen, field_llvm, nm);
            LLVMBuildStore(codegen->builder, field_val, field_alloca);
            ValueInfo* vi = value_info_new(nm, field_alloca, field_type);
            vi->is_lvalue = 1;
            vi->is_initialized = 1;
            codegen_add_value(codegen, vi);
            Variable* tv = variable_new(nm, field_type, decl->pos);
            if (tv) {
                tv->is_initialized = 1;
                scope_add_variable(checker->current_scope, tv);
            }
        }
        value_info_free(rhs);
        return 1;
    }

    // Generate code for each variable (single-LHS path)
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
            // Local variable. Zero-initialize on alloca so `var p Point`
            // (no explicit initializer) behaves like Go's zero value
            // semantics. Without this, struct fields read as garbage
            // from the stack.
            alloca_inst = codegen_create_entry_alloca(codegen, llvm_type, var_name);
            if (alloca_inst && !var_decl->values) {
                // For a `?T` local with no initializer, the Go-style zero value
                // must be nil ({is_null=1, ...}). LLVMConstNull would set the
                // is_null tag to 0, which reads as PRESENT — wrong. Route to the
                // shared null-nullable builder. Falls back to ConstNull if it
                // cannot be built (defensive; should not happen for valid ?T).
                if (var_type && var_type->kind == TYPE_NULLABLE) {
                    LLVMValueRef null_init = codegen_create_nullable_null(
                        codegen, llvm_type, var_type->data.nullable.base_type);
                    if (null_init)
                        LLVMBuildStore(codegen->builder, null_init, alloca_inst);
                    else
                        LLVMBuildStore(codegen->builder, LLVMConstNull(llvm_type), alloca_inst);
                } else {
                    LLVMBuildStore(codegen->builder, LLVMConstNull(llvm_type), alloca_inst);
                }
            }
        } else {
            // Global variable
            alloca_inst = LLVMAddGlobal(codegen->module, llvm_type, var_name);
            // Same nil-default rule as locals, but globals need a *constant*
            // initializer, so build {i1 true, zero_of_base} directly.
            if (var_type && var_type->kind == TYPE_NULLABLE) {
                Type* base_type = var_type->data.nullable.base_type;
                LLVMTypeRef base_llvm = base_type ? codegen_type_to_llvm(codegen, base_type) : NULL;
                LLVMValueRef fields[2];
                fields[0] = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), 1, 0);
                fields[1] = base_llvm ? LLVMConstNull(base_llvm)
                                      : LLVMConstNull(LLVMInt32TypeInContext(codegen->context));
                LLVMSetInitializer(alloca_inst,
                                   LLVMConstStructInContext(codegen->context, fields, 2, 0));
            } else {
                LLVMSetInitializer(alloca_inst, LLVMConstNull(llvm_type));
            }
        }

        if (!alloca_inst) {
            codegen_error(codegen, decl->pos, "Failed to create storage for variable '%s'", var_name);
            return 0;
        }
        
        // Generate initializer if present
        if (var_decl->values) {
            ValueInfo* init_value;

            // `var b ?T = nil` — intercept here so codegen_generate_null_literal
            // receives the declared ?T type and emits {is_null=1, zero_value}.
            // Without this intercept the generic nil fallback (a void* null pointer)
            // lands in the auto-wrap block below and causes an LLVM type mismatch.
            if (var_type && var_type->kind == TYPE_NULLABLE &&
                var_decl->values->type == AST_LITERAL &&
                ((LiteralNode*)var_decl->values)->literal_type == TOKEN_NIL) {
                init_value = codegen_generate_null_literal(codegen, checker, var_type);
            } else {
                init_value = codegen_generate_expression(codegen, checker, var_decl->values);
            }

            if (!init_value) {
                codegen_error(codegen, decl->pos, "Failed to generate initializer for variable '%s'", var_name);
                return 0;
            }

            // Auto-wrap a plain value into a nullable struct when the
            // declared type is TYPE_NULLABLE. `var hit ?int = 42`
            // builds a {is_null=0, value=42} aggregate.
            if (var_type && var_type->kind == TYPE_NULLABLE &&
                init_value->goo_type && init_value->goo_type->kind != TYPE_NULLABLE) {
                LLVMValueRef agg = LLVMGetUndef(llvm_type);
                LLVMValueRef tag = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), 0, 0);
                agg = LLVMBuildInsertValue(codegen->builder, agg, tag, 0, "null_tag");
                agg = LLVMBuildInsertValue(codegen->builder, agg, init_value->llvm_value, 1, "null_val");
                init_value->llvm_value = agg;
                init_value->goo_type = var_type;
            }

            // Widen a narrower integer initializer to the declared target type.
            // Intentionally NOT gated to literals: Goo's type checker permits
            // cross-width assignments (e.g. `var y int64 = x`, x an int32),
            // and SExt is the correct lowering for both literal and variable
            // initializers — gating to literals-only would miscompile a negative
            // variable initializer (the upper bits would stay zero, turning -5
            // into 4294967291 when read back as i64). Narrowing (from > to) is
            // not handled here because wider→narrower is not reached for the
            // supported integer types.
            {
                LLVMTypeRef init_ty = LLVMTypeOf(init_value->llvm_value);
                if (LLVMGetTypeKind(init_ty) == LLVMIntegerTypeKind &&
                    LLVMGetTypeKind(llvm_type) == LLVMIntegerTypeKind) {
                    unsigned from_bits = LLVMGetIntTypeWidth(init_ty);
                    unsigned to_bits   = LLVMGetIntTypeWidth(llvm_type);
                    if (from_bits < to_bits)
                        init_value->llvm_value = LLVMBuildSExt(
                            codegen->builder, init_value->llvm_value, llvm_type, "init_sext");
                }
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
        // Mirror the type-checker rule: a var with an explicit declared
        // type is zero-initialized at codegen alloca time, so it counts
        // as initialized even without an explicit `= …` initializer.
        value_info->is_initialized = (var_decl->values != NULL) || (var_decl->type != NULL);

        if (!codegen_add_value(codegen, value_info)) {
            codegen_error(codegen, decl->pos, "Failed to add variable '%s' to symbol table", var_name);
            value_info_free(value_info);
            return 0;
        }

        // Mirror to the type-checker scope so later expressions inside
        // the function body that re-invoke type_check_* (e.g. via
        // codegen_generate_binary_expr → type_check_binary_expr) can
        // resolve this identifier. See function_codegen.c::function_decl
        // for the broader story on why codegen and type-checker scopes
        // need to stay in sync.
        Variable* tv = variable_new(var_name, var_type, decl->pos);
        if (tv) {
            tv->is_initialized = value_info->is_initialized;
            scope_add_variable(checker->current_scope, tv);
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

    // M11-codegen-const: comptime fast path. If type_check_const_decl
    // attached a comptime-evaluated value to the Variable (see
    // include/types.h Variable.comptime_value + lesson-1778812208-594aea),
    // emit it directly as an LLVM constant. Bypasses
    // codegen_generate_expression entirely — important because that
    // path would refuse any RHS LLVM can't fold itself (call
    // expressions, etc.) at the LLVMIsConstant check below.
    //
    // Only int-typed comptime values are handled here for the MVP.
    // Float/bool/string fall through to the existing path. Comptime
    // consts whose RHS the engine couldn't evaluate
    // (var->comptime_value == NULL — e.g. fib(10) until
    // M11-engine-recursion lands) also fall through, preserving the
    // existing "must be compile-time constant" error message rather
    // than silently miscompiling.
    if (const_decl->is_comptime && const_decl->name_count > 0) {
        Variable* probe = type_checker_lookup_variable(checker, const_decl->names[0]);
        if (probe && probe->comptime_value
                  && probe->comptime_value->type == COMPTIME_VALUE_INT) {
            for (size_t i = 0; i < const_decl->name_count; i++) {
                const char* const_name = const_decl->names[i];
                Variable* var = type_checker_lookup_variable(checker, const_name);
                if (!var || !var->comptime_value
                         || var->comptime_value->type != COMPTIME_VALUE_INT) {
                    // Defensive: multi-name comptime const where some
                    // names lack an attached value. Shouldn't happen
                    // given type_check_const_decl's copy-per-name
                    // pattern, but bail to existing path rather than
                    // crash.
                    goto fallback;
                }
                LLVMTypeRef llvm_type = codegen_type_to_llvm(codegen, var->type);
                if (!llvm_type) {
                    codegen_error(codegen, decl->pos,
                                  "Failed to convert type for comptime constant '%s'",
                                  const_name);
                    return 0;
                }
                LLVMValueRef llvm_const = LLVMConstInt(
                    llvm_type,
                    (unsigned long long)var->comptime_value->int_value,
                    1 /* sign-extend */);
                LLVMValueRef global_const = LLVMAddGlobal(codegen->module, llvm_type, const_name);
                LLVMSetInitializer(global_const, llvm_const);
                LLVMSetGlobalConstant(global_const, 1);

                ValueInfo* value_info = value_info_new(const_name, global_const, var->type);
                if (!value_info) {
                    codegen_error(codegen, decl->pos,
                                  "Failed to create value info for comptime constant '%s'",
                                  const_name);
                    return 0;
                }
                value_info->is_lvalue = 0;
                value_info->is_initialized = 1;
                if (!codegen_add_value(codegen, value_info)) {
                    codegen_error(codegen, decl->pos,
                                  "Failed to add comptime constant '%s' to symbol table",
                                  const_name);
                    value_info_free(value_info);
                    return 0;
                }
            }
            return 1;
        }
    }
fallback:;
    // ^ empty statement after label — C99/CompCert require a label to
    // precede a statement, not a declaration. The "ValueInfo*" line
    // below is a declaration, so without this `;` ccomp rejects the
    // file (clang/C23 allow label-before-decl, ccomp does not).

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

