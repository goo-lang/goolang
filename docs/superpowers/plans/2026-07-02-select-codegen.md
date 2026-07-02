# Select Codegen Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix two silent-miscompile bugs found during PR #94: (Task 1) multi-statement select-case bodies emit only the FIRST statement; (Task 2) `var <narrow-int> = <initializer>` stores the initializer at its own (wider) width — an out-of-bounds stack write that clobbers the adjacent slot (root-caused from the reviewer's 'select recv corruption' repro; select was a bystander).

**Architecture:** Task 1 is a one-line-class fix: `codegen_generate_select_stmt` calls `codegen_generate_statement` ONCE on `select_case->body` (statement_codegen.c:1727-1728) while the body is a `->next` statement chain — switch codegen already loops (statement_codegen.c:595-600); mirror it. The checker (PR #94) already type-checks the full chain.

**Tech Stack:** C23, LLVM-C. Codegen-only.

## Global Constraints

- Branch: `fix/select-codegen` (already created off main @e1b54fb — do NOT commit on main).
- Commits: conventional style, imperative mood, `--no-gpg-sign`. Stage only named files; never stage anything under `.superpowers/`.
- Gate: `make lexer`, probe, then `eval "$(opam env --switch=default)"` STANDALONE, then `make verify` (all PASS; golden baseline 168/0 grows to 169/0) and `make test` (76 pass / 1 pre-existing skip). Select/channel goldens must stay green.
- Probes: same-width prints only (mixed-width Println bug).

## Reference: verified code landmarks (2026-07-02, main @e1b54fb)

- The bug: `src/codegen/statement_codegen.c:1726-1733` — `if (select_case->body) { if (!codegen_generate_statement(codegen, checker, select_case->body)) {...} }` — emits one statement; the parser chains body statements via `->next`.
- The reference: switch case-body loop, `src/codegen/statement_codegen.c:595-600` — `for (ASTNode* s = clause->body; s; s = s->next) { ... }` with the same error-cleanup shape (`codegen_pop_loop` + `free` + return 0).
- Terminator handling after the body (`:1735+`, "Branch to end, unless the body already terminated") stays as-is — it reads the CURRENT insert block after the loop, same as switch's `:601-602`.

---

### Task 1: Emit full select-case bodies

**Files:**
- Modify: `src/codegen/statement_codegen.c:1726-1733`
- Test: `examples/select_body_probe.goo` + `examples/select_body_probe.expected.txt`

**Interfaces:**
- Consumes: PR #94's checker (full-chain type annotations already present).
- Produces: every statement in a select-case body executes.

- [ ] **Step 1: Write the failing probe**

`examples/select_body_probe.goo`:
```go
package main

import "fmt"

func main() {
	ch := make_chan(int, 1)
	total := 0
	select {
	case ch <- 5:
		fmt.Println(1)
		total = total + 10
		fmt.Println(2)
	default:
		fmt.Println(0)
	}
	fmt.Println(total)
	v := <-ch
	select {
	case ch <- 9:
		fmt.Println(3)
	default:
		fmt.Println(4)
		total = total + v
		fmt.Println(total)
	}
}
```

`examples/select_body_probe.expected.txt`:
```
1
2
10
3
```

Wait — trace it: first select sends 5 (buffered, succeeds) → prints 1, total=10, prints 2. Then prints total (10). `v := <-ch` receives 5, emptying the buffer. Second select: `ch <- 9` succeeds (buffer empty) → prints 3. Expected: `1 2 10 3`. The default arm's multi-statement body is not exercised on the success path — REWORK: make the second select exercise default by NOT receiving first. Final probe (use this, not the sketch above):

```go
package main

import "fmt"

func main() {
	ch := make_chan(int, 1)
	total := 0
	select {
	case ch <- 5:
		fmt.Println(1)
		total = total + 10
		fmt.Println(2)
	default:
		fmt.Println(0)
	}
	fmt.Println(total)
	select {
	case ch <- 9:
		fmt.Println(3)
	default:
		fmt.Println(4)
		total = total + 100
		fmt.Println(total)
	}
	v := <-ch
	fmt.Println(v + total)
}
```

`examples/select_body_probe.expected.txt`:
```
1
2
10
4
110
115
```

Trace: buffered send 5 succeeds → 1, total=10, 2. Print 10. Second select: buffer FULL → default → 4, total=110, print 110. Recv 5 → 5+110=115. Both a comm arm and a default arm carry 3-statement bodies.

- [ ] **Step 2: Verify it fails today**

Run: `bin/goo -o build/select_body_probe examples/select_body_probe.goo && ./build/select_body_probe`
Expected today: `1 10 4 105` — the trailing statements of each body silently dropped (total never incremented in either body, only first Printlns fire). Record the ACTUAL output in your report (it demonstrates the drop).

- [ ] **Step 3: Fix — loop the body chain**

Replace the single-call block at statement_codegen.c:1726-1733 with the switch idiom:

```c
        // Generate case body. The body is a ->next statement chain — loop it
        // like switch codegen does (a single codegen_generate_statement call
        // silently dropped every statement after the first).
        for (ASTNode* s = select_case->body; s; s = s->next) {
            if (!codegen_generate_statement(codegen, checker, s)) {
                codegen_pop_loop(codegen);
                free(case_blocks);
                return 0;
            }
        }
```

The terminator check after it stays unchanged.

- [ ] **Step 4: Rebuild and verify the probe passes**

`make lexer`, compile+run the probe. Expected: `1 2 10 4 110 115`.

- [ ] **Step 5: Run the gate**

`eval "$(opam env --switch=default)"` (standalone), `make verify` → all PASS, golden 169/0 (select-probe, parallel-select-soak-probe, select_typecheck_probe green). `make test` → 76/1.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/statement_codegen.c examples/select_body_probe.goo examples/select_body_probe.expected.txt
git commit --no-gpg-sign -m "fix(codegen): emit every statement of a select-case body

codegen_generate_select_stmt called codegen_generate_statement once on
the case body, but the body is a ->next statement chain — everything
after the first statement was silently dropped (no error, no warning;
found while implementing the select type checker). Loop the chain like
switch codegen does."
```

---

## Final gate

`make verify` → ALL GREEN (169/0). `make test` → 76/1. ccomp: opam env standalone, `make ccomp-link` → PASS.


---

### Task 2: Width-coerce var-decl initializers (narrowing Trunc + signedness-correct widening)

**Files:**
- Modify: `src/codegen/function_codegen.c:980-999` (the integer-width block in `codegen_generate_var_decl`)
- Test: `examples/var_width_probe.goo` + `examples/var_width_probe.expected.txt`

**Interfaces:**
- Consumes: `type_is_signed` (used identically by the channel-send widening in lowlevel_codegen.c).
- Produces: `var n int32 = 4` stores 4 bytes (Trunc), not an 8-byte out-of-bounds smash; `var u uint64 = <uint32 var>` ZExts.

**Root cause (controller debugging, 2026-07-02):** `--emit-llvm` on the reviewer's min4 repro shows `store i64 4, ptr %n` with `%n = alloca i32` — the block at function_codegen.c:989-999 widens (SExt) when `from < to` but its comment claims narrowing "is not reached for the supported integer types". FALSE: untyped literals arrive as i64, so every `var <narrow> = <literal>` does an out-of-bounds store; corruption depends on alloca adjacency (the PR #94 reviewer's select repro was one lucky layout). Channel-free minimal: `var a int32 = 7` between two `:=` ints emits the same bad store.

- [ ] **Step 1: Write the failing probe**

`examples/var_width_probe.goo`:
```go
package main

import "fmt"

func main() {
	x := 1111
	var a int32 = 7
	y := 2222
	fmt.Println(x)
	fmt.Println(int(a))
	fmt.Println(y)
	var b int8 = 5
	var c int16 = 300
	fmt.Println(int(b))
	fmt.Println(int(c))
	var neg int32 = 0 - 9
	fmt.Println(int(neg))
	var w int64 = 44
	small := int32(6)
	var wide int64 = small
	fmt.Println(int(w))
	fmt.Println(int(wide))
	ub := uint32(4000000000)
	var uw uint64 = ub
	fmt.Println(uw == uint64(4000000000))
}
```

`examples/var_width_probe.expected.txt`:
```
1111
7
2222
5
300
-9
44
6
true
```

Coverage: the smash shape (narrow var between two wider locals — the neighbors must survive), int8/int16 narrows, a negative narrowing, widening from a variable (SExt path — signed), widening unsigned (`uint32 4000000000 → uint64` must be 4000000000, i.e. ZExt — printed as a bool comparison to keep prints same-width).

- [ ] **Step 2: Verify the miscompile today**

Run: `bin/goo --emit-llvm -o build/var_width.ll examples/var_width_probe.goo` and grep the IR:
`grep -E "alloca i32|store i64.*%a" build/var_width.ll.ll` (adjust to the actual emitted names) — expect a `store i64` into the i32 alloca `%a`. The runtime OUTPUT may accidentally look correct (adjacency luck) — the IR store-width mismatch is the authoritative failing evidence; record it verbatim. Also record the runtime output.

- [ ] **Step 3: Fix the width block**

Replace the block at function_codegen.c:989-999 with:

```c
            // Match the initializer's integer width to the declared type.
            // Untyped literals arrive at the default (widest) width, so BOTH
            // directions occur here:
            //   narrowing (var n int32 = 4): the literal is i64 — without a
            //     Trunc the store below writes 8 bytes into a 4-byte alloca,
            //     silently clobbering the adjacent stack slot (found as
            //     "select recv corruption"; the select was a bystander).
            //   widening (var y int64 = x32): extend with the signedness of
            //     the SOURCE type — SExt for signed, ZExt for unsigned (a
            //     uint32 4e9 must not sign-extend negative), same rule as the
            //     channel-send coercion in lowlevel_codegen.c.
            {
                LLVMTypeRef init_ty = LLVMTypeOf(init_value->llvm_value);
                if (LLVMGetTypeKind(init_ty) == LLVMIntegerTypeKind &&
                    LLVMGetTypeKind(llvm_type) == LLVMIntegerTypeKind) {
                    unsigned from_bits = LLVMGetIntTypeWidth(init_ty);
                    unsigned to_bits   = LLVMGetIntTypeWidth(llvm_type);
                    if (from_bits < to_bits) {
                        int use_sext = init_value->goo_type
                                     ? type_is_signed(init_value->goo_type) : 1;
                        init_value->llvm_value = use_sext
                            ? LLVMBuildSExt(codegen->builder, init_value->llvm_value,
                                            llvm_type, "init_sext")
                            : LLVMBuildZExt(codegen->builder, init_value->llvm_value,
                                            llvm_type, "init_zext");
                    } else if (from_bits > to_bits) {
                        init_value->llvm_value = LLVMBuildTrunc(
                            codegen->builder, init_value->llvm_value, llvm_type, "init_trunc");
                    }
                }
            }
```

CAUTION: this runs in `codegen_generate_var_decl`, which also serves GLOBAL declarations (the `else` at ~L1004 requires `LLVMIsConstant`). `LLVMBuildTrunc`/`SExt`/`ZExt` on a CONSTANT operand folds to a constant in LLVM, but the BUILDER must be positioned — check whether this block is reachable with `codegen->current_function == NULL` (global path). If it is, guard the builder-using coercion with `codegen->current_function &&` and, for the global path, rebuild integer constants at the target width with `LLVMConstInt(llvm_type, LLVMConstIntGetZExtValue(...), type_is_signed(...))` — the exact pattern composite_codegen.c:988-996 uses for the same LLVM-22 reason. Verify with a package-level `var g int32 = 7` compile.

- [ ] **Step 4: Rebuild and verify**

`make lexer`, run the probe → expected output exactly. Re-emit the IR and confirm `store i32` (post-Trunc) into `%a`, no `store i64` into any i32 alloca. Also verify the reviewer's canonical repro now behaves: `bin/goo -o build/min4 build/review/min4.goo && ./build/min4` → `1` then `5`.

- [ ] **Step 5: Run the gate**

`eval "$(opam env --switch=default)"` (standalone), `make verify` → all PASS, golden 170/0 (Task 1 already added one; int-width goldens like `int64-probe`, `conv-probe`, `chan-uint-probe`, `nullable-width-probe` must stay green). `make test` → 76/1.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/function_codegen.c examples/var_width_probe.goo examples/var_width_probe.expected.txt
git commit --no-gpg-sign -m "fix(codegen): width-coerce var-decl initializers before the store

var n int32 = 4 stored the untyped literal at its default i64 width
into the i32 alloca — an out-of-bounds 8-byte store that silently
clobbered the adjacent stack slot (surfaced as 'select recv
corruption'; the select was a bystander). Trunc when the initializer
is wider; extend by the SOURCE type's signedness when narrower (SExt
was unconditional — a uint32 4e9 initializer sign-extended negative).
Same coercion rule as the channel-send path."
```


## Final gate (after both tasks)

`make verify` -> ALL GREEN (170/0). `make test` -> 76/1. ccomp: opam env standalone, `make ccomp-link` -> PASS.

## Self-review notes

- Task 2 was appended after the controller root-caused the reviewer's repro via --emit-llvm (IR store-width mismatch is the authoritative evidence; runtime corruption is adjacency-dependent).
- Task 2 deliberately also fixes the unsigned-widening SExt (same block, same probe, channel-send precedent) — narrowing alone would leave a known wrong-value bug in the exact lines being edited.
- FOLLOW-UP note (not tasked): the recorded 'mixed-width Println corrupts the middle arg' latent bug may share this root cause family (width-mismatched stack stores) — worth re-probing after Task 2 lands.
- Task 1's probe Step 1 includes a worked trace; the first sketch is superseded by the final probe (kept for the reasoning trail).
