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
