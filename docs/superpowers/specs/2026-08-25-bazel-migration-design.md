# Bazel migration

**Status:** design approved, not implemented
**Date:** 2026-08-25
**Author:** Darragh Downey (with Claude)
**Model repo:** `~/Workspace/github.com/orca`

## 1. Context

`bin/goo` is built by a 6,489-line Makefile. That Makefile is not only a build
system: it is also the project's gate net. `VERIFY_ALL_DEPS` names **217**
gates, of which `verify-core` runs **214** (the three in `HEAVY_DEPS` need
CompCert or podman). 183 of the 217 are `*-probe:` targets and 34 are not.

The Makefile defines 184 `*-probe:` targets. The extra one, `m12-probe`
(`Makefile:3101`), is referenced nowhere else: no target depends on it and it
is absent from `VERIFY_ALL_DEPS`. **It never runs.** This is the defect class
`parity.sh` exists to surface, and it was found while counting for this spec.

A second instance sits in `src/`. `CLAUDE.md` states the invariant: *"`src/`
contains what SHIPS, or what a GATE exercises — nothing else."* Two files break
it. `src/main_simple.c` is used only by `test-main`, and `src/main_minimal.c`
only by `$(ANALYZER)`. Neither target is in `VERIFY_ALL_DEPS`, and neither file
is in `GOO_SRCS`, so neither ships in `bin/goo` nor is exercised by a gate.
Phase 1 must resolve them: either a gate adopts them, or they move to
`attic/src/`. Giving them a Bazel target that nothing depends on would carry
the debris across the migration, which is the outcome to avoid.

Measured on 2026-08-25:

| Artefact | Count |
|---|---|
| `src/**/*.c` | 71 (69 in 11 subdirectories, 2 at the root) |
| `include/**/*.h` | 96 |
| `tests/**/*.c` | 65 |
| `tests/**/*.goo` fixtures | 271 |
| `scripts/*.sh` | 46 |
| Gates in `VERIFY_ALL_DEPS` that are probes | 183 (155 inline, 28 script-backed) |
| Gates in `VERIFY_ALL_DEPS` that are not probes | 34 |
| `*-probe:` targets defined in the Makefile | 184 (one unreachable) |
| Distinct LLVM C API calls | 235, across 19 files |

Host toolchain on the development machine: bazel 9.2.0, llvm-config 22.1.8,
clang 22.1.8, gcc 16.2.1, bison 3.8.2, flex 2.6.4.

### Why change

The Makefile has three defects that are structural, not incidental:

1. **Sanitizers are per-target hand-rolled invocations.** `far-transport-asan`
   and `obj-header-tsan` each write their own `clang -fsanitize=...` line for
   one file, and `arena-valgrind-probe` does the same for valgrind. All three
   are in `verify-core`, so **211 of the 214 core gates run under no sanitizer
   and no valgrind at all.** There is no ubsan and no msan anywhere.
2. **Missing tools produce a skip, not a failure.** `arena-valgrind-probe` and
   `obj-header-tsan` both print `SKIPPED` and exit 0 when the tool is absent.
   The Makefile's own comment names this: *"a silent skip reads as a pass in a
   log."*
3. **A missing LLVM degrades silently.** `Makefile:60` sets
   `LLVM_CFLAGS = -DLLVM_AVAILABLE=0` and emits a `$(warning)`. A build with no
   code generator still exits 0.

Bazel addresses each one by a different mechanism:

- Defect 1 goes away because a sanitizer becomes a build *config* applied to
  the whole tree, not a target someone remembered to write. `--config=asan`
  covers all 214 gates instead of one.
- Defect 2 goes away because a config's tool is a build input. A missing
  valgrind fails the action; it cannot produce a skip that reads as a pass.
- Defect 3 goes away because the LLVM `repository_rule` calls `fail()` rather
  than defining `LLVM_AVAILABLE=0`.

### Goal

`bazel test //...` is the gate. The Makefile is deleted. Nothing that
`verify-core` proves today is lost in the process, and that claim is
**measured** rather than asserted.

## 2. Decisions

Four decisions were taken during design. Each records the alternatives and why
they were rejected.

### D1. Full migration, not parallel adoption

**Chosen:** everything moves to Bazel; the Makefile is deleted.

*Rejected — parallel adoption* (Bazel builds `src/` and the unit tests, Make
keeps the probes): avoids the largest chunk of work, but leaves two build
systems to maintain indefinitely and gives the sanitizer configs nothing to
apply to, since the probes are where nearly all execution happens.

*Rejected — leaf libraries only*: a useful beachhead, but it excludes
`src/codegen` and therefore excludes every gate that actually runs a compiled
program.

*Rejected — patterns without Bazel*: cheapest, and it would deliver the
sanitizer matrix. It cannot deliver the sandbox, so the non-hermetic
`llvm-config` shell-out at Makefile parse time survives.

**Cost accepted:** this is the largest of the four options and it rewrites the
gate net. D4 exists to make that safe.

### D2. LLVM by host `llvm-config`, with a version assertion

**Chosen:** a `repository_rule` shells out to `llvm-config`, exactly as the
Makefile does, but calls `fail()` when the version does not begin with `22.`.

*Rejected — pinned release tarball* (`http_archive` of the official LLVM 22
linux-x86_64 build): fully reproducible, which suits the existing
`repro-build-probe`. Costs roughly 1 GB on a cold cache and pins the project to
upstream's build configuration rather than Fedora's.

*Rejected — `toolchains_llvm`*: most reproducible option, and it removes the
host gcc/clang dependency as well. Swapping the C toolchain would surface new
warnings across all 71 source files, which is a second migration wearing the
costume of a first.

**Note:** this decision does **not** improve reproducibility over the Makefile.
It is chosen for parity of risk, and it converts a silent downgrade into a hard
failure, which is a real gain. Revisiting it later is cheap: only
`third_party/llvm/` changes.

### D3. A `goo_probe` macro, with its own teeth fixture

**Chosen:** one Starlark macro generates the compile/run/diff for the 155
inline probes. The 28 script-backed probes become `sh_test` with the script as
`data`.

*Rejected — mechanical 1:1* (183 separate `sh_test` rules, each wrapping its
recipe verbatim): highest fidelity and the easiest to diff against the
Makefile. Costs 183 near-duplicate shell scripts to maintain forever.

*Rejected — macro with no teeth fixture*: less work, and it would leave 155
gates depending on a code path never observed reporting a failure.

**The risk this creates, and the answer.** If the macro is wrong, all 155 tests
are wrong the same way and all 155 still print PASS. `testing/teeth` therefore
holds a `goo_probe` whose fixture output deliberately disagrees with its
expected file. `tools/verify_sanitizers.sh` asserts it goes RED, exactly as it
does for the four sanitizer defects. This mirrors `orca/testing/oom.bzl`, whose
docstring already states the principle: *"A target that sets them by hand and
omits linkstatic can silently inject nothing, so do not hand-roll them."*

### D4. Parity-gated strangler cutover

**Chosen:** Bazel and Make coexist. `tools/parity.sh` reads `VERIFY_ALL_DEPS`
from the Makefile, reads the test list from `bazel query`, and prints every
gate with no Bazel counterpart. The Makefile is deleted only when the count
reaches zero.

*Rejected — phase by phase with review only*: progress is visible in the diff,
but a probe dropped in the middle of a 6,489-line file is caught by human
attention rather than by a tool.

*Rejected — big bang*: shortest calendar time. A green `bazel test //...`
proves the tests that exist pass; it says nothing about the ones never written.
With 217 gates, that gap is the whole risk.

## 3. Target layout

```
MODULE.bazel  MODULE.bazel.lock  .bazelversion  .bazelrc  .bazelignore

third_party/llvm/
    llvm.bzl          repository_rule: version assert + cc_library generation
    ext.bzl           module extension wrapping it
    BUILD.llvm.tpl    template for the generated @llvm repo
    BUILD

tools/
    defs.bzl              goo_cc_library / goo_cc_test (prelude wiring)
    goo_probe.bzl         the probe macro
    run_probe.sh          shared runner: compile, run, diff
    parity.sh             Make <-> Bazel gate parity
    verify_sanitizers.sh  red/green proof for every config and for goo_probe
    covcheck.py           coverage floor gate (ported from orca)
    covcheck_test.py      the gate's own test
    coverage.sh
    coverage-policy.json  per-file coverage floors
    BUILD

testing/
    check.c check.h   assertion library
    BUILD
testing/teeth/
    asan_defect.c ubsan_defect.c tsan_defect.c msan_defect.c
    probe_defect.goo  probe_defect.expected.txt
    BUILD             every target tags = ["manual"]

include/BUILD         cc_library "prelude": xalloc.h + goo_assert.h
src/<pkg>/BUILD       one cc_library per directory (11 subdirectories;
                      the 2 root .c files are resolved in phase 1, not ported)
src/parser/BUILD      genrule(bison) + cc_library + grammar tripwire sh_test
src/compiler/BUILD    cc_binary "goo"
src/runtime/BUILD     cc_library replacing libgoo_runtime.a

tests/unit/**/BUILD   cc_test per suite
tests/probes/BUILD    155 goo_probe calls
tests/golden/BUILD    golden suites (271 fixtures as data)
goostd/BUILD          filegroup, a data dependency of probes that import stdlib
```

## 4. Mechanisms

### 4.1 The prelude

`Makefile:23` forces two headers into every translation unit:

```
-include include/xalloc.h -include include/goo_assert.h
```

Putting those flags in `.bazelrc` would compile, but Bazel would not know the
headers are inputs, so editing `goo_assert.h` would rebuild nothing. Instead
`tools/defs.bzl` provides `goo_cc_library` and `goo_cc_test`, which add both the
`copts` and a dependency on `//include:prelude`, where the headers are declared
as `hdrs`.

Every C target in the repo uses these macros. A bare `cc_library` would compile
without the prelude and fail at link, which is a loud failure, so this needs no
extra gate.

### 4.2 Bison

`Makefile:225` uses a GNU Make 4.3 grouped target (`&:`) so that one bison run
produces both `parser.tab.c` and `parser.tab.h`. Its comment explains that
without this, `-j` can invoke bison twice concurrently.

In Bazel this is a `genrule` with two entries in `outs`. A multi-output action
is scheduled once by construction, so the race cannot occur and the workaround
disappears.

The conflict tripwire (`scripts/grammar-tripwire.sh`) becomes an `sh_test` that
runs bison and compares against `EXPECTED_SR=31` and `EXPECTED_RR=0`. Per
`CLAUDE.md`, any delta remains stop-the-line.

### 4.3 LLVM

`third_party/llvm/llvm.bzl` runs:

```
llvm-config --version --includedir --libdir --libs core
```

It calls `fail()` when the version does not begin with `22.`, then writes a
`cc_library` exposed as `@llvm//:llvm_c` carrying the include path and the link
options. Only `src/codegen`, `src/compiler` and three files in `src/types`
depend on it.

### 4.4 The probe macro

```python
goo_probe(name, src, expected, args = [], data = [], tags = [])
```

generates an `sh_test` running `tools/run_probe.sh`, with the compiler binary,
the runtime library, the `.goo` fixture and the `.expected.txt` file as
runfiles. The runner compiles, executes, diffs, and exits non-zero on any
difference.

`run_probe.sh` must never mask a failure behind a pipe. Per `CLAUDE.md`, a
pipeline reports only its last stage's status.

### 4.5 The parity script

```
$ tools/parity.sh
make gates:   217
bazel tests:  204

UNMAPPED (13):
  v2-bootstrap-pilot
  repro-build-probe
  ...

parity.sh: 13 gates have no Bazel test
exit 1
```

Mapping is by name, using a normalisation rule (`switch-probe` maps to
`//tests/probes:switch`). Gates that are deliberately not migrated must be
listed in an explicit allowlist file with a reason, never dropped silently.

**parity.sh is the load-bearing component of this migration.** It is written in
phase 0, and phase 0 also proves it can report the opposite result: adding one
temporary Bazel target must move the count by exactly one, and removing one
gate name must move it back. A parity script that cannot detect a change is
indistinguishable from one reporting success.

## 5. Sanitizers, teeth, and coverage

`.bazelrc` follows orca's structure: a `san-base` config carrying
`-fno-omit-frame-pointer`, `-g` and `--strip=never`, with `asan`, `ubsan`,
`tsan` and `msan` layered on top. `valgrind` is a `--run_under` config. GCC is
available as `--config=gcc`.

`msan` is declared but **not claimed to work** until a red and green run is
recorded, because MSan needs an instrumented libc to avoid reporting glibc
internals.

Each config is proven by a deliberate defect in `testing/teeth`, tagged
`manual` so `bazel test //...` never runs it. `tools/verify_sanitizers.sh`
asserts each defect is RED under its own config and GREEN without it. The
second assertion matters as much as the first: a defect that fails in both
cases proves the code is broken, not that the sanitizer works.

Coverage ports orca's `covcheck.py` plus `coverage-policy.json`, with its own
`py_test`. Per orca's measured finding, coverage runs under GCC:
`bazel coverage` under Clang exits 0 and writes a report where every file reads
`LH:0 LF:0`, which is a silent false green.

## 6. CI

`.github/workflows/tests.yml` becomes a matrix over
`["", "--config=asan", "--config=ubsan", "--config=tsan"]`, plus a separate job
running `tools/verify_sanitizers.sh`. A third job runs `tools/parity.sh`, which
stays red until phase 7 and is therefore `continue-on-error` until then.

`repro.yml` keeps its podman gates, which are tagged rather than migrated.

## 7. Phases

Each phase is one PR. `parity.sh` runs from phase 0 and counts down from 217.

| # | Content | Exit criterion |
|---|---|---|
| 0 | Skeleton, `.bazelrc`, LLVM rule, `parity.sh` + its positive control | `parity.sh` reports 217 unmapped and its control passes |
| 1 | Leaf libraries and their unit tests. No LLVM, no bison. Resolve `main_simple.c` / `main_minimal.c`. | Unit suites green under Bazel and Make; `src/` holds nothing ungated |
| 2 | bison genrule, tripwire `sh_test`, lexer/parser/ast | Tripwire reports 31 S/R, 0 R/R |
| 3 | types, codegen, `//src/compiler:goo` | Bazel-built `goo` passes the same golden suite as `make bin/goo` |
| 4 | `goo_probe` macro, its teeth fixture, 155 inline probes | Teeth fixture goes RED; 155 probes green |
| 5 | 28 script probes, golden suites, `goostd` filegroup | — |
| 6 | Sanitizer configs, `testing/teeth`, coverage gate | `verify_sanitizers.sh` exits 0 |
| 7 | CI matrix, heavy gates tagged, **Makefile deleted** | `parity.sh` exits 0 |

Phase 3 carries the technical risk: 235 LLVM C API calls across 19 files must
link under a new dependency graph. Phases 4 and 5 carry the volume.

## 8. Risks

| Risk | Mitigation |
|---|---|
| The `goo_probe` macro is wrong in one way, so 155 gates fail identically and silently | The teeth fixture in `testing/teeth`, asserted by `verify_sanitizers.sh` |
| A gate is lost during migration | `parity.sh`, with an explicit allowlist for deliberate omissions |
| `parity.sh` itself is wrong | Phase 0 positive control: one added target must move the count by exactly one |
| Phase 3 link failure against `@llvm//:llvm_c` | Phase 3 compares the Bazel-built compiler against the Make-built one on the same golden suite |
| Bazel sandbox breaks probes that assume the source tree | Probes get explicit `data` deps; a missing runfile fails loudly rather than reading the worktree |
| Two build systems drift during phases 1-6 | Both run in CI until phase 7 |

## 9. Non-goals

- Reproducible LLVM. D2 keeps the host dependency. Revisit later; only
  `third_party/llvm/` changes.
- A reproducible C toolchain. `.bazelrc` pins absolute compiler paths, as orca
  does, but does not download one.
- Remote caching or a remote execution service.
- Rewriting probe *logic*. A probe's assertions move verbatim; only its harness
  changes.
- Migrating `attic/src/`. It links into no binary and stays out of the build.
