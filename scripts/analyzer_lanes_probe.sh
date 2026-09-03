#!/usr/bin/env bash
# analyzer_lanes_probe.sh -- task 16 of the 2026-09-02 verification tooling
# plan: the gcc strict lane and the clang-tidy lane, both reading task 14's
# compile-command capture (build/compile_commands.json), both writing SARIF
# or SARIF-shaped output under build/sarif/.
#
# TWO LANES, ONE CAPTURE. Task 15's cppcheck lane and this probe's two lanes
# all read the SAME list -- see tools/gen_compile_commands.sh's header for
# why the capture, not a hand-rolled `find src -name '*.c'`, is the source of
# truth (each entry carries the exact -I/-D/-include/-std flags gcc compiles
# that file with today).
#
# --- gcc lane -----------------------------------------------------------
# For each entry: run the entry's OWN "arguments" (so every -I/-D/-include/
# -std is exactly what the real build uses), with the object output
# redirected to a scratch path under build/sarif/gcc/.objs/ and
# "-Wall -Wextra -Wpedantic -fanalyzer -fdiagnostics-format=sarif-file"
# appended. gcc >= 13 writes "<source-basename>.sarif" BESIDE THE OBJECT
# (verified empirically: `gcc -o /tmp/outdir/t.o -c /tmp/srcdir/t.c
# -fdiagnostics-format=sarif-file` writes /tmp/outdir/t.c.sarif -- the
# directory of -o, the basename of the SOURCE, not of -o). Redirecting -o
# into build/sarif/gcc/.objs/ is what keeps the source tree clean, per the
# task text; the throwaway .o is never linked into anything. Each SARIF is
# then moved to build/sarif/gcc/<stem>.sarif (stem = source basename minus
# ".c"), which is where the task's own acceptance line expects it.
#
# CCACHE. Task 14's captured "arguments" start with "/usr/bin/ccache gcc
# ...". ccache does not track -fanalyzer's SARIF side file as part of a
# cached result: on a CACHE HIT it restores the .o and produces NO sarif at
# all, silently, exit 0. Verified: compiling the identical source+flags
# twice gives a real SARIF the first time and nothing the second time.
# CCACHE_DISABLE=1 is set for every gcc-lane invocation so this lane always
# forces a real compile.
#
# --- clang-tidy lane ------------------------------------------------------
# `clang-tidy -p <dir> <file>` reads the SAME compile_commands.json, but its
# JSONCompilationDatabase resolves a relative "file" by prefixing "directory"
# -- and task 14's capture deliberately writes "directory": "." for
# portability (see that script's header). A "." directory is not absolute,
# so the prefixed path is never absolute either, and clang-tidy reports
# "Compile command not found" for every single entry (verified: reproduced
# with the real 58-entry capture, fixed by rewriting "directory" to the repo
# root). This lane therefore works from a THROWAWAY copy of the capture,
# under build/sarif/clang-tidy/.cc/, with "directory" rewritten to the repo
# root -- never a change to tools/gen_compile_commands.sh's own output,
# which task 15's cppcheck lane already depends on staying "." (see that
# script's header on why absolute would make the derivation non-portable).
#
# This build's clang-tidy (LLVM 22) enables NO named check by default
# outside a .clang-tidy file -- `-dump-config` reports Checks:
# 'clang-diagnostic-*' but running with no -checks at all fails outright
# with "Error: no checks enabled." (verified). Absent a repository
# .clang-tidy (checked at runtime; there is none as of this commit), the
# lane passes an explicit -checks="clang-diagnostic-*,clang-analyzer-*" so
# the run actually exercises clang-analyzer's dataflow checks rather than
# silently doing nothing. A repository .clang-tidy, if one is added later,
# is used as-is (no -checks override), matching normal clang-tidy behaviour.
#
# clang-tidy has no native SARIF emitter (task 17 converts its plain-text
# output and cppcheck's XML into one SARIF-shaped database). Per file:
# build/sarif/clang-tidy/<stem>.txt (the raw run-together stdout+stderr) and,
# when --export-fixes is supported (checked at runtime), <stem>.yaml.
#
# --- companion edits (plan decision 2) ------------------------------------
#   - this file's --self-test, below, for probe-teeth-probe
#   - tools/parity-gate-count.txt +2 (two new VERIFY_ALL_DEPS entries)
#   - tests/probes/census.txt: this branch's base (main) does not have that
#     file, so there is nothing to regenerate (task 14 and 15's reports
#     record the same fact).
#
# Usage:
#   scripts/analyzer_lanes_probe.sh              # gate: fail on a lane error
#   scripts/analyzer_lanes_probe.sh --self-test  # positive+negative control
#
# Environment overrides:
#   JOBS                  parallel workers for both lanes (default: nproc)
#   COMPILE_COMMANDS_GEN  compile-command generator (default: tools/gen_compile_commands.sh)
#   COMPILE_COMMANDS_OUT  the capture itself (default: build/compile_commands.json)
#   SARIF_GCC_DIR         gcc lane output dir (default: build/sarif/gcc)
#   SARIF_TIDY_DIR        clang-tidy lane output dir (default: build/sarif/clang-tidy)
#   CLANG_TIDY_CHECKS     -checks= value when no .clang-tidy exists
#                         (default: clang-diagnostic-*,clang-analyzer-*)
set -euo pipefail

PROBE="analyzer-lanes-probe"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

JOBS="${JOBS:-$(nproc)}"
COMPILE_COMMANDS_GEN="${COMPILE_COMMANDS_GEN:-$REPO_ROOT/tools/gen_compile_commands.sh}"
COMPILE_COMMANDS_OUT="${COMPILE_COMMANDS_OUT:-$REPO_ROOT/build/compile_commands.json}"
SARIF_GCC_DIR="${SARIF_GCC_DIR:-$REPO_ROOT/build/sarif/gcc}"
SARIF_TIDY_DIR="${SARIF_TIDY_DIR:-$REPO_ROOT/build/sarif/clang-tidy}"
CLANG_TIDY_CHECKS="${CLANG_TIDY_CHECKS:-clang-diagnostic-*,clang-analyzer-*}"

if ! command -v gcc >/dev/null 2>&1; then
	echo "$PROBE: FAIL (gcc is required)" >&2
	exit 2
fi
if ! command -v clang-tidy >/dev/null 2>&1; then
	echo "$PROBE: FAIL (clang-tidy is required)" >&2
	exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "$PROBE: FAIL (python3 is required)" >&2
	exit 2
fi

GCC_VERSION="$(gcc --version | head -1)"
CLANG_TIDY_VERSION="$(clang-tidy --version | sed -n 's/^[[:space:]]*LLVM version[[:space:]]*//p')"

# --- the gcc lane, callable on ANY compile-command JSON (the real capture,
#     or the self-test's two-entry scratch one) -- one mechanism, two
#     callers, so the self-test proves the same code path the real run uses.
#     Prints one "GCC_FILE <file> rc=<n> count=<n> rules=<csv>" line per
#     entry and one final "GCC_SUMMARY entries=<n> fails=<n> results=<n>
#     wall=<seconds>" line. -----------------------------------------------
run_gcc_lane() { # $1 = compile_commands.json path, $2 = sarif out dir, $3 = objs scratch dir, $4 = jobs
	python3 - "$1" "$2" "$3" "$4" <<'PYEOF'
import json, os, sys, time
from concurrent.futures import ThreadPoolExecutor
import subprocess

cc_path, sarif_dir, objs_dir, jobs = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
repo_root = os.getcwd()

entries = json.load(open(cc_path, encoding="utf-8"))
os.makedirs(sarif_dir, exist_ok=True)
os.makedirs(objs_dir, exist_ok=True)

EXTRA = ["-Wall", "-Wextra", "-Wpedantic", "-fanalyzer", "-fdiagnostics-format=sarif-file"]

def stem_of(path):
    b = os.path.basename(path)
    return b[:-2] if b.endswith(".c") else b

def run_one(e):
    src = e["file"]
    stem = stem_of(src)
    args = list(e["arguments"])
    # "-c <src> -o <obj>" is always the tail of an entry's own arguments
    # (both gen_compile_commands.sh emission paths end this way -- see its
    # header). Find the LAST "-o" rather than assume a fixed index, in case
    # a future entry shape puts other flags after it.
    oi = max(i for i, tok in enumerate(args) if tok == "-o")
    obj_path = os.path.join(objs_dir, stem + ".o")
    args[oi + 1] = obj_path
    args = args + EXTRA
    cwd = os.path.join(repo_root, e.get("directory", "."))
    # CCACHE_DISABLE=1: task 14's own entries start with "/usr/bin/ccache
    # gcc ...", and ccache's cache does not know about -fanalyzer's SARIF
    # side file -- a CACHE HIT restores the .o and silently produces NO
    # sarif at all, with exit 0 (verified: the same source+flags compiled
    # twice gives a real SARIF the first time and nothing the second,
    # rc=0 both times). A gate that goes quiet on a warm cache instead of
    # failing loudly is worse than one that never ran -- force a real
    # compiler invocation every time.
    env = dict(os.environ)
    env["CCACHE_DISABLE"] = "1"
    t0 = time.time()
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True, env=env)
    dt = time.time() - t0
    gen_sarif = os.path.join(objs_dir, os.path.basename(src) + ".sarif")
    if r.returncode != 0:
        return (src, dt, r.returncode, 0, "", r.stderr[-2000:])
    if not os.path.isfile(gen_sarif):
        return (src, dt, 2, 0, "", f"no sarif produced at {gen_sarif}")
    final_sarif = os.path.join(sarif_dir, stem + ".sarif")
    os.replace(gen_sarif, final_sarif)
    with open(final_sarif, encoding="utf-8") as f:
        d = json.load(f)
    results = []
    for run in d.get("runs", []):
        results.extend(run.get("results", []))
    rule_ids = sorted({r2.get("ruleId", "") for r2 in results})
    return (src, dt, 0, len(results), ",".join(rule_ids), "")

t_start = time.time()
out = []
with ThreadPoolExecutor(max_workers=max(jobs, 1)) as ex:
    for res in ex.map(run_one, entries):
        out.append(res)
wall = time.time() - t_start

fails = 0
total_results = 0
for src, dt, rc, count, rules, err in out:
    print(f"GCC_FILE {src} rc={rc} count={count} rules={rules}")
    if rc != 0:
        fails += 1
        sys.stderr.write(f"gcc lane: {src} failed (rc={rc}): {err}\n")
    else:
        total_results += count

print(f"GCC_SUMMARY entries={len(entries)} fails={fails} results={total_results} wall={wall:.2f}")
PYEOF
}

# --- the clang-tidy lane, same shape as run_gcc_lane. ----------------------
run_tidy_lane() { # $1 = compile_commands.json path, $2 = out dir, $3 = jobs, $4 = -checks value (empty = use repo .clang-tidy), $5 = repo root (for the abs-directory rewrite)
	python3 - "$1" "$2" "$3" "$4" "$5" <<'PYEOF'
import json, os, sys, time
from concurrent.futures import ThreadPoolExecutor
import subprocess

cc_path, out_dir, jobs, checks, repo_root = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5]

entries = json.load(open(cc_path, encoding="utf-8"))
os.makedirs(out_dir, exist_ok=True)

# See this script's header: clang-tidy's JSONCompilationDatabase needs an
# ABSOLUTE "directory" to resolve a relative "file". Rewrite a throwaway
# copy rather than touch the real capture (task 15's cppcheck lane needs
# "." to stay relative).
cc_dir = os.path.join(out_dir, ".cc")
os.makedirs(cc_dir, exist_ok=True)
abs_entries = []
for e in entries:
    e2 = dict(e)
    e2["directory"] = repo_root
    abs_entries.append(e2)
with open(os.path.join(cc_dir, "compile_commands.json"), "w", encoding="utf-8") as f:
    json.dump(abs_entries, f)

export_fixes_ok = False
try:
    h = subprocess.run(["clang-tidy", "--help"], capture_output=True, text=True)
    export_fixes_ok = "--export-fixes" in h.stdout
except FileNotFoundError:
    pass

def stem_of(path):
    b = os.path.basename(path)
    return b[:-2] if b.endswith(".c") else b

def run_one(e):
    src = e["file"]
    stem = stem_of(src)
    args = ["clang-tidy"]
    if checks:
        args.append(f"-checks={checks}")
    args += ["-p", cc_dir]
    if export_fixes_ok:
        args.append(f"--export-fixes={os.path.join(out_dir, stem + '.yaml')}")
    args.append(src)
    t0 = time.time()
    r = subprocess.run(args, cwd=repo_root, capture_output=True, text=True)
    dt = time.time() - t0
    with open(os.path.join(out_dir, stem + ".txt"), "w", encoding="utf-8") as f:
        f.write(r.stdout)
        f.write(r.stderr)
    nwarn = r.stdout.count("warning:")
    # clang-tidy exits non-zero on a genuine compile ERROR in the TU (not on
    # a plain warning), which is the one failure mode worth surfacing loudly
    # -- see gcc lane's identical rc convention.
    return (src, dt, r.returncode, nwarn)

t_start = time.time()
out = []
with ThreadPoolExecutor(max_workers=max(jobs, 1)) as ex:
    for res in ex.map(run_one, entries):
        out.append(res)
wall = time.time() - t_start

fails = 0
total_warn = 0
for src, dt, rc, nwarn in out:
    print(f"TIDY_FILE {src} rc={rc} warnings={nwarn}")
    if rc != 0:
        fails += 1
    total_warn += nwarn

print(f"TIDY_SUMMARY entries={len(entries)} fails={fails} warnings={total_warn} wall={wall:.2f} export_fixes={1 if export_fixes_ok else 0}")
PYEOF
}

# --self-test: the positive control (a null dereference, exactly one gcc
# result whose rule id names it) and TWO negative controls (an empty
# function gives zero gcc results; clang-tidy still finds something on the
# bug so the lane is not silently inert).
if [ "${1:-}" = "--self-test" ]; then
	SELF="$PROBE --self-test"
	W="$(mktemp -d)"
	trap 'rm -rf "$W"' EXIT

	cat >"$W/bug.c" <<'EOF'
int bug(void) {
	int *p = 0;
	return *p;
}
EOF
	cat >"$W/ctrl.c" <<'EOF'
void ctrl(void) {
}
EOF
	cat >"$W/cc.json" <<EOF
[
  {"directory": "$W", "file": "bug.c", "arguments": ["gcc", "-c", "bug.c", "-o", "bug.o"]},
  {"directory": "$W", "file": "ctrl.c", "arguments": ["gcc", "-c", "ctrl.c", "-o", "ctrl.o"]}
]
EOF

	fail=0

	gcc_out="$(run_gcc_lane "$W/cc.json" "$W/sarif-gcc" "$W/objs-gcc" 2)"
	bug_line="$(printf '%s\n' "$gcc_out" | grep '^GCC_FILE bug.c ')"
	ctrl_line="$(printf '%s\n' "$gcc_out" | grep '^GCC_FILE ctrl.c ')"

	if printf '%s\n' "$bug_line" | grep -q 'count=1 ' && printf '%s\n' "$bug_line" | grep -q 'null-dereference'; then
		echo "    ok: positive control (null dereference) gives exactly one gcc result, ruleId names it"
		echo "        $bug_line"
	else
		echo "$SELF: FAIL (positive control did not give one null-dereference result): $bug_line"
		fail=1
	fi

	if printf '%s\n' "$ctrl_line" | grep -q 'count=0 '; then
		echo "    ok: negative control (empty function) gives zero gcc results"
		echo "        $ctrl_line"
	else
		echo "$SELF: FAIL (negative control gave a nonzero gcc result count): $ctrl_line"
		fail=1
	fi

	tidy_out="$(run_tidy_lane "$W/cc.json" "$W/tidy" 2 "clang-diagnostic-*,clang-analyzer-*" "$W")"
	tidy_bug_line="$(printf '%s\n' "$tidy_out" | grep '^TIDY_FILE bug.c ')"
	if printf '%s\n' "$tidy_bug_line" | grep -qE 'warnings=[1-9][0-9]*'; then
		echo "    ok: clang-tidy lane finds at least one warning on the same positive control"
		echo "        $tidy_bug_line"
	else
		echo "$SELF: FAIL (clang-tidy lane found no warning on the positive control): $tidy_bug_line"
		fail=1
	fi

	if [ "$fail" -ne 0 ]; then
		exit 1
	fi
	echo "$SELF: PASS (gcc lane: 1 result / 0 results; clang-tidy lane: >=1 warning)"
	exit 0
fi

if [ -n "${1:-}" ]; then
	echo "usage: $0 [--self-test]" >&2
	exit 2
fi

# --- the real run: task 14's capture over the whole tree. -----------------
if [ ! -x "$COMPILE_COMMANDS_GEN" ]; then
	echo "$PROBE: FAIL (compile-command generator not found or not executable: $COMPILE_COMMANDS_GEN)" >&2
	exit 2
fi

# Regenerate unconditionally, the same rule scripts/misra-scan.sh and
# scripts/compile_commands_current_probe.sh already follow -- a staleness
# check is one more thing to get wrong, where a fresh `make -Bn` dry run
# costs a second or two and is always correct.
"$COMPILE_COMMANDS_GEN" "$COMPILE_COMMANDS_OUT"
entry_count="$(python3 -c "import json,sys; print(len(json.load(open(sys.argv[1]))))" "$COMPILE_COMMANDS_OUT")"

rm -rf "$SARIF_GCC_DIR"
mkdir -p "$SARIF_GCC_DIR"
gcc_objs_dir="$SARIF_GCC_DIR/.objs"

rm -rf "$SARIF_TIDY_DIR"
mkdir -p "$SARIF_TIDY_DIR"

HAS_CLANG_TIDY_CFG=0
[ -f "$REPO_ROOT/.clang-tidy" ] && HAS_CLANG_TIDY_CFG=1
if [ "$HAS_CLANG_TIDY_CFG" -eq 1 ]; then
	tidy_checks=""
else
	tidy_checks="$CLANG_TIDY_CHECKS"
fi

gcc_out="$(run_gcc_lane "$COMPILE_COMMANDS_OUT" "$SARIF_GCC_DIR" "$gcc_objs_dir" "$JOBS")"
gcc_summary="$(printf '%s\n' "$gcc_out" | grep '^GCC_SUMMARY ')"
gcc_fails="$(printf '%s\n' "$gcc_summary" | sed -n 's/.*fails=\([0-9]*\).*/\1/p')"
gcc_results="$(printf '%s\n' "$gcc_summary" | sed -n 's/.*results=\([0-9]*\).*/\1/p')"
gcc_wall="$(printf '%s\n' "$gcc_summary" | sed -n 's/.*wall=\([0-9.]*\).*/\1/p')"

tidy_out="$(run_tidy_lane "$COMPILE_COMMANDS_OUT" "$SARIF_TIDY_DIR" "$JOBS" "$tidy_checks" "$REPO_ROOT")"
tidy_summary="$(printf '%s\n' "$tidy_out" | grep '^TIDY_SUMMARY ')"
tidy_fails="$(printf '%s\n' "$tidy_summary" | sed -n 's/.*fails=\([0-9]*\).*/\1/p')"
tidy_warnings="$(printf '%s\n' "$tidy_summary" | sed -n 's/.*warnings=\([0-9]*\).*/\1/p')"
tidy_wall="$(printf '%s\n' "$tidy_summary" | sed -n 's/.*wall=\([0-9.]*\).*/\1/p')"
tidy_export_fixes="$(printf '%s\n' "$tidy_summary" | sed -n 's/.*export_fixes=\([0-9]\).*/\1/p')"

echo "$PROBE: capture $COMPILE_COMMANDS_OUT, $entry_count entries"
echo "$PROBE: gcc lane  -- $GCC_VERSION"
echo "$PROBE: gcc lane  -- $gcc_fails/$entry_count compile failures, $gcc_results analyser results, ${gcc_wall}s wall, SARIF in $SARIF_GCC_DIR"
echo "$PROBE: clang-tidy lane -- $CLANG_TIDY_VERSION, repo .clang-tidy present: $([ "$HAS_CLANG_TIDY_CFG" -eq 1 ] && echo yes || echo no)"
echo "$PROBE: clang-tidy lane -- $tidy_fails/$entry_count compile failures, $tidy_warnings warnings, ${tidy_wall}s wall, export-fixes YAML: $([ "$tidy_export_fixes" = "1" ] && echo yes || echo no), output in $SARIF_TIDY_DIR"

if [ "${gcc_fails:-1}" != "0" ] || [ "${tidy_fails:-1}" != "0" ]; then
	echo "$PROBE: FAIL ($gcc_fails gcc-lane failure(s), $tidy_fails clang-tidy-lane failure(s))"
	exit 1
fi

echo "$PROBE: PASS (gcc $gcc_results results, clang-tidy $tidy_warnings warnings, across $entry_count translation units)"
