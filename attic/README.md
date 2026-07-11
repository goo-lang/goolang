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

If you resurrect anything from here, it must come with a parse/type/run
probe wired into `make verify-core` — that is the bar everything shipped
has to meet.
