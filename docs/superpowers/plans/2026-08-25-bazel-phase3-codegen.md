# Bazel Migration — Phase 3 (codegen and the compiler) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `//src/compiler:goo` under Bazel — the whole LLVM-dependent half of the compiler — and prove it emits byte-identical LLVM IR to `make bin/goo` across all 495 golden fixtures.

**Architecture:** `//src/codegen` is the first target to take `@llvm//:llvm_c`. `//src/compiler:goo` links it to the phase 2 front end. Equivalence is proven on emitted IR rather than on run behaviour, which needs no runtime archive and therefore no NNG. Nothing is removed from the Makefile.

**Tech Stack:** Bazel 9.2.0, `rules_cc` 0.2.17, `rules_shell` 0.6.1, LLVM 22 C API (18 on CI), GCC, C23.

**Spec:** `docs/superpowers/specs/2026-08-25-bazel-migration-design.md`
**Predecessors:** phases 0–1 (PR #321), phase 2 (PR #322)

## Global Constraints

Everything in the phase 0–1 and phase 2 plans' Global Constraints still applies. In addition:

- **This phase does NOT build the runtime archive, and does not touch NNG.** `lib/libgoo_runtime.a` is 13 runtime objects plus the whole of `libnng.a`, and NNG is a pinned cmake tarball (`third_party/nng-1.12.0.tar.gz`, sha256-checked). Porting it is phase 3b. Mixing a third-party cmake port into the LLVM port would put two hard problems in one phase and make a failure unattributable.
- **Equivalence is proven on emitted IR, not on run behaviour.** Verified 2026-08-25: `--emit-llvm` succeeds with `GOO_RUNTIME=/nonexistent` (exit 0, 12,037 bytes), while a full compile with the same setting fails at `ld`. So the IR path needs no archive at all.
- **The IR comparison needs NO normalisation, provided both compilers get the same `-o` BASENAME.** Verified 2026-08-25: the same basename in two different directories produces byte-identical output, and a different basename changes exactly two lines (`ModuleID` and `source_filename`). There are no absolute paths and no `GOO_VERSION` in the IR.
- **`//src/runtime:runtime` is NOT the runtime archive and must not be confused with it.** Phase 1 built it from `RUNTIME_SRCS` (11 files) for `obj_header_test`. The archive is built from `RUNTIME_OBJS` (13 files, adding `io.c` and `far_transport.c`) plus NNG, 102 members total. Phase 3b resolves this.
- **The differential harness is a MIGRATION-TIME check, not a permanent gate.** It compares two build systems, so it cannot be hermetic, and it becomes meaningless when the Makefile is deleted in phase 7. It is therefore tagged `manual`/`no-sandbox` and run explicitly in CI, in the same shape as `tools/parity_selftest.sh`. **Phase 7 deletes it.**
- **Do not pin an IR baseline.** A checked-in hash-per-fixture manifest would fire on every legitimate codegen change and train reviewers to regenerate it without looking. The question here is "do the two compilers agree today", which is answered by running both.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/codegen/BUILD` | `cc_library` over the 15 `CODEGEN_SRCS`, the first target to take `@llvm//:llvm_c` | 1 |
| `src/compiler/BUILD` | `cc_library` for `test_discovery.c`, `cc_binary` `goo` | 2 |
| `tools/compiler_differential.sh` | Runs both compilers over the fixtures and diffs the IR | 4 |
| `tools/BUILD` | `sh_test` for the differential, tagged manual | 4 |
| `.github/workflows/bazel.yml` | A job that builds both and runs the differential | 6 |

---

### Task 1: The codegen library

**Files:**
- Create: `src/codegen/BUILD`

**Interfaces:**
- Consumes: `@llvm//:llvm_c` (phase 0), `//src/types:frontend`, `//src/ast`, `//src/parser` (phase 2).
- Produces: `//src/codegen`. Task 2 depends on it.

**Background.** `Makefile:85` `CODEGEN_SRCS` names 15 files. All 15 reach LLVM: 14 include `codegen.h` or `codegen_cfctx.h` directly, and `value_scope.c` reaches it through `include/value_scope.h`. This is the first target in the repo to depend on `@llvm//:llvm_c`.

- [ ] **Step 1: Re-read the source list**

```bash
awk '/^CODEGEN_SRCS/{print}' Makefile | tr ' ' '\n' | grep -oE 'codegen/[a-z_]+\.c' | sed 's|codegen/||' | sort
```
Expected: 15 files. Use this output rather than the list below if they differ.

- [ ] **Step 2: Create `src/codegen/BUILD`**

```python
load("//tools:defs.bzl", "goo_cc_library")

# Makefile:85 CODEGEN_SRCS. The first target in this repo to take
# @llvm//:llvm_c.
#
# All 15 reach LLVM: 14 include codegen.h or codegen_cfctx.h directly, and
# value_scope.c reaches it through include/value_scope.h. That is the whole
# LLVM surface of the compiler apart from src/compiler/goo.c.
goo_cc_library(
    name = "codegen",
    srcs = [
        "call_codegen.c",
        "cfctx.c",
        "codegen.c",
        "composite_codegen.c",
        "error_union_codegen.c",
        "expression_codegen.c",
        "function_codegen.c",
        "interface_codegen.c",
        "lowlevel_codegen.c",
        "monomorphize.c",
        "nullable_codegen.c",
        "runtime_integration.c",
        "statement_codegen.c",
        "type_mapping.c",
        "value_scope.c",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//src/ast",
        "//src/comptime",
        "//src/errors",
        "//src/parser",
        "//src/types:frontend",
        "@llvm//:llvm_c",
    ],
)
```

- [ ] **Step 3: Build, and run the discovery loop on failure**

```bash
bazel build //src/codegen > /tmp/cg.log 2>&1; echo "EXIT=$?"
grep -E '^ERROR|fatal error' /tmp/cg.log | head -6
```

Read the error text before pattern-matching it. A `fatal error: X.h` means an undeclared private header — the class of `src/runtime/platform.h` in phase 1; add it to `srcs`. For an `undefined symbol:` at link time (this toolchain is `ld.lld`, not GNU ld), find the definer and add its library.

Stop and report if the loop needs a dependency on `//src/runtime` — codegen should reference the runtime by emitting calls to it, never by linking it.

- [ ] **Step 4: Confirm this target DOES reach LLVM, and that it is the only new one**

Every prior phase asserted the opposite, so this is the first target where a path is expected:
```bash
bazel query 'somepath(//src/codegen, @llvm//:llvm_c)' --noshow_progress 2>/dev/null
```
Expected: a path, at least two nodes. An empty result here means `@llvm//:llvm_c` is not actually being used and the includes are resolving some other way — stop and find out how.

- [ ] **Step 5: Confirm the front end still does NOT reach LLVM**

Adding codegen must not retroactively contaminate phase 2's targets:
```bash
for t in //src/types:frontend //src/parser //src/lexer //src/ast; do
  printf '%-24s %s\n' "$t" "$(bazel query "somepath($t, @llvm//:llvm_c)" --noshow_progress 2>/dev/null | wc -l)"
done
```
Expected: `0` for all four.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): the codegen library, the first target to take LLVM

Makefile:85 CODEGEN_SRCS, all 15 files. Fourteen include codegen.h or
codegen_cfctx.h directly and value_scope.c reaches LLVM through
include/value_scope.h, so the whole directory depends on @llvm//:llvm_c.

Confirmed by query that this target does reach @llvm//:llvm_c -- the first in
the repo to do so -- and that //src/types:frontend, //src/parser, //src/lexer
and //src/ast still do not.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: The compiler binary

**Files:**
- Create: `src/compiler/BUILD`

**Interfaces:**
- Consumes: `//src/codegen`.
- Produces: `//src/compiler:goo`, a `cc_binary`. Task 4 runs it.

**Background.** `Makefile:99` `COMPILER_SRCS = compiler/goo.c compiler/test_discovery.c`. `goo.c` includes `codegen.h`; `test_discovery.c` does not touch LLVM. `Makefile` also sets `-DGOO_VERSION='"$(GOO_VERSION)"'` target-specifically, with `GOO_VERSION ?= 0.1.0`, and `goo.c:30` carries an `#ifndef` fallback to the same string.

- [ ] **Step 1: Confirm the version handling before choosing a copt**

```bash
grep -nE 'GOO_VERSION' src/compiler/goo.c | head -4
grep -nE '^GOO_VERSION' Makefile
```
Expected: `goo.c:30-31` defines `"0.1.0"` under `#ifndef`, and the Makefile default is the same string. The two must agree, and `release-package-probe` would catch a drift.

- [ ] **Step 2: Create `src/compiler/BUILD`**

```python
load("@rules_cc//cc:defs.bzl", "cc_binary")
load("//tools:defs.bzl", "goo_cc_library")

# Makefile:99 COMPILER_SRCS, split by whether it touches LLVM.
# test_discovery.c does not, so it stays its own library.
goo_cc_library(
    name = "test_discovery",
    srcs = ["test_discovery.c"],
    visibility = ["//visibility:public"],
)

# NO -DGOO_VERSION here, deliberately. The Makefile sets it target-specifically
# with `GOO_VERSION ?= 0.1.0`, and goo.c:30-32 carries an #ifndef fallback to
# exactly that same string -- so omitting the define produces an identical
# binary and avoids a fragile nested-quoting copt. Task 2 Step 4 checks both
# binaries print the same --version. If a release ever sets GOO_VERSION to
# something else, this target needs a `defines` entry to match; the IR
# comparison is unaffected either way, because the version does not appear in
# emitted IR.
cc_binary(
    name = "goo",
    srcs = ["goo.c"],
    copts = [
        "-include",
        "include/xalloc.h",
        "-include",
        "include/goo_assert.h",
        "-I.",
        "-Iinclude",
        "-D_GNU_SOURCE",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":test_discovery",
        "//include:headers",
        "//include:prelude",
        "//src/codegen",
        "//src/types:frontend",
    ],
)
```

The `copts` are spelled out rather than using `goo_cc_library` because that macro only wraps `cc_library` and `cc_test`. If `tools/defs.bzl` later grows a `goo_cc_binary`, move this to it rather than keeping a second copy of `PRELUDE_COPTS`.

- [ ] **Step 3: Build and run the discovery loop**

```bash
bazel build //src/compiler:goo > /tmp/goo.log 2>&1; echo "EXIT=$?"
grep -E '^ERROR|undefined symbol|fatal error' /tmp/goo.log | head -8
```

This is where the full link happens, so this is where a missing library shows up. Resolve as in Task 1.

- [ ] **Step 4: Smoke-test the binary**

```bash
./bazel-bin/src/compiler/goo --version; echo "EXIT=$?"
./bin/goo --version
```
Expected: both print `Goo Compiler v0.1.0` and exit 0. A difference means the `-DGOO_VERSION` copt disagrees with the Makefile.

- [ ] **Step 5: Prove it emits IR without a runtime archive**

```bash
GOO_RUNTIME=/nonexistent ./bazel-bin/src/compiler/goo --emit-llvm \
    examples/switch_probe.goo -o /tmp/bz.ll > /tmp/bzemit.log 2>&1; echo "EXIT=$?"
wc -c < /tmp/bz.ll
```
Expected: exit 0 and a non-empty file. If it fails, the IR path is reaching the linker and Task 4's whole design is wrong — stop and report.

- [ ] **Step 6: The first real comparison, on one fixture**

```bash
mkdir -p /tmp/irA /tmp/irB
GOO_RUNTIME=/nonexistent ./bin/goo --emit-llvm examples/switch_probe.goo -o /tmp/irA/out.ll >/dev/null 2>&1
GOO_RUNTIME=/nonexistent ./bazel-bin/src/compiler/goo --emit-llvm examples/switch_probe.goo -o /tmp/irB/out.ll >/dev/null 2>&1
diff /tmp/irA/out.ll /tmp/irB/out.ll && echo "IDENTICAL"
```
Expected: `IDENTICAL`. The basenames match deliberately — a different basename changes `ModuleID` and `source_filename` and nothing else.

If this differs, do NOT proceed to Task 4. Read the diff: a difference here is either a real codegen divergence or a compiler-flag difference between the two builds, and Task 4 would just report it 495 times.

- [ ] **Step 7: Commit**

```bash
git add src/compiler/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): the compiler binary

Makefile:99 COMPILER_SRCS, split by whether it touches LLVM: test_discovery.c
does not and stays its own library, goo.c does and links //src/codegen.

GOO_VERSION mirrors the Makefile's default and goo.c's #ifndef fallback, both
0.1.0. Both binaries print the same --version string.

The Bazel-built compiler emits IR with GOO_RUNTIME pointing at nothing, and
its IR for examples/switch_probe.goo is byte-identical to bin/goo's.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Confirm the LLVM boundary claim end to end

**Files:** none. This task measures.

**Interfaces:**
- Consumes: `//src/compiler:goo`.
- Produces: evidence for the spec's claim about which files reach LLVM.

**Background.** The spec states LLVM reaches exactly the 15 files in `src/codegen/` plus `src/compiler/goo.c`. That was measured by reading includes. Now that a Bazel graph exists, it can be measured from the graph instead, which is the stronger instrument.

- [ ] **Step 1: Ask the graph which targets reach LLVM**

```bash
bazel query 'rdeps(//..., @llvm//:llvm_c)' --noshow_progress 2>/dev/null | sort
```
Expected: `@llvm//:llvm_c` itself, `//src/codegen`, `//src/compiler:goo`, and `//third_party/llvm:llvm_smoke`. Nothing else.

- [ ] **Step 2: Compare against the include-based measurement**

```bash
grep -rlE '#\s*include\s*[<"](codegen|codegen_cfctx|value_scope)\.h[>"]' src/ --include='*.c' | sort | wc -l
```
Expected: 15 (14 codegen + `goo.c`). `value_scope.c` is the 16th file, reached transitively, so the two counts differ by exactly one and both are correct — record which is which so the spec is not later "corrected" wrongly.

- [ ] **Step 3: Update the spec if either number is wrong**

The spec currently says "exactly 15 `.c` files -- the 14 in `src/codegen/` plus `src/compiler/goo.c`". If Step 1 and Step 2 show it is 16 (all 15 codegen files plus `goo.c`), fix it and say so in the commit.

- [ ] **Step 4: Commit any spec correction**

```bash
git add docs/superpowers/specs/2026-08-25-bazel-migration-design.md
git -c commit.gpgsign=false commit -m "docs(build): measure the LLVM boundary from the build graph

The spec's file list was measured by reading #include lines. With a Bazel
graph in place it can be measured from the graph instead, which is the
stronger instrument: bazel query rdeps(//..., @llvm//:llvm_c) states the
answer rather than inferring it from text.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: The IR differential harness

**Files:**
- Create: `tools/compiler_differential.sh`
- Modify: `tools/BUILD`

**Interfaces:**
- Consumes: `//src/compiler:goo` and a Make-built `bin/goo`.
- Produces: `//tools:compiler_differential`, tagged `manual`. Task 6 runs it in CI.

**Background.** This compares two build systems, so it cannot be hermetic and must not be a plain `bazel test`. It is tagged the way `tools/parity_selftest.sh` is, and **phase 7 deletes it** — once the Makefile is gone the question it answers no longer exists.

- [ ] **Step 1: Confirm the fixture count**

```bash
n=0; for f in examples/*.goo; do [ -f "${f%.goo}.expected.txt" ] && n=$((n+1)); done; echo "$n"
```
Expected: 495. This is the same set `scripts/run_golden.sh` uses (`EX_DIR` defaults to `examples`).

- [ ] **Step 2: Write the harness**

```sh
#!/usr/bin/env bash
# Compare the Bazel-built compiler against the Make-built one on emitted IR.
#
# WHAT THIS IS. A migration-time check, not a permanent gate. It compares two
# BUILD SYSTEMS, so it cannot be hermetic, and the question it answers stops
# existing when the Makefile is deleted in phase 7. PHASE 7 DELETES THIS FILE.
#
# WHY IR RATHER THAN RUN BEHAVIOUR. --emit-llvm needs no runtime archive:
# verified 2026-08-25 that it exits 0 with GOO_RUNTIME=/nonexistent while a
# full compile fails at ld. That keeps NNG out of this phase entirely. It is
# also a STRONGER equivalence claim than matching stdout, because two
# different IRs can print the same thing.
#
# WHY NO NORMALISATION. The IR embeds ModuleID and source_filename, both
# derived from the -o BASENAME, plus a host-derived target triple. Give both
# compilers the same basename in different directories and the output is
# byte-identical. Verified 2026-08-25.
#
# Exit codes: 0 every fixture identical, 1 a fixture differs, 2 a tool failed.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

MAKE_GOO="${MAKE_GOO:-bin/goo}"
BAZEL_GOO="${BAZEL_GOO:-bazel-bin/src/compiler/goo}"
EX_DIR="${EX_DIR:-examples}"

for c in "$MAKE_GOO" "$BAZEL_GOO"; do
    if [ ! -x "$c" ]; then
        echo "compiler_differential: TOOL FAILURE $c is not executable"
        echo "  build both first: make bin/goo && bazel build //src/compiler:goo"
        exit 2
    fi
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/a" "$work/b"

total=0; same=0; differ=0; failed=0
declare -a bad=()

for src in "$EX_DIR"/*.goo; do
    [ -f "${src%.goo}.expected.txt" ] || continue
    base="$(basename "$src" .goo)"
    total=$((total + 1))

    # Same BASENAME in both directories: that is what makes the IR comparable
    # with no normalisation.
    GOO_RUNTIME=/nonexistent "$MAKE_GOO"  --emit-llvm "$src" -o "$work/a/out.ll" >/dev/null 2>&1
    rc_a=$?
    GOO_RUNTIME=/nonexistent "$BAZEL_GOO" --emit-llvm "$src" -o "$work/b/out.ll" >/dev/null 2>&1
    rc_b=$?

    if [ "$rc_a" -ne "$rc_b" ]; then
        failed=$((failed + 1)); bad+=("$base (exit $rc_a vs $rc_b)"); continue
    fi
    if [ "$rc_a" -ne 0 ]; then
        # Both refused the same fixture. That is agreement, not a failure --
        # the reject fixtures live here too.
        same=$((same + 1)); continue
    fi
    if cmp -s "$work/a/out.ll" "$work/b/out.ll"; then
        same=$((same + 1))
    else
        differ=$((differ + 1)); bad+=("$base (IR differs)")
    fi
done

echo "compiler_differential: $total fixtures"
echo "  identical: $same"
echo "  differing: $differ"
echo "  exit-code mismatch: $failed"

if [ "$total" -eq 0 ]; then
    echo "compiler_differential: TOOL FAILURE no fixtures found under $EX_DIR"
    exit 2
fi
if [ "$differ" -eq 0 ] && [ "$failed" -eq 0 ]; then
    echo "compiler_differential: PASS both compilers emit identical IR"
    exit 0
fi
printf '  %s\n' "${bad[@]}" | head -40
echo "compiler_differential: FAIL"
exit 1
```

Make it executable: `chmod +x tools/compiler_differential.sh`

Note the empty-corpus guard. `scripts/run_golden.sh` had exactly this hole — it exited 0 having compared nothing — and `5c633f6` fixed it. Do not reintroduce it here.

- [ ] **Step 3: Build both and run it**

```bash
make bin/goo > /tmp/mk.log 2>&1; echo "make exit=$?"
bazel build //src/compiler:goo > /tmp/bz.log 2>&1; echo "bazel exit=$?"
./tools/compiler_differential.sh; echo "EXIT=$?"
```
Expected: `495 fixtures`, `identical: 495`, `differing: 0`, exit 0.

If any fixture differs, stop and read one diff before changing anything. A systematic difference across every fixture is a build-flag difference; a difference on a handful is a real codegen divergence and is a finding worth its own investigation.

- [ ] **Step 4: Register it, tagged manual**

Append to `tools/BUILD`:
```python
# Compares the Bazel-built compiler against the Make-built one on emitted IR.
#
# Tagged manual and no-sandbox because it needs BOTH build systems, so it can
# never be hermetic and `bazel test //...` must not try to run it. CI runs it
# explicitly, the way it runs parity_selftest.sh.
#
# PHASE 7 DELETES THIS TARGET AND ITS SCRIPT. Once the Makefile is gone the
# question it answers no longer exists.
sh_test(
    name = "compiler_differential",
    size = "large",
    srcs = ["compiler_differential.sh"],
    tags = [
        "external",
        "manual",
        "no-sandbox",
    ],
)
```

- [ ] **Step 5: Confirm `bazel test //...` still skips it**

```bash
bazel test //... > /tmp/all.log 2>&1; echo "EXIT=$?"
grep -c 'compiler_differential' /tmp/all.log
```
Expected: exit 0 and `0` — `//...` must not run it.

- [ ] **Step 6: Commit**

```bash
git add tools/compiler_differential.sh tools/BUILD
git -c commit.gpgsign=false commit -m "test(bazel): prove both compilers emit identical IR

495 golden fixtures, compiled with --emit-llvm by both bin/goo and
bazel-bin/src/compiler/goo, output compared byte for byte.

IR rather than run behaviour, for two reasons. --emit-llvm needs no runtime
archive -- verified that it exits 0 with GOO_RUNTIME=/nonexistent while a full
compile fails at ld -- which keeps NNG out of this phase entirely. And it is a
stronger claim than matching stdout, because two different IRs can print the
same thing.

No normalisation is needed. The IR embeds ModuleID and source_filename, both
derived from the -o basename, so giving both compilers the same basename in
different directories makes the output directly comparable.

Tagged manual and no-sandbox: it needs both build systems, so it can never be
hermetic and bazel test //... must not run it. It carries an empty-corpus
guard, because scripts/run_golden.sh had exactly that hole and exited 0 having
compared nothing (fixed in 5c633f6).

PHASE 7 DELETES THIS. Once the Makefile is gone the question stops existing.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Prove the differential can report a difference

**Files:** none. This task runs a mutation.

**Interfaces:**
- Consumes: `//tools:compiler_differential`.
- Produces: evidence that a real codegen divergence would be caught.

**Background.** A harness that compares two builds of the same source will pass on day one and prove nothing. It must be shown to fail when the compilers genuinely disagree.

- [ ] **Step 1: Confirm the anchor reaches emitted IR before using it**

The mutation must change output for EVERY fixture, or a green result means
nothing. `"entry"` is the LLVM basic-block name that `function_codegen.c` gives
every compiled function, so it appears in every fixture's IR:

```bash
grep -c '"entry"' src/codegen/function_codegen.c        # expect 5
GOO_RUNTIME=/nonexistent ./bin/goo --emit-llvm examples/switch_probe.goo -o /tmp/anchor.ll >/dev/null 2>&1
grep -c 'entry:' /tmp/anchor.ll                          # expect >= 1
```

Do NOT use `goo_pkg__` as the anchor. It appears 4 times in `codegen.c` but
**zero** times in a simple fixture's IR -- it is the package-qualified mangling
prefix, so it only shows up for package symbols. Mutating it would leave most
fixtures identical, and the differential would report a near-pass that means
nothing.

- [ ] **Step 2: Make the two compilers genuinely differ**

```bash
cp src/codegen/function_codegen.c /tmp/cg.bak
cat > /tmp/mut_cg.py <<'EOF'
import io
p = 'src/codegen/function_codegen.c'
s = io.open(p, encoding='utf-8').read()
old = '"entry"'
n = s.count(old)
assert n > 0, "anchor not found -- do not proceed"
io.open(p, 'w', encoding='utf-8').write(s.replace(old, '"entryX"'))
print("MUTATION APPLIED (%d sites)" % n)
EOF
python3 /tmp/mut_cg.py
diff /tmp/cg.bak src/codegen/function_codegen.c | head -4
```

- [ ] **Step 3: Rebuild ONLY the Bazel side, then run the differential**

```bash
bazel build //src/compiler:goo > /dev/null 2>&1
./tools/compiler_differential.sh > /tmp/diffred.log 2>&1; echo "RED exit=$? (expect 1)"
head -6 /tmp/diffred.log
```
Expected: exit 1, with a non-zero `differing:` count. `bin/goo` is stale and therefore still carries the unmutated codegen, which is exactly the divergence being simulated.

- [ ] **Step 4: Restore, rebuild both, confirm GREEN**

```bash
cp /tmp/cg.bak src/codegen/function_codegen.c
bazel build //src/compiler:goo > /dev/null 2>&1
make bin/goo > /dev/null 2>&1
./tools/compiler_differential.sh > /tmp/diffgreen.log 2>&1; echo "GREEN exit=$? (expect 0)"
tail -2 /tmp/diffgreen.log
git diff --stat src/codegen/function_codegen.c
```
Expected: exit 0, and `git diff --stat` prints nothing.

- [ ] **Step 5: Prove the empty-corpus guard fires**

```bash
EX_DIR=/tmp/definitely-empty ./tools/compiler_differential.sh; echo "EXIT=$? (expect 2)"
```
Expected: exit 2 with `no fixtures found`. A harness that reports success having compared nothing is the defect `5c633f6` fixed in the golden runner.

- [ ] **Step 6: Commit the evidence**

```bash
git -c commit.gpgsign=false commit --allow-empty -m "test(bazel): record that the IR differential can report a difference

A harness comparing two builds of the same source passes on day one and proves
nothing. Mutated the entry basic-block label in src/codegen/function_codegen.c, rebuilt
ONLY the Bazel side so the two compilers genuinely disagreed, and confirmed the
differential exits 1 with a non-zero differing count. Restored, rebuilt both,
exit 0, clean tree.

Also confirmed the empty-corpus guard: EX_DIR pointed at an empty directory
exits 2 rather than reporting success having compared nothing. That is the
defect 5c633f6 fixed in scripts/run_golden.sh.

<record the measured differing count here>

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```
Replace the placeholder line with the real count from Step 2.

---

### Task 6: Close phase 3

**Files:**
- Modify: `.github/workflows/bazel.yml`

- [ ] **Step 1: Add a CI job that builds both and runs the differential**

Append to `.github/workflows/bazel.yml`:
```yaml
  differential:
    runs-on: ubuntu-24.04
    name: both compilers emit identical IR
    steps:
      - uses: actions/checkout@v4

      - name: Install toolchain
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends \
            gcc-14 bison llvm-dev clang make

      - name: Resolve the toolchain paths
        id: tc
        run: |
          LLVMCFG="$(ls /usr/bin/llvm-config-* 2>/dev/null | sort -V | tail -1)"
          if [ -z "$LLVMCFG" ]; then LLVMCFG="$(command -v llvm-config || true)"; fi
          if [ -z "$LLVMCFG" ]; then echo "::error::no llvm-config"; exit 1; fi
          echo "llvm_config=$LLVMCFG" >> "$GITHUB_OUTPUT"

      - uses: bazel-contrib/setup-bazel@0.19.0
        with:
          bazelisk-cache: true
          disk-cache: ${{ github.workflow }}
          repository-cache: true

      # Both build systems, because the whole point is to compare them.
      - name: Build bin/goo with make
        run: make CC=gcc-14 LLVM_CONFIG="${{ steps.tc.outputs.llvm_config }}" bin/goo

      - name: Build //src/compiler:goo with bazel
        run: |
          bazel build //src/compiler:goo \
            --repo_env=CC=/usr/bin/gcc-14 \
            --repo_env=GOO_LLVM_CONFIG=${{ steps.tc.outputs.llvm_config }}

      - name: Compare emitted IR across 495 fixtures
        run: ./tools/compiler_differential.sh
```

- [ ] **Step 2: Validate the workflow**

```bash
python3 -c "import yaml;yaml.safe_load(open('.github/workflows/bazel.yml'));print('yaml ok')"
```

- [ ] **Step 3: Read the parity count**

```bash
./tools/parity.sh | head -3
```
Expected: **unchanged at 211**. This phase claims no gate, because `test-golden` and its siblings need a linked binary and therefore the runtime archive. That is phase 3b, and saying so plainly here is the point of the phase split.

- [ ] **Step 4: Confirm both build systems green**

```bash
bazel test //... > /tmp/b3.log 2>&1; echo "bazel exit=$?"; grep -cE '^//.*PASSED' /tmp/b3.log
./tools/parity_selftest.sh; echo "selftest exit=$?"
bash scripts/grammar-tripwire.sh src/parser/parser.y
```
Run `make verify-core` backgrounded with a 600s window if any Makefile input changed; if `git diff --stat <base>..HEAD -- Makefile 'src/**/*.c' 'src/**/*.h' scripts/` is empty, CI's `verify-core` job is the confirmation and a local run adds nothing.

- [ ] **Step 5: Push and open the PR**

```bash
git push -u origin build/bazel-phase3-codegen
gh pr create --base build/bazel-phase2-frontend --title "build(bazel): phase 3, codegen and the compiler" --body "$(cat <<'BODY'
Phase 3 of the Bazel migration. **Stacked on #322.**

Still additive. No Makefile target is removed.

## What is here

`//src/codegen` (15 files, the first target to take `@llvm//:llvm_c`) and `//src/compiler:goo`.

## Equivalence, on IR rather than behaviour

Both compilers emit **byte-identical LLVM IR** across all 495 golden fixtures.

`--emit-llvm` needs no runtime archive, verified: it exits 0 with `GOO_RUNTIME=/nonexistent` while a full compile fails at `ld`. That keeps NNG out of this phase. It is also a stronger claim than matching stdout, since two different IRs can print the same thing.

No normalisation is needed — the IR embeds only `ModuleID` and `source_filename`, both derived from the `-o` basename.

## Parity does not move, and that is deliberate

Still **211**. `test-golden`, `test-golden-o2` and `test-golden-reject` need a linked binary, which needs `lib/libgoo_runtime.a` — 13 runtime objects plus the whole of `libnng.a`, a pinned cmake tarball. That is **phase 3b**. Porting a third-party cmake build in the same phase as the LLVM port would make any failure unattributable.

## Teeth

The differential is proven able to fail: the `entry` basic-block label was mutated, only the Bazel side rebuilt, and it reported a non-zero differing count; restored, green, clean tree. The empty-corpus guard exits 2 rather than reporting success having compared nothing — the defect `5c633f6` fixed in `run_golden.sh`.

The differential is tagged `manual`/`no-sandbox` and **phase 7 deletes it**: it compares two build systems, so the question stops existing when the Makefile does.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01DHrKLCPRvkeBRutFyP2vsh
BODY
)"
```

- [ ] **Step 6: Read the real CI conclusion**

```bash
gh pr view <PR#> --json statusCheckRollup --jq '.statusCheckRollup[] | "\(.conclusion // .state)\t\(.name // .context)"'
```
Never a piped exit code. Every check must read SUCCESS, including the new `differential` job.

---

## What phase 3b needs

Phase 3b is the runtime archive, and it is where NNG arrives:

- `third_party/nng-1.12.0.tar.gz` is pinned with a sha256 and built by cmake with `CMAKE_C_ARCHIVE_CREATE/APPEND/FINISH` overridden to zero member mtime/uid/gid. All three rules are required — the Makefile comment records that CMake splits long object lists into a CREATE batch plus APPEND batches, and the default APPEND rule carries real timestamps.
- `addlib` copies member headers **verbatim**, so `ar -D` on the outer archive does not zero a member that arrived from NNG. Determinism has to be achieved at the source, and `archive-determinism-probe` asserts all 102 members carry mtime 0, uid 0, gid 0.
- `//src/runtime:runtime` (11 files, `RUNTIME_SRCS`) is **not** the archive's contents (13 files, `RUNTIME_OBJS`, adding `io.c` and `far_transport.c`). Phase 3b needs a second target, not a rename.
- Once the archive exists, `GOO_RUNTIME` points the compiler at it and `scripts/run_golden.sh` runs unmodified — it already takes `COMPILER` and `EX_DIR` from the environment. Three gates follow: `test-golden`, `test-golden-o2`, `test-golden-reject`, taking parity from 211 to 208.
