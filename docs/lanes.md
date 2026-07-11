# goostd/lanes: compile-time-proven SPMD lanes (P6 M1)

`goostd/lanes` (`goostd/lanes/lanes.go`) is a library-only, compile-time-proven
data-parallel primitive: `Partition` splits a `[]float64` into `count`
disjoint, exclusively-owned per-lane views, and the Goo compiler **rejects**
any program that touches the source slice after partitioning, lets a lane
view escape into a second goroutine, or lets a lane body reach any view but
its own. `Run` then spawns one goroutine per lane and drives a BSP
(bulk-synchronous-parallel) halo-exchange protocol between neighbors.

This document is the M1 user guide. It documents what ships today — no new
language surface, no 2D, no reductions. The normative design record is
`docs/superpowers/specs/2026-07-11-p6-lanes-m1-design.md` (design) and
`docs/superpowers/specs/2026-07-11-p6-lanes-m1-spike-findings.md` (spike);
if this guide and the design doc ever disagree, the design doc is the source
of truth for intent and `goostd/lanes/lanes.go` / `include/lane_ownership.h`
are the source of truth for actual behavior.

## The canonical program

`examples/lanes_stencil_probe.goo` is the milestone's centerpiece golden: a
1D heat-equation stencil computed two ways — a plain serial reference and a
`goostd/lanes.Run` BSP halo-exchange parallel version — asserted **bit-for-bit
identical**. N=16 cells, count=4 lanes (width 4 each), steps=8 BSP rounds,
Dirichlet boundary 0.0 on both ends, initial condition a unit spike at the
center cell. The update rule reads only a cell's own and immediate
neighbors' OLD values (no cross-lane reduction — see Limits below on why
that matters for bit-identity):

```goo
import "lanes"

p := lanes.Partition(parallelData, 4)
parallelResult := lanes.Run(p, steps, func(ctx *lanes.Lane) {
	own := ctx.Own()
	w := len(own)
	scratch := make([]float64, w)
	for {
		ctx.Publish()
		hl := ctx.HaloLeft()
		hr := ctx.HaloRight()
		j := 0
		for j < w {
			left := hl
			if j > 0 {
				left = own[j-1]
			}
			right := hr
			if j < w-1 {
				right = own[j+1]
			}
			scratch[j] = 0.25*left + 0.5*own[j] + 0.25*right
			j = j + 1
		}
		j = 0
		for j < w {
			own[j] = scratch[j]
			j = j + 1
		}
		if !ctx.Step() {
			break
		}
	}
})
```

Walkthrough:

- **`Partition(parallelData, 4)` moves `parallelData` and returns a
  `Partitioned` describing 4 disjoint tiles of width 4.** `count` is a
  `comptime` parameter, so the tile width `w = len(arr)/count` is a
  compile-time constant and `Partition` monomorphizes per distinct `count`
  (see Comptime specialization below).
- **`Run` calls `body` exactly once per lane** — it does not loop `body` for
  you. The multi-round BSP loop lives *inside* `body`, driven by
  `ctx.Step()`'s return value. This keeps `Run` itself unchanged (one call,
  one goroutine, one join per lane) and puts step-count control where it's
  a per-lane concern.
- **Publish → read (halo) → compute, every round.** `Publish()` sends this
  lane's current boundary cells to both neighbors; `HaloLeft()`/`HaloRight()`
  block-receive the neighbors' just-published values (or the fixed Dirichlet
  boundary at the domain's outer edges); then the lane computes every owned
  cell into a **scratch** slice and copies it back only after the whole
  round's compute is done. That scratch-then-copy is what makes a lane's
  intra-tile reads see exactly the previous round's values — matching the
  serial reference's old/new double-buffer discipline cell-for-cell.
- **Comparison is bit-for-bit** (`!=` on `float64`, no epsilon) against a
  serial reference computed by a plain nested loop, entirely outside
  `goostd/lanes`, never hand-computed. This is only meaningful because the
  update rule is associativity-safe (see Limits).

## The four compile-time guarantees

Wired as one new checker pass, `src/types/lane_ownership.c` /
`include/lane_ownership.h`, called from `type_check_program` after all
function bodies are checked. Each guarantee has a golden reject fixture
under `tests/golden/reject/`, exercised by `make test-golden-reject`.

### 1. `Partition` moves its argument (use-after-move)

`lanes.Partition(arr, count)` moves `arr`; any later read of `arr` in the
same function body is a compile error. Prevents aliasing the whole slice
through its parts.

```goo
data := []float64{1.0, 2.0, 3.0, 4.0}
p := lanes.Partition(data, 2)
fmt.Println(data[0])  // rejected: data was moved into Partition
```

Diagnostic substring: **`moved into lanes.Partition`**
(`tests/golden/reject/partition_move_reject.goo`).

A companion fixture, `partition_selfref_reject.goo`, pins the ordering rule
on `data = clone(data)`: the RHS `clone(data)` reads the still-moved `data`
and must reject *before* the assignment would otherwise revive the name.
Reassignment (a bare-identifier LHS `=`) is a **write**, not a read, and
does legally revive a moved name for subsequent use — see
`examples/lanes_repartition_probe.goo`, which partitions and re-partitions a
slice across a reassignment, all legally.

### 2. `Run` consumes its `Partitioned` argument (use-after-consume)

`lanes.Run` consumes its `Partitioned` argument via the same move machinery;
a second `Run` on the same binding is rejected.

```goo
p := lanes.Partition(data, 2)
out1 := lanes.Run(p, 1, body1)
out2 := lanes.Run(p, 1, body2)  // rejected: p already consumed
```

Diagnostic substring: **`consumed by lanes.Run`**
(`tests/golden/reject/partition_rerun_reject.goo`). Re-`Run`ning a
*different* partition stays legal.

### 3. Single-goroutine attribution (view-escape)

Inside a `lanes.Run` body, the `*Lane` context and every name derived from
it (`t := ctx.Own()`, and transitive copies) are "lane-derived." A `go` that
carries a lane-derived name — as a call argument or as a captured name of
the launched literal — is rejected.

```goo
out := lanes.Run(p, 1, func(ctx *lanes.Lane) {
	t := ctx.Own()
	go leak(t)  // rejected: t is lane-derived
})
```

Diagnostic substring: **`may not be passed to another goroutine`**
(`tests/golden/reject/partition_escape_reject.goo`).

### 4. View-capture rule

A `lanes.Run` body literal may not *capture* a `Partitioned` binding, a
moved source array, or a name lane-derived from an enclosing `Run` body.
This is a capture/escape rule, not index-range analysis — dynamic
out-of-range indexing through a lane's own view is an ordinary runtime
bounds panic, same as any Goo slice.

```goo
p := lanes.Partition(data, 2)
out := lanes.Run(p, 1, func(ctx *lanes.Lane) {
	q := p  // rejected: capturing the Partitioned binding
	_ = q
})
```

Diagnostic substring: **`lane body may not capture`**
(`tests/golden/reject/partition_capture_reject.goo`). Benign captures (outer
scalars like `steps`, non-partition slices) stay legal.

### The honest envelope of these checks

`include/lane_ownership.h` is the source of truth; do not let this summary
drift from it. In brief:

- This is **not** a general borrow checker — it is one self-contained,
  per-function AST walk shaped specifically around `lanes.Partition`/
  `lanes.Run`/`*Lane` call shapes (Option A scope boundary from the design
  doc). Code outside that shape gets zero extra scrutiny.
- The walk does not descend into `if let`, `select`, `arena { }`,
  `unsafe { }`, `comptime { }`, or a local `const` declaration at all — a
  move or a moved-name read inside one of these is invisible to it.
- The obligation-3 body walker is narrower still: taint does not propagate
  through multi-assignment targets or a plain-assignment `ExprStmt`
  (`x = ctx.Own()`), and it does not descend into nested non-launched
  closures.
- A bare, unbound `lanes.Partition(...)` statement (result discarded, not
  bound via `:=`/`var`) records no move at all.
- Every tracking table is flat and **shadow-unaware**: a shadowed name
  reusing a moved/lane-derived identifier is a known false-reject/
  false-accept-adjacent surface — a documented Option A scope boundary, not
  a bug fixed under M1. This extends to **detection itself**: a local
  variable named `lanes` with its own `Partition`/`Run` method is
  misattributed as the package call and false-rejects (clean diagnostic,
  no crash) — rename the variable.
- Move tracking is **flow-insensitive**: a `Partition` inside a conditional
  branch poisons reads after the branch even when the branch may not
  execute at runtime — conservative move-checker behavior.

All of the above is a deliberate under-reject direction: real and
load-bearing for the shapes it covers, not a proof that no lanes program
anywhere can misuse a view.

## The protocol

Per-neighbor channels, fixed `Publish → read (halo) → compute` order,
goroutine-parking throughout (never an OS-thread barrier).

- **Topology.** Between adjacent lanes `(i, i+1)`: `rightward[i]` carries
  lane `i`'s right boundary to lane `i+1`; `leftward[i]` carries lane
  `i+1`'s left boundary to lane `i`. Both are **capacity-1** channels.
  Capacity 0 (unbuffered) deadlocks: both neighbors call `Publish()` (send)
  before either calls its halo method (receive), so with capacity 0 each
  send blocks waiting for the other lane's receive — but that lane is
  itself blocked in its own `Publish()` send at the same moment. Capacity 1
  lets both sends land in the one-deep buffer immediately, so both lanes
  then proceed to receive what was just buffered.
- **Deadlock-freedom, in two sentences.** Every channel has exactly one
  producer and one consumer per step and is drained every step (empty at
  every step boundary, starting with step 0), so given "both sides publish
  before either reads halos," every send always finds a free slot and every
  receive always finds its value — meaning all lanes complete step *s*
  given step *s−1* complete, by induction over supersteps, for any `count`
  and independent of scheduler-thread count. A blocked send can never form
  a cycle because the consumer it is waiting on is never itself waiting on
  that same send.
- **Why not an OS-thread barrier.** Goroutines multiplex onto a small pool
  of scheduler worker threads (the M8 scheduler); a `pthread_barrier_t`-style
  barrier blocks *threads*, so with more lanes than worker threads it wedges
  the scheduler (a lane parked at the barrier can starve a still-running
  lane sharing its thread). Channel receives park *goroutines*, not
  threads, so they compose correctly with the M:N scheduler by construction
  — the same reasoning that ruled out a blocking transport call on a
  goroutine elsewhere in the runtime.

## Comptime specialization (the comptime leg)

`count` is a `comptime` parameter on `Partition`, so `Partition` itself
monomorphizes: distinct symbols per distinct `count` value used in a
program, pinned in the IR as `goo_pkg__lanes__Partition__n<value>` (e.g.
`goo_pkg__lanes__Partition__n2`, `goo_pkg__lanes__Partition__n4`), checked
by `make lanes-monomorphize-ir-pin` (deduped — exactly one `define` per
distinct count, no LLVM auto-uniquify `.1` duplicate escaping).

**Honest reading, not overstated:** the per-lane **body closure passed to
`Run`** is an ordinary runtime function value — it does *not* monomorphize
per `count`. Only `Partition` itself (and the width arithmetic inside it)
is comptime-specialized. Comptime parameters also do not propagate
transitively: `Run` cannot forward a comptime `count` argument into some
inner comptime function on your behalf, and a comptime argument must
itself be a compile-time constant at the call site (a runtime `int`
variable holding the count you want is rejected: `argument to comptime
parameter 'count' must be a compile-time constant`).

## Limits (non-goals)

Verbatim-consistent with the design spec's "M1 does NOT deliver" list, plus
the concrete behavioral quirks discovered while building it:

- **No general borrow-checking of arbitrary Goo code** — only the
  `lanes.Partition`/`lanes.Run`/`*Lane` shape is checked (Option A; the
  general case is the downstream generalization this milestone seeds).
- **No language-level `lanes`/`parallel for` construct** — library-only,
  ordinary Goo functions and types.
- **No 2D (or higher-dimensional) partitions** — `Partition` splits a flat
  `[]float64` along one axis only.
- **No non-shared-memory transport** — the only transport is in-process
  channels over one shared backing array; RDMA/NNG transports are later
  milestones designed to slot in behind the same `Publish`/`Halo` API, not
  present today.
- **No reductions** — the stencil's update rule is deliberately
  associativity-safe (each cell depends only on its own and immediate
  neighbors' OLD values, never a running sum split across lanes). A
  workload with a cross-lane floating-point reduction could legitimately
  reorder additions and produce a merely close-but-not-bit-identical
  answer; that shape is out of scope.
- **No transitive comptime forwarding** — see Comptime specialization
  above.
- **`steps=0` still runs exactly ONE round**, not zero. `Step()` only
  increments and checks *after* a round's `Publish`/halo/compute has
  already run (the body's canonical loop is publish-first, a do-while
  shape), so `Run(p, 0, body)` executes one full round before `Step()`
  returns false and the loop breaks. This is documented in `lanes.go`'s
  `Step` doc comment and verified directly (see Degenerate-shape probes
  below) — do not assume `steps=0` is a no-op.
- **A non-divisor `count` silently truncates the tail.** `Partition`'s
  width is `floor(len(arr)/count)`; if `count` does not evenly divide
  `len(arr)`, the trailing `len(arr) % count` elements are outside every
  lane's view and are never written by `Run`. This is documented on
  `Partition` in `lanes.go`, not enforced as a compile error.
- **`Lane.boundary` is fixed at `0.0`** (the Dirichlet constant used by edge
  lanes' missing-neighbor halo) — there is no setter in v1. A non-zero
  boundary would need a new `Partition`/`Run` parameter, out of scope for
  the frozen v1 API surface.
- **A comptime `count` argument must itself be a compile-time constant** —
  a literal or `const`, never a plain runtime variable, even if that
  variable's value happens to be known — see the Comptime specialization
  section.

## The race story

There is **no automated data-race gate** wired into `verify-core` for
`goostd/lanes`. This is a deliberate, documented decision, not a silent
gap — full detail, rationale, and a manual re-check runbook live in
`docs/lanes-race-runbook.md`. Summary: helgrind runs cleanly against the
compiled binaries but is **structurally blind** to goroutine-vs-goroutine
races under Goo's M:N `ucontext` scheduler (two cooperatively-scheduled
goroutines on one worker thread look like one thread to a pthread-based
detector), so a green helgrind run proves nothing here — wiring it as a gate
would be actively misleading. The real soundness mechanism is the four
compile-time proofs above; the runtime backstop that DOES run in
`verify-core` is a deterministic differential check — the stencil golden
(`-O0` and `-O2`) and the parallel soak below both assert **bit-for-bit
identical** output against a serial reference on every run. That is a
probabilistic empirical signal (a real race would eventually surface here
as nondeterminism), not a soundness proof; treat any mismatch as a
confirmed bug, never treat repeated green runs as proof of race-freedom.

## Measured soak numbers (session-measured)

`make stencil-parallel-probe` (part of `verify-core`) runs an
8-lane, CPU-bound `goostd/lanes` workload (comptime `count=8`) against the
identical computation run serially with no lanes/goroutines involved, and
asserts bit-for-bit identical output; wall-clock and CPU utilization are
reported informationally only (never a pass/fail threshold — an opt-in
`LANES_BENCH_ASSERT_SPEEDUP` env var exists for manual local gating).

**Session-measured 2026-07-11, 32-core machine:** ~7.5x speedup (8-lane vs.
serial wall-clock), ~794% CPU utilization on the 8-lane run, **bit-identical
output** on both variants. Numbers will vary by machine and by run
(wall-clock ratios are inherently noisy); only the bit-identical-output
assertion is load-bearing in `verify-core`.

## Degenerate shapes (verified, not committed as goldens)

Three edge shapes were exercised as throwaway probes during M1's final
regression pass (not committed — the golden set already covers the
representative case at count=4):

- **`count=1`** — a single lane owns the whole array; both `edgeL` and
  `edgeR` are true, so `Publish`/`HaloLeft`/`HaloRight` never touch a real
  channel (both halos always read the fixed `0.0` boundary). Ran and
  matched the serial reference bit-for-bit.
- **`count == len(arr)`** (width 1) — each lane's owned tile is exactly one
  cell, so that cell is simultaneously the lane's only owned cell *and*
  its own left/right boundary cell. Ran and matched the serial reference
  bit-for-bit.
- **`steps=0`** — verified `Run` still executes exactly one round (an
  instrumented lane body counted rounds and printed `1`), and that one
  round's output matches a serial reference computed for exactly one
  round, confirming the documented publish-first do-while behavior rather
  than assuming steps=0 is inert.
