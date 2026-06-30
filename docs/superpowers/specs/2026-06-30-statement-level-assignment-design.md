# Statement-level assignment (enables tuple-index assignment)

**Date:** 2026-06-30
**Phase:** v1 Phase 3 (Go-source compatibility)
**Trigger:** the TinyGo `sort` port — the idiomatic `Swap` is `s[i], s[j] = s[j], s[i]`,
which does not parse. Today's port uses a temp-based Swap workaround.
**North star:** `s[i], s[j] = s[j], s[i]` (and `p.a, p.b = p.b, p.a`) compile and run,
making the sort port fully Go-faithful.

## Problem

Multi-assignment to **indexed/selector lvalues** does not parse:
`s[i], s[j] = s[j], s[i]` → syntax error. Identifier-tuple (`a, b = b, a`) and
single index assign (`s[0] = s[1]`) both work; only the indexed/selector *tuple*
fails.

Three prior attempts (introducing an `assign_lhs` nonterminal of index/selector
forms) all produced reduce/reduce conflicts that corrupted single index/selector
assignment — because a non-identifier lvalue is reducible BOTH to `expression`
(for the expression-level single-assign rule) AND to the new target nonterminal.

## Root cause

This grammar treats assignment as an **expression**: `expression ASSIGN expression`
and `expression SHORT_ASSIGN expression` live in the `expression:` block
(parser.y:1286/1290). Multi-assignment is a separate **statement** rule hardcoded
to `identifier COMMA identifier ASSIGN expression COMMA expression` (in
`simple_stmt`). Any attempt to add index/selector multi-targets at the statement
level collides with the expression-level single-assign on the shared lvalue
reduction.

## Architecture decision

Make assignment a **statement**, Go-faithfully: `ExpressionList = ExpressionList`,
where the LHS uses the SAME `expression` nonterminal as everything else (one
reduction path → no reduce/reduce conflict), and **addressability is a semantic
(typecheck) check**, not a grammar one.

Rejected: (A) a separate `assign_lhs` nonterminal — the 3 prior attempts; intrinsic
r/r. (B) leave deferred with a permanent temp-Swap — un-Go-faithful; the landmine
persists for all real Go code.

## Design

### Grammar (`src/parser/parser.y`)

In `simple_stmt`, replace the identifier-only multi rule with single + multi rules
that both use `expression` for targets:
```
simple_stmt:
    expression                          // pure expr-statement (no assignment)
  | short_var_decl                      // := stays identifier-only (unchanged)
  | var_decl
  | expression ASSIGN expression                    // single assign, ANY lvalue
  | expression COMMA expression ASSIGN expression COMMA expression   // 2-target multi, ANY lvalue
  ;
```
Remove from the `expression:` block: `expression ASSIGN expression` (1286) and
`expression SHORT_ASSIGN expression` (1290). Remove the old
`identifier COMMA identifier ASSIGN …` multi rule (subsumed by the new one).
(v1 grammar produces exactly count==2 for multi — keep that bound; the single
rule covers count==1.)

Single vs multi disambiguates on `ASSIGN` vs `COMMA` after the first `expression`
— the same mechanism the identifier-tuple already relies on, so no new r/r.

### AST (minimal ripple)

- Single `expression ASSIGN expression` keeps producing the existing
  `BinaryExpr(ASSIGN)` node — typecheck/codegen for single assign are UNCHANGED.
- Multi produces `AST_MULTI_ASSIGN` via the existing `multi_assign_2_new($1,$3,$5,$7,0)`
  — its typecheck (`type_check_multi_assign`, per-target `type_check_expression`)
  and codegen (`codegen_generate_multi_assign`, per-target
  `codegen_emit_lvalue_address`) ALREADY handle arbitrary lvalue targets (that is
  why `a,b=b,a` and the temp-Swap work). So no AST/codegen change is needed beyond
  the grammar.

### Addressability (typecheck)

With `expression` as the LHS, the grammar now also accepts non-lvalue targets
(`f() = 3`, `a+b = 3`). Add an addressability check in the single-assign and
multi-assign typecheck: each target must be an identifier, index, selector, or
deref (the lvalue forms). Emit a clean `type_error` ("cannot assign to <expr>:
not addressable") otherwise — no silent acceptance, no crash. (The single-assign
path `type_check_assignment_op` and `type_check_multi_assign` are the two sites.)

## Testing

- `examples/index_swap_probe.goo` (+`.expected.txt`): `s[i], s[j] = s[j], s[i]`
  swaps slice elements; assert sorted output.
- `examples/selector_swap_probe.goo`: `p.a, p.b = p.b, p.a` swaps struct fields.
- Regression: `a, b = b, a` (identifiers), `s[0] = s[1]` (single index), `p.x = 5`
  (single selector), `x := s[i]` (short-decl), C-style for `i = i + 1` post, all
  still compile/run (covered by existing golden + new probes).
- Non-lvalue rejection: `f() = 3` cleanly errors (manual check; golden can't test
  compile errors).
- Capstone: update `examples/sort_named_probe.goo`'s `Swap` to the idiomatic
  `s[i], s[j] = s[j], s[i]` — the full sort still sorts `1 2 3`.

Gate: `make verify` ALL GREEN (incl. ccomp), golden green, `make test` 76/1.
Empirically measure bison s/r + r/r conflict deltas (no `%expect` gate exists);
a delta is acceptable only if golden + test stay green (bison resolves
correctly).

## Out of scope (follow-up)
Compound assignments `+= -= *= …` (handle only if they already exist at the
expression level and fall out of the move for free; otherwise defer). >2 targets
(grammar is bounded to 2 in v1). Chained `x = y = z` (not valid Go; removed by
this change — acceptable).

## Risk
- Removing assignment from `expression:` could change conflict counts or break a
  statement form that routed single-assign through `simple_stmt → expression`.
  Mitigated by empirical golden + test gating and the for-loop `simple_stmt`
  coverage.
- `:=` currently has a redundant expression-level rule (1290); removing it must
  not break `x := y` (handled by `short_var_decl`). Verify `:=` probes stay green.
