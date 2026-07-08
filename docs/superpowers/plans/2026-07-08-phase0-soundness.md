# Phase 0 Soundness & Safety Net Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The compiler and its harness made trustworthy: both silent miscompiles fixed, the zero-diagnostic crash and lexer overflow closed, panics Go-conformant, and a golden harness that sees timeouts, exit codes, stderr, and compile-rejects — capped by a ccomp-free `verify-core` gate.

**Architecture:** Harness first (Tasks 1–2) so every soundness fix (Tasks 3–6) pins its audit reproducer as a native fixture; `verify-core` (Task 7) aggregates last. Inside Task 3, the LLVMVerifyModule gate lands BEFORE the `!?T` type-checker rejection so the SIGILL reproducer proves the gate.

**Tech Stack:** C23, bash (scripts/run_golden.sh), GNU make, LLVM-C API.

**Spec:** `docs/superpowers/specs/2026-07-08-phase0-soundness-design.md` — normative; read first.

## Global Constraints

- C23. Clean diagnostics/failures only — no signals for user-caused errors, no LLVM verifier noise reaching users.
- NO `parser.y` changes anywhere (BLOCKED if you think otherwise). `./scripts/grammar-tripwire.sh` must stay `82 S/R + 256 R/R` exact — run as a guard in every task.
- FROZEN BEHAVIOR outside each task's target. Gates before every commit: `make test-golden` (321/0 at start, grows), `make comptime-value-reject-matrix` (18/18), `make comptime-generic-compose-ir-pin` (PASS), `make test` (76/1-skip), `make spmd-bench-probe` (PASS). Task 7 adds `make verify-core` for its own and later commits.
- Carve-out: Task 6 legitimately changes panic exit codes — probes asserting 134/SIGABRT update IN THE SAME COMMIT.
- A compiler bug beyond the six named fixes is BLOCKED, not a license to fix.
- Commits: conventional prefixes, atomic, `--no-gpg-sign`, imperative mood.

## File structure

- `scripts/run_golden.sh` — Tasks 1 (timeout/exit/stderr) only
- `scripts/run_golden_reject.sh` (new) + `tests/golden/reject/` (new tree) — Task 2
- `src/codegen/codegen.c` (~1592 emit path) — Task 3 (P0.2)
- `src/types/type_checker.c` (or wherever `!?T` return types check) — Task 3 (P0.3)
- `src/lexer/lexer.c` (:552-558 model for TOKEN_UNKNOWN; :316/:329 comment recursion) — Task 4
- `src/codegen/lowlevel_codegen.c` / statement codegen comma-ok path — Task 5
- `src/runtime/*.c` `goo_panic` definition — Task 6
- `Makefile` — Tasks 1, 2, 6 (probe migration/updates), 7 (`verify-core`)
- `examples/*.goo` fixtures + sidecars — Tasks 1–6

---

### Task 1: Golden runner — timeout, exit-code, and stderr conventions (P0.7 + P0.8)

**Files:** `scripts/run_golden.sh`; Makefile (delete `divzero-probe`, `bounds-probe`, `typeassert-abort-probe` recipes after migrating; remove from `verify` list); `examples/` fixtures.

**Interfaces — Produces:** per-fixture sidecars later tasks rely on: `<name>.exit` (single integer expected exit code; absent ⇒ 0 required), `<name>.stderr.txt` (substring that must appear in stderr), `GOLDEN_TIMEOUT` env (seconds, default 10).

- [ ] **Step 1: Failing self-tests.** Add two throwaway fixtures: `examples/tmp_loop_probe.goo` (`for {}` busy loop) + `.expected.txt` (empty), and `examples/tmp_exit_probe.goo` (program that prints `ok` then indexes out of bounds → today aborts 134) + `.expected.txt` containing `ok`. Run `GOLDEN_TIMEOUT=3 make test-golden`: TODAY the loop fixture hangs the suite forever and the exit fixture can PASS on stdout despite the abort. Record both behaviors as RED evidence (kill the hung run).
- [ ] **Step 2: Implement in `run_golden.sh`.** Wrap the fixture run in `timeout "${GOLDEN_TIMEOUT:-10}"`; capture `rc=$?` (real, unpiped — note the runner currently captures stdout via `$(...)`; restructure to a temp file so stdout, stderr, and rc are all real). After stdout diff: if `<name>.exit` exists, require `rc == $(cat .exit)`, else require `rc == 0` (124 from `timeout` is always FAIL, reported as `FAIL <name> (timeout)`). If `<name>.stderr.txt` exists, require `grep -qF -- "$(cat .stderr.txt)" <captured stderr>`. Keep the `.env` convention working.
- [ ] **Step 3: Verify self-tests.** Loop fixture now FAILs within 3s (`timeout`); exit fixture FAILs on rc (nonzero without `.exit`). Then delete both tmp fixtures. Full `make test-golden` → 321/0 (existing fixtures unaffected; any that legitimately exit nonzero get a `.exit` sidecar — report which, expected none).
- [ ] **Step 4: Migrate the three abort probes.** Re-express `divzero-probe`, `bounds-probe`, `typeassert-abort-probe` as golden fixtures (`examples/divzero_abort_probe.goo` + `.expected.txt` + `.exit` [134 today — Task 6 will flip to 2] + `.stderr.txt` with the runtime message; same pattern for the other two — copy the program bodies from the deleted Makefile recipes). Delete the three Makefile targets and their `verify` entries. `make test-golden` → 324/0; `make verify`'s remaining chain unaffected.
- [ ] **Step 5: Gates + commit.** All gates. `git commit --no-gpg-sign -m "test(golden): per-fixture timeout, exit-code and stderr conventions; migrate abort probes"`.

### Task 2: Data-driven compile-reject harness (P0.9)

**Files:** Create `scripts/run_golden_reject.sh`, `tests/golden/reject/*.goo` + `*.err.txt`; Makefile (`test-golden-reject` target, `.PHONY`, wire into `verify` next to `test-golden`; delete the 5 migrated probe targets).

**Interfaces — Produces:** `tests/golden/reject/<name>.goo` + `<name>.err.txt` convention: runner asserts (a) compiler exit nonzero, (b) no binary emitted, (c) stderr contains the `.err.txt` substring. Tasks 3–4 add fixtures here.

- [ ] **Step 1: Failing self-test.** Write `scripts/run_golden_reject.sh` skeleton mirroring `run_golden.sh`'s discovery loop over `tests/golden/reject/`, with a deliberate inverted pilot: add `tests/golden/reject/tmp_accepts.goo` containing a VALID program + `.err.txt` `nonsense` — runner must FAIL it (compiler exits 0). Run and record the FAIL (proves the harness can see acceptance-when-rejection-expected). Delete the tmp pilot.
- [ ] **Step 2: Migrate 5 pilots.** Pick 5 stable-stderr reject probes (recommended: `boolnot-reject-probe`, `addrlit-reject-probe`, `constdiv-reject-probe`, `funcsig-reject-probe`, `trailingcomma-reject-probe` — verify stderr stability yourself; open point 3 in the spec). For each: move the program into `tests/golden/reject/<name>.goo`, put the diagnostic substring in `<name>.err.txt`, delete the Makefile target, remove from `verify`.
- [ ] **Step 3: Wire + verify.** `make test-golden-reject` → `5 passed, 0 failed` line; joins `verify` and `.PHONY`. Confirm no binary is left behind on rejection (assert in the runner: output path absent after the compile attempt).
- [ ] **Step 4: Gates + commit.** All gates. `git commit --no-gpg-sign -m "test(reject): data-driven compile-reject golden harness with five migrated pilots"`.

### Task 3: LLVMVerifyModule gate, then reject !?T (P0.2 → P0.3, ordered)

**Files:** `src/codegen/codegen.c` (`codegen_emit_executable`, before `LLVMTargetMachineEmitToFile` at ~:1592); the type checker's function-return-type path for the `!?T` rejection; `tests/golden/reject/errunion_nullable.goo` + `.err.txt`.

- [ ] **Step 1: Failing test (the crash).** `/tmp` reproducer: `func f() !?int { return 42 }` called from main. TODAY: `bin/goo` dies SIGILL/exit 132, zero diagnostics. Record exact behavior.
- [ ] **Step 2: Verify gate (P0.2).** In `codegen_emit_executable`, before emitting: `LLVMVerifyModule(codegen->module, LLVMReturnStatusAction, &msg)`; on failure print `internal compiler error: generated invalid IR (please report): %s` via the existing `codegen_error` path, dispose msg, return 0 (driver exits 1). Re-run the reproducer: now exit 1 with the ICE diagnostic, NO signal. This is the gate's proof — record it before Step 3.
- [ ] **Step 3: Type-checker rejection (P0.3).** Reject any `!?T` (error union whose payload is nullable) at declaration/return-type check with: `error union of nullable type is not supported in v1` + position. Reproducer now exits 1 with THIS diagnostic (never reaches codegen). `?!int` stays compiling (frozen) — verify it still does; if it also emits invalid IR, the new gate catches it: report as open finding, do not change it.
- [ ] **Step 4: Pin.** Add `tests/golden/reject/errunion_nullable.goo` + `.err.txt` (`error union of nullable`). `make test-golden-reject` → 6/0.
- [ ] **Step 5: Gates + commit.** `git commit --no-gpg-sign -m "fix(codegen): verify module before emission; reject !?T cleanly in the type checker"`.

### Task 4: Lexer soundness — TOKEN_UNKNOWN gate + comment-skip iteration (P0.4 + P0.5)

**Files:** `src/lexer/lexer.c` only (:552-558 is the TOKEN_ERROR model; :316 and :329 are the two `return lexer_next_token(lexer)` comment tail-calls). Fixtures: `tests/golden/reject/unknown_token.goo` + `.err.txt`; Makefile `comment-lines-probe` beside `blank-lines-probe`.

- [ ] **Step 1: Failing tests.** (a) `x := 1 # 2` in main, print x — TODAY compiles and prints 1. (b) `python3 -c "print('package main'); print('func main() {'); [print('// c') for _ in range(400000)]; print('}')" > /tmp/comments.goo` — TODAY SIGSEGV (stack overflow ~100k). Record both.
- [ ] **Step 2: TOKEN_UNKNOWN gate.** Extend the `TOKEN_ERROR` block at lexer.c:552-558 to also match `TOKEN_UNKNOWN`: same `goo_lexer_error_count++` + positioned `%s:%d:%d: error: unknown token '%s'` diagnostic. Reproducer (a) now rejected, exit 1, names `#` at its column.
- [ ] **Step 3: Comment iteration.** Convert both comment paths to `continue` the existing `for (;;)` scan loop (the newline-skip path already does this — mirror it) instead of tail-recursing. Reproducer (b) compiles exit 0. ASI interplay preserved: `x := 10 // c` ⏎ `*p = 7` program still prints 7 (write this as a quick /tmp check; the shape is already pinned by `asi-hardening-probe`).
- [ ] **Step 4: Pin.** `tests/golden/reject/unknown_token.goo` + `.err.txt` (`unknown token`) → reject suite 7/0. New Makefile `comment-lines-probe` (generate the 400k file into `build/`, compile, assert exit 0), wired into `verify` + `.PHONY` beside `blank-lines-probe`.
- [ ] **Step 5: Gates + commit.** `git commit --no-gpg-sign -m "fix(lexer): reject unknown tokens; iterative comment skip survives 400k comment lines"`.

### Task 5: Comma-ok channel receive double-recv miscompile (P0.1)

**Files:** the comma-ok receive lowering (start at `src/codegen/lowlevel_codegen.c:143-156` `goo_chan_recv` emission and find the two-value form's caller — spec open point 1: locate the double-emission site; check whether `select` comma-ok shares it). Fixture: `examples/commaok_recv_probe.goo` + `.expected.txt`.

- [ ] **Step 1: Failing test.** Reproducer: buffered `ch := make(chan int, 1); ch <- 9; v, ok := <-ch; fmt.Println(v); fmt.Println(ok)`. TODAY: aborts `all goroutines are asleep - deadlock!` exit 2. Also `--emit-llvm` it and `grep -c 'call.*goo_chan_recv'` → 2. Record both.
- [ ] **Step 2: Locate + report.** Find where the two-value receive emits `goo_chan_recv` twice (likely once for the value, once for the ok status re-evaluating the receive expression). Report the mechanism in your notes before fixing.
- [ ] **Step 3: Fix.** One `goo_chan_recv` call; its status output feeds `ok`, its value output feeds `v`. Reproducer prints `9` then `true`, exit 0; IR grep → exactly 1 call. Single-value receive `v := <-ch` byte-identical IR before/after (diff one existing program's `.ll`).
- [ ] **Step 4: Pin + matrix.** Golden fixture `examples/commaok_recv_probe.goo` (buffered value → `9 true`, then drained-and-closed... no `close()` in v1 yet — instead second case: comma-ok inside `select` if it shares the path per Step 2; otherwise pin what exists and note it). `make test-golden` → 325/0 (report actual). Existing `commaok-probe` in verify still passes.
- [ ] **Step 5: Gates + commit.** `git commit --no-gpg-sign -m "fix(codegen): comma-ok channel receive emits a single goo_chan_recv"`.

### Task 6: Go-conformant panics — exit 2, opt-in abort (P0.6)

**Files:** the `goo_panic` definition in `src/runtime/` (grep `goo_panic` for the definition site); Makefile probes asserting 134/SIGABRT (spec open point 4: `grep -nE '134|SIGABRT|core' Makefile scripts/` first and inventory); the three `.exit` sidecars from Task 1 flip 134→2.

- [ ] **Step 1: Failing test.** Out-of-bounds program: TODAY exit 134 (SIGABRT + core). Record. Inventory every 134/SIGABRT assertion (report the list).
- [ ] **Step 2: Implement.** `goo_panic` prints the runtime error to stderr then `exit(2)`; if `GOO_PANIC_ABORT=1` in the environment, `abort()` instead (document in a comment: debugging escape hatch). 
- [ ] **Step 3: Update assertions same-commit.** Flip the Task 1 `.exit` sidecars to 2; update every inventoried Makefile probe to assert exit 2 (e.g. `panic-abort-probe`, `bits-div-abort-probe`, `funcnil-abort-probe`, `map-nilfunc-abort-probe`, `iface-target-assert-abort-probe`, `rtti-assert-panic-probe` — per your inventory). `GOO_PANIC_ABORT=1` run of the reproducer aborts (verify once manually).
- [ ] **Step 4: Gates + commit.** Full gates (`verify`'s abort probes now green on exit 2). `git commit --no-gpg-sign -m "fix(runtime): panics exit 2 per Go, GOO_PANIC_ABORT=1 restores abort"`.

### Task 7: `make verify-core` — the ccomp-free gate (P0.10)

**Files:** Makefile (+ CLAUDE.md documenting `verify` vs `verify-core`).

- [ ] **Step 1: Enumerate.** The ccomp-gated set in `verify`'s closure is exactly `v2-bootstrap-pilot` (depends `ccomp-build`; `ccomp-audit` is not in `verify`) — verify this claim yourself (spec open point 5): `make -n verify 2>&1 | grep -i ccomp` and read the dependency list at Makefile:2322.
- [ ] **Step 2: Implement.** `verify-core: <the verify list minus the ccomp-gated set>` (derive it textually from the `verify` line, don't duplicate-and-drift: define one variable `VERIFY_CORE_DEPS` used by both, with `verify: $(VERIFY_CORE_DEPS) v2-bootstrap-pilot`). `.PHONY` both.
- [ ] **Step 3: Prove on a stripped PATH.** `env PATH=/usr/bin:/bin make verify-core` (no opam shims) exits 0. `make verify` unchanged (still includes the pilot; on this machine it fails at ccomp as before — unchanged is the assertion, not green).
- [ ] **Step 4: Document.** CLAUDE.md: `verify-core` = the authoritative ccomp-free gate (pre-push-suitable everywhere); `verify` = full chain including the CompCert bootstrap pilot.
- [ ] **Step 5: Gates + commit.** All gates + `make verify-core` green. `git commit --no-gpg-sign -m "build: verify-core target runs the full probe net without CompCert"`.

---

## Self-review

- Spec coverage: P0.7+P0.8→Task 1, P0.9→Task 2, P0.2+P0.3→Task 3 (ordered), P0.4+P0.5→Task 4, P0.1→Task 5, P0.6→Task 6, P0.10→Task 7. Sequencing and the P0.6 same-commit carve-out match the spec's Global Constraints. Open verification points 1–5 are embedded in Tasks 5, 1, 2, 6, 7 respectively.
- Counts (321→324→ etc.) are expectations with "report actual" hedges; fixture names are suggestions the implementer may adjust — the conventions, not the names, are the contract.
- No placeholders; every reproducer's program shape is stated inline; C-side changes are anchored to verified file:line sites with the mechanism named, with investigation steps where the audit left the mechanism open (Task 5 Step 2).
