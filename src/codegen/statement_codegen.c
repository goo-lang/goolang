#include "codegen.h"
#include "comptime.h"
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
// Loop-context stack: break/continue target blocks for the innermost loop.
// Pushed on entry to a for-loop body, popped on exit. break branches to the
// top break block, continue to the top continue block.
static int codegen_push_loop(CodeGenerator* cg, LLVMBasicBlockRef brk, LLVMBasicBlockRef cont) {
    if (cg->loop_depth >= 32) return 0;       // too deep — caller emits codegen_error
    cg->loop_break_bb[cg->loop_depth] = brk;
    cg->loop_continue_bb[cg->loop_depth] = cont;
    cg->loop_depth++;
    return 1;
}
static void codegen_pop_loop(CodeGenerator* cg) { if (cg->loop_depth > 0) cg->loop_depth--; }

// Push a break-only scope for switch/select clause bodies. In Go/Goo a `break`
// inside a switch or select terminates that construct (not the enclosing loop),
// while `continue` is NOT bound by switch/select and must thread through to the
// nearest enclosing loop. We model this by reusing the loop-context stack:
// `break` targets `brk` (the construct's merge/end block) and `continue`
// inherits the enclosing loop's continue target (NULL when there is no
// enclosing loop, which makes `continue` here a clean "continue outside loop").
static int codegen_push_break_scope(CodeGenerator* cg, LLVMBasicBlockRef brk) {
    if (cg->loop_depth >= 32) return 0;       // too deep — caller emits codegen_error
    LLVMBasicBlockRef inherited_continue =
        cg->loop_depth > 0 ? cg->loop_continue_bb[cg->loop_depth - 1] : NULL;
    cg->loop_break_bb[cg->loop_depth] = brk;
    cg->loop_continue_bb[cg->loop_depth] = inherited_continue;
    cg->loop_depth++;
    return 1;
}

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
            ValueInfo* target = codegen_emit_lvalue_address(codegen, checker, t);
            if (!target || !target->is_lvalue) {
                codegen_error(codegen, t->pos,
                              "destructure target must be an addressable lvalue");
                value_info_free(rhs);
                return 0;
            }
            LLVMValueRef field = LLVMBuildExtractValue(codegen->builder,
                                                       rhs->llvm_value, (unsigned)i, "destr");
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
            codegen_add_value(codegen, vi);
            // Mirror the var-decl path: keep the checker scope populated so
            // later codegen re-checks of `nm` resolve (ignore dup failures).
            Variable* tv = variable_new(nm, rtypes[i], t->pos);
            if (tv) {
                tv->is_initialized = 1;
                scope_add_variable(checker->current_scope, tv);
            }
        } else {
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
                codegen_add_value(codegen, vi);
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
        case AST_UNSAFE_STMT:
            return codegen_generate_unsafe_stmt(codegen, checker, stmt);
        case AST_ASM_STMT:
            return codegen_generate_asm_stmt(codegen, checker, stmt);
        case AST_BREAK_STMT:
            if (codegen->loop_depth == 0) { codegen_error(codegen, stmt->pos, "break outside loop"); return 0; }
            LLVMBuildBr(codegen->builder, codegen->loop_break_bb[codegen->loop_depth - 1]);
            // Subsequent statements in this block are unreachable; start a fresh
            // block so later codegen has a valid (dead) insertion point.
            codegen_set_insert_point(codegen, codegen_create_block(codegen, "after.break"));
            return 1;
        case AST_CONTINUE_STMT:
            // A NULL continue target means the innermost scope is a break-only
            // switch/select with no enclosing loop — `continue` is illegal there.
            if (codegen->loop_depth == 0 ||
                codegen->loop_continue_bb[codegen->loop_depth - 1] == NULL) {
                codegen_error(codegen, stmt->pos, "continue outside loop"); return 0;
            }
            LLVMBuildBr(codegen->builder, codegen->loop_continue_bb[codegen->loop_depth - 1]);
            codegen_set_insert_point(codegen, codegen_create_block(codegen, "after.continue"));
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
    size_t pre_block_vt_size = codegen->value_table_size;

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
    codegen->value_table_size = pre_block_vt_size;
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
    if (!codegen_push_break_scope(codegen, merge_block)) {
        codegen_error(codegen, stmt->pos, "switch nested too deeply for break handling");
        free(body_blocks);
        return 0;
    }

    // Emit clause bodies. No implicit fallthrough: each body ends at merge.
    i = 0;
    for (ASTNode* c = sw->cases; c; c = c->next, i++) {
        CaseClauseNode* clause = (CaseClauseNode*)c;
        codegen_set_insert_point(codegen, body_blocks[i]);
        for (ASTNode* s = clause->body; s; s = s->next) {
            if (!codegen_generate_statement(codegen, checker, s)) { codegen_pop_loop(codegen); free(body_blocks); return 0; }
        }
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
            LLVMBuildBr(codegen->builder, merge_block);
        }
    }
    codegen_pop_loop(codegen);

    codegen_set_insert_point(codegen, merge_block);
    free(body_blocks);
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
    // The runtime iterator's key_out is a raw `const char**` (see
    // goo_map_iter_next_sv) — the map is string-keyed in practice (the
    // checker's TYPE_MAP arm binds whatever key type the Type carries, but
    // no non-string-keyed map construction path exists today), so this is
    // the codegen-side half of that same documented limitation rather than
    // a new one.
    if (!key_type || key_type->kind != TYPE_STRING) {
        codegen_error(codegen, stmt->pos,
                      "range over map: only string-keyed maps are supported");
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
    LLVMValueRef iter_fn = LLVMGetNamedFunction(codegen->module, "goo_map_iter_next_sv");
    if (!iter_fn) {
        LLVMTypeRef pp_type = LLVMPointerType(ptr_type, 0);
        LLVMTypeRef iter_params[] = { pp_type, pp_type, LLVMPointerType(i64, 0) };
        LLVMTypeRef iter_fn_type = LLVMFunctionType(i32, iter_params, 3, 0);
        iter_fn = LLVMAddFunction(codegen->module, "goo_map_iter_next_sv", iter_fn_type);
    }
    LLVMValueRef mkstr_fn = LLVMGetNamedFunction(codegen->module, "goo_string_new");
    if (!mkstr_fn) {
        LLVMTypeRef string_type = LLVMStructTypeInContext(ctx,
            (LLVMTypeRef[]){ ptr_type, i64 }, 2, 0);
        LLVMTypeRef mkstr_params[] = { ptr_type };
        LLVMTypeRef mkstr_fn_type = LLVMFunctionType(string_type, mkstr_params, 1, 0);
        mkstr_fn = LLVMAddFunction(codegen->module, "goo_string_new", mkstr_fn_type);
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

    // Out-param scratch for the iterator call, reused every iteration.
    LLVMValueRef key_cstr_alloca = codegen_alloc_local(codegen, ptr_type, "range_mkey_cstr");
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
        codegen_add_value(codegen, kv);
    }
    if (for_stmt->value_name) {
        val_alloca = codegen_alloc_local(codegen, val_llvm, for_stmt->value_name);
        ValueInfo* vv = value_info_new(for_stmt->value_name, val_alloca, value_type);
        vv->is_lvalue = 1;
        vv->is_initialized = 1;
        codegen_add_value(codegen, vv);
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

    // cond: goo_map_iter_next_sv(&cursor, &key_cstr, &val_slot) != 0. The
    // call itself advances the cursor and fills the out-slots — there is no
    // separate "advance" step in the post block (see below).
    codegen_set_insert_point(codegen, rcond);
    LLVMValueRef iter_args[3] = { cursor_alloca, key_cstr_alloca, val_slot_alloca };
    LLVMValueRef has_next = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(iter_fn),
                                           iter_fn, iter_args, 3, "map_has_next");
    LLVMValueRef cond_v = LLVMBuildICmp(codegen->builder, LLVMIntNE, has_next,
                                        LLVMConstInt(i32, 0, 0), "maprange_cond");
    LLVMBuildCondBr(codegen->builder, cond_v, rbody, rexit);

    // body: bind key (wrap the C string via goo_string_new — copies, so the
    // binding stays valid independent of the map entry) and value (cast the
    // int64 slot to the declared V, same convention as m[k] reads), then
    // run the loop body.
    codegen_set_insert_point(codegen, rbody);
    if (key_alloca) {
        LLVMValueRef kc = LLVMBuildLoad2(codegen->builder, ptr_type, key_cstr_alloca, "map_kcstr");
        LLVMValueRef mkstr_args[1] = { kc };
        LLVMValueRef kstr = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(mkstr_fn),
                                           mkstr_fn, mkstr_args, 1, "map_kstr");
        LLVMBuildStore(codegen->builder, kstr, key_alloca);
    }
    if (val_alloca) {
        LLVMValueRef slot = LLVMBuildLoad2(codegen->builder, i64, val_slot_alloca, "map_vslot");
        LLVMValueRef v = codegen_map_slot_to_value(codegen, slot, value_type);
        LLVMBuildStore(codegen->builder, v, val_alloca);
    }

    if (!codegen_push_loop(codegen, rexit, rpost)) {
        codegen_error(codegen, stmt->pos, "loop nesting too deep (max 32)");
        scope_pop(checker);
        return 0;
    }
    int body_ok = 1;
    if (for_stmt->body) {
        body_ok = codegen_generate_statement(codegen, checker, for_stmt->body);
    }
    codegen_pop_loop(codegen);
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
        // Auto-load if lvalue.
        LLVMValueRef raw = range_val->llvm_value;
        if (range_val->is_lvalue && range_val->goo_type) {
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

        // Extract data pointer (field 0) and length (field 1) — both
        // slices and (eventually) strings share this layout.
        LLVMValueRef data_ptr = LLVMBuildExtractValue(codegen->builder, raw, 0, "range_data");
        LLVMValueRef len64 = LLVMBuildExtractValue(codegen->builder, raw, 1, "range_len");

        // F7: range over a string iterates its bytes — the value var is an
        // int32 rune (v1 byte-wise), so the backing i8 byte is zero-extended
        // into it in the body below. Slices/arrays use their element type.
        int is_string_range = range_val->goo_type
                           && range_val->goo_type->kind == TYPE_STRING;
        Type* elem_type = NULL;
        LLVMTypeRef llvm_elem = NULL;
        if (range_val->goo_type && range_val->goo_type->kind == TYPE_SLICE) {
            elem_type = range_val->goo_type->data.slice.element_type;
            llvm_elem = elem_type ? codegen_type_to_llvm(codegen, elem_type) : NULL;
        } else if (is_string_range) {
            elem_type = type_checker_get_builtin(checker, TYPE_INT32);
            llvm_elem = LLVMInt32TypeInContext(codegen->context);
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
            codegen_add_value(codegen, kv);
        }

        // Allocate value var (per-iteration). Mirrored to type-check
        // scope below.
        LLVMValueRef val_alloca = NULL;
        if (for_stmt->value_name && llvm_elem && elem_type) {
            val_alloca = codegen_alloc_local(codegen, llvm_elem, for_stmt->value_name);
            ValueInfo* vv = value_info_new(for_stmt->value_name, val_alloca, elem_type);
            vv->is_lvalue = 1;
            vv->is_initialized = 1;
            codegen_add_value(codegen, vv);
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
        if (!codegen_push_loop(codegen, rexit, rpost)) {
            codegen_error(codegen, stmt->pos, "loop nesting too deep (max 32)");
            scope_pop(checker);
            value_info_free(range_val);
            return 0;
        }
        int body_ok = 1;
        if (for_stmt->body) {
            body_ok = codegen_generate_statement(codegen, checker, for_stmt->body);
        }
        codegen_pop_loop(codegen);
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
    if (!codegen_push_loop(codegen, exit_block, post_block)) {
        codegen_error(codegen, stmt->pos, "loop nesting too deep (max 32)");
        return 0;
    }
    if (for_stmt->body) {
        if (!codegen_generate_statement(codegen, checker, for_stmt->body)) {
            codegen_pop_loop(codegen);
            return 0;
        }
    }
    codegen_pop_loop(codegen);
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

                if (field_type && field_type->kind == TYPE_NULLABLE &&
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


        // Nullable nil-return intercept: `return nil` inside a `?T` function.
        // Without this, codegen_generate_null_literal produces a void* null
        // pointer (no expected-type context), which mismatches the `{i1, T}`
        // nullable return type and fails module verification. Intercept here,
        // generate the correct {is_null=1, zero_value} struct, and return it
        // directly — same pattern as var_decl's `var b ?T = nil` fix.
#if LLVM_AVAILABLE
        if (function_return_type && function_return_type->kind == TYPE_NULLABLE &&
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

        // Integer widening: widen the return value to match the function's
        // declared LLVM return type (e.g. `return 0` as i32 from an i64
        // function). Mirrors the same SExt guard in var_decl.
        {
            LLVMValueRef cur_fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(codegen->builder));
            LLVMTypeRef fn_ret = LLVMGetReturnType(LLVMGlobalGetValueType(cur_fn));
            LLVMTypeRef val_ty = LLVMTypeOf(final_return_value);
            if (LLVMGetTypeKind(val_ty) == LLVMIntegerTypeKind &&
                LLVMGetTypeKind(fn_ret) == LLVMIntegerTypeKind) {
                unsigned from_bits = LLVMGetIntTypeWidth(val_ty);
                unsigned to_bits   = LLVMGetIntTypeWidth(fn_ret);
                if (from_bits < to_bits)
                    final_return_value = LLVMBuildSExt(codegen->builder, final_return_value,
                                                       fn_ret, "ret_sext");
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

    // Resolve the target function value and its type.
    ValueInfo* func_val = codegen_generate_expression(codegen, checker, call->function);
    if (!func_val) return 0;
    LLVMValueRef callee = func_val->llvm_value;

    // M8 restriction: only direct top-level function targets. A closure or bound
    // method carries an environment we don't box yet — reject with a clear error
    // rather than spawn a goroutine that calls garbage.
    if (!callee || !LLVMIsAFunction(callee)) {
        codegen_error(codegen, stmt->pos,
                      "go: only direct function calls are supported "
                      "(closures and methods are not yet supported)");
        value_info_free(func_val);
        return 0;
    }
    LLVMTypeRef callee_ty = LLVMGlobalGetValueType(callee);

    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMTypeRef i64_type = LLVMInt64TypeInContext(ctx);

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

        LLVMTypeRef alloc_ty = LLVMFunctionType(void_ptr_type, &i64_type, 1, 0);
        LLVMValueRef alloc_fn = LLVMGetNamedFunction(codegen->module, "goo_alloc");
        if (!alloc_fn) alloc_fn = LLVMAddFunction(codegen->module, "goo_alloc", alloc_ty);

        LLVMValueRef box_size = LLVMSizeOf(box_struct);  // i64 target-size constant
        boxed = LLVMBuildCall2(codegen->builder, alloc_ty, alloc_fn, &box_size, 1, "go_box");

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

// Emit the current function's deferred calls in LIFO (last-registered-first)
// order. Called at every function-exit path immediately before the `ret`.
// No-op when the function registered no defers, so existing functions are
// unaffected.
//
// Each deferred call's arguments were snapshotted into entry-block allocas at
// the defer site (Go's defer-time arg evaluation), and the call's argument AST
// nodes were rewritten to synthetic identifiers referencing those snapshots.
// Here we (1) re-bind those synthetic names to their snapshot slots in the
// value table (the originating block's locals are long gone), then (2) emit
// each call guarded by its runtime "active" flag, so a defer that was never
// reached at runtime (e.g. inside a not-taken branch) does not run.
void codegen_emit_deferred_calls(CodeGenerator* codegen, TypeChecker* checker) {
#if LLVM_AVAILABLE
    if (!codegen || !checker) return;
    FunctionInfo* fi = codegen->current_function_info;
    if (!fi || fi->deferred_count == 0) return;
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
            codegen_add_value(codegen, vi);
        }

        // Guard the call with the runtime active flag.
        LLVMValueRef live = LLVMBuildLoad2(codegen->builder, i1, info->active_flag,
                                           "defer_live");
        LLVMBasicBlockRef run_bb = codegen_create_block(codegen, "defer_run");
        LLVMBasicBlockRef cont_bb = codegen_create_block(codegen, "defer_cont");
        LLVMBuildCondBr(codegen->builder, live, run_bb, cont_bb);

        LLVMPositionBuilderAtEnd(codegen->builder, run_bb);
        ValueInfo* result = codegen_generate_expression(codegen, checker, call);
        if (result) value_info_free(result);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)))
            LLVMBuildBr(codegen->builder, cont_bb);

        LLVMPositionBuilderAtEnd(codegen->builder, cont_bb);
    }
#else
    (void)codegen;
    (void)checker;
#endif
}

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

    // A defer inside a loop accumulates one deferred call PER ITERATION in Go,
    // which a single static per-defer slot cannot model. Rather than silently
    // miscompiling (running once, with the last iteration's snapshot), reject
    // it cleanly; once-per-iteration defers need a runtime defer stack (tracked
    // follow-up requiring runtime support).
    if (codegen->loop_depth > 0) {
        codegen_error(codegen, stmt->pos,
                      "defer inside a loop is not yet supported "
                      "(needs a runtime defer stack)");
        return 0;
    }

    if (defer_stmt->call->type != AST_CALL_EXPR) {
        codegen_error(codegen, stmt->pos, "defer requires a function call");
        return 0;
    }
    CallExprNode* call = (CallExprNode*)defer_stmt->call;

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
    // entry-block alloca, and rewrite the argument node to a synthetic
    // identifier that resolves to that snapshot when the call is emitted at
    // function exit.
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

    // Snapshot the method receiver (if any) first, rewriting the selector base
    // to a synthetic identifier that resolves to the snapshot at exit.
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
        ((SelectorExprNode*)call->function)->expr = (ASTNode*)rid;
        idx++;
    }

    ASTNode* prev = NULL;
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

        // Splice a synthetic identifier in place of the original argument.
        IdentifierNode* id = ast_identifier_new(nm, a->pos);
        ASTNode* idn = (ASTNode*)id;
        idn->next = nextarg;
        if (prev) prev->next = idn; else call->args = idn;
        prev = idn;

        a = nextarg;
        idx++;
    }

    // Mark this defer active at runtime (current block; runs only if reached).
    LLVMBuildStore(codegen->builder, LLVMConstInt(i1, 1, 0), flag);

    // Register the (rewritten) call node on the FunctionInfo and its codegen
    // info in the parallel cache. Both arrays grow in lock-step from index 0.
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
            if (!codegen_setup_select_case(codegen, checker, cases_array, case_index, select_case)) {
                free(case_blocks);
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
    if (!codegen_push_break_scope(codegen, end_block)) {
        codegen_error(codegen, stmt->pos, "select nested too deeply for break handling");
        free(case_blocks);
        return 0;
    }

    // Generate code for each case block
    case_index = 0;
    case_node = select_stmt->cases;
    while (case_node && case_index < case_count) {
        SelectCaseNode* select_case = (SelectCaseNode*)case_node;

        LLVMPositionBuilderAtEnd(codegen->builder, case_blocks[case_index]);

        // Generate case body. The body is a ->next statement chain — loop it
        // like switch codegen does (a single codegen_generate_statement call
        // silently dropped every statement after the first).
        for (ASTNode* s = select_case->body; s; s = s->next) {
            if (!codegen_generate_statement(codegen, checker, s)) {
                codegen_pop_loop(codegen);
                free(case_blocks);
                return 0;
            }
        }

        // Branch to end, unless the body already terminated (e.g. return/break).
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder))) {
            LLVMBuildBr(codegen->builder, end_block);
        }

        case_node = case_node->next;
        case_index++;
    }
    codegen_pop_loop(codegen);

    // Position builder at end block
    LLVMPositionBuilderAtEnd(codegen->builder, end_block);
    
    free(case_blocks);
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
int codegen_setup_select_case(CodeGenerator* codegen, TypeChecker* checker, 
                              LLVMValueRef cases_array, size_t case_index, 
                              SelectCaseNode* select_case) {
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