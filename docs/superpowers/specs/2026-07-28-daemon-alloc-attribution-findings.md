# Where the daemon's 1,377 bytes per request actually go — and why the planned first ARC release reclaims none of them

**Date:** 2026-07-28
**Status:** findings. Two measurements, one of which invalidates the planned
next increment and one of which is a soundness defect.
**Reproduce:** `scripts/daemon_alloc_attribution.sh [massif_requests] [dhat_requests]`

## Why this exists

ADR 0002 measured that `bench/daemon` retains about 1.63 KB per request
forever. It never said which allocation sites hold it. `.handoff.md` then
rejected two release shortcuts on estimates derived by reading the source, and
it corrected itself once on exactly this point, calling its earlier claim
"WRONG about its own payoff".

This document replaces the estimate with a measurement, so the scope of the
first ARC release consumer is chosen from evidence.

## Instrument

Two tools, because they answer different questions and disagree.

- **massif** gives LIVE bytes at the peak snapshot. That is the retention.
- **dhat** gives BLOCK COUNTS and total-allocated. That is the retain/release
  traffic ARC would emit, and massif cannot see it.

Reading dhat's total as "the leak" overstates `goo_slice_append` by 2.1x,
because `parts` doubles 1-2-4-8 and `realloc` frees three of the four buffers.
Reading massif alone hides that the program makes 47 allocations for each
request whose average payload is about 15 bytes.

### The instrument was verified before its output was trusted

The table must be able to report a different answer. The request string was
widened from 7 fields to 14, and each site moved as the source predicts:

| Site | 7 fields | 14 fields | Expected |
|---|---|---|---|
| `goo_strings_trim_space` | 14 blk/req | 28 blk/req | 2x, two loops over each field |
| `goo_map_set_sv` | 7 blk/req | 14 blk/req | 2x, one entry for each distinct key |
| `goo_string_new_with_length` (Split parts) | 7 blk/req | 14 blk/req | 2x |
| `goo_slice_append` | 4 blk/req | 5 blk/req | one more doubling, 1-2-4-8-16 |
| `goo_error_wrap` | 2 blk/req | 3 blk/req | one more non-numeric field ("ten") |

Retention also stayed exactly linear: 1,377 B/request at both 2,000 and 50,000
requests, and the massif total agrees with the peak useful-heap figure
(68,854,195 B / 50,000).

## Result 1 — the attribution

LIVE bytes retained for each request, at 50,000 requests. Total 1,377 B in 44
blocks.

| B/req | share | blk/req | Site | Reachable by a release on a LOCAL? |
|---:|---:|---:|---|---|
| 280 | 20.3% | 7 | `goo_map_set_sv [runtime.c:930]` | no — codegen cannot enumerate a map's keys |
| 262 | 19.0% | 14 | `goo_strings_trim_space [runtime.c:782]` | `f`, a string local |
| 144 | 10.5% | 4 | `goo_slice_append [runtime.c:1148]` | `parts`, a slice local |
| 128 | 9.3% | 1 | `goo_strings_split [runtime.c:739]` | `fields` backing array |
| 118 | 8.6% | 6 | `goo_string_new_with_length` <- `goo_strings_split` | `fields` elements |
| 103 | 7.5% | 4 | `goo_string_concat [runtime.c:510]` | temporaries, bound to no local |
| 90 | 6.5% | 2 | `goo_error_wrap [runtime.c:303]` (message copy) | `err`, an error local |
| 80 | 5.8% | 2 | `goo_error_wrap [runtime.c:301]` (error struct) | `err` |
| 41 | 3.0% | 2 | `goo_strings_map_case` <- `ToUpper` | temporary, bound to no local |
| 40 | 2.9% | 1 | `goo_map_new_sv [runtime.c:912]` | `counts` |
| 37 | 2.7% | 2 | `goo_int_to_string [runtime.c:527]` | temporaries |
| 35 | 2.5% | 1 | `goo_strings_join [runtime.c:761]` | temporary |
| 19 | 1.4% | 1 | `goo_string_new_with_length` <- `goo_string_new` | Split tail |

Three notes that change how the numbers read.

- **No single site dominates.** The top three are 20.3%, 19.0% and 10.5%.
  There is no one fix that moves the total by half.
- **The error path is 12.3%, not the 106 B the ledger records.** Measured at
  170 B in 4 blocks for each request, from `strconv.Atoi` failing on "four" and
  "six". The ledger row under-counts by 60%.
- **44 live blocks for each request, average payload about 15 bytes.** The
  allocation COUNT is the striking figure, not the byte total. At the measured
  10.17 ns for each atomic retain/release pair, a uniform ARC would add at
  least 44 pairs for each request.

### The ARC header costs much less RSS than it costs nominally

44 live blocks x 16 B = 704 B/request of header, against a measured RSS delta
of about 202 B/request (82.6 MB to 92.7 MB at 50,000 requests). glibc rounds a
chunk to 16 bytes with a 32-byte minimum, so a 3-byte `TrimSpace` result sat in
a 32-byte chunk before the header and still does. The 12.7% figure is real, and
the nominal 51% is what it would cost with a tighter allocator.

## Result 2 — the planned T4 reclaims ZERO on this benchmark

The plan scoped the first release consumer to string locals, and expected
`f := strings.TrimSpace(fields[i])` to give back 19.0%. It does not.

`bench/daemon` cannot be measured through the pass directly: its imports do not
resolve in a standalone harness, every stdlib call then reads as an
unsummarised external, and the verdicts go conservative for that reason rather
than for the reason under test. The shapes were therefore reproduced without
imports and measured through the real `local_escape_analyze`.

| Shape | Source | `f` verdict |
|---|---|---|
| A, control | `f := fresh(req); _ = f` | does not escape |
| **B, the daemon's loop** | `m[f] = m[f] + 1` | **ESCAPES** |
| C | `m[f] = 1` | does not escape |
| D | `parts = append(parts, f)` | ESCAPES |
| E | `f := view(req)` (a borrowed view) | does not escape |

Shape B is `handle`'s counting loop. `f` escapes, so no release is emitted, so
the 19.0% is not reclaimed. Shape D is `parts`, also escaping. **T4 as planned
frees nothing at all on the benchmark it exists to fix.**

The reason shape B escapes is incidental, and shape C shows it: the taint comes
from reading `m[f]` on the RIGHT side of the compound assign, not from storing
`f` as a key. Remove the read and the verdict flips.

Shape E is the case `include/local_escape.h` already warns about: the pass
answers "does the value outlive F", never "does the local own the value". It is
listed here because it confirms the warning holds in practice.

### The pass is precise. The gap is that the stdlib has no summaries

The machinery works wherever it has information:

| Shape | Callee | `f` verdict |
|---|---|---|
| H | `fmt.Println(f)`, on the non-retaining whitelist | does not escape |
| I | `drop(f)`, a user body with a real summary, non-retaining | does not escape |
| J | `keep(f)`, a user body that stores its argument | ESCAPES |

`goo_callee_is_non_retaining` (`src/types/nonretaining.c:42`) whitelists
builtins and `fmt.*` selectors only. A C shim has no Goo body, so
`param_escape_lookup` misses, and `call_taint` then takes its
"external/unregistered/no-summaries: pure-conservative" branch and marks the
argument escaping.

**So every local passed to any stdlib call is marked escaping.** In `handle`,
`f` escapes for three independent reasons: `strconv.Atoi(f)`,
`strings.ToUpper(f)`, and the shape B read. Closing any one of them changes
nothing. This is the dominant blocker, and it sits ahead of every other item
here.

## Result 3 — a map key is an untracked reference, and it is a soundness defect

Shape C flipping to "does not escape" is not only a lost optimisation. Consider
the same store with an escaping map:

```goo
func shapeF(req string) map[string]int {
	m := map[string]int{}
	f := fresh(req)
	m[f] = 1
	return m           // m escapes; f does NOT
}
```

Measured verdicts: `m` ESCAPES, `f` does not escape.

`goo_map_set_sv` stores the key pointer verbatim and never frees it. The
returned map therefore holds `f`. A release consumer acting on this verdict
frees a buffer that a live, returned map still points at — a use-after-free.

`assign_to_lvalue` marks only the RIGHT-hand taint. Any non-identifier lvalue
is a sink for the value assigned, and the INDEX expression of that lvalue is
never tainted. A map key is the one position where a local's pointer is stored
and no sink fires. The slice equivalent (shape G, `append(parts, f)` then
`return parts`) is correctly conservative, because `append` is an ordinary call
and the call sink covers it.

`.handoff.md` records "Map KEYS are a second, separate untracked reference" as
an ARC implementation gap. It is more than that. It is an under-marking in
`local_escape`, and `include/local_escape.h` names under-marking as the only
bug class that can dangle a pointer.

**Not exploitable today**, because nothing emits a release. That is exactly why
it must be closed before a consumer exists, not after.

## What follows, in this order

1. **Give the C shims param_escape summaries.** Nothing else matters until this
   lands: today every local passed to any stdlib call is marked escaping, so a
   release consumer reclaims nothing in any real program. The declaration site
   already exists — `src/types/shim_signatures.c` is a table, and it needs one
   more column. The same column T3 needs for `returns_borrowed`, so the two are
   one change.
2. **Close the map-key sink.** Taint the index expression of a non-identifier
   lvalue, in all three sibling passes. This is a soundness fix and it belongs
   ahead of any release emission, not after it.
3. **Re-scope the first release consumer, after 1 lands.** String locals
   reclaim nothing at present. Re-run the shapes through the pass once the
   shims are summarised, and pick the target from that table rather than from
   this one.
4. **The map is 23.2%** (`goo_map_set_sv` plus `goo_map_new_sv`) and no
   codegen-side release can reach it, which is the map-held-references spec's
   own argument. Item 1 does not change that. This is the largest share that
   only a runtime-side design can reach.

Items 1 and 2 are both edits to the analysis layer, both small, and both
prerequisites. Neither was in the plan this measurement was commissioned to
inform.
