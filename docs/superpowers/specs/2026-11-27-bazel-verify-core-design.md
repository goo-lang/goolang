# Bazel verify-core: build of record and incremental gates

Status: draft, 2026-11-27. Extends `2026-08-25-bazel-migration-design.md`
(phases 0-7, "the 08-25 design") and the state recorded in
`docs/superpowers/plans/HANDOFF-bazel-migration.md`. Where this document and
the 08-25 design disagree, this one wins; section 3 lists each change.

Companion data: `2026-11-27-bazel-verify-core-gates.tsv`, one row per
verify-core gate (258 rows, swept 2026-11-27 on `chore/superset-ratchet-l3`):
kind, inputs, external tools, silent skip, shared write paths, timing
dependence, Bazel target today, self-test, and the Makefile line. The gate
count moves with the branch (`tools/parity-gate-count.txt` read 260 on
`main` at `6c342ac1`); re-take the sweep before a stage starts rather than
trusting the row count.

## 1. Intent

The owner's words: "plan it properly, we should only be recomputing results
for a specific change."

Today the pre-push hook and CI run `make verify-core`, which runs every gate
serially (the recipe is a prerequisite list, and nothing passes `-j`). A push
that edits one script re-runs all 258 gates, including the cppcheck lane
(~230 s), which runs twice (section 2). A push takes on the order of 20
minutes; that figure is observed, not measured under controlled conditions.

**Success:** a push re-runs exactly the gates whose declared inputs changed,
and every other gate's result is a cache hit — and a cache hit means the same
thing an uncached run would have reported. The second half is the hard part
and most of this document.

**Target (owner, 2026-11-27):** with a warm cache, a push whose change is one
probe script completes `make verify-core` in **under 3 minutes**. The gates
that always run (F16, and the self-building gates of 5.3) set the floor, so
each one either moves into Bazel, is made fast, or moves to nightly. Stage 0
measures today's push time first, in one sitting, so the gain is quoted
against a measured figure and not the observed ~20 minutes.

### Goals

- Bazel is the build of record. `bin/goo` and `lib/libgoo_runtime.a` are
  copies of Bazel outputs, so every gate, Make-run or Bazel-run, tests the
  same binary that ships.
- Each verify-core gate becomes a Bazel test whose result is cached against
  its declared inputs.
- `make lexer` and `make verify-core` remain, as thin wrappers, so muscle
  memory, CLAUDE.md, the hook and CI keep working through the migration.

### Non-goals

- Remote execution, or a remote cache shared between machines.
- Deleting the Makefile. This supersedes phase 7 of the 08-25 design: the
  Makefile stays, reduced to wrappers (section 5.4).
- Rewriting probe logic. Assertions move verbatim; only the harness changes
  (unchanged from the 08-25 design).
- Speeding up any single gate.

### Anti-goals — a failure even if every check is green

1. **A cached PASS that an uncached run would report as FAIL.** A test whose
   declared inputs miss a file it reads (a script, a fixture, a tool on
   PATH) keeps its old verdict after that file changes. This is the worst
   outcome, because it looks exactly like success.
2. **A gate tests a binary other than the one that ships.** True today:
   Bazel tests use the Bazel binary, Make gates use `bin/goo`, and the Bazel
   runtime archive lacks NNG.
3. **A skip counted as a pass.** True today for 15 gates (section 2).
4. **A gate lost in the move.** The 08-25 design's `parity.sh` guards
   names; this design adds a guard on verdicts (section 5.3).

## 2. Facts this design rests on

Each fact names its source. Re-check any of them before acting on it.
The builder check of this document corrected F9, F12 and F14.

| # | Fact | Source |
|---|---|---|
| F1 | verify-core is a prerequisite list with no `-j`; 258 gates run serially | `Makefile` `verify-core:` recipe; hook `.githooks/pre-push` |
| F2 | 124 of the 258 gates have no Bazel target; 37 are SPLIT across several targets | companion TSV, column `bazel_target_today` |
| F3 | Bazel tests use the Bazel-built `//src/compiler:goo`, not `bin/goo` | 08-25 design phase 3; `tools/golden_bazel.sh` |
| F4 | `//src/runtime:runtime_full` has 12 objects, not 13: `far_transport.c` needs NNG; 9 golden fixtures fail under Bazel for that reason | `src/runtime/BUILD:221-228`; handoff "Deliberate divergences" |
| F5 | 15 gates exit 0 when a tool is missing: 11 need valgrind, `obj-header-tsan` (clang), `stdlib-coverage-drift` (go), `arena-rss-probe` and `archive-determinism-probe` (print SKIPPED) | companion TSV, column `silent_skip_if_missing` |
| F6 | Under Bazel, `GOO_PROBE_NO_SKIP` already turns a missing valgrind into a FAIL for the four `arc_*` gates | handoff, task 14 tail |
| F7 | `findings_probe.sh` runs `analyzer_lanes_probe.sh` itself, so under verify-core the three analyser lanes run twice, plus its case (g) cppcheck repeatability control runs the cppcheck lane twice more on the runtime units at -j1 | `scripts/findings_probe.sh:46-47`, `:257-292` |
| F8 | `tools/parity.sh` maps gates to targets by NAME only; it never compares verdicts | `tools/parity.sh` `report()` |
| F9 | `parity.sh` cannot see `test-golden-poison`: that line of `VERIFY_ALL_DEPS` starts with a TAB, `tr ' ' '\n'` keeps the tab, and the `^[a-zA-Z0-9_-]+$` filter drops the entry | `Makefile:3498`; `tools/parity.sh:38-40` `list_make_gates` |
| F10 | `.claude/` is not in `.bazelignore`, so `bazel test //...` descends into `.claude/worktrees/*` | `.bazelignore`; memory note on bare `//...` being red locally |
| F11 | Make compiles with `-Wall -Wextra -Wpedantic -Werror -std=c23 -g -D_GNU_SOURCE -include include/xalloc.h -include include/goo_assert.h`; `.bazelrc` sets only `-std=c23 -Wall -Wextra` globally | `Makefile:29-30`; `.bazelrc` |
| F12 | CI compiles with gcc-14 (`make CC=gcc-14`; `bazel.yml` passes `--repo_env=CC=/usr/bin/gcc-14`); the runner's `/usr/bin/gcc` is gcc-13, which rejects `-std=c23`. The workstation pin is `/usr/bin/gcc` | `tests.yml:133`, `bazel.yml:66`, `.bazelrc` |
| F13 | CI's Bazel job already has its own disk cache (`setup-bazel`, `disk-cache: ${{ github.workflow }}`) | `bazel.yml:58-60` |
| F14 | 23 scripts invoke `make` themselves (rebuilds with mutated sources, `WERROR=`, scratch trees). A plain `grep -l 'make '` reports 49, because it also matches comments | non-comment lines of `scripts/*.sh tools/*.sh` matching a `make` command word |
| F15 | 51 gates have a timing dependence (timeouts, thread scheduling, valgrind slowdown) | companion TSV, column `timing_dependence` |
| F16 | 7 gates cannot run inside a sandbox at all and stay out of Bazel by design | handoff, task 14 |
| F17 | `tests.yml` and the pre-push hook have no Bazel setup today; only `bazel.yml` installs Bazel | `.github/workflows/tests.yml`, `.githooks/pre-push` |
| F18 | Several `arc_*` gates call `git` only to find the repository root (`git rev-parse --show-toplevel`) | `scripts/arc_uaf_probe.sh:18`, `scripts/arc_reassign_probe.sh:59` |

Line numbers are as of `main` at `6c342ac1`; they drift.

## 3. Decisions

### D5. Bazel is the build of record (owner, 2026-11-27)

`bazel build //src/compiler:goo //src/runtime:goo_runtime_archive` produces
the shipped artefacts. `make lexer` runs that build and copies the two
outputs to `bin/goo` and `lib/libgoo_runtime.a`. Changes the 08-25 design's
D1: the Makefile is not deleted; it ends as wrappers.

Consequence, stated so nobody discovers it later: **Bazel (through
bazelisk) becomes a hard requirement** for `make lexer` and `make
verify-core` on every machine, including the `tests.yml` runner and the
pre-push hook (F17). Stage 1 adds Bazel setup to `tests.yml` and documents
bazelisk in the tool list. *This document's call:* the Make compile rules
stay, under a non-default target, until stage 3, for two reasons: the
self-building gates (5.3) still use them, and the flip PR must be revertible
on its own. They are deleted in stage 3 once no gate uses them.

### D6. Cache scope (owner, 2026-11-27)

- One local `--disk_cache` shared by every worktree on the workstation.
- CI keeps its own cache (F13), unchanged.
- A nightly job runs the Bazel gates with `--nocache_test_results` and
  compares each verdict with the cached verdict for the same commit. A
  mismatch is a red nightly.

### D8. A missing tool fails the gate (owner, 2026-11-27)

See H1. verify-core requires its tools; a gate never passes having tested
nothing.

### D9. NNG through a hand-written BUILD overlay (owner, 2026-11-27)

See 5.1.

### D7. Migration sequence: C (owner, 2026-11-27)

Three sequences were considered.

- **A. Build first, then gate families.** Flip the build (D5), then move one
  gate family per PR, each with a verdict-parity row. From the first PR one
  binary exists, so every parity row compares results for one artefact. Cost:
  the flip PR is large, and any flag difference (F11, F12) shows in every
  gate at once.
- **B. Gates first, build last.** Small PRs and an early cache gain. Until
  the last PR, Bazel tests and Make gates test two different binaries (F3,
  F4), so a parity row compares two artefacts and proves nothing about
  either. Rejected: it fails anti-goal 2 for the whole migration.
- **C. Four hygiene PRs, then A.** **Chosen.** Each hygiene defect (F5, F7,
  F9, F10) can make a parity row agree when it should not — a gate that
  exits 0 without valgrind agrees with its Bazel copy while neither tested
  anything. The fixes are small and needed under any sequence, and F7 alone
  removes several minutes from every push today.

## 4. Stage 0 — hygiene (four PRs, any order)

Each PR is independent, carries its own test, and ships before stage 1.

**H1. A missing tool is not a pass (F5).** Each of the 15 gates either FAILs
when its tool is missing, or reports a distinct SKIP status that the
verify-core recipe counts and refuses. Extend the existing
`GOO_PROBE_NO_SKIP` rule (F6) to every Make recipe of the 15 and set it in
the `verify-core` recipe. **Owner's decision (D8): FAIL, not a counted
SKIP.** verify-core then requires valgrind, clang, go and bazelisk on any
machine that runs it; CLAUDE.md's "safe for pre-push on any machine" becomes
"any machine with the documented tools". The tool list goes into CLAUDE.md in the
same PR. Self-test: run each of the 15 with the tool scrubbed from PATH and
assert non-zero exit (scrub PATH inside the self-test — never read the
machine's real PATH; see memory note on self-test controls).

**H2. The analyser lanes run once (F7).** `findings-probe` consumes the SARIF
that `analyzer-lanes-probe` already wrote, instead of re-running the lanes.
In Make this is a prerequisite plus a freshness check (refuse SARIF older
than the sources it covers, or that carries a different tool signature). The
case (g) repeatability control stays — it is a measurement, not a duplicate —
but runs only on the runtime units it already scopes. Keep a positive
control: delete one SARIF file, and the probe must FAIL rather than pass on
nothing (memory note: a build cache can empty an analyser gate). Measure the
verify-core wall time before and after in one sitting, and quote both.

**H3. `parity.sh` reads any whitespace (F9).** Split on `[:space:]`, not
`' '`. Add a self-test case: a fixture Makefile whose `VERIFY_ALL_DEPS`
line starts with a tab must count that gate. Expect the gate count to move
by one; bump `tools/parity-gate-count.txt` in the same commit.

**H4. `.claude/` in `.bazelignore` (F10).** One line. Proof: create a
`BUILD` file under a scratch `.claude/worktrees/x/` and show `bazel query
//...` does not list it.

## 5. Stage 1 — the build flip (D5)

### 5.1 NNG in Bazel (closes F4; the 08-25 design's phase 3c)

The runtime archive needs `far_transport.c` and NNG 1.12.0 from the pinned
tarball `third_party/nng-1.12.0.tar.gz` (sha256 in `Makefile:154`). Two
ways:

- **rules_foreign_cc `cmake()`** over the tarball. Runs NNG's own CMake with
  the same `-D` options the Makefile passes (`Makefile:180-186`), so the
  configuration cannot drift from upstream's. Cost: a new module dependency
  (download from the Bazel Central Registry — the owner approves the
  download first), and a CMake run inside an action, which is slower on a
  cold cache and harder to make deterministic.
- **A hand-written `cc_library` overlay** on the extracted tarball (an
  `http_archive`-style local repository with `build_file`). No new
  dependency, fully visible in a BUILD file, and Bazel-native caching. Cost:
  the BUILD file must list NNG's sources and reproduce the defines CMake
  derives (platform, transports), and it must be redone on every NNG bump.

**Owner's decision (D9): the overlay**, because the project already avoids
toolchain-level dependencies (08-25 design D2 rationale) and NNG changes
rarely. Mitigate the drift risk with a check that compares the overlay's
object list and defines against CMake's (`compile_commands.json` from a
one-off CMake run) on a version bump. If the defines prove unmanageable,
stop and bring `cmake()` back to the owner; do not switch silently.

Exit criterion: `tools/golden_bazel.sh` asserts 495/0 instead of 486/9
(numbers from the handoff; re-take them, the fixture count has moved).

### 5.2 Flag parity (F11, F12)

The Bazel compile must match the Make compile before the flip, or the flip
changes two variables at once (the 08-25 design's own rule for choosing
gcc). Specifically:

- Add `-Wpedantic` and `-Werror` to the Bazel build of `//src/...`, with a
  way to drop `-Werror` that mirrors `WERROR=` (a `--config` or a
  `bool_flag`), because three gate families deliberately build with
  warnings (CLAUDE.md, "WERROR").
- Add `-g`, `-D_GNU_SOURCE` and the two `-include` preludes where the 08-25
  design's prelude mechanism (its section 4.1) does not already supply them.
  Check, do not assume.
- `--compilation_mode`: Make builds unoptimised with `-g`. Choose the Bazel
  mode that produces the same flags and record the evidence.
- The compiler: `make lexer CC=gcc-14` must reach Bazel as
  `--repo_env=CC=/usr/bin/gcc-14`. The wrapper translates `CC`; an
  unrecognised `CC` value is an error, not a silent default.

Proof of parity: `tools/compiler_differential.sh` (both compilers emit
identical IR) passes over the golden corpus, and a byte comparison of the
`-###`/command lines for one translation unit under each build is recorded.

### 5.3 The flip

`make lexer` becomes: `bazel build <the two targets>`, then copy both
outputs to `bin/` and `lib/` (copy, not symlink — a symlink into
`bazel-out` changes under the next build and breaks any gate that holds the
path across a rebuild). `$(COMPILER)` and `$(RUNTIME_LIB)` become those
copies, so every unchanged Make gate now tests the Bazel binary.

Gates that rebuild the compiler on purpose (F14: mutation teeth,
`warning-count-probe`, `repro-build-probe`, `archive-determinism-probe`) are
not covered by the flip. Each one keeps its
own build, which must be stated in the gate (it measures A build, not THE
build), or moves to building through Bazel. List each with its choice in the
stage 1 PR. No such gate may silently start testing a different compile than
before.

Exit criteria for stage 1:

1. `make verify-core` green with `bin/goo` from Bazel.
2. `golden_bazel.sh` at N/0.
3. `compiler_differential.sh` green.
4. CI green on `tests.yml` and `bazel.yml`.

## 6. Stage 2 — gate families

### 6.1 What moves

A family is a set of gates that share a harness (the companion TSV's `kind`
column is the starting partition: golden fixtures, generated-source probes,
example probes, self-tests, unit binaries, analyser lanes, meta-gates, and
the rest). One family per PR.

Sequence (this design's call, correctable by the owner):

1. A family that already has Bazel targets, to prove the verdict-parity
   mechanism on a small set.
2. The golden suites and the analyser lanes, which cost the most serial
   time. *This document's call:* the analyser lanes move to Bazel, because
   leaving the ~230 s cppcheck lane always-run defeats the goal. Each lane
   becomes a build action (per translation unit where the tool allows it)
   under the no-`-Werror` mode of 5.2, writing SARIF as its output. Both
   `analyzer-lanes-probe` and `findings-probe` take that output as `data`,
   so the lane runs once per change by construction, which is H2's fix in
   Bazel form. The lanes compile sources with analyser flags; they do not
   build the shipped compiler, so they are not self-building gates in the
   sense of 5.3.
3. The rest, ordered by wall time saved per unit of work.

### 6.2 verify-core during the migration

`make verify-core` = `bazel test //:verify_core` (a `test_suite` of the
migrated families) followed by the Make prerequisite list of the families
not yet migrated. Each family PR moves that family from the second part to
the first. Both halves must pass; the recipe never reads an exit status
through a pipe.

### 6.3 The verdict-parity row

`parity.sh` proves a gate has a target with a matching NAME (F8). That does
not prove the target checks the same thing. Each family PR adds, for every
gate it moves, a row to a committed ledger
(`tools/parity-verdicts.tsv`, a new file): gate, target, Make verdict,
Bazel verdict, red-control verdict, commit. The red control is a deliberate
defect for that family (the 08-25 design's teeth fixtures are the model)
that must turn BOTH the Make gate and the Bazel target red. A row with a
green red-control is refused. The ledger is checked by a small script with
its own `--self-test`, which joins `parity.sh`'s list of the gates that "only
compare two build systems" and goes away with the Make half.

### 6.4 Declared inputs and the cache (anti-goal 1)

A Bazel test result is cached against its declared inputs. Three classes of
input are not declared today and must be before a gate moves:

- **Files the script reads.** Every fixture, helper script and baseline is
  a `data` dependency. The sandbox enforces this for files under the
  workspace (a missing runfile fails loudly).
- **Tools on PATH** (valgrind, cppcheck, clang-tidy, python3, spin, go,
  llvm-config, gcc). A tool upgrade does not change any input, so a stale
  verdict survives it. A repository rule records each tool's version string
  into a generated file (the same pattern as the LLVM version assertion,
  08-25 design D2), and each gate that uses the tool depends on that file. A
  version change then invalidates exactly the gates that use the tool.
- **Repository state.** A sandboxed test has no `.git`. A gate that uses
  `git` only to find the repository root (F18) switches to the runfiles
  root, which is already `$PWD` on entry (handoff, task 14). A gate that
  needs history or a diff is tagged `no-cache` and counted.
- **Machine state** that cannot be declared (CPU count for scheduling
  tests, a running podman). Such a gate is tagged `no-cache` (or `external`)
  and runs every time. The count of such tags is reported and kept small.

Timing-dependent gates (F15) are cache-safe only if their verdict does not
depend on load. A gate that flakes under `bazel test`'s parallelism is fixed
or tagged `exclusive`, never retried until green.

### 6.5 Shared write paths

Gates that write a fixed path (`/tmp/census.diff`, `build/oot`, the repo
root; companion TSV column `writes_shared_path`) collide under parallel
execution and escape the sandbox. Each one moves to `$TEST_TMPDIR` as part of
its family's PR.

## 7. Stage 3 — end state

- `make lexer` = Bazel build + copy (section 5.3).
- `make verify-core` = `bazel test //:verify_core --config=local`, plus the
  allowlisted gates that cannot run in a sandbox (F16) and the gates that
  build their own compiler on purpose (section 5.3), run by Make.
- `parity.sh` exits 0, with every exception in `tools/parity-allowlist.txt`
  with a reason (the 08-25 design's rule).
- The "compare two build systems" tools (handoff list) are deleted, except
  those still needed by the gates Make keeps.

## 8. Cache configuration (D6)

- `.bazelrc` gets `common:local --disk_cache=~/.cache/goolang-bazel-disk`
  and a size bound. The garbage-collection flag name differs between Bazel
  releases; confirm it on 9.2.0 (`.bazelversion`) before writing it. The
  hook and `make verify-core` pass `--config=local`; CI does not, so CI's
  cache (F13) is unchanged.
- Cold cache. The disk cache is shared, so a new worktree starts warm.
  *This document's call:* the size bound holds at least two full sets of
  verify-core outputs, so one branch switch does not evict the other
  branch's results; measure one full set and set the bound from it. CI's
  cache can be cold; a cold CI run is a full run, but parallel, and there
  is no fallback to Make.
- Each worktree has its own Bazel server and output base. With many
  worktrees that is many resident servers. Set `--max_idle_secs` in
  `common:local` so idle servers exit; measure resident memory with five
  worktrees before and after.
- Nightly (`nightly.yml`): job 1 runs `bazel test //:verify_core` with the
  CI cache; job 2 runs it with `--nocache_test_results` on the same commit;
  a comparison step diffs the per-target verdicts (from the test logs or
  the BEP JSON) and fails on any difference. The comparison has its own
  self-test: a fixture pair that differs in one verdict must fail, and an
  empty verdict list must fail (memory note: a differential harness must
  check its producers).

## 9. Risks

| Risk | Mitigation |
|---|---|
| Stale cached PASS (anti-goal 1) | Declared inputs (6.4), tool-version stamps, nightly uncached comparison (8) |
| Flip changes behaviour through a flag difference | Flag parity first (5.2), `compiler_differential.sh`, golden N/0 |
| NNG overlay drifts from upstream CMake | Object-list check on version bump (5.1); fall back to `cmake()` |
| A family's Bazel target asserts less than its Make gate | Verdict-parity row with a red control (6.3) |
| H1 makes verify-core fail on a machine without valgrind | Intended; documented tool list; owner confirms (4) |
| Parallel execution exposes flaky timing gates | Fix or `exclusive`; never retry-to-green (6.4) |
| Many Bazel servers exhaust memory | `--max_idle_secs`, measured (8) |
| A gate that calls `make` internally recurses into the new wrapper | Audit the 30 scripts (F14) in stage 1 |

## 10. Left to the builder

- The exact family partition and its order after step 2 of 6.1.
- The verdict ledger's file format and script.
- Which `--compilation_mode` reproduces Make's flags.
- The set of tools that get version stamps, from the companion TSV's
  `external_tools` column.
- Per-gate choice for the self-building gates (5.3).

## 11. Calls made in this document, not by the owner

- Copy, not symlink, into `bin/` and `lib/` (5.3).
- The family sequence (6.1).
- A new verdict ledger rather than extending `parity.sh` (6.3).
- `--max_idle_secs` for idle servers (8).
- Bazel becomes a hard requirement; Make compile rules kept until stage 3 (D5).
- The analyser lanes move to Bazel as build actions (6.1).
- Repository state: runfiles root, else `no-cache` (6.4).
- Disk cache bound of two full output sets; no Make fallback on a cold CI cache (8).
