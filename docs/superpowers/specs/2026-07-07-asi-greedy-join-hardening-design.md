# ASI Hardening — the four greedy-join silent miscompiles

**Date:** 2026-07-07
**Status:** Design approved; implementation pending
**Track:** v1 correctness floor (roadmap tasks P3-3 / P3-4 / P3-10 in `docs/2026-06-30-v1-roadmap.md`; called "Phase 4 ASI hardening" in the older `2026-06-29` doc — phase numbering drifted between the two, the *work* is the same)

## Problem

Goo's automatic semicolon insertion (ASI) is **narrow by design**: the lexer only
inserts a `;` at a newline when the *next* line begins with a binary-continuation
operator (`* + - / % & | ^`), to disambiguate cases like `p := &x` ⏎ `*p = v`
(otherwise parsed as the multiplication `&x * p`). For everything else it relies on
the grammar tolerating missing semicolons between newline-separated statements.

That tolerance fails — silently — in four cases, where a value-ending token on line 1
is greedily **joined** with the start of line 2. All four are confirmed
accept-then-miscompile (the worst defect class) against the current `bin/goo`:

| Source (two lines)      | Parsed as     | Prints | Go semantics                    |
|-------------------------|---------------|--------|---------------------------------|
| `g := id` ⏎ `(5)`       | `g := id(5)`  | `5`    | `g` = the func value; `(5)` sep |
| `b := a` ⏎ `[0]`        | `b := a[0]`   | `10`   | `b` = the slice; `[0]` separate |
| `q := p` ⏎ `.x`         | `q := p.x`    | `7`    | `q` = the struct; `.x` separate |
| `return` ⏎ `r + 1`      | `return r+1`  | `100`  | bare return → `99`              |

In Go, ASI inserts a `;` after the value-ending token in each case, so the second
line is a separate statement (or a clean syntax error). Goo emits no semicolon and
the parser joins the two lines, changing the program's meaning with no diagnostic.

## Goals

- Eliminate the four greedy-join silent miscompiles: after the fix each program
  either produces Go-correct results or fails with a clean parse/type error — never a
  silent join.
- Preserve every legitimate multi-line form that compiles today (no over-rejection).
- Keep the change confined to the lexer; no grammar/parser/AST/codegen changes, and no
  movement in the bison conflict baseline.

## Non-goals

- **Full Go ASI parity** (insert `;` after *every* value-ending token at end-of-line,
  unconditionally). More faithful but far larger blast radius: it breaks Goo's
  intentional `}` ⏎ `else` laxness and risks regressions in multi-line composite
  literals and call-argument lists. Explicitly rejected in favor of the targeted fix.
- **Grammar-level disambiguation** (precedence/lookahead in `parser.y`). Fights LALR,
  perturbs the conflict baseline (stop-the-line per the goo-grammar skill), highest
  risk. Rejected.
- Labeled `break`/`continue` semantics, method-chain reflow, or any formatting concern.

## Approach — targeted extension of the existing conditional ASI

All changes live in `src/lexer/lexer.c`: `token_ends_value` and the `'\n'` case of
`lexer_next_token`. Three additive parts.

### Part 1 — keyword terminators (unconditional)

When the previous token is `return`, `break`, `continue`, or `fallthrough` and the
lexer reaches a newline, emit a `;` **regardless of what starts the next line**. Go
always terminates after these keywords; their operand (a return expression, a
`break`/`continue` label) must sit on the same line, so a newline is unambiguously a
statement boundary. This is the only part that fires unconditionally, because the
`return` ⏎ `expr` miscompile has an *identifier* (not an operator/bracket) at the
start of line 2 and so is not reachable by the next-line-start guard below.

### Part 2 — greedy-join guard (conditional on next-line start)

Extend the existing next-line-start check. Today it inserts `;` when
`token_ends_value(prev)` **and** the next non-space char starts a continuation
operator. Add three characters to that trigger set: `(`, `[`, `.`. So a value-ending
token followed across a newline by `(`, `[`, or `.` terminates the statement,
fixing the call/index/selector joins.

`{` is deliberately **excluded**: `if cond` ⏎ `{` would otherwise be split from its
block. Only `(`, `[`, `.` are added.

This guard is safe because it is **asymmetric**. A legitimate multi-line continuation
places the operator at the *end* of line 1 (`foo().` ⏎ `bar()`, `a +` ⏎ `b`), so the
token immediately before the newline is the operator/dot — not value-ending — and the
guard does not fire. gofmt never begins a line with `(`/`[`/`.` after a value, so no
valid-Go input is broken.

### Part 3 — value-ending additions

Add `++` and `--` (`TOKEN_INCR`, `TOKEN_DECR`) to `token_ends_value`, so `x++` ⏎
`(f())` and similar also terminate. (Identifiers, literals, `)`, `]`, `}` are already
value-ending.)

## Testing (TDD)

The four probes fail today — they are the failing tests written first.

**Correctness (post-fix expected results):**
- `return` ⏎ `r + 1` with named result `r = 99` → prints **99** (bare return).
- `g := id` ⏎ `(5)` → two statements: `g` is the function value; the fixture asserts
  the un-joined behavior (e.g. `g(5)` on a later line yields the call result, proving
  `g` was not already invoked), or a clean parse error if `(5)` is not a valid
  statement.
- `b := a` ⏎ `[0]` → `b` is the slice (un-joined), asserted analogously.
- `q := p` ⏎ `.x` → `q` is the struct (un-joined), asserted analogously.

**Over-rejection guards (must still compile + run unchanged):**
- gofmt dot-at-end fluent chain (`B{v:0}.` ⏎ `Add().` ⏎ `Add()` → 2).
- multi-line call-argument list (`f(` ⏎ `a,` ⏎ `b,` ⏎ `)`).
- multi-line composite literal (`[]int{` ⏎ `1, 2,` ⏎ `}`).
- operator-at-end continuation (`a +` ⏎ `b`).
- the intentionally-lax `}` ⏎ `else`.

**Harness:** a new `asi-hardening-probe` Makefile target (modeled on the existing
`*-probe` targets), added to the `verify:` prerequisite list. Per case, the plan picks
the fitting form: a **run-and-diff golden** where line 2 forms a valid statement
post-fix (the `return` case is the headline golden — prints `99` not `100`), or a
**reject-probe** where it does not (asserting `rc != 0` with a clean parse/type error
and *no* `Module verification failed` / `LLVM ERROR`, i.e. caught at the front end, not
a verifier crash). Gate is `make verify` (all green) plus `scripts/grammar-tripwire.sh`
**unchanged before and after** (this is a lexer-only change; any conflict-count delta
is stop-the-line per the goo-grammar skill).

## Risks

Low. The single watch-item is over-rejection of a legitimate multi-line form; the
guard suite above is the mitigation. The `.` trigger is the most debated character but
is proven safe by the dot-at-end probe (that form ends line 1 with `.`, which is not
value-ending, so it is never touched). `{` exclusion prevents the obvious
control-flow-block regression.

## Files touched

- `src/lexer/lexer.c` — `token_ends_value` (Part 3), `'\n'` handler (Parts 1 & 2).
- `Makefile` — new `asi-hardening-probe` target + `verify:` wiring.
- `examples/` — probe fixtures (`.goo` + `.expected.txt`) for the correctness and
  over-rejection cases.
