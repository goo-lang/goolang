# Bazel verify-core, stages 0 and 1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure the gate net, remove four defects that would corrupt later evidence, and make Bazel the build that produces `bin/goo` and `lib/libgoo_runtime.a`, with a sound shared cache.

**Architecture:** `make` stays the entry point and becomes a wrapper: it computes a host fingerprint, calls Bazel with `--config=local`, and copies the two outputs. Every Make gate then tests the Bazel binary. Gate families move to Bazel in a LATER plan, written after Task 0's measurements and Task 11's spike exist.

**Tech Stack:** Bazel 9.2.0 (bazelisk), rules_cc 0.2.17, rules_shell 0.6.1, GNU Make, bash, gcc (workstation `/usr/bin/gcc`, CI `/usr/bin/gcc-14`), NNG 1.12.0.

**Spec:** `docs/superpowers/specs/2026-11-27-bazel-verify-core-design.md` ("the spec"). Companion: `docs/superpowers/specs/2026-11-27-bazel-verify-core-gates.tsv`. Earlier design: `docs/superpowers/specs/2026-08-25-bazel-migration-design.md`.

**Out of scope (next plan):** spec section 6 (gate families, `tools/family_parity.sh`, the compile/run split D10), section 7, and the nightly comparison of section 8.

## Global Constraints

- One PR per task unless a task says otherwise. Branch from the latest `main`. Never push to `main`.
- Commit with `git -c commit.gpgsign=false commit`. Conventional commit prefixes.
- `make verify-core` must be green before each push. Read the real exit status; never read a status through a pipe.
- A new gate needs, in the SAME commit: a `--self-test` mode, an entry in `VERIFY_ALL_DEPS`, the bump of `tools/parity-gate-count.txt`, and a regenerated census (`tools/probe_census.sh`, under `LC_ALL=C`).
- A self-test must not read machine state: scrub `PATH` inside it. A script that runs `git init` in a scratch directory must `unset GIT_DIR` first.
- A measurement is quoted only with all compared runs taken in one sitting.
- Do not download any binary, installer or release file without the owner's approval. A new `bazel_dep` is a download.
- C is C23. `golangci-lint`/`clippy` do not apply; `-Werror` applies to every C change.
- Quote every glob passed to a tool (`--include='*.c'`); the shell is zsh for interactive use, scripts are bash.
- A cached PASS must mean what an uncached run would report (spec anti-goal 1). When in doubt, a gate is `no-cache`.

## Review Focus

1. A tool upgraded in place (same path, new version): the next `make verify-core` must re-run, not reuse. Pinned by Task 7.
2. `make lexer CC=clang-18` or any `CC` the wrapper does not know: an error naming the value, never a silent gcc build. Pinned by Task 8.
3. `bazel` missing from `PATH`: `make lexer` fails with one line naming bazelisk, not a shell "command not found" deep in a recipe. Pinned by Task 8.
4. A stale SARIF file from an earlier run present when the lanes did not run: `findings-probe` must FAIL. Pinned by Task 4.
5. Two worktrees pushing at the same time against one disk cache: both complete, neither reads a half-written entry. Pinned by Task 10.

---

### Task 0: Measure the gate net (spec M0)

No product code. This task decides N and may change the rest of the plan.

**Files:**
- Create: `tools/gate_times.sh`
- Create: `docs/superpowers/specs/2026-11-27-gate-times.tsv`

**Interfaces:**
- Produces: `tools/gate_times.sh [--self-test] [gate ...]` — runs each named gate (default: every name from `tools/parity.sh --list-make-gates` minus `HEAVY_DEPS`) with `make <gate>` after one `make lexer`, serially, and writes one TSV row per gate: `gate<TAB>wall_seconds<TAB>exit_status`. Output path from `GATE_TIMES_OUT` (default stdout).

- [ ] **Step 1: Write `--self-test`.** Two fixture gates in a scratch Makefile (`MAKEFILE` override, as `parity.sh` does): one `sleep 1; true`, one `false`. Assert the first row's seconds are ≥ 1, the second row's `exit_status` is non-zero, and an empty gate list exits 2.
- [ ] **Step 2: Run `tools/gate_times.sh --self-test`.** Expected: FAIL (script does not exist).
- [ ] **Step 3: Implement the script.** Use `date +%s.%N` around each `make`; never pipe `make`'s status.
- [ ] **Step 4: Run the self-test.** Expected: PASS.
- [ ] **Step 5: Take the measurement** on a quiet machine, one sitting: `GATE_TIMES_OUT=docs/superpowers/specs/2026-11-27-gate-times.tsv tools/gate_times.sh`. Record `nproc` (32 on the workstation) and the total in the file's first comment line.
- [ ] **Step 6: Write the summary** at the top of the TSV as comment lines: total serial time; the 20 slowest gates; the summed time of the always-run set (the 7 gates named at `tests/probes/BUILD:338` onward, plus `release-decision-teeth`, `escape-teeth`, `escape-arm-coverage-selftest`, `warning-count-probe`, `repro-build-probe`, `archive-determinism-probe`).
- [ ] **Step 7: STOP. Report to the owner:** the total, the always-run sum, and a proposed N for "a compiler change with a warm cache". If the always-run sum exceeds 3 minutes, list each such gate with its time and ask which move to nightly. Do not start Task 5 or later before the owner answers. Tasks 1-4 may proceed.
- [ ] **Step 8: Commit** `chore(bazel): measure verify-core gate times`.

### Task 1: `.claude/` in `.bazelignore` (spec H4)

**Files:**
- Modify: `.bazelignore`

- [ ] **Step 1: Show the defect.** `mkdir -p .claude/worktrees/zz_probe && printf 'filegroup(name="x")\n' > .claude/worktrees/zz_probe/BUILD`, then `bazel query '//.claude/...'`. Expected: lists `//.claude/worktrees/zz_probe:x`.
- [ ] **Step 2: Add the line `.claude` to `.bazelignore`.**
- [ ] **Step 3: Re-run the query.** Expected: an error that the package is not found, or an empty result. Remove the scratch directory.
- [ ] **Step 4: Run `bazel test //...`** from a checkout that has worktrees under `.claude/`. Expected: no target under `.claude`.
- [ ] **Step 5: Commit** `fix(bazel): ignore .claude worktrees`.

### Task 2: `parity.sh` reads any whitespace (spec H3)

**Files:**
- Modify: `tools/parity.sh` (`list_make_gates`, lines 31-44)
- Modify: `tools/parity_test.sh`
- Modify: `tools/parity-gate-count.txt`

- [ ] **Step 1: Add a test case to `tools/parity_test.sh`:** a fixture Makefile whose `VERIFY_ALL_DEPS` has three entries, the second on a line that starts with a TAB. Assert `--list-make-gates` prints all three.
- [ ] **Step 2: Run `bazel test //tools:parity_test`.** Expected: FAIL, two gates listed.
- [ ] **Step 3: Change `tr ' ' '\n'` to `tr '[:space:]' '\n'` in `list_make_gates`.**
- [ ] **Step 4: Run the test.** Expected: PASS.
- [ ] **Step 5: Run `tools/parity.sh --list-make-gates | grep -c .`** and confirm `test-golden-poison` is in the list. Set `tools/parity-gate-count.txt` to the new count (expected: old count + 1). If `test-golden-poison` then shows as UNMAPPED, add it to `tools/parity-allowlist.txt` with the reason, or map it — read `Makefile` for what it runs before choosing.
- [ ] **Step 6: Regenerate the census; run `make census-current-probe`.** Expected: PASS.
- [ ] **Step 7: Commit** `fix(parity): count a tab-led gate`.

### Task 3: A missing tool fails the gate (spec H1, D8)

**Files:**
- Modify: the scripts and Makefile recipes of the 15 gates: `arc-reassign-probe`, `arc-map-key-local-probe`, `arc-multi-assign-probe`, `arc-uaf-probe`, `arc-uaf-selftest`, `arc-loop-carried-probe`, `arc-concat-operand-probe`, `arc-release-probe`, `arena-valgrind-probe`, `ast-free-leak-probe`, `ast-free-leak-selftest` (valgrind); `obj-header-tsan` (clang); `stdlib-coverage-drift` (go); `arena-rss-probe`, `archive-determinism-probe` (read each to find its tool)
- Create: `scripts/no_skip_probe.sh`
- Modify: `Makefile` (`verify-core`, `verify-nightly`, `verify` export `GOO_PROBE_NO_SKIP=1`; new gate `no-skip-probe`)
- Modify: `CLAUDE.md` (Build System: the required tool list; replace "safe for pre-push on any machine")

**Interfaces:**
- Consumes: the existing convention `GOO_PROBE_NO_SKIP=1` → FAIL where the script would print SKIPPED (see `scripts/arc_reassign_probe.sh:78-80`).
- Produces: `scripts/no_skip_probe.sh [--self-test]` — for each of the 15 gates, runs its script with the tool removed from a scrubbed `PATH` and `GOO_PROBE_NO_SKIP=1`, and requires a non-zero exit and a line containing `FAIL`.

- [ ] **Step 1: Write `scripts/no_skip_probe.sh`** with a table `gate|script-or-make-target|tool`. Build the scrubbed `PATH` as a temp directory of symlinks to every needed tool EXCEPT the one under test. `--self-test`: a fixture script that exits 0 without its tool must be reported; one that fails must pass; an empty table must exit 2.
- [ ] **Step 2: Run `scripts/no_skip_probe.sh`.** Expected: FAIL, naming each gate that still exits 0 (at least `obj-header-tsan`, `stdlib-coverage-drift`, `arena-rss-probe`, `archive-determinism-probe`, and any recipe-only valgrind gate).
- [ ] **Step 3: Apply the convention to each failing gate.** Where the skip lives in a Makefile recipe, move the tool check into the recipe as `if ! command -v <tool> ...; then [ "$${GOO_PROBE_NO_SKIP:-0}" = 1 ] && { echo "<gate>: FAIL (<tool> not found, and GOO_PROBE_NO_SKIP forbids a skip)"; exit 1; }; echo SKIPPED; fi`.
- [ ] **Step 4: Export `GOO_PROBE_NO_SKIP=1`** for the three `verify*` targets (target-specific `export`).
- [ ] **Step 5: Run `scripts/no_skip_probe.sh` and its `--self-test`.** Expected: PASS for both.
- [ ] **Step 6: Wire `no-skip-probe`** per Global Constraints (VERIFY_ALL_DEPS, count, census).
- [ ] **Step 7: Update `CLAUDE.md`.** State the required tools: valgrind, clang, go, cmake, bison, python3, llvm-config, cppcheck, clang-tidy.
- [ ] **Step 8: Run `make verify-core`.** Expected: green. Commit `fix(gates): fail when a gate's tool is missing`.

### Task 4: The analyser lanes run once (spec H2)

**Files:**
- Modify: `scripts/findings_probe.sh` (the pipeline section after the `--self-test` block; header lines 46-47)
- Modify: `Makefile` (`findings-probe` gets `analyzer-lanes-probe` as a prerequisite)
- Modify: `scripts/analyzer_lanes_probe.sh` (write a stamp)

**Interfaces:**
- Produces: `build/sarif/lanes.stamp`, written by `analyzer_lanes_probe.sh` only after all three lanes succeed. Content: one line `sig=<gcc-tidy-cppcheck signature> tree=<sha256 of the sorted list of "sha256  path" for every file the lanes read>`.
- Consumes (findings_probe.sh): recompute `tree=`; if the stamp is missing, or `sig`/`tree` differ, FAIL with `findings-probe: FAIL (analyser SARIF is missing or stale; run analyzer-lanes-probe)`. Otherwise use `build/sarif/{gcc,clang-tidy,cppcheck}/` as they are. `FINDINGS_RUN_LANES=1` restores the old self-running behaviour for a standalone call.

- [ ] **Step 1: Time the current state** in one sitting: `make analyzer-lanes-probe findings-probe` (record seconds).
- [ ] **Step 2: Add self-test cases to `findings_probe.sh --self-test`:** (h) no stamp → RED; (i) stamp with a wrong `tree` → RED; (j) valid stamp but one lane directory emptied → RED. Run. Expected: the three new cases FAIL to be red.
- [ ] **Step 3: Implement the stamp and the check.** The tree hash uses `LC_ALL=C sort`.
- [ ] **Step 4: Run `make findings-probe-selftest`.** Expected: PASS.
- [ ] **Step 5: Disable ccache for the lanes if a cache hit can skip SARIF output** (`CCACHE_DISABLE=1` in the lane recipe); confirm with the existing positive control.
- [ ] **Step 6: Re-time** `make analyzer-lanes-probe findings-probe` in the same sitting as Step 1. Quote both figures in the PR body.
- [ ] **Step 7: Run `make verify-core`.** Expected: green. Commit `perf(gates): run the analyser lanes once per verify-core`.

### Task 5: Flag parity between Make and Bazel (spec 5.2)

**Files:**
- Modify: `.bazelrc`
- Create: `tools/flag_parity.sh`
- Modify: `tools/BUILD`, `Makefile` (gate `flag-parity-probe`)

**Interfaces:**
- Produces: `.bazelrc` — `build --copt=-Wpedantic`, `build --copt=-Werror`, and `build:nowerror --copt=-Wno-error` (the Bazel form of `WERROR=`). Vendored and generated targets keep `copts = ["-w"]`.
- Produces: `tools/flag_parity.sh [--self-test]` — extracts the compile command of `src/lexer/lexer.c` from `make -n` and from `bazel aquery 'mnemonic("CppCompile", //src/lexer/...)'`, normalises both to a sorted set of flags, drops path-only differences (`-I`, `-o`, `-MF`, `-iquote`, sandbox prefixes), and fails on any other difference. An allowlist file `tools/flag-parity-allow.txt` holds each accepted difference with a reason; a bare entry is refused.

- [ ] **Step 1: Write `tools/flag_parity.sh` and its `--self-test`** (two fixture command lines that differ by `-Wpedantic` must fail; identical ones pass; an empty extraction exits 2).
- [ ] **Step 2: Run `tools/flag_parity.sh`.** Expected: FAIL, listing at least `-Wpedantic`, `-Werror`, and whatever `-g`/`-D_GNU_SOURCE`/`-include` differences exist.
- [ ] **Step 3: Add the flags to `.bazelrc`.** For each remaining difference: fix it in `.bazelrc` or `tools/defs.bzl`, or allowlist it with the reason. Record which `--compilation_mode` matches Make's `-g` with no `-O`.
- [ ] **Step 4: Run `bazel build //...`.** Fix each new `-Werror` failure in `src/` (not by `-w`). Expected: green.
- [ ] **Step 5: Run `tools/flag_parity.sh`, `tools/compiler_differential.sh`, `bazel test //...`.** Expected: all pass.
- [ ] **Step 6: Wire `flag-parity-probe`** per Global Constraints. It is tagged `no-sandbox` in Bazel or allowlisted in parity, because it runs `bazel aquery`.
- [ ] **Step 7: Run `make verify-core`; commit** `build(bazel): match the Makefile's warning flags`.

### Task 6: NNG in the Bazel runtime archive (spec 5.1, D9)

**Files:**
- Create: `third_party/nng/BUILD`, `third_party/nng/nng.bzl`, `third_party/nng/nng.BUILD`
- Modify: `MODULE.bazel`, `src/runtime/BUILD` (lines 221-242: add `far_transport.c`, depend on `@nng//:nng`)
- Modify: `tools/golden_bazel.sh`, `tools/golden_bazel_known_fail.txt`
- Create: `tools/nng_overlay_check.sh`

**Interfaces:**
- Produces: repository `@nng` from a module extension that extracts `third_party/nng-1.12.0.tar.gz` after checking sha256 `50b7264bd8f0901f7ebdf3ec7c48f4e23dd689bbe7b2917d9d8fad58ffd09e5c` (a mismatch calls `fail()`); target `@nng//:nng`, a static `cc_library` with `copts = ["-w"]`.
- Produces: `tools/nng_overlay_check.sh` — runs the Makefile's CMake configure (`Makefile:180-186` options) with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` in a scratch directory and compares its source list and `-D` defines with `bazel aquery` for `@nng//:nng`. Fails on any difference. Run by hand on an NNG bump; not in verify-core (it needs cmake and takes a configure).

- [ ] **Step 1: Get the reference.** Run the CMake configure once; save the source list and defines to the scratchpad.
- [ ] **Step 2: Write `tools/nng_overlay_check.sh`; run it.** Expected: FAIL (`@nng` does not exist).
- [ ] **Step 3: Write the extension and `nng.BUILD`** from the reference list. Use `rctx.extract` on the vendored tarball; no network.
- [ ] **Step 4: Run `tools/nng_overlay_check.sh`.** Expected: PASS. If the defines cannot be reproduced without generated headers, STOP and report to the owner (spec D9: do not switch to `cmake()` silently).
- [ ] **Step 5: Add `far_transport.c` to the runtime archive.** Expected object count: 13, matching `RUNTIME_OBJS`.
- [ ] **Step 6: Run `tools/golden_bazel.sh`.** Empty `golden_bazel_known_fail.txt` of `far_shim_probe` and the eight `lanes_*`; the expected split becomes N/0.
- [ ] **Step 7: Compare archives.** `nm --defined-only` on the Make and Bazel archives, sorted under `LC_ALL=C`; the symbol sets must be equal. Record the result.
- [ ] **Step 8: Run `bazel test //...` and `make verify-core`; commit** `build(bazel): add NNG to the runtime archive`.

### Task 7: Host fingerprint and the Bazel wrapper (spec 6.4 I3, I4; 8)

**Files:**
- Create: `tools/host_fingerprint.sh`, `tools/bazel_local.sh`
- Modify: `.bazelrc`, `tools/BUILD`, `Makefile` (gate `host-fingerprint-selftest`)
- Modify: `third_party/llvm/llvm.bzl` (watch the resolved `llvm-config`)

**Interfaces:**
- Produces: `tools/host_fingerprint.sh [--self-test] [--list]` — prints a 64-hex sha256 of the lines `<tool> <first line of version output>` for: the `CC` in use, `llvm-config`, `bison`, `valgrind`, `cppcheck`, `clang-tidy`, `clang`, `python3`, `go`, `spin`, `cmake`; plus `uname -r` and `getconf GNU_LIBC_VERSION`. A missing tool contributes `<tool> MISSING`. `--list` prints the lines.
- Produces: `tools/bazel_local.sh <bazel-command> [args...]` — runs `bazel <command> --config=local --action_env=GOO_HOST_FP=<fp> --test_env=GOO_HOST_FP=<fp> [--repo_env=CC=<path>] args...`. Reads `CC` from the environment through the map `gcc`→`/usr/bin/gcc`, `gcc-14`→`/usr/bin/gcc-14`, `clang`→`/usr/bin/clang`, stripping a leading `ccache`; any other value exits 2 with `bazel_local: unknown CC '<value>'`. `NOCACHE=1` adds `--nocache_test_results`. Exits 2 with `bazel_local: bazel not found; install bazelisk` when `bazel` is absent.
- Produces: `.bazelrc` — `common:local --disk_cache=~/.cache/goolang-bazel-disk`, a cache size bound, `common:local --max_idle_secs=<n>`, and `build --incompatible_strict_action_env`.

- [ ] **Step 1: Confirm on Bazel 9.2.0** (`bazel help build`): the default of `--incompatible_strict_action_env`, and the exact name of the disk-cache size flag. Write both into a `.bazelrc` comment.
- [ ] **Step 2: Write the `--self-test` of `host_fingerprint.sh`:** with a scrubbed `PATH` of stub tools, the hash is stable across two calls; changing one stub's version line changes the hash; removing a stub changes it; the output matches `^[0-9a-f]{64}$`.
- [ ] **Step 3: Run it.** Expected: FAIL. Implement. Expected: PASS.
- [ ] **Step 4: Write `tools/bazel_local.sh`** and a test `tools/bazel_local_test.sh` using a stub `bazel` on a scrubbed `PATH` that prints its arguments: assert the three flags are present, `CC=gcc-14` maps to `--repo_env=CC=/usr/bin/gcc-14`, `CC='ccache gcc'` maps to `/usr/bin/gcc`, `CC=tcc` exits 2, no `bazel` exits 2, `NOCACHE=1` adds the flag.
- [ ] **Step 5: Add the `.bazelrc` lines.** Size bound: placeholder of 20 GB until Task 10 measures one full output set; then twice that.
- [ ] **Step 6: Review Focus 1.** By hand: `tools/bazel_local.sh test //tests/unit/...` twice (second run fully cached); then prepend a directory with a stub `valgrind` printing a different version to `PATH`; run again. Expected: tests re-run. Record the three outputs.
- [ ] **Step 7: Add `rctx.watch()`** on the resolved `llvm-config` path in `third_party/llvm/llvm.bzl`. Check: `touch` a copy used through `GOO_LLVM_CONFIG` and confirm the repository rule re-runs.
- [ ] **Step 8: Wire `host-fingerprint-selftest`; run `make verify-core`; commit** `build(bazel): add the host fingerprint and the local wrapper`.

### Task 8: The flip — `make lexer` builds through Bazel (spec D5, 5.3)

Do not start before the owner answered Task 0 and Tasks 5-7 merged.

**Files:**
- Modify: `Makefile` (lines 142, 193, 215, 299-310 and the rules they depend on)
- Modify: `.github/workflows/tests.yml`, `.github/workflows/nightly.yml` (Bazel setup, as `bazel.yml:44-66`), `.githooks/pre-push` (comment only)
- Modify: `CLAUDE.md` (Build System section)
- Create: `docs/superpowers/plans/2026-11-27-make-callers-audit.md`

**Interfaces:**
- Consumes: `tools/bazel_local.sh` (Task 7), `//src/compiler:goo`, `//src/runtime:goo_runtime_archive`.
- Produces: `make lexer` — phony; runs `tools/bazel_local.sh build //src/compiler:goo //src/runtime:goo_runtime_archive`, then copies each output to `bin/goo` and `lib/libgoo_runtime.a` only when `cmp -s` reports a difference.
- Produces: `make lexer-legacy` — the previous compile rules, writing to `build/legacy/bin/goo` and `build/legacy/lib/libgoo_runtime.a`, never to `bin/` or `lib/`. Used by the self-building gates and for rollback.

- [ ] **Step 1: Audit the 23 scripts that call `make`.** List them with: `for f in scripts/*.sh tools/*.sh; do grep -v -E '^\s*#' "$f" | grep -q -E '(^|[;&|(\x60 ]|\$\()make( |$)' && echo "$f"; done`. Run the same loop for a string you know is present first (positive control). For each, record in the audit file: the target it calls, whether it needs a rebuilt compiler, and its choice — "tests the shipped binary" (uses `bin/goo`), or "builds its own" (switch to `lexer-legacy` paths and say so in its header). No script may change what it compiles without a line in the audit.
- [ ] **Step 2: Write the red test.** `tools/flip_probe.sh`: after `make lexer`, `sha256sum bin/goo` equals that of `bazel-bin/src/compiler/goo`, and the same for the archive; a second `make lexer` leaves both mtimes unchanged. Run. Expected: FAIL (hashes differ).
- [ ] **Step 3: Rewrite the `lexer` target; move the old rules under `lexer-legacy`.** `$(COMPILER)` and `$(RUNTIME_LIB)` become order-only products of `lexer`.
- [ ] **Step 4: Run `tools/flip_probe.sh`.** Expected: PASS. Wire it as gate `flip-probe` (Global Constraints).
- [ ] **Step 5: Review Focus 2 and 3.** `make lexer CC=tcc` → exit non-zero, message names `tcc`. `PATH=/usr/bin-without-bazel make lexer` → one line naming bazelisk.
- [ ] **Step 6: Apply the audit** to each of the 23 scripts.
- [ ] **Step 7: Add Bazel setup to `tests.yml` and `nightly.yml`.** Pin the action by SHA, as the other workflows do. `make CC=gcc-14` must reach Bazel as `--repo_env=CC=/usr/bin/gcc-14`.
- [ ] **Step 8: Run, in one sitting:** `make clean && make lexer && make verify-core`; `tools/golden_bazel.sh`; `tools/compiler_differential.sh` (now against `build/legacy/bin/goo`). Expected: all green.
- [ ] **Step 9: Update `CLAUDE.md`:** `make lexer` is a wrapper; Bazel through bazelisk is required; `make lexer-legacy` exists until the family migration ends; `NOCACHE=1`.
- [ ] **Step 10: Push; confirm CI with `gh pr view <n> --json statusCheckRollup`** — every `conclusion` SUCCESS. Commit message `build: make Bazel the build of record`.

### Task 9: A lint against symlink escape (spec 6.4 I2)

**Files:**
- Create: `scripts/symlink_escape_lint.sh`
- Create: `scripts/symlink-escape-baseline.txt`
- Modify: `Makefile`

**Interfaces:**
- Produces: `scripts/symlink_escape_lint.sh [--self-test]` — for every script that a Bazel test runs (from `bazel query 'kind("source file", deps(tests(//...)))'` filtered to `*.sh`; `LINT_FILES` overrides the query for the hermetic test), reports each non-comment line that matches `realpath`, `readlink -f`, `pwd -P` or `git rev-parse`, unless the line ends with `# escape-ok: <reason>`. The baseline file holds the existing count per file; the gate is a ratchet: a count may fall, never rise.

- [ ] **Step 1: Write `--self-test`:** a fixture with `realpath` → reported; the same line with `# escape-ok: reason` → not reported; `# escape-ok:` with no reason → reported; a comment line → not reported; an empty file list → exit 2.
- [ ] **Step 2: Run.** Expected: FAIL. Implement. Expected: PASS.
- [ ] **Step 3: Generate the baseline under `LC_ALL=C`.** Read the diffstat before committing it.
- [ ] **Step 4: Wire `symlink-escape-lint`** (allowlist it in parity: it runs a nested `bazel query`). Run `make verify-core`. Commit `feat(gates): ratchet symlink escapes in Bazel-run scripts`.

### Task 10: Proof that the cache hits, and its size (spec 6.4, 8)

**Files:**
- Create: `tools/cache_hit_proof.sh`
- Modify: `.bazelrc` (the size bound)

**Interfaces:**
- Produces: `tools/cache_hit_proof.sh [--self-test]` — creates two scratch worktrees of `HEAD` under `$TMPDIR` (after `unset GIT_DIR`), runs `tools/bazel_local.sh test //... --build_event_json_file=<f>` in the first, then in the second, and requires that every `testResult` in the second run has `cachedLocally` or `cacheHit` true. Prints each miss with its target. Removes both worktrees and their output bases (`bazel clean --expunge`) on exit.

- [ ] **Step 1: Write `--self-test`** on two fixture event files: all cached → pass; one uncached → fail naming it; zero test results → exit 2.
- [ ] **Step 2: Run the proof.** Expected: misses. For each, find the input that differs between the worktrees (an absolute path, an inherited variable, a `stamp`) and fix it in the target or the macro. Repeat until 100%.
- [ ] **Step 3: Review Focus 5.** Start the proof's two runs at the same time against an EMPTY disk cache (`--disk_cache` override to a scratch path). Expected: both exit 0 with equal verdicts.
- [ ] **Step 4: Measure** the disk cache size after one full run from empty (`du -sb`), and the resident memory of five idle Bazel servers before and after `--max_idle_secs`. Set the size bound to twice the measured set. Record the figures in the `.bazelrc` comment.
- [ ] **Step 5: Document** in `CLAUDE.md`: remove a worktree's output base when removing the worktree.
- [ ] **Step 6: Commit** `test(bazel): prove cross-worktree cache hits`. The proof is run by hand and in the Bazel CI job; it is not in verify-core (it runs the whole suite twice).

### Task 11: Spike — the hermetic sandbox (spec 6.4)

Throwaway. No merge. Output is a report.

**Files:**
- Create: `docs/superpowers/specs/2026-11-27-hermetic-sandbox-spike.md`

- [ ] **Step 1: Run** `bazel test //tests/unit/... --experimental_use_hermetic_linux_sandbox` with `--sandbox_add_mount_pair` for `/usr`, `/lib`, `/lib64`, `/bin`, `/etc/alternatives` as needed. Record each failure and the mount that fixed it.
- [ ] **Step 2: Run it for ten probe targets** from `//tests/probes`, including one valgrind gate and one that runs a compiled Goo program.
- [ ] **Step 3: Positive control.** Add a deliberate symlink escape to a scratch test (a script that does `cat "$(dirname "$(realpath "$0")")/../../Makefile"`). It must PASS under the default sandbox and FAIL under the hermetic one. If it passes under both, the spike has shown the flag does not close I2 here.
- [ ] **Step 4: Repeat Step 2 in the CI image** (the `bazel.yml` job, on a draft PR).
- [ ] **Step 5: Write the report:** works / does not, the mount list, the time overhead measured in one sitting against the default sandbox, and a recommendation for the family plan. Commit the report only.

---

## After this plan

Write the family plan (spec sections 6-8) from: Task 0's times, the owner's N, Task 10's hit rate, and Task 11's report. Its first task is `tools/family_parity.sh`; its first family is one that already has Bazel targets.
