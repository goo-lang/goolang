# Bazel Migration — Phases 0 and 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up a Bazel workspace that builds and tests the LLVM-free leaf of the goolang compiler, plus `tools/parity.sh` — the gate-parity tool that must reach zero before the Makefile may be deleted in phase 7.

**Architecture:** Bazel and Make coexist. Nothing is removed from the Makefile in these two phases. `parity.sh` reads `VERIFY_ALL_DEPS` from the Makefile and the test list from `bazel query`, and reports gates with no Bazel counterpart. It starts at the recorded baseline (216 on main) and is expected to stay red for the whole migration; its own positive control proves it can move.

**Tech Stack:** Bazel 9.2.0 (bzlmod), `rules_cc` 0.2.17, `rules_shell` 0.6.1, GCC 16.2.1, LLVM 22.1.8 via the host `llvm-config`, C23.

**Spec:** `docs/superpowers/specs/2026-08-25-bazel-migration-design.md`

## Global Constraints

- **Bazel version is pinned to `9.2.0`** in `.bazelversion`. Verified working on this machine.
- **Dependencies are exactly `rules_cc` 0.2.17 and `rules_shell` 0.6.1.** Both were resolved and run green on Bazel 9.2.0 on 2026-08-25. Do not add a dependency without recording why.
- **`sh_test` must be loaded from `@rules_shell//shell:sh_test.bzl`.** Bazel 9 removed the native rule.
- **The compiler pin is `/usr/bin/gcc`, an ABSOLUTE path, and GCC is the default — not clang.** `/usr/lib64/ccache/gcc` precedes `/usr/bin/gcc` on PATH, and the ccache shim cannot write to `~/.cache/ccache` inside Bazel's read-only sandbox, so every compile fails with `Read-only file system`. GCC rather than clang because phase 3 compares the Bazel-built compiler against the Make-built one, and the Makefile uses GCC. Changing the build system and the compiler together would make that comparison unattributable.
- **That pin is correct locally and WRONG on CI, which is why it is a `--repo_env` setting and not a hard-coded toolchain.** `.github/workflows/tests.yml` runs `ubuntu-24.04`, whose `/usr/bin/gcc` is gcc-13 and rejects `-std=c23`; it installs and uses gcc-14. Every CI invocation must pass `--repo_env=CC=/usr/bin/gcc-14`.
- **The LLVM version check is a FLOOR (`GOO_LLVM_MIN_MAJOR`, default 18), not an equality.** The workstation has llvm-config 22.1.8 as the unversioned binary; ubuntu-24.04's `llvm-dev` provides only a VERSIONED one (`llvm-config-18` and similar). An exact-major assert would refuse to configure on CI. A floor keeps the property that matters — a missing or too-old LLVM stops the build loudly — without pinning CI to a version its distribution does not ship. `GOO_LLVM_CONFIG` pins an explicit path when the unversioned binary is absent.
- **The language standard is C23** (`build --copt=-std=c23`), matching `Makefile:23`.
- **Commits MUST use `git -c commit.gpgsign=false commit`.** The 1Password SSH signing agent fails in this environment.
- **Every git write operation must be backgrounded with a 600s window.** A pre-commit hook runs `make test` and a pre-push hook runs `make verify-core`; both exceed a 2-minute foreground window. Verify the result with `git log -1`, never the exit code alone.
- **The shell is zsh: `${PIPESTATUS[0]}` reads EMPTY.** Use `$pipestatus` (1-indexed), or take an exit status with no pipe at all.
- **Never modify `user.email` in this repo.** The local `test@test` override is deliberate and keeps a personal address out of public commits.
- **`make verify-core` must stay green after every task.** Nothing in these phases removes or edits a Makefile target.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `.bazelversion` | Pins Bazel 9.2.0 | 1 |
| `.bazelignore` | Keeps Bazel out of `attic/`, `build/`, `bin/`, `lib/`, `goostd/` | 1 |
| `MODULE.bazel` | Module name, the two deps, the LLVM extension | 1, 2 |
| `.bazelrc` | Compiler pin, C23, warning set | 1 |
| `BUILD` (root) | `exports_files` for the Makefile, which `parity.sh` reads | 1 |
| `third_party/llvm/llvm.bzl` | `repository_rule`: version assert, header symlink, `cc_library` generation | 2 |
| `third_party/llvm/ext.bzl` | Module extension wrapping the rule | 2 |
| `third_party/llvm/BUILD` | Package marker | 2 |
| `include/BUILD` | `cc_library` `prelude`: `xalloc.h` + `goo_assert.h` | 3 |
| `tools/defs.bzl` | `goo_cc_library` / `goo_cc_test`: prelude wiring in one place | 3 |
| `tools/parity.sh` | Gate parity between Makefile and Bazel | 4, 5, 7 |
| `tools/parity_selftest.sh` | Proves `parity.sh` can report a change | 6 |
| `tools/parity-allowlist.txt` | Gates deliberately not migrated, each with a reason | 7 |
| `tools/BUILD` | `sh_test` targets for the parity tool and its self-test | 4, 6 |
| `tests/unit/BUILD` | `cc_library` `goo_check` from the existing `goo_check.h` | 9 |
| `src/runtime/BUILD` | `cc_library` `runtime`, the 11 files in `RUNTIME_SRCS` | 10 |
| `tests/unit/runtime/BUILD` | `cc_test` `obj_header_test` | 10 |
| `src/types/BUILD` | `cc_library` per escape/release unit | 13, 14 |
| `tests/unit/types/BUILD` | `cc_test` per ported suite | 13, 14 |

---

# PHASE 0 — Foundation

### Task 1: Bazel workspace skeleton

**Files:**
- Create: `.bazelversion`, `.bazelignore`, `MODULE.bazel`, `.bazelrc`, `BUILD`
- Modify: `.gitignore`

**Interfaces:**
- Consumes: nothing.
- Produces: a configured workspace. Later tasks rely on `bazel query //...` exiting 0, on the C23 flag, and on the `/usr/bin/gcc` pin.

- [ ] **Step 1: Confirm the workspace does not yet exist (the failing state)**

Run:
```bash
bazel query //... 2>&1 | tail -3
```
Expected: an error naming a missing `MODULE.bazel` or `WORKSPACE`. If this succeeds, a workspace already exists — stop and report.

- [ ] **Step 2: Create `.bazelversion`**

```
9.2.0
```

- [ ] **Step 3: Create `.bazelignore`**

`attic/` holds quarantined sources that link into no binary. Bazel must not traverse it, nor the Make output directories.

```
attic
build
bin
lib
node_modules
```

- [ ] **Step 4: Create `MODULE.bazel`**

```python
module(
    name = "goolang",
    version = "0.1.0",
)

# Verified on Bazel 9.2.0, 2026-08-25. Bazel 9 removed the native sh_* rules,
# so rules_shell is required for every probe test, not optional.
bazel_dep(name = "rules_cc", version = "0.2.17")
bazel_dep(name = "rules_shell", version = "0.6.1")
```

- [ ] **Step 5: Create `.bazelrc`**

```
# ---------------------------------------------------------------------------
# Compiler
#
# GCC, by an ABSOLUTE path, and this is deliberate on both counts.
#
# Absolute: Fedora puts ccache shims (/usr/lib64/ccache) ahead of gcc on PATH.
# The shims cannot write to ~/.cache/ccache inside Bazel's read-only sandbox,
# so every compile fails with "Read-only file system". Bazel keeps its own
# action cache, so ccache buys nothing here anyway. Verified 2026-08-25.
#
# GCC rather than clang: orca pins clang, and copying that here would be wrong.
# Phase 3 compares the Bazel-built compiler against the Make-built one, and the
# Makefile uses gcc. Pinning clang would change the build system AND the
# compiler in one step, so any behavioural difference would have two candidate
# causes and neither could be attributed.
#
# Clang is still built and tested, via --config=clang. The sanitizer configs
# added in phase 6 select it explicitly, because this machine's gcc has a
# broken libasan (a linker script with no matching .so).
# ---------------------------------------------------------------------------
build --repo_env=CC=/usr/bin/gcc
build:clang --repo_env=CC=/usr/bin/clang

# ---------------------------------------------------------------------------
# Language standard. Matches Makefile:23.
# ---------------------------------------------------------------------------
build --copt=-std=c23

# ---------------------------------------------------------------------------
# Warnings. Matches Makefile:23's -Wall -Wextra. Vendored or generated code is
# exempted per-target with copts = ["-w"], never by relaxing this set.
# ---------------------------------------------------------------------------
build --copt=-Wall
build --copt=-Wextra

test --test_output=errors
```

- [ ] **Step 6: Create the root `BUILD`**

`parity.sh` reads the Makefile, so the Makefile must be an exported file that a test can take as `data`.

```python
exports_files(["Makefile"])
```

- [ ] **Step 7: Add Bazel's symlinks to `.gitignore`**

Append:
```
# Bazel convenience symlinks
/bazel-*
```

- [ ] **Step 8: Verify the workspace configures**

Run:
```bash
bazel query //... --noshow_progress 2>/dev/null; echo "exit=$?"
bazel query //:Makefile --noshow_progress 2>/dev/null; echo "exit=$?"
```
Expected: the first exits 0 and lists NOTHING. `exports_files` declares a
source file, not a rule, and `//...` enumerates rules — an empty list is
correct here, not a failure. The second must print `//:Makefile` and exit 0,
which is what Task 4 depends on. A bogus label such as `//:NoSuchFile` exits 7,
so the check can report the negative.

- [ ] **Step 9: Verify the Makefile is untouched**

Run:
```bash
git status --short Makefile
```
Expected: no output. This plan never edits the Makefile.

- [ ] **Step 10: Commit**

```bash
git add .bazelversion .bazelignore MODULE.bazel .bazelrc BUILD .gitignore
git -c commit.gpgsign=false commit -m "build(bazel): workspace skeleton pinned to 9.2.0

GCC by absolute path on both counts. Absolute because /usr/lib64/ccache/gcc
precedes /usr/bin/gcc on PATH and the shim cannot write inside Bazel's
read-only sandbox. GCC rather than clang because phase 3 compares the
Bazel-built compiler against the Make-built one, and the Makefile uses gcc.

The Makefile is unchanged and make verify-core still owns the gate net.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```
Run this backgrounded with a 600s window, then verify with `git log -1`.

---

### Task 2: LLVM repository rule with a version assertion

**Files:**
- Create: `third_party/llvm/llvm.bzl`, `third_party/llvm/ext.bzl`, `third_party/llvm/BUILD`
- Create: `third_party/llvm/llvm_smoke.c` (the test)
- Modify: `MODULE.bazel`

**Interfaces:**
- Consumes: the workspace from Task 1.
- Produces: `@llvm//:llvm_c`, a `cc_library` carrying the LLVM C API headers and `-lLLVM-22`. Phase 3 depends on this label. Also `@llvm//:VERSION`, a file holding the detected version string.

**Background — why a plain `copts` entry cannot work.** Verified 2026-08-25: a `cc_test` with `copts = ["-isystem", "/usr/lib64/llvm22/include"]` fails to build with `The include path '/usr/lib64/llvm22/include' references a path outside of the execution root`. The headers must be symlinked into the repository.

- [ ] **Step 1: Write the failing test**

Create `third_party/llvm/llvm_smoke.c`:
```c
// Proves @llvm//:llvm_c exposes a usable LLVM C API: headers resolve, the
// symbols link, and a context round-trips. Deliberately tiny -- this gates the
// dependency, not LLVM itself.
#include <llvm-c/Core.h>

int main(void) {
    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("smoke", ctx);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    return 0;
}
```

Create `third_party/llvm/BUILD`:
```python
load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "llvm_smoke",
    size = "small",
    srcs = ["llvm_smoke.c"],
    deps = ["@llvm//:llvm_c"],
)
```

- [ ] **Step 2: Run it to verify it fails**

Run:
```bash
bazel test //third_party/llvm:llvm_smoke 2>&1 | tail -5
```
Expected: FAIL. The repository `@llvm` does not exist yet.

- [ ] **Step 3: Write the repository rule**

Create `third_party/llvm/llvm.bzl`:
```python
"""Locates LLVM through the host llvm-config and exposes its C API.

The headers are SYMLINKED into the repository rather than referenced by an
absolute -isystem path. Verified 2026-08-25: the absolute form fails with
"The include path ... references a path outside of the execution root", so
the symlink is load-bearing, not a style choice.

This rule is deliberately NOT hermetic -- it reads the host toolchain, exactly
as Makefile:50 does. What it adds is refusal: the Makefile prints a warning and
builds with LLVM_AVAILABLE=0, so a build with no code generator still exits 0.
"""

_REQUIRED_MAJOR = "22."   # superseded: see the note after this task

def _run(rctx, args):
    res = rctx.execute(args)
    if res.return_code != 0:
        fail("llvm-config failed: {} -> {}".format(args, res.stderr))
    return res.stdout.strip()

def _llvm_repo_impl(rctx):
    cfg = rctx.which("llvm-config")
    if cfg == None:
        fail("llvm-config is not on PATH. goolang requires LLVM 22.")

    version = _run(rctx, [cfg, "--version"])
    if not version.startswith(_REQUIRED_MAJOR):
        fail("goolang requires LLVM {}x, llvm-config reports {}".format(
            _REQUIRED_MAJOR,
            version,
        ))

    includedir = _run(rctx, [cfg, "--includedir"])
    libdir = _run(rctx, [cfg, "--libdir"])
    libs = _run(rctx, [cfg, "--libs", "core"]).split(" ")

    rctx.symlink(includedir, "include")
    rctx.file("VERSION", version + "\n")
    rctx.file("BUILD", '''load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "llvm_c",
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    linkopts = {linkopts},
    visibility = ["//visibility:public"],
)

exports_files(["VERSION"])
'''.format(linkopts = repr(["-L" + libdir] + libs)))

llvm_repo = repository_rule(
    implementation = _llvm_repo_impl,
    local = True,
    doc = "Configures @llvm from the host llvm-config, asserting the version.",
)
```

Create `third_party/llvm/ext.bzl`:
```python
load(":llvm.bzl", "llvm_repo")

def _llvm_ext_impl(_mctx):
    llvm_repo(name = "llvm")

llvm_ext = module_extension(implementation = _llvm_ext_impl)
```

- [ ] **Step 4: Wire the extension into `MODULE.bazel`**

Append:
```python
llvm_ext = use_extension("//third_party/llvm:ext.bzl", "llvm_ext")
use_repo(llvm_ext, "llvm")
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
bazel test //third_party/llvm:llvm_smoke 2>&1 | tail -5
```
Expected: `//third_party/llvm:llvm_smoke  PASSED`.

> **Note added while executing (2026-08-25).** The rule shipped is a version
> FLOOR, not the exact-major assert written above. `ubuntu-24.04` — the distro
> `tests.yml` already uses — ships LLVM 18 via `llvm-dev`, and only as a
> VERSIONED binary, so `startswith("22.")` refused to configure on CI and
> `rctx.which("llvm-config")` found nothing. The shipped rule searches
> `llvm-config`, then `llvm-config-22` down to `-18`, honours `GOO_LLVM_CONFIG`
> for an explicit path, and compares against `GOO_LLVM_MIN_MAJOR` (default 18).
> Read `third_party/llvm/llvm.bzl` for the version in the tree; the teeth check
> below still applies, with the floor raised instead of the major changed.

- [ ] **Step 6: Prove the version assertion has teeth**

A rule that cannot refuse is indistinguishable from no rule. Run:
```bash
# No file edit needed: the floor is an env var, so the mutation is a flag.
bazel test //third_party/llvm:llvm_smoke --repo_env=GOO_LLVM_MIN_MAJOR=99 \
    > /tmp/red.log 2>&1; echo "RED exit=$?"
grep -oE 'goolang requires LLVM 99 or newer[^"]*' /tmp/red.log | head -1
bazel test //third_party/llvm:llvm_smoke > /tmp/green.log 2>&1; echo "GREEN exit=$?"
```
Expected: `RED exit=1`, the grep prints `1`, `GREEN exit=0`. Do not take the exit status through a pipe.

- [ ] **Step 7: Confirm the CI override path also works**

CI cannot rely on an unversioned `llvm-config`, so the explicit-path override
must be exercised too:
```bash
bazel test //third_party/llvm:llvm_smoke \
    --repo_env=GOO_LLVM_CONFIG=/usr/bin/llvm-config > /tmp/ci.log 2>&1; echo "exit=$?"
```
Expected: exit 0. Because the floor lives in an env var rather than the source,
no mutation is left in the tree to clean up.

- [ ] **Step 8: Commit**

```bash
git add third_party/llvm MODULE.bazel
git -c commit.gpgsign=false commit -m "build(bazel): locate LLVM 22 and refuse anything else

Makefile:50 shells out to llvm-config and, on failure, prints a warning and
sets LLVM_AVAILABLE=0 -- so a build with no code generator exits 0. The rule
calls fail() instead.

Headers are symlinked into the repository. An absolute -isystem path fails
with 'references a path outside of the execution root'.

Teeth proven: with the assert set to 99., bazel test exits 1 reporting
'requires LLVM 99.x, llvm-config reports 22.1.8'; restored to 22., exit 0.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: The prelude and the shared macros

**Files:**
- Create: `include/BUILD`, `tools/defs.bzl`, `tools/BUILD`
- Create: `tools/prelude_probe.c` (the test)

**Interfaces:**
- Consumes: Task 1's workspace.
- Produces: `goo_cc_library(name, srcs, hdrs, deps, copts, **kwargs)` and `goo_cc_test(name, srcs, deps, **kwargs)` in `//tools:defs.bzl`. Every C target in phases 1-7 uses these two macros instead of raw `cc_library` / `cc_test`. Also `//include:prelude`, a `cc_library` exposing `xalloc.h` and `goo_assert.h`.

**Background.** `Makefile:23` forces two headers into every translation unit with `-include include/xalloc.h -include include/goo_assert.h`. Putting those flags in `.bazelrc` would compile, but Bazel would not know the headers are inputs, so editing `goo_assert.h` would rebuild nothing. Step 6 proves this.

- [ ] **Step 1: Write the failing test**

Create `tools/prelude_probe.c`. It calls `xmalloc` and `GOO_ASSERT` without including either header, so it compiles only if the prelude is forced in.

```c
// Proves the prelude reaches a translation unit that includes neither header.
// Makefile:23 forces xalloc.h and goo_assert.h in with -include; this file is
// the Bazel-side check that goo_cc_library/goo_cc_test do the same.
//
// Three things are asserted at once, and all three fail loudly under C23,
// which removed implicit function declarations:
//   xmalloc     -- comes from xalloc.h
//   GOO_ASSERT  -- comes from goo_assert.h, and takes ONE argument
//   free        -- comes from <stdlib.h>, which xalloc.h includes, so this
//                  also pins that the prelude arrives with its own includes.
//
// NOTE: there is no xfree. xalloc.h exports xmalloc, xcalloc, xrealloc and
// xstrdup only; memory is released with plain free().
#include <stddef.h>

int main(void) {
    void *p = xmalloc(16);
    GOO_ASSERT(p != NULL);
    free(p);
    return 0;
}
```

Create `tools/BUILD`:
```python
load("//tools:defs.bzl", "goo_cc_test")

goo_cc_test(
    name = "prelude_probe",
    size = "small",
    srcs = ["prelude_probe.c"],
)
```

- [ ] **Step 2: Run it to verify it fails**

Run:
```bash
bazel test //tools:prelude_probe 2>&1 | tail -5
```
Expected: FAIL — `//tools:defs.bzl` does not exist.

- [ ] **Step 3: Confirm the exact symbol names before writing the macro**

Do not guess the allocator's spelling. Run:
```bash
grep -nE '^[a-z].*\bx(malloc|free)\b|#define GOO_ASSERT' include/xalloc.h include/goo_assert.h | head
```
If `xmalloc`, `xfree` or `GOO_ASSERT` differ from the probe above, correct `tools/prelude_probe.c` to match the real names before continuing.

- [ ] **Step 4: Create `include/BUILD`**

```python
load("@rules_cc//cc:defs.bzl", "cc_library")

# The two headers Makefile:23 forces into every translation unit. They are
# declared as hdrs so Bazel treats them as inputs: a -include flag alone would
# compile, but an edit to goo_assert.h would rebuild nothing.
cc_library(
    name = "prelude",
    hdrs = [
        "goo_assert.h",
        "xalloc.h",
    ],
    visibility = ["//visibility:public"],
)

# Every other header in the tree. Split from :prelude so that a target which
# needs a header does not thereby acquire the forced includes.
cc_library(
    name = "headers",
    hdrs = glob(["**/*.h"]),
    includes = ["."],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 5: Create `tools/defs.bzl`**

```python
"""Shared C target macros.

Every C target in this repo uses goo_cc_library or goo_cc_test, never a bare
cc_library or cc_test. The forced-include prelude is the reason: a target that
sets the copts by hand and omits the //include:prelude dependency compiles
today and stops rebuilding when goo_assert.h changes. Do not hand-roll them.
"""

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# Mirrors Makefile:23. The paths are execroot-relative, which is where Bazel
# runs every action from.
PRELUDE_COPTS = [
    "-include",
    "include/xalloc.h",
    "-include",
    "include/goo_assert.h",
    "-I.",
    "-Iinclude",
    "-D_GNU_SOURCE",
]

PRELUDE_DEPS = [
    "//include:prelude",
    "//include:headers",
]

def goo_cc_library(name, copts = [], deps = [], **kwargs):
    """A cc_library with the goolang prelude forced in."""
    cc_library(
        name = name,
        copts = PRELUDE_COPTS + copts,
        deps = PRELUDE_DEPS + deps,
        **kwargs
    )

def goo_cc_test(name, copts = [], deps = [], **kwargs):
    """A cc_test with the goolang prelude forced in."""
    cc_test(
        name = name,
        copts = PRELUDE_COPTS + copts,
        deps = PRELUDE_DEPS + deps,
        **kwargs
    )
```

- [ ] **Step 6: Run the test to verify it passes**

Run:
```bash
bazel test //tools:prelude_probe 2>&1 | tail -5
```
Expected: `//tools:prelude_probe  PASSED`.

- [ ] **Step 7: Prove the header is a real input, not just a flag**

This is the whole reason the macro exists. Ask the action graph directly rather
than inferring it from rebuild behaviour:

```bash
bazel aquery 'mnemonic("CppCompile", //tools:prelude_probe)' --output=text > /tmp/aq.log 2>&1
echo "goo_assert.h: $(grep -c 'goo_assert\.h' /tmp/aq.log)"   # expect 2
echo "xalloc.h:     $(grep -c 'xalloc\.h' /tmp/aq.log)"       # expect 2
echo "control:      $(grep -c 'llvm-c/Core\.h' /tmp/aq.log)"  # expect 0
```

Expected: each prelude header appears twice — once as the `-include` argument
and once in the action's `Inputs` — and the control is 0, proving the grep
discriminates. A `0` for either header means it is not an input and
`include/BUILD` is wired wrongly.

**Do NOT test this by touching a header and grepping for a rebuild.** Both
halves of that check are broken, and both were tried on 2026-08-25:

- Bazel digests file CONTENT, not mtime, so `touch` correctly changes nothing.
- Bazel prints no `Compiling ...` line in the default output mode, only an
  `INFO: N processes` summary, so the grep string never matches and the check
  returns 0 whether the wiring is right or wrong.

A check that returns 0 in both cases cannot tell you anything. `aquery` states
the input set instead of inferring it.

- [ ] **Step 8: Commit**

```bash
git add include/BUILD tools/defs.bzl tools/BUILD tools/prelude_probe.c
git -c commit.gpgsign=false commit -m "build(bazel): force the prelude through a macro, not a flag

Makefile:23 forces xalloc.h and goo_assert.h into every translation unit.
A bare copt would compile while leaving the headers undeclared as inputs, so
editing goo_assert.h would rebuild nothing. goo_cc_library and goo_cc_test
carry both the copts and the //include:prelude dependency.

Gated by //tools:prelude_probe, which calls xmalloc and GOO_ASSERT while
including neither header, and by a touch-and-rebuild check.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: parity.sh — read the gates out of the Makefile

**Files:**
- Create: `tools/parity.sh`, `tools/parity_test.sh`, `tools/parity-gate-count.txt`
- Modify: `tools/BUILD`

**Interfaces:**
- Consumes: the root `BUILD`'s `exports_files(["Makefile"])` from Task 1.
- Produces: `tools/parity.sh --list-make-gates`, printing one gate name per line, sorted, to stdout. Task 5 consumes this.

- [ ] **Step 1: Write the failing test**

Create `tools/parity_test.sh`:
```sh
#!/usr/bin/env bash
# Gates tools/parity.sh's Makefile reader against facts measured on 2026-08-25.
#
# The three assertions are chosen so that a broken reader cannot pass:
#   - the exact count, so a reader that drops or duplicates entries fails
#   - a gate KNOWN to be present, so an empty read cannot pass       (positive control)
#   - a gate KNOWN to be absent, so a reader that returns everything fails
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

gates="$(./tools/parity.sh --list-make-gates)"
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "parity_test: --list-make-gates exited $rc"
    exit 2
fi

fail=0

# The count is a RECORDED BASELINE, not a constant, because it moves with the
# branch: golden-selftest is in VERIFY_ALL_DEPS on test/golden-runner-teeth
# (5c633f6) and not on main, so main reads 216 and that branch reads 217.
# Same idiom as scripts/grammar-tripwire.sh's EXPECTED_SR and
# scripts/probe-teeth-baseline.txt: a changed count is stop-the-line, and
# bumping it is a deliberate one-line edit rather than silent drift.
expected="$(grep -oE '^[0-9]+' tools/parity-gate-count.txt 2>/dev/null | head -1)"
if [ -z "$expected" ]; then
    echo "parity_test: TOOL FAILURE cannot read tools/parity-gate-count.txt"
    exit 2
fi
count="$(printf '%s\n' "$gates" | grep -c .)"
if [ "$count" -ne "$expected" ]; then
    echo "parity_test: FAIL gate count moved: baseline $expected, read $count"
    echo "  If a gate was added or removed on purpose, update"
    echo "  tools/parity-gate-count.txt in the same commit."
    fail=1
fi

# Positive control. m10-probe is in VERIFY_ALL_DEPS. If this is missing, the
# reader returned nothing useful and the count check above proves nothing.
if ! printf '%s\n' "$gates" | grep -qx 'm10-probe'; then
    echo "parity_test: FAIL m10-probe absent (reader returned nothing usable)"
    fail=1
fi

# Negative control. m12-probe is DEFINED in the Makefile at line 3101 but is
# absent from VERIFY_ALL_DEPS -- it never runs. A reader that scrapes target
# definitions instead of the variable would wrongly include it.
if printf '%s\n' "$gates" | grep -qx 'm12-probe'; then
    echo "parity_test: FAIL m12-probe present (reader scraped targets, not VERIFY_ALL_DEPS)"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "parity_test: PASS $count gates (baseline $expected), controls both correct"
fi
exit "$fail"
```

Make it executable: `chmod +x tools/parity_test.sh`

The `data` list below is what makes this work inside the sandbox. Verified
2026-08-25 on a reduced case: with `data = ["parity.sh", "//:Makefile"]`, a
script in `tools/` resolves the repo root as `$(dirname "$BASH_SOURCE")/..`
and reads the Makefile from runfiles correctly.

Add to `tools/BUILD`:
```python
load("@rules_shell//shell:sh_test.bzl", "sh_test")

sh_test(
    name = "parity_test",
    size = "small",
    srcs = ["parity_test.sh"],
    data = [
        "parity-gate-count.txt",
        "parity.sh",
        "//:Makefile",
    ],
)
```

Also create `tools/parity-gate-count.txt` holding the current baseline:
```
216
```
216 is the count on `main`. The `test/golden-runner-teeth` branch adds
`golden-selftest`, so it reads 217 there — bump this file in the merge commit.

- [ ] **Step 2: Run it to verify it fails**

Run:
```bash
bazel test //tools:parity_test 2>&1 | tail -6
```
Expected: FAIL — `tools/parity.sh` does not exist.

- [ ] **Step 3: Write the Makefile reader**

Create `tools/parity.sh`:
```sh
#!/usr/bin/env bash
# Gate parity between the Makefile and Bazel.
#
# The Makefile is this project's gate net: VERIFY_ALL_DEPS names every gate and
# verify-core runs all but the three in HEAVY_DEPS. The exact count moves with
# the branch, so it lives in tools/parity-gate-count.txt rather than here.
# The migration deletes the Makefile in phase 7, and
# this script is the only thing standing between "deleted" and "silently lost
# a gate". It must reach zero unmapped before the Makefile may be removed.
#
# Exit codes:
#   0  every gate has a Bazel counterpart
#   1  a gate is unmapped
#   2  a tool failed (could not read the Makefile, bazel query failed)
#
# Exit status is never read through a pipe: a pipeline reports only its last
# stage's status, which would hide a red result entirely.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAKEFILE="${MAKEFILE:-$root/Makefile}"

# Reads the VERIFY_ALL_DEPS variable specifically, NOT every *-probe: target.
# The distinction is load-bearing: m12-probe is defined at Makefile:3101 and is
# absent from VERIFY_ALL_DEPS, so it never runs. Scraping target definitions
# would report it as a gate needing migration.
list_make_gates() {
    if [ ! -r "$MAKEFILE" ]; then
        echo "parity: cannot read $MAKEFILE" >&2
        return 2
    fi
    awk '/^VERIFY_ALL_DEPS[[:space:]]*:?=/{f=1} f{print} f && !/\\[[:space:]]*$/{exit}' "$MAKEFILE" \
        | sed 's/VERIFY_ALL_DEPS[[:space:]]*:\?=//' \
        | tr -d '\\' \
        | tr ' ' '\n' \
        | grep -E '^[a-zA-Z0-9_-]+$' \
        | sort -u
}

case "${1:-}" in
    --list-make-gates)
        list_make_gates
        exit $?
        ;;
    *)
        echo "usage: parity.sh --list-make-gates" >&2
        exit 2
        ;;
esac
```

Make it executable: `chmod +x tools/parity.sh`

- [ ] **Step 4: Run the test to verify it passes**

Run:
```bash
bazel test //tools:parity_test --test_output=all 2>&1 | grep -E 'parity_test:|PASSED|FAILED'
```
Expected: `parity_test: PASS 216 gates (baseline 216), controls both correct`.

- [ ] **Step 5: Commit**

```bash
git add tools/parity.sh tools/parity_test.sh tools/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): read the gate list out of the Makefile

parity.sh is the only thing that will stand between deleting the Makefile in
phase 7 and silently losing a gate. This commit is its reader.

It parses VERIFY_ALL_DEPS specifically, not every *-probe: target. The
distinction is load-bearing: m12-probe is defined at Makefile:3101, is absent
from VERIFY_ALL_DEPS and never runs, so scraping definitions would report a
gate that does not exist. parity_test.sh asserts both controls.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: parity.sh — query Bazel and report the unmapped gates

**Files:**
- Modify: `tools/parity.sh`, `tools/parity_test.sh`

**Interfaces:**
- Consumes: `--list-make-gates` from Task 4.
- Produces: the default invocation `tools/parity.sh`, printing a count summary and the unmapped list, exiting 1 while any gate is unmapped. Task 6 proves it can move; Task 8 runs it in CI.

**Naming convention.** A Bazel test claims a gate by being named after it with hyphens replaced by underscores, anywhere in the tree. `switch-probe` is claimed by any target named `switch_probe`, for example `//tests/probes:switch_probe`. Exceptions go in the allowlist added in Task 7, never into a special case here.

- [ ] **Step 1: Extend the test first**

**This test must NOT invoke bazel.** It runs inside a Bazel sandbox, and a
nested `bazel query` contends on the output-base lock: it hangs or fails, it
never passes. So `parity.sh` reads its target list from `PARITY_BAZEL_TESTS`
when that variable is set, and the test injects a fixture. The real query path
runs only outside the sandbox, in `tools/parity_selftest.sh`.

Append to `tools/parity_test.sh`, before the final `exit "$fail"`:
```sh
# The mapping logic, exercised against an INJECTED target list. bazel is never
# invoked here: this script runs inside a Bazel sandbox and a nested query
# would contend on the output-base lock.
tmp_targets="$(mktemp)"
# Two real gates, spelled the way a Bazel target would be.
printf 'm10_probe\nswitch_probe\n' > "$tmp_targets"

report="$(PARITY_BAZEL_TESTS="$tmp_targets" ./tools/parity.sh 2>&1)"
rc=$?
if [ "$rc" -ne 1 ]; then
    echo "parity_test: FAIL report exited $rc, expected 1 (gates remain)"
    fail=1
fi

m="$(printf '%s\n' "$report" | sed -n 's/^make gates:[[:space:]]*//p')"
u="$(printf '%s\n' "$report" | sed -n 's/^unmapped:[[:space:]]*//p')"
mapped="$(printf '%s\n' "$report" | sed -n 's/^mapped:[[:space:]]*//p')"

# Exactly two gates were offered a counterpart, so exactly two must map.
if [ "$mapped" -ne 2 ]; then
    echo "parity_test: FAIL injected 2 targets, mapped $mapped"
    fail=1
fi
if [ "$((m - mapped))" -ne "$u" ]; then
    echo "parity_test: FAIL arithmetic: $m - $mapped != $u"
    fail=1
fi
rm -f "$tmp_targets"
```

- [ ] **Step 2: Run it to verify it fails**

Run:
```bash
bazel test //tools:parity_test --test_output=all 2>&1 | grep -E 'parity_test:|FAILED'
```
Expected: FAIL — `parity.sh` with no argument still prints usage and exits 2.

- [ ] **Step 3: Add the Bazel side**

Replace the `case` block in `tools/parity.sh` with:
```sh
# Every test target Bazel knows about, as a bare target name.
#
# PARITY_BAZEL_TESTS overrides the query with a file of target names. This is
# not a convenience: tools/parity_test.sh runs INSIDE a Bazel sandbox, and a
# nested bazel query contends on the output-base lock rather than returning.
# The override lets the mapping logic be tested hermetically. The real query
# path runs from tools/parity_selftest.sh, which is tagged no-sandbox.
list_bazel_tests() {
    if [ -n "${PARITY_BAZEL_TESTS:-}" ]; then
        if [ ! -r "$PARITY_BAZEL_TESTS" ]; then
            echo "parity: cannot read PARITY_BAZEL_TESTS=$PARITY_BAZEL_TESTS" >&2
            return 2
        fi
        sort -u < "$PARITY_BAZEL_TESTS"
        return 0
    fi
    local out
    out="$("${BAZEL:-bazel}" query 'tests(//...)' --output label 2>/dev/null)"
    if [ $? -ne 0 ]; then
        echo "parity: bazel query failed" >&2
        return 2
    fi
    printf '%s\n' "$out" | sed 's|.*:||' | sort -u
}

# switch-probe is claimed by a target named switch_probe.
gate_to_target() {
    printf '%s\n' "$1" | tr '-' '_'
}

report() {
    local gates targets unmapped=() mapped=0
    gates="$(list_make_gates)" || return 2
    targets="$(list_bazel_tests)" || return 2

    while IFS= read -r gate; do
        [ -z "$gate" ] && continue
        if printf '%s\n' "$targets" | grep -qx "$(gate_to_target "$gate")"; then
            mapped=$((mapped + 1))
        else
            unmapped+=("$gate")
        fi
    done <<< "$gates"

    echo "make gates: $(printf '%s\n' "$gates" | grep -c .)"
    echo "mapped:     $mapped"
    echo "unmapped:   ${#unmapped[@]}"

    if [ "${#unmapped[@]}" -gt 0 ]; then
        echo
        echo "UNMAPPED (${#unmapped[@]}):"
        printf '  %s\n' "${unmapped[@]}"
        echo
        echo "parity: ${#unmapped[@]} gates have no Bazel test"
        return 1
    fi
    echo
    echo "parity: every gate has a Bazel test"
    return 0
}

case "${1:-}" in
    --list-make-gates) list_make_gates; exit $? ;;
    --list-bazel-tests) list_bazel_tests; exit $? ;;
    "") report; exit $? ;;
    *) echo "usage: parity.sh [--list-make-gates|--list-bazel-tests]" >&2; exit 2 ;;
esac
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```bash
bazel test //tools:parity_test --test_output=all 2>&1 | grep -E 'parity_test:|PASSED|FAILED'
```
Expected: `parity_test: PASS`.

- [ ] **Step 5: Read the real report**

Run this from a normal shell, NOT through `bazel test` — this invocation does
call `bazel query`:
```bash
./tools/parity.sh; echo "exit=$?"
```
Expected: `make gates:` and `unmapped:` both equal the baseline in
`tools/parity-gate-count.txt` (216 on main), `mapped: 0`, `exit=1`. Record the number in the commit message.

- [ ] **Step 6: Commit**

```bash
git add tools/parity.sh tools/parity_test.sh
git -c commit.gpgsign=false commit -m "build(bazel): report gates with no Bazel counterpart

parity.sh now reads both sides and prints the unmapped list. It exits 1 while
any gate is unmapped, which is the expected state for the whole migration --
it reaches 0 in phase 7 and that is what licenses deleting the Makefile.

Baseline today: 216 make gates on main, 0 mapped, 216 unmapped.

A test claims a gate by being named after it with hyphens turned into
underscores. Exceptions go in an allowlist, never a special case here.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: Prove parity.sh can report a change

**Files:**
- Create: `tools/parity_selftest.sh`
- Modify: `tools/BUILD`

**Interfaces:**
- Consumes: the full report from Task 5.
- Produces: `//tools:parity_selftest`, an `sh_test` that must stay green for the rest of the migration.

**Why this task exists.** `parity.sh` is expected to print a large red number every day for weeks. A script that always prints a large red number is indistinguishable from a correct one, right up to the moment it reaches zero and licenses deleting the gate net. This task proves it can move, in both directions.

- [ ] **Step 1: Write the self-test**

Create `tools/parity_selftest.sh`:
```sh
#!/usr/bin/env bash
# Proves parity.sh can report the OPPOSITE result.
#
# parity.sh will print a large "unmapped:" count and exit 1 for most of this
# migration.
# A script hard-wired to do exactly that would look identical, and would still
# look identical on the day it wrongly reported zero and licensed deleting the
# Makefile. So: add one target that claims a real gate, assert the count drops
# by EXACTLY one, remove it, assert it returns.
#
# Exactly one, not merely "goes down": an off-by-one or a substring match would
# move the count by the wrong amount and must fail here.
#
# Exit codes: 0 has teeth, 1 lost its teeth, 2 a tool failed.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

# A real gate, chosen because it is in VERIFY_ALL_DEPS and is not the first or
# last entry, so an off-by-one in the reader cannot accidentally satisfy this.
GATE="m10-probe"
TARGET="m10_probe"
PKG="tools/parity_selftest_tmp"

# parity.sh exits 1 by design while gates remain. Under `set -o pipefail` that
# status becomes the PIPELINE's status, so `parity.sh | grep -q ...` reports
# failure even when grep matches. Every reader below therefore captures the
# report into a variable first and inspects it separately, never through a pipe
# whose status is tested. This bit on 2026-08-25: the guard below wrongly
# reported "m10-probe is not currently unmapped" while all 216 were unmapped.
parity_report() {
    ./tools/parity.sh 2>/dev/null
    return 0
}

unmapped_count() {
    parity_report | sed -n 's/^unmapped:[[:space:]]*//p'
}

cleanup() { rm -rf "$root/$PKG"; }
trap cleanup EXIT

before="$(unmapped_count)"
if [ -z "$before" ]; then
    echo "parity_selftest: TOOL FAILURE parity.sh printed no unmapped count"
    exit 2
fi

# Confirm the gate really is unmapped right now. Without this, a gate that was
# already mapped would make the delta 0 and the test would report lost teeth
# when the truth is a bad fixture.
report="$(parity_report)"
if ! printf '%s\n' "$report" | grep -qx "  $GATE"; then
    echo "parity_selftest: TOOL FAILURE $GATE is not currently unmapped; pick another"
    exit 2
fi

mkdir -p "$PKG"
cat > "$PKG/ok.sh" <<'INNER'
#!/bin/sh
exit 0
INNER
chmod +x "$PKG/ok.sh"
cat > "$PKG/BUILD" <<INNER
load("@rules_shell//shell:sh_test.bzl", "sh_test")

sh_test(
    name = "$TARGET",
    size = "small",
    srcs = ["ok.sh"],
)
INNER

after="$(unmapped_count)"
cleanup
restored="$(unmapped_count)"

rc=0
if [ "$((before - after))" -ne 1 ]; then
    echo "parity_selftest: NO TEETH adding $TARGET moved unmapped $before -> $after (want -1)"
    rc=1
fi
if [ "$restored" -ne "$before" ]; then
    echo "parity_selftest: NO TEETH removing $TARGET left unmapped at $restored (want $before)"
    rc=1
fi
if [ "$rc" -eq 0 ]; then
    echo "parity_selftest: HAS TEETH $before -> $after -> $restored on one target"
fi
exit "$rc"
```

Make it executable: `chmod +x tools/parity_selftest.sh`

- [ ] **Step 2: Run it directly to verify it fails for the right reason**

The self-test writes a Bazel package, so it cannot run inside the Bazel sandbox. Run it directly first:
```bash
./tools/parity_selftest.sh; echo "exit=$?"
```
Expected: `parity_selftest: HAS TEETH 216 -> 215 -> 216`, `exit=0`.

If it reports `NO TEETH`, `parity.sh` cannot see new targets and Task 5 is wrong — fix that before continuing.

- [ ] **Step 3: Prove the self-test itself can fail**

Break `parity.sh` deliberately and confirm the self-test notices:
```bash
cp tools/parity.sh /tmp/parity.sh.bak
sed -i 's/^    echo "unmapped:   ${#unmapped\[@\]}"/    echo "unmapped:   216"/' tools/parity.sh
./tools/parity_selftest.sh; echo "expect 1, got exit=$?"
cp /tmp/parity.sh.bak tools/parity.sh
./tools/parity_selftest.sh; echo "expect 0, got exit=$?"
```
Expected: exit 1 with a `NO TEETH` line, then exit 0. Confirm `tools/parity.sh` is restored with `git diff --stat tools/parity.sh` showing no change.

- [ ] **Step 4: Register it as a tagged Bazel target**

Add to `tools/BUILD`:
```python
# Writes a Bazel package into the source tree, so it cannot run inside the
# sandbox. Tagged so `bazel test //...` skips it; CI runs it explicitly, the
# way tools/verify_sanitizers.sh will be run in phase 6.
sh_test(
    name = "parity_selftest",
    size = "medium",
    srcs = ["parity_selftest.sh"],
    tags = [
        "external",
        "manual",
        "no-sandbox",
    ],
)
```

- [ ] **Step 5: Confirm `bazel test //...` still passes and skips it**

Run:
```bash
bazel test //... 2>&1 | tail -6
```
Expected: green, and `//tools:parity_selftest` is not among the tests executed.

- [ ] **Step 6: Commit**

```bash
git add tools/parity_selftest.sh tools/BUILD
git -c commit.gpgsign=false commit -m "test(bazel): prove parity.sh can report the opposite result

parity.sh will print a large red number every day until phase 7. A script
hard-wired to do exactly that looks identical -- including on the day it
wrongly reports zero and licenses deleting the gate net.

The self-test adds one target claiming a real gate, asserts the unmapped count
drops by EXACTLY one, removes it and asserts it returns. Exactly one, so an
off-by-one or a substring match fails here rather than passing quietly.

Verified in both directions: 216 -> 215 -> 216, and a deliberately hard-wired
count is caught.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: The allowlist for gates that will not be migrated

**Files:**
- Create: `tools/parity-allowlist.txt`
- Modify: `tools/parity.sh`, `tools/parity_test.sh`

**Interfaces:**
- Consumes: the report from Task 5.
- Produces: allowlist handling. An entry is `<gate> <reason>` on one line. A line with no reason is a hard error, so the allowlist cannot become a silent dumping ground.

- [ ] **Step 1: Extend the test first**

Append to `tools/parity_test.sh`, before the final `exit "$fail"`:
```sh
# An allowlisted gate must leave the unmapped count, and an entry with no
# reason must be refused rather than silently accepted.
tmp_allow="$(mktemp)"
tmp_empty="$(mktemp)"
: > "$tmp_empty"   # no Bazel targets at all, so only the allowlist can move it

# A gate NOT already mapped by the fixture above, so the delta is attributable
# to the allowlist alone.
echo "enum-probe covered elsewhere, see spec 4.7" > "$tmp_allow"
base_u="$(PARITY_BAZEL_TESTS="$tmp_empty" ./tools/parity.sh 2>/dev/null | sed -n 's/^unmapped:[[:space:]]*//p')"
allow_u="$(PARITY_BAZEL_TESTS="$tmp_empty" PARITY_ALLOWLIST="$tmp_allow" ./tools/parity.sh 2>/dev/null | sed -n 's/^unmapped:[[:space:]]*//p')"
if [ "$((base_u - allow_u))" -ne 1 ]; then
    echo "parity_test: FAIL allowlisting one gate moved unmapped $base_u -> $allow_u (want -1)"
    fail=1
fi

echo "enum-probe" > "$tmp_allow"
PARITY_BAZEL_TESTS="$tmp_empty" PARITY_ALLOWLIST="$tmp_allow" ./tools/parity.sh >/dev/null 2>&1
if [ $? -ne 2 ]; then
    echo "parity_test: FAIL an allowlist entry with no reason was accepted"
    fail=1
fi
rm -f "$tmp_allow" "$tmp_empty"
```

- [ ] **Step 2: Run it to verify it fails**

Run:
```bash
bazel test //tools:parity_test --test_output=all 2>&1 | grep -E 'parity_test:|FAILED'
```
Expected: FAIL — the allowlist is not implemented.

- [ ] **Step 3: Create the allowlist**

Create `tools/parity-allowlist.txt`:
```
# Gates that will NOT get a Bazel test, one per line, as: <gate> <reason>
#
# A reason is MANDATORY. An entry without one is a hard error (exit 2), so this
# file cannot quietly become the place gates go to be forgotten. Every entry
# here is a gate that parity.sh will stop counting, which is exactly the power
# that needs a written justification next to it.
#
# Empty today. Phase 7 is expected to add the podman and CompCert gates, which
# stay in their own CI workflow rather than moving to Bazel.
```

- [ ] **Step 4: Implement allowlist handling in `tools/parity.sh`**

Add before `report()`:
```sh
ALLOWLIST="${PARITY_ALLOWLIST:-$root/tools/parity-allowlist.txt}"

# Prints the allowlisted gate names. Refuses an entry with no reason: a bare
# gate name would silently drop a gate from the count, which is the one thing
# this whole script exists to prevent.
list_allowlisted() {
    [ -r "$ALLOWLIST" ] || return 0
    local line name rest
    while IFS= read -r line; do
        case "$line" in ''|'#'*) continue ;; esac
        name="${line%% *}"
        rest="${line#"$name"}"
        rest="${rest# }"
        if [ -z "$rest" ]; then
            echo "parity: allowlist entry '$name' has no reason" >&2
            return 2
        fi
        printf '%s\n' "$name"
    done < "$ALLOWLIST"
}
```

In `report()`, after `targets="$(list_bazel_tests)" || return 2`, add:
```sh
    local allowed
    allowed="$(list_allowlisted)" || return 2
```

and change the unmapped branch of the loop to skip allowlisted gates:
```sh
        if printf '%s\n' "$targets" | grep -qx "$(gate_to_target "$gate")"; then
            mapped=$((mapped + 1))
        elif printf '%s\n' "$allowed" | grep -qx "$gate"; then
            mapped=$((mapped + 1))
        else
            unmapped+=("$gate")
        fi
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
bazel test //tools:parity_test --test_output=all 2>&1 | grep -E 'parity_test:|PASSED|FAILED'
```
Expected: `parity_test: PASS`.

- [ ] **Step 6: Confirm the real count is unchanged**

The shipped allowlist is empty, so it must not move anything. Run:
```bash
./tools/parity.sh | head -3
```
Expected: still `unmapped:   216`.

- [ ] **Step 7: Commit**

```bash
git add tools/parity-allowlist.txt tools/parity.sh tools/parity_test.sh
git -c commit.gpgsign=false commit -m "build(bazel): allowlist gates that will not move, with a mandatory reason

Some gates will never get a Bazel test -- the podman and CompCert ones stay in
their own workflow. They need somewhere to go that is not 'quietly dropped'.

An entry is '<gate> <reason>' and a missing reason is exit 2, not a warning.
An allowlist that accepts a bare name is a place gates go to be forgotten,
which is the single thing parity.sh exists to prevent.

Shipped empty, so the count stays at 216.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: Wire Bazel and parity into CI

**Files:**
- Create: `.github/workflows/bazel.yml`

**Interfaces:**
- Consumes: everything from Tasks 1-7.
- Produces: a CI job that runs `bazel test //...`, and a non-blocking job that publishes the parity count. `tests.yml` and `repro.yml` are untouched, so `make verify-core` remains the blocking gate.

- [ ] **Step 1: Confirm the existing workflows are untouched**

Run:
```bash
git status --short .github/
```
Expected: no output.

- [ ] **Step 2: Create `.github/workflows/bazel.yml`**

```yaml
name: bazel

on:
  push:
    branches: [main]
  pull_request:

permissions:
  contents: read

jobs:
  test:
    runs-on: ubuntu-latest
    name: bazel test //...
    steps:
      - uses: actions/checkout@v4

      # .bazelrc pins CC to an absolute path to bypass Fedora's ccache shims,
      # which cannot write inside Bazel's read-only sandbox. Fail loudly here
      # rather than inside a compile action if the runner image ever moves it.
      - name: Check the pinned compiler exists
        run: |
          test -x /usr/bin/gcc || { echo "::error::/usr/bin/gcc missing"; exit 1; }
          /usr/bin/gcc --version

      # The LLVM repository rule refuses any version that is not 22.x, so a
      # runner without it must fail here with a readable message.
      - name: Check LLVM 22 is present
        run: |
          command -v llvm-config >/dev/null || { echo "::error::llvm-config missing"; exit 1; }
          llvm-config --version

      - uses: bazel-contrib/setup-bazel@0.19.0
        with:
          bazelisk-cache: true
          disk-cache: ${{ github.workflow }}
          repository-cache: true

      - name: bazel test //...
        run: bazel test //... --test_output=errors

      # Writes a package into the tree, so it is tagged manual and skipped by
      # //... above. It must be run explicitly or it never runs at all.
      - name: Prove parity.sh can report a change
        run: ./tools/parity_selftest.sh

  parity:
    runs-on: ubuntu-latest
    name: gate parity (informational until phase 7)
    # parity.sh exits 1 by design until every gate is migrated. This job
    # publishes the number so the count is visible on every PR; it becomes
    # blocking in phase 7, when reaching zero is what licenses deleting the
    # Makefile.
    continue-on-error: true
    steps:
      - uses: actions/checkout@v4
      - uses: bazel-contrib/setup-bazel@0.19.0
        with:
          bazelisk-cache: true
          disk-cache: ${{ github.workflow }}
          repository-cache: true
      - name: Report unmapped gates
        run: ./tools/parity.sh
```

- [ ] **Step 3: Verify the workflow file parses**

Run:
```bash
python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/bazel.yml')); print('yaml ok')"
```
Expected: `yaml ok`.

- [ ] **Step 4: Verify locally what CI will run**

Run:
```bash
bazel test //... 2>&1 | tail -5; echo "---"; ./tools/parity_selftest.sh; echo "selftest exit=$?"
```
Expected: Bazel green, self-test `HAS TEETH`, exit 0.

- [ ] **Step 5: Confirm make is still green**

Run backgrounded with a 600s window:
```bash
make verify-core > /tmp/vc.log 2>&1; echo "verify-core exit=$?"
```
Expected: exit 0. Read the log, not a piped status.

- [ ] **Step 6: Commit**

```bash
git add .github/workflows/bazel.yml
git -c commit.gpgsign=false commit -m "ci(bazel): run bazel test and publish the parity count

Two jobs. The first runs bazel test //... and then runs parity_selftest.sh
explicitly, because that target is tagged manual and //... skips it -- a
self-test that never runs is indistinguishable from one that passes.

The second publishes the unmapped gate count and is continue-on-error, because
parity.sh exits 1 by design until phase 7. It becomes blocking there, when
reaching zero is what licenses deleting the Makefile.

tests.yml and repro.yml are untouched: make verify-core is still the gate.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 7: Open the phase 0 PR**

```bash
git push -u origin build/bazel-phase0-foundation
gh pr create --title "build(bazel): phase 0 foundation and the parity gate" --body "$(cat <<'BODY'
Phase 0 of the Bazel migration. See `docs/superpowers/specs/2026-08-25-bazel-migration-design.md`.

This adds a Bazel workspace beside the Makefile. It removes nothing. `make verify-core` is still the gate.

## What is here

- Workspace pinned to Bazel 9.2.0, with `rules_cc` 0.2.17 and `rules_shell` 0.6.1
- `@llvm//:llvm_c` from the host `llvm-config`, which refuses any version that is not 22.x
- `goo_cc_library` / `goo_cc_test`, which force the `xalloc.h` + `goo_assert.h` prelude in as a real dependency
- `tools/parity.sh`, which reports how many of the 216 gates have no Bazel test

## The number

`parity.sh` reports **216 unmapped** and exits 1. That is the expected state and it stays red until phase 7. Reaching zero is what licenses deleting the Makefile.

## Teeth

Three things here could pass while doing nothing, so each is proven able to fail:

- The LLVM version assert: set to `99.`, `bazel test` exits 1 with `requires LLVM 99.x, llvm-config reports 22.1.8`; restored, exit 0.
- The prelude: `touch include/goo_assert.h` must trigger a recompile. A `copt` alone would not.
- `parity.sh`: adding one target that claims a real gate must move the count by exactly one, and removing it must move it back.

## Two findings

Counting for this work turned up two things that were already true:

- `m12-probe` (`Makefile:3101`) is referenced nowhere else and is absent from `VERIFY_ALL_DEPS`. It never runs. `parity.sh` deliberately reads the variable rather than scraping targets, so it does not report this as a gate needing migration.
- `src/main_simple.c` and `src/main_minimal.c` are reachable only from `test-main` and `$(ANALYZER)`, neither of which is in `VERIFY_ALL_DEPS`, and neither file is in `GOO_SRCS`. They neither ship nor pass a gate, which breaks the invariant `CLAUDE.md` states for `src/`. Phase 1 resolves them.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01DHrKLCPRvkeBRutFyP2vsh
BODY
)"
```

---

# PHASE 1 — The LLVM-free leaf

### Task 9: The assertion library as a Bazel target

**Files:**
- Create: `tests/unit/BUILD`

**Interfaces:**
- Consumes: Task 3's macros.
- Produces: `//tests/unit:goo_check`, a header-only `cc_library`. Every `cc_test` in Tasks 10-14 depends on it.

**Background.** orca's `testing/check.c` is deliberately NOT ported. `tests/unit/goo_check.h` already exists here and is stronger: it has a third outcome, `BROKEN` (exit 2), when a suite's executed row count disagrees with its declared count. Its own comment records why — `scripts/safety-baseline.txt` reached 139 dead entries out of 218 before anyone noticed, because a table whose rows stop executing prints nothing and exits 0.

- [ ] **Step 1: Confirm the header's real surface before wrapping it**

Run:
```bash
grep -nE '^\s*(static\s+)?(inline\s+)?[a-z].*goo_check[a-z_]*\(' tests/unit/goo_check.h | head -20
```
Record the exact function names. Task 10's test uses them and must not guess.

- [ ] **Step 2: Create `tests/unit/BUILD`**

```python
load("@rules_cc//cc:defs.bzl", "cc_library")

# The shared assertion and verdict surface for the C unit suites.
#
# orca's testing/check.c is NOT ported over this. This header carries a third
# outcome orca's does not -- BROKEN (exit 2) when a suite's executed row count
# disagrees with the count it declared -- and that is the outcome that catches
# a table whose rows silently stop executing.
cc_library(
    name = "goo_check",
    hdrs = ["goo_check.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 3: Verify it builds**

Run:
```bash
bazel build //tests/unit:goo_check 2>&1 | tail -3
```
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): expose goo_check.h as a library target

orca's testing/check.c is deliberately not ported. goo_check.h already carries
a third outcome orca's does not -- BROKEN, exit 2, when a suite's executed row
count disagrees with the count it declared. Replacing it would be a regression.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 10: The runtime library and the first ported suite

**Files:**
- Create: `src/runtime/BUILD`, `tests/unit/runtime/BUILD`

**Interfaces:**
- Consumes: `//tests/unit:goo_check`, `//tools:defs.bzl`.
- Produces: `//src/runtime:runtime`, a `cc_library` over the 11 files in `RUNTIME_SRCS`. Phase 4's probes depend on it. Also `//tests/unit/runtime:obj_header_test`.

**Why this suite first.** `obj_header_test` is the ONLY unit suite that already links narrowly, to `$(RUNTIME_LIB)` rather than `$(SRC_OBJS)`. Every other suite links the whole source tree, so this is the one place where the Makefile already states a real dependency set and Bazel does not have to discover it.

- [ ] **Step 1: Confirm the exact runtime source list**

Run:
```bash
awk '/^RUNTIME_SRCS/{f=1} f{print} f && !/\\[[:space:]]*$/{exit}' Makefile | tr ' ' '\n' | grep -oE 'runtime/[a-z_]+\.c' | sort
```
Expected: 11 files. Note that `src/runtime/` holds 13 `.c` files — `io.c` and `far_transport.c` are NOT in `RUNTIME_SRCS`. Do not add them.

- [ ] **Step 2: Write the failing test**

Create `tests/unit/runtime/BUILD`:
```python
load("//tools:defs.bzl", "goo_cc_test")

# The first suite ported, because it is the only one the Makefile already links
# narrowly: obj_header_test takes $(RUNTIME_LIB), where every other suite takes
# $(SRC_OBJS), the whole tree. Its dependency set is stated, not discovered.
goo_cc_test(
    name = "obj_header_test",
    size = "small",
    srcs = ["obj_header_test.c"],
    deps = [
        "//src/runtime",
        "//tests/unit:goo_check",
    ],
)
```

- [ ] **Step 3: Run it to verify it fails**

Run:
```bash
bazel test //tests/unit/runtime:obj_header_test 2>&1 | tail -5
```
Expected: FAIL — `//src/runtime` does not exist.

- [ ] **Step 4: Create `src/runtime/BUILD`**

```python
load("//tools:defs.bzl", "goo_cc_library")

# The 11 files in the Makefile's RUNTIME_SRCS, and only those. src/runtime/
# holds 13 .c files: io.c and far_transport.c are deliberately excluded,
# because RUNTIME_SRCS excludes them and this target must match what
# $(RUNTIME_LIB) actually contains.
goo_cc_library(
    name = "runtime",
    srcs = [
        "arena.c",
        "channels.c",
        "concurrency.c",
        "deadlock.c",
        "defer.c",
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

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
bazel test //tests/unit/runtime:obj_header_test --test_output=errors 2>&1 | tail -6
```
Expected: PASSED.

If it fails to link on an undefined symbol, that symbol names a real coupling the Makefile hid. Add the owning file to `srcs` only if it is in `RUNTIME_SRCS`; otherwise stop and report, because the suite is reaching outside `$(RUNTIME_LIB)` and the Makefile's own dependency line is wrong.

- [ ] **Step 6: Confirm Bazel and Make agree**

Run:
```bash
make obj-header-test > /tmp/mk.log 2>&1; echo "make exit=$?"
bazel test //tests/unit/runtime:obj_header_test > /tmp/bz.log 2>&1; echo "bazel exit=$?"
```
Expected: both 0. A disagreement here means the dependency sets differ, and the Bazel one is the suspect.

- [ ] **Step 7: Commit**

```bash
git add src/runtime/BUILD tests/unit/runtime/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): port the runtime library and obj_header_test

obj_header_test goes first because it is the only unit suite the Makefile
already links narrowly -- $(RUNTIME_LIB) rather than $(SRC_OBJS). Its real
dependency set is stated rather than discovered, so it is the one suite that
cannot mislead about what the unit under test actually needs.

The target carries the 11 files in RUNTIME_SRCS and not the 13 in the
directory: io.c and far_transport.c are excluded because RUNTIME_SRCS excludes
them.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 11: Prove the ported suite can still fail

**Files:**
- No new files. This task runs a mutation and records the result.

**Interfaces:**
- Consumes: `//tests/unit/runtime:obj_header_test`.
- Produces: evidence, recorded in the commit message, that the ported suite reports RED on a real defect.

**Why this task exists.** A ported test that passes proves the build works. It does not prove the test still tests anything: a dependency set that omits the unit under test, or a `main()` that returns 0 before the table runs, both produce a green PASSED. The Makefile-side suite has teeth; this confirms the Bazel-side one inherited them.

- [ ] **Step 1: Identify a predicate to mutate**

Run:
```bash
grep -nE 'GOO_OBJ_HEADER_SIZE|goo_zerobase' include/*.h src/runtime/runtime.c | head -10
```
Pick one arithmetic or comparison the header contract depends on. Record the file and line.

- [ ] **Step 2: Mutate it and confirm RED**

```bash
TARGET_FILE=<the file from step 1>
cp "$TARGET_FILE" /tmp/mutant.bak
# Apply a single-character change to the predicate identified above,
# for example flipping a >= to > or changing an offset by one.
bazel test //tests/unit/runtime:obj_header_test > /tmp/red.log 2>&1; echo "RED exit=$?"
tail -20 /tmp/red.log
```
Expected: non-zero exit, and the log names a failing row. If it exits 0, the suite does not exercise that predicate — pick another and repeat. If three attempts all pass, stop and report: the Bazel dependency set is probably wrong.

- [ ] **Step 3: Restore and confirm GREEN**

```bash
cp /tmp/mutant.bak "$TARGET_FILE"
bazel test //tests/unit/runtime:obj_header_test > /tmp/green.log 2>&1; echo "GREEN exit=$?"
git diff --stat "$TARGET_FILE"
```
Expected: exit 0, and `git diff --stat` prints nothing. A mutation left in the tree would be compiled silently by every later task.

- [ ] **Step 4: Confirm the tree is clean before committing**

Run:
```bash
git status --short src/ include/
```
Expected: no output.

- [ ] **Step 5: Commit the evidence**

There is no file change, so commit the record with `--allow-empty`:
```bash
git -c commit.gpgsign=false commit --allow-empty -m "test(bazel): record that obj_header_test kept its teeth after porting

A ported test that passes proves the build works, not that the test still
tests anything. A dependency set that omits the unit under test, or a main()
that returns before the table runs, both produce a green PASSED.

Mutated <file>:<line>, confirmed the suite reports RED and names the failing
row, restored it and confirmed GREEN. The tree is clean: a mutation left
behind would be compiled silently by every later task.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```
Replace `<file>:<line>` with the real location from Step 1.

---

### Task 12: Resolve the two ungated files in src/

**Files:**
- Move: `src/main_simple.c`, `src/main_minimal.c` (destination decided in Step 3)
- Modify: possibly `Makefile` (only if the files move — this is the single exception in these two phases)

**Interfaces:**
- Consumes: nothing.
- Produces: an `src/` tree in which every file either ships or is exercised by a gate.

**Background.** `CLAUDE.md` states: *"`src/` contains what SHIPS, or what a GATE exercises — nothing else."* `src/main_simple.c` is reachable only from `test-main` and `src/main_minimal.c` only from `$(ANALYZER)`. Neither target is in `VERIFY_ALL_DEPS`, and neither file is in `GOO_SRCS`. So they neither ship nor pass a gate.

- [ ] **Step 1: Re-verify the claim before acting on it**

Do not delete on the strength of this plan. Run:
```bash
for f in main_simple main_minimal; do echo "--- $f"; grep -n "$f" Makefile; done
echo "=== control: a file that IS gated ==="
grep -c 'runtime/arena.c' Makefile
```
Expected: each file appears only in the two lines quoted above, and the control prints a non-zero count proving the search works.

- [ ] **Step 2: Confirm neither is in the shipped object set**

Run:
```bash
make -pn 2>/dev/null | grep -E '^GOO_SRCS|^GOO_OBJS' | head -2 | grep -cE 'main_simple|main_minimal'
```
Expected: `0`.

- [ ] **Step 3: Decide the destination**

Two options. This is a judgment call the implementer must put to the user, not decide alone:

- **Move to `attic/src/`** — matches the 2026-08-17 quarantine precedent for unlinked subsystems, and is reversible.
- **Adopt them with a gate** — add `test-main` and `analyzer` to `VERIFY_ALL_DEPS`, making them real. Costs build time in `verify-core` for two binaries nothing depends on.

Ask the user which, and record the answer in the commit message. Do not proceed without it.

- [ ] **Step 4: Apply the decision**

If moving:
```bash
mkdir -p attic/src
git mv src/main_simple.c attic/src/main_simple.c
git mv src/main_minimal.c attic/src/main_minimal.c
```
Then update `Makefile:210` and `Makefile:4641-4642` to point at the new paths, or delete those two targets if the user chose deletion.

- [ ] **Step 5: Verify make is still green**

Run backgrounded with a 600s window:
```bash
make verify-core > /tmp/vc.log 2>&1; echo "verify-core exit=$?"
tail -5 /tmp/vc.log
```
Expected: exit 0. This is the one task in these phases that edits the Makefile, so this check is mandatory.

- [ ] **Step 6: Verify the invariant now holds**

Run:
```bash
for f in $(find src -maxdepth 1 -name '*.c'); do echo "still at src root: $f"; done
```
Expected: no output, if the files moved.

- [ ] **Step 7: Commit**

```bash
git add -A src attic Makefile
git -c commit.gpgsign=false commit -m "refactor(src): resolve two files that neither ship nor pass a gate

CLAUDE.md states the invariant: src/ contains what SHIPS, or what a GATE
exercises -- nothing else. main_simple.c reached only test-main and
main_minimal.c only \$(ANALYZER). Neither target is in VERIFY_ALL_DEPS and
neither file is in GOO_SRCS, so both broke it.

Found while counting gates for the Bazel migration spec, not by a gate --
which is the point: nothing in the tree was watching for this.

verify-core green after the move.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 13: Dependency discovery, and the release_decision suite

**Files:**
- Create: `src/types/BUILD`, `tests/unit/types/BUILD`

**Interfaces:**
- Consumes: `//tests/unit:goo_check`, `//tools:defs.bzl`.
- Produces: `//src/types:release_decision` and `//tests/unit/types:release_decision_test`. Task 14 follows the same procedure for three more suites.

**Background.** Every suite except `obj_header_test` links `$(SRC_OBJS)` — the whole source tree, LLVM included — so the Makefile does not state what any of them actually needs. Bazel requires the real set. The procedure below discovers it rather than guessing it.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/types/BUILD`:
```python
load("//tools:defs.bzl", "goo_cc_test")

# Dependency set discovered, not copied. The Makefile links $(SRC_OBJS) for
# this suite -- the whole tree -- so it states nothing about what the unit
# under test actually needs. Start narrow and widen on real link errors only.
goo_cc_test(
    name = "release_decision_test",
    size = "small",
    srcs = ["release_decision_test.c"],
    deps = [
        "//src/types:release_decision",
        "//tests/unit:goo_check",
    ],
)
```

- [ ] **Step 2: Run it to verify it fails**

Run:
```bash
bazel test //tests/unit/types:release_decision_test 2>&1 | tail -5
```
Expected: FAIL — `//src/types:release_decision` does not exist.

- [ ] **Step 3: Create the narrowest plausible library**

Create `src/types/BUILD`:
```python
load("//tools:defs.bzl", "goo_cc_library")

goo_cc_library(
    name = "release_decision",
    srcs = ["release_decision.c"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 4: Run the discovery loop**

Run:
```bash
bazel test //tests/unit/types:release_decision_test 2>&1 | grep -E 'undefined reference|error:' | head -20
```

For each `undefined reference to 'symbol'`, find the file that defines it:
```bash
grep -rln '^[a-zA-Z_].*\bSYMBOL\s*(' src/ --include='*.c'
```
Add that file to the `srcs` of a `goo_cc_library` — a new one if it is a separable unit, or this one if it is genuinely part of the same unit. Re-run. Repeat until the link succeeds.

**Stop conditions.** Report to the user rather than continuing if either occurs:
- The loop pulls in a file under `src/codegen/`, which means the suite transitively needs LLVM and does not belong in phase 1.
- The loop exceeds six iterations, which means the unit is not separable and the coupling is worth discussing before encoding it in a BUILD file.

- [ ] **Step 5: Record what was discovered**

Add a comment to the final `goo_cc_library` naming the symbols that forced each addition, in this form:
```python
goo_cc_library(
    name = "release_decision",
    srcs = [
        "release_decision.c",
        # Added for: <symbol>, referenced by release_decision.c
        "escape_core.c",
    ],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 6: Verify it passes and agrees with make**

Run:
```bash
bazel test //tests/unit/types:release_decision_test > /tmp/bz.log 2>&1; echo "bazel exit=$?"
make release-decision-test > /tmp/mk.log 2>&1; echo "make exit=$?"
```
Expected: both 0.

- [ ] **Step 7: Prove the ported suite has teeth**

Repeat Task 11's mutation procedure against `src/types/release_decision.c`. The Makefile side already has a mutation harness at `scripts/release_decision_teeth.sh`; read it for a predicate known to be covered:
```bash
head -30 scripts/release_decision_teeth.sh
```
Mutate that predicate, confirm RED, restore, confirm GREEN, and confirm `git diff --stat` is empty.

- [ ] **Step 8: Commit**

```bash
git add src/types/BUILD tests/unit/types/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): port release_decision_test with a discovered dependency set

The Makefile links \$(SRC_OBJS) for this suite -- the whole source tree,
LLVM included -- so it states nothing about what the unit under test needs.
The Bazel target starts at release_decision.c alone and widens only on a real
undefined reference, with a comment naming the symbol that forced each
addition.

Teeth confirmed by mutating a predicate that scripts/release_decision_teeth.sh
already covers: RED, restored, GREEN, clean tree.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 14: The three escape suites

**Files:**
- Modify: `src/types/BUILD`, `tests/unit/types/BUILD`

**Interfaces:**
- Consumes: Task 13's discovery procedure.
- Produces: `//tests/unit/types:param_escape_test`, `:block_escape_test`, `:local_escape_test`.

- [ ] **Step 1: Add the three test targets**

Append to `tests/unit/types/BUILD`:
```python
goo_cc_test(
    name = "param_escape_test",
    size = "small",
    srcs = ["param_escape_test.c"],
    deps = [
        "//src/types:param_escape",
        "//tests/unit:goo_check",
    ],
)

goo_cc_test(
    name = "block_escape_test",
    size = "small",
    srcs = ["block_escape_test.c"],
    deps = [
        "//src/types:block_escape",
        "//tests/unit:goo_check",
    ],
)

goo_cc_test(
    name = "local_escape_test",
    size = "small",
    srcs = ["local_escape_test.c"],
    deps = [
        "//src/types:local_escape",
        "//tests/unit:goo_check",
    ],
)
```

- [ ] **Step 2: Run all three to verify they fail**

Run:
```bash
bazel test //tests/unit/types:all 2>&1 | tail -8
```
Expected: three failures naming the three missing libraries.

- [ ] **Step 3: Add the three libraries, narrowest first**

Append to `src/types/BUILD`:
```python
goo_cc_library(
    name = "param_escape",
    srcs = ["param_escape.c"],
    visibility = ["//visibility:public"],
)

goo_cc_library(
    name = "block_escape",
    srcs = ["block_escape.c"],
    visibility = ["//visibility:public"],
)

goo_cc_library(
    name = "local_escape",
    srcs = ["local_escape.c"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 4: Run Task 13's discovery loop for each**

Run:
```bash
bazel test //tests/unit/types:all 2>&1 | grep -E 'undefined reference' | sort -u
```
Resolve each as in Task 13 Step 4, with the same two stop conditions. `escape_core.c` is the likely shared dependency — if two or more libraries need it, give it its own `goo_cc_library(name = "escape_core", srcs = ["escape_core.c"])` and depend on that rather than listing the file twice.

- [ ] **Step 5: Verify all four suites pass under both build systems**

Run:
```bash
bazel test //tests/unit/... > /tmp/bz.log 2>&1; echo "bazel exit=$?"
for t in param-escape-test block-escape-test local-escape-test release-decision-test; do
    make "$t" > "/tmp/mk-$t.log" 2>&1; echo "$t make exit=$?"
done
```
Expected: every status 0.

- [ ] **Step 6: Confirm no LLVM leaked into the leaf**

Phase 1 is defined as the LLVM-free leaf. Run:
```bash
bazel query 'somepath(//tests/unit/..., @llvm//:llvm_c)' 2>/dev/null | head
```
Expected: no output. Any path means a suite in this phase depends on LLVM and belongs in phase 3.

- [ ] **Step 7: Commit**

```bash
git add src/types/BUILD tests/unit/types/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): port the three escape suites

Same discovery procedure as release_decision_test: start at the single .c file
and widen only on a real undefined reference. escape_core.c is shared, so it
gets its own target rather than being listed in three srcs lists.

Confirmed no path from //tests/unit/... to @llvm//:llvm_c, which is what makes
this the LLVM-free leaf rather than an assertion that it is.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 15: Close phase 1

**Files:**
- No new files.

**Interfaces:**
- Consumes: everything in phases 0 and 1.
- Produces: a recorded parity count and a phase 1 PR.

- [ ] **Step 1: Read the parity count**

Run:
```bash
./tools/parity.sh; echo "exit=$?"
```
Expected: `unmapped` has dropped by the number of gates the ported suites claim. Record the exact before and after numbers.

- [ ] **Step 2: Confirm the drop matches the suites ported**

The suites ported are `obj-header-test`, `release-decision-test`, `param-escape-test`, `block-escape-test`, `local-escape-test`. Run:
```bash
for g in obj-header-test release-decision-test param-escape-test block-escape-test local-escape-test; do
    printf '%-24s %s\n' "$g" "$(./tools/parity.sh 2>/dev/null | grep -cx "  $g")"
done
```
Expected: every line prints `0`, meaning none is still unmapped. A `1` means the target name does not match the convention — fix the target name, not `parity.sh`.

- [ ] **Step 3: Confirm both build systems are green**

Run backgrounded with a 600s window:
```bash
bazel test //... > /tmp/bz.log 2>&1; echo "bazel exit=$?"
make verify-core > /tmp/vc.log 2>&1; echo "verify-core exit=$?"
./tools/parity_selftest.sh; echo "selftest exit=$?"
```
Expected: bazel 0, verify-core 0, selftest 0.

- [ ] **Step 4: Open the phase 1 PR**

```bash
git push -u origin build/bazel-phase1-leaf
gh pr create --title "build(bazel): phase 1, the LLVM-free leaf" --body "$(cat <<'BODY'
Phase 1 of the Bazel migration. See `docs/superpowers/specs/2026-08-25-bazel-migration-design.md`.

Still additive: `make verify-core` remains the gate and no probe is removed.

## What is here

- `//tests/unit:goo_check` wraps the existing header. orca's `check.c` is deliberately not ported — `goo_check.h` has a third outcome, `BROKEN`, that orca's lacks.
- `//src/runtime:runtime` and `obj_header_test`, the one suite the Makefile already linked narrowly.
- `release_decision_test` and the three escape suites, each with a **discovered** dependency set rather than a copied one.
- `src/main_simple.c` and `src/main_minimal.c` resolved.

## On dependency discovery

Every suite except `obj_header_test` links `$(SRC_OBJS)` — the whole source tree, LLVM included — so the Makefile states nothing about what any of them needs. Each Bazel target starts at a single `.c` file and widens only on a real undefined reference, with a comment naming the symbol that forced each addition.

## Teeth

Each ported suite was mutated and confirmed to report RED, then restored and confirmed GREEN, with a clean tree afterwards. A ported test that passes proves the build works, not that the test still tests anything.

`bazel query 'somepath(//tests/unit/..., @llvm//:llvm_c)'` returns nothing, which is what makes this the LLVM-free leaf rather than a claim that it is.

## Parity

`parity.sh` moved from 216 unmapped to <N>. It stays red until phase 7.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01DHrKLCPRvkeBRutFyP2vsh
BODY
)"
```
Replace `<N>` with the real number from Step 1.

---

## What phases 2-7 still need

This plan stops at the end of phase 1 deliberately. Phases 2-7 get their own plan once the foundation is proven, because every task in them assumes the `.bazelrc`, the prelude macros and `@llvm//:llvm_c` behave as designed. Writing them now would mean writing code blocks against a foundation that does not exist yet.

The open questions phase 1 will answer, and which the phase 2-7 plan needs:

- Does the discovery loop in Tasks 13-14 terminate cheaply, or is `src/types/` too coupled to split? That decides whether phase 3 can use fine-grained libraries or needs one large `//src:compiler_core`.
- Does `goo_cc_library`'s forced-include approach survive a file that includes `xalloc.h` explicitly? A duplicate include is harmless in C, but a duplicate `-include` plus a header without a guard is not.
- How many gates does one ported suite claim? Task 15 Step 2 measures it, which is what makes the phase 4 estimate real rather than assumed.
