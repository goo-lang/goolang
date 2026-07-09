#include "codegen.h"
#include "comptime.h"
#include "value_scope.h"
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

// Allocate storage for a named local: heap (goo_alloc, leaked) if the local
// is EITHER goroutine-escape-promoted (M8b, name-based, `force_promote=0`
// callers) OR forced by the caller (Closures Task 2: a var-decl/param whose
// VarDeclNode->is_captured is set — a nested func literal reads or writes
// it, so its slot must outlive this frame; see codegen_alloc_local's
// callers in codegen_generate_var_decl/codegen_generate_function_decl/
// codegen_generate_func_lit). Else a stack entry alloca. Under opaque
// pointers both return `ptr`, so all downstream loads/stores (which carry
// explicit types via ValueInfo->goo_type, never LLVMGetAllocatedType) are
// unchanged — this is the design's core "uniformity trick": a promoted
// slot's value-table entry is IDENTICAL in shape (a `ptr`, is_lvalue=1) to
// an alloca'd one.
static LLVMValueRef codegen_alloc_local_promoted(CodeGenerator* codegen, LLVMTypeRef type,
                                                 const char* name, int force_promote) {
    if (!force_promote && !escape_is_promoted(name))
        return codegen_create_entry_alloca(codegen, type, name);

    LLVMValueRef size = LLVMSizeOf(type);

    // Closures Task 2 (loop-variable capture fix, DECL-SITE allocation): a
    // capture-promoted slot is minted AT THE DECLARATION SITE (the current
    // insert position), NOT hoisted to the entry block. This is what gives a
    // captured local declared inside a loop body (the `j := i` copy
    // workaround) Go's per-iteration variable semantics: the declaration
    // statement executes once per iteration, so each iteration's goo_alloc
    // mints a FRESH slot, and each iteration's closure env captures that
    // iteration's slot address (the value table's SSA entry for the name is
    // rebound to the new call result each time the decl re-executes at
    // runtime... more precisely: the ONE call instruction yields a different
    // pointer on each dynamic execution, and every in-body use — including
    // the env-build store — consumes that same dominating SSA value, i.e.
    // the current iteration's slot). Entry-hoisting here would silently
    // share ONE slot across all iterations — verified to produce 6 instead
    // of Go's 3 on the three-closure copy-capture probe before this fix.
    // Dominance is safe: a declaration lexically precedes every use of its
    // name, so the call instruction dominates all uses the same way the
    // decl's zero-init/initializer store (emitted at this same position by
    // every caller) always has.
    if (force_promote) {
        return codegen_emit_alloc(codegen, size, ALLOC_KIND_DEFAULT, NULL);
    }

    // M8b goroutine-escape path (unchanged): emit the goo_alloc in the entry
    // block (like an alloca) so it dominates all uses and runs once per
    // call. Save/restore the builder position.
    LLVMBasicBlockRef cur = LLVMGetInsertBlock(codegen->builder);
    LLVMBasicBlockRef entry = codegen->current_function_info
        ? codegen->current_function_info->entry_block
        : LLVMGetEntryBasicBlock(codegen->current_function);
    LLVMValueRef first = entry ? LLVMGetFirstInstruction(entry) : NULL;
    if (first) LLVMPositionBuilderBefore(codegen->builder, first);
    else       LLVMPositionBuilderAtEnd(codegen->builder, entry);

    LLVMValueRef p = codegen_emit_alloc(codegen, size, ALLOC_KIND_DEFAULT, NULL);
    if (cur) LLVMPositionBuilderAtEnd(codegen->builder, cur);
    return p;
}

// Public entry point (declared in codegen.h; called from every codegen file
// that allocates a named local) — escape-promotion only, the pre-Task-2
// behavior. codegen_alloc_local_promoted's `force_promote=1` path (closure
// capture) is used only by the handful of call sites in THIS file that have
// a VarDeclNode to check ->is_captured on.
LLVMValueRef codegen_alloc_local(CodeGenerator* codegen, LLVMTypeRef type, const char* name) {
    return codegen_alloc_local_promoted(codegen, type, name, 0);
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

// Closures Branch B, Task 1: emit a func literal `func(params) result {body}`
// as its own LLVM function `__goo_lit_<n>` (module-unique via
// codegen->func_lit_counter — see its doc comment in codegen.h) with the
// env-first calling convention (codegen_get_funcval_call_type): env is LLVM
// parameter 0, present but UNUSED in T1 (no captures; Branch B's Task 2
// wires the env struct through it). The literal's VALUE is the universal
// fat-pointer pair `{__goo_lit_n, NULL}` — same InsertValue-into-ConstNull
// shape as the named-function identifier arm (expression_codegen.c's
// codegen_generate_identifier).
//
// Unlike every other function-emitting entry point in this file, this one
// runs MID-EXPRESSION: the ENCLOSING function's body is still being
// generated when this is called, so this is a NESTED (not sequential)
// function emission. Every piece of ambient CodeGenerator/TypeChecker state
// that codegen_generate_function_decl normally owns for the FULL DURATION
// of a function's emission must therefore be saved before entering the
// literal and restored after, mirroring codegen_generate_global_init_
// function's save/restore discipline (this file) with three ADDITIONS that
// only matter for a nested (not sequential) emission:
//   - value_table_function_start: codegen_enter_function OVERWRITES this
//     with the literal's own start offset; without saving/restoring it, the
//     OUTER function's eventual codegen_exit_function would truncate the
//     value table back to the LITERAL's start instead of the outer's own,
//     leaking the literal's locals past its exit.
//   - the M8b escape-promotion globals (g_escape_names et al., file-static
//     above): escape_prepass_compute OVERWRITES the promoted-name set from
//     whatever body it is given (it is not accumulative), so leaving it as
//     the literal's set would misroute goroutine-escape promotion for any
//     code emitted AFTER the literal in the enclosing function.
//   - codegen->cfctx (ControlFlowContext, codegen_cfctx.h — Codegen
//     hardening R1; formerly loop_depth/loop_break_bb/loop_continue_bb/
//     loop_label/loop_is_loop from gofmt-syntax-b Task 1 and goto_label_
//     count/goto_label_names/goto_label_blocks from Task 2, as separate
//     CodeGenerator fields): none of this state self-resets for a NESTED
//     emission the way codegen_enter_function resets it for a sequential
//     one — the loop/break stack self-balances via push/pop WITHIN one
//     function's own codegen, so a literal nested inside an outer loop/
//     switch/select would otherwise inherit the OUTER's frames on the SAME
//     stack (any push the literal's own body makes would land on top of
//     them at a non-zero depth), and the goto-label table holds
//     LLVMBasicBlockRef values scoped to the OUTER function. Left
//     unhandled, two distinct failure modes follow: a labeled `break`/
//     `continue` inside the literal could walk past the literal's own
//     frames and match an OUTER label (a cross-function branch, only
//     caught by the LLVM verifier with no source position); and a bare
//     `break`/`continue`/`goto` with no enclosing construct of its own
//     inside the literal would silently target the OUTER function's
//     blocks instead of erroring. cfctx_save takes a snapshot of the
//     WHOLE struct in one assignment (see its doc comment,
//     codegen_cfctx.h, for why this — not a per-field memcpy enumeration —
//     is the fix), loop_depth is reset to 0 for the literal's own
//     emission (codegen_enter_function, next, resets goto_label_count via
//     cfctx_reset), and cfctx_restore puts the outer function's entire
//     saved state back afterward.
// The type-checker scope mirror below CHAINS onto the enclosing scope (T1
// rooted it at package/global instead — no captures) and marks the pushed
// scope is_function_boundary=1, mirroring type_check_func_lit's real (non-
// stand-in) T2 behavior exactly, so a stray re-check from codegen (e.g.
// type_check_binary_expr on `n + 1` inside a captured-variable expression)
// resolves an enclosing local correctly instead of failing.
//
// Closures Task 2 (capture): a captured variable's storage was ALREADY
// promoted to the heap at its declaration/param-binding site (codegen_alloc_
// local_promoted, keyed off VarDeclNode->is_captured — see
// codegen_generate_var_decl / codegen_generate_function_decl), so its
// value-table entry is a `ptr` with is_lvalue=1 — IDENTICAL in shape to an
// ordinary alloca'd local (the design's "uniformity trick": no load/store
// path anywhere in this codebase special-cases LLVMIsAAllocaInst or calls
// LLVMGetAllocatedType, verified by grep and by reading codegen_generate_
// identifier's load path and codegen_emit_lvalue_address's store path,
// expression_codegen.c). This function does two more things beyond T1:
//   - PROLOGUE (right after param binding, below): for each name in
//     lit->captured_names, load the slot POINTER from env (LLVM param 0,
//     GEP field i) and codegen_add_value it with is_lvalue=1 — shadowing
//     any same-named entry still visible from the enclosing function's
//     region of codegen's flat, unscoped value table (critical: that outer
//     entry's llvm_value is an SSA value defined in a DIFFERENT LLVM
//     function and would be invalid IR if read directly from inside this
//     one). This is also what makes a TRANSITIVE capture "just work": a
//     nested literal built later in THIS body sees this rebound entry via
//     the ordinary codegen_lookup_value path when IT builds its own env.
//   - ENV BUILD (at the pair-construction site, after ambient state is
//     restored to the ENCLOSING function): if captured_count > 0, goo_alloc
//     a struct of captured_count opaque pointers and, for each name in
//     lit->captured_names (SAME array, SAME order as the prologue — a
//     change-together contract), store that name's CURRENT slot address
//     (codegen_lookup_value in the ENCLOSING function's context) into the
//     matching field.
ValueInfo* codegen_generate_func_lit(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_FUNC_LIT) return NULL;
    FuncLitNode* lit = (FuncLitNode*)expr;

    Type* fn_type = expr->node_type;
    if (!fn_type || fn_type->kind != TYPE_FUNCTION) {
        codegen_error(codegen, expr->pos, "internal: func literal missing resolved type");
        return NULL;
    }

    char lit_name[64];
    snprintf(lit_name, sizeof(lit_name), "__goo_lit_%lu", codegen->func_lit_counter++);

    LLVMTypeRef lit_llvm_ty = codegen_get_funcval_call_type(codegen, fn_type);
    LLVMTypeRef llvm_return_type = codegen_type_to_llvm(codegen, fn_type->data.function.return_type);
    if (!lit_llvm_ty || !llvm_return_type) {
        codegen_error(codegen, expr->pos, "internal: failed to build func literal signature");
        return NULL;
    }

    LLVMValueRef lit_fn = LLVMAddFunction(codegen->module, lit_name, lit_llvm_ty);
    LLVMSetLinkage(lit_fn, LLVMInternalLinkage);

    FunctionInfo* func_info = function_info_new(lit_name, lit_fn, fn_type->data.function.return_type);
    if (!func_info) {
        codegen_error(codegen, expr->pos, "Failed to create function info for func literal");
        return NULL;
    }
    func_info->entry_block = LLVMAppendBasicBlockInContext(codegen->context, lit_fn, "entry");

    // Save every piece of ambient state this nested emission touches.
    LLVMValueRef saved_function = codegen->current_function;
    FunctionInfo* saved_function_info = codegen->current_function_info;
    LLVMBasicBlockRef saved_block = LLVMGetInsertBlock(codegen->builder);
    size_t saved_value_table_function_start = codegen->value_table_function_start;
    const char* saved_escape_names[ESCAPE_MAX_NAMES];
    memcpy(saved_escape_names, g_escape_names, sizeof(g_escape_names));
    size_t saved_escape_count = g_escape_count;
    int saved_escape_has_go = g_escape_has_go;
    Type* saved_return_type = checker->current_return_type;
    Scope* enclosing_scope = checker->current_scope;

    // Codegen hardening R1: save the outer function's ENTIRE control-flow
    // state (loop/break stack, goto-label table, pending label,
    // fallthrough stack) in one struct assignment — see this function's
    // top doc comment and cfctx_save's own (codegen_cfctx.h). Must happen
    // BEFORE codegen_enter_function zeroes goto_label_count below (via
    // cfctx_reset). loop_depth is reset to 0 explicitly here (unlike
    // goto_label_count, nothing else resets it per-function — it self-
    // balances via push/pop within one function's own codegen) for the
    // literal's own emission.
    ControlFlowContext saved_cfctx;
    cfctx_save(&saved_cfctx, &codegen->cfctx);
    codegen->cfctx.loop_depth = 0;

    codegen_enter_function(codegen, func_info);
    codegen_set_insert_point(codegen, func_info->entry_block);

    // M8b: compute which of the LITERAL's OWN locals escape into a
    // goroutine spawned from ITS OWN body — scoped fresh per the doc
    // comment above (restored below, not left applied to the enclosing
    // function's remaining codegen).
    escape_prepass_compute(lit->body);

    // Mirror the type-checker scope for re-invocations of type_check_* from
    // inside body codegen (same pattern as codegen_generate_function_decl).
    // Closures Task 2: chain directly onto the enclosing scope (checker-
    // >current_scope is already `enclosing_scope`, untouched) instead of
    // T1's "walk to package/global root" — see the doc comment above.
    scope_push(checker);
    checker->current_scope->is_function_boundary = 1;
    checker->current_return_type = fn_type->data.function.return_type;

    // Bind parameters. LLVM param 0 is env (T1: unused); the literal's OWN
    // params start at LLVM index 1.
    size_t param_count = fn_type->data.function.param_count;
    if (lit->params && param_count > 0) {
        ASTNode* param = lit->params;
        size_t param_index = 0;
        while (param && param_index < param_count) {
            const char* param_name = NULL;
            int param_is_captured = 0;
            if (param->type == AST_VAR_DECL) {
                VarDeclNode* pd = (VarDeclNode*)param;
                if (pd->name_count > 0 && pd->names) param_name = pd->names[0];
                param_is_captured = pd->is_captured;
            }
            if (param_name) {
                Type* pgoo_type = fn_type->data.function.param_types[param_index];
                LLVMTypeRef p_llvm_type = codegen_type_to_llvm(codegen, pgoo_type);
                if (p_llvm_type) {
                    LLVMValueRef param_value = LLVMGetParam(lit_fn, (unsigned)(param_index + 1));
                    // Closures Task 2: this literal's OWN param may itself be
                    // captured by a literal NESTED within it — promote the
                    // same way a named function's captured param is.
                    LLVMValueRef param_alloca = codegen_alloc_local_promoted(
                        codegen, p_llvm_type, param_name, param_is_captured);
                    LLVMBuildStore(codegen->builder, param_value, param_alloca);

                    ValueInfo* param_info = value_info_new(param_name, param_alloca, pgoo_type);
                    param_info->is_lvalue = 1;
                    param_info->is_initialized = 1;
                    vscope_add(codegen, param_info);

                    Variable* pv = variable_new(param_name, pgoo_type, param->pos);
                    if (pv) {
                        pv->is_initialized = 1;
                        pv->decl_node = param;  // Closures Task 2
                        scope_add_variable(checker->current_scope, pv);
                    }
                }
                param_index++;
            }
            param = param->next;
        }
    }

    // Closures Task 2: closure prologue — rebind each captured name (SAME
    // order as lit->captured_names — the BUILD ORDER = PROLOGUE ORDER
    // contract with the env-build site below, near this function's return)
    // to the slot pointer loaded from env (LLVM param 0). See this
    // function's top doc comment for why shadowing via codegen_add_value is
    // required here (not optional): codegen's value table is flat and
    // unscoped, so without this an outer-function SSA value would otherwise
    // still be "visible" here — invalid IR across a function boundary.
    if (lit->captured_count > 0) {
        LLVMTypeRef vp = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
        LLVMTypeRef* env_fields = malloc(sizeof(LLVMTypeRef) * lit->captured_count);
        for (size_t i = 0; i < lit->captured_count; i++) env_fields[i] = vp;
        LLVMTypeRef env_ty = LLVMStructTypeInContext(codegen->context, env_fields,
                                                     (unsigned)lit->captured_count, 0);
        free(env_fields);
        LLVMValueRef env_param = LLVMGetParam(lit_fn, 0);

        for (size_t i = 0; i < lit->captured_count; i++) {
            const char* cname = lit->captured_names[i];
            // Re-resolve the captured variable's TYPE via the checker
            // mirror scope (now correctly chained onto the enclosing
            // scope, not T1's global-rooted stand-in) — walks up past this
            // literal's own just-pushed scope to wherever the capture was
            // originally declared. The RAW lookup helper is used
            // deliberately (not type_check_identifier): this is a type
            // lookup for codegen's own bookkeeping, not a fresh capture
            // detection — the real capture was already recorded during
            // the type-check pass that produced lit->captured_names.
            Variable* cvar = type_checker_lookup_variable(checker, cname);
            Type* cgoo_type = cvar ? cvar->type : NULL;

            LLVMValueRef field_ptr = LLVMBuildStructGEP2(codegen->builder, env_ty, env_param,
                                                         (unsigned)i, "envfield");
            LLVMValueRef slot_ptr = LLVMBuildLoad2(codegen->builder, vp, field_ptr, cname);

            ValueInfo* cvi = value_info_new(cname, slot_ptr, cgoo_type);
            cvi->is_lvalue = 1;
            cvi->is_initialized = 1;
            vscope_add(codegen, cvi);
        }
    }

    // Generate the body.
    int ok = 1;
    if (lit->body) {
        ok = codegen_generate_statement(codegen, checker, lit->body);
    }

    // Default-return handling per the named-func path
    // (codegen_generate_function_decl): a fall-off-the-end exit runs any
    // registered defers (LIFO) then returns void/zero. No is_entry_main
    // case — a literal is never `main`.
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
        codegen_emit_deferred_calls(codegen, checker);
        if (LLVMGetTypeKind(llvm_return_type) == LLVMVoidTypeKind) {
            LLVMBuildRetVoid(codegen->builder);
        } else {
            LLVMBuildRet(codegen->builder, LLVMConstNull(llvm_return_type));
        }
    }

    // Restore every piece of saved ambient state, in the mirror order of
    // the save above.
    checker->current_return_type = saved_return_type;
    scope_pop(checker);
    checker->current_scope = enclosing_scope;

    codegen_exit_function(codegen);
    codegen->value_table_function_start = saved_value_table_function_start;
    codegen->current_function = saved_function;
    codegen->current_function_info = saved_function_info;
    if (saved_block) LLVMPositionBuilderAtEnd(codegen->builder, saved_block);
    memcpy(g_escape_names, saved_escape_names, sizeof(g_escape_names));
    g_escape_count = saved_escape_count;
    g_escape_has_go = saved_escape_has_go;

    // Codegen hardening R1: restore the outer function's entire saved
    // control-flow state in one struct assignment — see this function's
    // top doc comment and the save site above.
    cfctx_restore(&codegen->cfctx, &saved_cfctx);

    function_info_free(func_info);

    if (!ok) {
        codegen_error(codegen, expr->pos, "Failed to generate func literal body");
        return NULL;
    }

    // Closures Task 2: build the closure's env in the NOW-RESTORED outer
    // (enclosing-function) builder position — see this function's top doc
    // comment. A non-capturing literal (T1's only case) keeps env NULL.
    LLVMTypeRef vp = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
    LLVMValueRef env_ptr = LLVMConstNull(vp);
    if (lit->captured_count > 0) {
        LLVMTypeRef* env_fields = malloc(sizeof(LLVMTypeRef) * lit->captured_count);
        for (size_t i = 0; i < lit->captured_count; i++) env_fields[i] = vp;
        LLVMTypeRef env_ty = LLVMStructTypeInContext(codegen->context, env_fields,
                                                     (unsigned)lit->captured_count, 0);
        free(env_fields);

        LLVMValueRef env_size = LLVMSizeOf(env_ty);
        env_ptr = codegen_emit_alloc(codegen, env_size, ALLOC_KIND_DEFAULT, NULL);

        for (size_t i = 0; i < lit->captured_count; i++) {
            // Current slot address for this name, in the ENCLOSING
            // function's codegen context (codegen's flat value table — see
            // the prologue's doc comment above for why this is always a
            // valid SSA value HERE: we are back in the function that
            // either declared the promoted slot directly, or — for a
            // TRANSITIVE capture — already rebound it via its OWN
            // prologue). is_lvalue=1 and llvm_value IS the slot address
            // already (a promoted local's ValueInfo, like an alloca's,
            // holds the address, not a loaded value) — no load needed.
            ValueInfo* slot = codegen_lookup_value(codegen, lit->captured_names[i]);
            if (!slot || !slot->is_lvalue) {
                codegen_error(codegen, expr->pos,
                              "internal: captured variable '%s' has no promoted slot",
                              lit->captured_names[i]);
                return NULL;
            }
            LLVMValueRef field_ptr = LLVMBuildStructGEP2(codegen->builder, env_ty, env_ptr,
                                                         (unsigned)i, "envfield");
            LLVMBuildStore(codegen->builder, slot->llvm_value, field_ptr);
        }
    }

    // Build the literal's VALUE — the universal fat-pointer pair
    // `{__goo_lit_n, env_ptr}` — in the NOW-RESTORED outer builder position
    // (the Branch A identifier-arm pattern, expression_codegen.c).
    LLVMTypeRef pair_ty = codegen_get_funcval_pair_type(codegen);
    LLVMValueRef pair = LLVMConstNull(pair_ty);
    pair = LLVMBuildInsertValue(codegen->builder, pair, lit_fn, 0, "funcval_lit");
    pair = LLVMBuildInsertValue(codegen->builder, pair, env_ptr, 1, "funcval_lit_env");
    return value_info_new(lit_name, pair, fn_type);
#endif
}

// Forward declaration for error union function generation
int codegen_generate_error_union_function(CodeGenerator* codegen, TypeChecker* checker, 
                                         FuncDeclNode* func_decl, Type* return_type);

#if LLVM_AVAILABLE
// os.Args (Task 4): the Goo entry `main` IS the process's actual C entry
// point — the OS/libc hands argc/argv to whatever function is linked as
// `main`, independent of what Goo's `func main()` source declares (always
// zero params). Two extra LLVM-level params are appended past the Goo
// surface's own param_count to receive them; they are never bound as Goo
// locals — the body's parameter-binding loop below walks func_decl->params,
// which is empty for main, so it never touches these two. is_entry_main's
// prologue (codegen_generate_function_decl) reads them straight off the
// LLVM function via LLVMGetParam and passes them to goo_os_args_init.
// A single helper keeps the predeclare pass and the real definition
// (which must agree byte-for-byte or LLVMGetNamedFunction's reuse below
// would silently keep the WRONG, param-less prototype) from drifting.
static LLVMTypeRef* codegen_append_entry_main_params(CodeGenerator* codegen,
                                                      LLVMTypeRef* param_types,
                                                      int* param_count) {
    LLVMTypeRef i32 = LLVMInt32TypeInContext(codegen->context);
    LLVMTypeRef argv_ty = LLVMPointerType(
        LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0), 0);
    param_types = realloc(param_types, sizeof(LLVMTypeRef) * (size_t)(*param_count + 2));
    param_types[*param_count]     = i32;
    param_types[*param_count + 1] = argv_ty;
    *param_count += 2;
    return param_types;
}

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
// Comptime value params Task 3: does this (non-method) function declaration
// carry any `comptime` parameter? Duplicated (not shared) with codegen.c's
// identically-named static helper — per-TU static helper is this codebase's
// existing idiom for a small single-purpose function with no shared header
// (see e.g. monomorphize.c's str_dup). See that copy's doc comment for the
// method-scoping rationale.
static int func_decl_has_comptime_param(FuncDeclNode* fd) {
    for (ASTNode* p = fd->params; p; p = p->next) {
        if (p->type != AST_VAR_DECL) continue;
        if (((VarDeclNode*)p)->is_comptime_param) return 1;
    }
    return 0;
}

static int codegen_predeclare_function(CodeGenerator* codegen, TypeChecker* checker,
                                       FuncDeclNode* func_decl) {
    // Function generics Task 4: a generic function template's signature
    // contains TYPE_PARAM (e.g. bare `T`), which type_from_ast below cannot
    // lower on its own (it needs the per-call substitution the monomorphizer
    // performs in M3) — skip the predeclare prototype entirely for a
    // template. Without this guard, a *declared-but-never-called* generic
    // function still reaches this pass (predeclare runs over every decl,
    // unconditionally, before the codegen_generate_declaration loop that
    // Task 4's other guard covers) and re-triggers "Unknown type 'T'" /
    // return-type lowering failures even though type-checking passed.
    if (func_decl->type_params) return 1;
    // Comptime value params Task 3: same reasoning, for the comptime axis —
    // a plain function's `[n]int`-shaped signature/body can't be lowered
    // under the template's own placeholder binding either. Methods are
    // excluded; see codegen.c's identically-scoped skip-guard.
    if (!func_decl->receiver && func_decl_has_comptime_param(func_decl)) return 1;

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
        if (is_entry_main) {
            param_types = codegen_append_entry_main_params(codegen, param_types, &param_count);
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
    // Function-generics Task 9: a monomorphized instance overrides whatever
    // symbol name was just computed with its mangled instance name (e.g.
    // `Id__int64`) — installed by codegen_generate_function_instance
    // (monomorphize.c) around this exact call, for the duration of stamping
    // one concrete instantiation of a generic template. Checked last so it
    // always wins over both the bare and package-mangled names. `emit_name`
    // itself is untouched, so the type-checker lookup just below (which must
    // still find the TEMPLATE's Variable, carrying its TYPE_PARAM-bearing
    // signature) keys on the ordinary bare/method name as usual.
    if (codegen->symbol_override) symbol_name = codegen->symbol_override;

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

    if (is_entry_main) {
        param_types = codegen_append_entry_main_params(codegen, param_types, &param_count);
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

    // os.Args (Task 4): stash the OS-provided argc/argv — read straight off
    // main's two synthetic LLVM params (codegen_append_entry_main_params
    // above; never Goo locals) — before ANY user or global-init code runs,
    // so os.Args is available even from a global initializer. This is now
    // the first instruction in main's body; global-init (next) runs after it.
    if (is_entry_main) {
        LLVMValueRef args_init_fn = LLVMGetNamedFunction(codegen->module, "goo_os_args_init");
        if (args_init_fn) {
            LLVMValueRef argc_param = LLVMGetParam(function, (unsigned)(param_count - 2));
            LLVMValueRef argv_param = LLVMGetParam(function, (unsigned)(param_count - 1));
            LLVMValueRef init_args[] = { argc_param, argv_param };
            LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(args_init_fn),
                           args_init_fn, init_args, 2, "");
        }
    }

    // Task 2 / var-init cluster: evaluate deferred (non-constant) global
    // initializers before any user code runs — must run ahead of the escape
    // pre-pass below (which emits no IR, so ordering against it doesn't
    // matter) and everything else user-authored. The symbol only exists if
    // codegen_generate_program's pre-pass (codegen_program_needs_global_init)
    // found a deferrable global initializer anywhere in the program — main's
    // own prologue never creates it, only looks it up, so a program with no
    // such initializer emits no call and no goo.global_init at all. (goo_init
    // in runtime_integration.c is a separate, still-unrelated symbol: it is
    // declared but never called from codegen.)
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
    // Closures Task 2: mark this mirror scope as a function boundary too —
    // codegen's own re-invocations of type_check_identifier (e.g. via
    // type_check_binary_expr, above) must see the SAME boundary shape the
    // original type-check pass did, or a captured variable's re-resolution
    // here would spuriously fail to find it / miscount crossings. See
    // type_check_function_decl's identical marking (type_checker.c).
    checker->current_scope->is_function_boundary = 1;
    if (func_decl->params) {
        // Comptime value params Task 3: which comptime parameter (0-based,
        // declaration order) the next is_comptime_param entry below binds —
        // matches CallExprNode.comptime_value_args' own compact ordering
        // (ast.h) and codegen->active_comptime_values' indexing (codegen.h),
        // both set up by codegen_generate_comptime_function_instance
        // (monomorphize.c) before this function's body is walked.
        size_t comptime_idx = 0;
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
                    // Closures Task 2: backref to the SAME VarDeclNode the
                    // original type-check pass used, so a captured-param
                    // re-resolution here (type_check_identifier) stamps the
                    // real AST node instead of tripping the "unsupported
                    // binding form" rejection against a mirror-only NULL.
                    pv->decl_node = (struct ASTNode*)pd;
                    // Comptime value params Task 3: this instance's concrete
                    // value for a `comptime` parameter — see
                    // codegen_generate_comptime_function_instance's doc
                    // comment (monomorphize.c). Binds the SAME field set
                    // type_check_function_decl's template pass binds to a
                    // placeholder (type_checker.c); this mirror Variable's
                    // binding is what goo_fold_const_int_ctx actually
                    // resolves during THIS instance's codegen (e.g.
                    // codegen_generate_var_decl's array-length re-fold,
                    // below). Guarded on the index staying in range — always
                    // true once a comptime-param function only ever reaches
                    // here via codegen_generate_comptime_function_instance
                    // (codegen.c's skip-guard keeps it off the ordinary
                    // single-emission path); out-of-range is a silent no-op
                    // rather than a crash if that invariant is ever violated.
                    if (pd->is_comptime_param &&
                        comptime_idx < codegen->active_comptime_value_n) {
                        int64_t v = codegen->active_comptime_values[comptime_idx];
                        pv->has_const_int_value = 1;
                        pv->const_int_value = (uint64_t)v;
                        pv->comptime_value = comptime_value_new(COMPTIME_VALUE_INT);
                        if (pv->comptime_value) pv->comptime_value->int_value = v;
                    }
                    scope_add_variable(checker->current_scope, pv);
                }
            }
            if (pd->is_comptime_param) comptime_idx++;
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
            int param_is_captured = 0;
            if (param->type == AST_VAR_DECL) {
                VarDeclNode* pd = (VarDeclNode*)param;
                if (pd->name_count > 0 && pd->names) param_name = pd->names[0];
                param_is_captured = pd->is_captured;
            } else if (param->type == AST_IDENTIFIER) {
                // Defensive: keep the old path working for any path that
                // builds params as bare identifiers.
                param_name = ((IdentifierNode*)param)->name;
            }
            if (param_name) {
                LLVMValueRef param_value = LLVMGetParam(function, param_index);

                // Closures Task 2: a captured param's slot must outlive this
                // call frame (a closure returned/stored may read it after
                // this function returns) — force the heap-allocated path
                // regardless of M8b's escape-name promotion.
                LLVMValueRef param_alloca = codegen_alloc_local_promoted(
                    codegen, param_types[param_index], param_name, param_is_captured);
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
            // Closures Task 2: a captured named-return can be read/written
            // from a closure that outlives this call (e.g. a deferred or
            // returned literal touching it) — force heap promotion the same
            // as any other captured local/param.
            LLVMValueRef slot = codegen_alloc_local_promoted(codegen, llvm_ft, rname, fd->is_captured);
            LLVMBuildStore(codegen->builder, LLVMConstNull(llvm_ft), slot);
            ValueInfo* vi = value_info_new(rname, slot, ft);
            vi->is_lvalue = 1;
            vi->is_initialized = 1;
            vscope_add(codegen, vi);
            // Mirror into the type-checker scope so re-checks from codegen
            // (e.g. binary-expr type resolution) can resolve the name.
            Variable* rv = variable_new(rname, ft, fd->base.pos);
            if (rv) {
                rv->is_initialized = 1;
                rv->decl_node = (struct ASTNode*)fd;  // Closures Task 2 (see param mirror above)
                scope_add_variable(checker->current_scope, rv);
            }
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

    // Comptime value params Task 3 (generalized in fix round 2): `decl->
    // node_type` was resolved ONCE during the template body-check pass
    // (type_check_function_decl), with any comptime parameter bound to
    // Step 3's PLACEHOLDER value — so an array length depending on it
    // (`var buf [n]int`, `var m [2][n]int`, `buf := [n]int{}`) is baked
    // into this cached Type with the placeholder, shared by every
    // monomorphized instance of this function since they all codegen from
    // the SAME template AST node (this exact VarDeclNode). Whenever a
    // comptime instance is active (active_comptime_value_n > 0 — set by
    // codegen_generate_comptime_function_instance, monomorphize.c, which
    // rebinds each comptime param's mirror Variable to THIS instance's
    // concrete value in codegen_generate_function_decl's param loop, before
    // the body is walked), re-derive the FULL declared type fresh from its
    // AST type node via type_from_ast against the checker's CURRENT scope:
    // its AST_ARRAY_TYPE case folds every length (at every nesting depth)
    // with goo_fold_const_int_ctx, which now resolves the comptime param to
    // this instance's literal. This is the array-length analogue of
    // codegen_type_to_llvm's TYPE_PARAM substitution (type_mapping.c),
    // which re-resolves a generic template's types per instance for the
    // exact same "one shared cached Type, many instances" reason. Round 1's
    // narrower version rebuilt only the OUTERMOST length of an explicit
    // `[expr]elem` annotation — `var m [2][n]int` kept its placeholder
    // INNER length and `buf := [n]int{}` (no annotation; the type rides on
    // the literal, handled in codegen_generate_array_lit) got nothing, both
    // compiling clean and then bounds-panicking at runtime. Gated (fix
    // round 6, C-r5) on the cached type containing a comptime_length-FLAGGED
    // array (goo_type_contains_comptime_array) — NOT just any array: an
    // unflagged array's template resolution was never placeholder-tainted
    // (its length doesn't reference the comptime param), and re-deriving it
    // here against the MIRROR scope split-brained under a block-local
    // `const n = 3` shadowing the param — the checker had resolved the
    // shadow, the mirror-scope re-derivation resolved the PARAM (the mirror
    // pre-registers params before any body statement, so at re-derivation
    // time the shadow may not even exist yet in the mirror chain). A
    // flagged array's length references the param by construction, so its
    // mirror-scope resolution is exactly right. Only the LOCAL var_type is
    // replaced, never decl->node_type: the template node is shared across
    // instances.
    if (codegen->active_comptime_value_n > 0 && goo_type_contains_comptime_array(var_type)) {
        ASTNode* type_node = var_decl->type;
        // Short decl with the type riding on an array literal
        // (`buf := [n]int{}`): re-derive from the literal's own type node.
        if (!type_node && var_decl->values &&
            var_decl->values->type == AST_ARRAY_LITERAL) {
            type_node = ((ArrayLitNode*)var_decl->values)->array_type;
        }
        if (type_node) {
            Type* fresh = type_from_ast(checker, type_node);
            // Fix round 3 (minor 3): a failed re-derivation is a HARD
            // codegen failure, not a fall-back to the placeholder type.
            // type_from_ast already emitted the real, positioned diagnostic
            // (e.g. "array length must be non-negative" for a negative
            // comptime instance value) — but on the CHECKER's error counter,
            // which codegen success doesn't consult; continuing with the
            // placeholder previously emitted a placeholder-sized binary
            // with exit 0 despite the printed error.
            if (!fresh) {
                codegen_error(codegen, decl->pos,
                    "cannot instantiate declared type for this comptime instance");
                return 0;
            }
            var_type = fresh;
        } else if (var_decl->values && var_decl->name_count == 1) {
            // Fix round 5 (C-r4): an INFERRED declaration (`c := a`) has no
            // AST type node anywhere — neither an annotation nor an
            // array-literal RHS — so the branches above never fired and
            // `c`'s alloca silently kept the template-cached PLACEHOLDER
            // type while `a` (already re-derived) carried the real length:
            // compiled clean, bounds-panicked at runtime on the first
            // c[1] access. Resolve the RHS expression's type against the
            // mirror scope instead: an identifier RHS resolves to the
            // mirror Variable's already-re-derived type, and a general
            // expression re-checks against the same instance-bound scope
            // (the established codegen re-invocation pattern). Chained
            // inference (`d := c`) works because each fixed decl registers
            // its mirror Variable and ValueInfo with the re-derived type
            // below. Scoped to single-name decls (multi-name destructures
            // never bind a whole comptime-length array to one name — their
            // node_type is a struct/error-union, which the contains-array
            // gate above already filters out; the guard is belt and
            // braces). Same hard-fail contract as the annotation branch:
            // never fall back to the placeholder.
            Type* fresh = type_check_expression(checker, var_decl->values);
            if (!fresh || !goo_type_contains_array(fresh)) {
                codegen_error(codegen, decl->pos,
                    "cannot resolve inferred type for this comptime instance");
                return 0;
            }
            var_type = fresh;
        }
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
        // Closures Task 2: is_captured is a single flag on the shared
        // VarDeclNode covering both destructured names — if EITHER n or err
        // is captured, promote both (safe over-promotion via the same
        // uniformity trick; see codegen_alloc_local_promoted).
        LLVMValueRef val_alloca = codegen_alloc_local_promoted(codegen, value_llvm, nm0, var_decl->is_captured);
        LLVMBuildStore(codegen->builder, value, val_alloca);
        ValueInfo* vi0 = value_info_new(nm0, val_alloca, value_type);
        vi0->is_lvalue = 1;
        vi0->is_initialized = 1;
        vscope_add(codegen, vi0);
        Variable* tv0 = variable_new(nm0, value_type, decl->pos);
        if (tv0) {
            tv0->is_initialized = 1;
            tv0->decl_node = (struct ASTNode*)var_decl;  // Closures Task 2
            scope_add_variable(checker->current_scope, tv0);
        }

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
        LLVMValueRef err_alloca = codegen_alloc_local_promoted(codegen, err_llvm, nm1, var_decl->is_captured);
        LLVMBuildStore(codegen->builder, err_val, err_alloca);
        ValueInfo* vi1 = value_info_new(nm1, err_alloca, err_type);
        vi1->is_lvalue = 1;
        vi1->is_initialized = 1;
        vscope_add(codegen, vi1);
        Variable* tv1 = variable_new(nm1, err_type, decl->pos);
        if (tv1) {
            tv1->is_initialized = 1;
            tv1->decl_node = (struct ASTNode*)var_decl;  // Closures Task 2
            scope_add_variable(checker->current_scope, tv1);
        }

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
                Type* key_type = idx_expr->expr->node_type->data.map.key_type;
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

                // Box a concrete key into an interface-typed map key BEFORE
                // slot-packing (Task 2) — no-op for every non-interface-keyed
                // map.
                if (!codegen_box_map_key_if_needed(codegen, checker, key_val, key_type, decl->pos)) {
                    value_info_free(map_val);
                    value_info_free(key_val);
                    return 0;
                }
                // Pack the key into its i64 slot (string keys: char* ptrtoint,
                // never the value-boxing path — codegen_map_key_to_slot).
                LLVMValueRef kp = codegen_map_key_to_slot(codegen, checker, key_val, key_type);

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

        // comma-ok type assertion: `v, ok := x.(T)` — sibling of the
        // comma-ok map read above (Task 2 of type assertions). Compute the
        // vtable-pointer match via codegen_interface_assert_match (shared
        // with the single-return arm in expression_codegen.c and Task 3's
        // type switch), then branch/phi between the unboxed concrete
        // (match) and T's zero value (miss — no panic, unlike single-
        // return), mirroring codegen_map_slot_to_value's zero-guard shape
        // (codegen.c ~574-601). Pack {v, ok} into the same {V, i1} aggregate
        // the generic ExtractValue loop below binds name0→v, name1→ok.
        if (var_decl->name_count == 2 && var_decl->is_short_decl &&
            var_decl->values->type == AST_TYPE_ASSERT) {
            TypeAssertNode* ta = (TypeAssertNode*)var_decl->values;
            Type* iface_type = ta->expr->node_type;
            Type* target = var_decl->values->node_type;
            if (iface_type && target) {
                ValueInfo* iv = codegen_generate_expression(codegen, checker, ta->expr);
                if (!iv) {
                    codegen_error(codegen, decl->pos,
                                  "Failed to evaluate comma-ok type assertion operand");
                    return 0;
                }
                LLVMValueRef iface_val = iv->llvm_value;
                if (iv->is_lvalue) {
                    LLVMTypeRef ity = codegen_type_to_llvm(codegen, iface_type);
                    if (ity) {
                        iface_val = LLVMBuildLoad2(codegen->builder, ity, iface_val, "ta.operand");
                    }
                }
                value_info_free(iv);

                LLVMTypeRef target_llvm = codegen_type_to_llvm(codegen, target);
                if (!target_llvm) {
                    codegen_error(codegen, decl->pos,
                                  "Failed to lower comma-ok type assertion target type");
                    return 0;
                }

                LLVMValueRef ta_v = NULL;
                LLVMValueRef ta_match = NULL;

                // Interface-target RTTI, Task 2: `v, ok := x.(I)` where I is
                // an interface reuses codegen_interface_target_match — its
                // `built` output is ALREADY the zero target-interface value
                // on a miss (the primitive's own nil-guard + per-candidate
                // select phi, interface_codegen.c), so {v, ok} is just
                // {built, match} directly. No ta.load/ta.done block dance
                // (that unbox+zero-phi shape below is for a CONCRETE target
                // only — unboxing a concrete value out of `data` makes no
                // sense when the target itself is an interface).
                if (target->kind == TYPE_INTERFACE) {
                    LLVMValueRef built = NULL;
                    ta_match = codegen_interface_target_match(codegen, checker, iface_val, target,
                                                              &built);
                    if (!ta_match || !built) {
                        codegen_error(codegen, decl->pos,
                                      "Failed to build comma-ok interface-target assertion");
                        return 0;
                    }
                    ta_v = built;
                } else {
                    LLVMValueRef ta_data = NULL;
                    ta_match = codegen_interface_assert_match(codegen, checker, iface_val,
                                                              iface_type, target, &ta_data);
                    if (!ta_match) {
                        codegen_error(codegen, decl->pos,
                                      "Failed to build comma-ok type assertion compare");
                        return 0;
                    }

                    // Branch/phi: on miss go straight to ta_done (V = zero,
                    // no unbox attempted); on match, unbox in ta_load, then
                    // join.
                    LLVMBasicBlockRef ta_entry_bb = LLVMGetInsertBlock(codegen->builder);
                    LLVMValueRef ta_fn = LLVMGetBasicBlockParent(ta_entry_bb);
                    LLVMBasicBlockRef ta_load_bb = LLVMAppendBasicBlockInContext(codegen->context, ta_fn, "ta.load");
                    LLVMBasicBlockRef ta_done_bb = LLVMAppendBasicBlockInContext(codegen->context, ta_fn, "ta.done");
                    LLVMBuildCondBr(codegen->builder, ta_match, ta_load_bb, ta_done_bb);

                    codegen_set_insert_point(codegen, ta_load_bb);
                    LLVMValueRef ta_loaded = codegen_interface_assert_unbox(codegen, target, ta_data);
                    if (!ta_loaded) {
                        codegen_error(codegen, decl->pos,
                                      "Failed to unbox comma-ok type assertion value");
                        return 0;
                    }
                    LLVMBuildBr(codegen->builder, ta_done_bb);
                    LLVMBasicBlockRef ta_load_exit_bb = LLVMGetInsertBlock(codegen->builder);

                    codegen_set_insert_point(codegen, ta_done_bb);
                    LLVMValueRef ta_phi = LLVMBuildPhi(codegen->builder, target_llvm, "ta.v");
                    LLVMValueRef ta_zero = LLVMConstNull(target_llvm);
                    LLVMValueRef ta_inc_vals[2] = { ta_zero, ta_loaded };
                    LLVMBasicBlockRef ta_inc_bbs[2] = { ta_entry_bb, ta_load_exit_bb };
                    LLVMAddIncoming(ta_phi, ta_inc_vals, ta_inc_bbs, 2);
                    ta_v = ta_phi;
                }

                LLVMTypeRef ta_i1t = LLVMInt1TypeInContext(codegen->context);
                LLVMTypeRef ta_agg_fields[2] = { target_llvm, ta_i1t };
                LLVMTypeRef ta_agg_type = LLVMStructTypeInContext(codegen->context, ta_agg_fields, 2, 0);
                LLVMValueRef ta_agg = LLVMGetUndef(ta_agg_type);
                ta_agg = LLVMBuildInsertValue(codegen->builder, ta_agg, ta_v,     0, "ta_commaok_v");
                ta_agg = LLVMBuildInsertValue(codegen->builder, ta_agg, ta_match, 1, "ta_commaok_agg");

                rhs = value_info_new(NULL, ta_agg, var_type);
            }
        }

        // comma-ok channel receive: `v, ok := <-ch` (Task 5) — sibling of the
        // comma-ok map/type-assert arms above. Calls goo_chan_recv EXACTLY
        // ONCE (the pre-fix behavior fell through to the generic single-LHS
        // loop below, which evaluates var_decl->values once per name — for
        // name_count==2 that is TWICE — so the second call blocked on the
        // now-drained channel and the runtime aborted with a spurious
        // deadlock). Its i32 status return feeds `ok` (truncated to i1) and
        // its void* out-param feeds `v`, packed into the same {V, i1}
        // aggregate the generic ExtractValue loop below unpacks.
        //
        // Deliberately NOT routed through codegen_generate_channel_recv
        // (lowlevel_codegen.c) — that helper discards the status and is the
        // single-value receive's only caller (expression_codegen.c); reusing
        // it here would need either a second call (the bug) or an ABI change
        // that risks perturbing the single-value receive's IR, which must
        // stay byte-identical (FROZEN, Task 5 brief). Duplicating the small
        // alloca/call/load sequence at this destructuring site keeps that
        // path untouched — the same trade-off the map/type-assert arms above
        // already made.
        if (var_decl->name_count == 2 && var_decl->is_short_decl &&
            var_decl->values->type == AST_UNARY_EXPR &&
            ((UnaryExprNode*)var_decl->values)->operator == TOKEN_ARROW) {
            UnaryExprNode* unary = (UnaryExprNode*)var_decl->values;

            ValueInfo* channel_val = codegen_generate_expression(codegen, checker, unary->operand);
            if (!channel_val) {
                codegen_error(codegen, decl->pos, "Failed to evaluate comma-ok channel receive operand");
                return 0;
            }

            Type* chan_goo = channel_val->goo_type;
            Type* elem_goo = (chan_goo && chan_goo->kind == TYPE_CHANNEL)
                             ? chan_goo->data.channel.element_type : NULL;
            LLVMTypeRef element_type = elem_goo
                ? codegen_type_to_llvm(codegen, elem_goo)
                : LLVMInt32TypeInContext(codegen->context);  // fallback, mirrors codegen_generate_channel_recv

            LLVMValueRef result_alloca = LLVMBuildAlloca(codegen->builder, element_type, "recv_result");
            LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
            LLVMValueRef result_ptr = LLVMBuildBitCast(codegen->builder, result_alloca, void_ptr_type, "result_as_void_ptr");

            LLVMTypeRef param_types_recv[] = { void_ptr_type, void_ptr_type };
            LLVMTypeRef recv_func_type = LLVMFunctionType(LLVMInt32TypeInContext(codegen->context), param_types_recv, 2, 0);
            LLVMValueRef recv_func = LLVMGetNamedFunction(codegen->module, "goo_chan_recv");
            if (!recv_func) {
                recv_func = LLVMAddFunction(codegen->module, "goo_chan_recv", recv_func_type);
            }

            // The one and only goo_chan_recv call for this statement.
            LLVMValueRef recv_args[] = { channel_val->llvm_value, result_ptr };
            LLVMValueRef status = LLVMBuildCall2(codegen->builder, recv_func_type, recv_func,
                                                  recv_args, 2, "commaok_recv_status");
            LLVMValueRef received_value = LLVMBuildLoad2(codegen->builder, element_type, result_alloca, "commaok_received_value");

            value_info_free(channel_val);

            // goo_chan_recv returns 1 on success, 0 on failure (runtime.c:
            // closed channel with no data parked). No `close()` builtin
            // reaches user code in v1 yet, so on every currently-reachable
            // path this receive either delivers a value (status=1, ok=true)
            // or blocks forever — status=0/ok=false is wired correctly for
            // when close() ships but is not exercised by any v1 program.
            LLVMTypeRef i1t = LLVMInt1TypeInContext(codegen->context);
            LLVMValueRef ok_bit = LLVMBuildTrunc(codegen->builder, status, i1t, "commaok_recv_ok");

            LLVMTypeRef agg_fields[2] = { element_type, i1t };
            LLVMTypeRef agg_type = LLVMStructTypeInContext(codegen->context, agg_fields, 2, 0);
            LLVMValueRef agg = LLVMGetUndef(agg_type);
            agg = LLVMBuildInsertValue(codegen->builder, agg, received_value, 0, "commaok_recv_v");
            agg = LLVMBuildInsertValue(codegen->builder, agg, ok_bit,         1, "commaok_recv_agg");

            rhs = value_info_new(NULL, agg, var_type);
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
            // Closures Task 2: see the error-union destructure path above —
            // one is_captured flag covers every destructured name.
            LLVMValueRef field_alloca = codegen_alloc_local_promoted(codegen, field_llvm, nm, var_decl->is_captured);
            LLVMBuildStore(codegen->builder, field_val, field_alloca);
            ValueInfo* vi = value_info_new(nm, field_alloca, field_type);
            vi->is_lvalue = 1;
            vi->is_initialized = 1;
            vscope_add(codegen, vi);
            Variable* tv = variable_new(nm, field_type, decl->pos);
            if (tv) {
                tv->is_initialized = 1;
                tv->decl_node = (struct ASTNode*)var_decl;  // Closures Task 2
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
            // Closures Task 2: promote to the heap if a nested func literal
            // captures this declaration (checker-stamped is_captured) — its
            // slot must outlive this frame.
            alloca_inst = codegen_alloc_local_promoted(codegen, llvm_type, var_name, var_decl->is_captured);
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

                // Fix round 6 (M-r5b): instance-time enforcement of the
                // array-length compatibility the checker DEFERRED for a
                // comptime-length array initializer (`var b [4]int = a` —
                // type_check_var_decl's comptime_len_deferred). Both types
                // here are instance-real; a genuine mismatch is a clean,
                // instance-named compile failure instead of an invalid-IR
                // store. Mirrors the assignment arm (expression_codegen.c)
                // and the call-argument arm (call_codegen.c) exactly.
                if (var_type && init_value->goo_type &&
                    var_type->kind == TYPE_ARRAY &&
                    init_value->goo_type->kind == TYPE_ARRAY &&
                    (var_type->data.array.comptime_length ||
                     init_value->goo_type->data.array.comptime_length) &&
                    var_type->data.array.length != init_value->goo_type->data.array.length) {
                    codegen_error(codegen, decl->pos,
                        "cannot initialize [%zu]-length array from [%zu]-length array in comptime instance '%s'",
                        var_type->data.array.length,
                        init_value->goo_type->data.array.length,
                        codegen->symbol_override ? codegen->symbol_override : "?");
                    value_info_free(init_value);
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

        if (!vscope_add(codegen, value_info)) {
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
            tv->decl_node = (struct ASTNode*)var_decl;  // Closures Task 2
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
                if (!vscope_add(codegen, vi)) {
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
                if (!vscope_add(codegen, vi)) {
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
                if (!vscope_add(codegen, value_info)) {
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
        
        if (!vscope_add(codegen, value_info)) {
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

