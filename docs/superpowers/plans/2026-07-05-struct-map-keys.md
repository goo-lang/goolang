# Struct-Typed Map Keys Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Support `map[StructType]V` for comparable structs — heap-copy the struct key (pointer in the int64 slot, like strings), compared by a synthesized per-field comparator function.

**Architecture:** Extend the existing `GooMapSV` string-key pattern. Add a `GOO_MAPKEY_STRUCT` kind and a per-map `key_eq` comparator function-pointer; codegen synthesizes (and caches) a field-by-field equality function per struct key type and passes its address to `goo_map_new_sv`. Spec: `docs/superpowers/specs/2026-07-05-struct-map-keys-design.md`.

**Tech Stack:** C23, LLVM-C API.

## Global Constraints

- Branch: `feat/struct-map-keys` (exists, base main @ d0e1ffc). Commit `--no-gpg-sign`; pre-commit runs `make test`.
- **NO grammar change** — struct keys already parse. `./scripts/grammar-tripwire.sh` must stay PASS **82 S/R + 256 R/R** (a no-op sanity; any delta means something unintended was touched).
- **`include/runtime.h` is edited (Task 1)** — the build is now incremental/header-aware (PR #123), so plain `make lexer` recompiles the dependents; no `make clean` needed. Real exit codes only (never a pipeline `$?`).
- Struct-key comparison compares **declared fields only** — never `memcmp`/`type_equals` (per the #115 finding: memcmp mishandles string fields, padding, floats, nesting).
- v1 struct-key field kinds: integer/bool/char/pointer (`icmp`), float32/float64 (`fcmp oeq`), string (`strcmp`), nested comparable struct (recurse). Array fields and slice/map/func fields are REJECTED at typecheck (array = "not yet supported in v1"; slice/map/func = "not comparable").
- Baselines: golden **248/0** (`bash scripts/run_golden.sh`), `make test` **76/1**, bison **82/256**, `make verify` ALL GREEN, `make ccomp-link` PASS (needs `eval "$(opam env --switch=default)"`). Every accept-golden's expected output produced by `go run` on an equivalent `.go`. Struct definitions in test programs MUST be multi-line (single-line struct bodies hit an unrelated ASI quirk — goo-grammar workaround #4).
- `goo_alloc(size_t)` is the runtime allocator (`include/runtime.h:45`).

---

### Task 1: Runtime — `GOO_MAPKEY_STRUCT` kind + comparator fn-pointer

**Files:**
- Modify: `include/runtime.h` (key-kind enum ~144, `GooMapSV` struct ~147, `goo_map_new_sv` decl ~151)
- Modify: `src/runtime/runtime.c` (`goo_map_key_eq` ~599, `goo_map_new_sv` ~616, the 4 `goo_map_key_eq` call sites at ~626/641/653/709)
- Modify: `src/codegen/runtime_integration.c` (~339 — the `goo_map_new_sv` extern declaration)
- Modify: every codegen `goo_map_new_sv` CALL site to pass a second arg (this task passes NULL everywhere — struct keys aren't wired until Task 2)

**Interfaces:**
- Consumes: `goo_alloc(size_t)`.
- Produces: `typedef int (*GooKeyEqFn)(int64_t, int64_t);`; `GOO_MAPKEY_STRUCT = 2`; `GooMapSV.key_eq`; `goo_map_new_sv(int32_t key_kind, GooKeyEqFn key_eq)`; runtime dispatch of struct keys through `key_eq`.

- [ ] **Step 1: Extend the runtime header**

In `include/runtime.h`, change the key-kind enum (was `enum { GOO_MAPKEY_STRING = 0, GOO_MAPKEY_INLINE = 1 };`) to add STRUCT, add the fn-ptr typedef, and the struct field + new `goo_map_new_sv` signature:

```c
typedef int (*GooKeyEqFn)(int64_t a, int64_t b);
enum { GOO_MAPKEY_STRING = 0, GOO_MAPKEY_INLINE = 1, GOO_MAPKEY_STRUCT = 2 };

typedef struct GooMapSV {
    struct GooMapEntrySV* head;
    int32_t key_kind;
    GooKeyEqFn key_eq;   // per-map struct-key comparator; NULL for string/inline maps
} GooMapSV;

GooMapSV* goo_map_new_sv(int32_t key_kind, GooKeyEqFn key_eq);
```

(Keep the surrounding forward decls / other prototypes exactly as they are — only these lines change.)

- [ ] **Step 2: Thread the comparator through the runtime**

In `src/runtime/runtime.c`, change the static `goo_map_key_eq` to take the map (so it can reach `key_eq`) and handle STRUCT:

```c
static int goo_map_key_eq(const GooMapSV* m, int64_t a, int64_t b) {
    if (m->key_kind == GOO_MAPKEY_STRING) {
        const char* sa = (const char*)(intptr_t)a;
        const char* sb = (const char*)(intptr_t)b;
        if (sa == sb) return 1;
        if (!sa || !sb) return 0;
        return strcmp(sa, sb) == 0;
    }
    if (m->key_kind == GOO_MAPKEY_STRUCT) {
        // Struct keys are stored as pointers to heap copies; a synthesized
        // per-field comparator does value-equality. NULL key_eq should never
        // happen for a STRUCT map (codegen always supplies it) — fall back to
        // pointer identity defensively.
        return m->key_eq ? m->key_eq(a, b) : (a == b);
    }
    return a == b;
}
```

Update its 4 call sites (~626, 641, 653, 709) from `goo_map_key_eq(m->key_kind, e->key, k)` to `goo_map_key_eq(m, e->key, k)`.

And `goo_map_new_sv`:

```c
GooMapSV* goo_map_new_sv(int32_t key_kind, GooKeyEqFn key_eq) {
    GooMapSV* m = goo_alloc(sizeof(GooMapSV));
    if (m) { m->head = NULL; m->key_kind = key_kind; m->key_eq = key_eq; }
    return m;
}
```

- [ ] **Step 3: Update the codegen extern + call sites to the new arity**

In `src/codegen/runtime_integration.c` (~339, the `goo_map_new_sv` extern declaration): change its LLVM function type to take TWO params — `{ i32, ptr }` returning the map pointer (the second param is the comparator fn-ptr as an opaque `ptr`). Match the exact declaration style used there for the existing 1-param form.

Grep every codegen call site of `goo_map_new_sv` (`grep -rn 'goo_map_new_sv' src/codegen/`) — at minimum `call_codegen.c` (make) and the map-literal creation site. This task passes a **NULL pointer** as the second arg at every site (struct keys are wired in Task 2):

```c
LLVMValueRef null_keyeq = LLVMConstPointerNull(LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0));
// goo_map_new_sv args become { key_kind, null_keyeq }
```

- [ ] **Step 4: Build + regression-verify (existing maps unaffected)**

```bash
make lexer                                 # exit 0 (runtime.h changed — incremental build rebuilds dependents)
bash scripts/run_golden.sh                 # 248 passed, 0 failed (UNCHANGED — the ABI change is transparent to string/inline maps)
make test                                  # 76 passed / 1 skip
./scripts/grammar-tripwire.sh              # PASS 82/256 (no grammar touched)
```

- [ ] **Step 5: Commit**

```bash
git add include/runtime.h src/runtime/runtime.c src/codegen/runtime_integration.c src/codegen/call_codegen.c
# plus any other codegen file with a goo_map_new_sv call site
git commit --no-gpg-sign -m "feat(runtime): GOO_MAPKEY_STRUCT + per-map key comparator fn-pointer (NULL-wired)"
```

---

### Task 2: Codegen — comparator synthesis, key packing, gate → `map[Point]int` works

**Files:**
- Modify: `src/codegen/codegen.c` (`codegen_map_key_kind` ~610, `codegen_map_key_to_slot`, `codegen_map_slot_to_key`; add the new `codegen_get_or_emit_struct_key_eq`)
- Modify: `include/codegen.h` (declare `codegen_get_or_emit_struct_key_eq`)
- Modify: codegen map-creation sites (`call_codegen.c` make ~510; the map-literal creation site) — pass the comparator instead of NULL for struct keys
- Modify: `src/types/type_checker.c` (`AST_MAP_TYPE` gate ~2753)
- Create: `examples/map_struct_key_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: `goo_map_new_sv(i32, ptr)` (Task 1); `goo_alloc`; `codegen_map_value_to_slot` / `codegen_map_slot_to_value`; `codegen_type_to_llvm`; the string-key arms in `codegen_map_key_to_slot`/`codegen_map_slot_to_key` (the mirror). String field access: a Goo string is `{ i8* ptr, i64 len }` — field 0 is the `char*` for `strcmp`.
- Produces: `LLVMValueRef codegen_get_or_emit_struct_key_eq(CodeGenerator*, TypeChecker*, Type* struct_type)` (the comparator fn value, cached); struct keys functioning end-to-end.

- [ ] **Step 1: Write the failing golden**

`examples/map_struct_key_probe.goo` (multi-line struct; go-run-verify):

```go
package main

import "fmt"

type Point struct {
	X int
	Y int
}

func main() {
	m := map[Point]int{}
	m[Point{X: 1, Y: 2}] = 5
	m[Point{X: 1, Y: 2}] = 7 // overwrite same key (distinct heap copy, equal fields)
	m[Point{X: 3, Y: 4}] = 9
	fmt.Println(m[Point{X: 1, Y: 2}])
	fmt.Println(m[Point{X: 3, Y: 4}])
	_, ok := m[Point{X: 9, Y: 9}]
	fmt.Println(ok)
	fmt.Println(len(m))
}
```

`examples/map_struct_key_probe.expected.txt` (produce via `go run` on the equivalent `.go` — MUST match byte-for-byte):

```
7
9
false
2
```

Confirm it fails pre-fix: `bin/goo -o build/msk examples/map_struct_key_probe.goo` → "map key type … not yet supported in v1".

- [ ] **Step 2: Synthesize the per-struct-type comparator**

Add `codegen_get_or_emit_struct_key_eq(CodeGenerator* codegen, TypeChecker* checker, Type* struct_type)` to `src/codegen/codegen.c` (declare in `include/codegen.h`). It returns a cached LLVM function `i32 (i64 a, i64 b)`. Cache keyed by struct type identity — reuse an existing per-type codegen cache if one exists (grep for how vtables/thunks are cached, e.g. by mangled type name); otherwise a generated unique name `goo.structeq.N` with a small `Type*`→`LLVMValueRef` map on the CodeGenerator. Emit (once per type):

```c
// Pseudocode-precise shape — transcribe to LLVM-C:
// define i32 @goo.structeq.<id>(i64 %a, i64 %b) {
//   %pa = inttoptr i64 %a to <StructTy>*
//   %pb = inttoptr i64 %b to <StructTy>*
//   ; for each field i:
//   ;   %fa = load field i from %pa ; %fb = load field i from %pb
//   ;   int/bool/char/ptr : %eq = icmp eq %fa, %fb ; br %eq, next, ret0
//   ;   float32/float64   : %eq = fcmp oeq %fa, %fb ; br %eq, next, ret0
//   ;   string ({i8*,i64}): %ca=extractvalue %fa,0 ; %cb=extractvalue %fb,0
//   ;                       %r = call i32 @strcmp(%ca,%cb) ; %eq=icmp eq %r,0 ; br %eq,next,ret0
//   ;   nested struct     : recurse — %na=inttoptr(ptrtoint fieldptr_a) etc.,
//   ;                       call @goo.structeq.<nested>(%na_i64, %nb_i64) ; br nonzero,next,ret0
//   ; all matched: ret i32 1
//   ; ret0: ret i32 0
// }
```

Emit with a builder positioned in the new function (save/restore the current insert block). Declare `strcmp` (`i32 (i8*, i8*)`) via the LLVMGetNamedFunction-or-declare pattern (mirror `codegen_string_from_cstr` in codegen.c). For a NESTED struct field, take the field's ADDRESS (GEP into %pa/%pb), `ptrtoint` to i64, and call that field type's comparator (obtained by a recursive `codegen_get_or_emit_struct_key_eq` — emit nested first). Use a single "mismatch" basic block that `ret i32 0`, and a chain of per-field compare→conditional-branch blocks ending in `ret i32 1`.

- [ ] **Step 3: Key kind, packing, and unpacking**

In `src/codegen/codegen.c`:
- `codegen_map_key_kind` (~610): `if (key_type && key_type->kind == TYPE_STRUCT) return 2; /*GOO_MAPKEY_STRUCT*/` before the string/inline return.
- `codegen_map_key_to_slot`: add a `TYPE_STRUCT` arm BEFORE the `codegen_map_value_to_slot` fallback — allocate a heap copy and return its pointer as the slot (mirror the string arm which stores the `char*`):

```c
if (kt && kt->kind == TYPE_STRUCT) {
    LLVMTypeRef sty = codegen_type_to_llvm(codegen, kt);
    LLVMValueRef alloc_fn = /* goo_alloc, via LLVMGetNamedFunction-or-declare (mirror codegen_alloc_local) */;
    LLVMValueRef sz = LLVMConstInt(i64, /* store size of sty */ LLVMABISizeOfType(codegen->target_data, sty), 0);
    LLVMValueRef mem = LLVMBuildCall2(codegen->builder, /*goo_alloc type*/, alloc_fn, &sz, 1, "skey_mem");
    LLVMValueRef sp  = LLVMBuildBitCast(codegen->builder, mem, LLVMPointerType(sty,0), "skey_ptr");
    LLVMBuildStore(codegen->builder, raw, sp);            // raw is the loaded struct value
    return LLVMBuildPtrToInt(codegen->builder, sp, i64, "skey_slot");
}
```

  (Find the codegen's target-data handle for `LLVMABISizeOfType`; grep how alloca sizes are computed elsewhere, or use `LLVMStoreSizeOfType`. If no target-data is handy, compute size via `LLVMSizeOf(sty)` which yields an i64 value directly — use whichever the codebase already uses for struct sizing.)
- `codegen_map_slot_to_key`: add a `TYPE_STRUCT` arm — `IntToPtr` the slot to `StructTy*` and `load` the struct value (mirror the string arm's `IntToPtr` + reconstruct):

```c
if (key_type && key_type->kind == TYPE_STRUCT) {
    LLVMTypeRef sty = codegen_type_to_llvm(codegen, key_type);
    LLVMValueRef sp = LLVMBuildIntToPtr(codegen->builder, slot, LLVMPointerType(sty,0), "skey_ptr");
    return LLVMBuildLoad2(codegen->builder, sty, sp, "skey_val");
}
```

- [ ] **Step 4: Pass the comparator at map creation + open the gate**

At every codegen `goo_map_new_sv` call site (make ~call_codegen.c:510, and the map-literal creation site): when the key type is a struct, pass `codegen_get_or_emit_struct_key_eq(codegen, checker, key_type)` (bit-cast to the opaque `ptr` param type) as the second arg instead of NULL; keep NULL for non-struct keys.

Open the comparability gate in `src/types/type_checker.c` `AST_MAP_TYPE` (~2753): admit a `TYPE_STRUCT` key iff recursively comparable. Add a helper near the gate:

```c
// A struct is a valid map key iff every field is comparable: a scalar
// (int/bool/char/pointer), float, string, or a nested comparable struct.
// Array fields are Go-comparable but deferred (v1); slice/map/func fields are
// never comparable. Returns 1 if usable as a key; on rejection sets *why to a
// static reason string for the diagnostic.
static int struct_is_comparable_key(Type* t, const char** why) {
    if (t->kind != TYPE_STRUCT) return 0;
    for (size_t i = 0; i < t->data.struct_type.field_count; i++) {
        Type* f = t->data.struct_type.fields[i].type;
        switch (f->kind) {
            case TYPE_STRING: case TYPE_BOOL: case TYPE_CHAR:
            case TYPE_FLOAT32: case TYPE_FLOAT64: case TYPE_POINTER:
                break;
            case TYPE_STRUCT:
                if (!struct_is_comparable_key(f, why)) return 0;
                break;
            case TYPE_ARRAY: *why = "array"; return 0;   // Go-legal, deferred
            default:
                if (type_is_integer(f)) break;
                *why = "noncomparable"; return 0;         // slice/map/func/...
        }
    }
    return 1;
}
```

In the gate, before the existing reject: if `key_type->kind == TYPE_STRUCT`, call it — on success set `key_ok = 1`; on `"array"` emit "not yet supported in v1", on `"noncomparable"` emit "invalid map key type: struct has a non-comparable field". (Adapt to the gate's existing `key_ok` / two-reason error structure — read it first.)

- [ ] **Step 5: Build, run, gate**

```bash
make lexer                                 # exit 0
bin/goo -o build/msk examples/map_struct_key_probe.goo && ./build/msk   # 7 / 9 / false / 2
bash scripts/run_golden.sh                 # 249 passed, 0 failed (+1)
make test                                  # 76/1
./scripts/grammar-tripwire.sh              # PASS 82/256
```

- [ ] **Step 6: Commit**

```bash
git add src/codegen/codegen.c include/codegen.h src/codegen/call_codegen.c src/types/type_checker.c examples/map_struct_key_probe.*
git commit --no-gpg-sign -m "feat(codegen,types): struct map keys via synthesized per-field comparator"
```

---

### Task 3: Correctness goldens (string-field, nested, float) + range + reject probes

**Files:**
- Create: `examples/map_struct_key_string.goo` + `.expected.txt`, `examples/map_struct_key_nested.goo` + `.expected.txt`, `examples/map_struct_key_range.goo` + `.expected.txt`
- Modify: `Makefile` (`struct-map-key-reject-probe` + `verify:`)

**Interfaces:** consumes Task 2's working struct keys.

- [ ] **Step 1: The string-field correctness golden (the centerpiece)**

`examples/map_struct_key_string.goo` — two keys whose string fields have DISTINCT addresses but EQUAL content must hit the same entry (proves content-equality, not pointer-equality):

```go
package main

import "fmt"

type Name struct {
	First string
	Last  string
}

func main() {
	m := map[Name]int{}
	m[Name{First: "Ada", Last: "Lovelace"}] = 1
	// build an equal key from concatenation so its field pointers differ
	first := "A" + "da"
	last := "Love" + "lace"
	m[Name{First: first, Last: last}] = 2 // same key by content — overwrites
	fmt.Println(m[Name{First: "Ada", Last: "Lovelace"}])
	fmt.Println(len(m))
}
```

Expected (via `go run`): `2` then `1`. Confirm `bin/goo` matches.

- [ ] **Step 2: Nested-struct-key + float-field goldens**

`examples/map_struct_key_nested.goo`:

```go
package main

import "fmt"

type Point struct {
	X int
	Y int
}
type Seg struct {
	A   Point
	B   Point
	Tag string
}

func main() {
	m := map[Seg]int{}
	m[Seg{A: Point{X: 0, Y: 0}, B: Point{X: 1, Y: 1}, Tag: "up"}] = 42
	fmt.Println(m[Seg{A: Point{X: 0, Y: 0}, B: Point{X: 1, Y: 1}, Tag: "up"}])
	_, ok := m[Seg{A: Point{X: 0, Y: 0}, B: Point{X: 1, Y: 1}, Tag: "down"}]
	fmt.Println(ok)
}
```

Expected via `go run`: `42` then `false`. Also add a float-field key case (a `struct{ K float64; N int }` key: insert, retrieve, miss) — its own golden or folded into nested; go-run-verify.

- [ ] **Step 3: Range golden**

`examples/map_struct_key_range.goo` — range a `map[Point]int`, sum `key.X + key.Y + value` (order-independent so it matches regardless of iteration order):

```go
package main

import "fmt"

type Point struct {
	X int
	Y int
}

func main() {
	m := map[Point]int{}
	m[Point{X: 1, Y: 2}] = 10
	m[Point{X: 3, Y: 4}] = 20
	sum := 0
	for k, v := range m {
		sum += k.X + k.Y + v
	}
	fmt.Println(sum)
}
```

Expected via `go run`: `40`. Confirm `bin/goo` matches (exercises `codegen_map_slot_to_key`'s struct arm).

- [ ] **Step 4: Reject probes**

Add `struct-map-key-reject-probe` to the `Makefile` (compile-must-fail + message grep), covering a struct key with a SLICE field ("not comparable" / "invalid map key type") and a struct key with an ARRAY field ("not yet supported in v1"). Mirror the existing `mapkey-reject-probe` structure. Wire it into the `verify:` target list.

- [ ] **Step 5: Gates**

```bash
make lexer                                 # exit 0
bash scripts/run_golden.sh                 # 249 + new goldens, 0 failed
make struct-map-key-reject-probe           # PASS
make mapkey-reject-probe                   # still PASS (string/int/etc. gate unchanged)
make test                                  # 76/1
```

- [ ] **Step 6: Commit**

```bash
git add examples/map_struct_key_*.goo examples/map_struct_key_*.expected.txt Makefile
git commit --no-gpg-sign -m "test(maps): struct-key goldens (string-field, nested, float, range) + reject probe"
```

---

### Task 4: Full sweep + handoff

**Files:** Modify `.handoff.md`.

- [ ] **Step 1: Full sweep (real exit codes)**

```bash
make lexer                                 # exit 0
make test                                  # 76/1
bash scripts/run_golden.sh                 # all pass, 0 failed
eval "$(opam env --switch=default)"
make verify                                # ALL GREEN GATES PASSED (incl. struct-map-key-reject-probe)
make ccomp-link                            # PASS
./scripts/grammar-tripwire.sh              # PASS 82/256
```

- [ ] **Step 2: Update `.handoff.md`**

Prepend: struct-typed map keys SHIPPED — comparable structs (scalar/float/string/nested fields) are now valid map keys via a synthesized per-field comparator; heap-copied key + `GOO_MAPKEY_STRUCT` + `key_eq` fn-ptr. Record the deferrals: array-in-struct-key fields + top-level array keys (Go-legal, "not yet supported in v1"), interface keys (separate cycle, now the sole remaining map-key gap), top-level float keys (trivial adjacent win, still rejected). Note the `goo_map_new_sv` ABI change (now 2-arg). Update the queue: interface map keys is the remaining item.

- [ ] **Step 3: Commit**

```bash
git add .handoff.md
git commit --no-gpg-sign -m "docs(handoff): struct-typed map keys shipped; interface keys remain"
```

After Task 4: push, PR, fresh-context whole-branch review before merge (mandatory — with deliberate probes per the #114/#115 lesson: distinct-address equal-content string-field keys, nested-struct keys, a NaN float-field key never found (matches Go), a pointer-field struct key comparing by pointer identity, and confirming string/int/pointer maps are unregressed).

---

## Execution notes (controller)

- SDD economy mode adapted for memory: **single implementer subagent per task + controller (main-loop) sequential probes** — do NOT fan out 10 parallel verifiers (they each run `make`/`go run` and spiked memory earlier this session).
- Task 1 is a pure runtime-ABI change; its reviewer confirms string/inline maps are byte-identical (golden unchanged) and every `goo_map_new_sv` call site passes the new arg.
- Task 2 is the intricate one (comparator synthesis). Controller probes DELIBERATELY hit: distinct-address equal-content string-field keys, nested keys, pointer-field keys (pointer identity), and the reject cases — not just the happy `map[Point]int` path.
- Golden count: 248 → 249 (Task 2) → +≈4 (Task 3). bison stays 82/256 (no grammar). `goo_map_new_sv` ABI goes 1-arg → 2-arg in Task 1 — every call site must be updated in the same task or the build breaks.
