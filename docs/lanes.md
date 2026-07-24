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

## M2: numeric kernels & collectives

M2-B2 adds three API surfaces on top of the frozen M1 `Partition`/`Run`/
`*Lane` core: a comptime-specialized stencil kernel and two deterministic
BSP collectives. Nothing above this section changed — `Partition`, `Run`,
`Own`, `ID`, `Publish`, `HaloLeft`, `HaloRight`, and `Step` are exactly as
documented above, and the four compile-time guarantees still apply
unmodified to any `lanes.Run` body, M2 calls included (see the reject
fixture discussion below). The normative design record for this milestone
is `docs/superpowers/specs/2026-07-23-p6-lanes-m2-b2-design.md`.

### The three new API surfaces

```go
func StencilStep(ctx *Lane, comptime radius int, coeffs []float64)

func (l *Lane) AllReduceSum(local float64) float64
func (l *Lane) AllReduceMax(local float64) float64
```

`StencilStep` runs one BSP round of a `(2*radius+1)`-tap stencil over the
calling lane's own tile: it publishes and receives `radius` boundary cells
per side, computes `out[i] = Σ coeffs[k] * in[i+k-radius]` (k ascending,
into the lane's own scratch buffer), and copies the result back before
returning. It is a package function, not a `Lane` method — comptime
parameters are still walled on methods
(`src/types/type_checker.c`, "not yet supported on methods"), so a method
form was never on the table for this milestone. `radius` is `comptime`, so
each distinct radius used in a program monomorphizes its own instance
(`goo_pkg__lanes__StencilStep__n<radius>`); `coeffs` is a slice, not a
`[2*radius+1]float64` array, because fixed-size arrays cannot cross a
function signature boundary in this dialect. `len(coeffs)` must equal
`2*radius+1` exactly, checked at entry and a panic (not a silent
truncation/pad) on mismatch. A second explicit guard rejects `radius`
wider than the lane's own tile width (`radius > len(ctx.own)`): the
sub-exchange protocol below only reaches one neighbor tile deep, so a
wider radius would need cells beyond what any single halo exchange can
supply, and `StencilStep` panics rather than reading out of bounds.

`AllReduceSum`/`AllReduceMax` are plain `*Lane` methods (no comptime
parameter, so the method wall is irrelevant to them): every lane
contributes its own `local` value, and every lane — including the
contributing ones — receives back the identical combined result. `Sum`
covers residual/L2-style norms; `Max` covers max-norm convergence checks
like the Jacobi loop below. There is no user-supplied combiner in this
milestone: a closure combiner would reopen the determinism argument
per-user-supplied-function, so only these two fixed, order-pinned
operations are exposed.

### The determinism contract

Both collectives combine per-lane partials in **fixed lane-ID order** —
lane 0's contribution, then lane 1's, then lane 2's, and so on — never
arrival order, regardless of which lane's goroutine happens to reach the
call first. This is what `allReduce`'s doc comment in `lanes.go` calls
load-bearing, not style: float addition is not associative, so a
combination in arrival order would produce a different rounding sequence,
and therefore a different bit pattern, than the fixed ID-order fold on the
same inputs. What is promised: bit-identical results across repeated runs
and across scheduler interleavings, for a **given** lane count. What is
explicitly **not** promised: bit-identity across *different* lane counts —
changing the lane count changes the tiling, which changes both the
per-lane partial sums and the order they're folded in, so a different
count is expected to (and, per the M2-B2 design doc, is understood to)
land on a different rounding sequence even for numerically-equivalent
input. Do not compare `AllReduceSum`/`AllReduceMax` output across two
`Partition` calls with different `count` arguments and expect equality.

### Collectives are whole-round events

The protocol rule, documented rather than compiler-enforced (like M1's
Publish → Halo → compute order): a collective call is a barrier exactly
like `Step()`'s parking, so every lane must call the same collective the
same number of times, in the same program position, every round — all
lanes or none. Breaking this symmetry deadlocks the missing lane's peers,
which block forever waiting on a partial/result that never arrives.

The concrete failure mode this rules out is breaking out of a convergence
loop *before* the round's collective call instead of after it — one lane
deciding "I've converged, skip the `AllReduceMax` this round" while its
neighbors still call it. The canonical shape, `examples/lanes_jacobi_probe.goo`,
gets this right: each round calls `lanes.StencilStep`, computes this
lane's local max delta, calls `ctx.AllReduceMax(local)` to learn the
round's *global* delta, and only **after** that call checks the tolerance
and `break`s:

```go
lanes.StencilStep(ctx, 1, coeffs)
local := /* this lane's max |own[i] - before[i]| */
delta := ctx.AllReduceMax(local)
mine = mine + 1.0
if delta < tol {
	break
}
if !ctx.Step() {
	break
}
```

Every lane executes exactly the same sequence of collective calls up to
and including the round that trips the tolerance check, because the break
decision is made only after all lanes have already rendezvoused inside
`AllReduceMax` for that round — the collective itself is unconditional per
round, only the loop exit is conditional, and it is placed after the
barrier that every lane already passed together.

### The radius-r halo sub-exchange protocol

M1's boundary channels are capacity-1 and carry one `float64` per send, so
a `StencilStep` with `radius > 1` cannot exchange its whole halo in a
single Publish/Halo round — it runs `radius` sequential **sub-exchanges**
instead. Sub-exchange `k` (0-indexed) ships this lane's `k`-th-from-edge
cell in each direction (`own[width-1-k]` rightward, `own[k]` leftward) and
receives the corresponding cell from each neighbor, buffering the results
into `haloBufL[k]`/`haloBufR[k]` (edge lanes fill from the fixed `0.0`
Dirichlet boundary instead of reading a channel). Each sub-exchange is
textually M1's proven both-sends-then-both-receives capacity-1 pattern —
the same shape `Publish`/`HaloLeft`/`HaloRight` use for radius 1 — so the
per-step deadlock-freedom argument documented under "The protocol" above
(every channel single-writer/single-reader and drained every use, so a
blocked send can never wait on a consumer that is itself waiting on that
same send) applies independently to each `k`. No lane starts sub-exchange
`k+1` before finishing sub-exchange `k`, because its own two receives for
`k` are what let it proceed past `k` at all — so the deadlock-freedom
argument composes across the `radius` sub-exchanges by straightforward
induction on `k`, the same way M1's per-step argument composes across BSP
rounds by induction on the round number.

### Boundary peel: branch-free interior, dispatching boundary loops

`StencilStep`'s tap-accumulation sweep is three loops, not one loop over
the whole tile. The interior loop (`radius <= i < w-radius`, where `w` is
the tile width) accumulates every tap with a plain `own[i+k-radius]` read
and no bounds dispatch at all, because for every `i` in that range
`i-radius >= 0` and `i+radius <= w-1` hold by construction — no cell the
interior loop ever touches can fall into a halo buffer, so the dispatch
isn't merely never-taken, it's structurally absent from that loop's body.
The two boundary loops (`i < radius`, and `i >= max(radius, w-radius)`)
keep the original per-cell dispatch (`idx < 0` → `haloBufL`, `idx >= w` →
`haloBufR`, else `own[idx]`) unchanged, because only cells within `radius`
of a tile edge can ever actually read a halo buffer. The right loop's
start is clamped to `max(radius, w-radius)` rather than always `w-radius`
so that when `radius <= w < 2*radius` (a tile narrower than two radii) the
two boundary loops meet or abut exactly, with neither a gap nor a
re-walked/double-computed cell. This is a pure control-flow restructuring
— every loop's accumulation is textually identical to the original
per-cell body, `acc := 0.0` then `k`-ascending `acc = acc + coeffs[k]*v` —
so it changes nothing about the bit-identity contract the golden
differentials pin. The payoff is unrolling: a branch-free, fixed-trip-count
interior loop is what a comptime-fold optimizer pass can actually unroll
(see Comptime fold below); a loop whose body still branches on a runtime
condition on every iteration is a much harder unrolling target regardless
of how constant its trip count is.

### Buffer discipline

Every kernel buffer `StencilStep` touches is per-lane and long-lived, never
allocated inside the BSP round loop: `ctx.scratch` (tile-width-sized) is
allocated once as the first action of each lane's goroutine, and reused for
every round of that lane's whole lifetime. `ctx.haloBufL`/`ctx.haloBufR`
are different: `Run` cannot size them, because it doesn't know `radius`
(that's a `StencilStep` argument, not a `Run`/`Partition` parameter), so
they are grown lazily — `StencilStep` allocates each the first time it
observes `len(haloBuf) < radius`, and never shrinks or reallocates it
again for a smaller subsequent radius call on the same lane. The reason
this discipline matters, not just style: v1 Goo has no systematic memory
reclamation (malloc with no GC and no ownership-based freeing — see this
repo's CLAUDE.md memory-model note), so a per-round `make()` inside a
kernel that runs `steps` rounds would leak `steps * width` `float64`s per
lane over a long-running kernel. One allocation per buffer per lane for
the entire `Run` call is the only discipline available without arena
support in this kernel's hot path.

### Comptime fold: what changed under the hood

Prior to this milestone's Task 4b, a `comptime` parameter like
`StencilStep`'s `radius` produced a distinctly-named, monomorphized
function symbol per distinct value used in a program
(`goo_pkg__lanes__StencilStep__n2`, etc.) — but *inside* that specialized
body, `radius` still arrived as an ordinary runtime function argument, an
`i64` value LLVM could not treat as a compile-time constant. Task 4b
(`src/codegen/function_codegen.c`) changed that: a monomorphized instance's
comptime parameter values are now folded directly into the instance's body
as LLVM constants, not passed as runtime arguments. "Comptime-specialized"
is therefore now literally true of the generated code, not just of the
symbol name — every loop bound and tap-index computation derived from
`radius` inside a specialized `StencilStep` instance is a compile-time
constant to LLVM, which is what makes full unrolling (see below) possible
in the first place. Measured before/after on the radius-2 instance: pre-fold
the specialized body had exactly 3 `fmul double` (one multiply-accumulate
site per tap loop, each still gated by a runtime trip count); post-fold the
same body has 16 `fmul double` — the interior and left-boundary tap loops
are fully unrolled into straight-line code, and even the one loop LLVM
still leaves as a genuine loop now branches on a literal trip count
(`icmp eq i64 %postfix_inc325, 5`) rather than a runtime value.

### Honest vectorization status

**Vectorization is not achieved, and this document does not claim SIMD.**
The `lanes-kernel-ir-pin` Makefile gate compiles
`examples/lanes_stencilstep_r2_probe.goo` at `-O2` and checks the
specialized `goo_pkg__lanes__StencilStep__n2` body for **unroll evidence**
— at least 5 `fmul double` instructions (16 measured) — rather than the
originally-intended vector-type predicate (any `<N x double>` in the
function body), because the vector-type predicate fails outright, both
before and after the Task 4b comptime fold: 0 matches, scoped to the
function body, either way. The fold did not cause this and cannot fix it:
the root cause is that every tap access (`own[...]` and `coeffs[k]`) still
carries a `goo_bounds_check` call, and that runtime function has no
`readnone`/`speculatable` attributes on its declaration — LLVM must treat
an opaque, possibly-side-effecting call conservatively on every loop
iteration, which blocks SLP/loop vectorization regardless of how constant
the trip count is. The honest summary: comptime specialization delivers
full unrolling with compile-time-constant trip counts — a real, measured,
IR-verified payoff — but not vectorization; closing that gap requires
attributing `goo_bounds_check` (or an equivalent inlined/attributed bounds
check) so LLVM can prove it side-effect-free and hoist it out of the
vectorization-blocking path. That is out of scope for this milestone and
is not something a library-level change to `goostd/lanes` can fix on its
own.

### Updated Limits (M2 additions)

Everything in "Limits (non-goals)" above still holds unmodified for M1
surfaces. M2 adds:

- **Still no 2D (or higher-dimensional) partitions** — `StencilStep`
  operates along the same single flat-array axis `Partition` does.
- **Still no generic `[T]` kernels** — `StencilStep` is `float64`-only,
  comptime-specialized on `radius` alone. A `Numeric`-constrained generic
  kernel (`StencilStep[T Numeric]`) is a real follow-up candidate but
  needs its own type-system design (instance-time operator checking); see
  `docs/spmd-harness.md`'s Limits section for the same open item on the
  SPMD side.
- **No user-supplied reduction combiners** — only `AllReduceSum` and
  `AllReduceMax` exist; a closure-combiner API is explicitly out of scope
  (see The three new API surfaces above for why).
- **No bit-identity across different lane counts** — see The determinism
  contract above; this is a property of float non-associativity, not a
  gap that could be closed by a smarter combine order.
- **`boundary` is still fixed at `0.0`** for every edge lane's
  missing-neighbor halo read, in `StencilStep`'s sub-exchanges exactly as
  in M1's `HaloLeft`/`HaloRight` — no new setter was added.
- **`radius` must not exceed the lane's own tile width** — enforced by an
  explicit panic (`lanes.StencilStep: radius must not exceed the lane
  tile width`) rather than silently reading out of bounds or wrapping;
  see The three new API surfaces above.

## M2-B1: far transport

M2-B1 adds a second way to run the exact same lane bodies: `RunFar`,
`Run`'s process-boundary sibling. Nothing in M1 or M2-B2 above changed —
`Partition`, `Run`, `StencilStep`, `AllReduceSum`/`AllReduceMax`, and every
compile-time guarantee still apply unmodified inside a `RunFar` body; the
milestone's whole job was making the *edges* of a lane's world reach across
a process boundary without the body noticing. The normative design record
is `docs/superpowers/specs/2026-07-24-p6-lanes-m2-b1-design.md`.

### The RunFar contract

```go
func RunFar(p Partitioned, steps int, rank int, world int, urlBase string, body func(ctx *Lane)) []float64
```

`world` cooperating OS processes (ranks `0..world-1`), each partitioning
its own **rank-local span** and calling `RunFar` with the identical
`steps`, `world`, and `urlBase`, and the identical **per-rank lane count**
(`p.count`, via each rank's own `Partition` call) — equal-count-per-rank is
a documented contract, enforced empirically by the bit-identity probes
rather than by a runtime check. Global Dirichlet boundary reads (the fixed
`0.0` a `HaloLeft`/`HaloRight`/`StencilStep` sub-exchange returns for a
missing neighbor) now occur only at the two true world edges — rank 0's
left and rank `world-1`'s right — not at every rank's local edges, since a
rank's local edges that face another rank are bridged across the wire
instead.

### Socket topology and pump architecture

Every rank **listens** on its right halo boundary and **dials** its left,
so each boundary URL (`urlBase + ".halo." + rank`) has exactly one owner
and process start order can't deadlock setup (`far.Dial` is
async-retry — `NNG_FLAG_NONBLOCK` — so a dialer just keeps trying until its
listener appears). Collectives get their own socket set: rank 0 listens
one socket per remote rank (`urlBase + ".coll." + r`), every other rank
dials its single socket to rank 0.

Two goroutines per far edge — a send-pump and a recv-pump — bridge a
rank's outermost lane's ordinary cap-1 boundary channel to that far
socket. A send-pump drains the channel into `far.SendF64`; a recv-pump
feeds `far.RecvF64` into the channel. This is the whole trick: from a lane
body's point of view, `Publish`/`HaloLeft`/`HaloRight`/`StencilStep`
still just push and pull on a cap-1 Go channel exactly as they do under
`Run` — the pumps are invisible to the body, and interior lanes (the ones
with neighbors on the same rank) are wired exactly as `Run` wires them,
untouched. Two consequences follow directly from that invisibility:
delivery is FIFO end-to-end (a cap-1 channel is FIFO by construction, NNG
pair sockets are FIFO per direction, and a pump does no reordering, so the
composition is FIFO), and the M1 cap-1 deadlock-freedom argument carries
over unmodified — a send-pump's only job is draining its slot into
`far.SendF64`, which buffers locally and does not wait on the remote, so a
blocked send still can never wait on a consumer that is itself waiting on
that same send.

The send-pump's quit path is drain-on-quit, not drop-on-quit: by the time
`RunFar` signals quit, `runCore` has already returned, so every lane
goroutine (the only possible producer into `sendL`/`sendR`) has already
joined — whatever the kernel's last `Publish()` enqueued (at most one
value, cap-1) is already sitting in the channel by the time the quit arm
runs, with nothing left that could arrive later and be missed. So the quit
arm does one non-blocking drain (an inner `select` with a `default`) before
signaling done: if a value is there, forward it; if not, the pump was
already caught up. Phrasing it as producer-quiescence-plus-bounded-channel
rather than "drain first in source order" is deliberate — it makes
drop-freedom independent of whichever policy the outer `select`'s arm scan
happens to use, order-independent by construction rather than by
incidental scheduling.

### The collective bit-identity protocol

Cross-rank `AllReduceSum`/`AllReduceMax` extend the same fixed-ID-order
combine M2-B2 established, just across the wire instead of across
goroutines: every non-zero rank forwards its **raw, uncombined partial**
to rank 0 (no per-rank pre-combining — a rank owning multiple lanes still
sends one raw value per lane, in lane-ID order), rank 0 folds every
partial — its own lanes' plus every remote rank's — in strict **global
ID-order** (`global lane id = rank * lanesPerRank + local id`, ascending),
then broadcasts the single combined result back down every coll socket.
Folding on any other axis — arrival order, or a rank pre-combining its own
lanes before forwarding — would, for non-associative float addition, land
on a different rounding sequence and therefore different bits; the
`far_collective_probe` fixture's dataset is deliberately non-associative
(`1e16` absorbs `1.0`) specifically so an arrival-order or
per-rank-pre-combine bug cannot pass by accident.

### Teardown order and failure model

Setup errors and mid-run transport errors **panic** with an explicit
message — a torn transport is unrecoverable for lockstep BSP, the same
process-fatal story as a panicking lane body. A rank whose peer legitimately
finished and closed sees `"far: closed"` from its own still-blocked
`far.RecvF64` — string-matched by the recv-pump as the ordinary, expected
end-of-run signal (not an error path) and forwarded as a clean pump exit.

Teardown itself runs a **marker-exchange protocol on every far socket a
rank holds** (halo and collective alike) before any `far.Close`: each side
pushes a marker value (`0.0` — its FIFO position is the signal, not the
number) as that socket's truly-final send, then blocks receiving its
peer's marker back, before either side proceeds to quit its pumps and
close. This exists because the local send-pump drain above only proves a
send has left *this rank's* enqueue step — it says nothing about whether
the *remote* rank has actually received it. NNG's own documentation for
`nng_close` is explicit that closing a socket with data still in transit
"will likely lead to loss of that data" and that there is **no automatic
linger or flush**: a message can sit in the local send buffer
(`NNG_OPT_SENDBUF`, `FAR_BUF_DEPTH` below) well after `far.SendF64`
returns, and NNG's close path unconditionally frees whatever is still
queued there. A rank's final halo `Publish()` or rank 0's final collective
broadcast are exactly the sends this could silently drop.

State the envelope honestly: completing the marker exchange on a socket
proves — via NNG's per-direction FIFO delivery — that this rank has now
received every message its peer ever sent on that socket, *including* the
peer's own marker, so any real payload sent upstream of a received marker
is proven delivered. Bit-identity on actual data is therefore airtight;
the residual is Two-Generals-shaped and scoped **only to the marker token
itself** — with no linger primitive on either end of a symmetric,
unconditional-push handshake, neither side gets a mathematical guarantee
that its *own* last send (the marker) was received before it proceeds to
close. What the exchange narrows that gap to: a full network round trip
(send marker, peer receives it and independently sends its own, this rank
receives that) has provably elapsed before either side reaches its close,
the same "wait a while first" mitigation NNG's own docs recommend, now
triggered by a real cross-rank event instead of a blind sleep. Should that
residual ever fire, it is never silent corruption — the failure mode is
always **loud**: a panic (an unexpected `far.SendF64`/`far.RecvF64`
failure) or a probe timeout (`scripts/far-probe.sh`'s 30s-per-rank wrapper
below), never a quietly-wrong bit. NNG has no flush-on-close primitive at
all (their docs, not an omission on this side), which is exactly why
closing this residual for good is a transport-level job — the vtable's
future AIO-based transport (`src/runtime/far_transport.h`'s ops interface)
is the structural fix, not a Goo-level protocol layered on top of a
transport that fundamentally can't confirm delivery of its last message.

### The envelope

Single-machine, multi-process is proven: every far probe runs `world`
real OS processes on one machine over `ipc://` sockets
(`scripts/far-probe.sh`), each under a 30-second timeout (a teardown hang
is a probe FAIL, not a wedged gate). Multi-machine (`tcp://` in place of
`ipc://`) is a documented future runbook, not exercised by any gate today.
Wire format is one message per value: 8-byte little-endian IEEE-754
`float64`, native on every target this compiles for today and documented
now for a future cross-machine transport where native byte order can't be
assumed.

Two hard numbers bound a process's far footprint
(`src/runtime/far_transport.c`): `FAR_MAX_SOCKETS` is **64** sockets per
process — a rank needs at most 2 halo sockets plus `world-1` (rank 0) or 1
(every other rank) collective sockets, so 64 covers any plausible
single-machine world with margin. `FAR_BUF_DEPTH` is **128** messages of
send/recv buffer per socket, configured explicitly (not inherited as an
NNG default) so "a send never blocks on remote progress" is a property
this transport sets on purpose; per the same comment, it bounds **per-rank
lane count for far runs to <= 128** — a run with more lanes per rank than
that on a single far socket exceeds the envelope this milestone measured
and gated.

### The gates table

| Gate | What it pins |
|---|---|
| `far-transport-test` | C unit test: shim ABI, FIFO ordering, the buffering envelope, the `"far: closed"` split |
| `far-transport-asan` | Same C unit test under AddressSanitizer (leak detection off — v1's documented no-GC memory model; corruption checks stay fully active) |
| `far-shim-probe` | End-to-end `far` shim: listen+dial to self over `ipc://`, send/recv both directions, both error-union branches |
| `far-halo-probe` | 2-rank radius-1 halo exchange, bit-identical to the serial reference (near mode pins `Run` against the same reference first) |
| `far-stencil-r2-probe` | Radius-2 sub-exchange ordering survives the wire, bit-identical |
| `far-collective-probe` | Cross-rank `AllReduceSum`/`AllReduceMax` global ID-order combine, bit-identical on non-associative data |
| `far-jacobi-probe` | The capstone: distributed Jacobi convergence — final field AND iteration count bit-identical to the serial tiled reference, run twice |

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
