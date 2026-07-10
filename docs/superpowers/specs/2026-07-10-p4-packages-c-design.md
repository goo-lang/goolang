# Phase 4 sub-project C — time, os I/O, strings/strconv vendoring, smoke suite (P4.6, P4.8, P4.9, P4.10, P4.11)

Date: 2026-07-10. Branch: `feat/p4-stdlib-c`. Prereq: sub-A merged (#175), sub-B merged (#176).
No grammar work expected — tripwire 121/256 exact is a hard requirement for every commit.

## Recon facts (verified 2026-07-10)

- **Three stdlib mechanisms coexist**: (1) declarative shim table `SHIM_TABLE[]`
  (shim_signatures.c:34-79) for plain callables → runtime symbols; (2) method-aware bespoke
  shims — sync only — seeded in goo.c:468-594 (`sync_make_opaque_struct`/`sync_export_type`/
  `sync_export_method`), intercepted in call_codegen.c:1358-1373 pre-owner-routing, backed by
  src/runtime/sync_shim.c double-checked lazy init; (3) vendored source packages under goostd/
  compiled via the normal multi-package walk. `strings` is a hybrid: source exports first,
  shim rows as per-symbol fallback (expression_checker.c:3616-3657 ordering).
- **`stdlib/` is DEAD legacy** (`stdlib/time/time.goo`, `stdlib/os/os.goo` — obsolete colon/`->`
  dialect, never referenced by Makefile or resolver). goostd/ is the live tree. Do not use
  stdlib/ files as templates; API-surface reference at most.
- **time primitives exist but are internal**: `goo_platform_time_ns()` (monotonic) and
  `goo_platform_sleep_ns(uint64)` in platform.c:128-160, declared only in src/runtime/platform.h
  (not include/runtime.h) — not reachable from codegen today. No language-facing time surface
  exists at all.
- **`!T` shim returns have exactly one precedent**: strconv.Atoi via `SHIM_RET_ATOI_RESULT`
  (shim_signatures.h:44-56, shim_signatures.c:135-142 → `type_error_union(int64, string)`) +
  bespoke `codegen_generate_atoi_call` (call_codegen.c:1174). `shim_ret_type()` has no generic
  !T builder — each error-union shim is a new ret-kind + codegen arm.
- **os.\* today**: Exit/Getenv/WriteFile/ReadByte/FileSize table rows (shim_signatures.c:50-54),
  Args value member (expression_checker.c:4204); runtime io.c:22,44,59. No whole-file read, no
  stdin line read.
- **goostd/strings** vendors only HasPrefix/HasSuffix/TrimPrefix/TrimSuffix (34 lines);
  **goostd/strconv** only FormatBool (11 lines). All P4.9/P4.10 functions are new vendoring.
  goostd/utf8 has DecodeRune/DecodeRuneInString (needed by EqualFold).
- **Skip-list membership**: `is_stdlib_shim_import` (goo.c:448-466) skips fmt/os/math/errors/sync
  only. strings AND strconv are walked as source packages with shim fallback — the strconv
  hybrid path is presumed-working but unexercised; verify before vendoring onto it.
- **Golden/reject harness is data-driven**: examples/<name>.goo + .expected.txt (+ .exit,
  .stderr.txt, .env sidecars) — run_golden.sh:31-102; reject fixtures in tests/golden/reject/
  with mandatory .err.txt. New probe make targets append to `VERIFY_ALL_DEPS` (Makefile:2368-2528).
  Existing `make smoke-stdlib` (Makefile:2189-2204) covers 4 symbols — milestone-era, superseded
  by P4.11.
- **KNOWN LATENT (rider candidate)**: `goo.fmt.<T>` / `goo.typedesc.<T>` globals are bare-name
  keyed (interface_codegen.c:279-281, 381-384) — same aliasing class 8ed66c9 fixed for vtables/
  thunks (pinned by pkg_iface_samename_probe) but left unfixed here; two same-named types boxed +
  %v-formatted silently share one formatter/descriptor (first-boxer-wins). Reachable now that
  cross-package types are common.
- Vendoring source channel: `curl -sL https://raw.githubusercontent.com/golang/go/master/src/...`
  (WebFetch refuses verbatim reproduction; curl works — established PR #88 practice).

## Design decisions (Fable, 2026-07-10 — DRAFT, confirm at execution)

### C1 (P4.6) — time as a method-aware shim (sync template)

**Chosen**: clone the sync mechanism. Seed in goo.c: `Duration` = named int64 type; `Time` =
struct with one int64 nanos field; exports `Sleep(Duration)`, `Now() Time`, method
`Time.UnixNano() int64`; Duration constants `Nanosecond/Microsecond/Millisecond/Second` as
value members (math.Pi pattern). Codegen: intercept `owner_package->import_path == "time"`
before the generic owner-routed path (beside the sync arm). Runtime: new src/runtime/time_shim.c
exporting `goo_time_now_ns()`/`goo_time_sleep_ns(int64)` wrapping the platform primitives,
declared in include/runtime.h, added to RUNTIME_SRCS. UnixNano on a Time value reads the field —
can lower to a field load, no runtime call.

Alternatives: (a) flat table rows (`Now() int64`) — not Go-shaped; any real Go snippet using
`time.Now().UnixNano()` would fail; rejected. (b) vendored source package — source packages
cannot reach runtime/syscall symbols; rejected (same reason as sync).

NOTE `goo_platform_time_ns` is MONOTONIC (CLOCK_MONOTONIC) — fine for the elapsed-time
acceptance gate, but `UnixNano` semantically wants wall clock. Decide at execution: add a
wall-clock primitive (CLOCK_REALTIME) for Now and keep monotonic for internal use. Lean: yes,
one new platform function — UnixNano returning boot-relative ns would be a silent lie.

Acceptance (roadmap): import "time" resolves; Sleep maps to platform sleep; 50ms-sleep program
measures elapsed >= 50ms, exits 0. Probe: time_sleep_probe using Now/UnixNano delta assert >= 50ms
(golden .exit 0; stdout prints a boolean/ok line, not the raw delta — nondeterministic).

### C2 (P4.8) — os.ReadFile → !string, os.ReadLine stdin shim

**Chosen**: Atoi-pattern extension. New ret-kinds `SHIM_RET_READFILE_RESULT`
(`type_error_union(string, string)`) and `SHIM_RET_READLINE_RESULT` (same); table rows
`os.ReadFile(string) !string`, `os.ReadLine() !string` (error on EOF — the loop-termination
signal a cat-like program needs). Runtime: `goo_os_read_file(path)` (whole file, byte-length
honest — embedded NULs survive via the {ptr,len} string ABI) and `goo_os_read_line()` (stdin,
strips trailing \n, error at EOF) in src/runtime/io.c, mirroring whatever struct-return contract
codegen_generate_atoi_call consumes (verify the exact ok/value/err marshaling before writing).

Alternatives: (a) generic !T builder in shim_ret_type + one generic error-union codegen arm —
righter long-term, but touches the Atoi path as a refactor rider; assess at execution, take only
if the third ret-kind makes the duplication obviously dumb (three strikes). (b) (T, error)
two-value returns Go-style — Goo's roadmap explicitly says !string; rejected.

Acceptance (roadmap): missing-file error path tested (reject is WRONG here — it's a RUNTIME
error path: golden probe with catch); cat-like program round-trips os.WriteFile content;
ReadLine consumes piped stdin (golden .env can't pipe stdin — runner change or a dedicated
probe make target; decide at execution, lean probe target like the existing net of bespoke
probes).

### C3 (P4.9) — strings vendoring: Index, Repeat, ReplaceAll, Fields, Count, TrimLeft/Right, EqualFold

**Chosen**: vendor from upstream Go source via curl, VERBATIM where the function is pure
(Repeat's core loop, Count, EqualFold), and where upstream leans on internal/bytealg
(Index, Count fast paths) take upstream's OWN pure-Go fallback loop, marked with the
established deviation-comment convention (deBruijn precedent, PR #88 arc). EqualFold needs
utf8.DecodeRuneInString — already vendored in goostd/utf8; this makes strings→utf8 the FIRST
goostd→goostd import (import "unicode/utf8" alias-normalizes to the flat dir). VERIFY the
cross-goostd walk early — it is the riskiest unknown in C3; if it fails, that fix precedes
vendoring. ReplaceAll = Replace(s, old, new, -1) — vendor Replace, export ReplaceAll wrapper.
Fields: upstream uses asciiSpace table + unicode.IsSpace slow path; take the ASCII fast path +
utf8-decode slow path with IsSpace inlined over the six Go whitespace runes (unicode package
does not exist — documented deviation).

Acceptance (roadmap): each function compiles via the source-package path, passes a table-driven
e2e .goo golden test; shim fallbacks (Contains/ToUpper/Split/Join...) unchanged — pin with an
existing-symbol regression probe.

### C4 (P4.10) — strconv vendoring: FormatInt (base 10/16), ParseInt decimal → !int64, Quote

**Chosen**: same vendoring channel. FormatInt: upstream small-int fast path + formatBits digit
loop, bases 10/16 (general 2..36 comes free if formatBits vendors cleanly — take it, gate on
10/16). ParseInt: signature ADAPTED to `ParseInt(s string) !int64` (roadmap-specified; upstream
returns (int64, error) with base/bitSize params — decimal-only subset, overflow + syntax error
paths kept). This is the FIRST goostd source function returning !T — the parser is shared so
`!int64` in a .go file should parse, but VERIFY error-union return/catch through the
source-package path before writing the body; if broken, that compiler fix is in-scope for C4.
Quote: upstream depends on IsPrint tables (unicode) — vendor quoteWith's escape loop with a
simplified printability rule (ASCII printable + \xNN/\uNNNN escapes otherwise), documented
deviation. First: verify the strconv hybrid path (source dir + shim fallback rows Itoa/Atoi)
actually composes — presumed-working, unexercised (recon flag).

Acceptance (roadmap): FormatInt base 10/16, ParseInt decimal subset incl. error path, Quote —
all pass e2e run tests.

### C5 (P4.11) — stdlib e2e smoke suite + shim-table drift catch

**Chosen**: smoke programs as REGULAR golden fixtures (examples/smoke_<pkg>_<area>.goo — the
harness already compiles, runs, asserts stdout/exit) + the novel piece: a coverage cross-check
script `scripts/check_stdlib_coverage.sh` that extracts every SHIM_TABLE row from
shim_signatures.c, the seeded sync/time exports, value members (os.Args, math.Pi, time consts),
and goostd exported funcs, then requires each symbol to appear in at least one smoke fixture.
New make target `stdlib-smoke-coverage` appended to VERIFY_ALL_DEPS. Adding a shim row without
smoke coverage then FAILS verify-core — the drift catch the roadmap wants. Includes: Atoi error
path, !T catch with fmt, ?T nil checks (roadmap-listed). Replace dead-syntax
tests/test_try_catch.goo (catch |err|) with working catch e {} form. Retire/absorb
make smoke-stdlib into the new target (keep the target name as an alias if coord milestone
references it).

Alternatives: separate runner + fixture dir — duplicates run_golden.sh for zero gain; rejected.
One giant smoke program — a single failure masks the rest, no per-symbol attribution; rejected.

### C6 (rider, recommended) — qualify goo.fmt/goo.typedesc keys

Mirror 8ed66c9's vtable/thunk fix at interface_codegen.c:279-281/381-384: key emitted formatter/
descriptor globals `<owner>__<T>` when `type_receiver_owner_package` is non-NULL. Small,
same-class, and C5's smoke fixtures will box+%v package types — leaving it latent risks the
smoke suite either tripping it as a confusing failure or silently pinning wrong output. Extend
pkg_iface_samename_probe to cover %v of both same-named types. If it balloons, drop back to
tracked-follow-up.

### Order: C1 → C2 → C3 → C4 → C5 (roadmap order; C5 last since it covers everything;
C6 lands before C5 so the smoke fixtures pin the FIXED behavior). C3/C4 verify-first items
(cross-goostd import; !T-return through source path; strconv hybrid) front-load the unknowns.

## Gates

Per-commit: make lexer + golden (405/0 baseline, BOTH -O0 and -O2) + reject (76/0 baseline) +
unit 76/1 + tripwire 121/256 exact (no grammar changes — any delta is stop-the-line) +
verify-core. New fixtures raise counts. Vendored bodies: byte-diff against upstream source in
the commit message where verbatim, deviation comments where not.
Pre-PR: review wave scaled to risk — shim/codegen dimension (Opus: time/os codegen arms,
error-union marshaling, C6 interface_codegen) + Go-parity/vendoring-fidelity dimension (Sonnet:
vendored bodies vs upstream, error semantics, smoke-coverage honesty). Sub-C exit: all five
roadmap acceptance rows demonstrably green + Phase 4 exit-gate program (multi-package +
strings/strconv/os.ReadFile/time.Sleep/sync.WaitGroup) compiles and runs.
