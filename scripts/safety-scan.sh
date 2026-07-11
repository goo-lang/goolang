#!/usr/bin/env bash
# Scan Goo's C source with snare's C memory-safety rule pack and fail on any
# finding not already in the accepted baseline. Local-only gate (no CI), matching
# snare's own local-verification workflow.
#
# Usage:
#   scripts/safety-scan.sh                 # gate: fail (exit 1) on new findings
#   scripts/safety-scan.sh --update-baseline   # accept current findings as the baseline
#
# Environment overrides:
#   SNARE_DIR   path to the snare checkout   (default: ../semgrep-competitor)
#   BASELINE    baseline fingerprint file    (default: scripts/safety-baseline.txt)
#   SCAN_ROOT   root the scan dirs live under (default: repo root)
#   SCAN_DIRS   space-separated dirs to scan  (default: "src include lib kernel")
set -euo pipefail

UPDATE=0
[[ "${1:-}" == "--update-baseline" ]] && UPDATE=1

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SNARE_DIR="${SNARE_DIR:-$(cd "$REPO_ROOT/../semgrep-competitor" 2>/dev/null && pwd || true)}"
BASELINE="${BASELINE:-$REPO_ROOT/scripts/safety-baseline.txt}"
SCAN_ROOT="${SCAN_ROOT:-$REPO_ROOT}"
SCAN_DIRS="${SCAN_DIRS:-src include lib kernel}"

if [[ -z "$SNARE_DIR" || ! -d "$SNARE_DIR" ]]; then
	echo "error: snare not found (set SNARE_DIR; tried ../semgrep-competitor)" >&2
	exit 2
fi
if ! command -v jq >/dev/null 2>&1; then
	echo "error: jq is required" >&2
	exit 2
fi

SNARE_BIN="$SNARE_DIR/bin/snare"
if [[ ! -x "$SNARE_BIN" ]]; then
	echo "building snare..." >&2
	( cd "$SNARE_DIR" && make build >&2 )
fi

current="$(mktemp)"
errfile="$(mktemp)"
trap 'rm -f "$current" "$errfile"' EXIT

# Scan each dir separately (snare has no exclude flag) and fingerprint findings as
# rule-id:dir/relpath:line. The dir prefix keeps a foo.c in src distinct from a
# foo.c in include and makes fingerprints stable regardless of scan order.
parse_errors=0
for d in $SCAN_DIRS; do
	target="$SCAN_ROOT/$d"
	[[ -d "$target" ]] || continue
	# --workers 1: serialize ingest. At full-tree scale (~230 files) parallel
	# ingest intermittently drops findings on large files (resource pressure ->
	# spurious parse errors), which makes the baseline unstable. Serial ingest is
	# slower but deterministic and gives the most complete parse coverage.
	json="$("$SNARE_BIN" scan "$target" --config "$SNARE_DIR/rules/c" --workers 1 --format json 2>>"$errfile" || true)"
	echo "$json" | jq -r --arg d "$d" '.[] | "\(.RuleID):\($d)/\(.Sink.File):\(.Sink.StartLine)"' >> "$current" || true
done
# snare prints "warning: N file(s) had parse errors" to stderr per scanned dir.
# Sum the N's with awk reading the file directly (no pipe): awk never fails on a
# no-match, so this stays clean under set -e / pipefail even with zero warnings.
parse_errors="$(awk '{ for (i = 1; i < NF; i++) if ($(i+1) == "file(s)") s += $i } END { print s+0 }' "$errfile")"

# Drop findings in generated files (bison output: parser.tab.c / parser.tab.h).
# They are build artifacts, not hand-written source — their findings aren't
# actionable, and they exist only after `make`, so scanning them would make the
# gate depend on build state (fresh checkout vs built tree).
{ grep -vE ':[^:]*/parser\.tab\.[ch]:' "$current" || true; } > "$current.filtered"
mv "$current.filtered" "$current"

sort -u -o "$current" "$current"
echo "scanned findings: $(wc -l < "$current" | tr -d ' ')  |  parse-error files: ${parse_errors:-0}" >&2

if [[ "$UPDATE" -eq 1 ]]; then
	cp "$current" "$BASELINE"
	echo "baseline updated: $(wc -l < "$BASELINE" | tr -d ' ') findings -> $BASELINE" >&2
	exit 0
fi

baseline_sorted="$(mktemp)"; trap 'rm -f "$current" "$errfile" "$baseline_sorted"' EXIT
[[ -f "$BASELINE" ]] && sort -u "$BASELINE" > "$baseline_sorted" || : > "$baseline_sorted"

new="$(comm -23 "$current" "$baseline_sorted" || true)"
if [[ -n "$new" ]]; then
	echo "" >&2
	echo "NEW safety findings not in the baseline:" >&2
	echo "$new" | sed 's/^/  /' >&2
	echo "" >&2
	echo "Fix them, or if accepted, refresh: SCAN_ROOT='$SCAN_ROOT' SCAN_DIRS='$SCAN_DIRS' scripts/safety-scan.sh --update-baseline" >&2
	exit 1
fi
echo "safety gate: OK — no new findings ($(wc -l < "$baseline_sorted" | tr -d ' ') baselined)" >&2
