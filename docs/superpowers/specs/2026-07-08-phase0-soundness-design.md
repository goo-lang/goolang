# Phase 0 — Soundness & Safety Net (design)

**Date:** 2026-07-08
**Source of requirements:** `docs/2026-07-08-v1-roadmap.md`, Phase 0 (tasks P0.1–P0.10, all
verified findings from workflow run wf_4fc64c40-b4f). This spec is normative where the two
differ in wording; the roadmap's acceptance criteria are reproduced here verbatim-in-substance.
**Branch:** `feat/phase0-soundness`, base `ce78952` (main after PR #162).
**Baseline gates at base:** golden 321/0 · comptime-value-reject-matrix 18/18 ·
comptime-generic-compose-ir-pin PASS · unit 76/1-skip · grammar-tripwire 82 S/R + 256 R/R
exact · spmd-bench-probe PASS.

## What "Phase 0" means (scope decision)

The v1 roadmap's keystone finding: the compiler's *capability* is well ahead of its
*trustworthiness*. Two proven silent miscompiles, a zero-diagnostic compiler crash, a lexer
stack overflow, non-conformant panic exits, and a golden harness that cannot see timeouts or
exit codes mean nothing downstream can be believed until this arc lands. Phase 0 delivers
exactly that trust: six soundness fixes, each pinned by a regression fixture in a hardened
data-driven harness, plus a ccomp-free aggregate gate. No new language surface. No feature
work. A compiler bug discovered beyond the six named fixes is BLOCKED — report it, do not
fix it here.

## Layer 1 — Harness hardening (built first; no compiler changes)

Built first so every soundness fix pins its regression as a *native* fixture — no interim
Makefile probes, no double-touching.

- **P0.7 — Golden runner: per-fixture timeout + exit-code capture.** Each fixture in
  `scripts/run_golden.sh` runs under `timeout` (default 10s, `GOLDEN_TIMEOUT` env override).
  A deliberately looping fixture FAILs within the limit. A nonzero binary exit marks FAIL
  even when stdout matches — unless the fixture declares an expected exit (P0.8). All 321
  existing fixtures still pass.
- **P0.8 — Expected exit code and expected stderr conventions.** `<name>.exit` (single
  integer) and `<name>.stderr.txt` (substring match) honored per fixture. At least 3 bespoke
  runtime-abort Makefile probes (divzero, bounds, typeassert-abort) re-expressed as golden
  fixtures and their Makefile targets deleted. `make verify` stays green.
- **P0.9 — Data-driven compile-reject harness (`test-golden-reject`).**
  `tests/golden/reject/<name>.goo` + `<name>.err.txt` convention; the runner asserts nonzero
  compiler exit, no binary emitted, and stderr contains the substring. Joins `verify`.
  5 existing `*-reject-probe` Makefile targets migrated as pilots and removed.

## Layer 2 — Soundness fixes (each with its audit reproducer as the RED test)

Ordering constraint: **P0.2 lands before P0.3**, so the `!?int` SIGILL reproducer proves the
verify gate converts a signal crash into a clean diagnostic before the type checker learns
to reject the construct earlier.

- **P0.2 — LLVMVerifyModule gate before executable emission.** `codegen_emit_executable`
  (the `codegen.c:1592` path) runs `LLVMVerifyModule` before `LLVMTargetMachineEmitToFile`.
  Any verifier-visible invalid IR produces a printed compiler-bug diagnostic and exit 1
  instead of a signal.
  **Premise refuted during implementation (recorded 2026-07-08):** the `!?int` module
  passes `LLVMVerifyModule` clean — the struct-constant mismatch is invisible to the
  verifier and only crashes later in SelectionDAG — so the gate cannot be proven with that
  reproducer; the SIGILL class is closed by P0.3's type-checker rejection instead. A
  pre-existing verify at `codegen_emit_executable`'s entry (audit missed it) already
  rejects verifier-visible IR (empirically proven with `math.Sqrt("x")`: exit 1,
  diagnostic, no binary). The pre-emission gate ships as a defensive backstop against
  future refactors, documented as such in-code.
- **P0.3 — Reject composed `!?T` in the type checker.** `func f() !?int` (any `!?T`) yields
  a clear diagnostic ("error union of nullable not supported in v1") and exit 1. No
  SIGILL/exit 132. Regression fixture in the P0.9 reject suite. Note the asymmetry: `?!int`
  in return position compiles today — leave it compiling (frozen) unless it also produces
  invalid IR, in which case report as an open finding.
- **P0.4 — Gate `TOKEN_UNKNOWN` like `TOKEN_ERROR`.** `x := 1 # 2` is REJECTED with
  file:line:col of `#` and nonzero exit (today: compiles and prints 1 — a silent
  acceptance of garbage source). Mirror the `TOKEN_ERROR` handling at `lexer.c:552-558`;
  count into `goo_lexer_error_count`. Reject fixture added; all existing verify probes pass.
- **P0.1 — Fix the comma-ok channel receive double-recv miscompile.** `v, ok := <-ch`
  lowers to exactly ONE `goo_chan_recv` whose status becomes `ok` (today: TWO calls — the
  second blocks on the now-empty channel and aborts with a spurious "all goroutines are
  asleep - deadlock!", exit 2, even with a value buffered). Acceptance: grep of the emitted
  `.ll` shows a single `goo_chan_recv` call for the statement; a buffered channel holding 9
  prints `9 true`, exit 0; golden fixture added.
- **P0.5 — Convert comment-skip tail recursion to iteration.** `lexer.c:316` and
  `lexer.c:329` no longer recurse per comment line: 400,000 consecutive `// c` lines compile
  exit 0 (today: SIGSEGV at ~100k). The comment+ASI interplay is preserved
  (`x := 10 // c` ⏎ `*p = 7` still prints 7). A comment-lines probe is added beside the
  existing blank-lines probe.
- **P0.6 — Go-conformant panics: exit status 2, no core dump.** An out-of-bounds program
  prints the runtime error and exits 2 (today: SIGABRT/exit 134). `GOO_PANIC_ABORT=1`
  restores `abort()` for debugging. Panic fixtures use P0.8's `.exit` convention; Makefile
  panic probes (those not yet migrated) assert exit 2.

## Layer 3 — Aggregate gate (last)

- **P0.10 — `make verify-core`.** Runs the full probe net EXCEPT ccomp-gated targets and
  exits 0 with no opam switch installed (verified with a stripped PATH). `make verify`
  itself is unchanged. CLAUDE.md documents both targets and when to use each.

## Global constraints (frozen behavior)

- C23. Clean diagnostics or clean failures only — no LLVM verifier noise reaching users,
  no signals for user-caused errors.
- NO `parser.y` changes anywhere in this arc (P0.4/P0.5 are `lexer.c`-only; if a fix seems
  to need grammar work, that is BLOCKED). `./scripts/grammar-tripwire.sh` must stay
  82 S/R + 256 R/R exact — run as a guard in every task.
- Everything outside each task's target behavior byte-for-byte frozen. Gates before every
  commit: `make test-golden` (321/0 at start, grows), `make comptime-value-reject-matrix`
  (18/18), `make comptime-generic-compose-ir-pin` (PASS), `make test` (76/1-skip),
  `make spmd-bench-probe` (PASS), tripwire. P0.10 adds `make verify-core` to that list for
  its own and subsequent commits.
- P0.6 changes observable exit codes of panicking programs: existing probes/fixtures that
  assert 134/SIGABRT are updated IN THE SAME COMMIT as the runtime change — that is the
  task's target behavior, not a frozen-behavior violation.
- Commits: conventional prefixes, atomic, `--no-gpg-sign`, imperative mood.

## Scope (YAGNI) — explicitly NOT this arc

- Nil semantics (P2.2), main-exit semantics (P3.3), catch-fallback form (P2.9) — open
  decisions owned by the human, untouched here.
- The `-O` no-op (P3.10), fake-tooling quarantine (P5.5), dead-code unlinking (P5.6),
  golden-runner parallelization (P5.8) — later phases.
- Migrating ALL bespoke Makefile probes to the new conventions — only the 3 (P0.8) + 5
  (P0.9) named pilots move now; wholesale migration is later housekeeping.
- Fixing `?!int` or any additional composed-type behavior beyond rejecting `!?T`.

## Testing

- TDD per fix: the audit reproducer is the failing test first (RED), then the fix (GREEN).
  Reproducers: buffered comma-ok recv (P0.1), `!?int` return (P0.2/P0.3), `x := 1 # 2`
  (P0.4), 400k comment lines (P0.5), out-of-bounds index (P0.6).
- The harness tasks are self-testing: a deliberately looping fixture must FAIL under P0.7's
  timeout; a wrong-exit fixture must FAIL under exit-code capture; each migrated pilot must
  fail if its assertion is inverted (prove the harness can see the failure it exists to
  catch — a harness that cannot fail is vacuous).
- Golden suite count grows with each pinned fixture; report actual counts, never assume.

## Open verification points for the implementer

1. P0.1: locate the double-emission site (expression vs statement codegen for the
   two-value receive form) and confirm whether `select` with comma-ok shares the path.
2. P0.7/P0.8: confirm how `scripts/run_golden.sh` discovers fixtures today and that the
   `.exit`/`.stderr.txt` sidecar convention cannot collide with existing sidecar files.
3. P0.9: verify which 5 `*-reject-probe` targets are the cleanest pilots (prefer ones whose
   stderr is stable across LLVM versions).
4. P0.6: inventory every existing probe/fixture asserting abort/134 semantics before
   flipping the default (grep for `134`, `SIGABRT`, `core` in Makefile + scripts).
5. P0.10: enumerate the ccomp-gated targets precisely (grep for `ccomp`/`opam` in the
   `verify` dependency closure) — `verify-core` must be `verify` minus exactly that set.
