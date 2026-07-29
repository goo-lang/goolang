# PR B: the self-store rule — what it actually reclaimed

Date: 2026-07-29. Compiler: `feat/t4-self-store-rule` @ `b5e425f`, against
`main` @ `2dc072a` (after #258–#269).

`.handoff.md` projected **822,000 bytes** for this change. The measured
reclaim is **640,000**. This file records the gap, because the cause is a
property of the map's ownership rule and not a defect in the change.

## The change

`assign_to_lvalue` marks `rhs_taint` when the lvalue is not an identifier.
For `counts[f] = counts[f] + 1` the right side carries the map's OWN bit out
of the `AST_INDEX_EXPR` arm, so the map was marked escaping on account of its
own contents. The rule subtracts the base's taint before marking, for a plain
identifier base found in the environment.

Direct proof of the mechanism, on `bench/daemon/daemon.goo`:

```
$ GOO_ARC_DEBUG=1 ./bin/goo -o /tmp/d bench/daemon/daemon.goo
# BEFORE (main)
[arc] handle: will release fields at exit (field=0, dtor=none)
[arc] handle: will release parts at exit (field=0, dtor=none)
# AFTER
[arc] handle: will release fields at exit (field=0, dtor=none)
[arc] handle: will release counts at exit (field=-1, dtor=goo_map_dtor)
[arc] handle: will release parts at exit (field=0, dtor=none)
```

`counts` gets a release site for the first time. `field=-1` is the slot's
contents, which is what a map's `i8*` form needs, and `goo_map_dtor` is the
destructor #269 added.

## The numbers, at 2,000 requests

Read from valgrind's SUMMARY lines, not from summing the individual records —
see the instrument note below.

| Build | definitely lost | indirectly lost | TOTAL |
|---|---|---|---|
| release OFF (`GOO_ARC_RELEASE=0`) | 1,216,000 | 1,537,982 | 2,753,982 |
| `main`, release ON | 1,207,982 | 1,002,000 | **2,209,982** |
| this branch, release ON | 1,389,982 | 180,000 | **1,569,982** |

**640,000 bytes reclaimed, 29.0% of what `main` leaves.** Output is identical
between the release-on and release-off builds.

The `main` figure of 2,209,982 reconciles EXACTLY with the total recorded in
`element_scan_spike.md`, which is the check that the instrument still agrees
with the earlier measurement.

## Why 640,000 and not the projected 822,000

The map's 902,000-byte record breaks down like this, and only part of it is
the map's to free:

| Component | Bytes | After the change |
|---|---|---|
| `goo_map_set_sv` — the entry nodes | 560,000 | FREED by `goo_map_dtor` |
| `goo_strings_trim_space` — key strings | 180,000 | freed with their record |
| `goo_strings_map_case` — key strings (`ToUpper`) | 82,000 | **SURVIVES**, indirect -> definite |
| the map header itself | 80,000 | FREED |

`goo_map_set_sv` stores keys VERBATIM (`e->key = k;`) and the map never owns
key storage — `goo_map_delete_sv` documents this, and `goo_map_dtor` correctly
does not free them. So when the map goes away its keys stop being INDIRECTLY
lost and become DIRECTLY lost. They change leak category, not existence.

`.handoff.md` counted the whole 822,000 indirect block as recoverable. The
part of it that is key storage was never the map's to release. Freeing keys is
the same open question as slice elements, and the ledger already records it.

## THE INSTRUMENT NOTE — `definitely lost` went UP

`definitely lost` moved 1,207,982 -> 1,389,982, an INCREASE of 182,000, while
the true total fell by 640,000. A gate reading only the `definitely lost` line
would have reported this correct change as a 182,000-byte REGRESSION.

This is the third time the same partial read has produced a wrong answer on
this work (element-scan spike, the #269 map probe, and now this). The rule
that holds: **read `definitely lost` AND `indirectly lost`, and take the
totals from the SUMMARY lines.**

A second trap found while writing this file: summing the individual loss
records DOUBLE-COUNTS. A `340,000 (160,000 direct, 180,000 indirect)` record
states a whole that already contains its indirect part, and those same bytes
appear again as their own `indirectly lost` record. A naive sum read
4,291,964 against a true 2,753,982. The summary lines are authoritative.

## A control that was not a control

The first measurement ran `GOO_ARC_RELEASE=0 ./build/daemon 2000` and got
output IDENTICAL to the release-on run, which reads as "the change does
nothing". `GOO_ARC_RELEASE` is a CODEGEN-time switch: it leaves the release
plan NULL and emits nothing, so it must be set on `./bin/goo`, not on the
compiled program.

Proving the control was real took one command, and it is the check worth
keeping:

```
$ GOO_ARC_DEBUG=1 ./bin/goo ... | grep -c '^\[arc\]'          # 3
$ GOO_ARC_RELEASE=0 GOO_ARC_DEBUG=1 ./bin/goo ... | grep -c '^\[arc\]'  # 0
```

The two binaries are the same SIZE and differ from byte 209, so a size
comparison would not have caught it either.

## What is left in the daemon

At 1,569,982 bytes per 2,000 requests, after this change:

| Origin | Bytes | Share |
|---|---|---|
| `goo_error_from_string` — `strconv.Atoi` failures | 340,000 | 21.7% |
| `goo_strings_trim_space` | 262,000 x2 | 33.4% |
| `goo_string_new_with_length` — `strings.Split` elements | 235,982 | 15.0% |
| `goo_strings_map_case` — the map's keys | 82,000 | 5.2% |
| the rest (concat, int_to_string, join) | ~348,000 | 22.2% |

The `strconv.Atoi` error objects are now the largest single item. They are
loop-scoped locals, so release_decision condition 4 refuses them — the CFG
problem, not a container problem.
