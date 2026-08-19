#!/bin/bash
# repro-build probe — two builds of the same commit, in two fresh containers,
# must produce byte-identical bin/goo and lib/libgoo_runtime.a.
#
# THE FAILURE THIS GATE IS MOST LIKELY TO HAVE is that both builds write the
# same path and the script hashes one file twice, which passes forever. That is
# what --self-test exists to catch: it injects real nondeterminism and asserts
# this probe reports DIFFER.
#
# BOTH artifacts are compared. bin/goo does not depend on the runtime archive
# (`$(COMPILER): $(GOO_OBJS) $(COMPILER_SRCS)`), so building one proves nothing
# about the other. The archive is also where `ar` nondeterminism hid.
set -u

PROBE="repro-build-probe"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ "${1:-}" = "--self-test" ]; then
    # TEETH. Inject real nondeterminism and require this probe to notice.
    # A mutation that fails to apply reads exactly like a passing probe, so the
    # injection is verified before the red result is trusted.
    ST="$(mktemp -d)"; trap 'rm -rf "$ST"' EXIT
    CLONE="$ST/clone"
    VICTIM="src/runtime/runtime.c"

    # A local clone is hardlinked, so this is cheap. The working repository is
    # never written to.
    if ! git clone -q "$ROOT" "$CLONE" 2>"$ST/clone.log"; then
        echo "$PROBE --self-test: FAIL (could not clone the repository)"
        cat "$ST/clone.log"; exit 1
    fi

    printf '\nconst char *goo_selftest_stamp(void) { return __TIME__; }\n' >> "$CLONE/$VICTIM"

    # VERIFY THE INJECTION LANDED. A mutation that fails to apply reads exactly
    # like a passing probe, so a red result is only meaningful once this holds.
    if ! grep -q '__TIME__' "$CLONE/$VICTIM"; then
        echo "$PROBE --self-test: FAIL (the injection did not land; a red result here would be meaningless)"
        exit 1
    fi

    # The gate builds from `git archive HEAD`, so the mutation must be committed
    # to reach the container. --no-verify skips the clone's pre-commit hook.
    git -C "$CLONE" add "$VICTIM"
    git -C "$CLONE" -c commit.gpgsign=false commit -q --no-verify -m "TEMP: self-test injection"

    ( cd "$CLONE" && ./scripts/repro_build_probe.sh ) >"$ST/out.log" 2>&1
    st=$?
    if [ "$st" -eq 0 ]; then
        echo "$PROBE --self-test: FAIL (probe reported PASS on a deliberately nondeterministic build)"
        sed 's/^/    /' "$ST/out.log"
        exit 1
    fi
    echo "$PROBE --self-test: PASS (injected __TIME__ was detected; probe can go red)"
    exit 0
fi

if ! command -v podman >/dev/null 2>&1; then
    echo "$PROBE: SKIPPED (podman not on PATH)"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

build() {  # $1 = output dir
    if ! "$ROOT/scripts/podman_build.sh" gate "$1" >"$WORK/build.log" 2>&1; then
        echo "$PROBE: FAIL (build into $1 errored)"
        tail -30 "$WORK/build.log"
        return 1
    fi
}

build "$WORK/A" || exit 1
build "$WORK/B" || exit 1

rc=0
for art in goo libgoo_runtime.a; do
    a="$WORK/A/$art"; b="$WORK/B/$art"
    if [ ! -f "$a" ] || [ ! -f "$b" ]; then
        echo "  MISSING: $art was not produced by both builds"; rc=1; continue
    fi
    # Guard against a build that silently produced an empty or truncated
    # artifact: a size-zero (or absurdly small) file would still pass the
    # inode and cmp checks below if both sides were empty the same way, and
    # that is exactly the vacuous-pass shape this probe exists to refuse.
    sa="$(stat -c %s "$a")"; sb="$(stat -c %s "$b")"
    if [ "$sa" -eq 0 ] || [ "$sb" -eq 0 ]; then
        echo "  FAIL: $art is EMPTY (A=$sa bytes, B=$sb bytes) — the build did not really produce it"; rc=1; continue
    fi
    # Guard against the same-file bug: the two paths must be distinct inodes.
    if [ "$(stat -c %i "$a")" = "$(stat -c %i "$b")" ]; then
        echo "  FAIL: $art is the SAME FILE in both builds — the comparison is vacuous"; rc=1; continue
    fi
    if cmp -s "$a" "$b"; then
        echo "  ok: $art identical ($(sha256sum "$a" | cut -c1-16)..., $sa bytes)"
    else
        echo "  DIFFER: $art"
        echo "    A $(sha256sum "$a" | cut -d' ' -f1)"
        echo "    B $(sha256sum "$b" | cut -d' ' -f1)"
        rc=1
    fi
done

if [ "$rc" -ne 0 ]; then
    echo "$PROBE: FAIL — the build is not reproducible."
    exit 1
fi
echo "$PROBE: PASS (bin/goo and lib/libgoo_runtime.a byte-identical across two containers)"
exit 0
