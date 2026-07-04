# Non-String Map Keys Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `map[K]V` for `K` in {string, integer/uint widths, bool, rune, byte, pointer} — non-string keys via an int64 key slot + a per-map key-kind tag, keeping the linear-scan assoc-list (no hash).

**Architecture:** The map is a linear-scan association list, so non-string keys need only equality, not hashing. Generalize `GooMapEntrySV.key` from `const char*` to an `int64_t` slot (the representation #110 already uses for values), add `key_kind` (STRING vs INLINE) to the map, and make the one per-kind difference a `strcmp`-vs-`==` branch. Codegen packs keys to slots (reusing #110's value-slot helper for inline; char*→i64 directly for strings) and lifts the two string-only gates (typecheck + range).

**Tech Stack:** C23, LLVM-C API, own C runtime.

## Global Constraints

- Branch: `feat/non-string-map-keys` (exists, base main @ 915265d). Commit `--no-gpg-sign`; pre-commit runs `make test`.
- Spec: `docs/superpowers/specs/2026-07-04-non-string-map-keys-design.md`. Scope: INLINE keys (int/uint widths, bool, rune, byte, pointer) + string. Structs/floats/interfaces/arrays = deferred-comparable (clean reject); slices/maps/funcs = non-comparable (clean reject).
- NO grammar changes (map key types already parse). `./scripts/grammar-tripwire.sh` must stay PASS at 82 S/R + 256 R/R — a delta means an unintended parser touch → STOP.
- `include/runtime.h` / `include/codegen.h` header edits: after editing, `make clean && make lexer` (no header deps — stale objects silently miscompile).
- The STRING key path must stay byte-identical behavior (same strcmp on the same char*). Every existing `map[string]V` golden is the regression gate.
- Baselines: golden 232/0 (`bash scripts/run_golden.sh`), `make test` 76 passed/1 skipped, bison 82/256.
- Every golden's expected output produced by `go run` on an equivalent `.go` program (go installed). Real exit codes only (never a pipeline `$?`). ccomp needs `eval "$(opam env --switch=default)"`.
- Reuse, don't duplicate: `codegen_map_value_to_slot` / `codegen_map_slot_to_value` / `codegen_map_value_is_inline` (codegen.c:529/535/566, decls codegen.h:434-436).

---

### Task 1: Runtime — slot key + key_kind + equality

**Files:**
- Modify: `include/runtime.h` (GooMapSV struct + key_kind enum + signatures), `src/runtime/runtime.c` (601-695: new/set/get/get_ok/delete/iter + `goo_map_key_eq`)

**Interfaces:**
- Produces (Task 2 codegen consumes these exact signatures):
  - `GooMapSV* goo_map_new_sv(int32_t key_kind)`
  - `void goo_map_set_sv(GooMapSV* m, int64_t k, int64_t v)`
  - `int64_t goo_map_get_sv(GooMapSV* m, int64_t k)`
  - `void goo_map_get_sv_ok(GooMapSV* m, int64_t k, int64_t* out, int* found)`
  - `void goo_map_delete_sv(GooMapSV* m, int64_t k)`
  - `int goo_map_iter_next_sv(struct GooMapEntrySV** cursor, int64_t* key_out, int64_t* val_out)`
  - `int64_t goo_map_len_sv(GooMapSV* m)` (unchanged)
  - enum: `GOO_MAPKEY_STRING = 0`, `GOO_MAPKEY_INLINE = 1`

- [ ] **Step 1: runtime.h — enum, struct field, signatures**

In `include/runtime.h`, near the map decls (~136-180):

```c
// Map key kind: how goo_map_key_eq compares two int64 key slots. STRING = the
// slot holds a char*, compared by strcmp; INLINE = the slot holds the key's
// bits (int/uint/bool/rune/byte/pointer), compared by ==. New kinds
// (struct/float) append here later.
enum { GOO_MAPKEY_STRING = 0, GOO_MAPKEY_INLINE = 1 };

typedef struct GooMapSV {
    struct GooMapEntrySV* head;
    int32_t key_kind;
} GooMapSV;
```

Update the six signatures listed in Interfaces (key param `const char*`→`int64_t`; `goo_map_new_sv` gains `int32_t key_kind`; iter `key_out` `const char**`→`int64_t*`). Update the doc comments that say "string-keyed" to note the slot key + key_kind.

- [ ] **Step 2: runtime.c — `goo_map_key_eq` + creation + entry key type**

At the top of the map section in `src/runtime/runtime.c` (before `goo_map_new_sv`):

```c
// Compare two int64 key slots per the map's key_kind. STRING: the slots hold
// char* — strcmp. INLINE: the slots hold the key's bits — direct ==.
static int goo_map_key_eq(int32_t kind, int64_t a, int64_t b) {
    if (kind == GOO_MAPKEY_STRING) {
        const char* sa = (const char*)(intptr_t)a;
        const char* sb = (const char*)(intptr_t)b;
        if (sa == sb) return 1;
        if (!sa || !sb) return 0;
        return strcmp(sa, sb) == 0;
    }
    return a == b;
}
```

Change `GooMapEntrySV.key` to `int64_t` (find its struct definition in runtime.c — it is the file-local entry struct). `goo_map_new_sv`:

```c
GooMapSV* goo_map_new_sv(int32_t key_kind) {
    GooMapSV* m = goo_alloc(sizeof(GooMapSV));
    if (m) { m->head = NULL; m->key_kind = key_kind; }
    return m;
}
```

- [ ] **Step 3: runtime.c — set/get/get_ok/delete use `goo_map_key_eq`**

In each of `goo_map_set_sv`, `goo_map_get_sv`, `goo_map_get_sv_ok`, `goo_map_delete_sv`: change the key param to `int64_t k`, and replace the `strcmp(e->key, k) == 0` comparison with `goo_map_key_eq(m->key_kind, e->key, k)`. In `set`, `e->key = k;` stores the slot verbatim (the string-ownership note stays: the map never owns key storage — for a STRING key the slot is the caller's char*). Example (set):

```c
void goo_map_set_sv(GooMapSV* m, int64_t k, int64_t v) {
    if (!m) return;
    GooMapEntrySV* e = (GooMapEntrySV*)m->head;
    while (e) {
        if (goo_map_key_eq(m->key_kind, e->key, k)) { e->value = v; return; }
        e = e->next;
    }
    e = goo_alloc(sizeof(GooMapEntrySV));
    if (!e) return;
    e->key = k; e->value = v; e->next = (GooMapEntrySV*)m->head; m->head = e;
}
```

(The old `if (!k) return;` null guards drop — an int64 key of 0 is a valid key, e.g. `m[0]`. Only `!m` guards remain.)

- [ ] **Step 4: runtime.c — iter yields the slot key**

`goo_map_iter_next_sv`: `key_out` becomes `int64_t*`; `*key_out = (*cursor)->key;` (the slot, not a char*).

- [ ] **Step 5: Build + commit**

```bash
make clean && make lexer                     # exit 0 (mandatory — headers changed)
make test                                    # 76/1
```
(Codegen still emits the OLD call shapes → golden suite will FAIL until Task 2. That is expected; do NOT run the golden suite as a gate here. Confirm the RUNTIME compiles and the unit tests pass.)

```bash
git add include/runtime.h src/runtime/runtime.c
git commit --no-gpg-sign -m "feat(runtime): slot map keys + key_kind (strcmp vs == equality)"
```

---

### Task 2: Codegen — key→slot, key_kind at creation, range key, externs

**Files:**
- Modify: `src/codegen/codegen.c` (add two key helpers), `include/codegen.h` (their decls)
- Modify: `src/codegen/runtime_integration.c` (339+: extern sigs), `src/codegen/expression_codegen.c` (map literal 223-240; `m[k]=v` 1104-1120), `src/codegen/composite_codegen.c` (`m[k]` read 81-99), `src/codegen/function_codegen.c` (comma-ok 1580-1640), `src/codegen/call_codegen.c` (make 505; delete 604), `src/codegen/statement_codegen.c` (range 833-919)

**Interfaces:**
- Consumes: Task 1's runtime signatures; `codegen_map_value_to_slot(codegen, value, type)` / `codegen_map_slot_to_value(codegen, slot, type)` / `codegen_map_value_is_inline(type)` (codegen.c).
- Produces: `int codegen_map_key_kind(Type* key_type)` (→ `GOO_MAPKEY_STRING`/`INLINE` as an int constant) and `LLVMValueRef codegen_map_key_to_slot(CodeGenerator*, TypeChecker*, LLVMValueRef key_val, Type* key_type)` + `LLVMValueRef codegen_map_slot_to_key(CodeGenerator*, LLVMValueRef slot, Type* key_type)` — used at every map op site + range.

- [ ] **Step 1: Two key helpers in codegen.c (+ decls in codegen.h)**

```c
// Map key kind for goo_map_new_sv: STRING(0) for string keys, INLINE(1) for
// integer/uint/bool/rune/byte/pointer. Matches runtime.h's enum.
int codegen_map_key_kind(Type* key_type) {
    return (key_type && key_type->kind == TYPE_STRING) ? 0 /*GOO_MAPKEY_STRING*/ : 1 /*INLINE*/;
}

// Pack a key value into the i64 slot. STRING: extract the char* and PtrToInt
// (NOT codegen_map_value_to_slot — that heap-boxes strings, which would break
// strcmp identity). INLINE: reuse the value-slot packer (its inline arm is
// exactly PtrToInt/ZExt/SExt/bitcast).
LLVMValueRef codegen_map_key_to_slot(CodeGenerator* codegen, TypeChecker* checker,
                                     LLVMValueRef key_val, Type* key_type) {
    (void)checker;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(codegen->context);
    if (key_type && key_type->kind == TYPE_STRING) {
        // string aggregate {i8*, i64}: take field 0 (the char*), int-ize it.
        LLVMValueRef cptr = LLVMBuildExtractValue(codegen->builder, key_val, 0, "kstr_ptr");
        return LLVMBuildPtrToInt(codegen->builder, cptr, i64, "kstr_slot");
    }
    return codegen_map_value_to_slot(codegen, key_val, key_type);
}

// Inverse for range key binding. STRING: i64→char*→rebuild {i8*, i64}? No — for
// range we only need the char* wrapped as a goo string. INLINE: reuse the value
// unpacker.
LLVMValueRef codegen_map_slot_to_key(CodeGenerator* codegen, LLVMValueRef slot, Type* key_type) {
    if (key_type && key_type->kind == TYPE_STRING) {
        // slot holds a char*; wrap into the goo string aggregate the same way
        // range's existing key handling built its string key (see the string
        // key construction in statement_codegen.c range before this change).
        LLVMTypeRef i8p = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
        LLVMValueRef cptr = LLVMBuildIntToPtr(codegen->builder, slot, i8p, "kstr_ptr");
        return codegen_string_from_cstr(codegen, cptr); // use the existing cstr->goo-string helper; if none, build {cptr, strlen} inline
    }
    return codegen_map_slot_to_value(codegen, slot, key_type);
}
```

NOTE: verify `codegen_string_from_cstr` exists (grep it); if the range code currently builds its string key differently, mirror THAT construction here. If range only ever exposes the raw char*, keep it minimal.

Declare the three in `include/codegen.h` next to the value-slot decls (434-436). `make clean && make lexer`.

- [ ] **Step 2: Extern signatures (runtime_integration.c:339+)**

Update the `add_runtime_function` decls to the new shapes: `goo_map_new_sv` takes one `i32` (key_kind) → ptr; `goo_map_set_sv` (ptr, i64, i64)→void; `goo_map_get_sv` (ptr, i64)→i64; `goo_map_get_sv_ok` (ptr, i64, i64*, i32*)→void; `goo_map_delete_sv` (ptr, i64)→void; `goo_map_iter_next_sv` (ptr*, i64*, i64*)→i32. Match the exact param-type builders already used there.

- [ ] **Step 3: Creation sites pass key_kind**

Map literal (expression_codegen.c:234) and `make` (call_codegen.c:505): the `goo_map_new_sv` call now passes `LLVMConstInt(i32, codegen_map_key_kind(K), 0)` where `K` is the map's key type (from the map type node). Both sites already know the map type.

- [ ] **Step 4: Key sites pack via `codegen_map_key_to_slot`**

At map literal set (expression_codegen.c:235), `m[k]=v` (expression_codegen.c:1112), `m[k]` read (composite_codegen.c:85), comma-ok (function_codegen.c:1617 — the existing string-only `ExtractValue 0`), delete (call_codegen.c:604): REPLACE the current string-key extraction with `codegen_map_key_to_slot(codegen, checker, key_val, key_type)` and pass the i64 slot. `key_type` is the map's key type (from the indexed map's `node_type->data.map.key_type`).

- [ ] **Step 5: Range key (statement_codegen.c:833-919)**

Remove the string-only guard at :863 ("only string-keyed maps are supported"). The iter now yields an i64 key slot; bind the range key var via `codegen_map_slot_to_key(codegen, slot, key_type)` at the key's type. Thread the map's key type (from the ranged map's `node_type`).

- [ ] **Step 6: Build + goldens (still gated on Task 3 for non-string, but string must be green)**

```bash
make clean && make lexer                     # exit 0
bash scripts/run_golden.sh                   # 232/0 — the STRING path must be fully restored
make test                                    # 76/1
```
Non-string maps still won't COMPILE (typecheck gate rejects them until Task 3) — that's fine. The gate here is: string maps regression-clean at 232/0.

- [ ] **Step 7: Commit**

```bash
git add src/codegen/codegen.c include/codegen.h src/codegen/runtime_integration.c \
        src/codegen/expression_codegen.c src/codegen/composite_codegen.c \
        src/codegen/function_codegen.c src/codegen/call_codegen.c src/codegen/statement_codegen.c
git commit --no-gpg-sign -m "feat(codegen): map key→slot, key_kind at creation, non-string range key"
```

---

### Task 3: Typecheck — lift the gate + comparability check + reject probes

**Files:**
- Modify: `src/types/type_checker.c` (2690 gate + key-type threading at index/delete/range)
- Modify: `Makefile` (reject probes + `verify:`)

**Interfaces:**
- Consumes: Tasks 1-2 (runtime + codegen accept non-string keys).
- Produces: non-string map types typecheck; wrong-typed index rejects; deferred/non-comparable key types reject with distinct messages.

- [ ] **Step 1: Replace the string-only gate (type_checker.c:2690)**

Replace the `"map key type must be string, got %s"` error with a comparability check. Accept `K` if its kind is in {TYPE_STRING, integer/uint widths (TYPE_INT8..TYPE_INT64, TYPE_UINT8..TYPE_UINT64), TYPE_BOOL, TYPE_CHAR/rune, byte, TYPE_POINTER}. (Use the same kind predicates the existing `codegen_map_value_is_inline` / numeric helpers use — match the codebase's TypeKind names.) Otherwise:

```c
// deferred-comparable: struct, float, interface, array
if (key_type->kind == TYPE_STRUCT || key_type->kind == TYPE_FLOAT32 ||
    key_type->kind == TYPE_FLOAT64 || key_type->kind == TYPE_INTERFACE ||
    key_type->kind == TYPE_ARRAY) {
    type_error(checker, pos,
        "map key type %s is not yet supported in v1 (comparable key types so far: "
        "string, integers, bool, rune, byte, pointers)", type_to_string(key_type));
    return NULL;
}
// non-comparable: slice, map, func (Go-permanent)
type_error(checker, pos,
    "invalid map key type %s (not comparable)", type_to_string(key_type));
return NULL;
```

(Adapt TypeKind spellings to the actual enum. If float is one TYPE_FLOAT kind, collapse.)

- [ ] **Step 2: Key-type threading at index / delete / range**

Ensure the index expression `m[k]` type-checks the key operand `k` against the map's key type K (a `string` index on `map[int]T` must reject with a key-type mismatch — Go: "cannot use ... as int value"). Same for `delete(m, k)` second arg and the range key binding type. Find the existing index/delete/range checks that assumed string and generalize them to compare against K.

- [ ] **Step 3: Reject probes (Makefile)**

Add `mapkey-reject-probe` (compile-must-fail + message grep), mirroring existing `*-reject-probe`, bundling: (a) `map[float64]int{}` → "not yet supported in v1"; (b) `map[[]int]int{}` → "not comparable"; (c) wrong-typed index — `m := map[int]int{}; _ = m["x"]` → key-type mismatch. Add to `verify:`.

- [ ] **Step 4: Build + spot-check + commit**

```bash
make clean && make lexer                     # exit 0
# non-string maps now COMPILE — spot check one:
printf 'package main\nimport "fmt"\nfunc main(){\n\tm := map[int]string{1:"a",2:"b"}\n\tfmt.Println(m[2])\n}\n' > build/mk.goo
bin/goo -o build/mk build/mk.goo && ./build/mk      # prints b
make mapkey-reject-probe                     # PASS
bash scripts/run_golden.sh                   # 232/0 (no new goldens yet — Task 4)
make test                                    # 76/1
```

```bash
git add src/types/type_checker.c Makefile
git commit --no-gpg-sign -m "feat(types): non-string map keys — comparability gate + key-type checks"
```

---

### Task 4: Goldens sweep + full sweep + handoff

**Files:**
- Create: `examples/map_int_key_probe.goo` + `.expected.txt`, `examples/map_char_key_probe.goo` + `.expected.txt`, `examples/map_ptr_key_probe.goo` + `.expected.txt`, `examples/map_key_range_probe.goo` + `.expected.txt`
- Modify: `.handoff.md`

**Interfaces:** consumes Tasks 1-3; produces the branch ship gates.

- [ ] **Step 1: Goldens (go-run-verify each)**

`examples/map_int_key_probe.goo` — int keys, full surface:

```go
package main

import "fmt"

func main() {
	m := map[int]string{1: "one", 2: "two"}
	m[3] = "three"
	m[2] = "TWO"
	fmt.Println(len(m))
	fmt.Println(m[2])
	fmt.Println(m[3])
	v, ok := m[9]
	fmt.Println(ok)
	fmt.Println(v)
	delete(m, 1)
	fmt.Println(len(m))
	fmt.Println(m[0])
}
```
`.expected.txt` (go run): `3` / `TWO` / `three` / `false` / (empty line) / `2` / (empty line).
(NOTE: `m[9]`/`m[0]` miss → zero value of string = "" → blank lines; verify exact output with go run.)

`examples/map_char_key_probe.goo` — rune + byte keys (dispatch-table shape):

```go
package main

import "fmt"

func main() {
	freq := map[rune]int{}
	for _, r := range "hello" {
		freq[r] = freq[r] + 1
	}
	fmt.Println(freq['l'])
	fmt.Println(freq['h'])
	fmt.Println(freq['z'])
}
```
`.expected.txt`: `2` / `1` / `0`.

`examples/map_ptr_key_probe.goo` — pointer keys (identity):

```go
package main

import "fmt"

type Node struct{ V int }

func main() {
	a := &Node{V: 1}
	b := &Node{V: 2}
	m := map[*Node]string{}
	m[a] = "a"
	m[b] = "b"
	fmt.Println(m[a])
	fmt.Println(m[b])
	fmt.Println(len(m))
	m[a] = "aa"
	fmt.Println(m[a])
	fmt.Println(len(m))
}
```
`.expected.txt`: `a` / `b` / `2` / `aa` / `2`.

`examples/map_key_range_probe.goo` — range yielding non-string keys (sum keys for order-independence):

```go
package main

import "fmt"

func main() {
	m := map[int]int{10: 1, 20: 2, 30: 3}
	sumK := 0
	sumV := 0
	for k, v := range m {
		sumK = sumK + k
		sumV = sumV + v
	}
	fmt.Println(sumK)
	fmt.Println(sumV)
}
```
`.expected.txt`: `60` / `6`.

- [ ] **Step 2: Full sweep (real exit codes)**

```bash
make clean && make lexer                     # exit 0
make test                                    # 76/1
bash scripts/run_golden.sh                   # 236/0 (232 + 4 new)
eval "$(opam env --switch=default)"
make verify                                  # ALL GREEN GATES PASSED
make ccomp-link                              # PASS
./scripts/grammar-tripwire.sh                # PASS 82/256 (no grammar change)
```

- [ ] **Step 3: Handoff**

Update `.handoff.md`: non-string map keys SHIPPED (this branch): inline keys (int/uint widths, bool, rune, byte, pointer) + string, via int64 key slot + key_kind (linear-scan eq, no hash). RECORD: struct/float/interface/array keys deferred (boxed-key eq — future cycle); slices/maps/funcs permanently rejected; map iteration order still deterministic-reverse-insertion (pre-existing deviation). Promote next queue head (papercut batch, or per the queue).

- [ ] **Step 4: Commit**

```bash
git add examples/map_*_probe.* .handoff.md
git commit --no-gpg-sign -m "test(maps): non-string key goldens (int/char/ptr/range); update handoff"
```

After Task 4: push, PR, fresh-context whole-branch review before merge (mandatory).

---

## Execution notes (controller)

- SDD economy mode: Sonnet implementers, controller (main loop) review + independent differential probes between tasks.
- T1 changes the runtime ABI (signatures) — codegen is stale until T2, so T1's gate is "runtime compiles + make test," NOT the golden suite. T2's gate restores string maps to 232/0. Don't mistake the intermediate red for a defect.
- Reviewers: T1's reviewer should confirm the `!k` null-guard removal is correct (int key 0 is valid) and the string-ownership note still holds; T2's should confirm string keys do NOT route through the boxing `codegen_map_value_to_slot` (the spec's key subtlety); T3's should confirm the two-reason reject messages are accurate (non-comparable vs deferred).
- No grammar changes anywhere: any parser.y touch is an automatic BLOCKED.
