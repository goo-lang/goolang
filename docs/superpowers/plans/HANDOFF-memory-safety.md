# Handoff: 2026-08-30 — decomposition, boundary gates, memory safety

## Goal

Divide the Bazel targets down to the real call graph, lock the new boundaries
so they cannot erode, and make memory safety a gate rather than a measurement.

## State

| PR | Branch | Base | Status |
|---|---|---|---|
| #326 | `build/bazel-decompose-types` | phase4-probes | **merged** |
| #327 | `build/bazel-boundaries-followup` | phase4-probes | **merged** |
| #328 | `build/bazel-visibility-sweep` | phase4-probes | **open**, CI running |
| #325 | `build/bazel-phase4-probes` | `main` | open, carries all of the above |

`main` is untouched at `de24a23`. #325 stays the single route to `main`, with
phase 4 tasks 6 to 17 still ahead of it.

## Completed

- [x] Every package divided to its cycle boundary. `src/` goes from 12 targets
      to 45. No package is wholly inside a cycle at this time.
- [x] Boundary gates with teeth: `layering_check` (clang only), visibility, no
      opt-out, no public target outside `//include`. `tools/verify_layering.sh`.
- [x] Six unit suites for `src/runtime`, which had none: arena, defer, io,
      sync_shim, testing, time_shim. Four came from subagents and were reviewed.
- [x] **`io.c` memory-safety fix.** Every returned `goo_string_t` used plain
      `malloc`, so ARC releasing one freed a pointer 16 bytes before the block.
      All five sites use `goo_alloc` at this time.
- [x] `test-golden-poison` wired into `VERIFY_ALL_DEPS`. It was `.PHONY` and
      reached no verify target, so it never operated.
- [x] `poison_test`, which proves the detector detects.
- [x] asan, ubsan and tsan in CI, each proven red under its own config and
      green without it. `tools/verify_sanitizers.sh`, `docs/SANITIZERS.md`.

## In Progress

- [ ] #328 CI. Six checks; read the verdict with
      `gh pr view 328 --json statusCheckRollup`. Do NOT wait on
      `.conclusion == null`: an in-progress check reports an EMPTY STRING, so
      that condition exits at once and reports "complete" over running checks.
      Gate on `.status != "COMPLETED"`.

## Blockers

- **tsan covers nothing that matters yet.** `src/runtime` opts out of
  instrumentation, because `src/codegen/codegen.c:1809` hardcodes `gcc` to link
  a compiled Goo program and passes no sanitizer flag. Teach codegen to pass
  link flags through, and tsan reaches the goroutine scheduler.

## Open Questions

- Do the `os.ReadFile` and `os.ReadLine` shim rows move to
  `non_retaining = 1`? The hazard is gone, so reclamation is now safe to
  consider. `shim_signatures.c` demands a proof against the runtime body first.
- Does a leak floor come before `detect_leaks` goes on? An asan-built compiler
  leaks 1928 bytes in 38 allocations on `hello_world.goo`, all of it goolang's
  own.

## Key Files

- `src/runtime/io.c` — `goo_alloc` at all five sites. `//src/runtime:io` gained
  an edge to `:core` and is no longer a leaf.
- `src/runtime/BUILD` — `NO_SANITIZE_COPTS` on 8 targets. Load-bearing.
- `.bazelrc` — `san-base`, `asan`, `ubsan`, `tsan`, `layering` configs.
- `tests/unit/goo_check.h` — `fflush` on every row header and FAIL line.
- `Makefile:3392` — `test-golden-poison` in `VERIFY_ALL_DEPS`.

## Next Steps

1. Read #328 CI, then merge #326-style: into `build/bazel-phase4-probes`, never
   into `main`.
2. Codegen link-flag passthrough, which unblocks tsan on the runtime.
3. A coverage floor. 58.1% branch and 56.5% MC/DC are held by no gate.
4. Phase 4 tasks 6 to 17, per `HANDOFF-bazel-migration.md`.

## Context to Remember

- **Bazel granularity does NOT make builds faster.** Measured: 2 to 3 actions
  either way, because Bazel already compiles each `.c` separately inside a
  coarse target. The decomposition buys testability and enforced boundaries.
- **`--per_file_copt` was abandoned after measurement.** Its regex matches a
  string this build does not document. Three plausible patterns instrumented
  `src/runtime` with no message.
- **Three gates were found defined but never wired**, across this session and
  the last: `m12-probe`, `grammar-tripwire.sh`, `test-golden-poison`. Check
  `VERIFY_ALL_DEPS` membership before trusting a target exists.
- **A script that compiles a source list by name is invisible to both build
  systems.** `scripts/ast_free_leak_probe.sh` broke on a new file, and only the
  pre-push hook caught it.
