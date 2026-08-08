# The coverage baseline — 197 gates reach 58.1% of the branches they guard

Taken 2026-08-08 on `measure/branch-coverage`, gcc 16.1.1, LLVM 22.1.8.
Command: `make coverage-goo`. 810 fixtures, 0 crashed, 0 timed out,
61/61 instrumented translation units wrote counters.

**This is the first coverage number this project has ever had.** The previous
`coverage` target reported one, and it measured nothing — see "The target that
measured nothing" below.

## The numbers

Scope is the shipped compiler: `GOO_SRCS` (the P5.6 reachable set), less
`src/runtime/`.

| Metric | Covered | Total | Percent |
|---|---|---|---|
| Branch directions taken | 12,852 | 22,103 | **58.1%** |
| MC/DC condition outcomes | 11,452 | 20,254 | **56.5%** |

51 files measured. Richard Hipp's SQLite bar is 100% MC/DC, reached in 2009
after a year of work, after which the inbound bug reports stopped.

`src/runtime/` (11 files, 893 branches) is excluded and reads 0.1%. It is
linked into `bin/goo` but executes only inside COMPILED programs, and this
corpus compiles without running. Measuring it needs its own corpus. Including
it would have reported 55.9% / 54.2% by padding the denominator with 893
permanently unreachable branches.

## The corpus

| Source | Fixtures |
|---|---|
| `examples/*.goo` | 595 |
| `tests/golden/reject/*.goo` | 156 |
| `tests/spec/**/*.goo` | 59 |
| **Total** | **810** |

Each fixture gets a FULL compile (`goo-cov -o <tmp> <fixture>`), so the link
path in `codegen.c` counts as well as the front end. Exit status is ignored on
purpose: a reject fixture must fail, and its diagnostic path is exactly what
the run came to measure. The corpus runs SERIALLY — concurrent processes merge
`.gcda` counters without a reliable lock, so a parallel run corrupts them.

## The instrument was verified

A coverage script that measures nothing reports a percentage either way, so the
number above is worth nothing until the script has been shown to move. Six
assertions refuse to print a number, and two of the six were confirmed by
firing them on purpose:

| Check | Confirmed |
|---|---|
| Binary is missing | fired: `BROKEN — no executable at bin/does-not-exist` |
| Binary carries no `.gcno` | fired: pointed at `bin/goo`, `BROKEN — built WITHOUT -fprofile-arcs` |
| Corpus is empty | not fired |
| No fixture reached the compiler | not fired |
| No `.gcda` written after N compiles | not fired |
| gcov ran without `-b`, so 0 branches | not fired |

The last one is not hypothetical. `gcov --json-format` emits an EMPTY
`branches` array unless `-b` is passed, and 0 branches across 61 files reads
exactly like a clean result. It was found by checking, not by review.

The end-to-end check is `make coverage-goo-selftest`, which halves the corpus:

| Corpus | Branch | MC/DC | Denominator |
|---|---|---|---|
| 810 fixtures | 58.1% | 56.5% | 22,103 |
| 405 fixtures (self-test) | 51.9% | 49.9% | 22,103 |

The numerator falls and the denominator holds, which is what a working
instrument does. (Both self-test figures were taken before `src/runtime/` was
split out, so they carry the old 22,996 denominator; the direction is the
result, not the absolute value.)

## Two dead modules, found by the measurement

Six files took ZERO branches across all 810 fixtures. Four are explainable:
`wasm_codegen.c` (no fixture targets WASM), `test_discovery.c` (the corpus
never runs `goo test`), `comptime_intrinsics.c`, `ergonomic_errors.c`.

Two are findings, both confirmed by grep afterwards:

**`src/errors/error.c` — 191 branches, 0 taken.** The module owns
`report_error`, `report_warning`, `report_fatal`, `print_all_errors` and the
whole `ErrorContext`. Outside itself it has exactly 3 call sites: 2 in
`src/parser/parser_errors.c` and 1 in `src/ide/time_travel_debug.c`, which is
not linked into `bin/goo`. All 156 reject fixtures produce their diagnostics
through `yyerror`'s direct `fprintf` at `src/parser/parser_errors.c:242`, which
bypasses `ErrorContext` entirely. The shared error reporting module is linked
and dead.

**`src/types/channel_checker.c` — 0 branches taken.** Every exported function
(`check_channel_operation_valid`, `type_check_channel_send`,
`type_check_channel_receive`, `check_channel_assignment_compatibility`,
`check_select_case_compatibility`, and 3 more) has ZERO external call sites in
`src/`. Channels, `select` and `close` are probe-gated and work, so the type
checking for them happens on another path. This module is linked and dead.

Both are P5.6-style quarantine candidates. Neither is fixed here.

## The target that measured nothing

The deleted `coverage` target was broken three independent ways, any one of
which alone would have voided the result:

1. It measured `bin/test_runner_coverage`. `test_runner` links 5 of the 21
   files under `tests/unit/`, and none of the 197 `verify-core` gates.
2. Its link line read `$(filter-out …, $(OBJS:_coverage.o=))`. The members of
   `$(OBJS)` end in `.o`, not `_coverage.o`, so the substitution was a no-op
   and it linked the NON-instrumented objects against one instrumented
   `test_main.o`.
3. `COVERAGE_LIBS = -lgcov` was defined at `Makefile:44` and referenced
   nowhere, so `-lgcov` never reached any link line.

It also referenced `$(BUILDBIN)`, which is not defined anywhere in the
Makefile.

## Re-measured 2026-08-08 after the xstrdup sweep and the asserts

The plan for this arc said the assert work would move the branch count, so the
baseline would have to be retaken. It was retaken on `main` at 7e93c84, which
carries all three of the coverage build, the xstrdup sweep and GOO_ASSERT.

**The number did not move at all** — not the percentage, and not the raw counts:

| | Before | After |
|---|---|---|
| Branch | 58.1% (12,852 / 22,103) | 58.1% (12,852 / 22,103) |
| MC/DC | 56.5% (11,452 / 20,254) | 56.5% (11,452 / 20,254) |

Identical numerator AND denominator is the shape of a stale build, so it was
checked rather than assumed. `bin/goo-cov --version` reports
`asserts: off, GOO_NEVER/GOO_ALWAYS folded (GOO_COVERAGE)`, a line that only
exists after the assert work, and `src/` holds 570 `xstrdup` calls and zero raw
`strdup`. The binary is current. The result is real.

The cause is exact, and it makes an implicit scoping decision load-bearing:

- `GOO_ASSERT` expands to `(void)0` under `GOO_COVERAGE`, by design. No branch.
- `GOO_NEVER` and `GOO_ALWAYS` fold to constants under `GOO_COVERAGE`, also by
  design — and nothing uses them yet, so there was nothing to fold.
- `xstrdup`'s `if (!p)` IS a new branch, inlined at all 570 call sites. It lives
  in `include/xalloc.h`, and the report counts only files under `src/`.

**That last exclusion is now doing real work and should be read as a choice.**
`scripts/coverage_corpus.sh` keeps a file only when its path starts with `src/`,
so every branch in a force-included header — `xalloc.h`'s four OOM checks,
`goo_assert.h`'s handler — sits outside the denominator. Counting them would add
one uncoverable branch per allocator wrapper, repeated at every inline site, and
none of them is reachable without allocator fault injection, which this project
does not do (`include/xalloc.h` exits on OOM by design).

The defensible reading: this denominator measures the compiler's own decisions,
not its allocator wrappers. It is not a claim that those branches are covered.

## What this does NOT measure

- **The runtime.** See above. Needs a corpus that RUNS compiled programs.
- **Whether the covered branches are covered CORRECTLY.** Coverage says a
  branch ran, not that its result was checked. That is what the mutation tests
  (`scripts/release_decision_teeth.sh`, `scripts/escape_teeth.sh`) are for, and
  they remain the stronger signal.
- **The unlinked frameworks.** Constraint inference, concept generics, HKT and
  the flow/reference-manager code are outside `GOO_SRCS` by P5.6 and outside
  this denominator.

## Re-take it

```
make coverage-goo             # the number
make coverage-goo-selftest    # prove the number can fall
make coverage-clean           # drop bin/goo-cov, build/cov/, coverage/
```

Not a gate, and deliberately absent from `VERIFY_ALL_DEPS`. A coverage target
invites tests that raise the number instead of tests that find bugs. The number
is an input to "where is the next probe worth writing", nothing more.
