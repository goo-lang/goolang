# M7 audit (2026-06-12)

Empirical disposition pass for the remaining M7 "Production Readiness" tasks. M7 was seeded from the 2025 PRD-era taskmaster list (tasks #27–40). One child shipped (M7-stdlib-expansion, 2026-05-14) and its closing lesson already flagged the rest of the track as premature: the language core was still landing baseline constructs at the time. Since then M8–M11 closed the construct gap; this audit re-checks each remaining task against what actually exists in the codebase and disposes of each one.

## TL;DR

Seven tasks open. Two are tractable as written and stay (**M7-modern-errors**, **M7-parser-cleanup**). Five are aspirational PRD scope — cancelled, with two right-sized refiles (**M7-c-ffi-spike**, **M7-multifile-audit**) capturing the real underlying needs. The near-term implementation work moves to a new **M12 Stdlib Breadth** track (see `M10_AUDIT.md` axis 2, now milestone-shaped after the user picked it as the first track).

## Per-task evidence and disposition

| Task | TM# | Claim | What the code says | Disposition |
|---|---|---|---|---|
| M7-rust-interop | 28 | "Automatic Rust code translation, gradual migration, Cargo integration" | Zero Rust-facing code anywhere in `src/`/`include/` (grep hits are incidental words). No FFI mechanism exists at all — but the compile pipeline already links C via `clang ... lib/libgoo_runtime.a` (`src/codegen/codegen.c:624`), so *C* FFI is the realistic substrate | **Cancel.** Refile `M7-c-ffi-spike`: probe what `extern "C"`-style declarations would take given the existing clang link step. Spike-only, no impl commitment |
| M7-multi-lang-interop | 32 | "Seamless interop with Rust/C/C++, automatic FFI generation, AI-powered code translation" | Depends on #28; same absence of substrate. "AI-powered translation" is not compiler work | **Cancel**, no refile — subsumed by whatever M7-c-ffi-spike concludes |
| M7-pkg-mgmt | 27 | "Automatic dependency resolution, feature detection, intelligent imports" | The package system is a hardcoded 4-entry strcmp dispatch (`src/codegen/call_codegen.c:61-82`, comment: "deliberate shortcut... no multi-file compilation yet"). There is nothing to manage packages *of* | **Cancel.** Refile `M7-multifile-audit`: scoping pass for real multi-file compilation (M10_AUDIT axis 3 called it "months of work" needing its own roadmap). Audit-only |
| M7-security-design | 30 | "Taint analysis, capability-based security, secure defaults" | Scaffolding exists — `src/security/` (5 files) + `src/types/taint_analysis.c` — but only in aux test targets (Makefile:489-505), never in the compiler build; `type_checker.c` includes the header and calls nothing. Same scaffolded-unreachable shape comptime had pre-M11. Carries a stale in-progress claim from a dead session | **Release stale claim, cancel.** Security annotations need a stable type system; when the time comes, the M11-audit pattern (audit → probe → MVP dispatch) applies to this scaffolding too |
| M7-security-analysis | 34 | "Vulnerability detection, side-channel analysis, CVSS reporting" | Depends on #30. Side-channel analysis for a language that gained struct literals last week | **Cancel**, no refile |
| M7-modern-errors | 39 | Replace fprintf with structured ErrorContext + error codes | Real and tractable: 29 files in `src/` still emit raw `fprintf(stderr, ...)` diagnostics | **Keep** (pending) |
| M7-parser-cleanup | 40 | Resolve parser warnings / R-R conflicts | Real: `bison` reports 68 shift/reduce + 156 reduce/reduce live. `M10_BASELINE_CONFLICTS_AUDIT.md` proved 152/156 R/R are a benign unary-vs-binary cascade, but that knowledge lives in a doc, not the grammar; conflict count obscures regressions (a new bad conflict hides among 224 known ones) | **Keep** (pending) |

## What replaces the cancelled scope

The actual near-term production-readiness gap is stdlib breadth: 4 of ~50 expected stdlib calls work (`fmt.Println`, `os.Exit`, `math.Sqrt`, `strings.Contains`). M10_AUDIT axis 2 sized it as accretion; with M8–M11 done it is now the highest-impact track and is seeded as **M12 Stdlib Breadth** with 10 children (strings-1, math-fns, math-pi, strings-2, os-getenv, os-args, os-files, fmt-printf, fmt-sprintf, probe-promotion) and a `m12-probe` gate destined for the `make verify` aggregate.

Also seeded: `M13-slice-literal-expr` — the last deferred M10 language gap (`[]int{1,2,3}` in expression position, deferred in `M10_GRAMMAR_DECISION.md`), unscoped placeholder.

## Lessons recorded with the cancellations

Each cancelled task gets a `coord release --cancelled` + rationale lesson so the negative result is preserved in the graph (house rule from the M10 grammar work: cancelled IDs document why the survivor approach won).
