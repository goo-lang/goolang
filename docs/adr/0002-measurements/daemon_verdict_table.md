# The daemon verdict table

Date: 2026-08-01
Compiler: `bin/goo` at `549d0bb` (branch `feat/t4-error-release-unblock`)
Command: `GOO_ARC_DEBUG=1 ./bin/goo build bench/daemon/daemon.goo -o /tmp/daemon_probe`

The `[arc?]` instrument landed in `36c1c6e`. Until now nobody had run it against
`bench/daemon/daemon.goo`, which is the program ADR 0002 exists to fix. This file
is that run.

## Why it exists

`.handoff.md` attributed `err`'s refusal to condition 4. Commit `36c1c6e`'s own
message records that condition 2 refused it first, and that the attribution had
been wrong for a session. The instrument was built to stop that repeating. It
only stops it if someone runs it and writes the answer down.

## The table — `handle`, the per-request function

| Local | Line | Verdict | Note |
|---|---|---|---|
| `req` | 22 | `RELEASE_NO_NO_BINDING` | a parameter, not a declared local |
| `fields` | 23 | **`RELEASE_OK`** | owns its elements, stride=16 (PR #275) |
| `counts` | 24 | **`RELEASE_OK`** | dtor=`goo_map_dtor` |
| `total` | 25 | `RELEASE_NO_REBOUND` | `int`, so no bytes |
| `i` | 26, 37 | `RELEASE_NO_LOOP_SCOPE` | `int`, so no bytes |
| `f` | 27 | **`RELEASE_NO_ESCAPES`** | see the finding below |
| `n` | 28 | `RELEASE_NO_LOOP_SCOPE` | `int`, so no bytes |
| `err` | 28 | **`RELEASE_NO_LOOP_SCOPE`** | tuple target, reaches condition 4 |
| `parts` | 36 | **`RELEASE_OK`** | owns its elements, stride=16 |

## The table — `main`

| Local | Line | Verdict |
|---|---|---|
| `requests` | 46 | `RELEASE_NO_REBOUND` |
| `n` | 48 | `RELEASE_NO_NOT_OWNED` |
| `err` | 48 | `RELEASE_NO_NOT_OWNED` |
| `last` | 53 | `RELEASE_NO_REBOUND` |
| `i` | 54 | `RELEASE_NO_LOOP_SCOPE` |

## Finding 1 — the `f` record does NOT belong to the kill rule

`.handoff.md` NEXT item 3 costs `f := strings.TrimSpace(fields[i])` at 262,000
bytes per 2,000 requests, and splits it two ways:

> ~82,000 (8.6%) is the `counts[strings.ToUpper(f)]` branch, where `f` is dead
> after the call and the map never sees it. That half needs the SAME loop-scope
> kill rule as item 2, not key ownership.

**That attribution is wrong.** `f` reads `RELEASE_NO_ESCAPES`, not
`RELEASE_NO_LOOP_SCOPE`. Condition 1 refuses it, so condition 4 never runs on
it. The loop-scoped kill rule cannot reclaim any part of the 262,000-byte
record, including the 82,000 half.

The cause is visible in the source. `f` has ONE declaration site at line 27 and
TWO uses. Line 31 writes `counts[f]`, and the write subscript sink marks `f`
escaping. The escape engine is location-insensitive by ADR 0004, and `escapes[]`
is only ever set true, so the mark from the `counts[f]` branch refuses `f` in
the `ToUpper` branch too.

Consequence: the whole `f` record rides on the escape verdict changing. That is
the map READ-arm relaxation plus per-site escape facts, then key ownership — not
the kill rule. Do not book any part of 262,000 against the kill-rule work.

## Finding 2 — `err` confirms the kill-rule premise, and needs BOTH fixes

`handle: err` reads `RELEASE_NO_LOOP_SCOPE`. So `err` now reaches condition 4,
and the 340,000-byte record does belong to the kill rule.

But compare `main: err`, the same `n, err := strconv.Atoi(..)` shape outside a
loop. It reads `RELEASE_NO_NOT_OWNED` — the tuple-destructure arm recording
NULL, which the open Task 3 fixes.

Both refusals are live on the same local in `handle`. The conditions are ordered
so `LOOP_SCOPE` answers first and hides the ownership refusal behind it. So:

- The kill rule alone leaves `handle: err` at `NOT_OWNED`.
- Tuple ownership alone leaves it at `LOOP_SCOPE`.

**The 340,000 bytes need both changes. Neither pays on its own.** Measure them
together, and do not expect a partial number from shipping one.

## Finding 3 — three locals already reclaim

`fields`, `counts` and `parts` all read `RELEASE_OK`. Two of them carry the
owned-elements column and one carries the map destructor. That is the shipped
T4 work visible on this program.

## What this instrument proves about itself

The run reports six distinct verdicts — `RELEASE_OK`, `NO_ESCAPES`,
`NO_LOOP_SCOPE`, `NO_REBOUND`, `NO_NOT_OWNED`, `NO_NO_BINDING` — and three of
them appear on locals inside `handle` alone. So the instrument discriminates
within the function under test, rather than reporting one value for everything.
No separate teeth check was needed: `parts` reading `RELEASE_OK` in the same
function is the proof that `f` reading `NO_ESCAPES` is a real verdict and not a
stuck default.
