# Correctness Follow-Ups Implementation Plan (arc 3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Close the non-grammar items from PR #193's follow-up backlog: chan-send silent truncation (the last known untyped-constant sink with the hole), the struct-with-slice-field boxing crash (a live verifier leak), the untyped-int→float false-rejects, and duplicate-case detection.

**Ground truth:** all four were empirically probed by the arc-2 fable review (verdicts + reproducer shapes in `.superpowers/sdd/progress.md` ARC 2 section and PR #193's body). No reproduction scout needed — each task's Step 1 re-confirms its shape at THIS branch's HEAD before fixing.

## Global Constraints

- C23. No naked returns. Struct-tail fields. Positioned `type_error` diagnostics; no LLVM verifier text may reach users (Task 2's current failure mode is exactly that).
- **NO parser.y/lexer changes.** The grammar-gap items (3-value `:=`, qualified composite literals, empty tagged switch) stay OUT of this arc. Tripwire **31 S/R + 0 R/R** exact, every task.
- Gates before EVERY commit — **including `make verify-core` end-to-end** (arc-2 lesson: task-level suites missed a verify probe pinning old semantics; never again): `make test` (76/1-skip), `make test-golden` (427/0 at start, grows), `make test-golden-o2`, `make test-golden-reject` (89/0, grows), `make comptime-value-reject-matrix` (19/19), both IR pins, tripwire exact, `make verify-core` ALL GREEN. If a verify probe pins behavior a task legitimately changes, updating it is part of that task WITH justification in the commit message (return-mismatch-probe precedent).
- The representability machinery is SETTLED: `int_const_fits_expected` + `is_untyped_int_const_expr` + `is_negated_int_const_expr` + `is_bare_int_literal` (src/types/type_checker.c ~3247-3330) and its documented deviation table. Tasks REUSE it; any change to the helpers themselves is stop-and-report, not a local edit.
- Shared-checkout caution (concurrent safety-scan session): verify branch `fix/correctness-followups` at task start and pre-commit; foreign untracked files and `.handoff.md` are not yours. NOTE: main now contains PR #194's xmalloc checked allocators — new allocations in src/ should follow that convention where the neighboring code does.
- Commits: conventional, `git -c commit.gpgsign=false commit`.

## File structure (expected)

- `src/types/channel_checker.c` (~:101 send path) — Task 1
- `src/codegen/interface_codegen.c` / boxing path + wherever struct-key-eq lowers — Task 2 (diagnosis decides)
- `src/types/type_checker.c` return gate + switch tag path; possibly `expression_checker.c` adapt machinery — Task 3
- `src/types/type_checker.c` `type_check_switch_stmt` — Task 4
- `tests/golden/reject/`, `examples/` fixtures per task

---

### Task 1: chan-send representability (the silent-truncation hole)

**Probed reality:** `ch <- 300` into `chan int8` compiles and the receiver prints 44. `type_check_channel_send` (src/types/channel_checker.c:101) has no representability check. Twice-precedented fix shape (return gate, switch cases).

- [ ] **Step 1: RED.** Re-confirm at HEAD: buffered `chan int8`, send `300`, receive+print → truncated value, exit 0. Also probe the FITTING direction (send `100` → must keep working) and a negative into `chan uint8` (should it reject today? record).
- [ ] **Step 2: Fix in `type_check_channel_send`:** an untyped int constant sent into an integer-element channel unifies iff representable — call the settled helpers exactly as `type_check_switch_stmt` does (including the `negated`/bare-literal threading and, if the element can be uint64, the conjunction rule). Stamp the constant's type so codegen emits the element width (the switch fix's `stamp_int_const_expr_type` pattern — check whether that static needs promoting to a shared internal header or the send path lives in the same TU reach; do NOT copy-paste the function).
- [ ] **Step 3: Fixtures.** Reject pairs: `chan_send_overflow_reject.goo|.err.txt` (300 → chan int8, substring `overflows`), `chan_send_negative_unsigned_reject.goo|.err.txt` (-1 → chan uint8). Accept golden `examples/chan_send_width_probe.goo` + expected: fitting constants at several widths (boundaries incl. 127/-128/255), sent and received back, values printed — plus a `chan uint64` send of MaxUint64 (conjunction reachability). Reject 89→91, golden 427→428.
- [ ] **Step 4:** full gates incl. verify-core; commit `fix(types): chan-send unifies untyped-constant width with the element type (representability-gated)`.

### Task 2: struct-with-slice-field boxing crash (live verifier leak)

**Probed reality:** boxing a struct containing a `[]T` field into `any` crashes with LLVM verifier output (struct-key-eq path). Diagnosis-first task — the arc-2 Task 3 report (arc2-task-3-report.md, shape matrix) has the reproducer.

- [ ] **Step 1: RED + root-cause.** Minimal reproducer; then trace where the illegal IR is built (the "struct-key-eq" name suggests a struct equality/hash path invoked during boxing — find the actual site, name file:line BEFORE fixing). Establish the trigger matrix: box-only (no print)? print? map-key use? struct==struct comparison with slice fields (Go REJECTS slice-containing structs as comparable — is the right fix a CHECKER rejection per Go's comparability rules rather than a codegen repair?).
- [ ] **Step 2: Fix at the root.** If the crash comes from synthesizing equality for a non-comparable type, the Go-correct fix is a positioned checker rejection where comparability is required, plus making BOXING itself not require equality synthesis (boxing a non-comparable struct is legal in Go; only `==` on the interface then panics at runtime — implement the v1-honest subset: allow boxing, reject static `==` where the checker can see non-comparability, document the dynamic case). Scope decision recorded in the report; if the diagnosis points somewhere genuinely different, follow the evidence and say so.
- [ ] **Step 3: Fixtures.** Accept golden: box + Println a slice-field struct (works via the documented fmt fallback or the fields path — whichever Task-3-of-arc-2's scope produces; assert actual behavior). Reject pair if Step 2 adds a comparability wall. Runtime-panic shape documented in the fixture header if applicable.
- [ ] **Step 4:** full gates incl. verify-core; commit `fix(codegen|types): <per diagnosis> — slice-field structs box without verifier leak`.

### Task 3: untyped-int → float targets (false rejects)

**Probed reality:** `func f() float64 { return 1 }` false-rejects ("cannot return int64 from a function returning float64") — Go-legal. Float-tag switch with const case cleanly rejects (was a crash pre-arc-2; also Go-legal). One adapter should fix both.

- [ ] **Step 1: RED.** Both shapes at HEAD; also `float32`, and a float LITERAL into a float32 return (does 3.9 → float32 work today? record), and the overflow-ish direction: what does Go do with `return 1e400`? (Rejects: constant overflows float64 — check whether the folder can even represent it; if untyped float folding doesn't exist, scope to INT-constants-into-float only and say so.)
- [ ] **Step 2: Implement** the untyped-int→float adaptation at the two sites (return gate's `int_const_coerce` currently requires an integer target — extend to float targets with exact-representability semantics: any int constant a float64 can hold exactly... Go's rule is representability in the float type, which for int64 magnitudes ≤ 2^53 is exact for float64; DECIDE and document the v1 rule — recommended: accept all int constants for float64 (Go does — rounding is defined), accept for float32 likewise; no overflow case exists for int64-range constants). Route the switch float-tag case through the same adaptation.
- [ ] **Step 3: Fixtures.** Accept golden `examples/float_target_const_probe.goo`: int constants returned from float64/float32 functions and used as float-tag switch cases, values printed; expected hand-verified (mind float formatting determinism — use values with exact decimal prints like 1, 2.5). Reject fixture only if a wall is deliberately kept (document which).
- [ ] **Step 4:** full gates incl. verify-core; commit `fix(types): untyped int constants adapt to float targets (return + switch tag)`.

### Task 4: duplicate constant case detection

**Probed reality:** two cases folding to the same value compile (first-match-wins). Go rejects duplicates.

- [ ] **Step 1: RED.** Duplicate int cases; duplicate after folding (`case 1:` + `case - -1:`... note `- -1` folds to 1 — include a folded-collision shape); duplicate string cases on a string tag (does Go reject those too? yes — check what this checker can compare; if string case dedup needs value comparison the checker lacks, scope to integer-constant duplicates and document).
- [ ] **Step 2: Implement in `type_check_switch_stmt`:** collect folded values of constant cases per switch; duplicate → positioned `type_error` at the SECOND occurrence (`duplicate case value %lld (previous case at %s:%d:%d)` — house style with the cross-reference position, like the move diagnostics).
- [ ] **Step 3: Fixtures.** Reject pair `switch_dup_case_reject.goo|.err.txt` (substring `duplicate case`); folded-collision shape included. Existing switch goldens must be duplicate-free (audit — if one has duplicates, that's a wrong fixture to fix with justification).
- [ ] **Step 4:** full gates incl. verify-core; commit `fix(types): duplicate constant case values rejected (folded-value comparison)`.

---

## Self-review
- Backlog items 1-4 (non-grammar) → Tasks 1-4 in priority order; chan-send leads per user direction. Grammar gaps + cosmetic diagnostic remain recorded, out of scope.
- Every task reuses the settled representability machinery rather than forking; Task 2 is diagnosis-first with an explicit Go-comparability re-aim hypothesis; Task 3 records a deliberate v1 float rule.
- verify-core in every task's gates (the arc-2 lesson, encoded).
- Final review: fresh-context whole-branch (fable) — probe cross-position again (the send fix is the LAST known sink; the final review should try to falsify that claim: range bounds? array index? map key? const decl?).
