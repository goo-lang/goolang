# &T{...} — Address of Struct Composite Literals

**Date:** 2026-07-02
**Status:** Approved (user, 2026-07-02)
**Scope decision:** Struct literals only. Array/slice/map literals get a clean
type error ("not yet supported") — follow-up if real code ever needs them.

## Problem

`return &Point{X: x, Y: y}` — the ubiquitous Go constructor idiom — fails with
`Cannot take address of non-lvalue` (codegen error, `expression_codegen.c`
`TOKEN_BIT_AND` arm). This is gap #2 in the 2026-07-01 capability assessment
and blocks most idiomatic Go ports that construct heap objects.

Root cause: the `&` lowering unconditionally asks
`codegen_emit_lvalue_address()` for the operand's storage address. A composite
literal is an rvalue — it has no storage. Go special-cases composite literals
as the one addressable rvalue; Goo's checker already lets `&<literal>` through
(`type_check_unary_expr` produces `type_pointer(operand_type)` for any
operand), so the gap is codegen-only plus a missing typecheck rejection for
the literal kinds we are NOT supporting.

## Approach (chosen: heap-allocate in the `&` codegen arm)

Considered:
- **A. Heap-allocate in codegen (chosen).** If the `&` operand is a struct
  composite literal, generate the literal value, `goo_alloc(sizeof(T))`,
  store, yield the pointer. ~30 lines; reuses proven patterns (heap locals in
  `function_codegen.c:168`, interface boxing alloc+store, `new` builtin's
  alloc call shape in `call_codegen.c:211`). Always-heap matches the
  leak-it-all runtime's cost model, and the escaping case (`return &Foo{...}`)
  is the entire point of the idiom.
- **B. Alloca + escape analysis.** Go-faithful performance, but generalizing
  the goroutine-specific M8b escape pass is a project of its own and a wrong
  answer miscompiles (dangling stack pointers). No observable benefit until
  there is a GC. Post-v1 follow-up.
- **C. Desugar to `new(T)` + field stores in the checker.** AST surgery
  mid-typecheck is against the codebase's grain (checker stamps types,
  codegen lowers); harder to extend later.

## Design

### Type checker — `type_check_unary_expr`, `TOKEN_BIT_AND` arm
(`src/types/expression_checker.c:745`)

Add a rejection for unsupported composite-literal operands BEFORE the generic
`type_pointer(operand_type)` result:
- Operand node kinds `AST_SLICE_EXPR`, `AST_ARRAY_LITERAL`, the map-literal
  node, or `AST_STRUCT_LITERAL` whose resolved type is not `TYPE_STRUCT`
  (a named-slice composite literal parses as AST_STRUCT_LITERAL and resolves
  to TYPE_SLICE — it must be rejected too, or it reaches struct codegen
  assumptions): clean error
  `"cannot take the address of a <kind> literal (only struct literals are supported)"`.
- `AST_STRUCT_LITERAL` resolving to `TYPE_STRUCT`: allowed; result stays
  `type_pointer(operand_type)` (already the behavior).
- All other operands: unchanged (identifiers keep borrow tracking; genuine
  non-lvalues still get codegen's existing error).

### Codegen — `codegen_generate_unary_expr`, `TOKEN_BIT_AND` case
(`src/codegen/expression_codegen.c:1370`)

Before the `codegen_emit_lvalue_address` call: if
`unary->operand->type == AST_STRUCT_LITERAL` and its `node_type` is
`TYPE_STRUCT`:
1. The generic operand generation at the top of the function has ALREADY
   produced the literal's aggregate value (`operand->llvm_value`, an rvalue)
   via `codegen_generate_struct_lit` — reuse it; do not re-generate (double
   evaluation would duplicate side effects in field expressions).
2. `goo_alloc(LLVMSizeOf(struct_llvm_type))` — same call shape as the `new`
   builtin arm; error out if `goo_alloc` is not in the module (mirrors
   interface boxing).
3. `LLVMBuildStore(literal_value, ptr)`.
4. Result: the pointer, `result_type = type_pointer(operand->goo_type)`.

Non-literal operands fall through to the existing lvalue path untouched.

### Error handling
- Checker rejects unsupported literal kinds (never reach codegen).
- Codegen errors on missing `goo_alloc` (module invariant, same as boxing).
- No new runtime C — no ccomp surface.

### Testing
- Golden probe `examples/addr_struct_lit_probe.goo` (+ `.expected.txt`):
  constructor function `func newPoint(x int, y int) *Point { return &Point{X: x, Y: y} }`
  (per-param types — multi-name `x, y int` params are parse gap #5, unrelated),
  field reads through the returned pointer, mutation through the pointer,
  a pointer-receiver method call, an inline positional `p := &Point{7, 9}`,
  and two constructed values alive at once (distinct allocations, no aliasing).
- Reject probe (Makefile target, pattern of `strindex-reject-probe`,
  `Makefile:471`): `&[]int{1}` must fail to compile with the new clean error,
  nonzero exit, no crash.
- Gates: `make verify` (golden N/0 + reject targets), `make test` (76/1),
  `make ccomp-link` PASS (should be trivially unaffected — no runtime change).

## Non-goals
- Escape analysis / stack allocation of non-escaping literals (post-v1).
- `&` on array/slice/map literals (clean error; follow-up on demand).
- Reclamation — allocations leak like every other allocation today (pre-GC).
