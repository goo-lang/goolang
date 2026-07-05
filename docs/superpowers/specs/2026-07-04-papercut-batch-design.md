# Papercut Batch — Design

Date: 2026-07-04 · Branch: `feat/papercut-batch` (base: main @ 59c615e, post-#115)
Status: user-approved design (high-value 4; one batch branch, approach A)

## Goal

Close the four highest-value accumulated real-usage friction gaps — one of which
(#6) is a silent correctness bug, not a papercut. Together they are much of what
makes ported Go feel rough.

## Scope (user-selected: high-value 4)

- **#6** multi-return interface coercion — SILENT MISCOMPILE (a concrete returned
  into an interface-typed multi-return prints empty / fails module-verify). Codegen.
- **#1** if-init guard — `if v, ok := x.(T); ok {` and `if x := f(); cond {`. Grammar.
- **#2** `m[k]++` / `m[k] += n` map compound writes — currently hard-rejected. Codegen.
- **#4** elided inner literals in map values — `map[string][]int{"a": {1,2}}`. Grammar.

Dropped (user): #5 `for {...}; stmt` one-liner (cosmetic — multi-line always works);
#3 blank-`_` reuse across `:=` (works except one narrow type-assertion comma-ok path).
Both recorded for a later cycle.

## Triage (probed 2026-07-04)

| # | repro | layer | status |
|---|---|---|---|
| 6 | `func f() (Sayer,Sayer){ return Cat{},Cat{} }` | codegen | compiles, prints EMPTY (go: meow meow); var form `return c,c` → module-verify fail |
| 1 | `if v, ok := m["a"]; ok {` | grammar | parse error |
| 2 | `m["a"]++` / `m["a"] += 5` | codegen | rejected: "cannot assign through a map value" |
| 4 | `map[string][]int{"a": {1,2}}` | grammar | parse error |

## 1. Codegen fixes (no grammar risk)

### #6 — multi-return interface box (correctness)
`statement_codegen.c:1315-1329`: the return-aggregate loop generates each return
value, loads lvalues, and `LLVMBuildInsertValue`s it into the return struct WITHOUT
interface-boxing. When the i-th return type is an interface and the value is
concrete, the raw bits go into an interface-shaped slot → empty/garbage (or verifier
failure for the variable form). FIX: mirror the nullable auto-wrap immediately above
(`:1324-1328`, same "field_type says X, value is bare, wrap it" shape) — when
`field_type->kind == TYPE_INTERFACE` and `vv->goo_type` is concrete (not already
interface), call `codegen_interface_box(codegen, checker, field_type, vv->goo_type,
raw)` before InsertValue. Same helper that fixed #110 I1 and backs #114. `field_type`
is already computed (`function_return_type->data.struct_type.fields[i].type` — the
multi-return aggregate is modeled as a struct). Value AND pointer-concrete both boxed
(codegen_interface_box already handles the pointer form via #113/#114).

### #2 — map compound writes (RMW desugar)
`expression_codegen.c:1047`: compound-assign resolves its target via
`codegen_emit_lvalue_address(binary->left)`, which rejects map-index targets (map
values aren't addressable — correct in general). FIX: BEFORE the lvalue-address
resolution, special-case a map-index target (`binary->left` is `AST_INDEX_EXPR` with
a `TYPE_MAP` base) and desugar to read-modify-write: read the current value via the
map-get path, compute `old <op> rhs` reusing the existing binary-op codegen, write
back via the map-set fast path. Postfix `m[k]++` is `m[k] += 1` through the same
desugar (find the postfix `++`/`--` codegen and route map-index targets here too).
Mirrors how the plain `m[k] = v` fast path already intercepts `TYPE_MAP` before the
lvalue helper — so the non-addressability guard stays intact for genuine `&m[k]` /
partial writes; only whole-value RMW is newly allowed (legal Go). Missing-key RMW
starts from the zero value (Go: `m[absent]++` → 1) — falls out of the map-get zero
default.

## 2. Grammar fixes (tripwire-gated: 82 S/R + 256 R/R, goo-grammar skill)

Both run `./scripts/grammar-tripwire.sh` before AND after; any delta → STOP, justified-
delta procedure (`-Wcounterexamples`, classify, prove, or revert). Quarantined LAST so
a BLOCK doesn't strand the codegen fixes; if either can't be done at zero delta without
a justified case, it ships without that item rather than raising the baseline for a
papercut.

### #1 — if-init guard
`simple_stmt` exists; `for` already parses `FOR simple_stmt SEMICOLON expression
SEMICOLON simple_stmt block` (parser.y:1220). Add `IF simple_stmt SEMICOLON expression
block` (+ the two else variants) beside `IF expression block` (parser.y:1147). PREFER
a desugar to `{ init; if cond {...} }` if it avoids an AST/codegen change (grammar-only
is lowest-risk); otherwise add an optional init field to the if-node. RISK: the S/R
between `IF expression block` and `IF simple_stmt SEMICOLON …` (an expression is also a
simple_stmt) — disambiguated on the SEMICOLON; the LBRACE_BODY cond-body bridge already
governs `if X {`. `-Wcounterexamples` front-loaded. SCOPING: the init var must not be
visible after the if (Go scoping) — the desugar's wrapping block gives this for free.

### #4 — elided inner literals in map values
The map-entry value doesn't accept a brace-elided composite (`{1,2}` with element type
inferred from V). Extend the map-value production to admit the elided `{...}` form,
inferring type from the map's value type V — threading the existing elided-composite
machinery (parser.y:45-52, `composite_value`) into the map-value position. Lower risk
than #1 (extending an existing elided path, not a new statement shape). Nested
(`map[string]map[string]int{"a": {"x":1}}`) and struct-value (`map[string]P{"a":
{X:1}}`) must also work.

## Testing (all goldens differential vs `go run`)

- #6: composite-literal form (`return Cat{},Cat{}`), variable form (`return c,c`),
  value AND pointer concrete, mixed `(int, Sayer)` return (raw field still passes),
  dispatch through the returned interfaces.
- #2: `m[k]++`, `m[k] += n`, `m[k] -= n`, missing-key `m[absent]++`→1, a
  `freq[k]++` counting loop; PIN that `&m[k]` and `m[k].F = v` still REJECT.
- #1: `if v, ok := m[k]; ok {}`, `if _, ok := ...`, `if x := f(); x > 0 {}`, else
  forms, and init-var-out-of-scope-after pin.
- #4: `map[string][]int{"a": {1,2}}`, nested elided, struct-value elided.
- Reject/regression probes (Makefile): map non-addressability guards intact (#2);
  if-init scoping.

Gates: golden grows from 237/0; `make test` 76/1; `make verify` + `ccomp-link` green;
bison tripwire 82/256 (hard gate on #1/#4, no-op sanity on #6/#2). Controller probes
between tasks DELIBERATELY use lvalue + degenerate shapes (the #114/#115 lesson), not
just the happy rvalue path.

## Tasks (risk-ascending, grammar last)

- T1: #6 multi-return interface box (codegen; correctness fix first).
- T2: #2 map compound writes (codegen; RMW desugar; guards preserved).
- T3: #1 if-init guard (grammar; tripwire; desugar-preferred).
- T4: #4 elided inner literals (grammar; tripwire).
- T5: sweep + handoff.

SDD economy mode; fresh-context whole-branch review before merge.
