# Haskell front end, Zig back end: strangler plan

Date: 2026-09-04. Status: draft, not yet approved. Baseline: main de24a23.

## Decisions this plan assumes

- **D1 Cut.** Haskell owns lexing, parsing, type checking, comptime, escape analysis and the release plan. Zig owns LLVM codegen (C API via `@cImport`), the runtime, the driver and the build. They meet at one serialized program: a typed, fully monomorphized AST plus the release plan plus the package export tables codegen needs.
- **D2 Swap discipline.** One `bin/goo` throughout. Each replaced component is validated by a differential gate in `verify-core` (token dump, AST dump, typed-program dump, plan dump, emitted IR) against the C implementation across all 495 run goldens and 157 reject goldens, flipped only when the diff is empty, then the C version is deleted. A `GOO_FE=c|hs` / `GOO_BE=c|zig` kill switch survives exactly one release after each flip.
- **D3 Interchange format.** Canonical JSON, versioned, deterministic key order, positions included. Text so it doubles as the diff format. Haskell via aeson, Zig via `std.json`. Revisit CBOR only if size measurably hurts.
- **D4 Toolchains.** GHC 9.10 series via ghcup with cabal, Zig 0.16.0, both pinned in-tree and asserted by a gate. The "clean machine" clause of P5's exit gate becomes "GHC and Zig installed, no CompCert".
- **D5 Diagnostics.** The type checker's message strings become a catalogue file both implementations read. Reject fixtures match stderr substrings, so verbatim messages are the parity contract; positions stay in the dump diff.
- **D6 Retirements, in order of arrival.** `ucontext` scheduler and NNG cmake build (with the runtime); C codegen and its 179 checker call sites (with the back end); Bison, `parser.y`, the conflict tripwire and the goo-grammar skill (with the front end); CompCert pilot, `ccomp_shim.h`, MISRA policy, the Bazel migration at phase 3b, and finally Make (with the build). Each retirement is its own commit with the gate list it removes.
- **D7 Oracle.** `make verify-core` exits 0 at every commit on the Linux runner. macOS arm64 joins the CI matrix at Phase 2 and stays.

## Phase 0: format and gates

1. **Pin the toolchains.** Add `toolchain/VERSIONS` (GHC, cabal, Zig), CI install steps on ubuntu-24.04, and a `toolchain-probe` gate that asserts the exact versions. AC: gate in `VERIFY_ALL_DEPS`; CI green; nothing built with them yet.
2. **Interchange schema v0.** `docs/superpowers/specs/goo-program-schema.md` plus a JSON Schema file covering the 114 AST kinds, 36 type kinds, release-plan sites (slot, field, elem stride), synthetic declarations, and package exports. AC: a hand-written sample validates; schema carries a version field; every AST kind maps to exactly one object shape.
3. **C serializer.** `bin/goo --emit-program` writes the schema after type checking and plan building, before codegen. AC: runs on all 652 fixtures without crashing; output validates; two runs are byte-identical; `program-dump-probe` gate.
4. **Differential harness with teeth.** `scripts/frontend_diff.sh <producer-a> <producer-b>` diffs canonical dumps per fixture and reports counts; self-test proves it can report a difference. AC: self-test row in `verify-core`; count file like `tools/parity-gate-count.txt`.
5. **Message catalogue.** Extract every `type_checker_error` format string into `catalogue/diagnostics.tsv`; the C checker reads from it. AC: `grep` finds no literal diagnostic string left in `src/types/*.c`; reject suite 157/157.

## Phase 1: untangle codegen from the checker (C only)

6. **Monomorphization moves to the front end.** The checker instantiates generic bodies and emits them as ordinary declarations; `src/codegen/monomorphize.c` and the `push/pop_type_params` calls go. AC: IR differential empty against a pinned pre-change `bin/goo` on all goldens; `comptime-generic-compose-ir-pin` green.
7. **Synthetic declarations become tree nodes.** Shim method adapters and bound thunks are declared by the checker as explicit nodes; the four `type_checker_declare_synthetic` calls in codegen go. AC: IR differential empty; `io_writer_probe`, `methods-probe` green.
8. **Codegen reads only the tree.** Replace the 64 `get_builtin`, 23 `lookup_variable`, 6 `lookup_method`, 6 `error_type`, 2 `pkg_dispatch_name` sites with reads from resolved nodes and the export table. AC: `grep -c 'type_checker_\|checker->' src/codegen/*.c` is 0; IR differential empty; serializer output unchanged.

## Phase 2: Zig runtime behind the C ABI

The runtime archive is a C ABI boundary, so it moves before codegen.

9. **Zig workspace.** `zig/build.zig` producing `lib/libgoo_runtime.a` from a stub plus the existing C files compiled by Zig's C compiler. AC: `zig-build-probe` gate; goldens green when linked against the Zig-built archive; NNG built by Zig from the vendored tarball, cmake and the `ar -D` MRI script deleted.
10. **Scheduler spike, then ADR 0007.** Prototype goroutines on Zig 0.16 `std.Io` and on a hand-written context switch; measure both against `go-probe`, `chan-mt-stress`, `mt-scheduler-stress`, `select-probe`, `parallel-soak-probe` on Linux and macOS arm64. AC: ADR records the choice with numbers; `ucontext` is not the answer on either platform.
11. **Port the runtime module by module.** One commit each: allocator and object header with ARC; strings; slices and maps; channels and the scheduler (from 10); sync; time; io and os (self-exe path via `std.fs.selfExePath`, deleting the two `/proc/self/exe` sites' runtime half); testing; defer; arena; far transport. AC per commit: that module's gates (`obj-header-tsan`, `alloc-doors-probe`, `arena-valgrind-probe` or its sanitizer equivalent, the `far-*` set) green; full `verify-core` green.
12. **Flip the runtime.** Delete `src/runtime/*.c`; macOS arm64 joins CI. AC: `verify-core` green on both runners; `repro-build-probe` reimplemented over `zig build` and green; `archive-determinism-probe` green without GNU ar.

## Phase 3: Zig codegen and driver

13. **Zig codegen skeleton.** `goo-be` reads the program dump and emits `.ll` for hello world through the LLVM C API. AC: `compiler_differential.sh` runs C-vs-Zig on one fixture; `zig-codegen-probe` gate.
14. **Codegen parity, one commit per module.** Mirror the C split: type mapping; functions and calls; statements; expressions; composites; error unions; nullables; interfaces and method dispatch; runtime integration; ARC release emission; -O pipelines. AC per commit: IR differential empty for the fixture subset the module covers, count recorded in a baseline file; final commit 495/495 at -O0 and -O2, reject suite unchanged.
15. **Zig driver.** `goo` CLI in Zig: `build`, `run`, `test`, `version`, all flags byte-identical in `--help`; calls the C front end as a subprocess emitting the program dump. AC: `exit-code-probe`, `subcommand-probe`, `cwd-link-probe`, `outoftree-probe`, `goo-test-probe`, `release-package-probe` green on both runners.
16. **Flip the back end.** Delete `src/codegen/`, the LLVM link from the C build, and the C driver; `GOO_BE` kill switch added. AC: `verify-core` green; `bin/goo` is the Zig driver; the C tree contains only the front end.

## Phase 4: Haskell front end, gate-only until whole

17. **Haskell workspace and lexer.** `hs/` cabal project; `goo-fe --emit-tokens` matching the C lexer's dump. AC: token differential empty on all 652 fixtures; `lexer-diff-probe` gate.
18. **Parser.** Hand-written recursive descent; the goo-grammar skill's LALR workaround sites become its test list. AC: AST differential empty on 652 fixtures; the 5 parse-error rejects produce identical stderr substrings.
19. **Type checker, in slices.** One commit each: declarations and scopes; expressions; statements; packages, shims and seeded types; method sets, embedding and interfaces; error unions and nullables; generics and monomorphization. AC per commit: typed-program differential empty on the fixtures the slice covers; that slice's reject messages match the catalogue.
20. **Escape analysis rows become data.** Convert the parameter (20), block (31), local (14) and release-decision rows to a fixture file both implementations read. AC: C tests pass from the file; row count unchanged; `teeth` scripts still detect a mutated arm.
21. **Escape analyses and release plan in Haskell.** Port with `Type__name` summary keys from the start (the registry collision fixed in C first, as its own PR, so the plan differential is meaningful). AC: rows green; plan differential empty; `arc_reason_census.sh` totals identical on both.
22. **Comptime engine.** AC: `comptime-probe` exit 55, `comptime-block-probe`, the reject matrix, all via the Haskell path in gate-only mode.
23. **Flip the front end.** Driver calls `goo-fe`; delete `src/lexer`, `src/parser`, `src/types`, `src/comptime`, `src/ast`, Bison, the tripwire; `GOO_FE` kill switch. AC: `verify-core` green on both runners; `src/` is empty.

## Phase 5: retire the C-era infrastructure

24. **Retire CompCert and MISRA.** Remove `v2-bootstrap-pilot`, `ccomp_shim.h`, `docs/misra`, `COMPCERT_AUDIT.md`. AC: `HEAVY_DEPS` loses the pilot; docs say why.
25. **Halt Bazel.** Delete `BUILD`, `MODULE.bazel*`, `tools/parity*.sh`, `bazel.yml`; ADR records phase 3b as the stopping point. AC: no Bazel file in tree; CI has two workflows.
26. **Make to zig build.** Move the gate net into `zig build verify-core` one stanza group at a time, parity script pattern reused with a count baseline. AC: `zig build verify-core` runs 217 gates; Makefile deleted in the final commit.
27. **Docs and ADRs.** ADR 0007 implementation language, ADR 0008 interchange format; CLAUDE.md, README, roadmap updated; the two kill switches removed. AC: `doc-claims-probe` extended to cover the new build commands.

## Risks named up front

- LLVM C API from Zig 0.16 against LLVM 22 or 23: prove in task 13 before task 14 starts.
- Zig 0.16 `std.Io` maturity for a preemptive-looking scheduler: task 10 exists to fail early.
- GHC binary size against ADR 0006's distribution story: measure at task 17 and record.
- Position parity between two lexers: make it part of the token differential, never normalized away.
- The escape analysis port is the only place a soundness regression can hide behind a green IR differential; tasks 20 and 21 exist so the rows, not the goldens, are its oracle.
