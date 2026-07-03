# Decl-Surface Breadth Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the approved decl-surface breadth design (docs/superpowers/specs/2026-07-03-decl-surface-breadth-design.md): `var a, b int` multi-name declarations, variadic `...T` parameters (pack-only), and range-over-map, plus a stretch task for buffered `make(chan T, n)`.

**Architecture:** Grammar risk ascends task by task (T1 smallest grammar delta, T2 largest, T3 zero grammar). T1 reuses `VarDeclNode.names[]` multi-name infra end to end. T2 is Go's slice-sugar model: `is_variadic` (already on FunctionType) wired to real signatures, body sees `[]T`, call sites pack trailing args via the existing slice-construction path. T3 adds a cursor iterator over the map runtime's linked list (head-insertion ⇒ deterministic reverse-insertion order — documented deviation from Go's randomization) + a map arm in the for-range checker and codegen.

**Tech Stack:** C23, LLVM-C, Bison (grammar tasks T1/T2/T4). Parser + checker + codegen + runtime (T3 only).

## Global Constraints

- Branch: `feat/decl-surface-breadth` (already created; spec committed at 44a77bf). Do NOT commit on main.
- Commits: conventional, imperative, `--no-gpg-sign`. Stage only named files; never stage `.superpowers/` or `.handoff.md`.
- **Bison discipline (binding, per spec):** baseline 79 shift/reduce + 256 reduce/reduce. After every parser.y edit: `make clean && make lexer`, record the exact conflict counts. A delta is acceptable ONLY with (a) a written justification naming the conflict family and (b) differential parse verification — the full golden suite parsing identically is the minimum bar (`make verify` green covers it since goldens compile). A delta you cannot explain = STOP/BLOCKED.
- Gate per task: `make lexer` (with clean, per above), probes, then `eval "$(opam env --switch=default)"`, `make verify` (ALL PASS; golden 188/0 grows per probe; all 24 reject probes stay green) and `make test` (76/1). STOP/BLOCKED on any regression.
- Go conformance: `go run`-verify every golden probe line; rejections match Go's accept/reject decision (repo wording). Record comparisons.
- Probe hygiene: bool-compare floats, exactly-representable values, same-width prints; map-iteration assertions must be ORDER-INDEPENDENT (sum/count/membership).
- Reject-probe pattern: mirror `cascade-reject-probe`/`consttrunc-reject-probe` (rm -f, rc≠0, no binary, anti-crash grep incl. Segmentation, diagnostic grep), wired into `verify`.
- Pre-commit hook runs `make test`.

## Reference: verified code landmarks (2026-07-03, branch @44a77bf)

- `TOKEN_ELLIPSIS` lexed at `src/lexer/lexer.c:418` ("..."); parser declares `%token ELLIPSIS` (parser.y:126) but NO production uses it yet. VERIFY lexer_bridge.c maps TOKEN_ELLIPSIS → ELLIPSIS before relying on it (grep `ELLIPSIS` there; add the case if missing — that is bridge code, not grammar).
- `func_param` grammar: parser.y:483-505 — `identifier type | type` (anonymous). Params are VarDeclNodes (names[0] + type).
- `var_decl` grammar: parser.y:569-613 — four single-identifier productions (`VAR identifier type`, `VAR identifier ASSIGN expression`, `VAR identifier type ASSIGN expression`, ownership-qualified). `short_var_decl` (parser.y:616-654) shows the repo convention for multi-name: EXPLICIT two-name productions (`identifier COMMA identifier SHORT_ASSIGN ...`), not a list nonterminal.
- `expression_list` nonterminal exists (parser.y:160, used by return/call arms).
- `VarDeclNode`: `names[]`/`name_count` (include/ast.h ~:281); `type_check_var_decl` already loops names (type_checker.c:800+, `bind_var_decl_name` helper from #104); codegen var-decl loops names too (function_codegen.c:888+).
- FunctionType variadic flag: `Type.data.function.is_variadic` — set for panic (type_checker.c:245); println/print are variadic via NULL param list. Call checking: expression_checker.c ~:2172 region (param_types/param_count arity+compat loop).
- Slice construction: composite_codegen.c slice-literal path (global tables ~:1087/:1126; LOCAL slice literals have their own arm — find `slice_lit`); slice ABI: `goo_slice_t` 3 fields, >16 bytes ⇒ BY POINTER across codegen↔C (m12 ABI rule — memory: goolang-slice-abi-by-pointer).
- for-range: checker rejection at `src/types/type_checker.c:1575` ("for-range supported only on slice/array/string types" — the map arm goes beside the slice/array/string arms above it); codegen range dispatch at `src/codegen/statement_codegen.c:628-660` (keys on `range_val->goo_type->kind`: TYPE_SLICE arm, string arm; the map arm is a new sibling).
- Map runtime: `GooMapSV { GooMapEntrySV* head }` (include/runtime.h:120), `GooMapEntrySV { const char* key; int64_t value; next }` linked list, HEAD insertion (runtime.c:507-535). Iterator = cursor over entries. Value slot is int64_t; codegen casts per declared V (existing get/set convention).
- `make(...)` grammar: type-in-call-arg productions parser.y:1472-1519 — accepts `map_type`/`slice_type` only; `make([]T, n)` second arg via the existing expression tail. Chan: `make_chan` builtin exists; `make(chan T)` unbuffered works today; `make(chan T, 1)` = parse error (probed 2026-07-03).
- Repro (2026-07-03): `func sum(nums ...int)` parse error at the `...`; `var a, b int` parse error at the comma; map range = clean checker rejection; `make(chan int, 1)` parse error.

---

### Task 1: `var a, b int` multi-name declarations

**Files:**
- Modify: `src/parser/parser.y` (var_decl productions)
- Modify: `src/types/type_checker.c` ONLY if the multi-name+initializer arity check needs it (read first)
- Test: `examples/multivar_probe.goo` + `.expected.txt`; `examples/multivar_reject.goo` + Makefile `multivar-reject-probe`
- Modify: `Makefile`

**Interfaces:**
- Produces: `VAR identifier COMMA identifier type` (and 3-name form) parse into one VarDeclNode with name_count 2/3 and no values — the existing checker/codegen multi-name loops take it from there. T2 does not depend on this.

- [ ] **Step 1: Probe** (`examples/multivar_probe.goo`):
```go
package main

import "fmt"

type P struct {
	X int
	Y int
}

func main() {
	var a, b int
	a = 1
	b = 2
	fmt.Println(a + b)
	var s, t string
	s = "go"
	t = "o"
	fmt.Println(s + t)
	var p, q P
	p.X = 3
	q.Y = 4
	fmt.Println(p.X + q.Y)
	var x, y, z int
	x = 1
	y = 2
	z = 3
	fmt.Println(x + y + z)
}
```
`.expected.txt`: `3` `goo` `7` `6`. `go run`-verify all lines.
- [ ] **Step 2: Reject probe** (`examples/multivar_reject.goo`): `var a, b int = 1` — Go rejects (`assignment mismatch: 2 variables but 1 value`). NOTE: this shape only exists if Step 4 adds the initializer form; if Step 4 ships the no-initializer form only, the reject probe instead asserts `var a, b int = 1` stays a PARSE error (rc≠0, no crash) — either way the program must not compile. Makefile `multivar-reject-probe` mirroring the established pattern (grep: `mismatch` if checker-rejected, or the parse-error text if grammar-rejected — pick after Step 4 and document in the target comment).
- [ ] **Step 3: Verify today** — both programs parse-error at the comma (RED; record exact positions).
- [ ] **Step 4: Grammar** — follow the short_var_decl convention (explicit productions, NOT a general list nonterminal — the repo precedent, and it bounds conflict risk):
```
    | VAR identifier COMMA identifier type {
        VarDeclNode* var = ast_var_decl_new(get_current_position());
        IdentifierNode* i1 = (IdentifierNode*)$2;
        IdentifierNode* i2 = (IdentifierNode*)$4;
        var->names = malloc(sizeof(char*) * 2);
        var->names[0] = strdup(i1->name);
        var->names[1] = strdup(i2->name);
        var->name_count = 2;
        var->type = $5;
        ast_node_free($2);
        ast_node_free($4);
        $$ = (ASTNode*)var;
    }
    | VAR identifier COMMA identifier COMMA identifier type { /* 3-name analogue, name_count = 3 */ }
```
(Write the 3-name action in full — same shape, three strdups.) The initializer-list form (`var a, b int = 1, 2`) is IN scope ONLY if it composes from existing pieces (e.g. reusing the F6 `multi_assign_2_new` machinery `... ASSIGN expression COMMA expression`); investigate, implement if clean, otherwise record as follow-up — the spec's minimum bar is the no-initializer form.
- [ ] **Step 5: Bison guard** — `make clean && make lexer`; record conflict counts; expect 79/256 unchanged (the VAR prefix disambiguates; a delta needs justification + differential verification per Global Constraints).
- [ ] **Step 6: Checker/codegen verification** — the multi-name no-values path: `type_check_var_decl` loops names with `final_type = declared_type` (each gets int, zero-init); codegen's var-decl loop allocas each. Probe proves it; if either end assumes name_count==1 somewhere on the no-values path, fix minimally and record.
- [ ] **Step 7: Gate** — probe passes; golden 189/0; reject probe (25 total); test 76/1.
- [ ] **Step 8: Commit** — "feat(parser): multi-name var declarations (var a, b int)".

---

### Task 2: Variadic `...T` parameters (pack-only)

**Files:**
- Modify: `src/parser/parser.y` (func_param production), `src/parser/lexer_bridge.c` (ELLIPSIS mapping if missing)
- Modify: `src/types/type_checker.c` (signature building: is_variadic + last-param []T), `src/types/expression_checker.c` (call-site arity/compat for variadic callees)
- Modify: `src/codegen/call_codegen.c` (call-site packing), possibly `src/codegen/function_codegen.c` (param binding — the body already sees a []T param if the TYPE is set right; verify)
- Test: `examples/variadic_probe.goo` + `.expected.txt`; `examples/variadic_reject.goo` + `examples/variadic_range_reject.goo` + Makefile targets
- Modify: `Makefile`

**Interfaces:**
- Consumes: nothing from T1.
- Produces: `FunctionType.is_variadic=1` with the LAST entry of param_types being `TYPE_SLICE of T`; call sites with `is_variadic` callees pack trailing args. T3 does not depend on this.

- [ ] **Step 1: Probe** (`examples/variadic_probe.goo`):
```go
package main

import "fmt"

func sum(nums ...int) int {
	t := 0
	for _, n := range nums {
		t += n
	}
	return t
}

func join(prefix string, nums ...int) int {
	if prefix == "p" {
		return sum(nums[0], nums[1])
	}
	return 0
}

func small(bs ...int8) int {
	t := 0
	for _, b := range bs {
		t += int(b)
	}
	return t
}

func main() {
	fmt.Println(sum())
	fmt.Println(sum(41))
	fmt.Println(sum(1, 2, 3))
	fmt.Println(join("p", 4, 5))
	fmt.Println(small(100, 27))
	r := sum(1, 2) + sum(3)
	fmt.Println(r)
}
```
`.expected.txt`: `0` `41` `6` `9` `127` `6`. `go run`-verify. Covers: zero/one/many args, fixed+variadic mix, indexing the variadic slice in the body, narrow element type (int8 — pack-site range-check interplay), results composing.
- [ ] **Step 2: Reject probes** — `examples/variadic_reject.goo`: `func f(a ...int, b int) int` (non-final variadic; Go: `can only use ... with final parameter`) → `variadic-reject-probe`, grep on the new diagnostic (repo wording, e.g. `variadic parameter must be the final parameter`). `examples/variadic_range_reject.goo`: `small(300)` with `func small(bs ...int8)` → `variadic-range-reject-probe`, grep `overflows int8` (the #102 net must fire at the pack site).
- [ ] **Step 3: Verify today** — all parse-error at `...` (RED; record).
- [ ] **Step 4: Bridge + grammar** — lexer_bridge: ensure `case TOKEN_ELLIPSIS: return ELLIPSIS;` exists (add beside the other punctuation cases if missing). Grammar: add to func_param:
```
    | identifier ELLIPSIS type {
        IdentifierNode* ident = (IdentifierNode*)$1;
        VarDeclNode* param = ast_var_decl_new(get_current_position());
        param->names = malloc(sizeof(char*));
        param->names[0] = strdup(ident->name);
        param->name_count = 1;
        param->type = $3;          /* element type T; variadic flag carried on the node */
        param->is_variadic_param = 1;
        param->values = NULL;
        ast_node_free($1);
        $$ = (ASTNode*)param;
    }
```
`is_variadic_param` is a NEW int field on VarDeclNode — **append it at the TAIL of the struct** (include/ast.h — the no-header-deps rule: `make clean` after, and appending avoids shifting offsets in stale objects; see the Makefile-no-header-deps memory convention baked into ast.h's own comment at :123-133). Anonymous variadic (`ELLIPSIS type`) — add only if the anonymous param production's users need it; Go allows it; include for symmetry if free, skip and record if it conflicts.
- [ ] **Step 5: Bison guard** — `make clean && make lexer`; conflict counts recorded; ELLIPSIS between identifier and type should be conflict-free (fresh token in that position); any delta per Global Constraints.
- [ ] **Step 6: Checker (signature)** — where function declarations build FunctionType from params (find it in type_checker.c — the func_decl arm): a param with `is_variadic_param` must be LAST (else the clean rejection from Step 2's probe, checked at declaration time); set `fn_type->data.function.is_variadic = 1` and record the param's type as `TYPE_SLICE of T` (use the existing slice-type constructor) so the BODY sees `nums` as []int through the ordinary param-binding path (verify function_codegen's param binding reads the declared type — if yes, zero codegen change for the body side).
- [ ] **Step 7: Checker (call site)** — in the call-arg loop (expression_checker.c ~:2172): when callee `is_variadic` (and param_types non-NULL — do NOT disturb the println/print/panic NULL-param special case), arity check becomes `argc >= param_count - 1`; fixed args check as today; each trailing arg checks/adapts against the ELEMENT type T (the existing per-arg adaptation call with T as the target — this is what makes the int8 range-check fire).
- [ ] **Step 8: Codegen (pack)** — in call_codegen.c where user-call args are emitted: for a variadic callee, emit fixed args as today, then build a slice from the trailing args and pass it as the final parameter. Reuse the LOCAL slice-literal construction path (find the arm composite_codegen uses for `[]int{a, b}` locals — alloca backing array, store elems, build goo_slice_t {ptr,len,cap}, pass BY POINTER per the ABI rule). Zero trailing args → {NULL,0,0} slice. Element stores use the existing per-elem coercion (`slice_coerce_elem`-style, srcsignedness-aware — #97).
- [ ] **Step 9: Gate** — probe + both reject probes pass; golden 190/0 (fmt.Println/print/panic goldens = the special-case guard); 26+ reject probes; test 76/1; ccomp-link PASS.
- [ ] **Step 10: Commit** — "feat(parser,types,codegen): variadic ...T parameters (pack-only)".

---

### Task 3: Range-over-map

**Files:**
- Modify: `src/runtime/runtime.c` + `include/runtime.h` (iterator — header change: `make clean`)
- Modify: `src/types/type_checker.c:1575` region (map arm in for-range check)
- Modify: `src/codegen/statement_codegen.c:628+` (map arm in range codegen)
- Test: `examples/maprange_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: nothing from T1/T2.
- Produces: `int goo_map_iter_next_sv(GooMapEntrySV** cursor, const char** key_out, int64_t* val_out)` — advances the cursor; returns 1 with outs filled while entries remain, 0 at end. Init = `cursor = (GooMapEntrySV*)m->head`.

- [ ] **Step 1: Probe** (`examples/maprange_probe.goo`):
```go
package main

import "fmt"

func main() {
	m := map[string]int{"a": 1, "b": 2, "c": 3}
	t := 0
	for _, v := range m {
		t += v
	}
	fmt.Println(t)
	n := 0
	for k := range m {
		if k == "a" || k == "b" || k == "c" {
			n = n + 1
		}
	}
	fmt.Println(n)
	kv := 0
	for k, v := range m {
		if k == "b" {
			kv = v
		}
	}
	fmt.Println(kv)
	e := map[string]int{}
	c := 0
	for _, v := range e {
		c += v
	}
	fmt.Println(c)
}
```
`.expected.txt`: `6` `3` `2` `0`. ALL assertions order-independent (sum/count/lookup). `go run`-verify. Check `map[string]int{}` empty-literal parses today — if not, build the empty case via `make(map[string]int)` instead and record.
- [ ] **Step 2: Verify today** — clean checker rejection (RED; record the message).
- [ ] **Step 3: Runtime iterator** — in runtime.c beside the other map functions, declared in runtime.h after `goo_map_len_sv`:
```c
// Map iteration: cursor-based walk of the entry list. Init the cursor to
// m->head (NULL map ⇒ NULL cursor ⇒ immediate end). Iteration order is the
// list order — HEAD insertion makes that REVERSE INSERTION ORDER, which is
// DETERMINISTIC. Documented deviation: Go deliberately randomizes map
// iteration order; Goo does not (recorded in the decl-surface-breadth
// spec). Entries deleted mid-iteration: unlinking a not-yet-visited entry
// skips it (Go-consistent); deleting the CURRENT entry frees the node the
// cursor points through — callers of the codegen'd loop cannot do that
// today (no delete inside range bodies in generated code paths), recorded
// as a limitation, not defended.
int goo_map_iter_next_sv(GooMapEntrySV** cursor, const char** key_out, int64_t* val_out);
```
(Implementation: if `!cursor || !*cursor` return 0; fill outs from `(*cursor)->key/value`; advance `*cursor = (*cursor)->next`; return 1. Note GooMapEntrySV is currently defined in runtime.c only — either move the struct definition to runtime.h or, better, type the cursor as `void**` in the header to keep the entry layout private; pick one, comment why.)
- [ ] **Step 4: Checker arm** — at type_checker.c:1575 region, beside the slice/array/string arms: TYPE_MAP range binds key var to the map's KEY type and value var to the VALUE type (read both from the map Type's data — do not hardcode string, even though the runtime is string-keyed today; the Type carries it).
- [ ] **Step 5: Codegen arm** — statement_codegen.c range dispatch: TYPE_MAP sibling arm. Loop skeleton mirrors the slice arm: alloca a cursor slot, store `m->head` (a GEP/load from the map pointer — mind that maps are POINTERS (GooMapSV*) in codegen), cond block calls `goo_map_iter_next_sv(cursor_slot, &k, &v)` (declare the runtime fn like the other goo_map externs), test the i1/i32 result, bind k (string) / v (cast the int64 slot to the declared value type — the existing map-get cast convention) into the loop vars, body, back-edge. `_` key/value skipped bindings per the existing slice-arm convention.
- [ ] **Step 6: Gate** — probe passes; golden 191/0; test 76/1; ccomp-link PASS (runtime change crosses the C boundary — the ABI memory says >16-byte structs by pointer; the iterator passes pointers only, fine, but run ccomp).
- [ ] **Step 7: Commit** — "feat(runtime,types,codegen): range over maps (deterministic order, documented deviation)".

---

### Task 4 (STRETCH — skip if T1/T2 conflict budget was not clean): buffered `make(chan T, n)`

**Files:**
- Modify: `src/parser/parser.y` (chan_type joins the make type-arg production, parser.y:1517-1519 region)
- Modify: `src/types/type_checker.c` / `src/codegen/call_codegen.c` (capacity arg plumbed to the existing make_chan/buffered-channel runtime — READ how `make(chan T)` unbuffered routes today and follow it)
- Test: `examples/makechan_probe.goo` + `.expected.txt`

- [ ] **Step 1: Probe**:
```go
package main

import "fmt"

func main() {
	ch := make(chan int, 2)
	ch <- 1
	ch <- 2
	a := <-ch
	b := <-ch
	fmt.Println(a + b)
}
```
`.expected.txt`: `3`. (Buffered cap 2 ⇒ two non-blocking sends, no goroutine needed.) `go run`-verify.
- [ ] **Step 2: Verify today** — parse error at the comma (RED).
- [ ] **Step 3: Grammar** — add `chan_type` (or the existing channel type nonterminal — find its name) to the make type-arg production alongside map_type/slice_type. Bison guard per Global Constraints.
- [ ] **Step 4: Plumb capacity** — the second expression arg reaches the make checker/codegen arm the same way `make([]T, n)`'s does; route it to the buffered-channel runtime constructor (the runtime has buffered channels — unbuffered/buffered goldens exist; find the constructor `make_chan`/`goo_chan_new` signature and pass cap).
- [ ] **Step 5: Gate** — probe passes; golden 192/0; chan goldens green; test 76/1.
- [ ] **Step 6: Commit** — "feat(parser,codegen): buffered make(chan T, n)".

---

## Final gate

`make verify` → ALL GREEN (191/0 or 192/0 with stretch; 26+ reject probes). `make test` → 76/1. `make ccomp-link` → PASS. Bison conflict counts recorded per grammar task in the reports.

## Self-review notes

- T1 follows the short_var_decl explicit-production precedent instead of a list nonterminal — bounded conflict risk over generality; `var a, b, c, d int` (4+) is out of scope, recorded.
- T2's new AST field appends at the struct tail per the repo's baked-in enum/struct append rule; the println/print/panic NULL-param special case is explicitly fenced off in Step 7.
- T3 types the public cursor as opaque (or moves the struct) — decided by the implementer with a comment; the mutation-during-iteration limitation is documented, not defended (YAGNI).
- Out of scope (recorded): spread calls `f(s...)`/`append(a, b...)`; 4+-name var decls; `var a, b = 1, 2` inferred multi-init unless T1 finds it free; non-string map keys; randomized iteration order; closures (next cycle).
