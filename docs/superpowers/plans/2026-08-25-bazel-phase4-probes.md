# Bazel Migration — Phase 4 (the probe gates) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give all **183** probe gates a Bazel counterpart, taking parity from 212 unmapped to **29**.

**Architecture:** Two macros, not one. A fixture macro for the 80 probes that compile an `examples/*.goo`; an assertion macro for the 69 that `printf`-generate their source, whose sources are extracted to real files behind a drift gate. 28 script-backed probes become `sh_test`. 6 are hand-written. The bulk is **generated** from the Makefile and re-checked by regeneration, because 183 hand-written declarations cannot be reviewed meaningfully.

**Tech Stack:** Bazel 9.2.0, `rules_cc` 0.2.17, `rules_shell` 0.6.1, python3 for the generator.

**Spec:** `docs/superpowers/specs/2026-08-25-bazel-migration-design.md` (see "The probe census")
**Predecessors:** phases 0–3b, all merged.

## Global Constraints

Everything in the phase 0–3b plans' Global Constraints still applies. In addition:

- **The Makefile is not edited in this phase.** Every probe keeps its recipe. This phase adds a second way to run the same assertions.
- **The census is the plan's spine, and it was measured, not assumed:** 28 script-backed, 48 golden-fixture, 32 other-`examples`, 69 `printf`-generated, 6 bespoke. 28+48+32+69+6 = 183. Regenerate it with the commands in Task 1 Step 1 before trusting any count here.
- **Probes need a linked, running binary,** so they need `//src/compiler:goo` AND `//src/runtime:goo_runtime_archive` (phase 3b), with `GOO_RUNTIME` and `GOOROOT` both pinned. Phase 3 established that the compiler resolves two things relative to its own executable, and the Bazel binary sits somewhere neither resolves from.
- **9 probes will not pass, and that is expected.** `far_shim_probe` and eight `lanes_*` reach the far transport and need NNG (phase 3c). Any probe whose fixture is one of those nine is tagged `manual` with a comment naming phase 3c, not quietly omitted.
- **A generated BUILD file is only trustworthy if regenerating it is a gate.** The generator runs in CI and its output is diffed against what is committed; a difference fails. Without that, the generated file is a snapshot nobody can verify.
- **Extracted probe sources are a second copy, and second copies drift.** `tools/probe_source_drift.sh` re-runs each Makefile recipe's `printf` and diffs the result against the extracted file. Both gates passing while testing different programs is precisely the failure this prevents.

---

## Atomic task list

Seventeen tasks, one logical commit each. Counts are the 2026-08-25 census;
**Task 1 re-measures them** and later tasks use the new numbers if they differ.

### A. Foundations

- [ ] **1. Record the probe census as a checked-in file.** `tests/probes/census.txt`, produced by a script, not by hand.
      *Accepts:* the script prints 28 / 48 / 32 / 69 / 6, total 183; the file is committed; re-running produces no diff.

- [ ] **2. The probe runner and the fixture macro.** `tools/run_probe.sh` plus `goo_probe()` in `tools/goo_probe.bzl`, mirroring `run_golden.sh`'s sidecar contract.
      *Accepts:* one hand-written `goo_probe` over a known-good fixture passes; exit statuses read with no pipe; `rc 124` is always a timeout, never compared to an expected code.

- [ ] **3. Prove the fixture macro can fail.** A teeth fixture whose expected output deliberately disagrees, tagged `manual`.
      *Accepts:* the wrong pair exits non-zero; a matching pair exits 0; a MISSING expected file exits non-zero rather than passing vacuously.
      *Risk:* **highest in the phase.** One macro generates ~150 tests; if it cannot fail, all 150 pass while asserting nothing.

### B. The 80 fixture probes

- [ ] **4. The generator, fixture probes only.** `tools/gen_probe_targets.py`.
      *Accepts:* refuses to guess -- any recipe it cannot parse confidently is printed to stderr and omitted, with the count reported. Silent omission fails this task.

- [ ] **5. The generated targets, all green.**
      *Accepts:* `bazel test //tests/probes:all` green except probes over the nine NNG fixtures, which are tagged `manual` naming phase 3c; at least 10 compared against `make <probe>` and agreeing.

- [ ] **6. The regeneration gate.** Regenerates and diffs against the committed file.
      *Accepts:* passes on the committed tree.

- [ ] **7. Prove the regeneration gate can fail.**
      *Accepts:* editing one line turns it red; restoring turns it green; the tree is clean afterwards.

### C. The 69 extracted probes

- [ ] **8. The assertion macro.** `goo_expect_probe()` -- exit code, stderr substring, stdout substring. A thin wrapper over `goo_probe`, not a forked runner.
      *Accepts:* one hand-written negative probe passes; the runner is shared, not duplicated.

- [ ] **9. Prove the assertion macro can fail.**
      *Accepts:* two teeth fixtures -- one wrong exit code, one absent stderr string -- each red, each green when corrected.

- [ ] **10. Extract the 69 sources.** Write `tests/probes/src/*.goo` by RUNNING each recipe's `printf`, never by transcribing.
      *Accepts:* 69 files exist; the extraction is scripted and repeatable; no file was typed by hand.

- [ ] **11. The source-drift gate.** `tools/probe_source_drift.sh` re-runs each `printf` and diffs against the extracted file.
      *Accepts:* passes on the committed tree; carries an empty-corpus guard.

- [ ] **12. Prove the drift gate can fail.**
      *Accepts:* a one-character edit gives exit 1 naming that probe; an empty corpus gives exit 2; restored gives exit 0.
      *Risk:* **second highest.** Without this the Make probe and the Bazel probe can test different programs while both pass.

- [ ] **13. Emit the 69 targets.** Extend the generator to the `printf` category.
      *Accepts:* the 69 green (minus NNG-blocked); the regeneration gate still passes; at least 10 compared against `make`.

### D. The tail

- [ ] **14. The 28 script-backed probes.** One `sh_test` each, script and fixtures as `data`.
      *Accepts:* all 28 green INSIDE the sandbox. A script reading a path not in `data` is fixed, not run outside the sandbox.

- [ ] **15. The bespoke six.** `arena-free`, `arena-valgrind`, `charlit-reject`, `goostd-resolver`, `hexesc-reject`, `stencil-race-runbook`.
      *Accepts:* each green or explicitly tagged, with a comment saying why it is not generated. **`arena-valgrind-probe` must not reproduce its silent skip** -- a real config or a `requires-valgrind` tag, never a `SKIPPED` that reads as a pass.

### E. Close

- [ ] **16. CI wiring.** Probe suite, regeneration gate and drift gate in the workflow.
      *Accepts:* YAML valid; `workflow-targets-probe` passes; no `${{ }}` inside a `make` line.

- [ ] **17. Close the phase.**
      *Accepts:* parity falls 212 to about 29; every probe gate mapped or allowlisted with a reason; both build systems green; both teeth fixtures still able to fail; the CI conclusion read from the API, never a piped exit.

## Where the risk actually sits

Five of the seventeen tasks (3, 7, 12, and part of 15 and 17) do nothing but
prove a check can fail. That ratio is the phase's risk profile, not caution.
Tasks 4-6 and 13 GENERATE roughly 150 tests from one parser, so a defect there
is not one wrong test but 150 confidently green ones. The generator's parse of
a recipe is an INFERENCE about what that recipe asserts, and a systematically
wrong inference is invisible. Task 5's "compare at least 10 against make" is
the only guard, and 10 may be too few.

Task 15 also hides a decision. `arena-valgrind-probe` prints SKIPPED and exits
0 when valgrind is absent. Migrating it faithfully would preserve a defect the
spec explicitly criticises, so the acceptance criterion forbids it -- which
means phase 4 CHANGES its behaviour rather than porting it.

## What phases 5–7 still need

- **Phase 5:** the 34 non-probe gates — unit suites, `*-selftest`, stress tests, `*-ir-pin`. Several are already mapped. Golden suites land with phase 3c.
- **Phase 6:** sanitizer configs and `testing/teeth` red/green proofs, plus the coverage floor. This is where `arena-valgrind-probe`'s silent skip is properly fixed.
- **Phase 7:** `parity.sh` reaches 0, the allowlist is justified entry by entry, and the Makefile is deleted along with `tools/compiler_differential.sh`, `tools/golden_bazel.sh`'s split assertion and `tools/probe_source_drift.sh` — all three exist only to compare two build systems.
