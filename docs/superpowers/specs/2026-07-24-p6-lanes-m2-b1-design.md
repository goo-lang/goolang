# Phase 6 lanes M2-B1: NNG far transport for distributed halo exchange — design

Date: 2026-07-24. Status: approved (user, this date). Successor to
`2026-07-23-p6-lanes-m2-b2-design.md` (M2-B2, merged as PR #209 → 2938783).
Decomposition position: step 4 of the Phase 6 ladder (0 spike → 1 comptime
metaprogramming → 2 shared-mem lane harness → 3 comptime-specialized
kernel → **4 far-transport**). B1 was sequenced *after* B2 by the recorded
decision; that sequencing is now satisfied.

Ledger note: the handoff's "stale PRs #144/#142 need disposition" item is
already resolved — #144 MERGED (ccache/bison build), #142 CLOSED
(superseded by the arc-18 receiver-kind work, 8d012cd + 56e9d7e). The
handoff update accompanying this milestone records that.

## Goals

1. Prove the lanes BSP protocol across a real process boundary: N OS
   processes on one machine exchange halos over a genuine transport (NNG),
   with the frozen `Partition`/`Run`/`Lane` surface and every M1/M2-B2
   protocol path (Publish/HaloLeft/HaloRight, radius-r sub-exchange,
   collectives) byte-for-byte unchanged.
2. Plant the transport-agnostic runtime interface the roadmap requires (a
   C-side ops vtable), so RDMA/AIO transports later replace NNG behind an
   unchanged surface.
3. Keep the evidence discipline: every claim probe-gated in
   `make verify-core` on any machine — which forces the NNG dependency to
   be vendored and pinned, not discovered.

## Scope decisions (user-approved, 2026-07-24)

| Decision | Choice | Rejected alternatives |
|---|---|---|
| Execution envelope | Multi-process, one machine (`ipc://` / `tcp://127.0.0.1`); multi-machine is a future runbook, not a gate | Multi-machine day one (not probe-gateable on one box); in-process transport swap only (proves no process boundary) |
| API surface | Remote-backed channels: lane fields stay `chan float64`; new `lanes.RunFar` wires process-edge boundaries | Explicit Send/Recv transport API (forks the just-frozen kernel protocol); process-transparent `Run` (hides a behavioral fork in a frozen function) |
| Data model | Rank-local spans: each rank allocates/initializes only its contiguous span; harness assembles outputs | Root scatter/gather (second protocol surface — scope creep); replicated array (nothing distributed but compute) |
| Collectives | In scope: cross-rank AllReduce so the Jacobi capstone runs distributed end-to-end | Halo-only (capstone couldn't run distributed — weaker evidence) |
| NNG policy | Vendor pinned stable 1.x under `third_party/`, static lib, sha256-recorded | System NNG + opt-in gate (default gate loses the flagship evidence); target installed 2.0.0-dev (moving-target dev API) |

## Approach: A (pump goroutines + minimal shim), chosen over B/C/D

Four candidates were compared:

- **A — pump goroutines + minimal NNG shim** (chosen): lane channel fields
  stay ordinary cap-1 channels; Goo-level pump goroutines bridge process-edge
  channels to shim calls; the C shim dispatches through a transport vtable.
- **B — new remote channel kind in the channel runtime**: purest
  "distributed channel", but modifies the hot send/recv/close dispatch of a
  heavily probed core file and must answer close/nil/select semantics for
  remote channels immediately. Highest regression risk, no milestone-visible
  gain.
- **C — NNG AIO wired into scheduler wakeups**: solves worker-pinning the
  runtime demonstrably does not suffer from yet (channel-blocked goroutines
  already park in `pthread_cond_wait`, pinning their worker — measured fact,
  `src/runtime/channels.c`). Deferred, not dropped: the vtable is the seam
  where AIO lands later.
- **D — hybrid** (runtime-encapsulated far-channel constructor + C pump
  threads): ranked strongest on encapsulation and worker hygiene, but
  requires a third caller class in `goo_sched_block_begin`'s deadlock
  accounting (mandatory — teardown would otherwise false-abort). The user
  chose A for now; D remains the natural refactor if far channels are ever
  promoted to a general runtime primitive.

A's deciding properties: **zero modification to existing C runtime paths**
(all code is additive), and the frozen protocol source never learns about
far-ness. A's honest costs: pump logic is authored in the vendored-Goo
dialect (workaround-laden — see lanes.go's header list), and pumps blocked
in `cond_wait`/`nng_recv` pin scheduler workers exactly as channel-blocked
goroutines do today (status quo, not a regression).

## Architecture (five additive components)

### 1. Vendored NNG

`third_party/nng-1.12.0/` from the pinned stable release tarball
(v1.12.0, latest stable at design time, 2026-07-24), sha256 recorded in
the Makefile. Built by a Makefile target via cmake into a static
`libnng.a`, linked into the binary link line. First vendored third-party
dependency in the repo; cmake becomes a build dependency; ccache keeps
rebuilds cheap. The installed /usr/local nng 2.0.0-dev is deliberately NOT
used (dev-snapshot API drift).

### 2. `far` shim package (transport-agnostic interface)

C side: `src/runtime/far_transport.c` + rows in
`src/types/shim_signatures.c`. Internally all five entry points dispatch
through a static ops vtable (`far_transport_ops`: listen/dial/send/recv/
close function pointers); NNG pair sockets are implementation #1. The
vtable — not the shim names — is the roadmap's transport-agnostic
interface; a future AIO/RDMA transport replaces the ops struct with no
surface change.

Goo-visible surface (deliberately tiny, lanes-internal envelope —
blocking send/recv only; no select/close-propagation semantics promised):

- `far.Listen(url string) !int` — bind + listen; handle or error.
- `far.Dial(url string) !int` — async-retry dialer (NNG default), so
  process start order does not matter.
- `far.SendF64(sock int, v float64)` — blocking send; panics on hard
  transport failure (unrecoverable for lockstep BSP — see Error handling).
- `far.RecvF64(sock int) !float64` — blocking recv; distinct error values
  for local-close vs transport failure (see Error handling).
- `far.Close(sock int)` — closes the socket; unblocks a blocked RecvF64.

New shim ret kind: `!int` result (mirrors the existing
`SHIM_RET_STRING_RESULT` pattern).

Wire format, documented now for future cross-machine use: one message per
halo cell / partial — 8-byte little-endian IEEE-754 float64.

### 3. `lanes.RunFar` (vendored Goo, additive package function)

```go
func RunFar(p Partitioned, steps int, rank int, world int,
            urlBase string, body func(ctx *Lane)) []float64
```

- `p` partitions the **rank-local** span (each rank allocates and
  deterministically initializes only its own contiguous piece of the
  global array).
- Interior wiring is `Run`'s, verbatim. The only difference is at process
  edges: rank r>0's lane 0 gets `edgeL=false` with `sendL/recvL` backed by
  pumps bridging to the left boundary socket; mirrored for rank r<world−1's
  last lane. Global Dirichlet boundaries (0.0) exist only at rank 0's lane
  0 (left) and rank world−1's last lane (right).
- Lane bodies, `Publish`/`HaloLeft`/`HaloRight`, `StencilStep`'s radius-r
  sub-exchange: unchanged. FIFO is preserved end-to-end (cap-1 channel →
  single pump → pair socket → single pump → cap-1 channel), which is what
  the sub-exchange protocol requires; the k+1-gated-on-k receive argument
  carries over unchanged.
- Validation up front (panic with explicit message, StencilStep style):
  `world >= 1`, `0 <= rank < world`, non-empty `urlBase`.

Socket topology (for world = W): one bidirectional pair socket per rank
boundary — `<urlBase>.halo.<r>` between ranks r and r+1 (r listens, r+1
dials) — plus one collective socket per non-zero rank —
`<urlBase>.coll.<r>` (rank 0 listens, rank r dials). Total (W−1) halo +
(W−1) coll sockets.

### 4. Pump goroutines (halo path only)

Per far edge, two pump goroutines spawned by RunFar:

- **send-pump**: `select` over the data channel and a cap-1 `quit`
  channel; data → `far.SendF64(sock, v)`; quit → exit.
- **recv-pump**: loop of `far.RecvF64(sock)` → send into the data channel;
  exits cleanly when RecvF64 yields the local-close error.

Termination deliberately uses only matrix-verified constructs (`select`,
buffered channels, error unions + `e.Error()`). Comma-ok receive and
closed-data-channel receive semantics are NOT load-bearing anywhere.

### 5. Cross-rank collectives (no pumps — direct shim calls)

`allReduce` grows a far branch behind new **additive** `Lane` fields
(rank, world, coll socket handle, isFar flag — existing field shapes
untouched). Blocking shim calls happen directly on lane-0 goroutines
(status-quo blocking semantics, acceptable under A).

**Bit-identity invariant** (the load-bearing decision): ranks never
pre-combine. Rank r>0's lane 0 forwards its `count` **raw per-lane
partials in local-ID order** over its coll socket. Rank 0's lane 0
flat-combines: its own local partials in ID order, then rank 1's partials
in order, then rank 2's, … — i.e. exactly the global lane-ID order. That
accumulation is instruction-for-instruction the sequence the in-process
scan performs, so for the same total lane count the distributed result is
bit-identical (float addition is not associative; any per-rank
pre-combine would change the association and break the differential
gate). The result rides back down the coll sockets; each rank's lane 0
distributes locally via the existing `results[]` channels. The barrier
property is inherited: every lane still blocks on its result delivery.

## Data flow summary

One far-edge Publish: lane → cap-1 send channel → send-pump →
`far.SendF64` → pair socket → remote recv-pump → cap-1 recv channel →
remote `HaloLeft`/`HaloRight`.

Deadlock-freedom extends M1's cap-1 argument: a Publish completes as soon
as the cap-1 slot frees; the send-pump's only job is draining that slot;
`nng_send` buffers without waiting on the remote. No send ever waits on
remote progress, so the two-party publish cycle cannot form. Receive-side
blocking is identical to in-process halo blocking.

## Teardown (no leaked goroutines)

The BSP protocol is symmetric lockstep: by the end of the last round every
sent message has been consumed — no in-flight residue by construction. At
join time (existing `done` drain + new pump-done channels) both pumps per
edge are idle-blocked. RunFar then, in order:

1. Sends on each send-pump's `quit` channel → select takes the quit arm →
   pump exits.
2. Calls `far.Close` on each socket → the blocked `RecvF64` returns the
   local-close error → recv-pump catches it and exits cleanly.
3. Drains all pump-done channels before returning.

Nothing outlives RunFar. A teardown hang is a red gate (probe timeouts).

## Error handling

1. **Setup** (`Listen`/`Dial`): error unions; RunFar catches and panics
   with a positioned message (`lanes.RunFar: dial <url>: <err>`). Fatal by
   design, consistent with the documented lanes panic story.
2. **Mid-run transport failure**: unrecoverable for lockstep BSP → pumps
   panic loudly, never hang silently. The recv-pump distinguishes clean
   teardown from mid-run death by **distinct error values** from the C
   shim: `"far: closed"` (NNG_ECLOSED after a local `far.Close`) vs
   `"far: recv failed: <nng error>"`. The pump's catch compares via
   `e.Error()` — no dependence on select-default or comma-ok.
3. **Protocol misuse** (bad rank/world/urlBase): up-front panics with
   explicit messages.

## Gates (all in `make verify-core`; self-contained via vendored NNG)

| Probe | Proves |
|---|---|
| Task-0 spike probe (permanent) | Vendored goostd source can import + call a shim package — the design's one flagged assumption |
| C unit test (`make test`) | far_transport roundtrip: listen/dial/send/recv/close + ECLOSED-vs-failure error split |
| far-halo-probe | 2 ranks × 2 lanes, radius-1 stencil: assembled output bit-identical to single-process 4-lane `Run` on the same global array |
| far-stencil-r2-probe | radius-2 sub-exchange survives the wire (FIFO ordering) |
| far-collective-probe | Distributed AllReduceSum/Max bit-identical to in-process; non-associative data (T1 trick) so combine-order deviation shows in the bits |
| far-jacobi-capstone | M2-B2 Jacobi convergence distributed 2×2: same final field AND same iteration count as in-process 4-lane, bit-identical |
| Determinism probe | Capstone twice → byte-identical outputs |
| Hygiene | Every probe timeout-wrapped (hang = red); ASan on the C unit test; valgrind scoped to far_transport.c's own allocations + NNG shutdown |

Launcher: `scripts/far-probe.sh` — spawns the world over `ipc://` URLs in
a `mktemp -d`, rank-tags stdout, assembles, diffs. Fixtures read
rank/world/urlBase from `os.Args` (verified) parsed with vendored
`strconv`.

No parser/lexer/grammar changes anywhere in B1 — the grammar tripwire is
trivially stable.

## Risks & flagged assumptions

1. **Task-0 spike (plan's first task)**: nothing in goostd today imports a
   shim package from vendored source; PR #213 Task B fixed shim dispatch on
   canonical import paths, so the machinery plausibly works, but it is
   unverified. Fallback if it fails: fix that checker path as part of B1
   (it is core to the stdlib model regardless).
2. **Vendored-dialect authoring risk**: pump/wiring code in lanes.go must
   respect the documented language-gap workarounds (per-iteration rebind
   inside classic 3-clause `for`, no combined `var`, etc.). Mitigation:
   TDD with reject fixtures, and the pump bodies kept minimal.
3. **First third-party vendored build**: cmake integration, static-lib
   linking, ccache behavior. Mitigation: isolate in one Makefile target;
   the C unit test gates it before any Goo-level work.
4. **NNG pair-socket buffering assumption**: the deadlock argument assumes
   `nng_send` does not block awaiting remote progress for our message
   sizes/counts. The C unit test pins this (send N messages before any
   recv; must not block) — evidence, not assumption.

## Out of scope (deferred, recorded)

- Multi-machine execution (future runbook; wire format documented now).
- AIO/RDMA transports (vtable seam planted; approach C deferred).
- Approach D's runtime-encapsulated far-channel constructor (natural
  refactor if far channels become a general primitive).
- Scatter/gather data distribution (push/pull), non-zero Dirichlet
  boundaries, 2D partitions, generic `[T]` kernels — all per earlier
  milestone records.
- Halo message batching (per-cell messages preserved for protocol
  identity; batching is a perf follow-up with no determinism impact).

## Amendments (2026-07-24, execution round)

Append-only: the sections above are the original design record, approved
on this date. The final whole-branch review (`.superpowers/sdd/final-review.md`,
Important 1) found the shipped implementation diverged from two of them in
ways not otherwise recorded in this file — both divergences are correct
engineering (one driven by a concrete bug the original premise missed, one
a deliberate hygiene-tooling substitution) but the normative spec must say
so. Nothing above this section is rewritten.

### (a) Teardown protocol as shipped

The **Teardown** section above (the 3-step quit → close → drain protocol,
resting on "by the end of the last round every sent message has been
consumed — no in-flight residue by construction") is **superseded**. Task
6's review (I1) disproved that premise at the NNG layer: the existing
local `done`/pump-done drain only proves *this rank's own enqueue step*
didn't race `Close` — it says nothing about whether the *remote* peer
actually received what was sent, and NNG's own `nng_close(3)` documentation
is explicit that there is no automatic linger or flush, so a message can
still be sitting in the local send buffer (`FAR_BUF_DEPTH`) when
`far.Close` frees it. Two concrete instances existed: a rank's final halo
`Publish()` and rank 0's final collective broadcast.

The shipped protocol adds a **symmetric marker-exchange phase on every far
socket a rank holds** (both halo sockets and every collective socket)
before any `far.Close`: each side pushes a marker value (`0.0` — its FIFO
position is the signal, not the number) as that socket's truly-final send,
then blocks receiving its peer's marker back, before either side proceeds
to quit its pumps and close. Every rank's pushes (both halo edges, then
every coll socket) are issued before any of that rank's receives — an
overlapped, not per-socket-sequential, ordering, chosen purely for latency
over the still-deadlock-free sequential alternative (sequential pushes
would chain a rank's wait proportional to its distance from the nearest
world boundary; overlapped pushes resolve every socket's round trip in one
hop regardless of chain length).

**Corrected premise, stated honestly:** completing the marker exchange on
a socket proves — via NNG's per-direction FIFO delivery — that *this* rank
has now received every message its peer ever sent on that socket,
including the peer's own marker, so any real payload sent upstream of a
received marker is proven delivered. It does **not** give either side a
mathematical guarantee that *its own* last send (the marker itself) was
received before it proceeds to close: with no linger primitive on either
end of a symmetric, unconditional-push handshake, that residual is
Two-Generals-shaped, and no finite protocol eliminates it outright. The
accepted residual is therefore honestly scoped to the marker token only —
never the payload — and is **always loud** on failure: a panic (an
unexpected `far.SendF64`/`far.RecvF64` failure) or a
`scripts/far-probe.sh` timeout, never a silently-wrong bit. Full account —
including the "a full round trip has now provably elapsed before close"
mitigation this buys, the same "wait a while first" `nng_close(3)` itself
recommends, now triggered by a real cross-rank event instead of a blind
sleep — lives in `docs/lanes.md`, "Teardown order and failure model".

### (b) Hygiene gate: ASan substitutes for valgrind

The **Gates** table's hygiene row above still commits to "valgrind scoped
to far_transport.c's own allocations + NNG shutdown". As shipped, that row
is **AddressSanitizer, pinned to clang** (`far-transport-asan`, run against
the C unit test; leak detection off — the v1 no-GC memory model makes
leak-checking meaningless, so memory-corruption checks (overflow/UAF/
double-free) stay fully active), not valgrind. Rationale (plan
self-review, `docs/superpowers/plans/2026-07-24-p6-lanes-m2-b1-nng-far-transport.md`):
ASan catches the same class of bug for the C unit's scope; a valgrind
far-run can still ride the existing `arena-valgrind-probe` pattern
post-merge if wanted. This is a substitution, not a drop — the row stays
gated in `make verify-core`.

### (c) Send-pump drain-on-quit

The **Pump goroutines** section above does not mention a quit-time drain.
As shipped (T4 review), a send-pump's quit arm performs a **one-shot
non-blocking drain** of its channel (`select` with a `default` arm) before
acking done, forwarding any single value the last `Publish` left queued.
This is safe because by quit time every lane goroutine has already joined
(no further producer can enqueue), so the drain is
producer-quiescence-plus-bounded-channel, not a "keep draining until
empty" loop — and it is correct independent of whichever policy the outer
`select`'s arm-scan uses, by construction rather than by incidental
scheduling order.
