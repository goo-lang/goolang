# Comptime-Value-Specialized Functions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A function parameter marked `comptime` (`func fill(comptime n int, seed int)`, called `fill(8, seed)`) whose argument must be a compile-time constant, monomorphized per distinct value with the value substituted as a literal — so `var buf [n]int` and `n`-bounded loops work inside the body.

**Architecture:** Extend three existing subsystems, no new ones. (1) Grammar/AST: a `COMPTIME identifier type` production on `func_param` sets a new `is_comptime_param` flag on the parameter's `VarDeclNode`. (2) Type checker: at a call, evaluate the arg for each comptime param through the existing comptime engine (`comptime_eval_expression`) and bind the param in the body as a compile-time constant (reusing the `comptime const` machinery). (3) Monomorphizer: key each instance on the comptime value(s) in addition to type args, stamping the value into the instance name and binding it as a const in the specialized body.

**Tech Stack:** C23; bison/flex parser; LLVM-C codegen; existing comptime engine (`src/comptime/`), monomorphizer (`src/codegen/monomorphize.c`).

## Global Constraints

- C23. Return errors, don't panic (except tests). No naked returns.
- New struct fields go at the STRUCT TAIL (no-header-deps convention): the Makefile lacks header deps, so a mid-struct insert silently miscompiles any TU rebuilt without `make clean`.
- **Grammar changes are stop-the-line.** Before AND after any `parser.y` edit, `./scripts/grammar-tripwire.sh` must report `82 S/R + 256 R/R` exact. Any delta is stop-the-line — follow the goo-grammar skill's conflict-ledger justified-delta procedure; do not proceed on an unexplained delta.
- Every task ends green: build (`make lexer`), the task's test, and no regression in `make test-golden` (currently 312/0). Commits unsigned if the 1Password agent is unavailable (`git commit --no-gpg-sign`).
- Spec: `docs/superpowers/specs/2026-07-07-comptime-value-specialized-functions-design.md`.

---

## File structure

- `include/ast.h` — add `is_comptime_param` to `VarDeclNode` (tail).
- `src/parser/parser.y` — add `COMPTIME identifier type` to `func_param`.
- `src/types/expression_checker.c` and/or `src/types/type_checker.c` — comptime-arg validation + param binding at call/signature time.
- `include/ast.h` (`CallExprNode`) — add `comptime_value_args` / `comptime_value_arg_count` (tail).
- `src/codegen/monomorphize.c` — extend instance key/name + per-instance const binding.
- `examples/comptime_value_specialize_probe.goo` (+ `.expected.txt`) — the `fill` demo.
- `Makefile` — `comptime-value-reject-probe` target + `verify` wiring.

---

### Task 1: `comptime` parameter — grammar + AST flag

**Files:**
- Modify: `include/ast.h` (VarDeclNode, tail)
- Modify: `src/parser/parser.y` (`func_param` rule)
- Test: `/tmp/t1_*.goo` throwaway parse checks (this task's deliverable is "it parses and the flag is set"; a committed probe comes in Task 4)

**Interfaces:**
- Produces: `VarDeclNode.is_comptime_param` (int, 1 iff the param was written `comptime name type`). Consumed by Tasks 2 and 3.

- [ ] **Step 1: Baseline the grammar tripwire**

Run: `./scripts/grammar-tripwire.sh`
Expected: `grammar-tripwire: PASS (82 S/R + 256 R/R — baseline exact)`

- [ ] **Step 2: Add the AST flag (tail-appended)**

In `include/ast.h`, in `VarDeclNode`, immediately BEFORE the closing `} VarDeclNode;` (after the last existing tail field), add:

```c
    // Comptime-value-specialized functions: set on a parameter written
    // `comptime name type`. The argument bound to it at a call must be a
    // compile-time constant; the function is monomorphized per distinct value
    // (the value is substituted as a literal in the specialized body). Appended
    // at the STRUCT TAIL per the no-header-deps convention above.
    int is_comptime_param;
```

Confirm `ast_var_decl_new` (`src/ast/ast_constructors.c` or `src/ast/ast.c`) zero-initializes the struct (memset/calloc) so the new field defaults to 0; if it field-by-field initializes, add `node->is_comptime_param = 0;`.

- [ ] **Step 3: Add the grammar production**

In `src/parser/parser.y`, in the `func_param:` rule, add a new alternative alongside `identifier type` (model it exactly on that alternative, plus the flag):

```c
    | COMPTIME identifier type {
        // Comptime value parameter `comptime name type`.
        IdentifierNode* ident = (IdentifierNode*)$2;
        VarDeclNode* param = ast_var_decl_new(get_current_position());
        param->names = malloc(sizeof(char*));
        param->names[0] = strdup(ident->name);
        param->name_count = 1;
        param->type = $3;
        param->values = NULL;
        param->is_comptime_param = 1;
        ast_node_free($2);
        $$ = (ASTNode*)param;
    }
```

- [ ] **Step 4: Rebuild and re-check the tripwire (STOP-THE-LINE gate)**

Run: `make lexer && ./scripts/grammar-tripwire.sh`
Expected: build OK; `82 S/R + 256 R/R — baseline exact`.
If the counts changed: STOP. `COMPTIME` already prefixes `comptime {}`, `comptime const`, `comptime func` — a new S/R conflict here is plausible. Follow the goo-grammar skill's conflict-ledger procedure to justify + re-baseline before continuing. Do not proceed on an unexplained delta.

- [ ] **Step 5: Verify parse + flag**

Create `/tmp/t1.goo`:
```goo
package main
func fill(comptime n int, seed int) int { return seed }
func main() { _ = fill(4, 10) }
```
Run: `bin/goo -o /tmp/t1 /tmp/t1.goo`
Expected: it PARSES (it may fail later in type-check/codegen — that's fine for this task; the requirement is no parse error). If you want to assert the flag directly, add a temporary `fprintf(stderr,...)` in `declare_function_signature` where params are walked, run, and remove it. Do NOT leave debug prints.

- [ ] **Step 6: Commit**

```bash
git add include/ast.h src/parser/parser.y src/ast/*.c
git commit --no-gpg-sign -m "feat(parser): comptime value parameter grammar + is_comptime_param AST flag"
```

---

### Task 2: Type-check — reject a runtime argument to a comptime parameter

**Files:**
- Modify: `src/types/expression_checker.c` (the call-argument checking path — find where a user call's arguments are type-checked against the callee signature; `type_check_call`/`type_check_generic_call`)
- Test: `/tmp/t2_*.goo` (committed probe in Task 4)

**Interfaces:**
- Consumes: `VarDeclNode.is_comptime_param` (Task 1); the comptime engine — `comptime_eval_expression(ComptimeContext* ctx, ASTNode* expr) -> ComptimeResult*` where `ComptimeResult.value` is a `ComptimeValue*` and `ComptimeValue.type == COMPTIME_VALUE_INT` carries `int_value` (int64_t). The live context is `checker->comptime_type_ctx->comptime_ctx` (see `type_checker.c` const-decl path ~line 1467).
- Produces: a validated per-comptime-parameter `int64_t` value available to Task 3 (attached to the call node — Task 3 defines the field).

- [ ] **Step 1: Write the failing reject test**

Create `/tmp/t2_reject.goo`:
```goo
package main
func fill(comptime n int, seed int) int { return seed }
func main() {
    x := 5
    _ = fill(x, 10)   // runtime x to a comptime parameter -> must be rejected
}
```
Run: `bin/goo -o /tmp/t2 /tmp/t2_reject.goo 2>&1 | head -2`
Expected NOW (before the change): NOT rejected for the right reason (it compiles, or fails elsewhere). Target: a clear error `argument to comptime parameter 'n' must be a compile-time constant`.

- [ ] **Step 2: Locate the call-argument check**

Read `src/types/expression_checker.c`; find where a call's argument expressions are checked against the callee's parameter list (the arity/argtype checker — the same place `call-argtype-probe` / `pkg-argcheck-probe` are enforced). Identify the callee `FuncDeclNode`/signature so you can see each parameter's `VarDeclNode` and its `is_comptime_param`.

- [ ] **Step 3: Implement the comptime-arg validation**

For each argument position whose parameter has `is_comptime_param == 1`: evaluate the argument through the comptime engine and require an int result. Sketch (adapt to the actual local names in the checker):

```c
// param_vd is the callee parameter VarDeclNode; arg_expr is the call arg ASTNode.
if (param_vd->is_comptime_param) {
    ComptimeContext* raw = checker->comptime_type_ctx ? checker->comptime_type_ctx->comptime_ctx : NULL;
    ComptimeResult* res = raw ? comptime_eval_expression(raw, arg_expr) : NULL;
    int ok = res && res->value && !res->error && res->value->type == COMPTIME_VALUE_INT;
    if (!ok) {
        if (res) comptime_result_free(res);
        type_error(checker, arg_expr->pos,
                   "argument to comptime parameter '%s' must be a compile-time constant",
                   param_vd->names[0]);
        return /* the checker's error value (0 / NULL as the function returns) */;
    }
    /* value is res->value->int_value; keep it for Task 3, then: */
    comptime_result_free(res);
}
```

Include `comptime.h` if not already included.

- [ ] **Step 4: Run the reject test — verify it now fails cleanly**

Run: `bin/goo -o /tmp/t2 /tmp/t2_reject.goo 2>&1 | head -2`
Expected: `... argument to comptime parameter 'n' must be a compile-time constant` and non-zero exit; NO "Module verification failed" (must be a type error, not invalid IR).

- [ ] **Step 5: Verify a comptime-constant argument is accepted (does not regress)**

Create `/tmp/t2_ok.goo`:
```goo
package main
func fill(comptime n int, seed int) int { return seed + n }
func main() { _ = fill(4, 10) }
```
Run: `bin/goo -o /tmp/t2ok /tmp/t2_ok.goo 2>&1 | head -2`
Expected: type-checks past this validation (it may still fail in codegen until Task 3 — acceptable; assert only that it does NOT hit the comptime-constant rejection).

- [ ] **Step 6: Regression + commit**

Run: `make test-golden 2>&1 | tail -1` → `312 passed, 0 failed`.
```bash
git add src/types/expression_checker.c
git commit --no-gpg-sign -m "feat(types): reject non-comptime-constant argument to a comptime parameter"
```

---

### Task 3: Monomorphize per comptime value + bind the parameter as a constant

**Files:**
- Modify: `include/ast.h` (`CallExprNode`, tail): add the comptime value tuple
- Modify: `src/types/expression_checker.c`: record the evaluated comptime values on the call node
- Modify: `src/codegen/monomorphize.c`: include the values in the instance key/name; bind the param as a const in the specialized body
- Modify: `src/types/type_checker.c` (`type_check_function_decl` body pass) and/or the signature builder: bind a comptime parameter's `Variable` with `comptime_value` / `has_const_int_value` / `const_int_value` so `[n]int` and `n`-bounded loops resolve inside the specialized body
- Test: `/tmp/t3_*.goo`

**Interfaces:**
- Consumes: Task 2's validated int values; `codegen_mangle_instance(const char* base, Type* const* args, size_t n)` and `codegen_generate_function_instance(...)` (`monomorphize.c:61,76`); the const-array-length resolver `goo_fold_const_int_ctx(checker, expr, &out)` used for `[N]int` lengths.
- Produces: distinct specialized instances per comptime value tuple, each with the parameter as a compile-time constant.

- [ ] **Step 1: Write the failing single-specialization test**

Create `/tmp/t3.goo`:
```goo
package main
import "fmt"
func fill(comptime n int, seed int) int {
    var buf [n]int
    i := 0
    for i < n {
        buf[i] = seed + i
        i = i + 1
    }
    sum := 0
    j := 0
    for j < n {
        sum = sum + buf[j]
        j = j + 1
    }
    return sum
}
func main() { fmt.Println(fill(4, 10)) }   // 10+11+12+13 = 46
```
Run: `bin/goo -o /tmp/t3 /tmp/t3.goo 2>&1 | head -3`
Expected NOW: FAILS — either `[n]int` non-constant length rejection, or the body can't see `n` as a constant. Target: compiles, runs, prints `46`.

- [ ] **Step 2: Carry comptime values on the call node**

In `include/ast.h`, in `CallExprNode`, at the tail (after `type_args`/`type_arg_count`), add:
```c
    // Comptime-value-specialized calls: the compile-time int value bound to
    // each comptime parameter at THIS call site, in parameter order (0 for
    // non-comptime positions is not stored — index by comptime-param slot).
    // Set by the type checker (Task 2/3), read by the monomorphizer. malloc'd
    // call sites must zero both — tail-appended, no-header-deps convention.
    int64_t* comptime_value_args;
    size_t   comptime_value_arg_count;
```
Grep every `CallExprNode` malloc/`ast_call_expr_new` site and zero the two new fields (mirror how `type_args`/`type_arg_count` are zeroed — the `type_args` doc comment lists the sites).

In Task 2's validation code, instead of discarding the value, append `res->value->int_value` to the call node's `comptime_value_args`.

- [ ] **Step 3: Bind the comptime parameter as a constant in the body**

In the function-body type-check pass (`type_check_function_decl` in `src/types/type_checker.c`, where each parameter is bound as a `Variable` in the body scope), for a parameter with `is_comptime_param == 1`, set on its `Variable` the same fields `comptime const` sets (`type_checker.c` ~1490-1503): `has_const_int_value = 1`, `const_int_value = <the value>`, and `comptime_value = comptime_value_new(COMPTIME_VALUE_INT)` with `int_value` set — so `goo_fold_const_int_ctx` resolves `n` in `[n]int` and constant-folds `n`-bounded loops. During the template body-check (no concrete call yet) a placeholder value is acceptable for type validity; the real per-instance value is applied during monomorphization (Step 4).

- [ ] **Step 4: Specialize per value in the monomorphizer**

In `src/codegen/monomorphize.c`: read `codegen_mangle_instance` (line 61) and `codegen_generate_function_instance` (line 76). Extend the instance identity to include the call's `comptime_value_args`: append the values to the mangled name (e.g. `fill__n4`) so distinct values produce distinct instances and identical values dedup. In `codegen_generate_function_instance`, before generating the body, bind each comptime parameter to its concrete `int64_t` (set the body `Variable`'s `has_const_int_value`/`const_int_value`/`comptime_value` to this instance's value) so `[n]int` and `n`-loops lower with the literal. Follow the existing type-arg threading pattern exactly — comptime values are a second key axis alongside `type_args`.

- [ ] **Step 5: Run the single-specialization test**

Run: `bin/goo -o /tmp/t3 /tmp/t3.goo && /tmp/t3`
Expected: `46`.

- [ ] **Step 6: Two distinct specializations**

Append to `/tmp/t3.goo`'s main: `fmt.Println(fill(2, 100))` (100+101 = 201). Rebuild + run.
Expected: `46` then `201`. Then confirm two instances in IR:
Run: `bin/goo --emit-llvm -o /tmp/t3ir /tmp/t3.goo >/dev/null 2>&1; grep -oE "fill__n[0-9]+|alloca \[[0-9]+ x i" /tmp/t3ir.ll | sort -u`
Expected: two distinct instance names / two distinct array sizes (`[4 x i…]` and `[2 x i…]`) — proving specialization, not a shared runtime-`n` body.

- [ ] **Step 7: Regression + commit**

Run: `make test-golden 2>&1 | tail -1` (312/0) and `make lexer` clean.
```bash
git add include/ast.h src/types/expression_checker.c src/types/type_checker.c src/codegen/monomorphize.c src/ast/*.c
git commit --no-gpg-sign -m "feat(codegen): monomorphize functions per comptime value; bind comptime params as constants"
```

---

### Task 4: Demo golden + reject probe, wired into verify

**Files:**
- Create: `examples/comptime_value_specialize_probe.goo` (+ `.expected.txt`)
- Modify: `Makefile` (`comptime-value-reject-probe` target + add both to `verify`)
- Test: the two probes themselves

**Interfaces:**
- Consumes: the end-to-end feature from Tasks 1-3.

- [ ] **Step 1: Golden probe**

Create `examples/comptime_value_specialize_probe.goo`:
```goo
// Comptime-value-specialized functions: `n` is a comptime parameter, so
// `var buf [n]int` (a fixed array whose length must be a compile-time
// constant) is legal and the function is specialized per value.
package main

import "fmt"

func fill(comptime n int, seed int) int {
    var buf [n]int
    i := 0
    for i < n {
        buf[i] = seed + i
        i = i + 1
    }
    sum := 0
    j := 0
    for j < n {
        sum = sum + buf[j]
        j = j + 1
    }
    return sum
}

func main() {
    fmt.Println(fill(4, 10))  // 46
    fmt.Println(fill(2, 100)) // 201
}
```
Create `examples/comptime_value_specialize_probe.expected.txt`:
```
46
201
```

- [ ] **Step 2: Run it via the golden harness**

Run: `make test-golden 2>&1 | grep comptime_value_specialize_probe`
Expected: `PASS comptime_value_specialize_probe`.

- [ ] **Step 3: Reject probe (Makefile target)**

In `Makefile`, add (model on `nonconst-arraylen-reject-probe`):
```make
# Comptime-value params: a runtime value to a comptime parameter is a clean
# type error (not invalid IR).
comptime-value-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== comptime-value-reject-probe: runtime arg to comptime param fails cleanly ==="
	@printf 'package main\nfunc fill(comptime n int, s int) int { return s }\nfunc main() { x := 5; _ = fill(x, 1) }\n' > build/cvr.goo
	@"$(COMPILER)" build/cvr.goo -o build/cvr.out 2>build/cvr.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "comptime-value-reject-probe: FAIL (compiled a runtime comptime arg)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/cvr.err; then echo "comptime-value-reject-probe: FAIL (invalid IR reached verifier)"; cat build/cvr.err; exit 1; fi; \
	  if grep -qiE "compile-time constant|comptime parameter" build/cvr.err; then echo "comptime-value-reject-probe: PASS"; else echo "comptime-value-reject-probe: FAIL (no clean diagnostic)"; cat build/cvr.err; exit 1; fi
```
Add `comptime-value-reject-probe` to the `.PHONY` line and to the `verify` prerequisite list (next to `nonconst-arraylen-reject-probe`).

- [ ] **Step 4: Run the reject probe**

Run: `make comptime-value-reject-probe`
Expected: `comptime-value-reject-probe: PASS`.

- [ ] **Step 5: Full regression + tripwire + commit**

Run: `make test-golden 2>&1 | tail -1` (313/0 with the new probe), `./scripts/grammar-tripwire.sh` (82/256), and the two analysis/arena test suites unaffected.
```bash
git add examples/comptime_value_specialize_probe.goo examples/comptime_value_specialize_probe.expected.txt Makefile
git commit --no-gpg-sign -m "test: comptime-value specialization golden + reject probe wired into verify"
```

---

## Self-review

- **Spec coverage:** Surface/semantics → Tasks 1-3. Grammar/AST → Task 1. Type checking (validate + bind) → Tasks 2-3. Monomorphization → Task 3. Success criterion (`[n]int` forcing function, two specializations, IR literal check) → Tasks 3-4. Reject case → Tasks 2, 4. Scope §6 (int-only, explicit, single-axis) → respected (no `[T]`-composition task; no comptime control-flow task). Testing section → Task 4. Every spec section maps to a task.
- **Placeholder scan:** The compiler-internals steps (Task 3 Step 4) give exact APIs + read-anchors + a concrete pass/fail test rather than fabricated internal code — deliberate for a subagent that reads the function during its task; the TEST pins the behavior. No "TBD"/"handle edge cases"/vague steps.
- **Type consistency:** `is_comptime_param` (Task 1) is the name used in Tasks 2/3; `comptime_value_args`/`comptime_value_arg_count` defined in Task 3 Step 2 and used in Step 4; `comptime_eval_expression`/`ComptimeResult.value`/`ComptimeValue.int_value` used consistently and match `include/comptime.h`.
