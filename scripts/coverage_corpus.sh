#!/bin/bash
# coverage_corpus.sh — branch coverage of the SHIPPED compiler.
#
# WHY THIS EXISTS. `make verify-core` runs 197 gates over 493 golden pairs and
# 156 reject fixtures, and nothing measured which branches of bin/goo any of
# that reaches. A gate count measures effort; a coverage number measures reach.
# The two are not the same, and only the second tells you where the next probe
# is worth writing.
#
# WHAT IT IS NOT. Not a gate. Deliberately absent from verify-core. A coverage
# target invites tests that raise the number instead of tests that find bugs.
# The output is an input to a decision, not a pass/fail verdict — this script
# exits 0 on any coverage percentage, and nonzero ONLY when it failed to
# measure at all.
#
# SCOPE. bin/goo-cov is built from GOO_SRCS, the P5.6 reachable set (see the
# comment above GOO_TYPES_SRCS in the Makefile). The constraint-inference, HKT,
# and concept-generics frameworks are NOT linked into the shipped compiler, so
# counting them would report a false low number and hide the reachable gaps.
#
# SERIAL ON PURPOSE. Concurrent processes merge .gcda counters without a
# reliable lock, so a parallel run silently corrupts them. GOLDEN_JOBS does not
# apply here. ~800 fixtures at ~0.3 s each is a few minutes.
#
# INSTRUMENT CHECKS. A coverage script that measures nothing reports 0% or
# 100%, and both look like an answer. Four assertions below refuse to print a
# number unless the measurement really happened: the binary must be
# instrumented, the corpus must be non-empty, the compiler must have run, and
# the run must have written counters. Prove it can fall by deleting fixtures:
#   scripts/coverage_corpus.sh --self-test

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

COMPILER="${COMPILER:-bin/goo-cov}"
COV_OBJDIR="${COV_OBJDIR:-build/cov}"
COVERAGE_DIR="${COVERAGE_DIR:-coverage}"
# Cap for the "worst files" table at the end. The full data is in the HTML.
TOP_N="${TOP_N:-15}"
# Per-fixture wall-clock limit. An instrumented build is ~4x slower than the
# shipped one, and a handful of fixtures exercise deliberately large inputs.
FIXTURE_TIMEOUT="${FIXTURE_TIMEOUT:-30}"

SELF_TEST=0
[ "${1:-}" = "--self-test" ] && SELF_TEST=1

fail() { echo "coverage-corpus: BROKEN — $1" >&2; exit 1; }

# --------------------------------------------------------------------------
# Instrument check 1: the binary exists and carries gcov counters.
# --------------------------------------------------------------------------
[ -x "$COMPILER" ] || fail "no executable at $COMPILER (run: make bin/goo-cov)"

gcno_count="$(find "$COV_OBJDIR" -name '*.gcno' 2>/dev/null | wc -l | tr -d ' ')"
[ "$gcno_count" -gt 0 ] || fail "no .gcno files in $COV_OBJDIR — $COMPILER was built WITHOUT -fprofile-arcs -ftest-coverage, so every counter would read zero"

echo "=== coverage-corpus: branch coverage of $COMPILER ==="
echo "  instrumented translation units: $gcno_count"

# --------------------------------------------------------------------------
# Zero the counters. .gcda files ACCUMULATE across runs, so a stale set from a
# previous corpus would inflate this one.
# --------------------------------------------------------------------------
find "$COV_OBJDIR" -name '*.gcda' -delete 2>/dev/null

# --------------------------------------------------------------------------
# Collect the corpus.
#   examples/         — 595 fixtures, the golden pass suite plus probe inputs
#   tests/golden/reject/ — 156 fixtures the compiler must REFUSE; these are the
#                       only systematic reach into the diagnostic paths
#   tests/spec/       — 59 Go-conformance fixtures
# --------------------------------------------------------------------------
mapfile -t FIXTURES < <(
    ls examples/*.goo 2>/dev/null
    ls tests/golden/reject/*.goo 2>/dev/null
    find tests/spec -name '*.goo' 2>/dev/null | sort
)

# --self-test halves the corpus. The number MUST fall; see the footer.
if [ "$SELF_TEST" -eq 1 ]; then
    half=$(( ${#FIXTURES[@]} / 2 ))
    FIXTURES=("${FIXTURES[@]:0:$half}")
    echo "  SELF-TEST: corpus cut to $half fixtures — the number below must be LOWER"
fi

# Instrument check 2: a corpus of zero files would report 0% and look like a
# result rather than a mistake.
[ "${#FIXTURES[@]}" -gt 0 ] || fail "corpus is empty — nothing to measure"
echo "  fixtures: ${#FIXTURES[@]}"

# --------------------------------------------------------------------------
# Drive the compiler. Only the COMPILE step counts: the compiler is the program
# under measurement, so running the binary it produces adds nothing. Exit
# status is ignored on purpose — a reject fixture MUST fail to compile, and its
# diagnostic path is exactly what we came to measure.
# --------------------------------------------------------------------------
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

compiled=0
crashed=0
timedout=0
i=0
for f in "${FIXTURES[@]}"; do
    i=$((i + 1))
    timeout "$FIXTURE_TIMEOUT" "$COMPILER" -o "$WORK/out" "$f" >/dev/null 2>&1
    rc=$?
    case "$rc" in
        124) timedout=$((timedout + 1)) ;;
        # 128+N means a signal. A SIGSEGV in the compiler is a real defect and
        # is worth surfacing here even though this script is not a gate.
        13[4-9]|1[4-9][0-9]) crashed=$((crashed + 1)); echo "  CRASH (rc=$rc): $f" ;;
    esac
    compiled=$((compiled + 1))
    if [ $((i % 100)) -eq 0 ]; then
        printf '  ... %d/%d\n' "$i" "${#FIXTURES[@]}"
    fi
done

echo "  compiled: $compiled   timed out: $timedout   crashed: $crashed"

# Instrument check 3: the loop ran the compiler at least once.
[ "$compiled" -gt 0 ] || fail "no fixture reached the compiler"

# Instrument check 4: counters were actually written. gcov flushes .gcda at
# exit(); a binary that died on a signal every time would leave none, and lcov
# would then report 0% as though it were a measurement.
gcda_count="$(find "$COV_OBJDIR" -name '*.gcda' 2>/dev/null | wc -l | tr -d ' ')"
[ "$gcda_count" -gt 0 ] || fail "no .gcda counters written after $compiled compiles — the binary never reached exit()"
echo "  counter files written: $gcda_count / $gcno_count"

# --------------------------------------------------------------------------
# Report, straight from gcov's JSON. lcov is deliberately NOT a dependency:
# gcov ships with gcc, so this runs on any machine that can build the compiler.
#
#   -b            emit branch data. WITHOUT IT the branches array is EMPTY and
#                 every file reads as 0 branches, which looks like a clean
#                 result rather than a missing flag. Measured, not assumed.
#   --conditions  emit MC/DC data, when the build carried -fcondition-coverage.
# --------------------------------------------------------------------------
command -v gcov >/dev/null || fail "gcov is not installed (it ships with gcc)"
command -v python3 >/dev/null || fail "python3 is not installed"

mkdir -p "$COVERAGE_DIR"
JSON_DIR="$WORK/json"
mkdir -p "$JSON_DIR"

emitted=0
for gcno in "$COV_OBJDIR"/*.gcno; do
    [ -e "$gcno" ] || continue
    base="$(basename "$gcno" .gcno)"
    if gcov --json-format --stdout -b --conditions -o "$COV_OBJDIR" "$gcno" \
            > "$JSON_DIR/$base.json" 2>/dev/null && [ -s "$JSON_DIR/$base.json" ]; then
        emitted=$((emitted + 1))
    else
        # --conditions is rejected by gcc 13 and older. Retry without it, so an
        # older toolchain still reports branch coverage.
        gcov --json-format --stdout -b -o "$COV_OBJDIR" "$gcno" \
            > "$JSON_DIR/$base.json" 2>/dev/null && [ -s "$JSON_DIR/$base.json" ] \
            && emitted=$((emitted + 1))
    fi
done

# Instrument check 5: gcov produced parseable output for at least one object.
[ "$emitted" -gt 0 ] || fail "gcov emitted no JSON for any of the $gcno_count objects"

TOP_N="$TOP_N" python3 - "$JSON_DIR" <<'PY'
import json, os, sys

json_dir = sys.argv[1]
top_n = int(os.environ.get("TOP_N", "15"))

per_file = {}          # src path -> [branches_total, branches_taken, cond_total, cond_covered]
for name in sorted(os.listdir(json_dir)):
    with open(os.path.join(json_dir, name)) as fh:
        try:
            data = json.load(fh)
        except json.JSONDecodeError:
            continue
    for f in data.get("files", []):
        src = f["file"]
        # Only the compiler's own sources. A system header pulled in by an
        # #include would otherwise pad the denominator with code we do not own.
        if not src.startswith("src/"):
            continue
        row = per_file.setdefault(src, [0, 0, 0, 0])
        for line in f.get("lines", []):
            for br in line.get("branches", []):
                row[0] += 1
                if br.get("count", 0) > 0:
                    row[1] += 1
            for cond in line.get("conditions", []):
                row[2] += cond.get("count", 0)
                row[3] += cond.get("covered", 0)

if not per_file:
    print("coverage-corpus: BROKEN — gcov reported no file under src/", file=sys.stderr)
    sys.exit(1)

# src/runtime/ is LINKED into bin/goo but never EXECUTED by it: those functions
# are emitted for the programs the compiler produces, and this corpus compiles
# without running. Counting them in the headline denominator understates the
# compiler by ~1,500 permanently-unreachable branches, and it silently merges
# two measurements that need two different corpora. Reported apart, not hidden.
def totals(items):
    return (sum(r[0] for _, r in items), sum(r[1] for _, r in items),
            sum(r[2] for _, r in items), sum(r[3] for _, r in items))

compiler_files = [kv for kv in per_file.items() if not kv[0].startswith("src/runtime/")]
runtime_files  = [kv for kv in per_file.items() if kv[0].startswith("src/runtime/")]

bt, bh, ct, ch = totals(compiler_files)

# Instrument check 6: a branch total of zero means gcov ran without -b, or the
# objects carry no counters. Either way the percentages below would be fiction.
if bt == 0:
    print("coverage-corpus: BROKEN — 0 branches found across "
          f"{len(per_file)} files; gcov ran without -b", file=sys.stderr)
    sys.exit(1)

print()
print("=== coverage of the COMPILER (GOO_OBJS, less src/runtime/) ===")
print(f"  files measured      {len(compiler_files)}")
print(f"  branch coverage     {100.0*bh/bt:5.1f}%   ({bh:,} of {bt:,} branch directions taken)")
if ct:
    print(f"  MC/DC coverage      {100.0*ch/ct:5.1f}%   ({ch:,} of {ct:,} condition outcomes)")
    print("                              (Hipp's SQLite bar is 100%)")
else:
    print("  MC/DC coverage      not measured (build lacked -fcondition-coverage)")

if runtime_files:
    rt, rh, _, _ = totals(runtime_files)
    pct = f"{100.0*rh/rt:5.1f}%" if rt else "    -"
    print()
    print(f"  src/runtime/ ({len(runtime_files)} files, {rt:,} branches) is EXCLUDED above: "
          f"{pct} here")
    print("  It is linked into bin/goo but runs only inside compiled programs,")
    print("  which this corpus does not execute. Measuring it needs its own corpus.")

# The ranked table below stays scoped to the compiler, for the same reason.
per_file = dict(compiler_files)

# Ranked worst-first by the COUNT of unreached branches, not by percentage: a
# file with 400 of 800 uncovered is worth more attention than a 3-branch file
# sitting at 0%.
print()
print(f"=== files with the most unreached branches (top {top_n}) ===")
print(f"  {'unreached':>9}  {'branch%':>7}  {'mcdc%':>6}  file")
ranked = sorted(per_file.items(), key=lambda kv: kv[1][0] - kv[1][1], reverse=True)
for src, (t, h, cT, cH) in ranked[:top_n]:
    mcdc = f"{100.0*cH/cT:5.1f}" if cT else "    -"
    print(f"  {t-h:9,}  {100.0*h/t:6.1f}%  {mcdc}%  {src}")

# Fully unreached files are a different signal from partly covered ones: they
# suggest either dead code or a whole feature with no fixture.
dead = [s for s, r in per_file.items() if r[0] > 0 and r[1] == 0]
if dead:
    print()
    print(f"=== {len(dead)} file(s) with ZERO branches taken ===")
    for s in sorted(dead):
        print(f"  {s}")
PY
rc=$?
[ "$rc" -eq 0 ] || exit "$rc"

echo
if [ "$SELF_TEST" -eq 1 ]; then
    echo "coverage-corpus: SELF-TEST done. Compare the percentage above against a"
    echo "full run. If it did not FALL, this script is not measuring the corpus."
else
    echo "coverage-corpus: done (not a gate — no pass/fail verdict)"
fi
