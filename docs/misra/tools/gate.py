#!/usr/bin/env python3
"""MISRA regression gate: fail on a NEW violation, not on the existing ones.

The codebase carries 8,675 accepted violations across the adopted guidelines.
A gate that fails on all of them would be switched off within a day. This one
records the current count per (file, rule) and fails only when a count rises or
a new pair appears.

    gate.py check   <scan-output> <baseline.tsv>   # exit 1 on a regression
    gate.py write   <scan-output> <baseline.tsv>   # record a new baseline

It also reports counts that FELL, so the baseline can be tightened after a fix.
A baseline that is never tightened stops being a gate and becomes a rug.
"""
import collections
import re
import sys

RULE_ID_RE = re.compile(r"^misra-c2012-(\d+\.\d+)$")


def tally(scan):
    """Count violations per (file, rule). Line numbers move on every edit, so
    they cannot be part of the key without making the baseline churn.

    Refuses a scan that did not actually run. cppcheck exits non-zero and
    checks nothing on a configuration error - a duplicate suppression is
    enough - and the resulting empty output is indistinguishable from a clean
    tree unless the run itself is validated first."""
    counts = collections.Counter()
    checked, exit_code = 0, None
    for raw in open(scan, encoding="utf-8", errors="replace"):
        line = raw.rstrip("\n")
        if line.startswith("Checking "):
            checked += 1
            continue
        if line.startswith("SCAN_EXIT="):
            exit_code = line.split("=", 1)[1].strip()
            continue
        if line.startswith("cppcheck: error:"):
            sys.exit("gate: the scan reported a configuration error:\n  %s"
                     % line)
        parts = line.split("|", 4)
        if len(parts) != 5:
            continue
        m = RULE_ID_RE.match(parts[2])
        if m:
            counts[(parts[0], m.group(1))] += 1

    if exit_code not in (None, "0"):
        sys.exit("gate: the scan exited %s. Refusing to trust its output."
                 % exit_code)
    if checked == 0:
        sys.exit("gate: %s records no 'Checking <file>' lines, so no "
                 "translation unit was analysed. A comparison against nothing "
                 "always passes; refusing to run." % scan)
    return counts


def read_baseline(path):
    base = collections.Counter()
    try:
        fh = open(path, encoding="utf-8")
    except OSError:
        sys.exit("gate: no baseline at %s. Run 'gate.py write' first." % path)
    with fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            f, rule, n = line.split("\t")
            base[(f, rule)] = int(n)
    return base


def write_baseline(counts, path, scan):
    with open(path, "w", encoding="utf-8") as f:
        f.write("# MISRA gate baseline. Regenerate with:\n"
                "#   docs/misra/tools/gate.py write <scan-output> %s\n"
                "# One line per file and rule: file<TAB>rule<TAB>count.\n"
                "# Counts may only go DOWN. gate.py check fails on any rise.\n"
                "# Total accepted violations: %d across %d file/rule pairs.\n"
                % (path, sum(counts.values()), len(counts)))
        for (fn, rule), n in sorted(counts.items()):
            f.write("%s\t%s\t%d\n" % (fn, rule, n))
    print("gate: wrote %s - %d violations across %d file/rule pairs (from %s)"
          % (path, sum(counts.values()), len(counts), scan))


def check(counts, base):
    regressions, improvements = [], []
    for key in sorted(set(counts) | set(base)):
        now, was = counts.get(key, 0), base.get(key, 0)
        if now > was:
            regressions.append((key, was, now))
        elif now < was:
            improvements.append((key, was, now))

    if improvements:
        got = sum(w - n for _, w, n in improvements)
        print("gate: %d violations fixed since the baseline, in %d file/rule "
              "pairs. Tighten it with 'gate.py write'." % (got, len(improvements)))
        for (fn, rule), was, now in improvements[:10]:
            print("    %-52s rule %-6s %d -> %d" % (fn, rule, was, now))

    if not regressions:
        print("gate: PASS - %d violations, none new against the baseline."
              % sum(counts.values()))
        return 0

    print("\ngate: FAIL - %d new MISRA violations" % sum(
        n - w for _, w, n in regressions))
    for (fn, rule), was, now in regressions:
        print("    %-52s rule %-6s %d -> %d" % (fn, rule, was, now))
    print("\nEither fix them, or - if the guideline should not apply - add the "
          "decision to docs/misra/tools/policy.py and a record under "
          "docs/misra/deviations/ before touching the baseline.")
    return 1


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("check", "write"):
        sys.exit(__doc__)
    mode, scan, baseline = sys.argv[1:4]
    counts = tally(scan)
    if mode == "write":
        write_baseline(counts, baseline, scan)
        return 0
    return check(counts, read_baseline(baseline))


if __name__ == "__main__":
    sys.exit(main())
