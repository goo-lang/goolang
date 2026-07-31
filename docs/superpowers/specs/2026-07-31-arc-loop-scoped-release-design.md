# Releasing a loop-scoped local — and the two guards that refuse `err` long before the kill rule does

**Date:** 2026-07-31
**Status:** design. Four steps, sequenced so that each one can be measured.
**Supersedes in part:** ADR 0004's costing of the kill rule (see "What this
corrects", below). It does not supersede the ADR's decision to keep the escape
engine location-insensitive — that stands.

## Why this exists

`.handoff.md` names `strconv.Atoi`'s error objects as NEXT item 2: **340,000
bytes per 2,000 requests, 35.7%** of what `bench/daemon/daemon.goo` still
retains after PR #275. It prescribes a fix:

> `err` is loop-scoped, so condition 4 refuses it. Needs release at ITERATION
> end across the normal, `continue`, `break` and `return` paths — the ADR 0004
> kill rule / CFG problem.

The first sentence is true. **It is not the binding constraint**, and building
the CFG first would have reclaimed zero bytes.

## The measurement that changed the plan

A temporary diagnostic in `codegen_arc_note_local` printed each local's verdict
before the `should_release` bail:

| fixture | local | verdict | emitted? |
|---|---|---|---|
| `n, err := strconv.Atoi(s)`, **function scope, no loop** | `err` | `RELEASE_NO_NOT_OWNED` | no |
| `e := errors.New("boom")`, single name | `e` | **`RELEASE_OK`** | **no** |

Two independent refusals, and neither is condition 4:

1. **The tuple destructure.** `n, err := strconv.Atoi(f)` has two targets and
   one value. `release_decision.c`'s `AST_MULTI_ASSIGN` arm records the binding
   value as `NULL` when the counts differ, so condition 2 refuses. The code
   comment there already says *"That is the daemon's `n, err :=
   strconv.Atoi(f)` shape"* — the connection to NEXT item 2 was simply never
   made.

2. **The codegen shape test.** An `error` local lowers to `{ i1, ptr }` —
   field 0 is the nil flag and the heap object is at **field 1**.
   `codegen_arc_note_local` accepts a bare pointer, a 3-field slice, or a
   2-field *string*, so it refuses this shape and emits nothing even on a
   `RELEASE_OK` verdict.

Removing the loop is what isolates them: at function scope, bound once, with no
loop anywhere, `err` still does not release. Condition 4 is never reached.

## What this corrects

ADR 0004 says "the kill rule and the CFG are one item of work, not two. Do not
cost them separately", and names its own supersede condition: *"If a future
measurement shows the kill rule reclaiming enough to justify a CFG, this ADR is
the thing to supersede."*

Two corrections, in opposite directions:

- **The kill rule is worth less on its own than the handoff implies**, because
  two cheaper guards refuse the same local first. Cost the CFG at 340,000 bytes
  ONLY as the third of three steps.
- **The exit-path problem is already solved.** The ADR treats "release at
  iteration end across normal / `continue` / `break` / `return`" as part of the
  CFG purchase. Codegen already enumerates exactly those exits for arena
  frees — `codegen_emit_arena_frees` (loop-depth keyed, for `break`/`continue`
  and their labelled forms), `codegen_emit_arena_frees_to_depth` (lexical, for
  `goto`), `return` freeing all active arenas, and a terminated-block guard
  that stops any path freeing twice. `examples/arena_loopexit_reclaim_probe.goo`
  gates it. **The CFG buys liveness, not exit enumeration.**

Related stale comment found while checking this: the block above
`codegen_emit_arena_frees` still claims "`break`/`continue` do NOT reach here
and still leak their arenas — safe, a documented follow-up". The call sites at
`statement_codegen.c:463` and `:478` pass `codegen->cfctx.loop_depth`, and the
probe proves the reclamation. The comment describes a state that no longer
exists and should be deleted with step D.

## The four steps

Sequenced so each has a target it can move. Only D moves the daemon, because
`err` is loop-scoped **as well as** tuple-bound — but A, B and C each have an
independently measurable fixture, and none of them can be judged on the
benchmark.

| step | daemon bytes | its own measurable target |
|---|---|---|
| A — verdict diagnostic | 0 | it *is* the instrument |
| B — error shape arm | 0 | a function-scoped single-name error |
| C — tuple-destructure ownership | 0 | a function-scoped `n, err :=` |
| **D — CFG, liveness, scope-exit release** | **340,000** | the daemon |

### Decomposition — this is two increments, not one

**A + B + C are one PR.** They share a subject (why an error local refuses),
they are each a handful of lines, and together they take a function-scoped
error from "refused by two guards" to "released". Their gates are rows and
small probes.

**D is a separate PR with its own spec.** It adds a module, and its central
soundness question — the loop-carried store below — is deliberately left open
here because it must be settled by measurement before an emission design is
worth writing. Do not expand this document to cover D's internals; write the
second spec once A+B+C are merged and the loop-carried measurement exists.

The sections below specify A, B and C to implementation depth. D is specified
only to the depth its open question allows.

### A — the verdict diagnostic

One line in `codegen_arc_note_local`, before the `should_release` bail:

```c
if (getenv("GOO_ARC_DEBUG")) {
    fprintf(stderr, "[arc?] %s: %s -> %s\n", fi->name, info->name,
            release_verdict_name(release_plan_verdict(codegen->release_plan,
                                                      fi->name, info->name)));
}
```

Today only approvals print, so a refusal is invisible, and "the plan refused"
is indistinguishable from "codegen refused the shape". Those are different
defects in different modules. This distinction is what the two measurements
above rest on, and nothing in the tree exposes it.

Ships first because every later step is diagnosed with it.

### B — the error shape arm

A fourth arm in `codegen_arc_note_local`'s shape test:

- LLVM shape is a 2-element struct, **and**
- `info->goo_type->kind == TYPE_ERROR`, **and**
- field 0 is `i1`, **and**
- field 1 is a pointer

→ `field = 1`.

**No emission change.** The exit already does
`LLVMBuildStructGEP2(..., (unsigned)site->field, ...)`, so an arbitrary field
index works. `-1` and `0` were the only values used, not the only ones
supported.

**Nil safety is already solved, and the guard is load-bearing here.**
`codegen_arc_zero_slot` stores `LLVMConstNull(slot_ty)` immediately after the
alloca. For `{ i1, ptr }` that is `zeroinitializer`, so a nil error reads
`{ false, null }` rather than the `{ i1 false, ptr undef }` the error-union
construction produces. `goo_release` is a no-op on NULL. The guarantee is
FAIL-CLOSED: no release site is recorded unless that store was emitted. This is
the machinery PR #265 and #267 built for a different shape, and it is what makes
this arm safe without a conditional release on the nil flag.

**Both halves are checked**, matching the slice and string arms. The element
count and the leading field together separate an error from a string: a string
is `{ ptr, i64 }` and leads with a pointer, an error is `{ i1, ptr }` and leads
with a flag. Neither test can match the other's shape.

**The hazard this arm creates.** `errors.Unwrap` returns `e->cause`, a pointer
*into* its argument. It carries `non_retaining = 0` precisely for that reason,
so condition 2 already refuses it — but before this arm, a wrong answer there
was inert, and after it a wrong answer frees the caller's error. It needs a row
and a probe that reads the unwrapped error's bytes after the call.

### C — tuple-destructure ownership

In `release_decision.c`'s `AST_MULTI_ASSIGN` arm, when a short declaration has
one value and several targets, attribute that value to **every** target instead
of recording `NULL`.

Soundness comes from an existing definition rather than a new one. Both
ownership sources are already defined over the whole result list:

- `shim_signature_is_non_retaining` — "does not retain a pointer argument past
  the call, AND does not return a value that aliases one".
- `ParamEscapeSummary.return_escapes` — "does F return a value derived from one
  of its own params?"

Neither is per-result, and neither needs to be: if no result aliases an
argument, then every result is owned. So `binding_is_owned` gives the right
answer per target with no new machinery.

Non-pointer results are filtered downstream. `n` in `n, err := Atoi(f)` is an
`i64` slot, which step B's shape test refuses — the same two-layer split that
lets the integer `+` arm in `binding_is_owned` stay deliberately approximate.
Do NOT try to recover precision by type-checking here; `release_decision` holds
no types, and that is the property that keeps it table-testable.

**Rejected alternative — a per-result ownership column in the shim table.** More
precise, considerably larger, and **no shim today returns a mixed tuple**, so it
buys nothing measurable. Revisit only when one does.

### D — CFG, liveness, and scope-exit release

New module `src/types/cfg.c` plus `include/cfg.h`, alongside its four siblings
and free of LLVM, so it stays pure and table-testable for the same reason
`release_decision` is. Codegen consumes a decision; it does not compute one.

**Scope, deliberately narrow.** The CFG serves liveness for release placement
only. The escape passes stay monotone and location-insensitive — `escapes[]`
keeps its "only ever set true" rule, `escape_env_add_or_union` keeps unioning,
and the four existing row matrices (param_escape 25 rows / 155 assertions,
block_escape 33 / 109, local_escape 32 / 40, release_decision 34+5+10 / 142)
carry no regression risk. Flow-sensitive escape — the kill rule proper — is a
separate decision to be made on its own measurement, not folded in here.

**Condition 4 relaxes** from "bound once at function scope" to "bound once per
entry to its declaring block".

**Placement is at scope exit on every exit path**, reusing the enumeration
above. Not at last use: a `defer` can read a local through a closure after its
final syntactic use, which is the release-versus-defer ordering that
`local_escape`'s `defer_is_like_go` comment names as a blocker. Scope-exit
placement stays behind defers, matching `codegen_emit_deferred_calls`, so that
ordering question does not have to be opened.

**The loop-carried hazard, which is the one genuinely new soundness question.**
A local declared inside a loop body whose value is stored into something
declared outside it must not be released at iteration end. That store is an
escape from the block, and `block_escape` — the same engine with the boundary
moved to a block — already has the sink vocabulary for it. Whether it can be
retargeted from arena blocks to any block, or needs the CFG's own liveness, is
the first thing step D must settle **by measurement**, before any emission is
written.

## Gates

Each step lands with its own, and none is judged on the daemon except D.

- **A** — no gate of its own; it is an instrument. Its own correctness is shown
  by the two fixtures in "The measurement that changed the plan", which must
  print the recorded verdicts.
- **B** — `release_decision_test` rows for an owned error and for
  `errors.Unwrap`; an `arc-release-probe` differential on a function-scoped
  single-name error; an `errors.Unwrap` probe that reads the unwrapped error's
  bytes after the call.
- **C** — rows for `n, err := Atoi(s)` (owned), for a tuple from a Goo callee
  whose `return_escapes` is true (refused), and for `a, b := x, y` (two values,
  two targets — unchanged behaviour).
- **D** — CFG row table; a loop-scoped release probe covering fall-through,
  `continue`, `break`, labelled forms, `goto` and `return`; a loop-carried
  store probe that must refuse; and the daemon measurement.

**Every gate must be shown able to report a failure before it is believed.** PR
#274 shipped a gate that passed against its own mutant, and PR #275's first
mutation attempt silently did not apply — a `sed` whose `|` delimiter collided
with a `||` in the pattern, which left the file untouched while the suite
reported PASS. Apply mutations with an asserting replace, and diff the file
before believing any result.

## Risks

- **Step B widens the blast radius of a wrong ownership answer on errors.**
  Before it, condition 2's verdict on an error local was inert. After it, a
  wrong `RELEASE_OK` frees a live error. `errors.Unwrap` is the specific shape
  to pin.
- **Step D's loop-carried store is the real soundness question**, and it has no
  analogue in the arena work: an arena frees a region whose lifetime the block
  defines, while a per-iteration release frees one named value that a later
  iteration might still reach.
- **Nothing before D moves the daemon.** Resist the temptation to report A, B or
  C as progress against the 340,000. They are progress against the guards that
  make the 340,000 reachable, which is not the same claim.

## Related

- ADR 0002 — the memory model this serves.
- ADR 0004 — the escape formulation, and the kill-rule costing this corrects.
- `.handoff.md` — NEXT item 2, and the ledger row recording that an append
  element-marking mutation turns no gate red.
- PR #275 — the immediately previous increment, and the source of the
  measurement discipline cited under Gates.
