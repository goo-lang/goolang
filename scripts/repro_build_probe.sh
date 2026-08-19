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

    # NOT __TIME__/__DATE__/__TIMESTAMP__: podman_build.sh sets
    # SOURCE_DATE_EPOCH, and GCC replaces those three macros with a FIXED
    # string derived from it — identical between build A and build B of the
    # same commit. An injection built on any of them adds no nondeterminism
    # at all; it silently disables these teeth. A per-build random nonce,
    # read from /dev/urandom by `make` itself inside each container, cannot
    # be flattened that way, because SOURCE_DATE_EPOCH only touches macros
    # that read the clock — it has no opinion on a value make computes from
    # entropy.
    #
    # The nonce is scoped to runtime.o's own compile via a target-specific
    # variable (the existing pattern at this Makefile's
    # `$(BUILDDIR)/runtime/far_transport.o: CFLAGS += ...` line), so it
    # cannot perturb any other object file's flags.
    cat >> "$CLONE/Makefile" <<'MAKEFRAG'

# repro-build-probe --self-test injection: force runtime.o to embed a fresh
# random value on every `make` invocation, so two separate container builds
# of this exact commit cannot produce a byte-identical archive.
$(BUILDDIR)/runtime/runtime.o: CFLAGS += -DGOO_SELFTEST_NONCE=$(shell od -An -N4 -tu4 /dev/urandom | tr -d " ")
MAKEFRAG

    cat >> "$CLONE/$VICTIM" <<'CFRAG'

#ifdef GOO_SELFTEST_NONCE
const unsigned int goo_selftest_nonce = GOO_SELFTEST_NONCE;
#endif
CFRAG

    # VERIFY THE INJECTION LANDED, in BOTH files. A mutation that fails to
    # apply reads exactly like a passing probe, so a red result is only
    # meaningful once this holds.
    if ! grep -q 'GOO_SELFTEST_NONCE' "$CLONE/Makefile" || ! grep -q 'goo_selftest_nonce' "$CLONE/$VICTIM"; then
        echo "$PROBE --self-test: FAIL (the injection did not land; a red result here would be meaningless)"
        echo "    Makefile has GOO_SELFTEST_NONCE: $(grep -c GOO_SELFTEST_NONCE "$CLONE/Makefile" 2>/dev/null || echo 0)"
        echo "    $VICTIM has goo_selftest_nonce: $(grep -c goo_selftest_nonce "$CLONE/$VICTIM" 2>/dev/null || echo 0)"
        exit 1
    fi

    # The gate builds from `git archive HEAD`, so the mutation must be committed
    # to reach the container. --no-verify skips the clone's pre-commit hook.
    git -C "$CLONE" add Makefile "$VICTIM"
    git -C "$CLONE" -c commit.gpgsign=false commit -q --no-verify -m "TEMP: self-test injection"

    ( cd "$CLONE" && ./scripts/repro_build_probe.sh ) >"$ST/out.log" 2>&1
    st=$?
    # Positive evidence, not just a non-zero exit code: a build error, a
    # missing artifact, or an equal-inode failure all exit non-zero too, and
    # once the injection genuinely works it must not be credited for a
    # failure it did not cause.
    if [ "$st" -eq 0 ] || ! grep -q 'DIFFER:' "$ST/out.log"; then
        echo "$PROBE --self-test: FAIL (probe did not report DIFFER on a deliberately nondeterministic build; inner exit=$st)"
        sed 's/^/    /' "$ST/out.log"
        exit 1
    fi
    echo "$PROBE --self-test: PASS (nonce-driven rebuild was reported DIFFER; probe can go red)"
    exit 0
fi

if ! command -v podman >/dev/null 2>&1; then
    echo "$PROBE: SKIPPED (podman not on PATH)"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Minimum plausible size for either artifact, in bytes. The real artifacts run
# about 2.1 MB (goo) and 1.5 MB (libgoo_runtime.a); this floor only exists to
# catch a truncated or near-empty file, not to model either real size.
MIN_SIZE=100000

build() {  # $1 = output dir, $2 = log file
    if ! "$ROOT/scripts/podman_build.sh" gate "$1" >"$2" 2>&1; then
        echo "$PROBE: FAIL (build into $1 errored; see $2)"
        tail -30 "$2"
        return 1
    fi
}

# Each build gets its own log — a shared log name means build B's log
# overwrites build A's, and that log is the whole diagnosis on a failure.
build "$WORK/A" "$WORK/build-A.log" || exit 1
build "$WORK/B" "$WORK/build-B.log" || exit 1

rc=0
for art in goo libgoo_runtime.a; do
    a="$WORK/A/$art"; b="$WORK/B/$art"
    if [ ! -f "$a" ] || [ ! -f "$b" ]; then
        echo "  MISSING: $art was not produced by both builds"; rc=1; continue
    fi
    # Guard against a build that silently produced an empty or truncated
    # artifact: a tiny file would still pass the inode and cmp checks below
    # if both sides were truncated the same way, and that is exactly the
    # vacuous-pass shape this probe exists to refuse.
    sa="$(stat -c %s "$a")"; sb="$(stat -c %s "$b")"
    if [ "$sa" -lt "$MIN_SIZE" ] || [ "$sb" -lt "$MIN_SIZE" ]; then
        echo "  FAIL: $art is IMPLAUSIBLY SMALL (A=$sa bytes, B=$sb bytes, floor=$MIN_SIZE) — the build did not really produce it"; rc=1; continue
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
