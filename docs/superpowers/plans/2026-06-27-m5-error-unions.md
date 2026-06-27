# M5 — Error Unions (`!T`) String-Error Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Goo's heavily-scaffolded-but-never-run `!T` error-union *string-error core* compile and run end-to-end, probe-gated, with zero regressions to the existing 21-probe suite.

**Architecture:** *Verify-and-fix*, not greenfield. The grammar, AST, type system, type checker, and most codegen for `!T`/`catch` already exist. Each task writes a CI probe (the failing test), runs it to discover what actually breaks, fixes the specific typecheck/codegen stage, and wires the probe into `make verify` + CI. No new grammar. Error value = a **string message**.

**Tech Stack:** C23, LLVM 22 C API, GNU Make, bison/flex parser, bash diff-based probe harness, GitHub Actions.

## Global Constraints

- **No new grammar.** `!T`, `expr catch e { … }`, and `error(...)` (an ordinary call) already parse. Touch `src/parser/*.y` only to *verify*. (Spec §2 decision 3.)
- **Error model = string message.** `!T` carries a value or an error **message string**. The type checker already defaults the `catch` variable to `TYPE_STRING` (`expression_checker.c:825`) and types a `catch` expression as the unwrapped value type `T` (`expression_checker.c:839`). (Spec §2 decision 1.)
- **Scope (locked):** declare `func f() !T`; `return v` (success auto-wrap); `return error("msg")` (builtin); `x := expr catch e { … }`. OUT OF SCOPE (do NOT implement): `try` propagation, typed/enum errors & payloads, custom error types, matching on error kind. (Spec §5.)
- **Representation:** `!T` = `{ i1 is_error, { T value, string msg } }` (existing `codegen_get_error_union_type`, `type_mapping.c:259`; default error slot `i8*`). success → `{is_error=false,{value=v, msg=undef}}`; error → `{is_error=true,{value=zero, msg="…"}}`. (Spec §4.)
- **`catch` semantics:** ok → `x = value` (type `T`); error → bind `e : string`, run block; block typically diverges (`return`); if it falls through, `x` = zero value of `T` (total). (Spec §5.)
- **Reuse M4's centralized integer width-adjust.** M4 centralized SExt/Trunc-to-slot-type inside `codegen_create_nullable_with_value`. The error-union value-wrap (`codegen_create_error_union_value`, `type_mapping.c:315`) currently has none — apply the same so `!int64` returning an i32 literal is well-typed. (Spec §4; Task 3.)
- **Build facts:** compiler binary is `bin/goo`; runtime `lib/libgoo_runtime.a`. Build: `make goo lib/libgoo_runtime.a`. **After editing any header** (`include/*.h`): `make clean && make goo`. `make verify` halts at `ccomp-build` (CompCert env gap) — IGNORE; the real gate is the explicit CI probe list.
- **CI wiring:** every new probe is added to BOTH the `verify:` target in `Makefile` AND the probe list in `.github/workflows/tests.yml:54`.
- **CI billing caveat:** GitHub Actions on `dd0wney/goolang` may be billing-blocked (jobs show red but never start, ~3s). Authoritative verification is LOCAL: `make test` + the probe list + `opt --passes=verify` on emitted IR. (`bin/goo --emit-llvm <f>.goo` writes textual IR to `<f>.ll`.)
- **clang/LSP false positives:** "header not found"/"unknown type" diagnostics are NOT real (the build uses `-Iinclude`). Trust `make`.

## Reference map (where things live)

| Concern | Location | Notes |
|---|---|---|
| `!T` type node → `TYPE_ERROR_UNION` | `src/types/type_checker.c:1249-1260` | `type_from_ast` for `AST_ERROR_UNION_TYPE` |
| `catch` typing (binds `e:string`, yields `T`) | `src/types/expression_checker.c:800-842` | error-var added to scope in typecheck |
| builtin recognition pattern (mirror for `error`) | typecheck `expression_checker.c:506,533`; codegen `call_codegen.c:76,94,110` | `strcmp(name,"len"/"cap"/"append")` in BOTH stages |
| error-union LLVM type `{i1,{value,err}}` | `src/codegen/type_mapping.c:259` | default error slot `i8*` |
| construct error-union value | `src/codegen/type_mapping.c:315` `codegen_create_error_union_value` | **no width-adjust** (Task 3) |
| extract value/error; is_error check | `type_mapping.c:338,348` | |
| **catch codegen** (mostly there) | `src/codegen/error_union_codegen.c:143` `codegen_generate_catch_expr_impl` | **error-var binding is a TODO** (~line 178); treats `catch_body` as an EXPRESSION though grammar parses a BLOCK; PHI-merge assumes both paths yield a value (breaks on divergence — M4 terminator lesson) |
| error-union FUNCTION codegen | `src/codegen/error_union_codegen.c` (`codegen_generate_error_union_function`) | verify `return v` auto-wraps to `{is_error=false,…}` |
| catch/try dispatch | `src/codegen/expression_codegen.c:26-29` | |
| M4 centralized width-adjust (pattern to copy) | `src/codegen/nullable_codegen.c` `codegen_create_nullable_with_value` | SExt/Trunc inner value to slot type before InsertValue |
| `error_union_codegen.c` IS linked | `Makefile:65` `CODEGEN_SRCS` | no M4-style linker trap |
| Probe pattern to copy | `examples/int64_probe.goo` + `.expected.txt`, Makefile `int64-probe:` | |
| `try` (DEFERRED — do not implement) | `expression_checker.c:778`, `error_union_codegen.c:97` | M6 |

---

## Task 1: Declare `!T` + success construct + `catch` (ok path) — `erru-catch-probe`

**Files:**
- Create: `examples/erru_catch_probe.goo`, `examples/erru_catch_probe.expected.txt`
- Modify: `Makefile` (add `erru-catch-probe:`; add to `verify:`), `.github/workflows/tests.yml:54`
- Likely fix (spike-confirmed): `src/codegen/error_union_codegen.c` (success auto-wrap in the error-union function return; `catch` ok-path; `catch_body` block handling), possibly `src/types/*` for `return v` typing in a `!T` function

**Interfaces:**
- Consumes: nothing (first task).
- Produces: a working `!int` declare + success construct + `catch` ok-path that Tasks 2–3 build on. No new public C signatures expected; reuse `codegen_error_union_*` helpers and `codegen_get_error_union_type`.

- [ ] **Step 1: Write the failing test (probe + expected)**

Create `examples/erru_catch_probe.goo`:

```go
// erru_catch_probe: !int declare + success construct (return v) observed via
// `catch` (ok path yields the unwrapped value). Foundational: an !T cannot be
// observed without catch. (error path is Task 2.)
package main

import "fmt"

func alwaysOk() !int {
    return 42
}

func main() {
    x := alwaysOk() catch e {
        fmt.Println("FAIL: ok value took catch", e)
        return
    }
    fmt.Println(x)
}
```

Create `examples/erru_catch_probe.expected.txt`:

```
42
```

- [ ] **Step 2: Build and run to confirm failure**

```bash
make goo lib/libgoo_runtime.a
mkdir -p build
bin/goo -o build/erru_catch_probe examples/erru_catch_probe.goo && ./build/erru_catch_probe
```

Expected: a **failure** — a typecheck error (`return 42` not accepted as `!int`), a codegen error, or an LLVM `verifyModule` failure (the catch PHI/merge or the success auto-wrap). Capture the exact text; it drives Step 3.

- [ ] **Step 3: Discovery spike — diagnose**

Triage the Step 2 failure:
- **Success auto-wrap:** does `codegen_generate_error_union_function` wrap `return 42` into `{is_error=false, {value=42, msg=undef}}`? If the raw i32 is returned (or it's ill-typed against the `{i1,{i32,i8*}}` return), fix the return path to build the success error-union via `codegen_create_error_union_value(union_type, value, /*is_error=*/0)` (`type_mapping.c:315`).
- **`catch` ok path:** `codegen_generate_catch_expr_impl` (`error_union_codegen.c:143`) branches on `is_error`; the success block extracts the value. Confirm the ok path's PHI yields the value cleanly.
- **`catch_body` is a BLOCK:** the impl calls `codegen_generate_expression(... catch_body)` but the grammar parses `catch_body` as a `block` (statement list). On the ok path the body isn't executed, so this may not bite here — but note it for Task 2. If it causes a failure now, route a block body through `codegen_generate_statement`/`codegen_generate_block_stmt` instead.

Record findings in the PR description. No code change in this step.

- [ ] **Step 4: Implement the minimal fix**

Apply the smallest change that makes the probe pass (typically: success auto-wrap in the error-union function return; ensure the `catch` ok path returns the unwrapped value). Keep helper prototypes unchanged. `make clean && make goo` if a header changed.

- [ ] **Step 5: Run to verify pass**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/erru_catch_probe examples/erru_catch_probe.goo
./build/erru_catch_probe | diff -u examples/erru_catch_probe.expected.txt -
```

Expected: no diff, exit 0. Also confirm well-typed IR:

```bash
bin/goo --emit-llvm examples/erru_catch_probe.goo && opt --passes=verify -disable-output examples/erru_catch_probe.ll && echo "IR verify CLEAN"
```

- [ ] **Step 6: Add the Makefile probe target**

Add to `Makefile` (mirror `int64-probe:`):

```make
erru-catch-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== erru-catch-probe: !int declare + success + catch ok path ==="
	$(COMPILER) -o build/erru_catch_probe examples/erru_catch_probe.goo
	@./build/erru_catch_probe > build/erru_catch_probe.actual.txt
	@if diff -u examples/erru_catch_probe.expected.txt build/erru_catch_probe.actual.txt; then \
	  echo "erru-catch-probe: PASS"; \
	else \
	  echo "erru-catch-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
```

Append `erru-catch-probe` to the `verify:` dependency list.

- [ ] **Step 7: Wire into CI**

Append `erru-catch-probe` to the probe list on `.github/workflows/tests.yml:54`.

- [ ] **Step 8: Verify the whole gate stays green (no regressions)**

```bash
make CC=gcc-14 LLVM_CONFIG="${LLVMCFG:-llvm-config}" \
  baseline-probe lvalue-probe file-io-probe pointer-probe pointer-write-probe \
  switch-probe methods-probe new-probe enum-probe match-probe append-probe \
  cap-probe map-probe int64-probe commaok-probe guard-probe \
  nullable-iflet-probe nullable-nilcmp-probe nullable-abi-probe \
  nullable-intret-probe nullable-assign-probe erru-catch-probe
```

Expected: every probe prints `PASS`; exit 0.

- [ ] **Step 9: Commit**

```bash
git add examples/erru_catch_probe.goo examples/erru_catch_probe.expected.txt Makefile .github/workflows/tests.yml src/
git commit --no-gpg-sign -m "feat(erru): wire !int declare + success + catch ok path (erru-catch-probe)"
```

---

## Task 2: `error("msg")` builtin + `catch` error path — `erru-error-probe`

**Files:**
- Create: `examples/erru_error_probe.goo`, `examples/erru_error_probe.expected.txt`
- Modify: `Makefile`, `.github/workflows/tests.yml:54`
- Likely fix (spike-confirmed): `src/types/expression_checker.c` (recognize `error(string)` builtin → `!T` from context), `src/codegen/call_codegen.c` (construct the error case), `src/codegen/error_union_codegen.c` (bind the error var `e` in the catch error block — the existing TODO; handle a diverging block; route `catch_body` as a block)

**Interfaces:**
- Consumes: Task 1's `!int` + `catch` ok path.
- Produces: `error("msg")` constructing `{is_error=true, msg="msg"}`; `catch` error path binding `e:string`. No new public signatures; reuse `codegen_create_error_union_value(...,/*is_error=*/1)` and `codegen_error_union_get_error`.

- [ ] **Step 1: Write the failing test (probe + expected)**

Create `examples/erru_error_probe.goo`:

```go
// erru_error_probe: error("msg") builtin + catch error path (bind e, print it,
// yield a fallback) and a catch-with-return divergence case.
package main

import "fmt"

func mightFail(bad bool) !int {
    if bad {
        return error("boom")
    }
    return 7
}

func main() {
    // error path: bind e, print message, yield fallback value
    a := mightFail(true) catch e {
        fmt.Println("caught:", e)
        return
    }
    fmt.Println("unreachable", a)
}
```

Create `examples/erru_error_probe.expected.txt`:

```
caught: boom
```

(The `catch` block diverges via `return`, so the `unreachable` line never prints — this exercises the divergence/terminator path. A fallthrough-to-zero-value case is added in Step 4 once the error binding works.)

- [ ] **Step 2: Build and run to confirm failure**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/erru_error_probe examples/erru_error_probe.goo && ./build/erru_error_probe
```

Expected: failure — most likely "undefined function error" (the builtin isn't recognized) at typecheck, or — once that's past — the catch block can't see `e` (the binding TODO), or a terminator failure because the catch block `return`s but the impl still tries to `br` to the merge/PHI. Capture the exact symptom.

- [ ] **Step 3: Discovery spike — diagnose**

- **`error()` builtin (typecheck):** add to `expression_checker.c` (mirror the `len`/`cap`/`append` recognition at `:506,533`): a call to `error` with one `string` argument types as the error-union `!T` expected by context (the enclosing function's return type, or the `catch` operand type). 
- **`error()` builtin (codegen):** add to `call_codegen.c` (mirror `:76-110`): build the error case via `codegen_create_error_union_value(union_type, msg_value, /*is_error=*/1)`.
- **Error-var binding (the TODO):** in `codegen_generate_catch_expr_impl` (`error_union_codegen.c:~178`), bind `catch_expr->error_var` to the extracted error value (`codegen_error_union_get_error`) in the error block's scope (alloca + store + `codegen_add_value` + checker scope var), mirroring how M4's if-let binds its var (`statement_codegen.c:62-73`).
- **Block body + divergence:** route `catch_body` through `codegen_generate_statement`/block, and when the error block ends in a terminator (e.g. `return`), do NOT emit the `br` to merge and exclude it from the PHI (the M4 terminator lesson). Confirm the PHI only includes branches that actually reach merge.

Record findings in the PR description.

- [ ] **Step 4: Implement the minimal fix**

Implement the `error()` builtin (typecheck + codegen), the catch error-var binding, block-body handling, and divergence-safe merge. Then extend the probe with a fallthrough case to lock in the zero-value semantics:

Append to `examples/erru_error_probe.goo`'s `main` (before/after the diverging case, adjusting so output stays deterministic) a non-diverging catch:

```go
    b := mightFail(true) catch e {
        fmt.Println("recovering from:", e)
    }
    fmt.Println("fallback:", b)
```

and update `examples/erru_error_probe.expected.txt` accordingly (e.g. add `recovering from: boom` and `fallback: 0`). Keep the diverging case too (split into two functions/sequences if needed so both run deterministically). `make clean && make goo` if a header changed.

- [ ] **Step 5: Run to verify pass**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/erru_error_probe examples/erru_error_probe.goo
./build/erru_error_probe | diff -u examples/erru_error_probe.expected.txt -
bin/goo --emit-llvm examples/erru_error_probe.goo && opt --passes=verify -disable-output examples/erru_error_probe.ll && echo "IR verify CLEAN"
```

Expected: no diff, exit 0, IR verify clean.

- [ ] **Step 6: Add the Makefile probe target**

```make
erru-error-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== erru-error-probe: error(\"msg\") builtin + catch error path ==="
	$(COMPILER) -o build/erru_error_probe examples/erru_error_probe.goo
	@./build/erru_error_probe > build/erru_error_probe.actual.txt
	@if diff -u examples/erru_error_probe.expected.txt build/erru_error_probe.actual.txt; then \
	  echo "erru-error-probe: PASS"; \
	else \
	  echo "erru-error-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
```

Append `erru-error-probe` to `verify:`.

- [ ] **Step 7: Wire into CI**

Append `erru-error-probe` to the probe list on `.github/workflows/tests.yml:54`.

- [ ] **Step 8: Verify the whole gate stays green**

Run the Step-8 command from Task 1 with `erru-error-probe` appended. Expected: all `PASS`, exit 0.

- [ ] **Step 9: Commit**

```bash
git add examples/erru_error_probe.goo examples/erru_error_probe.expected.txt Makefile .github/workflows/tests.yml src/
git commit --no-gpg-sign -m "feat(erru): error(\"msg\") builtin + catch error-var binding (erru-error-probe)"
```

---

## Task 3: ABI + width — `erru-abi-probe`

**Files:**
- Create: `examples/erru_abi_probe.goo`, `examples/erru_abi_probe.expected.txt`
- Modify: `Makefile`, `.github/workflows/tests.yml:54`
- Likely fix (spike-confirmed): `src/codegen/type_mapping.c:315` `codegen_create_error_union_value` (centralize integer SExt/Trunc-to-slot-type, mirroring M4's `codegen_create_nullable_with_value`); possibly the error-union function return ABI path for `!BigStruct`

**Interfaces:**
- Consumes: Tasks 1–2 (success construct, `error()`, `catch`).
- Produces: error-unions of a >16-byte value type and of `int64` correct across function boundaries. No new public signatures; reuse the existing struct-by-pointer ABI path and the centralized width-adjust.

- [ ] **Step 1: Write the failing test (probe + expected)**

Create `examples/erru_abi_probe.goo` (`Point` = 3×int64 = 24 bytes; `!int64` exercises width):

```go
// erru_abi_probe: error-union of a >16-byte struct across function boundaries,
// plus !int64 value-slot width (i64, not i32). Observed via catch.
package main

import "fmt"

type Point struct {
    X int64
    Y int64
    Z int64
}

func makePoint(bad bool) !Point {
    if bad {
        return error("no point")
    }
    return Point{X: 1, Y: 2, Z: 3}
}

func wide(bad bool) !int64 {
    if bad {
        return error("no wide")
    }
    return 5
}

func main() {
    p := makePoint(false) catch e {
        fmt.Println("FAIL point:", e)
        return
    }
    fmt.Println(p.X + p.Y + p.Z)

    w := wide(false) catch e {
        fmt.Println("FAIL wide:", e)
        return
    }
    fmt.Println(w)

    bad := makePoint(true) catch e {
        fmt.Println("caught:", e)
        return
    }
    fmt.Println("unreachable", bad.X)
}
```

Create `examples/erru_abi_probe.expected.txt`:

```
6
5
caught: no point
```

- [ ] **Step 2: Build and run to confirm failure**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/erru_abi_probe examples/erru_abi_probe.goo && ./build/erru_abi_probe
```

Expected: failure — most likely a wrong `wide` value or an `insertvalue` type mismatch (i32 `5` into the i64 value slot → `opt verify` failure), and/or a garbled `Point` sum from the large-struct ABI. Capture the exact symptom (and the verifier error if any).

- [ ] **Step 3: Discovery spike — diagnose**

- **Width:** `codegen_create_error_union_value` (`type_mapping.c:315`) inserts the value with no width adjustment, so `return 5` (i32) into a `!int64` value slot (i64) is ill-typed. Mirror M4's fix in `codegen_create_nullable_with_value`: SExt/Trunc the value to the slot's element type before `LLVMBuildInsertValue`. Centralize here so success-wrap, `error()`-wrap, and returns all benefit.
- **ABI:** compare against M4's `?Point` (`nullable-abi-probe`) and the slice/`m12` precedent. Confirm the `!Point` return crosses the function boundary correctly once the IR is well-typed (per M4, LLVM's backend handles >16-byte by-pointer automatically).

Record findings in the PR description.

- [ ] **Step 4: Implement the minimal fix**

Add the integer width adjustment inside `codegen_create_error_union_value` (gated to integer-type mismatches; structs/matching widths untouched). `make clean && make goo` if a header changed.

- [ ] **Step 5: Run to verify pass**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/erru_abi_probe examples/erru_abi_probe.goo
./build/erru_abi_probe | diff -u examples/erru_abi_probe.expected.txt -
bin/goo --emit-llvm examples/erru_abi_probe.goo && opt --passes=verify -disable-output examples/erru_abi_probe.ll && echo "IR verify CLEAN"
```

Expected: no diff, exit 0, IR verify clean.

- [ ] **Step 6: Add the Makefile probe target**

```make
erru-abi-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== erru-abi-probe: !BigStruct ABI + !int64 width across functions ==="
	$(COMPILER) -o build/erru_abi_probe examples/erru_abi_probe.goo
	@./build/erru_abi_probe > build/erru_abi_probe.actual.txt
	@if diff -u examples/erru_abi_probe.expected.txt build/erru_abi_probe.actual.txt; then \
	  echo "erru-abi-probe: PASS"; \
	else \
	  echo "erru-abi-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
```

Append `erru-abi-probe` to `verify:`.

- [ ] **Step 7: Wire into CI**

Append `erru-abi-probe` to the probe list on `.github/workflows/tests.yml:54`.

- [ ] **Step 8: Verify the whole gate stays green (full M5 suite)**

Run the Task-1 Step-8 command with `erru-catch-probe erru-error-probe erru-abi-probe` all appended (24 probes total). Expected: all `PASS`, exit 0.

- [ ] **Step 9: Annotate the deferred demo and commit**

Add a header comment to `examples/ergonomic_error_handling_demo.goo` marking it as a later-milestone target (it uses deferred `try` / custom error enums / `@error_hierarchy` and will not compile under M5):

```go
// M6+-TARGET (NOT M5): uses `try` propagation, custom error enums, and
// @error_hierarchy — all deferred. Does not compile under M5's string-error core.
```

```bash
git add examples/erru_abi_probe.goo examples/erru_abi_probe.expected.txt Makefile .github/workflows/tests.yml examples/ergonomic_error_handling_demo.goo src/
git commit --no-gpg-sign -m "feat(erru): !BigStruct ABI + !int64 width (erru-abi-probe); mark demo M6+"
```

---

## Final verification (after all three tasks)

- [ ] All 24 probes (21 existing + 3 new) green via the Task-3 Step-8 command.
- [ ] `make test` (unit suite) green — the other half of the CI `unit-tests` job.
- [ ] `opt --passes=verify` clean on all three new probes' IR.
- [ ] If CI is reachable (not billing-blocked), confirm green by reading the rollup (`gh pr view --json statusCheckRollup`), never a piped exit. If billing-blocked, local verification above is authoritative.
- [ ] Spec §9 success criteria all met.

## Spec coverage self-check

| Spec §5 element | Task |
|---|---|
| declare `func f() !T` | 1 (`!int`), 3 (`!Point`, `!int64`) |
| `return v` success auto-wrap | 1, 3 |
| `return error("msg")` builtin | 2, 3 |
| `x := expr catch e { … }` ok path | 1, 3 |
| `catch` error path (bind `e`, run block) | 2, 3 |
| `catch` divergence (return) + zero-value fallthrough | 2 |
| `!BigStruct` ABI + `!int64` width | 3 |
| deferred surface documented (demo → M6+) | 3 (Step 9) |
