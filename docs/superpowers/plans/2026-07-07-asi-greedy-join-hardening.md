# ASI Greedy-Join Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the four ASI greedy-join silent miscompiles (`f`⏎`(1)`, `a`⏎`[0]`, `v`⏎`.x`, `return`⏎`expr`) with a targeted lexer-only change.

**Architecture:** Extend the existing conditional automatic-semicolon-insertion in `src/lexer/lexer.c` with three additive parts: unconditional statement termination after `return`/`break`/`continue`/`fallthrough`; a greedy-join guard that inserts `;` when a value-ending token is followed across a newline by `(`, `[`, or `.`; and `++`/`--` added to the value-ending set. No grammar, parser, AST, or codegen changes.

**Tech Stack:** C23, flex/bison front end, LLVM codegen, Makefile-driven `*-probe` and `examples/*.goo` golden tests, `scripts/grammar-tripwire.sh` conflict baseline.

## Global Constraints

- C23 (`-std=c23`); the change is confined to `src/lexer/lexer.c` plus test files.
- `scripts/grammar-tripwire.sh` must PASS (unchanged conflict baseline) BEFORE and AFTER — any delta is stop-the-line (goo-grammar skill). This is a lexer-only change, so the count must not move.
- The compiler binary is `bin/goo` (Makefile `$(COMPILER)`). Build with `make bin/goo`.
- Golden fixtures live in `examples/<name>.goo` with a sibling `examples/<name>.expected.txt`; the `test-golden` harness (in `verify:`) runs them automatically.
- Reject/invariant probes are Makefile targets modeled on `generics-bound-reject-probe`; they must reject with a clean front-end error and NEVER emit `Module verification failed` / `LLVM ERROR`.
- No naked returns; explicit error handling; comments explain why.

---

### Task 1: Failing tests — goldens + the no-silent-join probe (RED)

Write the tests first. The `return` golden and the three invariant probe cases fail today; the over-rejection goldens pass today and guard against regressions in Task 2.

**Files:**
- Create: `examples/asi_return_probe.goo`, `examples/asi_return_probe.expected.txt`
- Create: `examples/asi_dotchain_probe.goo`, `examples/asi_dotchain_probe.expected.txt`
- Create: `examples/asi_multiline_probe.goo`, `examples/asi_multiline_probe.expected.txt`
- Create: `examples/asi_else_probe.goo`, `examples/asi_else_probe.expected.txt`
- Modify: `Makefile` (add the `asi-hardening-probe` target; NOT yet wired into `verify:`)

**Interfaces:**
- Consumes: `bin/goo` (`$(COMPILER)`), `$(RUNTIME_LIB)`.
- Produces: Makefile target `asi-hardening-probe` (wired into `verify:` in Task 2); four `examples/asi_*_probe.goo` goldens auto-consumed by `test-golden`.

- [ ] **Step 1: Write the `return`⏎expr golden (headline correctness case).**

`examples/asi_return_probe.goo`:
```go
package main

import "fmt"

// A bare `return` on its own line must terminate the statement; the following
// `r + 1` is a separate (unreachable) statement, so f() returns the named
// result r = 99. Before the ASI fix the lexer joined the lines into
// `return r + 1`, returning 100 — a silent miscompile.
func f() (r int) {
	r = 99
	return
	r + 1
}

func main() {
	fmt.Println(f())
}
```

`examples/asi_return_probe.expected.txt`:
```
99
```

- [ ] **Step 2: Write the dot-at-end chain golden (over-rejection guard — must still work).**

`examples/asi_dotchain_probe.goo`:
```go
package main

import "fmt"

type B struct{ v int }

func (b B) Add() B { return B{v: b.v + 1} }

// gofmt fluent chains put the dot at the END of the line. The token before each
// newline is `.` (not value-ending), so the ASI guard must never fire here.
func main() {
	r := B{v: 0}.
		Add().
		Add()
	fmt.Println(r.v)
}
```

`examples/asi_dotchain_probe.expected.txt`:
```
2
```

- [ ] **Step 3: Write the multi-line call-args + composite-literal golden (over-rejection guard).**

`examples/asi_multiline_probe.goo`:
```go
package main

import "fmt"

func add(a int, b int, c int) int { return a + b + c }

// Multi-line call-argument lists and composite literals end their continued
// lines with `,` / `{` / `(` (none value-ending), so ASI must not split them.
func main() {
	xs := []int{
		10,
		20,
		30,
	}
	n := add(
		xs[0],
		xs[1],
		xs[2],
	)
	fmt.Println(n)
}
```

`examples/asi_multiline_probe.expected.txt`:
```
60
```

- [ ] **Step 4: Write the `}`⏎`else` golden (over-rejection guard — intentional laxness preserved).**

`examples/asi_else_probe.goo`:
```go
package main

import "fmt"

// Goo intentionally tolerates `}` on its own line before `else` (more lenient
// than Go). The ASI change must not break this.
func classify(n int) string {
	if n > 0 {
		return "pos"
	}
	else {
		return "nonpos"
	}
}

func main() {
	fmt.Println(classify(5))
}
```

`examples/asi_else_probe.expected.txt`:
```
pos
```

- [ ] **Step 5: Verify the goldens' RED/GREEN state today.**

Run:
```bash
cd /data/Workspace/github.com/goolang
for n in asi_return_probe asi_dotchain_probe asi_multiline_probe asi_else_probe; do
  ./bin/goo -o build/$n examples/$n.goo 2>/dev/null && echo "$n -> $(./build/$n)"
done
```
Expected: `asi_return_probe -> 100` (RED — should be 99 after the fix); `asi_dotchain_probe -> 2`, `asi_multiline_probe -> 60`, `asi_else_probe -> pos` (GREEN today — over-rejection guards). If `asi_else_probe` does not compile today, that is a pre-existing gap — drop that one fixture and note it; do not attempt to fix `}`⏎`else` in this plan.

- [ ] **Step 6: Add the `asi-hardening-probe` Makefile target (no-silent-join invariant for call/index/selector).**

Add this target to `Makefile` immediately after the `generics-bound-reject-probe` target (do NOT add it to `verify:` yet — that happens in Task 2):
```makefile
# ASI greedy-join hardening: a value-ending token must NOT silently absorb a
# following `(`, `[`, or `.` across a newline. These three cases cannot be
# run-and-diff goldens (the un-joined line 2 — `(5)`, `[0]`, `.x` — is not a
# valid standalone statement), so each asserts the implementation-agnostic
# invariant: the program must NOT compile-and-print the joined-WRONG value, and
# must never reach the LLVM verifier with a crash. A clean front-end rejection
# or a correct reparse both pass.
asi-hardening-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== asi-hardening-probe: value-ending token must not join ( [ . across a newline ==="
	@# Case 1: `g := id` <nl> `(5)` — pre-fix joins to `g := id(5)` -> prints 5.
	@printf 'package main\nimport "fmt"\nfunc id(n int) int { return n }\nfunc main() {\n\tg := id\n\t(5)\n\tfmt.Println(g)\n}\n' > build/asi_call.goo
	@"$(COMPILER)" build/asi_call.goo -o build/asi_call.out 2>build/asi_call.err; rc=$$?; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/asi_call.err; then echo "asi-hardening-probe: FAIL (call case reached verifier)"; cat build/asi_call.err; exit 1; fi; \
	  if [ $$rc -eq 0 ] && [ "$$(./build/asi_call.out 2>/dev/null)" = "5" ]; then echo "asi-hardening-probe: FAIL (call case silently joined -> printed 5)"; exit 1; fi
	@# Case 2: `b := a` <nl> `[0]` — pre-fix joins to `b := a[0]` -> prints 10.
	@printf 'package main\nimport "fmt"\nfunc main() {\n\ta := []int{10, 20, 30}\n\tb := a\n\t[0]\n\tfmt.Println(b)\n}\n' > build/asi_index.goo
	@"$(COMPILER)" build/asi_index.goo -o build/asi_index.out 2>build/asi_index.err; rc=$$?; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/asi_index.err; then echo "asi-hardening-probe: FAIL (index case reached verifier)"; cat build/asi_index.err; exit 1; fi; \
	  if [ $$rc -eq 0 ] && [ "$$(./build/asi_index.out 2>/dev/null)" = "10" ]; then echo "asi-hardening-probe: FAIL (index case silently joined -> printed 10)"; exit 1; fi
	@# Case 3: `q := p` <nl> `.x` — pre-fix joins to `q := p.x` -> prints 7.
	@printf 'package main\nimport "fmt"\ntype P struct { x int; y int }\nfunc main() {\n\tp := P{x: 7, y: 9}\n\tq := p\n\t.x\n\tfmt.Println(q)\n}\n' > build/asi_sel.goo
	@"$(COMPILER)" build/asi_sel.goo -o build/asi_sel.out 2>build/asi_sel.err; rc=$$?; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/asi_sel.err; then echo "asi-hardening-probe: FAIL (selector case reached verifier)"; cat build/asi_sel.err; exit 1; fi; \
	  if [ $$rc -eq 0 ] && [ "$$(./build/asi_sel.out 2>/dev/null)" = "7" ]; then echo "asi-hardening-probe: FAIL (selector case silently joined -> printed 7)"; exit 1; fi
	@echo "asi-hardening-probe: PASS"
```

- [ ] **Step 7: Run the probe to verify it FAILS today.**

Run:
```bash
make asi-hardening-probe 2>&1 | tail -5
```
Expected: `asi-hardening-probe: FAIL (call case silently joined -> printed 5)` (the first case trips immediately — today the compiler joins the lines). This confirms the probe detects the miscompile.

- [ ] **Step 8: Commit (RED).**

```bash
git add examples/asi_return_probe.goo examples/asi_return_probe.expected.txt \
        examples/asi_dotchain_probe.goo examples/asi_dotchain_probe.expected.txt \
        examples/asi_multiline_probe.goo examples/asi_multiline_probe.expected.txt \
        examples/asi_else_probe.goo examples/asi_else_probe.expected.txt \
        Makefile
git commit -m "test(lexer): failing probes for the four ASI greedy-join miscompiles

return-probe golden expects 99 (joins to 100 today); asi-hardening-probe
asserts call/index/selector don't silently join to 5/10/7. RED until the
lexer fix lands."
```
Note: the pre-commit hook runs `make test` (unit tests), not the goldens, so the RED golden does not block the commit.

---

### Task 2: Implement the lexer ASI extension (GREEN)

**Files:**
- Modify: `src/lexer/lexer.c` — `token_ends_value` (~86-102), the `'\n'` handler (~145-157)
- Modify: `Makefile` — add `asi-hardening-probe` to the `verify:` prerequisite list

**Interfaces:**
- Consumes: `token_ends_value(TokenType)`, `char_starts_continuation_op(char)`, `lexer->prev_token_type`, `lexer->ch`, `lexer_peek_char(lexer)` (all existing in `lexer.c`).
- Produces: no new symbols; behavioral change only.

- [ ] **Step 1: Capture the grammar-conflict baseline BEFORE the change.**

Run:
```bash
cd /data/Workspace/github.com/goolang
./scripts/grammar-tripwire.sh; echo "tripwire rc=$?"
```
Expected: PASS, `tripwire rc=0`. (This is a lexer-only change; the count must be identical after.)

- [ ] **Step 2: Add `++` and `--` to `token_ends_value` (Part 3).**

In `src/lexer/lexer.c`, in `token_ends_value`, add two cases alongside the existing value-ending tokens:
```c
static int token_ends_value(TokenType t) {
    switch (t) {
        case TOKEN_IDENT:
        case TOKEN_INT:
        case TOKEN_FLOAT:
        case TOKEN_STRING:
        case TOKEN_CHAR:
        case TOKEN_TRUE:
        case TOKEN_FALSE:
        case TOKEN_RPAREN:
        case TOKEN_RBRACKET:
        case TOKEN_RBRACE:
        case TOKEN_INCREMENT:   // x++  — postfix, value-ending
        case TOKEN_DECREMENT:   // x--  — postfix, value-ending
            return 1;
        default:
            return 0;
    }
}
```

- [ ] **Step 3: Add the keyword-terminator branch (Part 1) and extend the join guard (Part 2).**

In `src/lexer/lexer.c`, in the `case '\n':` block, the current code (after consuming the newline and skipping horizontal whitespace) is:
```c
            if (token_ends_value(lexer->prev_token_type) &&
                char_starts_continuation_op(lexer->ch) &&
                !(lexer->ch == '/' &&
                  (lexer_peek_char(lexer) == '/' || lexer_peek_char(lexer) == '*'))) {
                lexer->prev_token_type = TOKEN_SEMICOLON;
                return token_new(TOKEN_SEMICOLON, ";", 1, current_pos);
            }
```
Replace it with:
```c
            // Part 1 — keyword terminators (unconditional). Go always ends the
            // statement after these keywords; their operand (a return
            // expression, a break/continue label) must sit on the same line, so
            // a newline here is unambiguously a boundary. This is separate from
            // the value-ending guard below because `return` <nl> `expr` has an
            // identifier (not an operator/bracket) starting line 2, which the
            // next-char guard would not catch.
            if (lexer->prev_token_type == TOKEN_RETURN ||
                lexer->prev_token_type == TOKEN_BREAK ||
                lexer->prev_token_type == TOKEN_CONTINUE ||
                lexer->prev_token_type == TOKEN_FALLTHROUGH) {
                lexer->prev_token_type = TOKEN_SEMICOLON;
                return token_new(TOKEN_SEMICOLON, ";", 1, current_pos);
            }
            // Part 2 — greedy-join guard. Insert a `;` when a value-ending token
            // is followed across a newline by a continuation operator (existing
            // behaviour) OR by `(`, `[`, `.` — otherwise the parser joins the
            // lines into a call / index / selector (silent miscompile). The
            // guard is asymmetric: a legitimate multi-line continuation leaves
            // the operator/dot at the END of line 1 (`foo().` <nl> `bar()`), so
            // the token before the newline is not value-ending and this does not
            // fire. `{` is deliberately excluded (`if cond` <nl> `{` must not
            // split). The `/` sub-condition still guards a comment-opening `//`
            // or `/*` from being treated as a continuation operator.
            if (token_ends_value(lexer->prev_token_type) &&
                (char_starts_continuation_op(lexer->ch) ||
                 lexer->ch == '(' || lexer->ch == '[' || lexer->ch == '.') &&
                !(lexer->ch == '/' &&
                  (lexer_peek_char(lexer) == '/' || lexer_peek_char(lexer) == '*'))) {
                lexer->prev_token_type = TOKEN_SEMICOLON;
                return token_new(TOKEN_SEMICOLON, ";", 1, current_pos);
            }
```

- [ ] **Step 4: Rebuild the compiler.**

Run:
```bash
make bin/goo 2>&1 | tail -3
```
Expected: builds cleanly (warnings from unrelated files are fine); `bin/goo` is produced.

- [ ] **Step 5: Run the no-silent-join probe — expect GREEN.**

Run:
```bash
make asi-hardening-probe 2>&1 | tail -3
```
Expected: `asi-hardening-probe: PASS`.

- [ ] **Step 6: Run the goldens — `return` case now prints 99, guards unchanged.**

Run:
```bash
for n in asi_return_probe asi_dotchain_probe asi_multiline_probe asi_else_probe; do
  ./bin/goo -o build/$n examples/$n.goo 2>/dev/null && echo "$n -> $(./build/$n)"
done
```
Expected: `asi_return_probe -> 99`, `asi_dotchain_probe -> 2`, `asi_multiline_probe -> 60`, `asi_else_probe -> pos`. (If `asi_else_probe` was dropped in Task 1 Step 5, omit it.)

- [ ] **Step 7: Confirm the grammar baseline is UNCHANGED.**

Run:
```bash
./scripts/grammar-tripwire.sh; echo "tripwire rc=$?"
```
Expected: PASS, `tripwire rc=0`, identical counts to Task 2 Step 1. Any delta is stop-the-line — revert and reassess.

- [ ] **Step 8: Wire the probe into `verify:`.**

In `Makefile`, add `asi-hardening-probe` to the `verify:` prerequisite list, immediately after `generics-bound-reject-probe`.

- [ ] **Step 9: Run the full gate.**

Run:
```bash
eval $(opam env --switch default)
make verify CCOMP=~/.opam/default/bin/ccomp 2>&1 | tail -6
```
Expected: `verify: ALL GREEN GATES PASSED`, golden count increased by the new fixtures (`N passed, 0 failed`), `asi-hardening-probe: PASS`.

- [ ] **Step 10: Commit (GREEN).**

```bash
git add src/lexer/lexer.c Makefile
git commit -m "fix(lexer): harden ASI against the four greedy-join miscompiles

Value-ending token <nl> ( [ . now inserts a semicolon (was silently joined
into a call/index/selector); return/break/continue/fallthrough <nl> always
terminate; ++/-- are value-ending. Lexer-only; grammar-tripwire unchanged.
asi-hardening-probe + asi_*_probe goldens now green; wired into verify."
```

---

## Self-Review

**Spec coverage:**
- Part 1 keyword terminators → Task 2 Step 3 (first `if`). ✔
- Part 2 `( [ .` join guard → Task 2 Step 3 (second `if`). ✔
- Part 3 `++`/`--` value-ending → Task 2 Step 2. ✔
- `{` exclusion → encoded (only `( [ .` added) + comment. ✔
- Correctness tests (4 cases) → return golden (Task 1 Step 1) + call/index/selector probe (Task 1 Step 6). ✔
- Over-rejection guards (dot-at-end, multi-line call/composite, `}`⏎else) → Task 1 Steps 2-4. Operator-at-end continuation is already covered by the pre-existing `escape-probe`/general suite and by the unchanged `char_starts_continuation_op` path; the multiline golden's `+`-free body plus the untouched operator logic make a dedicated fixture redundant (YAGNI). ✔
- `verify` + grammar-tripwire gate → Task 2 Steps 1, 7, 9. ✔

**Placeholder scan:** No TBD/TODO; all code blocks complete; the one contingency (`asi_else_probe` may not compile today) has an explicit fallback instruction, not a placeholder.

**Type consistency:** `token_ends_value`, `char_starts_continuation_op`, `lexer_peek_char`, `TOKEN_INCREMENT`, `TOKEN_DECREMENT`, `TOKEN_RETURN`, `TOKEN_BREAK`, `TOKEN_CONTINUE`, `TOKEN_FALLTHROUGH` all verified against `include/token.h` and `src/lexer/lexer.c`. Target name `asi-hardening-probe` consistent across Task 1 Step 6, Task 2 Step 8/9.
