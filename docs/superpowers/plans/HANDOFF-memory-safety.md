# Handoff: 2026-08-30 — decomposition, boundary gates, memory safety

## Goal

Divide the Bazel targets down to the real call graph, lock the new boundaries
so they cannot erode, and make memory safety a gate rather than a measurement.

## State

| PR | Branch | Base | Status |
|---|---|---|---|
| #326 | `build/bazel-decompose-types` | phase4-probes | **merged** |
| #327 | `build/bazel-boundaries-followup` | phase4-probes | **merged** |
| #328 | `build/bazel-visibility-sweep` | phase4-probes | **merged**, 9/9 green |
| #329 | `build/codegen-link-flags` | phase4-probes | **open**, the linker flags |
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
- [x] **`--linker` and `--link-flag`** (#329). codegen forked a fixed `gcc`
      with no flags, so an instrumented archive could never link. Measured:
      `--linker=clang --link-flag=-fsanitize=thread` yields 160 defined
      `__tsan_*` symbols against 0 without it, 98 KB to 1.65 MB.
      `scripts/link_flags_probe.sh`, seven cases, four of them negative.

## In Progress

- [ ] #329 CI. Read the verdict with
      `gh pr view 329 --json statusCheckRollup`. Do NOT wait on
      `.conclusion == null`: an in-progress check reports an EMPTY STRING, so
      that condition exits at once and reports "complete" over running checks.
      Gate on `.status != "COMPLETED"`.

## Blockers

- **tsan still covers nothing that matters, but the LINKER half is closed.**
  #329 gave codegen `--linker` and `--link-flag`, and proved clang plus
  `-fsanitize=thread` links and runs. Two edits remain, and neither is done:
  make `NO_SANITIZE_COPTS` conditional on the sanitizer configs with a
  `select()` in `src/runtime/BUILD`, and thread the two flags through the probe
  runner so the 76 probe tests link. Expect the flip to surface real reports in
  `concurrency.c`, `deadlock.c` and `sync.c` — that part is unbounded until it
  is tried, which is why #329 stopped short of it.

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
- `scripts/link_flags_probe.sh` — the linker-flag gate and its `--self-test`.
  BOTH are in `VERIFY_ALL_DEPS`; `probe-teeth-probe` only checks that a
  self-test EXISTS, never that it runs.
- `tools/parity-gate-count.txt` — 219. `tools/parity_test.sh` fails when the
  live count moves and this file does not, in the same commit.

## Next Steps

1. Read #329 CI, then merge #326-style: into `build/bazel-phase4-probes`, never
   into `main`.
2. Instrument `src/runtime`, now that the linker can resolve it. The `select()`
   plus the probe-runner flags, above.
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
