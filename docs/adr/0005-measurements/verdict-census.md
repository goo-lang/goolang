# The verdict census — condition 2 is second, and handoff item 4 is worth four locals

Taken 2026-08-03 against `791649f`, by `./scripts/arc_reason_census.sh`.
600 programs (`examples/*.goo` plus `bench/*/*.goo`), 0 failed to compile.

**This is a different question from `reason-census.md`, and the two must not be
read as the same table.** A REASON says why the escape analysis marked a local.
A VERDICT says which condition in `release_decision` refused it. The census
printed exactly one verdict before today — `RELEASE_OK` — so the relative size
of the conditions had never been measured at all.

Unlike the reason tallies, these are one row per local, so they SUM to the row
count: 14,667 all, 3,725 user code.

## The tally

| Verdict | all rows | user code |
|---|---|---|
| `RELEASE_NO_NO_BINDING` | 12,474 | 1,532 |
| `RELEASE_NO_ESCAPES` — condition 1 | 1,039 | **1,039** |
| **`RELEASE_NO_NOT_OWNED` — condition 2** | 586 | **586** |
| **`RELEASE_OK`** | 365 | **365** |
| `RELEASE_NO_LOOP_SCOPE` — condition 4 | 116 | 116 |
| `RELEASE_NO_UNKNOWN` | 31 | 31 |
| `RELEASE_NO_ARENA` — condition 3 | 26 | 26 |
| `RELEASE_NO_ALIASED` — condition 7 | 17 | 17 |
| `RELEASE_NO_BLOCK_ESCAPE` — condition 6 | 13 | 13 |
| `RELEASE_NO_REBOUND` | **0** | **0** |

`RELEASE_NO_NO_BINDING` is the only verdict whose all-rows figure dwarfs its
user-code one, for the same cause `reason-census.md` records: vendored goostd is
re-analysed once per importing program.

**THE SAME LIMIT APPLIES HERE AS THERE, AND IT IS NOT A FOOTNOTE: THIS COUNTS
LOCALS, NOT BYTES.** Six of the thirteen `BLOCK_ESCAPE` rows below turn out to
be integers and floats, which is exactly the kind of thing a local count cannot
see. Do not turn any row of this table into a work plan without opening the
programs, which is what the next section does for one row.

## Handoff item 4, measured — and its premise was wrong twice

`.handoff.md` item 4 read: "**Condition 6 still refuses a loop-declared rebound
local**, and NO measurement has been taken since ADR 0005 landed.
`for { x := mk(); x = mk(); total = total + len(x) }` reclaims nothing. Take the
measurement before planning." This is that measurement, and it says: do not plan
it.

That shape carries TWO candidate causes at once, so measuring it alone cannot
say which refuses. Four functions separate them (fixture kept out of tree; it is
reproduced verbatim at the end of this file):

| Function | shape | verdict for `x` | reclaims |
|---|---|---|---|
| `control` | loop-local, no rebind, no mention | `RELEASE_OK` | yes |
| `reboundOnly` | `x := mk(); x = mk()` | **`RELEASE_OK`** | **yes, BOTH allocations** |
| `mentionOnly` | `total = total + len(x)` | `RELEASE_NO_BLOCK_ESCAPE` | no |
| `reboundAndMention` | the handoff shape | `RELEASE_NO_BLOCK_ESCAPE` | no |

At 500 iterations, under `--leak-check=full`:

| build | in use at exit |
|---|---|
| `GOO_ARC_RELEASE=0` | 111,000 bytes / 3,000 blocks |
| default | 55,500 bytes / **1,500 blocks** |

The accounting closes to the block. 3,000 = 500 x 6 allocations per iteration
across the four functions. The 1,500 reclaimed are `control` (1) and
`reboundOnly` (2); the 1,500 left are `mentionOnly` (1) and `reboundAndMention`
(2). No `Invalid read`, `Invalid write` or `Invalid free` in either build.

### Finding 1 — the rebind is not a cause, and it never was

`reboundOnly` reads `RELEASE_OK` and reclaims both of its per-iteration
allocations. The reassign release (#285) and the loop-scoped release (#276)
already cover this shape between them.

### Finding 2 — `RELEASE_NO_REBOUND` is a DEAD VERDICT, and its comment lied

Zero rows in 600 programs, and that is not a corpus gap: **no line in `src/`
ever assigns it.** `grep -rn RELEASE_NO_REBOUND src/ include/` returns only the
enum member, the name-table `case`, and comments. A rebound local at function
scope also reads `RELEASE_OK`, so there is no shape that produces it.

Its comment in `include/release_decision.h` read `condition 4: assigned again
after declaration` — a live-sounding description of a rule the module lost. That
comment is where item 4 came from. It now says `DEAD: no producer in src/, 0
rows in the corpus`, with the cause beside it.

**This is the fifth time in this arc that a stale claim, not a stale
measurement, set the work plan.** The handoff did not measure and get a wrong
number; it read a comment and believed it.

### Finding 3 — condition 6 is worth at most FOUR locals, corpus-wide

13 rows, and opening every one of them:

| Class | Rows | Reachable by a precision change? |
|---|---|---|
| **scalars** — `handle: n` is `int` from `strconv.Atoi`; jacobi `delta`/`local`/`d` and allreduce `local`/`big` are `float64` | **6** | NO. No heap allocation, so refusing them costs zero bytes. |
| **hazards that must stay refused** — `carried: s` (`keep = s`), `switchBreak: keep` and `switchBreak: s` | **3** | NO. Releasing these dangles, and `switchBreak` is the PR #278 use-after-free. |
| remaining candidates — `arc_concat_operand main: s`, `arc_release_borrowed_elem main: echoed`, `arc_release_split_escape main: joined`, `closure_probe main: g` | **4** | At most. `g` is `fs[k]`, a borrowed slice element condition 2 refuses on its own. |

So the ceiling is 4 locals in 600 programs, against 365 released, on the one
condition whose header states that a wrong `true` frees live memory. The
refinement that would unlock them — the inverse of condition 2's binding table,
"does this expression PROPAGATE its operand or merely READ it" — is real work
with a real soundness surface. **The measurement does not support paying for
it.** Closed rather than deferred.

## What this reorders, and it is not on any list

**`RELEASE_NO_NOT_OWNED` refuses 586 user locals — more than the 365 that
release — and condition 2 is not an item in `.handoff.md` at all.** Every
handoff item from the ARC arc has been about condition 1 (the escape reasons) or
condition 6. Condition 2 is 45x item 4 and 1.6x the entire released population.

It also has a documented, concrete, already-diagnosed cause. From `#289`, and
from condition 2's own table in `include/release_decision.h`:

> a call to a Goo function — owned iff its `ParamEscapeSummary.return_escapes`
> is FALSE

and from the multi-assign work:

> condition 2 cannot prove a Goo callee returns fresh memory — `param_escape`'s
> `return_escapes` describes the returned VALUE, not its freshness

**That is a NAMED gap in a summary, not an unknown.** Whether 586 splits mostly
into that shape or into the borrowed-view rows (`a slice, index or selector
expression`, `another local`) is the next measurement, and this file does not
take it. **Do not quote a number for that split.**

## Instrument notes

- **The verdict tally is new code in `arc_reason_census.sh`, and it broke the
  script on the first attempt.** The awk program is a single-quoted shell string.
  An apostrophe in a comment ended it, and bash then tried to run awk syntax.
  The script still wrote an output file with a correct-looking header and a
  MISSING table, so reading only the top would have shown a clean census.
  A `NO APOSTROPHE` note now sits in that block.
- **`$?` after this script is `tee`, never the script.** The pipeline ends in
  `tee "$OUT"`, so an exit status read from the pipeline says nothing. Read the
  body for `syntax error` and `awk:`, which is what caught the defect above.
- **Cross-checked against an independent sweep.** The same corpus was tallied
  by a separate one-line awk over a kept raw file before this went into the
  script, and every cell agrees.
- The verdict counter increments BEFORE the arms that `next`, because three of
  them skip the rest of the body and a counter placed after one would
  under-report silently.

## Reproducing

```sh
./scripts/arc_reason_census.sh        # both tallies; 600 programs, ~2 min
GOO_ARC_DEBUG=1 ./bin/goo -o /dev/null prog.goo   # one program, verdict + reasons
```

The item 4 fixture, which is deliberately NOT in `examples/` because it gates
nothing and its whole result is "do not do this work":

```go
package main

import (
	"fmt"
	"strings"
)

func control(n int) int {
	hits := 0
	for i := 0; i < n; i++ {
		x := strings.TrimSpace("  aaaaaaaaaaaaaaaaaaaa  ")
		if len(x) == 20 {
			hits = hits + 1
		}
	}
	return hits
}

func reboundOnly(n int) int {
	hits := 0
	for i := 0; i < n; i++ {
		x := strings.TrimSpace("  bbbbbbbbbbbbbbbbbbbb  ")
		x = strings.TrimSpace("  cccccccccccccccccccc  ")
		if len(x) == 20 {
			hits = hits + 1
		}
	}
	return hits
}

func mentionOnly(n int) int {
	total := 0
	for i := 0; i < n; i++ {
		x := strings.TrimSpace("  dddddddddddddddddddd  ")
		total = total + len(x)
	}
	return total
}

func reboundAndMention(n int) int {
	total := 0
	for i := 0; i < n; i++ {
		x := strings.TrimSpace("  eeeeeeeeeeeeeeeeeeee  ")
		x = strings.TrimSpace("  ffffffffffffffffffff  ")
		total = total + len(x)
	}
	return total
}

func main() {
	n := 500
	fmt.Println(control(n))
	fmt.Println(reboundOnly(n))
	fmt.Println(mentionOnly(n))
	fmt.Println(reboundAndMention(n))
}
```

**Read the block count, not the byte total.** `main` reads `os.Args` and the
runtime copies `argv[0]` to the heap unfreed, so the byte figure moves with the
output path length while the block count holds.
