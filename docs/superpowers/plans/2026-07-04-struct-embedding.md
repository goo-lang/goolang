# Struct Embedding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Go-style struct embedding — anonymous fields with field/method promotion and interface satisfaction — per the approved spec `docs/superpowers/specs/2026-07-04-struct-embedding-design.md`.

**Architecture:** (1) Struct-body-scoped ASI in the lexer makes 1-token embedded fields terminate at newlines; (2) new semicolon-terminated grammar productions store embedded fields as ordinary named fields with an `is_embedded` flag; (3) a BFS resolver module (`src/types/embedding.c`) owns promotion semantics; (4) promotion is an AST-rewrite desugar in `type_check_selector_expr` (`o.X` → `(o.Base).X`), so codegen is untouched except (5) a fallback in the existing interface `build_thunk` that GEP-walks the embedding path.

**Tech Stack:** C23, bison/LALR, LLVM-C API, existing golden-probe + reject-probe test harness.

## Global Constraints

- **Header edits require `make clean`** before rebuilding — the Makefile has no header deps; stale objects silently miscompile (`include/ast.h`, `include/types.h`, `include/lexer.h` are all touched by this plan).
- **Bison baseline: 81 shift/reduce + 256 reduce/reduce, exact.** Check after every parser.y change: `bison -d -o /tmp/p.tab.c src/parser/parser.y 2>&1 | grep conflicts`. Expected output: `warning: 81 shift/reduce conflicts` and `warning: 256 reduce/reduce conflicts`. ANY drift = stop, run with `-Wcounterexamples`, and report to the controller — do not proceed on a changed count without sign-off.
- **Commits:** `git commit --no-gpg-sign`, conventional-commit messages, imperative mood. Pre-commit hook runs `make test` automatically.
- **Golden probes** are auto-discovered: any `examples/<name>.goo` with a sibling `examples/<name>.expected.txt` runs under `make test-golden` (`scripts/run_golden.sh`). Expected files must match stdout exactly.
- **Reject probes** are Makefile targets (pattern: `boolnot-reject-probe` at Makefile:499) and MUST be appended to the `verify:` target's dependency list (Makefile:1738).
- **Go differential:** every golden probe in this plan is valid Go. Generate/verify each `.expected.txt` with `go run <probe>.goo` (rename to `.go` in /tmp if needed: `cp examples/x_probe.goo /tmp/x.go && go run /tmp/x.go`). Goo output must be go-run-identical.
- Full gates before declaring any task done: `make lexer && make test`. Tasks 4+ also run `bash scripts/run_golden.sh` (expect ALL pass; count grows as tasks add probes; baseline before this plan = 200 passing).

---

### Task 1: Lexer — struct-body-scoped ASI

**Files:**
- Modify: `include/lexer.h` (Lexer struct, tail append)
- Modify: `src/lexer/lexer.c` (`case '{'` ~:167, `case '}'` ~:171, `case '\n'` :135–156, `lexer_new`)
- Test: `examples/embed_asi_base_probe.goo` + `.expected.txt` (regression pin), golden suite

**Interfaces:**
- Consumes: existing `lexer->prev_token_type` (set at lexer.c:510), `token_ends_value()` (lexer.c:84).
- Produces: inside struct bodies, a newline after a field-ending token yields a real `TOKEN_SEMICOLON`. Task 2's grammar relies on this. No behavior change outside struct bodies.

- [ ] **Step 1: Verify the keyword token name**

Run: `grep -n "TOKEN_STRUCT" include/token.h src/lexer/*.c | head -5`
Expected: `TOKEN_STRUCT` exists and is what the keyword `struct` lexes to. If the name differs, use the actual name everywhere below.

- [ ] **Step 2: Write the failing golden probe (parses only after Task 2 — for THIS task it must keep failing to compile, while the existing suite stays green)**

Create `examples/embed_asi_base_probe.goo`:

```go
package main

import "fmt"

type Base struct {
	N int
}

type Outer struct {
	Base
	X int
}

func main() {
	o := Outer{}
	o.Base.N = 41
	o.X = 1
	fmt.Println(o.Base.N + o.X)
}
```

Create `examples/embed_asi_base_probe.expected.txt` with content (verify with `go run`):

```
42
```

Run: `bin/goo examples/embed_asi_base_probe.goo -o /tmp/e1 ; echo rc=$?`
Expected: non-zero rc (parse error — grammar has no embedded production yet).

**IMPORTANT:** `scripts/run_golden.sh` will now count this as a FAIL. That is the intended TDD red state spanning Tasks 1–2. Do NOT run the full golden gate as a pass/fail criterion until Task 2 completes it; instead verify "exactly one new failure, name = embed_asi_base_probe":

Run: `bash scripts/run_golden.sh 2>/dev/null | tail -3`
Expected: `--- golden: 200 passed, 1 failed ---`

- [ ] **Step 3: Add the brace-context stack to the Lexer struct**

In `include/lexer.h`, append at the STRUCT TAIL (after `prev_token_type`):

```c
    // Struct-body-scoped ASI (struct embedding): one entry per currently-open
    // brace. asi_ctx[d] == 1 iff the '{' at depth d opened a struct body (it
    // immediately followed the `struct` keyword), in which case a newline
    // after a field-ending token inserts ';' so 1-token embedded fields
    // (`Base`) terminate at the line break. All other braces (enum, interface,
    // composite literals, blocks) are no-emit. Depths beyond the array are
    // treated as no-emit (depth still tracked for correct pops). Appended at
    // the struct tail per the no-header-deps convention.
    unsigned char asi_ctx[256];
    int asi_depth;
```

- [ ] **Step 4: Initialize, push, pop, and emit in lexer.c**

In `lexer_new` (find with `grep -n "lexer_new" src/lexer/lexer.c`), after `prev_token_type` is initialized, add:

```c
    lexer->asi_depth = 0;
    memset(lexer->asi_ctx, 0, sizeof(lexer->asi_ctx));
```

In `lexer_next_token`, `case '{':` (currently lexer.c:167–170) — classify BEFORE the token is created overwrites nothing (prev_token_type still holds the token before `{`):

```c
        case '{':
            token = token_new(TOKEN_LBRACE, "{", 1, current_pos);
            if (lexer->asi_depth >= 0 && lexer->asi_depth < (int)sizeof(lexer->asi_ctx)) {
                lexer->asi_ctx[lexer->asi_depth] =
                    (lexer->prev_token_type == TOKEN_STRUCT) ? 1 : 0;
            }
            lexer->asi_depth++;
            lexer_read_char(lexer);
            break;
        case '}':
            token = token_new(TOKEN_RBRACE, "}", 1, current_pos);
            if (lexer->asi_depth > 0) lexer->asi_depth--;
            lexer_read_char(lexer);
            break;
```

In the `case '\n':` block (lexer.c:135–156), AFTER the existing continuation-op insertion `if` and BEFORE the final `continue;`, add:

```c
            // Struct-body ASI (embedding): inside a struct body, a newline
            // after a field-ending token ends the field — Go's semicolon rule
            // scoped to struct bodies, so a 1-token embedded field (`Base`)
            // stops at the line break instead of absorbing the next line.
            if (lexer->asi_depth > 0 &&
                lexer->asi_depth <= (int)sizeof(lexer->asi_ctx) &&
                lexer->asi_ctx[lexer->asi_depth - 1] &&
                token_ends_value(lexer->prev_token_type)) {
                lexer->prev_token_type = TOKEN_SEMICOLON;
                return token_new(TOKEN_SEMICOLON, ";", 1, current_pos);
            }
```

- [ ] **Step 5: Rebuild from clean (header changed) and run regression gates**

Run: `make clean && make lexer && make test`
Expected: build OK; `All tests passed!` (76 pass / 1 skip).

Run: `bash scripts/run_golden.sh 2>/dev/null | tail -3`
Expected: `--- golden: 200 passed, 1 failed ---` — the ONLY failure is `embed_asi_base_probe` (compile). Existing multi-line structs now lex with `;` after each field and land on the pre-existing `identifier type SEMICOLON` productions; any OTHER failure means the emission condition is wrong — stop and fix.

- [ ] **Step 6: Commit**

```bash
git add include/lexer.h src/lexer/lexer.c examples/embed_asi_base_probe.goo examples/embed_asi_base_probe.expected.txt
git commit --no-gpg-sign -m "feat(lexer): struct-body-scoped ASI — newline ends a field inside struct bodies"
```

---

### Task 2: Grammar + AST — embedded field productions

**Files:**
- Modify: `include/ast.h` (VarDeclNode tail, :283–312)
- Modify: `src/ast/ast_constructors.c` (`ast_var_decl_new`, :120)
- Modify: `src/parser/parser.y` (prologue helper + `struct_field` productions, :1918–1969)
- Modify: `docs/superpowers/specs/2026-07-04-struct-embedding-design.md` (one-line refinement)
- Test: `examples/embed_asi_base_probe.goo` (from Task 1) now compiles green

**Interfaces:**
- Consumes: Task 1's `TOKEN_SEMICOLON` emission.
- Produces: `VarDeclNode.is_embedded` (int, 0/1) — set on embedded fields; field name == unqualified type name (`Base` for both `Base` and `*Base`); `field->type` is a `BasicTypeNode` or `PointerTypeNode` wrapping one. Tasks 3–6 key off `is_embedded`.

- [ ] **Step 1: Add `is_embedded` to VarDeclNode**

In `include/ast.h`, append at the VarDeclNode STRUCT TAIL (after `is_captured`, :311):

```c
    // Struct embedding: set by the parser on an anonymous (embedded) struct
    // field — `Base` or `*Base` inside a struct body. The field is otherwise
    // an ordinary named field (names[0] == the unqualified type name), so
    // layout/literals/explicit paths need no special cases; promotion logic
    // (src/types/embedding.c) keys off this flag. Appended at the STRUCT TAIL
    // per the no-header-deps convention above.
    int is_embedded;
```

In `src/ast/ast_constructors.c` `ast_var_decl_new` (:120), with the other field initializations, add:

```c
    node->is_embedded = 0;
```

(Also grep for any other place VarDeclNode is malloc'd raw and initialized field-by-field: `grep -rn "sizeof(VarDeclNode)" src/ | grep -v ast_constructors` — if parser.y or others build VarDeclNode without `ast_var_decl_new`, set `is_embedded = 0` there too. The four existing struct_field actions use `ast_var_decl_new`, so they are covered.)

- [ ] **Step 2: Add a prologue helper + the two embedded productions**

In `src/parser/parser.y`'s C prologue (the `%{ ... %}` block near the top, alongside other helpers like `reinterpret_grouped_names`), add:

```c
// Struct embedding: build the VarDeclNode for an anonymous field `Name` /
// `*Name`. The field is stored under the type's own name (Go's rule), with
// is_embedded set; the type node matches what type_name / pointer_type
// reductions would have built.
static ASTNode* make_embedded_field(ASTNode* ident_node, int is_pointer) {
    IdentifierNode* ident = (IdentifierNode*)ident_node;
    VarDeclNode* field = ast_var_decl_new(get_current_position());
    field->names = malloc(sizeof(char*));
    field->names[0] = strdup(ident->name);
    field->name_count = 1;
    BasicTypeNode* basic = (BasicTypeNode*)malloc(sizeof(BasicTypeNode));
    basic->base.type = AST_BASIC_TYPE;
    basic->base.pos = ident->base.pos;
    basic->base.node_type = NULL;
    basic->base.next = NULL;
    basic->name = strdup(ident->name);
    ASTNode* ty = (ASTNode*)basic;
    if (is_pointer) {
        PointerTypeNode* ptr = (PointerTypeNode*)malloc(sizeof(PointerTypeNode));
        ptr->base.type = AST_POINTER_TYPE;
        ptr->base.pos = ident->base.pos;
        ptr->base.node_type = NULL;
        ptr->base.next = NULL;
        ptr->element_type = ty;
        ty = (ASTNode*)ptr;
    }
    field->type = ty;
    field->values = NULL;
    field->is_embedded = 1;
    ast_node_free(ident_node);
    return (ASTNode*)field;
}
```

In the `struct_field:` rule (:1918), append two productions:

```yacc
    | identifier SEMICOLON {
        // Embedded (anonymous) field `Base;` — the ';' is explicit in
        // one-liners and ASI-inserted at newlines inside struct bodies.
        $$ = make_embedded_field($1, 0);
    }
    | MULTIPLY identifier SEMICOLON {
        // Embedded pointer field `*Base;`.
        $$ = make_embedded_field($2, 1);
    }
```

**Deliberately NO bare `identifier` production** (without `;`): it would S/R-conflict with `identifier type` on IDENT/MULTIPLY lookaheads. Consequence: a one-line `struct { Base }` must be written `struct { Base; }`. Multi-line structs need nothing — ASI supplies the `;`.

- [ ] **Step 3: Verify bison baseline unchanged**

Run: `bison -d -o /tmp/p.tab.c src/parser/parser.y 2>&1 | grep conflicts`
Expected: `81 shift/reduce` and `256 reduce/reduce`, exactly. Any drift: run `bison -Wcounterexamples ...`, capture the counterexamples, STOP and report.

- [ ] **Step 4: Build and verify the Task-1 probe goes green**

Run: `make lexer && bin/goo examples/embed_asi_base_probe.goo -o /tmp/e1 && /tmp/e1`
Expected: prints `42`.

(Why it works already with no typecheck changes: the parser names the embedded field `Base` with type `Base`, so `type_from_ast` builds an ordinary field and `o.Base.N` is plain two-level access.)

Run: `bash scripts/run_golden.sh 2>/dev/null | tail -3`
Expected: `--- golden: 201 passed, 0 failed ---`

Run: `make test`
Expected: `All tests passed!`

- [ ] **Step 5: Record the one-line refinement in the spec**

In `docs/superpowers/specs/2026-07-04-struct-embedding-design.md`, in the "Grammar delta" paragraph, replace the sentence about the bare-identifier production with:

```markdown
Embedded productions are **semicolon-terminated only** (`identifier SEMICOLON`,
`MUL identifier SEMICOLON`) — zero bison-conflict delta. The `;` is ASI-inserted
at newlines, so all multi-line Go source parses verbatim; the one refinement vs
Go is that a ONE-LINE `struct { Base }` must be written `struct { Base; }`
(same family as the documented `}; stmt` one-line quirk). A bare-identifier
production would cost ~2 justified S/R conflicts; deferred unless it bites.
```

- [ ] **Step 6: Commit**

```bash
git add include/ast.h src/ast/ast_constructors.c src/parser/parser.y docs/superpowers/specs/2026-07-04-struct-embedding-design.md
git commit --no-gpg-sign -m "feat(parser): embedded struct fields — Base; and *Base; productions, is_embedded flag"
```

---

### Task 3: Types — is_embedded plumbing + declaration constraints

**Files:**
- Modify: `include/types.h` (StructField, :213–219)
- Modify: `src/types/type_checker.c` (`type_from_ast` AST_STRUCT_TYPE case, :2197–2249; AST_ENUM_TYPE case :2251+)
- Modify: `Makefile` (`verify:` list + 4 new reject-probe targets)
- Test: reject probes below

**Interfaces:**
- Consumes: `VarDeclNode.is_embedded` (Task 2).
- Produces: `StructField.is_embedded` (int) — Tasks 4/6's resolver walks only flagged fields. Diagnostic strings (exact, used by probes):
  - `"embedded interface types are not yet supported"`
  - `"duplicate field name '%s' in struct"`
  - `"embedded field '%s' must be a named type or pointer to a named type"`
  - `"embedded fields are not supported in enum variants"`

- [ ] **Step 1: Add `is_embedded` to StructField**

In `include/types.h` (:213–219), append at the StructField tail:

```c
    // Struct embedding: 1 iff this member was declared anonymously (`Base` /
    // `*Base`). Promotion (src/types/embedding.c) recurses only into flagged
    // members. Appended at the tail — rebuild needs `make clean`.
    int is_embedded;
} StructField;
```

(i.e. the flag goes before the closing brace; keep existing members untouched.)

- [ ] **Step 2: Write the failing reject probes (Makefile targets)**

Append to `Makefile` (after the last existing reject-probe target, pattern copied from `boolnot-reject-probe` :499), and add all four names to the `verify:` dependency list (:1738):

```makefile
# Embedding: interface embedding is deferred — must reject cleanly, not crash.
embed-iface-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== embed-iface-reject-probe: embedded interface must reject ==="
	@printf 'package main\ntype I interface { M() int }\ntype S struct {\n\tI\n}\nfunc main(){ _ = S{} }\n' > build/embed_iface_reject.goo
	@rm -f build/embed_iface_reject
	@$(COMPILER) -o build/embed_iface_reject build/embed_iface_reject.goo > build/embed_iface_reject.out 2> build/embed_iface_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "embed-iface-reject-probe: FAIL (compiled rc=0)"; exit 1; fi; \
	if ! grep -q "embedded interface types are not yet supported" build/embed_iface_reject.err; then echo "embed-iface-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/embed_iface_reject.err; exit 1; fi; \
	echo "embed-iface-reject-probe: PASS (rejected rc=$$rc)"

# Embedding: duplicate member names (Base twice, or Base + field Base) reject.
embed-dup-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== embed-dup-reject-probe: duplicate embedded name must reject ==="
	@printf 'package main\ntype Base struct { N int }\ntype S struct {\n\tBase\n\t*Base\n}\nfunc main(){ _ = S{} }\n' > build/embed_dup_reject.goo
	@rm -f build/embed_dup_reject
	@$(COMPILER) -o build/embed_dup_reject build/embed_dup_reject.goo > build/embed_dup_reject.out 2> build/embed_dup_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "embed-dup-reject-probe: FAIL (compiled rc=0)"; exit 1; fi; \
	if ! grep -q "duplicate field name 'Base'" build/embed_dup_reject.err; then echo "embed-dup-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/embed_dup_reject.err; exit 1; fi; \
	echo "embed-dup-reject-probe: PASS (rejected rc=$$rc)"

# Embedding: only named types / pointers to named types can be embedded.
embed-badtype-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== embed-badtype-reject-probe: value-recursive embedding must reject, not hang ==="
	@printf 'package main\ntype A struct {\n\tB\n}\ntype B struct {\n\tA\n}\nfunc main(){ _ = A{} }\n' > build/embed_badtype_reject.goo
	@rm -f build/embed_badtype_reject
	@timeout 10 $(COMPILER) -o build/embed_badtype_reject build/embed_badtype_reject.goo > build/embed_badtype_reject.out 2> build/embed_badtype_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "embed-badtype-reject-probe: FAIL (compiled rc=0)"; exit 1; fi; \
	if [ $$rc -eq 124 ]; then echo "embed-badtype-reject-probe: FAIL (compiler hung on recursive embedding)"; exit 1; fi; \
	echo "embed-badtype-reject-probe: PASS (rejected rc=$$rc)"

# Embedding: enum variant bodies share struct_field_list; embedding there is out of scope.
embed-enum-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== embed-enum-reject-probe: embedded field in enum variant must reject ==="
	@printf 'package main\ntype Base struct { N int }\ntype E enum { V{Base;} }\nfunc main(){ }\n' > build/embed_enum_reject.goo
	@rm -f build/embed_enum_reject
	@$(COMPILER) -o build/embed_enum_reject build/embed_enum_reject.goo > build/embed_enum_reject.out 2> build/embed_enum_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "embed-enum-reject-probe: FAIL (compiled rc=0)"; exit 1; fi; \
	if ! grep -q "embedded fields are not supported in enum variants" build/embed_enum_reject.err; then echo "embed-enum-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/embed_enum_reject.err; exit 1; fi; \
	echo "embed-enum-reject-probe: PASS (rejected rc=$$rc)"
```

(Note: check the actual enum syntax used in the repo with `grep -rn "enum {" examples/*.goo | head -3` and adjust the enum probe source line to match real accepted syntax; the intent is a variant body containing `Base;`.)

Run each: `make embed-iface-reject-probe` etc.
Expected: ALL FAIL right now (the compiler currently accepts or mis-rejects these). That is the red state.

- [ ] **Step 3: Implement the constraints in type_from_ast**

In `src/types/type_checker.c`, AST_STRUCT_TYPE case (:2197–2249):

(a) Replace the silent skip at :2232:

```c
                if (fd->name_count == 0) {
                    type_error(checker, f->pos,
                               "internal: struct field with no name survived parsing");
                    free(result->data.struct_type.fields);
                    free(result);
                    return NULL;
                }
```

(b) After `Type* ft = ...` is built and null-checked (:2233–2238), add the embedded-type validation and flag copy:

```c
                if (fd->is_embedded) {
                    // Embedded member: must be a named type or pointer to one;
                    // interface embedding is a deferred feature, not an error
                    // of the user's making — say so specifically.
                    Type* base_t = ft;
                    if (base_t->kind == TYPE_POINTER)
                        base_t = base_t->data.pointer.pointee_type;
                    if (base_t && base_t->kind == TYPE_INTERFACE) {
                        type_error(checker, f->pos,
                                   "embedded interface types are not yet supported");
                        free(result->data.struct_type.fields);
                        free(result);
                        return NULL;
                    }
                    int named = base_t &&
                        ((base_t->kind == TYPE_STRUCT && base_t->data.struct_type.name) ||
                         base_t->name);
                    if (!named) {
                        type_error(checker, f->pos,
                                   "embedded field '%s' must be a named type or pointer to a named type",
                                   fd->names[0]);
                        free(result->data.struct_type.fields);
                        free(result);
                        return NULL;
                    }
                }
                result->data.struct_type.fields[idx].is_embedded = fd->is_embedded;
```

(c) After the field loop completes (before `result->size = total_size;`), add duplicate detection:

```c
            for (size_t a = 0; a < idx; a++) {
                for (size_t b = a + 1; b < idx; b++) {
                    if (strcmp(result->data.struct_type.fields[a].name,
                               result->data.struct_type.fields[b].name) == 0) {
                        type_error(checker, type_node->pos,
                                   "duplicate field name '%s' in struct",
                                   result->data.struct_type.fields[a].name);
                        free(result->data.struct_type.fields);
                        free(result);
                        return NULL;
                    }
                }
            }
```

(d) In the AST_ENUM_TYPE case (:2251+), where each variant's field chain is walked, reject flagged fields:

```c
                if (((VarDeclNode*)f)->is_embedded) {
                    type_error(checker, f->pos,
                               "embedded fields are not supported in enum variants");
                    /* free per the case's existing error path */
                    return NULL;
                }
```

(Adapt the free/cleanup lines to the case's existing error-path style — read the surrounding code first.)

(e) Recursion (probe `embed-badtype-reject-probe`): first RUN the probe. `type A struct { B }` / `type B struct { A }` most likely already errors as an unknown type (forward-ref rejection from the type-decl machinery). If it hangs instead: add a recursion guard to `type_from_ast` via a depth counter on TypeChecker (increment/decrement around the AST_STRUCT_TYPE case, error at depth > 64: `"invalid recursive type"`). Only add the guard if the probe demonstrates the hang.

- [ ] **Step 4: Clean rebuild, run the probes and gates**

Run: `make clean && make lexer && make test`
Expected: green.
Run: `make embed-iface-reject-probe embed-dup-reject-probe embed-badtype-reject-probe embed-enum-reject-probe`
Expected: all four PASS.
Run: `bash scripts/run_golden.sh 2>/dev/null | tail -1`
Expected: `201 passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add include/types.h src/types/type_checker.c Makefile
git commit --no-gpg-sign -m "feat(types): embedded-field constraints — named types only, dup names, deferred interfaces"
```

---

### Task 4: Resolver module + promoted-field desugar

**Files:**
- Create: `include/embedding.h`
- Create: `src/types/embedding.c`
- Modify: `Makefile` (add `build/types/embedding.o` to the object list — copy how `build/types/expression_checker.o` is listed/built)
- Modify: `src/types/expression_checker.c` (`type_check_selector_expr` struct branch, :3036–3059)
- Test: `examples/embed_field_probe.goo`, `examples/embed_ptr_probe.goo`, reject probe `embed-ambiguous-reject-probe`

**Interfaces:**
- Consumes: `StructField.is_embedded` (Task 3); `type_receiver_name` / `type_method_mangled_name` (src/types/types.c:783–802); `type_checker_lookup_variable`.
- Produces (consumed by Tasks 5–6):

```c
// include/embedding.h
#ifndef EMBEDDING_H
#define EMBEDDING_H

#include "types.h"
#include "type_checker.h"   // adjust to the header that declares TypeChecker

#define EMBED_MAX_DEPTH 8

typedef enum {
    EMBED_NOT_FOUND = 0,
    EMBED_FIELD,
    EMBED_METHOD,
    EMBED_AMBIGUOUS
} EmbedResultKind;

typedef struct {
    EmbedResultKind kind;
    // Field-name hops from the outer struct to the OWNER of the found member,
    // outermost first. Empty (len==0) never happens: direct members are the
    // caller's fast path, the resolver only reports promoted ones.
    const char* path[EMBED_MAX_DEPTH];
    size_t len;
    Type* type;          // FIELD: the field's type. METHOD: the mangled
                         // function's TYPE_FUNCTION (receiver = params[0]).
    Type* owner;         // the embedded type that directly owns the member
                         // (pointer already unwrapped) — Task 6 re-mangles
                         // against type_receiver_name(owner).
    char ambig_a[128];   // AMBIGUOUS only: two dotted paths for diagnostics,
    char ambig_b[128];   // e.g. "Base.X" / "Other.X".
} EmbedResult;

// BFS the embedding graph of `struct_type` for member `name` (field OR
// method), Go promotion rules: shallowest depth wins, outer shadows inner,
// >=2 hits at the winning depth => AMBIGUOUS. Direct (depth-0) members are
// NOT reported — callers handle those on their existing fast paths.
// Pointer-embedding cycles terminate via a visited set.
EmbedResult embedding_resolve(TypeChecker* checker, Type* struct_type,
                              const char* name);

#endif // EMBEDDING_H
```

- [ ] **Step 1: Write the failing golden probes**

`examples/embed_field_probe.goo` (verify expected with `go run`):

```go
package main

import "fmt"

type Inner struct {
	V int
}

type Mid struct {
	Inner
	M int
}

type Outer struct {
	Mid
	V int // shadows Inner.V at depth 2
}

func main() {
	o := Outer{}
	o.M = 10       // promoted from Mid (depth 1)
	o.V = 5        // direct field shadows Inner.V
	o.Inner.V = 7  // explicit path to the shadowed field
	fmt.Println(o.M)
	fmt.Println(o.V)
	fmt.Println(o.Mid.Inner.V)
	o.Mid.M = 11 // explicit path write
	fmt.Println(o.M)
}
```

`examples/embed_field_probe.expected.txt`:

```
10
5
7
11
```

`examples/embed_ptr_probe.goo`:

```go
package main

import "fmt"

type Base struct {
	N int
}

type Holder struct {
	*Base
	Tag int
}

func main() {
	b := Base{N: 3}
	h := Holder{Base: &b, Tag: 1}
	fmt.Println(h.N) // promoted read through the pointer
	h.N = 9          // promoted write through the pointer
	fmt.Println(b.N) // shared: the write hit b
	fmt.Println(h.Tag)
}
```

`examples/embed_ptr_probe.expected.txt`:

```
3
9
1
```

Note `o.Inner.V` in the field probe: `Inner` itself is reached by promotion (`o.Inner` == `o.Mid.Inner`) — embedded fields promote as fields too. This is intentional coverage.

Run: `bash scripts/run_golden.sh 2>/dev/null | tail -1`
Expected: `201 passed, 2 failed` (the two new probes; failures are compile errors "Struct has no field or method").

Also add the ambiguity reject probe to `Makefile` + `verify:` list:

```makefile
# Embedding: same-depth collision is an error only when the BARE name is used.
embed-ambiguous-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== embed-ambiguous-reject-probe: ambiguous promoted name must reject at use ==="
	@printf 'package main\nimport "fmt"\ntype A struct { X int }\ntype B struct { X int }\ntype S struct {\n\tA\n\tB\n}\nfunc main(){ s := S{}; s.A.X = 1; fmt.Println(s.X) }\n' > build/embed_ambig_reject.goo
	@rm -f build/embed_ambig_reject
	@$(COMPILER) -o build/embed_ambig_reject build/embed_ambig_reject.goo > build/embed_ambig_reject.out 2> build/embed_ambig_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "embed-ambiguous-reject-probe: FAIL (compiled rc=0 — ambiguous s.X accepted)"; exit 1; fi; \
	if ! grep -q "ambiguous selector 'X'" build/embed_ambig_reject.err; then echo "embed-ambiguous-reject-probe: FAIL (wrong/missing diagnostic)"; cat build/embed_ambig_reject.err; exit 1; fi; \
	echo "embed-ambiguous-reject-probe: PASS (rejected rc=$$rc)"
	@echo "=== declaration alone must stay LEGAL (Go rule) ==="
	@printf 'package main\nimport "fmt"\ntype A struct { X int }\ntype B struct { X int }\ntype S struct {\n\tA\n\tB\n}\nfunc main(){ s := S{}; s.A.X = 4; fmt.Println(s.A.X) }\n' > build/embed_ambig_ok.goo
	@$(COMPILER) -o build/embed_ambig_ok build/embed_ambig_ok.goo 2> build/embed_ambig_ok.err || (echo "embed-ambiguous-reject-probe: FAIL (declaring ambiguous struct rejected — should be use-site only)"; cat build/embed_ambig_ok.err; exit 1)
	@out=$$(build/embed_ambig_ok); [ "$$out" = "4" ] || (echo "embed-ambiguous-reject-probe: FAIL (explicit path broken)"; exit 1)
	@echo "embed-ambiguous-reject-probe: PASS (explicit path fine)"
```

- [ ] **Step 2: Implement the resolver**

Create `include/embedding.h` exactly as in the Interfaces block above (fix the TypeChecker header include to whatever `expression_checker.c` includes — check with `head -20 src/types/expression_checker.c`).

Create `src/types/embedding.c`:

```c
// Struct embedding promotion resolver (spec:
// docs/superpowers/specs/2026-07-04-struct-embedding-design.md).
// Single owner of Go's promotion rules: BFS by depth over embedded members,
// shallowest unique hit wins, ties at the winning depth are ambiguous.
#include "embedding.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define EMBED_MAX_QUEUE 64
#define EMBED_MAX_VISITED 64

typedef struct {
    Type* type;                        // struct type at this node
    const char* path[EMBED_MAX_DEPTH]; // field-name hops to reach it
    size_t len;
} QueueEntry;

// The nameable identity of a type for the visited set / method mangling.
static const char* embed_type_name(Type* t) {
    if (!t) return NULL;
    if (t->kind == TYPE_STRUCT) return t->data.struct_type.name;
    return t->name;
}

// Unwrap *T to T for descending into an embedded pointer member.
static Type* embed_deref(Type* t) {
    if (t && t->kind == TYPE_POINTER) return t->data.pointer.pointee_type;
    return t;
}

static void embed_format_path(char* out, size_t cap,
                              const char* const* path, size_t len,
                              const char* leaf) {
    size_t off = 0;
    out[0] = '\0';
    for (size_t i = 0; i < len; i++) {
        int n = snprintf(out + off, cap - off, "%s.", path[i]);
        if (n < 0 || (size_t)n >= cap - off) return;
        off += (size_t)n;
    }
    snprintf(out + off, cap - off, "%s", leaf);
}

// Look for `name` DIRECTLY on `t` (fields first, then method T__name).
// Returns 1 and fills member_type/is_method on a hit.
static int embed_direct_member(TypeChecker* checker, Type* t, const char* name,
                               Type** member_type, int* is_method) {
    if (t->kind == TYPE_STRUCT) {
        for (size_t i = 0; i < t->data.struct_type.field_count; i++) {
            StructField* f = &t->data.struct_type.fields[i];
            if (f->name && strcmp(f->name, name) == 0) {
                *member_type = f->type;
                *is_method = 0;
                return 1;
            }
        }
    }
    const char* tn = embed_type_name(t);
    if (tn) {
        char* mangled = type_method_mangled_name(tn, name);
        Variable* m = mangled ? type_checker_lookup_variable(checker, mangled) : NULL;
        free(mangled);
        if (m && m->type && m->type->kind == TYPE_FUNCTION) {
            *member_type = m->type;
            *is_method = 1;
            return 1;
        }
    }
    return 0;
}

EmbedResult embedding_resolve(TypeChecker* checker, Type* struct_type,
                              const char* name) {
    EmbedResult res;
    memset(&res, 0, sizeof(res));
    res.kind = EMBED_NOT_FOUND;
    if (!checker || !struct_type || struct_type->kind != TYPE_STRUCT || !name)
        return res;

    QueueEntry queue[EMBED_MAX_QUEUE];
    size_t head = 0, tail = 0;
    // Visited set WITH depths: a type may be reached again at the SAME depth
    // (diamond embedding — Go counts that as two members at that depth, i.e.
    // ambiguous), but never at a deeper one (that's a cycle, or shadowed by
    // the shallower occurrence either way).
    const char* visited[EMBED_MAX_VISITED];
    size_t visited_depth[EMBED_MAX_VISITED];
    size_t visited_count = 0;

    queue[tail].type = struct_type;
    queue[tail].len = 0;
    tail++;
    const char* rootname = embed_type_name(struct_type);
    if (rootname && visited_count < EMBED_MAX_VISITED) {
        visited[visited_count] = rootname;
        visited_depth[visited_count] = 0;
        visited_count++;
    }

    size_t hit_depth = 0;
    int hits = 0;

    while (head < tail) {
        // Process one full BFS LEVEL at a time so same-depth ties are seen
        // together before any deeper level is explored.
        size_t level_end = tail;
        size_t depth = queue[head].len; // all entries in [head, level_end) share it
        for (size_t q = head; q < level_end; q++) {
            Type* t = queue[q].type;
            // Depth 0 is the outer struct itself: its direct members are the
            // caller's fast path — only descend, don't match.
            if (queue[q].len > 0) {
                Type* mt = NULL;
                int ism = 0;
                if (embed_direct_member(checker, t, name, &mt, &ism)) {
                    hits++;
                    if (hits == 1) {
                        hit_depth = queue[q].len;
                        res.kind = ism ? EMBED_METHOD : EMBED_FIELD;
                        res.type = mt;
                        res.owner = t;
                        // Path to the OWNER: all hops (the member itself is
                        // resolved at the last hop's type).
                        res.len = queue[q].len;
                        for (size_t i = 0; i < res.len; i++)
                            res.path[i] = queue[q].path[i];
                        embed_format_path(res.ambig_a, sizeof(res.ambig_a),
                                          queue[q].path, queue[q].len, name);
                    } else if (queue[q].len == hit_depth) {
                        res.kind = EMBED_AMBIGUOUS;
                        embed_format_path(res.ambig_b, sizeof(res.ambig_b),
                                          queue[q].path, queue[q].len, name);
                        return res;
                    }
                    continue; // a hit shadows everything below it — don't descend
                }
            }
            if (hits > 0 && queue[q].len >= hit_depth)
                continue; // deeper than the winning depth — irrelevant
            if (t->kind != TYPE_STRUCT || queue[q].len >= EMBED_MAX_DEPTH)
                continue;
            for (size_t i = 0; i < t->data.struct_type.field_count; i++) {
                StructField* f = &t->data.struct_type.fields[i];
                if (!f->is_embedded) continue;
                Type* child = embed_deref(f->type);
                if (!child) continue;
                const char* cn = embed_type_name(child);
                size_t child_depth = queue[q].len + 1;
                int seen_shallower = 0;
                for (size_t v = 0; v < visited_count; v++) {
                    if (cn && visited[v] && strcmp(visited[v], cn) == 0 &&
                        visited_depth[v] < child_depth) {
                        seen_shallower = 1;
                        break;
                    }
                }
                if (seen_shallower) continue; // cycle or shadowed — don't descend
                if (cn && visited_count < EMBED_MAX_VISITED) {
                    visited[visited_count] = cn;
                    visited_depth[visited_count] = child_depth;
                    visited_count++;
                }
                if (tail >= EMBED_MAX_QUEUE) continue; // bounded; silently deep
                queue[tail].type = child;
                queue[tail].len = queue[q].len + 1;
                for (size_t p = 0; p < queue[q].len; p++)
                    queue[tail].path[p] = queue[q].path[p];
                queue[tail].path[queue[q].len] = f->name;
                tail++;
            }
        }
        head = level_end;
        if (hits > 0 && depth >= hit_depth) break; // winning level fully scanned
    }
    return res;
}
```

Add `build/types/embedding.o` to the Makefile: find the line listing `build/types/expression_checker.o` in the object list(s) (`grep -n "expression_checker.o" Makefile`) and add `build/types/embedding.o` alongside it in every list where the pattern appears; the generic `build/types/%.o: src/types/%.c` pattern rule (verify it exists: `grep -n "build/types/%.o" Makefile`) covers compilation.

- [ ] **Step 3: Wire the desugar into type_check_selector_expr**

In `src/types/expression_checker.c`, add `#include "embedding.h"` with the other includes. Add the wrap helper above `type_check_selector_expr`:

```c
// Struct embedding desugar: rewrite `o.X` into `(o.Hop1.Hop2).X` in place, so
// every downstream consumer (lvalue addressing, method receiver auto-address,
// pointer auto-deref) sees only constructs that already ship. Promotion in Go
// is DEFINED as this sugar — the rewrite is the spec, executed.
static ASTNode* embed_wrap_base(ASTNode* base, const EmbedResult* r, Position pos) {
    for (size_t i = 0; i < r->len; i++) {
        SelectorExprNode* s = (SelectorExprNode*)malloc(sizeof(SelectorExprNode));
        s->base.type = AST_SELECTOR_EXPR;
        s->base.pos = pos;
        s->base.node_type = NULL;
        s->base.next = NULL;
        s->expr = base;
        s->selector = strdup(r->path[i]);
        base = (ASTNode*)s;
    }
    return base;
}
```

In the struct branch, REPLACE the final rejection (`type_error(checker, expr->pos, "Struct has no field or method '%s'", ...); return NULL;` at :3058–3059) with:

```c
        EmbedResult er = embedding_resolve(checker, struct_type, selector->selector);
        if (er.kind == EMBED_FIELD || er.kind == EMBED_METHOD) {
            selector->expr = embed_wrap_base(selector->expr, &er, expr->pos);
            // Re-resolve: each inserted hop is a real (embedded) field, and
            // the leaf is now a direct member of its owner.
            return type_check_selector_expr(checker, expr);
        }
        if (er.kind == EMBED_AMBIGUOUS) {
            type_error(checker, expr->pos,
                       "ambiguous selector '%s' (found via %s and %s)",
                       selector->selector, er.ambig_a, er.ambig_b);
            return NULL;
        }
        type_error(checker, expr->pos, "Struct has no field or method '%s'", selector->selector);
        return NULL;
```

- [ ] **Step 4: Clean rebuild (types.h changed in Task 3's tree; embedding.h is new), run gates**

Run: `make clean && make lexer && make test`
Expected: green.
Run: `bin/goo examples/embed_field_probe.goo -o /tmp/ef && /tmp/ef`
Expected: `10 5 7 11` on four lines.
Run: `bin/goo examples/embed_ptr_probe.goo -o /tmp/ep && /tmp/ep`
Expected: `3 9 1` on three lines.
Run: `make embed-ambiguous-reject-probe`
Expected: PASS (both halves).
Run: `bash scripts/run_golden.sh 2>/dev/null | tail -1`
Expected: `203 passed, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add include/embedding.h src/types/embedding.c src/types/expression_checker.c Makefile examples/embed_field_probe.* examples/embed_ptr_probe.*
git commit --no-gpg-sign -m "feat(types): embedding resolver + promoted-field desugar (BFS, shallowest-unique, ambiguity at use)"
```

---

### Task 5: Promoted method calls

**Files:**
- Test only (expected): `examples/embed_method_probe.goo` + `.expected.txt`. The Task-4 desugar already rewrites method receivers (`EMBED_METHOD` takes the same wrap path); this task PROVES the method dimension and fixes any receiver fallout.

**Interfaces:**
- Consumes: Task 4's desugar; existing receiver auto-address/auto-deref in `src/codegen/call_codegen.c:985–1041`.
- Produces: promoted method calls compile with correct receiver semantics (mutation through pointer receivers reaches the embedded struct inside the outer value).

- [ ] **Step 1: Write the (possibly already passing) golden probe**

`examples/embed_method_probe.goo` (verify expected with `go run`):

```go
package main

import "fmt"

type Counter struct {
	N int
}

func (c *Counter) Inc() {
	c.N++
}

func (c Counter) Get() int {
	return c.N
}

type Words []string

func (w Words) Len() int {
	return len(w)
}

type Server struct {
	Counter
	Name string
}

type App struct {
	*Counter
	Words
}

func main() {
	s := Server{}
	s.Inc() // pointer-receiver method through VALUE embedding: must mutate s.Counter
	s.Inc()
	fmt.Println(s.Get())       // value receiver, promoted
	fmt.Println(s.Counter.N)   // proof the mutation landed inside s

	c := Counter{N: 10}
	a := App{Counter: &c, Words: Words{"a", "b", "c"}}
	a.Inc() // through POINTER embedding: mutates c
	fmt.Println(c.N)
	fmt.Println(a.Len()) // promoted method on embedded named non-struct type
}
```

`examples/embed_method_probe.expected.txt`:

```
2
2
11
3
```

- [ ] **Step 2: Run it — diagnose only if red**

Run: `make lexer && bin/goo examples/embed_method_probe.goo -o /tmp/em && /tmp/em`
Expected: the four lines above. This SHOULD pass with zero code changes: after desugar, `s.Inc()` is exactly `s.Counter.Inc()`, and pointer-receiver auto-addressing of a field lvalue shipped in #91.

**Probe pre-check:** the named-slice part (`Words{...}` literal, `len(w)` on a named slice) exercises named non-struct types beyond what is verified to ship. BEFORE blaming embedding, compile a scratch probe using `Words` WITHOUT any embedding (`w := Words{"a","b","c"}; fmt.Println(w.Len())`). If THAT fails, the gap is pre-existing named-type surface, not this feature: swap the `Words` portion of the probe for a named basic type that works (e.g. `type Score int` with `func (s Score) Doubled() int { return int(s) * 2 }`, embedded as `Score` in `App`, constructed via `Score(21)`), regenerate `.expected.txt` with `go run`, and note the pre-existing gap in the task report.

If it fails: the receiver-side desugar has a gap. Debug with the two-step form (`s.Counter.Inc()` directly in a scratch probe under `/tmp/claude-1000/-data-Workspace-github-com-goolang/1ed8be7c-3d34-4d33-a2b1-98824c7dc6a1/scratchpad/`) to separate "promotion rewrote wrong" from "explicit path was already broken", and report findings to the controller before changing code — do not guess-fix codegen.

- [ ] **Step 3: Full gates**

Run: `make test && bash scripts/run_golden.sh 2>/dev/null | tail -1`
Expected: tests green; `204 passed, 0 failed`.

- [ ] **Step 4: Commit**

```bash
git add examples/embed_method_probe.goo examples/embed_method_probe.expected.txt
git commit --no-gpg-sign -m "test(golden): promoted method calls — value/pointer receivers through value/pointer embedding"
```

---

### Task 6: Interface satisfaction + vtable thunks for promoted methods

**Files:**
- Modify: `src/types/type_checker.c` (`type_interface_satisfied`, :785–824)
- Modify: `src/codegen/interface_codegen.c` (`build_thunk`, :52–115)
- Test: `examples/embed_iface_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: `embedding_resolve` (Task 4); `codegen_get_struct_type` (src/codegen/type_mapping.c:158 — verify exact signature with `grep -n "codegen_get_struct_type" include/codegen.h src/codegen/type_mapping.c`).
- Produces: a concrete struct type satisfies an interface via promoted methods, and interface dispatch routes through a thunk that GEP-walks the embedding path.

- [ ] **Step 1: Write the failing golden probe**

`examples/embed_iface_probe.goo` (verify expected with `go run`):

```go
package main

import "fmt"

type Greeter interface {
	Greet() string
}

type Base struct {
	Name string
}

func (b Base) Greet() string {
	return "hi " + b.Name
}

type English struct {
	Base
	Loud bool
}

type Direct struct {
	Word string
}

func (d Direct) Greet() string {
	return d.Word
}

func main() {
	gs := []Greeter{English{Base: Base{Name: "ann"}, Loud: true}, Direct{Word: "yo"}}
	for _, g := range gs {
		fmt.Println(g.Greet())
	}
}
```

`examples/embed_iface_probe.expected.txt`:

```
hi ann
yo
```

Run: `bin/goo examples/embed_iface_probe.goo -o /tmp/ei ; echo rc=$?`
Expected: FAIL — "does not implement" diagnostic (satisfaction only sees direct `English__Greet`).

- [ ] **Step 2: Teach type_interface_satisfied the promoted fallback**

In `src/types/type_checker.c`, add `#include "embedding.h"`. In `type_interface_satisfied` (:785), replace the missing-method rejection (:798–800):

```c
        if (!mv || !mv->type || mv->type->kind != TYPE_FUNCTION) {
            // Not directly declared — promoted method via embedding?
            Type* impl_via_embed = NULL;
            if (concrete->kind == TYPE_STRUCT) {
                EmbedResult er = embedding_resolve(checker, concrete, im->name);
                if (er.kind == EMBED_METHOD) impl_via_embed = er.type;
            }
            if (!impl_via_embed) {
                *method_out = im->name; *reason_out = "missing"; return 0;
            }
            mv = NULL; // signature compare below uses `impl` directly
            impl = impl_via_embed;
        } else {
            impl = mv->type;
        }
```

and adjust the surrounding code: the current line `Type* impl = mv->type;` (:805) moves into the else-branch as shown (declare `Type* impl = NULL;` before the if). The signature-compare block (:807–821) is unchanged — it operates on `impl`, and a promoted method's receiver is still params[0], so the `+1` arithmetic holds.

- [ ] **Step 3: Teach build_thunk the promoted fallback**

In `src/codegen/interface_codegen.c`, add `#include "embedding.h"`. In `build_thunk` (:52):

(a) Replace the resolution block (:66–75) so a direct miss consults the resolver:

```c
    // Resolve the real method T__m and its registered receiver kind. If T
    // doesn't declare it directly, it is a PROMOTED method: resolve the
    // embedding path and re-mangle against the owning embedded type.
    EmbedResult epath;
    memset(&epath, 0, sizeof(epath));
    char* mangled = type_method_mangled_name(concrete_name, im->name);
    LLVMValueRef real_fn = mangled ? LLVMGetNamedFunction(codegen->module, mangled) : NULL;
    Variable* mvar = mangled ? type_checker_lookup_variable(checker, mangled) : NULL;
    free(mangled);
    if (!real_fn && concrete->kind == TYPE_STRUCT) {
        EmbedResult er = embedding_resolve(checker, concrete, im->name);
        if (er.kind == EMBED_METHOD) {
            epath = er;
            const char* otn = type_receiver_name(er.owner);
            char* om = otn ? type_method_mangled_name(otn, im->name) : NULL;
            real_fn = om ? LLVMGetNamedFunction(codegen->module, om) : NULL;
            mvar = om ? type_checker_lookup_variable(checker, om) : NULL;
            free(om);
        }
    }
    if (!real_fn) {
        codegen_error(codegen, (Position){0},
                      "internal: missing method implementation for interface thunk");
        return NULL;
    }
```

(b) Replace the receiver block (:91–99) with a path walk. `data` is a pointer to the boxed concrete value (`*Outer`); GEP hop-by-hop to a pointer to the embedded owner, loading through pointer-embedded hops:

```c
    // Receiver: walk the embedding path (empty for direct methods) from the
    // boxed *concrete to a pointer to the method's owner, loading through
    // pointer-embedded hops. Then pointer receivers take that pointer, value
    // receivers load through it.
    LLVMValueRef recv_ptr = data;
    Type* cur = concrete;
    for (size_t h = 0; h < epath.len; h++) {
        unsigned fidx = 0;
        int found = 0;
        for (size_t fi = 0; fi < cur->data.struct_type.field_count; fi++) {
            if (strcmp(cur->data.struct_type.fields[fi].name, epath.path[h]) == 0) {
                fidx = (unsigned)fi;
                found = 1;
                break;
            }
        }
        if (!found) {
            codegen_error(codegen, (Position){0},
                          "internal: embedding hop '%s' not found building thunk",
                          epath.path[h]);
            LLVMPositionBuilderAtEnd(codegen->builder, saved);
            return NULL;
        }
        LLVMTypeRef cur_llvm = codegen_get_struct_type(codegen, cur);
        recv_ptr = LLVMBuildStructGEP2(codegen->builder, cur_llvm, recv_ptr, fidx, "embed.hop");
        Type* ft = cur->data.struct_type.fields[fidx].type;
        if (ft->kind == TYPE_POINTER) {
            recv_ptr = LLVMBuildLoad2(codegen->builder, iface_ptr_type(codegen),
                                      recv_ptr, "embed.load");
            cur = ft->data.pointer.pointee_type;
        } else {
            cur = ft;
        }
    }
    if (ptr_recv) {
        call_args[0] = recv_ptr;
    } else {
        LLVMTypeRef llvm_owner = codegen_type_to_llvm(codegen, cur);
        call_args[0] = llvm_owner
                           ? LLVMBuildLoad2(codegen->builder, llvm_owner, recv_ptr, "recv")
                           : recv_ptr;
    }
```

Note the emission-order constraint: the resolution in (a) happens BEFORE `LLVMPositionBuilderAtEnd(codegen->builder, entry)`; the walk in (b) happens AFTER it (it emits instructions). Keep (a) where the old resolution block sat; (b) replaces the old receiver lines in the body-emission section. `saved` is already in scope from :82. If `codegen_get_struct_type`'s real name/signature differs, adapt (grep first, per Interfaces).

- [ ] **Step 4: Gates**

Run: `make lexer && bin/goo examples/embed_iface_probe.goo -o /tmp/ei && /tmp/ei`
Expected: `hi ann` then `yo`.
Run: `make test && bash scripts/run_golden.sh 2>/dev/null | tail -1`
Expected: green; `205 passed, 0 failed`.
Run: `make iface-satisfaction-probe iface-parse-probe`
Expected: both PASS (existing interface machinery unregressed).

- [ ] **Step 5: Commit**

```bash
git add src/types/type_checker.c src/codegen/interface_codegen.c examples/embed_iface_probe.*
git commit --no-gpg-sign -m "feat(interfaces): promoted methods satisfy interfaces — resolver fallback + path-walking thunks"
```

---

### Task 7: Edge probes, full verification, docs

**Files:**
- Create: `examples/embed_asi_edge_probe.goo` + `.expected.txt`
- Modify: `Makefile` (`embed-literal-reject-probe` + `verify:` list)
- Modify: `.handoff.md`

**Interfaces:**
- Consumes: everything above.
- Produces: the shipped feature's guardrails; updated handoff.

- [ ] **Step 1: ASI edge golden probe**

`examples/embed_asi_edge_probe.goo` (verify with `go run` — note the one-line struct uses `;` which is ALSO valid Go):

```go
package main

import "fmt"

type A struct {
	N int
}

type Edge struct {
	// comment between fields
	A

	Pair struct {
		X int
		Y int
	}
	Tail int // trailing comment after a field
}

type OneLine struct{ A; Z int }

func main() {
	e := Edge{}
	e.N = 1
	e.Pair.X = 2
	e.Tail = 3
	fmt.Println(e.N + e.Pair.X + e.Tail)
	o := OneLine{}
	o.N = 40
	o.Z = 2
	fmt.Println(o.N + o.Z)
}
```

`examples/embed_asi_edge_probe.expected.txt`:

```
6
42
```

This pins: comment lines and blank lines inside struct bodies don't break ASI; a nested anonymous `struct{...}` field type parses (inner braces are also struct-context — harmless); one-line embedding with explicit `;` works.

- [ ] **Step 2: Composite-literal promoted-name reject probe**

Append to `Makefile` + `verify:` list:

```makefile
# Embedding: promoted names are NOT valid composite-literal keys (Go rule) —
# only the embedded type's own name is. Pins the literal checker's flat scan.
embed-literal-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== embed-literal-reject-probe: promoted name as literal key must reject ==="
	@printf 'package main\ntype Base struct { N int }\ntype S struct {\n\tBase\n}\nfunc main(){ _ = S{N: 1} }\n' > build/embed_lit_reject.goo
	@rm -f build/embed_lit_reject
	@$(COMPILER) -o build/embed_lit_reject build/embed_lit_reject.goo > build/embed_lit_reject.out 2> build/embed_lit_reject.err; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "embed-literal-reject-probe: FAIL (compiled rc=0 — S{N:1} accepted)"; exit 1; fi; \
	echo "embed-literal-reject-probe: PASS (rejected rc=$$rc)"
	@echo "=== keyed embedded-type name must stay LEGAL ==="
	@printf 'package main\nimport "fmt"\ntype Base struct { N int }\ntype S struct {\n\tBase\n}\nfunc main(){ s := S{Base: Base{N: 7}}; fmt.Println(s.N) }\n' > build/embed_lit_ok.goo
	@$(COMPILER) -o build/embed_lit_ok build/embed_lit_ok.goo 2> build/embed_lit_ok.err || (echo "embed-literal-reject-probe: FAIL (keyed Base literal rejected)"; cat build/embed_lit_ok.err; exit 1)
	@out=$$(build/embed_lit_ok); [ "$$out" = "7" ] || (echo "embed-literal-reject-probe: FAIL (keyed literal wrong value: $$out)"; exit 1)
	@echo "embed-literal-reject-probe: PASS (keyed Base literal fine)"
```

Run: `make embed-literal-reject-probe`
Expected: PASS with no compiler changes (the literal checker scans direct fields only). If S{N:1} compiles, the literal checker consults promotion somewhere — report to controller; the fix direction is to keep literals on the flat scan.

- [ ] **Step 3: Full verification sweep**

Run, each to completion, reading real exit status (NO piped `$?`):

```bash
make clean && make lexer
make test
bash scripts/run_golden.sh 2>/dev/null | tail -1     # expect: 206 passed, 0 failed
make verify                                          # full probe chain incl. all new reject probes
make ccomp-link 2>/dev/null || true                  # if the target exists, expect PASS (check: grep -n "ccomp-link" Makefile)
bison -d -o /tmp/p.tab.c src/parser/parser.y 2>&1 | grep conflicts   # expect 81 + 256
```

All must be green. `make verify` is the pre-push gate — it must pass BEFORE the branch is pushed.

- [ ] **Step 4: Update .handoff.md**

Rewrite the NEXT QUEUE section: struct embedding SHIPPED (this branch); promote "func-typed map values" to #1; keep slice-write bounds + smalls; note new capability (embedding) + the one-line `struct { Base; }` refinement + follow-ups recorded in the spec (embedded interfaces, bare-identifier production if it bites, full ASI reform pilot proven).

- [ ] **Step 5: Commit**

```bash
git add examples/embed_asi_edge_probe.* Makefile .handoff.md
git commit --no-gpg-sign -m "test(embedding): ASI edge probes + literal-key reject; update handoff"
```

---

## Execution notes (controller)

- Branch: `feat/struct-embedding` off up-to-date `main` (`git fetch origin && git checkout -b feat/struct-embedding origin/main`).
- Per the session's model split: tasks execute via Sonnet subagents; the Fable controller reviews each task's diff and runs independent probes in the main loop before proceeding (the #104–#107 pattern that caught four real bugs).
- Tasks are strictly ordered (1→7); Task 5 is expected to be test-only but is a real reviewer gate — do not fold it into Task 4.
- After Task 7: push branch, open PR per repo convention. GH Actions is intentionally disabled; local `make verify` + `make test` are the authoritative gates.
