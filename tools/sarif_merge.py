#!/usr/bin/env python3
"""sarif_merge.py -- merge every tool's SARIF into one SARIF 2.1.0 log.

Task 17 of the 2026-09-02 verification tooling plan: one findings database
over gcc (scripts/analyzer_lanes_probe.sh), clang-tidy (converted by
tools/clang_tidy_to_sarif.py, since clang-tidy has no native SARIF emitter),
cppcheck (the same probe), and the probe verdicts (converted by
tools/probe_verdicts_to_sarif.py). scripts/findings_probe.sh is the
shrink-only gate that reads the merged file this script writes.

    sarif_merge.py --out build/findings.sarif [--validate] INPUT [INPUT ...]
    sarif_merge.py --validate-only EXISTING.sarif
    sarif_merge.py --check-limits EXISTING.sarif
    sarif_merge.py --self-test

Each INPUT is a .sarif file, or a directory searched (non-recursively is not
enough -- clang-tidy's own output nests under build/sarif/clang-tidy/.cc/ --
so the search is recursive) for *.sarif files. Every result from every input
is copied into ONE run per TOOL NAME (gcc's 58 per-file SARIF files, one run
each today, become ONE "GNU C23" run here; cppcheck's single file becomes
one "Cppcheck" run) -- this is the generalisation the task text asks for:
docs/misra/tools/gate.py tallies one tool's (file, rule) pairs, and this
does the same across every tool at once, keyed by (tool, rule, file,
fingerprint) instead.

STABLE FINGERPRINTS. A SARIF result's line number moves on every edit that
adds or removes a line above it -- the same reason docs/misra/tools/gate.py's
own tally key drops line numbers entirely (see that file's tally()
docstring). This script keeps the file (a moved finding must still be found)
but replaces the line NUMBER with the line's own NORMALISED TEXT, so a
finding whose line only shifted keeps its fingerprint, while a finding whose
line's content actually changed gets a new one (a graduated key: bloated
compared to a random UUID, but a real position anchor is worth eight ASCII
words of blast radius that only ever affects one document format).
Each result's "fingerprints" map gets one key, "goo/v1", holding
sha256("<ruleId>\\x1f<repo-relative file>\\x1f<normalised line text>").
Normalisation is str.strip() plus collapsing internal runs of whitespace to
one space -- enough to survive re-indentation without conflating two
genuinely different lines. A result with no line/column region (a whole-file
finding) or whose file cannot be read from disk (a location outside the
repository, or already gone) hashes an empty line text -- still stable,
just coarser: two such results on the SAME (tool, rule, file) collide, which
matches gate.py's own (file, rule) grain for exactly this case.

SCHEMA VALIDATION. --validate checks the merged document against the
vendored docs/misra/tools/sarif-schema-2.1.0.json, ONLY after verifying that
file's own sha256 against the sidecar docs/misra/tools/sarif-schema-2.1.0.json.sha256
-- the schema is fetched once, by hand, from
https://json.schemastore.org/sarif-2.1.0.json (see that file's header) and
never re-fetched by any gate; a corrupted or hand-edited copy must fail
loudly rather than silently validate against something else. Requires the
`jsonschema` package (see docs/misra/tools/README.md's own note on the
misra addon for the parallel "not in stdlib" case) -- this repository has it
installed; scripts/findings_probe.sh's own header records the version this
was built against.
"""
import glob
import gzip
import hashlib
import json
import os
import re
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEMA_PATH = os.path.join(REPO_ROOT, "docs", "misra", "tools", "sarif-schema-2.1.0.json")
SCHEMA_SHA256_PATH = SCHEMA_PATH + ".sha256"

WHITESPACE_RE = re.compile(r"\s+")

# GitHub code scanning rejects an upload above these limits. Task 18 checks
# them here, before the upload step, so a rejection never appears first on
# GitHub. The values come from the task 18 brief, which cites GitHub's
# "SARIF support for code scanning" documentation. Nobody re-read that page
# when this was written.
MAX_RUNS = 20
MAX_RESULTS_PER_RUN = 25000
MAX_GZIP_BYTES = 10 * 1024 * 1024


def normalise_line(text):
    """str.strip() plus collapsing internal whitespace runs to one space.
    See this module's own docstring ("STABLE FINGERPRINTS") for why."""
    return WHITESPACE_RE.sub(" ", text.strip())


def repo_relative(uri):
    """A SARIF artifactLocation.uri is either already relative to the
    working directory every lane runs from (REPO_ROOT -- verified: every
    lane in scripts/analyzer_lanes_probe.sh either `cd`s there first or is
    invoked with `directory: "."` resolved against it, and
    tools/clang_tidy_to_sarif.py strips clang-tidy's own leading "./" to
    match), or absolute (a system header, e.g. /usr/include/features.h,
    that gcc's -fanalyzer reports as a related location).

    A relative URI is joined against REPO_ROOT -- NEVER against the SARIF
    FILE'S OWN directory (build/sarif/gcc/, build/sarif/cppcheck/, ...),
    which is a DIFFERENT directory from the repository root every one of
    those lanes is actually relative to. FOUND while measuring this
    module's real output for task 17's report: an earlier version joined
    against the input file's directory instead, so line_text_at() below
    silently read from a path that does not exist, got "" back for every
    real (non-self-test) result, and 287 genuinely different
    misra-c2012-15.6 findings at 287 different lines in one file collapsed
    into ONE fingerprint -- a false collision the self-test's own fixture
    (an absolute file:// URI, which skips this join entirely) could not
    catch. See this module's own self-test for the regression case this
    added.

    FIX ROUND 1 (I2a): an out-of-repository result used to get
    os.path.relpath(abs_path, REPO_ROOT), which is a path with leading ".."
    segments whose COUNT depends on how deep REPO_ROOT sits on THIS
    checkout -- a different checkout depth (a different clone path, or CI's
    /home/runner/work/... layout) gives a DIFFERENT number of ".." segments
    for the SAME system header, so the same finding's key silently changes
    across machines (measured: 28 results across 2 keys flip on a
    different checkout). Ruling: a result outside the repository keeps its
    ABSOLUTE path as the file leg -- an absolute system path (e.g.
    /usr/include/features.h) is already the same string on any machine with
    the same toolchain, where a relpath is not. Only a file actually under
    REPO_ROOT is made repo-relative."""
    if uri.startswith("file://"):
        uri = uri[len("file://"):]
    abs_path = uri if os.path.isabs(uri) else os.path.normpath(os.path.join(REPO_ROOT, uri))
    rel = os.path.relpath(abs_path, REPO_ROOT)
    if rel == os.pardir or rel.startswith(os.pardir + os.sep):
        return abs_path, abs_path
    return rel, abs_path


def line_text_at(abs_path, line_no):
    """The normalised text of ONE line, 1-indexed like SARIF's own
    region.startLine. Returns (text, ok): ok is False when a line number WAS
    expected but the file could not be read or the line does not exist in
    it -- a real read failure, not a legitimate absence of a line to read.

    FIX ROUND 1 (I4): this distinction did not exist before -- both a
    genuine "no region at all" result and an UNREADABLE file (a stale
    resolved path, the exact bug this module's own history section above
    describes) hashed the same empty line text, so the second case failed
    completely silently. merge() below now counts every ok=False here and
    FAILS the whole run if that count is above zero: after the I2a fix,
    every location this module resolves is either genuinely inside the
    repository (always readable, or the tree is broken) or kept as an
    absolute system path (also always readable on the machine that
    produced the SARIF) -- so a nonzero count means a STALE SARIF (a moved
    or deleted source file since the scan ran) or a resolution bug, either
    way something this gate must not paper over with a quiet "" fingerprint."""
    if not line_no:
        return "", True
    if not os.path.isfile(abs_path):
        return "", False
    try:
        with open(abs_path, encoding="utf-8", errors="replace") as f:
            for i, line in enumerate(f, start=1):
                if i == line_no:
                    return normalise_line(line), True
    except OSError:
        return "", False
    return "", False


def fingerprint(rule_id, rel_path, line_text):
    digest = hashlib.sha256(
        "\x1f".join((rule_id or "", rel_path, line_text)).encode("utf-8")
    ).hexdigest()
    return digest


def find_inputs(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            files.extend(sorted(glob.glob(os.path.join(p, "**", "*.sarif"), recursive=True)))
        elif os.path.isfile(p):
            files.append(p)
        else:
            sys.exit("sarif_merge: input path does not exist: %s" % p)
    return files


def merge(input_paths):
    """Returns (doc, per_tool_counts, unreadable) where per_tool_counts is a
    {tool_name: result_count} dict in insertion (first-seen) order, for the
    verdict line every caller prints, and unreadable is the count of
    locations line_text_at() could not read (see that function's own
    docstring, FIX ROUND 1 / I4) -- callers must treat a nonzero unreadable
    count as a hard failure, not just a number to print."""
    tools = {}  # tool name -> {"driver": {...}, "results": [...]}
    order = []
    unreadable = 0

    for path in find_inputs(input_paths):
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
        for run in doc.get("runs", []):
            driver = run.get("tool", {}).get("driver", {})
            name = driver.get("name", "unknown")
            if name not in tools:
                tools[name] = {"driver": dict(driver), "results": []}
                order.append(name)
            bucket = tools[name]["results"]
            for result in run.get("results", []):
                result = dict(result)  # do not mutate the caller's parsed doc
                rule_id = result.get("ruleId", "")
                rel_path, abs_path = "", ""
                locations = result.get("locations") or []
                if locations:
                    art = locations[0].get("physicalLocation", {}).get("artifactLocation", {})
                    uri = art.get("uri", "")
                    if uri:
                        rel_path, abs_path = repo_relative(uri)
                region = locations[0].get("physicalLocation", {}).get("region", {}) if locations else {}
                line_no = region.get("startLine")
                if abs_path:
                    line_text, ok = line_text_at(abs_path, line_no)
                    if not ok:
                        unreadable += 1
                else:
                    line_text = ""
                fp = result.get("fingerprints", {})
                fp = dict(fp)
                fp["goo/v1"] = fingerprint(rule_id, rel_path, line_text)
                result["fingerprints"] = fp
                bucket.append(result)

    runs = []
    counts = {}
    for name in order:
        entry = tools[name]
        runs.append({"tool": {"driver": entry["driver"]}, "results": entry["results"]})
        counts[name] = len(entry["results"])

    doc = {
        "$schema": "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/master/Schemata/sarif-schema-2.1.0.json",
        "version": "2.1.0",
        "runs": runs,
    }
    return doc, counts, unreadable


def verify_schema_sha256():
    if not os.path.isfile(SCHEMA_SHA256_PATH):
        sys.exit("sarif_merge: no sidecar sha256 at %s" % SCHEMA_SHA256_PATH)
    with open(SCHEMA_SHA256_PATH, encoding="utf-8") as f:
        expected = f.read().split()[0].strip()
    with open(SCHEMA_PATH, "rb") as f:
        actual = hashlib.sha256(f.read()).hexdigest()
    if actual != expected:
        sys.exit(
            "sarif_merge: schema sha256 mismatch (expected %s, got %s) -- "
            "refusing to validate against a schema that does not match its "
            "own sidecar" % (expected, actual)
        )


def validate(doc):
    verify_schema_sha256()
    try:
        import jsonschema
    except ImportError:
        sys.exit(
            "sarif_merge: --validate requires the jsonschema package "
            "(pip install jsonschema; see docs/misra/tools/README.md)"
        )
    with open(SCHEMA_PATH, encoding="utf-8") as f:
        schema = json.load(f)
    validator_cls = jsonschema.validators.validator_for(schema)
    validator_cls.check_schema(schema)
    validator = validator_cls(schema)
    errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path))
    return errors


def check_upload_limits(raw, max_runs=MAX_RUNS, max_results=MAX_RESULTS_PER_RUN,
                        max_gzip=MAX_GZIP_BYTES):
    """Returns (problems, stats). raw is the file's bytes: the gzip limit
    applies to the bytes GitHub receives, so the size is measured on the
    real file and not on a re-serialised document. problems is empty when
    every limit holds."""
    doc = json.loads(raw)
    runs = doc.get("runs", [])
    per_run = [len(r.get("results", [])) for r in runs]
    gz = len(gzip.compress(raw))
    problems = []
    if len(runs) > max_runs:
        problems.append("%d runs, above the limit of %d runs per file" % (len(runs), max_runs))
    for i, n in enumerate(per_run):
        if n > max_results:
            name = runs[i].get("tool", {}).get("driver", {}).get("name", "?")
            problems.append("run %d (%s) has %d results, above the limit of %d per run"
                            % (i, name, n, max_results))
    if gz > max_gzip:
        problems.append("%d bytes gzipped, above the limit of %d" % (gz, max_gzip))
    return problems, {"runs": len(runs), "max_results": max(per_run, default=0), "gzip": gz}


def _write_sarif(path, tool_name, tool_version, rule_id, uri, line):
    doc = {
        "version": "2.1.0",
        "$schema": "x",
        "runs": [{
            "tool": {"driver": {"name": tool_name, "version": tool_version}},
            "results": [{
                "ruleId": rule_id,
                "level": "warning",
                "message": {"text": "x"},
                "locations": [{"physicalLocation": {
                    "artifactLocation": {"uri": uri},
                    "region": {"startLine": line},
                }}],
            }],
        }],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)


# --self-test: in-memory synthetic fixtures, no repository files touched --
# same convention as tools/pthread_return_count.py --self-test. Five cases:
#   1. two SARIF files sharing a tool name merge into ONE run, with counts
#      summed -- the core "generalise gate.py across tools" behaviour.
#   2. a fingerprint is STABLE when a finding's line moves but its text does
#      not (the whole reason the key is text, not a line number).
#   3. a fingerprint CHANGES when the line's actual text changes, so two
#      genuinely different findings on the same (tool, rule, file) never
#      collide by accident.
#   4. a result with no line/region info does not crash, and hashes an
#      empty line text rather than raising.
#   5. --validate's schema check: a doc missing "version" is invalid (the
#      same case scripts/findings_probe.sh --self-test drives end to end);
#      a well-formed minimal doc is valid.
def self_test():
    global REPO_ROOT
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        sample = os.path.join(tmp, "sample.c")
        with open(sample, "w", encoding="utf-8") as f:
            f.write("int f(void) {\n    int *p = 0;\n    return *p;\n}\n")
        uri = "file://" + sample

        a_dir = os.path.join(tmp, "a")
        b_dir = os.path.join(tmp, "b")
        os.makedirs(a_dir)
        os.makedirs(b_dir)
        _write_sarif(os.path.join(a_dir, "one.sarif"), "GNU C23", "16", "-Wanalyzer-null-dereference", uri, 2)
        _write_sarif(os.path.join(a_dir, "two.sarif"), "GNU C23", "16", "-Wanalyzer-null-dereference", uri, 2)
        _write_sarif(os.path.join(b_dir, "cppcheck.sarif"), "Cppcheck", "2.21.1", "nullPointer", uri, 2)

        doc, counts, _ = merge([a_dir, b_dir])
        if counts == {"GNU C23": 2, "Cppcheck": 1} and len(doc["runs"]) == 2:
            print("    ok: two SARIF files under one tool name merge into one run, counts summed")
        else:
            print("FAIL: expected 2 runs, counts {'GNU C23': 2, 'Cppcheck': 1}, got %d runs, counts %s"
                  % (len(doc["runs"]), counts))
            ok = False

        fp_before = doc["runs"][0]["results"][0]["fingerprints"]["goo/v1"]

        # 2. Shift the finding to line 3 (a blank line inserted above line 2,
        # text unchanged) -- the fingerprint must not move.
        with open(sample, "w", encoding="utf-8") as f:
            f.write("\nint f(void) {\n    int *p = 0;\n    return *p;\n}\n")
        _write_sarif(os.path.join(a_dir, "one.sarif"), "GNU C23", "16", "-Wanalyzer-null-dereference", uri, 3)
        doc2, _, _ = merge([a_dir])
        fp_after_shift = doc2["runs"][0]["results"][0]["fingerprints"]["goo/v1"]
        if fp_after_shift == fp_before:
            print("    ok: fingerprint is stable when a finding's line moves but its text does not")
        else:
            print("FAIL: fingerprint changed on a pure line shift (%s -> %s)" % (fp_before, fp_after_shift))
            ok = False

        # 3. Now change line 3's actual TEXT -- the fingerprint must move.
        with open(sample, "w", encoding="utf-8") as f:
            f.write("\nint f(void) {\n    int *q = 0;\n    return *q;\n}\n")
        doc3, _, _ = merge([a_dir])
        fp_after_edit = doc3["runs"][0]["results"][0]["fingerprints"]["goo/v1"]
        if fp_after_edit != fp_before:
            print("    ok: fingerprint changes when the line's own text changes")
        else:
            print("FAIL: fingerprint did not change after the line's text changed")
            ok = False

        # 4. No region at all -- must not crash, and must still fingerprint.
        no_region = {
            "version": "2.1.0", "$schema": "x",
            "runs": [{"tool": {"driver": {"name": "NoRegionTool"}},
                      "results": [{"ruleId": "r", "message": {"text": "x"},
                                   "locations": [{"physicalLocation": {"artifactLocation": {"uri": uri}}}]}]}],
        }
        no_region_path = os.path.join(tmp, "c")
        os.makedirs(no_region_path)
        with open(os.path.join(no_region_path, "x.sarif"), "w", encoding="utf-8") as f:
            json.dump(no_region, f)
        try:
            doc4, counts4, unreadable4 = merge([no_region_path])
            has_fp = "goo/v1" in doc4["runs"][0]["results"][0]["fingerprints"]
        except Exception as e:  # noqa: BLE001 -- this IS the crash check
            print("FAIL: a result with no region raised %r" % (e,))
            ok = False
        else:
            if has_fp and unreadable4 == 0:
                print("    ok: a result with no region does not crash, still gets a fingerprint, "
                      "and does NOT count as unreadable (I4: absence of a region is not a read failure)")
            else:
                print("FAIL: a result with no region merged with no fingerprint or a nonzero unreadable count "
                      "(has_fp=%s, unreadable=%d)" % (has_fp, unreadable4))
                ok = False

        # 4b. I4: a result that DOES name a line, in a file that does not
        # exist on disk -- this IS a read failure and must be counted.
        missing_file = {
            "version": "2.1.0", "$schema": "x",
            "runs": [{"tool": {"driver": {"name": "GhostTool"}},
                      "results": [{"ruleId": "r", "message": {"text": "x"},
                                   "locations": [{"physicalLocation": {
                                       "artifactLocation": {"uri": "file://" + os.path.join(tmp, "does-not-exist.c")},
                                       "region": {"startLine": 3}}}]}]}],
        }
        missing_dir = os.path.join(tmp, "d")
        os.makedirs(missing_dir)
        with open(os.path.join(missing_dir, "x.sarif"), "w", encoding="utf-8") as f:
            json.dump(missing_file, f)
        _, _, unreadable_missing = merge([missing_dir])
        if unreadable_missing == 1:
            print("    ok: a result naming a line in a file that does not exist counts as ONE unreadable location")
        else:
            print("FAIL: expected exactly 1 unreadable location for a missing file, got %d" % unreadable_missing)
            ok = False

        # 4c. I2a: repo_relative() keeps an ABSOLUTE path for anything
        # outside the repository, never a relpath -- a relpath's ".." count
        # depends on how deep REPO_ROOT sits on the CURRENT checkout, so
        # the same system-header finding would get a DIFFERENT key on a
        # different clone. Unit-tested directly against the function (the
        # merge() pipeline never rewrites a result's OWN "uri" field --
        # only the fingerprint reflects this resolution), with a path built
        # from REPO_ROOT's own parent, guaranteed outside it on any machine.
        outside_path = os.path.join(os.path.dirname(REPO_ROOT), "definitely-outside-the-repo.c")
        rel_out, abs_out = repo_relative("file://" + outside_path)
        if rel_out == outside_path == abs_out:
            print("    ok: a path outside the repository keeps its absolute form, not a relpath")
        else:
            print("FAIL: expected %r for an out-of-repo path, got rel=%r abs=%r" % (outside_path, rel_out, abs_out))
            ok = False

        # 5. A RELATIVE uri must resolve against REPO_ROOT, never against
        # the SARIF FILE'S OWN directory. REGRESSION CASE: an earlier
        # version of repo_relative() joined a relative uri against the
        # input file's directory instead, so on this repository's real
        # output (every lane writes a repo-root-relative uri from a SARIF
        # file that lives in build/sarif/<tool>/, a DIFFERENT directory)
        # line_text_at() silently read a path that does not exist, got ""
        # for every real result, and 287 genuinely different
        # misra-c2012-15.6 findings at 287 different lines in one file
        # collapsed into ONE fingerprint -- caught only by inspecting the
        # real merged output, because this self-test's OTHER fixtures all
        # use an absolute file:// uri, which skips the buggy join entirely.
        # REPO_ROOT is monkeypatched to a scratch tree here so this proves
        # the regression without touching a real repository file.
        saved_root = REPO_ROOT
        try:
            with tempfile.TemporaryDirectory() as scratch_root:
                REPO_ROOT = scratch_root
                os.makedirs(os.path.join(scratch_root, "src"))
                with open(os.path.join(scratch_root, "src", "rel.c"), "w", encoding="utf-8") as f:
                    f.write("int a(void) { return 1; }\nint b(void) { return 2; }\n")
                sarif_subdir = os.path.join(scratch_root, "build", "sarif", "gcc")
                os.makedirs(sarif_subdir)
                _write_sarif(os.path.join(sarif_subdir, "one.sarif"), "GNU C23", "16", "ruleA", "src/rel.c", 1)
                _write_sarif(os.path.join(sarif_subdir, "two.sarif"), "GNU C23", "16", "ruleA", "src/rel.c", 2)
                doc5, _, _ = merge([sarif_subdir])
                fps = {r["fingerprints"]["goo/v1"] for r in doc5["runs"][0]["results"]}
            if len(fps) == 2:
                print("    ok: two results on different lines of a repo-root-relative file, resolved from a "
                      "SARIF file sitting in a DIFFERENT directory, get two different fingerprints")
            else:
                print("FAIL: two genuinely different lines collapsed to %d fingerprint(s) -- "
                      "relative URI resolution regressed" % len(fps))
                ok = False
        finally:
            REPO_ROOT = saved_root

    # 6. Schema validation: missing "version" is invalid; a minimal
    # well-formed document is valid. Uses the REAL vendored schema and its
    # sha256 sidecar (both committed), so this also exercises
    # verify_schema_sha256() on the untouched, correct file.
    valid_doc = {"version": "2.1.0", "$schema": "x", "runs": []}
    invalid_doc = {"$schema": "x", "runs": []}
    if not validate(valid_doc):
        print("    ok: a minimal well-formed document validates against the vendored schema")
    else:
        print("FAIL: a minimal well-formed document failed schema validation")
        ok = False
    if validate(invalid_doc):
        print("    ok: a document missing \"version\" fails schema validation")
    else:
        print("FAIL: a document missing \"version\" passed schema validation")
        ok = False

    # 7. Upload limits (task 18). Each limit has a case that goes RED, and a
    # file inside every limit is the positive control.
    def limits(runs, **kw):
        raw = json.dumps({"version": "2.1.0", "runs": runs}).encode()
        return check_upload_limits(raw, **kw)[0]

    def run_of(n):
        return {"tool": {"driver": {"name": "t"}}, "results": [{"ruleId": "r"}] * n}

    if not limits([run_of(2)] * 3):
        print("    ok: a file inside every upload limit is GREEN")
    else:
        print("FAIL: a file inside every upload limit went RED")
        ok = False
    for label, problems, needle in (
            ("21 runs", limits([run_of(1)] * 21), "21 runs"),
            ("25,001 results in one run", limits([run_of(25001)]), "25001 results"),
            ("a gzip size above the limit", limits([run_of(5)], max_gzip=10), "bytes gzipped")):
        if any(needle in p for p in problems):
            print("    ok: %s is RED" % label)
        else:
            print("FAIL: %s did not go RED: %r" % (label, problems))
            ok = False

    if ok:
        print("sarif_merge.py --self-test: PASS")
        return 0
    print("sarif_merge.py --self-test: FAIL")
    return 1


def main(argv):
    if argv == ["--self-test"]:
        return self_test()

    # --validate-only <file>: validate an EXISTING SARIF document as-is,
    # with no merge step. merge() always sets "version" on its own output,
    # so feeding it a version-less INPUT can never produce an invalid
    # OUTPUT -- scripts/findings_probe.sh's own self-test needs a way to
    # prove its gate goes red on a genuinely invalid build/findings.sarif,
    # which this gives it directly.
    if len(argv) == 2 and argv[0] == "--validate-only":
        with open(argv[1], encoding="utf-8") as f:
            doc = json.load(f)
        errors = validate(doc)
        if errors:
            print("sarif_merge: SCHEMA INVALID -- %d error(s)" % len(errors))
            for e in errors[:20]:
                print("    %s: %s" % ("/".join(str(p) for p in e.absolute_path), e.message))
            return 1
        print("sarif_merge: schema OK (%s, sha256 verified)" % SCHEMA_PATH)
        return 0

    # --check-limits <file>: the GitHub upload limits, on the real file.
    if len(argv) == 2 and argv[0] == "--check-limits":
        with open(argv[1], "rb") as f:
            problems, stats = check_upload_limits(f.read())
        if problems:
            print("sarif_merge: UPLOAD LIMIT EXCEEDED in %s" % argv[1])
            for p in problems:
                print("    %s" % p)
            return 1
        print("sarif_merge: upload limits OK (%s: %d runs of %d, at most %d results in a run of %d, "
              "%d bytes gzipped of %d)" % (argv[1], stats["runs"], MAX_RUNS, stats["max_results"],
                                          MAX_RESULTS_PER_RUN, stats["gzip"], MAX_GZIP_BYTES))
        return 0

    out_path = None
    do_validate = False
    inputs = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--out":
            i += 1
            out_path = argv[i]
        elif a == "--validate":
            do_validate = True
        else:
            inputs.append(a)
        i += 1

    if not out_path or not inputs:
        sys.exit(__doc__)

    doc, counts, unreadable = merge(inputs)

    if do_validate:
        errors = validate(doc)
        if errors:
            print("sarif_merge: SCHEMA INVALID -- %d error(s)" % len(errors))
            for e in errors[:20]:
                print("    %s: %s" % ("/".join(str(p) for p in e.absolute_path), e.message))
            return 1
        print("sarif_merge: schema OK (%s, sha256 verified)" % SCHEMA_PATH)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
        f.write("\n")

    total = sum(counts.values())
    per_tool = ", ".join("%s: %d" % (name, n) for name, n in counts.items())
    print("sarif_merge: wrote %s -- %d results (%s), %d unreadable location(s)"
          % (out_path, total, per_tool, unreadable))
    if unreadable:
        # I4: a nonzero count here means a location's source file could not
        # be read even though the SARIF pointed at a specific line -- see
        # line_text_at()'s own docstring on why, after the I2a fix, this can
        # only mean a stale SARIF or a resolution bug, never a legitimate
        # absence. Fail loudly rather than ship a silently-degraded
        # fingerprint (the exact failure mode that hid the 287-finding
        # collapse this task's own history already found once).
        print("sarif_merge: FAIL -- %d location(s) could not be read; "
              "re-run the lanes (build/sarif/* may be stale) before trusting %s"
              % (unreadable, out_path))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
