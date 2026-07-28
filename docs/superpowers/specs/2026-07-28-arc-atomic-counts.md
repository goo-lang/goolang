# The ARC reference count is atomic — measured, not assumed

**Date:** 2026-07-28
**Status:** implemented and gated. Closes ADR 0002's open question "whether
release is threaded (atomic counts) or per-goroutine".
**Follows:** the object header (PR #251), `2026-07-28-arc-parameter-boundary.md`.

## Why the question could not be deferred

The parameter boundary gives `go f(p)` a +1 transfer, because a goroutine
outlives the caller's frame. From that point two goroutines can hold a
reference to one object, and a data race on the count frees a live one.

## What the codebase says

Three facts, each from the code rather than from an assumption.

| Question | Evidence | Answer |
|---|---|---|
| Do two goroutines run at the same moment on two cores? | `concurrency.c:110` spawns `goo_default_thread_count()` OS threads — `GOMAXPROCS` or NCPU, capped at `GOO_MAX_OS_THREADS` (16). | **YES** |
| Does a goroutine move between OS threads? | Workers take work from a SHARED ready queue. `scheduler_main_loop` republishes a yielded goroutine to that queue, and any worker may then take it. | **YES** |
| Does the hot numeric kernel allocate in its loop? | `goostd/lanes/lanes.go:81` — each lane sizes `scratch` on first call and reuses it every round. | **NO** |

Goroutines are ucontext coroutines (`concurrency.c:44`), which reads like a
single-threaded design and is not one: they are multiplexed onto real OS
threads.

The first answer makes a non-atomic count racy. The second removes every scheme
keyed on "the OS thread that owns this object", because that thread changes.
The third says the cost misses the place where Goo competes.

## The decision

**Atomic, uniformly, with one code path and no dependency on any analysis.**

| Operation | Order | Why |
|---|---|---|
| `goo_retain` | `__ATOMIC_RELAXED` | You must already hold a valid reference in order to retain one, so the increment itself synchronises nothing. Relaxed here is correct, not lazy. |
| `goo_release` | `__ATOMIC_RELEASE` on the `fetch_sub` | Puts every write this goroutine made to the object before another goroutine's destruction of it. |
| destruction | `__ATOMIC_ACQUIRE` fence when the previous value was 1 | Pairs with every other goroutine's release, so their writes are visible before the memory is reused. |
| `goo_obj_refcount` | `__ATOMIC_RELAXED` load | ADVISORY ONLY. With two goroutines the answer is stale the instant it returns. A stronger order would imply a guarantee the value cannot carry. |

This is the shape Rust's `Arc` and Boost's `shared_ptr` use.

A `static_assert` on `__atomic_always_lock_free(sizeof(uint64_t), 0)` holds the
count lock-free. A count that fell back to `libatomic` would take a LOCK inside
every retain, which is a performance defect nobody would find by reading the
file. Verified in the object code: `goo_retain` compiles to a single
`lock addq`, `goo_release` to a single `lock xadd`, and the archive has no
undefined `__atomic_*` symbol.

## The defect this fixed, which PR #251 introduced

`goo_release` read the count, compared it against 0, and then decremented —
three separate steps. Two goroutines could each observe 1 and each decrement,
which is a double free.

Measured before the fix, with 8 threads doing 100,000 retains each:

```
Row 12: concurrent retain does not lose an update
  (count is 146667, expected 800001 — 653334 updates lost)
free(): double free detected in tcache 2
```

**82% of the increments vanished, and the process then aborted.** The single
`__atomic_fetch_sub`, tested on the value it returns, removes the lost update
and the time-of-check-to-time-of-use window together.

## The cost, measured

Uncontended, single-threaded, pinned to one core, 50 million retain/release
pairs, three runs agreeing to within 0.01 ns:

| | ns per retain+release pair |
|---|---|
| atomic | **10.17** |
| non-atomic, like-for-like | 2.69 |
| delta | **7.48** (3.78x) |

The baseline is two `noinline` calls through a `volatile` pointer, so both
sides pay one call and one load-modify-store and the only difference left is
the lock prefix. The first version of this benchmark reported the baseline at
0.000 s because the compiler deleted the loop, which would have made the ratio
meaningless — the benchmark now refuses to print a number if either loop takes
under 0.01 s.

That cost lands on allocation-heavy code that escape analysis cannot prove.
Measurement 4 of ADR 0002 says it does not land on the numeric kernel, because
that loop does not allocate.

## Why not biased counting

Biased counting keeps an owner field and takes a fast non-atomic path when the
current owner is the one touching the object. It is faster in the steady state,
and it is a large project. An error in it produces non-deterministic corruption,
which is the hardest defect class to find.

The same tie-breaker settled the three ARC boundaries before this one: prefer
the option whose wrong answer costs time over the option whose wrong answer
corrupts memory. Atomic-always cannot be wrong.

**This choice forecloses nothing.** Biased counting is a pure optimisation above
atomic-always, and the header's `reserved` field is 8 bytes and unused, so an
owner field and a "shared" flag already have somewhere to live. Note one
constraint it inherits from the table above: a goroutine migrates between OS
threads, so any future owner must be the GOROUTINE, never the OS thread.

## Gates

- `obj_header_test` rows 12-14: concurrent retain, balanced retain/release, and
  a race to the final release. 14 rows, 29 checks.
- `obj-header-tsan`: the same test under ThreadSanitizer, because the ordinary
  build cannot see a race and a lost update is not guaranteed to show on any
  one run. Skips loudly if clang is absent.
- Both are in `verify-core`.

The TSan gate was confirmed to have teeth: with `goo_retain` made non-atomic
again it reported `data race ... in goo_retain` and a `heap-use-after-free`.

## Still open

- Cycles. A cycle leaks by design under reference counting.
- Map-held references (`2026-07-28-arc-map-held-references.md`) and slices of
  pointers, neither implemented.
- No codegen emits retain or release yet, so no program pays this cost or gets
  any benefit from it.
