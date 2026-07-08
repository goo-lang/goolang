# SPMD harness

Pattern guide for goroutine-per-lane parallelism in Goo. This documents what
exists and compiles today — no new language surface, no stdlib. See
`docs/superpowers/specs/2026-07-08-spmd-harness-design.md` for the design
rationale and the compiler fix (defer arg-stash) that unblocked the pattern.

Every snippet below is either a verbatim excerpt from a committed golden
(`examples/spmd_*.goo`) or a program compiled and run during this task
(the `spmd-bench-probe` Makefile target's inline kernel).

## The canonical program

`examples/spmd_fanout_probe.goo` is the keystone golden: a comptime-specialized
slice-reduction kernel, `len()`-arithmetic partitioning into even tiles plus an
uneven remainder tile, and buffered-channel fan-in. Reproduced code-verbatim
below (the golden's standalone header/inline comments are elided; the code
itself is unchanged):

```goo
package main

import "fmt"

func lane(comptime tile int, xs []int64, out chan int64) {
	var buf [tile]int64
	i := 0
	for i < tile {
		buf[i] = xs[i]
		i = i + 1
	}
	sum := buf[0]
	i = 1
	for i < tile {
		sum = sum + buf[i]
		i = i + 1
	}
	out <- sum
}

func main() {
	const TILE = 4
	const REM = 2
	data := []int64{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}

	n := len(data)
	nfull := n / TILE       // 10 / 4 = 2 full TILE=4 windows
	remStart := nfull * TILE // 8: first index of the leftover window

	tiles := [][]int64{}
	i := 0
	for i < nfull {
		lo := i * TILE
		hi := lo + TILE
		tiles = append(tiles, data[lo:hi])
		i = i + 1
	}

	results := make(chan int64, 3)

	i = 0
	for i < nfull {
		go lane(TILE, tiles[i], results)
		i = i + 1
	}
	go lane(REM, data[remStart:n], results)

	total := int64(0)
	i = 0
	for i < 3 {
		total = total + <-results
		i = i + 1
	}
	fmt.Println(total)
}
```

Output: `55` (= sum(1..10), independently checkable).

Walkthrough:

- **`lane` is comptime-specialized, not generic.** `comptime tile int` means
  every distinct *value* of `tile` that `main` calls with gets its own
  compiled instance — `lane__n4` for the two full-width tiles, `lane__n2`
  for the remainder. This is what lets `var buf [tile]int64` be a real
  fixed-size stack array per instance rather than a dynamically-sized one.
- **Partitioning is derived from `len(data)`, not hardcoded.** `nfull` and
  `remStart` are computed from `n := len(data)`, so the same shape holds for
  any input length by changing only `data` and `TILE` — **except** the
  channel capacity (`make(chan int64, 3)`) and the receive count
  (`for i < 3`), which encode `nfull + 1` (the lane count) and must be
  updated in step with `data`/`TILE` too, e.g. `nlanes := nfull + 1`.
- **`TILE`/`REM` are `const`; the slice bounds (`lo`, `hi`, `remStart`) are
  ordinary runtime `int`s.** A comptime parameter's argument must itself be
  a compile-time constant; the arithmetic that produces slice *bounds* has
  no such restriction.
- **The remainder tile is a second, distinct comptime tuple** (`lane__n2`)
  fanned out concurrently with the `lane__n4` goroutines. This is the shape
  every uneven partition takes, and it's exactly what the defer-stash fix
  (this sub-project's Task 1) had to unblock: two distinct instantiations of
  the same template in flight together.
- **Buffered fan-in (`cap 3`, one slot per lane) is the join.** All three
  sends can land before `main` drains any of them, so the three receives are
  order-independent — summation is commutative, so the total is
  deterministic regardless of goroutine interleaving or scheduling.

## Idioms

The canonical program above already demonstrates four idioms in context:
`len()`-arithmetic partitioning, `const` tile sizes, the remainder tile as a
second comptime tuple, and buffered-channel fan-in as the join. The rest of
the pinned pattern lives in the other three goldens:

### Multi-lane, same tuple: capture the loop variable per-iteration

When many lanes share one kernel instance (rather than fanning out across
distinct tuples), each `go` call must bind its own copy of the loop variable.
`examples/spmd_multilane_probe.goo` spawns 8 goroutines onto a single
instance (`lane__n8`):

```goo
i := 0
for i < 8 {
	go lane(8, int64(i), results)
	i = i + 1
}
```

`i` is captured per-call at each `go` statement, not by reference to one
shared loop variable — each lane observes its own value. Output: `224` (=
`8 * (0+1+...+7)`).

### Defer-in-kernel, at two tuples

`examples/spmd_defer_probe.goo` uses `defer` (closure-literal form) for the
kernel's send, dispatched at two distinct comptime tuples (`tile=4`,
`tile=2`) concurrently — the shape that used to fail closed before the
defer-stash fix:

```goo
func lane(comptime tile int, xs []int64, out chan int64) {
	var buf [tile]int64
	i := 0
	for i < tile {
		buf[i] = xs[i]
		i = i + 1
	}
	sum := int64(0)
	j := 0
	for j < tile {
		sum = sum + buf[j]
		j = j + 1
	}
	defer func(v int64) {
		out <- v
	}(sum)
}
```

### The closure-wrapper for generic result channels

`chan T` cannot infer `T` from a concrete channel argument at a call site
(a pre-existing gap — see Limits below), so a generic/composed kernel's
result can't be collected by passing a `chan T` parameter directly. The
workaround, from `examples/spmd_composed_probe.goo`, is to wrap the `go`
dispatch in a closure that captures a *concrete*-typed channel instead:

```goo
func kernel[T any](comptime n int, seed T) T {
	var buf [n]T
	i := 0
	for i < n {
		buf[i] = seed
		i = i + 1
	}
	return buf[n-1]
}

func main() {
	ich := make(chan int64, 1)
	fch := make(chan float64, 1)
	x := int64(42)
	y := 2.5

	// Fan-out: both composed instances dispatched before any receive.
	go func() {
		ich <- kernel(4, x)
	}()
	go func() {
		fch <- kernel(4, y)
	}()

	// Fan-in: drain in a fixed order (separate channels, so no race).
	a := <-ich
	b := <-fch

	fmt.Println(a)
	fmt.Println(b)
}
```

`kernel[T any]` composed with `comptime n int` still fans out concurrently
(both closures dispatched before either channel drains) — the closure
wrapper only changes how the *result* is collected, not the dispatch shape.
Note `kernel` does data movement only (copy into `[n]T`, return a slot), not
arithmetic on `T` — see Limits below.

## Limits and workarounds

Current limits, documented not fixed — this list is verbatim-consistent with
the design spec's Scope section
(`docs/superpowers/specs/2026-07-08-spmd-harness-design.md`, "Scope (YAGNI,
first cut)"); if the two ever disagree, the spec is the source of truth.

- **Arithmetic on `T any` in template bodies is rejected.** Generic kernels
  are limited to data movement; numeric kernels drop `[T any]` and use
  comptime-only specialization instead. No in-body workaround exists (no
  `Numeric` constraint; `T`→concrete conversion is rejected). Top-ranked
  follow-up candidate: a built-in numeric constraint or instance-time
  operator checking — a type-system design of its own, out of scope here.
- **`chan T` cannot infer from a concrete channel argument.** Workaround
  idioms: a concrete channel parameter, or the closure wrapper shown above.
  (Pre-existing; `unify_types` lacks `TYPE_CHANNEL`.)
- **No `time` stdlib** — benchmark timing is external, via `/usr/bin/time`
  (see `spmd-bench-probe` below). **No `sync` primitives** — channel fan-in
  IS the join. **No `.goo`-level scheduler control** — `GOMAXPROCS` env var
  plus an NCPU default, clamped to `[1,16]`, per `src/runtime/concurrency.c`.
- **`fmt.Println` on `T`-typed values in template bodies; package-global
  assignment in template bodies; bare `<-ch;` discard diagnostic** —
  pre-existing, catalogued, workarounds trivial (concrete types / channel
  fan-in / assign to `_`).

## Measured numbers

**787% CPU utilization, ~6.2x wall-clock speedup**, 8-lane vs. serial, on a
32-core machine — session-measured during the SPMD scout that grounded this
sub-project (2026-07-08, method: external `/usr/bin/time`). This is the
number the design spec cites as the sub-project's original evidence that `go`
is genuinely NCPU-multicore, not cooperatively-scheduled on one thread.

This task's own `spmd-bench-probe` target (a CPU-bound comptime kernel —
a tight LCG loop, `ITERS=200000000`, `N=8` lanes — built and run twice, once
fanned out via `go`, once as a serial baseline) reproduced comparable numbers
on the same class of machine, session-measured, same method, across three
consecutive runs:

| run | 8-lane wall | 8-lane CPU | serial wall | speedup |
|-----|-------------|------------|--------------|---------|
| 1   | 0.609s      | 788%       | 3.989s       | 6.55x   |
| 2   | 0.616s      | 781%       | 3.712s       | 6.03x   |
| 3   | 0.451s      | 791%       | 3.371s       | 7.47x   |

**These are session-measured numbers on one 32-core development machine, not
reproducible CI numbers.** `spmd-bench-probe` reports wall-clock and CPU
utilization as informational lines only; it never asserts a speedup
threshold (an opt-in `SPMD_BENCH_ASSERT_SPEEDUP` gate exists for manual local
runs — see the Makefile target's comment). Confirmed with `GOMAXPROCS=1`: the
same probe still compiles, runs, and asserts bit-identical output correctly
— the 8-lane variant simply runs no faster than serial (speedup ~0.8x) when
only one OS thread is available, which is expected and does not fail the
probe. The pattern's parallelism is real on multicore hardware; the harness
only asserts correctness, by design, so it holds on machines with far fewer
cores too.
