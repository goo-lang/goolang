# Lvalue Goroutine Args & Channel Sends Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the last two known lvalue-consumer bugs from PR #91's audit: (1) `go f(p.V)` fails LLVM verification (the goroutine arg boxer boxes the field's ADDRESS); (2) `ch <- t.N` silently sends corrupt data when the field is narrower than the channel element (the send lvalue path bypasses the width-coercion machinery and memcpys elem_size bytes from the field address — reproduced: int32 field 5 into `chan int64` arrives as 25769803781 = 0x600000005, the adjacent field bleeding in).

**Architecture:** Same family as PR #91: a field selector returns the field's ADDRESS (`is_lvalue=1`); every consumer owns the load. Two remaining consumers don't load: the goroutine-arg boxer (Task 1) and the channel-send data-pointer paths — both the plain send helper AND the select-case send copy (Task 2). Fix = the established auto-load idiom (`load codegen_type_to_llvm(goo_type) when is_lvalue`), which for sends also routes lvalues into the EXISTING rvalue coercion+alloca machinery (fixing width corruption AND read-at-rendezvous timing in one move — the value is snapshotted at evaluation, matching Go).

**Tech Stack:** C23, LLVM-C API. No parser, runtime C, or header changes.

**Root-cause evidence (2026-07-02, reproduced on main @051c0e2):** `go show(p.V)` → "Call parameter type does not match function signature! ... call void @show(ptr %ld_arg)"; `ch <- t.N` (int32 into chan int64, adjacent field Pad=6) → prints 25769803781. NOT reproduced / out of scope: `(*pp).V` works correctly (the goo_type=NULL hazard lives only in `codegen_generate_ptr_deref`, which serves the niche `@ptr` DEREF syntax, not `*p` — recorded as a hardening follow-up, no task here).

## Global Constraints

- Branch: `fix/lvalue-send-args` (already created off main @051c0e2 — do NOT commit on main).
- Commits: conventional style, imperative mood, `--no-gpg-sign`. Stage only named files; never stage anything under `.superpowers/`.
- No header edits expected; if any, `make clean` first.
- Gate per task: `make lexer`, run the task's probe, then `eval "$(opam env --switch=default)"` STANDALONE, then `make verify` (all PASS; golden baseline 165/0 grows per probe) and `make test` (76 pass / 1 pre-existing skip).
- Bison conflicts: no grammar changes — count must stay 79 S/R + 256 R/R (nothing should touch it; if it moves, something is wrong).
- KNOWN LATENT BUG: 3+ sequential `fmt.Println` of different-width ints corrupts the middle arg. Task 2's probe converts channel-received `int64`s through `int(...)` so every print is the same width — keep that.
- Concurrency probes must stay deterministic: main-exit is an intentional implicit join (all goroutines finish before exit); single-goroutine prints are safe.

## Reference: verified code landmarks (2026-07-02, main @051c0e2)

- Goroutine arg boxing: `src/codegen/statement_codegen.c:1215-1235` — the arg loop stores `av->llvm_value` and `LLVMTypeOf(av->llvm_value)` raw into `arg_vals`/`arg_types`; for an lvalue that's the field's ADDRESS and `ptr` type, so the box field type and later thunk call both go wrong.
- Plain channel send: `src/codegen/lowlevel_codegen.c:15-70` (`codegen_generate_chan_send` or similar — the function whose body starts "Generate channel and value expressions"). Structure: `value_ptr = value_val->llvm_value; if (!value_val->is_lvalue) { <careful elem-type coercion + alloca> }` — the lvalue case skips the entire block and hands the field address straight to the runtime memcpy.
- Select-case send (separate copy of the same flaw): `src/codegen/statement_codegen.c:1813-1827` — `if (value_val->is_lvalue) data_ptr = value_val->llvm_value; else { alloca of LLVMTypeOf + store }`. NOTE: this copy's rvalue branch also lacks elem-type coercion (allocas `LLVMTypeOf(value)`), a narrower pre-existing gap — Task 2 fixes the LVALUE path at this site with a load (so lvalues at least go through a correct-width load of their own type and land in the rvalue branch); upgrading the select rvalue branch to full elem-type coercion was ORIGINALLY out of scope. RESOLVED DURING EXECUTION: the probe tripped exactly this (loaded i32 into an LLVMTypeOf alloca, 8-byte memcpy over-read -> 214748364850); the controller approved a scope extension and fd59b12 ships the select-send coercion mirroring the plain-send path.
- The auto-load idiom (reference): PR #91's sites, e.g. the unary operand load in `src/codegen/expression_codegen.c` (`unary_load`), or the if-condition `cond_load` in statement_codegen.c.
- Existing probes for syntax reference: `examples/go_probe.goo` (goroutines), `examples/select_probe.goo` (select + `make_chan(int, 1)` buffered channels).
- Widen/coerce reference: the send rvalue path itself (lowlevel_codegen.c:15-70) — SExt/ZExt by `type_is_signed(goo_type)`.

---

### Task 1: Goroutine-arg lvalue auto-load

**Files:**
- Modify: `src/codegen/statement_codegen.c:1223-1234` (the goroutine arg-generation loop)
- Test: `examples/go_lvalue_arg_probe.goo` + `examples/go_lvalue_arg_probe.expected.txt`

**Interfaces:**
- Consumes: PR #91-correct selector reads; `codegen_type_to_llvm`.
- Produces: `go f(p.V)` compiles and passes the field's VALUE, snapshotted at spawn (Go semantics: args evaluated at the `go` statement).

- [ ] **Step 1: Write the failing probe**

`examples/go_lvalue_arg_probe.goo`:
```go
package main

import "fmt"

type T struct {
	V int
}

func show(n int) {
	fmt.Println(n)
}

func main() {
	p := &T{V: 7}
	go show(p.V)
	p.V = 9
}
```

`examples/go_lvalue_arg_probe.expected.txt`:
```
7
```

Coverage: lvalue selector arg through a pointer; the `p.V = 9` after the spawn pins Go's evaluate-at-go-statement semantics — the goroutine must print the snapshot (7), not the mutated value. Deterministic via the main-exit implicit join.

- [ ] **Step 2: Verify it fails today**

Run: `bin/goo examples/go_lvalue_arg_probe.goo 2>&1 | head -4`
Expected: `Module verification failed: Call parameter type does not match function signature!` with a `ptr` passed where an int is declared.

- [ ] **Step 3: Add the auto-load in the arg loop**

In `src/codegen/statement_codegen.c`, inside the arg loop (after `av` is generated and null-checked, before `arg_vals[i] = ...`):

```c
            // Auto-load lvalue args (bare field selectors return the field's
            // ADDRESS) — box the VALUE, snapshotted at the go statement (Go
            // semantics), not a pointer into the enclosing frame. Same idiom
            // as the if/for/unary/receiver load sites.
            if (av->is_lvalue && av->goo_type) {
                LLVMTypeRef at = codegen_type_to_llvm(codegen, av->goo_type);
                if (at) {
                    av->llvm_value = LLVMBuildLoad2(codegen->builder, at,
                                                    av->llvm_value, "go_arg_load");
                    av->is_lvalue = 0;
                }
            }
            arg_vals[i] = av->llvm_value;
            arg_types[i] = LLVMTypeOf(av->llvm_value);
```

(The last two lines already exist — only the load block is new. Match the loop's exact variable names and indentation; read the function first.)

- [ ] **Step 4: Rebuild and verify the probe passes**

Run: `make lexer`, then `bin/goo -o build/go_lvalue_arg_probe examples/go_lvalue_arg_probe.goo && ./build/go_lvalue_arg_probe`
Expected: `7`.

- [ ] **Step 5: Run the gate**

`eval "$(opam env --switch=default)"` (standalone), `make verify` → all PASS, golden 166/0; the goroutine goldens (`go-probe`, `escape-probe`, `escape-range-probe`, `go_identifiers_probe`, stress probes) must stay green. `make test` → 76/1.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/statement_codegen.c examples/go_lvalue_arg_probe.goo examples/go_lvalue_arg_probe.expected.txt
git commit --no-gpg-sign -m "fix(codegen): load lvalue goroutine args before boxing

go f(p.V) boxed the field's ADDRESS (and typed the box field ptr),
failing LLVM verification at the thunk call. Load the value at the go
statement — Go's evaluate-at-spawn semantics — with the same idiom as
the if/for/unary/receiver sites."
```

---

### Task 2: Channel-send lvalue load + coercion (plain send AND select-case send)

**Files:**
- Modify: `src/codegen/lowlevel_codegen.c` (the chan-send function, L15-70 region)
- Modify: `src/codegen/statement_codegen.c:1813-1827` (select-case send)
- Test: `examples/chan_lvalue_send_probe.goo` + `examples/chan_lvalue_send_probe.expected.txt`

**Interfaces:**
- Consumes: the send rvalue path's existing elem-type coercion machinery (unchanged).
- Produces: `ch <- t.N` sends the correctly-widened value, snapshotted at evaluation time; select-case sends of lvalues likewise load first.

- [ ] **Step 1: Write the failing probe**

`examples/chan_lvalue_send_probe.goo`:
```go
package main

import "fmt"

type T struct {
	N int32
	Pad int32
	W int64
}

func main() {
	ch := make_chan(int64, 2)
	t := T{N: 5, Pad: 6, W: 100}
	ch <- t.N
	t.N = 50
	ch <- t.W
	a := <-ch
	fmt.Println(int(a))
	b := <-ch
	fmt.Println(int(b))
	c2 := make_chan(int64, 1)
	select {
	case c2 <- t.N:
		fmt.Println(int(1))
	default:
		fmt.Println(int(0))
	}
	d := <-c2
	fmt.Println(int(d))
}
```

`examples/chan_lvalue_send_probe.expected.txt`:
```
5
100
1
50
```

Coverage: narrow lvalue into wider channel (the corruption case — must be 5, NOT 25769803781), mutation AFTER a buffered send proving snapshot-at-evaluation (still 5), same-width lvalue (100), select-case send of a narrow lvalue post-mutation (50). Every print is wrapped in `int(...)` so all four are the same width (mixed-width Println bug).
NOTE on the select case: after the Task 2 load, the select send's value is an i32 rvalue going into the `LLVMTypeOf`-shaped alloca while the channel elem is i64 — if the received value is wrong (not 50), the select rvalue branch's missing coercion (the out-of-scope pre-existing gap) is being hit THROUGH your change; STOP and report with the observed value rather than widening scope silently. (Empirically the plain-send fix pattern below is expected to handle it — see Step 3 — but verify against reality.)

- [ ] **Step 2: Verify it fails today**

Run: `bin/goo -o build/chan_lvalue_send_probe examples/chan_lvalue_send_probe.goo && ./build/chan_lvalue_send_probe`
Expected: compiles but the first line is 25769803781 (or similar Pad-bleed), not 5.

- [ ] **Step 3: Fix both send sites**

1. `src/codegen/lowlevel_codegen.c` (plain send): immediately after `value_val` is generated and null-checked, insert the load and clear the flag so the EXISTING rvalue coercion+alloca block runs for former lvalues:

```c
    // Auto-load lvalue send values (a field selector returns the field's
    // ADDRESS). Loading here (a) snapshots the value at evaluation time —
    // Go semantics; the old path memcpy'd from the field address at
    // rendezvous — and (b) routes the value through the elem-type coercion
    // below, which the address shortcut bypassed: sending an int32 field
    // into a chan int64 memcpy'd 8 bytes from a 4-byte field (adjacent
    // memory bled into the payload).
    if (value_val->is_lvalue && value_val->goo_type) {
        LLVMTypeRef vt = codegen_type_to_llvm(codegen, value_val->goo_type);
        if (vt) {
            value_val->llvm_value = LLVMBuildLoad2(codegen->builder, vt,
                                                   value_val->llvm_value, "send_load");
            value_val->is_lvalue = 0;
        }
    }
```

Then check what the lvalue branch below it looked like (`if (!value_val->is_lvalue) { ... }` with the lvalue case using the address directly) — after this insertion the lvalue case is unreachable for loadable values; leave the structure intact (the `is_lvalue` fallback now only catches goo_type-less edge values) or simplify per the surrounding style, your judgment, but do NOT remove the rvalue coercion block.

2. `src/codegen/statement_codegen.c:1813-1827` (select-case send): insert the same load block (same comment, one line noting "same as the plain-send path") after its `value_val` generation, before the `if (value_val->is_lvalue)` fork.

- [ ] **Step 4: Rebuild and verify the probe passes**

Run: `make lexer`, then compile+run the probe.
Expected: `5 100 1 50`, one per line. If the last line is not 50, see the Step 1 NOTE — stop and report.

- [ ] **Step 5: Run the gate**

`eval "$(opam env --switch=default)"` (standalone), `make verify` → all PASS, golden 167/0; the channel goldens (`chan-probe`, `chan-elem-probe`, `chan-padded-probe`, `chan-uint-probe`, `unbuffered-probe`, `select-probe`, soak/stress probes) must stay green. `make test` → 76/1.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/lowlevel_codegen.c src/codegen/statement_codegen.c examples/chan_lvalue_send_probe.goo examples/chan_lvalue_send_probe.expected.txt
git commit --no-gpg-sign -m "fix(codegen): load lvalue channel-send values at evaluation time

The send paths (plain and select-case) handed a field selector's
ADDRESS straight to the runtime memcpy: a field narrower than the
channel element bled adjacent memory into the payload (int32 field 5
into chan int64 arrived as 25769803781), and the value was read at
rendezvous instead of at evaluation. Load lvalues up front — the
existing rvalue elem-type coercion then applies — same idiom as the
other lvalue-consumer fixes."
```

---

## Final gate (after both tasks)

`make verify` → ALL GREEN (167/0). `make test` → 76/1. ccomp (no runtime C change — run anyway):
```bash
eval "$(opam env --switch=default)"
make ccomp-link
```

## Self-review notes

- Two consumers, two tasks, independently committable; both reuse the exact idiom shipped four times in PR #91 — no new invention.
- Task 2's STOP-and-report fork fired as designed; the select-send rvalue coercion WAS then brought into scope by controller decision and shipped in the same commit (see the landmark note above).
- Out of scope, recorded: `@ptr` DEREF codegen returns goo_type=NULL (niche syntax, `*p` unaffected); select-send rvalue coercion gap.
