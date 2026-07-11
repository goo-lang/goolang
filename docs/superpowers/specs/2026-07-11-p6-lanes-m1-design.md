# Phase 6 Milestone 1 — Compile-time-proven SPMD lanes (design)

**Date:** 2026-07-11
**Status:** Design approved; implementation plan pending.
**Track:** Phase 6 (scientific compute / SPMD lanes) — see
`docs/2026-07-08-v1-roadmap.md` Post-v1 Phase 6, and the direction memory
`goolang-parallel-lanes-comptime`.

## Definition

Milestone 1 delivers a **partition primitive whose disjointness the Goo
compiler proves at compile time**, demonstrated by a 1D-stencil SPMD
harness. The safety guarantee is the deliverable; the numerics are its
carrier. Concretely, at the end of M1:

- `lanes.Partition(arr, count)` splits a slice into `count` disjoint,
  exclusively-owned per-lane views, and the compiler **rejects** any
  program that (a) touches the source slice after partitioning, (b) lets a
  lane view escape into a second goroutine, or (c) lets a lane body reach
  any view other than its own.
- A 1D heat-equation stencil runs `count` lane goroutines through a fixed
  number of BSP steps, exchanging boundary values by per-neighbor channel
  handshake, and produces a result bit-identical to a serial reference.
- `count` and the derived tile width are **comptime** values, so the
  per-lane body monomorphizes per partition shape (the comptime leg).

M1 does NOT deliver: general borrow-checking of arbitrary Goo code (that is
the downstream generalization this milestone seeds), a language-level
`lanes`/`parallel for` construct (library-only), 2D partitions, or any
non-shared-memory transport (RDMA/NNG are later milestones behind the same
API).

## Why compile-time proof is the spine (not a later milestone)

Goo's differentiator is memory safety. Concurrent disjoint mutation of
shared memory is exactly the case where "safe by construction of the
library API" is not a language guarantee — it holds only until someone
writes a lane by hand, and the compiler proves nothing. So the primary
deliverable is the compiler check, and the runtime race detector is
demoted to its correct role: the *test that the compile-time proof is
sound*, not the guarantee itself.

**Tractability — the comptime lever.** A general "do these two mutable
slice views alias?" question is a may-alias problem. But because `count`
and tile width `w = total/count` are **comptime-known constants**, the
disjointness of tile `i` and tile `j` is *constant-interval arithmetic on
compile-time values* — decidable, no general aliasing solver required.
This is precisely the banked thesis ("comptime-known partition map → hoist
aliasing invariants into compile-time ownership proofs"), and it is what
makes M1 a milestone rather than a research epic.

## What already exists to build on

- **`src/types/ownership_checker.c`** — a move/borrow-checker skeleton
  (`is_move_operation`, `mark_variable_moved`, `check_borrow_rules`,
  `check_ownership_assignment`, `check_variable_lifetime`,
  `check_function_call_ownership`), with `OWNERSHIP_BORROWED` and the
  `ERROR_USE_AFTER_MOVE` / `ERROR_INVALID_BORROW` diagnostics already
  defined. It is currently **unwired** (zero call sites; dropped from
  `GOO_OBJS` in P5.6). M1 wires it on the partition/goroutine path only.
- **`src/types/param_escape.c` / `block_escape.c`** — wired escape
  analyses that already model **Sink #4: every argument to a `go`-launched
  call escapes** (the #30 goroutine-arg escape work). This is the existing
  mechanism that crosses the goroutine boundary; M1 extends it to carry
  *which* lane view crossed into *which* goroutine.
- The **soundness-siblings** contract between `param_escape` and
  `block_escape` (fail-closed `default:` arms): any new AST shape M1
  introduces must be handled in both, conservatively, or omission stays
  safe. See those files' header comments.
- **Buffered capacity-1 channels** (Component 3's one runtime dependency)
  are verified implemented: `src/runtime/channels.c` carries the full
  `capacity > 0` send/receive path, exercised by spec-conformance fixtures
  (`tests/spec/type_channel.goo`, `tests/spec/stmt_go_select.goo`). No
  hidden runtime prerequisite.
- **Prerequisite gap (found during planning):** comptime parameters are
  currently REJECTED on package functions (`type_checker.c:1018-1020`,
  pinned by `comptime-value-reject-matrix` case 8), so `Partition`'s
  signature requires lifting that wall — with package-qualified
  monomorphization symbols — before Component 1 can compile. The
  implementation plan carries this as its own early task; the matrix case
  is replaced (runtime-arg-across-packages still rejects), not dropped.
  Relatedly, no goostd package uses goroutines, channels, closures, or
  comptime today — `goostd/lanes` is the first, so the plan de-risks that
  combination with a committed fixture package before the real one.

## Architecture

Approach 1 (from brainstorm): pure-Goo `goostd/lanes` package on existing
runtime primitives, no C-runtime changes. Scope decision A: wire the
ownership checker's move/borrow bones only where reachable from
`Partition` and `go`.

### Component 1 — `goostd/lanes` package (Goo source)

Vendored Goo source (comptime params resolve inside it, like
`strings`/`strconv`). Public surface:

```goo
package lanes

// Partition MOVES arr and splits it into `count` disjoint per-lane views.
// count is comptime → the tile width w = len(arr)/count is a compile-time
// constant and the views are fixed-stride. After this call arr is moved:
// any use of arr is a compile error (ERROR_USE_AFTER_MOVE).
func Partition(arr []float64, comptime count int) Partitioned

// Run launches `count` lane goroutines over `steps` BSP iterations, each
// driving `body` with its Lane context, then returns the reassembled
// backing array (tiles are disjoint views into it, so this is in place).
func Run(p Partitioned, steps int, body func(ctx *Lane)) []float64

type Partitioned struct { /* opaque: backing array + count + width */ }

type Lane struct { /* id, steps, own-tile view, halo fields, channels */ }

func (l *Lane) Own() []float64   // the lane's full owned tile (its
                                 // partition), EXCLUDING neighbor halos;
                                 // the tile's own boundary cells are owned
                                 // and writable.
func (l *Lane) Publish()         // send this lane's current boundary values
                                 // to neighbors (cb_push_back).
func (l *Lane) HaloLeft() float64  // blocking recv of left neighbor's right
func (l *Lane) HaloRight() float64 // boundary; edge lanes return the fixed
                                   // Dirichlet boundary (cb_wait_front).
func (l *Lane) Step() bool       // advance BSP step; false after `steps`.
```

### Component 2 — Memory & aliasing contract

Message-passing exchange. The **only** shared memory is one backing
`[]float64`, sliced into `count` disjoint tiles; halo values travel by
channel (a copied `float64`), so there is no shared halo memory.

- **Layout.** `Partition` allocates/takes the backing array and records
  `count` + `w = len(arr)/count`. Lane `i` gets the view
  `arr[i*w : (i+1)*w]`. Tiles are non-overlapping sub-slices; reassembly is
  free (results are already in place). Each lane has two goroutine-local
  `float64` halo fields, populated from channel receives.
- **Rule 1 — tile writes are private and disjoint.** Lane `i` writes only
  within its own tile view; `Own()` hands back only that view. Disjointness
  is structural (non-overlapping index ranges) and, with comptime `count`,
  provable by constant-interval arithmetic.
- **Rule 2 — halo values are transferred, not shared.** A neighbor's
  boundary value arrives as a channel receive and is stored in a local
  field; sender and receiver are ordered by the channel's happens-before
  edge; no memory is aliased between them.
- **Endgame fidelity.** Value-copy-over-channel is message passing; a
  future RDMA/NNG transport replaces the channel with a one-sided put into
  registered remote memory *behind the same `Publish`/`Halo` API*. M1
  proves the ownership discipline any transport must preserve.
- **Decision-log note.** The brainstorm's Q3 chose "one-sided shared
  buffers"; during the Tenstorrent refinement this evolved into the
  FIFO-handshake / message-passing model (Tenstorrent's own
  `cb_push_back`/`cb_wait_front` is a two-sided FIFO, not RDMA one-sided).
  Recorded here so the written record matches the log.

### Component 3 — Handshake step protocol

Per-neighbor channels, fixed `publish → read → compute` order,
goroutine-parking throughout.

- **Topology.** Between adjacent lanes `(i, i+1)`: `rightward[i]` (i→i+1,
  i's right boundary) and `leftward[i]` (i+1→i, i+1's left boundary), both
  **capacity 1**. Capacity 0 (unbuffered) deadlocks (both send before
  either receives); capacity 1 lets both sends complete.
- **One step, per lane:**
  1. `Publish()` — send current boundary: right on `rightward[i]`, left on
     `leftward[i-1]` (edge lanes skip the missing side). Step 0 sends the
     initial-condition boundary; thereafter the previous step's computed
     boundary.
  2. `HaloLeft()` / `HaloRight()` — blocking receives into the halo fields
     (`rightward[i-1]`, `leftward[i]`); edge lanes return the fixed
     boundary.
  3. **Compute** — write every owned cell: interior from `Own()`, the two
     edge cells from the just-received halos (fresh for this step).
  4. `Step()` — increment; false after `steps`.
- **Deadlock-freedom (super-step induction).** Every channel has exactly
  one producer and one consumer per step, so each consumer's receive drains
  its channel every step; channels are empty at each step boundary (step 0
  starts empty). Given empty channels and send-both-before-receive-both,
  every send finds a free slot and every receive finds its value — so all
  lanes complete step *s* given step *s−1*. Adjacent-lane skew is bounded
  under one step by back-pressure, and a blocked send cannot form a cycle
  because the consumer it waits on is never itself waiting on that send.
  Holds for any `count`, independent of scheduler-thread count.
- **Why not an OS-thread barrier.** Goroutines multiplex onto scheduler
  threads (M8); a `pthread_barrier_t`-style barrier blocks *threads*, so
  with more lanes than threads it wedges the scheduler. Channel receives
  park *goroutines* correctly by construction. This is the same hazard as
  a blocking `nng_recv` on a goroutine and is why Approach 1 was chosen.

### Component 4 — Compile-time safety checks (the deliverable)

Wire the ownership checker's move/borrow bones on the partition/goroutine
path. Four proof obligations:

1. **`Partition` moves its argument.** Model the call as a move of `arr`
   (`mark_variable_moved`); any subsequent use of `arr` is
   `ERROR_USE_AFTER_MOVE`. Prevents aliasing the whole through the parts.
2. **Views are pairwise disjoint.** By constant-interval arithmetic over
   comptime `count`/`w`. (Structural for the blessed `Partition`; the
   checker records the split so downstream misuse is caught.)
3. **Each view is attributed to exactly one goroutine.** Extend the
   existing Sink #4 goroutine-escape path to record which lane view escapes
   into which `go` call; a view escaping into two goroutines is
   `ERROR_INVALID_BORROW`.
4. **A lane body reaches no view but its own (view-capture rule).** A lane
   body may touch lane-shared state only through its own `*Lane` context:
   capturing the `Partitioned` value, the pre-partition backing array, or
   another lane's view (directly or via closure capture) into a lane body
   is a positioned `ERROR_INVALID_BORROW`-family diagnostic. This is
   deliberately a *capture/escape* rule — the same machinery as
   obligation 3, checked at the `Run`/`go` boundary — **not** index-range
   analysis of writes. Dynamic out-of-range indexing *through a lane's own
   view* is already a runtime bounds panic by ordinary slice semantics
   (each view is a sub-slice with its own length), and static index-range
   verification of arbitrary lane bodies is general bounds analysis — the
   exact rabbit hole the scope boundary below walls off. Comptime-constant
   indices provably out of a view's range MAY additionally be rejected at
   compile time where the constant-folder already sees them, but that is
   opportunistic, not a proof obligation.

Scope boundary (Option A): these checks run only where reachable from
`Partition`/`go`; general borrow-checking of arbitrary Goo is out of scope
and is the downstream generalization M1 seeds. The runtime race detector
(Component 5, probe 4) is the test that these proofs are sound, not a
substitute for them.

## Testing & acceptance

All gates wired into `make verify-core`. Safety-first ordering.

1. **Compile-time safety reject-probes (primary gate)** — each must
   fail to compile with the named diagnostic (compile-must-fail +
   message grep, the goo-grammar reject-probe discipline):
   - `partition-move-reject-probe`: use of `arr` after `Partition` →
     `ERROR_USE_AFTER_MOVE`.
   - `partition-escape-reject-probe`: a lane view captured into a second
     goroutine → `ERROR_INVALID_BORROW`.
   - `partition-capture-reject-probe`: a lane body capturing the
     `Partitioned` value / backing array / another lane's view →
     view-capture diagnostic (obligation 4).
   If any of these compiles, the milestone fails.
2. **Functional correctness golden** — `stencil_probe`: 1D heat-equation
   stencil, fixed `count`/steps/initial-condition; expected output produced
   by a **serial reference** computation (never hand-written), asserted
   bit-for-bit. Runs at -O0 and -O2.
3. **Comptime specialization IR pin** — `lanes-monomorphize-ir-pin`
   (modeled on `comptime-generic-compose-ir-pin`): assert the IR contains
   distinct specialized lane bodies per comptime `count`.
4. **Runtime race-freedom (soundness test)** — `stencil-race-probe`: the
   stencil under a race detector / helgrind, asserting zero data races —
   catches a bug in the compile-time proof.
5. **Genuine multicore soak** — `stencil-parallel-probe`: CPU-bound
   `count`-partition workload asserting wall-time speedup and ~`count`
   cores busy (the Spike 0 shape), proving parallelism not concurrency.

## Risks & open questions

- **View→goroutine attribution across `go`.** Obligation 3 extends Sink #4
  to carry view identity; the exact representation (tagging the escape
  fact with the partition index) is the riskiest new analysis. **Ordering
  requirement: this is spiked BEFORE the implementation plan's task
  breakdown commits to an approach** — the spike's outcome (representation
  of view identity in the escape fact) shapes the obligation-3 and
  obligation-4 tasks, since both ride the same capture/escape machinery.
  Precedent exists (#30) but not for per-element identity.
- **Diagnostic quality.** Obligations 3/4's capture diagnostics need a
  position and an actionable phrasing; a poor diagnostic here is the kind
  of UX gap the conformance work flagged (method-expression/explicit-inst
  errors). Budget for message-quality, not just detection.
- **Race-detector availability.** Confirm helgrind/TSan works against the
  compiled binaries in the local gate environment — **resolve during the
  obligation-3 spike** (it is a cheap environment probe and decides probe
  4's shape early). If unavailable, the race-probe becomes a documented
  manual runbook (spike discipline) rather than a verify-core gate.
- **`float64` bit-for-bit reproducibility.** The serial reference and the
  parallel run must produce identical bits — the stencil update must be
  associativity-safe (each cell's new value depends only on its own +
  neighbor old values, no cross-lane reduction), which the 1D stencil
  satisfies. A workload with a floating reduction would break this and is
  out of scope.
