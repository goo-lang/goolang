# Stdlib Unblockers: trailing commas, []byte↔string, spread — Design

Date: 2026-07-04 · Branch: `feat/stdlib-unblockers` (base: main @ e4a3419, post-#110)
Status: user-approved design (brainstorm, section-by-section)

## Goal

Cut the distance to "vendored pure-Go stdlib source compiles on Goo" by closing
four highest-payoff idiom gaps, gated by ONE verbatim, gofmt'd stdlib function
running go-identically. This is the batch shape that shipped as #108 (port
unblockers): small known-shape items, risk-ascending, one PR.

Acceptance bar (user-selected): a real, unmodified stdlib function — pre-Builder
`strings.Join` — compiles verbatim and runs go-run-identically, plus per-feature
micro-goldens and reject probes.

## Scope

1. Trailing-comma grammar arms for `struct_lit` and `map_lit` (ONLY — see evidence).
2. `[]byte(s)` and `string(b []byte)` conversions, copy semantics.
3. Spread `f(s...)` through the variadic pack ABI + `append(dst, s...)` including
   Go's blessed `append([]byte, string...)` form.
4. `copy(dst, src)` builtin (incl. Go's blessed `copy([]byte, string)` form) —
   ADDED BY SPEC SELF-REVIEW: the actual Go 1.9 `strings.Join` builds with
   `copy`, not append-spread; every pre-Builder string function does. A verbatim
   gate is impossible without it, and it shares its runtime core with §3's bulk
   append helper (min-length memmove), so it joins the batch instead of sinking it.
5. Verbatim `strings.Join` (Go 1.9) gate probe.

Non-goals (recorded, not built): general `[]T(x)` conversions beyond `[]byte(string)`
(`[]rune(s)` is a future typecheck-only extension); non-string map keys; type
assertions; funcval `== nil`; `m[k]++`; embedded-NUL string fidelity.

## Evidence (probed 2026-07-04 against main-equivalent binary)

| Probe | Result |
|---|---|
| `[]int{1, 2,}` one-line + multiline gofmt form | OK (arms exist: parser.y:2167/2208; elided struct 2240) |
| `P{X: 1,}` / multiline keyed struct w/ trailing comma | PARSE-FAIL (`struct_lit`, parser.y:2050 — no COMMA arm) |
| `map[string]int{"a": 1,}` / multiline | PARSE-FAIL (`map_lit`, parser.y:2015 — no COMMA arm) |
| `sum(s...)` (variadic callee) | PARSE-FAIL at `s...` |
| `[]byte("hi")` | PARSE-FAIL at `(` |
| `string(b)` for `b []byte` | blocked behind `[]byte` parse; typecheck arm absent |

The handoff's "multiline trailing comma" framing was imprecise: the gap is
per-PRODUCTION (named struct literals and map literals, both line forms), not
per-line-structure.

## Design

### 1. Trailing commas (parser only)

One new alternative per production, mirroring the four existing proven arms:

```yacc
struct_lit: ... | identifier LBRACE struct_lit_inits COMMA RBRACE
map_lit:    ... | map_type   LBRACE map_entry_list   COMMA RBRACE
```

- Struct arm reuses `struct_literal_new` verbatim (parser.y:49/3237).
- Map arm's action is identical to the non-comma arm's key/value side-channel
  extraction (parser.y:2016–2029): factor that block into a `map_literal_new`
  helper FIRST, then both arms call it — the same refactor `struct_literal_new`
  models. No duplicated action bodies.
- `{,}` stays rejected: both arms require non-empty inits/entries (Go-consistent).
- `&T{...,}` is covered for free (the #90 heap arm parses through `struct_lit`)
  but gets an explicit golden line anyway.

Conflict analysis: after `inits COMMA`, lookahead RBRACE reduces the new arm;
any element-start token shifts — LR(1)-decidable, same shape as the existing
slice/array/elided arms. Expected bison delta: ZERO (hard gate below).

### 2. `[]byte(s)` / `string(b)` (grammar arm + typecheck + 2 runtime helpers)

Goo has no conversion AST node — `string(x)`/`byte(n)` are IDENT-callee calls
resolved at typecheck (the F2 pattern). `[]byte` is not an IDENT, so:

- **Grammar:** one primary-expression arm `slice_type LPAREN expression RPAREN`
  producing a dedicated conversion node (new AST arm carrying the parsed
  `slice_type` + operand; do NOT overload CallNode's IDENT-callee shape).
  Precedent for `slice_type` in call-shaped positions: `make()`'s
  `type_call_arg` split (parser.y:1696–1717). In expression contexts a complete
  `slice_type` is currently followed only by LBRACE (composite literal), so
  LPAREN should slot in conflict-free.
- **Typecheck:** grammar stays general (`[]T(x)` parses); typecheck restricts:
  element type must be `byte` AND operand must be `string`, else
  `"[]T(x) conversion is only supported for []byte(string) in v1"`.
  Reverse direction: extend the existing `string(x)` conversion arm to accept a
  `[]byte` operand alongside rune/byte.
- **Runtime (both directions COPY — Go-exact):**
  - `goo_slice* goo_slice_from_string(const char* s)` — alloc len bytes, memcpy;
    returns bare pointer per the slice-ABI rule (3-field `goo_slice_t` never
    crosses codegen↔C by value).
  - `char* goo_string_from_bytes(goo_slice* b)` — alloc len+1, memcpy,
    NUL-terminate.
- **Known rep limitation (documented, not solved):** Goo strings are
  NUL-terminated; byte slices containing 0x00 truncate on conversion back —
  the constraint the whole string layer already has.

### 3. Spread `f(s...)` + `append(dst, s...)`

**Foundation:** #105's variadic ABI (call_codegen.c:1127–1144) lowers a variadic
call to fixed params + exactly ONE packed-slice arg. Spread = skip the packing,
pass the given slice as the pack. Go's aliasing semantics (callee shares the
caller's backing array) fall out; the by-pointer slice ABI is respected because
the spread value takes exactly the shape the pack path already produces.

- **Parser:** `ELLIPSIS` token exists (param use at parser.y:577). One arm in the
  call-argument production: trailing `expression ELLIPSIS` sets a spread flag on
  the call node. Grammar accepts spread anywhere in the list; TYPECHECK enforces
  position (better diagnostics, less grammar surgery).
- **Typecheck (Go's exact rules), each with its own diagnostic + reject probe:**
  - callee must be variadic;
  - the spread arg must be final;
  - args before it must exactly fill the fixed params (no mixing packed
    elements with spread: `f(1, 2, s...)` rejects when only one fixed param);
  - operand must be `[]E` with `E` IDENTICAL to the variadic element type
    (no coercion — Go-exact; `f(int32s...)` into `...int64` rejects).
- **Codegen:** in the variadic-call path, when the spread flag is set, bypass
  pack construction and pass the operand's slice as the single pack arg.
- **`append(dst, s...)`:** separate builtin codegen arm (append is not the
  variadic-fn path). New runtime helper `goo_slice_append_slice` (single grow +
  bulk element move — legal because elem types are identical). The one
  heterogeneous pair Go blesses, `append([]byte, string...)`, routes through
  the same helper reading the string's bytes (composes with §2's helpers).
  Self-append (`append(b, b...)`) is legal Go: capture src ptr+len BEFORE any
  grow and move with memmove, never memcpy.

### 4. `copy(dst, src)` builtin

Go semantics exactly: copies `min(len(dst), len(src))` elements, returns that
count as `int`; no allocation, no growth; overlapping src/dst legal (memmove).
Second blessed form: `copy(dst []byte, src string)`. Typecheck: `dst` must be
`[]E`, `src` `[]E` with identical `E` (or the []byte/string pair); result type
int. Runtime: `int64_t goo_slice_copy(goo_slice* dst, goo_slice* src, size_t elem_size)`
plus a string-source variant — memmove core shared with `goo_slice_append_slice`.
Parser: `copy` follows the existing builtin pattern (IDENT-callee call resolved
at typecheck, like `append`/`len`/`delete`) — NO grammar change.

### 5. Verbatim gate probe: Go 1.9 `strings.Join`

`examples/strings_join_probe.goo`: Go 1.9's `strings.Join` VERBATIM (pinned
revision + BSD attribution comment; spec self-review confirmed its real body —
`switch len(a)` with 0/1/2/3 fast paths via string `+`, length precompute,
`b := make([]byte, n)`, `bp := copy(b, a[0])`, `bp += copy(b[bp:], sep)` loops
over `a[1:]`, `return string(b)`). Composes §2 (`string(b)`), §4 (`copy`, both
forms exercised via sep/elems), plus already-shipped make/slicing/range/switch.
The probe's `main` supplies the rest: §1 coverage via gofmt'd multiline
composite literals with trailing commas (including a `map[string][]string`
table, composing with #110 map values) and §3 coverage by calling its own
gofmt'd variadic helper with spread (`joinAll(parts...)` wrapping `Join`) and
`append(acc, chunk...)` accumulation. Expected output `go run`-verified.
(§3's spread thus has no verbatim-stdlib exemplar in the gate — small
self-contained variadic stdlib functions don't exist without interface or
`Clean`-sized dependencies; micro-goldens carry §3, the probe's main proves
composition.)

Rule: if the verbatim source trips anything OUTSIDE this batch's scope, that is
a finding to record (#108-re-probe style), not scope to absorb — the probe then
ships with the minimal documented deviation.

## Error handling summary (new diagnostics)

- `[]T(x)` with non-byte elem or non-string operand → clean v1-scoped message (§2).
- Spread: non-variadic callee / non-final spread / fixed-param mismatch /
  element-type mismatch → four distinct messages (§3).
- `copy`: element-type mismatch / non-slice dst / string dst → distinct messages (§4).
- No new runtime panics; `{,}` and existing rejections unchanged.

## Testing

- §1 goldens: named-struct + map trailing commas (one-line + multiline),
  `&T{...,}`; reject probe `{,}`.
- §2 goldens: round-trip `string([]byte(s)) == s`; mutation independence BOTH
  directions (copy pin); empty string; `len([]byte("héllo")) == 6`.
  Rejects: `[]int("x")`, `[]byte(42)`.
- §3 goldens: user-variadic forwarding `sum(s...)`; aliasing pin (`mut(s...)`
  mutates caller's backing array — Go-verified); empty + nil slice spread;
  append growth incl. string form; self-append `append(b, b...)`.
  Rejects: the four typecheck rules.
- §4 goldens: partial copy both directions (dst shorter / src shorter, return
  count pinned); overlapping copy within one slice (memmove pin);
  `copy(b, "str")`; zero-length edges. Rejects: elem mismatch,
  non-slice dst, `copy(string, ...)`.
- §5: the verbatim `strings.Join` probe (the batch's ship gate).
- Every golden's expected output verified with `go run` before commit.

## Tasks (risk-ascending), gates

- T1 trailing commas (pure grammar) → T2 `[]byte`↔`string` → T3 spread through
  the pack path → T4 bulk builtins (`copy` + `append` spread incl. string
  forms, shared memmove core) → T5 verbatim probe + full sweep + handoff.
  Each task a separate reviewer gate with controller probes (SDD economy mode:
  Sonnet implements, Fable controller reviews).
- Per-task: `make test` (76/1), golden count monotonic from 215/0, bison
  tripwire **exactly 81 S/R + 256 R/R** — T1/T2/T3 each touch the grammar; ANY
  delta stops the task for shape-diff + differential verification (#92
  precedent).
- Final: full `make verify` + `ccomp-link` (opam `default` switch), push, PR,
  fresh-context whole-branch review before merge (the step that caught #109's
  ptr-boxing find and #110's I1).

## Risks

- §2's `slice_type LPAREN` is the only grammar change with real conflict risk
  (interplay with the `type_call_arg` lookahead split). Mitigation: the hard
  bison gate + stop rule; fallback is restricting the arm's position further.
- §3 aliasing must be pinned by golden, not assumed — if the pack path turns
  out to copy at the ABI boundary, that's a deviation finding, not a silent pass.
- §5's verbatim source is Go-version-sensitive (`Builder` from 1.10 on) — the
  pinned Go 1.9 revision needs exactly §2+§4 and nothing newer; pin the exact
  revision in the probe's attribution comment. If the pinned source still trips
  an out-of-scope gap, record-don't-absorb applies.
- Scope grew once already (self-review added `copy`) — the batch is now FROZEN
  at §§1–5; any further discovery is a recorded finding, not new scope.
