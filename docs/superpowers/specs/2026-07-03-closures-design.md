# Closures — Design

**Date:** 2026-07-03. **Status:** design approved interactively by user (semantics: "Full Go, always-heap"; surface: "Full expression position"; overall design + two-branch staging: approved).
**Prior state (main @97c561f, probed):** `add := func(x int) int {...}` = parse error at `func`; `func(int) int` as a PARAM TYPE parses but fails the checker (`Invalid type node` — `type_from_ast` has no TYPE_FUNCTION-from-syntax arm); named funcs pass as `go` targets; function-valued struct fields call correctly (#104-verified dispatch distinction).

## Decisions (user-locked)

1. **Semantics: full Go, always-heap capture.** Capture is by VARIABLE — mutation shared between closure(s) and enclosing scope, closures may be returned/stored/outlive their frame. Correctness mechanism: heap promotion of captured variables (below). Escape analysis is NOT required for correctness and is deferred as a recorded optimization (`escape_analysis.c` is Task-19-era infrastructure, treated as untrusted/unwired).
2. **Surface: full expression position.** Literals assignable, passable (⇒ func types as param/field/return types must typecheck), returnable, storable in composites, immediately invocable (`func(){...}()`), and usable in the `go func(){...}()` idiom. `defer func(){...}()` included iff defer's existing statement shape accepts a call expression — verify, include if free, record if not.

## Representation — universal fat pointer

Every function VALUE is `{ fn_ptr, env_ptr }` — exactly 16 bytes (two SysV INTEGER eightbytes; passes by value; the >16-byte by-pointer ABI rule does NOT trigger — do not route these through the by-pointer path).

- **Closure functions** are emitted as ordinary LLVM functions with a hidden FIRST parameter: `env_ptr` (an i8*/opaque pointer; the function bitcasts/GEPs it to its own env struct type).
- **Indirect calls** (through any function-typed value: variable, param, field, element, return) extract fn_ptr and env_ptr and call `fn_ptr(env_ptr, args...)`.
- **Named functions used as values** get a synthesized thunk, once per (function, module): `T_thunk(env, args...) { return F(args...); }`, and the value is `{ T_thunk, NULL }`. This mirrors PR #30's goroutine thunk mechanism. Rejected alternative: passing env to env-less functions and relying on SysV to ignore extras (fragile, calling-convention-dependent).
- **Direct calls to named functions are untouched** — still bare `call @F(args...)`; zero regression surface for the existing call path.
- **Migration:** `codegen_get_function_type`/TYPE_FUNCTION's LLVM mapping changes from bare fn pointer to the fat struct wherever a function type is a VALUE type (fields, params, locals, returns). Existing function-valued-field goldens guard this.

## Capture — free-variable analysis + heap promotion

- **Analysis (checker):** for each func literal, free variables = identifiers inside the literal body that resolve to a scope OUTSIDE the literal but INSIDE some enclosing function (locals/params of enclosing functions; globals and package symbols are NOT captures — they resolve as today). The scope chain gains a function-boundary marker; resolution crossing a boundary records a capture on the literal's AST node and marks the resolved variable `is_captured`.
- **Promotion (codegen):** any variable with `is_captured` allocates its storage via `goo_alloc` (heap) at declaration instead of `alloca`; ALL accesses (enclosing function and every capturing closure) go through the heap slot. Parameters that are captured copy into a promoted slot at function entry.
- **Env:** per literal, an env struct `{ T1* slot1, T2* slot2, ... }` of POINTERS to the promoted slots, `goo_alloc`d at closure creation; the closure value is `{ literal_fn, env }`. Two closures capturing the same variable each hold a pointer to the SAME slot ⇒ shared mutation (Go semantics). Nested literals: an inner literal capturing an outer-function variable receives the same slot pointer (its enclosing literal's env carries it; capture analysis is per-literal against the full chain).
- **Recursion:** `var f func(int) int; f = func(n int) int { ... f(n-1) ... }` works by capture-of-f (f is captured ⇒ promoted ⇒ the closure reads the slot at call time, seeing the assignment).

## Grammar

`FUNC LPAREN func_params RPAREN func_result? block` as a primary expression. Real bison risk (FUNC currently only begins declarations): the 79 S/R + 256 R/R discipline applies — exact delta recorded, any delta needs a written conflict-family justification + full-suite differential verification (the #92 BANG precedent). Statement-position immediate invocation (`func(){...}()` as an expression statement) must work for the `go`/`defer` idioms. The pre-existing newline-blind `var f func()` absorption wart (ASI-absence family) gets probed/recorded, not fixed.

## Types/checker beyond capture

- `type_from_ast` gains the function-type arm (grammar already parses `func(int) int` in type position) — builds TYPE_FUNCTION with param/return types (+ is_variadic if `...T` appears; variadic func TYPES follow #105's model).
- Func literal checking: signature from the literal's params/result; body checked in a fresh function scope chained (with boundary marker) to the enclosing scope; the literal's expression type is the TYPE_FUNCTION.
- Assignability: named func / literal / func-typed value are all assignment-compatible when signatures match (type_compatible gains the arm if missing).
- **Nil func values:** declaring `var f func(int) int` zero-values to `{NULL, NULL}`; CALLING a nil func value is a runtime abort with a clear message (Go: panic) — mirrors the `divzero-probe`/`bounds-probe` runtime-abort pattern, gated by an abort probe.

## Staging — two branches

- **Branch A `feat/func-values`** (no literals): type_from_ast func-type arm; fat-pointer representation migration; named-funcs-as-values via thunks; indirect calls through variables/params/fields/returns/elements; nil-call abort. Deliverable: `apply(inc, 41)` callbacks, function-valued fields/slices, `var f func(int) int; f = inc; f(1)`. Standalone value; de-risks the ABI migration with zero grammar changes.
- **Branch B `feat/closures`** (literals + capture): literal grammar; capture analysis + promotion; env construction; immediate invocation; `go func(){...}()`; recursion-via-capture; defer-literal if free. Consumes Branch A's representation unchanged.

## Testing

Per branch: golden probes `go run`-verified (Branch A: callback passing, func-typed fields/params/returns, reassignment; Branch B: counter factory with returned-closure mutation, TWO closures sharing one variable, immediate invocation, `go func` with join-by-channel, nested literals, recursion-via-capture); abort probe for nil-func call (Makefile pattern like `divzero-probe`); reject probes where Go rejects (signature-mismatched assignment). Full gates each task (verify/test/ccomp; bison counts each grammar task).

## Out of scope (recorded)

Stack allocation of non-escaping envs (needs real escape analysis); method values/expressions (`x.Method` as a value); bound-method closures; `escape_analysis.c` integration; generics interplay; goroutine-capture interplay beyond what `go func(){}()` naturally exercises (the #30 arg-boxing path stays as-is for named-func go statements).

## Execution

SDD economy mode per repo pattern: Sonnet implementers, Fable controller main-loop reviews with direct probes. Spec + plan committed on-branch; PR bodies flag decisions.
