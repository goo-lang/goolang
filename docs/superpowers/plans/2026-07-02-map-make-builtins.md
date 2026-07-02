# Map/Make Builtins Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the `len(m)` compiler segfault on maps, add the `delete(m, k)` builtin, and implement the `make` builtin for maps (`make(map[K]V[, hint])`) and slices (`make([]T, n[, cap])`).

**Architecture:** Maps are a string-keyed linked list (`GooMapSV` in `src/runtime/runtime.c`) lowered to an opaque `i8*`. All three features follow the existing builtin pattern: builtins are ordinary identifiers (NOT keywords) routed by name-match in the type checker (`type_check_call_expr`) and codegen (`codegen_generate_call_expr`). The only grammar change is allowing `map_type`/`slice_type` as the first call argument — both have first-sets disjoint from expressions (`MAP` keyword / `RBRACKET_SLICE` token), so this is expected to add zero bison conflicts.

**Tech Stack:** C23, bison, LLVM-C API (LLVM 22), CompCert-compatible runtime C.

## Global Constraints

- Branch `feat/map-make-builtins` off current `main` — **create the branch BEFORE the first commit** (a prior PR was accidentally committed on main).
- Commits: conventional style, imperative mood, `--no-gpg-sign`. Never stage anything under `.superpowers/`. Stage only named files.
- **Header edits require `make clean`** — the Makefile has no header dependencies; stale objects silently miscompile. Any new enum values go at the TAIL of the enum.
- New runtime C must stay CompCert-compilable (no VLAs, no `__int128`, plain C).
- Bison conflict baseline: **78 shift/reduce + 256 reduce/reduce** (measured on pre-task HEAD; the committed `parser.output` snapshot saying 68/156 was stale). After any grammar change, verify the count. Target: unchanged. If it changes, judge by behavior (full gate) — but report it.
- Gate per task: `make lexer` (rebuild), run the task's probe(s), then `make verify` (golden suite, currently **156/0** — your task adds probes, so the pass count grows but failures must stay 0) and `make test` (currently **76 pass / 1 pre-existing failure** — 76/1 is green; do not chase the 1).
- ccomp gate (final task only): `eval "$(opam env --switch=default)"` as a STANDALONE command (not piped/inlined), then `make ccomp-link` → must PASS.
- Probes: create `examples/<name>_probe.goo` + `examples/<name>_probe.expected.txt`. The golden suite (`make test-golden`, part of `verify`) auto-discovers them — no Makefile registration needed.
- KNOWN LATENT BUG (do not trip it): 3+ sequential `fmt.Println` of **different-width** ints corrupts the middle arg. In probes, print same-width values or wrap narrow values in `int(...)`.
- Runtime maps support **string keys only** (`goo_map_set_sv(map, const char* key, int64_t slot)`). Non-string map keys must produce a clean type error, never reach codegen.
- Zsh gotcha: don't write `make $VAR` with an unquoted variable in scripts (word-split).

## Reference: current code landmarks (verified 2026-07-02)

- Map runtime: `src/runtime/runtime.c` L507-556 — `goo_map_new_sv()`, `goo_map_set_sv(m,k,v)`, `goo_map_get_sv(m,k)`, `goo_map_get_sv_ok(m,k,out,found)`. Struct `GooMapSV { struct GooMapEntrySV* head; }` in `include/runtime.h` L120-128.
- Map LLVM lowering: `src/codegen/type_mapping.c` L56-58 — `TYPE_MAP` → opaque `i8*`.
- The len segfault: `src/codegen/call_codegen.c` L270-287 — `LLVMBuildExtractValue(raw, 1, "len")` on the opaque map pointer. `cap` at L288-302 has the identical hazard.
- Builtin registration (identifier resolution): `type_checker_add_builtin_functions`, `src/types/type_checker.c` L210-304.
- Builtin call checking: `type_check_call_expr` name-match chain, `src/types/expression_checker.c` L903-1050 (`new` arm at ~L962 is the template — it converts an arg to a Type via `type_from_ast`).
- Builtin codegen: `codegen_generate_call_expr` name-match chain, `src/codegen/call_codegen.c` L162+ (`new` arm at L211-232 is the template).
- Runtime symbol declarations for codegen: `src/codegen/runtime_integration.c` L326-349 (map functions).
- Value boxing: `codegen_map_value_to_slot` / `codegen_map_slot_to_value`, `src/codegen/codegen.c` L475-498. Keys pass as the string's data pointer (`ExtractValue(key, 0)`).
- Call grammar: `parser.y` L1434-1455 (`primary_expr LPAREN expression_list RPAREN`). Types: `map_type` L2039-2050, slice types use the `RBRACKET_SLICE` lexer-bridge token (L2023-2036, `src/parser/lexer_bridge.c`).

> The C snippets below are reference implementations. Match the exact AST field names, helper names, and error-reporting calls used by the neighboring arms you're editing (e.g. the `new` arm) — those neighbors are the source of truth for plumbing details like how the arg linked-list is walked and how errors are emitted.

---

### Task 1: Fix `len(m)` segfault (runtime `goo_map_len_sv` + typecheck guard)

**Files:**
- Modify: `src/runtime/runtime.c` (~L556, after `goo_map_get_sv_ok`)
- Modify: `include/runtime.h` (~L128, after map prototypes)
- Modify: `src/codegen/runtime_integration.c` (~L349, after map declarations)
- Modify: `src/codegen/call_codegen.c` (L270-302, `len` and `cap` arms)
- Modify: `src/types/expression_checker.c` (`type_check_call_expr` chain ~L903-1050)
- Test: `examples/map_len_probe.goo` + `examples/map_len_probe.expected.txt`

**Interfaces:**
- Consumes: existing `GooMapSV` linked list, opaque-ptr map lowering.
- Produces: `int64_t goo_map_len_sv(GooMapSV* m)` — runtime, counts entries; `len(m)` valid on maps; `len`/`cap` on unsupported types = clean type error (used by Task 2's probe via `len` after `delete`).

- [ ] **Step 1: Write the failing probe**

`examples/map_len_probe.goo`:
```go
package main

import "fmt"

func main() {
	m := map[string]int{"a": 1, "b": 2, "c": 3}
	fmt.Println(len(m))
	m["d"] = 4
	fmt.Println(len(m))
	m["a"] = 9
	fmt.Println(len(m))
}
```

`examples/map_len_probe.expected.txt`:
```
3
4
4
```
(Note: `m["a"] = 9` overwrites an existing key — length stays 4.)

- [ ] **Step 2: Verify it fails (segfault today)**

Run: `bin/goo examples/map_len_probe.goo; echo "exit=$?"`
Expected: `exit=139` (compiler SIGSEGV) — this is the bug.

- [ ] **Step 3: Add the runtime function**

In `src/runtime/runtime.c`, after `goo_map_get_sv_ok` (~L556):
```c
int64_t goo_map_len_sv(GooMapSV* m) {
    if (!m) return 0;
    int64_t n = 0;
    for (struct GooMapEntrySV* e = m->head; e != NULL; e = e->next) {
        n++;
    }
    return n;
}
```

In `include/runtime.h`, after the existing map prototypes:
```c
int64_t goo_map_len_sv(GooMapSV* m);
```

- [ ] **Step 4: Declare the symbol for codegen**

In `src/codegen/runtime_integration.c`, next to the other `goo_map_*` declarations (~L326-349), declare `goo_map_len_sv` with LLVM type `i64 (ptr)`. Follow the exact declaration pattern of `goo_map_get_sv` (which is `i64 (ptr, ptr)`).

- [ ] **Step 5: Add the TYPE_MAP branch in len codegen**

In `src/codegen/call_codegen.c` `len` arm (L270-287): BEFORE the ExtractValue path, check the argument's Goo type. If it is `TYPE_MAP`, load the map pointer if the value is an lvalue (mirror the existing load logic in the same arm), then emit a call to `goo_map_len_sv` and return its `i64` result wrapped the same way the slice path wraps its result. The slice/string path stays untouched.

- [ ] **Step 6: Add the typecheck guard for len/cap**

In `src/types/expression_checker.c` `type_check_call_expr`, add arms to the builtin name-match chain (next to the existing `cap` arm ~L1003):
- `len`: type-check the single argument; if its type kind is not one of `TYPE_SLICE`, `TYPE_STRING`, `TYPE_MAP` → clean type error `"len() requires a slice, string, or map argument"`. Result type: INT64. (This converts today's `len(array)` compiler crash into a clean error too — arrays are a follow-up.)
- `cap`: if the existing `cap` arm doesn't already reject maps, make `cap(m)` on a `TYPE_MAP` a clean type error `"cap() is not defined for maps"` (Go semantics; the codegen ExtractValue path would segfault on the opaque pointer otherwise).
Match the error-emission style of the neighboring arms exactly.

- [ ] **Step 7: Rebuild (header changed → clean) and verify the probe passes**

Run: `make clean && make lexer` then `bin/goo examples/map_len_probe.goo && ./examples/map_len_probe.out` (check how golden probes execute — `scripts/run_golden.sh` shows the compile+run convention; produce output matching the expected file).
Expected: `3` / `4` / `4`.

Also verify the guard: create a throwaway file with `cap` applied to a map — expect a clean type error mentioning cap, exit code nonzero, NO segfault. Do not commit the throwaway.

- [ ] **Step 8: Run the gate**

Run: `make verify` → golden must be N/0 (N grew by 1). Run: `make test` → 76/1.

- [ ] **Step 9: Commit**

```bash
git add src/runtime/runtime.c include/runtime.h src/codegen/runtime_integration.c src/codegen/call_codegen.c src/types/expression_checker.c examples/map_len_probe.goo examples/map_len_probe.expected.txt
git commit --no-gpg-sign -m "fix(codegen): len(m) on a map segfaulted the compiler

len/cap codegen assumed a {ptr,len,cap} aggregate and called
LLVMBuildExtractValue on the opaque map pointer. Add goo_map_len_sv
runtime counting, route TYPE_MAP through it, and give len/cap a
typecheck guard so unsupported argument types get a clean error
instead of reaching the ExtractValue path."
```

---

### Task 2: `delete(m, k)` builtin

**Files:**
- Modify: `src/runtime/runtime.c` (after `goo_map_len_sv`)
- Modify: `include/runtime.h`
- Modify: `src/codegen/runtime_integration.c`
- Modify: `src/types/type_checker.c` (`type_checker_add_builtin_functions` L210-304)
- Modify: `src/types/expression_checker.c` (`type_check_call_expr` chain)
- Modify: `src/codegen/call_codegen.c` (builtin dispatch chain)
- Test: `examples/map_delete_probe.goo` + `examples/map_delete_probe.expected.txt`

**Interfaces:**
- Consumes: `goo_map_len_sv` from Task 1 (probe uses `len` to observe deletion).
- Produces: `void goo_map_delete_sv(GooMapSV* m, const char* k)` — runtime; `delete(m, k)` statement-position builtin returning void.

- [ ] **Step 1: Write the failing probe**

`examples/map_delete_probe.goo`:
```go
package main

import "fmt"

func main() {
	m := map[string]int{"a": 1, "b": 2, "c": 3}
	delete(m, "b")
	fmt.Println(len(m))
	fmt.Println(m["b"])
	v, ok := m["b"]
	fmt.Println(v)
	fmt.Println(ok)
	delete(m, "zzz")
	fmt.Println(len(m))
	delete(m, "a")
	delete(m, "c")
	fmt.Println(len(m))
}
```

`examples/map_delete_probe.expected.txt`:
```
2
0
0
false
2
0
```

- [ ] **Step 2: Verify it fails**

Run: `bin/goo examples/map_delete_probe.goo 2>&1 | head -3`
Expected: type error — `Undefined variable 'delete'`.

- [ ] **Step 3: Add the runtime function**

FIRST read `goo_map_set_sv` (`src/runtime/runtime.c` L519-533) and check whether it duplicates the key (strdup/malloc+copy) or stores the caller's pointer. Then in `src/runtime/runtime.c` after `goo_map_len_sv`:
```c
void goo_map_delete_sv(GooMapSV* m, const char* k) {
    if (!m || !k) return;
    struct GooMapEntrySV** p = &m->head;
    while (*p != NULL) {
        if (strcmp((*p)->key, k) == 0) {
            struct GooMapEntrySV* dead = *p;
            *p = dead->next;
            /* free dead->key here IFF goo_map_set_sv owns/duplicates it */
            free(dead);
            return;
        }
        p = &(*p)->next;
    }
}
```
Prototype in `include/runtime.h`: `void goo_map_delete_sv(GooMapSV* m, const char* k);`

- [ ] **Step 4: Wire all three compiler layers**

1. `src/codegen/runtime_integration.c`: declare `goo_map_delete_sv` as `void (ptr, ptr)`, following the `goo_map_set_sv` declaration pattern.
2. `src/types/type_checker.c` `type_checker_add_builtin_functions`: register `delete` as an `is_builtin=1` predeclared identifier, mirroring how `panic` (L246) is registered (void-returning builtin).
3. `src/types/expression_checker.c` `type_check_call_expr` chain: add a `delete` arm — exactly two arguments; arg1 must be `TYPE_MAP` (else clean error `"delete() requires a map as its first argument"`); arg2 must be assignable to the map's key type (string today); result type void (match how `panic`'s arm expresses a void result).
4. `src/codegen/call_codegen.c` dispatch chain: add a `delete` arm — generate the map expression (load if lvalue, mirroring the map handling in the `len` arm from Task 1), generate the key and extract its data pointer with `LLVMBuildExtractValue(key, 0, ...)` (same as the map-write path in `src/codegen/expression_codegen.c` L703-705), call `goo_map_delete_sv`, return a void/unit ValueInfo the way `panic`'s arm does.

- [ ] **Step 5: Rebuild and verify the probe passes**

Run: `make clean && make lexer` (header changed), then compile+run the probe.
Expected output: exactly the expected file (`2 0 0 false 2 0`, one per line).

- [ ] **Step 6: Run the gate**

Run: `make verify` → N/0. Run: `make test` → 76/1.

- [ ] **Step 7: Commit**

```bash
git add src/runtime/runtime.c include/runtime.h src/codegen/runtime_integration.c src/types/type_checker.c src/types/expression_checker.c src/codegen/call_codegen.c examples/map_delete_probe.goo examples/map_delete_probe.expected.txt
git commit --no-gpg-sign -m "feat(runtime,types,codegen): delete(m, k) map builtin

Linked-list unlink in the runtime (goo_map_delete_sv), registered as
a predeclared void builtin with a two-arg typecheck (map + key), and
codegen'd like the existing map read/write paths (string key passed
as its data pointer)."
```

---

### Task 3: `make(map[K]V[, hint])` — grammar (type as call argument) + map arm

**Files:**
- Modify: `src/parser/parser.y` (call_expr rules L1434-1455)
- Modify: `src/types/expression_checker.c` (`type_check_call_expr` chain)
- Modify: `src/types/type_checker.c` (`type_checker_add_builtin_functions`)
- Modify: `src/codegen/call_codegen.c` (dispatch chain)
- Test: `examples/make_map_probe.goo` + `examples/make_map_probe.expected.txt`

**Interfaces:**
- Consumes: `goo_map_new_sv()` (existing runtime), `map_type`/`slice_type` grammar nonterminals.
- Produces: grammar acceptance of `callee(TYPE)` and `callee(TYPE, exprs...)` where TYPE is a `map_type` or `slice_type` — Task 4 relies on the slice_type variant already parsing after this task; a typecheck `make` arm that Task 4 extends with the slice case.

- [ ] **Step 1: Write the failing probe**

`examples/make_map_probe.goo`:
```go
package main

import "fmt"

func main() {
	m := make(map[string]int)
	fmt.Println(len(m))
	m["x"] = 10
	m["y"] = 20
	fmt.Println(m["x"])
	fmt.Println(m["y"])
	fmt.Println(len(m))
	h := make(map[string]int, 16)
	h["k"] = 5
	fmt.Println(h["k"])
	fmt.Println(len(h))
}
```

`examples/make_map_probe.expected.txt`:
```
0
10
20
2
5
1
```
(Go semantics: the size hint pre-sizes only; `len` of a fresh map is 0. Our list runtime ignores the hint — same observable behavior.)

- [ ] **Step 2: Verify it fails**

Run: `bin/goo examples/make_map_probe.goo 2>&1 | head -2`
Expected: `Parse error ... syntax error`.

- [ ] **Step 3: Grammar — allow a type as the first call argument**

In `parser.y`, add alternatives to the call rule (L1434-1455). First check what `$$` the `type`/`map_type` nonterminals produce (an AST node — the same shape `type_from_ast` consumes; verify in the grammar file). Add:
```
    | primary_expr LPAREN type_call_arg RPAREN
    | primary_expr LPAREN type_call_arg COMMA expression_list RPAREN
```
with a new nonterminal:
```
type_call_arg
    : map_type
    | slice_type
    ;
```
Actions: build the same CallExpr node the existing rules build, with the type AST node as the FIRST argument (prepended to the expression_list in the COMMA variant). Do NOT add `make` as a keyword — it stays an identifier; the checker rejects non-`make` callees with a type argument.

Rationale (for the reviewer) — CORRECTED during Task 3 review: the first-sets are NOT disjoint (`map_lit` also starts with `MAP`; a slice type's first token is `LBRACKET`, shared with `slice_lit`). The parser stays conflict-free via the FOLLOW-token split after `map_type` reduces (`LBRACE` → literal vs `RPAREN`/`COMMA` → type arg) and the `RBRACKET_SLICE` second token for slice types. Empirically verified: 0 conflict delta (78 S/R + 256 R/R unchanged).

- [ ] **Step 4: Verify conflict count**

Run: `make lexer 2>&1 | grep -i conflict; grep -c "conflict" src/parser/parser.output || true` and inspect the summary line in `src/parser/parser.output`.
Expected: still 68 shift/reduce, 156 reduce/reduce. If different, STOP and report before proceeding (behavior-test if the delta is small, but the controller must know).

- [ ] **Step 5: Typecheck arm for `make`**

1. Register `make` as a predeclared `is_builtin=1` identifier in `type_checker_add_builtin_functions` (mirror `new`).
2. In `type_check_call_expr`, add a `make` arm (place it near the `new` arm ~L962, which is the template for treating arg1 as a type):
   - Resolve arg1 to a `Type*` via `type_from_ast` (works for both the new grammar path and an identifier arg — same as `new`).
   - `TYPE_MAP`: key type must be `TYPE_STRING`, else clean error `"make: map keys other than string are not yet supported"`. Optional second arg: type-check it, require an integer type (it's a capacity hint, ignored at codegen). More than 2 args → error.
   - `TYPE_SLICE`: for THIS task emit clean error `"make([]T, ...) is implemented in a later change"` — Task 4 replaces this stub with the real check. (Keeps this task shippable and the error honest if Task 4 is reverted.)
   - Anything else → clean error `"make() requires a map or slice type"`.
   - Result type: the resolved Type; stamp `expr->node_type` so codegen can read it (same mechanism as `new`).
3. Also handle the non-`make` callee with a `type_call_arg`: if the first argument is a type-shaped AST node and the callee is not `make` (and not a conversion the checker already handles), emit clean error `"type used as value"`. Verify what the existing checker does when it type-checks the bare type node — do not let it fall through to a crash.

- [ ] **Step 6: Codegen arm for `make` (map case)**

In `call_codegen.c` dispatch chain (near the `new` arm L211): if callee is `make` and `expr->node_type->kind == TYPE_MAP`: if a hint argument exists, generate it (evaluate for side effects) and discard; emit `call goo_map_new_sv()` (already declared in runtime_integration.c — the map-literal path uses it) and return the `i8*` result as a TYPE_MAP ValueInfo, mirroring how the map-literal codegen (`src/codegen/expression_codegen.c` L86-127) wraps its result.

- [ ] **Step 7: Rebuild and verify the probe passes**

Run: `make lexer` (no header change expected; if you touched any header, `make clean` first), compile+run the probe.
Expected: `0 10 20 2 5 1`, one per line.

- [ ] **Step 8: Run the gate**

Run: `make verify` → N/0. Run: `make test` → 76/1.

- [ ] **Step 9: Commit**

```bash
git add src/parser/parser.y src/types/type_checker.c src/types/expression_checker.c src/codegen/call_codegen.c examples/make_map_probe.goo examples/make_map_probe.expected.txt
git commit --no-gpg-sign -m "feat(parser,types,codegen): make(map[K]V) builtin

Grammar: map_type/slice_type now valid as a call's first argument
(disjoint first-sets - MAP keyword / RBRACKET_SLICE token can't start
an expression; conflict count unchanged). make is a predeclared
identifier, not a keyword. Map case lowers to goo_map_new_sv; the
optional capacity hint is type-checked and ignored (list runtime).
make([]T, n) is stubbed with a clean error for the follow-up."
```

---

### Task 4: `make([]T, n[, cap])` — slice case

**Files:**
- Modify: `src/runtime/runtime.c` (after `goo_map_delete_sv`)
- Modify: `include/runtime.h`
- Modify: `src/codegen/runtime_integration.c`
- Modify: `src/types/expression_checker.c` (replace the Task 3 slice stub)
- Modify: `src/codegen/call_codegen.c` (extend the `make` arm)
- Test: `examples/make_slice_probe.goo` + `examples/make_slice_probe.expected.txt`

**Interfaces:**
- Consumes: Task 3's grammar (`make([]int, 3)` already parses) and typecheck arm (replaces its TYPE_SLICE stub).
- Produces: `void* goo_slice_alloc(int64_t count, int64_t elem_size)` — zero-initialized backing store; `make([]T, n)` → slice with len=n, cap=n; `make([]T, n, c)` → len=n, cap=c.

- [ ] **Step 1: Write the failing probe**

`examples/make_slice_probe.goo`:
```go
package main

import "fmt"

func main() {
	s := make([]int, 3)
	fmt.Println(len(s))
	fmt.Println(cap(s))
	fmt.Println(s[0])
	s[1] = 42
	fmt.Println(s[1])
	t := make([]int, 2, 8)
	fmt.Println(len(t))
	fmt.Println(cap(t))
	b := make([]byte, 4)
	fmt.Println(len(b))
	b[3] = 200
	fmt.Println(int(b[3]))
}
```
(`int(b[3])` deliberately widens the byte before printing — see the mixed-width print constraint.)

`examples/make_slice_probe.expected.txt`:
```
3
3
0
42
2
8
4
200
```

- [ ] **Step 2: Verify it fails**

Run: `bin/goo examples/make_slice_probe.goo 2>&1 | head -2`
Expected: the Task 3 stub's clean type error (`make([]T, ...) is implemented in a later change`) — NOT a parse error (Task 3's grammar covers this).

- [ ] **Step 3: Add the runtime allocator**

`src/runtime/runtime.c`:
```c
void* goo_slice_alloc(int64_t count, int64_t elem_size) {
    if (count <= 0 || elem_size <= 0) {
        /* still return a valid non-NULL allocation for len 0 */
        return calloc(1, 1);
    }
    return calloc((size_t)count, (size_t)elem_size);
}
```
Prototype in `include/runtime.h`: `void* goo_slice_alloc(int64_t count, int64_t elem_size);`
Declare in `src/codegen/runtime_integration.c` as `ptr (i64, i64)`.

- [ ] **Step 4: Typecheck — replace the slice stub**

In the `make` arm's `TYPE_SLICE` case from Task 3: require a length argument (2nd arg), integer-typed; optional 3rd arg (cap), integer-typed; more than 3 args → error; missing length → clean error `"make([]T) requires a length argument"`. Result type: the resolved slice Type, stamped on `expr->node_type`.

- [ ] **Step 5: Codegen — slice case in the `make` arm**

If `expr->node_type->kind == TYPE_SLICE`:
1. Generate the length expr → coerce/extend to `i64` (see how existing slice-literal codegen widths its len — `src/codegen/composite_codegen.c` is the reference).
2. Cap: generate 3rd arg → `i64` if present, else reuse the length value.
3. Element size: compute from the element's LLVM type via `LLVMABISizeOfType(LLVMGetModuleDataLayout(codegen->module), elem_llvm_ty)` (or the codebase's existing element-size helper if one exists — check how `append`'s codegen (L303) sizes elements and use the same mechanism).
4. `data = call goo_slice_alloc(cap, elem_size)` — allocate CAP elements so writes within cap after a future reslice are safe; zero-initialized = Go zero values.
5. Build the `{ptr, i64 len, i64 cap}` slice aggregate exactly the way the slice-literal codegen builds one (same field order, same struct type from `type_mapping.c` L39-53), and return it as a TYPE_SLICE ValueInfo.
No runtime bounds check for `len > cap` in this change — note it as a follow-up in the commit body.

- [ ] **Step 6: Rebuild and verify the probe passes**

Run: `make clean && make lexer` (header changed), compile+run the probe.
Expected: `3 3 0 42 2 8 4 200`, one per line.

- [ ] **Step 7: Full gate including ccomp**

Run: `make verify` → N/0 (N = baseline 156 + 4 new probes). Run: `make test` → 76/1.
Then, as separate commands:
```bash
eval "$(opam env --switch=default)"
make ccomp-link
```
Expected: PASS (byte-identical baseline output). The new runtime functions are plain C — if ccomp rejects anything, fix the runtime code, not the gate.

- [ ] **Step 8: Commit**

```bash
git add src/runtime/runtime.c include/runtime.h src/codegen/runtime_integration.c src/types/expression_checker.c src/codegen/call_codegen.c examples/make_slice_probe.goo examples/make_slice_probe.expected.txt
git commit --no-gpg-sign -m "feat(runtime,types,codegen): make([]T, n[, cap]) slice builtin

Zero-initialized backing store via goo_slice_alloc(count, elem_size)
(calloc = Go zero values), slice aggregate {ptr,len,cap} built like
the slice-literal path. len>cap runtime check is a noted follow-up."
```

---

## Self-review notes

- Spec coverage: len segfault (Task 1), delete (Task 2), make map (Task 3), make slice (Task 4). Range-over-map is explicitly OUT of scope (separate PR — needs a runtime iterator). `make(chan T)` out of scope (`make_chan` builtin already exists; unifying is a follow-up).
- Type consistency: `goo_map_len_sv(GooMapSV*) -> int64_t` (T1, used by T2's probe), `goo_map_delete_sv(GooMapSV*, const char*)` (T2), `goo_slice_alloc(int64_t, int64_t) -> void*` (T4). Task 3 produces the grammar + stub Task 4 consumes.
- Deviation from Go, both deliberate: map capacity hint ignored (list runtime, same observable semantics); no len>cap panic in make (no runtime panic-with-format infra yet; follow-up).
