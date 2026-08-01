# ADR 0005 — an escape is recorded as a SET OF REASONS, not a boolean

Date: 2026-08-01
Status: accepted as a DIRECTION. Nothing here is implemented, and this ADR
changes no code.

Supersedes nothing. Refines ADR 0002's elision pass and ADR 0004's formulation.

## Context

The escape passes answer one bit per slot: does this value outlive its boundary.
Three consumers read that bit, and by 2026-08-01 two separate pieces of work had
stalled on the same thing — **the bit conflates causes that need different
answers**.

### The measurement that forced it

`bench/daemon/daemon.goo` spends its last 262,000 bytes per 2,000 requests on
`f := strings.TrimSpace(fields[i])`, used as a map key by `counts[f] = counts[f]
+ 1`. The runtime already has everything needed to reclaim it:
`goo_map_set_sv_owning` sets `owns_key`, `goo_map_clear_sv` releases it,
`goo_map_dtor` calls clear, and a duplicate incoming key is released rather than
dropped.

The gap is one call. `release_plan_key_is_owned` asks `binding_is_owned` on the
key expression, and that answers FALSE for a bare identifier. So
`counts[strings.ToUpper(f)]` in the same function qualifies and `counts[f]` does
not — a difference in SPELLING, not in ownership. Letting an identifier key
qualify was measured at **262,000 -> 82,000 bytes, 0 invalid accesses, correct
output**: 180,000 bytes, 68.7%.

It was reverted, because it is unsafe, and `arc-map-key-local-probe` caught it:

```go
func escapesByReturn(x string) string {
	m := map[string]int{}
	s := strings.TrimSpace(x)
	m[s] = 1
	return s        // the CALLER still holds s
}
```

Both `f` and `s` read `escapes = true`. For `f` the cause is "a map-key store
marked it", and handing the buffer to the map is exactly right. For `s` the
cause is "it leaves the function", and handing it to the map frees a buffer the
caller holds — an invalid read in `goo_strings_to_upper`. **One bit, two causes,
opposite correct actions.**

### The second consumer, stalled on the same bit

Condition 6 refuses a loop-declared local whose name appears in the right-hand
side of a store whose target outlives the iteration. Its own comment calls this
"A MENTION, NOT A FLOW". It blocks `counts[g] = counts[g] + 1` and it blocks an
ordinary accumulator such as `total = total + len(x)` — measured this session on
a loop-declared rebound local, which reclaims nothing for that reason.

## Decision

**Replace the escape boolean with a set of reason flags.** `escapes` becomes
`reasons != 0`, so no consumer that only wants "did it escape at all" changes.

Reasons follow the sinks the engine already enumerates: RETURN, GLOBAL_STORE,
CALL_RETAIN, GO_ARG, DEFER_ARG, CHAN_SEND, MAP_KEY, and the conservative
catch-all a default arm raises.

`escape_core.c` has 21 `escape_mark` call sites. Each names its reason.

## Alternatives, and why they lost

**A second boolean** (`escapes_beyond_function` beside `escapes`). Smallest
diff, and it answers the map-key question directly. Rejected: it creates two
pieces of state that must agree and are maintained by different code, which is
the EXACT shape of PR #278's use-after-free — `cfctx.loop_depth` and
release_decision's `block_depth` were assumed equal, and verify-core, 493
goldens, a fresh valgrind probe and an 8-of-8 teeth run were all genuinely green
over the mismatch. Buying that failure mode again to save a day is a bad trade.
It also does nothing for condition 6.

**Answer the narrower question in release_decision instead**, syntactically,
without touching the shared engine. Rejected outright: it re-implements escape
analysis in a second place. `src/runtime/runtime.c` already names that class in
its own words — "a fourth copy of 'should I free this key' is how the two drift
defects in the escape passes started."

## Consequences

- `release_plan_key_is_owned` can accept an identifier key whose local escapes
  ONLY via MAP_KEY. That is the 180,000 bytes, and it becomes safe rather than
  merely measured.
- Condition 6 can distinguish a mention that flows from one that does not,
  which is the remaining half of item 1 and the loop-rebind case together.
- The escape teeth gain a per-reason axis. `scripts/escape_teeth.sh` mutates
  pass-specific conditions today; a reason is exactly the kind of condition it
  is built to mutate, and `scripts/escape_arm_coverage.sh` already has a
  per-arm matrix to extend.

## Risks, stated plainly

- It touches the SHARED engine behind param_escape, block_escape and
  local_escape. Every change there in this leg has needed the arm-coverage
  matrix re-run, and two produced a use-after-free.
- Over-marking stays safe and under-marking dangles a pointer, so a reason that
  is dropped rather than renamed is the dangerous edit. The default arm must
  keep raising the conservative catch-all.

## What does NOT follow from this ADR

The residual **82,000 bytes** after the map-key win are a THIRD class and this
decision does not address them: `f` reaches `counts[strings.ToUpper(f)]` on the
non-numeric branch, so its own buffer is handed to nobody. Ownership that
transfers on one branch and not another cannot be expressed by a single release
site, whatever the escape reason says. Do not scope it into this change.
