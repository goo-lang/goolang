# External Readiness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Goo repository ready for an external reader (IBM Quantum first): true and gated claims, no known silent wrong result in the quantum path, a tidy tree, and a `v0.1.0` GitHub Release that a reader installs and tries in minutes.

**Architecture:** Four parts in three waves. Part 1 (credibility) with Part 4 (the quantum page) and Part 3 (tidy) run in parallel, because they change different files. Part 2 (the release and the quickstart) starts when Parts 1, 3 and 4 are on `main`. Four new gates hold the claims true: `quantum-emit-probe`, `readme-claims-probe`, `md-links-probe` and `quickstart-probe`, each with its self-test.

**Tech Stack:** C23 compiler and runtime (gcc-14 in CI, `-Werror -Wpedantic`), GNU Make gates, bash probe scripts (an embedded `python3 -I` heredoc is allowed for parsing), GitHub Actions, podman, `actions/attest-build-provenance`.

**Spec:** `docs/superpowers/specs/2026-11-27-external-readiness-design.md` (approved 2026-11-27). Also read `docs/superpowers/specs/2026-08-20-toolchain-distribution-design.md` for the tarball layout, the version injection and the gate shape.

## Global Constraints

- Rule 0: every claim in `README.md` and `docs/quantum.md` names a gate in `make verify-core`, or carries the exact label `external, not gated`.
- Do not install libqrmi, qiskit, qir-runner, verilator or circt. Do not submit a live QPU job. Do not download a binary or an installer.
- `main` is protected. Every change is a PR. The 8 required checks are `verify-core`, `bazel test //...`, `header layering and visibility`, `sanitizers (asan)`, `sanitizers (tsan)`, `sanitizers (ubsan)`, `both compilers emit identical IR` and `repro-build`. Merge only on green. Read the verdict with `gh pr view <n> --json statusCheckRollup`, never through a pipe.
- Each PR keeps `make verify-core` green and adds no finding to either findings baseline (`scripts/findings-baseline.gcc14-tidy18-cppcheck2.13.tsv` for CI, `scripts/findings-baseline.gcc16-tidy23-cppcheck2.22.tsv` for the host).
- A new gate commit carries all of these in the same commit: the `--self-test` arm in its script, the probe and its `-selftest` target in `VERIFY_ALL_DEPS`, `.PHONY`, `LC_ALL=C ./tools/probe_census.sh > tests/probes/census.txt`, `python3 tools/gen_probe_targets.py tests/probes/census.txt > tests/probes/generated.bzl`, and `tools/parity-gate-count.txt` raised by 2 (260 on `main` at 2026-11-27).
- Two PRs add gates in parallel (PR-1 and PR-8). The PR that merges second rebases, reads the count on `main`, and adds 2 to that number. Do not add to a stale number.
- A task that adds a golden fixture in `examples/` raises `EXPECT_PASS` in `tools/golden_bazel.sh:22` (517 on `main`) by the number of fixtures.
- C code: C23, `xmalloc`/`xcalloc`/`xrealloc`/`xstrdup` only (`alloc-doors-probe`), every file ends with a newline (clang 18 `-Wnewline-eof`). Build with the default `WERROR`.
- Every user-facing compiler error goes through `diag_emit` with a position (`diag-format-probe`). A `type_error` string in `src/types/*.c` also needs `catalogue/diagnostics.tsv` regenerated (`diagnostics-drift-probe`).
- Shell: read an exit status as `cmd > log 2>&1; echo $?`. Quote every glob. A probe fails on an empty work list.
- A change to `src/parser/parser.y` or `src/parser/lexer_bridge.c`, comments included, runs `./scripts/grammar-tripwire.sh` before and after. Both runs must PASS.
- In a code file (`.c`, `.h`, `.y`, `.goo`, `.go`, `Makefile`, `.sh`), a path edit replaces text in place and keeps the line count. Line numbers in diagnostics and fixtures must not move.
- The README changes only in Part 1, Part 4 and Part 2 tasks. Part 3 tasks do not touch `README.md`.
- Commits: conventional subject, imperative mood, `git -c commit.gpgsign=false commit`, the session attribution line at the end. The pre-commit hook runs `make test`.
- Release decisions (owner, 2026-11-27): `v0.1.0` is a GitHub Release only. No R2, no `dist.goolang.org`, no minisign, no `goop`. Linux x86_64 natively. macOS through one `docker run` or `podman run` command. The first tag is `v0.1.0-rc.1`.

## Review Focus

1. **A qubit id that the simulator cannot hold, in a hardware emit mode.** `qir.QubitAt(20)` under `--emit-qasm3` or `--emit-qir` targets a 156-qubit device and must still compile. The compile-time refusal of Task 4 applies only to a native build. Task 4 adds the test.
2. **A qubit that crosses a function boundary by return, under `--emit-qasm3`.** Reproduced on 2026-11-27: the emitter writes the gates for the wrong qubit and exits 0. Task 3 refuses it and adds the test.
3. **The 17th `qir.Alloc()` and a runtime `qir.ResultAt(n)` with `n >= 64`, in a native build.** Reproduced on 2026-11-27: both alias index 0 and exit 0. Task 5 adds the tests.
4. **The owner's local AI-workflow files after the option-B merge.** A pull of a commit that deletes tracked files also deletes them from the owner's main checkout. Task 11 adds the backup and restore steps.
5. **A clean machine with only the documented prerequisites.** `bin/goo` links `libLLVM`, json-c, curl and zlib dynamically, and it calls `gcc` to link. A missing shared library or a missing `libc6-dev` breaks the quickstart. Task 20 runs the published quickstart in a clean pinned `ubuntu:24.04` container that installs only the documented packages.

---

## Measured inputs (2026-11-27, this worktree, merged with `origin/main` at `c0b64acb`)

### The quantum defects, reproduced

Programs are in `/tmp/claude-1000/-mnt-ssd2-Workspace-github-com-goolang/91a25246-c17e-4366-a39e-6904b4115a86/scratchpad/ready-probe/`.

| Program | Command | Observed | Exit |
|---|---|---|---|
| `w02_rz_const.goo`: `const theta = 0.785398`, `qir.Rz(theta, q)`, `qir.Rz(theta*2.0, q)`, `qir.Rz(0.785398, q)` | `--emit-qasm3` | `rz(0.0) q[0];` twice, then `rz(0.785398) q[0];` | 0 |
| `w02_rz_comptime.goo`: `comptime const PHI float64 = 0.25` | `--emit-qasm3` | `rz(0.0) q[0];` | 0 |
| `w02_rz_var.goo`: `t := 0.5`, `qir.Rz(t, q)` | `--emit-qasm3` | `rz(0.0) q[0];` | 0 |
| `w02_rz_const.goo` | `--emit-qir` | `rz__body(double 7.853980e-01, ...)`, `double f0x3FF921FAFC8B007A`: correct | 0 |
| `w01_qubit16.goo`: `qir.X(qir.QubitAt(16))`, measure qubit 0 | native | `q0: true q16: true` (qubit 16 is qubit 0) | 0 |
| `w01_alloc17.goo`: 17 `qir.Alloc()`, `X` on the 17th, measure the first | native | `true` (the 17th is qubit 0) | 0 |
| `w01_result64.goo`: `qir.Mz(a, qir.ResultAt(64))`, read result 0 | native | `r0: true` (result 64 is result 0) | 0 |
| `w01_result_var.goo`: same with `n := 64` | native | `r0: true` | 0 |
| `w03_ret.goo`: `func fresh() qir.Qubit` returns an allocated qubit after `X` | `--emit-qasm3` | `x q[1];` then every later gate on `q[0]` | 0 |
| `w01_qubit16.goo` | `--emit-qasm3` | `qubit[17] q;`, gates on `q[16]`: correct for hardware | 0 |

An instrumented copy of `qasm3_codegen.c` (scratchpad only, not committed) printed a line at each `0.0` angle and each unresolved qubit. It reported 0 hits on all 8 `examples/qir_*.goo` programs, and it reported hits on `w02` and `w03` (positive control). So the circuits in the README live-QPU table did not use either fallback.

No gate in `make verify-core` reads `--emit-qasm3` or `--emit-qir` output today. `qir-runner-probe` exists, but it needs `qir-runner` and it is not in `VERIFY_ALL_DEPS`.

### Other facts the tasks use

- `make spec-conformance` today: `58/58 rows verified`, `works=50 divergent=1 rejected-by-decision=3 absent=4`, `90% (50/55)`. `docs/GO_SPEC_CONFORMANCE.md` says 47/52 (snapshot 2026-07-27).
- Superset ratchet: `tests/superset/pass-L1.txt` 520, `pass-L2.txt` 180, `pass-L3.txt` 172 programs, of 794 `*.go` files in `third_party/go-test-corpus/go1.27.4/`.
- Already on `main`: `scripts/package_toolchain.sh` (layout, `GOOSTD_PKG_DIRS`, deterministic tar), `make dist`, `release-package-probe` (in `verify-core`), `GOO_VERSION ?= 0.1.0` in `Makefile:294`, and `GOO_VERSION` forwarded by `scripts/podman_build.sh`.
- `bin/goo` links `libLLVM.so.<ver>`, `libjson-c`, `libcurl`, `libz` dynamically. CI and the podman image use Ubuntu 24.04 `llvm-dev` (LLVM 18). The README says "LLVM 22+".
- A tag `v1.0.0` exists on `origin` (2026-07-11), with no GitHub Release. See the questions.
- The QPU project 1 design is draft PR #362 (`docs/qpu-project-1`), https://github.com/goo-lang/goolang/pull/362.
- `CONTRIBUTING.md` and `docs/quantum.md` do not exist. `SECURITY.md` is on `main` (PR #358 merged).

### Tidy references (Part 3)

Reference search: Makefile, `scripts/`, `tools/`, every `BUILD`/`.bzl` file, `.github/`, `.githooks/`. Closure of `verify-core`, `verify`, `verify-nightly` and `test` taken from `make -pq` (positive control: `goo-test-probe` and `gpu-kernel-reject-probe` are in it).

| Directory | Build references | Used by a live target | Decision |
|---|---|---|---|
| `ide/` (6 files, VS Code extension) | 0 | no | move to `attic/ide/` |
| `kernel/` (9 files) | 0 in build files. `scripts/safety-scan.sh:14,24` default `SCAN_DIRS`, `docs/SAFETY_SCAN.md:25` | no (`make safety` is local-only, `kernel/` has no C, the baseline has 0 `kernel` entries) | move to `attic/kernel/` |
| `templates/` (6 files) | 1: `tools/project_wizard/main.c:156`, built only by `wizard` (`Makefile:4981-4996`) | no (`wizard` and `dev-tools` are outside every gate closure and every workflow) | move to `attic/templates/` with `tools/project_wizard/` |
| `testing/` (14 files) | 45 path hits and 25 `//testing` labels. `tools/verify_layering.sh`, `tools/verify_probe_teeth.sh`, `tools/verify_sanitizers.sh`, run by `.github/workflows/bazel.yml:86,197,252` | yes (required checks) | stays |

The spec quotes 104, 5, 1 and 22 hits. Those counts include plain words ("kernel", "testing", "side") that are not paths.

Root files: the nine `test_*.c`, `performance_report.txt` and `demo-project/` have 0 references in any build file. Prose mentions only: `docs/superpowers/specs/2026-08-20-package-manager-design.md:34,54,62,251` (dated design record, left as is).

---

## PR map and order

| PR | Tasks | Waits for | Model mix |
|---|---|---|---|
| PR-1 `fix(qasm3)` | 1, 2, 3 | none | sonnet |
| PR-2 `fix(qir-sim)` | 4, 5 | none | sonnet, haiku |
| PR-3 `docs(spec)` conformance refresh | 6 | none | haiku |
| PR-4 `docs(readme)` | 7, 8 | PR-1, PR-2, PR-3 | sonnet |
| PR-5 `docs(quantum)` | 9 | PR-4 | sonnet |
| PR-6 `chore: root and AI files` | 10, 11 | none | haiku |
| PR-7 `chore: attic` | 12 | none | haiku |
| PR-8 `docs: history and links gate` | 13, 14, 15 | PR-6, PR-7 | sonnet, haiku |
| PR-9 `docs: CONTRIBUTING` | 16 | none | haiku |
| (owner action) | 17 | none | none |
| PR-10 `feat(release): quickstart gate` | 18, 19 | PR-1 to PR-9 | sonnet |
| PR-11 `ci(release)` | 20 | PR-10 | sonnet |
| (tag runs) | 21 | PR-11, owner approval | haiku |
| PR-12 `ci: drop the Microsoft apt source` (OPTIONAL) | 22 | owner approval | haiku |

Wave A, in parallel: PR-1, PR-2, PR-3, PR-6, PR-7, PR-9, and PR-12 if approved. These PRs change different files, except the gate-count files of PR-1 (see Global Constraints).
Wave B: PR-4 and PR-8, in parallel. Wave C: PR-5. Wave D: PR-10, then PR-11, then Task 21.
Inside one PR, run the tasks in order.

Shared files that two PRs change: `Makefile` (PR-1, PR-4, PR-7, PR-8, PR-10, at different hunks), `tools/parity-gate-count.txt` and `tests/probes/census.txt` (PR-1, PR-4, PR-8, PR-10), `CLAUDE.md` (PR-6 then PR-8), `attic/README.md` and `docs/SAFETY_SCAN.md` (PR-7 then PR-8). The order above keeps each pair sequential, except the gate-count files.

---

## Part 1: credibility

### Task 1: `quantum-emit-probe`, a gate on QASM3 and QIR output (PR-1, sonnet)

Pin the current output of the 8 quantum examples before Tasks 2 and 3 change the emitter. The measurement above found no fallback in these 8 programs, so their current output is the reference.

**Files:**
- Create: `scripts/quantum_emit_probe.sh`
- Create: `tests/quantum_emit/<name>.qasm` for each `examples/qir_*.goo` (8 files)
- Modify: `Makefile` (two targets, `VERIFY_ALL_DEPS`, `.PHONY`), `tests/probes/census.txt`, `tests/probes/generated.bzl`, `tools/parity-gate-count.txt`

**Interfaces:**
- Produces: `scripts/quantum_emit_probe.sh [--self-test]`. Environment: `QE_ROOT` (default `git rev-parse --show-toplevel`), `GOO_BIN` (default `$QE_ROOT/bin/goo`), `QE_DIR` (default `$QE_ROOT/tests/quantum_emit`).
- Produces the fixture contract that Tasks 2 and 3 use:
  - `QE_DIR/<name>.qasm`: the source is `QE_DIR/<name>.goo` if it exists, else `examples/<name>.goo`. `goo --emit-qasm3 <src> -o <tmp>` must exit 0 and the output must equal the `.qasm` file byte for byte.
  - `QE_DIR/<name>.err.txt`: the source is `QE_DIR/<name>.goo`. `--emit-qasm3` must exit non-zero, must not create the `-o` file, and stderr must contain the `.err.txt` text.
  - QIR smoke, for each source of a `.qasm` fixture: `goo --emit-qir <src> -o <tmp>.ll` exits 0 and the file contains `"qir_profiles"="adaptive_profile"` and `"entry_point"`. QIR text depends on the LLVM version, so the probe does not compare it byte for byte.
- Produces: Make targets `quantum-emit-probe` and `quantum-emit-selftest`, both with prerequisites `$(COMPILER) $(RUNTIME_LIB)`.

- [ ] **Step 1: Write the self-test arm first.** `--self-test` builds a temporary `QE_DIR` and asserts each of these, by exit status and by the named FAIL text:
  - control: one copied `.qasm` fixture of `qir_teleport_probe` gives `quantum-emit-probe: PASS`.
  - a changed byte in the `.qasm` gives `FAIL (<name>: qasm differs)`.
  - a stub `GOO_BIN` that exits 0 and writes nothing gives `FAIL (<name>: no output file)`.
  - an `.err.txt` fixture whose program compiles gives `FAIL (<name>: compiled, expected a refusal)`.
  - an empty `QE_DIR` gives `FAIL (no fixtures)`.
  - a stub QIR output without `"qir_profiles"="adaptive_profile"` gives `FAIL (<name>: qir profile attribute missing)`.
  Each mutation is checked to have landed before the probe runs on it.
- [ ] **Step 2: Run the self-test and see it fail.** Run: `bash scripts/quantum_emit_probe.sh --self-test > build/qe_st.log 2>&1; echo $?`. Expected: non-zero, because the check mode does not exist yet.
- [ ] **Step 3: Implement the check mode** per the contract above. Each compile has a 20-second `timeout`. The summary line is `quantum-emit-probe: PASS (<n> qasm, <m> refusals, <k> qir)`.
- [ ] **Step 4: Record the 8 references.** For each `examples/qir_*.goo`, write `bin/goo --emit-qasm3 examples/<name>.goo -o tests/quantum_emit/<name>.qasm`. Read each file once: the qubit count must match the program, and no line may be `rz(0.0)`.
- [ ] **Step 5: Run both modes.** Run `make quantum-emit-probe` and `make quantum-emit-selftest`. Expected: `quantum-emit-probe: PASS (8 qasm, 0 refusals, 8 qir)` and the self-test PASS line.
- [ ] **Step 6: Wire the gate** (Global Constraints: `VERIFY_ALL_DEPS`, census, `generated.bzl`, parity count). Run `make verify-core > build/vc.log 2>&1; echo $?`. Expected: `0` and `verify-core: ALL GREEN GATES PASSED (ccomp-free)` as the last line.
- [ ] **Step 7: Commit.** `test(qasm3): pin QASM3 and QIR output with quantum-emit-probe`

### Task 2: constant rotation angles in `--emit-qasm3` (PR-1, sonnet)

The fix: compute a constant angle at compile time. Refuse any angle that is not a constant, with a positioned diagnostic. Never write `0.0` for an angle that the source does not give as zero.

**Files:**
- Modify: `src/types/expression_helpers.c`, `include/types.h` (one declaration next to `goo_fold_const_int_ctx`, line 1226)
- Modify: `src/codegen/qasm3_codegen.c` (`qasm3_format_float` at line 178, the `Rx`/`Ry`/`Rz` branch at line 272, `Qasm3Ctx`, `codegen_emit_qasm3`)
- Create: `tests/quantum_emit/rz_named_const.goo` + `.qasm`, `rz_const_expr.goo` + `.qasm`, `rz_comptime_const.goo` + `.qasm`, `rz_negative_const.goo` + `.qasm`, `rz_runtime_angle_reject.goo` + `.err.txt`

**Interfaces:**
- Consumes: the Task 1 fixture contract.
- Produces: `int goo_fold_const_float_ctx(TypeChecker* checker, ASTNode* expr, double* out);` returns 1 and sets `*out` for a float literal, an integer literal, an identifier bound to a `const` or `comptime const` with a foldable initializer, unary `-` and `+`, binary `+ - * /`, and parentheses. It returns 0 for anything else. Mirror how `goo_fold_const_int_ctx` resolves a named constant.
- Produces: `int failed;` in `Qasm3Ctx`, and `static void qasm3_refuse(Qasm3Ctx* ctx, Position pos, const char* fmt, ...)`. `qasm3_refuse` sends a `DIAG_ERROR` through `diag_emit` at `pos` and sets `failed`. When `failed` is set, `codegen_emit_qasm3` writes no output file and returns 0. Task 3 uses both.

- [ ] **Step 1: Write the fixtures.** Each `.goo` allocates one qubit, applies the rotation, measures, and consumes the qubit (copy the shape of `w02_rz_const.goo`). Expected lines in the `.qasm` files:
  - `rz_named_const`: `const theta = 0.785398`, `qir.Rz(theta, q)` gives `rz(0.785398) q[0];`
  - `rz_const_expr`: `qir.Rz(theta*2.0, q)` gives `rz(1.570796) q[0];`
  - `rz_comptime_const`: `comptime const PHI float64 = 0.25`, `qir.Rz(PHI, q)` gives `rz(0.25) q[0];`
  - `rz_negative_const`: `qir.Rz(-theta/2.0, q)` gives `rz(-0.392699) q[0];`
  - `rz_runtime_angle_reject`: `t := 0.5`, `qir.Rz(t, q)`. `.err.txt` holds `qir.Rz angle must be a compile-time constant for --emit-qasm3`.
  Write the rest of each `.qasm` file from the header and measure lines that the emitter writes today for the same circuit.
- [ ] **Step 2: Run the gate and see it fail.** Run `make quantum-emit-probe`. Expected: FAIL on `rz_named_const` (the output has `rz(0.0)`).
- [ ] **Step 3: Implement `goo_fold_const_float_ctx`.**
- [ ] **Step 4: Implement the angle path in `qasm3_codegen.c`.** Replace `qasm3_format_float` with `static int qasm3_format_angle(Qasm3Ctx* ctx, ASTNode* expr, char* buf, size_t n)`. A bare literal keeps its source text, so the arq_canary output stays byte-identical. A folded value prints with the shortest `%.<p>g`, `p` from 1 to 17, that reads back to the same `double`. When folding fails, call `qasm3_refuse` at the angle argument's position, with the gate name (`qir.Rx`, `qir.Ry` or `qir.Rz`) in the message.
- [ ] **Step 5: Run the gates.** Run `make quantum-emit-probe`, `make diag-format-probe` and `make test-golden`. Expected: PASS, with `5 qasm` fixtures more and `1 refusals` in the summary.
- [ ] **Step 6: Run `make verify-core`.** Expected: exit 0 and the ALL GREEN line.
- [ ] **Step 7: Commit.** `fix(qasm3): fold constant rotation angles and refuse a runtime angle`

### Task 3: refuse an unresolved qubit or result in `--emit-qasm3` (PR-1, sonnet)

This defect was found while planning (program `w03_ret.goo`). The controller confirms the scope (see the questions). Today an unknown name becomes index 0 at `qasm3_codegen.c:263`, `:280`, `:480`, and the `Mz` branch at `:290-296` skips silently. `FreeZero` and `MeasureReset` skip silently when the name is unknown.

**Files:**
- Modify: `src/codegen/qasm3_codegen.c` (each `qasm3_find_var` call site that reads a qubit or a result: lines 254, 262, 279, 291, 295, 328, 449, 479)
- Create: `tests/quantum_emit/qubit_from_helper_reject.goo` + `.err.txt`

**Interfaces:**
- Consumes: `qasm3_refuse` and `Qasm3Ctx.failed` from Task 2.

- [ ] **Step 1: Write the fixture.** Copy `w03_ret.goo`. The `.err.txt` holds `--emit-qasm3 cannot resolve qubit 'b' to a hardware index`.
- [ ] **Step 2: Run `make quantum-emit-probe` and see it fail.** Expected: `FAIL (qubit_from_helper_reject: compiled, expected a refusal)`.
- [ ] **Step 3: Implement.** At each listed site, when the qubit or result name does not resolve, call `qasm3_refuse` at the argument position: `--emit-qasm3 cannot resolve qubit '%s' to a hardware index` or `... result '%s' ...`. Remove every `: 0` fallback for a qubit or a result index.
- [ ] **Step 4: Run the gates.** Run `make quantum-emit-probe`. Expected: PASS, and the 8 example references unchanged. A changed example reference means that the example used a fallback: stop and report it, because a README live-QPU row depends on that circuit.
- [ ] **Step 5: Run `make verify-core`.** Expected: exit 0.
- [ ] **Step 6: Commit.** `fix(qasm3): refuse a qubit or result that does not resolve`

### Task 4: refuse a simulator-range qubit or result id at compile time, in a native build (PR-2, sonnet)

A native build links the embedded simulator, which has 16 qubits and 64 results. A hardware emit mode does not link it. `qir.QubitAt` already requires a constant id (`src/types/qir_ownership.c:313`), so a native build can refuse every out-of-range `QubitAt` at compile time.

**Files:**
- Modify: `include/runtime.h` (two macros), `include/codegen.h` (one field next to `emit_qir` at line 303), `src/compiler/goo.c` (set the field next to `codegen->emit_qir` at line 1562), `src/codegen/call_codegen.c` (the `QubitAt`/`ResultAt` branch at lines 2433-2447)
- Modify: `src/codegen/BUILD` only if the target that compiles `call_codegen.c` lacks `//include:runtime_h`
- Create: `tests/golden/reject/qir_qubit_at_sim_range_reject.goo` + `.err.txt`, `tests/golden/reject/qir_result_at_sim_range_reject.goo` + `.err.txt`
- Create: `tests/quantum_emit/qubit_at_20.goo` + `.qasm` (the hardware-mode control)

**Interfaces:**
- Produces: `#define GOO_QIR_SIM_MAX_QUBITS 16` and `#define GOO_QIR_SIM_MAX_RESULTS 64` in `include/runtime.h`. Task 5 uses them in `qir_sim.c`.
- Produces: `int qir_sim_linked;` in `CodeGenerator`. `goo.c` sets it to 1 when the output is an executable (`goo build`, `goo run`, `goo test`), and to 0 for `--emit-llvm`, `--emit-qir`, `--emit-qasm3`, `--emit-mlir` and `--emit-sv`.

- [ ] **Step 1: Write the fixtures.**
  - `qir_qubit_at_sim_range_reject.goo`: `b := qir.QubitAt(16)`, then consume `b`. `.err.txt`: `qir.QubitAt(16) is out of range for the embedded simulator (16 qubits)`.
  - `qir_result_at_sim_range_reject.goo`: `r := qir.ResultAt(64)`. `.err.txt`: `qir.ResultAt(64) is out of range for the embedded simulator (64 results)`.
  - `qubit_at_20.goo` + `.qasm`: `qir.QubitAt(20)`, `X`, measure, consume. The `.qasm` declares `qubit[21] q;`.
- [ ] **Step 2: Run the gates and see them fail.** Run `make test-golden-reject`. Expected: FAIL on both new fixtures (they compile today). Run `make quantum-emit-probe`. Expected: PASS (the hardware mode is correct today, and it must stay correct).
- [ ] **Step 3: Implement.** In the `QubitAt`/`ResultAt` branch, after `goo_fold_const_int_ctx` succeeds, refuse with `codegen_error` at the argument position when `codegen->qir_sim_linked` is set and the id is at or above the limit. The message adds: `; use --emit-qasm3 or --emit-qir for a larger device`.
- [ ] **Step 4: Run the gates.** Run `make test-golden-reject`, `make quantum-emit-probe`, `make diag-format-probe`, `make test-golden`. Expected: all PASS.
- [ ] **Step 5: Run `make verify-core`.** Expected: exit 0.
- [ ] **Step 6: Commit.** `fix(qir): refuse a simulator-range qubit or result id at compile time`

### Task 5: runtime range checks in the simulator (PR-2, haiku; move to sonnet if haiku fails)

**Files:**
- Modify: `src/runtime/qir_sim.c` (`qir_sim_qubit_index` line 56, `qir_sim_result_index` line 63, `__quantum__rt__qubit_allocate` line 90), `src/runtime/BUILD` (add `deps = ["//include:runtime_h"]` to the `qir_sim` target at line 172)
- Create: `examples/qir_alloc_exhausted_abort_probe.goo` + `.expected.txt` + `.exit` + `.stderr.txt`, `examples/qir_result_index_abort_probe.goo` + `.expected.txt` + `.exit` + `.stderr.txt`
- Modify: `tools/golden_bazel.sh:22` (`EXPECT_PASS` 517 to 519)

**Interfaces:**
- Consumes: `GOO_QIR_SIM_MAX_QUBITS`, `GOO_QIR_SIM_MAX_RESULTS` from Task 4, and `goo_panic` from `include/runtime.h` (prints `panic: <message>`, exits 2).

- [ ] **Step 1: Write the fixtures.** Use the `.exit` and `.stderr.txt` sidecar rules of `scripts/run_golden.sh`.
  - `qir_alloc_exhausted_abort_probe.goo`: copy `w01_alloc17.goo`. Print one line before the 17th `Alloc`. `.expected.txt` holds that line. `.exit` holds `2`. `.stderr.txt` holds `panic: qir: qubit allocation failed: all 16 simulator qubits are in use`.
  - `qir_result_index_abort_probe.goo`: copy `w01_result_var.goo` (a runtime index 64). `.exit` holds `2`. `.stderr.txt` holds `panic: qir: result index 64 is out of range (the embedded simulator has 64 results)`.
- [ ] **Step 2: Run `make test-golden` and see both fail.** Expected: both exit 0 today, so the `.exit` check fails.
- [ ] **Step 3: Implement.** Replace the two `& (MAX - 1u)` masks with a range check that calls `goo_panic`. The qubit-index message is `qir: qubit index %u is out of range (the embedded simulator has 16 qubits)`. Goo source cannot reach that check after Task 4, so it is a backstop for IR built in another way. Replace the `return (void*)(uintptr_t)0;` at the end of `__quantum__rt__qubit_allocate` with the allocation panic. Use the shared macros for both limits.
- [ ] **Step 4: Run the gates.** Run `make test-golden` and `make test-golden-o2`. Expected: PASS, two fixtures more than before.
- [ ] **Step 5: Run `make verify-core`.** Expected: exit 0.
- [ ] **Step 6: Commit.** `fix(qir-sim): panic on an exhausted register or an out-of-range result`

### Task 6: refresh `docs/GO_SPEC_CONFORMANCE.md`, and gate its headline (PR-3, haiku)

**Files:**
- Modify: `scripts/run_spec_conformance.sh`, `docs/GO_SPEC_CONFORMANCE.md`

**Interfaces:**
- Produces: the headline format `(<works> works / <tested> tested constructs, excluding <rejected> deliberate rejections)`, which `spec-conformance` now checks.

- [ ] **Step 1: Add the check.** After the summary, `run_spec_conformance.sh` greps `docs/GO_SPEC_CONFORMANCE.md` for `(<works> works / <tested> tested constructs, excluding <rejected> deliberate rejections)` with the numbers of this run. A miss prints `spec-conformance: FAIL (docs/GO_SPEC_CONFORMANCE.md headline does not match this run)` and exits 1.
- [ ] **Step 2: Run `make spec-conformance` and see it fail.** Expected: the FAIL line, because the document says 47/52.
- [ ] **Step 3: Update the document.** The snapshot line gets today's date and `**90% conformance** (50 works / 55 tested constructs, excluding 3 deliberate rejections)`. Update each construct row that changed since 2026-07-27. Find them with `git log -p --since=2026-07-27 -- tests/spec/manifest.tsv`.
- [ ] **Step 4: Run `make spec-conformance`.** Expected: `58/58 rows verified, 0 drifted` and no FAIL line.
- [ ] **Step 5: Commit.** `docs(spec): refresh the conformance matrix and gate its headline`

### Task 7: `readme-claims-probe` (PR-4, sonnet)

**Files:**
- Create: `scripts/readme_claims_probe.sh`
- Modify: `Makefile`, `tests/probes/census.txt`, `tests/probes/generated.bzl`, `tools/parity-gate-count.txt`

**Interfaces:**
- Produces: `scripts/readme_claims_probe.sh [--self-test]`. Environment: `CLAIMS_ROOT` (default the git toplevel), `GOO_BIN`. Make targets `readme-claims-probe` and `readme-claims-selftest`.
- Produces the Markdown contract that Tasks 8, 9 and 19 write to:
  - Scanned files: each tracked `*.md` with a line that is exactly `<!-- claims:begin -->`. A marker inside other text (for example, in backticks in this plan) does not count. Without `.git` (the self-test tree), scan with `find`. `README.md` must have a claims block, or the probe fails.
  - A claims block is the lines between `<!-- claims:begin -->` and `<!-- claims:end -->`. Each table row counts, except the header row and the `|---` separator row. The header's last cell must be `Gate`.
  - A Gate cell holds one of two forms:
    - The exact text `external, not gated`. The same row must not hold `✅` or the word `Verified`.
    - One or more backticked tokens, joined only by `,`, `+` or spaces. Each token is either a name in the core gate set, or a repository path or glob that matches at least one tracked file under `examples/` or `tests/`. The core gate set is the output of `./tools/parity.sh --list-make-gates`, minus the names in the `HEAVY_DEPS :=` line of the `Makefile`.
  - The superset block is the text between `<!-- superset:begin -->` and `<!-- superset:end -->` in `README.md`. It must hold exactly four integers, in this order: the line counts of `tests/superset/pass-L1.txt`, `pass-L2.txt` and `pass-L3.txt` (non-empty lines that do not start with `#`), then the count of `*.go` files in `third_party/go-test-corpus/go1.27.4/`.
  - A fenced ```` ```goo ```` block in a scanned file, with `package main` as its first non-blank line, must compile with `$GOO_BIN -o <tmp>/x <tmp>/prog.goo` from `CLAIMS_ROOT`. The binary must exit 0. If the next non-blank line after the block is `Output:` and a ```` ```text ```` block follows, stdout must equal that block.
  - Zero claim rows in total is a FAIL.

- [ ] **Step 1: Write the self-test.** Each case builds a temporary tree with a `README.md`, a stub `Makefile` `HEAVY_DEPS` line and a stub gate list, then asserts the exit status and the FAIL text:
  - control: a valid table, superset block and goo block give PASS.
  - an unknown gate token gives `FAIL (README.md:<line>: unknown gate or path 'no-such-probe')`.
  - an empty Gate cell gives `FAIL (README.md:<line>: empty Gate cell)`.
  - a heavy-only gate (`repro-build-probe`) gives `FAIL (... 'repro-build-probe' is not in verify-core)`.
  - an `external, not gated` row with `✅` gives `FAIL (... external row claims verified)`.
  - a superset number one off gives `FAIL (README.md: superset numbers ... do not match the ratchet ...)`.
  - a goo block that does not compile gives `FAIL (README.md:<line>: goo block does not compile)`.
  - no claims block gives `FAIL (README.md has no claims block)`.
- [ ] **Step 2: Run the self-test and see it fail** (no check mode yet).
- [ ] **Step 3: Implement the check mode** per the contract. The summary line is `readme-claims-probe: PASS (<rows> rows, <gates> gate names, <paths> paths, <blocks> goo blocks, <files> files)`.
- [ ] **Step 4: Run the self-test.** Expected: `readme-claims-selftest: PASS`. Do not wire the gate yet: today's `README.md` has no claims block, and the check mode fails on it. Task 8 wires it.
- [ ] **Step 5: Commit.** `test(readme): add readme-claims-probe (not yet wired)`

### Task 8: rewrite `README.md` against its gates (PR-4, sonnet)

This task does every item of coord task `goolang:Doc-readme-truth`.

**Files:**
- Modify: `README.md`, `Makefile` (wire Task 7), `tests/probes/census.txt`, `tests/probes/generated.bzl`, `tools/parity-gate-count.txt`

**Interfaces:**
- Consumes: the Task 7 contract, `quantum-emit-probe` (Task 1), the Task 4 and Task 5 fixtures, and the Task 6 numbers.
- Produces: the anchors `#what-goo-adds-to-go`, `#go-compatibility`, `#quantum` and `#live-hardware-runs`, and the claims block. Tasks 9 and 19 link to these anchors.

- [ ] **Step 1: Write the claims table** (`| Capability | Syntax | Gate |`) inside the markers. Use these rows. For each row, open the recipe or script of each gate and confirm that it exercises the claim. Drop a row that no gate covers, or move it to a "Not yet verified" list.

  | Capability | Gate (to confirm) |
  |---|---|
  | Error unions `!T`, `try`, `catch` | `erru-catch-probe`, `erru-error-probe`, `examples/erru_*.goo` |
  | Nullable types `?T`, `if let` | `nullable-iflet-probe`, `nullable-nilcmp-probe` |
  | `comptime` evaluation and value specialisation | `comptime-probe`, `comptime-block-probe`, `comptime-value-reject-matrix` |
  | Inference-only generics | `generics-reject-probe`, `examples/gen_*.goo` |
  | ARC with escape-analysis elision | `arc-release-probe`, `escape-teeth`, `release-decision-teeth` |
  | `arena {}` regions (the narrow reach that `CLAUDE.md` states) | `arena-free-probe` |
  | Goroutines, channels, `select` | `chan-probe`, `select-probe`, `deadlock-goroutine-probe` |
  | `goostd/lanes` (`Partition`, `Run`, `StencilStep`, `RunFar`) | `stencil-parallel-probe`, `far-stencil-r2-probe` |
  | Linear `qir.Qubit` ownership (five rules) | `test-golden-reject`, `tests/golden/reject/qir_*_reject.goo` |
  | Embedded 16-qubit simulator, range-checked | `test-golden`, `test-golden-o2`, `examples/qir_*.goo`, `tests/golden/reject/qir_*_sim_range_reject.goo` |
  | OpenQASM 3 output (constant angles) and QIR output (structure only) | `quantum-emit-probe` |
  | FPGA path: `-O2` output equals a Python model of `--emit-sv` (no RTL simulator) | `fpga-equiv-probe` |
  | `goo test` | `goo-test-probe` |

- [ ] **Step 2: Write the Go compatibility section** (`## Go compatibility`). Remove the "Strict Go Superset ... compile unmodified" claim. Put the corpus name `go1.27.4` outside the markers, then write: `<!-- superset:begin -->bin/goo parses at least 520, compiles at least 180 and runs at least 172 of the 794 programs<!-- superset:end -->`, with the gate name `superset-probe`. Add a link to `docs/GO_SPEC_CONFORMANCE.md` with the Task 6 headline numbers.
- [ ] **Step 3: Fix the quantum section.** Rule 5 says that a fixed local register `[N]qir.Qubit` (N from 1 to 16) with constant indices is allowed (`examples/qir_array_register_probe.goo`, `tests/golden/reject/qir_array_runtime_index_reject.goo`). Remove `lanes.Map`, `owned` / `borrowed`, and the "Laptop: AerSimulator" row (Qiskit is external).
- [ ] **Step 4: Rewrite the backends table as `## Live hardware runs`.** Keep every row. Add a last column `Gate`. Rows that run on `qir_sim.c` name `test-golden` and the example path. Every other row (Aer, the `Fake*` snapshots, IBM Heron, Quantinuum) carries `external, not gated`, with no `✅` and no bold "Verified". Under the table, write that Qiskit transpiled each circuit to the backend before submission. Also write that rows with the same job ID were one batched job: `dc2n70inj2vs73dh0mag` (3 programs), `dc2o7nts2omc738cabp0` (3 programs) and `a867b5c5-20f2-45e3-acc8-6f67e88cf24a` (2 programs). Label the submission scripts `external, not gated`.
- [ ] **Step 5: Fix the rest.** Remove the count badges ("187 Probes", "497 Goldens", "165 Rejects") and the "LLVM 22+" badge. State the build prerequisites that CI uses: gcc-14 and the Ubuntu 24.04 `llvm-dev` (LLVM 18). Read the version from the `Tool versions` step of a recent `tests.yml` run. Remove the counts from the verification section. Update the repository layout. Keep the three Language Tour goo blocks, with an `Output:` block after each one (`listening on port: 8443`, `1 3 2`, `42`).
- [ ] **Step 6: Wire `readme-claims-probe`** (Global Constraints). Run `make readme-claims-probe`. Expected: PASS with at least 13 rows and 3 goo blocks.
- [ ] **Step 7: Run `make verify-core`.** Expected: exit 0.
- [ ] **Step 8: Commit.** `docs(readme): make every claim name its gate`
- [ ] **Step 9: When PR-4 merges, close `goolang:Doc-readme-truth`** with the `coord-claim` skill (release, task done, PR link). Controller action.

## Part 4: the quantum page

### Task 9: `docs/quantum.md` (PR-5, sonnet)

**Files:**
- Create: `docs/quantum.md`
- Modify: `README.md` (one link to `docs/quantum.md` in the quantum section)

**Interfaces:**
- Consumes: the Task 7 contract (the page has its own claims block, so `readme-claims-probe` scans it with no change), the README anchor `#live-hardware-runs`, and PR #362.

- [ ] **Step 1: Write the page** with the six sections of the spec, Part 4:
  1. What Goo gives QPU work: the five ownership rules, one language for the circuit and the classical decoder (ARC, no tracing collector), QIR and OpenQASM 3 output.
  2. What is measured: a claims block. Use one row for each ownership rule, with its fixture: rule 1 `qir_use_after_move_reject`, rule 2 `qir_alias_cx_reject`, rule 3 `qir_unconsumed_scope_reject`, rule 4 `qir_branch_mismatch_reject` + `qir_loop_consume_reject`, rule 5 `qir_closure_capture_reject` (each under `tests/golden/reject/`). Add rows for the simulator runs, the simulator range checks, QASM3 output (`quantum-emit-probe`) and QIR structure (`quantum-emit-probe`).
  3. What ran on real hardware: a link to `README.md#live-hardware-runs`, with the label `external, not gated` and the Qiskit transpilation statement.
  4. What is not done: no native submission (the submission scripts use Qiskit and the Azure SDK), no routing or layout, 16 qubits and 64 results in the simulator, constant rotation angles only in `--emit-qasm3`, a qubit must resolve in the emitter, no QIR tool validates the output in a gate (`qir-runner-probe` is outside `verify-core`), no RTL simulation on the FPGA path.
  5. The roadmap: QPU project 1, scope A, with a link to https://github.com/goo-lang/goolang/pull/362 (a draft under review).
  6. What a partnership could add: API and backend access for a pinned backend description, a review of the native-gate translation, recorded responses for offline gates, and a backend to test against.
- [ ] **Step 2: Run `make readme-claims-probe`.** Expected: PASS, and the file count in the summary goes from 1 to 2.
- [ ] **Step 3: Read each prose claim outside the table** against its source. The probe does not see prose.
- [ ] **Step 4: Run `make verify-core`.** Expected: exit 0.
- [ ] **Step 5: Commit.** `docs(quantum): add the partner page for QPU work`

## Part 3: tidy

### Task 10: remove the root clutter (PR-6, haiku)

**Files:**
- Delete: `test_annotation_minimal.c`, `test_annotation_system.c`, `test_auto_parallel_simple.c`, `test_dependency_analysis.c`, `test_loop_transformation.c`, `test_performance_models.c`, `test_simd_vectorization.c`, `test_simple_transformation.c`, `test_task_parallelism.c`, `performance_report.txt`, `demo-project/`

- [ ] **Step 1: Prove that nothing names them** (with a positive control). Run `git grep -n -F -e test_task_parallelism.c -e performance_report.txt -e demo-project -- Makefile scripts tools .github '*BUILD*' '*.bzl'`. Expected: no line. Then run the same command for `release_package_probe.sh`. Expected: at least one line.
- [ ] **Step 2: Remove the files.** Use `git rm -r` on the 11 paths.
- [ ] **Step 3: Run `make verify-core`.** Expected: exit 0.
- [ ] **Step 4: Commit.** `chore: remove the unused root test sources and demo project`

### Task 11: option B for the AI-workflow files, and the `CLAUDE.md` cut (PR-6, haiku)

**Files:**
- Untrack: `.handoff.md`, `.superpowers/` (1 tracked file), `.coord/` (3 files), `.taskmaster/` (22 files). Keep `.claude/skills/` (3 files) in git.
- Modify: `.gitignore`, `CLAUDE.md`

- [ ] **Step 1: Untrack the files.** Run `git rm -r --cached .handoff.md .superpowers .coord .taskmaster`. Do not delete them from disk.
- [ ] **Step 2: Update `.gitignore`.** Add `/.handoff.md`, `/.coord/`, `/.taskmaster/`, `/.claude/*` and `!/.claude/skills/`. Remove the obsolete `.taskmaster/state.json` line. `.superpowers/` is already there.
- [ ] **Step 3: Verify.** Run `git status --short --ignored .claude .taskmaster .coord .handoff.md .superpowers`. Expected: the four untracked paths show as ignored (`!!`), and `.claude/skills/` stays tracked (`git ls-files .claude` lists 3 files).
- [ ] **Step 4: Cut `CLAUDE.md` lines 20-70** (`### Task Master Commands` through the `### Quick Start Workflow` list). Keep the status disclaimer (lines 13-18). Remove the `.taskmaster/` line in the project structure list (line 336).
- [ ] **Step 5: Run `make test`.** Expected: PASS.
- [ ] **Step 6: Commit.** `chore: keep the AI-workflow files out of git (option B)`
- [ ] **Step 7: Owner runbook, before the owner pulls PR-6 into the main checkout.** Copy the files aside: `cp -a .handoff.md .coord .taskmaster .superpowers <backup-dir>/`. Pull. Then copy them back. They are ignored now, so they stay local. Each other worktree that merges `main` loses the same files: restore them there in the same way, or from git with `git archive <PR-6 parent SHA> .handoff.md .coord .taskmaster | tar -x`. Put this runbook in the PR body.

### Task 12: move the dead directories to `attic/` (PR-7, haiku)

**Files:**
- Move: `ide/` to `attic/ide/`, `kernel/` to `attic/kernel/`, `templates/` to `attic/templates/`, `tools/project_wizard/` to `attic/tools/project_wizard/`
- Modify: `Makefile:4981` (delete `PROJECT_WIZARD`), `Makefile:4989` (remove `wizard` from `dev-tools`), `Makefile:4991-4996` (delete the `wizard` target and rule), and any `.PHONY` entry for `wizard`
- Modify: `scripts/safety-scan.sh:14` and `:24` (default `SCAN_DIRS` becomes `src include lib`)
- Modify: `docs/SAFETY_SCAN.md:25` (remove `kernel/`)
- Modify: `docs/adr/0006-toolchain-distribution-and-package-ecosystem.md:31` and `docs/superpowers/specs/2026-08-20-package-manager-design.md:34` (`tools/project_wizard/main.c` becomes `attic/tools/project_wizard/main.c`)
- Modify: `attic/README.md` (one entry for each moved directory: what it is, why it moved, the date)

- [ ] **Step 1: Move with `git mv`.**
- [ ] **Step 2: Edit the references** listed above.
- [ ] **Step 3: Prove that no old path remains** (with a positive control). Run `git grep -n -E '(^|[^a-z/])(ide/vscode|kernel/(build|memory|runtime|platform|interrupts)|templates/(cli_tool|microservice|web_api)|tools/project_wizard)' -- . ':!attic'`. Expected: no line. Run the same pattern with `':!attic'` removed. Expected: hits inside `attic/`.
- [ ] **Step 4: Run `make verify-core`** and `bash scripts/safety-scan.sh --check-baseline`. Expected: exit 0 and `safety-baseline-check: PASS`.
- [ ] **Step 5: Commit.** `chore: move ide, kernel, templates and the wizard to attic`

### Task 13: `md-links-probe` (PR-8, sonnet)

**Files:**
- Create: `scripts/md_links_probe.sh`
- Modify: `docs/SAFETY_SCAN.md:4` (the link `(../../semgrep-competitor)` points outside the repository: make it plain text)
- Modify: `Makefile`, `tests/probes/census.txt`, `tests/probes/generated.bzl`, `tools/parity-gate-count.txt`

**Interfaces:**
- Produces: `scripts/md_links_probe.sh [--self-test]`, environment `MDL_ROOT`. Make targets `md-links-probe` and `md-links-selftest`.
- Rules: scan each tracked `*.md` file except under `attic/` and `third_party/`. Remove fenced code blocks and inline code spans first. Check each inline link and image `[text](target)` and each reference definition `[id]: target`. Skip a target with a URL scheme (`https:`, `mailto:` and so on) and a target that starts with `#`. Remove any `#fragment`. A relative target resolves from the file's directory, and a target that starts with `/` resolves from the repository root. The target must exist and must be inside the repository. Report `FAIL (<file>:<line>: broken link -> <target>)`. Anchors are not checked (state this limit in the script header).

- [ ] **Step 1: Write the self-test.** Cases: a control tree with valid links gives PASS. A link to a missing file gives the FAIL line. A broken link inside inline code gives PASS. A link that leaves the repository gives FAIL. A tree with no `.md` files gives `FAIL (no markdown files)`.
- [ ] **Step 2: Run the self-test and see it fail.**
- [ ] **Step 3: Implement.** The summary line is `md-links-probe: PASS (<links> links in <files> files)`.
- [ ] **Step 4: Run `make md-links-probe`.** Expected: one FAIL, at `docs/SAFETY_SCAN.md:4`. Fix that line. Run again. Expected: PASS. A scan on 2026-11-27, with fenced blocks and inline code removed, found 20 relative links outside `attic/`, and the only broken one is `docs/SAFETY_SCAN.md:4`.
- [ ] **Step 5: Wire the gate** (Global Constraints). Run `make verify-core`. Expected: exit 0.
- [ ] **Step 6: Commit.** `test(docs): add md-links-probe, a gate on relative Markdown links`

### Task 14: move the internal history to `docs/history/` (PR-8, sonnet)

**Files:**
- Move to `docs/history/` (19 files): `2026-06-29-v1-roadmap.md`, `2026-06-30-v1-roadmap.md`, `2026-07-01-array-literals-plan.md`, `2026-07-01-elided-composite-literals-plan.md`, `2026-07-01-int-uint-64bit-migration-plan.md`, `2026-07-08-v1-roadmap.md`, `2026-07-28-arc1-rust-comparison-findings.md`, `M7_AUDIT.md`, `M10_ALT_SYNTAX_PROBE.md`, `M10_AUDIT.md`, `M10_BASELINE_CONFLICTS_AUDIT.md`, `M10_COMPOSITE_LITERAL_SPIKE.md`, `M10_COMPOSITE_LITERAL_SPIKE_2.md`, `M10_GLR_PROBE.md`, `M10_GRAMMAR_DECISION.md`, `M10_LEXER_LAYER_PROBE.md`, `COMPCERT_AUDIT.md`, `COMPTIME_AUDIT.md`, `SELF_HOSTING_ASSESSMENT.md` (the last three are the "and similar" choice: see the questions)
- Create: `docs/history/README.md` (what these files are: dated roadmaps, plans, audits and spikes, kept for the record, not current guidance)
- Modify: each file that names an old path. Measured on 2026-11-27: 78 lines in 47 files. 23 of those lines are inside files that also move. The other 55 lines are in these 38 files:
  - `2026-06-29-v1-roadmap.md`: `docs/superpowers/plans/2026-06-29-v1-phase0.md:11`, `-phase1.md:11`, `-phase2.md:11`, `-phase3.md:13`
  - `2026-06-30-v1-roadmap.md`: `docs/superpowers/specs/2026-07-07-asi-greedy-join-hardening-design.md:5`
  - `2026-07-08-v1-roadmap.md`: `CLAUDE.md:18`, `attic/README.md:11`, `.claude/skills/goo-grammar/references/conflict-ledger.md:724,739`, `docs/adr/0001-nil-semantics-go-parity-with-emitted-deref-checks.md:9,62`, `docs/adr/0005-measurements/scale-400k.md:62`, `docs/adr/0006-toolchain-distribution-and-package-ecosystem.md:19,438`, `docs/superpowers/plans/2026-07-08-gofmt-syntax-a.md:98`, `docs/superpowers/plans/2026-07-11-p6-lanes-m1.md:224`, `docs/superpowers/plans/2026-07-24-nil-deref-checks.md:547,565,583`, `docs/superpowers/plans/2026-07-24-p6-lanes-m2-b1-nng-far-transport.md:1885,2067,2088`, `docs/superpowers/specs/2026-07-08-gofmt-syntax-a-design.md:4`, `docs/superpowers/specs/2026-07-08-phase0-soundness-design.md:4`, `docs/superpowers/specs/2026-07-09-gofmt-syntax-b-design.md:5`, `docs/superpowers/specs/2026-07-09-p2-typesystem-a-design.md:96`, `docs/superpowers/specs/2026-07-10-p3-concurrency-a-design.md:3`, `docs/superpowers/specs/2026-07-10-p3-runtime-b-design.md:3`, `docs/superpowers/specs/2026-07-11-p6-lanes-m1-design.md:6`, `examples/phase3_capstone_probe.goo:2`, `examples/phase4_capstone_probe.goo:1`, `goostd/filepath/filepath.go:12`, `Makefile:100,200,1721,4573`, `scripts/check_stdlib_coverage.sh:7`, `src/parser/parser.y:409`, `tests/cli/cli_test.sh:4`
  - `2026-07-28-arc1-rust-comparison-findings.md`: `bench/README.md:4`
  - `M10_GRAMMAR_DECISION.md`: `src/parser/lexer_bridge.c:22`, `src/parser/parser.y:185,2592`
  - `M10_LEXER_LAYER_PROBE.md`: `src/parser/lexer_bridge.c:22`, `src/parser/parser.y:185`
  - `COMPCERT_AUDIT.md`: `.githooks/pre-push:35`, `include/ccomp_shim.h:17`, `Makefile:326,339` (line 339 is an executed `echo`), `docs/superpowers/plans/2026-09-04-haskell-zig-strangler-plan.md:57`
  - `COMPTIME_AUDIT.md`: `examples/comptime_probe.goo:8`, `Makefile:3112`
  - `M7_AUDIT.md`: `.coord/seed.json:2` (no edit if PR-6 is on `main`, because `.coord/` is out of git)
  - `SELF_HOSTING_ASSESSMENT.md`: `docs/superpowers/specs/2026-06-25-m2-ast-data-modeling-design.md:4`, `docs/superpowers/specs/2026-06-26-m3-foundation-hardening-design.md:4`
  - Every Markdown link in these lines is a backticked path, not a link, so `md-links-probe` does not see them. The grep in Step 3 is the guard.
- Not changeable: the message of tag `v1.0.0` names `docs/2026-07-08-v1-roadmap.md`. `docs/history/README.md` says that the file moved.

- [ ] **Step 1: Run the grammar tripwire.** Run `./scripts/grammar-tripwire.sh > build/gt_before.log 2>&1; echo $?`. Expected: `0`.
- [ ] **Step 2: Move the files and edit each reference.** Use `git mv`. Change `docs/<name>` to `docs/history/<name>`. A bare `<name>` stays when the file that holds it also moves. In any other file, a bare `<name>` becomes `docs/history/<name>`. In a code file, keep the line count (Global Constraints).
- [ ] **Step 3: Prove that no old path remains** (with a positive control). Run `git grep -n -E 'docs/(2026-0[67]-[0-9]{2}-[a-z0-9-]+|M7_AUDIT|M10_[A-Z_0-9]+|COMPCERT_AUDIT|COMPTIME_AUDIT|SELF_HOSTING_ASSESSMENT)\.md' -- . ':!docs/superpowers/plans/2026-11-27-external-readiness.md' ':!docs/history/README.md' > build/old_paths.txt; echo $?`. Expected: `1` (no match) and an empty file. Then run `git grep -n -F 'docs/history/2026-07-08-v1-roadmap.md' > build/new_paths.txt; wc -l < build/new_paths.txt`. Expected: at least 30.
- [ ] **Step 4: Run the tripwire again.** Expected: `0`, with the same counts as Step 1.
- [ ] **Step 5: Run `make verify-core`.** Expected: exit 0, including `md-links-probe`.
- [ ] **Step 6: Commit.** `docs: move dated roadmaps, plans and audits to docs/history`

### Task 15: `docs/README.md`, an index for a new reader (PR-8, haiku)

**Files:**
- Create: `docs/README.md`

- [ ] **Step 1: Write the index.** Use one line for each entry: `01-VISION.md`, `02-LANGUAGE-SPECIFICATION.md`, `GO_SPEC_CONFORMANCE.md`, `adr/` (list the six ADRs), `SANITIZERS.md`, `SAFETY_SCAN.md`, `misra/README.md`, `lanes.md`, `lanes-race-runbook.md`, `spmd-harness.md`, `superpowers/` (design specs and plans), and `history/README.md`. Leave out `quantum.md`. Task 19 adds it.
- [ ] **Step 2: Run `make md-links-probe`.** Expected: PASS.
- [ ] **Step 3: Commit.** `docs: add an index for docs/`

### Task 16: `CONTRIBUTING.md` (PR-9, haiku)

**Files:**
- Create: `CONTRIBUTING.md`

- [ ] **Step 1: Write the file.** Include these sections:
  - Build: the CI prerequisites (gcc-14, Ubuntu 24.04 `llvm-dev`, bison, the apt line of `.github/workflows/tests.yml:97-102`), then `make -j"$(nproc)" lexer`.
  - Verify: `make verify-core > build/vc.log 2>&1; echo $?`.
  - The PR rule: name the 8 required checks, and say that a PR merges only on green.
  - Commits: conventional style, imperative mood, one logical change for each commit.
  - TDD: write the failing fixture first (`examples/<name>.goo` + `.expected.txt`, or `tests/golden/reject/<name>.goo` + `.err.txt`).
  - A new gate: the self-test, census, `generated.bzl` and parity-count rule.
  - Grammar changes: `./scripts/grammar-tripwire.sh`.
  - Security: a link to `SECURITY.md`.
  - AI help: some commits carry `Co-Authored-By` lines for AI assistance, and those lines stay.
- [ ] **Step 2: Run `make test`.** Expected: PASS. If `md-links-probe` is on `main`, also run `make md-links-probe`. Expected: PASS.
- [ ] **Step 3: Commit.** `docs: add CONTRIBUTING.md`

### Task 17: repository description and topics (owner action, no PR)

- [ ] **Step 1: The controller sends this text to the owner for approval.** Description: `Goo: a Go-derived compiled language with error unions, nullables, comptime and compile-time linear qubits. Native code through LLVM, plus QIR and OpenQASM 3 output.` Topics: `compiler`, `programming-language`, `golang`, `llvm`, `quantum-computing`, `qir`, `openqasm`, `c23`.
- [ ] **Step 2: After approval, run** `gh repo edit goo-lang/goolang --description "<text>" --add-topic compiler,programming-language,golang,llvm,quantum-computing,qir,openqasm,c23`. Then confirm with `gh repo view goo-lang/goolang --json description,repositoryTopics`.

## Part 2: try it

### Task 18: `quickstart-probe` (PR-10, sonnet)

**Files:**
- Create: `scripts/quickstart_probe.sh`
- Modify: `Makefile`, `tests/probes/census.txt`, `tests/probes/generated.bzl`, `tools/parity-gate-count.txt`

**Interfaces:**
- Produces: `scripts/quickstart_probe.sh [--self-test | --published <version>]`. Environment: `QS_ROOT` (default: the git toplevel, or the script's parent directory when no `.git` exists), `QS_DOC` (default `$QS_ROOT/README.md`). Make targets `quickstart-probe` and `quickstart-selftest`.
- Produces the Markdown contract for Task 19 (each marker is a line by itself, as in Task 7):
  - `<!-- prereqs:begin -->` / `<!-- prereqs:end -->`: one `sh` block with the apt line for Ubuntu 24.04.
  - `<!-- install:begin -->` / `<!-- install:end -->`: one `sh` block. Its first line is `GOO_VERSION=<x.y.z>`. The block downloads the tarball and `SHA256SUMS` from `https://github.com/goo-lang/goolang/releases/download/v${GOO_VERSION}/`, runs `sha256sum --ignore-missing -c SHA256SUMS`, extracts the tarball, puts its `bin/` on `PATH`, and runs `goo version`.
  - `<!-- quickstart:begin -->` / `<!-- quickstart:end -->`: one `console` block. A line `$ <cmd>` is a command, and the lines after it, up to the next `$ ` line, are its exact combined stdout and stderr. Each command must exit 0. A command that must fail ends with `; echo "exit status: $?"`, so the status is part of the output.
  - The macOS line: a `docker run` command with the image `docker.io/library/ubuntu:24.04@sha256:<digest>`. The digest must equal the `FROM` digest of `Containerfile`.
- Modes:
  - Default (local, in `verify-core`): run `scripts/package_toolchain.sh` into a temporary directory, extract the tarball, unset `GOOROOT` and `GOO_RUNTIME`, `cd` to an empty temporary directory, and put the extracted `bin/` on `PATH`. The install block is not run. A command of the form `curl -fsSLO https://raw.githubusercontent.com/goo-lang/goolang/v${GOO_VERSION}/<path>` becomes `cp "$QS_ROOT/<path>" .`. Every other command runs as written.
  - `--published <version>`: run the prerequisites block when the user is root (remove a leading `sudo `), run the install block with `GOO_VERSION=<version>`, then run the quickstart block as written, all in an empty directory.
  - Both modes check that the `console` block of `docs/quantum.md` is byte-identical to the one in `README.md`, and that the `docker run` digest equals the `Containerfile` digest.

- [ ] **Step 1: Write the self-test.** It uses a small fixture document and the local mode. Cases:
  - control: PASS.
  - an expected output line changed gives `FAIL (step <n>: output differs)`.
  - a command that exits 1 without `echo "exit status: $?"` gives `FAIL (step <n>: exit 1)`.
  - the `docs/quantum.md` copy differs gives `FAIL (quickstart blocks differ between README.md and docs/quantum.md)`.
  - a changed digest gives `FAIL (docker image digest differs from Containerfile)`.
  - an empty quickstart block gives `FAIL (no quickstart steps)`.
  - the run is made from the repository root gives `FAIL (probe cwd is inside the repository)`. This last case is the guard against the tier-4 lookup.
- [ ] **Step 2: Run the self-test and see it fail.**
- [ ] **Step 3: Implement.** The summary line is `quickstart-probe: PASS (<n> steps, local tarball <name>)` or `(... published v<version>)`.
- [ ] **Step 4: Run the self-test.** Expected: PASS. Do not wire the gate yet. Task 19 writes the blocks and wires it.
- [ ] **Step 5: Commit.** `test(release): add quickstart-probe (not yet wired)`

### Task 19: the install and quickstart sections (PR-10, sonnet)

**Files:**
- Modify: `README.md` (`## Install`, `## Quantum quickstart`, the macOS line, the `CONTRIBUTING.md` link), `docs/quantum.md` (the same quickstart block), `docs/README.md` (add `quantum.md`)
- Modify: `Makefile` (wire Task 18), `tests/probes/census.txt`, `tests/probes/generated.bzl`, `tools/parity-gate-count.txt`

**Interfaces:**
- Consumes: the Task 18 contract.

- [ ] **Step 1: Write the prerequisites block.** Ubuntu 24.04 x86_64. Start from `sudo apt-get install -y --no-install-recommends ca-certificates curl gcc libc6-dev libllvm18 libjson-c5 libcurl4t64 zlib1g`. Confirm the list in Task 20: the clean-container run proves it complete. Before that, check each library of `ldd bin/goo` in the image built by `make podman-image`, with `dpkg -S`.
- [ ] **Step 2: Write the install block** per the contract, with `GOO_VERSION=0.1.0` and the triple `x86_64-unknown-linux-gnu`. Add the optional `gh attestation verify goo-${GOO_VERSION}-x86_64-unknown-linux-gnu.tar.gz --repo goo-lang/goolang` line after the block, outside the markers.
- [ ] **Step 3: Write the quickstart block.** Use four steps, each with the output that the local run gives:
  1. `curl` `examples/qir_teleport_probe.goo`, then `goo -o teleport qir_teleport_probe.goo`, then `./teleport`. Expected output: `teleport failures: 0` and `toffoli result: true`.
  2. `curl` `tests/golden/reject/qir_use_after_move_reject.goo`, then `goo -o uam qir_use_after_move_reject.goo; echo "exit status: $?"`. Expected output: the header `qir_use_after_move_reject.goo:8:8: error: use of moved linear qubit 'q1' (moved at line 7)`, the snippet and caret lines, and `exit status: 1`. Use this existing fixture, which `test-golden-reject` pins. Do not write a new fixture for the README.
  3. `goo --emit-qasm3 qir_teleport_probe.goo -o teleport.qasm`, then `head -4 teleport.qasm`.
  4. `goo --emit-qir qir_teleport_probe.goo -o teleport.ll`, then `grep -o '"qir_profiles"="adaptive_profile"' teleport.ll`.
- [ ] **Step 4: Write the macOS line.** Write: "Run `docker run --rm -it --platform linux/amd64 docker.io/library/ubuntu:24.04@sha256:<Containerfile digest>`, then run the three blocks above inside the container, without `sudo`." Also say that a native macOS build needs a compiler change (the exe-directory lookup is inside `#ifdef __linux__`).
- [ ] **Step 5: Copy the quickstart block** byte for byte into `docs/quantum.md`. Add `quantum.md` to `docs/README.md`, and add the `CONTRIBUTING.md` link to the README.
- [ ] **Step 6: Wire `quickstart-probe`** (Global Constraints). Run `make quickstart-probe`, `make readme-claims-probe` and `make md-links-probe`. Expected: all three PASS.
- [ ] **Step 7: Run `make verify-core`.** Expected: exit 0.
- [ ] **Step 8: Commit.** `docs(readme): add the install steps and the quantum quickstart, gated`

### Task 20: the release workflow (PR-11, sonnet)

**Files:**
- Create: `.github/workflows/release.yml`, `docs/releases/v0.1.0.md`
- Modify: `.github/workflows/tests.yml` (add `workflow_call:` to `on:`)

**Interfaces:**
- Consumes: `scripts/podman_build.sh gate`, `scripts/package_toolchain.sh`, `scripts/quickstart_probe.sh --published`, `make podman-image`.

- [ ] **Step 1: Write `release.yml`.** Pin every action by commit SHA, as the other workflows do. Find each SHA with `gh api repos/<owner>/<action>/git/ref/tags/<tag>`, which is an API read, not a download. The workflow-level permissions are `contents: read`. The jobs:
  - Trigger: `on: push: tags: ['v[0-9]+.[0-9]+.[0-9]+', 'v[0-9]+.[0-9]+.[0-9]+-rc.[0-9]+']`.
  - `verify-core`: `uses: ./.github/workflows/tests.yml`.
  - `build` (needs `verify-core`, ubuntu-24.04):
    - Check out with `fetch-depth: 0`.
    - Fail unless `git merge-base --is-ancestor "$GITHUB_SHA" origin/main` succeeds.
    - Set `VERSION="${GITHUB_REF_NAME#v}"`.
    - Run `make podman-image`, then `GOO_VERSION="$VERSION" bash scripts/podman_build.sh gate out`.
    - Package inside the image, so the host needs no LLVM 18 and the tarball gets `lib/goo-toolchain.txt`: `podman run --rm --env SOURCE_DATE_EPOCH="$(git log -1 --format=%ct)" -v "$PWD:/src:ro,z" -v "$PWD/out:/out:z" -v "$PWD/dist:/dist:z" goolang-build:local sh -c 'COMPILER=/out/goo GOO_RUNTIME=/out/libgoo_runtime.a bash /src/scripts/package_toolchain.sh /dist'`.
    - Assert that the tarball name is `goo-$VERSION-x86_64-unknown-linux-gnu.tar.gz`.
    - Run `(cd dist && sha256sum goo-*.tar.gz > SHA256SUMS)`.
    - Upload `dist/` as an artifact.
  - `release` (needs `build`; permissions `contents: write`, `id-token: write`, `attestations: write`):
    - Download the artifact.
    - Attest the build provenance with `actions/attest-build-provenance`, with `subject-path: dist/goo-*.tar.gz`.
    - Run `gh release create "$GITHUB_REF_NAME" dist/goo-*.tar.gz dist/SHA256SUMS --verify-tag --title "Goo $VERSION" --notes-file "docs/releases/v${VERSION%%-rc.*}.md"`. Add `--prerelease` when the tag holds `-rc.`.
  - `verify-published` (needs `release`; permissions `contents: write`, used only for the rollback):
    - On the runner, download the two files with the README URLs, then run `sha256sum -c SHA256SUMS` and `gh attestation verify <tarball> --repo "$GITHUB_REPOSITORY"`.
    - Run `podman run --rm -v "$PWD:/repo:ro,z" docker.io/library/ubuntu:24.04@sha256:<Containerfile digest> bash /repo/scripts/quickstart_probe.sh --published "$VERSION"`.
    - Require the line `quickstart-probe: PASS` in the log, as `repro.yml` does for its probes.
    - A last step with `if: failure()` runs `gh release edit "$GITHUB_REF_NAME" --draft=true`, so a release that fails its own test is not left public.
- [ ] **Step 2: Write `docs/releases/v0.1.0.md`.** Include:
  - What the release holds (`bin/goo`, `lib/libgoo_runtime.a`, the nine stdlib packages, `lib/goo-toolchain.txt`).
  - The platform: Linux x86_64, with the Ubuntu 24.04 libraries of the prerequisites block, and a system C compiler. macOS runs through the container only, and a native build needs a compiler change.
  - The install and verify commands, as a link to the README.
  - The known limits, as a link to `docs/quantum.md` section 4.
  - No other claim.
- [ ] **Step 3: Run the static checks.** Run `make readme-claims-probe` and `make md-links-probe`. Expected: PASS. If `actionlint` is already on `PATH`, also run `actionlint .github/workflows/release.yml .github/workflows/tests.yml`. Do not install it.
- [ ] **Step 4: Confirm that `tests.yml` still runs** on the PR. Expected: the `verify-core` check is green on PR-11.
- [ ] **Step 5: Commit.** `ci(release): build, attest and publish a tarball on a version tag`

### Task 21: the `v0.1.0-rc.1` run, then `v0.1.0` (no PR, owner approval, haiku watches the runs)

- [ ] **Step 1: Get owner approval.** The owner approves the tag push, and gives the decision on the existing `v1.0.0` tag (see the questions).
- [ ] **Step 2: Push the rc tag.** On the `main` commit that holds PR-11, run `git tag -a v0.1.0-rc.1 -m "Goo 0.1.0 release candidate 1"` and `git push origin v0.1.0-rc.1`.
- [ ] **Step 3: Watch the run.** Run `gh run list --workflow release.yml --limit 1 --json databaseId`, then `gh run watch <id>`, then `gh run view <id> --json conclusion`. Expected: `"conclusion":"success"`, and `gh release view v0.1.0-rc.1 --json isPrerelease,assets` shows `isPrerelease: true` with 2 assets.
- [ ] **Step 4: Rerun the published quickstart by hand.** Run `bash scripts/quickstart_probe.sh --published 0.1.0-rc.1` on a Linux x86_64 machine outside the repository directory. Expected: the PASS line.
- [ ] **Step 5: Push `v0.1.0`** on the same commit, and repeat Step 3. Expected: success, and `isPrerelease: false`.

## Optional

### Task 22 (OPTIONAL, needs owner approval): drop the `packages.microsoft.com` apt source in CI (PR-12, haiku)

The GitHub runner image adds an apt source `packages.microsoft.com`. That source returned `403 Forbidden` twice on 2026-11-27 and made two CI jobs red at the toolchain install. No job installs a package from it.

**Files:**
- Modify: `.github/workflows/tests.yml:97`, `.github/workflows/nightly.yml:58`, `.github/workflows/codeql.yml:42`, `.github/workflows/bazel.yml:28,158,222,274` (seven `sudo apt-get update` sites)

- [ ] **Step 1: Add the removal before each `sudo apt-get update`.** Insert this line: `sudo grep -l -r 'packages.microsoft.com' /etc/apt/sources.list.d/ | xargs -r sudo rm -f -v`. The `-v` flag prints each file that it removes. The grep covers both the `.list` and the `.sources` formats.
- [ ] **Step 2: Prove the count.** Run `git grep -c 'packages.microsoft.com' -- .github/workflows`. Expected: 7 in total. Run `git grep -c 'apt-get update' -- .github/workflows`. Expected: 7 in total.
- [ ] **Step 3: Open the PR.** Expected: all 8 required checks green, and each install step log prints the removed file name.
- [ ] **Step 4: Commit.** `ci: drop the Microsoft apt source before apt-get update`

---

## Self-review (done while writing)

- Spec coverage. 1a: Tasks 2-5. 1b: Tasks 6-8. 2a: Tasks 20-21. 2b: Tasks 18-19. 3a: Tasks 10-12. 3b: Tasks 13-15. 3c: Tasks 16-17 (`SECURITY.md` is on `main`). Part 4: Task 9. Each controller call has a task: `.claude/skills/` stays (Task 11), the task-master cut (Task 11), the reused ownership fixture (Task 19), the draft PR link (Task 9), `readme-claims-probe` (Task 7), and the quickstart gate on a local tarball and on the published files (Tasks 18 and 20).
- Names used across tasks: `qasm3_refuse` and `Qasm3Ctx.failed` (Task 2, then Task 3). `GOO_QIR_SIM_MAX_QUBITS` and `GOO_QIR_SIM_MAX_RESULTS` (Task 4, then Task 5). `qir_sim_linked` (Task 4). The marker names (Task 7, then Tasks 8, 9, 19; Task 18, then Tasks 19, 20). `quantum-emit-probe` (Task 1, then Tasks 8, 9).
- Each Review Focus line has its test: 1 in Task 4 (`qubit_at_20`), 2 in Task 3, 3 in Task 5, 4 in Task 11 Step 7 (a runbook, not a test), 5 in Task 20 (`verify-published`).

## Questions for the controller

1. **The `v1.0.0` tag.** `origin` has an annotated tag `v1.0.0` (2026-07-11, "Goo v1.0.0", it names `docs/2026-07-08-v1-roadmap.md`). A `v0.1.0` release after it inverts the semver order for any tool that sorts tags. The options:
   - Keep `v0.1.0` as decided, and write in the release notes that `v1.0.0` was an internal milestone tag.
   - Delete the old tag. This needs owner action, and clones keep their copy.
   - Choose a version above 1.0.0.
   This plan assumes the first option until the owner decides.
2. **Task 3 is a third silent wrong result**, found while planning (`w03_ret.goo`: a qubit returned by a helper function). Do you approve it in PR-1? The fix only refuses: it makes `--emit-qasm3` fail where it was silently wrong.
3. **Task 5 widens the 1a scope** to the 17th `Alloc` and a result index of 64 or more. Both are reproduced and alias index 0 in the same way. Do you agree that G2 covers them?
4. **The compile-time refusal of Task 4 applies to native builds only**, so `QubitAt(16)` and above stay legal for `--emit-qasm3` and `--emit-qir`. Do you confirm this reading of "refuse it at compile time"?
5. **The "and similar" set for `docs/history/`.** This plan adds `COMPCERT_AUDIT.md`, `COMPTIME_AUDIT.md` and `SELF_HOSTING_ASSESSMENT.md` to the 16 files that the spec names. `docs/superpowers/` stays where it is. Do you agree?
6. **`.handoff.md` citations.** 98 prose lines cite `.handoff.md` (20 in ADRs, 64 in `docs/superpowers/`). After option B the file is not in the tree, but it is in the git history. This plan leaves the citations as they are. Do you agree?
7. **Other AI-workflow files.** `.env.example` is the task-master API-key template (it asks for `ANTHROPIC_API_KEY`). `scripts/prd.txt` is the 2025 task-master PRD (it describes kernel actors and GPU kernels). Option B does not name them, and this plan leaves them. Should they also leave git?
8. **The `templates/` move takes `tools/project_wizard/` and the `wizard` Make target with it**, and it edits ADR 0006 line 31. The alternative is to keep all three in place. This plan moves them, by the rule of 3a.
9. **`ide/`** is a VS Code syntax extension with zero references, so the rule moves it to `attic/`. A reader can find syntax colouring useful. Do you confirm the move?
10. **The live-hardware table stays in `README.md`**, as 1b says, and `docs/quantum.md` links to it. The alternative is to move the table to the quantum page and keep a summary in the README.
11. **The macOS path uses the plain pinned `ubuntu:24.04` image** (the `Containerfile` base digest), and the reader runs the Linux steps inside it. The alternative is to publish a ready image to GHCR, which needs `packages: write` and a second published artifact. This plan uses the plain image.
12. **The release workflow reruns `verify-core` through `workflow_call` of `tests.yml`.** The alternative is to read the check runs of the tag commit, which is faster but depends on the API.
13. **G3 says "one documented command".** This plan gives one block of commands (download, check, extract, `PATH`). A single `install.sh` asset is the alternative, but a reader cannot audit it as easily. Is the block enough?
14. **Task 22** (the Microsoft apt source) needs owner approval.
15. **Task 17** (the description and topics text) needs owner approval.

## Controller rulings and owner decisions (2026-11-27)

Owner decisions:
- Version: delete the old annotated tag `v1.0.0` (tag object d00b9df4, commit c0b68068, 2026-07-11, "Goo v1.0.0"; no GitHub Release uses it), then release `v0.1.0`. The owner deletes the tag (an auto-mode rule blocks the agent). To create it again: `git tag -a v1.0.0 c0b68068 -m "Goo v1.0.0"`. Task 21 starts only after the tag is gone.
- Task 17 is DONE: description "Goo: a Go superset with compile-time linear qubits, automatic reference counting with no GC pauses, and QIR / OpenQASM 3 output.", topics compiler, golang, llvm, memory-safety, openqasm, programming-language, qir, quantum-computing.
- Task 22 is APPROVED and joins wave A.
- Execution: subagent-driven, one review for each PR.

Controller answers to the planner's questions:
2. Task 3 is in scope (a silent wrong result in the quantum path, goal G2).
3. Yes: G2 covers the 17th `Alloc` and a result index of 64 or more (Task 5).
4. Yes: the compile-time refusal is for native builds only. `QubitAt(16)` and above stay legal for `--emit-qasm3` and `--emit-qir`.
5. Yes: COMPCERT_AUDIT, COMPTIME_AUDIT and SELF_HOSTING_ASSESSMENT move too. docs/superpowers/ stays.
6. Yes: the prose citations of `.handoff.md` stay. The file stays in the git history.
7. `.env.example` and `scripts/prd.txt` stay for now.
8. Yes: `tools/project_wizard/` and the `wizard` target move with `templates/`.
9. Yes: `ide/` moves to `attic/` (no build, gate or workflow uses it).
10. The live-hardware table stays in the README. The quantum page links to it.
11. On macOS, the plain `ubuntu:24.04` image.
12. The release workflow runs `verify-core` again through `workflow_call`.
13. One block of install commands is enough for v0.1.0. An `install.sh` comes with `goop` later.
