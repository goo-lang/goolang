# Per-Type × Per-Value Composition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A function may declare both generic type parameters and comptime value parameters (`func kernel[T any](comptime n int, data T) T`), monomorphized per (type-args × comptime-values) tuple, so `var buf [n]T` works with the real per-instance length and element type — the SPMD "per-type kernel × per-tile" shape.

**Architecture:** Extend the existing two single-axis mechanisms; no new subsystems. (1) Checker: lift the comptime+type-params declaration wall; capture comptime values inside `type_check_generic_call` via a helper shared with `type_check_call_expr`; record composed seeds on `GenericInstantiation` (tail-extended with value payload). (2) Monomorphizer: combined mangler (`base__<typetok>...__n<v>...`), instance generator installing BOTH substitution envs, combined case in the nested-discovery recursion threading the same `seen`/`stamped_count`. (3) Call sites: three-way rewrite (combined/generic/comptime) in call_codegen and go-stmt (building the go-stmt generic half, which doesn't exist yet).

**Tech stack:** C23; existing monomorphizer (`src/codegen/monomorphize.c`), checker (`src/types/`), LLVM-C codegen.

**Spec:** `docs/superpowers/specs/2026-07-08-comptime-generic-composition-design.md` — read it first; its "Mechanism" section is normative and carries the file:line anchors.

## Global Constraints

- C23. Return errors, don't panic (except tests). No naked returns.
- New struct fields at the STRUCT TAIL (no-header-deps convention). After ANY header edit: `make clean && make lexer`.
- **No parser.y / lexer changes are expected in this entire plan.** If you believe one is needed, STOP (BLOCKED). `./scripts/grammar-tripwire.sh` must stay `82 S/R + 256 R/R` exact — run it in every task's regression step as a pure guard.
- Diagnostics are clean type errors — never "Module verification failed"/LLVM verifier noise reaching the user. The reject matrix greps for verifier text; keep it that way.
- Every task ends green: `make lexer`, the task's tests, `make test-golden` (313/0 at plan start; grows in Task 4), `make comptime-value-reject-matrix` (16/16 at start; grows in Task 4), `make test` (76 pass/1 pre-existing skip).
- Commits: conventional prefixes, `--no-gpg-sign`.
- Both single axes are FROZEN behavior: generic-only and comptime-only programs must be byte-for-byte unchanged (the golden suite pins much of this; controls in each task pin the rest).

## File structure

- `include/types.h` — `GenericInstantiation` tail: `int64_t* comptime_values; size_t comptime_value_n;`
- `src/types/type_checker.c` — lift wall (a) (~807-828); new `comptime n T` wall; recorder extension + free path (~113-136, 475-487)
- `src/types/expression_checker.c` — shared capture helper; capture in `type_check_generic_call` (~2426-2606); first-visit contract there
- `src/codegen/monomorphize.c` — combined mangler; combined instance generator; combined recursion case; driver
- `src/codegen/call_codegen.c` — three-way rewrite (~1451-1505)
- `src/codegen/statement_codegen.c` — go-stmt generic + combined rewiring (~1719-1744)
- `examples/comptime_generic_compose_probe.goo` (+ `.expected.txt`) — Task 4
- `Makefile` — matrix additions (Task 4)

---

### Task 1: Checker — declare, capture, and record both axes

**Files:** `include/types.h`, `src/types/type_checker.c`, `src/types/expression_checker.c`. Tests: `/tmp/t1_*.goo` throwaway checks (committed probes in Task 4).

**Interfaces produced:** `GenericInstantiation.comptime_values/comptime_value_n` (tail, independently owned, freed in the list teardown); composed seeds recorded at generic call sites; new declaration wall for `comptime n T`. Consumed by Tasks 2–3.

- [ ] **Step 1: Failing test.** `/tmp/t1.goo`: `func kernel[T any](comptime n int, data T) T { return data }` + `func main() { _ = kernel(4, 10) }`. Run `bin/goo -o /tmp/t1 /tmp/t1.goo` — expect TODAY: `comptime parameters are not yet supported together with type parameters`. Target end-of-task: past that wall (may still fail in codegen — Task 2's job; assert only the wall is gone and no capture crash).
- [ ] **Step 2: Extend `GenericInstantiation`** (include/types.h:455-460) with the two tail fields + doc comment (ownership: malloc'd copy owned by the list; 0/NULL for generic-only seeds). Update `type_check_record_instantiation` (type_checker.c:475-487) to accept and copy the value payload (new signature or a sibling `..._with_values` — pick what matches house style), and the teardown (~113-136) to free it. `make clean && make lexer` after the header edit.
- [ ] **Step 3: Lift wall (a)** (type_checker.c:807-828): delete the rejection; in its place, add the NEW wall — walk comptime params and reject any whose declared type node resolves to/contains a type parameter: `comptime parameter type cannot be a type parameter (not yet supported)`. Keep the sibling method/package walls (830-888) untouched.
- [ ] **Step 4: Verify template body-check coexistence.** With wall (a) lifted, `type_check_function_decl` runs BOTH the type-param push (~1038-1052) and the comptime placeholder binding (~1105-1127) for a composed function. Read both blocks; confirm the save/restore of `active_type_params` is LIFO-consistent with the placeholder path (spec open-point 4). Fix ordering if broken; note findings in your report either way.
- [ ] **Step 5: Shared capture helper.** Extract the per-argument comptime validation/capture from `type_check_call_expr`'s loop (expression_checker.c:3385-3465: `func_decl_param_at`, two-tier fold, realloc-append to `call->comptime_value_args`, error paths) into a static helper; `type_check_call_expr` calls it (behavior identical — verify with the sub-project-1 accept/reject regressions).
- [ ] **Step 6: Capture in `type_check_generic_call`** (~2426-2606): honor the first-visit contract (`expr->node_type == NULL`, mirroring 2662-2672 including the free/zero on first visit); in the per-arg loop, a position whose param `is_comptime_param`: call the helper (validate + capture), and EXCLUDE the position from unification (no `unify_types` for it); non-comptime positions unchanged. After the loop, record ONE seed carrying both payloads via the extended recorder. Generic-only calls must record exactly as before (0 values).
- [ ] **Step 7: Reject regressions.** `/tmp/t1_reject.goo` (runtime arg to comptime position of a generic call: `x := 5; _ = kernel(x, 10)`) → the sub-project-1 diagnostic `argument to comptime parameter 'n' must be a compile-time constant`. `/tmp/t1_tparam.goo` (`func bad[T any](comptime n T) T {...}`) → the new wall. Generic-only (`Id[T any]`) and comptime-only (`fill`) controls unchanged.
- [ ] **Step 8: Regression + commit.** Golden 313/0, matrix 16/16, unit 76/1-skip, tripwire exact. `git commit --no-gpg-sign -m "feat(types): capture and record composed generic+comptime instantiations"`.

---

### Task 2: Monomorphizer — combined instances

**Files:** `src/codegen/monomorphize.c`. Tests: `/tmp/t2_*.goo`.

**Interfaces consumed:** Task 1's composed seeds. **Produced:** combined mangled symbols + generated instances; consumed by Task 3's rewiring (until Task 3, calls won't dispatch — this task's test asserts INSTANCE GENERATION via IR, not execution).

- [ ] **Step 1: Failing test.** `/tmp/t2.goo` = Task 1's t1.goo with a `[n]T` body (`var buf [n]T; ...` summing loop). Expect TODAY: type-checks (Task 1) but codegen fails or misdispatches. Target: `bin/goo --emit-llvm` shows `kernel__int64__n4` (or the mangler's actual int token) with a `[4 x ...]` alloca.
- [ ] **Step 2: Combined mangler.** `codegen_mangle_combined_instance(base, Type* const* targs, size_t nt, const int64_t* values, size_t nv)`: type tokens first (reuse `codegen_type_mangle_token`), then `__n<v>` segments (reuse the comptime marker). Model on both existing manglers (monomorphize.c:61-68, 160-174); document the arity-fixed collision argument from the spec.
- [ ] **Step 3: Combined generation.** Extend `codegen_generate_function_instance` (76-153) to take the value payload (0/NULL = today's behavior): install `active_comptime_values/active_comptime_value_n` alongside `active_subst` and the checker-side type-param push; SAVE/RESTORE BOTH axes' fields unconditionally (the map flags today's generators leave the other axis's fields untouched — a leak either way is a bug). The mirror-scope rebinding (function_codegen.c:1006-1062) and `[n]T` re-derivation (1543-1566) then work unchanged — verify, don't reimplement.
- [ ] **Step 4: Driver + recursion.** In `codegen_monomorphize`'s generic-seed loop (766-772): a seed with `comptime_value_n > 0` routes to the combined path (mangler + generator), threading the SAME `seen`/`stamped_count`. In `mono_instantiate` (552-644): the nested-call combined case — both `type_arg_count > 0` and `comptime_value_arg_count > 0` → substitute type args under the enclosing env, concrete-check, recurse combined. `collect_generic_calls` already collects these nodes (389-406) — verify, don't duplicate.
- [ ] **Step 5: IR verification.** t2 at two value×type combos (`kernel(4, 10)`, `kernel(2, 10)`, plus a second type if float64 literals infer cleanly): distinct symbols per tuple, dedup on a repeated tuple, distinct alloca sizes. Execution NOT expected yet (call sites unwired) — if the program happens to link/dispatch, flag it in your report (it means Task 3's surface is smaller than mapped).
- [ ] **Step 6: Regression + commit.** Gates as always. `git commit --no-gpg-sign -m "feat(codegen): monomorphize combined generic+comptime instances"`.

---

### Task 3: Call-site + go/defer dispatch, end-to-end

**Files:** `src/codegen/call_codegen.c`, `src/codegen/statement_codegen.c`. Tests: `/tmp/t3_*.goo`.

- [ ] **Step 1: Failing test.** Task 2's t2.goo must now RUN and print correct sums per instance. Expect TODAY: undefined-identifier-class failure or wrong dispatch (the `!func_val` else-branch, call_codegen.c:1494, drops the comptime axis).
- [ ] **Step 2: Three-way rewrite in call_codegen** (~1451-1505): both axes present → combined mangler lookup (substitute type args under `active_subst` first, exactly as the generic block does at 1460-1466); generic-only and comptime-only branches unchanged. Build `func_val` with the substituted concrete signature (mirror 1468-1472).
- [ ] **Step 3: go-stmt rewiring** (statement_codegen.c:1719-1744): add the generic-only and combined cases beside the existing comptime one (same three-way). This BUILDS the missing generic go-stmt rewiring (pre-existing gap, map §1 closing note) — pin `go GenericFn(...)` (generic-only) with its own /tmp test since no baseline exists.
- [ ] **Step 4: End-to-end verification.** (a) t2.goo runs, correct sums, IR still shows distinct instances + dedup; (b) `go kernel(4, ch-collected)` correct; (c) `defer kernel(2, ...)` correct; (d) nested composed→composed with literal args (map: recursion + first-visit contract under codegen re-invocation — watch `type_check_generic_call`'s re-entry, call_codegen.c:113/1415/1798); (e) generic-only `go Id(...)` now works (new); (f) comptime-only and generic-only non-go controls unchanged.
- [ ] **Step 5: Value-escape spot-check.** A composed function used as a value (`f := kernel`) must reject via the existing `has_comptime_params` walls — verify one shape; if it does NOT reject, stop and report (interface between the axes' walls broke).
- [ ] **Step 6: Regression + commit.** Gates. `git commit --no-gpg-sign -m "feat(codegen): dispatch composed instances at call/go/defer sites"`.

---

### Task 4: Probes wired into verify

**Files:** `examples/comptime_generic_compose_probe.goo` (+ `.expected.txt`), `Makefile`. NO src/include changes — a failing probe due to a compiler bug is BLOCKED, not a license to fix compiler code.

- [ ] **Step 1: Golden probe** per the spec's Testing section: `kernel[T any](comptime n int, seed T) T` with `[n]T` buffer; instances (int64,4)×2 call sites, (int64,2), (float64,4) if float inference is clean (else a second int-width type; report what you used); a `go` dispatch collected via channel; a nested composed→composed call. Expected outputs computed by hand in the `.expected.txt`.
- [ ] **Step 2: Run via harness**: `make test-golden` → 314/0 (313 + this probe).
- [ ] **Step 3: Matrix additions** (model on existing cases, same diagnostic-grep + verifier-noise-grep discipline): `composed-tparam-comptime-type` (`comptime n T` declaration reject); `composed-runtime-comptime-arg` (runtime value to comptime position of generic call); `composed-fn-as-value` (assignment of composed function rejects). Existing 16 must stay green → 19/19. Wire into `.PHONY` + `verify` next to the existing matrix.
- [ ] **Step 4: IR pin.** Extend the golden probe's expected side or a small Makefile check greping `--emit-llvm` output for the three distinct combined symbols + the dedup count (follow how sub-project 1's probe pinned IR, if it did — else a matrix-style target).
- [ ] **Step 5: Full regression + commit.** Golden 314/0, matrix 19/19, unit, tripwire. `git commit --no-gpg-sign -m "test(comptime): composed generic+comptime golden + matrix walls wired into verify"`.

---

## Self-review

- Spec coverage: surface/semantics → Tasks 1–3; mechanism decisions 1–2 → Task 1, 3–5 → Task 2, 6 → Task 3, 7 → Task 2 Step 4; walls table → Task 1 (lift + new) and Task 4 (pins); testing section → Task 4; open verification points 1–4 → Task 4 Step 1 (explicit syntax), Task 1 Step 2 (recorder/frees), Task 2 Step 4 + Task 3 (skip-guards/dispatch), Task 1 Step 4 (LIFO).
- The `!func_val` hazard (map §5, sharpest wall) is Task 3 Step 2's explicit target with Step 1 as its failing test.
- Frozen-axes guarantee: every task carries generic-only and comptime-only controls plus the full golden/matrix gates.
- No fabricated internals: every anchor is from the 2026-07-08 machinery map at a9cf5a6; where the map flags uncertainty (explicit `[T]` call syntax, float inference), tasks say verify-and-report rather than assume.
