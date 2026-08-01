# ADR 0005 result — 180,000 bytes, and the two things the ADR got wrong

Every number below comes from a run on this tree, paired with
`baseline-before.md`. Read that file first: it records what these are measured
against, and the `argv[0]` trap that makes the daemon's byte total depend on the
output path.

## 1. The daemon — 262,205 -> 82,205

```bash
./bin/goo -O2 -o /tmp/daemon bench/daemon/daemon.goo    # SAME PATH as the baseline
valgrind /tmp/daemon 2000
```

| | before | after |
|---|---|---|
| in use at exit | 262,205 bytes | **82,205 bytes** |
| blocks | 14,003 | **4,003** |
| frees | 80,001 | **90,001** |
| ERROR SUMMARY | 0 errors | **0 errors** |

**180,000 bytes, 68.7% of what was left**, and it is the reverted spike's
prediction to the byte. The two agree by different routes — the spike let an
UNSAFE rule through and measured the same allocations — which is evidence the
rule reaches the intended set and nothing more.

Output is `23` with release on, `23` with release off (`GOO_ARC_RELEASE=0`), and
`23` from `go run bench/daemon/daemon.go 2000`.

The `f` that held those bytes:

```
handle: f -> SUBSCRIPT_STORE        (was SUBSCRIPT_STORE|CONTAINER_STORE)
handle: f -> RELEASE_NO_ESCAPES     (unchanged, and load-bearing)
```

`f` is still never released. The MAP frees the key, and the local's refusal is
what makes the map the sole owner rather than the second one.

## 2. The gates

| Gate | before | after |
|---|---|---|
| `verify-core` | ALL GREEN, 99.40 s | ALL GREEN, 125.53 s |
| `param_escape_test` | 166 assertions, 27 rows | unchanged |
| `block_escape_test` | 112 assertions, 34 rows | unchanged |
| `local_escape_test` | 43 assertions | **69** |
| `release_decision_test` | 197 assertions | **207** |
| `release-decision-teeth` | 9 of 9 | **12 of 12** |
| `escape-teeth` | 13 of 13 | **13 + 11 reasons** |
| `test-golden` / `-o2` / `-reject` | 493 / 493 / 156 | unchanged |
| known-red probes in the tree | 1 | **0** |

`arc-map-key-local-probe` moved into `VERIFY_ALL_DEPS`. All three of its
conditions are guarded, measured by removing each and re-running:

| removed | probe result |
|---|---|
| ownership | FAIL, output differs between release OFF and ON |
| reason set | FAIL, `Invalid read of size 1 at goo_strings_map_case` |
| key-site count | FAIL, output differs between release OFF and ON |

The reason-set failure is the use-after-free this ADR exists to prevent, and
**the output was still correct under it**. Only valgrind saw it.

## 3. Arm coverage — four arms closed by ONE row

```bash
./scripts/escape_arm_coverage.sh --self-test    # 7 of 7, and it was FAILING before
./scripts/escape_arm_coverage.sh                # clean tree only
```

`AST_UNARY_EXPR`, `AST_SELECTOR_EXPR`, `AST_FUNC_LIT` and `AST_STRUCT_LITERAL`
went GAP -> COVERED for `local`. All four by local row 42, and the mechanism is
worth knowing:

An `over` mutation adds UNCLASSIFIED to every slot it touches. A row asserting
`escapes == true` cannot see that — true stays true — which is why the script's
header says this direction needs a PRECISION row. **A row asserting the exact
reason SET sees the extra bit while still being a soundness row.** That header
sentence is now wrong for `local` and should be corrected when someone next
touches it.

## 4. What the ADR got wrong

**It named 21 mark sites. There are 15.** Corrected in the ADR itself.

**Its reason list was short by three.** CONTAINER_STORE, CALLEE_VALUE and
CLOSURE_CAPTURE have no name in it. Both errors have the same cause: the list
was written from the sinks the decision needed rather than from the sites that
exist.

**It does not name the condition that refuses `twoMaps`.** This is the
substantive one. `a[s] = 1; b[s] = 2` marks `s` with SUBSCRIPT_STORE and
nothing else, so "escapes only as a subscript" is TRUE of a local handed to two
maps, and the reason set cannot refuse it. Counting the key sites is the only
thing that does. A reason set says WHERE a value went, never HOW MANY TIMES.

**The route it predicted was not the route taken.** The ADR expected the
condition-6 half to matter. Condition 6 is not involved: `f` reads
RELEASE_NO_ESCAPES, condition 1, and still does. What was needed instead was a
precision fix nobody had scoped — the READ half of `m[k]` was carrying the key's
taint into the store sink. `.handoff.md` recorded tightening that as worth
"~0%", which was TRUE under one boolean and false under a reason set.

## 5. What did NOT change, and stays open

- **The residual 82,205 bytes** are the third class the ADR names and excludes:
  `f` reaches `counts[strings.ToUpper(f)]` on the non-numeric branch, so its own
  buffer is handed to nobody. Ownership that transfers on one branch and not the
  other cannot be expressed by one release site.
- **Condition 6** is untouched. The loop-declared rebound local still refuses,
  and that work is unstarted.
- **The CALLEE_VALUE ceiling** is named but not lifted. Every local with a
  method set is still unreleasable. Local row 42 now makes it measurable, which
  is the first thing a fix would need.
- **`AST_SLICE_INDEX_EXPR`** still unions its low and high bounds into a read's
  result. They are integers, so the same argument applies, but no measurement
  demands it and it was left alone deliberately.
