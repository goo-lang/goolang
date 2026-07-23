# gofmt-syntax sub-project B — implementation plan (5 TDD tasks)

**Spec:** docs/superpowers/specs/2026-07-09-gofmt-syntax-b-design.md
**Branch:** feat/go-syntax-grammar-b. Sequential execution (every task touches parser.y).
**Per-task bar:** goo-grammar skill; tripwire 84/256 exact or ledger-justified same-commit; RED fixture first; golden count only grows; expected outputs via `go run`; atomic conventional commit; `make test` green (pre-commit hook).

## Task 1 — Labels + labeled break/continue (P1.5) — DONE (`725e864`)

- Grammar: `statement: identifier COLON statement`; `BREAK identifier`; `CONTINUE identifier`.
- AST tail-append: `AST_LABEL_STMT` (LabelStmtNode), `AST_BREAK_LABEL_STMT`, `AST_CONTINUE_LABEL_STMT` (+ ast_node_free cases, purpose-built constructors).
- Types: per-function label registry (dup-label error). Codegen: `loop_label[]` parallel arrays + pending-label handoff to loop/switch/select push sites; labeled break/continue stack walk.
- Fixtures: `examples/label_break_continue_probe.goo` (nested loops, `continue outer` + `break outer`, go-run-verified); reject: `label_dup`, `break_unknown_label`.
- Header touched (ast.h, codegen.h, types.h) → `make clean` first; audit all new-node malloc sites.
- Tripwire: 84 → **88 S/R** (+4), 256 R/R unchanged — ledger-justified same commit (two independent +2s: `BREAK`/`CONTINUE identifier` proven lexically unreachable via keyword-terminator ASI; `label_stmt`'s +2 is incidental func-result-family growth, not a new ambiguity class).

## Task 2 — goto (P1.6) [depends: T1 registry] — DONE (`60eca1f`)

- Grammar: `GOTO identifier` → `AST_GOTO_STMT`.
- Types: label pre-pass per function; `goto` to unknown label → positioned error.
- Codegen: per-function label→block table, lazy block creation, backpatch-free (blocks created on first mention); label stmt positions block; goto emits br (terminated-block skip already handles dead tails — verify).
- Fixtures: `goto_probe` (backward loop 0,1,2 + forward skip, go-run-verified); reject: `goto_undefined_label`.
- Tripwire: **zero delta** (88 S/R + 256 R/R) — `GOTO identifier`'s mandatory operand has no competing bare reduce to conflict against.

## Task 3 — fallthrough (P1.7) [independent of T2] — DONE (`06dfe92`)

- Grammar: `FALLTHROUGH` as statement → `AST_FALLTHROUGH_STMT`.
- Types: final-statement-of-non-last-case-of-expression-switch checks; distinct diagnostics for: last case, not-last-statement, type switch, select, outside switch.
- Codegen: fallthrough-target stack pushed per case body with `body_blocks[i+1]` (source order incl. default).
- Fixtures: `fallthrough_probe` (chain incl. into-default, go-run-verified); reject: `fallthrough_last_case`, `fallthrough_outside_switch`, `fallthrough_type_switch`.
- Tripwire: **zero delta** (88 S/R + 256 R/R) — FALLTHROUGH was already an unconditional ASI keyword-terminator, so the single-token arm has no following-token FOLLOW-set collision to conflict against.

## Task 4 — select value binding (P1.10) [independent] — DONE (`e0fe2d3`)

- Grammar: `CASE identifier DECLARE_ASSIGN expression COLON` + `ASSIGN` form (receive-ness validated semantically).
- AST: tail-append `bind_name`/`is_declare` to SelectCaseNode; init at every alloc site.
- Types: bind name = element type, case-body scope; `=` form checks assignability. Reject `v, ok :=` with the close()-deferral diagnostic.
- Codegen: copy recv_space → named alloca / existing lvalue in the dispatched block; ONE receive only (P0.1 lesson); existing select fixtures byte-identical.
- Fixtures: `select_bind_probe` (two channels, `:=` binds, deterministic output), `select_bind_assign_probe` (`=` form); reject: `select_bind_commaok`.
- Tripwire: **zero delta** (88 S/R + 256 R/R) — same "mandatory operand, no competing bare reduce" shape as goto (Task 2); a T4 row was added to the ledger's history table in Task 5's docs commit (it had shipped without one, unlike T2/T3's zero-delta rows — a gap caught by Task 5's truth pass).

## Task 5 — Phase 1 exit-gate corpus probe + truth pass — DONE

- `examples/gofmt_corpus_probe.goo`: every Phase 1 construct (spec list), expected output from `go run`, Go source inlined in header. Golden 344 → 345 (+1), byte-identical to `go run` across 5 (Go) and 8 (compiled `bin/goo`) repeated runs.
- Docs: divergences (unused labels, goto scope-jumps, no v-ok binding) verified accurate against shipped T1-T4 code (no edit needed — checker has no unused-label check, no goto scope-jump check, and `select_bind_commaok` confirms the v-ok reject); T4's missing ledger history-table row added (zero-delta, matching T2/T3's precedent of documenting zero-delta changes too); this plan doc's tasks marked done with commit hashes. Roadmap P1.5/6/7/10 line items are out of this task's file scope (only this plan doc, per the dispatch) — left untouched.

## Exit gate (whole sub-project)

corpus probe green in golden; tripwire PASS; golden ≥ 336 + ~8 new, 0 failed; reject suite ≥ 14 + ~7 new; `make verify-core` ALL GREEN; whole-branch final review (fresh-context, adversarial) before PR.
