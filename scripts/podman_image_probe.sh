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

if podman run --rm "$IMAGE" test -s /etc/goo-toolchain.txt; then
    echo "  ok: /etc/goo-toolchain.txt recorded"
else
    echo "  MISSING: /etc/goo-toolchain.txt"; fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "$PROBE: FAIL"
    exit 1
fi
echo "$PROBE: PASS (toolchain present, ccache absent, versions recorded)"
exit 0
