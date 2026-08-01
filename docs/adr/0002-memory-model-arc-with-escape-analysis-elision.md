# 2. Memory model: automatic reference counting, elided by the existing escape analysis

Date: 2026-07-28
Status: accepted (2026-07-28). **LARGELY IMPLEMENTED** — ARC 1 shipped in PRs
#258–#286 and is ON by default (`GOO_ARC_RELEASE=0` is the kill switch).
Refined by ADR 0004 (alias-based escape formulation) and ADR 0005 (an escape
is a SET OF REASONS, not a boolean).

**Re-measured 2026-08-02, three builds in one sitting**
(`docs/adr/0005-measurements/scale-400k.md`): the daemon peaks at 26,276 KB at
400,000 requests, against a 793,248 KB control and Go's 8,232 KB — 96.7% of
peak reclaimed, and 3.19x Go rather than the 81x this document opens with.
Retention fell from 1,433 to 41 bytes for each request.

**THE CONTEXT SECTION BELOW IS THE PRE-ARC MEASUREMENT AND IS KEPT VERBATIM.**
Its 1.63 KB per request and 651 MB at 400,000 requests are the baseline this
work is judged against, not a current claim. Do not quote them as today's
figures.

Still open, each named rather than minimised: reference cycles leak; the
CALLEE_VALUE ceiling makes every local with a method set unreleasable and its
corpus-wide reach is UNMEASURED; a multi-assign target gets no store release
(a leak, untested rather than proven absent); an unbound temporary has no
release site; and `arena` is NOT yet re-specified as a proven region, which
the "two questions this choice forces" section below requires.

## Context

The stated goal for Goo is to replace Rust. On the axis that *defines* Rust —
memory safety with deterministic freeing and no garbage collector — Goo today
sits below both Rust and Go, because it frees almost nothing.

This ADR is design only. It names one axis and the evidence for it. It changes
no compiler code.

### What was measured, and how

Every number below comes from a command, not an estimate. Peak resident set
size is `/usr/bin/time -v`, the method `scripts/arena_rss_probe.sh` already
uses. Go figures are `go1.26.1` on the equivalent program.

**1. The daemon shape — a loop that allocates per request and holds nothing.**
This is the program class v1 cannot express. Written without an arena on
purpose: the question is what happens by DEFAULT.

| requests | Goo peak RSS | Go peak RSS |
|---|---|---|
| 50,000 | 82.6 MB | 7.3 MB |
| 100,000 | 163.7 MB | 8.0 MB |
| 200,000 | 325.5 MB | 8.3 MB |
| 400,000 | 651.2 MB | 8.2 MB |

Goo is exactly linear: double the requests, double the memory. Go is flat.
That is about **1.63 KB retained per request, forever**. At a modest 1,000
requests per second a Goo service passes 8 GB in roughly 80 minutes and is
killed. "Unbounded growth" is therefore measured, not asserted.

**2. The opt-in `arena` does NOT rescue this shape.** Wrapping the per-request
work in `arena { }` produced 651,172 KB against 651,164 KB without it — no
difference at all. Moving the work so it sits *lexically* inside the arena
block did not help either (301,188 KB against 301,936 KB).

The reason is a boundary nobody had written down. Contrast, same loop count:

| Allocation shape inside `arena { }` | arena | plain block | Reclaimed? |
|---|---|---|---|
| `new(int)`, `&Pair{A: i}` | 1.7 MB | 26.8 MB | **yes, 16x** |
| `[]int{}` plus `append` | 14.2 MB | 13.7 MB | **no** |

**Arenas reclaim only the allocation sites codegen emits directly in the block
(`new`, `&T{}`) and that `block_escape` proves non-escaping.** Every allocation
made through a runtime helper — `append`, slice and map literals, string
operations, and therefore EVERY stdlib call — is untouched. `CLAUDE.md` calls
arenas "the only bulk-free mechanism", which is true and much narrower than it
sounds. Real code allocates through helpers.

**3. A command-line tool survives, but does not stream.** `googrep` over
increasing inputs:

| input | file size | Goo peak RSS |
|---|---|---|
| 50,000 lines | 1.0 MB | 7.0 MB |
| 200,000 lines | 4.3 MB | 24.7 MB |
| 800,000 lines | 17.9 MB | 94.3 MB |

About **5.3x the input size**, against 2.5 MB flat for `grep(1)`. Survivable,
because the process exits — but Goo cannot process a file larger than about a
fifth of memory where `grep` streams any size.

**4. The numeric hot path does not allocate.** `goostd/lanes`'s `StencilStep`
allocates only on first use (`make` guarded by a length test) and then works
over flat `float64` slices. `make stencil-kernel-bench`: 8-lane wall 0.168s
against serial 0.736s, bit-identical output.

This matters more than it looks. **A per-object runtime cost cannot touch
Goo's differentiator, because the differentiator does not allocate in its
loop.** That converts the usual objection to reference counting from a
principled one into an empirical one that does not apply here.

**5. Annotation burden, counted rather than guessed.** Across the real stdlib
directories: 130 functions and methods, of which 6 return a slice, map or
pointer and 20 return a string; 4 exported struct types hold a slice or
pointer field. Under Rust-style lifetimes, those **26 functions and 4 types
need annotations or owned returns — 20% of the stdlib surface** — plus every
user program with the same shapes. Under any inference-based or
runtime-counted model the figure is zero.

**6. What exists already.** `src/types/param_escape.c` and
`src/types/block_escape.c` RUN today and drive arena auto-promotion from
codegen. `src/types/ownership_checker.c` compiles into `bin/goo` but exports
one function with zero call sites — it is a name, not a system, and must not
be mistaken for a starting point.

### Why the arena reaches so little — diagnosed after the decision

The measurements above say arenas cover almost nothing. The follow-up says WHY,
and it changes the shape of the implementation work:

- **11 of the 13 `codegen_emit_alloc` call sites pass `NULL` as the alloc
  site** — interface boxing (`interface_codegen.c`), closure environments and
  go-arg boxes (`function_codegen.c`), map value boxes
  (`composite_codegen.c`). `block_escape_site_escapes` returns TRUE for a NULL
  site by contract, so those allocations fall through to the heap NO MATTER
  what the analysis could prove. Measured: interface boxing inside an arena,
  400,000 iterations, 13.9 MB with the arena against 14.5 MB without.
- **`append` and slice growth allocate inside the RUNTIME**
  (`goo_slice_alloc`, `goo_slice_append` in `src/runtime/runtime.c`), which has
  no arena awareness at all. They never reach `codegen_emit_alloc`.

**Both the plumbing AND the analysis are short, and neither alone is enough.**
`include/block_escape.h` states its own scope: it classifies exactly two site
kinds, `new(T)` and `&<composite literal>`, and says every other allocation
funnelled through `codegen_emit_alloc` "is out of scope for this cut … a lookup
finds no decision for them -> miss -> heap (conservative, safe)". So threading
the AST node through those 11 sites would produce a MISS, not a decision, and
change nothing on its own.

That gives two phases, and the first is larger than a plumbing job:

1. Extend `block_escape` to classify the additional site kinds (interface box,
   closure env, go-arg box, map value box, slice-literal backing) AND thread
   the real node through the call sites that currently discard it. Both halves
   ship together or neither has an effect.
2. Give the runtime allocator a notion of the current region, so
   helper-allocated memory (`append`, string operations, map inserts) can be
   region-routed at all. This is where the daemon's 1.63 KB per request lives.

**The soundness rule constrains phase 1 hard, and it is asymmetric.** Per that
same header: `true` (escapes) is always a safe answer, and `false` asserts
"provably does not outlive the block". Under-marking is the ONLY bug class that
can dangle a pointer — a use-after-free, not a lost optimisation. Every new
site kind must therefore default to `true` and be narrowed only where the
analysis genuinely understands the construct.

## Decision

Adopt **automatic reference counting, with the existing escape analysis used
as the elision pass.**

An allocation gets a reference count. Retain and release are emitted at
ownership transfers, and `param_escape`/`block_escape` — which already compute
exactly the "this cannot outlive here" property — remove the traffic wherever
non-escape is provable. What they prove today becomes a stack or arena
allocation with no counting at all; what they cannot prove gets counted.
Nothing leaks by default.

The existing `arena { }` becomes the user-visible name for a proven region,
not a separate mechanism. It stays, and it stops being the only thing standing
between a program and unbounded growth.

### Why not the alternatives

**Rust-style ownership and borrowing — rejected, and the reason is strategic
rather than technical.** It is the only axis that literally matches "replace
Rust", and it would end Goo's Go compatibility, which is the property every
arc shipped so far exists to build. This session alone added an embedded
interface, named-type conversion and `io.Writer` purely so that Go's spelling
works unchanged. Lifetimes invalidate that investment across 20% of the stdlib
surface and all user code. It also imports the single most cited reason people
decline Rust — its learning curve. Copying a competitor's principal weakness in
order to compete with it is a poor trade.

**Tracing garbage collection — rejected.** It solves the daemon completely and
would be the least work. It also gives up the position: with a GC and Go's
syntax, Goo is Go with better error handling, and there is no argument left
against using Go. Pauses are the lesser objection; the loss of the
deterministic-freeing claim is the real one.

**Pure region inference — rejected as the PRIMARY model, and this is the
closest call.** It needs no annotations, the machinery is live, and `arena` is
already its surface. But inference is incomplete in general, so every
allocation it cannot prove needs a fallback — and with no fallback, that
allocation leaks, which is exactly today's situation. Measurement 2 shows the
current coverage is `new` and `&T{}` only. Region inference is therefore kept
as the *elision* half of the decision above rather than discarded: it is how
ARC gets cheap, not a substitute for it.

### What this does and does not claim

It does not make Goo into Rust. It gives deterministic freeing with no
lifetime annotations, which is a different position and, on the evidence
above, a more defensible one for a language whose selling point is Go's
spelling. Anyone who needs provable zero-cost aliasing discipline still wants
Rust.

## Consequences

### Positive

- The daemon class becomes expressible. Per-request allocations drop to zero
  references at the end of their iteration and are freed there.
- No annotation burden. All 130 stdlib functions and every existing program
  compile unchanged; Go source still pastes in.
- Freeing is deterministic. No pauses, no collector thread, no heap headroom
  multiplier.
- The existing escape analysis becomes MORE valuable, not obsolete.
- The hot numeric kernel is untouched, by measurement 4.

### Negative, and named rather than minimised

- **Reference cycles leak.** This is the real cost and it has no cheap fix.
  Swift ships this way and documents it, with weak references as the escape
  hatch. A cycle collector is a later decision, not part of this one.
- Retain and release traffic costs time on allocation-heavy code that escape
  analysis cannot prove. Measurement 4 says the kernel is safe; ordinary
  application code will pay something.
- Reference counting is a runtime mechanism. A reader who equates "no runtime"
  with "systems language" will count this against Goo, and they are not wrong
  to notice it.

### The two questions this choice forces

**Does `unsafe` exist, and what does it permit?** Yes, and it must, because
part of why Rust is usable is that its escape hatch is honest. Minimum
surface: take a raw pointer, and suppress counting for a value whose lifetime
the author asserts. It must be a named block, greppable, and it must never be
required to write ordinary code — if `unsafe` becomes routine, the model has
failed.

**What happens to `arena { }`?** It stays and is re-specified as the
user-visible name for a proven region. Today it is an optimisation that
sometimes silently does nothing (measurement 2). Under this decision it
becomes a place where the compiler must PROVE the region holds, and say so
when it cannot, rather than quietly falling back to a leak.

## Open, and deliberately not decided here

- Cycle collection: ship one, or document the leak with weak references.
- Whether release is threaded (atomic counts) or per-goroutine. Goo has real
  concurrency, so this is not free, and it is a design task of its own.
- The migration order. Nothing above says whether counting lands before or
  after the escape analysis is widened past `new` and `&T{}`.
