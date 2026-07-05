# Design: interface-typed map keys (vtable-carried equality)

**Date:** 2026-07-05
**Branch:** `feat/interface-map-keys` (base main @ a7008b8)
**Queue item:** the last map-key gap — `map[InterfaceType]V` / `map[any]V`. Builds on struct keys (#129, `goo.structeq.<T>`) and vtable-ptr type identity (#114 assertions).

## Problem

`map[SomeInterface]V` (and `map[any]V`) is rejected by the comparability gate (`type_from_ast` AST_MAP_TYPE). Go allows any comparable interface as a map key; equality is **same dynamic type AND equal dynamic value**, and Go **panics at runtime** when the dynamic type is uncomparable (`hash of unhashable type []int`).

The runtime keys on an 8-byte slot with a per-map comparator (#129). Comparing two interface keys needs the concrete type's value-equality *reachable at runtime from the boxed value* — the dynamic type isn't known statically (`map[any]V` accepts anything). The current vtable (`goo.vtable.<Concrete>.<Iface>`) is just an array of method thunks — no equality function. There is no correct non-invasive version: a raw `data`-word compare is silently wrong for string dynamic values (compares `char*` addresses) and struct dynamic values (compares heap-copy addresses), and those can't be rejected statically.

## Approach

Prepend a per-concrete-type **value-equality function** to each vtable (slot 0; method thunks shift to 1..n). An interface key is heap-copied `{vtable, data}` (pointer in the slot, like a struct key). One generic runtime comparator does **vtable identity → dispatch to `vtable[0]` on the data words**. Uncomparable dynamic types get a panic-stub eq.

**Why data derefs work:** value concretes box `data = goo_alloc(sizeof T); store` — a *pointer to a heap copy* (interface_codegen.c:262-271). So a struct's interface `data` is exactly what `goo.structeq.<T>` already expects (a pointer to a struct copy); scalar/string eq just deref. Pointer concretes box `data = the pointer` (aliased), so their eq is pointer identity on the data words.

**Chosen over** a 3-word interface repr `{typeinfo, vtable, data}` (more invasive — touches every box/dispatch/assertion) and a runtime vtable→eq side-table (can't be populated from a raw vtable pointer). The vtable-slot-0 approach is the smallest correct change: the vtable is indexed for dispatch at exactly one site.

## Components

### 1. Per-concrete-type eq function — `codegen_get_or_emit_type_eq(codegen, checker, concrete)`
New codegen entry (declare in `include/codegen.h`), returns a cached `i32 (i64 a, i64 b)` LLVM function for a concrete type, keyed by `Type*` identity:
- **pointer type `*T`**: `return a == b;` (data words are the pointers → identity).
- **integer/bool/char**: deref both `i64` args as `T*`, `icmp eq` the loaded values.
- **float32/float64**: deref, `fcmp oeq`.
- **string**: deref both to the `{i8*,i64}` string value, `strcmp` field 0.
- **struct**: reuse `codegen_get_or_emit_struct_key_eq(concrete)` directly (its `i64,i64`→ptr-to-copy signature already matches).
- **slice / map / func (uncomparable)**: a panic-stub `i32 goo.uncmpeq(i64,i64)` that calls `goo_panic("comparing uncomparable type")` — Go-faithful. Emitted once, shared.

### 2. Vtable emission (`codegen_interface_vtable`, interface_codegen.c:182)
Emit the vtable global as `n+1` ptr slots: slot 0 = `codegen_get_or_emit_type_eq(concrete)` (bitcast to ptr), slots 1..n = the method thunks (unchanged order). The eq fn is per concrete, so `goo.vtable.Foo.Stringer[0]` and `goo.vtable.Foo.any[0]` reference the same fn.

### 3. Method-dispatch shift (`codegen_interface_dispatch`, interface_codegen.c:~308)
The one dispatch GEP `gep_idx = idx` becomes `gep_idx = idx + 1` (methods now live at slot idx+1; slot 0 is eq). This is the only ABI-visible change; the #114 interface/assertion golden set is the regression net.

### 4. Runtime comparator + kind (`runtime.h`/`runtime.c`)
New `GOO_MAPKEY_IFACE = 3`. A generic `int goo_iface_key_eq(int64_t a, int64_t b)` (in runtime.c, or codegen-emitted): treat `a`,`b` as pointers to the boxed `{void* vtable; void* data}` interface value; load both; if `vt_a == NULL && vt_b == NULL` → 1 (both nil); if `vt_a != vt_b` → 0 (different dynamic type, incl. one-nil); else `((GooKeyEqFn)((void**)vt_a)[0])(data_a_as_i64, data_b_as_i64)`. Interface maps set `key_kind = GOO_MAPKEY_IFACE` and `key_eq = goo_iface_key_eq` (via the #129 `goo_map_new_sv(kind, key_eq)` path). `goo_map_key_eq` (runtime.c) dispatches `IFACE → m->key_eq(a, b)` (same as STRUCT — both use the fn-ptr).

### 5. Key packing (`codegen.c`)
- `codegen_map_key_kind`: return `GOO_MAPKEY_IFACE` for a `TYPE_INTERFACE` key.
- `codegen_map_key_to_slot`: `TYPE_INTERFACE` arm — `goo_alloc(sizeof {ptr,ptr})`, store the loaded interface value, `PtrToInt` the pointer → slot (mirror the struct arm).
- `codegen_map_slot_to_key`: `TYPE_INTERFACE` arm — `IntToPtr` + load the `{vtable,data}` value.
- Map creation (make + literal): pass `goo_iface_key_eq` (get-or-declared extern) as the `goo_map_new_sv` comparator for interface keys.

### 6. Comparability gate (`type_from_ast` AST_MAP_TYPE)
Admit `TYPE_INTERFACE` keys unconditionally (statically comparable). The dynamic uncomparable case is handled by the panic-stub eq at runtime, NOT a compile error.

## Data flow

`m[k] = v` (k an interface) → codegen heap-copies `{vt,data}`, `PtrToInt` → slot → `goo_map_set_sv` → runtime scan calls `goo_map_key_eq(m, e->key, slot)` → `goo_iface_key_eq` → vtable identity → `vt[0]` eq on the two `data` words → for a struct dynamic value, that's `goo.structeq.<T>` comparing fields.

## Error handling

- Uncomparable dynamic key (`var k any = []int{...}; m[k] = v`): the panic-stub eq fires on the first comparison → `goo_panic` abort (non-zero exit, "comparing uncomparable type"). Matches Go's panic (timing: Go panics on insert-hash, we on first compare — a nonempty-map insert or any lookup triggers it; documented).
- nil interface key: both-nil → equal (one entry); nil vs non-nil → unequal (vtables differ).
- The eq fn cannot otherwise fail.

## Testing

go-run-verified goldens (multi-line struct/interface defs — single-line bodies hit the ASI quirk):
- `map[any]int` with **int**, **string** (distinct-address equal-content → same entry), **struct-value**, and **pointer** dynamic keys: same-type match, cross-type miss (`any(1)` != `any("1")` != `any(int64(1))`), overwrite.
- `map[Stringer]int` (a method interface) with two concrete implementers → distinct entries; same concrete equal-value → same entry.
- **nil interface key** round-trips; nil != a non-nil key.
- range over `map[any]int` yielding interface keys.

Reject/panic probes (Makefile, wired into `verify:`):
- `iface-map-key-uncomparable-probe`: `[]int` (or a func) into a `map[any]int` used as a key **runtime-panics** with "comparing uncomparable type" (compile-then-run, assert non-zero exit + message).

Regression net (the vtable-shift is the risk): the full existing golden suite (interface method dispatch, embedding, type assertions/switches from #113/#114) must stay green — a wrong shift breaks method calls loudly. `make verify` ALL GREEN, `ccomp-link` PASS, bison **82/256 unchanged** (no grammar change).

## Out of scope

- **Hashing** — the map stays a linear-scan association list; interface keys inherit O(n), like every key kind.
- **Non-comparable-at-compile interface keys** — n/a: interface *types* are always comparable; only dynamic values can be uncomparable (→ runtime panic).
- The `type_to_string` interface `?` rendering (interfaces show as `?` in diagnostics) — a trivial adjacent polish; fold in only if it improves an error message this feature adds, otherwise leave.
