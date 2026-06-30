# Error Bridge + strconv.Atoi Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `(int,error) == !int` usable: `n, err := <!T>` destructures a Goolang error union into `(value, ?error)`, so `n, err := strconv.Atoi(s); if err != nil { … }` compiles and runs.

**Architecture:** Special-case a `TYPE_ERROR_UNION` RHS in the 2-target destructure (typecheck + codegen) — bind `n : value-arm`, `err : error` (`?*int8`), extract the value arm (like `catch`) and build `err` nil ⟺ !is_error. Add `strconv.Atoi → !int`, `errors.New → error`, and a round-trip capstone.

**Tech Stack:** C23, LLVM-C, error unions (`TYPE_ERROR_UNION`), nullable (`?error` = `?*int8`).

## Global Constraints

- Gate = `make verify` ALL GREEN (incl. ccomp via `eval "$(opam env --switch=default)"`) + `make test` 76/1 + golden, no regressions.
- Commits FAIL to sign — `git commit --no-gpg-sign`. No naked returns / silent failures.
- Golden probes auto-discovered: `examples/<name>.goo` + `.expected.txt`.
- References: `!T` layout `{i1 is_error, union}` — `ExtractValue 0` = is_error (error_union_codegen.c:85); `catch`/`try` value unwrap in error_union_codegen.c (mirror for `n`). Destructure typecheck type_checker.c:604 (`name_count==2 && is_short_decl`) + `=` multi-assign ~961; destructure codegen function_codegen.c:588. `error` type = `type_nullable(type_pointer(int8))` (type_checker.c:1538). `codegen_create_error_union_success/_error` (error_union_codegen.c:11/56). `goo_string_to_int` (runtime, shipped). Spec: `docs/superpowers/specs/2026-06-30-error-bridge-design.md`.

---

### Task 1: The `!T → (value, ?error)` destructure bridge

**Files:**
- Modify: `src/types/type_checker.c` (multi-LHS-short-decl ~604; the `=` multi-assign ~961 if it shares the path)
- Modify: `src/codegen/function_codegen.c` (destructure ~588)
- Test: `examples/erru_destructure_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: `TYPE_ERROR_UNION` (`type->data.error_union.value_type`/`.error_type`); the value-arm extraction `catch`/`try` use; `type_nullable`/the `error` builtin type.
- Produces: `n, err := <!T expr>` binds `n : value_type`, `err : error` correctly.

- [ ] **Step 1: Write the failing probe** — `examples/erru_destructure_probe.goo`:

```go
package main
import "fmt"
func mk(b bool) !int {
	if b { return 7 }
	return error("bad")
}
func main() {
	n, err := mk(true)
	if err != nil { fmt.Println("FAIL ok-path") } else { fmt.Println(n) } // 7
	m, e2 := mk(false)
	if e2 != nil { fmt.Println("caught") } else { fmt.Println(m) }         // caught
}
```
`examples/erru_destructure_probe.expected.txt`:
```
7
caught
```
(Confirm the `error("…")` builtin syntax for constructing the error arm by reading an existing erru probe, e.g. `examples/erru_error_probe.goo`; adjust if the constructor differs.)

- [ ] **Step 2: Verify it's wrong (not the expected output)**

Run: `make bin/goo && ./bin/goo -o /tmp/ed examples/erru_destructure_probe.goo && /tmp/ed`
Expected: WRONG output (the current generic-struct destructure reads raw union fields — `n` gets the is_error flag, so the ok branch misfires / prints a wrong number), OR a compile error. Document the actual wrong behavior.

- [ ] **Step 3: Typecheck — bind the two targets from the `!T` arms.** In `type_checker.c` multi-LHS-short-decl (~604), BEFORE the generic struct-destructure handling: if the single RHS expression's type is `TYPE_ERROR_UNION`, set `names[0]`'s type to `rhs->data.error_union.value_type` and `names[1]`'s type to the `error` builtin (`type_nullable(type_pointer(type_checker_get_builtin(checker, TYPE_INT8)))`, matching type_checker.c:1538). Register both variables with those types. Show the code. Apply the same to the `=` multi-assign path (~961) if 2-target `=` from a `!T` is in scope (else note it deferred).

- [ ] **Step 4: Codegen — extract value arm + build `?error`.** In `function_codegen.c` destructure (~588), BEFORE the generic `ExtractValue 0/1` loop: if the RHS `goo_type` is `TYPE_ERROR_UNION`:
  - `is_error = LLVMBuildExtractValue(rhs, 0, "is_error")`.
  - `n` (target0) = the unwrapped value arm — mirror how `catch`/`try` extract the value (read error_union_codegen.c's value-unwrap; typically extract union field 1 and interpret as the value type). Store into target0's alloca (type = value_type).
  - `err` (target1) = a `?error` `{i1 is_null, *int8}`: `is_null = NOT is_error`; pointer = `LLVMBuildSelect(is_error, <non-null i8* — e.g. an inttoptr 1 or a global marker>, null)`. Build the nullable struct (mirror `codegen_create_nullable_with_value` / the nullable layout). Store into target1's alloca (type = error).
  Show the code. Keep the generic struct-destructure path unchanged for non-`!T` RHS.

- [ ] **Step 5: Verify the probe + regression**

Run: `make bin/goo && ./bin/goo -o /tmp/ed examples/erru_destructure_probe.goo && /tmp/ed`
Expected: `7` then `caught`.
Regression: `func two()(int,int){return 1,2}` + `a,b:=two()` still prints `1 2` (write to /tmp, run). And an existing `catch` probe still passes.

- [ ] **Step 6: Full gate + commit**

```bash
eval "$(opam env --switch=default)" && make verify && make test
git add src/types/type_checker.c src/codegen/function_codegen.c examples/erru_destructure_probe.*
git commit --no-gpg-sign -m "feat(types,codegen): destructure !T into (value, ?error) — n, err := <!T>"
```

---

### Task 2: strconv.Atoi → !int

**Files:**
- Modify: `src/types/expression_checker.c` (register `strconv.Atoi`)
- Modify: `src/codegen/call_codegen.c` (`codegen_generate_atoi_call` building the `!int`; route it)
- Test: `examples/atoi_catch_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: `goo_string_to_int` (runtime); `codegen_create_error_union_success/_error`; the `error("…")` payload codegen (call_codegen.c:263-290).
- Produces: `strconv.Atoi(string) → !int`, consumable by `catch` AND Task 1's destructure.

- [ ] **Step 1: Write the failing probe** — `examples/atoi_catch_probe.goo`:

```go
package main
import ("fmt"; "strconv")
func main() {
	x := strconv.Atoi("42") catch e { fmt.Println("FAIL"); return }
	fmt.Println(x)                 // 42
	y := strconv.Atoi("nope") catch e { fmt.Println("caught"); return }
	fmt.Println(y)
}
```
`examples/atoi_catch_probe.expected.txt`:
```
42
caught
```

- [ ] **Step 2: Verify failure** — Run: `./bin/goo -o /tmp/ac examples/atoi_catch_probe.goo` → `no member 'Atoi'`.

- [ ] **Step 3: Register Atoi → !int** — in `stdlib_package_lookup`, add (read the `error()` builtin's error-union return construction for the type):
```c
    if (strcmp(package, "strconv") == 0 && strcmp(name, "Atoi") == 0) {
        Type* int_t = type_checker_get_builtin(checker, TYPE_INT64);
        Type* err_t = type_checker_get_builtin(checker, TYPE_STRING);
        return type_function(NULL, 0, type_error_union(int_t, err_t));
    }
```

- [ ] **Step 4: Codegen `codegen_generate_atoi_call` + route.** Read `codegen_create_error_union_success/_error` and the `error("…")` codegen (call_codegen.c:263-290). alloca `i64 out`; `ok = goo_string_to_int(strArg, &out)`; branch on `ok`: success → `codegen_create_error_union_success(union_llvm, load(out), int_type)`; failure → build msg goo_string `"strconv.Atoi: invalid syntax"` → `codegen_create_error_union_error(union_llvm, msg)`; PHI the union at the merge. Return a ValueInfo of the `!int` type. Route `strconv.Atoi` in the stdlib dispatch beside `Itoa`. Show the code.

- [ ] **Step 5: Verify** — `make bin/goo && ./bin/goo -o /tmp/ac examples/atoi_catch_probe.goo && /tmp/ac` → `42` `caught`.

- [ ] **Step 6: Full gate + commit**
```bash
eval "$(opam env --switch=default)" && make verify && make test
git add src/types/expression_checker.c src/codegen/call_codegen.c examples/atoi_catch_probe.*
git commit --no-gpg-sign -m "feat(strconv): Atoi(string) !int via error union + goo_string_to_int"
```

---

### Task 3: errors.New(string) error

**Files:**
- Modify: `src/types/type_checker.c` (`stdlib_packages[]` += `"errors"`)
- Modify: `src/types/expression_checker.c` (`errors.New → error`)
- Modify: `src/codegen/call_codegen.c` (route `errors.New`)
- Test: `examples/errors_new_probe.goo` + `.expected.txt`

**Interfaces:**
- Produces: `errors.New(string) → error` (a non-nil `?error` value).

- [ ] **Step 1: Failing probe** — `examples/errors_new_probe.goo`:
```go
package main
import ("fmt"; "errors")
func main() {
	e := errors.New("boom")
	if e != nil { fmt.Println("got") } else { fmt.Println("nil") } // got
}
```
`.expected.txt`: `got`

- [ ] **Step 2: Verify failure** — `Undefined variable 'errors'` / `no member 'New'`.

- [ ] **Step 3: Register** — add `"errors"` to `stdlib_packages[]` (type_checker.c); add `errors.New → error` in `stdlib_package_lookup` (return type = the `error` builtin, `type_nullable(type_pointer(int8))`).

- [ ] **Step 4: Codegen** — route `errors.New` in the stdlib dispatch: produce a non-nil `?error` value — a nullable `{is_null=0, ptr=<non-null>}`. For v1 the pointer is a non-null marker (the message-carrying `.Error()` is deferred). Build the nullable directly (mirror the nullable layout used elsewhere). Show the code.

- [ ] **Step 5: Verify** — `./bin/goo -o /tmp/en examples/errors_new_probe.goo && /tmp/en` → `got`.

- [ ] **Step 6: Full gate + commit**
```bash
eval "$(opam env --switch=default)" && make verify && make test
git add src/types/type_checker.c src/types/expression_checker.c src/codegen/call_codegen.c examples/errors_new_probe.*
git commit --no-gpg-sign -m "feat(errors): errors.New(string) error"
```

---

### Task 4: Capstone — the Go error round-trip

**Files:** Create `examples/error_roundtrip_probe.goo` + `.expected.txt`

**Interfaces:** Consumes Tasks 1-3 + the (int,error) tuple producing (already on main).

- [ ] **Step 1: Capstone probe** — `examples/error_roundtrip_probe.goo`:
```go
package main
import ("fmt"; "strconv")
func parse(s string) (int, error) {
	n, err := strconv.Atoi(s)
	if err != nil { return 0, err }
	return n, nil
}
func main() {
	a, e1 := parse("41")
	if e1 != nil { fmt.Println("unexpected") } else { fmt.Println(a + 1) } // 42
	_, e2 := parse("nope")
	if e2 != nil { fmt.Println("caught") } else { fmt.Println("missed") }  // caught
}
```
`.expected.txt`:
```
42
caught
```

- [ ] **Step 2: Run it** — `./bin/goo -o /tmp/rt examples/error_roundtrip_probe.goo && /tmp/rt` → `42` `caught`. If it fails, the failing facet's task is incomplete — return to it. Also do a manual `.go`-extension check: copy to `/tmp/rt.go`, `goo /tmp/rt.go && /tmp/rt.out`.

- [ ] **Step 3: Full gate + commit**
```bash
eval "$(opam env --switch=default)" && make verify && make test
git add examples/error_roundtrip_probe.*
git commit --no-gpg-sign -m "test(golden): Go error round-trip — (int,error) == !int end-to-end"
```

- [ ] **Step 4: Update memory** — append to `goolang-v1-roadmap`: error bridge + Atoi + errors.New shipped; `n, err := strconv.Atoi(s)` works; `(int,error) == !int` real end-to-end; `.Error()` message still deferred (Phase 6).

---

## Self-Review

- **Spec coverage:** bridge → Task 1; Atoi → Task 2; errors.New → Task 3; capstone → Task 4. `.Error()` message deferred (spec + plan agree). ✓
- **Placeholder scan:** Task 1 codegen (value-arm extract + `?error` build) and Task 2 Atoi codegen are specified by the exact helpers + the `catch`/`error()` patterns to mirror — bounded "read the existing unwrap," not placeholders. All probe code complete. ✓
- **Type consistency:** `TYPE_ERROR_UNION`, `type_error_union`, `codegen_create_error_union_success/_error`, `goo_string_to_int`, `type_nullable(type_pointer(int8))` for `error`, used consistently. ✓
- **Ordering:** Task 1 (bridge — the keystone) → Task 2 (Atoi, consumed via bridge+catch) → Task 3 (errors.New) → Task 4 (capstone needs all). ✓
