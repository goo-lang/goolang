# Frontend Go-Syntax Completeness — Phase F1 (Implementation Plan)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`).

**Goal:** Make `bin/goo` parse + type-check + run the *pervasive* Go syntax that the stdlib-interop spike found missing — the wall before any Go stdlib code (or self-hosting) can compile. This phase covers the **tractable high-leverage** gaps; the big features (closures, interface decls/dispatch, variadic, generics) are a deferred Phase F2.

**Spike evidence (2026-06-29):** real Go stdlib fails at the frontend. This phase closes the most pervasive, independently-tackleable gaps.

**Architecture:** Mostly additive grammar + type-check + codegen, each gap independent, ordered easiest-first so the `test-golden` oracle is exercised continuously and grammar-heavy work lands with the conflict guard.

**Tech Stack:** C23, `src/parser/parser.y` (yacc), `src/lexer/`, the type checker (`src/types/`), codegen (`src/codegen/`), Makefile golden runner.

## Global Constraints
- **Build env:** `eval $(opam env --switch=default)` before any `make`; ignore `parser.tab.c` recipe-override warnings.
- **Gates:** `make verify` (incl. `test-golden`) + `make test` (76/1) stay green; every existing golden case keeps passing.
- **Grammar changes (F4/F5/F6) must NOT increase the parser conflict count** (currently 68 s/r + 156 r/r) — rebuild with bison and compare. If a change adds conflicts, resolve or escalate; do not introduce fragile grammar.
- `int` is i32 today (separate milestone) — small-valued fixtures.
- Each gap is verified by a golden probe (positive feature) and where relevant a negative probe (clean rejection). TDD: RED first.
- Commit discipline: conventional commits, `--no-gpg-sign`, stage only named files; never `git add .`/`.superpowers/`/`build/`.
- **Escalate, don't force:** if a gap needs broad rework (e.g. slice codegen reslicing, multi-assign tuple ABI) beyond a localized change, report DONE_WITH_CONCERNS / BLOCKED with specifics.

---

### Task 1 (F1): Blank identifier `_` as an assignment target
**Files:** `src/types/` (assignment type-check — `expression ASSIGN expression`), `src/codegen/` (assignment codegen), Makefile (`blank-assign-probe`)
**Current:** `_ = f()` → "Undefined variable '_'" (LHS `_` type-checked as an expression). Note `a, _ := f()` (decl) and `for _, v := range` ALREADY work — scope is only the plain-assignment target.
- [ ] **Step 1: RED golden probe** — `examples/blank_assign_probe.goo`: `_ = sideEffect()` (a func printing) discards the value; program prints the side effect once. Expected deterministic stdout.
- [ ] **Step 2: Run → FAIL** ("Undefined variable '_'").
- [ ] **Step 3: Implement** — in assignment type-check, when the LHS is the identifier `_`: skip the LHS lookup, type-check the RHS, discard. Codegen: evaluate the RHS for side effects, store nowhere. Keep normal assignment unchanged.
- [ ] **Step 4: GREEN** + `make verify` green.
- [ ] **Step 5: Commit** — `feat(types): blank identifier _ as an assignment target (F1)`.

---

### Task 2 (F2): Type conversions `T(x)` for builtin numeric/string types
**Files:** `src/types/` (call type-check — recognize a type name in call position as a conversion), `src/codegen/call_codegen.c` (emit the conversion), Makefile (`conv-probe`)
**Current:** `int(x)` → "Undefined variable 'int'" (callee `int` treated as a variable). `byte(n)`, `string(b)` similar.
- [ ] **Step 1: RED golden probe** — `examples/conv_probe.goo`: `var x int64 = 300; y := int(x)`, `b := byte(65)`, print results. Expected deterministic.
- [ ] **Step 2: Run → FAIL**.
- [ ] **Step 3: Implement** — in call type-check, if the callee is a builtin type name (`int`,`int8/16/32/64`,`uint*`,`byte`,`float32/64`,`bool`,`string`) with exactly one arg, treat as a conversion: result type = the named type; validate the source is convertible (numeric↔numeric, int↔byte; `string(intbytes)`/`string([]byte)` may be deferred — reject cleanly if unsupported). Codegen: SExt/Trunc/SIToFP/FPToSI/bitcast as appropriate (reuse the slice_coerce_elem / codegen_convert_value patterns).
- [ ] **Step 4: GREEN** + `make verify` green.
- [ ] **Step 5: Commit** — `feat: builtin type conversions T(x) (F2)`.

---

### Task 3 (F3): Char/rune literals `'x'`
**Files:** `src/lexer/lexer.c` (lex `'x'`/`'\n'`/`'\''` to a rune/int token), `src/types/` (type as int/rune), `src/codegen/` (constant), Makefile (`charlit-probe`)
**Current:** `'0'` → parse error (char literals not lexed).
- [ ] **Step 1: RED golden probe** — `examples/charlit_probe.goo`: `c := 'A'; fmt.Println(c)` (prints 65), and char arithmetic `'0' + 5`. Expected deterministic.
- [ ] **Step 2: Run → FAIL** (parse error).
- [ ] **Step 3: Implement** — lexer: recognize single-quoted char literals incl. common escapes (`\n \t \\ \' \0`), produce an integer/rune token. Type: int (rune = i32 today). Codegen: constant int. Confirm the existing TOKEN set / parser accepts the new token in expression position with NO new grammar conflicts.
- [ ] **Step 4: GREEN** + `make verify` green; conflicts unchanged.
- [ ] **Step 5: Commit** — `feat(lexer): char/rune literals (F3)`.

---

### Task 4 (F4): Grouped `const ( ... )` blocks + `iota`
**Files:** `src/parser/parser.y` (grouped const), `src/types/` (iota sequencing), Makefile (`const-group-probe`)
**Current:** `const ( a = 1\nb = 2 )` and `iota` → parse error.
- [ ] **Step 1: RED golden probe** — `examples/const_group_probe.goo`: a grouped const block with explicit values AND an `iota` sequence (`const ( A = iota; B; C )` → 0,1,2). Print them.
- [ ] **Step 2: Run → FAIL** (parse error).
- [ ] **Step 3: Implement** — grammar: `const ( const_spec_list )` where each spec is `ident [= expr]` (a bare ident repeats the previous expr with iota incremented). Rebuild parser; NO new conflicts. Type/codegen: evaluate each const, threading `iota` (the spec index within the group). Keep single `const x = v` working.
- [ ] **Step 4: GREEN** + `make verify` green; conflicts unchanged.
- [ ] **Step 5: Commit** — `feat: grouped const blocks + iota (F4)`.

---

### Task 5 (F5): Slice/substring expressions `s[i:j]`
**Files:** `src/parser/parser.y` (slice form of index_expr), `src/ast/`, `src/types/`, `src/codegen/composite_codegen.c`, Makefile (`slice-expr-probe`)
**Current:** `s[1:3]` → parse error (only `s[i]` indexing parses).
- [ ] **Step 1: RED golden probe** — `examples/slice_expr_probe.goo`: substring `s := "hello"; fmt.Println(s[1:3])` → `el`; and a slice reslice `xs := [10,20,30,40]; ys := xs[1:3]; fmt.Println(ys[0], len(ys))` → `20 2`.
- [ ] **Step 2: Run → FAIL** (parse error).
- [ ] **Step 3: Implement** — grammar: add `postfix_expr LBRACKET expression COLON expression RBRACKET` (and optionally the `[i:]`/`[:j]` forms) as a slice expression AST node. Rebuild; NO new conflicts (COLON already a token; watch the map-literal `key:val` interaction). Type: result is the same slice type, or string for a string base. Codegen: for strings, build a substring goo_string (data+offset, len = j-i); for slices, a reslice header (data+i, len=j-i, cap adjusted). Escalate if reslice codegen needs broad runtime support.
- [ ] **Step 4: GREEN** + `make verify` green; conflicts unchanged.
- [ ] **Step 5: Commit** — `feat: slice/substring expressions s[i:j] (F5)`.

---

### Task 6 (F6): Multiple assignment `a, b := 1, 2` (multiple RHS values)
**Files:** `src/parser/parser.y` (multi-RHS short-decl + multi-assign), `src/types/`, `src/codegen/`, Makefile (`multi-assign-probe`)
**Current:** `a, b := 1, 2` → parse error. (Multi-LHS with a single multi-value RHS like `a, b := f()` already works.)
- [ ] **Step 1: RED golden probe** — `examples/multi_assign_probe.goo`: `a, b := 1, 2` then `a, b = b, a` (swap) → print `2 1`.
- [ ] **Step 2: Run → FAIL** (parse error).
- [ ] **Step 3: Implement** — grammar: extend short-var-decl and plain-assignment to accept comma-separated LHS and RHS lists of equal length. Rebuild; NO new conflicts (watch the existing `ident COMMA ident SHORT_ASSIGN expr` rule for the f()-destructuring case — keep it). Type: each LHS gets the corresponding RHS type. Codegen: evaluate ALL RHS values first (Go's simultaneous-assignment semantics — required for the swap), then store. Escalate if simultaneous-eval needs temporaries beyond a localized change.
- [ ] **Step 4: GREEN** (swap prints `2 1`, proving simultaneous eval) + `make verify` green; conflicts unchanged.
- [ ] **Step 5: Commit** — `feat: multiple assignment a, b := 1, 2 with simultaneous eval (F6)`.

---

### Task 7 (F7): `range` over string
**Files:** `src/types/` (allow range over string), `src/codegen/statement_codegen.c` (range desugar for strings), Makefile / golden
**Current:** `for i, c := range "hi"` → "for-range supported only on slice/array types in M8".
- [ ] **Step 1: RED golden probe** — `examples/range_string_probe.goo`: `for i, c := range "hi" { fmt.Println(i, c) }` → `0 104` / `1 105` (byte values; rune decoding deferred — document as ASCII/byte-wise for v1).
- [ ] **Step 2: Run → FAIL**.
- [ ] **Step 3: Implement** — type-check: accept a string range source (key int index, value int byte/rune). Codegen: desugar like the slice range but over the string's bytes (data ptr + len from the goo_string header). Note: v1 iterates bytes (not decoded runes) — document the limitation.
- [ ] **Step 4: GREEN** + `make verify` green.
- [ ] **Step 5: Commit** — `feat: range over string (byte-wise) (F7)`.

---

### Task 8 (F8): `error` builtin interface type (as a usable type name)
**Files:** `src/types/` (register `error` as a builtin type), Makefile / golden
**Current:** `func f() error` → "Unknown type 'error'". Note `!T` error unions are a SEPARATE Goo mechanism — this is Go's `error` interface type as a nameable type.
- [ ] **Step 1: RED probe** — `func f() error { return nil }` currently errors; target: it type-checks. Since full interface dispatch is deferred (Phase 6), scope F8 to: `error` is a recognized type name that accepts `nil` and is usable in signatures; calling methods on it (`.Error()`) is deferred to Phase 6. A probe: `func f() error { return nil }; func main() { e := f(); if e == nil { fmt.Println("ok") } }` → `ok`.
- [ ] **Step 2: Run → FAIL** ("Unknown type 'error'").
- [ ] **Step 3: Implement** — register `error` as a builtin (a nilable interface-like type): resolves as a type, accepts `nil`, supports `== nil`. Codegen: represent as a nullable pointer for now (full vtable dispatch is Phase 6). If method-call-on-error proves needed, escalate to Phase 6 rather than half-implementing dispatch.
- [ ] **Step 4: GREEN** + `make verify` green.
- [ ] **Step 5: Commit** — `feat(types): error builtin type (nil-comparable; dispatch deferred to Phase 6) (F8)`.

---

## Final verification
- [ ] `_ = expr` discards (F1); `int(x)`/`byte(n)` convert (F2); `'A'` is 65 (F3); grouped const + iota work (F4); `s[i:j]` substring/reslice (F5); `a,b := 1,2` + swap (F6); `range` over string (F7); `error` is a nil-comparable type (F8).
- [ ] `make verify` ALL GREEN (incl. `test-golden`); `make test` 76/1; parser conflict count NOT increased.

## Deferred to Phase F2 (bigger features)
Closures / func literals; variadic params `...T`; interface declarations + `interface{}` + dynamic dispatch (roadmap Phase 6); generics; `&T{...}` address-of composite literals (if not already working); rune-decoded range. These unblock the rest of the stdlib but are larger, conflict-prone, or depend on interfaces.

## Spec coverage self-check
| Spike gap | Plan task |
|---|---|
| blank `_` assignment target | F1 |
| type conversions T(x) | F2 |
| char/rune literals | F3 |
| grouped const + iota | F4 |
| slice exprs s[i:j] | F5 |
| multiple assignment | F6 |
| range over string | F7 |
| error builtin type | F8 |
| closures / interfaces / variadic / generics | DEFERRED → F2 |
