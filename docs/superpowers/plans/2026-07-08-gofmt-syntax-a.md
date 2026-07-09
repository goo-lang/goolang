# Gofmt-Syntax Sub-Project A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ordinary gofmt-formatted Go syntax parses: multi-method interfaces (the Phase-2 gate), switch-with-init, grouped `var (...)`, call trailing commas, raw strings, and line-starting `<-ch`, plus the positive ASI probe and the LALR strategy record.

**Architecture:** Grammar tasks first in risk order (P1.1 interface fix → P1.4 switch-init → P1.2 var groups → P1.3 trailing commas), then the two lexer-only tasks (P1.8 raw strings, P1.9 receive ASI), then the probe (P1.11) and the ledger record (P1.12, which must state the FINAL baseline). Every parser/lexer touch runs the goo-grammar skill procedure.

**Tech Stack:** C23, bison 3.8 (LALR + lexer feedback), bash probes, `go run` for golden expected outputs.

**Spec:** `docs/superpowers/specs/2026-07-08-gofmt-syntax-a-design.md` — normative; read first. The goo-grammar skill (`.claude/skills/goo-grammar/`) and its `references/workarounds.md` (§ numbers below) are mandatory reading for Tasks 1–4 and 6.

## Global Constraints

- Tripwire brackets EVERY task: `./scripts/grammar-tripwire.sh` PASS (82 S/R + 256 R/R exact) before AND after. Any delta = stop-the-line → justified-delta procedure (counterexample classification, differential goldens, same-commit `EXPECTED_*` + ledger-history update). Never rationalize a +1 inline.
- FROZEN: everything that parses at base keeps its meaning; everything rejected stays rejected unless the task's acceptance names it. Gates before every commit: `make test-golden` (325/0 at start, grows), `make test-golden-reject` (7/0, grows), `make comptime-value-reject-matrix` (18/18), `make comptime-generic-compose-ir-pin` (PASS), `make test` (76/1-skip), `make spmd-bench-probe` (PASS), `make asi-hardening-probe` (PASS). `make verify-core` at arc end and after any justified delta.
- Accept-goldens: expected output produced by `go run` on the equivalent Go program; state that program in the fixture header (println-shim substitutions noted). Reject fixtures: `tests/golden/reject/` convention (single-line `.err.txt`).
- Header edits: tail-append only; `make clean` after any header change (§8); new parser-allocated fields initialized at every malloc site.
- Commits: conventional prefixes, atomic, `--no-gpg-sign`, imperative.

## File structure

- `src/parser/parser.y` — Tasks 1 (interface_method_list ~:2219-2290), 2 (switch arms ~:1506-1548, IF model :1270), 3 (VAR group beside CONST group :956-:962 + desugar), 4 (call_expr arms :1911-:2000)
- `src/parser/lexer_bridge.c` / `src/lexer/lexer.c` — Task 1 if the fix is interface-body-scoped ASI (struct-body precedent, lexer.c:158 region §4); Tasks 5-6 lexer.c only
- `.claude/skills/goo-grammar/references/conflict-ledger.md` — Tasks 1-4 (any deltas), Task 8
- `Makefile` — Task 7 (`asi-gocompat-probe`); `examples/` + `tests/golden/reject/` — Tasks 1-6

---

### Task 1: Interface method-list fix (P1.1 — gates Phase 2)

**Files:** `src/parser/parser.y` (interface_method / interface_method_list, :2219-2290); possibly `src/lexer/lexer.c` (:158-region struct-body ASI) + `src/parser/lexer_bridge.c` if the fix is body-scoped ASI. Fixtures: `examples/iface_multimethod_probe.goo` (+expected), reject fixture only if a new loud rejection appears.

- [ ] **Step 1: Failing tests.** (a) `type I interface { Inc()\n Get() int }` with a struct implementing both, called through the interface — TODAY: parse error at the second method. (b) `{ Inc()\n Dec() }` — same. (c) Regression baselines that MUST keep passing: single-method interfaces, `{ Get() int\n Set(n int) }` ordering, embedded interfaces (`interface { Reader; ... }` — iface-parse-probe / embed-* probes pin these). Record exact diagnostics.
- [ ] **Step 2: Diagnose before editing (spec open point 1).** Run `bison -Wcounterexamples` reasoning on the hypothesis: `interface_method: identifier LPAREN RPAREN` vs `... func_result` — after `Inc ( )`, a following identifier is absorbed as func_result (§6 newline-blind hazard), so `Inc()\nGet() int` parses as `Inc() Get` + garbage. Confirm/refute with a probe: `{ Inc() Get() int }` on ONE line should fail identically if the hypothesis holds. Report the mechanism.
- [ ] **Step 3: Fix.** Preferred shape if Step 2 confirms: interface-body-scoped ASI mirroring the struct-body pilot (§4, lexer.c:158 region — the lexer/bridge already tracks struct bodies; extend the same tracking to interface bodies) + SEMICOLON-tolerant arms in `interface_method_list` (`interface_method_list SEMICOLON interface_method` and trailing `SEMICOLON` acceptance), so a newline between method specs terminates the spec. If Step 2 refutes, follow the actual mechanism — but NO new terminal and no touching `func_result` itself (§6: its first set must not change).
- [ ] **Step 4: Tripwire + differential.** Tripwire exact or ledgered delta. Diff the parse of every base-accepted interface form (Step 1c list) — behavior identical. New goldens: multi-method interface with a void method first, implemented and dispatched dynamically (expected output via `go run`).
- [ ] **Step 5: Gates + commit.** `git commit --no-gpg-sign -m "fix(parser): interface method specs terminate at newlines like struct fields"`.

### Task 2: switch-with-init (P1.4)

**Files:** `src/parser/parser.y` (:1506-1548 switch arms; IF init model at :1270). Fixtures: `examples/switch_init_probe.goo` (+expected), `tests/golden/reject/switch_init_scope.goo` (+.err.txt).

- [ ] **Step 1: Failing tests.** `switch x := 2; x { case 2: println(1) }` — TODAY parse error at ~4:17. Type-switch-with-init form: FIRST check what type-switch binding exists at base (spec open point 3): compile `switch v := a.(type)` without init — scope the init mirror to exactly the base surface.
- [ ] **Step 2: Implement.** Mirror the IF pattern: `SWITCH simple_stmt SEMICOLON expression LBRACE_BODY ...` (and the type-switch variant if base has it). The init statement's scope = the switch statement (init var visible in all cases, NOT after) — reuse the IF-init scoping mechanism in the checker if it needs a node change (tail-append, malloc-site audit).
- [ ] **Step 3: Tripwire + fixtures.** Tripwire exact or ledgered (the IF-init precedent suggests a clean mirror is possible). Golden: init + tag switch selecting middle case + default fallthrough-absence, `go run`-derived output. Reject fixture: using the init var after the switch → `Undefined variable` (or checker's actual wording).
- [ ] **Step 4: Gates + commit.** `git commit --no-gpg-sign -m "feat(parser): switch statements accept an init statement like if"`.

### Task 3: Grouped var declarations (P1.2)

**Files:** `src/parser/parser.y` (VAR group beside CONST group :956-962; desugar model `desugar_const_group` :3516). Fixtures: `examples/var_group_probe.goo` (+expected).

- [ ] **Step 1: Failing test.** File-scope and in-function `var (\n g1 = 10\n g2 = 20\n z int\n)`; `println(g1+g2)` and `println(z)` — TODAY parse error even with explicit semicolons (record).
- [ ] **Step 2: Check the CONST group's separator mechanism (spec open point 5)** — per-line ASI or explicit SEMICOLONs in `const_spec_list`? Mirror exactly that for `var_spec_list` + a `desugar_var_group` following `desugar_const_group`'s shape (bare `z int` → zero-value var decl, values==NULL).
- [ ] **Step 3: Tripwire + golden.** File scope uses constant initializers only (P3.7 owns non-constant globals — put a comment in the fixture); in-function group exercises all three spec forms. `go run` output (30, 0).
- [ ] **Step 4: Gates + commit.** `git commit --no-gpg-sign -m "feat(parser): grouped var ( ... ) declarations"`.

### Task 4: Trailing comma in call arguments (P1.3)

**Files:** `src/parser/parser.y` (call arms :1927, :2000, method-call arm :3291, DEREF arm :2855 — extend the shared list or each arm, per what Step 2 finds). Fixtures: `examples/call_trailing_comma_probe.goo` (+expected).

- [ ] **Step 1: Failing test.** `add(\n4,\n5,\n)` multi-line (gofmt's canonical shape) — TODAY parse error (record). Also confirm `{,}` fixture and spread probe still pass at base.
- [ ] **Step 2: Map the arg-list productions (spec open point 2).** Do the call arms share `expression_list`? Note `expression_list` is used in non-call contexts too (returns, assignments) — adding COMMA-tolerance to `expression_list` itself would leak trailing commas everywhere (NOT acceptance). Add `COMMA RPAREN` variants to the CALL arms only (the §5 COMMA-before-RBRACE analog): `primary_expr LPAREN expression_list COMMA RPAREN` etc. Spread arm (:1954) untouched — `f(xs...,)` stays a parse error. `type_call_arg` arm (:2000): extend or exclude deliberately; state which.
- [ ] **Step 3: Tripwire + goldens.** LR(1) argument mirrors §5 (after `list COMMA`, RPAREN reduces, an expression token shifts) — expect zero delta; ledger if not. Golden: multi-line call + single-line `f(1, 2,)` + method call with trailing comma; `go run` output. Reject check: `f(,)` and `f(1,,)` stay parse errors (add to the golden's header commentary or as a reject fixture — implementer's call, report which).
- [ ] **Step 4: Gates + commit.** `git commit --no-gpg-sign -m "feat(parser): trailing comma in call argument lists"`.

### Task 5: Raw string literals (P1.8 — lexer only)

**Files:** `src/lexer/lexer.c` (string scanning; backtick currently → TOKEN_UNKNOWN, rejected since Phase 0). Fixtures: `examples/rawstring_probe.goo` (+expected), `tests/golden/reject/rawstring_unterminated.goo` (+.err.txt).

- [ ] **Step 1: Failing test.** ``s := `raw \n string` `` — TODAY: loud `unknown token` rejection (P0.4). Record.
- [ ] **Step 2: Implement.** Backtick scan: consume to closing backtick; content literal (no escapes — `\n` stays backslash+n); CR (0x0D) bytes stripped per Go spec; may span newlines (interior newlines are content, NOT ASI triggers — the scan happens inside the token so the ASI path never sees them; verify prev_token_type is STRING after). Emit TOKEN_STRING with byte length. Unterminated (EOF before closing backtick) → TOKEN_ERROR with the OPENING backtick's position.
- [ ] **Step 3: Tripwire (trivially exact — no grammar change) + fixtures.** Goldens: single-line, multi-line, backslash-heavy — expected output via `go run` (raw strings are identical in Go). Reject fixture: unterminated → `.err.txt` substring from your actual diagnostic. ALSO: a raw string containing `#` and backtick-adjacent code after the literal (proves the scanner exits correctly).
- [ ] **Step 4: Gates + commit.** `git commit --no-gpg-sign -m "feat(lexer): raw string literals (backticks) per the Go spec"`.

### Task 6: ASI for line-starting receive (P1.9 — lexer only)

**Files:** `src/lexer/lexer.c` (`char_starts_continuation_op` :112-115 and the newline-ASI decision that consults it). Fixtures: `examples/asi_recv_probe.goo` (+expected).

- [ ] **Step 1: Failing test.** `x := 1` ⏎ `<-ch` (buffered ch pre-loaded; result discarded via `_ = <-ch`? NO — bare `<-ch;` discard has a known diagnostic gap; use `v := <-ch` — wait, that's not line-starting `<-`. The failing shape per the audit: a WAIT statement `<-ch` alone on a line after a value-ending token). TODAY: joins into `1 <- ch`-ish send and rejects "Cannot send to non-channel type int64". Record. NOTE: if bare `<-ch` as a statement is ALSO rejected by the checker independently (bare-discard diagnostic), the golden must use the form Go accepts and Goo can run: `<-done` as a blocking wait on a channel that a goroutine closes... no close() — a goroutine that SENDS. `go func(){ ch <- 1 }()` ⏎ `x := 2` ⏎ `<-ch` ⏎ `println(x)`.
- [ ] **Step 2: Implement.** `<` is currently NOT in the continuation set (verify — spec open point 4). The hazard is the reverse: today's newline handling doesn't break the line before `<-`. Add the peek-guarded rule: at a newline, if the previous token is value-ending AND the next non-space chars are `<` followed by `-`, insert the statement break (mirror the existing value-ending+continuation logic, inverted). Plain `<` and `<=`/`<<` at line start keep today's behavior — write /tmp probes for `x := 1` ⏎ `< 2` (whatever today does, unchanged) and confirm asi-hardening-probe green.
- [ ] **Step 3: Tripwire (exact) + golden.** The Step 1 program as `examples/asi_recv_probe.goo`, `go run`-derived output. 5 runs (it has a goroutine — ensure deterministic join: the receive IS the join).
- [ ] **Step 4: Gates + commit.** `git commit --no-gpg-sign -m "fix(lexer): ASI terminates the line before a line-starting channel receive"`.

### Task 7: Positive ASI regression probe (P1.11)

**Files:** `Makefile` (`asi-gocompat-probe` next to `asi-hardening-probe`, into `verify`/VERIFY list + `.PHONY`).

- [ ] **Step 1: Write the probe.** One inline program (heredoc/printf to build/) exercising the verified-good matrix with ASSERTED stdout: no-semicolon statements; bare return/break/continue (in context); `*p = v` after `p := &x`; trailing-op continuation (`a := 1 +` ⏎ `2`); dot continuation (`fmt.` ⏎ `Println`? — only if that's actually supported; verify and include only base-verified-good forms); struct embedding on its own line; receive-at-line-start (Task 6's behavior); comment+ASI (`x := 10 // c` ⏎ `*p = 7`). Compile + run + diff expected; unpiped rc per line.
- [ ] **Step 2: Self-test the probe.** Invert one expected value → probe must FAIL; restore.
- [ ] **Step 3: Wire + gates.** Into VERIFY_ALL_DEPS (Task 7 of Phase 0's variable — appended, order at the end near asi-hardening-probe's position is fine but note it), `.PHONY`. `make asi-gocompat-probe` PASS; `make verify-core` PASS.
- [ ] **Step 4: Commit.** `git commit --no-gpg-sign -m "test(asi): positive Go-compat ASI matrix probe"`.

### Task 8: LALR decision record (P1.12 — docs only)

**Files:** `.claude/skills/goo-grammar/references/conflict-ledger.md`.

- [ ] **Step 1: Write the section.** "v1 parser strategy: LALR(1) + lexer feedback (decision record, 2026-07-08)": the two conflict families and their neutralization (targeted ASI, LBRACE_BODY bridge); the four historically-blamed constructs verified working (cite the runtime evidence from docs/2026-07-08-v1-roadmap.md's verdicts); GLR rejected (ambiguity surfaces at runtime, loses tripwire exactness); recursive-descent rewrite rejected (cost/risk vs a working 3.7k-line grammar mid-v1). State the FINAL baseline after Tasks 1-4 (82/256 or the ledgered successor) and this arc's deltas if any.
- [ ] **Step 2: Tripwire (trivially PASS) + commit.** `git commit --no-gpg-sign -m "docs(grammar): decision record — stay LALR(1) + lexer feedback for v1"`.

---

## Self-review

- Spec coverage: P1.1→T1, P1.4→T2, P1.2→T3, P1.3→T4, P1.8→T5, P1.9→T6, P1.11→T7, P1.12→T8; spec open points 1-5 embedded in T1.2, T4.2, T2.1, T6.2, T3.2 respectively. Frozen-behavior carve-outs named per task ({,}, spread, func_result first set).
- Counts stated as expectations with report-actual hedges; fixture names are suggestions, conventions are the contract.
- Task 6 Step 1's program-shape reasoning is deliberately in the plan (the naive reproducer trips the bare-discard gap) — the implementer gets the working shape, not a trap.
