# Error wrapping: `fmt.Errorf("%w", err)` + `errors.Unwrap` (Phase 6b)

**Date:** 2026-07-01
**Branch:** `feat/v1-error-wrapping`
**Phase:** v1 Phase 6b (error-chain core; follows Phase 6 error messages, PR #57 merged)
**Builds on:** `2026-07-01-error-messages-design.md`. Phase 6 chose Approach B (`error` = opaque
`i8*` handle to a heap `goo_error`) specifically so wrapping could be added by using the struct's
existing `cause` field without changing `error`'s shape. This milestone cashes that in.

## Problem

Go composes error chains: `fmt.Errorf("outer: %w", inner)` produces an error whose message is
`"outer: " + inner.Error()` AND which records `inner` as a wrapped cause, retrievable via
`errors.Unwrap`. Goolang has `fmt.Errorf` (Phase 6) but no `%w` verb and no `errors.Unwrap`, so
error chains can't be built or traversed.

**Scope (chosen): wrapping core only** — `%w` + `errors.Unwrap` + chain-aware messages.
`errors.Is` (sentinel-identity chain walk) and `errors.As` (needs concrete error types Goolang
lacks — its `error` is one opaque handle) are DEFERRED to a later milestone; `As` in particular
would first require an interface-backed error model.

## Key realization

In Go, `%w` renders the wrapped error's message into the outer string **at creation time** (exactly
like `%v`), and separately records the wrapped error for `Unwrap`. So `.Error()` needs **no change** —
it already returns the stored, pre-composed message, which contains the chain text. The only new
state is the `cause` pointer. This milestone is therefore small and surgical: teach `Errorf`'s `%w`
to render + capture a cause, and add `errors.Unwrap` to read it back.

## Groundwork (verified on merged `main`)

- `struct goo_error { const char* message; int code; struct goo_error* cause; }` (runtime.h:21) —
  `cause` already exists and is used nowhere yet.
- `goo_error_from_string(goo_string) -> goo_error*` (sets `cause=NULL`), `goo_error_message(goo_error*)
  -> goo_string` (Phase 6). `error` = `?*int8` tagged `->name="error"`; `type_is_error(Type*)`
  predicate (types.c:713).
- `fmt.Errorf` = `codegen_generate_errorf_call` (call_codegen.c ~1797): literal-format guard + the
  shared walker `fmt_emit_segments(..., sprintf_mode=1, ...)` builds a `goo_string`, then boxes via
  `goo_error_from_string`.
- The shared walker `fmt_emit_segments` (call_codegen.c ~1240+) handles `%d %s %f %t %v %%`, is used by
  Printf (mode 0) and Sprintf/Errorf (mode 1); the error display path
  `codegen_error_display_string(codegen, err_loaded, pos)` (call_codegen.c, Phase 6b hardening) builds
  the `select(is_null, "<nil>", goo_error_message(handle))` goo_string.
- `errors.New` is registered in `stdlib_package_lookup` (expression_checker.c ~1250) and routed in
  `call_codegen.c` (~411); `errors.Unwrap` mirrors this registration/route.

## Chosen approach — `wrap_out` out-param on the shared walker

`fmt_emit_segments` gains a `LLVMValueRef* wrap_out` parameter. Printf/Sprintf callers pass `NULL`;
`Errorf` passes `&cause`. `%w` handling lives in the one shared walker, so `%w` is rejected in
Printf/Sprintf for free and single-`%w` is enforced via the out-param.

Rejected: a dedicated Errorf walker (duplicates ~100 lines of verb handling, drifts); a two-pass
`%w`→`%v` format rewrite (fragile string rewriting + arg-index bookkeeping).

Runtime helpers vs GEP: two runtime helpers (`goo_error_wrap`, `goo_error_unwrap`) rather than
introducing a `goo_error` LLVM struct to GEP — keeps the Phase 6 opaque-`i8*`-handle invariant intact
(only these helpers know the struct layout).

## Components (tasks)

### Task 1 — Runtime: wrap + unwrap
- `goo_error_t* goo_error_wrap(goo_string_t msg, goo_error_t* cause)` (runtime.c): identical to
  `goo_error_from_string` but sets `error->cause = cause` instead of `NULL`.
- `goo_error_t* goo_error_unwrap(goo_error_t* e)` (runtime.c): `return e ? e->cause : NULL;`.
- Declare in runtime.h; register in runtime_integration.c: `goo_error_wrap : (goo_string, i8*) -> i8*`;
  `goo_error_unwrap : (i8*) -> i8*`.

### Task 2 — `%w` in the shared walker
- Add `LLVMValueRef* wrap_out` to `fmt_emit_segments`'s signature; update the two existing call sites
  (Printf codegen, Sprintf codegen) to pass `NULL`.
- Add `w` to the walker's recognized-verb set (alongside `d/s/f/t/v`) so it isn't rejected as an
  unknown verb before the branch runs; `%w` consumes one argument like the other verbs (advance the
  arg cursor).
- Add a `%w` verb branch:
  - If the arg is not an error (`!type_is_error(arg_val->goo_type)`) → clean
    `codegen_error` `"fmt.Errorf: %%w requires an error argument"`.
  - If `wrap_out == NULL` → clean error `"%%w is only valid in fmt.Errorf"`.
  - If `*wrap_out != NULL` (already set) → clean error `"fmt.Errorf: multiple %%w not supported (v1)"`.
  - Else: render the wrapped message with `codegen_error_display_string(...)` and concat into the
    accumulator (mode 1 is the only mode that reaches here, since NULL `wrap_out` rejects `%w`);
    extract the arg's handle (field 1 of the loaded nullable) and store it into `*wrap_out`.

### Task 3 — `Errorf` boxes with the cause
- `codegen_generate_errorf_call`: declare `LLVMValueRef cause = NULL`; pass `&cause` to
  `fmt_emit_segments`. After the message goo_string is built: if `cause != NULL`, box via
  `goo_error_wrap(msg, cause)`; else `goo_error_from_string(msg)` (unchanged). Same nullable-error
  assembly `{is_null=0, handle@1}`.

### Task 4 — `errors.Unwrap`
- Typecheck: register `errors.Unwrap(error) -> error` in `stdlib_package_lookup` (returns
  `type_function(NULL, 0, type_checker_error_type(checker))`); the single-arg must be an error.
- Codegen (new `errors`/`Unwrap` route next to `errors.New`): evaluate the arg to the loaded nullable
  error; extract the handle (field 1); call `goo_error_unwrap` → `goo_error*` cause; build the nullable
  result `{ is_null = icmp eq(cause, null), ptr = cause }` typed `type_checker_error_type`. A nil or
  non-wrapped input yields nil (the runtime helper null-guards, and a null cause → is_null=1).

### Task 5 — Tests (golden probes) + capstone
- `errorf_wrap_probe` — `fmt.Errorf("outer: %w", errors.New("inner")).Error()` → `outer: inner`
- `unwrap_probe` — `e := fmt.Errorf("outer: %w", errors.New("inner")); errors.Unwrap(e).Error()` → `inner`
- `unwrap_nil_probe` — `if errors.Unwrap(errors.New("x")) == nil { println "nil" }` → `nil`
- `error_wrap_capstone` — 2-level chain: `a=errors.New("root"); b=fmt.Errorf("mid: %w", a);
  c=fmt.Errorf("top: %w", b)`; `c.Error()` → `top: mid: root`; `errors.Unwrap(c).Error()` → `mid: root`;
  `errors.Unwrap(errors.Unwrap(c)).Error()` → `root`
- Gate: `make verify` (golden +4/0, ALL GREEN) + `make test` 76/1.
- **Negative cases are NOT golden probes** (golden runs successful programs only): `%w` with a
  non-error arg, `%w` in `Printf`/`Sprintf`, and multiple `%w` are compile-time diagnostics — verify
  each manually with a throwaway `.goo` that must fail to compile with the expected message, documented
  in the task report; do not add them to `examples/`.

## Data flow

```
fmt.Errorf("outer: %w", inner)
   walker: render inner's message into acc  +  *wrap_out = inner.handle
   Errorf: cause!=NULL -> goo_error_wrap(acc, cause) -> {is_null=0, handle}
errors.Unwrap(e)
   goo_error_unwrap(e.handle) -> cause -> {is_null = cause==null, ptr = cause}
.Error()  (unchanged) -> the stored, already-composed message
```

## Error handling / edges
- `%w` non-error arg / `%w` outside Errorf / multiple `%w` → clean compile-time diagnostics.
- `errors.Unwrap` of nil or non-wrapped error → nil (runtime null-guard + null cause → is_null=1).
- `%w` of a *nil* error → the display helper renders `<nil>` into the message and `cause` is set to the
  nil error's null handle (Unwrap later yields nil). No crash.
- Boxed errors (incl. wrapped chains) leak — consistent with the v1 runtime; `goo_error_free` frees a
  chain recursively but is unused for now.

## Out of scope (deferred)
- `errors.Is` (sentinel-identity chain walk) and `errors.As` (needs concrete error types /
  interface-backed errors). `fmt.Errorf` with multiple `%w` (Go 1.20+). `errors.Join`.
