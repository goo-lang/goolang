# Phase 3 sub-project C — fmt slices, nil-map panic, real -O, link hardening (P3.8-P3.11)

Date: 2026-07-10. Branch: `feat/p3-backend-c`. Prereqs: PRs #171, #172 merged.
Closes Phase 3 (exit gate: producer/consumer program green at BOTH -O0 and -O2).

## Recon facts (verified 2026-07-10)

- **Maps are NULL-tolerant everywhere**: no lazy-init exists anywhere — a nil map stays NULL; writes
  silently vanish at `goo_map_set_sv`'s guard (`runtime.c:754`). Reads/`len`/comma-ok/`delete`/range
  all zero-value/no-op on nil (Go-correct; `delete` on nil no-ops in Go too). Compound assign
  `m[k]+=1` routes get-then-set, so a set-side panic covers it (Go panics there too).
- **fmt lowering**: per-type recursion `codegen_emit_fmt_value` (`call_codegen.c:2276-2462`); slices/
  arrays hit the unsupported error at `:2451`. Struct arm (`:2387`) is the model. Slices are
  `{ptr,len}` aggregates. Sprintf's parallel value-string formatter (`:2469+`, struct arm
  `:2590-2613`) needs the same arm. No runtime slice-print helper exists.
- **-O is parsed then ignored**: `goo.c:158-164` validates 0-3 into `options->opt_level`, never read
  again. `codegen_optimize` is an uncalled TODO stub (`codegen.c:1714-1717`). Target machine
  hardcodes `LLVMCodeGenLevelDefault` (`codegen.c:1595`). LLVM 22 new-PM C API: `LLVMRunPasses`
  (llvm-c/Transforms/PassBuilder.h) — greenfield, no existing new-PM usage.
- **Link**: `system(link_command)` at `codegen.c:1695` inside `codegen_emit_executable`
  (fixed 2048-byte buffer, `gcc -no-pie -o <exe> <obj> <archive> -lm -lpthread` on Linux;
  `-no-pie` + archive-before-`-lm` ordering are load-bearing, comment `:1673-1679`).
  `options->link_libs` parsed (`goo.c:180-182`) and never passed to codegen — silently dropped.
  No fork/exec anywhere in the tree. On-failure `remove(object)` at `:1699` is pinned by
  link-cleanup-probe (Makefile:1385) and MUST survive the rewrite.

## Design decisions (Fable, 2026-07-10)

### C1 (P3.9) — nil-map write panic, runtime-side

Change `goo_map_set_sv`'s `if (!m) return;` to `goo_panic("assignment to entry in nil map")`
(Go's exact message). Runtime-side beats codegen null-checks: one line covers every write site
(direct + compound assign) with zero IR churn. Reads/len/comma-ok/delete/range guards untouched.
Probes: run fixture (nil-map read zero + len 0 + delete no-op + comma-ok false all still fine),
abort fixture (`var m map[string]int; m["x"] = 1` → exit 2, Go message). Alternative rejected:
codegen null-check per write site — N sites, IR churn, no benefit.

### C2 (P3.8) — fmt %v for slices/arrays, codegen-side

Add TYPE_SLICE and TYPE_ARRAY arms to `codegen_emit_fmt_value` (Println/Printf path) and the
Sprintf value-string formatter: emit `[`, an IR loop over the elements (dynamic len from the
{ptr,len} aggregate; static N for arrays) recursing into the element formatter with a space
separator between elements, then `]` — Go's `[1 2 3]` shape. Recursion gives nested slices,
[]string, slices-of-structs for free. []byte prints as Go does for %v ([104 105], NOT a string —
Go only strings []byte under %s). Narrow the `:2451` diagnostic accordingly. No runtime helper
(alternative rejected: a C-side goo_print_slice would need per-element-type dispatch at runtime —
codegen already knows the element type statically).

### C3 (P3.10) — real optimization passes

Implement `codegen_optimize` via `LLVMRunPasses(module, "default<O1|2|3>", tm, opts)`; call it
from the driver between `codegen_generate_program` and emit (goo.c ~:781-803) when opt_level>0;
map opt_level onto the target machine's `LLVMCodeGenOptLevel` too (`codegen.c:1595`). O0 = today's
path, byte-identical. Acceptance: -O2 IR measurably differs from -O0 on a composite program;
golden suite green at -O2 — the runner gains an optional `GOOFLAGS` env passthrough
(`run_golden.sh`, default empty = today's behavior) so `GOOFLAGS=-O2 ./scripts/run_golden.sh`
runs the whole suite optimized. That passthrough IS the Phase 3 exit-gate mechanism.

### C4 (P3.11) — link via fork/execvp argv, honor link_libs

Replace the `system()` link with fork + execvp using an argv vector (no shell: output paths with
spaces work; no fixed 2048-byte truncation). Preserve exactly: the Linux `-no-pie` and
archive-before-`-lm` ordering, the on-failure `remove(object)` (link-cleanup-probe), the
on-success object removal, and the GOO_RUNTIME override. Thread `options->link_libs` into
codegen (fields on CodeGenerator set by the driver before emit) and append `-l<lib>` args AFTER
the runtime archive (link order: user libs may depend on nothing of ours; runtime first matches
today's -lm placement rationale). Link failure: exit path returns error with the argv echoed
(join for the message only). Windows arm keeps system() (no execvp; out of v1 test surface) with
a comment. Probes: output path containing a space links and runs; a program calling a libm
function via `-lm` explicitly... (-lm already default) — use `--link m` equivalence or a trivial
`-l` case exercisable without new C deps: pass `-lm` via the flag and assert success, plus the
existing link-cleanup-probe stays green.

## Execution order & gates

C1 → C2 (one dispatch: runtime+fmt), then C3 → C4 (second dispatch: backend/driver). Per-commit:
make lexer + golden + reject + unit + tripwire (121/256 exact; zero grammar changes expected).
Sub-C exit: `GOOFLAGS=-O2` golden run 100% green + verify-core + fresh-context review wave.
Phase 3 exit-gate probe: producer/consumer program (goroutines, close, range-chan, defer-in-loop,
method value callback, non-const global) compiles and runs at -O0 AND -O2 — add as a golden
fixture `phase3_capstone_probe`.
