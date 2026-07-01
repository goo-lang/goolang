# Plan: make `int`/`uint` 64-bit (Go-spec conformance)

**Status:** proposed, awaiting review. **Branch:** `fix/int-uint-width` (spike reverted; currently clean at golden 136/0).

## Why
Go spec: `uint` is 32 or 64 bits, `int` is the same size as `uint`; on this 64-bit
target both are **64-bit**. Goo hardwires them to **32-bit**, so `int(uint32(0xFFFFFFFF))`
gives `-1` (should be `4294967295`) and `var x int = 3000000000` overflows. Fix = migrate
`int`/`uint` to 64-bit.

## Ground truth from the spike
- **Printing/runtime is already 64-bit-clean** — `goo_println_int(int64_t)`, `goo_print_uint(uint64_t)`, `goo_int_to_string(int64_t)`; the Println dispatch SExt/ZExts to 64-bit regardless of source width. **No runtime changes.**
- The type checker + codegen already have shape-based untyped-literal adaptation (`is_untyped_int_rooted` / `adapt_untyped_int_operand`) that keys on AST shape, **not** on the `TYPE_INT32` kind — so it does **not** need changing.
- The load-bearing risk is the handful of places that use `kind == TYPE_INT32` as a *proxy for "the untyped default"*. Those must move to the new default.
- Remapping the keyword ALONE breaks 12 golden probes; the full set below fixes all 12 with no probe changes outside these site groups.

## The one design decision
Goo represents an untyped integer constant by giving it a concrete default type (today
`TYPE_INT32`). We keep that approximation and simply move the default to **`TYPE_INT64`**
(Go's default type for an untyped integer constant is `int`). We do **not** introduce a
separate "untyped" flag in this migration — the existing shape-based adaptation already
covers operand adaptation; only the kind-based *sentinels* move from INT32→INT64.

Risk this leaves: `integer_binop_result_type`'s "the INT32 side yields to the sized operand"
rule becomes "the INT64 side yields" — which could let a *genuine* `int` yield to a `uintN`
in a mixed expression. In Go that mix is a compile error; Goo currently allows it. This is
pre-existing laxness, not a regression, and the golden+bits suites exercise the real mixed
cases (they pass today and must keep passing). Flagged for watch during Step 3.

## Steps (each rebuilds + runs `make test-golden`; the whole set lands as ONE commit because partial states fail)

**Step 1 — Keyword `int`/`uint` → 64-bit** (Cat 1)
- `src/types/expression_checker.c:740,745`
- `src/types/type_checker.c:1770,1775,1812,1817`
- `src/types/concept_generics.c:1403` (`type_int(32,1)`→`type_int(64,1)`)
- `src/comptime/comptime_types.c:174`; verify `comptime_intrinsics.c:70` flows through the resolver
- Leave the explicit `int32`/`uint32` name rows untouched.

**Step 2 — Untyped int-literal default → int64** (Cat 2)
- `src/types/expression_checker.c:392` (`type_check_literal` TOKEN_INT default → `TYPE_INT64`)
- `src/codegen/expression_codegen.c:234-267` (small-magnitude branch → emit `i64`/`TYPE_INT64`)
- `src/types/expression_checker.c:155` (empty `[]` literal element default → `TYPE_INT64`)

**Step 3 — Move the adaptation sentinels off INT32** (Cat 3, highest risk)
- `src/types/expression_helpers.c:117-118` — change the `== TYPE_INT32` "untyped default yields" checks to the new default (`TYPE_INT64`). Verify the shape-based adaptation still makes `1<<32`, uintN masks, and OnesCount/shift probes pass.
- `src/codegen/expression_codegen.c:251` — change the adapted-literal fast-path sentinel `nt->kind != TYPE_INT32` → `!= TYPE_INT64` so the fast path still fires for genuine int64 targets.

**Step 4 — `len`/`cap` and inferred-int results → int64** (Cat 6)
- `len`/`cap` return type: `src/types/type_checker.c:243,255`; `src/types/expression_checker.c:888-889`; codegen `src/codegen/call_codegen.c:249-252,265-268` (drop the i64→i32 truncation, return i64/TYPE_INT64).
- Inferred-"language int" result stamps (range index, select index, switch arm, etc.): review + change `TYPE_INT32`→`TYPE_INT64` at `type_checker.c:575,642,954,1428,1437`; `expression_checker.c:1304,1356-1363`; `statement_codegen.c:615,626,647`; `function_codegen.c:471,1079`; `error_union_codegen.c:470`; `lowlevel_codegen.c:106,171`; `type_level_programming.c:76`; `protocol_oriented_programming.c:841`. Each is "some inferred/builtin result is the language int". Change only those that are genuinely "int"; keep GEP/tag/ABI i32 (Cat 5 keep-list).

**Step 5 — DO NOT TOUCH (Cat 5 keep-list):** GEP/array-base/struct-field index i32 constants, error-union discriminant tag, wasm `memory.grow/size.i32`, C `main` i32 return, channel/MMIO/runtime ABI signatures, bool→i32 zext for printing. Full list in the spike map.

**Step 6 — Re-baseline + new probes**
- Re-run full `verify` + `test` + `ccomp-link`. For any golden whose output legitimately changes (values formerly wrapping at int32), regenerate `.expected.txt` and eyeball the diff.
- Add probes: `int_width_probe` (`var x int = 3000000000` prints 3000000000; `int(uint32(0xFFFFFFFF))` == 4294967295; large-int arithmetic) and a `len`-returns-int check (`var n int = len(s)` with a big-ish slice).

## Gate / rollback
- Whole migration = one atomic commit (partial states fail to compile — proven).
- Gate: `make verify` (golden) + `make test` + `make ccomp-link` all green before commit.
- Rollback = `git checkout` the touched files (spike already validated this returns to 136/0).

## Out of scope (follow-ups)
- `int` vs `int64` *type identity* (Go treats them as distinct types of equal size). This migration makes them the same width but does not enforce the identity distinction — consistent with Goo's current pragmatism (`rune == int32` today).
- `uintptr`, platform-conditional 32-bit `int` (we target 64-bit only).
