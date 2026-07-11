#include "codegen.h"
#include "comptime.h"
#include "value_scope.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Statement code generation: the statement dispatcher and every
// statement kind (block, expr, if, for, return, go, defer, select,
// unsafe, asm). Split from function_codegen.c (refactor, no behavior
// change) — declarations (func/var/const) stay there.

// Defined in src/types/type_checker.c. Registers a synthetic defer-snapshot
// binding so the deferred call re-type-checked at function exit resolves its
// rewritten `__goo_deferN_argM` arguments instead of erroring. Forward-declared
// here (rather than in the shared types.h) to keep this change scoped.
void type_checker_declare_synthetic(TypeChecker* checker, const char* name, Type* type);


#if LLVM_AVAILABLE
// Loop-context stack (break/continue targets, labeled break/continue, goto
// labels): pushed on entry to a for-loop/switch/select body, popped on
// exit. Codegen hardening R1 moved this state off CodeGenerator into
// codegen->cfctx (ControlFlowContext, codegen_cfctx.h) and the push/pop/
// find/get-or-create logic that used to live here as file-static helpers
// (codegen_push_loop/codegen_pop_loop/codegen_push_break_scope/codegen_
// get_or_create_label_block) into src/codegen/cfctx.c as cfctx_push_loop/
// cfctx_pop/cfctx_push_break_scope/cfctx_get_or_create_goto_block — see
// that file and codegen_cfctx.h for the full API and field-by-field detail.

// Arena-regions early-exit free: defined below, used by the break/continue
// arms of codegen_generate_statement above its definition.
static void codegen_emit_arena_frees(CodeGenerator* codegen, int min_loop_depth);

// arena-goto fix: defined below, used by the AST_GOTO_STMT arm of
// codegen_generate_statement above its definition.
static void codegen_emit_arena_frees_to_depth(CodeGenerator* codegen, int target_arena_depth);

// --- defer codegen state -------------------------------------------------
// Per-defer LLVM state that the header's FunctionInfo cannot carry (it only
// stores the call AST nodes in `deferred_calls`). This array is kept in
// lock-step with current_function_info->deferred_calls and is reset when a
// function registers its first defer (deferred_count == 0). The compiler is
// single-threaded and emits one function at a time (a function is fully
// generated — defers registered AND emitted at every exit — before the next
// begins), so a file-static cache keyed by the owning FunctionInfo is safe.
//
// Why snapshots + an active flag (vs. re-walking the stored call at exit):
//   * arg snapshot at defer-time gives Go's "args evaluated at defer-time"
//     semantics and lets the deferred call reference defer-time values after
//     the body block's locals have been truncated out of the value table
//     (the old re-walk-at-exit emitted "Undefined identifier" for any local
//     argument on the fall-off-the-end path);
//   * the runtime active flag makes a defer inside a not-taken branch NOT run
//     (the old design ran every statically-registered defer unconditionally).
typedef struct {
    LLVMValueRef  active_flag;   // i1 alloca, 0 at entry, set to 1 when reached
    size_t        arg_count;     // number of snapshotted call arguments
    LLVMValueRef* arg_slots;     // entry-block alloca per snapshotted argument
    Type**        arg_types;     // goo type of each snapshot (for the exit load)
    char**        arg_names;     // synthetic identifier names bound at exit
    // Destructive-splice fix: the synthetic identifier(s) built at defer-time
    // to stand in for the original call arguments / method receiver. These
    // are NOT linked into the shared template AST here — codegen_emit_
    // deferred_calls splices them into the call node ONLY for the duration of
    // a single emission and restores the originals immediately after (see
    // that function). Ownership: both fields are allocated once per defer
    // statement (via ast_identifier_new) and owned by this DeferCodegenInfo;
    // freed in defer_info_reset. They are never attached to the permanent
    // template AST, so nothing else can reach or double-free them, and the
    // ORIGINAL argument/receiver nodes are untouched — still owned by the
    // template AST exactly as the parser built it.
    ASTNode*      args_synth_head; // synthetic chain standing in for call->args (NULL if no args)
    ASTNode*      recv_synth;      // synthetic identifier standing in for the method receiver (NULL if none)
} DeferCodegenInfo;

static DeferCodegenInfo* g_defer_info = NULL;
static size_t            g_defer_info_count = 0;
static size_t            g_defer_info_capacity = 0;
static FunctionInfo*     g_defer_info_owner = NULL;

static void defer_info_reset(FunctionInfo* owner) {
    for (size_t i = 0; i < g_defer_info_count; i++) {
        for (size_t j = 0; j < g_defer_info[i].arg_count; j++)
            free(g_defer_info[i].arg_names[j]);
        free(g_defer_info[i].arg_names);
        free(g_defer_info[i].arg_slots);
        free(g_defer_info[i].arg_types);
        // Free the synthetic identifier nodes built for this defer's transient
        // AST splice (see codegen_emit_deferred_calls and the struct comment
        // above). They are only ever linked into the call node for the
        // duration of a single emission and unlinked immediately after, so by
        // the time we get here nothing references them — this is their sole
        // owner and sole free site.
        ast_node_free(g_defer_info[i].args_synth_head);
        ast_node_free(g_defer_info[i].recv_synth);
    }
    g_defer_info_count = 0;
    g_defer_info_owner = owner;
}

// Initialise a defer's active flag to 0 unconditionally in the entry block,
// so a defer placed inside a branch that is never taken stays inactive.
static void defer_entry_store_zero(CodeGenerator* cg, LLVMValueRef flag, LLVMTypeRef i1) {
    LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->builder);
    LLVMBasicBlockRef entry = cg->current_function_info
        ? cg->current_function_info->entry_block
        : LLVMGetEntryBasicBlock(cg->current_function);
    LLVMValueRef term = entry ? LLVMGetBasicBlockTerminator(entry) : NULL;
    if (term) LLVMPositionBuilderBefore(cg->builder, term);
    else      LLVMPositionBuilderAtEnd(cg->builder, entry);
    LLVMBuildStore(cg->builder, LLVMConstNull(i1), flag);
    if (cur) LLVMPositionBuilderAtEnd(cg->builder, cur);
}
#endif

// F6: `a, b := v1, v2` / `a, b = v1, v2`. Simultaneous-evaluation semantics:
// pass 1 evaluates EVERY right-hand side to an rvalue (loading lvalue operands
// now, before any store), pass 2 binds/stores. So `a, b = b, a` swaps — both
// rvalues are read before either target is written.
int codegen_generate_multi_assign(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_MULTI_ASSIGN) return 0;
    MultiAssignNode* ma = (MultiAssignNode*)stmt;

    // Destructuring assignment `a, b = f()`: a SINGLE multi-return value (a
    // struct) spread across the two targets. Detected by a values chain shorter
    // than the target count. Evaluate the struct once, ExtractValue each field,
    // and store into the corresponding target lvalue. (The `:=` two-target
    // single-value form is a VarDeclNode, handled elsewhere — this is the `=`
    // assignment form.)
    size_t vcount = 0;
    for (ASTNode* v = ma->values; v; v = v->next) vcount++;
    if (vcount == 1 && ma->count == 2 && !ma->is_short_decl) {
        ValueInfo* rhs = codegen_generate_expression(codegen, checker, ma->values);
        if (!rhs) {
            codegen_error(codegen, stmt->pos, "Failed to evaluate destructure RHS");
            return 0;
        }
        size_t i = 0;
        for (ASTNode* t = ma->targets; t; t = t->next, i++) {
            // `_` discards its field — no lvalue to resolve, no store.
            if (t->type == AST_IDENTIFIER &&
                strcmp(((IdentifierNode*)t)->name, "_") == 0) {
                continue;
            }
            ValueInfo* target = codegen_emit_lvalue_address(codegen, checker, t);
            if (!target || !target->is_lvalue) {
                codegen_error(codegen, t->pos,
                              "destructure target must be an addressable lvalue");
                value_info_free(rhs);
                return 0;
            }
            LLVMValueRef field = LLVMBuildExtractValue(codegen->builder,
                                                       rhs->llvm_value, (unsigned)i, "destr");
            // Coerce the field to the target's width before storing (same
            // rule as the two-value pass-2 store below): an int-returning
            // f() spread into narrower targets otherwise stores 8 bytes
            // over each narrow alloca and corrupts the stack. Per-field
            // signedness comes from the multi-return's TYPE_STRUCT payload
            // (mirrors the `:=` destructure in function_codegen.c).
            Type* field_goo = (rhs->goo_type &&
                               rhs->goo_type->kind == TYPE_STRUCT &&
                               i < rhs->goo_type->data.struct_type.field_count)
                              ? rhs->goo_type->data.struct_type.fields[i].type
                              : NULL;
            // Box a concrete returned field into an interface-typed target's
            // {vtable, data} value (mirrors the two-value pass-2 store
            // below). Without the box the raw struct is stored over the
            // interface slot and the vtable reads as stack garbage.
            if (target->goo_type && target->goo_type->kind == TYPE_INTERFACE &&
                field_goo && field_goo->kind != TYPE_INTERFACE) {
                LLVMValueRef boxed = codegen_interface_box(codegen, checker,
                                                           target->goo_type,
                                                           field_goo, field);
                if (!boxed) {
                    codegen_error(codegen, t->pos,
                                  "failed to box value into interface on destructure");
                    value_info_free(rhs);
                    return 0;
                }
                field = boxed;
            }
            if (target->goo_type) {
                LLVMTypeRef tt = codegen_type_to_llvm(codegen, target->goo_type);
                if (tt) {
                    field = codegen_coerce_to_type(codegen, field,
                                                   field_goo ? type_is_signed(field_goo) : 1,
                                                   tt);
                }
            }
            LLVMBuildStore(codegen->builder, field, target->llvm_value);
        }
        value_info_free(rhs);
        return 1;
    }

    // Pass 1: evaluate all RHS values up front (the load-bearing step).
    LLVMValueRef rvals[2];
    Type* rtypes[2];
    size_t n = 0;
    for (ASTNode* v = ma->values; v && n < ma->count && n < 2; v = v->next) {
        ValueInfo* vi = codegen_generate_expression(codegen, checker, v);
        if (!vi) {
            codegen_error(codegen, stmt->pos, "Failed to evaluate multi-assign value");
            return 0;
        }
        // Read the current value of an lvalue operand NOW, before any store.
        if (vi->is_lvalue && vi->goo_type) {
            LLVMTypeRef vt = codegen_type_to_llvm(codegen, vi->goo_type);
            if (vt) {
                vi->llvm_value = LLVMBuildLoad2(codegen->builder, vt, vi->llvm_value, "ma_rval");
                vi->is_lvalue = 0;
            }
        }
        rvals[n] = vi->llvm_value;
        rtypes[n] = vi->goo_type;
        n++;
        value_info_free(vi);
    }

    // Pass 2: bind (`:=`) or store (`=`) each target.
    size_t i = 0;
    for (ASTNode* t = ma->targets; t; t = t->next, i++) {
        if (i >= n) {
            codegen_error(codegen, stmt->pos, "multi-assign target/value count mismatch");
            return 0;
        }

        if (ma->is_short_decl) {
            const char* nm = ((IdentifierNode*)t)->name;
            // `_` is a discard — no slot, no binding (mirrors the typecheck).
            if (strcmp(nm, "_") == 0) {
                continue;
            }
            LLVMTypeRef llty = codegen_type_to_llvm(codegen, rtypes[i]);
            if (!llty) {
                codegen_error(codegen, t->pos, "multi-assign: no type for '%s'", nm);
                return 0;
            }
            LLVMValueRef slot = codegen_alloc_local(codegen, llty, nm);
            LLVMBuildStore(codegen->builder, rvals[i], slot);
            ValueInfo* vi = value_info_new(nm, slot, rtypes[i]);
            vi->is_lvalue = 1;
            vi->is_initialized = 1;
            vscope_add(codegen, vi);
            // Mirror the var-decl path: keep the checker scope populated so
            // later codegen re-checks of `nm` resolve (ignore dup failures).
            Variable* tv = variable_new(nm, rtypes[i], t->pos);
            if (tv) {
                tv->is_initialized = 1;
                scope_add_variable(checker->current_scope, tv);
            }
        } else {
            // `_` discards its value — no lvalue to resolve, no store.
            if (t->type == AST_IDENTIFIER &&
                strcmp(((IdentifierNode*)t)->name, "_") == 0) {
                continue;
            }
            ValueInfo* target = codegen_emit_lvalue_address(codegen, checker, t);
            if (!target || !target->is_lvalue) {
                codegen_error(codegen, t->pos,
                              "multi-assign target must be an addressable lvalue");
                return 0;
            }
            // Box a concrete implementer into the interface's {vtable, data}
            // value when assigning into an interface-typed lvalue (mirrors the
            // var-decl init / call-arg boxing). interface→interface needs no
            // box — same layout, store the struct directly.
            LLVMValueRef sval = rvals[i];
            if (target->goo_type && target->goo_type->kind == TYPE_INTERFACE &&
                rtypes[i] && rtypes[i]->kind != TYPE_INTERFACE) {
                LLVMValueRef boxed = codegen_interface_box(codegen, checker,
                                                           target->goo_type,
                                                           rtypes[i], rvals[i]);
                if (!boxed) {
                    codegen_error(codegen, t->pos,
                                  "failed to box value into interface on assignment");
                    return 0;
                }
                sval = boxed;
            }
            // Coerce a mismatched-width RHS to the target's width before
            // storing (mirrors the single-assign TOKEN_ASSIGN arm). An
            // untyped-int rvalue is an i64; storing it raw into an i8/i16/i32
            // slot writes 8 bytes over a narrower alloca — clobbering
            // adjacent locals and the epilogue (correct prints, then
            // SIGSEGV/garbage). The shared helper no-ops on kinds it doesn't
            // handle (aggregates, pointers, matching types).
            if (target->goo_type && rtypes[i]) {
                LLVMTypeRef tt = codegen_type_to_llvm(codegen, target->goo_type);
                if (tt) {
                    sval = codegen_coerce_to_type(codegen, sval,
                                                  type_is_signed(rtypes[i]), tt);
                }
            }
            // Do NOT free `target`: for an identifier it aliases the live
            // value-table entry (freeing it would undefine the variable).
            LLVMBuildStore(codegen->builder, sval, target->llvm_value);
        }
    }
    return 1;
#endif
}

int codegen_generate_statement(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
    if (!codegen || !checker || !stmt) return 0;
    
    switch (stmt->type) {
        case AST_BLOCK_STMT:
            return codegen_generate_block_stmt(codegen, checker, stmt);
        case AST_EXPR_STMT:
            return codegen_generate_expr_stmt(codegen, checker, stmt);
        case AST_VAR_DECL:
            return codegen_generate_var_decl(codegen, checker, stmt);
        case AST_CONST_DECL:
            // Local const inside a function body (`const n = 64`). Reuses the
            // package-const emitter; the resulting ValueInfo is added to the
            // function-scoped value table so reads within the body resolve it.
            return codegen_generate_const_decl(codegen, checker, stmt);
        case AST_MULTI_ASSIGN:
            return codegen_generate_multi_assign(codegen, checker, stmt);
        case AST_IF_STMT:
            return codegen_generate_if_stmt(codegen, checker, stmt);
        case AST_IF_LET_STMT: {
            // Desugar `if let v = expr { … } [else { … }]` to:
            //   evaluate expr (TYPE_NULLABLE struct {i1 is_null, T value})
            //   br is_null, .else_or_skip, .then
            //   .then: alloca v; v = ExtractValue 1; codegen then_stmt
            //   .else: codegen else_stmt if present
            //   .exit
            // Declarations are hoisted to the top of the block so the
            // CompCert build (C99-strict, no mid-block decls) accepts.
            IfLetStmtNode* il;
            ValueInfo* nv;
            LLVMValueRef raw;
            LLVMValueRef is_null;
            LLVMBasicBlockRef then_bb;
            LLVMBasicBlockRef else_bb;
            LLVMBasicBlockRef exit_bb;
            Type* inner_type;
            int then_ok;
            int else_ok;

            il = (IfLetStmtNode*)stmt;
            nv = codegen_generate_expression(codegen, checker, il->nullable_expr);
            if (!nv) return 0;
            raw = nv->llvm_value;
            if (nv->is_lvalue && nv->goo_type) {
                LLVMTypeRef lt = codegen_type_to_llvm(codegen, nv->goo_type);
                if (lt) raw = LLVMBuildLoad2(codegen->builder, lt, raw, "il_load");
            }
            is_null = LLVMBuildExtractValue(codegen->builder, raw, 0, "is_null");
            then_bb = codegen_create_block(codegen, "iflet.then");
            else_bb = codegen_create_block(codegen, "iflet.else");
            exit_bb = codegen_create_block(codegen, "iflet.exit");
            LLVMBuildCondBr(codegen->builder, is_null, else_bb, then_bb);

            codegen_set_insert_point(codegen, then_bb);
            inner_type = nv->goo_type ? nv->goo_type->data.nullable.base_type : NULL;
            scope_push(checker);
            if (il->var_name && inner_type) {
                LLVMTypeRef inner_llvm = codegen_type_to_llvm(codegen, inner_type);
                LLVMValueRef val = LLVMBuildExtractValue(codegen->builder, raw, 1, il->var_name);
                LLVMValueRef alloca_v = codegen_alloc_local(codegen, inner_llvm, il->var_name);
                LLVMBuildStore(codegen->builder, val, alloca_v);
                ValueInfo* vi = value_info_new(il->var_name, alloca_v, inner_type);
                vi->is_lvalue = 1; vi->is_initialized = 1;
                vscope_add(codegen, vi);
                {
                    Variable* tv = variable_new(il->var_name, inner_type, stmt->pos);
                    if (tv) { tv->is_initialized = 1; scope_add_variable(checker->current_scope, tv); }
                }
            }
            then_ok = il->then_stmt ? codegen_generate_statement(codegen, checker, il->then_stmt) : 1;
            scope_pop(checker);
            // Only add the branch if the then block didn't already emit a
            // terminator (e.g. `return` inside `if let q = p { return ... }`
            // adds a ret, and a second br would be "terminator in middle of BB").
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)))
                LLVMBuildBr(codegen->builder, exit_bb);

            codegen_set_insert_point(codegen, else_bb);
            else_ok = il->else_stmt ? codegen_generate_statement(codegen, checker, il->else_stmt) : 1;
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)))
                LLVMBuildBr(codegen->builder, exit_bb);

            codegen_set_insert_point(codegen, exit_bb);
            // If both branches terminated (e.g. both `return`), exit_bb has no
            // predecessors — no br instructions jump to it.  Emit `unreachable`
            // so the block is well-formed for the LLVM verifier.  The normal
            // fall-through case (at least one branch without a terminator) adds
            // a `br exit_bb`, giving exit_bb a predecessor, so this guard is a
            // no-op in that case.
            if (!LLVMGetFirstUse(LLVMBasicBlockAsValue(exit_bb)))
                LLVMBuildUnreachable(codegen->builder);
            value_info_free(nv);
            return then_ok && else_ok;
        }
        case AST_LABEL_STMT: {
            // gofmt-syntax-b Task 1: `L: stmt`. Stash the name in
            // pending_label so a wrapped for/switch/select/type-switch's OWN
            // push (cfctx_push_loop/cfctx_push_break_scope) tags its
            // frame with it; then generate the wrapped statement normally.
            // Any OTHER statement shape (not itself a construct that
            // pushes) just leaves pending_label unconsumed — cleared
            // unconditionally below so it can never leak onto some later,
            // unrelated sibling statement's push.
            //
            // gofmt-syntax-b Task 2 (P1.6): every label also gets (or
            // reuses) a real LLVMBasicBlockRef via
            // cfctx_get_or_create_goto_block, so any `goto` in the
            // function — forward or backward — has a branch target. LLVM
            // blocks have no implicit fallthrough, so if the current block
            // hasn't already terminated (this label directly follows an
            // ordinary statement, not a goto/return/break), link it in
            // with an explicit `br` before moving the insertion point.
            LabelStmtNode* label = (LabelStmtNode*)stmt;
            LLVMBasicBlockRef label_bb = cfctx_get_or_create_goto_block(codegen, label->name);
            if (!label_bb) {
                codegen_error(codegen, stmt->pos, "too many labels in one function (max 64)");
                return 0;
            }
            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
                LLVMBuildBr(codegen->builder, label_bb);
            }
            codegen_set_insert_point(codegen, label_bb);
            codegen->cfctx.pending_label = label->name;
            int ok = label->stmt ? codegen_generate_statement(codegen, checker, label->stmt) : 1;
            codegen->cfctx.pending_label = NULL;
            return ok;
        }
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
        case AST_SWITCH_STMT:
            return codegen_generate_switch_stmt(codegen, checker, stmt);
        case AST_TYPE_SWITCH:
            return codegen_generate_type_switch_stmt(codegen, checker, stmt);
        case AST_UNSAFE_STMT:
            return codegen_generate_unsafe_stmt(codegen, checker, stmt);
        case AST_ASM_STMT:
            return codegen_generate_asm_stmt(codegen, checker, stmt);
        case AST_ARENA_BLOCK:
            return codegen_generate_arena_stmt(codegen, checker, stmt);
        case AST_BREAK_STMT:
            if (codegen->cfctx.loop_depth == 0) { codegen_error(codegen, stmt->pos, "break outside loop"); return 0; }
            // Free the arenas pushed INSIDE the loop we are breaking out of
            // (arena_loop_depth >= this loop's depth), before the branch — an
            // enclosing-loop arena (shallower depth) is left alive.
            codegen_emit_arena_frees(codegen, codegen->cfctx.loop_depth);
            LLVMBuildBr(codegen->builder, codegen->cfctx.loop_break_bb[codegen->cfctx.loop_depth - 1]);
            // Subsequent statements in this block are unreachable; start a fresh
            // block so later codegen has a valid (dead) insertion point.
            codegen_set_insert_point(codegen, codegen_create_block(codegen, "after.break"));
            return 1;
        case AST_CONTINUE_STMT:
            // A NULL continue target means the innermost scope is a break-only
            // switch/select with no enclosing loop — `continue` is illegal there.
            if (codegen->cfctx.loop_depth == 0 ||
                codegen->cfctx.loop_continue_bb[codegen->cfctx.loop_depth - 1] == NULL) {
                codegen_error(codegen, stmt->pos, "continue outside loop"); return 0;
            }
            // Free the arenas pushed inside this loop iteration before jumping
            // to the loop post/condition; the next iteration re-creates them.
            codegen_emit_arena_frees(codegen, codegen->cfctx.loop_depth);
            LLVMBuildBr(codegen->builder, codegen->cfctx.loop_continue_bb[codegen->cfctx.loop_depth - 1]);
            codegen_set_insert_point(codegen, codegen_create_block(codegen, "after.continue"));
            return 1;
        case AST_BREAK_LABEL_STMT: {
            // gofmt-syntax-b Task 1: `break L` — walk the break-scope stack
            // top-down (innermost first, matching Go's "nearest enclosing
            // labeled statement" rule) for a frame tagged with L. Unlike
            // bare `break`, a labeled break may match ANY frame kind (loop
            // OR switch/select/type-switch) at ANY depth, not just the
            // innermost — cfctx_find_label (codegen_cfctx.h) is that walk.
            BreakLabelStmtNode* bl = (BreakLabelStmtNode*)stmt;
            int target = cfctx_find_label(&codegen->cfctx, bl->label);
            if (target < 0) {
                codegen_error(codegen, stmt->pos, "label '%s' not defined or not enclosing", bl->label);
                return 0;
            }
            // Free every arena pushed at or inside the target frame — same
            // formula as bare `break` (which passes loop_depth, i.e. the
            // target-plus-one for the innermost frame); generalized to
            // target+1 for an arbitrary enclosing frame.
            codegen_emit_arena_frees(codegen, target + 1);
            LLVMBuildBr(codegen->builder, codegen->cfctx.loop_break_bb[target]);
            codegen_set_insert_point(codegen, codegen_create_block(codegen, "after.break"));
            return 1;
        }
        case AST_CONTINUE_LABEL_STMT: {
            // gofmt-syntax-b Task 1: `continue L` — same stack walk as
            // `break L`, but restricted to loop_is_loop==1 frames: Go only
            // lets `continue` target an enclosing FOR, never a switch/
            // select even if that construct happens to carry the label —
            // cfctx_find_loop_label (codegen_cfctx.h) is that restricted walk.
            ContinueLabelStmtNode* cl = (ContinueLabelStmtNode*)stmt;
            int target = cfctx_find_loop_label(&codegen->cfctx, cl->label);
            if (target < 0) {
                codegen_error(codegen, stmt->pos, "label '%s' not defined or not enclosing", cl->label);
                return 0;
            }
            codegen_emit_arena_frees(codegen, target + 1);
            LLVMBuildBr(codegen->builder, codegen->cfctx.loop_continue_bb[target]);
            codegen_set_insert_point(codegen, codegen_create_block(codegen, "after.continue"));
            return 1;
        }
        case AST_GOTO_STMT: {
            // gofmt-syntax-b Task 2 (P1.6): `goto L`. The type checker has
            // already proven L is declared somewhere in this function
            // (positioned "undefined label" error otherwise, well before
            // codegen runs) — get or (for a goto that lexically precedes
            // its label, i.e. a backward jump) create L's block and branch
            // to it unconditionally.
            //
            // arena-goto fix: free every arena this goto EXITS before the
            // branch, exactly like break/continue above. Unlike those,
            // goto has no loop-depth relationship to its target at all —
            // only arena LEXICAL nesting matters — so the free count comes
            // from the target label's arena-nesting depth, computed at
            // type-check time (type_check_statement's AST_GOTO_STMT case,
            // type_checker.c) and looked up here by name from the SAME
            // checker->tc_fctx.goto_label_names table that case walks. The checker
            // has already proven that depth's arena-chain is a prefix of
            // this goto's own (else it rejected the program with "goto
            // into arena block is not supported" before codegen ever
            // ran), so every arena from that depth up to the goto's
            // current codegen->arena_depth is safe to free and none of
            // them are shared with the label's continuation.
            GotoStmtNode* got = (GotoStmtNode*)stmt;
            int target_arena_depth = 0;
            if (checker && got->label) {
                for (size_t i = 0; i < checker->tc_fctx.goto_label_count; i++) {
                    if (checker->tc_fctx.goto_label_names[i] &&
                        strcmp(checker->tc_fctx.goto_label_names[i], got->label) == 0) {
                        target_arena_depth = (int)checker->tc_fctx.goto_label_arena_depth[i];
                        break;
                    }
                }
            }
            codegen_emit_arena_frees_to_depth(codegen, target_arena_depth);
            LLVMBasicBlockRef target = cfctx_get_or_create_goto_block(codegen, got->label);
            if (!target) {
                codegen_error(codegen, stmt->pos, "too many labels in one function (max 64)");
                return 0;
            }
            LLVMBuildBr(codegen->builder, target);
            // Fresh dead-code continuation block, SAME pattern as break/
            // continue above — NOT the block-level terminated-block skip
            // this task's own design note initially assumed. Probed and
            // rejected: `codegen_generate_block_stmt`'s guard does not
            // merely skip the dead statements after a goto, it `break`s
            // the whole statement-list loop, so any LABEL textually
            // following the goto in the same block (the ordinary forward-
            // goto shape, e.g. `goto Skip; ...; Skip: ...`) would never get
            // positioned at all — an unreachable, unterminated orphan
            // block, an LLVM verifier failure. Giving the dead tail its own
            // insertion point lets that later AST_LABEL_STMT run normally
            // and correctly reuse/position the real target block.
            codegen_set_insert_point(codegen, codegen_create_block(codegen, "after.goto"));
            return 1;
        }
        case AST_FALLTHROUGH_STMT:
            // gofmt-syntax-b Task 3 (P1.7): `fallthrough` — br to the
            // CURRENT case body's fallthrough target, pushed by
            // codegen_generate_switch_stmt (statement_codegen.c) around
            // each clause body's emission; top of stack is always the
            // innermost/currently-emitting case body. The type checker
            // (type_check_switch_like_body) has already proven this is
            // legal — final statement of a non-last expression-switch
            // clause — before codegen ever runs; the guards below are
            // defensive only (mirrors cfctx_push_loop's "too deep"
            // convention, codegen_cfctx.h).
            if (codegen->cfctx.fallthrough_depth == 0 ||
                codegen->cfctx.fallthrough_target_bb[codegen->cfctx.fallthrough_depth - 1] == NULL) {
                codegen_error(codegen, stmt->pos, "fallthrough has no target");
                return 0;
            }
            LLVMBuildBr(codegen->builder,
                        codegen->cfctx.fallthrough_target_bb[codegen->cfctx.fallthrough_depth - 1]);
            // Same dead-code-continuation pattern as break/continue/goto
            // above — fallthrough is a terminator, so subsequent (illegal,
            // per the type checker) statements in this clause body still
            // get a valid, if dead, insertion point.
            codegen_set_insert_point(codegen, codegen_create_block(codegen, "after.fallthrough"));
            return 1;
        case AST_COMPTIME_BLOCK: {
            // M11-block-dispatch: `comptime { ... }` blocks produce no
            // runtime code in the MVP scope. The type checker already
            // validated the body in subtask M11-types-const-stub; here
            // we simply emit nothing for the block, treating it as a
            // pure compile-time-only construct. A future task can lift
            // the values escaping from a comptime block to outer-scope
            // constants — out of scope for the MVP.
            return 1;
        }
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

    // Block scope: bindings declared inside this block must not outlive it.
    // Snapshot the value-table high-water mark and truncate back to it on the
    // way out, so an inner `x := ...` cannot leak past the block (Go scoping).
    // Mirrors the match-arm teardown in composite_codegen.c. We reset the size
    // without freeing the truncated ValueInfo* (matching existing behavior;
    // the leak is a separate follow-up).
    size_t pre_block_vt_size = vscope_enter(codegen);

    ASTNode* current = block->statements;
    while (current) {
        // Skip emission once the current block already has a terminator (e.g.
        // an if-let whose branches both return left an `unreachable` exit_bb);
        // appending later statements would put a terminator mid-block.
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)))
            break;
        if (!codegen_generate_statement(codegen, checker, current)) {
            return 0;
        }
        current = current->next;
    }

    // Restore on the normal-end and early-break paths.
    vscope_exit(codegen, pre_block_vt_size);
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
    
    // Auto-load if the condition is an lvalue (e.g. a bare field selector
    // like `if p.A` — the selector returns the field's ADDRESS; branching
    // on it is a verifier error, not a bool test).
    LLVMValueRef cond_val = condition->llvm_value;
    if (condition->is_lvalue && condition->goo_type) {
        LLVMTypeRef ct = codegen_type_to_llvm(codegen, condition->goo_type);
        if (ct) cond_val = LLVMBuildLoad2(codegen->builder, ct, cond_val, "cond_load");
    }
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

#if LLVM_AVAILABLE
// Load an rvalue from a ValueInfo, dereferencing if it is an lvalue. Mirrors
// the load logic used by the if-let lowering so switch tags and case
// expressions that resolve to variables compare by value, not by address.
static LLVMValueRef switch_rvalue(CodeGenerator* codegen, ValueInfo* vi) {
    LLVMValueRef v = vi->llvm_value;
    if (vi->is_lvalue && vi->goo_type) {
        LLVMTypeRef lt = codegen_type_to_llvm(codegen, vi->goo_type);
        if (lt) v = LLVMBuildLoad2(codegen->builder, lt, v, "switch.load");
    }
    return v;
}
#endif

// Expression switch lowering. A Go switch has no implicit fallthrough, so it
// lowers to a chain of equality comparisons: each case expression is compared
// against the tag, branching to that clause's body on a match or to the next
// test otherwise. If no case matches, control flows to the default clause (if
// present) or to the merge block. Each clause body branches to merge.
int codegen_generate_switch_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available for switch statements");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_SWITCH_STMT) return 0;

    SwitchStmtNode* sw = (SwitchStmtNode*)stmt;

    // Evaluate the tag once, up front.
    ValueInfo* tag_vi = codegen_generate_expression(codegen, checker, sw->tag);
    if (!tag_vi) return 0;
    LLVMValueRef tag_val = switch_rvalue(codegen, tag_vi);
    // P1-4: a string tag can't be compared with icmp ({ptr,i64} is an invalid
    // ICmp operand); each case is matched with goo_string_eq instead.
    int tag_is_string = tag_vi->goo_type && tag_vi->goo_type->kind == TYPE_STRING;
    // Correctness-followups arc 3, task 3 (found during implementation, not
    // pre-existing scope): a float tag ALSO can't be compared with icmp —
    // LLVM's ICmp requires integer/pointer operands, so `switch f { case
    // 2.5: }` (f float64, case a SAME-KIND float — already type-checked as
    // comparable before this task touched anything) crashed the verifier
    // ("Invalid operand types for ICmp instruction") on every path, not just
    // the untyped-int-constant-case shape this task's checker fix newly
    // accepts. Confirmed via a same-kind float/float probe at this arc's
    // HEAD, pre-dating any change in this commit — a pre-existing, never-
    // triggered gap (no float-tag switch golden existed). Without this fix,
    // the checker fix above would turn the untyped-int-into-float-tag case
    // from a clean reject back into a verifier crash — the exact regression
    // class this arc's Global Constraints forbid ("no LLVM verifier text may
    // reach users"). Matched with LLVMBuildFCmp/LLVMRealOEQ, the same pair
    // used for `==` on float operands elsewhere in codegen (see
    // expression_codegen.c's binary-expr float arm).
    int tag_is_float = tag_vi->goo_type && type_is_float(tag_vi->goo_type);
    value_info_free(tag_vi);

    LLVMBasicBlockRef merge_block = codegen_create_block(codegen, "switch.merge");

    size_t clause_count = 0;
    for (ASTNode* c = sw->cases; c; c = c->next) clause_count++;
    if (clause_count == 0) {
        LLVMBuildBr(codegen->builder, merge_block);
        codegen_set_insert_point(codegen, merge_block);
        return 1;
    }

    // One body block per clause; remember the default clause's body if any.
    LLVMBasicBlockRef* body_blocks = malloc(sizeof(LLVMBasicBlockRef) * clause_count);
    if (!body_blocks) return 0;
    LLVMBasicBlockRef default_body = NULL;
    size_t i = 0;
    for (ASTNode* c = sw->cases; c; c = c->next, i++) {
        body_blocks[i] = codegen_create_block(codegen, "switch.case");
        if (((CaseClauseNode*)c)->exprs == NULL) default_body = body_blocks[i];
    }

    // Comparison chain: test each non-default clause's expressions in order.
    i = 0;
    for (ASTNode* c = sw->cases; c; c = c->next, i++) {
        CaseClauseNode* clause = (CaseClauseNode*)c;
        if (clause->exprs == NULL) continue;  // default tested last
        for (ASTNode* e = clause->exprs; e; e = e->next) {
            ValueInfo* ev = codegen_generate_expression(codegen, checker, e);
            if (!ev) { free(body_blocks); return 0; }
            LLVMValueRef ev_rv = switch_rvalue(codegen, ev);
            value_info_free(ev);
            LLVMValueRef cmp;
            if (tag_is_string) {
                // P1-4: match via goo_string_eq (returns i32 0/1) -> i1.
                LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_string_eq");
                if (!fn) { codegen_error(codegen, stmt->pos, "goo_string_eq not found in module"); free(body_blocks); return 0; }
                LLVMValueRef args[2] = { tag_val, ev_rv };
                LLVMValueRef eqi = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn),
                                                  fn, args, 2, "switch.streq");
                LLVMValueRef z = LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0);
                cmp = LLVMBuildICmp(codegen->builder, LLVMIntNE, eqi, z, "switch.cmp");
            } else if (tag_is_float) {
                // See tag_is_float's doc comment above: ICmp is invalid on
                // floating-point operands, so a float tag needs FCmp.
                cmp = LLVMBuildFCmp(codegen->builder, LLVMRealOEQ, tag_val, ev_rv, "switch.cmp");
            } else {
                cmp = LLVMBuildICmp(codegen->builder, LLVMIntEQ, tag_val, ev_rv, "switch.cmp");
            }
            LLVMBasicBlockRef next_test = codegen_create_block(codegen, "switch.test");
            LLVMBuildCondBr(codegen->builder, cmp, body_blocks[i], next_test);
            codegen_set_insert_point(codegen, next_test);
        }
    }
    // Fell through every test: go to default body, else merge.
    LLVMBuildBr(codegen->builder, default_body ? default_body : merge_block);

    // `break` inside a case must terminate the SWITCH (Go semantics), not the
    // enclosing loop. Push a break-only scope targeting merge_block; `continue`
    // still threads through to the enclosing loop (or errors if none).
    if (!cfctx_push_break_scope(&codegen->cfctx, merge_block)) {
        codegen_error(codegen, stmt->pos, "switch nested too deeply for break handling");
        free(body_blocks);
        return 0;
    }

    // Emit clause bodies. No implicit fallthrough: each body ends at merge
    // unless it ends in an explicit `fallthrough` statement.
    i = 0;
    for (ASTNode* c = sw->cases; c; c = c->next, i++) {
        CaseClauseNode* clause = (CaseClauseNode*)c;
        codegen_set_insert_point(codegen, body_blocks[i]);
        // gofmt-syntax-b Task 3 (P1.7): push THIS body's fallthrough
        // target — the NEXT clause's body block in SOURCE ORDER (the
        // default clause participates in source order per the Go spec,
        // exactly like body_blocks[] itself above), or NULL for the
        // switch's last clause. NULL is a defensive fallback only: the
        // type checker (type_check_switch_like_body) has already rejected
        // `fallthrough` in the last clause before codegen ever runs.
        if (codegen->cfctx.fallthrough_depth >= 32) {
            codegen_error(codegen, stmt->pos, "switch nested too deeply for fallthrough handling");
            cfctx_pop(&codegen->cfctx);
            free(body_blocks);
            return 0;
        }
        codegen->cfctx.fallthrough_target_bb[codegen->cfctx.fallthrough_depth] =
            (i + 1 < clause_count) ? body_blocks[i + 1] : NULL;
        codegen->cfctx.fallthrough_depth++;
        for (ASTNode* s = clause->body; s; s = s->next) {
            if (!codegen_generate_statement(codegen, checker, s)) {
                codegen->cfctx.fallthrough_depth--;
                cfctx_pop(&codegen->cfctx);
                free(body_blocks);
                return 0;
            }
        }
        codegen->cfctx.fallthrough_depth--;
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
            LLVMBuildBr(codegen->builder, merge_block);
        }
    }
    cfctx_pop(&codegen->cfctx);

    codegen_set_insert_point(codegen, merge_block);
    free(body_blocks);
    return 1;
#endif
}

// Type switch lowering (Task 3 of type assertions): `switch [v :=] x.(type)
// { case T1: … case Tn, Tm: … default: … }` desugars to an ordered chain of
// vtable-pointer-identity compares, exactly mirroring
// codegen_generate_switch_stmt's icmp-chain shape above but comparing
// vtable pointers (via the shared codegen_interface_assert_match /
// codegen_interface_assert_unbox helpers from Task 2, NOT reimplemented
// here) instead of tag values.
//
// PERF (extract-once): the operand expression is evaluated exactly ONCE,
// up front (`iface_val`) — never re-evaluated per case, which matters both
// for perf and for correctness if the operand has side effects (a call,
// e.g. `switch v := getShape().(type)`). Its `data` field (interface field
// 1) is ALSO extracted exactly once here and reused for every case's
// unbox — it is the SAME raw pointer regardless of which case ends up
// matching (only the unbox TARGET type differs per case), so every call
// below passes `data_out = NULL` to codegen_interface_assert_match and
// unboxes through this cached `data` instead of letting the helper
// re-derive it per case. What is NOT hoisted: assert_match's internal
// `vt_have` (interface field 0) extraction — avoiding that would mean
// bypassing assert_match's pointer-target normalization (the *T-reuses-
// pointee's-vtable dance hardened in df41fb2) and hand-rolling the compare,
// which the brief for this task explicitly rules out ("do not reimplement
// the vtable compare"). Each repeated ExtractValue of the SAME iface_val
// SSA value is a single pure instruction with no re-evaluation of anything
// — cheap, and not a correctness concern either way.
int codegen_generate_type_switch_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available for type switch statements");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_TYPE_SWITCH) return 0;

    TypeSwitchNode* tsw = (TypeSwitchNode*)stmt;
    Type* iface_type = tsw->expr->node_type;
    if (!iface_type) {
        codegen_error(codegen, stmt->pos,
                      "internal: type switch operand missing resolved type");
        return 0;
    }

    ValueInfo* iv = codegen_generate_expression(codegen, checker, tsw->expr);
    if (!iv) return 0;
    LLVMValueRef iface_val = iv->llvm_value;
    // Same is_lvalue-load idiom as AST_TYPE_ASSERT's operand handling
    // (expression_codegen.c) and codegen_interface_dispatch's call site.
    if (iv->is_lvalue) {
        LLVMTypeRef ity = codegen_type_to_llvm(codegen, iface_type);
        if (ity) iface_val = LLVMBuildLoad2(codegen->builder, ity, iface_val, "tsw.operand");
    }
    value_info_free(iv);

    LLVMBasicBlockRef merge_block = codegen_create_block(codegen, "tsw.merge");

    size_t clause_count = 0;
    for (ASTNode* c = tsw->cases; c; c = c->next) clause_count++;
    if (clause_count == 0) {
        LLVMBuildBr(codegen->builder, merge_block);
        codegen_set_insert_point(codegen, merge_block);
        return 1;
    }

    LLVMBasicBlockRef* body_blocks = malloc(sizeof(LLVMBasicBlockRef) * clause_count);
    if (!body_blocks) return 0;
    // Interface-target RTTI, Task 3: for a single-type clause whose case
    // type is itself an interface (`case Speaker:`), the match-building loop
    // below builds the bound `v` value (codegen_interface_target_match's
    // `built` {vtable,data} out-param) at the SAME time it builds the match
    // bit — there is no separate unbox step the way a concrete case has one.
    // That `built` value must survive from the match loop into the second
    // (body-emitting) loop below, so it's stashed here per-clause. Only
    // populated for single-type interface clauses; NULL (unused) for every
    // concrete/nil/multi-type clause.
    LLVMValueRef* built_vals = calloc(clause_count, sizeof(LLVMValueRef));
    if (!built_vals) { free(body_blocks); return 0; }
    LLVMBasicBlockRef default_body = NULL;
    size_t i = 0;
    for (ASTNode* c = tsw->cases; c; c = c->next, i++) {
        body_blocks[i] = codegen_create_block(codegen, "tsw.case");
        if (((TypeCaseNode*)c)->types == NULL) default_body = body_blocks[i];
    }

    // Extracted once (see header comment); reused for every case's unbox.
    LLVMValueRef data = LLVMBuildExtractValue(codegen->builder, iface_val, 1, "tsw.data");

    // Zero interface value {NULL,NULL} for `case nil:` — a nil interface
    // compares the WHOLE value (vtable AND data both null), not just data,
    // since a non-nil interface could (in principle) box a value whose
    // heap data pointer coincides with NULL only if the vtable were also
    // NULL, which never happens for a real boxed value.
    LLVMTypeRef iface_llvm = codegen_type_to_llvm(codegen, iface_type);
    LLVMValueRef zero_iface = iface_llvm ? LLVMConstNull(iface_llvm) : NULL;

    i = 0;
    for (ASTNode* c = tsw->cases; c; c = c->next, i++) {
        TypeCaseNode* clause = (TypeCaseNode*)c;
        if (!clause->types) continue;  // default tested last
        for (ASTNode* t = clause->types; t; t = t->next) {
            LLVMValueRef match;
            int is_nil = (t->type == AST_LITERAL && ((LiteralNode*)t)->literal_type == TOKEN_NIL);
            if (is_nil) {
                if (!zero_iface) {
                    codegen_error(codegen, t->pos, "internal: cannot lower nil interface compare");
                    free(body_blocks);
                    free(built_vals);
                    return 0;
                }
                LLVMValueRef vt_have = LLVMBuildExtractValue(codegen->builder, iface_val, 0, "tsw.vt");
                LLVMValueRef zero_vt = LLVMBuildExtractValue(codegen->builder, zero_iface, 0, "tsw.zvt");
                LLVMValueRef zero_data = LLVMBuildExtractValue(codegen->builder, zero_iface, 1, "tsw.zdata");
                LLVMValueRef vt_eq = LLVMBuildICmp(codegen->builder, LLVMIntEQ, vt_have, zero_vt, "tsw.vteq");
                LLVMValueRef data_eq = LLVMBuildICmp(codegen->builder, LLVMIntEQ, data, zero_data, "tsw.deq");
                match = LLVMBuildAnd(codegen->builder, vt_eq, data_eq, "tsw.nileq");
            } else {
                Type* case_type = t->node_type;
                if (!case_type) {
                    codegen_error(codegen, t->pos, "internal: type switch case missing resolved type");
                    free(body_blocks);
                    free(built_vals);
                    return 0;
                }
                if (case_type->kind == TYPE_INTERFACE) {
                    // Interface-target RTTI, Task 3: `case Speaker:` routes
                    // through the closed-world enumeration primitive (Task 1)
                    // instead of the concrete-target vtable-pointer compare —
                    // it also hands back the built (T,Speaker) interface
                    // value that IS `v` on a match, so stash it for the
                    // body-emitting loop below (only meaningful when this is
                    // the clause's sole case type — see that loop's
                    // single_concrete check).
                    LLVMValueRef built = NULL;
                    match = codegen_interface_target_match(codegen, checker, iface_val,
                                                           case_type, &built);
                    if (!match) {
                        codegen_error(codegen, t->pos,
                            "internal: cannot build type switch interface-target match");
                        free(body_blocks);
                        free(built_vals);
                        return 0;
                    }
                    if (clause->types == t && t->next == NULL) {
                        built_vals[i] = built;
                    }
                } else {
                    match = codegen_interface_assert_match(codegen, checker, iface_val,
                                                           iface_type, case_type, NULL);
                    if (!match) {
                        codegen_error(codegen, t->pos, "internal: cannot build type switch vtable compare");
                        free(body_blocks);
                        free(built_vals);
                        return 0;
                    }
                }
            }
            LLVMBasicBlockRef next_test = codegen_create_block(codegen, "tsw.test");
            LLVMBuildCondBr(codegen->builder, match, body_blocks[i], next_test);
            codegen_set_insert_point(codegen, next_test);
        }
    }
    // Fell through every test: go to default body, else merge.
    LLVMBuildBr(codegen->builder, default_body ? default_body : merge_block);

    // `break` inside a case must terminate the SWITCH (Go semantics), matching
    // the plain switch's break-scope convention above.
    if (!cfctx_push_break_scope(&codegen->cfctx, merge_block)) {
        codegen_error(codegen, stmt->pos, "type switch nested too deeply for break handling");
        free(body_blocks);
        free(built_vals);
        return 0;
    }

    i = 0;
    for (ASTNode* c = tsw->cases; c; c = c->next, i++) {
        TypeCaseNode* clause = (TypeCaseNode*)c;
        codegen_set_insert_point(codegen, body_blocks[i]);

        // Mirror `v` to the type-checker's OWN scope (parallels the
        // for-range arms above, e.g. ~line 922) — NOT just codegen's value
        // table. codegen_generate_call_expr re-invokes type_check_call_expr
        // during method-call codegen (to re-derive recv_offset etc., which
        // isn't persisted between passes), and that re-check resolves the
        // receiver via checker->current_scope, independent of codegen's own
        // value table. Without this, `v.Method()` inside a case body fails
        // typecheck's identifier resolution ("Undefined variable 'v'")
        // even though `v.Field` (which never re-invokes the checker) works
        // fine — verified live: the first version of this function only
        // registered `v` in codegen's value table and broke exactly this
        // way on a method-call probe.
        scope_push(checker);
        if (tsw->bind_name) {
            const char* vname = ((IdentifierNode*)tsw->bind_name)->name;
            size_t case_type_count = 0;
            for (ASTNode* t = clause->types; t; t = t->next) case_type_count++;
            // Single-type case (and not `case nil:`, which has no Type* to
            // bind) narrows `v` to that case's own type — unboxed to the
            // concrete value for a concrete case, or (Task 3) the ALREADY-
            // BUILT target-interface value stashed in built_vals[i] for an
            // interface case, since codegen_interface_target_match built
            // that value as part of the match test itself (no separate
            // unbox step exists for an interface target). Multi-type/
            // default/nil keeps `v` at the operand's interface type —
            // mirrors type_check_type_switch_stmt's identical rule exactly.
            int single_type_case = (case_type_count == 1 && clause->types &&
                                    clause->types->type != AST_LITERAL);
            Type* bind_type = single_type_case ? clause->types->node_type : iface_type;
            LLVMTypeRef bind_llvm = codegen_type_to_llvm(codegen, bind_type);
            if (!bind_llvm) {
                codegen_error(codegen, c->pos, "internal: cannot lower type switch bind type");
                scope_pop(checker);
                cfctx_pop(&codegen->cfctx);
                free(body_blocks);
                free(built_vals);
                return 0;
            }
            LLVMValueRef bound_val;
            if (single_type_case && bind_type->kind == TYPE_INTERFACE) {
                bound_val = built_vals[i];
                if (!bound_val) {
                    codegen_error(codegen, c->pos,
                        "internal: missing built interface-target value for type switch case");
                    scope_pop(checker);
                    cfctx_pop(&codegen->cfctx);
                    free(body_blocks);
                    free(built_vals);
                    return 0;
                }
            } else if (single_type_case) {
                bound_val = codegen_interface_assert_unbox(codegen, bind_type, data);
                if (!bound_val) {
                    codegen_error(codegen, c->pos, "internal: cannot unbox type switch case value");
                    scope_pop(checker);
                    cfctx_pop(&codegen->cfctx);
                    free(body_blocks);
                    free(built_vals);
                    return 0;
                }
            } else {
                bound_val = iface_val;  // multi-type/default/nil: v stays the interface value
            }
            LLVMValueRef slot = codegen_alloc_local(codegen, bind_llvm, vname);
            LLVMBuildStore(codegen->builder, bound_val, slot);
            ValueInfo* vi = value_info_new(vname, slot, bind_type);
            if (vi) {
                vi->is_lvalue = 1;
                vi->is_initialized = 1;
                vscope_add(codegen, vi);
            }
            Variable* cv = variable_new(vname, bind_type, c->pos);
            if (cv) {
                cv->is_initialized = 1;
                scope_add_variable(checker->current_scope, cv);
            }
        }

        for (ASTNode* s = clause->body; s; s = s->next) {
            if (!codegen_generate_statement(codegen, checker, s)) {
                scope_pop(checker);
                cfctx_pop(&codegen->cfctx);
                free(body_blocks);
                free(built_vals);
                return 0;
            }
        }
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
            LLVMBuildBr(codegen->builder, merge_block);
        }
        scope_pop(checker);
    }
    cfctx_pop(&codegen->cfctx);

    codegen_set_insert_point(codegen, merge_block);
    free(body_blocks);
    free(built_vals);
    return 1;
#endif
}

// Range-over-map codegen: cursor-based walk of the runtime's linked-list
// map via goo_map_iter_next_sv (include/runtime.h). Mirrors the
// slice/array/string range skeleton in codegen_generate_for_stmt below
// (cond/body/post/exit blocks, break/continue wiring, mirrored
// type-checker scope) but cannot share its body: that skeleton indexes a
// {ptr,len} aggregate (extractvalue + GEP), while a map is a bare
// GooMapSV* pointer (see type_mapping.c's TYPE_MAP case) whose entries are
// visited by calling the iterator, not by indexing.
//
// `map_ptr` is the already-evaluated (and auto-loaded, if the range
// expression was an lvalue) GooMapSV* value; `map_type` is its Goo Type
// (TYPE_MAP), carrying key_type/value_type. Returns 0 and leaves an error
// on codegen->diagnostics via codegen_error on failure (matching the
// slice/array/string arm's convention), else the body's success flag.
static int codegen_generate_map_range_loop(CodeGenerator* codegen, TypeChecker* checker,
                                            ForStmtNode* for_stmt, ASTNode* stmt,
                                            LLVMValueRef map_ptr, Type* map_type) {
    Type* key_type = map_type->data.map.key_type;
    Type* value_type = map_type->data.map.value_type;
    if (!value_type) {
        codegen_error(codegen, stmt->pos, "range over map: missing value type");
        return 0;
    }
    if (!key_type) {
        codegen_error(codegen, stmt->pos, "range over map: missing key type");
        return 0;
    }

    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);

    // Declare the runtime externs this loop calls, if not already present
    // in the module — same fallback-declare pattern codegen_alloc_local
    // uses for goo_alloc (function_codegen.c): avoids adding these to
    // runtime_integration.c's central table, which is out of scope for
    // this change.
    LLVMValueRef init_fn = LLVMGetNamedFunction(codegen->module, "goo_map_iter_init_sv");
    if (!init_fn) {
        LLVMTypeRef init_params[] = { ptr_type };
        LLVMTypeRef init_fn_type = LLVMFunctionType(ptr_type, init_params, 1, 0);
        init_fn = LLVMAddFunction(codegen->module, "goo_map_iter_init_sv", init_fn_type);
    }
    // int goo_map_iter_next_sv(GooMapEntrySV** cursor, int64_t* key_out,
    // int64_t* val_out) — both out-params are now i64 slots (Task 1's ABI
    // change); the key slot is unpacked to the declared key type via
    // codegen_map_slot_to_key below, same as the value slot always was.
    LLVMValueRef iter_fn = LLVMGetNamedFunction(codegen->module, "goo_map_iter_next_sv");
    if (!iter_fn) {
        LLVMTypeRef pp_type = LLVMPointerType(ptr_type, 0);
        LLVMTypeRef i64p = LLVMPointerType(i64, 0);
        LLVMTypeRef iter_params[] = { pp_type, i64p, i64p };
        LLVMTypeRef iter_fn_type = LLVMFunctionType(i32, iter_params, 3, 0);
        iter_fn = LLVMAddFunction(codegen->module, "goo_map_iter_next_sv", iter_fn_type);
    }

    // Cursor slot: GooMapEntrySV* (opaque — entry layout stays private to
    // runtime.c). Initialized via goo_map_iter_init_sv rather than a
    // GEP+load of GooMapSV's head field: the runtime call is NULL-safe, so
    // ranging over a nil map (`var m map[string]int`, Go's zero value)
    // yields a NULL cursor ⇒ zero iterations instead of faulting on the
    // head load — and codegen carries no mirror of GooMapSV's layout.
    LLVMValueRef init_args[1] = { map_ptr };
    LLVMValueRef head_val = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(init_fn),
                                           init_fn, init_args, 1, "map_head");

    LLVMValueRef cursor_alloca = codegen_alloc_local(codegen, ptr_type, "range_mcursor");
    LLVMBuildStore(codegen->builder, head_val, cursor_alloca);

    // Out-param scratch for the iterator call, reused every iteration. Both
    // are raw i64 slots now — key_slot_alloca held a char* pre-Task-2 when
    // only string keys existed; codegen_map_slot_to_key unpacks it to K.
    LLVMValueRef key_slot_alloca = codegen_alloc_local(codegen, i64, "range_mkey_slot");
    LLVMValueRef val_slot_alloca = codegen_alloc_local(codegen, i64, "range_mval_slot");

    LLVMTypeRef val_llvm = codegen_type_to_llvm(codegen, value_type);
    LLVMTypeRef key_llvm = codegen_type_to_llvm(codegen, key_type);
    if (!val_llvm || !key_llvm) {
        codegen_error(codegen, stmt->pos, "range over map: unresolvable key/value type");
        return 0;
    }

    // Allocate key/value vars (per-iteration) and register with codegen's
    // value table. `_` is bound like any other name (never looked up) —
    // matches the slice/array/string arm's convention just above.
    LLVMValueRef key_alloca = NULL, val_alloca = NULL;
    if (for_stmt->key_name) {
        key_alloca = codegen_alloc_local(codegen, key_llvm, for_stmt->key_name);
        ValueInfo* kv = value_info_new(for_stmt->key_name, key_alloca, key_type);
        kv->is_lvalue = 1;
        kv->is_initialized = 1;
        vscope_add(codegen, kv);
    }
    if (for_stmt->value_name) {
        val_alloca = codegen_alloc_local(codegen, val_llvm, for_stmt->value_name);
        ValueInfo* vv = value_info_new(for_stmt->value_name, val_alloca, value_type);
        vv->is_lvalue = 1;
        vv->is_initialized = 1;
        vscope_add(codegen, vv);
    }

    // Mirror loop vars to type-checker scope (parallels the slice/array/
    // string arm below).
    scope_push(checker);
    if (for_stmt->key_name) {
        Variable* kvar = variable_new(for_stmt->key_name, key_type, stmt->pos);
        if (kvar) { kvar->is_initialized = 1; scope_add_variable(checker->current_scope, kvar); }
    }
    if (for_stmt->value_name) {
        Variable* vvar = variable_new(for_stmt->value_name, value_type, stmt->pos);
        if (vvar) { vvar->is_initialized = 1; scope_add_variable(checker->current_scope, vvar); }
    }

    LLVMBasicBlockRef rcond = codegen_create_block(codegen, "maprange.cond");
    LLVMBasicBlockRef rbody = codegen_create_block(codegen, "maprange.body");
    LLVMBasicBlockRef rpost = codegen_create_block(codegen, "maprange.post");
    LLVMBasicBlockRef rexit = codegen_create_block(codegen, "maprange.exit");

    LLVMBuildBr(codegen->builder, rcond);

    // cond: goo_map_iter_next_sv(&cursor, &key_slot, &val_slot) != 0. The
    // call itself advances the cursor and fills the out-slots — there is no
    // separate "advance" step in the post block (see below).
    codegen_set_insert_point(codegen, rcond);
    LLVMValueRef iter_args[3] = { cursor_alloca, key_slot_alloca, val_slot_alloca };
    LLVMValueRef has_next = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(iter_fn),
                                           iter_fn, iter_args, 3, "map_has_next");
    LLVMValueRef cond_v = LLVMBuildICmp(codegen->builder, LLVMIntNE, has_next,
                                        LLVMConstInt(i32, 0, 0), "maprange_cond");
    LLVMBuildCondBr(codegen->builder, cond_v, rbody, rexit);

    // body: bind key (unpack the i64 slot to the declared key type via
    // codegen_map_slot_to_key — string keys rebuild the goo string aggregate
    // via goo_string_new, same copy-semantics as before this change; inline
    // keys just cast back) and value (cast the int64 slot to the declared V,
    // same convention as m[k] reads), then run the loop body.
    codegen_set_insert_point(codegen, rbody);
    if (key_alloca) {
        LLVMValueRef kslot = LLVMBuildLoad2(codegen->builder, i64, key_slot_alloca, "map_kslot");
        LLVMValueRef kval = codegen_map_slot_to_key(codegen, kslot, key_type);
        LLVMBuildStore(codegen->builder, kval, key_alloca);
    }
    if (val_alloca) {
        LLVMValueRef slot = LLVMBuildLoad2(codegen->builder, i64, val_slot_alloca, "map_vslot");
        LLVMValueRef v = codegen_map_slot_to_value(codegen, slot, value_type);
        LLVMBuildStore(codegen->builder, v, val_alloca);
    }

    if (!cfctx_push_loop(&codegen->cfctx, rexit, rpost)) {
        codegen_error(codegen, stmt->pos, "loop nesting too deep (max 32)");
        scope_pop(checker);
        return 0;
    }
    int body_ok = 1;
    if (for_stmt->body) {
        body_ok = codegen_generate_statement(codegen, checker, for_stmt->body);
    }
    cfctx_pop(&codegen->cfctx);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)))
        LLVMBuildBr(codegen->builder, rpost);

    // post: nothing to advance — goo_map_iter_next_sv already advanced the
    // cursor in place during the cond call. Just loop back.
    codegen_set_insert_point(codegen, rpost);
    LLVMBuildBr(codegen->builder, rcond);

    codegen_set_insert_point(codegen, rexit);
    scope_pop(checker);
    return body_ok;
}

// Range-over-channel codegen (P3.2): repeatedly calls goo_chan_recv; a 0
// status (closed+drained — P3.1's zero-value contract memsets the
// out-buffer on that terminal call, so the loop never sees stack garbage)
// is the loop-exit condition, mirroring how Go's compiler lowers `for v :=
// range ch`. Diverges from the slice/array/string skeleton for the same
// reason maps do (see codegen_generate_map_range_loop just above): a
// channel lowers to a bare opaque pointer (type_mapping.c TYPE_CHANNEL),
// not a {ptr,len} aggregate, and there is no length to index against —
// trying a receive and checking its status IS the only way to know the
// loop is done.
//
// Grammar quirk: the single-var form `for v := range ch` parses `v` into
// ForStmtNode.key_name (the slice/array/string index slot — the grammar
// predates channel range and has no dedicated production for it). There is
// no index for a channel, so key_name is reinterpreted here as the
// received element. The two-variable form is rejected in the type checker
// before codegen ever runs (type_check_for_stmt's TYPE_CHANNEL arm), so
// value_name is always NULL by the time this function is reached.
//
// `chan_ptr` is the already-evaluated (and auto-loaded, if the range
// expression was an lvalue) channel pointer; `chan_type` is its Goo Type
// (TYPE_CHANNEL), carrying element_type. Returns 0 and leaves an error on
// codegen->diagnostics via codegen_error on failure, else the body's
// success flag — same convention as the map/slice/array/string arms.
static int codegen_generate_channel_range_loop(CodeGenerator* codegen, TypeChecker* checker,
                                                ForStmtNode* for_stmt, ASTNode* stmt,
                                                LLVMValueRef chan_ptr, Type* chan_type) {
    Type* elem_type = chan_type->data.channel.element_type;
    if (!elem_type) {
        codegen_error(codegen, stmt->pos, "range over channel: missing element type");
        return 0;
    }

    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
    LLVMTypeRef elem_llvm = codegen_type_to_llvm(codegen, elem_type);
    if (!elem_llvm) {
        codegen_error(codegen, stmt->pos, "range over channel: unresolvable element type");
        return 0;
    }

    // Declare-if-needed, matching function_codegen.c's comma-ok receive
    // (~:2000-2005) — the pattern this brief points at. Each of the three
    // goo_chan_recv call sites (single-value receive in lowlevel_codegen.c,
    // comma-ok in function_codegen.c, this loop) declares independently
    // rather than sharing a central table, same trade-off
    // codegen_generate_map_range_loop makes for goo_map_iter_next_sv above.
    LLVMTypeRef recv_param_types[] = { void_ptr_type, void_ptr_type };
    LLVMTypeRef recv_func_type = LLVMFunctionType(i32, recv_param_types, 2, 0);
    LLVMValueRef recv_func = LLVMGetNamedFunction(codegen->module, "goo_chan_recv");
    if (!recv_func) {
        recv_func = LLVMAddFunction(codegen->module, "goo_chan_recv", recv_func_type);
    }

    // Element slot: allocated ONCE outside the loop and reused every
    // iteration — goo_chan_recv writes into it by pointer each call.
    LLVMValueRef elem_alloca = codegen_alloc_local(codegen, elem_llvm,
                                                   for_stmt->key_name ? for_stmt->key_name : "range_cv");
    if (for_stmt->key_name) {
        ValueInfo* vv = value_info_new(for_stmt->key_name, elem_alloca, elem_type);
        vv->is_lvalue = 1;
        vv->is_initialized = 1;
        vscope_add(codegen, vv);
    }

    // Mirror the loop var to type-checker scope (parallels the map/slice/
    // array/string arms).
    scope_push(checker);
    if (for_stmt->key_name) {
        Variable* vvar = variable_new(for_stmt->key_name, elem_type, stmt->pos);
        if (vvar) { vvar->is_initialized = 1; scope_add_variable(checker->current_scope, vvar); }
    }

    LLVMBasicBlockRef rcond = codegen_create_block(codegen, "chanrange.cond");
    LLVMBasicBlockRef rbody = codegen_create_block(codegen, "chanrange.body");
    LLVMBasicBlockRef rpost = codegen_create_block(codegen, "chanrange.post");
    LLVMBasicBlockRef rexit = codegen_create_block(codegen, "chanrange.exit");

    LLVMBuildBr(codegen->builder, rcond);

    // cond: goo_chan_recv(ch, &elem_slot) — blocks while the channel is open
    // and empty (correct Go semantics; the deadlock detector handles a
    // producer that never sends/closes), returns 1 with the received value
    // in elem_slot, or 0 (closed+drained, elem_slot zeroed by the runtime's
    // P3.1 contract) which is the loop-exit condition.
    codegen_set_insert_point(codegen, rcond);
    LLVMValueRef elem_ptr = LLVMBuildBitCast(codegen->builder, elem_alloca, void_ptr_type, "chanrange_elem_ptr");
    LLVMValueRef recv_args[2] = { chan_ptr, elem_ptr };
    LLVMValueRef status = LLVMBuildCall2(codegen->builder, recv_func_type, recv_func,
                                         recv_args, 2, "chanrange_status");
    LLVMValueRef cond_v = LLVMBuildICmp(codegen->builder, LLVMIntNE, status,
                                        LLVMConstInt(i32, 0, 0), "chanrange_cond");
    LLVMBuildCondBr(codegen->builder, cond_v, rbody, rexit);

    // body: elem_alloca already holds the received value (loaded by
    // goo_chan_recv itself, out-parameter style) — just run the loop body.
    codegen_set_insert_point(codegen, rbody);
    if (!cfctx_push_loop(&codegen->cfctx, rexit, rpost)) {
        codegen_error(codegen, stmt->pos, "loop nesting too deep (max 32)");
        scope_pop(checker);
        return 0;
    }
    int body_ok = 1;
    if (for_stmt->body) {
        body_ok = codegen_generate_statement(codegen, checker, for_stmt->body);
    }
    cfctx_pop(&codegen->cfctx);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)))
        LLVMBuildBr(codegen->builder, rpost);

    // post: nothing to advance — the next cond-block recv call does the
    // work (parallels the map arm's cursor-advances-in-the-cond-call note).
    codegen_set_insert_point(codegen, rpost);
    LLVMBuildBr(codegen->builder, rcond);

    codegen_set_insert_point(codegen, rexit);
    scope_pop(checker);
    return body_ok;
}

int codegen_generate_for_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_FOR_STMT) return 0;

    ForStmtNode* for_stmt = (ForStmtNode*)stmt;

    // For-range: desugar `for k[, v] := range expr { ... }` to an
    // indexed loop. Evaluate the range expression once, get its
    // length, then loop k from 0..len. If value_name is set, load
    // the element via the slice's underlying pointer + GEP.
    if (for_stmt->range_expr) {
        // Evaluate range expression once.
        ValueInfo* range_val = codegen_generate_expression(codegen, checker, for_stmt->range_expr);
        if (!range_val) return 0;
        // Auto-load if lvalue — EXCEPT for TYPE_ARRAY. An array lowers to a
        // raw LLVM [N x T] value, not a {ptr,len} struct like slices/strings,
        // so loading it here would discard the address we need for the GEP
        // below and load the (possibly large) aggregate for nothing; the
        // TYPE_ARRAY branch reads range_val->llvm_value directly instead.
        LLVMValueRef raw = range_val->llvm_value;
        if (range_val->is_lvalue && range_val->goo_type
            && range_val->goo_type->kind != TYPE_ARRAY) {
            LLVMTypeRef lt = codegen_type_to_llvm(codegen, range_val->goo_type);
            if (lt) raw = LLVMBuildLoad2(codegen->builder, lt, raw, "range_load");
        }

        // Map range diverges immediately: `raw` here is a bare GooMapSV*
        // pointer (see type_mapping.c TYPE_MAP), not the {ptr,len}
        // aggregate the extractvalue calls below assume, so it gets its
        // own loop skeleton rather than falling through.
        if (range_val->goo_type && range_val->goo_type->kind == TYPE_MAP) {
            int ok = codegen_generate_map_range_loop(codegen, checker, for_stmt, stmt,
                                                      raw, range_val->goo_type);
            value_info_free(range_val);
            return ok;
        }

        // Channel range diverges immediately too — `raw` here is the
        // channel's opaque pointer (already auto-loaded above, same as the
        // map case), not a {ptr,len} aggregate.
        if (range_val->goo_type && range_val->goo_type->kind == TYPE_CHANNEL) {
            int ok = codegen_generate_channel_range_loop(codegen, checker, for_stmt, stmt,
                                                          raw, range_val->goo_type);
            value_info_free(range_val);
            return ok;
        }

        // F7: range over a string iterates its bytes — the value var is an
        // int32 rune (v1 byte-wise), so the backing i8 byte is zero-extended
        // into it in the body below. Slices/arrays use their element type.
        int is_string_range = range_val->goo_type
                           && range_val->goo_type->kind == TYPE_STRING;
        int is_array_range = range_val->goo_type
                           && range_val->goo_type->kind == TYPE_ARRAY;
        Type* elem_type = NULL;
        LLVMTypeRef llvm_elem = NULL;
        LLVMValueRef data_ptr;
        LLVMValueRef len64;

        if (is_array_range) {
            // Arrays are a raw [N x T] aggregate — the iteration count is
            // the STATIC length (never read from the value), and the "data
            // pointer" is the array's own address, not an extracted field.
            Type* arr_type = range_val->goo_type;
            elem_type = arr_type->data.array.element_type;
            llvm_elem = elem_type ? codegen_type_to_llvm(codegen, elem_type) : NULL;
            len64 = LLVMConstInt(LLVMInt64TypeInContext(codegen->context),
                                 (unsigned long long)arr_type->data.array.length, 0);
            if (range_val->is_lvalue) {
                // Addressable (a variable): `raw` above skipped the load for
                // arrays, so it's still the pointer to the array.
                data_ptr = raw;
            } else {
                // Rvalue (e.g. an inline `[3]int{...}` composite literal):
                // no address to reuse, so spill the value into a temp
                // alloca and GEP off of that instead.
                LLVMTypeRef arr_llvm = codegen_type_to_llvm(codegen, arr_type);
                LLVMValueRef tmp_alloca = codegen_create_alloca(codegen, arr_llvm, "range_arr_tmp");
                LLVMBuildStore(codegen->builder, raw, tmp_alloca);
                data_ptr = tmp_alloca;
            }
        } else {
            // Extract data pointer (field 0) and length (field 1) — both
            // slices and strings share this layout.
            data_ptr = LLVMBuildExtractValue(codegen->builder, raw, 0, "range_data");
            len64 = LLVMBuildExtractValue(codegen->builder, raw, 1, "range_len");

            if (range_val->goo_type && range_val->goo_type->kind == TYPE_SLICE) {
                elem_type = range_val->goo_type->data.slice.element_type;
                llvm_elem = elem_type ? codegen_type_to_llvm(codegen, elem_type) : NULL;
            } else if (is_string_range) {
                elem_type = type_checker_get_builtin(checker, TYPE_INT32);
                llvm_elem = LLVMInt32TypeInContext(codegen->context);
            }
        }

        // Allocate index var; register it in scope under key_name.
        LLVMTypeRef i32 = LLVMInt32TypeInContext(codegen->context);
        LLVMValueRef idx_alloca = codegen_alloc_local(codegen, i32,
                                                     for_stmt->key_name ? for_stmt->key_name : "range_i");
        LLVMBuildStore(codegen->builder, LLVMConstInt(i32, 0, 0), idx_alloca);
        if (for_stmt->key_name) {
            ValueInfo* kv = value_info_new(for_stmt->key_name, idx_alloca,
                                          type_checker_get_builtin(checker, TYPE_INT32));
            kv->is_lvalue = 1;
            kv->is_initialized = 1;
            vscope_add(codegen, kv);
        }

        // Allocate value var (per-iteration). Mirrored to type-check
        // scope below.
        LLVMValueRef val_alloca = NULL;
        if (for_stmt->value_name && llvm_elem && elem_type) {
            val_alloca = codegen_alloc_local(codegen, llvm_elem, for_stmt->value_name);
            ValueInfo* vv = value_info_new(for_stmt->value_name, val_alloca, elem_type);
            vv->is_lvalue = 1;
            vv->is_initialized = 1;
            vscope_add(codegen, vv);
        }

        // Rune-aware string range (Go semantics): each iteration decodes the
        // UTF-8 rune at the current byte index and advances by its byte width,
        // NOT by 1. width_alloca carries the width from the body to the post
        // (increment) block; rune_slot receives the decoded rune. Both are
        // needed even when the value var is unused (`for i := range s`) because
        // the byte-index advance still depends on the rune width.
        LLVMValueRef width_alloca = NULL, rune_slot = NULL;
        if (is_string_range) {
            width_alloca = codegen_alloc_local(codegen, i32, "range_w");
            rune_slot = codegen_alloc_local(codegen, i32, "range_rune");
        }

        // Mirror loop vars to type-checker scope.
        scope_push(checker);
        if (for_stmt->key_name) {
            Variable* kv = variable_new(for_stmt->key_name,
                                       type_checker_get_builtin(checker, TYPE_INT32), stmt->pos);
            if (kv) { kv->is_initialized = 1; scope_add_variable(checker->current_scope, kv); }
        }
        if (for_stmt->value_name && elem_type) {
            Variable* vv = variable_new(for_stmt->value_name, elem_type, stmt->pos);
            if (vv) { vv->is_initialized = 1; scope_add_variable(checker->current_scope, vv); }
        }

        LLVMBasicBlockRef rcond = codegen_create_block(codegen, "range.cond");
        LLVMBasicBlockRef rbody = codegen_create_block(codegen, "range.body");
        // Dedicated increment block so `continue` re-runs the index bump before
        // re-testing the condition (branching straight to rcond would skip the
        // increment and loop forever).
        LLVMBasicBlockRef rpost = codegen_create_block(codegen, "range.post");
        LLVMBasicBlockRef rexit = codegen_create_block(codegen, "range.exit");

        LLVMBuildBr(codegen->builder, rcond);
        // cond: i < len  (compare in i64 to match len)
        codegen_set_insert_point(codegen, rcond);
        LLVMValueRef i_loaded = LLVMBuildLoad2(codegen->builder, i32, idx_alloca, "i");
        LLVMValueRef i64_widened = LLVMBuildSExt(codegen->builder, i_loaded,
                                                 LLVMInt64TypeInContext(codegen->context), "i64");
        LLVMValueRef cond_v = LLVMBuildICmp(codegen->builder, LLVMIntSLT, i64_widened, len64, "range_cond");
        LLVMBuildCondBr(codegen->builder, cond_v, rbody, rexit);

        // body: optionally load element, then run body
        codegen_set_insert_point(codegen, rbody);
        if (is_string_range) {
            // Rune-aware (Go): decode the UTF-8 rune at data_ptr[i]. Always run
            // (even with no value var) because the post block advances i by the
            // returned byte width. width -> width_alloca; rune -> value var.
            LLVMValueRef dec = LLVMGetNamedFunction(codegen->module, "goo_utf8_decode");
            if (!dec) { codegen_error(codegen, stmt->pos, "goo_utf8_decode missing"); scope_pop(checker); value_info_free(range_val); return 0; }
            LLVMValueRef args[4] = { data_ptr, len64, i64_widened, rune_slot };
            LLVMValueRef width = LLVMBuildCall2(codegen->builder,
                                                LLVMGlobalGetValueType(dec), dec, args, 4, "rune_w");
            LLVMBuildStore(codegen->builder, width, width_alloca);
            if (val_alloca) {
                LLVMValueRef r = LLVMBuildLoad2(codegen->builder, i32, rune_slot, "rune");
                LLVMBuildStore(codegen->builder, r, val_alloca);
            }
        } else if (val_alloca && llvm_elem) {
            // Slice/array: load the element directly.
            LLVMValueRef indices[] = { i_loaded };
            LLVMValueRef elem_ptr = LLVMBuildGEP2(codegen->builder, llvm_elem,
                                                  data_ptr, indices, 1, "elem_ptr");
            LLVMValueRef elem_val = LLVMBuildLoad2(codegen->builder, llvm_elem, elem_ptr, "elem");
            LLVMBuildStore(codegen->builder, elem_val, val_alloca);
        }
        // break exits to rexit; continue jumps to rpost (the increment block).
        if (!cfctx_push_loop(&codegen->cfctx, rexit, rpost)) {
            codegen_error(codegen, stmt->pos, "loop nesting too deep (max 32)");
            scope_pop(checker);
            value_info_free(range_val);
            return 0;
        }
        int body_ok = 1;
        if (for_stmt->body) {
            body_ok = codegen_generate_statement(codegen, checker, for_stmt->body);
        }
        cfctx_pop(&codegen->cfctx);
        // Only branch to the post block if the body didn't already terminate
        // (e.g. a bare `return` as the loop body's last statement) — otherwise
        // we'd emit a second terminator and produce invalid IR.
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)))
            LLVMBuildBr(codegen->builder, rpost);

        // post: advance the byte index. A string range advances by the decoded
        // rune's byte WIDTH (Go semantics); slices/arrays advance by 1.
        codegen_set_insert_point(codegen, rpost);
        LLVMValueRef i_now = LLVMBuildLoad2(codegen->builder, i32, idx_alloca, "i_inc");
        LLVMValueRef step = LLVMConstInt(i32, 1, 0);
        if (is_string_range) {
            step = LLVMBuildLoad2(codegen->builder, i32, width_alloca, "range_step");
        }
        LLVMValueRef i_next = LLVMBuildAdd(codegen->builder, i_now, step, "i_next");
        LLVMBuildStore(codegen->builder, i_next, idx_alloca);
        LLVMBuildBr(codegen->builder, rcond);

        codegen_set_insert_point(codegen, rexit);
        scope_pop(checker);
        value_info_free(range_val);
        return body_ok;
    }

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

        // Auto-load lvalue conditions (bare field selectors) — same as the if path.
        LLVMValueRef cond_val = condition->llvm_value;
        if (condition->is_lvalue && condition->goo_type) {
            LLVMTypeRef ct = codegen_type_to_llvm(codegen, condition->goo_type);
            if (ct) cond_val = LLVMBuildLoad2(codegen->builder, ct, cond_val, "cond_load");
        }

        LLVMBuildCondBr(codegen->builder, cond_val, body_block, exit_block);
        value_info_free(condition);
    } else {
        // Infinite loop
        LLVMBuildBr(codegen->builder, body_block);
    }
    
    // Generate body block. Push the loop context so break/continue inside the
    // body resolve to this loop's exit (break) and post/increment (continue)
    // blocks. continue targets post_block so the increment runs before the
    // condition is re-tested.
    codegen_set_insert_point(codegen, body_block);
    if (!cfctx_push_loop(&codegen->cfctx, exit_block, post_block)) {
        codegen_error(codegen, stmt->pos, "loop nesting too deep (max 32)");
        return 0;
    }
    if (for_stmt->body) {
        if (!codegen_generate_statement(codegen, checker, for_stmt->body)) {
            cfctx_pop(&codegen->cfctx);
            return 0;
        }
    }
    cfctx_pop(&codegen->cfctx);
    // Skip the post-block branch if the body already terminated (e.g. a bare
    // `return` last statement) to avoid a second terminator / invalid IR.
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)))
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
        // Multi-value return: `return a, b` parses as values=[a]->next=b.
        // Detect 2+ values and build an anonymous struct via
        // LLVMBuildInsertValue, then ret that struct. The function's
        // declared return type is already a TYPE_STRUCT (anonymous,
        // from the parser's multi_return_type_list rule).
        if (return_stmt->values->next) {
            Type* function_return_type =
                codegen->current_function_info ? codegen->current_function_info->goo_type : NULL;
            if (!function_return_type || function_return_type->kind != TYPE_STRUCT) {
                codegen_error(codegen, stmt->pos,
                              "Multi-value return but function return type is not a tuple");
                return 0;
            }
            LLVMTypeRef ret_llvm = codegen_type_to_llvm(codegen, function_return_type);
            LLVMValueRef agg = LLVMGetUndef(ret_llvm);
            size_t i = 0;
            for (ASTNode* v = return_stmt->values; v; v = v->next, i++) {
                // The tuple slot's declared type. A nullable slot (e.g. `error`
                // is ?*int8, so `(int, error)` has a nullable 2nd field) needs a
                // context-typed value: a bare `nil`/value lowered without it is
                // an i8* null / scalar that mismatches the {i1,T} slot and emits
                // malformed IR (crashes the LLVM backend). Mirrors the single-
                // return nil intercept and composite-literal nullable auto-wrap.
                Type* field_type = (function_return_type->kind == TYPE_STRUCT &&
                                    i < function_return_type->data.struct_type.field_count)
                                   ? function_return_type->data.struct_type.fields[i].type : NULL;

                if (field_type &&
                    (field_type->kind == TYPE_NULLABLE || type_is_nilable_ref_kind(field_type)) &&
                    v->type == AST_LITERAL &&
                    ((LiteralNode*)v)->literal_type == TOKEN_NIL) {
                    ValueInfo* nil_vi = codegen_generate_null_literal(codegen, checker, field_type);
                    if (!nil_vi) return 0;
                    agg = LLVMBuildInsertValue(codegen->builder, agg, nil_vi->llvm_value, (unsigned)i, "ret_field");
                    value_info_free(nil_vi);
                    continue;
                }

                ValueInfo* vv = codegen_generate_expression(codegen, checker, v);
                if (!vv) return 0;
                LLVMValueRef raw = vv->llvm_value;
                if (vv->is_lvalue && vv->goo_type) {
                    LLVMTypeRef lt = codegen_type_to_llvm(codegen, vv->goo_type);
                    if (lt) raw = LLVMBuildLoad2(codegen->builder, lt, raw, "ret_load");
                }
                // Auto-wrap a bare `T` into a `?T` slot (e.g. `return 1, x` where
                // the field is nullable and x is a bare value).
                if (field_type && field_type->kind == TYPE_NULLABLE &&
                    vv->goo_type && vv->goo_type->kind != TYPE_NULLABLE) {
                    LLVMTypeRef nty = codegen_type_to_llvm(codegen, field_type);
                    if (nty) raw = codegen_create_nullable_with_value(codegen, nty, raw, vv->goo_type);
                }
                // Box a concrete return value into an interface-typed return
                // field. Without this the raw concrete bits land in an
                // interface-shaped slot (empty/garbage output, or a verifier
                // failure for the variable form). Mirrors the nullable
                // auto-wrap above and the map/assignment interface-box arms.
                if (field_type && field_type->kind == TYPE_INTERFACE &&
                    vv->goo_type && vv->goo_type->kind != TYPE_INTERFACE) {
                    LLVMValueRef boxed = codegen_interface_box(codegen, checker,
                                                               field_type,
                                                               vv->goo_type, raw);
                    if (!boxed) {
                        codegen_error(codegen, v->pos,
                                      "failed to box concrete return value into interface");
                        value_info_free(vv);
                        return 0;
                    }
                    raw = boxed;
                }
                agg = LLVMBuildInsertValue(codegen->builder, agg, raw, (unsigned)i, "ret_field");
                value_info_free(vv);
            }
            codegen_emit_deferred_calls(codegen, checker);
            LLVMBuildRet(codegen->builder, agg);
            return 1;
        }

        // Get function return type early — needed for nullable nil-return
        // intercept below (before the expression is evaluated).
        Type* function_return_type = NULL;
        if (codegen->current_function_info && codegen->current_function_info->goo_type) {
            function_return_type = codegen->current_function_info->goo_type;
        }


        // Nil-return intercept: `return nil` inside a `?T` function, OR
        // (P2.2 option A) a function whose return type is a bare pointer/
        // slice/map/channel/function. Without this, codegen_generate_
        // null_literal produces a void* null pointer (no expected-type
        // context), which mismatches the `{i1, T}` nullable return type (or
        // a slice/func's aggregate return type) and fails module
        // verification. Intercept here, generate the correct
        // {is_null=1, zero_value} struct (or bare zero value), and return it
        // directly — same pattern as var_decl's `var b ?T = nil` fix.
#if LLVM_AVAILABLE
        if (function_return_type &&
            (function_return_type->kind == TYPE_NULLABLE ||
             type_is_nilable_ref_kind(function_return_type)) &&
            return_stmt->values->type == AST_LITERAL &&
            ((LiteralNode*)return_stmt->values)->literal_type == TOKEN_NIL) {
            ValueInfo* nil_vi = codegen_generate_null_literal(codegen, checker, function_return_type);
            if (!nil_vi) return 0;
            codegen_emit_deferred_calls(codegen, checker);
            LLVMBuildRet(codegen->builder, nil_vi->llvm_value);
            value_info_free(nil_vi);
            return 1;
        }
#endif

        // Single value return — original path.
        ValueInfo* return_value = codegen_generate_expression(codegen, checker, return_stmt->values);
        if (!return_value) {
            return 0;
        }

        // Auto-load an lvalue result (e.g. `return p.x`): a selector/index
        // returns the field address, which must be dereferenced to the scalar
        // value before being returned, or the function emits `ret ptr`.
#if LLVM_AVAILABLE
        if (return_value->is_lvalue && return_value->goo_type) {
            LLVMTypeRef rvt = codegen_type_to_llvm(codegen, return_value->goo_type);
            if (rvt) {
                return_value->llvm_value = LLVMBuildLoad2(codegen->builder, rvt, return_value->llvm_value, "retval");
                return_value->is_lvalue = 0;
            }
        }
#endif

        // Handle error union returns
#if LLVM_AVAILABLE
        LLVMValueRef final_return_value = return_value->llvm_value;
        if (function_return_type) {
            final_return_value = codegen_generate_error_return(codegen, return_value->llvm_value,
                                                             return_value->goo_type, function_return_type);
        }

        // Nullable auto-wrap: `return Point{...}` inside a `?Point` function.
        // When the function's declared return type is ?T (TYPE_NULLABLE) and the
        // actual return value has the inner type T (not already nullable), wrap it
        // in an {is_null=0, value} aggregate — same InsertValue pattern used in
        // var_decl for `var hit ?int = 42`. This is the core ABI fix for M4 Task 3:
        // the {i1, BigStruct} wrapper struct is what LLVM's backend sees as the
        // "large" return type (>16 bytes on x86-64), so fixing the wrapper here
        // makes the by-pointer return convention apply to the correct aggregate.
        if (function_return_type && function_return_type->kind == TYPE_NULLABLE &&
            return_value->goo_type && return_value->goo_type->kind != TYPE_NULLABLE) {
            LLVMTypeRef nullable_llvm = codegen_type_to_llvm(codegen, function_return_type);
            if (nullable_llvm) {
                // Delegate to the shared nullable-wrap helper (the same one
                // the multi-value return, call-arg, and struct/array-literal
                // wrap sites use) instead of reimplementing the InsertValue
                // pair inline. The helper's coercion covers both narrow->wide
                // int widening (what the old inline SExt guard here handled)
                // and cross-kind int->float coercion (which the old guard
                // did not) — fixes `return n` (typed int) into a ?float64
                // return slot, which crashed the LLVM verifier the same way
                // the var-decl path did before it was routed through this
                // helper. A `return` always executes inside a function body,
                // so the builder is guaranteed positioned here.
                final_return_value = codegen_create_nullable_with_value(
                    codegen, nullable_llvm, final_return_value, return_value->goo_type);
            }
        }

        // Interface return-boxing (P4-5): `return Sq{...}` from a function whose
        // declared return type is an interface — box the concrete value into the
        // {vtable, data} interface value before returning it.
        if (function_return_type && function_return_type->kind == TYPE_INTERFACE &&
            return_value->goo_type && return_value->goo_type->kind != TYPE_INTERFACE) {
            LLVMValueRef boxed = codegen_interface_box(codegen, checker, function_return_type,
                                                       return_value->goo_type, final_return_value);
            if (!boxed) { value_info_free(return_value); return 0; }
            final_return_value = boxed;
        }

        // Integer width coercion: match the return value to the function's
        // declared LLVM return type. Widening (`return 0` as i32 from an i64
        // function) always applies. Narrowing (`return 1` from an int8
        // function) is only reachable for an untyped integer CONSTANT that
        // the checker has already confirmed is representable at the target
        // width (Go representability rule) — a non-constant narrowing return,
        // or a constant that doesn't fit, is rejected before codegen (see the
        // int_const_coerce gate and its int_const_fits_expected range check in
        // type_check_return_stmt) — so here we rebuild the constant at the
        // target width rather than emit a Trunc on a runtime value. Mirrors
        // var_decl's narrowing/widening const path.
        {
            LLVMValueRef cur_fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(codegen->builder));
            LLVMTypeRef fn_ret = LLVMGetReturnType(LLVMGlobalGetValueType(cur_fn));
            LLVMTypeRef val_ty = LLVMTypeOf(final_return_value);
            if (LLVMGetTypeKind(val_ty) == LLVMIntegerTypeKind &&
                LLVMGetTypeKind(fn_ret) == LLVMIntegerTypeKind) {
                unsigned from_bits = LLVMGetIntTypeWidth(val_ty);
                unsigned to_bits   = LLVMGetIntTypeWidth(fn_ret);
                if (from_bits < to_bits) {
                    final_return_value = LLVMBuildSExt(codegen->builder, final_return_value,
                                                       fn_ret, "ret_sext");
                } else if (from_bits > to_bits && LLVMIsConstant(final_return_value)) {
                    int use_sext = return_value->goo_type
                                 ? type_is_signed(return_value->goo_type) : 1;
                    unsigned long long raw = use_sext
                        ? (unsigned long long)LLVMConstIntGetSExtValue(final_return_value)
                        : LLVMConstIntGetZExtValue(final_return_value);
                    final_return_value = LLVMConstInt(fn_ret, raw, use_sext);
                }
            }
        }

        codegen_emit_deferred_calls(codegen, checker);
        LLVMBuildRet(codegen->builder, final_return_value);
#else
        codegen_generate_error_return(codegen, return_value->llvm_value,
                                    return_value->goo_type, function_return_type);
#endif
        value_info_free(return_value);
    } else {
        // Bare return.
        LLVMValueRef cur_fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(codegen->builder));
        LLVMTypeRef fn_ret = LLVMGetReturnType(LLVMGlobalGetValueType(cur_fn));

        // Named return parameters (P3-5): `func f() (x int, y int) { ...; return }`
        // yields the current values of the named-result locals. Load each in
        // field order and rebuild the aggregate the function's LLVM signature
        // expects (the struct return type built by the parser/type checker).
        FunctionInfo* fi = codegen->current_function_info;
        if (fi && fi->named_result_count > 0) {
            // A SINGLE named result `func f() (r T)` returns T DIRECTLY: the
            // parser's 1-field result tuple was collapsed to its field type in
            // type_from_ast, so the function's return ABI *is* T — whether T is
            // a scalar (i32) or an LLVM aggregate (string `{ptr,i64}`, slice
            // `{ptr,i64,i64}`, struct). Load the one named local and return it
            // as-is. The discriminator MUST be arity (count == 1), NOT the LLVM
            // type kind: an aggregate-typed single result has a StructTypeKind
            // return type, but it is still a one-value return, so the multi-
            // result InsertValue path below would emit invalid IR for it
            // (`insertvalue {ptr,i64} undef, {ptr,i64} %r, 0`). The >=2 named-
            // result case keeps the struct-aggregate path.
            if (fi->named_result_count == 1) {
                ValueInfo* rv = codegen_lookup_value(codegen, fi->named_result_names[0]);
                if (!rv) {
                    codegen_error(codegen, stmt->pos,
                                  "named result '%s' not in scope for bare return",
                                  fi->named_result_names[0]);
                    return 0;
                }
                LLVMTypeRef vt = codegen_type_to_llvm(codegen, rv->goo_type);
                LLVMValueRef loaded = LLVMBuildLoad2(codegen->builder, vt, rv->llvm_value, "named_ret");
                codegen_emit_deferred_calls(codegen, checker);
                LLVMBuildRet(codegen->builder, loaded);
                return 1;
            }
            LLVMValueRef agg = LLVMGetUndef(fn_ret);
            for (size_t i = 0; i < fi->named_result_count; i++) {
                ValueInfo* rv = codegen_lookup_value(codegen, fi->named_result_names[i]);
                if (!rv) {
                    codegen_error(codegen, stmt->pos,
                                  "named result '%s' not in scope for bare return",
                                  fi->named_result_names[i]);
                    return 0;
                }
                LLVMTypeRef vt = codegen_type_to_llvm(codegen, rv->goo_type);
                LLVMValueRef loaded = LLVMBuildLoad2(codegen->builder, vt, rv->llvm_value, "named_ret");
                agg = LLVMBuildInsertValue(codegen->builder, agg, loaded, (unsigned)i, "named_ret_agg");
            }
            codegen_emit_deferred_calls(codegen, checker);
            LLVMBuildRet(codegen->builder, agg);
            return 1;
        }

        // Otherwise: if the enclosing function has a non-void LLVM signature
        // (e.g. the entry-point main, lowered to `i32 @main`), return a zero of
        // that type so the IR stays well-typed; otherwise a plain void return.
        codegen_emit_deferred_calls(codegen, checker);
        if (LLVMGetTypeKind(fn_ret) != LLVMVoidTypeKind) {
            LLVMBuildRet(codegen->builder, LLVMConstNull(fn_ret));
        } else {
            LLVMBuildRetVoid(codegen->builder);
        }
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
    
    // Standard goroutine implementation for native targets.
    //
    // `go f(a, b)` lowers to: heap-box the evaluated arguments into an anonymous
    // struct, generate a per-call-site thunk `void __goo_thunk_N(i8*)` that
    // unboxes the args, calls the real function, frees the box, and returns; then
    // spawn the goroutine with goo_go(thunk, box). The runtime goo_go demands a
    // uniform void(*)(void*) entry, so the thunk adapts the user function's
    // arbitrary signature. All LLVM types are built in codegen->context (the
    // same context fix M7 applied to channels) so the module verifies.
    LLVMContextRef ctx = codegen->context;

    if (go_stmt->call->type != AST_CALL_EXPR) {
        codegen_error(codegen, stmt->pos, "Go statement must contain a function call");
        return 0;
    }

    CallExprNode* call = (CallExprNode*)go_stmt->call;

    // Comptime+generic composition (sub-project 2, Task 3): `go` targets a
    // monomorphized instance's mangled symbol exactly like an ordinary call
    // site (codegen_generate_call_expr, call_codegen.c) — a template is
    // never emitted under its bare name for ANY axis that specializes it, so
    // codegen_resolve_callee below would misreport "Undefined identifier".
    // This mirrors call_codegen's three-way (combined / generic-only /
    // comptime-only) byte-for-byte, one call-site type over: the generic-only
    // case is NEW here (pre-existing gap — `go GenericFn(...)` previously had
    // no rewiring at all, only the comptime-only case below existed), added
    // as required substrate for the combined case. `call->function` is
    // guaranteed non-NULL by the AST_CALL_EXPR check above, but each branch
    // still guards it defensively like the pre-existing comptime-only arm did.
    ValueInfo* func_val = NULL;
    if (call->type_arg_count > 0 && call->comptime_value_arg_count > 0 &&
        call->function && call->function->type == AST_IDENTIFIER) {
        IdentifierNode* gid = (IdentifierNode*)call->function;
        Type** concrete_args = malloc(sizeof(Type*) * call->type_arg_count);
        if (!concrete_args) return 0;
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
            Variable* gvar = type_checker_lookup_variable(checker, gid->name);
            Type* concrete_sig = gvar
                ? type_substitute(gvar->type, concrete_args, call->type_arg_count)
                : NULL;
            func_val = value_info_new(gid->name, inst, concrete_sig);
        }
        free(concrete_args);
    } else if (call->type_arg_count > 0 && call->function &&
               call->function->type == AST_IDENTIFIER) {
        IdentifierNode* gid = (IdentifierNode*)call->function;
        Type** concrete_args = malloc(sizeof(Type*) * call->type_arg_count);
        if (!concrete_args) return 0;
        for (size_t i = 0; i < call->type_arg_count; i++) {
            concrete_args[i] = type_substitute(call->type_args[i],
                codegen->active_subst, codegen->active_subst_n);
        }
        char* sym = codegen_mangle_instance(gid->name, concrete_args, call->type_arg_count);
        LLVMValueRef inst = LLVMGetNamedFunction(codegen->module, sym);
        free(sym);
        if (inst) {
            Variable* gvar = type_checker_lookup_variable(checker, gid->name);
            Type* concrete_sig = gvar
                ? type_substitute(gvar->type, concrete_args, call->type_arg_count)
                : NULL;
            func_val = value_info_new(gid->name, inst, concrete_sig);
        }
        free(concrete_args);
    } else if (call->comptime_value_arg_count > 0 && call->function &&
               call->function->type == AST_IDENTIFIER) {
        // Comptime value params Task 3 (fix round 2): `go fill(4, 10)` must
        // dispatch to the monomorphized instance symbol (`fill__n4`), never the
        // bare name — a plain comptime-param function's template is never
        // emitted under its bare name, so codegen_resolve_callee below would
        // fail with a misleading "Undefined identifier 'fill'". The go
        // statement's call was type-checked (type_check_go_stmt), so
        // comptime_value_args is established and captured per ast.h's
        // invariant. The instance value is a real LLVM function
        // (LLVMIsAFunction holds), so the arg-boxing/thunk path below handles
        // it like any other direct top-level callee — the comptime argument
        // stays an ordinary boxed parameter at the call boundary (it is a
        // constant only INSIDE the specialized body). Reached only when the
        // combined branch above did NOT fire (call->type_arg_count == 0
        // here), by construction of this if/else-if/else-if chain.
        IdentifierNode* cid = (IdentifierNode*)call->function;
        char* csym = codegen_mangle_comptime_instance(cid->name,
            call->comptime_value_args, call->comptime_value_arg_count);
        LLVMValueRef inst = csym ? LLVMGetNamedFunction(codegen->module, csym) : NULL;
        free(csym);
        if (inst) {
            Variable* cvar = type_checker_lookup_variable(checker, cid->name);
            func_val = value_info_new(cid->name, inst, cvar ? cvar->type : NULL);
        }
    }

    // Resolve the target function value and its type. Task 2 (universal
    // fat-pointer function values): MUST go through codegen_resolve_callee
    // (call_codegen.c), NOT codegen_generate_expression directly. A bare
    // identifier naming an unshadowed top-level function must resolve to
    // the BARE LLVM global here — the M8 LLVMIsAFunction check right below
    // requires it. Calling codegen_generate_expression directly would hit
    // codegen_generate_identifier's fat-pointer VALUE-wrapping fallback for
    // ANY bare function name, breaking every `go namedFunc(...)` golden.
    // This keeps the arg-boxing path below byte-for-byte unchanged for the
    // one shape it supports; anything else still yields a function VALUE
    // and is still cleanly rejected by the LLVMIsAFunction check (pre-Task-2
    // this could crash instead — see the Task 2 report's crash-site #1).
    if (!func_val) {
        func_val = codegen_resolve_callee(codegen, checker, call->function);
    }
    if (!func_val) return 0;
    LLVMValueRef callee = func_val->llvm_value;

    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);

    // M8 restriction (relaxed by Task 3 below): a direct top-level function
    // target calls unconditionally unchanged via the arg-boxing/thunk path
    // that follows this if/else. Anything else — a func literal or a local
    // variable/param/selector/index holding a function VALUE — is handled
    // by the shape-equivalence spawn below instead of being rejected
    // outright (bound methods still fall through to codegen_resolve_
    // callee's own "not found" error before ever reaching here — see the
    // task report).
    if (!callee || !LLVMIsAFunction(callee)) {
        // Task 3: `go <zero-arg VOID function value>()`. The universal
        // fat-pointer pair `{fn_ptr, env_ptr}` (call_codegen.c's
        // representation for every non-direct function value, including a
        // func literal's captured closure) is EXACTLY the runtime's
        // required `void(*)(void*)` goroutine-thunk shape whenever the
        // callee takes zero arguments and returns nothing: extract fn_ptr/
        // env_ptr and spawn `goo_go(fn_ptr, env_ptr)` directly — no boxing,
        // no per-call-site thunk synthesis needed (unlike the named-
        // function path below, which exists to ADAPT an arbitrary
        // signature to that same shape). A captured variable's slot was
        // already heap-promoted at its declaration site (Task 2), so it
        // safely outlives this frame across the goroutine boundary.
        //
        // Go itself discards a go-statement's call result, so a non-void
        // callee is rejected rather than silently dropping the value (see
        // also `go vet`'s unused-result-style diagnostics for discarded
        // calls). An argument-carrying value/literal target is a distinct,
        // separately out-of-scope case (needs either boxing this
        // representation's env alongside the arg box, or wrapping in an
        // adapter thunk) — rejected with its own message; capturing the
        // values into the closure instead is the workaround today.
        if (call->args) {
            codegen_error(codegen, stmt->pos,
                          "go with argument-carrying function values is not "
                          "yet supported; capture the values instead");
            value_info_free(func_val);
            return 0;
        }

        Type* callee_type = func_val->goo_type;
        if (!callee_type || callee_type->kind != TYPE_FUNCTION ||
            !callee_type->data.function.return_type ||
            callee_type->data.function.return_type->kind != TYPE_VOID) {
            codegen_error(codegen, stmt->pos,
                          "go: only void-returning function values are "
                          "supported (the result would be discarded)");
            value_info_free(func_val);
            return 0;
        }

        // codegen_generate_selector_expr / codegen_generate_index_expr (and
        // an identifier naming a local var/param) return a func-typed
        // field/element/slot as its ADDRESS (is_lvalue=1) — load the pair
        // through the goo type before extracting, mirroring the general
        // indirect-call site (call_codegen.c). A func literal's value
        // (is_lvalue=0, the common `go func(){...}()` shape) is already the
        // pair itself and needs no load.
        LLVMValueRef pair_val = callee;
        if (func_val->is_lvalue) {
            LLVMTypeRef pair_ty = codegen_type_to_llvm(codegen, callee_type);
            if (!pair_ty) {
                codegen_error(codegen, stmt->pos,
                              "internal: cannot lower go target's function-value type");
                value_info_free(func_val);
                return 0;
            }
            pair_val = LLVMBuildLoad2(codegen->builder, pair_ty, pair_val, "go_funcval_load");
        }

        LLVMValueRef fn_ptr  = LLVMBuildExtractValue(codegen->builder, pair_val, 0, "go_funcval_fn");
        LLVMValueRef env_ptr = LLVMBuildExtractValue(codegen->builder, pair_val, 1, "go_funcval_env");

        // Same `void(*)(void*)` thunk type the named-function path below
        // builds for goo_go's first parameter — built independently here
        // (LLVM literal function types are uniqued per-context, so this is
        // the identical LLVMTypeRef either way) since this branch returns
        // before reaching that shared declaration.
        LLVMTypeRef thunk_ty = LLVMFunctionType(LLVMVoidTypeInContext(ctx), &void_ptr_type, 1, 0);
        LLVMTypeRef goo_go_params[] = { LLVMPointerType(thunk_ty, 0), void_ptr_type };
        LLVMTypeRef goo_go_type = LLVMFunctionType(void_ptr_type, goo_go_params, 2, 0);
        LLVMValueRef goo_go_func = LLVMGetNamedFunction(codegen->module, "goo_go");
        if (!goo_go_func) goo_go_func = LLVMAddFunction(codegen->module, "goo_go", goo_go_type);

        LLVMValueRef spawn_args[] = { fn_ptr, env_ptr };
        LLVMBuildCall2(codegen->builder, goo_go_type, goo_go_func, spawn_args, 2, "");

        value_info_free(func_val);
        return 1;
    }
    LLVMTypeRef callee_ty = LLVMGlobalGetValueType(callee);

    // Count + evaluate the call arguments (in the caller's block).
    size_t arg_count = 0;
    for (ASTNode* a = call->args; a; a = a->next) arg_count++;

    LLVMValueRef* arg_vals = NULL;
    LLVMTypeRef* arg_types = NULL;
    if (arg_count > 0) {
        arg_vals = malloc(sizeof(LLVMValueRef) * arg_count);
        arg_types = malloc(sizeof(LLVMTypeRef) * arg_count);
        ASTNode* a = call->args;
        for (size_t i = 0; i < arg_count; i++, a = a->next) {
            ValueInfo* av = codegen_generate_expression(codegen, checker, a);
            if (!av) {
                free(arg_vals);
                free(arg_types);
                value_info_free(func_val);
                return 0;
            }
            // Auto-load lvalue args (bare field selectors return the field's
            // ADDRESS) — box the VALUE, snapshotted at the go statement (Go
            // semantics), not a pointer into the enclosing frame. Same idiom
            // as the if/for/unary/receiver load sites.
            if (av->is_lvalue && av->goo_type) {
                LLVMTypeRef at = codegen_type_to_llvm(codegen, av->goo_type);
                if (at) {
                    av->llvm_value = LLVMBuildLoad2(codegen->builder, at,
                                                    av->llvm_value, "go_arg_load");
                    av->is_lvalue = 0;
                }
            }
            arg_vals[i] = av->llvm_value;
            arg_types[i] = LLVMTypeOf(av->llvm_value);
            value_info_free(av);
        }
    }

    // Heap-box the arguments. boxed stays null for zero-arg calls.
    LLVMValueRef boxed = LLVMConstNull(void_ptr_type);
    LLVMTypeRef box_struct = NULL;
    if (arg_count > 0) {
        box_struct = LLVMStructTypeInContext(ctx, arg_types, (unsigned)arg_count, 0);

        LLVMValueRef box_size = LLVMSizeOf(box_struct);  // i64 target-size constant
        boxed = codegen_emit_alloc(codegen, box_size, ALLOC_KIND_DEFAULT, NULL);

        for (size_t i = 0; i < arg_count; i++) {
            LLVMValueRef field = LLVMBuildStructGEP2(codegen->builder, box_struct, boxed,
                                                     (unsigned)i, "go_arg_ptr");
            LLVMBuildStore(codegen->builder, arg_vals[i], field);
        }
    }
    free(arg_vals);
    free(arg_types);

    // Emit the per-call-site thunk in its own function, saving/restoring the
    // shared builder's insert position so the caller's IR stream is untouched.
    static unsigned thunk_counter = 0;
    char thunk_name[32];
    snprintf(thunk_name, sizeof(thunk_name), "__goo_thunk_%u", thunk_counter++);

    LLVMTypeRef thunk_ty = LLVMFunctionType(LLVMVoidTypeInContext(ctx), &void_ptr_type, 1, 0);
    LLVMValueRef thunk_fn = LLVMAddFunction(codegen->module, thunk_name, thunk_ty);
    LLVMSetLinkage(thunk_fn, LLVMInternalLinkage);

    LLVMBasicBlockRef saved_block = LLVMGetInsertBlock(codegen->builder);
    LLVMBasicBlockRef thunk_entry = LLVMAppendBasicBlockInContext(ctx, thunk_fn, "entry");
    LLVMPositionBuilderAtEnd(codegen->builder, thunk_entry);

    LLVMValueRef box_param = LLVMGetParam(thunk_fn, 0);  // i8*
    LLVMValueRef* call_args = NULL;
    if (arg_count > 0) {
        call_args = malloc(sizeof(LLVMValueRef) * arg_count);
        for (size_t i = 0; i < arg_count; i++) {
            LLVMValueRef field = LLVMBuildStructGEP2(codegen->builder, box_struct, box_param,
                                                     (unsigned)i, "ld_arg_ptr");
            LLVMTypeRef fty = LLVMStructGetTypeAtIndex(box_struct, (unsigned)i);
            call_args[i] = LLVMBuildLoad2(codegen->builder, fty, field, "ld_arg");
        }
    }
    // Call the real function; any return value is discarded (Go semantics).
    LLVMBuildCall2(codegen->builder, callee_ty, callee, call_args, (unsigned)arg_count, "");
    free(call_args);

    if (arg_count > 0) {
        LLVMTypeRef free_ty = LLVMFunctionType(LLVMVoidTypeInContext(ctx), &void_ptr_type, 1, 0);
        LLVMValueRef free_fn = LLVMGetNamedFunction(codegen->module, "goo_free");
        if (!free_fn) free_fn = LLVMAddFunction(codegen->module, "goo_free", free_ty);
        LLVMBuildCall2(codegen->builder, free_ty, free_fn, &box_param, 1, "");
    }
    LLVMBuildRetVoid(codegen->builder);

    // Back to the caller's block to emit the spawn.
    LLVMPositionBuilderAtEnd(codegen->builder, saved_block);

    LLVMTypeRef goo_go_params[] = { LLVMPointerType(thunk_ty, 0), void_ptr_type };
    LLVMTypeRef goo_go_type = LLVMFunctionType(void_ptr_type, goo_go_params, 2, 0);
    LLVMValueRef goo_go_func = LLVMGetNamedFunction(codegen->module, "goo_go");
    if (!goo_go_func) goo_go_func = LLVMAddFunction(codegen->module, "goo_go", goo_go_type);

    LLVMValueRef spawn_args[] = { thunk_fn, boxed };
    LLVMBuildCall2(codegen->builder, goo_go_type, goo_go_func, spawn_args, 2, "");

    value_info_free(func_val);
    return 1;
#endif
}

// Arena-regions early-exit free (Task 6 follow-up): emit goo_arena_free for
// every arena currently on codegen->arena_stack, innermost first. Called on
// every function-exit path (from codegen_emit_deferred_calls) so a `return`
// OUT of one or more arena blocks reclaims their arenas instead of leaking
// them — the fall-through free in codegen_generate_arena_stmt only fires when
// control reaches the physical end of the block, which a `return` jumps past.
//
// Soundness: this does NOT modify arena_depth (the arena_stmt that pushed each
// arena still owns its pop). Multiple return sites each emit their own free of
// the same arena SSA, but only one return executes per run, and the
// terminated-block guard in codegen_generate_arena_stmt skips the fall-through
// free once a return has terminated the block — so no path frees an arena
// twice. `break`/`continue` do NOT reach here (they are loop exits, not
// function exits) and still leak their arenas — safe, a documented follow-up.
// Emit goo_arena_free for every active arena whose push-time loop_depth is
// >= min_loop_depth, innermost first. `return` passes min_loop_depth 0 to free
// ALL active arenas (it exits the whole function); `break`/`continue` pass the
// current codegen->cfctx.loop_depth to free only the arenas pushed INSIDE the loop
// they exit, never an enclosing-loop arena the loop keeps using. Does NOT
// modify arena_depth/arena_loop_depth — the arena_stmt still owns each pop, and
// the terminated-block guard in codegen_generate_arena_stmt keeps any exit path
// from also taking the fall-through free (so no arena is freed twice).
static void codegen_emit_arena_frees(CodeGenerator* codegen, int min_loop_depth) {
#if LLVM_AVAILABLE
    if (!codegen || codegen->arena_depth <= 0) return;
    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMTypeRef free_fn_ty = LLVMFunctionType(LLVMVoidTypeInContext(ctx), &ptr_type, 1, 0);
    LLVMValueRef free_fn = LLVMGetNamedFunction(codegen->module, "goo_arena_free");
    if (!free_fn) free_fn = LLVMAddFunction(codegen->module, "goo_arena_free", free_fn_ty);
    for (int i = codegen->arena_depth - 1; i >= 0; i--) {
        if (codegen->arena_loop_depth[i] < min_loop_depth) continue;
        LLVMValueRef arena = codegen->arena_stack[i];
        if (arena) LLVMBuildCall2(codegen->builder, free_fn_ty, free_fn, &arena, 1, "");
    }
#else
    (void)codegen; (void)min_loop_depth;
#endif
}

// arena-goto fix ("goto out of an arena{} block skips goo_arena_free"):
// emit goo_arena_free for every arena at codegen->arena_stack index
// target_arena_depth..arena_depth-1 (innermost first) — i.e. every arena
// pushed AFTER the goto's target label's own arena-nesting depth. Unlike
// codegen_emit_arena_frees above (loop-depth-keyed, for break/continue's
// "which loop am I exiting" question), a `goto` has no loop relationship
// to its target — only arena LEXICAL nesting (codegen->arena_stack's
// actual push order, which for this single-pass recursive-descent codegen
// exactly mirrors AST containment) matters, so this compares directly
// against arena_stack position. Callers must pass a target_arena_depth
// the type checker has already proven is <= arena_depth and whose arena
// chain is a genuine prefix of the current one (AST_GOTO_STMT's
// type-check case, type_checker.c's "goto into arena block is not
// supported" diagnostic) — this function trusts that and does not
// re-validate arena identity, only the depth bound.
static void codegen_emit_arena_frees_to_depth(CodeGenerator* codegen, int target_arena_depth) {
#if LLVM_AVAILABLE
    if (!codegen || target_arena_depth < 0 || codegen->arena_depth <= target_arena_depth) return;
    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMTypeRef free_fn_ty = LLVMFunctionType(LLVMVoidTypeInContext(ctx), &ptr_type, 1, 0);
    LLVMValueRef free_fn = LLVMGetNamedFunction(codegen->module, "goo_arena_free");
    if (!free_fn) free_fn = LLVMAddFunction(codegen->module, "goo_arena_free", free_fn_ty);
    for (int i = codegen->arena_depth - 1; i >= target_arena_depth; i--) {
        LLVMValueRef arena = codegen->arena_stack[i];
        if (arena) LLVMBuildCall2(codegen->builder, free_fn_ty, free_fn, &arena, 1, "");
    }
#else
    (void)codegen; (void)target_arena_depth;
#endif
}

// Emit the current function's deferred calls in LIFO (last-registered-first)
// order. Called at every function-exit path immediately before the `ret`.
// No-op when the function registered no defers, so existing functions are
// unaffected.
//
// Each deferred call's arguments were snapshotted into entry-block allocas at
// the defer site (Go's defer-time arg evaluation), and synthetic identifier
// nodes referencing those snapshots were built there too — but NOT linked
// into the call's argument list at that point (`call` is a node in the
// shared template AST; a permanent rewrite would corrupt it for the next
// instantiation). Here we (1) re-bind those synthetic names to their
// snapshot slots in the value table (the originating block's locals are long
// gone), then (2) emit each call guarded by its runtime "active" flag (so a
// defer that was never reached at runtime, e.g. inside a not-taken branch,
// does not run), splicing the synthetic nodes into the call transactionally
// for the duration of that one emission and restoring the originals
// immediately after (see the splice/restore below).
void codegen_emit_deferred_calls(CodeGenerator* codegen, TypeChecker* checker) {
#if LLVM_AVAILABLE
    if (!codegen || !checker) return;
    // Free the arenas this return is leaving BEFORE running defers. Defers'
    // arguments escaped to the heap (a defer inside an arena block marks its
    // args escaping — see block_escape.c), so they never point into these
    // arenas and the ordering is immaterial for correctness. Runs on every
    // exit path; a no-op when no arena is active (arena_depth == 0). A return
    // exits the whole function, so it frees ALL active arenas (min_loop_depth 0).
    codegen_emit_arena_frees(codegen, 0);
    FunctionInfo* fi = codegen->current_function_info;
    if (!fi) return;

    // P3.4 stack mode: every exit path emits the SAME single call,
    // regardless of how many (if any) defers THIS dynamic execution
    // actually pushed — goo_defer_run is a no-op on a never-pushed
    // (zeroed) frame (see its doc comment, include/runtime.h). This
    // entirely replaces the static active-flag LIFO walk below for a
    // stack-mode function; the two mechanisms are never combined in one
    // function (FunctionInfo.defer_stack_mode's doc comment).
    if (fi->defer_stack_mode) {
        if (!fi->defer_frame) return;  // defensive: mode set but frame never allocated
        LLVMContextRef ctx = codegen->context;
        LLVMTypeRef ptr_ty = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
        LLVMTypeRef run_ty = LLVMFunctionType(LLVMVoidTypeInContext(ctx), &ptr_ty, 1, 0);
        LLVMValueRef run_fn = LLVMGetNamedFunction(codegen->module, "goo_defer_run");
        if (!run_fn) run_fn = LLVMAddFunction(codegen->module, "goo_defer_run", run_ty);
        LLVMBuildCall2(codegen->builder, run_ty, run_fn, &fi->defer_frame, 1, "");
        return;
    }

    if (fi->deferred_count == 0) return;
    if (g_defer_info_owner != fi || g_defer_info_count < fi->deferred_count) return;

    LLVMTypeRef i1 = LLVMInt1TypeInContext(codegen->context);

    for (size_t i = fi->deferred_count; i > 0; i--) {
        DeferCodegenInfo* info = &g_defer_info[i - 1];
        ASTNode* call = fi->deferred_calls[i - 1];

        // Re-bind the defer-time argument snapshots so the rewritten call's
        // synthetic identifiers resolve to them (LIFO lookup => latest wins,
        // so re-binding on every exit path is harmless).
        for (size_t j = 0; j < info->arg_count; j++) {
            if (!info->arg_slots[j]) continue;
            ValueInfo* vi = value_info_new(info->arg_names[j], info->arg_slots[j],
                                           info->arg_types[j]);
            if (!vi) continue;
            vi->is_lvalue = 1;
            vi->is_initialized = 1;
            vscope_add(codegen, vi);
        }

        // Guard the call with the runtime active flag.
        LLVMValueRef live = LLVMBuildLoad2(codegen->builder, i1, info->active_flag,
                                           "defer_live");
        LLVMBasicBlockRef run_bb = codegen_create_block(codegen, "defer_run");
        LLVMBasicBlockRef cont_bb = codegen_create_block(codegen, "defer_cont");
        LLVMBuildCondBr(codegen->builder, live, run_bb, cont_bb);

        LLVMPositionBuilderAtEnd(codegen->builder, run_bb);

        // Destructive-splice fix: `call` (and its selector base, for a
        // method-call defer) is a node in the SHARED template AST — this
        // DeferStmtNode is revisited once per instantiation (comptime,
        // generic, or composed), so leaving a mutation on it after this
        // emission would corrupt the next instance's view of the same
        // statement (the pre-fix bug: instance 2 found instance 1's synthetic
        // `__goo_defer0_argN` identifiers where its own original argument
        // expressions used to be). Splice the synthetic identifiers in only
        // for this one call, and restore the saved originals immediately
        // after — a transaction fully contained within this loop iteration,
        // so no code outside it (including this same function's OWN next
        // iteration, or the next instance's compilation) ever observes the
        // mutated state. saved_args/saved_recv are borrowed pointers into the
        // template AST — not owned here, not freed here, just parked for the
        // duration of the transaction.
        CallExprNode* call_expr = (CallExprNode*)call;
        ASTNode* saved_args = call_expr->args;
        SelectorExprNode* sel = NULL;
        ASTNode* saved_recv = NULL;
        if (info->recv_synth && call_expr->function &&
            call_expr->function->type == AST_SELECTOR_EXPR) {
            sel = (SelectorExprNode*)call_expr->function;
            saved_recv = sel->expr;
            sel->expr = info->recv_synth;
        }
        if (info->args_synth_head) call_expr->args = info->args_synth_head;

        ValueInfo* result = codegen_generate_expression(codegen, checker, call);
        if (result) value_info_free(result);

        call_expr->args = saved_args;
        if (sel) sel->expr = saved_recv;

        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)))
            LLVMBuildBr(codegen->builder, cont_bb);

        LLVMPositionBuilderAtEnd(codegen->builder, cont_bb);
    }
#else
    (void)codegen;
    (void)checker;
#endif
}

#if LLVM_AVAILABLE
// P3.4 stack-mode defer: called once per LEXICAL defer statement (per
// codegen pass) in a stack-mode function. Snapshots the receiver/args at
// the CURRENT insert point (defer-TIME evaluation, Go semantics — NOT
// entry-hoisted, so a loop body re-runs this on every iteration and each
// dynamic execution captures its own values), heap-allocates an env cell
// holding them, synthesizes a private per-statement thunk that unpacks the
// env and re-emits the original call, and pushes {thunk, env} onto this
// function's runtime defer stack. Executing this statement at runtime IS
// the registration, so LIFO + per-iteration snapshots fall out for free —
// no active-flag needed the way the static path needs one.
//
// The thunk body reuses codegen_generate_expression on the ORIGINAL call
// AST node (transient-splice-and-restore, exactly like
// codegen_emit_deferred_calls does for the static path — see that
// function's doc comment for why the splice must be transactional) rather
// than hand-rolling a raw LLVMBuildCall2: that is what makes this path
// correct for every call shape the static path already supports — builtins
// like fmt.Println, user functions, methods — for free, instead of
// reimplementing call_codegen.c's builtin/method/vararg resolution here.
//
// CALLEE SNAPSHOT (review 2026-07-10): a callee that is itself a VALUE — a
// func literal (capturing or not), a local variable holding a func value, a
// method value, a call returning a func — cannot be re-emitted inside the
// thunk: re-emission would resolve it against the OUTER function's
// allocas/instructions (codegen's value table is flat), producing LLVM's
// "Referring to an instruction in another function" verifier ICE. Go's own
// rule decides the fix ("each time a defer statement executes, the function
// value and parameters are evaluated as usual and saved anew"): the callee
// is just one more defer-time value, so it is snapshotted into the env like
// any argument — evaluated NOW in the outer function (it lowers to the
// universal {fn_ptr, env_ptr} funcval pair; TYPE_FUNCTION's LLVM lowering,
// type_mapping.c), stored as the env's leading field, and rebound in the
// thunk to a synthetic identifier spliced into call->function for the
// emission — call_codegen's indirect-funcval path then handles the actual
// call (nil check, env-first ABI, variadic packing) unchanged. Callees that
// resolve to module-global symbols from any function (package selectors,
// named top-level functions, method selectors — the receiver, not the
// method, is the value there) keep the plain re-emit, which also keeps the
// static path's exact capability envelope for them.
static int codegen_generate_defer_stmt_stack(CodeGenerator* codegen, TypeChecker* checker,
                                             ASTNode* stmt, FunctionInfo* fi,
                                             CallExprNode* call) {
    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef ptr_ty = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);

    if (!fi->defer_frame) {
        codegen_error(codegen, stmt->pos, "internal: stack-mode function has no defer frame");
        return 0;
    }

    // Same receiver-detection as the static path (codegen_generate_defer_stmt
    // below): `defer x.m(args)` snapshots the selector base at defer-time
    // UNLESS it's a package selector (fmt.Println) whose base has no
    // runtime value.
    //
    // callee_expr: non-NULL when the callee itself must be snapshotted (see
    // the CALLEE SNAPSHOT doc block above). Mutually exclusive with
    // recv_base by construction — a selector callee takes the recv arm, a
    // non-selector callee the value-classification arm.
    ASTNode* recv_base = NULL;
    ASTNode* callee_expr = NULL;
    if (call->function && call->function->type == AST_SELECTOR_EXPR) {
        ASTNode* base = ((SelectorExprNode*)call->function)->expr;
        if (base && base->node_type && base->node_type->kind != TYPE_PACKAGE)
            recv_base = base;
    } else if (call->function && call->function->type == AST_IDENTIFIER) {
        // Identifier callee: a NAMED top-level function is not in codegen's
        // value table (it lives in the type-checker's function registry and
        // resolves to a module-global symbol from any function — safe to
        // re-emit in the thunk). A value-table hit whose type is a function
        // is a func-typed local/param — a value that must be snapshotted.
        ValueInfo* cv = codegen_lookup_value(codegen,
                                             ((IdentifierNode*)call->function)->name);
        if (cv && cv->goo_type && cv->goo_type->kind == TYPE_FUNCTION)
            callee_expr = call->function;
    } else if (call->function && call->function->node_type &&
               call->function->node_type->kind == TYPE_FUNCTION) {
        // Any other function-typed callee expression: func literal
        // (capturing or not), call returning a func, indexed func slot, ...
        callee_expr = call->function;
    }

    size_t argc = 0;
    for (ASTNode* a = call->args; a; a = a->next) argc++;
    size_t total = argc + (recv_base ? 1 : 0) + (callee_expr ? 1 : 0);

    // Evaluate every snapshot value now, in ORIGINAL-function context, at
    // the current (possibly loop-body) insert point.
    LLVMValueRef* vals = total ? calloc(total, sizeof(LLVMValueRef)) : NULL;
    Type**        types = total ? calloc(total, sizeof(Type*)) : NULL;
    if (total && (!vals || !types)) {
        free(vals); free(types);
        codegen_error(codegen, stmt->pos, "out of memory snapshotting defer args");
        return 0;
    }

    size_t idx = 0;
    if (callee_expr) {
        // Evaluate the callee to its funcval pair (Go: the function value
        // is evaluated when the defer statement executes). A capturing
        // literal builds its capture env HERE, in the outer function, where
        // the captured slots are legal to reference.
        ValueInfo* cvv = codegen_generate_expression(codegen, checker, callee_expr);
        if (!cvv) {
            free(vals); free(types);
            codegen_error(codegen, callee_expr->pos, "failed to evaluate deferred callee");
            return 0;
        }
        LLVMValueRef cval = cvv->llvm_value;
        Type* ct = cvv->goo_type ? cvv->goo_type : callee_expr->node_type;
        if (cvv->is_lvalue && ct) {
            LLVMTypeRef lt = codegen_type_to_llvm(codegen, ct);
            if (lt) cval = LLVMBuildLoad2(codegen->builder, lt, cval, "sdefer_fn");
        }
        value_info_free(cvv);
        if (!ct || ct->kind != TYPE_FUNCTION) {
            free(vals); free(types);
            codegen_error(codegen, callee_expr->pos,
                          "internal: deferred callee did not resolve to a function type");
            return 0;
        }
        vals[idx] = cval; types[idx] = ct; idx++;
    }
    if (recv_base) {
        ValueInfo* rv = codegen_generate_expression(codegen, checker, recv_base);
        if (!rv) {
            free(vals); free(types);
            codegen_error(codegen, recv_base->pos, "failed to evaluate defer receiver");
            return 0;
        }
        LLVMValueRef rval = rv->llvm_value;
        Type* rt = rv->goo_type;
        if (rv->is_lvalue && rt) {
            LLVMTypeRef lt = codegen_type_to_llvm(codegen, rt);
            if (lt) rval = LLVMBuildLoad2(codegen->builder, lt, rval, "sdefer_recv");
        }
        value_info_free(rv);
        vals[idx] = rval; types[idx] = rt; idx++;
    }
    for (ASTNode* a = call->args; a; a = a->next) {
        ValueInfo* av = codegen_generate_expression(codegen, checker, a);
        if (!av) {
            free(vals); free(types);
            codegen_error(codegen, a->pos, "failed to evaluate defer argument");
            return 0;
        }
        LLVMValueRef val = av->llvm_value;
        Type* gt = av->goo_type;
        if (av->is_lvalue && gt) {
            LLVMTypeRef lt = codegen_type_to_llvm(codegen, gt);
            if (lt) val = LLVMBuildLoad2(codegen->builder, lt, val, "sdefer_arg");
        }
        value_info_free(av);
        vals[idx] = val; types[idx] = gt; idx++;
    }

    // Build the env struct type (one field per snapshot, receiver first —
    // same order the values were just evaluated in) and heap-allocate one
    // instance sized to hold them all (goo_alloc via codegen_emit_alloc —
    // the same allocator method values' bound-receiver cell uses,
    // composite_codegen.c). A zero-arg, zero-receiver defer (e.g. `defer
    // cleanup()`) needs no env at all — NULL, and the thunk below ignores
    // its env param.
    LLVMTypeRef  env_ty = NULL;
    LLVMTypeRef* field_types = NULL;
    LLVMValueRef env_ptr;
    if (total > 0) {
        field_types = malloc(sizeof(LLVMTypeRef) * total);
        if (!field_types) {
            free(vals); free(types);
            codegen_error(codegen, stmt->pos, "out of memory building defer env type");
            return 0;
        }
        for (size_t i = 0; i < total; i++) {
            field_types[i] = types[i] ? codegen_type_to_llvm(codegen, types[i]) : LLVMTypeOf(vals[i]);
            if (!field_types[i]) {
                free(field_types); free(vals); free(types);
                codegen_error(codegen, stmt->pos, "internal: failed to lower defer snapshot type");
                return 0;
            }
        }
        env_ty = LLVMStructTypeInContext(ctx, field_types, (unsigned)total, 0);

        LLVMValueRef size = LLVMSizeOf(env_ty);
        env_ptr = codegen_emit_alloc(codegen, size, ALLOC_KIND_DEFAULT, NULL);
        if (!env_ptr) {
            free(field_types); free(vals); free(types);
            codegen_error(codegen, stmt->pos, "internal: failed to allocate defer env");
            return 0;
        }
        for (size_t i = 0; i < total; i++) {
            LLVMValueRef field = LLVMBuildStructGEP2(codegen->builder, env_ty, env_ptr,
                                                      (unsigned)i, "sdefer_env_field");
            LLVMBuildStore(codegen->builder, vals[i], field);
        }
    } else {
        env_ptr = LLVMConstNull(ptr_ty);
    }

    // Synthesize the per-statement thunk `void @<fn>.defer<N>_thunk(ptr
    // env)` as its own LLVM function. `fi->deferred_count` is reused purely
    // as a per-function defer-statement counter here (stack mode never
    // touches fi->deferred_calls[]/g_defer_info — those are the static
    // path's own bookkeeping) so every thunk name is unique within this
    // function, and fi->name is already unique per instantiation (mangled
    // per monomorphized instance), so the full thunk symbol is unique
    // module-wide too.
    char thunk_name[300];
    snprintf(thunk_name, sizeof(thunk_name), "%s.defer%zu_thunk",
             fi->name ? fi->name : "fn", fi->deferred_count);
    LLVMTypeRef thunk_ty = LLVMFunctionType(LLVMVoidTypeInContext(ctx), &ptr_ty, 1, 0);
    LLVMValueRef thunk_fn = LLVMAddFunction(codegen->module, thunk_name, thunk_ty);
    LLVMSetLinkage(thunk_fn, LLVMInternalLinkage);

    // --- Save every piece of ambient state this nested emission touches
    // (mirrors codegen_generate_func_lit's save/restore block exactly —
    // function_codegen.c). Must happen BEFORE codegen_enter_function. ---
    LLVMValueRef       saved_function = codegen->current_function;
    FunctionInfo*      saved_function_info = codegen->current_function_info;
    LLVMBasicBlockRef  saved_block = LLVMGetInsertBlock(codegen->builder);
    size_t             saved_vtab_start = codegen->value_table_function_start;
    Scope*             enclosing_scope = checker->current_scope;
    ControlFlowContext saved_cfctx;
    cfctx_save(&saved_cfctx, &codegen->cfctx);
    codegen->cfctx.loop_depth = 0;

    FunctionInfo* thunk_fi = function_info_new(thunk_name, thunk_fn, NULL);
    if (!thunk_fi) {
        codegen_error(codegen, stmt->pos, "out of memory building defer thunk");
        free(field_types); free(vals); free(types);
        return 0;
    }
    thunk_fi->entry_block = LLVMAppendBasicBlockInContext(ctx, thunk_fn, "entry");
    codegen_enter_function(codegen, thunk_fi);
    codegen_set_insert_point(codegen, thunk_fi->entry_block);

    scope_push(checker);
    checker->current_scope->is_function_boundary = 1;

    LLVMValueRef env_param = LLVMGetParam(thunk_fn, 0);

    // Unpack each env field into a fresh entry alloca inside the THUNK
    // (codegen_create_entry_alloca now targets thunk_fi's entry block, since
    // codegen->current_function_info was just repointed at it), bind a
    // synthetic identifier to it exactly the way the static path's
    // codegen_generate_defer_stmt does for its own entry-alloca snapshots —
    // same naming scheme (`__goo_defer<N>_recv`/`_arg<M>`, plus `_fn` for a
    // snapshotted callee); safe to reuse verbatim because a function is
    // EITHER fully static or fully stack-mode, never mixed, so there is no
    // cross-mode collision risk. A snapshotted CALLEE binds as a func-typed
    // lvalue, which is precisely the shape call_codegen's indirect-funcval
    // arm expects — the spliced call below goes through the same nil-check +
    // env-first indirect call any `f()` on a func-typed local takes.
    ASTNode* callee_synth = NULL;
    ASTNode* recv_synth = NULL;
    ASTNode* args_synth_head = NULL;
    ASTNode* prev_synth = NULL;
    for (size_t i = 0; i < total; i++) {
        LLVMTypeRef field_llvm = field_types[i];
        LLVMValueRef field_ptr = LLVMBuildStructGEP2(codegen->builder, env_ty, env_param,
                                                      (unsigned)i, "sdefer_thunk_field");
        LLVMValueRef field_val = LLVMBuildLoad2(codegen->builder, field_llvm, field_ptr, "sdefer_thunk_load");
        LLVMValueRef slot = codegen_create_entry_alloca(codegen, field_llvm, "sdefer_thunk_slot");
        LLVMBuildStore(codegen->builder, field_val, slot);

        int is_callee = (callee_expr && i == 0);
        int is_recv = (recv_base && i == 0);  // exclusive with is_callee
        char nm[64];
        if (is_callee) {
            snprintf(nm, sizeof(nm), "__goo_defer%zu_fn", fi->deferred_count);
        } else if (is_recv) {
            snprintf(nm, sizeof(nm), "__goo_defer%zu_recv", fi->deferred_count);
        } else {
            size_t argi = i - ((recv_base || callee_expr) ? 1 : 0);
            snprintf(nm, sizeof(nm), "__goo_defer%zu_arg%zu", fi->deferred_count, argi);
        }

        ValueInfo* vi = value_info_new(nm, slot, types[i]);
        if (vi) { vi->is_lvalue = 1; vi->is_initialized = 1; vscope_add(codegen, vi); }
        type_checker_declare_synthetic(checker, nm, types[i]);

        IdentifierNode* id = ast_identifier_new(nm, stmt->pos);
        ((ASTNode*)id)->node_type = types[i];
        if (is_callee) {
            callee_synth = (ASTNode*)id;
        } else if (is_recv) {
            recv_synth = (ASTNode*)id;
        } else {
            ASTNode* idn = (ASTNode*)id;
            if (prev_synth) prev_synth->next = idn; else args_synth_head = idn;
            prev_synth = idn;
        }
    }
    free(field_types); free(vals); free(types);

    // Splice the synthetic nodes into the ORIGINAL (shared template) call
    // node for exactly this one emission, restoring the originals right
    // after — same transactional pattern codegen_emit_deferred_calls uses,
    // just performed once (at thunk-build time) instead of once per exit
    // site, since the call is emitted exactly once here (inside the
    // thunk), not re-emitted at every function exit. A snapshotted callee
    // splices call->function itself (the whole callee expression is
    // replaced by the synthetic identifier); a method receiver splices only
    // the selector's base, leaving the selector node in place.
    ASTNode* saved_args = call->args;
    ASTNode* saved_fn = NULL;
    SelectorExprNode* sel = NULL;
    ASTNode* saved_recv = NULL;
    if (callee_synth) {
        saved_fn = call->function;
        call->function = callee_synth;
    } else if (recv_synth && call->function && call->function->type == AST_SELECTOR_EXPR) {
        sel = (SelectorExprNode*)call->function;
        saved_recv = sel->expr;
        sel->expr = recv_synth;
    }
    if (args_synth_head) call->args = args_synth_head;

    ValueInfo* result = codegen_generate_expression(codegen, checker, (ASTNode*)call);
    if (result) value_info_free(result);

    call->args = saved_args;
    if (callee_synth) call->function = saved_fn;
    if (sel) sel->expr = saved_recv;

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)))
        LLVMBuildRetVoid(codegen->builder);

    // --- Restore ambient state, mirror order of the save above. ---
    scope_pop(checker);
    checker->current_scope = enclosing_scope;

    codegen_exit_function(codegen);
    function_info_free(thunk_fi);

    codegen->value_table_function_start = saved_vtab_start;
    codegen->current_function = saved_function;
    codegen->current_function_info = saved_function_info;
    if (saved_block) codegen_set_insert_point(codegen, saved_block);
    cfctx_restore(&codegen->cfctx, &saved_cfctx);

    // Back in the outer (original) function: push {thunk, env}. THIS call
    // site is what runs once per dynamic execution (once per loop
    // iteration), giving Go's per-iteration defer semantics — the thunk
    // itself is emitted only once, at compile time.
    LLVMTypeRef push_params[] = { ptr_ty, ptr_ty, ptr_ty };
    LLVMTypeRef push_ty = LLVMFunctionType(LLVMVoidTypeInContext(ctx), push_params, 3, 0);
    LLVMValueRef push_fn = LLVMGetNamedFunction(codegen->module, "goo_defer_push");
    if (!push_fn) push_fn = LLVMAddFunction(codegen->module, "goo_defer_push", push_ty);
    LLVMValueRef push_args[] = { fi->defer_frame, thunk_fn, env_ptr };
    LLVMBuildCall2(codegen->builder, push_ty, push_fn, push_args, 3, "");

    fi->deferred_count++;  // per-function thunk/synthetic-name uniquifier only
    return 1;
}
#endif

int codegen_generate_defer_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available for defer statements");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_DEFER_STMT) return 0;

    DeferStmtNode* defer_stmt = (DeferStmtNode*)stmt;
    if (!defer_stmt->call) return 1;

    FunctionInfo* fi = codegen->current_function_info;
    if (!fi) {
        codegen_error(codegen, stmt->pos, "defer statement outside of a function");
        return 0;
    }

    if (defer_stmt->call->type != AST_CALL_EXPR) {
        codegen_error(codegen, stmt->pos, "defer requires a function call");
        return 0;
    }
    CallExprNode* call = (CallExprNode*)defer_stmt->call;

    // P3.4: a defer inside a loop accumulates one deferred call PER
    // ITERATION in Go, which a single static per-lexical-defer slot cannot
    // model. The pre-pass (function_codegen.c's defer_prepass_needs_stack,
    // run before body codegen) already decided, for the WHOLE function,
    // whether any of its defers are loop-nested — if so, EVERY defer here
    // routes through the runtime stack instead (per-function fork, not
    // per-statement: see FunctionInfo.defer_stack_mode's doc comment for
    // why mixing the two mechanisms within one function is unsound). This
    // used to be an outright rejection; loop-nested defers are now fully
    // supported.
    if (fi->defer_stack_mode) {
        return codegen_generate_defer_stmt_stack(codegen, checker, stmt, fi, call);
    }

    // Fail-closed backstop (defense in depth, review 2026-07-10): the
    // pre-pass decided this function needs no runtime stack, yet codegen is
    // NOW inside a real loop (cfctx tracks actual loop frames independently
    // of the pre-pass — loop_is_loop[i] is 1 only for cfctx_push_loop
    // frames, 0 for switch/select/type-switch break-scopes, so this never
    // fires for a defer under a plain switch). That means the pre-pass
    // walker missed the container this loop is nested under. Emitting the
    // static single-slot form here would run the defer ONCE with the last
    // iteration's snapshot — the exact silent-miscompile class the walker's
    // ARENA_BLOCK/LABEL_STMT gaps produced before they were fixed. Refuse
    // loudly instead: a compile error is strictly better than wrong output,
    // and matches pre-P3.4 behavior (which rejected all loop defers).
    for (int i = 0; i < codegen->cfctx.loop_depth; i++) {
        if (codegen->cfctx.loop_is_loop[i]) {
            codegen_error(codegen, stmt->pos,
                          "internal: loop-nested defer reached the static defer path "
                          "(defer pre-pass coverage bug — see defer_prepass_walk's "
                          "maintenance contract, function_codegen.c)");
            return 0;
        }
    }

    // First defer of this function: reset the parallel codegen-info cache.
    if (fi->deferred_count == 0) defer_info_reset(fi);

    LLVMTypeRef i1 = LLVMInt1TypeInContext(codegen->context);

    // Runtime active flag: 0 at entry, set to 1 here (at the defer site, so it
    // only becomes 1 if control actually reaches this defer at runtime).
    LLVMValueRef flag = codegen_create_entry_alloca(codegen, i1, "defer_active");
    if (!flag) {
        codegen_error(codegen, stmt->pos, "failed to allocate defer flag");
        return 0;
    }
    defer_entry_store_zero(codegen, flag, i1);

    // Snapshot each argument NOW (defer-time evaluation), store it into an
    // entry-block alloca, and build (but do not yet link in) a synthetic
    // identifier node that resolves to that snapshot when the call is
    // emitted at function exit. The original argument node is left
    // untouched here — `call` is shared template AST, so a permanent
    // rewrite would corrupt it for the next instantiation; the synthetic
    // node is only spliced in transactionally, per emission, by
    // codegen_emit_deferred_calls.
    size_t argc = 0;
    for (ASTNode* a = call->args; a; a = a->next) argc++;

    // Method-call receiver: `defer x.m(args)` evaluates the receiver `x` at
    // defer-time (Go semantics), but it lives in the selector base (call->func),
    // not call->args. Snapshot it too so it doesn't dangle at function exit
    // ("Undefined identifier 'x'"). Skip PACKAGE selectors like `fmt.Println`
    // (base resolves to TYPE_PACKAGE, not a value) — those need no snapshot.
    ASTNode* recv_base = NULL;
    if (call->function && call->function->type == AST_SELECTOR_EXPR) {
        ASTNode* base = ((SelectorExprNode*)call->function)->expr;
        if (base && base->node_type && base->node_type->kind != TYPE_PACKAGE)
            recv_base = base;
    }
    size_t total = argc + (recv_base ? 1 : 0);

    DeferCodegenInfo cinfo;
    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.active_flag = flag;
    cinfo.arg_count = total;
    if (total > 0) {
        cinfo.arg_slots = calloc(total, sizeof(LLVMValueRef));
        cinfo.arg_types = calloc(total, sizeof(Type*));
        cinfo.arg_names = calloc(total, sizeof(char*));
        if (!cinfo.arg_slots || !cinfo.arg_types || !cinfo.arg_names) {
            free(cinfo.arg_slots); free(cinfo.arg_types); free(cinfo.arg_names);
            codegen_error(codegen, stmt->pos, "out of memory snapshotting defer args");
            return 0;
        }
    }

    size_t idx = 0;

    // Snapshot the method receiver (if any) first. A synthetic identifier
    // that will resolve to the snapshot at exit is built now but NOT linked
    // into call->function here — call is the shared template AST node (this
    // DeferStmtNode is revisited once per instantiation), so splicing it in
    // permanently would corrupt the template for the next instance (the
    // destructive-splice bug). codegen_emit_deferred_calls links it in only
    // for the duration of each emission and restores the original receiver
    // immediately after.
    if (recv_base) {
        ValueInfo* rv = codegen_generate_expression(codegen, checker, recv_base);
        if (!rv) {
            codegen_error(codegen, recv_base->pos, "failed to evaluate defer receiver");
            return 0;
        }
        LLVMValueRef rval = rv->llvm_value;
        Type* rt = rv->goo_type;
        if (rv->is_lvalue && rt) {
            LLVMTypeRef lt = codegen_type_to_llvm(codegen, rt);
            if (lt) rval = LLVMBuildLoad2(codegen->builder, lt, rval, "defer_recv");
        }
        LLVMTypeRef slot_ty = rt ? codegen_type_to_llvm(codegen, rt) : LLVMTypeOf(rval);
        LLVMValueRef slot = codegen_create_entry_alloca(codegen, slot_ty, "defer_recv_slot");
        LLVMBuildStore(codegen->builder, rval, slot);
        value_info_free(rv);

        char nm[64];
        snprintf(nm, sizeof(nm), "__goo_defer%zu_recv", fi->deferred_count);
        cinfo.arg_slots[idx] = slot;
        cinfo.arg_types[idx] = rt;
        cinfo.arg_names[idx] = strdup(nm);
        type_checker_declare_synthetic(checker, nm, rt);

        IdentifierNode* rid = ast_identifier_new(nm, recv_base->pos);
        ((ASTNode*)rid)->node_type = rt;
        cinfo.recv_synth = (ASTNode*)rid;
        idx++;
    }

    // Build (but do not splice) a synthetic identifier chain standing in for
    // call->args, in original argument order. Same reasoning as the receiver
    // above: `a` (the original argument nodes) is left completely untouched —
    // still owned by the template AST, still linked exactly as the parser
    // built it — and cinfo.args_synth_head is a SEPARATE, independent chain
    // that codegen_emit_deferred_calls splices into call->args only for the
    // duration of each emission.
    ASTNode* prev_synth = NULL;
    ASTNode* a = call->args;
    while (a) {
        ASTNode* nextarg = a->next;

        ValueInfo* av = codegen_generate_expression(codegen, checker, a);
        if (!av) {
            codegen_error(codegen, a->pos, "failed to evaluate defer argument");
            return 0;
        }
        LLVMValueRef val = av->llvm_value;
        Type* gt = av->goo_type;
        if (av->is_lvalue && gt) {
            LLVMTypeRef lt = codegen_type_to_llvm(codegen, gt);
            if (lt) {
                val = LLVMBuildLoad2(codegen->builder, lt, val, "defer_arg");
            }
        }
        LLVMTypeRef slot_ty = gt ? codegen_type_to_llvm(codegen, gt) : LLVMTypeOf(val);
        LLVMValueRef slot = codegen_create_entry_alloca(codegen, slot_ty, "defer_arg_slot");
        LLVMBuildStore(codegen->builder, val, slot);
        value_info_free(av);

        char nm[64];
        snprintf(nm, sizeof(nm), "__goo_defer%zu_arg%zu", fi->deferred_count, idx);
        cinfo.arg_slots[idx] = slot;
        cinfo.arg_types[idx] = gt;
        cinfo.arg_names[idx] = strdup(nm);

        // Register the synthetic name in the type-checker scope with its real
        // snapshotted type. codegen_emit_deferred_calls re-type-checks the
        // rewritten call at function exit (call_codegen invokes
        // type_check_call_expr to recover the return type); without this binding
        // a USER-DEFINED deferred call would emit a spurious "Undefined variable
        // '__goo_deferN_argM'" for each arg. fmt.Println takes a different
        // codegen path that skips the re-check, which is why the builtin-only
        // probe never surfaced this.
        type_checker_declare_synthetic(checker, nm, gt);

        // Chain a synthetic identifier onto cinfo.args_synth_head, standing
        // in for this argument. NOT linked into call->args here — see the
        // comment above the `while (a)` loop and codegen_emit_deferred_calls.
        IdentifierNode* id = ast_identifier_new(nm, a->pos);
        ASTNode* idn = (ASTNode*)id;
        if (prev_synth) prev_synth->next = idn; else cinfo.args_synth_head = idn;
        prev_synth = idn;

        a = nextarg;
        idx++;
    }

    // Mark this defer active at runtime (current block; runs only if reached).
    LLVMBuildStore(codegen->builder, LLVMConstInt(i1, 1, 0), flag);

    // Register the call node (still holding its ORIGINAL args/receiver —
    // untouched by this function) on the FunctionInfo and its codegen info in
    // the parallel cache. Both arrays grow in lock-step from index 0.
    if (fi->deferred_count >= fi->deferred_capacity) {
        size_t newcap = fi->deferred_capacity ? fi->deferred_capacity * 2 : 4;
        ASTNode** grown = realloc(fi->deferred_calls, newcap * sizeof(ASTNode*));
        if (!grown) {
            codegen_error(codegen, stmt->pos, "out of memory registering defer");
            return 0;
        }
        fi->deferred_calls = grown;
        fi->deferred_capacity = newcap;
    }
    if (g_defer_info_count >= g_defer_info_capacity) {
        size_t newcap = g_defer_info_capacity ? g_defer_info_capacity * 2 : 4;
        DeferCodegenInfo* grown = realloc(g_defer_info, newcap * sizeof(DeferCodegenInfo));
        if (!grown) {
            codegen_error(codegen, stmt->pos, "out of memory registering defer info");
            return 0;
        }
        g_defer_info = grown;
        g_defer_info_capacity = newcap;
    }
    fi->deferred_calls[fi->deferred_count] = defer_stmt->call;
    g_defer_info[fi->deferred_count] = cinfo;
    g_defer_info_count = fi->deferred_count + 1;
    fi->deferred_count++;

    return 1;
#endif
}

int codegen_generate_select_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available for select statements");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_SELECT_STMT) return 0;

    LLVMContextRef ctx = codegen->context;
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

    // Create array of select cases (one slot per case; the default case's slot
    // is marked inactive with a NULL channel so the runtime skips it). All types
    // are built in codegen->context — the same fix M7 applied to channels.
    LLVMTypeRef select_case_type = codegen_get_select_case_type(codegen);
    LLVMValueRef cases_array = LLVMBuildArrayAlloca(codegen->builder, select_case_type,
                                                   LLVMConstInt(LLVMInt64TypeInContext(ctx), case_count, 0),
                                                   "select_cases");

    // Create basic blocks for each case and the end
    LLVMBasicBlockRef* case_blocks = malloc(sizeof(LLVMBasicBlockRef) * case_count);
    // gofmt-syntax-b Task 4 (P1.10): the receive `recv_space` alloca
    // codegen_setup_select_case builds for each receive case (calloc'd to
    // all-NULL so send cases and the default's unused slot stay NULL — a
    // binding can only exist on a receive case, enforced in the type
    // checker). The dispatch loop below reads back through this array to
    // copy the ALREADY-RECEIVED value into the bound name/lvalue — goo_select
    // has already performed the one and only receive by the time any case
    // block runs, so this is a plain load, never a second goo_chan_recv.
    LLVMValueRef* recv_spaces = calloc(case_count, sizeof(LLVMValueRef));
    LLVMBasicBlockRef default_block = NULL;
    LLVMBasicBlockRef end_block = LLVMAppendBasicBlockInContext(ctx, codegen->current_function, "select_end");

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
                free(recv_spaces);
                return 0;
            }
            has_default = 1;
            default_block = LLVMAppendBasicBlockInContext(ctx, codegen->current_function, "select_default");
            case_blocks[case_index] = default_block;

            // Mark this slot inactive: goo_select skips cases whose channel is
            // NULL, so the default never counts as "ready" during polling.
            LLVMValueRef d_idx = LLVMConstInt(LLVMInt64TypeInContext(ctx), case_index, 0);
            LLVMValueRef d_slot = LLVMBuildGEP2(codegen->builder, select_case_type, cases_array,
                                                &d_idx, 1, "default_slot");
            LLVMValueRef d_chan = LLVMBuildStructGEP2(codegen->builder, select_case_type, d_slot, 0,
                                                      "default_chan_field");
            LLVMBuildStore(codegen->builder,
                           LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(ctx), 0)), d_chan);
        } else {
            // Regular case
            char case_name[32];
            snprintf(case_name, sizeof(case_name), "select_case_%zu", case_index);
            case_blocks[case_index] = LLVMAppendBasicBlockInContext(ctx, codegen->current_function, case_name);

            // Setup select case data
            if (!codegen_setup_select_case(codegen, checker, cases_array, case_index, select_case,
                                            &recv_spaces[case_index])) {
                free(case_blocks);
                free(recv_spaces);
                return 0;
            }
        }
        
        case_node = case_node->next;
        case_index++;
    }
    
    // Call goo_select to determine which case is ready. We pass the FULL case
    // count (the default's slot is skipped at runtime via its NULL channel) so
    // the returned index lines up with case_blocks[]. timeout encodes blocking
    // policy: 0 => non-blocking because a default is present (fire it if nothing
    // is ready); -1 => block until a case becomes ready.
    LLVMValueRef select_func = codegen_get_select_function(codegen);
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMTypeRef i64_ty = LLVMInt64TypeInContext(ctx);
    LLVMTypeRef param_types[] = {
        void_ptr_type,   // goo_select_case_t* cases
        i64_ty,          // size_t num_cases
        i64_ty           // int64_t timeout_ns
    };
    LLVMTypeRef func_type = LLVMFunctionType(LLVMInt32TypeInContext(ctx), param_types, 3, 0);

    LLVMValueRef case_count_val = LLVMConstInt(i64_ty, case_count, 0);
    LLVMValueRef timeout_val = LLVMConstInt(i64_ty,
                                            has_default ? 0ULL : (unsigned long long)(-1LL), 0);

    LLVMValueRef args[] = { cases_array, case_count_val, timeout_val };
    LLVMValueRef selected_case = LLVMBuildCall2(codegen->builder, func_type, select_func, args, 3, "selected_case");

    // Create switch based on the result. -1 (nothing ready, default present) and
    // any unmatched value fall through to the default (or end) block.
    LLVMValueRef switch_inst = LLVMBuildSwitch(codegen->builder, selected_case,
                                               has_default ? default_block : end_block,
                                               (unsigned)case_count);

    // Add cases to switch
    case_index = 0;
    case_node = select_stmt->cases;
    while (case_node && case_index < case_count) {
        SelectCaseNode* select_case = (SelectCaseNode*)case_node;

        if (select_case->comm != NULL) {
            // Regular case - add to switch (full-list index matches the runtime).
            LLVMValueRef case_val = LLVMConstInt(LLVMInt32TypeInContext(ctx), case_index, 0);
            LLVMAddCase(switch_inst, case_val, case_blocks[case_index]);
        }

        case_node = case_node->next;
        case_index++;
    }
    
    // `break` inside a select case must terminate the SELECT (Go semantics),
    // not the enclosing loop. Push a break-only scope targeting end_block;
    // `continue` still threads through to the enclosing loop (or errors if none).
    if (!cfctx_push_break_scope(&codegen->cfctx, end_block)) {
        codegen_error(codegen, stmt->pos, "select nested too deeply for break handling");
        free(case_blocks);
        free(recv_spaces);
        return 0;
    }

    // Generate code for each case block
    case_index = 0;
    case_node = select_stmt->cases;
    while (case_node && case_index < case_count) {
        SelectCaseNode* select_case = (SelectCaseNode*)case_node;

        LLVMPositionBuilderAtEnd(codegen->builder, case_blocks[case_index]);

        // Case scope: a `:=` bind above must not outlive this case's body —
        // neither past the select (if it shadows an outer variable) nor into
        // a later case's body of the same select (codegen emits every case
        // block regardless of which one runs at runtime, so an untruncated
        // value table lets a later case's lookup see an earlier case's
        // never-stored alloca). Snapshot the high-water mark now and
        // truncate back after the body, same mechanism as the match-arm
        // teardown in composite_codegen.c and the block-stmt teardown above
        // in this file — the type checker already guarantees no legal
        // cross-case reference to this binding exists, so truncation alone
        // (no explicit lookup needed) is safe.
        size_t pre_case_vt_size = vscope_enter(codegen);

        // gofmt-syntax-b Task 4 (P1.10): copy the already-received value into
        // the bound name/lvalue BEFORE the body runs. goo_select has already
        // performed the ONE AND ONLY receive for this case (recv_spaces[case_
        // index] is the buffer codegen_setup_select_case wrote it into) — this
        // is a plain load + store/alloca, never a second goo_chan_recv (the
        // P0.1 miscompile class). recv_spaces[case_index] is NULL for every
        // case this arc's pre-existing fixtures exercise (no bind_name), so
        // this block is a no-op for them.
        if (select_case->bind_name && recv_spaces[case_index]) {
            // `_` is a discard, like every other short-decl form (mirrors
            // the type checker's skip-declare-for-`_` above) — the receive
            // already happened via goo_select; nothing further to bind.
            if (!(select_case->is_declare && strcmp(select_case->bind_name, "_") == 0)) {
                Type* elem_type = select_case->comm->node_type;
                LLVMTypeRef elem_llvm = elem_type
                    ? codegen_type_to_llvm(codegen, elem_type)
                    // mirrors codegen_setup_select_case's own i32 fallback
                    : LLVMInt32TypeInContext(ctx);
                LLVMValueRef loaded = LLVMBuildLoad2(codegen->builder, elem_llvm,
                                                     recv_spaces[case_index], "select_bind_load");
                if (select_case->is_declare) {
                    // `:=` — fresh alloca, scoped to this case body (mirrors
                    // the if-let / multi-assign short-decl binding pattern
                    // elsewhere in this file). The value table is truncated
                    // back to pre_case_vt_size after the body below, so this
                    // binding does not leak into a later case or past the
                    // select.
                    LLVMValueRef slot = codegen_alloc_local(codegen, elem_llvm, select_case->bind_name);
                    LLVMBuildStore(codegen->builder, loaded, slot);
                    ValueInfo* vi = value_info_new(select_case->bind_name, slot, elem_type);
                    vi->is_lvalue = 1;
                    vi->is_initialized = 1;
                    vscope_add(codegen, vi);
                } else {
                    // `=` — store into the existing variable's alloca. The
                    // type checker already proved bind_name is a declared,
                    // type-compatible variable in an enclosing scope.
                    ValueInfo* existing = codegen_lookup_value(codegen, select_case->bind_name);
                    if (!existing || !existing->is_lvalue) {
                        codegen_error(codegen, case_node->pos,
                                     "select case: '%s' is not an assignable variable",
                                     select_case->bind_name);
                        cfctx_pop(&codegen->cfctx);
                        free(case_blocks);
                        free(recv_spaces);
                        return 0;
                    }
                    // Box a concrete received value into an interface-typed
                    // target (mirrors the multi-assign '=' path above and
                    // type_check_assignment_op's ordinary `x = e` path) —
                    // same layout store otherwise (interface->interface
                    // needs no box).
                    LLVMValueRef sval = loaded;
                    if (existing->goo_type && existing->goo_type->kind == TYPE_INTERFACE &&
                        elem_type && elem_type->kind != TYPE_INTERFACE) {
                        LLVMValueRef boxed = codegen_interface_box(codegen, checker,
                                                                   existing->goo_type,
                                                                   elem_type, loaded);
                        if (!boxed) {
                            codegen_error(codegen, case_node->pos,
                                         "failed to box select-received value into interface");
                            cfctx_pop(&codegen->cfctx);
                            free(case_blocks);
                            free(recv_spaces);
                            return 0;
                        }
                        sval = boxed;
                    }
                    LLVMBuildStore(codegen->builder, sval, existing->llvm_value);
                }
            }
        }

        // Generate case body. The body is a ->next statement chain — loop it
        // like switch codegen does (a single codegen_generate_statement call
        // silently dropped every statement after the first).
        for (ASTNode* s = select_case->body; s; s = s->next) {
            if (!codegen_generate_statement(codegen, checker, s)) {
                cfctx_pop(&codegen->cfctx);
                free(case_blocks);
                free(recv_spaces);
                return 0;
            }
        }

        // Branch to end, unless the body already terminated (e.g. return/break).
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
            LLVMBuildBr(codegen->builder, end_block);
        }

        // Restore the value table: this case's `:=` bind (if any) must not
        // be visible to the next case's bind/body codegen, nor after the
        // select once we exit this loop on the last case.
        vscope_exit(codegen, pre_case_vt_size);

        case_node = case_node->next;
        case_index++;
    }
    cfctx_pop(&codegen->cfctx);

    // Position builder at end block
    LLVMPositionBuilderAtEnd(codegen->builder, end_block);

    free(case_blocks);
    free(recv_spaces);
    return 1;
#endif
}

#if LLVM_AVAILABLE
// Helper function to get select case type. Layout must match goo_select_case_t
// in runtime.h exactly: { goo_channel_t*, void*, int is_send, int ready }.
LLVMTypeRef codegen_get_select_case_type(CodeGenerator* codegen) {
    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
    LLVMTypeRef field_types[] = {
        void_ptr_type,  // goo_channel_t* channel
        void_ptr_type,  // void* data
        i32,            // int is_send
        i32             // int ready
    };
    return LLVMStructTypeInContext(ctx, field_types, 4, 0);
}
#endif

#if LLVM_AVAILABLE
// Helper function to get goo_select function
LLVMValueRef codegen_get_select_function(CodeGenerator* codegen) {
    LLVMValueRef select_func = LLVMGetNamedFunction(codegen->module, "goo_select");
    if (!select_func) {
        // Declare goo_select if not already declared
        LLVMContextRef ctx = codegen->context;
        LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);
        LLVMTypeRef param_types[] = {
            void_ptr_type,   // goo_select_case_t* cases
            i64,             // size_t num_cases
            i64              // int64_t timeout_ns
        };
        LLVMTypeRef func_type = LLVMFunctionType(LLVMInt32TypeInContext(ctx), param_types, 3, 0);
        select_func = LLVMAddFunction(codegen->module, "goo_select", func_type);
    }
    return select_func;
}
#endif

#if LLVM_AVAILABLE
// Helper function to setup select case data
//
// gofmt-syntax-b Task 4 (P1.10): out_recv_space, when non-NULL, receives the
// `recv_space` alloca built below for a RECEIVE case (untouched for a send
// case or the default's inactive slot) — the dispatch loop in
// codegen_generate_select_stmt reads it back to copy the already-received
// value into a select-case value binding, without ever calling
// goo_chan_recv a second time.
int codegen_setup_select_case(CodeGenerator* codegen, TypeChecker* checker,
                              LLVMValueRef cases_array, size_t case_index,
                              SelectCaseNode* select_case,
                              LLVMValueRef* out_recv_space) {
    // Get pointer to the case struct in the array. cases_array is a pointer to
    // the first select_case_type element (from LLVMBuildArrayAlloca), so the
    // i-th case is a single-index GEP — NOT a two-index [0, i] which would
    // wrongly select field i of element 0.
    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef select_case_type = codegen_get_select_case_type(codegen);
    LLVMValueRef elem_index = LLVMConstInt(LLVMInt64TypeInContext(ctx), case_index, 0);
    LLVMValueRef case_ptr = LLVMBuildGEP2(codegen->builder, select_case_type, cases_array,
                                          &elem_index, 1, "case_ptr");
    
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

            // Task #9: auto-load an lvalue channel operand — a struct-field
            // or index selector (`case b.ch <- v:`) returns the channel's
            // storage ADDRESS, not the pointer value goo_select expects.
            // Same guard as the plain-send path (codegen_generate_channel_
            // send, lowlevel_codegen.c).
            if (channel_val->is_lvalue && channel_val->goo_type) {
                LLVMTypeRef ct = codegen_type_to_llvm(codegen, channel_val->goo_type);
                if (ct) {
                    channel_val->llvm_value = LLVMBuildLoad2(codegen->builder, ct,
                                                             channel_val->llvm_value, "select_send_chan_load");
                    channel_val->is_lvalue = 0;
                }
            }
            channel = channel_val->llvm_value;

            ValueInfo* value_val = codegen_generate_expression(codegen, checker, binary->right);
            if (!value_val) return 0;

            // Auto-load lvalue send values (a field selector returns the
            // field's ADDRESS) — same as the plain-send path
            // (codegen_generate_channel_send in lowlevel_codegen.c):
            // loading here snapshots the value at evaluation time and
            // avoids memcpy'ing extra bytes from adjacent memory when the
            // field is narrower than the channel element.
            if (value_val->is_lvalue && value_val->goo_type) {
                LLVMTypeRef vt = codegen_type_to_llvm(codegen, value_val->goo_type);
                if (vt) {
                    value_val->llvm_value = LLVMBuildLoad2(codegen->builder, vt,
                                                           value_val->llvm_value, "send_load");
                    value_val->is_lvalue = 0;
                }
            }

            // Get pointer to the value
            if (value_val->is_lvalue) {
                data_ptr = value_val->llvm_value;
            } else {
                // Determine the alloca type from the channel's declared
                // element type — same as the plain-send path
                // (codegen_generate_channel_send in lowlevel_codegen.c).
                // Using LLVMTypeOf(value_val->llvm_value) is wrong: a loaded
                // int32 field (or a literal 42) may arrive as i32 while the
                // channel's element type is i64. goo_select memcpys elem_size
                // bytes from the pointer; if the alloca is narrower, that is
                // a stack-overread (UB). Use the channel's element LLVM type
                // instead and widen/truncate integer values to match.
                Type* chan_goo_s = channel_val->goo_type;
                Type* elem_goo_s = (chan_goo_s && chan_goo_s->kind == TYPE_CHANNEL)
                                   ? chan_goo_s->data.channel.element_type : NULL;
                LLVMTypeRef elem_llvm_s = elem_goo_s
                    ? codegen_type_to_llvm(codegen, elem_goo_s)
                    : LLVMTypeOf(value_val->llvm_value);  // fallback: keep value's own type

                // Coerce the value to match the channel's element width —
                // int<->int (SExt/ZExt/Trunc), int->float, and float<->float
                // (FPExt/FPTrunc) — via the shared width-coercion helper
                // (codegen_coerce_to_type). Signedness signal (matters for
                // the int<->int arm only): the Goo type of the value being
                // sent; fall back to the channel's element type when none is
                // attached — same as the plain-send path
                // (codegen_generate_channel_send in lowlevel_codegen.c).
                Type* widen_ty = value_val->goo_type ? value_val->goo_type : elem_goo_s;
                int use_sext = widen_ty ? type_is_signed(widen_ty) : 1;
                LLVMValueRef send_value = codegen_coerce_to_type(codegen, value_val->llvm_value,
                                                                  use_sext, elem_llvm_s);

                // Store value temporarily
                LLVMValueRef temp_alloca = LLVMBuildAlloca(codegen->builder,
                                                          elem_llvm_s,
                                                          "temp_send_value");
                LLVMBuildStore(codegen->builder, send_value, temp_alloca);
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

            // Task #9: same auto-load guard as the send arm above (and
            // codegen_generate_channel_recv, lowlevel_codegen.c) — a
            // struct-field or index channel operand (`case v := <-b.ch:`)
            // is an lvalue (the field's address), not the channel pointer.
            if (channel_val->is_lvalue && channel_val->goo_type) {
                LLVMTypeRef ct = codegen_type_to_llvm(codegen, channel_val->goo_type);
                if (ct) {
                    channel_val->llvm_value = LLVMBuildLoad2(codegen->builder, ct,
                                                             channel_val->llvm_value, "select_recv_chan_load");
                    channel_val->is_lvalue = 0;
                }
            }
            channel = channel_val->llvm_value;

            // Allocate space for the received value sized to the channel's
            // element type (not a hardcoded i32, which truncates int64/structs).
            LLVMTypeRef recv_ty = LLVMInt32TypeInContext(ctx);  // fallback
            Type* chan_goo = channel_val->goo_type;
            if (chan_goo && chan_goo->kind == TYPE_CHANNEL &&
                chan_goo->data.channel.element_type) {
                LLVMTypeRef et = codegen_type_to_llvm(codegen, chan_goo->data.channel.element_type);
                if (et) recv_ty = et;
            }
            data_ptr = LLVMBuildAlloca(codegen->builder, recv_ty, "recv_space");
            if (out_recv_space) *out_recv_space = data_ptr;

            value_info_free(channel_val);
        }
    }
    
    if (!channel || !data_ptr) {
        codegen_error(codegen, select_case->comm->pos, "Invalid channel operation in select case");
        return 0;
    }
    
    // Cast pointers to void*
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    channel = LLVMBuildBitCast(codegen->builder, channel, void_ptr_type, "channel_void_ptr");
    data_ptr = LLVMBuildBitCast(codegen->builder, data_ptr, void_ptr_type, "data_void_ptr");
    
    // Set the case fields
    // case_ptr->channel = channel
    LLVMValueRef channel_field_ptr = LLVMBuildStructGEP2(codegen->builder, select_case_type, case_ptr, 0, "channel_field");
    LLVMBuildStore(codegen->builder, channel, channel_field_ptr);
    
    // case_ptr->data = data_ptr
    LLVMValueRef data_field_ptr = LLVMBuildStructGEP2(codegen->builder, select_case_type, case_ptr, 1, "data_field");
    LLVMBuildStore(codegen->builder, data_ptr, data_field_ptr);
    
    // case_ptr->is_send = is_send. The struct field is i32 (matching C `int`),
    // so the constant must be i32 — storing an i1 here would write only one byte
    // and leave the upper three bytes as uninitialized garbage, which the
    // runtime would read as a nonzero (and wrong) is_send.
    LLVMValueRef is_send_field_ptr = LLVMBuildStructGEP2(codegen->builder, select_case_type, case_ptr, 2, "is_send_field");
    LLVMValueRef is_send_val = LLVMConstInt(LLVMInt32TypeInContext(ctx), is_send, 0);
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

// Arena region statement generation
//
// Arena-regions Task 6: allocate a real arena on entry, push it so 7c's
// gate routes the body's non-escaping allocations into it
// (codegen_arena_eligible / codegen_emit_alloc), and free it on normal
// fall-through exit. See docs/superpowers/specs/2026-07-07-arena-6-
// arena-free-at-block-exit-design.md for the soundness argument.
//
// Free-only-on-fall-through is deliberate: a `return`/`break`/`continue`
// inside the body already emitted a terminator and branched away before
// reaching the free, so on those paths the arena leaks (safe — no worse
// than today's allocate-and-leak baseline) rather than risking a
// free-before-jump or a double free. Nothing reachable after the block can
// still point into this arena: an escaping value (or a value embedded in
// an escaping value, via 7b's field-taint union) is routed to the heap by
// 7c, never the arena — so freeing the whole arena cannot dangle a live
// pointer.
int codegen_generate_arena_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_ARENA_BLOCK) return 0;
    ArenaBlockNode* arena_blk = (ArenaBlockNode*)stmt;

    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMTypeRef size_type = LLVMInt64TypeInContext(ctx);

    // get-or-declare goo_arena_new : i8* (i64) — same LLVMGetNamedFunction-
    // or-LLVMAddFunction pattern codegen_emit_alloc uses for goo_arena_alloc
    // (both are already declared into the module by
    // codegen_declare_runtime_functions, so this normally just looks them
    // up; the fallback keeps this function safe to call standalone, e.g.
    // from a lightweight test harness that skips that declaration pass).
    LLVMTypeRef new_fn_ty = LLVMFunctionType(ptr_type, &size_type, 1, 0);
    LLVMValueRef new_fn = LLVMGetNamedFunction(codegen->module, "goo_arena_new");
    if (!new_fn) new_fn = LLVMAddFunction(codegen->module, "goo_arena_new", new_fn_ty);

    LLVMTypeRef free_fn_ty = LLVMFunctionType(LLVMVoidTypeInContext(ctx), &ptr_type, 1, 0);
    LLVMValueRef free_fn = LLVMGetNamedFunction(codegen->module, "goo_arena_free");
    if (!free_fn) free_fn = LLVMAddFunction(codegen->module, "goo_arena_free", free_fn_ty);

    // 0 lets the runtime pick GOO_ARENA_DEFAULT_BLOCK_SIZE (arena.c).
    LLVMValueRef zero_size = LLVMConstInt(size_type, 0, 0);
    LLVMValueRef arena = LLVMBuildCall2(codegen->builder, new_fn_ty, new_fn, &zero_size, 1, "arena_new");

    codegen_arena_push(codegen, arena);
    int ok = codegen_generate_statement(codegen, checker, arena_blk->body);
    codegen_arena_pop(codegen);
    if (!ok) return 0;

    // Free ONLY on normal fall-through: any return/break/continue inside
    // the body already emitted a terminator and jumped away, so this point
    // is reachable iff control fell off the end of the body.
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
        LLVMBuildCall2(codegen->builder, free_fn_ty, free_fn, &arena, 1, "");
    }
    return 1;
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