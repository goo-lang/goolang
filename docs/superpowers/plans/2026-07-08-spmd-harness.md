# SPMD Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The goroutine-per-lane SPMD keystone delivered: the defer-stash compiler bug fixed (the one hard blocker), the scout-proven SPMD idioms committed as goldens, a correctness-asserting parallelism benchmark wired into `verify`, and the pattern documented.

**Spec:** `docs/superpowers/specs/2026-07-08-spmd-harness-design.md` — normative; read first. The scout report's program shapes are reproduced in the spec's goldens section.

## Global Constraints

- C23. Clean type errors / clean failures only — no LLVM verifier noise. No naked returns.
- No parser.y/lexer changes expected anywhere in this plan (BLOCKED if you think otherwise). `./scripts/grammar-tripwire.sh` must stay `82 S/R + 256 R/R` exact — run as a guard in every task.
- FROZEN BEHAVIOR: everything shipped by sub-projects 1-2 must be byte-for-byte unchanged except the defer-stash fix's own target behavior. Gates before every commit: `make test-golden` (315/0 at start, grows), `make comptime-value-reject-matrix` (18/18), `make comptime-generic-compose-ir-pin` (PASS), `make test` (76/1-skip).
- Commits: conventional prefixes, `--no-gpg-sign`.

## File structure

- `src/codegen/statement_codegen.c` (~2280, defer arg-stash) — Task 1 only
- `examples/spmd_fanout_probe.goo`, `examples/spmd_defer_probe.goo` (+ `.expected.txt`) — Tasks 1-2
- `Makefile` — `spmd-bench-probe` target + verify wiring — Task 3
- `docs/spmd-harness.md` — Task 3

---

### Task 1: Fix the defer-stash destructive splice

**Files:** `src/codegen/statement_codegen.c`. Tests: `/tmp/t1_*.goo` + committed `examples/spmd_defer_probe.goo` (+ expected).

- [ ] **Step 1: Failing tests (the scout's exact shapes).** (a) Direct-call defer at two tuples: a comptime template with `defer send(out, v)`-style deferred call, instantiated at tile=4 and tile=2 → TODAY: `Undefined identifier '__goo_defer0_arg0'` + `failed to evaluate defer argument`, exit 1. (b) The `go`-dispatched form (deferred send inside a kernel launched via `go`, two tuples) — same failure. Reproduce both; record exact diagnostics as RED evidence.
- [ ] **Step 2: Read the stash.** src/codegen/statement_codegen.c ~2280: the defer arg-stash splices synthetic identifier nodes (`__goo_defer0_arg0`…) into the shared template AST. Identify: what is swapped in, where the originals go, whether the closure-literal defer form shares the path (spec open point 1), and whether go-dispatched defer emission shares it (open point 2). Report findings.
- [ ] **Step 3: Make the template AST observably unchanged after emission.** Choose the mechanism the code favors: (a) save the original arg nodes and swap them back immediately after the stash's emission consumes the synthetic ones; or (b) clone-and-splice on a per-emission copy; or (c) rewrite at the LLVM value level without touching the AST. Requirement either way: after emitting instance 1, instance 2's emission sees pristine args. Mind ownership — no double-free of arg nodes, no leak of the synthetic nodes (state who frees what in a comment).
- [ ] **Step 4: Verify the matrix of shapes.** All must now compile and run correctly: two-tuple × {direct-call defer, closure-literal defer} × {plain call, go-dispatched} on the comptime axis; plus one generic-only two-type deferred case and one composed two-tuple case (the bug is axis-independent — the fix must be too). Single-tuple defer (both forms) byte-for-byte unchanged — diff IR before/after your change for one single-tuple program to prove it.
- [ ] **Step 5: Commit the golden.** `examples/spmd_defer_probe.goo` (+ expected): deferred channel send in a kernel, uneven tiles (two tuples), go-dispatched — the natural SPMD shape. Run via `make test-golden` → 316/0.
- [ ] **Step 6: Regression + commit.** All gates. `git commit --no-gpg-sign -m "fix(codegen): defer arg-stash no longer mutates the shared template AST"`.

---

### Task 2: The SPMD pattern goldens

**Files:** `examples/spmd_fanout_probe.goo` (+ expected; sibling files if cleaner). NO src/ changes — a compiler bug found here is BLOCKED, not a license to fix.

- [ ] **Step 1: Keystone golden.** The scout's P1-v2 shape: comptime slice kernel with `[tile]int64` local reduction, `[][]int64` partitioning, loop-spawned `go lane(TILE, tiles[i], results)`, buffered fan-in, deterministic aggregate output. EXTEND it with the uneven-remainder lane (two lanes tile=4 + one lane tile=2 → two comptime tuples concurrent) and `len()`-arithmetic partitioning (scout P6 idioms). Deterministic output only (aggregates, not per-lane order).
- [ ] **Step 2: Multi-lane same-tuple golden.** Scout P2: 8 loop-spawned goroutines on ONE instance, loop-var args captured per-iteration, output 224 (or your verified arithmetic).
- [ ] **Step 3: Composed-lane golden.** A generic×comptime kernel in the SPMD shape within today's limits (data movement on T, concrete `chan int64` collection or closure wrapper — scout P5c idiom). Pins composition-in-SPMD and the wrapper idiom.
- [ ] **Step 4: Run + commit.** `make test-golden` → 319/0 (or your actual count; report). All gates. `git commit --no-gpg-sign -m "test(spmd): keystone fan-out, multi-lane, and composed-lane goldens"`.

---

### Task 3: Benchmark probe + guide

**Files:** `Makefile`, `docs/spmd-harness.md`. NO src/ changes.

- [ ] **Step 1: `spmd-bench-probe` target.** Two inline programs (model recipe hygiene on the existing probes — `rc=$$?` off the compiler invocation, no piped-exit masking): a CPU-bound comptime kernel run (a) 8-lane via `go`, (b) serial. ASSERT: both compile, both run, outputs bit-identical. REPORT (informational echo, never pass/fail): `/usr/bin/time -v` (or `-l`/portable fallback — detect) wall + CPU for both. Optional `SPMD_BENCH_ASSERT_SPEEDUP` env gate, default off, documented in the recipe comment. Must pass on low-core machines (correctness only).
- [ ] **Step 2: Wire into verify + .PHONY** (next to the existing probe targets).
- [ ] **Step 3: The guide.** `docs/spmd-harness.md`: the canonical program annotated; the idioms (partitioning, const tiles, remainder tile = second tuple, buffered fan-in as the join, closure wrapper for generic channels); current limits + workarounds from the spec's Scope section verbatim-consistent (don't re-derive, don't contradict); the measured numbers (787% CPU / ~6.2× wall, 32-core machine, method: external `/usr/bin/time`) labeled as session-measured.
- [ ] **Step 4: Full regression + commit.** All gates + `make spmd-bench-probe` PASS. `git commit --no-gpg-sign -m "test(spmd): parallelism benchmark probe + SPMD harness guide"`.

---

## Self-review

- Spec coverage: compiler fix → Task 1; pinned pattern → Tasks 1-2 goldens; proof → Task 3 probe; guide → Task 3; open points 1-2 → Task 1 Step 2, point 3 → Task 3 Step 1. Scope/YAGNI items are documented in the guide, not fixed — matching the spec's scope decision.
- The one src/ change is Task 1's single file; Tasks 2-3 are frozen-compiler tasks with explicit BLOCKED discipline.
- Golden counts are stated as expectations with "report actual" hedges where arithmetic depends on the implementer's final program shapes.
