#!/usr/bin/env python3
"""Aggregate a cppcheck MISRA scan of the Goo compiler into a report.

Three things this does that a bare count does not:

1. It splits hand-written code from bison-generated code. MISRA section 6.8 and
   Appendix E treat automatically generated code separately, and parser.tab.c
   is not code anyone edits.
2. It quarantines rules proved to be tool artefacts, with the evidence recorded
   next to them, instead of letting them dominate the headline number.
3. It reports what the scan could NOT check. A translation unit that fails to
   parse yields zero findings, which looks exactly like a clean file.
"""
import collections
import re
import sys

RULE_ID_RE = re.compile(r"^misra-c2012-(\d+\.\d+)$")
TEXT_RE = re.compile(r"^(Rule|Dir) (\d+\.\d+) (Required|Advisory|Mandatory)$")
PARSE_FAIL_IDS = {"syntaxError", "internalAstError", "cppcheckError",
                  "internalError", "preprocessorErrorDirective"}

# Bison writes these, and its #line directives make cppcheck attribute findings
# back to parser.y, which is a grammar file rather than C.
GENERATED = ("src/parser/parser.tab.c", "src/parser/parser.tab.h",
             "src/parser/parser.y")

# Rules whose findings were checked by hand and traced to a defect in the
# checker, not in this codebase. Each entry has to carry its evidence.
ARTEFACTS = {
    "7.3": "addon regex ^(0[xX])?[0-9a-fA-FpP.]+[Uu]*l+[Uu]*$ also matches the "
           "identifiers call(357), decl(90), al, bl; only 2 of 384 tokens are "
           "real numeric literals",
    "17.3": "flagged calls are atomic_* (197), pthread_cond_* (87), curl_* (73) "
            "and json_object_* - declared functions from libraries cppcheck has "
            "no config for. C23 makes a true implicit declaration an error, so a "
            "build that passes -std=c23 -Wall -Wextra cannot contain one. Adding "
            "--library=posix,gnu already cut this from 1666 to 660.",
}


def load_categories(path):
    cats, texts = {}, {}
    lines = open(path, encoding="ascii").read().splitlines()
    for i, line in enumerate(lines):
        m = TEXT_RE.match(line.strip())
        if m:
            cats[m.group(2)] = m.group(3)
            texts[m.group(2)] = lines[i + 1].strip() if i + 1 < len(lines) else ""
    return cats, texts


def area(path):
    parts = path.split("/")
    if parts[0] == "src" and len(parts) > 2:
        return "src/" + parts[1]
    if parts[0] == "src":
        return "src/(top level)"
    return parts[0]


def table(title, rows, cats, texts):
    print()
    print("=" * 78)
    print(title)
    print("=" * 78)
    if not rows:
        print("  (none)")
        return
    print("%-9s %-10s %8s  %s" % ("RULE", "CATEGORY", "COUNT", "TEXT"))
    for rule, n in rows:
        print("%-9s %-10s %8d  %s"
              % (rule, cats.get(rule, "?"), n, texts.get(rule, "")[:70]))


def main():
    scan, rule_texts = sys.argv[1], sys.argv[2]
    cats, texts = load_categories(rule_texts)

    hand, gen = [], []
    parse_fail, checked, other = [], set(), collections.Counter()
    exit_code = None

    for raw in open(scan, encoding="utf-8", errors="replace"):
        line = raw.rstrip("\n")
        if line.startswith("Checking "):
            checked.add(line[len("Checking "):].split(" ...")[0].split(":")[0])
            continue
        if line.startswith("SCAN_EXIT="):
            exit_code = line.split("=", 1)[1]
            continue
        parts = line.split("|", 4)
        if len(parts) != 5:
            continue
        f, ln, rid, sev, msg = parts
        m = RULE_ID_RE.match(rid)
        if not m:
            other[rid] += 1
            if rid in PARSE_FAIL_IDS:
                parse_fail.append((f, ln, rid, msg))
            continue
        (gen if f in GENERATED else hand).append((f, ln, m.group(1)))

    print("=" * 78)
    print("SCAN COVERAGE  (this decides whether any count below means anything)")
    print("=" * 78)
    print("cppcheck exit code                : %s" % exit_code)
    print("translation units checked         : %d" % len(checked))
    print("parse / preprocessor failures     : %d" % len(parse_fail))
    for f, ln, rid, msg in parse_fail[:10]:
        print("    %s:%s  %s  %s" % (f, ln, rid, msg[:60]))
    print("findings in hand-written code     : %d" % len(hand))
    print("findings in bison-generated code  : %d" % len(gen))

    real = [(f, l, r) for f, l, r in hand if r not in ARTEFACTS]
    quarantined = [(f, l, r) for f, l, r in hand if r in ARTEFACTS]

    by_rule = collections.Counter(r for _, _, r in real)
    by_cat = collections.Counter(cats.get(r, "?") for _, _, r in real)

    print()
    print("=" * 78)
    print("HEADLINE  (hand-written code, tool artefacts removed)")
    print("=" * 78)
    print("violations                        : %d" % len(real))
    # The denominator is what the addon can check, not what MISRA defines.
    # misra.py implements 130 rule checks and no directive checks, so 43 of the
    # 173 guidelines - every Dir among them - were never examined.
    print("distinct guidelines violated      : %d of the 130 rules the addon "
          "implements" % len(by_rule))
    print("guidelines never examined         : 43 (17 directives + 26 rules)")
    print("files with at least one violation : %d" % len({f for f, _, _ in real}))
    for cat in ("Mandatory", "Required", "Advisory"):
        n = len({r for _, _, r in real if cats.get(r) == cat})
        print("  %-10s %8d violations across %2d guidelines"
              % (cat, by_cat[cat], n))

    for cat in ("Mandatory", "Required", "Advisory"):
        rows = [(r, n) for r, n in by_rule.most_common() if cats.get(r) == cat]
        table("%s GUIDELINES VIOLATED (%d)" % (cat.upper(), len(rows)),
              rows, cats, texts)

    print()
    print("=" * 78)
    print("QUARANTINED - COUNTED BUT NOT BELIEVED")
    print("=" * 78)
    for rule, why in sorted(ARTEFACTS.items()):
        n = sum(1 for _, _, r in quarantined if r == rule)
        print("Rule %-6s %6d findings  [%s]" % (rule, n, cats.get(rule, "?")))
        print("    reason: %s" % why)

    print()
    print("=" * 78)
    print("BISON-GENERATED CODE (reported separately, MISRA section 6.8)")
    print("=" * 78)
    for rule, n in collections.Counter(r for _, _, r in gen).most_common(8):
        print("Rule %-6s %6d  %s" % (rule, n, texts.get(rule, "")[:56]))

    print()
    print("=" * 78)
    print("BY AREA (hand-written, artefacts removed)")
    print("=" * 78)
    for a, n in collections.Counter(area(f) for f, _, _ in real).most_common():
        print("%-28s %8d" % (a, n))

    print()
    print("=" * 78)
    print("TOP 12 FILES")
    print("=" * 78)
    for f, n in collections.Counter(f for f, _, _ in real).most_common(12):
        print("%-58s %8d" % (f, n))


if __name__ == "__main__":
    main()
