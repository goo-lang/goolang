# Concrete-Type RTTI on the Empty Interface — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable type assertions (`x.(int)`) and type switches (`switch v := x.(type)`) to **concrete** types on the empty interface (`any` / `interface{}`), by lifting two stale rejections.

**Architecture:** No new machinery. Post-#132 every boxed value carries a per-`(concrete,iface)` vtable global (`goo.vtable.<T>.any`, slot 0 = per-type eq fn, deduped by name → stable address). Vtable-pointer identity already discriminates concrete types for non-empty interfaces; the empty-interface path was rejected under a now-false "identical empty vtable" premise. Removing the two guards routes `any` through the same, spike-proven mechanism. No grammar change, no codegen change.

**Tech Stack:** C23 compiler sources (`src/types/`), LLVM-C codegen (unchanged here), bison grammar (unchanged), goo-run goldens (`examples/*.goo` + `.expected.txt` via `scripts/run_golden.sh`), Makefile reject/panic probes.

## Global Constraints

- Do NOT touch `src/parser/parser.y`, `lexer_bridge.c`, or token emission — no grammar change. The bison tripwire must stay **82 S/R + 256 R/R** (`./scripts/grammar-tripwire.sh`), though with no grammar edit it is unaffected.
- Build with `make lexer` (exit 0). No header (`include/*.h`) is edited, so **no `make clean`** is required.
- Golden expected output is produced by `go run` on an equivalent program, **never hand-written**. The repo dir has `.git` but no `go.mod`, so `go run file.goo` fails there — capture expected via a throwaway module:
  ```bash
  d=$(mktemp -d); cp FILE.goo "$d/main.go"; ( cd "$d" && go mod init m >/dev/null 2>&1 && go run . )
  ```
- Interface-**target** assertions (`x.(Stringer)`, `case Stringer:`) are OUT of scope and MUST remain rejected — do not touch those guards.
- The CompCert gate needs `ccomp` on PATH: `eval "$(opam env --switch=default)"` before `make verify`.
- Commits: conventional, imperative, atomic. End messages with the two trailer lines used across this repo (`Co-Authored-By:` and `Claude-Session:`). If commit fails on a 1Password signing error, retry with `git -c commit.gpgsign=false commit --no-verify` (background session; agent unreachable).

---

### Task 1: Type assertion to a concrete type on `any`

Lift the empty-interface guard in the expression-level `AST_TYPE_ASSERT` check so `x.(int)` and its comma-ok form work when `x : any`. This one guard covers the plain assert, the comma-ok form (`v, ok := x.(T)`), and the `v := x.(T)` decl form — all route through this case.

**Files:**
- Modify: `src/types/expression_checker.c` (remove the `method_count == 0` block at ~line 554; keep the interface-target block just below)
- Create: `examples/rtti_assert_any.goo`, `examples/rtti_assert_any.expected.txt`
- Modify: `Makefile` (add `rtti-assert-panic-probe` target; wire into `verify:`)

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: the fact that concrete assertions on `any` compile — Task 3 relies on the KEEP guards (interface-target) still firing.

- [ ] **Step 1: Safety audit — confirm exactly which guards exist**

Run:
```bash
cd /data/Workspace/github.com/goolang
grep -rn 'empty interface is not supported\|to an interface type is not supported' src/types/
```
Expected: exactly four matches — two "empty interface" (LIFT: `expression_checker.c` ~554, `type_checker.c` ~2274) and two "to an interface type" (KEEP: `expression_checker.c` ~571, `type_checker.c` ~2328). If more "empty interface" guards appear, STOP and report — the plan assumed two.

- [ ] **Step 2: Write the failing golden**

Create `examples/rtti_assert_any.goo`:
```go
package main

import "fmt"

func main() {
	var x interface{} = 7
	n := x.(int)
	fmt.Println(n + 1)

	var y interface{} = "hi"
	s, ok := y.(string)
	fmt.Println(s, ok)

	m, ok2 := y.(int)
	fmt.Println(m, ok2)
}
```
Capture expected output from Go and save it:
```bash
d=$(mktemp -d); cp examples/rtti_assert_any.goo "$d/main.go"
( cd "$d" && go mod init m >/dev/null 2>&1 && go run . ) > examples/rtti_assert_any.expected.txt
cat examples/rtti_assert_any.expected.txt
```
Expected file contents:
```
8
hi true
0 false
```

- [ ] **Step 3: Run it to verify it fails (currently rejected)**

Run:
```bash
bin/goo examples/rtti_assert_any.goo -o /tmp/rtti_assert_any 2>&1 | head -1
```
Expected: a compile error containing `type assertion on the empty interface is not supported in v1`.

- [ ] **Step 4: Lift the guard**

In `src/types/expression_checker.c`, delete this block (currently ~lines 543–559 — the comment and the `if`):
```c
            // Empty interface (`interface{}`, method_count == 0): the dynamic-
            // type check codegen emits is a vtable-pointer compare
            // (x.vtable == &goo.vtable.T.I), which relies on each candidate
            // type having a distinct vtable. A zero-method interface's vtable
            // is an identical empty [0 x ptr] array for EVERY concrete type,
            // so the compare always matches — a silent miscompile, not a
            // clean failure. Reject here rather than let it through: real
            // empty-interface discrimination needs runtime type information
            // (a type-descriptor tag), deferred to the same RTTI cycle as
            // assert-to-interface (the TYPE_INTERFACE target rejection two
            // blocks below).
            if (operand_type->data.interface.method_count == 0) {
                type_error(checker, expr->pos,
                    "type assertion on the empty interface is not supported in v1 "
                    "(requires runtime type information)");
                return NULL;
            }
```
Leave everything else in the case intact — in particular the `if (target_type->kind == TYPE_INTERFACE)` block below it (the interface-target reject) stays.

- [ ] **Step 5: Build**

Run: `make lexer`
Expected: exit 0 (warnings ok).

- [ ] **Step 6: Run the golden to verify it passes**

Run:
```bash
bin/goo examples/rtti_assert_any.goo -o /tmp/rtti_assert_any && /tmp/rtti_assert_any
```
Expected:
```
8
hi true
0 false
```

- [ ] **Step 7: Add the assert-miss panic probe to the Makefile**

Add near the other probe targets in `Makefile`:
```makefile
rtti-assert-panic-probe: lexer
	@printf 'package main\nfunc main(){ var x interface{} = "s"; _ = x.(int) }\n' > build/rtti_panic.goo
	@$(COMPILER) build/rtti_panic.goo -o build/rtti_panic 2>/dev/null || (echo "FAIL: should compile"; exit 1)
	@if build/rtti_panic; then echo "FAIL: expected panic, got clean exit"; exit 1; fi
	@echo "PASS rtti-assert-panic-probe (assert-miss on any panics)"
```
Add `rtti-assert-panic-probe` to the `verify:` target's dependency/recipe list (mirror how existing `*-probe` targets are wired — grep `verify:` in the Makefile to match the exact style; if `$(COMPILER)`/`build/` are not the local names, use whatever the neighbouring probes use).

- [ ] **Step 8: Run the panic probe**

Run: `make rtti-assert-panic-probe`
Expected: `PASS rtti-assert-panic-probe (assert-miss on any panics)`

- [ ] **Step 9: Commit**

```bash
git add src/types/expression_checker.c examples/rtti_assert_any.goo examples/rtti_assert_any.expected.txt Makefile
git -c commit.gpgsign=false commit --no-verify -m "feat(types): type assertion to concrete type on the empty interface

Lift the stale empty-interface guard in AST_TYPE_ASSERT — post-#132 the
per-type vtable (slot-0 eq fn) makes vtable-pointer identity distinguish
concrete types under any. Covers x.(T), comma-ok, and v := x.(T). Keeps
the interface-target reject. Golden + assert-miss panic probe.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01U5mDRKdK3K6PC1mqXutAYa"
```

---

### Task 2: Type switch to concrete cases on `any`

Lift the empty-interface guard in `type_check_type_switch_stmt` so `switch v := x.(type)` discriminates concrete cases when `x : any`.

**Files:**
- Modify: `src/types/type_checker.c` (remove the `method_count == 0` block at ~lines 2266–2278; keep the interface-target reject in the case loop ~2328)
- Create: `examples/rtti_type_switch_any.goo`, `examples/rtti_type_switch_any.expected.txt`

**Interfaces:**
- Consumes: nothing from Task 1 (independent guard, independent file).
- Produces: nothing later tasks depend on beyond the KEEP guards staying intact.

- [ ] **Step 1: Write the failing golden**

Create `examples/rtti_type_switch_any.goo`:
```go
package main

import "fmt"

type Point struct {
	X int
}

func describe(x interface{}) {
	switch v := x.(type) {
	case int:
		fmt.Println("int", v+1)
	case string:
		fmt.Println("string", v)
	case bool:
		fmt.Println("bool", v)
	case *Point:
		fmt.Println("ptr Point", v.X)
	case Point:
		fmt.Println("Point", v.X)
	case nil:
		fmt.Println("nil")
	default:
		fmt.Println("other")
	}
}

func main() {
	describe(41)
	describe("hi")
	describe(true)
	describe(&Point{X: 5})
	describe(Point{X: 6})
	describe(nil)
	describe(3.14)
}
```
Capture expected from Go:
```bash
d=$(mktemp -d); cp examples/rtti_type_switch_any.goo "$d/main.go"
( cd "$d" && go mod init m >/dev/null 2>&1 && go run . ) > examples/rtti_type_switch_any.expected.txt
cat examples/rtti_type_switch_any.expected.txt
```
Expected file contents:
```
int 42
string hi
bool true
ptr Point 5
Point 6
nil
other
```

- [ ] **Step 2: Run it to verify it fails (currently rejected)**

Run:
```bash
bin/goo examples/rtti_type_switch_any.goo -o /tmp/rtti_ts 2>&1 | head -1
```
Expected: a compile error containing `type switch on the empty interface is not supported in v1`.

- [ ] **Step 3: Lift the guard**

In `src/types/type_checker.c`, delete this block (currently ~lines 2266–2278 — the comment and the `if`):
```c
    // Empty interface (`interface{}`, method_count == 0): same miscompile as
    // x.(T) on an empty-interface operand (see AST_TYPE_ASSERT's guard in
    // expression_checker.c) — the vtable-pointer identity check has no
    // signal when every concrete type shares an identical empty vtable, so
    // `switch x.(type)` would always match the first case. Reject rather
    // than let it silently pick the wrong branch; deferred to the RTTI
    // cycle alongside assert-to-interface.
    if (iface_type->data.interface.method_count == 0) {
        type_error(checker, stmt->pos,
                   "type switch on the empty interface is not supported in v1 "
                   "(requires runtime type information)");
        return 0;
    }
```
Leave the rest of the function intact — the interface-target reject in the per-case loop (~line 2328) stays.

- [ ] **Step 4: Build**

Run: `make lexer`
Expected: exit 0.

- [ ] **Step 5: Run the golden to verify it passes**

Run:
```bash
bin/goo examples/rtti_type_switch_any.goo -o /tmp/rtti_ts && diff <(/tmp/rtti_ts) examples/rtti_type_switch_any.expected.txt && echo MATCH
```
Expected: `MATCH` (no diff output).

- [ ] **Step 6: Multi-type case check (v keeps `any`)**

Create `/tmp/rtti_multi.goo`:
```go
package main

import "fmt"

func f(x interface{}) {
	switch v := x.(type) {
	case int, string:
		fmt.Println("int-or-string", v)
	default:
		fmt.Println("other")
	}
}

func main() {
	f(1)
	f("a")
	f(true)
}
```
Run:
```bash
bin/goo /tmp/rtti_multi.goo -o /tmp/rtti_multi && /tmp/rtti_multi
d=$(mktemp -d); cp /tmp/rtti_multi.goo "$d/main.go"; ( cd "$d" && go mod init m >/dev/null 2>&1 && go run . )
```
Expected: both print identically:
```
int-or-string 1
int-or-string a
other
```
If they differ, STOP and report (multi-type-case binding is a separate issue).

- [ ] **Step 7: Commit**

```bash
git add src/types/type_checker.c examples/rtti_type_switch_any.goo examples/rtti_type_switch_any.expected.txt
git -c commit.gpgsign=false commit --no-verify -m "feat(types): type switch to concrete cases on the empty interface

Lift the stale empty-interface guard in type_check_type_switch_stmt so
switch v := x.(type) discriminates concrete cases on any via the existing
per-case vtable-identity compare. Keeps the interface-target reject.
Golden covering int/string/bool/*T/struct/nil/default.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01U5mDRKdK3K6PC1mqXutAYa"
```

---

### Task 3: Confirm KEEP guards still fire + full regression gates

Prove the out-of-scope interface-target cases are still rejected (no accidental over-lift), then run every gate green before the PR.

**Files:**
- Modify: `Makefile` (add `rtti-iface-target-reject-probe`; wire into `verify:`)

**Interfaces:**
- Consumes: the KEEP guards from Tasks 1 & 2 (interface-target rejects).
- Produces: a green `make verify` and golden suite — the merge gate.

- [ ] **Step 1: Add the interface-target reject probe to the Makefile**

Add near the other probes:
```makefile
rtti-iface-target-reject-probe: lexer
	@printf 'package main\ntype S interface{ M() int }\nfunc main(){ var x interface{} = 1; _, _ = x.(S) }\n' > build/rtti_rej_assert.goo
	@if $(COMPILER) build/rtti_rej_assert.goo -o build/rtti_rej_assert 2>build/rtti_rej_assert.err; then echo "FAIL: assert-to-interface should be rejected"; exit 1; fi
	@grep -q "to an interface type is not supported" build/rtti_rej_assert.err || (echo "FAIL: wrong rejection message"; cat build/rtti_rej_assert.err; exit 1)
	@printf 'package main\ntype S interface{ M() int }\nfunc main(){ var x interface{} = 1; switch x.(type) { case S: } }\n' > build/rtti_rej_sw.goo
	@if $(COMPILER) build/rtti_rej_sw.goo -o build/rtti_rej_sw 2>build/rtti_rej_sw.err; then echo "FAIL: case-interface should be rejected"; exit 1; fi
	@grep -q "to an interface type is not supported" build/rtti_rej_sw.err || (echo "FAIL: wrong rejection message"; cat build/rtti_rej_sw.err; exit 1)
	@echo "PASS rtti-iface-target-reject-probe (interface targets still rejected)"
```
Wire `rtti-iface-target-reject-probe` into `verify:` alongside `rtti-assert-panic-probe` (match the existing probe wiring style).

- [ ] **Step 2: Run the reject probe**

Run: `make rtti-iface-target-reject-probe`
Expected: `PASS rtti-iface-target-reject-probe (interface targets still rejected)`

- [ ] **Step 3: Golden suite**

Run: `bash scripts/run_golden.sh`
Expected: last line `--- golden: N passed, 0 failed ---` with N increased by 2 versus main (the two new goldens).

- [ ] **Step 4: Unit tests**

Run: `make test`
Expected: `Passed: 76`, `Skipped: 1`, `All tests passed!`.

- [ ] **Step 5: Full verify incl. CompCert**

Run:
```bash
eval "$(opam env --switch=default)"
make verify
```
Expected: exit 0 (the `rtti-assert-panic-probe` and `rtti-iface-target-reject-probe` PASS lines appear; no `Error`).

- [ ] **Step 6: Grammar tripwire (no-op sanity)**

Run: `./scripts/grammar-tripwire.sh`
Expected: `grammar-tripwire: PASS (82 S/R + 256 R/R — baseline exact)` (unchanged — no grammar edit).

- [ ] **Step 7: Commit**

```bash
git add Makefile
git -c commit.gpgsign=false commit --no-verify -m "test(types): probes — assert-miss panics, interface targets still rejected

Wire two Makefile probes into verify: assert-miss on any runtime-panics;
x.(Interface) and case Interface stay compile-rejected (out of scope).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01U5mDRKdK3K6PC1mqXutAYa"
```

---

## Self-Review

**Spec coverage:**
- Lift expression_checker guard → Task 1. ✅
- Lift type_checker switch guard → Task 2. ✅
- Keep interface-target rejects → Task 3 probe + "leave intact" notes in Tasks 1–2. ✅
- Safety audit (enumerate empty-iface paths) → Task 1 Step 1. ✅
- go-run goldens (int/string/bool/*T/struct/nil/default, multi-type, comma-ok, plain assert) → Task 1 golden (assert + comma-ok) + Task 2 golden (switch matrix) + Step 6 (multi-type). ✅
- `any` through function param/return → Task 2 golden's `describe(...)` calls (param) covers it; explicit return not separately tested but the param path exercises the same box flow. ✅ (adequate)
- Reject/panic probes → Task 1 panic probe, Task 3 reject probe. ✅
- Known limitation int≡int64 → documented in spec, no task needed (not fixed). ✅
- Gates (tripwire/golden/verify/CI) → Task 3. ✅

**Placeholder scan:** No TBD/TODO; every code/command step has literal content. The Makefile-wiring steps say "match the existing probe style" because the exact `verify:` recipe lines are repo-local — the implementer greps `verify:` to see them; this is a lookup, not a placeholder.

**Type consistency:** No new function signatures introduced (pure deletions + goldens + Makefile). Guard message strings quoted verbatim from source. Golden expected outputs match the spike-observed values.
