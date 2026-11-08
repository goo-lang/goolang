# The Power of Ten record

Gerard Holzmann's "The Power of Ten: Rules for Developing Safety-Critical
Code" (IEEE Computer, June 2006) sets out ten rules for C aimed at code
whose failure is unacceptable. The talk that introduced them makes the
point this record exists to honour: a rule with no checker has no effect.
Naming a rule is free; a checker is what makes it true of the tree instead
of true of the intention.

## What this record is for

For each of the ten rules, this page names the checker that holds it, or
records the word "unchecked" when none exists yet. Three rules —
5, 7 and 10 — have a checker, and those three are the ones this project's
verification plan gates in `make verify-core`
(`assertion-density-probe`, `pthread-return-probe`, `warning-count-probe`).
The other seven are recorded here with a measured count and a decision, not
because the count is a target, but so that a later decision to adopt, defer,
or decline any of them starts from a number instead of from nothing.

This is a companion to `docs/misra/README.md`, which covers MISRA C:2012.
Holzmann's ten rules and MISRA's 173 guidelines overlap in places (Rule 3
matches MISRA Dir 4.12, deviation D-11; Rule 7's pointer-parameter half
overlaps MISRA Rule 17.7, deviation D-02) but neither is a subset of the
other, so this record stands on its own rather than folding into the MISRA
table.

## The ten rules

| Rule | Count (instrument, date) | Checker | Decision |
|---|---|---|---|
| 1. Restrict to simple control flow constructs (no `goto`, `setjmp`/`longjmp`, direct or indirect recursion) | 91 hits (Cobra 5.4, `p10` script, 2026-09-02) | unchecked | Declined by design. Of the 91 hits, 1 is the `goto`, and 2 `setjmp` plus 1 `longjmp` are in `src/runtime/testing.c`, the documented mechanism behind `t.Fatal` in `goo test`. The rest are recursion, which a compiler that walks trees is built from. |
| 2. Give all loops a fixed upper bound | 13 hits (Cobra 5.4, `p10` script, 2026-09-02) | unchecked | Not adopted now. The hits include a loop over `argv` up to its `NULL` terminator, a bound that comes from data rather than from a fixed limit in the source. A useful check here would need to tell that case apart from a genuinely unbounded loop, which the script does not attempt. |
| 3. Do not use dynamic memory allocation after initialization | 1,794 hits (Cobra 5.4, `p10` script, 2026-09-02) | unchecked | Declined. Deviation record D-11 (`docs/misra/deviations/D-11.md`) already covers this ground under MISRA Dir 4.12: dynamic allocation is the memory model (ARC, arenas, `xalloc`). |
| 4. Limit functions to 60 lines of text (the Cobra `p10` script's own bound is 75) | 141 functions over 75 lines (Cobra 5.4, `p10` script, 2026-09-02) | unchecked | Not adopted now. The largest is a 522-line `switch` over AST node kinds in `src/ast/ast.c`. Bringing every function under a line limit is a refactoring programme in its own right, not something a gate can demand overnight. |
| 5. Use a minimum of two assertions per function, on average | 23 assertions in 40,376 executable lines, 0.057% (`assertion-density-probe`, 2026-09-02, `scripts/assertion-density-baseline.txt`) | `assertion-density-probe` (`make verify-core`) | A target. The gate is shrink-proof (it can only hold or increase, never fall) but the density is far under the rule's roughly 2% implied by "two per function". Assertions are on by default in `bin/goo` since task 24b, with `GOO_ASSERT_OFF` as the kill switch (see `docs/misra/README.md`'s "Known limits of the checker" section). |
| 6. Declare data objects at the smallest possible scope | no count | unchecked | Unchecked. The Cobra `p10` script's rule 6 check calls out to a `scope_check` back-end tool that this build did not compile, so the script produced no number for this rule at all. |
| 7. Check the return value of every function, and check the validity of every parameter | 1,054 unchecked pointer parameters (Cobra 5.4, `p10` script, 2026-09-02); 40 unchecked pthread returns (`pthread-return-probe`, 2026-09-02, `scripts/pthread-return-baseline.txt`) | `pthread-return-probe` (`make verify-core`), pthread return values only | A target for the pthread slice, which the gate holds shrink-only at 40 unchecked calls. Everything else in this rule is unchecked here: the 1,054 unchecked pointer parameters, and the remaining unchecked return values, which sit on the fix backlog under MISRA Rule 17.7 (`docs/misra/README.md`). |
| 8. Limit the use of the preprocessor to file inclusion and simple macros | 51 hits (Cobra 5.4, `p10` script, 2026-09-02) | unchecked | Not adopted now. |
| 9. Limit pointer use to one level of dereferencing | 46 hits (Cobra 5.4, `p10` script, 2026-09-02) | unchecked | Not adopted now. |
| 10. Compile with all warnings active, and fix every one | 75 C warnings, 8 bison warnings, gcc-16 16.2.1, `-Wall -Wextra -std=c23` (`warning-count-probe`, 2026-09-02, `scripts/warning-count-baseline.txt`) | `warning-count-probe` (`make verify-core`) | A target, not met. The flags are already on. The gate is shrink-only today; it does not yet fail on a nonzero count, and there is no `-Werror`. Getting to zero and adding `-Werror` is task 22 of the verification plan. |

Every count above that came straight from Cobra's `p10` script is a Cobra
count, not a decision about severity: Cobra counts pattern matches, and a
pattern match is not the same thing as a defect. Rules 5, 7 and 10 quote a
second, narrower number from this project's own gate scripts where one
exists, because that is the number the gate actually holds the tree to.

## The instrument, and its two known defects

Instrument: Cobra 5.4, source at `github.com/nimble-code/Cobra`, `master` at
commit `eebff89637542b0bfdc76739fbad4f6a365c51ae` (this build printed
`Version 5.4 - 13 April 2026`). Built with `make all` in Cobra's own `src/`
directory; Cobra's back-end tools were not built. Run over 68 `src/*/*.c`
files on 2026-09-02, excluding the generated parser (`src/parser/parser.tab.c`),
single core, raw tokens (no `-cpp`), exit 0, wall time 0.89 s.

Two defects in the instrument, as run here, rather than in the code it
measured:

1. **A broken check inside the `p10` script itself.** Its rule 5 check
   prints `Rule 5: counted 8 assertions; it should be > 498.32` and then
   `expr: syntax error` on a malformed test against a `.fnm` variable,
   before the check can complete. The 8 it printed there is neither the
   1,443-pattern count in the rule 5 row of Cobra's own summary table, nor
   the 23 assertions this project's `assertion-density-probe` counts by its
   own rule (`scripts/assertion_density_probe.sh`); none of the three claim
   to measure the same thing, and this record uses the gate's number for
   rule 5, not either of Cobra's.
2. **Rule 6 needs the `scope_check` back-end tool**, which this build did
   not compile, so rule 6 above has no count at all rather than a wrong one.

### Reproducing the numbers

```sh
# 1. Build Cobra at the pinned commit.
git clone https://github.com/nimble-code/Cobra
cd Cobra
git checkout eebff89637542b0bfdc76739fbad4f6a365c51ae
( cd src && make all )

# 2. Point it at its own rule set.
export C_BASE="$(pwd)/rules"

# 3. Run from a scratch directory: Cobra writes result files
#    (e.g. _P10_.txt) into the current working directory.
mkdir /tmp/p10-scratch && cd /tmp/p10-scratch
/path/to/Cobra/src/cobra -terse -f p10 -j1 \
    $(ls /path/to/goolang/src/*/*.c | grep -v parser.tab.c)
```

Cobra takes a file list rather than a directory, so the generated parser
file has to be filtered out of the glob before it is passed in, as the
`grep -v` above does.

## Verification

`bash scripts/doc_claims_probe.sh` passed on 2026-09-02, unaffected by this
page (it checks stdlib and packaging claims, none of which this page
makes). The three gate numbers quoted above were confirmed by running each
gate directly on 2026-09-02:

- `make assertion-density-probe` → `PASS (23 assertions / 40376 executable
  lines = 0.057%, src/ 68 files, baseline 23 / 40376)`
- `make pthread-return-probe` → `PASS (40 unchecked pthread returns in
  src/, 68 files, baseline 40)`
- `make warning-count-probe` → `PASS (75 C warnings, 8 bison warnings,
  gcc-16 16.2.1, -Wall -Wextra -std=c23, full rebuild of bin/goo, baseline
  75)`
