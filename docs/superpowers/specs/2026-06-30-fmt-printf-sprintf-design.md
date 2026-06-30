# fmt.Printf / fmt.Sprintf

**Date:** 2026-06-30
**Phase:** v1 Phase 5 (stdlib surface)
**Trigger:** the TinyGo stdlib recon — `fmt.Printf`/`Sprintf` are the highest-frequency
missing stdlib functions; the flagship `hello.goo` and nearly every real Go program need
formatted output.

## Problem

`fmt.Printf("x=%d\n", 42)` → "Package 'fmt' has no member 'Printf'"; `fmt.Sprintf` likewise.
Only `fmt.Println` exists (typed `(NULL, 0, void)`, codegen walks args and dispatches per
type to `goo_print_int/string/bool/float`).

## Architecture decision

**Compile-time format-string parsing.** The format argument must be a string literal;
codegen walks it at compile time, splitting it into literal chunks and verbs, and emits the
matching runtime calls interleaved. A non-literal format (`Printf(fmtVar, …)`) is a clean
`codegen_error`/`type_error` — not supported in v1.

Rejected: a runtime `goo_printf(fmt, ...)` with C varargs — Go-faithful for variable formats
but fragile ABI (the slice-by-pointer rule makes C-varargs aggregate passing risky) plus a
whole runtime format engine. Compile-time covers the overwhelming majority of real usage.

## Verbs (v1)

`%d` int · `%s` string · `%f` float · `%t` bool · `%v` default-per-type · `%%` literal `%`.
Width/precision/flags (`%5.2f`) are OUT of scope (clean error on an unsupported verb).

## Components

### 1. Typecheck (`src/types/expression_checker.c` `stdlib_package_lookup`)
Register `fmt.Printf` and `fmt.Sprintf` as variadic, mirroring `Println`'s
`type_function(NULL, 0, void_t)` shape. `Printf` returns void; `Sprintf` returns string
(`type_function(NULL, 0, string_t)`). The format (first) arg's string-ness is enforced at
codegen (where the literal is required anyway).

### 2. Codegen — shared format walker (`src/codegen/call_codegen.c`)
A helper `format_segments(const char* fmt)` (or inline walk) that yields an ordered list of
{literal-chunk | verb}. The Printf and Sprintf entry points share the walk:
- **Printf** (`codegen_generate_printf_call`): for each literal chunk emit `goo_print(chunk)`;
  for each verb emit the matching `goo_print_<type>(arg)` using the next argument (reusing the
  existing per-type print helpers, exactly as `codegen_generate_println_call` dispatches).
  `%v` dispatches on the arg's `goo_type` (int→`goo_print_int`, etc.).
- **Sprintf** (`codegen_generate_sprintf_call`): build the result `goo_string_t` by
  `goo_string_concat`-ing, in order: each literal chunk (as a goo_string) and each verb's arg
  converted to a goo_string via the new runtime helpers. Return the accumulated string value.

Verb/argument-count mismatch (too few/many args for the verbs) is a clean `codegen_error`.
A non-string-literal format arg is a clean error.

### 3. Runtime (Sprintf only) — `src/runtime/runtime.c` + `include/runtime.h`
New: `goo_string_t goo_int_to_string(int64_t)`, `goo_string_t goo_float_to_string(double)`,
`goo_string_t goo_bool_to_string(int)` (snprintf into a heap goo_string). `%s` uses the arg's
goo_string directly. These helpers are **reusable for a later `strconv.Itoa` milestone**.

## Data flow

`fmt.Sprintf("a=%d!", n)` →
walk → [lit "a=", verb %d, lit "!"] →
`goo_string_concat(goo_string_concat(lit("a="), goo_int_to_string(n)), lit("!"))` → return.

`fmt.Printf("a=%d!\n", n)` →
`goo_print(lit "a=")`; `goo_print_int(n)`; `goo_print(lit "!\n")`.

## Testing (golden, TDD)

- `printf_probe`: `Printf("x=%d y=%s t=%t\n", 42, "hi", true)` → `x=42 y=hi t=true`.
- `sprintf_probe`: `s := Sprintf("[%d]", 7); fmt.Println(s)` → `[7]`.
- `printf_pct_probe`: `Printf("100%%\n")` → `100%`.
- `printf_v_probe`: `%v` on int/string/bool.
- Non-literal-format rejection (manual; golden can't test compile errors): `Printf(f, 1)`
  where `f` is a variable → clean error.

Gate: `make verify` ALL GREEN (incl. ccomp), golden green (+ new probes), `make test` 76/1.

## Out of scope (follow-up)
Width/precision/flags; `%x`/`%o`/`%b`/`%q`/`%p`/`%c`; `Fprintf`/`Errorf`; non-literal format
strings; struct `%v` (only scalar/string args in v1 — a struct arg to `%v` cleanly errors).

## Risk
- The new runtime helpers must heap-allocate the goo_string consistently with existing
  goo_string conventions (length + data); mirror `goo_string_concat`'s allocation. Mitigated
  by the sprintf golden probe asserting exact output.
- `%v` type dispatch must match the Println per-type dispatch; reuse that logic.
