# Var-Init Cluster (1w/1x/1o) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the three var-initializer gaps queued post-#101 — all reproduced on main @ee83639: (1w) `var q ?float64 = n` nullable-wrap of a typed int crashes the verifier (`insertvalue {i1,double}, i64`); (1x) any *global* initializer containing an identifier operand SIGSEGVs the compiler (rc=139; `var y = x`, `var z = x + 1`); (1o) no constant representability checks anywhere (`var b int8 = 300` prints 44, `uint8 = 256` prints 0, `float32 = 1e40` silently inf — Go rejects all).

**Architecture:** (T1) Route every nullable auto-wrap site through the existing centralized `codegen_create_nullable_with_value` and teach it to coerce via the shared `codegen_coerce_to_type` helper (PR #99) instead of its int-only SExt/Trunc — one fix covers var-decl, reassign, param, and return wraps. (T2) Synthesize a module init function (`goo.global_init`): global storage keeps its constant/zero LLVM initializer as today, but non-constant initializer *expressions* are collected and evaluated inside the init function (declaration order) with the builder positioned, then stored; main's entry calls it first. This fixes the SIGSEGV class at its root (unpositioned builder at module scope) and should make `var x = f()` legal as a byproduct — the AST_CALL_EXPR pre-rejection gets retired if probes confirm. (T3) A `literal_fits_type` representability check at the checker's stamping choke points (var-decl, both adapters' literal arms, constant conversions), with negation context threaded through the unary-minus recursion so `-128` fits `int8` while `128` does not. Per-literal checks only — we do not constant-fold, so `int8 = 100+100` wrapping at runtime is a documented deviation (same stamp-and-compute clause as #101).

**Tech Stack:** C23, LLVM-C. Codegen (T1, T2) + checker (T3). No parser/header changes expected; if T2 genuinely needs a header change, STOP and report (Makefile has no header deps — `make clean` rule).

## Global Constraints

- Branch: `fix/var-init-cluster` off main @ee83639. Do NOT commit on main.
- Commits: conventional, imperative, `--no-gpg-sign`. Stage only named files; never stage `.superpowers/` or `.handoff.md`.
- Gate per task: `make lexer`, probes, then `eval "$(opam env --switch=default)"`, `make verify` (ALL PASS; golden 183/0 grows per probe: 184/185/186 + T3's reject probes) and `make test` (76/1). STOP/BLOCKED on any regression. Guard goldens: the whole nullable family (`nullable_*_probe`, `unwrap_nil_probe`), `cross_kind_probe`, `const_expr_probe`, `composite_field_adapt_probe`, `float_adapt_probe`, `int_width_probe`, `var_width_probe`, and all `*-reject-probe` targets.
- Go conformance: where `go` is on PATH (go1.26), derive/verify probe expectations with `go run` and record the comparison; otherwise reason from the Go spec and say so.
- Probe hygiene: bool-compare floats, exactly-representable values, same-width prints.
- Nullable value extraction syntax is `if let v = q { ... }` (see `examples/nullable_iflet_probe.goo`).
- Reject-probe pattern: mirror `constdiv-reject-probe` (Makefile ~:455+): `rm -f` stale binary, compile rc≠0, no binary emitted, anti-verifier-crash grep (`Module verification failed|LLVM ERROR`), diagnostic grep on a distinctive substring, wired into `verify` beside the existing ones.
- Bison untouched (79/256).

## Reference: verified code landmarks (2026-07-03, main @ee83639)

- Centralized wrap helper: `codegen_create_nullable_with_value`, `src/codegen/nullable_codegen.c:11-48` — has an int↔int SExt/Trunc slot-coercion block (:28-43) but NO float arms; `{i1, double}` slot with an i64 value inserts raw → verifier error.
- Var-decl auto-wrap (the (1w) crash site): `src/codegen/function_codegen.c:1014-1022` — inlines `LLVMBuildInsertValue` directly, bypassing the helper entirely. Comment "Auto-wrap a plain value into a nullable struct".
- Other wrap sites (per `examples/nullable_width_probe.goo` header): return path `src/codegen/statement_codegen.c:~919-1025` (has its own SExt guard), reassign + param wraps already call the helper (`nullable_codegen.c`, `call_codegen.c:1014`).
- Shared coercion helper: `codegen_coerce_to_type`, `src/codegen/codegen.c:914` — int↔int (signedness-aware), int→float, float↔float; REQUIRES a positioned builder; no-ops on kinds it doesn't handle.
- Module-scope var path: `src/codegen/function_codegen.c:923-940` (LLVMAddGlobal + constant zero/nil initializer), initializer generation :948+, the AST_CALL_EXPR module-scope pre-rejection :965-971 (with `global_init_is_conversion_call` exemption), the constant-rebuild width path :1066+ (module scope can't touch the builder — the comment explains the null-insert-block SIGSEGV mechanism; identifier operands hit exactly this via `codegen_generate_expression` → identifier load).
- Program entry: `codegen_generate_program`, `src/codegen/codegen.c:221`. Main-function special-casing: `src/codegen/function_codegen.c:264,374,421`.
- Checker var-decl: `type_check_var_decl`, `src/types/type_checker.c:800`.
- Adapters + rooted predicates (post-#101): `adapt_untyped_int_operand` / `adapt_untyped_float_operand`, `src/types/expression_checker.c` (~:530-700; both recurse through unary minus/plus/complement and arithmetic binops; the int adapter's unary-minus leg is where T3's negation context comes from).
- Literals: `LiteralNode` (`include/ast.h:425-435`) stores the raw TEXT (`char* value`); int literals parse via `strtoull` (raw value always non-negative — negatives arrive as unary minus above the literal; see the T1 comment in `expression_codegen.c`'s float-stamp arm).
- Repro probes from scoping (2026-07-03): `var q ?float64 = n` → `Invalid InsertValueInst operands! %null_val = insertvalue { i1, double } ..., i64 %n1`; `var y = x` and `var z = x + 1` at package level → compiler rc=139; `var b int8 = 300` → 44, `var u uint8 = 256` → 0, `int8(300)` → 44, `var f float32 = 1e40` → +inf, all silently accepted.

---

### Task 1: Nullable auto-wrap coerces cross-kind (1w)

**Files:**
- Modify: `src/codegen/nullable_codegen.c` (helper gains `codegen_coerce_to_type` delegation)
- Modify: `src/codegen/function_codegen.c:1014-1022` (var-decl wrap routes through the helper)
- Test: `examples/nullable_adapt_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: `codegen_coerce_to_type` (codegen.c:914) — `LLVMValueRef codegen_coerce_to_type(CodeGenerator*, LLVMValueRef v, int src_is_signed, LLVMTypeRef target)`; READ its actual signature before calling, the plan's rendering may drift.
- Produces: `codegen_create_nullable_with_value` safely wraps ANY numeric value into any numeric `?T` slot (int→float, float widths, int widths). T2's init function may evaluate nullable global inits through the same var-decl path.

- [ ] **Step 1: Probe** (`examples/nullable_adapt_probe.goo`):
```go
package main

import "fmt"

func takesNF(p ?float64) bool {
	if let v = p {
		return v == 3.0
	}
	return false
}

func makeNF(n int) ?float64 {
	return n
}

func main() {
	n := 3
	var q ?float64 = n
	if let v = q {
		fmt.Println(v == 3.0)
	}
	var w ?float32 = n
	if let v = w {
		fmt.Println(v == float32(3.0))
	}
	var s ?int32 = n
	if let v = s {
		fmt.Println(int(v))
	}
	fmt.Println(takesNF(n))
	r := makeNF(4)
	if let v = r {
		fmt.Println(v == 4.0)
	}
	var t ?float64 = nil
	t = n
	if let v = t {
		fmt.Println(v == 3.0)
	}
}
```
`.expected.txt`: `true` `true` `3` `true` `true` `true`.
Cover: var-decl wrap (the crash), float32 slot, int-width slot (regression vs nullable_width_probe), param auto-wrap, return auto-wrap, reassign wrap — all with a TYPED int source (`n`), which is the broken direction (untyped literals already adapt in the checker).
NOTE: `?T` in a PARAMETER position and `int` param feeding a `?float64` return — verify these SHAPES parse/typecheck today before leaning on them; if a shape is rejected by the checker (not codegen), drop that line to a minimal equivalent (e.g. return a local instead) and record why.
- [ ] **Step 2: Verify today** — record per line which crash (verifier error), which mis-evaluate, which already pass. `go run` has no `?T`; state the intended Goo semantics instead (wrap-of-int-into-float-nullable holds the converted value).
- [ ] **Step 3: Fix the helper** — in `codegen_create_nullable_with_value`, replace the int-only SExt/Trunc block (:28-43) with a call to `codegen_coerce_to_type(codegen, value, <signedness of value_type, default signed>, slot_type)`. Keep the guard "only when types differ"; keep aggregates untouched (the helper no-ops on kinds it doesn't handle — verify that claim by reading it). Signedness: derive from the `Type* value_type` param via `type_is_signed` when non-NULL, else default signed (matches function_codegen.c:1055's convention).
- [ ] **Step 4: Route the var-decl wrap through the helper** — replace the inline InsertValue pair at function_codegen.c:1014-1022 with `codegen_create_nullable_with_value(codegen, llvm_type, init_value->llvm_value, init_value->goo_type)`. This block runs for locals (builder positioned). Module-scope nullable inits with non-constant values will flow through T2's init function later; today they hit the constant backstop — verify behavior is not WORSE than before for `var g ?int = 5` at global scope (it currently works via constant path? probe it; if global `?T = literal` regresses, keep the inline path for module scope and route only the function-scope branch).
- [ ] **Step 5: Check the return-path wrap** (statement_codegen.c:~919-1025 own SExt guard): `return n` into `?float64` is in the probe. If it fails after Steps 3-4, route it through the helper too; if it passes (its own guard may coincidentally cover it via the checker's stamping), leave it and record.
- [ ] **Step 6: Gate** — probe passes; full golden (184/0, nullable family green), test 76/1.
- [ ] **Step 7: Commit** — "fix(codegen): nullable auto-wrap coerces cross-kind values via shared helper".

---

### Task 2: Global initializers with non-constant expressions (1x)

**Files:**
- Modify: `src/codegen/function_codegen.c` (module-scope var path: collect deferred initializers; retire/rework the :965 call guard)
- Modify: `src/codegen/codegen.c` (`codegen_generate_program` :221 — synthesize `goo.global_init`, call it from main entry)
- Test: `examples/global_init_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: nothing from T1 (independent; sequential only for merge hygiene).
- Produces: a `goo.global_init` LLVM function evaluated before user main. T3 does not depend on it.

**Design (decided; alternatives recorded):** Synthesized init function (Option A) over extending the clean-rejection guard to identifiers (Option B). A is Go-conformant for the dominant cases (`var y = x`, `var z = x + 1`, `var w = f()` — all everyday Go) and kills the whole unpositioned-builder class; B is cheap but rejects real Go. Cost of A: initializer ORDER is declaration order, NOT Go's dependency-resolved order — a forward reference (`var a = b` before `var b = 5`) reads the zero value where Go computes 5. That deviation is documented in the probe header and code comment; full dependency ordering is out of scope (recorded follow-up). If A hits a structural wall (e.g. main-entry hook can't be placed without header changes), STOP: report BLOCKED with findings; the fallback is B (clean rejection, message style of :967) as a stopgap task.

- [ ] **Step 1: Probe** (`examples/global_init_probe.goo`):
```go
package main

import "fmt"

func double(n int) int {
	return n * 2
}

var x = 5
var y = x
var z = x + 1
var w = double(x)
var f = 0.5 * 3.0
var s = "go" + "o"

func main() {
	fmt.Println(x + y)
	fmt.Println(z)
	fmt.Println(w)
	fmt.Println(f == 1.5)
	fmt.Println(s)
}
```
`.expected.txt`: `10` `6` `10` `true` `goo`.
Verify each line with `go run` (all legal Go, declaration order == dependency order here by construction). Header comment MUST document the deviation: initializers run in declaration order; forward references read zero values (Go reorders) — do not add forward-ref lines to the probe.
- [ ] **Step 2: Verify today** — `var y = x` / `var z = x + 1`: compiler rc=139 (SIGSEGV). `var w = double(x)`: clean rejection via the :965 guard. `var s = "go"+"o"` and `var f = 0.5*3.0`: record actuals (constant-folded arithmetic may already pass via LLVMIsConstant — if a line already works, keep it as regression coverage).
- [ ] **Step 3: Collect deferred initializers** — in the module-scope var path (function_codegen.c :948+): after creating the global with its zero/nil constant initializer (existing :923-940 code unchanged), decide: if the initializer expression is a compile-time-constant shape (current behavior — generate, LLVMIsConstant true, set as initializer), keep today's path. Otherwise do NOT generate at module scope; append `(global LLVMValueRef, ASTNode* expr, Type* declared_type, Position pos)` to a deferred list on the CodeGenerator (a simple growable array field — add to the struct in the .c-visible header ONLY if CodeGenerator's struct is defined in a header; if that requires touching `include/codegen.h`, that IS allowed for a struct-field addition but requires `make clean` in the gate — note it in the report; the Makefile has no header deps). "Constant shape" test: generate ONLY when the expression is a literal, or a constant-foldable form the current code already handles (conversion-of-literal via the :965 exemption, literal arithmetic IF it currently produces LLVMIsConstant). Simplest robust rule: try nothing new at module scope — treat literals and nil as constant (today's working set), defer EVERYTHING else including calls (retiring the :965 guard). Preserve the nullable/interface global constant-init behavior exactly.
- [ ] **Step 4: Synthesize the init function** — in `codegen_generate_program` after all declarations are generated: if the deferred list is non-empty, create `void @goo.global_init()`, position the builder in its entry block, set `codegen->current_function` to it, and for each deferred entry run THE SAME pipeline the local var-decl path uses (generate expression → lvalue auto-load → nullable auto-wrap → interface box → width-coerce via `codegen_coerce_to_type` → `LLVMBuildStore` into the global). Extract that pipeline into a small static helper if sharing is clean; do NOT duplicate the four transform blocks verbatim (the #101 reviews flagged duplication — factor or call through). `LLVMBuildRetVoid` at the end; restore `current_function`/builder state.
- [ ] **Step 5: Call it before user main** — find where main's entry block is built (function_codegen.c:264/374/421 landmarks); insert a call to `goo.global_init` as main's first instruction (only when the function was created). Confirm ordering vs any existing runtime setup call already emitted at main entry — global inits must run AFTER runtime init if one exists (READ the main prologue; record what you find).
- [ ] **Step 6: Retire the :965 call guard** — with calls deferred to the init function, `var w = double(x)` must now work. Delete the guard + `global_init_is_conversion_call` IF all its cases route through the deferred path cleanly (probe `var g = int64(5)` still works — add a probe line if not covered); if a builtin call at global scope (e.g. `var m = make(map[string]int)`) misbehaves inside the init function, keep a narrowed guard for that specific class, record why, and add the follow-up. Test `make(map[string]int)` + a write in main as a one-off (not in the golden probe unless it works).
- [ ] **Step 7: Gate** — probe passes; full golden 185/0 (pay attention to every existing global-using golden: `var_width_probe`, global slice/array table probes, `nullable_struct_lit_probe`), test 76/1.
- [ ] **Step 8: Commit** — "feat(codegen): evaluate non-constant global initializers in a synthesized init function".

---

### Task 3: Constant representability checks (1o)

**Files:**
- Modify: `src/types/expression_checker.c` (new `literal_fits_type` helper; calls in both adapters' literal arms with negation context)
- Modify: `src/types/type_checker.c` (var-decl literal check; constant-conversion check if that path lives here — READ `type_check_var_decl` :800 first and record where conversions are checked)
- Test: `examples/const_range_probe.goo` + `.expected.txt`; Makefile reject probes `constint8-reject-probe`, `constuint8-reject-probe`, `constf32-reject-probe` + `examples/constint8_reject.goo`, `examples/constuint8_reject.goo`, `examples/constf32_reject.goo`
- Modify: `Makefile` (three reject targets, wired into `verify`)

**Interfaces:**
- Consumes: the adapters and their unary-minus recursion legs (post-#101 shapes).
- Produces: `static bool literal_fits_type(const LiteralNode* lit, const Type* target, bool negated)` — true iff the literal's value is representable in `target` (negated = an odd number of enclosing unary minuses). Emits nothing; callers emit the error (Go's wording: `constant 300 overflows int8`).

- [ ] **Step 1: Positive probe** (`examples/const_range_probe.goo`):
```go
package main

import "fmt"

func main() {
	var a int8 = 127
	var b int8 = -128
	var c uint8 = 255
	var d uint8 = 0
	var e int16 = -32768
	var f int32 = 2147483647
	var g float32 = 3.5
	var h float64 = -0.25
	fmt.Println(int(a), int(b))
	fmt.Println(int(c), int(d))
	fmt.Println(int(e))
	fmt.Println(int(f))
	fmt.Println(g == float32(3.5))
	fmt.Println(h == -0.25)
}
```
`.expected.txt`: `127 -128` `255 0` `-32768` `2147483647` `true` `true`.
Boundary values on BOTH sides — `-128`/`-32768` are the negation-context cases (raw literal 128/32768 alone would overflow). Verify with `go run`.
- [ ] **Step 2: Reject probes** — three programs, one shape each, mirroring `constdiv_reject.goo`'s header style (state Go rejects these too — these are Go-CONFORMANT rejections):
  - `constint8_reject.goo`: `var b int8 = 300` (grep: `overflows int8`)
  - `constuint8_reject.goo`: `var u uint8 = -1` (grep: `overflows uint8`; Go: `constant -1 overflows uint8`)
  - `constf32_reject.goo`: `var f float32 = 1e40` (grep: `overflows float32`)
  Makefile targets mirror `constdiv-reject-probe` exactly (rm -f, rc≠0, no binary, anti-crash grep, diagnostic grep), wired into `verify`.
- [ ] **Step 3: Verify today** — all three reject programs currently compile and run (record outputs: 44, 255, +inf); positive probe should pass already (record).
- [ ] **Step 4: Implement `literal_fits_type`** — in expression_checker.c near the adapters:
  - INT literal (TOKEN_INT): parse `lit->value` with `strtoull` (+ `errno == ERANGE` → does not fit anything; also handle hex/octal prefixes if the lexer passes them through — READ how the codegen literal arm parses, mirror it). For signed targets: fits iff `v <= max(target)` when !negated, `v <= (unsigned)max(target)+1` when negated. For unsigned targets: fits iff `!negated || v == 0`, and `v <= umax(target)`. Bounds per width from the Type's kind (int8/16/32/64, uint8/16/32/64; plain int/uint = 64-bit per Goo).
  - FLOAT literal (TOKEN_FLOAT): `strtod`; for float32 targets: fits iff `isfinite((float)v)` (overflow → ±inf) — underflow-to-zero is NOT an error (Go allows constants that round; only overflow rejects). float64: `errno != ERANGE`.
  - INT literal meeting a FLOAT target: always fits (float64 exactly represents any int the lexer produces up to 2^53 — beyond that Go still converts with rounding and does NOT reject; do not error).
- [ ] **Step 5: Wire the call sites** —
  - `adapt_untyped_int_operand`: its literal arm receives the stamp target; thread a `bool negated` parameter through the recursion (flipped at the unary-MINUS leg, unchanged at plus/complement — NOTE `^1` negation semantics: Go evaluates `^` on the CONSTANT, out of scope for range checks; exclude `^`-rooted from checking rather than get it wrong: stamping already computes it at runtime width, record as deviation).
  - `adapt_untyped_float_operand`: same for its float-literal arm.
  - `type_check_var_decl` (:800): the direct literal-meets-declared-type path if it does not route through the adapters — READ first; only add a check where stamping happens WITHOUT the adapters.
  - Constant conversions (`int8(300)`): find where conversion calls typecheck their operand; if the operand is an untyped literal (± unary minus), call `literal_fits_type` with the conversion target. Go rejects `int8(300)`; runtime-value conversions (`int8(x)`) stay legal truncation.
  - Error message format: `constant %s overflows %s` (mirror Go; include the literal text, and `-` prefix when negated).
- [ ] **Step 6: Deviation note** — `var b int8 = 100 + 100` will NOT be caught (per-literal checks; we don't fold). Confirm behavior (100 and 100 each fit; runtime wraps), document in the probe header and the helper's doc comment under the same stamp-and-compute clause as #101. Do NOT add that shape to any probe.
- [ ] **Step 7: Gate** — positive probe + all three reject probes PASS; full golden 186/0 (adaptation net especially — the adapters changed signature/behavior), test 76/1.
- [ ] **Step 8: Commit** — "feat(types): reject unrepresentable constants at typed sinks (Go conformance)".

---

## Final gate

`make verify` → ALL GREEN (186/0 + 6 reject probes). `make test` → 76/1. ccomp: opam env standalone, `make ccomp-link` → PASS.

## Self-review notes

- T1 is deliberately DRY-first (teach the helper, route the bypass through it) — the #101 reviews dinged duplicated logic twice; don't repeat that.
- T2's declaration-order semantics is an honest documented deviation; dependency-ordered init is a recorded follow-up, not silent scope creep. The BLOCKED fallback (clean rejection) is stated so the implementer never ships a half-working init path.
- T3's negation threading is the known hard edge (`-128` must fit int8); boundary values are pinned on both sides in the positive probe, and `^`-rooted literals are excluded (recorded) rather than mis-checked.
- Out of scope (recorded follow-ups): dependency-ordered global init; folded-expression range checks; `%` on floats rejection + mid-codegen re-typecheck removal (already filed from #101).
