# Func-Typed (and Generic) Map Values — Design

**Date:** 2026-07-04
**Status:** Approved (user-validated: scope, lifetime, approach, both design sections)
**Prior art:** `2026-07-03-closures-design.md` (funcval fat-pointer ABI),
interface boxing (`codegen_interface_box`, the heap-box template),
`chan_padded_probe` (ABI-size-not-field-sum lesson).

## Goal

Maps with arbitrary value types — headline: closure dispatch tables
(`map[string]func(int) int`), and with the same mechanism `map[string]string`,
struct/float/slice/interface values, and nested maps. Today the typecheck gate
(`type_checker.c:2465`) rejects everything but integer/bool/char/pointer values
because the runtime map slot is a hard `int64_t`.

```go
ops := map[string]func(int) int{
    "double": func(x int) int { return x * 2 },
    "next":   func(x int) int { return x + 1 },
}
fmt.Println(ops["double"](21))   // 42
ops["missing"](1)                 // panic: call of nil function (Go parity)
```

## Decisions (user-approved)

1. **Scope: generic, by size.** Any value type is admitted. Inline types keep
   today's i64-slot casting; everything else heap-boxes. No type is rejected at
   the map-value gate anymore. Keys stay **string-only** (unchanged, separate
   recorded gap).
2. **Box lifetime: leak on overwrite/delete**, consistent with closure envs and
   interface boxes (no GC yet). Recorded as GC-era work; `goo_free` exists when
   that day comes.
3. **Approach: slot boxing in codegen** (Approach A below). Zero runtime ABI
   changes.

## Approach record (alternatives considered)

- **A — slot boxing in codegen: CHOSEN.** Keep the `int64_t` slot and all seven
  `goo_map_*_sv` signatures. Widen the two slot-coercion helpers with one
  symmetric boxing arm. Localized, generalizes to every type, reuses two
  shipped patterns (interface boxing, slot casting).
- **B — widen the slot to 16 bytes:** touches every `_sv` signature and all
  operation sites, yet still can't hold arbitrary structs — the generic scope
  would need boxing anyway. Worst of both; rejected.
- **C — element-size-generic map rewrite (slice/chan style):** the right
  long-term runtime shape, but the linked-list map is due for a hash-table
  replacement post-v1 and this rewrite would be discarded with it. Rejected
  for blast radius against a doomed data structure.

## Classification (one predicate, one place)

New codegen predicate `map_value_is_inline(Type* v)`:

- **Inline** — integer family (`TYPE_INT*`/`TYPE_UINT*`), `TYPE_BOOL`,
  `TYPE_CHAR`, `TYPE_POINTER`, and (new, free) `TYPE_MAP` + `TYPE_CHAN` whose
  LLVM reps are opaque pointers — one `PtrToInt`/`IntToPtr` arm, enabling
  nested `map[string]map[string]int` with no boxing.
- **Boxed** — everything else: `TYPE_FUNCTION` (16-byte `{fn, env}` pair,
  env-FIRST contract untouched — the pair is stored/loaded whole),
  `TYPE_STRING` (`{ptr, i64}`), floats, structs, slices, arrays, interfaces,
  nullables, error unions.

Floats could inline via bitcast; boxing them keeps the rule binary. Inline
floats are a recorded optimization, not scope.

## Coercion helpers (the core change)

`src/codegen/codegen.c:523–546`, both helpers gain one symmetric arm:

- `codegen_map_value_to_slot(V, val)` — boxed V:
  `box = goo_alloc(LLVMSizeOf(V_llvm)); store val, box; slot = PtrToInt(box)`.
  `LLVMSizeOf` (ABI size) is mandatory — padding-correct per the chan-padded
  lesson; never sum Goo field sizes.
- `codegen_map_slot_to_value(V, slot)` — boxed V, **guarded**:
  `slot == 0 ? zeroinitializer(V_llvm) : load(IntToPtr(slot))`.
  The guard is load-bearing: `goo_map_get_sv` returns 0 on a missing key; an
  unguarded load is a null-deref segfault.

## Zero-value semantics (Go parity)

Missing key (plain read) and comma-ok miss (`found == 0`) both produce V's
`zeroinitializer`:
- funcval → `{null, null}`; calling it hits the existing nil-func panic
  (`call of nil function`) — Go's behavior for a missing dispatch entry.
- string → `{null, 0}`; must print as `""` — same representation as an
  uninitialized `var s string` (probe-verified during implementation; if the
  print path can't take a null `char*`, fix at the print site, not by giving
  maps a special empty string).
- struct/slice/etc. → all-zero value copy.

## Operation sites

All value-carrying sites already funnel through the two helpers (or gain the
call): write `expression_codegen.c:921–957`; literal `expression_codegen.c:88–128`
(+ value-type check `expression_checker.c:283–322`); read fast path
`composite_codegen.c:81–103`; comma-ok `function_codegen.c:1580–1629`; range
value slot `statement_codegen.c:767`. `make`/`len`/`delete` don't touch values.
Each site's change: pass V's `Type*` through; the helpers do the rest.
Existing literal/assignment coercion (`codegen_coerce_to_type`, PR #99 helper)
runs BEFORE slot conversion so untyped constants adapt to V first.

Typecheck gate `type_checker.c:2451–2473`: drop the value-type rejection
entirely (key stays string-only with today's message).

## Go-semantics guards (map values are not addressable)

Boxing must not accidentally make map values addressable:
- `&m[k]` → typecheck reject.
- Partial updates through a map index — `m[k].F = v`, `m[k][i] = v` — →
  typecheck reject (Go rejects; without the guard the lvalue path would error
  uglily or silently mutate a box nobody else reads).
- Whole-value forms stay legal per Go: `m[k] = v`; `m[k]++` / `m[k] += 1` for
  inline types **if they already work today** (probe-verify; not newly built).
- `m[k].F` as an RVALUE read is legal — existing rvalue-selector path.
- Copy semantics: reads load the value out of the box; mutating the source
  after insert must not affect the map's copy (golden-pinned).

## Pre-existing correctness check (folded in)

The runtime's `goo_map_set_sv` may head-insert without checking for an existing
key. Probe FIRST: `m["a"]=1; m["a"]=2; len(m)` — if it yields 2, fix the
runtime to update-in-place (overwrite correctness matters once dispatch tables
update entries). Only fix if the probe proves it broken.

## Testing

- **Goldens** (go-run differential): dispatch table of closures — build,
  call, update an entry, call again; missing-entry call → nil-func panic
  (abort-probe pattern, matching Go's panic-on-nil-call); `map[string]string`
  incl. missing → `""`; struct values with copy-semantics proof;
  `map[string]float64`; nested `map[string]map[string]int`; range + comma-ok
  over boxed values (single-entry or order-safe per the documented
  deterministic reverse-insertion iteration order).
- **Reject probes:** `&m[k]`; `m[k].F = v`.
- **Guards:** parser untouched → bison stays exactly 81 S/R + 256 R/R with no
  new productions; golden baseline 207 + new probes; full `make verify` +
  `make test`; `ccomp-link` PASS.

## Out of scope (recorded)

- Non-string map keys (separate gap, unchanged message).
- Freeing boxes / GC (decision 2); inline-float optimization; hash-table map
  runtime (post-v1).
- Map-value addressability extensions beyond Go (boxes would permit them;
  deliberately not exposed).
