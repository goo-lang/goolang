#!/usr/bin/env python3
"""findings_gate.py -- shrink-only gate over one merged SARIF findings file.

The generalisation of docs/misra/tools/gate.py the task text asks for:
that file tallies cppcheck's own (file, rule) pairs from ONE tool's plain
text; this tallies (tool, rule, file, fingerprint) from tools/sarif_merge.py's
merged SARIF across every tool at once, using its stable "goo/v1" fingerprint
in place of a line number for the same reason gate.py's own tally() drops
line numbers -- see that file's docstring, and sarif_merge.py's.

    findings_gate.py check <sarif> <baseline-prefix> <suppressions.tsv>
    findings_gate.py write <sarif> <baseline-prefix> <suppressions.tsv>

check exits 1 on a regression (a key present now, absent from the baseline).
write records the current, non-suppressed keys as the new baseline.

FIX ROUND 1 (I2b) -- THE BASELINE IS KEYED BY TOOLCHAIN SIGNATURE.
<baseline-prefix> is a path prefix, not a file. The gate derives a
toolchain signature from the merged SARIF's own tool.driver versions
(gcc<major>-tidy<major>-cppcheck<major>.<minor>, for example
gcc16-tidy23-cppcheck2.22) and reads <prefix>.<signature>.tsv. MEASURED
(2026-11-08): a toolchain upgrade from LLVM 22 / Cppcheck 2.21 to LLVM 23 /
Cppcheck 2.22 moved clang-tidy by -3 and Cppcheck by +2 baseline rows with
no code change. A baseline for one toolchain is therefore not evidence about
another. When no file matches the current signature the gate FAILS LOUDLY,
names the exact command to make one, and writes a CANDIDATE baseline for
that signature next to the SARIF (under build/) so CI can upload it as an
artifact for a person to commit. It never passes against a missing
baseline, and it never compares against a baseline of a different signature.

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

FIX ROUND 1 (I1) -- THE COMPARISON KEY IS A COUNT, NOT A SET MEMBERSHIP.
The task's own plan text ("rule, file and fingerprint as the key") and this
module's first cut both compared the RAW SET of (tool, rule, file,
fingerprint) 4-tuples. MEASURED: editing only the TEXT of one already-
accepted finding (its line unchanged, its content edited) changes its
fingerprint, so the OLD 4-tuple vanishes and a NEW one appears for the SAME
(tool, rule, file) -- the set comparison reported this as one improvement
(the old fingerprint) and one regression (the new one) FOR THE SAME
UNDERLYING FINDING, and exited 1. That is nonsense: nothing regressed.

The fix: the gate compares the COUNT of accepted findings per
(tool, rule, file), shrink-only -- a count ABOVE the baseline's is red, EQUAL
or LOWER is green, and a drop is reported as an improvement. Fingerprints
still travel in the SARIF and in every baseline row (one row per finding,
same 4 columns as before), but they are used ONLY to NAME which specific
lines are new in a regression's printed output -- never as part of the
comparison itself. Editing one finding's text now correctly stays GREEN,
because the (tool, rule, file) count does not move.
"""
import collections
import fnmatch
import json
import os
import re
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JUSTIFICATION_RE = re.compile(r"^(D-\d+|defect:[^:\s]+:.+)$")
DEVIATIONS_DIR = os.path.join(REPO_ROOT, "docs", "misra", "deviations")
UPDATE_COMMAND = "scripts/findings_probe.sh --update-baseline"


def _major(version):
    m = re.match(r"\s*(\d+)", version or "")
    return m.group(1) if m else None


def _major_minor(version):
    m = re.match(r"\s*(\d+)\.(\d+)", version or "")
    return "%s.%s" % m.groups() if m else None


def toolchain_signature(sarif_path):
    """gcc<major>-tidy<major>-cppcheck<major>.<minor>, read from the merged
    SARIF's own driver versions -- never from the machine, so the signature
    names the toolchain that PRODUCED the findings. A tool absent from the
    file, or with an unparseable version, contributes '<tool>unknown', which
    matches no committed baseline and therefore fails loudly."""
    with open(sarif_path, encoding="utf-8") as f:
        doc = json.load(f)
    gcc = tidy = cppcheck = None
    for run in doc.get("runs", []):
        driver = run.get("tool", {}).get("driver", {})
        name = driver.get("name")
        version = driver.get("semanticVersion") or driver.get("version")
        if name == "GNU C23":
            gcc = _major(version)
        elif name == "clang-tidy":
            tidy = _major(version)
        elif name == "Cppcheck":
            cppcheck = _major_minor(version)
    return "gcc%s-tidy%s-cppcheck%s" % (gcc or "unknown", tidy or "unknown", cppcheck or "unknown")


def baseline_file(prefix, signature):
    return "%s.%s.tsv" % (prefix, signature)


def tally(sarif_path):
    """Returns (keys, per_tool) where keys is a set of
    (tool, rule, file, fingerprint) 4-tuples (still one entry per finding,
    used to compute per-(tool,rule,file) COUNTS and to name specific new
    fingerprints in check()'s output -- see this module's own docstring,
    FIX ROUND 1 / I1, on why the key itself is no longer the comparison
    unit) and per_tool is a {tool_name: result_count} Counter, for the
    verdict line.

    FIX ROUND 1 (M6): a tool's entry is registered on EVERY run, even one
    with zero results (`per_tool[tool] += 0` still creates the Counter key)
    -- before this fix, a run with no results (verify-core-gates on an
    all-green pipeline, for example) was silently absent from per_tool
    entirely, so the verdict line named four tools when five actually ran."""
    with open(sarif_path, encoding="utf-8") as f:
        doc = json.load(f)
    keys = set()
    per_tool = collections.Counter()
    for run in doc.get("runs", []):
        tool = run.get("tool", {}).get("driver", {}).get("name", "unknown")
        per_tool[tool] += 0
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


def group_by_triple(keys):
    """(tool, rule, file, fingerprint) 4-tuples -> {(tool, rule, file): set
    of fingerprints}. len() of a triple's set is its ACCEPTED COUNT -- the
    comparison unit after FIX ROUND 1 / I1."""
    grouped = collections.defaultdict(set)
    for tool, rule, file_, fp in keys:
        grouped[(tool, rule, file_)].add(fp)
    return grouped


def load_suppressions(path, deviations_dir=None):
    """Returns (rows, bad) where rows is a list of
    (tool, rule, file_glob, justification) and bad is a list of
    (line_no, raw_line, reason) for every row this gate must refuse.

    FIX ROUND 1 (I3): a "D-NN" justification must name a record that EXISTS
    as <deviations_dir>/D-NN.md. The shape alone let D-99 through."""
    if deviations_dir is None:
        deviations_dir = DEVIATIONS_DIR
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
            if justification.startswith("D-") and \
                    not os.path.isfile(os.path.join(deviations_dir, justification + ".md")):
                bad.append((lineno, line, "deviation record %s.md does not exist under %s"
                            % (justification, deviations_dir)))
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
    # M5: a baseline must not record the machine. sarif_path arrives as an
    # ABSOLUTE path from every real caller (scripts/findings_probe.sh always
    # passes $REPO_ROOT/build/findings.sarif), and that absolute path used
    # to be written verbatim into this committed file's header comment --
    # leaking one worktree's own filesystem layout into version control.
    # Recorded relative to REPO_ROOT instead, the same way every other path
    # in this file already is.
    display_path = os.path.relpath(sarif_path, REPO_ROOT) if os.path.isabs(sarif_path) else sarif_path
    with open(path, "w", encoding="utf-8") as f:
        f.write(
            "# findings-probe SHRINK-ONLY baseline. Regenerate with:\n"
            "#   scripts/findings_probe.sh --update-baseline\n"
            "# One line per tool<TAB>rule<TAB>file<TAB>fingerprint (tools/sarif_merge.py's\n"
            "# own stable \"goo/v1\" hash -- rule id + repo-relative file + the finding's\n"
            "# own normalised line text, NOT its line number).\n"
            "# The GATE compares COUNTS per (tool, rule, file) -- see this module's own\n"
            "# docstring, FIX ROUND 1 / I1. The fingerprint column exists to NAME which\n"
            "# specific lines are new when a count rises; it is not itself the key.\n"
            "# A (tool, rule, file) whose count here is above a later run's is an\n"
            "# IMPROVEMENT. A count above what is recorded here is a REGRESSION and fails\n"
            "# the gate. Suppressed rows (docs/misra/findings-suppressions.tsv) never\n"
            "# appear here at all. Never edit by hand.\n"
            "# %d accepted findings, from %s.\n" % (len(keys), display_path)
        )
        for k in sorted(keys):
            f.write("\t".join(k) + "\n")


def check(sarif_path, baseline_prefix, suppressions_path, deviations_dir=None):
    """Returns (rc, lines) -- lines is the full report, ready to print.

    FIX ROUND 1 (I1): compares the COUNT of accepted fingerprints per
    (tool, rule, file), not raw 4-tuple set membership -- see this module's
    own docstring for the measured reason. A regression's printed line still
    names the SPECIFIC new fingerprints (now-minus-baseline, within that
    triple), for a human to find the actual new line; that naming is
    cosmetic and never affects the red/green decision."""
    rows, bad = load_suppressions(suppressions_path, deviations_dir)
    out = []
    if bad:
        out.append("findings-gate: FAIL -- %d suppression row(s) in %s have no valid justification:"
                    % (len(bad), suppressions_path))
        for lineno, line, reason in bad:
            out.append("    line %d: %s (%s)" % (lineno, line, reason))
        return 1, out

    now_keys, per_tool = tally(sarif_path)
    signature = toolchain_signature(sarif_path)
    baseline_path = baseline_file(baseline_prefix, signature)
    if not os.path.isfile(baseline_path):
        candidate = os.path.join(os.path.dirname(os.path.abspath(sarif_path)),
                                 os.path.basename(baseline_path))
        write_baseline(candidate, filter_suppressed(now_keys, rows), sarif_path)
        out.append("findings-gate: FAIL -- no baseline for toolchain signature %s (%s is absent)."
                    % (signature, baseline_path))
        out.append("    A baseline for another toolchain is not evidence about this one.")
        out.append("    A candidate baseline was written to %s." % candidate)
        out.append("    To accept it: run `%s` on this toolchain and commit the file, or copy the candidate to %s."
                    % (UPDATE_COMMAND, baseline_path))
        return 1, out
    base_keys = read_baseline(baseline_path)
    now_f = filter_suppressed(now_keys, rows)
    base_f = filter_suppressed(base_keys, rows)

    now_triples = group_by_triple(now_f)
    base_triples = group_by_triple(base_f)

    regressions, improvements = [], []
    for triple in sorted(set(now_triples) | set(base_triples)):
        now_fps = now_triples.get(triple, set())
        base_fps = base_triples.get(triple, set())
        now_c, base_c = len(now_fps), len(base_fps)
        if now_c > base_c:
            regressions.append((triple, base_c, now_c, sorted(now_fps - base_fps)))
        elif now_c < base_c:
            improvements.append((triple, base_c, now_c, sorted(base_fps - now_fps)))

    if improvements:
        out.append("findings-gate: %d (tool, rule, file) group(s) shrank since the baseline. "
                    "Tighten it with --update-baseline." % len(improvements))
        for (tool, rule, file_), base_c, now_c, fps in improvements[:10]:
            out.append("    %-16s %-40s %-40s %d -> %d" % (tool, rule, file_, base_c, now_c))

    per_tool_str = ", ".join("%s: %d" % (n, c) for n, c in sorted(per_tool.items()))
    verdict = ("%s: %d findings (%s), %d suppressed, toolchain %s, baseline %s (%d accepted)"
               % (sarif_path, sum(per_tool.values()), per_tool_str, len(now_keys) - len(now_f),
                  signature, baseline_path, len(base_keys)))

    if regressions:
        out.append("findings-gate: FAIL -- %d (tool, rule, file) group(s) rose above the baseline"
                    % len(regressions))
        for (tool, rule, file_), base_c, now_c, fps in regressions[:20]:
            new_str = ", ".join(fp[:12] for fp in fps[:3])
            out.append("    %-16s %-40s %-40s %d -> %d  new: %s"
                        % (tool, rule, file_, base_c, now_c, new_str))
        out.append("findings-gate: FAIL %s" % verdict)
        return 1, out

    out.append("findings-gate: PASS %s" % verdict)
    return 0, out


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) != 4 or argv[0] not in ("check", "write"):
        sys.exit(__doc__)
    mode, sarif_path, baseline_prefix, suppressions_path = argv

    if mode == "write":
        rows, bad = load_suppressions(suppressions_path)
        if bad:
            print("findings-gate: FAIL -- %d suppression row(s) in %s have no valid justification:"
                  % (len(bad), suppressions_path))
            for lineno, line, reason in bad:
                print("    line %d: %s (%s)" % (lineno, line, reason))
            return 1
        keys, _ = tally(sarif_path)
        baseline_path = baseline_file(baseline_prefix, toolchain_signature(sarif_path))
        write_baseline(baseline_path, filter_suppressed(keys, rows), sarif_path)
        print("findings-gate: wrote %s -- %d accepted findings (from %s)"
              % (baseline_path, len(filter_suppressed(keys, rows)), sarif_path))
        return 0

    rc, lines = check(sarif_path, baseline_prefix, suppressions_path)
    print("\n".join(lines))
    return rc


# --self-test: four cases, matching scripts/findings_probe.sh's own --self-test
# (that script drives this module through the shell entry point; this direct
# call proves the module's own logic in isolation).
def self_test():
    ok = True
    sig = "gcc16-tidy23-cppcheck2.22"
    with tempfile.TemporaryDirectory() as tmp:
        def drivers(extra_tool_results=()):
            # The three drivers the signature reads; GNU C23 carries the results.
            return [
                {"tool": {"driver": {"name": "GNU C23", "version": "16.2.1 20261029"}},
                 "results": list(extra_tool_results)},
                {"tool": {"driver": {"name": "clang-tidy", "version": "23.1.4"}}, "results": []},
                {"tool": {"driver": {"name": "Cppcheck", "semanticVersion": "2.22.0"}}, "results": []},
            ]

        def write_sarif(path, results):
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"version": "2.1.0", "runs": drivers(results)}, f)

        def result(rule, file_, fp):
            return {"ruleId": rule, "message": {"text": "x"},
                    "locations": [{"physicalLocation": {"artifactLocation": {"uri": file_}}}],
                    "fingerprints": {"goo/v1": fp}}

        prefix = os.path.join(tmp, "baseline")
        write_baseline(baseline_file(prefix, sig), {("GNU C23", "r1", "a.c", "fp1")}, "seed")
        devs = os.path.join(tmp, "deviations")
        os.mkdir(devs)
        open(os.path.join(devs, "D-01.md"), "w", encoding="utf-8").close()

        def run_check(sarif, suppressions):
            return check(sarif, prefix, suppressions, devs)

        # (a) a result NOT in the baseline -> RED.
        sarif_a = os.path.join(tmp, "a.sarif")
        write_sarif(sarif_a, [result("r1", "a.c", "fp1"), result("r2", "b.c", "fp2")])
        suppressions_empty = os.path.join(tmp, "empty-suppressions.tsv")
        open(suppressions_empty, "w", encoding="utf-8").close()
        rc, _ = run_check(sarif_a, suppressions_empty)
        if rc == 1:
            print("    ok: a finding not in the baseline is RED")
        else:
            print("FAIL: a new finding did not go RED (rc=%d)" % rc)
            ok = False

        # I1 REGRESSION CASE: editing the TEXT of one already-accepted
        # finding (same count, new fingerprint) must stay GREEN.
        sarif_edited = os.path.join(tmp, "edited.sarif")
        write_sarif(sarif_edited, [result("r1", "a.c", "fp1x")])
        rc, lines = run_check(sarif_edited, suppressions_empty)
        if rc == 0:
            print("    ok: editing one accepted finding's text (same count, new fingerprint) stays GREEN")
        else:
            print("FAIL: editing an accepted finding's text alone went RED: %r" % lines)
            ok = False

        # M6: the verdict line must name EVERY tool that ran, including one
        # whose run produced zero results.
        rc, lines = run_check(sarif_edited, suppressions_empty)
        if "clang-tidy: 0" in lines[-1] and "toolchain %s" % sig in lines[-1]:
            print("    ok: the verdict line names a zero-result tool and the toolchain signature")
        else:
            print("FAIL: the verdict line misses a zero-result tool or the signature: %r" % lines[-1])
            ok = False

        # I2b: a toolchain with no baseline file is RED, names the update
        # command, and leaves a candidate baseline beside the SARIF. A
        # baseline for ANOTHER signature must not be used.
        sarif_other = os.path.join(tmp, "other.sarif")
        with open(sarif_other, "w", encoding="utf-8") as f:
            runs = drivers([result("r1", "a.c", "fp1")])
            runs[1]["tool"]["driver"]["version"] = "24.0.0"
            json.dump({"version": "2.1.0", "runs": runs}, f)
        rc, lines = run_check(sarif_other, suppressions_empty)
        candidate = os.path.join(tmp, "baseline.gcc16-tidy24-cppcheck2.22.tsv")
        if rc == 1 and any(UPDATE_COMMAND in l for l in lines) and os.path.isfile(candidate):
            print("    ok: a toolchain with no baseline is RED, names the command, and writes a candidate")
        else:
            print("FAIL: a missing-signature baseline did not fail loudly (rc=%d, candidate=%s): %r"
                  % (rc, os.path.isfile(candidate), lines))
            ok = False

        # (b) a suppression row with an empty justification -> RED.
        bad_suppressions = os.path.join(tmp, "bad-suppressions.tsv")
        with open(bad_suppressions, "w", encoding="utf-8") as f:
            f.write("GNU C23\tr2\tb.c\t\n")
        rc, lines = run_check(sarif_a, bad_suppressions)
        if rc == 1 and any("no valid justification" in l for l in lines):
            print("    ok: a suppression row with an empty justification is RED")
        else:
            print("FAIL: an empty-justification suppression row did not go RED: %r" % lines)
            ok = False

        # I3: a well-shaped justification naming a record that does not
        # exist (D-99) is RED.
        missing_record = os.path.join(tmp, "missing-record.tsv")
        with open(missing_record, "w", encoding="utf-8") as f:
            f.write("GNU C23\tr2\tb.c\tD-99\n")
        rc, lines = run_check(sarif_a, missing_record)
        if rc == 1 and any("D-99.md does not exist" in l for l in lines):
            print("    ok: a suppression naming a nonexistent deviation record (D-99) is RED")
        else:
            print("FAIL: a suppression naming D-99 did not go RED: %r" % lines)
            ok = False

        # (c) a merged file that fails schema validation -> RED. M4: this
        # drives tools/sarif_merge.py --validate-only for real. A copy of
        # sarif_a without its required "version" field must exit nonzero.
        invalid = os.path.join(tmp, "invalid.sarif")
        with open(sarif_a, encoding="utf-8") as f:
            doc = json.load(f)
        del doc["version"]
        with open(invalid, "w", encoding="utf-8") as f:
            json.dump(doc, f)
        proc = subprocess.run([sys.executable, os.path.join(REPO_ROOT, "tools", "sarif_merge.py"),
                               "--validate-only", invalid], capture_output=True, text=True)
        if proc.returncode != 0 and "SCHEMA INVALID" in proc.stdout + proc.stderr:
            print("    ok: a schema-invalid merged file makes sarif_merge.py --validate-only exit nonzero")
        else:
            print("FAIL: a schema-invalid file did not fail validation (rc=%d): %s%s"
                  % (proc.returncode, proc.stdout, proc.stderr))
            ok = False

        # (d) the SAME sarif, now WITH a suppression naming a record that
        # exists (D-01) -> only r1 is compared, already in the baseline -> GREEN.
        real_suppressions = os.path.join(tmp, "real-suppressions.tsv")
        with open(real_suppressions, "w", encoding="utf-8") as f:
            f.write("GNU C23\tr2\tb.c\tD-01\n")
        rc, lines = run_check(sarif_a, real_suppressions)
        if rc == 0:
            print("    ok: suppressing the new finding's (tool, rule, file) with an existing record is GREEN")
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
