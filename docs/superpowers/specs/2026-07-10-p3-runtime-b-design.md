# Phase 3 sub-project B — defer-in-loop, method values, globals close-out (P3.4, P3.6, P3.7)

Date: 2026-07-10. Branch: `feat/p3-runtime-b`. Roadmap: docs/2026-07-08-v1-roadmap.md Phase 3.
Prereq: PR #171 (sub-A) merged.

## Recon facts (verified 2026-07-10)

- **Defer today is fully static**: one entry-block arg-snapshot alloca + i1 active-flag per lexical
  defer (`statement_codegen.c:2511-2695`), emitted LIFO inline at 8 exit sites (6 return-shape sites
  in statement_codegen.c + fall-off-the-end at function_codegen.c:581/:1229) by
  `codegen_emit_deferred_calls` (`:2425`). Loop rejection at `statement_codegen.c:2532-2537`
  (`cfctx.loop_depth > 0`). Arg/receiver snapshots are defer-time (Go-correct). No runtime defer
  symbols exist.
- **Method values**: `type_check_selector_expr` returns the method's TYPE_FUNCTION with the
  receiver spliced as params[0] (`expression_checker.c:4325-4338`); the receiver is only stripped
  when the callee is itself a method selector (`:3449-3476`) — a bound `f` mis-checks arity at
  `:3886`. Codegen has no value-position selector-method path (`composite_codegen.c:389` fields
  only). Func values are universal fat pointers `{fn_ptr, env_ptr}` (env FIRST param) with an
  env-ignoring thunk for named functions (`function_codegen.c:262`, `type_mapping.c:325`); the
  closures spec explicitly deferred method values.
- **P3.7 is already satisfied for the main package**: non-constant initializers (identifiers,
  calls, binops, non-const composites) are deferred into a synthesized `goo.global_init()`
  (`function_codegen.c:1355-1383, 2292`) called in main's prologue right after
  `goo_os_args_init` (`:1002-1029`), declaration order preserved. Verified live:
  `var counter = compute()` + `var doubled = counter * 2` → 42/84. The rejection at
  `function_codegen.c:2141-2159` fires ONLY for source-compiled non-main packages (one
  program-wide init function exists; per-package init ordering is a P4 concern).

## Design decisions (Fable, 2026-07-10)

### B1 (P3.4) — runtime defer stack, per-function fork

**Chosen: per-function mechanism fork.** A function whose defers are all loop-free keeps today's
static machinery unchanged (byte-identical IR — differential-gated). A function containing at
least one loop-nested defer routes ALL its defers through a new runtime stack.

Why not alternatives: (a) uniform runtime stack for every function — cleaner single mechanism,
but churns IR for every existing defer site and adds heap traffic to the common case for zero
user-visible gain; (b) static machinery + runtime stack coexisting within one function — REJECTED
as unsound: global LIFO across a mixed top-level + loop defer sequence cannot be honored when half
the entries are inline emissions and half live on a stack. The per-function fork keeps exactly one
mechanism per function, which is the LIFO-correctness boundary.

**Runtime API** (new `src/runtime/defer.c` + declarations in include/runtime.h, linked into the
runtime archive like channels.c):

```c
typedef struct goo_defer_frame {
    struct goo_defer_entry* entries;  // {void (*fn)(void*); void* env;}
    size_t len, cap;
} goo_defer_frame_t;
void goo_defer_push(goo_defer_frame_t* f, void (*fn)(void*), void* env);  // grows via realloc
void goo_defer_run(goo_defer_frame_t* f);  // LIFO; frees each env after its call; frees entries
```

**Codegen (stack-mode functions):**
- Entry block: alloca a zeroed `goo_defer_frame_t`.
- Per lexical defer statement: synthesize a private `void @<fn>__deferN_thunk(ptr env)` whose body
  unpacks the env struct (snapshotted args + receiver, same fields the static path snapshots
  today) and makes the call; at the defer site: heap-allocate the env (goo_alloc), store the
  defer-time snapshots, `goo_defer_push(frame, thunk, env)`. Executing the statement IS the
  registration — the static path's active-flag semantics fall out for free, and each loop
  iteration pushes a fresh env (per-iteration snapshots, Go-correct).
- Exit sites: in stack-mode, every site that today calls `codegen_emit_deferred_calls` calls
  `goo_defer_run(frame)` instead, preserving the existing relative order with
  `codegen_emit_arena_frees` exactly as the static path has it.
- Panic interaction: unchanged (v1 panics abort without running defers; recover is post-v1).

**Acceptance/probes:** `for i:=0;i<3;i++ { defer fmt.Println(i) }` prints 2,1,0 at exit; mixed
top-level-then-loop defers unwind strict global LIFO; defer under `if` inside a loop only pushes
when executed; method-receiver defer in a loop snapshots per iteration; early `return` runs the
stack; loop-free functions' IR is byte-identical before/after (differential on existing
defer/spmd_defer probes); hard error at statement_codegen.c:2532 removed.

### B2 (P3.6) — method values as receiver-carrying thunks

- **Typecheck**: when a method selector appears in non-call position, return the method type with
  the receiver stripped (params[1..]) so `f := c.get` gives `func() int`; the existing
  method-call path (`:3449-3476`) is untouched. Value receivers: receiver snapshotted at bind
  time (copy semantics, Go-correct). Pointer receivers: env carries the pointer;
  addressable-auto-& per the method-set rules from P2.1.
- **Codegen**: extend the named-function-as-value mechanism — emit (once per (type, method) pair)
  a thunk `void/T @T__m__bound_thunk(ptr env, args...)` that loads the receiver from env and
  tail-calls the mangled `T__m`; a method value is `{thunk, env}` where env is a heap cell
  holding the bound receiver. Value-position selector codegen gains a method arm beside the
  field walk (`composite_codegen.c:389`).
- **Acceptance/probes:** `f := c.get; f()` returns c.n; receiver mutation after bind is invisible
  through a value-receiver method value but visible through a pointer-receiver one; method value
  passed as a callback parameter and called in another function; wrong-arg-count no longer
  reported; golden probe for each.

### B3 (P3.7) — close out with probe + docs

No mechanism work. Extend/refresh `examples/global_init_probe.goo` (its header still claims call
initializers are rejected — stale) to pin: call initializer, dependency chain in declaration
order, composite with call element. Roadmap row updated: satisfied for the main package;
per-imported-package init explicitly moved under P4.2/P4.3 scope. The rejection message at
function_codegen.c:2141-2159 stays for source packages (accurate) but gains "in imported
packages" wording so it can't be read as a blanket restriction.

## Gates

Per-commit: make lexer + make test + golden/reject suites green; tripwire 121/256 exact (no
grammar changes expected in any of B1-B3 — defer/method-value syntax already parses).
B1 additionally: IR differential on loop-free defer probes (byte-identical).
Pre-PR: make verify-core ALL GREEN; fresh-context review wave scaled to branch risk.
