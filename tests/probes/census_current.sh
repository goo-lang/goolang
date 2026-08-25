#!/usr/bin/env bash
# Assert tests/probes/census.txt is current.
#
# The census decides which macro each of the 183 probe gates gets, so a stale
# copy silently mis-routes probes. Regenerating and diffing is what makes it a
# derivation rather than a snapshot.
#
# Exit: 0 current, 1 stale, 2 the generator failed.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"

fresh="$(mktemp)"
trap 'rm -f "$fresh"' EXIT

./tools/probe_census.sh > "$fresh" 2>/dev/null
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "census_current: TOOL FAILURE probe_census.sh exited $rc"
    exit 2
fi

if diff -u tests/probes/census.txt "$fresh" > /tmp/census.diff 2>&1; then
    echo "census_current: PASS $(grep -c . tests/probes/census.txt) probes, census is current"
    exit 0
fi

echo "census_current: FAIL tests/probes/census.txt is stale"
head -20 /tmp/census.diff
echo "  regenerate with: ./tools/probe_census.sh > tests/probes/census.txt"
exit 1
