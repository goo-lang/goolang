# M10 GLR-parser probe (2026-05-15)

Empirical probe of strategy 1 from `docs/M10_COMPOSITE_LITERAL_SPIKE_2.md`:
does Bison's `%glr-parser` mode resolve the `IDENT . LBRACE` ambiguity
between struct-literals (`Point{x:3, y:4}`) and `if X { body }` cleanly?

## TL;DR

- **GLR does not work as an incremental swap-in.** Bison-generation
  succeeds with 220 conflicts (64 S/R + 156 R/R) and the compiler
  builds, but every non-trivial program parses to "syntax is ambiguous"
  at runtime — including programs with no struct literals, no `if X {…}`,
  and no arithmetic. The baseline 156 R/R conflicts that LALR resolves
  silently via default-shift become true runtime ambiguities under GLR.
  Plus the grammar has duplicate-derivation productions (`FUNC ident
  LPAREN RPAREN block` vs `FUNC ident func_signature block`) that LALR's
  state-machine collapsed but GLR yields parallel parses that no `%dprec`
  resolves.
- **Conflict count rises by 3 S/R only.** struct_lit adds 3 new
  S/R conflicts (IDENT . LBRACE shape) and no new R/R. The R/R count
  stays at 156. So the grammar-machine side of the equation is clean —
  it's the runtime-ambiguity side that bites.
- **Parse-time cost wasn't directly measurable** because every test
  program fails to parse. The LALR baseline runs `baseline_probe.goo`
  end-to-end in ~0.06–0.09s; the GLR build errors at line 21 in ~0.01s
  (process startup dominates; parse-up-to-error is sub-millisecond).
  Real comparison would require fixing the runtime-ambiguity first.
- **Bison 2.3 (Apple default) has a known wart with GLR mode:** the
  `%{...%}` prologue gets duplicated into the generated header, so any
  `T var = ...;` definition triggers a duplicate-symbol link error. I
  worked around by moving the definition into the post-grammar epilogue.
  This is fragile and project-specific; future maintenance would want
  bison 3.x (available via homebrew at `/opt/homebrew/opt/bison/bin/bison`).
- **Verdict: GLR is incompatible with the current grammar as written.**
  Adopting it requires either (a) a multi-week pass to add `%dprec` and
  `%merge` annotations to every ambiguous production cluster, or (b)
  refactoring the grammar to eliminate redundant productions. Both
  exceed the spike scope.

## What I did

Grammar edits in `src/parser/parser.y`:

1. Added `%glr-parser`, `%expect 64`, `%expect-rr 156` after `%union`.
2. Added `%type <node> struct_lit struct_lit_inits struct_lit_init`.
3. Moved `ASTNode* ast_root = NULL;` out of the `%{...%}` prologue (which
   bison 2.3 GLR mode duplicates into the header, causing duplicate-symbol
   link errors) into the post-`%%` epilogue, with an `extern` declaration
   in the prologue.
4. Added `struct_lit` as a new alternative in `primary_expr` with
   `%dprec 1` (lower) vs `identifier` at `%dprec 2` (higher), biasing
   the parse toward bare-identifier wherever both succeed.
5. Added new rules `struct_lit`, `struct_lit_inits`, `struct_lit_init`
   per the probe spec.

Bison invocation: `bison -d -o src/parser/parser.tab.c src/parser/parser.y`
(uses `/usr/bin/bison` 2.3, which is what the project Makefile invokes).

Build: `make -s lexer`. Required a `rm -rf build/parser` to clear stale
object files after the `ast_root` re-shape; clean builds successfully
thereafter.

Tests run:

- `bin/goo -o /tmp/p examples/baseline_probe.goo`
- `bin/goo -o /tmp/p examples/comptime_probe.goo`
- `bin/goo -o /tmp/p /tmp/struct_test.goo` (per spec)
- `bin/goo -o /tmp/p /tmp/if_test.goo` (per spec)
- `bin/goo -o /tmp/p /tmp/empty_func.goo` (minimal `func f() int { return 0 }`)
- `bin/goo -o /tmp/p /tmp/min_test.goo` (minimal `func main() {}`)
- `bin/goo -o /tmp/p /tmp/tinier.goo` (package-only, no func)
- Timing: `/usr/bin/time -p` × 5 on LALR baseline and GLR build.

## Empirical results

### Bison output

```
$ bison -d -o src/parser/parser.tab.c src/parser/parser.y
src/parser/parser.y:1991.5-2007.5: warning: useless rule: js_interop: ...
src/parser/parser.y:2011.5-2025.5: warning: useless rule: dom_access: ...
(no conflict warnings — %expect 64 and %expect-rr 156 silence them)
```

Raw counts (with `%expect` directives removed):

```
$ bison -d ...: conflicts: 64 shift/reduce, 156 reduce/reduce
```

Delta from baseline: **+3 S/R, +0 R/R**. The 3 new S/R conflicts are
all in the IDENT . LBRACE state (the cluster the spike was designed to
handle).

State count: **518 states** (vs 506 baseline). +12 new states for the
struct_lit / struct_lit_inits / struct_lit_init productions.

### Build status

- `bison -d`: clean (no errors, just 2.3-vintage useless-rule warnings
  same as LALR baseline).
- `make -s lexer`: succeeds AFTER cleaning stale `build/parser/*.o`
  with `rm -rf build/parser`. Without the clean, link fails with:

```
duplicate symbol '_ast_root' in:
    build/parser/parser.tab.o
    build/parser/lexer_bridge.o
```

Root cause: bison 2.3 in GLR mode emits the `%{...%}` prologue into both
`parser.tab.c` AND `parser.tab.h` (LALR mode emits to `.c` only). Stale
`.o` files compiled against the LALR header (no `ast_root` declaration
in header) get rebuilt against the GLR header (with `extern ASTNode*
ast_root;`) but the build system doesn't auto-detect the prologue
change. Workaround: move the definition to the post-`%%` epilogue and
declare `extern` in the prologue, then clean before rebuild.

### Test case results

| Test | Input | LALR baseline | GLR build |
|---|---|---|---|
| `baseline_probe.goo` | 130-line probe with `for`, `if`, arithmetic, structs | 19 PASS lines | **Parse error at line 21:6: syntax is ambiguous** |
| `comptime_probe.goo` | comptime block test | exit 55 | **Parse error at line 32:10: syntax is ambiguous** |
| `/tmp/struct_test.goo` | `p := Point{x: 3, y: 4}; fmt.Println(p.x)` | (untested on LALR — composite literal not supported) | **Parse error at line 8:2: syntax is ambiguous** |
| `/tmp/if_test.goo` | `if x > 0 { fmt.Println("ok") }` | (works) | **Parse error at line 7:2: syntax is ambiguous** |
| `/tmp/min_test.goo` | `package main; func main() {}` | (works) | **Parse error at line 4:2: syntax is ambiguous** |
| `/tmp/empty_func.goo` | `func f() int { return 0 }` | (works) | **Parse error at line 3:2: syntax is ambiguous** |
| `/tmp/tinier.goo` | `package main` (alone) | (parses, fails at link) | parses (fails at link, same as LALR) |

**Every program with a function declaration fails.** Even the most
trivial `func main() {}` triggers GLR's runtime ambiguity detection.

The minimal reproducer (`func main() {}`) is ambiguous because the
existing grammar has duplicate productions for "func with empty
signature":

```
FUNC identifier LPAREN RPAREN block             // alt 1
FUNC identifier func_signature block            // alt 2 (where func_signature: LPAREN RPAREN ; is valid)
```

LALR's state-merging absorbed this; GLR keeps both parses, neither
gets `%dprec`, and bison reports the input as ambiguous.

The line numbers reported (e.g., 4:2, 7:2, 8:2) are mostly
end-of-file — bison detected the ambiguity during parse but only
reports it at the point it can't disambiguate by lookahead, which is
typically the closing `}` of the program.

### Timing data

```
LALR baseline (full parse + codegen + link of baseline_probe.goo):
  real 0.09  (cold)
  real 0.06
  real 0.06
  real 0.06
  real 0.06
  → median 0.06s

GLR build (parse fails at line 21, no codegen reached):
  real 0.02  (cold)
  real 0.01
  real 0.01
  real 0.01
  real 0.01
  → median 0.01s
```

This is **not a meaningful parse-time comparison** because GLR aborts
at line 21 of 130 with an ambiguity error before any codegen. The LALR
baseline is doing 6× more work. A proper measurement requires a GLR
build that completes parses successfully.

Process startup dominates both columns (~10–20ms for binary load /
LLVM init). The actual parse-up-to-error in GLR is sub-millisecond
on this file size.

## Verdict on strategy 1 (GLR)

**Doesn't work as an incremental swap-in.** Status: blocked, not
"works-with-caveats".

Evidence:
- Every non-trivial program fails to parse under GLR. The advisor's
  pre-probe flag was correct: the 152 R/R conflicts in cluster A of
  `M10_BASELINE_CONFLICTS_AUDIT.md` (unary-vs-binary operator ambiguity)
  become runtime ambiguities under GLR. Cluster A alone covers every
  arithmetic expression, which is in nearly every Goolang program.
- Additionally, **duplicate productions** in the function-decl grammar
  (alt 1 vs alt 2 above) yield non-conflict parallel parses that GLR
  doesn't resolve. These are NOT in the 217 baseline conflicts — LALR
  silently picked one via state-merging, but GLR explores both.
- `%dprec` on `primary_expr` alternatives is too low a granularity to
  fix this. The redundant productions are at `top_level_decl` level,
  not in expression terminals.

Productionalized LOC estimate:

- Bare-minimum to get parse-completion on existing programs: add
  `%dprec` annotations to every alternative pair that LALR previously
  resolved silently. From the parser.output, that's the 4 cluster-A
  states (152 R/R) plus the function-decl alternatives plus the
  return-stmt alternatives — order of **40–80 `%dprec` annotations**.
- Per-cluster, deciding the right priority would require either a
  reference implementation to compare parse trees against, or a grammar
  rewrite to eliminate the redundancy. Either is 1–2 weeks.
- Plus `%merge` actions where two parses with the same priority both
  succeed (the cluster-A states have 38 lookaheads each that share the
  reduce/reduce — minimum **4 `%merge` functions**, each non-trivial
  because they need to pick the right semantic action).
- Plus a bison-3.x migration path because 2.3 GLR is buggy
  (prologue-in-header) and the project's CI/build doesn't pin bison.
  **Order of 200–400 LOC** in `parser.y` plus build-system changes.

**Total estimated cost: 1–3 weeks of focused grammar work**, with the
risk that even after the cleanup, GLR runtime overhead on the existing
156 R/R clusters makes the compiler measurably slower (typical GLR
overhead 2–10× in ambiguous regions; cluster A's 4 states are hit on
every arithmetic expression, so overhead is pervasive).

Compared to:
- **Strategy 2 (lexer-layer LBRACE_BODY token):** delta is ~50 LOC in
  the lexer bridge + 1 new token + 1 new rule alternative in `block`.
  Doesn't perturb the 217 LALR conflicts. Documented as "viable with
  caveats" in `docs/M10_COMPOSITE_LITERAL_SPIKE_2.md`.
- **Strategy 3 (alt-syntax `&Point{...}` or `.{...}`):** delta is one
  rule in `primary_expr` with a non-IDENT leading token. Zero conflict
  perturbation. Has UX cost (diverges from Go).

GLR is the most expensive of the three by a large margin.

## Open questions / risks

- **Could the 156 R/R conflicts be cleaned up first?** The
  `M10_BASELINE_CONFLICTS_AUDIT.md` recommendation says no — cleanup
  itself is 1–2 weeks and brings no functional gain. If GLR is on the
  table, the cleanup becomes a *prerequisite*, pushing total cost to
  3–4 weeks. This reframes the audit's conclusion: cleanup is "not
  recommended for LALR-only path" but is *required* for the GLR path.
- **Bison version pinning.** Project Makefile invokes `bison` (PATH).
  On macOS this is 2.3 (BSD). On Linux CI it'd be 3.x. Behavior
  differs. If GLR is adopted, the build needs to require a specific
  bison version or bundle a known-good `.tab.c`.
- **GLR error reporting.** Under GLR, parse errors say "syntax is
  ambiguous" pointing to where the parser couldn't resolve, not where
  the user's typo is. This regresses UX vs LALR's "unexpected token X
  expected Y". Users would see "syntax is ambiguous at line 7 column 2"
  for a missing semicolon — much worse diagnostic quality.
- **Performance not measured.** Parse-time comparison requires a build
  that successfully parses both sides. Followup: if anyone pursues
  GLR, write a tiny grammar fragment that parses identically under
  LALR and GLR (e.g., just an expression list with no arithmetic) and
  time both. Expected overhead: 2× on simple expressions, 5–10× on
  programs that hit the cluster-A clusters heavily.
- **Probe didn't fully test the disambiguation.** The `%dprec 2` for
  identifier vs `%dprec 1` for struct_lit might have correctly biased
  the struct_lit conflict — but I couldn't observe this because every
  program failed before reaching a struct_lit. If/when the prerequisite
  cleanup happens, re-test whether `%dprec` actually resolves the
  IDENT . LBRACE ambiguity in `if X { body }` correctly.

## Conclusion

Strategy 1 (GLR parser) is empirically blocked by the current grammar's
mix of 156 baseline R/R conflicts plus redundant productions in
function-decl. The fix is feasible (1–3 weeks of grammar refactoring +
bison migration) but exceeds the M10 composite-literal scope by an
order of magnitude. Strategy 2 (lexer-layer) and Strategy 3
(alt-syntax) are both substantially cheaper.

Recommend: pick Strategy 2 or 3 for M10. Reserve Strategy 1 (GLR) for
a future grammar-modernization milestone that's scoped explicitly
around it.
