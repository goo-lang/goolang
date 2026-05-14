# CompCert Compatibility Audit

**Date**: 2026-05-15
**CompCert version available**: `coq-compcert 2.0.0` (opam, not yet installed locally)
**Goo CFLAGS**: `-Wall -Wextra -std=c23 -g -Iinclude -I/opt/homebrew/include`
**Method**: static-only audit (CompCert install is a separate task; `ccomp` not run)

## Context

The long-term path is: bootstrap Goo through a verified C compiler so the
trusted base for self-hosting Goo→Goo is sound. CompCert is the canonical
verified-C-compiler (Rocq-based, ~15 years of work at INRIA). This audit
catalogues what stands between Goo's current C source and "buildable
under CompCert."

The honest summary: **substantial work**. Goo's compiler is written in
C23 with `-std=c23`, uses C11 atomics, depends on the LLVM C API for the
entire codegen path, and uses GNU `typeof`. CompCert supports C99 + a
subset of C11 + no GNU extensions beyond `__attribute__` (mostly ignored).

## Blockers — must change for a CompCert build

### 1. `-std=c23` build flag

`Makefile:2` forces C23 mode. CompCert speaks C99 + some C11. A separate
CompCert-build flag setup is needed (e.g., a `ccomp` build target that
substitutes `-std=c99`).

### 2. LLVM C API dependency (478 call sites)

```
grep -rE "\bLLVMBuild|\bLLVMConst|\bLLVMType|\bLLVMAdd|\bLLVMGet" src/  →  478 matches
```

Files including `<llvm-c/...>`: at least 1 directly, with `LLVMValueRef`/
`LLVMTypeRef` flowing transitively through `include/codegen.h` and into
every codegen translation unit.

CompCert **can** compile call sites against an opaque external library —
it doesn't need to understand LLVM's internals to emit calls. But:
- The resulting binary still depends on unverified LLVM at link time.
  The trusted base extends only as far as CompCert; LLVM is in the TCB.
- LLVM types in CompCert-visible function signatures are fine as long
  as they're opaque pointers / structs.

**Decision needed**: keep LLVM as an unverified external dep (pragmatic),
or replace it with a CompCert-friendly backend (e.g., C output, or asm
emitted via CompCert's own backend). Replacement is a multi-month rewrite
roughly equivalent to writing a new codegen.

### 3. C11 atomics (`_Atomic`, `<stdatomic.h>`)

Files using `_Atomic`:
- `src/types/taint_analysis.c`
- `src/security/security_auditing.c` (2 uses)
- `src/security/security_framework.c`
- `src/security/capability_security.c`
- plus 25+ token-level matches across src+include

Files including `<stdatomic.h>`:
- `include/work_stealing.h`
- `include/capability_security.h`
- `include/numa_scheduling.h`
- `include/async_resource.h`
- `include/async_streams.h`

CompCert has limited atomics support. These data structures need
non-atomic equivalents for the CompCert build, or to be excluded.
Tactically: most usage is in security/auditing modules that are
not on the core compile path — same pattern as the IPFS package
manager was during M7-stdlib-expansion. Likely safe to exclude
from the CompCert build target.

### 4. GNU `typeof` (3 compound-literal sites)

`src/security/capability_security.c` (2) and
`src/security/security_framework.c` (1):

```c
system->audit_log[system->audit_count++] = (typeof(system->audit_log[0])){
    ...
};
```

`typeof` is GNU C (and C23, but CompCert is pre-C23). Rewrite with
explicit type names. Trivial fix.

## Non-blocking but worth noting

### `__attribute__((unused))` — 51 uses

CompCert tolerates `__attribute__` syntactically and ignores most
attributes (including `unused`). No change needed.

### Designated array initializers (C99) — fine

```c
[SAFETY_CHECK_TYPE]   = "Type Safety",
[SAFETY_CHECK_MEMORY] = "Memory Safety",
```

C99, supported.

### `__builtin_*` — 5 uses

Need to check each call site. Some `__builtin_*` are CompCert-recognised
(`__builtin_unreachable`, `__builtin_expect`); others may not be.
Sample audit deferred until the install lands.

### 4,113 lines of "verification" C code

`src/types/contracts.c`, `proof_generation.c`, `dependent_types.c`,
`bounds_verifier.c` total ~4k lines. Per the established pattern in
this repo (`status-docs-not-ground-truth` memory), most of this is
likely aspirational — the type-checker has been a year-old TODO
in plenty of other places. Even if it builds, it doesn't currently
prove anything verifiable. Recommended to **exclude from the
CompCert build target** until the contracts→Rocq pipeline is real
(future `V1-contracts-rocq-export` task).

## Recommended minimal CompCert-build subset

To prove the path works without doing the whole adaptation up-front:

| Subset | Include |
|---|---|
| Core | `src/lexer/`, `src/parser/parser_errors.c`, `src/ast/` |
| Exclude | Everything else, especially `src/codegen/`, `src/security/`, `src/runtime/concurrency.c` |
| Build target | new `make ccomp-core` that uses `-std=c99` and `ccomp` |

This compiles a parser-only binary that can `--emit-tokens` and
`--emit-ast` under a verified trust base. The full codegen path stays
on `gcc`/`clang` for now. Bootstrap proper still requires the
codegen path to be brought under CompCert (or replaced).

## Next steps — coord tasks

- `V1-ccomp-install` — get `ccomp` actually installed (opam install
  was blocked on system deps requiring sudo; user-driven follow-up)
- `V1-ccomp-core-build` — `make ccomp-core` for lexer + ast subset
- `V1-llvm-decision` — pick: keep LLVM as opaque external, or replace
- `V1-atomics-cleanup` — non-atomic versions of the security counters
- `V1-typeof-cleanup` — replace 3 `typeof` sites with explicit types
- `V1-contracts-audit` — separate audit of `src/types/contracts.c`
  and friends to know what's salvageable for the Rocq pipeline
- `V1-rocq-pilot` — the originally-proposed verified-typechecker
  pilot, sequenced after `V1-ccomp-core-build`

## Verification

Re-run this audit with the helper script committed alongside this doc:

```
make ccomp-audit
```

The target prints counts for every category above and exits 0 — it's
a regression check, not a gate. The numbers will shift as adaptation
work lands; the doc tracks the current state.
