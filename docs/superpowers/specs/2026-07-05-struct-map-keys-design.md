# Design: struct-typed map keys (synthesized per-field comparator)

**Date:** 2026-07-05
**Branch:** `feat/struct-map-keys` (base main @ d0e1ffc)
**Queue item:** the deferred "interface/struct-typed map keys" — this cycle covers **struct keys only** (interface keys are a separate follow-up building on this).

## Problem

`map[StructType]V` is rejected: `type_from_ast`'s `AST_MAP_TYPE` comparability gate (`src/types/type_checker.c:2753-2761`) admits only string / integer / bool / rune / byte / pointer keys — a struct key errors "map key type … is not yet supported in v1". Go allows any *comparable* struct as a map key. The runtime (`GooMapSV`) keys on an 8-byte `int64` slot with a `key_kind` tag: `GOO_MAPKEY_STRING` (compared by `strcmp` on the stored `char*`) or `GOO_MAPKEY_INLINE` (raw `==`). A struct doesn't fit 8 bytes and needs value-equality, so neither kind works.

Critically (per the #115 finding), struct keys must NOT be compared via struct-blind `type_equals` or naive `memcmp`: `memcmp` mishandles string fields (compares `char*` addresses, not content), padding bytes (uninitialized differs), floats (NaN/±0), and nested structs. Correct equality requires per-declared-field comparison.

## Approach

Extend the string-key pattern. A string key lives as a `char*` in the `int64` slot, compared by `strcmp`. A struct key becomes a **heap copy**, its pointer in the slot, compared by a **synthesized per-field comparator function** — the struct analogue of `strcmp`. The comparator compares *declared fields* only, so padding is never touched, strings compare by content, floats get IEEE `==`, and nested structs recurse into their own comparator.

**Chosen over** (a) `memcmp` of a zeroed copy — wrong for string/float/nested fields, only handles integer/bool/pointer structs; rejected as leaving a real gap. (b) a type-descriptor walked by a generic C runtime function — viable, but this codebase already emits per-type helper functions (thunks, boxing), so a synthesized comparator function is more idiomatic and composes naturally for nesting (a struct field's comparison is a call to that type's comparator).

## Components (5 units)

### 1. Comparability gate (typecheck)
`type_from_ast` `AST_MAP_TYPE` (`type_checker.c:2753`): admit a `TYPE_STRUCT` key iff **recursively comparable** — every field is a scalar (integer/bool/char/pointer), float, string, or a nested comparable struct. A new helper `type_is_comparable_key(Type*)` performs the recursive check. Reject (clean error) a struct with any slice/map/func field (recursively) → "invalid map key type: struct field '<f>' of type <T> is not comparable". Reject a struct with an **array** field → "…not yet supported in v1" (Go-legal; deferred to bound the comparator scope). The existing string/int/bool/char/pointer admission is unchanged.

### 2. Runtime (`include/runtime.h`, `src/runtime/runtime.c`)
- New `GOO_MAPKEY_STRUCT = 2` alongside `GOO_MAPKEY_STRING = 0` / `GOO_MAPKEY_INLINE = 1`.
- `GooMapSV` gains `int (*key_eq)(int64_t a, int64_t b)` — the per-map struct comparator, NULL for non-struct maps.
- `goo_map_new_sv(int32_t key_kind, void* key_eq)` — signature extended (was `(int32_t)`); `key_eq` is NULL for string/inline maps. **Runtime ABI change** → touches every `goo_map_new_sv` call site (make + map literal) and the codegen extern declaration. (The incremental build from #123 handles the `runtime.h` change; no `make clean` needed.)
- `goo_map_key_eq(kind, a, b)`: add `if (kind == GOO_MAPKEY_STRUCT) return m_key_eq ? m_key_eq(a, b) : (a == b);` — but `goo_map_key_eq` is a static file-scope helper taking `(kind, a, b)`; it must also receive the map's `key_eq` fn-ptr. Thread it through: change `goo_map_key_eq` to `goo_map_key_eq(GooMapSV* m, int64_t a, int64_t b)` (reads `m->key_kind` and `m->key_eq`), updating its ~4 call sites inside `runtime.c` (set/get/get_ok/delete). `a`/`b` for STRUCT are the two stored struct-copy pointers (cast from `int64`).

### 3. Comparator synthesis (codegen)
Per struct key type, emit an LLVM function `goo.structeq.<id>(i64 a, i64 b) -> i32` (cached — emit once per distinct struct type). **Cache key = struct type identity, not name:** named and anonymous struct types must both work, so the cache is keyed by a structural signature (field kinds + names, recursively) or the resolved `Type*`; `<id>` is a generated unique suffix (a counter or the mangled signature), never assumed to be a source-level name. Body: cast `a`,`b` to the struct pointer type; for each field, load both and compare —
- integer/bool/char/pointer → `icmp eq`;
- float32/float64 → `fcmp oeq` (Go `==`; NaN≠NaN falls out, so a NaN-float-field key is never retrievable, matching Go);
- string → `strcmp(af, bf) == 0` on the two `char*` fields;
- nested struct → call *that* type's `goo.structeq.<T>` on the field addresses (recurse; emit the nested comparator first).
All fields must match → return 1, else 0 (short-circuit: branch to a `return 0` block on the first mismatch). A new codegen entry point `codegen_get_or_emit_struct_key_eq(codegen, checker, struct_type)` returns the function value (emitting + caching on first request).

### 4. Key packing (codegen)
- `codegen_map_key_to_slot` (`codegen.c`): for a `TYPE_STRUCT` key, `goo_alloc(sizeof struct)`, store the struct value into it, `PtrToInt` the pointer → `int64` slot (mirrors the string arm, which stores the `char*`). The key must be a value (load if lvalue) before the copy.
- `codegen_map_slot_to_key` (`codegen.c`, range unpack): for `TYPE_STRUCT`, `IntToPtr` the slot → struct pointer, `load` the struct value (mirrors string reconstruction).
- `codegen_map_key_kind` (`codegen.c:610`): return `GOO_MAPKEY_STRUCT` for a struct key.

### 5. Map creation (codegen)
At `make(map[Struct]V)` (`call_codegen.c:510`) and map-literal creation: pass the synthesized comparator's address as the new `goo_map_new_sv` second argument (via `codegen_get_or_emit_struct_key_eq`); pass NULL for non-struct keys. Update the `goo_map_new_sv` extern declaration in `runtime_integration.c`.

## Data flow

`m[Point{X:1,Y:2}] = 5` → codegen copies the key struct to heap, `PtrToInt` → slot → `goo_map_set_sv(m, slot, 5)` → runtime linear scan calls `goo_map_key_eq(m, e->key, slot)` → for STRUCT kind, `m->key_eq(e->key, slot)` → the synthesized `goo.structeq.Point` casts both to `Point*` and compares X, Y. Two distinct heap copies with equal fields compare **equal** (the correctness property — like content-equal strings hitting the same entry).

## Error handling

- Non-comparable struct key (slice/map/func field, recursively) → typecheck error, compile fail, no codegen.
- Array-field struct key → "not yet supported in v1" typecheck error (Go-legal, deferred).
- The comparator itself cannot fail at runtime; a NULL `key_eq` for a STRUCT map (should never happen) falls back to raw `==` (pointer identity) — defensive, not relied upon.

## Testing

go-run-verified goldens (multi-line struct definitions — single-line struct bodies hit an unrelated ASI quirk, goo-grammar workaround #4):
- `map[Point]int` (`Point{X,Y int}`): insert, overwrite same key, comma-ok present/absent, delete, len, range summing keys' fields.
- **`map[Name]int` where `Name struct{ First, Last string }`** — the string-field correctness case: build two keys with distinct-address but equal-content string fields (e.g. one from a literal, one from concatenation) and confirm they hit the SAME entry.
- Nested key: `Outer{ P Point; Tag string }`.
- Float field: `struct{ X float64 }` key round-trips (and a NaN-field key is never found, matching Go — documented, verified vs `go run`).
- Range over a `map[Point]int` yielding struct keys.

Reject-probes (compile-must-fail + message grep, wired into `verify:`):
- `struct-map-key-reject-probe`: a struct key with a **slice** field ("not comparable"); a struct key with an **array** field ("not yet supported in v1").

Gate baselines: golden count grows by the new goldens; `make test` 76/1; **bison 82/256 unchanged** (no grammar change — struct keys already parse; only typecheck/codegen/runtime change); `make verify` ALL GREEN; `ccomp-link` PASS.

## Out of scope

- **Interface-typed map keys** — separate cycle (needs dynamic-type identity + boxed-value comparison on top of this).
- **Array-in-struct-key fields** and **top-level array keys** (`map[[3]int]V`) — Go-legal; deferred (the comparator would need an element loop). Rejected cleanly.
- **Float top-level keys** (`map[float64]V`) — a trivial adjacent win (fits the inline slot), but out of this cycle's struct-key focus; left rejected as today.
- Hashing — the map remains a linear-scan association list (unchanged); struct keys inherit its O(n) lookup, same as every other key kind today.
