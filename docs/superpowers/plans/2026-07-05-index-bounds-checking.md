# Index Bounds Checking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the slice/array index bounds-check hole — `s[i]=x`, `arr[i]=x`, and `arr[i]` (read) currently access past the buffer/array unchecked; make every slice/array element access uniformly bounds-checked.

**Architecture:** Promote the existing `codegen_emit_bounds_check` helper from `static` to shared (declared in `include/codegen.h`), then call it at the three uncovered element-address sites — mirroring the slice-read arm that already checks. Runtime check only (matches the read path); no typecheck/grammar change. Spec: `docs/superpowers/specs/2026-07-05-index-bounds-checking-design.md`.

**Tech Stack:** C23, LLVM-C API.

## Global Constraints

- Branch: `fix/index-bounds-checking` (exists, base main @ ba1d853). Commit `--no-gpg-sign`; pre-commit runs `make test`.
- **No grammar/typecheck change** — `./scripts/grammar-tripwire.sh` must stay PASS **82 S/R + 256 R/R** (a no-op sanity here; any delta means something unintended was touched). Runtime bounds only; constant-index compile-error for arrays is OUT of scope.
- **`include/codegen.h` is edited in Task 1** → that task MUST build with `make clean && make lexer` (the Makefile has no header→object deps; a plain `make lexer` leaves stale objects that silently miscompile). Tasks that touch only `.c` files use `make lexer`.
- **Array OOB probes MUST use a VARIABLE index** (`i := 5; arr[i]=x`). A constant OOB array index (`arr[5]` on `[3]int`) is a *compile* error in Go — not go-comparable and not the runtime path under test. Slice OOB probes may use a constant index (Go bounds-checks slices at runtime).
- In-bounds cases are go-run-verified goldens (`go run` produces the `.expected.txt`); OOB-abort cases are `Makefile` probes (compile-then-run, assert non-zero exit + "bounds check failed" message), NOT goldens.
- Baselines: golden **241/0** (`bash scripts/run_golden.sh`), `make test` **76/1**, bison **82/256**, `make verify` ALL GREEN, `make ccomp-link` PASS. ccomp needs `eval "$(opam env --switch=default)"` in the same shell. Real exit codes only (never a pipeline `$?`).
- The bounds-check panic message is `bounds check failed` (emitted by `goo_bounds_check`); probes grep for that string.

---

### Task 1: Slice-write bounds check + promote the shared helper

**Files:**
- Modify: `src/codegen/composite_codegen.c` (helper `static`→non-static, ~line 15)
- Modify: `include/codegen.h` (add prototype, ~line 308 next to `codegen_widen_index`)
- Modify: `src/codegen/expression_codegen.c` (`TYPE_SLICE` write arm, ~826-840)
- Modify: `Makefile` (`slice-write-bounds-probe` target + `verify:` list)
- Create: `examples/index_bounds_probe.goo` + `.expected.txt` (in-bounds golden, slice + array)

**Interfaces:**
- Consumes: `codegen_emit_bounds_check(codegen, index, length, expr)` (composite_codegen.c:15 — after this task, declared in codegen.h); `codegen_widen_index` (already shared); the write-path `TYPE_SLICE` arm's `slice_val`/`idx64`.
- Produces: a shared `void codegen_emit_bounds_check(CodeGenerator*, LLVMValueRef index, LLVMValueRef length, ASTNode* expr)` usable from any codegen `.c`; bounds-checked slice writes.

- [ ] **Step 1: Write the failing probe (slice write OOB must abort)**

Add to `Makefile` (near the existing `bounds-probe` target, ~line 1391):

```makefile
slice-write-bounds-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== slice-write-bounds-probe: s[5]=x and s[-1]=x on len-3 slice must abort ==="
	@printf 'package main\nfunc main(){ s:=[]int{1,2,3}; s[5]=9; _=s }\n' > build/swb_oob.goo
	@"$(COMPILER)" build/swb_oob.goo -o build/swb_oob.out 2>build/swb_oob.cerr || \
	  { echo "slice-write-bounds-probe: FAIL (compile)"; cat build/swb_oob.cerr; exit 1; }
	@./build/swb_oob.out 2>build/swb_oob.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "slice-write-bounds-probe: FAIL (OOB write did not abort, rc=0)"; exit 1; fi; \
	  if ! grep -qi "bounds check failed" build/swb_oob.err; then echo "slice-write-bounds-probe: FAIL (no bounds message)"; cat build/swb_oob.err; exit 1; fi
	@printf 'package main\nfunc main(){ s:=[]int{1,2,3}; i:=-1; s[i]=9; _=s }\n' > build/swb_neg.goo
	@"$(COMPILER)" build/swb_neg.goo -o build/swb_neg.out 2>build/swb_neg.cerr || \
	  { echo "slice-write-bounds-probe: FAIL (compile neg)"; cat build/swb_neg.cerr; exit 1; }
	@./build/swb_neg.out 2>build/swb_neg.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "slice-write-bounds-probe: FAIL (negative-index write did not abort, rc=0)"; exit 1; fi; \
	  if ! grep -qi "bounds check failed" build/swb_neg.err; then echo "slice-write-bounds-probe: FAIL (no bounds message on neg)"; cat build/swb_neg.err; exit 1; fi
	@echo "slice-write-bounds-probe: PASS"
```

Run: `make slice-write-bounds-probe`
Expected: **FAIL** — the OOB write currently does NOT abort (writes past the buffer; rc likely 0 or a raw SIGSEGV without the "bounds check failed" message). Confirm it fails BEFORE the fix.

- [ ] **Step 2: Promote the helper to shared**

In `src/codegen/composite_codegen.c` (~line 15), change:

```c
static void codegen_emit_bounds_check(CodeGenerator* codegen, LLVMValueRef index,
                                      LLVMValueRef length, ASTNode* expr) {
```

to (drop `static`):

```c
void codegen_emit_bounds_check(CodeGenerator* codegen, LLVMValueRef index,
                               LLVMValueRef length, ASTNode* expr) {
```

In `include/codegen.h` (~line 308, next to `LLVMValueRef codegen_widen_index(...)`), add:

```c
void codegen_emit_bounds_check(CodeGenerator* codegen, LLVMValueRef index,
                               LLVMValueRef length, ASTNode* expr);
```

(If `codegen.h` guards LLVM-typed prototypes behind `#if LLVM_AVAILABLE` / an `LLVMValueRef` typedef, place this prototype in the SAME guarded region as `codegen_widen_index` so the `LLVMValueRef` type is in scope — match whatever `codegen_widen_index`'s declaration does.)

- [ ] **Step 3: Add the bounds check to the slice-write arm**

In `src/codegen/expression_codegen.c`, the `TYPE_SLICE` arm of `codegen_emit_lvalue_address` (~826-840). Currently it loads `slice_val` and extracts only field 0 (`data_ptr`). Add the length extract + check immediately before the element GEP:

```c
        if (base_type->kind == TYPE_SLICE) {
            // base->llvm_value points to the slice struct { ptr, len, cap }; load
            // it, take the data pointer (field 0), and GEP into the backing buffer.
            Type* elem_type = base_type->data.slice.element_type;
            LLVMValueRef slice_val = LLVMBuildLoad2(codegen->builder,
                                                    codegen_type_to_llvm(codegen, base_type),
                                                    base->llvm_value, "slice_load");
            LLVMValueRef data_ptr = LLVMBuildExtractValue(codegen->builder, slice_val, 0, "slice_ptr");
            // Bounds-check the write against the slice length (field 1) before the
            // element GEP — mirrors the slice-READ arm in composite_codegen.c so
            // s[i]=x aborts on out-of-range instead of writing past the buffer.
            LLVMValueRef slice_len = LLVMBuildExtractValue(codegen->builder, slice_val, 1, "slice_len");
            codegen_emit_bounds_check(codegen, idx64, slice_len, expr);
            LLVMValueRef elem_ptr = LLVMBuildGEP2(codegen->builder,
                                                  codegen_type_to_llvm(codegen, elem_type),
                                                  data_ptr, &idx64, 1, "slice_elem");
            ValueInfo* out = value_info_new(NULL, elem_ptr, elem_type);
            out->is_lvalue = 1;
            return out;
        }
```

- [ ] **Step 4: Build (clean — header changed) and verify the probe passes**

```bash
make clean && make lexer          # exit 0 (clean REQUIRED: include/codegen.h changed)
make slice-write-bounds-probe     # slice-write-bounds-probe: PASS
```

- [ ] **Step 5: Add the in-bounds golden (slice + array, go-run-verified)**

`examples/index_bounds_probe.goo` (in-bounds writes/reads are unchanged by this fix — this pins that the bounds check does NOT break valid access; array in-bounds is included now because it already works and stays correct after Task 2):

```go
package main

import "fmt"

func main() {
	s := []int{1, 2, 3}
	s[0] = 10
	s[2] = 30
	fmt.Println(s[0])
	fmt.Println(s[1])
	fmt.Println(s[2])
	var arr [3]int
	arr[0] = 7
	i := 2
	arr[i] = 9
	fmt.Println(arr[0])
	fmt.Println(arr[i])
}
```

`examples/index_bounds_probe.expected.txt` (verify by running `go run` on an equivalent `.go` — must match byte-for-byte):

```
10
2
30
7
9
```

Add `slice-write-bounds-probe` to the `verify:` target's dependency list (in `Makefile`, alongside the existing `bounds-probe`). Then:

```bash
bash scripts/run_golden.sh        # 242 passed, 0 failed (was 241)
make test                         # 76 passed / 1 failed (pre-existing)
```

- [ ] **Step 6: Commit**

```bash
git add src/codegen/composite_codegen.c include/codegen.h src/codegen/expression_codegen.c Makefile examples/index_bounds_probe.*
git commit --no-gpg-sign -m "fix(codegen): bounds-check slice-index writes; share codegen_emit_bounds_check"
```

---

### Task 2: Array read + array write bounds checks

**Files:**
- Modify: `src/codegen/composite_codegen.c` (`TYPE_ARRAY` READ arm, ~110-135)
- Modify: `src/codegen/expression_codegen.c` (`TYPE_ARRAY` write arm, ~812-824)
- Modify: `Makefile` (`array-bounds-probe` target + `verify:` list)

**Interfaces:**
- Consumes: the shared `codegen_emit_bounds_check` (from Task 1); `base_type->data.array.length` (`size_t`, the fixed array's element count); `idx64` (already computed in both arms).
- Produces: bounds-checked array reads and writes.

- [ ] **Step 1: Write the failing probe (array read + write OOB must abort — VARIABLE index)**

Add to `Makefile` (near `slice-write-bounds-probe`):

```makefile
array-bounds-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== array-bounds-probe: arr[i]=x and _=arr[i] with i out of range must abort (variable index) ==="
	@printf 'package main\nfunc main(){ var arr [3]int; i:=5; arr[i]=9; _=arr }\n' > build/awb_oob.goo
	@"$(COMPILER)" build/awb_oob.goo -o build/awb_oob.out 2>build/awb_oob.cerr || \
	  { echo "array-bounds-probe: FAIL (compile write)"; cat build/awb_oob.cerr; exit 1; }
	@./build/awb_oob.out 2>build/awb_oob.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "array-bounds-probe: FAIL (OOB array write did not abort, rc=0)"; exit 1; fi; \
	  if ! grep -qi "bounds check failed" build/awb_oob.err; then echo "array-bounds-probe: FAIL (no bounds message on write)"; cat build/awb_oob.err; exit 1; fi
	@printf 'package main\nimport "fmt"\nfunc main(){ var arr [3]int; i:=5; fmt.Println(arr[i]) }\n' > build/arb_oob.goo
	@"$(COMPILER)" build/arb_oob.goo -o build/arb_oob.out 2>build/arb_oob.cerr || \
	  { echo "array-bounds-probe: FAIL (compile read)"; cat build/arb_oob.cerr; exit 1; }
	@./build/arb_oob.out 2>build/arb_oob.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "array-bounds-probe: FAIL (OOB array read did not abort, rc=0)"; exit 1; fi; \
	  if ! grep -qi "bounds check failed" build/arb_oob.err; then echo "array-bounds-probe: FAIL (no bounds message on read)"; cat build/arb_oob.err; exit 1; fi
	@echo "array-bounds-probe: PASS"
```

Run: `make array-bounds-probe`
Expected: **FAIL** — array read and write OOB currently do NOT abort. Confirm BEFORE the fix.

- [ ] **Step 2: Add the bounds check to the array-WRITE arm**

In `src/codegen/expression_codegen.c`, the `TYPE_ARRAY` arm of `codegen_emit_lvalue_address` (~812-824), before the element GEP:

```c
        if (base_type->kind == TYPE_ARRAY) {
            // base->llvm_value is a pointer to the array; GEP the element.
            // Bounds-check against the fixed length (static N) first — mirrors
            // the slice-write arm; arr[i]=x aborts on out-of-range.
            LLVMValueRef arr_len = LLVMConstInt(LLVMInt64TypeInContext(codegen->context),
                                                (unsigned long long)base_type->data.array.length, 0);
            codegen_emit_bounds_check(codegen, idx64, arr_len, expr);
            LLVMValueRef indices[] = {
                LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0),
                idx64
            };
            LLVMValueRef elem_ptr = LLVMBuildGEP2(codegen->builder,
                                                  codegen_type_to_llvm(codegen, base_type),
                                                  base->llvm_value, indices, 2, "array_elem");
            ValueInfo* out = value_info_new(NULL, elem_ptr, base_type->data.array.element_type);
            out->is_lvalue = 1;
            return out;
        }
```

- [ ] **Step 3: Add the bounds check to the array-READ arm**

In `src/codegen/composite_codegen.c`, the `TYPE_ARRAY` case of `codegen_generate_index_expr` (~110-135). `idx64` is already computed just above the `switch` (~line 106). Insert the check at the top of the case, before the GEP:

```c
        case TYPE_ARRAY: {
            element_type = base_type->data.array.element_type;

            // Bounds-check the read against the fixed length (static N) before
            // the element GEP — arrays previously skipped this (only slices
            // checked), so arr[i] could read past the array.
            LLVMValueRef arr_len = LLVMConstInt(LLVMInt64TypeInContext(codegen->context),
                                                (unsigned long long)base_type->data.array.length, 0);
            codegen_emit_bounds_check(codegen, idx64, arr_len, expr);

            // For arrays, generate GEP
            LLVMValueRef indices[] = {
                LLVMConstInt(LLVMInt32TypeInContext(codegen->context), 0, 0),  // Array base
                idx64  // Array index
            };
            /* ...existing is_lvalue / alloca GEP logic unchanged... */
```

(Leave the rest of the `TYPE_ARRAY` case — the `is_lvalue` vs alloca GEP branch — exactly as-is; only prepend the two lines above.)

- [ ] **Step 4: Build (no header change this task) and verify the probe passes**

```bash
make lexer                        # exit 0 (only .c files changed)
make array-bounds-probe           # array-bounds-probe: PASS
make slice-write-bounds-probe     # still PASS (regression check)
```

- [ ] **Step 5: Gates + regression**

Add `array-bounds-probe` to the `verify:` target's dependency list. Then:

```bash
bash scripts/run_golden.sh        # 242 passed, 0 failed (unchanged — no new golden; in-bounds arr access still correct)
make bounds-probe                 # PASS (existing slice-READ probe not weakened)
make test                         # 76 passed / 1 failed (pre-existing)
```

- [ ] **Step 6: Commit**

```bash
git add src/codegen/composite_codegen.c src/codegen/expression_codegen.c Makefile
git commit --no-gpg-sign -m "fix(codegen): bounds-check array-index reads and writes"
```

---

### Task 3: Full sweep + handoff

**Files:**
- Modify: `.handoff.md`

**Interfaces:** consumes T1-T2; produces the branch ship gates.

- [ ] **Step 1: Full sweep (real exit codes)**

```bash
make clean && make lexer          # exit 0
make test                         # 76/1
bash scripts/run_golden.sh        # 242/0
eval "$(opam env --switch=default)"
make verify                       # ALL GREEN GATES PASSED (incl. bounds-probe, slice-write-bounds-probe, array-bounds-probe)
make ccomp-link                   # PASS
./scripts/grammar-tripwire.sh     # PASS 82/256 (unchanged — no grammar touched)
```

- [ ] **Step 2: Update `.handoff.md`**

Prepend a new dated section: index bounds checking SHIPPED — slice writes, array writes, and array reads now bounds-check via the shared `codegen_emit_bounds_check` (slice reads already did). Note the queue effect: **NEXT QUEUE #2 (slice-index write bounds) is now RESOLVED**; also mark **#5 (`m[k]++`) as already RESOLVED by the papercut batch (PR #116)** so it is not re-attempted (prune-in-place, struck through per the "edit, don't rewrite history" rule). Record the out-of-scope follow-ups: constant-index compile-error for fixed arrays (typecheck), and slice-expression `s[low:high]` bounds (`codegen_generate_slice_index_expr` still deferred). Promote the next queue head (#3 funcval `== nil`, or #4b map-value legal-Go deviations — controller's call).

- [ ] **Step 3: Commit**

```bash
git add .handoff.md
git commit --no-gpg-sign -m "docs(handoff): index bounds checking shipped; queue updated"
```

After Task 3: push, PR, fresh-context whole-branch review before merge (mandatory — with deliberate degenerate-shape probes per the #114/#115 lesson: variable vs constant OOB index, negative index, in-bounds boundary `s[len-1]`/`s[len]`, array-in-struct and slice-of-slice element writes, `a[i]++` postfix inheriting the write check).

---

## Execution notes (controller)

- SDD economy mode: Sonnet implementers, controller (main loop) review + independent differential probes between tasks. Per the #114/#115 lesson, controller probes DELIBERATELY use boundary/degenerate shapes (exact `s[len]` off-by-one, `s[len-1]` in-bounds edge, negative, variable-vs-constant array index, `a[i]++` postfix, nested element writes), not just the happy path.
- T1 is the only task touching a header (`codegen.h`) — its reviewer confirms the `make clean` was honored and the prototype landed in the correct LLVM-guarded region.
- T2 reviewer confirms array length uses the static `data.array.length` (not a loaded header field) and the existing slice-READ `bounds-probe` is NOT weakened.
- Golden count: 241 → 242 (Task 1 adds one in-bounds golden; Task 2 adds none). Probes: +2 targets (slice-write-bounds-probe, array-bounds-probe), both wired into `verify:`.
- bison MUST stay 82/256 (no grammar touched) — any delta means an unintended edit; stop and investigate.
