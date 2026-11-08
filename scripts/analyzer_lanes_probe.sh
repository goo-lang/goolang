#!/usr/bin/env bash
# analyzer_lanes_probe.sh -- task 16 of the 2026-09-02 verification tooling
# plan: the gcc strict lane and the clang-tidy lane, both reading task 14's
# compile-command capture (build/compile_commands.json), both writing SARIF
# or SARIF-shaped output under build/sarif/. Task 17 adds a third lane,
# cppcheck, so task 17's tools/sarif_merge.py has one place to read all
# three tools' SARIF from.
#
# THREE LANES, ONE CAPTURE. This probe's three lanes and task 15's own
# misra-scan.sh all read the SAME list -- see tools/gen_compile_commands.sh's
# header for why the capture, not a hand-rolled `find src -name '*.c'`, is
# the source of truth (each entry carries the exact -I/-D/-include/-std
# flags gcc compiles that file with today).
#
# --- gcc lane -----------------------------------------------------------
# For each entry: run the entry's OWN "arguments" (so every -I/-D/-include/
# -std is exactly what the real build uses), with the object output
# redirected to a scratch path under build/sarif/gcc/.objs/ and
# "-Wall -Wextra -Wpedantic -fanalyzer -fdiagnostics-format=sarif-file"
# appended. gcc >= 13 writes "<dumpbase-without-its-own-extension>.<source
# extension>.sarif" BESIDE THE OBJECT (verified empirically: an object
# named weirdname123.o compiled from ctrl.c writes weirdname123.c.sarif in
# the object's own directory). Redirecting -o into build/sarif/gcc/.objs/
# is what keeps the source tree clean, per the task text; the throwaway .o
# is never linked into anything.
#
# OUTPUT KEYS. Every output of both lanes is named after the entry's
# repository-relative "file" path with "/" replaced by "__", extension
# kept (for example "src/runtime/channels.c" -> "src__runtime__channels.c",
# so the gcc lane's SARIF is build/sarif/gcc/src__runtime__channels.c.sarif
# and the clang-tidy lane's text is
# build/sarif/clang-tidy/src__runtime__channels.c.txt). Keying by the
# BASENAME ALONE (an earlier version of this script did this) silently
# clobbers two entries that share a basename in different directories
# under JOBS-way parallelism -- neither compile fails, so nothing signals
# the collision; see --self-test's "same basename, two directories" case,
# which proves two distinct files come out. Before either lane starts,
# every entry's key is checked for a collision (two DIFFERENT "file"
# values that happen to produce the same key, for example a literal "__"
# in a path segment) and the probe fails loudly, rather than clobbering,
# if one is found.
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
# with "Error: no checks enabled." (verified). For each entry, this lane
# WALKS UP from that entry's own file directory to the entry's declared
# project root (bounded there, not to the filesystem root, since anything
# above the project is out of this probe's business) looking for a
# ".clang-tidy" file, the same direction clang-tidy itself searches. Absent
# one anywhere on that walk (there is none as of this commit, checked at
# run time for every entry), the lane passes an explicit
# -checks="clang-diagnostic-*,clang-analyzer-*" so the run actually
# exercises clang-analyzer's dataflow checks rather than silently doing
# nothing. A directory that DOES have a .clang-tidy on its walk is used
# as-is (no -checks override), matching normal clang-tidy behaviour.
#
# clang-tidy has no native SARIF emitter (task 17 converts its plain-text
# output and cppcheck's XML into one SARIF-shaped database). Per file, keyed
# as above: build/sarif/clang-tidy/<key>.txt (the raw run-together
# stdout+stderr) and, when --export-fixes is supported (checked at
# runtime), <key>.yaml.
#
# --- cppcheck lane (task 17) -----------------------------------------------
# cppcheck's own --project mode reads the WHOLE compile database in one
# process and reports every file's findings together, unlike the gcc and
# clang-tidy lanes above, which invoke the tool once PER FILE. So this lane
# writes ONE aggregate SARIF file, build/sarif/cppcheck/cppcheck.sarif,
# rather than one per entry, and there is no per-file key scheme to collide.
#
# It reuses docs/misra/tools/scan.sh -- the SAME script misra-scan.sh calls
# -- rather than a bespoke cppcheck invocation, so its flags (--std=c11,
# --enable=style, --library=posix/gnu, the two --include= headers) never
# drift from that gate's. Task 18 checks the SARIF gate sees the same counts
# misra-scan.sh's text baseline does; a second, smaller flag set here could
# not be trusted for that comparison (VERIFIED: dropping --enable=style
# alone, which an early draft of this lane did by copying only the flags the
# task text names, took the misra rule count on this tree from 2,491 to 8 --
# cppcheck's MISRA checks report at "style" severity and are silently
# dropped without it).
#
# THE ADDON CONFIG. docs/misra/tools/README.md deliberately keeps a REAL
# misra.json (one built from a licensed MISRA PDF's rule text) out of this
# repository, and misra-scan.sh has no default for it -- MISRA_JSON must be
# set by hand. That is fine for misra-scan.sh, a local-only gate never run
# in CI (see its own header), but analyzer-lanes-probe is in VERIFY_ALL_DEPS
# and must stay "green on any machine" (Makefile, verify-core's own
# description) with no licensed PDF anywhere near it. The addon SCRIPT NAME
# is not copyrighted text -- only the guideline text a --rule-texts= file
# would carry is -- so this lane's default, docs/misra/tools/misra.json, is
# `{"script": "misra.py"}` with no --rule-texts arg: it is committed, and
# VERIFIED to change no rule ID and no count from a licensed config, only
# the message text (a bare config reports "misra violation (use
# --rule-texts=<file> to get proper output)" in place of the guideline's own
# wording). Set MISRA_JSON to a richer config for readable messages; the
# gate's own numbers do not move either way.
#
# --self-test's positive control reuses the gcc/clang-tidy lanes' own
# "bug.c" fixture (a null dereference, not `static`, no prototype): cppcheck
# reports it as misra-c2012-8.4 (external declaration) among others, so the
# SAME fixture proves all three lanes without a fourth scratch file.
#
# A UNIT THAT STOPS AT #error WAS ANALYSED BY NOTHING (task 17b). cppcheck
# abandons a translation unit at a preprocessor #error, reports one
# preprocessorErrorDirective, and still prints "Checking <file>" for it and
# exits 0, so "checked" counted it and the lane went green. Until task 17b
# that was all 16 LLVM units (src/codegen/*.c, src/compiler/goo.c): each
# stopped in llvm-c/DataTypes.h, and a suppression in
# docs/misra/misra-suppressions.txt hid the report. The lane now fails when
# any result has a parse-failure ID (preprocessorErrorDirective, syntaxError,
# internalError, internalAstError, cppcheckError: the set in
# docs/misra/tools/aggregate.py), and also when a suppression-list entry or
# an inline `cppcheck-suppress` comment has an ID glob that covers one of
# them (fnmatch, so "preprocessor*" and "*" count), because such an entry
# makes the first check blind. tools/cppcheck_lane_check.py does both.
# Known exceptions, listed by name there: the three all-ID Bison entries
# ("*:src/parser/parser.tab.c", ".tab.h", "parser.y"). A parse failure in
# those files stays hidden; they are excluded from MISRA on purpose.
run_cppcheck_lane() { # $1 = compile_commands.json, $2 = sarif out dir, $3 = jobs, $4 = misra addon config, $5 = suppressions list
	local cc_path="$1" out_dir="$2" jobs="$3" misra_json="$4" suppressions="$5"
	mkdir -p "$out_dir"
	local run_log="$out_dir/cppcheck-run.log"
	local sarif_out="$out_dir/cppcheck.sarif"
	local entry_count
	entry_count="$(python3 -c "import json,sys; print(len(json.load(open(sys.argv[1]))))" "$cc_path")"
	local t0 t1 wall
	t0="$(date +%s.%N)"
	MISRA_JSON="$misra_json" "$REPO_ROOT/docs/misra/tools/scan.sh" "$run_log" \
		-j "$jobs" \
		"--suppressions-list=$suppressions" \
		"--project=$cc_path" \
		--output-format=sarif \
		"--output-file=$sarif_out" \
		-- >/dev/null
	t1="$(date +%s.%N)"
	wall="$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')"
	# scan.sh's own last command is always an `echo`, so ITS exit status
	# tells a caller nothing -- read the SCAN_EXIT= marker it writes into
	# the run log instead, the same convention docs/misra/tools/gate.py's
	# tally() already relies on for the plain-text gate.
	local scan_exit checked results
	scan_exit="$(sed -n 's/^SCAN_EXIT=//p' "$run_log" | tail -1)"
	checked="$(grep -c '^Checking ' "$run_log" || true)"
	results=0
	if [ -f "$sarif_out" ]; then
		results="$(python3 -c "
import json, sys
d = json.load(open(sys.argv[1]))
print(sum(len(r.get('results', [])) for r in d.get('runs', [])))
" "$sarif_out")"
	fi
	# tools/cppcheck_lane_check.py prints one CPPCHECK_PARSE_FAILURE line per
	# parse-failure result, one CPPCHECK_BLIND_SUPPRESSION line per list entry
	# or inline comment that hides one, and CPPCHECK_KNOWN_EXCEPTION lines for
	# the three Bison entries; cppcheck_lane_verdict below fails on the first
	# two. A failure of the tool itself is a lane failure too (counts "?").
	local check_out pp_errors="?" blind="?"
	if check_out="$(python3 "$REPO_ROOT/tools/cppcheck_lane_check.py" "$sarif_out" "$suppressions" "$cc_path" "$REPO_ROOT" 2>&1)"; then
		pp_errors="$(printf '%s\n' "$check_out" | sed -n 's/^CPPCHECK_CHECK parse_failures=\([0-9]*\) .*/\1/p')"
		blind="$(printf '%s\n' "$check_out" | sed -n 's/^CPPCHECK_CHECK .* blind_suppressions=\([0-9]*\)$/\1/p')"
	fi
	printf '%s\n' "$check_out" | grep -v '^CPPCHECK_CHECK ' || true
	echo "CPPCHECK_SUMMARY entries=$entry_count checked=$checked exit=${scan_exit:-none} results=$results wall=$wall parse_failures=${pp_errors:-?} blind_suppressions=${blind:-?}"
}

# The lane's one verdict, shared by the real run, --cppcheck-lane and
# --self-test. $1 = a CPPCHECK_SUMMARY line. Returns 0 only when cppcheck
# exited 0, checked at least one unit, no unit hit a parse failure, and no
# suppression (list entry or inline comment) hides one.
cppcheck_lane_verdict() {
	local summary="$1"
	printf '%s\n' "$summary" | grep -q ' exit=0 ' || return 1
	if printf '%s\n' "$summary" | grep -q ' checked=0 '; then
		return 1
	fi
	printf '%s\n' "$summary" | grep -q ' parse_failures=0 ' || return 1
	printf '%s\n' "$summary" | grep -qE ' blind_suppressions=0$' || return 1
	return 0
}

# --- companion edits (plan decision 2) ------------------------------------
#   - this file's --self-test, below, for probe-teeth-probe
#   - tools/parity-gate-count.txt +2 (two new VERIFY_ALL_DEPS entries --
#     findings-probe and findings-probe-selftest, task 17's own gate; this
#     lane joining analyzer-lanes-probe adds no new VERIFY_ALL_DEPS entry)
#   - tests/probes/census.txt: this branch's base (main) does not have that
#     file, so there is nothing to regenerate (task 14 and 15's reports
#     record the same fact).
#
# Usage:
#   scripts/analyzer_lanes_probe.sh              # gate: fail on a lane error
#   scripts/analyzer_lanes_probe.sh --self-test  # positive+negative control
#   scripts/analyzer_lanes_probe.sh --cppcheck-lane <db> <out dir> <jobs>
#                                                # the cppcheck lane alone
#
# Environment overrides:
#   JOBS                  parallel workers for all three lanes (default: nproc)
#   COMPILE_COMMANDS_GEN  compile-command generator (default: tools/gen_compile_commands.sh)
#   COMPILE_COMMANDS_OUT  the capture itself (default: build/compile_commands.json)
#   SARIF_GCC_DIR         gcc lane output dir (default: build/sarif/gcc)
#   SARIF_TIDY_DIR        clang-tidy lane output dir (default: build/sarif/clang-tidy)
#   SARIF_CPPCHECK_DIR    cppcheck lane output dir (default: build/sarif/cppcheck)
#   CLANG_TIDY_CHECKS     fallback -checks= value, used per file only where
#                         that file's own directory walk finds no
#                         .clang-tidy (default: clang-diagnostic-*,clang-analyzer-*)
#   MISRA_JSON            cppcheck addon config (default: docs/misra/tools/misra.json)
#   MISRA_SUPPRESSIONS    cppcheck suppression list (default: docs/misra/misra-suppressions.txt)
set -euo pipefail

PROBE="analyzer-lanes-probe"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

JOBS="${JOBS:-$(nproc)}"
COMPILE_COMMANDS_GEN="${COMPILE_COMMANDS_GEN:-$REPO_ROOT/tools/gen_compile_commands.sh}"
COMPILE_COMMANDS_OUT="${COMPILE_COMMANDS_OUT:-$REPO_ROOT/build/compile_commands.json}"
SARIF_GCC_DIR="${SARIF_GCC_DIR:-$REPO_ROOT/build/sarif/gcc}"
SARIF_TIDY_DIR="${SARIF_TIDY_DIR:-$REPO_ROOT/build/sarif/clang-tidy}"
SARIF_CPPCHECK_DIR="${SARIF_CPPCHECK_DIR:-$REPO_ROOT/build/sarif/cppcheck}"
CLANG_TIDY_CHECKS="${CLANG_TIDY_CHECKS:-clang-diagnostic-*,clang-analyzer-*}"
MISRA_JSON="${MISRA_JSON:-$REPO_ROOT/docs/misra/tools/misra.json}"
MISRA_SUPPRESSIONS="${MISRA_SUPPRESSIONS:-$REPO_ROOT/docs/misra/misra-suppressions.txt}"

if ! command -v gcc >/dev/null 2>&1; then
	echo "$PROBE: FAIL (gcc is required)" >&2
	exit 2
fi
if ! command -v clang-tidy >/dev/null 2>&1; then
	echo "$PROBE: FAIL (clang-tidy is required)" >&2
	exit 2
fi
if ! command -v cppcheck >/dev/null 2>&1; then
	echo "$PROBE: FAIL (cppcheck is required)" >&2
	exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "$PROBE: FAIL (python3 is required)" >&2
	exit 2
fi
if [ ! -f "$MISRA_JSON" ]; then
	echo "$PROBE: FAIL (no cppcheck addon config at $MISRA_JSON)" >&2
	exit 2
fi

GCC_VERSION="$(gcc --version | head -1)"
CLANG_TIDY_VERSION="$(clang-tidy --version | sed -n 's/.*LLVM version[[:space:]]*//p')"
CPPCHECK_VERSION="$(cppcheck --version)"

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

def key_of(path):
    # Repository-relative path, "/" -> "__", extension kept -- see this
    # script's header ("OUTPUT KEYS"). Keying by the basename alone lets
    # two entries in different directories with the same basename clobber
    # each other's scratch object and SARIF under JOBS-way parallelism,
    # silently (neither compile fails).
    return path.replace("/", "__")

# Fail loudly, before compiling anything, if two DIFFERENT entries would
# produce the SAME key (for example a literal "__" in one path segment
# colliding with a real "/" in another) -- a collision here is exactly the
# clobbering this keying scheme exists to prevent, and must never be
# discovered by one SARIF silently overwriting another.
seen = {}
for e in entries:
    k = key_of(e["file"])
    seen.setdefault(k, []).append(e["file"])
dups = {k: v for k, v in seen.items() if len(v) > 1}
if dups:
    for k, files in dups.items():
        sys.stderr.write(f"analyzer-lanes gcc lane: output key {k!r} is shared by: {', '.join(files)}\n")
    sys.exit(3)

def run_one(e):
    src = e["file"]
    key = key_of(src)
    ext = os.path.splitext(src)[1] or ".c"
    obj_stem = key[: -len(ext)] if key.endswith(ext) else key
    args = list(e["arguments"])
    # "-c <src> -o <obj>" is always the tail of an entry's own arguments
    # (both gen_compile_commands.sh emission paths end this way -- see its
    # header). Find the LAST "-o" rather than assume a fixed index, in case
    # a future entry shape puts other flags after it.
    oi = max(i for i, tok in enumerate(args) if tok == "-o")
    obj_path = os.path.join(objs_dir, obj_stem + ".o")
    args[oi + 1] = obj_path
    # This lane COUNTS findings (the findings-probe ratchet compares them), so a
    # finding must not stop the compile. CFLAGS carries -Werror since Task 22,
    # and -fanalyzer/-Wpedantic findings would turn it into a compile failure.
    args = [a for a in args if a != "-Werror"]
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
    # gcc's dumpbase defaults to the -o basename minus ITS OWN extension
    # (obj_stem here), and the SARIF it writes is "<dumpbase><source
    # extension>.sarif" -- which is exactly "<obj_stem><ext>.sarif" ==
    # "<key>.sarif", since obj_stem + ext == key by construction above.
    gen_sarif = os.path.join(objs_dir, key + ".sarif")
    if r.returncode != 0:
        return (src, dt, r.returncode, 0, "", r.stderr[-2000:])
    if not os.path.isfile(gen_sarif):
        return (src, dt, 2, 0, "", f"no sarif produced at {gen_sarif}")
    final_sarif = os.path.join(sarif_dir, key + ".sarif")
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
run_tidy_lane() { # $1 = compile_commands.json path, $2 = out dir, $3 = jobs, $4 = fallback -checks= value (used per-file only where that file's own .clang-tidy walk finds nothing), $5 = repo root (for the abs-directory rewrite)
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

def key_of(path):
    # See the gcc lane's identical helper and this script's header
    # ("OUTPUT KEYS"): repository-relative path, "/" -> "__", extension
    # kept, so two same-basename entries in different directories cannot
    # clobber each other's .txt/.yaml output.
    return path.replace("/", "__")

seen = {}
for e in entries:
    k = key_of(e["file"])
    seen.setdefault(k, []).append(e["file"])
dups = {k: v for k, v in seen.items() if len(v) > 1}
if dups:
    for k, files in dups.items():
        sys.stderr.write(f"analyzer-lanes clang-tidy lane: output key {k!r} is shared by: {', '.join(files)}\n")
    sys.exit(3)

# M1: walk up from a file's own directory to its entry's declared project
# root looking for ".clang-tidy", the same direction clang-tidy itself
# searches -- not just the project root alone. Under ten lines, per the
# review's own bar for choosing the walk over a documented root-only check.
def has_config(file_dir, stop_dir):
    d = os.path.abspath(file_dir)
    stop_dir = os.path.abspath(stop_dir)
    while True:
        if os.path.isfile(os.path.join(d, ".clang-tidy")):
            return True
        if d == stop_dir or os.path.dirname(d) == d:
            return False
        d = os.path.dirname(d)

def run_one(e):
    src = e["file"]
    key = key_of(src)
    orig_dir = e.get("directory", ".")
    base = orig_dir if os.path.isabs(orig_dir) else os.path.join(repo_root, orig_dir)
    file_dir = os.path.dirname(os.path.join(base, src))
    cfg_found = has_config(file_dir, base)
    args = ["clang-tidy"]
    if checks and not cfg_found:
        args.append(f"-checks={checks}")
    args += ["-p", cc_dir]
    if export_fixes_ok:
        args.append(f"--export-fixes={os.path.join(out_dir, key + '.yaml')}")
    args.append(src)
    t0 = time.time()
    r = subprocess.run(args, cwd=repo_root, capture_output=True, text=True)
    dt = time.time() - t0
    with open(os.path.join(out_dir, key + ".txt"), "w", encoding="utf-8") as f:
        f.write(r.stdout)
        f.write(r.stderr)
    nwarn = r.stdout.count("warning:")
    # clang-tidy exits non-zero on a genuine compile ERROR in the TU (not on
    # a plain warning), which is the one failure mode worth surfacing loudly
    # -- see gcc lane's identical rc convention.
    return (src, dt, r.returncode, nwarn, cfg_found)

t_start = time.time()
out = []
with ThreadPoolExecutor(max_workers=max(jobs, 1)) as ex:
    for res in ex.map(run_one, entries):
        out.append(res)
wall = time.time() - t_start

fails = 0
total_warn = 0
any_cfg = False
for src, dt, rc, nwarn, cfg_found in out:
    print(f"TIDY_FILE {src} rc={rc} warnings={nwarn}")
    if rc != 0:
        fails += 1
    total_warn += nwarn
    any_cfg = any_cfg or cfg_found

print(f"TIDY_SUMMARY entries={len(entries)} fails={fails} warnings={total_warn} wall={wall:.2f} export_fixes={1 if export_fixes_ok else 0} config_found={1 if any_cfg else 0}")
PYEOF
}

# --self-test: the positive control (a null dereference, exactly one gcc
# result whose rule id names it), the zero-results negative control (an
# empty function, rc=0 AND count=0), a THIRD case that a lane FAILURE
# (a file that does not compile, rc != 0) is never confused with a clean
# zero-results run, clang-tidy's own zero-warnings negative control on the
# same empty function, and the path-keying fix itself: two files that
# share a basename in two different directories must not clobber each
# other's SARIF.
if [ "${1:-}" = "--self-test" ]; then
	SELF="$PROBE --self-test"
	W="$(mktemp -d)"
	trap 'rm -rf "$W"' EXIT
	mkdir -p "$W/a" "$W/b"

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
	cat >"$W/broken.c" <<'EOF'
int broken(void) {
	int x = ;
}
EOF
	# Same basename ("same.c"), two different directories, two different
	# bodies (one null-deref, one empty) -- an old basename-only key would
	# have one entry's SARIF silently overwrite the other's, with neither
	# compile failing, so nothing would signal it. Different bodies make
	# the two expected counts (1 and 0) distinguishable proof that the
	# right file produced the right SARIF, not just that two files exist.
	cat >"$W/a/same.c" <<'EOF'
int same(void) {
	int *p = 0;
	return *p;
}
EOF
	cat >"$W/b/same.c" <<'EOF'
void same(void) {
}
EOF
	cat >"$W/cc.json" <<EOF
[
  {"directory": "$W", "file": "bug.c", "arguments": ["gcc", "-c", "bug.c", "-o", "bug.o"]},
  {"directory": "$W", "file": "ctrl.c", "arguments": ["gcc", "-c", "ctrl.c", "-o", "ctrl.o"]},
  {"directory": "$W", "file": "broken.c", "arguments": ["gcc", "-c", "broken.c", "-o", "broken.o"]},
  {"directory": "$W", "file": "a/same.c", "arguments": ["gcc", "-c", "a/same.c", "-o", "a/same.o"]},
  {"directory": "$W", "file": "b/same.c", "arguments": ["gcc", "-c", "b/same.c", "-o", "b/same.o"]}
]
EOF

	fail=0

	gcc_out="$(run_gcc_lane "$W/cc.json" "$W/sarif-gcc" "$W/objs-gcc" 3)"
	gcc_summary="$(printf '%s\n' "$gcc_out" | grep '^GCC_SUMMARY ')"
	bug_line="$(printf '%s\n' "$gcc_out" | grep '^GCC_FILE bug.c ')"
	ctrl_line="$(printf '%s\n' "$gcc_out" | grep '^GCC_FILE ctrl.c ')"
	broken_line="$(printf '%s\n' "$gcc_out" | grep '^GCC_FILE broken.c ')"
	a_line="$(printf '%s\n' "$gcc_out" | grep '^GCC_FILE a/same.c ')"
	b_line="$(printf '%s\n' "$gcc_out" | grep '^GCC_FILE b/same.c ')"

	if printf '%s\n' "$bug_line" | grep -q 'rc=0 ' && printf '%s\n' "$bug_line" | grep -q 'count=1 ' && printf '%s\n' "$bug_line" | grep -q 'null-dereference'; then
		echo "    ok: positive control (null dereference) gives exactly one gcc result, ruleId names it, rc=0"
		echo "        $bug_line"
	else
		echo "$SELF: FAIL (positive control did not give rc=0 and one null-dereference result): $bug_line"
		fail=1
	fi

	# I2: rc=0 AND count=0, not just count=0 -- the "no sarif produced"
	# failure path also prints count=0, with rc=2, so count=0 alone cannot
	# tell a clean negative control apart from a broken lane.
	if printf '%s\n' "$ctrl_line" | grep -q 'rc=0 ' && printf '%s\n' "$ctrl_line" | grep -q 'count=0 '; then
		echo "    ok: negative control (empty function) gives rc=0 AND zero gcc results"
		echo "        $ctrl_line"
	else
		echo "$SELF: FAIL (negative control was not rc=0 with zero gcc results): $ctrl_line"
		fail=1
	fi

	# I2, continued: a file that cannot compile must be a LANE FAILURE
	# (non-zero rc, counted in GCC_SUMMARY's fails=), never reported as a
	# clean "zero results" run -- the two must stay distinguishable.
	if printf '%s\n' "$broken_line" | grep -qE 'rc=[1-9][0-9]*' && ! printf '%s\n' "$broken_line" | grep -q 'rc=0 '; then
		echo "    ok: a file that does not compile is reported with a non-zero rc, not as zero results"
		echo "        $broken_line"
	else
		echo "$SELF: FAIL (a broken file did not get a non-zero rc): $broken_line"
		fail=1
	fi
	if printf '%s\n' "$gcc_summary" | grep -qE 'fails=[1-9]'; then
		echo "    ok: GCC_SUMMARY counts the broken file as a lane failure"
		echo "        $gcc_summary"
	else
		echo "$SELF: FAIL (GCC_SUMMARY did not count the broken file as a failure): $gcc_summary"
		fail=1
	fi

	# I1: same basename ("same.c"), two directories -- both must succeed,
	# and each must carry ITS OWN count (1 for a/, 0 for b/), not a count
	# borrowed from whichever entry a race let write last.
	if printf '%s\n' "$a_line" | grep -q 'rc=0 ' && printf '%s\n' "$a_line" | grep -q 'count=1 '; then
		echo "    ok: a/same.c (null deref) gives rc=0, count=1"
		echo "        $a_line"
	else
		echo "$SELF: FAIL (a/same.c did not give rc=0, count=1): $a_line"
		fail=1
	fi
	if printf '%s\n' "$b_line" | grep -q 'rc=0 ' && printf '%s\n' "$b_line" | grep -q 'count=0 '; then
		echo "    ok: b/same.c (empty function) gives rc=0, count=0"
		echo "        $b_line"
	else
		echo "$SELF: FAIL (b/same.c did not give rc=0, count=0): $b_line"
		fail=1
	fi

	# The proof is on DISK, not just in the printed line: two distinct
	# SARIF files, keyed by path, each independently holding its own
	# entry's count -- the shape of bug this fix corrects is exactly a
	# printed line looking right while one file silently overwrote the
	# other on disk.
	a_sarif="$W/sarif-gcc/a__same.c.sarif"
	b_sarif="$W/sarif-gcc/b__same.c.sarif"
	if [ -f "$a_sarif" ] && [ -f "$b_sarif" ]; then
		a_count="$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(sum(len(r.get('results',[])) for r in d.get('runs',[])))" "$a_sarif")"
		b_count="$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(sum(len(r.get('results',[])) for r in d.get('runs',[])))" "$b_sarif")"
		if [ "$a_count" = "1" ] && [ "$b_count" = "0" ]; then
			echo "    ok: two distinct SARIF files on disk, $a_sarif (1 result) and $b_sarif (0 results)"
		else
			echo "$SELF: FAIL (on-disk counts wrong: $a_sarif=$a_count, $b_sarif=$b_count, want 1 and 0)"
			fail=1
		fi
	else
		echo "$SELF: FAIL (expected two distinct SARIF files, missing one): $a_sarif $b_sarif"
		fail=1
	fi

	tidy_out="$(run_tidy_lane "$W/cc.json" "$W/tidy" 3 "clang-diagnostic-*,clang-analyzer-*" "$W")"
	tidy_bug_line="$(printf '%s\n' "$tidy_out" | grep '^TIDY_FILE bug.c ')"
	tidy_ctrl_line="$(printf '%s\n' "$tidy_out" | grep '^TIDY_FILE ctrl.c ')"
	if printf '%s\n' "$tidy_bug_line" | grep -qE 'warnings=[1-9][0-9]*'; then
		echo "    ok: clang-tidy lane finds at least one warning on the same positive control"
		echo "        $tidy_bug_line"
	else
		echo "$SELF: FAIL (clang-tidy lane found no warning on the positive control): $tidy_bug_line"
		fail=1
	fi
	# M3: clang-tidy's own negative control -- the empty function must give
	# zero warnings, not merely "gcc found zero" carried over by assumption.
	if printf '%s\n' "$tidy_ctrl_line" | grep -q 'warnings=0'; then
		echo "    ok: clang-tidy lane gives zero warnings on the empty-function negative control"
		echo "        $tidy_ctrl_line"
	else
		echo "$SELF: FAIL (clang-tidy lane gave a nonzero warning count on the negative control): $tidy_ctrl_line"
		fail=1
	fi

	# cppcheck positive control (task 17): the SAME bug.c fixture used above
	# (a null dereference, not `static`, no prototype) is also a MISRA
	# violation -- misra-c2012-8.4, "a function with external linkage needs
	# exactly one visible declaration" -- among cppcheck's own findings
	# (nullPointer, constVariablePointer). One fixture, three lanes.
	cppcheck_out="$(run_cppcheck_lane "$W/cc.json" "$W/sarif-cppcheck" 3 "$MISRA_JSON" "$MISRA_SUPPRESSIONS")"
	cppcheck_summary="$(printf '%s\n' "$cppcheck_out" | grep '^CPPCHECK_SUMMARY ')"
	bug_misra_hit="$(python3 -c "
import json
d = json.load(open('$W/sarif-cppcheck/cppcheck.sarif'))
for run in d.get('runs', []):
    for r in run.get('results', []):
        rid = r.get('ruleId', '')
        for loc in r.get('locations', []):
            uri = loc.get('physicalLocation', {}).get('artifactLocation', {}).get('uri', '')
            if uri.endswith('/bug.c') and rid.startswith('misra-c2012-'):
                print(rid)
" 2>/dev/null | head -1)"
	if [ -n "$bug_misra_hit" ] && printf '%s\n' "$cppcheck_summary" | grep -qE 'exit=0 '; then
		echo "    ok: cppcheck lane finds a MISRA violation ($bug_misra_hit) on the same positive control"
		echo "        $cppcheck_summary"
	else
		echo "$SELF: FAIL (cppcheck lane found no misra-c2012-* result on bug.c): $cppcheck_summary"
		fail=1
	fi

	# Task 17b: the verdict itself. GREEN on bug.c and ctrl.c, which have no
	# parse failure, so the RED cases below prove the verdict can tell the
	# difference, not only that it can say no.
	cat >"$W/cc-clean.json" <<EOF
[
  {"directory": "$W", "file": "bug.c", "arguments": ["gcc", "-c", "bug.c", "-o", "bug.o"]},
  {"directory": "$W", "file": "ctrl.c", "arguments": ["gcc", "-c", "ctrl.c", "-o", "ctrl.o"]}
]
EOF
	clean_out="$(run_cppcheck_lane "$W/cc-clean.json" "$W/sarif-cppcheck-clean" 1 "$MISRA_JSON" "$MISRA_SUPPRESSIONS")"
	clean_summary="$(printf '%s\n' "$clean_out" | grep '^CPPCHECK_SUMMARY ')"
	if cppcheck_lane_verdict "$clean_summary"; then
		echo "    ok: the cppcheck verdict is GREEN on bug.c and ctrl.c, which have no parse failure"
		echo "        $clean_summary"
	else
		echo "$SELF: FAIL (the cppcheck verdict is RED on fixtures with no parse failure): $clean_summary"
		printf '%s\n' "$clean_out" | grep -v '^CPPCHECK_SUMMARY ' || true
		fail=1
	fi

	# Task 17b fix round 1: a syntaxError is a parse failure too. The first
	# fixture set holds broken.c, which does not parse, so its run above must
	# be RED and must name broken.c.
	if ! cppcheck_lane_verdict "$cppcheck_summary" \
		&& printf '%s\n' "$cppcheck_summary" | grep -q ' exit=0 ' \
		&& printf '%s\n' "$cppcheck_out" | grep -qE '^CPPCHECK_PARSE_FAILURE [^ ]*broken\.c:[0-9]+ syntaxError '; then
		echo "    ok: a unit with a syntaxError is RED, although cppcheck exits 0"
		printf '%s\n' "$cppcheck_out" | grep '^CPPCHECK_PARSE_FAILURE ' | sed 's/^/        /'
	else
		echo "$SELF: FAIL (a syntaxError result did not make the cppcheck lane RED): $cppcheck_summary"
		fail=1
	fi

	# Task 17b: a unit that stops at an #error in a header it includes, the
	# shape of the llvm-c/DataTypes.h defect. cppcheck still prints
	# "Checking pp.c" and exits 0, so only the parse-failure check can make
	# it RED.
	mkdir -p "$W/pp"
	cat >"$W/pp/stop.h" <<'EOF'
#error "stop: cppcheck analyses nothing after this line"
EOF
	cat >"$W/pp/pp.c" <<'EOF'
#include "stop.h"
int pp(void) {
	int *p = 0;
	return *p;
}
EOF
	cat >"$W/cc-pp.json" <<EOF
[
  {"directory": "$W/pp", "file": "pp.c", "arguments": ["gcc", "-c", "pp.c", "-o", "pp.o"]}
]
EOF
	pp_out="$(run_cppcheck_lane "$W/cc-pp.json" "$W/sarif-cppcheck-pp" 1 "$MISRA_JSON" "$MISRA_SUPPRESSIONS")"
	pp_summary="$(printf '%s\n' "$pp_out" | grep '^CPPCHECK_SUMMARY ')"
	if ! cppcheck_lane_verdict "$pp_summary" \
		&& printf '%s\n' "$pp_summary" | grep -q ' exit=0 ' \
		&& printf '%s\n' "$pp_summary" | grep -qE ' checked=[1-9]' \
		&& printf '%s\n' "$pp_summary" | grep -q ' parse_failures=1 '; then
		echo "    ok: a unit that stops at an #error is RED, although cppcheck exits 0 and prints 'Checking' for it"
		echo "        $pp_summary"
	else
		echo "$SELF: FAIL (a unit that stops at an #error did not make the cppcheck lane RED): $pp_summary"
		fail=1
	fi

	# Task 17b: the same unit with a list that hides the #error. cppcheck then
	# reports nothing for pp.c, so the list itself must make the lane RED.
	# Two entries, one run each: the exact ID (the old
	# docs/misra/misra-suppressions.txt entry), and a GLOB that covers it
	# (fix round 1: a prefix grep for the ID let "preprocessor*" through).
	for blind_entry in 'preprocessorErrorDirective:*/stop.h' 'preprocessor*:*/stop.h'; do
		cp "$MISRA_SUPPRESSIONS" "$W/blind-suppressions.txt"
		printf '%s\n' "$blind_entry" >>"$W/blind-suppressions.txt"
		blind_out="$(run_cppcheck_lane "$W/cc-pp.json" "$W/sarif-cppcheck-blind" 1 "$MISRA_JSON" "$W/blind-suppressions.txt")"
		blind_summary="$(printf '%s\n' "$blind_out" | grep '^CPPCHECK_SUMMARY ')"
		if ! cppcheck_lane_verdict "$blind_summary" \
			&& printf '%s\n' "$blind_summary" | grep -q ' parse_failures=0 ' \
			&& printf '%s\n' "$blind_summary" | grep -qE ' blind_suppressions=1$'; then
			echo "    ok: a list entry '$blind_entry' that hides an #error is RED, although the hidden unit reports nothing"
			echo "        $blind_summary"
		else
			echo "$SELF: FAIL (a list entry '$blind_entry' that hides an #error did not make the cppcheck lane RED): $blind_summary"
			fail=1
		fi
	done

	# Task 17b fix round 1: an INLINE suppression of the #error (scan.sh passes
	# --inline-suppr). The list is the real one, which hides nothing.
	mkdir -p "$W/inl"
	cat >"$W/inl/stop.h" <<'EOF'
// cppcheck-suppress preprocessorErrorDirective
#error "stop: cppcheck analyses nothing after this line"
EOF
	cp "$W/pp/pp.c" "$W/inl/inl.c"
	cat >"$W/cc-inl.json" <<EOF
[
  {"directory": "$W/inl", "file": "inl.c", "arguments": ["gcc", "-c", "inl.c", "-o", "inl.o"]}
]
EOF
	inl_out="$(run_cppcheck_lane "$W/cc-inl.json" "$W/sarif-cppcheck-inl" 1 "$MISRA_JSON" "$MISRA_SUPPRESSIONS")"
	inl_summary="$(printf '%s\n' "$inl_out" | grep '^CPPCHECK_SUMMARY ')"
	if ! cppcheck_lane_verdict "$inl_summary" \
		&& printf '%s\n' "$inl_summary" | grep -q ' parse_failures=0 ' \
		&& printf '%s\n' "$inl_summary" | grep -qE ' blind_suppressions=1$' \
		&& printf '%s\n' "$inl_out" | grep -q '^CPPCHECK_BLIND_SUPPRESSION .*stop\.h:1 '; then
		echo "    ok: an inline cppcheck-suppress comment that hides an #error is RED"
		echo "        $inl_summary"
	else
		echo "$SELF: FAIL (an inline suppression of an #error did not make the cppcheck lane RED): $inl_summary"
		printf '%s\n' "$inl_out" | grep -v '^CPPCHECK_SUMMARY ' || true
		fail=1
	fi

	# The real list holds the three Bison all-ID entries. They must be
	# reported as known exceptions, never as blind entries, and nothing else
	# in the real list may count as blind ($clean_summary above already
	# requires blind_suppressions=0).
	known_n="$(printf '%s\n' "$clean_out" | grep -c '^CPPCHECK_KNOWN_EXCEPTION ' || true)"
	if [ "$known_n" = "3" ]; then
		echo "    ok: the three Bison all-ID entries are reported as known exceptions"
	else
		echo "$SELF: FAIL (expected 3 known Bison exceptions in the real list, got $known_n)"
		fail=1
	fi

	if [ "$fail" -ne 0 ]; then
		exit 1
	fi
	echo "$SELF: PASS (gcc: 1/0/failure distinguished, path-keyed same-basename pair correct; clang-tidy: >=1 warning / 0 warnings; cppcheck: >=1 misra-c2012-* result; a syntaxError unit, an #error unit, an exact and a glob list entry and an inline comment that hide one are RED)"
	exit 0
fi

# --cppcheck-lane <compile_commands.json> <out dir> <jobs>: the cppcheck lane
# alone, on a database the caller chose, through the SAME run_cppcheck_lane
# the real run uses. Task 17b: scripts/findings_probe.sh's repeatability
# control runs it twice on the runtime units with the database order
# reversed, so the control exercises the lane, not a copy of its flags.
if [ "${1:-}" = "--cppcheck-lane" ]; then
	if [ "$#" -ne 4 ]; then
		echo "usage: $0 --cppcheck-lane <compile_commands.json> <out dir> <jobs>" >&2
		exit 2
	fi
	lane_out="$(run_cppcheck_lane "$2" "$3" "$4" "$MISRA_JSON" "$MISRA_SUPPRESSIONS")"
	printf '%s\n' "$lane_out"
	lane_summary="$(printf '%s\n' "$lane_out" | grep '^CPPCHECK_SUMMARY ')"
	if ! cppcheck_lane_verdict "$lane_summary"; then
		echo "$PROBE --cppcheck-lane: FAIL ($2: $lane_summary)" >&2
		exit 1
	fi
	exit 0
fi

if [ -n "${1:-}" ]; then
	echo "usage: $0 [--self-test | --cppcheck-lane <compile_commands.json> <out dir> <jobs>]" >&2
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

rm -rf "$SARIF_CPPCHECK_DIR"
mkdir -p "$SARIF_CPPCHECK_DIR"

# M1: the ".clang-tidy discovered anywhere?" decision is now made PER FILE,
# inside run_tidy_lane's own directory walk (see that function and this
# script's header). $CLANG_TIDY_CHECKS is always passed as the fallback;
# it is only actually used for a file whose own walk finds nothing.
gcc_out="$(run_gcc_lane "$COMPILE_COMMANDS_OUT" "$SARIF_GCC_DIR" "$gcc_objs_dir" "$JOBS")"
gcc_summary="$(printf '%s\n' "$gcc_out" | grep '^GCC_SUMMARY ')"
gcc_fails="$(printf '%s\n' "$gcc_summary" | sed -n 's/.*fails=\([0-9]*\).*/\1/p')"
gcc_results="$(printf '%s\n' "$gcc_summary" | sed -n 's/.*results=\([0-9]*\).*/\1/p')"
gcc_wall="$(printf '%s\n' "$gcc_summary" | sed -n 's/.*wall=\([0-9.]*\).*/\1/p')"

tidy_out="$(run_tidy_lane "$COMPILE_COMMANDS_OUT" "$SARIF_TIDY_DIR" "$JOBS" "$CLANG_TIDY_CHECKS" "$REPO_ROOT")"
tidy_summary="$(printf '%s\n' "$tidy_out" | grep '^TIDY_SUMMARY ')"
tidy_fails="$(printf '%s\n' "$tidy_summary" | sed -n 's/.*fails=\([0-9]*\).*/\1/p')"
tidy_warnings="$(printf '%s\n' "$tidy_summary" | sed -n 's/.*warnings=\([0-9]*\).*/\1/p')"
tidy_wall="$(printf '%s\n' "$tidy_summary" | sed -n 's/.*wall=\([0-9.]*\).*/\1/p')"
tidy_export_fixes="$(printf '%s\n' "$tidy_summary" | sed -n 's/.*export_fixes=\([0-9]\).*/\1/p')"
tidy_config_found="$(printf '%s\n' "$tidy_summary" | sed -n 's/.*config_found=\([0-9]\).*/\1/p')"

cppcheck_out="$(run_cppcheck_lane "$COMPILE_COMMANDS_OUT" "$SARIF_CPPCHECK_DIR" "$JOBS" "$MISRA_JSON" "$MISRA_SUPPRESSIONS")"
cppcheck_summary="$(printf '%s\n' "$cppcheck_out" | grep '^CPPCHECK_SUMMARY ')"
cppcheck_checked="$(printf '%s\n' "$cppcheck_summary" | sed -n 's/.*checked=\([0-9]*\).*/\1/p')"
cppcheck_exit="$(printf '%s\n' "$cppcheck_summary" | sed -n 's/.*exit=\([a-z0-9]*\).*/\1/p')"
cppcheck_results="$(printf '%s\n' "$cppcheck_summary" | sed -n 's/.*results=\([0-9]*\).*/\1/p')"
cppcheck_wall="$(printf '%s\n' "$cppcheck_summary" | sed -n 's/.*wall=\([0-9.]*\).*/\1/p')"
# The gate is "did the scan actually run", never "how many findings" -- a
# high MISRA count is expected (see docs/misra/README.md's own accepted
# baseline) and is task 17's findings-probe's business, not this lane's.
# Both checks mirror docs/misra/tools/gate.py's tally(): a nonzero exit or
# zero "Checking " lines means the scan cannot be trusted, and a comparison
# against nothing always passes silently unless this refuses it first.
# NOT compared against entry_count: cppcheck prints a SECOND "Checking
# <file>: <defines>..." line for a file with more than one preprocessor
# configuration, so the raw line count legitimately exceeds the entry count
# (VERIFIED: 100 lines for 58 entries on this tree) -- gate.py's own
# "checked == 0" refusal is the proven bar, not equality with entry_count.
# Task 17b adds the #error check to the same verdict: see
# cppcheck_lane_verdict and the "A UNIT THAT STOPS AT #error" note above.
cppcheck_fails=0
if ! cppcheck_lane_verdict "$cppcheck_summary"; then
	cppcheck_fails=1
	printf '%s\n' "$cppcheck_out" | grep -E '^CPPCHECK_(PARSE_FAILURE|BLIND_SUPPRESSION) ' >&2 || true
fi

echo "$PROBE: capture $COMPILE_COMMANDS_OUT, $entry_count entries"
# The compiler the lane RAN is the first non-ccache token of the capture's
# own arguments (under `make CC=gcc-14` that is gcc-14), not the gcc on PATH.
# The merged SARIF's driver version, which feeds the toolchain signature,
# comes from that same compiler.
GCC_VERSION="$(python3 - "$COMPILE_COMMANDS_OUT" <<'PYEOF'
import json, subprocess, sys
args = json.load(open(sys.argv[1]))[0]["arguments"]
cc = next(a for a in args if not a.endswith("ccache"))
print(cc + ": " + subprocess.run([cc, "--version"], capture_output=True, text=True).stdout.splitlines()[0])
PYEOF
)"
echo "$PROBE: gcc lane  -- $GCC_VERSION"
echo "$PROBE: gcc lane  -- $gcc_fails/$entry_count compile failures, $gcc_results analyser results, ${gcc_wall}s wall, SARIF in $SARIF_GCC_DIR"
echo "$PROBE: clang-tidy lane -- $CLANG_TIDY_VERSION, a .clang-tidy found on any file's directory walk: $([ "$tidy_config_found" = "1" ] && echo yes || echo no)"
echo "$PROBE: clang-tidy lane -- $tidy_fails/$entry_count compile failures, $tidy_warnings warnings, ${tidy_wall}s wall, export-fixes YAML: $([ "$tidy_export_fixes" = "1" ] && echo yes || echo no), output in $SARIF_TIDY_DIR"
echo "$PROBE: cppcheck lane -- $CPPCHECK_VERSION, addon config $MISRA_JSON"
# $cppcheck_checked can exceed $entry_count -- see run_cppcheck_lane's own
# comment: a file with more than one preprocessor configuration gets a
# SECOND "Checking <file>: <defines>..." line, so this counts lines, not
# distinct files.
cppcheck_pp="$(printf '%s\n' "$cppcheck_summary" | sed -n 's/.*parse_failures=\([0-9?]*\).*/\1/p')"
cppcheck_blind="$(printf '%s\n' "$cppcheck_summary" | sed -n 's/.*blind_suppressions=\([0-9?]*\).*/\1/p')"
echo "$PROBE: cppcheck lane -- $entry_count entries, $cppcheck_checked 'Checking' lines, exit=$cppcheck_exit, $cppcheck_results findings, ${cppcheck_pp:-?} parse failure(s), ${cppcheck_blind:-?} suppression(s) hiding one, ${cppcheck_wall}s wall, SARIF in $SARIF_CPPCHECK_DIR"

# M2: the terminal verdict line carries the artefact -- the capture path
# and all three SARIF directories -- the way
# scripts/compile_commands_current_probe.sh's own PASS line names the
# capture path and entry count.
if [ "${gcc_fails:-1}" != "0" ] || [ "${tidy_fails:-1}" != "0" ] || [ "$cppcheck_fails" != "0" ]; then
	echo "$PROBE: FAIL ($COMPILE_COMMANDS_OUT: $entry_count entries; gcc $gcc_fails failure(s) in $SARIF_GCC_DIR; clang-tidy $tidy_fails failure(s) in $SARIF_TIDY_DIR; cppcheck exit=$cppcheck_exit checked=$cppcheck_checked/$entry_count parse_failures=${cppcheck_pp:-?} blind_suppressions=${cppcheck_blind:-?} in $SARIF_CPPCHECK_DIR)"
	exit 1
fi

echo "$PROBE: PASS ($COMPILE_COMMANDS_OUT: $entry_count entries; gcc $gcc_results results in $SARIF_GCC_DIR; clang-tidy $tidy_warnings warnings in $SARIF_TIDY_DIR; cppcheck $cppcheck_results findings in $SARIF_CPPCHECK_DIR)"
