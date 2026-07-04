# Stdlib Unblockers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close four stdlib-blocking idiom gaps — trailing commas in struct/map literals, `[]byte`↔`string` conversions, spread `f(s...)`, and the `copy` builtin — gated by verbatim Go 1.9 `strings.Join` running go-identically.

**Architecture:** Parser-first batch on the existing LALR grammar + typecheck/codegen/runtime stack. Spread rides #105's variadic pack ABI (one packed-slice arg); `copy` and append-spread share a memmove runtime core; conversions get a new tail-of-enum AST node. Spec: `docs/superpowers/specs/2026-07-04-stdlib-unblockers-design.md`.

**Tech Stack:** C23, bison/flex, LLVM-C API, own C runtime (`src/runtime/runtime.c`).

## Global Constraints

- Branch: `feat/stdlib-unblockers` (exists, base main @ e4a3419). Commit with `--no-gpg-sign`. Conventional commits, imperative mood.
- Bison tripwire: `bison -d -o /tmp/p.tab.c src/parser/parser.y 2>&1 | grep conflicts` must print EXACTLY `81 shift/reduce` and `256 reduce/reduce` after every parser change. ANY delta → STOP the task, report with the conflict shape diff (`bison -Wcounterexamples`), do not proceed.
- `include/ast.h` changes: append new enum values / struct fields at the TAIL only, then `make clean && make lexer` — the Makefile has no header deps; stale objects silently miscompile.
- Baselines at branch start: golden 215/0 (`bash scripts/run_golden.sh`), `make test` = 76 passed/1 skipped, `make verify` green. Golden count only ever grows.
- Exit codes: never read a pipeline's `$?` for a gate. Run the command bare, or capture with `; echo EXIT=$?`.
- Runtime ABI rule: `goo_slice_t` (3 fields, >16 bytes) never crosses codegen↔C by value — helpers take/return bare data pointers + explicit lengths.
- Every golden's expected output is verified with `go run` on an equivalent `.go` file before commit (write the `.go`, run it, paste output into `.expected.txt`).
- Each task ends: `make test` green, golden suite green, bison tripwire clean, commit. Pre-commit hook runs `make test` automatically.

---

### Task 1: Trailing commas in `struct_lit` and `map_lit`

**Files:**
- Modify: `src/parser/parser.y` (map_lit ~2015, struct_lit ~2050, forward decls ~49)
- Create: `examples/trailing_comma_probe.goo`, `examples/trailing_comma_probe.expected.txt`
- Modify: `Makefile` (new `trailingcomma-reject-probe`, add to `verify:` list at line ~1755)

**Interfaces:**
- Consumes: existing `struct_literal_new(char* type_name_owned, ASTNode* inits)` (parser.y:49/3237).
- Produces: `static ASTNode* map_literal_new(ASTNode* map_type, ASTNode* entries)` in parser.y — Task 5's probe relies on both literal forms accepting trailing commas.

Context for a fresh engineer: slice/array/elided-struct literals ALREADY accept trailing commas (arms at parser.y:2167, 2208, 2240). Only the two productions below lack the arm. `{,}` must stay rejected.

- [ ] **Step 1: Write the failing golden**

`examples/trailing_comma_probe.goo`:

```go
package main

import "fmt"

type Pt struct {
	X int
	Y int
}

func main() {
	p := Pt{
		X: 1,
		Y: 2,
	}
	q := Pt{X: 3,}
	m := map[string]int{
		"a": 1,
		"b": 2,
	}
	n := map[string]int{"c": 7,}
	r := &Pt{
		X: 9,
	}
	fmt.Println(p.X + p.Y)
	fmt.Println(q.X)
	fmt.Println(m["b"])
	fmt.Println(n["c"])
	fmt.Println(r.X)
}
```

`examples/trailing_comma_probe.expected.txt` (go-run-verified — write the same program as `/tmp/tc.go` with package main, run `go run /tmp/tc.go`, confirm):

```
3
3
2
7
9
```

- [ ] **Step 2: Verify it fails today**

Run: `bin/goo -o build/tc examples/trailing_comma_probe.goo`
Expected: `Parse error ... syntax error` (at the first `X: 1,` line's closing brace region). If it PASSES, stop — baseline assumption broken, report.

- [ ] **Step 3: Factor `map_literal_new`, add both COMMA arms**

In parser.y, next to the `struct_literal_new` forward decl (~line 49) add:

```c
/* Shared by the map_lit arms (with and without trailing comma).
   Extracts the parallel values list that map_entry_list stashes on the
   keys head's node_type side-channel (cleared so type_check/codegen
   never see it). */
static ASTNode* map_literal_new(ASTNode* map_type_node, ASTNode* entries);
```

At the bottom of parser.y (next to `struct_literal_new`'s definition ~3237):

```c
static ASTNode* map_literal_new(ASTNode* map_type_node, ASTNode* entries) {
    MapLitNode* lit = (MapLitNode*)malloc(sizeof(MapLitNode));
    lit->base.type = AST_PAREN_EXPR;
    lit->base.pos = get_current_position();
    lit->base.node_type = NULL;
    lit->base.next = NULL;
    lit->map_type = map_type_node;
    if (entries) {
        lit->keys = entries;
        lit->values = (ASTNode*)((ASTNode*)entries)->node_type;
        ((ASTNode*)entries)->node_type = NULL;
    } else {
        lit->keys = NULL;
        lit->values = NULL;
    }
    return (ASTNode*)lit;
}
```

Replace the two existing `map_lit` arm bodies with calls to the helper, and add the trailing-comma arms so the production reads:

```yacc
map_lit:
    map_type LBRACE map_entry_list RBRACE        { $$ = map_literal_new($1, $3); }
    | map_type LBRACE map_entry_list COMMA RBRACE { $$ = map_literal_new($1, $3); }
    | map_type LBRACE RBRACE                      { $$ = map_literal_new($1, NULL); }
    ;
```

Add one arm to `struct_lit` (identical action to the first arm):

```yacc
    | identifier LBRACE struct_lit_inits COMMA RBRACE {
        IdentifierNode* type_ident = (IdentifierNode*)$1;
        $$ = struct_literal_new(strdup(type_ident->name), $3);
        ast_node_free($1);
    }
```

- [ ] **Step 4: Bison tripwire, rebuild, golden passes**

Run: `bison -d -o /tmp/p.tab.c src/parser/parser.y 2>&1 | grep conflicts`
Expected: `81 shift/reduce`, `256 reduce/reduce` EXACTLY (stop rule otherwise).
Run: `make lexer` (exit 0), then `bin/goo -o build/tc examples/trailing_comma_probe.goo && ./build/tc`
Expected: the five lines from Step 1. Then `bash scripts/run_golden.sh` → `216 passed, 0 failed`.

- [ ] **Step 5: Reject probe — `{,}` stays illegal**

Makefile, next to `map-addr-reject-probe` (~line 2155), same shape (compile must FAIL):

```makefile
trailingcomma-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== trailingcomma-reject-probe: bare comma literal must reject ==="
	@printf 'package main\nfunc main(){\n\tm := map[string]int{,}\n\t_ = m\n}\n' > build/tc_reject.goo
	@if $(COMPILER) -o build/tc_reject build/tc_reject.goo 2>build/tc_reject.err; then \
	  echo "trailingcomma-reject-probe: FAIL (bare-comma literal compiled)"; exit 1; \
	else \
	  echo "trailingcomma-reject-probe: PASS (rejected)"; \
	fi
```

Append `trailingcomma-reject-probe` to the `verify:` prerequisite list (line ~1755). Run: `make trailingcomma-reject-probe` → PASS.

- [ ] **Step 6: Full task gate + commit**

Run: `make test` (76/1, exit 0), `bash scripts/run_golden.sh` (216/0).

```bash
git add src/parser/parser.y examples/trailing_comma_probe.* Makefile
git commit --no-gpg-sign -m "feat(parser): trailing commas in struct and map literals"
```

---

### Task 2: `[]byte(s)` / `string(b)` conversions

**Files:**
- Modify: `include/ast.h` (TAIL: `AST_SLICE_CONVERSION` enum value + `SliceConvNode`), `include/runtime.h`, `src/runtime/runtime.c`
- Modify: `src/parser/parser.y` (one primary_expr arm), `src/types/expression_checker.c`, `src/codegen/expression_codegen.c`
- Create: `examples/bytesconv_probe.goo` + `.expected.txt`
- Modify: `Makefile` (`bytesconv-reject-probe` + `verify:`)

**Interfaces:**
- Consumes: `slice_type` grammar nonterminal; string rep = `{i8* ptr, i64 len}` aggregate; slice rep = `{ptr, len, cap}`; `goo_alloc(size_t)`.
- Produces: `void* goo_bytes_from_string(const char* p, int64_t len)` and `char* goo_cstr_from_bytes(void* data, int64_t len)` (runtime.h) — Task 4's string-source arms and Task 5's probe depend on both conversions.

- [ ] **Step 1: Failing golden**

`examples/bytesconv_probe.goo` (go-run-verify the expected output first):

```go
package main

import "fmt"

func main() {
	s := "hello"
	b := []byte(s)
	b[0] = 'H'
	fmt.Println(string(b))
	fmt.Println(s)
	fmt.Println(len([]byte("héllo")))
	e := []byte("")
	fmt.Println(len(e))
	t := string(b)
	b[1] = 'X'
	fmt.Println(t)
}
```

`examples/bytesconv_probe.expected.txt`:

```
Hello
hello
6
0
Hello
```

Run `bin/goo -o build/bc examples/bytesconv_probe.goo` → Expected: `Parse error` at `[]byte(s)`.

- [ ] **Step 2: AST node (tail of ast.h) + `make clean`**

At the TAIL of the node-type enum in `include/ast.h`: `AST_SLICE_CONVERSION,` and at the tail of the node structs:

```c
// []T(expr) conversion form (v1: only []byte(string) admitted — enforced by
// the type checker, not the grammar). slice_type holds the parsed AST_SLICE_TYPE.
typedef struct {
    ASTNode base;
    struct ASTNode* slice_type;
    struct ASTNode* operand;
} SliceConvNode;
```

Then: `make clean && make lexer` (MANDATORY after ast.h edits — no header deps).

- [ ] **Step 3: Grammar arm**

In parser.y, add to `primary_expr`'s alternatives (near the composite-literal arms ~2152):

```yacc
    | slice_type LPAREN expression RPAREN {
        SliceConvNode* conv = (SliceConvNode*)malloc(sizeof(SliceConvNode));
        conv->base.type = AST_SLICE_CONVERSION;
        conv->base.pos = get_current_position();
        conv->base.node_type = NULL;
        conv->base.next = NULL;
        conv->slice_type = $1;
        conv->operand = $3;
        $$ = (ASTNode*)conv;
    }
```

Bison tripwire (81/256 EXACT — this is the batch's riskiest grammar edit; on any delta STOP with `-Wcounterexamples` output).

- [ ] **Step 4: Typecheck**

In `src/types/expression_checker.c`'s `type_check_expression` switch, add a case (locate the AST_INDEX_EXPR case for placement pattern):

```c
        case AST_SLICE_CONVERSION: {
            SliceConvNode* conv = (SliceConvNode*)expr;
            Type* target = type_check_expression(checker, conv->slice_type);
            Type* src = type_check_expression(checker, conv->operand);
            if (!target || !src) return NULL;
            if (target->kind != TYPE_SLICE ||
                target->data.slice.element_type->kind != TYPE_UINT8) {
                type_error(checker, expr->pos,
                    "[]T(x) conversion is only supported for []byte(string) in v1");
                return NULL;
            }
            if (src->kind != TYPE_STRING) {
                type_error(checker, expr->pos,
                    "[]byte(x) requires a string operand, got %s", type_to_string(src));
                return NULL;
            }
            expr->node_type = target;
            return target;
        }
```

(If `byte` maps to a different TypeKind than `TYPE_UINT8` in `include/types.h`, use that kind — check how the lexer/checker resolve the `byte` keyword; do not guess.)

Then extend the existing `string(x)` conversion arm (find it: `grep -n '"string"' src/types/expression_checker.c` — the F2 builtin-conversion block that today accepts rune/byte/int operands): add an accepted-operand arm for `TYPE_SLICE` whose element kind is the byte kind, result `TYPE_STRING`. Follow the surrounding arms' exact style; reject other slices with the existing incompatible-conversion error.

- [ ] **Step 5: Runtime helpers**

`include/runtime.h` (near `goo_slice_alloc`, line ~188):

```c
// []byte(s) / string(b) conversion cores. Copy semantics (Go-exact): the
// result never aliases the source. Bare-pointer ABI (goo_slice_t never
// crosses by value); lengths explicit.
void* goo_bytes_from_string(const char* p, int64_t len);
char* goo_cstr_from_bytes(void* data, int64_t len);
```

`src/runtime/runtime.c` (near `goo_slice_alloc`, ~716):

```c
void* goo_bytes_from_string(const char* p, int64_t len) {
    if (len < 0) len = 0;
    void* data = goo_alloc(len > 0 ? (size_t)len : 1);
    if (len > 0) memcpy(data, p, (size_t)len);
    return data;
}

char* goo_cstr_from_bytes(void* data, int64_t len) {
    if (len < 0) len = 0;
    char* s = (char*)goo_alloc((size_t)len + 1);
    if (len > 0) memcpy(s, data, (size_t)len);
    s[len] = '\0';   // known rep limitation: embedded NULs truncate downstream C-string ops
    return s;
}
```

- [ ] **Step 6: Codegen**

In `src/codegen/expression_codegen.c`'s `codegen_generate_expression` dispatch, add an `AST_SLICE_CONVERSION` case: generate the operand (load if lvalue, standard idiom), `ExtractValue 0/1` for string ptr+len, call `goo_bytes_from_string`, build the slice aggregate `{ptr, len, cap=len}` (`LLVMBuildInsertValue` ×3 into the slice LLVM type from `codegen_type_to_llvm`). For the `string(b)` direction, in the existing string-conversion codegen arm add the slice-operand case: extract data ptr + len from the slice aggregate, call `goo_cstr_from_bytes`, build the string aggregate `{ptr, len}`. Declare both runtime fns where other `goo_*` externs are declared (follow `goo_slice_alloc`'s declaration pattern in codegen).

- [ ] **Step 7: Gates, reject probe, commit**

Golden run → `217 passed, 0 failed`; `./build/bc` output matches Step 1. Makefile `bytesconv-reject-probe` (same reject shape as Task 1 Step 5) with program `\tb := []int("x")\n` → must fail with the v1-scoped message (grep the message in the probe: `grep -q "only supported for" build/bytesconv_reject.err`). Add to `verify:`. Run `make test`.

```bash
git add include/ast.h include/runtime.h src/runtime/runtime.c src/parser/parser.y \
        src/types/expression_checker.c src/codegen/expression_codegen.c \
        examples/bytesconv_probe.* Makefile
git commit --no-gpg-sign -m "feat(conv): []byte(string) and string([]byte) with copy semantics"
```

---

### Task 3: Spread `f(s...)`

**Files:**
- Modify: `include/ast.h` (TAIL field on CallExprNode), `src/parser/parser.y` (call_expr arm ~1650), `src/types/expression_checker.c` (call checking), `src/codegen/call_codegen.c` (variadic pack path, ~1127–1260)
- Create: `examples/spread_probe.goo` + `.expected.txt`
- Modify: `Makefile` (`spread-reject-probe` + `verify:`)

**Interfaces:**
- Consumes: `CallExprNode {function, args}` (ast.h:494-499); variadic pack path (`callee_is_variadic`, `fixed_count`, `llvm_argc` at call_codegen.c:1138-1146).
- Produces: `CallExprNode.has_spread` flag — Task 4's `append(dst, s...)` arm reads the same flag.

- [ ] **Step 1: Failing golden**

`examples/spread_probe.goo` (go-run-verify):

```go
package main

import "fmt"

func sum(xs ...int) int {
	t := 0
	for _, x := range xs {
		t += x
	}
	return t
}

func mut(xs ...int) {
	xs[0] = 99
}

func tagged(tag string, xs ...int) int {
	return len(tag) + sum(xs...)
}

func main() {
	s := []int{1, 2, 3}
	fmt.Println(sum(s...))
	mut(s...)
	fmt.Println(s[0])
	var e []int
	fmt.Println(sum(e...))
	fmt.Println(tagged("ab", s...))
}
```

`.expected.txt`: `6`, `99`, `0`, `106` (verify with go run — `99+2+3=104`, `+2` = `106`). Compile today → Parse error at `s...`.

- [ ] **Step 2: AST flag + grammar arm**

ast.h, TAIL of CallExprNode:

```c
typedef struct {
    ASTNode base;
    struct ASTNode* function;
    struct ASTNode* args;       // Argument list
    int has_spread;             // final arg is `expr...` (Go spread); malloc'd
                                // call sites must zero it — see parser arms
} CallExprNode;
```

AUDIT REQUIRED: `grep -n 'malloc(sizeof(CallExprNode))' src/parser/parser.y` — every existing site must gain `call->has_spread = 0;`. Then `make clean && make lexer`.

parser.y, new `call_expr` alternative next to the `primary_expr LPAREN expression_list RPAREN` arm (~1650), same action body plus the flag (spread is grammatically FINAL-ONLY — Go treats non-final `...` as a syntax error too):

```yacc
    | primary_expr LPAREN expression_list ELLIPSIS RPAREN {
        /* identical construction to the plain-arg arm above */
        ...
        call->args = $3;
        call->has_spread = 1;
        $$ = (ASTNode*)call;
    }
```

(Transcribe the plain arm's malloc/field-init body exactly; only `has_spread` differs.) Bison tripwire: 81/256 EXACT.

- [ ] **Step 3: Typecheck rules**

In the function-call checking path of `expression_checker.c` (where a resolved callee's signature is compared against args — near the variadic acceptance added by #105), when `((CallExprNode*)expr)->has_spread`:

```c
    // Go spread rules: callee variadic; spread is the sole variadic-slot
    // argument (fixed params exactly filled); operand []E with E identical
    // to the variadic element type (no coercion).
    if (!fn_type->data.function.is_variadic) {
        type_error(checker, expr->pos, "spread argument requires a variadic function");
        return NULL;
    }
    if (arg_count != fn_type->data.function.param_count) {
        type_error(checker, expr->pos,
            "spread call must supply exactly the fixed arguments then one slice (want %zu args, got %zu)",
            fn_type->data.function.param_count, arg_count);
        return NULL;
    }
    Type* last_t = /* type of final arg (already checked above) */;
    Type* elem_want = fn_type->data.function.param_types[fn_type->data.function.param_count - 1];
    // elem_want is the variadic slot; unwrap its slice wrapper if the
    // signature stores []E there (check declare_function_signature).
    if (last_t->kind != TYPE_SLICE ||
        !type_identical(last_t->data.slice.element_type, elem_want_elem)) {
        type_error(checker, expr->pos,
            "cannot spread %s into variadic parameter ...%s",
            type_to_string(last_t), type_to_string(elem_want_elem));
        return NULL;
    }
```

(Adapt names to the surrounding block: the #105 code already computes fixed/variadic split — read it first, reuse its locals. `type_identical` = whatever strict-equality helper the file uses; if only `type_compatible` exists, use it with an explicit elem-kind equality check.)

- [ ] **Step 4: Codegen — bypass the pack**

In call_codegen.c's variadic branch (after the fixed-arg loop, where the pack is built from trailing actuals): when `call->has_spread`, generate the final arg expression, load if lvalue, and use its slice value AS the pack arg (`args[llvm_argc - 1]`) in exactly the form the pack builder passes its result (match by-value aggregate vs pointer — read the pack builder's last lines and mirror them). No element copies — Go aliasing semantics (the golden's `mut(s...)` pins it).

- [ ] **Step 5: Gates, reject probe, commit**

Golden 218/0; spread probe output matches. `spread-reject-probe` (reject shape): three programs in one recipe — spread on non-variadic (`func f(a int)`; `f(s...)`), elem mismatch (`[]int32` into `...int64`), missing fixed arg (`tagged(s...)`); each must fail, grep each distinct message. Add to `verify:`; `make test`.

```bash
git add include/ast.h src/parser/parser.y src/types/expression_checker.c \
        src/codegen/call_codegen.c examples/spread_probe.* Makefile
git commit --no-gpg-sign -m "feat(calls): spread f(s...) through the variadic pack ABI"
```

---

### Task 4: `copy` builtin + `append(dst, s...)`

**Files:**
- Modify: `include/runtime.h`, `src/runtime/runtime.c` (memmove core), `src/types/type_checker.c` (~304, builtin registration), `src/types/expression_checker.c` (~2360, append arm + new copy arm), `src/codegen/call_codegen.c` (~590 append lowering + copy lowering)
- Create: `examples/copy_probe.goo` + `.expected.txt`, `examples/append_spread_probe.goo` + `.expected.txt`
- Modify: `Makefile` (`copy-reject-probe` + `verify:`)

**Interfaces:**
- Consumes: `CallExprNode.has_spread` (Task 3); `goo_bytes_from_string` string-byte access convention (Task 2); existing `goo_slice_append` and the append arm's two-arg shape (expression_checker.c:2360-2385: append is TWO-ARG ONLY today — this task adds the spread form, NOT general multi-element append, which stays a recorded non-goal).
- Produces: runtime `int64_t goo_slice_copy_raw(void* dst, int64_t dst_len, const void* src, int64_t src_len, int64_t elem_size)` and `void goo_slice_append_bulk(void* dst_slice /* goo_slice_t* */, const void* src, int64_t src_len, int64_t elem_size)` — Task 5's verbatim Join needs `copy` (both forms).

- [ ] **Step 1: Failing goldens**

`examples/copy_probe.goo` (go-run-verify):

```go
package main

import "fmt"

func main() {
	src := []int{1, 2, 3, 4}
	dst := []int{0, 0}
	n := copy(dst, src)
	fmt.Println(n)
	fmt.Println(dst[0] + dst[1])
	m := copy(src[1:4], src[0:3])
	fmt.Println(m)
	fmt.Println(src[1])
	fmt.Println(src[3])
	b := []byte("??????")
	k := copy(b, "hi")
	fmt.Println(k)
	fmt.Println(string(b))
	var empty []int
	fmt.Println(copy(empty, src))
}
```

`.expected.txt` (paste from `go run`): `2`, `3`, `3`, `1`, `3`, `2`, `hi????`, `0`. Note the overlap case (`copy(src[1:4], src[0:3])`) — Go memmove semantics: src becomes `1 1 2 3`.

`examples/append_spread_probe.goo` (go-run-verify):

```go
package main

import "fmt"

func main() {
	a := []int{1, 2}
	b := []int{3, 4}
	a = append(a, b...)
	fmt.Println(len(a))
	fmt.Println(a[3])
	a = append(a, a...)
	fmt.Println(len(a))
	fmt.Println(a[7])
	bs := []byte("ab")
	bs = append(bs, "cd"...)
	fmt.Println(string(bs))
	var z []int
	z = append(z, b...)
	fmt.Println(z[1])
}
```

`.expected.txt`: `4`, `4`, `8`, `4`, `abcd`, `4`. Both must fail to compile today (copy unknown / spread arg on append rejected).

- [ ] **Step 2: Runtime memmove core**

runtime.h:

```c
// copy(dst, src) core: moves min(dst_len, src_len) elements, returns the
// count. memmove — overlapping ranges legal (Go-exact). Raw-pointer ABI.
int64_t goo_slice_copy_raw(void* dst, int64_t dst_len,
                           const void* src, int64_t src_len, int64_t elem_size);
// append(dst, s...) bulk arm: snapshot src, grow dst by src_len, move in.
// Snapshot makes self-append (append(b, b...)) safe across the grow.
void goo_slice_append_bulk(goo_slice_t* dst, const void* src,
                           int64_t src_len, int64_t elem_size);
```

runtime.c:

```c
int64_t goo_slice_copy_raw(void* dst, int64_t dst_len,
                           const void* src, int64_t src_len, int64_t elem_size) {
    int64_t n = dst_len < src_len ? dst_len : src_len;
    if (n <= 0 || elem_size <= 0) return 0;
    memmove(dst, src, (size_t)(n * elem_size));
    return n;
}

void goo_slice_append_bulk(goo_slice_t* dst, const void* src,
                           int64_t src_len, int64_t elem_size) {
    if (!dst || src_len <= 0 || elem_size <= 0) return;
    size_t bytes = (size_t)(src_len * elem_size);
    void* snap = goo_alloc(bytes);          // survives a growth realloc that
    memcpy(snap, src, bytes);               // frees/moves dst's old block
    /* grow using the same policy as goo_slice_append (read it first and
       reuse its capacity-doubling block verbatim), then: */
    memcpy((char*)dst->data + dst->length * elem_size, snap, bytes);
    dst->length += (size_t)src_len;
    goo_free(snap);
}
```

(`goo_slice_t` field names: read `include/runtime.h`'s struct before transcribing — adjust `length`/`data`/`capacity` spellings to match.)

- [ ] **Step 3: Typecheck — copy arm + append spread**

Register the name (type_checker.c ~304, next to `append_var`) following `append_var`'s exact pattern with a placeholder signature (the real checking is the dedicated arm, as with append).

expression_checker.c, next to the append arm (~2360), new arm:

```c
        if (strcmp(func_ident->name, "copy") == 0) {
            if (!call->args || !call->args->next || call->args->next->next) {
                type_error(checker, expr->pos, "copy expects exactly two arguments (dst, src)");
                return NULL;
            }
            Type* dst_t = type_check_expression(checker, call->args);
            if (!dst_t) return NULL;
            if (dst_t->kind != TYPE_SLICE) {
                type_error(checker, expr->pos,
                           "copy: destination must be a slice, got %s", type_to_string(dst_t));
                return NULL;
            }
            Type* src_t = type_check_expression(checker, call->args->next);
            if (!src_t) return NULL;
            int byte_dst = dst_t->data.slice.element_type->kind == TYPE_UINT8; /* byte kind per Task 2 */
            int ok = (src_t->kind == TYPE_SLICE &&
                      type_compatible(src_t->data.slice.element_type,
                                      dst_t->data.slice.element_type) &&
                      src_t->data.slice.element_type->kind ==
                      dst_t->data.slice.element_type->kind)
                     || (byte_dst && src_t->kind == TYPE_STRING);
            if (!ok) {
                type_error(checker, expr->pos,
                           "copy: cannot copy %s into %s", type_to_string(src_t), type_to_string(dst_t));
                return NULL;
            }
            expr->node_type = type_new(TYPE_INT64); /* use the file's int-type constructor idiom */
            return expr->node_type;
        }
```

Extend the append arm: when `((CallExprNode*)expr)->has_spread`, the second arg must be `[]E` identical-elem to the first (or `string` when dst elem is the byte kind) instead of a bare element; error text `"append: cannot spread %s into %s"`. Two-arg count check unchanged (spread is still two args).

- [ ] **Step 4: Codegen — lower both**

call_codegen.c: append arm (~590): when `has_spread`, extract src slice's `{data, len}` (or string's `{ptr, len}` for the string form), take dst slice's address (the arm already works on a slice lvalue for in-place append — mirror it), call `goo_slice_append_bulk(dst_ptr, src_data, src_len, elem_size)` with `elem_size` from the dst elem type via the same sizing idiom `goo_slice_append` lowering uses. New copy arm: extract both headers' `{data, len}`, elem size from dst elem type, call `goo_slice_copy_raw`, result is the returned i64. Declare both externs beside the existing runtime decls.

- [ ] **Step 5: Gates, reject probe, commit**

`make clean && make lexer` if any header changed; golden 220/0; both new goldens' outputs match go run. `copy-reject-probe`: elem mismatch (`copy([]int, []string)`), string dst (`copy("x", b)`), each greps its message. Add to `verify:`; `make test`.

```bash
git add include/runtime.h src/runtime/runtime.c src/types/type_checker.c \
        src/types/expression_checker.c src/codegen/call_codegen.c \
        examples/copy_probe.* examples/append_spread_probe.* Makefile
git commit --no-gpg-sign -m "feat(builtins): copy(dst, src) incl. string source; append(dst, s...) bulk arm"
```

---

### Task 5: Verbatim `strings.Join` gate + sweep + handoff

**Files:**
- Create: `examples/strings_join_probe.goo` + `.expected.txt`
- Modify: `.handoff.md`

**Interfaces:** consumes everything (§1 trailing commas, §2 string(b), §3 spread, §4 copy); produces the branch's ship gate.

- [ ] **Step 1: The probe**

`examples/strings_join_probe.goo` — the `Join` function below must be VERBATIM from Go 1.9 `src/strings/strings.go` (pin: go1.9 release tag; BSD license header comment required). Fetch it (`https://raw.githubusercontent.com/golang/go/go1.9/src/strings/strings.go`) and diff your transcription — do NOT retype from this plan. Expected shape (for orientation only):

```go
// Join concatenates the elements of a to create a single string. The separator
// string sep is placed between elements in the resulting string.
func Join(a []string, sep string) string {
	switch len(a) {
	case 0:
		return ""
	case 1:
		return a[0]
	case 2:
		return a[0] + sep + a[1]
	case 3:
		return a[0] + sep + a[1] + sep + a[2]
	}
	n := len(sep) * (len(a) - 1)
	for i := 0; i < len(a); i++ {
		n += len(a[i])
	}

	b := make([]byte, n)
	bp := copy(b, a[0])
	for _, s := range a[1:] {
		bp += copy(b[bp:], sep)
		bp += copy(b[bp:], s)
	}
	return string(b)
}
```

KNOWN RISK to verify while transcribing: `b[bp:]` is a single-bound slice expression — ast.h documents `[i:]` as deferred (F5 note: "Both bounds required in v1"). If `b[bp:]` fails to parse, the RECORD-DON'T-ABSORB rule applies: substitute `b[bp:len(b)]` with a `// goo-deviation:` comment on those two lines, record the `[i:]` gap as a queue find in the handoff, and note the probe is verbatim-minus-one-documented-deviation. Do NOT implement open-ended slicing in this branch.

The probe's `main` (not verbatim — it supplies §1 + §3 coverage):

```go
func joinAll(sep string, parts ...string) string {
	return Join(parts, sep)
}

func main() {
	words := []string{
		"goo",
		"runs",
		"go",
		"source",
	}
	fmt.Println(Join(words, " "))
	fmt.Println(Join(words[0:1], "-"))
	fmt.Println(Join(words[0:2], "-"))
	fmt.Println(Join(words[0:3], "-"))
	fmt.Println(joinAll("+", words...))
	table := map[string][]string{
		"csv": {"a", "b", "c"},
		"tsv": {"x", "y"},
	}
	fmt.Println(Join(table["csv"], ","))
	fmt.Println(Join(table["tsv"], "\t"))
}
```

(If elided inner literals `{"a", ...}` inside the map literal don't parse, use explicit `[]string{"a", "b", "c",}` — with its trailing comma — and note it; elided-inner-literal support is a #110-era known limitation, not this branch's scope.)

Write the equivalent `.go` file (Join is stdlib there — call `strings.Join`), `go run` it, paste output into `.expected.txt`.

- [ ] **Step 2: Run + fix-forward**

`bin/goo -o build/sj examples/strings_join_probe.goo && ./build/sj` → must match `.expected.txt`. Any failure here is adjudicated by the controller: in-batch defect → fix in the owning task's files; out-of-scope gap → record + minimal documented deviation.

- [ ] **Step 3: Full verification sweep (real exit codes)**

```bash
make clean && make lexer                     # exit 0
make test                                    # 76/1, exit 0
bash scripts/run_golden.sh                   # 221 passed, 0 failed
eval "$(opam env --switch=default)"
make verify                                  # ALL GREEN GATES PASSED
make ccomp-link                              # PASS
bison -d -o /tmp/p.tab.c src/parser/parser.y 2>&1 | grep conflicts   # 81 + 256 exactly
```

- [ ] **Step 4: Handoff + commit**

Update `.handoff.md`: stdlib unblockers SHIPPED (this branch); queue promotes ptr-boxing miscompile pair to #1; record any Step 1/2 finds (e.g. `[i:]` open slicing). Then:

```bash
git add examples/strings_join_probe.* .handoff.md
git commit --no-gpg-sign -m "test(stdlib): verbatim Go 1.9 strings.Join gate probe; update handoff"
```

After Task 5: push, PR, fresh-context whole-branch review before merge (mandatory — this step caught real miscompiles on #109 and #110).

---

## Execution notes (controller)

- SDD economy mode: Sonnet implementers (one per task, fresh context), Fable controller reviews diff + runs independent probes between tasks.
- Tasks 2 and 3 both touch the grammar — they are the bison-risk tasks; the tripwire stop-rule is non-negotiable.
- Task 4 Step 2's grow-policy transcription ("reuse goo_slice_append's capacity block") is the one place an implementer must read neighboring code before writing — flag it in the dispatch brief.
- Plan self-review notes: append stays two-arg (multi-element `append(s, a, b)` is a recorded non-goal, NOT silently added); `b[bp:]` open slicing is a pre-identified probable deviation with its handling decided up front.
