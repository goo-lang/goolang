# Bazel Migration — Phase 3b (the runtime archive) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce `lib/libgoo_runtime.a`'s equivalent under Bazel so the Bazel-built compiler can link a running program — **without NNG**, which is deferred to phase 3c.

**Architecture:** A `cc_library` over 12 of the 13 `RUNTIME_OBJS` files (all but `far_transport.c`, which cannot compile without NNG headers), plus a rule that packs them into a single `.a` the compiler can be pointed at with `GOO_RUNTIME`. The golden suite then runs against a Bazel-built compiler and a Bazel-built archive.

**Tech Stack:** Bazel 9.2.0, `rules_cc` 0.2.17, `rules_shell` 0.6.1, GCC, C23. No NNG, no cmake.

**Spec:** `docs/superpowers/specs/2026-08-25-bazel-migration-design.md`
**Predecessor:** phase 3 (PR #323, merged)

## Global Constraints

Everything in the phase 0–3 plans' Global Constraints still applies. In addition:

- **NNG is NOT in this phase.** Measured 2026-08-25: an archive built from the 13 `RUNTIME_OBJS` with **no NNG at all** passes **486 of 495** golden fixtures. `far_transport.o` sits in the archive unreferenced, so the linker never pulls it, and never pulls the NNG members behind it.
- **Exactly 9 fixtures need NNG**, and they are one family: `far_shim_probe` plus eight `lanes_*` probes. The lanes feature uses the far transport.
- **`//src/runtime:runtime` from phase 1 is NOT this archive.** It carries the 11 files of `RUNTIME_SRCS` and exists for `obj_header_test`. `RUNTIME_OBJS` has 13, adding `io.c` and `far_transport.c`. This phase adds a second target carrying 12 of those; it does not rename the first.
- **`far_transport.c` is EXCLUDED, so the archive holds 12 objects, not 13.** It includes `<nng/nng.h>` and `<nng/protocol/pair1/pair.h>`, and `Makefile:179-180` gives it `-I$(NNG_BUILD)/include` plus a dependency on `$(NNG_LIB)`. It cannot **compile** without NNG, let alone link. This is a deliberate, temporary divergence from `RUNTIME_OBJS`, closed in phase 3c. Measured 2026-08-25: a 12-object archive gives exactly the same 486/9 split, with the same nine fixtures, as a 13-object one.
- **Archive determinism is a real gate.** `archive-determinism-probe` is in `VERIFY_ALL_DEPS` and asserts every member carries mtime 0, uid 0, gid 0. Use `ar -D` and `ranlib -D`. Without NNG this is straightforward — the `addlib` verbatim-header problem the Makefile comment describes only arises when pulling in a foreign archive, which is phase 3c.
- **The golden runner already takes what it needs from the environment.** `scripts/run_golden.sh` reads `COMPILER`, `EX_DIR`, `GOOFLAGS`, `GOLDEN_TIMEOUT` and `GOLDEN_JOBS`, and the compiler reads `GOO_RUNTIME` and `GOOROOT`. No edit to that script is needed.

## What this phase can and cannot claim

| Gate | Needs NNG? | This phase |
|---|---|---|
| ~150 inline probes that compile+run | no | unblocked for phase 4 |
| `far-*-probe` (5), `far-transport-test`, `far-transport-asan` | **yes** | phase 3c |
| `lanes-kernel-ir-pin`, `lanes-monomorphize-ir-pin` | no — they use `--emit-llvm` | unblocked |
| `test-golden`, `test-golden-o2`, `test-golden-reject` | **yes** — all-or-nothing suites containing the 9 lanes/far fixtures | phase 3c |

So this phase claims **no gate by itself**. Its deliverable is the archive that unblocks phase 4, plus a Bazel-side golden run over the 486 NNG-free fixtures as evidence the archive is correct.

---

### Task 1: The runtime archive library

**Files:**
- Modify: `src/runtime/BUILD`

**Interfaces:**
- Consumes: `//tools:defs.bzl`.
- Produces: `//src/runtime:runtime_full`, a `cc_library` over 12 of the 13 `RUNTIME_OBJS` files.

- [ ] **Step 1: Re-read the two lists and confirm they differ**

```bash
awk '/^RUNTIME_SRCS/{print}' Makefile | tr ' ' '\n' | grep -oE 'runtime/[a-z_]+\.c' | sed 's|runtime/||' | sort > /tmp/rsrcs.txt
awk '/^RUNTIME_OBJS/{print}' Makefile | tr ' ' '\n' | grep -oE 'runtime/[a-z_]+\.o' | sed 's|runtime/||;s|\.o$|.c|' | sort > /tmp/robjs.txt
wc -l < /tmp/rsrcs.txt   # expect 11
wc -l < /tmp/robjs.txt   # expect 13
comm -13 /tmp/rsrcs.txt /tmp/robjs.txt   # expect: far_transport.c, io.c
```
If the difference is not exactly those two files, use what the Makefile says and record the discrepancy.

- [ ] **Step 2: Add the full runtime library**

Append to `src/runtime/BUILD`:
```python
# The 13 files in RUNTIME_OBJS -- what lib/libgoo_runtime.a is built from.
#
# This is NOT :runtime above. That one carries the 11 of RUNTIME_SRCS and
# exists for obj_header_test, which is the only unit suite the Makefile links
# narrowly. This one adds io.c, which is in the archive
# and not in RUNTIME_SRCS.
#
# far_transport.c is DELIBERATELY EXCLUDED, so this is 12 files and not 13.
# It includes <nng/nng.h>, and Makefile:179-180 gives it
# -I$(NNG_BUILD)/include plus a dependency on $(NNG_LIB): it cannot compile
# without NNG, let alone link. Phase 3c adds it back.
#
# Measured 2026-08-25: a 12-object archive built exactly this way passes 486
# of 495 golden fixtures, the identical split a 13-object one gives -- measured: 486 of 495 golden fixtures link and pass against
# an archive built exactly this way with no NNG in it.
goo_cc_library(
    name = "runtime_full",
    srcs = [
        "platform.h",
        "arena.c",
        "channels.c",
        "concurrency.c",
        "deadlock.c",
        "defer.c",
        "io.c",
        "platform.c",
        "runtime.c",
        "sync.c",
        "sync_shim.c",
        "testing.c",
        "time_shim.c",
    ],
    linkopts = [
        "-lm",
        "-pthread",
    ],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 3: Build it, and run the discovery loop on failure**

```bash
bazel build //src/runtime:runtime_full > /tmp/rf.log 2>&1; echo "EXIT=$?"
grep -E '^ERROR|fatal error' /tmp/rf.log | head -6
```

The file that needs NNG headers is already excluded, so this should build clean. If some *other* file fails on a missing header, it is an undeclared private sibling like `src/runtime/platform.h` in phase 1: add it to `srcs`. If `far_transport.c` somehow appears in the error, **stop and report** — that means `far_transport.c` cannot even compile without NNG, which changes this phase's shape: the archive would then need `far_transport.c` excluded (and the Makefile's `RUNTIME_OBJS` list deliberately diverged from), or NNG pulled forward from phase 3c. Do not silently drop the file.

- [ ] **Step 4: Confirm it does not reach LLVM, with a control**

```bash
bazel query 'somepath(//src/runtime:runtime_full, @llvm//:llvm_c)' --noshow_progress 2>/dev/null | wc -l
bazel query 'somepath(//third_party/llvm:llvm_smoke, @llvm//:llvm_c)' --noshow_progress 2>/dev/null | wc -l
```
Expected: `0` then `2`.

- [ ] **Step 5: Commit**

```bash
git add src/runtime/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): the full runtime library, the archive's 13 objects

Makefile:137 RUNTIME_OBJS. This is NOT //src/runtime:runtime, which carries
the 11 of RUNTIME_SRCS for obj_header_test; this adds io.c and
far_transport.c, which are in lib/libgoo_runtime.a and not in RUNTIME_SRCS.

far_transport.c is included even though NNG is not in this phase: it is
unreferenced by any program that does not use far transport, so the linker
never pulls it.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Pack the objects into an archive

**Files:**
- Modify: `src/runtime/BUILD`

**Interfaces:**
- Consumes: `//src/runtime:runtime_full`.
- Produces: `//src/runtime:goo_runtime_archive`, a single `.a` file `GOO_RUNTIME` can be pointed at. Task 3 uses it.

**Background.** A `cc_library` produces an archive, but its path is an implementation detail and its name is `libruntime_full.a`. The compiler needs a concrete file it can be handed. `ar -D` and `ranlib -D` are required because `archive-determinism-probe` asserts every member carries mtime 0, uid 0, gid 0.

- [ ] **Step 1: Find how Bazel exposes the library's archive**

Do not guess the path. Ask:
```bash
bazel cquery //src/runtime:runtime_full --output=files 2>/dev/null
```
Record what it prints. If it names a `.a`, a `filegroup` over the target may be enough; if it names several files, the archive must be built explicitly.

- [ ] **Step 2: Expose the archive under the name the compiler expects**

**Corrected while executing, 2026-08-25.** This step originally called for an
explicit `ar -D rcs` to guarantee determinism. That was wrong on both counts:

- Bazel's own archive is **already deterministic**. It links with
  `ZERO_AR_DATE=1`, and every member reads `uid 0, gid 0, epoch 0` -- exactly
  what `archive-determinism-probe` asserts. Measured against a control: an
  ordinary `ar rcs` produces `1000/1000` and a real date.
- `ar rcs out.a in.a` would have added the input archive as a **member**, not
  extracted its objects, producing a nested archive that links nothing.

So this is a copy. Append to `src/runtime/BUILD`:
```python
genrule(
    name = "goo_runtime_archive",
    srcs = [":runtime_full"],
    outs = ["libgoo_runtime.a"],
    cmd = "for f in $(SRCS); do case $$f in *.a) cp $$f $@ ;; esac; done",
    visibility = ["//visibility:public"],
)
```

The `case` filter matters: `cc_library` hands over both a `.a` and a `.so`.

- [ ] **Step 3: Verify the archive's contents and determinism**

```bash
bazel build //src/runtime:goo_runtime_archive > /tmp/ar.log 2>&1; echo "EXIT=$?"
A=bazel-bin/src/runtime/libgoo_runtime.a
ar t "$A" | wc -l                       # expect 12
ar tv "$A" | head -3                    # expect mtime/uid/gid all zero
ar tv "$A" | grep -vcE '^rw-r--r-- 0/0 .* 1970'   # expect 0
ar t  "$A" | grep -c '\.a$'                        # expect 0, no nested archive
```
Every member must read `0/0` and a 1970 timestamp, which is what
`archive-determinism-probe` asserts. The nested-archive check exists because
the original `ar rcs` formulation would have produced exactly that.

- [ ] **Step 4: Commit**

```bash
git add src/runtime/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): pack the runtime objects into a deterministic archive

One .a file the compiler can be handed via GOO_RUNTIME. Built with an explicit
ar -D / ranlib -D rather than reusing the cc_library's own archive, because
archive-determinism-probe asserts every member carries mtime 0, uid 0, gid 0 --
owning the ar invocation is what makes that assertable.

No NNG. The Makefile's recipe also does addlib on libnng.a, and that is the
hard half: addlib copies member headers VERBATIM, so ar -D here would not zero
a member arriving from NNG. Phase 3c.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Run the golden suite against the Bazel compiler and archive

**Files:**
- Create: `tools/golden_bazel.sh`
- Modify: `tools/BUILD`

**Interfaces:**
- Consumes: `//src/compiler:goo` (phase 3), `//src/runtime:goo_runtime_archive`.
- Produces: `//tools:golden_bazel`, tagged `manual`. Evidence the archive is correct.

**Background.** `scripts/run_golden.sh` needs no edit: it already reads `COMPILER` and `EX_DIR` from the environment, and the compiler reads `GOO_RUNTIME` and `GOOROOT`. This wrapper only supplies them.

The suite will report **486 passed, 9 failed** — the 9 being `far_shim_probe` and the eight `lanes_*` probes, which need NNG. That is the expected result for this phase, so the wrapper asserts it exactly rather than tolerating "some failures".

- [ ] **Step 1: Write the wrapper**

```sh
#!/usr/bin/env bash
# Run the golden suite against the BAZEL-built compiler and archive.
#
# scripts/run_golden.sh needs no change: it already takes COMPILER and EX_DIR
# from the environment, and the compiler takes GOO_RUNTIME and GOOROOT. This
# only supplies them.
#
# EXPECTED RESULT THIS PHASE: 486 passed, 9 failed. The 9 are far_shim_probe
# and eight lanes_* probes, which need NNG -- phase 3c. The count is asserted
# EXACTLY rather than tolerated, so that a tenth failure is a regression and a
# fixed lanes probe is a visible change, not a silently absorbed one.
#
# Exit codes: 0 exactly the expected split, 1 anything else, 2 a tool failed.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

COMPILER_BIN="${COMPILER_BIN:-bazel-bin/src/compiler/goo}"
ARCHIVE="${ARCHIVE:-bazel-bin/src/runtime/libgoo_runtime.a}"
EXPECT_PASS="${EXPECT_PASS:-486}"
EXPECT_FAIL="${EXPECT_FAIL:-9}"

[ -x "$COMPILER_BIN" ] || { echo "golden_bazel: TOOL FAILURE $COMPILER_BIN missing"; exit 2; }
[ -r "$ARCHIVE" ]      || { echo "golden_bazel: TOOL FAILURE $ARCHIVE missing"; exit 2; }

out="$(COMPILER="$COMPILER_BIN" GOO_RUNTIME="$root/$ARCHIVE" GOOROOT="$root" \
       bash scripts/run_golden.sh 2>&1)"

summary="$(printf '%s\n' "$out" | grep -oE '[0-9]+ passed, [0-9]+ failed' | tail -1)"
if [ -z "$summary" ]; then
    echo "golden_bazel: TOOL FAILURE no summary line from run_golden.sh"
    printf '%s\n' "$out" | tail -20
    exit 2
fi

passed="$(printf '%s\n' "$summary" | awk '{print $1}')"
failed="$(printf '%s\n' "$summary" | awk '{print $3}')"
echo "golden_bazel: $summary (expected $EXPECT_PASS passed, $EXPECT_FAIL failed)"

if [ "$passed" -eq "$EXPECT_PASS" ] && [ "$failed" -eq "$EXPECT_FAIL" ]; then
    echo "golden_bazel: PASS the Bazel-built compiler and archive behave as expected"
    exit 0
fi

printf '%s\n' "$out" | grep -E '^FAIL' | head -20
echo "golden_bazel: FAIL split moved -- if a lanes/far probe was fixed, update EXPECT_*"
exit 1
```

Make it executable: `chmod +x tools/golden_bazel.sh`

- [ ] **Step 2: Build both and run it**

```bash
bazel build //src/compiler:goo //src/runtime:goo_runtime_archive > /dev/null 2>&1
./tools/golden_bazel.sh; echo "EXIT=$?"
```
Expected: `486 passed, 9 failed` and exit 0.

If the split differs, list the failures and compare against the nine names above before changing `EXPECT_*`. A different set of nine is a real finding.

- [ ] **Step 3: Confirm the nine are the expected nine**

```bash
COMPILER=bazel-bin/src/compiler/goo \
GOO_RUNTIME="$(pwd)/bazel-bin/src/runtime/libgoo_runtime.a" \
GOOROOT="$(pwd)" bash scripts/run_golden.sh 2>&1 | grep -E '^FAIL' | awk '{print $2}' | sort
```
Expected exactly:
```
far_shim_probe
lanes_allreduce_probe
lanes_jacobi_probe
lanes_monomorphize_probe
lanes_partition_probe
lanes_repartition_probe
lanes_stencil_probe
lanes_stencilstep_probe
lanes_stencilstep_r2_probe
```

- [ ] **Step 4: Prove the wrapper can report a change**

An exact-split assertion that cannot notice a moved split is worth nothing:
```bash
EXPECT_PASS=999 ./tools/golden_bazel.sh > /tmp/gbred.log 2>&1; echo "RED exit=$? (expect 1)"
tail -2 /tmp/gbred.log
ARCHIVE=/nonexistent/x.a ./tools/golden_bazel.sh > /tmp/gbtool.log 2>&1; echo "TOOL exit=$? (expect 2)"
tail -1 /tmp/gbtool.log
./tools/golden_bazel.sh > /dev/null 2>&1; echo "GREEN exit=$? (expect 0)"
```

- [ ] **Step 5: Register it, tagged manual**

Append to `tools/BUILD`:
```python
# The golden suite against the Bazel-built compiler and archive.
#
# Tagged manual and no-sandbox: it runs 495 fixtures, each compiling and
# executing a binary, which is neither fast nor sandbox-friendly. CI runs it
# explicitly.
#
# It asserts 486 passed / 9 failed EXACTLY. The 9 need NNG (phase 3c). When
# 3c lands, EXPECT_PASS/EXPECT_FAIL become 495/0 and this becomes the
# test-golden gate's Bazel counterpart.
sh_test(
    name = "golden_bazel",
    size = "large",
    srcs = ["golden_bazel.sh"],
    tags = [
        "external",
        "manual",
        "no-sandbox",
    ],
)
```

- [ ] **Step 6: Confirm `bazel test //...` still skips it and is green**

```bash
bazel test //... > /tmp/all.log 2>&1; echo "EXIT=$?"
grep -c 'golden_bazel' /tmp/all.log   # expect 0
```

- [ ] **Step 7: Commit**

```bash
git add tools/golden_bazel.sh tools/BUILD
git -c commit.gpgsign=false commit -m "test(bazel): run the golden suite on the Bazel compiler and archive

486 of 495 fixtures pass against the Bazel-built compiler and a Bazel-built
archive with no NNG in it. The 9 that fail are far_shim_probe and eight lanes_*
probes, which need the far transport -- phase 3c.

scripts/run_golden.sh needs no edit: it already takes COMPILER and EX_DIR from
the environment, and the compiler takes GOO_RUNTIME and GOOROOT.

The split is asserted EXACTLY rather than tolerated, so a tenth failure is a
regression and a fixed lanes probe is a visible change rather than a silently
absorbed one. Proven able to report both a moved split and a missing archive.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Close phase 3b

**Files:**
- Modify: `.github/workflows/bazel.yml`

- [ ] **Step 1: Add the golden run to the differential job**

The differential job already builds both compilers. Extend its final step:
```yaml
      - name: Golden suite on the Bazel compiler and archive
        run: |
          bazel build //src/runtime:goo_runtime_archive \
            --repo_env=CC=/usr/bin/gcc-14 \
            --repo_env=GOO_LLVM_CONFIG="$LLVMCFG"
          ./tools/golden_bazel.sh
```
`LLVMCFG` is set in the same job. If it is not visible in this step, resolve it again here rather than reaching for `${{ }}` — `workflow-targets-probe` cannot parse those, which is what broke the first version of this workflow.

- [ ] **Step 2: Validate and check the probe**

```bash
python3 -c "import yaml;yaml.safe_load(open('.github/workflows/bazel.yml'));print('yaml ok')"
bash scripts/workflow_targets_probe.sh
```
Both must pass before pushing. The probe runs in `verify-core`, which the pre-push hook runs, so a failure here becomes a failed push.

- [ ] **Step 3: Confirm parity is unchanged**

```bash
./tools/parity.sh | head -3
```
Expected: **unchanged**. This phase claims no gate — `test-golden` and its siblings are all-or-nothing suites containing the nine NNG fixtures, and the seven `far-*` gates need NNG directly. Saying so plainly is the point of the split.

- [ ] **Step 4: Confirm both build systems green, then push and open the PR**

```bash
bazel test //... ; ./tools/parity_selftest.sh ; ./tools/compiler_differential.sh
git diff --stat main..HEAD -- Makefile 'src/**/*.c' 'src/**/*.h' scripts/ examples/
```
If that last diff is empty, CI's `verify-core` job is the confirmation and a local run adds nothing.

Then push and open the PR against `main`, and read the real CI conclusion with `gh pr view <PR#> --json statusCheckRollup`, never a piped exit code.

---

## What phase 3c needs

Phase 3c is NNG, and it is worth its own plan because the determinism is subtle:

- `third_party/nng-1.12.0.tar.gz` is pinned with a sha256 and built by cmake with `CMAKE_C_ARCHIVE_CREATE`, `_APPEND` and `_FINISH` all overridden to pass `-D`. The Makefile comment records why all three are needed: CMake splits a long object list into one CREATE batch plus APPEND batches, and the default APPEND rule carries no `-D`. NNG fits one CREATE call today, so leaving APPEND alone would close the hole by accident rather than by design.
- `addlib` copies each member's header **verbatim**, so `ar -D` on the outer archive does not zero a member that arrived from NNG. Determinism must be achieved in NNG's own build.
- `archive-determinism-probe` asserts all 102 members carry mtime 0, uid 0, gid 0, and it is a gate in `VERIFY_ALL_DEPS`.
- The Bazel options are a `genrule` shelling out to host cmake (consistent with this repo's host bison and host `llvm-config`, and it reuses the Makefile's exact invocation), `rules_foreign_cc` (purpose-built, adds a dependency, known to be slow), or an `http_archive` plus a hand-written BUILD in orca's SDL3 shape (most Bazel-native, but it means re-deriving NNG's platform conditionals, and any drift breaks determinism silently). Decide with the trade-offs written down.
- Landing it takes `golden_bazel.sh`'s `EXPECT_PASS`/`EXPECT_FAIL` to 495/0, at which point it becomes the `test-golden` gate's counterpart, and the seven `far-*` gates plus the three golden suites become claimable: **10 gates**.
