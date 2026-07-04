# Generic Map Values Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Maps with any value type — closure dispatch tables (`map[string]func(int) int`), `map[string]string`, structs, floats, nested maps — per the approved spec `docs/superpowers/specs/2026-07-04-func-map-values-design.md`.

**Architecture:** The runtime's `int64_t` map slot and all seven `goo_map_*_sv` signatures stay untouched. A single codegen predicate classifies value types as inline (today's integer/bool/char/pointer casting, plus map/chan handles) or boxed (`goo_alloc` + store + `PtrToInt` in; guarded `slot==0 ? zero : load(IntToPtr)` out). All six operation sites already funnel through the two slot helpers, so widening the helpers carries literals, writes, reads, comma-ok, and range automatically. Typecheck's value gate opens; Go's map-value non-addressability is enforced with two guards.

**Tech Stack:** C23, LLVM-C API, existing golden-probe + reject-probe harness. NO parser changes.

## Global Constraints

- **Bison baseline: 81 shift/reduce + 256 reduce/reduce, exact.** This plan touches no grammar; verify once in Task 5 (`bison -d -o /tmp/p.tab.c src/parser/parser.y 2>&1 | grep conflicts`) to prove it.
- **No header edits are planned.** If a task ends up touching `include/*.h`, run `make clean` before rebuilding (no header deps in the Makefile).
- **Commits:** `git commit --no-gpg-sign`, conventional messages, imperative mood. Pre-commit hook runs `make test`.
- **Golden probes** auto-discover: `examples/<name>.goo` + `examples/<name>.expected.txt`, run via `bash scripts/run_golden.sh`. Baseline at branch start: **207 passed, 0 failed**.
- **Reject/abort probes** are Makefile targets appended to the `verify:` dependency list (pattern: `boolnot-reject-probe`; abort-at-runtime pattern: `funcnil-abort-probe` — find with `grep -n -A12 "^funcnil-abort-probe:" Makefile` and mirror its structure).
- **Go differential:** every golden in this plan is valid Go; verify each `.expected.txt` with `go run` (`cp examples/x.goo /tmp/x.go && go run /tmp/x.go`).
- **Box lifetime is LEAK by decision** — do not add `goo_free` calls anywhere in this plan.
- Branch: `feat/func-map-values` (already exists, spec committed on it).

---

### Task 1: Open the gate + boxing/unboxing arms (strings, floats prove it)

**Files:**
- Modify: `src/types/type_checker.c:2462-2472` (drop the value-type rejection)
- Modify: `src/codegen/codegen.c:523-546` (the two slot helpers + new predicate)
- Modify: `src/codegen/expression_codegen.c` write site (~:951) and literal site (~:119) — pre-slot coercion
- Test: `examples/map_string_probe.goo`, `examples/map_float_probe.goo` (+ `.expected.txt` each)

**Interfaces:**
- Consumes: `codegen_coerce_to_type(CodeGenerator*, LLVMValueRef v, int src_signed, LLVMTypeRef to)` (codegen.c:962); `goo_alloc` runtime decl (runtime_integration.c:88); `codegen_type_to_llvm`.
- Produces (Tasks 2–4 rely on these):
  - `int codegen_map_value_is_inline(Type* v)` in codegen.c (declared in codegen.h) — 1 for integer family / TYPE_BOOL / TYPE_CHAR / TYPE_POINTER / TYPE_MAP / TYPE_CHAN, else 0.
  - `codegen_map_value_to_slot` handles ANY value type (boxes when not inline).
  - `codegen_map_slot_to_value` handles ANY value type (guarded unbox; slot 0 → `LLVMConstNull(V)`).

- [ ] **Step 1: Write the failing goldens**

`examples/map_string_probe.goo` (verify expected with `go run`; note: iteration avoided — single reads only, so no order dependence):

```go
package main

import "fmt"

func main() {
	m := map[string]string{"a": "alpha", "b": "beta"}
	m["c"] = "gamma"
	m["a"] = "ALPHA"
	fmt.Println(m["a"])
	fmt.Println(m["c"])
	fmt.Println(len(m))
	fmt.Println(m["missing"])
	v, ok := m["b"]
	fmt.Println(v)
	fmt.Println(ok)
}
```

`examples/map_string_probe.expected.txt` (the `m["missing"]` line is EMPTY — zero-value string prints as ""):

```
ALPHA
gamma
3

beta
true
```

`examples/map_float_probe.goo`:

```go
package main

import "fmt"

func main() {
	m := map[string]float64{"pi": 3.25}
	m["e"] = 2.5
	m["i"] = 4 // untyped int constant must adapt to float64 before boxing
	fmt.Println(m["pi"] + m["e"])
	fmt.Println(m["i"])
	fmt.Println(m["nope"])
}
```

`examples/map_float_probe.expected.txt`:

```
5.75
4
0
```

(Check the exact float formatting against BOTH `go run` and Goo's existing float printing — if Goo prints `5.75` and go prints `5.75`, fine; if formats disagree on `4` vs `4.000000`, adjust the probe to values that format identically, e.g. use `.5` fractions, keeping go-identical output as the requirement.)

Run: `bash scripts/run_golden.sh 2>/dev/null | tail -1`
Expected: `207 passed, 2 failed` — both new probes fail to COMPILE with `map value type ... is not supported yet`.

- [ ] **Step 2: Open the typecheck gate**

In `src/types/type_checker.c`, `AST_MAP_TYPE` case, DELETE lines 2462–2472 (the value-slot comment and the value-type rejection `if`) entirely, leaving key check → `return type_map(key_type, value_type);`. Replace the deleted comment with:

```c
            // Any value type is accepted: inline scalars ride the 8-byte
            // runtime slot directly; everything else is heap-boxed by the
            // codegen slot helpers (spec 2026-07-04-func-map-values).
```

- [ ] **Step 3: Add the predicate and the boxing arm to value_to_slot**

In `src/codegen/codegen.c`, immediately above `codegen_map_value_to_slot` (:523), add:

```c
// Map slot classification (spec 2026-07-04-func-map-values): inline types
// ride the i64 slot as a cast; everything else (funcvals, strings, floats,
// structs, slices, interfaces, ...) is heap-boxed and the slot holds the
// box pointer. TYPE_MAP/TYPE_CHAN are opaque runtime pointers — inline.
// Non-static: the write/literal sites (expression_codegen.c) and the
// lvalue guard (Task 4) consult it too; declared in codegen.h.
int codegen_map_value_is_inline(Type* v) {
    if (!v) return 0;
    return type_is_integer(v) || v->kind == TYPE_BOOL || v->kind == TYPE_CHAR ||
           v->kind == TYPE_POINTER || v->kind == TYPE_MAP || v->kind == TYPE_CHAN;
}
```

And declare it in `include/codegen.h` next to the two helper declarations (:415-416):

```c
int codegen_map_value_is_inline(Type* value_type);
```

**Header edited → `make clean` before the Step 6 rebuild.**

In `codegen_map_value_to_slot`, replace the body after the NULL-check with:

```c
    LLVMTypeRef i64 = LLVMInt64TypeInContext(codegen->context);
    if (value_type->kind == TYPE_POINTER || value_type->kind == TYPE_MAP ||
        value_type->kind == TYPE_CHAN) {
        return LLVMBuildPtrToInt(codegen->builder, value, i64, "map_slot");
    }
    if (!codegen_map_value_is_inline(value_type)) {
        // Boxed value: goo_alloc(sizeof V), store, slot = box pointer.
        // ABI size via LLVMSizeOf — padding-correct (chan-padded lesson).
        // Boxes leak on overwrite/delete by decision (no GC yet; same as
        // closure envs and interface boxes).
        LLVMTypeRef vt = codegen_type_to_llvm(codegen, value_type);
        LLVMValueRef alloc_fn = LLVMGetNamedFunction(codegen->module, "goo_alloc");
        if (!vt || !alloc_fn) return NULL;
        LLVMValueRef size = LLVMSizeOf(vt);
        LLVMValueRef box = LLVMBuildCall2(codegen->builder,
                                          LLVMGlobalGetValueType(alloc_fn),
                                          alloc_fn, &size, 1, "map_box");
        LLVMBuildStore(codegen->builder, value, box);
        return LLVMBuildPtrToInt(codegen->builder, box, i64, "map_slot");
    }
    // Sign-extend a SIGNED integer when widening into the slot, so a negative
    // value narrower than the slot keeps its sign (e.g. the i32 literal -1
    // into a map[string]int64 must read back as -1, not 4294967295). Integer
    // literals are always emitted i32, so this widening is real. Unsigned,
    // bool, and char zero-extend. The read truncates back to V's width.
    LLVMBool is_signed = (value_type->kind >= TYPE_INT8 && value_type->kind <= TYPE_INT64);
    return LLVMBuildIntCast2(codegen->builder, value, i64, is_signed, "map_slot");
```

- [ ] **Step 4: Add the guarded unbox arm to slot_to_value**

Replace `codegen_map_slot_to_value`'s body after the NULL-check and `vt` build with:

```c
    if (value_type->kind == TYPE_POINTER || value_type->kind == TYPE_MAP ||
        value_type->kind == TYPE_CHAN) {
        return LLVMBuildIntToPtr(codegen->builder, slot, vt, "map_val");
    }
    if (!codegen_map_value_is_inline(value_type)) {
        // Boxed value with the ZERO GUARD: goo_map_get_sv returns 0 on a
        // missing key, and loading through slot 0 would be a null deref.
        // slot == 0  →  V's zero value (nil funcval / "" string / zero struct);
        // otherwise  →  load the value out of the box (copy semantics).
        LLVMTypeRef i64 = LLVMInt64TypeInContext(codegen->context);
        LLVMValueRef is_miss = LLVMBuildICmp(codegen->builder, LLVMIntEQ, slot,
                                             LLVMConstInt(i64, 0, 0), "map_miss");
        LLVMBasicBlockRef cur = LLVMGetInsertBlock(codegen->builder);
        LLVMValueRef fn = LLVMGetBasicBlockParent(cur);
        LLVMBasicBlockRef load_bb =
            LLVMAppendBasicBlockInContext(codegen->context, fn, "map_unbox");
        LLVMBasicBlockRef done_bb =
            LLVMAppendBasicBlockInContext(codegen->context, fn, "map_unbox_done");
        LLVMBuildCondBr(codegen->builder, is_miss, done_bb, load_bb);
        LLVMPositionBuilderAtEnd(codegen->builder, load_bb);
        LLVMValueRef box = LLVMBuildIntToPtr(
            codegen->builder, slot,
            LLVMPointerTypeInContext(codegen->context, 0), "map_box");
        LLVMValueRef loaded = LLVMBuildLoad2(codegen->builder, vt, box, "map_boxed_val");
        LLVMBuildBr(codegen->builder, done_bb);
        LLVMPositionBuilderAtEnd(codegen->builder, done_bb);
        LLVMValueRef phi = LLVMBuildPhi(codegen->builder, vt, "map_val");
        LLVMValueRef zero = LLVMConstNull(vt);
        LLVMValueRef inc_vals[2] = { zero, loaded };
        LLVMBasicBlockRef inc_bbs[2] = { cur, load_bb };
        LLVMAddIncoming(phi, inc_vals, inc_bbs, 2);
        return phi;
    }
    return LLVMBuildIntCast2(codegen->builder, slot, vt, /*isSigned=*/0, "map_val");
```

- [ ] **Step 5: Pre-slot coercion at the write and literal sites**

Untyped constants must adapt to V BEFORE boxing (`m["i"] = 4` into `map[string]float64`). At BOTH sites — `src/codegen/expression_codegen.c` write site (the line `LLVMValueRef slot = codegen_map_value_to_slot(codegen, vv->llvm_value, val_type);` at ~:951) and literal site (same call shape at ~:119) — insert immediately before that line:

```c
                LLVMTypeRef want_vt = codegen_type_to_llvm(codegen, val_type);
                if (want_vt && !codegen_map_value_is_inline(val_type)) {
                    int src_signed = vv->goo_type &&
                        vv->goo_type->kind >= TYPE_INT8 &&
                        vv->goo_type->kind <= TYPE_INT64;
                    vv->llvm_value = codegen_coerce_to_type(
                        codegen, vv->llvm_value, src_signed, want_vt);
                }
```

(The predicate and its codegen.h declaration were added in Step 3.)

- [ ] **Step 6: Clean rebuild, goldens green**

Run: `make clean && make lexer && make test`
Expected: green (76 pass / 1 skip).
Run: `bash scripts/run_golden.sh 2>/dev/null | tail -1`
Expected: `209 passed, 0 failed`.
If `map_string_probe` fails on the EMPTY line for `m["missing"]`: the print path can't take a `{null,0}` string. Fix at the print site (find with `grep -n "goo_print_string\|print.*string" src/codegen/call_codegen.c src/runtime/runtime.c | head`), guarding null `char*` → print empty — do NOT special-case the map path.

- [ ] **Step 7: Commit**

```bash
git add src/types/type_checker.c src/codegen/codegen.c src/codegen/expression_codegen.c include/codegen.h examples/map_string_probe.* examples/map_float_probe.*
git commit --no-gpg-sign -m "feat(maps): generic value types — slot boxing with zero-guard unbox"
```

---

### Task 2: Func-typed values — dispatch table + nil-panic + comma-ok/range

**Files:**
- Test: `examples/map_dispatch_probe.goo` + `.expected.txt`, `examples/map_boxed_range_probe.goo` + `.expected.txt`
- Modify: `Makefile` (`map-nilfunc-abort-probe` target + `verify:` list)

**Interfaces:**
- Consumes: Task 1's helpers (boxing covers TYPE_FUNCTION — the 16-byte `{fn, env}` pair stores/loads whole; env-FIRST contract untouched because the pair is never unpacked in map paths).
- Produces: golden coverage for the feature's headline; nothing new for later tasks.

- [ ] **Step 1: Write the dispatch-table golden**

`examples/map_dispatch_probe.goo` (verify with `go run`):

```go
package main

import "fmt"

func triple(x int) int {
	return x * 3
}

func main() {
	base := 10
	ops := map[string]func(int) int{
		"double": func(x int) int { return x * 2 },
		"offset": func(x int) int { return x + base }, // capturing closure in a map
	}
	ops["triple"] = triple // named func value
	fmt.Println(ops["double"](21))
	fmt.Println(ops["offset"](5))
	fmt.Println(ops["triple"](14))
	ops["double"] = func(x int) int { return x * 20 } // overwrite an entry
	fmt.Println(ops["double"](21))
	f, ok := ops["nope"]
	fmt.Println(ok)
	if f == nil {
		fmt.Println("nil entry")
	}
}
```

`examples/map_dispatch_probe.expected.txt`:

```
42
15
42
420
false
nil entry
```

(If `f == nil` comparison for func values doesn't compile — check with a scratch probe; funcval nil-compare may not have shipped — replace those last 4 lines of the probe with `_, ok := ops["nope"]` + `fmt.Println(ok)` and drop `nil entry` from expected; note the substitution in your report.)

- [ ] **Step 2: Write the nil-panic abort probe (Makefile target)**

Mirror `funcnil-abort-probe`'s structure (grep it first). Append to `Makefile` + add `map-nilfunc-abort-probe` to `verify:`:

```makefile
# Generic map values: calling a MISSING dispatch entry must hit the existing
# nil-func panic (zero-guard unbox yields {null,null}), not segfault.
map-nilfunc-abort-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== map-nilfunc-abort-probe: ops[missing]() must panic cleanly ==="
	@printf 'package main\nfunc main(){\n\tops := map[string]func() int{"a": func() int { return 1 }}\n\t_ = ops["missing"]()\n}\n' > build/map_nilfunc_abort.goo
	@$(COMPILER) -o build/map_nilfunc_abort build/map_nilfunc_abort.goo || (echo "map-nilfunc-abort-probe: FAIL (did not compile)"; exit 1)
	@./build/map_nilfunc_abort > build/map_nilfunc_abort.out 2> build/map_nilfunc_abort.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "map-nilfunc-abort-probe: FAIL (rc=0 — nil call did not abort)"; exit 1; fi; \
	if [ $$rc -ge 132 ] && [ $$rc -ne 134 ]; then echo "map-nilfunc-abort-probe: FAIL (rc=$$rc looks like a raw crash, not goo_panic)"; exit 1; fi; \
	if ! grep -q "nil function" build/map_nilfunc_abort.err; then echo "map-nilfunc-abort-probe: FAIL (missing panic message)"; cat build/map_nilfunc_abort.err; exit 1; fi; \
	echo "map-nilfunc-abort-probe: PASS (rc=$$rc, clean panic)"
```

(Adapt the rc/message checks to EXACTLY what `funcnil-abort-probe` does — same panic mechanism, same assertions. The message text is `call of nil function` per call_codegen.c:222-242.)

- [ ] **Step 3: Write the boxed range/comma-ok golden**

`examples/map_boxed_range_probe.goo` — single-entry map so iteration order can't matter:

```go
package main

import "fmt"

func main() {
	m := map[string]string{"only": "value"}
	for k, v := range m {
		fmt.Println(k)
		fmt.Println(v)
	}
	n := map[string]float64{"x": 1.5}
	total := 0.0
	for _, v := range n {
		total = total + v
	}
	fmt.Println(total)
}
```

`examples/map_boxed_range_probe.expected.txt`:

```
only
value
1.5
```

- [ ] **Step 4: Run everything**

Run: `bash scripts/run_golden.sh 2>/dev/null | tail -1`
Expected: `211 passed, 0 failed`.
Run: `make map-nilfunc-abort-probe`
Expected: PASS.
Run: `make test`
Expected: green.

These SHOULD pass with zero code changes (Task 1's arms are type-agnostic). If the dispatch probe fails, isolate which operation broke (literal vs write vs read vs call) with a minimal scratch probe in /tmp/claude-1000/-data-Workspace-github-com-goolang/1ed8be7c-3d34-4d33-a2b1-98824c7dc6a1/scratchpad/ and report BLOCKED with the evidence rather than patching blind.

- [ ] **Step 5: Commit**

```bash
git add examples/map_dispatch_probe.* examples/map_boxed_range_probe.* Makefile
git commit --no-gpg-sign -m "test(maps): dispatch-table goldens + missing-entry nil-panic probe"
```

---

### Task 3: Struct values (copy semantics) + nested maps

**Files:**
- Test: `examples/map_struct_probe.goo` + `.expected.txt`, `examples/map_nested_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: Task 1's helpers (structs box by ABI size; nested maps ride inline as opaque pointers).
- Produces: golden coverage; nothing new.

- [ ] **Step 1: Struct-values golden**

`examples/map_struct_probe.goo` (verify with `go run`):

```go
package main

import "fmt"

type P struct {
	X int
	Y int
}

func main() {
	p := P{X: 1, Y: 2}
	m := map[string]P{"a": p}
	p.X = 100 // mutate source AFTER insert — map's copy must be unaffected
	fmt.Println(m["a"].X)
	fmt.Println(m["a"].Y)
	q := m["a"] // read is a copy too
	q.Y = 200
	fmt.Println(m["a"].Y)
	z := m["missing"] // zero-value struct
	fmt.Println(z.X + z.Y)
}
```

`examples/map_struct_probe.expected.txt`:

```
1
2
2
0
```

- [ ] **Step 2: Nested-map golden**

`examples/map_nested_probe.goo` (note: Go returns the zero value for reads through a missing OUTER key — inner nil map reads are safe; Goo's `goo_map_get_sv` is NULL-safe the same way):

```go
package main

import "fmt"

func main() {
	m := map[string]map[string]int{
		"inner": {"a": 1},
	}
	m["inner"]["b"] = 2
	fmt.Println(m["inner"]["a"] + m["inner"]["b"])
	fmt.Println(m["ghost"]["x"]) // missing outer -> nil inner -> 0
}
```

`examples/map_nested_probe.expected.txt`:

```
3
0
```

(If the elided inner-literal form `{"a": 1}` doesn't parse — it requires elided composite support in map literal values — use the explicit form `"inner": map[string]int{"a": 1}` and verify THAT is also valid Go with `go run`. Note which form you used in the report.)

- [ ] **Step 3: Run + commit**

Run: `bash scripts/run_golden.sh 2>/dev/null | tail -1`
Expected: `213 passed, 0 failed`. (`make test` green.)
Expected to pass without code changes; same isolation rule as Task 2 Step 4 if not.

```bash
git add examples/map_struct_probe.* examples/map_nested_probe.*
git commit --no-gpg-sign -m "test(maps): struct copy-semantics + nested-map goldens"
```

---

### Task 4: Addressability guards (Go semantics)

**Files:**
- Modify: `src/types/expression_checker.c` (unary `&` typecheck, TOKEN_AND case ~:1786 with the literal checks at ~:1889-1912)
- Modify: `src/codegen/expression_codegen.c` (`codegen_emit_lvalue_address`, AST_INDEX_EXPR arm — find with `grep -n "AST_INDEX_EXPR" src/codegen/expression_codegen.c | head`)
- Modify: `Makefile` (`map-addr-reject-probe` target + `verify:` list)

**Interfaces:**
- Consumes: nothing new.
- Produces: diagnostic strings (exact, probe-pinned):
  - `"cannot take the address of a map value (map values are not addressable)"`
  - `"cannot assign through a map value (map values are not addressable; assign the whole value: m[k] = v)"`

- [ ] **Step 1: Write the failing reject probe**

Append to `Makefile` + `verify:` list:

```makefile
# Generic map values: Go's map values are NOT addressable. &m[k] and partial
# writes (m[k].F = v) must reject at compile time — without the guard the
# lvalue path would silently mutate a private box nobody reads back.
map-addr-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== map-addr-reject-probe: &m[k] must reject ==="
	@printf 'package main\nfunc main(){\n\tm := map[string]int{"a": 1}\n\tp := &m["a"]\n\t_ = p\n}\n' > build/map_addr_reject.goo
	@rm -f build/map_addr_reject
	@$(COMPILER) -o build/map_addr_reject build/map_addr_reject.goo > /dev/null 2> build/map_addr_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "map-addr-reject-probe: FAIL (&m[k] compiled)"; exit 1; fi; \
	if ! grep -q "cannot take the address of a map value" build/map_addr_reject.err; then echo "map-addr-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/map_addr_reject.err; exit 1; fi; \
	echo "map-addr-reject-probe: PASS"
	@echo "=== m[k].F = v must reject ==="
	@printf 'package main\ntype P struct { X int }\nfunc main(){\n\tm := map[string]P{"a": P{X: 1}}\n\tm["a"].X = 5\n}\n' > build/map_field_reject.goo
	@rm -f build/map_field_reject
	@$(COMPILER) -o build/map_field_reject build/map_field_reject.goo > /dev/null 2> build/map_field_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "map-addr-reject-probe: FAIL (m[k].F = v compiled)"; exit 1; fi; \
	if ! grep -q "cannot assign through a map value" build/map_field_reject.err; then echo "map-addr-reject-probe: FAIL (wrong/missing partial-write diagnostic)"; cat build/map_field_reject.err; exit 1; fi; \
	echo "map-addr-reject-probe: PASS (partial write rejected)"
```

Run: `make map-addr-reject-probe`
Expected: FAIL (one or both halves — today `&m[k]` and `m[k].X = 5` either compile or die with the wrong error). Record which in the report.

- [ ] **Step 2: Typecheck guard for `&m[k]`**

In `src/types/expression_checker.c`, in the unary `TOKEN_AND` (address-of) handling near the existing literal-address checks (~:1889), add BEFORE the general operand handling:

```c
            // Go semantics: map values are not addressable — &m[k] is illegal.
            // (The runtime slot may hold a heap box, but exposing it would
            // alias storage that overwrite replaces silently.)
            if (unary->operand && unary->operand->type == AST_INDEX_EXPR) {
                IndexExprNode* ix = (IndexExprNode*)unary->operand;
                Type* bt = type_check_expression(checker, ix->expr);
                if (bt && bt->kind == TYPE_MAP) {
                    type_error(checker, expr->pos,
                               "cannot take the address of a map value "
                               "(map values are not addressable)");
                    return NULL;
                }
            }
```

(Adapt the local variable names — `unary`, `operand` — to the actual node struct in that function; read the surrounding case first. The operand's checked type is re-derived cheaply; expressions are idempotent to re-check.)

- [ ] **Step 3: Codegen lvalue backstop for partial writes**

In `src/codegen/expression_codegen.c`, `codegen_emit_lvalue_address`, at the TOP of the `AST_INDEX_EXPR` arm (before any GEP logic), add:

```c
            // Go semantics: a map index is never an lvalue address. Direct
            // `m[k] = v` is intercepted earlier (assignment fast path); any
            // request that reaches HERE is a partial write through a map
            // value (m[k].F = v, m[k][i] = v) or an address-of that slipped
            // past typecheck — all illegal.
            {
                IndexExprNode* ix_node = (IndexExprNode*)expr;
                Type* base_goo = ix_node->expr ? ix_node->expr->node_type : NULL;
                if (base_goo && base_goo->kind == TYPE_MAP) {
                    codegen_error(codegen, expr->pos,
                                  "cannot assign through a map value (map values are "
                                  "not addressable; assign the whole value: m[k] = v)");
                    return NULL;
                }
            }
```

(Verify how that arm names its IndexExprNode and whether base type is available as `node_type` — if the arm re-derives the base type differently, follow its existing pattern. `node_type` is stamped by typecheck; assignments are typechecked before codegen.)

- [ ] **Step 4: Pin current inline compound behavior (informational probe)**

Check what works TODAY for inline values (spec: pin, don't build):

Run in scratch (/tmp/claude-1000/-data-Workspace-github-com-goolang/1ed8be7c-3d34-4d33-a2b1-98824c7dc6a1/scratchpad/):

```go
package main

import "fmt"

func main() {
	m := map[string]int{"a": 1}
	m["a"] = m["a"] + 1 // read-modify-write spelled out — must work
	fmt.Println(m["a"])
}
```

Expected: compiles, prints 2. Then try `m["a"]++` and `m["a"] += 1` variants in scratch. RECORD the findings in your report only — do not modify committed probes and do not implement anything: if `m[k]++` works it's pinned by the #108 postfix machinery; if it doesn't compile, it's a recorded gap.

- [ ] **Step 5: Rebuild, probe green, full regression**

Run: `make lexer && make map-addr-reject-probe`
Expected: PASS (both halves).
Run: `bash scripts/run_golden.sh 2>/dev/null | tail -1 && make test`
Expected: `213 passed, 0 failed`; tests green. The backstop must NOT break array/slice index assignment — goldens cover those heavily; any golden regression means the guard fired on a non-map base (check the `node_type` derivation).

- [ ] **Step 6: Commit**

```bash
git add src/types/expression_checker.c src/codegen/expression_codegen.c Makefile
git commit --no-gpg-sign -m "feat(maps): enforce Go map-value non-addressability — & and partial writes reject"
```

---

### Task 5: Overwrite pinning, sweep, handoff

**Files:**
- Test: `examples/map_overwrite_probe.goo` + `.expected.txt`
- Modify: `.handoff.md`

**Interfaces:** consumes everything; produces the shipped feature's final gates.

- [ ] **Step 1: Overwrite/len pinning golden**

`goo_map_set_sv` already updates in place (runtime.c:611 — verified during planning); this golden PINS that with boxed values (each overwrite allocates a fresh box; len must not grow):

`examples/map_overwrite_probe.goo` (verify with `go run`):

```go
package main

import "fmt"

func main() {
	m := map[string]string{}
	m["k"] = "one"
	m["k"] = "two"
	m["k"] = "three"
	fmt.Println(len(m))
	fmt.Println(m["k"])
	n := map[string]func() int{}
	n["f"] = func() int { return 1 }
	n["f"] = func() int { return 2 }
	fmt.Println(len(n))
	fmt.Println(n["f"]())
}
```

`examples/map_overwrite_probe.expected.txt`:

```
1
three
1
2
```

- [ ] **Step 2: Full verification sweep**

Run each to completion, reading REAL exit codes (no pipes on the command whose status matters):

```bash
make clean && make lexer
make test                                            # green
bash scripts/run_golden.sh 2>/dev/null | tail -1     # expect: 214 passed, 0 failed
make verify                                          # full chain incl. map-nilfunc-abort + map-addr-reject
make ccomp-link                                      # PASS
bison -d -o /tmp/p.tab.c src/parser/parser.y 2>&1 | grep conflicts   # 81 + 256 exactly (no parser changes)
```

- [ ] **Step 3: Update .handoff.md**

Mark generic map values SHIPPED (this branch): any value type; inline vs boxed rule; zero-guard semantics (missing dispatch entry → nil-func panic, missing string → ""); Go non-addressability enforced; boxes leak by decision (GC-era item alongside envs/iface boxes). NEXT QUEUE promotes: pointer-concrete interface boxing miscompile pair (from #109's review) and slice-index WRITE bounds checking. Keep the trailing-comma parse gap item.

- [ ] **Step 4: Commit**

```bash
git add examples/map_overwrite_probe.* .handoff.md
git commit --no-gpg-sign -m "test(maps): overwrite/len pinning; update handoff — generic map values shipped"
```

---

## Execution notes (controller)

- Branch `feat/func-map-values` already exists with the spec committed; base for the first task review package is that spec commit.
- Model split: Sonnet implementers, Fable controller review with independent probes between tasks (the pattern that caught the diamond-ambiguity class last cycle).
- Tasks 2 and 3 are expected test-only; they are still separate reviewer gates — a probe failure there means a Task 1 defect, handled as BLOCKED + evidence, not inline patching.
- After Task 5: push, PR, fresh-context whole-branch review before merge (last cycle's final review found a real pre-existing miscompile — keep the step).
