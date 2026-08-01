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

---

# The outcome, 2026-08-01

Route 1 was taken. This section records what the route actually cost and what it
bought, because the section above deliberately did not choose between the two.

## Route 1 as built differs from route 1 as proposed

The proposal was a blacklist: "refuse any loop-declared local that appears as the
RHS of an assignment whose LHS is not declared in the same block". It called that
sound by construction, and a blacklist of sink shapes is not — a sink nobody
listed frees live memory silently.

What shipped keeps the property and drops the enumeration. The walk in
`release_decision.c` is TOTAL: a statement kind it does not recognise sets
`unreadable` and refuses the whole function. So every store a READABLE function
can perform passes through `note_assignment` or `note_declaration`, and hooking
both enumerates the sinks by construction rather than by inspection.

The rule is a MENTION test with a depth comparison, not a flow test: a local
declared at loop depth D is refused when its name appears anywhere in the RHS of
a store whose target lives longer than D.

## What it cost

**`deadEach` is now REFUSED**, where this document predicted route 1 would
approve it. `n = n + len(s)` mentions `s`, and proving that `len` propagates
nothing needs the inverse of condition 2's binding table, which does not exist.
The prediction assumed a flow test. A mention test is cheaper and stricter.

This did not cost the measured bytes, because the daemon's `err` appears only in
`if err == nil` — a condition, not a store.

## What it bought

340,000 bytes on `bench/daemon/daemon.goo`, both builds measured in one sitting:
952,098 → 612,100 in use at exit per 2,000 requests. 160,000 direct plus 180,000
indirect, matching the record's recorded composition on both halves.

## Finding 2 is now covered, and that was checked rather than assumed

`carrySlice` still reads `RELEASE_NO_ESCAPES`: `append`'s elements keep
`retains = true`, so condition 1 refuses `s` before condition 6 is consulted. The
accidental safety this document warned about is therefore still what holds today.

The difference is that there is now a second, real reason underneath it. Moving
the condition 6 test above condition 1 makes `carrySlice: s` read
`RELEASE_NO_BLOCK_ESCAPE`. So future precision work on append's element marking
no longer creates a hazard with no gate to notice it.

## The gate, and the reason it must be valgrind

`examples/arc_loop_carried_probe.goo` covers the hazard plus the six exit paths.
With condition 6 removed it reports 3 `Invalid read` through
`carried()` → `ToUpper` → `strlen` on a freed block.

**Its stdout is byte-identical and its exit status is 0 in both cases.** The
freed bytes have not been reused by the time they are read. A golden fixture, an
exit-status check and a diff of output are all blind to this defect. That is why
the gate greps valgrind's log and why it also asserts that the release-off side
leaks something — a probe that measures nothing reports green too.
