# Plan: array literals `[N]T{...}`

**Status:** proposed. The recurring blocker for table-driven stdlib (utf8's
`first [256]uint8`, the deBruijn `var [N]byte{...}` we worked around as const
strings, general arrays).

## Current state (spiked)
- `[3]int{10, 20, 30}` mis-parses: the parser has no `array_type LBRACE
  expression_list RBRACE` rule, so it routes to the struct-literal path with
  `type_name = "int"` (the element type) — hence "Unknown type 'int' in struct
  literal". The `[3]` dimension is dropped.
- Array **indexing** already works (composite_codegen.c TYPE_ARRAY). Only
  *construction* (the literal) is missing.
- A close template exists: the **slice** literal path `[]T{...}`
  (parser.y:1837 `slice_type LBRACE expression_list RBRACE`) is fully wired
  (parser + AST + type-check + codegen). Array literals should mirror it, plus
  carry the fixed size N.

## Steps
1. **Parser**: add an `array_type` use in a composite-literal rule —
   `array_type LBRACE expression_list RBRACE` and the empty `... LBRACE RBRACE`
   form. `array_type` is `LBRACKET expression RBRACKET type` (already parsed at
   1883). Build an array-literal AST node carrying the element type, the size
   expression, and the element list. Watch the reduce/reduce interaction with
   the struct-literal and slice-literal rules (the `[` … `]` … `{` lookahead).
2. **AST**: either a new `ArrayLiteralNode` (size + elem type + elements) or
   reuse the slice-literal node with an added size field. Prefer reuse if the
   slice node already holds an element list + element type.
3. **Type check**: `[N]T{e...}` → `TYPE_ARRAY` of T, size N (const-folded from
   the size expression via goo_fold_const_int). Validate: element count ≤ N
   (Go zero-fills the rest), each element assignable to T. Stamp node_type.
4. **Codegen**: materialize an array value — alloca `[N x T]`, store each
   element at its index (GEP), zero-fill omitted trailing elements. For a
   package-level `var tab = [N]T{...}`, emit a constant global array initializer
   (builder-free, like the const-string globals) so it works at global scope.
5. **Probes**: local `[3]int{...}` (index + len), package-level
   `var tab = [4]byte{...}` (the table shape), a partial literal (zero-fill),
   and an index with a uint8 (reuses the sign-extension fix).

## Payoff
Unlocks: utf8 `RuneCountInString`/`ValidString`/`DecodeRune` (the `first` table),
truly-verbatim deBruijn tables in math/bits (drop the const-string deviation),
and general array support across the stdlib.

## Notes / risks
- Grammar conflicts: the composite-literal area is already conflict-heavy;
  add the rule, then judge by BEHAVIOR (full golden + a thorough array probe),
  not conflict count — the lesson from tagless switch / unary `^`.
- Package-level array-global codegen is the analog of the C1 const-string
  crash fix; emit a constant initializer, never the IR builder, at global scope.
