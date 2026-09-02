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

- [x] **6. The regeneration gate.** `tests/probes/targets_current.sh`, the name
      `generated.bzl`'s own header already prescribed. Runs in make
      (`targets-current-probe`) AND under `bazel test //...` — it needs no
      nested bazel, so unlike `census_current` it carries no `manual` tag.
      *Accepts:* passes on the committed tree. **Met:** 63 targets, current.

- [x] **7. Prove the regeneration gate can fail.** `--self-test`: a control plus
      three mutations, each on a COPY of the tree, because this task's
      acceptance requires the work tree clean afterwards and `census_current.sh`
      operates on `$root` directly. The Bazel wiring was proven separately by
      mutating `generated.bzl` and confirming exit 3.
      *Accepts:* editing one line turns it red; restoring turns it green; the tree is clean afterwards. **Met.**

      **Found while doing this:** `census_current` was tagged `manual`, had no
      Makefile target and no workflow, so it had NEVER RUN. Its first execution
      found the census stale — `link-flags-probe` had landed in #329 without it.
      `census-current-probe` is now a make gate too. Census 183 to 186.

      **The 1–5 boxes above are stale, not the work.** `HANDOFF-bazel-migration.md`
      records tasks 1–5 as done, and the artefacts are all present. Nobody
      ticked them.

### B2. The reject probes (added while executing)

- [x] **4b. The reject macro.** `goo_reject_probe()` -- the compile must fail, no binary may be emitted, a named diagnostic must appear, and stderr must NOT carry "Module verification failed" or "LLVM ERROR".
      *Why it was added:* the census's `example` category turned out to be 18 compile-reject probes and 14 run-shaped ones. A reject probe's fourth assertion distinguishes a language rejection from a compiler crash, and BOTH give a non-zero exit. The approved two-macro design had nowhere to put it.
      *Accepts:* `stderr_contains` is mandatory -- "the compile failed" is also satisfied by a crash, a missing file, or a typo in the fixture name.

- [x] **4c. Prove the reject macro can fail.**
      *Accepts:* a program that compiles cleanly, declared as a reject, goes red; a genuinely-rejected fixture with its real diagnostic goes green.

### C. The 69 extracted probes

- [x] **8. The assertion macro.** `goo_expect_probe()` in `tools/goo_probe.bzl`.
      It calls the same `_probe` as `goo_probe`, so the runner is shared, and it
      `fail()`s at load time on a probe that asserts nothing.
      *Accepts:* one hand-written negative probe passes; the runner is shared, not duplicated. **Met** (done before this PR; the box was never ticked).

- [x] **9. Prove the assertion macro can fail.** `testing/teeth/BUILD`:
      `probe_exit_defect` (wrong exit code) and `probe_stderr_defect` (absent
      stderr string), each with a control over the same program.
      *Accepts:* two teeth fixtures -- one wrong exit code, one absent stderr string -- each red, each green when corrected. **Met.**
      **But `tools/verify_probe_teeth.sh`, which runs them, was never wired** --
      no Makefile target and no workflow, so it had never executed. It is a CI
      step now, in the bazel workflow, since it drives bazel and `verify-core`
      is bazel-free. Its first run: 7 checks, 4 defects red, 3 controls green.

- [x] **10. Extract the sources.** **161 sources, not 69.** The task named the
      69 GATES; only 27 of them write one source, and the rest write up to six.
      **159 are extracted**, under `tests/probes/src/<gate>/`; 156 `.goo` plus
      3 `.go` package files.
      **Two gates are REFUSED, with the reason printed on every run.**
      `blank-lines-probe` and `comment-lines-probe` build their source
      mechanically at scale (`yes '' | head -n 1000000`), so extracting them
      put 3 MB of blank lines and comments in the tree — exactly what a
      generator exists to avoid, and neither file carries anything a diff can
      use. They stay Make-only until task 15 gives them a Bazel target that
      GENERATES the source at build time. `EXPECTED_REFUSED` is asserted, so a
      third refusal has to be deliberate.
      *Accepts:* the extraction is scripted and repeatable; no file was typed by hand. **Met** --
      `tools/probe_source_drift.sh --extract` runs the real recipes and parses
      nothing. Parsing was rejected on evidence: `spmd-bench-probe` and
      `stencil-parallel-probe` write their sources with a backslash-continued
      `printf '%s\n'` spanning 30 lines, which any line-based extractor
      truncates in silence.

- [x] **11. The source-drift gate.** `tools/probe_source_drift.sh`. Extractor
      and gate are the same tool, because extraction IS the derivation -- the
      shape `census_current.sh` and `targets_current.sh` already use.
      *Accepts:* passes on the committed tree; carries an empty-corpus guard. **Met**, plus a
      second guard: a printf gate that writes NO source is a tool failure, not
      a pass, because it would mean the tool silently stopped covering it.

- [x] **12. Prove the drift gate can fail.**
      *Accepts:* a one-character edit gives exit 1 naming that probe; an empty corpus gives exit 2; restored gives exit 0. **Met**, plus a control.
      *Risk:* **second highest.** Without this the Make probe and the Bazel probe can test different programs while both pass.

- [x] **13. Emit the targets.** The generator reads `tests/probes/src/` for the
      `printf` category and judges each SOURCE on its own, never a gate
      all-or-nothing.
      *Accepts:* green; the regeneration gate still passes; at least 10 compared against `make`. **Met:**
      **136 targets** (63 fixture + 73 printf across 37 gates), `bazel test
      //tests/probes:all` 137/137, and 15 gates compared against `make` with 15
      agreements and 0 disagreements.
      **100 sources refused, each with a printed cause** — the deliverable, not
      a shortfall. Largest groups: 30 with no single expected exit code, 13
      comparing captured stdout, 12 with no recipe line naming them.

      **Three parser defects the run caught, all refusing wrongly rather than
      passing wrongly.** The LLVM-verifier guard
      (`grep -qiE "Module verification failed|LLVM ERROR"`) read as an
      unmodelled alternation and refused 44 sources for carrying boilerplate
      `goo_reject_probe` implements natively. The 3 extracted `.go` package
      members would have produced `.goo` labels for files that do not exist.
      And a diagnostic holding regex metacharacters was copied into
      `stderr_contains`, which is `grep -qF` — Starlark rejected the escape,
      which was the loud half of a defect whose quiet half is a pattern that
      never matches.

      **One contract was widened rather than worked around.** The recipes
      assert with `grep -qi`, and a case-SENSITIVE match is a different
      assertion: `strindex-reject-probe` greps for `error` where the compiler
      prints `Error`. Refusing every case-insensitive diagnostic cost 49
      targets, so `--stderr-ignorecase` was added to `tools/run_probe.sh` and
      both macros instead. The Bazel side now asserts what the recipe asserts.
      **Re-scoped by task 10's measurement.** This is not 69 targets. 161
      sources exist, each needing its own assertions, and the assertions are
      interleaved per source in the recipe. Some cannot be expressed at all:
      `asi-hardening-probe` asserts *"if exit is 0 AND stdout is 5, FAIL"*, a
      negative compound condition `goo_expect_probe` has no form for. Expect
      the generator to REFUSE a real fraction and print reasons. That is the
      design working, and the refusal count is the deliverable, not a defect.

### D. The tail

- [~] **14. The script-backed probes.** One `sh_test` each. **10 of 29 operate.**
      Not ticked: this is a partial pass, and the remaining 19 are recorded in
      `tests/probes/BUILD` with a measured cause each rather than declared red
      or tagged `manual` and left never-run.

      **The contract.** `COMPILER` names the compiler; the compiler finds its
      own archive and stdlib through `GOO_RUNTIME` and `GOOROOT`, which it
      already reads. Necessary, and NOT sufficient — two further layers showed
      up only when a target ran: the four `arc_*` probes take `ROOT` from
      `git rev-parse` and a sandbox has no `.git`, and `$(rootpath)` is
      relative to the runfiles root while those same scripts `cd "$ROOT"`
      before using it.

      **8 cannot be sandboxed at all.** `podman_image_probe.sh` EXITS 0 when
      podman is missing, so a target for it is green everywhere without podman
      while asserting nothing. `repro_build_probe.sh` builds `git archive
      HEAD` on purpose. `archive_determinism_probe.sh` rebuilds the archive
      in-tree. The four `far-*` share a helper taking arguments and need NNG
      (phase 3c).

      **7 were mis-grouped by me**, from the Makefile's PREREQUISITE list.
      Five of them invoke a C compiler through a shell name (`$CC_`,
      `${CC:-gcc}`) — found by reading the scripts, missed by a
      command-position grep. They also scan or compile `src/**`, and
      **there cannot be a `//:c_sources`**: a glob does not cross a package
      boundary, and every `src/*` has its own BUILD. Ten per-package
      filegroups would be needed.

      **The four `arc_*` gates are done.** `$0` and `${BASH_SOURCE[0]}`
      resolve to a symlink under `tests/probes/`, one directory too deep for
      their old `dirname` fallback to find the root, so the fix is `$PWD` —
      already the runfiles root on entry. Sitting next to it: `GOO_PROBE_NO_SKIP`
      turns a missing valgrind into a FAIL under Bazel, where a SKIPPED log
      line is read by nobody, and the Bazel CI job now installs valgrind so
      the four gates exercise their assertions there instead of skipping them.

      **3 remain**: `arena_rss_probe` reads `tests/examples/` (a second
      fixture tree), `goo_test_probe` needs whole test packages with
      sidecars, and `dead_package_code_probe` imports `strings` so it needs a
      `GOOROOT` the macro cannot name — `//goostd:files` is a filegroup
      `$(rootpath)` refuses to expand. The same cwd resolver tier that
      unblocked the four `arc_*` gates means `dead_package_code_probe` most
      likely needs only `//goostd:files` in `data` too — unmeasured.

      *Next:* the 3 remaining sandbox failures are the cheapest remaining
      work, one data list each. The 10 per-package filegroups unlock 5 more.
      The 8 stay out.
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
