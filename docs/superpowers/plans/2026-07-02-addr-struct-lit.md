# &T{...} Struct-Literal Address Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `&Point{X: x, Y: y}` (the Go constructor idiom) compiles and runs — the literal gets leaked heap storage via `goo_alloc` and the expression yields `*Point`; non-struct composite literals get a clean type error.

**Architecture:** Codegen-only feature plus a typecheck guard. The checker already types `&<anything>` as `*T`; the failure is in `codegen_generate_unary_expr`'s `TOKEN_BIT_AND` arm, which demands an lvalue address. Task 1 adds a struct-literal special case there (reuse the already-generated aggregate value, `goo_alloc(sizeof(T))`, store, return the pointer — the pattern of heap locals in `function_codegen.c:168` and the `new` builtin arm in `call_codegen.c:211`). Task 2 makes the checker reject `&` on slice/array/map literals with a specific error so they never reach codegen's generic lvalue error.

**Tech Stack:** C23, LLVM-C API (LLVM 22). No runtime C changes, no grammar changes, no header changes.

**Spec:** `docs/superpowers/specs/2026-07-02-addr-struct-lit-design.md` (approved 2026-07-02).

## Global Constraints

- Branch: `feat/addr-struct-lit` (already created off main @61f02dd — do NOT commit on main).
- Commits: conventional style, imperative mood, `--no-gpg-sign`. Stage only named files; never stage anything under `.superpowers/`.
- No header edits are expected. If you DO touch any header, run `make clean` first (Makefile has no header deps; stale objects silently miscompile).
- Gate per task: `make lexer` (rebuild), run the task's probe, then `make verify` (golden suite + probe targets, all must pass; golden failures must stay 0) and `make test` (currently 76 pass / 1 pre-existing skip — 76/1 is green; do not chase the 1).
- KNOWN LATENT BUG (do not trip it): 3+ sequential `fmt.Println` of different-width ints corrupts the middle arg. The probes below print only `int` values — keep it that way.
- Zsh gotcha: don't write `make $VAR` with an unquoted variable in scripts.
- Multi-name params (`x, y int`) are a known parse gap — write per-param types (`x int, y int`).

## Reference: verified code landmarks (2026-07-02)

- Codegen `&` arm: `src/codegen/expression_codegen.c:1370-1383` (`case TOKEN_BIT_AND` inside `codegen_generate_unary_expr`). Locals in scope: `unary` (UnaryExprNode*), `operand` (ValueInfo* — generic operand generation already ran at the top of the function), `operand_llvm` (`operand->llvm_value`), `result`, `result_type`. After the switch, `value_info_free(operand)` runs at L1391 — a `break` path must NOT free `operand` itself.
- Checker `&` arm: `src/types/expression_checker.c:745-766` (`case TOKEN_BIT_AND` inside `type_check_unary_expr`). `operand_type` is already resolved; the arm ends with `result_type = type_pointer(operand_type)`.
- AST node kinds (include/ast.h): struct literal = `AST_STRUCT_LITERAL` (both keyed `Point{x: 3}` and positional `Point{3, 4}`); slice literal = `AST_SLICE_EXPR`; array literal = `AST_ARRAY_LITERAL`; map literal = `AST_PAREN_EXPR` (repurposed tag — grouping parens parse as identity and never produce a node, verified at ast.h:638 and parser.y:1793).
- A named-slice composite literal parses as `AST_STRUCT_LITERAL` but resolves to `TYPE_SLICE` — discriminate by BOTH node tag and resolved type kind.
- `goo_alloc` call shape: `call_codegen.c:211-232` (`new` arm): `LLVMGetNamedFunction(codegen->module, "goo_alloc")`, `LLVMValueRef size = LLVMSizeOf(t)`, `LLVMBuildCall2(..., alloc_fn, &size, 1, name)`.
- Reject-probe Makefile pattern: `strindex-reject-probe`, Makefile:471-480 (inline printf'd source, compile must fail, no binary emitted, grep the diagnostic). Registration: the `verify:` dependency list at Makefile:1217.
- Golden run-probes: `examples/<name>_probe.goo` + `.expected.txt`, auto-discovered by `make test-golden` (part of `verify`) — no registration needed.

---

### Task 1: `&StructLit{...}` heap lowering + golden probe

**Files:**
- Modify: `src/codegen/expression_codegen.c:1370` (the `TOKEN_BIT_AND` case)
- Test: `examples/addr_struct_lit_probe.goo` + `examples/addr_struct_lit_probe.expected.txt`

**Interfaces:**
- Consumes: existing struct-literal codegen (the operand value is already generated when the arm runs), `goo_alloc` (declared in every module), `type_pointer()`.
- Produces: `&<AST_STRUCT_LITERAL resolving to TYPE_STRUCT>` yields a `ValueInfo` holding a `*T` pointer to a heap copy. Task 2's checker guard relies on codegen only ever seeing the struct case.

- [ ] **Step 1: Write the failing probe**

`examples/addr_struct_lit_probe.goo`:
```go
package main

import "fmt"

type Point struct {
	X int
	Y int
}

func (p *Point) Sum() int {
	return p.X + p.Y
}

func newPoint(x int, y int) *Point {
	return &Point{X: x, Y: y}
}

func main() {
	p := newPoint(3, 4)
	fmt.Println(p.X)
	fmt.Println(p.Y)
	q := &Point{7, 9}
	fmt.Println(q.X)
	q.X = 21
	fmt.Println(q.X)
	fmt.Println(p.X)
	fmt.Println(p.Sum())
	fmt.Println(q.Sum())
}
```

`examples/addr_struct_lit_probe.expected.txt`:
```
3
4
7
21
3
7
30
```

Coverage notes: constructor-return (the escaping case that motivates heap allocation), keyed AND positional literal forms, mutation through one pointer not affecting the other (distinct allocations), pointer-receiver method calls, all printed values are `int` (same width — see Global Constraints).

- [ ] **Step 2: Verify it fails today**

Run: `bin/goo examples/addr_struct_lit_probe.goo 2>&1 | head -3`
Expected: `Error at ...: Cannot take address of non-lvalue` (a codegen error — NOT a type error and NOT a parse error).

- [ ] **Step 3: Implement the struct-literal case in the `&` arm**

In `src/codegen/expression_codegen.c`, replace the start of `case TOKEN_BIT_AND: {` (currently at L1370) so the case reads:

```c
        case TOKEN_BIT_AND: {
            // &StructType{...}: Go's addressable-rvalue special case. The
            // literal has no storage — give it leaked heap storage
            // (goo_alloc, the same lifetime model as escaping locals in
            // function_codegen.c) and yield the pointer. The generic
            // operand generation above already produced the aggregate
            // value; reuse it rather than re-generating (re-generation
            // would double-evaluate side effects in field expressions).
            if (unary->operand->type == AST_STRUCT_LITERAL &&
                operand->goo_type && operand->goo_type->kind == TYPE_STRUCT) {
                LLVMTypeRef struct_llvm = codegen_type_to_llvm(codegen, operand->goo_type);
                if (!struct_llvm) {
                    codegen_error(codegen, expr->pos, "&literal: cannot lower struct type");
                    value_info_free(operand);
                    return NULL;
                }
                LLVMValueRef lit_val = operand_llvm;
                if (operand->is_lvalue) {
                    lit_val = LLVMBuildLoad2(codegen->builder, struct_llvm,
                                             lit_val, "addr_lit_load");
                }
                LLVMValueRef alloc_fn = LLVMGetNamedFunction(codegen->module, "goo_alloc");
                if (!alloc_fn) {
                    codegen_error(codegen, expr->pos, "&literal: goo_alloc unavailable");
                    value_info_free(operand);
                    return NULL;
                }
                LLVMValueRef size = LLVMSizeOf(struct_llvm);
                LLVMValueRef heap_ptr = LLVMBuildCall2(codegen->builder,
                                                       LLVMGlobalGetValueType(alloc_fn),
                                                       alloc_fn, &size, 1, "addr_lit");
                LLVMBuildStore(codegen->builder, lit_val, heap_ptr);
                result = heap_ptr;
                result_type = type_pointer(operand->goo_type);
                break;
            }
            // Address-of (`&x`). The generic operand above was loaded (an
            // identifier auto-loads), so use the lvalue-address helper to get
            // the operand's storage address rather than its value.
            ValueInfo* addr = codegen_emit_lvalue_address(codegen, checker, unary->operand);
            if (!addr || !addr->is_lvalue) {
                codegen_error(codegen, expr->pos, "Cannot take address of non-lvalue");
                value_info_free(operand);
                return NULL;
            }
            result = addr->llvm_value;
            result_type = type_pointer(addr->goo_type);
            break;
        }
```

The existing non-literal path (from `ValueInfo* addr = ...` down) is byte-for-byte unchanged — only the struct-literal block above it is new. The `break` flows to the existing `value_info_free(operand)` after the switch; do not free `operand` on the success path.

- [ ] **Step 4: Rebuild and verify the probe passes**

Run: `make lexer` (no header change → no clean needed), then
`bin/goo -o build/addr_struct_lit_probe examples/addr_struct_lit_probe.goo && ./build/addr_struct_lit_probe`
Expected: `3 4 7 21 3 7 30`, one per line, exactly matching the expected file.

- [ ] **Step 5: Run the gate**

Run: `make verify` → all targets PASS, golden N/0 (N grew by 1). Run: `make test` → 76 pass / 1 skip.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/expression_codegen.c examples/addr_struct_lit_probe.goo examples/addr_struct_lit_probe.expected.txt
git commit --no-gpg-sign -m "feat(codegen): &T{} takes the address of a struct literal

Go's addressable-rvalue special case. The & arm demanded an lvalue
address, so a composite-literal operand died with 'Cannot take address
of non-lvalue'. Struct literals now get leaked heap storage (goo_alloc
+ store of the already-generated aggregate value, the escaping-locals
lifetime model) and yield *T. Unblocks the constructor idiom
'return &Foo{...}'. Non-struct literals are rejected at typecheck in
the follow-up commit."
```

---

### Task 2: Clean typecheck rejection of `&` on non-struct composite literals

**Files:**
- Modify: `src/types/expression_checker.c:745` (the `TOKEN_BIT_AND` case)
- Modify: `Makefile` (new `addrlit-reject-probe` target + registration in the `verify:` list at ~L1217)

**Interfaces:**
- Consumes: Task 1's codegen (struct literals are the one literal kind allowed through to codegen).
- Produces: `&` on `AST_SLICE_EXPR` / `AST_ARRAY_LITERAL` / `AST_PAREN_EXPR` (map literal) / `AST_STRUCT_LITERAL` resolving to a non-struct type → `type_error` containing "cannot take the address of a ... literal"; compile fails with nonzero exit, no crash.

- [ ] **Step 1: Verify the current (bad) behavior**

Run:
```bash
printf 'package main\nfunc main(){ p := &[]int{1, 2}; _ = p }\n' > build/addrlit_probe_tmp.goo
bin/goo build/addrlit_probe_tmp.goo 2>&1 | head -3
```
Expected today: the generic codegen error `Cannot take address of non-lvalue` (or a struct-codegen crash) — NOT a parse error. If this is a PARSE error instead, try `&map[string]int{"a": 1}` and `&[2]int{1, 2}` as the operand; use whichever reaches typecheck/codegen for the reject probe in Step 3, and report the parse gap in the task summary. Clean up: `rm -f build/addrlit_probe_tmp.goo`.

- [ ] **Step 2: Add the checker rejection**

In `src/types/expression_checker.c`, at the top of `case TOKEN_BIT_AND:` (L745, BEFORE the `if (unary->operand->type == AST_IDENTIFIER)` borrow check), insert:

```c
        case TOKEN_BIT_AND:  // & - take reference/borrow
            // Go allows & on any composite literal; Goo supports only the
            // struct case (heap-allocated by codegen). Reject the other
            // literal kinds here with a specific error — without this they
            // pass typecheck (this arm points ANY operand) and die in
            // codegen with the unhelpful "Cannot take address of non-lvalue".
            if (unary->operand->type == AST_SLICE_EXPR ||
                unary->operand->type == AST_ARRAY_LITERAL ||
                unary->operand->type == AST_PAREN_EXPR) {
                // AST_PAREN_EXPR is the map-literal tag (grouping parens
                // parse as identity and never produce a node — ast.h:638).
                const char* kind = unary->operand->type == AST_SLICE_EXPR ? "slice"
                                 : unary->operand->type == AST_ARRAY_LITERAL ? "array"
                                 : "map";
                type_error(checker, expr->pos,
                          "cannot take the address of a %s literal (only struct literals are supported)",
                          kind);
                return NULL;
            }
            if (unary->operand->type == AST_STRUCT_LITERAL &&
                operand_type->kind != TYPE_STRUCT) {
                // A named-slice/array composite literal parses as
                // AST_STRUCT_LITERAL and resolves to its underlying kind —
                // it must not reach the struct-only codegen path.
                type_error(checker, expr->pos,
                          "cannot take the address of a %s composite literal (only struct literals are supported)",
                          type_to_string(operand_type));
                return NULL;
            }
            // Check if we can borrow this value
```

Everything from `// Check if we can borrow this value` down is unchanged.

- [ ] **Step 3: Add the reject probe to the Makefile**

Next to `strindex-reject-probe` (after Makefile:480), following its exact shape:

```make
# &T{} supports STRUCT literals only — & on a slice/array/map literal must be
# a clean type error naming the literal kind, never the generic non-lvalue
# codegen error or a crash. Guards the expression_checker.c rejection.
addrlit-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== addrlit-reject-probe: & on a slice literal must reject ==="
	@printf 'package main\nfunc main(){ p := &[]int{1, 2}; _ = p }\n' > build/addrlit_reject.goo
	@rm -f build/addrlit_reject
	@$(COMPILER) -o build/addrlit_reject build/addrlit_reject.goo > build/addrlit_reject.out 2> build/addrlit_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "addrlit-reject-probe: FAIL (compiled rc=0 — &slice-literal silently accepted)"; exit 1; fi; \
	if [ -x build/addrlit_reject ]; then echo "addrlit-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "address of a slice literal" build/addrlit_reject.err; then echo "addrlit-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/addrlit_reject.err; exit 1; fi; \
	echo "addrlit-reject-probe: PASS (rejected rc=$$rc)"
```

(If Step 1 showed the slice-literal operand doesn't PARSE, substitute the operand and grep string with the literal kind that does reach the checker, and say so in the task summary.)

Register it: in the `verify:` dependency list (Makefile:1217), add `addrlit-reject-probe` immediately before `test-golden`.

- [ ] **Step 4: Rebuild and verify the rejection**

Run: `make lexer`, then `make addrlit-reject-probe`.
Expected: `addrlit-reject-probe: PASS (rejected rc=1)`.
Also spot-check the positive path still works: `bin/goo -o build/addr_struct_lit_probe examples/addr_struct_lit_probe.goo && ./build/addr_struct_lit_probe` → same 7 lines as Task 1.

- [ ] **Step 5: Run the gate**

Run: `make verify` → all targets PASS including `addrlit-reject-probe`, golden N/0. Run: `make test` → 76/1.

- [ ] **Step 6: Commit**

```bash
git add src/types/expression_checker.c Makefile
git commit --no-gpg-sign -m "feat(types): reject & on non-struct composite literals cleanly

Slice/array/map literals (and AST_STRUCT_LITERAL nodes resolving to a
non-struct type, e.g. named-slice composites) now fail typecheck with
an error naming the literal kind, instead of reaching codegen's generic
'Cannot take address of non-lvalue'. Struct literals stay allowed —
they lower to heap storage as of the previous commit. addrlit-reject-
probe guards the diagnostic."
```

---

## Final gate (after both tasks)

- `make verify` → ALL GREEN. `make test` → 76/1.
- ccomp (no runtime C changed, so this must pass trivially — run it anyway, as separate commands):
```bash
eval "$(opam env --switch=default)"
make ccomp-link
```
Expected: PASS, byte-identical baseline.

## Self-review notes

- Spec coverage: codegen heap lowering (Task 1), checker rejection incl. the named-slice AST_STRUCT_LITERAL edge (Task 2), golden probe with constructor/positional/mutation/method coverage (Task 1), reject probe (Task 2), gates incl. ccomp (final). Non-goals (escape analysis, non-struct literal support, reclamation) have no tasks — by design.
- Type consistency: Task 1 produces `*T` via `type_pointer(operand->goo_type)`; checker already produced `type_pointer(operand_type)` — consistent. Task 2 uses only node tags + `operand_type`, both in scope at the insertion point.
- The one open empirical question (does `&[]int{...}` parse?) has an explicit verification step with fallbacks (Task 2 Steps 1/3), not an assumption.
