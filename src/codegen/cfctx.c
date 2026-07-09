// Codegen hardening R1: ControlFlowContext implementation. Every function
// here is a mechanical extraction of logic that used to live inline in
// statement_codegen.c (cfctx_push_loop/cfctx_push_break_scope/cfctx_pop/
// cfctx_find_label/cfctx_find_loop_label/cfctx_get_or_create_goto_block —
// formerly the file-static codegen_push_loop/codegen_pop_loop/codegen_push_
// break_scope/codegen_get_or_create_label_block plus the inline stack-walk
// loops in the AST_BREAK_LABEL_STMT/AST_CONTINUE_LABEL_STMT arms) or
// function_codegen.c (cfctx_save/cfctx_restore — formerly codegen_generate_
// func_lit's hand-enumerated per-field memcpy save/restore). No behavior
// change: see docs/superpowers/specs/2026-07-09-codegen-hardening-design.md,
// section R1.
#include "codegen.h"
#include "codegen_cfctx.h"
#include <string.h>

#if LLVM_AVAILABLE

int cfctx_push_loop(ControlFlowContext* cf, LLVMBasicBlockRef brk, LLVMBasicBlockRef cont) {
    if (!cf || cf->loop_depth >= 32) return 0;  // too deep — caller emits codegen_error
    cf->loop_break_bb[cf->loop_depth] = brk;
    cf->loop_continue_bb[cf->loop_depth] = cont;
    cf->loop_label[cf->loop_depth] = cf->pending_label;
    cf->loop_is_loop[cf->loop_depth] = 1;
    cf->pending_label = NULL;
    cf->loop_depth++;
    return 1;
}

int cfctx_push_break_scope(ControlFlowContext* cf, LLVMBasicBlockRef brk) {
    if (!cf || cf->loop_depth >= 32) return 0;  // too deep — caller emits codegen_error
    LLVMBasicBlockRef inherited_continue =
        cf->loop_depth > 0 ? cf->loop_continue_bb[cf->loop_depth - 1] : NULL;
    cf->loop_break_bb[cf->loop_depth] = brk;
    cf->loop_continue_bb[cf->loop_depth] = inherited_continue;
    cf->loop_label[cf->loop_depth] = cf->pending_label;
    cf->loop_is_loop[cf->loop_depth] = 0;
    cf->pending_label = NULL;
    cf->loop_depth++;
    return 1;
}

void cfctx_pop(ControlFlowContext* cf) {
    if (cf && cf->loop_depth > 0) cf->loop_depth--;
}

int cfctx_find_label(const ControlFlowContext* cf, const char* label) {
    if (!cf || !label) return -1;
    for (int i = cf->loop_depth - 1; i >= 0; i--) {
        if (cf->loop_label[i] && strcmp(cf->loop_label[i], label) == 0) return i;
    }
    return -1;
}

int cfctx_find_loop_label(const ControlFlowContext* cf, const char* label) {
    if (!cf || !label) return -1;
    for (int i = cf->loop_depth - 1; i >= 0; i--) {
        if (cf->loop_is_loop[i] && cf->loop_label[i] && strcmp(cf->loop_label[i], label) == 0) {
            return i;
        }
    }
    return -1;
}

void cfctx_reset(ControlFlowContext* cf) {
    if (!cf) return;
    cf->goto_label_count = 0;
}

void cfctx_save(ControlFlowContext* out, const ControlFlowContext* cf) {
    if (!out || !cf) return;
    *out = *cf;
}

void cfctx_restore(ControlFlowContext* cf, const ControlFlowContext* saved) {
    if (!cf || !saved) return;
    *cf = *saved;
}

// gofmt-syntax-b Task 2 (P1.6): return the LLVMBasicBlockRef for label
// `name` within the current function, creating it on first mention (from
// either a `goto` or the label's own AST_LABEL_STMT — whichever is
// generated first; backward gotos hit the AST_LABEL_STMT path first,
// forward gotos hit this path first, both converge on the same block).
// NULL on overflow (>64 labels) or if `name`/`cg->current_function` is
// unavailable — callers must treat NULL as a codegen error, mirroring
// cfctx_push_loop's own "too deep" convention. Defensive only in practice:
// the type checker's goto_label_names (types.h) shares this same 64 bound
// and has already rejected any function with more labels before codegen
// runs. Takes CodeGenerator* (not just ControlFlowContext*) because
// codegen_create_block needs the positioned module/current_function that
// only CodeGenerator carries.
LLVMBasicBlockRef cfctx_get_or_create_goto_block(CodeGenerator* cg, const char* name) {
    if (!cg || !name || !cg->current_function) return NULL;
    ControlFlowContext* cf = &cg->cfctx;
    for (size_t i = 0; i < cf->goto_label_count; i++) {
        if (cf->goto_label_names[i] && strcmp(cf->goto_label_names[i], name) == 0) {
            return cf->goto_label_blocks[i];
        }
    }
    if (cf->goto_label_count >= 64) return NULL;
    LLVMBasicBlockRef block = codegen_create_block(cg, name);
    if (!block) return NULL;
    cf->goto_label_names[cf->goto_label_count] = name;
    cf->goto_label_blocks[cf->goto_label_count] = block;
    cf->goto_label_count++;
    return block;
}

#endif // LLVM_AVAILABLE
