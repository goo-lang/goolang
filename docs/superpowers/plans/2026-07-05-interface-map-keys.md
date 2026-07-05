# Interface-Typed Map Keys Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Support `map[InterfaceType]V` / `map[any]V` — equality by dynamic-type identity + dynamic-value equality, with a Go-faithful runtime panic on uncomparable dynamic types.

**Architecture:** Prepend a per-concrete-type equality function to each vtable (slot 0; method thunks shift to 1..n). An interface key is a heap-copied `{vtable, data}`; a generic runtime comparator does vtable-identity → dispatch to `vtable[0]` on the boxed data words. Spec: `docs/superpowers/specs/2026-07-05-interface-map-keys-design.md`.

**Tech Stack:** C23, LLVM-C API.

## Global Constraints

- Branch: `feat/interface-map-keys` (exists, base main @ a7008b8). Commit `--no-gpg-sign`; pre-commit runs `make test`.
- **NO grammar change** — `./scripts/grammar-tripwire.sh` must stay PASS **82 S/R + 256 R/R** (no-op sanity).
- **The vtable ABI changes in Task 1** (slot 0 = eq, methods at idx+1). The regression net is the FULL existing golden suite — any wrong shift breaks interface method dispatch / embedding / type assertions LOUDLY (those goldens go red). `make lexer` is incremental/header-aware (PR #123); `include/runtime.h` / `include/codegen.h` edits still just need `make lexer`.
- Interface `data` for a VALUE concrete is a pointer to a `goo_alloc`'d heap copy (interface_codegen.c:262-271); for a POINTER concrete it is the pointer itself. The eq fn derefs value concretes, compares pointer concretes by identity.
- `goo_panic(const char* msg)` is `__attribute__((noreturn))` (runtime.h:50). `goo_alloc(size_t)`.
- Reuse: `codegen_get_or_emit_struct_key_eq(codegen, checker, struct_type)` (#129, codegen.h:495) IS the struct concrete's eq fn (signature already `i32(i64,i64)` over ptr-to-copy). `goo_map_new_sv(int32_t key_kind, GooKeyEqFn key_eq)` and `GooKeyEqFn = int(*)(int64_t,int64_t)` (#129).
- Baselines: golden **257/0**, `make test` **76/1**, bison **82/256**, `make verify` ALL GREEN, `make ccomp-link` PASS (needs `eval "$(opam env --switch=default)"`). Struct/interface defs in tests MUST be multi-line (single-line struct bodies hit an unrelated ASI quirk). Real exit codes only.

---

### Task 1: Per-type eq function + vtable slot-0 + dispatch shift (the ABI change)

**Files:**
- Modify: `src/codegen/codegen.c` (add `codegen_get_or_emit_type_eq`), `include/codegen.h` (declare it)
- Modify: `src/codegen/interface_codegen.c` (`codegen_interface_vtable` ~182 — emit n+1 slots; `codegen_interface_dispatch` ~308 — GEP idx→idx+1)

**Interfaces:**
- Consumes: `codegen_get_or_emit_struct_key_eq` (#129); `goo_alloc`; `goo_panic`; `codegen_type_to_llvm`; the string llvm type `{i8*, i64}` (field 0 = char*).
- Produces: `LLVMValueRef codegen_get_or_emit_type_eq(CodeGenerator*, TypeChecker*, Type* concrete)` — a cached `i32 (i64 a, i64 b)` fn; vtables whose slot 0 is that fn.

- [ ] **Step 1: Synthesize the per-concrete-type eq fn**

Add to `src/codegen/codegen.c` (declare in `include/codegen.h`) `codegen_get_or_emit_type_eq`, cached by `Type*` identity (add a small `Type*`→`LLVMValueRef` cache on CodeGenerator, or reuse the structeq cache pattern). Emits `i32 @goo.typeeq.<id>(i64 a, i64 b)`:
- **TYPE_POINTER**: `icmp eq i64 %a, %b` → zext to i32 → ret. (data words are the pointers.)
- **integer/bool/char (`type_is_integer` || TYPE_BOOL || TYPE_CHAR)**: `%pa = inttoptr %a to T*; %pb = inttoptr %b to T*; %va = load; %vb = load; icmp eq; zext; ret`.
- **TYPE_FLOAT32/FLOAT64**: same but `fcmp oeq`.
- **TYPE_STRING**: `%pa = inttoptr %a to {i8*,i64}*; load; extractvalue 0 → char*`; same for b; `%r = call i32 @strcmp(ca, cb); icmp eq %r, 0; zext; ret`. Declare `strcmp` via the get-or-declare pattern (mirror `codegen_string_from_cstr`, codegen.c).
- **TYPE_STRUCT**: `return codegen_get_or_emit_struct_key_eq(codegen, checker, concrete);` — its `i32(i64,i64)` over ptr-to-copy is exactly right; no new fn.
- **TYPE_SLICE / TYPE_MAP / (func)**: return a single shared `i32 @goo.uncmpeq(i64,i64)` whose body is `call void @goo_panic(i8* "comparing uncomparable map key type"); unreachable`. Emit once (cache by a sentinel).
Save/restore the builder insert position around emitting each fn.

- [ ] **Step 2: Emit the eq fn at vtable slot 0**

In `codegen_interface_vtable` (interface_codegen.c:182): the vtable array becomes `n+1` ptr slots. slot 0 = `LLVMBuildBitCast`/const-bitcast of `codegen_get_or_emit_type_eq(codegen, checker, concrete)` to the vtable's ptr element type; slots 1..n = the existing method thunks (unchanged order). Update the `LLVMArrayType(ptrty, n)` to `n+1` and the `slots` array construction accordingly (allocate n+1, put eq at [0], thunks at [1..n]).

- [ ] **Step 3: Shift method dispatch by one slot**

In `codegen_interface_dispatch` (interface_codegen.c ~308): the dispatch GEP `gep_idx = LLVMConstInt(i64, idx, 0)` becomes `LLVMConstInt(i64, idx + 1, 0)` — methods now live at slot idx+1 (slot 0 is eq). This is the ONLY dispatch-index change; grep `goo.vtable`/vtable indexing to confirm no other site computes a raw method slot.

- [ ] **Step 4: Build + REGRESSION-verify (the shift must not break dispatch)**

```bash
make lexer                                 # exit 0
bash scripts/run_golden.sh                 # 257 passed, 0 failed — UNCHANGED (interface method dispatch, embedding, type assertions all still work; a wrong shift makes these RED)
make test                                  # 76/1
./scripts/grammar-tripwire.sh              # PASS 82/256
```
Also spot-run an existing interface golden directly (e.g. `bin/goo -o build/e examples/embed_iface_probe.goo && ./build/e`) and confirm it matches `examples/embed_iface_probe.expected.txt`.

- [ ] **Step 5: Commit**

```bash
git add src/codegen/codegen.c include/codegen.h src/codegen/interface_codegen.c
git commit --no-gpg-sign -m "feat(codegen): vtable-carried per-type eq fn (slot 0); method dispatch shifts to idx+1"
```

---

### Task 2: Runtime comparator + IFACE kind + key packing + gate → `map[any]int` works

**Files:**
- Modify: `include/runtime.h` (kind enum, `goo_iface_key_eq` decl), `src/runtime/runtime.c` (`goo_iface_key_eq`, `goo_map_key_eq` IFACE arm)
- Modify: `src/codegen/runtime_integration.c` (`goo_iface_key_eq` extern), `src/codegen/codegen.c` (`codegen_map_key_kind`, `codegen_map_key_to_slot`, `codegen_map_slot_to_key`), map-creation sites (make + literal)
- Modify: `src/types/type_checker.c` (AST_MAP_TYPE gate)
- Create: `examples/map_iface_key_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: Task 1's vtable slot-0 eq; `GooKeyEqFn`/`goo_map_new_sv(kind, key_eq)`/`goo_map_key_eq(const GooMapSV*, ...)` (#129); the boxed interface value `{void* vtable; void* data}`.
- Produces: `GOO_MAPKEY_IFACE = 3`; `int goo_iface_key_eq(int64_t a, int64_t b)`; functioning interface keys.

- [ ] **Step 1: Write the failing golden**

`examples/map_iface_key_probe.goo` (go-run-verify):

```go
package main

import "fmt"

func main() {
	m := map[any]int{}
	m[1] = 10
	m[1] = 20 // same dynamic type+value → overwrite
	m[2] = 30
	m["1"] = 40 // different dynamic type → distinct entry
	fmt.Println(m[1])
	fmt.Println(m[2])
	fmt.Println(m["1"])
	_, ok := m[9]
	fmt.Println(ok)
	fmt.Println(len(m))
}
```

`.expected.txt` (via `go run`): `20` / `30` / `40` / `false` / `3`. Confirm pre-fix: `bin/goo` rejects `map[any]int` at the gate.

- [ ] **Step 2: Runtime kind + comparator**

`include/runtime.h`: add `GOO_MAPKEY_IFACE = 3` to the kind enum; declare `int goo_iface_key_eq(int64_t a, int64_t b);`.

`src/runtime/runtime.c`: add the comparator (the boxed interface value is two pointer words; a key slot is a pointer to one such value):

```c
int goo_iface_key_eq(int64_t a, int64_t b) {
    void** ia = (void**)(intptr_t)a;  // -> { vtable, data }
    void** ib = (void**)(intptr_t)b;
    void* vta = ia[0]; void* vtb = ib[0];
    if (vta == NULL && vtb == NULL) return 1;   // both nil interfaces
    if (vta != vtb) return 0;                    // different dynamic type (or one nil)
    GooKeyEqFn eq = (GooKeyEqFn)((void**)vta)[0]; // vtable slot 0 = per-type eq
    return eq((int64_t)(intptr_t)ia[1], (int64_t)(intptr_t)ib[1]); // compare the data words
}
```

In `goo_map_key_eq` (runtime.c) add `if (m->key_kind == GOO_MAPKEY_IFACE) return m->key_eq ? m->key_eq(a, b) : (a == b);` (same shape as the STRUCT arm).

- [ ] **Step 3: Codegen kind + packing + creation + extern**

`src/codegen/runtime_integration.c`: declare the `goo_iface_key_eq` extern (`i32 (i64, i64)`), mirroring how other runtime fns are declared, so its address can be taken.

`src/codegen/codegen.c`:
- `codegen_map_key_kind`: `if (key_type && key_type->kind == TYPE_INTERFACE) return 3; /*GOO_MAPKEY_IFACE*/`.
- `codegen_map_key_to_slot`: `TYPE_INTERFACE` arm (mirror the struct arm) — `goo_alloc(sizeof {ptr,ptr})`, store the loaded interface value, `PtrToInt` → slot.
- `codegen_map_slot_to_key`: `TYPE_INTERFACE` arm — `IntToPtr` slot → `{ptr,ptr}*`, `load` the interface value.

Map-creation sites (make in call_codegen.c, literal in expression_codegen.c — the two `goo_map_new_sv` call sites): when the key is `TYPE_INTERFACE`, pass `LLVMGetNamedFunction(module, "goo_iface_key_eq")` (bitcast to the comparator param ptr) as the 2nd arg.

- [ ] **Step 4: Open the gate**

`src/types/type_checker.c` AST_MAP_TYPE gate: admit `TYPE_INTERFACE` keys — add `key_type->kind == TYPE_INTERFACE` to the `key_ok` condition (interfaces are statically comparable; the uncomparable dynamic case is the runtime panic-stub, NOT a compile error). Do NOT move TYPE_INTERFACE into the deferred-reject branch.

- [ ] **Step 5: Build, run, gate**

```bash
make lexer                                 # exit 0
bin/goo -o build/mik examples/map_iface_key_probe.goo && ./build/mik   # 20 / 30 / 40 / false / 3
bash scripts/run_golden.sh                 # 258 passed, 0 failed (+1)
make test                                  # 76/1
./scripts/grammar-tripwire.sh              # PASS 82/256
```

- [ ] **Step 6: Commit**

```bash
git add include/runtime.h src/runtime/runtime.c src/codegen/runtime_integration.c src/codegen/codegen.c src/codegen/call_codegen.c src/codegen/expression_codegen.c src/types/type_checker.c examples/map_iface_key_probe.*
git commit --no-gpg-sign -m "feat(runtime,codegen,types): interface map keys via vtable-carried equality dispatch"
```

---

### Task 3: Correctness goldens (string/struct/pointer/nil/Stringer) + range + uncomparable-panic probe

**Files:**
- Create: `examples/map_iface_key_string.goo`+`.expected.txt`, `_struct.goo`+`.expected.txt`, `_nil.goo`+`.expected.txt`, `_stringer.goo`+`.expected.txt`, `_range.goo`+`.expected.txt`
- Modify: `Makefile` (`iface-map-key-uncomparable-probe` + `verify:`)

**Interfaces:** consumes Task 2's working interface keys.

- [ ] **Step 1: String + struct + pointer dynamic-value goldens**

`examples/map_iface_key_string.goo` — distinct-address equal-content string dynamic keys hit the same entry:

```go
package main

import "fmt"

func main() {
	m := map[any]int{}
	m["Ada"] = 1
	s := "A" + "da"
	m[s] = 2 // same string content, different address → overwrite
	fmt.Println(m["Ada"])
	fmt.Println(len(m))
}
```

Expected via `go run`: `2` / `1`. Add `_struct.goo` (a `struct{X,Y int}` dynamic value as `any` key — insert/retrieve/miss, using a struct built two ways to prove content-equality) and confirm each matches `go run`.

- [ ] **Step 2: nil-interface + Stringer + pointer goldens**

`examples/map_iface_key_nil.goo` — a nil `any` key:

```go
package main

import "fmt"

func main() {
	m := map[any]int{}
	var k any // nil
	m[k] = 7
	fmt.Println(m[nil])
	m[1] = 8
	fmt.Println(len(m)) // nil and 1 are distinct
}
```

Expected via `go run`: `7` / `2`. Add `_stringer.goo` (`map[Stringer]int` with two concrete implementers — distinct entries; same concrete+value → same entry) and a pointer-dynamic-value case (two `*T` to equal values do NOT collide; same pointer does). go-run-verify each.

- [ ] **Step 3: Range golden**

`examples/map_iface_key_range.goo` — range a `map[any]int`, accumulate values order-independently (interface keys aren't easily summable, so sum the VALUES): insert 3 entries, `for _, v := range m { sum += v }`, print sum. Expected via `go run`. Exercises `codegen_map_slot_to_key`'s interface arm (the key var binds even if unused — use `for _, v` or `for k, v` reading nothing structural from k).

- [ ] **Step 4: Uncomparable-dynamic-key panic probe**

`Makefile` `iface-map-key-uncomparable-probe` (compile-then-run, assert RUNTIME abort — this is a runtime panic, not a compile error; mirror the runtime abort probes like `panic-abort-probe`):

```makefile
iface-map-key-uncomparable-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== iface-map-key-uncomparable-probe: uncomparable dynamic key must runtime-panic ==="
	@printf 'package main\nfunc main(){\n\tm := map[any]int{}\n\tvar k any = []int{1,2,3}\n\tm[k] = 1\n\tm[k] = 2\n}\n' > build/imk_unc.goo
	@$(COMPILER) -o build/imk_unc build/imk_unc.goo 2>build/imk_unc.cerr || { echo "iface-map-key-uncomparable-probe: FAIL (should COMPILE, panic at runtime)"; cat build/imk_unc.cerr; exit 1; }
	@./build/imk_unc 2>build/imk_unc.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "iface-map-key-uncomparable-probe: FAIL (no panic)"; exit 1; fi; \
	  if ! grep -qi "comparing uncomparable" build/imk_unc.err; then echo "iface-map-key-uncomparable-probe: FAIL (wrong message)"; cat build/imk_unc.err; exit 1; fi
	@echo "iface-map-key-uncomparable-probe: PASS"
```

Wire it into `verify:`. (Note: it must COMPILE — the interface *type* is comparable; the panic is at the second `m[k]=2`, the first comparison against the existing entry.)

- [ ] **Step 5: Gates**

```bash
make lexer                                 # exit 0
bash scripts/run_golden.sh                 # 258 + new goldens, 0 failed
make iface-map-key-uncomparable-probe      # PASS
make mapkey-reject-probe                   # still PASS
make struct-map-key-reject-probe           # still PASS (struct keys unaffected)
make test                                  # 76/1
```

- [ ] **Step 6: Commit**

```bash
git add examples/map_iface_key_*.goo examples/map_iface_key_*.expected.txt Makefile
git commit --no-gpg-sign -m "test(maps): interface-key goldens (string/struct/pointer/nil/Stringer/range) + uncomparable panic probe"
```

---

### Task 4: Full sweep + handoff

**Files:** Modify `.handoff.md`.

- [ ] **Step 1: Full sweep**

```bash
make clean && make lexer                   # exit 0
make test                                  # 76/1
bash scripts/run_golden.sh                 # all pass, 0 failed
eval "$(opam env --switch=default)"
make verify                                # ALL GREEN (incl. iface-map-key-uncomparable-probe)
make ccomp-link                            # PASS
./scripts/grammar-tripwire.sh              # PASS 82/256
```

- [ ] **Step 2: Update `.handoff.md`**

Prepend: interface-typed map keys SHIPPED — `map[Iface]V`/`map[any]V` via a per-concrete-type eq fn carried at vtable slot 0 (methods shifted to idx+1), a generic `goo_iface_key_eq` doing vtable-identity → `vtable[0]` dispatch, `GOO_MAPKEY_IFACE`; uncomparable dynamic types runtime-panic (Go-faithful). **This closes the map-key story** (string/int/bool/rune/byte/pointer + non-string + struct + interface). Note the vtable ABI change (slot 0 = eq). Record the timing nuance (panic on first compare vs Go's insert-hash) and the still-deferred float top-level keys (trivial). Update the queue.

- [ ] **Step 3: Commit**

```bash
git add .handoff.md
git commit --no-gpg-sign -m "docs(handoff): interface-typed map keys shipped; map-key story complete"
```

After Task 4: push, PR, fresh-context whole-branch review before merge (mandatory — with deliberate probes per the #114/#115 lesson: cross-type non-collision `any(int64(1))` vs `any(1)` vs `any("1")`, distinct-address string content-equality, struct dynamic value, pointer identity, nil key, an uncomparable dynamic key runtime-panic, AND a full regression pass on interface method dispatch / embedding / type assertions since the vtable shifted).

---

## Execution notes (controller)

- Memory-light: single implementer subagent per task + controller (main-loop) SEQUENTIAL probes. Do NOT fan out parallel verifiers.
- **Task 1 is the risk** (vtable ABI shift). Its regression test IS the existing golden suite staying 257/0 — a wrong `idx+1` breaks interface method dispatch loudly. Controller MUST directly re-run a few interface/assertion goldens (embed_iface, typeassert, typeswitch) after Task 1.
- Task 2 controller probes: cross-type non-collision (`any(1)` vs `any("1")`), and confirm existing struct/string/int maps are unregressed.
- Golden count: 257 → 258 (Task 2) → +≈5 (Task 3). bison stays 82/256 (no grammar). The vtable slot count changed n→n+1 — every interface method call now dispatches through idx+1; the ONLY dispatch site is `codegen_interface_dispatch` (confirm no second site in Task 1 Step 3).
