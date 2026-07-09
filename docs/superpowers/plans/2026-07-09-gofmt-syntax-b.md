# gofmt-syntax sub-project B — implementation plan (5 TDD tasks)

**Spec:** docs/superpowers/specs/2026-07-09-gofmt-syntax-b-design.md
**Branch:** feat/go-syntax-grammar-b. Sequential execution (every task touches parser.y).
**Per-task bar:** goo-grammar skill; tripwire 84/256 exact or ledger-justified same-commit; RED fixture first; golden count only grows; expected outputs via `go run`; atomic conventional commit; `make test` green (pre-commit hook).

## Task 1 — Labels + labeled break/continue (P1.5)

- Grammar: `statement: identifier COLON statement`; `BREAK identifier`; `CONTINUE identifier`.
- AST tail-append: `AST_LABEL_STMT` (LabelStmtNode), `AST_BREAK_LABEL_STMT`, `AST_CONTINUE_LABEL_STMT` (+ ast_node_free cases, purpose-built constructors).
- Types: per-function label registry (dup-label error). Codegen: `loop_label[]` parallel arrays + pending-label handoff to loop/switch/select push sites; labeled break/continue stack walk.
- Fixtures: `examples/label_break_continue_probe.goo` (nested loops, `continue outer` + `break outer`, go-run-verified); reject: `label_dup`, `break_unknown_label`.
- Header touched (ast.h, codegen.h, types.h) → `make clean` first; audit all new-node malloc sites.

## Task 2 — goto (P1.6) [depends: T1 registry]

- Grammar: `GOTO identifier` → `AST_GOTO_STMT`.
- Types: label pre-pass per function; `goto` to unknown label → positioned error.
- Codegen: per-function label→block table, lazy block creation, backpatch-free (blocks created on first mention); label stmt positions block; goto emits br (terminated-block skip already handles dead tails — verify).
- Fixtures: `goto_probe` (backward loop 0,1,2 + forward skip, go-run-verified); reject: `goto_undefined_label`.

## Task 3 — fallthrough (P1.7) [independent of T2]

- Grammar: `FALLTHROUGH` as statement → `AST_FALLTHROUGH_STMT`.
- Types: final-statement-of-non-last-case-of-expression-switch checks; distinct diagnostics for: last case, not-last-statement, type switch, select, outside switch.
- Codegen: fallthrough-target stack pushed per case body with `body_blocks[i+1]` (source order incl. default).
- Fixtures: `fallthrough_probe` (chain incl. into-default, go-run-verified); reject: `fallthrough_last_case`, `fallthrough_outside_switch`, `fallthrough_type_switch`.

## Task 4 — select value binding (P1.10) [independent]

- Grammar: `CASE identifier DECLARE_ASSIGN expression COLON` + `ASSIGN` form (receive-ness validated semantically).
- AST: tail-append `bind_name`/`is_declare` to SelectCaseNode; init at every alloc site.
- Types: bind name = element type, case-body scope; `=` form checks assignability. Reject `v, ok :=` with the close()-deferral diagnostic.
- Codegen: copy recv_space → named alloca / existing lvalue in the dispatched block; ONE receive only (P0.1 lesson); existing select fixtures byte-identical.
- Fixtures: `select_bind_probe` (two channels, `:=` binds, deterministic output), `select_bind_assign_probe` (`=` form); reject: `select_bind_commaok`.

## Task 5 — Phase 1 exit-gate corpus probe + truth pass

- `examples/gofmt_corpus_probe.goo`: every Phase 1 construct (spec list), expected output from `go run`, Go source inlined in header.
- Docs: divergences (unused labels, goto scope-jumps, no v-ok binding) confirmed present in design doc; ledger history updated if any task carried a delta; roadmap P1.5/6/7/10 marked done in the working notes.

## Exit gate (whole sub-project)

corpus probe green in golden; tripwire PASS; golden ≥ 336 + ~8 new, 0 failed; reject suite ≥ 14 + ~7 new; `make verify-core` ALL GREEN; whole-branch final review (fresh-context, adversarial) before PR.
