#!/bin/bash
# Arc 1 — measure Goo against Rust and Go.
#
# The project goal names Rust. Until this script existed, nothing in the
# repository had ever measured Rust, so every priority rested on a guess.
#
# REPORTS, NEVER GATES. Cross-language wall-clock is noisy and depends on the
# machine, the core count and what else is running. This script prints a table
# and exits 0 unless something is genuinely broken (a program failed to build,
# or two languages disagreed on the answer). It is deliberately NOT in
# verify-core: a timing threshold there would fail on a smaller machine and
# teach people to ignore a red gate.
#
# THE ONE HARD ASSERTION: for each benchmark, every language must print the
# SAME output. A row where the outputs differ is not a measurement — it means
# the programs are not the same computation, and the timings are meaningless.
# That check is why this script can fail at all.
#
# Method: peak resident set size comes from `/usr/bin/time -v`, the same way
# scripts/arena_rss_probe.sh does it (the helper below is lifted from there).
# Wall clock is the median of N runs, not the mean — one scheduler hiccup
# should not move the number.
#
# TWO METHODOLOGY RULES, both learned the hard way on 2026-07-28. The first
# version of this script had neither, and it reported a 1.5x Goo-against-Rust
# deficit on the scalar benchmark that DID NOT EXIST.
#
#   1. PIN THE CORES. On a 32-core box an unpinned process migrates between
#      cores and turbo states, which made the same binary alternate between
#      0.08 s and 0.12 s run to run. Serial benchmarks get one core; parallel
#      benchmarks get cores 0-7, which is the 8 the algorithm asks for. Every
#      language in a row gets the same set.
#   2. MAKE THE WORKLOAD BIG ENOUGH. `/usr/bin/time -f %e` resolves to 10 ms.
#      A benchmark that finishes in 0.08 s is eight ticks, so a one-tick wobble
#      is a 12% swing and a four-tick wobble reads as 1.5x. Every benchmark
#      here is now sized to run for at least half a second.
#
# When the scalar benchmark was re-measured under both rules it came out at
# 0.86 s for Goo and 0.86 s for Rust — exact parity. Do not remove either rule.
#
# Usage:
#   bash scripts/rust_compare_bench.sh              # all benchmarks
#   bash scripts/rust_compare_bench.sh stencil      # one benchmark
#   RUNS=9 bash scripts/rust_compare_bench.sh       # more samples

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMPILER="${COMPILER:-$ROOT/bin/goo}"
GO="${GO:-/usr/local/go/bin/go}"
RUNS="${RUNS:-5}"
BENCH="$ROOT/bench"

fail() { echo "FAIL: $1"; exit 1; }
skip() { echo "rust-compare-bench: SKIPPED ($1)"; exit 0; }

[ -x "$COMPILER" ] || fail "compiler not found at $COMPILER (run 'make')"
command -v "$GO" >/dev/null 2>&1 || skip "go not found at $GO"
command -v rustc >/dev/null 2>&1 || skip "rustc not found"

TIME_BIN="/usr/bin/time"
[ -x "$TIME_BIN" ] || skip "/usr/bin/time not found"
"$TIME_BIN" -v true >/dev/null 2>&1 || skip "/usr/bin/time -v unsupported"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

# Core pinning — methodology rule 1. PIN is set per benchmark: a single core
# for the serial ones, cores 0-7 for the 8-way parallel one. If taskset is
# missing (macOS has no equivalent), say so ONCE and continue unpinned rather
# than silently reporting noisier numbers as if they were clean.
PIN=""
PIN_WARNED=0
if command -v taskset >/dev/null 2>&1; then
    HAVE_TASKSET=1
else
    HAVE_TASKSET=0
fi
pinned() {
    if [ "$HAVE_TASKSET" = "1" ] && [ -n "$PIN" ]; then
        taskset -c "$PIN" "$@"
    else
        if [ "$HAVE_TASKSET" = "0" ] && [ "$PIN_WARNED" = "0" ]; then
            echo "  NOTE: taskset unavailable — numbers are UNPINNED and noisier (see the header)" >&2
            PIN_WARNED=1
        fi
        "$@"
    fi
}

# Peak RSS in KB. Same parse as scripts/arena_rss_probe.sh's peak_rss_kb.
peak_rss_kb() {
    "$TIME_BIN" -v $( [ "$HAVE_TASKSET" = "1" ] && [ -n "$PIN" ] && echo "taskset -c $PIN" ) "$@" \
        >/dev/null 2>"$WORKDIR/t.txt" || return 1
    grep -F "Maximum resident set size" "$WORKDIR/t.txt" | grep -oE '[0-9]+' | tail -1
}

# Median wall-clock seconds over $RUNS runs, on the pinned core set.
median_wall() {
    local i
    : > "$WORKDIR/w.txt"
    for i in $(seq 1 "$RUNS"); do
        "$TIME_BIN" -f "%e" $( [ "$HAVE_TASKSET" = "1" ] && [ -n "$PIN" ] && echo "taskset -c $PIN" ) "$@" \
            2>>"$WORKDIR/w.txt" >/dev/null || return 1
    done
    sort -g "$WORKDIR/w.txt" | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'
}

# Wall-clock seconds for one compile, median of 3. Cold-ish: each run removes
# the previous artifact so nothing is served from a build cache we did not ask
# for. Go and cargo keep their own caches, which is noted in the findings.
compile_secs() {
    local out="$1"; shift
    local i
    : > "$WORKDIR/c.txt"
    for i in 1 2 3; do
        rm -f "$out"
        "$TIME_BIN" -f "%e" "$@" 2>>"$WORKDIR/c.txt" >/dev/null || return 1
    done
    sort -g "$WORKDIR/c.txt" | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'
}

size_kb() { echo $(( ( $(stat -c%s "$1") + 1023 ) / 1024 )); }

# ---------------------------------------------------------------------------
# One row: language, binary, run-arguments. Records output for the agreement
# check, then wall clock, peak RSS and binary size.
# ---------------------------------------------------------------------------
declare -A OUT
ROWS=""

measure() {
    local bench="$1" lang="$2" bin="$3"; shift 3
    local out wall rss sz
    out="$("$bin" "$@" 2>/dev/null)" || { echo "  $lang: RUN FAILED"; return 1; }
    OUT["$bench/$lang"]="$out"
    wall="$(median_wall "$bin" "$@")"
    rss="$(peak_rss_kb "$bin" "$@")"
    sz="$(size_kb "$bin")"
    printf "  %-12s wall=%-8s rssKB=%-9s binKB=%-7s out=%s\n" "$lang" "$wall" "$rss" "$sz" "$out"
}

# Assert every language in a benchmark printed the same thing.
check_agreement() {
    local bench="$1"; shift
    local first="" k lang
    for lang in "$@"; do
        k="$bench/$lang"
        [ -n "${OUT[$k]+x}" ] || continue
        if [ -z "$first" ]; then
            first="${OUT[$k]}"
        elif [ "${OUT[$k]}" != "$first" ]; then
            echo "  DISAGREEMENT: $lang printed '${OUT[$k]}', expected '$first'"
            return 1
        fi
    done
    echo "  agreement: OK (all languages printed '$first')"
    return 0
}

FAILED=0
WANT="${1:-all}"
want() { [ "$WANT" = "all" ] || [ "$WANT" = "$1" ]; }

echo "=== rust-compare-bench: Goo against Rust and Go ==="
echo "runs=$RUNS  cores=$(nproc)  goo=$($COMPILER --version 2>&1 | head -1)"
echo "go=$($GO version)  rustc=$(rustc --version)"
echo ""

# --- 1. scalar loop, no allocation ----------------------------------------
if want scalar; then
    PIN=2
    echo "[scalar] dependent 64-bit LCG chain, 1000M iterations. No allocation. Pinned to core $PIN."
    $COMPILER -O2 -o "$WORKDIR/scalar_goo" "$BENCH/scalar/scalar.goo" >/dev/null 2>&1 || fail "scalar: goo build"
    $GO build -o "$WORKDIR/scalar_go" "$BENCH/scalar/scalar.go" || fail "scalar: go build"
    rustc -O -o "$WORKDIR/scalar_rs" "$BENCH/scalar/scalar.rs" 2>/dev/null || fail "scalar: rustc build"
    measure scalar goo  "$WORKDIR/scalar_goo"
    measure scalar go   "$WORKDIR/scalar_go"
    measure scalar rust "$WORKDIR/scalar_rs"
    check_agreement scalar goo go rust || FAILED=1
    echo ""
fi

# --- 2. the daemon shape ---------------------------------------------------
if want daemon; then
    PIN=2
    N="${DAEMON_N:-1000000}"
    echo "[daemon] allocate per request, hold nothing. $N requests."
    echo "         This is the retention axis: peak RSS is the number that matters."
    $COMPILER -O2 -o "$WORKDIR/daemon_goo" "$BENCH/daemon/daemon.goo" >/dev/null 2>&1 || fail "daemon: goo build"
    $GO build -o "$WORKDIR/daemon_go" "$BENCH/daemon/daemon.go" || fail "daemon: go build"
    rustc -O -o "$WORKDIR/daemon_rs" "$BENCH/daemon/daemon.rs" 2>/dev/null || fail "daemon: rustc build"
    measure daemon goo  "$WORKDIR/daemon_goo" "$N"
    measure daemon go   "$WORKDIR/daemon_go" "$N"
    measure daemon rust "$WORKDIR/daemon_rs" "$N"
    check_agreement daemon goo go rust || FAILED=1
    echo ""
fi

# --- 3. 1D stencil, 8-way parallel ----------------------------------------
if want stencil; then
    PIN=0-7
    echo "[stencil] 1<<20 cells, 1000 rounds, 8 lanes. Parallel throughput. Pinned to cores $PIN."
    echo "          rust-simd needs nightly. It is reported separately BECAUSE"
    echo "          std::simd is not what Rust ships to users on stable."
    $COMPILER -O2 -o "$WORKDIR/stencil_goo" "$BENCH/stencil/stencil.goo" >/dev/null 2>&1 || fail "stencil: goo build"
    $GO build -o "$WORKDIR/stencil_go" "$BENCH/stencil/stencil.go" || fail "stencil: go build"
    ( cd "$BENCH/stencil/rust" && cargo build --release >/dev/null 2>&1 ) || fail "stencil: cargo build"
    measure stencil goo  "$WORKDIR/stencil_goo"
    measure stencil go   "$WORKDIR/stencil_go"
    measure stencil rust "$BENCH/stencil/rust/target/release/stencil-bench"
    LANGS="goo go rust"
    if cargo +nightly --version >/dev/null 2>&1 &&
       ( cd "$BENCH/stencil/rust-simd" && cargo +nightly build --release >/dev/null 2>&1 ); then
        measure stencil rust-simd "$BENCH/stencil/rust-simd/target/release/stencil-simd-bench"
        LANGS="$LANGS rust-simd"
    else
        echo "  rust-simd:   SKIPPED (no nightly toolchain)"
    fi
    check_agreement stencil $LANGS || FAILED=1
    echo ""
fi

# --- 4. string and slice heavy text processing -----------------------------
if want text; then
    PIN=2
    N="${TEXT_N:-1000000}"
    echo "[text] $N lines through Fields/Index/ToUpper/append/Join."
    echo "       Exercises the runtime helper allocation path ADR 0002 phase 2 owns."
    $COMPILER -O2 -o "$WORKDIR/text_goo" "$BENCH/text/text.goo" >/dev/null 2>&1 || fail "text: goo build"
    $GO build -o "$WORKDIR/text_go" "$BENCH/text/text.go" || fail "text: go build"
    rustc -O -o "$WORKDIR/text_rs" "$BENCH/text/text.rs" 2>/dev/null || fail "text: rustc build"
    measure text goo  "$WORKDIR/text_goo" "$N"
    measure text go   "$WORKDIR/text_go" "$N"
    measure text rust "$WORKDIR/text_rs" "$N"
    check_agreement text goo go rust || FAILED=1
    echo ""
fi

# --- 5. hello world: compile time and binary size --------------------------
if want hello; then
    PIN=""
    echo "[hello] one library call. COMPILE time and BINARY size, not run time."
    gc=$(compile_secs "$WORKDIR/h_goo" $COMPILER -O2 -o "$WORKDIR/h_goo" "$BENCH/hello/hello.goo")
    oc=$(compile_secs "$WORKDIR/h_go"  $GO build -o "$WORKDIR/h_go"  "$BENCH/hello/hello.go")
    rc=$(compile_secs "$WORKDIR/h_rs"  rustc -O -o "$WORKDIR/h_rs"   "$BENCH/hello/hello.rs")
    printf "  %-12s compile=%-8s binKB=%s\n" goo  "$gc" "$(size_kb "$WORKDIR/h_goo")"
    printf "  %-12s compile=%-8s binKB=%s\n" go   "$oc" "$(size_kb "$WORKDIR/h_go")"
    printf "  %-12s compile=%-8s binKB=%s\n" rust "$rc" "$(size_kb "$WORKDIR/h_rs")"
    OUT["hello/goo"]="$("$WORKDIR/h_goo")"
    OUT["hello/go"]="$("$WORKDIR/h_go")"
    OUT["hello/rust"]="$("$WORKDIR/h_rs")"
    check_agreement hello goo go rust || FAILED=1
    echo ""
fi

if [ "$FAILED" -ne 0 ]; then
    echo "rust-compare-bench: FAIL (a benchmark's languages disagreed — the timings above are void)"
    exit 1
fi
echo "rust-compare-bench: done (report only, never a gate)"
