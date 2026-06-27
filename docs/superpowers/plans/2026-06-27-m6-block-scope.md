# M6 — Block-Scoped Value-Table Teardown Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix a pre-existing lexical-shadowing miscompile so inner-block variable bindings stop leaking past their scope, gated by a new CI probe, with zero regressions.

**Architecture:** Single-point codegen fix — `codegen_generate_block_stmt` snapshots `value_table_size` on entry and restores it on exit, mirroring the existing match-arm teardown. This is *verify-and-fix* / TDD: write a probe that reproduces the leak (RED), apply the one-function fix (GREEN), confirm no regression. Codegen-only; no grammar, runtime, or type-system changes.

**Tech Stack:** C23, LLVM 22 C API, GNU Make, bash diff-based probe harness, GitHub Actions.

## Global Constraints

- **Codegen-only.** The type checker already scope_push/pops around constructs (`src/types/type_checker.c`); this fix makes codegen's flat value table agree. The spike confirms the checker scopes plain blocks before relying on it. (Spec §3 decision 1.)
- **Single choke point.** Apply the snapshot/restore in `codegen_generate_block_stmt` (`src/codegen/statement_codegen.c:123`) only — if/for/while/catch/if-let bodies are all `AST_BLOCK_STMT` routed through it. Do NOT add per-construct teardown. (Spec §3 decision 2.)
- **Correctness only — do NOT free truncated `ValueInfo*`.** Mirror the match-arm pattern (`composite_codegen.c:538/650`): reset `value_table_size` without freeing. The leak is a separate documented follow-up. (Spec §3 decision 3.)
- **Restore on EVERY exit path.** The function has a normal end (`return 1`), an early `break` (when the current block already has a terminator), and an error `return 0`. The restore must run for the normal + break paths; the error path aborts compilation so its table state is moot. (Spec §2.)
- **No new grammar.** (Spec §3 decision 4.)
- **Build facts:** compiler `bin/goo`, runtime `lib/libgoo_runtime.a`. Build: `make goo lib/libgoo_runtime.a`. After editing any `include/*.h`: `make clean && make goo`. `make verify` halts at `ccomp-build` (CompCert env gap) — ignore; the real gate is the CI probe list.
- **CI wiring:** the new probe goes in BOTH the `verify:` target (`Makefile`) AND the probe list in `.github/workflows/tests.yml:54`.
- **CI billing caveat:** Actions on `dd0wney/goolang` may be billing-blocked (jobs show red, never start). Authoritative verification is LOCAL: `make test` + the probe list + `opt --passes=verify` (`bin/goo --emit-llvm <f>.goo` writes textual IR to `<f>.ll`).
- **clang/LSP false positives:** "header not found"/"unknown type" diagnostics are NOT real (build uses `-Iinclude`). Trust `make`.

## Reference map

| Concern | Location | Notes |
|---|---|---|
| Block lowering (the fix site) | `src/codegen/statement_codegen.c:123` `codegen_generate_block_stmt` | iterates statements; has an early `break` when the insert block already has a terminator |
| Teardown pattern to mirror | `src/codegen/composite_codegen.c:538,650` | `size_t pre = codegen->value_table_size; … codegen->value_table_size = pre;` |
| Value-table fields | `include/codegen.h:46,53` | `value_table_size`, `value_table_function_start` |
| LIFO lookup (why leak shows) | `src/codegen/codegen.c:321` | reverse scan returns most-recent binding |
| Probe pattern to copy | `examples/int64_probe.goo` + `.expected.txt`, Makefile `int64-probe:` | |
| Regression guards in the full gate | `nullable-iflet-probe`, `match-probe`, `guard-probe` | shared block path; must stay green |

---

## Task 1: Block-scoped value-table teardown + `block-scope-probe`

**Files:**
- Create: `examples/block_scope_probe.goo`, `examples/block_scope_probe.expected.txt`
- Modify: `src/codegen/statement_codegen.c` (`codegen_generate_block_stmt`, ~line 123)
- Modify: `Makefile` (add `block-scope-probe:`; add to `verify:`), `.github/workflows/tests.yml:54`

**Interfaces:**
- Consumes: nothing new — uses existing `codegen->value_table_size` (`include/codegen.h:46`).
- Produces: correct block scoping for all block-bodied constructs. No new public C signatures.

- [ ] **Step 1: Write the failing test (probe + expected)**

Create `examples/block_scope_probe.goo`:

```go
// block_scope_probe: inner-block variable redeclarations must not leak past
// their scope (Go semantics). Reproduces the pre-existing flat-value-table
// miscompile that M5's LIFO lookup surfaced. Includes nested blocks, a loop
// body, and an if-let regression guard.
package main

import "fmt"

func main() {
    x := 1
    if x == 1 {
        x := 2
        fmt.Println(x)
    }
    fmt.Println(x)

    y := 10
    if y == 10 {
        y := 20
        if y == 20 {
            y := 30
            fmt.Println(y)
        }
        fmt.Println(y)
    }
    fmt.Println(y)

    for i := 0; i < 2; i = i + 1 {
        z := i * 5
        fmt.Println(z)
    }

    var n ?int = 5
    if let v = n {
        fmt.Println(v)
    }
}
```

Create `examples/block_scope_probe.expected.txt`:

```
2
1
30
20
10
0
5
5
```

(Before the fix, the post-block prints leak: the second line shows `2` instead of `1`, and the `y` lines show `30`/`30` instead of `20`/`10`. The if-let line `5` is the regression guard. Match regression is covered separately by `match-probe`/`guard-probe` in the full gate, Step 7.)

- [ ] **Step 2: Build and run to confirm it fails**

```bash
make goo lib/libgoo_runtime.a
mkdir -p build
bin/goo -o build/block_scope_probe examples/block_scope_probe.goo && ./build/block_scope_probe
```

Expected: a **failure** — the output diverges from expected because inner redeclarations leak: line 2 prints `2` (not `1`), and the `y` block lines print `30` then `30` then `30` (not `20`/`10`). Capture the actual output as RED evidence.

- [ ] **Step 3: Discovery spike — confirm the mechanism and checker alignment**

- Confirm the leak is in `codegen_generate_block_stmt` (`statement_codegen.c:123`): it iterates statements without snapshotting/restoring `value_table_size`, so an inner `x := 2` appends a new binding that LIFO (`codegen.c:321`) keeps returning after the block ends.
- Confirm the **type checker** scopes plain blocks (so the source is well-typed and the checker already treats the inner `x` as a distinct binding and the outer `x` as restored after the block). Grep `src/types/type_checker.c` for `scope_push`/`scope_pop` around the block/if/for paths. If the checker does NOT scope plain blocks, STOP and report — the fix may need a checker change too (the spec assumes codegen-only).

Record findings in the PR description. No code change in this step.

- [ ] **Step 4: Apply the fix**

In `src/codegen/statement_codegen.c`, modify `codegen_generate_block_stmt` to snapshot `value_table_size` before iterating and restore it after the loop (covers the normal-end and early-`break` paths; the error `return 0` aborts compilation so its table state is moot):

```c
int codegen_generate_block_stmt(CodeGenerator* codegen, TypeChecker* checker, ASTNode* stmt) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, stmt->pos, "LLVM support not available");
    return 0;
#else
    if (!codegen || !checker || !stmt || stmt->type != AST_BLOCK_STMT) return 0;

    BlockStmtNode* block = (BlockStmtNode*)stmt;

    // Block scope: bindings declared inside this block must not outlive it.
    // Snapshot the value-table high-water mark and truncate back to it on the
    // way out, so an inner `x := ...` cannot leak past the block (Go scoping).
    // Mirrors the match-arm teardown in composite_codegen.c. We reset the size
    // without freeing the truncated ValueInfo* (matching existing behavior;
    // the leak is a separate follow-up).
    size_t pre_block_vt_size = codegen->value_table_size;

    ASTNode* current = block->statements;
    while (current) {
        // Skip emission once the current block already has a terminator (e.g.
        // an if-let whose branches both return left an `unreachable` exit_bb);
        // appending later statements would put a terminator mid-block.
        if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)))
            break;
        if (!codegen_generate_statement(codegen, checker, current)) {
            return 0;
        }
        current = current->next;
    }

    // Restore on the normal-end and early-break paths.
    codegen->value_table_size = pre_block_vt_size;
    return 1;
#endif
}
```

No header changed, so a plain `make goo lib/libgoo_runtime.a` suffices.

- [ ] **Step 5: Run to verify it passes**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/block_scope_probe examples/block_scope_probe.goo
./build/block_scope_probe | diff -u examples/block_scope_probe.expected.txt -
bin/goo --emit-llvm examples/block_scope_probe.goo && opt --passes=verify -disable-output examples/block_scope_probe.ll && echo "IR verify CLEAN"
```

Expected: no diff, exit 0, IR verify clean. (Use `opt-22` if `opt` is unavailable.)

- [ ] **Step 6: Add the Makefile probe target**

Add to `Makefile` (mirror `int64-probe:`):

```make
block-scope-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== block-scope-probe: inner-block redeclarations do not leak ==="
	$(COMPILER) -o build/block_scope_probe examples/block_scope_probe.goo
	@./build/block_scope_probe > build/block_scope_probe.actual.txt
	@if diff -u examples/block_scope_probe.expected.txt build/block_scope_probe.actual.txt; then \
	  echo "block-scope-probe: PASS"; \
	else \
	  echo "block-scope-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
```

Append `block-scope-probe` to the `verify:` dependency list.

- [ ] **Step 7: Wire into CI and verify the whole gate stays green**

Append `block-scope-probe` to the probe list on `.github/workflows/tests.yml:54`, then run the full gate (the if-let/match/catch probes are the key regression guards for the shared block path):

```bash
make CC=gcc-14 LLVM_CONFIG="${LLVMCFG:-llvm-config}" \
  baseline-probe lvalue-probe file-io-probe pointer-probe pointer-write-probe \
  switch-probe methods-probe new-probe enum-probe match-probe append-probe \
  cap-probe map-probe int64-probe commaok-probe guard-probe \
  nullable-iflet-probe nullable-nilcmp-probe nullable-abi-probe \
  nullable-intret-probe nullable-assign-probe \
  erru-catch-probe erru-error-probe erru-abi-probe block-scope-probe
```

Expected: every probe prints `PASS`; exit 0 (25 probes total).

- [ ] **Step 8: Run the unit suite (the other half of CI's unit-tests job)**

```bash
make test 2>&1 | tail -5
```

Expected: "All tests passed!" (76 passed + 1 pre-existing skip, or similar — no NEW failures vs main).

- [ ] **Step 9: Commit**

```bash
git add examples/block_scope_probe.goo examples/block_scope_probe.expected.txt \
        src/codegen/statement_codegen.c Makefile .github/workflows/tests.yml
git commit --no-gpg-sign -m "fix(codegen): block-scoped value-table teardown so inner redeclarations don't leak (block-scope-probe)"
```

---

## Final verification

- [ ] `block-scope-probe` green; outer `x`/`y` correctly restored after inner blocks (Go semantics).
- [ ] All 25 probes green via the Step-7 command; `opt --passes=verify` clean on the new probe.
- [ ] `make test` shows no new failures.
- [ ] No regression to `nullable-iflet-probe`, `match-probe`, `guard-probe`, `erru-catch-probe` (shared block path).
- [ ] Spec §8 success criteria met.

## Spec coverage self-check

| Spec element | Step |
|---|---|
| snapshot/restore in `codegen_generate_block_stmt`, all exit paths | 4 |
| codegen-only; checker-alignment confirmed | 3 |
| correctness only (no free) | 4 |
| inner-redeclare-doesn't-leak + nested + loop-body | 1, 5 |
| if-let regression guard (in probe) + match regression (full gate) | 1, 7 |
| no-regression to 24 existing probes | 7 |
| probe wired into verify: + tests.yml | 6, 7 |
| local verification (CI billing-blocked) | 5, 7, 8 |
