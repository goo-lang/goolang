# Pointer-Concrete Interface Boxing (the #109 pair) — Design

Date: 2026-07-04 · Branch: `fix/ptr-iface-boxing` (base: main @ bf63344, post-#112)
Status: user-approved design (approach "C + A hardening", full-pair scope confirmed)

## Problem

Found in #109's final review, queue #1 since. Two defects, one root:

1. `var i Iface = &b` — `codegen_interface_box` stores the POINTER value into the
   heap box, but `build_thunk` assumes the data slot points at the POINTEE: value
   receivers load the struct through it, pointer receivers pass it raw
   (interface_codegen.c:111-148). On current main this fails at compile time
   (LLVM verifier: thunk passes `ptr` where the method wants the struct) — safer
   than the #109-era garbage, but `var i Iface = &b` is routine Go.
2. Thunk/vtable names key on `type_receiver_name(concrete)`, which maps `Box` and
   `*Box` to the same `"Box"` — whichever boxing shape emitted first would be
   silently reused for the other (the mechanism behind the original garbage
   symptom).

The shield: `type_interface_satisfied`'s embed fallback (type_checker.c:815) only
tries promoted methods for `TYPE_STRUCT` concretes, so `*Outer` fails with a clean
"missing method" instead of reaching the miscompile. Direct methods on `*T`
already pass typecheck (receiver-name resolution), which is why the repro reaches
codegen.

## Approach (user-selected): C representation + A hardening

Considered: (A) heap-box the pointer + deref-aware, separately-named thunk sets —
works but doubles thunk sets and spends an allocation to store a pointer;
(B) deref at box time (copy the pointee) — REJECTED, Go's `&b` boxing aliases,
never copies; (C) Go's own representation — data slot IS the pointer. C chosen,
plus A's defensive guard in the thunk builder.

### 1. Representation (C) — `codegen_interface_box` (interface_codegen.c:206)

When `concrete->kind == TYPE_POINTER` and the pointee is a nameable method-bearing
type: normalize to the pointee for vtable construction (`codegen_interface_vtable`
called with the POINTEE type), skip `goo_alloc`+store, and insert the pointer
value directly as the data word. The value-boxed path is byte-for-byte unchanged.

Why this is correct with ZERO thunk changes: in both shapes `data` points at the
concrete struct — value-boxed: at the heap copy; pointer-boxed: at the caller's
object. The existing receiver logic (value receiver loads through `data`, pointer
receiver passes `data`) is already right for both. Vtable "collision" between
`Box` and `*Box` becomes intentional reuse.

Consequences (all Go-exact):
- Aliasing: mutations of `b` visible through `i` and vice versa; a
  pointer-receiver method called through `i` mutates `b` itself (not a box copy).
- One less allocation per pointer boxing (and one less leak, pre-GC).
- Method set: both receiver kinds reachable from `*T` — matches Go; no new
  deviation (Goo's existing leniency for value-boxed `T` reaching pointer-receiver
  methods stays as the documented #109 deviation, with box-copy semantics).

All boxing sinks funnel through this one helper — var-decl init
(function_codegen.c:1363), assignment (expression_codegen.c:1063), multi-assign
(statement_codegen.c:215), call args (call_codegen.c P4-5 arm), map values (#110
I1 fix sites) — so the normalization fixes every sink at once. Verify each with a
probe anyway (Testing).

### 2. Thunk hardening (the A piece) — `build_thunk` (interface_codegen.c:53)

After §1, `build_thunk` must never see a `TYPE_POINTER` concrete. Add an explicit
guard at entry: if `concrete->kind == TYPE_POINTER`, emit
`"internal: pointer concrete reached thunk builder un-normalized"` and return
NULL — belt-and-braces against future direct callers of
`codegen_interface_vtable`/`build_thunk` re-creating the verifier failure.
`codegen_interface_dispatch` is rep-agnostic — untouched.

### 3. Gate lift (SECOND, ordered) — promoted satisfaction through `*Outer`

Normalize `*S → S` (struct pointees only) in:
- `type_interface_satisfied`'s embed fallback (type_checker.c:815), and
- `build_thunk`'s promoted-method resolution mirror (interface_codegen.c:76)
  — after §1's box-side normalization the concrete arriving there is already the
  pointee, so this mirror is expected to need no change; verify rather than edit
  blindly, and rely on §2's guard.

ORDERING RULE (from the queue, enforced as plan task order): the gate lifts only
after §1 is implemented AND golden-covered. Lifting first converts today's clean
error into wrong output.

## Recorded, not fixed

- Nil `*Box` inside an interface: pointer-receiver methods receive nil (Go-exact —
  Go runs them); value-receiver methods crash on the load where Go panics with a
  clean nil-deref message. Consistent with Goo's raw pointer-deref behavior
  compiler-wide (no nil checks on deref). Probe documents it; NOT golden'd.
- `var i Iface = nil` stays rejected ("nil does not implement I") — existing
  recorded item, same family as funcval == nil.
- Dangling hazard: the data word can hold `&local` and the interface can outlive
  the frame — identical exposure to every `&local` use pre-GC/escape-analysis;
  not new to this change (the OLD code had the same hazard one indirection over).
- Interface→interface assign stays "v1: permissive" (type_checker.c:859) — scope
  explicitly excluded by user.

## Error handling

No new user-facing diagnostics. §2's internal guard is compiler-internal. The
`*Outer` "missing method" rejection DISAPPEARS (that is the point of the pair);
non-satisfying pointer types still get the existing "does not implement" message
with the pointee's method report.

## Testing (all goldens `go run`-differential; zero parser changes — run
`./scripts/grammar-tripwire.sh` once anyway as a no-op sanity per the goo-grammar
skill)

- `ptr_iface_probe` golden: `var i Iface = &b`; mutation of `b` visible through
  `i`; ptr-receiver method through `i` mutates `b`; value-receiver through `i`
  reads current state; heterogeneous `[]Iface{b-value, &b2}` mixing both boxing
  shapes of the SAME type (pins vtable reuse); interface passed across a function
  boundary retains aliasing.
- `embed_ptr_satisfaction_probe` golden (after gate lift): `*Outer` satisfies via
  a promoted method — both a value-embedded and a pointer-embedded inner, depth 2;
  dispatch through the promoted thunk go-identical.
- Regression golden: value-boxed copy semantics UNCHANGED — `var i Iface = b;
  b.N = 99` NOT visible through `i` (pins that C didn't leak aliasing into the
  value path).
- Sink probes (controller, need not all be goldens): &b through call-arg boxing,
  map[string]Iface value, and multi-assign.
- Nil-pointer probe (documented behavior, not golden'd).
- Gates: golden count grows from 221/0, `make test` 76/1, `make verify` +
  `ccomp-link` green, bison 81/256 untouched.

## Tasks (risk-ascending)

T1 §1 representation + §2 guard + `ptr_iface_probe` + regression golden →
T2 §3 gate lift + `embed_ptr_satisfaction_probe` → T3 sink probes + sweep +
handoff. SDD economy mode; fresh-context whole-branch review before merge.
