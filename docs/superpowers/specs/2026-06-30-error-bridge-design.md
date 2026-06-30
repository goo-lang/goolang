# Error bridge: `n, err := <!T>` + strconv.Atoi (makes `(int,error) == !int` real)

**Date:** 2026-06-30
**Branch:** `feat/v1-error-bridge`
**Phase:** v1 Phase 5 (Go-source compatibility + stdlib)
**Supersedes:** the native-tuple-vs-strict-!T discussion in `2026-06-30-go-error-model-design.md`.
The chosen approach is **native-tuple + bridge**; the `(int,error)` result-tuple already works
(PR #55 fixed the `nil`-in-nullable-tuple-field crash). This milestone adds the missing bridge so
a `!T`-returning function (like `strconv.Atoi`) is consumable the Go way.

## Problem

`strconv.Atoi` returns `!int` (Goolang's differentiator). To let Go code call it —
`n, err := strconv.Atoi(s); if err != nil { … }` — a `!T` value must destructure into
`(value, ?error)`. Today `n, err := <!T>` does NOT reject but is WRONG: the multi-LHS destructure
treats the `!T` as a generic 2-field struct and `ExtractValue`s its raw fields, so `n` gets the
`is_error` flag and `err` gets the union data (a latent miscompile).

## Groundwork (verified)
- `(int, error)` result-tuple producing/consuming works (PR #55). `error` = `?*int8`; `nil` and
  `if err != nil` work via the nullable nil-compare.
- `!T` layout: `{ i1 is_error, union{ value, error } }`. `ExtractValue 0` = is_error;
  `catch`/`try` unwrap the value arm (error_union_codegen.c — mirror its value extraction).
- Multi-LHS destructure: typecheck at type_checker.c:604 (`name_count==2 && is_short_decl`);
  codegen at function_codegen.c:588 (generic struct-return: `ExtractValue` fields 0/1).
- Atoi runtime ready: `goo_string_to_int(goo_string_t, int64_t* out) -> int ok` (shipped #55).
  Error-union constructors `codegen_create_error_union_success/_error` exist.

## Components (tasks)

### Task 1 — the `!T → (value, ?error)` destructure bridge
- **Typecheck** (type_checker.c:604 multi-LHS-short-decl, and the `=` multi-assign at ~961): when
  the single RHS is `TYPE_ERROR_UNION` and there are 2 targets, bind `target[0] : value_arm type`
  and `target[1] : error` (the `?*int8` nullable error type — `type_from_ast`'s `error` resolution).
  Do NOT treat the `!T` as a generic struct.
- **Codegen** (function_codegen.c:588 and the multi-assign equivalent): when the RHS is `!T`:
  - `is_error = ExtractValue(u, 0)`.
  - `target0 (n)` = the unwrapped value arm (mirror the `catch`/`try` value extraction in
    error_union_codegen.c — extract union field 1 as the value type). Go semantics permit `n` to be
    the zero/garbage value when `is_error`; v1 may extract the value arm directly (the caller checks
    `err` first). If trivial, `select(is_error, zero(T), value_arm)` is preferred.
  - `target1 (err)` = a `?error` (`{i1 is_null, *int8}`): `is_null = NOT is_error`; pointer =
    `select(is_error, <non-null marker>, null)`. For v1 control-flow this only needs correct
    nil-ness (printing the message is the deferred `.Error()`).
- **Acceptance:** with a helper `func mk(b bool) !int { … }`, `n, err := mk(true)` gives `n` = value,
  `err == nil`; `mk(false)` gives `err != nil`. Coexists with `x := mk(b) catch e {…}`.

### Task 2 — strconv.Atoi → !int
- Register `strconv.Atoi → type_error_union(int64, error_t)`; codegen builds the `!int` via
  `goo_string_to_int` + `codegen_create_error_union_success/_error` (the message arm via the
  `error("…")` builtin codegen pattern). See the prior strconv plan's Atoi task for exact codegen.
- **Acceptance:** `x := strconv.Atoi("42") catch e {…}` → 42; `strconv.Atoi("nope")` → error path.

### Task 3 — errors.New(string) error
- Register `errors` package (in `stdlib_packages[]` AND `stdlib_package_lookup`) +
  `errors.New(string) -> error`. Codegen: produce a non-nil `?error` value carrying the message
  (or a non-null marker pointer for v1). Enables `return 0, errors.New("…")` on the producing side.
- **Acceptance:** `var e error = errors.New("x"); if e != nil { fmt.Println("got") }` → `got`.

### Task 4 — capstone: the Go round-trip as a `.go` file
- `examples/error_roundtrip_probe.goo` (or `.go`; golden needs `.goo`, so use `.goo` for the gate
  and note a `.go` manual check):
```go
package main
import ("fmt"; "strconv")
func parse(s string) (int, error) {
	n, err := strconv.Atoi(s)   // !T -> (value, ?error) bridge (Task 1)
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
- **Golden expected:** `42` then `caught`. This is the full `(int,error) == !int`: `Atoi`'s `!int`
  consumed as `(n, err)`, propagated into a `(int,error)` function, re-consumed by `parse`'s caller.

## Testing
Per-task golden probes (bridge ok/err via a local `!int` helper; Atoi catch ok/err; errors.New nil);
the capstone. Gate: `make verify` ALL GREEN (incl. ccomp), golden green, `make test` 76/1.

## Out of scope (follow-up)
`error.Error()` message method (Phase 6) — so printing `err` directly is deferred; `errors.Is/As/%w`;
producing-side `return v, e` where `e` is a runtime-variable `?error` of unknown nil-ness needing a
runtime arm-branch (the literal `nil` and known-non-nil cases are covered; the general var case can
be a follow-up if a probe needs it).

## Risk
- The bridge must NOT change genuine `(int,int)`/struct-return destructure — gate strictly on the RHS
  being `TYPE_ERROR_UNION`. Mitigated by a `(int,int)` regression probe.
- The value-arm extraction must match how `catch`/`try` unwrap, or `n` is garbage even on the ok path.
  Mitigated by the bridge-ok probe asserting `n`'s exact value.
- `err`'s `?error` nil-ness must align with `is_error` (nil ⟺ !is_error) so `if err != nil` is correct.
  Mitigated by the capstone exercising both arms.
