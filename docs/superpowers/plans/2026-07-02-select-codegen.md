# Select Codegen Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix select-statement codegen bugs found during PR #94: (Task 1) multi-statement select-case bodies silently emit only the FIRST statement; (Task 2 — pending root-cause, to be appended once the repro is confirmed) a reported recv-corruption interaction with narrow locals.

**Architecture:** Task 1 is a one-line-class fix: `codegen_generate_select_stmt` calls `codegen_generate_statement` ONCE on `select_case->body` (statement_codegen.c:1727-1728) while the body is a `->next` statement chain — switch codegen already loops (statement_codegen.c:595-600); mirror it. The checker (PR #94) already type-checks the full chain.

**Tech Stack:** C23, LLVM-C. Codegen-only.

## Global Constraints

- Branch: `fix/select-codegen` (already created off main @e1b54fb — do NOT commit on main).
- Commits: conventional style, imperative mood, `--no-gpg-sign`. Stage only named files; never stage anything under `.superpowers/`.
- Gate: `make lexer`, probe, then `eval "$(opam env --switch=default)"` STANDALONE, then `make verify` (all PASS; golden baseline 168/0 grows to 169/0) and `make test` (76 pass / 1 pre-existing skip). Select/channel goldens must stay green.
- Probes: same-width prints only (mixed-width Println bug).

## Reference: verified code landmarks (2026-07-02, main @e1b54fb)

- The bug: `src/codegen/statement_codegen.c:1726-1733` — `if (select_case->body) { if (!codegen_generate_statement(codegen, checker, select_case->body)) {...} }` — emits one statement; the parser chains body statements via `->next`.
- The reference: switch case-body loop, `src/codegen/statement_codegen.c:595-600` — `for (ASTNode* s = clause->body; s; s = s->next) { ... }` with the same error-cleanup shape (`codegen_pop_loop` + `free` + return 0).
- Terminator handling after the body (`:1735+`, "Branch to end, unless the body already terminated") stays as-is — it reads the CURRENT insert block after the loop, same as switch's `:601-602`.

---

### Task 1: Emit full select-case bodies

**Files:**
- Modify: `src/codegen/statement_codegen.c:1726-1733`
- Test: `examples/select_body_probe.goo` + `examples/select_body_probe.expected.txt`

**Interfaces:**
- Consumes: PR #94's checker (full-chain type annotations already present).
- Produces: every statement in a select-case body executes.

- [ ] **Step 1: Write the failing probe**

`examples/select_body_probe.goo`:
```go
package main

import "fmt"

func main() {
	ch := make_chan(int, 1)
	total := 0
	select {
	case ch <- 5:
		fmt.Println(1)
		total = total + 10
		fmt.Println(2)
	default:
		fmt.Println(0)
	}
	fmt.Println(total)
	v := <-ch
	select {
	case ch <- 9:
		fmt.Println(3)
	default:
		fmt.Println(4)
		total = total + v
		fmt.Println(total)
	}
}
```

`examples/select_body_probe.expected.txt`:
```
1
2
10
3
```

Wait — trace it: first select sends 5 (buffered, succeeds) → prints 1, total=10, prints 2. Then prints total (10). `v := <-ch` receives 5, emptying the buffer. Second select: `ch <- 9` succeeds (buffer empty) → prints 3. Expected: `1 2 10 3`. The default arm's multi-statement body is not exercised on the success path — REWORK: make the second select exercise default by NOT receiving first. Final probe (use this, not the sketch above):

```go
package main

import "fmt"

func main() {
	ch := make_chan(int, 1)
	total := 0
	select {
	case ch <- 5:
		fmt.Println(1)
		total = total + 10
		fmt.Println(2)
	default:
		fmt.Println(0)
	}
	fmt.Println(total)
	select {
	case ch <- 9:
		fmt.Println(3)
	default:
		fmt.Println(4)
		total = total + 100
		fmt.Println(total)
	}
	v := <-ch
	fmt.Println(v + total)
}
```

`examples/select_body_probe.expected.txt`:
```
1
2
10
4
110
115
```

Trace: buffered send 5 succeeds → 1, total=10, 2. Print 10. Second select: buffer FULL → default → 4, total=110, print 110. Recv 5 → 5+110=115. Both a comm arm and a default arm carry 3-statement bodies.

- [ ] **Step 2: Verify it fails today**

Run: `bin/goo -o build/select_body_probe examples/select_body_probe.goo && ./build/select_body_probe`
Expected today: `1 10 4 105` — the trailing statements of each body silently dropped (total never incremented in either body, only first Printlns fire). Record the ACTUAL output in your report (it demonstrates the drop).

- [ ] **Step 3: Fix — loop the body chain**

Replace the single-call block at statement_codegen.c:1726-1733 with the switch idiom:

```c
        // Generate case body. The body is a ->next statement chain — loop it
        // like switch codegen does (a single codegen_generate_statement call
        // silently dropped every statement after the first).
        for (ASTNode* s = select_case->body; s; s = s->next) {
            if (!codegen_generate_statement(codegen, checker, s)) {
                codegen_pop_loop(codegen);
                free(case_blocks);
                return 0;
            }
        }
```

The terminator check after it stays unchanged.

- [ ] **Step 4: Rebuild and verify the probe passes**

`make lexer`, compile+run the probe. Expected: `1 2 10 4 110 115`.

- [ ] **Step 5: Run the gate**

`eval "$(opam env --switch=default)"` (standalone), `make verify` → all PASS, golden 169/0 (select-probe, parallel-select-soak-probe, select_typecheck_probe green). `make test` → 76/1.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/statement_codegen.c examples/select_body_probe.goo examples/select_body_probe.expected.txt
git commit --no-gpg-sign -m "fix(codegen): emit every statement of a select-case body

codegen_generate_select_stmt called codegen_generate_statement once on
the case body, but the body is a ->next statement chain — everything
after the first statement was silently dropped (no error, no warning;
found while implementing the select type checker). Loop the chain like
switch codegen does."
```

---

## Final gate

`make verify` → ALL GREEN (169/0). `make test` → 76/1. ccomp: opam env standalone, `make ccomp-link` → PASS.

## Self-review notes

- Task 2 (the reported narrow-local/recv corruption) is deliberately absent until the exact repro is confirmed — the controller could not reproduce it from the description and has requested the reviewer's verbatim probe; it will be appended (or recorded as not-reproducible) before the branch ships.
- The probe's Step 1 includes a worked trace because buffered-channel select arms are easy to mis-predict; the first sketch in this plan is superseded by the final probe (kept for the reasoning trail).
