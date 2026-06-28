# M8 — Goroutine Spawning (`go f(ch)`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `go f(ch)` actually spawn a goroutine that runs and communicates back over a buffered channel, probe-gated and deterministic.

**Architecture:** *Verify-and-fix*, codegen + one typecheck rejection. `codegen_generate_go_stmt` currently (a) builds LLVM types in the global context (the M7 bug class → `verifyModule` failure) and (b) hardcodes `NULL` as the goroutine argument. The fix: build in `codegen->context`, and pass the call's single pointer-sized argument (a channel, which lowers to `i8*` = the `void* arg` of `goo_go`) straight through — no marshaling. A typecheck guard rejects out-of-scope `go` forms. The threading runtime (`goo_go`, scheduler) and grammar already work.

**Tech Stack:** C23, LLVM 22 C API, pthread runtime, GNU Make, bash diff-based probe harness.

## Global Constraints

- **Single pointer-sized argument.** `go f(ch)` passes the call's one argument as `goo_go`'s `void* arg`. A goroutine fn taking one pointer param (`func produce(ch chan int)`) lowers to `void(i8*)` ≡ `goo_goroutine_func_t`; the channel (an `i8*`) is passed directly, NO marshaling struct. (Spec §2/§4.)
- **Reject out-of-scope `go` forms at compile time** (mirrors M7's capacity-0 rejection): `go f(a, b, …)` (>1 arg) and `go f(x)` where `x` is not pointer-sized. `go f()` (niladic, NULL arg) is allowed. (Spec §2/§5.)
- **Codegen-only fix + one typecheck rejection.** No runtime or grammar changes. (Spec §2/§3.)
- **Use `codegen->context`** for all LLVM type/constant builders in the go-stmt codegen (the M7 context-bug fix). (Spec §3.)
- **No new grammar.**
- **Build facts:** compiler `bin/goo`, runtime `lib/libgoo_runtime.a`. Build: `make goo lib/libgoo_runtime.a`. After editing any `include/*.h`: `make clean && make goo`. `make verify` halts at `ccomp-build` (CompCert gap) — ignore; the real gate is the CI probe list.
- **Run concurrency probes under a timeout** (`timeout 10 ./build/<probe>`) so a hang fails loudly instead of blocking the gate.
- **CI wiring:** the new probe goes in BOTH `verify:` (Makefile) AND `.github/workflows/tests.yml:54`.
- **CI billing caveat:** Actions on `dd0wney/goolang` may be billing-blocked (jobs show red, never start). Authoritative verification is LOCAL: the probe list + `opt --passes=verify` + `make test`.
- **clang/LSP false positives:** "header not found"/"unknown type" diagnostics are NOT real (build uses `-Iinclude`). Trust `make`.

## Reference map

| Concern | Location | Notes |
|---|---|---|
| go-stmt codegen (global-ctx bug + NULL arg) | `src/codegen/statement_codegen.c:691` `codegen_generate_go_stmt` | native path ~line 740-768 builds `LLVMPointerType(LLVMInt8Type(),0)`, `LLVMVoidType()`, `LLVMFunctionType` in global ctx; passes `null_arg` |
| go-stmt typecheck (add rejection here) | `src/types/type_checker.c:967` `type_check_go_stmt` | currently accepts any call; add arg-count/arg-kind guard |
| arg iteration pattern | `src/codegen/call_codegen.c:76-131` (len/cap/append) | `call->args`, `call->args->next` |
| runtime spawn (no change) | `src/runtime/concurrency.c` `goo_go` | `goo_go(void(*)(void*), void*)`; auto-inits scheduler |
| channel = i8* (the convention) | `src/codegen/type_mapping.c:308` `codegen_get_channel_type` | `LLVMPointerType(LLVMInt8TypeInContext(ctx),0)` |
| `type_is_pointer`-ish helpers | `include/types.h`, `src/types/types.c` | for the "pointer-sized arg" check (channel/pointer kinds) |
| probe pattern to copy | `examples/chan_probe.goo` + Makefile `chan-probe:` | M7 channel probe |
| context-fix precedent | M7 commit `2817655` (channels) | same `…InContext(ctx)` shape |

---

## Task 1: `go f(ch)` spawns a goroutine + reject out-of-scope forms (`go-probe`)

**Files:**
- Create: `examples/go_probe.goo`, `examples/go_probe.expected.txt`
- Modify: `src/codegen/statement_codegen.c` (`codegen_generate_go_stmt`)
- Modify: `src/types/type_checker.c` (`type_check_go_stmt`)
- Modify: `Makefile` (add `go-probe:`; add to `verify:`), `.github/workflows/tests.yml:54`

**Interfaces:**
- Consumes: M7 channels (`make_chan`, `<-`), runtime `goo_go`, `codegen_get_channel_type`, `codegen->context`.
- Produces: working `go f(ch)` spawning. No new public signatures.

- [x] **Step 1: Write the failing test (probe + expected)**

Create `examples/go_probe.goo`:

```go
// go_probe: `go f(ch)` spawns a goroutine that runs and delivers a value over a
// buffered channel; main receives it (the receive is the deterministic join).
package main

import "fmt"

func produce(ch chan int) {
    ch <- 7
}

func produce9(ch chan int) {
    ch <- 9
}

func main() {
    c := make_chan(int, 1)
    go produce(c)
    x := <-c
    fmt.Println(x)

    d := make_chan(int, 1)
    go produce9(d)
    fmt.Println(<-d)
}
```

Create `examples/go_probe.expected.txt`:

```
7
9
```

- [x] **Step 2: Build and run to confirm it fails**

```bash
make goo lib/libgoo_runtime.a
mkdir -p build
bin/goo -o build/go_probe examples/go_probe.goo
```

Expected: **FAIL** — LLVM `verifyModule` error ("Function context does not match Module context" / call-param mismatch on `goo_go`, the global-context bug), or — if it compiles — a hang/garbage because the goroutine got `NULL` instead of the channel. Capture as RED. (If it produces a binary, run `timeout 10 ./build/go_probe` and capture the wrong/garbled output or hang.)

- [x] **Step 3: Discovery spike — scheduler lifecycle + confirm mechanism**

Confirm (a) the failure is the global-context bug and/or the dropped `NULL` arg in `codegen_generate_go_stmt` (native path, ~lines 740-768); (b) read `src/runtime/concurrency.c` `goo_go`/`goo_scheduler_init` and determine whether the program **exits cleanly (rc 0)** once `main` returns with a live scheduler thread, or whether it hangs at teardown. Record the finding in the PR description. If a teardown hang is found, the minimal fix (detached threads, or shutdown on main-return) is in scope; note it. No code change in this step.

- [x] **Step 4: Fix the go-stmt codegen (context + argument)**

In `src/codegen/statement_codegen.c` `codegen_generate_go_stmt` (native path), replace the global-context builders and the `NULL` argument. Use `codegen->context`, and pass the call's single argument (bitcast to `i8*`) as `goo_go`'s `arg`:

```c
    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);

    // Goroutine function value (e.g. `produce`) — a void(i8*) function pointer.
    ValueInfo* func_val = codegen_generate_expression(codegen, checker, call->function);
    if (!func_val) return 0;

    // Argument: pass the single call argument (a channel = i8*) straight through
    // as goo_go's void* arg. Niladic `go f()` passes NULL (current behavior).
    LLVMValueRef arg_ptr = LLVMConstNull(void_ptr_type);
    if (call->args) {
        ValueInfo* a = codegen_generate_expression(codegen, checker, call->args);
        if (!a) { value_info_free(func_val); return 0; }
        arg_ptr = a->llvm_value;
        if (LLVMTypeOf(arg_ptr) != void_ptr_type) {
            arg_ptr = LLVMBuildBitCast(codegen->builder, arg_ptr, void_ptr_type, "go_arg");
        }
        value_info_free(a);
    }

    // goo_go(func, arg) — declare in-context.
    LLVMTypeRef func_ptr_type = LLVMPointerType(
        LLVMFunctionType(LLVMVoidTypeInContext(ctx), &void_ptr_type, 1, 0), 0);
    LLVMTypeRef param_types[] = { func_ptr_type, void_ptr_type };
    LLVMTypeRef goo_go_type = LLVMFunctionType(void_ptr_type, param_types, 2, 0);
    LLVMValueRef goo_go_func = LLVMGetNamedFunction(codegen->module, "goo_go");
    if (!goo_go_func) goo_go_func = LLVMAddFunction(codegen->module, "goo_go", goo_go_type);

    LLVMValueRef func_as_ptr = func_val->llvm_value;
    if (LLVMTypeOf(func_as_ptr) != func_ptr_type) {
        func_as_ptr = LLVMBuildBitCast(codegen->builder, func_as_ptr, func_ptr_type, "go_func");
    }
    LLVMValueRef go_args[] = { func_as_ptr, arg_ptr };
    LLVMBuildCall2(codegen->builder, goo_go_type, goo_go_func, go_args, 2, "");
    value_info_free(func_val);
    return 1;
```

(Leave the WASM branch above it unchanged. Confirm during the spike whether the existing code already binds `func_val`/`call` exactly so — adapt variable names to the actual code, keeping the in-context types and the real-argument pass-through.)

- [x] **Step 5: Add the typecheck rejection for out-of-scope forms**

In `src/types/type_checker.c` `type_check_go_stmt` (line 967), after type-checking the call, reject multi-arg and non-pointer-arg forms. Count `call->args`; if more than one, or if the single arg's type is not pointer-sized (a channel or pointer), error:

```c
    if (go_stmt->call && go_stmt->call->type == AST_CALL_EXPR) {
        CallExprNode* call = (CallExprNode*)go_stmt->call;
        size_t argc = 0;
        for (ASTNode* a = call->args; a; a = a->next) argc++;
        if (argc > 1) {
            type_error(checker, stmt->pos,
                "goroutines with more than one argument are not supported yet; "
                "pass a single pointer-sized value such as a channel (multi-arg goroutines are deferred)");
            return 0;
        }
        if (argc == 1) {
            Type* at = type_check_expression(checker, call->args);
            if (at && at->kind != TYPE_CHANNEL && at->kind != TYPE_POINTER) {
                type_error(checker, stmt->pos,
                    "goroutine argument must be a single pointer-sized value (a channel or pointer); "
                    "passing values by goroutine argument is deferred");
                return 0;
            }
        }
    }
```

(Use the actual `TYPE_CHANNEL`/`TYPE_POINTER` enum names from `include/types.h`; if a `type_is_pointer`-style helper exists, prefer it. Adapt to the real `CallExprNode`/`type_error` signatures.) If a header changed, `make clean && make goo`; otherwise `make goo lib/libgoo_runtime.a`.

- [x] **Step 6: Run to verify the probe passes**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/go_probe examples/go_probe.goo
timeout 10 ./build/go_probe | diff -u examples/go_probe.expected.txt -
echo "exit: $?"
bin/goo --emit-llvm examples/go_probe.goo && opt --passes=verify -disable-output examples/go_probe.ll && echo "IR verify CLEAN"
```

Expected: no diff, exit 0 (not 124/timeout), IR verify clean. (Use `opt-22` if `opt` is absent.)

- [x] **Step 7: Verify the rejections (manual, no probe — negative cases)**

```bash
printf 'package main\nfunc w(a chan int, b chan int){}\nfunc main(){ c := make_chan(int,1)\n go w(c, c) }\n' > /tmp/go_multi.goo
bin/goo -o /tmp/go_multi /tmp/go_multi.goo 2>&1 | head -2   # expect the multi-arg compile error
printf 'package main\nfunc w(n int){}\nfunc main(){ go w(5) }\n' > /tmp/go_val.goo
bin/goo -o /tmp/go_val /tmp/go_val.goo 2>&1 | head -2       # expect the non-pointer-arg compile error
```

Expected: both produce the M8-scope compile errors (capture for the report). No probe is added for negative cases (the diff harness can't assert a compile failure).

- [x] **Step 8: Add the Makefile probe target (under timeout) + CI**

Add to `Makefile`:

```make
go-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== go-probe: go f(ch) spawns a goroutine; main joins via buffered channel ==="
	$(COMPILER) -o build/go_probe examples/go_probe.goo
	@timeout 10 ./build/go_probe > build/go_probe.actual.txt; rc=$$?; \
	if [ $$rc -ne 0 ]; then echo "go-probe: FAIL (exit $$rc — hang or crash)"; exit 1; fi
	@if diff -u examples/go_probe.expected.txt build/go_probe.actual.txt; then \
	  echo "go-probe: PASS"; \
	else \
	  echo "go-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
```

Append `go-probe` to the `verify:` dependency list and to the probe list on `.github/workflows/tests.yml:54`.

- [x] **Step 9: Verify the whole gate + unit suite**

Run the full CI probe list (read from `tests.yml:54`, now incl. `go-probe`) plus `make test`. Expected: every probe `PASS`, exit 0; `make test` no new failures. (The channel probes — `chan-probe` etc. — are the key regression guard, since `go` builds on them.)

- [x] **Step 10: Commit**

```bash
git add examples/go_probe.goo examples/go_probe.expected.txt \
        src/codegen/statement_codegen.c src/types/type_checker.c \
        Makefile .github/workflows/tests.yml
git commit --no-gpg-sign -m "feat(concurrency): go f(ch) spawns a goroutine with its channel arg (go-probe); reject out-of-scope go forms"
```

---

## Final verification

- [x] `go-probe` green under timeout (exit 0, output `7`/`9`); `opt --passes=verify` clean.
- [x] Multi-arg and non-pointer-arg `go` forms rejected at compile time (Step 7 evidence).
- [x] Full CI probe gate + `make test` green — no regressions (esp. channel probes). `make test`: 76 passed, 1 skipped, 0 failed.
- [x] Clean program exit with a live scheduler confirmed (Step 3 spike): `go-probe` exits 0 under `timeout 10` with a live scheduler thread.
- [x] Spec §10 success criteria met.

**Status: COMPLETE.** Shipped in [PR #28](https://github.com/dd0wney/goolang/pull/28). Local gate fully green (authoritative); GitHub Actions red is the known dd0wney billing block (jobs never start — "recent account payments have failed"), not a real failure.

## Spec coverage self-check

| Spec §5/§10 element | Step |
|---|---|
| `go f(ch)` spawns + passes channel arg | 4, 6 |
| context-bug fix (in-context types) | 4 |
| reject multi-arg / non-pointer-arg | 5, 7 |
| deterministic probe via buffered-channel join | 1, 6 |
| scheduler clean-exit confirmed | 3 |
| probe under timeout | 6, 8 |
| wired into verify: + tests.yml | 8 |
| no regressions / local verification | 9 |
