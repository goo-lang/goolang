# P6 M1 lanes — spike findings (the three open questions)

**Date:** 2026-07-11
**Status:** Spike complete. Feeds Tasks 2 (comptime-wall lift), 6 (view-capture
detection), 8 (race-probe shape) of the P6 M1 implementation plan.
**Method:** throwaway `/tmp/spike_*.goo` probes + source reading against
`bin/goo` built from branch `feat/p6-lanes-m1`. No `src/` file was modified.

This document answers the three questions the design's *Risks & open questions*
section ordered BEFORE the task breakdown commits to approaches. Each answer is
backed by a file:line citation or reproduced command output. The three
DECISIONS are stated crisply at the end of each section.

All line anchors in the task brief were verified accurate, with two path
clarifications: the brief's `monomorphize.c` is `src/codegen/monomorphize.c`
and its `ast.h` is `include/ast.h`. `type_checker.c:1018-1020`,
`goo.c:949`, `expression_checker.c:992-1043`, `ast.h:718-740`,
`monomorphize.c:894`, and `Makefile:1798-1799` all resolve as described.

---

## Question 1 — Size the package-comptime wall lift

### 1.0 The reject reproduces exactly

Fabricated a throwaway GOOROOT (the `comptime-value-reject-matrix` trick,
Makefile:1798-1799) with a package `cpkg` exporting a comptime-param function,
and a `main` calling it:

```
$ mkdir -p /tmp/spike_gooroot/goostd/cpkg
$ printf 'package cpkg\nfunc Fill(comptime n int, s int) int { return s }\n' \
    > /tmp/spike_gooroot/goostd/cpkg/cpkg.go
$ printf 'package main\nimport "cpkg"\nfunc main() { _ = cpkg.Fill(2, 1) }\n' \
    > /tmp/spike_pkg.goo
$ GOOROOT=/tmp/spike_gooroot ./bin/goo /tmp/spike_pkg.goo -o /tmp/spike_pkg_bin
Type error at cpkg:2:27: comptime parameters on package functions are not yet supported
Error: failed to compile package "cpkg"   (exit 1)
```

Note the reject fires at DECLARATION time (`cpkg:2:27`, while type-checking the
package), not at main's call site — matching the wall's documented design
(reject at declaration closes the dangling-seed hazard). Source of the message:
`src/types/type_checker.c:1018-1020`.

### 1(a) Where the template lookup breaks

Two independent breaks, both downstream of merely deleting the wall:

1. **The recorded seed's `fn` Variable carries no template decl.** A
   `pkg.Fill(...)` selector call type-checks against a FRESH export-copy
   Variable, built by `package_export_filter` (type_checker.c:201-205). That
   copy propagates only `name`, `type`, `declared_pos`, `is_initialized`,
   `mutability`, `is_builtin` — it does **not** copy `func_decl_node` (nor any
   comptime/generic template fields). Selector call-checking records that same
   export copy as the callee (`checked_callee = exp`,
   expression_checker.c:3634-3640; type resolution at 4300-4308). So a comptime
   seed recorded via `type_check_record_comptime_instantiation`
   (type_checker.c:586-597) would hold `fn->func_decl_node == NULL`. The
   consumer `comptime_instantiate` bails on exactly that:
   `ASTNode* decl_node = fn_var->func_decl_node; if (!decl_node || ...) return 1;`
   (monomorphize.c:787-788) — a silent no-op success, so **no instance is
   emitted** and the call's rewire to `Fill__n4` later fails a bare-name lookup.

2. **The true template Variable is freed before the worklist runs.**
   `codegen_monomorphize`'s comptime worklist (monomorphize.c:931-936) runs
   during *main's* codegen. But `compile_resolved_packages` tears the package
   scope down — `scope_pop(checker); checker->current_package = NULL;`
   (goo.c:1043-1044) — immediately after the package's own codegen, and
   `scope_pop` FREES that scope's Variables (the lifetime contract documented at
   goo.c:1036-1040 and the wall comment at type_checker.c:998-1007, front (b)).
   The package AST (`e->ast`) survives in the `PkgGraph`, but the fn *Variable*
   that would carry `func_decl_node` does not.

**Answer to "would `checker->instantiations` recording Just Work for a selector
call?": No, on both counts above.** The lift is not a one-line wall deletion.

### 1(b) Does the mangled name need a package qualifier?

**Yes.** `comptime_instantiate` mangles with the BARE function name:
`char* sym = codegen_mangle_comptime_instance(fn_var->name, values, n);`
(monomorphize.c:791), and `codegen_mangle_comptime_instance` appends only
`__n<value>` segments (monomorphize.c:210-220). Two packages each exporting
`Fill(comptime n int, …)` would both mangle to `Fill__n4` and collide in the
shared LLVM module. The wall comment names this as front (c)
(type_checker.c:1007-1009). The existing package-function symbol convention is
`goo_pkg__<pkg>__<base>` (function_codegen.c:1182, 1089), so the fix is to
thread that package prefix into the mangle `base` — yielding
`goo_pkg__lanes__Partition__n4` (the brief's sketch `lanes__Partition__n4` is
directionally right; the concrete symbol should follow the existing
`goo_pkg__` prefix so it lands in `GOO_OBJS`-reachable emission).

### 1(c) Touch points (with line anchors)

| Site | File:line | Change |
|---|---|---|
| The wall | `src/types/type_checker.c:1014-1025` | Relax: allow comptime params on VENDORED-SOURCE package funcs; keep rejecting runtime-arg-across-packages. |
| Export copy | `src/types/type_checker.c:194-210` (`package_export_filter`) | Carry `func_decl_node` + comptime/generic template fields into the export copy — OR record the seed against the real template Variable. |
| Package lifetime | `src/compiler/goo.c:1041-1044` (`compile_resolved_packages`) | The template FuncDecl + package-level symbols its body references must survive `scope_pop`/`current_package=NULL` until main's `codegen_monomorphize` runs. |
| Seed recording | `src/types/expression_checker.c:3630-3642`, `:4300-4308` | Record the comptime seed against a callee Variable that carries the template (currently the export copy that does not). |
| Comptime seed consumer | `src/codegen/monomorphize.c:787-788, 931-936` | Where the `func_decl_node==NULL` no-op currently swallows the seed. |
| Mangle base | `src/codegen/monomorphize.c:210-220, 791` | Package-qualify `base` before `__n<value>` appends. |
| Instance emission | `src/codegen/monomorphize.c:880` (`codegen_generate_comptime_function_instance`) | Emit under the `goo_pkg__<pkg>__<base>__n<v>` symbol; package funcs already use `goo_pkg__` (function_codegen.c:1182). |
| Reject-matrix | `Makefile:1824-1825` | Replace the `package-declaration` case (per the design's "matrix case is replaced, not dropped"): runtime-arg-across-packages still rejects; comptime-const-arg into a vendored package now COMPILES. |

### DECISION 1

Lifting the wall for `goostd/lanes` is a multi-site change, NOT a wall
deletion. Task 2 must, at minimum: (i) preserve the package template's
`func_decl_node` past the package's own codegen (extend `package_export_filter`
to carry it, or record seeds against the surviving template Variable and keep
that Variable alive), and (ii) package-qualify the comptime instance mangle
(`goo_pkg__lanes__Partition__n4`) to dodge same-name collisions. The matrix's
`package-declaration` case is replaced by a vendored-source COMPILE case plus a
retained runtime-arg-across-packages REJECT case.

---

## Question 2 — Obligation-3/4 detection representation

### 2.0 `captured_names` IS populated by the time an escape/ownership pass runs

`FuncLitNode.captured_names` (ast.h:718-740) is filled by the type checker's
`type_checker_record_capture` (expression_checker.c:992-1043) as it walks each
literal body during type-checking — including TRANSITIVE relay into every
enclosing literal on the `literal_stack` (expression_checker.c:1028-1041). The
existing wired escape analyses run AFTER type-checking completes: `param_escape_
analyze` / `block_escape_analyze` are invoked at the TOP of
`codegen_generate_program` (codegen.c:355-356), and `param_escape.c`'s Sink #3
already CONSUMES `captured_names` as the authoritative, populated source —
"We read captured_names[] as populated by the type checker; we deliberately do
NOT re-walk the closure body" (param_escape.c:489-492, loop at 495-499).

**So a `lane_ownership.c` pass placed at the same point (end of type-check /
top of codegen) sees fully-populated `captured_names` for nested literals** —
the proposed representation is sound on timing. Confirmed for nested literals
specifically because the transitive relay (expression_checker.c:1028-1041)
propagates a deep capture out through every enclosing literal before pass 2
ends.

### 2.1 How a `pkg.Fn(...)` selector's package is identified

The exact mechanism (cited for Task 6, which must recognise `lanes.Partition`
/ `lanes.Run` calls): resolve the selector's left identifier to its
TYPE_PACKAGE marker Variable, then confirm the member in that package's
`exports` scope.

- Type resolution: `type_checker_lookup_variable(checker, pkg_ident->name)` →
  `pkg_marker->package` → `scope_lookup_variable(pkg_marker->package->exports,
  selector->selector)` — **expression_checker.c:4300-4307**.
- Call-time identity check (same lookup, used to enable arg checking):
  **expression_checker.c:3630-3642** (`st->kind == TYPE_PACKAGE` branch;
  `exp->type == func_type` identity gate).
- The marker itself is seeded by `type_checker_seed_package_marker`
  (goo.c:1034), carrying the `Package*`.

So a lane pass identifies "this call is `lanes.Partition`" by: callee is an
`AST_SELECTOR_EXPR` whose `expr` is an `AST_IDENTIFIER` resolving to a
TYPE_PACKAGE marker whose `package->import_path`/`name` is `lanes`, and whose
`selector` is `Partition`/`Run`.

### DECISION 2

Adopt the proposed representation: `lane_ownership.c` is a self-contained
per-function AST walk, run at the type-check/codegen boundary (same point as
`param_escape`), that (i) marks variables bound to a `lanes.Partition(...)`
result as partition-origin, (ii) at each `AST_GO_STMT` flags any
partition-origin / `*Lane`-derived name appearing in the call args OR the
callee literal's `captured_names`, and (iii) at `lanes.Run(...)` inspects the
body-literal's `captured_names` for partition-origin names. **Detection
contract:** the fact carried across the `go`/`Run` boundary is the *identifier
name* of a partition-origin or `*Lane`-derived value, matched against
`FuncLitNode.captured_names` (already populated, already the mechanism Sink #3
uses) plus direct call-arg identifiers; package identity of a selector call is
resolved via the TYPE_PACKAGE marker → `exports` lookup
(expression_checker.c:4300-4307 / 3630-3642). No new per-element index
representation is needed — view identity is carried at variable-name
granularity, consistent with the design's obligation-4 "capture/escape rule,
not index-range analysis" (design Component 4, obligation 4).

---

## Question 3 — Helgrind vs the goroutine runtime

### 3.0 Helgrind completes, but is BLIND to goroutine races

Compiled `examples/spmd_fanout_probe.goo` and ran it under helgrind
(valgrind-3.27.1, present — `arena-valgrind-probe` is live in verify-core):

```
$ ./bin/goo examples/spmd_fanout_probe.goo -o /tmp/spmd_probe_bin   # exit 0
$ valgrind --tool=helgrind --error-exitcode=99 /tmp/spmd_probe_bin
55
... Warning: client switching stacks?  SP change: ...   (bounded; "further instances will not be shown")
ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 2071 from 14)   # exit 0
```

Helgrind runs to completion, output is correct, and it does NOT drown in false
positives — the M8 scheduler's ucontext stack-switching produces a handful of
bounded `client switching stacks?` WARNINGS (not errors), then self-silences.
So option (b)'s premise (noise) does not hold.

**The decisive probe: does helgrind DETECT a real race?** Injected a blatant
data race — two goroutines each incrementing a shared `*int64` one million
times with no synchronization:

```
$ ./bin/goo /tmp/spike_race.goo -o /tmp/spike_race_bin   # exit 0 (no ownership check yet — the M1 target)
$ valgrind --tool=helgrind --error-exitcode=99 /tmp/spike_race_bin
3000000                                                  # NOT 4000000 → lost updates → race is REAL
ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 1621 from 14)   # exit 0 — helgrind saw NOTHING
$ GOMAXPROCS=8 valgrind --tool=helgrind ... /tmp/spike_race_bin   # 3000000, ERROR SUMMARY: 0 errors
$ GOMAXPROCS=1 valgrind --tool=helgrind ... /tmp/spike_race_bin   # 3000000, ERROR SUMMARY: 0 errors
```

The output `3000000` (not the race-free `4000000`) proves the race genuinely
occurs — updates were lost — yet helgrind reports **0 errors at every
GOMAXPROCS**. This is a false NEGATIVE, not noise.

**Root cause.** The scheduler is an M:N ucontext design: goroutines are swapped
via `swapcontext` and "ucontext keeps the same OS thread across the switch"
(concurrency.c:41-42); GOMAXPROCS worker pthreads multiplex many goroutines
(concurrency.c:60-89). Helgrind's unit of concurrency is the pthread. Two
goroutines cooperatively scheduled onto one worker are, to helgrind, ONE thread
— same-thread accesses are never races — so it structurally cannot see
goroutine-vs-goroutine races. This is inherent to any pthread-based detector
(helgrind, DRD) against ucontext user-threads; suppressions cannot fix
blindness, only silence noise.

### DECISION 3

**Verdict (c): manual-runbook fallback.** Helgrind is NOT viable as a
verify-core soundness gate — not as-is and not with a `--suppressions` file —
because it is BLIND (false negatives) to goroutine-vs-goroutine races under the
ucontext M:N scheduler, not merely noisy. Task 8 must therefore NOT wire a
`stencil-race-probe` helgrind gate into verify-core. Instead, the soundness
test for the compile-time proof (design probe 4) becomes:

- **Primary:** the compile-time reject-probes (design probes 1) are themselves
  the proof that races cannot be written — these stay the primary gate.
- **Empirical backstop (documented runbook, not a race-detector gate):** a
  deterministic differential stress test — the `stencil_probe` output asserted
  bit-identical to the serial reference across many repeated runs and at
  -O0/-O2 (design probe 2). A lost update from an unsound proof would perturb
  the bits.
- A future TSan-via-codegen path (LLVM `-fsanitize=thread` with runtime
  happens-before annotations teaching TSan about ucontext switches) is the only
  way a dynamic detector could work here; it is out of M1 scope and recorded as
  the post-v1 option.

This matches the design's own documented fallback ("If unavailable, the
race-probe becomes a documented manual runbook … rather than a verify-core
gate", Risks section).

---

## Consistency with the plan

None of the three findings contradicts the plan's approach; each sharpens a
task:
- **Task 2** — the wall lift is multi-site (Q1), as the design's prerequisite
  note anticipated; no approach change, but the touch-point table is now
  concrete.
- **Task 6** — the proposed name-granularity capture detection is validated
  (Q2); `captured_names` timing and package-identity mechanism confirmed.
- **Task 8** — the race-probe SHAPE changes from "helgrind verify-core gate" to
  "compile-time rejects + deterministic differential backstop" (Q3). This is
  the one finding that alters a task's shape, and it lands squarely inside the
  design's pre-authorised fallback, so no plan revision is required.
