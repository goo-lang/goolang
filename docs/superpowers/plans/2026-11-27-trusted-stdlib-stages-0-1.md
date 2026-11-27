# Trusted stdlib, stages 0 and 1: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Goo compiles the 22 stage 1 packages from unchanged Go 1.27.4 source, each pinned against its pristine copy and gated by its own upstream tests, and the API count ratchets from 112 toward 644 of 8,433 linux/amd64 symbols.

**Architecture:** A vendoring tool copies the selected files of a package from the local Go 1.27.4 tree into `goostd/<import path>/` and into a pristine copy under `third_party/go1.27.4-src/`, and writes each deviation (a keyword rename, a file swap) into `goostd/VENDOR.tsv`. Gates prove that the vendored tree equals the pristine copy modulo the ledger (T-3, T-8), that each counted symbol has a calling test (T-4), and that upstream tests pass (T-5). Each package is one task; its compiler gaps are fixed as Go-compatibility tasks first.

**Tech Stack:** C23 compiler (`bin/goo`), bash and Python 3 gate scripts, the Go 1.27.4 toolchain at `/usr/local/go` on the developer machine only (CI has no Go).

**Spec:** `docs/superpowers/specs/2026-11-27-trusted-stdlib-design.md` (sections 6, 7, 8 and 11, and the owner's Decisions). The compiler gaps are tasks of `docs/superpowers/plans/2026-11-27-go-compat-wave-1.md` (branch `docs/go-compat-plan`).

## Global Constraints

- Denominator: linux/amd64, 8,433 symbols; report the application subset (3,513) beside it (Decision 1).
- Source: `/usr/local/go/src` (`go version` must print `go1.27.4`); the tool refuses any other version. No download.
- File selection: `GoFiles` with `GOOS=linux GOARCH=amd64` and the `purego` tag; drop every `.s` file; swap an assembly-backed file for its generic twin when one exists; the ledger records each swap (design 8.3).
- Stage 1 has NO trust boundary: no vendored file may import `unsafe`, `reflect`, `runtime`, `sync/atomic` or `internal/...`. The tool refuses such a package.
- Goo keywords in Go source (`match`, `arena`, `enum`, `try`) are renamed at vendor time, scope-aware, with a `name_` suffix, one ledger row each (Decision 4, design 8.2 W1).
- Vendored packages live in nested directories (`goostd/container/list/`), not in the flat alias table.
- Each `.expected.txt` comes from `go run`, never by hand.
- A new gate needs a `--self-test` with real faults, a census line (`LC_ALL=C ./tools/probe_census.sh > tests/probes/census.txt`), a bump in `tools/parity-gate-count.txt` and `python3 tools/gen_probe_targets.py tests/probes/census.txt > tests/probes/generated.bzl`, in the same commit.
- A grammar change uses the goo-grammar skill; `./scripts/grammar-tripwire.sh` must PASS before and after.
- The superset ratchet (`./scripts/superset_probe.sh`) must not fall in any task.
- Both findings baselines give 0 new (host `make findings-probe`, and the CI toolchain in the `ci` container in a private `/w-<task>` directory).

## Review Focus

1. A vendored file that differs from the pristine copy with no ledger row: T-3 must catch it, and each package task must show T-3 green on a byte-exact copy.
2. A symbol counted with no test or golden that calls it (anti-goal A4): T-4 must not count it.
3. A compiler fix for one package that changes the meaning of other valid Go: the superset ratchet and the golden suites must hold, and each fix gets a golden from `go run`.
4. A keyword rename that touches an exported name, a string, a comment or the wrong scope: T-8 runs `go vet` on the renamed file at vendor time and checks each rename row.
5. An upstream test excluded with no reason, or a pass count that falls: T-5 keeps an exclusion ledger with a reason per test, and its count ratchets.

---

### Task S0.1: The `append` prerequisite (in flight)

Done when PR #376 (the safe fix) and the recovery branch `fix/append-alias-sound` (condition 7 sound, replaced arrays freed again, `&L` refusal, daemon block gate) are merged. No new work in this plan.

### Task S0.2: Stage 1 head start (in flight)

Branch `feat/stdlib-stage1` vendors container/list, container/ring, encoding and structs by hand, with goldens. Done when its PR merges. Task S0.4 re-vendors these four through the tool.

### Task S0.3: The metric and gate T-4 `stdlib-api-ratchet`

**Files:** `scripts/stdlib-coverage.py`, `scripts/check_stdlib_coverage.sh`, `docs/stdlib-coverage.json`, new `tests/stdlib/api-floor.txt`, new `scripts/stdlib_api_ratchet.sh`, `Makefile`.

**Interfaces:** Produces `make stdlib-api-ratchet` and `tests/stdlib/api-floor.txt` (one line: `linux-amd64 <count> application <count>`), which each package task raises.

- [ ] Step 1: Write the self-test faults first: delete one golden that calls a counted symbol in a copy, and require FAIL; set the floor one above the count, and require FAIL; the unchanged tree is the control (GREEN).
- [ ] Step 2: Run them: RED, because the gate does not exist.
- [ ] Step 3: `stdlib-coverage.py` counts a symbol only when `check_stdlib_coverage.sh` finds a test or golden that calls it, on the linux/amd64 API files, and prints both counts. The probe fails when either count falls below the floor. It learns nested `goostd/` paths.
- [ ] Step 4: Run the probe and its self-test: GREEN. Commit the floor at today's measured value.
- [ ] Step 5: Wire the gate (census, parity, generated.bzl). Commit.

### Task S0.4: The vendoring tool and gates T-3 `vendor-drift-probe` and T-8 `keyword-rename-probe`

**Files:** new `tools/vendor_go.py`, new `goostd/VENDOR.tsv`, new `third_party/go1.27.4-src/` (selected files only), new `scripts/vendor_drift_probe.sh`, `Makefile`.

**Interfaces:** Produces `python3 tools/vendor_go.py <import path>`, which every package task uses. Ledger columns (tab-separated): `package  file  kind  detail`, where kind is `rename` (detail `old->new@line`), `swap` (detail `asm_file->generic_file`), `drop` (detail `file.s`) or `override` (detail: reason).

- [ ] Step 1: Write the T-3 self-test faults: change one byte of a vendored file with no ledger row (FAIL); a ledger `rename` row with no matching rename (FAIL, the T-8 part); a pristine file missing (FAIL); the unchanged tree (GREEN).
- [ ] Step 2: Run them: RED.
- [ ] Step 3: Implement the tool: check `go version` is go1.27.4; run `go list -json` under `GOOS=linux GOARCH=amd64 -tags purego`; refuse a package whose files import a stage 1 forbidden path; copy the selected files into both trees; rename Goo keywords with Go's own scanner (`go/scanner` through a small Go helper run by the tool) so strings and comments are untouched; run `go vet` on the renamed package; write the ledger rows. The probe (no Go needed) applies the ledger to the pristine copy and compares bytes with the vendored tree.
- [ ] Step 4: Re-vendor container/list, container/ring, encoding and structs through the tool (after S0.2 merges); the vendored bytes must equal the hand copy, or the difference is explained in the commit.
- [ ] Step 5: Run the probe and its self-test: GREEN. Wire the gate. Commit.

### Task S0.5: Gate T-5 `upstream-tests-probe`

**Files:** new `scripts/upstream_tests_probe.sh`, new `tests/stdlib/upstream-tests.tsv` (columns: `package  test  status  reason`), the tool learns to vendor `_test.go` files of the same package (external `package x_test` files only when `goo test` can compile them; see the CLAUDE.md "Known divergence").

- [ ] Step 1: Self-test faults: delete one passing upstream test from a copy (FAIL); an `excluded` row with an empty reason (FAIL); a `pass` row whose test fails (FAIL); control GREEN.
- [ ] Step 2: RED. Step 3: implement: for each vendored package, run `goo test goostd/<path>` with the excluded tests removed by the tool at vendor time (the ledger records each exclusion as `override`), and compare the pass list with the TSV. Step 4: GREEN on container/list and container/ring. Step 5: wire; commit.

### Task S1.n: One package (repeat in this order)

Order (design 11.4, smallest first): cmp, unicode/utf16, image/color, image/color/palette, unicode (measure its compile time first, risk 9: report the `goo build` time and stop if it is above 60 s), then bufio, container/heap, hash, encoding/ascii85, encoding/csv, text/tabwriter, text/scanner, html, math/cmplx, mime/quotedprintable, compress/lzw, go/build/constraint, crypto.

For each package:

- [ ] Step 1: `python3 tools/vendor_go.py <path>`; T-3 GREEN.
- [ ] Step 2: `bin/goo build ./goostd/<path>`: record the first error. If it is a Go-compatibility gap, STOP this task, name the wave 1 task (or a new row for the controller), and return; the controller schedules the gap first.
- [ ] Step 3: A golden fixture `examples/stdlib_<name>.goo` that calls each exported symbol that the package's doc examples use, with `.expected.txt` from `go run`: RED before the package compiles, GREEN after.
- [ ] Step 4: Upstream tests through T-5: add the package's rows; each exclusion has a reason.
- [ ] Step 5: Raise the T-4 floor to the new measured count.
- [ ] Step 6: Gates: T-3, T-4, T-5, test-golden, test-golden-o2, make test, superset (must not fall), density, both findings checks. Commit; push under the shared lock; PR; the controller reviews and merges.

### Compiler gaps (Go-compatibility tasks, scheduled by the controller before the package that needs them)

From design 8.1, in the order of files unblocked: wave 1 Task 10 (case lists, 25 files), Task 11 (qualified composite literal, 15), Task 15 (`[...]T{}`, 14), `&^=` (8, Task 6 ruling 4 — check that #375 covered it), `*p++` on a dereference (4), number literal forms (hex float, `.5`, `_` separators, 4), embedded qualified type in an interface (4), struct tags (3), Task 12 (local type declarations, 2), directional channel types in fields (2), Task 13 (anonymous struct literal, 2), three-index slice (1), Task 9 (`var` with three or more names, 1), grouped `type ( ... )` (1). Type sets, generic types and type aliases are stage 2.
