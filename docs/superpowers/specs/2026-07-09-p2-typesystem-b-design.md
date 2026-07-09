# Phase 2 sub-project B — forward type refs (P2.3), missing-return (P2.4), nullable equality (P2.5)

**Date:** 2026-07-09
**Branch:** `feat/p2-typesystem-b` (off main, post PR #168)
**Premises:** verified by static trace + corpus analysis 2026-07-09 (recon report); roadmap anchors corrected (tie-the-knot now type_checker.c:2062-2207; the generic codegen failure now expression_codegen.c:2399). New finding folded in: `T == ?T` accepts/rejects asymmetrically by operand order.

## T1 — P2.3: forward and mutual type references

**Mechanism (extends two proven primitives, no new machinery):** `type_check_program` already runs two passes with a name-only signature hoist for FUNCTIONS (type_checker.c:644-655, declare_function_signature); and `type_check_type_decl` already does shell-allocate → resolve-body → tie-the-knot for SELF-references (type_checker.c:2073-2113, proven by nil_ptr_walk_probe). T1 = a type-shell pre-pass before today's pass 1: walk `prog->decls`, register an empty TYPE_STRUCT/TYPE_ENUM shell + forward Variable for every top-level `type` name (exactly the per-decl shell code, hoisted); pass 1's body resolution then ties each knot into its pre-registered shell.

**Scope:** struct and enum declarations (the kinds the shell primitive supports). Interface declarations referencing later-declared types: include only if it falls out free from the same pre-pass; otherwise document as a follow-up. Duplicate-name diagnostics must be unchanged (the pre-pass sees duplicates first — keep the error identical). `type_check_package` (stdlib path, :669-728) mirrors the same shape — apply there too or stdlib vendoring hits the same wall later.

**Acceptance (roadmap P2.3):** `type A struct { b *B }` before `type B` compiles; mutual recursion (A↔B pointer fields) compiles in BOTH orders and a traversal probe runs; self-reference path (nil_ptr_walk_probe) unchanged; duplicate type name still rejected with today's message.

## T2 — P2.4: missing-return analysis for value-returning functions

**Current state (verified):** zero return-path analysis exists; codegen unconditionally appends `ret zero` to unterminated value-returning functions (function_codegen.c:1215-1240) — the silent-0 mechanism. That codegen fallback STAYS (it is load-bearing for the terminator-blind LLVM plumbing); the checker gains the analysis so the fallback becomes unreachable-for-user-code.

**Terminating-statement predicate** (Go spec §Terminating statements, extended for Goo constructs; the corpus-validated minimum is marked ★ — 7 at-risk goldens depend on those):
- ★ `return` (any form); `panic(...)` call statement
- ★ `if`/`else` where BOTH branches terminate; ★ `if-let`/`else` both terminate (AST_IF_LET_STMT is a distinct node)
- ★ `switch` and ★ type-switch with a `default` clause where every clause body terminates (a clause ending in `fallthrough` delegates to the next clause's terminator)
- `for` with no condition (`for {}`) containing no `break` targeting it (labeled break considered); `goto`; labeled statement wrapping a terminating statement
- ★ `arena { ... }` block whose final statement terminates (Goo-specific — delegate like a plain block); plain block whose final statement terminates
- select with no default? Go: select{} blocks forever = terminating; a select where every case body terminates is terminating — implement the simple Go rule (select with no break and all cases terminating); if awkward, document select-as-nonterminating as a v1 conservatism with a reject-side note (it forces a dead `return`, annoying but sound).

**Application:** value-returning named functions AND func literals; diagnostic `missing return` positioned at the closing brace. Void functions untouched.

**Acceptance (roadmap P2.4):** the `f(x int) int { if x>0 { return 1 } }` shape rejected; all 7 recon-named at-risk goldens (arena_embedded_escape_probe, arena_escape_return_probe, asi_else_probe, nullable_assign_probe, nullable_intret_probe, type_switch_probe, type_switch_valptr_probe) still green; full corpus green; reject fixtures: bare missing return, if-without-else, switch-without-default, non-terminating last clause.

## T3 — P2.5: nullable equality (tag-aware, symmetric)

**Decision (this spec):** implement tag-aware semantics for `?T == ?T` AND both mixed orders `T == ?T` / `?T == T` — not rejection. Rationale: `?T` is the flagship differentiator; comparing an optional against a concrete value is the idiomatic read (`if x == opt`), if-let already covers unwrap-style code, and the asymmetric today-behavior (one order accepted then crashes codegen) proves users will write it. Ordered comparisons on `?T` stay rejected.

**Semantics:** `?a == ?b` ⇔ `tag(a)==tag(b) && (both-nil || payload(a)==payload(b))`. `t == ?b` ⇔ `!nil(b) && t == payload(b)` (symmetric for the flipped order). `!=` is the negation. Payload comparison reuses the existing scalar/string equality lowering — restrict v1 to payload types that already have `==` (int/float/bool/string/pointer); `?Struct == ?Struct` rejected with a positioned diagnostic (struct equality is its own future feature).

**Implementation anchors (recon-verified):** checker — explicit arms in `type_check_comparison_op` (expression_helpers.c, before the generic fallback at :439) for NULLABLE==NULLABLE (bases compatible) and NULLABLE==T/T==NULLABLE (base compatible with T); this REPLACES the accidental asymmetric acceptance via `type_compatible` (fixing the direction-sensitivity finding). Codegen — new TOKEN_EQ/TOKEN_NE arm mirroring F2's pattern (expression_codegen.c:2230+): extract `is_null` (field 0) and payload (field 1) per the `{i1, payload}` layout (nullable_codegen.c:15), branchless `and/or` of the tag and payload compares.

**Acceptance (roadmap P2.5):** golden covering all three `?T==?T` cases (nil/nil true, nil/value false, value/value payload-compare) + both mixed orders + `!=`; nothing reaches "Failed to generate binary operation" (expression_codegen.c:2399) for any `?T` comparison; `?Struct==?Struct` and `?T < ?T` reject fixtures; nullable_nilcmp_probe unchanged.

## Review regime

Checker-semantics heavy: ONE Fable dimension (termination-analysis adversarial shapes — nested terminators, fallthrough chains ending switches, arena-in-if-in-switch compositions, dead-code-after-return, func literals as last statements; plus ?T equality edge shapes incl. width-mixed payloads `?int64 == ?int64` from narrow literals) + ONE Sonnet fixture/regression-accounting dimension; Opus verify critical/major only. Haiku gate.

## Review outcome (2026-07-09)

Right-sized review (1 Fable semantics + 1 Sonnet fixture dimension, Opus verifiers): **1 confirmed
major, fixed pre-merge** — ?Struct types rendered as "??" in the new diagnostics (root-fixed in
type_nullable via type_to_string, `1d084e6`; reject fixture pins ?Point). Two majors **refuted as
pre-existing on main**, recorded as follow-ups:
- Struct EMBEDDING a later-declared struct still rejected (embed check predates the shell pre-pass;
  forward-ref via ordinary pointer/slice/map fields works).
- Package-level var initializers cannot reference a later-declared struct's fields.
Clean areas: three-way mutual recursion, method-body forward refs, composite-type forward refs,
all termination edge shapes probed (labeled break targeting, fallthrough chains, for{} rules,
dead-code-after-terminator through codegen), nullable NaN semantics, evaluation-once for ?T
call operands, width-mixed payloads. T2 shipped two documented soundness-tightening deviations
(break-tracking in switch; any-statement-terminates blocks matching Goo's dead-code grammar).
