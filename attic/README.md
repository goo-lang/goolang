# attic/

Dead code preserved for reference — **nothing in this directory is built,
tested, or shipped**, and none of it is part of the v1 surface.

- `stdlib/` (6.4k lines) — an early aspirational Goo standard library. Its
  syntax predates the current grammar and does not parse with today's
  compiler. The real, tested stdlib surface is the C shim layer
  (`src/types/shim_signatures.c` + runtime) plus the vendored source
  packages under `goostd/` resolved via GOOROOT. Moved here in P5.6
  (docs/2026-07-08-v1-roadmap.md) rather than deleted so the intended API
  sketches remain browsable.

- `status-docs/` — pre-audit status documents (P5.9). These claimed "100%
  test pass", "systems programming ready", completed LLVM/interface/memory-
  safety integrations, etc. The 2026-07-08 v1 audit showed the claims false
  or describing frameworks that were never reachable from `bin/goo` (and
  were unlinked in P5.6). Kept verbatim as a record of what was claimed.

  Three subdirectories were added by the 2026-08-17 truth pass. Every file in
  them advertised a feature that does not reach `bin/goo`:

  - `status-docs/language/` (2 files) — `COMPILATION_STATUS.md` ticked
    REPL, code completion, VS Code extension, LSP, Debug Adapter Protocol,
    performance monitoring, time-travel debugging and hot reload as ✅
    working; all of them are unlinked. `README_GOO_LANGUAGE.md`'s headline
    examples do not compile: `if user? { ... user!.name }` is a parse error
    (the live syntax is `if let`), and `!Error` is an unknown type.
  - `status-docs/features/` (5 files) — `repl.md`, `hot_reload.md`,
    `time_travel_debugging.md`, `performance_monitoring.md`,
    `enhanced_error_reporting.md`, for the `src/ide` and `src/errors`
    modules quarantined in the same pass.
  - `status-docs/task-reports/` (11 files) — "COMPLETION REPORT" documents
    for IDE integration, the performance dashboard, LSP navigation, syntax
    highlighting, dependent types, contract programming and proof
    generation. None of these ships.
- `docs/` — the June-2026 aspirational design suite (architecture, safety
  system, performance guarantees, killer features, WebAssembly, stdlib
  guide, developer experience, ...). These describe **intended designs,
  not verified behavior**. The living documents are `docs/01-VISION.md`,
  `docs/02-LANGUAGE-SPECIFICATION.md` (divergences are recorded there with
  locking tests), the dated roadmaps/specs under `docs/`, and `README.md`.

If you resurrect anything from here, it must come with a parse/type/run
probe wired into `make verify-core` — that is the bar everything shipped
has to meet.
