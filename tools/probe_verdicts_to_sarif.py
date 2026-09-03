#!/usr/bin/env python3
"""probe_verdicts_to_sarif.py -- fold two non-analyser signals into SARIF.

Task 17: tools/sarif_merge.py's three analyser lanes (gcc, clang-tidy,
cppcheck) all read source and report a LOCATION. Two more signals this
repository already tracks as shrink-only ratchets do not fit that shape,
so this converts each into its own SARIF run for tools/sarif_merge.py to
pick up like any other:

  pthread-baseline  scripts/pthread-return-baseline.txt's per-file counts
                     (scripts/pthread_return_probe.sh's own accepted-debt
                     record, updated only by that probe's --update-baseline)
                     -> one result per file with a nonzero count, ruleId
                     "goo/pthread-return-unchecked", the count in the
                     message. A file's row reads as accepted-until-fixed
                     debt, the same way a misra-suppressions.txt entry
                     reads -- this is the recorded state of that ratchet,
                     not a fresh scan (running the live counter again would
                     double the same signal against two shrink-only gates).

  gate-log           any log following this repository's own "<gate>:
                     PASS|FAIL (...)" convention (every probe script here
                     prints one such line; scripts/findings_probe.sh feeds
                     it the lanes' own captured stdout, since running the
                     full verify-core target is out of scope for this gate)
                     -> one result per FAIL line, ruleId "goo/gate-failed",
                     level error, the gate name and its own message text
                     copied through. A PASS line produces nothing.

    probe_verdicts_to_sarif.py pthread-baseline <baseline.txt> --out <out.sarif>
    probe_verdicts_to_sarif.py gate-log <log> [<log> ...] --out <out.sarif>
    probe_verdicts_to_sarif.py --self-test

Nothing here reads the network or invokes another tool; both modes are pure
text-to-SARIF conversions of a file already on disk.
"""
import json
import os
import re
import sys
import tempfile

GATE_LINE_RE = re.compile(r"^([A-Za-z][\w-]*): (PASS|FAIL)\b(.*)$")


def convert_pthread_baseline(path):
    """One result per NONZERO row. A "0" row is not a finding -- the same
    reason docs/misra/tools/gate.py never emits a row for a fixed pair."""
    results = []
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 2:
                continue
            file_path, count_s = parts
            try:
                count = int(count_s)
            except ValueError:
                continue
            if count <= 0:
                continue
            results.append({
                "ruleId": "goo/pthread-return-unchecked",
                "level": "warning",
                "message": {"text": "%d unchecked pthread_* return(s) in this file "
                                     "(scripts/pthread-return-baseline.txt)" % count},
                "locations": [{"physicalLocation": {"artifactLocation": {"uri": file_path}}}],
            })
    driver = {"name": "pthread-return-probe",
              "informationUri": "scripts/pthread_return_probe.sh"}
    return {
        "$schema": "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/master/Schemata/sarif-schema-2.1.0.json",
        "version": "2.1.0",
        "runs": [{"tool": {"driver": driver}, "results": results}],
    }


def convert_gate_log(paths):
    """One result per FAIL line. Refuses a log with not even one PASS-or-FAIL
    line at all -- the same "a comparison against nothing always passes"
    protection docs/misra/tools/gate.py's tally() applies to a scan that
    produced no 'Checking' lines. A silently empty log must not read as
    "every gate passed"."""
    results = []
    total_lines = 0
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for raw in f:
                m = GATE_LINE_RE.match(raw.rstrip("\n"))
                if not m:
                    continue
                total_lines += 1
                gate, verdict, rest = m.groups()
                if verdict != "FAIL":
                    continue
                results.append({
                    "ruleId": "goo/gate-failed",
                    "level": "error",
                    "message": {"text": "%s: FAIL%s" % (gate, rest)},
                })
    if total_lines == 0:
        sys.exit(
            "probe_verdicts_to_sarif: no '<gate>: PASS|FAIL' line found in "
            "%s -- refusing to report zero failures from a log that shows "
            "no gate ran at all" % ", ".join(paths)
        )
    driver = {"name": "verify-core-gates"}
    return {
        "$schema": "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/master/Schemata/sarif-schema-2.1.0.json",
        "version": "2.1.0",
        "runs": [{"tool": {"driver": driver}, "results": results}],
    }


# --self-test: in-memory synthetic fixtures, no repository files touched.
def self_test():
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        baseline = os.path.join(tmp, "baseline.txt")
        with open(baseline, "w", encoding="utf-8") as f:
            f.write(
                "# comment\n\n"
                "src/runtime/channels.c\t17\n"
                "src/runtime/sync.c\t0\n"
            )
        doc = convert_pthread_baseline(baseline)
        results = doc["runs"][0]["results"]
        if len(results) == 1 and results[0]["message"]["text"].startswith("17 "):
            print("    ok: a nonzero row converts to one result naming its count; a zero row does not")
        else:
            print("FAIL: expected exactly one result naming count 17, got %r" % results)
            ok = False
        if results[0]["ruleId"] == "goo/pthread-return-unchecked":
            print("    ok: ruleId is goo/pthread-return-unchecked")
        else:
            print("FAIL: unexpected ruleId %r" % results[0]["ruleId"])
            ok = False

        log_pass_only = os.path.join(tmp, "green.log")
        with open(log_pass_only, "w", encoding="utf-8") as f:
            f.write("some noise\nanalyzer-lanes-probe: PASS (58 entries)\nmore noise\n")
        doc2 = convert_gate_log([log_pass_only])
        if doc2["runs"][0]["results"] == []:
            print("    ok: an all-PASS log converts to zero results")
        else:
            print("FAIL: an all-PASS log produced results: %r" % doc2["runs"][0]["results"])
            ok = False

        log_mixed = os.path.join(tmp, "mixed.log")
        with open(log_mixed, "w", encoding="utf-8") as f:
            f.write(
                "analyzer-lanes-probe: PASS (58 entries)\n"
                "findings-probe: FAIL (3 regressions)\n"
            )
        doc3 = convert_gate_log([log_mixed])
        results3 = doc3["runs"][0]["results"]
        if len(results3) == 1 and results3[0]["ruleId"] == "goo/gate-failed" \
                and "findings-probe" in results3[0]["message"]["text"]:
            print("    ok: a FAIL line converts to one goo/gate-failed result naming the gate")
        else:
            print("FAIL: expected one goo/gate-failed result naming findings-probe, got %r" % results3)
            ok = False

        empty_log = os.path.join(tmp, "empty.log")
        with open(empty_log, "w", encoding="utf-8") as f:
            f.write("nothing gate-shaped here\n")
        try:
            convert_gate_log([empty_log])
        except SystemExit:
            print("    ok: a log with no PASS|FAIL line at all is refused, not read as zero failures")
        else:
            print("FAIL: a log with no gate-shaped line at all was accepted")
            ok = False

    if ok:
        print("probe_verdicts_to_sarif.py --self-test: PASS")
        return 0
    print("probe_verdicts_to_sarif.py --self-test: FAIL")
    return 1


def main(argv):
    if argv == ["--self-test"]:
        return self_test()

    if len(argv) < 2:
        sys.exit(__doc__)
    mode = argv[0]

    if mode == "pthread-baseline":
        rest = argv[1:]
        if "--out" not in rest:
            sys.exit(__doc__)
        oi = rest.index("--out")
        baseline_path, out_path = rest[0], rest[oi + 1]
        doc = convert_pthread_baseline(baseline_path)
    elif mode == "gate-log":
        rest = argv[1:]
        if "--out" not in rest:
            sys.exit(__doc__)
        oi = rest.index("--out")
        log_paths, out_path = rest[:oi], rest[oi + 1]
        doc = convert_gate_log(log_paths)
    else:
        sys.exit(__doc__)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
        f.write("\n")
    print("probe_verdicts_to_sarif: wrote %s -- %d results" % (out_path, len(doc["runs"][0]["results"])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
