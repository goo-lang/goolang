# goostd/lanes race-detection runbook

P6 M1 Task 8. `goostd/lanes` (Partition/Run/Lane — `goostd/lanes/lanes.go`)
gives every lane a compile-time-disjoint view of the backing array plus a
BSP halo-exchange protocol. This document is the authorized fallback for
Task 8's Step 1: **no automated data-race detector is wired into
`verify-core`** for this runtime path. It exists so that fact is a
documented, deliberate decision — not a silent gap — and so a future
session with better tooling knows exactly where to pick this up.

## Why there is no automated race gate

The M1 design's *Risks & open questions* section pre-authorized a documented
manual runbook as the fallback if a race detector proved non-viable. The
Task 1 spike (`docs/superpowers/specs/2026-07-11-p6-lanes-m1-spike-findings.md`
Section 3) tested that directly and found something more specific than
"unavailable": **helgrind is installed, runs to completion, and reports
zero errors — while being structurally blind to the exact class of bug it
exists to catch.**

The spike compiled a probe with two goroutines each incrementing a shared
`*int64` one million times with no synchronization, then ran it under
helgrind:

```
$ ./bin/goo /tmp/spike_race.goo -o /tmp/spike_race_bin
$ valgrind --tool=helgrind --error-exitcode=99 /tmp/spike_race_bin
3000000                                                  # NOT 4000000 -> lost updates -> race is REAL
ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 1621 from 14)   # exit 0 -- helgrind saw NOTHING
```

The printed `3000000` (instead of the race-free `4000000`) proves updates
were genuinely lost — a real race occurred — at every `GOMAXPROCS` value
tested (1 and 8). Helgrind's `ERROR SUMMARY` was `0 errors` every time.

**Root cause:** the M8 scheduler is an M:N `ucontext` design — goroutines
are cooperatively swapped via `swapcontext` onto a small pool of
`GOMAXPROCS` worker pthreads (`src/runtime/concurrency.c:41-42, 60-89`).
Helgrind's unit of concurrency is the OS thread. Two goroutines
cooperatively scheduled onto the same worker pthread are, to helgrind, ONE
thread — same-thread accesses are by definition never races — so it cannot
see goroutine-vs-goroutine races at all. This is inherent to any
pthread-based detector (helgrind, DRD) run against a ucontext user-thread
scheduler; it is a **false negative**, not noise, and no `--suppressions`
file can fix blindness (suppressions only silence findings a tool DID
produce).

Wiring a helgrind gate into `verify-core` on top of this would be actively
harmful: a green check-mark next to "race detector: PASS" would certify
race-freedom the tool structurally cannot observe. That is worse than
having no automated gate at all, so none is wired.

## What the compile-time proofs guarantee (and do not)

The real soundness mechanism for lanes is **compile-time rejection**, not
runtime detection: `include/lane_ownership.h` / `src/types/lane_ownership.c`
implement four proof obligations as a single per-function AST walk, wired
from `type_check_program` after body-checking completes. Each obligation
has a golden reject fixture under `tests/golden/reject/`, exercised by
`make test-golden-reject` (81/0 in `verify-core`):

| Obligation | Guarantee | Reject fixture |
|---|---|---|
| 1 — move-on-Partition | `lanes.Partition(arr, count)` MOVES `arr`; any later read of `arr` in the same function is a compile error. | `tests/golden/reject/partition_move_reject.goo` (also: `partition_selfref_reject.goo` pins that a read-before-write on the RHS of `data = clone(data)` rejects before the write revives the name). |
| 2 — consume-on-Run | `lanes.Run` CONSUMES its `Partitioned` argument; a second `Run` on the same binding is use-after-consume. | `tests/golden/reject/partition_rerun_reject.goo` |
| 3 — single-goroutine attribution | Inside a `lanes.Run` body, the `*Lane` ctx and every name derived from it (`ctx.Own()`, and copies of it) may not be passed to another goroutine via `go`, as a call argument or as a captured name of the launched literal. | `tests/golden/reject/partition_escape_reject.goo` |
| 4 — view-capture rule | A `lanes.Run` body literal may not capture a `Partitioned` binding, a moved source array, or a name lane-derived from an enclosing `Run` body. | `tests/golden/reject/partition_capture_reject.goo` |

**What this does NOT cover** (the documented under-reject envelope —
`include/lane_ownership.h`'s "UNDER-WALKED STATEMENT KINDS" comment is the
source of truth; summarized here, do not let this summary drift from that
file):

- The walk does not descend into `AST_IF_LET_STMT`, `AST_SELECT_STMT`,
  `AST_ARENA_BLOCK`, `AST_UNSAFE_STMT`, `AST_COMPTIME_BLOCK`, or a local
  `AST_CONST_DECL` at all. A `lanes.Partition` move or a moved-name read
  occurring inside one of these is silently invisible to obligation 1.
- The obligation-3 body walker is narrower still: taint does not propagate
  through `AST_MULTI_ASSIGN` targets or a plain-assignment `ExprStmt`
  (`x = ctx.Own()`), and it does not descend into `IF_LET`/`SELECT`/
  `ARENA`/`UNSAFE` or into nested non-launched `FuncLit` bodies.
- A bare, unbound `lanes.Partition(...)` statement (result discarded, not
  assigned via `:=`/`var`) records NO move at all — only the
  `VarDeclNode` path recognizes a Partition call as a move.
- Every tracking table in the walk is flat and **shadow-unaware**: a
  shadowed name reusing a moved/lane-derived identifier is the one known
  false-reject/false-accept-adjacent surface (documented as the Option A
  scope boundary, not a bug to fix under M1).
- This is not a general borrow checker — it is one self-contained,
  per-function AST walk shaped specifically around `lanes.Partition`/
  `lanes.Run`/`*Lane` call shapes. Code outside that shape gets zero
  extra scrutiny.

All of the above is a deliberate **under-reject** direction (the walk fails
closed only in the sense that it never crashes or rejects code it wasn't
built to reason about — it does not fail closed in the sense of catching
every violation). It is a real, load-bearing soundness mechanism for the
shapes it does cover; it is not a proof that no lanes program anywhere can
race.

## Deterministic differential backstop that DOES run in verify-core

Two things in `verify-core` provide an empirical (not exhaustive) check
that the lanes runtime path behaves deterministically, i.e. race-free in
practice on the paths they exercise:

1. **`examples/lanes_stencil_probe.goo`** (part of `test-golden` and
   `test-golden-o2`, so it runs at both -O0 and -O2 on every `verify-core`):
   a 1D heat-equation stencil computed two ways — a plain serial reference
   loop with no goroutines, and a `goostd/lanes.Run` BSP halo-exchange
   parallel version — and asserted **bit-for-bit identical**, element by
   element, no epsilon (`examples/lanes_stencil_probe.expected.txt`:
   `IDENTICAL` / `0.999985` / `0.027771` / `0.196381`). The update rule is
   deliberately associativity-safe (each cell depends only on its own and
   its immediate neighbors' OLD values, never a cross-lane reduction), so
   bit-identity is achievable and meaningful, not a coincidence.
2. **`stencil-parallel-probe`** (this task, also in `verify-core`): an
   8-lane CPU-bound workload built via `lanes.Partition`/`lanes.Run` (comptime
   `count=8`), asserted bit-identical against the same computation run
   serially with no lanes/goroutines involved at all, plus informational
   wall/CPU/speedup reporting (see its Makefile recipe comment for the
   opt-in `LANES_BENCH_ASSERT_SPEEDUP` hard gate).

**Why this is a backstop and not a proof:** a genuine data race in the
lanes runtime path (a scheduling-order-dependent lost update, exactly like
the spike's `3000000 != 4000000` injection) would eventually surface here
as **nondeterminism** — a mismatch between the parallel and serial outputs
on some run. But "eventually" is doing real work in that sentence: a race
that only manifests under a rare interleaving may pass any single
`verify-core` invocation and still be present. This is a probabilistic
empirical signal, not a soundness proof — treat a failure here as
conclusive evidence of a bug, but do not treat repeated passes as proof of
race-freedom.

## Manual runbook: re-checking with future tooling

Three things a human (or a future session) can do that are deliberately
NOT automated in `verify-core`, because they are either known-blind, not
yet built, or too noisy to gate on:

### 1. Re-run the spike's helgrind invocation directly

To confirm the blindness finding still holds (e.g. after a valgrind
upgrade, or after a scheduler change):

```
./bin/goo examples/lanes_stencil_probe.goo -o /tmp/stencil_helgrind_check
valgrind --tool=helgrind --error-exitcode=99 /tmp/stencil_helgrind_check
```

Expected today: exit 0, `ERROR SUMMARY: 0 errors ...`, output identical to
`examples/lanes_stencil_probe.expected.txt`. That "0 errors" tells you
NOTHING about race-freedom per the finding above — it is expected whether
or not a race exists. Only re-run this as a smoke check that helgrind still
runs cleanly against this binary (no crash, no new noise flood), not as
evidence of correctness.

### 2. What a TSan-instrumented build would require

LLVM's `-fsanitize=thread` (ThreadSanitizer) is the only realistic dynamic
detector that could work here, but it is NOT a drop-in flag against this
runtime: TSan's happens-before model is built around pthread/futex
primitives, and it has no built-in understanding of `swapcontext`-based
user-space fiber switches — without help it would see the same "coalesce
cooperatively-scheduled goroutines into one execution context" problem as
helgrind, or worse, misattribute a same-worker context switch as a false
race. Making TSan work would require:

- Building the runtime and codegen output with `-fsanitize=thread` wired
  through `Makefile`'s compile/link flags for the runtime archive and
  emitted object code.
- Teaching TSan about the scheduler's context switches via its fiber
  annotation API (`__tsan_switch_to_fiber` / `__tsan_create_fiber`,
  called from `src/runtime/concurrency.c` at each `swapcontext` boundary)
  so it treats each goroutine as its own logical thread of execution
  rather than folding cooperatively-scheduled goroutines into their shared
  worker pthread.
- Re-validating against the spike's own decisive probe (the blatant
  unsynchronized double-increment) to confirm TSan now DETECTS it, before
  trusting it as a gate for anything else.

This is out of M1 scope (it is a codegen/runtime change, not a lanes-package
change) and is recorded here as the concrete post-v1 option.

### 3. The spike's race-detecting differential method: repeat-and-diff

The empirical method the spike used to prove the race was real in the
first place — run the workload many times and look for nondeterminism —
is also the most honest manual check available for `goostd/lanes` today.
Paste this to run the stencil probe 20 times and flag any run whose output
differs from the golden expectation:

```
for i in $(seq 20); do
  ./bin/goo examples/lanes_stencil_probe.goo -o build/race_check_bin >/tmp/race_check_compile.$i 2>&1 || { echo "run $i: COMPILE FAIL"; cat /tmp/race_check_compile.$i; continue; }
  ./build/race_check_bin > /tmp/race_check_run_$i.out
  if diff -u examples/lanes_stencil_probe.expected.txt /tmp/race_check_run_$i.out >/dev/null; then
    echo "run $i: IDENTICAL (matches golden)"
  else
    echo "run $i: MISMATCH -- possible race, investigate immediately"
    diff -u examples/lanes_stencil_probe.expected.txt /tmp/race_check_run_$i.out
  fi
done
```

Recompiling every iteration (rather than reusing one binary) also
exercises compile-time nondeterminism, not just runtime scheduling
nondeterminism. For a runtime-scheduling-only variant, compile once and
loop only the `./build/race_check_bin > ...` / diff lines — that isolates
whether the SAME binary produces different answers across runs, which is
the more direct race signal.

Any mismatch here is conclusive: investigate as a real bug (start with
`lane_ownership.c`'s under-reject envelope above — that is the first place
a real race would slip through the compile-time proofs).

## When to revisit

Revisit this runbook (and re-open the question of an automated gate) when
either of these becomes true:

- The runtime grows a **pthread-per-goroutine mode** (one OS thread per
  goroutine instead of the M:N ucontext scheduler) — even as a debug-only
  build configuration — because that would make helgrind's thread-grained
  view actually correspond to the program's real concurrency, closing the
  blindness gap without any new tooling.
- A **TSan fiber-annotation integration** (Section 2 above) is built and
  validated against the spike's decisive probe. At that point a
  `stencil-race-probe` gate (compile the fiber-annotated build, run under
  TSan, assert clean) becomes a meaningful `verify-core` addition — mirror
  it on `arena-valgrind-probe`'s SKIP-loudly-if-absent discipline
  (`Makefile`, `arena-valgrind-probe` target) so it degrades gracefully on
  machines without the instrumented build.
