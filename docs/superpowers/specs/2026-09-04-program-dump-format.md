# The program dump format

Status: describes the format `bin/goo --emit-ast-json` and `bin/goo
--emit-program` produce today, version `1`. Written from the emitter
(`src/compiler/program_dump.c`, `include/program_dump.h`), the writer
(`src/ast/json_writer.c`), and the structural checker
(`scripts/program_dump_check.py`), not from the Phase 0 plan text, which
predates the code and no longer matches it in places.

Audience: a reader who must write a second producer (for example, a Haskell
front end) or a consumer of this format, and who does not read the C source.

## Purpose

The dump is the interchange the front-end migration is measured against. It
gives one JSON object per AST node, a de-duplicated type table addressed by
integer id, and one release plan per file. Two commands produce it:

- `bin/goo --emit-ast-json -o /dev/null <file>` — the **parse** stage: the
  parsed program, no type ids, `"plan": null` for every file.
- `bin/goo --emit-program -o /dev/null <file>` — the **typed** stage: the
  same tree after type checking, with type ids on every node the checker
  stamps, plus a release plan per file.

Both write one JSON document to stdout and exit 0. `-o /dev/null` sets the
normal compile output path, which the dump does not use, so no other file is
written.

## The envelope

```
{
  "goo_program_dump": 1,
  "stage": "parse" | "typed",
  "files": [ <file entry>, ... ],
  "types": [ <type entry>, ... ]
}
```

`types` is always present. At the parse stage it is always an empty array
(`scripts/program_dump_check.py` fails a parse-stage dump that has any
entries in it).

A file entry:

```
{
  "file": "<path as given on the command line>",
  "package": "<package clause name>",
  "imports": [ <IMPORT_SPEC node>, ... ],
  "decls": [ <declaration node>, ... ],
  "plan": null | [ <release plan function entry>, ... ]
}
```

`plan` is `null` at the parse stage always, and at the typed stage whenever
`GOO_ARC_RELEASE=0` left the release plan unbuilt for that file (see
"Invariant 1" below). There is no `AST_PROGRAM`-kind node anywhere in the
dump: the per-file object above IS the program node, spelled out field by
field, rather than wrapped in a node with `"kind": "PROGRAM"`.

### Worked example — parse stage

`./bin/goo --emit-ast-json -o /dev/null examples/minimal.goo` on:

```go
package main

func main() {
}
```

produces:

```json
{
  "goo_program_dump": 1,
  "stage": "parse",
  "files": [
    {
      "file": "examples/minimal.goo",
      "package": "main",
      "imports": [],
      "decls": [
        {
          "kind": "FUNC_DECL",
          "pos": [3, 6, 0],
          "name": "main",
          "is_comptime": false,
          "is_unsafe": false,
          "has_receiver": false,
          "type_params": [],
          "params": [],
          "return_type": null,
          "annotations": [],
          "body": {
            "kind": "BLOCK_STMT",
            "pos": [4, 2, 29],
            "statements": []
          }
        }
      ],
      "plan": null
    }
  ],
  "types": []
}
```

### Worked example — typed stage, a type table and a release plan

`./bin/goo --emit-program -o /dev/null examples/erru_catch_probe.goo`, on a
two-function program (`alwaysOk() !int`, called from `main` through
`catch`), produces (trimmed to the parts this section is about):

```json
{
  "goo_program_dump": 1,
  "stage": "typed",
  "files": [
    {
      "file": "examples/erru_catch_probe.goo",
      "package": "main",
      "imports": [ { "kind": "IMPORT_SPEC", "pos": [7,1,235], "path": "fmt", "alias": null } ],
      "decls": [ /* two FUNC_DECL nodes, every node carrying a "type" field now */ ],
      "plan": [
        { "function": "alwaysOk", "locals": [], "owned_keys": [], "owned_concat_operands": [] },
        {
          "function": "main",
          "locals": [
            { "name": "x", "verdict": "RELEASE_NO_ESCAPES", "reasons": "UNCLASSIFIED", "owns_elems": false }
          ],
          "owned_keys": [],
          "owned_concat_operands": []
        }
      ]
    }
  ],
  "types": [
    { "id": 0, "kind": "INT64",    "size": 8,  "align": 8, "package": null, "name": "int64" },
    { "id": 1, "kind": "ERROR_UNION", "size": 12, "align": 8, "package": null, "value": 0, "error": null },
    { "id": 2, "kind": "FUNCTION", "size": 8,  "align": 8, "package": null, "params": [], "return": 1, "is_variadic": false, "has_comptime_params": false },
    { "id": 3, "kind": "VOID",     "size": 0,  "align": 1, "package": null, "name": "void" },
    { "id": 4, "kind": "FUNCTION", "size": 8,  "align": 8, "package": null, "params": [], "return": 3, "is_variadic": true, "has_comptime_params": false },
    { "id": 5, "kind": "PACKAGE",  "size": 0,  "align": 0, "package": null, "name": null },
    { "id": 6, "kind": "STRING",   "size": 8,  "align": 8, "package": null, "name": "string" },
    { "id": 7, "kind": "INT8",     "size": 1,  "align": 1, "package": null, "name": "int8" },
    { "id": 8, "kind": "POINTER",  "size": 8,  "align": 8, "package": null, "pointee": 7 },
    { "id": 9, "kind": "NULLABLE", "size": 9,  "align": 8, "package": null, "base": 8 }
  ]
}
```

One caveat this run shows directly: type id 5 is `PACKAGE`, the type of the
identifier `fmt` in `fmt.Println(...)`, and it carries `"name": null`. A
`TYPE_PACKAGE` marker (`type_checker_seed_package_marker`,
`src/types/type_checker.c`) never stores the package's own name on the
`Type` — the name lives on the `Variable` that owns it, which is not
dumped. A consumer that needs to know WHICH package a `PACKAGE`-typed value
denotes must read it off the enclosing `SELECTOR_EXPR`'s `"expr"` node
(here, the `IDENTIFIER` node named `"fmt"`), not off the type entry.

## Node families

Every node object opens with `"kind"` and `"pos": [line, column, offset]`,
in that order. A node whose position names a file other than the file
entry it sits under carries an extra `"file": "<name>"` field right after
`"pos"` (a node built from a different source than the enclosing file,
for example a synthesized `goo test` entry point). At the typed stage,
every node the type checker stamped with a `Type*` also carries a
`"type": <type table id>` field, placed immediately after `"pos"` /
`"file"` and before every kind-specific field.

Fields below are listed in emission order, which is also JSON key order
(Invariant 1). "child" means a nested node object or `null`. "list" means a
JSON array of node objects (empty, not `null`, when there are none).
"list<str>" is an array of strings. Every "str" field is written by the
same helper (`emit_str` over `jw_string`), which maps a NULL `char*` to
JSON `null` uniformly — so a "str" field not called out below as
nullable can still be `null` in principle if the compiler ever leaves
that pointer unset; the per-field notes below call out the cases known to
happen on real fixtures. A `kind` name reused across two field sets
(`AST_SLICE_EXPR`/`AST_PAREN_EXPR`, described below) is noted where it
occurs. Every kind name and field below comes from a `case` arm in
`emit_node` (`src/compiler/program_dump.c`); no other kind is emitted — an
unhandled `ASTNodeType` aborts (Invariant 3).

### Declarations

| kind | fields |
|---|---|
| `PACKAGE_DECL` | `name` (str) |
| `IMPORT_SPEC` | `path` (str), `alias` (str or null) |
| `FUNC_DECL` | `name` (str), `is_comptime` (bool), `is_unsafe` (bool), `has_receiver` (bool — true means the receiver is `params[0]`, not a separate field), `type_params` (list), `params` (list), `return_type` (child), `annotations` (list), `body` (child, null for a signature-only declaration) |
| `VAR_DECL` | `names` (list<str>), `decl_type` (child), `values` (list), `ownership` (str, one of `OWNERSHIP_NONE`/`OWNERSHIP_OWNED`/`OWNERSHIP_BORROWED`/`OWNERSHIP_SHARED`), `is_short_decl` (bool), `is_variadic_param` (bool), `is_captured` (bool), `is_embedded` (bool), `is_comptime_param` (bool) |
| `CONST_DECL` | `names` (list<str>), `decl_type` (child), `values` (list), `is_comptime` (bool) |
| `TYPE_DECL` | `name` (str), `decl_type` (child) |
| `EXTERN_DECL` | `name` (str), `abi` (str), `params` (list), `return_type` (child), `library` (str) |
| `ATTRIBUTE` | `name` (str), `args` (list) |

### Statements

| kind | fields |
|---|---|
| `BLOCK_STMT` | `statements` (list) |
| `EXPR_STMT` | `expr` (child) |
| `IF_STMT` | `condition` (child), `then` (child), `else` (child) |
| `IF_LET_STMT` | `var_name` (str), `nullable_expr` (child), `then` (child), `else` (child) |
| `FOR_STMT` | `init` (child), `condition` (child), `post` (child), `range_expr` (child), `key_name` (str or null), `value_name` (str or null), `body` (child) |
| `RETURN_STMT` | `values` (list) |
| `BREAK_STMT` | (none) |
| `CONTINUE_STMT` | (none) |
| `FALLTHROUGH_STMT` | (none) |
| `DEFER_STMT` | `call` (child) |
| `GO_STMT` | `call` (child) |
| `SELECT_STMT` | `cases` (list) |
| `SELECT_CASE` | `comm` (child), `body` (list), `bind_name` (str or null), `is_declare` — a JSON **integer** 0/1, emitted with the same helper as an ordinary int field, not the bool helper; treat it as truthy/falsy, not as `true`/`false` |
| `SWITCH_STMT` | `tag` (child), `cases` (list) |
| `CASE_CLAUSE` | `exprs` (list), `body` (list) |
| `TYPE_SWITCH` | `bind_name` (child — a node, not a string; see the unstamped-shapes table), `expr` (child), `cases` (list) |
| `TYPE_CASE` | `types` (list), `body` (list) |
| `UNSAFE_STMT` | `body` (child) |
| `ARENA_BLOCK` | `body` (child) |
| `COMPTIME_BLOCK` | `body` (child) |
| `LABEL_STMT` | `name` (str), `stmt` (child) |
| `BREAK_LABEL_STMT` | `label` (str) |
| `CONTINUE_LABEL_STMT` | `label` (str) |
| `GOTO_STMT` | `label` (str) |
| `MULTI_ASSIGN` | `targets` (list), `values` (list), `count` (int), `is_short_decl` (bool) |

### Expressions

| kind | fields |
|---|---|
| `IDENTIFIER` | `name` (str) |
| `LITERAL` | `literal_type` (str, a token name such as `INT`, `STRING`, `NIL`), `value` (string, see "Strings are byte sequences" below), `length` (int, the byte count of `value`) |
| `BINARY_EXPR` | `op` (str, a token name such as `PLUS`), `left` (child), `right` (child) |
| `UNARY_EXPR` | `op` (str), `operand` (child) — this is also how `&x` and `*x` are represented (`TOKEN_BIT_AND` / `TOKEN_MULTIPLY`); there is no separate `ADDR_OF`/`PTR_DEREF` kind in the dump |
| `POSTFIX_EXPR` | `op` (str), `operand` (child) |
| `CALL_EXPR` | `function` (child), `args` (list), `has_spread` (bool), `type_args` (list of type-table ids — **always empty at the parse stage**, even for an explicit `Id[T](x)` instantiation, because there are no `Type*` values until type checking), `comptime_value_args` (list<int>, filled at parse time and present at both stages) |
| `INDEX_EXPR` | `expr` (child), `index` (child) |
| `SELECTOR_EXPR` | `expr` (child), `selector` (str) |
| `SLICE_LIT` | `elements` (list), `elem_type` (child) — this is the `AST_SLICE_EXPR` enum slot, reused for slice literals |
| `MAP_LIT` | `map_type` (child), `keys` (list), `values` (list) — this is the `AST_PAREN_EXPR` enum slot, reused for map literals |
| `SLICE_INDEX_EXPR` | `expr` (child), `low` (child), `high` (child) |
| `STRUCT_LITERAL` | `type_name` (str), `is_keyed` (bool), `field_names` (list<str>, empty if the literal is positional), `field_values` (list), `field_count` (int) |
| `ARRAY_LITERAL` | `array_type` (child), `elements` (list) |
| `KEYED_ELEMENT` | `key` (child), `value` (child) |
| `FUNC_LIT` | `params` (list), `return_type` (child), `body` (child), `captured_names` (list<str>) |
| `SLICE_CONVERSION` | `slice_type` (child), `operand` (child) |
| `TYPE_ASSERT` | `expr` (child), `asserted_type` (child) |
| `TRY_EXPR` | `expr` (child) |
| `CATCH_EXPR` | `expr` (child), `error_var` (str), `catch_body` (child) |
| `MATCH_EXPR` | `expr` (child), `cases` (list) |
| `MATCH_CASE` | `pattern` (child), `guard` (child, null with no guard clause), `body` (list) |
| `GUARD_CONDITION` | `condition` (child) |

### Type expressions

| kind | fields |
|---|---|
| `BASIC_TYPE` | `name` (str), `package` (str or null — a qualified name's package prefix, e.g. `sync.Mutex`) |
| `ARRAY_TYPE` | `length` (child — an expression node, see the unstamped-shapes table), `element_type` (child) |
| `SLICE_TYPE` | `element_type` (child) |
| `MAP_TYPE` | `key_type` (child), `value_type` (child) |
| `CHAN_TYPE` | `element_type` (child), `pattern` (str, one of `CHAN_PATTERN_BASIC`/`_PUB`/`_SUB`/`_REQ`/`_REP`/`_PUSH`/`_PULL`), `endpoint` (str or null) |
| `FUNC_TYPE` | `params` (list), `return_type` (child) |
| `INTERFACE_TYPE` | `methods` (list) |
| `STRUCT_TYPE` | `fields` (list), `is_result_tuple` (bool) |
| `ENUM_TYPE` | `variants` (list) |
| `ENUM_VARIANT` | `name` (str), `fields` (list) |
| `POINTER_TYPE` | `element_type` (child) |
| `REFERENCE_TYPE` | `element_type` (child), `is_mutable` (bool) |
| `UNSAFE_PTR_TYPE` | `element_type` (child) |
| `ERROR_UNION_TYPE` | `value_type` (child), `error_type` (child, null for a bare `!T`) |
| `NULLABLE_TYPE` | `base_type` (child) |

### Patterns

| kind | fields |
|---|---|
| `PATTERN` | `pattern_type` (str, one of the five below), plus the fields that pattern type carries |

`pattern_type` selects which further fields follow:

| `pattern_type` | further fields |
|---|---|
| `PATTERN_LITERAL` | `literal` (child) |
| `PATTERN_IDENTIFIER` | `name` (str), `id_type` (child, a type-expression node, or null) |
| `PATTERN_WILDCARD` | (none) |
| `PATTERN_DESTRUCTURE` | `type_name` (str), `fields` (list) |
| `PATTERN_TYPE` | `type_name` (str), `fields` (list) — same shape as `PATTERN_DESTRUCTURE` |
| `PATTERN_OR` | `patterns` (list) |

## The type table

`types` is an array of entries, each addressed by its position in the array
(its `"id"`). Every entry has, in order:

```
"id"      int, equal to the entry's own array index
"kind"    str, a TypeKind name (see below)
"size"    int, byte size
"align"   int, byte alignment
"package" str or null — the type's OWNER package (null for a builtin, an
          anonymous type, or a type declared in `main`); this is unrelated
          to which package a PACKAGE-kind entry itself represents (see the
          worked example above)
```

followed by kind-specific fields:

| `kind` | further fields |
|---|---|
| `ARRAY` | `element` (type id), `length` (int), `comptime_length` (bool) |
| `SLICE` | `element` (type id) |
| `MAP` | `key` (type id), `value` (type id) |
| `CHANNEL` | `element` (type id), `pattern` (str), `endpoint` (str or null) |
| `FUNCTION` | `params` (list<type id>), `return` (type id), `is_variadic` (bool), `has_comptime_params` (bool) |
| `POINTER` | `pointee` (type id) |
| `REFERENCE` | `referenced` (type id), `is_mutable` (bool) |
| `STRUCT` | `name` (str, present ONLY when the struct is nominal — an anonymous struct carries no `name` key at all, never a null one), `fields` (list of `{name, type (type id), offset (int), ownership (str), is_embedded (bool)}`) |
| `ENUM` | `name` (str, nominal only, same rule as STRUCT), `variants` (list of `{name, payload (type id or null), tag (int)}`) |
| `INTERFACE` | `name` (str, nominal only), `is_synthesized` (bool), `method_count` (int), `methods` (list of `{name, type (type id)}`) |
| `ERROR_UNION` | `value` (type id), `error` (type id or null) |
| `NULLABLE` | `base` (type id) |
| `QUALIFIED` | `base` (type id), `ownership` (str) |
| `PARAM` | `param_name` (str or null), `index` (int), `constraint` (type id or null) |
| every scalar (`VOID`, `BOOL`, `INT8`…`UINT64`, `FLOAT32`, `FLOAT64`, `STRING`, `CHAR`, `PACKAGE`, `UNKNOWN`, `POISON`) | `name` (str or null) — a fixed display string set once when the `Type` singleton is created, e.g. `"int64"`, `"void"`, `"string"` |

`TypeKind` has 35 members. 31 of them appear in the table above or the
scalar row; `TYPE_CONCEPT`, `TYPE_PARAM_HKT`, `TYPE_CONSTRUCTOR`, and
`TYPE_APPLICATION` abort by name if a live `Type*` of one of them ever
reaches the dump — they belong to the constraint-inference/HKT framework
that P5.6 unlinked from `bin/goo` (see this repository's `CLAUDE.md`,
"Key Features"), so a fixture reaching one is a real finding, not a
gap to patch quietly.

### The structural-id rule

An id is assigned to a type SHAPE, not to a `Type*` pointer. The checker
allocates a fresh `Type*` for most composite constructions (`type_slice`,
`type_map`, and so on all allocate on every call), so two nodes typed
`[]int` in the same file hold two different `Type*` values but share one
table entry. The rule, by kind:

- **Scalars** (`VOID`…`CHAR`, `UNKNOWN`, `POISON`, `PACKAGE`): the key is
  the kind alone. Every `int64` in a file is one entry.
- **Nominal types** (`STRUCT`, `ENUM`, `INTERFACE` with a non-null name):
  the key is kind + name + owner package name (`""` for no package). The
  id is assigned ON ENTRY, before the type's own fields are visited.
- **Composites** (`ARRAY`, `SLICE`, `MAP`, `CHANNEL`, `FUNCTION`,
  `POINTER`, `REFERENCE`, `ERROR_UNION`, `NULLABLE`, `QUALIFIED`,
  `PARAM`): the key is kind + the type's own scalar attributes (array
  length, channel pattern and endpoint, function variadic/comptime flags,
  and so on) + the ids of its component types, computed bottom-up (a
  component's id is resolved before it is folded into the parent's key).
- **Anonymous structs** (`STRUCT` with a null name): the key is kind plus
  every field's name and type id.

Two spellings of `[]int` therefore share one id; two structs named `Node`
declared in two different packages do not, because the owner package name
is part of the nominal key.

`Type.name` (a `snprintf`'d display string, truncated at 64 or 128 bytes
for `[10]int`-, `[]int`-, `map[K]V`-, and channel-shaped names —
`src/types/types.c:157,205,226,260,315`) is never part of a composite's
key or dump, exactly because it can truncate. Only scalar and nominal
entries use it, where it cannot.

**Cycle argument.** `struct Node { next *Node }` types `Node`'s own field
as a pointer back to `Node` itself. This does not loop forever because a
nominal type's key is name + kind + package alone — it never inspects the
struct's fields to build its key. `type_id(Node)` therefore completes and
is memoized in one step, with no recursion, before anything ever visits
`Node`'s fields and asks for `type_id(Node)` again for the `next` field. A
composite type can only cycle back to itself through a named type this
way, because every composite constructor in `types.c` builds from an
already-complete component — so no other kind needs an equivalent guard.

## The release plan

At the typed stage, `plan` on a file entry is `null` when
`GOO_ARC_RELEASE=0` left that file's plan unbuilt, or an array of one entry
per user-defined function/method:

```
{
  "function": "<function name>",
  "locals": [
    {
      "name": "<local variable name>",
      "verdict": "<one of the ReleaseVerdict names below>",
      "reasons": "<'|'-joined EscapeReasons flag names, or 'NONE', or 'UNCLASSIFIED'>",
      "owns_elems": <bool>
    },
    ...
  ],
  "owned_keys": [ [line, column, offset], ... ],
  "owned_concat_operands": [ [line, column, offset], ... ]
}
```

`verdict` is one of: `RELEASE_OK`, `RELEASE_NO_ESCAPES`,
`RELEASE_NO_NOT_OWNED`, `RELEASE_NO_ARENA`, `RELEASE_NO_LOOP_SCOPE`,
`RELEASE_NO_REBOUND`, `RELEASE_NO_ALIASED`, `RELEASE_NO_BLOCK_ESCAPE`,
`RELEASE_NO_NO_BINDING`, `RELEASE_NO_UNKNOWN` (`include/release_decision.h`).
`reasons` is diagnostic only — CLAUDE.md's memory-model section says
plainly that nothing in the compiler may branch on it, and a consumer of
the dump should treat it the same way: read it to explain a verdict, never
to derive one. `owned_keys` and `owned_concat_operands` are each a list of
source positions (not nodes), naming the map-index-assignment keys and
string-concatenation operands the plan judged to be fresh temporaries.

## The three invariants

**1. Determinism.** Key order is emission order — the JSON writer never
sorts or buffers a key. Two runs of the SAME input, in the SAME
environment, give byte-identical output. That last clause is exact:
`GOO_ARC_RELEASE=0` is the documented ARC kill switch. It changes what the
compiler does (it skips building a release plan), and the dump reports the
compiler faithfully by emitting `"plan": null` in that case. The dump is
not a function of the source alone — it is a function of the source AND
the compiler configuration, and a differential comparison of two dumps is
only meaningful when both were produced under the same configuration.

**2. The type table is closed under reference, and ids are structural.**
`types[i].id == i` for every entry. Every id any node's `"type"` field or
any type entry's own fields (`element`, `pointee`, a struct field's
`type`, and so on) names resolves to an entry inside the table — there is
no dangling reference. Because ids come from a structural key rather than
from visit order, one id always means one type shape: two spellings of
`[]int` share an id, and two structs with the same name declared in
different packages do not.

**3. Every unhandled kind aborts by name.** `emit_node`'s `default` arm
calls `die_ast_kind`, which prints `program-dump: unsupported AST node
kind <N> (<AST_KIND_NAME>)` to stderr and aborts the process; the type
table's kind-name lookup does the same for a `Type` it does not expect. A
node or type silently skipped, rather than refused, would let a
differential gate pass on a fixture it never actually compared — the
abort is what makes `scripts/program_dump_probe.sh`'s coverage claim
true.

## The version rule

`"goo_program_dump": 1` is the format version. A consumer MUST compare it
for equality and refuse any dump whose value it does not recognize —
never attempt to read a dump under a version number it was not written
for, even if the shape looks compatible.

This is the rule Charon enforces for the LLBC intermediate format that
Aeneas reads: Charon stamps every file it writes with a `CharonVersion`,
which Aeneas checks on deserialization, and Aeneas additionally pins the
exact Charon commit it was built against in a `charon-pin` file, rather
than trusting a compatible-looking version number alone. The Haskell
front end this dump exists to support, and any later consumer, inherit
the identical problem — a producer and a consumer built at different
times must have an explicit, checked way to know whether they still agree
on the shape.

## Not in this dump

- **Bodies of imported packages.** `compile_resolved_packages` runs before
  the dump hook, so an imported package's exported types appear in the
  type table wherever a local declaration references them, but the
  imported package's own declarations never appear in any file's
  `"decls"` — the dump only ever lists the files given on the command
  line.
- **Synthetic declarations.** `type_checker_declare_synthetic` is called
  only from codegen (`statement_codegen.c:3748,3969,4021`), after this
  dump's hook has already run, and it registers a `Variable` directly in
  a `Scope` — it never adds a node to the AST, so there is nothing for
  the walker to visit.
- **Monomorphized instances.** `codegen_generate_function_instance`
  (`monomorphize.c:129-223`) re-walks the SAME template AST once per
  instantiation. A generic function's body therefore carries only the
  abstract type stamps from the original type-check pass, never a
  concrete per-instantiation type. Phase 1 of the strangler plan moves
  monomorphization into the front end, which is expected to close this
  gap.

## Ten AST shapes with no type id in the typed stage

Each of the following is an `EXPR_KINDS` shape (a shape ordinarily
expected to carry `"type"` at the typed stage) that the type checker
resolves by name, by shape, or by constant-folding rather than through
the ordinary expression-checking path — so it legitimately carries no
`"type"` field. Each is allowlisted by name in
`scripts/program_dump_check.py`'s `known_unstamped` /
`_unchecked_subtrees`, with a comment naming one reproducing fixture.
Closing any of these (giving the shape a real type, or removing it from
`EXPR_KINDS`) is Phase 1 work, not Phase 0's.

| shape | why the checker never stamps it | fixture |
|---|---|---|
| A builtin/`make_chan`/`new`/`make`/named-type-conversion/explicit-generic-instantiation callee (an `IDENTIFIER` or `INDEX_EXPR` in a `CALL_EXPR`'s `"function"`) | `type_check_call_expr` recognizes the callee by name, by lookup, or by shape and handles the whole call itself, never calling the ordinary expression checker on the callee node | `examples/append_probe.goo`, `examples/chan_probe.goo`, `examples/named_type_conv_probe.goo`, `examples/generic_explicit_inst_probe.goo` |
| The type argument of `make`/`new`/`make_chan` (`args[0]`) | resolved via `type_from_ast`, a type-name lookup, not `type_check_expression` | `examples/chan_probe.goo` (`make_chan`), `examples/new_probe.goo` (`new`) |
| The blank identifier `_` | a discard target, never a resolved `Variable` | `examples/type_assert_valptr_probe.goo` |
| `TypeSwitchNode.bind_name` | the switch's own declaration-site name holder (the `v` in `switch v := x.(type)`), never independently type-checked as an expression | `examples/type_switch_probe.goo` |
| An `ARRAY_TYPE.length` expression | constant-folded (`goo_fold_const_int_ctx`) as a whole, so no descendant of it is stamped either | `examples/array_literal_probe.goo`, `examples/const_array_probe.goo` |
| A `KEYED_ELEMENT.key` | resolved via `goo_fold_const_int`, never `type_check_expression` | `examples/composite_widen_probe.goo` |
| An embedded interface name in another interface's method list | a type reference, not a checked expression | `examples/interface_embedding.goo` |
| A destructure pattern field name | a binding name (`case Num(v):` binds `v`), never a resolved value | `examples/match_probe.goo` |
| `case nil` in a type switch | `type_check_type_switch` explicitly skips the type-check stamp for this one case arm | `examples/iface_target_switch.goo` |
| A select case's send comm (`ch <- v`) as a whole | checked by evaluating its left and right operands individually, mirroring `codegen_setup_select_case`, never routed through `type_check_expression` as one node | `examples/select_send_closed_abort_probe.goo` |

## Grammar-reachable but unexercised kinds

Nine `ASTNodeType` values the parser can construct have no `case` arm in
`emit_node` today, because no fixture in the corpus reaches one. If a
future fixture does, the dump aborts by name (Invariant 3) rather than
silently dropping the node — that is designed behavior, not a defect to
fix on sight. Fields below are read from `include/ast.h`, for whoever adds
the arm:

| kind | fields |
|---|---|
| `AST_HKT_PARAM` | an `IdentifierNode` retagged to this kind: `name` (str) |
| `AST_CONCEPT_DECL` | `name` (str), `type_params` (list), `requirements` (child) |
| `AST_ASM_STMT` | `assembly_code` (str), `outputs` (list), `inputs` (list), `clobbers` (list<str>, from a `char**`/count pair) |
| `AST_ATOMIC_EXPR` | `expr` (child), `operation` (str), `operand` (child) |
| `AST_BARRIER_CALL` | `barrier_name` (str) |
| `AST_PARALLEL_FOR` | `init` (child), `condition` (child), `increment` (child), `body` (child), `schedule_type` (str), `chunk_size` (int) |
| `AST_PARALLEL_REDUCE` | `array` (child), `init_value` (child), `reduction_func` (child), `operation` (str) |
| `AST_THREAD_LOCAL_DECL` | `name` (str), `type` (child), `init_value` (child) |
| `AST_VOLATILE_EXPR` | `expr` (child) |

## Strings are byte sequences, not text

A Goo string literal can hold a byte that is not valid UTF-8 (a `"\xff"`
escape decodes to exactly that one byte). The JSON writer
(`src/ast/json_writer.c`) passes a well-formed UTF-8 sequence through
verbatim — a genuine multi-byte character such as `"café"` or `"中文"` is
untouched — but escapes any OTHER byte above `0x7F` as `\u00XX`, one
escape per byte, never as a decoded code point. A consumer must read such
an escape as one raw byte, not as a Unicode character. The `"length"`
field beside a `LITERAL` node's `"value"` is the value's byte count
(`l->length`, not a character count), and it is the check: re-encode
`"value"` back to bytes (undoing JSON's own escaping, including any
`\u00XX` byte-escape) and its length must equal `"length"`.

## Future work: one schema, not two hand-written readers

The Haskell and Zig readers this dump is meant to support should each get
their deserializer GENERATED from one shared schema, rather than each
being hand-written against this document separately. Charon takes the
same approach for the LLBC
format: its OCaml deserializer is generated from the Rust type
definitions that produce the format, rather than kept in sync by hand.
This document is accurate as of the version stamped in the envelope, but
a generated reader is the only way two independently maintained producers
stay honest about the same shape as the format grows.
