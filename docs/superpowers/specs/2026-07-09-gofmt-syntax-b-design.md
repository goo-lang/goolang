# gofmt-syntax completeness — sub-project B (Phase 1, control-flow half)

**Date:** 2026-07-09
**Branch:** `feat/go-syntax-grammar-b` (off main @ e81f17a, post PR #164)
**Roadmap tasks:** P1.5 (labels + labeled break/continue), P1.6 (goto), P1.7 (fallthrough), P1.10 (select value binding), plus the Phase 1 exit-gate corpus probe (docs/2026-07-08-v1-roadmap.md).
**Discipline:** goo-grammar skill throughout — tripwire baseline **84 S/R + 256 R/R exact** (the script is authoritative; the roadmap doc's 82 is stale), any delta ledger-justified same-commit; golden count only ever grows; expected outputs produced by `go run`, never hand-written.

## Why this grouping

Sub-project A was grammar-local; every task here needs new **codegen control-flow machinery** (branch targets, backpatching, case-body plumbing). T1→T2 share the label registry; T3 and T4 are independent. All tasks touch `parser.y`, so implementation is sequential on one branch.

## Current state (verified by recon, exact anchors)

- `GOTO`/`FALLTHROUGH`: lexed (token.c:178,183), bridged (lexer_bridge.c:180,186), declared (`parser.y:170-171`), **zero productions** — confirmed.
- break/continue: no label operand (parser.y:1521-1533); payload-less base `ASTNode`s. Codegen targets = fixed-depth stack `loop_break_bb[32]`/`loop_continue_bb[32]`/`loop_depth` on `CodeGenerator` (include/codegen.h:102-106), pushed at 3 loop-lowering sites (statement_codegen.c:1088/1290/1371) and via `codegen_push_break_scope` for switch/select/type-switch (:637/:2530/:826).
- COLON: no `statement`-level production begins `identifier COLON` — label arm is unclaimed. Lexer-bridge COLON special-case only pops M10 IF-frames (match guards, lexer_bridge.c:338-347); SELECT/SWITCH/FOR frames pop on `{` only, so `L: for {` should be bridge-safe — must be verified by probe, the bridge is stateful.
- switch lowering: block-per-case, `body_blocks[]` is a **local** in `codegen_generate_switch_stmt` (statement_codegen.c:565-661); each body ends `br merge` when unterminated (:651-653) — the fallthrough hook point.
- select: `select_case: CASE expression COLON ...` only (parser.y:1581-1585); received values land in an anonymous `recv_space` alloca (:2727), never name-bound. Comma-ok receive is a separate, working construct (function_codegen.c:1915-1971) — the pattern to crib for binding.
- No label/goto machinery exists anywhere in the type checker; per-function fixed-array registries have precedent (`active_type_params[32]`, `literal_stack`) — tail-append convention.

## Design

### T1 — P1.5: labeled statements + `break label` / `continue label`

**Grammar.** New arm on `statement:`: `identifier COLON statement` → `AST_LABEL_STMT`. New arms `BREAK identifier` / `CONTINUE identifier`. The label arm widens `identifier`'s FOLLOW set at statement start with COLON — expect possible S/R conflicts against… nothing known (no competing COLON at statement level), but counterexample-analyze any delta; zero-delta formulations preferred.

**AST (tail-append, new constructors, never `ast_node_copy`).** `AST_LABEL_STMT` (`LabelStmtNode{base, char* name, ASTNode* stmt}`), `AST_BREAK_LABEL_STMT` / `AST_CONTINUE_LABEL_STMT` (`{base, char* label}`). *Alternative rejected:* adding a label field to the existing `AST_BREAK_STMT` — its alloc sites build a bare `ASTNode`, so casting old nodes to a derived struct is an OOB read (exactly the ast_node_copy failure class). Separate node types cost two enum entries and keep every existing path untouched. `ast_node_free` cases required for all three.

**Type check.** Per-function flat label registry on `TypeChecker` (fixed array `{name, pos}[64]` + count, tail-appended; reset at function entry — labels are function-scoped, not lexical). Checks: duplicate label → error. Unused labels: Go errors on unused labels; **v1 divergence: allow unused** (documented below) to keep T1 small; revisit with a vet-style pass.

**Codegen.** Parallel array `const char* loop_label[32]` (+ same for break-scopes) tail-appended next to `loop_break_bb`. `AST_LABEL_STMT` whose child is for/switch/select/type-switch sets a `pending_label` field consumed by the next push; labeled break/continue walk the stack top-down for the matching frame (`continue` only matches loop frames). No match → positioned error "label L not defined or not enclosing". A label on a non-loop statement is legal Go (it's a goto target) — codegen emits the child statement normally in T1; the goto plumbing arrives in T2.

**Acceptance (roadmap P1.5).** Nested-loop probe with `outer:` + `continue outer` + `break outer`, output verified by `go run`; reject fixtures: duplicate label; `break x` where `x` is not an enclosing label.

### T2 — P1.6: `goto label`

**Grammar.** `GOTO identifier` → `AST_GOTO_STMT{base, char* label}`.

**Type check.** Registry from T1 gains a use-list: collect labels in a pre-pass over the function body (they're visible function-wide, forward refs legal), then `goto x` with no such label → positioned "undefined label" error (roadmap acceptance: clean error, no crash).

**Codegen.** Per-function `label → LLVMBasicBlockRef` table on `CodeGenerator` (fixed array, reset in `codegen_enter_function` like `value_table_function_start`). Block created lazily at first mention (goto or label); `AST_LABEL_STMT` positions the block at the current point (br-into from fallthrough path, then continue emitting inside it); `AST_GOTO_STMT` emits `br` and terminates the block (dead code after goto: the existing terminated-block skip from the M4 wave, statement_codegen.c `codegen_generate_block_stmt`, already handles this — verify by probe).

**v1 scope cut (documented divergence):** Go's "goto must not jump over variable declarations / into a block" checks are NOT enforced in v1. Jumping into a block over declarations yields whatever the IR does (allocas are function-entry, so no UB at the LLVM level, but shadowing semantics may surprise). Recorded in the divergences section; a reject-side pass is P2-adjacent follow-up work.

**Acceptance.** Backward goto loop prints 0,1,2 (go-run-verified); forward goto skips a statement; undefined label → clean error (reject fixture).

### T3 — P1.7: `fallthrough`

**Grammar.** `FALLTHROUGH` as a plain statement → `AST_FALLTHROUGH_STMT` (bare base node). All placement rules are semantic checks (better diagnostics than parse errors).

**Type check.** Legal only as the **final statement** of a case body in an **expression switch**, and not in the last clause; illegal in type switches and select (Go spec). Each violation gets its own positioned diagnostic + reject fixture.

**Codegen.** Fixed-depth fallthrough-target stack on `CodeGenerator` (tail-append), pushed per case body during the emission loop (:644-654) with `body_blocks[i+1]` (default clause participates in source order per Go). `AST_FALLTHROUGH_STMT` → `br` to top of that stack. *Alternative rejected:* pre-scanning the body's last statement and special-casing before dispatch — breaks when the body ends in a nested block and duplicates dispatcher logic; the stack mirrors the proven break-scope pattern.

**Acceptance (roadmap).** `switch 1 { case 1: println(1); fallthrough; case 2: println(2) }` prints 1 then 2; fallthrough-in-last-case and fallthrough-outside-switch are clean compile errors.

### T4 — P1.10: select value-binding cases

**Grammar.** `select_case` gains: `CASE identifier DECLARE_ASSIGN expression COLON ...` and `CASE identifier ASSIGN expression COLON ...` (expression must be a receive — validated semantically, where the diagnostic can say "select case must be a receive", rather than encoding `ARROW expression` syntactically; choose whichever formulation is zero-delta, semantic validation preferred).

**AST.** Tail-append fields to `SelectCaseNode`: `char* bind_name; int is_declare;` — audit every `SelectCaseNode` alloc site to initialize (malloc-garbage-flag hazard per skill).

**Type check.** Bound name gets the channel's **element type**, scoped to the case body (existing scope push around case bodies; verify one exists, add if not). `=` form requires an existing assignable variable of the element type.

**Codegen.** In the dispatched case block, copy the case's `recv_space` into a named alloca registered in the value table (`:=`) or store into the existing lvalue (`=`) before emitting the body — sibling of the comma-ok receive lowering (function_codegen.c:1915-1971), but reading from the select's already-received buffer; **exactly one receive happens, in goo_select** (no second recv — the P0.1 lesson).

**v1 scope cut:** `case v, ok := <-ch:` is rejected with a positioned "v, ok select binding requires close(); not supported in v1" diagnostic + reject fixture — `ok` is meaningless until close() lands (P3.1); wiring it now would hardcode `true`.

**Acceptance (roadmap).** Two-channel select where each case binds and prints its received value (go-run-verified, deterministic via buffered channels/sequencing); `=` form probe; `v, ok` reject fixture.

### T5 — Phase 1 exit gate: gofmt corpus probe

One golden fixture `examples/gofmt_corpus_probe.goo` composing EVERY Phase 1 construct: grouped var, switch-init, labeled break+continue, goto loop, fallthrough chain, raw strings, multi-line call with trailing comma, void-method-first multi-method interface (dispatched through the interface), select with value binding, line-starting `<-ch` after a block. Expected output from `go run` on the equivalent Go program (inlined in the header comment per fixture convention). Plus a docs truth pass: divergences from this sub-project (unused labels allowed, goto scope-jumps unchecked, no `v, ok` select binding) recorded in the design doc and, where user-visible, in the conflict-ledger notes.

## Risks

1. **Label grammar FOLLOW-set widening** — the one real conflict risk; mitigations: counterexample analysis, ledger, behavioral probes for `L: for {`, `L: switch {`, label-then-composite-literal shapes (M10 bridge is stateful — probe it).
2. **Fallthrough into default / clause ordering** — Go falls through in **source order** including default; probe that shape explicitly.
3. **Select binding × M10 SELECT frame** — `case v := <-ch:` puts `:=` inside a select frame; verify the bridge doesn't misclassify the COLON (it only pops IF frames, but probe it).
4. **Stack depth** — label/loop arrays stay at 32 with the existing "nesting too deep" error path; goto label table 64 with a clean error on overflow.

## Documented v1 divergences introduced here

- Unused labels are not an error (Go: error).
- goto jump-over-declaration / jump-into-block restrictions unchecked.
- `v, ok :=` select binding rejected until close() (P3.1).

### arena-goto fix (2026-07-09): a targeted exception to the jump-into-block cut

The "goto jump-into-block restrictions unchecked" line above was written on
the (incorrect, for `arena{}`) assumption that jumping into a block is at
worst a shadowing surprise — "allocas are function-entry, so no UB at the
LLVM level" — which holds for plain blocks but not for `arena{}` blocks,
which carry their own runtime lifecycle (`goo_arena_new` on entry,
`goo_arena_free` on every exit path). A backward `goto` from outside an
`arena{}` block to a label inside it re-enters the body without re-running
`goo_arena_new`, then falls through the block's end a second time and frees
the same arena twice — a use-after-free store followed by a double free,
SIGSEGV at runtime, invisible at compile time. Two changes close this off,
scoped narrowly to arenas so the general (non-arena) jump-into-block
divergence above is otherwise untouched:

- **goto out of one or more `arena{}` blocks** (legal in Go, and now
  correctly accepted) frees every arena the goto exits before branching —
  `AST_GOTO_STMT` (statement_codegen.c) now calls a new
  `codegen_emit_arena_frees_to_depth`, mirroring how `break`/`continue`/
  `break L`/`continue L` already free arenas above their target frame, but
  keyed on the target label's ARENA-nesting depth (there is no loop
  relationship for goto to key off) rather than loop depth.
- **goto backward INTO an `arena{}` block from outside** is now a type-check
  error ("goto into arena block is not supported"): `type_checker.c` tracks
  each label's enclosing-arena identity chain (`TypeChecker.arena_chain`,
  types.h) alongside the existing goto-label forward-reference table, and
  `AST_GOTO_STMT`'s check rejects any goto whose own current arena chain
  does not have the target label's chain as a prefix — i.e. a goto may only
  ever *exit* arenas it is already inside, never enter one it isn't.

Golden: `examples/arena_goto_probe.goo` (goto exiting one arena inside a
loop, and a goto exiting two nested arenas in one jump — values correct,
exit 0; also wired into `arena-valgrind-probe`'s UAF/double-free gate,
Makefile, as a regression fence for the double-free shape specifically).
Reject: `tests/golden/reject/goto_into_arena.goo` (the exact double-free
shape from before this fix, now rejected pre-codegen).

**Known follow-up, discovered but out of scope here:** `block_escape.c`'s
escape-analysis walkers have no `AST_GOTO_STMT`/`AST_LABEL_STMT` case, so
any function containing a goto or label conservatively marks every
allocation site in it as escaping (`mark_all_escapes`'s "genuinely
unhandled statement kind" default) — every arena allocation in such a
function falls back to the heap `goo_alloc` path regardless of this fix.
Confirmed empirically: `examples/arena_goto_probe.goo`'s allocation-heavy
loop shape shows the same peak RSS whether or not it is wrapped in
`arena{}` (~405MB either way, 200k iterations), which is why this fixture
is a values-correctness-only golden test and is not wired into
`arena-rss-probe.sh`'s RSS-delta gate. Closing this gap (teaching
`block_escape.c` to walk `AST_GOTO_STMT`/`AST_LABEL_STMT`) is separate,
unassigned follow-up work.
