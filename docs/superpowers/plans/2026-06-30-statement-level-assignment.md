# Statement-Level Assignment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `s[i], s[j] = s[j], s[i]` (tuple assignment to index/selector lvalues) parse and run, by moving assignment from the expression level to the statement level (Go-faithful), so the TinyGo `sort` port's `Swap` becomes idiomatic.

**Architecture:** Assignment becomes a `simple_stmt` rule whose LHS uses the same `expression` nonterminal as everything else (one reduction path → no reduce/reduce conflict, unlike the 3 prior `assign_lhs`-nonterminal attempts). Addressability of targets becomes a semantic typecheck. Single-assign keeps producing the existing `ExprStmt(BinaryExpr ASSIGN)` AST so typecheck/codegen are unchanged; multi keeps producing `AST_MULTI_ASSIGN` (whose per-target lvalue handling already works).

**Tech Stack:** bison/flex grammar (`src/parser/parser.y`), C23, LLVM, `scripts/run_golden.sh`, `make verify`.

## Global Constraints

- Gate = `make verify` ALL GREEN (incl. ccomp via `eval "$(opam env --switch=default)"`) + `make test` 76/1 + golden, no regressions.
- No `%expect`/conflict-count gate exists — conflict-count deltas are acceptable ONLY if golden + make test stay green (bison resolves the conflict correctly). MEASURE the bison `shift/reduce` + `reduce/reduce` counts before and after and report them.
- Editing `parser.y` regenerates `parser.tab.c` on `make bin/goo`; no `make clean` needed for a `.y` change.
- Commits FAIL to sign here (1Password agent unavailable) — use `git commit --no-gpg-sign`.
- No naked returns / no silent failures.
- Golden probes auto-discovered: `examples/<name>.goo` + `examples/<name>.expected.txt`.
- Baseline conflict counts (current main): 73 shift/reduce, 219 reduce/reduce (from a clean `make bin/goo`). Spec: `docs/superpowers/specs/2026-06-30-statement-level-assignment-design.md`.

---

### Task 1: Move assignment to the statement level + addressability check

**Files:**
- Modify: `src/parser/parser.y` — `simple_stmt` production (the `identifier COMMA identifier ASSIGN …` rule) and the `expression:` block (remove `expression ASSIGN expression` and `expression SHORT_ASSIGN expression`).
- Modify: `src/types/expression_helpers.c` (`type_check_assignment_op`) and `src/types/type_checker.c` (`type_check_multi_assign`) — add an addressability guard.
- Test: `examples/index_swap_probe.goo` + `.expected.txt`.

**Interfaces:**
- Consumes: `ast_binary_expr_new(lhs, op, rhs, pos)`, `bison_token_to_token_type(ASSIGN)`, `multi_assign_2_new(t1,t2,v1,v2,is_short)`, `ExprStmtNode`/`AST_EXPR_STMT` (all already used in `simple_stmt`).
- Produces: tuple-index assignment parses; non-lvalue assignment targets are rejected at typecheck.

- [ ] **Step 1: Record baseline conflicts**

Run: `make bin/goo 2>&1 | grep -E 'shift/reduce|reduce/reduce'`
Expected: `73 shift/reduce conflicts`, `219 reduce/reduce conflicts`. Note them.

- [ ] **Step 2: Write the failing probe** — `examples/index_swap_probe.goo`:

```go
// index_swap_probe: multi-assign to INDEXED lvalues — the sort Swap idiom.
// Go evaluates all RHS before any store, so the swap is correct.
package main

import "fmt"

func main() {
	s := []int{10, 20, 30}
	i := 0
	j := 2
	s[i], s[j] = s[j], s[i] // -> 30 20 10
	fmt.Println(s[0])       // 30
	fmt.Println(s[1])       // 20
	fmt.Println(s[2])       // 10
}
```

`examples/index_swap_probe.expected.txt`:
```
30
20
10
```

- [ ] **Step 3: Verify it fails**

Run: `./bin/goo -o /tmp/isw examples/index_swap_probe.goo`
Expected: `Parse error ... syntax error` near `s[i], s[j] = ...`.

- [ ] **Step 4: Restructure the grammar.** In `src/parser/parser.y`, find the `simple_stmt:` production. Replace its multi-assign rule:

```
    | identifier COMMA identifier ASSIGN expression COMMA expression {
        $$ = multi_assign_2_new($1, $3, $5, $7, 0);
    }
    ;
```

with single + multi rules that use `expression` for targets (the single rule wraps the assignment `BinaryExpr` in an `ExprStmt`, identical to how `simple_stmt → expression` wraps a bare `s[i] = x` today, so codegen is unchanged):

```
    // Single assignment to any lvalue: `x = e`, `s[i] = e`, `p.f = e`. Was an
    // expression-level rule (expression ASSIGN expression); moved here so the
    // LHS shares the `expression` reduction and tuple assignment below has no
    // reduce/reduce conflict. Wrapped in ExprStmt to match the prior AST shape.
    | expression ASSIGN expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(ASSIGN), $3, get_current_position());
        ExprStmtNode* es = (ExprStmtNode*)malloc(sizeof(ExprStmtNode));
        es->base.type = AST_EXPR_STMT;
        es->base.pos = get_current_position();
        es->base.node_type = NULL;
        es->base.next = NULL;
        es->expr = (ASTNode*)binary;
        $$ = (ASTNode*)es;
    }
    // Tuple assignment to any lvalues: `a, b = e1, e2`, `s[i], s[j] = s[j], s[i]`.
    // Targets are full expressions (addressability checked in the typechecker).
    | expression COMMA expression ASSIGN expression COMMA expression {
        $$ = multi_assign_2_new($1, $3, $5, $7, 0);
    }
    ;
```

Then in the `expression:` block, DELETE these two rules (the single-assign and short-assign expression productions — search for `expression ASSIGN expression {` and `expression SHORT_ASSIGN expression {`):

```
    | expression ASSIGN expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(ASSIGN), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
    | expression SHORT_ASSIGN expression {
        BinaryExprNode* binary = ast_binary_expr_new($1, bison_token_to_token_type(SHORT_ASSIGN), $3, get_current_position());
        $$ = (ASTNode*)binary;
    }
```

- [ ] **Step 5: Rebuild, measure conflicts, run the probe**

Run: `make bin/goo 2>&1 | grep -E 'shift/reduce|reduce/reduce'` — record the new counts (a delta is OK; the gate is golden+test green).
Run: `./bin/goo -o /tmp/isw examples/index_swap_probe.goo && /tmp/isw`
Expected: `30 20 10`. If it is a parse error, the `expression COMMA expression` LHS is conflicting — STOP and report the exact conflict; do NOT add an `assign_lhs` nonterminal (that is the known dead end).

- [ ] **Step 6: Regression-check the common assignment shapes by hand**

Run each and confirm it compiles + runs (write each to `/tmp` and run):
- `a, b = b, a` (identifiers): `func main(){a:=1;b:=2;a,b=b,a;fmt.Println(a);fmt.Println(b)}` → `2` `1`
- `s[0] = s[1]` (single index): → no error
- `p.x = 5` then read (single selector): → no error
- `x := s[i]` (short-decl): → no error
- C-style for post `for i:=0;i<2;i=i+1 {}`: → no error
- `x := y` (plain short-decl, ensure `:=` still works after removing the expression-level SHORT_ASSIGN rule): → no error

Any failure here means the grammar move regressed a real shape — fix before continuing.

- [ ] **Step 7: Add the addressability guard (typecheck).** The grammar now accepts non-lvalue targets like `f() = 3`. In `src/types/expression_helpers.c`, in `type_check_assignment_op`, before the existing compatibility logic, reject a non-addressable target:

```c
    // The grammar accepts any expression as an assignment LHS; enforce
    // addressability here. Lvalues are identifiers, index, selector, and deref.
    if (target->type != AST_IDENTIFIER && target->type != AST_INDEX_EXPR &&
        target->type != AST_SELECTOR_EXPR &&
        !(target->type == AST_UNARY_EXPR &&
          ((UnaryExprNode*)target)->operator == TOKEN_MULTIPLY)) {
        type_error(checker, pos, "cannot assign to a non-addressable expression");
        return NULL;
    }
```
(Confirm the deref node shape: read how `*p` is represented — `AST_UNARY_EXPR` with `TOKEN_MULTIPLY`, or a dedicated deref node — and match it. If deref is rare/unsupported as an lvalue today, the three forms identifier/index/selector suffice; note what you used.)

Mirror the same guard per target in `src/types/type_checker.c` `type_check_multi_assign` (the `=` branch, where each target `t` is checked).

- [ ] **Step 8: Verify the addressability rejection**

Write `/tmp/bad.goo` with `func main(){ foo() }`-style non-lvalue: `func f() int {return 1}` and in main `f() = 3`. Run `./bin/goo -o /tmp/bad /tmp/bad.goo` → expect a clean `cannot assign to a non-addressable expression` type error, no binary, no crash. Report the exact message.

- [ ] **Step 9: Full gate**

Run: `eval "$(opam env --switch=default)" && make verify && make test`
Expected: `verify: ALL GREEN GATES PASSED`, golden all pass (incl. `index_swap_probe`), `make test` 76/1.

- [ ] **Step 10: Commit**

```bash
git add src/parser/parser.y src/types/expression_helpers.c src/types/type_checker.c examples/index_swap_probe.goo examples/index_swap_probe.expected.txt
git commit --no-gpg-sign -m "feat(parser,types): statement-level assignment enables tuple-index assign"
```

---

### Task 2: Capstone — idiomatic Swap + selector swap probe

**Files:**
- Create: `examples/selector_swap_probe.goo` + `.expected.txt`
- Modify: `examples/sort_named_probe.goo` (replace the temp-based `Swap` with the idiomatic tuple form)

**Interfaces:**
- Consumes: Task 1 (tuple-index/selector assignment).

- [ ] **Step 1: Write the selector-swap probe** — `examples/selector_swap_probe.goo`:

```go
// selector_swap_probe: tuple assignment to struct-field selectors.
package main

import "fmt"

type Pair struct {
	a int
	b int
}

func main() {
	p := Pair{a: 1, b: 2}
	p.a, p.b = p.b, p.a // swap fields
	fmt.Println(p.a)    // 2
	fmt.Println(p.b)    // 1
}
```

`examples/selector_swap_probe.expected.txt`:
```
2
1
```

- [ ] **Step 2: Verify it passes** (Task 1 already enables it)

Run: `./bin/goo -o /tmp/ssw examples/selector_swap_probe.goo && /tmp/ssw`
Expected: `2` then `1`. If it fails, Task 1's selector-target handling is incomplete — return to Task 1.

- [ ] **Step 3: Make the sort capstone idiomatic.** In `examples/sort_named_probe.goo`, replace the temp-based `Swap` body:

```go
func (s IntSlice) Swap(i int, j int) {
	tmp := s[i]
	s[i] = s[j]
	s[j] = tmp
}
```

with the idiomatic Go form:

```go
func (s IntSlice) Swap(i int, j int) {
	s[i], s[j] = s[j], s[i]
}
```

- [ ] **Step 4: Verify the sort still sorts**

Run: `./bin/goo -o /tmp/snp examples/sort_named_probe.goo && /tmp/snp`
Expected: `1` `2` `3`.

- [ ] **Step 5: Full gate + commit**

```bash
eval "$(opam env --switch=default)" && make verify && make test
git add examples/selector_swap_probe.goo examples/selector_swap_probe.expected.txt examples/sort_named_probe.goo
git commit --no-gpg-sign -m "test(golden): idiomatic tuple Swap in sort + selector swap probe"
```

- [ ] **Step 6: Update memory** — append to `goolang-v1-roadmap`: statement-level assignment shipped; `s[i],s[j]=s[j],s[i]` works; the TinyGo sort port is now fully Go-faithful (no temp-Swap workaround).

---

## Self-Review

- **Spec coverage:** grammar move → Task 1 Steps 4-5; remove expression-level assign → Task 1 Step 4; addressability typecheck → Task 1 Steps 7-8; regression shapes → Task 1 Step 6; index_swap probe → Task 1; selector_swap probe + idiomatic capstone → Task 2. Conflict measurement → Task 1 Steps 1/5. Out-of-scope (compound assign, >2 targets, chained) not tasked — correct. ✓
- **Placeholder scan:** all grammar/typecheck code is concrete; Step 7 flags the one genuine unknown (deref-lvalue node shape) with a fallback (identifier/index/selector suffice) — not a placeholder, a bounded verify. ✓
- **Type consistency:** `multi_assign_2_new`, `ast_binary_expr_new`, `bison_token_to_token_type`, `ExprStmtNode`/`AST_EXPR_STMT`, `AST_INDEX_EXPR`/`AST_SELECTOR_EXPR`/`AST_IDENTIFIER` used consistently with their real definitions. ✓
- **Ordering:** Task 1 is the whole grammar+typecheck change (a non-lvalue-rejecting, regression-clean deliverable); Task 2 is probes/capstone. ✓
