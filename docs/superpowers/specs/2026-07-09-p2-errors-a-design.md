# Phase 2 sub-project C — error-union interop & ergonomics (P2.6–P2.9, P2.11)

**Date:** 2026-07-09
**Branch:** `feat/p2-errors-a` (off main, post PR #169)
**User decision:** P2.9 = the value-yielding `catch => expr` form (2026-07-09).
**Premises:** recon-verified 2026-07-09. Key correction to the roadmap: there is NO reusable (T,error)→!T bridge to generalize — strconv.Atoi's !int is two hand-written special cases (type-check string-match at expression_checker.c:4140-4146; bespoke codegen at call_codegen.c:2018-2101). The working (T,error) mechanism is the generic result-tuple destructure (type_checker.c:1951-1958, function_codegen.c:1770-1786, pinned by tuple_error_probe). Expression-position catch ALREADY works (erru_catch_value_probe pins value-producing handlers).

## T1 — P2.7: catch-bound variable gets the `error` interface

**Verified gap:** catch binds `e` as the union's error arm defaulting to raw TYPE_STRING (expression_checker.c:4413-4419; error_union_codegen.c:255-260), so `e.Error()` fails with "Selector on non-struct, non-package type" — while the destructure path binds the real `error` interface (type_checker_error_type, type_checker.c:1966) and boxes via goo_error_from_string (function_codegen.c:1710-1734), so `err.Error()` works. Close the asymmetry:
1. expression_checker.c:4413-4419 → bind `type_checker_error_type(checker)`.
2. error_union_codegen.c:255-266 → box the raw error through goo_error_from_string before storing (mirror function_codegen.c:1727-1729), so extraction matches the working call_codegen.c:1119-1130 shape.
3. Carry the destructure path's documented non-string-error-arm degradation (function_codegen.c:1706-1734) identically.
**Acceptance:** `f() catch e { fmt.Println(e.Error()) }` prints the original message (golden); `fmt.Println(e)` unchanged; the erru_* fixture family green.

## T2 — P2.6: try/catch accept user-declared (T, error) returns

**Design (option ii from recon — extend the gates, don't rewrite call returns):** `type_check_try_expr` / `type_check_catch_expr` (expression_checker.c:4337-4459) additionally accept a 2-field `is_result_tuple` struct (ast.h:768) whose second field is the `error` interface (type_is_error). Result type = field 0's type. Codegen: in the try/catch lowering (error_union_codegen.c:119-174 / :248-347), when the operand is a result tuple: evaluate ONCE, extract field 0 (value) and field 1 (error), synthesize the same control flow as !T but keying is-error on `field1 != nil` (the error interface is nullable-pointer-shaped — {i1,ptr}; nil-check via the existing nullable is_null read). Error binding for catch: field 1 IS already the boxed error interface — no re-boxing.
**Semantics:** `try classic()` propagates: in a `!T`-returning function, the propagated error converts to the union's error arm (string arm: via e.Error(); document if the enclosing function returns (T,error) — v1: try only propagates OUT of !T-returning functions, a (T,error)-returning enclosing function gets a clean diagnostic "try requires the enclosing function to return an error union" + reject fixture). `classic() catch e {}` binds e as the error interface (consistent with T1), zero/fallback value semantics identical to !T catch.
**Acceptance (roadmap P2.6):** golden probes for both paths (`try classic()` inside a !T function propagating; `classic() catch e {}` binding with recovery); tuple_error_probe (the destructure sibling) untouched and green; atoi fixtures green.

## T3 — P2.9: `catch => expr` + the ast_node_new privatization rider

**Sugar over proven machinery:** the value-producing block-catch already works, so `f() catch => -1` desugars to the existing CatchExprNode with no bound variable and a fallback expression. Grammar: `catch_expr: expression CATCH FAT_ARROW expression` — check whether '=>' lexes today; if not, add the two-char token (precedent: &^ TOKEN_AND_NOT, enum tail-append, bridged). goo-grammar skill discipline: tripwire 121/256 exact or counterexample-analyzed ledger delta; probe `catch => x + 1` greediness (the fallback expression binds per normal expression rules; document the parse).
**Type check:** fallback expression must be assignable to the union's value type (or the result-tuple's field 0 once T2 lands); error variable absent — no binding.
**Rider (same branch, parser.y finally in scope):** add `ast_break_stmt_new`/`ast_continue_stmt_new`/`ast_fallthrough_stmt_new` typed constructors, reroute parser.y:1349/1365/1406, make bare `ast_node_new` static to ast.c — completing #167's R3.
**Acceptance:** goldens `catch_arrow_probe` (`x := f() catch => -1`, both paths; nested in call args; `catch =>` with a call fallback); tripwire discipline; rider proven by `nm`/grep showing no external ast_node_new.

## T4 — P2.8: diagnostics quality pass (scoped to the four verified gaps)

1. **Sibling constructor rendering (the #169 pattern, 4+ sites):** type_pointer (types.c:315-323), type_slice (:177-192), type_map (:196-215), type_error_union (:359-379), plus type_reference/type_function — replace bare `->name` with `type_to_string(...)` so `*Node`/`[]Node`/`map[Node]int`/`!Node` render structurally. Reject-fixture or probe each.
2. **Cascade suppression:** a failed initializer registers a POISONED scope entry (Variable with an error-marker type) so later references to the name produce no further diagnostics (verified cascade: one root error currently spawns 2 spurious diagnostics per subsequent use). Keep the root diagnostic byte-identical.
3. **Unhandled !T at the binding:** `x := f()` where the inferred type is a raw !T (RHS is not try/catch/destructure) → positioned error naming the three options ("error union must be handled: use try, catch, or v, err := destructuring"). Also append the same remedy hint to the existing `Cannot assign !T to T` message. Reject fixtures for both. (Verified today: raw binding compiles SILENTLY — no diagnostic at all.)
4. **try-precedence hint (diagnostic only — precedence unchanged):** TRY binds loose (parser.y:211 %right), so `try f() + 1` parses as `try (f() + 1)` and dies with "Arithmetic operation requires numeric operands". When that arithmetic error fires with an error-union operand, append: "error unions must be unwrapped before arithmetic — did you mean (try f()) + 1?". Reject fixture pins the hint.

## T5 — P2.11: golden capstone pinning the v1 !T/?T contract

Fixtures (run + reject) per the roadmap list, updated for decided semantics: catch => fallback (both paths); try across two frames; unhandled !T rejected (T4's fixture may satisfy); ?T unwrap-panic path (expected-exit fixture — the P0.8 sidecar convention); ?T in struct field and param; ?T==?T as shipped in #169; (T,error) bridging both paths (T2's fixtures may satisfy). Audit which already exist post T1-T4; add only the gaps; one summary comment block in the spec listing the full capstone matrix with fixture names.

## Out of scope (documented)

Changing TRY's precedence (behavior change to existing programs; revisit post-v1 with a corpus diff); catch on ?T (nullables use if-let); (T,error)-returning enclosing functions receiving try-propagation.

## Review regime

1 Fable dimension (error-path semantics: double-evaluation of tuple operands, catch=>-inside-catch, try/catch on method calls & !T methods, error boxing identity through e.Error(), poisoned-binding interaction with valid later shadowing) + 1 Sonnet fixture dimension; Opus verify critical/major. Haiku gate.
