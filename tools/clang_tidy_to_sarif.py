#!/usr/bin/env python3
"""clang_tidy_to_sarif.py -- convert clang-tidy's plain-text output to SARIF.

clang-tidy has no native SARIF emitter (verified against this repository's
LLVM 22 build: `clang-tidy --help` names no `--output-format` flag at all).
scripts/analyzer_lanes_probe.sh's clang-tidy lane already writes its raw
stdout+stderr per file under build/sarif/clang-tidy/<key>.txt (see that
script's header, "OUTPUT KEYS"); this converts every one of those into ONE
SARIF 2.1.0 log, so tools/sarif_merge.py has a real file to read for the
clang-tidy tool, the same way it already reads gcc's and cppcheck's own
native SARIF.

    clang_tidy_to_sarif.py --out build/sarif/clang-tidy/clang-tidy.sarif \\
        --version "22.1.8" INPUT [INPUT ...]
    clang_tidy_to_sarif.py --self-test

Each INPUT is a .txt file or a directory (searched recursively for *.txt,
so build/sarif/clang-tidy/.cc/ -- the lane's own throwaway compile-command
copy -- is walked too; harmless, since it holds no .txt files).

THE LINE FORMAT (VERIFIED against the real 58-entry capture, 778 warnings):
a diagnostic HEADER line is exactly

    <path>:<line>:<col>: warning: <message> [<check-name>]

optionally ending "error:" instead of "warning:" for a genuine compile
error in the translation unit (rare; the lane's own header notes clang-tidy
exits non-zero for this, distinct from a plain warning). Every other line
in the file is either a "note:" line (clang-tidy's own elaboration on the
SAME diagnostic -- for example "expanded from macro ..." -- never a new,
separate finding) or SOURCE CONTEXT: an indented "<n> | <source text>" line
plus a "      | ^~~~" caret line. Anchoring the header regex to the START of
the line is what tells these apart: a context line's own path column is a
bare line number, never a real path, so `^([^\\s:][^:]*):(\\d+):(\\d+): ` can
only match a genuine diagnostic header. VERIFIED: four lines in this
repository's own captured output contain the literal substring "error:"
purely as C-string payload inside a `fprintf(stderr, "%s:%d:%d: error: ...")`
call that clang-tidy is quoting back as source context; none of them anchors
the regex, so all four are correctly skipped.

A header with no bracketed check name (clang-tidy always closes its own
diagnostics with one, but this is not guaranteed by the tool's contract) is
still emitted, with ruleId "clang-tidy-unclassified" -- printed to stderr as
a warning, never silently dropped, so a future clang-tidy release that
changes this format is loud rather than invisible in the findings count.
"""
import glob
import json
import os
import re
import sys
import tempfile

HEADER_RE = re.compile(
    r"^(?P<path>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): "
    r"(?P<level>warning|error): (?P<msg>.*?)(?: \[(?P<check>[\w][\w.,-]*)\])?$"
)


def normalise_uri(path):
    # clang-tidy is invoked with cwd = REPO_ROOT (see the lane's header) and
    # a "./"-relative file argument for one of its two path styles -- strip
    # the leading "./" so this matches gcc's and cppcheck's own bare
    # "src/..." style and the three tools fingerprint the same file the
    # same way in tools/sarif_merge.py.
    if path.startswith("./"):
        return path[2:]
    return path


def parse_file(path):
    results = []
    unclassified = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = HEADER_RE.match(line)
            if not m:
                continue
            check = m.group("check")
            if not check:
                unclassified += 1
                check = "clang-tidy-unclassified"
            results.append({
                "ruleId": check,
                "level": "error" if m.group("level") == "error" else "warning",
                "message": {"text": m.group("msg")},
                "locations": [{
                    "physicalLocation": {
                        "artifactLocation": {"uri": normalise_uri(m.group("path"))},
                        "region": {
                            "startLine": int(m.group("line")),
                            "startColumn": int(m.group("col")),
                        },
                    }
                }],
            })
    return results, unclassified


def find_inputs(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            files.extend(sorted(glob.glob(os.path.join(p, "**", "*.txt"), recursive=True)))
        elif os.path.isfile(p):
            files.append(p)
        else:
            sys.exit("clang_tidy_to_sarif: input path does not exist: %s" % p)
    return files


def convert(input_paths, version):
    all_results = []
    unclassified_total = 0
    for path in find_inputs(input_paths):
        results, unclassified = parse_file(path)
        all_results.extend(results)
        unclassified_total += unclassified
    if unclassified_total:
        sys.stderr.write(
            "clang_tidy_to_sarif: %d diagnostic(s) had no [check-name] and "
            "were classified as clang-tidy-unclassified\n" % unclassified_total
        )
    driver = {"name": "clang-tidy", "informationUri": "https://clang.llvm.org/extra/clang-tidy/"}
    if version:
        driver["version"] = version
    doc = {
        "$schema": "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/master/Schemata/sarif-schema-2.1.0.json",
        "version": "2.1.0",
        "runs": [{"tool": {"driver": driver}, "results": all_results}],
    }
    return doc


# --self-test: in-memory synthetic fixtures, no repository files touched.
# Cases mirror this module's own docstring claims:
#   1. a real warning header is converted, with the check name as ruleId.
#   2. a "note:" line attached to a warning is NOT a separate result.
#   3. a source-context line containing the literal substring "error:"
#      inside quoted text does not get mistaken for a diagnostic header --
#      the exact shape found in this repository's own captured output.
#   4. a header with no bracketed check name still converts, classified
#      "clang-tidy-unclassified" rather than silently dropped.
#   5. an empty (zero-warning) file converts to zero results.
FIXTURE = """\
./include/goo_assert.h:83:5: warning: Call to function 'fprintf' is insecure [clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling]
   83 |     fprintf(stderr, "goo: %s FAILED: %s\\n  at %s:%d in %s()\\n",
      |     ^~~~~~~
./include/goo_assert.h:83:5: note: Call to function 'fprintf' is insecure
   83 |     fprintf(stderr, "goo: %s FAILED: %s\\n  at %s:%d in %s()\\n",
      |     ^~~~~~~
  672 |         fprintf(stderr, "%s:%d:%d: error: %s\\n",
  672 |         fprintf(stderr, "%s:%d:%d: error: %s\\n",
src/x.c:10:3: warning: something odd
"""


def self_test():
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        fixture_path = os.path.join(tmp, "sample.txt")
        with open(fixture_path, "w", encoding="utf-8") as f:
            f.write(FIXTURE)
        doc = convert([fixture_path], "22.1.8")
        results = doc["runs"][0]["results"]

        if len(results) == 2:
            print("    ok: the fixture's 1 real warning + 1 unclassified header convert "
                  "to exactly 2 results (its note line and quoted-source-context lines "
                  "correctly produce none)")
        else:
            print("FAIL: expected 2 results (the warning + the unclassified header), got %d: %r"
                  % (len(results), results))
            ok = False

        rule_ids = {r["ruleId"] for r in results}
        if "clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling" in rule_ids:
            print("    ok: the real warning's bracketed check name becomes its ruleId")
        else:
            print("FAIL: expected the real check name among ruleIds, got %r" % rule_ids)
            ok = False

        if "clang-tidy-unclassified" in rule_ids:
            print("    ok: a header with no bracketed check name is classified, not dropped")
        else:
            print("FAIL: expected clang-tidy-unclassified among ruleIds, got %r" % rule_ids)
            ok = False

        uris = {r["locations"][0]["physicalLocation"]["artifactLocation"]["uri"] for r in results}
        if "include/goo_assert.h" in uris:
            print("    ok: a leading './' is stripped from the file URI")
        else:
            print("FAIL: expected 'include/goo_assert.h' (no './') among URIs, got %r" % uris)
            ok = False

        empty_path = os.path.join(tmp, "empty.txt")
        with open(empty_path, "w", encoding="utf-8"):
            pass
        empty_doc = convert([empty_path], "22.1.8")
        if empty_doc["runs"][0]["results"] == []:
            print("    ok: an empty (zero-warning) file converts to zero results")
        else:
            print("FAIL: an empty file produced results: %r" % empty_doc["runs"][0]["results"])
            ok = False

    if ok:
        print("clang_tidy_to_sarif.py --self-test: PASS")
        return 0
    print("clang_tidy_to_sarif.py --self-test: FAIL")
    return 1


def main(argv):
    if argv == ["--self-test"]:
        return self_test()

    out_path = None
    version = None
    inputs = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--out":
            i += 1
            out_path = argv[i]
        elif a == "--version":
            i += 1
            version = argv[i]
        else:
            inputs.append(a)
        i += 1

    if not out_path or not inputs:
        sys.exit(__doc__)

    doc = convert(inputs, version)
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
        f.write("\n")
    print("clang_tidy_to_sarif: wrote %s -- %d results" % (out_path, len(doc["runs"][0]["results"])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
