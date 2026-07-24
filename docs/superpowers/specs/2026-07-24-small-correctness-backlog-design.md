# Small-correctness-backlog arc: five ledgered fixes, three PRs — design

Date: 2026-07-24. Status: approved (user, this date). Successor to the
nil-semantics arc (ADR 0001, PRs #214/#216) and lanes M2-B1 (PR #215).
Source of scope: `.handoff.md` "Next-arc candidates" item 2 — the five
small ledgered correctness items, taken whole (user-approved over the
codegen-pair-only and skip-grammar subsets).

## Goals

1. Clear the entire five-item small-correctness ledger in one arc:
   empty `select` `default:`/case body no-parse, slice-index-as-method-arg
   LLVM verify failure, `&*p` fold-to-`p`, builtin-shadowing lockstep
   gate, and the sync/time own-import seeding asymmetry.
2. Every fix lands fixture-first (TDD) and is exercised by
   `make verify-core` — structurally where possible (rewiring existing
   probes so the fix is hit on every run), not just by a one-off fixture.
3. Grammar risk stays isolated: exactly one PR touches `parser.y`, under
   the goo-grammar skill and the 31 S/R / 0 R/R tripwire.

## Scope decisions (user-approved, 2026-07-24)

| Decision | Choice | Rejected alternatives |
|---|---|---|
| Arc scope | All five ledgered items | Codegen soundness pair only (leaves three items ledgered); all-but-grammar (grammar item would need its own micro-arc anyway) |
| PR split | Three PRs by compiler layer: grammar / checker / codegen | One PR per item (5× merge+verify overhead); single arc PR (mixes grammar/checker/codegen in one review and one revert unit) |
| Sequencing | PR 1 → PR 2 → PR 3, each merged on green before the next starts | Parallel branches (conflicts in conformance doc, probe scripts, handoff ledger) |
| Execution model | Same as arcs 17–19: spec → plan → subagent execution → main-loop review → merge on green | Direct main-loop implementation (no independent execution/review split) |

## PR 1 — grammar: empty statement lists in `select` bodies

Discovered M2-B1 T4/T6: Goo's select-case grammar requires a non-empty
statement list — a bare `default:` (and an empty `case` arm) fails to
parse where Go accepts both. Every far send-pump's quit-drain currently
works around it with an `_ = 0` discard (`goostd/lanes/lanes.go`).

- **Fix:** allow an empty statement list in select-case/default bodies in
  `src/parser/parser.y` — epsilon production or optional-list, whichever
  the goo-grammar skill's conflict ledger favors. Both `default:` and
  `case` arms, not just `default:` (Go accepts both).
- **Process (mandatory):** goo-grammar skill; `./scripts/grammar-tripwire.sh`
  PASS before and after; any conflict delta is stop-the-line and goes
  through the justified-delta ledger procedure.
- **Fixtures first:** golden accept-fixtures for a bare `default:` and an
  empty `case` arm inside `select`, with runtime behavior asserted (the
  empty arm is selected and falls through).
- **Structural permanence:** remove the `_ = 0` workaround discards in
  `goostd/lanes/lanes.go` — every far probe in verify-core then exercises
  the new production on every run (same permanence pattern as M2-B1's
  seeding fix).
- **Docs:** add the conformance row to `docs/GO_SPEC_CONFORMANCE.md`
  (already flagged there as a candidate in the handoff).

## PR 2 — type checker: builtin shadowing + sync/time own-import seeding

### Item A — builtin-shadowing lockstep gate

Builtin call dispatch fires on identifier TEXT before scope lookup, so a
user-shadowed `len`/`cap`/`append`/`copy`/`delete`/`close` (plus
`clear`/`min`/`max` if present as dispatch names) still dispatches to the
builtin. Go: predeclared identifiers are shadowable.

- **Fix:** gate builtin dispatch behind the existing
  `name_is_user_shadowed` (`src/types/expression_checker.c:2522`) — the
  pattern `string()`/conversions already use at two call sites. Lockstep:
  one shared gate at the dispatch choke point, not per-builtin copies.
  Codegen must follow the checker's resolution (a shadowed name lowers as
  the user symbol, never the builtin intrinsic).
- **Fixtures first:** golden tests where a local variable and a local
  function shadow `len` (user symbol must win); a regression fixture
  proving unshadowed builtins still dispatch; a reject fixture if the
  shadowed use is ill-typed for the user symbol.

### Item B — sync/time own-import seeding asymmetry

`seed_package_own_shim_imports` (`src/types/type_checker.c:859`) seeds a
vendored package's own plain-shim imports (`fmt`/`os`/`math`/`errors`/
`far`) into its own pushed scope, but deliberately excludes `sync`/`time`
— their bespoke struct/method export builders
(`seed_sync_package_exports`/`seed_time_package_exports`,
`src/compiler/goo.c:689/834`) only run on the main-import path today. A
vendored package importing `sync` or `time` therefore doesn't resolve.

- **Fix:** make the two bespoke builders callable from
  `seed_package_own_shim_imports` (move or expose them across the
  compiler/type-checker boundary; exact placement decided at plan time
  following existing header conventions) so a vendored package's own
  `import "sync"`/`import "time"` seeds into that package's own scope
  before its decls are checked.
- **Fixtures first (no-masking discipline):** a vendored test package
  using `sync.Mutex` (and a `time` counterpart) whose probe `main`
  deliberately does NOT import `sync`/`time` itself — so resolution
  cannot leak from main's imports, the exact masking failure the M2-B1
  spike documented.

## PR 3 — codegen: slice-index method arg + `&*p` fold

### Item A — slice-index expression as a method-call argument (miscompile)

`ctx.AllReduceSum(contrib[ctx.ID()])` fails LLVM module verification —
"Call parameter type does not match function signature!" — because the
GEP pointer isn't loaded before the call. Any method call (value or
pointer receiver) with a slice-index argument reproduces; a plain
function call with the same argument shape is unaffected.

- **Fix:** systematic-debugging first — diff the method-call
  argument-emission path against the plain-call path and land the missing
  rvalue load at the shared choke point (the arc-16 index-lvalue-widen
  fix is the adjacent precedent).
- **Fixtures first:** golden runtime tests for value-receiver and
  pointer-receiver methods taking a slice-index argument directly.
- **Structural permanence:** remove the bind-to-local workarounds in
  `examples/lanes_allreduce_probe.goo`, `far_collective_probe.goo`,
  `far_jacobi_probe.goo` so verify-core exercises the fix on every run.

### Item B — `&*p` folds to `p` (no nil check)

Go folds `&*p` to `p`: no nil-deref panic at the `&*` site even when `p`
is nil; the panic happens only when the resulting pointer is actually
dereferenced. The nil-semantics arc's inline checks over-fire here
(documented as a carried Minor in ADR 0001's final review).

- **Fix:** at the address-of emission site — when the operand of `&` is a
  star-deref, return the pointer operand directly and skip
  `codegen_emit_nil_check`. Emission-time fold, NOT an AST rewrite: the
  nil check is not lost, it relocates to wherever the resulting pointer
  is later dereferenced, exactly Go's semantics.
- **Fixtures first:** extend `scripts/nil_deref_probe.sh`: `q := &*p` on
  nil `p` succeeds (exit 0 path); a subsequent `*q` still panics with
  exit 2. Existing 11 cases must stay green.
- **Perf guard:** `make lanes-kernel-ir-pin` stays PASS (the fold can
  only remove checks, but the gate is cheap and already wired).

## Gates (every PR)

- Fixture first, red before green (TDD).
- `make lexer && make test && make verify-core` before PR; post-merge
  verify-core on main before the next PR starts.
- PR 1 additionally: grammar tripwire PASS before/after, goo-grammar
  skill engaged for any `parser.y` edit.
- Ledger updates ride each PR: `.handoff.md` burndown row per item;
  `docs/GO_SPEC_CONFORMANCE.md` rows where relevant (select empty body;
  builtin shadowability).

## Execution amendments (plan-time discoveries, 2026-07-24)

Three facts found while grounding the plan revise the design above; the
plan (`docs/superpowers/plans/2026-07-24-small-correctness-backlog.md`)
incorporates them:

1. **PR 1 scope widens to switch and type-switch.** `switch { case x: }`
   with an empty body also fails to parse — `select_case`, `case_clause`,
   and `type_case_clause` all route their bodies through the same
   epsilon-free `statement_list` (verified empirically with `bin/goo`).
   Same root cause, same fix: a scoped `case_body` nonterminal
   (`statement_list | %empty`), keeping `statement_list` itself
   epsilon-free (it is shared by if/for/block — grammar-wide blast
   radius). Conformance rows cover both families.
2. **The shadowing gate must land in codegen too.** `call_codegen.c`
   re-dispatches builtins on raw identifier text independently of the
   checker (8 strcmp arms at lines 814–1138); a checker-only gate would
   leave codegen lowering a shadowed `len` as the builtin. The fix
   mirrors the existing two-layer pattern `string()`/conversions already
   use (checker `name_is_user_shadowed` + codegen
   `type_checker_lookup_variable` guards).
3. **Only `far_collective_probe.goo` carries bind-to-local workarounds**
   (two sites). `lanes_allreduce_probe.goo` and `far_jacobi_probe.goo`
   pass plain scalar accumulators by natural shape and need no change —
   the design's "three files" was over-counted.

## Non-goals

- Diagnostics-quality family (positions one statement late,
  folded-value-not-source-text) — next arc candidate, untouched here.
- Dup-case float-literal detection, double-diagnostic on type-switch
  vtable failure — ledgered, out of scope.
- Message-wording divergences left documented by ADR 0001 (nil-slice
  index message, nil func-value call message) — behavior already matches
  Go; out of scope.
- The pre-existing `ValueInfo* ptr` leak at the star-write lvalue arm —
  noted for eventual cleanup, not this arc.
