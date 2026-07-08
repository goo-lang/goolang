# Gofmt-syntax completeness, sub-project A (design)

**Date:** 2026-07-08
**Source of requirements:** `docs/2026-07-08-v1-roadmap.md`, Phase 1 items P1.1–P1.4,
P1.8–P1.9, P1.11–P1.12 (acceptance criteria reproduced verbatim-in-substance here; this
spec is normative where wording differs). Sub-project B (labels/goto/fallthrough/select
value-binding — P1.5–P1.7, P1.10) is explicitly NOT this arc.
**Branch:** `feat/go-syntax-grammar-a`, base `614c01e` (main after PR #163).
**Baseline gates at base:** golden 325/0 · reject 7/0 · matrix 18/18 · ir-pin PASS ·
unit 76/1-skip · tripwire 82 S/R + 256 R/R exact · spmd-bench-probe PASS · stripped-PATH
`verify-core` exit 0.
**Mandatory companion:** the goo-grammar skill (`.claude/skills/goo-grammar/`) governs
every parser.y/lexer touch in this arc — tripwire before AND after each task; any
conflict-count delta is stop-the-line and takes the justified-delta procedure
(counterexample classification, differential golden verification, same-commit
`EXPECTED_SR`/`EXPECTED_RR` + ledger-history update). The workaround map
(`references/workarounds.md`) section numbers cited below are load-bearing reading.

## What this arc means (scope decision)

The compiler is sound (Phase 0) but rejects ordinary gofmt output. This arc makes the
six highest-leverage *syntax* forms parse — prioritizing P1.1, the interface-grammar bug
that gates all of Phase 2's interface-correctness work — while the codegen-bearing
control-flow constructs wait for sub-project B. Grammar changes here are accept-only:
nothing that parses today may change meaning (frozen), and `{,}` stays rejected
everywhere (workarounds §5; the migrated reject fixture pins it).

## The tasks

- **P1.1 — Interface method-list fix (FIRST: gates Phase 2).** Today
  `type I interface { Inc()\n Get() int }` is a parse error at the second method when
  the first is void (no params, no return); single-method interfaces and orderings like
  `Get() int\n Set(n int)` parse. Diagnose the method-spec separator handling against
  the newline-blind grammar (§6) and fix within the interface-body productions.
  Acceptance: both `{ Inc()\n Get() int }` and `{ Inc()\n Dec() }` parse and reach the
  type checker; single-method interfaces unchanged; embedded-interface arms
  (`interface { Reader; ... }`, shipped PR #134) unchanged; the resulting programs
  type-check and RUN (golden with dynamic dispatch through the void method). Highest
  conflict risk of the arc — expect ledger work.
- **P1.4 — switch-with-init.** `switch x := 2; x { case 2: ... }` and the type-switch
  form `switch v := f(); w := v.(type)` compile and run correctly. Mirror the IF init
  arm (parser.y:1270) onto the SWITCH arms (parser.y:1506-1548); the new arms consume
  the LBRACE_BODY bridge token exactly as the existing switch arms do (§1). Init-scoped
  variables visible in all cases; existing `if-init-scope-reject-probe` semantics
  (init var not visible after the statement) replicated for switch as a reject fixture.
- **P1.2 — Grouped `var (...)`.** File-scope and in-function
  `var (\n g1 = 10\n g2 = 20\n z int\n)` compiles; `g1+g2` prints 30; `z` gets its
  zero value. Mirror the existing IMPORT/CONST group productions. Note the roadmap
  caveat: only constant initializers at package scope work today (non-constant globals
  are P3.7) — the golden uses constants at file scope, full forms in-function.
- **P1.3 — Trailing comma in call arguments.** `add(\n4,\n5,\n)` compiles and prints 9
  (gofmt emits this in every multi-line call). Replicate the COMMA-before-RBRACE
  pattern (§5) as a COMMA-before-RPAREN arm on the call argument list — LR(1)-decidable
  by the same argument. The spread arm (§7) is untouched: `f(xs...,)` is OUT of scope
  and stays a parse error; `{,}` composite forms stay rejected (existing fixture).
  Method calls and builtin calls (println, make via type_call_arg §3) get the same
  treatment only if they share the argument-list production — do not fork parallel
  arms (§3's conflict-factory warning).
- **P1.8 — Raw string literals (lexer-only).** Backtick strings per the Go spec:
  content taken literally (backslashes are data), may span lines, carriage returns
  stripped, no escape processing; emitted as the existing TOKEN_STRING with byte
  length (NO new terminal → grammar and tripwire structurally unchanged). Unterminated
  raw string → TOKEN_ERROR with position (rejected loudly via the Phase 0 gate).
  RED evidence is free: since P0.4, a backtick is a loud `unknown token` rejection.
  Goldens: single-line, multi-line, backslash-heavy (`\n` stays two characters —
  assert output against `go run` on the equivalent program). Reject fixture:
  unterminated.
- **P1.9 — ASI: line-starting `<-` (lexer-only).** `x := 1` ⏎ `<-ch` currently joins
  into a send expression and rejects ("Cannot send to non-channel type int64"); Go
  treats the newline as a terminator and `<-ch` as a receive statement. Insert the
  statement break before a line-starting `<-` when the previous token is value-ending;
  the guard fires only on `<` peeking `-` (plain `<` comparisons keep today's
  continuation behavior). Send-on-its-own-line and same-line receives unchanged.
  `make asi-hardening-probe` stays green.
- **P1.11 — Positive ASI regression probe.** A Makefile `asi-gocompat-probe` that
  compiles AND RUNS the verified-good ASI matrix with asserted stdout: no-semicolon
  statements, bare return/break/continue, `*p = v` after `&x`, trailing-op and dot
  continuations, struct embedding on its own line, receive-at-line-start (P1.9's
  behavior), comment+ASI interplay. Wired into `verify` (thus `verify-core`) and
  `.PHONY`. Unpiped rc discipline throughout.
- **P1.12 — Decision record: stay LALR(1) + lexer feedback for v1.** A new section in
  `references/conflict-ledger.md` stating the strategy with evidence: all 338 baseline
  conflicts fall in two understood families neutralized by targeted ASI + the
  LBRACE_BODY bridge; the four historically-blamed constructs verified working at
  runtime; GLR and recursive-descent rejected with rationale (GLR: unbounded
  ambiguity surfacing at runtime, loses the tripwire's exactness; rewrite: risk and
  cost against a working 3.7k-line grammar mid-v1). Pure docs commit; tripwire PASS
  trivially. If this arc's tasks changed the baseline via justified deltas, the
  record states the NEW baseline.

## Global constraints (frozen behavior)

- C23. goo-grammar skill procedure on every parser.y/lexer_bridge.c/lexer token
  change; tripwire exact-or-ledgered per task; conflict deltas never ride along
  unexplained.
- Everything that parses at base keeps its parse (frozen meaning); everything
  rejected at base stays rejected unless a task's acceptance names it. Gates before
  every commit: `make test-golden` (325/0 at start, grows), `make test-golden-reject`
  (7/0, grows), `make comptime-value-reject-matrix` (18/18),
  `make comptime-generic-compose-ir-pin` (PASS), `make test` (76/1-skip),
  `make spmd-bench-probe` (PASS), `make asi-hardening-probe` (PASS). Full
  `make verify-core` at least at arc end and after any justified delta.
- Accept-behavior goldens: expected output produced by `go run` on the equivalent Go
  program (skill rule 6), never hand-written — state the Go program used in the
  fixture header. Where Goo-vs-Go surface differs irreducibly (e.g. `println`
  builtin), keep the computation Go-checkable and note the print-shim substitution.
- Header edits (include/ast.h etc.): tail-append only; `make clean` after any header
  change (§8); new parser-allocated node fields initialized at every malloc site.
- Commits: conventional prefixes, atomic, `--no-gpg-sign`, imperative mood.

## Scope (YAGNI) — explicitly NOT this arc

- Labels, goto, fallthrough, select value-binding (sub-project B).
- `f(xs...,)` (spread + trailing comma); `{,}` anywhere; enum-body ASI (§4's recorded
  gap); single-line grouped imports; anonymous struct literals.
- Non-constant package-scope initializers in var groups (P3.7 owns it).
- Any type-first-set widening (§6 hazard) — none of these tasks adds type tokens.
- Grammar-conflict family cleanup (P5.10) — the ledger record documents, not shrinks,
  the baseline.

## Testing

- TDD per task: RED = the roadmap's exact reproducer rejected/misparsed at base
  (record diagnostics); GREEN = accept goldens with go-run-derived expected output;
  reject fixtures for the new loud failures (unterminated raw string, switch-init
  scope leak).
- Tripwire runs bracket every task; ledger deltas (if any) carry the differential
  golden evidence in the same commit.
- The suite counts only ever grow; report actual counts, never assume.

## Open verification points for the implementer

1. P1.1: the precise failure mechanism — is the method-list separator an ASI gap
   (newline not terminating a void method spec) or a grammar ambiguity between
   `Inc()` and a method named `Inc` with result type on the next line (§6 family)?
   The fix differs; diagnose with bison counterexamples before editing.
2. P1.3: whether method calls/builtins share the call-argument production (extend
   once) or have parallel arms (extend each — and say so); verify `type_call_arg`
   (§3) forms tolerate the trailing comma or are excluded, deliberately.
3. P1.4: whether the type-switch arm's binding (`w := v.(type)`) exists at base or
   only the plain tag form — scope the init-arm mirror to what exists; do not add
   new type-switch surface.
4. P1.9: confirm `<` is absent from `char_starts_continuation_op` today
   (lexer.c:112-115 per the audit) and that adding the peek-guard doesn't disturb
   `<=`/`<<` line-start cases (write probes for both).
5. P1.2: whether the CONST group production carries per-line ASI or explicit
   semicolons — mirror whichever mechanism it actually uses.
