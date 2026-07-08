# SPMD harness (design)

Sub-project 3 of the SPMD/parallel-lanes keystone (Leg 3 of the hybrid-memory direction).
Sub-projects 1-2 (PRs #160, #161) delivered the substrate: comptime-value-specialized
functions and per-type × per-value composition, with `go`/`defer` dispatch. This
sub-project delivers the keystone itself: the goroutine-per-lane SPMD pattern proven,
pinned, and documented end-to-end — plus the one compiler fix that hard-blocks it.

Grounding: the SPMD scout report (2026-07-08, main @ 11754a2, programs under
/tmp/spmd_scout/ during the session; the load-bearing ones become committed goldens
here). Measured on real compiled Goo SPMD code this session: 8 lanes at **787% CPU,
wall 0.607s** vs serial **99% CPU, 3.747s** (~6.2× wall, ~7.9 cores) — independently
reconfirming Spike 0's figure on the shipped machinery.

## What "harness" means here (scope decision)

No new language surface and no stdlib (the language has no `time` package, no `sync`
primitives; channel fan-in is the only join, and `go` is NCPU-multicore by default via
the M8 scheduler — all verified). The harness is:

1. **The defer-stash compiler fix** — the single hard blocker for real SPMD kernels.
2. **The pattern, pinned**: the scout-proven SPMD idioms committed as goldens.
3. **The proof, wired**: a CPU-bound benchmark probe in `verify` that asserts
   correctness deterministically and reports (not asserts) parallel timing.
4. **The pattern, documented**: a short SPMD guide with the canonical program.

## The compiler fix: defer-stash destructive splice

`defer f(args)` inside ANY template (generic, comptime, or composed) instantiated at
≥2 distinct tuples fails on the second instance with
`Undefined identifier '__goo_defer0_arg0'` + `failed to evaluate defer argument`
(fails closed). Mechanism (from the sub-project-2 final review + scout confirmation):
the defer arg-stash (src/codegen/statement_codegen.c, ~2280) destructively splices
synthetic identifier nodes into the SHARED template AST; the second instance's
emission then finds already-rewritten args whose synthetic names resolve to nothing.

Precisely bounded by the scout: single-tuple defer works (both closure-literal and
direct-call-with-args forms, including repeated calls to the same instance); the bug
is specifically the SECOND DISTINCT TUPLE. This bites the natural SPMD program the
moment partitioning goes uneven (remainder tile = second tuple) and the kernel uses
`defer` for cleanup/sends.

**Fix requirement (behavior, not mechanism):** template AST must be observably
unchanged after any instance's emission — either restore the original argument nodes
after the stash-emit (save/swap-back), or make the stash non-mutating (clone the
spliced nodes per emission / rewrite at the LLVM value level). Implementer chooses
whichever the code structure favors; the test is the same either way: a kernel with
`defer` (direct-call form AND closure form) instantiated at two tuples compiles and
runs correctly, and single-tuple defer behavior is byte-for-byte unchanged. This is
a pre-existing bug on ALL axes (generic-only, comptime-only, composed — verified at
sub-project-2's base), so the fix must be verified on all three.

## The pinned pattern (goldens, from the scout's proven programs)

- **Keystone fan-out** (`spmd_fanout_probe`): comptime-specialized slice kernel
  (`lane(comptime tile int, xs []int64, out chan int64)` with `[tile]int64` local
  reduction), `[][]int64` partitioning, `go lane(TILE, tiles[i], results)` in a loop,
  buffered-channel fan-in. (Scout P1-v2, output 36.)
- **Uneven tiles** (same probe or sibling): two lanes at `tile=4` + remainder lane at
  `tile=2` — two distinct comptime tuples fanned out concurrently. (Scout P6-remainder.)
- **Multi-lane same-tuple**: 8 goroutines in a `for` loop on one instance, loop-var
  args correctly captured. (Scout P2, output 224.)
- **Defer-in-kernel at two tuples** (post-fix): deferred channel send in the kernel,
  uneven tiles. (Scout P4e shape — the fix's end-to-end pin.)
- **Closure-wrapper collection**: `go func(){ ch <- kernel(4, x) }()`. (Scout P5c —
  the documented `chan T` workaround idiom.)
- Composed (generic×comptime) lane doing data movement with concrete-channel
  collection — pins the composition in the SPMD shape within today's limits.

## The proof (benchmark probe)

`spmd-bench-probe` Makefile target in `verify`: builds the scout-P3-style CPU-bound
comptime kernel twice (8-lane fan-out and serial baseline), runs both, ASSERTS
bit-identical results (deterministic, CI-safe), and REPORTS wall-clock and CPU
utilization via `/usr/bin/time` for both (informational lines, never a pass/fail
threshold — timing assertions are flaky by nature; the parallelism evidence lands in
the target's output and the guide). An optional `SPMD_BENCH_ASSERT_SPEEDUP=<factor>`
env knob may gate a threshold for manual runs, default off.

## The guide

`docs/spmd-harness.md`: the canonical program annotated line-by-line; the idioms
(partitioning with `len()` arithmetic and slice bounds, const tile sizes, remainder
tiles as a second comptime tuple, buffered-channel fan-in as the join, the
closure-wrapper for generic result channels); the current limits with their
workarounds (below); the measured parallelism numbers with the measurement method.

## Scope (YAGNI, first cut) — current limits, documented not fixed

- **Arithmetic on `T any` in template bodies is rejected** (scout Wall A — NEW
  catalogue entry, not previously known): generic kernels are limited to data
  movement; numeric kernels drop `[T any]` and use comptime-only specialization.
  No in-body workaround exists (no `Numeric` constraint; `T`→concrete conversion
  rejected). **Top-ranked follow-up (sub-project 4 candidate): a built-in numeric
  constraint or instance-time operator checking** — a type-system design of its own.
- `chan T` cannot infer from a concrete channel argument — workaround idioms: concrete
  channel param, or the closure wrapper. (Pre-existing; unify_types lacks TYPE_CHANNEL.)
- No `time` stdlib (benchmark timing is external via `/usr/bin/time`); no `sync`
  primitives (channel fan-in IS the join); no `.goo`-level scheduler control
  (GOMAXPROCS env + NCPU default, clamped [1,16], per src/runtime/concurrency.c).
- `fmt.Println` on T-typed values in template bodies; package-global assignment in
  template bodies; bare `<-ch;` discard diagnostic — pre-existing, catalogued,
  workarounds trivial (concrete types / channel fan-in / assign to `_`).

## Testing

- Defer fix, PINNED as committed goldens (comptime axis, two distinct tuples each):
  closure-literal form (`spmd_defer_probe.goo`), direct-call form
  (`spmd_defer_direct_probe.goo`), and method-receiver form
  (`spmd_defer_method_probe.goo`, exercising the recv_synth splice path). Generic-only
  and composed-axis defer were SESSION-VERIFIED ONLY during Task 1, via ad hoc `/tmp`
  programs — not committed as goldens. Single-tuple defer regression pinned (existing
  behavior frozen); the exact scout repro shapes.
- The goldens above wired into `test-golden` (suite currently 315/0 → grows).
- `spmd-bench-probe` in `verify` (correctness-asserting).
- Frozen everything: golden suite, reject matrix 18/18, ir-pin, unit 76/1-skip,
  grammar tripwire 82 S/R + 256 R/R (no parser changes expected).

## Open verification points for the implementer

1. The defer stash's exact splice site and whether the closure-literal defer form
   shares it (the scout saw both forms work single-tuple; confirm one fix covers both).
2. Whether defer-in-goroutine (deferred send inside a `go`-dispatched instance)
   emission shares the same stash path.
3. The benchmark's lane-count portability: the probe must pass on machines with fewer
   cores (correctness assert only; report whatever parallelism the machine gives).
