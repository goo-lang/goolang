#!/bin/bash
# Run a goolang build inside the pinned image.
#
# TWO MODES, because a gate and a person want different things.
#
#   gate <outdir>  Source is COPIED IN from `git archive HEAD`. No host writes,
#                  so rootless-podman file ownership and SELinux labelling never
#                  arise. `git archive` also excludes build/, bin/ and lib/,
#                  which matters: a recursive copy would carry stale artifacts in
#                  and make would treat them as up to date, so two "clean" builds
#                  would compare the same untouched files and pass forever.
#
#   dev [args...]  Source is MOUNTED. Iteration without a rebuild. `:z` is
#                  required on a Fedora host for SELinux; --userns=keep-id stops
#                  root-owned files landing in build/ and bin/.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${GOO_BUILD_IMAGE:-goolang-build:local}"
MODE="${1:-}"

# CCACHE= is passed on every make line. Makefile:7 is
# `CCACHE ?= $(shell command -v ccache)`, so a future image that gained ccache
# would silently start replaying cached objects.
MAKEVARS='CCACHE= CC=gcc-14'

case "$MODE" in
  gate)
    OUT="${2:-}"
    if [ -z "$OUT" ]; then echo "usage: $0 gate <outdir>" >&2; exit 2; fi
    mkdir -p "$OUT"
    TAR="$(mktemp)"; trap 'rm -f "$TAR"' EXIT
    ( cd "$ROOT" && git archive HEAD ) > "$TAR"

    # `podman cp` feeds the tarball in, so the container's own stdin stays free.
    # Piping the script to `sh -s` AND the tar to stdin cannot both work.
    #
    # $MAKEVARS is not used here even though it holds the same two tokens: it
    # is a shell variable, and this payload is single-quoted so it reaches the
    # container literally, unexpanded. Quoting it in a way that would expand
    # inside a single-quoted string hurts readability for a two-token literal,
    # so the flags are written out again below instead.
    CID="$(podman create \
      --env SOURCE_DATE_EPOCH="$(cd "$ROOT" && git log -1 --format=%ct)" \
      --volume "$OUT:/out:z" \
      "$IMAGE" sh -c '
        cd /src
        make CCACHE= CC=gcc-14 -j"$(nproc)" bin/goo lib/libgoo_runtime.a >/tmp/build.log 2>&1 || {
          echo "build FAILED"; tail -40 /tmp/build.log; exit 1; }
        cp bin/goo /out/goo
        cp lib/libgoo_runtime.a /out/libgoo_runtime.a')"
    # A container fresh from `podman create` sits in state "created": its
    # storage layer is not mounted yet, and on this podman (5.8.4, rootless)
    # `podman cp` into it fails with "could not be found on container" for
    # any destination, even an existing directory. `container init` mounts
    # the layer without running the entrypoint, which is exactly what a copy
    # target needs. Confirmed by direct test: `podman cp` to a freshly
    # created container fails every time without this line, and succeeds
    # every time with it.
    podman container init "$CID"
    podman cp - "$CID:/src" < "$TAR"
    podman start -a "$CID"; rc=$?
    podman rm -f "$CID" >/dev/null 2>&1
    exit $rc
    ;;

  dev)
    shift
    podman run --rm -it \
      --volume "$ROOT:/src:z" \
      --userns=keep-id \
      --env SOURCE_DATE_EPOCH="$(cd "$ROOT" && git log -1 --format=%ct)" \
      "$IMAGE" make $MAKEVARS "$@"
    ;;

  *)
    echo "usage: $0 {gate <outdir>|dev [make-args...]}" >&2
    exit 2
    ;;
esac
