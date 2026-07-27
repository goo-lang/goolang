# Can a seeded shim method reach an interface vtable?

**Date:** 2026-07-28
**Method:** compile probes against `bin/goo` at `45352f4` (main, post-#232).
Record each failure exactly. Fix nothing.

**Why this question.** The `io` arc needs `os.Stdout` to satisfy `io.Writer`.
`os` is a C shim, so `os.File` would be a *seeded* type — minted by a
`seed_<pkg>_package_exports` function, with no Goo source and no AST method
body. Nothing in the tree has ever boxed a seeded type into an interface, so
whether the vtable path can even reach one was unknown.

## Verdict

**Yes, and the change is small — but it is not free.** Both halves of the
mechanism already ship. Neither is connected to the other. The connecting
change is about 30 lines in one function, plus a decision about where the
"seeded method to runtime symbol" mapping lives.

The pointer-identity property `os.Stdout` depends on is already correct.

## Finding 1 — the checker accepts, codegen fails

```go
type Locker interface { Lock(); Unlock() }

var mu sync.Mutex
var l Locker = &mu       // <- fails here
```

```
Error at spike_seeded_iface.goo:20:2: internal: missing method implementation for interface thunk
```

This is the predicted boundary, and the split matters:

- **The checker is already correct.** `type_interface_satisfied`
  (`type_checker.c:2187`) resolves each method by mangled name through the
  owner-routed `type_checker_lookup_method`. That is the P4.3 route that lets
  `kinds.Rect` satisfy `kinds.Shaper` across a package boundary. It finds
  `Mutex.Lock` in `sync`'s exports and reports satisfaction. No checker work
  is needed for the `io` arc.
- **Codegen is where it stops.** `build_thunk`
  (`interface_codegen.c:53-147`) resolves the implementation with
  `LLVMGetNamedFunction` on the mangled package symbol
  (`goo_pkg__sync__Mutex__Lock`). No seeded type ever defines such a symbol,
  so the lookup returns NULL and the function raises the diagnostic above.

## Finding 2 — seeded methods have no function at all, by design

A seeded method is not a function that happens to live elsewhere. It does not
exist as a callable Goo entity.

`mu.Lock()` lowers through `codegen_generate_sync_method_call`
(`call_codegen.c:479-502`), which is a name-matched `strcmp` chain:

| Receiver type name | Selector | Runtime symbol |
|---|---|---|
| `Mutex` | `Lock` | `goo_sync_mutex_lock` |
| `Mutex` | `Unlock` | `goo_sync_mutex_unlock` |
| `WaitGroup` | `Add` | `goo_sync_wg_add` |
| `WaitGroup` | `Done` | `goo_sync_wg_done` |
| `WaitGroup` | `Wait` | `goo_sync_wg_wait` |

So the mapping from a seeded method to its implementation exists in exactly
one place, and that place is a direct-call intercept that a vtable never
reaches.

## Finding 3 — the declare-and-link mechanism already ships

The missing piece is not new machinery. The `fmt.Fprint` intercept
(`call_codegen.c:1780-1792`) already does exactly what a thunk would need:

```c
LLVMValueRef ffn = LLVMGetNamedFunction(codegen->module, "goo_fwrite_string");
if (!ffn) ffn = LLVMAddFunction(codegen->module, "goo_fwrite_string", fty);
LLVMBuildCall2(codegen->builder, fty, ffn, fargs, 2, "");
```

Declare the symbol when it is absent, call it, and let the link against
`lib/libgoo_runtime.a` bind it. This path is gated today by every fixture that
writes to stderr.

`build_thunk` needs the same three lines, reached when `real_fn` is NULL but
the checker's `mvar` is not. That pair of conditions is precisely "the checker
knows this method exists, and no Goo function implements it" — which is the
definition of a seeded method, and needs no new flag to detect.

## Finding 4 — pointer boxing preserves identity (measured)

`os.Stdout` becomes a `*os.File`, so `Write` must act on the real descriptor
rather than a copy. Measured rather than assumed:

```go
c := Counter{n: 0}
var b Bumper = &c
b.Bump(); b.Bump()
fmt.Println(c.n)
```

| | output | exit |
|---|---|---|
| `bin/goo` | `2` | 0 |
| `go run` (go1.26.1) | `2` | 0 |

Byte-identical. A pointer boxed into an interface reaches the original object.
`codegen_interface_box`'s pointer form stores the pointer in the data word
instead of heap-copying the pointee, which is what makes this hold.

**Consequence for the arc.** `os.Stdout` and `os.Stderr` must be `*os.File`,
not `os.File`. That is Go's spelling anyway, so the constraint costs nothing —
but it is load-bearing, not cosmetic, and a value-form `os.File` would write
to a copy.

## Recommendation for Task 3

**Give the seeded-method mapping ONE owner.** The `strcmp` chain in
`call_codegen.c` is a lookup table wearing control flow. Both the direct-call
intercept and `build_thunk` need to answer the same question — "which runtime
symbol implements `<Type>.<Method>` for package `<p>`?" — so extract it into a
single table-driven function and route both callers through it.

This is not a style preference. This repository has now paid four times for
two call paths that answer the same question with hand-copied code:
`codegen_coerce_arg_to_param` (PR #220), `codegen_emit_variadic_pack`
(PR #224), `codegen_emit_fixed_call_arg` (PR #225), and the vendored-package
fourth loop (PR #230). A direct-call path and a vtable path that each carry
their own copy of the sync/os method table is the same shape, at the same
layer, in the same file.

The seeded-method table is also the natural home for the receiver convention:
`goo_sync_*` takes a `void**` slot, and `goo_os_file_write` should take the
file pointer plus the byte slice. One table row can carry both the symbol and
its receiver form.

## What this spike did NOT establish

Named honestly, because the arc depends on these and they stay open:

1. **No thunk was built against a runtime symbol.** Findings 1 to 3 show that
   each half works and that nothing structural blocks the join. They do not
   show a working `os.File` in a vtable. Task 3 must verify that end to end,
   and must verify it RED first.
2. **Boxing a `sync.Mutex` stays wrong regardless.** Value-boxing a mutex
   copies it, which Go's own vet rejects. The `Locker` probe is a diagnostic
   for this spike, not a feature request. Do not add sync to any vtable path
   without deciding the copy question separately.
3. **The interface-in-struct gap is untouched here.** Task 1 covers a promoted
   method reached through an embedded interface, which is a DYNAMIC dispatch.
   `build_thunk`'s promotion path (`interface_codegen.c:118-142`) re-mangles
   against the owning embedded type and assumes a static symbol, so it cannot
   serve that shape either. Same function, different missing case.
