# LALR workaround map — where the grammar cheats, and how to extend it safely

Anchors verified 2026-07-04 on main @ 32a53d1; §§4/6/8 rewritten 2026-07-11
post-P5.10 (PR #183: Go-shaped statement termination + generalized ASI).
Each entry answers: what breaks if you don't know this.

## 1. LBRACE_BODY lexer bridge — `if X {` vs `X{...}`
- Where: `src/parser/lexer_bridge.c:23` (frame push/pop), `:239` (emission decision);
  token doc at `src/parser/parser.y:143-149`; block arms at parser.y:946+.
- The bridge emits `LBRACE_BODY` for condition-body braces so `struct_lit`'s plain
  `LBRACE` only matches outside cond contexts (or inside parens).
- Breaks if ignored: a new production starting `identifier LBRACE` will fight every
  `if`/`for`/`switch` body; a new statement form with a `{` body must get the
  LBRACE_BODY variant arm or it won't parse after conditions.

## 2. RBRACKET_SLICE token split — empty literal `[]` vs slice-type prefix `[]T`
- Where: doc at `src/parser/parser.y:150-158`; emission in `src/parser/lexer_bridge.c:74`
  region (lookahead after `[]`).
- The lexer decides between `LBRACKET RBRACKET` (empty literal) and
  `LBRACKET RBRACKET_SLICE type` so the parser never faces the reduce-vs-shift choice.
- Breaks if ignored: any new context where `[]` can start a type (conversions, params)
  must be reachable through `slice_type` (which consumes RBRACKET_SLICE), NOT hand-rolled
  `LBRACKET RBRACKET` sequences — those match only the empty-literal token shape.

## 3. type_call_arg lookahead split — `make(map[K]V, ...)` / `make([]T, n)`
- Where: `src/parser/parser.y:2076` (production), commentary at parser.y:2020-2035.
- Type forms as FIRST call args are grammar-only; the checker enforces the callee is
  `make`. CHAN is disjoint from primary_expr's first set; MAP/LBRACKET needed the split.
- Breaks if ignored: adding another builtin that takes a type argument should reuse
  `type_call_arg`, not introduce parallel type-arg arms (conflict factory).
- Related precedent: `[]byte(s)` conversions ride `slice_type LPAREN expression RPAREN`
  as a primary_expr arm (#111) — after a complete `slice_type`, LBRACE means composite
  literal, LPAREN means conversion; both are LR(1)-clean.

## 4. Generalized rule-1 ASI (P5.10) — newline after a value-ender → `;`, four exceptions
- Where: `src/lexer/lexer.c`, the `'\n'` case of lexer_next_token (Part 1 =
  unconditional keyword terminators RETURN/BREAK/CONTINUE/FALLTHROUGH; Part 2 =
  the generalized rule); `token_ends_value()` defines the ender set (NIL included).
- Since P5.10 the lexer inserts a SEMICOLON at EVERY newline following a
  value-ending token — Go spec rule 1 — because the grammar's statement lists are
  now terminator-REQUIRING (see the statement_list/stmt_seq/final_stmt comment in
  parser.y). Four exceptions, each a pinned Goo-accepts-where-Go-rejects leniency:
  next char `)` (asi_multiline_probe), next char `}` (composite literals; blocks'
  final statements ride final_stmt), `...` (asi_spread_probe), and the word `else`
  (asi_else_probe, word-boundary checked). The older struct-body/var-group scoped
  ASI (asi_ctx, #109/P1.2) remains in place beneath it — subsumed for value-enders,
  still load-bearing for its group-scoped edge shapes.
- Breaks if ignored: a new statement/decl construct whose members end in
  value-enders WILL receive semicolons between newline-separated members — give it
  member-attached trailing-SEMICOLON arms (`spec SEMICOLON`, the house pattern:
  package_clause/import_spec/const_spec/enum_variant/top_level_decl all carry
  them). NEVER a list-level `list SEMICOLON` arm (creates the more-list-vs-
  trailing-terminator conflict — rejected twice, see var_member's note in
  parser.y). And any new leniency exception in the lexer MUST come with a golden
  that pins it, or a future cleanup will silently tighten the language.

## 5. COMMA-before-RBRACE arms — trailing commas in literals
- All five literal productions now carry the arm (post-#111): map parser.y:2366,
  named-struct :2382, slice :2462, array :2482, elided-struct :2500.
- The pattern is LR(1)-decidable (after `list COMMA`, lookahead RBRACE reduces, an
  element token shifts) — safe to replicate for future bracketed list productions.
- `{,}` stays rejected everywhere: every arm requires a non-empty list. Keep it that way.

## 6. Func-result absorption hazard — NEUTRALIZED at statement level by P5.10
- Historical form: with no statement-level ASI, `var f func()` followed on the NEXT
  LINE by a type-start token was absorbed into the func result type (`func() !b`).
- Post-P5.10 status: `)` is a value-ender, so the generalized ASI (§4) inserts a
  SEMICOLON at the newline and the absorption cannot happen across lines (verified
  empirically 2026-07-11: `var f func()` ↵ `!true` compiles as two statements).
- STILL REAL on a single line: `var f func() !b` (same line) absorbs by design —
  that IS a result type. The residual hazard when extending type syntax: any new
  token added to a type's FIRST set widens what a SAME-LINE func-result position
  can swallow. The cross-line case is now guarded by ASI rather than luck; do not
  remove `)` from token_ends_value without re-opening this whole family.

## 7. Spread arm — final-only by construction
- `call_expr: primary_expr LPAREN expression_list ELLIPSIS RPAREN` (#111): ELLIPSIS sits
  after the whole list, so non-final spread is a parse error (Go-consistent) without any
  checker work. `CallExprNode.has_spread` is set ONLY by this arm; every other
  CallExprNode allocation site must zero it (all 5 sites live in parser.y — audit if you
  add one).

## 8. Header edits — dependency tracking EXISTS now, tail-append still the rule
- CORRECTED 2026-07-11: the Makefile HAS header dependencies (-MMD/-MP, PR #123) —
  header edits rebuild their dependents automatically; `make clean` is no longer
  required for correctness after an `include/ast.h` edit.
- `include/ast.h`: append new enum values and struct fields at the TAIL only — the
  convention survives the dep-tracking fix because out-of-tree consumers and any
  future dep-tracking regression both fail silently on mid-enum renumbering, and
  tail-append costs nothing.
- If you add a field to a parser-allocated node, audit every
  `malloc(sizeof(<Node>))` site to initialize it — malloc garbage in a flag is a
  heisenbug factory (the has_spread audit precedent, §7).
