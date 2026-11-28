#!/usr/bin/env python3
"""Signature and tree hash of the analyser lanes' inputs.

scripts/analyzer_lanes_probe.sh writes build/sarif/lanes.stamp from this
module after all three lanes pass. scripts/findings_probe.sh recomputes the
same two values and refuses SARIF whose stamp differs. One module computes
both sides, so the two scripts cannot disagree about what a lane reads.

  lanes_stamp.py sig  <compile_commands.json>   print the toolchain signature
  lanes_stamp.py tree <compile_commands.json>   print the input tree hash

The list of files comes from the compile database, which is the lanes' own
source of truth: each entry's source file, every non-system header that the
preprocessor reports for it (gcc -MM), and the database itself. A lane also
reads the cppcheck addon directory, the suppression list, every .clang-tidy
on a source file's directory walk, the Makefile (the database derives from
its recipes), and the lane and capture scripts, so those are hashed as well.
"""
import concurrent.futures
import hashlib
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _cc_of(args):
    return next(a for a in args if not a.endswith("ccache"))


def _version_line(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not r.stdout.strip():
        raise SystemExit("lanes_stamp: cannot read the version of %s" % cmd[0])
    return r.stdout.strip().splitlines()[0]


def signature(db_path):
    entries = json.load(open(db_path, encoding="utf-8"))
    if not entries:
        raise SystemExit("lanes_stamp: %s has no entries" % db_path)
    cc = _cc_of(entries[0]["arguments"])
    return signature_from([
        cc + ": " + _version_line([cc, "--version"]),
        "clang-tidy: " + _version_line(["clang-tidy", "--version"]),
        "cppcheck: " + _version_line(["cppcheck", "--version"]),
    ], os.environ.get("CLANG_TIDY_CHECKS", ""))


def signature_from(version_lines, clang_tidy_checks):
    """Pure part of sig. CLANG_TIDY_CHECKS changes what the tidy lane reports."""
    parts = list(version_lines) + ["CLANG_TIDY_CHECKS=" + clang_tidy_checks]
    return hashlib.sha256("\n".join(parts).encode()).hexdigest()


def _deps(entry):
    """Source and non-system headers of one entry, repo-relative."""
    args = list(entry["arguments"])
    out = []
    skip = False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        # The Makefile's own dependency flags would send the -MM output to a
        # file, and an empty list here would hash the sources but no header.
        if a in ("-c", "-Werror", "-MD", "-MMD", "-MP", "-M", "-MM", "-MG"):
            continue
        out.append(a)
    out.append("-MM")
    cwd = os.path.join(REPO, entry.get("directory", "."))
    env = dict(os.environ, CCACHE_DISABLE="1")
    r = subprocess.run(out, cwd=cwd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        raise SystemExit("lanes_stamp: -MM failed for %s: %s" % (entry["file"], r.stderr[-500:]))
    text = r.stdout.replace("\\\n", " ")
    toks = text.split()
    paths = [t for t in toks[1:] if t != "\\"]
    return [os.path.relpath(os.path.normpath(os.path.join(cwd, p)), REPO) for p in paths]


def _walk_files(root):
    for d, dirs, files in os.walk(root):
        # Python writes __pycache__ on import, and another gate may import
        # the addon between the lanes and findings-probe.
        dirs[:] = [x for x in dirs if x != "__pycache__"]
        for f in files:
            if f.endswith(".pyc"):
                continue
            yield os.path.relpath(os.path.join(d, f), REPO)


def input_files(db_path):
    entries = json.load(open(db_path, encoding="utf-8"))
    files = {os.path.relpath(os.path.abspath(db_path), REPO)}
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for deps in ex.map(_deps, entries):
            files.update(deps)
    # A .clang-tidy is found by walking up from each source file's directory.
    for e in entries:
        d = os.path.dirname(os.path.normpath(os.path.join(REPO, e.get("directory", "."), e["file"])))
        while True:
            cand = os.path.join(d, ".clang-tidy")
            if os.path.isfile(cand):
                files.add(os.path.relpath(cand, REPO))
            if d == REPO or len(d) <= len(REPO):
                break
            d = os.path.dirname(d)
    misra_json = os.environ.get("MISRA_JSON", os.path.join(REPO, "docs/misra/tools/misra.json"))
    misra_supp = os.environ.get("MISRA_SUPPRESSIONS", os.path.join(REPO, "docs/misra/misra-suppressions.txt"))
    files.update(_walk_files(os.path.dirname(os.path.abspath(misra_json))))
    for extra in (misra_supp, "Makefile", "scripts/analyzer_lanes_probe.sh",
                  "tools/gen_compile_commands.sh", "tools/cppcheck_xml_to_sarif.py",
                  "tools/cppcheck_lane_check.py",
                  "tools/lanes_stamp.py"):
        p = extra if os.path.isabs(extra) else os.path.join(REPO, extra)
        files.add(os.path.relpath(p, REPO))
    return sorted(files)


def tree_hash(db_path):
    lines = []
    for rel in input_files(db_path):
        p = os.path.join(REPO, rel)
        if not os.path.isfile(p):
            raise SystemExit("lanes_stamp: an input file is missing: %s" % rel)
        h = hashlib.sha256(open(p, "rb").read()).hexdigest()
        lines.append("%s  %s" % (h, rel))
    # Byte order, so the hash does not depend on the locale.
    lines.sort(key=lambda s: s.encode())
    return hashlib.sha256(("\n".join(lines) + "\n").encode()).hexdigest()


def main(argv):
    if len(argv) != 3 or argv[1] not in ("sig", "tree"):
        sys.stderr.write(__doc__)
        return 2
    print(signature(argv[2]) if argv[1] == "sig" else tree_hash(argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
