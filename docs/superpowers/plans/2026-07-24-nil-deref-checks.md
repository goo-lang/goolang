# Nil-Deref Checks Implementation Plan (ADR 0001)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the last four unguarded nil SIGSEGVs and the one silent-wrong-value nil behavior with arc-17-style inline checks, making every nil failure a diagnosable Go-style panic (ADR 0001, accepted).

**Architecture:** A new `codegen_emit_nil_check` helper (mirroring `codegen_emit_bounds_check`, `src/codegen/composite_codegen.c:28`) emits `icmp eq null` → cold branch → noreturn `goo_nil_deref_fail(file, line)` → unreachable, at the five pointer-deref emission sites plus the interface-dispatch vtable and the `error(nil).Error()` guard. LLVM's O2 pipeline eliminates/hoists checks on provably-non-nil paths, exactly as proven for bounds checks in arc 17 (PR #211).

**Tech Stack:** C23, LLVM-C, existing probe-script harness (`scripts/exit_code_probe.sh` pattern).

## Global Constraints

- **ADR 0001 is normative** (`docs/adr/0001-nil-semantics-go-parity-with-emitted-deref-checks.md`); `.handoff.md` item 0 is the work order.
- **Canonical message is API**: stderr must contain `panic: runtime error: invalid memory address or nil pointer dereference` (Go's text) and exit with code **2**. A `nil dereference at <file>:<line>` diagnostic line precedes it (mirrors `goo_bounds_fail`'s file:line convention).
- **Go-parity semantics, exactly**: a method call on a **typed-nil receiver is LEGAL** and must dispatch — only the *field access inside* panics (that's why the receiver site is the selector-through-pointer check, not a call-site check). A **nil interface** method call panics at dispatch. Do not "improve" on Go.
- **NO parser/lexer/grammar changes.** `./scripts/grammar-tripwire.sh` at exact baseline (31 S/R / 0 R/R) before AND after.
- Commit form: `git -c commit.gpgsign=false commit` (1Password signing fails), conventional commits, imperative mood.
- After every task: `make test` green + the task's probe green. Full goldens (`make test-golden` / `test-golden-o2`, 463/0) after Tasks 2–4 (they change emitted IR); `make verify-core` at the end of Tasks 2–5.
- Machine note: if anything triggers an NNG rebuild, prefix `PATH=/usr/bin:$PATH` (broken `~/.local/bin/cmake` pip shim).
- Never trust a piped exit code; read real outputs.
- Perf gate (ADR risk section): `lanes-kernel-ir-pin` must stay green and the arc-17 vector-type predicate must still pass — nil checks on hoisted, loop-invariant receiver pointers must not break kernel vectorization. If the pin goes red, STOP and escalate (do not weaken the pin, do not add `nonnull` attributes — that would delete the checks and reintroduce the SIGSEGV).

---

### Task 1: Runtime fail function + remove dead `goo_null_check`

**Files:**
- Modify: `src/runtime/runtime.c` (add `goo_nil_deref_fail` next to `goo_bounds_fail`; delete `goo_null_check` at ~line 1076)
- Modify: `include/runtime.h` (declare new fn in the arc-17 fail-function block ~line 370; delete `goo_null_check` decl ~line 393 and the `GOO_NULL_CHECK` macro ~line 398)
- Modify: `src/codegen/runtime_integration.c` (declare `goo_nil_deref_fail` with noreturn/cold/nounwind next to `goo_bounds_fail` ~line 589; delete the `goo_null_check` declaration ~line 627)

**Interfaces:**
- Produces: `void goo_nil_deref_fail(const char* file, int line)` — prints the diagnostic + canonical panic, never returns, exit 2. Declared to codegen as noreturn/cold/nounwind. Consumed by every later task.
- Removes: `goo_null_check` / `GOO_NULL_CHECK` (handoff: "dead code to remove" — codegen never emitted calls; verify with the grep in Step 1).

- [ ] **Step 1: Confirm goo_null_check is emission-dead**

Run: `grep -rn "goo_null_check\|GOO_NULL_CHECK" src/ include/ | grep -v "runtime.c\|runtime.h\|runtime_integration.c"`
Expected: no output (only the three definition/declaration sites exist). If anything else shows up, STOP and report — the handoff's dead-code claim would be wrong.

- [ ] **Step 2: Add the fail function, delete the dead one**

In `src/runtime/runtime.c`, directly below `goo_bounds_fail`:

```c
// ADR 0001: cold noreturn target of the inline nil checks
// (codegen_emit_nil_check) at pointer-deref/field/interface-dispatch
// sites — same shape as goo_bounds_fail above. The message text is Go's
// canonical nil-panic wording and is pinned by scripts/nil_deref_probe.sh;
// changing it is a contract change, not a wording tweak.
void goo_nil_deref_fail(const char* file, int line) {
    fprintf(stderr, "nil dereference at %s:%d\n", file, line);
    goo_panic("runtime error: invalid memory address or nil pointer dereference");
}
```

Delete the whole `goo_null_check` function (~line 1076).

In `include/runtime.h`: add `void goo_nil_deref_fail(const char* file, int line);` to the arc-17 fail-function block (~line 370, beside `goo_bounds_fail`); delete the `goo_null_check` prototype and the `GOO_NULL_CHECK` macro.

- [ ] **Step 3: Declare to codegen with the cold attributes**

In `src/codegen/runtime_integration.c`, next to the `goo_bounds_fail` declaration (~line 589-604), mirroring it exactly:

```c
    // ADR 0001: void goo_nil_deref_fail(const char* file, int line) —
    // UNCONDITIONAL fail, noreturn. codegen_emit_nil_check calls this only
    // on the cold failure edge, so it needs noreturn/cold/nounwind, same
    // as goo_bounds_fail above.
    {
        LLVMTypeRef params[] = { i8_ptr_type, i32_type };
        add_runtime_function(codegen, "goo_nil_deref_fail", void_type, params, 2);
        LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_nil_deref_fail");
        add_fn_attr(codegen->context, fn, "noreturn");
        add_fn_attr(codegen->context, fn, "cold");
        add_fn_attr(codegen->context, fn, "nounwind");
    }
```

(Use the exact local type-variable names the `goo_bounds_fail` block uses — read it first; if its attr calls differ in shape, mirror them verbatim.) Delete the `goo_null_check` declaration block (~line 627-630).

- [ ] **Step 4: Build + verify**

Run: `make lexer && nm lib/libgoo_runtime.a | grep -E "goo_nil_deref_fail|goo_null_check"`
Expected: `T goo_nil_deref_fail` present, `goo_null_check` ABSENT.
Run: `make test`
Expected: green (13/13 CLI + unit suite).

- [ ] **Step 5: Commit**

```bash
git add src/runtime/runtime.c include/runtime.h src/codegen/runtime_integration.c
git -c commit.gpgsign=false commit -m "feat(runtime): goo_nil_deref_fail cold noreturn target; remove dead goo_null_check (ADR 0001)"
```

---

### Task 2: `codegen_emit_nil_check` + the five pointer-deref sites + probe

**Files:**
- Modify: `src/codegen/composite_codegen.c` (helper beside `codegen_emit_bounds_check` at :28; field-read site ~:847)
- Modify: `src/codegen/expression_codegen.c` (star-read ~:2777; star-write lvalue arm ~:1097; selector lvalue arm ~:953; `m[k].F` pointer arm ~:931)
- Modify: `src/codegen/codegen.h` or the header that exports `codegen_emit_bounds_check` (add the two new prototypes beside it — find with `grep -rn "codegen_emit_bounds_check" include/ src/codegen/*.h`)
- Create: `scripts/nil_deref_probe.sh`
- Modify: `Makefile` (`nil-deref-probe` target + append to VERIFY_ALL_DEPS)

**Interfaces:**
- Consumes: `goo_nil_deref_fail` (Task 1).
- Produces (used verbatim by Tasks 3–4):
  - `void codegen_emit_nil_check_cond(CodeGenerator* codegen, LLVMValueRef is_nil, ASTNode* expr)` — `is_nil` is an i1; splits the block, cold fail edge calls `goo_nil_deref_fail(file, line)` from `expr->pos`, builder left in the continue block.
  - `void codegen_emit_nil_check(CodeGenerator* codegen, LLVMValueRef ptr, ASTNode* expr)` — builds `icmp eq ptr, null` and delegates to the cond variant.
  - Probe harness contract: `scripts/nil_deref_probe.sh` compiles inline fixtures, asserts **exit 2** AND stderr contains BOTH `nil dereference at ` and `panic: runtime error: invalid memory address or nil pointer dereference`.

- [ ] **Step 1: Write the probe script (RED first)**

`scripts/nil_deref_probe.sh`:

```bash
#!/bin/bash
# ADR 0001 nil-deref probes: each unguarded-site fixture must PANIC with
# Go's canonical message and exit 2 — never SIGSEGV (exit 139). Mirrors
# scripts/exit_code_probe.sh's inline-fixture harness. Also pins the
# LEGAL nil cases (typed-nil method dispatch without field access) so the
# checks never over-fire and break Go parity.

set -u

fail() { echo "FAIL: $1"; exit 1; }

COMPILER="$(cd "$(dirname "$0")/.." && pwd)/bin/goo"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

[ -x "$COMPILER" ] || fail "compiler not found at $COMPILER (run 'make')"

MSG='panic: runtime error: invalid memory address or nil pointer dereference'

# Compile $2, run it, assert exit 2 + both stderr markers. $1 = case name.
check_nilpanic() {
    local name="$1"
    local src="$WORKDIR/$name.goo" exe="$WORKDIR/$name.out" err="$WORKDIR/$name.err"
    printf "%s" "$2" > "$src"
    if ! "$COMPILER" "$src" -o "$exe" > "$WORKDIR/$name.log" 2>&1; then
        sed 's/^/    /' "$WORKDIR/$name.log"; fail "$name: compilation failed"
    fi
    "$exe" >/dev/null 2>"$err"; local got=$?
    [ "$got" = "2" ] || { sed 's/^/    stderr: /' "$err"; fail "$name: exit $got, expected 2 (139 = still SIGSEGV)"; }
    grep -q "nil dereference at " "$err" || { sed 's/^/    stderr: /' "$err"; fail "$name: missing file:line diagnostic"; }
    grep -qF "$MSG" "$err" || { sed 's/^/    stderr: /' "$err"; fail "$name: missing canonical panic message"; }
    echo "  ok: $name (panic, exit 2)"
}

# Compile $2, run it, assert clean exit 0 and stdout == $3. $1 = case name.
check_ok() {
    local name="$1" expected_out="$3"
    local src="$WORKDIR/$name.goo" exe="$WORKDIR/$name.out"
    printf "%s" "$2" > "$src"
    if ! "$COMPILER" "$src" -o "$exe" > "$WORKDIR/$name.log" 2>&1; then
        sed 's/^/    /' "$WORKDIR/$name.log"; fail "$name: compilation failed"
    fi
    local out; out="$("$exe" 2>"$WORKDIR/$name.err")"; local got=$?
    [ "$got" = "0" ] || { sed 's/^/    stderr: /' "$WORKDIR/$name.err"; fail "$name: exit $got, expected 0"; }
    [ "$out" = "$expected_out" ] || fail "$name: stdout '$out', expected '$expected_out'"
    echo "  ok: $name (legal, exit 0)"
}

check_nilpanic star_read 'package main
import "fmt"
func main() {
	var p *int
	fmt.Println(*p)
}
'

check_nilpanic star_write 'package main
func main() {
	var p *int
	*p = 1
}
'

check_nilpanic field_read 'package main
import "fmt"
type T struct{ x int }
func main() {
	var p *T
	fmt.Println(p.x)
}
'

check_nilpanic field_write 'package main
type T struct{ x int }
func main() {
	var p *T
	p.x = 1
}
'

check_nilpanic nil_receiver_method_field 'package main
import "fmt"
type T struct{ x int }
func (t *T) Get() int { return t.x }
func main() {
	var p *T
	fmt.Println(p.Get())
}
'

# LEGAL Go: a method on a typed-nil receiver that never touches fields
# runs fine — the check must live at the FIELD access, not the call.
check_ok nil_receiver_method_no_field 'package main
import "fmt"
type T struct{ x int }
func (t *T) Tag() int { return 42 }
func main() {
	var p *T
	fmt.Println(p.Tag())
}
' '42'

# LEGAL: non-nil paths unaffected.
check_ok non_nil_paths 'package main
import "fmt"
type T struct{ x int }
func main() {
	v := 7
	p := &v
	*p = *p + 1
	t := &T{x: 5}
	t.x = t.x + 1
	fmt.Println(*p)
	fmt.Println(t.x)
}
' '8
6'

echo "nil-deref-probe: PASS (all cases)"
```

Run: `chmod +x scripts/nil_deref_probe.sh && ./scripts/nil_deref_probe.sh`
Expected: FAIL on `star_read` with `exit 139` (the raw SIGSEGV — RED evidence; record the exact failure line in your report). If a panic-case unexpectedly compiles-fails instead, record which and adapt the fixture minimally (the probe matrix says all five compile today).

- [ ] **Step 2: Write the helper (mirrors codegen_emit_bounds_check)**

In `src/codegen/composite_codegen.c`, directly below `codegen_emit_bounds_check` (after line ~66), inside the same `#if LLVM_AVAILABLE`:

```c
// ADR 0001: emit an INLINE `ptr == null` compare + conditional branch to a
// cold fail block calling the noreturn goo_nil_deref_fail(file, line) —
// the exact bounds-check shape above, applied to nil. Sites: unary *p
// read/write, struct-field access through a pointer (read + lvalue paths —
// which also covers a nil RECEIVER whose method touches fields, since that
// panic happens at the field selector inside the method body, Go-parity),
// interface dispatch (vtable null), and error(nil).Error(). The cond
// variant exists because two sites (interface vtable, error handle) start
// from an extracted value or an existing i1 rather than a raw pointer.
//
// Splits the current block; the builder is left in the continue block.
void codegen_emit_nil_check_cond(CodeGenerator* codegen, LLVMValueRef is_nil,
                                 ASTNode* expr) {
    LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_nil_deref_fail");
    // Same known footgun as bounds: no symbol -> unguarded (best-effort).
    // codegen_declare_runtime_functions always declares it.
    if (!fn) return;

    LLVMValueRef cur_fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(codegen->builder));
    LLVMBasicBlockRef fail_bb = LLVMAppendBasicBlockInContext(codegen->context, cur_fn, "nil_fail");
    LLVMBasicBlockRef cont_bb = LLVMAppendBasicBlockInContext(codegen->context, cur_fn, "nil_ok");
    LLVMBuildCondBr(codegen->builder, is_nil, fail_bb, cont_bb);

    LLVMPositionBuilderAtEnd(codegen->builder, fail_bb);
    LLVMValueRef file = LLVMBuildGlobalStringPtr(codegen->builder,
        expr->pos.filename ? expr->pos.filename : "<input>", "nil_file");
    LLVMValueRef line = LLVMConstInt(LLVMInt32TypeInContext(codegen->context),
                                     (unsigned long long)expr->pos.line, 0);
    LLVMValueRef args[2] = { file, line };
    LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn), fn, args, 2, "");
    LLVMBuildUnreachable(codegen->builder);

    LLVMPositionBuilderAtEnd(codegen->builder, cont_bb);
}

void codegen_emit_nil_check(CodeGenerator* codegen, LLVMValueRef ptr,
                            ASTNode* expr) {
    if (!ptr) return;
    LLVMValueRef is_nil = LLVMBuildICmp(codegen->builder, LLVMIntEQ, ptr,
                                        LLVMConstNull(LLVMTypeOf(ptr)), "nil_cond");
    codegen_emit_nil_check_cond(codegen, is_nil, expr);
}
```

Add both prototypes to the header that declares `codegen_emit_bounds_check` (found in Step 0 grep), beside it.

- [ ] **Step 3: Wire the five sites**

Site A — **star read**, `src/codegen/expression_codegen.c` ~:2773 (`TOKEN_MULTIPLY` in `codegen_generate_unary_expr`): insert the check between obtaining `operand_llvm` (the pointer VALUE — the lvalue auto-load at ~:2745 already ran) and the `LLVMBuildLoad2`:

```c
        case TOKEN_MULTIPLY:
            // Dereference pointer. operand_llvm is the pointer value; the
            // pointee LLVM type comes from the goo type (LLVMGetElementType is
            // unusable under opaque pointers).
            if (operand->goo_type->kind == TYPE_POINTER) {
                codegen_emit_nil_check(codegen, operand_llvm, expr);
                LLVMTypeRef pointee = codegen_type_to_llvm(codegen, operand->goo_type->data.pointer.pointee_type);
                result = LLVMBuildLoad2(codegen->builder, pointee, operand_llvm, "deref");
            } else {
```

Site B — **star write**, `src/codegen/expression_codegen.c` ~:1097 (`codegen_emit_lvalue_address` AST_UNARY_EXPR arm). CAUTION: `ptr->llvm_value` here may be an UNLOADED lvalue (an alloca holding the pointer) — the arc-16 lesson. Determine what `codegen_generate_expression` returns for the operand (an identifier comes back as an lvalue alloca); if `ptr->is_lvalue`, load the pointer value first for BOTH the check and the returned address — the current code passing the alloca onward means the store path loads later; do NOT change the return contract, only check the loaded VALUE:

```c
        if (un->operator == TOKEN_MULTIPLY) {
            ValueInfo* ptr = codegen_generate_expression(codegen, checker, un->operand);
            if (!ptr || !ptr->goo_type || ptr->goo_type->kind != TYPE_POINTER) return NULL;
            LLVMValueRef pval = ptr->llvm_value;
            if (ptr->is_lvalue) {
                pval = LLVMBuildLoad2(codegen->builder,
                    LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),
                    ptr->llvm_value, "deref_ptr_load");
            }
            codegen_emit_nil_check(codegen, pval, expr);
            ValueInfo* out = value_info_new(NULL, pval,
                                            ptr->goo_type->data.pointer.pointee_type);
            out->is_lvalue = 1;
            return out;
        }
```

NOTE the return now carries the LOADED pointer as the address (correct: the store address IS the pointer value). Verify against the current behavior first: if the existing code returning the unloaded alloca actually works today (i.e. some downstream load exists), then returning `pval` changes double-load behavior — reconcile by reading the assignment store path, and keep the semantics identical apart from the added check; record what you found in the report. The probe's `star_write` and `non_nil_paths` cases are the behavioral referee.

Site C — **field read through pointer**, `src/codegen/composite_codegen.c` ~:847 (selector codegen's pointer-to-struct unwrap): after the block that produces the struct address (both the lvalue-load and rvalue arms), before the GEP fork:

```c
    if (base_type->kind == TYPE_POINTER && base_type->data.pointer.pointee_type->kind == TYPE_STRUCT) {
        if (base_val->is_lvalue) {
            base_val->llvm_value = LLVMBuildLoad2(codegen->builder,
                                                  LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),
                                                  base_val->llvm_value, "struct_ptr");
        }
        codegen_emit_nil_check(codegen, base_val->llvm_value, expr);
        base_type = base_type->data.pointer.pointee_type;
        base_val->is_lvalue = 1;
    }
```

Site D — **field write through pointer**, `src/codegen/expression_codegen.c` ~:953 (lvalue-address selector arm): after the `struct_addr` load:

```c
        if (st && st->kind == TYPE_POINTER && st->data.pointer.pointee_type &&
            st->data.pointer.pointee_type->kind == TYPE_STRUCT) {
            struct_addr = LLVMBuildLoad2(codegen->builder,
                                         LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0),
                                         base->llvm_value, "struct_ptr");
            codegen_emit_nil_check(codegen, struct_addr, expr);
            st = st->data.pointer.pointee_type;
        }
```

Site E — **`m[k].F` through pointer map value**, `src/codegen/expression_codegen.c` ~:931: after generating `ptr_val`:

```c
                    ValueInfo* ptr_val = codegen_generate_expression(codegen, checker, sel->expr);
                    if (!ptr_val) return NULL;
                    codegen_emit_nil_check(codegen, ptr_val->llvm_value, expr);
                    return codegen_emit_struct_field_lvalue(
```

(If `ptr_val` can be an lvalue here, apply the same load-first discipline as Site B; read the inline map-get comment above the site — it says the fast path returns the pointer directly as an rvalue.)

- [ ] **Step 4: Run the probe to GREEN**

Run: `make lexer && ./scripts/nil_deref_probe.sh`
Expected: `nil-deref-probe: PASS (all cases)` — every panic case exits 2 with both markers; both legal cases exit 0. Debug note: `nil_receiver_method_field` panics via Site C *inside the method body* — its `nil dereference at` line must point into the method's field access, not the call site (check the reported line number; record it).

- [ ] **Step 5: Makefile target + verify wiring**

```makefile
# ADR 0001: nil-deref checks — every unguarded nil SIGSEGV is now a
# diagnosable Go-parity panic (exit 2, canonical message); legal typed-nil
# dispatch stays legal.
nil-deref-probe: $(COMPILER) $(RUNTIME_LIB)
	@bash scripts/nil_deref_probe.sh
	@echo "nil-deref-probe: PASS"
.PHONY: nil-deref-probe
```

Append `nil-deref-probe` to the VERIFY_ALL_DEPS definition (grep for it; append to the base list, not the filter-out).

- [ ] **Step 6: Full IR-affecting gates**

Run: `make test && make test-golden && make test-golden-o2 && make lanes-kernel-ir-pin`
Expected: ALL green, 463/0 both levels, kernel pin PASS (vector predicate intact). If lanes-kernel-ir-pin fails: STOP per Global Constraints — report the IR delta, do not weaken the pin.

- [ ] **Step 7: Commit**

```bash
git add src/codegen/composite_codegen.c src/codegen/expression_codegen.c scripts/nil_deref_probe.sh Makefile
# plus the header that gained the prototypes
git -c commit.gpgsign=false commit -m "feat(codegen): inline nil checks at pointer deref/field sites — cold noreturn fail path (ADR 0001)"
```

---

### Task 3: Nil interface dispatch check

**Files:**
- Modify: `src/codegen/call_codegen.c` (interface-dispatch path, ~:1656 "Interface dispatch (P4-5)")
- Modify: `scripts/nil_deref_probe.sh` (two new cases)

**Interfaces:**
- Consumes: `codegen_emit_nil_check` (Task 2).
- Produces: nil user-interface method calls panic at dispatch; typed-nil-inside-interface still dispatches (Go parity).

- [ ] **Step 1: Add the RED cases to the probe**

Append to `scripts/nil_deref_probe.sh` (before the final PASS echo):

```bash
check_nilpanic nil_interface_dispatch 'package main
import "fmt"
type Speaker interface{ Speak() int }
func main() {
	var s Speaker
	fmt.Println(s.Speak())
}
'

# LEGAL Go: an interface HOLDING a typed-nil *T dispatches fine; only a
# field access inside the method panics (covered by Task 2 site C).
check_ok typed_nil_in_interface 'package main
import "fmt"
type Speaker interface{ Speak() int }
type T struct{ x int }
func (t *T) Speak() int { return 9 }
func main() {
	var p *T
	var s Speaker = p
	fmt.Println(s.Speak())
}
' '9'
```

Run: `./scripts/nil_deref_probe.sh`
Expected: FAIL at `nil_interface_dispatch` with exit 139 (RED). If `typed_nil_in_interface` fails for an unrelated reason (e.g. boxing a nil pointer into an interface misbehaves), record it — that is a separate pre-existing bug; report and let the controller decide scope.

- [ ] **Step 2: Emit the check on the vtable**

In `src/codegen/call_codegen.c`'s interface-dispatch path (the `recv_type->kind == TYPE_INTERFACE` block ~:1656): after `iface_val` is loaded (post the `iv->is_lvalue` load), find where the vtable is extracted from the `{vtable, data}` aggregate (read the following ~40 lines to locate the `LLVMBuildExtractValue(..., 0, ...)` for the vtable — mirror the local naming). Insert, immediately after the vtable extraction and BEFORE any load through it:

```c
            // ADR 0001: a NIL INTERFACE (no boxed type -> null vtable) has
            // nothing to dispatch to — Go panics with the canonical
            // nil-deref message. Checked on the VTABLE only, deliberately
            // NOT the data pointer: an interface holding a typed-nil *T
            // has a real vtable and MUST dispatch (Go parity — the panic,
            // if any, happens at the field access inside the method).
            codegen_emit_nil_check(codegen, vtable_val, expr);
```

(`vtable_val` = whatever local holds the extracted vtable pointer; if the extraction yields a non-pointer (e.g. i64), build the icmp against the matching zero constant via `codegen_emit_nil_check_cond` instead — record which shape you found.)

- [ ] **Step 3: GREEN + gates + commit**

Run: `make lexer && ./scripts/nil_deref_probe.sh && make test && make test-golden`
Expected: all cases green incl. both new ones; goldens 463/0 (interface fixtures unaffected — every existing golden dispatches on non-nil interfaces).

```bash
git add src/codegen/call_codegen.c scripts/nil_deref_probe.sh
git -c commit.gpgsign=false commit -m "feat(codegen): nil-interface dispatch panics with canonical message; typed-nil dispatch stays legal (ADR 0001)"
```

---

### Task 4: Remove the `error(nil).Error()` empty-string guard

**Files:**
- Modify: `src/codegen/call_codegen.c` ~:1613-1654 (the `error.Error()` nil-guarded read)
- Modify: `scripts/nil_deref_probe.sh` (one new case)

**Interfaces:**
- Consumes: `codegen_emit_nil_check_cond` (Task 2).
- Produces: `error(nil).Error()` panics (ADR: the last silent-wrong-value nil behavior removed). Existing fixtures that relied on `""` are real bugs — fix the fixtures, not the semantics (ADR Risks).

- [ ] **Step 1: RED case**

Append to `scripts/nil_deref_probe.sh`:

```bash
check_nilpanic error_nil_error 'package main
import "fmt"
func main() {
	var e error
	fmt.Println(e.Error())
}
'
```

Run: `./scripts/nil_deref_probe.sh`
Expected: FAIL at `error_nil_error` — today it prints "" and exits 0 (the silent wrong value; record the observed behavior as RED evidence).

- [ ] **Step 2: Replace the select-guard with the fail branch**

In the `error.Error()` block (~:1613): after `is_null` and `handle` are extracted, replace the entire empty-string construction + `LLVMBuildSelect` tail with:

```c
            // ADR 0001: error(nil).Error() PANICS (Go parity: method call
            // on a nil interface value). Replaces the former select()'d
            // empty-string guard — the probe matrix's one silent-wrong-value
            // cell. goo_error_message is now only ever called on the
            // non-null arm.
            codegen_emit_nil_check_cond(codegen, is_null, expr);
            LLVMValueRef msgfn = LLVMGetNamedFunction(codegen->module, "goo_error_message");
            if (!msgfn) {
                codegen_error(codegen, expr->pos, "goo_error_message not found in module");
                return NULL;
            }
            LLVMTypeRef msgfn_ty = LLVMGlobalGetValueType(msgfn);
            LLVMValueRef cargs[] = { handle };
            LLVMValueRef msg = LLVMBuildCall2(codegen->builder, msgfn_ty, msgfn, cargs, 1, "err.msg");
            return value_info_new(NULL, msg, type_checker_get_builtin(checker, TYPE_STRING));
```

(The `is_null` extraction stays; the `goo_error_message` call MOVES to after the check so it only executes on the survivor path. Delete the `empty`/`LLVMBuildSelect` lines and the old comment about the harmless null-arm call — it is no longer true and no longer needed.)

- [ ] **Step 3: GREEN + fixture sweep**

Run: `make lexer && ./scripts/nil_deref_probe.sh`
Expected: all cases PASS.
Run: `make test && make test-golden && make test-golden-o2`
Expected: goldens 463/0 — IF any golden/reject fixture goes red because it relied on `error(nil).Error() == ""`, per the ADR that fixture has a latent bug: fix the FIXTURE (give it a real error or guard with `!= nil`), regenerate its expectation ONLY if its intent is preserved, and list every touched fixture in the report with a one-line justification each.

- [ ] **Step 4: Commit**

```bash
git add src/codegen/call_codegen.c scripts/nil_deref_probe.sh
# plus any fixture files the sweep repaired
git -c commit.gpgsign=false commit -m "feat(codegen): error(nil).Error() panics — remove the empty-string guard (ADR 0001)"
```

---

### Task 5: Docs (nil matrix), perf evidence, full gate

**Files:**
- Modify: `docs/02-LANGUAGE-SPECIFICATION.md` (nil-semantics section with the 24-cell matrix)
- Modify: `docs/2026-07-08-v1-roadmap.md` (P2.2 closed: decided + implemented)
- Modify: `.handoff.md` (item 0 resolved; note the two REMAINING message-wording divergences as documented-not-fixed)
- Modify: `docs/GO_SPEC_CONFORMANCE.md` ONLY if a divergence row it lists is affected (read it; the nil work should not move matrix rows — say so in the report either way)

**Interfaces:**
- Consumes: everything above.
- Produces: the documented nil contract + the ADR's perf-risk evidence.

- [ ] **Step 1: Write the spec section**

In `docs/02-LANGUAGE-SPECIFICATION.md`, add (or extend an existing nil/pointers section — read the file's structure first and match its voice) a "Nil semantics (Go parity)" section containing:
1. The contract sentence: Goo adopts Go's nil semantics for `*T`/`[]T`/`map`/`chan`/`func`/interfaces/`error`; `?T`/`?*T` remain the opt-in non-nullable differentiator (ADR 0001).
2. The 24-cell matrix as a table, reconstructed from ADR 0001's Context paragraph plus the now-fixed cells. Rows (status after this arc — every row `panics` below is pinned by `scripts/nil_deref_probe.sh`): nil assignment/comparison for the five nilable kinds (works); nil-map read / write / delete / range (works, Go-parity incl. write panic per the P3.9 decision — copy its wording from the roadmap if it differs); nil-slice len/append (works); nil-channel send/recv block forever, close(nil) panics (works); pointer deref read → panics; pointer deref write → panics; field read via nil pointer → panics; field write via nil pointer → panics; nil-receiver method not touching fields → runs (legal); nil-receiver method touching fields → panics; nil user-interface dispatch → panics; typed-nil in interface dispatch → runs; `error(nil).Error()` → panics.
3. The canonical panic text and exit code 2, plus the `nil dereference at file:line` diagnostic line.
4. The two REMAINING message-wording divergences (nil slice index, nil func call — message text differs from Go's; behavior conforms) listed as documented divergences, deliberately not changed in this arc.

- [ ] **Step 2: Roadmap + handoff**

`docs/2026-07-08-v1-roadmap.md`: in Open decisions, mark the P2.2 nil-semantics fork RESOLVED (ADR 0001, this arc) — append, don't rewrite the decision trail. `.handoff.md`: replace item 0's "NEXT ARC (decided, ready to implement)" with a one-paragraph resolution (commits, probe name, the two wording divergences left documented, `goo_null_check` removed).

- [ ] **Step 3: Perf evidence (ADR risk gate)**

Run: `make lanes-kernel-ir-pin`
Expected: PASS (vector predicate + fast-math flags intact — nil checks on receiver field access hoist out of kernel loops because lanes code binds `own := ctx.own` before its hot loops).
Then grep the kernel IR for the residual check count:
Run: `grep -c "nil_fail" build/lm_ir.ll 2>/dev/null; "$(pwd)/bin/goo" --emit-llvm examples/lanes_stencilstep_probe.goo -o build/nilcheck_ir.ll && grep -c "nil_fail" build/nilcheck_ir.ll`
Record the counts in the report and in `.handoff.md`'s resolution paragraph (the arc-17 precedent: 15 residual bounds checks were ledgered as headroom; do the same for nil-check residuals — evidence, not assertion).

- [ ] **Step 4: The full gate**

Run: `make test && make test-golden && make test-golden-o2 && make verify-core && ./scripts/grammar-tripwire.sh`
Expected: ALL green — verify-core now includes `nil-deref-probe`; tripwire 31 S/R / 0 R/R exact. Read real outputs.

- [ ] **Step 5: Commit**

```bash
git add docs/02-LANGUAGE-SPECIFICATION.md docs/2026-07-08-v1-roadmap.md .handoff.md
# plus GO_SPEC_CONFORMANCE.md only if touched
git -c commit.gpgsign=false commit -m "docs(spec,roadmap): nil-semantics contract + 24-cell matrix; P2.2 closed (ADR 0001)"
```

---

## Plan Self-Review (done at write time)

- **Spec (ADR + handoff item 0) coverage:** 4 SIGSEGV sites → Task 2 (star read/write, field read/write — nil-receiver-method covered by the field-selector site, matching Go's panic location) + Task 3 (interface dispatch); silent wrong value → Task 4; `goo_nil_deref_fail` + canonical message + exit 2 → Task 1; `goo_null_check` removal → Task 1; arc-17 pattern reuse → Task 2 helper mirrors `codegen_emit_bounds_check` verbatim-shaped; perf measurement before merge → Task 5 Step 3 + the lanes-kernel-ir-pin STOP rule; spec gains the matrix → Task 5; optional message-wording alignment → deliberately OUT of scope, documented as divergences (Task 5 Step 1.4) — recorded here as the plan's one scope decision against an optional item.
- **Placeholder scan:** clean; the three "read the neighboring code first" notes (attr-call shape in Task 1, vtable extraction local in Task 3, star-write load reconciliation in Task 2 Site B) each name the exact symbol to mirror and the behavioral referee (probe case) that adjudicates.
- **Type consistency:** `codegen_emit_nil_check(CodeGenerator*, LLVMValueRef, ASTNode*)` / `codegen_emit_nil_check_cond(CodeGenerator*, LLVMValueRef, ASTNode*)` used identically in Tasks 2/3/4; `goo_nil_deref_fail(const char*, int)` matches Task 1's runtime definition and runtime_integration declaration; probe function names (`check_nilpanic`/`check_ok`) consistent across Tasks 2–4.
