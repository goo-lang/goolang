# Struct Embedding — Design

**Date:** 2026-07-04
**Status:** Approved (user-validated section by section)
**Prior art:** `2026-06-30-named-non-struct-types-design.md` (method mangling),
`2026-06-30-interface-assign-gaps-design.md` / `2026-06-30-interface-containers-design.md`
(interface satisfaction + vtables).

## Goal

Go-style struct embedding: anonymous fields with field and method promotion, so that
real Go code using composition-by-embedding ports verbatim. This is the last MED-LARGE
wall identified by the 2026-07-03 re-probe before out-of-tree Go ports are broadly viable.

```go
type Base struct { N int }
func (b *Base) Bump() { b.N++ }

type Outer struct {
    Base        // value embedding
    X int
}
// o.N, o.Bump(), and Outer satisfying interfaces via Bump all work.
```

## Scope (decided)

**In:**
- Value embedding of named struct types: `struct { Base }`.
- Pointer embedding: `struct { *Base }` (promotion through an auto-deref hop).
- Embedding named non-struct types (`type MyInt int` with methods) — cheap because
  method mangling already handles them.
- Promoted **field** access (`o.X` where `X` lives in an embedded type, any depth).
- Promoted **method** calls (`o.M()` dispatching to `Base__M`).
- Promoted methods count toward **interface satisfaction**, including dynamic dispatch
  through vtables (forwarding thunks).
- Go-exact ambiguity semantics: shallowest-unique wins, outer shadows inner,
  same-depth collision is an error **at the use site only**; explicit paths
  (`o.Base.X`) always work.
- Struct-body-scoped ASI in the lexer (prerequisite; see Parsing).

**Out (deferred, clean errors or recorded):**
- Embedding **interface** types in structs (`struct { io.Reader }`) → explicit
  "not yet supported" typecheck error. Separate mechanism (dynamic-dispatch promotion,
  satisfaction-by-containment) on top of a young vtable system.
- Go's method-set asymmetry (pointer-receiver methods only in `*T`'s method set):
  **deliberately not enforced.** Goo's dispatch is compiler-wide lenient
  (auto-address/auto-deref; `type_receiver_name` strips `*`). Embedding inherits that
  leniency. Documented compatible-superset deviation.
- New nil checks: promoted access through a nil `*Base` traps exactly like today's
  `o.P.V` through nil (mechanism parity with existing pointer selectors).

## Parsing: struct-body-scoped ASI (the hard part)

**Problem.** `struct_field_list` is juxtaposed and the grammar is newline-blind.
Every current field is ≥2 tokens, so greedy `identifier type` pairing is deterministic.
An embedded field is **1 token**; `struct { Base \n X int }` is `IDENT IDENT IDENT`
and shift-preference would misparse it as field `Base` of type `X` plus embedded
`int` — with `A \n B` (two embedded fields) silently becoming *field A of type B*.

**Decision: implement Go's semicolon-insertion rule scoped to struct bodies.**
- Lexer keeps a small brace-context stack: `{` following `STRUCT` pushes
  *struct-field context*; any other `{` (enum, interface, nested non-struct brace)
  pushes *no-emit*; `}` pops.
- In struct-field context, at a newline, if the previous token can end a field
  (IDENT, `)`, `]`, `}`), emit a synthetic `SEMICOLON`.
- Outside struct bodies behavior is byte-identical (zero regression surface for the
  golden suite). Existing multi-line structs start receiving `;` tokens and land on
  the **already existing** `identifier type SEMICOLON` field productions.
- This is a deliberately scoped pilot of the full-ASI reform on the long-standing queue.

**Grammar delta** (`parser.y` `struct_field`): add embedded-field productions.
Embedded productions are **semicolon-terminated only** (`identifier SEMICOLON`,
`MUL identifier SEMICOLON`) — zero bison-conflict delta. The `;` is ASI-inserted
at newlines, so all multi-line Go source parses verbatim; the one refinement vs
Go is that a ONE-LINE `struct { Base }` must be written `struct { Base; }`
(same family as the documented `}; stmt` one-line quirk). A bare-identifier
production would cost ~2 justified S/R conflicts; deferred unless it bites.

Qualified embedded type names (`pkg.Type`) follow whatever the `type_name`
grammar supports today (goostd is flat); no new surface invented for them.

## Representation

- `VarDeclNode` gains tail flag `is_embedded` (append at struct tail per the
  ast.h no-header-deps convention; build sequence needs `make clean`).
  Parser stores the Go-rule implicit name: `Base` and `*Base` both yield a field
  named `"Base"`; `field->type` holds the (possibly pointer) type node.
- `StructField` (types.h) gains matching `is_embedded` (tail append).
- `type_from_ast` (AST_STRUCT_TYPE) builds the embedded member as an **ordinary
  field** of `struct_type.fields[]`. LLVM layout (positional), composite literals,
  and explicit-path access work with zero special-casing.
- The `name_count == 0 → continue` silent-drop at `type_checker.c:2232` becomes an
  internal error (unreachable once embedded fields are always named).

**Declaration-site constraints (typecheck errors):**
- Embedded type must be a named struct type, named non-struct type, or pointer to one.
- Interface types → the deferred "not yet supported" error.
- Duplicate names (`struct { Base; Base }`, `struct { Base; *Base }`, or collision
  with a declared field) → duplicate-field error.
- Value-embedding recursion (A embeds B embeds A) → infinite size, rejected.
  Pointer-embedding cycles (`A{*B}` / `B{*A}`) are **legal**, so the resolver
  carries a visited set.

## The embedding resolver (one new module)

`src/types/embedding.c` — single BFS over the embedding graph, the sole owner of
promotion semantics. Consumed by (1) selector typechecking, (2) interface
satisfaction, (3) vtable construction.

```
embedding_resolve(checker, structType, name) →
    FOUND_FIELD  { path }                 // list of (field index, deref_needed)
  | FOUND_METHOD { path, mangled_name }
  | AMBIGUOUS    { depth, candidates }    // ≥2 hits at the shallowest hit depth
  | NOT_FOUND
```

- Depth 0 = direct fields + direct `T__name` method lookup (methods are mangled
  globals registered as Variables; resolver takes the checker for lookups).
- Level k = members of embedded types at depth k. First depth with exactly one hit
  wins; outer shadows inner. Visited set terminates pointer cycles.

## Promotion = AST-rewrite desugar (no codegen changes for direct use)

When the selector typechecker's flat scan misses and the resolver returns a path,
**rewrite the AST in place**: `o.X` with path `[Base]` becomes `(o.Base).X` by
inserting intermediate `SelectorExprNode`s; `o.M()` rewrites the receiver to
`o.Base` before normal method resolution. Downstream code then sees only
already-shipped constructs — single-level field GEPs, selector-through-pointer
loads (#91), pointer/value receiver auto-addressing (call_codegen). Promotion in
Go is *defined* as this sugar; the mutable C AST lets us execute the definition
literally. Addressability of `o.Base` follows `o`, matching Go.

`AMBIGUOUS` at a use site → error
`ambiguous selector o.X (found via Base.X and Other.X)` — both paths spelled out
because the fix (write the explicit path) is exactly what the message shows.

## Interface satisfaction + vtable thunks (the only codegen change)

- `type_interface_satisfied`: on direct `T__m` miss, ask the resolver for a
  promoted method and compare signatures as today.
- `codegen_interface_vtable`: same fallback when filling slots. For a promoted
  slot, emit a forwarding thunk (`Outer__m__promoted` mangling): take the boxed
  `*Outer` receiver the dispatch already passes, GEP along the resolver's path
  (loading at each pointer hop), tail-call `Base__m`. Thunks are emitted only when
  an `Outer→Interface` conversion actually compiles — no dead code. Typecheck and
  thunk emitter consume the identical path structure, so they cannot disagree.

## Edge semantics (decided)

- **Nil pointer embedding:** hardware trap, same as existing nil pointer selectors.
- **Composite literals:** `Outer{Base: Base{...}, X: 1}` keyed and positional forms
  work for free. Promoted names in literals (`Outer{InnerField: ...}`) remain
  illegal (Go rule); the literal checker already scans only direct fields —
  pinned by a reject probe.
- **Explicit paths** (`o.Base.X`) bypass promotion entirely.

## Testing

- **Golden probes** (go-run differential): value embedding field+method promotion;
  depth-2 transitive promotion; pointer embedding with mutation through the
  promoted path; shadowing (outer wins); interface satisfied via promoted method
  with real dynamic dispatch through the thunk; promoted method on an embedded
  named non-struct type.
- **Reject probes:** ambiguous bare selector; promoted name in a composite literal;
  embedded interface (deferred message); duplicate embedded names; value-embedding
  recursion.
- **ASI probe family (lexer):** multi-line structs with/without embedding; comments
  and blank lines between fields; one-line `struct { Base; X int }`; nested
  `struct{...}` inside a field type; enum/interface bodies unchanged.
- **Guards:** bison 81/256 exact; golden suite + probe count green; full `make test`.

## Approach record (alternatives considered)

- **A — promotion-aware resolution at every site, no thunks:** ruled out; a vtable
  slot needs a `*Outer`-ABI function pointer, so a wrapper is forced for nonzero
  offsets regardless — the "no thunks" premise is unachievable.
- **B — eager forwarding thunks for all promoted methods:** viable but needs a new
  whole-program post-declaration pass, generates dead thunks, and makes
  error-at-use ambiguity diagnostics awkward (the generator must silently skip
  ambiguous names, degrading the eventual error).
- **C — use-site desugar + lazy vtable thunks: chosen.** One resolver module owns
  the semantics; the heavily-tested method-call path is reused rather than
  paralleled; thunks only where ABI forces them.
- **Parsing (a) — explicit `;` terminators + line guard:** rejected because every
  real Go port would need hand-editing, defeating the feature's purpose.
  **(b) struct-scoped ASI: chosen**, in the same PR.
