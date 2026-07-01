# Error wrapping (`%w` + `errors.Unwrap`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `fmt.Errorf("outer: %w", inner)` builds a wrapped error whose message renders the chain and whose cause is `inner`; `errors.Unwrap` retrieves the cause.

**Architecture:** `%w` renders the wrapped error's message into the outer string at creation (like `%v`, reusing `codegen_error_display_string`) and captures the wrapped handle via a new `wrap_out` out-param on the shared `fmt_emit_segments` walker; `fmt.Errorf` then boxes with `goo_error_wrap(msg, cause)` (stores `cause`) instead of `goo_error_from_string`. `errors.Unwrap` reads `goo_error.cause` back via a runtime helper. `.Error()` and the `error` type are unchanged.

**Tech Stack:** C23, LLVM-C 22, hand-emitted IR. Tests are golden probes (`examples/<name>.goo` + `.expected.txt`, auto-discovered by `scripts/run_golden.sh`).

## Global Constraints

- Build env: `eval "$(opam env --switch=default)"` (run STANDALONE, never piped into `tail`/other — a pipe runs it in a subshell and drops the opam env → false `ccomp` failure) before `make verify`.
- Commits: `git commit --no-gpg-sign` (1Password agent unavailable).
- Gate before marking any task done: `make verify` → `verify: ALL GREEN GATES PASSED` (golden count rising, /0 failures) AND `make test` → 76 passed / 1 skipped.
- Grouped imports must be **newline-separated**: `import (\n\t"fmt"\n\t"errors"\n)`. The inline `import ("a"; "b")` does NOT parse.
- `error` = the nullable `{ i1 is_null, i8* handle }` tagged `->name="error"` (SSOT `type_checker_error_type()`); a non-nil error is `is_null=0` with the boxed `goo_error*` (as `i8*`) in field 1. The handle is opaque `i8*` — only runtime helpers know the `goo_error` layout.
- `type_is_error(const Type*)` (src/types/types.c) is the error-tag predicate. `codegen_error_display_string(codegen, err_loaded, pos)` builds `select(is_null, "<nil>", goo_error_message(handle))`.
- Only ONE `%w` per `fmt.Errorf` (v1). Negative cases (`%w` non-error / `%w` outside Errorf / multiple `%w`) are compile-time diagnostics, verified manually — NOT golden probes (golden runs only successful programs; do not add failing programs to `examples/`).

---

### Task 1: Runtime — `goo_error_wrap` + `goo_error_unwrap`

**Files:**
- Modify: `src/runtime/runtime.c` (add both functions near `goo_error_from_string`, ~line 108; refactor `goo_error_from_string`)
- Modify: `include/runtime.h` (declare both, near line 54)
- Modify: `src/codegen/runtime_integration.c` (register both, next to the `goo_error_from_string`/`goo_error_message` block)

**Interfaces:**
- Consumes: existing `struct goo_error { const char* message; int code; struct goo_error* cause; }`, `goo_alloc`, `goo_error_from_string`.
- Produces:
  - `goo_error_t* goo_error_wrap(goo_string_t msg, goo_error_t* cause)` — heap `goo_error` with a NUL-terminated copy of `msg`, `code=-1`, `cause=cause`.
  - `goo_error_t* goo_error_unwrap(goo_error_t* e)` — returns `e ? e->cause : NULL`.
  - LLVM symbols: `goo_error_wrap : (goo_string, i8*) -> i8*`; `goo_error_unwrap : (i8*) -> i8*`.

- [ ] **Step 1: Implement `goo_error_wrap` and refactor `goo_error_from_string` onto it**

In `src/runtime/runtime.c`, replace the existing `goo_error_from_string` body so it delegates, and add `goo_error_wrap` + `goo_error_unwrap`:

```c
// Build a heap goo_error from a goo_string message with an explicit cause.
// Copies length bytes plus a trailing NUL (goo_string.data is not assumed
// NUL-terminated). code=-1.
goo_error_t* goo_error_wrap(goo_string_t msg, goo_error_t* cause) {
    goo_error_t* error = goo_alloc(sizeof(goo_error_t));
    size_t len = msg.length;
    char* copy = goo_alloc(len + 1);
    if (msg.data && len > 0) {
        memcpy(copy, msg.data, len);
    }
    copy[len] = '\0';
    error->message = copy;
    error->code = -1;
    error->cause = cause;
    return error;
}

// Unchanged behavior: box a message with no cause.
goo_error_t* goo_error_from_string(goo_string_t msg) {
    return goo_error_wrap(msg, NULL);
}

// Return the wrapped cause (or NULL if none / e is NULL).
goo_error_t* goo_error_unwrap(goo_error_t* e) {
    return e ? e->cause : NULL;
}
```
(Delete the OLD standalone `goo_error_from_string` body — it is now the two-line delegator above.)

- [ ] **Step 2: Declare in the header**

In `include/runtime.h`, after the `goo_error_from_string`/`goo_error_message` declarations (~line 54):

```c
goo_error_t* goo_error_wrap(goo_string_t msg, goo_error_t* cause);
goo_error_t* goo_error_unwrap(goo_error_t* e);
```

- [ ] **Step 3: Register the runtime symbols for codegen**

In `src/codegen/runtime_integration.c`, after the `goo_error_message` registration block (`ptr_type` = `i8*` is already in scope in this function — reuse it as Phase 6 did; if not named locally, build `LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0)`):

```c
// goo_error_t* goo_error_wrap(goo_string_t msg, goo_error_t* cause)
{
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
    LLVMTypeRef params[] = { string_type, i8ptr };
    add_runtime_function(codegen, "goo_error_wrap", i8ptr, params, 2);
}
// goo_error_t* goo_error_unwrap(goo_error_t* e)
{
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
    LLVMTypeRef params[] = { i8ptr };
    add_runtime_function(codegen, "goo_error_unwrap", i8ptr, params, 1);
}
```

- [ ] **Step 4: Build to verify it compiles and links**

Run: `eval "$(opam env --switch=default)"; make 2>&1 | tail -5`
Expected: builds `bin/goo`, no new errors (pre-existing deprecation warnings are fine).

- [ ] **Step 5: Regression gate**

Run: `eval "$(opam env --switch=default)"; make verify 2>&1 | tail -2 && make test 2>&1 | grep -E "Passed|Skipped"`
Expected: `ALL GREEN GATES PASSED` golden 108/0 (unchanged — `goo_error_from_string` refactor is behavior-preserving); `make test` 76 passed / 1 skipped.

- [ ] **Step 6: Commit**

```bash
git add src/runtime/runtime.c include/runtime.h src/codegen/runtime_integration.c
git commit --no-gpg-sign -m "feat(runtime): goo_error_wrap + goo_error_unwrap (from_string delegates to wrap)"
```

---

### Task 2: `%w` end-to-end (walker `wrap_out` param + `fmt.Errorf` wrapping)

**Files:**
- Modify: `src/codegen/call_codegen.c` — `fmt_emit_segments` signature (~1339) + `%w` verb branch (after the `%v` arm, ~1699) + the 3 callers (Printf ~1768, Sprintf ~1806, Errorf ~1837) + `codegen_generate_errorf_call` boxing (~1840)
- Create: `examples/errorf_wrap_probe.goo`, `examples/errorf_wrap_probe.expected.txt`

**Interfaces:**
- Consumes: `goo_error_wrap` (Task 1), `codegen_error_display_string`, `type_is_error`.
- Produces: `fmt.Errorf("...%w", err)` returns a wrapped error whose message renders the chain and whose `cause` is `err`'s handle.

- [ ] **Step 1: Write the failing golden probe**

`examples/errorf_wrap_probe.goo`:

```go
package main
import (
	"fmt"
	"errors"
)
func main() {
	e := fmt.Errorf("outer: %w", errors.New("inner"))
	fmt.Println(e.Error())
}
```

`examples/errorf_wrap_probe.expected.txt`:

```
outer: inner
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `bin/goo -o build/errorf_wrap_probe examples/errorf_wrap_probe.goo && build/errorf_wrap_probe`
Expected: FAIL — `%w` is an unknown verb today (or "unsupported"), no wrap.

- [ ] **Step 3: Add the `wrap_out` param to the walker and update all 3 callers**

In `src/codegen/call_codegen.c`, change the `fmt_emit_segments` signature (line ~1339) to add `LLVMValueRef* wrap_out` after `out_str`:

```c
static int fmt_emit_segments(CodeGenerator* c, TypeChecker* tc,
                              const char* fmt_str, ASTNode* args,
                              int sprintf_mode, LLVMValueRef* out_str,
                              LLVMValueRef* wrap_out,
                              Position call_pos) {
```

Update the three call sites to pass the new arg:
- Printf (~1768): `if (!fmt_emit_segments(codegen, checker, fmt_str, fmt_arg->next, 0, NULL, NULL, expr->pos)) {`
- Sprintf (~1806): `if (!fmt_emit_segments(codegen, checker, fmt_str, fmt_arg->next, 1, &result, NULL, expr->pos)) {`
- Errorf (~1837): (changed fully in Step 5) `if (!fmt_emit_segments(codegen, checker, fmt_str, fmt_arg->next, 1, &msg_str, &cause, expr->pos)) {`

- [ ] **Step 4: Add the `%w` verb branch**

In the verb chain, INSERT a `%w` branch immediately after the `%v` arm's closing `}` (i.e. right before the final `else { ... "unknown verb" ... }`). `%w` is only reachable in `fmt.Errorf` (the only caller passing non-NULL `wrap_out`, always `sprintf_mode=1`), so it only concatenates into `acc`:

```c
} else if (verb == 'w') {
    // %w — wrap an error (fmt.Errorf only). Renders the wrapped error's
    // message like %v AND records its handle as the cause via wrap_out.
    if (!wrap_out) {
        codegen_error(c, arg_cursor->pos, "fmt: %%w is only valid in fmt.Errorf");
        value_info_free(arg_val); ok = 0; break;
    }
    if (!type_is_error(arg_val->goo_type)) {
        codegen_error(c, arg_cursor->pos, "fmt.Errorf: %%w requires an error argument");
        value_info_free(arg_val); ok = 0; break;
    }
    if (*wrap_out) {
        codegen_error(c, arg_cursor->pos, "fmt.Errorf: multiple %%w not supported (v1)");
        value_info_free(arg_val); ok = 0; break;
    }
    LLVMValueRef disp = codegen_error_display_string(c, arg_val->llvm_value, arg_cursor->pos);
    if (!disp) { value_info_free(arg_val); ok = 0; break; }
    LLVMValueRef cargs[] = { acc, disp };
    acc = LLVMBuildCall2(c->builder, LLVMGlobalGetValueType(concat_fn), concat_fn, cargs, 2, "sp_acc");
    // Record the wrapped error's handle (field 1 of the loaded nullable) as the cause.
    *wrap_out = LLVMBuildExtractValue(c->builder, arg_val->llvm_value, 1, "w.cause");
}
```

Note: the arg is already fetched and loaded generically before the verb switch (`arg_val->llvm_value`, see ~line 1489-1500), and `concat_fn`/`arg_cursor` are already in scope (used by `%s`/`%v`).

- [ ] **Step 5: Make `fmt.Errorf` box with the cause**

In `codegen_generate_errorf_call` (~1820), declare `cause` before the walker call and box conditionally. Replace the body from the `LLVMValueRef msg_str = NULL;` line through the `handle` construction with:

```c
    const char* fmt_str = ((LiteralNode*)fmt_arg)->value;
    LLVMValueRef msg_str = NULL;
    LLVMValueRef cause = NULL;
    if (!fmt_emit_segments(codegen, checker, fmt_str, fmt_arg->next, 1, &msg_str, &cause, expr->pos)) {
        return NULL;
    }
    LLVMValueRef handle;
    if (cause) {
        LLVMValueRef wrap_fn = LLVMGetNamedFunction(codegen->module, "goo_error_wrap");
        if (!wrap_fn) { codegen_error(codegen, expr->pos, "goo_error_wrap not found in module"); return NULL; }
        LLVMValueRef wargs[] = { msg_str, cause };
        handle = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(wrap_fn), wrap_fn, wargs, 2, "errorf_wrap");
    } else {
        LLVMValueRef from_str = LLVMGetNamedFunction(codegen->module, "goo_error_from_string");
        if (!from_str) { codegen_error(codegen, expr->pos, "goo_error_from_string not found in module"); return NULL; }
        LLVMValueRef bargs[] = { msg_str };
        handle = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(from_str), from_str, bargs, 1, "errorf_box");
    }
```
(Keep the following `err_type`/`err_val` InsertValue assembly and `return value_info_new(...)` unchanged — `handle` now feeds it.)

- [ ] **Step 6: Run the probe to confirm it passes**

Run: `bin/goo -o build/errorf_wrap_probe examples/errorf_wrap_probe.goo && build/errorf_wrap_probe`
Expected: `outer: inner`

- [ ] **Step 7: Verify the negative diagnostics (manual — NOT golden probes)**

Write throwaway files under `build/` (NOT `examples/`) and confirm each fails to COMPILE with the expected message; delete them after:
- `%w` outside Errorf — `s := fmt.Sprintf("%w", errors.New("x"))` → error `%w is only valid in fmt.Errorf`.
- `%w` non-error arg — `e := fmt.Errorf("%w", "str")` → error `%w requires an error argument`.
- multiple `%w` — `e := fmt.Errorf("%w %w", errors.New("a"), errors.New("b"))` → error `multiple %w not supported`.
Run e.g.: `printf 'package main\nimport (\n\t"fmt"\n\t"errors"\n)\nfunc main() {\n\ts := fmt.Sprintf("%%w", errors.New("x"))\n\tfmt.Println(s)\n}\n' > build/neg1.goo && bin/goo -o build/neg1 build/neg1.goo 2>&1 | grep -i "only valid in fmt.Errorf"` (expect a match, nonzero compile). Note the outcomes in the task report; `rm build/neg*.goo` after.

- [ ] **Step 8: Full gate**

Run: `eval "$(opam env --switch=default)"; make verify 2>&1 | tail -2 && make test 2>&1 | grep -E "Passed|Skipped"`
Expected: `ALL GREEN GATES PASSED`, golden 109/0 (errorf_wrap_probe added; existing printf/sprintf/println probes still green — proves the walker signature change didn't regress plain formatting); `make test` 76/1.

- [ ] **Step 9: Commit**

```bash
git add src/codegen/call_codegen.c examples/errorf_wrap_probe.goo examples/errorf_wrap_probe.expected.txt
git commit --no-gpg-sign -m "feat(fmt): %w wraps an error in fmt.Errorf (sets cause + renders chain)"
```

---

### Task 3: `errors.Unwrap`

**Files:**
- Modify: `src/types/expression_checker.c` — register `errors.Unwrap` in `stdlib_package_lookup` (after the `errors.New` block, ~line 1250)
- Modify: `src/codegen/call_codegen.c` — new `errors`/`Unwrap` route (after the `errors.New` route, ~line 451)
- Create: `examples/unwrap_probe.goo`, `examples/unwrap_probe.expected.txt`, `examples/unwrap_nil_probe.goo`, `examples/unwrap_nil_probe.expected.txt`

**Interfaces:**
- Consumes: `goo_error_unwrap` (Task 1), the wrapped error from `fmt.Errorf` (Task 2), `type_checker_error_type`.
- Produces: `errors.Unwrap(error) -> error` — the cause, or nil if none.

- [ ] **Step 1: Write the failing golden probes**

`examples/unwrap_probe.goo`:

```go
package main
import (
	"fmt"
	"errors"
)
func main() {
	e := fmt.Errorf("outer: %w", errors.New("inner"))
	u := errors.Unwrap(e)
	fmt.Println(u.Error())
}
```
`examples/unwrap_probe.expected.txt`:
```
inner
```

`examples/unwrap_nil_probe.goo`:

```go
package main
import (
	"fmt"
	"errors"
)
func main() {
	u := errors.Unwrap(errors.New("x"))
	if u == nil {
		fmt.Println("nil")
	} else {
		fmt.Println("notnil")
	}
}
```
`examples/unwrap_nil_probe.expected.txt`:
```
nil
```

- [ ] **Step 2: Run them to confirm they fail**

Run: `bin/goo -o build/unwrap_probe examples/unwrap_probe.goo`
Expected: FAIL — `errors.Unwrap` not recognized (typecheck: unknown selector on `errors`).

- [ ] **Step 3: Register the typecheck return type**

In `src/types/expression_checker.c`, in `stdlib_package_lookup`, after the `errors.New` block (~line 1250):

```c
// errors.Unwrap(error) -> error  (returns the wrapped cause, or nil)
if (strcmp(package, "errors") == 0 && strcmp(name, "Unwrap") == 0) {
    return type_function(NULL, 0, type_checker_error_type(checker));
}
```

- [ ] **Step 4: Add the codegen route**

In `src/codegen/call_codegen.c`, immediately after the `errors.New` route's closing `}` (~line 451), add:

```c
if (strcmp(pkg->name, "errors") == 0 && strcmp(sel->selector, "Unwrap") == 0) {
    // errors.Unwrap(error) -> error: read goo_error.cause via the runtime
    // helper, rebuild the nullable {is_null = cause==null, ptr = cause}.
    if (!call->args) {
        codegen_error(codegen, expr->pos, "errors.Unwrap: expected an error argument");
        return NULL;
    }
    ValueInfo* ev = codegen_generate_expression(codegen, checker, call->args);
    if (!ev) return NULL;
    LLVMValueRef err_loaded = ev->llvm_value;
    if (ev->is_lvalue && ev->goo_type) {
        LLVMTypeRef et = codegen_type_to_llvm(codegen, ev->goo_type);
        if (et) err_loaded = LLVMBuildLoad2(codegen->builder, et, err_loaded, "unwrap_load");
    }
    value_info_free(ev);

    LLVMValueRef handle = LLVMBuildExtractValue(codegen->builder, err_loaded, 1, "unwrap.handle");
    LLVMValueRef unwrap_fn = LLVMGetNamedFunction(codegen->module, "goo_error_unwrap");
    if (!unwrap_fn) { codegen_error(codegen, expr->pos, "goo_error_unwrap not found in module"); return NULL; }
    LLVMValueRef uargs[] = { handle };
    LLVMValueRef cause = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(unwrap_fn), unwrap_fn, uargs, 1, "unwrap.cause");

    LLVMTypeRef i8pt = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
    LLVMValueRef is_null = LLVMBuildICmp(codegen->builder, LLVMIntEQ, cause, LLVMConstNull(i8pt), "unwrap.isnull");
    Type* err_type = type_checker_error_type(checker);
    LLVMTypeRef err_llvm = codegen_type_to_llvm(codegen, err_type);
    LLVMValueRef err_val = LLVMGetUndef(err_llvm);
    err_val = LLVMBuildInsertValue(codegen->builder, err_val, is_null, 0, "uw.is_null");
    err_val = LLVMBuildInsertValue(codegen->builder, err_val, cause, 1, "uw.ptr");
    return value_info_new(NULL, err_val, err_type);
}
```

- [ ] **Step 5: Run the probes to confirm they pass**

Run: `bin/goo -o build/unwrap_probe examples/unwrap_probe.goo && build/unwrap_probe`
Expected: `inner`
Run: `bin/goo -o build/unwrap_nil_probe examples/unwrap_nil_probe.goo && build/unwrap_nil_probe`
Expected: `nil`

- [ ] **Step 6: Full gate**

Run: `eval "$(opam env --switch=default)"; make verify 2>&1 | tail -2 && make test 2>&1 | grep -E "Passed|Skipped"`
Expected: `ALL GREEN GATES PASSED`, golden 111/0; `make test` 76/1.

- [ ] **Step 7: Commit**

```bash
git add src/types/expression_checker.c src/codegen/call_codegen.c examples/unwrap_probe.goo examples/unwrap_probe.expected.txt examples/unwrap_nil_probe.goo examples/unwrap_nil_probe.expected.txt
git commit --no-gpg-sign -m "feat(errors): errors.Unwrap(error) returns the wrapped cause"
```

---

### Task 4: Capstone — multi-level chain

**Files:**
- Create: `examples/error_wrap_capstone.goo`, `examples/error_wrap_capstone.expected.txt`

**Interfaces:**
- Consumes: everything above (`%w`, `errors.Unwrap`, `errors.New`, `.Error()`).
- Produces: an end-to-end proof; no source changes.

- [ ] **Step 1: Write the capstone probe**

`examples/error_wrap_capstone.goo`:

```go
package main
import (
	"fmt"
	"errors"
)
func main() {
	a := errors.New("root")
	b := fmt.Errorf("mid: %w", a)
	c := fmt.Errorf("top: %w", b)
	fmt.Println(c.Error())
	fmt.Println(errors.Unwrap(c).Error())
	fmt.Println(errors.Unwrap(errors.Unwrap(c)).Error())
}
```

`examples/error_wrap_capstone.expected.txt`:

```
top: mid: root
mid: root
root
```

- [ ] **Step 2: Run the capstone**

Run: `bin/goo -o build/error_wrap_capstone examples/error_wrap_capstone.goo && build/error_wrap_capstone`
Expected:
```
top: mid: root
mid: root
root
```
(If nested `errors.Unwrap(errors.Unwrap(c))` exposes an rvalue-arg gap, note it and split into intermediate `let`-style bindings — but the core must pass: the 3-line chain rendering and one level of Unwrap.)

- [ ] **Step 3: Full gate**

Run: `eval "$(opam env --switch=default)"; make verify 2>&1 | tail -2 && make test 2>&1 | grep -E "Passed|Skipped"`
Expected: `ALL GREEN GATES PASSED`, golden 112/0; `make test` 76/1.

- [ ] **Step 4: Commit**

```bash
git add examples/error_wrap_capstone.goo examples/error_wrap_capstone.expected.txt
git commit --no-gpg-sign -m "test(errors): capstone — 2-level %w chain + errors.Unwrap"
```

---

## Verification (end-to-end)

1. `eval "$(opam env --switch=default)"` then `make verify` — the 4 new probes (`errorf_wrap`, `unwrap`, `unwrap_nil`, `error_wrap_capstone`) compile via `bin/goo`, run, and match `.expected.txt`; existing Printf/Sprintf/Println probes stay green (proves the walker signature change is safe). Expect `verify: ALL GREEN GATES PASSED`, golden 112/0.
2. `make test` — 76 passed, 1 skipped.
3. Spot-run the capstone: `bin/goo -o build/error_wrap_capstone examples/error_wrap_capstone.goo && build/error_wrap_capstone` → `top: mid: root` / `mid: root` / `root`.
4. Whole-branch review, then push + open PR; confirm `MERGEABLE`/`CLEAN` via `gh pr view` (CI is workflow_dispatch-only; the local gate is authoritative).

## Self-Review notes

- **Spec coverage:** Task 1 = runtime wrap/unwrap; Task 2 = `%w` (walker param + Errorf boxing + negative diagnostics); Task 3 = `errors.Unwrap`; Task 4 = capstone + gate. All spec components mapped. `.Error()` unchanged (spec's key realization) — no task touches it, correct.
- **Risk:** the `fmt_emit_segments` signature change (Task 2) touches all 3 callers; a missed caller is a compile error caught immediately by `make`. The Println/Printf/Sprintf golden probes staying green in Step 8 is the regression guard.
- **Reused, not rebuilt:** `goo_error` struct + `goo_alloc`, `codegen_error_display_string`, `type_is_error`, `type_checker_error_type`, `add_runtime_function`, `stdlib_package_lookup`, the `errors.New` route/box shape.
- **Golden count:** starts at 108 (post-Phase-6b-hardening main); +1 Task 2, +2 Task 3, +1 Task 4 → 112.
