# Spike: what is actually left in the daemon, and is the `[]T` element scan the right next target?

Date: 2026-07-29. Compiler: `main` @ `4ea6f39` (after PRs #258–#268).

`.handoff.md` ranked a `[]T` element scan as the next item that would move the
benchmark, at "8.6% of the daemon", and called it "bounded on its own". This
spike measures what is really left and answers both claims.

**Result: the element scan is NOT the largest remaining item, and it is not the
best next target. The map is roughly twice as large and its hardest problem —
ownership — is already solved and already implemented.**

## Method

```sh
./bin/goo -o build/daemon_on bench/daemon/daemon.goo        # release ON
valgrind --leak-check=full --show-leak-kinds=definite --num-callers=8 \
    ./build/daemon_on 2000
```

Every leak record was attributed to the first non-allocator frame in its stack.
Two instrument corrections were needed before the numbers meant anything:

1. Grouping on the innermost frame attributed 100.0% to `goo_alloc`, which is
   the allocator in every stack. A share of 100% from one site is a tell that
   the grouping key is wrong, not a finding.
2. The first parse summed 967,982 bytes against valgrind's reported 1,207,982
   definitely lost, and ignored 1,002,000 indirectly lost entirely. The gap was
   valgrind's `(X direct, Y indirect)` record phrasing, which is exactly the
   container-holding-elements case this spike is about. **The two records that
   phrasing hid are the two largest.**

Totals reconcile exactly: 2,209,982 bytes lost per 2,000 requests.

## What is left, after everything shipped in #258–#268

| # | Origin | Bytes / 2,000 req | Share | Reclaimable by the element scan? |
|---|---|---|---|---|
| 20 | `goo_map_new_sv` — the `counts` map | 902,000 (80,000 direct + 822,000 indirect) | **40.8%** | NO |
| 18 | `goo_error_from_string` — `strconv.Atoi` failures | 340,000 (160,000 + 180,000) | **15.4%** | NO |
| 17 | `goo_strings_trim_space` | 262,000 | 11.9% | PARTLY |
| 16 | `goo_string_new_with_length` — `strings.Split` elements | 235,982 | 10.7% | YES |
| 4–13 | `concat`, `int_to_string`, `join`, more `trim_space` | 470,000 | 21.3% | NO — these build the RETURN value |

`fields` and `parts` already release their BUFFERS today (`field=0`); what leaks
is the element strings inside them.

**Element-scan ceiling: about 20%** — Split's 7 elements per request (12.4%)
plus the appended `TrimSpace` results (~8%, approximate: `trim_space` has two
call sites in `handle` and only the appended one is an element, and this spike
did not isolate them).

## Why the map is the better target

The map is 40.8%, and 822,000 of its 902,000 bytes are INDIRECT — the
`GooMapEntrySV` nodes hanging off it. `.handoff.md` records the map as
"unreachable by any codegen-side release", which is true but incomplete. It is
reachable by a RUNTIME destructor, and nobody has costed one.

**The ownership rule already exists and is already implemented.**
`goo_map_delete_sv` (`src/runtime/runtime.c:1023`) documents it:

> Key ownership: `goo_map_set_sv` above stores the caller's pointer verbatim
> (`e->key = k;`) rather than duplicating it — the map never owns key storage.
> So this frees only the entry node itself; freeing `dead->key` would free
> memory the map does not own (e.g. a string literal's constant data).

So a `goo_map_free_sv` walks the entry list and frees each node, and must NOT
free keys. That is the same rule `goo_map_delete_sv` already applies per entry.
The hard part of this work is done.

Contrast the element scan, which needs a NEW ownership rule for slice elements
and has no precedent to copy. The ledger already records that `append`'s
elements genuinely escape, correctly — so "release the elements when the slice
is released" is not obviously sound and needs its own analysis.

## The structural finding

`goo_release` ends in `goo_free(ptr)` — **exactly one block**. The ARC header has
no type tag; its fourth field is `uint64_t reserved`, documented as
"pads to GOO_OBJ_HEADER_SIZE; future flags/type tag".

So NOTHING in the release path can walk what an object contains. Both the
element scan and the map destructor need that capability, by different routes:

- the element scan can avoid it — codegen knows the static element type at the
  release site and can emit an inline loop
- the map cannot — its bucket layout is runtime-owned, so it needs a runtime
  `goo_map_free_sv` called from the release site

The map route is the smaller change of the two, because the runtime already
knows its own layout.

## The blocker on the map, and it is narrow

The daemon's `counts` is NOT approved today. Measured by narrowing:

| Shape | Verdict |
|---|---|
| `counts["a"] = n` | RELEASE |
| `counts[k] = n`, k a param | RELEASE |
| map written in a loop | RELEASE |
| with an import present | RELEASE |
| in a function returning a string | RELEASE |
| **`counts[f] = counts[f] + 1`** (compound update, loop) | **REFUSED** |

The compound map update is the single construct that refuses it. `.handoff.md`
already carries this as a deliberate over-conservatism — "A map READ marks its
key escaping ... only the base needs marking" — with the recorded justification
that tightening it "reclaims ~0%".

**That justification is now out of date.** It was measured when nothing could
free a map at all. With a destructor, the same construct gates 40.8%.

## Recommendation

Do the map, not the element scan. Three pieces, in this order, each measurable:

1. `goo_map_free_sv` in the runtime: walk the entry list, free each node, never
   free a key. Mirrors `goo_map_delete_sv`, which is the existing precedent.
2. Call it from the release site for a `TYPE_MAP` local. Codegen already knows
   the local is a map — `codegen_arc_note_local` records `field = -1` for one
   today, because a map's LLVM form is an opaque pointer.
3. Tighten the map read/write base marking so `counts[f] = counts[f] + 1` stops
   refusing the base. This is the narrow blocker above, and it is the only piece
   that touches shared escape-analysis arms.

Expected: up to 822,000 of 2,209,982 per 2,000 requests — about 37% of what
remains — without step 3 reaching the keys, which the map does not own.

Do the element scan afterwards, if at all. It is worth about half as much, and
its ownership rule is the one nobody has written yet.

## What this spike did not settle

- The exact split of `trim_space` between the loop-local `f` and the appended
  element. Both call sites are in `handle`; only the second is element-reachable.
- Whether `goo_error_from_string` at 15.4% is reclaimable. Those errors are
  loop-scoped locals, so condition 4 refuses them, and that is the ADR 0004 kill
  rule / CFG problem rather than a container problem. It is the third-largest
  item and nobody has costed it either.
