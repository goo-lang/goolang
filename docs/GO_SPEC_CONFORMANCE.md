# Go Spec Conformance Matrix

**Snapshot:** 2026-07-11, v1.0.0 (`c0b6806`) — **79% conformance**
(38 works / 48 tested constructs, excluding 2 deliberate rejections).

**Method.** One tiny fixture per Go-spec construct under `tests/spec/`,
statuses determined **empirically** (compiled and run against `bin/goo`,
outputs checked against Go semantics), recorded in
`tests/spec/manifest.tsv`, and enforced by `make spec-conformance`
(in `make verify-core`). The runner is a drift gate: a `works` row that
stops working, a `divergent` row that changes behavior, or an
`absent`/`rejected` row that starts compiling **fails the build** until
this matrix is updated. The percentage above is therefore a tracked,
probe-backed figure — regenerate it with `make spec-conformance`; do not
edit it by hand.

**Reading the number honestly.** This measures *construct coverage*, not
"fraction of real Go programs that compile": real code hits stdlib-surface
walls (see `scripts/check_stdlib_coverage.sh` for the audited stdlib) and
the divergences below before it hits spec walls. The suite covers the
constructs ordinary gofmt-formatted Go uses; it does not (yet) test every
sentence of the spec — rows are added as gaps are found, and the
percentage moves only with evidence.

## Status vocabulary

| Status | Meaning |
|---|---|
| **works** | Go-parity behavior, verified output |
| **divergent** | compiles and runs, behavior differs from Go — pinned, listed below |
| **rejected** | deliberately not in v1 (a decision, with a clean diagnostic where noted) |
| **absent** | a gap: construct missing; failure is incidental, not designed |

## Matrix by chapter (from manifest.tsv — regenerate, don't hand-edit)

| Chapter | works | divergent | rejected | absent | Constructs |
|---|---|---|---|---|---|
| Lexical | 4 | – | – | – | ASI/newlines, raw strings, rune literals, hex escapes |
| Declarations | 6 | – | – | – | grouped var, const+iota, short var, named types, tuple assign, blank ident |
| Types | 8 | – | – | 1 | arrays, slices, maps, struct embedding, pointers, closures, interfaces, channels; **absent:** complex |
| Expressions | 6 | – | – | 1 | keyed/elided composites, type assertions, method values, conversions, shift/div/mod, user variadics; **absent:** method expressions |
| Statements | 10 | 1 | – | – | if-init, for (3 forms), range (slices, string→**runes**, channels), switch-init + type switch, fallthrough, labels/goto, defer-in-loop, go+select; **divergent:** named-result defer |
| Builtins | 3 | – | 1 | 2 | len/cap/make/new/append/delete, copy, panic (exit 2); **rejected:** recover (clean v1 diagnostic); **absent:** min/max, clear |
| Generics | 1 | – | – | 3 | `[T any]` inference-only funcs; **absent:** explicit instantiation, union constraints (P2.10), generic types |
| Packages | – | 1 | – | 1 | **divergent (SILENT):** `init()` compiles but never runs; **absent:** shim-package alias imports |
| Errors | 1 | – | – | – | (T, error) ↔ !T bridging, e.Error() |
| System | – | – | 1 | – | **rejected:** unsafe (v1 Non-goal) |

## Known divergences (pinned by fixtures — changing them fails the gate)

1. **`init()` silently never runs** (`pkg_init_func`) — compiles clean,
   initializer skipped (Go: 42, Goo: 1). The *silent* class is the worst
   kind; v1.0.1 candidate fix: clean compile reject (recover-style
   diagnostic) or a real implementation.
2. **Named-result defer mutation** (`div_named_result_defer`) — a deferred
   function's write to a named result is not reflected in the returned
   value (Go: 6, Goo: 5). Documented in `docs/02-LANGUAGE-SPECIFICATION.md`.

## Corrections this matrix has already produced

- **Range over string is RUNE-correct** (`str_range_runes`): yields decoded
  runes with byte-offset indices, exact Go parity. The long-standing
  "string range yields bytes" note (P4-era) was stale — empirical testing
  beats remembered state.
- `copy()` works (an old reject-probe name suggested otherwise); user
  variadic functions (`...int`) work; `const`+`iota` groups work.

## Not measured here

Deliberate v1 rejections are excluded from the denominator (recover,
unsafe — see the roadmap's **v1 Non-goals**). GPU syntax is gated
separately (`gpu-kernel-reject-probe`). The stdlib surface has its own
coverage gate (`check_stdlib_coverage.sh`).
