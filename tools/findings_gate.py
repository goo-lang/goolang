#!/usr/bin/env python3
"""findings_gate.py -- shrink-only gate over one merged SARIF findings file.

The generalisation of docs/misra/tools/gate.py the task text asks for:
that file tallies cppcheck's own (file, rule) pairs from ONE tool's plain
text; this tallies (tool, rule, file, fingerprint) from tools/sarif_merge.py's
merged SARIF across every tool at once, using its stable "goo/v1" fingerprint
in place of a line number for the same reason gate.py's own tally() drops
line numbers -- see that file's docstring, and sarif_merge.py's.

    findings_gate.py check <sarif> <baseline.tsv> <suppressions.tsv>
    findings_gate.py write <sarif> <baseline.tsv> <suppressions.tsv>

check exits 1 on a regression (a key present now, absent from the baseline).
write records the current, non-suppressed keys as the new baseline.

SUPPRESSIONS. <suppressions.tsv>: tool<TAB>rule<TAB>file-glob<TAB>justification,
one per line (blank lines and "#" comments skipped). A suppressed
(tool, rule, file) triple is dropped from BOTH sides of the comparison, so
it can never appear as a regression OR an improvement -- it is out of scope,
the same way a cppcheck --suppressions-list entry never reaches
docs/misra/tools/gate.py's own tally at all. Generalises docs/misra/
misra-suppressions.txt's policy across every tool: every justification must
be "D-NN" (a deviation record under docs/misra/deviations/) or
"defect:<tool>:<id>" (a documented checker defect) or the row -- and
therefore the whole gate run -- is red. A suppression with no justification
is not a suppression, it is silence.
"""
import collections
import fnmatch
import json
import os
import re
import sys
import tempfile

JUSTIFICATION_RE = re.compile(r"^(D-\d+|defect:[^:\s]+:.+)$")


def tally(sarif_path):
    """Returns (keys, per_tool) where keys is a set of
    (tool, rule, file, fingerprint) 4-tuples and per_tool is a
    {tool_name: result_count} Counter, for the verdict line."""
    with open(sarif_path, encoding="utf-8") as f:
        doc = json.load(f)
    keys = set()
    per_tool = collections.Counter()
    for run in doc.get("runs", []):
        tool = run.get("tool", {}).get("driver", {}).get("name", "unknown")
        for result in run.get("results", []):
            rule = result.get("ruleId", "")
            file_ = ""
            locations = result.get("locations") or []
            if locations:
                art = locations[0].get("physicalLocation", {}).get("artifactLocation", {})
                file_ = art.get("uri", "")
            fp = result.get("fingerprints", {}).get("goo/v1", "")
            keys.add((tool, rule, file_, fp))
            per_tool[tool] += 1
    return keys, per_tool


def load_suppressions(path):
    """Returns (rows, bad) where rows is a list of
    (tool, rule, file_glob, justification) and bad is a list of
    (line_no, raw_line, reason) for every row this gate must refuse."""
    rows, bad = [], []
    if not os.path.isfile(path):
        return rows, bad
    with open(path, encoding="utf-8") as f:
        for lineno, raw in enumerate(f, start=1):
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 4:
                bad.append((lineno, line, "expected 4 tab-separated columns (tool, rule, file-glob, justification)"))
                continue
            tool, rule, file_glob, justification = parts
            if not JUSTIFICATION_RE.match(justification):
                bad.append((lineno, line, "justification %r is neither D-NN nor defect:<tool>:<id>" % justification))
                continue
            rows.append((tool, rule, file_glob, justification))
    return rows, bad


def is_suppressed(tool, rule, file_, suppressions):
    for s_tool, s_rule, s_glob, _just in suppressions:
        if s_tool != "*" and s_tool != tool:
            continue
        if not fnmatch.fnmatch(rule, s_rule):
            continue
        if not fnmatch.fnmatch(file_, s_glob):
            continue
        return True
    return False


def filter_suppressed(keys, suppressions):
    return {k for k in keys if not is_suppressed(k[0], k[1], k[2], suppressions)}


def read_baseline(path):
    keys = set()
    if not os.path.isfile(path):
        return keys
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) == 4:
                keys.add(tuple(parts))
    return keys


def write_baseline(path, keys, sarif_path):
    with open(path, "w", encoding="utf-8") as f:
        f.write(
            "# findings-probe SHRINK-ONLY baseline. Regenerate with:\n"
            "#   scripts/findings_probe.sh --update-baseline\n"
            "# One line per tool<TAB>rule<TAB>file<TAB>fingerprint (tools/sarif_merge.py's\n"
            "# own stable \"goo/v1\" hash -- rule id + repo-relative file + the finding's\n"
            "# own normalised line text, NOT its line number).\n"
            "# A key here and absent from a later run is an IMPROVEMENT.\n"
            "# A key present in a later run and absent here is a REGRESSION and fails\n"
            "# the gate. Suppressed keys (docs/misra/findings-suppressions.tsv) never\n"
            "# appear here at all. Never edit by hand.\n"
            "# %d accepted findings, from %s.\n" % (len(keys), sarif_path)
        )
        for k in sorted(keys):
            f.write("\t".join(k) + "\n")


def check(sarif_path, baseline_path, suppressions_path):
    """Returns (rc, lines) -- lines is the full report, ready to print."""
    rows, bad = load_suppressions(suppressions_path)
    out = []
    if bad:
        out.append("findings-gate: FAIL -- %d suppression row(s) in %s have no valid justification:"
                    % (len(bad), suppressions_path))
        for lineno, line, reason in bad:
            out.append("    line %d: %s (%s)" % (lineno, line, reason))
        return 1, out

    now_keys, per_tool = tally(sarif_path)
    base_keys = read_baseline(baseline_path)
    now_f = filter_suppressed(now_keys, rows)
    base_f = filter_suppressed(base_keys, rows)

    regressions = sorted(now_f - base_f)
    improvements = sorted(base_f - now_f)

    if improvements:
        out.append("findings-gate: %d finding(s) fixed since the baseline. Tighten it with --update-baseline."
                    % len(improvements))
        for tool, rule, file_, fp in improvements[:10]:
            out.append("    %-16s %-40s %s (%s)" % (tool, rule, file_, fp[:12]))

    per_tool_str = ", ".join("%s: %d" % (n, c) for n, c in sorted(per_tool.items()))
    verdict = ("%s: %d findings (%s), %d suppressed, baseline %s (%d accepted)"
               % (sarif_path, sum(per_tool.values()), per_tool_str, len(now_keys) - len(now_f),
                  baseline_path, len(base_keys)))

    if regressions:
        out.append("findings-gate: FAIL -- %d new finding(s) not in the baseline" % len(regressions))
        for tool, rule, file_, fp in regressions[:20]:
            out.append("    %-16s %-40s %s (%s)" % (tool, rule, file_, fp[:12]))
        out.append("findings-gate: FAIL %s" % verdict)
        return 1, out

    out.append("findings-gate: PASS %s" % verdict)
    return 0, out


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) != 4 or argv[0] not in ("check", "write"):
        sys.exit(__doc__)
    mode, sarif_path, baseline_path, suppressions_path = argv

    if mode == "write":
        rows, bad = load_suppressions(suppressions_path)
        if bad:
            print("findings-gate: FAIL -- %d suppression row(s) in %s have no valid justification:"
                  % (len(bad), suppressions_path))
            for lineno, line, reason in bad:
                print("    line %d: %s (%s)" % (lineno, line, reason))
            return 1
        keys, _ = tally(sarif_path)
        write_baseline(baseline_path, filter_suppressed(keys, rows), sarif_path)
        print("findings-gate: wrote %s -- %d accepted findings (from %s)"
              % (baseline_path, len(filter_suppressed(keys, rows)), sarif_path))
        return 0

    rc, lines = check(sarif_path, baseline_path, suppressions_path)
    print("\n".join(lines))
    return rc


# --self-test: four cases, matching scripts/findings_probe.sh's own --self-test
# (that script drives this module through the shell entry point; this direct
# call proves the module's own logic in isolation).
def self_test():
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        def write_sarif(path, results):
            doc = {"version": "2.1.0", "runs": [{
                "tool": {"driver": {"name": "GNU C23"}},
                "results": results,
            }]}
            with open(path, "w", encoding="utf-8") as f:
                json.dump(doc, f)

        def result(rule, file_, fp):
            return {"ruleId": rule, "message": {"text": "x"},
                    "locations": [{"physicalLocation": {"artifactLocation": {"uri": file_}}}],
                    "fingerprints": {"goo/v1": fp}}

        baseline_path = os.path.join(tmp, "baseline.tsv")
        write_baseline(baseline_path, {("GNU C23", "r1", "a.c", "fp1")}, "seed")

        # (a) a result NOT in the baseline -> RED.
        sarif_a = os.path.join(tmp, "a.sarif")
        write_sarif(sarif_a, [result("r1", "a.c", "fp1"), result("r2", "b.c", "fp2")])
        suppressions_empty = os.path.join(tmp, "empty-suppressions.tsv")
        open(suppressions_empty, "w", encoding="utf-8").close()
        rc, _ = check(sarif_a, baseline_path, suppressions_empty)
        if rc == 1:
            print("    ok: a finding not in the baseline is RED")
        else:
            print("FAIL: a new finding did not go RED (rc=%d)" % rc)
            ok = False

        # (b) a suppression row with an empty justification -> RED, even
        # though the finding it would have suppressed is otherwise clean.
        bad_suppressions = os.path.join(tmp, "bad-suppressions.tsv")
        with open(bad_suppressions, "w", encoding="utf-8") as f:
            f.write("GNU C23\tr2\tb.c\t\n")
        rc, lines = check(sarif_a, baseline_path, bad_suppressions)
        if rc == 1 and any("no valid justification" in l for l in lines):
            print("    ok: a suppression row with an empty justification is RED")
        else:
            print("FAIL: an empty-justification suppression row did not go RED: %r" % lines)
            ok = False

        # (c) a merged file that fails schema validation -> RED. This
        # module has no schema logic of its own (tools/sarif_merge.py
        # --validate-only owns that); scripts/findings_probe.sh's own
        # --self-test drives that check directly. Documented here so the
        # four cases stay numbered the same in both places.
        print("    ok: (c) schema validation is exercised by scripts/findings_probe.sh's own self-test")

        # (d) the SAME sarif, now WITH a suppression that actually matches
        # r2 (a real justification this time) -> only r1 is compared, which
        # is already in the baseline -> GREEN.
        real_suppressions = os.path.join(tmp, "real-suppressions.tsv")
        with open(real_suppressions, "w", encoding="utf-8") as f:
            f.write("GNU C23\tr2\tb.c\tD-01\n")
        rc, lines = check(sarif_a, baseline_path, real_suppressions)
        if rc == 0:
            print("    ok: suppressing the new finding's (tool, rule, file) with a valid justification is GREEN")
        else:
            print("FAIL: a validly-suppressed new finding still went RED: %r" % lines)
            ok = False

    if ok:
        print("findings_gate.py --self-test: PASS")
        return 0
    print("findings_gate.py --self-test: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
