# 1. Nil semantics: Go parity, with emitted nil-deref checks

Date: 2026-07-23
Status: accepted

## Context

The roadmap's P2.2 called nil semantics "the single largest Go-compat fork,
currently undocumented either way" (docs/2026-07-08-v1-roadmap.md, Open
decisions). A 24-operation empirical probe matrix (2026-07-23) showed the
fork is mostly already decided by implementation: 17/24 nil operations ship
Go-parity behavior (nil assignment/comparison for *T/[]T/map/chan/func,
nil-map read/write/delete/range, nil-slice len/append, nil-channel
block/close-panic). What remains: 4 raw SIGSEGVs (pointer deref read/write,
nil-receiver method touching fields, nil user-interface dispatch), 1 silent
wrong value (`error(nil).Error()` returns "" via a deliberate codegen
guard), and 2 message-wording divergences.

## Decision

Adopt **Go-parity nil semantics** as the documented contract. Close the
remaining gaps by **emitting inline nil checks** at the four unguarded
deref/dispatch sites — `icmp eq null` → cold branch calling a new
`noreturn` `goo_nil_deref_fail` printing Go's canonical message ("invalid
memory address or nil pointer dereference"), exit 2 — the same
compare-and-cold-fail pattern arc 17 shipped for bounds checks (LLVM
eliminates checks on provably-non-nil paths). Remove the
`error(nil).Error()` empty-string guard so it panics like Go. Non-nullable
pointers remain expressible via `?T`/`?*T` as an opt-in differentiator, not
the default.

## Alternatives Considered

| Option | Pros | Cons |
|--------|------|------|
| SIGSEGV handler → panic (Go's own mechanism) | Zero per-deref cost | Async-signal-safety under the M:N ucontext scheduler; can't distinguish nil deref from other faults; riskiest machinery for the same user-visible result |
| Non-nullable pointers (?*T only) | Genuine differentiator | Foreclosed in practice: 17/24 Go-parity cells shipped (incl. nil-map write, decided 2026-07-10); breaks Go paste-in |
| Document-only (deref = crash) | Cheapest | Leaves a silent-wrong-value bug and undiagnosable exit-139 crashes |

## Consequences

### Positive
- Every nil failure becomes a diagnosable Go-style panic; the last
  silent-wrong-value nil behavior is removed.
- Spec can document the full 24-cell matrix as the contract.

### Negative
- Nil checks on unproven derefs cost a compare+branch (arc 17 evidence:
  LLVM removes most, and cold blocks don't pollute hot paths).

### Risks
- Perf regression in pointer-heavy loops: mitigate by reusing the arc-17
  inline pattern (proven optimizable) and measuring with the existing
  bench targets before merge.
- `error(nil)` guard removal may surface latent nil-error bugs in fixtures:
  those are real bugs; fix the fixtures, not the semantics.

## References
- Probe matrix + raw log: session scratchpad `nilprobe/matrix.md` (2026-07-23);
  summary in `.handoff.md`'s next-arc entry.
- Arc 17 (inline bounds checks, PR #211) — the emission pattern to reuse.
- docs/2026-07-08-v1-roadmap.md P2.2 + Open decisions; P3.9 nil-map-write
  decision (2026-07-10).
