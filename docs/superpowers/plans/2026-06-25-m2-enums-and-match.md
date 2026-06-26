# Enums + `match` (M2 Gate) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add payload-carrying tagged unions (`type Expr enum {...}`) and lower the already-parsed `match` so a Goo program can build and walk a tagged-union mini-AST — satisfying the M2 gate.

**Architecture:** Enums ride the existing `type_decl → type` chain: a new `enum_type` type-expression mirrors `struct_type`. Each variant is a named struct (payload). A `Type` of new kind `TYPE_ENUM` carries `{name, variants[]}`; each variant carries a `TYPE_STRUCT` payload + a discriminant tag. LLVM layout is `{ i32 tag, [N x i8] payload }` (N = max variant payload size); construction and `match` go through memory (alloca + GEP + bitcast), consistent with existing struct field-access codegen. `match` is statement-style (no value), dispatches on the tag via an LLVM `switch`, binds payload fields positionally inside each arm, and is exhaustiveness-checked.

**Tech Stack:** C23, Bison/Flex grammar (`parser.y`), LLVM-C API (LLVM 22) codegen, Makefile build, `.goo` probe + `.expected.txt` diff gates.

## Global Constraints

- Language standard: **C23** (matches the rest of the project).
- New compiler `.c` files must be added to a module source list in the `Makefile` (`LEXER_SRCS`/`PARSER_SRCS`/`AST_SRCS`/`TYPES_SRCS`/`CODEGEN_SRCS`); the generic `%.o` rule then compiles them. **This plan adds no new `.c` files** — all changes extend existing files, so no source-list edits are needed.
- Build both artifacts before running any probe: `make goo` (compiler) **and** `make lib/libgoo_runtime.a` (runtime; not rebuilt by `make goo`).
- Emitted binaries link with `-no-pie` already handled by the compiler driver.
- Every new probe is wired into the `verify` aggregate (`Makefile:457`) **and** `.github/workflows/tests.yml`.
- No existing probe/test may be weakened; new capability adds gates only.
- Conventional commits, imperative mood, atomic. End commit messages with the `Co-Authored-By` trailer used in this repo.
- Enum AST node-kind enum entries that could shift existing values must be appended at the **tail** of the `ASTNodeType` enum (mirrors the `AST_STRUCT_LITERAL` comment at `include/ast.h`).
- Surface decisions (locked in the design spec): variant declaration is keyed struct fields; **enum-value construction** supports keyed or positional struct-literal form (rides existing struct-literal codegen); **match payload binding is positional** (`case Add{l, r}:`), riding the existing `expression_list` destructure grammar — keyed pattern binding is out of scope. `match` is statement-style. Exhaustiveness is required unless a `default`/wildcard arm is present.

---

## File map (what each task touches)

- `include/token.h`, `src/lexer/token.c` — `TOKEN_ENUM` keyword (Task 2).
- `src/parser/parser.y` — `enum_type`/`enum_variant_list`/`enum_variant` productions, `ENUM` token, `%type` decls (Task 3).
- `include/ast.h`, `src/ast/ast_constructors.c` (+ `src/ast/ast.c` free/visit if present) — `AST_ENUM_TYPE`, `AST_ENUM_VARIANT`, `EnumTypeNode`, `EnumVariantNode`, constructors (Task 3).
- `include/types.h` — `TYPE_ENUM`, `enum_type` union member, `EnumVariant` struct (Task 4).
- `src/types/type_checker.c` — `type_from_ast` `AST_ENUM_TYPE` case; variant registration in `type_check_type_decl` (Task 4).
- `src/types/expression_checker.c` — variant path in `type_check_struct_literal`; `type_check_expression` `AST_MATCH_EXPR` case (Tasks 5, 8).
- `src/codegen/type_mapping.c` — `codegen_get_enum_type` + `TYPE_ENUM` in `codegen_type_to_llvm` (Task 6).
- `src/codegen/composite_codegen.c` — variant path in `codegen_generate_struct_lit` (Task 6).
- `src/codegen/expression_codegen.c` — `AST_MATCH_EXPR` case in `codegen_generate_expression` (Task 9).
- `src/codegen/statement_codegen.c` or new fn in `composite_codegen.c` — `match` lowering (Task 9).
- `examples/enum_ctor_probe.goo` + `.expected.txt`, `examples/match_probe.goo` + `.expected.txt` — gates (Tasks 1, 7).
- `Makefile`, `.github/workflows/tests.yml` — probe targets + CI wiring (Tasks 6, 9).

---

## Task 1: Write the failing enum-construction probe (RED)

This is the first TDD checkpoint: a program that *declares* an enum and *constructs* variant values. It is observable via exit code / printed output without `match`, so it isolates the enum decl + construction pipeline from match lowering.

**Files:**
- Create: `examples/enum_ctor_probe.goo`
- Create: `examples/enum_ctor_probe.expected.txt`

**Interfaces:**
- Produces: the surface syntax the rest of the plan must parse/lower — `type <Name> enum { <Variant>{<fields>} ... }` declarations and keyed variant construction `<Variant>{field: value}`.

- [ ] **Step 1: Write the probe**

`examples/enum_ctor_probe.goo`:
```goo
// enum_ctor_probe: exercises enum declaration + variant construction only
// (no match yet). Proves: type Expr enum {...} parses, a variant literal
// constructs, and the program compiles+links+runs. Each construct prints a
// PASS line.
package main

import "fmt"

type Shape enum {
    Circle{radius: int}
    Rect{w: int, h: int}
}

func main() {
    var c Shape = Circle{radius: 5}
    var r Shape = Rect{w: 3, h: 4}
    // We cannot inspect payloads without match yet; constructing and
    // assigning to an enum-typed variable is the observable behavior.
    _ = c
    _ = r
    fmt.Println("PASS: enum declared")
    fmt.Println("PASS: variants constructed")
}
```

`examples/enum_ctor_probe.expected.txt`:
```
PASS: enum declared
PASS: variants constructed
```

- [ ] **Step 2: Verify it fails today**

Run: `make goo && ./bin/goo -o build/enum_ctor_probe examples/enum_ctor_probe.goo`
Expected: FAIL — a parse/syntax error on the `enum` keyword (it is not yet a token), e.g. `unexpected token` near `enum`.

- [ ] **Step 3: Commit the RED probe**

```bash
git add examples/enum_ctor_probe.goo examples/enum_ctor_probe.expected.txt
git commit -m "test(enum): add failing enum construction probe

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Add the `enum` keyword token

**Files:**
- Modify: `include/token.h` (TokenType enum — add `TOKEN_ENUM`)
- Modify: `src/lexer/token.c` (keyword table ~line 190; token-name array ~line 40)

**Interfaces:**
- Produces: `TOKEN_ENUM` available to the lexer bridge / Bison `ENUM` token (Task 3).

- [ ] **Step 1: Add the token enumerator**

In `include/token.h`, add `TOKEN_ENUM` to the `TokenType` enum next to `TOKEN_STRUCT` (place it adjacent to the other keyword tokens; appending near `TOKEN_STRUCT` keeps keyword tokens grouped).

- [ ] **Step 2: Register the keyword string**

In `src/lexer/token.c`, add to the keyword table (keep alphabetical ordering near `"const"`/`"default"`):
```c
    {"enum", TOKEN_ENUM},
```
And add the name-array entry near `[TOKEN_STRUCT] = "STRUCT",`:
```c
    [TOKEN_ENUM] = "ENUM",
```

- [ ] **Step 3: Build to verify it compiles**

Run: `make goo`
Expected: PASS (compiler builds). The probe still fails at parse (no grammar rule yet) — that is fine; this task only adds the token.

- [ ] **Step 4: Commit**

```bash
git add include/token.h src/lexer/token.c
git commit -m "feat(lexer): add enum keyword token

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Grammar + AST for `enum_type`

Add an `enum_type` type-expression (sibling of `struct_type`) and its AST nodes. Variants reuse the existing `struct_field_list` production for their field bodies.

**Files:**
- Modify: `src/parser/parser.y` (`%token` line 40; `%type` ~line 105; new productions near `struct_type` ~line 1298; add `| enum_type` to the `type` production ~line 1275)
- Modify: `include/ast.h` (append `AST_ENUM_TYPE`, `AST_ENUM_VARIANT` at the **tail** of `ASTNodeType`; add `EnumTypeNode`, `EnumVariantNode` structs; declare constructors)
- Modify: `src/ast/ast_constructors.c` (define `ast_enum_type_new`, `ast_enum_variant_new`)

**Interfaces:**
- Produces:
  - `typedef struct { ASTNode base; struct ASTNode* variants; } EnumTypeNode;` (node kind `AST_ENUM_TYPE`; `variants` is a `next`-chained list of `EnumVariantNode`).
  - `typedef struct { ASTNode base; char* name; struct ASTNode* fields; } EnumVariantNode;` (node kind `AST_ENUM_VARIANT`; `fields` is a `next`-chained list of `VarDeclNode`, same shape as struct fields).
  - `EnumTypeNode* ast_enum_type_new(ASTNode* variants, Position pos);`
  - `EnumVariantNode* ast_enum_variant_new(const char* name, ASTNode* fields, Position pos);`

- [ ] **Step 1: Declare token + types in the grammar**

In `src/parser/parser.y`, add `ENUM` to the keyword `%token` line (line 40):
```c
%token SELECT STRUCT SWITCH TYPE VAR ENUM
```
Add a `%type` line after the existing `struct_*` line (~line 99):
```c
%type <node> enum_type enum_variant_list enum_variant
```

- [ ] **Step 2: Add the grammar productions**

In `src/parser/parser.y`, immediately after the `struct_type` production (~line 1298), add:
```c
enum_type:
    ENUM LBRACE enum_variant_list RBRACE {
        $$ = (ASTNode*)ast_enum_type_new($3, get_current_position());
    }
    | ENUM LBRACE RBRACE {
        $$ = (ASTNode*)ast_enum_type_new(NULL, get_current_position());
    }
    ;

enum_variant_list:
    enum_variant { $$ = $1; }
    | enum_variant_list enum_variant {
        ast_add_child($1, $2);
        $$ = $1;
    }
    ;

enum_variant:
    identifier LBRACE struct_field_list RBRACE {
        IdentifierNode* ident = (IdentifierNode*)$1;
        $$ = (ASTNode*)ast_enum_variant_new(ident->name, $3, get_current_position());
        ast_node_free($1);
    }
    | identifier LBRACE RBRACE {
        IdentifierNode* ident = (IdentifierNode*)$1;
        $$ = (ASTNode*)ast_enum_variant_new(ident->name, NULL, get_current_position());
        ast_node_free($1);
    }
    ;
```
Then add to the `type` production (next to `| struct_type { $$ = $1; }`, ~line 1275):
```c
    | enum_type { $$ = $1; }
```

- [ ] **Step 3: Add AST node kinds + structs**

In `include/ast.h`, append at the **tail** of the `ASTNodeType` enum (next to `AST_STRUCT_LITERAL`, before `AST_NODE_COUNT`):
```c
    AST_ENUM_TYPE,         // type X enum { Variant{...} ... }
    AST_ENUM_VARIANT,      // a single enum variant: Name{ field: T ... }
```
Add the node structs (near `StructTypeNode`, ~line 521):
```c
// Enum (tagged union) type: `enum { Circle{radius int}  Rect{w int; h int} }`.
// `variants` is a next-chained list of EnumVariantNode.
typedef struct {
    ASTNode base;
    struct ASTNode* variants;
} EnumTypeNode;

// A single enum variant. `fields` is a next-chained list of VarDeclNode
// (same shape as struct fields), or NULL for a payloadless variant.
typedef struct {
    ASTNode base;
    char* name;
    struct ASTNode* fields;
} EnumVariantNode;
```
Declare the constructors (near `ast_var_decl_new`, ~line 983):
```c
EnumTypeNode* ast_enum_type_new(struct ASTNode* variants, Position pos);
EnumVariantNode* ast_enum_variant_new(const char* name, struct ASTNode* fields, Position pos);
```

- [ ] **Step 4: Define the constructors**

In `src/ast/ast_constructors.c`, add (mirror the existing `ast_var_decl_new`/`ast_struct_*` constructors for base-field initialization):
```c
EnumTypeNode* ast_enum_type_new(ASTNode* variants, Position pos) {
    EnumTypeNode* n = (EnumTypeNode*)calloc(1, sizeof(EnumTypeNode));
    if (!n) return NULL;
    n->base.type = AST_ENUM_TYPE;
    n->base.pos = pos;
    n->base.node_type = NULL;
    n->base.next = NULL;
    n->variants = variants;
    return n;
}

EnumVariantNode* ast_enum_variant_new(const char* name, ASTNode* fields, Position pos) {
    EnumVariantNode* n = (EnumVariantNode*)calloc(1, sizeof(EnumVariantNode));
    if (!n) return NULL;
    n->base.type = AST_ENUM_VARIANT;
    n->base.pos = pos;
    n->base.node_type = NULL;
    n->base.next = NULL;
    n->name = name ? strdup(name) : NULL;
    n->fields = fields;
    return n;
}
```

- [ ] **Step 5: Build to verify the grammar compiles + the probe now parses past `enum`**

Run: `make goo && ./bin/goo -o build/enum_ctor_probe examples/enum_ctor_probe.goo`
Expected: the parse error on `enum` is gone. It now FAILS later — in the type checker — with something like `Cannot resolve type` / unknown type for `Shape` or the variant literal (no `TYPE_ENUM` yet). That deeper failure confirms grammar+AST landed. Also confirm `make goo` reports no Bison conflicts introduced (the build does not error).

- [ ] **Step 6: Commit**

```bash
git add src/parser/parser.y include/ast.h src/ast/ast_constructors.c
git commit -m "feat(parser): enum_type grammar and AST nodes

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: `TYPE_ENUM` + type resolution + variant registration

Build a `TYPE_ENUM` from an `AST_ENUM_TYPE`, and register the enum name plus each variant constructor name in scope so literals resolve.

**Files:**
- Modify: `include/types.h` (`TYPE_ENUM` in `TypeKind`; `EnumVariant` struct; `enum_type` union member)
- Modify: `src/types/type_checker.c` (`type_from_ast` `AST_ENUM_TYPE` case ~near the `AST_STRUCT_TYPE` case at line 978; variant registration inside `type_check_type_decl` ~line 545)

**Interfaces:**
- Consumes: `EnumTypeNode`/`EnumVariantNode` (Task 3); the struct-building logic in `type_from_ast`'s `AST_STRUCT_TYPE` case (line 978).
- Produces:
  - `TypeKind` value `TYPE_ENUM`.
  - `typedef struct EnumVariant { char* name; Type* payload; int tag; } EnumVariant;` — `payload` is a `TYPE_STRUCT` (possibly 0 fields), `tag` is the variant index.
  - `Type` union member `struct { char* name; EnumVariant* variants; size_t variant_count; } enum_type;`
  - Scope registration: the enum name resolves (via `type_checker_lookup_variable`) to a `Variable` whose `.type` is the `TYPE_ENUM`; **each variant name** also resolves to a `Variable` whose `.type` is the **same `TYPE_ENUM`** (so a variant literal can find its parent enum + tag by searching `variants` for the literal's `type_name`).

- [ ] **Step 1: Add the type kind + structures**

In `include/types.h`, add `TYPE_ENUM` to `TypeKind` (next to `TYPE_STRUCT`, line 38):
```c
    TYPE_STRUCT,
    TYPE_ENUM,
```
Add the `EnumVariant` struct (near `StructField`, ~line 205):
```c
// One variant of a tagged union: a named payload struct + its discriminant.
typedef struct EnumVariant {
    char* name;
    Type* payload;   // a TYPE_STRUCT (0+ fields)
    int tag;         // discriminant index, 0-based in declaration order
} EnumVariant;
```
Add the union member in `struct Type` (next to `struct_type`, ~line 133):
```c
        // Enum (tagged union) type
        struct {
            char* name;
            EnumVariant* variants;
            size_t variant_count;
        } enum_type;
```

- [ ] **Step 2: Build a `TYPE_ENUM` in `type_from_ast`**

In `src/types/type_checker.c`, add a case to `type_from_ast` next to `AST_STRUCT_TYPE` (line 978). For each variant, build its payload `TYPE_STRUCT` by reusing the same field-walk the struct case uses (count `AST_VAR_DECL` fields, allocate `StructField[]`, resolve each field type, compute offsets/size). Compute the enum size as `tag_size (4, padded to 8) + max(payload size)`:
```c
        case AST_ENUM_TYPE: {
            EnumTypeNode* en = (EnumTypeNode*)type_node;
            size_t vcount = 0;
            for (ASTNode* v = en->variants; v; v = v->next)
                if (v->type == AST_ENUM_VARIANT) vcount++;

            Type* result = type_new(TYPE_ENUM);
            if (!result) return NULL;
            result->data.enum_type.variant_count = vcount;
            result->data.enum_type.variants =
                vcount ? calloc(vcount, sizeof(EnumVariant)) : NULL;

            size_t max_payload = 0, max_align = 4;
            size_t idx = 0;
            for (ASTNode* v = en->variants; v; v = v->next) {
                if (v->type != AST_ENUM_VARIANT) continue;
                EnumVariantNode* vn = (EnumVariantNode*)v;

                // Build the payload TYPE_STRUCT from vn->fields (same walk
                // as the AST_STRUCT_TYPE case above).
                size_t fcount = 0;
                for (ASTNode* f = vn->fields; f; f = f->next)
                    if (f->type == AST_VAR_DECL) fcount++;
                Type* payload = type_new(TYPE_STRUCT);
                payload->data.struct_type.field_count = fcount;
                payload->data.struct_type.fields =
                    fcount ? calloc(fcount, sizeof(StructField)) : NULL;
                payload->data.struct_type.name = strdup(vn->name);
                size_t off = 0, palign = 1, fidx = 0;
                for (ASTNode* f = vn->fields; f; f = f->next) {
                    if (f->type != AST_VAR_DECL) continue;
                    VarDeclNode* fd = (VarDeclNode*)f;
                    if (fd->name_count == 0) continue;
                    Type* ft = fd->type ? type_from_ast(checker, fd->type) : NULL;
                    if (!ft) { free(result); return NULL; }
                    payload->data.struct_type.fields[fidx].name = strdup(fd->names[0]);
                    payload->data.struct_type.fields[fidx].type = ft;
                    payload->data.struct_type.fields[fidx].offset = off;
                    off += ft->size ? ft->size : 8;
                    if (ft->align > palign) palign = ft->align;
                    fidx++;
                }
                payload->size = off;
                payload->align = palign;

                result->data.enum_type.variants[idx].name = strdup(vn->name);
                result->data.enum_type.variants[idx].payload = payload;
                result->data.enum_type.variants[idx].tag = (int)idx;
                if (off > max_payload) max_payload = off;
                if (palign > max_align) max_align = palign;
                idx++;
            }
            // Layout: { i32 tag, [max_payload x i8] }. Pad tag to payload align.
            size_t tag_slot = (max_align > 4) ? max_align : 4;
            result->size = tag_slot + max_payload;
            result->align = max_align;
            return result;
        }
```

- [ ] **Step 3: Register variant constructor names in `type_check_type_decl`**

In `src/types/type_checker.c`, in `type_check_type_decl` (line 545), after the existing struct-name stamping and **before/alongside** registering the enum name, register each variant name as a `Variable` whose `.type` is the enum `Type`. Add, right after `resolved` is computed and confirmed:
```c
    if (resolved->kind == TYPE_ENUM) {
        if (!resolved->data.enum_type.name)
            resolved->data.enum_type.name = strdup(td->name);
        for (size_t i = 0; i < resolved->data.enum_type.variant_count; i++) {
            const char* vname = resolved->data.enum_type.variants[i].name;
            Variable* ctor = variable_new(vname, resolved, decl->pos);
            if (!ctor) return 0;
            ctor->is_initialized = 1;
            ctor->is_builtin = 1;
            if (!scope_add_variable(checker->current_scope, ctor)) {
                // A duplicate variant name across enums is a conflict; report.
                type_error(checker, decl->pos,
                           "Enum variant '%s' already declared", vname);
                variable_free(ctor);
                return 0;
            }
        }
    }
```
(The existing tail of the function still registers `td->name` → `resolved` as the enum's own named type.)

- [ ] **Step 4: Build to verify it compiles + probe advances**

Run: `make goo && ./bin/goo -o build/enum_ctor_probe examples/enum_ctor_probe.goo`
Expected: still FAILS, but now further along — the literal `Circle{radius: 5}` is checked by `type_check_struct_literal`, which currently rejects a non-`TYPE_STRUCT` named type with `'Circle' is not a struct type`. That specific error confirms `TYPE_ENUM` + variant registration work; Task 5 fixes the literal path.

- [ ] **Step 5: Commit**

```bash
git add include/types.h src/types/type_checker.c
git commit -m "feat(types): TYPE_ENUM resolution and variant registration

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Typecheck variant construction literals

Extend `type_check_struct_literal` so that when the literal's `type_name` resolves to a `TYPE_ENUM` (i.e. it is a variant constructor), it checks the initializers against the matching variant's payload struct and yields the **enum** type.

**Files:**
- Modify: `src/types/expression_checker.c` (`type_check_struct_literal`, lines 96-183)

**Interfaces:**
- Consumes: variant registration from Task 4 (`type_name` → `Variable.type` is `TYPE_ENUM`).
- Produces: a struct/variant literal whose `expr->node_type` is the `TYPE_ENUM` when constructing a variant; the bound variant is recoverable in codegen by searching `node_type->data.enum_type.variants` for `lit->type_name`.

- [ ] **Step 1: Add the enum-variant branch**

In `src/types/expression_checker.c`, in `type_check_struct_literal` (line 96), after looking up `named` and before the existing `struct_type->kind != TYPE_STRUCT` rejection, add an enum branch:
```c
    Type* named_type = named->type;
    if (named_type->kind == TYPE_ENUM) {
        // Variant constructor: find the variant whose name == lit->type_name.
        EnumVariant* variant = NULL;
        for (size_t i = 0; i < named_type->data.enum_type.variant_count; i++) {
            if (strcmp(named_type->data.enum_type.variants[i].name, lit->type_name) == 0) {
                variant = &named_type->data.enum_type.variants[i];
                break;
            }
        }
        if (!variant) {
            type_error(checker, expr->pos, "'%s' is not a variant of enum '%s'",
                       lit->type_name, named_type->data.enum_type.name);
            return NULL;
        }
        // Reuse the struct-field checking logic against variant->payload by
        // temporarily treating it as the struct_type for the existing keyed/
        // positional initializer checks below.
        struct_type = variant->payload;
        // ... fall through to the existing keyed/positional checks, which use
        //     struct_type->data.struct_type.{fields,field_count} ...
        // After the existing checks succeed, yield the ENUM type, not payload:
        // (handled at the end — see Step 2)
        expr->node_type = named_type;   // the enum
        // Run the same field checks as the struct path, then return named_type.
    }
```
Refactor so the keyed/positional initializer-checking block (already present at lines 121-179) runs against `struct_type` (now the payload for variants), then return `named_type` for the enum case and `struct_type` for the plain struct case. Concretely: keep the existing checking block, but change the two `return struct_type;`/`expr->node_type = struct_type;` tail so that, when `named_type->kind == TYPE_ENUM`, it sets `expr->node_type = named_type;` and `return named_type;`.

- [ ] **Step 2: Make the existing struct path explicit**

Ensure the non-enum path is unchanged: when `named_type->kind == TYPE_STRUCT`, keep `struct_type = named_type;` and the existing `expr->node_type = struct_type; return struct_type;`. Only the enum case overrides the returned/stamped type to the enum.

- [ ] **Step 3: Build to verify the probe type-checks (codegen now the blocker)**

Run: `make goo && ./bin/goo -o build/enum_ctor_probe examples/enum_ctor_probe.goo`
Expected: type-checking passes; the failure (if any) now comes from **codegen** — e.g. `codegen_type_to_llvm` returning NULL for `TYPE_ENUM` (`Failed to lower struct type` or a null-type assertion). That shift to a codegen error confirms Task 5.

- [ ] **Step 4: Commit**

```bash
git add src/types/expression_checker.c
git commit -m "feat(types): typecheck enum variant construction literals

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Codegen for enum type + variant construction; ctor probe GREEN

Lower `TYPE_ENUM` to the LLVM aggregate `{ i32, [N x i8] }`, and lower a variant literal to a populated enum value. This closes the first RED→GREEN cycle.

**Files:**
- Modify: `src/codegen/type_mapping.c` (add `codegen_get_enum_type`; add `TYPE_ENUM` case to `codegen_type_to_llvm`, line 9-89)
- Modify: `src/codegen/composite_codegen.c` (enum-variant branch in `codegen_generate_struct_lit`, lines 283-354)
- Modify: `Makefile` (add `enum-probe` target; add to `verify`)
- Modify: `.github/workflows/tests.yml` (run the probe)

**Interfaces:**
- Consumes: `TYPE_ENUM` layout from Task 4 (`result->size` = tag_slot + max_payload).
- Produces: `LLVMTypeRef codegen_get_enum_type(CodeGenerator*, const Type*)` returning `{ i32, [N x i8] }`; a variant literal lowers to an SSA enum aggregate value (`value_info_new(NULL, loaded, enum_type)`).

- [ ] **Step 1: Lower the enum LLVM type**

In `src/codegen/type_mapping.c`, add:
```c
LLVMTypeRef codegen_get_enum_type(CodeGenerator* codegen, const Type* type) {
    if (!codegen || !type || type->kind != TYPE_ENUM) return NULL;
    // { i32 tag, [N x i8] payload } where N covers the largest variant.
    size_t tag_slot = (type->align > 4) ? type->align : 4;
    size_t payload_bytes = (type->size > tag_slot) ? (type->size - tag_slot) : 0;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(codegen->context);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(codegen->context);
    LLVMTypeRef payload = LLVMArrayType(i8, (unsigned)payload_bytes);
    LLVMTypeRef members[2] = { i32, payload };
    return LLVMStructTypeInContext(codegen->context, members, 2, 0);
}
```
And add to `codegen_type_to_llvm` (the switch at line 9):
```c
        case TYPE_ENUM:
            return codegen_get_enum_type(codegen, type);
```
Declare `codegen_get_enum_type` in the same header that declares `codegen_get_struct_type` (find it with `grep -rn codegen_get_struct_type include/`).

- [ ] **Step 2: Lower variant construction**

In `src/codegen/composite_codegen.c`, in `codegen_generate_struct_lit` (line 283), branch when `expr->node_type->kind == TYPE_ENUM`. Build the payload struct value the existing way (against the variant's `payload` TYPE_STRUCT), then write `{tag, payload}` through an alloca and return the loaded enum value:
```c
    if (struct_type->kind == TYPE_ENUM) {
        // Find the variant + tag for lit->type_name.
        EnumVariant* variant = NULL;
        for (size_t i = 0; i < struct_type->data.enum_type.variant_count; i++) {
            if (strcmp(struct_type->data.enum_type.variants[i].name, lit->type_name) == 0) {
                variant = &struct_type->data.enum_type.variants[i];
                break;
            }
        }
        if (!variant) {
            codegen_error(codegen, expr->pos, "No variant '%s' in enum", lit->type_name);
            return NULL;
        }
        LLVMTypeRef enum_ty = codegen_type_to_llvm(codegen, struct_type);
        LLVMTypeRef payload_ty = codegen_type_to_llvm(codegen, variant->payload);

        // Build the payload aggregate (reuse the struct InsertValue loop, but
        // against variant->payload's fields). Easiest: temporarily treat
        // payload as the struct_type for the existing field-insert loop, or
        // factor that loop into a helper codegen_build_struct_value(codegen,
        // checker, lit, variant->payload) returning an LLVMValueRef.
        LLVMValueRef payload_val =
            codegen_build_struct_value(codegen, checker, lit, variant->payload);
        if (!payload_val) return NULL;

        // alloca enum; store tag; bitcast payload slot; store payload; load.
        LLVMValueRef tmp = codegen_create_alloca(codegen, enum_ty, "enum_tmp");
        LLVMValueRef tag_ptr = LLVMBuildStructGEP2(codegen->builder, enum_ty, tmp, 0, "tag_ptr");
        LLVMBuildStore(codegen->builder,
            LLVMConstInt(LLVMInt32TypeInContext(codegen->context), variant->tag, 0), tag_ptr);
        LLVMValueRef pslot = LLVMBuildStructGEP2(codegen->builder, enum_ty, tmp, 1, "payload_slot");
        LLVMValueRef pcast = LLVMBuildBitCast(codegen->builder, pslot,
            LLVMPointerType(payload_ty, 0), "payload_cast");
        LLVMBuildStore(codegen->builder, payload_val, pcast);
        LLVMValueRef loaded = LLVMBuildLoad2(codegen->builder, enum_ty, tmp, "enum_val");
        return value_info_new(NULL, loaded, struct_type);
    }
```
Factor the existing field-insert loop (lines 308-352) into `static LLVMValueRef codegen_build_struct_value(CodeGenerator*, TypeChecker*, StructLiteralNode* lit, Type* struct_type)` returning the `agg`, so both the struct path and the enum payload reuse it (DRY). The plain struct path then becomes `return value_info_new(NULL, codegen_build_struct_value(...), struct_type);`.

- [ ] **Step 2b: Build and run the probe — expect GREEN**

Run:
```bash
make goo && make lib/libgoo_runtime.a
./bin/goo -o build/enum_ctor_probe examples/enum_ctor_probe.goo
./build/enum_ctor_probe
```
Expected output:
```
PASS: enum declared
PASS: variants constructed
```

- [ ] **Step 3: Add the Makefile probe target**

In `Makefile`, after the `pointer-probe`/`new-probe` targets, add (mirror `lvalue-probe` at line 298):
```makefile
# M2 enum gate (construction): declare a tagged union and construct variants.
enum-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/enum_ctor_probe examples/enum_ctor_probe.goo
	@./build/enum_ctor_probe > build/enum_ctor_probe.actual.txt
	@if diff -u examples/enum_ctor_probe.expected.txt build/enum_ctor_probe.actual.txt; then \
	  echo "enum-probe: PASS"; \
	else \
	  echo "enum-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
```
Append `enum-probe` to the `verify` target list (line 457).

- [ ] **Step 4: Wire into CI**

In `.github/workflows/tests.yml`, add a step that runs the probe alongside the existing `*-probe` invocations (grep the file for `lvalue-probe` and copy that step's shape for `enum-probe`).

- [ ] **Step 5: Verify the aggregate gate**

Run: `make goo && make lib/libgoo_runtime.a && make enum-probe`
Expected: `enum-probe: PASS`.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/type_mapping.c src/codegen/composite_codegen.c Makefile .github/workflows/tests.yml
git commit -m "feat(codegen): lower enum type and variant construction

Closes the enum construction RED->GREEN cycle (enum-probe).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: Write the failing `match` gate probe (RED)

The M2 milestone gate: build a tagged-union mini-AST, walk it with `match`, recursing through pointer payloads, and assert the evaluated result.

**Files:**
- Create: `examples/match_probe.goo`
- Create: `examples/match_probe.expected.txt`

**Interfaces:**
- Consumes: enum declaration + construction (Tasks 2-6).
- Produces: the `match` surface the lowering must support — statement-style `match <expr> { case Variant{a, b}: ... default: ... }` with positional payload binding.

- [ ] **Step 1: Write the probe**

`examples/match_probe.goo`:
```goo
// match_probe: the M2 gate. Build a small Expr AST (a tagged union) and
// evaluate it by walking the tree with `match` over the tag, recursing
// through *Expr payloads. Statement-style match; positional payload binding.
package main

import "fmt"

type Expr enum {
    Num{value: int}
    Add{lhs: *Expr, rhs: *Expr}
}

func eval(e *Expr) int {
    var result int
    match *e {
    case Num{v}:
        result = v
    case Add{l, r}:
        result = eval(l) + eval(r)
    }
    return result
}

func main() {
    // Build  Add(Num(2), Add(Num(3), Num(4)))  == 9
    n2 := new(Expr)
    *n2 = Num{value: 2}
    n3 := new(Expr)
    *n3 = Num{value: 3}
    n4 := new(Expr)
    *n4 = Num{value: 4}
    inner := new(Expr)
    *inner = Add{lhs: n3, rhs: n4}
    root := new(Expr)
    *root = Add{lhs: n2, rhs: inner}

    if eval(root) == 9 {
        fmt.Println("PASS: match evaluates tagged-union AST")
    }
}
```

`examples/match_probe.expected.txt`:
```
PASS: match evaluates tagged-union AST
```

> Note: this relies on `new(T)` + deref-store (`*p = v`), both shipped in M1 (PRs #15/#16). If the executing agent finds enum-by-pointer storage interacts badly with `new`, fall back to value-typed children using an index/arena, but prefer the pointer form — it is the realistic AST shape.

- [ ] **Step 2: Verify it fails**

Run: `make goo && make lib/libgoo_runtime.a && ./bin/goo -o build/match_probe examples/match_probe.goo`
Expected: FAIL — `match` has no typecheck/codegen, so either the type checker ignores `AST_MATCH_EXPR` (leaving `result` unset / a checker error) or codegen errors on the unhandled node. Capture the exact message.

- [ ] **Step 3: Commit the RED probe**

```bash
git add examples/match_probe.goo examples/match_probe.expected.txt
git commit -m "test(match): add failing M2 gate probe (tagged-union AST walk)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Typecheck `match`

Add an `AST_MATCH_EXPR` case to the expression checker: resolve the scrutinee, check each arm's variant pattern, bind positional payload names in the arm scope, and enforce exhaustiveness.

**Files:**
- Modify: `src/types/expression_checker.c` (`type_check_expression` switch, ~line 85; add `type_check_match_expr`)

**Interfaces:**
- Consumes: `TYPE_ENUM` (Task 4); `MatchExprNode`/`MatchCaseNode`/`PatternNode` (`include/ast.h`); `PATTERN_DESTRUCTURE` carries `data.destructure.type_name` (variant name) and `data.destructure.fields` (a `next`-chained `expression_list` of binding identifiers, in declared field order); `PATTERN_WILDCARD` = `default`.
- Produces: a typechecked match whose arms have payload bindings registered as locals; `expr->node_type` set to a void/unit type (statement-style). Scope push/pop per arm via the existing scope API (find with `grep -rn "scope_enter\|scope_push\|type_checker.*scope" src/types`).

- [ ] **Step 1: Add the dispatch case + checker function**

In `src/types/expression_checker.c`, add to the `type_check_expression` switch (line 85):
```c
        case AST_MATCH_EXPR:
            return type_check_match_expr(checker, expr);
```
Implement:
```c
Type* type_check_match_expr(TypeChecker* checker, ASTNode* expr) {
    MatchExprNode* m = (MatchExprNode*)expr;
    Type* scrut = type_check_expression(checker, m->expr);
    if (!scrut) return NULL;

    int is_enum = (scrut->kind == TYPE_ENUM);
    // Track which variants are covered, plus whether a default exists.
    size_t vcount = is_enum ? scrut->data.enum_type.variant_count : 0;
    int* covered = vcount ? calloc(vcount, sizeof(int)) : NULL;
    int has_default = 0;

    for (ASTNode* c = m->cases; c; c = c->next) {
        MatchCaseNode* mc = (MatchCaseNode*)c;
        PatternNode* p = (PatternNode*)mc->pattern;
        // Per-arm scope so payload bindings don't leak.
        type_checker_enter_scope(checker);   // use the project's actual scope-enter fn
        if (p->pattern_type == PATTERN_WILDCARD) {
            has_default = 1;
        } else if (is_enum && p->pattern_type == PATTERN_DESTRUCTURE) {
            const char* vn = p->data.destructure.type_name;
            EnumVariant* variant = NULL;
            int vidx = -1;
            for (size_t i = 0; i < vcount; i++) {
                if (strcmp(scrut->data.enum_type.variants[i].name, vn) == 0) {
                    variant = &scrut->data.enum_type.variants[i]; vidx = (int)i; break;
                }
            }
            if (!variant) {
                type_error(checker, c->pos, "'%s' is not a variant of enum '%s'",
                           vn, scrut->data.enum_type.name);
                type_checker_exit_scope(checker); free(covered); return NULL;
            }
            if (covered[vidx]) {
                type_error(checker, c->pos, "Duplicate match arm for variant '%s'", vn);
                type_checker_exit_scope(checker); free(covered); return NULL;
            }
            covered[vidx] = 1;
            // Positional bindings: each identifier in destructure.fields binds
            // to the variant payload field at the same index.
            size_t fi = 0;
            Type* payload = variant->payload;
            for (ASTNode* b = p->data.destructure.fields; b; b = b->next, fi++) {
                if (fi >= payload->data.struct_type.field_count) {
                    type_error(checker, c->pos,
                        "Too many bindings for variant '%s' (has %zu fields)",
                        vn, payload->data.struct_type.field_count);
                    type_checker_exit_scope(checker); free(covered); return NULL;
                }
                if (b->type != AST_IDENTIFIER) continue;  // ignore non-ident (e.g. literal)
                IdentifierNode* bind = (IdentifierNode*)b;
                if (strcmp(bind->name, "_") == 0) continue;  // wildcard binding
                Variable* var = variable_new(bind->name,
                                             payload->data.struct_type.fields[fi].type, c->pos);
                var->is_initialized = 1;
                scope_add_variable(checker->current_scope, var);
            }
        }
        // else: literal/identifier patterns over non-enum scrutinees keep
        // their existing meaning (value match); no binding bookkeeping here.

        if (mc->guard) {
            type_check_expression(checker, ((GuardConditionNode*)mc->guard)->condition);
        }
        // Check the arm body statements in this scope.
        for (ASTNode* s = mc->body; s; s = s->next)
            type_check_statement(checker, s);   // use the project's stmt-check fn
        type_checker_exit_scope(checker);
    }

    if (is_enum && !has_default) {
        for (size_t i = 0; i < vcount; i++) {
            if (!covered[i]) {
                type_error(checker, expr->pos,
                    "Non-exhaustive match: variant '%s' not handled (add a case or `default:`)",
                    scrut->data.enum_type.variants[i].name);
                free(covered); return NULL;
            }
        }
    }
    free(covered);
    expr->node_type = type_checker_get_builtin(checker, TYPE_VOID);
    return expr->node_type;
}
```
Replace `type_checker_enter_scope`/`exit_scope`/`type_check_statement`/`type_checker_get_builtin` with the project's actual symbol names (grep `src/types` to confirm: `grep -rn "enter_scope\|exit_scope\|type_check_statement\|get_builtin" src/types`). Declare `type_check_match_expr` in the header that declares `type_check_struct_literal`.

- [ ] **Step 2: Build + confirm the checker now passes, codegen is the blocker**

Run: `make goo && ./bin/goo -o build/match_probe examples/match_probe.goo`
Expected: type-checking passes (no "non-exhaustive" error — both `Num` and `Add` are handled); failure now comes from codegen not handling `AST_MATCH_EXPR`. Confirm by the changed error message.

- [ ] **Step 3: Commit**

```bash
git add src/types/expression_checker.c
git commit -m "feat(types): typecheck match with exhaustiveness + payload binding

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: Codegen `match`; gate probe GREEN

Lower `match` to: load the discriminant, `switch` on it, and in each arm bind the positional payload fields then emit the body; all arms branch to a merge block. Statement-style → no result value.

**Files:**
- Modify: `src/codegen/expression_codegen.c` (`codegen_generate_expression` switch, ~line 42 — add `AST_MATCH_EXPR`)
- Modify: `src/codegen/composite_codegen.c` (add `codegen_generate_match`) — or `statement_codegen.c` near the existing `switch` lowering; place it beside the construct it most resembles.
- Modify: `Makefile` (`match-probe` target; add to `verify`)
- Modify: `.github/workflows/tests.yml` (run the probe)

**Interfaces:**
- Consumes: enum layout `{ i32 tag, [N x i8] }` (Task 6); the per-arm binding semantics fixed in Task 8 (positional, declared order).
- Produces: `ValueInfo* codegen_generate_match(CodeGenerator*, TypeChecker*, ASTNode*)` returning a void `ValueInfo` (no usable value).

- [ ] **Step 1: Add the dispatch case**

In `src/codegen/expression_codegen.c` (line 42):
```c
        case AST_MATCH_EXPR:
            return codegen_generate_match(codegen, checker, expr);
```

- [ ] **Step 2: Implement match lowering**

In `src/codegen/composite_codegen.c`:
```c
ValueInfo* codegen_generate_match(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
#if !LLVM_AVAILABLE
    codegen_error(codegen, expr->pos, "LLVM support not available");
    return NULL;
#else
    MatchExprNode* m = (MatchExprNode*)expr;

    // Scrutinee. Need it in memory to read the tag + bitcast the payload.
    ValueInfo* scrut = codegen_generate_expression(codegen, checker, m->expr);
    if (!scrut) return NULL;
    Type* enum_type = scrut->goo_type;
    if (!enum_type || enum_type->kind != TYPE_ENUM) {
        codegen_error(codegen, expr->pos, "match scrutinee is not an enum");
        return NULL;
    }
    LLVMTypeRef enum_ty = codegen_type_to_llvm(codegen, enum_type);
    LLVMValueRef scrut_ptr;
    if (scrut->is_lvalue) {
        scrut_ptr = scrut->llvm_value;
    } else {
        scrut_ptr = codegen_create_alloca(codegen, enum_ty, "match_scrut");
        LLVMBuildStore(codegen->builder, scrut->llvm_value, scrut_ptr);
    }

    LLVMValueRef tag_ptr = LLVMBuildStructGEP2(codegen->builder, enum_ty, scrut_ptr, 0, "tag_ptr");
    LLVMValueRef tag = LLVMBuildLoad2(codegen->builder,
        LLVMInt32TypeInContext(codegen->context), tag_ptr, "tag");

    LLVMBasicBlockRef cur = LLVMGetInsertBlock(codegen->builder);
    LLVMValueRef fn = LLVMGetBasicBlockParent(cur);
    LLVMBasicBlockRef merge = LLVMAppendBasicBlockInContext(codegen->context, fn, "match_end");

    // Find a default arm (PATTERN_WILDCARD), else merge is the default target.
    LLVMBasicBlockRef default_bb = merge;
    // Count non-default arms for the switch.
    unsigned narms = 0;
    for (ASTNode* c = m->cases; c; c = c->next) {
        PatternNode* p = (PatternNode*)((MatchCaseNode*)c)->pattern;
        if (p->pattern_type != PATTERN_WILDCARD) narms++;
    }
    // Pre-create the default block if there is a wildcard arm.
    for (ASTNode* c = m->cases; c; c = c->next) {
        PatternNode* p = (PatternNode*)((MatchCaseNode*)c)->pattern;
        if (p->pattern_type == PATTERN_WILDCARD) {
            default_bb = LLVMAppendBasicBlockInContext(codegen->context, fn, "match_default");
            break;
        }
    }
    LLVMValueRef sw = LLVMBuildSwitch(codegen->builder, tag, default_bb, narms);

    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(codegen->context), 0);
    for (ASTNode* c = m->cases; c; c = c->next) {
        MatchCaseNode* mc = (MatchCaseNode*)c;
        PatternNode* p = (PatternNode*)mc->pattern;
        LLVMBasicBlockRef arm;
        if (p->pattern_type == PATTERN_WILDCARD) {
            arm = default_bb;
        } else {
            // Resolve variant + tag.
            const char* vn = p->data.destructure.type_name;
            EnumVariant* variant = NULL;
            for (size_t i = 0; i < enum_type->data.enum_type.variant_count; i++)
                if (strcmp(enum_type->data.enum_type.variants[i].name, vn) == 0)
                    { variant = &enum_type->data.enum_type.variants[i]; break; }
            arm = LLVMAppendBasicBlockInContext(codegen->context, fn, vn);
            LLVMAddCase(sw, LLVMConstInt(LLVMInt32TypeInContext(codegen->context),
                        variant->tag, 0), arm);
            LLVMPositionBuilderAtEnd(codegen->builder, arm);

            // Bind positional payload fields as arm locals.
            LLVMTypeRef payload_ty = codegen_type_to_llvm(codegen, variant->payload);
            LLVMValueRef pslot = LLVMBuildStructGEP2(codegen->builder, enum_ty, scrut_ptr, 1, "pslot");
            LLVMValueRef pptr = LLVMBuildBitCast(codegen->builder, pslot,
                LLVMPointerType(payload_ty, 0), "pptr");
            size_t fi = 0;
            for (ASTNode* b = p->data.destructure.fields; b; b = b->next, fi++) {
                if (b->type != AST_IDENTIFIER) continue;
                IdentifierNode* bind = (IdentifierNode*)b;
                if (strcmp(bind->name, "_") == 0) continue;
                StructField* f = &variant->payload->data.struct_type.fields[fi];
                LLVMValueRef fptr = LLVMBuildStructGEP2(codegen->builder, payload_ty,
                    pptr, (unsigned)fi, bind->name);
                LLVMValueRef fval = LLVMBuildLoad2(codegen->builder,
                    codegen_type_to_llvm(codegen, f->type), fptr, bind->name);
                // Register the binding as a named local for the arm body.
                // Use the same local-variable registration the codegen uses
                // for `:=` (grep: codegen_declare_local / named-value table).
                codegen_bind_local(codegen, bind->name, fval, f->type);
            }
        }
        if (arm == default_bb)
            LLVMPositionBuilderAtEnd(codegen->builder, default_bb);

        // Emit the arm body, then branch to merge (if not already terminated).
        for (ASTNode* s = mc->body; s; s = s->next)
            codegen_generate_statement(codegen, checker, s);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(codegen->builder)))
            LLVMBuildBr(codegen->builder, merge);
    }

    LLVMPositionBuilderAtEnd(codegen->builder, merge);
    return value_info_new(NULL, NULL, type_checker_get_builtin(checker, TYPE_VOID));
#endif
}
```
Replace `codegen_bind_local` / `codegen_generate_statement` with the project's actual symbols (grep: `grep -rn "codegen_generate_statement\|bind.*local\|named_value\|declare_local" src/codegen`). Declare `codegen_generate_match` in the header that declares `codegen_generate_struct_lit`.

- [ ] **Step 3: Build, compile, and run the gate probe — expect GREEN**

Run:
```bash
make goo && make lib/libgoo_runtime.a
./bin/goo -o build/match_probe examples/match_probe.goo
./build/match_probe
```
Expected output:
```
PASS: match evaluates tagged-union AST
```
If it prints nothing, the `eval` result ≠ 9 — debug payload binding (field index/order) and the recursion through `*Expr` (pointer load before `match *e`).

- [ ] **Step 4: Add the Makefile target + wire CI**

In `Makefile`, after `enum-probe`, add:
```makefile
# M2 gate (the milestone deliverable): build a tagged-union AST and walk it
# with `match` over the tag, recursing through pointer payloads.
match-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	$(COMPILER) -o build/match_probe examples/match_probe.goo
	@./build/match_probe > build/match_probe.actual.txt
	@if diff -u examples/match_probe.expected.txt build/match_probe.actual.txt; then \
	  echo "match-probe: PASS"; \
	else \
	  echo "match-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
```
Append `match-probe` to the `verify` list (line 457). Add a `match-probe` step to `.github/workflows/tests.yml` next to `enum-probe`.

- [ ] **Step 5: Run the full verify gate**

Run: `make goo && make lib/libgoo_runtime.a && make verify`
Expected: all probes PASS, including `enum-probe` and `match-probe`, with no regression in the existing probes.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/expression_codegen.c src/codegen/composite_codegen.c Makefile .github/workflows/tests.yml
git commit -m "feat(codegen): lower match (tag dispatch + payload binding)

Closes the M2 gate: a tagged-union AST is walked via match over the tag
(match-probe). Enums + match complete the gate-critical slice of M2.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:**
- Tagged unions / enums (spec §1) → Tasks 2-6. ✓
- Lower `match` (spec §2) → Tasks 8-9, statement-style ✓, exhaustive-or-default ✓ (Task 8 Step 1), positional payload binding ✓.
- M2 gate (walk tagged-union mini-AST with match) → Task 7/9 `match_probe`. ✓
- Error handling via diagnostics, no panics (spec) → `type_error` paths in Tasks 4/5/8. ✓
- Probe + Makefile + CI cadence (spec testing) → Tasks 6/9. ✓
- **Dynamic slices (spec §3) and general maps (spec §4) are NOT in this plan** — they are separate, independent deliverables with their own plans (per the spec's sequencing and the writing-plans scope guidance). This plan covers only the gate-critical enums + match.

**Placeholder scan:** No "TBD"/"handle edge cases"/"write tests for the above". Each code step shows code. Two intentional grep-and-confirm notes flag project-specific symbol names (`type_checker_enter_scope`, `codegen_bind_local`, `codegen_generate_statement`) the implementer must match to the real API — these are explicit verification steps, not placeholders.

**Type consistency:** `EnumTypeNode.variants`, `EnumVariantNode.{name,fields}`, `EnumVariant.{name,payload,tag}`, `enum_type.{name,variants,variant_count}`, and the `{i32 tag, [N x i8] payload}` LLVM layout are used identically across Tasks 3, 4, 5, 6, 8, 9. Variant lookup is by name against `data.enum_type.variants` everywhere. Construction yields `node_type = TYPE_ENUM` (Task 5) which Task 6 codegen and Task 8/9 match consume consistently.

## Notes for the executor

- **Bison conflicts:** after Task 3, watch `make goo` output for new shift/reduce conflicts. `enum_variant: identifier LBRACE struct_field_list RBRACE` could interact with struct-literal grammar in some contexts; it should be safe because enum variants only appear inside an `enum_type` body, but verify the build is clean.
- **Scope/local-binding APIs are the main unknowns.** Before Tasks 8-9, run the two greps noted to pin the real function names for scope enter/exit, statement checking/codegen, and local-variable binding. Adjust the snippets to match.
- **Fallback for the gate probe** (Task 7 note): if enum-through-pointer storage misbehaves with `new`, switch the children to an arena/index representation, but keep the pointer form as the target.
