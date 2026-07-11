# Correctness Burn-Down Implementation Plan (arc 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Retire the correctness debt that reproduces at `dd11713`: the sibling return-type poisoning (new find), the tagged-switch-on-rune verifier leak, and the `fmt.Println(any)` aggregate fallback — plus commit the fixtures that pin two proven-but-unpinned comptime shapes.

**Ground truth:** `.superpowers/sdd/arc2-repro-report.md` (reproduction scout @ dd11713) is normative for what exists. Three legacy suspects (multi-return interface coercion, mixed-width print, literal-nil→iface) did NOT reproduce and are retired in memory — do not chase them.

## Global Constraints

- C23. No naked returns. Struct-tail fields. Diagnostics = positioned `type_error`; **no `Module verification failed`/LLVM text may reach users** (B3's current failure mode is exactly this).
- **NO parser.y/lexer changes.** The two grammar gaps the scout found (3-value `:=` multi-assign; qualified composite literals `pkg.Type{F: v}`) are OUT OF SCOPE — recorded as conformance follow-ups, not licenses. Tripwire stays **31 S/R + 0 R/R** exact, every task.
- Gates before every commit: `make test` (76/1-skip), `make test-golden` (422/0 at start, grows), `make test-golden-o2`, `make test-golden-reject` (81/0), `make comptime-value-reject-matrix` (18/18 at start; Task 4 may grow it), both IR pins PASS, tripwire exact.
- FROZEN: all existing goldens byte-identical except where a task's fix legitimately changes output (B5 changes `Println(any)` aggregate output — audit which existing fixtures print boxed aggregates before assuming none).
- Commits: conventional, `git -c commit.gpgsign=false commit`.
- Shared-checkout caution: a concurrent safety-scan session may touch this tree. Verify branch `fix/correctness-burndown` at task start and pre-commit; foreign untracked files (safety-scan set) and `.handoff.md` are not yours.

## File structure (expected; Task 1's diagnosis may adjust)

- `src/types/` (checker state leak — Task 1; switch-case literal typing — Task 2)
- `src/codegen/` (switch lowering if the fix is codegen-side — Task 2; `interface_codegen.c` fmt emission — Task 3)
- `examples/` +4-5 golden pairs; `Makefile` matrix additions — Task 4
- `goostd/cpkg/cpkg.go` may gain a function for the R-fixtures — Task 4

---

### Task 1: Sibling return-type poisoning (new find — diagnose first)

**Symptom (scout report §B2-incidental):** an `int8`-returning function poisons return-type checking for a LATER `int64`-returning sibling in the same file. Root cause unknown — suspected checker state leak (something width-related cached across function decls and not reset).

- [ ] **Step 1: Minimal RED.** Reduce the scout's shape to the smallest two-function program that misbehaves; record exact diagnostic/misbehavior. Establish the trigger matrix: which width pairs, does ordering matter, does an interposed function clear it, function-scope vs package-scope.
- [ ] **Step 2: Root-cause.** Instrument/trace the checker (TcFunctionContext reset paths, any width/type caches, `declare_function_signature` vs `type_check_function_decl` state). Name the leaked state (file:line) BEFORE writing the fix. If the leak is in codegen instead, say so and re-aim.
- [ ] **Step 3: Fix at the root** (reset/scope the leaked state; no symptom patches). **Step 4:** golden `examples/sibling_return_width_probe.goo` + expected pinning the trigger matrix's worst shape. **Step 5:** full gates; commit `fix(types): <named state> leaked across sibling function decls — reset per function`.

### Task 2: Tagged switch on rune/int32 with char-literal cases (B3)

**Symptom:** `switch r { case '\a': }` with `r` rune/int32 → `Module verification failed ... icmp eq i32 %r1, i64 10` leaked to user, exit 1. Char literals are typed i64 while the tag is i32; legal Go shape must COMPILE and run.

- [ ] **Step 1: RED** with the scout's exact reproducer + a behavioral variant (multiple cases incl. default, fallthrough-free semantics asserted).
- [ ] **Step 2: Fix where case literals meet the tag type** — unify case-expression width with the tag's type in the checker (preferred: the same untyped-constant convertibility rules the binary-expr path uses) or coerce at lowering; wrong-typed NON-constant case exprs must still reject cleanly (`case "x":` on an int tag → positioned type error, add to the reject fixtures if not already covered).
- [ ] **Step 3:** golden `examples/switch_rune_char_probe.goo` (tagged switch over rune with char-literal cases incl. `'\a'`, `'\n'`, ASCII letters; deterministic output) + reject pair if Step 2 added a wall. **Step 4:** update `goostd/strconv/strconv.go`'s workaround comment (do NOT rewrite its tagless switches — comment-only: the bug is fixed, workaround kept until a dedicated vendoring pass). **Step 5:** full gates; commit `fix(types): tagged switch unifies char-literal case width with rune/int32 tag`.

### Task 3: fmt.Println(any) aggregate formatting (B5)

**Symptom:** boxed aggregates print bare type name (`Point`) instead of Go-style fields (`{1 2}`); scalars fine. Fallback lives at `src/codegen/interface_codegen.c` `codegen_get_or_emit_type_fmt` (~250-266) — deliberately scoped in v1.

- [ ] **Step 1: RED** — `var x any = Point{1,2}; fmt.Println(x)` prints `Point` today; expected `{1 2}`. Also nested struct, struct w/ string field, slice-of-struct boxed (decide+document which shapes are IN scope; match concrete-struct Println's existing formatting exactly — byte-parity with the unboxed print of the same value is the acceptance bar).
- [ ] **Step 2: Implement** by routing the boxed path through the SAME per-type fmt emission the concrete path uses (the type descriptor/RTTI slot from #132/#135 identifies the boxed type). No new formatting logic — reuse; if a shape can't reuse (e.g. boxed slice), keep the current fallback FOR THAT SHAPE and document.
- [ ] **Step 3: Audit existing goldens** for boxed-aggregate prints whose expected output changes (grep expected.txt for bare type-name lines near any-prints); update only those with justification in the commit. **Step 4:** golden `examples/println_any_struct_probe.goo`. **Step 5:** full gates; commit `feat(codegen): fmt.Println(any) prints aggregate fields via the concrete type's fmt path`.

### Task 4: Riders — pin the proven shapes

- [ ] **Step 1:** `examples/pkg_global_comptime_probe.goo` + expected (comptime pkg-fn reading a package `var`; scout R1 shape, prints 115) — may extend `goostd/cpkg`.
- [ ] **Step 2:** `examples/pkg_comptime_structparam_probe.goo` + expected (R2 shape: `func Read(comptime n int, b Box) int`; avoid the qualified-composite-literal gap — construct the Box via a helper function, and note the gap in the fixture comment).
- [ ] **Step 3:** matrix case `package-generic-comptime`: `cpkg.GenFill[T any]`-shaped decl called cross-package must reject with its actual (noisy-but-clean) diagnostic — capture the real message first, then pin it; matrix count 18→19, echo line updated.
- [ ] **Step 4:** fix the `goostd/cpkg` header-comment wording drift (T2 parked Minor). **Step 5:** full gates; commit `test(comptime): pin pkg-global + struct-param comptime shapes; generic+comptime cross-package reject matrix case`.

---

## Self-review
- Every live finding from the repro report has a task (N1→T1, B3→T2, B5→T3); retired suspects have memory updates (done pre-plan); grammar gaps explicitly out-of-scope with follow-up note; riders→T4.
- Fix tasks all demand root-cause naming before fixing (T1 Step 2, T2 Step 2 unification-not-patch, T3 reuse-not-reimplement).
- Final review: fresh-context whole-branch (fable) — checker + switch lowering + fmt codegen are miscompile-adjacent; probe degenerate widths (int8 tags, negative runes, empty switch) deliberately.
