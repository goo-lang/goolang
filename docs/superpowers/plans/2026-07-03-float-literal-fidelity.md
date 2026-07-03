# Float Literal Fidelity + Constant Truncation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the parser's float-literal text corruption — the top escalation from #102's whole-branch review — and close the `int(3.5)` truncation-class conformance gap. Reproduced on main @bf800b0: `1e70 > 1e69` computes **false** (Go: true); `0.30000000000000004 == 0.3` computes **true** (Go: false — the `%f` re-serialization keeps only 6 decimal places, corrupting ANY float needing more precision, not just huge ones); `int(3.5)` compiles to 3 (Go rejects: `constant 3.5 truncated to integer`) while `int(2.0)` is legal in both.

**Architecture:** (T1) Replace the parser action's `%f` re-serialization with shortest-round-trip formatting: try `%.15g`, `%.16g`, `%.17g` and keep the first whose `strtod` round-trips to the original double. This is action-code only — no grammar production changes, no token retyping, bison conflict counts must stay exactly 79 shift/reduce + 256 reduce/reduce. Fixes value fidelity for every representable literal AND de-uglifies diagnostics (`constant 1e+40 overflows float32` instead of 40 expanded digits). The lexer's `atof` saturation of over-range literals to inf (text becomes `"inf"`) remains and is already handled by the finiteness checks from #102. The deeper alternative — passing the raw source text through bison's `%union` — was ruled out for this branch: it retypes `FLOAT_LITERAL`'s semantic value, touching lexer bridge memory conventions and every consumer, for a cosmetic-only gain (source-exact vs value-exact text); recorded as a follow-up if source-exactness ever matters. (T2) Reject non-integral float constants in integer conversions at the existing conversion-check hook: `int(3.5)` → error; `int(2.0)` stays legal; runtime `int(x)` untouched.

**Tech Stack:** C23, LLVM-C, Bison (action code only). Parser action + checker.

## Global Constraints

- Branch: `fix/float-literal-fidelity` off main @bf800b0. Do NOT commit on main.
- Commits: conventional, imperative, `--no-gpg-sign`. Stage only named files; never stage `.superpowers/` or `.handoff.md`.
- **Bison guard (T1-critical):** `parser.y` edits are ACTION CODE ONLY. After `make lexer`, verify the conflict counts are unchanged: the build output (or `bison -v` report) must show exactly **79 shift/reduce, 256 reduce/reduce**. Any delta = STOP/BLOCKED. Run `make clean && make lexer` after touching parser.y (regenerated parser.tab.c + the no-header-deps Makefile).
- Gate per task: `make lexer` (with clean, per above), probes, then `eval "$(opam env --switch=default)"`, `make verify` (ALL PASS; golden 186/0 grows per probe: 187/188; all 21 reject probes stay green) and `make test` (76/1). STOP/BLOCKED on any regression. Float-sensitive guard goldens: `const_range_probe`, `const_expr_probe`, `cross_kind_probe`, `float_adapt_probe`, `float_binop_probe`, `composite_field_adapt_probe`, `nullable_adapt_probe`, `global_init_probe`, and every `const*-reject-probe`.
- Go conformance: verify probe expectations with `go run` (go1.26 on PATH), record comparisons.
- Probe hygiene: bool-compare floats, same-width prints. NOTE for T1's probe: constant-vs-constant comparisons of NON-representable decimals (`0.1+0.2 == 0.3` as constants) hit the documented stamp-and-compute deviation (Go folds exactly; we compute at double) — route such shapes through runtime variables where both languages agree, or use directly-representable comparisons.
- Reject-probe pattern: mirror `constconv-reject-probe` exactly, wired into `verify`.
- Pre-commit hook runs `make test`.

## Reference: verified code landmarks (2026-07-03, main @bf800b0)

- Float literal action: `src/parser/parser.y:2386-2389` — `char float_str[64]; snprintf(float_str, sizeof(float_str), "%f", $1); ast_literal_new(TOKEN_FLOAT, float_str, ...)`. `$1` is a C `double` (`%token <real> FLOAT_LITERAL`, :75).
- Lexer bridge: `src/parser/lexer_bridge.c:335-336` — `yylval.real = atof(token->literal)`; the raw source text exists here but is discarded (do NOT change this in T1 — the chosen fix works downstream of it).
- INT literal action (for contrast, untouched): `parser.y:2380-2383` (`%lld`, value-preserving for in-range).
- Conversion range check (T2's hook): `check_conversion_operand_range` in `src/types/expression_checker.c` (~:648, added by #102 T3b) — check-only, no stamping; handles literal ± enclosing unary minus with negation threading; `^`/binops excluded. T2 adds the truncation check beside the overflow check.
- `literal_fits_type` and the float parsing conventions (strtod, `float64_is_finite` bit-pattern): same file, ~:497-800.
- Repro probes (2026-07-03, main @bf800b0): `1e70 > 1e69` → false; `0.30000000000000004 == 0.3` → true; `1e307 > 1e306` → true (truncation happens to preserve this one — do not conclude large literals are safe); `int(3.5)` → 3; `int(2.0)` → 2. Go: true / false / true / reject `constant 3.5 truncated to integer` / 2.
- Known-not-broken (verified): `var u uint64 = 18446744073709551615` compiles and `u > 0` is true — but the INT pipeline clamps at LLONG_MAX per the #102-corrected comment, so `u`'s actual VALUE may be clamped. T1 Step 2 records what `u == 18446744073709551615` and `u / 2` actually produce (Go-compare); fixing the int pipeline is OUT OF SCOPE for this branch (record findings for the queue).

---

### Task 1: Shortest-round-trip float literal serialization

**Files:**
- Modify: `src/parser/parser.y:2386-2389` (action code only)
- Test: `examples/float_fidelity_probe.goo` + `.expected.txt`

**Interfaces:**
- Produces: `LiteralNode.value` text for TOKEN_FLOAT round-trips exactly to the lexed double via `strtod`. Everything downstream (checker range checks, codegen emission, diagnostics) consumes text as before — no signature changes.

- [ ] **Step 1: Probe** (`examples/float_fidelity_probe.goo`):
```go
package main

import "fmt"

func main() {
	fmt.Println(1e70 > 1e69)
	x := 1e307
	fmt.Println(x > 1e306)
	y := 0.30000000000000004
	z := 0.3
	fmt.Println(y == z)
	a := 0.1
	b := 0.2
	fmt.Println(a+b == 0.3)
	fmt.Println(a+b == 0.30000000000000004)
	fmt.Println(1.5e-300 > 0.0)
	fmt.Println(2.5 == 2.5)
}
```
`.expected.txt`: `true` `true` `false` `false` `true` `true` `true`.
Cover: huge-magnitude ordering (the headline bug), near-max round-trip, long-mantissa distinctness, the classic 0.1+0.2 through RUNTIME variables (both languages agree at runtime — `a+b == 0.3` false, `== 0.30000000000000004` true), subnormal-adjacent small value, and a short-decimal regression lock. Verify every line with `go run` (all runtime-variable shapes chosen so Go agrees) and record.
- [ ] **Step 2: Verify today** — record per line (line 1 false, line 3 true today; others record). ALSO record the uint64 side-probe (`var u uint64 = 18446744073709551615; fmt.Println(u == 18446744073709551615)` vs `go run`) for the queue — do NOT fix it.
- [ ] **Step 3: Implement** — replace the `%f` snprintf in the FLOAT_LITERAL action with shortest-round-trip formatting:
```c
| FLOAT_LITERAL {
    /* Shortest text that round-trips to the lexed double: try increasing
       precision until strtod(text) == value. %.17g always round-trips
       IEEE-754 double (so the loop terminates); shorter wins for
       diagnostics ("0.3", "1e+70", "3.5") and keeps LiteralNode.value
       VALUE-exact for the checker's range checks and codegen's strtod.
       Replaces the old "%f" (6 fractional digits, 64-char cap) which
       corrupted any literal needing more precision — 1e70 > 1e69
       computed false. Over-range literals still arrive here as inf
       (lexer atof saturation) and format as "inf"/"-inf"; the checker's
       finiteness rejection (float64_is_finite) owns that case. */
    char float_str[32];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(float_str, sizeof(float_str), "%.*g", prec, $1);
        if (strtod(float_str, NULL) == $1) break;
    }
    LiteralNode* lit = ast_literal_new(TOKEN_FLOAT, float_str, get_current_position());
    ...
```
Keep the rest of the action identical (read it fully first — the plan shows only the changed lines' shape; preserve whatever follows `ast_literal_new`). Note `%.17g` max length is ~24 chars — 32 is comfortable. Mind negative zero and inf: `strtod("inf")==inf` holds, so the loop exits at prec 15 for inf — fine.
- [ ] **Step 4: Bison guard** — `make clean && make lexer`; verify conflict counts in the bison output are exactly 79 shift/reduce + 256 reduce/reduce (grep the build log or run `bison -v` on the report). Any change = STOP (you edited more than action code).
- [ ] **Step 5: Diagnostics check** — recompile the reject shapes and record the NEW messages: `var f float32 = 1e40` should now read `constant 1e+40 overflows float32` (was 40 expanded digits); `var f float64 = 1e309` stays `constant inf overflows float64` (lexer saturation, documented). All 21 reject probes must still PASS (they grep stable substrings).
- [ ] **Step 6: Gate** — probe passes; full golden 187/0 (float-sensitive net especially — literal texts changed for every float in every probe, so codegen strtod paths are exercised branch-wide); test 76/1.
- [ ] **Step 7: Commit** — `git add src/parser/parser.y examples/float_fidelity_probe.goo examples/float_fidelity_probe.expected.txt` + "fix(parser): float literal text round-trips exactly (was %f-truncated)".

---

### Task 2: Reject non-integral float constants in integer conversions

**Files:**
- Modify: `src/types/expression_checker.c` (`check_conversion_operand_range` ~:648 gains the truncation check)
- Test: probe lines added to `examples/const_range_probe.goo` + `.expected.txt`; `examples/consttrunc_reject.goo` + Makefile target `consttrunc-reject-probe`
- Modify: `Makefile` (one reject target, wired into `verify`)

**Interfaces:**
- Consumes: `check_conversion_operand_range`'s existing literal±minus shape detection and negation threading (T1 changes literal TEXT quality, which this check reads via strtod — sequence after T1).
- Produces: `int-kind(FLOAT_LITERAL)` conversions reject unless the value is integral AND in range.

- [ ] **Step 1: Probe additions** — to `examples/const_range_probe.goo` (positive side):
```go
	i := int(2.0)
	j := int32(-4.0)
	fmt.Println(i, int(j))
```
Append `2 -4` to `.expected.txt` (`go run`-verify: both legal Go — integral values). Reject probe `examples/consttrunc_reject.goo`:
```go
package main

import "fmt"

func main() {
	a := int(3.5)
	fmt.Println(a)
}
```
Makefile `consttrunc-reject-probe` mirroring `constconv-reject-probe` (rm -f, rc≠0, no binary, anti-crash grep, diagnostic grep `truncated to integer`), wired into `verify` (→ 22 reject probes). Header: Go rejects too (`constant 3.5 truncated to integer`) — Go-CONFORMANT rejection.
- [ ] **Step 2: Verify today** — `int(3.5)` → 3, `int(2.0)` → 2, `int32(-4.0)` → record (RED evidence).
- [ ] **Step 3: Implement** — in `check_conversion_operand_range`, beside the existing overflow check: when the conversion target is integer-kind and the operand literal is TOKEN_FLOAT (± minus, same shape detection), `strtod` the text; if the value is finite and `value != trunc(value)` (use a truncation comparison that avoids `<math.h>` if the ccomp constraint bit — check how `float64_is_finite` avoided it; `(double)(long long)value == value` is safe for |value| < 2^63, and out-of-range magnitudes already fail the overflow check first — order the checks so overflow rejects before integrality is tested) → error `constant %s%s truncated to integer` mirroring Go's message (literal text, `-` prefix when negated). Integral-and-in-range floats proceed (existing behavior). Runtime conversions unaffected (shape detection already excludes non-literals).
- [ ] **Step 4: Cross-checks** — `int8(200.0)`: integral but overflows int8 → must reject with the OVERFLOW message (Go agrees: `constant 200 overflows int8`) — verify check ordering produces that, record. `float64(3.5)`, `float32(2.5)`: float targets never hit this check — verify unaffected.
- [ ] **Step 5: Gate** — positive lines + reject probe PASS; full golden 188/0; all 22 reject probes; test 76/1.
- [ ] **Step 6: Commit** — "feat(types): reject non-integral float constants in integer conversions (Go conformance)".

---

## Final gate

`make verify` → ALL GREEN (188/0 + 22 reject probes). `make test` → 76/1. ccomp: opam env standalone, `make ccomp-link` → PASS (T1 touched generated parser code — confirm ccomp still builds it).

## Self-review notes

- T1 deliberately stays downstream of the lexer (`atof` saturation and the raw-text discard are untouched): the chosen serialization is value-exact for every finite double the lexer delivers, which is the entire bug class probed. Source-text pass-through (bison token retyping) is the recorded deeper option; the "inf" diagnostic text is its only remaining artifact.
- T1's probe routes non-representable-decimal comparisons through runtime variables so Go and Goo agree — the constant-fold deviation (#101) is not re-litigated here.
- T2 orders overflow-before-integrality so `int8(200.0)` gets the overflow message (Go's behavior), and reuses the existing shape detection rather than a second scanner.
- Out of scope (recorded): INT-literal pipeline clamping at LLONG_MAX (uint64-max literals — T1 Step 2 records the evidence); `var i int = 2.0` implicit form (Goo stricter than Go via #100's float→int rejection — separate conformance decision); #101 backlog (mid-codegen re-typecheck, float-% rejection, cascade noise).
