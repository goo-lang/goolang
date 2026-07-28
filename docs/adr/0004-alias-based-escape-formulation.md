# ADR 0004 — the escape analysis is a location-insensitive alias formulation, and that is deliberate

Date: 2026-07-29
Status: accepted as a DIRECTION. Nothing here is implemented, and this ADR
changes no code.

Sources: `rust-lang/polonius`, and Niko Matsakis, "An alias-based formulation of
the borrow checker", 2018-04-27.

## Context

ADR 0002 decided automatic reference counting with the existing escape analysis
as the elision pass. T4 is the first consumer that will actually free memory.
Its three conditions are "does not outlive the function", "the local OWNS the
value", and "not arena-routed", and `include/local_escape.h` states plainly that
the analysis answers only the first:

> ALIASING IS NOT ESCAPE [...] `TrimPrefix` returns `s[len(prefix):]`, a view
> into its argument's buffer. A local bound to that value does not own it, and
> releasing it would free the caller's string.

Rust solved a closely related problem and published the formulation. This ADR
records what carries over, what does not, and why the gap that remains is
acceptable rather than merely unfixed.

## The correspondence

Polonius redefines a region. It stops being a set of program points and becomes
**a set of loans**. `requires(R, L, P)` says region `R` depends on loan `L`
staying valid at point `P`, and `subset` is the transitive closure of the
type-checker's requirements across control flow.

Goo's engine already has that half:

| Polonius | `src/types/escape_core.c` |
|---|---|
| origin = a set of loans | `TaintSet` = the set of this unit's slots the value can alias |
| `requires(R, L, P)` | one taint bit on one slot |
| `subset` transitive closure | `escape_taint_union_into` plus the fixpoint loop |
| `borrow_region(R, L, P)` | `expr_source_slot`, which sets a site's own bit |
| `region_live_at`, `loan_live_at` | **absent** — no program points, no CFG |
| `invalidates(P, L)` → `error(P)` | **absent, and not wanted** |

A `TaintSet` IS an origin. The engine was written as an alias formulation
without the name.

## Why the absent half is acceptable here

Two things are missing, and they are not equally important.

**The `error` relation is not wanted at all.** Rust must reject a program. Goo
marks `escapes[]` and rejects nothing, so the entire error-reporting and
diagnostic machinery that dominates a borrow checker is out of scope. That is a
simplification, not a debt.

**Location insensitivity is a real loss with a benign failure mode.** Polonius
describes its own location-insensitive variant as "faster, but may yield
spurious errors". That sentence is the whole argument for this ADR:

- In a borrow checker, a spurious result **rejects a valid program**. The user
  cannot proceed. Location insensitivity is therefore close to unusable there,
  which is why Rust pays for the program points.
- In an ARC elision analysis, the identical imprecision is a conservative
  `escapes = true`. The consequence is that a value keeps leaking, exactly as it
  does today. Nothing breaks, and nothing is rejected.

**Record this asymmetry, because it is the reason not to build a CFG.** Goo sits
at the cheap point of Polonius's own design space, and the cost it pays there is
the cost it is already paying.

## What to take, and what to leave

### Take: origin-emptiness as the ownership test (T4 condition 2)

A non-empty origin means the value requires a loan, so it is a VIEW and not
owned. An empty origin means it aliases nothing this unit tracks. That is the
shape of the answer T4 needs for `TrimPrefix`, and the machinery to carry it
already exists.

**Before T4 relies on this, confirm the slot universe of each pass is the right
loan universe.** `param_escape`'s slots are parameters, `block_escape`'s are
allocation sites, `local_escape`'s are locals. "Aliases none of MY slots" is not
the same statement as "owns its value", and the difference has not been
measured. PR #256 is the warning: the `non_retaining` column deliberately
carries two facts in one bit, and `errors.Unwrap` retains nothing yet returns a
pointer INTO its argument. A borrowed result is exactly the case a
retention-only view of the world gets wrong.

### Leave: the kill rule, because it is the CFG

Polonius kills a loan when the variable holding it is reassigned, and liveness
gates propagation across each control-flow edge, so stale constraints do not
accumulate.

Goo has no kill. `escape_env_add_or_union` only ever unions, and `escapes[]` is
documented as "only ever set true". Once anything taints a local it stays
tainted for the whole function.

That is the larger prize. A reassigned loop local is the daemon's retention
shape, and a kill rule would let a release land at the reassignment instead of at
function exit. But **monotonicity is what makes the fixpoint terminate and the
soundness argument short.** A kill rule needs flow sensitivity, flow sensitivity
needs program points, and the passes walk the AST with no CFG.

**The kill rule and the CFG are one item of work, not two.** Do not cost them
separately.

### The measured ceiling on all of it

A better analysis raises the ceiling on the ~30.2% of the daemon's bytes that
locals hold. It does not touch the 23.2% the map holds, because
`goo_map_set_sv` keeps the key pointer verbatim and never frees it, and no
codegen-side release can reach that. Polonius's own prototype was, by its
author's admission, slower than the analysis it replaced.

So the order is: take the cheap ownership idea, keep the location-insensitive
engine, and treat the CFG as a decision that needs its own measurement of what a
kill rule would actually reclaim.

## Consequences

- T4 gets a named source for its ownership condition instead of inventing one.
- The absence of a CFG becomes a recorded decision with a reason, rather than an
  unexamined limitation.
- `local_escape`'s `defer_is_like_go = true` stays as it is. Its comment already
  names T4's release-versus-defer ordering as the blocker, and that ordering is
  the same liveness question this ADR declines to buy.
- If a future measurement shows the kill rule reclaiming enough to justify a
  CFG, this ADR is the thing to supersede.

## Related

- ADR 0002 — the memory model this serves.
- `docs/adr/0002-measurements/escape_arm_coverage.md` — how well the three row
  tables actually cover the engine, which is what T4 will stand on.
- `include/escape_core.h` — the six hooks, and the two defects that came from
  hand-mirroring the walk.
