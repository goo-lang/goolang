# strconv.Itoa / strconv.Atoi

**Date:** 2026-06-30
**Phase:** v1 Phase 5 (stdlib surface)
**Trigger:** the TinyGo stdlib recon — `strconv` is a top missing package; `Itoa`/`Atoi`
(int↔string) are the most-used functions. `Itoa` reuses the `goo_int_to_string` helper
added by the fmt.Printf milestone (PR #54).

## Problem

`strconv.Itoa(123)` / `strconv.Atoi("42")` → "Undefined variable 'strconv'": the package
is not registered in `stdlib_package_lookup`.

## Architecture decision

`strconv.Atoi` returns **`!int`** (a Goolang error union), NOT Go's `(int, error)`. Error
unions are Goolang's differentiator and already have full machinery (`TYPE_ERROR_UNION`,
the `error(msg)` builtin, `try`/`catch`, the success/error codegen constructors). `Atoi`
returning `!int` is the idiomatic Goolang shape and integrates with `try`/`catch` directly.
(`errors.New` and Go-style `(int, error)` multi-return are out of scope.)

## Components

### 1. Typecheck (`src/types/expression_checker.c` `stdlib_package_lookup`)
- `strconv.Itoa`: `type_function(NULL, 0, string_t)`.
- `strconv.Atoi`: `type_function(NULL, 0, type_error_union(int_t, error_t))` — `!int`. Read
  how the `error(msg)` builtin builds its error-union return type (expression_checker.c
  ~:827-842) to construct the same `!int` type (`int_t` = TYPE_INT64 builtin; `error_t` =
  whatever error type the existing error-union uses).

### 2. Runtime (`src/runtime/runtime.c` + `include/runtime.h`)
- `goo_int_to_string` already exists (reused by Itoa). **Add the bundled truncation guard**
  (the PR #54 follow-up): after `snprintf`, `if (n < 0 || (size_t)n >= sizeof(buf)) goo_panic(...)`
  in `goo_int_to_string` and `goo_float_to_string` — now that `Itoa` reuses the int helper.
- New: `int goo_string_to_int(goo_string_t s, int64_t* out)` — parse `s` as a base-10 signed
  integer; on success store into `*out` and return 1; on empty/invalid/overflow return 0.
  Use `strtoll` with full-consumption + `errno`/`endptr` validation (reject trailing junk,
  empty, overflow). Declare it in the module (`runtime_integration.c`, near `goo_string_concat`):
  param = the goo_string struct + an i64* out-pointer, returns i32.

### 3. Codegen (`src/codegen/call_codegen.c` — `codegen_generate_stdlib_call` / the package dispatch ~:296-320)
- **Itoa**: emit `goo_int_to_string(arg)` (SExt arg to i64), return the resulting string value.
- **Atoi**: alloca an `i64 out`; emit `ok = goo_string_to_int(arg, &out)`; build the `!int`
  error union by branching on `ok`:
  - success → `codegen_create_error_union_success(union_type, load(out))`
  - failure → `codegen_create_error_union_error(union_type, error_value)` where `error_value`
    is the error payload the existing `error("...")` codegen produces (reuse that path with a
    literal message like `"strconv.Atoi: invalid integer"`).
  Read `codegen_create_error_union_success`/`_error` (error_union_codegen.c:11/56) and how the
  `error()` builtin codegen builds the error payload, and mirror the union LLVM type the
  typechecker assigned. Return a ValueInfo of the `!int` type.

## Data flow

`v := try strconv.Atoi("42")` → `goo_string_to_int({"42",2}, &out)`=1 → success-union(out=42) →
`try` unwraps to 42. `strconv.Atoi("x")` → ok=0 → error-union → `try` propagates / `catch` fires.

## Testing (golden, TDD)

- `itoa_probe`: `fmt.Println(strconv.Itoa(123))` → `123`; negative `strconv.Itoa(-7)` → `-7`.
- `atoi_ok_probe`: `catch`-style or value-extracting the success arm of `strconv.Atoi("42")` → `42`.
  (Use the existing `catch` idiom the erru golden probes use; read one for the exact syntax.)
- `atoi_err_probe`: `strconv.Atoi("nope")` failure is caught/handled → a recovery value printed.
- Truncation guard: covered by build + existing int-printing probes (the guard is unreachable
  with valid ints; assert no regression).

Gate: `make verify` ALL GREEN (incl. ccomp), golden green (+ new probes), `make test` 76/1.

## Out of scope (follow-up)
`errors.New` (overlaps the `error()` builtin); `strconv.FormatInt`/`ParseInt`/`Quote`/`ParseFloat`/
`ParseBool`; bases other than 10; Go-style `(int, error)` multi-return.

## Risk
- The `!int` union LLVM type at the call site must match the type the typechecker assigned to
  `strconv.Atoi`'s return (so `try`/`catch` consume it correctly). Mitigated by reading how the
  `error()` builtin's return union is typed + the atoi_ok/atoi_err golden probes exercising the
  full try/catch path.
- `goo_string_to_int` must reject partial parses (`"12x"`), empty, and overflow — not silently
  return a wrong value. Covered by the atoi_err probe + careful strtoll validation.
