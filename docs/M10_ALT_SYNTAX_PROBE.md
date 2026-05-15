# M10 alternative-syntax probe (2026-05-15)

Empirical probe of "Strategy 3" from `docs/M10_COMPOSITE_LITERAL_SPIKE_2.md` —
pick an unambiguous-by-construction surface syntax for composite literals so
the `IDENT . LBRACE` cond/struct-literal ambiguity never arises.

Baseline (clean `parser.y`): **61 shift/reduce, 156 reduce/reduce.**
Bison build via `bison -d -o src/parser/parser.tab.c src/parser/parser.y`.
Runtime verification via `make lexer` + `bin/goo` on small `.goo` probes
(linker errors against `lib/libgoo_runtime.a` are expected in this worktree
and unrelated — only parse / type-check outcomes matter).

## TL;DR

- **Candidates A (`Point.{...}`) and C (`Point[...]`) are non-starters at the
  grammar level.** Both add only 3–4 S/R conflicts on paper, but in each case
  Bison's default-shift resolution commits the parser into a dead-end state
  that breaks a load-bearing existing construct: A breaks **all selectors**
  (`fmt.Println`, `obj.field`); C breaks **all indexing** (`arr[0]`, `m[k]`).
  Verified empirically: `bin/goo` on `fmt.Println("hi")` parse-fails at the
  `(` under A, and `arr[0]` parse-fails at the `0` under C.
- **Candidates B (`&Point{...}`) and D (`#Point{...}`) are both grammatically
  clean.** B adds +1 S/R; D adds +1 S/R; neither perturbs R/R. Both leave
  selectors and indexing untouched. Both make the original `if x { body }`
  cond/struct-literal ambiguity go away by construction.
- **B's grammatical "cost" is semantic, not syntactic.** Mandating `&` means
  every struct literal is a heap-pointer, so `[]User{...}` cannot exist — it
  becomes `[]*User{&User{...}, &User{...}}`. Value-typed slices of structs
  (extremely common in Go) become impossible without a separate non-pointer
  form. B also still has one corner: `if &x { body }` parse-fails (`&IDENT`
  in cond position is the same shape that motivated the whole problem). The
  parens workaround `if (&x) { body }` parses cleanly.
- **D is the cleanest grammatically.** A sigil prefix (`#` is free in
  `include/token.h` / `src/lexer/lexer.c`) is a 3-token unique prefix
  `SIGIL_HASH IDENT LBRACE` that cannot start any other construct. The 1 added
  S/R is benign (RETURN's "empty vs expression" implicit-semicolon ambiguity,
  same shape as every other expression-starting token). Cost: ~5 lines of
  lexer + bridge work to actually emit the token.
- **Recommended pick if forced**: **D (prefix sigil)** for pure grammatical
  cleanness, or **B (`&Type{}`)** if Go-aesthetic conformance is weighted
  highly and the value-vs-pointer semantic divergence is acceptable. Both
  beat the current "stuck" state by a wide margin. Neither A nor C should
  ship.

## Candidate comparison table

| Cand. | Syntax | Δ S/R | Δ R/R | Parse status of probes | UX assessment | Go-source compat. break | Recommendation |
|---|---|---|---|---|---|---|---|
| A | `Point.{x: 3, y: 4}` | +4 | 0 | **Breaks `fmt.Println`** — `IDENT.IDENT(...)` selector hijacked into struct-lit path | Zig-flavored, foreign to Go readers | YES — `pkg.member` ambiguous with `Type.{}` | **Reject** |
| B | `&Point{x: 3, y: 4}` | +1 | 0 | Works. `if total == 6 { body }` works. `if &x { body }` fails (parens fix). | Always-pointer; loses value-typed struct slices | YES — `Type{}` value form gone | Acceptable if pointer-by-default is OK |
| C | `Point[x: 3, y: 4]` | +3 | 0 | **Breaks `arr[0]`** — `IDENT[expr]` index_expr hijacked into struct-lit path | Reads like a generic instantiation or index, confusing | YES — indexing semantics ambiguous | **Reject** |
| D | `#Point{x: 3, y: 4}` | +1 | 0 | Grammar-only probe (no lexer support); state-table analysis clean | Unfamiliar sigil; reads like a preprocessor or attribute | YES — new sigil unknown to Go | **Cleanest; pick if Go-aesthetic loss is tolerable** |

## Per-candidate detail

### A. Zig-style `Point.{x: 3, y: 4}`

Grammar change: `struct_lit: identifier DOT LBRACE struct_lit_inits RBRACE`.
Bison count: 65 S/R, 156 R/R (+4 S/R).

The +4 S/R appear at the state where `identifier .` can either reduce
to `primary_expr` (continuing into `selector_expr: primary_expr DOT identifier`
or `index_expr: primary_expr LBRACKET ...`) **or** shift the DOT into the new
struct_lit prefix. Bison's default-shift resolution commits to struct_lit.
The successor state on DOT then only accepts LBRACE; any other lookahead is
an error.

Empirical check (`/tmp/probe_a_pkg.goo`): `fmt.Println("hi")` parse-fails at
column 17 (the `(` after `Println`). The parser shifted on `.`, entered the
struct-lit-prefix state, saw `Println` (IDENT), kept shifting (state then
expected LBRACE), saw `(`, and errored.

The Zig form `p := Point.{x: 3, y: 4}` itself parses fine — but it has
destroyed every selector in the language. Non-starter.

### B. Always-pointer `&Point{x: 3, y: 4}`

Grammar change: `struct_lit: BIT_AND identifier LBRACE struct_lit_inits RBRACE`.
Bison count: 62 S/R, 156 R/R (+1 S/R).

The +1 S/R is at the `BIT_AND identifier .` state, where Bison must choose
between reducing IDENT to primary_expr (continuing the existing `BIT_AND
unary_expr` address-of operator path) and shifting LBRACE (entering struct_lit).
Default-shift resolves correctly: LBRACE goes to struct_lit. Crucially, the
shift only fires *when LBRACE is the lookahead* — `&x + 1` and `&obj.field`
are unaffected because their lookahead after `&x` is PLUS or DOT, not LBRACE.

Empirical check:
- `p := &Point{x: 3, y: 4}` — parses + type-checks cleanly.
- `if total == 6 { x := total }` — parses cleanly. **The original cond bug
  is fully avoided** because struct_lit now requires `&` so bare `IDENT { ... }`
  in cond position never matches struct_lit.
- `if &x { body }` — parse-fails. This is the residual corner: `&x` in cond
  position is the same `BIT_AND IDENT LBRACE` shape that struct_lit eats.
  Workaround: `if (&x) { body }` parses cleanly.
- `arr[0]`, `fmt.Println(...)` — unaffected; both parse cleanly.

UX patterns:
- `p := &Point{x: 3, y: 4}` — readable, clearly Go-flavored with explicit
  pointer semantics. Same shape Go programmers already use deliberately.
- `users := []*User{&User{name: "alice"}, &User{name: "bob"}}` — **the
  outer slice type changes** from `[]User` (value-typed) to `[]*User`
  (pointer-typed). This is the meaningful semantic cost: value slices of
  structs (a Go workhorse pattern) become inexpressible without a separate
  non-pointer struct-literal form.
- `func makeOrigin() Point { return &Point{0, 0} }` — type-mismatched
  (returns `*Point`, declared `Point`). The function signature would have
  to change to `*Point`, propagating heap-allocation semantics throughout
  the program.

The semantic cost is the killer. Grammatically B is fine; semantically,
forbidding value-typed struct literals is a meaningful divergence from Go's
stack-friendly defaults. If the project is willing to make `Type{}` always
mean pointer-to-Type (with new-allocation semantics), B works; otherwise it
needs a second syntax for the value form, at which point the cond-disambig
returns.

### C. Square brackets `Point[x: 3, y: 4]`

Grammar change: `struct_lit: identifier LBRACKET struct_lit_inits RBRACKET`.
Bison count: 64 S/R, 156 R/R (+3 S/R).

Same defect as A, applied to `[` instead of `.`. The conflict at `identifier .`
between reduce-to-primary_expr (then `primary_expr LBRACKET expression RBRACKET`
is index_expr) vs shift-LBRACKET (struct_lit prefix) resolves to shift. The
successor state on LBRACKET (state 208 in the generated table) only accepts
IDENTIFIER (the start of a `struct_lit_init: identifier COLON expression`),
not the arbitrary expression that index_expr needs.

Empirical check (`/tmp/probe_c_idx.goo`): `arr[0]` parse-fails at column 16
(the `0`). Indexing is broken across the entire language. Even an attempt to
restrict struct_lit to keyed-only form (omitting the positional form) doesn't
help — the failure is at the very first token inside `[`.

C also has a UX problem: `Point[T]` is the natural reading for generic
instantiation, which Goolang already uses for concepts (`concept Foo[T] {}`)
and type parameters. Overloading `[` for struct literals collides with the
existing generics direction.

Non-starter.

### D. Prefix sigil `#Point{x: 3, y: 4}`

Grammar change: add `%token SIGIL_HASH` and
`struct_lit: SIGIL_HASH identifier LBRACE struct_lit_inits RBRACE`.
Bison count: 62 S/R, 156 R/R (+1 S/R).

The +1 S/R is at the RETURN-statement state (state 321 in the generated
table) deciding between "empty return" and "return with expression" — Bison
sees SIGIL_HASH could start an expression (struct_lit is reachable via
primary_expr → expression) so it gets added to the lookahead set, joining
the same implicit-semicolon shift/reduce that every other expression-starter
token already participates in. This is the standard, benign baseline pattern;
no existing construct is perturbed.

Critically, **the 3-token prefix `SIGIL_HASH IDENT LBRACE` is unique to
struct_lit** — no other production starts with `#`, so there is no shift/
reduce decision to make. Selectors, indexing, address-of, unary operators,
and `if X { body }` cond positions are all entirely unaffected.

UX patterns:
- `p := #Point{x: 3, y: 4}` — readable, unambiguous. Sigil reads as a tag
  or marker; weakly familiar from C preprocessor / Lisp reader macros.
- `users := []User{#User{name: "alice"}, #User{name: "bob"}}` — clean. The
  outer `[]User` keeps value semantics (no forced pointers like B). Inner
  `#User{...}` is the literal.
- `func makeOrigin() Point { return #Point{0, 0} }` — works, value-typed.

Adoption work (not done in this probe): ~5 lines.
- `include/token.h`: add `TOKEN_SIGIL_HASH`.
- `src/lexer/lexer.c`: add `case '#': token = token_new(TOKEN_SIGIL_HASH, ...);`.
- `src/parser/lexer_bridge.c`: add `case TOKEN_SIGIL_HASH: return SIGIL_HASH;`.
- Documentation: the sigil needs a name and a justification in the spec.

Alternatives if `#` reads poorly: `@` is taken (DEREF/ATTRIBUTE), `~` is
taken (BIT_NOT), `$` is free but visually heavier. `#` reads cleanly.

## Verdict on strategy 3 (alt-syntax)

**Strategy 3 is viable** — but only the prefix-sigil (D) and always-pointer
(B) variants. The two more Go-aesthetic options (Zig dot-brace A and square
brackets C) both fail empirically: their default-shift resolutions break
selectors and indexing respectively, and no precedence-directive fix is
visible without redesigning the surrounding grammar.

If the project is willing to absorb the Go-source-compatibility break
(CLAUDE.md describes the project as "Go-compatible", so this is a real
cost), **D (prefix sigil)** is the cleanest pick. It adds 1 benign Bison
conflict, leaves the entire rest of the grammar untouched, costs ~5 lines
of lexer + bridge work, and supports both keyed and positional forms with
value semantics.

**B (`&Type{...}`)** is the next-best, retaining most Go visual aesthetics
but mandating heap-pointer semantics for every struct literal — a meaningful
language-design divergence that should be a deliberate operator decision, not
a side-effect of the grammar fix.

If neither divergence is acceptable, Strategy 3 should be rejected in favor
of Strategy 1 (GLR parser) or Strategy 2 (lexer-context tracking) from the
spike-2 doc.

## Open questions

- **Q1.** Does the project accept the Go-source-compat break? The CLAUDE.md
  framing ("Go-compatible language with additional features") suggests no,
  but M7-stdlib-expansion and `for i, v := range` already diverged. A
  threshold needs to be set.
- **Q2.** If D is picked, what's the sigil's name in the spec? "Hash literal",
  "struct constructor", "compound literal"? Naming affects discoverability
  in errors and tutorials.
- **Q3.** If B is picked, does the project add a parallel value-form syntax
  later (`Point{...}` reserved for value, `&Point{...}` for pointer)? That
  re-introduces the cond ambiguity for the value form. Better to commit to
  pointer-only and document the consequence.
- **Q4.** The +1 S/R conflict in D appears in the RETURN state. Same shape
  for B's +1 conflict (in the BIT_AND state). Are these the same "expression
  may follow" implicit-semicolon class that the existing 61 baseline conflicts
  belong to? Worth folding into the M10-baseline-conflicts audit work.
- **Q5.** None of the four candidates were paired with `%prec` / precedence
  directives. A precedence rule that gives the struct_lit prefix lower priority
  than reduce-to-primary_expr might rescue A or C. Out of scope for this
  probe; flag as a follow-up if the grammatically-clean options are rejected
  on aesthetic grounds.
