# Most Direct Path to TinyGo-like Standard Library Support

**Date:** 2026-07-01
**Branch:** `feat/v1-stdlib-imports`
**Source:** Approved ultraplan (cloud session), pasted verbatim below.
**Status:** Keystone de-risked (see note); executing the minimal first shippable milestone (Phase 0 + Phase 1 on `errors`).

## Keystone spike result (verified before execution)

The one unknown that could have forced a redesign — **string/source lifetime across multiple
parsed files** — is resolved SOUND:
- `parse_input` (src/parser/lexer_bridge.c:378) is self-contained: creates its own lexer,
  resets M10 state on entry, runs `yyparse()`, sets global `ast_root`, frees the lexer.
- AST constructors `str_dup` every stored string (ast_constructors.c:46, 59-60, 126, 140, …),
  so AST nodes own their strings independently of the source buffer / token memory.
- Therefore the driver can parse each package, snapshot `ast_root`, free that source, and
  proceed — the multi-package "compile once, cache by path, topological order" model is safe.

---

## Context

**Goal (user-clarified):** "Goo is a Go superset, so the core Go standard library should
actually work in Goo." This is precisely TinyGo's model: TinyGo does **not** rewrite the
stdlib — it compiles the *real upstream Go stdlib source* and overrides only the
runtime/syscall-heavy packages. We want the same for Goo.

**Why now:** Today's stdlib is a hand-wired **C shim** covering ~20 functions across
6 packages. Each function is manually duplicated in three places, so it does not scale and
the stdlib is written in C, not Goo. Meanwhile a real `stdlib/*.goo` tree exists but is
written in a **stale dialect the current grammar cannot parse**, so it is never compiled.

### Verified current state (all confirmed by reading the code)

- **End-to-end compile→run WORKS today.** `bin/goo file.goo -o out` → parse → type-check →
  LLVM IR → object → link against `lib/libgoo_runtime.a` (clang/gcc) → native executable.
  Driver: `src/compiler/goo.c` `compile_file`. Link: `src/codegen/codegen.c`
  `codegen_emit_executable` (~627) + `goo_runtime_archive_path`. (The old
  `COMPILER_STATUS.md` claim that executables aren't produced is **stale**.)
- **The grammar is a genuine Go superset.** `src/parser/parser.y` accepts Go-canonical
  `func f(a int, b int) (int, int)`, named-field struct literals (`struct_lit`, ~1667),
  maps, `range`, `switch`/`case`, `defer`, `go`, interfaces, anonymous-func **closures**
  (`FUNC func_signature` expr, ~1904), `iota` (`substitute_iota`), and type params — plus
  Goo extensions `!T` (error union), `?T` (nullable), `try`, ownership. Passing examples
  confirm: `examples/baseline_probe.goo`, `composite_map_probe.goo`, `commaok_probe.goo`.
- **Imports are parsed but never resolved.** `ProgramNode.imports` (list of
  `ImportSpecNode{path, alias}`, `include/ast.h`) is walked with a literal
  `// TODO: Handle imports` in both `src/types/type_checker.c:294` and
  `src/codegen/codegen.c:219`.
- **Current stdlib = hardcoded C shim, wired in 3 places per function:**
  1. Type-checker signatures: `stdlib_package_lookup` in
     `src/types/expression_checker.c` (~1137), consulted by `type_check_selector_expr`
     (~1263) when the selector base is `TYPE_PACKAGE`.
  2. Package names registered as `TYPE_PACKAGE` vars in `type_checker.c` (~205:
     `{"fmt","os","strings","math","strconv","errors"}`).
  3. Codegen routing if-chain in `src/codegen/call_codegen.c` (~310+), each `pkg.Fn`
     mapped to a `goo_*` call. Impls in `src/runtime/runtime.c`, `io.c`; declared to LLVM
     in `src/codegen/runtime_integration.c` (`codegen_declare_runtime_functions`).
- **`stdlib/*.goo` is stale** (colon params `a: int`, `->` results — `ARROW` is a channel
  token, not result syntax). It does not parse under the current grammar. Treat as
  non-authoritative; do not build on it.
- **Test harness:** `scripts/run_golden.sh` compiles+runs+diffs `examples/*.goo` against
  `examples/*.expected.txt`; `make lexer` builds `bin/goo` + `lib/libgoo_runtime.a`;
  per-probe Makefile targets (`baseline-probe`, `smoke-stdlib`, `m12-probe`).

## Strategy (TinyGo model adapted to Goo)

The single missing foundation is **package import + multi-file compilation**. Everything
else is incremental. Once packages can be loaded and compiled, we consume *real Go stdlib
source* for the pure/leaf packages and provide thin **overrides** (Goo- or C-backed) for
the runtime/syscall-heavy ones — exactly how TinyGo splits its stdlib.

The hardcoded C shim is **kept as a per-symbol fallback** throughout, so every currently
green gate (`baseline-probe`, `smoke-stdlib`, `m12-probe`) stays green while real packages
take over one symbol at a time.

### Additional verified facts that shape the design

- Scope model is a **single flat namespace**: `struct Scope` (`include/types.h:257`) holds
  one `Variable*` list — functions, vars, named types, methods (`T__m`), and package
  markers all live in it. There is no separate type table or package namespace yet.
- Codegen emits into **one LLVM module**; calls resolve by `LLVMGetNamedFunction(module,
  name)`. So cross-package symbols only need **unique mangled names**, not separate
  objects/linking.
- No `GOOROOT`/build-tag/`//go:build`/`//go:linkname` machinery exists (greenfield). No
  generic **monomorphization** engine exists (concept type-params only).
- Named-field struct literals **do** parse (`struct_lit_init: identifier COLON expression`,
  parser.y:1729) — the `baseline_probe` comment saying otherwise is stale.

## Staged implementation

### Phase 0 — Import resolution + multi-package compilation (the core unlock)

1. **GOOROOT + source layout.** New `src/package/import_resolver.c` (dir already exists),
   mirroring `goo_runtime_archive_path()` precedence: `$GOOROOT` → `<exe>/../lib/goostd` →
   `./goostd`. A package = the `*.go` files in `goostd/<path>/` (dir = package, Go
   semantics; skip `_test.go`). Do **not** reuse the stale `stdlib/*.goo` tree.
   Exposes `resolve_import(path) -> PackageSource{files, name}`.
2. **Package namespace data structures.** Add `struct Package { import_path; name; Scope*
   exports; int state; ... }` to `include/types.h`, plus `TypeChecker.packages` and
   `current_package`. Reuse `Scope`/`Variable`/`scope_*` verbatim — no changes to those.
3. **Name mangling.** Non-`main` package symbols get `goo_pkg__<pkg>__<name>` (methods:
   `goo_pkg__<pkg>__<Type>__<method>`), applied at `LLVMAddFunction`
   (`function_codegen.c:293`). `main` keeps bare names (backward compatible).
4. **Import-graph walk in the driver.** In `goo.c compile_file`, between parse (~326) and
   type-check (~350): resolve imports, recursively `parse_input` each package (capturing
   the global `ast_root` immediately after each call — the parser is global-state, so
   serialize + snapshot), build the dependency DAG from each package's own `imports`,
   detect cycles via the tri-color `state`, and process in **topological order** (leaves
   first, `main` last) so a package's `exports` are populated before any importer needs
   them. Cache by import path (compile each package once; one shared `TypeChecker` +
   `CodeGenerator`/module).
5. **Package-scoped checking + export filter.** Refactor `type_check_program`
   (`type_checker.c:272`) → `type_check_package(checker, pkg, program)`: push a package
   scope, run the existing decl loop unchanged, then copy **exported** (leading `A`–`Z`)
   top-level symbols into `pkg->exports`. Make the hardcoded package-marker seeding
   (`type_checker.c:205`) conditional on real imports and carry the `Package*` on the
   marker.
6. **Selector resolution (the key edit).** In `type_check_selector_expr`
   (`expression_checker.c:1273`), for a `TYPE_PACKAGE` base: look up
   `scope_lookup_variable(pkg->exports, selector)` first (real `TYPE_FUNCTION` with true
   parameter types → real arg checking); **fall back** to `stdlib_package_lookup` for
   runtime intrinsics. Mirror the same order in codegen (`call_codegen.c:310+`): try
   `LLVMGetNamedFunction(module, goo_pkg__<pkg>__<name>)` before the hardcoded `goo_*`
   arms.
7. **Exit criterion.** A hand-written `goostd/mypkg/mypkg.go` with one exported pure
   function, imported and called from a `.goo` file, compiles and runs; all existing
   golden gates stay green.

### Phase 1 — Prove it on real upstream Go source (pure/leaf packages)

- **First package: `errors`** (recommended). Tiny, pure, and it exercises the whole new
  machinery — exported struct (`errorString`), constructor (`New`), a method satisfying the
  `error` interface, and `Is`/`As`/`Unwrap`. It already has a shim, so flip symbols over
  one at a time and diff against the green path. Forces the `error`-type reconciliation
  (Goo models `error` as a nullable boxed value; Go uses an interface) on the smallest
  surface.
- Then, increasing difficulty: `math/bits` → `unicode/utf8` → `math` → `strconv` →
  `strings` → `bytes` → `sort`.
- **Triage method:** vendor upstream `$(go env GOROOT)/src/<pkg>/*.go` (drop `_test.go`,
  `*.s`, unwanted `//go:build` variants), run `bin/goo <file> --emit-ast` then full
  compile, and bucket each failure as a narrow parser / typechecker / codegen gap task.
  Never invent syntax to route around real Go.

### Phase 2 — Runtime-backed packages via thin overrides (TinyGo model)

- **Override by filename convention, not build tags** (build-tag parsing is out of scope):
  in `resolve_import`, overlay `*_goo.go` files over vendored upstream; on symbol-name
  collision the override wins (drop the upstream decl). Packages needing `unsafe`/asm/
  syscalls simply don't vendor those files — the `_goo.go` override supplies plain-Goo
  bodies that call `goo_*` intrinsics.
- **Binding to `libgoo_runtime.a`:** reuse existing `goo_*` where already shimmed; for new
  runtime funcs, declare in `codegen_declare_runtime_functions`
  (`runtime_integration.c:48+`), implement in `src/runtime/`, and expose via a small
  intrinsic-lowering table in `call_codegen.c` (the one new codegen hook Phase 2 needs).
- Order of leverage: `os` (Exit/Getenv/file I/O already backed) → `io` (Reader/Writer
  interfaces) → real variadic `fmt` → `sync`/`runtime`/`syscall` (thin shims over
  `src/runtime/` concurrency).

## Critical files

- `src/compiler/goo.c` — `compile_file`: import-graph walk + `parse_input` re-entrancy.
- `src/package/import_resolver.c` *(new)* — GOOROOT resolution, per-package source discovery, `*_goo.go` overlay.
- `include/types.h` — `Package` struct; `TypeChecker.packages`/`current_package`.
- `src/types/type_checker.c` — `type_check_program`→`type_check_package`, export filter, conditional package seeding (~205).
- `src/types/expression_checker.c` — `type_check_selector_expr` (~1273) consult `pkg->exports` first; keep `stdlib_package_lookup` (~1137) as fallback.
- `src/codegen/call_codegen.c` — selector routing (~310): mangled real symbol before `goo_*` arms; new intrinsic table for Phase 2.
- `src/codegen/function_codegen.c` — apply cross-package mangling (~293).

## Key risks

1. **Generics (highest).** `sort`/`slices`/`maps` and increasingly `strings`/`bytes` use
   type params; no monomorphization exists. Milestone 1 uses non-generic package subsets;
   generic instantiation is a separate large follow-on.
2. **`unsafe`/asm/`//go:linkname`.** Handled by the override convention — never vendor
   those files.
3. **Interface satisfaction / method sets** (`error`, `io.Reader`) — harden
   `type_interface_satisfied`; `errors` surfaces it early.
4. **Parser global state** (`ast_root`/`current_lexer`) — serialize + snapshot in the
   driver walk. (RESOLVED sound — see keystone spike note above.)
5. **Closures/maps/rune-range codegen** — verify capture and `for i,r := range s` for
   `unicode/utf8`/`strings`.

**Minimal first shippable milestone:** Phase 0 + Phase 1 on `errors` only. Explicitly
excludes generics, `unsafe`, build tags, and variadic `fmt`.

## Cross-cutting constraint: syntax-extension policy

The stdlib goal ("compile real Go source") makes **Go lexical/source compatibility a hard
constraint** that governs any new syntax. Rules, in priority order:

1. **Never reserve a new bare keyword.** A word like `parallel`/`matrix`/`kernel` would
   shadow identifiers that appear in real Go stdlib source and silently break it. (Even Go
   adds features via *shadowable predeclared identifiers* like `any`/`min`/`max`, not new
   reserved words.)
2. **Sigil-led constructs are the safe extension surface.** Bytes Go's lexer rejects
   (`@`, `#`, and the existing `!T`/`?T`) can never appear in valid Go source, so they
   cannot collide. This is already how error-union/nullable/`@attribute` coexist with Go.
3. **Prefer the existing `@attribute` machinery over new syntax** for compiler directives
   (parallelism, SIMD, GPU, inlining). It already exists and is the idiomatic Goo analogue
   of Go's `//go:` comment directives — promoted to first-class annotations.
4. **Prefixed keywords (`par_`, `gpu_`) only when a real statement grammar is needed**
   (e.g. `par_parallel for`), not for annotations.
5. **True "decorator" (wrap/replace a function) belongs to the existing comptime/derive
   macro system** (`src/comptime`, `derive_macros`, `template_macros`), not a new runtime
   `@` concept — attributes are compile-time metadata, not higher-order wrappers.

Practical implication for this effort: **freeze new surface syntax during the stdlib
milestones.** Every new bare token is a risk to upstream Go compatibility; lean on
attributes + macros instead. Note `@` is currently overloaded (deref operator *and*
attribute prefix) — disambiguated by position today; revisit before adding more `@` load.

## Verification

- `make lexer` builds cleanly; existing gates (`make baseline-probe`, `make smoke-stdlib`,
  `make m12-probe`) stay green (shim fallback intact).
- Phase 0: a hand-written `goostd/mypkg/mypkg.go` imported from a `.goo` file compiles,
  links, and runs with correct output.
- Phase 1: a new golden fixture (`examples/errors_probe.goo` + `.expected.txt`, run via
  `scripts/run_golden.sh`) imports real upstream `errors` and matches Go's behavior;
  extend to `strconv`/`strings` probes whose output matches Go for the same inputs.
