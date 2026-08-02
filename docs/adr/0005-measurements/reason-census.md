# The reason census — CALLEE_VALUE is 138 locals, and it is NOT the ceiling

> **UPDATE 2026-08-02 — CALL_RETAIN's 655 is now SPLIT, and 87% of it was the
> analysis declining to answer rather than a measured escape.** The tables below
> were taken before the split and are kept as they were recorded. The split is
> at the end of this file, under "The CALL_RETAIN split". `CALL_RETAIN` in the
> tally below now reads as `CALL_OPAQUE` 578 + `CALL_RETAIN` 86 +
> `CALL_VARIADIC` 12.

Taken 2026-08-02 on `feat/arc-debug-reasons`, `verify-core` ALL GREEN (exit 0).
Command: `./scripts/arc_reason_census.sh`. 599 programs (`examples/*.goo` plus
`bench/*/*.goo`), 0 failed to compile.

**This retires a claim, and it is not the claim anyone expected to retire.**
Four handoffs named the CALLEE_VALUE ceiling as the largest item left and
refused to give a number. The number is now taken, and CALLEE_VALUE is fifth.

## The counts

Rows are LOCALS, one per `[arc?]` line. "User code" excludes `goo_pkg__*`,
because vendored goostd is re-analysed once for every program that imports it
and would otherwise be counted 599 times.

| | all rows | user code |
|---|---|---|
| rows seen | 14,505 | 3,708 |
| released (`RELEASE_OK`) | 361 | **361** |
| refused | 14,144 | 3,347 |
| ...UNANALYSED, `reasons=ALL` | 12,336 | 1,539 |
| ...refused with a real reason set | 1,808 | **1,808** |
| ......of those, `reasons=NONE` | 751 | 751 |

Every released local is user code. Not one vendored goostd local releases.

## The reason tallies

A local may carry several reasons, so these overlap and do not sum. Misses are
excluded — see the next section, which is the load-bearing part.

| Reason | Locals | Share of the 1,057 that escape for a named cause |
|---|---|---|
| **CALL_RETAIN** | **655** | **62.0%** |
| UNCLASSIFIED | 439 | 41.5% |
| CALLEE_VALUE | **138** | **13.1%** |
| RETURN | 104 | 9.8% |
| CLOSURE_CAPTURE | 88 | 8.3% |
| GO_ARG | 89 | 8.4% |
| SUBSCRIPT_STORE | 67 | 6.3% |
| CONTAINER_STORE | 42 | 4.0% |
| DEFER_ARG | 20 | 1.9% |
| GLOBAL_STORE | 18 | 1.7% |
| CHAN_SEND | 10 | 0.9% |

1,057 = the 1,808 refusals with a real set, less the 751 whose set is `NONE`.
A `NONE` set means an escape reason refused nothing and some OTHER condition
did — loop scope, not owned, block escape, rebound, aliased.

## THE TRAP THIS MEASUREMENT HAD TO AVOID

**A miss reads as `ESCAPE_REASON_ALL`, and counting it as eleven reasons
inflates every tally beyond use.** `local_escape.h` requires the fail-closed
answer, so an unknown function, an unknown local, and a PARAMETER all report
every reason at once. 1,539 user rows read that way.

Cross-tabbed against the verdict, those rows are:

| Verdict | Rows |
|---|---|
| `RELEASE_NO_NO_BINDING` | 1,528 |
| `RELEASE_NO_ESCAPES` | 9 |
| `RELEASE_NO_UNKNOWN` | 2 |

So they are parameters and the like, refused by the no-binding condition
BEFORE any escape question is asked. The `ALL` is correct rather than a
defect, and attributing it to a reason would have credited the ceiling with
1,528 locals it has nothing to do with. The script counts `ALL` as UNANALYSED
and never as a reason.

## What this changes

- **CALLEE_VALUE is not the largest item.** 138 locals, 3.7% of user locals,
  13.1% of those refused for a named escape cause. Real, and fifth.
- **CALL_RETAIN is the ceiling**, at 655 — 4.7x CALLEE_VALUE. Any local handed
  to a callee whose summary says it retains gets marked, AND
  `escape_core.c`'s call arm sets `retains = true` for every external or
  unregistered callee, which is pure-conservative by design.
- **UNCLASSIFIED at 439 is the second**, and it is the catch-all a default arm
  raises. A reason set that is 41.5% "I do not know" is itself the finding.

## WHAT THIS DOES NOT SAY, and the limit is severe

**THIS COUNTS LOCALS, NOT BYTES.** A refused local may hold 8 bytes or 8
megabytes, and nothing here distinguishes them. The daemon's whole 180,000-byte
win came from ONE local. So this table ranks how OFTEN a reason refuses, and it
must not be read as how much memory each reason costs. Anyone turning this into
a work plan needs the byte measurement as well, and that measurement does not
exist yet.

It also does not split CALL_RETAIN's 655 between two very different causes: a
callee whose `param_escape` summary genuinely reports retention, and a callee
that is external or unregistered and therefore treated as retaining without
evidence. Those want opposite fixes — the second is precision that is there for
the taking, the first is a real escape. **That split is the next measurement,
and it is not taken here.** Do not quote a number for it.

---

# The CALL_RETAIN split — 578 of it was never a measurement

Taken 2026-08-02 on `feat/arc-split-call-retain`, `verify-core` ALL GREEN
(exit 0). Command: `./scripts/arc_reason_census.sh`, 600 programs, 0 failed.
(600 rather than the 599 above: `examples/arc_multi_assign_probe.goo` joined the
corpus in #289.)

**The previous section named CALL_RETAIN the ceiling at 655 and said the split
between "genuine retention" and "treated as retaining without evidence" was the
next measurement, with no number. Here is the number.**

## The three causes

`escape_core.c`'s call arm has five ways to decide `retains`, and three of them
mark. They were one bit.

| Reason | Locals | What it means |
|---|---|---|
| **`CALL_OPAQUE`** | **578** | Nobody looked. External, unregistered, or no summary. |
| `CALL_RETAIN` | 86 | `param_escape` READ the callee and measured that it keeps the argument. |
| `CALL_VARIADIC` | 12 | A summary exists and is silent about this position — a spread, or an argument past the parameter count. |

**87% of what the old bit reported was the analysis declining to answer.** Only
86 locals had a measurement behind them.

`CALL_RETAIN` is a fact about the PROGRAM. `CALL_OPAQUE` is a fact about the
ANALYSIS. Only the second can be removed by making the analysis better, and one
bit for both told a reader nothing about which they faced. That is ADR 0005's
own argument, applied a second time.

## What the split is WORTH, which is a smaller number

Bit counts overstate the prize, because a local carrying `CALL_OPAQUE` may be
refused by three other reasons as well. The number that matters is locals whose
reason set contains ONLY no-evidence bits — the ones that would reach
`reasons == NONE` if these arms became precise:

| | user-code locals |
|---|---|
| released today (`RELEASE_OK`) | 365 |
| **refused, reasons are ONLY no-evidence** | **216** |
| refused, carries at least one evidenced reason | 843 |

**All 216 read `RELEASE_NO_ESCAPES`** — condition 1 is what refuses them, and
the no-evidence mark is the sole cause. So 216 is the UPPER BOUND on what
precision here would unlock, against 365 released today. Not one of them is
promised: each would still face conditions 2 through 7.

## DO NOT READ THE 578 AS A WORK PLAN — the reachable fix is much narrower

Tallying the callee at each `CALL_OPAQUE` mark (temporary instrumentation, not
committed) gives, by MARK rather than by local:

| Callee | Marks |
|---|---|
| `make` | 15,670 |
| `rune` | 14,260 |
| *(non-identifier callee — methods)* | 11,943 |
| `uint` / `uint64` / `uint32` / `int` | ~27,000 combined |
| `append` (args 1..n) | 4,364 |
| `panic` | 3,572 |
| `copy` | 3,034 |
| `error`, `string`, `byte`, `new`, `close`, `delete`, `min`, `max` | smaller |

**These are BUILTINS AND TYPE CONVERSIONS, not unregistered functions.** The
handoff guessed the fix was "register the external callees"; it is not. Most of
these arguments are INTEGERS that no release path would ever have freed, so
their marks cost nothing at all.

A numeric conversion cannot retain — `uint64(x)` produces a new value. `make`
takes sizes. But `panic(x)` genuinely propagates out of the function, `copy`
writes one buffer into another, and `string([]byte)` allocates. **The safe set
is smaller than the list and has not been determined here.** The next step is to
decide, one builtin at a time and with a row each, which belong on
`goo_callee_is_non_retaining`'s whitelist — NOT to add them in bulk.

## What is pinned

- `local_escape_test` rows **49** (`CALL_OPAQUE`, unregistered callee) and
  **50** (`CALL_VARIADIC`, spread). Row **41** is unchanged and is the
  discriminating case: a resolved callee with a real summary still reads
  `CALL_RETAIN` alone, which is what proves the split left the evidenced path
  untouched. 74 assertions, was 70.
- `escape_teeth.sh` `CORE_EXPECTED` 11 → **13**, with `reason-call-opaque` and
  `reason-call-variadic` added and `reason-call-retain` repointed at the
  default initialiser. All 13 report CAUGHT.
- The default in the loop is the EVIDENCED reason on purpose, so an arm that
  forgets to name its cause claims evidence it does not have — and
  `reason-call-retain` is what catches that.
