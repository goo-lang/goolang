#!/usr/bin/env bash
# Scan Goo's C source against the adopted MISRA C:2012 subset and fail on any
# violation not already in the accepted baseline. Local-only gate (no CI),
# matching the safety-scan.sh workflow.
#
# The policy — which guidelines are adopted, which are deviated, and why — is
# in docs/misra/README.md. Deviation records are in docs/misra/deviations/.
#
# Usage:
#   scripts/misra-scan.sh                      # gate: fail (exit 1) on new findings
#   scripts/misra-scan.sh --update-baseline    # accept current findings as the baseline
#   scripts/misra-scan.sh --report             # full breakdown, never fails
#
# Environment overrides:
#   MISRA_JSON   cppcheck addon config          (default: docs/misra/tools/misra.json)
#   BASELINE     accepted-violation counts      (default: scripts/misra-baseline.txt)
#   MISRA_OUT    raw scan output                (default: build/misra-scan.txt)
#   JOBS         cppcheck parallelism           (default: nproc)
#
# Requires a rule-text file built from your own licensed copy of the standard.
# See docs/misra/tools/README.md — it is not in this repository, because the
# guideline text is copyrighted.
set -euo pipefail

MODE=gate
case "${1:-}" in
	--update-baseline) MODE=update ;;
	--report)          MODE=report ;;
	"")                ;;
	*) echo "usage: $0 [--update-baseline|--report]" >&2; exit 2 ;;
esac

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$REPO_ROOT/docs/misra/tools"
BASELINE="${BASELINE:-$REPO_ROOT/scripts/misra-baseline.txt}"
MISRA_OUT="${MISRA_OUT:-$REPO_ROOT/build/misra-scan.txt}"
CONFIG="${MISRA_JSON:-$TOOLS/misra.json}"
JOBS="${JOBS:-$(nproc)}"

if ! command -v cppcheck >/dev/null 2>&1; then
	echo "error: cppcheck is required" >&2
	exit 2
fi
if [[ ! -f "$CONFIG" ]]; then
	echo "error: no cppcheck addon config at $CONFIG" >&2
	echo "       build one from your licensed MISRA PDF; see docs/misra/tools/README.md" >&2
	echo "       or set MISRA_JSON to point at an existing config" >&2
	exit 2
fi

mkdir -p "$(dirname "$MISRA_OUT")"

MISRA_JSON="$CONFIG" "$TOOLS/scan.sh" "$MISRA_OUT" \
	-j "$JOBS" \
	"--suppressions-list=$REPO_ROOT/docs/misra/misra-suppressions.txt" \
	-- $(cd "$REPO_ROOT" && find src -name '*.c' | sort)

case "$MODE" in
	update) exec python3 "$TOOLS/gate.py" write "$MISRA_OUT" "$BASELINE" ;;
	report) python3 "$TOOLS/gate.py" check "$MISRA_OUT" "$BASELINE" || true
	        exit 0 ;;
	gate)   exec python3 "$TOOLS/gate.py" check "$MISRA_OUT" "$BASELINE" ;;
esac
