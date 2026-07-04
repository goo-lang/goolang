# Type Assertions & Type Switches Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `x.(T)` (comma-ok + single-return) and `switch v := x.(type)` for concrete target types, via vtable-pointer identity, building on #113's interface representation.

**Architecture:** Dynamic-type identity is a pointer comparison — `x.vtable == &goo.vtable.T.I` — reusing #113's per-`(concrete,interface)` vtable globals with zero new RTTI. A new `AST_TYPE_ASSERT` node carries `{expr, asserted_type}`; the type switch desugars to an ordered chain of the same check. Grammar is the risk center (goo-grammar skill governs; tripwire 81 S/R + 256 R/R).

**Tech Stack:** C23, bison/flex, LLVM-C API.

## Global Constraints

- Branch: `feat/type-assertions` (exists, base main @ fc3211b). Commit `--no-gpg-sign`; pre-commit hook runs `make test`.
- Spec: `docs/superpowers/specs/2026-07-04-type-assertions-design.md`. Scope: CONCRETE target types only; assert-to-interface is a clean v1 rejection. All three forms.
- **Grammar tripwire (T1, T3 touch parser.y):** `./scripts/grammar-tripwire.sh` must PASS (81 S/R + 256 R/R) before AND after any parser change. ANY delta → STOP, follow the goo-grammar skill's justified-delta procedure (`bison -Wcounterexamples`, classify, prove via goldens + probes, or revert). Do not absorb a delta.
- `include/ast.h`: append `AST_TYPE_ASSERT` at the enum TAIL (before `AST_NODE_COUNT` at line 159) and the node struct at the file tail; then `make clean && make lexer` (no header deps — stale objects silently miscompile).
- Baselines: golden 224/0 (`bash scripts/run_golden.sh`), `make test` 76 passed/1 skipped, bison 81/256.
- Every golden's expected output produced by `go run` on an equivalent `.go` program (go installed) — never hand-written. Real exit codes only (never a pipeline `$?`).
- `make verify` + `make ccomp-link` need `eval "$(opam env --switch=default)"` first.
- Deviation (recorded, from spec): single-return panic message uses static names (`"interface conversion: I is not T"`) — no dynamic-type name (no RTTI). Do NOT try to name the dynamic type.

---

### Task 1: Assertion grammar + `AST_TYPE_ASSERT` node

**Files:**
- Modify: `include/ast.h` (TAIL: `AST_TYPE_ASSERT` enum value + `TypeAssertNode` struct)
- Modify: `src/parser/parser.y` (one arm beside `selector_expr` ~1834)
- Create: `examples/type_assert_parse_probe.goo` + `.expected.txt` (parse+run smoke, minimal)

**Interfaces:**
- Consumes: `type` grammar nonterminal; `SelectorExprNode` pattern (ast.h:543, parser.y:1834-1838).
- Produces: `AST_TYPE_ASSERT` node type + `TypeAssertNode { ASTNode base; struct ASTNode* expr; struct ASTNode* asserted_type; }` — Task 2 (typecheck/codegen) and Task 3 (type switch) consume it.

- [ ] **Step 1: Confirm clean grammar baseline**

Run: `./scripts/grammar-tripwire.sh`
Expected: `PASS (81 S/R + 256 R/R — baseline exact)`. If not, stop — tree is off-baseline before you start.

- [ ] **Step 2: Add the AST node (tail of ast.h) + make clean**

In `include/ast.h`, add `AST_TYPE_ASSERT,` immediately before `AST_NODE_COUNT` (line 159). At the file's struct tail (after `SliceConvNode`):

```c
// x.(T) type assertion. asserted_type is the parsed target type node.
// The comma-ok vs single-return form is NOT stored here — it is decided by
// assignment context at typecheck/codegen (2 LHS names vs 1), exactly like
// the comma-ok map read.
typedef struct {
    ASTNode base;
    struct ASTNode* expr;
    struct ASTNode* asserted_type;
} TypeAssertNode;
```

Run: `make clean && make lexer` (exit 0 — mandatory after ast.h edit).

- [ ] **Step 3: Write the parse smoke golden**

`examples/type_assert_parse_probe.goo` (go-run-verify the output first — this exercises the happy path end-to-end, so it also depends on Task 2; for THIS task's RED/GREEN, Step 5 checks it PARSES, and the run is re-confirmed after Task 2. Keep it minimal):

```go
package main

import "fmt"

type Animal interface {
	Sound() string
}

type Dog struct {
	Name string
}

func (d Dog) Sound() string {
	return "woof"
}

func main() {
	var a Animal = Dog{Name: "Rex"}
	d, ok := a.(Dog)
	fmt.Println(ok)
	fmt.Println(d.Name)
}
```

`examples/type_assert_parse_probe.expected.txt`:

```
true
Rex
```

- [ ] **Step 4: Add the grammar arm**

In `parser.y`, add beside the `selector_expr` production (after the arm at ~1834-1846) a new `primary_expr` alternative (locate where `selector_expr`/`index_expr`/`call_expr` are listed as `primary_expr` alternatives ~1587 and add a sibling, OR add the arm to `selector_expr` itself — follow whichever the surrounding structure uses; the KEY is `primary_expr DOT LPAREN type RPAREN`):

```yacc
    | primary_expr DOT LPAREN type RPAREN {
        TypeAssertNode* ta = (TypeAssertNode*)malloc(sizeof(TypeAssertNode));
        ta->base.type = AST_TYPE_ASSERT;
        ta->base.pos = get_current_position();
        ta->base.node_type = NULL;
        ta->base.next = NULL;
        ta->expr = $1;
        ta->asserted_type = $4;
        $$ = (ASTNode*)ta;
    }
```

- [ ] **Step 5: Tripwire + parse verification**

Run: `./scripts/grammar-tripwire.sh`
Expected: `PASS (81 S/R + 256 R/R)`. On ANY delta: STOP, run `bison -Wcounterexamples -d -o /tmp/p.tab.c src/parser/parser.y 2>&1 | head -60`, and report BLOCKED with the counterexample — the `DOT LPAREN` vs `DOT identifier` split should be clean (one-token lookahead), so a delta means something unexpected.

Run: `make lexer` (exit 0), then `bin/goo -o build/tap examples/type_assert_parse_probe.goo`.
Expected at THIS task's stage: it PARSES (no syntax error). It may fail LATER in typecheck/codegen with "type assertion not implemented" or similar — that's expected until Task 2. If it emits a SYNTAX error, the grammar arm is wrong. If typecheck happens to already handle it and it runs, even better — note it.

- [ ] **Step 6: Commit**

```bash
git add include/ast.h src/parser/parser.y examples/type_assert_parse_probe.*
git commit --no-gpg-sign -m "feat(parser): x.(T) type-assertion grammar + AST_TYPE_ASSERT node"
```

(The golden is NOT yet in the run-suite as passing if codegen isn't done — if `run_golden.sh` would now FAIL on it, that's a problem: either Task 2's codegen must land first, or hold the golden until Task 2. DECISION: hold the golden file's addition to the run-suite until Task 2 — commit the `.goo`/`.expected.txt` but if the golden runner picks it up and fails, move both files into Task 2's commit instead. Verify with `bash scripts/run_golden.sh` before committing: if it drops below 224/0, the probe isn't runnable yet — defer it to Task 2 and commit only ast.h + parser.y here.)

---

### Task 2: Assertion typecheck + codegen (comma-ok + single-return)

**Files:**
- Modify: `src/types/expression_checker.c` (type_check for `AST_TYPE_ASSERT`)
- Modify: `src/codegen/expression_codegen.c` (single-return codegen for `AST_TYPE_ASSERT`)
- Modify: `src/codegen/function_codegen.c` (comma-ok 2-name short-decl arm, mirror ~1584)
- Create: `examples/type_assert_probe.goo` + `.expected.txt`, `examples/type_assert_ptr_probe.goo` + `.expected.txt`
- Modify: `Makefile` (`typeassert-abort-probe` for single-return miss + `verify:`)

**Interfaces:**
- Consumes: `AST_TYPE_ASSERT` / `TypeAssertNode` (Task 1); `type_interface_satisfied(checker, iface, concrete, &method, &reason)` (type_checker.c:798, returns 1 if concrete implements iface); `codegen_interface_vtable(codegen, checker, iface, concrete)` (interface_codegen.c:178, returns the vtable global); interface value layout `{vtable, data}` extracted via `LLVMBuildExtractValue(iface_val, 0/1)` (see codegen_interface_dispatch interface_codegen.c:254-255); `goo_panic`; the comma-ok map-read pattern at function_codegen.c:1584-1640.
- Produces: a working `x.(T)` in both forms — Task 3's type switch reuses the same vtable-compare + extract lowering.

- [ ] **Step 1: Typecheck — resolve target, static rejects, result type**

In `expression_checker.c`'s `type_check_expression`, add an `AST_TYPE_ASSERT` case. Resolve the operand type and the asserted type; enforce:
- operand must be `TYPE_INTERFACE`, else `type_error(... "invalid type assertion: operand is not an interface type")`.
- asserted target resolved via the file's type-node resolver (`type_from_ast` — same helper Task-2 of the stdlib branch used for slice conversions; grep `type_from_ast` for the exact name).
- if the target is itself `TYPE_INTERFACE`: `type_error(... "type assertion to an interface type is not supported in v1 (concrete target types only)")`.
- else (concrete target): call `type_interface_satisfied(checker, operand_type, target_type, &method, &reason)`; if 0, `type_error(... "impossible type assertion: %s does not implement %s (%s method %s)", ...)`.
- set `expr->node_type = target_type` (the single-value result type; comma-ok's bool second value is synthesized at the assignment site, mirroring the map read).

Store the resolved operand interface type where codegen can retrieve it (the operand node's `node_type` already carries it after `type_check_expression(conv->expr)` — codegen re-reads `ta->expr->node_type`).

- [ ] **Step 2: Codegen — single-return (panic on miss)**

In `expression_codegen.c`'s dispatch, add an `AST_TYPE_ASSERT` case (single-value context). Generate:
- the operand interface value; `iface_type = ta->expr->node_type` (TYPE_INTERFACE), `target = expr->node_type`.
- `vt_want = codegen_interface_vtable(codegen, checker, iface_type, target)`.
- `vt_have = LLVMBuildExtractValue(builder, iface_val, 0, "ta.vt")`.
- `match = LLVMBuildICmp(builder, LLVMIntEQ, vt_have, vt_want, "ta.match")`.
- conditional branch: match block loads the concrete (`data = ExtractValue 1`; `LLVMBuildLoad2` of `target`'s LLVM type from `data`); miss block calls `goo_panic` with a string constant `"interface conversion: <I> is not <T>"` (build the message with the static type names via `type_to_string`), then unreachable. Return the loaded value (the match block is the continuation).

Follow the branch/phi idiom already used for the map zero-guard (codegen.c:574-601 region) for block structure.

- [ ] **Step 3: Codegen — comma-ok (2-name short-decl)**

In `function_codegen.c`, beside the comma-ok map-read arm (~1584, `var_decl->name_count == 2 && var_decl->is_short_decl && var_decl->values->type == AST_INDEX_EXPR`), add a sibling for `AST_TYPE_ASSERT`:
- compute `match` exactly as Step 2 (vtable compare).
- `v` = branch+phi between the loaded target (match) and `LLVMConstNull(target_llvm)` / `zeroinitializer` (miss) — no panic.
- pack `{v, ok}` into the `{V, i1}` aggregate the generic ExtractValue loop binds to `name0`→v, `name1`→ok, exactly as the map arm does (function_codegen.c:1634+).

- [ ] **Step 4: Goldens + abort probe**

`examples/type_assert_probe.goo` — comma-ok hit + miss + single-return hit + zero-value-on-miss (go-run-verify):

```go
package main

import "fmt"

type Animal interface {
	Sound() string
}

type Dog struct {
	Name string
}

func (d Dog) Sound() string {
	return "woof"
}

type Cat struct {
	Lives int
}

func (c Cat) Sound() string {
	return "meow"
}

func main() {
	var a Animal = Dog{Name: "Rex"}
	d, ok := a.(Dog)
	fmt.Println(ok)
	fmt.Println(d.Name)
	c, ok2 := a.(Cat)
	fmt.Println(ok2)
	fmt.Println(c.Lives)
	d2 := a.(Dog)
	fmt.Println(d2.Sound())
}
```

`.expected.txt`:

```
true
Rex
false
0
woof
```

`examples/type_assert_ptr_probe.goo` — pointer-concrete target composing with #113 (go-run-verify):

```go
package main

import "fmt"

type Counter interface {
	Get() int
}

type Box struct {
	N int
}

func (b *Box) Get() int {
	return b.N
}

func main() {
	b := Box{N: 5}
	var c Counter = &b
	p, ok := c.(*Box)
	fmt.Println(ok)
	p.N = 9
	fmt.Println(b.N)
}
```

`.expected.txt`:

```
true
9
```

`Makefile` `typeassert-abort-probe` (single-return miss panics — mirror `map-nilfunc-abort-probe` at Makefile:1426):

```makefile
typeassert-abort-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== typeassert-abort-probe: failed single-return x.(T) must panic ==="
	@printf 'package main\ntype A interface {\n\tM() int\n}\ntype X struct{ V int }\nfunc (x X) M() int { return x.V }\ntype Y struct{ V int }\nfunc (y Y) M() int { return y.V }\nfunc main() {\n\tvar a A = X{V: 1}\n\t_ = a.(Y)\n}\n' > build/typeassert_abort.goo
	@"$(COMPILER)" build/typeassert_abort.goo -o build/typeassert_abort.out 2>build/typeassert_abort.cerr || \
	  { echo "typeassert-abort-probe: FAIL (compile)"; cat build/typeassert_abort.cerr; exit 1; }
	@./build/typeassert_abort.out 2>build/typeassert_abort.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "typeassert-abort-probe: FAIL (bad assert did not abort)"; exit 1; fi; \
	if ! grep -q "interface conversion" build/typeassert_abort.err; then echo "typeassert-abort-probe: FAIL (no conversion panic message)"; cat build/typeassert_abort.err; exit 1; fi
	@echo "typeassert-abort-probe: PASS"
```

Append `typeassert-abort-probe` to the `verify:` prerequisite list.

- [ ] **Step 5: Gates + commit**

```bash
make lexer                                   # exit 0
bin/goo -o build/tap examples/type_assert_probe.goo && ./build/tap    # matches expected
bin/goo -o build/tapp examples/type_assert_ptr_probe.goo && ./build/tapp
make typeassert-abort-probe                  # PASS
bash scripts/run_golden.sh                   # +2 or +3 (incl. Task 1's held probe) — count grows, 0 failed
make test                                    # 76/1
```

```bash
git add src/types/expression_checker.c src/codegen/expression_codegen.c src/codegen/function_codegen.c \
        examples/type_assert_probe.* examples/type_assert_ptr_probe.* Makefile
# include examples/type_assert_parse_probe.* here IF it was deferred from Task 1
git commit --no-gpg-sign -m "feat(iface): x.(T) type assertions — comma-ok + single-return via vtable-ptr compare"
```

---

### Task 3: Type switch

**Files:**
- Modify: `include/ast.h` (TAIL: `AST_TYPE_SWITCH` + `TypeSwitchNode`, `AST_TYPE_CASE` + `TypeCaseNode` — or reuse case-clause with a type-list; pick and document)
- Modify: `src/parser/parser.y` (`switch_stmt` type-switch arm + `type_switch_guard` + `type_case_list`)
- Modify: `src/types/*` (typecheck: per-case satisfaction, bound-var typing, dup/default checks)
- Modify: `src/codegen/statement_codegen.c` (desugar to the vtable-compare chain)
- Create: `examples/type_switch_probe.goo` + `.expected.txt`, `examples/type_switch_fmt_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: the vtable-compare + extract lowering from Task 2 (factor it into a reusable helper in Task 2 if clean, else replicate); `type_interface_satisfied`; `codegen_interface_vtable`.
- Produces: `switch v := x.(type) { case … }` working.

- [ ] **Step 1: Confirm baseline + write goldens**

`./scripts/grammar-tripwire.sh` → PASS. Then `examples/type_switch_probe.goo` (bound var, multi-arm, multi-type case, default, nil — go-run-verify):

```go
package main

import "fmt"

type Shape interface {
	Area() int
}

type Sq struct {
	S int
}

func (s Sq) Area() int {
	return s.S * s.S
}

type Rect struct {
	W int
	H int
}

func (r Rect) Area() int {
	return r.W * r.H
}

type Circle struct {
	R int
}

func (c Circle) Area() int {
	return 3 * c.R * c.R
}

func describe(s Shape) string {
	switch v := s.(type) {
	case Sq:
		return fmt.Sprintf("square %d", v.S)
	case Rect:
		return fmt.Sprintf("rect %d", v.W*v.H)
	case Sq, Circle:
		return "sq-or-circle"
	default:
		return "other"
	}
}

func main() {
	fmt.Println(describe(Sq{S: 3}))
	fmt.Println(describe(Rect{W: 2, H: 4}))
	fmt.Println(describe(Circle{R: 2}))
}
```

`.expected.txt` (verify with go run — note `Sq` matches the first arm, so the multi-type `Sq, Circle` arm is reached only by Circle):

```
square 3
rect 8
sq-or-circle
```

`examples/type_switch_fmt_probe.goo` — the fmt-shaped motivating case over a heterogeneous slice (go-run-verify):

```go
package main

import "fmt"

func stringify(vals []interface{}) {
	for _, x := range vals {
		switch v := x.(type) {
		case int:
			fmt.Println(v + 1)
		case string:
			fmt.Println(v)
		case bool:
			fmt.Println(v)
		default:
			fmt.Println("?")
		}
	}
}

func main() {
	stringify([]interface{}{1, "hi", true})
}
```

`.expected.txt`:

```
2
hi
true
```

NOTE: if `[]interface{}` or `interface{}` literals aren't fully supported, this probe may surface a pre-existing gap — if so, RECORD it (record-don't-absorb) and substitute a named single-method interface with concrete implementers, keeping the int/string/bool spirit via distinct concrete types. Confirm which path you took in the report.

- [ ] **Step 2: AST nodes + grammar**

Add `AST_TYPE_SWITCH` + `TypeSwitchNode { ASTNode base; ASTNode* bind_name /* IdentifierNode or NULL */; ASTNode* expr; ASTNode* cases; }` and a type-case representation (`TypeCaseNode { ASTNode base; ASTNode* types /* type-list, NULL = default */; ASTNode* body; ASTNode* next; }`) at the ast.h tail; `make clean && make lexer`.

Grammar (`switch_stmt` arm + helpers), matching the spec:

```yacc
    | SWITCH type_switch_guard LBRACE_BODY type_case_list RBRACE { /* build TypeSwitchNode */ }
    | SWITCH type_switch_guard LBRACE type_case_list RBRACE { /* same (fallback) */ }
```

with `type_switch_guard : primary_expr DOT LPAREN TYPE RPAREN` and `identifier SHORT_ASSIGN primary_expr DOT LPAREN TYPE RPAREN`, and `type_case_list`/`type_case_clause` (`CASE type_list COLON statement_list` | `DEFAULT COLON statement_list`), `type_list : type | type_list COMMA type`.

Tripwire after: PASS 81/256, or STOP with `-Wcounterexamples`. This is the batch's highest-risk grammar change (shared SWITCH prefix); the guard's `DOT LPAREN TYPE RPAREN` tail must commit the parse before any CASE — if bison reports a conflict in the SWITCH states, follow the justified-delta procedure, do not absorb.

- [ ] **Step 3: Typecheck**

Operand must be interface; each case type must be concrete and satisfy the operand interface (`type_interface_satisfied`), else the same "impossible" / "not supported for interface target" errors as Task 2; reject duplicate case types and >1 default. Bound var `v`: type = the case type in a single-type case, the operand interface type in a multi-type/default case (introduce the binding into each case body's scope with the right type).

- [ ] **Step 4: Codegen desugar**

In `statement_codegen.c`, lower the type switch to an ordered chain: extract the operand's vtable once, then for each case type emit `icmp eq vt, &goo.vtable.Ti.I` (reuse Task 2's compare); on match, bind `v` (load the concrete for single-type cases; keep the interface value for multi-type/default) and run the body, then branch to the switch end. `case nil:` compares the whole interface value to a zero interface. `default` is the fall-through arm.

- [ ] **Step 5: Gates + commit**

```bash
make lexer                                   # exit 0
./scripts/grammar-tripwire.sh                # PASS 81/256
bin/goo -o build/ts examples/type_switch_probe.goo && ./build/ts
bin/goo -o build/tsf examples/type_switch_fmt_probe.goo && ./build/tsf
bash scripts/run_golden.sh                   # count grows, 0 failed
make test                                    # 76/1
```

```bash
git add include/ast.h src/parser/parser.y src/types/ src/codegen/statement_codegen.c examples/type_switch_*
git commit --no-gpg-sign -m "feat(iface): type switch — switch v := x.(type) over concrete cases"
```

---

### Task 4: Reject-probe sweep + full sweep + handoff

**Files:**
- Modify: `Makefile` (reject probes + `verify:`)
- Modify: `.handoff.md`

**Interfaces:** consumes Tasks 1-3; produces the branch's ship gates.

- [ ] **Step 1: Reject probes**

Add to the Makefile (each compile-must-fail + message grep, mirroring existing `*-reject-probe` shape) and to `verify:`:
- `typeassert-reject-probe`: bundles (a) `x.(T)` on a non-interface operand → "not an interface type"; (b) impossible assertion (`T` doesn't implement `I`) → "impossible type assertion"; (c) assert-to-interface target → "not supported in v1".
- `typeswitch-reject-probe`: (a) duplicate case type → dup error; (b) two `default`s → default error.

Run each: `make typeassert-reject-probe typeswitch-reject-probe` → PASS.

- [ ] **Step 2: Full sweep (real exit codes)**

```bash
make clean && make lexer                     # exit 0
make test                                    # 76/1
bash scripts/run_golden.sh                   # final count, 0 failed
eval "$(opam env --switch=default)"
make verify                                  # ALL GREEN GATES PASSED
make ccomp-link                              # PASS
./scripts/grammar-tripwire.sh                # PASS (81 S/R + 256 R/R)
```

- [ ] **Step 3: Update `.handoff.md`**

Mark type assertions + type switches SHIPPED (this branch): concrete targets, all three forms, vtable-ptr identity. Record: assert-to-interface deferred (needs RTTI); single-return panic message is static-names-only (no dynamic-type name); any `[]interface{}`/`interface{}`-literal gap found in Task 3. Promote next queue head (non-string map keys). Keep other queue items intact.

- [ ] **Step 4: Commit**

```bash
git add Makefile .handoff.md
git commit --no-gpg-sign -m "test(iface): type-assertion/switch reject probes; sweep; update handoff"
```

After Task 4: push, PR, fresh-context whole-branch review before merge (mandatory).

---

## Execution notes (controller)

- SDD economy mode: Sonnet implementers, controller (main loop) review + independent differential probes between tasks.
- T1 and T3 touch the grammar — the tripwire stop-rule is non-negotiable; T3 is the highest-risk (shared SWITCH prefix). Front-load `-Wcounterexamples` there.
- Task 1's golden-deferral note (Step 6): if the parse probe isn't runnable until Task 2's codegen lands, commit only ast.h+parser.y in T1 and move the golden into T2 — never commit a golden that makes `run_golden.sh` drop below 224/0.
- T2 Step 2/3 should factor the vtable-compare-and-extract into one helper so T3 reuses it rather than replicating — flag in the T3 brief.
- Reviewers: T2's reviewer should confirm the comma-ok miss returns a real zero value (not garbage) and the single-return miss actually panics (not UB); T3's should check bound-var typing (concrete in single-type case, interface in multi/default) and that `case nil` matches a nil interface.
