# M4 — Nullable Types (`?T`) Safe Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Goo's already-plumbed-but-never-run `?T` nullable safe-core actually compile and run end-to-end, probe-gated, with zero regressions to the existing 21-probe suite.

**Architecture:** This is *verify-and-fix*, not greenfield. The AST, grammar, type system, and most codegen for `?T` already exist. Each task writes a CI probe (the failing test), runs it to discover what actually breaks when the scaffolding first executes, fixes the specific typecheck/codegen stage, and wires the probe into `make verify` + CI. No new grammar is added.

**Tech Stack:** C23, LLVM 22 C API, GNU Make, bison/flex parser, bash diff-based probe harness, GitHub Actions.

## Global Constraints

- **No new grammar.** `?T`, `if let`, and `nil` already parse. Touch `src/parser/*.y` only to *verify*, never to add syntax. (Spec §2 decision 3.)
- **Safe-only surface.** In scope: declare `?T`, assign a `T` value (auto-wrap), assign `nil`, `if let x = e { } else { }`, `x == nil` / `x != nil`. Out of scope (M5): `x!`, `x?`, `x ?? d`, `!T`, `try`. (Spec §5.)
- **Representation:** `?T` = LLVM struct `{ i1 is_null, T value }`. `nil` → `{true, undef}`; value `v` → `{false, v}`; default-zero is `nil`. (Spec §4.)
- **ABI rule:** when `sizeof({i1, T}) > 16 bytes`, the nullable crosses function-call / codegen↔C boundaries **by pointer, not by value** (the `m12_probe` / slice-ABI precedent). Must be exercised, not assumed. (Spec §4.)
- **Build facts:** compiler binary is `bin/goo`; runtime is `lib/libgoo_runtime.a`. Build with `make goo` + `make lib/libgoo_runtime.a`. **After editing any header** (`runtime.h`/`types.h`/`codegen.h`/`ast.h`): `make clean && make goo` (incremental misses header deps). `make verify` halts at `ccomp-build` (CompCert env gap) — IGNORE; the real gate is the explicit CI probe list.
- **CI wiring:** every new probe is added to BOTH the `verify:` target in `Makefile` AND the probe list in `.github/workflows/tests.yml:54`.
- **Cadence:** one PR per task off updated `main`; confirm CI green by reading the status rollup before claiming done.

## Deviation from spec §6 (intentional, coverage-preserving)

The spec's §6 split was `nullable-decl-probe` (decl+ABI) → `nullable-iflet-probe` → `nullable-nilcmp-probe`. During file-level analysis we found a **nullable value cannot be observed without an unwrap/compare primitive** — so "declare/construct" is not independently testable. This plan therefore:

- **Task 1 (`nullable-iflet-probe`)** pairs construction (declare/assign/nil) with `if let` (the observation primitive that already works), establishing the core.
- **Task 2 (`nullable-nilcmp-probe`)** isolates `== nil` / `!= nil` (the primitive most likely to need new codegen).
- **Task 3 (`nullable-abi-probe`)** isolates the >16-byte ABI landmine in its own review gate (highest risk), using both primitives from Tasks 1–2 to observe.

Every surface element and the ABI case from spec §5/§6 remains covered. The change improves dependency-ordering and isolates the riskiest piece for its own reviewer gate (per writing-plans task-right-sizing).

## Reference map (where things live)

| Concern | Location | Notes |
|---|---|---|
| `?T` type node → `TYPE_NULLABLE` | `src/types/type_checker.c:1263` | `type_from_ast` for `AST_NULLABLE_TYPE` |
| `if-let` typing | `src/types/type_checker.c:736` | requires nullable, binds base type |
| `nil` assignable to nullable | `src/types/expression_checker.c:277` | |
| **Real** if-let codegen (USE THIS) | `src/codegen/statement_codegen.c:25-86` | solid: scope push, alloca, binds var |
| **Dead** if-let helper (AVOID) | `src/codegen/nullable_codegen.c:118` | unfinished var-binding TODO; likely unused |
| nullable construct / wrap | `src/codegen/nullable_codegen.c:11,233` | `create_nullable_with_value`, `..._assignment` |
| nil-literal codegen | `src/codegen/nullable_codegen.c:263` | **needs expected nullable type threaded in** |
| is_null / get_value helpers | `src/codegen/nullable_codegen.c:50,58` | extract index 0 / 1 |
| ABI precedent | `examples/m12_probe.goo`, Makefile `m12-probe` | >16B structs by pointer |
| Probe pattern to copy | `examples/int64_probe.goo` + `.expected.txt`, Makefile `int64-probe:` | |

---

## Task 1: Core nullable — construct + `if let` observe (`?int`)

**Files:**
- Create: `examples/nullable_iflet_probe.goo`
- Create: `examples/nullable_iflet_probe.expected.txt`
- Modify: `Makefile` (add `nullable-iflet-probe:` target; add to `verify:` deps)
- Modify: `.github/workflows/tests.yml:54` (append `nullable-iflet-probe` to probe list)
- Likely fix (spike-confirmed): `src/codegen/nullable_codegen.c` (nil-literal expected-type threading), `src/codegen/expression_codegen.c` and/or `src/codegen/statement_codegen.c` (var-decl init wrap / nil context), `src/types/type_checker.c` / `expression_checker.c` (auto-wrap typing)

**Interfaces:**
- Consumes: nothing (first task).
- Produces: a working `?int` construction + `if let` path that Tasks 2 and 3 rely on for observation. No new public C signatures expected; if the spike requires threading the target type into nil codegen, reuse the existing `codegen_generate_null_literal(codegen, checker, expected_type)` signature at `nullable_codegen.c:263` — do not change its prototype.

- [ ] **Step 1: Write the failing test (the probe + expected output)**

Create `examples/nullable_iflet_probe.goo`:

```go
// nullable_iflet_probe: ?int safe core — declare, assign a value, assign nil,
// and observe both via `if let` (the value path binds; the nil path takes else).
// Establishes the construction + observation primitives the rest of M4 builds on.
package main

import "fmt"

func main() {
    var a ?int = 42
    if let v = a {
        fmt.Println(v)
    } else {
        fmt.Println("FAIL: value took else")
    }

    var b ?int = nil
    if let v = b {
        fmt.Println(v)
    } else {
        fmt.Println("PASS: nil took else")
    }
}
```

Create `examples/nullable_iflet_probe.expected.txt`:

```
42
PASS: nil took else
```

- [ ] **Step 2: Build the compiler and run the probe to confirm it fails**

```bash
make goo lib/libgoo_runtime.a
mkdir -p build
bin/goo -o build/nullable_iflet_probe examples/nullable_iflet_probe.goo && ./build/nullable_iflet_probe
```

Expected: a **failure** — either a compile error (parse/typecheck/codegen) or wrong/garbled output, or an LLVM `verifyModule` failure. This proves the scaffolding has never run. Capture the exact failure text; it drives Step 3.

- [ ] **Step 3: Discovery spike — diagnose the actual breakage**

Triage the Step 2 failure into one of:
- **Parse error** → confirm `var a ?int = 42` and `if let v = a { } else { }` actually parse (grammar says they should). If the probe uses a syntax Goo doesn't accept, adjust the probe to the accepted in-scope equivalent (e.g. explicit two-line declare-then-assign) — staying within the safe-only surface. Do NOT add grammar.
- **Typecheck error** → likely auto-wrap of `42` into `?int` (check `expression_checker.c:277` region and the var-decl init path in `type_checker.c`).
- **Codegen error / bad output** → most likely `nil` lowering: `codegen_generate_null_literal` (`nullable_codegen.c:263`) only emits `{is_null=true,…}` when handed the expected nullable type. Verify the var-decl-with-initializer path passes the target `?int` type into nil codegen. Also confirm the real if-let path (`statement_codegen.c:25`) is the one taken (NOT the dead helper at `nullable_codegen.c:118`).

Write findings as a one-paragraph note in the PR description. This step has no code change of its own; it scopes Step 4.

- [ ] **Step 4: Implement the minimal fix**

Apply the smallest change that makes the probe pass, in the stage identified by Step 3. Typical expected fix: thread the declared `?int` type into the initializer/nil codegen so `var b ?int = nil` produces `{is_null=true, undef}` and `var a ?int = 42` produces `{is_null=false, 42}`. Keep `codegen_generate_null_literal`'s existing prototype. If a header was edited, `make clean && make goo`.

- [ ] **Step 5: Run the probe to verify it passes**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/nullable_iflet_probe examples/nullable_iflet_probe.goo
./build/nullable_iflet_probe | diff -u examples/nullable_iflet_probe.expected.txt -
```

Expected: no diff output, exit 0.

- [ ] **Step 6: Add the Makefile probe target**

Add to `Makefile` (mirror the `int64-probe:` target exactly):

```make
nullable-iflet-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== nullable-iflet-probe: ?int construct + if-let observe ==="
	$(COMPILER) -o build/nullable_iflet_probe examples/nullable_iflet_probe.goo
	@./build/nullable_iflet_probe > build/nullable_iflet_probe.actual.txt
	@if diff -u examples/nullable_iflet_probe.expected.txt build/nullable_iflet_probe.actual.txt; then \
	  echo "nullable-iflet-probe: PASS"; \
	else \
	  echo "nullable-iflet-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
```

Append `nullable-iflet-probe` to the `verify:` target's dependency list.

- [ ] **Step 7: Wire into CI**

In `.github/workflows/tests.yml`, append `nullable-iflet-probe` to the end of the probe list on line 54 (the `make ... <probes>` invocation under "Language probes").

- [ ] **Step 8: Verify the whole gate stays green (no regressions)**

```bash
make CC=gcc-14 LLVM_CONFIG="${LLVMCFG:-llvm-config}" \
  baseline-probe lvalue-probe file-io-probe pointer-probe pointer-write-probe \
  switch-probe methods-probe new-probe enum-probe match-probe append-probe \
  cap-probe map-probe int64-probe commaok-probe guard-probe nullable-iflet-probe
```

Expected: every probe prints `PASS`; the command exits 0.

- [ ] **Step 9: Commit**

```bash
git add examples/nullable_iflet_probe.goo examples/nullable_iflet_probe.expected.txt Makefile .github/workflows/tests.yml src/
git commit -m "feat(nullable): wire ?int construct + if-let end-to-end (nullable-iflet-probe)"
```

---

## Task 2: `nil` comparison (`x == nil` / `x != nil`)

**Files:**
- Create: `examples/nullable_nilcmp_probe.goo`
- Create: `examples/nullable_nilcmp_probe.expected.txt`
- Modify: `Makefile` (add `nullable-nilcmp-probe:` target; add to `verify:`)
- Modify: `.github/workflows/tests.yml:54`
- Likely fix (spike-confirmed): `src/codegen/expression_codegen.c` (binary `==`/`!=` where one operand is nullable and the other is `nil`), `src/types/expression_checker.c` (typing `nullable == nil` → `bool`)

**Interfaces:**
- Consumes: Task 1's `?int` construction (declare/assign/nil).
- Produces: `x == nil` / `x != nil` evaluating to `bool` by reading the `is_null` field via `codegen_nullable_is_null` (`nullable_codegen.c:50`). Task 3 reuses this for ABI observation.

- [ ] **Step 1: Write the failing test (probe + expected)**

Create `examples/nullable_nilcmp_probe.goo`:

```go
// nullable_nilcmp_probe: ?int compared against nil yields bool by reading the
// is_null flag — in an if-condition and assigned to a bool, both operands.
package main

import "fmt"

func main() {
    var a ?int = 7
    if a != nil {
        fmt.Println("PASS: value != nil")
    }
    if a == nil {
        fmt.Println("FAIL: value == nil")
    }

    var b ?int = nil
    if b == nil {
        fmt.Println("PASS: nil == nil")
    }

    present := a != nil
    missing := b == nil
    if present && missing {
        fmt.Println("PASS: nil-compare as bool values")
    }
}
```

Create `examples/nullable_nilcmp_probe.expected.txt`:

```
PASS: value != nil
PASS: nil == nil
PASS: nil-compare as bool values
```

- [ ] **Step 2: Build and run to confirm failure**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/nullable_nilcmp_probe examples/nullable_nilcmp_probe.goo && ./build/nullable_nilcmp_probe
```

Expected: failure — most likely a codegen/typecheck error because comparing a `{i1, T}` struct to `nil` has no handler yet, or an LLVM type-mismatch in `verifyModule`. Capture the exact error.

- [ ] **Step 3: Discovery spike — diagnose**

Determine where `==`/`!=` against `nil` is handled for a nullable operand:
- **Typecheck:** does `expression_checker.c` accept `nullable == nil` and assign it `bool`? If it rejects or mis-types, fix here.
- **Codegen:** in `expression_codegen.c`'s binary-expr path, detect "one side is a nullable value, other side is the `nil` literal" and lower to `is_null` (for `== nil`) or `not is_null` (for `!= nil`) via `codegen_nullable_is_null` (`nullable_codegen.c:50`). Confirm operand order is handled both ways (`a == nil` and `nil == a`).

Record findings in the PR description.

- [ ] **Step 4: Implement the minimal fix**

Add the nullable-vs-`nil` case to the binary `==`/`!=` codegen (and typing if needed): if either operand is a nullable value and the other is the `nil` literal, result = `is_null` for `==`, `LLVMBuildNot(is_null)` for `!=`; result type `bool` (i1). Do not handle nullable-vs-nullable equality (out of scope). `make clean && make goo` if a header changed.

- [ ] **Step 5: Run to verify pass**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/nullable_nilcmp_probe examples/nullable_nilcmp_probe.goo
./build/nullable_nilcmp_probe | diff -u examples/nullable_nilcmp_probe.expected.txt -
```

Expected: no diff, exit 0.

- [ ] **Step 6: Add the Makefile probe target**

```make
nullable-nilcmp-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== nullable-nilcmp-probe: ?int == nil / != nil ==="
	$(COMPILER) -o build/nullable_nilcmp_probe examples/nullable_nilcmp_probe.goo
	@./build/nullable_nilcmp_probe > build/nullable_nilcmp_probe.actual.txt
	@if diff -u examples/nullable_nilcmp_probe.expected.txt build/nullable_nilcmp_probe.actual.txt; then \
	  echo "nullable-nilcmp-probe: PASS"; \
	else \
	  echo "nullable-nilcmp-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
```

Append `nullable-nilcmp-probe` to `verify:`.

- [ ] **Step 7: Wire into CI**

Append `nullable-nilcmp-probe` to the probe list on `.github/workflows/tests.yml:54`.

- [ ] **Step 8: Verify the whole gate stays green**

```bash
make CC=gcc-14 LLVM_CONFIG="${LLVMCFG:-llvm-config}" \
  baseline-probe lvalue-probe file-io-probe pointer-probe pointer-write-probe \
  switch-probe methods-probe new-probe enum-probe match-probe append-probe \
  cap-probe map-probe int64-probe commaok-probe guard-probe \
  nullable-iflet-probe nullable-nilcmp-probe
```

Expected: all `PASS`, exit 0.

- [ ] **Step 9: Commit**

```bash
git add examples/nullable_nilcmp_probe.goo examples/nullable_nilcmp_probe.expected.txt Makefile .github/workflows/tests.yml src/
git commit -m "feat(nullable): lower ?T == nil / != nil to is_null read (nullable-nilcmp-probe)"
```

---

## Task 3: Large-struct nullable ABI (`?BigStruct` across functions)

**Files:**
- Create: `examples/nullable_abi_probe.goo`
- Create: `examples/nullable_abi_probe.expected.txt`
- Modify: `Makefile` (add `nullable-abi-probe:` target; add to `verify:`)
- Modify: `.github/workflows/tests.yml:54`
- Likely fix (spike-confirmed): `src/codegen/` call/return ABI path (by-pointer for `{i1, BigStruct}` > 16 bytes), mirroring the slice/`m12_probe` treatment

**Interfaces:**
- Consumes: Task 1 (`if let` observe) and Task 2 (`== nil`).
- Produces: nullable values of base types whose `{i1, T}` exceeds 16 bytes correctly returned from and passed to functions. No new public signatures; reuses the existing struct-by-pointer ABI path.

- [ ] **Step 1: Write the failing test (probe + expected)**

Create `examples/nullable_abi_probe.goo` (`Point` has three `int64` fields = 24 bytes, so `{i1, Point}` exceeds 16 bytes and must travel by pointer):

```go
// nullable_abi_probe: a nullable of a >16-byte struct must cross function-call
// boundaries by pointer, not by value (slice/m12 ABI precedent). Exercises
// return ?Point and pass ?Point, observed via if-let and == nil.
package main

import "fmt"

type Point struct {
    X int64
    Y int64
    Z int64
}

func makePoint(present bool) ?Point {
    if present {
        return Point{X: 1, Y: 2, Z: 3}
    }
    return nil
}

func sumOrZero(p ?Point) int64 {
    if let q = p {
        return q.X + q.Y + q.Z
    }
    return 0
}

func main() {
    a := makePoint(true)
    if a != nil {
        fmt.Println("PASS: ?Point value != nil")
    }
    fmt.Println(sumOrZero(a))

    b := makePoint(false)
    if b == nil {
        fmt.Println("PASS: ?Point nil == nil")
    }
    fmt.Println(sumOrZero(b))
}
```

Create `examples/nullable_abi_probe.expected.txt`:

```
PASS: ?Point value != nil
6
PASS: ?Point nil == nil
0
```

- [ ] **Step 2: Build and run to confirm failure**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/nullable_abi_probe examples/nullable_abi_probe.goo && ./build/nullable_abi_probe
```

Expected: failure — most likely a wrong sum (garbage from by-value struct truncation across the call boundary), an LLVM `verifyModule` ABI/type mismatch, or a crash. This is the highest-risk corner. Capture the exact symptom.

- [ ] **Step 3: Discovery spike — diagnose the ABI path**

Compare against how the codebase already passes large structs by pointer (see `m12-probe` and the slice ABI, memory `goolang-slice-abi-by-pointer`). Check:
- How does the call/return path in codegen decide by-value vs by-pointer? Does it account for the `{i1, T}` nullable wrapper size, or only the inner `T`?
- Does `return Point{...}` from a `?Point` function auto-wrap then return by the correct convention?
- Does `if let q = p` correctly load `q` when `p` arrived by pointer?

Record findings in the PR description.

- [ ] **Step 4: Implement the minimal fix**

Extend the existing struct-by-pointer ABI decision to include nullable wrapper structs whose total size exceeds 16 bytes, reusing the established by-pointer path (do not invent a new convention). Ensure return-auto-wrap and parameter-load both honor it. `make clean && make goo` if a header changed.

- [ ] **Step 5: Run to verify pass**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/nullable_abi_probe examples/nullable_abi_probe.goo
./build/nullable_abi_probe | diff -u examples/nullable_abi_probe.expected.txt -
```

Expected: no diff, exit 0.

- [ ] **Step 6: Add the Makefile probe target**

```make
nullable-abi-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== nullable-abi-probe: ?BigStruct (>16B) by-pointer across functions ==="
	$(COMPILER) -o build/nullable_abi_probe examples/nullable_abi_probe.goo
	@./build/nullable_abi_probe > build/nullable_abi_probe.actual.txt
	@if diff -u examples/nullable_abi_probe.expected.txt build/nullable_abi_probe.actual.txt; then \
	  echo "nullable-abi-probe: PASS"; \
	else \
	  echo "nullable-abi-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
```

Append `nullable-abi-probe` to `verify:`.

- [ ] **Step 7: Wire into CI**

Append `nullable-abi-probe` to the probe list on `.github/workflows/tests.yml:54`.

- [ ] **Step 8: Verify the whole gate stays green (full M4 suite)**

```bash
make CC=gcc-14 LLVM_CONFIG="${LLVMCFG:-llvm-config}" \
  baseline-probe lvalue-probe file-io-probe pointer-probe pointer-write-probe \
  switch-probe methods-probe new-probe enum-probe match-probe append-probe \
  cap-probe map-probe int64-probe commaok-probe guard-probe \
  nullable-iflet-probe nullable-nilcmp-probe nullable-abi-probe
```

Expected: all `PASS`, exit 0.

- [ ] **Step 9: Annotate the deferred demo and commit**

Add a header comment to `examples/demos/nullable_types_demo.goo` marking it as an M5 target (it uses the deferred `x!` / `x?` surface and will not compile under M4 — this prevents it being mistaken for a regression):

```go
// M5-TARGET (NOT M4): uses force-unwrap `x!` and presence-test `x?`, which are
// deferred to M5. Does not compile under M4's safe-only nullable core.
```

```bash
git add examples/nullable_abi_probe.goo examples/nullable_abi_probe.expected.txt Makefile .github/workflows/tests.yml examples/demos/nullable_types_demo.goo src/
git commit -m "feat(nullable): ?BigStruct by-pointer ABI across calls (nullable-abi-probe); mark demo M5"
```

---

## Final verification (after all three tasks)

- [ ] All 24 probes (21 existing + 3 new) green via the Step-8 command in Task 3.
- [ ] `make verify` reaches "ALL GREEN GATES PASSED" (ignoring the CompCert `ccomp-build` halt, which is an env gap, not a failure).
- [ ] CI green on each PR — confirmed by reading the status rollup (`gh pr view --json statusCheckRollup` / `gh run view`), never a piped exit code.
- [ ] Spec §9 success criteria all met: three probes pass, wired into CI + `make verify`, no regressions, large-struct ABI exercised, deferred surface documented.

## Spec coverage self-check

| Spec §5 / §6 element | Task |
|---|---|
| declare `?T` (var, field, return, param) | 1 (var/local), 3 (return/param/field via `Point`) |
| assign `T` value (auto-wrap) | 1, 3 |
| assign `nil` | 1, 3 |
| `if let` unwrap (+ `else`) | 1, 3 |
| `x == nil` / `x != nil` | 2, 3 |
| large-struct ABI by-pointer | 3 |
| deferred surface documented (demo → M5) | 3 (Step 9) |
