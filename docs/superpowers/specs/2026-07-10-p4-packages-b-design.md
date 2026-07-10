# Phase 4 sub-project B — pkg.Type, cross-package methods, sync (P4.2, P4.3, P4.7)

Date: 2026-07-10. Branch: `feat/p4-packages-b`. Prereq: sub-A merged (#175). Grammar work:
goo-grammar skill procedure is MANDATORY (tripwire before/after, ledger for any delta).

## Recon facts (verified 2026-07-10; uncommitted sub-A in tree, none of it touches these)

- Type grammar hub: `type` (parser.y:2131) → `type_name: identifier` (:2527) mints
  BasicTypeNode{name} (single string). `var p shapes.Point` fails at the DOT (identifier
  reduces to type; DOT not in follow set).
- `pkg.Type(x)` vs `pkg.Fn(x)` both already parse as call_expr over selector_expr — the
  CHECKER distinguishes; no parse-time fork needed for conversions.
- STALE COMMENT HAZARD: parser.y:2014-2015 claims baseline "81 S/R + 256 R/R" — the script
  (grammar-tripwire.sh EXPECTED_SR/RR) is authoritative (currently 121/256). Fix the comment
  in sub-B's grammar commit.
- Package exports already include TYPES: package_export_filter copies every capitalized
  symbol SHARING the same Type* (type_checker.c:195-212, published at :778). Because the
  Type* is shared, codegen's struct-cache (keyed on Type*, type_mapping.c:214) interns
  cross-package uses to ONE LLVM struct — the struct-name-interning hazard is satisfied by
  construction; do not clone exported Type* or re-key the cache by name.
- P4.3 break: method lookup (expression_checker.c:4337-4342) does a CURRENT-scope lookup of
  bare `Point__Sum`; the method Variable lives only in the (torn-down) package scope +
  pkg->exports. Codegen method arm (call_codegen.c:1225) uses the bare mangled name; package
  functions already prefix via codegen.c:18 (`goo_pkg__<pkg>__<base>`).
- sync runtime symbols all exist (runtime.h:626-636). goostd source packages have NO extern
  mechanism — sync must be compiler-shimmed, and it exports TYPES with METHODS, which the
  (sub-A) shim table does not model.

## Design decisions (Fable, DRAFT — confirm at execution)

### B1 (P4.2) — grammar + checker for qualified type names

- Grammar: `type_name: identifier DOT identifier` new arm. Expected-clean LALR shape
  (identifier already a type-start; shift-DOT beats reduce in the same state), but the
  tripwire verdict is the only truth — any delta goes through the ledger's classify+probe
  procedure or the change reverts. SCOPE CUT: qualified COMPOSITE LITERALS
  (`shapes.Point{...}`) are NOT in B1 — they enter LBRACE_BODY territory (workarounds §1);
  construction happens via package constructor functions for now; rider B4 assesses the
  literal arm separately after B1's conflict picture is known.
- AST: append `char* package` to BasicTypeNode TAIL (header rules: make clean; audit BOTH
  mint sites — parser.y:2529 and the embedded-field helper parser.y:31 — plus any other
  malloc(sizeof(BasicTypeNode)) to zero it).
- Checker: type_from_ast's AST_BASIC_TYPE arm (type_checker.c:3882): when package is set,
  resolve the package marker variable (TYPE_PACKAGE), scope_lookup_variable(pkg->exports,
  name), take ->type; positioned errors for unknown package / unknown exported type /
  lowercase (unexported) names. AST_IDENTIFIER near-duplicate arm (:3820) audited for the
  same treatment where reachable.
- Probes: var/param/field/return positions with an exporting goostd test package; reject
  fixtures: unknown pkg.Type, unexported pkg.type; tripwire evidence in the commit.

### B2 (P4.3) — cross-package method resolution

- Checker: when the receiver's struct Type resolves via a package export, method lookup
  must consult THAT package's exports for the mangled name. The Type needs to know its
  owner: append an owning-package back-pointer field to Type's struct data (TAIL; or a
  parallel registry if Type layout is too hot — decide at execution after sizing the
  blast radius). Lookup order: current scope (today's behavior, intra-package) → owning
  package exports.
- Codegen: the method-call arm (call_codegen.c:1225) emits the package-prefixed symbol
  (`goo_pkg__<pkg>__Point__Sum`) when the receiver type is package-owned — reuse the
  call_codegen.c:85 helper. Method VALUES (P3.6 bound thunks) on package types: same
  prefixing in the bound-thunk path — add a probe.
- Probe: goostd test package exports Point with Sum method; main calls p.Sum() and binds
  f := p.Sum (method value); smoke both.

### B3 (P4.7) — sync as a method-aware shim

- Route: compiler shim (source packages cannot call runtime symbols). sync exports types
  WaitGroup/Mutex laid out as the runtime structs; `var wg sync.WaitGroup` allocas the
  struct; methods lower to goo_waitgroup_add/done/wait, goo_mutex_lock/unlock with &recv.
- **DESIGN RISK to resolve at execution**: Go's zero-value contract (`var mu sync.Mutex`
  usable immediately) vs pthread init requirements — a zero-filled pthread_mutex_t is not
  portably valid. Options: (a) runtime lazy-init flag checked in lock/add (one branch per
  op — likely acceptable); (b) require make-style construction (breaks Go compat);
  (c) glibc-only zero-init assumption (rejected — unportable). Lean (a); verify
  goo_mutex_new/goo_waitgroup_new internals first.
- Acceptance (roadmap): 4 goroutines increment under Mutex, wg.Wait, prints 'counter: 4'.

### Order: B1 → B2 → B3 (each gates the next). Struct-field channel segfault fix (task #9)
slots naturally after B2 (same selector/codegen neighborhood).

## Gates

goo-grammar procedure for every parser.y commit (tripwire exact-or-ledgered, golden grows,
`go run`-verified expected outputs for accepts). Standard suite + verify-core per commit.
Review wave: grammar dimension is mandatory Opus; parity dimension Sonnet.
