# Most Direct Path to TinyGo-like Standard Library Support

**Date:** 2026-07-01
**Branch:** `feat/v1-stdlib-imports`
**Source:** Approved ultraplan (cloud session), pasted verbatim below.
**Status:** Keystone de-risked (see note); executing the minimal first shippable milestone (Phase 0 + Phase 1 on `errors`).

## UPDATE — plan refined to v3 (broad-coverage thesis + real-import-graph ladder)

The approved plan was refined twice after this doc's first capture. Consolidated deltas
(Phase 0 is **byte-identical** across all versions — only downstream framing changed):

- **Strategic thesis:** the target is **broad std coverage** (jumpstart the language by
  running real programs), not one demo package. `errors` is only the first rung. "The whole
  std" is a ladder, not a switch: measure progress as the growing % of std packages that
  compile + pass a behavior probe.
- **Execution context (env reconciliation):** the plan's Phase 0.0 "unblock the build"
  notes (gcc 13.3 rejecting `-std=c23`, LLVM header gating) were observed in the *planning
  sandbox*. This local machine is gcc 16.1.1 / clang 22.1.8 / LLVM 22.1.8 — all accept
  `-std=c23`, `bin/goo` builds and runs, and two milestones (#57/#58) merged green this
  session. **M0 is already satisfied here; Phase 0.0 is skipped locally.** (Apply the
  toolchain-aware `-std` flag only as a portability nicety if touched.)
- **Phase 1 precedence = reverse-topological, leaf-first** (from the real Go 1.24 import
  graph): step 0 `errors` (deps none; land plain `New`/`Unwrap` first — `Is`/`As`/`Join`
  pull `internal/reflectlite`, defer) → step 1 `unicode/utf8`, `unicode`, `math/bits`,
  `sync/atomic` (deps none) → step 2 `math` → `strconv` (**first major hub reachable from
  leaves alone** — the visible jumpstart) → step 3 `io` (needs the `sync`→`runtime` shim) →
  step 4 `bytes`, `strings` → `bufio` → step 5 `sort` (**gated on generics**) → step 6
  `fmt` (heaviest — pulls `reflect`/`os`; keep the C-shim fallback until then).
- **Generics/`iter` fork (decide before step 4):** on Go ≥1.21 the classic text/number core
  is NOT generics-free (`sort→slices`, `bytes`/`strings`→`iter`). Recommended: **(a)**
  vendor the core from **≤ Go 1.20** upstream to stay generics-free; revisit **(b)** stubs +
  generics-first when Tier-3 generic packages become the goal.
- **Milestone ladder:** M0 = clean build (green here). M1 = Phase 0 + `errors` (machinery
  proven). M2 = steps 1–2 (leaves → `math` → `strconv`, the visible jumpstart). M3 = steps
  3–4. M4+ = `sort`/`fmt` + beyond as generics/reflect mature.
- **Coverage scoreboard:** a `goostd/STATUS.md` / `make std-status` manifest tracks every
  std package's state (`compiles` / `passes-probe` / `partial` / `deferred`), one golden
  probe per landed package (`examples/std_<pkg>_probe.goo`, diffed against real `go run`).

**v4 additions (canonical spec + version pin):**
- **The authoritative definition of "the whole std" is Go's own `api/*.txt` manifest** —
  the union of `$GOROOT/api/go1.txt`, `go1.1.txt`, … `go1.26.txt`. Each line is one promised
  exported symbol (`pkg PKG, {func|type|method|var|const} NAME SIGNATURE`). Per-version files
  list only that version's additions; the cumulative union is the full contract.
- **Pin one upstream Go version as the north star: Go 1.26 / master** (per the user). Vendor
  from that tag for consistency — the only exception is the temporary bootstrap of vendoring
  a few generics-free core packages from ≤1.20 until generics land (the `iter`/`slices` fork).
- **Coverage scoreboard is symbol-exact:** `make std-status` (script under `scripts/`) parses
  every `pkg PKG, KIND NAME …` line from `$GOROOT/api/go1*.txt` into a symbol set, records per
  landed `goostd/<pkg>` which symbols compile and which have a passing probe, and emits
  `goostd/STATUS.md` with per-package + overall percentages (`symbols-compiling` /
  `symbols-probed` / `deferred`). Progress = % of the manifest symbol set at `probed`. This
  doubles as a regression gate. A package is "done" only when its `api/*.txt` exported symbols
  compile AND its `examples/std_<pkg>_probe.goo` matches real `go run` output.

The full v3/v4 text (import-graph deps per package, tier lists, api-manifest format) lives in
the session history; this doc keeps the durable design + the ladder. **Current execution:** M1
(Phase 0 + `errors`), Phase 0 in progress.

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
