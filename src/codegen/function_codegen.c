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

    // Skip functions whose return type lowers to an aggregate (tuple/multi-
    // return, named-return struct): codegen_type_to_llvm mints a FRESH anonymous
    // struct on each call, so a prototype declared here and the body's return
    // value would be two distinct {..} literal types and fail the verifier
    // ("return type does not match operand type of return inst"). Such functions
    // keep their original single-pass emission — self-consistent, just without
    // forward-reference support (no plain leaf package needs a forward call into
    // a tuple-returning function; that case is deferred).
    LLVMTypeRef lowered_ret = codegen_type_to_llvm(codegen, return_type);
    if (!lowered_ret || LLVMGetTypeKind(lowered_ret) == LLVMStructTypeKind) return 1;

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

            // Auto-load an lvalue initializer to its rvalue before any further
            // processing. An index/selector initializer (e.g. `tmp := s[i]`,
            // `x := p.field`) returns the element ADDRESS with is_lvalue=1; the
            // store below — and the nullable/interface/sext transforms that
            // precede it — all expect a VALUE. Storing the raw address writes a
            // pointer into the value-typed slot: the wrong value, and for an
            // 8-byte pointer in a narrower slot (e.g. a 4-byte int element) an
            // out-of-bounds stack write that clobbers the adjacent slot. Mirrors
            // the load idiom used by the `=` assignment, return, range, and
            // defer paths.
            if (init_value->is_lvalue && init_value->goo_type) {
                LLVMTypeRef load_ty = codegen_type_to_llvm(codegen, init_value->goo_type);
                if (load_ty) {
                    init_value->llvm_value = LLVMBuildLoad2(codegen->builder, load_ty,
                                                            init_value->llvm_value, "init_load");
                    init_value->is_lvalue = 0;
                }
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

            // Box a concrete value into an interface value (P4-5) when the
            // declared type is an interface. The {vtable, data} box replaces the
            // concrete value before it is stored into the interface-typed slot.
            if (var_type && var_type->kind == TYPE_INTERFACE &&
                init_value->goo_type && init_value->goo_type->kind != TYPE_INTERFACE) {
                LLVMValueRef boxed = codegen_interface_box(codegen, checker, var_type,
                                                           init_value->goo_type,
                                                           init_value->llvm_value);
                if (!boxed) { value_info_free(init_value); return 0; }
                init_value->llvm_value = boxed;
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
                    // Untyped local const: type by the folded value's magnitude.
                    ct = (folded <= 2147483647ULL)
                             ? type_checker_get_builtin(checker, TYPE_INT32)
                         : (folded <= 9223372036854775807ULL)
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

