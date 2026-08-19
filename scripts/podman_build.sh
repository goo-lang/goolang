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
    # Resolve to an absolute path BEFORE any podman call. A relative <outdir>
    # is a valid podman NAMED VOLUME name, so `--volume "$OUT:/out:z"` with a
    # relative $OUT silently creates (or reuses) a volume of that name instead
    # of bind-mounting the host directory. The run then still exits 0, the
    # host directory stays empty, and the artifacts land inside the volume
    # where nothing later looks for them.
    OUT="$(cd "$OUT" && pwd)"
    TAR="$(mktemp)"; trap 'rm -f "$TAR"' EXIT
    ( cd "$ROOT" && git archive HEAD ) > "$TAR"

    # A single `podman run`, with the tar piped straight to the container's
    # stdin. An earlier version of this script used `podman create` /
    # `podman cp` / `podman start` instead, on the belief that stdin was
    # needed for a piped setup script and so was unavailable for the tar
    # stream. That belief was wrong: the `sh -c` payload below is an
    # ARGUMENT, not something read from stdin, so stdin was free the whole
    # time and the four-command sequence bought nothing. `--rm` removes the
    # container on every exit path, including a SIGINT, so no separate
    # `podman rm` is needed either.
    #
    # $MAKEVARS is not used here even though it holds the same two tokens: it
    # is a shell variable, and this payload is single-quoted so it reaches the
    # container literally, unexpanded. Quoting it in a way that would expand
    # inside a single-quoted string hurts readability for a two-token literal,
    # so the flags are written out again below instead.
    podman run --rm -i \
      --env SOURCE_DATE_EPOCH="$(cd "$ROOT" && git log -1 --format=%ct)" \
      --volume "$OUT:/out:z" \
      "$IMAGE" sh -c '
        mkdir -p /src
        tar -x -C /src || { echo "archive extraction FAILED" >&2; exit 1; }
        cd /src
        make CCACHE= CC=gcc-14 -j"$(nproc)" bin/goo lib/libgoo_runtime.a >/tmp/build.log 2>&1 || {
          echo "build FAILED"; tail -40 /tmp/build.log; exit 1; }
        cp bin/goo /out/goo
        cp lib/libgoo_runtime.a /out/libgoo_runtime.a' < "$TAR"
    rc=$?
    exit $rc
    ;;

  dev)
    shift
    # -i keeps stdin open; -t allocates a pty only when the caller has one, so
    # a non-interactive caller (a script, a CI step) does not trip podman's
    # "input device is not a TTY" warning.
    TTY_FLAG=""
    [ -t 0 ] && TTY_FLAG="-t"
    podman run --rm -i $TTY_FLAG \
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
