# LALR workaround map — where the grammar cheats, and how to extend it safely

Anchors verified 2026-07-04 on main @ 32a53d1. Each entry answers: what breaks if you
don't know this.

## 1. LBRACE_BODY lexer bridge — `if X {` vs `X{...}`
- Where: `src/parser/lexer_bridge.c:23` (frame push/pop), `:239` (emission decision);
  token doc at `src/parser/parser.y:164-170`; block arms at parser.y:1020+.
- The bridge emits `LBRACE_BODY` for condition-body braces so `struct_lit`'s plain
  `LBRACE` only matches outside cond contexts (or inside parens).
- Breaks if ignored: a new production starting `identifier LBRACE` will fight every
  `if`/`for`/`switch` body; a new statement form with a `{` body must get the
  LBRACE_BODY variant arm or it won't parse after conditions.

## 2. RBRACKET_SLICE token split — empty literal `[]` vs slice-type prefix `[]T`
- Where: doc at `src/parser/parser.y:171-176`; emission in `src/parser/lexer_bridge.c:74`
  region (lookahead after `[]`).
- The lexer decides between `LBRACKET RBRACKET` (empty literal) and
  `LBRACKET RBRACKET_SLICE type` so the parser never faces the reduce-vs-shift choice.
- Breaks if ignored: any new context where `[]` can start a type (conversions, params)
  must be reachable through `slice_type` (which consumes RBRACKET_SLICE), NOT hand-rolled
  `LBRACKET RBRACKET` sequences — those match only the empty-literal token shape.

## 3. type_call_arg lookahead split — `make(map[K]V, ...)` / `make([]T, n)`
- Where: `src/parser/parser.y:1764` (production), commentary at parser.y:1730-1763.
- Type forms as FIRST call args are grammar-only; the checker enforces the callee is
  `make`. CHAN is disjoint from primary_expr's first set; MAP/LBRACKET needed the split.
- Breaks if ignored: adding another builtin that takes a type argument should reuse
  `type_call_arg`, not introduce parallel type-arg arms (conflict factory).
- Related precedent: `[]byte(s)` conversions ride `slice_type LPAREN expression RPAREN`
  as a primary_expr arm (#111) — after a complete `slice_type`, LBRACE means composite
  literal, LPAREN means conversion; both are LR(1)-clean.

## 4. Struct-body-scoped ASI — newline → `;` inside struct bodies ONLY
- Where: `src/lexer/lexer.c:158` region.
- Shipped with #109 as a scoped pilot. Enum bodies deliberately get NO ASI (multi-line
  enum-variant embedding therefore hits a raw parse error — recorded message-quality gap).
- Breaks if ignored: expecting ASI anywhere else (statement level, literals) fails —
  Goo's grammar is otherwise newline-blind (see §6). One-line `struct { Base }` needs an
  explicit `;` before `}` (no bare-identifier embed production — S/R conflict).

## 5. COMMA-before-RBRACE arms — trailing commas in literals
- All five literal productions now carry the arm (post-#111): map parser.y:2066,
  named-struct :2082, slice :2198, array :2239, elided-struct :2271.
- The pattern is LR(1)-decidable (after `list COMMA`, lookahead RBRACE reduces, an
  element token shifts) — safe to replicate for future bracketed list productions.
- `{,}` stays rejected everywhere: every arm requires a non-empty list. Keep it that way.

## 6. Newline-blind func-result absorption hazard
- The grammar has no statement-level ASI, so `var f func()` followed on the NEXT LINE by
  a type-start token gets absorbed into the func result type (`var f func()` ↵ `!b`
  parses as `func() !b`). Whole-family hazard around `func_result` (parser.y:194, arms
  from :388).
- Breaks if ignored: adding tokens to a type's first set silently widens what the
  func-result position can swallow from the next line. Check this before extending
  type syntax.

## 7. Spread arm — final-only by construction
- `call_expr: primary_expr LPAREN expression_list ELLIPSIS RPAREN` (#111): ELLIPSIS sits
  after the whole list, so non-final spread is a parse error (Go-consistent) without any
  checker work. `CallExprNode.has_spread` is set ONLY by this arm; every other
  CallExprNode allocation site must zero it (all 5 sites live in parser.y — audit if you
  add one).

## 8. Header edits — the silent-miscompile rule
- The Makefile has NO header dependencies: after editing `include/ast.h` (or any header),
  run `make clean && make lexer`, or stale objects silently miscompile.
- `include/ast.h`: append new enum values and struct fields at the TAIL only — mid-enum
  insertion renumbers every later node type against stale objects.
