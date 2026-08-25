#!/usr/bin/env bash
# Proves parity.sh can report the OPPOSITE result.
#
# parity.sh will print a large red number every day for most of this migration.
# A script hard-wired to do exactly that would look identical, and would still
# look identical on the day it wrongly reported zero and licensed deleting the
# Makefile. So: add one target that claims a real gate, assert the count drops
# by EXACTLY one, remove it, assert it returns.
#
# Exactly one, not merely "goes down": an off-by-one or a substring match would
# move the count by the wrong amount and must fail here.
#
# Exit codes: 0 has teeth, 1 lost its teeth, 2 a tool failed.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

# A real gate, chosen because it is in VERIFY_ALL_DEPS and is neither the first
# nor the last entry, so an off-by-one in the reader cannot accidentally
# satisfy this.
GATE="m10-probe"
TARGET="m10_probe"
PKG="tools/parity_selftest_tmp"

# parity.sh exits 1 by design while gates remain. Under `set -o pipefail` that
# status becomes the PIPELINE's status, so `parity.sh | grep -q ...` reports
# failure even when grep matches. Every reader below therefore captures the
# report into a variable first and inspects it separately, never through a pipe
# whose status is tested.
parity_report() {
    ./tools/parity.sh 2>/dev/null
    return 0
}

unmapped_count() {
    parity_report | sed -n 's/^unmapped:[[:space:]]*//p'
}

cleanup() { rm -rf "${root:?}/$PKG"; }
trap cleanup EXIT

before="$(unmapped_count)"
if [ -z "$before" ]; then
    echo "parity_selftest: TOOL FAILURE parity.sh printed no unmapped count"
    exit 2
fi

# Confirm the gate really is unmapped right now. Without this, a gate that was
# already mapped would make the delta 0 and the test would report lost teeth
# when the truth is a bad fixture.
report="$(parity_report)"
if ! printf '%s\n' "$report" | grep -qx "  $GATE"; then
    echo "parity_selftest: TOOL FAILURE $GATE is not currently unmapped; pick another"
    exit 2
fi

mkdir -p "$PKG"
cat > "$PKG/ok.sh" <<'INNER'
#!/bin/sh
exit 0
INNER
chmod +x "$PKG/ok.sh"
cat > "$PKG/BUILD" <<INNER
load("@rules_shell//shell:sh_test.bzl", "sh_test")

sh_test(
    name = "$TARGET",
    size = "small",
    srcs = ["ok.sh"],
)
INNER

after="$(unmapped_count)"
cleanup
restored="$(unmapped_count)"

rc=0
if [ "$((before - after))" -ne 1 ]; then
    echo "parity_selftest: NO TEETH adding $TARGET moved unmapped $before -> $after (want -1)"
    rc=1
fi
if [ "$restored" -ne "$before" ]; then
    echo "parity_selftest: NO TEETH removing $TARGET left unmapped at $restored (want $before)"
    rc=1
fi
if [ "$rc" -eq 0 ]; then
    echo "parity_selftest: HAS TEETH $before -> $after -> $restored on one target"
fi
exit "$rc"
