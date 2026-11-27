# External readiness: a repo that IBM Quantum can try and partner on

Date: 2026-11-27
Status: design, for owner approval.

## Intent

The owner will show the Goo repository to IBM Quantum and other parties in a
few weeks. After a look at the repo, the owner wants such a reader to do two
things:

1. **Try it.** Install Goo and run a quantum example on a laptop, with no IBM
   account, in a few minutes.
2. **Partner on QPU work.** See a quantum story that is true, measured and
   honest about its gaps, so that a partnership on the QPU work is a
   reasonable next step.

The main risk is credibility, not clutter. An IBM engineer reads the quantum
claims first and can test them in minutes. A claim that is false or that has
no gate costs more trust than any amount of clutter.

## Goals

- G1. Every claim in the README and in the new quantum page maps to a gate in
  `make verify-core`, or carries the label "external, not gated" (Rule 0, the
  owner's decision of 2026-11-25).
- G2. The quantum path gives no silent wrong result that we know of.
- G3. A reader installs a released Goo with one documented command and runs a
  quantum example locally.
- G4. The root of the repo and `docs/` show what a reader needs, and the
  internal working notes leave git.
- G5. One page, `docs/quantum.md`, tells a partner reader what Goo does for
  QPU work, what is not done, the QPU project 1 design, and what a
  partnership could add.

## Non-goals

- QPU project 1 itself (submission with no Python). It has its own design
  (draft PR, branch `docs/qpu-project-1`) and follows the Go-compatibility
  wave. The quantum page links to it as the roadmap.
- New quantum features.
- Go-compatibility work. It has its own plan (wave 1). The dropped `@` token is
  in that plan.
- A pitch deck or an email to IBM. Those are outside the repo. The owner can
  ask for them separately.
- A package manager. ADR 0006 puts it after the release pipeline.

## Anti-goals (each one is a failure even if the work is otherwise done)

- A claim of "verified" or "works" with no gate.
- A live QPU result shown as gated. Live job IDs carry "external, not gated".
- A release whose README or release notes still carry a false claim.
- A tidy step that breaks a gate or a link: a moved file that a gate, a script,
  `CLAUDE.md` or an ADR still names by its old path.
- The loss of any history: files leave the tree, not the git history.
- Hiding the AI help. The commit history keeps its co-author lines.

## Constraints

- The owner's binding decisions of 2026-11-25 (superset rule, Rule 0, no
  install of libqrmi, qiskit, qirrunner, verilator or circt and no live quota
  job without fresh approval).
- `main` is protected: every change is a PR with 8 required checks, merged on
  green.
- Every PR keeps `make verify-core` green, and both findings baselines.
- A new gate needs its `--self-test`, its census line and the parity count in
  the same commit.
- Option B for the AI-workflow files (the owner's choice, 2026-11-27): keep
  `CLAUDE.md` in git. Remove `.handoff.md`, `.superpowers/`, `.coord/`,
  `.taskmaster/` and `.claude/` from git, keep them on the owner's machine, and
  add them to `.gitignore`.

## Approaches

| Approach | Order | Trade-off |
|---|---|---|
| **1. Credibility and tidy in parallel, then the release (chosen)** | Parts 1 and 3 at the same time, then part 2 | Fastest. The release carries true claims. A conflict is possible only in the README, so part 3 does not touch it. |
| 2. Everything in sequence | 1, then 3, then 2 | Simplest to review. About a week slower. |
| 3. Release first | 2, then 1 and 3 | The longest part starts first, but the first release would carry today's false claims. |

The owner chose approach 1, with the quantum page, on 2026-11-27.

## Part 1: credibility

### 1a. Two silent wrong results in the quantum path (found 2026-11-27)

- `--emit-qasm3` writes `rz(0.0)` for a constant angle that is not a bare
  literal, and exits 0 (QPU design, probe w02). The fix: compute the constant
  angle at compile time, or refuse it with a positioned diagnostic. A silent
  `0.0` is never correct. A golden fixture covers a named constant angle and a
  constant expression.
- `src/runtime/qir_sim.c` treats qubit 16 as qubit 0 (probe w01). The
  simulator has 16 qubits. The fix: refuse an index of 16 or more with a clear
  runtime error and a non-zero exit, and, if the type checker can see a
  constant index, refuse it at compile time. A fixture covers index 16.
- Each fix adds its fixture to the README claim that it supports.

### 1b. The README and the claims

The README (352 lines) is the first page that a reader sees. The rewrite:

- Each row of the feature table names its gate (the Make target or the fixture
  family). A feature with no gate leaves the table or moves to a "not yet
  verified" list.
- The live QPU table keeps its rows, with the label "external, not gated", and
  says that Qiskit transpiled each circuit before submission. Rows that share
  one job ID say that it was one batched job.
- The superset claim uses the measured numbers of `superset-probe` (L1, L2 and
  L3 out of 794), not "Go-compatible".
- The coord task `goolang:Doc-readme-truth` gives these items: drop
  `lanes.Map`, fix "Law 5" for `[N]qir.Qubit`, the superset definition with
  the probe numbers, and label live QPU jobs "external, not gated". This part
  does all of them and closes that task.
- Controller call: a small gate, `readme-claims-probe`, checks that each row of
  the README feature table names a Make target or a fixture that exists. Rule 0
  asks for this. It needs its self-test, census line and parity count.
- `docs/GO_SPEC_CONFORMANCE.md` shows a snapshot from 2026-07-27. It must show
  the numbers that `make spec-conformance` gives today.

## Part 2: try it

### 2a. The first release

**The owner decided (2026-11-27): `v0.1.0` is a GitHub Release.** It has no
Cloudflare R2, no `dist.goolang.org` and no minisign signature yet. Those, and
the `goop` installer, come in a later release, as ADR 0006 sub-project A
describes. From that spec, take the tarball layout
(`docs/superpowers/specs/2026-08-20-toolchain-distribution-design.md`, "The
layout" and "What ships in a tarball"), the version injection, and the gate.

The release workflow, on a tag push: build in the reproducible podman image that
`repro-build` already proves, make the tarball and a `SHA256SUMS` file, attest
the build provenance with `actions/attest-build-provenance`, and attach all of
them to a GitHub Release. The README gives the install command (download,
`sha256sum -c`, untar, put `bin/` on `PATH`) and the `gh attestation verify`
command.

The first run is a pre-release tag, `v0.1.0-rc.1`, so that the first real run
of the pipeline is not the public `v0.1.0`. The release workflow then tests the
published files: it downloads them, checks the sums, and runs the quickstart.

Platforms (the owner decided, 2026-11-27): Linux x86_64 natively. On macOS,
the quickstart runs in the pinned container with one `podman run` or
`docker run` command, which the quickstart gives. A native macOS build needs a
compiler change (the exe-directory lookup is inside `#ifdef __linux__`) and is
later work. The release notes say so. The toolchain needs a system C compiler,
and the quickstart lists it as a prerequisite.

### 2b. A quantum quickstart

A page section (in the README and in `docs/quantum.md`) that a reader can
follow on a laptop with no IBM account:

1. Install the release.
2. Compile `examples/qir_teleport_probe.goo` and run it on the embedded
   simulator.
3. Show the compile-time ownership check: an existing reject fixture for the
   linear-qubit rules (for example the `qir_discard` family in
   `tests/diag_format/help/`, or a `tests/golden/reject/` fixture), with its
   exact diagnostic. Reuse a fixture that a gate already pins. Do not write a
   new one for the README.
4. Emit OpenQASM 3 and QIR from the same program.

Each step is a command with its expected output. A gate in `verify-core` runs
the same commands on a tarball that it builds locally (a `quickstart-probe`
with its self-test). The release workflow runs them again on the published
files after each tag.

## Part 3: tidy

### 3a. The root

- Remove from the tree: the nine `test_*.c` files at the root,
  `performance_report.txt` and `demo-project/`. No Make target, script, Bazel
  file or workflow names them (checked on 2026-11-27).
- `ide/`, `kernel/`, `templates/` and `testing/` have references (104, 5, 1 and
  22 hits in the Makefile, scripts, Bazel files and workflows). The builder
  reads each reference. A directory that only dead targets use moves to
  `attic/` with those targets. A directory that a live target uses stays.
- Option B for the AI-workflow files (see Constraints). Controller call:
  `.claude/skills/` STAYS in git, because the `goo-grammar` skill is project
  tooling that `CLAUDE.md` and `scripts/grammar-tripwire.sh` name. Only the
  other `.claude/` content leaves git.
- Controller call: `CLAUDE.md` stays, with one cut: the task-master command
  reference, which its own status note calls out of date.

### 3b. `docs/`

- A reader needs: the vision, the language specification, the conformance
  matrix, the ADRs, the quantum page, and the build and verification guides.
- Internal history: the dated roadmaps and plans in `docs/` root, the
  `M7_*` and `M10_*` audits and spikes, and similar. Move them to
  `docs/history/`, with a short `README.md` there that says what they are.
- Before any move, search for each old path in the whole tree, `CLAUDE.md`
  included, and update each reference in the same commit. A link-check step
  proves that no Markdown link in the repo points at a missing file. It checks
  only links inside the repo. Links to URLs are out of its scope.
- Add a `docs/README.md` index for a new reader.

### 3c. Community files

- `CONTRIBUTING.md`: how to build, how to run `make verify-core`, the PR rule
  (8 required checks), the commit style, and the TDD rule.
- `SECURITY.md` is already in PR #358.
- GitHub repository description and topics (the owner sets these, or approves
  the text).

## Part 4: the quantum page, `docs/quantum.md`

For a partner reader. Sections:

1. What Goo gives QPU work: compile-time linear qubits (the five ownership
   rules), one language for the circuit and the classical decoder with no
   garbage-collector pauses, and QIR and OpenQASM 3 output.
2. What is measured, each item with its gate.
3. What ran on real hardware, labelled "external, not gated", with the
   statement that Qiskit transpiled each circuit.
4. What is not done: no native submission yet, no routing, a 16-qubit
   simulator, and the known limits.
5. The roadmap: QPU project 1 (scope A), with a link to its design. The design
   is a draft PR under review, so the page links to that PR.
6. What a partnership could add: API and backend access for a pinned backend
   description, review of the native-gate translation, recorded responses for
   offline gates, and a backend to test against.

## Left to the builder

- The exact form of the `rz` fix (compute or refuse), within "never silent".
- The link-check tool (a short script is enough).
- The split of each part into PRs. Small PRs are preferred.
- Whether a directory in 3a moves or stays, by the rule given there.

## Decisions recorded

- 2026-11-27, owner: approach 1 with the quantum page; option B for the
  AI-workflow files; `v0.1.0` as a GitHub Release with no R2, domain or
  minisign; Linux natively and the container on macOS.
- Controller calls (marked in the text): `.claude/skills/` stays in git; the
  task-master cut in `CLAUDE.md`; reuse of an existing ownership fixture; the
  link to the draft PR; the `readme-claims-probe` gate; the quickstart gate on a
  local tarball and on the published files.

## Open items for the owner

- The repository description and topics text (the builder proposes it).
