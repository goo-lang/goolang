# Comptime-value-specialized functions (design)

Sub-project 1 of the SPMD/parallel-lanes keystone (Leg 3 of the hybrid-memory direction).
The one unbuilt leg of that direction is comptime-**value** metaprogramming: today comptime
can *evaluate* a value (`comptime const N = fib(10)` → 55 at compile time, verified) but a
comptime value cannot yet *drive codegen structure*. This sub-project delivers the first such
capability — a function specialized by a compile-time value — which is the SPMD kernel-
specialization enabler and the substrate the later legs build on.

Prerequisite already verified (Spike 0, 2026-07-07): the M8 scheduler gives true multi-core
parallelism (8 CPU-bound goroutines → ~7.8× speedup, 765% CPU on 32 cores), so goroutine-per-
lane will actually parallelize. That result de-risks the direction but is independent of this
sub-project.

## Surface & semantics

A function parameter may be marked `comptime`:

```goo
func fill(comptime n int, seed int) int {
    var buf [n]int          // legal ONLY because n is a compile-time constant
    i := 0
    for i < n {             // bound is a compile-time constant in each specialization
        buf[i] = seed + i
        i = i + 1
    }
    sum := 0
    j := 0
    for j < n {
        sum = sum + buf[j]
        j = j + 1
    }
    return sum
}

func main() {
    a := fill(4, 10)        // specialization with n == 4
    b := fill(8, 10)        // specialization with n == 8
}
```

- **The argument bound to a comptime parameter must be a comptime-constant** — a literal, a
  `const`/`comptime const`, or any expression the comptime engine can evaluate. `fill(x, s)`
  for a runtime `x` is a compile error with a clear diagnostic.
- **Inside the body, `n` is a compile-time constant.** It can do what a runtime value cannot:
  size a fixed array (`[n]int`), fix a loop bound, feed compile-time arithmetic. In each
  specialization the compiler sees `n` as a literal, so LLVM can unroll/vectorize/elide bounds
  checks that a runtime bound would force it to keep.
- **The function is monomorphized per distinct comptime value.** `fill(4, …)` and `fill(8, …)`
  compile to two specialized instances; two `fill(8, …)` calls share one instance.

## Grammar / AST

- Add a `comptime` qualifier to the function-parameter grammar rule: `COMPTIME identifier
  type` (alongside the existing `identifier type`). `COMPTIME` is already a token.
- Mark the parameter's `VarDeclNode` as comptime. **Verify** whether `VarDeclNode` already
  carries an `is_comptime` flag; if not, add one at the STRUCT TAIL (the no-header-deps
  convention — a mid-struct insert silently miscompiles any TU rebuilt without `make clean`).
- **This is the only grammar change, and it is stop-the-line.** Use the goo-grammar skill:
  run `scripts/grammar-tripwire.sh` before and after; it must stay at the 82 S/R + 256 R/R
  baseline, or any delta is justified and re-baselined per the conflict ledger. `COMPTIME`
  already prefixes `comptime {…}` blocks, `comptime const`, and `comptime func`; a new use as
  a parameter prefix must be checked for new LALR conflicts.
- Call sites are unchanged — `fill(8, 10)` is an ordinary call. This is the reason the design
  pivoted away from `fill[8](10)`: explicit `[…]` call-site instantiation collides with array
  indexing and is documented "grammar-hard" (deferred even for type args, generics Tier C).

## Type checking

- A function is a **comptime-specialized function** iff any parameter is marked comptime.
- At a call to such a function, for each comptime parameter: evaluate the corresponding
  argument through the existing comptime engine (`comptime_eval_expression(raw_ctx, argExpr)`
  — the same entry `comptime const` uses, `type_checker.c:~1467`). If it yields no value / an
  error, reject: *"argument to comptime parameter 'n' must be a compile-time constant"*.
- Bind `n` in the body's scope as a comptime constant, reusing the machinery `comptime const`
  already populates on a `Variable` (`comptime_value`, `has_const_int_value`/`const_int_value`)
  so that `[n]int` array lengths and `n`-bounded loops resolve to the concrete value (the
  const-array-length path — `goo_fold_const_int_ctx` — already resolves a const `N` in
  `[N]int`; a comptime parameter must feed the same path).
- Record the evaluated comptime value(s) on the call node for the monomorphizer (see below).

## Monomorphization

- Reuse the existing monomorphizer (`src/codegen/monomorphize.c`, `mono_instantiate`,
  children-before-parents + dedup). Today an instance is keyed on the call's **type args**
  (`CallExprNode.type_args`). Extend the instance key to also include the **comptime value
  args**.
- Carry the evaluated comptime values on the call node — mirror `type_args`/`type_arg_count`
  with a parallel `comptime_value_args`/`comptime_value_arg_count` (or an equivalent
  value-tuple), set by the type checker, read by the monomorphizer. Malloc'd `CallExprNode`
  sites must zero the new fields (same discipline the `type_args` doc comment notes).
- Each specialized instance substitutes its comptime parameter(s) as literal constants in the
  body, so the instance's `n` is a compile-time value everywhere it is used. Instances with
  identical value tuples dedup to one; distinct tuples get distinct instances (stamp the
  values into the instance name for debuggability, e.g. `fill$n=8`).

## Success criterion & demo

The forcing function is `var buf [n]int` inside the kernel: **a comptime `n` uniquely enables
it** — a runtime `n` is rejected because array lengths must be constant. So the demo cannot be
faked by const-folding a runtime value.

Golden probe `examples/comptime_value_specialize_probe.goo` (the `fill` example above, or
equivalent): `fill(4, 10)` and `fill(8, 10)` both compile and produce the correct sums from
two distinct specializations. Assertions:
1. **Correctness** — stdout matches the expected per-`n` results (golden).
2. **Specialization is real** — emitted IR (`bin/goo --emit-llvm`) shows two instances with
   the array/loop bound as the literal `4` and `8` respectively (not a shared runtime-`n`
   body). A grep for the two distinct sized allocas / the stamped instance names.
3. **Rejection** — a sibling reject-probe: passing a runtime value to a comptime parameter
   (`fill(someRuntimeInt, 10)`) is a clean compile error, not invalid IR.

Wire the golden into `test-golden` and the reject case into a `comptime-value-reject-probe`
in `verify`.

## Scope (YAGNI, first cut)

- **Comptime `int` parameters only.** Other comptime types (bool, other ints, comptime types-
  as-values) are follow-ups.
- **Explicit values only** — the comptime argument is supplied at the call; no inference.
- **Comptime-value params OR `[T]` type generics, not both simultaneously.** Composing the two
  monomorphization axes (per-type × per-value — the full SPMD "per-type kernel × per-tile"
  goal) is a documented follow-up once the single-axis value specialization is proven.
- No comptime control-flow that *emits* code (`comptime if`/`comptime for` selecting/emitting
  bodies) — that is the separate Zig-style-metaprogramming option we did not pick for the
  first deliverable.

## Testing

- `comptime-value-specialize-probe` (golden): correctness of two specializations + IR check
  that `n` is a literal per instance.
- `comptime-value-reject-probe`: runtime value to a comptime parameter → clean diagnostic, no
  invalid IR (mirrors the existing reject-probe pattern, e.g. `nonconst-arraylen-reject-probe`).
- Grammar tripwire at its baseline (or justified re-baseline) before/after.
- Full `make verify` + `make test` green.

## Open verification points for the implementer

1. Confirm `VarDeclNode`'s flag situation (has `is_comptime`? has `is_captured`/
   `is_variadic_param` — add the new flag at the tail if needed).
2. Confirm `comptime_eval_expression` evaluates an arbitrary call-argument expression (not
   only a const-decl RHS) and returns an int `ComptimeValue`; confirm the value-extraction API.
3. Confirm the monomorphizer's instance-key/dedup structure and the cleanest place to fold in
   the value tuple (name-stamp vs a structured key).
4. Confirm `[n]int` resolves its length from a comptime-parameter-bound `Variable` the same
   way it resolves from a `const` (the `goo_fold_const_int_ctx` path).
