#!/bin/bash
# podman-image probe — the build image must carry the exact CI toolchain, must
# NOT carry ccache, and must record what it resolved.
#
# ccache is excluded deliberately. It gives nothing to a one-shot build, and its
# presence lets a determinism gate pass for the wrong reason: with ccache active
# a second build replays cached objects and reports IDENTICAL whatever the
# compiler does.
set -u

PROBE="podman-image-probe"
IMAGE="${GOO_BUILD_IMAGE:-goolang-build:local}"

if ! command -v podman >/dev/null 2>&1; then
    echo "$PROBE: SKIPPED (podman not on PATH)"
    exit 0
fi

if ! podman image exists "$IMAGE" 2>/dev/null; then
    echo "$PROBE: FAIL ($IMAGE is not built — run 'make podman-image')"
    exit 1
fi

fail=0
need() {  # $1 = command that must exist in the image
    if podman run --rm "$IMAGE" sh -c "command -v $1 >/dev/null 2>&1"; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1"; fail=1
    fi
}
for c in make gcc-14 bison clang valgrind cmake python3; do need "$c"; done

if podman run --rm "$IMAGE" sh -c 'command -v ccache >/dev/null 2>&1'; then
    echo "  PRESENT BUT MUST NOT BE: ccache"
    fail=1
else
    echo "  ok: ccache absent"
fi

TOOLCHAIN_FILE=/etc/goo-toolchain.txt
toolchain_content="$(podman run --rm "$IMAGE" cat "$TOOLCHAIN_FILE" 2>/dev/null)"
if [ -z "$toolchain_content" ]; then
    echo "  MISSING: $TOOLCHAIN_FILE"; fail=1
else
    echo "  ok: $TOOLCHAIN_FILE recorded"
    # Non-empty is not enough on its own: the recording step used to be able
    # to lose one tool's line silently and still leave a plausible-looking
    # file behind (see Containerfile / scripts/record_toolchain.sh). Check
    # each tool's own line is actually present, and name the one that is not.
    for tool in gcc-14 clang bison valgrind cmake python3 llvm-config; do
        if printf '%s\n' "$toolchain_content" | grep -q "^$tool: "; then
            echo "  ok: $TOOLCHAIN_FILE records $tool"
        else
            echo "  MISSING line for $tool in $TOOLCHAIN_FILE"; fail=1
        fi
    done
fi

if [ "$fail" -ne 0 ]; then
    echo "$PROBE: FAIL"
    exit 1
fi
echo "$PROBE: PASS (toolchain present, ccache absent, versions recorded)"
exit 0
