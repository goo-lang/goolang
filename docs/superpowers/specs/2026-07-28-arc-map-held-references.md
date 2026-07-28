# Map-held references under ARC — tag the map, and ship the retain before the release

**Date:** 2026-07-28
**Status:** recommendation. No live defect: the arena side is sound and now gated.
**Follows:** `2026-07-28-arc-arena-coexistence.md` (the parameter boundary) and
`2026-07-28-arc-return-convention.md` (the return boundary). This is the
largest hole those two left open.

## The problem

`codegen_map_value_to_slot` packs a pointer into the map's `int64_t` slot with
`LLVMBuildPtrToInt` (`codegen.c:726`, and `:738` for a boxed value). The runtime
map stores `int64_t key` and `int64_t value` (`runtime.c:782`) and knows nothing
about pointers.

**A map slot is the one place in Goo where a pointer stops being a pointer.**
Under ARC nothing retains on insert, and nothing releases on overwrite, delete,
or teardown.

The dangerous direction is not the leak. It is this: the last stack reference
dies, the count reaches zero, the object is freed, and the map still holds its
address. The next map read is a use-after-free.

## What is NOT broken — measured, not assumed

Storing an arena-allocated pointer into a longer-lived map is safe today.
`assign_to_lvalue` treats every non-identifier lvalue as an unconditional sink
(`block_escape.c:599`, `:620`), so `m[k] = p` marks the site and it goes to the
heap.

Measured on `examples/arena_map_store_probe.goo`: the site emits `goo_alloc`,
the program prints `7`, valgrind is clean. That rule was a comment and is now a
gate, in `arena-free-probe` and `arena-valgrind-probe` (16/16 each). It is worth
gating precisely because the `PtrToInt` above means no later pass can re-derive
the fact.

Also already recorded, at `codegen.c:731`: a boxed map value "leaks on
overwrite/delete by decision (no GC yet)". So the leak is a known position, not
a discovery.

## The options

**A. Give the map a value-kind tag, and let the runtime retain and release.**

`GooMapSV` already carries `key_kind` and a `key_eq` function pointer
(`runtime.c:788`), so a map already carries type metadata and the precedent
exists. Add `value_kind`, set at `goo_map_new_sv` from the static value type.
`goo_map_set_sv` then retains the new slot and releases the old one,
`goo_map_delete_sv` releases, and a teardown walks the chain and releases every
slot.

- Correct at every site with no duplicated codegen. Compound assign is covered
  for free, because `m[k] += 1` routes get-then-set through the same function
  (`runtime.c:797`).
- The metadata rides the map, so a map reached through an interface, stored in a
  struct, or passed to another function still behaves.
- Costs: retain and release become runtime symbols called from the map, and the
  map gains one field.

**B. Emit retain and release at the codegen call sites.**

- No runtime change, and the static value type is known at each write.
- An overwrite needs a read-modify-write at EVERY write site: read the old slot,
  retain the new, release the old. That has to be repeated across the direct
  write, the compound assign, and the map-literal paths.
- **It cannot free a map at all.** Codegen cannot enumerate the live keys, so
  teardown is unreachable from the call sites. This is disqualifying on its own.

**C. Box every map value and put a header on the box.**

Uniform, and it makes the slot self-describing. It also costs a heap box for
every integer value, which is exactly what the inline fast path
(`codegen_map_value_is_inline`) exists to avoid. Rejected on cost.

**D. Retain on insert and never release.**

A leak, never a dangle. Honest, sound in the safety sense, and it matches the
position already documented at `codegen.c:731`.

## Recommendation: A, shipped in D's order

Adopt **A**, and land it in two steps.

1. **Retain on insert, and do not release.** This is D, and it is the safety
   half. A map keeps its values alive.
2. **Add the release** in `set` (overwrite), `delete`, and teardown.

The order is the point. A missing retain is a use-after-free; a missing release
is a leak. Ship the direction whose failure mode is a wrong number in a memory
graph before the one whose failure mode is silent corruption. The same asymmetry
already governs the escape analysis, where `true` only costs performance.

**Why A rather than B: teardown.** Codegen cannot enumerate a map's live keys,
so B can never release a whole map. A design that cannot free a map cannot fix
the daemon shape, and the daemon shape is the entire reason ADR 0002 exists.

**One hazard to write into the implementation.** Retain the NEW value before
releasing the OLD one. The reverse order makes `m[k] = m[k]` free the object it
is about to store. This is the classic Objective-C setter bug, and step 2 is
exactly where it appears.

## Scope note — maps are not the only container

The same argument applies wherever a container the runtime manages holds a
pointer. A slice of pointers is the next case: `goo_slice_append` grows raw
backing memory and copies bytes, so an appended pointer is retained by nobody.
Slices differ from maps in that codegen still emits a typed store, so the
pointer stays a pointer in the IR — but the runtime helper that copies and grows
does not know it. That case needs its own pass and is NOT decided here.

## What this does NOT decide

- Atomic against per-goroutine counts. Open from ADR 0002.
- Cycles. A map holding a pointer to a struct that holds the map is a cycle, and
  ARC leaks it by design.
- Map KEYS. `goo_map_set_sv` stores the caller's pointer verbatim and
  deliberately never frees a key (`runtime.c:877`). A string or struct key is
  therefore also an untracked reference, with the same shape as the value
  problem and a different ownership comment already attached to it.
