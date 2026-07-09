# Phase 2 sub-project A — method-set enforcement (P2.1) + Go-compatible nil (P2.2)

**Date:** 2026-07-09
**Branch:** `feat/p2-typesystem-a` (off main @ 7b53ab9, post PR #167)
**User decision:** P2.2 = **Option A, Go-compatible nil** (2026-07-09); non-nullable-pointer design repositioned as possible post-v1 strict mode. `?T` remains the opt-in safety layer.
**Premises:** verified by execution/trace on this branch 2026-07-09 (recon report) — P2.1 and P2.2 premises HOLD; the literal-nil→iface bug in older notes is FIXED (21d8370, merged).

## T1 — P2.1: enforce Go method-set rules in interface satisfaction — **STATUS: DONE (8d012cd)**

**Current state (verified):** `type_interface_satisfied` (type_checker.c:1378-1459) matches by name+signature only — no receiver-kind gate. `var s Stringerish = Node{id:7}` with a pointer-receiver `Label()` compiles and prints 107 (`examples/interface_ptr_recv_probe.goo` pins this wrong-per-Go behavior); pointer-receiver methods dispatched on a boxed value mutate a disconnected heap copy (interface_codegen.c:496-511).

**Reference implementation exists:** unmerged branch `fix/interface-receiver-kind-soundness`, commit `62d0d8c` — receiver-kind gate (`params[0]->kind == TYPE_POINTER`), `EmbedResult.via_pointer` composition for embedded satisfaction, AND the collector fix. It diverged pre-#166/#167 (TcFunctionContext restructuring), so: attempt cherry-pick; on conflict, reapply its diff manually against current code. Do not reinvent the design.

**Coupling hazard (memory-confirmed, anchor-verified):** `codegen_collect_iface_implementers` calls `type_interface_satisfied` with the VALUE type at interface_codegen.c:683. When the gate lands, that call MUST switch to the pointer form (`*T`, the method-set superset) or `x.(I)` on pointer-boxed values breaks. The reference commit does this — keep it.

**Fixture semantics change (deliberate, documented):** `interface_ptr_recv_probe.goo` switches to `&Node{id:7}` (go-verified; the old form becomes a reject fixture `iface_value_ptr_recv_reject` with a diagnostic naming the method and receiver kind, mirroring Go's message shape: "Node does not implement Stringerish (method Label has pointer receiver)").

**Acceptance (roadmap P2.1):** value-satisfies-pointer-receiver rejected with method+kind named; `&C{}` and addressable-auto-& still pass; the full interface/embedding/type-assert regression list from the recon report green; RTTI pointer-boxed `x.(I)` probes green.

## T2 — P2.2: Go-compatible nil for bare `*T` / `[]T` / `map[K]V` / `chan T` / `func` — **STATUS: DONE (199f94b)**

**Current state (verified per-kind):** assignment and comparison rejected for all five kinds EXCEPT func comparison + zero-value (already Go-correct: expression_helpers.c:339-343, funcval_nilcmp/funcnil_abort fixtures). Zero values already lower to `LLVMConstNull` unconditionally (function_codegen.c:2100-2101). Nil literal → `TYPE_UNKNOWN`/"nil" (expression_checker.c:~1134); only NULLABLE arm in `type_compatible` (types.c:785-818, fallthrough return 0 at ~814).

**Type checker:**
- `type_compatible`: nil (`TYPE_UNKNOWN` named "nil") accepted when `to->kind ∈ {TYPE_POINTER, TYPE_SLICE, TYPE_MAP, TYPE_CHANNEL, TYPE_FUNCTION}` (function assignment is the missing fifth; comparison already works).
- `type_check_comparison_op` (expression_helpers.c:321-364): add POINTER/SLICE/MAP/CHANNEL arms mirroring the existing TYPE_FUNCTION arm (== and != only; ordered comparisons stay rejected).
- Do NOT wire the dead `check_null_safety_assignment` (ownership_checker.c:88 — zero callers, wrong message now); it stays dead until P5.6 unlinks it.

**Codegen (representations from type_mapping.c, recon-anchored):**
- nil materialization with expected type: pointer/map/chan → `LLVMConstNull(ptr)`; slice → `{null, 0, 0}` (field order ptr/len/cap is load-bearing); func → `{null, null}` fat pair. Extend `codegen_generate_null_literal` (nullable_codegen.c:301-316) to take the expected kind rather than defaulting to `*void`; thread expected type at the assignment/var-init/call-arg/return intercept sites the recon enumerated (composite_codegen.c:558,1037; call_codegen.c:1618; statement_codegen.c:1608,1673; function_codegen.c:1362,2178,2363 — audit each, extend where the kind can now be one of the five).
- Comparison lowering: pointer/map/chan → `icmp eq ptr`; slice → extract ptr field, icmp against null (Go: `s == nil` iff backing ptr nil); func → existing pair-fn-slot compare untouched.

**Deliberately out of scope (documented):** nil-map WRITE semantics (P3.9 open decision — current lazy-init behavior unchanged); send/recv on nil channel (Go blocks forever; v1 behavior documented as-is, no probe that hangs); nil func CALL already panics correctly (keep fixture green).

**Acceptance (roadmap P2.2 option a):** nil1 (assign/compare all five kinds), nil2 (linked-list walk: `for n != nil { ...; n = n.next }` — the headline unlock), nil3 (nil slice: len 0, append grows, == nil flips after append — go-verified) probes compile and run; map read from nil map yields zero value (Go-parity read side); all 44 existing nil-referencing fixtures green; `?T` family untouched (tag-nil vs bare-nil never conflated — `?*T = nil` still sets the tag, not the pointer).

## T3 — probes, characterization, truth pass — **STATUS: DONE**

- New goldens (go-verified): `nil_ptr_walk_probe` (linked list), `nil_kinds_probe` (assign+compare matrix over the five kinds), `nil_slice_probe`, `nil_map_read_probe`, `func_nil_assign_probe`. New reject: ordered nil comparison (`p < nil`), nil to value type (`var x int = nil`).
- Characterization probe for the recon's open gap: mixed literal-nil + typed-nil map keys (`var k any; m[k]=1; m[nil]=2; len(m)` — go-verified expected; if it exposes the suspected mismatch, file it as a finding in the report, fix only if small).
- Truth pass: update `docs/02-LANGUAGE-SPECIFICATION.md` nil section if one exists (check); roadmap working notes; this spec's divergence list.

### T3 completion notes (2026-07-09)

**Items 1+2 (goldens + characterization probe) were already delivered inside
T2's commit `199f94b`**, not a separate T3 commit: that commit's own message
documents "RED-first: 6 new go-verified golden fixtures (nil_kinds_probe,
nil_ptr_walk_probe, nil_slice_probe, nil_map_read_probe,
func_nil_assign_probe, mixed_nil_key_probe) and 2 reject fixtures
(nil_ordered_cmp_reject, nil_to_value_reject)" — i.e. the T2 agent pulled the
T3-scoped fixture work forward into its own TDD cycle rather than leaving it
for a follow-up task. `mixed_nil_key_probe.goo`'s header is even
self-labeled "T3 characterization" and states the finding: the recon's
suspected literal-nil/typed-nil map-key mismatch does **NOT** reproduce on
this branch (`m[k]` and `m[nil]` resolve to the same key, matching Go).
There is nothing left for T3 to add on this front; re-adding the same
fixtures would violate the "only add what T1/T2 didn't already" instruction.

This task independently re-verified all of it rather than trusting the
commit message:

- All 6 `.goo` fixtures (`nil_kinds_probe`, `nil_ptr_walk_probe`,
  `nil_slice_probe`, `nil_map_read_probe`, `func_nil_assign_probe`,
  `mixed_nil_key_probe`) compile and run as **plain Go** via `go run`
  (go1.26.1) and match their committed `.expected.txt` byte-for-byte —
  confirms the "go-verified" provenance claims are real, not asserted.
- Both reject fixtures (`nil_ordered_cmp_reject`, `nil_to_value_reject`)
  carry specific, non-generic `.err.txt` messages ("Cannot compare
  incompatible types *int64 and nil", "Cannot assign nil to int64") and
  PASS under `run_golden_reject.sh`.
- Gates rerun clean: `grammar-tripwire.sh` 121 S/R + 256 R/R exact ·
  `run_golden.sh` 357/0 · `run_golden_reject.sh` 26/0 · `make test` 76/77 (1
  pre-existing skip) · `make funcnil-abort-probe funcval-nilcmp-probe`
  (the HARD INVARIANT probes outside `run_golden.sh`'s discovery) both PASS.
- 50 `examples/*.goo` fixtures now reference `nil` (44 pre-T2 baseline + 6
  new), consistent with T2's "44 existing nil-referencing fixtures green"
  acceptance line.

**Item 3 (truth pass) results:**

- `docs/02-LANGUAGE-SPECIFICATION.md` has **no nil section** — grep for
  `nil|null` (case-insensitive) across the whole 743-line doc returns zero
  hits; the only nullable-type mention at all is a single `?T` return-type
  example inside the Interfaces EBNF grammar snippet (line 117), predating
  this feature. Per this task's instruction ("update if exists, note
  absence if not"), no section was added — the doc is an older/aspirational
  v1.0 spec that doesn't yet cover `?T` itself, let alone bare-nil. Flagging
  this as a real documentation gap (not just for nil, but for `?T` broadly)
  for a future docs-focused task rather than papering over it here.
- Out-of-scope list confirmed unshipped: `git show 199f94b` touching
  `src/codegen/` and `src/types/` was grepped for map-write, channel
  send/recv, and func-call-nil edits — zero hits. The three items T2
  documented as deliberately out of scope (nil-map WRITE semantics
  unchanged, nil-channel send/recv unchanged, nil-func CALL panic path
  unchanged) are exactly as documented; nothing crept in.
- Roadmap doc `docs/2026-07-08-v1-roadmap.md` (P2.1/P2.2 rows, lines
  51-52/86/103/107-108/199) is a dated snapshot that still lists P2.1/P2.2
  as open — left untouched here (out of this task's file scope) but the
  next roadmap refresh should mark both DONE against `8d012cd`/`199f94b`.

## Review regime

Behavior-changing feature work: ONE Fable dimension (nil-semantics miscompile hunting: nil in struct fields, nil through interface boxing, nil across function boundaries, ?*T vs *T conflation shapes, method-set edge shapes incl. embedding-via-pointer) + Opus repro/refute on critical/major findings only; Sonnet mechanical dimension for fixture quality. No 5-dim panel.

## Review outcome (F4 truth pass, 2026-07-09)

Fresh-context whole-branch review (`git diff main...HEAD`: 8d012cd, 199f94b,
6ea4e2b, plus this spec) ran the review regime above: one Fable dimension
(semantics-fable) + one Sonnet dimension (fixtures-sonnet), Opus repro+refute
on every critical/major finding, right-sized per branch risk. Result:
`confirmed=3 plausible=2 rejected=0`. All three CONFIRMED findings were
fixed; both PLAUSIBLE findings were adversarially refuted as pre-existing
(unchanged by this branch) — one was fixed anyway as a proactive Go-compat
improvement, the other is recorded below as a known, deliberate divergence.

### Findings fixed

**F1 — Empty slice literal `[]T{}` compiled to a nil-equivalent (critical,
CONFIRMED both by direct repro and adversarial refute-attempt).** Both the
local-scope zero-size allocation path (`goo_alloc(0)` → `NULL`,
`runtime.c`) and the global-scope constant path (`LLVMConstNull(...)` for
`count==0`, `composite_codegen.c`) gave an empty slice literal a NULL
backing pointer. Since T2 made `s == nil` a live, user-visible comparison
(extract field 0, compare to null), `[]int{}` at local *or* package scope
silently compared `== nil`, contradicting Go (`[]int{}` is empty-but-non-nil)
and the comparison lowering's own comment. Confirmed NOT pre-existing: on
`main` the identical source is a hard type-check reject (`slice == nil` was
rejected before T2 made it legal), so this was latent dead code that T2's
own feature made live and wrong. **Fixed by `12c7461`** — a shared
`goo_zerobase` sentinel address stands in for any zero-size allocation
(local *and* arena paths), so `[]T{}` is non-nil while `var s []T` (true
zero value, never calls `goo_alloc`) stays nil. Golden: 357→358
(`empty_slice_nil_probe`, go-run-verified).

**F2 — `nil_ordered_cmp_reject` pinned only the direction commit 199f94b's
own message called "already safe"; the actual new-risk reversed operand
(`nil < p`) had zero regression coverage (minor, CONFIRMED — coverage gap,
not a live bug; manually confirmed `nil < p` was already correctly
rejected).** **Fixed by `9346893`**, which adds
`nil_ordered_cmp_reversed_reject` pinning `nil < p` for the case the ordered-
comparison guard (`expression_helpers.c`) actually protects. The same
commit also proactively closes the *related* PLAUSIBLE finding below (two
non-nil reference values failing to compare) even though that finding was
independently refuted as pre-existing and therefore non-gating — fixing it
was in scope for the same diagnostic-quality pass and shares the same
type-checker/codegen touch points. It adds: reference-identity `==`/`!=`
for non-nil pointer and channel pairs (Go allows address/identity
comparison, previously an accidental codegen crash — "Failed to generate
binary operation" — because it slipped past the type checker's generic
fallback with no codegen lowering); and explicit, positioned rejects for
same-kind non-nil slice/map/func comparisons (`"<kind> can only be compared
to nil"`, matching Go's diagnostic shape), where they previously died with
the same generic codegen crash instead of a real diagnostic. Does **not**
touch struct/value equality or `any == any` (interface==interface) —
verified pre-existing and explicitly out of this fix's scope; still fails
at codegen the same way it did before this branch and before this fix.
Golden: 358→359 (`ref_identity_probe`); reject: 26→30
(`func_eq_reject`, `map_eq_reject`, `slice_eq_reject`,
`nil_ordered_cmp_reversed_reject`).

**F3 — `nil` rejected for non-empty interface targets in var-init,
assignment, and call-arg position, but accepted in return position — Go
accepts all four (minor, CONFIRMED).** Interface is Go's sixth nilable
kind and was deliberately outside T2's five-kind scope
(`type_is_nilable_ref_kind`: pointer/slice/map/chan/func only — see T2
above); at the three non-return intercept sites the bare nil literal was
instead routed into `type_interface_satisfied` as if it were a concrete
type, producing `"nil does not implement I (missing method M)"`, while
`return nil` worked because the return path had its own separate
`TYPE_UNKNOWN` short-circuit. Confirmed pre-existing on `main` too (same
reject), but newly in-scope as the natural extension once T2 shipped
Go-compatible nil for the other five kinds. **Fixed by `1591a03`** — the
same short-circuit the return path already had (checking for the
`TYPE_UNKNOWN`/nil-literal marker before the does-not-implement check) was
added at `check_interface_assign` (the choke point for var-init,
assignment, struct/map-literal fields, slice elements, index-assign,
type-switch case checking) and the call-argument interface check in
`expression_checker.c`. No codegen change needed — `codegen_interface_box`
already special-cased a `TYPE_UNKNOWN` concrete correctly; the gap was
purely type-check routing. Golden: 359→360 (`iface_nil_probe`,
go-run-verified: `var i I = nil`, box-then-nil round trip, `take(nil)`
call-arg).

### Known deliberate divergence (not fixed — recorded, not silent)

**Value-boxed concrete passes `x.(I)` and `switch x := b.(type) { case I:
...}` at runtime when only `*C` (not `C`) implements `I` — PLAUSIBLE,
adversarially refuted as pre-existing, not gating this branch, but real and
worth naming.** Both the initial repro-verify and the independent refute
pass confirmed the *runtime* Go-divergence (`b.(I)` returns `ok=true` and
the type-switch takes `case I` for a value-boxed `C{v:8}` where `M()` has a
pointer receiver — Go returns `ok=false` / takes `default`) reproduces
byte-for-byte identically on `main`, i.e. this branch did not introduce it.
What this branch *did* introduce is an internal inconsistency: the STATIC
path (T1, `8d012cd`) now correctly enforces Go method-set rules
(`var x I = C{v:8}` is rejected — `"C does not implement I (pointer-receiver
method M)"`), while the DYNAMIC/RTTI path still accepts the identical
value at runtime. Before T1, both paths were permissive and therefore
mutually consistent (wrong, but consistent); after T1, the static path is
Go-correct and the dynamic path is not.

This is a **deliberate trade-off of the RTTI collector coupling documented
in T1 above** ("Coupling hazard... `codegen_collect_iface_implementers`
... MUST switch to the pointer form (`*T`, the method-set superset) or
`x.(I)` on pointer-boxed values breaks") and independently recorded in
project memory
(`goolang-recv-kind-collector-coupling.md`: "RTTI implementer collector
must stay PERMISSIVE (checks `*T`) or `x.(I)` on pointer-boxed values
breaks"). The collector's descriptor loop currently emits *both*
value-form and pointer-form RTTI descriptors for every candidate struct so
that legitimate pointer-boxed `x.(I)` keeps matching; the side effect is
that a value-boxed concrete whose *value* method set is missing a
pointer-receiver method still matches through the pointer-form descriptor.
Tightening the collector to be strict would break the pointer-boxed case
T1 explicitly protects — not a safe substitution.

**Named follow-up (not scheduled by this spec):** assertion-side
method-set enforcement — discriminate boxing kind in the runtime RTTI
match itself (emit the value-form descriptor for a candidate only when the
*value* form satisfies `I`, keep the pointer-form descriptor unconditionally
so `&C{}` stays green) rather than gating at the collector's candidacy
test. This is a distinct, larger change from T1/T2's scope (touches the
runtime descriptor emission and match dispatch, not just the type checker)
and is left for a future P2-series task rather than folded into this
docs-only pass.

### Still deferred, unchanged

- **nil-map WRITE semantics** — still **P3.9** (open decision), exactly as
  T2 documented it out of scope; this pass made no change here. Read-side
  behavior (comma-ok read from nil map yields zero value) is unaffected and
  remains Go-parity.
- **nil-channel send/recv** and **nil-func CALL** — unchanged, as
  documented in T2 ("Deliberately out of scope" above); not touched by F1–F3.
- **`any == any` (interface == interface)** — still fails at codegen with
  the generic "Failed to generate binary operation" (confirmed pre-existing,
  unchanged by F2's fix, which was scoped to pointer/chan/slice/map/func
  only). Not in this branch's five-kind + interface-nil scope; a candidate
  for a future comparison-completeness pass alongside the assertion-side
  method-set follow-up above.

### Fixture accounting (F1–F3)

All seven fixtures added by F1–F3 are present in the current golden/reject
counts and each carries an inlined Go-provenance comment header
(`go-run-verified` / explicit Go diagnostic text quoted in the reject
fixture's header), confirmed by direct inspection, not by trusting commit
messages:

| Fixture | Kind | Added by | Provenance |
|---|---|---|---|
| `examples/empty_slice_nil_probe.goo` | golden | F1 (`12c7461`) | go-run-verified byte-for-byte, header states the contrast (local/global/post-append literal vs. zero-value) |
| `examples/ref_identity_probe.goo` | golden | F2 (`9346893`) | go-run-verified byte-for-byte, header states pointer/channel identity semantics |
| `tests/golden/reject/func_eq_reject.goo` | reject | F2 (`9346893`) | header quotes Go's diagnostic ("func can only be compared to nil"); `.err.txt` matches |
| `tests/golden/reject/map_eq_reject.goo` | reject | F2 (`9346893`) | header quotes Go's diagnostic ("map can only be compared to nil"); `.err.txt` matches |
| `tests/golden/reject/slice_eq_reject.goo` | reject | F2 (`9346893`) | header quotes Go's diagnostic ("slice can only be compared to nil"); `.err.txt` matches |
| `tests/golden/reject/nil_ordered_cmp_reversed_reject.goo` | reject | F2 (`9346893`) | header quotes Go's diagnostic (untyped nil operand not defined for `<`); `.err.txt` matches |
| `examples/iface_nil_probe.goo` | golden | F3 (`1591a03`) | go-run-verified byte-for-byte, header cross-references the finding it closes |

Verified counts at HEAD (re-run, not trusted from commit messages):
`grammar-tripwire.sh` PASS (121 S/R + 256 R/R, baseline exact) ·
`run_golden.sh` **360/0** (357 pre-F1 baseline + 3: F1 `empty_slice_nil_probe`,
F2 `ref_identity_probe`, F3 `iface_nil_probe`) ·
`run_golden_reject.sh` **30/0** (26 pre-F2 baseline + 4, all from F2:
`func_eq_reject`, `map_eq_reject`, `slice_eq_reject`,
`nil_ordered_cmp_reversed_reject`) · `make test` **76/77** (1 pre-existing
skip, unrelated to this branch).

### `docs/02-LANGUAGE-SPECIFICATION.md` re-check

T3 found zero `nil`/`null` hits across the doc's 743 lines and concluded it
predates `?T` itself, let alone bare-nil, and left it untouched as an
out-of-file-scope documentation gap. Re-verified at this pass's HEAD
(post F1–F3): still zero `nil`/`null` hits, and `git log --follow` shows
the file's last real edit predates this branch entirely. Nothing changed
that would newly put it in scope, so — per the "update if exists, note
absence if not" instruction — it is intentionally left untouched here too.
The gap (no `nil` *or* `?T` coverage in the language spec) remains flagged
for a future docs-focused task, not silently dropped.
