# fmt.Println struct / pointer-to-struct formatting (design)

Goal: let `fmt.Println` format a struct value (`{f0 f1 …}`) and a pointer-to-struct
(`&{f0 f1 …}`), Go-style. This is independently useful AND activates the arena 7a′
whitelist end-to-end: `fmt.Println(node)` with `node := &T{…}` now type-checks/lowers, so a
non-escaping arena struct can be printed and stay in the arena (7a′ already marks fmt.Println
non-retaining; today no program can exercise it because fmt rejects pointer/struct args).

## Where

`src/codegen/call_codegen.c`, the variadic `fmt.Println` arg loop (~2105–2244). Today it
type-dispatches each arg inline (string/int/uint/bool/float/error/interface) and errors on
anything else (~2234).

## Change

1. Extract the per-arg dispatch into a recursive static helper:
   `static int codegen_emit_fmt_value(CodeGenerator* cg, TypeChecker* checker,
    LLVMValueRef val, Type* ty, int depth, Position pos)` — emits the no-newline print calls
   for ONE already-loaded value; returns 1 ok / 0 on error (emitting a source-located
   codegen_error). Move the existing string/int/uint/bool/float/error/interface cases into
   it verbatim. The Println loop keeps its lvalue-load, space-between-args, and trailing-
   newline logic and just calls the helper per arg with `depth = 0`.

2. New cases in the helper:
   - **TYPE_STRUCT** — `goo_print("{")`; for each field i in `ty->data.struct_type.fields`
     (`field_count`): if i>0 `goo_print(" ")`; `LLVMValueRef fv = LLVMBuildExtractValue(val, i)`;
     recurse `codegen_emit_fmt_value(cg, checker, fv, fields[i].type, depth+1, pos)`; then
     `goo_print("}")`.
   - **TYPE_POINTER whose `data.pointer.pointee_type` is TYPE_STRUCT** — emit a nil check
     (`icmp eq val, null` → branch to a `fmt.nil` / `fmt.nonnil` block, both jumping to a
     `fmt.cont` block; no phi needed, just print side effects): nil → `goo_print("<nil>")`;
     non-nil → `goo_print("&")`, load the struct (`LLVMBuildLoad2(struct_llvm, val)`), recurse
     on the pointee struct type at depth+1. Continue in `fmt.cont`.
   - **depth > 6** (recursion cap, REQUIRED — the formatter recurses over TYPES at codegen
     time, so a self-referential struct like `Node{ next *Node }` would recurse forever
     without this): `goo_print("...")` and return 1. Pick 6.

3. Out of scope (keep the existing clean error): pointer-to-non-struct (`*int` etc. — Go
   prints an address, non-deterministic; not needed for the arena case), slices, maps,
   arrays, functions. Error message can stay but add "struct, pointer-to-struct" to the
   supported list.

4. Apply the same helper to `fmt.Print`/`fmt.Printf` ONLY if they already share this arg
   loop; if they are separate code paths, leave them and note it (Println is the motivating
   case).

Use `goo_print` (takes a `const char*`; build the literal with
`LLVMBuildGlobalStringPtr` or the existing constant-string helper) for the punctuation
`{` `}` `&` ` ` `<nil>` `...`. Get `goo_print` via `LLVMGetNamedFunction(module,"goo_print")`
(already used in this function).

## Tests

1. **Golden** `examples/fmt_struct_probe.goo` (+ `.expected.txt`), wired into `test-golden`
   auto-discovery (just add the files):
   ```goo
   package main
   import "fmt"
   type Point struct { x int; y int }
   func main() {
       p := Point{x: 3, y: 4}
       fmt.Println(p)          // {3 4}
       q := &Point{x: 5, y: 6}
       fmt.Println(q)          // &{5 6}
   }
   ```
   expected:
   ```
   {3 4}
   &{5 6}
   ```
   Also add a struct with mixed scalar field types (int, string, bool) to prove per-field
   dispatch, e.g. `{7 hi true}`.

2. **End-to-end 7a′ activation** `examples/arena_fmt_println_probe.goo` — the payoff:
   ```goo
   package main
   import "fmt"
   type Node struct { v int }
   func main() {
       arena {
           node := &Node{v: 42}
           fmt.Println(node)   // &{42}
       }
       fmt.Println("after")
   }
   ```
   expected `&{42}\nafter`. VERIFY via `bin/goo --emit-llvm` that `node`'s `&Node{}` routes
   to **`goo_arena_alloc`** (not `goo_alloc`) — i.e. the whitelist keeps it in the arena now
   that fmt.Println accepts it. valgrind-clean (`--leak-check=no --error-exitcode=99`). Wire
   this probe into `arena-free-probe` + `arena-valgrind-probe` (bump the counts) — it is the
   first compilable end-to-end proof that 7a′ works.

3. Regression: full `make test-golden` green; `arena_transparent_probe` still 42/after;
   analysis tests (param/block-escape) still green; grammar tripwire 82/256.

## Notes

- Deterministic: struct-of-scalars prints identically every run (no addresses).
- Soundness: this only ADDS lowering for previously-rejected arg types; existing scalar
  paths are byte-identical (verify a golden's `.ll` spot-check if convenient).
