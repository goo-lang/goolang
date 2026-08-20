#!/usr/bin/env bash
# testcase-report — which GOO_TESTCASE markers were never driven both ways.
#
# A marker is a claim that a boundary matters (see include/goo_assert.h). Under
# a coverage build it expands to a real branch, so gcov records each direction.
# This script reads those counters back and names every marker that only ever
# went one way.
#
# IT READS GCOV'S JSON, NOT THE .gcov TEXT FILES. The first version of this
# script parsed the text format, and it was wrong about the pipeline it serves:
# scripts/coverage_corpus.sh runs `gcov --json-format --stdout` and never writes
# a .gcov file at all, so a real corpus run would have reported "no data" while
# the fixture-sized probe stayed green. The invocation below, including the
# --conditions retry for gcc 13 and older, is deliberately the same one that
# script uses. One format, one failure mode, one place to fix it.
#
# The JSON shape being read:
#   {"files":[{"file":"src/types/release_decision.c",
#              "lines":[{"line_number":5,"branches":[{"count":0},{"count":1}]}]}]}
# Two branch entries, one per direction. A direction with count 0 means no test
# reached it.
#
# EXIT STATUS. 0 whether or not markers are incomplete -- the incomplete list is
# the product, not a verdict, and the caller decides what to do with it. 2 when
# the script COULD NOT MEASURE: no gcov output, or no marker found. That
# distinction is the whole point. A run that never happened must not read the
# same as a run in which every boundary was reached, which is the failure mode
# coverage_corpus.sh spends six separate assertions avoiding.
set -u

PROG="testcase-report"
GCOV_DIR=""
SOURCES=()

while [ $# -gt 0 ]; do
	case "$1" in
		--gcov-dir) GCOV_DIR="${2:-}"; shift 2 ;;
		--source)   SOURCES+=("${2:-}"); shift 2 ;;
		*) echo "$PROG: unknown argument '$1'" >&2; exit 2 ;;
	esac
done

if [ -z "$GCOV_DIR" ] || [ "${#SOURCES[@]}" -eq 0 ]; then
	echo "$PROG: usage: $0 --gcov-dir DIR --source FILE [--source FILE ...]" >&2
	exit 2
fi

command -v gcov    >/dev/null || { echo "$PROG: BROKEN -- gcov is not installed" >&2; exit 2; }
command -v python3 >/dev/null || { echo "$PROG: BROKEN -- python3 is not installed" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
JSON_DIR="$WORK/json"
mkdir -p "$JSON_DIR"

emitted=0
for gcno in "$GCOV_DIR"/*.gcno; do
	[ -e "$gcno" ] || continue
	base="$(basename "$gcno" .gcno)"
	if gcov --json-format --stdout -b --conditions -o "$GCOV_DIR" "$gcno" \
	        > "$JSON_DIR/$base.json" 2>/dev/null && [ -s "$JSON_DIR/$base.json" ]; then
		emitted=$((emitted + 1))
	else
		# --conditions is rejected by gcc 13 and older, exactly as
		# coverage_corpus.sh documents. Retry without it.
		gcov --json-format --stdout -b -o "$GCOV_DIR" "$gcno" \
			> "$JSON_DIR/$base.json" 2>/dev/null && [ -s "$JSON_DIR/$base.json" ] \
			&& emitted=$((emitted + 1))
	fi
done

if [ "$emitted" -eq 0 ]; then
	echo "$PROG: BROKEN -- gcov emitted no JSON for any object in $GCOV_DIR."
	echo "  Nothing was measured. A count of 0 incomplete markers here would"
	echo "  mean the run did not happen, not that every boundary was reached."
	exit 2
fi

PROG="$PROG" python3 - "$JSON_DIR" "${SOURCES[@]}" <<'PY'
import json, os, sys

prog = os.environ.get("PROG", "testcase-report")
json_dir = sys.argv[1]
sources = sys.argv[2:]

# line-number -> branch counts, keyed by the path gcov reported.
cov = {}
for name in sorted(os.listdir(json_dir)):
    try:
        with open(os.path.join(json_dir, name)) as fh:
            data = json.load(fh)
    except (json.JSONDecodeError, OSError):
        continue
    for f in data.get("files", []):
        table = cov.setdefault(os.path.normpath(f["file"]), {})
        for line in f.get("lines", []):
            counts = [b.get("count", 0) for b in line.get("branches", [])]
            if counts:
                table.setdefault(line["line_number"], []).extend(counts)

def lookup(src):
    """gcov reports the path as the compiler saw it, which need not match the
    path we were handed. Prefer an exact or suffix match; fall back to the base
    name only when nothing else matches, and never silently pick between two."""
    n = os.path.normpath(src)
    if n in cov:
        return cov[n]
    for k in cov:
        if n.endswith(os.sep + k) or k.endswith(os.sep + n):
            return cov[k]
    base = os.path.basename(n)
    hits = [k for k in cov if os.path.basename(k) == base]
    return cov[hits[0]] if len(hits) == 1 else None

markers = 0
incomplete = []
for src in sources:
    if not os.path.isfile(src):
        continue
    table = lookup(src)
    if table is None:
        continue
    with open(src, errors="replace") as fh:
        for lineno, text in enumerate(fh, 1):
            if "GOO_TESTCASE(" not in text:
                continue
            markers += 1
            counts = table.get(lineno, [])
            # Fewer than two directions means gcov saw no two-way branch on
            # that line at all, which is also "not driven both ways".
            if len(counts) < 2 or min(counts) == 0:
                incomplete.append((src, lineno, text.strip()))

if markers == 0:
    print(f"{prog}: BROKEN -- gcov data present, but no GOO_TESTCASE marker found.",
          file=sys.stderr)
    print("  Either the sources given carry no markers, or the grep that finds",
          file=sys.stderr)
    print("  them no longer matches the macro's spelling.", file=sys.stderr)
    sys.exit(2)

for src, lineno, text in incomplete:
    print(f"  {src}:{lineno}: {text}")
print(f"{prog}: {markers} marker(s), {len(incomplete)} not exercised both ways")
PY
exit $?
