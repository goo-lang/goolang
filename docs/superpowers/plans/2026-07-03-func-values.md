# Func Values (Closures Branch A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement Branch A of the approved closures design (docs/superpowers/specs/2026-07-03-closures-design.md): function types resolve, functions are first-class values via the universal fat-pointer representation, indirect calls work everywhere, nil-func calls abort cleanly. NO func literals (Branch B).

**Architecture:** (T1) Fix the `func_type` grammar action (it has a TODO and passes the raw `func_signature` node through — why `func(int) int` types fail as "Invalid type node") to build the existing-but-unused `FuncTypeNode`, and give `type_from_ast` an AST_FUNC_TYPE arm → TYPE_FUNCTION. This alone may make bare-pointer callbacks (`apply(inc, 41)`) work end to end — verify. (T2) Migrate the function VALUE representation to the fat pointer `{fn_ptr, env_ptr}` (16 bytes exactly — by-value legal, the >16 by-pointer rule must NOT fire): TYPE_FUNCTION's value mapping becomes the pair struct; named-func-to-value sites build `{thunk, NULL}` with a once-per-function synthesized env-ignoring thunk (the #30 goroutine-thunk precedent); indirect calls extract and call `fn(env, args...)`; direct calls to named functions stay bare (zero change). (T3) Nil-func-call runtime abort + signature-mismatch rejection.

**Tech Stack:** C23, LLVM-C. Parser action-code (T1 only), checker, codegen. No new runtime functions expected (abort reuses the existing runtime-abort mechanism — READ how divzero aborts).

## Global Constraints

- Branch: `feat/func-values` (created; spec at 7b5eadb). Do NOT commit on main.
- Commits: conventional, imperative, `--no-gpg-sign`. Stage only named files; never stage `.superpowers/` or `.handoff.md`.
- Bison discipline (T1 touches parser.y ACTION CODE ONLY): baseline 79 shift/reduce + 256 reduce/reduce, exact counts recorded after `make clean && make lexer`; any delta = STOP (an action-only change cannot move them).
- Gate per task: `make lexer` (clean-first when parser.y/ast.h touched), probes, then `eval "$(opam env --switch=default)"`, `make verify` (ALL PASS; golden 192/0 grows per probe; 27 reject probes stay green) and `make test` (76/1) and `make ccomp-link` (PASS). STOP/BLOCKED on any regression. **T2 is the risk task — it changes the LLVM type of every function-typed VALUE; the whole suite guards it, function-valued-field and `go`-statement goldens especially.**
- Go conformance: `go run`-verify probes; rejections match Go's decisions (repo wording).
- Probe hygiene: bool-compare floats, same-width prints.
- Pre-commit hook runs `make test`.

## Reference: verified code landmarks (2026-07-03, branch @7b5eadb)

- Grammar: `func_type: FUNC func_signature { /* TODO: Create proper function type */ $$ = $2; }` (parser.y:2221-2226) — the bug. `FuncTypeNode { base, params, return_type }` exists unused (include/ast.h:548-552, AST_FUNC_TYPE at :62). READ what `func_signature` yields (its own production) to harvest params/return into the FuncTypeNode; the params chain is VarDeclNodes (the func_param machinery, same as #105 used).
- `type_from_ast` dispatch: src/types/type_checker.c:2031+ — add the AST_FUNC_TYPE case; build TYPE_FUNCTION via `type_function(param_types, count, return_type)`; honor `is_variadic_param` on the last param (the #105 model: last param type becomes `type_slice(T)` + `is_variadic=1`).
- Function LLVM types: `codegen_get_function_type` (src/codegen/type_mapping.c:239) returns the LLVM FUNCTION type — used by call sites and declarations; the TYPE_FUNCTION case of `codegen_type_to_llvm` (type_mapping.c:63-64) is the VALUE mapping T2 changes to the pair struct. Split the two uses carefully — direct-call and function-declaration paths keep the function type.
- Named-func-as-value site: expression_codegen.c identifier arm ~:161-165 (`LLVMGetNamedFunction(codegen->module, ident->name)` / pkg_sym) — where a function identifier in non-callee position yields the bare pointer today.
- Indirect-call path: #104's review verified function-valued FIELD calls work — find the call path in call_codegen.c that calls through a non-global function value (`LLVMGlobalGetValueType` at the direct path REQUIRES a global — the indirect path must already differ or has a hole; READ `codegen_generate_call_expr`'s dispatch and record what exists).
- Thunk precedent: statement_codegen.c:1355+ (`go f(a,b)` heap-box + thunk + `goo_go(thunk, box)`).
- Runtime abort mechanism: READ how `divzero-probe`'s abort is emitted (grep `divzero`/`goo_panic` in codegen) and reuse it for nil-func calls.
- Zero value: `var f func(int) int` with no initializer takes the var-decl zero path (`LLVMConstNull(llvm_type)`) — with the pair struct this is `{NULL, NULL}` for free; verify.
- Repro (2026-07-03): `func apply(f func(int) int, v int) int` — parses, checker fails `Invalid type node` at the param.

---

### Task 1: Function types resolve (FuncTypeNode + type_from_ast arm)

**Files:**
- Modify: `src/parser/parser.y:2221-2226` (action only), `src/types/type_checker.c` (type_from_ast arm), `src/ast/ast.c`/`ast_constructors.c` ONLY if FuncTypeNode needs a constructor/free arm (check both — a node kind that never constructed may lack free handling; the no-header-deps append rule applies if ast.h needs anything, but it should NOT — the struct exists)
- Test: `examples/funcval_probe.goo` + `.expected.txt`

**Interfaces:**
- Produces: `func(T...) R` in ANY type position (param, var, return, field, slice elem) resolves to TYPE_FUNCTION. T2 consumes this everywhere.

- [ ] **Step 1: Probe** (`examples/funcval_probe.goo`):
```go
package main

import "fmt"

func inc(x int) int {
	return x + 1
}

func dbl(x int) int {
	return x * 2
}

func apply(f func(int) int, v int) int {
	return f(v)
}

func pick(b bool) func(int) int {
	if b {
		return inc
	}
	return dbl
}

func main() {
	fmt.Println(apply(inc, 41))
	fmt.Println(apply(dbl, 21))
	var f func(int) int
	f = inc
	fmt.Println(f(1))
	g := pick(false)
	fmt.Println(g(21))
}
```
`.expected.txt`: `42` `42` `2` `42`. `go run`-verify. Covers: func-typed param (callback), var decl + assign + indirect call, func-typed RETURN (named funcs returned), reassignment through pick.
- [ ] **Step 2: Verify today** — record: parse OK, checker "Invalid type node" per func-typed position (RED).
- [ ] **Step 3: Grammar action** — read `func_signature`'s production/yield; rewrite the func_type action to build a FuncTypeNode (base.type=AST_FUNC_TYPE, params=<the signature's param chain>, return_type=<its result>) instead of passing $2 through. Action code only; `make clean && make lexer`; conflict counts must be EXACTLY 79/256 (action-only change — any delta means you changed grammar, STOP).
- [ ] **Step 4: type_from_ast arm** — AST_FUNC_TYPE → walk the params chain (VarDeclNodes) building param_types (honoring `is_variadic_param` per the #105 model), resolve return (NULL → void), `type_function(...)`. Also verify `type_compatible` handles TYPE_FUNCTION vs TYPE_FUNCTION (signature equality: param count/types + return + variadic flag) — add the arm if missing (T3's reject probe depends on it).
- [ ] **Step 5: End-to-end check** — with types resolving, does the probe pass with TODAY's bare-pointer value representation? (codegen TYPE_FUNCTION mapping + the identifier arm + some call path may already compose.) If yes: full gate, done. If a specific shape fails in codegen (e.g. indirect call through a local var), record EXACTLY which and reduce the probe to the working subset + list the failures in the report — T2 picks them up (do NOT hack codegen here).
- [ ] **Step 6: Gate** — probe (possibly reduced) passes; golden 193/0; test 76/1.
- [ ] **Step 7: Commit** — "feat(parser,types): function types resolve (FuncTypeNode + type_from_ast arm)".

---

### Task 2: Universal fat-pointer function values

**Files:**
- Modify: `src/codegen/type_mapping.c` (TYPE_FUNCTION value mapping → {i8*, i8*} pair), `src/codegen/expression_codegen.c` (identifier-as-value builds {thunk, NULL}), `src/codegen/call_codegen.c` (indirect-call extraction; direct-call path untouched), `src/codegen/function_codegen.c` (thunk synthesis helper if it lives here; follow where #30's thunk builder sits)
- Test: `examples/funcval_probe.goo` grows (restore any Task 1 reductions + add field/slice shapes)

**Interfaces:**
- Consumes: T1 (types resolve). Produces: every function-typed VALUE is `{fn_ptr, env_ptr}`; indirect calls emit `fn(env, args...)`; named funcs as values are `{thunk_F, NULL}` where `thunk_F(env, args...) = F(args...)`. Branch B builds closures on exactly this representation — the env parameter position (FIRST) is load-bearing; document it as a change-together contract at the thunk builder, the indirect-call site, and the spec.

- [ ] **Step 1: Extend the probe** — add to funcval_probe.goo (keep Task 1 lines):
```go
type Ops struct {
	F func(int) int
}

func run(o Ops, v int) int {
	return o.F(v)
}
```
and in main: `o := Ops{F: dbl}; fmt.Println(run(o, 5))` (→ `10`), `fs := []func(int) int{inc, dbl}; fmt.Println(fs[0](0) + fs[1](3))` (→ `7`). Update `.expected.txt` (`42 42 2 42 10 7`). `go run`-verify. If a shape fails for a PRE-EXISTING reason unrelated to representation (e.g. slice-of-func literals never worked), record and drop that line — do not force it.
- [ ] **Step 2: Verify current state** — record which shapes work on bare pointers post-T1 (RED/GREEN table).
- [ ] **Step 3: Value mapping** — `codegen_type_to_llvm` TYPE_FUNCTION → `{ptr, ptr}` struct (build once in context, name it e.g. "goo.funcval"). AUDIT every consumer of the old bare-pointer mapping (grep TYPE_FUNCTION across src/codegen) and classify each use VALUE vs SIGNATURE in your report — signature uses keep `codegen_get_function_type`.
- [ ] **Step 4: Thunk synthesis** — helper `LLVMValueRef codegen_get_func_thunk(codegen, checker, Type* fn_type, LLVMValueRef named_fn, const char* name)`: get-or-create `@name.__thunk` with signature `ret (i8* env, params...)` whose body calls `named_fn(params...)` and returns. Mirror #30's thunk-building conventions (block save/restore, builder positioning). Cache via LLVMGetNamedFunction on the thunk name.
- [ ] **Step 5: Identifier-as-value** — the expression_codegen identifier arm (~:161): when yielding a FUNCTION value in non-callee position, build the pair {thunk, ConstNull} as a first-class aggregate (InsertValue x2 or ConstStruct). Callee-position direct calls MUST bypass (the call path resolves the callee itself — verify it does not route through the value arm; if it does, gate on context and record how).
- [ ] **Step 6: Indirect calls** — in codegen_generate_call_expr: when the callee expression yields a function-typed VALUE (not a direct global): extract fn_ptr (idx 0) and env_ptr (idx 1), build the call as `fn(env, args...)` — the LLVM function type for the call is derived from the goo TYPE_FUNCTION (env i8* prepended). Existing direct-named path (LLVMGlobalGetValueType on the global) unchanged.
- [ ] **Step 7: Zero value + stores** — `var f func(int) int` zero path yields ConstNull of the pair (verify free); assignments/stores/params/returns of the pair work through the existing aggregate machinery (16 bytes — confirm no by-pointer path triggers; the m12 rule is >16).
- [ ] **Step 8: Gate** — FULL probe passes; golden 193/0 (function-valued-field goldens + `go`-statement goldens + entire suite); test 76/1; ccomp PASS. STOP on any golden regression you cannot attribute.
- [ ] **Step 9: Commit** — "feat(codegen): universal fat-pointer function values (thunks for named funcs)".

---

### Task 3: Nil-func-call abort + signature-mismatch rejection

**Files:**
- Modify: `src/codegen/call_codegen.c` (nil check at indirect calls), `src/types/expression_checker.c` or `types.c` ONLY if type_compatible's TYPE_FUNCTION arm wasn't completed in T1
- Test: `examples/funcnil_abort.goo` + Makefile `funcnil-abort-probe`; `examples/funcsig_reject.goo` + Makefile `funcsig-reject-probe`

**Interfaces:** Consumes T2's indirect-call site.

- [ ] **Step 1: Abort probe** — `examples/funcnil_abort.goo`: `var f func(int) int` then `f(1)`. Go panics (`invalid memory address or nil pointer dereference`-class). Makefile `funcnil-abort-probe` mirrors `bits-div-abort-probe`/`divzero-probe`'s runtime-abort pattern (compiles OK, run aborts non-zero with a message grep — use `nil function` in the message). Emit: at the indirect-call site, icmp fn_ptr vs null → abort branch calling the same runtime abort divzero uses, message "call of nil function".
- [ ] **Step 2: Reject probe** — `examples/funcsig_reject.goo`: `func two(a, b int) int` assigned to `var f func(int) int` — Go rejects (cannot use two ... as func(int) int). `funcsig-reject-probe` greps the checker's mismatch message (repo wording, e.g. `Cannot assign func(int, int) int to func(int) int` — requires type_to_string to render TYPE_FUNCTION legibly; add/fix its arm if it prints garbage, record).
- [ ] **Step 3: Verify today** — record both RED behaviors post-T2 (nil call = raw segfault? mismatch = accepted or garbled?).
- [ ] **Step 4: Implement + gate** — probes PASS (golden count unchanged — abort/reject probes aren't goldens; verify 193/0 + 29 probes total); test 76/1; ccomp PASS.
- [ ] **Step 5: Commit** — "feat(types,codegen): nil-func-call abort; reject signature-mismatched func assignment".

---

## Final gate

`make verify` → ALL GREEN (193/0 + 29 reject/abort probes). `make test` → 76/1. `make ccomp-link` → PASS. Bison 79/256 exact (T1's action-only change cannot move it).

## Self-review notes

- T1 Step 5 deliberately allows a reduced probe rather than hacking codegen early — T2 owns representation; the reduction list is the T2 contract.
- T2 Step 3's audit-and-classify (VALUE vs SIGNATURE uses) is the safety net for the migration — the #104 lesson (assumed-dead vs live) says enumerate, don't assume.
- The env-first parameter position is documented as a change-together contract in three places — Branch B builds on it unseen.
- Out of scope (recorded): func literals and captures (Branch B); method values; `go f(...)`'s arg-boxing path (untouched — named-func go statements don't route through function VALUES).
