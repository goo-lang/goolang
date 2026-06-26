# M3 — Foundation Hardening (Design)

Date: 2026-06-26
Milestone: M3 of the self-hosting roadmap (`docs/SELF_HOSTING_ASSESSMENT.md`)
Status: IMPLEMENTED — shipped to `main` 2026-06-26 (PRs #20 int64 coercion,
#21 comma-ok maps, #22 match guards). See "As-built status" at the end.

## Goal

Finish what M2 started. M2 shipped enums+`match`, dynamic slices, and
`map[string]V`, but left three sharp edges that block real programs:

1. A **correctness defect** — integer literals are always `i32`, so any mix with
   an `int64` value fails LLVM module verification.
2. A **missing map ergonomic** — there is no way to distinguish "key absent"
   from "value is the zero value".
3. A **silently-dropped feature** — `match` guards (`case X{..} if cond:`) parse
   and typecheck but are never lowered, so the guard is ignored.

The milestone gate is:

> A program can declare and compare `int64` values, read a map with
> presence-checking (`v, ok := m[k]`), and gate a `match` arm with a guard —
> each exercised by a probe that builds, runs, and verifies.

This is a project whose north star is "enhance Go": Goo = Go plus power
features. M3 hardens the Go-compatible foundation those features stand on.

## Verified starting state (read from source, not assumed)

Pinned by parallel read-only reconnaissance on 2026-06-26.

### Feature 1 — int64 literal coercion
- Integer literals are unconditionally `i32`:
  `src/codegen/expression_codegen.c:196-197`
  (`LLVMConstInt(LLVMInt32TypeInContext(...), value, 1)`, type `TYPE_INT32`).
- Binary-expr codegen `codegen_generate_binary_expr`
  (`expression_codegen.c:389-692`) passes operands to `LLVMBuildAdd` etc. with
  **no width reconciliation**.
- Comparison codegen (`expression_codegen.c:577-640`) likewise feeds
  `LLVMBuildICmp` operands of potentially mismatched width → verify failure.
- Literal operands are distinguishable at codegen: `expr->type == AST_LITERAL`
  (`include/ast.h:45`).
- A coercion helper exists — `codegen_convert_value`
  (`src/codegen/type_mapping.c:391-441`) — but it widens with **`LLVMBuildZExt`
  unconditionally** (line 408, commented "assume zero extend for now"). That is
  a latent sign bug for signed `int64`. `LLVMBuildSExt` is already used for
  signed widening in `src/codegen/call_codegen.c:594`.
- Backend is the real LLVM C API (`src/codegen/codegen.c:30-31`).

### Feature 2 — comma-ok map reads
- Runtime getter `goo_map_get_sv(GooMapSV*, const char*) -> int64_t`
  (`include/runtime.h:104`, `src/runtime/runtime.c:383`) returns `0` on a
  missing key — **no presence bit**. No `_ok` variant exists.
- Multi-LHS `a, b := expr` **already exists end-to-end**:
  - Parser: `parser.y:560-577` builds a `VarDeclNode` with `name_count == 2`.
  - Typecheck: `src/types/type_checker.c:436-449` — when the RHS is a
    `TYPE_STRUCT` with ≥ `name_count` fields, each name binds to a field type.
  - Codegen: `src/codegen/function_codegen.c:255-284` — destructures the RHS
    struct via `LLVMBuildExtractValue`.
- Map index read lowers to a **bare value** today
  (`src/codegen/composite_codegen.c:38-60`), and typecheck returns the bare
  value type (`src/types/expression_checker.c:579-627`).
- `bool` is fully wired: `TYPE_BOOL` (`include/types.h`), `type_bool()`
  (`src/types/types.c:42`), mapped to `i1` (`type_mapping.c:104`).

### Feature 3 — match guard lowering
- AST carries the guard: `MatchCaseNode.guard` (`include/ast.h:729`),
  `GuardConditionNode.condition` (`ast.h:757`).
- Parser captures it: `parser.y:2058-2073` (the
  `CASE pattern guard_condition COLON ...` production) and `guard_condition: IF
  expression` (`parser.y:2119`).
- Typecheck checks it in arm scope, **after** payload bindings are registered:
  `src/types/expression_checker.c:970-974`.
- Codegen **ignores it**: `codegen_generate_match`
  (`src/codegen/composite_codegen.c:448-602`) dispatches via an LLVM `switch` on
  the enum tag (line 504); payload fields are bound at lines 541-580; the arm
  body is emitted at lines 586-588. There is **no** guard evaluation between
  binding and body.
- An injection point exists after payload binding (line 580) and before body
  emission (line 586), where bound fields are in scope.
- Existing probe `examples/match_probe.goo` has **no** guard coverage.

## Locked decisions

| # | Decision | Choice | Rationale |
|---|---|---|---|
| 1 | Scope | All three items, one spec, three sequenced PRs | User chose full scope; each item is independently shippable. |
| 2 | Order | int64 → comma-ok → match guards | int64 is a correctness defect and unblocks the most (int64 arithmetic, int64-valued maps); the other two are additive. |
| 3 | F1 coercion trigger | **Literals only** — coerce an `AST_LITERAL` operand to the other operand's integer type; never auto-coerce two typed variables | Go-faithful: only untyped constants adapt; two mismatched real variables still require explicit conversion, so we don't silently hide width bugs. |
| 4 | F1 widening | **Sign-aware** — `SExt` to widen signed `int` literals (not the buggy ZExt path); `Trunc` to narrow | Negative `int64` literals must sign-extend correctly; matches the maps sign-extend lesson. |
| 5 | F2 representation | Map index read in 2-LHS context yields `TYPE_STRUCT{V, bool}`, reusing the existing multi-LHS destructure path | New surface is one runtime fn + a context-gated tuple form; rides proven machinery. |
| 6 | F2 form | `v, ok := m[k]` (`:=`) only | Multi-LHS plain `=` assignment does not exist in the parser; deferred. |
| 7 | F3 guard-false target | Branch to the `default`/wildcard arm block if present, else the match merge block (no-op) | Matches Go's "no case matched → nothing happens"; correct for the common shapes. |
| 8 | F3 same-tag arms | Out of scope (documented limitation) | Dispatch is a tag-`switch` (one block per tag); LLVM rejects duplicate switch cases, so two same-tag guarded arms need a dispatch restructure deferred to a follow-up. |

Alternatives considered and rejected:
- **F1 fix at the literal codegen site** — rejected: `codegen_generate_literal`
  cannot see its sibling operand's type; would need context threading that does
  not exist.
- **F1 reuse `codegen_convert_value` unchanged** — rejected: its ZExt-always
  widening miscompiles negative `int64`. We use a sign-aware widen.
- **F1 coerce any width mismatch** — rejected: would silently coerce two real
  variables and hide bugs; not Go-faithful.
- **F2 sentinel-in-band** (reserve a value to mean "absent") — rejected: no
  value is safe to reserve for a general 8-byte slot.
- **F3 restructure match dispatch now** (tag-switch → per-tag if-else chain to
  support same-tag guards) — rejected for M3: larger change, higher regression
  risk; deferred as a follow-up once guards land.

## Component design

### Feature 1 — int64 / int literal coercion

**Where:** `codegen_generate_binary_expr` (`expression_codegen.c:389-692`),
covering both the arithmetic and comparison operator groups.

**Logic:** after both operands are generated and any lvalues auto-loaded,
reconcile integer widths *before* the operator builder call:

1. If both operands are integers and their LLVM widths differ, and exactly one
   operand's AST node is `AST_LITERAL`, coerce the literal operand to the other
   operand's integer type.
2. Coercion is sign-aware: widen with `SExt` (Goo int literals are signed),
   narrow with `Trunc`. (A narrowing literal — e.g. assigning into a smaller
   typed context — is rare here but handled for completeness.)
3. If neither or both are literals, or widths already match, emit unchanged
   (Go-faithful: mismatched typed variables are a type error surfaced earlier,
   not silently coerced here).

A small static helper (e.g. `coerce_int_literal_operand`) keeps the two operator
groups DRY. Reuse `LLVMBuildSExt`/`LLVMBuildTrunc` directly rather than the
ZExt-bound `codegen_convert_value`.

### Feature 2 — comma-ok map reads `v, ok := m[k]`

**Runtime** (`include/runtime.h`, `src/runtime/runtime.c`): add
```c
void goo_map_get_sv_ok(GooMapSV* m, const char* k, int64_t* out, int* found);
```
`*found` is `1` if the key is present (and `*out` is its value), else `0` (and
`*out` is `0`). The existing `goo_map_get_sv` is unchanged for single-value
reads.

**Typecheck** (`src/types/type_checker.c` var-decl path + the map branch of
`type_check_index_expr`): when a map-index expression is the RHS of a `:=` with
`name_count == 2`, the index read's result type is `TYPE_STRUCT{value_type,
bool}` instead of the bare `value_type`. Single-LHS reads are unchanged. The
var-decl path then binds name 0 → `V`, name 1 → `bool` via the existing
per-name-types logic.

**Codegen** (`src/codegen/composite_codegen.c` map-index branch, driven by the
multi-LHS var-decl path in `function_codegen.c`): in the 2-LHS context, call
`goo_map_get_sv_ok` with `alloca`'d `out`/`found` slots, load them, convert the
value slot to `V` (reusing `codegen_map_slot_to_value`) and the found flag to
`i1`, and build the `{V, bool}` aggregate. The existing `ExtractValue`
destructure then binds `v` and `ok`. Single-LHS `v := m[k]` keeps the current
bare-value path.

The context signal flows from the var-decl (`name_count == 2` + RHS is a map
index) into how the index read is lowered — single-value reads never see the
tuple form.

### Feature 3 — match guard lowering

**Where:** `codegen_generate_match` (`composite_codegen.c:448-602`), at the
injection point after payload binding (line 580) and before body emission
(line 586).

**Logic:** if `mc->guard` is non-null:
1. Generate the guard condition: `codegen_generate_expression(codegen, checker,
   ((GuardConditionNode*)mc->guard)->condition)`. Bound payload fields are in
   scope, so guards may reference them.
2. Emit `LLVMBuildCondBr`: true → a fresh "arm body" block (where the body is
   then emitted); false → the fallback block — the `default`/wildcard arm's
   block if the match has one, else the match merge block.
3. Wildcard/`default` arms with a guard are handled the same way (no payload
   binding, guard references outer scope only).

This requires resolving the fallback block before per-arm lowering (locate the
wildcard arm's block, or fall back to merge). The body-emission structure is
otherwise unchanged.

## Testing

Per-feature cadence (the one that worked in M2): probe RED → implement → GREEN,
building (`make goo` + `make lib/libgoo_runtime.a`) and running the probe at each
step; then an adversarial review-workflow (finders → independent skeptic
verifiers); fix verified findings; document limitations; one PR per feature;
confirm CI green by reading the run rollup (not a piped exit code).

New gate per feature — each adds:
- `examples/<feat>_probe.goo` + `examples/<feat>_probe.expected.txt`
- a Makefile target, added to the `verify` aggregate
- an entry in `.github/workflows/tests.yml`

Probe sketches:
- **F1:** `var d int64 = -1; if d == -1 { print "PASS" }` plus an int64
  arithmetic line; asserts the module verifies and runs.
- **F2:** set a key, read present (`v, ok` with `ok == true`) and absent
  (`ok == false`, `v == 0`); assert both.
- **F3:** an enum with a payload, two arms differing only by guard
  (`case Some{n} if n > 0` vs `default`), exercised with values on both sides of
  the guard.

After header edits (`runtime.h`/`types.h`/etc.): `make clean && make goo`
(incremental builds miss header deps). `make verify` failing only at
`ccomp-build` is the known CompCert-env gap and is ignored.

## Sequencing

Three PRs, in order: (1) int64 literal coercion, (2) comma-ok map reads,
(3) match guard lowering. Each is atomic, independently shippable, and gated by
its own probe + CI entry.

## Known limitations (to record on completion)

- **F1:** only untyped literals coerce; two mismatched typed integer variables
  still require explicit conversion (by design, Go-faithful).
- **F2:** comma-ok is supported for `:=` only; `v, ok = m[k]` (plain multi-assign)
  is deferred until multi-LHS `=` exists. Only `map[string]V` with an 8-byte
  slot is in scope (the M2 map shape).
- **F3:** two arms with the same enum tag but different guards are not supported
  (tag-`switch` dispatch; LLVM rejects duplicate switch cases). True same-tag
  sequential guard fallthrough needs a dispatch restructure, deferred as a
  follow-up.

## As-built status (M3 shipped 2026-06-26)

All three features merged to `main` @ `f71393f`; 16 probes green end-to-end.
Gates `int64-probe`, `commaok-probe`, `guard-probe` added to `make verify` + CI.

Deltas and discoveries vs. the design (all reviewed and verified):

- **F1 — scope grew (correctly):** the literal-coercion at the binary/comparison
  site was necessary but not sufficient. `var d int64 = -1` also stored an `i32`
  into an `i64` alloca (read back as `4294967295`), so a **sign-extending widen in
  the var-decl store path** (`function_codegen.c`) was added. That var-decl widen
  is **intentionally not literal-gated**: Goo's type checker is lenient — it
  permits cross-width typed assignment `var y int64 = x` (x int32) — so `SExt` is
  the correct lowering for both literal and variable initialisers; gating to
  literals-only would *miscompile* a negative variable init. Decision #3's
  "literals only" therefore governs the **binary/comparison path** (the bug-hiding
  case); the var-decl widen is deliberately broader. A regression probe locks this
  in. Optional strict-mode follow-up: have the type checker reject cross-width
  typed assignments for full Go-faithfulness (not done — superset behaviour, fits
  "C++ to Go's C").
- **F2 — as designed.** `goo_map_get_sv_ok` + typecheck `{V,bool}` synthesis +
  codegen aggregate, reusing the existing multi-LHS destructure. Probe also proves
  the headline claim (a key mapped to `0` returns `ok=true` — present-zero ≠ absent).
  Two struct-synthesis leaks were caught in review and fixed (use the cached
  `TYPE_BOOL` builtin; free on the fields-`malloc` failure path).
- **F3 — as designed, with a review-caught miscompile fixed.** A guarded
  **wildcard** catch-all (`case _ if cond:`) self-looped on a false guard
  (`arm == fallback` → infinite loop). Fixed: a guarded wildcard arm's false-guard
  falls to the **merge** block, not the default block. Also hardened: in-context
  block API, guard `ValueInfo` freed on all paths, and a no-value guard now errors
  instead of emitting an ungated body. The same-tag-guard limitation above stands.

Full per-task record, findings, and review verdicts: `.superpowers/sdd/progress.md`.
