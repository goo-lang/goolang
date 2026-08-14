#!/usr/bin/env python3
"""The adoption decision for every MISRA C:2012 guideline, as data.

Decisions:
  ADOPT       already clean, enforced from today by the cppcheck gate
  ADOPT_OTHER adopted, but no cppcheck check exists - enforced by another means
  FIX         adopted, violations exist, on a dated backlog
  DEVIATE     Required/Mandatory guideline not followed - needs a deviation record
  DECLINE     Advisory guideline not followed - MISRA 6.2.3 needs a note, not a record
  OUT         not in MISRA C:2012 Third edition first revision

Emits the tables for docs/misra/README.md and checks its own arithmetic.
"""
import collections
import re
import sys

# ---------------------------------------------------------------- decisions
# Violated rules. Every entry is a deliberate call, keyed by rule number.
DECIDED = {
    # --- Required, not followed: each needs a Project Deviation record -----
    "21.3":  ("DEVIATE", "D-01", "malloc/free is the memory model: ARC, arenas, xalloc"),
    "17.7":  ("DEVIATE", "D-02", "1,709 waived by D-02 (printf and mem/str family); 1,516 kept, 840 of them an unchecked pthread_* return"),
    "14.4":  ("DEVIATE", "D-03", "if (ptr) and if (!x) are the house idiom"),
    "21.6":  ("DEVIATE", "D-04", "a compiler must print diagnostics"),
    "11.3":  ("DEVIATE", "D-05", "the AST downcast (CallExprNode *)node is the architecture"),
    "17.2":  ("DEVIATE", "D-06", "recursive descent parser and AST walks"),
    "17.1":  ("DEVIATE", "D-07", "varargs diagnostic printers"),
    "21.10": ("DEVIATE", "D-08", "the compiler measures and reports time"),
    "21.8":  ("DEVIATE", "D-09", "exit() carries the compiler's exit status"),
    "21.4":  ("DEVIATE", "D-10", "setjmp/longjmp implements t.Fatal in goo test"),

    # --- Required, adopted, violations to fix ------------------------------
    "15.6":  ("FIX", "P2", "braces; clang-tidy readability-braces-around-statements"),
    "10.4":  ("FIX", "P3", "essential type category; start in src/types"),
    "5.6":   ("FIX", "P3", "typedef uniqueness; triage first"),
    "8.6":   ("FIX", "P2", "one external definition; an ODR defect class"),
    "8.4":   ("FIX", "P2", "compatible declaration visible"),
    "15.7":  ("FIX", "P3", "if...else if needs a terminal else"),
    "10.1":  ("FIX", "P3", "inappropriate essential type"),
    "10.3":  ("FIX", "P3", "narrowing assignment"),
    "11.8":  ("FIX", "P1", "free((void*)x) on a const char* field; a real defect class"),
    "5.7":   ("FIX", "P3", "tag uniqueness"),
    "12.2":  ("FIX", "P1", "shift out of range is undefined behaviour"),
    "4.1":   ("FIX", "P1", "unterminated escape sequence"),
    "20.7":  ("FIX", "P1", "unparenthesised macro parameter"),
    "10.8":  ("FIX", "P3", "composite expression cast"),
    "16.4":  ("FIX", "P1", "a missing default hides a new enum case; the sites switch on kind tags such as comptime value->type"),
    "8.5":   ("FIX", "P2", "declared once in one file"),
    "5.8":   ("FIX", "P2", "external identifier uniqueness"),
    "14.2":  ("FIX", "P2", "for loop well-formed"),
    "22.8":  ("FIX", "P1", "errno not cleared before the call"),
    "22.9":  ("FIX", "P1", "errno not tested after the call"),
    "11.6":  ("FIX", "P1", "void pointer to arithmetic type"),
    "16.3":  ("FIX", "P1", "missing break; fall-through must be explicit"),
    "7.4":   ("FIX", "P1", "string literal assigned to a non-const object"),
    "21.7":  ("FIX", "P1", "atoi/atof cannot report an error; use strtol"),
    "21.1":  ("FIX", "P1", "#define on a reserved identifier is undefined"),
    "10.6":  ("FIX", "P3", "composite expression assigned to a wider type"),
    "9.5":   ("FIX", "P2", "designated initializer array size"),
    "8.2":   ("FIX", "P2", "prototype form with named parameters"),
    "3.1":   ("FIX", "P1", "/* inside a comment"),
    "21.16": ("FIX", "P1", "memcmp on pointers to different types"),
    "11.9":  ("FIX", "P2", "NULL is the only null pointer constant"),
    "16.2":  ("FIX", "P1", "switch label not in the enclosing compound statement"),
    "7.1":   ("FIX", "P1", "octal constant"),
    "7.2":   ("FIX", "P2", "u suffix on unsigned constants"),
    "2.2":   ("FIX", "P1", "dead code"),
    "11.1":  ("FIX", "P1", "function pointer conversion is undefined"),
    "22.10": ("FIX", "P1", "errno tested after a non-errno-setting call"),
    "21.15": ("FIX", "P1", "memcpy pointer argument types"),
    "20.12": ("FIX", "P1", "macro parameter used with # and elsewhere"),
    "21.9":  ("FIX", "P2", "bsearch/qsort"),
    "9.3":   ("FIX", "P1", "partially initialized array"),
    "15.3":  ("FIX", "P1", "goto label scope"),
    "16.1":  ("FIX", "P1", "switch not well-formed"),
    "10.7":  ("FIX", "P3", "composite operand of a wider type"),

    # --- Advisory, not followed: MISRA 6.2.3, a note rather than a record --
    "15.5":  ("DECLINE", "", "single exit; CLAUDE.md prefers early returns"),
    "12.1":  ("DECLINE", "", "explicit precedence; style, no defect class"),
    "11.5":  ("DECLINE", "", "void* to object* is every allocator result"),
    "8.7":   ("DECLINE", "", "1,197 sites; revisit as a link-surface cleanup"),
    "18.4":  ("DECLINE", "", "pointer arithmetic is normal in a lexer and parser"),
    "13.3":  ("DECLINE", "", "++ with other side effects"),
    "5.9":   ("DECLINE", "", "internal linkage identifier uniqueness"),
    "19.2":  ("DECLINE", "", "unions carry the AST and value representation"),
    "17.8":  ("DECLINE", "", "parameter modified"),
    "20.10": ("DECLINE", "", "# and ## build the diagnostic and probe macros"),
    "12.3":  ("DECLINE", "", "comma operator"),
    "8.9":   ("DECLINE", "", "block scope for single-use objects"),
    "18.5":  ("DECLINE", "", "more than two pointer levels"),
    "15.4":  ("DECLINE", "", "more than one break or goto per loop"),
    "20.1":  ("DECLINE", "", "#include preceded by other than directives"),
    "11.4":  ("DECLINE", "", "pointer to integer conversion"),
    "20.5":  ("DECLINE", "", "#undef"),

    # --- Advisory, adopted, violations to fix ------------------------------
    "2.5":   ("FIX", "P2", "unused macro declarations; a cheap cleanup"),
    "2.7":   ("FIX", "P2", "unused parameters"),
    "2.3":   ("FIX", "P2", "unused type declarations"),
    "13.4":  ("FIX", "P1", "if (x = y) is a real defect class"),
    "2.4":   ("FIX", "P2", "unused tag declarations"),
    "15.1":  ("FIX", "P1", "one goto; remove it"),

    # --- Outside this edition ----------------------------------------------
    "1.4":   ("OUT", "", "added by a later amendment; no text in this edition"),
    "21.21": ("OUT", "", "added by a later amendment; no text in this edition"),

    # --- Proved to be checker defects, not code defects --------------------
    "7.3":   ("ADOPT", "", "kept enforced; the 384 reports are an addon regex bug"),
    "17.3":  ("ADOPT", "", "kept enforced; C23 makes a real violation a build error"),
}

# Directives. No tool checks any of them (MISRA 6.5).
DIRECTIVES = {
    "1.1":  ("ADOPT_OTHER", "", "docs/adr and docs/*-roadmap record the choices"),
    "2.1":  ("ADOPT", "", "make verify-core fails on any compilation error"),
    "3.1":  ("ADOPT_OTHER", "", "task-master and the roadmap exit gates"),
    "4.1":  ("ADOPT_OTHER", "", "valgrind, ASan, TSan probes and the parser fuzzer"),
    "4.2":  ("ADOPT", "", "one site, commented: the compiler barrier in crypto_secure_memzero, src/security/crypto_security.c:42"),
    "4.3":  ("ADOPT", "", "the one asm statement is isolated inside crypto_secure_memzero and emits no instructions"),
    "4.4":  ("ADOPT_OTHER", "", "code review"),
    "4.5":  ("ADOPT_OTHER", "", "code review; Rules 5.1-5.9 carry most of this"),
    "4.6":  ("DECLINE", "", "int and size_t are the house types, not sized typedefs"),
    "4.7":  ("ADOPT_OTHER", "", "paired with Rule 17.7; see deviation D-02"),
    "4.8":  ("DECLINE", "", "opaque types would fight the AST downcast pattern"),
    "4.9":  ("DECLINE", "", "the probe and diagnostic macros need to be macros"),
    "4.10": ("ADOPT", "", "every header carries an include guard"),
    "4.11": ("ADOPT_OTHER", "", "code review and the fuzzer"),
    "4.12": ("DEVIATE", "D-11", "dynamic memory allocation is the memory model"),
    "4.13": ("ADOPT_OTHER", "", "code review"),
    "4.14": ("ADOPT_OTHER", "", "the parser fuzzer is the main enforcement here"),
}

# A rule whose findings are split between a deviation and the fix backlog.
# The counts come from tools/split_17_7.py, which resolves each finding's
# column to the discarded callee. Re-measure after any change to D-02's list.
SPLIT = {
    "17.7": {"waived": 1709, "fix": 1516, "phase": "P1",
             "measured": "2026-08-14 by tools/split_17_7.py"},
}

RULE_ID_RE = re.compile(r"^misra-c2012-(\d+\.\d+)$")
TEXT_RE = re.compile(r"^(Rule|Dir) (\d+\.\d+) (Required|Advisory|Mandatory)$")
IMPL_RE = re.compile(r"^    def misra_(\d+)_(\d+)\(")
GENERATED = ("src/parser/parser.tab.c", "src/parser/parser.tab.h",
             "src/parser/parser.y")
ARTEFACTS = {"7.3", "17.3"}


def load(scan, rule_texts, addon):
    cats, texts = {}, {}
    lines = open(rule_texts, encoding="ascii").read().splitlines()
    for i, line in enumerate(lines):
        m = TEXT_RE.match(line.strip())
        if m:
            cats[(m.group(1), m.group(2))] = m.group(3)
            texts[(m.group(1), m.group(2))] = lines[i + 1].strip()

    implemented = set()
    for line in open(addon, encoding="utf-8", errors="replace"):
        m = IMPL_RE.match(line)
        if m:
            implemented.add("%s.%s" % (m.group(1), m.group(2)))

    fired = collections.Counter()
    for raw in open(scan, encoding="utf-8", errors="replace"):
        p = raw.rstrip("\n").split("|", 4)
        if len(p) == 5 and p[0] not in GENERATED:
            m = RULE_ID_RE.match(p[2])
            if m:
                fired[m.group(1)] += 1
    return cats, texts, implemented, fired


def decide(num, cat, implemented, fired):
    """Decision for one rule: explicit entry first, then the defaults."""
    if num in DECIDED:
        return DECIDED[num]
    if num in implemented:
        return ("ADOPT", "", "checked and clean")
    return ("ADOPT_OTHER", "", "no cppcheck check; review, sanitizer or fuzzer")


def main():
    scan, rule_texts, addon, out = sys.argv[1:5]
    cats, texts, implemented, fired = load(scan, rule_texts, addon)

    order = lambda n: tuple(int(x) for x in n.split("."))
    rules = sorted((n for k, n in cats if k == "Rule"), key=order)
    dirs = sorted((n for k, n in cats if k == "Dir"), key=order)

    rows = []
    for n in rules:
        d, tag, why = decide(n, cats[("Rule", n)], implemented, fired)
        rows.append(("Rule", n, cats[("Rule", n)], d, tag, why,
                     fired.get(n, 0), n in implemented))
    for n in dirs:
        d, tag, why = DIRECTIVES[n]
        rows.append(("Dir", n, cats[("Dir", n)], d, tag, why, 0, False))

    # --- arithmetic checks: a wrong total here would be published as policy
    problems = []
    for kind, n, cat, d, tag, why, hits, impl in rows:
        if cat == "Mandatory" and d in ("DEVIATE", "DECLINE"):
            problems.append("%s %s is Mandatory and cannot be deviated" % (kind, n))
        if cat == "Advisory" and d == "DEVIATE":
            problems.append("%s %s is Advisory: decline it, do not deviate" % (kind, n))
        if cat == "Required" and d == "DECLINE":
            problems.append("%s %s is Required: it needs a deviation record" % (kind, n))
        if d == "DEVIATE" and not tag:
            problems.append("%s %s deviates with no record id" % (kind, n))
    violated = {n for n in rules if fired.get(n, 0) and n not in ARTEFACTS}
    undecided = violated - set(DECIDED)
    if undecided:
        problems.append("violated but undecided: " + ", ".join(sorted(undecided)))
    if problems:
        sys.exit("policy table is inconsistent:\n  " + "\n  ".join(problems))

    # The tag column carries either a deviation id (D-nn) or a backlog phase
    # (Pn). They are different things and must not be counted together.
    dev_of = lambda t: t if t.startswith("D-") else ""
    phase_of = lambda t: t if t.startswith("P") else ""

    in_edition = [r for r in rows if r[3] != "OUT"]
    by = collections.Counter(r[3] for r in in_edition)
    devs = sorted({dev_of(r[4]) for r in rows if dev_of(r[4])})
    print("guidelines in this edition : %d (%d rules, %d directives)"
          % (len(in_edition),
             sum(1 for r in in_edition if r[0] == "Rule"), len(dirs)))
    print("outside this edition       : %d (%s)"
          % (len(rows) - len(in_edition),
             ", ".join(r[1] for r in rows if r[3] == "OUT")))
    for k in ("ADOPT", "ADOPT_OTHER", "FIX", "DEVIATE", "DECLINE"):
        print("  %-12s %4d" % (k, by[k]))
    print("adopted in total           : %d of %d"
          % (by["ADOPT"] + by["ADOPT_OTHER"] + by["FIX"], len(in_edition)))
    print("deviation records needed   : %d  (%s)" % (len(devs), ", ".join(devs)))
    def counts(kind, num, decision, hits):
        """Split one rule's findings into (to fix, waived). A rule in SPLIT is
        partly deviated and partly kept, so its two halves are measured."""
        s = SPLIT.get(num) if kind == "Rule" else None
        if s:
            if s["fix"] + s["waived"] != hits:
                sys.exit("SPLIT[%s] sums to %d but the scan reports %d. "
                         "Re-run tools/split_17_7.py."
                         % (num, s["fix"] + s["waived"], hits))
            return s["fix"], s["waived"]
        if decision == "FIX":
            return hits, 0
        if decision in ("DEVIATE", "DECLINE"):
            return 0, hits
        return 0, 0

    tally = {}
    for kind, n, cat, d, tag, why, hits, impl in rows:
        tally[(kind, n)] = counts(kind, n, d, hits)

    fixes = sum(f for f, _ in tally.values())
    waived = sum(w for _, w in tally.values())
    out_of = sum(r[6] for r in rows if r[3] == "OUT")
    print("violations to fix          : %d" % fixes)
    print("violations waived          : %d" % waived)
    print("violations out of scope    : %d" % out_of)
    print("total                      : %d" % (fixes + waived + out_of))
    for phase in ("P1", "P2", "P3"):
        sel = [r for r in rows
               if phase_of(r[4]) == phase
               or (SPLIT.get(r[1], {}).get("phase") == phase and r[0] == "Rule")]
        print("  %s: %2d rules, %6d violations"
              % (phase, len(sel), sum(tally[(r[0], r[1])][0] for r in sel)))

    with open(out, "w", encoding="utf-8") as f:
        f.write("kind\tnum\tcategory\tdecision\tdeviation\tphase"
                "\tviolations\tto_fix\twaived\tchecked\tnote\n")
        for kind, n, cat, d, tag, why, hits, impl in rows:
            n_fix, n_waived = tally[(kind, n)]
            phase = phase_of(tag) or (SPLIT.get(n, {}).get("phase", "")
                                      if kind == "Rule" else "")
            f.write("%s\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%s\t%s\n"
                    % (kind, n, cat, d, dev_of(tag), phase, hits,
                       n_fix, n_waived, "yes" if impl else "no", why))
    print("\nwrote %s" % out)


if __name__ == "__main__":
    main()
