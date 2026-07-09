#ifndef CODEGEN_CFCTX_H
#define CODEGEN_CFCTX_H

#include <stddef.h>

// This header is included FROM codegen.h, inside the definition of
// `struct CodeGenerator` — it must therefore be self-contained (no
// `#include "codegen.h"`, which would be circular). The LLVM-availability
// detection below mirrors codegen.h's own block exactly, for the same
// reason codegen.h has one: LLVMBasicBlockRef must resolve to a real type
// when LLVM is present and the struct must still compile (as a harmless
// stub) when it is not.
#ifdef __has_include
#if __has_include(<llvm-c/Core.h>)
#include <llvm-c/Core.h>
#define CFCTX_LLVM_AVAILABLE 1
#else
#define CFCTX_LLVM_AVAILABLE 0
#endif
#else
#define CFCTX_LLVM_AVAILABLE 0
#endif

#if CFCTX_LLVM_AVAILABLE

// Codegen hardening R1: control-flow scratch state for the function
// CURRENTLY being generated — break/continue targets (plain and labeled),
// the per-function goto-label table, and the fallthrough-target stack.
// Previously this was 20+ parallel-array fields directly on CodeGenerator
// (loop_break_bb/loop_continue_bb/loop_label/loop_is_loop/loop_depth,
// goto_label_names/blocks/count, pending_label, fallthrough_target_bb/
// fallthrough_depth); consolidating them here means a nested (not
// sequential) emission — codegen_generate_func_lit — saves/restores the
// WHOLE family with one struct assignment (cfctx_save/cfctx_restore)
// instead of an enumerated memcpy per field, which is what let the F2
// class of bugs (a new field added to this family and forgotten at a
// save/restore site) exist in the first place. See docs/superpowers/specs/
// 2026-07-09-codegen-hardening-design.md, section R1.
typedef struct ControlFlowContext {
    // Loop-context stack for break/continue targets (depth-bounded;
    // nesting deeper than 32 is rejected with a codegen error by
    // cfctx_push_loop/cfctx_push_break_scope).
    LLVMBasicBlockRef loop_break_bb[32];
    LLVMBasicBlockRef loop_continue_bb[32];
    int loop_depth;

    // gofmt-syntax-b Task 1 (P1.5): labeled break/continue. Parallel to
    // loop_break_bb/loop_continue_bb above, indexed in lockstep with
    // loop_depth — loop_label[i] is the label name (or NULL) attached to
    // the i-th pushed frame, loop_is_loop[i] is 1 for a real loop frame
    // (cfctx_push_loop) and 0 for a break-only switch/select/type-switch
    // frame (cfctx_push_break_scope): `continue LABEL` must only match a
    // loop_is_loop==1 frame (Go: continue targets a FOR, never a switch/
    // select), while `break LABEL` matches either kind. pending_label is
    // set by AST_LABEL_STMT (statement_codegen.c) just before dispatching
    // its wrapped statement, and consumed (cleared) by the very next
    // cfctx_push_loop/cfctx_push_break_scope call — so it only ever tags
    // the ONE frame the label directly wraps.
    const char* loop_label[32];
    int loop_is_loop[32];
    const char* pending_label;

    // gofmt-syntax-b Task 2 (P1.6): per-function label -> LLVMBasicBlockRef
    // table for `goto`. Reset per-function (cfctx_reset, called from
    // codegen_enter_function) — UNLIKE loop_label/loop_is_loop above, which
    // self-balance via push/pop within one function's own codegen, a
    // label's block is created once and never popped, so an explicit reset
    // is required or a second function would see the first's blocks.
    const char* goto_label_names[64];
    LLVMBasicBlockRef goto_label_blocks[64];
    size_t goto_label_count;

    // gofmt-syntax-b Task 3 (P1.7): fixed-depth fallthrough-target stack.
    // Self-balancing within codegen_generate_switch_stmt's own body-
    // emission loop (push before each clause body, pop after) — no
    // per-function reset needed, unlike goto_label_count above.
    LLVMBasicBlockRef fallthrough_target_bb[32];
    int fallthrough_depth;
} ControlFlowContext;

// Push a real loop frame (`for`): brk/cont are this loop's break/continue
// target blocks. Consumes (clears) any pending label set by an
// immediately-enclosing AST_LABEL_STMT (cf->pending_label), tagging THIS
// frame with it. Returns 0 (caller emits a codegen error) at the 32-frame
// depth bound; 1 on success.
int cfctx_push_loop(ControlFlowContext* cf, LLVMBasicBlockRef brk, LLVMBasicBlockRef cont);

// Push a break-only frame (switch/select/type-switch clause body): `break`
// targets `brk`; `continue` inherits the enclosing loop's continue target
// (NULL if there is none, making `continue` here a clean "continue outside
// loop"). Same pending-label handoff and depth bound as cfctx_push_loop.
int cfctx_push_break_scope(ControlFlowContext* cf, LLVMBasicBlockRef brk);

// Pop the innermost frame (loop or break-only). No-op at depth 0.
void cfctx_pop(ControlFlowContext* cf);

// `break L` target search: innermost-first walk over frames of EITHER kind
// (loop or break-only). Returns the matching frame's stack index, or -1 if
// no enclosing frame carries label `label`.
int cfctx_find_label(const ControlFlowContext* cf, const char* label);

// `continue L` target search: innermost-first walk restricted to
// loop_is_loop==1 frames — Go only lets `continue` target an enclosing
// FOR, never a switch/select even if that construct carries the label.
// Returns the matching frame's stack index, or -1 if none.
int cfctx_find_loop_label(const ControlFlowContext* cf, const char* label);

// Per-function reset: clears the goto-label table (goto_label_count = 0)
// only. Called from codegen_enter_function at the start of every
// function's codegen — see goto_label_count's doc comment above for why
// this table (unlike the loop/break stack) needs an explicit reset.
void cfctx_reset(ControlFlowContext* cf);

// Whole-struct save/restore — one assignment each, so a nested (not
// sequential) emission via codegen_generate_func_lit saves the enclosing
// function's ENTIRE control-flow state, resets what it must for its own
// (fresh) emission, and restores the saved state afterward, with no
// per-field enumeration to fall out of sync as this struct grows.
void cfctx_save(ControlFlowContext* out, const ControlFlowContext* cf);
void cfctx_restore(ControlFlowContext* cf, const ControlFlowContext* saved);

// cfctx_get_or_create_goto_block (the `goto`-label get-or-create helper)
// needs a positioned LLVM context (module/current_function) beyond what
// ControlFlowContext alone holds, so its prototype lives on CodeGenerator*
// in codegen.h instead of here — declaring it in this header would need
// CodeGenerator's type, and codegen.h embeds ControlFlowContext as one of
// CodeGenerator's own members, so this header must not depend on it.

#else
// Stub for LLVM-unavailable builds — the stub CodeGenerator (codegen.h's
// #else branch) never emits loop/goto/switch code, so it carries no
// ControlFlowContext member and nothing here is ever referenced; this
// exists only so any TU that includes this header unconditionally still
// compiles.
typedef struct ControlFlowContext { int _cfctx_stub; } ControlFlowContext;
#endif // CFCTX_LLVM_AVAILABLE

#endif // CODEGEN_CFCTX_H
