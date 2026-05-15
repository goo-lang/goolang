# M10 composite-literal grammar spike (2026-05-15)

Empirical investigation of the Bison shift/reduce conflict introduced by adding `Type{...}` composite literals to `primary_expr`. The M10 audit identified the conflict by inspection; this spike confirms it via Bison, evaluates four resolutions, and recommends one with a concrete impl plan.

## TL;DR

- **Conflict confirmed via Bison.** Adding `IDENT LBRACE field_list RBRACE` as a `primary_expr` alternative produces exactly **1 shift/reduce conflict** at the state where the parser has just shifted `IDENT` and lookahead is `LBRACE`. Bison's default tie-break (shift) would silently break bare-identifier conditions like `if X { body }` — `X { body }` would parse as a composite literal followed by nothing.
- **Resolution A (parallel `expression_nc` hierarchy) Bison-validates with zero conflicts.** Verified on a minimal reproduction grammar.
- **Recommended:** Resolution A. Adds ~80–120 lines of Bison productions across `expression_nc / binary_expr_nc / unary_expr_nc / postfix_expr_nc / primary_expr_nc`, all passthrough actions (`$$ = $1`). 5 grammar sites swap `expression` for `expression_nc` in `if/for/switch`-like cond positions. Users get the standard Go quirk: `if (Point{1,2}.x > 0)` works, `if Point{1,2}.x > 0` doesn't.
- **Impl scope estimate:** 200–300 LOC of parser.y additions, plus the three sibling impl tasks (struct lit, slice lit, map lit) at maybe 100–150 LOC each across `parser.y` / `ast.c` / `type_checker.c` / `expression_codegen.c`. Total M10 composite-literal work: 600–900 LOC. Audit's original 200 LOC/child estimate was too optimistic.

## Empirical conflict reproduction

A minimal Bison input with the offending production:

```
%token IDENT NUM LBRACE RBRACE LPAREN RPAREN IF COLON COMMA
%%
stmt:     IF expr block | expr_stmt ;
expr:     IDENT | NUM | composite_lit | LPAREN expr RPAREN ;
composite_lit: IDENT LBRACE field_list RBRACE ;
field_list: /* empty */ | field | field_list COMMA field ;
field:    IDENT COLON expr | expr ;
block:    LBRACE stmt_list RBRACE ;
```

`bison -d` reports:

```
/tmp/test_conflict.y: conflicts: 1 shift/reduce
```

`bison --verbose` pinpoints the state:

```
state 1
    7 expr: IDENT .
   11 composite_lit: IDENT . LBRACE field_list RBRACE

    LBRACE  shift, and go to state 11
    LBRACE    [reduce using rule 7 (expr)]
```

The parser has consumed `IDENT` and sees `LBRACE` next. Two valid productions apply: reduce `IDENT` to `expr` (so `LBRACE` starts an if-body), or shift `LBRACE` to begin a composite literal. Bison defaults to shift, which means `if X { body }` always parses as `X{body}` (the composite literal) — breaking all bare-identifier conditions.

The existing slice literal form `[1, 2, 3]` does **not** cause this conflict: square brackets aren't reusable as block delimiters, so there's nothing to disambiguate. This is why the slice form already works in `if [1,2,3][0] == 1 { ... }` while the curly-brace form can't.

## Candidate resolutions

### A. Parallel `expression_nc` hierarchy *(recommended)*

Add a parallel non-terminal cascade (`expression_nc → binary_expr_nc → unary_expr_nc → postfix_expr_nc → primary_expr_nc`) that's identical to the normal expression hierarchy except `primary_expr_nc` excludes the `composite_lit` alternative. Use `expression_nc` in `if/for/switch` cond positions. Inner positions (call args, index, paren-grouped) immediately switch back to the normal `expression` rule, so composite literals work everywhere except at the immediate stmt-leading slot.

Validated on the minimal grammar with zero Bison conflicts:

```
expr:        IDENT | NUM | composite_lit | LPAREN expr RPAREN ;
expr_no_clit: IDENT | NUM            | LPAREN expr RPAREN ;
stmt: IF expr_no_clit block | expr_stmt ;
```

`bison -d` → exit 0, no conflicts.

**Pros:**
- Zero syntax burden on users in non-cond positions (`x := Point{1, 2}` works as expected).
- Matches Go's de-facto syntactic constraint exactly.
- Pure grammar change; no lexer state, no semantic-level handling.
- Bison-validated empirically.

**Cons:**
- ~80–120 lines of Bison rule duplication. Mostly passthrough actions.
- Adds a "did you mean parens?" UX moment for the rare `if Point{...}.x > 0` pattern. Mitigation: error-recovery hint.
- Slight grammar maintenance tax: future changes to the expression cascade must mirror to `_nc`.

### B. Lexer context flag

Lexer tracks whether composite literals are currently allowed (push "no-clit" on `IF`/`FOR`/`SWITCH`, pop on the matching body-LBRACE). The lexer returns `LBRACE` normally but in the no-clit state, an `IDENT . LBRACE` transition is suppressed in the parser via a different token (e.g., `LBRACE_NOT_CLIT`).

**Pros:**
- Single token, no grammar duplication.
- Same user-facing semantics as A.

**Cons:**
- Lexer becomes context-sensitive — fragile in the face of nested `if` / `for` inside expressions (lambda bodies, parenthesized contexts).
- Bison can't validate the context flag — bugs only surface at runtime.
- The `lexer_bridge.c` glue would need significant rework.

Not recommended — the grammar-level fix in A is cleaner and validates statically.

### C. Different syntax for composite literals

Use a syntactically unambiguous form: `~Point{...}`, `@Point{...}`, `.{...}` (Zig-style anonymous), or `(Point){...}` (always-parenthesized).

**Pros:**
- No grammar conflict at all.
- Could enable richer literal forms (anonymous structs).

**Cons:**
- Breaks Go-source compatibility, which the M7-stdlib-expansion shortcut and `for i, v := range` syntax both signal as a project intent.
- Higher user-facing cost than A's quirk.
- Loses the natural mental model of `Type{fields}`.

Not recommended — the Goolang project has been Go-aesthetic-conformant elsewhere.

### D. Required parens in cond positions (parse error otherwise)

Bison shifts (default), produces a composite literal, then the parser detects "composite literal in stmt-leading cond position" semantically and rejects with an error.

**Pros:**
- Smallest grammar change (no `_nc` hierarchy).
- Error message can be helpful: "wrap struct literal in parens: `if (Point{...}.x > 0)`".

**Cons:**
- Bare-identifier conditions (`if x { ... }` where `x : bool`) silently break: `x { ... }` parses as `x{}` composite literal of an empty struct followed by nothing. The default-shift behavior means the parser doesn't even know the user meant a bare cond.
- Hard to recover sensibly from a parse error this deep in the grammar.

Not recommended for the same reason as Bison's default — bare bool conds break silently.

## Touch-point inventory for resolution A

Sites where the existing parser uses `expression` in a stmt-leading cond context (these become `expression_nc`):

| Line | Rule | Context |
|---|---|---|
| 734 | `IF expression block` | If statement |
| 745 | `IF expression block ELSE block` | If/else |
| 756 | `IF expression block ELSE if_stmt` | Else-if chain |
| 798 | `FOR expression block` | While-style for |
| 806 | `FOR simple_stmt SEMICOLON expression SEMICOLON simple_stmt block` | C-style for, middle expr is the cond |
| 1856 | `IF expression` | Comprehension or expression-level if (verify usage) |

Sites where `expression` stays unchanged (these are unambiguous because they're inside delimiters):

- `expression_list` in call args
- `expression` after `RANGE`
- `expression` inside `LPAREN ... RPAREN`
- `expression` in index `LBRACKET ... RBRACKET`
- `expression` as RHS of `ASSIGN` / `SHORT_ASSIGN`
- `expression` in `RETURN expression`

So `expression_nc` is only used at the 6 sites above. Each is a one-token swap.

## Recommended decomposition

The M10 audit filed three impl children plus a probe gate. After this spike, the cleaner shape is **one shared grammar prep + three thin children**:

```
M10
├── M10-audit                       done
├── M10-variadic-println            done
├── M10-range-two-var               cancelled (already works)
├── M10-composite-literal-spike     <this doc>
├── M10-composite-literal-grammar   NEW — parallel _nc hierarchy in parser.y
│                                    only. Adds AST_STRUCT_LITERAL,
│                                    AST_MAP_LITERAL_GO (the {} form,
│                                    distinct from existing AST_SLICE_EXPR
│                                    for [] form), with empty-body parse-only
│                                    behavior so it can land independently.
├── M10-struct-literal              REFILE — pure type-check + codegen
│                                   atop M10-composite-literal-grammar.
│                                   ~100 LOC.
├── M10-collection-literal-go       REFILE — pure type-check + codegen for
│                                   the {} form atop the grammar work.
│                                   The existing []-form (AST_SLICE_EXPR)
│                                   stays untouched. ~120 LOC.
└── M10-probe-gate                  REFILE — examples/m10_probe.goo
                                    exercising struct + map + slice
                                    {} forms + Println. Joins make verify
                                    once children gate green.
```

This makes the grammar work a single atomic landing — easier to review, easier to revert — and the typed children become small mechanical changes once it ships.

## Impl plan for M10-composite-literal-grammar (the next child)

Rough order, ~250 LOC of parser.y:

1. **Add `expression_nc / binary_expr_nc / unary_expr_nc / postfix_expr_nc / primary_expr_nc`** as parallel rules. Bodies are passthrough actions (`$$ = $1`). Each rule mirrors its normal sibling minus the composite-literal alternative.
2. **Add `struct_lit` and `map_lit_go` productions** to `primary_expr` only:
   ```
   primary_expr: ...
       | struct_lit
       | map_lit_go ;
   struct_lit: type_name LBRACE struct_field_list_opt RBRACE ;
   map_lit_go: map_type LBRACE map_entry_list_opt RBRACE ;
   ```
   For this child, the AST nodes can be stubs: tag as `AST_STRUCT_LITERAL` and `AST_MAP_LITERAL_GO` but leave the type-check and codegen arms as TODOs that emit a clear "not yet supported" error. The grammar lands cleanly; impl children turn the stubs into real code.
3. **Swap `expression` → `expression_nc`** at the 6 sites in the touch-point table.
4. **Verify:** `make lexer` builds, `bison -d` reports zero conflicts, existing examples (`baseline_probe`, `comptime_probe`, etc.) still parse and run.

## Open questions

- **Q1.** The grammar duplication adds maintenance tax for future expression-hierarchy edits. Is there an existing Goolang convention for keeping parallel rules in sync (a comment marker, a script, a CI lint)? Probably worth adding one as part of the grammar landing — a `// MIRROR _nc:` annotation per pair would catch drift.
- **Q2.** The existing slice literal `[1, 2, 3]` (AST_SLICE_EXPR) coexists with the new `[]int{1, 2, 3}` form. Should they be different AST kinds (current proposal) or unified? Different kinds are simpler — different surface syntax often *wants* different node types for diagnostics anyway. Unifying can be a follow-on.
- **Q3.** Map literal in Goolang currently has a parser rule (`map_lit`) producing `AST_MAP_LITERAL`. Does that already handle the `map[K]V{...}` Go form, or only an internal `{k:v, ...}` form? Worth checking before the grammar child lands — may not need a new AST kind.
- **Q4.** Does parser.y line 1856's `IF expression` (looks like a comprehension or expression-level if) also need `expression_nc`? Verify the rule's surrounding context during the grammar landing.

## Conclusion

The grammar conflict is real, has a clean known fix (parallel `_nc` hierarchy), and Bison-validates empirically. The next M10 child should be `M10-composite-literal-grammar` landing the parser-only changes, with the typed impl children (`M10-struct-literal`, `M10-collection-literal-go`, `M10-probe-gate`) refiled to depend on it.

Total revised M10 estimate: ~600–900 LOC across grammar + 3 typed forms. Higher than the audit's 200 LOC/child but matches the actual shape of the work.
