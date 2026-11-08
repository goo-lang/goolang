#!/usr/bin/env python3
"""Find what makes the cppcheck lane blind to a translation unit.

cppcheck abandons a unit at a parse failure (an #error, a syntax error, an
internal error), reports ONE result for it, still prints "Checking <file>",
and exits 0. Task 17b found 16 of 58 units in that state for months: each
stopped at the #error in llvm-c/DataTypes.h, and a suppression entry hid the
one result that said so. This tool reports the three ways that can happen
again:

  CPPCHECK_PARSE_FAILURE <file>:<line> <ruleId> <message>
      a result whose ruleId is a parse-failure ID: cppcheck did not analyse
      the unit that produced it.
  CPPCHECK_BLIND_SUPPRESSION <where> <entry>
      a suppression-list entry, or an inline `cppcheck-suppress` comment in a
      scanned file, whose ID glob covers a parse-failure ID. Such an entry
      hides the result above, so the first check cannot see the unit.
  CPPCHECK_KNOWN_EXCEPTION <where> <entry>
      one of the three all-ID Bison entries. They cover every ID by design
      (generated code is out of MISRA scope), so a parse failure in those
      three files stays hidden. Any OTHER all-ID entry is blind.

The last line is always:
  CPPCHECK_CHECK parse_failures=<n> blind_suppressions=<n>

The parse-failure IDs come from docs/misra/tools/aggregate.py, which already
treats them as "the scan could not check this unit", so the two lists cannot
drift apart.

Usage: cppcheck_lane_check.py <cppcheck sarif> <suppressions list>
                              <compile_commands.json> <repo root>
A missing SARIF file counts as no results (the lane fails on cppcheck's exit
status for that case).
"""
import fnmatch
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "docs", "misra", "tools"))
from aggregate import PARSE_FAIL_IDS  # noqa: E402

KNOWN_EXCEPTIONS = (
    "*:src/parser/parser.tab.c",
    "*:src/parser/parser.tab.h",
    "*:src/parser/parser.y",
)

# `cppcheck-suppress id`, `cppcheck-suppress[id1,id2]`, and the -file,
# -begin, -end and -macro forms.
INLINE_RE = re.compile(r"cppcheck-suppress(?:-file|-begin|-end|-macro)?\s*(\[[^\]]*\]|[^\s\[]+)")


def hidden_ids(id_glob):
    """The parse-failure IDs that a suppression ID glob covers."""
    return sorted(pid for pid in PARSE_FAIL_IDS if fnmatch.fnmatchcase(pid, id_glob))


def parse_failures(sarif_path):
    out = []
    if not os.path.isfile(sarif_path):
        return out
    with open(sarif_path, encoding="utf-8") as f:
        doc = json.load(f)
    for run in doc.get("runs", []):
        for r in run.get("results", []):
            rule = r.get("ruleId", "")
            if rule not in PARSE_FAIL_IDS:
                continue
            loc = (r.get("locations") or [{}])[0].get("physicalLocation", {})
            uri = loc.get("artifactLocation", {}).get("uri", "?")
            line = loc.get("region", {}).get("startLine", "?")
            out.append("CPPCHECK_PARSE_FAILURE %s:%s %s %s"
                       % (uri, line, rule, r.get("message", {}).get("text", "")))
    return out


def list_entries(suppressions_path):
    """(line number, entry) for each entry of a cppcheck text suppression list."""
    with open(suppressions_path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            entry = raw.strip()
            if not entry or entry.startswith("#") or entry.startswith("//"):
                continue
            yield n, entry


def check_list(suppressions_path):
    blind, known = [], []
    for n, entry in list_entries(suppressions_path):
        id_glob = entry.split(":", 1)[0].strip()
        if not hidden_ids(id_glob):
            continue
        where = "%s:%d" % (suppressions_path, n)
        if entry in KNOWN_EXCEPTIONS:
            known.append("CPPCHECK_KNOWN_EXCEPTION %s %s" % (where, entry))
        else:
            blind.append("CPPCHECK_BLIND_SUPPRESSION %s %s" % (where, entry))
    return blind, known


def scanned_files(cc_path, repo_root):
    """The files cppcheck reads with --inline-suppr: every database entry's
    own directory, plus src/ and include/ of the repository, recursively."""
    with open(cc_path, encoding="utf-8") as f:
        entries = json.load(f)
    dirs_flat, dirs_deep = set(), set()
    for e in entries:
        base = e.get("directory", ".")
        if not os.path.isabs(base):
            base = os.path.join(repo_root, base)
        path = e["file"] if os.path.isabs(e["file"]) else os.path.join(base, e["file"])
        dirs_flat.add(os.path.dirname(os.path.normpath(path)))
    for sub in ("src", "include"):
        d = os.path.join(repo_root, sub)
        if os.path.isdir(d):
            dirs_deep.add(d)
    files = set()
    for d in dirs_deep:
        for root, _, names in os.walk(d):
            files.update(os.path.join(root, n) for n in names if n.endswith((".c", ".h")))
    for d in dirs_flat:
        if os.path.isdir(d):
            files.update(os.path.join(d, n) for n in os.listdir(d) if n.endswith((".c", ".h")))
    return sorted(files)


def check_inline(cc_path, repo_root):
    blind = []
    for path in scanned_files(cc_path, repo_root):
        with open(path, encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                for m in INLINE_RE.finditer(line):
                    token = m.group(1)
                    globs = token[1:-1].split(",") if token.startswith("[") else [token]
                    if any(hidden_ids(g.strip()) for g in globs):
                        rel = os.path.relpath(path, repo_root)
                        shown = path if rel.startswith("..") else rel
                        blind.append("CPPCHECK_BLIND_SUPPRESSION %s:%d %s" % (shown, n, line.strip()))
    return blind


def main(argv):
    if len(argv) != 5:
        print("usage: cppcheck_lane_check.py <cppcheck sarif> <suppressions list> "
              "<compile_commands.json> <repo root>", file=sys.stderr)
        return 2
    sarif_path, suppressions_path, cc_path, repo_root = argv[1:5]
    failures = parse_failures(sarif_path)
    blind, known = check_list(suppressions_path)
    blind += check_inline(cc_path, os.path.abspath(repo_root))
    for line in failures + blind + known:
        print(line)
    print("CPPCHECK_CHECK parse_failures=%d blind_suppressions=%d" % (len(failures), len(blind)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
