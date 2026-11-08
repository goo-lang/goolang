#!/usr/bin/env bash
# sim_probe.sh — run one program under the simulator over N seeds, one process
# per seed, replay every non-pass, and report by verdict.
# Usage: scripts/sim_probe.sh <program> --seeds N [--first-seed K] [--expect VERDICT] [--fault-spurious-wake] [--fault-thread-create]
#        scripts/sim_probe.sh --self-test
#
# Task 6 of the 2026-09-03 deterministic-simulation plan
# (docs/superpowers/specs/2026-09-03-deterministic-simulation-design.md,
# sections 3 and 7). <program> names tests/concurrency/<program>.c or
# tests/sim/<program>.c; the Makefile's build/sim/% rules build it against the
# sim archive, and this script asks make for build/sim/<program>.
#
# VERDICTS are the simulator's exit codes, never the trace header's label: the
# runtime's own goo_deadlock_abort calls exit(2) on a coroutine, and the trace
# names that "exit", not runtime-deadlock-abort. The exit code is the one
# signal every ending carries. A usage error in the simulator exits 64, which
# is no verdict, and reads as crash-64.
#
# REPLAY. Every non-pass seed runs a second time, and its trace must be
# byte-identical to the first run's, with the same exit code. A seed whose
# replay differs reads replay-mismatch, and the probe exits 14 whatever else
# happened: a failure that does not reproduce from its seed is not a result.
#
# THE TRACE DIR is a fresh mktemp directory, never the repo. A trace is
# written on every ending, a pass included, and a full ring is about 2.5 MB of
# text, so the trace of each passing seed is deleted after its run; the trace
# of each non-pass seed is kept, because replay and the operator both read it.
# The summary reports the peak size of the trace dir. An all-pass run removes
# the dir.
#
# THE WAKE HOOK is UNSET by default (spec section 3): the seed, through the
# simulator's seeded preemption, must open the wake-to-unregister window, as a
# loaded OS does. The sim build of a tests/concurrency program sets
# GOO_SIM_BUILD, so false_deadlock_test runs with the hook unset. A
# GOO_TEST_WAKE_DELAY_NS in the environment passes through, and the verdict
# line then names it, so a run with a hand-set window is visible as one.
#
# THE PREEMPTION MODEL is the simulator's default unless GOO_SIM_PREEMPT_PPM
# is in the environment. The verdict line names the model as each trace
# header states it, never as this script assumes it: "preempt ppm=<n>" or
# "preempt off". Seeds whose headers name different models are a tool
# failure, because their verdicts do not add up.
#
# EXPECT. A seed SUCCEEDS when its verdict is the expected one: `pass` unless
# --expect names another verdict. deadlock_poll_test is a real deadlock by
# construction, so it runs with --expect runtime-deadlock-abort, and a seed
# that reads pass there FAILS. The replay rule does not change: every seed
# that does not exit 0 replays, an expected abort included, and a mismatch
# outranks the expectation (replay-mismatch can never be expected). A seed
# that succeeds and replays identically keeps no trace, as a pass does.
#
# FAULTS are off unless a flag turns one on. --fault-thread-create makes the
# seam fail one thread create in sixteen with EAGAIN. Its one target is
# goo_scheduler_init, which asserts that the create returned non-NULL, so the
# process stops there on SIGABRT -- a planned stop, as in Go, and not a runtime
# defect. With that flag on, and only then, that exact assertion reads
# expected-eagain-stop and does not fail the run.
#
# PROBE EXIT: 0 every seed passed (or, with --fault-thread-create, stopped at
# the expected assertion); 1 a seed failed; 14 a replay differed; 2 a tool
# failure (bad usage, a build that failed, a self-test pipeline that broke).
#
# TEETH. --self-test proves the probe can go red, three ways:
#   (a) a mutant sim archive whose goo_sched_wake_waiters unregisters nobody
#       -- the mutation scripts/false_deadlock_probe.sh applies -- linked with
#       the build/sim rule's flags (GOO_SIM_BUILD included), must abort
#       with runtime-deadlock-abort on at least one of seeds 1..256, and the
#       first such seed must replay byte for byte;
#   (b) build/sim/sim_invariant_test, which unlocks a sync.Mutex on a thread
#       that does not own it, must read invariant, detail unlock-not-owner;
#   (c) a control: the tracked runtime, built through the SAME pipeline as
#       the mutant, and build/sim/false_deadlock_test from make, must both pass
#       seeds 1..32. Without it, a broken pipeline turns (a) red and reads as
#       success.
# All three parts run with the hook unset and the default preemption model:
# the self-test clears every GOO_SIM_* and GOO_TEST_WAKE_DELAY_NS variable of
# the caller's environment first, so ambient state cannot change its answer.
set -u
PROBE="sim-probe"
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT" || exit 2
# A planned SIGABRT (the EAGAIN stop, a failed GOO_ASSERT) must not store a
# core per seed. A piped core_pattern (systemd-coredump) still runs: measured
# 2026-11-08, about 60 ms per abort, against about 11 ms for a passing seed.
ulimit -c 0

CC_="${CC_PROBE:-gcc}"
SCHED_SRC="src/runtime/concurrency.c"
SIM_LIB="build/libgoo_runtime_sim.a"
SIM_MAIN_OBJ="build/sim/sim_main.o"
# The flags the Makefile compiles a runtime object with, plus src/runtime for
# platform.h, because the mutant compiles from a copy outside that directory.
RT_CFLAGS="-std=c23 -g -D_GNU_SOURCE -I. -Iinclude -Isrc/runtime -include include/xalloc.h -include include/goo_assert.h"
# The two lines GOO_ASSERT prints when goo_scheduler_init sees a NULL thread.
EAGAIN_ASSERT_LINE="goo: ASSERT FAILED: g_scheduler->os_threads[i] != NULL"
EAGAIN_WHERE_RE="^  at src/runtime/concurrency\.c:[0-9]+ in goo_scheduler_init\(\)$"

FAULT_SPURIOUS=0
FAULT_THREAD_CREATE=0
EXPECT=pass
# The verdict names --expect accepts: every name verdict_name gives except
# replay-mismatch, which is the driver's own and never a program's result.
EXPECTABLE="pass runtime-deadlock-abort sim-deadlock invariant step-limit time-limit limit"

# The exit code -> the verdict name, into VERDICT (a global, not an echo: a
# command substitution forks once per seed).
verdict_name() { case "$1" in 0) VERDICT="pass";; 2) VERDICT="runtime-deadlock-abort";; 10) VERDICT="sim-deadlock";; 11) VERDICT="invariant";; 12) VERDICT="step-limit";; 13) VERDICT="time-limit";; 14) VERDICT="replay-mismatch";; 15) VERDICT="limit";; *) VERDICT="crash-$1";; esac; }

tool_failure() { echo "$PROBE: TOOL FAILURE $*" >&2; exit 2; }

is_count() { case "$1" in ''|*[!0-9]*) return 1;; esac; [ "$1" -gt 0 ]; }

# $1 bin, $2 seed, $3 tracedir. The exit code is the simulator's. The steps
# come from the trace header, which every ending writes.
run_seed() {
    GOO_SIM_SEED="$2" GOO_SIM_TRACE_DIR="$3" \
        GOO_SIM_FAULT_SPURIOUS_WAKE="$FAULT_SPURIOUS" \
        GOO_SIM_FAULT_THREAD_CREATE_EAGAIN="$FAULT_THREAD_CREATE" \
        "$1" >"$3/$2.out" 2>"$3/$2.err"
}

# The loop below forks as little as it can: a fork costs about 1 ms here, and
# a seed of false_deadlock_test costs about 1.4 ms. Measured 2026-11-08 over
# seeds 1..256: the first cut of this loop forked about ten times per seed and
# took 15.4 ms/seed; this one takes 3.7 ms/seed, with the same output.

# $1 trace file. Sets HDR_STEPS (the header's step count, or "?") and
# HDR_MODEL: "preempt ppm=<n> e=<min>..<max> step_cost_ns=<c>", or "preempt
# off" when the header has no preempt_ppm field, or "?" when there is no
# trace. A preempt field this script cannot read is a tool failure: the model
# is in the verdict line, and a guessed one is worse than none.
read_header() {
    local h="" re_steps=' steps=([0-9]+)'
    local re_model=' preempt_ppm=([0-9]+) preempt_e_min=([0-9]+) preempt_e_max=([0-9]+) step_cost_ns=([0-9]+)'
    HDR_STEPS="?"; HDR_MODEL="?"
    [ -f "$1" ] && IFS= read -r h < "$1"
    [ -n "$h" ] || return 0
    [[ $h =~ $re_steps ]] && HDR_STEPS="${BASH_REMATCH[1]}"
    if [[ $h =~ $re_model ]]; then
        HDR_MODEL="preempt ppm=${BASH_REMATCH[1]} e=${BASH_REMATCH[2]}..${BASH_REMATCH[3]} step_cost_ns=${BASH_REMATCH[4]}"
    elif [[ $h == *preempt_* ]]; then
        tool_failure "cannot read the preemption model in the header of $1: $h"
    else
        HDR_MODEL="preempt off"
    fi
}

# The wake hook as this run passes it: unset, or hand-set from the environment.
hook_label() {
    local d
    if d="$(printenv GOO_TEST_WAKE_DELAY_NS)"; then echo "wake delay ${d} ns HAND-SET"; else echo "hook unset"; fi
}

# $1 exit code, $2 err file. Sets VERDICT, with the planned EAGAIN stop named
# only when its fault is on and its assertion is the one that fired.
classify() {
    if [ "$FAULT_THREAD_CREATE" = 1 ] && [ "$1" -eq 134 ] \
        && grep -qxF -- "$EAGAIN_ASSERT_LINE" "$2" && grep -qE -- "$EAGAIN_WHERE_RE" "$2"; then
        VERDICT=expected-eagain-stop
        return
    fi
    verdict_name "$1"
}

# The trace dir's size is tracked, not walked: KEPT_BYTES is what earlier
# seeds left, and each run adds the bytes of its own files. One stat per call.
KEPT_BYTES=0
PEAK_BYTES=0
seed_bytes() {   # $1 tracedir, $2 seed -> SEED_BYTES, the size of every file of that seed
    local b
    SEED_BYTES=0
    while read -r b; do SEED_BYTES=$((SEED_BYTES + b)); done \
        < <(stat -c %s -- "$1/$2.trace" "$1/$2.trace.first" "$1/$2.out" "$1/$2.err" 2>/dev/null)
}
note_peak() {    # $1 tracedir, $2 seed
    seed_bytes "$1" "$2"
    [ $((KEPT_BYTES + SEED_BYTES)) -gt "$PEAK_BYTES" ] && PEAK_BYTES=$((KEPT_BYTES + SEED_BYTES))
    return 0
}

# $1 bin, $2 seed, $3 tracedir, $4 first exit code -> 0 when the second run
# exits the same way and writes the same trace, byte for byte.
replay_matches() {
    mv "$3/$2.trace" "$3/$2.trace.first"
    run_seed "$1" "$2" "$3"
    local rc=$?
    note_peak "$3" "$2"
    [ "$rc" -eq "$4" ] || return 1
    cmp -s "$3/$2.trace.first" "$3/$2.trace"
}

# $1 bin, $2 first seed, $3 count, $4 tracedir. One line per seed. Sets
# COUNTS, FAILS, MISMATCHES, STEPS_TOTAL, MAX_STEPS, FIRST_FAIL (seed verdict
# steps), FIRST_SEED_OF (verdict -> first seed), WALL_NS, and MODEL (from the
# trace headers). A seed whose header names another model is a tool failure.
declare -A COUNTS FIRST_SEED_OF
run_seeds() {
    local bin="$1" first="$2" n="$3" dir="$4" seed rc steps replay t0
    COUNTS=(); FIRST_SEED_OF=(); FAILS=0; MISMATCHES=0; STEPS_TOTAL=0; FIRST_FAIL=""; MAX_STEPS=0; MODEL=""
    t0="$(date +%s%N)"
    for ((seed = first; seed < first + n; seed++)); do
        run_seed "$bin" "$seed" "$dir"; rc=$?
        note_peak "$dir" "$seed"
        classify "$rc" "$dir/$seed.err"
        read_header "$dir/$seed.trace"
        steps="$HDR_STEPS"
        if [ -z "$MODEL" ] || [ "$MODEL" = "?" ]; then MODEL="$HDR_MODEL"
        elif [ "$HDR_MODEL" != "?" ] && [ "$HDR_MODEL" != "$MODEL" ]; then
            tool_failure "seed $seed ran under '$HDR_MODEL', an earlier seed under '$MODEL'"
        fi
        replay=""
        if [ "$rc" -eq 0 ] && [ "$EXPECT" = pass ]; then
            rm -f "$dir/$seed.trace" "$dir/$seed.out" "$dir/$seed.err"
        elif [ "$rc" -eq 0 ]; then
            # A pass where another verdict is expected: a failure, so its
            # trace is kept as the evidence. The replay rule covers the
            # verdicts other than pass, so it does not replay.
            :
        elif [ ! -f "$dir/$seed.trace" ]; then
            # No trace: the simulator stopped before sim_init (a usage error,
            # exit 64) or died where no handler runs. Nothing to replay
            # against, so the seed fails under its own verdict, not as a
            # replay mismatch.
            replay=" replay=no-trace"
        elif replay_matches "$bin" "$seed" "$dir" "$rc"; then
            replay=" replay=identical"
            rm -f "$dir/$seed.trace.first"
        else
            replay=" replay=differs (first $VERDICT)"
            VERDICT="replay-mismatch"
            MISMATCHES=$((MISMATCHES + 1))
        fi
        if [ "$VERDICT" = "$EXPECT" ] && [ "$rc" -ne 0 ]; then
            rm -f "$dir/$seed.trace" "$dir/$seed.out" "$dir/$seed.err"
        fi
        seed_bytes "$dir" "$seed"
        KEPT_BYTES=$((KEPT_BYTES + SEED_BYTES))
        if [ "$steps" != "?" ]; then
            STEPS_TOTAL=$((STEPS_TOTAL + steps))
            [ "$steps" -gt "$MAX_STEPS" ] && MAX_STEPS=$steps
        fi
        COUNTS[$VERDICT]=$(( ${COUNTS[$VERDICT]:-0} + 1 ))
        [ -z "${FIRST_SEED_OF[$VERDICT]:-}" ] && FIRST_SEED_OF[$VERDICT]=$seed
        if [ "$VERDICT" != "$EXPECT" ] && [ "$VERDICT" != expected-eagain-stop ]; then
            FAILS=$((FAILS + 1))
            [ -z "$FIRST_FAIL" ] && FIRST_FAIL="$seed $VERDICT steps=$steps"
        fi
        echo "$seed $VERDICT steps=$steps$replay"
    done
    WALL_NS=$(( $(date +%s%N) - t0 ))
}

print_counts() {
    local v
    echo "--- verdict count ---"
    for v in "${!COUNTS[@]}"; do echo "$v ${COUNTS[$v]}"; done | LC_ALL=C sort
}

# ms per seed, three decimals, from WALL_NS and a seed count.
per_seed_ms() { awk -v ns="$WALL_NS" -v n="$1" 'BEGIN { printf "%.3f", ns / n / 1000000 }'; }

# $1 program name, $2 log file, $3 a scratch dir to remove on failure ("" for none).
build_program() {
    if ! make -s "build/sim/$1" >"$2" 2>&1; then
        sed 's/^/    /' "$2"
        [ -n "${3:-}" ] && rm -rf "$3"
        tool_failure "make build/sim/$1 failed"
    fi
    [ -x "build/sim/$1" ] || tool_failure "build/sim/$1 is not an executable after make"
}

# ---------------------------------------------------------------------------
# The self-test.
# ---------------------------------------------------------------------------

# $1 source, $2 work dir, $3 out binary: compile the source as concurrency.o,
# swap it into a copy of the sim archive, and link false_deadlock_test against
# that copy, the way the Makefile's build/sim/% rule links it.
build_with_sched() {
    local src="$1" w="$2" out="$3" before after
    mkdir -p "$w" || return 1
    # The member name is the basename: concurrency.o replaces concurrency.o.
    # shellcheck disable=SC2086  # RT_CFLAGS and CC_PROBE are word-split on purpose
    $CC_ $RT_CFLAGS -c "$src" -o "$w/concurrency.o" 2>"$w/cc.err" || return 1
    cp "$SIM_LIB" "$w/lib.a" || return 1
    before="$(ar t "$w/lib.a" | wc -l)"
    ar -Dr "$w/lib.a" "$w/concurrency.o" || return 1
    after="$(ar t "$w/lib.a" | wc -l)"
    [ "$before" -eq "$after" ] || { echo "    the swap ADDED a member ($before -> $after)" >"$w/cc.err"; return 1; }
    mkdir -p "$w/x" && (cd "$w/x" && ar x "../lib.a" concurrency.o) || return 1
    cmp -s "$w/x/concurrency.o" "$w/concurrency.o" || { echo "    the archive's concurrency.o is not the one compiled here" >"$w/cc.err"; return 1; }
    # shellcheck disable=SC2086
    # The flags of the Makefile's build/sim/% rule for tests/concurrency.
    # GOO_SIM_BUILD compiles out the hook refusal; without it every seed
    # exits 1 with the hook unset, and control (c) catches that.
    $CC_ -std=c23 -D_GNU_SOURCE -DGOO_SIM_BUILD -Iinclude -I. -Dmain=goo_sim_program_main \
        tests/concurrency/false_deadlock_test.c "$SIM_MAIN_OBJ" "$w/lib.a" -lm -o "$out" 2>>"$w/cc.err"
}

self_test() {
    local SELF="$PROBE --self-test" W rc=0 before_sched k abort_seed v
    # The pass bar's conditions, not the caller's: hook unset, default model,
    # default limits, faults off.
    for v in $(compgen -e); do
        case "$v" in GOO_SIM_*|GOO_TEST_WAKE_DELAY_NS) unset "$v" ;; esac
    done
    FAULT_SPURIOUS=0; FAULT_THREAD_CREATE=0; EXPECT=pass
    W="$(mktemp -d "${TMPDIR:-/tmp}/sim-probe-self.XXXXXX")" || tool_failure "mktemp failed"
    before_sched="$(sha256sum "$SCHED_SRC" | cut -d' ' -f1)"
    build_program false_deadlock_test "$W/make.log"
    build_program sim_invariant_test "$W/make.log"

    sed 's|^void goo_sched_wake_waiters(goo_goroutine_t\*\* waiters) {|void goo_sched_wake_waiters(goo_goroutine_t** waiters) { (void)waiters; return;|' \
        "$SCHED_SRC" > "$W/concurrency.c"
    [ "$(grep -c '(void)waiters; return;' "$W/concurrency.c")" -eq 1 ] || {
        echo "  SELF-TEST BROKEN -- goo_sched_wake_waiters' signature in $SCHED_SRC no longer matches the sed"
        exit 2; }
    [ "$before_sched" = "$(sha256sum "$SCHED_SRC" | cut -d' ' -f1)" ] || {
        echo "  SELF-TEST BROKEN -- tracked $SCHED_SRC was modified"; exit 2; }

    build_with_sched "$SCHED_SRC" "$W/ctl" "$W/ctl_bin" || {
        sed 's/^/    /' "$W/ctl/cc.err"; tool_failure "the control build failed"; }
    build_with_sched "$W/concurrency.c" "$W/mut" "$W/mut_bin" || {
        sed 's/^/    /' "$W/mut/cc.err"; tool_failure "the mutant build failed"; }
    cmp -s "$W/ctl_bin" "$W/mut_bin" && tool_failure "the mutant binary equals the control binary"

    echo "--- (c) control: the tracked runtime through the mutant's pipeline, seeds 1..32 -> MUST pass ---"
    mkdir -p "$W/ctl_trace"
    run_seeds "$W/ctl_bin" 1 32 "$W/ctl_trace" >"$W/ctl.log"
    print_counts
    if [ "$FAILS" -ne 0 ] || [ "$MISMATCHES" -ne 0 ]; then
        echo "  CONTROL FAILED -- first: $FIRST_FAIL. The pipeline is broken, not the tree."
        rc=1
    fi
    echo "--- (c) control: build/sim/false_deadlock_test from make, seeds 1..32 -> MUST pass ---"
    mkdir -p "$W/make_trace"
    run_seeds build/sim/false_deadlock_test 1 32 "$W/make_trace" >"$W/make_ctl.log"
    print_counts
    if [ "$FAILS" -ne 0 ] || [ "$MISMATCHES" -ne 0 ]; then
        echo "  CONTROL FAILED -- first: $FIRST_FAIL"
        rc=1
    fi

    echo "--- (a) mutant: goo_sched_wake_waiters unregisters nobody, seeds 1..256 -> MUST abort ---"
    mkdir -p "$W/mut_trace"
    run_seeds "$W/mut_bin" 1 256 "$W/mut_trace" >"$W/mut.log"
    print_counts
    MUT_MODEL="$MODEL"
    echo "    wall $(per_seed_ms 256) ms/seed over 256 seeds, steps total $STEPS_TOTAL, max $MAX_STEPS, $MODEL, $(hook_label)"
    abort_seed="${FIRST_SEED_OF[runtime-deadlock-abort]:-}"
    if [ -z "$abort_seed" ]; then
        echo "  MUTANT STAYED GREEN -- no runtime-deadlock-abort on seeds 1..256."
        echo "  A sample trace header (seed 1):"
        GOO_SIM_SEED=1 GOO_SIM_TRACE_DIR="$W" "$W/mut_bin" >/dev/null 2>&1
        head -1 "$W/1.trace" | sed 's/^/    /'
        rc=1
    else
        k="$abort_seed"
        echo "    first runtime-deadlock-abort: $(grep -m1 "^$k " "$W/mut.log") (seeds_to_first_failure=$k)"
        if [ "${COUNTS[replay-mismatch]:-0}" -ne 0 ]; then
            echo "  MUTANT REPLAY DIFFERS on ${COUNTS[replay-mismatch]} seed(s), first ${FIRST_SEED_OF[replay-mismatch]}"
            rc=1
        fi
    fi

    echo "--- (b) invariant: a sync.Mutex unlocked by a non-owner, seeds 1..4 -> MUST read invariant ---"
    mkdir -p "$W/inv_trace"
    run_seeds build/sim/sim_invariant_test 1 4 "$W/inv_trace" >"$W/inv.log"
    print_counts
    if [ "${COUNTS[invariant]:-0}" -ne 4 ]; then
        echo "  INVARIANT CASE DID NOT READ invariant on every seed:"
        sed 's/^/    /' "$W/inv.log"
        rc=1
    elif ! head -1 "$W/inv_trace/1.trace" | grep -q ' verdict=invariant detail=unlock-not-owner$'; then
        echo "  INVARIANT CASE fired another check: $(head -1 "$W/inv_trace/1.trace")"
        rc=1
    fi

    # (d) --expect, both ways: a program that aborts by construction meets an
    # expected abort, and a passing program does NOT meet it. Without the
    # second half, an --expect that matched every verdict would read green.
    build_program deadlock_poll_test "$W/make.log"
    echo "--- (d) --expect runtime-deadlock-abort: build/sim/deadlock_poll_test, seeds 1..4 -> MUST meet it ---"
    EXPECT=runtime-deadlock-abort
    mkdir -p "$W/exp_trace"
    run_seeds build/sim/deadlock_poll_test 1 4 "$W/exp_trace" >"$W/exp.log"
    print_counts
    if [ "$FAILS" -ne 0 ] || [ "${COUNTS[runtime-deadlock-abort]:-0}" -ne 4 ]; then
        echo "  EXPECTED ABORT NOT MET on every seed:"
        sed 's/^/    /' "$W/exp.log"
        rc=1
    fi
    echo "--- (d) --expect runtime-deadlock-abort: build/sim/false_deadlock_test, seeds 1..4 -> MUST fail every seed ---"
    mkdir -p "$W/exp_neg_trace"
    run_seeds build/sim/false_deadlock_test 1 4 "$W/exp_neg_trace" >"$W/exp_neg.log"
    print_counts
    if [ "$FAILS" -ne 4 ] || [ "${COUNTS[pass]:-0}" -ne 4 ]; then
        echo "  A PASS MET AN EXPECTED ABORT -- --expect does not discriminate:"
        sed 's/^/    /' "$W/exp_neg.log"
        rc=1
    fi
    EXPECT=pass

    if [ "$rc" -ne 0 ]; then
        echo "$SELF: FAIL (work dir kept: $W)"
        exit 1
    fi
    rm -rf "$W"
    echo "$SELF: PASS (mutant red at seed $k, invariant red, control green, --expect both ways; $MUT_MODEL, $(hook_label))"
    exit 0
}

# ---------------------------------------------------------------------------
# The run.
# ---------------------------------------------------------------------------
[ "${1:-}" = "" ] && tool_failure "usage: $0 <program> --seeds N [--first-seed K] [--expect VERDICT] [--fault-spurious-wake] [--fault-thread-create] | --self-test"
case "$1" in
    --self-test) self_test ;;
esac

PROGRAM="$1"; shift
SEEDS=""; FIRST=1
while [ $# -gt 0 ]; do
    case "$1" in
        --seeds) SEEDS="${2:-}"; shift 2 || tool_failure "--seeds needs a count" ;;
        --first-seed) FIRST="${2:-}"; shift 2 || tool_failure "--first-seed needs a seed" ;;
        --expect) EXPECT="${2:-}"; shift 2 || tool_failure "--expect needs a verdict" ;;
        --fault-spurious-wake) FAULT_SPURIOUS=1; shift ;;
        --fault-thread-create) FAULT_THREAD_CREATE=1; shift ;;
        *) tool_failure "unknown argument '$1'" ;;
    esac
done
is_count "$SEEDS" || tool_failure "--seeds must be a count greater than 0, not '$SEEDS'"
is_count "$FIRST" || tool_failure "--first-seed must be a seed greater than 0, not '$FIRST'"
case "$PROGRAM" in */*|.*) tool_failure "<program> is a name, not a path: '$PROGRAM'" ;; esac
case " $EXPECTABLE " in *" $EXPECT "*) ;; *) tool_failure "--expect must be one of: $EXPECTABLE; not '$EXPECT'" ;; esac

BIN="build/sim/$PROGRAM"
TRACE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/sim-probe.XXXXXX")" || tool_failure "mktemp failed"
build_program "$PROGRAM" "$TRACE_DIR/make.log" "$TRACE_DIR"
rm -f "$TRACE_DIR/make.log"

LAST=$((FIRST + SEEDS - 1))
echo "=== $PROBE: $BIN, seeds $FIRST..$LAST, expect $EXPECT, faults spurious-wake=$FAULT_SPURIOUS thread-create=$FAULT_THREAD_CREATE, $(hook_label) ==="
run_seeds "$BIN" "$FIRST" "$SEEDS" "$TRACE_DIR"
print_counts
echo "wall $(per_seed_ms "$SEEDS") ms/seed, steps max $MAX_STEPS, peak trace dir $PEAK_BYTES bytes"

case "$MODEL" in ''|'?') MODEL="preempt model unknown (no trace)" ;; esac
ARTEFACT="$BIN, seeds $FIRST..$LAST, expect $EXPECT, steps total $STEPS_TOTAL, $MODEL, $(hook_label)"
if [ "$MISMATCHES" -ne 0 ]; then
    echo "$PROBE: FAIL replay-mismatch on $MISMATCHES seed(s), first ${FIRST_SEED_OF[replay-mismatch]} ($ARTEFACT, trace dir $TRACE_DIR)"
    exit 14
fi
if [ "$FAILS" -ne 0 ]; then
    echo "$PROBE: FAIL $FAILS seed(s), first: $FIRST_FAIL ($ARTEFACT, trace dir $TRACE_DIR)"
    exit 1
fi
if [ -n "$(ls -A "$TRACE_DIR")" ]; then
    echo "$PROBE: PASS ($ARTEFACT, trace dir $TRACE_DIR)"
else
    rmdir "$TRACE_DIR"
    echo "$PROBE: PASS ($ARTEFACT, trace dir $TRACE_DIR, removed: every seed reached $EXPECT)"
fi
exit 0
