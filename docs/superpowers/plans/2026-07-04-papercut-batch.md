# Papercut Batch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close four high-value friction gaps — multi-return interface coercion (a silent miscompile), `m[k]++`/`m[k] += n`, `if init; cond {`, and elided inner literals in map values.

**Architecture:** Two codegen fixes first (no grammar risk): interface-box concrete values at the multi-return aggregate site (mirroring the existing nullable auto-wrap), and desugar map-index compound/postfix targets to read-modify-write (mirroring the existing `m[k]=v` fast path). Then two tripwire-gated grammar additions (if-init form via the proven `simple_stmt SEMICOLON expression` shape; elided composite in map-value position). Spec: `docs/superpowers/specs/2026-07-04-papercut-batch-design.md`.

**Tech Stack:** C23, bison, LLVM-C API.

## Global Constraints

- Branch: `feat/papercut-batch` (exists, base main @ 59c615e). Commit `--no-gpg-sign`; pre-commit runs `make test`.
- Spec scope: exactly the high-value 4 (#6, #2, #1, #4). #5 (`}; stmt`) and #3 (blank-`_`) are OUT.
- **Grammar tripwire (T3, T4 only):** `./scripts/grammar-tripwire.sh` must PASS at **82 S/R + 256 R/R** before AND after any parser.y change. ANY delta → STOP, follow the goo-grammar skill's justified-delta procedure (`bison -Wcounterexamples`, classify, prove, or revert). If a grammar item can't reach zero delta without a justified case, it ships WITHOUT that item (report BLOCKED, controller decides) rather than raising the baseline for a papercut. T1/T2 touch no grammar — tripwire is a no-op sanity there.
- `include/ast.h` edits (only if a grammar task needs an AST field): tail-append + `make clean && make lexer`. Prefer desugars that avoid AST changes.
- Baselines: golden 237/0 (`bash scripts/run_golden.sh`), `make test` 76/1, bison 82/256.
- Every golden's expected output produced by `go run` on an equivalent `.go` program (go installed). Real exit codes only (never a pipeline `$?`). ccomp needs `eval "$(opam env --switch=default)"`.
- The map-value non-addressability guards MUST stay (`&m[k]`, `m[k].F = v` still reject) — #2 only adds whole-value RMW.

---

### Task 1: #6 multi-return interface coercion (silent miscompile)

**Files:**
- Modify: `src/codegen/statement_codegen.c` (return-aggregate loop, ~1315-1330)
- Create: `examples/multiret_iface_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: `codegen_interface_box(codegen, checker, iface_type, concrete_type, value)` (interface_codegen.c:206) — boxes a concrete value (value- or pointer-form) into an interface; returns the {vtable,data} aggregate.
- Produces: multi-return functions with interface return types boxing concrete returns correctly.

- [ ] **Step 1: Write the failing golden**

`examples/multiret_iface_probe.goo` (go-run-verify — the exact silent-miscompile repros plus a mixed return):

```go
package main

import "fmt"

type Sayer interface {
	Say() string
}

type Cat struct {
	Name string
}

func (c Cat) Say() string {
	return "meow " + c.Name
}

type Dog struct {
	Name string
}

func (d Dog) Say() string {
	return "woof " + d.Name
}

func pairLit() (Sayer, Sayer) {
	return Cat{Name: "a"}, Dog{Name: "b"}
}

func pairVar(c Cat) (Sayer, Sayer) {
	return c, c
}

func mixed() (int, Sayer) {
	return 7, Cat{Name: "m"}
}

func main() {
	x, y := pairLit()
	fmt.Println(x.Say())
	fmt.Println(y.Say())
	p, q := pairVar(Cat{Name: "v"})
	fmt.Println(p.Say())
	fmt.Println(q.Say())
	n, s := mixed()
	fmt.Println(n)
	fmt.Println(s.Say())
}
```

`examples/multiret_iface_probe.expected.txt`:

```
meow a
woof b
meow v
meow v
7
meow m
```

- [ ] **Step 2: Verify current failure**

Run: `bin/goo -o build/mr examples/multiret_iface_probe.goo && ./build/mr`
Expected: WRONG — `pairLit` prints empty/garbage lines, or `pairVar` fails module verification (`Invalid InsertValueInst operands`). Confirm the miscompile before fixing.

- [ ] **Step 3: Box concrete returns into interface fields**

In `statement_codegen.c`'s return-aggregate loop, immediately BEFORE the final `agg = LLVMBuildInsertValue(codegen->builder, agg, raw, (unsigned)i, "ret_field");` (line ~1329), insert an interface-boxing arm mirroring the nullable auto-wrap just above it:

```c
                // Box a concrete return value into an interface-typed return
                // field. Without this the raw concrete bits land in an
                // interface-shaped slot (empty/garbage output, or a verifier
                // failure for the variable form). Mirrors the nullable
                // auto-wrap above and the map/assignment interface-box arms.
                if (field_type && field_type->kind == TYPE_INTERFACE &&
                    vv->goo_type && vv->goo_type->kind != TYPE_INTERFACE) {
                    LLVMValueRef boxed = codegen_interface_box(codegen, checker,
                                                               field_type,
                                                               vv->goo_type, raw);
                    if (!boxed) {
                        codegen_error(codegen, v->pos,
                                      "failed to box concrete return value into interface");
                        value_info_free(vv);
                        return 0;
                    }
                    raw = boxed;
                }
```

(`raw` is already loaded-if-lvalue by the block above; `field_type` is the i-th return type computed at :1301-1303; `v` is the return-value AST node in scope.)

- [ ] **Step 4: Build, run, gate**

```bash
make lexer                                    # exit 0
bin/goo -o build/mr examples/multiret_iface_probe.goo && ./build/mr    # matches expected
bash scripts/run_golden.sh                    # 238 passed, 0 failed
make test                                     # 76/1
```

- [ ] **Step 5: Commit**

```bash
git add src/codegen/statement_codegen.c examples/multiret_iface_probe.*
git commit --no-gpg-sign -m "fix(codegen): interface-box concrete values at multi-return aggregate"
```

---

### Task 2: #2 map compound writes (m[k]++, m[k] += n)

**Files:**
- Modify: `src/codegen/expression_codegen.c` (compound-assign ~1047; postfix `AST_POSTFIX_EXPR` ~185)
- Create: `examples/map_compound_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: `codegen_map_key_to_slot(codegen, checker, kv, key_type)`, `codegen_map_value_to_slot(codegen, value, val_type)`, `codegen_map_slot_to_value(codegen, slot, val_type)` (#115 helpers); `goo_map_get_sv(map, i64 key)→i64` and `goo_map_set_sv(map, i64 key, i64 val)`; the `m[k]=v` fast-path pattern at expression_codegen.c:1111-1130.
- Produces: `m[k]++`, `m[k]--`, `m[k] += n` (and other compound ops) on map-index targets.

- [ ] **Step 1: Write the failing golden**

`examples/map_compound_probe.goo` (go-run-verify):

```go
package main

import "fmt"

func main() {
	m := map[string]int{"a": 1}
	m["a"]++
	fmt.Println(m["a"])
	m["a"] += 5
	fmt.Println(m["a"])
	m["a"] -= 2
	fmt.Println(m["a"])
	m["b"]++
	fmt.Println(m["b"])
	freq := map[rune]int{}
	for _, r := range "banana" {
		freq[r]++
	}
	fmt.Println(freq['a'])
	fmt.Println(freq['n'])
}
```

`examples/map_compound_probe.expected.txt`:

```
2
7
5
1
3
2
```

- [ ] **Step 2: Verify current failure**

Run: `bin/goo -o build/mc examples/map_compound_probe.goo`
Expected: rejected — `cannot assign through a map value` / `Compound-assignment target must be an addressable lvalue`.

- [ ] **Step 3: Add a shared map-RMW helper**

In `expression_codegen.c` (near the compound-assign block), add a static helper that reads-modifies-writes a map element. It takes the map-index target node, the base op, and the RHS value node (RHS is NULL for postfix ++/--, which use a literal 1):

```c
// Read-modify-write for a map-index target `m[k]`: read old via goo_map_get_sv,
// compute old <op> rhs (or old +/- 1 for postfix), write via goo_map_set_sv.
// Returns the NEW value (loaded), or NULL on error. The plain m[k]=v fast path
// (this file, ~line 1111) is the model for the get/set + slot conversions.
static ValueInfo* codegen_map_index_rmw(CodeGenerator* codegen, TypeChecker* checker,
                                        ASTNode* index_node, TokenType base_op,
                                        ASTNode* rhs_or_null) {
    IndexExprNode* idx = (IndexExprNode*)index_node;
    Type* base_t = type_check_expression(checker, idx->expr);
    if (!base_t || base_t->kind != TYPE_MAP) return NULL;
    Type* key_type = base_t->data.map.key_type;
    Type* val_type = base_t->data.map.value_type;

    LLVMValueRef get_fn = LLVMGetNamedFunction(codegen->module, "goo_map_get_sv");
    LLVMValueRef set_fn = LLVMGetNamedFunction(codegen->module, "goo_map_set_sv");
    if (!get_fn || !set_fn) { codegen_error(codegen, index_node->pos, "map get/set missing"); return NULL; }

    ValueInfo* mv = codegen_generate_expression(codegen, checker, idx->expr);
    ValueInfo* kv = codegen_generate_expression(codegen, checker, idx->index);
    if (!mv || !kv) { value_info_free(mv); value_info_free(kv); return NULL; }
    LLVMValueRef kslot = codegen_map_key_to_slot(codegen, checker, kv, key_type);

    // old = slot_to_value(goo_map_get_sv(m, kslot))
    LLVMValueRef gargs[2] = { mv->llvm_value, kslot };
    LLVMValueRef old_slot = LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(get_fn),
                                           get_fn, gargs, 2, "rmw_get");
    LLVMValueRef old_val = codegen_map_slot_to_value(codegen, old_slot, val_type);

    // rhs value: the RHS expr, or a constant 1 for postfix
    LLVMValueRef rhs;
    if (rhs_or_null) {
        ValueInfo* rv = codegen_generate_expression(codegen, checker, rhs_or_null);
        if (!rv) { value_info_free(mv); value_info_free(kv); return NULL; }
        rhs = rv->is_lvalue && rv->goo_type
              ? LLVMBuildLoad2(codegen->builder, codegen_type_to_llvm(codegen, rv->goo_type), rv->llvm_value, "rmw_rhs")
              : rv->llvm_value;
        value_info_free(rv);
    } else {
        rhs = LLVMConstInt(codegen_type_to_llvm(codegen, val_type), 1, 0);
    }

    // new = old <op> rhs — reuse the integer binop builders (val types here are
    // the map value type; for the admitted numeric value types this is a plain
    // Add/Sub/... — match the operator builders the postfix/binary paths use).
    LLVMValueRef newv;
    switch (base_op) {
        case TOKEN_PLUS:  newv = LLVMBuildAdd(codegen->builder, old_val, rhs, "rmw_add"); break;
        case TOKEN_MINUS: newv = LLVMBuildSub(codegen->builder, old_val, rhs, "rmw_sub"); break;
        case TOKEN_MULTIPLY: newv = LLVMBuildMul(codegen->builder, old_val, rhs, "rmw_mul"); break;
        default:
            codegen_error(codegen, index_node->pos,
                          "unsupported compound op on map value (v1: + - *)");
            value_info_free(mv); value_info_free(kv); return NULL;
    }

    // goo_map_set_sv(m, kslot, value_to_slot(new))
    LLVMValueRef nslot = codegen_map_value_to_slot(codegen, newv, val_type);
    LLVMValueRef sargs[3] = { mv->llvm_value, kslot, nslot };
    LLVMBuildCall2(codegen->builder, LLVMGlobalGetValueType(set_fn), set_fn, sargs, 3, "");

    value_info_free(mv); value_info_free(kv);
    ValueInfo* out = value_info_new();  // use this file's ValueInfo allocator
    out->llvm_value = newv; out->goo_type = val_type; out->is_lvalue = 0;
    return out;
}
```

NOTE: adapt `value_info_new()`/allocation to this file's actual ValueInfo constructor; match the operator builders (LLVMBuildAdd etc.) the existing binary-op codegen uses for the map value's width. The admitted compound ops for v1 are `+ - *` (extend the switch if the existing binary path trivially supports more — but do not overbuild). Postfix passes `TOKEN_PLUS`/`TOKEN_MINUS` with `rhs_or_null == NULL`.

- [ ] **Step 4: Route map-index targets in compound-assign and postfix**

Compound-assign (expression_codegen.c ~1047, before `codegen_emit_lvalue_address`): if `binary->left` is `AST_INDEX_EXPR` whose base is `TYPE_MAP`, return `codegen_map_index_rmw(codegen, checker, binary->left, base_op, binary->right)` instead of resolving the lvalue address.

Postfix (`AST_POSTFIX_EXPR` ~185, before the lvalue-address resolve): if `p->operand` is `AST_INDEX_EXPR` whose base is `TYPE_MAP`, return `codegen_map_index_rmw(codegen, checker, p->operand, p->operator == TOKEN_INCREMENT ? TOKEN_PLUS : TOKEN_MINUS, NULL)`.

Guard the base-type check via `type_check_expression(checker, idx->expr)` and `kind == TYPE_MAP` — mirror the `m[k]=v` fast path's map detection. Non-map index targets fall through to the existing GEP/lvalue path unchanged.

- [ ] **Step 5: Build, run, gates + regression pin**

```bash
make lexer                                    # exit 0
bin/goo -o build/mc examples/map_compound_probe.goo && ./build/mc     # matches expected
bash scripts/run_golden.sh                    # 239 passed, 0 failed
make map-addr-reject-probe                    # PASS — &m[k]/m[k].F=v STILL reject (not weakened)
make test                                     # 76/1
```

- [ ] **Step 6: Commit**

```bash
git add src/codegen/expression_codegen.c examples/map_compound_probe.*
git commit --no-gpg-sign -m "feat(codegen): m[k]++ / m[k] += n via read-modify-write desugar"
```

---

### Task 3: #1 if-init guard (grammar, tripwire)

**Files:**
- Modify: `src/parser/parser.y` (`if_stmt` ~1147)
- Create: `examples/if_init_probe.goo` + `.expected.txt`
- Modify: `Makefile` (`if-init-scope-reject-probe` + `verify:`)

**Interfaces:**
- Consumes: `simple_stmt` nonterminal; the `for` C-style init form (parser.y:1220) as the shape precedent; block/statement-list construction helpers.
- Produces: `if init; cond { }` and its else forms.

- [ ] **Step 1: Confirm baseline + write golden**

`./scripts/grammar-tripwire.sh` → PASS 82/256. Then `examples/if_init_probe.goo` (go-run-verify):

```go
package main

import "fmt"

func main() {
	m := map[string]int{"a": 1}
	if v, ok := m["a"]; ok {
		fmt.Println("has a", v)
	}
	if _, ok := m["z"]; ok {
		fmt.Println("has z")
	} else {
		fmt.Println("no z")
	}
	if x := 3 * 4; x > 10 {
		fmt.Println("big", x)
	} else {
		fmt.Println("small", x)
	}
}
```

`examples/if_init_probe.expected.txt`:

```
has a 1
no z
big 12
```

- [ ] **Step 2: Verify current parse failure**

Run: `bin/goo -o build/ii examples/if_init_probe.goo`
Expected: `Parse error ... syntax error` at the first `if v, ok := ...; ok`.

- [ ] **Step 3: Add the grammar arms (desugar to a wrapping block)**

In `parser.y`, add init-form arms to `if_stmt` (parser.y:1147). PREFER a desugar that wraps `{ init; if cond {...} }` so no AST/codegen change is needed and the init var scopes correctly (out of scope after the if). Add three arms mirroring the existing `IF expression block [ELSE ...]` set:

```yacc
    | IF simple_stmt SEMICOLON expression block {
        /* desugar: { init; if cond block } — reuse the block/statement-list
           constructors used elsewhere in this file. Build an IfStmtNode from
           ($4 cond, $5 block), then wrap [$2 init, if] in a new block node so
           the init var scopes to the wrapper only. */
        ASTNode* ifn = /* ast_if_stmt_new($4, $5, NULL, pos) — match existing IF arm's constructor */;
        $$ = /* block_new([$2, ifn]) — match how `block` / statement_list wraps children */;
    }
    | IF simple_stmt SEMICOLON expression block ELSE block {
        ASTNode* ifn = /* ast_if_stmt_new($4, $5, $7, pos) */;
        $$ = /* block_new([$2, ifn]) */;
    }
    | IF simple_stmt SEMICOLON expression block ELSE if_stmt {
        ASTNode* ifn = /* ast_if_stmt_new($4, $5, $7, pos) */;
        $$ = /* block_new([$2, ifn]) */;
    }
```

Transcribe the EXACT constructor the existing `IF expression block` arms use (read parser.y:1148-1170 for `ast_if_stmt_new` or the actual name/signature) and the EXACT block/statement-list wrapper the codebase uses (grep how `block:` and `statement_list` build nodes). If the wrapper-block desugar proves awkward (e.g. block scoping doesn't isolate the init var), fall back to adding an optional `init` field to the if-node (tail-append in ast.h + a codegen tweak to emit init before the condition) — but try the desugar first.

- [ ] **Step 4: Tripwire (THE gate)**

Run: `./scripts/grammar-tripwire.sh`
Expected: PASS 82/256. On ANY delta: STOP, run `bison -Wcounterexamples -d -o /tmp/p.tab.c src/parser/parser.y 2>&1 | head -80`, and either justify per the goo-grammar procedure (update EXPECTED_SR + ledger in-commit with a proven counterexample analysis) OR, if it can't be justified cleanly, report BLOCKED — this item then ships omitted rather than raising the baseline for a papercut. The `simple_stmt SEMICOLON expression` shape is proven in for-init, so a clean pass is expected; the risk is the S/R vs `IF expression block` (disambiguated on SEMICOLON).

- [ ] **Step 5: Build, run, scope reject probe**

```bash
make lexer                                    # exit 0
bin/goo -o build/ii examples/if_init_probe.goo && ./build/ii   # matches expected
bash scripts/run_golden.sh                    # 240 passed, 0 failed
make test                                     # 76/1
```

`Makefile` `if-init-scope-reject-probe` (the init var must NOT be visible after the if — Go scoping; compile-must-fail):

```makefile
if-init-scope-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== if-init-scope-reject-probe: init var out of scope after if ==="
	@printf 'package main\nimport "fmt"\nfunc main(){\n\tif x := 1; x > 0 {\n\t\t_ = x\n\t}\n\tfmt.Println(x)\n}\n' > build/ifscope.goo
	@if $(COMPILER) -o build/ifscope build/ifscope.goo 2>build/ifscope.err; then \
	  echo "if-init-scope-reject-probe: FAIL (x leaked past if)"; exit 1; \
	else echo "if-init-scope-reject-probe: PASS"; fi
```

Add `if-init-scope-reject-probe` to `verify:`. Run `make if-init-scope-reject-probe` → PASS.

- [ ] **Step 6: Commit**

```bash
git add src/parser/parser.y examples/if_init_probe.* Makefile
git commit --no-gpg-sign -m "feat(parser): if-init guard — if init; cond { } (desugar to scoped block)"
```

---

### Task 4: #4 elided inner literals in map values (grammar, tripwire)

**Files:**
- Modify: `src/parser/parser.y` (map-entry value / composite_value production)
- Create: `examples/map_elided_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: the elided-composite machinery (`composite_value`, parser.y:45-52); `map_entry`/`map_entry_list`.
- Produces: `map[K]V{key: {...}}` where `{...}` is an elided composite of V's type.

- [ ] **Step 1: Confirm baseline + write golden**

`./scripts/grammar-tripwire.sh` → PASS 82/256. Then `examples/map_elided_probe.goo` (go-run-verify):

```go
package main

import "fmt"

type P struct {
	X int
	Y int
}

func main() {
	sl := map[string][]int{"a": {1, 2}, "b": {3}}
	fmt.Println(sl["a"][1])
	fmt.Println(sl["b"][0])
	st := map[string]P{"p": {X: 1, Y: 2}}
	fmt.Println(st["p"].Y)
	nm := map[string]map[string]int{"m": {"k": 9}}
	fmt.Println(nm["m"]["k"])
}
```

`examples/map_elided_probe.expected.txt`:

```
2
3
2
9
```

- [ ] **Step 2: Verify current parse failure**

Run: `bin/goo -o build/me examples/map_elided_probe.goo`
Expected: `Parse error ... syntax error` at the `{1, 2}` inner literal.

- [ ] **Step 3: Extend the map-value production**

Read `map_entry` and the value nonterminal it uses (parser.y ~2200 map_lit region + the `composite_value`/elided machinery at :45-52). The map-entry value currently accepts an `expression` (or similar) but not a brace-elided composite. Add the elided `{...}` form as an accepted map-entry value, producing the same elided-composite node the top-level `[]T{...}`/struct-elided paths produce (type inferred at typecheck from the map's value type V). Transcribe the EXACT elided-composite node constructor the existing elided arms use (e.g. `struct_literal_new(NULL, ...)` for elided structs, and whatever the elided slice/array element uses). The value's type is resolved from V at typecheck — the parser just produces the untyped elided node.

- [ ] **Step 4: Tripwire (THE gate)**

Run: `./scripts/grammar-tripwire.sh`
Expected: PASS 82/256. On ANY delta: STOP, `-Wcounterexamples`, justify-or-BLOCK per the goo-grammar procedure (BLOCK → item ships omitted). Lower risk than Task 3 (extending an existing elided path), but still the hard gate.

- [ ] **Step 5: Build, run, gates**

```bash
make lexer                                    # exit 0
bin/goo -o build/me examples/map_elided_probe.goo && ./build/me   # matches expected
bash scripts/run_golden.sh                    # 241 passed, 0 failed
make test                                     # 76/1
```

- [ ] **Step 6: Commit**

```bash
git add src/parser/parser.y examples/map_elided_probe.*
git commit --no-gpg-sign -m "feat(parser): elided inner composite literals in map values"
```

---

### Task 5: Full sweep + handoff

**Files:**
- Modify: `.handoff.md`

**Interfaces:** consumes T1-T4; produces the branch ship gates.

- [ ] **Step 1: Full sweep (real exit codes)**

```bash
make clean && make lexer                      # exit 0
make test                                     # 76/1
bash scripts/run_golden.sh                    # 241/0 (237 + 4 new; fewer if a grammar item BLOCKED)
eval "$(opam env --switch=default)"
make verify                                   # ALL GREEN GATES PASSED
make ccomp-link                               # PASS
./scripts/grammar-tripwire.sh                 # PASS 82/256
```

- [ ] **Step 2: Update `.handoff.md`**

Mark the papercut batch items SHIPPED (this branch): #6 multi-return interface coercion (was a silent miscompile — concrete returns now boxed), #2 `m[k]++`/`+=`/`-=` map compound writes (RMW desugar; guards intact), #1 if-init guard, #4 elided inner literals in map values. Record any grammar item that BLOCKED (shipped omitted, with the counterexample reason). Keep #5 (`}; stmt`) and #3 (blank-`_`) in the queue as still-open. Note the map-RMW ops are `+ - *` in v1 (other compound ops on map values still reject — extend if needed later). Promote next queue head (slice-index write bounds checking, or per the queue).

- [ ] **Step 3: Commit**

```bash
git add .handoff.md
git commit --no-gpg-sign -m "docs(handoff): papercut batch shipped; queue updated"
```

After Task 5: push, PR, fresh-context whole-branch review before merge (mandatory — with deliberate lvalue/degenerate-shape probes per the #114/#115 lesson).

---

## Execution notes (controller)

- SDD economy mode: Sonnet implementers, controller (main loop) review + independent differential probes between tasks. Per the #114/#115 lesson, controller probes DELIBERATELY use lvalue operands and degenerate shapes, not just the happy rvalue path.
- T1 (#6) is the correctness fix — its reviewer confirms the box helper fires for value AND pointer concretes and the mixed-return raw field still passes through unboxed.
- T2 (#2) reviewer confirms `&m[k]` / `m[k].F=v` still REJECT (guards not weakened) and missing-key RMW starts from zero.
- T3/T4 are the tripwire-risk tasks. The 82/256 stop-rule is non-negotiable; a BLOCK means the item ships omitted, not a raised baseline. Front-load `-Wcounterexamples`.
- Golden counts assume all 4 land: 237 → 238 → 239 → 240 → 241. If a grammar item BLOCKs, adjust down and note it.
