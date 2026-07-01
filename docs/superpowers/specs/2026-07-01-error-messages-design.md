# Error messages: `err.Error()` + `fmt.Errorf` (Phase 6, message-carrying errors)

**Date:** 2026-07-01
**Branch:** `feat/v1-error-messages`
**Phase:** v1 Phase 6 (first post-bridge error milestone)
**Builds on:** `2026-06-30-error-bridge-design.md` (PR #56, merged). The bridge made
`(int, error) == !int` real but `error` was a non-null **marker** with no message. This
milestone gives `error` a real message and the `.Error()` accessor.

## Problem

After the error bridge, `error` = `?*int8` is a nullable pointer used only as a presence
**marker**: `errors.New(msg)` evaluates then *discards* `msg` (call_codegen.c:420), and the
`n, err := <!T>` destructure derives `err` from the union's `is_error` flag while throwing the
union's `goo_string` message away (function_codegen.c:559, `inttoptr(1)` marker). So:

- `err.Error()` does not exist — there is no message to return.
- `fmt.Println(err)` / `%v` cannot show a message.
- `fmt.Errorf` does not exist.

Phase 6 (scope: **Core + `fmt.Errorf`**, no `%w` wrapping / `Is` / `As`) makes errors carry their
text end-to-end.

## Chosen approach — Approach B (Go-faithful nullable heap pointer)

`error` carries its message as a pointer to a heap-allocated `goo_error`. This matches Go
(`errors.New` → heap `*errorString`) and is forward-compatible with `%w` wrapping: the *pointee*
grows (the `goo_error.cause` field already exists) without ever changing `error`'s shape.

**Key realization:** the type stays `?*int8` at the type-system level; codegen reinterprets the
`i8*` as an opaque **handle to a heap `goo_error_t`**. This keeps every nullable mechanism
(`!= nil`, the `(int,error)` tuple null-insert, the `is_null` bridge math) working with **zero**
changes — we only replace the `inttoptr(1)` marker with a real boxed pointer.

Rejected alternatives (recorded for the decision trail):
- **A — `error = ?string`** (nullable string, message in payload): smaller, alloc-free, but not
  pre-shaped for `%w` wrapping (payload must widen later). Lost to B on the forward-compat horizon.
- **C — `error = goo_string`** (null data-ptr = nil): abandons the nullable nil-check shape, forcing
  `!= nil` and the tuple null-insert to special-case; strictly dominated by A. Rejected.

## Groundwork (verified)

- Runtime already defines `struct goo_error { const char* message; int code; struct goo_error* cause; }`
  with `goo_new_error()`, `goo_error_free()` (runtime.c:84, runtime.h:21). `cause` is the future
  `%w` chain. `goo_alloc(size)` is the allocator (runtime.c:43).
- `error` SSOT: `type_checker_error_type()` returns `type_nullable(type_pointer(TYPE_INT8))`
  (type_checker.c:114) — the single seam, called by the `error` keyword, the `n,err` bind, and
  `errors.New`.
- `!T` error arm carries a real `goo_string` (type_mapping.c:269; Atoi stores
  `"strconv.Atoi: invalid syntax"` at call_codegen.c:927). `catch` already prints it.
- The destructure bridge (function_codegen.c:517–578) builds `err` as
  `{is_null = NOT is_error, ptr = select(is_error, inttoptr(1), null)}`.
- `fmt.Printf`/`fmt.Sprintf` share `fmt_emit_segments` (Sprintf mode builds a `goo_string` via
  `goo_string_concat` + `goo_int/float/bool_to_string`); format must be a string literal.
- Atoi error path already uses cond-br + PHI (call_codegen.c:925) — the precedent for gating an
  allocation onto the error-only path.

## Components (tasks)

### Task 1 — Runtime: box and read a message
- `goo_error_t* goo_error_from_string(goo_string_t msg)` (runtime.c): `goo_alloc` a `goo_error`,
  copy `msg.data[0..length]` into a `goo_alloc`'d `length+1` buffer with a trailing NUL, set
  `message` to it, `code = -1`, `cause = NULL`. (Does not assume `msg.data` is NUL-terminated.)
- `goo_string_t goo_error_message(goo_error_t* e)`: return `{ e->message, strlen(e->message) }`.
  Embedded-NUL truncation is an accepted v1 edge (error messages are ASCII in practice).
- Register both in `runtime_integration.c` so codegen can emit calls
  (`goo_error_from_string : (goo_string) -> i8*`-ish handle; `goo_error_message : (i8*) -> goo_string`).
  Signatures use the opaque `i8*` handle to avoid introducing a `goo_error` LLVM struct type.

### Task 2 — Type seam: tag the error type
- In `type_checker_error_type()` stamp `->name = "error"` on the returned nullable (one line; the
  shape stays `?*int8`). This lets `.Error()` dispatch and `fmt` recognize errors **precisely**
  rather than firing on any user `?*int8`/`?*byte`.
- Verify `type_nullable` tolerates a post-construction `->name` and that `type_equals` (structural)
  still treats a tagged `error` as compatible everywhere it is today (the bridge, `== nil`,
  multi-return). If `type_equals` regresses on the name, compare structurally and gate `.Error()` on
  `name=="error"` only.

### Task 3 — Producer: `errors.New` carries the message
- call_codegen.c:411: evaluate the arg to a `goo_string`; call `goo_error_from_string` →
  `goo_error*`; bitcast to `i8*`; return the nullable `{is_null=0, ptr=handle}`. Removes the
  discard-and-`inttoptr(1)` code.

### Task 4 — Producer: `fmt.Errorf`
- Typecheck: register `fmt.Errorf(format string, args...) -> error` (mirror `fmt.Sprintf`
  registration); same *format-must-be-literal* / verb / arg-count diagnostics as Printf/Sprintf.
- Codegen (new `fmt.Errorf` selector route): run the Sprintf-mode `fmt_emit_segments` to build a
  `goo_string`, then box it via `goo_error_from_string` and return the nullable error. `Errorf` =
  `Sprintf` + box.

### Task 5 — Producer: `n, err := <!T>` carries the union's message
- function_codegen.c:559: replace the `inttoptr(1)` marker. **Branch on `is_error`**:
  - error block: extract the union's error arm `goo_string` (add helper
    `codegen_error_union_get_error` — ExtractValue data-union field 1, mirroring
    `codegen_error_union_get_value`), call `goo_error_from_string`, bitcast to `i8*`.
  - success block: `null` `i8*`.
  - PHI the two into `err_ptr`. `is_null = NOT is_error` is unchanged.
- The branch keeps the **common success path allocation-free** (no box on the no-error path).
- **Assumption:** the `!T` error arm is the default `goo_string` (the case the `error(msg)`
  differentiator and `strconv.Atoi` produce). A `!T` with an explicit non-string error arm is out of
  scope here; if one is destructured, keep the current non-null marker (no message) rather than
  mis-boxing — a clean degradation, not a crash.

### Task 6 — Consumer: `err.Error()`
- Typecheck (expression_checker.c selector path): base type `name=="error"`, selector `"Error"`,
  zero args → returns `TYPE_STRING`.
- Codegen: extract `ptr` from the nullable, **nil-guard** (if `is_null`/null, produce an empty
  `goo_string {null, 0}` rather than dereferencing — safer than Go's nil-panic, chosen for v1),
  else bitcast `i8*`→`goo_error*` and call `goo_error_message`.

### Task 7 — Consumer: `fmt` prints the message
- In the fmt/println emit path: when an arg's type `name=="error"`, emit
  `is_null ? print "<nil>" : print goo_error_message(handle)`. Applies to `fmt.Println(err)` and
  `%v`. (`%v`/`Println` already dispatch on arg type; add the `error` case alongside the scalar/string cases.)

### Task 8 — Tests (golden probes — the v1 oracle)
- `errors_new_error_probe` — `errors.New("boom").Error()` → `boom`
- `errorf_probe` — `fmt.Errorf("bad: %s", "x").Error()` → `bad: x`
- `destructure_error_msg_probe` — `n, err := strconv.Atoi("bad"); if err != nil { println(err.Error()) }`
  → `strconv.Atoi: invalid syntax`, and `n == 0`
- `fmt_println_error_probe` — a nil error prints `<nil>`; a non-nil error prints its message
- `error_messages_capstone` — `func parse(s) (int, error)` built from `errors.New` / `fmt.Errorf`;
  caller prints `err.Error()` on failure and the value on success
- Gate: `make verify` ALL GREEN + `make test` 76/1.

## Data flow

```
errors.New(msg) ─┐
fmt.Errorf(...)  ├─► goo_error_from_string ─► heap goo_error{message} ─► i8* handle ─► {is_null=0, ptr}
n,err:=<!T> (err)┘                                                                          │
                                                                                           ▼
                                          err.Error() / fmt  ◄── goo_error_message(handle) ◄┘
nil error ──────────────────────────────────────────────────────► {is_null=1, null}
```

## Error handling / edges
- **`nil.Error()`**: codegen nil-guard returns `""` (does not dereference null). Chosen over Go's
  nil-panic for v1 safety.
- **Non-literal `Errorf` format**: same clean compile error as Printf/Sprintf (reused diagnostic).
- **Embedded NUL** in a message: `strlen` truncates on read (documented v1 edge).
- **Memory**: boxed errors leak (no `free`), consistent with the rest of the v1 runtime;
  `goo_error_free` exists for a later GC/cleanup pass.

## Out of scope (deferred to a later milestone)
- `%w` wrapping, `errors.Is`, `errors.As`, `errors.Unwrap`, `fmt.Errorf` with `%w`.
- Interface-backed `error` (Goolang's `error` remains a special nullable handle, not a general
  interface). When interfaces and `error` unify, `.Error()` becomes ordinary method dispatch.
- `goo_error.code` is set to `-1` and unused; reserved for a future errno-style API.
