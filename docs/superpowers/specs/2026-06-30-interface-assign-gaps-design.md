# Interface assignment gaps — concrete→interface `=` and interface→interface

**Date:** 2026-06-30
**Branch:** `feat/v1-phase4-iface-gaps`
**Phase:** v1 Phase 4 (interfaces), follow-up to PR #48 (interface-parameter boxing)
**North star:** TinyGo-style `sort` — `sort.Sort(data Interface)` is pure-Go built
entirely on an interface; closing these gaps lets a hand-ported `sort.Sort`
compile end-to-end.

## Problem

After PR #48, a concrete implementer can be boxed into an interface at three
sites: var-decl init, return, and **call arguments**. Two assignment shapes
still produce clean rejections (no invalid IR, but not yet supported):

1. **Plain `=` assignment** of a concrete implementer into an interface-typed
   variable: `var s Shape; s = Sq{...}`.
2. **interface→interface** assignment / passing: `var a Shape = Sq{}; var b Shape; b = a`.

Both are needed by package-internal stdlib code (e.g. `sort.Sort` passing its
`data Interface` parameter onward to helpers typed `Interface`).

Out of scope (deferred follow-up): interfaces nested in slices/structs.

## Root cause

Plain `x = y` is represented as a single-element `AST_MULTI_ASSIGN` (there is no
dedicated single-assign node). The `=` branch of `type_check_multi_assign`
(`src/types/type_checker.c:940`) validates with raw `type_compatible(vt, tt)`,
bypassing the interface logic that var-decl already uses
(`check_interface_assign`, `type_checker.c:581`). Codegen
(`codegen_generate_multi_assign`, `src/codegen/statement_codegen.c:179`) stores
the rvalue directly with no boxing.

## Design

Three changes, each reusing existing machinery (no new satisfaction or boxing
logic):

### 1. Typecheck — `type_check_multi_assign` (`type_checker.c:940`)

In the `=` branch, when the target type `tt` is `TYPE_INTERFACE`, route through
the existing `check_interface_assign(checker, vt, tt, t->pos)` instead of raw
`type_compatible`. This single change covers BOTH:
- concrete→interface (runs `type_interface_satisfied`, emits "X does not
  implement Y" on a non-implementer), and
- interface→interface (already permissive — `check_interface_assign` returns 1
  at `type_checker.c:534`).

### 2. Codegen — `codegen_generate_multi_assign` (`statement_codegen.c:179`)

Before the `LLVMBuildStore` in the `=` (non-short-decl) branch: if
`target->goo_type->kind == TYPE_INTERFACE` AND the rvalue type `rtypes[i]` is
concrete (`!= TYPE_INTERFACE`), box via the existing
`codegen_interface_box(codegen, checker, target->goo_type, rtypes[i], rvals[i])`
and store the boxed `{vtable, data}` value. If the rvalue is already an
interface, store directly — matching layout, no box.

### 3. interface→interface through call-args / return

Verify the existing paths (`call_codegen.c:621` arg boxing, return boxing) pass
an already-interface value straight through (they skip boxing when the arg is
already `TYPE_INTERFACE`). Add a probe to lock the behavior; only touch code if
the probe fails.

## Testing (TDD, golden-probe driven)

New `examples/interface_assign_probe.goo`:
- declare an interface var, assign a concrete implementer via `=`, dispatch;
- assign that interface var to a second interface var via `=`, dispatch;
- assert printed output via `examples/interface_assign_probe.expected.txt`.

Wire as a `make` probe target and a golden case (`scripts/run_golden.sh`
discovers `examples/*.goo` + `.expected.txt`). Write it FAILING first, then
implement 1 → 2 → 3.

Gate: golden suite stays green (currently 77/0), `make test` 76/1,
`iface-parse-probe` + `iface-satisfaction-probe` still pass. ccomp-build is a
separate gate (CompCert not installed in this env).

## Alternatives considered

- **Dedicated single-assign AST node + separate typecheck/codegen path** —
  rejected: duplicates multi-assign logic and diverges from how `:=` already
  flows. Reusing `check_interface_assign` + `codegen_interface_box` keeps one
  definition of "what satisfies" and "how we box," continuing the consolidation
  PR #48 established (`type_interface_satisfied` promoted to public).

## Risk

The interface→interface store must use the correct LLVM struct type for the
target. Mitigated by the golden probe asserting real dispatched output, not just
a clean compile.
