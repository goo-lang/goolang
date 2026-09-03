# Cobra calibration spike — specification

Plan: docs/superpowers/plans/2026-09-02-verification-tooling-next-steps.md, tasks 12 and 13.
Status: draft for review, 2026-09-03. Nothing here is a gate until task 13 decides.

## 1. Purpose

Decide, from measurement, whether Cobra (nimble-code/Cobra, Gerard
Holzmann's structural code analyser) can enforce MISRA C:2012 rules that
this project today enforces by review only. The spike must be able to
answer NO. A calibration that cannot fail is not a calibration.

## 2. Which rules, and why the two lists disagree

`docs/misra/enforcement-plan.md` and the task 12 brief use two different
definitions of "rules cppcheck cannot examine":

| Definition | Source | Count | Cobra has a definition for |
|---|---|---|---|
| Review-only: enforcement column is `review` | enforcement-plan.md main table | 12 Rules + 8 Directives | 3 of the 12 (2.6, 17.4, 18.3) |
| No cppcheck check: decision `ADOPT_OTHER` | guideline-decisions.tsv | 28 Rules + 9 Directives | most |
| The brief's ten | task-12-brief.md | 1.3 2.1 2.6 3.2 5.3 17.4 18.1 18.2 18.3 18.6 | all ten |

Measured against the installed cppcheck (2.21.1, `misra.py`, 133
`misra_N_M` functions): none of the 22 rule numbers in the union of the
first and third rows has a cppcheck check. The positive control, Rule 5.1,
is in the list.

The spike calibrates in two tiers:

- **Tier 1, the pass bar: the brief's ten, minus 1.3.** Every one has an
  existing `m12_rule<N>_<M>` block in Cobra's `rules/misra/misra2012.def`,
  so calibration is "run Cobra's own check against controls", the cheapest
  honest measurement of whether Cobra works on this code at all. Rule 1.3 is
  dropped: its block carries the author's own comment "not really a match
  of the m2012 rule?" and the same rule is listed in `misra2012_notyet`.
  A rule the tool's author doubts is not a calibration subject. Nine rules.
- **Tier 2, the enforcement gap: the twelve review-only Rules.** Three
  overlap tier 1. Of the other nine, Cobra lists seven as not yet done and
  is silent on 12.5 and 21.13. Tier 2 is measured only where a Cobra
  definition exists (2.6, 17.4, 18.3, through tier 1). The other nine are
  recorded in the deliverable table as "no definition", each with a verdict
  on whether a structural analyser could express it at all (plausible:
  8.13, 12.5, 14.3, 20.6, 21.13; needs semantics a token pattern cannot
  see: 13.2, 17.5, 22.1, 22.3). Writing a new Cobra rule is out of scope.

Directives stay out, except Dir 4.7 (error information shall be tested),
which the second instrument covers.

## 3. Two defects in the documents, fixed first

`enforcement-plan.md` gives two answers for two rules. The spike's first
commit resolves both, with the reason:

- **Rule 17.4**: main table `review`, second table "`-Wreturn-type`, in
  `-Wall`". The compiler warning is real and verify-core requires a clean
  build, so the main table becomes `compiler`. 17.4 then leaves the
  review-only set, and tier 1 still measures it.
- **Rule 22.4**: main table `valgrind, sanitizer, fuzzer`, second table
  `review`. No sanitizer reports a write to a read-only stream, so the main
  table becomes `review`.

Cobra's `misra2012.def` labels rule 18.6 findings with the caption
`M12_R18_5`, a naming bug in Cobra's file. The harness counts 18.6 by that
caption and the spec records the bug for an upstream report.

## 4. The instrument

- **Source**: a scratch build of Cobra pinned to commit
  `eebff89637542b0bfdc76739fbad4f6a365c51ae` (2026-05-30). The spike
  fetches it once by hand into a scratch directory. Nothing in the spike is
  a gate, so plan decision 1 (no gate reads the network) is not touched.
  If task 13 decides yes, the tarball and its sha256 are vendored under
  `third_party/` and built by `scripts/cobra_bootstrap.sh`, a copy of
  `scripts/spin_bootstrap.sh`'s shape.
- **Build**: `make` in `src/`, then `make install` in `src/` for the
  back-end tools, because rule 8.9 calls `scope_check`.
- **Run shape**: from a build directory, never the source tree, because
  Cobra writes result files into the working directory. Single thread:
  `-N` serves queries only, and the brief records a hang with `-N` on a
  rule file. Batch mode is `-f`, which implies `-quiet`. The full set runs
  as `cobra -terse -f misra2012 -F <file list>`; a per-rule count is taken
  from the caption in each finding line (`file:line: ... M12_R<N>_<M>`).
  The file list is the `.c` files under `src/` with `-Iinclude -I.
  -D_GNU_SOURCE`, one per line, until task 14's compile-command capture
  replaces it.
- **Second instrument**: `rules/refmod/func_patterns.cobra` (Cobra book,
  section 9.5) finds return values that are not tested. Run it over `src/`
  and record its count beside the pthread-return ratchet's count from task
  1. The two instruments share no code, so agreement is evidence and
  disagreement is a finding.

## 5. The controls, per rule

For each tier 1 rule, two minimal C files of the same shape:

| Rule | Violating control | Compliant twin |
|---|---|---|
| 2.1 | a statement after an unconditional `return` | the statement moved before the `return` |
| 2.6 | a label no `goto` targets | the label removed |
| 3.2 | a line splice (`\`) inside a `//` comment | the comment without the splice |
| 5.3 | an inner-scope identifier hides an outer one | the inner identifier renamed |
| 17.4 | a non-void function with one exit path that has no `return` | an explicit `return` on that path |
| 18.1 | a pointer stepped past one-past-the-end of its array | the loop bound reduced by one |
| 18.2 | subtraction of two pointers into different objects | both pointers into one array |
| 18.3 | `<` between addresses of two unrelated objects | `<` between two elements of one array |
| 18.6 | the address of an automatic object stored in a static | the address stored in an automatic of the same scope |

Four **known incorrect reports** from the brief are negative controls of
the harness, not of Cobra: the static buffer case for 18.6, the `while (0)`
case for 2.1, the `strchr` call for 13.5, and the `sizeof` without
parentheses for 13.6. Each must produce a finding that the hand check
classifies as incorrect. A harness that never records "incorrect" is
broken, and this is how it proves it is not.

## 6. The measurements, and the pass bar

For each tier 1 rule:

1. The violating control produces at least one finding with the rule's
   caption. Otherwise the rule FAILS calibration: Cobra cannot see it.
2. The compliant twin produces zero findings with that caption. Otherwise
   the rule FAILS calibration: Cobra cannot tell the two apart.
3. On `src/`: the finding count, and a hand check of the first ten
   findings in file order, each classified correct or incorrect with one
   line of reason. The incorrect-report rate is incorrect over checked.

The spike PASSES when at least one rule passes both controls. Task 13 then
vendors Cobra. A rule becomes a gate only when it passes both controls and
its incorrect-report rate is at most 2 of 10; a passing rule above that
rate is recorded as "seen, not adopted", with the ten classifications, and
its `review` row stays. No rule is adopted on the strength of a green
control alone.

The second instrument reports two counts side by side and a one-line
reading of the difference.

## 7. Deliverable

A table in `docs/misra/cobra-calibration.md`, one row per tier 1 rule:
rule, violating control (pass/fail), compliant twin (pass/fail), count on
`src/`, hand-checked 10 (correct/incorrect), incorrect-report rate,
verdict (gate / seen not adopted / cannot see). Then the tier 2 rows with
"no definition" and the plausibility verdict. Then the two instrument
counts. Then the task 13 decision with its reason.

If task 13 vendors Cobra: one make target `cobra-misra-probe` in
`VERIFY_ALL_DEPS` per plan decision 2 (a `--self-test` that runs the
violating controls and expects red, the parity count bump, the census
regeneration where the base has it), a suppression baseline in the shape of
`docs/misra/misra-suppressions.txt`, and the adopted rules' enforcement
rows changed from `review` to `cobra`. The Bazel target waits for the phase
4 branch, per decision 3.

## 8. Out of scope

New Cobra rules for tier 2's nine. Mechanising the Power of Ten record
with Cobra's `p10` rule set (a later task; the record's measured status
per rule is the baseline it would have to match). Any change to the source
tree to satisfy a Cobra finding: the spike measures, it does not fix.

## 9. Risks

Cobra's tokeniser, not a compiler front end, reads C23 code; a construct it
cannot parse is a finding to record, not a reason to stop. The `-N` hang.
The 18.6 caption bug. The doubtful 1.3 block, which is why it is out. A
hand check is one person's reading: the ten classifications are written
down so a second reader can disagree.
