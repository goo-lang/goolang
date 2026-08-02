# The reason census — CALLEE_VALUE is 138 locals, and it is NOT the ceiling

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
