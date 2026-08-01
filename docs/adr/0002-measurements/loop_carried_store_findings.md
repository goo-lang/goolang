# The loop-carried store, measured

Date: 2026-08-01
Compiler: `bin/goo` at `a180c85` (main, after PR #276 and #277)
Fixture: three functions, one hazard shape each, one control.

The loop-scoped-release design says this, of step D:

> Whether it can be retargeted from arena blocks to any block, or needs the
> CFG's own liveness, is the first thing step D must settle **by measurement**,
> before any emission is written.

This is that measurement. **It says neither option works as stated, because the
distinguishing fact is not computed anywhere today.**

## The fixture and the verdicts

Read with `GOO_ARC_DEBUG=1`. `ESCAPES` is checked BEFORE `LOOP_SCOPE`, so a
local reading `LOOP_SCOPE` is one that condition 1 did NOT catch.

| Function | shape | verdict for `s` |
|---|---|---|
| `carryLocal` | `last = s`, `last` declared outside the loop | `RELEASE_NO_LOOP_SCOPE` |
| `carrySlice` | `acc = append(acc, s)`, `acc` outside the loop | `RELEASE_NO_ESCAPES` |
| `deadEach` | `n = n + len(s)`, nothing retains `s` | `RELEASE_NO_LOOP_SCOPE` |

## Finding 1 — the hazard and the control are indistinguishable

`carryLocal` and `deadEach` both read `RELEASE_NO_LOOP_SCOPE`. Condition 4 is
the ONLY thing refusing either of them, and it refuses on a syntactic property
(declared inside a loop) that both share.

So relaxing condition 4 without adding a new fact frees `s` at the end of each
iteration in `carryLocal`, while `last` still points at that buffer. The
following `return len(last)` then reads freed memory.

`last` itself is refused as `RELEASE_NO_REBOUND`, so nothing double-frees. The
failure is a dangling read, not a double free.

## Finding 2 — `carrySlice` is safe by accident, not by reasoning

`append`'s ELEMENTS keep `retains = true` in `call_taint`, so `s` is marked
escaping and condition 1 refuses it. Nothing reasoned about the loop. Any
future precision work on `append` element marking would silently turn this
safe shape into a second hazard, and no gate would notice.

## Finding 3 — why neither existing module answers it

- **`block_escape` has the right BOUNDARY and the wrong SOURCE.** Its sources
  are ALLOCATION SITES (`new(T)`, `&T{}`) lexically inside an `arena { }`
  block. A named local bound to a shim result is not a site it classifies, so
  retargeting the boundary alone never makes it see `s`.
- **`local_escape` has the right SOURCE and the wrong BOUNDARY.** It answers
  "does this local escape the FUNCTION". In `carryLocal`, `last = s`
  propagates `s`'s taint to `last`, and `last` never leaves the function — so
  at function granularity `s` genuinely does not escape. The answer is correct
  and useless: the value outlives its ITERATION, which is a boundary nobody
  computes.

## What step D therefore needs

A fact that does not exist today: **does this local's value reach anything
whose lifetime is longer than one iteration of its declaring block?**

Two routes, and this measurement does not choose between them.

1. **A conservative syntactic guard.** Refuse any loop-declared local that
   appears as the RHS of an assignment whose LHS is not declared in the same
   block, or that is passed to a callee that retains it. Sound by
   construction, cheap, and no new dataflow. It refuses `carryLocal` and
   approves `deadEach`. **It also approves the daemon's `err`**, which is only
   ever compared to nil — so the 340,000 bytes are reachable this way.
2. **A real block-boundary escape fact.** The `block_escape` engine with named
   locals added to its source vocabulary and its boundary set to any block
   rather than an `arena { }`. More reach, and it is the same engine three
   passes already share, so a soundness change there touches all of them.

Route 1 is enough for the measured bytes. Route 2 is what a general answer
needs. Do not conflate them, and do not report route 1 as having solved the
general problem.

## The gate this needs either way

A loop-carried store probe that must REFUSE, in the shape of `carryLocal`. It
must be shown able to fail: with the guard removed, the probe must report a
release for `s`, and valgrind must show the invalid read through `last`.
