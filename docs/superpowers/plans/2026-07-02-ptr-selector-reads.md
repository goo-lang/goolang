# Pointer-Selector Read Miscompiles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix two pre-existing miscompiles surfaced by PR #90's review: (A) chained selector reads through a pointer-typed struct field (`o.P.V`) return garbage; (B) a bare field selector as an `if`/`for` condition or unary operand emits its address instead of its value (LLVM verify failure `Branch condition is not 'i1'`).

**Architecture:** Two independent root causes, one task each. (A) `codegen_generate_selector_expr`'s pointer-to-struct branch has inverted lvalue logic and loads the wrong type — fix it to load the POINTER from the slot when the base is an lvalue, and to use the pointer value directly (it already IS the struct's address) when it's an rvalue; the write path (`codegen_emit_lvalue_address`, expression_codegen.c:363-373) already does this correctly and is the reference. (B) `if`-condition, `for`-condition, and unary-operand codegen must auto-load lvalue ValueInfos the way binary-op codegen already does (expression_codegen.c:968-971 is the reference idiom).

**Tech Stack:** C23, LLVM-C API (LLVM 22). No runtime C, grammar, or header changes.

**Root-cause evidence (2026-07-02 debugging session):** `br ptr %A` / `%A = getelementptr %T, ptr %struct_tmp` verifier dumps; `o.P.V` → 380743696 while `tmp := o.P; tmp.V` → 7; `for p.A` fails identically to `if p.A`; writes (`p.N = 42`), assignments (`x := p.A`), and comparisons (`for p.N > 40`) all already work.

## Global Constraints

- Branch: `fix/ptr-selector-reads` (already created off main @d126d63 — do NOT commit on main).
- Commits: conventional style, imperative mood, `--no-gpg-sign`. Stage only named files; never stage anything under `.superpowers/`.
- No header edits expected; if any, `make clean` first (Makefile has no header deps).
- Gate per task: `make lexer`, run the task's probe, then `eval "$(opam env --switch=default)"` STANDALONE followed by `make verify` (all PASS, golden failures stay 0; baseline 163/0 grows with each probe) and `make test` (76 pass / 1 pre-existing skip is green).
- KNOWN LATENT BUG (do not trip it): 3+ sequential `fmt.Println` of different-width ints corrupts the middle arg. Print `int` values (and bools, which print as true/false) only.
- Multi-name params/decls (`var a, b int`) don't parse — one declaration per name.

## Reference: verified code landmarks (2026-07-02)

- Selector READ path: `codegen_generate_selector_expr`, `src/codegen/composite_codegen.c:330-428`. The broken pointer branch is L362-373; the GEP fork on `base_val->is_lvalue` is L404-419. The temp-copy path (alloca `struct_tmp`) at L410-419 must REMAIN for genuine struct-VALUE rvalue bases (e.g. a function returning a struct).
- Selector WRITE path (correct reference): `codegen_emit_lvalue_address`, `src/codegen/expression_codegen.c:348-397` — note L367-373: pointer base → `LLVMBuildLoad2(builder, LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0), base->llvm_value, "struct_ptr")`.
- Auto-load idiom (correct reference): binary-op operand loading, `src/codegen/expression_codegen.c:966-995`.
- If-condition: `src/codegen/statement_codegen.c:442-455` (`condition->llvm_value` used raw at L454).
- For-condition: `src/codegen/statement_codegen.c:794-803` (raw at L802).
- Unary operand: `src/codegen/expression_codegen.c:1319-1331` (`operand_llvm = operand->llvm_value` raw; the `TOKEN_BIT_AND` case must keep receiving the UNLOADED operand — it handles lvalues itself via `codegen_emit_lvalue_address`, and the struct-literal case loads explicitly).
- `codegen_generate_expression` auto-loads identifiers (returns rvalues) but selector/index results are lvalues (address + `is_lvalue=1`) — consumers own the load.

---

### Task 1: Fix the selector read path's pointer-to-struct branch

**Files:**
- Modify: `src/codegen/composite_codegen.c:362-373` (pointer branch in `codegen_generate_selector_expr`)
- Test: `examples/ptr_selector_probe.goo` + `examples/ptr_selector_probe.expected.txt`

**Interfaces:**
- Consumes: existing ValueInfo contract (`is_lvalue` ⇒ `llvm_value` is an address).
- Produces: `o.P.V` chained reads correct; single-level `p.V` reads GEP the real struct (no temp copy) — Task 2's probe relies on selector reads being correct.

- [ ] **Step 1: Write the failing probe**

`examples/ptr_selector_probe.goo`:
```go
package main

import "fmt"

type Inner struct {
	V int
	W int
}

type Outer struct {
	P *Inner
	N int
}

func main() {
	q := &Inner{V: 7, W: 11}
	o := Outer{P: q, N: 1}
	fmt.Println(o.P.V)
	fmt.Println(o.P.W)
	tmp := o.P
	fmt.Println(tmp.V)
	p := &Inner{V: 20, W: 22}
	fmt.Println(p.V)
	p.V = 30
	fmt.Println(p.V)
	oo := &Outer{P: p, N: 2}
	fmt.Println(oo.P.V)
	fmt.Println(oo.N)
}
```

`examples/ptr_selector_probe.expected.txt`:
```
7
11
7
20
30
30
2
```

Coverage: chained read through a value-struct base (`o.P.V`), second field (GEP index ≠ 0), two-step equivalence, single-level pointer read, read-after-write through the same pointer (catches any stale-temp-copy regression), chain through a pointer base (`oo.P.V` — pointer → pointer), non-pointer field of a pointer base.

- [ ] **Step 2: Verify it fails today**

Run: `bin/goo -o build/ptr_selector_probe examples/ptr_selector_probe.goo && ./build/ptr_selector_probe`
Expected: compiles, but the `o.P.V` / `o.P.W` / `oo.P.V` lines print garbage (non-deterministic values) instead of 7 / 11 / 30.

- [ ] **Step 3: Fix the pointer branch**

In `src/codegen/composite_codegen.c`, replace L362-373 (the `// Handle pointer to struct` block):

```c
    // Handle pointer to struct: resolve to the struct's address, then GEP.
    // Two shapes arrive here:
    //   - lvalue: llvm_value is the address of the POINTER SLOT (a chained
    //     selector like o.P, or any pointer field) — load the pointer out
    //     of the slot; the loaded value is the struct's address.
    //   - rvalue: llvm_value IS the pointer (an auto-loaded identifier like
    //     p) — it is already the struct's address; no load.
    // Either way the result is the struct's address, so mark it lvalue and
    // let the GEP fork below index the real struct storage directly. (The
    // old code had this inverted — it skipped the load for lvalues, GEPing
    // into the pointer slot itself and reading garbage, and for rvalues
    // loaded the whole struct and spilled it to a temp copy.)
    if (base_type->kind == TYPE_POINTER && base_type->data.pointer.pointee_type->kind == TYPE_STRUCT) {
        if (base_val->is_lvalue) {
            base_val->llvm_value = LLVMBuildLoad2(codegen->builder,
                                                  LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),
                                                  base_val->llvm_value, "struct_ptr");
        }
        base_type = base_type->data.pointer.pointee_type;
        base_val->is_lvalue = 1;
    }
```

Everything below (the `TYPE_STRUCT` check, field lookup, and the L404-419 GEP fork) is unchanged — after this fix a pointer base always takes the `is_lvalue` GEP branch with the struct's real address, and the temp-copy branch remains for genuine struct-value rvalues (e.g. selecting on a struct returned by value).

- [ ] **Step 4: Rebuild and verify the probe passes**

Run: `make lexer`, then `bin/goo -o build/ptr_selector_probe examples/ptr_selector_probe.goo && ./build/ptr_selector_probe`
Expected: `7 11 7 20 30 30 2`, one per line.

- [ ] **Step 5: Run the gate**

Run: `eval "$(opam env --switch=default)"` (standalone), then `make verify` → all PASS, golden 164/0. Then `make test` → 76/1. Methods and pointer-receiver golden probes (`methods-probe`, `ptr-recv-nonaddr-probe`, `addr_struct_lit_probe`) exercise the old temp-copy path — they must stay green.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/composite_codegen.c examples/ptr_selector_probe.goo examples/ptr_selector_probe.expected.txt
git commit --no-gpg-sign -m "fix(codegen): selector reads through pointer fields read the real struct

The pointer-to-struct branch in codegen_generate_selector_expr had its
lvalue logic inverted: a chained selector base (o.P — address of the
pointer slot) was never loaded, so the field GEP indexed the stack slot
itself and read garbage; an rvalue base (auto-loaded p) loaded the whole
struct and spilled it to a temp copy, correct only by accident. Load the
pointer from the slot when the base is an lvalue, use the pointer value
directly otherwise — either way GEP the real struct storage, matching
what the write path (codegen_emit_lvalue_address) already did."
```

---

### Task 2: Auto-load lvalue conditions and unary operands

**Files:**
- Modify: `src/codegen/statement_codegen.c:442-455` (if condition) and `:794-803` (for condition)
- Modify: `src/codegen/expression_codegen.c:1319-1331` (unary operand)
- Test: `examples/lvalue_cond_probe.goo` + `examples/lvalue_cond_probe.expected.txt`

**Interfaces:**
- Consumes: Task 1 (selector reads through pointers must be correct for the probe's `p.A` to hold the right value); `codegen_type_to_llvm`.
- Produces: `if <lvalue>`, `for <lvalue>`, and `<unary-op> <lvalue>` load the value first. The `&` operator still receives the unloaded operand.

- [ ] **Step 1: Write the failing probe**

`examples/lvalue_cond_probe.goo`:
```go
package main

import "fmt"

type T struct {
	A bool
	N int
}

func main() {
	p := &T{A: true, N: 3}
	if p.A {
		fmt.Println(1)
	} else {
		fmt.Println(0)
	}
	v := T{A: false, N: 5}
	if v.A {
		fmt.Println(1)
	} else {
		fmt.Println(0)
	}
	for p.A {
		fmt.Println(2)
		p.A = false
	}
	if !p.A {
		fmt.Println(3)
	}
	fmt.Println(-p.N)
	fmt.Println(^v.N)
}
```

`examples/lvalue_cond_probe.expected.txt`:
```
1
0
2
3
-3
-6
```

Coverage: bare selector condition through a pointer (`if p.A`) AND through a value struct (`if v.A` — the bug is not pointer-specific), `for` with a bare selector condition that flips mid-loop, unary NOT / negation / complement on selector operands (`^5` = -6 in Go's two's complement).

- [ ] **Step 2: Verify it fails today**

Run: `bin/goo -o build/lvalue_cond_probe examples/lvalue_cond_probe.goo 2>&1 | head -4`
Expected: `Module verification failed: Branch condition is not 'i1' type!` with `br ptr %A` over a GEP — the condition is the field's address.

- [ ] **Step 3: Auto-load the if condition**

In `src/codegen/statement_codegen.c`, the if-statement codegen currently reads (L442-455 area):

```c
    // Generate condition
    ValueInfo* condition = codegen_generate_expression(codegen, checker, if_stmt->condition);
    if (!condition) {
        ...
    }
    ...
    // Generate conditional branch
    LLVMValueRef cond_val = condition->llvm_value;
    value_info_free(condition);
```

Insert an auto-load between generation and use, mirroring the binary-op idiom (expression_codegen.c:966):

```c
    // Auto-load if the condition is an lvalue (e.g. a bare field selector
    // like `if p.A` — the selector returns the field's ADDRESS; branching
    // on it is a verifier error, not a bool test).
    LLVMValueRef cond_val = condition->llvm_value;
    if (condition->is_lvalue && condition->goo_type) {
        LLVMTypeRef ct = codegen_type_to_llvm(codegen, condition->goo_type);
        if (ct) cond_val = LLVMBuildLoad2(codegen->builder, ct, cond_val, "cond_load");
    }
    value_info_free(condition);
```

(Adapt variable names to the exact surrounding code — read the function first; the existing `cond_val` assignment is replaced by this block.)

- [ ] **Step 4: Auto-load the for condition**

Same idiom in `src/codegen/statement_codegen.c` L794-803: between `codegen_generate_expression(... for_stmt->condition)` and `LLVMBuildCondBr(...)`, load `condition->llvm_value` into a local `cond_val` if `condition->is_lvalue`, and pass `cond_val` to `LLVMBuildCondBr`. Comment: `// Auto-load lvalue conditions (bare field selectors) — same as the if path.`

- [ ] **Step 5: Auto-load the unary operand (except &)**

In `src/codegen/expression_codegen.c` `codegen_generate_unary_expr` (L1319-1331): after `operand` is generated and `result_type` resolved, replace `LLVMValueRef operand_llvm = operand->llvm_value;` with:

```c
    LLVMValueRef operand_llvm = operand->llvm_value;
    // Auto-load an lvalue operand (bare field selector / index result) for
    // value-consuming operators: -x, !x, ^x, *x all need the VALUE. The
    // address-of case is excluded — TOKEN_BIT_AND works on the unloaded
    // operand (it resolves storage itself via codegen_emit_lvalue_address,
    // and its struct-literal case loads explicitly).
    if (unary->operator != TOKEN_BIT_AND && operand->is_lvalue && operand->goo_type) {
        LLVMTypeRef ot = codegen_type_to_llvm(codegen, operand->goo_type);
        if (ot) operand_llvm = LLVMBuildLoad2(codegen->builder, ot, operand_llvm, "unary_load");
    }
```

Do NOT modify the switch cases themselves; `TOKEN_MULTIPLY` (deref) then operates on the loaded pointer value, which is correct (`*o.P` derefs the pointer stored in the field).

- [ ] **Step 6: Rebuild and verify the probe passes**

Run: `make lexer`, then `bin/goo -o build/lvalue_cond_probe examples/lvalue_cond_probe.goo && ./build/lvalue_cond_probe`
Expected: `1 0 2 3 -3 -6`, one per line.

- [ ] **Step 7: Run the gate**

Run: `eval "$(opam env --switch=default)"` (standalone), then `make verify` → all PASS, golden 165/0. Then `make test` → 76/1. The `&T{}` probe (`addr_struct_lit_probe`) exercises the TOKEN_BIT_AND exclusion — it must stay green.

- [ ] **Step 8: Commit**

```bash
git add src/codegen/statement_codegen.c src/codegen/expression_codegen.c examples/lvalue_cond_probe.goo examples/lvalue_cond_probe.expected.txt
git commit --no-gpg-sign -m "fix(codegen): load lvalue conditions and unary operands before use

A bare field selector returns the field's ADDRESS (is_lvalue) and every
consumer owns the load — binary ops and println do it, but if/for
conditions branched on the raw address ('Branch condition is not i1'
verifier failure) and unary ops operated on it. Auto-load at all three
sites, mirroring the binary-op idiom. TOKEN_BIT_AND keeps the unloaded
operand (it resolves storage itself)."
```

---

## Final gate (after both tasks)

- `make verify` → ALL GREEN (golden 165/0). `make test` → 76/1.
- ccomp (no runtime C changed — run anyway, separate commands):
```bash
eval "$(opam env --switch=default)"
make ccomp-link
```
Expected: PASS.

## Self-review notes

- Two root causes, two tasks, independently committable; Task 2's probe depends on Task A only for value correctness of `p.A` reads (the verifier failure reproduces regardless).
- Task 1 deliberately preserves the temp-copy path for struct-VALUE rvalue bases — only the pointer branch changes.
- Deliberate non-goals: `switch`/`match` tags (lower via comparisons, which auto-load), `&&`/`||` shortcut operands (binary path, already loads), postfix `++` (rejects selectors at parse today). A shared `codegen_rvalue()` helper was considered and rejected: the codebase's established idiom is the inline load (binary ops repeat it twice already); introducing a cross-file helper is a refactor for another day.
