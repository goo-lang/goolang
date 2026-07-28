# ADR 0002 phase 2 — scoping the runtime region before building it

**Date:** 2026-07-28
**Status:** findings. No compiler or runtime code changed.
**Follows:** ADR 0002 (accepted), ADR 0003 (accepted).

**Why a findings pass first.** Phase 2 is the largest change proposed anywhere
in the current plan, and this repository has twice been paid back for spiking
before implementing (PR #233 stopped an interface-box implementation that would
have failed; the arena step-0 pass found two pre-existing defects instead of
building on them). It has also, twice today, been paid back for measuring
before asserting.

## What the daemon actually does

`bench/daemon/daemon.goo` under valgrind, which counts every allocation rather
than sampling.

| requests | allocs | frees | bytes allocated |
|---|---|---|---|
| 2,000 | 94,004 | 6,001 | 1,574,240 |
| 4,000 | 188,004 | 12,001 | 3,144,240 |

Exactly linear. Per request: **47 allocations, 3 frees, 785 bytes**. At 200
requests valgrind's own leak summary reports 76,800 bytes definitely lost plus
57,800 indirectly — **673 bytes per request never reclaimed**.

The gap between 785 bytes allocated and the ~1.63 KB of RSS ADR 0002 measured
is malloc bookkeeping: 47 headers per request at 16 bytes each is another
752 bytes. Both numbers are right, and they measure different things.

## Where it comes from

Direct bytes only, so a parent is not counted twice with its children. 200
requests.

| Runtime helper | bytes | share |
|---|---|---|
| `goo_slice_append` (via `goo_realloc`) | 25,600 | 33.3% |
| `goo_strings_split` | 22,400 | 29.2% |
| `goo_error_wrap` / `goo_error_from_string` | 9,600 | 12.5% |
| `goo_string_concat` | 7,800 | 10.2% |
| `goo_map_new_sv` | 4,800 | 6.3% |
| `goo_strings_join` | 3,800 | 4.9% |
| `goo_strings_trim_space` | 1,800 | 2.3% |
| `goo_int_to_string` | 1,000 | 1.3% |

Two things stand out beyond the totals.

**`goo_error_from_string` leaks 106 bytes per request.** The daemon calls
`strconv.Atoi` on `"four"` and `"six"`, so two failed conversions per request
each allocate an error value nothing ever frees. Any Go program that treats a
failed parse as ordinary control flow pays this, and it is 12.5% of this
program's leak.

**Not one leaked byte came from `codegen_emit_alloc`.** Every record traces to
a runtime helper. That is ADR 0002's claim, now confirmed by direct attribution
rather than inferred from an RSS delta.

## The structural finding, and it is better news than ADR 0002 assumed

ADR 0002 says the runtime "has no arena awareness at all", which reads as a
problem spread across the whole runtime. It is not. There are **two doors**:

- `goo_alloc` — `src/runtime/runtime.c:53`, a bare `malloc`
- `goo_realloc` — `src/runtime/runtime.c:75`, a bare `realloc`

Every helper in the table above goes through one of them. Making those two
region-aware reaches all of it.

**Five allocation sites bypass both** and would need separate handling or an
explicit exemption: `src/runtime/io.c` at lines 96, 152 and 205 (plus its two
`realloc` growth sites), `src/runtime/arena.c:36` (the arena's own block
allocator, which must stay a real `malloc`), and `src/runtime/platform.c:35`.

**There is no object header and no reference-counting infrastructure.**
`goo_alloc` returns raw `malloc` memory. The `ref_count` fields that exist live
in `src/runtime/actor_system.c`, which is NOT linked into `bin/goo`.

## The soundness problem phase 2 must answer

Today's arena is **lexical and proven**: a site is arena-allocated only where
`block_escape` proves the value cannot outlive the block.

A region in the runtime is **dynamic extent**: every allocation made while the
region is current lands in it, including allocations that escape. That is
exactly the under-marking class ADR 0002 calls the only bug that can dangle a
pointer.

Three ways to close it, with real trade-offs:

1. **Thread the region through helper signatures.** Sound and explicit. Changes
   roughly 50 runtime function signatures and every call site that codegen
   emits.
2. **Thread-local current region, cleared by codegen around calls whose result
   escapes.** Reuses `block_escape` at call granularity, and touches no helper
   signature. But a helper allocates internal nodes as well as its result — a
   map's buckets share the result's lifetime, so this is only sound if the
   whole call is classified, not the returned pointer.
3. **Region-tag every allocation with a header and check on free.** Sound,
   catches mistakes at runtime rather than compile time, and costs a word per
   object plus a branch per free.

## The question this pass exists to raise

**Phase 2 may not be needed for the daemon at all.**

ADR 0002's decision is reference counting, with escape analysis as the elision
pass. Under ARC, the daemon's per-request garbage drops to zero references at
the end of its iteration and is freed there — with no region involved. A region
only helps a program that opts into `arena { }`.

So the honest ordering question is whether phase 2 is:

- **a prerequisite for ARC** — it is not, on the evidence here. ARC needs an
  object header and retain/release emission. It does not need a region.
- **a way to make `arena { }` complete** — it is exactly that, and ADR 0002
  says so: "Phase 1 makes `arena` honest for the shapes it already claims to
  cover."
- **the thing that moves the 834x** — ARC moves it automatically. A region
  moves it only for code inside an explicit `arena { }`.

The daemon in `bench/` has no `arena { }` in it, on purpose: ADR 0002 chose
that shape to ask "what happens by DEFAULT". A region changes nothing for it
unless the author opts in. **ARC is what changes the default.**

## Recommendation

**Go to ARC (ADR 0002 step 4) and treat the runtime region as optional
follow-on work**, on this evidence:

- The two doors make ARC's allocation-side change small: an object header goes
  in `goo_alloc`/`goo_realloc`, in one place each.
- Both remaining parity targets in ADR 0003 — deterministic memory, and the
  daemon being expressible — are ARC's job, not the region's.
- The region's soundness problem is real and unsolved, and the cheapest sound
  version (option 1) is a ~50-signature change that buys nothing for a program
  without an explicit `arena { }`.

**The first ARC increment should itself be a spike**, because the header
changes the layout of every heap object and codegen currently assumes
`goo_alloc` returns the object address itself. That assumption needs auditing
before any code is written.

## What was NOT established here

- No design for retain/release placement, and no decision on atomic against
  per-goroutine counts. ADR 0002 left both open and they stay open.
- Nothing about cycles. ADR 0002 records that they leak and that a collector is
  a later decision.
- No measurement of what ARC would cost the numeric kernel. ADR 0002
  measurement 4 argues it cannot matter because that loop does not allocate,
  and `bench/stencil` is now the probe that would confirm it.
