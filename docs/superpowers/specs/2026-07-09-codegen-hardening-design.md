# Codegen hardening — ControlFlowContext, scoped symbol table, constructor lockdown

**Date:** 2026-07-09
**Branch:** `refactor/codegen-hardening` (off main @ 05841d6, post PR #166)
**User decisions (2026-07-09):** R2 = API **plus** the block-scope semantic fix (separate commits); TypeChecker mirror **included**; timing = now, before P2.1.
**Motivation:** the sub-B review's F2 (closure clobbered goto/loop-label state) and F3 (select binding leaked past case scope) majors, plus the pre-existing block-scope shadowing miscompile, are all instances of two structural defects: hand-enumerated save/restore of parallel arrays, and caller-side scope discipline over a flat value table. Fix the classes, not more instances, before Phase 2/3 add state to exactly these structs. (Ousterhout: information leakage → change amplification → unknown unknowns; define errors out of existence.)

## Acceptance regime (two bars)

- **Mechanical commits (R3, R1, R1-TC, R2a):** byte-identical IR on the 12-fixture pinned net (regenerate baselines fresh on this branch first — the parser-refactor net predates PR #166's merge commit; same fixture list, same recipe), tripwire 121/256 exact, golden 350/0, reject 23/0, `make test` green.
- **Behavior-changing commit (R2b only):** changes the output of currently-miscompiled programs, so the bar is `go run`-verified goldens plus zero regressions elsewhere; the IR net is expected to differ ONLY for fixtures exercising block-scoped redeclaration (list every diffed fixture in the commit body with the before/after semantics).

## R3 — constructor lockdown (first: smallest, unblocks nothing, protects everything)

Delete `ast_node_copy` (src/ast/ast.c — under-allocates derived structs; sole caller is dead macro code `src/advanced_macro_system.c:498`, which is linked but never invoked; delete or stub that call site with the file's existing conventions). Make bare `ast_node_new` private to ast.c (`static`, or rename with an `ast_internal_` prefix if TUs share it legitimately) so all construction flows through typed constructors in `ast_constructors.c`. Compile errors from the privatization ARE the audit — fix each by switching to (or adding) a typed constructor.

## R1 — ControlFlowContext (codegen)

New `include/codegen_cfctx.h` + `src/codegen/cfctx.c`: a single struct owning what are now 20+ parallel-array fields on `CodeGenerator` — `loop_break_bb/loop_continue_bb/loop_label/loop_is_loop/loop_depth`, `goto_label_names/blocks/count`, `pending_label`, and the fallthrough-target stack from P1.7. API: `cfctx_push_loop/push_break_scope/pop`, `cfctx_find_label/find_loop_label`, `cfctx_get_or_create_goto_block`, `cfctx_reset(per-function)`, and the two that make F2-class bugs unrepresentable: `cfctx_save(&saved)` / `cfctx_restore(&saved)` — one struct assignment each, used by `codegen_generate_func_lit`. `CodeGenerator` keeps ONE `ControlFlowContext cfctx;` member (tail-appended; the old fields are REMOVED in the same commit — a transitional alias period would defeat the point). All ~15 touch sites in statement_codegen.c/codegen.c/function_codegen.c mechanically rerouted.

*Alternative rejected:* keeping fields flat and adding only save_all/restore_all helpers — stops the F2 class but leaves 47-site information leakage and the push-site label handoff spread across files.

## R1-TC — TypeChecker mirror

Same treatment for the checker's per-function scratch families in `include/types.h`: `active_type_params`, `literal_stack`, the T1 label registry, the T2 goto-label registry (+ arena-chain snapshots from the F1 fix). One `TcFunctionContext` struct with reset/save/restore, used at function AND func-literal boundaries (the checker already resets these correctly — this commit is pure structure, byte-identical bar applies).

## R2a — scoped symbol table API (mechanical half)

New `src/codegen/value_scope.c`: `vscope_enter(cg)` / `vscope_exit(cg)` wrapping the existing high-water-mark truncation idiom, plus `vscope_add(...)` replacing raw `codegen_add_value` at all ~47 call sites. Every existing snapshot/truncate site (function start, match arms, select cases from the F3 fix) reroutes through the API. NO new truncation points in this commit — plain blocks still leak, byte-identical bar applies. The commit's value is that scope lifetime now has exactly one owner.

## R2b — block scope semantic fix (the payoff)

`codegen_generate_block_stmt` gains `vscope_enter/exit` — plain `{ }` blocks now tear down bindings, fixing the pre-existing block-scope shadowing miscompile ([[goolang-block-scope-shadowing-bug]], M6 candidate). RED first: golden `block_scope_shadow_probe.goo` reproducing the known miscompile shape (inner redeclaration leaking past scope), expected output from `go run`. Sweep existing fixtures for any that accidentally DEPEND on the leak (they'd fail after the fix — each such fixture is itself a latent-bug report; fix the fixture and note it). Type-checker side already scopes correctly (verify; if it also leaks, that's a second RED case, same commit).

## Risks

1. R1 removes fields wholesale — any missed touch site is a compile error (good), but a site that cached a raw array pointer would compile and misbehave: grep for `loop_break_bb`/`goto_label_` address-taking before starting.
2. R2b may surface fixtures relying on leak semantics — budget for fixture triage, don't force-green them.
3. Fixed-size limits stay as-is (32/64 + clean errors) — capacity changes are out of scope.

## Review regime

R1/R2a/R3/R1-TC are differential-proven → single Opus diff reviewer (evaluation-order/linkage/completeness of field migration). R2b changes behavior → one Fable dimension (miscompile hunting on scope shapes: nested blocks, if/for bodies, closures × redeclaration, arena blocks × shadowing) + Opus verifier on findings. No full panel.
