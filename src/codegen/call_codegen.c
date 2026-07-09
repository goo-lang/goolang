#include "codegen.h"
#include "embedding.h"
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
static ValueInfo* codegen_generate_printf_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
static ValueInfo* codegen_generate_sprintf_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
static ValueInfo* codegen_generate_fmt_sprint_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
static ValueInfo* codegen_generate_fmt_sprintln_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
static ValueInfo* codegen_generate_errorf_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);
static ValueInfo* codegen_generate_atoi_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr);

#if LLVM_AVAILABLE
// Given a loaded `error` value {i1 is_null, i8* handle}, produce the goo_string
// to display: "<nil>" when null, else goo_error_message(handle). Shared by
// fmt.Println's error case and the %v verb (defined below, near fmt_emit_segments).
static LLVMValueRef codegen_error_display_string(CodeGenerator* codegen, LLVMValueRef err_loaded, Position pos);

// fmt.Println value formatter (struct/pointer-to-struct formatting task):
// recursive per-value dispatch, extracted from the Println arg loop so
// TYPE_STRUCT and pointer-to-struct can recurse into it. See its definition
// (near codegen_generate_println_call) for the full contract.
static int codegen_emit_fmt_value(CodeGenerator* codegen, TypeChecker* checker,
                                   LLVMValueRef val, Type* ty, int depth, Position pos);

// fmt.Sprintf %v value formatter: the string-building counterpart to
// codegen_emit_fmt_value above. Same recursive dispatch/shape (struct,
// pointer-to-struct, depth cap) but CONCATENATES into a goo_string
// accumulator (via goo_string_concat) instead of calling goo_print. See its
// definition (right after codegen_emit_fmt_value) for the full contract.
static LLVMValueRef codegen_build_fmt_value_string(CodeGenerator* codegen, TypeChecker* checker,
                                                    LLVMValueRef acc, LLVMValueRef val,
                                                    Type* ty, int depth, Position pos);
#endif

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

// stdlib Phase 0 (Task 5): lower a call into a source-compiled package, e.g.
// `mypkg.Double(21)`. A package's exported functions are emitted into this
// module under the mangled symbol goo_pkg__<pkg>__<name> (see
// codegen_package_symbol_name). If that symbol exists this is a real package
// call: evaluate the args and emit a direct call. If it does NOT exist we are
// not a source-package selector — `*handled` stays 0 so the caller falls
// through to the hardcoded stdlib shim arms (fmt/os/math/...), which remain the
// per-symbol FALLBACK. `*handled` becomes 1 once we own the call (whether the
// emit succeeds or errors, in which case NULL is returned).
static ValueInfo* codegen_generate_pkg_selector_call(CodeGenerator* codegen,
                                                     TypeChecker* checker,
                                                     ASTNode* expr,
                                                     const char* pkg_name,
                                                     const char* sel_name,
                                                     int* handled) {
    *handled = 0;
    size_t n = strlen("goo_pkg__") + strlen(pkg_name) + strlen("__")
             + strlen(sel_name) + 1;
    char* sym = malloc(n);
    if (!sym) return NULL;
    snprintf(sym, n, "goo_pkg__%s__%s", pkg_name, sel_name);
    LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, sym);
    free(sym);
    if (!fn) return NULL;  // not a real package symbol → shim fallback

    *handled = 1;
    CallExprNode* call = (CallExprNode*)expr;
    size_t argc = 0;
    for (ASTNode* a = call->args; a; a = a->next) argc++;
    LLVMValueRef* args = argc ? malloc(sizeof(LLVMValueRef) * argc) : NULL;
    if (argc && !args) return NULL;
    size_t i = 0;
    for (ASTNode* a = call->args; a; a = a->next, i++) {
        // codegen_generate_expression loads scalar lvalues to a value, so the
        // raw llvm_value already matches the callee's parameter type (same
        // pattern as the struct-method call path below).
        ValueInfo* av = codegen_generate_expression(codegen, checker, a);
        if (!av) { free(args); return NULL; }
        args[i] = av->llvm_value;
        value_info_free(av);
    }
    LLVMValueRef result = LLVMBuildCall2(codegen->builder,
        LLVMGlobalGetValueType(fn), fn, args, (unsigned)argc, "");
    free(args);
    return value_info_new(NULL, result, type_check_call_expr(checker, expr));
}

// Builtin numeric type-conversion name (F2): mirrors
// builtin_conversion_target() in the type checker — the names that produce an
// actual value conversion. This set is intentionally numeric-only: the checker
// recognizes `string`/`bool` as conversion names too (full set in
// name_is_builtin_conv_name) but REJECTS them as unsupported in v1, so a
// `string(x)`/`bool(x)` program fails type-check and never reaches codegen.
// Codegen therefore only ever needs the names it can actually lower.
// Change-together: this set is mirrored in function_codegen.c's
// global_init_is_conversion_call (static there too, so duplicated rather than
// shared) — update both when the kind list changes.
static int is_builtin_conv_name(const char* name) {
    if (!name) return 0;
    static const char* kinds[] = {
        "int", "int8", "int16", "int32", "int64",
        "uint", "uint8", "uint16", "uint32", "uint64",
        "byte", "rune", "float32", "float64",
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

// Resolve a call/go-statement's CALLEE expression. A bare identifier NOT
// shadowed by a local variable/parameter (i.e. it names a package-level
// function) resolves to the BARE LLVM global function value — the
// direct-call fast path, UNCONDITIONALLY UNCHANGED from before the universal
// fat-pointer migration (see docs/superpowers/specs/2026-07-03-closures-
// design.md "Representation": "Direct calls to named functions are
// untouched — still bare `call @F(args...)`"). Any other callee expression
// (a local variable/parameter holding a function value, a struct field, a
// slice element, or a chained call's return value) evaluates through the
// ordinary expression path and yields the universal `{ fn_ptr, env_ptr }`
// pair — callers must extract and indirect-call it (see
// codegen_generate_call_expr's direct-vs-indirect gate below).
//
// Used by BOTH codegen_generate_call_expr (this file) and
// codegen_generate_go_stmt (statement_codegen.c). The go-statement use is
// what keeps `go f(...)`'s arg-boxing path from EVER seeing a function
// VALUE: a go target is, by construction, only ever a bare unshadowed
// identifier (the go statement's own LLVMIsAFunction check already rejects
// anything else), so routing its callee resolution through here — instead
// of calling codegen_generate_expression directly, which would hit
// codegen_generate_identifier's thunk-wrapping fallback for ANY bare
// function name — is required, not optional, for that non-negotiable to
// hold. (statement_codegen.c is outside this task's file allowlist; the
// one-line call-site swap there is flagged in the task report.)
ValueInfo* codegen_resolve_callee(CodeGenerator* codegen, TypeChecker* checker, ASTNode* callee_expr) {
#if !LLVM_AVAILABLE
    (void)checker;
    codegen_error(codegen, callee_expr ? callee_expr->pos : (Position){0, 0, 0, NULL},
                  "LLVM support not available");
    return NULL;
#else
    if (callee_expr && callee_expr->type == AST_IDENTIFIER) {
        IdentifierNode* id = (IdentifierNode*)callee_expr;
        if (!codegen_lookup_value(codegen, id->name)) {
            LLVMValueRef func_val = codegen_lookup_global_function(codegen, checker, id->name);
            if (func_val) {
                Variable* func_var = type_checker_lookup_variable(checker, id->name);
                Type* func_type = func_var ? func_var->type : NULL;
                return value_info_new(id->name, func_val, func_type);
            }
        }
    }
    return codegen_generate_expression(codegen, checker, callee_expr);
#endif
}

#if LLVM_AVAILABLE
// Task 3: a nil function value (`var f func(int) int`; never assigned)
// zero-values to the fat pointer { NULL, NULL } — calling it jumps to a
// NULL instruction pointer (raw SIGSEGV, no message). Go panics cleanly
// ("invalid memory address or nil pointer dereference"); mirror that with
// the SAME runtime-abort mechanism the divzero/bounds checks use
// (codegen_emit_divzero_check, expression_codegen.c: icmp against null,
// conditional branch to a panic block that calls goo_panic(message) and
// terminates with `unreachable`, else fall through). Fires ONLY on this
// INDIRECT call site — a direct named-function call (LLVMIsAFunction
// branch above) can never be nil, so it stays unconditional and unchanged.
static void codegen_emit_funcnil_check(CodeGenerator* codegen, LLVMValueRef fn_ptr,
                                        ASTNode* expr) {
    (void)expr;
    LLVMValueRef panic_fn = LLVMGetNamedFunction(codegen->module, "goo_panic");
    if (!panic_fn) return;  // no panic symbol: emit the call unguarded

    LLVMValueRef null_ptr = LLVMConstNull(LLVMTypeOf(fn_ptr));
    LLVMValueRef isnil = LLVMBuildICmp(codegen->builder, LLVMIntEQ, fn_ptr, null_ptr, "funcnil");
    LLVMBasicBlockRef panic_bb = codegen_create_block(codegen, "funcnil.panic");
    LLVMBasicBlockRef cont_bb = codegen_create_block(codegen, "funcnil.cont");
    LLVMBuildCondBr(codegen->builder, isnil, panic_bb, cont_bb);

    codegen_set_insert_point(codegen, panic_bb);
    LLVMValueRef msg = LLVMBuildGlobalStringPtr(codegen->builder,
                                                "call of nil function", "funcnil_msg");
    LLVMValueRef args[1] = { msg };
    LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(panic_fn), panic_fn, args, 1, "");
    LLVMBuildUnreachable(codegen->builder);

    codegen_set_insert_point(codegen, cont_bb);
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
        // string(x) where x is ANY integer kind (Task 2, port unblocker). The
        // checker has already validated x (any integer kind, including
        // TYPE_CHAR) and stamped TYPE_STRING on expr->node_type — see the
        // "string(x)" arm in expression_checker.c, placed ahead of ITS
        // generic numeric-conversion gate for the same reason this arm is
        // placed ahead of the generic numeric-conversion codegen below.
        // Unlike a numeric conversion, the target width isn't the operand's
        // final representation: the runtime helper goo_string_from_rune
        // does the real work (UTF-8-encodes the value as a Unicode code
        // point), so this lowering only widens/narrows the operand to the
        // i32 that helper expects (codegen_numeric_convert, reused from the
        // numeric-conversion path below) and calls it. Scope: rune/byte->
        // string ONLY — []byte(s)/string([]byte) are out of scope and never
        // reach here (the checker rejects a non-integer/non-char source).
        //
        // Mirror the checker's shadowing gate (see the numeric-conversion
        // comment below for the rationale): a user `func string(...)` makes
        // `string(x)` an ordinary call, not a conversion.
        if (strcmp(func_name->name, "string") == 0 && call->args && !call->args->next
            && !type_checker_lookup_variable(checker, func_name->name)) {
            ValueInfo* src = codegen_generate_expression(codegen, checker, call->args);
            if (!src) return NULL;
            LLVMValueRef sval = src->llvm_value;
            if (src->is_lvalue && src->goo_type) {
                LLVMTypeRef st = codegen_type_to_llvm(codegen, src->goo_type);
                if (st) sval = LLVMBuildLoad2(codegen->builder, st, sval, "conv_load");
            }

            // string(b) where b is []byte (Task 2, stdlib unblocker): the
            // checker has already confirmed b's element kind is the byte
            // kind (see the checker's "string" arm, expression_checker.c),
            // ahead of the rune/int path below — a slice source never
            // reaches that path. Go copies on conversion: goo_cstr_from_bytes
            // allocates+memcpy's a fresh NUL-terminated buffer rather than
            // aliasing the slice's backing store.
            if (src->goo_type && src->goo_type->kind == TYPE_SLICE) {
                LLVMValueRef data_ptr = LLVMBuildExtractValue(codegen->builder, sval, 0, "strconv_sl_data");
                LLVMValueRef data_len = LLVMBuildExtractValue(codegen->builder, sval, 1, "strconv_sl_len");
                value_info_free(src);

                // goo_cstr_from_bytes is not registered by
                // runtime_integration.c (out of this task's file allowlist)
                // — declare it lazily here on first use, the same
                // lazy-fallback pattern this arm already uses below for
                // goo_string_from_rune.
                LLVMValueRef to_cstr_fn = LLVMGetNamedFunction(codegen->module, "goo_cstr_from_bytes");
                if (!to_cstr_fn) {
                    LLVMTypeRef i8_ptr = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
                    LLVMTypeRef i64_l = LLVMInt64TypeInContext(codegen->context);
                    // data is void* at the C ABI level; i8* is bit-compatible.
                    LLVMTypeRef params[] = { i8_ptr, i64_l };
                    LLVMTypeRef fn_type = LLVMFunctionType(i8_ptr, params, 2, 0);
                    to_cstr_fn = LLVMAddFunction(codegen->module, "goo_cstr_from_bytes", fn_type);
                }
                LLVMValueRef cstr_args[] = { data_ptr, data_len };
                LLVMValueRef cstr = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(to_cstr_fn),
                                                   to_cstr_fn, cstr_args, 2, "strconv_cstr");

                LLVMTypeRef string_l = LLVMStructTypeInContext(codegen->context,
                    (LLVMTypeRef[]){
                        LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),
                        LLVMInt64TypeInContext(codegen->context)
                    }, 2, 0);
                LLVMValueRef sresult = LLVMGetUndef(string_l);
                sresult = LLVMBuildInsertValue(codegen->builder, sresult, cstr, 0, "strconv_str_data");
                sresult = LLVMBuildInsertValue(codegen->builder, sresult, data_len, 1, "strconv_str_len");
                return value_info_new(NULL, sresult, type_checker_get_builtin(checker, TYPE_STRING));
            }

            Type* i32_type = type_checker_get_builtin(checker, TYPE_INT32);
            LLVMTypeRef i32_l = LLVMInt32TypeInContext(codegen->context);
            LLVMValueRef rune_val = codegen_numeric_convert(codegen, sval, src->goo_type, i32_type, i32_l);
            value_info_free(src);

            // goo_string_from_rune is not registered by runtime_integration.c
            // (out of this task's file allowlist) — declare it lazily here
            // on first use, the same lazy-fallback pattern statement_codegen.c
            // uses for goo_string_new in the map-range lowering (its
            // goo_string_new comment explains why: avoids widening the
            // central runtime_integration.c table for a single call site).
            // The struct layout {i8*, i64} mirrors runtime_integration.c's
            // string_type exactly (goo_string_t: {char* data; size_t length}).
            LLVMValueRef from_rune_fn = LLVMGetNamedFunction(codegen->module, "goo_string_from_rune");
            if (!from_rune_fn) {
                LLVMTypeRef string_l = LLVMStructTypeInContext(codegen->context,
                    (LLVMTypeRef[]){
                        LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),
                        LLVMInt64TypeInContext(codegen->context)
                    }, 2, 0);
                LLVMTypeRef params[] = { i32_l };
                LLVMTypeRef fn_type = LLVMFunctionType(string_l, params, 1, 0);
                from_rune_fn = LLVMAddFunction(codegen->module, "goo_string_from_rune", fn_type);
            }
            LLVMValueRef args[] = { rune_val };
            LLVMValueRef result = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(from_rune_fn),
                                                 from_rune_fn, args, 1, "str_from_rune");
            return value_info_new(NULL, result, type_checker_get_builtin(checker, TYPE_STRING));
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
            LLVMTypeRef elem_llvm = codegen_type_to_llvm(codegen, ptr_type->data.pointer.pointee_type);
            LLVMValueRef size = LLVMSizeOf(elem_llvm);
            LLVMValueRef p = codegen_emit_alloc(codegen, size, ALLOC_KIND_DEFAULT, expr);
            return value_info_new(NULL, p, ptr_type);
        }
        if (strcmp(func_name->name, "make") == 0) {
            // make(map[K]V[, hint]) -> GooMapSV* (map-literal codegen's
            // path, minus the key/value inserts: src/codegen/
            // expression_codegen.c's AST_PAREN_EXPR/MapLitNode case).
            // make([]T, n[, cap]) -> {ptr,len,cap} slice header built in IR
            // over a goo_slice_alloc'd zeroed backing store (calloc = Go
            // zero values), same aggregate shape as the slice-literal path.
            Type* made_type = expr->node_type;
            if (made_type && made_type->kind == TYPE_SLICE) {
                LLVMTypeRef elem_llvm = codegen_type_to_llvm(
                    codegen, made_type->data.slice.element_type);
                if (!elem_llvm) {
                    codegen_error(codegen, expr->pos, "make: cannot lower slice element type");
                    return NULL;
                }
                // Length (2nd arg; the type checker guarantees it exists
                // and is an integer). Load if lvalue, then widen to i64
                // with signedness-correct extension.
                ValueInfo* lv = codegen_generate_expression(codegen, checker, call->args->next);
                if (!lv) return NULL;
                if (lv->is_lvalue && lv->goo_type) {
                    LLVMTypeRef lt = codegen_type_to_llvm(codegen, lv->goo_type);
                    if (lt) {
                        lv->llvm_value = LLVMBuildLoad2(codegen->builder, lt,
                                                        lv->llvm_value, "make_len_load");
                        lv->is_lvalue = 0;
                    }
                }
                LLVMValueRef len64 = codegen_widen_index(codegen, lv);
                value_info_free(lv);
                // Capacity (optional 3rd arg) defaults to the length.
                LLVMValueRef cap64 = len64;
                if (call->args->next->next) {
                    ValueInfo* cv = codegen_generate_expression(codegen, checker,
                                                                call->args->next->next);
                    if (!cv) return NULL;
                    if (cv->is_lvalue && cv->goo_type) {
                        LLVMTypeRef ct = codegen_type_to_llvm(codegen, cv->goo_type);
                        if (ct) {
                            cv->llvm_value = LLVMBuildLoad2(codegen->builder, ct,
                                                            cv->llvm_value, "make_cap_load");
                            cv->is_lvalue = 0;
                        }
                    }
                    cap64 = codegen_widen_index(codegen, cv);
                    value_info_free(cv);
                }
                LLVMValueRef alloc_fn = LLVMGetNamedFunction(codegen->module, "goo_slice_alloc");
                if (!alloc_fn) {
                    codegen_error(codegen, expr->pos, "make: goo_slice_alloc unavailable");
                    return NULL;
                }
                // Allocate CAP elements (not len) so writes within capacity
                // after a future reslice stay in bounds. No len>cap runtime
                // check yet — noted follow-up.
                LLVMValueRef elem_size = LLVMSizeOf(elem_llvm);
                LLVMValueRef alloc_args[2] = { cap64, elem_size };
                LLVMValueRef data = LLVMBuildCall2(codegen->builder,
                                                   LLVMGlobalGetValueType(alloc_fn),
                                                   alloc_fn, alloc_args, 2, "make_slice_data");
                LLVMTypeRef slice_llvm = codegen_type_to_llvm(codegen, made_type);
                LLVMValueRef slice_val = LLVMGetUndef(slice_llvm);
                slice_val = LLVMBuildInsertValue(codegen->builder, slice_val, data, 0, "slice_ptr");
                slice_val = LLVMBuildInsertValue(codegen->builder, slice_val, len64, 1, "slice_len");
                slice_val = LLVMBuildInsertValue(codegen->builder, slice_val, cap64, 2, "slice_cap");
                return value_info_new(NULL, slice_val, made_type);
            }
            if (made_type && made_type->kind == TYPE_CHANNEL) {
                // make(chan T[, capacity]) -> buffered/unbuffered channel.
                // codegen_generate_make_chan_call reads expr->node_type
                // (the resolved TYPE_CHANNEL, already stamped above) for
                // the element size/ABI layout and call->args->next for the
                // optional capacity expression — it never inspects
                // call->args[0] itself, so it is agnostic to whether that
                // node is a bare type-name identifier (make_chan(T, n)) or
                // a chan_type AST node (make(chan T, n)). Reused as-is
                // rather than duplicating the ABI-size/capacity plumbing.
                return codegen_generate_make_chan_call(codegen, checker, expr);
            }
            if (!made_type || made_type->kind != TYPE_MAP) {
                codegen_error(codegen, expr->pos, "make: missing resolved map or slice type");
                return NULL;
            }
            // The optional size hint is evaluated for side effects (Go
            // allows an arbitrary expression there) and discarded — the
            // list-backed map runtime has no pre-sizing to apply it to.
            if (call->args && call->args->next) {
                ValueInfo* hint = codegen_generate_expression(codegen, checker, call->args->next);
                if (!hint) return NULL;
                value_info_free(hint);
            }
            LLVMValueRef new_fn = LLVMGetNamedFunction(codegen->module, "goo_map_new_sv");
            if (!new_fn) {
                codegen_error(codegen, expr->pos, "make: goo_map_new_sv unavailable");
                return NULL;
            }
            LLVMValueRef key_kind = LLVMConstInt(LLVMInt32TypeInContext(codegen->context),
                                                 codegen_map_key_kind(made_type->data.map.key_type), 0);
            // A struct key needs the synthesized per-field comparator so the
            // runtime's key_kind==STRUCT arm can call it (goo_map_key_eq,
            // runtime.c); an interface key needs the runtime's
            // goo_iface_key_eq (Task 2, key_kind==IFACE); every other key
            // kind passes NULL (unused by the runtime for STRING/INLINE).
            LLVMValueRef keyeq_ptr;
            LLVMTypeRef keyeq_ptr_ty = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
            if (made_type->data.map.key_type && made_type->data.map.key_type->kind == TYPE_STRUCT) {
                LLVMValueRef cmp_fn = codegen_get_or_emit_struct_key_eq(
                    codegen, checker, made_type->data.map.key_type);
                if (!cmp_fn) {
                    codegen_error(codegen, expr->pos,
                                  "make: failed to synthesize struct key comparator");
                    return NULL;
                }
                keyeq_ptr = LLVMBuildBitCast(codegen->builder, cmp_fn, keyeq_ptr_ty, "keyeq_ptr");
            } else if (made_type->data.map.key_type && made_type->data.map.key_type->kind == TYPE_INTERFACE) {
                LLVMValueRef cmp_fn = LLVMGetNamedFunction(codegen->module, "goo_iface_key_eq");
                if (!cmp_fn) {
                    codegen_error(codegen, expr->pos, "make: goo_iface_key_eq unavailable");
                    return NULL;
                }
                keyeq_ptr = LLVMBuildBitCast(codegen->builder, cmp_fn, keyeq_ptr_ty, "keyeq_ptr");
            } else {
                keyeq_ptr = LLVMConstPointerNull(keyeq_ptr_ty);
            }
            LLVMValueRef new_args[] = { key_kind, keyeq_ptr };
            LLVMValueRef m = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(new_fn),
                                            new_fn, new_args, 2, "make_map");
            return value_info_new(NULL, m, made_type);
        }
        if (strcmp(func_name->name, "println") == 0) {
            return codegen_generate_println_call(codegen, checker, expr);
        }
        if (strcmp(func_name->name, "panic") == 0) {
            // panic(v) -> goo_panic(message); the runtime prints "panic: <msg>"
            // and abort()s. A string arg passes its data pointer; any other arg
            // uses a generic message (structured panic values are future work).
            // goo_panic never returns, so terminate the block with `unreachable`.
            LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_panic");
            if (!fn) {
                codegen_error(codegen, expr->pos, "goo_panic not found in module");
                return NULL;
            }
            LLVMValueRef msg = NULL;
            if (call->args) {
                ValueInfo* v = codegen_generate_expression(codegen, checker, call->args);
                if (v && v->goo_type && v->goo_type->kind == TYPE_STRING) {
                    LLVMValueRef sv = v->llvm_value;
                    if (v->is_lvalue) {
                        LLVMTypeRef st = codegen_type_to_llvm(codegen, v->goo_type);
                        if (st) sv = LLVMBuildLoad2(codegen->builder, st, sv, "panic_load");
                    }
                    msg = LLVMBuildExtractValue(codegen->builder, sv, 0, "panic_msg");
                }
                if (v) value_info_free(v);
            }
            if (!msg) {
                msg = LLVMBuildGlobalStringPtr(codegen->builder, "panic", "panic_generic");
            }
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn), fn, &msg, 1, "");
            // goo_panic abort()s at runtime, so code after it is dead — but we do
            // NOT emit `unreachable` here: the surrounding statement (if-body,
            // function epilogue) adds the block terminator, and a second
            // terminator would be invalid IR. Post-panic code is emitted and
            // never executed, same as any void builtin call.
            return value_info_new(NULL, NULL, type_checker_get_builtin(checker, TYPE_VOID));
        }
        if (strcmp(func_name->name, "len") == 0 && call->args) {
            // len(arg) — extract field 1 (the length) from a slice or
            // string struct. Both share the `{ ptr, i64 }` layout, so
            // a single InsertValue path covers them. Array len could
            // be constant-folded; deferred until needed.
            ValueInfo* arg = codegen_generate_expression(codegen, checker, call->args);
            if (!arg) return NULL;
            // Array: len is the static element count (a compile-time constant).
            // An array is a raw [N x T] value, not a {ptr,len} aggregate, so
            // there is no header field to extract — and we return before the
            // load below so a large array is never materialized just to be
            // discarded.
            if (arg->goo_type && arg->goo_type->kind == TYPE_ARRAY) {
                LLVMValueRef n = LLVMConstInt(LLVMInt64TypeInContext(codegen->context),
                                              (unsigned long long)arg->goo_type->data.array.length, 0);
                value_info_free(arg);
                return value_info_new(NULL, n, type_checker_get_builtin(checker, TYPE_INT64));
            }
            LLVMValueRef raw = arg->llvm_value;
            if (arg->is_lvalue && arg->goo_type) {
                LLVMTypeRef lt = codegen_type_to_llvm(codegen, arg->goo_type);
                if (lt) raw = LLVMBuildLoad2(codegen->builder, lt, raw, "len_load");
            }
            // TYPE_MAP lowers to an opaque i8* (GooMapSV*), not the
            // {ptr,len,cap}/{ptr,len} aggregate slices/strings share —
            // ExtractValue below would segfault on it. Route through the
            // runtime entry-count helper instead.
            if (arg->goo_type && arg->goo_type->kind == TYPE_MAP) {
                LLVMValueRef len_fn = LLVMGetNamedFunction(codegen->module, "goo_map_len_sv");
                if (!len_fn) {
                    codegen_error(codegen, expr->pos, "len: goo_map_len_sv unavailable");
                    value_info_free(arg);
                    return NULL;
                }
                LLVMValueRef map_len = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(len_fn),
                                                      len_fn, &raw, 1, "map_len");
                value_info_free(arg);
                return value_info_new(NULL, map_len, type_checker_get_builtin(checker, TYPE_INT64));
            }
            // Go: len returns `int` (i64 here) — the slice header already
            // stores the length as i64, so no truncation.
            LLVMValueRef len64 = LLVMBuildExtractValue(codegen->builder, raw, 1, "len");
            value_info_free(arg);
            return value_info_new(NULL, len64, type_checker_get_builtin(checker, TYPE_INT64));
        }
        if (strcmp(func_name->name, "delete") == 0 && call->args && call->args->next) {
            // delete(m, k) — unlink the entry for k from m via
            // goo_map_delete_sv. Map handling mirrors the len() arm above
            // (load the map pointer if it's an lvalue); the key is packed
            // into its i64 slot via codegen_map_key_to_slot, same as every
            // other map op site (m[k] = v, src/codegen/expression_codegen.c).
            ValueInfo* map_arg = codegen_generate_expression(codegen, checker, call->args);
            if (!map_arg) return NULL;
            LLVMValueRef map_raw = map_arg->llvm_value;
            if (map_arg->is_lvalue && map_arg->goo_type) {
                LLVMTypeRef mt = codegen_type_to_llvm(codegen, map_arg->goo_type);
                if (mt) map_raw = LLVMBuildLoad2(codegen->builder, mt, map_raw, "delete_map_load");
            }
            ValueInfo* key_arg = codegen_generate_expression(codegen, checker, call->args->next);
            if (!key_arg) { value_info_free(map_arg); return NULL; }
            Type* del_key_type = (map_arg->goo_type && map_arg->goo_type->kind == TYPE_MAP)
                ? map_arg->goo_type->data.map.key_type : key_arg->goo_type;
            // Box a concrete key into an interface-typed map key BEFORE
            // slot-packing (Task 2) — no-op for every non-interface-keyed map.
            if (!codegen_box_map_key_if_needed(codegen, checker, key_arg, del_key_type, expr->pos)) {
                value_info_free(map_arg);
                value_info_free(key_arg);
                return NULL;
            }
            LLVMValueRef key_ptr = codegen_map_key_to_slot(codegen, checker, key_arg, del_key_type);
            LLVMValueRef del_fn = LLVMGetNamedFunction(codegen->module, "goo_map_delete_sv");
            if (!del_fn) {
                codegen_error(codegen, expr->pos, "delete: goo_map_delete_sv unavailable");
                value_info_free(map_arg);
                value_info_free(key_arg);
                return NULL;
            }
            LLVMValueRef args[2] = { map_raw, key_ptr };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(del_fn), del_fn, args, 2, "");
            value_info_free(map_arg);
            value_info_free(key_arg);
            return value_info_new(NULL, NULL, type_checker_get_builtin(checker, TYPE_VOID));
        }
        if (strcmp(func_name->name, "cap") == 0 && call->args) {
            // cap(slice) — extract field 2 (capacity) from the 3-field slice
            // header. Mirrors len() but reads field 2 instead of field 1.
            ValueInfo* arg = codegen_generate_expression(codegen, checker, call->args);
            if (!arg) return NULL;
            // Array: cap == len == the static element count (a compile-time
            // constant); no {ptr,len,cap} header to extract. Return before the
            // load so a large array is never materialized.
            if (arg->goo_type && arg->goo_type->kind == TYPE_ARRAY) {
                LLVMValueRef n = LLVMConstInt(LLVMInt64TypeInContext(codegen->context),
                                              (unsigned long long)arg->goo_type->data.array.length, 0);
                value_info_free(arg);
                return value_info_new(NULL, n, type_checker_get_builtin(checker, TYPE_INT64));
            }
            LLVMValueRef raw = arg->llvm_value;
            if (arg->is_lvalue && arg->goo_type) {
                LLVMTypeRef lt = codegen_type_to_llvm(codegen, arg->goo_type);
                if (lt) raw = LLVMBuildLoad2(codegen->builder, lt, raw, "cap_load");
            }
            // Go: cap returns `int` (i64 here) — no truncation.
            LLVMValueRef cap64 = LLVMBuildExtractValue(codegen->builder, raw, 2, "cap");
            value_info_free(arg);
            return value_info_new(NULL, cap64, type_checker_get_builtin(checker, TYPE_INT64));
        }
        if (strcmp(func_name->name, "append") == 0 && call->args && call->args->next) {
            // append(dst, elem) -> slice, OR append(dst, s...) -> slice (Task
            // 4's bulk arm, selected by `has_spread`). Both share the same
            // dst-in-slot / possibly-regrown-header shape: dst is spilled to
            // a temp alloca so goo_slice_append{,_bulk}'s in-place amortized
            // grow (rewriting {data,len,cap}) is visible to the caller, which
            // stores the reloaded result back via `s = append(s, ...)`.
            Type* slice_t = expr->node_type;  // resolved by the type checker
            if (!slice_t || slice_t->kind != TYPE_SLICE) {
                codegen_error(codegen, expr->pos, "append: missing resolved slice type");
                return NULL;
            }
            LLVMTypeRef slice_llvm = codegen_type_to_llvm(codegen, slice_t);
            LLVMTypeRef elem_llvm = codegen_type_to_llvm(codegen, slice_t->data.slice.element_type);

            ValueInfo* sv = codegen_generate_expression(codegen, checker, call->args);
            if (!sv) return NULL;
            LLVMValueRef slice_val = sv->llvm_value;
            if (sv->is_lvalue) slice_val = LLVMBuildLoad2(codegen->builder, slice_llvm, slice_val, "append_slice");
            value_info_free(sv);

            LLVMValueRef slice_slot = codegen_create_entry_alloca(codegen, slice_llvm, "append_slice_slot");
            LLVMBuildStore(codegen->builder, slice_val, slice_slot);

            if (call->has_spread) {
                // src is []E (identical elem to dst) or, when dst's element
                // is byte, a string — extract {data,len} from whichever
                // aggregate it is, same field-0/1 convention the []byte(s)
                // conversion arm (expression_codegen.c AST_SLICE_CONVERSION)
                // uses for a string, and cap()/len() use for a slice.
                LLVMValueRef bulk_fn = LLVMGetNamedFunction(codegen->module, "goo_slice_append_bulk");
                if (!bulk_fn) {
                    codegen_error(codegen, expr->pos, "goo_slice_append_bulk not found in module");
                    return NULL;
                }
                ValueInfo* src_v = codegen_generate_expression(codegen, checker, call->args->next);
                if (!src_v) return NULL;
                Type* src_t = src_v->goo_type;
                LLVMValueRef src_raw = src_v->llvm_value;
                if (src_v->is_lvalue && src_t) {
                    LLVMTypeRef st = codegen_type_to_llvm(codegen, src_t);
                    if (st) src_raw = LLVMBuildLoad2(codegen->builder, st, src_raw, "append_spread_src");
                }
                LLVMValueRef src_data, src_len;
                if (src_t && src_t->kind == TYPE_STRING) {
                    src_data = LLVMBuildExtractValue(codegen->builder, src_raw, 0, "append_spread_str_ptr");
                    src_len  = LLVMBuildExtractValue(codegen->builder, src_raw, 1, "append_spread_str_len");
                } else {
                    src_data = LLVMBuildExtractValue(codegen->builder, src_raw, 0, "append_spread_slice_data");
                    src_len  = LLVMBuildExtractValue(codegen->builder, src_raw, 1, "append_spread_slice_len");
                }
                value_info_free(src_v);

                LLVMValueRef elem_size = LLVMSizeOf(elem_llvm);
                LLVMValueRef args[] = { slice_slot, src_data, src_len, elem_size };
                LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(bulk_fn), bulk_fn, args, 4, "");
            } else {
                LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_slice_append");
                if (!fn) { codegen_error(codegen, expr->pos, "goo_slice_append not found in module"); return NULL; }

                ValueInfo* ev = codegen_generate_expression(codegen, checker, call->args->next);
                if (!ev) return NULL;
                LLVMValueRef elem_val = ev->llvm_value;
                if (ev->is_lvalue) {
                    LLVMTypeRef et = ev->goo_type ? codegen_type_to_llvm(codegen, ev->goo_type) : elem_llvm;
                    elem_val = LLVMBuildLoad2(codegen->builder, et, elem_val, "append_elem");
                }

                // Coerce the element to the slice's element type — the slot
                // below is elem_llvm-sized and goo_slice_append copies
                // elem_llvm's size, so a mismatched-width value stored raw
                // either leaves undef upper bytes (an int8 -5 appended onto
                // []int64 read back as 251) or OOB-writes past a narrower
                // slot (a float64 appended onto []float32 stores 8 bytes into
                // a 4-byte slot). Same source-signedness rule as the var-decl,
                // constant-rebuild, and literal-element fixes, now centralized
                // in codegen_coerce_to_type.
                int append_src_signed = ev->goo_type ? type_is_signed(ev->goo_type) : 1;
                elem_val = codegen_coerce_to_type(codegen, elem_val, append_src_signed, elem_llvm);
                value_info_free(ev);

                LLVMValueRef elem_slot = codegen_create_entry_alloca(codegen, elem_llvm, "append_elem_slot");
                LLVMBuildStore(codegen->builder, elem_val, elem_slot);

                LLVMValueRef elem_size = LLVMSizeOf(elem_llvm);
                LLVMValueRef args[] = { slice_slot, elem_slot, elem_size };
                LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn), fn, args, 3, "");
            }

            LLVMValueRef result = LLVMBuildLoad2(codegen->builder, slice_llvm, slice_slot, "append_result");
            return value_info_new(NULL, result, slice_t);
        }
        if (strcmp(func_name->name, "copy") == 0 && call->args && call->args->next) {
            // copy(dst, src) -> int (Go-exact: min(len(dst), len(src))
            // elements moved via memmove — overlap-safe). dst is always a
            // slice (typecheck-enforced); src is a slice OR, when dst's
            // element is byte, a string. copy() never resizes dst, so unlike
            // append() there is no slot/pointer indirection needed — just
            // extract each side's raw {data,len} and hand them straight to
            // goo_slice_copy_raw.
            LLVMValueRef copy_fn = LLVMGetNamedFunction(codegen->module, "goo_slice_copy_raw");
            if (!copy_fn) {
                codegen_error(codegen, expr->pos, "goo_slice_copy_raw not found in module");
                return NULL;
            }

            ValueInfo* dv = codegen_generate_expression(codegen, checker, call->args);
            if (!dv) return NULL;
            Type* dst_t = dv->goo_type;
            if (!dst_t || dst_t->kind != TYPE_SLICE) {
                codegen_error(codegen, expr->pos, "copy: missing resolved destination slice type");
                value_info_free(dv);
                return NULL;
            }
            LLVMTypeRef dst_llvm = codegen_type_to_llvm(codegen, dst_t);
            LLVMValueRef dst_raw = dv->llvm_value;
            if (dv->is_lvalue) dst_raw = LLVMBuildLoad2(codegen->builder, dst_llvm, dst_raw, "copy_dst_load");
            value_info_free(dv);
            LLVMValueRef dst_data = LLVMBuildExtractValue(codegen->builder, dst_raw, 0, "copy_dst_data");
            LLVMValueRef dst_len  = LLVMBuildExtractValue(codegen->builder, dst_raw, 1, "copy_dst_len");

            ValueInfo* sv2 = codegen_generate_expression(codegen, checker, call->args->next);
            if (!sv2) return NULL;
            Type* src_t = sv2->goo_type;
            LLVMValueRef src_raw = sv2->llvm_value;
            if (sv2->is_lvalue && src_t) {
                LLVMTypeRef st = codegen_type_to_llvm(codegen, src_t);
                if (st) src_raw = LLVMBuildLoad2(codegen->builder, st, src_raw, "copy_src_load");
            }
            LLVMValueRef src_data, src_len;
            if (src_t && src_t->kind == TYPE_STRING) {
                src_data = LLVMBuildExtractValue(codegen->builder, src_raw, 0, "copy_src_str_ptr");
                src_len  = LLVMBuildExtractValue(codegen->builder, src_raw, 1, "copy_src_str_len");
            } else {
                src_data = LLVMBuildExtractValue(codegen->builder, src_raw, 0, "copy_src_slice_data");
                src_len  = LLVMBuildExtractValue(codegen->builder, src_raw, 1, "copy_src_slice_len");
            }
            value_info_free(sv2);

            LLVMTypeRef copy_elem_llvm = codegen_type_to_llvm(codegen, dst_t->data.slice.element_type);
            LLVMValueRef elem_size = LLVMSizeOf(copy_elem_llvm);
            LLVMValueRef args[] = { dst_data, dst_len, src_data, src_len, elem_size };
            LLVMValueRef n = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(copy_fn),
                                            copy_fn, args, 5, "copy_n");
            return value_info_new(NULL, n, type_checker_get_builtin(checker, TYPE_INT64));
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
            // stdlib Phase 0 (Task 5): a call into a source-compiled package
            // (exports emitted as goo_pkg__<pkg>__<name>) routes here FIRST. If
            // no such symbol exists we fall through to the hardcoded stdlib shim
            // arms below (the per-symbol FALLBACK, left untouched).
            {
                int handled = 0;
                ValueInfo* pv = codegen_generate_pkg_selector_call(
                    codegen, checker, expr, pkg->name, sel->selector, &handled);
                if (handled) return pv;
            }
            if (strcmp(pkg->name, "fmt") == 0 && strcmp(sel->selector, "Println") == 0) {
                // fmt.Println(arg) ≡ println(arg) for now (single-arg subset).
                return codegen_generate_println_call(codegen, checker, expr);
            }
            if (strcmp(pkg->name, "fmt") == 0 && strcmp(sel->selector, "Print") == 0) {
                return codegen_generate_fmt_print_call(codegen, checker, expr);
            }
            if (strcmp(pkg->name, "fmt") == 0 && strcmp(sel->selector, "Printf") == 0) {
                return codegen_generate_printf_call(codegen, checker, expr);
            }
            if (strcmp(pkg->name, "fmt") == 0 && strcmp(sel->selector, "Sprintf") == 0) {
                return codegen_generate_sprintf_call(codegen, checker, expr);
            }
            if (strcmp(pkg->name, "fmt") == 0 && strcmp(sel->selector, "Sprint") == 0) {
                return codegen_generate_fmt_sprint_call(codegen, checker, expr);
            }
            if (strcmp(pkg->name, "fmt") == 0 && strcmp(sel->selector, "Sprintln") == 0) {
                return codegen_generate_fmt_sprintln_call(codegen, checker, expr);
            }
            if (strcmp(pkg->name, "fmt") == 0 && strcmp(sel->selector, "Errorf") == 0) {
                return codegen_generate_errorf_call(codegen, checker, expr);
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
            if (strcmp(pkg->name, "strconv") == 0 && strcmp(sel->selector, "Itoa") == 0) {
                // strconv.Itoa(int) -> string via goo_int_to_string(int64_t)
                // SExt the int arg to i64 since goo_int_to_string takes int64_t.
                LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_int_to_string");
                if (!fn) { codegen_error(codegen, expr->pos, "goo_int_to_string not found"); return NULL; }
                ValueInfo* a = codegen_generate_expression(codegen, checker, call->args);
                if (!a) return NULL;
                LLVMValueRef v = LLVMBuildSExt(codegen->builder, a->llvm_value,
                                               LLVMInt64TypeInContext(codegen->context), "itoa_arg");
                LLVMValueRef args[] = { v };
                LLVMValueRef res = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn), fn, args, 1, "itoa");
                value_info_free(a);
                return value_info_new(NULL, res, type_checker_get_builtin(checker, TYPE_STRING));
            }
            if (strcmp(pkg->name, "strconv") == 0 && strcmp(sel->selector, "Atoi") == 0) {
                return codegen_generate_atoi_call(codegen, checker, expr);
            }
            if (strcmp(pkg->name, "errors") == 0 && strcmp(sel->selector, "New") == 0) {
                // errors.New(string) -> error. Box the message into a heap goo_error and
                // store its pointer (as i8*) in the nullable error handle.
                if (!call->args) {
                    codegen_error(codegen, expr->pos, "errors.New: expected a string argument");
                    return NULL;
                }
                ValueInfo* msg = codegen_generate_expression(codegen, checker, call->args);
                if (!msg) return NULL;
                LLVMValueRef msg_val = msg->llvm_value;
                if (msg->is_lvalue && msg->goo_type) {
                    LLVMTypeRef mt = codegen_type_to_llvm(codegen, msg->goo_type);
                    if (mt) msg_val = LLVMBuildLoad2(codegen->builder, mt, msg_val, "errnew_msg");
                }
                value_info_free(msg);

                LLVMValueRef from_str = LLVMGetNamedFunction(codegen->module, "goo_error_from_string");
                if (!from_str) { codegen_error(codegen, expr->pos, "goo_error_from_string not found in module"); return NULL; }
                LLVMTypeRef from_str_ty = LLVMGlobalGetValueType(from_str);
                LLVMValueRef args1[] = { msg_val };
                LLVMValueRef handle = LLVMBuildCall2(codegen->builder, from_str_ty, from_str, args1, 1, "errnew_box");

                Type* err_type = type_checker_error_type(checker);
                LLVMTypeRef err_llvm = codegen_type_to_llvm(codegen, err_type);
                LLVMValueRef is_null = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), 0, 0);
                LLVMValueRef err_val = LLVMGetUndef(err_llvm);
                err_val = LLVMBuildInsertValue(codegen->builder, err_val, is_null, 0, "en.is_null");
                err_val = LLVMBuildInsertValue(codegen->builder, err_val, handle, 1, "en.ptr");
                return value_info_new(NULL, err_val, err_type);
            }
            if (strcmp(pkg->name, "errors") == 0 && strcmp(sel->selector, "Unwrap") == 0) {
                // errors.Unwrap(error) -> error: read goo_error.cause via the runtime
                // helper, rebuild the nullable {is_null = cause==null, ptr = cause}.
                if (!call->args) {
                    codegen_error(codegen, expr->pos, "errors.Unwrap: expected an error argument");
                    return NULL;
                }
                ValueInfo* ev = codegen_generate_expression(codegen, checker, call->args);
                if (!ev) return NULL;
                LLVMValueRef err_loaded = ev->llvm_value;
                if (ev->is_lvalue && ev->goo_type) {
                    LLVMTypeRef et = codegen_type_to_llvm(codegen, ev->goo_type);
                    if (et) err_loaded = LLVMBuildLoad2(codegen->builder, et, err_loaded, "unwrap_load");
                }
                value_info_free(ev);

                LLVMValueRef handle2 = LLVMBuildExtractValue(codegen->builder, err_loaded, 1, "unwrap.handle");
                LLVMValueRef unwrap_fn = LLVMGetNamedFunction(codegen->module, "goo_error_unwrap");
                if (!unwrap_fn) { codegen_error(codegen, expr->pos, "goo_error_unwrap not found in module"); return NULL; }
                LLVMValueRef uargs[] = { handle2 };
                LLVMValueRef cause = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(unwrap_fn), unwrap_fn, uargs, 1, "unwrap.cause");

                LLVMTypeRef i8pt = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
                LLVMValueRef uw_is_null = LLVMBuildICmp(codegen->builder, LLVMIntEQ, cause, LLVMConstNull(i8pt), "unwrap.isnull");
                Type* uw_err_type = type_checker_error_type(checker);
                LLVMTypeRef uw_err_llvm = codegen_type_to_llvm(codegen, uw_err_type);
                LLVMValueRef uw_err_val = LLVMGetUndef(uw_err_llvm);
                uw_err_val = LLVMBuildInsertValue(codegen->builder, uw_err_val, uw_is_null, 0, "uw.is_null");
                uw_err_val = LLVMBuildInsertValue(codegen->builder, uw_err_val, cause, 1, "uw.ptr");
                return value_info_new(NULL, uw_err_val, uw_err_type);
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

        // error.Error(): nil-guarded read of the boxed message (Phase 6 Task 3).
        // The error type is a tagged nullable handle, not a struct — it has no
        // method set, so it must be special-cased before the struct/interface
        // dispatch below (which would resolve "error__Error", find nothing, and
        // fall through to the generic call path's "Undefined identifier").
        if (type_is_error(recv_type) &&
            strcmp(msel->selector, "Error") == 0) {
            ValueInfo* rv = codegen_generate_expression(codegen, checker, msel->expr);
            if (!rv) return NULL;
            LLVMValueRef recv_val = rv->llvm_value;
            if (rv->is_lvalue) {
                LLVMTypeRef rt = codegen_type_to_llvm(codegen, recv_type);
                if (rt) {
                    recv_val = LLVMBuildLoad2(codegen->builder, rt, recv_val, "err.recv");
                }
            }
            value_info_free(rv);

            // recv_val is the loaded nullable {i1 is_null, i8* handle}.
            LLVMValueRef is_null = LLVMBuildExtractValue(codegen->builder, recv_val, 0, "err.is_null");
            LLVMValueRef handle  = LLVMBuildExtractValue(codegen->builder, recv_val, 1, "err.handle");
            LLVMValueRef msgfn = LLVMGetNamedFunction(codegen->module, "goo_error_message");
            if (!msgfn) {
                codegen_error(codegen, expr->pos, "goo_error_message not found in module");
                return NULL;
            }
            LLVMTypeRef msgfn_ty = LLVMGlobalGetValueType(msgfn);
            LLVMValueRef cargs[] = { handle };
            LLVMValueRef msg = LLVMBuildCall2(codegen->builder, msgfn_ty, msgfn, cargs, 1, "err.msg");
            // nil-guard: empty goo_string {null,0} when is_null. goo_error_message
            // itself null-checks its arg too, so calling it on the null arm is
            // harmless — only the select()'d result matters.
            LLVMTypeRef str_llvm = codegen_get_basic_type(codegen, TYPE_STRING);
            LLVMValueRef empty = LLVMGetUndef(str_llvm);
            empty = LLVMBuildInsertValue(codegen->builder, empty,
                LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0)), 0, "empty.data");
            empty = LLVMBuildInsertValue(codegen->builder, empty,
                LLVMConstInt(LLVMInt64TypeInContext(codegen->context), 0, 0), 1, "empty.len");
            LLVMValueRef result = LLVMBuildSelect(codegen->builder, is_null, empty, msg, "err.error_result");
            return value_info_new(NULL, result, type_checker_get_builtin(checker, TYPE_STRING));
        }

        // Interface dispatch (P4-5): when the receiver is an interface value,
        // lower the call to a vtable dispatch instead of a direct mangled call.
        if (recv_type && recv_type->kind == TYPE_INTERFACE) {
            ValueInfo* iv = codegen_generate_expression(codegen, checker, msel->expr);
            if (!iv) return NULL;
            LLVMValueRef iface_val = iv->llvm_value;
            // A selector/index receiver (h.sh, shapes[0]) is returned as an
            // lvalue — the field/element address. Load the {vtable, data}
            // value before dispatch (which ExtractValues it); an identifier
            // receiver is already a loaded rvalue.
            if (iv->is_lvalue) {
                LLVMTypeRef ity = codegen_type_to_llvm(codegen, recv_type);
                if (ity) {
                    iface_val = LLVMBuildLoad2(codegen->builder, ity, iface_val,
                                               "ifc.recv");
                }
            }
            value_info_free(iv);

            size_t argc = 0;
            for (ASTNode* a = call->args; a; a = a->next) argc++;
            LLVMValueRef* dargs = argc ? malloc(argc * sizeof(LLVMValueRef)) : NULL;
            size_t i = 0;
            for (ASTNode* a = call->args; a; a = a->next, i++) {
                ValueInfo* av = codegen_generate_expression(codegen, checker, a);
                if (!av) { free(dargs); return NULL; }
                dargs[i] = av->llvm_value;
                value_info_free(av);
            }
            ValueInfo* r = codegen_interface_dispatch(codegen, checker, iface_val,
                                                      recv_type, msel->selector, dargs, argc);
            free(dargs);
            return r;
        }

        // Function generics Tier B: if the receiver's static type is a type
        // parameter, resolve it through the active monomorphization subst env
        // so `T.M()` dispatches to the concrete `C__M` (not the never-emitted
        // `T__M`). Identity on the non-generic path (active_subst is NULL).
        recv_type = (Type*)codegen_resolve_type(codegen, recv_type);

        const char* tn = type_receiver_name(recv_type);
        char* mangled = tn ? type_method_mangled_name(tn, msel->selector) : NULL;
        LLVMValueRef fn = mangled ? LLVMGetNamedFunction(codegen->module, mangled) : NULL;

        // Function generics Tier B: a bound method PROMOTED through an
        // embedded field (e.g. `Wrap` embeds `Base`, which declares
        // `Label`) has no direct `Wrap__Label` symbol — methods are only
        // ever emitted under their DECLARING type's mangled name. The
        // ordinary (non-generic) call path never hits this: the type
        // checker rewrites a promoted call's receiver expression to the
        // owning embedded field at type-check time (embed_wrap_base,
        // expression_checker.c's struct-selector path), so by codegen
        // recv_type is already the owner. A bounded T's method call is
        // checked differently — type_check_selector_expr's TYPE_PARAM
        // branch resolves directly against the interface bound with no AST
        // rewrite (expression_checker.c ~3628) — so after substitution the
        // receiver here is still the OUTER struct. Fall back to the same
        // embedding-resolve BFS the interface-thunk codegen already uses
        // (interface_codegen.c's build_thunk) to find the owning type, and
        // remember the hop path in `promo` so the receiver-address
        // construction below can walk it with GEPs (mirroring build_thunk's
        // own hop-walking loop) instead of the ordinary single-level
        // receiver logic.
        EmbedResult promo;
        memset(&promo, 0, sizeof(promo));
        if (!fn && recv_type && recv_type->kind == TYPE_STRUCT) {
            EmbedResult er = embedding_resolve(checker, recv_type, msel->selector);
            if (er.kind == EMBED_METHOD) {
                const char* otn = type_receiver_name(er.owner);
                char* omangled = otn ? type_method_mangled_name(otn, msel->selector) : NULL;
                LLVMValueRef ofn = omangled ? LLVMGetNamedFunction(codegen->module, omangled) : NULL;
                if (ofn) {
                    free(mangled);
                    mangled = omangled;
                    fn = ofn;
                    promo = er;
                } else {
                    free(omangled);
                }
            }
        }

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
            if (promo.kind == EMBED_METHOD) {
                // Promoted-through-embedding receiver: address the outer
                // struct, then GEP through each hop field (mirrors
                // build_thunk's hop-walking loop in interface_codegen.c),
                // following pointer indirection when a hop field is itself
                // an embedded pointer, landing on the owner's address. Then
                // pointer receivers take that address directly; value
                // receivers load through it.
                ValueInfo* addr = codegen_emit_lvalue_address(codegen, checker, msel->expr);
                if (!addr || !addr->is_lvalue) {
                    codegen_error(codegen, expr->pos,
                        "cannot call promoted method '%s' on non-addressable value",
                        msel->selector);
                    free(mangled);
                    return NULL;
                }
                // Identifier lvalues alias the value table; do not free addr
                // (mirrors the `&x` address-of path below and in
                // expression_codegen.c).
                LLVMValueRef recv_ptr = addr->llvm_value;
                Type* cur = recv_type;
                for (size_t h = 0; h < promo.len; h++) {
                    unsigned fidx = 0;
                    int found = 0;
                    for (size_t fi = 0; fi < cur->data.struct_type.field_count; fi++) {
                        if (strcmp(cur->data.struct_type.fields[fi].name, promo.path[h]) == 0) {
                            fidx = (unsigned)fi;
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        codegen_error(codegen, expr->pos,
                            "internal: embedding hop '%s' not found resolving promoted method '%s'",
                            promo.path[h], msel->selector);
                        free(mangled);
                        return NULL;
                    }
                    LLVMTypeRef cur_llvm = codegen_get_struct_type(codegen, cur);
                    recv_ptr = LLVMBuildStructGEP2(codegen->builder, cur_llvm, recv_ptr, fidx, "embed.hop");
                    Type* ft = cur->data.struct_type.fields[fidx].type;
                    if (ft->kind == TYPE_POINTER) {
                        recv_ptr = LLVMBuildLoad2(codegen->builder,
                            LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),
                            recv_ptr, "embed.load");
                        cur = ft->data.pointer.pointee_type;
                    } else {
                        cur = ft;
                    }
                }
                if (ptr_recv) {
                    recv_arg = recv_ptr;
                } else {
                    LLVMTypeRef owner_llvm = codegen_type_to_llvm(codegen, cur);
                    recv_arg = owner_llvm
                        ? LLVMBuildLoad2(codegen->builder, owner_llvm, recv_ptr, "recv_load")
                        : recv_ptr;
                }
            } else if (ptr_recv && !recv_is_ptr_value) {
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
                // A selector-expression receiver (e.g. `o.P.m()`) arrives as
                // an lvalue: llvm_value is the ADDRESS of the pointer-typed
                // field slot, not the pointer value itself. Load the pointer
                // out of the slot first, then dereference it below. An
                // identifier receiver (`p.m()`) is already the loaded
                // pointer value (is_lvalue=0) and needs no extra load here.
                if (pv->is_lvalue && pv->goo_type) {
                    LLVMTypeRef pt = codegen_type_to_llvm(codegen, pv->goo_type);
                    if (pt) ptr = LLVMBuildLoad2(codegen->builder, pt, ptr, "recv_load");
                }
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
                // Value receiver on a value, or pointer receiver on a
                // pointer. codegen_generate_expression auto-loads
                // IDENTIFIER lvalues to a value, but a selector-expression
                // receiver (e.g. `oo.P.Bump()`) arrives as an lvalue whose
                // llvm_value is the field's storage ADDRESS (is_lvalue=1) —
                // consumers own that load. Without it, the field's address
                // itself gets passed as the receiver: a pointer-receiver
                // call would then read/write through the field slot as if
                // it held a struct, corrupting the field (e.g. incrementing
                // the pointer's bit pattern in place); a value-receiver call
                // would load garbage starting at that address.
                ValueInfo* recv = codegen_generate_expression(codegen, checker, msel->expr);
                if (!recv) { free(mangled); return NULL; }
                LLVMValueRef rv = recv->llvm_value;
                if (recv->is_lvalue && recv->goo_type) {
                    LLVMTypeRef rt = codegen_type_to_llvm(codegen, recv->goo_type);
                    if (rt) rv = LLVMBuildLoad2(codegen->builder, rt, rv, "recv_load");
                }
                recv_arg = rv;
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

    // Function-generics Task 10 (Part B — call rewiring): a generic call
    // site (call->type_arg_count > 0, stamped by type_check_generic_call in
    // expression_checker.c only when the callee resolves to a generic
    // Variable) must dispatch to its MONOMORPHIZED INSTANCE symbol, never
    // the template's own bare name — codegen_generate_declaration
    // (codegen.c) skips emitting the generic template itself (Task 4), so
    // e.g. `Id` was never registered in this module; only instances like
    // `Id__int64` were, by codegen_monomorphize's worklist (Part A,
    // monomorphize.c) which ran to completion BEFORE any function body
    // (including this one) was emitted. This takes precedence over the
    // ordinary bare-name resolution below.
    //
    // type_substitute resolves each call->type_args[i] through
    // codegen->active_subst/active_subst_n: identity when this call site is
    // itself concrete (e.g. a call inside main — active_subst is NULL,
    // active_subst_n is 0, so the TYPE_PARAM case's bounds check never
    // fires and every arg is returned unchanged), or a full recursive
    // rewrite (TYPE_PARAM -> the enclosing instance's concrete binding, and
    // composite shapes like TYPE_SLICE/TYPE_POINTER/TYPE_FUNCTION rewritten
    // element-wise) when this call is being emitted INSIDE a generic body
    // that codegen_generate_function_instance is currently stamping (Part
    // A's nested-call discovery always pre-stamps that nested callee before
    // this body is ever emitted, so the instance looked up below is
    // guaranteed to already exist — this path only looks it up, it never
    // stamps). This MUST be the same recursive substitution Part A's
    // mono_instantiate uses to stamp the nested instance's mangled name
    // (monomorphize.c) — the single-level codegen_resolve_type only
    // rewrites a bare top-level TYPE_PARAM, so a composite type-arg like
    // `[]T` would keep its unbound element (`[]T` instead of `[]int`) and
    // mangle to a symbol Part A never stamped.
    // Comptime+generic composition (sub-project 2, Task 3): a call site can
    // carry BOTH axes at once (call->type_arg_count > 0 AND
    // call->comptime_value_arg_count > 0), e.g. `kernel[int64](4, arr)` where
    // `kernel` is declared `func kernel[T](comptime n int, arr [n]T)`. That
    // combined instance was stamped by the monomorphizer under
    // codegen_mangle_combined_instance's name (types first, then `__n<v>`
    // segments — monomorphize.c), NOT under either single-axis mangling, so
    // it must be looked up FIRST, before either single-axis branch below gets
    // a chance to mangle-and-miss (a per-type-only or per-value-only lookup
    // against a combined symbol always fails, which is exactly the
    // `Undefined identifier` regression this three-way replaces). This is a
    // genuine three-way dispatch (if / else-if / else-if): combined, then
    // generic-only, then comptime-only — never more than one branch can fire
    // for a given call, so `func_val` is unambiguous going into the fallback
    // below.
    ValueInfo* func_val = NULL;
    if (call->type_arg_count > 0 && call->comptime_value_arg_count > 0 &&
        call->function->type == AST_IDENTIFIER) {
        IdentifierNode* gid = (IdentifierNode*)call->function;
        Type** concrete_args = malloc(sizeof(Type*) * call->type_arg_count);
        if (!concrete_args) return NULL;
        // Same substitution discipline as the generic-only branch below:
        // when this call site sits inside an enclosing generic instance
        // currently being stamped, its own type_args are still expressed in
        // terms of the ENCLOSING instance's type params and must be resolved
        // through codegen->active_subst before mangling, or the lookup below
        // targets a symbol Part A (monomorphize.c) never stamped.
        for (size_t i = 0; i < call->type_arg_count; i++) {
            concrete_args[i] = type_substitute(call->type_args[i],
                codegen->active_subst, codegen->active_subst_n);
        }
        char* sym = codegen_mangle_combined_instance(gid->name, concrete_args,
            call->type_arg_count, call->comptime_value_args,
            call->comptime_value_arg_count);
        LLVMValueRef inst = sym ? LLVMGetNamedFunction(codegen->module, sym) : NULL;
        free(sym);
        if (inst) {
            // Mirror the generic-only branch's substituted-signature
            // discipline: downstream nullable-wrap / interface-box /
            // numeric-width-coercion logic needs the concrete
            // (post-type-substitution) parameter/return types the emitted
            // LLVM function actually has, not the template's TYPE_PARAM-
            // bearing signature. The comptime axis never changes the
            // callee's TYPE (only which literal a `comptime` param resolves
            // to inside the body), so only the type args need substituting
            // here — exactly like the generic-only branch.
            Variable* gvar = type_checker_lookup_variable(checker, gid->name);
            Type* concrete_sig = gvar
                ? type_substitute(gvar->type, concrete_args, call->type_arg_count)
                : NULL;
            func_val = value_info_new(gid->name, inst, concrete_sig);
        }
        free(concrete_args);
    } else if (call->type_arg_count > 0 && call->function->type == AST_IDENTIFIER) {
        IdentifierNode* gid = (IdentifierNode*)call->function;
        Type** concrete_args = malloc(sizeof(Type*) * call->type_arg_count);
        if (!concrete_args) return NULL;
        for (size_t i = 0; i < call->type_arg_count; i++) {
            concrete_args[i] = type_substitute(call->type_args[i],
                codegen->active_subst, codegen->active_subst_n);
        }
        char* sym = codegen_mangle_instance(gid->name, concrete_args, call->type_arg_count);
        LLVMValueRef inst = LLVMGetNamedFunction(codegen->module, sym);
        free(sym);
        if (inst) {
            // The instance's REAL (post-substitution) signature — downstream
            // nullable-wrap / interface-box / numeric-width-coercion logic
            // below needs the concrete parameter/return types the emitted
            // LLVM function actually has, not the template's own
            // TYPE_PARAM-bearing signature.
            Variable* gvar = type_checker_lookup_variable(checker, gid->name);
            Type* concrete_sig = gvar
                ? type_substitute(gvar->type, concrete_args, call->type_arg_count)
                : NULL;
            func_val = value_info_new(gid->name, inst, concrete_sig);
        }
        free(concrete_args);
    } else if (call->comptime_value_arg_count > 0 &&
               call->function->type == AST_IDENTIFIER) {
        // Comptime value params Task 3 (call rewiring): a comptime call site
        // (call->comptime_value_arg_count > 0, stamped by type_check_call_expr
        // in expression_checker.c only for a plain identifier callee — see that
        // function's recording-site doc comment) must dispatch to its
        // MONOMORPHIZED INSTANCE symbol, never the template's own bare name:
        // codegen_generate_declaration (codegen.c) skips emitting a plain
        // comptime-param function's template directly, so e.g. `fill` is never
        // registered in this module under its bare name — only instances like
        // `fill__n4` are, by codegen_monomorphize's comptime worklist
        // (monomorphize.c), which runs to completion BEFORE any function body
        // (including this one) is emitted. Mirrors the generic-call rewiring
        // above, one axis over: no type_substitute needed here, since a
        // comptime value doesn't change the callee's TYPE (only which literal
        // `n` resolves to inside its body) — the signature is just the plain
        // Variable's own type, unchanged. Reached only when the combined
        // branch above did NOT fire (call->type_arg_count == 0 here), by
        // construction of this if/else-if/else-if chain.
        IdentifierNode* cid = (IdentifierNode*)call->function;
        char* sym = codegen_mangle_comptime_instance(cid->name,
            call->comptime_value_args, call->comptime_value_arg_count);
        LLVMValueRef inst = sym ? LLVMGetNamedFunction(codegen->module, sym) : NULL;
        free(sym);
        if (inst) {
            Variable* cvar = type_checker_lookup_variable(checker, cid->name);
            func_val = value_info_new(cid->name, inst, cvar ? cvar->type : NULL);
        }
    }

    if (!func_val) {
        // See codegen_resolve_callee's comment: a bare identifier not
        // shadowed by a local variable/parameter resolves to the BARE LLVM
        // global function (the unconditionally-unchanged direct-call path);
        // anything else evaluates through the ordinary expression path and
        // may yield the universal `{ fn_ptr, env_ptr }` function-VALUE pair.
        func_val = codegen_resolve_callee(codegen, checker, call->function);
    }
    if (!func_val) return NULL;

    // The callee's declared parameter types drive nullable auto-wrapping:
    // a bare `T` or the nil literal passed to a `?T` parameter must be
    // lowered to the {i1,T} nullable struct, not stored raw.
    Type* func_goo_type = func_val->goo_type;

    // Generate arguments
    size_t arg_count = 0;
    ASTNode* arg = call->args;
    while (arg) {
        arg_count++;
        arg = arg->next;
    }

    // Task 2 (variadic ...T params, pack-only — Go's slice-sugar model, NOT
    // C varargs): a variadic callee's LLVM call site has one arg per FIXED
    // (non-variadic) param plus exactly ONE packed-slice arg built from
    // however many trailing actuals were given (0 or more) — never a 1:1
    // mapping with arg_count like the ordinary path below. param_types is
    // guaranteed non-NULL and param_count >= 1 here: is_variadic is only
    // ever set (declare_function_signature) alongside a real signature whose
    // last entry is the TYPE_SLICE element wrapper; the NULL-param
    // println/print/panic builtins are user-callable only by their own
    // dedicated codegen paths (dispatched earlier in this function), never
    // reaching this generic call path.
    int callee_is_variadic = func_goo_type && func_goo_type->kind == TYPE_FUNCTION
                              && func_goo_type->data.function.is_variadic
                              && func_goo_type->data.function.param_types
                              && func_goo_type->data.function.param_count > 0;
    size_t fixed_count = callee_is_variadic
        ? func_goo_type->data.function.param_count - 1 : arg_count;
    size_t llvm_argc = callee_is_variadic
        ? func_goo_type->data.function.param_count : arg_count;

    LLVMValueRef* args = NULL;
    if (llvm_argc > 0) {
        args = malloc(sizeof(LLVMValueRef) * llvm_argc);
        if (!args) {
            value_info_free(func_val);
            return NULL;
        }

        arg = call->args;
        for (size_t i = 0; i < fixed_count; i++) {
            Type* param_type = NULL;
            if (func_goo_type && func_goo_type->kind == TYPE_FUNCTION &&
                i < func_goo_type->data.function.param_count) {
                param_type = func_goo_type->data.function.param_types[i];
            }

            if (param_type &&
                (param_type->kind == TYPE_NULLABLE || type_is_nilable_ref_kind(param_type)) &&
                arg->type == AST_LITERAL &&
                ((LiteralNode*)arg)->literal_type == TOKEN_NIL) {
                // nil literal → build the param's null-nullable (?T) or bare
                // zero value (P2.2 option A: pointer/slice/map/chan/func)
                // directly, using the param's declared type as context.
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

            // Fix round 6 (M-r5a): instance-time enforcement of the
            // array-length compatibility the checker DEFERRED for a
            // comptime-length array argument (type_check_call_expr's
            // comptime_len_deferred). arg_val->goo_type is the instance's
            // re-derived REAL type, so a genuine mismatch at THIS instance
            // is a clean, instance-named compile failure instead of an
            // invalid-IR pass of a differently-sized aggregate. Gated on
            // the comptime_length flag (either side) — ordinary mismatched
            // array arguments never reach codegen.
            if (param_type && arg_val->goo_type &&
                param_type->kind == TYPE_ARRAY &&
                arg_val->goo_type->kind == TYPE_ARRAY &&
                (param_type->data.array.comptime_length ||
                 arg_val->goo_type->data.array.comptime_length) &&
                param_type->data.array.length != arg_val->goo_type->data.array.length) {
                codegen_error(codegen, arg->pos,
                    "cannot pass [%zu]-length array to [%zu]-length array parameter in comptime instance '%s'",
                    arg_val->goo_type->data.array.length,
                    param_type->data.array.length,
                    codegen->symbol_override ? codegen->symbol_override : "?");
                value_info_free(arg_val);
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

            // Box a concrete argument into an interface parameter (P4-5).
            if (param_type && param_type->kind == TYPE_INTERFACE &&
                arg_val->goo_type && arg_val->goo_type->kind != TYPE_INTERFACE) {
                if (arg_val->is_lvalue && arg_val->goo_type) {
                    LLVMTypeRef at = codegen_type_to_llvm(codegen, arg_val->goo_type);
                    if (at) {
                        arg_val->llvm_value = LLVMBuildLoad2(codegen->builder, at, arg_val->llvm_value, "ifargld");
                        arg_val->is_lvalue = 0;
                    }
                }
                LLVMValueRef boxed = codegen_interface_box(codegen, checker, param_type,
                                                           arg_val->goo_type, arg_val->llvm_value);
                if (!boxed) { value_info_free(arg_val); free(args); value_info_free(func_val); return NULL; }
                arg_val->llvm_value = boxed;
                arg_val->goo_type = param_type;
            }

            // Load a scalar lvalue argument before use. The nullable/interface
            // arms above already load internally when they fire; this is the
            // general (non-nullable, non-interface) fallthrough, which used to
            // pass the raw element pointer straight through. A struct-field
            // arg like `f(p.V)` crashed the LLVM verifier ("Call parameter
            // type does not match function signature!") instead of passing
            // the field's loaded value — same family as the nullable/
            // interface arms' own load-before-use step.
            if (arg_val->is_lvalue && arg_val->goo_type) {
                LLVMTypeRef at = codegen_type_to_llvm(codegen, arg_val->goo_type);
                if (at) {
                    arg_val->llvm_value = LLVMBuildLoad2(codegen->builder, at, arg_val->llvm_value, "argld");
                    arg_val->is_lvalue = 0;
                }
            }

            // Numeric width coercion: the checker's P2-2 guard rejects a
            // numeric argument whose width/kind differs from the declared
            // parameter for calls it can verify by signature identity, but
            // that guard only fires when the callee is resolved that way
            // (see expression_checker.c's check_signature gate) — calls it
            // cannot see through that way could still admit a numeric arg
            // whose LLVM representation differs from the parameter's. Coerce
            // here (a no-op when the LLVM types already match) using the
            // shared width-coercion helper, the same idiom already applied at
            // every other numeric sink (var-decl, append, channel-send).
            if (param_type && type_is_numeric(param_type) &&
                arg_val->goo_type && type_is_numeric(arg_val->goo_type)) {
                LLVMTypeRef param_llvm = codegen_type_to_llvm(codegen, param_type);
                if (param_llvm) {
                    int src_signed = type_is_signed(arg_val->goo_type);
                    arg_val->llvm_value = codegen_coerce_to_type(
                        codegen, arg_val->llvm_value, src_signed, param_llvm);
                }
            }

            args[i] = arg_val->llvm_value;
            value_info_free(arg_val);
            arg = arg->next;
        }

        // Task 2: pack every remaining trailing actual (0 or more — `arg`
        // now points at the first trailing node, or NULL if none were
        // given) into the variadic param's slice. Reuses the SAME
        // construction helper the `[]T{...}` literal path uses
        // (codegen_build_slice_from_elems, exposed non-static in codegen.h
        // for this call site) instead of duplicating the alloca/store/
        // insertvalue sequence: it evaluates each trailing arg expression
        // itself (once — not double-evaluated here), applying the identical
        // nullable-wrap / interface-box / constant-width-normalize /
        // slice_coerce_elem rules a `[]int{...}` literal's elements get.
        // Zero trailing args (`sum()`) is exactly the `first_elem == NULL`
        // case that path already handles for an empty literal.
        if (callee_is_variadic && call->has_spread) {
            // Task 3: `f(s...)` bypasses the per-element pack builder
            // entirely — the typechecker (expression_checker.c's has_spread
            // block) already guarantees `arg` is the sole trailing node and
            // its type is []E with E identical to the variadic element type.
            // Pass its slice value straight through as the pack arg (Go
            // aliasing semantics — the golden's `mut(s...)` mutating s[0]
            // pins this): generate the operand, load if it arrived as an
            // lvalue (mirrors the generic fixed-arg lvalue-load above), and
            // use the resulting {ptr,len,cap} aggregate value AS-IS, with no
            // copy. This is the exact by-value shape
            // codegen_build_slice_from_elems itself returns (its final
            // insertvalue chain builds the same aggregate), so both paths
            // hand args[fixed_count] the identical representation.
            ValueInfo* spread_val = codegen_generate_expression(codegen, checker, arg);
            if (!spread_val) {
                free(args);
                value_info_free(func_val);
                return NULL;
            }
            if (spread_val->is_lvalue && spread_val->goo_type) {
                LLVMTypeRef st = codegen_type_to_llvm(codegen, spread_val->goo_type);
                if (st) {
                    spread_val->llvm_value = LLVMBuildLoad2(codegen->builder,
                        st, spread_val->llvm_value, "spreadld");
                    spread_val->is_lvalue = 0;
                }
            }
            args[fixed_count] = spread_val->llvm_value;
            value_info_free(spread_val);
        } else if (callee_is_variadic) {
            Type* slice_type = func_goo_type->data.function.param_types[fixed_count];
            ValueInfo* packed = codegen_build_slice_from_elems(
                codegen, checker, arg, slice_type, expr->pos);
            if (!packed) {
                free(args);
                value_info_free(func_val);
                return NULL;
            }
            args[fixed_count] = packed->llvm_value;
            value_info_free(packed);
        }
    }

    // Generate the call. A direct callee (LLVMIsAFunction — the bare-call
    // path codegen_resolve_callee preserves above) calls unconditionally
    // unchanged: `call @F(args...)`. Anything else is a function VALUE: the
    // universal fat pointer `{ fn_ptr, env_ptr }` (env FIRST — see
    // docs/superpowers/specs/2026-07-03-closures-design.md
    // "Representation", a change-together contract Branch B's closures build
    // on unseen). codegen_generate_selector_expr / codegen_generate_
    // index_expr both return a func-typed field/element as its ADDRESS
    // (is_lvalue=1) — load it through the goo type before extracting; a
    // local var/param is the same shape. Extract fn_ptr (idx 0) / env_ptr
    // (idx 1) and call `fn_ptr(env_ptr, args...)` — the exact ABI
    // codegen_get_func_thunk's thunks and codegen_get_funcval_call_type
    // both build (function_codegen.c / type_mapping.c).
    LLVMValueRef callee_val = func_val->llvm_value;
    LLVMValueRef result;

    if (callee_val && LLVMIsAFunction(callee_val)) {
        // Unchanged bare-call path. LLVMGetElementType doesn't work with
        // LLVM 22 opaque pointers — use LLVMGlobalGetValueType which
        // returns the underlying function type directly for any global
        // value (functions are globals). Use an empty result name for void
        // functions (invalid to name a void value).
        LLVMTypeRef func_llvm_type = LLVMGlobalGetValueType(callee_val);
        const char* result_name = (LLVMGetTypeKind(LLVMGetReturnType(func_llvm_type)) == LLVMVoidTypeKind)
                                  ? "" : "call";
        result = LLVMBuildCall2(codegen->builder, func_llvm_type, callee_val, args, (unsigned)llvm_argc, result_name);
    } else {
        if (func_val->is_lvalue && func_goo_type) {
            LLVMTypeRef pair_ty = codegen_type_to_llvm(codegen, func_goo_type);
            if (pair_ty) {
                callee_val = LLVMBuildLoad2(codegen->builder, pair_ty, callee_val, "funcval_load");
            }
        }
        if (!callee_val || !func_goo_type || func_goo_type->kind != TYPE_FUNCTION) {
            codegen_error(codegen, expr->pos,
                          "internal: indirect call target is not a function value");
            free(args);
            value_info_free(func_val);
            return NULL;
        }

        LLVMValueRef fn_ptr  = LLVMBuildExtractValue(codegen->builder, callee_val, 0, "funcval_fn");
        LLVMValueRef env_ptr = LLVMBuildExtractValue(codegen->builder, callee_val, 1, "funcval_env");

        // Task 3: guard the indirect call only — a nil func VALUE (zero-
        // valued `var f func(...)...`, never assigned) has fn_ptr == NULL.
        codegen_emit_funcnil_check(codegen, fn_ptr, expr);

        LLVMTypeRef call_type = codegen_get_funcval_call_type(codegen, func_goo_type);
        if (!call_type) {
            codegen_error(codegen, expr->pos, "internal: cannot build indirect-call type");
            free(args);
            value_info_free(func_val);
            return NULL;
        }

        LLVMValueRef* full_args = malloc(sizeof(LLVMValueRef) * (llvm_argc + 1));
        if (!full_args) {
            free(args);
            value_info_free(func_val);
            return NULL;
        }
        full_args[0] = env_ptr;
        for (size_t i = 0; i < llvm_argc; i++) full_args[i + 1] = args[i];

        const char* result_name = (LLVMGetTypeKind(LLVMGetReturnType(call_type)) == LLVMVoidTypeKind)
                                  ? "" : "call";
        result = LLVMBuildCall2(codegen->builder, call_type, fn_ptr, full_args,
                                (unsigned)(llvm_argc + 1), result_name);
        free(full_args);
    }

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

// strconv.Atoi(string) -> !int
// Calls the runtime goo_string_to_int(goo_string_t s, int64_t* out) -> int.
// On success (ok!=0) wraps *out as a !int success union; on failure wraps
// "strconv.Atoi: invalid syntax" as the error string.
static ValueInfo* codegen_generate_atoi_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available for strconv.Atoi");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;
    CallExprNode* call = (CallExprNode*)expr;
    if (!call->args) {
        codegen_error(codegen, expr->pos, "strconv.Atoi: expected one string argument");
        return NULL;
    }

    // The !int result type is in expr->node_type (set by type_check_call_expr).
    Type* result_type = expr->node_type;
    if (!result_type || !type_is_error_union(result_type)) {
        codegen_error(codegen, expr->pos, "strconv.Atoi: no !int type context");
        return NULL;
    }
    LLVMTypeRef union_llvm = codegen_type_to_llvm(codegen, result_type);
    if (!union_llvm) return NULL;

    // Locate the pre-declared runtime function.
    LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_string_to_int");
    if (!fn) {
        codegen_error(codegen, expr->pos, "goo_string_to_int not found in module");
        return NULL;
    }

    // Evaluate the string argument; load through lvalue if needed.
    ValueInfo* str_vi = codegen_generate_expression(codegen, checker, call->args);
    if (!str_vi) return NULL;
    LLVMValueRef str_val = str_vi->llvm_value;
    if (str_vi->is_lvalue && str_vi->goo_type) {
        LLVMTypeRef st = codegen_type_to_llvm(codegen, str_vi->goo_type);
        if (st) str_val = LLVMBuildLoad2(codegen->builder, st, str_val, "atoi_str");
    }
    value_info_free(str_vi);

    // alloca i64 out — receives the parsed value on success.
    LLVMTypeRef i64_type = LLVMInt64TypeInContext(codegen->context);
    LLVMValueRef out_ptr = codegen_create_entry_alloca(codegen, i64_type, "atoi_out");

    // ok = goo_string_to_int(str_val, out_ptr)
    LLVMValueRef call_args[] = { str_val, out_ptr };
    LLVMValueRef ok = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn),
                                     fn, call_args, 2, "atoi_ok");

    // Branch on ok != 0.
    LLVMValueRef zero_i32 = LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0);
    LLVMValueRef cond = LLVMBuildICmp(codegen->builder, LLVMIntNE, ok, zero_i32, "atoi_cond");

    LLVMBasicBlockRef success_block = codegen_create_block(codegen, "atoi.success");
    LLVMBasicBlockRef error_block   = codegen_create_block(codegen, "atoi.error");
    LLVMBasicBlockRef merge_block   = codegen_create_block(codegen, "atoi.merge");
    LLVMBuildCondBr(codegen->builder, cond, success_block, error_block);

    // Success block: load *out, wrap in !int success union.
    codegen_set_insert_point(codegen, success_block);
    LLVMValueRef int_val = LLVMBuildLoad2(codegen->builder, i64_type, out_ptr, "atoi_val");
    Type* int_type = result_type->data.error_union.value_type;
    LLVMValueRef succ = codegen_create_error_union_success(codegen, union_llvm, int_val, int_type);
    LLVMBuildBr(codegen->builder, merge_block);
    LLVMBasicBlockRef success_exit = LLVMGetInsertBlock(codegen->builder);

    // Error block: build a goo_string_t from the literal message and wrap in !int error union.
    codegen_set_insert_point(codegen, error_block);
    const char* err_msg = "strconv.Atoi: invalid syntax";
    LLVMValueRef msg_ptr = LLVMBuildGlobalStringPtr(codegen->builder, err_msg, "atoi_err_ptr");
    LLVMTypeRef str_llvm = codegen_get_basic_type(codegen, TYPE_STRING);
    LLVMValueRef msg_val = LLVMGetUndef(str_llvm);
    msg_val = LLVMBuildInsertValue(codegen->builder, msg_val, msg_ptr, 0, "atoi_err_data");
    LLVMValueRef msg_len = LLVMConstInt(i64_type,
                                        (unsigned long long)strlen(err_msg), 0);
    msg_val = LLVMBuildInsertValue(codegen->builder, msg_val, msg_len, 1, "atoi_err_len");
    LLVMValueRef errv = codegen_create_error_union_error(codegen, union_llvm, msg_val);
    LLVMBuildBr(codegen->builder, merge_block);
    LLVMBasicBlockRef error_exit = LLVMGetInsertBlock(codegen->builder);

    // Merge block: PHI the two !int union values.
    codegen_set_insert_point(codegen, merge_block);
    LLVMValueRef phi = LLVMBuildPhi(codegen->builder, union_llvm, "atoi_result");
    LLVMAddIncoming(phi, &succ, &success_exit, 1);
    LLVMAddIncoming(phi, &errv, &error_exit, 1);

    return value_info_new(NULL, phi, result_type);
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
        // Coerce an integer arg to the runtime param's width. Now that untyped
        // literals default to i64, a literal like the offset in
        // os.ReadByte(path, 0) must narrow to a runtime fn's i32 param (and a
        // narrow value widens with the arg's signedness).
        else if (i < param_count &&
                 LLVMGetTypeKind(param_types[i]) == LLVMIntegerTypeKind &&
                 LLVMGetTypeKind(LLVMTypeOf(val)) == LLVMIntegerTypeKind) {
            unsigned pw = LLVMGetIntTypeWidth(param_types[i]);
            unsigned vw = LLVMGetIntTypeWidth(LLVMTypeOf(val));
            if (vw > pw) {
                val = LLVMBuildTrunc(codegen->builder, val, param_types[i], "arg_trunc");
            } else if (vw < pw) {
                val = (v->goo_type && !type_is_signed(v->goo_type))
                    ? LLVMBuildZExt(codegen->builder, val, param_types[i], "arg_zext")
                    : LLVMBuildSExt(codegen->builder, val, param_types[i], "arg_sext");
            }
        }
        args[i] = val;
        value_info_free(v);
    }

    LLVMValueRef result = LLVMBuildCall2(codegen->builder,
                                         LLVMGlobalGetValueType(func),
                                         func, args, (unsigned)arg_count,
                                         return_kind == TYPE_VOID ? "" : "stdlib_ret");
    free(args);

    // Bool-returning shims (e.g. goo_strings_contains) return an i32 0/1 from
    // the C runtime, but TYPE_BOOL is i1 — so the raw result can't drive a
    // branch (`if strings.Contains(...)` emitted `br i32`). Coerce to i1.
    if (return_kind == TYPE_BOOL &&
        LLVMGetTypeKind(LLVMTypeOf(result)) == LLVMIntegerTypeKind &&
        LLVMGetIntTypeWidth(LLVMTypeOf(result)) != 1) {
        LLVMValueRef zero = LLVMConstInt(LLVMTypeOf(result), 0, 0);
        result = LLVMBuildICmp(codegen->builder, LLVMIntNE, result, zero, "shim_bool_i1");
    }

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

#if LLVM_AVAILABLE
// codegen_emit_fmt_value: recursive fmt.Println value formatter (struct /
// pointer-to-struct formatting task). Emits the no-newline print call(s) for
// ONE already-loaded value `val` of goo type `ty`. Moved verbatim from the
// Println arg loop's inline per-kind dispatch (string/int/uint/bool/float/
// error/interface — byte-identical behavior), plus two new recursive cases:
//
//   - TYPE_STRUCT: "{" field0 " " field1 ... "}" (Go-style struct format),
//     each field value extracted with LLVMBuildExtractValue and recursed on.
//   - TYPE_POINTER to TYPE_STRUCT: a nil check (icmp eq null, branch to a
//     nil/nonnil block, both joining a cont block — no phi needed, this is
//     print side effects only) — nil prints "<nil>", non-nil prints "&" then
//     loads the struct and recurses on the pointee at depth+1.
//
// `depth` starts at 0 for a top-level Println argument. depth > 6 prints
// "..." and stops WITHOUT recursing further: this formatter recurses over
// TYPES at codegen time (there is no runtime value to bottom out on, only
// the static type shape), so a self-referential struct
// (`type Node struct { v int; next *Node }`) would otherwise send the
// compiler into unbounded recursion emitting IR forever — this cap is
// required for the compiler to terminate on such a type, not merely a
// nicety for deeply-nested output.
//
// Pointer-to-non-struct (e.g. *int), slices, maps, arrays, and functions
// stay out of scope (existing clean codegen_error) — Go prints raw
// addresses for those, which is non-deterministic and not needed here.
//
// Returns 1 on success, 0 after emitting a source-located codegen_error.
static int codegen_emit_fmt_value(CodeGenerator* codegen, TypeChecker* checker,
                                   LLVMValueRef val, Type* ty, int depth, Position pos) {
    (void)checker;
    LLVMValueRef print_func = LLVMGetNamedFunction(codegen->module, "goo_print");
    if (!print_func) {
        codegen_error(codegen, pos, "goo_print function not found in module");
        return 0;
    }

    // Recursion cap (REQUIRED, see doc comment above): a self-referential
    // struct type recurses over TYPES at codegen time, not over a bounded
    // runtime value, so without this the compiler itself would never
    // terminate on `type Node struct { next *Node }`.
    if (depth > 6) {
        LLVMValueRef dots = LLVMBuildGlobalStringPtr(codegen->builder, "...", "fmt_depth_cap");
        LLVMValueRef dargs[] = { dots };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(print_func),
                      print_func, dargs, 1, "");
        return 1;
    }

    TypeKind kind = (ty ? ty->kind : TYPE_VOID);

    // error (Phase 6 Task 4): tagged nullable, not one of fmt's primitive
    // kinds, so it must be special-cased before the kind switch below (it
    // would otherwise fall into the "unsupported argument type" error).
    // Print "<nil>" when null, else the boxed message — same nil-guard/
    // extract/goo_error_message shape as .Error() (Task 3, ~line 495).
    if (type_is_error(ty)) {
        LLVMValueRef to_print = codegen_error_display_string(codegen, val, pos);
        if (!to_print) return 0;

        LLVMValueRef str_fn = LLVMGetNamedFunction(codegen->module, "goo_print_string");
        if (!str_fn) {
            codegen_error(codegen, pos, "goo_print_string not found in module");
            return 0;
        }
        LLVMValueRef pargs[] = { to_print };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(str_fn),
                      str_fn, pargs, 1, "");
    } else if (kind == TYPE_STRING) {
        // Pass the whole goo_string struct to the length-aware printer.
        // Extracting just the data ptr and calling goo_print (strlen) is
        // wrong for a substring (F5): a shared-buffer slice like
        // "hello"[1:3] has no '\0' after "el", so strlen would read past
        // the logical length. goo_print_string honours the length field.
        LLVMValueRef str_fn = LLVMGetNamedFunction(codegen->module, "goo_print_string");
        if (!str_fn) {
            codegen_error(codegen, pos, "goo_print_string not found in module");
            return 0;
        }
        LLVMValueRef args[] = { val };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(str_fn),
                      str_fn, args, 1, "");
    } else if (kind == TYPE_INT8 || kind == TYPE_INT16 || kind == TYPE_INT32 || kind == TYPE_INT64) {
        LLVMValueRef int_fn = LLVMGetNamedFunction(codegen->module, "goo_print_int");
        LLVMValueRef widened = LLVMBuildSExt(codegen->builder, val,
                                             LLVMInt64TypeInContext(codegen->context), "sext");
        LLVMValueRef args[] = { widened };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(int_fn),
                      int_fn, args, 1, "");
    } else if (kind == TYPE_UINT8 || kind == TYPE_UINT16 || kind == TYPE_UINT32 || kind == TYPE_UINT64) {
        // Unsigned: zero-extend (not sign-extend) to u64 and use the unsigned
        // printer so large values print correctly (uint64 above INT64_MAX
        // would show negative through goo_print_int).
        LLVMValueRef uint_fn = LLVMGetNamedFunction(codegen->module, "goo_print_uint");
        LLVMValueRef widened = LLVMBuildZExt(codegen->builder, val,
                                             LLVMInt64TypeInContext(codegen->context), "zext");
        LLVMValueRef args[] = { widened };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(uint_fn),
                      uint_fn, args, 1, "");
    } else if (kind == TYPE_BOOL) {
        LLVMValueRef bool_fn = LLVMGetNamedFunction(codegen->module, "goo_print_bool");
        LLVMValueRef widened = LLVMBuildZExt(codegen->builder, val,
                                             LLVMInt32TypeInContext(codegen->context), "zext");
        LLVMValueRef args[] = { widened };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(bool_fn),
                      bool_fn, args, 1, "");
    } else if (kind == TYPE_FLOAT32 || kind == TYPE_FLOAT64) {
        LLVMValueRef float_fn = LLVMGetNamedFunction(codegen->module, "goo_print_float");
        LLVMValueRef widened = (kind == TYPE_FLOAT32)
            ? LLVMBuildFPExt(codegen->builder, val,
                             LLVMDoubleTypeInContext(codegen->context), "fpext")
            : val;
        LLVMValueRef args[] = { widened };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(float_fn),
                      float_fn, args, 1, "");
    } else if (kind == TYPE_INTERFACE) {
        // Print an interface value by its dynamic type: {vtable,data} ->
        // goo_iface_format (runtime helper: nil vtable -> "<nil>", else
        // vtable[0]=desc, desc.fmt_fn(data)) -> goo_print_string.
        LLVMValueRef ival = val;   // {ptr vtable, ptr data}
        LLVMValueRef vtab = LLVMBuildExtractValue(codegen->builder, ival, 0, "ifvt");
        LLVMValueRef data = LLVMBuildExtractValue(codegen->builder, ival, 1, "ifdata");

        LLVMValueRef fmtcall = LLVMGetNamedFunction(codegen->module, "goo_iface_format");
        if (!fmtcall) {
            codegen_error(codegen, pos, "goo_iface_format not found");
            return 0;
        }
        LLVMValueRef fargs[] = { vtab, data };
        LLVMValueRef s = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fmtcall),
                                        fmtcall, fargs, 2, "ifstr");

        LLVMValueRef str_fn = LLVMGetNamedFunction(codegen->module, "goo_print_string");
        if (!str_fn) {
            codegen_error(codegen, pos, "goo_print_string not found in module");
            return 0;
        }
        LLVMValueRef pargs[] = { s };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(str_fn), str_fn, pargs, 1, "");
    } else if (kind == TYPE_STRUCT) {
        // "{" field0 " " field1 ... "}" — Go-style struct formatting.
        LLVMValueRef brace_open = LLVMBuildGlobalStringPtr(codegen->builder, "{", "fmt_struct_open");
        LLVMValueRef oargs[] = { brace_open };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(print_func),
                      print_func, oargs, 1, "");

        size_t fcount = ty->data.struct_type.field_count;
        StructField* fields = ty->data.struct_type.fields;
        for (size_t i = 0; i < fcount; i++) {
            if (i > 0) {
                LLVMValueRef sp = LLVMBuildGlobalStringPtr(codegen->builder, " ", "fmt_struct_sp");
                LLVMValueRef spargs[] = { sp };
                LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(print_func),
                              print_func, spargs, 1, "");
            }
            LLVMValueRef fv = LLVMBuildExtractValue(codegen->builder, val, (unsigned)i, "fmt_struct_field");
            if (!codegen_emit_fmt_value(codegen, checker, fv, fields[i].type, depth + 1, pos)) {
                return 0;
            }
        }

        LLVMValueRef brace_close = LLVMBuildGlobalStringPtr(codegen->builder, "}", "fmt_struct_close");
        LLVMValueRef cargs[] = { brace_close };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(print_func),
                      print_func, cargs, 1, "");
    } else if (kind == TYPE_POINTER && ty->data.pointer.pointee_type &&
               ty->data.pointer.pointee_type->kind == TYPE_STRUCT) {
        // Pointer-to-struct: nil check -> "<nil>", else "&" + the pointee
        // struct's own formatting (no phi — both arms just emit print side
        // effects and fall through to a shared continuation block).
        LLVMValueRef null_ptr = LLVMConstNull(LLVMTypeOf(val));
        LLVMValueRef is_null = LLVMBuildICmp(codegen->builder, LLVMIntEQ, val, null_ptr, "fmt_ptr_isnull");
        LLVMBasicBlockRef nil_bb    = codegen_create_block(codegen, "fmt.nil");
        LLVMBasicBlockRef nonnil_bb = codegen_create_block(codegen, "fmt.nonnil");
        LLVMBasicBlockRef cont_bb   = codegen_create_block(codegen, "fmt.cont");
        LLVMBuildCondBr(codegen->builder, is_null, nil_bb, nonnil_bb);

        codegen_set_insert_point(codegen, nil_bb);
        LLVMValueRef nil_str = LLVMBuildGlobalStringPtr(codegen->builder, "<nil>", "fmt_ptr_nilstr");
        LLVMValueRef nargs[] = { nil_str };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(print_func),
                      print_func, nargs, 1, "");
        LLVMBuildBr(codegen->builder, cont_bb);

        codegen_set_insert_point(codegen, nonnil_bb);
        LLVMValueRef amp = LLVMBuildGlobalStringPtr(codegen->builder, "&", "fmt_ptr_amp");
        LLVMValueRef aargs[] = { amp };
        LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(print_func),
                      print_func, aargs, 1, "");

        Type* pointee = ty->data.pointer.pointee_type;
        LLVMTypeRef struct_llvm = codegen_type_to_llvm(codegen, pointee);
        if (!struct_llvm) {
            codegen_error(codegen, pos, "fmt.Println: cannot lower pointee struct type");
            return 0;
        }
        LLVMValueRef loaded = LLVMBuildLoad2(codegen->builder, struct_llvm, val, "fmt_ptr_load");
        if (!codegen_emit_fmt_value(codegen, checker, loaded, pointee, depth + 1, pos)) {
            return 0;
        }
        LLVMBuildBr(codegen->builder, cont_bb);

        codegen_set_insert_point(codegen, cont_bb);
    } else {
        // P0-3: an unsupported argument type is a clean source-located
        // codegen error, not a type-mismatched goo_print call that only
        // surfaces (as invalid IR) at the LLVM verifier.
        codegen_error(codegen, pos,
                      "fmt.Println: unsupported argument type (only string, integer, "
                      "bool, float, struct, and pointer-to-struct are supported in v1)");
        return 0;
    }

    return 1;
}

// fmt_sprintf_lit: build a literal goo_string_t {data, len} from a C string
// constant. Used by codegen_build_fmt_value_string for the punctuation it
// concatenates around struct/pointer values ("{", "}", "&", " ", "<nil>",
// "..."). Mirrors the literal-chunk construction fmt_emit_segments does for
// each literal format-string chunk (~line 2589-2598 below).
static LLVMValueRef fmt_sprintf_lit(CodeGenerator* codegen, LLVMTypeRef string_llvm, const char* s) {
    LLVMValueRef ptr = LLVMBuildGlobalStringPtr(codegen->builder, s, "sp_lit");
    LLVMValueRef len = LLVMConstInt(LLVMInt64TypeInContext(codegen->context), strlen(s), 0);
    LLVMValueRef str = LLVMGetUndef(string_llvm);
    str = LLVMBuildInsertValue(codegen->builder, str, ptr, 0, "sp_lit_ptr");
    str = LLVMBuildInsertValue(codegen->builder, str, len, 1, "sp_lit_len");
    return str;
}

// codegen_build_fmt_value_string: fmt.Sprintf %v value formatter (struct /
// pointer-to-struct formatting task). String-building counterpart to
// codegen_emit_fmt_value directly above: identical recursive per-kind
// dispatch (error/string/int/uint/bool/float/interface/struct/pointer-to-
// struct) and the same depth>6 termination cap (required for the same
// reason — a self-referential struct type recurses over TYPES at codegen
// time, not a bounded runtime value, so without the cap the compiler itself
// would never terminate on `type Node struct { next *Node }`). The
// difference: instead of calling goo_print for side effects, each case
// concatenates its piece onto a goo_string accumulator via
// goo_string_concat and returns the new accumulator.
//
// This is also why pointer-to-struct needs a phi where codegen_emit_fmt_value
// didn't: printing has no data dependency between the nil/non-nil arms (both
// just emit side-effecting print calls and fall through to a shared
// continuation block), but building a STRING value does — the result differs
// per arm, so the cont block must phi the two arms' produced accumulators.
//
// Returns the new accumulator goo_string, or NULL after emitting a
// source-located codegen_error.
static LLVMValueRef codegen_build_fmt_value_string(CodeGenerator* codegen, TypeChecker* checker,
                                                    LLVMValueRef acc, LLVMValueRef val,
                                                    Type* ty, int depth, Position pos) {
    (void)checker;
    LLVMValueRef concat_fn = LLVMGetNamedFunction(codegen->module, "goo_string_concat");
    if (!concat_fn) {
        codegen_error(codegen, pos, "goo_string_concat function not found in module");
        return NULL;
    }
    LLVMTypeRef string_llvm = codegen_get_basic_type(codegen, TYPE_STRING);

    // Recursion cap (REQUIRED, see codegen_emit_fmt_value's doc comment
    // above for why): mirrors that cap exactly so Sprintf agrees with
    // Println/Printf on where a deeply-nested/self-referential type gets
    // truncated.
    if (depth > 6) {
        LLVMValueRef dots = fmt_sprintf_lit(codegen, string_llvm, "...");
        LLVMValueRef cargs[] = { acc, dots };
        return LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn),
                              concat_fn, cargs, 2, "sp_acc");
    }

    TypeKind kind = (ty ? ty->kind : TYPE_VOID);

    if (type_is_error(ty)) {
        LLVMValueRef disp = codegen_error_display_string(codegen, val, pos);
        if (!disp) return NULL;
        LLVMValueRef cargs[] = { acc, disp };
        return LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn),
                              concat_fn, cargs, 2, "sp_acc");
    } else if (kind == TYPE_STRING) {
        LLVMValueRef cargs[] = { acc, val };
        return LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn),
                              concat_fn, cargs, 2, "sp_acc");
    } else if (kind == TYPE_INT8 || kind == TYPE_INT16 || kind == TYPE_INT32 || kind == TYPE_INT64) {
        LLVMValueRef int_fn = LLVMGetNamedFunction(codegen->module, "goo_int_to_string");
        if (!int_fn) { codegen_error(codegen, pos, "goo_int_to_string not found in module"); return NULL; }
        LLVMValueRef widened = LLVMBuildSExt(codegen->builder, val,
                                             LLVMInt64TypeInContext(codegen->context), "sext");
        LLVMValueRef sargs[] = { widened };
        LLVMValueRef s = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(int_fn),
                                        int_fn, sargs, 1, "sp_int");
        LLVMValueRef cargs[] = { acc, s };
        return LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn),
                              concat_fn, cargs, 2, "sp_acc");
    } else if (kind == TYPE_UINT8 || kind == TYPE_UINT16 || kind == TYPE_UINT32 || kind == TYPE_UINT64) {
        LLVMValueRef uint_fn = LLVMGetNamedFunction(codegen->module, "goo_uint_to_string");
        if (!uint_fn) { codegen_error(codegen, pos, "goo_uint_to_string not found in module"); return NULL; }
        LLVMValueRef widened = LLVMBuildZExt(codegen->builder, val,
                                             LLVMInt64TypeInContext(codegen->context), "zext");
        LLVMValueRef sargs[] = { widened };
        LLVMValueRef s = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(uint_fn),
                                        uint_fn, sargs, 1, "sp_uint");
        LLVMValueRef cargs[] = { acc, s };
        return LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn),
                              concat_fn, cargs, 2, "sp_acc");
    } else if (kind == TYPE_BOOL) {
        LLVMValueRef bool_fn = LLVMGetNamedFunction(codegen->module, "goo_bool_to_string");
        if (!bool_fn) { codegen_error(codegen, pos, "goo_bool_to_string not found in module"); return NULL; }
        LLVMValueRef widened = LLVMBuildZExt(codegen->builder, val,
                                             LLVMInt32TypeInContext(codegen->context), "zext");
        LLVMValueRef sargs[] = { widened };
        LLVMValueRef s = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(bool_fn),
                                        bool_fn, sargs, 1, "sp_bool");
        LLVMValueRef cargs[] = { acc, s };
        return LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn),
                              concat_fn, cargs, 2, "sp_acc");
    } else if (kind == TYPE_FLOAT32 || kind == TYPE_FLOAT64) {
        LLVMValueRef float_fn = LLVMGetNamedFunction(codegen->module, "goo_float_to_string");
        if (!float_fn) { codegen_error(codegen, pos, "goo_float_to_string not found in module"); return NULL; }
        LLVMValueRef widened = (kind == TYPE_FLOAT32)
            ? LLVMBuildFPExt(codegen->builder, val,
                             LLVMDoubleTypeInContext(codegen->context), "fpext")
            : val;
        LLVMValueRef sargs[] = { widened };
        LLVMValueRef s = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(float_fn),
                                        float_fn, sargs, 1, "sp_float");
        LLVMValueRef cargs[] = { acc, s };
        return LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn),
                              concat_fn, cargs, 2, "sp_acc");
    } else if (kind == TYPE_INTERFACE) {
        LLVMValueRef vtab = LLVMBuildExtractValue(codegen->builder, val, 0, "ifvt");
        LLVMValueRef data = LLVMBuildExtractValue(codegen->builder, val, 1, "ifdata");
        LLVMValueRef fmtcall = LLVMGetNamedFunction(codegen->module, "goo_iface_format");
        if (!fmtcall) { codegen_error(codegen, pos, "goo_iface_format not found"); return NULL; }
        LLVMValueRef fargs[] = { vtab, data };
        LLVMValueRef s = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fmtcall),
                                        fmtcall, fargs, 2, "ifstr");
        LLVMValueRef cargs[] = { acc, s };
        return LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn),
                              concat_fn, cargs, 2, "sp_acc");
    } else if (kind == TYPE_STRUCT) {
        // "{" field0 " " field1 ... "}" — Go-style struct formatting,
        // threaded through the accumulator (same shape as
        // codegen_emit_fmt_value's TYPE_STRUCT case, but concatenating
        // instead of printing).
        LLVMValueRef brace_open = fmt_sprintf_lit(codegen, string_llvm, "{");
        LLVMValueRef oargs[] = { acc, brace_open };
        acc = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn),
                             concat_fn, oargs, 2, "sp_acc");

        size_t fcount = ty->data.struct_type.field_count;
        StructField* fields = ty->data.struct_type.fields;
        for (size_t i = 0; i < fcount; i++) {
            if (i > 0) {
                LLVMValueRef sp = fmt_sprintf_lit(codegen, string_llvm, " ");
                LLVMValueRef spargs[] = { acc, sp };
                acc = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn),
                                     concat_fn, spargs, 2, "sp_acc");
            }
            LLVMValueRef fv = LLVMBuildExtractValue(codegen->builder, val, (unsigned)i, "fmt_struct_field");
            acc = codegen_build_fmt_value_string(codegen, checker, acc, fv, fields[i].type, depth + 1, pos);
            if (!acc) return NULL;
        }

        LLVMValueRef brace_close = fmt_sprintf_lit(codegen, string_llvm, "}");
        LLVMValueRef cargs2[] = { acc, brace_close };
        return LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn),
                              concat_fn, cargs2, 2, "sp_acc");
    } else if (kind == TYPE_POINTER && ty->data.pointer.pointee_type &&
               ty->data.pointer.pointee_type->kind == TYPE_STRUCT) {
        // Pointer-to-struct: nil check -> "<nil>", else "&" + the pointee
        // struct's own formatting. Unlike codegen_emit_fmt_value's twin
        // case, the two arms here produce DIFFERENT VALUES (not just side
        // effects), so the cont block needs a phi over the two arms'
        // accumulators.
        LLVMValueRef null_ptr = LLVMConstNull(LLVMTypeOf(val));
        LLVMValueRef is_null = LLVMBuildICmp(codegen->builder, LLVMIntEQ, val, null_ptr, "fmt_ptr_isnull");
        LLVMBasicBlockRef nil_bb    = codegen_create_block(codegen, "spfmt.nil");
        LLVMBasicBlockRef nonnil_bb = codegen_create_block(codegen, "spfmt.nonnil");
        LLVMBasicBlockRef cont_bb   = codegen_create_block(codegen, "spfmt.cont");
        LLVMBuildCondBr(codegen->builder, is_null, nil_bb, nonnil_bb);

        codegen_set_insert_point(codegen, nil_bb);
        LLVMValueRef nil_lit = fmt_sprintf_lit(codegen, string_llvm, "<nil>");
        LLVMValueRef nargs[] = { acc, nil_lit };
        LLVMValueRef acc_nil = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn),
                                              concat_fn, nargs, 2, "sp_acc_nil");
        // Capture the block we're actually leaving from — flushing the
        // chunk above can't branch, so this is still nil_bb here, but we
        // capture it the same way as the non-nil arm below for symmetry
        // and so a future edit that adds control flow to this arm can't
        // silently invalidate the phi's incoming block.
        LLVMBasicBlockRef nil_end = LLVMGetInsertBlock(codegen->builder);
        LLVMBuildBr(codegen->builder, cont_bb);

        codegen_set_insert_point(codegen, nonnil_bb);
        LLVMValueRef amp_lit = fmt_sprintf_lit(codegen, string_llvm, "&");
        LLVMValueRef aargs[] = { acc, amp_lit };
        LLVMValueRef acc1 = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn),
                                           concat_fn, aargs, 2, "sp_acc_amp");

        Type* pointee = ty->data.pointer.pointee_type;
        LLVMTypeRef struct_llvm = codegen_type_to_llvm(codegen, pointee);
        if (!struct_llvm) {
            codegen_error(codegen, pos, "fmt.Sprintf: cannot lower pointee struct type");
            return NULL;
        }
        LLVMValueRef loaded = LLVMBuildLoad2(codegen->builder, struct_llvm, val, "fmt_ptr_load");
        LLVMValueRef acc_nn = codegen_build_fmt_value_string(codegen, checker, acc1, loaded,
                                                              pointee, depth + 1, pos);
        if (!acc_nn) return NULL;
        // Captured AFTER the recursive call, NOT nonnil_bb: recursing into
        // a nested struct/pointer field can itself append basic blocks
        // (e.g. a nested pointer field runs this same nil-check), so the
        // block the builder is actually in now is the true predecessor for
        // the phi below — using nonnil_bb here would build a phi with a
        // stale/wrong incoming block and fail the LLVM verifier.
        LLVMBasicBlockRef nn_end = LLVMGetInsertBlock(codegen->builder);
        LLVMBuildBr(codegen->builder, cont_bb);

        codegen_set_insert_point(codegen, cont_bb);
        LLVMValueRef phi = LLVMBuildPhi(codegen->builder, string_llvm, "sp_ptr_phi");
        LLVMValueRef incoming_vals[]   = { acc_nil, acc_nn };
        LLVMBasicBlockRef incoming_blocks[] = { nil_end, nn_end };
        LLVMAddIncoming(phi, incoming_vals, incoming_blocks, 2);
        return phi;
    } else {
        codegen_error(codegen, pos,
                      "fmt.Sprintf: %%v: unsupported argument type (only string, integer, "
                      "bool, float, struct, and pointer-to-struct are supported in v1)");
        return NULL;
    }
}
#endif

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

        // Type-dispatched value formatting (scalar/string/bool/float/error/
        // interface, plus struct and pointer-to-struct) lives in the
        // recursive codegen_emit_fmt_value helper — depth 0 for a top-level
        // Println argument.
        if (!codegen_emit_fmt_value(codegen, checker, arg_val->llvm_value,
                                     arg_val->goo_type, 0, a->pos)) {
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

// fmt.Print: variadic, no trailing newline. Go's rule: a space is written
// between two adjacent operands iff NEITHER is a string. Each operand is
// formatted by the shared recursive helper codegen_emit_fmt_value (same as
// Println/Printf %v), so structs, pointers-to-struct, and every scalar kind
// format identically here.
ValueInfo* codegen_generate_fmt_print_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;
    CallExprNode* call = (CallExprNode*)expr;

    LLVMValueRef print_func = LLVMGetNamedFunction(codegen->module, "goo_print");
    if (!print_func) {
        codegen_error(codegen, expr->pos, "goo_print function not found in module");
        return NULL;
    }

    int first = 1;
    int prev_is_string = 0;
    for (ASTNode* a = call->args; a; a = a->next) {
        ValueInfo* arg_val = codegen_generate_expression(codegen, checker, a);
        if (!arg_val) {
            codegen_error(codegen, expr->pos, "Failed to generate argument for fmt.Print");
            return NULL;
        }
        // Load through an lvalue (field/index selectors arrive as an address).
        if (arg_val->is_lvalue && arg_val->goo_type) {
            LLVMTypeRef at = codegen_type_to_llvm(codegen, arg_val->goo_type);
            if (at) {
                arg_val->llvm_value = LLVMBuildLoad2(codegen->builder, at,
                                                     arg_val->llvm_value, "fmt_print_arg");
                arg_val->is_lvalue = 0;
            }
        }
        int is_string = arg_val->goo_type && arg_val->goo_type->kind == TYPE_STRING;
        // Separator: a space between two adjacent operands only when NEITHER is
        // a string (Go's fmt.Print rule — distinct from Println's always-space).
        if (!first && !prev_is_string && !is_string) {
            LLVMValueRef sp = LLVMBuildGlobalStringPtr(codegen->builder, " ", "fmt_print_sep");
            LLVMValueRef spargs[] = { sp };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(print_func),
                          print_func, spargs, 1, "");
        }
        if (!codegen_emit_fmt_value(codegen, checker, arg_val->llvm_value,
                                    arg_val->goo_type, 0, a->pos)) {
            value_info_free(arg_val);
            return NULL;
        }
        prev_is_string = is_string;
        first = 0;
        value_info_free(arg_val);
    }
    // fmt.Print writes no trailing newline.
    return value_info_new(NULL, NULL, type_checker_get_builtin(checker, TYPE_VOID));
#endif
}

ValueInfo* codegen_generate_print_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;

    CallExprNode* call = (CallExprNode*)expr;

    // Go's builtin `print`: variadic, NO separators between operands and NO
    // trailing newline (the builtin `println` — codegen_generate_println_call —
    // is the spaces+newline variant). Each operand is formatted by the shared
    // recursive helper codegen_emit_fmt_value, so scalars, structs, and
    // pointers-to-struct all work. The previous stub took only the first
    // argument and passed its raw value straight to goo_print (which takes a
    // char*), producing invalid IR for every non-char* argument — including a
    // plain string, whose value is a goo_string struct, not a char*.
    for (ASTNode* a = call->args; a; a = a->next) {
        ValueInfo* arg_val = codegen_generate_expression(codegen, checker, a);
        if (!arg_val) {
            codegen_error(codegen, expr->pos, "Failed to generate argument for print");
            return NULL;
        }
        if (arg_val->is_lvalue && arg_val->goo_type) {
            LLVMTypeRef at = codegen_type_to_llvm(codegen, arg_val->goo_type);
            if (at) {
                arg_val->llvm_value = LLVMBuildLoad2(codegen->builder, at,
                                                     arg_val->llvm_value, "print_arg");
                arg_val->is_lvalue = 0;
            }
        }
        if (!codegen_emit_fmt_value(codegen, checker, arg_val->llvm_value,
                                    arg_val->goo_type, 0, a->pos)) {
            value_info_free(arg_val);
            return NULL;
        }
        value_info_free(arg_val);
    }

    return value_info_new(NULL, NULL, type_checker_get_builtin(checker, TYPE_VOID));
#endif
}

#if LLVM_AVAILABLE
// Given a loaded `error` value {i1 is_null, i8* handle}, produce the goo_string
// to display: "<nil>" when null, else goo_error_message(handle). Shared by
// fmt.Println's error case and the %v verb. Returns NULL (after emitting a
// codegen_error) if a required runtime symbol is missing.
static LLVMValueRef codegen_error_display_string(CodeGenerator* codegen, LLVMValueRef err_loaded, Position pos) {
    LLVMValueRef is_null = LLVMBuildExtractValue(codegen->builder, err_loaded, 0, "edisp.is_null");
    LLVMValueRef handle  = LLVMBuildExtractValue(codegen->builder, err_loaded, 1, "edisp.handle");
    LLVMValueRef msgfn = LLVMGetNamedFunction(codegen->module, "goo_error_message");
    if (!msgfn) { codegen_error(codegen, pos, "goo_error_message not found in module"); return NULL; }
    LLVMValueRef cargs[] = { handle };
    LLVMValueRef msg = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(msgfn), msgfn, cargs, 1, "edisp.msg");
    LLVMTypeRef str_llvm = codegen_get_basic_type(codegen, TYPE_STRING);
    LLVMValueRef nil_ptr = LLVMBuildGlobalStringPtr(codegen->builder, "<nil>", "edisp.nilstr");
    LLVMValueRef nil_str = LLVMGetUndef(str_llvm);
    nil_str = LLVMBuildInsertValue(codegen->builder, nil_str, nil_ptr, 0, "edisp.nil.data");
    nil_str = LLVMBuildInsertValue(codegen->builder, nil_str,
        LLVMConstInt(LLVMInt64TypeInContext(codegen->context), 5, 0), 1, "edisp.nil.len");
    return LLVMBuildSelect(codegen->builder, is_null, nil_str, msg, "edisp.result");
}
#endif

// fmt_emit_segments: shared format-string walker used by Printf (sprintf_mode=0)
// and Sprintf (sprintf_mode=1).  For Printf mode it emits
// goo_print/goo_print_string/goo_print_int/goo_print_bool/goo_print_float calls
// for each literal text chunk and format verb.  For Sprintf mode it accumulates
// a goo_string_t result via goo_string_concat and writes it to *out_str.
//
// Parameters:
//   c            — the active CodeGenerator
//   tc           — the active TypeChecker
//   fmt_str      — the already-escape-processed format string (from LiteralNode->value)
//   args         — the ASTNode list of verb arguments (format arg NOT included)
//   sprintf_mode — 0 for Printf, 1 for Sprintf
//   out_str      — (sprintf_mode=1 only) receives the built goo_string_t
//   wrap_out     — non-NULL only for fmt.Errorf; enables the %w verb and
//                  receives the wrapped error's handle (the cause) if seen
//   call_pos     — source position of the call site, used for non-arg error messages
//
// Returns 1 on success, 0 after codegen_error on any mismatch.
#if LLVM_AVAILABLE
static int fmt_emit_segments(CodeGenerator* c, TypeChecker* tc,
                              const char* fmt_str, ASTNode* args,
                              int sprintf_mode, LLVMValueRef* out_str,
                              LLVMValueRef* wrap_out,
                              Position call_pos) {

    // -- Printf-mode runtime functions --
    LLVMValueRef print_fn       = NULL;
    LLVMValueRef print_str_fn   = NULL;
    LLVMValueRef print_int_fn   = NULL;
    LLVMValueRef print_bool_fn  = NULL;
    LLVMValueRef print_float_fn = NULL;

    // -- Sprintf-mode runtime functions + accumulator --
    LLVMValueRef concat_fn       = NULL;
    LLVMValueRef int_to_str_fn   = NULL;
    LLVMValueRef float_to_str_fn = NULL;
    LLVMValueRef bool_to_str_fn  = NULL;
    LLVMTypeRef  string_llvm     = NULL;
    LLVMValueRef acc             = NULL;  // current accumulated goo_string_t

    if (!sprintf_mode) {
        // Collect needed runtime functions for Printf once up front.
        print_fn       = LLVMGetNamedFunction(c->module, "goo_print");
        print_str_fn   = LLVMGetNamedFunction(c->module, "goo_print_string");
        print_int_fn   = LLVMGetNamedFunction(c->module, "goo_print_int");
        print_bool_fn  = LLVMGetNamedFunction(c->module, "goo_print_bool");
        print_float_fn = LLVMGetNamedFunction(c->module, "goo_print_float");

        if (!print_fn || !print_str_fn || !print_int_fn ||
            !print_bool_fn || !print_float_fn) {
            codegen_error(c, call_pos,
                          "fmt.Printf: required runtime print functions not found in module");
            return 0;
        }
    } else {
        // Collect to_string helpers and concat for Sprintf.
        concat_fn       = LLVMGetNamedFunction(c->module, "goo_string_concat");
        int_to_str_fn   = LLVMGetNamedFunction(c->module, "goo_int_to_string");
        float_to_str_fn = LLVMGetNamedFunction(c->module, "goo_float_to_string");
        bool_to_str_fn  = LLVMGetNamedFunction(c->module, "goo_bool_to_string");

        if (!concat_fn || !int_to_str_fn || !float_to_str_fn || !bool_to_str_fn) {
            codegen_error(c, call_pos,
                          "fmt.Sprintf: required runtime to_string functions not found in module");
            return 0;
        }

        // Initialize accumulator to an empty goo_string_t { "", 0 }.
        string_llvm = codegen_get_basic_type(c, TYPE_STRING);
        LLVMValueRef empty_ptr = LLVMBuildGlobalStringPtr(c->builder, "", "sp_empty");
        LLVMValueRef zero64 = LLVMConstInt(LLVMInt64TypeInContext(c->context), 0, 0);
        acc = LLVMGetUndef(string_llvm);
        acc = LLVMBuildInsertValue(c->builder, acc, empty_ptr, 0, "sp_acc_ptr");
        acc = LLVMBuildInsertValue(c->builder, acc, zero64, 1, "sp_acc_len");
    }

    size_t fmt_len = strlen(fmt_str);
    // chunk accumulates consecutive literal (non-verb) characters.
    // Escapes are already processed by the lexer so this is pure byte-copy.
    char* chunk = malloc(fmt_len + 1);
    if (!chunk) {
        codegen_error(c, call_pos,
                      sprintf_mode ? "fmt.Sprintf: out of memory"
                                   : "fmt.Printf: out of memory");
        return 0;
    }
    size_t chunk_len = 0;

    ASTNode* arg_cursor = args;  // next verb argument to consume
    int ok = 1;

    for (size_t i = 0; i <= fmt_len && ok; i++) {
        char ch = (i < fmt_len) ? fmt_str[i] : '\0';

        // Accumulate non-verb characters into the current literal chunk.
        if (ch != '%' && ch != '\0') {
            chunk[chunk_len++] = ch;
            continue;
        }

        // Flush the current literal chunk (if any).
        if (chunk_len > 0) {
            chunk[chunk_len] = '\0';
            if (!sprintf_mode) {
                // Printf: emit goo_print(cstr)
                LLVMValueRef cstr = LLVMBuildGlobalStringPtr(c->builder, chunk, "pf_chunk");
                LLVMValueRef pargs[] = { cstr };
                LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(print_fn),
                               print_fn, pargs, 1, "");
            } else {
                // Sprintf: build a goo_string_t from the literal chunk and concat.
                LLVMValueRef cstr = LLVMBuildGlobalStringPtr(c->builder, chunk, "sp_chunk");
                LLVMValueRef clen = LLVMConstInt(LLVMInt64TypeInContext(c->context),
                                                  chunk_len, 0);
                LLVMValueRef chunk_str = LLVMGetUndef(string_llvm);
                chunk_str = LLVMBuildInsertValue(c->builder, chunk_str, cstr, 0, "");
                chunk_str = LLVMBuildInsertValue(c->builder, chunk_str, clen, 1, "");
                LLVMValueRef cargs[] = { acc, chunk_str };
                acc = LLVMBuildCall2(c->builder,
                                     LLVMGlobalGetValueType(concat_fn),
                                     concat_fn, cargs, 2, "sp_acc");
            }
            chunk_len = 0;
        }

        if (ch == '\0') break;  // end of format string

        // ch == '%': consume the verb character.
        i++;
        if (i >= fmt_len) {
            codegen_error(c, call_pos,
                          sprintf_mode
                          ? "fmt.Sprintf: trailing '%%' with no verb at end of format string"
                          : "fmt.Printf: trailing '%%' with no verb at end of format string");
            ok = 0;
            break;
        }
        char verb = fmt_str[i];

        if (verb == '%') {
            // %% → emit a literal '%'.
            if (!sprintf_mode) {
                LLVMValueRef cstr = LLVMBuildGlobalStringPtr(c->builder, "%", "pf_pct");
                LLVMValueRef pargs[] = { cstr };
                LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(print_fn),
                               print_fn, pargs, 1, "");
            } else {
                LLVMValueRef cstr = LLVMBuildGlobalStringPtr(c->builder, "%", "sp_pct");
                LLVMValueRef one64 = LLVMConstInt(LLVMInt64TypeInContext(c->context), 1, 0);
                LLVMValueRef pct_str = LLVMGetUndef(string_llvm);
                pct_str = LLVMBuildInsertValue(c->builder, pct_str, cstr, 0, "");
                pct_str = LLVMBuildInsertValue(c->builder, pct_str, one64, 1, "");
                LLVMValueRef cargs[] = { acc, pct_str };
                acc = LLVMBuildCall2(c->builder,
                                     LLVMGlobalGetValueType(concat_fn),
                                     concat_fn, cargs, 2, "sp_acc");
            }
            continue;
        }

        // Every other verb consumes one argument from the caller's list.
        if (!arg_cursor) {
            codegen_error(c, call_pos,
                          sprintf_mode
                          ? "fmt.Sprintf: too few arguments for format string"
                          : "fmt.Printf: too few arguments for format string");
            ok = 0;
            break;
        }

        ValueInfo* arg_val = codegen_generate_expression(c, tc, arg_cursor);
        if (!arg_val) { ok = 0; break; }

        // Load through lvalue (field/index selectors arrive as address + is_lvalue).
        if (arg_val->is_lvalue && arg_val->goo_type) {
            LLVMTypeRef at = codegen_type_to_llvm(c, arg_val->goo_type);
            if (at) {
                arg_val->llvm_value = LLVMBuildLoad2(c->builder, at,
                                                      arg_val->llvm_value, "pf_load");
                arg_val->is_lvalue = 0;
            }
        }

        TypeKind kind = arg_val->goo_type ? arg_val->goo_type->kind : TYPE_VOID;

        if (verb == 'd') {
            // %d — signed decimal integer
            if (kind != TYPE_INT8 && kind != TYPE_INT16 &&
                kind != TYPE_INT32 && kind != TYPE_INT64) {
                codegen_error(c, arg_cursor->pos,
                              sprintf_mode ? "fmt.Sprintf: %%d requires an integer argument"
                                           : "fmt.Printf: %%d requires an integer argument");
                value_info_free(arg_val);
                ok = 0;
                break;
            }
            LLVMValueRef w = LLVMBuildSExt(c->builder, arg_val->llvm_value,
                                            LLVMInt64TypeInContext(c->context), "sext");
            if (!sprintf_mode) {
                LLVMValueRef pargs[] = { w };
                LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(print_int_fn),
                               print_int_fn, pargs, 1, "");
            } else {
                LLVMValueRef s_args[] = { w };
                LLVMValueRef s = LLVMBuildCall2(c->builder,
                                                LLVMGlobalGetValueType(int_to_str_fn),
                                                int_to_str_fn, s_args, 1, "sp_int");
                LLVMValueRef cargs[] = { acc, s };
                acc = LLVMBuildCall2(c->builder,
                                     LLVMGlobalGetValueType(concat_fn),
                                     concat_fn, cargs, 2, "sp_acc");
            }

        } else if (verb == 's') {
            // %s — string (length-aware, safe for substrings). An interface
            // argument is also accepted (v1: routed through goo_iface_format
            // identically to %v — no Stringer/%!s(...) semantics yet).
            if (kind == TYPE_INTERFACE) {
                LLVMValueRef ival = arg_val->llvm_value;   // {ptr vtable, ptr data}
                LLVMValueRef vtab = LLVMBuildExtractValue(c->builder, ival, 0, "ifvt");
                LLVMValueRef data = LLVMBuildExtractValue(c->builder, ival, 1, "ifdata");

                LLVMValueRef fmtcall = LLVMGetNamedFunction(c->module, "goo_iface_format");
                if (!fmtcall) {
                    codegen_error(c, arg_cursor->pos, "goo_iface_format not found");
                    value_info_free(arg_val);
                    ok = 0;
                    break;
                }
                LLVMValueRef fargs[] = { vtab, data };
                LLVMValueRef s = LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(fmtcall),
                                                fmtcall, fargs, 2, "ifstr");
                if (!sprintf_mode) {
                    LLVMValueRef pargs[] = { s };
                    LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(print_str_fn),
                                   print_str_fn, pargs, 1, "");
                } else {
                    LLVMValueRef cargs[] = { acc, s };
                    acc = LLVMBuildCall2(c->builder,
                                         LLVMGlobalGetValueType(concat_fn),
                                         concat_fn, cargs, 2, "sp_acc");
                }
                value_info_free(arg_val);
                arg_cursor = arg_cursor->next;
                continue;
            }
            if (kind != TYPE_STRING) {
                codegen_error(c, arg_cursor->pos,
                              sprintf_mode ? "fmt.Sprintf: %%s requires a string argument"
                                           : "fmt.Printf: %%s requires a string argument");
                value_info_free(arg_val);
                ok = 0;
                break;
            }
            if (!sprintf_mode) {
                LLVMValueRef pargs[] = { arg_val->llvm_value };
                LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(print_str_fn),
                               print_str_fn, pargs, 1, "");
            } else {
                // %s: the arg is already a goo_string_t — concat directly.
                LLVMValueRef cargs[] = { acc, arg_val->llvm_value };
                acc = LLVMBuildCall2(c->builder,
                                     LLVMGlobalGetValueType(concat_fn),
                                     concat_fn, cargs, 2, "sp_acc");
            }

        } else if (verb == 'f') {
            // %f — floating-point
            if (kind != TYPE_FLOAT32 && kind != TYPE_FLOAT64) {
                codegen_error(c, arg_cursor->pos,
                              sprintf_mode ? "fmt.Sprintf: %%f requires a float argument"
                                           : "fmt.Printf: %%f requires a float argument");
                value_info_free(arg_val);
                ok = 0;
                break;
            }
            LLVMValueRef w = (kind == TYPE_FLOAT32)
                ? LLVMBuildFPExt(c->builder, arg_val->llvm_value,
                                  LLVMDoubleTypeInContext(c->context), "fpext")
                : arg_val->llvm_value;
            if (!sprintf_mode) {
                LLVMValueRef pargs[] = { w };
                LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(print_float_fn),
                               print_float_fn, pargs, 1, "");
            } else {
                LLVMValueRef s_args[] = { w };
                LLVMValueRef s = LLVMBuildCall2(c->builder,
                                                LLVMGlobalGetValueType(float_to_str_fn),
                                                float_to_str_fn, s_args, 1, "sp_float");
                LLVMValueRef cargs[] = { acc, s };
                acc = LLVMBuildCall2(c->builder,
                                     LLVMGlobalGetValueType(concat_fn),
                                     concat_fn, cargs, 2, "sp_acc");
            }

        } else if (verb == 't') {
            // %t — boolean ("true"/"false")
            if (kind != TYPE_BOOL) {
                codegen_error(c, arg_cursor->pos,
                              sprintf_mode ? "fmt.Sprintf: %%t requires a bool argument"
                                           : "fmt.Printf: %%t requires a bool argument");
                value_info_free(arg_val);
                ok = 0;
                break;
            }
            LLVMValueRef w = LLVMBuildZExt(c->builder, arg_val->llvm_value,
                                            LLVMInt32TypeInContext(c->context), "zext");
            if (!sprintf_mode) {
                LLVMValueRef pargs[] = { w };
                LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(print_bool_fn),
                               print_bool_fn, pargs, 1, "");
            } else {
                LLVMValueRef s_args[] = { w };
                LLVMValueRef s = LLVMBuildCall2(c->builder,
                                                LLVMGlobalGetValueType(bool_to_str_fn),
                                                bool_to_str_fn, s_args, 1, "sp_bool");
                LLVMValueRef cargs[] = { acc, s };
                acc = LLVMBuildCall2(c->builder,
                                     LLVMGlobalGetValueType(concat_fn),
                                     concat_fn, cargs, 2, "sp_acc");
            }

        } else if (verb == 'v') {
            // %v — default format: dispatch on arg kind, matching Println's dispatch.
            if (type_is_error(arg_val->goo_type)) {
                // error: not one of the primitive kinds below, so it must be
                // special-cased first — same display shape as Println's error
                // case (Task 4), shared via codegen_error_display_string.
                LLVMValueRef disp = codegen_error_display_string(c, arg_val->llvm_value, arg_cursor->pos);
                if (!disp) { value_info_free(arg_val); ok = 0; break; }
                if (!sprintf_mode) {
                    LLVMValueRef pargs[] = { disp };
                    LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(print_str_fn),
                                   print_str_fn, pargs, 1, "");
                } else {
                    LLVMValueRef cargs[] = { acc, disp };
                    acc = LLVMBuildCall2(c->builder,
                                         LLVMGlobalGetValueType(concat_fn),
                                         concat_fn, cargs, 2, "sp_acc");
                }
            } else if (kind == TYPE_STRING) {
                if (!sprintf_mode) {
                    LLVMValueRef pargs[] = { arg_val->llvm_value };
                    LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(print_str_fn),
                                   print_str_fn, pargs, 1, "");
                } else {
                    LLVMValueRef cargs[] = { acc, arg_val->llvm_value };
                    acc = LLVMBuildCall2(c->builder,
                                         LLVMGlobalGetValueType(concat_fn),
                                         concat_fn, cargs, 2, "sp_acc");
                }
            } else if (kind == TYPE_INT8 || kind == TYPE_INT16 ||
                       kind == TYPE_INT32 || kind == TYPE_INT64) {
                LLVMValueRef w = LLVMBuildSExt(c->builder, arg_val->llvm_value,
                                                LLVMInt64TypeInContext(c->context), "sext");
                if (!sprintf_mode) {
                    LLVMValueRef pargs[] = { w };
                    LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(print_int_fn),
                                   print_int_fn, pargs, 1, "");
                } else {
                    LLVMValueRef s_args[] = { w };
                    LLVMValueRef s = LLVMBuildCall2(c->builder,
                                                    LLVMGlobalGetValueType(int_to_str_fn),
                                                    int_to_str_fn, s_args, 1, "sp_int");
                    LLVMValueRef cargs[] = { acc, s };
                    acc = LLVMBuildCall2(c->builder,
                                         LLVMGlobalGetValueType(concat_fn),
                                         concat_fn, cargs, 2, "sp_acc");
                }
            } else if (kind == TYPE_BOOL) {
                LLVMValueRef w = LLVMBuildZExt(c->builder, arg_val->llvm_value,
                                                LLVMInt32TypeInContext(c->context), "zext");
                if (!sprintf_mode) {
                    LLVMValueRef pargs[] = { w };
                    LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(print_bool_fn),
                                   print_bool_fn, pargs, 1, "");
                } else {
                    LLVMValueRef s_args[] = { w };
                    LLVMValueRef s = LLVMBuildCall2(c->builder,
                                                    LLVMGlobalGetValueType(bool_to_str_fn),
                                                    bool_to_str_fn, s_args, 1, "sp_bool");
                    LLVMValueRef cargs[] = { acc, s };
                    acc = LLVMBuildCall2(c->builder,
                                         LLVMGlobalGetValueType(concat_fn),
                                         concat_fn, cargs, 2, "sp_acc");
                }
            } else if (kind == TYPE_FLOAT32 || kind == TYPE_FLOAT64) {
                LLVMValueRef w = (kind == TYPE_FLOAT32)
                    ? LLVMBuildFPExt(c->builder, arg_val->llvm_value,
                                      LLVMDoubleTypeInContext(c->context), "fpext")
                    : arg_val->llvm_value;
                if (!sprintf_mode) {
                    LLVMValueRef pargs[] = { w };
                    LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(print_float_fn),
                                   print_float_fn, pargs, 1, "");
                } else {
                    LLVMValueRef s_args[] = { w };
                    LLVMValueRef s = LLVMBuildCall2(c->builder,
                                                    LLVMGlobalGetValueType(float_to_str_fn),
                                                    float_to_str_fn, s_args, 1, "sp_float");
                    LLVMValueRef cargs[] = { acc, s };
                    acc = LLVMBuildCall2(c->builder,
                                         LLVMGlobalGetValueType(concat_fn),
                                         concat_fn, cargs, 2, "sp_acc");
                }
            } else if (kind == TYPE_INTERFACE) {
                // Interface value: {vtable,data} -> goo_iface_format (runtime
                // helper: nil vtable -> "<nil>", else vtable[0]=desc,
                // desc.fmt_fn(data)) — same extraction as Task 2's Println arm.
                LLVMValueRef ival = arg_val->llvm_value;   // {ptr vtable, ptr data}
                LLVMValueRef vtab = LLVMBuildExtractValue(c->builder, ival, 0, "ifvt");
                LLVMValueRef data = LLVMBuildExtractValue(c->builder, ival, 1, "ifdata");

                LLVMValueRef fmtcall = LLVMGetNamedFunction(c->module, "goo_iface_format");
                if (!fmtcall) {
                    codegen_error(c, arg_cursor->pos, "goo_iface_format not found");
                    value_info_free(arg_val);
                    ok = 0;
                    break;
                }
                LLVMValueRef fargs[] = { vtab, data };
                LLVMValueRef s = LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(fmtcall),
                                                fmtcall, fargs, 2, "ifstr");
                if (!sprintf_mode) {
                    LLVMValueRef pargs[] = { s };
                    LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(print_str_fn),
                                   print_str_fn, pargs, 1, "");
                } else {
                    LLVMValueRef cargs[] = { acc, s };
                    acc = LLVMBuildCall2(c->builder,
                                         LLVMGlobalGetValueType(concat_fn),
                                         concat_fn, cargs, 2, "sp_acc");
                }
            } else if (kind == TYPE_STRUCT ||
                       (kind == TYPE_POINTER && arg_val->goo_type &&
                        arg_val->goo_type->data.pointer.pointee_type &&
                        arg_val->goo_type->data.pointer.pointee_type->kind == TYPE_STRUCT)) {
                // %v of a struct / pointer-to-struct. Printf mode reuses the
                // recursive print-based formatter shared with fmt.Println
                // (codegen_emit_fmt_value -> {f0 f1} / &{...}). Sprintf mode
                // uses the string-building counterpart (codegen_build_fmt_value_string)
                // since that helper PRINTS rather than accumulating a goo_string.
                if (sprintf_mode) {
                    acc = codegen_build_fmt_value_string(c, tc, acc, arg_val->llvm_value,
                                                          arg_val->goo_type, 0, arg_cursor->pos);
                    if (!acc) {
                        value_info_free(arg_val);
                        ok = 0;
                        break;
                    }
                } else if (!codegen_emit_fmt_value(c, tc, arg_val->llvm_value,
                                            arg_val->goo_type, 0, arg_cursor->pos)) {
                    value_info_free(arg_val);
                    ok = 0;
                    break;
                }
            } else {
                codegen_error(c, arg_cursor->pos,
                              sprintf_mode
                              ? "fmt.Sprintf: %%v: unsupported argument type "
                                "(only string, integer, bool, float, struct, "
                                "pointer-to-struct supported in v1)"
                              : "fmt.Printf: %%v: unsupported argument type "
                                "(only string, integer, bool, float, struct, "
                                "pointer-to-struct supported in v1)");
                value_info_free(arg_val);
                ok = 0;
                break;
            }

        } else if (verb == 'w') {
            // %w — wrap an error (fmt.Errorf only). Renders the wrapped error's
            // message like %v AND records its handle as the cause via wrap_out.
            if (!wrap_out) {
                codegen_error(c, arg_cursor->pos, "fmt: %%w is only valid in fmt.Errorf");
                value_info_free(arg_val); ok = 0; break;
            }
            if (!type_is_error(arg_val->goo_type)) {
                codegen_error(c, arg_cursor->pos, "fmt.Errorf: %%w requires an error argument");
                value_info_free(arg_val); ok = 0; break;
            }
            if (*wrap_out) {
                codegen_error(c, arg_cursor->pos, "fmt.Errorf: multiple %%w not supported (v1)");
                value_info_free(arg_val); ok = 0; break;
            }
            LLVMValueRef disp = codegen_error_display_string(c, arg_val->llvm_value, arg_cursor->pos);
            if (!disp) { value_info_free(arg_val); ok = 0; break; }
            LLVMValueRef cargs[] = { acc, disp };
            acc = LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(concat_fn), concat_fn, cargs, 2, "sp_acc");
            // Record the wrapped error's handle (field 1 of the loaded nullable) as the cause.
            *wrap_out = LLVMBuildExtractValue(c->builder, arg_val->llvm_value, 1, "w.cause");

        } else {
            codegen_error(c, call_pos,
                          sprintf_mode
                          ? "fmt.Sprintf: unknown format verb '%%%c'"
                          : "fmt.Printf: unknown format verb '%%%c'",
                          verb);
            value_info_free(arg_val);
            ok = 0;
            break;
        }

        value_info_free(arg_val);
        arg_cursor = arg_cursor->next;
    }

    free(chunk);

    if (!ok) return 0;

    // Reject excess arguments not consumed by any verb.
    if (arg_cursor) {
        codegen_error(c, arg_cursor->pos,
                      sprintf_mode
                      ? "fmt.Sprintf: too many arguments for format string"
                      : "fmt.Printf: too many arguments for format string");
        return 0;
    }

    if (sprintf_mode) *out_str = acc;
    return 1;
}
#endif  /* LLVM_AVAILABLE */

// codegen_generate_printf_call: entry point for fmt.Printf.  Validates that the
// first argument is a compile-time string literal, then delegates to the shared
// fmt_emit_segments walker (sprintf_mode=0) to emit goo_print* calls for each
// literal chunk and format verb.
static ValueInfo* codegen_generate_printf_call(CodeGenerator* codegen,
                                                TypeChecker* checker,
                                                ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;

    CallExprNode* call = (CallExprNode*)expr;

    // First argument must be a string literal — Printf is a compile-time
    // format walker, not a runtime sprintf.
    ASTNode* fmt_arg = call->args;
    if (!fmt_arg ||
        fmt_arg->type != AST_LITERAL ||
        ((LiteralNode*)fmt_arg)->literal_type != TOKEN_STRING) {
        codegen_error(codegen, expr->pos,
                      "fmt.Printf: format must be a string literal");
        return NULL;
    }

    const char* fmt_str = ((LiteralNode*)fmt_arg)->value;

    // Walk the format string, emitting print calls for each segment.
    // Pass fmt_arg->next so the walker sees only the verb arguments.
    if (!fmt_emit_segments(codegen, checker, fmt_str, fmt_arg->next, 0, NULL, NULL, expr->pos)) {
        return NULL;
    }

    return value_info_new(NULL, NULL, type_checker_get_builtin(checker, TYPE_VOID));
#endif
}

// codegen_generate_sprintf_call: entry point for fmt.Sprintf.  Validates that
// the first argument is a compile-time string literal, then delegates to the
// shared fmt_emit_segments walker (sprintf_mode=1) to accumulate a goo_string_t
// result via goo_string_concat.  Returns a ValueInfo of TYPE_STRING.
static ValueInfo* codegen_generate_sprintf_call(CodeGenerator* codegen,
                                                 TypeChecker* checker,
                                                 ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;

    CallExprNode* call = (CallExprNode*)expr;

    // First argument must be a string literal — Sprintf is a compile-time
    // format walker that builds the string at codegen time, not at runtime.
    ASTNode* fmt_arg = call->args;
    if (!fmt_arg ||
        fmt_arg->type != AST_LITERAL ||
        ((LiteralNode*)fmt_arg)->literal_type != TOKEN_STRING) {
        codegen_error(codegen, expr->pos,
                      "fmt.Sprintf: format must be a string literal");
        return NULL;
    }

    const char* fmt_str = ((LiteralNode*)fmt_arg)->value;

    // Walk the format string, accumulating a goo_string_t in sprintf_mode=1.
    LLVMValueRef result = NULL;
    if (!fmt_emit_segments(codegen, checker, fmt_str, fmt_arg->next, 1, &result, NULL, expr->pos)) {
        return NULL;
    }

    return value_info_new(NULL, result, type_checker_get_builtin(checker, TYPE_STRING));
#endif
}

// fmt.Sprint(args...) -> string. The string-returning counterpart to fmt.Print:
// operands are formatted via the string-building helper
// codegen_build_fmt_value_string (scalars, structs, pointers-to-struct), with
// Go's Sprint spacing rule — a space is inserted between two adjacent operands
// only when NEITHER is a string — and no trailing newline.
static ValueInfo* codegen_generate_fmt_sprint_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;
    CallExprNode* call = (CallExprNode*)expr;

    LLVMValueRef concat_fn = LLVMGetNamedFunction(codegen->module, "goo_string_concat");
    if (!concat_fn) { codegen_error(codegen, expr->pos, "goo_string_concat not found in module"); return NULL; }
    LLVMTypeRef string_llvm = codegen_get_basic_type(codegen, TYPE_STRING);

    LLVMValueRef acc = fmt_sprintf_lit(codegen, string_llvm, "");  // empty goo_string
    int first = 1, prev_is_string = 0;
    for (ASTNode* a = call->args; a; a = a->next) {
        ValueInfo* arg_val = codegen_generate_expression(codegen, checker, a);
        if (!arg_val) { codegen_error(codegen, expr->pos, "Failed to generate argument for fmt.Sprint"); return NULL; }
        if (arg_val->is_lvalue && arg_val->goo_type) {
            LLVMTypeRef at = codegen_type_to_llvm(codegen, arg_val->goo_type);
            if (at) { arg_val->llvm_value = LLVMBuildLoad2(codegen->builder, at, arg_val->llvm_value, "sprint_arg"); arg_val->is_lvalue = 0; }
        }
        int is_string = arg_val->goo_type && arg_val->goo_type->kind == TYPE_STRING;
        if (!first && !prev_is_string && !is_string) {
            LLVMValueRef sp = fmt_sprintf_lit(codegen, string_llvm, " ");
            LLVMValueRef ca[] = { acc, sp };
            acc = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn), concat_fn, ca, 2, "sprint_sep");
        }
        acc = codegen_build_fmt_value_string(codegen, checker, acc, arg_val->llvm_value, arg_val->goo_type, 0, a->pos);
        if (!acc) { value_info_free(arg_val); return NULL; }
        prev_is_string = is_string; first = 0;
        value_info_free(arg_val);
    }
    return value_info_new(NULL, acc, type_checker_get_builtin(checker, TYPE_STRING));
#endif
}

// fmt.Sprintln(args...) -> string. The string-returning counterpart to
// fmt.Println: operands are ALWAYS space-separated with a trailing newline.
static ValueInfo* codegen_generate_fmt_sprintln_call(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;
    CallExprNode* call = (CallExprNode*)expr;

    LLVMValueRef concat_fn = LLVMGetNamedFunction(codegen->module, "goo_string_concat");
    if (!concat_fn) { codegen_error(codegen, expr->pos, "goo_string_concat not found in module"); return NULL; }
    LLVMTypeRef string_llvm = codegen_get_basic_type(codegen, TYPE_STRING);

    LLVMValueRef acc = fmt_sprintf_lit(codegen, string_llvm, "");
    int first = 1;
    for (ASTNode* a = call->args; a; a = a->next) {
        ValueInfo* arg_val = codegen_generate_expression(codegen, checker, a);
        if (!arg_val) { codegen_error(codegen, expr->pos, "Failed to generate argument for fmt.Sprintln"); return NULL; }
        if (arg_val->is_lvalue && arg_val->goo_type) {
            LLVMTypeRef at = codegen_type_to_llvm(codegen, arg_val->goo_type);
            if (at) { arg_val->llvm_value = LLVMBuildLoad2(codegen->builder, at, arg_val->llvm_value, "sprintln_arg"); arg_val->is_lvalue = 0; }
        }
        if (!first) {
            LLVMValueRef sp = fmt_sprintf_lit(codegen, string_llvm, " ");
            LLVMValueRef ca[] = { acc, sp };
            acc = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn), concat_fn, ca, 2, "sprintln_sep");
        }
        acc = codegen_build_fmt_value_string(codegen, checker, acc, arg_val->llvm_value, arg_val->goo_type, 0, a->pos);
        if (!acc) { value_info_free(arg_val); return NULL; }
        first = 0;
        value_info_free(arg_val);
    }
    LLVMValueRef nl = fmt_sprintf_lit(codegen, string_llvm, "\n");
    LLVMValueRef ca[] = { acc, nl };
    acc = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(concat_fn), concat_fn, ca, 2, "sprintln_nl");
    return value_info_new(NULL, acc, type_checker_get_builtin(checker, TYPE_STRING));
#endif
}

// codegen_generate_errorf_call: entry point for fmt.Errorf. Mirrors
// codegen_generate_sprintf_call (compile-time format walker via
// fmt_emit_segments, sprintf_mode=1) to build a goo_string_t message, then
// boxes it into a heap goo_error via goo_error_from_string and wraps the
// resulting handle into the nullable error representation, matching the
// errors.New boxing path.
static ValueInfo* codegen_generate_errorf_call(CodeGenerator* codegen,
                                               TypeChecker* checker,
                                               ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;
    CallExprNode* call = (CallExprNode*)expr;
    ASTNode* fmt_arg = call->args;
    if (!fmt_arg || fmt_arg->type != AST_LITERAL ||
        ((LiteralNode*)fmt_arg)->literal_type != TOKEN_STRING) {
        codegen_error(codegen, expr->pos, "fmt.Errorf: format must be a string literal");
        return NULL;
    }
    const char* fmt_str = ((LiteralNode*)fmt_arg)->value;
    LLVMValueRef msg_str = NULL;
    LLVMValueRef cause = NULL;
    if (!fmt_emit_segments(codegen, checker, fmt_str, fmt_arg->next, 1, &msg_str, &cause, expr->pos)) {
        return NULL;
    }
    LLVMValueRef handle;
    if (cause) {
        LLVMValueRef wrap_fn = LLVMGetNamedFunction(codegen->module, "goo_error_wrap");
        if (!wrap_fn) { codegen_error(codegen, expr->pos, "goo_error_wrap not found in module"); return NULL; }
        LLVMValueRef wargs[] = { msg_str, cause };
        handle = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(wrap_fn), wrap_fn, wargs, 2, "errorf_wrap");
    } else {
        LLVMValueRef from_str = LLVMGetNamedFunction(codegen->module, "goo_error_from_string");
        if (!from_str) { codegen_error(codegen, expr->pos, "goo_error_from_string not found in module"); return NULL; }
        LLVMValueRef bargs[] = { msg_str };
        handle = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(from_str), from_str, bargs, 1, "errorf_box");
    }
    Type* err_type = type_checker_error_type(checker);
    LLVMTypeRef err_llvm = codegen_type_to_llvm(codegen, err_type);
    LLVMValueRef err_val = LLVMGetUndef(err_llvm);
    err_val = LLVMBuildInsertValue(codegen->builder, err_val,
        LLVMConstInt(LLVMInt1TypeInContext(codegen->context), 0, 0), 0, "ef.is_null");
    err_val = LLVMBuildInsertValue(codegen->builder, err_val, handle, 1, "ef.ptr");
    return value_info_new(NULL, err_val, err_type);
#endif
}
