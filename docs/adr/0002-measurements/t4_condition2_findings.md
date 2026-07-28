# T4 condition 2, measured: two findings, one of which retargets T4

Measured 2026-07-29 against `2ef41fa`.

Probes: `dump_ownership.c` and `dump_local_escape.c`, both run on
`ownership_shapes.goo` in this directory. `ownership_shapes.goo` is
DELIBERATELY IMPORT-FREE, because `.handoff.md` records a confident-looking
local_escape table whose every verdict was conservative for an unrelated reason:
the imports had not resolved, and the tell was `i`, a plain int loop counter,
reading as escaping. One cause per verdict needs a reproduction with nothing to
resolve.

## Why this ran before T4

T4 has three conditions. `include/local_escape.h` says plainly that the pass
answers only the first:

> This module answers "does the value outlive F", NOT "does the local own the
> value". A release consumer needs BOTH.

ADR 0004 proposed origin-emptiness as the answer, and flagged one thing to
confirm first: that each pass's slot universe is the right loan universe. This
is that check. It found the proposal does not work, and then it found something
larger.

## Finding 1 — origin-emptiness cannot answer condition 2. It is constant-false.

`local_escape.c:45` — "A slot here is a local index." Line 170 — "a local keeps
its own bit for life."

So **every local's TaintSet contains at least its own bit, and is never empty.**
The test ADR 0004 proposed carries no information at all: it is not merely
imprecise, it can never be true.

The deeper reason is worse. What a local BORROWS FROM is a parameter, and
parameters are not in `local_escape`'s slot space. The borrowed-from relation is
**not representable** in that loan universe.

Measured, on the discriminating case:

| Local | Bound to | Truth | `local_escape` says |
|---|---|---|---|
| `a` | `makeOwned()`, returns `new(int)` | OWNED | does not escape -> release |
| `b` | `borrowView(s)`, returns `s[1:]` | BORROWED from the caller | does not escape -> release |
| `c` | `s[1:]` directly | BORROWED from the caller | does not escape -> release |

A T4 built on `local_escape` alone frees the caller's buffer in two distinct
shapes.

## Finding 1a — the fact DOES survive, one level up

`ParamEscapeSummary.return_escapes` is documented as "does F return a value
derived from one of its own params?". That IS the borrowed-result relation,
stated per CALLEE rather than per local, and it is already exposed through
`param_escape_lookup`.

Measured:

| Callee | `return_escapes` | Reading |
|---|---|---|
| `borrowView` | true | result may alias an argument -> caller does NOT own it |
| `makeOwned` | false | result derives from no argument -> caller owns it |

So condition 2 is answerable by a rule at the local's BINDING SITE, not by a
property of the local:

| Bound to | Owned? | Source of the answer |
|---|---|---|
| `new(T)`, `&T{}`, a composite literal | YES | the site is an allocation |
| a call to a Goo function | iff `return_escapes == false` | `param_escape_lookup` |
| a call to a C shim | iff `non_retaining == 1` | `shim_signature_is_non_retaining` |
| a slice, index or selector of another value | NO | it is a view -- covers `c := s[1:]` |
| another local | NO | an alias, and only one owner may release |
| anything not recognised | NO | conservative, the safe side |

The shim column already carries the right meaning, and this is worth recording
because the handoff decided against a separate `returns_borrowed` flag. That
decision holds up here. `non_retaining == 1` is justified in
`src/types/shim_signatures.c` against each runtime body, and the audit is
explicit that every whitelisted `strings` entry COPIES -- the table even notes
that `TrimSpace` copies where Go's returns a slice of its argument.
`errors.Unwrap` is `0` precisely because it returns `e->cause`, a pointer INTO
its argument. The one bit separates owned from borrowed correctly.

## Finding 2 — T4's chosen first target reclaims ZERO on the daemon

This is the one that changes the plan.

`.handoff.md` scopes T4 as "emit `goo_release` for owned, non-escaping ERROR
locals at function exit", and picks `err` over `fields` because "`err` is the
smaller: a plain pointer, one owner, no container".

**`err` is loop-bound.** In `bench/daemon/daemon.goo` it is
`n, err := strconv.Atoi(f)`, INSIDE `handle`'s `for` loop.

`local_escape` reports ONE boolean per local NAME for the whole function, and
the engine has no kill rule: `escape_env_add_or_union` only unions, and
`escapes[]` only ever becomes true. Measured, `h`'s function-scope `outer` and
its loop-bound `inner` are indistinguishable -- both "does not escape", with no
scope information in the API at all.

So a release at FUNCTION EXIT frees whatever the local holds at that moment: one
of N, leaking N-1.

**For the daemon it is worse than 1/N.** The request is
`"1, 2, 3, four, 5, six, 7"`. Seven fields, so seven iterations. `Atoi` fails on
`four` and `six` and succeeds on the rest, so at `handle`'s exit `err` holds the
result for `"7"` -- which is **nil**. `goo_release(NULL)` is a checked no-op
(`include/runtime.h`).

**T4 as specified frees nothing at all, not the projected 5.8%.** The two real
error objects are overwritten and leaked.

## What T4 should target instead

A local that is bound ONCE at function scope, not inside a loop.

In `handle`, `fields := strings.Split(req, ",")` is that shape:

| | `err` (as specified) | `fields` (recommended) |
|---|---|---|
| Scope | inside the loop, rebound 7x | function scope, bound once |
| Owned? | yes -- `strconv.Atoi` is `non_retaining = 1` | yes -- `strings.Split` is `non_retaining = 1` |
| Non-escaping? | yes, measured in T0+T3 | yes, measured in T0+T3 |
| Reclaimed by a function-exit release | **0 B** -- holds nil at exit | **128 B/req, 9.3%** |
| Not reclaimed (shallow) | the 90 B of message copies | its 6 element strings, 118 B, 8.6% |

The handoff chose `err` for implementation simplicity. That reasoning was sound
for the code shape and wrong about the byte outcome, because scope was not part
of the comparison.

## What T4 now needs that it did not before

1. **A scope test.** "Bound once at function scope" is not derivable from
   `LocalEscapeResult`, which is a name plus a boolean. Two routes: add the fact
   to `local_escape` (a `declared_in_loop` bit beside `escapes`), or read it in
   codegen, which already tracks block scope (`src/codegen/value_scope.c`). The
   codegen route needs no change to a soundness-critical pass, so it looks
   cheaper.

2. **The binding-site ownership rule** from finding 1a, which is new work rather
   than a lookup, and which no existing row covers.

3. **Releasing a loop-bound local is the kill rule**, and ADR 0004 already
   records that the kill rule and the CFG are one item of work and not two. So
   `err` is not a small follow-up to `fields`. It is the CFG.

## Instrument notes

- `ownership_shapes.goo` is import-free on purpose. See the top of this file.
- `dump_ownership.c` prints the per-callee summaries AND the per-local verdicts
  side by side, so the two levels can be compared in one run rather than
  reconciled by hand.
- Neither probe is a gate. They are evidence for a design decision.
- Both were built against the same object set the unit suites link. Build line:
  see `README.md` in this directory.

## Reproducing

```sh
# built against the same objects as the unit suites; see README.md
./dump_ownership   docs/adr/0002-measurements/ownership_shapes.goo
./dump_local_escape docs/adr/0002-measurements/ownership_shapes.goo
```
