# The ARC return convention — owned (+1), with the ABI kept uniform

**Date:** 2026-07-28
**Status:** recommendation, with the return boundary audited and gated.
**Follows:** `2026-07-28-arc-arena-coexistence.md`, which settled the PARAMETER
boundary and named this as the next ARC decision.

## The question

Coexistence settled who releases a pointer that crosses INTO a function. It did
not settle who releases one that crosses back OUT. When an ARC-compiled Goo
function returns a pointer, does the caller receive a reference it owns and must
release, or one it merely borrows?

## First, the boundary is sound today — measured

An arena pointer must never be returned, or the caller holds a pointer into a
region the block is about to free. `block_escape` marks a returned value as
escaping, so the site goes to the heap.

The shape worth checking is the one where the return sink has nothing to walk:

```goo
func id(q *T) (p *T) {
    p = q
    return          // ReturnStmtNode->values is EMPTY
}
```

`return_escapes` stays false here, because the sink iterates `->values`. The
parameter is still marked — by a different sink. `analyze_function_body` seeds
`LocalEnv` from `f->param_names` ONLY, so a named result is not a local, and
`p = q` is a store to a non-local. That fires the store-escape sink instead.

Measured on `examples/arena_named_result_probe.goo`: the site emits `goo_alloc`,
the program prints `42`, and valgrind is clean.

**This is sound for a fragile reason.** Register named results as locals — an
obvious-looking tidy-up — and the ONLY sink covering this shape disappears,
because the return sink cannot see a bare `return`. Gated now by `param_escape`
row 20 and the probe above, both of which state that consequence.

## The options

**A. Owned (+1).** The callee hands back a reference the caller owns and
releases.

- Handles the fresh-allocation case (`return &T{}`) with no annotation. Nobody
  else holds that object, so +0 would free or leak it immediately. This case is
  the common one.
- Keeps ONE convention at every call site, so an indirect call needs no summary.
- Costs a retain/release pair on a pass-through return that escape analysis
  cannot remove.

**B. Borrowed (+0).** The callee hands back a pointer it still owns.

- Removes the traffic on getters such as `l.Head()`.
- Cannot express the fresh-allocation case at all, so it needs a per-function
  annotation to say which convention applies. ADR 0002 counted the annotation
  burden of this model at zero, and this reintroduces it.
- Unsound without a borrow scope. Any store to `l.head` between the return and
  the caller's use frees the returned object. Preventing that is exactly the
  lifetime machinery ADR 0002 rejected.

**C. Mixed, inferred from `return_escapes`.** Return +0 when the value is
provably owned by a live parameter, +1 otherwise.

- The signal exists: `return_escapes` already means "returns a value derived
  from a parameter".
- It makes the convention part of the ABI. A caller must know which convention
  each callee uses, and the summary lookup fails on exactly the shapes Goo has
  most of — interface dispatch, function values, goroutines. Those default to
  +1, so a function reachable both ways needs two entry conventions or the
  conservative one anyway.
- `return_escapes` is ONE bool for the whole function, not per result. A
  function returning `(*T, *U)` gets a single answer for both.

## Recommendation: A, owned (+1), with escape-analysis elision

Two reasons, and the second is the decisive one.

1. It is the only option that handles fresh allocation without an annotation,
   and fresh allocation is the common return.
2. **A uniform ABI is reversible; a summary-dependent one is not.** Under C, an
   indirect call whose summary is missing does not merely lose precision — it
   picks the wrong convention, which double-frees or leaks. That is the exact
   failure mode of the receiver bug fixed alongside this: the summary lookup
   missed, and the conservative default was the only thing standing between the
   compiler and a use-after-free. Under A a missing summary costs a retain and
   a release, and nothing else.

C stays available as a later ELISION over a uniform ABI, which is the safe
order: get the convention uniform, then remove provable traffic. It should not
be the convention itself.

The cost lands on the return path. ADR 0002 measurement 4 says the hot numeric
kernel does not allocate in its loop, so this does not touch the differentiator.

## What this does NOT decide

- Atomic against per-goroutine counts. Still open from ADR 0002.
- Cycles. Still open.
- Map slots holding pointers as `i64` (`codegen.c:736`, `:1384`, `:1405`).
  Invisible to ARC, and still the largest un-decided hole.
- Multiple results with mixed ownership. Option A makes every returned pointer
  +1, so this does not block the decision, but a per-result `return_escapes`
  would be needed before any C-style elision.
