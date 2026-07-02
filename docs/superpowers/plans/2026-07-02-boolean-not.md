# Boolean NOT (`!expr`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `!expr` (Go's boolean NOT) parses and works: `!true` → false, `!ok` in conditions, `!p.A` through pointers; `!<non-bool>` is a clean type error.

**Architecture:** Parser-only change. `!` lexes to `TOKEN_BANG` (bison `BANG`), which today appears in exactly one production: `BANG type` (the `!T` error-union type). The parser also has a dead `NOT unary_expr` production — the lexer never emits `TOKEN_NOT`, so the boolean-NOT typecheck arm (`type_check_unary_expr` TOKEN_NOT case: requires bool) and codegen arm (`LLVMBuildNot`, with PR #91's lvalue auto-load) are dead code from source. Fix: add a `BANG unary_expr` alternative to `unary_expr` whose action builds the UnaryExprNode with **TOKEN_NOT** (via `bison_token_to_token_type(NOT)`), reviving the existing dead arms unchanged. `BANG` already has unary precedence (`%right NOT BIT_NOT BANG`, parser.y:186).

**Tech Stack:** C23, bison. No typecheck, codegen, runtime, or header changes.

**Discovered:** during PR #91 (probe `!p.A` didn't parse). Recorded as gating gap 1c.

## Global Constraints

- Branch: `feat/boolean-not` (already created off main @71eb45b — do NOT commit on main).
- Commits: conventional style, imperative mood, `--no-gpg-sign`. Stage only named files; never stage anything under `.superpowers/`.
- **Bison conflict baseline: 78 shift/reduce + 256 reduce/reduce.** After the grammar change, check the summary in `src/parser/parser.output`. If the count changes, STOP and report the delta with the relevant state(s) before proceeding — the interaction risk is `BANG type` (error-union) vs `BANG unary_expr` becoming viable in the same LALR state. RESOLVED during execution: count is 79 S/R (+1, BANG joins the existing shift-wins family in the func_signature states; see parser.y comment).
- Gate: `make lexer`, run probes, then `eval "$(opam env --switch=default)"` STANDALONE, then `make verify` (all PASS; golden baseline 164/0 grows to 165/0 with the probe) and `make test` (76 pass / 1 pre-existing skip).
- Probes print only same-width values (ints/bools) — known mixed-width Println bug.

## Reference: verified code landmarks (2026-07-02, main @71eb45b)

- Lexer `!` handling: `src/lexer/lexer.c:308-317` — `!=` → TOKEN_NE, else TOKEN_BANG. UNCHANGED by this plan.
- Dead production: `parser.y:1273-1276` — `NOT unary_expr` (keep or remove; see Task 1 Step 3).
- Unary precedence declaration: `parser.y:186` — `%right NOT BIT_NOT BANG` (BANG already listed).
- Error-union type production: `parser.y:2170` — `BANG type` inside `error_union_type`. UNCHANGED.
- The `^x` complement production at `parser.y:1280-1287` is the pattern for a token reused across contexts (`%prec` comment explains the approach).
- Typecheck TOKEN_NOT arm: `src/types/expression_checker.c:724-732` ("Logical not requires boolean type, got %s"). UNCHANGED — this task makes it reachable.
- Codegen TOKEN_NOT arm: `src/codegen/expression_codegen.c` unary switch (`LLVMBuildNot`), operand auto-loaded since PR #91. UNCHANGED.
- Reject-probe Makefile pattern: `addrlit-reject-probe` (search Makefile for it); registration in the `verify:` list before `test-golden`.

---

### Task 1: `BANG unary_expr` production + probes

**Files:**
- Modify: `src/parser/parser.y` (unary_expr rule L1271+; optionally remove the dead NOT production)
- Modify: `Makefile` (new `boolnot-reject-probe` target + registration in `verify:`)
- Test: `examples/boolean_not_probe.goo` + `examples/boolean_not_probe.expected.txt`

**Interfaces:**
- Consumes: existing TOKEN_NOT typecheck/codegen arms; PR #91's unary lvalue auto-load.
- Produces: `!expr` parsing at unary precedence; `!<non-bool>` clean type error.

- [ ] **Step 1: Write the failing probe**

`examples/boolean_not_probe.goo`:
```go
package main

import "fmt"

type Flags struct {
	On bool
}

func main() {
	fmt.Println(!true)
	b := false
	fmt.Println(!b)
	fmt.Println(!!b)
	x := 5
	if !(x > 10) {
		fmt.Println(1)
	}
	p := &Flags{On: false}
	if !p.On {
		fmt.Println(2)
	}
	ok := true
	if !ok {
		fmt.Println(3)
	} else {
		fmt.Println(4)
	}
	fmt.Println(!b == true)
}
```

`examples/boolean_not_probe.expected.txt`:
```
false
true
false
1
2
4
true
```

Coverage: literal, variable, double negation, parenthesized comparison, selector through pointer (exercises the #91 auto-load), if/else both arms, and precedence (`!b == true` must parse as `(!b) == true` → true, i.e. unary binds tighter than `==`).

- [ ] **Step 2: Verify it fails today**

Run: `bin/goo examples/boolean_not_probe.goo 2>&1 | head -3`
Expected: a PARSE error at the first `!` (not a type error).

- [ ] **Step 3: Add the grammar production**

In `parser.y`, in the `unary_expr` rule, REPLACE the dead `NOT unary_expr` alternative (L1273-1276) with:

```
    | BANG unary_expr {
        // Boolean NOT `!x`. The lexer emits BANG for a lone `!` (TOKEN_NOT
        // was never emitted — the old NOT production was dead grammar).
        // BANG doubles as the error-union type marker (`!T`, the BANG type
        // production in error_union_type) — the two never share an LALR
        // state because type and expression positions are disjoint; the
        // conflict count check below guards that claim. Store TOKEN_NOT so
        // the existing typecheck (bool-only) and codegen (LLVMBuildNot)
        // arms serve the expression unchanged.
        UnaryExprNode* unary = ast_unary_expr_new(bison_token_to_token_type(NOT), $2, get_current_position());
        $$ = (ASTNode*)unary;
    }
```

(Replacing rather than adding keeps NOT out of the grammar's reachable token set; NOT stays declared in the `%right` line and the bridge, harmless. If replacing causes any bison complaint about the unused NOT token, keep the declaration — only the production body moves to BANG.)

- [ ] **Step 4: Verify the conflict count**

Run: `make lexer 2>&1 | grep -i conflict; grep -m2 "conflicts" src/parser/parser.output`
Expected: **78 shift/reduce, 256 reduce/reduce — unchanged.** If different, STOP: capture the delta and the conflicting state from parser.output, report before proceeding.

- [ ] **Step 5: Verify the probe passes and !T still works**

Run: `bin/goo -o build/boolean_not_probe examples/boolean_not_probe.goo && ./build/boolean_not_probe`
Expected: `false true false 1 2 4 true`, one per line.
Then confirm the error-union grammar is intact: the golden suite has !T probes (`erru-catch-probe`, `erru-error-probe`, `erru-abi-probe`) — they run in the Step 7 gate; additionally compile one manually now: `grep -l "!int\|!string" examples/*.goo | head -3` and `bin/goo` one of them successfully.

- [ ] **Step 6: Add the reject probe**

Makefile target next to `addrlit-reject-probe`, following its exact shape:

```make
# !x is boolean-only — !5 must be a clean type error (the TOKEN_NOT typecheck
# arm), not a parse error or a crash. Guards the BANG unary_expr production's
# routing into the boolean-NOT semantic.
boolnot-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== boolnot-reject-probe: ! on a non-bool must reject ==="
	@printf 'package main\nfunc main(){ x := !5; _ = x }\n' > build/boolnot_reject.goo
	@rm -f build/boolnot_reject
	@$(COMPILER) -o build/boolnot_reject build/boolnot_reject.goo > build/boolnot_reject.out 2> build/boolnot_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "boolnot-reject-probe: FAIL (compiled rc=0 — !5 silently accepted)"; exit 1; fi; \
	if [ -x build/boolnot_reject ]; then echo "boolnot-reject-probe: FAIL (emitted a binary despite the error)"; exit 1; fi; \
	if ! grep -q "Logical not requires boolean" build/boolnot_reject.err; then echo "boolnot-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/boolnot_reject.err; exit 1; fi; \
	echo "boolnot-reject-probe: PASS (rejected rc=$$rc)"
```

Register `boolnot-reject-probe` in the `verify:` dependency list immediately before `test-golden`. Run `make boolnot-reject-probe` → PASS.

- [ ] **Step 7: Run the gate**

Run: `eval "$(opam env --switch=default)"` (standalone), then `make verify` → all PASS incl. the erru probes and the new reject probe, golden 165/0. Then `make test` → 76/1.

- [ ] **Step 8: Commit**

```bash
git add src/parser/parser.y Makefile examples/boolean_not_probe.goo examples/boolean_not_probe.expected.txt
git commit --no-gpg-sign -m "feat(parser): boolean NOT !expr

A lone ! lexes to BANG, which the grammar only used for the !T
error-union type — the NOT unary production was dead (the lexer never
emits TOKEN_NOT), so boolean not was unparseable and its typecheck and
codegen arms were dead code. Route BANG unary_expr into TOKEN_NOT to
revive them. Conflict count unchanged (78 S/R + 256 R/R): the type and
expression positions of BANG never share an LALR state. boolnot-reject-
probe guards the bool-only typecheck."
```

---

## Final gate

`make verify` → ALL GREEN (165/0). `make test` → 76/1. ccomp (no runtime C change — run anyway):
```bash
eval "$(opam env --switch=default)"
make ccomp-link
```

## Self-review notes

- Single task: grammar production + positive probe + reject probe are one reviewable unit; splitting would leave the production untested mid-branch.
- The `^x` production comment (parser.y:1280) documents the codebase's pattern for context-overloaded tokens; the new comment follows it.
- Non-goals: emitting TOKEN_NOT from the lexer (would break `!T`); `&&`/`||` interactions (binary grammar untouched — `!a && b` parses as `(!a) && b` via existing precedence, covered implicitly by unary binding tighter than all binaries).
