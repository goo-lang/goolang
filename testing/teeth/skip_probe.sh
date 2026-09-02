#!/usr/bin/env bash
set -uo pipefail
command -v goo-tool-that-never-exists >/dev/null 2>&1 && exit 0
[ "${GOO_PROBE_NO_SKIP:-0}" = 1 ] && { echo "skip-probe: FAIL (goo-tool-that-never-exists not found, and GOO_PROBE_NO_SKIP forbids a skip)"; exit 1; }
echo "goo-tool-that-never-exists not found — SKIPPED"
exit 0
