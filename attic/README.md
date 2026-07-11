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
- `docs/` — the June-2026 aspirational design suite (architecture, safety
  system, performance guarantees, killer features, WebAssembly, stdlib
  guide, developer experience, ...). These describe **intended designs,
  not verified behavior**. The living documents are `docs/01-VISION.md`,
  `docs/02-LANGUAGE-SPECIFICATION.md` (divergences are recorded there with
  locking tests), the dated roadmaps/specs under `docs/`, and `README.md`.

If you resurrect anything from here, it must come with a parse/type/run
probe wired into `make verify-core` — that is the bar everything shipped
has to meet.
