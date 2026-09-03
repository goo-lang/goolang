# Bazel migration — handoff, 2026-08-25

## State

| Phase | PR | Status |
|---|---|---|
| 0 + 1 foundation, LLVM-free leaf | #321 | merged |
| 2 front end | #322 | merged |
| 3 codegen + compiler | #323 | merged |
| 3b runtime archive (no NNG) | #324 | merged |
| 4 probe gates, tasks 1–5 of 17 | **#325** | **open, green, CLEAN** |
| 4 probe gates, tasks 6–7 (the two derivation gates) | #330 | merged |
| 4 probe gates, tasks 8–12 (extracted sources, drift gate) | #331 | merged |
| 4 probe gates, task 13 (printf targets) | #332 | merged |
| 4 probe gates, task 14 in part, 6 of 29 | **#333** | **merged** |
| 4 probe gates, task 14 in part, the four arc_* gates, 10 of 29 | #334 | merged |
| 4 probe gates, task 14 in part, the last three sandbox failures and string_literal_header, 14 of 29 | #335 | merged |
| 4 probe gates, task 14 in part, the three C-fixture gates, 17 of 29 | #336 | merged |
| 4 probe gates, task 14 in part, the five src/** gates, 22 of 29 | #337 | merged |

**Parity: 224 gates** as of #331 (was 217). `make verify-core` and
`bazel test //...` are both green. `bazel test //...` runs 181 tests
(measured 2026-09-03). `verify-core` now carries three DERIVATION gates —
census, generated.bzl and the extracted sources — which together cost about
110 s, most of it the source-drift self-test.

## Pick up here

`docs/superpowers/plans/2026-08-25-bazel-phase4-probes.md`, **task 15
onward**. Tasks 1–13 and the two added ones (4b, 4c) are done. Task 14 is
a PARTIAL pass by design — 22 of 29 script gates operate, and one bucket
of 7 remains, recorded with a measured cause each rather than declared red
or tagged `manual`. WARNING: the 1–5 checkboxes in that file were never
ticked, though every artefact is present — do not read an empty box there
as open work. 6 to 14 are ticked or `[~]`, with evidence.

**Task 13 is done. Task 14 is done to the extent it will ever be — 7 of 29
stay out by design, permanently. Next is task 15** (the bespoke EIGHT, up
from six — the derivation gates classify there, correctly: they are
hand-written sh_tests), 16–17 (CI, close).

**Task 14's tail — the five src/**-scanning gates — is now closed.** The
four `arc_*` gates were done earlier: `$0` and `${BASH_SOURCE[0]}` resolve
to a symlink under `tests/probes/`, one directory too deep for their old
`dirname` fallback, so the fix is `$PWD` — already the runfiles root on
entry — and `GOO_PROBE_NO_SKIP` turns a missing valgrind into a FAIL under
Bazel rather than a SKIPPED line nobody reads. Ten per-package filegroups
under `src/`, one in `include/` and one in `src/parser/gen/` unlocked the
last five: a glob does not cross a package boundary, so there cannot be a
single `//:c_sources`. Full measured causes, RED lines and the mechanism
(the twelve filegroups, the macro's new per-target `env`, `PARSER_TAB_C`,
the `CC_PROBE` fallback, the no-skip rule, the `package_toolchain.sh`
two-line override, and three further symlink-related fixes the original
plan did not anticipate) are in the phase-4 plan's task 14 entry. The
remaining 7 stay out permanently, by design — none of them can run inside
a sandbox at all.

**Task 13 is done. #332 delivered 136 targets and 100 refusals**, and the
refusal count is the deliverable there, not a shortfall.

**How task 13 was scoped, kept for the next re-scope:** It is not 69
targets. 159 sources are committed under `tests/probes/src/<gate>/`, each
needing its own assertions, and the recipes interleave those assertions per
source. Some cannot be expressed at all: `asi-hardening-probe` asserts *"if exit
is 0 AND stdout is 5, FAIL"*, a negative compound condition `goo_expect_probe`
has no form for. **Expect the generator to refuse a large fraction, and treat
the refusal count as the deliverable.** Its refusals are what found three wrong
parse rules and a missing category the first time.

**Three gates were found never to have run, in three sessions.**
`test-golden-poison`, then `census_current`, then `verify_probe_teeth.sh`. The
last is the worst of the three: `testing/teeth/BUILD` calls it the most
important check in the phase, because one macro generates ~150 targets and if
it cannot fail they all pass while asserting nothing. Lessons:

- **A `manual` tag is not a wiring plan.** If you tag a gate `manual` for nested
  bazel, add its runner in the SAME commit — a make target if it needs no
  bazel, a CI step if it does. `verify_probe_teeth.sh` drives bazel, so it is a
  step in the bazel job; `census_current` needs none, so it is a make gate.
- **`probe-teeth-probe` scans `scripts/*.sh` ONLY.** Measured 2026-08-30: 14
  shell files sit outside that glob (12 in `tools/`, 2 in `tests/probes/`) and
  **12 of them have no `--self-test`**, including the gates `verify_layering.sh`,
  `golden_bazel.sh` and `compiler_differential.sh`. The scan never considers
  them, so their absence of teeth was never a decision. Widening it is unclaimed
  work, and it will go red until those files get teeth or a baseline entry.
- **Read `git log --stat` before pushing generated output.** The first
  extraction faithfully collected `comment_lines_probe.goo` (2 MB) and
  `blank_lines_probe.goo` (1 MB), both `yes '' | head -n 1000000`. Every gate
  passed — the files matched the derivation exactly. They are REFUSED now, with
  the reason printed and `EXPECTED_REFUSED` asserted.

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
