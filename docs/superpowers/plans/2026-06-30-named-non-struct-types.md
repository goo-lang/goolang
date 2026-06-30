# Named Non-Struct Types Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make named non-struct types (`type IntSlice []int`, `type MyInt int`) first-class — methods, selector dispatch, interface boxing, and composite literals — so a hand-ported `sort` compiles and runs.

**Architecture:** Stamp the existing generic `Type.name` onto named non-struct types in `type_check_type_decl`; each downstream site (selector typecheck, interface vtable, composite-literal typecheck/codegen) then recognizes a named non-struct type instead of assuming struct. No new type kind, no `TYPE_NAMED` wrapper.

**Tech Stack:** C23, LLVM-C 22, bison/flex frontend, `scripts/run_golden.sh` golden probes, `make verify` gate.

## Global Constraints

- `golangci-lint`-equivalent here = `make verify` ALL GREEN (incl. ccomp via `eval "$(opam env --switch=default)"`) + `make test` 76/1 + golden suite, per commit.
- No naked returns / silent failures; errors via `type_error`/`codegen_error`.
- Golden probes auto-discovered: `examples/<name>.goo` + `examples/<name>.expected.txt`.
- Build the compiler with `make bin/goo`; run a probe with `./bin/goo -o /tmp/x examples/<name>.goo && /tmp/x`.
- Spec: `docs/superpowers/specs/2026-06-30-named-non-struct-types-design.md`.
- The tuple-index `Swap` (`s[i],s[j]=s[j],s[i]`) is a SEPARATE deferred milestone — port code uses a temp-based Swap.

---

### Task 1: Facet 0 — stamp the generic name on named non-struct types

**Files:**
- Modify: `src/types/type_checker.c` (in `type_check_type_decl`, after the interface name-stamp at ~851, before the alias registration)
- Test: `examples/named_method_probe.goo` + `.expected.txt`

**Interfaces:**
- Produces: a named non-struct `Type` now has `->name` set; `type_receiver_name(t)` returns the declared name; a method `func (s IntSlice) M()` registers as `IntSlice__M`.

- [ ] **Step 1: Write the failing probe** — `examples/named_method_probe.goo`:

```go
// named_method_probe: a method on a named slice type, called directly.
package main

import "fmt"

type IntSlice []int

func (s IntSlice) First() int { return s[0] }

func main() {
	var s IntSlice = []int{7, 8, 9}
	fmt.Println(s.First()) // 7
}
```

`examples/named_method_probe.expected.txt`:

```
7
```

- [ ] **Step 2: Verify it fails**

Run: `./bin/goo -o /tmp/nm examples/named_method_probe.goo`
Expected: `Selector on non-struct, non-package type` (Task 2 fixes the selector; this probe also needs Task 1's naming to register the method). NOTE: this probe is the Task 2 acceptance; Task 1 alone is verified by Step 4 below.

- [ ] **Step 3: Implement the name stamp** — in `src/types/type_checker.c`, immediately after the interface name-stamp block (the `if (resolved->kind == TYPE_INTERFACE ...)` ending ~851), add:

```c
    // Stamp the declared name onto a named NON-struct/enum/interface type
    // (e.g. `type IntSlice []int`, `type MyInt int`). Those kinds carry no
    // kind-specific name field, so use the generic Type.name — which
    // type_receiver_name() already falls back to — enabling method mangling
    // (`IntSlice__M`), selector dispatch, and interface boxing on named types.
    if (resolved->kind != TYPE_STRUCT && resolved->kind != TYPE_ENUM &&
        resolved->kind != TYPE_INTERFACE && !resolved->name) {
        resolved->name = strdup(td->name);
    }
```

- [ ] **Step 4: Verify naming works (freshness check)** — add a temporary assert-style probe `examples/named_underlying_probe.goo` proving the underlying slice ops still work AND a plain `[]int` is unaffected (catches the shared-builtin risk from the spec):

```go
package main

import "fmt"

type IntSlice []int

func main() {
	var s IntSlice = []int{1, 2, 3}
	var t []int = []int{4, 5}
	fmt.Println(len(s)) // 3
	fmt.Println(len(t)) // 2  (plain []int must be unaffected by stamping)
}
```

`examples/named_underlying_probe.expected.txt`:
```
3
2
```

Run: `./bin/goo -o /tmp/nu examples/named_underlying_probe.goo && /tmp/nu`
Expected: `3` then `2`. If `len(t)` is wrong or the compiler crashes, `type_from_ast` shares a builtin slice Type — clone before stamping: replace the stamp with `Type* named = type_clone(resolved); named->name = strdup(td->name); resolved = named;` (check `types.c` for an existing `type_copy`/`type_clone`; if absent, only stamp when `resolved` is freshly allocated — confirm via a quick `type_from_ast` read).

- [ ] **Step 5: Run the full gate**

Run: `eval "$(opam env --switch=default)" && make bin/goo && make test-golden && make test`
Expected: golden all pass (incl. the two new probes), `make test` 76/1.

- [ ] **Step 6: Commit**

```bash
git add src/types/type_checker.c examples/named_underlying_probe.goo examples/named_underlying_probe.expected.txt
git commit -m "feat(types): stamp generic name on named non-struct types (Facet 0)"
```

(Hold `named_method_probe` for Task 2 — it needs the selector fix.)

---

### Task 2: Facet 1 — selector method resolution on named non-struct types

**Files:**
- Modify: `src/types/expression_checker.c:1231` (the final `Selector on non-struct` error — add a named-method lookup before it)
- Verify (no change expected): `src/codegen/call_codegen.c` method-call path (uses `type_receiver_name` + mangled call, already general)
- Test: `examples/named_method_probe.goo` (from Task 1)

**Interfaces:**
- Consumes: `Type.name` set by Task 1; `type_method_mangled_name(name, sel)` and `type_checker_lookup_variable(checker, mangled)` (used identically in the struct path at expression_checker.c:1199-1208).
- Produces: `s.Method(args)` on a named non-struct receiver typechecks to the method's function type.

- [ ] **Step 1: Confirm the probe still fails at the selector**

Run: `./bin/goo -o /tmp/nm examples/named_method_probe.goo`
Expected: `Selector on non-struct, non-package type` at the `s.First()` line.

- [ ] **Step 2: Implement named-method selector resolution** — in `src/types/expression_checker.c`, replace the final fallthrough:

```c
    type_error(checker, expr->pos, "Selector on non-struct, non-package type");
    return NULL;
```

with:

```c
    // Named non-struct type (e.g. `type IntSlice []int`) method call: resolve
    // `Name__selector` exactly like the struct method path above (1199-1208).
    if (expr_type->name) {
        char* mangled = type_method_mangled_name(expr_type->name, selector->selector);
        Variable* m = mangled ? type_checker_lookup_variable(checker, mangled) : NULL;
        free(mangled);
        if (m && m->type && m->type->kind == TYPE_FUNCTION) {
            expr->node_type = m->type;
            return m->type;
        }
    }

    type_error(checker, expr->pos, "Selector on non-struct, non-package type");
    return NULL;
```

- [ ] **Step 3: Verify the probe compiles and runs**

Run: `make bin/goo && ./bin/goo -o /tmp/nm examples/named_method_probe.goo && /tmp/nm`
Expected: `7`. If it typechecks but crashes/miscompiles at codegen, read the struct method-call path in `src/codegen/call_codegen.c` (around the `type_receiver_name(recv_type)` + `type_method_mangled_name` call, ~446) and confirm it does not gate on `recv_type->kind == TYPE_STRUCT`; if it does, broaden that guard to also accept a named non-struct (`recv_type->name != NULL`). Show the exact edit in the commit.

- [ ] **Step 4: Add a named-int method probe** (proves generality beyond slices) — `examples/named_int_method_probe.goo`:

```go
package main

import "fmt"

type Celsius int

func (c Celsius) Doubled() int { return int(c) + int(c) }

func main() {
	var t Celsius = 21
	fmt.Println(t.Doubled()) // 42
}
```

`.expected.txt`: `42`. If `int(c)` conversion is unsupported, simplify the body to `return 2 * 2` is NOT allowed (must use the receiver) — instead return a field-free constant computed from the receiver via a supported op; if no receiver op compiles, note it and keep only the slice probe, recording the int-method gap as a follow-up.

- [ ] **Step 5: Run the full gate** (`make verify` + `make test`), expect ALL GREEN, golden +probes.

- [ ] **Step 6: Commit**

```bash
git add src/types/expression_checker.c examples/named_method_probe.* examples/named_int_method_probe.*
git commit -m "feat(types): selector method dispatch on named non-struct types (Facet 1)"
```

---

### Task 3: Facet 2 — interface boxing + dispatch of named non-struct values (segfault)

**Files:**
- Investigate/Modify: `src/codegen/interface_codegen.c` (`codegen_interface_box`, `build_thunk`, `codegen_interface_dispatch`)
- Test: `examples/named_iface_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: Task 1 naming (vtable mangling now resolves `IntSlice__M`); Task 2 dispatch.
- Produces: a named non-struct value boxed into an interface dispatches correctly.

- [ ] **Step 1: Write the failing probe** — `examples/named_iface_probe.goo`:

```go
// named_iface_probe: a named slice type implements an interface and is
// dispatched through it — the sort.Interface shape.
package main

import "fmt"

type Sizer interface {
	Size() int
}

type IntSlice []int

func (s IntSlice) Size() int { return len(s) }

func report(z Sizer) {
	fmt.Println(z.Size())
}

func main() {
	var s IntSlice = []int{4, 5, 6}
	report(s) // 3
}
```

`.expected.txt`: `3`.

- [ ] **Step 2: Run it and capture the failure mode**

Run: `./bin/goo -o /tmp/ni examples/named_iface_probe.goo && /tmp/ni; echo "rc=$?"`
Expected (pre-fix): compiles, then `rc=139` (segfault), OR a codegen error.

- [ ] **Step 3: Root-cause with systematic-debugging** (REQUIRED SUB-SKILL: superpowers:systematic-debugging). Get a backtrace:

Run: `gdb -q -batch -ex run -ex bt --args ./bin/goo -o /tmp/ni examples/named_iface_probe.goo 2>&1 | tail -20`

Likely sites: `codegen_interface_box` (`LLVMSizeOf`/`LLVMBuildStore` of a named slice — a 3-field `goo_slice_t`, not a scalar — see [[goolang-slice-abi-by-pointer]]), or `build_thunk` loading the receiver. The named slice's `codegen_type_to_llvm` must yield the underlying slice struct type; if `codegen_type_to_llvm` returns NULL or a wrong type for a *named* slice, that is the bug. Form ONE hypothesis, state it, fix minimally.

- [ ] **Step 4: Implement the single root-cause fix.** Most probable: `codegen_type_to_llvm` (src/codegen/type_mapping.c) does not handle a `TYPE_SLICE` carrying a `name` (or a named type generally) — make it map a named non-struct type by its underlying kind (ignore `->name` for layout). Show the exact edit. Do NOT bundle unrelated changes.

- [ ] **Step 5: Verify**

Run: `make bin/goo && ./bin/goo -o /tmp/ni examples/named_iface_probe.goo && /tmp/ni; echo rc=$?`
Expected: `3`, rc=0.

- [ ] **Step 6: Full gate + commit**

```bash
eval "$(opam env --switch=default)" && make verify && make test
git add src/codegen/ examples/named_iface_probe.*
git commit -m "fix(codegen): box/dispatch named non-struct values through interfaces (Facet 2)"
```

---

### Task 4: Facet 3 — composite literals for named slice/map/array

**Files:**
- Modify: `src/types/expression_checker.c:223-229` (`type_check_struct_literal` — redirect named slice/map/array to the underlying composite path)
- Modify: `src/codegen/composite_codegen.c` (`codegen_generate_struct_lit` — lower a named slice/map/array literal via the underlying slice/map path)
- Test: `examples/named_composite_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: Task 1 naming; the existing slice-literal typecheck (`type_check` AST_SLICE_LITERAL path, expression_checker.c ~100-160) and slice-literal codegen (`codegen_generate_slice_lit`, composite_codegen.c ~847).
- Produces: `IntSlice{...}` typechecks/lowers as a slice literal of the underlying element type.

- [ ] **Step 1: Write the failing probe** — `examples/named_composite_probe.goo`:

```go
// named_composite_probe: composite literal using a named slice type name.
package main

import "fmt"

type IntSlice []int

func main() {
	s := IntSlice{3, 1, 2}
	fmt.Println(s[0]) // 3
	fmt.Println(s[2]) // 2
}
```

`.expected.txt`:
```
3
2
```

- [ ] **Step 2: Verify failure**

Run: `./bin/goo -o /tmp/nc examples/named_composite_probe.goo`
Expected: `'IntSlice' is not a struct type, cannot use composite literal`.

- [ ] **Step 3: Typecheck redirect** — in `src/types/expression_checker.c`, replace the non-struct rejection at 223-229 with a branch that, when `struct_type->kind` is `TYPE_SLICE`/`TYPE_MAP`/`TYPE_ARRAY`, validates the literal's `field_values` as elements of the underlying type and stamps `expr->node_type = struct_type` (reuse the element-compat checks from the AST_SLICE_LITERAL path — factor a shared helper `check_slice_elements(checker, elements, want_elem, pos)` if the logic is non-trivial, to keep DRY). Named `int`/`func` keep the existing rejection. Show the exact code in the commit; if the struct-literal node stores values in `field_values` while slice codegen expects `elements`, normalize in the codegen step (Step 4), not here.

- [ ] **Step 4: Codegen redirect** — in `src/codegen/composite_codegen.c` `codegen_generate_struct_lit`, when the resolved literal type is a named `TYPE_SLICE`/`TYPE_MAP`/`TYPE_ARRAY`, build the value via the underlying path (call into `codegen_generate_slice_lit`'s logic with the literal's values as elements and the named type's element type). Factor the slice-lowering core into a helper both entry points call if needed. Show exact code.

- [ ] **Step 5: Verify**

Run: `make bin/goo && ./bin/goo -o /tmp/nc examples/named_composite_probe.goo && /tmp/nc`
Expected: `3` then `2`.

- [ ] **Step 6: Full gate + commit**

```bash
eval "$(opam env --switch=default)" && make verify && make test
git add src/types/expression_checker.c src/codegen/composite_codegen.c examples/named_composite_probe.*
git commit -m "feat(types,codegen): composite literals for named slice/map/array (Facet 3)"
```

---

### Task 5: Capstone — the `sort` port golden probe

**Files:**
- Test: `examples/sort_named_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: Tasks 1-4 (naming, selector dispatch, interface boxing, composite literal).

- [ ] **Step 1: Write the capstone probe** — `examples/sort_named_probe.goo` (temp-based Swap; the tuple-index Swap is a separate deferred milestone):

```go
package main

import "fmt"

type Interface interface {
	Len() int
	Less(i int, j int) bool
	Swap(i int, j int)
}

func insertionSort(items Interface, a int, b int) {
	for i := a + 1; i < b; i++ {
		for j := i; j > a; j-- {
			if items.Less(j, j-1) {
				items.Swap(j, j-1)
			}
		}
	}
}

func Sort(items Interface) { insertionSort(items, 0, items.Len()) }

type IntSlice []int

func (s IntSlice) Len() int               { return len(s) }
func (s IntSlice) Less(i int, j int) bool { return s[i] < s[j] }
func (s IntSlice) Swap(i int, j int) {
	tmp := s[i]
	s[i] = s[j]
	s[j] = tmp
}

func main() {
	s := IntSlice{3, 1, 2}
	Sort(s)
	fmt.Println(s[0]) // 1
	fmt.Println(s[1]) // 2
	fmt.Println(s[2]) // 3
}
```

`.expected.txt`:
```
1
2
3
```

- [ ] **Step 2: Run it**

Run: `./bin/goo -o /tmp/sn examples/sort_named_probe.goo && /tmp/sn`
Expected: `1 2 3`. If it fails, the failing facet's task is incomplete — return to it (do NOT patch around it here).

- [ ] **Step 3: Full gate + commit**

```bash
eval "$(opam env --switch=default)" && make verify && make test
git add examples/sort_named_probe.*
git commit -m "test(golden): TinyGo-style sort port runs end-to-end (named-types capstone)"
```

- [ ] **Step 4: Update memory** — append to `goolang-v1-roadmap` memory: named-non-struct-types milestone complete; sort port runs; remaining sort-faithfulness gap = tuple-index Swap (deferred grammar milestone).

---

## Self-Review

- **Spec coverage:** Facet 0 → Task 1; Facet 1 → Task 2; Facet 2 → Task 3; Facet 3 → Task 4; capstone sort → Task 5; conversions explicitly out of scope (spec + plan agree). ✓
- **Placeholder scan:** Tasks 3 and 4 implementation steps are investigation-framed (segfault root-cause; struct-lit→slice-lit bridging) because the exact edit depends on a gdb backtrace / the slice-lit internal shape — each gives the precise file, the most-probable site, the hypothesis, and a concrete acceptance, which is the honest maximum before the investigation. All test code is complete. ✓
- **Type consistency:** `type_receiver_name`, `type_method_mangled_name`, `type_checker_lookup_variable`, `codegen_type_to_llvm`, `codegen_generate_slice_lit`, `codegen_interface_box` used consistently with their real signatures. ✓
- **Ordering:** Task 1 (naming) is the keystone every later task consumes; 2/3/4 are independent given 1; 5 is the capstone. ✓
