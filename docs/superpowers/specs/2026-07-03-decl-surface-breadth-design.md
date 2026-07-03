# Go Declaration-Surface Breadth Cluster — Design

**Date:** 2026-07-03. **Status:** awaiting user review (brainstormed solo under standing auth while user AFK; every "Decision" below is vetoable).
**Prior state (main @583337c, all probed 2026-07-03):** `func sum(nums ...int)` = parse error; `var a, b int` = parse error; `for k, v := range m` (map) = clean checker rejection ("for-range supported only on slice/array/string types"); `func(){}` literal = parse error (closures explicitly OUT of this cluster — own design cycle next).

## Goal

Close three everyday-Go declaration/iteration gaps in one branch — `var a, b int`, variadic `...T` parameters (pack-only), and range-over-map — plus a stretch task for buffered `make(chan T, n)`. These are the highest-payoff single-branch items left before closures (the big rock, deliberately excluded).

## Sequencing decision

**Decision: breadth before closures** (user question timed out; recommended option adopted). Rationale: the three features are single-branch sized with well-understood semantics; closures need their own brainstorm (capture model, escape analysis, closure ABI) and multiple branches. Grammar risk ascends task by task: T1 smallest delta, T2 largest, T3 zero grammar.

**Bison discipline (binding, from the #92 BANG precedent):** baseline is 79 shift/reduce + 256 reduce/reduce. Per grammar task: record the exact delta; a small justified delta is acceptable ONLY with differential parse verification (existing goldens parse identically) and a written justification naming the conflict family; a silent or unexplained delta = STOP. `make clean` after every parser.y edit.

## Task 1: `var a, b int` (multi-name declarations)

- Grammar: `var_decl` productions (parser.y:569+) gain an identifier-list form. `VarDeclNode.names[]`/`name_count` already exist (the `a, b := f()` destructure uses them) — the AST and checker/codegen multi-name paths are largely in place; `type_check_var_decl` already loops names.
- Semantics: `var a, b int` zero-initializes both (existing single-name zero-value path per name). `var a, b int = 1, 2` (explicit value list): include ONLY if the existing value-list grammar composes without new productions; otherwise ship the no-initializer form and record the initializer-list form as follow-up.
- Reject probe: `var a, b int = 1` (arity mismatch; Go: "assignment mismatch") must reject cleanly.
- Golden probe: declare/assign/print pairs incl. two names of a struct type (zero-value structs), `go run`-verified.

## Task 2: Variadic `...T` parameters (pack-only)

**Decision: Go's slice-sugar model, not C varargs.** `func sum(nums ...int) int` — inside the body `nums` IS `[]int` (the existing slice type/ABI: >16-byte runtime structs cross by pointer, the m12 ABI rule); at each call site codegen packs the trailing arguments into a freshly built slice using the existing slice-literal construction path. C-style va_list was rejected: non-Go semantics, no fit with the runtime or checker.

- Grammar: `...` (ELLIPSIS) in the FINAL parameter position only. Non-final variadic (`func f(a ...int, b int)`) = clean rejection (Go-conformant, reject probe). Check whether the lexer already has an ELLIPSIS token (spread/range?) before adding one.
- Types: `Type.data.function.is_variadic` already exists (panic uses it; println/print are variadic via NULL-param special-case). Wire it: the last param's declared type is `[]T`; call-site arity check becomes ≥ (fixed params) with trailing args each compatible with `T` (the existing call-arg adaptation net — untyped constants adapt to `T`, #100/#101 machinery).
- Codegen: call site builds a `goo_slice_t` of the trailing args (stack backing where the existing slice-literal path allows; otherwise its heap path — follow the existing mechanism, do not invent). Zero trailing args → empty slice (len 0), must work (probe line).
- **Deferred (recorded):** spread calls `f(s...)` — different grammar position (postfix `...` on a call argument); `append(a, b...)` likewise. Pack-only covers the dominant declaration-side pattern.
- Guard: `fmt.Println`/`print`/`panic` keep their special-cased paths — the entire golden suite plus `println-badtype-probe` guards regression.
- Golden probe: fixed+variadic mix (`func f(prefix string, nums ...int)`), zero-arg call, one-arg, many-args, result flowing into calls; plus one narrow-type line (`func g(bs ...int8)` called with in-range literals) so the #102 range-check net is exercised at the pack site — and a REJECT shape for it (`g(300)` must reject `constant 300 overflows int8`). `go run`-verified.

## Task 3: Range-over-map

**Decision: runtime iterator, not key-snapshot.** New runtime surface `goo_map_iter_sv` (init) + `goo_map_iter_next_sv` (returns has-next; key/value out-params), walking the map's internal bucket order. Snapshot-of-keys was rejected: hidden O(n) allocation per loop and worse mutation-during-iteration semantics. No grammar change (range parse already accepts maps; the gate is the checker arm).

- Checker: map arm in the for-range check — `for k := range m` (k: string), `for k, v := range m` (v: the value type), `for _, v := range m`. The runtime is string-keyed today (`goo_map_*_sv`), so k is TYPE_STRING; when integer keys land later the checker arm reads the map's key type (write it that way now — read key type from the map Type, which happens to be string).
- Codegen: loop skeleton mirrors the slice-range form (init iterator on a stack slot; cond = iter_next; bind k/v allocas; body; branch back). Value binding uses the existing per-elem coercion conventions.
- **Documented deviation:** iteration order is DETERMINISTIC (internal order), not Go's randomized order. Recorded in the runtime header, the probe header, and the gaps memory — probes and user code must not depend on order; probes assert order-independent facts (sum, count, membership).
- Mutation during iteration: whatever the bucket walk gives — document actual behavior with a one-off (not a golden), record. Go's own guarantee is loose here (deleted entries not visited; added entries may or may not be).
- Golden probe: sum-of-values, key-membership count, `for k := range` form, `_, v` form, empty map (zero iterations), single-entry map. `go run`-verified (order-independent assertions only).

## Task 4 (STRETCH): buffered `make(chan T, n)`

Include ONLY if T1+T2 land with a clean/justified conflict budget and schedule allows; otherwise record. `make(chan T)` unbuffered and the `make_chan` builtin exist; this adds the capacity argument through the existing make(...) grammar arm (map/slice make already take a second arg — check whether chan reuses that production; if yes this may be checker/codegen-only). Probe: producer/consumer with capacity 2, no goroutine needed for non-blocking sends up to cap.

## Testing summary

- Golden: 188 → 191 (probe per task; 192 if stretch lands).
- Reject probes: 24 → 26+ (`multivar-arity-reject`, `variadic-nonfinal-reject`; more if shapes emerge).
- Full gates per task: make lexer (clean after parser.y), verify ALL GREEN, test 76/1, ccomp-link PASS, bison delta recorded per task.
- Go conformance: every golden line `go run`-verified; rejections match Go's accept/reject decision (wording per repo vocabulary).

## Out of scope (recorded)

Closures/func literals (next design cycle); spread calls `f(s...)` and `append(a, b...)`; map key types beyond string; randomized map iteration order; select binding forms (1j); `var a, b = 1, 2` type-inferred multi-init (unless it falls out of T1 for free — record either way).

## Execution

SDD on branch `feat/decl-surface-breadth`, Sonnet implementers, controller (Fable) main-loop reviews with direct probes — the #104-validated economy pattern.
