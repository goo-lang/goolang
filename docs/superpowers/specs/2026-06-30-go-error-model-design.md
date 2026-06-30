# Go error model: `(T, error) ≡ !T`

**Date:** 2026-06-30
**Branch:** `feat/v1-strconv` (this milestone absorbs the strconv work; the branch name is legacy)
**Phase:** v1 Phase 5 (Go-source compatibility + stdlib)
**Trigger / north star:** real Go code that does `n, err := strconv.Atoi(s); if err != nil { return 0, err }; return n, nil`
compiles and runs — Goolang's `!T` differentiator and Go's `(T, error)` become the **same type**.

## Principle

`!T` is the single physical representation: `{ i1 is_error, union{ T value; error err } }`.
`(T, error)` is the Go-facing spelling of that exact type — not a separate tuple. Both
consuming forms (`x := f() catch e {…}` and `n, err := f()`) and both producing forms
(`func f() !T { return … }` and `func f() (T, error) { return v, nil }`) read/write the one
representation. `strconv.Atoi → !int` is the canonical value the whole thing drives.

Already shipped on this branch (prereqs): `strconv.Itoa`, the `goo_int/float/bool_to_string`
+ `goo_string_to_int` runtime helpers, snprintf truncation guards, and `.go` file acceptance.

## Verified groundwork (do not re-derive)
- Go-style multi-return + destructure ALREADY work: `func two() (int,int){return 1,2}` with
  `a, b := two()` compiles and prints `1 2`. So 2-value return signatures parse and 2-target
  multi-assign from a single call destructures. The error-model work reuses this plumbing.
- The `!T` error arm type defaults to `TYPE_STRING` (error_union_codegen.c:289) — an `error`
  value is a message; `error(msg)` builtin + `errors.New(msg)` produce it.
- Package identifiers must be registered in `stdlib_packages[]` (type_checker.c) AND
  `stdlib_package_lookup` (expression_checker.c) — `errors` and `strconv` both need this.

## Components (tasks)

### Task A — `error` builtin type + `nil` error + `errors.New`
- Introduce `error` as a builtin type = the error-arm type (string-backed in v1). Register the
  identifier `error` as a type name so `(T, error)` and `var e error` parse/typecheck.
- A nullable `?error` is the type of a destructured `err`; `nil` is the absent error; reuse the
  existing nullable nil-compare so `if err != nil` works.
- Register `errors` package + `errors.New(string) error` (in `stdlib_packages[]` +
  `stdlib_package_lookup`); codegen produces an `error` value from the message string (reuse the
  `error(msg)` builtin's payload construction).
- Acceptance: `var e error = errors.New("x"); if e != nil { fmt.Println(e) }` compiles, prints `x`.

### Task B — Consuming: destructure `n, err := <!T>`
- Typecheck `type_check_multi_assign` / the `:=`-with-2-targets path: when there is ONE RHS of
  type `!T` and TWO targets, bind `target[0] : T` (value arm) and `target[1] : ?error`.
  (Distinct from the existing 2-RHS multi-assign and the genuine `(int,int)` tuple destructure.)
- Codegen: evaluate the union once; `target0 = is_error ? zero(T) : value_arm`;
  `target1 = is_error ? nullable(error_arm) : nil`.
- Acceptance: `n, err := strconv.Atoi("42")` → `n==42`, `err==nil`; `strconv.Atoi("x")` →
  `err != nil`, `n==0`. Coexists with `catch`.

### Task C — Producing: `func f() (T, error)` signature ≡ `!T`
- Parser/typecheck: a 2-value return signature whose SECOND element is exactly `error` resolves
  the function's return type to `type_error_union(T, error_t)` — i.e. `!T`. `(int, int)` and
  other non-error 2-tuples are unaffected (stay genuine multi-return).
- Acceptance: `func f() (int, error)` typechecks identically to `func f() !int` (a probe that
  returns such a function's result into an `!int`-consuming context).

### Task D — Producing: `return v, e` builds the union arms
- In a `(T, error)`/`!T` function, `return v, nil` → success arm (value = v); `return zero, e`
  with non-nil `e` → error arm. General `return v, e` where `e : ?error` lowers to a runtime
  branch: `is_error = (e != nil)`; build the matching arm. Literal `nil` folds to success at
  compile time.
- Acceptance: `func ok() (int,error){return 7,nil}` and
  `func bad() (int,error){return 0, errors.New("no")}` each consumed both via `n,err:=` and
  `catch` give correct results.

### Task E — Capstone: the strconv round-trip (a `.go` file)
- `examples/go_error_model_probe.go` (a real `.go` file, exercising Task-`.go`):
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
- Also add `strconv.Atoi → !int` itself (the original strconv Task 3) as part of this — it's the
  driver. Build the `!int` via `goo_string_to_int` + `codegen_create_error_union_success/_error`
  (see the strconv plan's Atoi task for the exact codegen).
- Golden expected: `42` then `caught`.

## Testing
Per-task golden probes (errors.New + nil; destructure ok/err; `(T,error)` signature; return arms),
plus the `.go` capstone. Gate: `make verify` ALL GREEN (incl. ccomp), golden green, `make test` 76/1.

## Out of scope (follow-up)
`error` as a full interface (wrapping/`errors.Is`/`As`/`%w`); multiple non-error multi-returns
mapping to anything; `error` values richer than a message string; `panic`/`recover` interplay.

## Risk
- The `(T,error)`-as-`!T` mapping must NOT change genuine `(int,int)` multi-return — gate strictly
  on the second element being the `error` type. Mitigated by a `(int,int)` regression probe.
- `return v, e` runtime arm-selection on `e`'s nil-ness must match what `catch`/destructure expect.
  Mitigated by the capstone exercising both a literal-nil and an errors.New path through both
  consuming forms.
- `error` arm type is string-backed in v1; `errors.New`/`error()` must agree on the payload shape
  so a produced error round-trips to a printed message. Mitigated by Task A's print acceptance.
