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

Re-run the static audit with the helper script committed alongside this
doc:

```
make ccomp-audit
```

The target prints counts for every category above and exits 0 — it's
a regression check, not a gate. The numbers will shift as adaptation
work lands; the doc tracks the current state.

## Empirical run (CompCert 3.15 installed 2026-05-15)

After CompCert 3.15 was installed (opam, V1-ccomp-install), running
`ccomp -c` against every `.c` file in `src/` (excluding `src/package/`,
which is excluded from the regular build):

```
make ccomp-survey                          # see Makefile target
```

**Result: 69 of 119 files compile cleanly under ccomp.** Required
flags:

```
ccomp -c file.c -Iinclude -I/opt/homebrew/include \
  -I/opt/homebrew/Cellar/llvm/22.1.4/include \
  -std=c99 -fstruct-passing \
  -DLLVM_AVAILABLE=1 -D__STDC_CONSTANT_MACROS \
  -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS
```

`-fstruct-passing` is mandatory — without it ccomp rejects functions
that take a `goo_string_t` (or any struct) by value, which the lexer
does heavily.

### What actually killed the 50 failing files

The static-grep audit overstated two issues and missed one entirely:

**LLVM C API — NOT a blocker.** CompCert treats every `LLVMValueRef`,
`LLVMBuild*`, etc. as an opaque external symbol once the LLVM headers
are on the include path. The 478 call sites compile fine. Codegen +
runtime files (`src/codegen/*.c`, `src/runtime/*.c`) all pass except
where they pull in `<stdatomic.h>` transitively.

**`_Atomic` / `<stdatomic.h>` — the real headline blocker.**
Including `<stdatomic.h>` at all kills CompCert with:

```
.../stdatomic.h:97:9: syntax error after 'typedef' and before '_Atomic'.
```

This cascades through transitive includes. Anything that includes
`include/work_stealing.h`, `include/async_resource.h`,
`include/async_streams.h`, `include/numa_scheduling.h`, or
`include/capability_security.h` is dead until those headers are
either guarded by `#ifndef __COMPCERT__` or rewritten to avoid C11
atomics.

**`typedef enum : type` (C23 enum with underlying type) — the
contracts-header killer.** `include/contracts.h:19`:

```c
typedef enum : unsigned char {
    CONTRACT_PRECONDITION = 0,
    CONTRACT_POSTCONDITION,
    ...
} ContractType;
```

CompCert: `syntax error after 'enum' and before ':'`. The `:
unsigned char` is C23. Every file including `contracts.h`
(`src/types/contracts.c`, `proof_generation.c`,
`flow_analysis_core.c`, etc.) fails on this single line. Trivial
fix: strip the `: unsigned char` underlying-type specifier.

### Other real failures (one-offs)

- `src/comptime/code_specialization.c:245` — VLA (variable-length
  array). CompCert doesn't support VLAs.
- `src/errors/error_recovery.c:117` — actual code bug: duplicate
  typedef `RecoveryConfig` with different definitions. Worth fixing
  regardless of CompCert. (One more pattern of the
  status-docs-not-ground-truth issue — that subsystem is supposedly
  done.)
- `src/security/crypto_security.c:42` — inline `asm`. CompCert needs
  `-finline-asm` flag to accept it; might just work if added.

### Updated minimal CompCert-buildable subset

Empirically verified to compile cleanly under ccomp with the flags
above:

- `src/lexer/`, `src/parser/`, `src/ast/`
- Most of `src/types/` (those NOT including contracts.h or transitively
  pulling in stdatomic.h)
- All of `src/codegen/` (except files that transitively reach
  stdatomic.h)
- Most of `src/runtime/`, `src/errors/`, `src/comptime/`, `src/ide/`

The "minimal pilot" originally proposed (lexer + parser + ast) is
much smaller than what actually works. **A near-complete compiler
build under CompCert is achievable** if the stdatomic.h and contracts.h
header issues are fixed — likely <100 lines of source edits total.

### Updated path to V1 completion

Revised next steps:

1. **`V1-stdatomic-cleanup`** — guard `<stdatomic.h>` includes with
   `#ifndef __COMPCERT__` and provide stub `_Atomic` macro for ccomp
   builds. ~5 headers affected.
2. **`V1-c23-enum-cleanup`** — strip C23 underlying-type from enums
   (1 site in contracts.h, possibly more — needs a sweep).
3. **`V1-ccomp-link`** — `make ccomp-core` target that compiles the
   verified-buildable subset and links it. The linker step is where
   we'll find out whether CompCert .o files link with system libs.
4. **`V1-ccomp-full`** — get to 119/119 (or document the residual gap
   honestly).

The full Goo bootstrap path then becomes much cheaper than predicted:
not a multi-month codegen rewrite, but a multi-day header-cleanup pass.

## State after V1-stdatomic-cleanup (2026-05-15)

**91 of 119 files compile** under CompCert (77%, up from 69 / 58%).

Mechanism: `include/ccomp_shim.h` declares stub typedefs and macros
for everything CompCert can't natively handle, and `make ccomp-survey`
force-includes the shim via `-include include/ccomp_shim.h`. Source
edits to non-shim files were minimal — only 2 sites in
`include/shared_variables.h` had the `_Atomic(T)` parametrized form
that can't be expressed via a flat keyword macro; both rewritten to
`GOO_ATOMIC_PTR` / `GOO_ATOMIC_SIZE_T` macros defined in the shim.

Shim handles, transparently under `__COMPCERT__`:

- `_Atomic`, `_Thread_local`, `__thread` — stripped to empty
- `<stdbool.h>` — pulled in (C23 makes `bool` a keyword; C99 needs it)
- 18 `atomic_*_t` typedefs (least/fast/intptr/etc.) — non-atomic
- `memory_order` enum + relaxed/seq_cst/etc. — constants only
- `atomic_load`/`store`/`fetch_add`/etc. — degrade to plain reads/writes
- `GOO_ATOMIC_PTR` / `GOO_ATOMIC_SIZE_T` — for the 2 `_Atomic(T)` sites

**Correctness trade-off**: ccomp-built binaries from this tree are
single-thread-safe only. Atomic operations under the shim are NOT
atomic. V1's goal is verifying the COMPILER's translation
correctness, not the runtime's concurrency guarantees — so this is
in scope. Don't run ccomp-built binaries in a multi-threaded context.

## Long tail of remaining failures (~28 files)

Each is a per-file fix, no longer a single cascade:

| Pattern | Files | Fix |
|---|---|---|
| `^` Apple block-syntax in expressions | async_streams.c:1019 | rewrite as plain function pointer |
| `typeof(...)` compound literal | capability_security.c:576 + 2 others | rewrite with explicit type names |
| Undeclared types in headers (genuine code bug) | error_reporting.h:193 (`ErrorContext` never declared), error_recovery.h:117 (dup typedef) | declare or unify the types |
| `Pipeline` struct dup-defined | advanced_channels.h:482 | unify the duplicate |
| VLA | code_specialization.c:245 | fixed-size buffer |
| Inline `asm` | crypto_security.c:42 | add `-finline-asm` flag or rewrite |
| Other long-tail | ~20 files | per-file investigation |

These are tracked as future `V1-*` follow-ups; not blocking V1-ccomp-link
since the lexer/parser/AST/most of types and codegen all build under
CompCert today.
