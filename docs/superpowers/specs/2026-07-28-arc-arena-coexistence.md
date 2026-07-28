# Can ARC and `arena { }` coexist? — yes, and the mechanism already runs

**Date:** 2026-07-28
**Status:** answered, plus one use-after-free the answer exposed and this change fixes.
**Answers:** the "Recommended next increment" of
`2026-07-28-arc-header-feasibility-audit.md`, item 2 — the single open question
blocking all ARC work.

## The question

`goo_arena_alloc` bump-allocates inside a block (`src/runtime/arena.c:69`). An
arena object is not a `malloc` result, so a `release` that reached one and
called `free` is undefined behaviour.

ADR 0002's answer is that escape analysis elides ARC exactly where the arena
applies. The audit accepted that for the CALLER and rejected it for the callee:
pass an arena-allocated value to a function, and the callee cannot see where it
came from, so a conservative callee emits `release` on it.

## The answer: option 2, callee-side elision from the 7a summaries

The audit rejected this option because "a summary says whether a param escapes,
not where it was allocated". That objection does not hold, because of a
property of `call_taint` in `src/types/block_escape.c`:

```c
} else if (callee) {
    retains = (i < callee->param_count) ? callee->escapes[i] : true;
} else {
    retains = true; // external/unregistered/no-summaries: pure-conservative
}
if (retains) mark_escapes(ctx, &arg_taints[i]);
```

A site is marked escaping — and therefore routed to the heap — whenever the
callee retains that argument. So:

> **An arena pointer can only ever be bound to a parameter whose summary says
> `escapes == false`.**

If ARC emits no retain or release for a parameter proven non-escaping (a
borrowed/guaranteed convention, as Swift spells it), then a callee never emits
a release on a pointer that could be arena-allocated. Caller-side and
callee-side elision are driven by the SAME summary, and the two mechanisms
provably never meet. The callee does not need to know where the value came
from, because the caller already refused to arena-allocate anything reaching a
retaining parameter.

The conservative defaults do the load-bearing work: an unresolved callee, a
variadic tail, and every `go` argument all retain, so each forces the heap.

## Why not the other two shapes

**Option 1, a flag bit in the header — worse than the audit records.** It
assumes an arena object HAS a header. It has none: `goo_arena_alloc` returns a
bare bump pointer (`arena.c:90`). A release would read `ptr - HDR`, land in the
previous object's tail, and then `free` an interior pointer. Rescuing option 1
means giving arena allocations a header too — 16 bytes each, since the payload
alignment is already 16 and no smaller header is legal — plus a branch per
release, and it re-opens the one-allocation-door asymmetry PR #247 closed.
Option 2 costs zero runtime bytes and zero branches.

**Option 3, forbid an arena value crossing a function boundary — unnecessary.**
Sound and cheap, but option 2 buys the same soundness without narrowing
`arena { }`.

## The precondition, which did NOT hold — a live use-after-free

The theorem needs the marking to be complete. It was not.

For a method call the callee is a SELECTOR expression, so the summary lookup
(which requires an `AST_IDENTIFIER`) misses. A receiver is not a member of
`call->args`, so the retain-all rule for an unresolved callee never reached it.
Both sibling modules computed the callee taint and discarded it —
`block_escape.c` in `call_taint`, `param_escape.c` in its own call handler.

This is the same hole `handle_go_call` already closed for `go p.m()`, left open
on the ordinary call path. The `go` form is masked by a separate defect (a
pointer receiver there fails module verification); the ordinary form was not
masked at all.

Measured on `examples/arena_method_recv_probe.goo`, one arena block containing
the same escape spelled two ways:

| Spelling | Callee resolves? | Emitted allocator | Correct? |
|---|---|---|---|
| `stashFn(a)` | yes, identifier | `goo_alloc` | yes, heap |
| `b.stashM()` | no, selector | `goo_arena_alloc` | **no** |

The emitted IR freed that arena while the global still pointed into it. At
runtime the probe printed `11` then `4503668347830287`, and **exited 0**.
Valgrind: `Invalid read of size 8`.

Under ARC the same hole stops being a wrong number and becomes `free()` on an
interior bump pointer — heap corruption.

## The fix in this change

Mark the callee taint instead of discarding it, for the non-identifier shape
only, in both sibling modules. An IDENTIFIER callee keeps its old behaviour on
purpose: calling a closure held in a local reads its captured cells but cannot
leak the environment itself, which `block_escape` row 29 pins.

Cost, pinned deliberately as row 31 rather than left to drift: an unresolved
callee cannot be proven non-retaining, so ANY method call on a site now marks
it. Arena coverage narrows by that much.

Gates: `block_escape` rows 30 and 31, `param_escape` row 19,
`examples/arena_method_recv_probe.goo` in `arena-free-probe` and
`arena-valgrind-probe` (14/14 each) and in the golden suite (489). The probe was
confirmed to FAIL with the two source fixes reverted — the gate has teeth.

## REFINED — the parameter rule is uniform, not summary-driven

`2026-07-28-arc-parameter-boundary.md` strengthens the argument above. Making
the elision UNIFORM (every parameter borrowed, never summary-driven) is strictly
better, for the reason the return convention independently established: a
summary-dependent ABI is unsound when the summary is missing, and Goo's most
common call shapes are exactly the ones the lookup cannot resolve.

Under uniform borrowing, no callee ever releases a parameter, so an arena
pointer passed to any function is never released by it — with no appeal to the
summary at all. The theorem below still holds; it simply stops being the thing
ARC's safety rests on.

The summary remains load-bearing for the ARENA routing decision it always drove.
The receiver hole fixed alongside this document is therefore still a real
defect, and is not retroactively made harmless.

## Recovering the lost precision — a follow-up, not this change

Resolve `p.m()` to its real summary. Blocked as things stand: the registry keys
on the BARE name and `registry_find` returns the first match
(`param_escape.c:158`), so `p.m()` could resolve to a different type's `m`.
That needs receiver-type-qualified keys, which also closes a latent
function-and-method collision on the identifier path. Do it only if arena
coverage measurably suffers — the `arena-rss-probe` figures are the evidence to
judge it on.

## What this does NOT establish

- No header was added. No retain or release is emitted anywhere. ARC is
  unblocked by this answer, not started by it.
- Nothing about atomic against per-goroutine counts, or cycles.
- Map slots still hold pointers as `i64` (`codegen.c:736`, `:1384`, `:1405`),
  which is invisible to ARC and still needs its own decision.
- The soundness argument covers the parameter boundary. A RETURNED
  arena pointer is handled by the existing return sink, but ARC's return
  convention (owned against borrowed) is a separate decision.
