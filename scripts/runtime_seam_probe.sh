#!/usr/bin/env bash
# runtime-seam-probe — the scheduler and channel logic reach the OS through
# include/goo_os.h only. A raw pthread_ call or a _Thread_local in these files
# escapes the simulator (tests/sim) silently, so this grep keeps the seam whole.
set -u
PROBE="runtime-seam-probe"
ROOT="${RUNTIME_SEAM_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
FILES="src/runtime/concurrency.c src/runtime/channels.c src/runtime/sync.c"

scan() {  # $1 = root; prints offending lines; exit 0 if none
    local root="$1" hits
    hits="$(cd "$root" && grep -n -E 'pthread_|_Thread_local' $FILES | grep -v -E '^[^:]+:[0-9]+:\s*//' )"
    if [ -n "$hits" ]; then echo "$hits"; return 1; fi
    return 0
}

if [ "${1:-}" = "--self-test" ]; then
    SELF="$PROBE --self-test"
    W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
    mkdir -p "$W/src/runtime"
    for f in $FILES; do cp "$ROOT/$f" "$W/$f"; done
    # Control: the real tree is green.
    if ! scan "$W" >/dev/null; then echo "$SELF: FAIL (control: the real tree already has a raw call)"; exit 1; fi
    echo "    ok: the control copy is clean"
    # Mutant 1: a raw pthread call in channels.c must be caught.
    printf 'static void zz(void) { pthread_mutex_lock(0); }\n' >> "$W/src/runtime/channels.c"
    if scan "$W" >/dev/null; then echo "$SELF: FAIL (a raw pthread_mutex_lock in channels.c was not caught)"; exit 1; fi
    echo "    ok: a raw pthread_mutex_lock is caught"
    cp "$ROOT/src/runtime/channels.c" "$W/src/runtime/channels.c"
    # Mutant 2: a thread-local in concurrency.c must be caught.
    printf 'static _Thread_local int zz;\n' >> "$W/src/runtime/concurrency.c"
    if scan "$W" >/dev/null; then echo "$SELF: FAIL (a _Thread_local in concurrency.c was not caught)"; exit 1; fi
    echo "    ok: a _Thread_local is caught"
    echo "$SELF: PASS (3 files, 2 mutations red, control green)"
    exit 0
fi

if out="$(scan "$ROOT")"; then
    echo "$PROBE: PASS ($FILES: no pthread_ token, no _Thread_local, tree $ROOT)"
    exit 0
fi
echo "$PROBE: FAIL (raw OS use in the runtime logic files)"; echo "$out" | sed 's/^/    /'; exit 1
