# M10 audit (2026-05-15)

Empirical scoping pass for M10. There is no design doc that defines M10's contract — the seed lists `M10` as pending with a single dep on M9 (now done), no description, no children. This audit canvasses the codebase for gaps a *user* would hit, groups them into candidate scopes, and recommends one.

## TL;DR

Three axes of pickup are visible. In order of user-impact:

1. **Language feature completion** — M8 shipped the *minimum form* of each deferred construct (zero-init + read), not the idiomatic form. Struct literals (`Point{x: 3, y: 4}`), slice literals in expression position (`[]int{1,2,3}` outside a `var`), positional struct literals (`Pair{1,2}`), and variadic calls all parse-fail or silently drop args.

2. **Stdlib breadth** — only **4** stdlib calls work (`fmt.Println`, `os.Exit`, `math.Sqrt`, `strings.Contains`). The other ~50 expected functions across `fmt`/`os`/`math`/`strings` type-error at `Package X has no member Y`.

3. **Multi-file compilation / real package system** — currently the "deliberate shortcut" per the seed comment. M7-pkg-mgmt is the existing strand. Big work, unblocks ecosystem.

**Recommendation: scope (1).** It's bounded (4–6 concrete children), it's the highest-impact-per-LOC gap (idiomatic Goolang code is currently unwritable without it), and it builds directly on the M8 children that already shipped. Stdlib expansion (2) is a useful side-axis but quadratic (codegen × runtime) and not a milestone-shaped chunk. Multi-file compilation (3) is huge (months of work) and wants its own roadmap pass, not a milestone-sized scope.

## Audit programs

Seven `bin/goo` invocations covering both stdlib and language-feature gaps.

| ID | Source | Status | Diagnosis |
|---|---|---|---|
| 1 | `fmt.Printf("hello %d\n", 42)` | Type error | `Package 'fmt' has no member 'Printf'` — only 4 fmt/os/math/strings calls wired (line 642-657 of `src/codegen/expression_codegen.c`) |
| 2 | `strings.ToUpper("hello")` | Type error | Same — `Package 'strings' has no member 'ToUpper'` |
| 3 | `fmt.Println(os.Args)` | Type error | Same — `Package 'os' has no member 'Args'` |
| 4 | `p := Point{x: 3, y: 4}` | Parse error | Struct literal not in grammar. Per `examples/baseline_probe.goo` comment: M8-struct shipped only `var p Point` zero-init + field read |
| 5 | `for i, v := range []int{1, 2, 3}` | Parse error | Slice literal in expression position not in grammar |
| 6 | `fmt.Println("a", "b", "c")` | OK, prints `a` | **Silent correctness bug** — variadic dropped to first arg only |
| 7 | `p := Pair{1, 2}` | Parse error | Positional struct literal not in grammar either |

Reproduction: each program is single-file under `/tmp/m10_probe{1..7}.goo`; the failures are deterministic and reproducible via `bin/goo -o /tmp/X /tmp/m10_probeN.goo`.

## Per-axis analysis

### Axis 1 — Language feature completion (RECOMMENDED)

What M8 actually shipped vs. what users expect:

| Construct | M8 child | Shipped form | Idiomatic form | Gap |
|---|---|---|---|---|
| Struct | M8-struct | `var p Point; p.x` | `Point{x: 3, y: 4}` and `Point{3, 4}` | Both literal forms |
| Slice literal | M8-slice-literal | `var s []int; len(s)` | `[]int{1, 2, 3}` in any expression | Expression-position grammar + codegen |
| Map literal | M8-map-literal | `var m map[string]int; m["k"]` | `map[string]int{"a": 1}` | Same shape as slice literal |
| Multi-return | M8-multi-return | tuple return + multi-LHS destructure | Likely complete — verify | Probably done |
| For-range | M8-for-range | `for i := range slice {}` | `for i, v := range coll` (two-var) | Probably needs i, v form |
| Variadic | (not filed) | First arg only | All args | New construct |

The M8 children's `done` status in coord is **technically correct** (the minimum form ships) but **functionally misleading** (idiomatic code still fails). M10 closes this gap.

Suggested children (4 of them, sized to ~50–150 LOC each):

- **`M10-struct-literal`** — parse + AST + type-check + codegen for both `T{f: v}` (keyed) and `T{v1, v2}` (positional). Touches `parser.y`, `ast.c`, `type_checker.c`, `expression_codegen.c`. Probably ~200 LOC across them.
- **`M10-collection-literal`** — `[]T{...}` and `map[K]V{...}` in expression position (not just `var` initializer). Most of the codegen exists for the `var`-init case; this is reaching the literal from a general expression context.
- **`M10-range-two-var`** — `for i, v := range coll` lowering. Probably small if one-var range works.
- **`M10-variadic-println`** — fix the silent drop in `fmt.Println`. Loop over args, emit per-type dispatch (the M9-fmt-println-int pattern), put a `' '` between them like Go. Touches `codegen_generate_println_call`. ~30 LOC.

Optional 5th: **`M10-string-format`** — `fmt.Printf` and `fmt.Sprintf` (the variadic + format-string version). Largest scope of the bunch; would be a M10 *or* defer to M10-followup.

**Gate (proposed):** `make verify` adds `m10-probe.goo` exercising all 4 children together — struct literal in a map, indexed via range with two-var, printed via variadic Println. Single example, single gate.

### Axis 2 — Stdlib breadth (SIDE AXIS)

The dispatch pattern at `src/codegen/expression_codegen.c:642–657` hard-codes 4 stdlib functions:

```c
if (strcmp(pkg->name, "fmt") == 0 && strcmp(sel->selector, "Println") == 0) ...
if (strcmp(pkg->name, "os") == 0 && strcmp(sel->selector, "Exit") == 0) ...
if (strcmp(pkg->name, "math") == 0 && strcmp(sel->selector, "Sqrt") == 0) ...
if (strcmp(pkg->name, "strings") == 0 && strcmp(sel->selector, "Contains") == 0) ...
```

Each call adds a row × `goo_X_Y` runtime function pair. Quadratic-feeling but each addition is ~30 LOC: 1 row above + 1 runtime fn + 1 `add_runtime_function` registration. Examples of high-value adds:

- `fmt.Printf`, `fmt.Sprintf`, `fmt.Errorf` (format-string driven; needs a varargs printf-style runtime)
- `strings.ToUpper`, `strings.ToLower`, `strings.Split`, `strings.Join`, `strings.TrimSpace`
- `os.Args`, `os.Getenv`, `os.ReadFile`, `os.WriteFile`
- `math.Pi`, `math.Pow`, `math.Abs`, `math.Min`, `math.Max`

This is real work (~5–10 functions per package × 4 packages = 20–40 additions) but feels more like *checklist M7-stdlib-expansion follow-on* than a milestone-shaped scope. Better as accretion under the existing `M7-stdlib-expansion` (currently in-progress) than as M10.

### Axis 3 — Multi-file compilation / package system (TOO BIG)

The seed comment is explicit:

> no multi-file compilation yet, so package method calls become direct runtime intrinsic emits.

Without multi-file: no third-party libraries, no community ecosystem, no `go get`-equivalent. This is `M7-pkg-mgmt` (currently pending) and likely takes months. Not a 1-2 week milestone. Scoping it would itself be a research task (compare with Go's package resolution, Rust's cargo, etc.).

## Recommended M10 decomposition

```
M10 (parent)
├── M10-audit               ← this document
├── M10-struct-literal      ← parse + AST + type + codegen for T{...} forms
├── M10-collection-literal  ← []T{...} + map[K]V{...} in expression position
├── M10-range-two-var       ← for i, v := range coll
├── M10-variadic-println    ← fix silent-drop in Println
└── M10-probe-gate          ← examples/m10_probe.goo + make m10-probe
```

With deps roughly:
- `M10-audit` blocks all children
- Children land in any order (independent grammars + dispatch)
- `M10-probe-gate` blocks on all 4 children

Total estimated work: 500–800 LOC across `parser.y`, `ast.c`, `type_checker.c`, `expression_codegen.c`, `function_codegen.c`, plus 1 new example file and 1 Makefile target. Comparable scope to M11-engine-recursion (which was ~70 LOC + cleanup) but with more parse-grammar work.

## Open questions

- **Q1.** Should `M10-variadic-println` extend to all `fmt.*` calls or just `Println`? Recommendation: just `Println` for M10; `Printf` (with format strings) is its own M10-followup or stdlib accretion.
- **Q2.** Should the audit promote `make m10-probe` into `make verify` immediately, or only after M10 closes (like comptime-probe was)? Recommendation: same pattern — keep it out of verify until the milestone gates green, then promote.
- **Q3.** The M8 children are marked `done` in coord but ship only minimum forms. Should we re-open M8-struct/M8-slice-literal/M8-map-literal and ship the full form there, or file M10-* as proper follow-ons? Recommendation: file as M10-* — coord doesn't have a "reopen" verb and the work is meaningfully different (literal-as-expression vs. var-init).

## Conclusion

M10 is **Idiomatic Goolang**: closing the gap between "compiles a curated example" and "compiles code a programmer would actually write." Four bounded children (~500–800 LOC total) plus an `m10-probe` gate. The audit's recommendation is to file the decomposition and proceed milestone-style.

Alternative scopings (stdlib breadth, multi-file compilation) exist but are either too small (better as accretion) or too big (better as a research-led roadmap pass) for the M10 shape.
