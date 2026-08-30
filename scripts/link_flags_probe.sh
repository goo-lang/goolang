#!/usr/bin/env bash
# --linker / --link-flag passthrough. codegen_emit_executable forks its OWN
# linker to build a compiled Goo program (src/codegen/codegen.c). Until this
# landed the program name was hardcoded and no flag reached it, so an
# instrumented libgoo_runtime.a could not link and src/runtime had to opt out
# of every sanitizer (.bazelrc, NO_SANITIZE_COPTS in src/runtime/BUILD).
#
# THE NEGATIVE CASES ARE THE POINT. A silently discarded flag still satisfies
# "the build works", so each passthrough is proven by a value that MUST break
# the link, and by that value appearing in the echoed link command. A missing
# "Usage:" separates a real link failure from goo rejecting its own argv --
# both exit non-zero, and only one of them is evidence.
#
# --self-test proves THIS script can go red, by running it against stub
# compilers that break the passthrough three different ways.

set -u

fail() { echo "FAIL: $1"; exit 1; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# The self-test substitutes a stub here. Nothing else sets it.
COMPILER="${LINK_FLAGS_PROBE_COMPILER:-$ROOT/bin/goo}"

[ -x "$COMPILER" ] || fail "compiler not found at $COMPILER (run 'make')"

# --------------------------------------------------------------------------
# Three injections plus a CONTROL. Without the control, a stub harness that is
# simply broken turns every injection red and reads as success.
# --------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
    SELF="link-flags-probe --self-test"
    W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
    bad_self=0

    # Write a stub compiler whose body ($1) runs before the real goo does.
    stub() {
        cat > "$W/goo" <<STUB
#!/usr/bin/env bash
$1
exec "$COMPILER" "\$@"
STUB
        chmod +x "$W/goo"
    }
    run() { LINK_FLAGS_PROBE_COMPILER="$W/goo" "$0" >"$W/out.log" 2>&1; }

    stub ':'
    if run; then echo "    ok: control (pass-through stub) is GREEN"
    else echo "$SELF: FAIL (control already red -- the harness is broken, not the tree)"
         sed 's/^/        /' "$W/out.log"; exit 1; fi

    check() {
        stub "$2"
        if run; then echo "$SELF: FAIL (stayed green after: $1)"
             sed 's/^/        /' "$W/out.log"; bad_self=1
        else echo "    ok: '$1' turns it red"; fi
    }

    # The defect this probe exists to catch: the flags parse but never reach
    # the linker, so both bogus values link cleanly.
    check "a compiler that silently discards both flags" \
        'a=(); for x in "$@"; do case "$x" in --linker=*|--link-flag=*) ;; *) a+=("$x");; esac; done; set -- "${a[@]}"'

    # Proves the "Usage:" guard has teeth. Without that guard this injection
    # stays GREEN: goo rejecting its own argv also exits non-zero.
    check "a compiler that rejects both flags with a usage banner" \
        'for x in "$@"; do case "$x" in --linker=*|--link-flag=*) echo "Usage: goo [options]" >&2; exit 1;; esac; done'

    # Proves the positive control has teeth, not just the negative cases.
    check "a compiler whose every link fails" \
        'echo "Linking failed with command: stub" >&2; exit 1'

    [ "$bad_self" = 0 ] || exit 1
    echo "PASS: $SELF (control green, 3 injections red)"
    exit 0
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

SRC="$WORKDIR/hello.goo"
printf 'package main\nfunc main() {}\n' > "$SRC"

RC=0
LOG=""
EXE=""
# Compile $SRC with the remaining arguments. Sets RC, LOG and EXE.
compile_case() {
    local name="$1"; shift
    EXE="$WORKDIR/$name.out"
    LOG="$WORKDIR/$name.log"
    "$COMPILER" "$SRC" -o "$EXE" "$@" > "$LOG" 2>&1
    RC=$?
}

# The link must succeed and the binary must run.
expect_links() {
    local name="$1"; shift
    compile_case "$name" "$@"
    if [ "$RC" != 0 ]; then
        sed 's/^/    /' "$LOG"; fail "$name: compile exited $RC, expected 0"
    fi
    [ -x "$EXE" ] || fail "$name: no executable at $EXE"
    "$EXE" >/dev/null 2>&1 || fail "$name: the linked binary exited $?"
    echo "  ok: $name"
}

# The link must fail, name the offending value, and not be an argv rejection.
expect_link_failure() {
    local name="$1" needle="$2"; shift 2
    compile_case "$name" "$@"
    [ "$RC" != 0 ] || fail "$name: compile exited 0; the flag was discarded"
    grep -q "Usage:" "$LOG" && fail "$name: goo rejected its own argv, not the link"
    grep -q "Linking failed with command:" "$LOG" \
        || fail "$name: no link-failure diagnostic; the failure is not the link"
    grep -qF -- "$needle" "$LOG" \
        || fail "$name: '$needle' is absent from the link command"
    [ -e "$EXE" ] && fail "$name: a binary was emitted despite the failed link"
    echo "  ok: $name (link failed, and names $needle)"
}

# 1. Positive control. Without it, a compiler broken for any other reason
#    would make every negative case below pass for the wrong cause.
expect_links default_links

# 2. Naming the default linker explicitly must behave as the default does.
expect_links explicit_linker --linker=gcc

# 3. A benign flag must reach the linker without breaking a good build.
expect_links benign_link_flag --link-flag=-Wl,--as-needed

# 4. Teeth for --linker.
expect_link_failure bad_linker "$WORKDIR/no-such-linker" \
    --linker="$WORKDIR/no-such-linker"

# 5. Teeth for --link-flag.
expect_link_failure bad_link_flag -Wl,--goo-no-such-option \
    --link-flag=-Wl,--goo-no-such-option

echo "PASS: --linker and --link-flag reach the forked linker"
exit 0
