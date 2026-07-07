# Arena leg — Task 7c: Thread the block-escape decision into codegen_emit_alloc (design)

Branch: `feat/arena-regions`. Depends on: 7a (`8a17857`), 7b (`7c1bf8c`). Followed by: 6
(turn on arena push+free at block exit — the first point anything is actually freed), 8
(adversarial valgrind probe).

## Why this is safe to land now

7c wires the escape gate into the allocation choke point, but the arena stack is **empty for
every program until Task 6** (nothing pushes an arena yet — T3/T4/T5 are inert on that
axis). So `codegen_arena_current()` is always NULL, the gate never selects the arena, and
every allocation still routes to `goo_alloc` exactly as today. The change is **inert but
wired** — identical to how T3 added the routing structure without anything exercising it.
The regression gate is therefore strict: the full golden suite must stay **byte-identical**.

Landing 7c *before* 6 is the safety ordering: when 6 finally pushes an arena, the escape gate
is already in place, so escaping allocations route to heap *by construction* — 6 never has an
intermediate state that could route an escaping value into an about-to-be-freed arena.

## The gate

`codegen_emit_alloc` currently routes to the arena when `codegen_arena_current() != NULL &&
kind == ALLOC_KIND_DEFAULT` (T3). 7c adds one clause: **and the site does not escape its
block**. Because `block_escape_site_escapes` returns `true` on a NULL/unknown site, an
unclassified site (or a caller that passes NULL) falls through to heap automatically.

New public predicate (the testable seam):

```c
// True iff an allocation for `alloc_site` should be routed to the active arena.
bool codegen_arena_eligible(CodeGenerator* cg, ASTNode* alloc_site, AllocKind kind) {
    return codegen_arena_current(cg) != NULL
        && kind == ALLOC_KIND_DEFAULT
        && !block_escape_site_escapes(cg->block_escape, alloc_site);
}
```

`codegen_emit_alloc` gains an `ASTNode* alloc_site` parameter and uses this predicate in
place of its inline `current_arena && kind == DEFAULT` check.

## Wiring

1. **CodeGenerator** gains `struct BlockEscapeResult* block_escape;` (tail-appended after
   `arena_depth` per the no-header-deps convention; forward-declare `struct
   BlockEscapeResult;` in codegen.h). Initialized NULL in `codegen_new`. Freed in
   `codegen_free` (`block_escape_result_free`).

2. **Run the analyses at codegen entry** — in `codegen_generate_program`, right after the
   `AST_PROGRAM` check / `codegen_initialize_target`, on the SAME `program` AST codegen will
   emit from (so site AST-node pointers match by identity):
   ```c
   if (codegen->block_escape) block_escape_result_free(codegen->block_escape); // re-entry guard
   ParamEscapeResult* pe = param_escape_analyze(program);
   codegen->block_escape = block_escape_analyze(program, pe); // does NOT retain pe
   param_escape_result_free(pe);
   ```
   Analysis is an optimization: if either returns NULL (OOM), leave `block_escape` NULL and
   continue — the gate then treats every site as escaping (heap), i.e. fails safe. Do NOT
   abort codegen on analysis failure. (`codegen_generate_program` may run once per package —
   the re-entry guard frees any prior result before overwriting.)
   Precondition already satisfied: `type_check_program` runs before
   `codegen_generate_program` in every pipeline path (`src/main.c`, `src/compiler/goo.c`), so
   `FuncLitNode.captured_names[]` is populated before `block_escape_analyze` reads it.

3. **`codegen_emit_alloc` signature** gains `ASTNode* alloc_site`. Update all 11 callers:
   - `src/codegen/call_codegen.c:~406` (`new(T)`): pass `expr` — the `AST_CALL_EXPR` node
     `block_escape` recorded as the site.
   - `src/codegen/expression_codegen.c:~2396` (`&<composite>`): pass `expr` — the
     `AST_UNARY_EXPR` node `block_escape` recorded.
   - The other 9 callers (interface/map boxing, closure env, go-arg, slice data,
     local-promoted — `codegen.c:613/1220/1241`, `interface_codegen.c:504`,
     `function_codegen.c:207/221/585`, `statement_codegen.c:1859`, `composite_codegen.c:1186`):
     pass `NULL`. Those forms are not classified sites this cut → NULL → heap, unchanged.

## Test — `arena-routing-test` (C unit test, no LLVM builder needed)

`tests/unit/codegen/arena_routing_test.c`, table-driven. For a Goo program with an arena
block containing at least one non-escaping site and one escaping site (e.g.
`func f() *int { arena { keep := new(int); tmp := new(int); _ = tmp; return keep } }`):
parse → `type_check_program` → `param_escape_analyze` → `block_escape_analyze`. Then build a
lightweight `CodeGenerator` WITHOUT full LLVM init — `calloc(1, sizeof(CodeGenerator))`, set
`cg->block_escape = result` — since `codegen_arena_eligible` touches only
`arena_stack`/`arena_depth`/`block_escape`, never the builder/module. Assert:

- With a dummy arena pushed (`cg->arena_stack[0] = (LLVMValueRef)0x1; cg->arena_depth = 1;`):
  for every `result->decisions[i]`, `codegen_arena_eligible(cg, decisions[i].site,
  ALLOC_KIND_DEFAULT) == !decisions[i].escapes_block` — i.e. the gate mirrors the analysis
  (non-escaping → arena-eligible, escaping → not).
- With the stack empty (`arena_depth = 0`): `codegen_arena_eligible(...) == false` for every
  site (nothing routes to arena when no arena is active — proves the inert-today property).
- `codegen_arena_eligible(cg, NULL, ALLOC_KIND_DEFAULT) == false` (NULL site → heap).
- `codegen_arena_eligible(cg, site, ALLOC_KIND_<non-default>) == false` if another AllocKind
  exists; otherwise skip.

Wire `arena-routing-test` into `.PHONY`, `verify` (next to `block-escape-test`),
`clean-tests`, `.gitignore`.

## Regression gate (the load-bearing check for an inert change)

- Full golden suite byte-identical (the gate never fires — arena stack empty). Build clean;
  `param-escape-test` + `block-escape-test` still green; `examples/arena_transparent_probe`
  still 42/after; grammar tripwire 82/256.

## Explicitly NOT in 7c

- No arena is pushed or freed (that is Task 6). The gate is wired but dormant.
- No new site kinds beyond 7b's `new(T)` / `&composite`.
- The block-scope-teardown precondition for Task 6 (a block-local that leaks past the block
  would be a UAF once the arena is freed) is NOT addressed here — it is a Task 6 gate.
