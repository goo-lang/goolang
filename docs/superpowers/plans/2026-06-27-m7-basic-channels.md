# M7 — Basic Channels End-to-End Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the basic channel value path (`make_chan` / `<-` send / `<-` recv) compile to valid LLVM IR and run in-process over a buffered channel, for any element type, probe-gated.

**Architecture:** *Verify-and-fix*, codegen-only. Task 1 fixes the measured LLVM-context bug so `int` channels round-trip (the verifier failure). Task 2 derives the element type/size from the channel type so non-`int` elements (int64, small struct) round-trip without truncation. The runtime (`channels.c`) and type checker are already correct.

**Tech Stack:** C23, LLVM 22 C API, GNU Make, bash diff-based probe harness.

## Global Constraints

- **Codegen-only.** Fix the channel codegen's use of the wrong LLVM context and its hardcoded element type. No grammar, runtime, or type-system changes. (Spec §2.)
- **Use `codegen->context` everywhere.** Replace global-context builders (`LLVMInt8Type()`, `LLVMInt32Type()`, `LLVMInt64Type()`, `LLVMPointerType(LLVMInt8Type(),0)`) with `…InContext(codegen->context)`. `LLVMFunctionType` derives its context from its component types, so fixing those fixes it. (Spec §1.)
- **Scope:** in-process, buffered, single-goroutine `make_chan(T[, n])` / `ch <- v` / `v := <-ch`. OUT OF SCOPE: `go` spawning, unbuffered blocking, `select` (M8); pattern channels (M9); networked endpoints; Go `make(chan T)` syntax; comma-ok recv. (Spec §2.)
- **No new grammar.**
- **Build facts:** compiler `bin/goo`, runtime `lib/libgoo_runtime.a`. Build: `make goo lib/libgoo_runtime.a`. After editing any `include/*.h`: `make clean && make goo`. `make verify` halts at `ccomp-build` (CompCert gap) — ignore; the real gate is the CI probe list.
- **CI wiring:** each new probe goes in BOTH `verify:` (Makefile) AND `.github/workflows/tests.yml:54`.
- **CI billing caveat:** Actions on `dd0wney/goolang` may be billing-blocked (jobs show red, never start). Authoritative verification is LOCAL: the probe list + `opt --passes=verify` (`bin/goo --emit-llvm <f>.goo` writes textual IR to `<f>.ll`) + `make test`.
- **clang/LSP false positives:** "header not found"/"unknown type" diagnostics are NOT real (build uses `-Iinclude`). Trust `make`.

## Deviation from spec §5 (intentional)

The spec described one `chan-probe` with three cases. This plan splits it into **`chan-probe`** (int round-trip + buffered FIFO — Task 1) and **`chan-elem-probe`** (non-int element — Task 2), so each task has its own independently-rejectable CI gate. Same coverage; cleaner task boundary and finer regression signal (matches the repo's probe granularity).

## Reference map

| Concern | Location | Notes |
|---|---|---|
| send codegen (global-context builders) | `src/codegen/lowlevel_codegen.c` `codegen_generate_channel_send` (~10-66) | uses `LLVMInt8Type()`, `LLVMInt32Type()`, `LLVMInt64Type()`, `LLVMPointerType(LLVMInt8Type(),0)`, `LLVMFunctionType` |
| recv codegen (global ctx + hardcoded i32 element) | `src/codegen/lowlevel_codegen.c` `codegen_generate_channel_recv` (~75-128) | `element_type = LLVMInt32Type()` hardcoded |
| make_chan codegen (global ctx + sizeof(int)) | `src/codegen/call_codegen.c` `codegen_generate_make_chan_call` (~453-520) | `elem_size = sizeof(int)` hardcoded; `result_info->goo_type = NULL` |
| channel type (already correct, in-context) | `src/codegen/type_mapping.c:308` `codegen_get_channel_type` | reference for the right idiom |
| channel element Goo type | `type->data.channel.element_type` | used in `type_check_channel_send_op` (`expression_checker.c`) |
| Goo Type byte size | `Type.size` field (`include/types.h`) | for `elem_size` |
| runtime (no change) | `src/runtime/channels.c` | `goo_make_chan(size_t,size_t)`, `goo_chan_send/recv(goo_channel_t*, void*)` |
| probe pattern to copy | `examples/int64_probe.goo` + `.expected.txt`, Makefile `int64-probe:` | |

---

## Task 1: LLVM-context fix — `int` channels round-trip (`chan-probe`)

**Files:**
- Create: `examples/chan_probe.goo`, `examples/chan_probe.expected.txt`
- Modify: `src/codegen/lowlevel_codegen.c` (send + recv), `src/codegen/call_codegen.c` (make_chan)
- Modify: `Makefile` (add `chan-probe:`; add to `verify:`), `.github/workflows/tests.yml:54`

**Interfaces:**
- Consumes: existing `codegen->context`, `codegen_get_channel_type`, runtime `goo_make_chan`/`goo_chan_send`/`goo_chan_recv`.
- Produces: `int` channel make/send/recv that passes `verifyModule`. No new public signatures.

- [ ] **Step 1: Write the failing test (probe + expected)**

Create `examples/chan_probe.goo`:

```go
// chan_probe: basic in-process buffered channel value path (int) — make_chan,
// send (<-), receive (<-), single goroutine. Reproduces the LLVM-context bug.
package main

import "fmt"

func main() {
    ch := make_chan(int, 1)
    ch <- 42
    x := <-ch
    fmt.Println(x)

    // buffered FIFO order, capacity 2
    q := make_chan(int, 2)
    q <- 1
    q <- 2
    a := <-q
    b := <-q
    fmt.Println(a)
    fmt.Println(b)
}
```

Create `examples/chan_probe.expected.txt`:

```
42
1
2
```

- [ ] **Step 2: Build and run to confirm it fails**

```bash
make goo lib/libgoo_runtime.a
mkdir -p build
bin/goo -o build/chan_probe examples/chan_probe.goo
```

Expected: **FAIL** at LLVM verification with "Function context does not match Module context! @goo_make_chan" and "Call parameter type does not match function signature" on `goo_chan_send`/`goo_chan_recv`. Capture as RED evidence.

- [ ] **Step 3: Fix the context in `make_chan` codegen**

In `src/codegen/call_codegen.c` `codegen_generate_make_chan_call`, replace the global-context builders with in-context ones:

```c
    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef i64 = LLVMInt64TypeInContext(ctx);

    LLVMValueRef elem_size = LLVMConstInt(i64, sizeof(int), 0);   // Task 2 generalizes this
    LLVMValueRef buffer_size = LLVMConstInt(i64, 0, 0);
    if (arg_count == 2) {
        ASTNode* size_arg = call->args->next;
        ValueInfo* size_val = codegen_generate_expression(codegen, checker, size_arg);
        if (!size_val) return NULL;
        buffer_size = size_val->llvm_value;
        if (LLVMTypeOf(buffer_size) != i64) {
            buffer_size = LLVMBuildZExt(codegen->builder, buffer_size, i64, "buffer_size_ext");
        }
        value_info_free(size_val);
    }

    LLVMTypeRef param_types[] = { i64, i64 };
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMTypeRef make_chan_func_type = LLVMFunctionType(void_ptr_type, param_types, 2, 0);
    LLVMValueRef make_chan_func = LLVMGetNamedFunction(codegen->module, "goo_make_chan");
    if (!make_chan_func) make_chan_func = LLVMAddFunction(codegen->module, "goo_make_chan", make_chan_func_type);

    LLVMValueRef args[] = { elem_size, buffer_size };
    LLVMValueRef channel = LLVMBuildCall2(codegen->builder, make_chan_func_type, make_chan_func, args, 2, "new_channel");
```

Also set the result's channel type so downstream recv can find the element type (Task 2 relies on this; harmless now):

```c
    result_info->goo_type = expr->node_type;  // the resolved TYPE_CHANNEL from the type checker
```

- [ ] **Step 4: Fix the context in send + recv codegen**

In `src/codegen/lowlevel_codegen.c` `codegen_generate_channel_send`, build all types in-context:

```c
    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    // temp alloca uses LLVMTypeOf(value) (already correct); bitcast to void_ptr_type above
    LLVMTypeRef param_types[] = { void_ptr_type, void_ptr_type };
    LLVMTypeRef send_func_type = LLVMFunctionType(LLVMInt32TypeInContext(ctx), param_types, 2, 0);
```

and in `codegen_generate_channel_recv`:

```c
    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef element_type = LLVMInt32TypeInContext(ctx);   // Task 2 generalizes this
    LLVMValueRef result_alloca = LLVMBuildAlloca(codegen->builder, element_type, "recv_result");
    LLVMTypeRef void_ptr_type = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMTypeRef param_types_recv[] = { void_ptr_type, void_ptr_type };
    LLVMTypeRef recv_func_type = LLVMFunctionType(LLVMInt32TypeInContext(ctx), param_types_recv, 2, 0);
```

(Keep the rest of each function unchanged.) No header changed → plain `make goo lib/libgoo_runtime.a`.

- [ ] **Step 5: Run to verify it passes**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/chan_probe examples/chan_probe.goo
./build/chan_probe | diff -u examples/chan_probe.expected.txt -
bin/goo --emit-llvm examples/chan_probe.goo && opt --passes=verify -disable-output examples/chan_probe.ll && echo "IR verify CLEAN"
```

Expected: no diff, exit 0, IR verify clean. (Use `opt-22` if `opt` is absent.)

- [ ] **Step 6: Add the Makefile probe target + CI**

Add to `Makefile` (mirror `int64-probe:`):

```make
chan-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== chan-probe: in-process buffered int channel round-trip + FIFO ==="
	$(COMPILER) -o build/chan_probe examples/chan_probe.goo
	@./build/chan_probe > build/chan_probe.actual.txt
	@if diff -u examples/chan_probe.expected.txt build/chan_probe.actual.txt; then \
	  echo "chan-probe: PASS"; \
	else \
	  echo "chan-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
```

Append `chan-probe` to `verify:` and to the probe list on `.github/workflows/tests.yml:54`.

- [ ] **Step 7: Discovery spike — document endpoint-transport reality (spec §6 deliverable)**

Read `src/runtime/channels.c` `goo_make_chan` / `goo_chan_send` / `goo_chan_recv`. Confirm and record in the PR description whether buffered channels are an in-process ring buffer and whether the `endpoint` string drives any real network transport (expected: in-process only; endpoints stored but no transport). One paragraph; no code change.

- [ ] **Step 8: Verify the whole gate + unit suite**

Run the full CI probe list (read it from `tests.yml:54`) plus `make test`. Expected: every probe `PASS`, exit 0; `make test` no new failures.

- [ ] **Step 9: Commit**

```bash
git add examples/chan_probe.goo examples/chan_probe.expected.txt \
        src/codegen/lowlevel_codegen.c src/codegen/call_codegen.c Makefile .github/workflows/tests.yml
git commit --no-gpg-sign -m "fix(codegen): build channel make/send/recv in codegen context so int channels run (chan-probe)"
```

---

## Task 2: Element-type plumbing — non-`int` channels (`chan-elem-probe`)

**Files:**
- Create: `examples/chan_elem_probe.goo`, `examples/chan_elem_probe.expected.txt`
- Modify: `src/codegen/call_codegen.c` (make_chan elem_size from element type), `src/codegen/lowlevel_codegen.c` (recv element type from channel type)
- Modify: `Makefile`, `.github/workflows/tests.yml:54`

**Interfaces:**
- Consumes: Task 1's in-context channel codegen; `result_info->goo_type = expr->node_type` set in Task 1 so a channel value carries its `TYPE_CHANNEL`.
- Produces: `make_chan(T,n)` sizing the channel to `T`, and `<-ch` allocating/loading the channel's element type. No new public signatures.

- [ ] **Step 1: Write the failing test (probe + expected)**

Create `examples/chan_elem_probe.goo`:

```go
// chan_elem_probe: channels of a non-int element must not truncate. int64 and a
// small struct round-trip through a buffered channel with exact values.
package main

import "fmt"

type Pair struct {
    A int64
    B int64
}

func main() {
    c := make_chan(int64, 1)
    c <- 9000000000        // > 2^32, would truncate if elem size were 4 bytes
    n := <-c
    fmt.Println(n)

    p := make_chan(Pair, 1)
    p <- Pair{A: 1, B: 2}
    q := <-p
    fmt.Println(q.A + q.B)
}
```

Create `examples/chan_elem_probe.expected.txt`:

```
9000000000
3
```

- [ ] **Step 2: Build and run to confirm it fails**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/chan_elem_probe examples/chan_elem_probe.goo && ./build/chan_elem_probe
```

Expected: **FAIL** — the `int64` value prints truncated/garbled (channel sized to `sizeof(int)`=4 and recv loads i32), and/or the `Pair` case mis-typed. Capture the wrong values as RED evidence.

- [ ] **Step 3: Size `make_chan` from the element type**

In `codegen_generate_make_chan_call` (`call_codegen.c`), derive `elem_size` from the channel's element Goo type instead of `sizeof(int)`. The resolved channel type is `expr->node_type` (a `TYPE_CHANNEL`); its element type is `expr->node_type->data.channel.element_type`, and the byte size is the element type's `size` field:

```c
    size_t elem_bytes = sizeof(int);   // fallback
    if (expr->node_type && expr->node_type->kind == TYPE_CHANNEL &&
        expr->node_type->data.channel.element_type) {
        elem_bytes = expr->node_type->data.channel.element_type->size;
    }
    LLVMValueRef elem_size = LLVMConstInt(i64, elem_bytes, 0);
```

(If `Type.size` is 0/unset for some types, fall back to `LLVMABISizeOfType(codegen->target_data, codegen_type_to_llvm(codegen, element_type))` — confirm during the spike which is reliable.)

- [ ] **Step 4: Derive the recv element type from the channel**

In `codegen_generate_channel_recv` (`lowlevel_codegen.c`), replace the hardcoded `element_type` and the result `goo_type` with the channel's element type, read from the operand's channel `goo_type`:

```c
    Type* chan_goo = channel_val->goo_type;
    Type* elem_goo = (chan_goo && chan_goo->kind == TYPE_CHANNEL)
                     ? chan_goo->data.channel.element_type : NULL;
    LLVMTypeRef element_type = elem_goo
        ? codegen_type_to_llvm(codegen, elem_goo)
        : LLVMInt32TypeInContext(codegen->context);   // fallback
    // … alloca/recv/load unchanged, using element_type …
    result_info->goo_type = elem_goo
        ? elem_goo : type_checker_get_builtin(checker, TYPE_INT32);
```

(The send path already stores the actual value via `LLVMTypeOf(value)`, so once the channel `elem_size` matches the element type, send copies the right byte count.) `make clean && make goo` only if a header changed (none expected).

- [ ] **Step 5: Run to verify it passes**

```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/chan_elem_probe examples/chan_elem_probe.goo
./build/chan_elem_probe | diff -u examples/chan_elem_probe.expected.txt -
bin/goo --emit-llvm examples/chan_elem_probe.goo && opt --passes=verify -disable-output examples/chan_elem_probe.ll && echo "IR verify CLEAN"
```

Expected: no diff (`9000000000` and `3`), exit 0, IR verify clean.

- [ ] **Step 6: Add the Makefile probe target + CI**

```make
chan-elem-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== chan-elem-probe: non-int channel elements (int64, struct) no truncation ==="
	$(COMPILER) -o build/chan_elem_probe examples/chan_elem_probe.goo
	@./build/chan_elem_probe > build/chan_elem_probe.actual.txt
	@if diff -u examples/chan_elem_probe.expected.txt build/chan_elem_probe.actual.txt; then \
	  echo "chan-elem-probe: PASS"; \
	else \
	  echo "chan-elem-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
```

Append `chan-elem-probe` to `verify:` and to `.github/workflows/tests.yml:54`.

- [ ] **Step 7: Verify the whole gate + unit suite**

Run the full CI probe list (now incl. `chan-probe` + `chan-elem-probe`) plus `make test`. Expected: all `PASS`, exit 0; no new unit failures.

- [ ] **Step 8: Commit**

```bash
git add examples/chan_elem_probe.goo examples/chan_elem_probe.expected.txt \
        src/codegen/call_codegen.c src/codegen/lowlevel_codegen.c Makefile .github/workflows/tests.yml
git commit --no-gpg-sign -m "fix(codegen): size channels by element type so non-int channels don't truncate (chan-elem-probe)"
```

---

## Final verification (after both tasks)

- [ ] `chan-probe` and `chan-elem-probe` green; exact values (`42`/`1`/`2`; `9000000000`/`3`).
- [ ] `opt --passes=verify` clean on both probes' IR.
- [ ] Full CI probe gate + `make test` green — no regressions.
- [ ] Endpoint-transport reality documented (Task 1 Step 7) — the NPU-thesis ground truth.
- [ ] Spec §9 success criteria met.

## Spec coverage self-check

| Spec §5/§9 element | Task |
|---|---|
| context fix → int round-trip | 1 |
| buffered FIFO order | 1 |
| non-int element (int64 + struct) | 2 |
| `opt verify` clean | 1, 2 |
| probes wired into verify: + tests.yml | 1, 2 |
| endpoint-transport documented (§6) | 1 (Step 7) |
| no regressions / local verification | 1, 2 |
