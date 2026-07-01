# Error messages (`err.Error()` + `fmt.Errorf`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give Goolang's `error` a real message: `err.Error()` returns text, `fmt` prints it, and `fmt.Errorf` builds formatted errors.

**Architecture:** `error` stays `?*int8` at the type level; codegen reinterprets the `i8*` as an opaque handle to a heap `goo_error` (the struct already exists in the runtime, and its `cause` field is the future `%w` hook). Producers (`errors.New`, `fmt.Errorf`, the `n,err := <!T>` bridge) box a message into a `goo_error` and store its pointer in the nullable; consumers (`.Error()`, `fmt`) read it back. Every existing nullable mechanism (`!= nil`, the `(int,error)` tuple null-insert, the bridge's `is_null` math) is unchanged.

**Tech Stack:** C23, LLVM-C 22, hand-emitted IR. Tests are golden probes (`examples/<name>.goo` + `.expected.txt`, auto-discovered by `scripts/run_golden.sh`).

## Global Constraints

- Build env: `eval "$(opam env --switch=default)"` before `make verify` (CompCert path). The pre-push hook sources it itself.
- Commits: `git commit --no-gpg-sign` (1Password agent unavailable in this env).
- Gate before marking any task done: `make verify` (ALL GREEN GATES PASSED, golden N/0) **and** `make test` (76 passed, 1 skipped).
- Grouped imports must be **newline-separated**; `import ("a"; "b")` does NOT parse.
- `error` SSOT is `type_checker_error_type()` (`src/types/type_checker.c:114`) — do not hand-build `?*int8` elsewhere.
- Language `int` = i32 (TYPE_INT32). `goo_string` is 16 B and crosses the C ABI **by value** (same as `goo_string_to_int`).
- Conventional commits, imperative mood, one logical change per commit.

---

### Task 1: Runtime — box and read an error message

**Files:**
- Modify: `src/runtime/runtime.c` (add two functions after `goo_new_error_with_code`, ~line 104)
- Modify: `include/runtime.h` (declare them near `goo_new_error`, ~line 51)
- Modify: `src/codegen/runtime_integration.c` (register them next to `goo_string_to_int`, ~line 246)

**Interfaces:**
- Consumes: existing `struct goo_error { const char* message; int code; struct goo_error* cause; }` (runtime.h:21), `goo_alloc(size_t)` (runtime.c:43), `struct goo_string { char* data; size_t length; }`.
- Produces:
  - `goo_error_t* goo_error_from_string(goo_string_t msg)` — heap `goo_error` holding a NUL-terminated copy of `msg`.
  - `goo_string_t goo_error_message(goo_error_t* e)` — `{ e->message, strlen(e->message) }`.
  - Runtime symbols registered so codegen can emit calls: `goo_error_from_string : (goo_string) -> i8*`, `goo_error_message : (i8*) -> goo_string` (the `goo_error_t*` is registered as `i8*` — an opaque handle; no `goo_error` LLVM struct type is introduced).

- [ ] **Step 1: Implement the two runtime functions**

In `src/runtime/runtime.c`, after `goo_new_error_with_code` (line 104):

```c
// Build a heap goo_error from a goo_string message. Copies length bytes plus a
// trailing NUL (goo_string.data is not assumed NUL-terminated). code=-1, no cause.
goo_error_t* goo_error_from_string(goo_string_t msg) {
    goo_error_t* error = goo_alloc(sizeof(goo_error_t));
    size_t len = msg.length;
    char* copy = goo_alloc(len + 1);
    if (msg.data && len > 0) {
        memcpy(copy, msg.data, len);
    }
    copy[len] = '\0';
    error->message = copy;
    error->code = -1;
    error->cause = NULL;
    return error;
}

// Return the message as a goo_string. strlen on read (embedded-NUL truncates —
// accepted v1 edge; error messages are ASCII in practice).
goo_string_t goo_error_message(goo_error_t* e) {
    goo_string_t s;
    if (e && e->message) {
        s.data = (char*)e->message;
        s.length = strlen(e->message);
    } else {
        s.data = NULL;
        s.length = 0;
    }
    return s;
}
```

- [ ] **Step 2: Declare them in the header**

In `include/runtime.h`, after `goo_new_error_with_code` (line 52):

```c
goo_error_t* goo_error_from_string(goo_string_t msg);
goo_string_t goo_error_message(goo_error_t* e);
```

- [ ] **Step 3: Register the runtime symbols for codegen**

In `src/codegen/runtime_integration.c`, after the `goo_string_to_int` block (line ~249). `string_type`, `i8_ptr_type`/pointer helpers are already in scope in this function (mirror the existing blocks; if a generic `i8*` is not already named, build it with `LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0)`):

```c
// goo_error_t* goo_error_from_string(goo_string_t msg)  [handle returned as i8*]
{
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
    LLVMTypeRef params[] = { string_type };
    add_runtime_function(codegen, "goo_error_from_string", i8ptr, params, 1);
}
// goo_string_t goo_error_message(goo_error_t* e)  [handle passed as i8*]
{
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
    LLVMTypeRef params[] = { i8ptr };
    add_runtime_function(codegen, "goo_error_message", string_type, params, 1);
}
```

- [ ] **Step 4: Build to verify it compiles and links**

Run: `eval "$(opam env --switch=default)"; make 2>&1 | tail -5`
Expected: builds `bin/goo` with no errors (deprecation warnings are pre-existing).

- [ ] **Step 5: Commit**

```bash
git add src/runtime/runtime.c include/runtime.h src/codegen/runtime_integration.c
git commit --no-gpg-sign -m "feat(runtime): goo_error_from_string + goo_error_message (Phase 6 scaffold)"
```

---

### Task 2: Producer — `errors.New` carries the message

**Files:**
- Modify: `src/codegen/call_codegen.c:411-434` (the `errors`/`New` branch)

**Interfaces:**
- Consumes: `goo_error_from_string` (Task 1), `type_checker_error_type(checker)` (type_checker.c:114).
- Produces: `errors.New(msg)` returns the nullable error `{is_null=0, ptr=<goo_error* as i8*>}` carrying the real message.

- [ ] **Step 1: Replace the discard-and-marker codegen**

In `src/codegen/call_codegen.c`, replace the body of the `errors`/`New` branch (lines 411-434, the part after the package/selector match) so the message is boxed instead of discarded:

```c
if (strcmp(pkg->name, "errors") == 0 && strcmp(sel->selector, "New") == 0) {
    // errors.New(string) -> error. Box the message into a heap goo_error and
    // store its pointer (as i8*) in the nullable error handle.
    if (!call->args) {
        codegen_error(codegen, expr->pos, "errors.New: expected a string argument");
        return NULL;
    }
    ValueInfo* msg = codegen_generate_expression(codegen, checker, call->args);
    if (!msg) return NULL;
    LLVMValueRef msg_val = msg->llvm_value;
    if (msg->is_lvalue && msg->goo_type) {
        LLVMTypeRef mt = codegen_type_to_llvm(codegen, msg->goo_type);
        if (mt) msg_val = LLVMBuildLoad2(codegen->builder, mt, msg_val, "errnew_msg");
    }
    value_info_free(msg);

    LLVMValueRef from_str = LLVMGetNamedFunction(codegen->module, "goo_error_from_string");
    LLVMTypeRef from_str_ty = LLVMGlobalGetValueType(from_str);
    LLVMValueRef args1[] = { msg_val };
    LLVMValueRef handle = LLVMBuildCall2(codegen->builder, from_str_ty, from_str, args1, 1, "errnew_box");

    Type* err_type = type_checker_error_type(checker);
    LLVMTypeRef err_llvm = codegen_type_to_llvm(codegen, err_type);
    LLVMValueRef is_null = LLVMConstInt(LLVMInt1TypeInContext(codegen->context), 0, 0);
    LLVMValueRef err_val = LLVMGetUndef(err_llvm);
    err_val = LLVMBuildInsertValue(codegen->builder, err_val, is_null, 0, "en.is_null");
    err_val = LLVMBuildInsertValue(codegen->builder, err_val, handle, 1, "en.ptr");
    return value_info_new(NULL, err_val, err_type);
}
```

- [ ] **Step 2: Build to verify it compiles**

Run: `eval "$(opam env --switch=default)"; make 2>&1 | tail -3`
Expected: builds with no new errors.

- [ ] **Step 3: Verify the existing `errors_new_probe` still passes**

Run: `bin/goo -o build/errors_new_probe examples/errors_new_probe.goo && build/errors_new_probe`
Expected: `got` (the `e != nil` path still works — is_null=0 unchanged).

- [ ] **Step 4: Commit**

```bash
git add src/codegen/call_codegen.c
git commit --no-gpg-sign -m "feat(errors): errors.New boxes the message into a heap goo_error"
```

---

### Task 3: Consumer — `err.Error()` + tag the error type

**Files:**
- Modify: `src/types/type_checker.c:114` (stamp `->name = "error"`)
- Modify: `src/types/expression_checker.c` (`type_check_selector_expr` — recognize `.Error()`)
- Modify: `src/codegen/call_codegen.c` (selector-call codegen — emit `.Error()`)
- Create: `examples/errors_new_error_probe.goo`, `examples/errors_new_error_probe.expected.txt`

**Interfaces:**
- Consumes: `goo_error_message` (Task 1), the boxed handle from `errors.New` (Task 2).
- Produces: `err.Error()` typechecks to `string` and codegens to the message `goo_string`. The error type now carries `->name = "error"`.

- [ ] **Step 1: Write the failing golden probe**

`examples/errors_new_error_probe.goo`:

```go
package main
import (
	"fmt"
	"errors"
)
func main() {
	e := errors.New("boom")
	fmt.Println(e.Error())
}
```

`examples/errors_new_error_probe.expected.txt`:

```
boom
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `bin/goo -o build/errors_new_error_probe examples/errors_new_error_probe.goo && build/errors_new_error_probe`
Expected: FAIL — typecheck rejects `.Error()` (selector on the error type) or codegen errors. (Records the gap.)

- [ ] **Step 3: Tag the error type**

In `src/types/type_checker.c`, `type_checker_error_type()` (line 114):

```c
Type* type_checker_error_type(TypeChecker* checker) {
    Type* t = type_nullable(type_pointer(type_checker_get_builtin(checker, TYPE_INT8)));
    if (t && !t->name) {
        t->name = str_dup("error");
    }
    return t;
}
```

- [ ] **Step 4: Typecheck `.Error()` in `type_check_selector_expr`**

In `src/types/expression_checker.c`, inside `type_check_selector_expr`, before the existing "Selector on non-struct, non-package type" rejection, add a branch: if the base type is the error type (`base->name && strcmp(base->name, "error") == 0`) and the selector is `"Error"`, return `string`:

```c
// error.Error() -> string (Phase 6). The error type is the tagged nullable
// handle (name=="error"); .Error() reads its boxed message.
if (base_type && base_type->name && strcmp(base_type->name, "error") == 0 &&
    strcmp(selector->selector, "Error") == 0) {
    return type_checker_get_builtin(checker, TYPE_STRING);
}
```

(Use whatever local already holds the resolved base type in this function — match the surrounding code's variable name; `selector->selector` is the member name.)

- [ ] **Step 5: Codegen `.Error()` in the selector-call path**

In `src/codegen/call_codegen.c`, where selector calls on a value receiver are handled (the method-call codegen path), add an early special case: when the receiver's `goo_type->name == "error"` and the selector is `"Error"`, emit the nil-guarded message read. Evaluate the receiver to the nullable value, extract `is_null` (field 0) and `ptr` (field 1), and `select` an empty string when null:

```c
// error.Error(): nil-guarded read of the boxed message.
// recv_val is the loaded nullable {i1 is_null, i8* handle}.
LLVMValueRef is_null = LLVMBuildExtractValue(codegen->builder, recv_val, 0, "err.is_null");
LLVMValueRef handle  = LLVMBuildExtractValue(codegen->builder, recv_val, 1, "err.handle");
LLVMValueRef msgfn = LLVMGetNamedFunction(codegen->module, "goo_error_message");
LLVMTypeRef  msgfn_ty = LLVMGlobalGetValueType(msgfn);
LLVMValueRef cargs[] = { handle };
LLVMValueRef msg = LLVMBuildCall2(codegen->builder, msgfn_ty, msgfn, cargs, 1, "err.msg");
// nil-guard: empty goo_string {null,0} when is_null
LLVMTypeRef str_llvm = codegen_get_basic_type(codegen, TYPE_STRING);
LLVMValueRef empty = LLVMGetUndef(str_llvm);
empty = LLVMBuildInsertValue(codegen->builder, empty,
    LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0)), 0, "empty.data");
empty = LLVMBuildInsertValue(codegen->builder, empty,
    LLVMConstInt(LLVMInt64TypeInContext(codegen->context), 0, 0), 1, "empty.len");
LLVMValueRef result = LLVMBuildSelect(codegen->builder, is_null, empty, msg, "err.error_result");
return value_info_new(NULL, result, type_checker_get_builtin(checker, TYPE_STRING));
```

Note: `goo_error_message` is called on both arms but the result is only `select`ed; passing a null handle is harmless because `goo_error_message` null-checks `e` (Task 1). If a strict no-call-on-null is preferred, branch+PHI instead — not required for correctness here.

- [ ] **Step 6: Run the probe to confirm it passes**

Run: `bin/goo -o build/errors_new_error_probe examples/errors_new_error_probe.goo && build/errors_new_error_probe`
Expected: `boom`

- [ ] **Step 7: Full gate**

Run: `eval "$(opam env --switch=default)"; make verify 2>&1 | tail -3 && make test 2>&1 | tail -4`
Expected: `verify: ALL GREEN GATES PASSED`, golden count incremented /0; `make test` 76/1.

- [ ] **Step 8: Commit**

```bash
git add src/types/type_checker.c src/types/expression_checker.c src/codegen/call_codegen.c examples/errors_new_error_probe.*
git commit --no-gpg-sign -m "feat(errors): err.Error() returns the message; tag error type"
```

---

### Task 4: Consumer — `fmt` prints an error's message

**Files:**
- Modify: `src/codegen/call_codegen.c` (the print/println emit path — `codegen_generate_print_call` / the `%v`/println arg dispatch)
- Create: `examples/fmt_println_error_probe.goo`, `examples/fmt_println_error_probe.expected.txt`

**Interfaces:**
- Consumes: `goo_error_message` (Task 1), the tagged error type (Task 3).
- Produces: an error-typed arg to `fmt.Println` / `%v` prints `<nil>` when null, else the message.

- [ ] **Step 1: Write the failing golden probe**

`examples/fmt_println_error_probe.goo`:

```go
package main
import (
	"fmt"
	"errors"
)
func main() {
	var a error = nil
	fmt.Println(a)
	b := errors.New("kaboom")
	fmt.Println(b)
}
```

`examples/fmt_println_error_probe.expected.txt`:

```
<nil>
kaboom
```

- [ ] **Step 2: Run to confirm it fails**

Run: `bin/goo -o build/fmt_println_error_probe examples/fmt_println_error_probe.goo && build/fmt_println_error_probe`
Expected: FAIL — prints a pointer/garbage or errors, not `<nil>`/`kaboom`. (If `var a error = nil` does not parse, fall back to a function returning `error` and `nil`; note in the commit.)

- [ ] **Step 3: Handle the error case in the print emit path**

In the println/`%v` per-arg dispatch in `src/codegen/call_codegen.c` (where it switches on the arg's `TypeKind`/type to pick `goo_print_*`), add an error case **before** the generic pointer/nullable handling: when `arg->goo_type->name == "error"`, emit `is_null ? print "<nil>" : goo_print_string(goo_error_message(handle))`. Reuse the `is_null`/`handle` extraction and the empty/`goo_error_message` pattern from Task 3 Step 5; for the `<nil>` literal use a global string `goo_string` and `goo_print_string` (mirror `codegen_generate_print_call`'s existing string emit, e.g. the `LLVMConstString("",0,0)` usage at call_codegen.c:1192 for the empty case). Select between the `<nil>` `goo_string` and the message `goo_string`, then call `goo_println_string` once.

- [ ] **Step 4: Run the probe to confirm it passes**

Run: `bin/goo -o build/fmt_println_error_probe examples/fmt_println_error_probe.goo && build/fmt_println_error_probe`
Expected:
```
<nil>
kaboom
```

- [ ] **Step 5: Full gate**

Run: `eval "$(opam env --switch=default)"; make verify 2>&1 | tail -3 && make test 2>&1 | tail -4`
Expected: ALL GREEN; 76/1.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/call_codegen.c examples/fmt_println_error_probe.*
git commit --no-gpg-sign -m "feat(fmt): print an error's message (<nil> when nil)"
```

---

### Task 5: Producer — `fmt.Errorf`

**Files:**
- Modify: `src/types/expression_checker.c:1138` (register `fmt.Errorf` in `stdlib_package_lookup`)
- Modify: `src/codegen/call_codegen.c:313` (route `fmt`/`Errorf` to a new `codegen_generate_errorf_call`) + add the function (mirror `codegen_generate_sprintf_call` at line 1665)
- Create: `examples/errorf_probe.goo`, `examples/errorf_probe.expected.txt`

**Interfaces:**
- Consumes: `fmt_emit_segments(... sprintf_mode=1 ...)`, `goo_error_from_string` (Task 1), `type_checker_error_type` (Task 3 tag).
- Produces: `fmt.Errorf(format, args...)` typechecks to `error` and codegens a boxed error from the Sprintf-built `goo_string`.

- [ ] **Step 1: Write the failing golden probe**

`examples/errorf_probe.goo`:

```go
package main
import (
	"fmt"
)
func main() {
	e := fmt.Errorf("bad: %s", "x")
	fmt.Println(e.Error())
}
```

`examples/errorf_probe.expected.txt`:

```
bad: x
```

- [ ] **Step 2: Run to confirm it fails**

Run: `bin/goo -o build/errorf_probe examples/errorf_probe.goo && build/errorf_probe`
Expected: FAIL — `fmt.Errorf` unknown (typecheck) / no codegen route.

- [ ] **Step 3: Register the typecheck return type**

In `src/types/expression_checker.c`, after the `fmt.Sprintf` block (line 1138):

```c
// fmt.Errorf(format string, args...) -> error  (Sprintf + box)
if (strcmp(package, "fmt") == 0 && strcmp(name, "Errorf") == 0) {
    return type_function(NULL, 0, type_checker_error_type(checker));
}
```

- [ ] **Step 4: Add the codegen function and route to it**

In `src/codegen/call_codegen.c`, add the forward declaration near line 15 and the route next to the Sprintf route (line 313):

```c
if (strcmp(pkg->name, "fmt") == 0 && strcmp(sel->selector, "Errorf") == 0) {
    return codegen_generate_errorf_call(codegen, checker, expr);
}
```

Add the function (model on `codegen_generate_sprintf_call`, line 1665): same literal-format guard and `fmt_emit_segments(..., 1, &result, ...)` to build the `goo_string`, then box and wrap as an error:

```c
static ValueInfo* codegen_generate_errorf_call(CodeGenerator* codegen,
                                               TypeChecker* checker,
                                               ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    if (!codegen || !checker || !expr || expr->type != AST_CALL_EXPR) return NULL;
    CallExprNode* call = (CallExprNode*)expr;
    ASTNode* fmt_arg = call->args;
    if (!fmt_arg || fmt_arg->type != AST_LITERAL ||
        ((LiteralNode*)fmt_arg)->literal_type != TOKEN_STRING) {
        codegen_error(codegen, expr->pos, "fmt.Errorf: format must be a string literal");
        return NULL;
    }
    const char* fmt_str = ((LiteralNode*)fmt_arg)->value;
    LLVMValueRef msg_str = NULL;
    if (!fmt_emit_segments(codegen, checker, fmt_str, fmt_arg->next, 1, &msg_str, expr->pos)) {
        return NULL;
    }
    LLVMValueRef from_str = LLVMGetNamedFunction(codegen->module, "goo_error_from_string");
    LLVMTypeRef from_str_ty = LLVMGlobalGetValueType(from_str);
    LLVMValueRef bargs[] = { msg_str };
    LLVMValueRef handle = LLVMBuildCall2(codegen->builder, from_str_ty, from_str, bargs, 1, "errorf_box");
    Type* err_type = type_checker_error_type(checker);
    LLVMTypeRef err_llvm = codegen_type_to_llvm(codegen, err_type);
    LLVMValueRef err_val = LLVMGetUndef(err_llvm);
    err_val = LLVMBuildInsertValue(codegen->builder, err_val,
        LLVMConstInt(LLVMInt1TypeInContext(codegen->context), 0, 0), 0, "ef.is_null");
    err_val = LLVMBuildInsertValue(codegen->builder, err_val, handle, 1, "ef.ptr");
    return value_info_new(NULL, err_val, err_type);
#endif
}
```

- [ ] **Step 5: Run the probe to confirm it passes**

Run: `bin/goo -o build/errorf_probe examples/errorf_probe.goo && build/errorf_probe`
Expected: `bad: x`

- [ ] **Step 6: Full gate**

Run: `eval "$(opam env --switch=default)"; make verify 2>&1 | tail -3 && make test 2>&1 | tail -4`
Expected: ALL GREEN; 76/1.

- [ ] **Step 7: Commit**

```bash
git add src/types/expression_checker.c src/codegen/call_codegen.c examples/errorf_probe.*
git commit --no-gpg-sign -m "feat(fmt): fmt.Errorf builds a formatted error"
```

---

### Task 6: Producer — `n, err := <!T>` carries the union's message

**Files:**
- Modify: `src/codegen/function_codegen.c:550-566` (the `?error` arm of the destructure bridge)
- Create: `examples/destructure_error_msg_probe.goo`, `examples/destructure_error_msg_probe.expected.txt`

**Interfaces:**
- Consumes: `codegen_error_union_get_error(codegen, rhs->llvm_value)` (already exists, error_union_codegen.c:100 — extracts the union's error-arm `goo_string`), `goo_error_from_string` (Task 1).
- Produces: the destructured `err` carries the union's message; `err.Error()` returns it.

- [ ] **Step 1: Write the failing golden probe**

`examples/destructure_error_msg_probe.goo`:

```go
package main
import (
	"fmt"
	"strconv"
)
func main() {
	n, err := strconv.Atoi("bad")
	if err != nil {
		fmt.Println(err.Error())
	}
	fmt.Println(n)
}
```

`examples/destructure_error_msg_probe.expected.txt`:

```
strconv.Atoi: invalid syntax
0
```

- [ ] **Step 2: Run to confirm it fails**

Run: `bin/goo -o build/destructure_error_msg_probe examples/destructure_error_msg_probe.goo && build/destructure_error_msg_probe`
Expected: FAIL — `err.Error()` reads the `inttoptr(1)` marker (garbage/segfault), not the message.

- [ ] **Step 3: Box the union's error arm on the error path (branch + PHI)**

In `src/codegen/function_codegen.c`, replace the marker logic at lines 556-563 (`is_null`, `non_null`, `null_ptr`, `err_ptr` select) with a branch that only boxes on the error path. `is_error` is already computed (line 527); `rhs->llvm_value` is the `!T` aggregate:

```c
LLVMValueRef is_null = LLVMBuildNot(codegen->builder, is_error, "err_is_null");
LLVMTypeRef i8pt = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);

// Branch: box the union's goo_string error arm only when is_error (keeps the
// common success path allocation-free). PHI the resulting i8* handle.
LLVMValueRef fn = LLVMGetBasicBlockParent(LLVMGetInsertBlock(codegen->builder));
LLVMBasicBlockRef box_bb   = LLVMAppendBasicBlockInContext(codegen->context, fn, "err.box");
LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(codegen->context, fn, "err.box.merge");
LLVMBasicBlockRef entry_bb = LLVMGetInsertBlock(codegen->builder);
LLVMBuildCondBr(codegen->builder, is_error, box_bb, merge_bb);

// box_bb: extract the error arm goo_string and box it.
codegen_set_insert_point(codegen, box_bb);
LLVMValueRef arm = codegen_error_union_get_error(codegen, rhs->llvm_value);
LLVMValueRef from_str = LLVMGetNamedFunction(codegen->module, "goo_error_from_string");
LLVMTypeRef from_str_ty = LLVMGlobalGetValueType(from_str);
LLVMValueRef fargs[] = { arm };
LLVMValueRef boxed = LLVMBuildCall2(codegen->builder, from_str_ty, from_str, fargs, 1, "err.boxed");
LLVMBuildBr(codegen->builder, merge_bb);
LLVMBasicBlockRef box_exit = LLVMGetInsertBlock(codegen->builder);

// merge_bb: PHI null (success) vs boxed (error).
codegen_set_insert_point(codegen, merge_bb);
LLVMValueRef err_ptr = LLVMBuildPhi(codegen->builder, i8pt, "err_ptr");
LLVMValueRef null_ptr = LLVMConstNull(i8pt);
LLVMAddIncoming(err_ptr, &null_ptr, &entry_bb, 1);
LLVMAddIncoming(err_ptr, &boxed, &box_exit, 1);
```

Keep the following `err_val` assembly (insert `is_null` at 0, `err_ptr` at 1) as-is.

- [ ] **Step 4: Run the probe to confirm it passes**

Run: `bin/goo -o build/destructure_error_msg_probe examples/destructure_error_msg_probe.goo && build/destructure_error_msg_probe`
Expected:
```
strconv.Atoi: invalid syntax
0
```

- [ ] **Step 5: Confirm the bridge's zero-on-error and existing probes still hold**

Run: `bin/goo -o build/erru_zero_on_error_probe examples/erru_zero_on_error_probe.goo && build/erru_zero_on_error_probe`
Expected: unchanged (n == 0 on error). Then the full gate:
Run: `eval "$(opam env --switch=default)"; make verify 2>&1 | tail -3 && make test 2>&1 | tail -4`
Expected: ALL GREEN; 76/1.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/function_codegen.c examples/destructure_error_msg_probe.*
git commit --no-gpg-sign -m "feat(errors): n,err destructure carries the union's message"
```

---

### Task 7: Capstone — `(int, error)` round-trip with real messages

**Files:**
- Create: `examples/error_messages_capstone.goo`, `examples/error_messages_capstone.expected.txt`

**Interfaces:**
- Consumes: everything above (errors.New / Errorf producers, `.Error()`, `(int,error)` tuple from the bridge milestone).
- Produces: an end-to-end proof; no source changes.

- [ ] **Step 1: Write the capstone probe**

`examples/error_messages_capstone.goo`:

```go
package main
import (
	"fmt"
	"strconv"
)
func parse(s string) (int, error) {
	n, err := strconv.Atoi(s)
	if err != nil {
		return 0, fmt.Errorf("parse %s: %s", s, err.Error())
	}
	return n, nil
}
func main() {
	n, err := parse("42")
	if err != nil {
		fmt.Println(err.Error())
	} else {
		fmt.Println(n)
	}
	_, err2 := parse("bad")
	if err2 != nil {
		fmt.Println(err2.Error())
	}
}
```

`examples/error_messages_capstone.expected.txt`:

```
42
parse bad: strconv.Atoi: invalid syntax
```

- [ ] **Step 2: Run the capstone**

Run: `bin/goo -o build/error_messages_capstone examples/error_messages_capstone.goo && build/error_messages_capstone`
Expected:
```
42
parse bad: strconv.Atoi: invalid syntax
```

If `fmt.Errorf` wrapping `err.Error()` exposes a gap (e.g. nested goo_string in the format walker), simplify the error message to `fmt.Errorf("parse failed: %s", s)` and note the limitation in the commit; the core round-trip (value `42`, a non-nil error with a message) is the must-pass.

- [ ] **Step 3: Full gate**

Run: `eval "$(opam env --switch=default)"; make verify 2>&1 | tail -3 && make test 2>&1 | tail -4`
Expected: `verify: ALL GREEN GATES PASSED`, golden +6 from the milestone start /0; `make test` 76/1.

- [ ] **Step 4: Commit**

```bash
git add examples/error_messages_capstone.*
git commit --no-gpg-sign -m "test(errors): Phase 6 capstone — (int,error) round-trip with messages"
```

---

## Verification (end-to-end)

1. `eval "$(opam env --switch=default)"` then `make verify` — every golden probe (including the 6 new ones) compiles via `bin/goo`, runs, and matches its `.expected.txt`; CompCert link gate green. Expect `verify: ALL GREEN GATES PASSED`.
2. `make test` — 76 passed, 1 skipped (the C unit suite; unaffected).
3. Spot-run the capstone: `bin/goo -o build/error_messages_capstone examples/error_messages_capstone.goo && build/error_messages_capstone` → `42` then `parse bad: strconv.Atoi: invalid syntax`.
4. Whole-branch review, then push + open PR; confirm `MERGEABLE`/`CLEAN` via `gh pr view` (CI is workflow_dispatch-only; the local gate is authoritative).

## Self-Review notes

- **Spec coverage:** Task 1 = runtime helpers; Task 2 = `errors.New` producer; Task 3 = `.Error()` consumer + type tag; Task 4 = `fmt` print; Task 5 = `fmt.Errorf`; Task 6 = destructure producer; Task 7 = capstone + gate. All spec components mapped.
- **Risk (spec Task 2 note):** if stamping `->name="error"` regresses `type_equals` (the bridge / `== nil` / multi-return), fall back to structural recognition of the error type and gate `.Error()`/`fmt` on the structural shape. Watch `make verify` after Task 3 Step 3 specifically — a green gate there clears this risk.
- **Reused, not rebuilt:** `goo_error` struct + `goo_alloc`, `codegen_error_union_get_error`, `codegen_generate_sprintf_call`/`fmt_emit_segments`, `add_runtime_function`, `stdlib_package_lookup`, `type_checker_error_type`.
