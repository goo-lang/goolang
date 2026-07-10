# Phase 4 sub-project A — shim signatures, import aliases, relative imports (P4.1, P4.4, P4.5)

Date: 2026-07-10. Branch: `feat/p4-packages-a`. Prereq: Phase 3 complete (#171-#174).
No grammar exposure: import paths are STRING_LITERALs (parser.y:305-319); slash and ./ are
string data. Verified none of the three tasks touch the parser.

## Recon facts (verified 2026-07-10)

- **Shim knowledge is spread over four hand-synced sites**: checker `stdlib_package_lookup`
  (expression_checker.c:4146-4293, strcmp chains returning `type_function(NULL,0,ret)` — zero
  param info), codegen lowering chains (call_codegen.c:907-1120+ generic + bespoke arms; fmt.*
  emitters), value members (composite_codegen.c:536,542), and the driver's
  `is_stdlib_shim_import` list + marker seeding (goo.c:441,462).
- **Arity checking deliberately skips shims**: `check_signature` fires only for identifier
  variables, methods, and source-package exports matched by type identity
  (expression_checker.c:3544-3639); shim types match none → `math.Sqrt("x")` reaches the LLVM
  verifier. Comment at :3613 documents the old intent.
- **Import resolution is one flat join**: `resolve_import` (import_resolver.c:86) does
  `GOOROOT-tier/goostd/<path>`; `unicode/utf8` fails there. No source-dir tier exists; the main
  filename is not threaded into the walk (goo.c:491→resolve_import takes only the path).
- Multi-package pipeline (topo walk, cycle detection, `goo_pkg__` mangling, --dump-packages)
  fully exists — P4.5 is resolver plumbing only.

## Design decisions (Fable, 2026-07-10)

### A1 (P4.1) — declarative shim signature table, checker-side

**Chosen**: one static table `shim_signatures[]` — `{pkg, name, ret, params[], variadic}` — in a
new small unit consumed by `stdlib_package_lookup`, which now builds `type_function` with REAL
param lists; the call checker's `check_signature` gate learns to fire for shim-resolved callees
(a marker on the returned type or a parallel lookup — implementer picks the least invasive hook
consistent with the :3609 source-export arm). fmt.\*: `Println/Print/Sprint/Sprintln` = variadic
any (no fixed params — args stay unchecked beyond arity>=0); `Printf/Sprintf/Errorf` = fixed
first param string + variadic any (first-arg type IS checked). Codegen's bespoke arms stay
untouched this task — the checker now guarantees the arities they already assume.

Alternatives: (a) param-info inline in the existing strcmp chains — keeps four sync sites and
buries signatures in control flow; rejected. (b) full checker+codegen single-table unification —
right direction but codegen arms are bespoke marshaling, not table-driven; deferred to the P5.6
dead-code/structure pass with a pointer comment. The declarative table at least becomes the
single source of truth for SIGNATURES, and codegen drift against it is caught by the P4.11 smoke
suite (every symbol exercised end-to-end).

Acceptance (roadmap): `math.Sqrt("x")`, `strings.Contains(1,2)`, wrong-arity calls rejected at
type-check with positions; existing e2e probes pass. Reject fixtures for each class + a
positive probe that every shim still compiles (smoke-lite; the full suite is P4.11).

### A2 (P4.4) — import path alias normalization

A tiny normalization table consulted before the GOOROOT join AND in `is_stdlib_shim_import` /
marker seeding: `unicode/utf8 → utf8`, `math/bits → bits`. Flat spellings keep working. e2e
probe imports both nested spellings and calls one symbol from each.

### A3 (P4.5) — source-dir-relative imports

Thread the main .goo file's directory into the import walk (field on the walk context set once
in compile_file; NOT a global if avoidable). Resolution:
- `./name` (leading dot-slash) → resolved against the main-source dir ONLY.
- bare `name` → GOOROOT tiers first, then the main-source dir as a LAST tier.

**Deliberate deviation from the roadmap sentence** ("searches the main .goo file's directory
before GOOROOT"): source-dir-FIRST for bare names would let a local directory named `strings`
silently shadow the stdlib — a footgun. `./` is the explicit local spelling (resolves only
locally); bare names prefer the stdlib and fall back to local. Record this in the roadmap row
when marking done. Probes: `./mathx` beside the program imports and runs without GOOROOT
containing it; a bare-name local package resolves via the last tier; goostd resolution,
--dump-packages topo order, and cycle detection unchanged (existing probes).

## Gates

Per-commit: make lexer + golden (393/0 baseline) + reject (64/0 baseline) + unit 76/1 + tripwire
121/256 exact (no grammar changes — hard requirement) + verify-core. New fixtures raise counts.
Pre-PR: review wave scaled to risk (checker/resolver — moderate; no codegen IR changes expected
beyond none). Sub-A exit: the three roadmap acceptance rows demonstrably green.
