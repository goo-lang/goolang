# ARC header feasibility — auditing what an object header would break

**Date:** 2026-07-28
**Status:** findings, plus one fix the audit forced.
**Follows:** ADR 0002 (accepted), ADR 0003 (accepted), the phase-2 scoping spike.

**Why this pass.** ADR 0002 chose reference counting. Every ARC implementation
needs somewhere to keep the count, and `goo_alloc` returns raw `malloc` memory
today with no header at all. Codegen assumes the returned pointer *is* the
object address. That assumption had to be audited before any code was written.

## The design that requires the fewest changes

Put the header **before** the payload, the way `malloc` itself does:
`goo_alloc(n)` allocates `HDR + n` and returns `base + HDR`. Every caller keeps
seeing the object address, so no codegen change is needed for layout, and no C
shim notices.

That is the cheap part. The audit is about what still breaks.

## What breaks, in order of severity

### 1. There were THREE allocation doors, not two — and two of them disagreed

The phase-2 spike said `goo_alloc` (`runtime.c:53`) and `goo_realloc`
(`runtime.c:75`). That was incomplete. `goo_slice_alloc` (`runtime.c:925`)
called **raw `calloc`**.

And a slice's backing store was *grown* by `goo_slice_append` through
`goo_realloc`. So a buffer entered the heap by one door and was resized by
another. That is harmless while both are bare libc calls — and it is heap
corruption the moment a header exists, because `goo_realloc` would offset a
pointer that never had one.

**Fixed in this change.** `goo_slice_alloc` now goes through `goo_alloc` and
zeroes explicitly. Three properties of the old `calloc` are preserved on
purpose, and the third was unwritten:

- zeroed memory, for Go's zero-value guarantee;
- never NULL and never the shared `goo_zerobase` sentinel, because `bytes` is
  at least 1 — a zero-length slice keeps the unique non-NULL data pointer that
  `goo_slice_get`'s null-slice panic convention depends on;
- **an overflow check**, which `calloc` did for free. `calloc(count, size)`
  fails when the product overflows; a plain multiply wraps, under-allocates,
  and hands back a buffer the caller writes past.

Gated by `scripts/alloc_doors_probe.sh`, now in `verify-core`.

### 2. Arena memory has no malloc header and cannot be individually freed

`goo_arena_alloc` bump-allocates inside a block (`arena.c`). An arena object is
not a `malloc` result, so a release that reached one and called `free` would be
undefined behaviour.

ADR 0002's answer is that escape analysis elides ARC exactly where the arena
applies: an arena-eligible value is *proven* block-local, so no retain or
release is emitted for it. The two mechanisms then never meet.

**The hazard that answer does not cover:** an arena-allocated value passed to a
function. The callee cannot see where it came from, and a conservative callee
emits release on it. Elision is a caller-side property; the callee needs one
too. This is the single most important open question for ARC and it is not
solved here.

### 3. The `goo_zerobase` sentinel has no header either

Every zero-size allocation aliases one static byte. ARC must never write a
count through it. `goo_free` already special-cases it, so the precedent and the
shape of the fix both exist — but every new header read or write needs the same
guard.

### 4. Pointers stored as integers are invisible to ARC

`codegen.c:736`, `:1384` and `:1405` store allocated pointers into map slots
via `LLVMBuildPtrToInt`. A count cannot be maintained through an `i64` that the
compiler no longer treats as a pointer. Map-held references need their own
decision.

### 5. Alignment

The header must be a multiple of 16 to keep `max_align_t` guarantees for the
payload. `arena.c` already uses `GOO_ARENA_ALIGNMENT == 16`, so 16 is the
consistent choice.

## Known exemptions, listed so they are decisions rather than oversights

These allocate with raw libc on purpose and must NOT be routed through
`goo_alloc`:

| Site | Why |
|---|---|
| `arena.c:36` | the arena's own block backing store — it *is* the region |
| `platform.c:35` | private platform buffers, never a Goo value |
| `io.c:96,152,205` (+2 `realloc`) | private I/O buffers, never a Goo value |
| `concurrency.c`, `channels.c` | scheduler and channel internals |

`scripts/alloc_doors_probe.sh` therefore scopes itself to `runtime.c`, and says
so. If Goo-visible allocation ever moves into one of these, the probe and this
table get updated together.

## What this change actually delivers

Not ARC. One prerequisite, which is independently correct: **Goo-visible heap
memory now enters and leaves through one door.** That is worth having whether
the next step is ARC, a region tag, or neither, and the asymmetry it removed
was a real latent bug rather than a hypothetical one.

## Recommended next increment

Answer the callee-side elision question (item 2) before writing any ARC code.
It decides whether ARC and `arena { }` can coexist at all, and no amount of
header plumbing helps if the answer is no.

## What was NOT established here

- No header was added. No retain or release is emitted anywhere.
- No decision on atomic against per-goroutine counts. ADR 0002 left it open.
- Nothing about cycles.
- No measurement of ARC's cost. `bench/stencil` is the probe that would show it,
  and ADR 0002 measurement 4 predicts it cannot matter there because that loop
  does not allocate.
