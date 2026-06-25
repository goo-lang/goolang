# M2 — Data Modeling for an AST (Design)

Date: 2026-06-25
Milestone: M2 of the self-hosting roadmap (`docs/SELF_HOSTING_ASSESSMENT.md`)
Status: approved design; precedes the implementation plan.

## Goal

Give Goo the data-modeling primitives a compiler needs to represent and walk an
AST: payload-carrying tagged unions, tag dispatch over them (`match`), dynamic
slices, and string-keyed maps with a general value type. The milestone gate is:

> Represent and walk a small tagged-union tree (a mini-AST) with a `match` over
> the tag — including recursion through pointer-typed payloads.

## Verified starting state (read from source, not assumed)

- `switch` is **already** fully lowered — grammar (`parser.y:1022`), typecheck
  (`src/types/type_checker.c`), codegen (`src/codegen/statement_codegen.c`),
  shipped with a probe in PR #6. It does value-equality dispatch. It is **not**
  part of M2. `match` is the tag-dispatch construct M2 delivers.
- `match` **parses** today: `MatchExprNode`/`MatchCaseNode` (`ast.h:706`),
  reachable in expression position (`parser.y:1064`), with pattern forms
  `PATTERN_LITERAL`, `PATTERN_IDENTIFIER` (incl. typed `x: T`),
  `PATTERN_WILDCARD` (`_`/`default`), and `PATTERN_DESTRUCTURE`
  (`Variant{field: bind}`). It has **zero** typecheck/codegen — it dead-ends.
- Slice runtime **already exists**: `goo_slice { data, length, capacity }`
  (`include/runtime.h:34`) with a working `goo_slice_append` (`runtime.c:412`).
  Dynamic slices are therefore compiler-side wiring, not new runtime.
- Maps today are the MVP `goo_map_*_si` (string→int) only.
- The type system is a tagged union `Type` with a `kind` discriminant
  (`include/types.h:14`); `struct_type` (`types.h:129`) is the pattern new
  aggregate kinds follow. There is no `TYPE_ENUM` yet.

## Locked decisions

| # | Decision | Choice | Rationale |
|---|---|---|---|
| 1 | Scope | Full M2 in one spec, sequenced PRs | User chose comprehensive coverage. |
| 2 | Enum model | **Struct-payload variants**; repr `{ i32 tag; <payload union> }` | Reuses the destructure pattern `match` already parses; rides existing `struct_type` machinery; only the enum *declaration* grammar + codegen are new. |
| 3 | `match` value | Statement-style (void) | The gate is a tree-walker (visitor); avoids result-slot/phi codegen and arm-type unification this milestone. |
| 4 | Exhaustiveness | Required unless `default`/wildcard present | Catches missing-variant bugs when walking AST nodes — high value for a compiler. |
| 5 | Map value | `goo_map_sv` — string key → 8-byte value slot | Covers `map[string]*Symbol` (symbol tables) and `map[string]int` (interning), the dominant compiler needs, for the least runtime work. |

Alternatives considered and rejected: Rust-style positional enum variants
(needs *new* decl grammar **and** a new positional match-pattern — more parser
surface/risk); plain C-style enums + manual tagged struct (payloads
unsafe/manual; weak for the AST gate); value-producing `match` (deferred —
not needed by the gate); arbitrary-size map values (more runtime work than the
symbol-table use case requires).

## Component design

### 1. Tagged unions / enums

**Surface syntax** — mirrors how Goo declares every named type (`type Point
struct {...}`); there is no top-level `struct`/`enum` keyword, so an enum is the
RHS of a `type` declaration:
```goo
type Expr enum {
    Num{value: int}
    Add{lhs: *Expr, rhs: *Expr}
}
```

**Grammar** (`src/parser/parser.y`): a new `enum_type` type-expression
production (sibling of `struct_type`, added to the `type` production) that rides
the existing `type_decl` → `type` chain — no new top-level rule. Each variant
declares a name plus a brace-enclosed field list reusing the existing
`struct_field_list` production, so a variant is syntactically a named struct.

**AST** (`include/ast.h`, `src/ast`): `EnumDeclNode { char* name; ASTNode*
variants }` and `VariantNode { char* name; ASTNode* fields }` (fields are the
existing `StructField` list). New `AST_ENUM_DECL`, `AST_ENUM_VARIANT` kinds.

**Type system** (`include/types.h`, `src/types`): add `TYPE_ENUM`:
```c
struct {
    char* name;
    struct EnumVariant* variants;   // each carries a reused struct_type
    size_t variant_count;
} enum_type;
```
Register the enum name as a type and each variant name as a constructor bound to
its parent enum in the current scope.

**Layout / codegen** (`src/codegen`): represent an enum value as
`{ i32 tag, [N x i8] payload }`, where `N = max_v sizeof(variant_v struct)` and
the payload is bit-cast to the active variant's struct type. Construction
`Add{lhs: l, rhs: r}` stores `tag = index(Add)` then writes the variant struct
into the payload region. Field read on a known-variant value bit-casts the
payload and uses the existing struct-field path.

### 2. Lower `match`

**Typecheck** (`src/types`):
- Resolve the scrutinee type. If it is an enum, each `case Variant{f: bind}:`
  must name a variant of that enum; bind each destructured name to its field's
  type within the arm's scope.
- Enforce **exhaustiveness**: error if any variant is unhandled and no
  `default`/wildcard arm exists. Error on duplicate variant arms.
- Preserve existing literal/identifier/wildcard pattern handling for non-enum
  scrutinees (so `match` over a value still typechecks).
- Guards (`if cond`) typecheck as a bool condition within the arm scope.

**Codegen** (`src/codegen`): load the discriminant `tag`; lower dispatch as an
LLVM `switch` on the tag (or a compare chain) to per-arm blocks. In each arm,
bit-cast the payload to the variant struct and materialize the bound names as
arm-local allocas/values, then emit the arm body. A guard adds an extra
conditional branch inside the arm (fail → next-candidate / default). All arms
branch to a common merge block. Statement-style → no result value produced.

### 3. Dynamic slices

Add `append`, `len`, `cap` as builtins:
- Dispatch in `src/types/expression_checker.c` (typecheck: `append([]T, T) []T`,
  `len([]T) int`, `cap([]T) int`).
- Lower in `src/codegen/call_codegen.c` to the existing `goo_slice_append` and
  header field reads.
- Slice values carry the 3-field `{data, len, cap}` header so `append` can
  realloc-and-grow; `s = append(s, x)` returns the (possibly moved) header,
  which the caller stores back.

### 4. General maps

New `goo_map_sv` runtime (`src/runtime/`): string key → 8-byte value slot
(holds an `int64` or any pointer). Wire its symbols through
`src/codegen/runtime_integration.c`. The compiler casts the slot to/from the
declared value type `V`. `map[string]V` literals, `m[k]`, `m[k] = v`, and the
comma-ok read keep their current surface; codegen retargets the existing
`goo_map_*_si` call sites to `_sv` and reinterprets the slot per `V`.

## Error handling

All new checks report through the existing diagnostic path — no panics in the
checker:
- non-exhaustive `match` over an enum (no `default`)
- unknown variant in a `case`
- destructured field not present on the variant
- duplicate variant arm
- map value-type mismatch on store/read

Codegen only asserts invariants the typechecker has already guaranteed.

## Testing strategy

TDD per item, mirroring the M1 probe cadence. Each PR adds an
`examples/<name>_probe.goo` + `<name>_probe.expected.txt`, a Makefile target,
and entries in the `verify` aggregate and `.github/workflows/tests.yml`:

- `enum_probe.goo` — construct each variant, read fields back.
- `match_probe.goo` — **M2 gate**: build an `Expr` mini-AST tree, walk/evaluate
  it via `match` over the tag, recursing through `*Expr` payloads; assert the
  evaluated result.
- `slice_probe.goo` — append in a loop, observe `len`/`cap` growth, index reads.
- `map_probe.goo` — string→pointer symbol-table round-trip (insert, lookup,
  comma-ok miss).

No existing probe or test is weakened; new capability adds gates only.

## Sequencing

PRs land independently green; each is build-verified (`make goo` **and**
`make lib/libgoo_runtime.a`, then the probe) before merge.

1. **Enums** — grammar → AST → type → codegen; `enum_probe`.
2. **`match` lowering** — depends on (1); `match_probe` is the milestone gate.
3. **Dynamic slices** — independent of (1)/(2); `slice_probe`.
4. **General maps** — independent; `map_probe`.

PRs 3 and 4 may proceed in parallel with 1–2 (disjoint files: slices/maps touch
`expression_checker.c`/`call_codegen.c`/`runtime`, enums/match touch
`parser.y`/`ast`/`types`/`codegen` statement+expr paths).

## Out of scope (this milestone)

- Value-producing `match` (result slot / phi / arm-type unification).
- Positional enum variants and positional constructor patterns.
- Non-string map keys; arbitrary-size (by-value struct) map values.
- Generic/parameterized enums.
- A general `extern`/FFI mechanism (M2 continues the builtins-to-runtime
  pattern from M1).
