# Interface-Target RTTI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Allow type assertions and type switches to an **interface target** — `x.(Stringer)`, `v, ok := x.(Stringer)`, and `switch x.(type) { case Stringer: }` — on an interface/`any` operand.

**Architecture:** Closed-world enumeration. Goo compiles the whole program, so at each interface-target site the compiler enumerates the concrete types implementing the target interface `I` (scope walk + `type_interface_satisfied`) and emits a runtime chain comparing the operand's dynamic-type descriptor (`box.vtable[0]`, from PR #137) against each implementer's descriptor. On match with `T`, the target value `{ (T,I) vtable, box.data }` is built. One shared codegen primitive serves all three forms.

**Tech Stack:** C23, LLVM-C 22 IR builder, the repo's golden-probe + abort-probe harness, `make verify` / `make test`.

## Global Constraints

- TDD: write the failing golden/abort probe first, watch it fail, then implement. (CLAUDE.md)
- Every task ends green on: `bash scripts/run_golden.sh` (0 failures), `make test` (76 pass / 1 skip), `./scripts/grammar-tripwire.sh` (**82 S/R + 256 R/R** — no grammar change in this plan), and `make verify` ALL GREEN (run `eval "$(opam env --switch=default)"` first so `ccomp` is on PATH).
- Interface box stays `{ ptr vtable, ptr data }`; method vtable slots 1..n unchanged. Dynamic-type identity is `box.vtable[0]` (the per-type descriptor pointer from #137).
- No new runtime metadata: reuse `codegen_get_or_emit_type_desc`, `codegen_interface_vtable`, and `goo_panic_iface_conversion` as-is.
- Build the compiler with `make lexer` (binary `bin/goo`). Ignore the pre-existing `_XOPEN_SOURCE`/`write_file` warnings.

**Reference implementations to read before starting:**
- `src/codegen/interface_codegen.c` — `codegen_interface_assert_match` (~593, concrete vtable-identity compare), `codegen_interface_vtable` (~205), `codegen_get_or_emit_type_desc` (~210).
- `src/codegen/expression_codegen.c` — `AST_TYPE_ASSERT` single-return lowering (~215) incl. the `goo_panic_iface_conversion` miss block.
- `src/codegen/function_codegen.c` — comma-ok assertion lowering (~1690–1740, phi over match).
- `src/codegen/statement_codegen.c` — type-switch codegen (~695–748), including the `case nil:` null-vtable test.
- `src/types/expression_checker.c:552` — the assertion interface-target rejection. `src/types/type_checker.c:2321` — the type-switch case satisfaction check.
- `src/runtime/runtime.c:642` — `goo_panic_iface_conversion(const char* iface_name, void* vtable, const char* target_name)`.

**Enumeration contract (used by every task):** a new helper

```c
// Collect declared concrete types implementing `iface`. Walks to the root
// scope from checker->current_scope and returns each variable that NAMES a
// struct type (v->type->kind == TYPE_STRUCT && v->type->data.struct_type.name
// && strcmp(v->name, v->type->data.struct_type.name) == 0) for which
// type_interface_satisfied(checker, iface, T) holds. Caller frees *out.
// Returns count (0 is valid — the assertion then always fails at runtime).
size_t codegen_collect_iface_implementers(TypeChecker* checker, Type* iface, Type*** out);
```

Enumeration runs at **codegen** time (after all types/methods are registered, so
declaration order is irrelevant). The name-equals-type-name test distinguishes a
type declaration (`type Point struct{}` → variable `Point`) from a value of that
type (`var p Point` → variable `p`).

---

### Task 1: Enumeration helper + match/build primitive + `x.(I)` single-return

Deliver the shared primitive and the standalone assertion. This is the load-bearing task; Tasks 2–3 reuse the primitive.

**Files:**
- Modify: `src/types/expression_checker.c` — lift the interface-target rejection (~552).
- Create/modify: `src/codegen/interface_codegen.c` — `codegen_collect_iface_implementers` + `codegen_interface_target_match`.
- Modify: `include/codegen.h` — declare both.
- Modify: `src/codegen/expression_codegen.c` — route interface targets through the primitive in `AST_TYPE_ASSERT`.
- Create: `examples/iface_target_assert.goo` + `.expected.txt`; `examples/iface_target_assert_abort_probe.goo`; wire an abort-probe target into `Makefile` `verify:`.

**Interfaces:**
- Consumes: `type_interface_satisfied` (types.h), `codegen_get_or_emit_type_desc(codegen, checker, T, pointer_form)`, `codegen_interface_vtable(codegen, checker, I, T, pointer_form)`, `goo_panic_iface_conversion`.
- Produces:
  - `size_t codegen_collect_iface_implementers(TypeChecker*, Type* iface, Type*** out)` (see contract above).
  - `LLVMValueRef codegen_interface_target_match(CodeGenerator* codegen, TypeChecker* checker, LLVMValueRef iface_val, Type* target_iface, LLVMValueRef* built_out)` — returns the `i1` match bit and writes the built target-interface value (LLVM `{vtable,data}` struct) to `*built_out`. Self-contained: creates its own nil-guard branch + join, leaves the builder at the join block.

- [ ] **Step 1: Write the failing probes**

`examples/iface_target_assert.goo`:

```goo
package main

import "fmt"

type Speaker interface {
	Speak() string
}

type Dog struct{ name string }

func (d Dog) Speak() string { return d.name + " says woof" }

func main() {
	var x interface{} = Dog{name: "Rex"}
	s := x.(Speaker)
	fmt.Println(s.Speak())
}
```

`examples/iface_target_assert.expected.txt`:

```
Rex says woof
```

`examples/iface_target_assert_abort_probe.goo` (a value NOT implementing Speaker):

```goo
package main

type Speaker interface {
	Speak() string
}

func main() {
	var x interface{} = 42
	_ = x.(Speaker)   // panics: interface conversion: int64 is not Speaker
}
```

- [ ] **Step 2: Confirm they fail for the right reason**

```bash
./bin/goo examples/iface_target_assert.goo -o /tmp/ita 2>&1 | head -2
```
Expected: `type assertion to an interface type is not supported in v1`.

- [ ] **Step 3: Lift the assertion typecheck rejection**

In `src/types/expression_checker.c` (~552), remove the `target_type->kind == TYPE_INTERFACE` rejection block. For an interface target, accept the assertion without the concrete-satisfaction check that follows (that check is for concrete targets). Structure:

```c
if (target_type->kind == TYPE_INTERFACE) {
    // Interface target: a runtime-checked assertion (Go semantics). Always
    // well-formed on an interface operand; success is decided at runtime by
    // enumerating implementers in codegen. Do NOT require static satisfaction.
    expr->node_type = target_type;
    return target_type;
}
// ... existing concrete-target path (type_interface_satisfied gate) unchanged ...
```

- [ ] **Step 4: Add the enumeration helper**

In `src/codegen/interface_codegen.c`, add `codegen_collect_iface_implementers` per the contract above: walk `checker->current_scope` to the root (`while (s->parent) s = s->parent;`), iterate `s->variables` (linked list via `->next`), and for each variable naming a struct type, call `type_interface_satisfied(checker, iface, v->type, &m, &r)`; collect matches into a malloc'd array. Declare it in `include/codegen.h`.

- [ ] **Step 5: Add the match/build primitive**

In `src/codegen/interface_codegen.c`, add `codegen_interface_target_match`. Structure (mirror the `case nil:` null-guard in `statement_codegen.c` and the `insertvalue` boxing in `codegen_interface_box`):

```c
// Extract vtab = iface_val field 0, data = field 1.
// entry: is_null = icmp eq vtab, null;  condbr is_null, join(false), nonnull.
// nonnull block:
//   desc_have = load ptr from vtab[0]  (GEP element 0 of the vtable array)
//   match = false (i1);  built = zero target-iface value ({null,null})
//   impls = codegen_collect_iface_implementers(checker, target_iface, &arr)
//   for each T in impls (deterministic order):
//     form = pointer_form if T is *struct with nameable pointee else 0
//     desc_T = codegen_get_or_emit_type_desc(codegen, checker, base_of(T), form)
//     eq = icmp eq desc_have, desc_T
//     vt_TI = codegen_interface_vtable(codegen, checker, target_iface, base_of(T), form)
//     iv_T  = insertvalue {vt_TI, data}          // side-effect-free
//     built = select(eq, iv_T, built)
//     match = or(match, eq)
//   br join
// join: match_phi = phi(false from entry, match from nonnull)
//       built_phi = phi(zero from entry, built from nonnull)
// *built_out = built_phi;  return match_phi.
```

`base_of(T)` and `form` mirror `codegen_interface_assert_match`'s value-vs-pointer
form selection (interface_codegen.c ~611–617). The zero target-iface value is
`LLVMConstNull(codegen_type_to_llvm(target_iface))`.

- [ ] **Step 6: Route interface targets in the single-return assertion**

In `src/codegen/expression_codegen.c` `AST_TYPE_ASSERT` (~215), branch on `target->kind`. Concrete → existing path unchanged. Interface →

```c
LLVMValueRef built = NULL;
LLVMValueRef match = codegen_interface_target_match(codegen, checker, iface_val, target, &built);
// miss -> panic (reuse the existing goo_panic_iface_conversion block, with
//   iface_name = target interface's name, vtable = iface_val field 0);
// match -> the result value IS `built` (already the target-interface value).
```
Build `match_bb`/`miss_bb`; in `miss_bb` call `goo_panic_iface_conversion(target_name_global, vtab, target_name_global)` + `unreachable` (the dynamic name comes from `vtab`'s descriptor at runtime; `iface_name`/`target_name` globals are the target interface's name). In `match_bb`, return `value_info_new(NULL, built, target)`.

- [ ] **Step 7: Build, run probes GREEN**

```bash
make lexer 2>&1 | grep -iE "error:"; echo done
./bin/goo examples/iface_target_assert.goo -o /tmp/ita && /tmp/ita | diff - examples/iface_target_assert.expected.txt && echo "assert OK"
# abort probe: compiles, runs, non-zero exit, message names the dynamic type
./bin/goo examples/iface_target_assert_abort_probe.goo -o /tmp/itab && (/tmp/itab 2>/tmp/itab.err; test $? -ne 0) && grep -q "is not Speaker" /tmp/itab.err && echo "abort OK"
```

- [ ] **Step 8: Wire the abort probe + full gate + commit**

Add an `iface-target-assert-abort-probe` target to the `Makefile` mirroring `typeassert-abort-probe` (compile, run, assert non-zero exit, grep `is not Speaker`), and add it to the `verify:` dependency list.

```bash
eval "$(opam env --switch=default)"
bash scripts/run_golden.sh 2>&1 | tail -2   # +1 golden, 0 failures
make test 2>&1 | tail -2
make verify 2>&1 | tail -2                    # ALL GREEN
git add src/types/expression_checker.c src/codegen/interface_codegen.c include/codegen.h \
        src/codegen/expression_codegen.c Makefile examples/iface_target_assert.* \
        examples/iface_target_assert_abort_probe.goo
git commit -m "feat(rtti): assert to an interface target — x.(I)

Closed-world enumeration: codegen_collect_iface_implementers finds concrete
implementers of I; codegen_interface_target_match compares x's dynamic
descriptor (box.vtable[0]) against each and builds {(T,I) vtable, data} on
match. Single-return x.(I) panics on miss (dynamic type name via #137).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: comma-ok `v, ok := x.(I)`

Reuse the primitive in the comma-ok assertion path.

**Files:**
- Modify: `src/codegen/function_codegen.c` — comma-ok assertion lowering (~1690–1740).
- Create: `examples/iface_target_commaok.goo` + `.expected.txt`.

**Interfaces:**
- Consumes: `codegen_interface_target_match` (Task 1).
- Produces: no new symbols.

- [ ] **Step 1: Write the failing probe**

`examples/iface_target_commaok.goo`:

```goo
package main

import "fmt"

type Speaker interface {
	Speak() string
}

type Dog struct{}

func (d Dog) Speak() string { return "woof" }

func try(x interface{}) {
	if s, ok := x.(Speaker); ok {
		fmt.Println("speaks:", s.Speak())
	} else {
		fmt.Println("not a speaker")
	}
}

func main() {
	try(Dog{})
	try(42)
	var n interface{}
	try(n)
}
```

`examples/iface_target_commaok.expected.txt`:

```
speaks: woof
not a speaker
not a speaker
```

- [ ] **Step 2: Confirm failure**

```bash
./bin/goo examples/iface_target_commaok.goo -o /tmp/ico 2>&1 | head -2
```
Expected: a codegen/typecheck error on the interface-target comma-ok (concrete comma-ok path can't unbox an interface target).

- [ ] **Step 3: Route interface targets in the comma-ok path**

In `function_codegen.c` (~1690), where the comma-ok assertion currently calls `codegen_interface_assert_match` + `codegen_interface_assert_unbox` + phi, branch on `target->kind`. Interface target →

```c
LLVMValueRef built = NULL;
LLVMValueRef match = codegen_interface_target_match(codegen, checker, iface_val, target, &built);
// aggregate {v, ok} = { built, match } — built is already the zero target-iface
// value when match is false (the primitive's join phi), so NO separate unbox or
// zero-phi is needed here; skip the ta.load/ta.done block dance for this arm.
```
Assemble the `{target_llvm, i1}` aggregate from `built` and `match` directly.

- [ ] **Step 4: Build, run probe GREEN**

```bash
make lexer 2>&1 | grep -iE "error:"; echo done
./bin/goo examples/iface_target_commaok.goo -o /tmp/ico && /tmp/ico | diff - examples/iface_target_commaok.expected.txt && echo "commaok OK"
```

- [ ] **Step 5: Full gate + commit**

```bash
eval "$(opam env --switch=default)"
bash scripts/run_golden.sh 2>&1 | tail -2
make verify 2>&1 | tail -2
git add src/codegen/function_codegen.c examples/iface_target_commaok.*
git commit -m "feat(rtti): comma-ok assert to an interface target — v, ok := x.(I)

Interface-target comma-ok reuses codegen_interface_target_match; {v, ok} =
{built, match}. False (incl. nil interface) yields the zero interface value.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: type-switch `case I:`

Allow interface cases in a type switch, binding `v` to the built target value.

**Files:**
- Modify: `src/types/type_checker.c` — type-switch case satisfaction check (~2321).
- Modify: `src/codegen/statement_codegen.c` — interface-case codegen in the type switch (~710–810).
- Create: `examples/iface_target_switch.goo` + `.expected.txt`.

**Interfaces:**
- Consumes: `codegen_interface_target_match` (Task 1).
- Produces: no new symbols.

- [ ] **Step 1: Write the failing probe**

`examples/iface_target_switch.goo`:

```goo
package main

import "fmt"

type Speaker interface {
	Speak() string
}

type Dog struct{}

func (d Dog) Speak() string { return "woof" }

func classify(x interface{}) {
	switch v := x.(type) {
	case int:
		fmt.Println("int", v)
	case Speaker:
		fmt.Println("speaker:", v.Speak())
	case nil:
		fmt.Println("nil")
	default:
		fmt.Println("other")
	}
}

func main() {
	classify(7)
	classify(Dog{})
	var n interface{}
	classify(n)
	classify("x")
}
```

`examples/iface_target_switch.expected.txt`:

```
int 7
speaker: woof
nil
other
```

- [ ] **Step 2: Confirm failure**

```bash
./bin/goo examples/iface_target_switch.goo -o /tmp/its 2>&1 | head -2
```
Expected: rejection on the `case Speaker:` interface case.

- [ ] **Step 3: Allow interface cases in the typecheck**

In `type_checker.c` (~2321), the case loop calls `type_interface_satisfied(iface_type, case_type)` and rejects on failure. Add: if `case_type->kind == TYPE_INTERFACE`, skip that rejection (runtime-checked); the bound `v` in that clause has type `case_type`. Ensure the clause's `v` binding type resolves to the interface for the interface case (single-type case binds `v` to the case type — the existing `single_concrete` path; extend it to interface case types).

- [ ] **Step 4: Interface-case codegen**

In `statement_codegen.c`'s type-switch codegen, where each case's match is built: for a case whose resolved `node_type->kind == TYPE_INTERFACE`, use `codegen_interface_target_match(codegen, checker, iface_val, case_type, &built)` for the match bit (instead of `codegen_interface_assert_match`), and bind the clause's `v` to `built` (the target-interface value) rather than the unboxed concrete. Concrete and `nil` cases stay on their existing paths. Interface cases test in source order like the others (first match wins).

- [ ] **Step 5: Build, run probe GREEN**

```bash
make lexer 2>&1 | grep -iE "error:"; echo done
./bin/goo examples/iface_target_switch.goo -o /tmp/its && /tmp/its | diff - examples/iface_target_switch.expected.txt && echo "switch OK"
```

- [ ] **Step 6: Full gate + commit**

```bash
eval "$(opam env --switch=default)"
bash scripts/run_golden.sh 2>&1 | tail -2
make verify 2>&1 | tail -2
git add src/types/type_checker.c src/codegen/statement_codegen.c examples/iface_target_switch.*
git commit -m "feat(rtti): type switch to an interface case — case I:

Interface cases in switch x.(type) reuse codegen_interface_target_match; v
binds to the built target-interface value. Interface/concrete/nil cases test
in source order (first match wins, Go-faithful).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: edge cases + regression hardening

Cover the sharp edges the primitive must get right, and confirm no regression to the shared machinery.

**Files:**
- Create: `examples/iface_target_multi.goo` + `.expected.txt`; `examples/iface_target_ptr.goo` + `.expected.txt`.

**Interfaces:** consumes Tasks 1–3; no new symbols.

- [ ] **Step 1: Write the edge probes**

`examples/iface_target_multi.goo` — a type implementing two interfaces, asserted to each:

```goo
package main

import "fmt"

type Reader interface{ Read() int }
type Writer interface{ Write() int }

type File struct{ n int }

func (f File) Read() int  { return f.n }
func (f File) Write() int { return f.n * 2 }

func main() {
	var x interface{} = File{n: 21}
	r := x.(Reader)
	w := x.(Writer)
	fmt.Println(r.Read(), w.Write())
}
```

`examples/iface_target_multi.expected.txt`:

```
21 42
```

`examples/iface_target_ptr.goo` — a pointer-receiver implementer:

```goo
package main

import "fmt"

type Counter interface{ Get() int }

type C struct{ n int }

func (c *C) Get() int { return c.n }

func main() {
	var x interface{} = &C{n: 5}
	if v, ok := x.(Counter); ok {
		fmt.Println(v.Get())
	}
}
```

`examples/iface_target_ptr.expected.txt`:

```
5
```

- [ ] **Step 2: Confirm they pass (Tasks 1–3 should already cover them)**

```bash
for p in iface_target_multi iface_target_ptr; do
  ./bin/goo examples/$p.goo -o /tmp/$p && /tmp/$p | diff - examples/$p.expected.txt && echo "$p OK"
done
```
If either fails, the primitive's form selection (value vs pointer) or multi-interface enumeration needs a fix — resolve before committing.

- [ ] **Step 3: Full regression gate + commit**

```bash
eval "$(opam env --switch=default)"
bash scripts/run_golden.sh 2>&1 | tail -2   # all new probes + existing, 0 failures
make test 2>&1 | tail -2
./scripts/grammar-tripwire.sh
make verify 2>&1 | tail -2                    # ALL GREEN
git add examples/iface_target_multi.* examples/iface_target_ptr.*
git commit -m "test(rtti): interface-target edge probes — multi-interface + pointer receiver

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage** (against `2026-07-06-interface-target-rtti-design.md`):
- Lift assertion rejection → Task 1 Step 3. Lift type-switch case rejection → Task 3 Step 3.
- Enumeration helper → Task 1 Step 4 (contract at top).
- Match/build primitive → Task 1 Step 5.
- `x.(I)` single-return + panic → Task 1 Step 6. comma-ok → Task 2. type-switch `case I:` → Task 3.
- Nil interface → primitive's null-vtable guard (Task 1 Step 5), exercised in Task 2 (`try(n)`) and Task 3 (`classify(n)`).
- Source-order precedence → Task 3 (int before Speaker; existing sequential case tests).
- Multi-interface + pointer receiver → Task 4.
- Panic dynamic name → Task 1 Step 6 (reuses #137 `goo_panic_iface_conversion`).
- Regression net → each task's `make verify`; Task 4 the final gate.
- Open question 1 (enumeration timing) → RESOLVED: at codegen, all decls registered.
- Open question 3 (panic wording) → RESOLVED: `is not I`, reusing the concrete message.

**Placeholder scan:** the primitive body (Task 1 Step 5) and interface-case codegen (Task 3 Step 4) are given as precise structured pseudocode citing the exact templates (`case nil:` null-guard, `codegen_interface_box` insertvalue, `codegen_interface_assert_match` form selection) rather than literal LLVM-C, because assembling ~60 lines of LLVM-C API calls verbatim invites transcription errors; every operation and its source template is named. All other steps carry literal code/commands.

**Type consistency:** `codegen_interface_target_match(codegen, checker, iface_val, target_iface, &built)` → `i1` and `codegen_collect_iface_implementers(checker, iface, &out)` → `size_t` are used identically in Tasks 1–3. `built` is the target-interface `{vtable,data}` LLVM value in all three lowerings; the zero value is `LLVMConstNull` of the target interface's LLVM type throughout.
