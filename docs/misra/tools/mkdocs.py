#!/usr/bin/env python3
"""Generate docs/misra/README.md and docs/misra/enforcement-plan.md.

The tables come from decisions.tsv so that the numbers in the prose cannot
drift away from the numbers in the table. Re-run this after editing the
decision data in policy.py.
"""
import collections
import csv
import os
import re
import sys

TEXT_RE = re.compile(r"^(Rule|Dir) (\d+\.\d+) (Required|Advisory|Mandatory)$")

DECISION_LABEL = {
    "ADOPT": "Adopted, clean",
    "ADOPT_OTHER": "Adopted, enforced by other means",
    "FIX": "Adopted, fix backlog",
    "DEVIATE": "Deviation record",
    "DECLINE": "Declined (advisory)",
    "OUT": "Outside this edition",
}

PHASE_TITLE = {
    "P1": "Phase 1 - undefined behaviour and real defect classes",
    "P2": "Phase 2 - declaration hygiene and dead code",
    "P3": "Phase 3 - the essential type model",
}

PHASE_BLURB = {
    "P1": "Every rule here marks code that is either undefined, or a defect "
          "class this project has already been bitten by. It splits in two. "
          "Twenty-six rules carry 357 violations between them, which is an "
          "afternoon of work. Rule 17.7 carries the other 1,516 on its own, "
          "840 of them an unchecked `pthread_*` return value, and that one is "
          "a project in itself. Do the 357 first.",
    "P2": "Declaration hygiene, dead code and brace discipline. Mostly "
          "mechanical. Rule 15.6 dominates the count and is fixable with "
          "`clang-tidy -checks=readability-braces-around-statements -fix`.",
    "P3": "The essential type model. Real value, but the largest and least "
          "mechanical group. Do it one directory at a time, starting with "
          "`src/types`, and only after Phase 1 and Phase 2 are green.",
}


def load_texts(path):
    cats, texts = {}, {}
    lines = open(path, encoding="ascii").read().splitlines()
    for i, line in enumerate(lines):
        m = TEXT_RE.match(line.strip())
        if m:
            cats[(m.group(1), m.group(2))] = m.group(3)
            texts[(m.group(1), m.group(2))] = lines[i + 1].strip()
    return cats, texts


def order(row):
    return (row["kind"] != "Dir", tuple(int(x) for x in row["num"].split(".")))


def name(row):
    return "%s %s" % (row["kind"], row["num"])


def main():
    tsv, rule_texts, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
    cats, texts = load_texts(rule_texts)
    rows = sorted(csv.DictReader(open(tsv, encoding="utf-8"), delimiter="\t"),
                  key=order)
    for r in rows:
        for k in ("violations", "to_fix", "waived"):
            r[k] = int(r[k])
        r["text"] = texts.get((r["kind"], r["num"]), "")

    live = [r for r in rows if r["decision"] != "OUT"]
    by = collections.Counter(r["decision"] for r in live)
    adopted = by["ADOPT"] + by["ADOPT_OTHER"] + by["FIX"]
    fixes = sum(r["to_fix"] for r in rows)
    waived = sum(r["waived"] for r in rows)
    out_of = sum(r["violations"] for r in rows if r["decision"] == "OUT")

    os.makedirs(outdir, exist_ok=True)
    os.makedirs(os.path.join(outdir, "deviations"), exist_ok=True)

    w = open(os.path.join(outdir, "README.md"), "w", encoding="utf-8").write

    w("""# MISRA C:2012 adoption policy

This directory records which MISRA C:2012 guidelines the Goo compiler adopts,
which it does not, and why. It is the input to a build gate, not a
certification artefact.

Source: MISRA C:2012, Third edition, first revision (February 2019), 17
directives and 156 rules. Guideline headline text is quoted from a licensed
copy solely to identify the guideline under discussion.

## This is not a compliance claim

MISRA C:2012 section 5.5 sets the bar for claiming compliance. This project
does not meet it, and does not try to:

- **The language is out of scope.** MISRA C:2012 covers C90 and C99 (section
  3.1). This compiler is built with `-std=c23`. A later MISRA edition covers
  C11/C18; this one does not.
- **The target is out of scope.** MISRA addresses embedded critical systems on
  a freestanding implementation. `bin/goo` is a hosted development tool.
- **Coverage is partial.** The checking tool implements 130 of the 156 rules
  and none of the 17 directives.

What this policy *is*: a deliberate subset of MISRA's defect-class guidance,
adopted because it catches bugs that make a compiler wrong, with an honest
record of everything deliberately left out.

## Summary

| | Count |
|---|---|
| Guidelines in this edition | %d (156 rules + 17 directives) |
| **Adopted** | **%d** |
| &nbsp;&nbsp;- already clean, held by the gate | %d |
| &nbsp;&nbsp;- enforced by review, sanitizer or fuzzer | %d |
| &nbsp;&nbsp;- adopted with a fix backlog | %d |
| Deviated (Required, needs a record) | %d |
| Declined (Advisory, MISRA 6.2.3) | %d |
| Violations to fix | %s |
| Violations waived by deviation or decline | %s |

Baseline scan: %s violations in hand-written code = %s to fix + %s waived
+ %s outside this edition.
No Mandatory guideline is violated, which is what makes the rest of this
policy possible: MISRA 6.2.1 permits no deviation from a Mandatory guideline.

## Only 30%% of these violations are in code that ships

`bin/goo` links 57 of the 147 `.c` files under `src/`. The rest is the Task
#22-era framework code that P5.6 unlinked — constraint inference, concept
generics, HKT, proof obligations, the IPFS client — kept only behind standalone
test targets. Measured 2026-08-14:

| Where | Violations | Share |
|---|---|---|
| Files linked into `bin/goo` | 9,205 | 30.0%% |
| Files **not** linked | 20,329 | 66.2%% |
| Headers | 1,189 | 3.9%% |

**Two thirds of the backlog is in code the project has already decided not to
ship.** Fix the shipped set first. Phase 1 restricted to shipped code and
headers is **170 violations** excluding Rule 17.7 — a single sitting, and it is
the highest-value slice of the whole exercise.

Reproduce the split with:

    # the informational LLVM banner shares the line, so filter to paths
    make --eval='__p: ; @echo $(GOO_SRCS)' __p | tr ' ' '\\n' | grep '\\.c$'

## How the decision was made

A guideline is adopted when breaking it can produce **undefined behaviour, a
wrong compilation result, memory corruption, or a leaked resource**. A
guideline is refused when it encodes embedded-C house style, or when it
forbids something this compiler is built out of.

The three big refusals all fall in the second group. Dynamic memory, recursion
and the AST downcast are not incidental habits; they are the architecture. See
D-01, D-05, D-06 and D-11.

""" % (len(live), adopted, by["ADOPT"], by["ADOPT_OTHER"], by["FIX"],
       by["DEVIATE"], by["DECLINE"], "{:,}".format(fixes),
       "{:,}".format(waived), "{:,}".format(fixes + waived + out_of),
       "{:,}".format(fixes), "{:,}".format(waived), out_of))

    w("## Project deviations (Required guidelines not followed)\n\n")
    w("MISRA 5.4 requires a formal record for each. One file each, in "
      "`deviations/`, using the Appendix I format.\n\n")
    w("| ID | Guideline | Waived | Reason |\n|---|---|---|---|\n")
    for r in sorted((r for r in rows if r["decision"] == "DEVIATE"),
                    key=lambda r: r["deviation"]):
        w("| [%s](deviations/%s.md) | %s - %s | %s | %s |\n"
          % (r["deviation"], r["deviation"], name(r), r["text"],
             "{:,}".format(r["waived"]), r["note"]))

    w("\n## Declined advisory guidelines\n\n")
    w("MISRA 6.2.3 does not require a formal deviation for an advisory "
      "guideline, but it does ask that non-compliance be documented. This "
      "table is that record.\n\n")
    w("| Guideline | Waived | Reason |\n|---|---|---|\n")
    for r in sorted((r for r in rows if r["decision"] == "DECLINE"),
                    key=lambda r: -r["waived"]):
        w("| %s - %s | %s | %s |\n"
          % (name(r), r["text"], "{:,}".format(r["waived"]), r["note"]))

    w("\n## Fix backlog\n\n")
    for phase in ("P1", "P2", "P3"):
        sel = sorted((r for r in rows if r["phase"] == phase),
                     key=lambda r: -r["to_fix"])
        n = sum(r["to_fix"] for r in sel)
        w("### %s\n\n%s\n\n" % (PHASE_TITLE[phase], PHASE_BLURB[phase]))
        w("%d rules, %s violations to fix.\n\n"
          % (len(sel), "{:,}".format(n)))
        w("| Guideline | Category | To fix | Note |\n|---|---|---|---|\n")
        for r in sel:
            w("| %s - %s | %s | %s | %s |\n"
              % (name(r), r["text"], r["category"],
                 "{:,}".format(r["to_fix"]), r["note"]))
        w("\n")

    w("## Every guideline\n\n")
    w("| Guideline | Category | Decision | Violations | Checked by cppcheck |\n")
    w("|---|---|---|---|---|\n")
    for r in rows:
        w("| %s - %s | %s | %s%s | %s | %s |\n"
          % (name(r), r["text"], r["category"],
             DECISION_LABEL[r["decision"]],
             (" (%s)" % r["deviation"]) if r["deviation"] else
             ((" (%s)" % r["phase"]) if r["phase"] else ""),
             r["violations"] or "-", r["checked"]))

    w("""
## Running the gate

    make misra                     # fail on any violation not in the baseline
    make misra-baseline            # accept the current findings as the baseline
    scripts/misra-scan.sh --report # full breakdown, never fails

`make misra` does not fail on the 7,168 accepted violations. It records the
count per file and rule in `scripts/misra-baseline.txt` and fails only when a
count rises or a new pair appears. This is the same shape as `make safety`.

Setup is one step and it is not automatic: the cppcheck addon needs a rule-text
file built from **your own licensed copy** of the standard. That file is not in
this repository, because the guideline text is copyrighted. See
`tools/README.md`. Until it exists, `make misra` exits 2 with an explanation
rather than passing silently.

For the same reason the gate is **not** part of `make verify-core`. A fresh
checkout cannot run it, so wiring it into the shared gate would break the build
for anyone without the PDF.

Verified 2026-08-14 that the gate has teeth: adding an octal constant in a new
file made it report `rule 7.1  0 -> 1` and exit 1; removing the file returned it
to exit 0. A gate that has never been seen to fail is not a gate.

After a fix, tighten the baseline. `make misra` prints how many violations
fell, and a baseline that is never tightened stops being a gate.

See `enforcement-plan.md` for what each adopted guideline is enforced by.

## Known limits of the checker

Read these before trusting any number this tool produces.

- **cppcheck exits 0 with violations present.** A gate must pass
  `--error-exitcode=1`, or it will report green forever.
- **Rule 7.3 is broken in the addon.** Its regex
  `^(0[xX])?[0-9a-fA-FpP.]+[Uu]*l+[Uu]*$` matches the identifiers `call`,
  `decl`, `al` and `bl` as if they were numeric literals. In the baseline
  scan, 382 of 384 reports were identifiers. The rule stays adopted, because
  the two real reports are real; the noise must be suppressed per site.
- **Rule 17.3 reports library calls as implicit declarations.** cppcheck has
  no signature for `atomic_*`, `pthread_cond_*`, `curl_*` or `json_object_*`,
  so it reports every call as an implicit declaration. C23 makes a true
  implicit declaration a constraint violation, so a build that passes
  `-std=c23 -Wall -Wextra` cannot contain one. Adding `--library=posix
  --library=gnu` cut this from 1,666 to 660.
- **Generated code is excluded.** `src/parser/parser.tab.c` and the `parser.y`
  lines its `#line` directives point at produce 1,542 findings, 912 of them
  Rule 20.13. MISRA section 6.8 treats generated code separately.
- **cppcheck stops at C11.** Any C23-only construct is parsed as C11.
- **Platform-conditional code is invisible.** The scan runs on Linux, so an
  `#ifdef _WIN32` branch is never preprocessed and never checked. The `system()`
  call at `src/codegen/codegen.c:1797` is a real Rule 21.8 violation that the
  baseline reports as absent. A guideline reading clean means clean *on this
  platform's preprocessed source*, not clean in the tree.
""")

    # ------------------------------------------------------------ enforcement
    e = open(os.path.join(outdir, "enforcement-plan.md"), "w",
             encoding="utf-8").write
    e("""# MISRA guideline enforcement plan

For each adopted guideline, what actually enforces it. MISRA 5.5 calls this a
compliance matrix. A guideline with no enforcement listed is not adopted; see
`README.md`.

Enforcement mechanisms in this project:

| Tag | Mechanism |
|---|---|
| `cppcheck` | The cppcheck MISRA addon, run over `src/` |
| `compiler` | `gcc -Wall -Wextra -std=c23`, which `make verify-core` requires to be clean |
| `valgrind` | `arena-valgrind-probe`, `ast-free-leak-probe` and the other valgrind gates |
| `sanitizer` | `far-transport-asan` (ASan) and the TSan targets |
| `fuzzer` | `bin/fuzz_parse`, see `tests/fuzz/README.md` |
| `review` | Code review, with no tool support |

""")
    MECH = {
        "1.1": "review", "2.1": "compiler", "3.1": "review",
        "4.1": "valgrind, sanitizer, fuzzer", "4.2": "review (1 site)", "4.3": "review (1 site)",
        "4.4": "review", "4.5": "review", "4.7": "review", "4.10": "compiler",
        "4.11": "review, fuzzer", "4.13": "review", "4.14": "fuzzer",
    }
    e("| Guideline | Category | Decision | Enforced by |\n|---|---|---|---|\n")
    for r in rows:
        if r["decision"] in ("DEVIATE", "DECLINE", "OUT"):
            continue
        if r["kind"] == "Dir":
            how = MECH.get(r["num"], "review")
        elif r["checked"] == "yes":
            how = "cppcheck"
        else:
            how = "review"
            if r["num"] in ("9.1", "19.1", "21.17", "21.18", "22.2", "22.4",
                            "22.6", "18.1", "18.2", "18.6"):
                how = "valgrind, sanitizer, fuzzer"
            elif r["num"] in ("1.1", "1.3", "2.1", "3.2", "5.3", "8.3"):
                how = "compiler"
        e("| %s - %s | %s | %s | %s |\n"
          % (name(r), r["text"], r["category"],
             DECISION_LABEL[r["decision"]], how))

    e("""
## The Mandatory guidelines the checker cannot see

MISRA 6.2.1 permits no deviation from a Mandatory guideline, so these need a
credible enforcement story even though cppcheck has no check for them.

| Guideline | Enforced by |
|---|---|
| Rule 9.1 - object with automatic storage read before being set | valgrind, MemorySanitizer-class checks |
| Rule 12.5 - `sizeof` of a function parameter declared as an array | compiler warning, review |
| Rule 17.4 - non-void function with an exit path and no return | `-Wreturn-type`, which is in `-Wall` |
| Rule 19.1 - object assigned or copied to an overlapping object | valgrind, ASan |
| Rule 21.13 - `<ctype.h>` argument outside `unsigned char` | review; the lexer is the only caller |
| Rule 21.17 - `<string.h>` access beyond the object | ASan, valgrind, fuzzer |
| Rule 21.18 - `<string.h>` `size_t` argument out of range | ASan, valgrind, fuzzer |
| Rule 22.2 - freeing a block that was not allocated | ASan, valgrind, `ast-free-leak-probe` |
| Rule 22.4 - writing to a stream opened read-only | review |
| Rule 22.6 - using a `FILE *` after it is closed | ASan, valgrind |

Most of this list is already covered by gates that exist and run today. That
is the honest reason a Mandatory guideline can be claimed without a MISRA
tool: another instrument checks it, and that instrument has teeth.
""")
    print("wrote README.md and enforcement-plan.md to %s" % outdir)


if __name__ == "__main__":
    main()
