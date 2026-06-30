# strconv.Itoa / strconv.Atoi Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `strconv.Itoa(int) string` and `strconv.Atoi(string) !int` (a Goolang error union), reusing the `goo_int_to_string` helper, plus the bundled `snprintf` truncation guard.

**Architecture:** `Itoa` lowers to `goo_int_to_string`. `Atoi` lowers to a new `goo_string_to_int` parse helper + builds an `!int` error union via `codegen_create_error_union_success`/`_error` — integrating with the existing `try`/`catch`.

**Tech Stack:** C23, LLVM-C, `goo_string_t` runtime, error unions (`TYPE_ERROR_UNION`), `scripts/run_golden.sh`.

## Global Constraints

- Gate = `make verify` ALL GREEN (incl. ccomp via `eval "$(opam env --switch=default)"`) + `make test` 76/1 + golden, no regressions.
- Commits FAIL to sign — use `git commit --no-gpg-sign`.
- No naked returns / no silent failures; errors via `codegen_error`/`type_error`.
- Golden probes auto-discovered: `examples/<name>.goo` + `.expected.txt`.
- References: error-union constructors `codegen_create_error_union_success(codegen, union_type, value, value_type)` / `codegen_create_error_union_error(codegen, union_type, error_value)` (`src/codegen/error_union_codegen.c:11,56`). The `error("msg")` builtin codegen (`src/codegen/call_codegen.c:263-290`) shows how to build `union_llvm` + a string error payload — mirror it for Atoi's failure arm. Stdlib dispatch block at `call_codegen.c:296+`. `stdlib_package_lookup` at `expression_checker.c:1118`. The `error()` builtin's error-union RETURN type construction is at `expression_checker.c:~827-842`. try/catch syntax: `x := expr() catch e { ... }` (see `examples/erru_catch_probe.goo`). Spec: `docs/superpowers/specs/2026-06-30-strconv-itoa-atoi-design.md`.

---

### Task 1: Runtime `goo_string_to_int` + truncation guard

**Files:**
- Modify: `include/runtime.h` (declare `goo_string_to_int`)
- Modify: `src/runtime/runtime.c` (implement `goo_string_to_int`; add truncation guard to `goo_int_to_string` + `goo_float_to_string`)
- Modify: `src/codegen/runtime_integration.c` (declare `goo_string_to_int` in the module)

**Interfaces:**
- Produces: `int goo_string_to_int(goo_string_t s, int64_t* out)` — returns 1 and sets `*out` on a valid base-10 signed int; returns 0 on empty/invalid/trailing-junk/overflow.

- [ ] **Step 1: Add the truncation guard** to `goo_int_to_string` and `goo_float_to_string` in `src/runtime/runtime.c` — after each `snprintf`, before the `goo_alloc`:

```c
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        goo_panic("goo_*_to_string: snprintf overflow");
    }
```
(Use the actual function's buffer name; this closes the latent UB flagged in the fmt.Printf review now that strconv.Itoa reuses goo_int_to_string. Confirm `goo_panic` is declared/visible in runtime.c — it is used elsewhere in this file.)

- [ ] **Step 2: Declare `goo_string_to_int` in `include/runtime.h`** (near `goo_int_to_string`):

```c
int goo_string_to_int(goo_string_t s, int64_t* out);
```

- [ ] **Step 3: Implement in `src/runtime/runtime.c`.** Read `goo_string_t` field names first. Then:

```c
int goo_string_to_int(goo_string_t s, int64_t* out) {
    if (!out || s.length == 0) return 0;
    // Copy to a NUL-terminated stack buffer (s.data may be a non-terminated slice).
    char buf[32];
    if (s.length >= sizeof(buf)) return 0;  // too long to be a valid int64
    memcpy(buf, s.data, s.length);
    buf[s.length] = '\0';
    errno = 0;
    char* end = NULL;
    long long v = strtoll(buf, &end, 10);
    if (errno != 0) return 0;          // overflow/underflow
    if (end == buf || *end != '\0') return 0;  // empty or trailing junk
    *out = (int64_t)v;
    return 1;
}
```
(Ensure `<errno.h>` and `<string.h>` are included in runtime.c — they likely are; add if not.)

- [ ] **Step 4: Declare in the module** — in `src/codegen/runtime_integration.c`, near the `goo_int_to_string` declaration (added by the fmt milestone), mirror `add_runtime_function`: `goo_string_to_int` takes TWO params — the `goo_string_t` struct (`string_type`) and an `i64*` pointer (`LLVMPointerType(i64_type, 0)` or the codebase's pointer-type helper) — and returns `i32_type`. Read the existing two-param declarations (e.g. `goo_string_concat`) for the exact form.

- [ ] **Step 5: Build + gate**

Run: `eval "$(opam env --switch=default)" && make bin/goo && make test`
Expected: builds clean, `make test` 76/1. (No behavior probe yet — Task 3 exercises `goo_string_to_int` via Atoi; Itoa truncation guard is unreachable with valid ints.)

- [ ] **Step 6: Commit**

```bash
git add include/runtime.h src/runtime/runtime.c src/codegen/runtime_integration.c
git commit --no-gpg-sign -m "feat(runtime): goo_string_to_int + snprintf truncation guards"
```

---

### Task 2: strconv.Itoa

**Files:**
- Modify: `src/types/expression_checker.c` (`stdlib_package_lookup` — register `strconv.Itoa`)
- Modify: `src/codegen/call_codegen.c` (route `strconv.Itoa` in the stdlib dispatch ~:296+)
- Test: `examples/itoa_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: `goo_int_to_string` (module fn, takes i64 → goo_string).
- Produces: `strconv.Itoa(int)` → string.

- [ ] **Step 1: Write the failing probe** — `examples/itoa_probe.goo`:

```go
package main
import (
	"fmt"
	"strconv"
)
func main() {
	fmt.Println(strconv.Itoa(123))
	fmt.Println(strconv.Itoa(-7))
}
```
`examples/itoa_probe.expected.txt`:
```
123
-7
```

- [ ] **Step 2: Verify failure**

Run: `./bin/goo -o /tmp/it examples/itoa_probe.goo`
Expected: `Undefined variable 'strconv'` (or "no member 'Itoa'").

- [ ] **Step 3: Register Itoa in the typechecker** — in `stdlib_package_lookup` (`expression_checker.c`), beside an existing entry, add (use the same string-builtin fetch the `strings.*` entries use for the return type):

```c
    if (strcmp(package, "strconv") == 0 && strcmp(name, "Itoa") == 0) {
        return type_function(NULL, 0, string_t);
    }
```

- [ ] **Step 4: Route Itoa codegen** — in the stdlib dispatch block (`call_codegen.c:296+`, beside the fmt/os entries), add a dedicated lowering (the generic `codegen_generate_stdlib_call` may not SExt the int arg to the i64 the runtime fn expects, so do it explicitly):

```c
            if (strcmp(pkg->name, "strconv") == 0 && strcmp(sel->selector, "Itoa") == 0) {
                LLVMValueRef fn = LLVMGetNamedFunction(codegen->module, "goo_int_to_string");
                if (!fn) { codegen_error(codegen, expr->pos, "goo_int_to_string not found"); return NULL; }
                ValueInfo* a = codegen_generate_expression(codegen, checker, call->args);
                if (!a) return NULL;
                LLVMValueRef v = LLVMBuildSExt(codegen->builder, a->llvm_value,
                                               LLVMInt64TypeInContext(codegen->context), "itoa_arg");
                LLVMValueRef args[] = { v };
                LLVMValueRef res = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(fn), fn, args, 1, "itoa");
                value_info_free(a);
                return value_info_new(NULL, res, type_checker_get_builtin(checker, TYPE_STRING));
            }
```
(Adapt `value_info_new` / the string builtin fetch to the codebase's actual signatures — read a nearby `value_info_new(... TYPE_STRING)` use, e.g. the Sprintf return.)

- [ ] **Step 5: Build + run**

Run: `make bin/goo && ./bin/goo -o /tmp/it examples/itoa_probe.goo && /tmp/it`
Expected: `123` then `-7`.

- [ ] **Step 6: Full gate + commit**

```bash
eval "$(opam env --switch=default)" && make verify && make test
git add src/types/expression_checker.c src/codegen/call_codegen.c examples/itoa_probe.*
git commit --no-gpg-sign -m "feat(strconv): Itoa(int) string via goo_int_to_string"
```

---

### Task 3: strconv.Atoi → !int

**Files:**
- Modify: `src/types/expression_checker.c` (register `strconv.Atoi` → `!int`)
- Modify: `src/codegen/call_codegen.c` (`codegen_generate_atoi_call` building the error union; route it)
- Test: `examples/atoi_ok_probe.goo`, `examples/atoi_err_probe.goo` (+ `.expected.txt`)

**Interfaces:**
- Consumes: `goo_string_to_int` (Task 1); `codegen_create_error_union_success`/`_error`; the `error("msg")` codegen pattern (`call_codegen.c:263-290`).

- [ ] **Step 1: Write the failing probes**

`examples/atoi_ok_probe.goo`:
```go
package main
import (
	"fmt"
	"strconv"
)
func main() {
	x := strconv.Atoi("42") catch e {
		fmt.Println("FAIL: ok took catch")
		return
	}
	fmt.Println(x)
}
```
`examples/atoi_ok_probe.expected.txt`:
```
42
```

`examples/atoi_err_probe.goo`:
```go
package main
import (
	"fmt"
	"strconv"
)
func main() {
	x := strconv.Atoi("nope") catch e {
		fmt.Println("caught")
		return
	}
	fmt.Println(x)
}
```
`examples/atoi_err_probe.expected.txt`:
```
caught
```

- [ ] **Step 2: Verify failure**

Run: `./bin/goo -o /tmp/ao examples/atoi_ok_probe.goo`
Expected: `Undefined variable 'strconv'` / "no member 'Atoi'".

- [ ] **Step 3: Register Atoi → !int.** In `stdlib_package_lookup`, add (read the `error()` builtin's error-union return construction at `expression_checker.c:~827-842` for how to build the `!int` type — `type_error_union(int64_builtin, error_type)`; the error_type is whatever that path uses, typically the string/error builtin):

```c
    if (strcmp(package, "strconv") == 0 && strcmp(name, "Atoi") == 0) {
        Type* int_t = type_checker_get_builtin(checker, TYPE_INT64);
        Type* err_t = type_checker_get_builtin(checker, TYPE_STRING); // match what error() uses
        return type_function(NULL, 0, type_error_union(int_t, err_t));
    }
```

- [ ] **Step 4: Implement `codegen_generate_atoi_call` + route it.** Read `codegen_create_error_union_success`/`_error` (error_union_codegen.c:11/56) and the `error()` builtin codegen (call_codegen.c:263-290) for how `union_llvm` and the string error payload are built. Then:
  - Determine the `!int` union LLVM type the same way the `error()` path does (from the expression's resolved `node_type`, or `codegen_type_to_llvm` of the Atoi return type).
  - alloca an `i64 out`; `ok = call goo_string_to_int(strArg, out_ptr)`.
  - Branch on `ok != 0`:
    - then: `succ = codegen_create_error_union_success(codegen, union_llvm, load(out), int_type)`
    - else: build a goo_string message `"strconv.Atoi: invalid syntax"` (same way error() builds its msg_val) → `errv = codegen_create_error_union_error(codegen, union_llvm, msg_val)`
    - PHI the two union values at the merge block.
  - Return a ValueInfo of the `!int` type wrapping the PHI.
  Route `strconv.Atoi` in the stdlib dispatch to `codegen_generate_atoi_call`.

- [ ] **Step 5: Build + run both probes**

Run: `make bin/goo && ./bin/goo -o /tmp/ao examples/atoi_ok_probe.goo && /tmp/ao && ./bin/goo -o /tmp/ae examples/atoi_err_probe.goo && /tmp/ae`
Expected: `42` then `caught`.

- [ ] **Step 6: Full gate + commit**

```bash
eval "$(opam env --switch=default)" && make verify && make test
git add src/types/expression_checker.c src/codegen/call_codegen.c examples/atoi_ok_probe.* examples/atoi_err_probe.*
git commit --no-gpg-sign -m "feat(strconv): Atoi(string) !int via error union + goo_string_to_int"
```

- [ ] **Step 7: Update memory** — append to `goolang-v1-roadmap`: strconv.Itoa + Atoi→!int shipped (Atoi uses the error-union differentiator + try/catch); goo_string_to_int added; truncation guards landed. Next stdlib: errors.New, strconv.FormatInt/ParseFloat, more strings.

---

## Self-Review

- **Spec coverage:** runtime goo_string_to_int + truncation guard → Task 1; Itoa → Task 2; Atoi→!int → Task 3; try/catch probes → Task 3; errors.New / FormatInt etc. out of scope (spec + plan agree). ✓
- **Placeholder scan:** runtime + Itoa codegen are complete; the Atoi codegen (Step 4) is specified by the exact constructors + the error() pattern to mirror + the branch/PHI structure — the one "read the existing pattern" is how the `error()` path types its union, which is a bounded read, not a placeholder. ✓
- **Type consistency:** `goo_string_to_int`, `goo_int_to_string`, `codegen_create_error_union_success`/`_error`, `type_error_union`, `type_checker_get_builtin(TYPE_INT64/STRING)`, `value_info_new` used consistently. ✓
- **Ordering:** Task 1 (runtime, needed by 3) + Task 2 (Itoa, independent) → Task 3 (Atoi). ✓
