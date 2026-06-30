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

// Defined in expression_codegen.c: storage address of an addressable
// expression (no load). Used here for pointer-receiver auto-address-of (P2-3).
ValueInfo* codegen_emit_lvalue_address(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

#if LLVM_AVAILABLE
// Materialize a call argument as a C string pointer (char*): evaluate it,
// load through any lvalue, then extract the data pointer (field 0) of a
// goo_string value. Used by the strings.* lowerings whose runtime symbols
// take `const char*`. Returns NULL on failure.
static LLVMValueRef codegen_arg_as_cstr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* arg) {
    ValueInfo* v = codegen_generate_expression(codegen, checker, arg);
    if (!v) return NULL;
    LLVMValueRef val = v->llvm_value;
    if (v->is_lvalue && v->goo_type) {
        LLVMTypeRef at = codegen_type_to_llvm(codegen, v->goo_type);
        if (at) val = LLVMBuildLoad2(codegen->builder, at, val, "argval");
    }
    if (v->goo_type && v->goo_type->kind == TYPE_STRING) {
        val = LLVMBuildExtractValue(codegen->builder, val, 0, "str_ptr");
    }
    value_info_free(v);
    return val;
}

// Builtin numeric type-conversion name (F2): mirrors
// builtin_conversion_target() in the type checker — the names that produce an
// actual value conversion. This set is intentionally numeric-only: the checker
// recognizes `string`/`bool` as conversion names too (full set in
// name_is_builtin_conv_name) but REJECTS them as unsupported in v1, so a
// `string(x)`/`bool(x)` program fails type-check and never reaches codegen.
// Codegen therefore only ever needs the names it can actually lower.
static int is_builtin_conv_name(const char* name) {
    if (!name) return 0;
    static const char* kinds[] = {
        "int", "int8", "int16", "int32", "int64",
        "uint", "uint8", "uint16", "uint32", "uint64",
        "byte", "float32", "float64",
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        if (strcmp(name, kinds[i]) == 0) return 1;
    }
    return 0;
}

// Emit a numeric value conversion `T(x)` (F2). Picks the LLVM cast from the
// source/target LLVM kinds and the SOURCE signedness (so int(byte(200)) is
// 200, not -56): widen with SExt for signed sources, ZExt for unsigned;
// narrow with Trunc; int<->float via {S,U}IToFP / FPTo{S,U}I; float<->float
// via FPExt/FPTrunc. `from`/`to` are the Goo types; `to_l` the target LLVM type.
static LLVMValueRef codegen_numeric_convert(CodeGenerator* codegen, LLVMValueRef v,
                                            Type* from, Type* to, LLVMTypeRef to_l) {
    LLVMTypeRef from_l = LLVMTypeOf(v);
    if (from_l == to_l) return v;
    LLVMTypeKind fk = LLVMGetTypeKind(from_l), tk = LLVMGetTypeKind(to_l);
    int from_signed = type_is_signed(from);
    int to_signed = type_is_signed(to);

    if (fk == LLVMIntegerTypeKind && tk == LLVMIntegerTypeKind) {
        unsigned fb = LLVMGetIntTypeWidth(from_l), tb = LLVMGetIntTypeWidth(to_l);
        if (fb < tb) {
            return from_signed
                ? LLVMBuildSExt(codegen->builder, v, to_l, "conv_sext")
                : LLVMBuildZExt(codegen->builder, v, to_l, "conv_zext");
        }
        if (fb > tb) return LLVMBuildTrunc(codegen->builder, v, to_l, "conv_trunc");
        return v;  // same width: signedness is not represented in LLVM ints
    }
    if (fk == LLVMIntegerTypeKind && (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind)) {
        return from_signed
            ? LLVMBuildSIToFP(codegen->builder, v, to_l, "conv_sitofp")
            : LLVMBuildUIToFP(codegen->builder, v, to_l, "conv_uitofp");
    }
    if ((fk == LLVMFloatTypeKind || fk == LLVMDoubleTypeKind) && tk == LLVMIntegerTypeKind) {
        return to_signed
            ? LLVMBuildFPToSI(codegen->builder, v, to_l, "conv_fptosi")
            : LLVMBuildFPToUI(codegen->builder, v, to_l, "conv_fptoui");
    }
    if ((fk == LLVMFloatTypeKind || fk == LLVMDoubleTypeKind) &&
        (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind)) {
        if (fk == LLVMFloatTypeKind && tk == LLVMDoubleTypeKind)
            return LLVMBuildFPExt(codegen->builder, v, to_l, "conv_fpext");
        if (fk == LLVMDoubleTypeKind && tk == LLVMFloatTypeKind)
            return LLVMBuildFPTrunc(codegen->builder, v, to_l, "conv_fptrunc");
        return v;
    }
    return v;
}
#endif

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
        // Builtin numeric type conversion `T(x)` (F2). The type checker has
        // already validated the single numeric/char argument and stamped the
        // target type on expr->node_type. Evaluate the operand and emit the
        // appropriate cast.
        //
        // Mirror the checker's shadowing gate: Go permits a user symbol to
        // shadow a predeclared type name, so `func int(n int) int` makes
        // `int(5)` a CALL, not a conversion. The name alone is not enough to
        // decide — consult the symbol table. At codegen time every top-level
        // function lives in the (persisted) global scope regardless of source
        // order, so a non-NULL lookup means the name is shadowed: fall through
        // to the ordinary call path instead of silently converting. Without
        // this, `int(5)` printed 5 (conversion) rather than 105 (the user's
        // function) — a silent miscompile of code that is legal in Go.
        if (is_builtin_conv_name(func_name->name) && call->args && !call->args->next
            && !type_checker_lookup_variable(checker, func_name->name)) {
            Type* target = expr->node_type;
            if (!target) {
                codegen_error(codegen, expr->pos, "conversion: missing resolved target type");
                return NULL;
            }
            ValueInfo* src = codegen_generate_expression(codegen, checker, call->args);
            if (!src) return NULL;
            LLVMValueRef sval = src->llvm_value;
            if (src->is_lvalue && src->goo_type) {
                LLVMTypeRef st = codegen_type_to_llvm(codegen, src->goo_type);
                if (st) sval = LLVMBuildLoad2(codegen->builder, st, sval, "conv_load");
            }
            LLVMTypeRef to_l = codegen_type_to_llvm(codegen, target);
            if (!to_l) { value_info_free(src); return NULL; }
            LLVMValueRef out = codegen_numeric_convert(codegen, sval, src->goo_type, target, to_l);
            value_info_free(src);
            return value_info_new(NULL, out, target);
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
        if (strcmp(func_name->name, "cap") == 0 && call->args) {
            // cap(slice) — extract field 2 (capacity) from the 3-field slice
            // header. Mirrors len() but reads field 2 instead of field 1.
            ValueInfo* arg = codegen_generate_expression(codegen, checker, call->args);
            if (!arg) return NULL;
            LLVMValueRef raw = arg->llvm_value;
            if (arg->is_lvalue && arg->goo_type) {
                LLVMTypeRef lt = codegen_type_to_llvm(codegen, arg->goo_type);
                if (lt) raw = LLVMBuildLoad2(codegen->builder, lt, raw, "cap_load");
            }
            LLVMValueRef cap64 = LLVMBuildExtractValue(codegen->builder, raw, 2, "cap");
            LLVMValueRef cap32 = LLVMBuildTrunc(codegen->builder, cap64,
                                                LLVMInt32TypeInContext(codegen->context), "cap_i32");
            value_info_free(arg);
            return value_info_new(NULL, cap32, type_checker_get_builtin(checker, TYPE_INT32));
        }
        if (strcmp(func_name->name, "append") == 0 && call->args && call->args->next) {
            // append(slice, elem) -> slice. goo_slice_append grows the slice
            // in place (amortized 2x) and rewrites {data,len,cap}. It takes
            // the slice and element BY POINTER, so we spill both to slots,
            // call, then load and return the (possibly moved) header. Go value
            // semantics: the caller stores it back via `s = append(s, x)`.
            Type* slice_t = expr->node_type;  // resolved by the type checker
            if (!slice_t || slice_t->kind != TYPE_SLICE) {
                codegen_error(codegen, expr->pos, "append: missing resolved slice type");
                return NULL;
            }
            LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_slice_append");
            if (!fn) { codegen_error(codegen, expr->pos, "goo_slice_append not found in module"); return NULL; }

            ValueInfo* sv = codegen_generate_expression(codegen, checker, call->args);
            if (!sv) return NULL;
            LLVMTypeRef slice_llvm = codegen_type_to_llvm(codegen, slice_t);
            LLVMValueRef slice_val = sv->llvm_value;
            if (sv->is_lvalue) slice_val = LLVMBuildLoad2(codegen->builder, slice_llvm, slice_val, "append_slice");
            value_info_free(sv);

            ValueInfo* ev = codegen_generate_expression(codegen, checker, call->args->next);
            if (!ev) return NULL;
            LLVMTypeRef elem_llvm = codegen_type_to_llvm(codegen, slice_t->data.slice.element_type);
            LLVMValueRef elem_val = ev->llvm_value;
            if (ev->is_lvalue) {
                LLVMTypeRef et = ev->goo_type ? codegen_type_to_llvm(codegen, ev->goo_type) : elem_llvm;
                elem_val = LLVMBuildLoad2(codegen->builder, et, elem_val, "append_elem");
            }
            value_info_free(ev);

            LLVMValueRef slice_slot = codegen_create_entry_alloca(codegen, slice_llvm, "append_slice_slot");
            LLVMBuildStore(codegen->builder, slice_val, slice_slot);
            LLVMValueRef elem_slot = codegen_create_entry_alloca(codegen, elem_llvm, "append_elem_slot");
            LLVMBuildStore(codegen->builder, elem_val, elem_slot);

            LLVMValueRef elem_size = LLVMSizeOf(elem_llvm);
            LLVMValueRef args[] = { slice_slot, elem_slot, elem_size };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn), fn, args, 3, "");

            LLVMValueRef result = LLVMBuildLoad2(codegen->builder, slice_llvm, slice_slot, "append_result");
            return value_info_new(NULL, result, slice_t);
        }
        if (strcmp(func_name->name, "print") == 0) {
            return codegen_generate_print_call(codegen, checker, expr);
        }
        // error("msg") -> !T error union (error case).
        // The resolved !T is in expr->node_type (set by the type checker).
        // Fall back to the current function's return type if needed.
        if (strcmp(func_name->name, "error") == 0 && call->args && !call->args->next) {
            Type* result_type = expr->node_type;
            if (!result_type || !type_is_error_union(result_type)) {
                result_type = codegen->current_function_info
                              ? codegen->current_function_info->goo_type : NULL;
            }
            if (!result_type || !type_is_error_union(result_type)) {
                codegen_error(codegen, expr->pos,
                              "error(): no !T context available (not inside !T function)");
                return NULL;
            }
            LLVMTypeRef union_llvm = codegen_type_to_llvm(codegen, result_type);
            if (!union_llvm) return NULL;

            // Evaluate the message argument — produces a goo_string_t {i8*, i64}.
            ValueInfo* msg_vi = codegen_generate_expression(codegen, checker, call->args);
            if (!msg_vi) return NULL;
            LLVMValueRef msg_val = msg_vi->llvm_value;
            if (msg_vi->is_lvalue && msg_vi->goo_type) {
                LLVMTypeRef mt = codegen_type_to_llvm(codegen, msg_vi->goo_type);
                if (mt) msg_val = LLVMBuildLoad2(codegen->builder, mt, msg_val, "err_msg_load");
            }
            value_info_free(msg_vi);

            // Build the error case. codegen_create_error_union_error inserts
            // msg_val into field 1 of the data union, which is goo_string_t
            // (the default error type after the type_mapping.c change).
            LLVMValueRef error_val = codegen_create_error_union_error(codegen, union_llvm, msg_val);
            if (!error_val) return NULL;
            return value_info_new(NULL, error_val, result_type);
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
                // void goo_strings_split(goo_slice_t* out, const char* s, const char* sep)
                // The []string result returns through an out-pointer: a 3-field
                // slice can't cross the C ABI by value from hand-emitted IR.
                // Spill a result slot, call, then load the populated slice.
                Type* ret_type = expr->node_type;
                if (!ret_type || ret_type->kind != TYPE_SLICE || !call->args || !call->args->next) {
                    codegen_error(codegen, expr->pos, "strings.Split: expected (string, string) -> []string");
                    return NULL;
                }
                LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_strings_split");
                if (!fn) { codegen_error(codegen, expr->pos, "goo_strings_split not found in module"); return NULL; }
                LLVMValueRef s_ptr = codegen_arg_as_cstr(codegen, checker, call->args);
                LLVMValueRef sep_ptr = codegen_arg_as_cstr(codegen, checker, call->args->next);
                if (!s_ptr || !sep_ptr) return NULL;
                LLVMTypeRef slice_llvm = codegen_type_to_llvm(codegen, ret_type);
                LLVMValueRef out = codegen_create_entry_alloca(codegen, slice_llvm, "split_out");
                LLVMValueRef args[] = { out, s_ptr, sep_ptr };
                LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn), fn, args, 3, "");
                LLVMValueRef slice_val = LLVMBuildLoad2(codegen->builder, slice_llvm, out, "split_slice");
                return value_info_new(NULL, slice_val, ret_type);
            }
            if (strcmp(pkg->name, "strings") == 0 && strcmp(sel->selector, "Join") == 0) {
                // goo_string_t goo_strings_join(const goo_slice_t* parts, const char* sep)
                // Spill the []string value to a slot and pass its address — a
                // 3-field slice can't cross the C ABI by value from hand-emitted IR.
                if (!call->args || !call->args->next) {
                    codegen_error(codegen, expr->pos, "strings.Join: expected ([]string, string) -> string");
                    return NULL;
                }
                LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_strings_join");
                if (!fn) { codegen_error(codegen, expr->pos, "goo_strings_join not found in module"); return NULL; }
                ValueInfo* parts = codegen_generate_expression(codegen, checker, call->args);
                if (!parts) return NULL;
                LLVMValueRef parts_val = parts->llvm_value;
                LLVMTypeRef slice_llvm = parts->goo_type ? codegen_type_to_llvm(codegen, parts->goo_type) : NULL;
                if (!slice_llvm) { value_info_free(parts); codegen_error(codegen, expr->pos, "strings.Join: bad []string arg"); return NULL; }
                if (parts->is_lvalue) parts_val = LLVMBuildLoad2(codegen->builder, slice_llvm, parts_val, "parts_val");
                LLVMValueRef slot = codegen_create_entry_alloca(codegen, slice_llvm, "join_parts");
                LLVMBuildStore(codegen->builder, parts_val, slot);
                value_info_free(parts);
                LLVMValueRef sep_ptr = codegen_arg_as_cstr(codegen, checker, call->args->next);
                if (!sep_ptr) return NULL;
                LLVMValueRef args[] = { slot, sep_ptr };
                LLVMValueRef result = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn), fn, args, 2, "join");
                return value_info_new(NULL, result, type_checker_get_builtin(checker, TYPE_STRING));
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
            // Receiver kind comes from the method's registered type: a pointer
            // receiver (`func (c *T) m()`) takes params[0] as a pointer. For an
            // addressable value call `c.m()`, Go (and Goo as a Go superset)
            // auto-takes the address `(&c).m()` — pass the lvalue's storage
            // address, not a loaded struct value (which would mismatch the
            // signature and fail LLVM verification). When the receiver
            // expression is already a pointer, pass it through unchanged.
            Variable* mvar = type_checker_lookup_variable(checker, mangled);
            Type* recv_param = (mvar && mvar->type && mvar->type->kind == TYPE_FUNCTION
                                && mvar->type->data.function.param_count > 0)
                ? mvar->type->data.function.param_types[0] : NULL;
            int ptr_recv = recv_param && recv_param->kind == TYPE_POINTER;
            int recv_is_ptr_value = recv_type && recv_type->kind == TYPE_POINTER;

            // Invariant: a registered method always carries its receiver as
            // params[0] (the parser splices it in; type_check_function_decl
            // records it). If we can't see it, the receiver kind is unknown
            // and any guess (value vs pointer) risks passing a mismatched
            // argument into the call — which the LLVM verifier rejects with a
            // crash and no diagnostic. Fail cleanly on the broken invariant
            // instead of degrading to a verifier abort.
            if (!recv_param) {
                codegen_error(codegen, expr->pos,
                    "internal: cannot resolve receiver type for method '%s'",
                    msel->selector);
                free(mangled);
                return NULL;
            }

            LLVMValueRef recv_arg = NULL;
            if (ptr_recv && !recv_is_ptr_value) {
                // Auto-address-of: the receiver must be an addressable lvalue.
                ValueInfo* addr = codegen_emit_lvalue_address(codegen, checker, msel->expr);
                if (!addr || !addr->is_lvalue) {
                    codegen_error(codegen, expr->pos,
                        "cannot call pointer-receiver method '%s' on non-addressable value",
                        msel->selector);
                    free(mangled);
                    return NULL;
                }
                // Identifier lvalues alias the value table; do not free addr
                // (mirrors the `&x` address-of path in expression_codegen.c).
                recv_arg = addr->llvm_value;
            } else if (!ptr_recv && recv_is_ptr_value) {
                // Auto-deref: a value-receiver method called on a pointer
                // value `p.m()` is `(*p).m()` in Go (and Goo). Load the
                // pointed-to struct and pass it by value to match the
                // value-receiver signature. Without this, the raw pointer
                // would be handed to a value param and fail LLVM verification.
                ValueInfo* pv = codegen_generate_expression(codegen, checker, msel->expr);
                if (!pv) { free(mangled); return NULL; }
                LLVMValueRef ptr = pv->llvm_value;
                value_info_free(pv);
                LLVMTypeRef val_llvm = codegen_type_to_llvm(codegen, recv_param);
                if (!val_llvm) {
                    codegen_error(codegen, expr->pos,
                        "cannot lower receiver type for method '%s'",
                        msel->selector);
                    free(mangled);
                    return NULL;
                }
                recv_arg = LLVMBuildLoad2(codegen->builder, val_llvm, ptr, "recv_deref");
            } else {
                // Value receiver on a value, or pointer receiver on a pointer:
                // codegen_generate_expression loads lvalues to a value, which
                // already matches the param type in both cases.
                ValueInfo* recv = codegen_generate_expression(codegen, checker, msel->expr);
                if (!recv) { free(mangled); return NULL; }
                recv_arg = recv->llvm_value;
                value_info_free(recv);
            }
            size_t margc = 1;
            for (ASTNode* a = call->args; a; a = a->next) margc++;
            LLVMValueRef* margs = malloc(sizeof(LLVMValueRef) * margc);
            if (!margs) { free(mangled); return NULL; }
            margs[0] = recv_arg;
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
        
        // The callee's declared parameter types drive nullable auto-wrapping:
        // a bare `T` or the nil literal passed to a `?T` parameter must be
        // lowered to the {i1,T} nullable struct, not stored raw.
        Type* func_goo_type = func_val->goo_type;

        arg = call->args;
        for (size_t i = 0; i < arg_count; i++) {
            Type* param_type = NULL;
            if (func_goo_type && func_goo_type->kind == TYPE_FUNCTION &&
                i < func_goo_type->data.function.param_count) {
                param_type = func_goo_type->data.function.param_types[i];
            }

            if (param_type && param_type->kind == TYPE_NULLABLE &&
                arg->type == AST_LITERAL &&
                ((LiteralNode*)arg)->literal_type == TOKEN_NIL) {
                // nil literal → build the param's null-nullable directly.
                ValueInfo* nil_val = codegen_generate_null_literal(codegen, checker, param_type);
                if (!nil_val) {
                    free(args);
                    value_info_free(func_val);
                    return NULL;
                }
                args[i] = nil_val->llvm_value;
                value_info_free(nil_val);
                arg = arg->next;
                continue;
            }

            ValueInfo* arg_val = codegen_generate_expression(codegen, checker, arg);
            if (!arg_val) {
                free(args);
                value_info_free(func_val);
                return NULL;
            }

            // Auto-wrap a bare value into the param's nullable type. If the
            // argument is already nullable (e.g. passing a `?int` variable),
            // no wrapping is needed.
            if (param_type && param_type->kind == TYPE_NULLABLE &&
                arg_val->goo_type && arg_val->goo_type->kind != TYPE_NULLABLE) {
                if (arg_val->is_lvalue && arg_val->goo_type) {
                    LLVMTypeRef at = codegen_type_to_llvm(codegen, arg_val->goo_type);
                    if (at) {
                        arg_val->llvm_value = LLVMBuildLoad2(codegen->builder, at, arg_val->llvm_value, "argld");
                        arg_val->is_lvalue = 0;
                    }
                }
                LLVMTypeRef nullable_llvm = codegen_type_to_llvm(codegen, param_type);
                if (nullable_llvm) {
                    arg_val->llvm_value = codegen_create_nullable_with_value(
                        codegen, nullable_llvm, arg_val->llvm_value, arg_val->goo_type);
                    arg_val->goo_type = param_type;
                }
            }

            args[i] = arg_val->llvm_value;
            value_info_free(arg_val);
            arg = arg->next;
        }
    }
    
    // Generate call. LLVMGetElementType doesn't work with LLVM 22 opaque
    // pointers — use LLVMGlobalGetValueType which returns the underlying
    // function type directly for any global value (functions are globals).
    // Use an empty result name for void functions (invalid to name a void value).
    LLVMTypeRef func_llvm_type = LLVMGlobalGetValueType(func_val->llvm_value);
    const char* result_name = (LLVMGetTypeKind(LLVMGetReturnType(func_llvm_type)) == LLVMVoidTypeKind)
                              ? "" : "call";
    LLVMValueRef result = LLVMBuildCall2(codegen->builder, func_llvm_type, func_val->llvm_value, args, (unsigned)arg_count, result_name);


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
    
    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);

    // Derive elem_size from the channel's element type (Task 2).
    // We must use the LLVM ABI size (with alignment padding) rather than
    // Type.size (a sequential field sum that ignores padding). For a struct
    // like {int8, int64} Goo computes Type.size=9 while the LLVM ABI size
    // is 16 (7 bytes of padding follow the int8 field so int64 is aligned).
    // Using Type.size would make goo_make_chan allocate 9-byte ring slots,
    // causing goo_chan_send/recv to memcpy only 9 of the 16 bytes and silently
    // truncate the int64 field.
    //
    // target_machine is NULL here — it is only set inside codegen_emit_to_file,
    // which runs after all expression lowering. The previous fallback via
    // codegen->target_machine was therefore permanently dead code.
    //
    // Instead: the module's target triple is set by codegen_initialize_target
    // before any expression lowering, so we can derive the correct ABI layout
    // by spinning up a temporary TargetMachine from that triple.
    size_t elem_bytes = sizeof(int);
    if (expr->node_type && expr->node_type->kind == TYPE_CHANNEL &&
        expr->node_type->data.channel.element_type) {
        Type* elem_t = expr->node_type->data.channel.element_type;
        LLVMTypeRef llvm_elem = codegen_type_to_llvm(codegen, elem_t);
        int got_abi_size = 0;
        if (llvm_elem) {
            // Build a throw-away TargetMachine purely to get the ABI data
            // layout for this platform.  The module triple is always non-empty
            // at this point (codegen_initialize_target guarantees it).
            const char* mod_triple = LLVMGetTarget(codegen->module);
            if (mod_triple && *mod_triple) {
                LLVMTargetRef tgt = NULL;
                char* err = NULL;
                if (!LLVMGetTargetFromTriple(mod_triple, &tgt, &err)) {
                    LLVMTargetMachineRef tmp_tm = LLVMCreateTargetMachine(
                        tgt, mod_triple, "generic", "",
                        LLVMCodeGenLevelNone, LLVMRelocDefault,
                        LLVMCodeModelDefault);
                    if (tmp_tm) {
                        LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tmp_tm);
                        if (td) {
                            uint64_t abi_sz = LLVMABISizeOfType(td, llvm_elem);
                            if (abi_sz > 0) {
                                elem_bytes = (size_t)abi_sz;
                                got_abi_size = 1;
                            }
                            LLVMDisposeTargetData(td);
                        }
                        LLVMDisposeTargetMachine(tmp_tm);
                    }
                } else {
                    if (err) LLVMDisposeMessage(err);
                }
            }
        }
        if (!got_abi_size && elem_t->size > 0) {
            // LLVM path unavailable (no registered target for the triple).
            // Type.size is correct for primitives and aligned structs; it
            // under-counts padded structs, but there is no better option here.
            elem_bytes = elem_t->size;
        }
    }
    LLVMValueRef elem_size = LLVMConstInt(i64, elem_bytes, 0);

    // Get buffer size (default to 0 for unbuffered channel)
    LLVMValueRef buffer_size = LLVMConstInt(i64, 0, 0);
    if (arg_count == 2) {
        // Generate the buffer size argument
        ASTNode* size_arg = call->args->next;  // Second argument
        ValueInfo* size_val = codegen_generate_expression(codegen, checker, size_arg);
        if (!size_val) return NULL;

        // Convert to size_t if needed
        buffer_size = size_val->llvm_value;
        if (LLVMTypeOf(buffer_size) != i64) {
            buffer_size = LLVMBuildZExt(codegen->builder, buffer_size, i64, "buffer_size_ext");
        }
        value_info_free(size_val);
    }

    // Get the goo_make_chan function — build all types in codegen->context
    LLVMTypeRef param_types[] = { i64, i64 };
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
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
    result_info->goo_type = expr->node_type;  // resolved TYPE_CHANNEL from type checker (Task 2 uses this)
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
            // Pass the whole goo_string struct to the length-aware printer.
            // Extracting just the data ptr and calling goo_print (strlen) is
            // wrong for a substring (F5): a shared-buffer slice like
            // "hello"[1:3] has no '\0' after "el", so strlen would read past
            // the logical length. goo_print_string honours the length field.
            LLVMValueRef str_fn = LLVMGetNamedFunction(codegen->module, "goo_print_string");
            if (!str_fn) {
                codegen_error(codegen, a->pos, "goo_print_string not found in module");
                value_info_free(arg_val);
                return NULL;
            }
            LLVMValueRef args[] = { arg_val->llvm_value };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(str_fn),
                          str_fn, args, 1, "");
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
            // P0-3: an unsupported argument type is a clean source-located
            // codegen error, not a type-mismatched goo_print call that only
            // surfaces (as invalid IR) at the LLVM verifier.
            codegen_error(codegen, a->pos,
                          "fmt.Println: unsupported argument type (only string, integer, "
                          "bool, and float are supported in v1)");
            value_info_free(arg_val);
            return NULL;
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
