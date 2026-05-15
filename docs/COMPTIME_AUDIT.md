# Comptime audit (M11-audit, 2026-05-15)

Empirical state of Zig-style `comptime` end-to-end through `bin/goo`. This is the deliverable for coord task `M11-audit`. Every claim in this document is grounded in a runnable command — verify by running, not by reading.

## TL;DR

- The parser, AST, and lexer for `comptime` all work. The comptime *engine* in `src/comptime/` (~6,200 LOC) is fully unreachable from the normal compile pipeline.
- `comptime const X int = 42` *appears* to compile and run successfully (exit 0). The output is wrong (`*` not `42`) but this is **a pre-existing `fmt.Println(int_const)` bug, not a comptime bug** — regular `const X int = 42` produces the same wrong output.
- `comptime { ... }` block form fails cleanly at `src/types/type_checker.c:555` (`Unknown statement type`) because `type_check_statement` has no `case AST_COMPTIME_BLOCK` arm.
- **LLVM is silently doing the comptime work for arithmetic-on-int-literals already** via its standard constant folder. This means the originally-planned MVP demo (`comptime const X = 6 * 7`) would prove *nothing* about comptime — it would behave identically with or without the `comptime` keyword. The MVP must be re-scoped to use something LLVM *cannot* fold.
- The C-level engine tests don't even link (`make comptime_test` fails with `Undefined symbols: _type_new`) — the Makefile target's source list is missing `src/types/types.c`.

## Audit programs

Five `bin/goo` invocations. Source files in `/tmp/audit_{a,b,c,d,e}.goo`, reproducible via the commands below.

| ID | Source | Compile | Run output | Exit | Diagnosis |
|---|---|---|---|---|---|
| a | `comptime const X int = 42` | OK | `*` | 0 | silent miscompile (Println bug) |
| b | `comptime const X int = 6 * 7` | OK | `*` | 0 | same — proves LLVM folded `6*7` |
| c | bare `comptime { x := 1 + 1 }` | **fails** | — | 1 | `type_check_statement` default arm |
| d | `const X int = 42` (no comptime) | OK | `*` | 0 | pre-existing const-print bug |
| e | `const X int = 65` (ASCII A) | OK | `A` | 0 | confirms char-printing hypothesis |

Reproduction:
```bash
bin/goo -o /tmp/a /tmp/audit_a.goo && /tmp/a   # prints *
bin/goo -o /tmp/b /tmp/audit_b.goo && /tmp/b   # prints *
bin/goo -o /tmp/c /tmp/audit_c.goo             # Type error at line:9:1 Unknown statement type
bin/goo -o /tmp/d /tmp/audit_d.goo && /tmp/d   # prints *
bin/goo -o /tmp/e /tmp/audit_e.goo && /tmp/e   # prints A
```

## Per-layer state

### Lexer — ✓ working
`comptime` keyword produces `TOKEN_COMPTIME` (token.c keyword table). Verified by program (a)/(b)/(c) all parsing the keyword without lex errors.

### Parser — ✓ working
Three grammar rules in `src/parser/parser.y`:
- `comptime_block: COMPTIME block` (line 1454) → builds `AST_COMPTIME_BLOCK`
- `COMPTIME FUNC ... { ... }` (line 300) → builds `FuncDecl` with `is_comptime = 1`
- `COMPTIME CONST ident type ASSIGN expr` (line 541) → builds `ConstDeclNode` with `is_comptime = 1`

The bare `comptime { }` form (program c) reaches the parser without error; the rejection happens later (in the type checker).

### AST — ✓ working
- `AST_COMPTIME_BLOCK` enum at `include/ast.h:73`
- `ComptimeBlockNode` struct, `ast_comptime_block_new()`, name table, free handling at `src/ast/ast.c:62, 237, 847-851`
- `is_comptime` flag on `FuncDeclNode` and `ConstDeclNode`

### Type checker — ✗ **dispatch gap**
**Smoking gun #1: `AST_COMPTIME_BLOCK` is never dispatched.**
- `src/types/type_checker.c:513-557` is `type_check_statement`. Its switch covers `AST_BLOCK_STMT`, `AST_EXPR_STMT`, `AST_VAR_DECL`, `AST_IF_STMT`, `AST_IF_LET_STMT`, `AST_FOR_STMT`, `AST_RETURN_STMT`, `AST_BREAK_STMT`/`CONTINUE_STMT`, `AST_GO_STMT`, `AST_SELECT_STMT`. **No `case AST_COMPTIME_BLOCK`.** Default arm (line 554-555) emits `"Unknown statement type"` — exactly what program (c) sees.

**Smoking gun #2: `is_comptime` is never read.**
- `grep -rn 'is_comptime' src/types/` → empty.
- `type_check_const_decl` (`src/types/type_checker.c:407-459`) processes `ConstDeclNode` identically whether `is_comptime` is 0 or 1. The flag is silently ignored.

The comptime engine's intended entry point (`comptime_type_evaluate` declared at `include/types.h:440`, `type_check_comptime_expr` at `include/types.h:446`, `type_check_comptime_block` at `include/types.h:447`) — these are declared but never *called* from the visitor dispatch. They're orphaned API surface.

### Codegen — ✗ **dispatch gap**
**Smoking gun #3: codegen also ignores `is_comptime`.**
- `grep -rn 'is_comptime\|AST_COMPTIME' src/codegen/` → empty.
- `codegen_const_decl` (`src/codegen/function_codegen.c:365-440`-ish) calls `codegen_generate_expression` on the RHS, expects `LLVMIsConstant` to be true (line 387), then emits a global with the value as initializer. **LLVM does the arithmetic folding here** — that's why program (b) produces 42 even with no comptime engine involvement.

**Smoking gun #4: no `AST_COMPTIME_BLOCK` arm in the statement-codegen switch.** The block form would crash or be ignored if it ever reached codegen, but it doesn't because the type checker rejects it first.

### Runtime / fmt.Println — ✗ **separate bug, blocks any const-printing demo**
Programs (a)/(b)/(d) all print `*` for value 42; program (e) prints `A` for value 65. The int *value* is correct (LLVM stored it correctly, the symbol lookup works). The print *format* is wrong: `fmt.Println` is treating an `int`-typed const as `char/byte`. This is unrelated to comptime but **blocks the planned MVP demo** because the demo prints an int constant.

Suspected location: how `fmt.Println` resolves the LLVM type of its var-arg argument when the source value is a global const (which is a pointer-to-int, not an int directly). The codegen path for `fmt.Println(X)` likely passes the *pointer* and the receiving format path mis-types it.

### Comptime engine — present, scaffolded, unreachable
- `src/comptime/` is 6,222 lines across 8 files. Two entry points reference `AST_COMPTIME_BLOCK`:
  - `src/comptime/comptime.c:1112` — `if (!comptime_block || comptime_block->type != AST_COMPTIME_BLOCK) { ... }`
  - `src/comptime/comptime_types.c:272` — same type guard
- No call site in `src/types/` or `src/codegen/` invokes these. They're library entry points with no callers.
- C-level tests at `tests/test_comptime.c` exercise the engine directly via C calls, never via a parsed Goo program.

### Tests — ✗ **don't even link**
`make comptime_test` fails:
```
Undefined symbols for architecture arm64: _type_new
  referenced from _comptime_value_get_type in comptime-21dd9d.o
```
The Makefile target at line 928 lists `tests/test_comptime.c $(SRCDIR)/comptime/comptime.c $(SRCDIR)/ast/ast.c $(SRCDIR)/lexer/lexer.c $(SRCDIR)/lexer/token.c` but **omits `src/types/types.c`** which defines `type_new` (referenced from `comptime.c:comptime_value_get_type`). Joins the family of broken make targets — `make test-lexer`, `make test-units` (per session memory `status-docs-not-ground-truth`).

## Exact functions to fix (minimum cut for M11)

1. **`src/types/type_checker.c:513` — `type_check_statement`**
   Add `case AST_COMPTIME_BLOCK: return type_check_comptime_block(checker, stmt);` arm before the default. Helper function needs writing (the declaration at `include/types.h:447` exists but the body doesn't, per `grep`).

2. **`src/types/type_checker.c:407` — `type_check_const_decl`**
   Read `const_decl->is_comptime`. If set, evaluate the RHS via `comptime_type_evaluate()` and attach the resulting `ComptimeValue` to the const decl so codegen can read it. For arithmetic-on-int-literals this is redundant with LLVM folding — but it's the dispatch that makes the *engine* reachable.

3. **`src/codegen/function_codegen.c:~370` — `codegen_const_decl`**
   Read `is_comptime` (or the attached `ComptimeValue` from step 2) and emit the comptime-evaluated value directly as an LLVM constant. For the MVP this is also redundant with LLVM folding for trivial arithmetic — but is required for **anything LLVM can't fold** (recursive function calls, comptime-only constructs).

4. **`src/codegen/codegen.c:~236` — statement dispatch**
   Add `case AST_COMPTIME_BLOCK:` arm. For MVP scope: emit nothing (block produces no runtime code). Future: lower escaped comptime values into outer-scope constants.

5. **Makefile target for `comptime_test`** (line 928)
   Add `src/types/types.c` (and probably `src/errors/error.c`) to the source list so the C-level engine tests can actually build & run. Without this, the engine has no regression net.

## Revised MVP demo strategy

The original M11 plan's MVP (`comptime const X int = 6 * 7` → `42`) is **insufficient** because:
1. LLVM already folds `6 * 7` to `42` at compile time during normal const codegen — proves nothing about comptime.
2. `fmt.Println(int_const)` is broken regardless of comptime — would print `*` not `42`.

Two viable replacement strategies:

### Option α — strengthen the MVP (recommended)
Demo a `comptime const` whose RHS uses *something LLVM does not constant-fold by default*. Candidates:
- Recursive function call: `comptime const FIB10 int = fib(10)` — requires comptime function dispatch.
- Comptime-only operator or builtin: e.g., a hypothetical `@sizeof`, `@typeof`, `@compiler_version` — would need to be added.
- Differential probe: `comptime const A int = expensive_loop()` vs. `const B int = expensive_loop()` — if A is a literal in IR and B is a `call`, comptime is verifiably distinct.

The recursive `fib(10)` path is the most informative and aligns most closely with Zig's design. **It also requires fixing the `fmt.Println(int)` bug as a prerequisite** so the value can be verified at runtime — or sidestepping it by using exit code (`os.Exit(fib(10))` → process exits 55).

### Option β — keep the MVP at `6 * 7`, fix Println, accept that LLVM does the work
Cheaper. Proves the dispatch wiring exists without proving the engine evaluates. Honest if labeled clearly: "M11 MVP wires the dispatch; future M11-* tasks (function calls, comptime-only ops) prove evaluation."

**Recommendation:** Option α with `os.Exit` as the verification channel — sidesteps the Println bug, proves LLVM-can't-fold-this evaluation, and stays small. Concretely:
```goo
package main
import "os"

func fib(n int) int {
    if n < 2 { return n }
    return fib(n-1) + fib(n-2)
}

comptime const FIB10 int = fib(10)

func main() {
    os.Exit(FIB10)  // exits 55 iff comptime evaluated fib(10)
}
```
`make comptime-probe` runs the binary and asserts exit code 55.

## Plan revisions

The originally-planned M11 chain is *mostly* still right, but two tasks need scope adjustment:

| Task | Original scope | Revised scope |
|---|---|---|
| M11-probe-gate | `examples/comptime_probe.goo` prints `42` | Probe uses `os.Exit(FIB10)` → exit 55, with `fib(10)` as the RHS — sidesteps Println bug, proves LLVM-folding is insufficient |
| M11-types-const | Wire `is_comptime` for arithmetic | Wire `is_comptime` + dispatch into the comptime engine for function-call RHS (the engine must be reachable, not just the flag) |
| M11-codegen-const | Emit literal from comptime value | Same — but now actually load-bearing (LLVM can't fold `fib(10)` itself) |

The remaining tasks (`M11-probe-strengthen`, `M11-block-dispatch`, `M11-verify-gate`) are unchanged.

A new prerequisite emerges:
- **M11-pre-println-fix** (or `M9-fmt-println-int` if filed under M9) — fix the `fmt.Println(int_const)` char-printing bug. Either as a strict prereq of the MVP or by using `os.Exit` as the verification channel to sidestep it.

## Open questions / risks

- **R1.** `type_check_comptime_block` is declared at `include/types.h:447` but its definition is not in `src/types/` per `grep`. Either it's elsewhere or needs writing. Confirm during M11-types-const.
- **R2.** `comptime_type_evaluate` likely needs a working `ComptimeContext` and bindings for `fib` to be callable. The engine may not yet support recursive function dispatch — would push M11 toward Option β + a follow-up "M11-comptime-fn-call" task.
- **R3.** Adding `src/types/types.c` to the `comptime_test` link line may surface further missing-symbol cascades (the engine probably depends on more of `src/types/`, `src/errors/`, etc.). Audit-of-the-audit-target.
- **R4.** Touching `src/types/type_checker.c` or `src/codegen/function_codegen.c` historically destabilizes the existing gates. Mitigation per plan: run `baseline-probe` + `smoke-stdlib` + `v2-bootstrap-pilot` after each M11-* commit.

## Conclusion

Minimum cut for M11 MVP: **2 dispatch arms (~50 LOC total) + 1 helper function body + 1 Makefile-line fix + 1 example file**. The hard work is in deciding what the demo actually proves; the wiring itself is small.

The Option α `fib(10)` demo is the cleanest path: it sidesteps the unrelated Println bug, exercises the comptime engine in a way LLVM cannot replicate, and produces a binary whose exit code is unambiguous evidence of compile-time evaluation.
