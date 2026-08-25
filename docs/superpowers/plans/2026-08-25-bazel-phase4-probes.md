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

### Task 1: Re-measure the census

**Files:** none. This task measures, and everything after depends on it.

- [ ] **Step 1: Regenerate the five category lists**

```bash
./tools/parity.sh --list-make-gates > /tmp/gates.txt
for f in script golden ex pf bespoke; do : > /tmp/c_$f.txt; done
for g in $(grep -E -- '-probe$' /tmp/gates.txt); do
  body=$(awk -v p="^$g:" '$0~p{f=1;next} f&&/^[^\t]/{exit} f' Makefile)
  if echo "$body" | grep -q 'scripts/'; then echo "$g" >> /tmp/c_script.txt; continue; fi
  src=$(echo "$body" | grep -oE 'examples/[a-z0-9_]+\.goo' | head -1)
  if [ -n "$src" ]; then
    if [ -f "${src%.goo}.expected.txt" ]; then echo "$g" >> /tmp/c_golden.txt
    else echo "$g" >> /tmp/c_ex.txt; fi
    continue
  fi
  if echo "$body" | grep -qE 'which (valgrind|clang)|for name in|for f in|_NAMES\)'; then echo "$g" >> /tmp/c_bespoke.txt
  elif echo "$body" | grep -q 'printf'; then echo "$g" >> /tmp/c_pf.txt
  else echo "$g" >> /tmp/c_bespoke.txt; fi
done
for f in script golden ex pf bespoke; do printf '%-10s %s\n' "$f" "$(grep -c . /tmp/c_$f.txt)"; done
echo "total: $(cat /tmp/c_*.txt | grep -c .)"
```

Expected: `28 48 32 69 6`, total 183. **If any count differs, use the new numbers** and say so in every commit message that quotes one. The Makefile moves; this plan's numbers are a measurement from 2026-08-25, not a constant.

- [ ] **Step 2: Record the bespoke six by name**

```bash
cat /tmp/c_bespoke.txt
```
Expected: `arena-free-probe`, `arena-valgrind-probe`, `charlit-reject-probe`, `goostd-resolver-probe`, `hexesc-reject-probe`, `stencil-race-runbook-probe`. Task 8 handles these individually.

---

### Task 2: The fixture macro

**Files:**
- Create: `tools/goo_probe.bzl`, `tools/run_probe.sh`
- Modify: `tools/BUILD`

**Interfaces:**
- Consumes: `//src/compiler:goo`, `//src/runtime:goo_runtime_archive`.
- Produces: `goo_probe(name, src, expected, exit_code, stderr_contains, env, tags)` in `//tools:goo_probe.bzl`. Tasks 3 and 4 use it.

**Background.** `scripts/run_golden.sh` already implements exactly these semantics — expected stdout, an optional `.exit` sidecar, an optional `.stderr.txt` substring, an optional `.env` file, a timeout. The runner here mirrors that contract deliberately so the two cannot disagree about what a fixture means.

- [ ] **Step 1: Read the contract being mirrored**

```bash
sed -n '1,30p' scripts/run_golden.sh
```
The sidecar semantics are documented in that header. Do not invent different ones.

- [ ] **Step 2: Write the runner**

`tools/run_probe.sh` takes the compiler, archive, GOOROOT, source, expected file and optional assertions as arguments; compiles, runs with a timeout, and checks stdout, exit code and stderr substring. It must:

- take exit statuses with no pipe (`rc=$?` directly), per `CLAUDE.md`;
- treat `rc 124` as a timeout failure always, never compared against an expected exit code;
- fail if the expected file is missing, rather than passing vacuously.

- [ ] **Step 3: Write the macro**

```python
load("@rules_shell//shell:sh_test.bzl", "sh_test")

def goo_probe(name, src, expected = None, exit_code = 0, stderr_contains = None,
              env_file = None, size = "small", tags = []):
    """Compile a .goo fixture, run it, and check stdout / exit code / stderr."""
    args = ["$(location //src/compiler:goo)",
            "$(location //src/runtime:goo_runtime_archive)",
            "$(location %s)" % src,
            str(exit_code)]
    data = [src, "//src/compiler:goo", "//src/runtime:goo_runtime_archive",
            "//goostd:files"]
    ...
```

Pin `GOOROOT` in the runner, not in the macro, so there is one place that knows it.

- [ ] **Step 4: Prove the macro can report a failure**

Add to `testing/teeth/`: a fixture whose `.expected.txt` deliberately disagrees with what the program prints, and a `goo_probe` over it tagged `manual`. Assert it exits non-zero, then that a matching pair exits 0.

**This is the single most important check in the phase.** One macro will generate ~150 tests; if it cannot fail, all 150 pass while asserting nothing.

- [ ] **Step 5: Prove the runner rejects a missing expected file**

```bash
# with a --expected pointing at a nonexistent path
```
Expected: non-zero and a message naming the missing file. A runner that treats "no expected output" as "nothing to compare, therefore pass" is the `run_golden.sh` empty-corpus defect (`5c633f6`) in miniature.

- [ ] **Step 6: Commit**

---

### Task 3: Generate the 80 fixture probes

**Files:**
- Create: `tools/gen_probe_targets.py`, `tests/probes/BUILD` (generated)
- Modify: `tools/BUILD`

**Interfaces:**
- Consumes: the macro from Task 2, the census from Task 1.
- Produces: 80 `goo_probe` targets, and `//tools:probe_targets_current`, a gate asserting the generated file is current.

- [ ] **Step 1: Write the generator**

`tools/gen_probe_targets.py` parses each probe recipe in the Makefile and emits a `goo_probe(...)` call: the `examples/*.goo` it compiles, its expected file, an exit code if the recipe asserts one, a stderr substring if it greps one. It must **refuse to guess**: a recipe it cannot parse confidently is listed on stderr and omitted, and the count of omissions is printed. Silent omission is the failure mode to avoid.

- [ ] **Step 2: Generate and read the output**

```bash
python3 tools/gen_probe_targets.py > tests/probes/BUILD
grep -c 'goo_probe(' tests/probes/BUILD    # expect 80 minus any omissions
```
Read the omissions. Each is either a genuine bespoke case for Task 8 or a generator gap worth fixing.

- [ ] **Step 3: Run them**

```bash
bazel test //tests/probes:all --test_output=errors 2>&1 | tail -20
```
Expect failures on any probe whose fixture is one of the nine NNG ones; tag those `manual` with a comment naming phase 3c.

- [ ] **Step 4: Compare against Make, probe by probe**

For a sample of at least 10, run `make <probe>` and the Bazel target and confirm both pass. A probe that passes under Bazel and fails under Make is a Bazel target testing something weaker.

- [ ] **Step 5: Gate the generated file**

```python
sh_test(
    name = "probe_targets_current",
    srcs = ["probe_targets_current.sh"],
    data = ["gen_probe_targets.py", "//:Makefile", "//tests/probes:BUILD"],
)
```
The script regenerates and diffs. A generated file nobody regenerates is a snapshot, not a derivation.

- [ ] **Step 6: Prove that gate can fail** — edit one line of the generated file, confirm the gate goes red, restore.

- [ ] **Step 7: Commit**

---

### Task 4: The assertion macro

**Files:**
- Modify: `tools/goo_probe.bzl`

**Interfaces:**
- Produces: `goo_expect_probe(name, src, exit_code, stderr_contains, stdout_contains)` for the 69 negative tests.

- [ ] **Step 1: Confirm what the 69 assert**

```bash
for g in $(cat /tmp/c_pf.txt); do
  body=$(awk -v p="^$g:" '$0~p{f=1;next} f&&/^[^\t]/{exit} f' Makefile)
  if   echo "$body" | grep -qE 'diff -u'; then echo "diff"
  elif echo "$body" | grep -qE 'rc.*-ne|abort|134'; then echo "exit"
  elif echo "$body" | grep -q 'grep -q'; then echo "grep"
  else echo "other"; fi
done | sort | uniq -c
```
Expected roughly: 37 exit, 23 grep, 4 diff, 5 other.

- [ ] **Step 2: Implement it as a thin wrapper over `goo_probe`**

The two differ only in which assertions are mandatory. Do not fork the runner.

- [ ] **Step 3: Prove it can fail**, both on a wrong exit code and on an absent stderr string. Two teeth fixtures, both tagged `manual`.

- [ ] **Step 4: Commit**

---

### Task 5: Extract the 69 sources, behind a drift gate

**Files:**
- Create: `tests/probes/src/*.goo` (69 files), `tools/probe_source_drift.sh`
- Modify: `tools/BUILD`

**Background.** Each of the 69 builds its source with `printf` inside the recipe. Extracting it gives Bazel a real file — readable, diffable, and usable by the same macro as the other 80. It also creates a **second copy**, and the whole risk is that the two drift while both gates pass.

- [ ] **Step 1: Extract by RUNNING the recipe, not by re-typing it**

For each probe, run its `printf` and capture the file it writes. Do not transcribe the source by hand — a transcription error produces a fixture that tests something adjacent and passes.

- [ ] **Step 2: Write the drift gate**

`tools/probe_source_drift.sh` re-runs each recipe's `printf` into a scratch file and diffs it against the extracted fixture. Exit 1 on any difference, naming the probe. Exit 2 if it finds fewer fixtures than expected — an empty-corpus guard, the same one `run_golden.sh` was missing.

- [ ] **Step 3: Prove the drift gate fails on a drifted file**

Edit one extracted fixture by one character, confirm exit 1 naming that probe, restore, confirm exit 0.

- [ ] **Step 4: Prove the empty-corpus guard fires** — point it at an empty directory, expect exit 2.

- [ ] **Step 5: Commit**

---

### Task 6: Apply the assertion macro to the 69

- [ ] **Step 1: Extend the generator** to emit `goo_expect_probe` calls for the `printf` category, reading the assertion from the recipe.
- [ ] **Step 2: Regenerate, run, and read the omissions.**
- [ ] **Step 3: Compare against Make for a sample of at least 10.**
- [ ] **Step 4: Confirm `probe_targets_current` still passes** — the generator changed, so the committed file must be regenerated in the same commit.
- [ ] **Step 5: Commit**

---

### Task 7: The 28 script-backed probes

**Files:**
- Create: `tests/probes/scripts/BUILD`

- [ ] **Step 1: List them** — `cat /tmp/c_script.txt`.
- [ ] **Step 2: One `sh_test` each**, with the script, the compiler, the archive and any fixtures it reads as `data`. These are uniform; a small macro is worthwhile.
- [ ] **Step 3: Run them and resolve missing runfiles.** A script that reads a path not in `data` fails in the sandbox and passes outside it — prefer the sandbox's answer.
- [ ] **Step 4: Commit**

---

### Task 8: The bespoke six

Handle each individually, with a comment naming why it is not generated:

| Probe | Why bespoke |
|---|---|
| `arena-free-probe` | loops a name matrix |
| `arena-valgrind-probe` | needs valgrind; today it SKIPS when absent, which reads as a pass. Under Bazel make it a `--config=valgrind` target instead, or tag it `requires-valgrind` — do not reproduce the silent skip. |
| `charlit-reject-probe` | reject matrix |
| `goostd-resolver-probe` | exercises GOOROOT resolution itself, so it cannot take the pinned `GOOROOT` the others do |
| `hexesc-reject-probe` | reject matrix |
| `stencil-race-runbook-probe` | runbook check, not a compile |

- [ ] **One step per probe**, each ending green and compared against `make`.
- [ ] **Commit**

---

### Task 9: Close phase 4

- [ ] **Step 1: Read parity.**

```bash
./tools/parity.sh | head -3
```
Expected: unmapped falls from 212 to about **29** — the 34 non-probe gates minus the 5 already mapped.

- [ ] **Step 2: Confirm every probe gate is mapped.**

```bash
for g in $(grep -E -- '-probe$' /tmp/gates.txt); do
  ./tools/parity.sh 2>/dev/null | grep -qx "  $g" && echo "STILL UNMAPPED: $g"
done
```
Expected: only the NNG nine, if any of them are probe gates, each with an allowlist entry naming phase 3c.

- [ ] **Step 3: Both build systems green**, plus `probe_targets_current`, `probe_source_drift`, and both teeth fixtures still able to fail.

- [ ] **Step 4: Push, open the PR against `main`, read the real CI conclusion.**

---

## What phases 5–7 still need

- **Phase 5:** the 34 non-probe gates — unit suites, `*-selftest`, stress tests, `*-ir-pin`. Several are already mapped. Golden suites land with phase 3c.
- **Phase 6:** sanitizer configs and `testing/teeth` red/green proofs, plus the coverage floor. This is where `arena-valgrind-probe`'s silent skip is properly fixed.
- **Phase 7:** `parity.sh` reaches 0, the allowlist is justified entry by entry, and the Makefile is deleted along with `tools/compiler_differential.sh`, `tools/golden_bazel.sh`'s split assertion and `tools/probe_source_drift.sh` — all three exist only to compare two build systems.
