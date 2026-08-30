# Bazel migration — handoff, 2026-08-25

## State

| Phase | PR | Status |
|---|---|---|
| 0 + 1 foundation, LLVM-free leaf | #321 | merged |
| 2 front end | #322 | merged |
| 3 codegen + compiler | #323 | merged |
| 3b runtime archive (no NNG) | #324 | merged |
| 4 probe gates, tasks 1–5 of 17 | **#325** | **open, green, CLEAN** |
| 4 probe gates, tasks 6–7 (the two derivation gates) | **#330** | **open** |

**Parity: 222 gates** as of #330 (was 217). `make verify-core` and
`bazel test //...` are both green. `bazel test //...` runs 82 tests.

## Pick up here

`docs/superpowers/plans/2026-08-25-bazel-phase4-probes.md`, **tasks 8 onward**.
Tasks 1–7 and the two added ones (4b, 4c) are done. WARNING: the 1–5 checkboxes
in that file were never ticked, though every artefact is present — do not read
an empty box there as open work. 6 and 7 are ticked.

Tasks 6 and 7 are done in #330: `tests/probes/targets_current.sh`, in make as
`targets-current-probe` AND under `bazel test //...`, with a `--self-test` whose
three mutations each run on a COPY of the tree.

**Next is tasks 8–13** (the 69 printf-generated probes, extracted to real files
behind a drift gate), then 14 (the 28 script-backed), 15 (the bespoke EIGHT, up
from six — the two new derivation gates classify there, correctly: they are
hand-written sh_tests), 16–17 (CI, close).

**Read this before task 8.** `census_current` — the pattern task 6 was told to
copy — had NEVER RUN. It is tagged `manual`, so `bazel test //...` skips it, and
it had no Makefile target and no workflow. Its first execution found the census
stale: `link-flags-probe` had landed in #329 through nine green CI checks
without it. Two lessons for the remaining tasks:

- A `manual` tag is not a wiring plan. If you tag a gate `manual` because of
  nested bazel, add the make-side target in the SAME commit.
- `probe-teeth-probe` scans `scripts/*.sh` only, so nothing under
  `tests/probes/` is checked for teeth. `census_current.sh` still has no
  `--self-test`. Widening that scan is unclaimed work.

## What is still refused, and why

The generator omits 17 of the 80 fixture probes, each with a printed reason.
Run it to see them:

    python3 tools/gen_probe_targets.py tests/probes/census.txt >/dev/null

  12  no expected file          -- not yet investigated
   3  printf mixed with a fixture (asi-gocompat has 22 cases, readline, osargs)
   1  arc-release-probe          24 fixtures in one recipe
   1  passes argv to the binary

The 12 are the only genuinely unknown group left. Look at those first.

## Three things a successor should know

**The generator refuses rather than guesses, and that is the design.** Its
first run emitted zero of eighty. The printed refusals are what found three
wrong parse rules and an entire missing category. If it ever starts emitting
targets for recipes it does not understand, that property is gone.

**A reject probe is not an exit-code check.** It asserts four things, and the
fourth -- that stderr carries neither "Module verification failed" nor
"LLVM ERROR" -- separates a language rejection from a compiler crash. Both give
a non-zero exit. `stderr_contains` is mandatory for the same reason: "the
compile failed" is also satisfied by a typo in a fixture name.

**Compare against `make`, all of them, not a sample.** Every generated probe so
far was run under both build systems: 63 agree, 0 disagree. The comparison is
scripted, so the cost is runtime rather than effort.

## Deliberate divergences from the Makefile, to close later

- `//src/runtime:runtime_full` has **12** objects, not the 13 of `RUNTIME_OBJS`.
  `far_transport.c` cannot compile without NNG headers. Phase 3c closes this.
- 9 golden fixtures fail under Bazel: `far_shim_probe` and eight `lanes_*`.
  Same cause. `tools/golden_bazel.sh` asserts 486/9 exactly; when 3c lands it
  becomes 495/0.
- The default compiler is GCC, inverting orca's clang default, so phase 3's
  compiler comparison changes one variable rather than two.

## Things that exist only to compare two build systems

Phase 7 deletes all of these along with the Makefile:

    tools/compiler_differential.sh     both compilers emit identical IR
    tools/golden_bazel.sh              the 486/9 split assertion
    tools/parity.sh + selftest         the gate parity count
    tools/probe_census.sh              which macro each probe gets

## Two gates that were found not running at all

Recorded because the class matters more than the instances:

- `m12-probe` (`Makefile:3101`) is defined, referenced nowhere, and absent from
  `VERIFY_ALL_DEPS`. It has never run. Still true; not fixed here.
- `scripts/grammar-tripwire.sh` was stated in `CLAUDE.md` as stop-the-line and
  had no Makefile target, no `VERIFY_ALL_DEPS` entry and no workflow. Phase 2
  gave it one; it now runs on every PR.
