# Non-String Map Keys — Design

Date: 2026-07-04 · Branch: `feat/non-string-map-keys` (base: main @ 915265d, post-#114)
Status: user-approved design (inline-key scope; unify approach A)

## Goal

`map[K]V` for `K` beyond `string` — integer/uint widths, `bool`, `rune`/`byte`,
and pointers — the last tier-2 stdlib wall. Unblocks `map[int]T`, `map[rune]int`,
`map[*T]V`, the dispatch-table and counting idioms that pervade real Go.

## Scope (user-selected)

- Admitted key types: `string` (existing), all integer/uint widths, `bool`,
  `rune`, `byte`, and pointer types — all INLINE (fit the int64 slot losslessly).
- Deferred (comparable, need boxed-key eq — future cycle): comparable structs
  (field-wise / memcmp), floats (value ==, NaN!=NaN), interfaces (dynamic-type
  eq — ties into RTTI), arrays.
- Permanently rejected (non-comparable in Go): slices, maps, functions.

## Key insight

The map is a LINEAR-SCAN association list (`GooMapEntrySV { key, value, next }`,
`runtime.c:601-695`), NOT a hash table — "correctness over performance". So
non-string keys need only an EQUALITY test, no hash function. And #110 already
represents map VALUES as int64 slots (inline vs boxed); the SAME slot
representation applies to keys. The whole change is "compare key slots by kind,"
not "add a hash table."

## Approach A (unify): one slot-keyed map with a key-kind tag

Considered: (B) a parallel inline-key API alongside the untouched string API —
zero string-path risk but duplicates the entire list logic and doesn't generalize
to struct/float keys; (C) a hash-table rewrite — orthogonal perf work, larger,
YAGNI (linear scan is the established model). A chosen: DRY, one implementation,
the key-kind enum extends cleanly to struct/float later, symmetric with #110.

## 1. Runtime (`src/runtime/runtime.c`, `include/runtime.h`)

- `GooMapSV` gains `int32_t key_kind` (set at creation).
  `GooMapEntrySV.key` changes `const char*` → `int64_t` (a slot, like the value).
- `key_kind` enum (`GOO_MAPKEY_STRING`, `GOO_MAPKEY_INLINE`) in runtime.h.
- The ONLY per-kind difference is equality, factored into one static helper:
  ```c
  static int goo_map_key_eq(int32_t kind, int64_t a, int64_t b) {
      if (kind == GOO_MAPKEY_STRING)
          return strcmp((const char*)(intptr_t)a, (const char*)(intptr_t)b) == 0;
      return a == b;  // INLINE: int/uint/bool/rune/byte/pointer bits
  }
  ```
- API signature changes (names kept; `_sv` now reads "slot key, slot value"):
  - `goo_map_new_sv(void)` → `goo_map_new_sv(int32_t key_kind)`
  - `goo_map_set_sv(m, const char* k, int64_t v)` → `(m, int64_t k, int64_t v)`
  - `goo_map_get_sv(m, const char* k)` → `(m, int64_t k)`
  - `goo_map_get_sv_ok(m, const char* k, ...)` → `(m, int64_t k, ...)`
  - `goo_map_delete_sv(m, const char* k)` → `(m, int64_t k)`
  - `goo_map_iter_next_sv(cursor, const char** key_out, ...)` →
    `(cursor, int64_t* key_out, ...)`
  - `goo_map_len_sv` unchanged (no key param).
- Each of set/get/get_ok/delete replaces its inline `strcmp` with
  `goo_map_key_eq(m->key_kind, e->key, k)`. List walk, value slot, len, iter
  structure all unchanged.
- String-key ownership note (runtime.c:684-690) stays: the map never owns key
  storage; the slot holds the caller's `char*` verbatim.

STRING path is byte-identical behavior (same strcmp on the same char*), just
routed through the kind branch — the existing map goldens are the regression gate.

## 2. Codegen (`src/codegen/composite_codegen.c`, `runtime_integration.c`,
   `statement_codegen.c` range, plus the map-write/read/delete sites)

- **Key→slot** at every op site (set `m[k]=v`, read `m[k]`, comma-ok, `delete`,
  keys emitted into a literal): lower the key expr, convert to i64 slot.
  - INLINE key kinds (int/uint widths, bool, rune, byte, pointer): reuse #110's
    `codegen_map_value_to_slot` — its inline arm (`PtrToInt`/`ZExt`/`SExt`/
    bitcast) is exactly right, and `codegen_map_value_is_inline` returns true for
    all of them.
  - STRING key: do NOT route through `codegen_map_value_to_slot` — that helper
    HEAP-BOXES strings (a string VALUE boxes in #110), which would make each key
    a distinct box and break `strcmp` identity. Instead extract the string's
    `char*` (as the existing string-key codegen already does) and reinterpret to
    i64 directly. Symmetrically, slot→key for a string reinterprets i64 back to
    `char*` (never `codegen_map_slot_to_value`, which would unbox).
  Because non-inline non-string keys are rejected at typecheck (§3), the value
  helper's boxing arm is never reached for a key.
- **Key-kind at creation**: `make(map[K]V)` and map literals emit
  `goo_map_new_sv(kind)` where `kind = codegen_map_key_kind(K)` — a new helper
  returning `GOO_MAPKEY_STRING` for `TYPE_STRING`, `GOO_MAPKEY_INLINE` otherwise.
- **Slot→key on range**: range-over-map (#105) yields the key; reconstruct from
  the slot at K's type (int stays, pointer via `IntToPtr`, string reinterpret to
  `char*`). Thread K's type through the existing range-map lowering.
- Extern decls (`runtime_integration.c:339+`) updated to the new signatures
  (i64 key; `key_kind` arg on new; i64* key_out on iter).
- #110 value-boxing path UNTOUCHED (this is the key axis only).

## 3. Typecheck (`src/types/`)

- LIFT the string-only gate ("map key type must be string, got …") to a
  COMPARABILITY gate: accept `K` in {string, integer/uint widths, bool, rune,
  byte, pointer}. Reject otherwise with a message that distinguishes the two
  reasons accurately:
  - deferred-comparable (`struct`, `float`, `interface`, `array`):
    `"map key type %s is not yet supported in v1 (comparable key types so far:
    string, integers, bool, rune, byte, pointers)"`.
  - non-comparable (`slice`, `map`, `func`):
    `"invalid map key type %s (not comparable)"` (Go-permanent).
- Thread K through the key-consuming sites: `make`/literal key-type resolution,
  `m[k]` index key-type check (index operand must match K — `m["x"]` on
  `map[int]T` rejects), comma-ok, `delete(m, k)`, range key binding type.
- `len`/`cap` guards and #110 value machinery unaffected.

## Testing (all goldens differential vs `go run`)

- `map[int]V`: set/get/overwrite/len/delete/comma-ok-miss (int dispatch table).
- `map[rune]int`, `map[byte]int` (char keys — text processing).
- `map[bool]string` (2-key edge).
- `map[*T]V` pointer keys: two distinct pointers = distinct keys; same pointer
  collides (identity semantics, aliasing-verified vs go run).
- range over `map[int]V` yielding int keys.
- Regression: a `map[string]V` golden stays green (key_kind=STRING path); a
  `map[string]func()int` (composing with #110) stays green.
- Reject probes (Makefile, compile-must-fail + message grep):
  `map[float64]T` → deferred-comparable message; `map[[]int]T` → non-comparable
  message; wrong-typed index (`m["x"]` on `map[int]T`) → key-type mismatch.

Gates: golden grows from the current baseline; `make test` 76/1; `make verify` +
`ccomp-link` green; bison 82/256 UNCHANGED (no grammar — map key types already
parse; typecheck+codegen+runtime only); `grammar-tripwire.sh` no-op sanity.

## Tasks (risk-ascending)

- T1: runtime — key_kind + slot key + `goo_map_key_eq`, signatures updated,
  string path preserved (existing map goldens green).
- T2: codegen — key→slot at all sites, key_kind at creation, range key
  reconstruction, extern decls.
- T3: typecheck — gate lift + comparability check (two-reason diagnostic) +
  key-type threading + reject probes.
- T4: goldens sweep (all new key types) + full sweep + handoff.

SDD economy mode; fresh-context whole-branch review before merge.
