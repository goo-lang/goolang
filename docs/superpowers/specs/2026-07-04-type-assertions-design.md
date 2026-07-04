# Type Assertions & Type Switches — Design

Date: 2026-07-04 · Branch: `feat/type-assertions` (base: main @ fc3211b, post-#113)
Status: user-approved design (concrete-target-only scope; all three forms)

## Goal

`x.(T)` (comma-ok and single-return) and `switch v := x.(type)` for CONCRETE
target types, building on the now-sound interface representation (#113). Foundation
for `errors.As`-style concrete extraction and fmt/formatting type-switch dispatch.

## Scope (user-selected)

- Target `T` is a CONCRETE type only (struct, `*T`, named type). Assert-to-interface
  (`x.(J)`, J an interface) is DEFERRED with a clean v1 error — it needs RTTI.
- All three forms this branch: comma-ok `v, ok := x.(T)`, single-return `v := x.(T)`
  (panics on miss), type switch `switch [v :=] x.(type) { case … }`.

Non-goals (recorded): assert-to-interface targets; naming the dynamic type in the
single-return panic message (both need the RTTI/type-descriptor cycle); reflect.

## Mechanism — vtable-pointer identity (no new RTTI)

The interface value is `{ vtable_ptr, data_ptr }` (#113). The vtable global is
uniquely named per `(concrete, interface)` pair — `goo.vtable.<T>.<I>`, a private
constant. Therefore, for `x` of static interface type `I`, the dynamic type is `T`
iff `x.vtable == &goo.vtable.T.I`.

At an assertion site both `I` (x's static type) and `T` are known statically, so
codegen calls the existing `codegen_interface_vtable(I, T)` (interface_codegen.c:178)
— it returns the canonical named global, creating it on first reference or finding
the existing one, so pointer identity holds either way — then:
- extract x's vtable word (`ExtractValue 0`),
- `icmp eq` the two pointers → the match bit.

Extracting the value on match: `ExtractValue 1` gives the data pointer, which points
at a `T` in BOTH boxing shapes (value-boxed: heap copy; pointer-boxed: the object).
Load it as `T` — the exact inverse of #113's boxing.

CONSTRAINT (recorded): pointer identity holds within a single module. Goo compiles
whole-program to one module today (no separate compilation of Goo objects), so this
is sound now; it would need a stable type-id if Goo ever gains separate compilation.

### Form lowering

- **comma-ok** `v, ok := x.(T)`: `ok` = the icmp bit; `v` = branch+phi between the
  loaded `T` (match) and `T`'s `zeroinitializer` (miss). Not a panic.
- **single-return** `v := x.(T)`: match path loads `T`; miss path calls `goo_panic`
  with `"interface conversion: I is not T"` (see deviation below).
- **type switch** `switch [v :=] x.(type) { case T1: … case Tn: … default: }`:
  desugars to an ordered chain of the icmp checks; the matching arm binds `v` to
  that case's concrete type. `case nil:` matches a nil interface (icmp the whole
  `{null,null}` value). Multi-type `case T1, T2:` and `default:` keep `v` at the
  interface type `I` (Go's rule).

The comma-ok/single-return split is NOT grammatical — both parse to one
`AST_TYPE_ASSERT` node; assignment context (2 LHS names vs 1) selects the form at
typecheck/codegen, exactly as the existing comma-ok map read does.

## Grammar (risk center — goo-grammar skill governs; tripwire 81 S/R + 256 R/R)

Two net-new constructs; no lexer changes (`TYPE`, `DOT`, `LPAREN`, `LBRACE_BODY`
all exist).

1. **Assertion postfix** — one arm beside `selector_expr` (parser.y:1834):
   ```
   | primary_expr DOT LPAREN type RPAREN   → AST_TYPE_ASSERT { expr, asserted_type }
   ```
   After `primary_expr DOT`, lookahead `identifier` (selector) vs `LPAREN`
   (assertion) is one-token-clean — same shape as #111's `[]byte(x)` conversion arm.

2. **Type switch** — a distinct `switch_stmt` arm (+ LBRACE fallback mirroring the
   existing switch arms):
   ```
   | SWITCH type_switch_guard LBRACE_BODY type_case_list RBRACE
   type_switch_guard : [ identifier SHORT_ASSIGN ] primary_expr DOT LPAREN TYPE RPAREN
   type_case_list    : type_case_clause+
   type_case_clause  : CASE type_list COLON statement_list | DEFAULT COLON statement_list
   ```
   Conflict resolution: the guard's `DOT LPAREN TYPE RPAREN` tail commits the parse
   to the type-switch arm before any `CASE` is seen, so the type-case-list and the
   expression switch's `case_clause_list` never actually collide. `TYPE` inside `.( )`
   is only legal here, keeping it out of the plain-assertion arm (whose interior is
   a `type`, and `type` productions don't start with the `TYPE` keyword).

   RISK: the type-switch arm shares the `SWITCH … LBRACE_BODY … RBRACE` prefix with
   three existing switch arms. `bison -Wcounterexamples` is front-loaded on this task;
   a genuine delta triggers the justified-delta procedure (conflict-ledger), never
   silent absorption.

## Typecheck & semantics

Static rejections (all clean errors, no codegen reached):
- Operand not an interface: `"invalid type assertion: <expr> is not an interface type"`.
- Impossible assertion — `T` doesn't implement `I` (reuse `type_interface_satisfied`
  from #113): `"impossible type assertion: T does not implement I (missing method M)"`.
  Guarantees `goo.vtable.T.I` is always legitimately constructible.
- Assert-to-interface target (the deferred case): `"type assertion to an interface
  type is not supported in v1 (concrete target types only)"`.
- Type-switch: each case type must implement `I`; duplicate case types rejected;
  at most one `default`.

Result types / binding:
- comma-ok: `v : T`, `ok : bool`.
- type-switch bound `v`: `T` in a single-type case; interface type `I` in a
  multi-type case or `default`.
- comma-ok miss zero value: `T`'s `zeroinitializer` (nil for `*T`, zero struct for a
  struct, 0 for a named numeric) — the map zero-guard helper.

Nil interface (`{null,null}`): matches no concrete `T` → comma-ok `false`,
single-return panics, `case nil:` matches.

### Deviation (recorded, not fixed)

Single-return panic message. Go: `interface conversion: main.I is main.Cat, not
main.Dog` (names the actual dynamic type). Goo has no RTTI, so no dynamic-type name
at runtime → `"interface conversion: I is not T"` (static names only). Functionally
identical panic; less informative message. Inherent to the concrete-only/no-RTTI
scope; naming the dynamic type is exactly what the deferred RTTI cycle adds.

## Testing (all goldens differential vs `go run`)

Assertion-expression goldens:
- comma-ok hit + miss on the same interface value (`v, ok := x.(Cat)`, x holds Cat
  then Dog) — ok true/false, v the value / the zero value.
- single-return hit (concrete returned, methods callable); Makefile ABORT probe for
  the single-return miss → `goo_panic` (non-zero exit + message grep, like
  `map-nilfunc-abort-probe`).
- pointer-concrete target `x.(*Box)` composing with #113 — assert out to the pointer,
  mutate through it, observe aliasing.
- zero-value-on-miss for a struct target and a `*T` target (nil).

Type-switch goldens:
- multi-case with bound `v` to distinct concrete arms; `default`; multi-type
  `case T1, T2:` (v stays interface); `case nil:` on a nil interface.
- fmt-shaped `case int:`/`case string:`/`case bool:` over a heterogeneous `[]I` —
  the motivating exemplar.

Reject probes (Makefile, compile-must-fail + message grep): non-interface operand;
impossible assertion; assert-to-interface target; duplicate case type; two defaults.

Deviation: the single-return panic-message form is asserted as the Goo text (not
Go's), documented — not silently skipped.

Gates: golden count grows from 224/0; `make test` 76/1; `make verify` +
`ccomp-link` green; bison tripwire 81/256 (hard gate; `-Wcounterexamples`
front-loaded on the type-switch grammar task).

## Tasks (risk-ascending)

- T1: assertion grammar (`AST_TYPE_ASSERT` node, the `selector_expr` sibling arm) +
  tripwire. Parse-only deliverable (a probe that parses `x.(T)` to the right AST).
- T2: assertion typecheck + codegen — comma-ok + single-return, static rejects,
  vtable-ptr compare + extract + zero-guard/panic; assertion goldens + abort probe.
- T3: type-switch grammar + desugar/codegen (bound var, multi-type, default, nil);
  type-switch goldens.
- T4: reject-probe sweep + full sweep (verify, ccomp, tripwire) + handoff.

SDD economy mode; fresh-context whole-branch review before merge.
