#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Call-expression lowering: user function calls, the make_chan
// builtin, stdlib package calls (os.Exit, math.Sqrt, ...), and the
// type-dispatched variadic fmt.Println / fmt.Print family.
// Split from expression_codegen.c (refactor, no behavior change).

static ValueInfo* codegen_generate_stdlib_call(CodeGenerator* codegen, TypeChecker* checker,
                                               ASTNode* expr, const char* runtime_symbol,
                                               TypeKind return_kind, int unused_extra);

ValueInfo* codegen_generate_call_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;
    
    CallExprNode* call = (CallExprNode*)expr;
    
    // Check for built-in functions first
    if (call->function->type == AST_IDENTIFIER) {
        IdentifierNode* func_name = (IdentifierNode*)call->function;
        if (strcmp(func_name->name, "make_chan") == 0) {
            return codegen_generate_make_chan_call(codegen, checker, expr);
        }
        if (strcmp(func_name->name, "new") == 0) {
            // new(T) -> heap-allocated *T. The type checker stored the result
            // type (*T) on the node; allocate sizeof(T) via goo_alloc and
            // return the raw pointer typed as *T. The buffer is zeroed by
            // goo_alloc and never freed (the prototype's allocate-and-leak
            // model, consistent with slice literals).
            Type* ptr_type = expr->node_type;
            if (!ptr_type || ptr_type->kind != TYPE_POINTER) {
                codegen_error(codegen, expr->pos, "new: missing resolved pointer type");
                return NULL;
            }
            LLVMValueRef alloc_fn = LLVMGetNamedFunction(codegen->module, "goo_alloc");
            if (!alloc_fn) {
                codegen_error(codegen, expr->pos, "new: goo_alloc unavailable");
                return NULL;
            }
            LLVMTypeRef elem_llvm = codegen_type_to_llvm(codegen, ptr_type->data.pointer.pointee_type);
            LLVMValueRef size = LLVMSizeOf(elem_llvm);
            LLVMValueRef p = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(alloc_fn),
                                            alloc_fn, &size, 1, "new");
            return value_info_new(NULL, p, ptr_type);
        }
        if (strcmp(func_name->name, "println") == 0) {
            return codegen_generate_println_call(codegen, checker, expr);
        }
        if (strcmp(func_name->name, "len") == 0 && call->args) {
            // len(arg) — extract field 1 (the length) from a slice or
            // string struct. Both share the `{ ptr, i64 }` layout, so
            // a single InsertValue path covers them. Array len could
            // be constant-folded; deferred until needed.
            ValueInfo* arg = codegen_generate_expression(codegen, checker, call->args);
            if (!arg) return NULL;
            LLVMValueRef raw = arg->llvm_value;
            if (arg->is_lvalue && arg->goo_type) {
                LLVMTypeRef lt = codegen_type_to_llvm(codegen, arg->goo_type);
                if (lt) raw = LLVMBuildLoad2(codegen->builder, lt, raw, "len_load");
            }
            LLVMValueRef len64 = LLVMBuildExtractValue(codegen->builder, raw, 1, "len");
            LLVMValueRef len32 = LLVMBuildTrunc(codegen->builder, len64,
                                                LLVMInt32TypeInContext(codegen->context), "len_i32");
            value_info_free(arg);
            return value_info_new(NULL, len32, type_checker_get_builtin(checker, TYPE_INT32));
        }
        if (strcmp(func_name->name, "print") == 0) {
            return codegen_generate_print_call(codegen, checker, expr);
        }
    }

    // Stdlib package calls. The type checker resolves these against a
    // hardcoded symbol table (see stdlib_package_lookup); codegen routes
    // each known call to its runtime backing. This is the deliberate
    // shortcut for M7-stdlib-expansion: no multi-file compilation yet,
    // so package method calls become direct runtime intrinsic emits.
    if (call->function->type == AST_SELECTOR_EXPR) {
        SelectorExprNode* sel = (SelectorExprNode*)call->function;
        if (sel->expr && sel->expr->type == AST_IDENTIFIER) {
            IdentifierNode* pkg = (IdentifierNode*)sel->expr;
            if (strcmp(pkg->name, "fmt") == 0 && strcmp(sel->selector, "Println") == 0) {
                // fmt.Println(arg) ≡ println(arg) for now (single-arg subset).
                return codegen_generate_println_call(codegen, checker, expr);
            }
            if (strcmp(pkg->name, "os") == 0 && strcmp(sel->selector, "Exit") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_exit", TYPE_VOID, 0);
            }
            if (strcmp(pkg->name, "os") == 0 && strcmp(sel->selector, "Getenv") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_os_getenv", TYPE_STRING, 0);
            }
            if (strcmp(pkg->name, "os") == 0 && strcmp(sel->selector, "WriteFile") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_sys_write_file", TYPE_INT32, 0);
            }
            if (strcmp(pkg->name, "os") == 0 && strcmp(sel->selector, "ReadByte") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_sys_read_byte", TYPE_INT32, 0);
            }
            if (strcmp(pkg->name, "os") == 0 && strcmp(sel->selector, "FileSize") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_sys_file_size", TYPE_INT32, 0);
            }
            if (strcmp(pkg->name, "math") == 0 && strcmp(sel->selector, "Sqrt") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_math_sqrt", TYPE_FLOAT64, 0);
            }
            if (strcmp(pkg->name, "math") == 0 && strcmp(sel->selector, "Pow") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_math_pow", TYPE_FLOAT64, 0);
            }
            if (strcmp(pkg->name, "math") == 0 && strcmp(sel->selector, "Abs") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_math_abs", TYPE_FLOAT64, 0);
            }
            if (strcmp(pkg->name, "math") == 0 && strcmp(sel->selector, "Min") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_math_min", TYPE_FLOAT64, 0);
            }
            if (strcmp(pkg->name, "math") == 0 && strcmp(sel->selector, "Max") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_math_max", TYPE_FLOAT64, 0);
            }
            if (strcmp(pkg->name, "strings") == 0 && strcmp(sel->selector, "Contains") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_strings_contains", TYPE_BOOL, 1);
            }
            if (strcmp(pkg->name, "strings") == 0 && strcmp(sel->selector, "ToUpper") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_strings_to_upper", TYPE_STRING, 0);
            }
            if (strcmp(pkg->name, "strings") == 0 && strcmp(sel->selector, "ToLower") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_strings_to_lower", TYPE_STRING, 0);
            }
            if (strcmp(pkg->name, "strings") == 0 && strcmp(sel->selector, "TrimSpace") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_strings_trim_space", TYPE_STRING, 0);
            }
            if (strcmp(pkg->name, "strings") == 0 && strcmp(sel->selector, "Split") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_strings_split", TYPE_SLICE, 0);
            }
            if (strcmp(pkg->name, "strings") == 0 && strcmp(sel->selector, "Join") == 0) {
                return codegen_generate_stdlib_call(codegen, checker, expr,
                                                    "goo_strings_join", TYPE_STRING, 0);
            }
        }
    }
    
    // Method call: `recv.method(args)` where recv is a (pointer-to-)struct
    // value. Lowered to a direct call to the mangled function "T__method"
    // with the receiver prepended as the first argument. Non-method
    // selectors (unmatched packages) leave fn NULL and fall through to the
    // generic path below.
    if (call->function->type == AST_SELECTOR_EXPR) {
        SelectorExprNode* msel = (SelectorExprNode*)call->function;
        Type* recv_type = type_check_expression(checker, msel->expr);
        const char* tn = type_receiver_name(recv_type);
        char* mangled = tn ? type_method_mangled_name(tn, msel->selector) : NULL;
        LLVMValueRef fn = mangled ? LLVMGetNamedFunction(codegen->module, mangled) : NULL;
        if (fn) {
            // Receiver: codegen_generate_expression already loads lvalues, so
            // recv->llvm_value is the struct value (matches arg handling).
            ValueInfo* recv = codegen_generate_expression(codegen, checker, msel->expr);
            if (!recv) { free(mangled); return NULL; }
            size_t margc = 1;
            for (ASTNode* a = call->args; a; a = a->next) margc++;
            LLVMValueRef* margs = malloc(sizeof(LLVMValueRef) * margc);
            if (!margs) { value_info_free(recv); free(mangled); return NULL; }
            margs[0] = recv->llvm_value;
            value_info_free(recv);
            int ok = 1;
            size_t i = 1;
            for (ASTNode* a = call->args; a; a = a->next, i++) {
                ValueInfo* av = codegen_generate_expression(codegen, checker, a);
                if (!av) { ok = 0; break; }
                margs[i] = av->llvm_value;
                value_info_free(av);
            }
            if (!ok) { free(margs); free(mangled); return NULL; }
            LLVMValueRef result = LLVMBuildCall2(codegen->builder,
                LLVMGlobalGetValueType(fn), fn, margs, (unsigned)margc, "");
            free(margs);
            free(mangled);
            return value_info_new(NULL, result, type_check_call_expr(checker, expr));
        }
        free(mangled);
    }

    // Generate function expression
    ValueInfo* func_val = codegen_generate_expression(codegen, checker, call->function);
    if (!func_val) return NULL;
    
    // Generate arguments
    size_t arg_count = 0;
    ASTNode* arg = call->args;
    while (arg) {
        arg_count++;
        arg = arg->next;
    }
    
    LLVMValueRef* args = NULL;
    if (arg_count > 0) {
        args = malloc(sizeof(LLVMValueRef) * arg_count);
        if (!args) {
            value_info_free(func_val);
            return NULL;
        }
        
        arg = call->args;
        for (size_t i = 0; i < arg_count; i++) {
            ValueInfo* arg_val = codegen_generate_expression(codegen, checker, arg);
            if (!arg_val) {
                for (size_t j = 0; j < i; j++) {
                    // Note: We should free the ValueInfo structures too, but we don't have them here
                }
                free(args);
                value_info_free(func_val);
                return NULL;
            }
            args[i] = arg_val->llvm_value;
            value_info_free(arg_val);
            arg = arg->next;
        }
    }
    
    // Generate call. LLVMGetElementType doesn't work with LLVM 22 opaque
    // pointers — use LLVMGlobalGetValueType which returns the underlying
    // function type directly for any global value (functions are globals).
    LLVMValueRef result = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(func_val->llvm_value), func_val->llvm_value, args, (unsigned)arg_count, "call");


    free(args);
    
    // Get return type
    Type* return_type = type_check_call_expr(checker, expr);
    value_info_free(func_val);
    
    if (!return_type) {
        return NULL;
    }
    
    return value_info_new(NULL, result, return_type);
#endif
}

ValueInfo* codegen_generate_make_chan_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if LLVM_AVAILABLE
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;
    
    CallExprNode* call = (CallExprNode*)expr;
    
    // Count arguments: make_chan(type, buffer_size)
    size_t arg_count = 0;
    ASTNode* arg = call->args;
    while (arg) {
        arg_count++;
        arg = arg->next;
    }
    
    if (arg_count < 1 || arg_count > 2) {
        codegen_error(codegen, expr->pos, "make_chan requires 1 or 2 arguments: make_chan(type) or make_chan(type, buffer_size)");
        return NULL;
    }
    
    // For now, assume element size of int (4 bytes) - this should be determined from the type argument
    LLVMValueRef elem_size = LLVMConstInt(LLVMInt64Type(), sizeof(int), 0);
    
    // Get buffer size (default to 0 for unbuffered channel)
    LLVMValueRef buffer_size = LLVMConstInt(LLVMInt64Type(), 0, 0);
    if (arg_count == 2) {
        // Generate the buffer size argument
        ASTNode* size_arg = call->args->next;  // Second argument
        ValueInfo* size_val = codegen_generate_expression(codegen, checker, size_arg);
        if (!size_val) return NULL;
        
        // Convert to size_t if needed
        buffer_size = size_val->llvm_value;
        if (LLVMTypeOf(buffer_size) != LLVMInt64Type()) {
            buffer_size = LLVMBuildZExt(codegen->builder, buffer_size, LLVMInt64Type(), "buffer_size_ext");
        }
        value_info_free(size_val);
    }
    
    // Get the goo_make_chan function
    LLVMTypeRef param_types[] = {
        LLVMInt64Type(),  // size_t elem_size
        LLVMInt64Type()   // size_t buffer_size
    };
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8Type(), 0);
    LLVMTypeRef make_chan_func_type = LLVMFunctionType(void_ptr_type, param_types, 2, 0);
    
    LLVMValueRef make_chan_func = LLVMGetNamedFunction(codegen->module, "goo_make_chan");
    if (!make_chan_func) {
        // Declare goo_make_chan if not already declared
        make_chan_func = LLVMAddFunction(codegen->module, "goo_make_chan", make_chan_func_type);
    }
    
    // Call goo_make_chan(elem_size, buffer_size)
    LLVMValueRef args[] = { elem_size, buffer_size };
    LLVMValueRef channel = LLVMBuildCall2(codegen->builder, make_chan_func_type, make_chan_func, args, 2, "new_channel");
    
    // Create result value info
    ValueInfo* result_info = malloc(sizeof(ValueInfo));
    result_info->name = NULL;
    result_info->llvm_value = channel;
    result_info->goo_type = NULL;  // Should be a channel type
    result_info->is_lvalue = 0;
    result_info->is_moved = 0;
    result_info->is_initialized = 1;
    
    return result_info;
#else
    codegen_error(codegen, expr->pos, "Channel creation requires LLVM");
    return NULL;
#endif
}

// Built-in function implementations
// codegen_generate_stdlib_call lowers a stdlib package call (e.g.
// os.Exit, math.Sqrt) to a direct LLVM call against a pre-declared
// runtime function. unused_extra exists so future stub flags fit
// into the signature without a refactor; pass 0 for now.
//
// Behaviour notes:
//   - TYPE_STRING arguments have their pointer field extracted before
//     being passed (matches goo_strings_contains / goo_println, etc.).
//   - The return-type kind drives the LLVM result. Callers that want a
//     non-primitive return (e.g. a string-builder) need their own path.
static ValueInfo* codegen_generate_stdlib_call(CodeGenerator* codegen, TypeChecker* checker,
                                               ASTNode* expr, const char* runtime_symbol,
                                               TypeKind return_kind, int unused_extra) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    (void)unused_extra;
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;

    CallExprNode* call = (CallExprNode*)expr;
    LLVMValueRef func = LLVMGetNamedFunction(codegen->module, runtime_symbol);
    if (!func) {
        codegen_error(codegen, expr->pos, "%s not found in module", runtime_symbol);
        return NULL;
    }

    // Count + materialise args
    size_t arg_count = 0;
    for (ASTNode* a = call->args; a; a = a->next) arg_count++;

    // Declared param types drive int→double coercion below: stdlib args
    // are unchecked by the type checker (type_function(NULL, 0, ret)),
    // so `math.Pow(2, 10)` would otherwise emit verifier-invalid IR.
    LLVMTypeRef fn_type = LLVMGlobalGetValueType(func);
    unsigned param_count = LLVMCountParamTypes(fn_type);
    LLVMTypeRef param_types[8] = {0};
    if (param_count > 8) param_count = 8;
    if (param_count) LLVMGetParamTypes(fn_type, param_types);

    LLVMValueRef* args = arg_count ? malloc(sizeof(LLVMValueRef) * arg_count) : NULL;
    ASTNode* a = call->args;
    for (size_t i = 0; i < arg_count; i++, a = a->next) {
        ValueInfo* v = codegen_generate_expression(codegen, checker, a);
        if (!v) {
            free(args);
            codegen_error(codegen, expr->pos, "Failed to generate arg %zu for %s", i, runtime_symbol);
            return NULL;
        }
        // Selector/index args arrive as lvalues (field address) — load
        // the value before use, same as the Println arg loop does.
        if (v->is_lvalue && v->goo_type) {
            LLVMTypeRef at = codegen_type_to_llvm(codegen, v->goo_type);
            if (at) {
                v->llvm_value = LLVMBuildLoad2(codegen->builder, at, v->llvm_value, "argval");
                v->is_lvalue = 0;
            }
        }
        LLVMValueRef val = v->llvm_value;
        if (v->goo_type && v->goo_type->kind == TYPE_STRING) {
            val = LLVMBuildExtractValue(codegen->builder, val, 0, "str_ptr");
        }
        if (i < param_count &&
            LLVMGetTypeKind(param_types[i]) == LLVMDoubleTypeKind &&
            LLVMGetTypeKind(LLVMTypeOf(val)) == LLVMIntegerTypeKind) {
            val = LLVMBuildSIToFP(codegen->builder, val, param_types[i], "sitofp");
        }
        args[i] = val;
        value_info_free(v);
    }

    LLVMValueRef result = LLVMBuildCall2(codegen->builder,
                                         LLVMGlobalGetValueType(func),
                                         func, args, (unsigned)arg_count,
                                         return_kind == TYPE_VOID ? "" : "stdlib_ret");
    free(args);

    // Non-builtin returns (e.g. []string from strings.Split) can't be
    // expressed as a TypeKind — carry the type checker's resolved call
    // type instead. Builtin returns keep the explicit kind so codegen
    // doesn't depend on the checker having run a specific path.
    Type* ret_type = (return_kind == TYPE_SLICE && expr->node_type)
        ? expr->node_type
        : type_checker_get_builtin(checker, return_kind);
    return value_info_new(NULL, result, ret_type);
#endif
}

ValueInfo* codegen_generate_println_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;

    CallExprNode* call = (CallExprNode*)expr;

    // Get the goo_println function from the module
    LLVMValueRef println_func = LLVMGetNamedFunction(codegen->module, "goo_println");
    if (!println_func) {
        codegen_error(codegen, expr->pos, "goo_println function not found in module");
        return NULL;
    }
    
    // Variadic Println (M10-variadic-println): walk the args list,
    // emit one print-no-newline call per arg, a single-space separator
    // between adjacent args, and a single trailing newline. Empty arg
    // list still emits one newline so `fmt.Println()` prints a blank
    // line — matches Go's behavior. Each arg is type-dispatched the
    // same way M9-fmt-println-int did the single-arg case; the
    // *_println_* runtime fns are replaced here with their no-newline
    // siblings (goo_print_int/bool/float/string).
    size_t arg_count = 0;
    for (ASTNode* a = call->args; a; a = a->next) arg_count++;

    LLVMValueRef print_func = LLVMGetNamedFunction(codegen->module, "goo_print");
    if (!print_func) {
        codegen_error(codegen, expr->pos, "goo_print function not found in module");
        return NULL;
    }

    size_t idx = 0;
    for (ASTNode* a = call->args; a; a = a->next, idx++) {
        ValueInfo* arg_val = codegen_generate_expression(codegen, checker, a);
        if (!arg_val) {
            codegen_error(codegen, expr->pos, "Failed to generate argument for Println");
            return NULL;
        }

        // Selector/index args arrive as lvalues (field address) — load
        // the value before width-dispatching, same as binary_expr does.
        if (arg_val->is_lvalue && arg_val->goo_type) {
            LLVMTypeRef at = codegen_type_to_llvm(codegen, arg_val->goo_type);
            if (at) {
                arg_val->llvm_value = LLVMBuildLoad2(codegen->builder, at, arg_val->llvm_value, "argval");
                arg_val->is_lvalue = 0;
            }
        }

        TypeKind kind = (arg_val->goo_type ? arg_val->goo_type->kind : TYPE_VOID);
        if (kind == TYPE_STRING) {
            LLVMValueRef ptr = LLVMBuildExtractValue(codegen->builder, arg_val->llvm_value, 0, "str_ptr");
            LLVMValueRef args[] = { ptr };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(print_func),
                          print_func, args, 1, "");
        } else if (kind == TYPE_INT8 || kind == TYPE_INT16 || kind == TYPE_INT32 || kind == TYPE_INT64) {
            LLVMValueRef int_fn = LLVMGetNamedFunction(codegen->module, "goo_print_int");
            LLVMValueRef widened = LLVMBuildSExt(codegen->builder, arg_val->llvm_value,
                                                 LLVMInt64TypeInContext(codegen->context), "sext");
            LLVMValueRef args[] = { widened };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(int_fn),
                          int_fn, args, 1, "");
        } else if (kind == TYPE_BOOL) {
            LLVMValueRef bool_fn = LLVMGetNamedFunction(codegen->module, "goo_print_bool");
            LLVMValueRef widened = LLVMBuildZExt(codegen->builder, arg_val->llvm_value,
                                                 LLVMInt32TypeInContext(codegen->context), "zext");
            LLVMValueRef args[] = { widened };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(bool_fn),
                          bool_fn, args, 1, "");
        } else if (kind == TYPE_FLOAT32 || kind == TYPE_FLOAT64) {
            LLVMValueRef float_fn = LLVMGetNamedFunction(codegen->module, "goo_print_float");
            LLVMValueRef widened = (kind == TYPE_FLOAT32)
                ? LLVMBuildFPExt(codegen->builder, arg_val->llvm_value,
                                 LLVMDoubleTypeInContext(codegen->context), "fpext")
                : arg_val->llvm_value;
            LLVMValueRef args[] = { widened };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(float_fn),
                          float_fn, args, 1, "");
        } else {
            // Fallback: pass through to goo_print. Will surface at the
            // verifier if the type's wrong rather than crashing here.
            LLVMValueRef args[] = { arg_val->llvm_value };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(print_func),
                          print_func, args, 1, "");
        }
        value_info_free(arg_val);

        // Space separator between adjacent args (matches Go's fmt.Println).
        if (idx + 1 < arg_count) {
            LLVMValueRef space = LLVMBuildGlobalStringPtr(codegen->builder, " ", "ps");
            LLVMValueRef args[] = { space };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(print_func),
                          print_func, args, 1, "");
        }
    }

    // Trailing newline. Single goo_println("") call covers both the
    // 0-arg case (just a blank line) and the post-args newline.
    {
        LLVMValueRef empty_str = LLVMBuildGlobalStringPtr(codegen->builder, "", "empty");
        LLVMValueRef args[] = { empty_str };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(println_func),
                      println_func, args, 1, "");
    }
    
    // Return void value
    ValueInfo* result = malloc(sizeof(ValueInfo));
    result->name = NULL;
    result->llvm_value = NULL;
    result->goo_type = type_checker_get_builtin(checker, TYPE_VOID);
    result->is_lvalue = 0;
    result->is_moved = 0;
    result->is_initialized = 1;
    
    return result;
#endif
}

ValueInfo* codegen_generate_print_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;
    
    CallExprNode* call = (CallExprNode*)expr;
    
    // Get the goo_print function from the module
    LLVMValueRef print_func = LLVMGetNamedFunction(codegen->module, "goo_print");
    if (!print_func) {
        codegen_error(codegen, expr->pos, "goo_print function not found in module");
        return NULL;
    }
    
    // Handle arguments similar to println
    if (!call->args) {
        // print() with no arguments - do nothing
        LLVMValueRef empty_str = LLVMConstString("", 0, 0);
        LLVMValueRef args[] = { empty_str };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(print_func), 
                      print_func, args, 1, "");
    } else {
        ValueInfo* arg_val = codegen_generate_expression(codegen, checker, call->args);
        if (!arg_val) {
            codegen_error(codegen, expr->pos, "Failed to generate argument for print");
            return NULL;
        }
        
        LLVMValueRef args[] = { arg_val->llvm_value };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(print_func), 
                      print_func, args, 1, "");
        
        value_info_free(arg_val);
    }
    
    // Return void value
    ValueInfo* result = malloc(sizeof(ValueInfo));
    result->name = NULL;
    result->llvm_value = NULL;
    result->goo_type = type_checker_get_builtin(checker, TYPE_VOID);
    result->is_lvalue = 0;
    result->is_moved = 0;
    result->is_initialized = 1;
    
    return result;
#endif
}
