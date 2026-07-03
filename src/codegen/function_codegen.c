#include "codegen.h"
#include "comptime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Function and declaration code generation

// ---- M8b: goroutine-escape pre-pass (file-static; function codegen is
// non-reentrant and Goo has no nested function decls) --------------------------

#define ESCAPE_MAX_NAMES 256
static const char* g_escape_names[ESCAPE_MAX_NAMES];
static size_t g_escape_count;
static int g_escape_has_go;

// Root local name of an lvalue whose address is taken: descend through selector
// (`x.f`) and index (`x[i]`) to the underlying identifier. (Parens are folded
// away by the parser — there is no paren-expr node — and AST_PAREN_EXPR is a
// repurposed slot for map literals, which are not addressable lvalues.)
static const char* escape_root_local(ASTNode* lv) {
    while (lv) {
        switch (lv->type) {
            case AST_IDENTIFIER: return ((IdentifierNode*)lv)->name;
            case AST_SELECTOR_EXPR: lv = ((SelectorExprNode*)lv)->expr; break;
            case AST_INDEX_EXPR:    lv = ((IndexExprNode*)lv)->expr; break;
            default: return NULL;
        }
    }
    return NULL;
}

static void escape_add(const char* name) {
    if (!name) return;
    for (size_t i = 0; i < g_escape_count; i++)
        if (strcmp(g_escape_names[i], name) == 0) return;  // dedup
    if (g_escape_count < ESCAPE_MAX_NAMES)
        g_escape_names[g_escape_count++] = name;
}

// Recursively visit a node and its `next`-chained siblings, recording any `&`
// address-of root local and noting whether a `go` statement appears.
static void escape_walk(ASTNode* n) {
    for (; n; n = n->next) {
        switch (n->type) {
            case AST_GO_STMT: {
                g_escape_has_go = 1;
                escape_walk(((GoStmtNode*)n)->call);
                break;
            }
            case AST_UNARY_EXPR: {
                UnaryExprNode* u = (UnaryExprNode*)n;
                if (u->operator == TOKEN_BIT_AND)
                    escape_add(escape_root_local(u->operand));
                escape_walk(u->operand);
                break;
            }
            case AST_BLOCK_STMT: escape_walk(((BlockStmtNode*)n)->statements); break;
            case AST_EXPR_STMT:  escape_walk(((ExprStmtNode*)n)->expr); break;
            case AST_IF_STMT: {
                IfStmtNode* s = (IfStmtNode*)n;
                escape_walk(s->condition); escape_walk(s->then_stmt); escape_walk(s->else_stmt);
                break;
            }
            case AST_IF_LET_STMT: {
                IfLetStmtNode* s = (IfLetStmtNode*)n;
                escape_walk(s->nullable_expr); escape_walk(s->then_stmt); escape_walk(s->else_stmt);
                break;
            }
            case AST_FOR_STMT: {
                ForStmtNode* s = (ForStmtNode*)n;
                escape_walk(s->init); escape_walk(s->condition); escape_walk(s->post);
                escape_walk(s->range_expr); escape_walk(s->body);
                break;
            }
            case AST_RETURN_STMT: escape_walk(((ReturnStmtNode*)n)->values); break;
            case AST_VAR_DECL:    escape_walk(((VarDeclNode*)n)->values); break;
            case AST_DEFER_STMT:  escape_walk(((DeferStmtNode*)n)->call); break;
            case AST_BINARY_EXPR: {
                BinaryExprNode* b = (BinaryExprNode*)n;
                escape_walk(b->left); escape_walk(b->right);
                break;
            }
            case AST_POSTFIX_EXPR: escape_walk(((PostfixExprNode*)n)->operand); break;
            // (AST_PAREN_EXPR is a repurposed map-literal slot, not a wrapper —
            // a map-literal value position holding `&x` is rare; left to the
            // default no-op, which only ever under-promotes, never wrongly.)
            case AST_CALL_EXPR: {
                CallExprNode* c = (CallExprNode*)n;
                escape_walk(c->function); escape_walk(c->args);
                break;
            }
            case AST_INDEX_EXPR: {
                IndexExprNode* ix = (IndexExprNode*)n;
                escape_walk(ix->expr); escape_walk(ix->index);
                break;
            }
            case AST_SELECTOR_EXPR: escape_walk(((SelectorExprNode*)n)->expr); break;
            case AST_SWITCH_STMT: {
                SwitchStmtNode* s = (SwitchStmtNode*)n;
                escape_walk(s->tag); escape_walk(s->cases);
                break;
            }
            case AST_CASE_CLAUSE: {
                CaseClauseNode* c = (CaseClauseNode*)n;
                escape_walk(c->exprs); escape_walk(c->body);
                break;
            }
            case AST_SELECT_STMT: escape_walk(((SelectStmtNode*)n)->cases); break;
            case AST_SELECT_CASE: {
                SelectCaseNode* c = (SelectCaseNode*)n;
                escape_walk(c->comm); escape_walk(c->body);
                break;
            }
            // match expression: scrutinee + list of MatchCaseNode siblings
            case AST_MATCH_EXPR: {
                MatchExprNode* m = (MatchExprNode*)n;
                escape_walk(m->expr); escape_walk(m->cases);
                break;
            }
            // match case: pattern, optional guard, body
            case AST_MATCH_CASE: {
                MatchCaseNode* mc = (MatchCaseNode*)n;
                escape_walk(mc->pattern); escape_walk(mc->guard); escape_walk(mc->body);
                break;
            }
            // try/catch: walk the inner expression and, for catch, the catch body
            case AST_TRY_EXPR:  escape_walk(((TryExprNode*)n)->expr); break;
            case AST_CATCH_EXPR: {
                CatchExprNode* ce = (CatchExprNode*)n;
                escape_walk(ce->expr); escape_walk(ce->catch_body);
                break;
            }
            // unsafe block: recurse into its body
            case AST_UNSAFE_STMT: escape_walk(((UnsafeStmtNode*)n)->body); break;
            // slice literal [e1, e2, …]: recurse into element list
            case AST_SLICE_EXPR: escape_walk(((SliceLitNode*)n)->elements); break;
            // struct literal: recurse into field value expressions so &x inside
            // Box{p: &x} is detected and x is promoted.
            case AST_STRUCT_LITERAL: escape_walk(((StructLiteralNode*)n)->field_values); break;
            // defensive: parser currently emits AST_UNARY_EXPR for & but guard
            // against a future AST_ADDR_OF path reaching here.
            case AST_ADDR_OF: {
                AddrOfNode* a = (AddrOfNode*)n;
                escape_add(escape_root_local(a->operand));
                escape_walk(a->operand);
                break;
            }
            default: break;  // leaves (identifier, literal, types): nothing to recurse
        }
    }
}

static void escape_prepass_compute(ASTNode* body) {
    g_escape_count = 0;
    g_escape_has_go = 0;
    escape_walk(body);
    if (!g_escape_has_go) g_escape_count = 0;  // promote only in go-containing functions
}

static int escape_is_promoted(const char* name) {
    if (!name) return 0;
    for (size_t i = 0; i < g_escape_count; i++)
        if (strcmp(g_escape_names[i], name) == 0) return 1;
    return 0;
}

// Allocate storage for a named local: heap (goo_alloc, leaked) if the local is
// goroutine-escape-promoted, else a stack entry alloca. Under opaque pointers
// both return `ptr`, so all downstream loads/stores (which carry explicit types)
// are unchanged.
LLVMValueRef codegen_alloc_local(CodeGenerator* codegen, LLVMTypeRef type, const char* name) {
    if (!escape_is_promoted(name))
        return codegen_create_entry_alloca(codegen, type, name);

    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef i64t = LLVMInt64TypeInContext(ctx);
    LLVMTypeRef vp = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMTypeRef alloc_ty = LLVMFunctionType(vp, &i64t, 1, 0);
    LLVMValueRef alloc_fn = LLVMGetNamedFunction(codegen->module, "goo_alloc");
    if (!alloc_fn) alloc_fn = LLVMAddFunction(codegen->module, "goo_alloc", alloc_ty);

    // Emit the goo_alloc in the entry block (like an alloca) so it dominates all
    // uses and runs once per call. Save/restore the builder position.
    LLVMBasicBlockRef cur = LLVMGetInsertBlock(codegen->builder);
    LLVMBasicBlockRef entry = codegen->current_function_info
        ? codegen->current_function_info->entry_block
        : LLVMGetEntryBasicBlock(codegen->current_function);
    LLVMValueRef first = entry ? LLVMGetFirstInstruction(entry) : NULL;
    if (first) LLVMPositionBuilderBefore(codegen->builder, first);
    else       LLVMPositionBuilderAtEnd(codegen->builder, entry);

    LLVMValueRef size = LLVMSizeOf(type);
    LLVMValueRef p = LLVMBuildCall2(codegen->builder, alloc_ty, alloc_fn, &size, 1,
                                    name ? name : "go_escape_local");
    if (cur) LLVMPositionBuilderAtEnd(codegen->builder, cur);
    return p;
}

// Get-or-create the value-thunk for named function `name` (Goo type
// `fn_type`, LLVM global `named_fn`): `<name>.__thunk(env, params...) =
// named_fn(params...)`. `env` (thunk param 0) is ignored — a named function
// captures nothing — but MUST be present and FIRST: every indirect call site
// (codegen_generate_call_expr, call_codegen.c) calls through the universal
// `{ fn_ptr, env_ptr }` pair as `fn_ptr(env_ptr, args...)`, so any callable
// stored in that pair — including a bare named function's thunk — must share
// this exact ABI (env-FIRST is a change-together contract Branch B's
// closures build on unseen; see docs/superpowers/specs/
// 2026-07-03-closures-design.md "Representation").
//
// Mirrors two established get-or-create-thunk conventions in this codebase:
// PR #30's goroutine thunk (statement_codegen.c's codegen_generate_go_stmt —
// per-call-site synthesis, block save/restore) and interface_codegen.c's
// build_thunk (the closer structural analog — a reusable, name-cached,
// get-or-create thunk). interface_codegen.c itself is not modified by this
// task (outside its file allowlist); only its convention is mirrored here.
//
// Cached via LLVMGetNamedFunction on the thunk's own symbol name — idempotent:
// a second call for the same function returns the existing thunk, matching
// the spec's "once per (function, module)" requirement. The thunk's base
// name is taken from named_fn's OWN LLVM symbol (not the bare `name` param)
// so a package-mangled function (goo_pkg__<pkg>__<base>) gets a correctly
// disambiguated thunk instead of colliding with a same-named function in
// another package; `name` is used only as a defensive fallback if that
// lookup is empty.
LLVMValueRef codegen_get_func_thunk(CodeGenerator* codegen, TypeChecker* checker,
                                    Type* fn_type, LLVMValueRef named_fn,
                                    const char* name) {
#if !LLVM_AVAILABLE
    (void)checker;
    (void)fn_type;
    (void)named_fn;
    (void)name;
    return NULL;
#else
    (void)checker;  // no re-type-checking needed inside a thunk body
    if (!codegen || !fn_type || fn_type->kind != TYPE_FUNCTION || !named_fn || !name) return NULL;

    size_t base_len = 0;
    const char* llvm_name = LLVMGetValueName2(named_fn, &base_len);
    const char* base_name = (llvm_name && base_len > 0) ? llvm_name : name;

    char thunk_name[256];
    snprintf(thunk_name, sizeof(thunk_name), "%s.__thunk", base_name);
    LLVMValueRef existing = LLVMGetNamedFunction(codegen->module, thunk_name);
    if (existing) return existing;

    LLVMTypeRef thunk_ty = codegen_get_funcval_call_type(codegen, fn_type);
    LLVMTypeRef named_fn_ty = codegen_get_function_type(codegen, fn_type);
    if (!thunk_ty || !named_fn_ty) return NULL;

    LLVMValueRef thunk = LLVMAddFunction(codegen->module, thunk_name, thunk_ty);
    LLVMSetLinkage(thunk, LLVMInternalLinkage);

    // Emit the thunk body, saving/restoring the outer insert point.
    LLVMBasicBlockRef saved = LLVMGetInsertBlock(codegen->builder);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(codegen->context, thunk, "entry");
    LLVMPositionBuilderAtEnd(codegen->builder, entry);

    // Thunk param 0 is env (ignored); the wrapped function's real params
    // start at thunk param index 1.
    size_t np = fn_type->data.function.param_count;
    LLVMValueRef* call_args = np ? malloc(sizeof(LLVMValueRef) * np) : NULL;
    for (size_t i = 0; i < np; i++) call_args[i] = LLVMGetParam(thunk, (unsigned)(i + 1));

    LLVMTypeRef ret_llvm = LLVMGetReturnType(thunk_ty);
    int is_void = LLVMGetTypeKind(ret_llvm) == LLVMVoidTypeKind;
    LLVMValueRef call = LLVMBuildCall2(codegen->builder, named_fn_ty, named_fn,
                                       call_args, (unsigned)np, is_void ? "" : "thunk_call");
    free(call_args);

    if (is_void) {
        LLVMBuildRetVoid(codegen->builder);
    } else {
        LLVMBuildRet(codegen->builder, call);
    }

    if (saved) LLVMPositionBuilderAtEnd(codegen->builder, saved);
    return thunk;
#endif
}

// Forward declaration for error union function generation
int codegen_generate_error_union_function(CodeGenerator* codegen, TypeChecker* checker, 
                                         FuncDeclNode* func_decl, Type* return_type);

#if LLVM_AVAILABLE
// Prototype pre-pass (forward-reference support): declare a plain function's
// LLVM prototype in the module BEFORE any body is emitted, so a call to a
// function defined later in the file/package resolves — call sites look the
// callee up with LLVMGetNamedFunction (see expression_codegen.c), so the
// prototype merely needs to exist. This mirrors the type checker's
// hoist_function_signatures. The symbol-name mangling and the LLVM function-type
// construction deliberately parallel codegen_generate_function_decl below (which
// now find-or-creates the same prototype, then fills in the body); keeping them
// in step is what makes the two passes agree on name and signature.
//
// Error-union functions are skipped: their prototype is built by
// codegen_generate_error_union_function when the decl is reached, and no plain
// leaf package forward-references one (that case is deferred). Returns 1 on
// success (including the skip), 0 on failure.
static int codegen_predeclare_function(CodeGenerator* codegen, TypeChecker* checker,
                                       FuncDeclNode* func_decl) {
    Type* return_type = func_decl->return_type
        ? type_from_ast(checker, func_decl->return_type)
        : type_checker_get_builtin(checker, TYPE_VOID);
    if (!return_type || type_is_error_union(return_type)) return 1;

    // Skip functions returning a NAMED LLVM struct (a user struct returned by
    // value): its LLVM type is keyed on the Type* pointer, and the prototype's
    // and body's independent type_from_ast() results are distinct pointers, so
    // the two would mint `anon`/`anon.1` named types and fail the verifier.
    // Anonymous tuple/multi-return structs now lower to a uniqued LITERAL struct
    // (codegen_get_struct_type), which is identical across calls — so those we
    // DO predeclare, giving forward-reference support for multi-return functions
    // (e.g. utf8 DecodeRune calling decodeRuneSlow, defined later). LLVM's own
    // literal-vs-named distinction (LLVMGetStructName == NULL for literals) is
    // the reliable discriminator.
    LLVMTypeRef lowered_ret = codegen_type_to_llvm(codegen, return_type);
    if (!lowered_ret) return 1;
    if (LLVMGetTypeKind(lowered_ret) == LLVMStructTypeKind &&
        LLVMGetStructName(lowered_ret) != NULL) {
        return 1;
    }

    // Method mangling (T__m) then package prefixing (goo_pkg__<pkg>__...).
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
    const char* symbol_name = emit_name;
    char* pkg_mangled = codegen_package_symbol_name(checker, emit_name);
    if (pkg_mangled) symbol_name = pkg_mangled;

    // Idempotent: only create if not already present (a later reached decl, or a
    // repeat pass, must not add a duplicate that LLVM would rename).
    if (!LLVMGetNamedFunction(codegen->module, symbol_name)) {
        LLVMTypeRef llvm_return_type = lowered_ret;
        int is_entry_main = (!func_decl->receiver &&
                             strcmp(func_decl->name, "main") == 0 &&
                             return_type->kind == TYPE_VOID);
        if (is_entry_main) llvm_return_type = LLVMInt32TypeInContext(codegen->context);

        LLVMTypeRef* param_types = NULL;
        int param_count = 0;
        Variable* func_var = type_checker_lookup_variable(checker, emit_name);
        if (func_var && func_var->type->kind == TYPE_FUNCTION &&
            func_var->type->data.function.param_count > 0) {
            param_count = func_var->type->data.function.param_count;
            param_types = malloc(sizeof(LLVMTypeRef) * param_count);
            for (int i = 0; i < param_count; i++) {
                param_types[i] = codegen_type_to_llvm(
                    codegen, func_var->type->data.function.param_types[i]);
            }
        }
        if (llvm_return_type) {
            LLVMTypeRef function_type =
                LLVMFunctionType(llvm_return_type, param_types, param_count, 0);
            LLVMAddFunction(codegen->module, symbol_name, function_type);
        }
        if (param_types) free(param_types);
    }

    free(mangled);
    free(pkg_mangled);
    return 1;
}
#endif

int codegen_predeclare_functions(CodeGenerator* codegen, TypeChecker* checker, ASTNode* decls) {
    if (!codegen || !checker) return 0;
#if LLVM_AVAILABLE
    for (ASTNode* d = decls; d; d = d->next) {
        if (d->type == AST_FUNC_DECL) {
            if (!codegen_predeclare_function(codegen, checker, (FuncDeclNode*)d)) return 0;
        }
    }
#endif
    return 1;
}

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

    // stdlib Phase 0 (Task 4): a non-main package's top-level functions (plain
    // AND methods) are emitted under a mangled symbol `goo_pkg__<pkg>__<base>`
    // so they never collide with main's bare names in the shared module. The
    // main package (checker->current_package == NULL) is UNCHANGED — bare names
    // — which keeps the no-import path byte-identical. `emit_name` (the bare
    // function name, or the method-mangled `T__m`) stays the type-checker lookup
    // key below; only the LLVM SYMBOL name is package-prefixed. Using emit_name
    // as the base means methods are prefixed too (fixes the earlier gap where a
    // package method emitted under the bare `T__m` and collided with main).
    const char* symbol_name = emit_name;
    char* pkg_mangled = codegen_package_symbol_name(checker, emit_name);
    if (pkg_mangled) symbol_name = pkg_mangled;

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

    // Create the function (mangled symbol for non-main packages; bare for main),
    // or reuse the prototype the forward-reference pre-pass
    // (codegen_predeclare_function) already declared — creating a second
    // LLVMAddFunction under the same name would make LLVM rename it and break
    // call resolution.
    LLVMValueRef function = LLVMGetNamedFunction(codegen->module, symbol_name);
    if (!function) {
        function = LLVMAddFunction(codegen->module, symbol_name, function_type);
    }
    
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
    
    // Create function info under the SAME (possibly mangled) symbol name used
    // for LLVMAddFunction, so intra-package call resolution stays consistent.
    FunctionInfo* func_info = function_info_new(symbol_name, function, return_type);
    free(mangled);     // emit_name was copied by LLVMAddFunction / function_info_new
    free(pkg_mangled); // symbol_name likewise copied; safe to free the mangled buffer
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

    // Task 2 / var-init cluster: evaluate deferred (non-constant) global
    // initializers before any user code runs — must be main's very FIRST
    // instruction, ahead of the escape pre-pass below (which emits no IR,
    // so ordering against it doesn't matter) and everything else. The
    // symbol only exists if codegen_generate_program's pre-pass
    // (codegen_program_needs_global_init) found a deferrable global
    // initializer anywhere in the program — main's own prologue never
    // creates it, only looks it up, so a program with no such initializer
    // emits no call and no goo.global_init at all. There is no other
    // runtime-init call at main entry today to order against (goo_init in
    // runtime_integration.c is declared but never called from codegen).
    if (is_entry_main) {
        LLVMValueRef global_init_fn = LLVMGetNamedFunction(codegen->module, "goo.global_init");
        if (global_init_fn) {
            LLVMTypeRef void_ty = LLVMFunctionType(LLVMVoidTypeInContext(codegen->context), NULL, 0, 0);
            LLVMBuildCall2(codegen->builder, void_ty, global_init_fn, NULL, 0, "");
        }
    }

    // M8b: compute which locals escape into a goroutine and must be heap-promoted.
    escape_prepass_compute(func_decl->body);

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
            // Task 2: mirror declare_function_signature's []T wrap for a
            // variadic param so re-invoked type-check calls during codegen
            // (e.g. `for _, n := range nums`) see the same slice type the
            // signature and body-binding passes already agree on.
            if (pd->is_variadic_param && pt) pt = type_slice(pt);
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

                LLVMValueRef param_alloca = codegen_alloc_local(codegen, param_types[param_index], param_name);
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

    // Named return parameters (P3-5): bind each named result as a
    // zero-initialized in-scope local (its own alloca), mirror it into the
    // type-checker scope, and record its name on the FunctionInfo in field
    // order. Assignments in the body write through these allocas; a bare
    // `return` (see codegen_generate_return_stmt) loads them and rebuilds
    // the aggregate. The parser encodes named results as an inline
    // StructTypeNode; anonymous tuple results use synthetic `_N` names and
    // are skipped (they are produced by explicit `return a, b`).
    if (func_decl->return_type && func_decl->return_type->type == AST_STRUCT_TYPE) {
        StructTypeNode* st = (StructTypeNode*)func_decl->return_type;
        size_t cap = 0;
        for (ASTNode* f = st->fields; f; f = f->next)
            if (f->type == AST_VAR_DECL) cap++;
        char** names = cap ? calloc(cap, sizeof(char*)) : NULL;
        size_t nnamed = 0;
        for (ASTNode* f = st->fields; f; f = f->next) {
            if (f->type != AST_VAR_DECL) continue;
            VarDeclNode* fd = (VarDeclNode*)f;
            if (fd->name_count == 0 || !fd->names) continue;
            if (is_synthetic_result_name(fd->names[0])) continue;
            const char* rname = fd->names[0];
            Type* ft = fd->type ? type_from_ast(checker, fd->type) : NULL;
            if (!ft) continue;
            LLVMTypeRef llvm_ft = codegen_type_to_llvm(codegen, ft);
            if (!llvm_ft) continue;
            LLVMValueRef slot = codegen_alloc_local(codegen, llvm_ft, rname);
            LLVMBuildStore(codegen->builder, LLVMConstNull(llvm_ft), slot);
            ValueInfo* vi = value_info_new(rname, slot, ft);
            vi->is_lvalue = 1;
            vi->is_initialized = 1;
            codegen_add_value(codegen, vi);
            // Mirror into the type-checker scope so re-checks from codegen
            // (e.g. binary-expr type resolution) can resolve the name.
            Variable* rv = variable_new(rname, ft, fd->base.pos);
            if (rv) { rv->is_initialized = 1; scope_add_variable(checker->current_scope, rv); }
            if (names) names[nnamed++] = strdup(rname);
        }
        if (nnamed > 0) {
            func_info->named_result_names = names;
            func_info->named_result_count = nnamed;
        } else {
            free(names);
        }
    }

    // Generate function body
    int result = 1;
    if (func_decl->body) {
        result = codegen_generate_statement(codegen, checker, func_decl->body);
    }
    
    // Add return if missing
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
        if (is_entry_main) {
            // Implicit structured-concurrency join (intentional Goo superset of
            // Go): block until every goroutine spawned via `go` has finished, so
            // fire-and-forget side effects are observable before the process
            // exits — unlike Go, where main-return abandons running goroutines.
            // The scheduler is lazily created by the first goo_go();
            // goo_scheduler_wait() is a no-op when none ran. (A `main` that exits
            // via an explicit `return` currently bypasses this; making the join
            // uniform across all exit paths is a tracked follow-up.)
            LLVMTypeRef wait_ty = LLVMFunctionType(LLVMVoidTypeInContext(codegen->context), NULL, 0, 0);
            LLVMValueRef wait_fn = LLVMGetNamedFunction(codegen->module, "goo_scheduler_wait");
            if (!wait_fn) wait_fn = LLVMAddFunction(codegen->module, "goo_scheduler_wait", wait_ty);
            LLVMBuildCall2(codegen->builder, wait_ty, wait_fn, NULL, 0, "");
        }
        // Run any registered defers (LIFO) on the fall-off-the-end exit path.
        codegen_emit_deferred_calls(codegen, checker);
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

#if LLVM_AVAILABLE
// Task 2b: recursive all-constant-elements check over a composite literal's
// element tree. Answers: will composite_codegen.c's module-scope
// "all-constant fast path" handle every element builder-free? Constant
// shapes:
//   - any bare literal (int/float/string/bool/char/nil);
//   - an identifier that resolves to a registered CONSTANT global (a
//     package-level `const` — codegen_generate_const_decl registers these
//     as is_lvalue=0 globals with LLVMSetGlobalConstant; identifier codegen
//     then returns the INITIALIZER constant without any load, i.e.
//     builder-free). This case is REQUIRED by the goostd lookup tables —
//     utf8's `first`/`acceptRanges` and their like are composed of const
//     identifiers (`as`, `xx`, `locb`, ...), and they must stay on the
//     immediate path (bits/utf8 goldens are the guard). An identifier that
//     resolves to a VAR needs an LLVMBuildLoad2 — not constant;
//   - a keyed element `k: v` whose key folds via goo_fold_const_int and
//     whose value is recursively constant;
//   - a nested composite (incl. the elided `{locb, hicb}` form) whose
//     elements are all recursively constant.
// Everything else — var identifiers, calls (incl. conversions: deferral
// evaluates them correctly, so no special exemption is needed), binary and
// unary expressions, selectors, indexing — is NOT constant, so the
// enclosing composite defers to goo.global_init (main package) or hits the
// package-scope clean rejection (imported packages, where nothing needs it
// today).
//
// `codegen == NULL` selects PRE-PASS mode (codegen_program_needs_global_init
// runs before ANY declaration is generated, so the value table holds no
// consts yet): identifiers count as NOT constant. That makes the pre-pass a
// strict over-approximation of the decl-time answer — it may predict
// deferral for a composite the decl-time check keeps immediate (worst case:
// an empty goo.global_init is synthesized and called), but it can never
// predict "no deferral" when decl time defers, which is the direction that
// would lose initializers (main couldn't call a prototype that was never
// created).
static int global_init_elem_is_const(CodeGenerator* codegen, ASTNode* e) {
    if (!e) return 0;
    switch (e->type) {
        case AST_LITERAL:
            return 1;
        case AST_IDENTIFIER: {
            if (!codegen) return 0;  // pre-pass: consts not registered yet
            ValueInfo* vi = codegen_lookup_value(codegen, ((IdentifierNode*)e)->name);
            if (!vi || vi->is_lvalue || !vi->llvm_value) return 0;
            if (!LLVMIsAGlobalVariable(vi->llvm_value) ||
                !LLVMIsGlobalConstant(vi->llvm_value)) return 0;
            // Mirror identifier codegen's builder-free const fast path
            // exactly: it substitutes the initializer, so one must exist.
            LLVMValueRef init = LLVMGetInitializer(vi->llvm_value);
            return init && LLVMIsConstant(init);
        }
        case AST_KEYED_ELEMENT: {
            KeyedElementNode* ke = (KeyedElementNode*)e;
            uint64_t k;
            if (!goo_fold_const_int(ke->key, &k)) return 0;
            return global_init_elem_is_const(codegen, ke->value);
        }
        case AST_SLICE_EXPR: {
            for (ASTNode* el = ((SliceLitNode*)e)->elements; el; el = el->next)
                if (!global_init_elem_is_const(codegen, el)) return 0;
            return 1;
        }
        case AST_ARRAY_LITERAL: {
            for (ASTNode* el = ((ArrayLitNode*)e)->elements; el; el = el->next)
                if (!global_init_elem_is_const(codegen, el)) return 0;
            return 1;
        }
        case AST_STRUCT_LITERAL: {
            for (ASTNode* fv = ((StructLiteralNode*)e)->field_values; fv; fv = fv->next)
                if (!global_init_elem_is_const(codegen, fv)) return 0;
            return 1;
        }
        default:
            return 0;
    }
}

// Task 2 / var-init cluster: module-scope initializer classification. Can
// `expr` (a package-level var's initializer) be evaluated NOW, with no
// positioned LLVM builder, as a true module constant? Trusted to skip the
// builder entirely: a bare literal (or nil), and a composite literal whose
// element tree is recursively all-constant (global_init_elem_is_const —
// the goostd lookup-table shape, handled by composite_codegen.c's constant
// fast path). Everything else — identifiers, calls, binary/unary
// expressions, selectors, indexing, composites with any non-constant
// element (Task 2b fix: `var t = []int{a}` used to SIGSEGV on this path) —
// is deferred to goo.global_init(), which runs with a real positioned
// builder before user main. This subsumes the old call-rejection guard: a
// call is now deferred rather than cleanly rejected — fixing the SIGSEGV
// root cause (module-scope codegen_generate_expression touching an
// unpositioned builder), since we never attempt generation for these
// shapes at module scope at all.
//
// Nullable carve-out (Task 2b widened, review m4): ANY non-nil initializer
// into a `?T` global defers — not just cross-KIND literals. The module-
// scope nullable-wrap fallback is a bare InsertValue with no coercion, so
// a literal whose LLVM type mismatches the base slot in WIDTH alone
// (`var g ?float32 = 2.5`: double into float; `var h ?int32 = 5`: i64 into
// i32) built a malformed constant that died in the LLVM emitter with a
// location-less "invalid number of bytes". Deferral routes these through
// codegen_create_nullable_with_value, which width- and kind-coerces first.
// `?T = nil` stays immediate: the nil intercept in codegen_generate_var_decl
// builds the {is_null=1, zero} constant without the builder.
//
// `codegen == NULL` selects pre-pass mode — see global_init_elem_is_const.
static int global_init_should_defer(CodeGenerator* codegen, ASTNode* expr, Type* var_type) {
    if (!expr) return 0;

    if (expr->type == AST_LITERAL &&
        ((LiteralNode*)expr)->literal_type == TOKEN_NIL) {
        return 0;
    }

    if (var_type && var_type->kind == TYPE_NULLABLE) return 1;

    if (expr->type == AST_ARRAY_LITERAL || expr->type == AST_SLICE_EXPR ||
        expr->type == AST_STRUCT_LITERAL) {
        // All-constant composites MUST stay immediate: deferring them would
        // break the imported goostd packages (bits/utf8 lookup tables),
        // whose codegen_generate_program passes share this one module and
        // cannot own a second goo.global_init — see the package-scope
        // rejection in codegen_generate_var_decl below.
        return !global_init_elem_is_const(codegen, expr);
    }

    return expr->type != AST_LITERAL;
}

// Append a module-scope initializer that global_init_should_defer flagged to
// the deferred list, to be evaluated later by
// codegen_generate_global_init_function. Growable array; mirrors
// codegen_add_value's realloc pattern in codegen.c. Returns 0 on allocation
// failure.
static int codegen_defer_global_init(CodeGenerator* codegen, LLVMValueRef global,
                                     ASTNode* expr, Type* declared_type, Position pos) {
    if (codegen->deferred_global_init_count >= codegen->deferred_global_init_capacity) {
        size_t new_cap = codegen->deferred_global_init_capacity == 0
                        ? 8 : codegen->deferred_global_init_capacity * 2;
        DeferredGlobalInit* grown = realloc(codegen->deferred_global_inits,
                                            sizeof(DeferredGlobalInit) * new_cap);
        if (!grown) return 0;
        codegen->deferred_global_inits = grown;
        codegen->deferred_global_init_capacity = new_cap;
    }
    DeferredGlobalInit* entry = &codegen->deferred_global_inits[codegen->deferred_global_init_count++];
    entry->global = global;
    entry->expr = expr;
    entry->declared_type = declared_type;
    entry->pos = pos;
    return 1;
}

// Shared local-scope initializer pipeline (DRY: previously inlined four
// times — once per transform — at the bottom of codegen_generate_var_decl's
// per-name loop; #101's reviews flagged the duplication risk of copying it
// again for goo.global_init). Used by BOTH an ordinary function-body `var`
// declaration and each deferred global initializer evaluated inside the
// synthesized goo.global_init() (codegen_generate_global_init_function).
// Applies, in order: lvalue auto-load, nullable auto-wrap, interface box,
// width-coerce. Pure code-motion extraction — behavior (including the
// module-scope constant-rebuild arms of steps 2 and 4, reachable only via a
// STALE positioned builder left by an earlier function, since a fresh
// module-scope caller is now filtered out by global_init_should_defer
// before ever reaching this helper) is unchanged from the pre-extraction
// inline code. On failure, init_value is freed and 0 is returned — callers
// must not free it again. On success, returns 1 with init_value mutated in
// place, ready to store.
static int codegen_apply_local_init_pipeline(CodeGenerator* codegen, TypeChecker* checker,
                                              Type* var_type, LLVMTypeRef llvm_type,
                                              ValueInfo* init_value) {
    // 1. Auto-load an lvalue initializer to its rvalue. An index/selector
    // initializer (e.g. `tmp := s[i]`, `x := p.field`) returns the element
    // ADDRESS with is_lvalue=1; the store later — and the nullable/
    // interface/sext transforms before it — all expect a VALUE.
    if (init_value->is_lvalue && init_value->goo_type) {
        LLVMTypeRef load_ty = codegen_type_to_llvm(codegen, init_value->goo_type);
        if (load_ty) {
            init_value->llvm_value = LLVMBuildLoad2(codegen->builder, load_ty,
                                                    init_value->llvm_value, "init_load");
            init_value->is_lvalue = 0;
        }
    }

    // 2. Auto-wrap a plain value into a nullable struct when the declared
    // type is TYPE_NULLABLE.
    if (var_type && var_type->kind == TYPE_NULLABLE &&
        init_value->goo_type && init_value->goo_type->kind != TYPE_NULLABLE) {
        if (codegen->current_function) {
            // Positioned builder: route through the shared nullable-wrap
            // helper, which coerces the value to the slot's element type
            // first (fixes e.g. a typed int value into a float nullable
            // slot).
            init_value->llvm_value = codegen_create_nullable_with_value(
                codegen, llvm_type, init_value->llvm_value, init_value->goo_type);
        } else {
            // No positioned builder (reachable only via a stale block left
            // by an earlier function — see this function's header comment):
            // keep the original inline InsertValue pair, which does not
            // coerce the value's width/kind first.
            LLVMValueRef agg = LLVMGetUndef(llvm_type);
            LLVMValueRef tag = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), 0, 0);
            agg = LLVMBuildInsertValue(codegen->builder, agg, tag, 0, "null_tag");
            agg = LLVMBuildInsertValue(codegen->builder, agg, init_value->llvm_value, 1, "null_val");
            init_value->llvm_value = agg;
        }
        init_value->goo_type = var_type;
    }

    // 3. Box a concrete value into an interface value (P4-5) when the
    // declared type is an interface.
    if (var_type && var_type->kind == TYPE_INTERFACE &&
        init_value->goo_type && init_value->goo_type->kind != TYPE_INTERFACE) {
        LLVMValueRef boxed = codegen_interface_box(codegen, checker, var_type,
                                                   init_value->goo_type,
                                                   init_value->llvm_value);
        if (!boxed) { value_info_free(init_value); return 0; }
        init_value->llvm_value = boxed;
        init_value->goo_type = var_type;
    }

    // 4. Match the initializer's width to the declared type (narrowing,
    // widening, and float-width coercion — see the pre-extraction comment
    // this replaced for the full narrowing/widening/float rationale).
    {
        LLVMTypeRef init_ty = LLVMTypeOf(init_value->llvm_value);
        if (init_ty != llvm_type) {
            int use_sext = init_value->goo_type
                         ? type_is_signed(init_value->goo_type) : 1;
            if (codegen->current_function) {
                init_value->llvm_value = codegen_coerce_to_type(
                    codegen, init_value->llvm_value, use_sext, llvm_type);
            } else if (LLVMIsConstant(init_value->llvm_value)) {
                // No positioned builder (see step 2's comment): rebuild the
                // constant at the target width directly instead of calling
                // the builder-requiring coercion helper.
                LLVMTypeKind fk = LLVMGetTypeKind(init_ty);
                LLVMTypeKind tk = LLVMGetTypeKind(llvm_type);
                if (fk == LLVMIntegerTypeKind && tk == LLVMIntegerTypeKind) {
                    unsigned long long raw = use_sext
                        ? (unsigned long long)LLVMConstIntGetSExtValue(init_value->llvm_value)
                        : LLVMConstIntGetZExtValue(init_value->llvm_value);
                    init_value->llvm_value = LLVMConstInt(llvm_type, raw, use_sext);
                } else if ((fk == LLVMFloatTypeKind || fk == LLVMDoubleTypeKind) &&
                           (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind)) {
                    LLVMBool loses_info;
                    double d = LLVMConstRealGetDouble(init_value->llvm_value, &loses_info);
                    init_value->llvm_value = LLVMConstReal(llvm_type, d);
                } else if (fk == LLVMIntegerTypeKind &&
                           (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind)) {
                    double d = use_sext
                        ? (double)LLVMConstIntGetSExtValue(init_value->llvm_value)
                        : (double)(unsigned long long)LLVMConstIntGetZExtValue(init_value->llvm_value);
                    init_value->llvm_value = LLVMConstReal(llvm_type, d);
                }
            }
        }
    }
    return 1;
}
#endif

int codegen_program_needs_global_init(ASTNode* decls) {
#if !LLVM_AVAILABLE
    (void)decls;
    return 0;
#else
    for (ASTNode* d = decls; d; d = d->next) {
        if (d->type != AST_VAR_DECL) continue;
        VarDeclNode* vd = (VarDeclNode*)d;
        if (!vd->values) continue;
        // codegen == NULL: pre-pass mode — no declaration has been generated
        // yet, so const identifiers can't be resolved; the classifier
        // over-approximates (may predict deferral the decl-time check avoids;
        // never the reverse). See global_init_elem_is_const.
        if (global_init_should_defer(NULL, vd->values, d->node_type)) return 1;
    }
    return 0;
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


    // Go-style error-union destructure `n, err := <!T>` — evaluate the !T
    // once, then bind name0 to the unwrapped value arm and name1 to a `?error`
    // ({i1 is_null, i8*}) that is nil exactly when the union holds no error.
    // Must precede the generic struct-destructure: a !T is a 2-field
    // {i1 is_error, union} aggregate, and ExtractValue'ing its raw fields would
    // hand the is_error flag to name0 and the union payload to name1.
    if (var_decl->name_count == 2 && var_decl->is_short_decl &&
        var_type->kind == TYPE_ERROR_UNION && var_decl->values) {
        ValueInfo* rhs = codegen_generate_expression(codegen, checker, var_decl->values);
        if (!rhs) {
            codegen_error(codegen, decl->pos, "Failed to generate !T destructure RHS");
            return 0;
        }

        // is_error flag (struct index 0) drives both the value arm and the
        // ?error nil polarity.
        LLVMValueRef is_error = codegen_error_union_is_error(codegen, rhs->llvm_value);

        // name0 = unwrapped value arm (mirrors catch/try: ExtractValue 1 then 0).
        Type* value_type = var_type->data.error_union.value_type;
        const char* nm0 = var_decl->names[0];
        LLVMValueRef value = codegen_error_union_get_value(codegen, rhs->llvm_value);
        LLVMTypeRef value_llvm = codegen_type_to_llvm(codegen, value_type);
        // Go semantics: name0 is the zero value when the union holds an error.
        // The error arm leaves the value slot undef (the error constructor only
        // writes the error slot), so `n, err := Atoi("bad")` must read 0 for n,
        // not poison. select(is_error, zero(T), value) — mirrors the err_ptr
        // select below and the spec's preferred form for the value arm.
        value = LLVMBuildSelect(codegen->builder, is_error,
            LLVMConstNull(value_llvm), value, "val_zero_on_err");
        LLVMValueRef val_alloca = codegen_alloc_local(codegen, value_llvm, nm0);
        LLVMBuildStore(codegen->builder, value, val_alloca);
        ValueInfo* vi0 = value_info_new(nm0, val_alloca, value_type);
        vi0->is_lvalue = 1;
        vi0->is_initialized = 1;
        codegen_add_value(codegen, vi0);
        Variable* tv0 = variable_new(nm0, value_type, decl->pos);
        if (tv0) { tv0->is_initialized = 1; scope_add_variable(checker->current_scope, tv0); }

        // name1 = ?error {i1 is_null, i8*}. nil ⟺ !is_error, so `err != nil`
        // (which lowers to !is_null) is true exactly when is_error is true.
        const char* nm1 = var_decl->names[1];
        Type* err_type = type_checker_error_type(checker);
        LLVMTypeRef err_llvm = codegen_type_to_llvm(codegen, err_type);
        LLVMTypeRef i8pt = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
        LLVMValueRef is_null = LLVMBuildNot(codegen->builder, is_error, "err_is_null");

        // Branch: box the union's goo_string error arm only when is_error (keeps the
        // common success path allocation-free). PHI the resulting i8* handle.
        LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(codegen->builder));
        LLVMBasicBlockRef box_bb   = LLVMAppendBasicBlockInContext(codegen->context, fn, "err.box");
        LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(codegen->context, fn, "err.box.merge");
        LLVMBasicBlockRef entry_bb = LLVMGetInsertBlock(codegen->builder);
        LLVMBuildCondBr(codegen->builder, is_error, box_bb, merge_bb);

        // box_bb: extract the error arm goo_string and box it — but only when
        // the union's error arm is string-shaped (goo_string_t {i8*, i64}).
        // That's true both when error_type == NULL (falls back to the default
        // TYPE_STRING, per codegen_get_error_union_type/type_mapping.c:272-277)
        // AND when error_type is explicitly the builtin TYPE_STRING — which is
        // what strconv.Atoi's `!int` return actually carries (expression_checker.c
        // ~1243: `type_error_union(int_t, err_t)` with err_t = builtin TYPE_STRING,
        // not NULL). Checking only `== NULL` misclassifies that real case as
        // "non-default" and segfaults destructure_error_msg_probe (the marker
        // path replaces the boxed message, so .Error() dereferences inttoptr(1)
        // as if it were a goo_error). A genuinely non-string explicit error arm
        // (e.g. a custom !T error type — not constructible in current v1 syntax)
        // isn't a goo_string, so codegen_error_union_get_error/goo_error_from_string
        // would build invalid IR for it; keep a non-null marker instead (spec
        // Task 5's promised degradation): `err != nil` still holds, .Error()
        // yields "" (no message).
        Type* err_arm_type = var_type->data.error_union.error_type;
        int default_arm = (err_arm_type == NULL) || (err_arm_type->kind == TYPE_STRING);
        codegen_set_insert_point(codegen, box_bb);
        LLVMValueRef boxed;
        if (default_arm) {
            LLVMValueRef arm = codegen_error_union_get_error(codegen, rhs->llvm_value);
            LLVMValueRef from_str = LLVMGetNamedFunction(codegen->module, "goo_error_from_string");
            if (!from_str) { codegen_error(codegen, decl->pos, "goo_error_from_string not found in module"); value_info_free(rhs); return 0; }
            LLVMValueRef fargs[] = { arm };
            boxed = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(from_str), from_str, fargs, 1, "err.boxed");
        } else {
            // Explicit non-string error arm: not goo_string, can't box a message in v1.
            // Keep a non-null marker so `err != nil` holds; .Error() yields "" (no message).
            boxed = LLVMBuildIntToPtr(codegen->builder,
                LLVMConstInt(LLVMInt64TypeInContext(codegen->context), 1, 0), i8pt, "err_marker");
        }
        LLVMBuildBr(codegen->builder, merge_bb);
        LLVMBasicBlockRef box_exit = LLVMGetInsertBlock(codegen->builder);

        // merge_bb: PHI null (success) vs boxed (error).
        codegen_set_insert_point(codegen, merge_bb);
        LLVMValueRef err_ptr = LLVMBuildPhi(codegen->builder, i8pt, "err_ptr");
        LLVMValueRef null_ptr = LLVMConstNull(i8pt);
        LLVMAddIncoming(err_ptr, &null_ptr, &entry_bb, 1);
        LLVMAddIncoming(err_ptr, &boxed, &box_exit, 1);

        LLVMValueRef err_val = LLVMGetUndef(err_llvm);
        err_val = LLVMBuildInsertValue(codegen->builder, err_val, is_null, 0, "err.is_null");
        err_val = LLVMBuildInsertValue(codegen->builder, err_val, err_ptr, 1, "err.ptr");
        LLVMValueRef err_alloca = codegen_alloc_local(codegen, err_llvm, nm1);
        LLVMBuildStore(codegen->builder, err_val, err_alloca);
        ValueInfo* vi1 = value_info_new(nm1, err_alloca, err_type);
        vi1->is_lvalue = 1;
        vi1->is_initialized = 1;
        codegen_add_value(codegen, vi1);
        Variable* tv1 = variable_new(nm1, err_type, decl->pos);
        if (tv1) { tv1->is_initialized = 1; scope_add_variable(checker->current_scope, tv1); }

        value_info_free(rhs);
        return 1;
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
            LLVMValueRef field_alloca = codegen_alloc_local(codegen, field_llvm, nm);
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
            alloca_inst = codegen_alloc_local(codegen, llvm_type, var_name);
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
            // Task 2 / var-init cluster: a module-scope initializer that
            // global_init_should_defer flags (anything but a bare
            // literal/nil or an all-constant composite, plus any non-nil
            // nullable init) cannot be safely generated now — see that
            // function's header comment for the SIGSEGV root cause. Queue
            // it and let codegen_generate_global_init_function evaluate it
            // later, with a real positioned builder, in a synthesized
            // goo.global_init() called before user main. Declaration order
            // becomes the evaluation order. Deviation from Go: a forward
            // reference (`var p = q` before `var q = 7`) never reaches this
            // path — the type checker's def-before-use rule rejects it
            // ("Undefined variable") — where Go would reorder and compute
            // 7. Rejection-where-Go-reorders, not wrong values (documented
            // in examples/global_init_probe.goo).
            if (!codegen->current_function &&
                global_init_should_defer(codegen, var_decl->values, var_type)) {
                if (checker->current_package) {
                    // Defensive boundary (see global_init_should_defer's
                    // header comment): deferral only works for a SINGLE
                    // program-wide goo.global_init, populated exclusively
                    // during main's codegen_generate_program pass. No
                    // current goostd package needs this (every package-
                    // level var is a constant composite literal, kept on
                    // today's path above), but a future one might — fail
                    // cleanly here instead of silently colliding with (or
                    // reprocessing stale entries alongside) another
                    // package's goo.global_init.
                    codegen_error(codegen, decl->pos,
                        "Package-level variable '%s' requires a constant initializer "
                        "(non-constant package-scope globals are not yet supported)",
                        var_name);
                    return 0;
                }
                if (!codegen_defer_global_init(codegen, alloca_inst, var_decl->values,
                                               var_type, decl->pos)) {
                    codegen_error(codegen, decl->pos,
                        "Failed to queue deferred initializer for global '%s'", var_name);
                    return 0;
                }
            } else {
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

                // Shared local-scope pipeline (lvalue auto-load, nullable
                // auto-wrap, interface box, width-coerce) — see
                // codegen_apply_local_init_pipeline. On failure it has already
                // freed init_value.
                if (!codegen_apply_local_init_pipeline(codegen, checker, var_type, llvm_type, init_value)) {
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

// Task 2 / var-init cluster: fill in goo.global_init()'s body from the
// deferred global-initializer list (see global_init_should_defer /
// codegen_defer_global_init). No-op if codegen_program_needs_global_init
// found nothing to defer — the prototype was never pre-created by
// codegen_generate_program, so LLVMGetNamedFunction finds nothing and there
// is nothing to fill in. Each deferred entry runs the SAME local-scope
// pipeline an ordinary function-body var-decl uses
// (codegen_apply_local_init_pipeline), just against a fresh entry generated
// here instead of one inlined in codegen_generate_var_decl. Entries run in
// DECLARATION order — not Go's dependency-resolved order. In practice the
// difference surfaces as REJECTION, not wrong values: a forward reference
// (`var p = q` before `var q = 7`) is stopped much earlier by the type
// checker's def-before-use rule ("Undefined variable"), where Go would
// reorder and compute 7 (documented in examples/global_init_probe.goo's
// header).
int codegen_generate_global_init_function(CodeGenerator* codegen, TypeChecker* checker) {
#if !LLVM_AVAILABLE
    (void)checker;
    return codegen ? 1 : 0;
#else
    if (!codegen || !checker) return 0;

    LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo.global_init");
    if (!fn) {
        // The pre-pass (codegen_program_needs_global_init) found nothing
        // deferrable, so the prototype was never pre-created. Nothing to
        // fill in — and nothing can have been deferred: the pre-pass
        // over-approximates the decl-time classifier (identifiers count as
        // non-constant there), so decl-time deferral implies a pre-pass
        // "needs init" answer. Assert that direction anyway (Task 2b m6):
        // silently returning with pending entries would DROP initializers.
        if (codegen->deferred_global_init_count > 0) {
            codegen_error(codegen, codegen->deferred_global_inits[0].pos,
                "Internal error: %zu deferred global initializer(s) with no "
                "goo.global_init prototype (pre-pass under-approximated)",
                codegen->deferred_global_init_count);
            return 0;
        }
        return 1;
    }

    // Double-fill guard (Task 2b m6): a body means a previous pass already
    // filled this function. With nothing newly deferred that's a benign
    // repeat call; with pending entries, appending a second entry block (or
    // silently returning) would miscompile — refuse loudly instead.
    if (LLVMCountBasicBlocks(fn) != 0) {
        if (codegen->deferred_global_init_count == 0) return 1;
        codegen_error(codegen, codegen->deferred_global_inits[0].pos,
            "Internal error: goo.global_init already has a body; refusing to "
            "drop %zu newly deferred global initializer(s)",
            codegen->deferred_global_init_count);
        return 0;
    }

    FunctionInfo* func_info = function_info_new("goo.global_init", fn, NULL);
    if (!func_info) {
        codegen_error(codegen, (Position){0, 0, 0, "codegen"},
                     "Failed to create function info for goo.global_init");
        return 0;
    }
    func_info->entry_block = LLVMAppendBasicBlockInContext(codegen->context, fn, "entry");

    // Save the ambient codegen state so this synthesis — which runs after
    // every ordinary declaration has already been generated — leaves it
    // exactly as it found it. Defensive: codegen_generate_program calls
    // this last today, but preserving the invariant costs nothing and
    // avoids surprises for any future caller.
    LLVMValueRef saved_function = codegen->current_function;
    FunctionInfo* saved_function_info = codegen->current_function_info;
    LLVMBasicBlockRef saved_block = LLVMGetInsertBlock(codegen->builder);

    codegen_enter_function(codegen, func_info);
    codegen_set_insert_point(codegen, func_info->entry_block);

    int ok = 1;
    for (size_t i = 0; i < codegen->deferred_global_init_count; i++) {
        DeferredGlobalInit* d = &codegen->deferred_global_inits[i];
        Type* var_type = d->declared_type;
        LLVMTypeRef llvm_type = codegen_type_to_llvm(codegen, var_type);
        if (!llvm_type) {
            codegen_error(codegen, d->pos, "Failed to convert type for deferred global initializer");
            ok = 0;
            break;
        }

        ValueInfo* init_value;
        if (var_type && var_type->kind == TYPE_NULLABLE &&
            d->expr->type == AST_LITERAL &&
            ((LiteralNode*)d->expr)->literal_type == TOKEN_NIL) {
            init_value = codegen_generate_null_literal(codegen, checker, var_type);
        } else {
            init_value = codegen_generate_expression(codegen, checker, d->expr);
        }
        if (!init_value) {
            codegen_error(codegen, d->pos, "Failed to generate deferred global initializer");
            ok = 0;
            break;
        }

        if (!codegen_apply_local_init_pipeline(codegen, checker, var_type, llvm_type, init_value)) {
            ok = 0;
            break;
        }

        LLVMBuildStore(codegen->builder, init_value->llvm_value, d->global);
        value_info_free(init_value);
    }

    if (ok) {
        LLVMBuildRetVoid(codegen->builder);
        // Consume the queue (Task 2b m6): the entries are now compiled into
        // the body, so a hypothetical later pass must see an empty list —
        // paired with the double-fill guard above, re-processing (or
        // silently dropping) them becomes impossible.
        codegen->deferred_global_init_count = 0;
    }

    codegen_exit_function(codegen);

    // Restore ambient state.
    codegen->current_function = saved_function;
    codegen->current_function_info = saved_function_info;
    if (saved_block) LLVMPositionBuilderAtEnd(codegen->builder, saved_block);

    function_info_free(func_info);
    return ok;
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

    // Compile-time integer constant folding (128-bit): a mask like `1<<32 - 1`
    // must evaluate to its true value (4294967295), but codegen_generate_
    // expression below would emit a width-truncated `shl i32 1, 32` and get it
    // wrong. Fold pure integer constant expressions here and emit the value
    // directly. Works for both package and local consts (no type-checker
    // Variable required — a local const's is already torn down by codegen time).
    {
        uint64_t folded;
        if (goo_fold_const_int(const_decl->values, &folded)) {
            for (size_t i = 0; i < const_decl->name_count; i++) {
                const char* const_name = const_decl->names[i];
                Variable* known = type_checker_lookup_variable(checker, const_name);
                Type* ct = known ? known->type : NULL;
                if (!ct) {
                    // Untyped int const default type is `int` (int64 here);
                    // a value past int64's signed range takes uint64.
                    ct = (folded <= 9223372036854775807ULL)
                             ? type_checker_get_builtin(checker, TYPE_INT64)
                             : type_checker_get_builtin(checker, TYPE_UINT64);
                }
                LLVMTypeRef lt = codegen_type_to_llvm(codegen, ct);
                if (!lt) {
                    codegen_error(codegen, decl->pos,
                                  "Failed to convert type for constant '%s'", const_name);
                    return 0;
                }
                LLVMValueRef cv = LLVMConstInt(lt, (unsigned long long)(uint64_t)folded,
                                               type_is_signed(ct));
                LLVMValueRef g = LLVMAddGlobal(codegen->module, lt, const_name);
                LLVMSetInitializer(g, cv);
                LLVMSetGlobalConstant(g, 1);
                ValueInfo* vi = value_info_new(const_name, g, ct);
                if (!vi) { codegen_error(codegen, decl->pos, "value info alloc failed"); return 0; }
                vi->is_lvalue = 0;
                vi->is_initialized = 1;
                if (!codegen_add_value(codegen, vi)) {
                    codegen_error(codegen, decl->pos,
                                  "Failed to add constant '%s' to symbol table", const_name);
                    value_info_free(vi);
                    return 0;
                }
                if (!known) {
                    Variable* tcv = variable_new(const_name, ct, decl->pos);
                    if (tcv) {
                        tcv->is_initialized = 1;
                        scope_add_variable(checker->current_scope, tcv);
                    }
                }
            }
            return 1;
        }
    }

    // Compile-time string constant folding: a const initialised by string-
    // literal concatenation ("" + "\x00..." + ...) — the math/bits table shape.
    // Fold it to one byte buffer and emit a constant goo_string global, so the
    // const is compile-time. The `+` otherwise lowers to a runtime
    // goo_string_concat call, which is not an LLVM constant (the const-decl
    // rejects it below with "must be compile-time constant"). Works at both
    // package and local scope. Placed after the integer fold so int constants
    // (which never fold as strings) keep their existing path.
    {
        char* sbuf = NULL;
        size_t slen = 0;
        if (goo_fold_const_string(const_decl->values, &sbuf, &slen)) {
            LLVMValueRef sval = codegen_const_string_value(codegen, sbuf, slen);
            free(sbuf);
            Type* st = type_checker_get_builtin(checker, TYPE_STRING);
            LLVMTypeRef lt = codegen_type_to_llvm(codegen, st);
            for (size_t i = 0; i < const_decl->name_count; i++) {
                const char* const_name = const_decl->names[i];
                LLVMValueRef g = LLVMAddGlobal(codegen->module, lt, const_name);
                LLVMSetInitializer(g, sval);
                LLVMSetGlobalConstant(g, 1);
                ValueInfo* vi = value_info_new(const_name, g, st);
                if (!vi) { codegen_error(codegen, decl->pos, "value info alloc failed"); return 0; }
                vi->is_lvalue = 0;
                vi->is_initialized = 1;
                if (!codegen_add_value(codegen, vi)) {
                    codegen_error(codegen, decl->pos,
                                  "Failed to add constant '%s' to symbol table", const_name);
                    value_info_free(vi);
                    return 0;
                }
                if (!type_checker_lookup_variable(checker, const_name)) {
                    Variable* tcv = variable_new(const_name, st, decl->pos);
                    if (tcv) {
                        tcv->is_initialized = 1;
                        scope_add_variable(checker->current_scope, tcv);
                    }
                }
            }
            return 1;
        }
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
        
        // Get the constant's type. A package-level const has a persisted
        // type-checker Variable; a LOCAL const (inside a function body) does not
        // — its type-check scope was torn down after that function was checked —
        // so fall back to the type inferred for the initializer during codegen.
        Variable* var = type_checker_lookup_variable(checker, const_name);
        Type* const_type = var ? var->type : const_value->goo_type;
        if (!const_type) {
            codegen_error(codegen, decl->pos, "Cannot determine type for constant '%s'", const_name);
            value_info_free(const_value);
            return 0;
        }

        // Convert type to LLVM type
        LLVMTypeRef llvm_type = codegen_type_to_llvm(codegen, const_type);
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
        ValueInfo* value_info = value_info_new(const_name, global_const, const_type);
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

        // Mirror the constant into the type-checker scope so codegen-time
        // re-type-checks of later expressions that reference it resolve the
        // name. Codegen re-invokes type_check_* for e.g. a binary operand
        // (`n - 1`), and only params are mirrored into the type-checker scope on
        // function entry — a LOCAL const would otherwise read as "Undefined
        // variable". A package-level const's Variable already persists from the
        // type-check pass, so only register when the name is not already in
        // scope (avoids a duplicate).
        if (!type_checker_lookup_variable(checker, const_name)) {
            Variable* tcv = variable_new(const_name, const_type, decl->pos);
            if (tcv) {
                tcv->is_initialized = 1;
                scope_add_variable(checker->current_scope, tcv);
            }
        }
    }

    value_info_free(const_value);
    return 1;
#endif
}

