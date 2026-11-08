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

# explain_mismatch <artefact-name> <A-path> <B-path>
#
# The cmp verdict and the two sha256 hashes printed by the caller stay
# exactly as they are — this only ADDS a report of WHERE the bytes differ,
# printed after them. It never changes the FAIL verdict or its exit code.
#
# Tool priority: diffoscope when it is on PATH (it already does both member-
# and instruction-level diffing, for both artefact kinds, better than the
# fallback below). Otherwise: an `ar`+sha256sum member walk for an archive,
# `llvm-objdump`/`objdump` section and disassembly diffs for a binary. With
# none of those tools present, say so plainly instead of guessing.
#
# Every branch caps its own output, and the whole function stays well under
# 150 lines, so one huge mismatch cannot flood the log that a person reads.
explain_mismatch() {
    local artefact="$1" a="$2" b="$3"
    echo "  mismatch report for $artefact:"

    if command -v diffoscope >/dev/null 2>&1; then
        echo "    (report tool: diffoscope)"
        diffoscope --max-diff-block-lines 40 "$a" "$b" 2>&1 | head -60 | sed 's/^/    /'
        return 0
    fi

    case "$artefact" in
    *.a)
        if ! command -v ar >/dev/null 2>&1 || ! command -v sha256sum >/dev/null 2>&1; then
            echo "    (report tool: NONE — diffoscope and ar/sha256sum are all absent from PATH)"
            return 0
        fi
        echo "    (report tool: ar + sha256sum member walk — diffoscope not on PATH)"
        local ta tb only_a only_b common m ha hb differing=0 total=0 first=""
        ta="$(mktemp)"; tb="$(mktemp)"
        ar t "$a" | sort >"$ta"
        ar t "$b" | sort >"$tb"
        only_a="$(comm -23 "$ta" "$tb")"
        only_b="$(comm -13 "$ta" "$tb")"
        if [ -n "$only_a" ]; then
            echo "    member(s) only in A:"; printf '%s\n' "$only_a" | head -10 | sed 's/^/      /'
        fi
        if [ -n "$only_b" ]; then
            echo "    member(s) only in B:"; printf '%s\n' "$only_b" | head -10 | sed 's/^/      /'
        fi
        common="$(comm -12 "$ta" "$tb")"
        while IFS= read -r m; do
            [ -z "$m" ] && continue
            total=$((total + 1))
            ha="$(ar p "$a" "$m" 2>/dev/null | sha256sum | cut -d' ' -f1)"
            hb="$(ar p "$b" "$m" 2>/dev/null | sha256sum | cut -d' ' -f1)"
            if [ "$ha" != "$hb" ]; then
                differing=$((differing + 1))
                [ -z "$first" ] && first="$m"
            fi
        done <<<"$common"
        rm -f "$ta" "$tb"
        if [ -n "$first" ]; then
            echo "    first differing member: $first"
            echo "    $differing of $total common member(s) differ"
        else
            echo "    every common member is byte-identical — the mismatch is in archive metadata (member order, timestamps, index) rather than any member's content"
        fi
        ;;
    *)
        local objdump="" ha hb da db first_hunk line func
        if command -v llvm-objdump >/dev/null 2>&1; then
            objdump="llvm-objdump"
        elif command -v objdump >/dev/null 2>&1; then
            objdump="objdump"
        else
            echo "    (report tool: NONE — diffoscope and llvm-objdump/objdump are all absent from PATH)"
            return 0
        fi
        echo "    (report tool: $objdump — diffoscope not on PATH)"

        # The "file format" line embeds each build's own tmp-dir path, so it
        # always differs even when the bytes that matter do not; filtered out
        # so it cannot masquerade as the first real difference below.
        ha="$(mktemp)"; hb="$(mktemp)"
        "$objdump" -h "$a" 2>&1 | grep -v 'file format' >"$ha"
        "$objdump" -h "$b" 2>&1 | grep -v 'file format' >"$hb"
        echo "    section table diff:"
        diff -u "$ha" "$hb" | head -40 | sed 's/^/      /'
        rm -f "$ha" "$hb"

        da="$(mktemp)"; db="$(mktemp)"
        "$objdump" -d --no-show-raw-insn "$a" 2>&1 | grep -v 'file format' >"$da"
        "$objdump" -d --no-show-raw-insn "$b" 2>&1 | grep -v 'file format' >"$db"
        first_hunk="$(diff "$da" "$db" | head -1)"
        if [ -n "$first_hunk" ]; then
            line="$(printf '%s' "$first_hunk" | sed -E 's/^([0-9]+).*/\1/')"
            func="$(sed -n "1,${line}p" "$da" | tac | grep -m1 -E '^[0-9a-f]+ <.*>:' | sed -E 's/^[0-9a-f]+ <(.*)>:.*/\1/')"
            if [ -n "$func" ]; then
                echo "    first differing function: $func"
            else
                echo "    first difference falls before any function label (data or header)"
            fi
        fi
        echo "    disassembly diff (first 40 lines):"
        diff "$da" "$db" | head -40 | sed 's/^/      /'
        rm -f "$da" "$db"
        ;;
    esac
}

# THE VERDICT MUST NAME WHAT IT TESTED.
#
# This probe builds `git archive HEAD`, never the working tree. That is Limit 6
# of the design doc, it was documented and unenforced, and it produced a real
# false conclusion on 2026-08-20: a change sat uncommitted, the probe passed,
# and the PASS was read as evidence about the change. It was evidence about
# HEAD.
#
# A banner at the top would have scrolled past. The commit and the dirty count
# therefore go in the VERDICT LINE -- the one line a person reads, copies into
# a report, and quotes. `git diff --stat HEAD` covers tracked modifications;
# untracked files cannot reach `git archive HEAD` either, so they are counted
# too.
tested_what() {
	local sha dirty untracked
	sha="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
	dirty="$(git -C "$ROOT" diff --name-only HEAD 2>/dev/null | grep -c . || true)"
	untracked="$(git -C "$ROOT" ls-files --others --exclude-standard 2>/dev/null | grep -c . || true)"
	local excluded=$(( ${dirty:-0} + ${untracked:-0} ))
	if [ "$excluded" -gt 0 ]; then
		printf 'tested %s; WARNING %s local change(s) were NOT in this build' "$sha" "$excluded"
	else
		printf 'tested %s, worktree clean' "$sha"
	fi
}

if [ "${1:-}" = "--self-test" ]; then
    # FAST CASE FIRST. explain_mismatch's archive path needs no container: two
    # tiny local archives, two members each, one member's content differing,
    # prove the ar/sha256sum member walk names the right member before paying
    # for the container case below.
    FT="$(mktemp -d)"
    mkdir -p "$FT/a" "$FT/b"
    printf 'same content\n' >"$FT/a/one.o"
    printf 'same content\n' >"$FT/b/one.o"
    printf 'A content\n' >"$FT/a/two.o"
    printf 'B content\n' >"$FT/b/two.o"
    ( cd "$FT/a" && ar rcs libtest.a one.o two.o ) >/dev/null 2>&1
    ( cd "$FT/b" && ar rcs libtest.a one.o two.o ) >/dev/null 2>&1
    FT_OUT="$(explain_mismatch "libtest.a" "$FT/a/libtest.a" "$FT/b/libtest.a")"
    rm -rf "$FT"
    if ! printf '%s\n' "$FT_OUT" | grep -q 'first differing member: two.o'; then
        echo "$PROBE --self-test: FAIL (fast unit case did not name two.o as the differing member)"
        printf '%s\n' "$FT_OUT" | sed 's/^/    /'
        exit 1
    fi
    echo "    ok: fast unit case (two local archives) names two.o as the differing member"

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
    #
    # The identity is passed EXPLICITLY rather than inherited. A CI runner has
    # no user.email, and git then refuses with "empty ident name". This script
    # runs under `set -u` and not `set -e`, so that refusal was silent: HEAD
    # kept the clean tree, the inner probe honestly reported IDENTICAL, and the
    # self-test reported FAIL. That reads as "the probe cannot go red" when the
    # truth is "the self-test never ran". Measured on this workflow's first CI
    # run, which is also the first time it ever executed off this machine.
    git -C "$CLONE" add Makefile "$VICTIM"
    if ! git -C "$CLONE" \
            -c commit.gpgsign=false \
            -c user.email=repro-probe@localhost \
            -c user.name='repro-build-probe self-test' \
            commit -q --no-verify -m "TEMP: self-test injection"; then
        echo "$PROBE --self-test: FAIL (could not commit the injection)"
        exit 1
    fi

    # VERIFY THE INJECTION REACHED THE SURFACE THE PROBE ACTUALLY READS.
    # The work-tree grep above is necessary and NOT sufficient, and the gap
    # between the two is exactly what hid this bug: the probe builds
    # `git archive HEAD`, so a mutation that is in the tree but not in the
    # commit leaves the tree looking correct and the archive clean.
    if ! git -C "$CLONE" show 'HEAD:Makefile' 2>/dev/null | grep -q 'GOO_SELFTEST_NONCE' \
       || ! git -C "$CLONE" show "HEAD:$VICTIM" 2>/dev/null | grep -q 'goo_selftest_nonce'; then
        echo "$PROBE --self-test: FAIL (injection is in the work tree but not in HEAD, which is what the probe builds)"
        exit 1
    fi

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
    # The injection lands ONLY in runtime.o's compile, so a correct report
    # must name that member specifically -- not merely say SOMETHING differs.
    if ! grep -q 'first differing member: runtime.o' "$ST/out.log"; then
        echo "$PROBE --self-test: FAIL (probe went DIFFER but its report did not name runtime.o as the differing member)"
        sed 's/^/    /' "$ST/out.log"
        exit 1
    fi
    echo "$PROBE --self-test: PASS (nonce-driven rebuild was reported DIFFER, and the report named runtime.o; probe can go red) [$(tested_what)]"
    echo "    report excerpt:"
    grep -A2 'mismatch report for libgoo_runtime.a' "$ST/out.log" | sed 's/^/    /'
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
        explain_mismatch "$art" "$a" "$b"
        rc=1
    fi
done

if [ "$rc" -ne 0 ]; then
    echo "$PROBE: FAIL — the build is not reproducible."
    exit 1
fi
echo "$PROBE: PASS (bin/goo and lib/libgoo_runtime.a byte-identical across two containers) [$(tested_what)]"
exit 0
