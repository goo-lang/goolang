#!/usr/bin/env bash
# Scan Goo's C source against the adopted MISRA C:2012 subset and fail on any
# violation not already in the accepted baseline. Local-only gate (no CI),
# matching the safety-scan.sh workflow.
#
# The policy — which guidelines are adopted, which are deviated, and why — is
# in docs/misra/README.md. Deviation records are in docs/misra/deviations/.
#
# THE FILE LIST: task 15 of the 2026-09-02 verification tooling plan. cppcheck
# reads `--project=build/compile_commands.json` (task 14's derivation of
# GOO_OBJS's own per-file compiler flags), not a hand-rolled `find src`. Two
# consequences, both intentional:
#   - Every file gets ITS OWN -I/-D flags, the exact ones gcc compiles it
#     with, instead of one generic flag set assembled by hand in scan.sh.
#   - Coverage narrows to GOO_OBJS: files linked into bin/goo. A file that is
#     in src/ but not in GOO_OBJS (kept there under CLAUDE.md's "src/ contains
#     what SHIPS, or what a GATE exercises" rule, such as the seven
#     proof/dependent-types files scripts/proof_cache_shell_probe.sh and
#     scripts/ast_free_leak_probe.sh compile by name) is no longer scanned
#     here. See task 15's report for the full before/after file diff.
# build/compile_commands.json is regenerated on every run (same "just derive
# it again" rule scripts/compile_commands_current_probe.sh already follows),
# so this gate never reads a stale capture.
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
#   COMPILE_COMMANDS_GEN  compile-command generator (default: tools/gen_compile_commands.sh)
#   COMPILE_COMMANDS_OUT  the capture itself       (default: build/compile_commands.json)
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
COMPILE_COMMANDS_GEN="${COMPILE_COMMANDS_GEN:-$REPO_ROOT/tools/gen_compile_commands.sh}"
COMPILE_COMMANDS_OUT="${COMPILE_COMMANDS_OUT:-$REPO_ROOT/build/compile_commands.json}"

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
if [[ ! -x "$COMPILE_COMMANDS_GEN" ]]; then
	echo "error: compile-command generator not found or not executable: $COMPILE_COMMANDS_GEN" >&2
	exit 2
fi

mkdir -p "$(dirname "$MISRA_OUT")"

# Regenerate on every run, unconditionally -- the same rule
# scripts/compile_commands_current_probe.sh already follows, and for the same
# reason: a staleness check is one more thing to get wrong, where a fresh
# `make -Bn` dry run costs a second or two and is always correct.
"$COMPILE_COMMANDS_GEN" "$COMPILE_COMMANDS_OUT"

MISRA_JSON="$CONFIG" "$TOOLS/scan.sh" "$MISRA_OUT" \
	-j "$JOBS" \
	"--suppressions-list=$REPO_ROOT/docs/misra/misra-suppressions.txt" \
	"--project=$COMPILE_COMMANDS_OUT" --

case "$MODE" in
	update) exec python3 "$TOOLS/gate.py" write "$MISRA_OUT" "$BASELINE" ;;
	report) python3 "$TOOLS/gate.py" check "$MISRA_OUT" "$BASELINE" || true
	        exit 0 ;;
	gate)   exec python3 "$TOOLS/gate.py" check "$MISRA_OUT" "$BASELINE" ;;
esac
