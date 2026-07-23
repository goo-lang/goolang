# Arena leg — Task 6: Push + free the arena at block exit (design)

Branch: `feat/arena-regions`. Depends on: 7a (`8a17857`), 7b (`7c1bf8c`), 7c (`7a1161c`),
plus the block-scope precondition (INVESTIGATED, SATISFIED — see the arena memory). This is
the FIRST task that actually frees memory: a mistake is a use-after-free or double-free, so
correctness is verified adversarially with valgrind.

## What changes

Rewrite `codegen_generate_arena_stmt` (`src/codegen/statement_codegen.c:2563`) from the
transparent pass-through to: allocate a real arena on entry, route the body's non-escaping
allocations into it (7c's gate already does this once an arena is on the stack), and free it
on the normal fall-through exit.

```c
int codegen_generate_arena_stmt(cg, checker, stmt) {
    ArenaBlockNode* ab = (ArenaBlockNode*)stmt;
    // goo_arena_new(i64 0) — 0 lets the runtime pick GOO_ARENA_DEFAULT_BLOCK_SIZE.
    // get-or-declare goo_arena_new : i8* (i64), and goo_arena_free : void (i8*)
    // (both already declared into the module by runtime_integration.c T4;
    //  use the LLVMGetNamedFunction-or-LLVMAddFunction pattern of codegen_emit_alloc).
    LLVMValueRef arena = <call goo_arena_new(i64 0)>;
    codegen_arena_push(cg, arena);                       // 7c gate now routes into `arena`
    int ok = codegen_generate_statement(cg, checker, ab->body);
    codegen_arena_pop(cg);                               // compile-time bookkeeping
    if (!ok) return 0;
    // Free ONLY on normal fall-through: any return/break/continue inside the body
    // already emitted a terminator and jumped away, so this block is unterminated
    // iff control fell off the end of the body.
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
        <call goo_arena_free(arena)>;
    }
    return 1;
}
```

## Why fall-through-only free is sound (the core argument)

- **No double-free / no free-then-use.** Each dynamic execution leaves the block by exactly
  one path. The free is emitted once, at the physical end of the body, reachable only when
  control falls through. A `return`/`break`/`continue` inside the body terminates its block
  and branches away *before* reaching the free — so on those paths the free never runs.
  Therefore the free executes at most once per execution, and always as the last thing before
  leaving the block — never before an in-block use.
- **Nothing reachable after the block lives in the arena.** Three independent guarantees:
  1. A variable declared inside the block is not in scope after it (type checker rejects
     post-block use; codegen truncates the value table) — the investigated precondition.
  2. A value that escapes to an outer name (assignment to an outer/global lvalue, `return`,
     closure capture, goroutine, retaining call) is marked escaping by 7b → routed to the
     heap by 7c, not the arena.
  3. A value *embedded in* an escaping value is also marked escaping — 7b's `expr_taint`
     unions a composite's field taints into the composite's own taint, so
     `return &Node{p: tmp}` marks both the `&Node` site AND `tmp`'s site escaping → both heap.
  So after the arena is freed, every still-live pointer refers to heap memory. Freeing the
  whole arena cannot dangle a live pointer.
- **Known limitation (documented, safe):** on an early-exit path (`return`/`break`/`continue`
  leaving the block) the arena is not freed — it leaks. This is safe (no UAF, no double-free)
  and no worse than today's allocate-and-leak baseline; it only forgoes reclaim on that path.
  Freeing on early-exit paths (defer-style cleanup before each terminator) is a later
  refinement. Note this does NOT use the function-scoped defer mechanism (which has the
  loop-slot limitation at statement_codegen.c:2007) — the free is inline, so an arena block
  inside a loop works: `goo_arena_new`/`free` run each iteration, fresh arena each time.

Nested `arena {}` blocks: the recursion handles them — each pushes/frees its own arena, and
`codegen_arena_current` (top of stack) routes an inner allocation to the innermost arena.

## Verification (UAF stakes — this is the load-bearing part)

### The valgrind invocation
The prototype is allocate-and-leak (`goo_alloc` never frees), so *every* heap allocation is a
valgrind leak. Leaks are therefore EXPECTED and must be ignored; only genuine memory-access
errors signal a bug. Use:

    valgrind --leak-check=no --error-exitcode=99 ./probe

This trips exit 99 on an **invalid read/write** (use-after-free of arena memory) or an
**invalid/double free** — and NOT on the expected leaks. That is exactly the UAF/double-free
gate we need.

### Golden + valgrind probe matrix (new `examples/*.goo` + a valgrind runner)
Each must (a) produce correct stdout (golden) and (b) be clean under the valgrind invocation
above. VERIFY each parses/compiles first; if a shape doesn't parse, use the closest parsing
equivalent and note it.

1. **arena-reclaim** — non-escaping allocs used only inside the block; block falls through;
   program continues and prints after. Proves the arena is freed on fall-through and later
   code is unaffected. (valgrind: no invalid access.)
2. **arena-escape-return** — `func f() *int { arena { p := new(int); *p = 7; return p } }`;
   `main` calls f, dereferences the result AFTER f returned, prints 7. Proves a returned
   arena-candidate is heap-promoted and survives. (valgrind: no invalid read of *result.)
3. **arena-escape-store** — outer/global `keep`; `arena { keep = new(int); *keep = 5 }`;
   deref `keep` after the block, print 5. Proves store-escape → heap survives.
4. **arena-embedded-escape** (the subtle one) — inside the block: `tmp := new(int); *tmp = 9;`
   build an escaping value that EMBEDS `tmp` (e.g. a struct with a pointer field set to `tmp`,
   returned or stored to an outer var); after the block, read through the embedded pointer and
   print 9. Proves an arena value embedded in an escaping value is itself promoted to heap
   (7b field-taint union). If the struct-embedding shape doesn't parse/lower, fall back to the
   simplest equivalent that still forces `tmp` to escape via embedding, and note it.
5. **arena-loop-reclaim** — a loop (e.g. 100000 iterations) whose body is an `arena {}` that
   builds several `new(...)`/`&T{}` temporaries used only within the block. Proves the
   per-iteration arena is freed each iteration (does not OOM / stays bounded). valgrind clean;
   additionally assert it runs to completion and prints a final marker. (RSS-flat capstone —
   a coarse bound is fine: it completes without unbounded growth.)

Wire a `arena-free-probe` target (compile + run + stdout diff for the golden cases) and an
`arena-valgrind-probe` target (runs the valgrind invocation over the compiled probes, asserts
exit 0 / no "Invalid read|Invalid write|Invalid free|double free" in output). Add both to
`.PHONY`, `verify`, and (binaries) `.gitignore`. If valgrind is unavailable in the
environment, the target must SKIP loudly (print a clear "valgrind not found — SKIPPED" and
exit 0), never silently pass.

### Regression
- Full golden suite still green; `arena_transparent_probe` still prints `42`/`after` (now with
  goo_arena_new/free emitted around it — stdout unchanged).
- `param-escape-test`, `block-escape-test`, `arena-routing-test` still green.
- Grammar tripwire 82 S/R + 256 R/R.
- Emitted IR for an arena block references `goo_arena_new` and `goo_arena_free` (sanity that
  the wiring actually fires now — the transparent handler emitted neither).

## Explicitly NOT in Task 6
- No free on early-exit paths (return/break/continue leaving the block) — that arena leaks
  (safe). Defer-style all-paths cleanup is a later refinement.
- No new site kinds beyond 7b's `new(T)`/`&composite`.
- No change to the escape analyses or 7c's gate.
