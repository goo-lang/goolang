#!/usr/bin/env bash
# check_stdlib_coverage.sh — P4.11 shim-table drift catch.
#
# Extracts the ENTIRE stdlib symbol surface mechanically from source (not a
# hand-maintained list) and requires each symbol to appear in at least one
# golden-wired examples/*.goo fixture (a .goo with a sibling .expected.txt —
# the exact fixture set scripts/run_golden.sh compiles, runs, and asserts
# stdout/exit code for). A shim row or goostd export added without smoke
# coverage FAILS this script — the drift catch
# docs/2026-07-08-v1-roadmap.md:159 asks for. Wired into `verify-core` via
# the `stdlib-smoke-coverage` Makefile target.
#
# Surface extracted:
#   1. SHIM_TABLE rows                  (src/types/shim_signatures.c)
#   2. Package VALUE members            (os.Args, math.Pi — expression_checker.c)
#   3. time.Duration constants          (type_checker.c: time_export_value calls)
#   4. Seeded package funcs             (type_checker.c: time_export_func calls — Sleep/Now)
#   5. Seeded methods                   (type_checker.c: sync_export_method + time_export_method)
#   6. goostd exported funcs            (^func [A-Z] in strings/strconv/utf8/bits/
#      lanes — the REAL stdlib source dirs; test-only goostd packages like
#      kinds/shapes/mypkg/pkgcheck/fwdref/cpkg are compiler-test fixtures, not
#      stdlib, and are intentionally out of scope here. `^func [A-Z]` only
#      matches bare package-level funcs, not methods — goostd/lanes' methods
#      (Own/ID/Publish/HaloLeft/HaloRight/Step, all `func (l *Lane) ...`) are
#      invisible to this regex and NOT extracted; they are covered by
#      examples/lanes_stencil_probe.goo's golden, just not mechanically
#      verified by this script. Extending the extractor to receivers is
#      YAGNI for this milestone — not worth it for one package.)
#
# Robustness: every extraction step asserts a sane MINIMUM count and dies
# loudly if it comes up short — a shim_signatures.c/type_checker.c rename or
# move that broke the awk/sed/grep patterns below must be a hard failure,
# never a silent "extracted zero symbols, vacuously PASS".
#
# Matching is intentionally loose (substring/word-bounded, not full parse):
# a callable is "covered" if `<pkg>.<Name>(` appears anywhere in a golden
# fixture's source; a seeded METHOD (sync/time) is covered if `.<Method>(`
# appears anywhere (methods are called on arbitrary receiver names, e.g.
# `mu.Lock()`, so there is no fixed package-qualified spelling to anchor on).
#
# Exit 0 = every extracted symbol covered. Exit 1 = uncovered symbol(s)
# (all listed, never truncated) or an extraction sanity check failed.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

SHIM_SRC="src/types/shim_signatures.c"
# P6 M2-B1 Task 10: seed_sync_package_exports/seed_time_package_exports (and
# the time_export_*/sync_export_method calls inside them) moved from goo.c
# to type_checker.c so a vendored/local package's own `import "sync"`/`"time"`
# can seed them too (not just main's). Sections 3/4/5 below grep this file.
SEED_SRC="src/types/type_checker.c"
EXPR_CHECKER="src/types/expression_checker.c"
EXAMPLES_DIR="examples"

for f in "$SHIM_SRC" "$SEED_SRC" "$EXPR_CHECKER"; do
    if [ ! -f "$f" ]; then
        echo "check-stdlib-coverage: FAIL (source file moved: $f not found)"
        exit 1
    fi
done

# --- Golden-wired fixture list (name.goo + sibling name.expected.txt) ----
GOLDEN_LIST="$(mktemp)"
trap 'rm -f "$GOLDEN_LIST"' EXIT
for f in "$EXAMPLES_DIR"/*.goo; do
    exp="${f%.goo}.expected.txt"
    [ -f "$exp" ] && printf '%s\n' "$f" >> "$GOLDEN_LIST"
done

GOLDEN_MIN=100
golden_count="$(wc -l < "$GOLDEN_LIST" | tr -d '[:space:]')"
if [ "${golden_count:-0}" -lt "$GOLDEN_MIN" ]; then
    echo "check-stdlib-coverage: FAIL (found only $golden_count golden-wired $EXAMPLES_DIR/*.goo fixtures, expected >= $GOLDEN_MIN — run_golden.sh wiring or examples/ layout may have moved)"
    exit 1
fi

# KNOWN, DOCUMENTED carve-outs: one "symbol:probe-name" pair per line.
# Each of these symbols is exercised by a REAL functional probe (its own
# Makefile target, in VERIFY_ALL_DEPS already) that a golden fixture
# structurally cannot replace — see each entry's own comment for why.
#
#   os.ReadLine reads real stdin, and run_golden.sh has no mechanism to pipe
#   stdin into a fixture (see its own header comment and
#   examples/os_readline_probe.goo's doc comment) — a golden fixture calling
#   it would either hang waiting on a real terminal's stdin or behave
#   nondeterministically depending on the invoker's stdin. Covered by
#   `readline-probe` (stdin fed by file redirection).
#
#   lanes.RunFar (M2-B1 T4) spawns cooperating OS PROCESSES over a real NNG
#   transport (scripts/far-probe.sh) — run_golden.sh compiles and runs ONE
#   binary with no argv, so it cannot multi-process a fixture. RunFar's own
#   fixture (examples/far_halo_probe.goo) deliberately has NO sibling
#   .expected.txt for exactly this reason (see its doc comment) and so is
#   invisible to $GOLDEN_LIST by construction, not oversight. Covered by
#   `far-halo-probe` (near-mode single-process reference check plus the
#   real 2-rank far-mode run via scripts/far-probe.sh).
#
# Listed explicitly (not silently dropped) so each exception is visible to
# any reader/reviewer of this script.
NON_GOLDEN_COVERABLE="os.ReadLine:readline-probe
lanes.RunFar:far-halo-probe"

covered() {
    # $1 = extended-regex pattern. Go/Goo identifiers are [A-Za-z0-9_] only,
    # so pkg/name tokens need no metachar escaping.
    xargs grep -lE -- "$1" < "$GOLDEN_LIST" 2>/dev/null | grep -q .
}

missing=()
skipped=()
total=0

record() {
    # $1 = human label, $2 = regex pattern, $3 = optional "symbol key" to
    # check against NON_GOLDEN_COVERABLE (defaults to $1).
    local label="$1" pattern="$2" key="${3:-$1}"
    total=$((total + 1))
    local probe
    probe="$(printf '%s\n' "$NON_GOLDEN_COVERABLE" | awk -F: -v k="$key" '$1==k{print $2; exit}')"
    if [ -n "$probe" ]; then
        skipped+=("$label (documented non-golden carve-out; see ${probe})")
        return
    fi
    if ! covered "$pattern"; then
        missing+=("$label")
    fi
}

# --- 1. SHIM_TABLE rows (shim_signatures.c) ------------------------------
shim_pairs="$(awk '/static const ShimSignature SHIM_TABLE\[\] = \{/{flag=1; next} /^\};/{flag=0} flag' "$SHIM_SRC" \
    | grep -E '^[[:space:]]*\{[[:space:]]*"' \
    | sed -E 's/^[[:space:]]*\{[[:space:]]*"([A-Za-z0-9_]+)",[[:space:]]*"([A-Za-z0-9_]+)".*/\1 \2/')"

SHIM_MIN=25
shim_count="$(printf '%s\n' "$shim_pairs" | grep -c . || true)"
if [ "${shim_count:-0}" -lt "$SHIM_MIN" ]; then
    echo "check-stdlib-coverage: FAIL (extracted only $shim_count SHIM_TABLE rows from $SHIM_SRC, expected >= $SHIM_MIN — table may have moved/renamed; update this script's awk/sed range)"
    exit 1
fi

while IFS=' ' read -r pkg name; do
    [ -z "$pkg" ] && continue
    record "${pkg}.${name}" "\\b${pkg}\\.${name}\\(" "${pkg}.${name}"
done <<< "$shim_pairs"

# --- 2. Value members (expression_checker.c: os.Args, math.Pi) ----------
value_pairs="$(grep -oE 'strcmp\(package, "[A-Za-z0-9_]+"\) == 0 && strcmp\(name, "[A-Za-z0-9_]+"\) == 0' "$EXPR_CHECKER" \
    | sed -E 's/strcmp\(package, "([A-Za-z0-9_]+)"\) == 0 && strcmp\(name, "([A-Za-z0-9_]+)"\) == 0/\1 \2/')"

VALUE_MIN=2
value_count="$(printf '%s\n' "$value_pairs" | grep -c . || true)"
if [ "${value_count:-0}" -lt "$VALUE_MIN" ]; then
    echo "check-stdlib-coverage: FAIL (extracted only $value_count package value members from $EXPR_CHECKER, expected >= $VALUE_MIN — the os.Args/math.Pi intercept may have moved)"
    exit 1
fi

while IFS=' ' read -r pkg name; do
    [ -z "$pkg" ] && continue
    record "${pkg}.${name}" "\\b${pkg}\\.${name}\\b" "${pkg}.${name}"
done <<< "$value_pairs"

# --- 3. time.Duration constants (type_checker.c: time_export_value calls) --
time_value_pairs="$(grep -oE 'time_export_value\(pkg, "[A-Za-z0-9_]+"' "$SEED_SRC" \
    | sed -E 's/time_export_value\(pkg, "([A-Za-z0-9_]+)"/time \1/')"

TIME_VALUE_MIN=4
time_value_count="$(printf '%s\n' "$time_value_pairs" | grep -c . || true)"
if [ "${time_value_count:-0}" -lt "$TIME_VALUE_MIN" ]; then
    echo "check-stdlib-coverage: FAIL (extracted only $time_value_count time Duration constants from $SEED_SRC, expected >= $TIME_VALUE_MIN — seed_time_package_exports may have moved)"
    exit 1
fi

while IFS=' ' read -r pkg name; do
    [ -z "$pkg" ] && continue
    record "${pkg}.${name}" "\\b${pkg}\\.${name}\\b" "${pkg}.${name}"
done <<< "$time_value_pairs"

# --- 4. Seeded package-level funcs (type_checker.c: time_export_func calls) -
time_func_pairs="$(grep -oE 'time_export_func\(pkg, "[A-Za-z0-9_]+"' "$SEED_SRC" \
    | sed -E 's/time_export_func\(pkg, "([A-Za-z0-9_]+)"/time \1/')"

TIME_FUNC_MIN=2
time_func_count="$(printf '%s\n' "$time_func_pairs" | grep -c . || true)"
if [ "${time_func_count:-0}" -lt "$TIME_FUNC_MIN" ]; then
    echo "check-stdlib-coverage: FAIL (extracted only $time_func_count time package funcs from $SEED_SRC, expected >= $TIME_FUNC_MIN — seed_time_package_exports may have moved)"
    exit 1
fi

while IFS=' ' read -r pkg name; do
    [ -z "$pkg" ] && continue
    record "${pkg}.${name}" "\\b${pkg}\\.${name}\\(" "${pkg}.${name}"
done <<< "$time_func_pairs"

# --- 5. Seeded methods (type_checker.c: sync_export_method + time_export_method) -
method_names="$(grep -oE '(sync|time)_export_method\([^,]+,[^,]+, *"[A-Za-z0-9_]+"' "$SEED_SRC" \
    | sed -E 's/.*, *"([A-Za-z0-9_]+)"$/\1/')"

METHOD_MIN=5
method_count="$(printf '%s\n' "$method_names" | grep -c . || true)"
if [ "${method_count:-0}" -lt "$METHOD_MIN" ]; then
    echo "check-stdlib-coverage: FAIL (extracted only $method_count seeded methods from $SEED_SRC, expected >= $METHOD_MIN — sync/time_export_method call sites may have moved)"
    exit 1
fi

while IFS= read -r name; do
    [ -z "$name" ] && continue
    record ".${name}(" "\\.${name}\\(" ".${name}("
done <<< "$method_names"

# --- 6. goostd exported funcs (real stdlib source dirs only) ------------
# lanes (P6 M1): Partition/Run are plain package funcs and match `^func [A-Z]`
# below; the Lane methods (Own/ID/Publish/HaloLeft/HaloRight/Step) do NOT —
# receivers make them `^func (l *Lane) ...`, invisible to this regex. They
# get real functional coverage from examples/lanes_stencil_probe.goo (the
# BSP halo-exchange golden), just not mechanical extraction here — extending
# the extractor to receiver methods is YAGNI for one package this milestone.
# goostd/cpkg is a deliberate fixture package (comptime-generic compose
# probes) outside stdlib scope, same as kinds/shapes/mypkg/pkgcheck/fwdref —
# not added here.
GOOSTD_PKG_DIRS="strings:goostd/strings strconv:goostd/strconv utf8:goostd/utf8 bits:goostd/bits lanes:goostd/lanes"

GOOSTD_MIN=50
goostd_total=0
for entry in $GOOSTD_PKG_DIRS; do
    pkg="${entry%%:*}"
    dir="${entry#*:}"
    if [ ! -d "$dir" ]; then
        echo "check-stdlib-coverage: FAIL (goostd package dir moved: $dir not found)"
        exit 1
    fi
    names="$(find "$dir" -maxdepth 1 -name '*.go' -exec grep -h -oE '^func [A-Z][A-Za-z0-9_]*' {} + \
        | sed -E 's/^func //')"
    while IFS= read -r name; do
        [ -z "$name" ] && continue
        goostd_total=$((goostd_total + 1))
        record "${pkg}.${name}" "\\b${pkg}\\.${name}\\(" "${pkg}.${name}"
    done <<< "$names"
done

if [ "$goostd_total" -lt "$GOOSTD_MIN" ]; then
    echo "check-stdlib-coverage: FAIL (extracted only $goostd_total exported funcs across goostd/{strings,strconv,utf8,bits,lanes}, expected >= $GOOSTD_MIN — package layout may have moved)"
    exit 1
fi

# --- Report ---------------------------------------------------------------
echo "check-stdlib-coverage: extracted $shim_count shim rows, $value_count value members, $time_value_count time constants, $time_func_count time funcs, $method_count seeded methods, $goostd_total goostd funcs ($total symbols total) across $golden_count golden fixtures"

if [ "${#skipped[@]}" -gt 0 ]; then
    echo "check-stdlib-coverage: ${#skipped[@]} documented carve-out(s) (not golden-coverable, verified elsewhere):"
    for s in "${skipped[@]}"; do
        echo "  - $s"
    done
fi

if [ "${#missing[@]}" -gt 0 ]; then
    echo "check-stdlib-coverage: FAIL — ${#missing[@]} symbol(s) with no golden smoke coverage:"
    for m in "${missing[@]}"; do
        echo "  - $m"
    done
    exit 1
fi

echo "check-stdlib-coverage: PASS"
exit 0
