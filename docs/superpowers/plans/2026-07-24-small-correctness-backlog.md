# Small-Correctness-Backlog Arc Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Clear the five ledgered small-correctness items (empty case bodies, builtin shadowing, sync/time own-import seeding, slice-index-as-method-arg miscompile, `&*p` fold) in three layer-scoped PRs.

**Architecture:** Three sequential PRs — grammar (parser.y case-body epsilon), type checker + codegen shadow gates and seeding move, codegen argument-load + address-of fold. Every fix is fixture-first and exercised by `make verify-core`; grammar risk is isolated to PR 1 under the tripwire.

**Tech Stack:** C23, bison (LALR), LLVM-C API, bash probe scripts, Makefile gates.

**Spec:** `docs/superpowers/specs/2026-07-24-small-correctness-backlog-design.md` (including its "Execution amendments" section — three plan-time discoveries: switch/type-switch share the empty-body root cause; the shadow gate must land in checker AND codegen; only `far_collective_probe.goo` has actual bind-to-local workarounds).

## Global Constraints

- Commit form (1Password signing agent is broken here): `git -c commit.gpgsign=false commit -m "..."` — every commit, and every commit message ends with the trailer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- Conventional commits, imperative mood: `fix:`, `feat:`, `test:`, `docs:`.
- Gates before each PR: `make lexer && make test && make verify-core` all green.
- PR 1 additionally: `bash scripts/grammar-tripwire.sh` must PASS (31 S/R / 0 R/R exact) before AND after; any delta is stop-the-line → the goo-grammar skill's justified-delta procedure (`.claude/skills/goo-grammar/references/conflict-ledger.md`): classify counterexamples, update `EXPECTED_SR`/`EXPECTED_RR` + ledger entry in the same commit. Any `src/parser/parser.y` edit requires the goo-grammar skill loaded first.
- Accept fixtures: `examples/<name>.goo` + `examples/<name>.expected.txt` (auto-discovered by `make test-golden` / `test-golden-o2`). Reject fixtures: `tests/golden/reject/<name>.goo` + `<name>.err.txt` (stderr substring).
- Branch per PR off up-to-date main; merge with `gh pr merge --merge` once green (merge authority is delegated on green local gates — the repo has no cloud CI by open decision); post-merge `make verify-core` on main before the next PR starts.
- TDD: record the RED (failing) output in the task log before implementing. Fixture and fix are committed together (a committed red fixture would break `make test-golden` for bisection).
- Do NOT touch `src/parser/lexer_bridge.c` or lexer token emission in this arc.

---

## PR 1 — grammar: empty case bodies (branch `fix/empty-case-bodies`)

### Task 1: RED fixtures for empty case bodies

**Files:**
- Create: `examples/empty_case_body_probe.goo`
- Create: `examples/empty_case_body_probe.expected.txt`

**Interfaces:**
- Produces: the golden fixture Task 2 must turn green.

- [ ] **Step 1: Check the type-switch idiom used by the existing spec fixture**

Run: `grep -n "switch" tests/spec/stmt_switch_init_type.goo | head` and read the file. Use the same `interface{}`/`any` + `switch v.(type)` spelling it uses in the fixture below (adjust the type-switch block's syntax to match, keeping the empty-body shape).

- [ ] **Step 2: Write the fixture**

`examples/empty_case_body_probe.goo`:
```go
// Empty case bodies (Go parity): select `default:`/`case:`, expression
// switch, and type switch all accept an empty statement list. Grammar-level
// fix — all three families route through the same statement_list nonterminal.
package main

import "fmt"

func classify(n int) string {
	switch n {
	case 0:
	case 1:
		return "one"
	default:
	}
	return "other"
}

func main() {
	fmt.Println(classify(0))
	fmt.Println(classify(1))
	fmt.Println(classify(2))

	var v interface{} = 3
	switch v.(type) {
	case int:
	default:
		fmt.Println("not int")
	}
	fmt.Println("typeswitch ok")

	ch := make(chan int, 1)
	select {
	case <-ch:
		fmt.Println("recv wrong")
	default:
	}
	ch <- 7
	select {
	case <-ch:
	default:
		fmt.Println("default wrong")
	}
	fmt.Println("select ok")
}
```

`examples/empty_case_body_probe.expected.txt`:
```
other
one
other
typeswitch ok
select ok
```

- [ ] **Step 3: Record RED**

Run: `make lexer && bin/goo examples/empty_case_body_probe.goo -o /tmp/ecb 2>&1 | head -5`
Expected: `Parse error ... syntax error` (exit non-zero). Record the exact output. Do NOT commit yet.

### Task 2: grammar fix — scoped case-body nonterminal

**Files:**
- Modify: `src/parser/parser.y` (select_case at 1539–1592, case_clause at 1810–1817, type_case_clause at ~1752–1758)
- Possibly modify: `scripts/grammar-tripwire.sh` + `.claude/skills/goo-grammar/references/conflict-ledger.md` (ONLY via the justified-delta procedure)

**Interfaces:**
- Consumes: Task 1's fixture.
- Produces: `case_body` nonterminal; case/select-case AST nodes whose body may be NULL — Task 3 and all downstream consumers rely on NULL meaning "empty body, no-op".

- [ ] **Step 1: Load the goo-grammar skill** (mandatory before any parser.y edit) and run `bash scripts/grammar-tripwire.sh` — must print `PASS (31 S/R + 0 R/R — baseline exact)`.

- [ ] **Step 2: Add the scoped nonterminal and swap it in**

Add (near statement_list, ~line 1063):
```
/* Case bodies (select/switch/type-switch) may be empty (Go parity).
   Scoped epsilon: statement_list itself stays epsilon-free — it is shared
   by if/for/block and an epsilon there would have grammar-wide blast
   radius. NULL body = empty, every consumer treats it as a no-op. */
case_body:
      statement_list { $$ = $1; }
    | %empty         { $$ = NULL; }
    ;
```
(If parser.y doesn't already use `%empty`, use the bare-alternative spelling `| { $$ = NULL; }` matching local convention.) Declare `%type <node> case_body` next to the existing `%type` lines (186–187).

Then replace `statement_list` with `case_body` in ALL body positions of: the five `select_case` alternatives (lines 1540, 1544, 1555, 1566, 1582), both `case_clause` alternatives (1811, 1814), and both `type_case_clause` alternatives (~1752–1758). Do not touch any other rule.

- [ ] **Step 3: Tripwire**

Run: `bash scripts/grammar-tripwire.sh`
Expected: PASS at 31/0. If a delta: STOP — run the goo-grammar conflict-ledger justified-delta procedure (classify each new conflict with `bison -Wcounterexamples`, decide benign/harmful; if benign, update `EXPECTED_SR`/`EXPECTED_RR` and add the ledger entry in the same commit; if harmful, fall back to the `block`-template alternative: duplicate each case alternative with the body elided, e.g. `| DEFAULT COLON { ... body NULL ... }`, and re-measure).

- [ ] **Step 4: NULL-body tolerance in consumers**

Run: `grep -rn "cases" src/types/*.c src/codegen/*.c | grep -i "select\|case_clause\|SwitchStmt" | head -30` and inspect every loop that walks a `SelectCaseNode`/case-clause `body`. Each must tolerate `body == NULL` (skip statement checking/emission, still perform the case's match/recv semantics: an empty matched switch case does NOT fall through; an empty select arm still completes the recv; an empty `default:` still makes the select non-blocking). Add `if (node->body)` guards where a NULL would crash or mis-lower.

- [ ] **Step 5: GREEN**

Run: `make lexer && bin/goo examples/empty_case_body_probe.goo -o /tmp/ecb && /tmp/ecb`
Expected: output matches `empty_case_body_probe.expected.txt` exactly.
Then: `make test && make test-golden && make test-golden-o2` — all green.

- [ ] **Step 6: Commit**

```bash
git add src/parser/parser.y examples/empty_case_body_probe.goo examples/empty_case_body_probe.expected.txt
git -c commit.gpgsign=false commit -m "fix(parser): allow empty case bodies in select/switch/type-switch (Go parity)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```
(Include tripwire/ledger files in the same commit if the justified-delta path was taken; include any consumer NULL-guard files.)

### Task 3: remove the lanes.go workarounds (structural permanence)

**Files:**
- Modify: `goostd/lanes/lanes.go:860-869` and `:905-912`

- [ ] **Step 1: Replace both workaround arms with bare `default:`**

At lines 863–868 (the `sendL` drain select) delete the four comment lines and the `_ = 0`, leaving:
```go
	select {
	case v := <-sendL:
		far.SendF64(sockL, v)
	default:
	}
```
Same at 908–911 for the `sendR` mirror.

- [ ] **Step 2: Verify the far gates still pass**

Run: `make verify-core 2>&1 | tail -20`
Expected: ALL GREEN (the seven far gates now exercise the empty-default production on every run).

- [ ] **Step 3: Commit**

```bash
git add goostd/lanes/lanes.go
git -c commit.gpgsign=false commit -m "refactor(lanes): drop the _ = 0 empty-default workarounds

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task 4: spec-conformance rows

**Files:**
- Create: `tests/spec/stmt_switch_empty_case.goo` + `.expected.txt`
- Create: `tests/spec/stmt_select_empty_case.goo` + `.expected.txt`
- Modify: `tests/spec/manifest.tsv` (near line 44), regenerate `docs/GO_SPEC_CONFORMANCE.md`

- [ ] **Step 1: Write the two spec fixtures** — split `examples/empty_case_body_probe.goo` into a switch-only fixture (the `classify` function + type-switch block; expected `other`/`one`/`other`/`typeswitch ok`) and a select-only fixture (the two select blocks; expected `select ok`). Same code, trimmed per file.

- [ ] **Step 2: Add manifest rows** (tab-separated, matching line 41/44's format):
```
stmt_switch_empty_case	run	works	Statements	empty case/default bodies in expression + type switch
stmt_select_empty_case	run	works	Statements	empty case/default bodies in select
```

- [ ] **Step 3: Regenerate and verify**

Run: `make spec-conformance && git diff --stat docs/GO_SPEC_CONFORMANCE.md`
Expected: the Statements row's works-count rises by 2. Run whatever runner `make spec-conformance` uses to confirm both new rows PASS.

- [ ] **Step 4: Commit**

```bash
git add tests/spec/ docs/GO_SPEC_CONFORMANCE.md
git -c commit.gpgsign=false commit -m "test(spec): conformance rows for empty case bodies

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task 5: PR 1 gates and merge

- [ ] **Step 1:** `make lexer && make test && make verify-core` — ALL GREEN; `bash scripts/grammar-tripwire.sh` PASS.
- [ ] **Step 2:** Update `.handoff.md`: move the empty-`default:` item from "Small ledgered follow-ups" into the burndown ledger table (`| — | empty select/switch case bodies no-parse | this arc, PR #N |`). Commit as `docs(handoff): ...`.
- [ ] **Step 3:** Push branch, open PR (`gh pr create`), title `fix(parser): empty case bodies in select/switch/type-switch (Go parity)`; body summarizes fixture + tripwire result + lanes workaround removal; merge with `gh pr merge --merge` when green; run post-merge `make verify-core` on main.

---

## PR 2 — checker: builtin shadowing + sync/time seeding (branch `fix/builtin-shadow-seeding`)

### Task 6: RED fixture — builtin shadowing

**Files:**
- Create: `examples/builtin_shadow_probe.goo` + `.expected.txt`

- [ ] **Step 1: Write the fixture**

```go
// Go parity: predeclared identifiers (len/cap/min/...) are shadowable.
// Builtin dispatch currently fires on identifier TEXT before scope lookup
// in BOTH the checker and codegen — a user len/cap/min must win here.
package main

import "fmt"

func len(s string) int { return 42 }
func cap(n int) int { return n + 100 }
func min(a int, b int) int { return a * b }

func main() {
	fmt.Println(len("abc"))
	fmt.Println(cap(1))
	fmt.Println(min(3, 4))
	xs := []int{1, 2, 3}
	fmt.Println(append(xs, 4)[3])
}
```

`.expected.txt`:
```
42
101
12
4
```
(The `append` line is the unshadowed-regression control in the same fixture; the wider golden suite is the full regression net.)

- [ ] **Step 2: Record RED**

Run: `bin/goo examples/builtin_shadow_probe.goo -o /tmp/bs && /tmp/bs`
Expected: type error, wrong output, or miscompile (the builtin intercepts). Record exactly what happens. Do NOT commit yet.

### Task 7: checker gate

**Files:**
- Modify: `src/types/expression_checker.c` — the strcmp arms at lines 3752 (`append`), 3821 (`copy`), 3850 (`len`), 3870 (`cap`), 3894 (`close`), 3928 (`delete`), 3984 (`clear`), 4006 (`min`), 4009 (`max`)

**Interfaces:**
- Consumes: `name_is_user_shadowed(TypeChecker*, const char*)` at `expression_checker.c:2522` (checks `scope_lookup_variable` + comptime top-level funcs).
- Produces: a shadowed builtin call falls through to line 4062's ordinary `type_check_expression(checker, call->function)` resolution.

- [ ] **Step 1: Add the gate to all nine arms** — mechanical, one pattern (shown for `len`; repeat for each listed line):

```c
        if (strcmp(func_ident->name, "len") == 0 &&
            !name_is_user_shadowed(checker, func_ident->name)) {
```
Scope note: `new`/`make`/`make_chan`/`recover` are NOT gated this arc (matches the handoff's lockstep list; `error` already has its own gate at 4021).

- [ ] **Step 2: Build + partial check**

Run: `make lexer && bin/goo examples/builtin_shadow_probe.goo -o /tmp/bs && /tmp/bs`
Expected: either GREEN already, or an LLVM-level failure/wrong output proving codegen's independent dispatch (Task 8) is now the blocker. Record which.

### Task 8: codegen gate (mirror)

**Files:**
- Modify: `src/codegen/call_codegen.c` — the arms at lines 814 (`len`), 859 (`close`), 893 (`delete`), 931 (`clear`), 968 (`cap`), 992 (`append`), 1084 (`copy`), 1138 (`min`/`max`)

**Interfaces:**
- Consumes: the existing mirror pattern at `call_codegen.c:518-519`/`612-613` (`!type_checker_lookup_variable(checker, func_name->name)` guarding `string`/conversions).

- [ ] **Step 1: Add the mirrored guard to all eight arms** (shown for `len`):

```c
    if (strcmp(func_name->name, "len") == 0 && call->args
        && !type_checker_lookup_variable(checker, func_name->name)) {
```

- [ ] **Step 2: GREEN + contingency**

Run: `make lexer && bin/goo examples/builtin_shadow_probe.goo -o /tmp/bs && /tmp/bs && diff <(/tmp/bs) examples/builtin_shadow_probe.expected.txt`
Expected: exact match. CONTINGENCY: if the top-level-func shadows (len/cap/min are funcs, not vars) still hit the builtin arms, `type_checker_lookup_variable` isn't seeing top-level funcs at codegen time — add a small static helper in call_codegen.c that ALSO consults the comptime func registry exactly as `name_is_user_shadowed` does (same two lookups), and use it in all eight arms AND retrofit it to the 518/612 conversion arms so the two families can't drift.

- [ ] **Step 3: Full suites**

Run: `make test && make test-golden && make test-golden-o2` — green (proves unshadowed builtins everywhere still lower correctly).

- [ ] **Step 4: Commit**

```bash
git add src/types/expression_checker.c src/codegen/call_codegen.c examples/builtin_shadow_probe.goo examples/builtin_shadow_probe.expected.txt
git -c commit.gpgsign=false commit -m "fix(types,codegen): user-shadowed builtins dispatch to the user symbol

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task 9: RED fixture — sync/time own-import seeding

**Files:**
- Create: `examples/syncimport/` local package (mirror `examples/p4pkg/`'s file extension and layout exactly — check `ls examples/p4pkg/` first)
- Create: `examples/sync_own_import_probe.goo` + `.expected.txt`

- [ ] **Step 1: Write the package** (`examples/syncimport/syncimport.go`, extension per p4pkg):

```go
// A local source package whose OWN import list names sync and time.
// Its probe main deliberately does NOT import sync/time — resolution
// must come from this package's own imports (M2-B1 no-masking rule).
package syncimport

import "sync"
import "time"

var mu sync.Mutex
var count int

func Bump() {
	mu.Lock()
	count = count + 1
	mu.Unlock()
}

func Count() int { return count }

func Nap() {
	time.Sleep(time.Millisecond)
}
```

- [ ] **Step 2: Write the probe** (`examples/sync_own_import_probe.goo`):

```go
package main

import "fmt"
import "./syncimport"

func main() {
	syncimport.Bump()
	syncimport.Bump()
	syncimport.Nap()
	fmt.Println(syncimport.Count())
}
```
`.expected.txt`: `2`

- [ ] **Step 3: Record RED**

Run: `bin/goo examples/sync_own_import_probe.goo -o /tmp/si 2>&1 | head -5`
Expected: unresolved `sync`/`time` inside the package. Record it. Do NOT commit yet.

### Task 10: move the seeders and wire the vendored path

**Files:**
- Modify: `src/compiler/goo.c` (remove lines ~612–862's sync/time seeding statics; keep `seed_imported_stdlib_markers` calling the moved functions)
- Modify: `src/types/type_checker.c` (receive the moved code near `seed_package_own_shim_imports:859`; extend that function)
- Modify: `include/types.h` (two new prototypes)
- Modify: the stale doc comment at `type_checker.c:828-837` (it documents the exclusion this task removes)

**Interfaces:**
- Produces: `void seed_sync_package_exports(TypeChecker* checker, Package* pkg);` and `void seed_time_package_exports(TypeChecker* checker, Package* pkg);` — non-static, declared in `include/types.h`, called from BOTH `goo.c`'s `seed_imported_stdlib_markers` (main path, unchanged behavior) and `type_checker.c`'s `seed_package_own_shim_imports` (new).

- [ ] **Step 1: Move code bodily** from `goo.c` to `type_checker.c`: `sync_make_opaque_struct` (612–639), `sync_export_type` (641–652), `sync_export_method` (654–683), `seed_sync_package_exports` (689–707), `time_make_time_struct` (719–736), `time_make_duration_type` (738–760), `time_export_type` (766–772), `time_export_value` (779–784), `time_export_func` (790–798), `time_export_method` (806–824), `seed_time_package_exports` (834–862). Helpers stay `static`; the two `seed_*` entry points become non-static. All dependencies (`type_new`, `type_pointer`, `type_copy`, `type_function`, `variable_new`, `scope_add_variable`, `type_checker_get_builtin`, `type_method_mangled_name`) are already declared in `include/types.h`, and `type_checker.c` has its own `str_dup` (line 10) — the moved code must use it (delete goo.c's copy only if now unused).

- [ ] **Step 2: Add idempotence guard** at the top of each moved `seed_*_package_exports`: if the package already exports its anchor symbol (`Mutex` for sync, `Duration` for time — use the same lookup mechanism `sync_export_type` writes through), return immediately. This prevents duplicate exports when main AND a vendored package both import the same shim.

- [ ] **Step 3: Declare in `include/types.h`** next to `type_check_package` (line 1063):
```c
void seed_sync_package_exports(TypeChecker* checker, Package* pkg);
void seed_time_package_exports(TypeChecker* checker, Package* pkg);
```

- [ ] **Step 4: Extend `seed_package_own_shim_imports`** (`type_checker.c:859`) — before the plain-shim `continue`-filter, add:
```c
        if (spec->path && (strcmp(spec->path, "sync") == 0 ||
                           strcmp(spec->path, "time") == 0)) {
            const char* short_name = spec->alias ? spec->alias : spec->path;
            Package* p = type_checker_add_package(checker, spec->path, short_name);
            if (!p) return false;
            if (strcmp(spec->path, "sync") == 0)
                seed_sync_package_exports(checker, p);
            else
                seed_time_package_exports(checker, p);
            type_checker_seed_package_marker(checker, short_name, p);
            continue;
        }
```
Rewrite the 828–837 doc comment: the exclusion is gone; note the seeders now live here and both paths share them.

- [ ] **Step 5: GREEN**

Run: `make lexer && bin/goo examples/sync_own_import_probe.goo -o /tmp/si && /tmp/si`
Expected: `2`. Then `make test && make test-golden && make verify-core` (verify-core includes `stdlib-smoke-coverage`, which greps `sync_export_method`/`time_export_*` call sites — it scans by pattern, so confirm it still finds them at their new location; if it hardcodes `goo.c` as the file, update `scripts/check_stdlib_coverage.sh`'s path in the same commit).

- [ ] **Step 6: Commit**

```bash
git add src/compiler/goo.c src/types/type_checker.c include/types.h scripts/check_stdlib_coverage.sh examples/syncimport examples/sync_own_import_probe.goo examples/sync_own_import_probe.expected.txt
git -c commit.gpgsign=false commit -m "fix(types): vendored packages' own sync/time imports now seed their scope

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task 11: PR 2 gates and merge

- [ ] **Step 1:** `make lexer && make test && make verify-core` — ALL GREEN.
- [ ] **Step 2:** `.handoff.md`: burndown rows for both items; delete the now-stale "known asymmetry" paragraph in the M2-B1 Task-0 note (lines ~132–138) and the builtin-shadowing gap paragraph (~158–164). Commit `docs(handoff): ...`.
- [ ] **Step 3:** Push, `gh pr create` (title `fix(types,codegen): builtin shadowing gate + sync/time own-import seeding`), merge on green, post-merge `make verify-core` on main.

---

## PR 3 — codegen: slice-index method arg + `&*p` fold (branch `fix/method-arg-load-nil-fold`)

### Task 12: RED fixture — slice-index as method argument

**Files:**
- Create: `examples/method_sliceidx_arg_probe.goo` + `.expected.txt`

- [ ] **Step 1: Write the fixture**

```go
// A slice-index expression passed DIRECTLY as a method-call argument used
// to fail LLVM module verification ("Call parameter type does not match
// function signature!") — the method-arg loop never loaded the element
// GEP. Plain calls with the same argument shape were unaffected.
package main

import "fmt"

type Acc struct{ total int }

func (a *Acc) AddP(n int) { a.total = a.total + n }
func (a Acc) SumV(n int) int { return a.total + n }

func plain(n int) int { return n + 1 }

func main() {
	xs := []int{10, 20, 30}
	a := Acc{}
	a.AddP(xs[1])
	fmt.Println(a.total)
	fmt.Println(a.SumV(xs[2]))
	fmt.Println(plain(xs[0]))
}
```
`.expected.txt`:
```
20
50
11
```

- [ ] **Step 2: Record RED**

Run: `bin/goo examples/method_sliceidx_arg_probe.goo -o /tmp/ms 2>&1 | head -5`
Expected: LLVM module verification failure mentioning `Call parameter type does not match function signature!`. Record it. Do NOT commit yet.

### Task 13: load lvalue arguments in the method-call loop

**Files:**
- Modify: `src/codegen/call_codegen.c:1997-2009` (the method-arg loop)

**Interfaces:**
- Consumes: the plain-call path's load-before-use precedent at `call_codegen.c:2307-2321` (commit `2c30703`) and arc-16's shape in `composite_codegen.c:153-166` (commit `ed4ce3e`). Read BOTH before editing and mirror the plain-call guard conditions exactly.

- [ ] **Step 1: Insert the load** between `codegen_generate_expression` and the `codegen_coerce_to_type` call in the loop:

```c
                ValueInfo* av = codegen_generate_expression(codegen, checker, a);
                if (!av) { ok = 0; break; }
                // Load a scalar lvalue argument (e.g. a slice-index GEP)
                // before coercion — mirrors the plain-call path (2c30703)
                // and arc-16's index-lvalue-widen shape.
                if (av->is_lvalue && av->goo_type) {
                    LLVMTypeRef at = codegen_type_to_llvm(codegen, av->goo_type);
                    if (at) {
                        av->llvm_value = LLVMBuildLoad2(codegen->builder, at,
                                                        av->llvm_value, "argld");
                        av->is_lvalue = 0;
                    }
                }
                LLVMValueRef v = av->llvm_value;
```
If the plain-call precedent at 2307–2321 carries extra guards (e.g. it skips aggregate types or already-boxed values), replicate those guards verbatim — the two loops must not drift.

- [ ] **Step 2: GREEN**

Run: `make lexer && bin/goo examples/method_sliceidx_arg_probe.goo -o /tmp/ms && /tmp/ms` → exact expected output. Then `make test && make test-golden && make test-golden-o2`.

- [ ] **Step 3: Commit**

```bash
git add src/codegen/call_codegen.c examples/method_sliceidx_arg_probe.goo examples/method_sliceidx_arg_probe.expected.txt
git -c commit.gpgsign=false commit -m "fix(codegen): load slice-index lvalue arguments in method calls

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task 14: remove far_collective_probe workarounds

**Files:**
- Modify: `examples/far_collective_probe.goo` (lines 34–45 and 78–83)

- [ ] **Step 1:** Replace the near-mode workaround (comment at 34–42, binding at 43) with direct calls:
```go
			s := ctx.AllReduceSum(contrib[ctx.ID()])
			m := ctx.AllReduceMax(contrib[ctx.ID()])
```
Same in the far-mode branch (comment 78–80, binding 81): `contrib[base+ctx.ID()]` inline in both calls. Note: `lanes_allreduce_probe.goo` / `far_jacobi_probe.goo` have NO workarounds (spec amendment #3) — leave them untouched.

- [ ] **Step 2:** `make verify-core 2>&1 | tail -20` — far gates green (the miscompile fix is now structurally exercised).

- [ ] **Step 3: Commit** — `refactor(examples): drop far_collective bind-to-local workarounds` (same commit form).

### Task 15: RED probe cases — `&*p` fold

**Files:**
- Modify: `scripts/nil_deref_probe.sh` (append after the `error_nil_error` case, line ~181)

- [ ] **Step 1: Add two cases**

```sh
# LEGAL Go: &*p folds to p — no nil check fires at the &* site even when
# p is nil, and p is evaluated exactly once. The panic belongs to the
# LATER real deref (next case).
check_ok addr_of_deref_fold 'package main
import "fmt"
func main() {
	var p *int
	q := &*p
	if q == nil {
		fmt.Println("folded")
	}
}
' 'folded'

check_nilpanic addr_of_deref_then_deref 'package main
import "fmt"
func main() {
	var p *int
	q := &*p
	fmt.Println(*q)
}
'
```

- [ ] **Step 2: Record RED**

Run: `bash scripts/nil_deref_probe.sh; echo "exit=$?"`
Expected: `addr_of_deref_fold` FAILS (panics today — nil checks fire at `expression_codegen.c:2806` and `:1132`, and `p` is even evaluated twice). Record it. Do NOT commit yet.

### Task 16: implement the fold

**Files:**
- Modify: `src/codegen/expression_codegen.c` — `codegen_generate_unary_expr` (starts 2741; eager operand evaluation at 2756–2757)

**Interfaces:**
- Consumes: `codegen_generate_expression`, `codegen_type_to_llvm`, `value_info_free` — all already used in this file.

- [ ] **Step 1: Short-circuit BEFORE the eager operand evaluation** (the eager `codegen_generate_expression(codegen, checker, unary->operand)` at 2756 is what recursively fires the star-read nil check at 2806 — the fold must run before it):

```c
    // &*e folds to e (Go parity, ADR 0001 carried Minor): no nil check at
    // the &* site and e is evaluated exactly once. The nil check is not
    // lost — it fires at whatever later dereferences the result.
    if (unary->operator == TOKEN_BIT_AND && unary->operand &&
        unary->operand->type == AST_UNARY_EXPR &&
        ((UnaryExprNode*)unary->operand)->operator == TOKEN_MULTIPLY) {
        ASTNode* inner = ((UnaryExprNode*)unary->operand)->operand;
        ValueInfo* pv = codegen_generate_expression(codegen, checker, inner);
        if (!pv) return NULL;
        if (!pv->goo_type || pv->goo_type->kind != TYPE_POINTER) {
            codegen_error(codegen, expr->pos,
                          "Cannot take address of non-lvalue");
            value_info_free(pv);
            return NULL;
        }
        if (pv->is_lvalue) {
            LLVMTypeRef pt = codegen_type_to_llvm(codegen, pv->goo_type);
            if (pt) {
                pv->llvm_value = LLVMBuildLoad2(codegen->builder, pt,
                                                pv->llvm_value, "ptrld");
                pv->is_lvalue = 0;
            }
        }
        return pv;
    }
```
Match the file's local variable names (`unary` is the cast of `expr` — confirm the actual identifier at 2741–2755 and the exact token enum spellings `TOKEN_BIT_AND`/`TOKEN_MULTIPLY` from the existing switch arms).

- [ ] **Step 2: GREEN + perf guard**

Run: `make lexer && bash scripts/nil_deref_probe.sh && echo PROBE-PASS`
Expected: all 13 cases pass (11 existing + 2 new).
Run: `make lanes-kernel-ir-pin` — PASS (the fold only removes checks; this confirms no vectorization regression).

- [ ] **Step 3: Full suites**

Run: `make test && make test-golden && make test-golden-o2` — green.

- [ ] **Step 4: Commit**

```bash
git add src/codegen/expression_codegen.c scripts/nil_deref_probe.sh
git -c commit.gpgsign=false commit -m "fix(codegen): fold &*p to p — nil check moves to the real deref site

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task 17: PR 3 gates and merge

- [ ] **Step 1:** `make lexer && make test && make verify-core` — ALL GREEN.
- [ ] **Step 2:** `.handoff.md`: burndown rows for both items; strike the `&*p` carried-Minor note inside item 0's paragraph and the slice-index NEW paragraph (~191–199). Update `docs/02-LANGUAGE-SPECIFICATION.md`'s nil-semantics matrix cell for `&*p` if it lists the over-fire divergence. Commit `docs(handoff,spec): ...`.
- [ ] **Step 3:** Push, `gh pr create` (title `fix(codegen): method-call slice-index args + &*p fold-to-p`), merge on green, post-merge `make verify-core` on main.

### Task 18: arc close-out (on main, post-merge)

- [ ] **Step 1:** `.handoff.md` "Where we are" + "Next-arc candidates": all five items cleared; promote diagnostics-quality to candidate #1. Note any NEW divergences discovered during the arc as ledger entries instead of fixing them.
- [ ] **Step 2:** Commit `docs(handoff): session handoff — small-correctness arc complete` directly on main (matches prior handoff commits).
