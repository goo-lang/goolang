#!/usr/bin/env bash
# runtime-seam-probe — the scheduler and channel logic reach the OS through
# include/goo_os.h only. A raw pthread_ call or a _Thread_local in these files
# escapes the simulator (tests/sim) silently, so this grep keeps the seam whole.
set -u
PROBE="runtime-seam-probe"
ROOT="${RUNTIME_SEAM_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
FILES="src/runtime/concurrency.c src/runtime/channels.c src/runtime/sync.c"
# C23 thread_local and GNU __thread are the other two spellings of a TLS slot.
PATTERN='pthread_|_Thread_local|\bthread_local\b|\b__thread\b'

# Prints offending lines. Exit 0 = none, 1 = hits, 2 = TOOL FAILURE (a
# scanned file is missing, or grep itself failed). No pipe: a pipe would hide
# grep's exit status 2 behind the last stage.
scan() {
    local root="$1" hits f rc
    for f in $FILES; do
        if [ ! -f "$root/$f" ]; then
            echo "$PROBE: TOOL FAILURE ($f does not exist under $root)"
            return 2
        fi
    done
    hits="$(cd "$root" && grep -n -E "$PATTERN" $FILES)"
    rc=$?
    if [ "$rc" -gt 1 ]; then
        echo "$PROBE: TOOL FAILURE (grep exited $rc)"
        return 2
    fi
    if [ "$rc" -eq 1 ]; then return 0; fi
    # Drop whole-line // comments. grep -v exits 1 when nothing is left.
    hits="$(printf '%s\n' "$hits" | grep -v -E '^[^:]+:[0-9]+:[[:space:]]*//')"
    rc=$?
    if [ "$rc" -gt 1 ]; then
        echo "$PROBE: TOOL FAILURE (comment filter exited $rc)"
        return 2
    fi
    if [ "$rc" -eq 0 ]; then echo "$hits"; return 1; fi
    return 0
}

if [ "${1:-}" = "--self-test" ]; then
    SELF="$PROBE --self-test"
    W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
    mkdir -p "$W/src/runtime"
    for f in $FILES; do cp "$ROOT/$f" "$W/$f"; done
    # Control: the real tree is green.
    scan "$W" >/dev/null; rc=$?
    if [ "$rc" -ne 0 ]; then echo "$SELF: FAIL (control: rc $rc on the real tree)"; exit 1; fi
    echo "    ok: the control copy is clean"
    # Mutant 1: a raw pthread call in channels.c must be caught.
    printf 'static void zz(void) { pthread_mutex_lock(0); }\n' >> "$W/src/runtime/channels.c"
    scan "$W" >/dev/null; rc=$?
    if [ "$rc" -ne 1 ]; then echo "$SELF: FAIL (a raw pthread_mutex_lock in channels.c: rc $rc, want 1)"; exit 1; fi
    echo "    ok: a raw pthread_mutex_lock is caught"
    cp "$ROOT/src/runtime/channels.c" "$W/src/runtime/channels.c"
    # Mutants 2-4: each TLS spelling in concurrency.c must be caught.
    for decl in 'static _Thread_local int zz;' 'static thread_local int zz;' 'static __thread int zz;'; do
        cp "$ROOT/src/runtime/concurrency.c" "$W/src/runtime/concurrency.c"
        printf '%s\n' "$decl" >> "$W/src/runtime/concurrency.c"
        scan "$W" >/dev/null; rc=$?
        if [ "$rc" -ne 1 ]; then echo "$SELF: FAIL ('$decl' in concurrency.c: rc $rc, want 1)"; exit 1; fi
        echo "    ok: '$decl' is caught"
    done
    cp "$ROOT/src/runtime/concurrency.c" "$W/src/runtime/concurrency.c"
    # Mutant 5: a missing scanned file must be a tool failure, never a pass.
    rm "$W/src/runtime/sync.c"
    scan "$W" >/dev/null; rc=$?
    if [ "$rc" -ne 2 ]; then echo "$SELF: FAIL (a missing sync.c: rc $rc, want 2)"; exit 1; fi
    echo "    ok: a missing file is a tool failure"
    cp "$ROOT/src/runtime/sync.c" "$W/src/runtime/sync.c"
    echo "$SELF: PASS (3 files, 5 mutations red, control green)"
    exit 0
fi

out="$(scan "$ROOT")"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "$PROBE: PASS ($FILES: no pthread_, _Thread_local, thread_local or __thread token, tree $ROOT)"
    exit 0
fi
if [ "$rc" -eq 2 ]; then echo "$out"; exit 2; fi
echo "$PROBE: FAIL (raw OS use in the runtime logic files)"; echo "$out" | sed 's/^/    /'; exit 1
