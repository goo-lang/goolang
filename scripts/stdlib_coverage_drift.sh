#!/bin/bash
# stdlib-coverage drift gate.
#
# Why this exists. docs/stdlib-coverage.json is the number this project quotes
# for stdlib reach, and `make stdlib-coverage` regenerated it by hand. Nothing
# ran that target, so the file sat at 77 symbols / 8 packages from 2026-07-06
# until the 2026-08-17 audit while the tree had moved to 112 / 12. A number
# nobody re-measures is a number nobody should quote.
#
# WHAT IT COMPARES, and what it deliberately does not. The gate compares the
# NUMERATOR only: the per-package list of supported symbol NAMES. That set is a
# property of this repo (goostd/ source plus the audited C-shim list), so it is
# identical on every machine.
#
# It ignores go_version, every total, and every percentage, because those come
# from $GOROOT/api/go1*.txt and move when the developer's Go release moves. A
# gate that goes red because someone upgraded Go is a gate people learn to
# ignore — the same reasoning ADR 0003 gives for keeping the benchmark harness
# a reporter rather than a gate.
#
# SKIPS cleanly with exit 0 when `go` is absent, matching the valgrind probes.
# verify-core has to stay green on a clean machine.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMMITTED="$ROOT/docs/stdlib-coverage.json"

if ! command -v go >/dev/null 2>&1; then
  echo "stdlib-coverage-drift: SKIPPED (no go on PATH; \$GOROOT/api is the denominator source)"
  exit 0
fi

if [ ! -f "$COMMITTED" ]; then
  echo "stdlib-coverage-drift: FAIL (docs/stdlib-coverage.json is missing)"
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FRESH="$TMP/stdlib-coverage.json"

if ! GOO_STDLIB_COVERAGE_OUT="$FRESH" python3 "$ROOT/scripts/stdlib-coverage.py" >"$TMP/run.log" 2>&1; then
  echo "stdlib-coverage-drift: FAIL (regeneration errored)"
  cat "$TMP/run.log"
  exit 1
fi

# Reduce each report to "package<TAB>sym,sym,sym" — the machine-independent part.
reduce() {
  python3 - "$1" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for p in sorted(d.get("packages", []), key=lambda x: x["package"]):
    print(p["package"] + "\t" + ",".join(sorted(p.get("symbols", []))))
PY
}

if diff -u <(reduce "$COMMITTED") <(reduce "$FRESH") > "$TMP/drift.diff"; then
  n=$(python3 -c "import json;print(json.load(open('$COMMITTED'))['overall']['supported_symbols'])")
  p=$(python3 -c "import json;print(json.load(open('$COMMITTED'))['overall']['packages_touched'])")
  echo "stdlib-coverage-drift: PASS ($n symbols across $p packages, committed report matches the tree)"
  exit 0
fi

echo "stdlib-coverage-drift: FAIL — docs/stdlib-coverage.json no longer matches the tree."
echo "The supported-symbol set moved. Regenerate and commit the result:"
echo "    make stdlib-coverage"
echo
sed -n '1,60p' "$TMP/drift.diff"
exit 1
