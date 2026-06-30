# fmt.Printf / fmt.Sprintf Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `fmt.Printf` (formatted print) and `fmt.Sprintf` (formatted string build) with compile-time format-string parsing, so real Go programs with formatted output compile and run.

**Architecture:** Compile-time format walker (format must be a string literal). Printf reuses the existing per-type `goo_print_*` runtime helpers (same dispatch as `codegen_generate_println_call`); Sprintf builds a `goo_string_t` via `goo_string_concat` + new `<type>→string` runtime helpers. Verbs: `%d %s %f %t %v %%`.

**Tech Stack:** C23, LLVM-C, `goo_string_t` runtime, `scripts/run_golden.sh`, `make verify`.

## Global Constraints

- Gate = `make verify` ALL GREEN (incl. ccomp via `eval "$(opam env --switch=default)"`) + `make test` 76/1 + golden, no regressions.
- Commits FAIL to sign — use `git commit --no-gpg-sign`.
- No naked returns / no silent failures; errors via `codegen_error`/`type_error`.
- Golden probes auto-discovered: `examples/<name>.goo` + `examples/<name>.expected.txt`.
- A non-string-literal format arg, an unknown verb, and a verb/arg-count mismatch must each be a CLEAN error (no crash, no silent wrong output).
- Reference: `codegen_generate_println_call` (`src/codegen/call_codegen.c:897`) — the per-type print dispatch to copy. `goo_string_concat` declared at `runtime_integration.c:203`. Println is routed at `call_codegen.c:176` and `:305`. `stdlib_package_lookup` at `expression_checker.c:1118` (Println registered ~:1125). Spec: `docs/superpowers/specs/2026-06-30-fmt-printf-sprintf-design.md`.

---

### Task 1: Runtime `<type>→string` helpers

**Files:**
- Modify: `include/runtime.h` (declare 3 helpers near `goo_string_concat`, line ~71)
- Modify: `src/runtime/runtime.c` (implement near `goo_string_concat`, line ~212)
- Modify: `src/codegen/runtime_integration.c` (declare in the module near `goo_string_concat`, line ~203)

**Interfaces:**
- Produces: `goo_string_t goo_int_to_string(int64_t)`, `goo_string_t goo_float_to_string(double)`, `goo_string_t goo_bool_to_string(int)` — each returns a heap-allocated goo_string. Reusable by a later `strconv` milestone.

- [ ] **Step 1: Declare in `include/runtime.h`** (after `goo_string_concat`):

```c
goo_string_t goo_int_to_string(int64_t value);
goo_string_t goo_float_to_string(double value);
goo_string_t goo_bool_to_string(int value);
```

- [ ] **Step 2: Implement in `src/runtime/runtime.c`.** Read `goo_string_concat` (line ~212) first to match the goo_string allocation convention (how it sets `.data`/`.length`, the heap alloc used). Then add (adapt the struct-field names to match goo_string_t):

```c
goo_string_t goo_int_to_string(int64_t value) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)value);
    char* data = (char*)malloc((size_t)n + 1);
    memcpy(data, buf, (size_t)n + 1);
    goo_string_t s; s.data = data; s.length = (size_t)n; return s;
}
goo_string_t goo_float_to_string(double value) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%g", value);
    char* data = (char*)malloc((size_t)n + 1);
    memcpy(data, buf, (size_t)n + 1);
    goo_string_t s; s.data = data; s.length = (size_t)n; return s;
}
goo_string_t goo_bool_to_string(int value) {
    const char* lit = value ? "true" : "false";
    size_t n = strlen(lit);
    char* data = (char*)malloc(n + 1);
    memcpy(data, lit, n + 1);
    goo_string_t s; s.data = data; s.length = n; return s;
}
```
(Use the EXACT field names from `goo_string_t` in `include/runtime.h` — if it is `{ char* data; size_t length; }` the above is correct; if names differ, match them.)

- [ ] **Step 3: Declare in the module** — in `src/codegen/runtime_integration.c`, right after the `goo_string_concat` declaration (~line 206), mirroring its `string_type`-returning shape. Read lines 203-206 for the exact `add_runtime_function` call form, then add three declarations: each takes ONE param (`goo_int_to_string`: i64; `goo_float_to_string`: double; `goo_bool_to_string`: i32) and returns `string_type`. Use the same `string_type`/`int_type`/etc. locals already in scope there.

- [ ] **Step 4: Build + link smoke**

Run: `eval "$(opam env --switch=default)" && make bin/goo && make test`
Expected: builds clean (the 3 functions link), `make test` 76/1. (No behavior probe yet — Task 3 exercises these via Sprintf.)

- [ ] **Step 5: Commit**

```bash
git add include/runtime.h src/runtime/runtime.c src/codegen/runtime_integration.c
git commit --no-gpg-sign -m "feat(runtime): goo_int/float/bool_to_string helpers for Sprintf"
```

---

### Task 2: fmt.Printf

**Files:**
- Modify: `src/types/expression_checker.c` (`stdlib_package_lookup` — register `fmt.Printf`)
- Modify: `src/codegen/call_codegen.c` (add `codegen_generate_printf_call` + a shared format walker; route it where Println is routed at :176/:305)
- Test: `examples/printf_probe.goo`, `examples/printf_pct_probe.goo`, `examples/printf_v_probe.goo` (+ `.expected.txt`)

**Interfaces:**
- Consumes: `goo_print` / `goo_print_string` / `goo_print_int` / `goo_print_bool` / `goo_print_float` (module functions used by `codegen_generate_println_call`).
- Produces: a shared format walker usable by Task 3's Sprintf. Define it as:
  `static int fmt_emit_segments(CodeGenerator* c, TypeChecker* tc, const char* fmt, ASTNode* args, int sprintf_mode, LLVMValueRef* out_str);` — walks `fmt`; for `sprintf_mode==0` emits `goo_print*` calls and ignores `out_str`; for `sprintf_mode==1` builds and returns the concatenated `goo_string` in `*out_str`. Returns 1 on success, 0 (after `codegen_error`) on a verb/arg mismatch or unknown verb. (Task 2 implements the Printf side; Task 3 fills the sprintf branch.)

- [ ] **Step 1: Write the failing probes**

`examples/printf_probe.goo`:
```go
package main
import "fmt"
func main() {
	fmt.Printf("x=%d y=%s t=%t\n", 42, "hi", true)
}
```
`examples/printf_probe.expected.txt`:
```
x=42 y=hi t=true
```

`examples/printf_pct_probe.goo`:
```go
package main
import "fmt"
func main() {
	fmt.Printf("100%% done\n")
}
```
`examples/printf_pct_probe.expected.txt`:
```
100% done
```

`examples/printf_v_probe.goo`:
```go
package main
import "fmt"
func main() {
	fmt.Printf("%v %v %v\n", 7, "ok", false)
}
```
`examples/printf_v_probe.expected.txt`:
```
7 ok false
```

- [ ] **Step 2: Verify they fail**

Run: `./bin/goo -o /tmp/pf examples/printf_probe.goo`
Expected: `Package 'fmt' has no member 'Printf'`.

- [ ] **Step 3: Register Printf in the typechecker.** In `src/types/expression_checker.c` `stdlib_package_lookup`, next to the `fmt`/`Println` entry (~line 1125), add:

```c
    if (strcmp(package, "fmt") == 0 && strcmp(name, "Printf") == 0) {
        return type_function(NULL, 0, void_t);
    }
```

- [ ] **Step 4: Implement the format walker + Printf codegen** in `src/codegen/call_codegen.c`. Read `codegen_generate_println_call` (lines 897-1000) for the exact per-type print dispatch (string→`goo_print_string`, int kinds→`goo_print_int` with SExt, bool→`goo_print_bool`, float→`goo_print_float`). Write `fmt_emit_segments` per the Interfaces contract:
  - Require the first arg (`call->args`) to be a string literal (`AST_LITERAL` with string literal_type). If not, `codegen_error(... "Printf/Sprintf format must be a string literal")` and return 0. Extract its C-string text.
  - Walk the text. On a run of non-`%` chars, in Printf mode emit `goo_print(<that-literal-as-a-global-string>)` (use the same string-constant emission the codegen already uses for string literals — find how `codegen_generate_literal` builds a goo_string global and reuse it; for Printf you can call `goo_print` with the raw cstring chunk if a cstring-printing helper exists, else build a goo_string and call `goo_print_string`).
  - On `%`, read the next char: `%` → emit a literal `%`; `d`/`s`/`f`/`t` → take the next arg (after the format arg), type-check-generate it, and emit the matching `goo_print_*`; `v` → dispatch on the arg's `goo_type->kind` exactly like Println does. Any other char → `codegen_error("unknown format verb %%%c", c)` return 0.
  - Track an arg cursor; if a verb needs an arg but none remain, or args remain after the walk, `codegen_error` (count mismatch) return 0.
  - `codegen_generate_printf_call` calls `fmt_emit_segments(..., sprintf_mode=0, NULL)` and returns a void ValueInfo.
- Route it: where `codegen_generate_println_call` is dispatched (`call_codegen.c:176` and `:305`), add the `Printf` selector branch calling `codegen_generate_printf_call`.

- [ ] **Step 4b: Build + run the probes**

Run: `make bin/goo && for p in printf_probe printf_pct_probe printf_v_probe; do ./bin/goo -o /tmp/$p examples/$p.goo && /tmp/$p; done`
Expected: `x=42 y=hi t=true`, `100% done`, `7 ok false`.

- [ ] **Step 5: Full gate + commit**

```bash
eval "$(opam env --switch=default)" && make verify && make test
git add src/types/expression_checker.c src/codegen/call_codegen.c examples/printf_probe.* examples/printf_pct_probe.* examples/printf_v_probe.*
git commit --no-gpg-sign -m "feat(fmt): Printf with compile-time format walker (%d %s %f %t %v %%)"
```

---

### Task 3: fmt.Sprintf

**Files:**
- Modify: `src/types/expression_checker.c` (register `fmt.Sprintf` → string)
- Modify: `src/codegen/call_codegen.c` (`codegen_generate_sprintf_call` + the sprintf branch of `fmt_emit_segments`; route at :176/:305)
- Test: `examples/sprintf_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: `fmt_emit_segments` (Task 2); `goo_string_concat`, `goo_int_to_string`/`goo_float_to_string`/`goo_bool_to_string` (Task 1).

- [ ] **Step 1: Write the failing probe** — `examples/sprintf_probe.goo`:

```go
package main
import "fmt"
func main() {
	s := fmt.Sprintf("[%d:%s:%t]", 7, "ok", true)
	fmt.Println(s)
}
```
`examples/sprintf_probe.expected.txt`:
```
[7:ok:true]
```

- [ ] **Step 2: Verify failure**

Run: `./bin/goo -o /tmp/sp examples/sprintf_probe.goo`
Expected: `Package 'fmt' has no member 'Sprintf'`.

- [ ] **Step 3: Register Sprintf (returns string).** In `stdlib_package_lookup`, add (string builtin is fetched the same way `void_t` is — read how the strings.* entries get their string return type, ~line 1175):

```c
    if (strcmp(package, "fmt") == 0 && strcmp(name, "Sprintf") == 0) {
        return type_function(NULL, 0, string_t);  // string_t = the string builtin, as used by strings.* entries
    }
```

- [ ] **Step 4: Implement the sprintf branch.** In `fmt_emit_segments`, the `sprintf_mode==1` path: accumulate a `goo_string_t` result. Start from an empty/first literal goo_string; for each literal chunk `goo_string_concat(acc, <chunk-goo_string>)`; for each verb, convert the arg to a goo_string (`%s` → the arg directly; `%d`/int → `goo_int_to_string`; `%f` → `goo_float_to_string`; `%t` → `goo_bool_to_string`; `%v` → dispatch on kind) and `goo_string_concat(acc, conv)`. Write the final accumulator to `*out_str`. `codegen_generate_sprintf_call` calls `fmt_emit_segments(..., sprintf_mode=1, &result)` and returns a ValueInfo of string type holding `result`. Route the `Sprintf` selector at :176/:305.

- [ ] **Step 5: Build + run**

Run: `make bin/goo && ./bin/goo -o /tmp/sp examples/sprintf_probe.goo && /tmp/sp`
Expected: `[7:ok:true]`.

- [ ] **Step 6: Full gate + commit**

```bash
eval "$(opam env --switch=default)" && make verify && make test
git add src/types/expression_checker.c src/codegen/call_codegen.c examples/sprintf_probe.*
git commit --no-gpg-sign -m "feat(fmt): Sprintf builds string via format walker + to_string helpers"
```

---

### Task 4: Capstone — flagship Printf + non-literal rejection

**Files:**
- Create: `examples/printf_hello_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: Tasks 2-3.

- [ ] **Step 1: Flagship probe** — `examples/printf_hello_probe.goo`:

```go
package main
import "fmt"
func main() {
	name := "world"
	n := 3
	fmt.Printf("hello %s, you have %d messages\n", name, n)
	line := fmt.Sprintf("sum=%d", 1+2+3)
	fmt.Println(line)
}
```
`examples/printf_hello_probe.expected.txt`:
```
hello world, you have 3 messages
sum=6
```

- [ ] **Step 2: Run it**

Run: `./bin/goo -o /tmp/ph examples/printf_hello_probe.goo && /tmp/ph`
Expected: the two lines above.

- [ ] **Step 3: Verify non-literal format rejection (manual; golden can't test compile errors)**

Write `/tmp/nl.goo`: `package main` / `import "fmt"` / `func main(){ f := "%d\n"; fmt.Printf(f, 1) }`. Run `./bin/goo -o /tmp/nl /tmp/nl.goo` → expect a clean `format must be a string literal` error, no binary. Report the exact message.

- [ ] **Step 4: Full gate + commit**

```bash
eval "$(opam env --switch=default)" && make verify && make test
git add examples/printf_hello_probe.*
git commit --no-gpg-sign -m "test(golden): flagship Printf/Sprintf capstone probe"
```

- [ ] **Step 5: Update memory** — append to `goolang-v1-roadmap`: fmt.Printf/Sprintf shipped (compile-time walker, %d/%s/%f/%t/%v/%%); goo_int/float/bool_to_string runtime helpers added (reusable for strconv); next stdlib gaps = strconv (Itoa/Atoi), errors.New.

---

## Self-Review

- **Spec coverage:** runtime helpers → Task 1; Printf (typecheck+walker+codegen+routing) → Task 2; Sprintf → Task 3; verbs %d/%s/%f/%t/%v/%% across Tasks 2-3 probes; non-literal & mismatch errors → Task 2 walker + Task 4 Step 3; capstone → Task 4. Width/precision out of scope (spec + plan agree). ✓
- **Placeholder scan:** runtime helper code is complete; the format walker is specified by contract + per-verb behavior + the exact Println dispatch to mirror (the one genuine "read the existing pattern" is the string-literal-global emission, pointed at `codegen_generate_literal`) — bounded, not a placeholder. ✓
- **Type consistency:** `fmt_emit_segments` signature, `goo_int_to_string`/`goo_float_to_string`/`goo_bool_to_string`, `goo_string_concat`, `goo_print_*`, `type_function(NULL,0,…)` used consistently across tasks. ✓
- **Ordering:** Task 1 (runtime, needed by Task 3) and Task 2 (Printf, defines the walker) → Task 3 (Sprintf, needs both) → Task 4 (capstone). ✓
