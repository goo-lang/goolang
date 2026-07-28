# The ARC parameter boundary — uniformly borrowed, which costs zero instructions

**Date:** 2026-07-28
**Status:** specification. The header it builds on is implemented and gated;
this boundary emits no code by construction, and the three places that DO emit
because of it are named below.
**Follows:** the object header (this change), `2026-07-28-arc-arena-coexistence.md`,
`2026-07-28-arc-return-convention.md`.

## The rule

**Every parameter is borrowed (+0). The call boundary emits nothing.**

The caller keeps its own reference alive across the call, because it releases
only at the end of its own scope, which is after the call returns. The callee
therefore does not need a reference of its own, and must not release one it
never took.

This is uniform: it does NOT vary with the `param_escape` summary. A callee that
stores its parameter still takes no reference *at the boundary* — the STORE
takes it (see "What does emit", below).

## Why uniform, and not summary-driven

`2026-07-28-arc-arena-coexistence.md` framed the elision as "emit no retain or
release for a parameter proven non-escaping", which reads as summary-driven.
Making it uniform is strictly better, for the reason the return convention
already established: **a summary-dependent ABI is unsound when the summary is
missing, and Goo's most common call shapes are exactly the ones the lookup
cannot resolve** — interface dispatch, function values, and method calls
through a selector.

Uniform borrowing removes that risk entirely, and it strengthens the
coexistence guarantee rather than weakening it:

> Under uniform borrowing, **no callee ever releases a parameter.** So an arena
> pointer passed to any function is never released by that function — with no
> appeal to the summary at all.

The summary is still load-bearing, but for the ARENA routing decision it always
drove, not for ARC's safety. The receiver hole fixed in PR #248 therefore
remains a real defect for arena routing, and is not retroactively made harmless
by this rule.

## What DOES emit, because of this rule

The boundary is free. Three consequences of it are not, and each is a separate
increment.

**1. `go f(p)` must transfer +1.** This is the one true exception. A goroutine
outlives the caller's scope, so the caller cannot guarantee liveness across it.
The caller must retain each argument, and the goroutine must release them when
it finishes. The machinery already sees this shape: `handle_go_call` marks every
`go` argument as escaping unconditionally, in both sibling modules.

**2. Returning a borrowed value requires a retain.** The return convention is
owned (+1), so `func id(q *T) *T { return q }` must retain `q` before returning
it — the caller will release a reference the callee never took otherwise.
`return_escapes` is exactly the signal for this: it already means "this function
returns a value derived from one of its own parameters". That is its concrete
ARC use.

**3. Storing a parameter retains at the store.** `g = p` inside a callee takes
the reference. This is the store boundary, and it is not specified here.

## Why this ordering is the cheap one

The boundary is where most ARC implementations put their traffic, and it is the
hottest place in a program. Making it free means the remaining work is confined
to sites that are rarer and easier to reason about: goroutine spawns, returns of
borrowed values, and stores.

It also means the parameter boundary needs no probe of its own. A gate for
"emits nothing" is a gate on the absence of instructions, which the golden IR
suites already cover implicitly — the 491 goldens passing unchanged with the
header in place is that evidence.

## What is implemented, and what is not

**Implemented in this change:** the object header only. `goo_alloc` carries a
16-byte header, `goo_retain`/`goo_release`/`goo_obj_refcount` exist and are
tested (11 rows, 25 checks), and the whole thing is proven invisible — 491
goldens at -O0 and -O2, arena probes valgrind-clean.

**NOT implemented:** no retain or release is emitted by codegen anywhere. The
header costs 16 bytes per allocation and buys nothing yet:

| daemon | before | with header | delta |
|---|---|---|---|
| 50,000 requests | 82.6 MB | 92.7 MB | +12.2% |
| 400,000 requests | 651.2 MB | 733.9 MB | +12.7% |

That cost is real and is paid in advance. It only turns into a win when the
release side lands and the daemon's per-request memory stops accumulating.

**The next increment that produces observable behaviour** is scope-exit release
of an owned local: a fresh allocation that provably does not escape its scope is
released at scope end. That is the smallest change that makes a Goo program free
something it does not free today, and `bench/daemon` is the instrument that
shows it.

## Still open

- Atomic against per-goroutine counts. Consequence 1 above (`go` transferring
  +1) is where this stops being theoretical, because two goroutines can then
  hold references to one object.
- Cycles.
- Map-held references, specified in `2026-07-28-arc-map-held-references.md`.
- Slices of pointers, not specified anywhere yet.
