#!/usr/bin/env bash
# findings_probe.sh -- task 17 of the 2026-09-02 verification tooling plan:
# one SARIF findings database with a shrink-only gate, generalising
# docs/misra/tools/gate.py (one tool's (file, rule) tally) across every
# analyser and probe verdict this repository already produces.
#
# PIPELINE, every stage regenerated unconditionally (no staleness check --
# see scripts/analyzer_lanes_probe.sh's own header for why that policy is
# deliberate here: a fresh run costs under two minutes and is always
# correct, where a staleness check is one more thing to get wrong):
#
#   1. scripts/analyzer_lanes_probe.sh   -- gcc + clang-tidy + cppcheck,
#      writes build/sarif/{gcc,clang-tidy,cppcheck}/. Its own stdout is
#      captured into a gate log (below).
#   2. scripts/pthread_return_probe.sh   -- the pthread ratchet's live
#      verdict, appended to the SAME gate log.
#   3. tools/clang_tidy_to_sarif.py      -- clang-tidy has no native SARIF
#      emitter; converts its .txt lane output into one SARIF run.
#   4. tools/probe_verdicts_to_sarif.py  -- twice: once over
#      scripts/pthread-return-baseline.txt (the ratchet's own RECORDED,
#      accepted debt -- not step 2's live count again, which would double-
#      gate the same signal against two shrink-only baselines), once over
#      the gate log from steps 1-2 (a FAIL line from either becomes a
#      goo/gate-failed finding here, rather than aborting this script --
#      the merged file should still reflect everything else that ran).
#   5. tools/sarif_merge.py --validate  -- one build/findings.sarif, schema-
#      checked against the vendored docs/misra/tools/sarif-schema-2.1.0.json.
#   5b. tools/findings_gate.py annotate -- task 18: a result that a row of
#      docs/misra/findings-suppressions.tsv matches gains the SARIF
#      "suppressions" entry (kind external, justification D-NN), so the
#      GitHub Security tab shows it as suppressed, not removed. The file is
#      then validated again, and tools/sarif_merge.py --check-limits checks
#      the GitHub upload limits (20 runs, 25,000 results per run, 10 MB
#      gzipped).
#   6. tools/findings_gate.py check     -- the shrink-only comparison
#      against scripts/findings-baseline.<signature>.tsv, filtered through
#      docs/misra/findings-suppressions.tsv. The signature
#      (gcc<major>-tidy<major>-cppcheck<major>.<minor>) is read from the
#      merged SARIF's own driver versions; a toolchain with no baseline file
#      fails loudly. See that module's own docstring for the count-per-
#      (tool, rule, file) comparison and the suppression policy (every row
#      needs an EXISTING docs/misra/deviations/D-NN.md record or a
#      documented checker defect, generalising docs/misra/
#      misra-suppressions.txt's own rule across every tool).
#
# UNDER `make verify-core`, this means the three analyser lanes run TWICE --
# once as analyzer-lanes-probe's own VERIFY_ALL_DEPS entry, once again as
# step 1 above, since this script must also work standalone (`bash
# scripts/findings_probe.sh`, no Makefile dependency graph to rely on) and
# a staleness check across two independently-invocable gates is exactly the
# fragile kind this repository's probes already choose not to build. An
# accepted ~70s against verify-core's many minutes.
#
# Usage:
#   scripts/findings_probe.sh                    # gate (exit 1 on red)
#   scripts/findings_probe.sh --update-baseline   # accept current findings
#   scripts/findings_probe.sh --check-only        # steps 5-6 only, against
#                                                  # the CURRENT $FINDINGS_SARIF
#                                                  # (no pipeline re-run) -- what
#                                                  # --self-test drives against
#                                                  # synthetic fixtures
#   scripts/findings_probe.sh --self-test         # six synthetic cases and one
#                                                  # cppcheck repeatability control,
#                                                  # see below; never runs the real
#                                                  # pipeline
#
# Environment overrides:
#   FINDINGS_SARIF         merged output (default: build/findings.sarif)
#   FINDINGS_BASELINE      shrink-only baseline PREFIX; the file read is
#                          <prefix>.<toolchain signature>.tsv (default prefix:
#                          scripts/findings-baseline)
#   FINDINGS_SUPPRESSIONS  suppression list (default: docs/misra/findings-suppressions.tsv)
#   SARIF_GCC_DIR / SARIF_TIDY_DIR / SARIF_CPPCHECK_DIR  same meaning and
#                          defaults as scripts/analyzer_lanes_probe.sh; passed
#                          through to it so both scripts agree on paths even
#                          when overridden.
set -euo pipefail

PROBE="findings-probe"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

FINDINGS_SARIF="${FINDINGS_SARIF:-$REPO_ROOT/build/findings.sarif}"
FINDINGS_BASELINE="${FINDINGS_BASELINE:-$REPO_ROOT/scripts/findings-baseline}"
FINDINGS_SUPPRESSIONS="${FINDINGS_SUPPRESSIONS:-$REPO_ROOT/docs/misra/findings-suppressions.tsv}"
export SARIF_GCC_DIR="${SARIF_GCC_DIR:-$REPO_ROOT/build/sarif/gcc}"
export SARIF_TIDY_DIR="${SARIF_TIDY_DIR:-$REPO_ROOT/build/sarif/clang-tidy}"
export SARIF_CPPCHECK_DIR="${SARIF_CPPCHECK_DIR:-$REPO_ROOT/build/sarif/cppcheck}"
SARIF_PROBES_DIR="${SARIF_PROBES_DIR:-$REPO_ROOT/build/sarif/probes}"
PTHREAD_BASELINE="${PTHREAD_BASELINE:-$REPO_ROOT/scripts/pthread-return-baseline.txt}"
GATE_LOG="${GATE_LOG:-$REPO_ROOT/build/sarif/probes/gate-log.txt}"

if ! command -v python3 >/dev/null 2>&1; then
	echo "$PROBE: FAIL (python3 is required)" >&2
	exit 2
fi

# --check-only: validate + gate the CURRENT $FINDINGS_SARIF as it already
# sits on disk, no pipeline re-run. Used by --self-test (below) so each
# synthetic case runs in milliseconds rather than the ~70s full pipeline,
# and reusable directly by anyone who already has a fresh merged file from
# a previous run.
run_check_only() { # $1 = sarif path
	local sarif="$1"
	if ! python3 "$REPO_ROOT/tools/sarif_merge.py" --validate-only "$sarif"; then
		return 1
	fi
	# Task 18: the GitHub upload limits, checked here so a rejection never
	# appears first on GitHub.
	if ! python3 "$REPO_ROOT/tools/sarif_merge.py" --check-limits "$sarif"; then
		return 1
	fi
	python3 "$REPO_ROOT/tools/findings_gate.py" check "$sarif" "$FINDINGS_BASELINE" "$FINDINGS_SUPPRESSIONS"
}

if [ "${1:-}" = "--check-only" ]; then
	run_check_only "$FINDINGS_SARIF"
	exit $?
fi

# ---------------------------------------------------------------------------
# --self-test: six SYNTHETIC cases and one control on the real runtime units;
# the real pipeline is NOT run (fix round 1 / M1: it used to fall through
# into the pipeline as case (d), so verify-core ran every lane three times).
#   (a) a synthetic SARIF with one result not in the baseline -> RED
#   (b) a suppression row with an empty justification -> RED
#   (c) a merged file that fails schema validation (dropped "version") -> RED
#   (d) positive control: the same SARIF against a matching baseline -> GREEN
#       (without it, (a)-(c) prove only that the gate can say no).
#   (e) a SARIF with 21 runs (the GitHub limit is 20) -> RED, task 18.
#   (f) a suppressed result is annotated, still schema-valid, and still
#       present in the file, task 18.
#   (g) the cppcheck lane on the runtime units, at -j1, forward and with the
#       database reversed: any (rule, file) count that differs -> RED, task
#       17b. About 7s; the only case that runs a real analyser.
# The real merged file is gated by `make findings-probe`, not here.
# Each drives run_check_only end to end (validate, THEN gate), through THIS
# script's own entry point -- not tools/findings_gate.py directly, which
# has its own, separate --self-test for the module's logic in isolation.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
	SELF="$PROBE --self-test"
	W="$(mktemp -d)"
	trap 'rm -rf "$W"' EXIT
	fail=0
	SIG="gcc1-tidy1-cppcheck1.0" # the signature write_sarif's driver versions yield

	write_sarif() { # $1 = out path, $2 = ruleId, $3 = file, $4 = fingerprint
		python3 -c "
import json, sys
out, rule, file_, fp = sys.argv[1:5]
doc = {'version': '2.1.0', '\$schema': 'x', 'runs': [
    {'tool': {'driver': {'name': 'GNU C23', 'version': '1.0'}},
     'results': [{'ruleId': rule, 'message': {'text': 'x'},
                  'locations': [{'physicalLocation': {'artifactLocation': {'uri': file_}}}],
                  'fingerprints': {'goo/v1': fp}}]},
    {'tool': {'driver': {'name': 'clang-tidy', 'version': '1.0'}}, 'results': []},
    {'tool': {'driver': {'name': 'Cppcheck', 'semanticVersion': '1.0.0'}}, 'results': []},
]}
json.dump(doc, open(out, 'w'))
" "$1" "$2" "$3" "$4"
	}

	# (a) one result, empty baseline, empty suppressions -> RED. The env
	# prefix is INSIDE the command substitution, scoped to run_check_only's
	# own subshell.
	write_sarif "$W/a.sarif" "r1" "a.c" "fp1"
	: >"$W/empty.$SIG.tsv"
	: >"$W/empty-suppressions.tsv"
	out_a="$(FINDINGS_BASELINE="$W/empty" FINDINGS_SUPPRESSIONS="$W/empty-suppressions.tsv" \
		run_check_only "$W/a.sarif" 2>&1)" && rc_a=0 || rc_a=$?
	if [ "${rc_a:-0}" -ne 0 ]; then
		echo "    ok: a finding not in the baseline is RED"
	else
		echo "$SELF: FAIL (a new finding stayed GREEN): $out_a"
		fail=1
	fi

	# (b) the SAME finding, now suppressed by a row with an EMPTY
	# justification -- must be RED for the suppression row itself.
	printf 'GNU C23\tr1\ta.c\t\n' >"$W/bad-suppressions.tsv"
	out_b="$(FINDINGS_BASELINE="$W/empty" FINDINGS_SUPPRESSIONS="$W/bad-suppressions.tsv" \
		run_check_only "$W/a.sarif" 2>&1)" && rc_b=0 || rc_b=$?
	if [ "${rc_b:-0}" -ne 0 ] && printf '%s\n' "$out_b" | grep -q "no valid justification"; then
		echo "    ok: a suppression row with an empty justification is RED"
	else
		echo "$SELF: FAIL (an empty-justification suppression row did not go RED): $out_b"
		fail=1
	fi

	# (c) a merged file missing the required top-level "version" field ->
	# RED at the SCHEMA VALIDATION step, before the gate comparison ever
	# runs (so this fails even against a baseline that would otherwise be green).
	python3 -c "
import json
doc = json.load(open('$W/a.sarif'))
del doc['version']
json.dump(doc, open('$W/c.sarif', 'w'))
"
	# tool, rule, file, startLine, startColumn, message, fingerprint: the
	# synthetic result has no region, so both location fields are empty.
	printf 'GNU C23\tr1\ta.c\t\t\tx\tfp1\n' >"$W/matching.$SIG.tsv"
	out_c="$(FINDINGS_BASELINE="$W/matching" FINDINGS_SUPPRESSIONS="$W/empty-suppressions.tsv" \
		run_check_only "$W/c.sarif" 2>&1)" && rc_c=0 || rc_c=$?
	if [ "${rc_c:-0}" -ne 0 ] && printf '%s\n' "$out_c" | grep -q "SCHEMA INVALID"; then
		echo "    ok: a merged file missing \"version\" fails schema validation, before the gate ever runs"
	else
		echo "$SELF: FAIL (a schema-invalid file did not go RED at validation): $out_c"
		fail=1
	fi

	# (d) positive control: the valid SARIF against the matching baseline.
	out_d="$(FINDINGS_BASELINE="$W/matching" FINDINGS_SUPPRESSIONS="$W/empty-suppressions.tsv" \
		run_check_only "$W/a.sarif" 2>&1)" && rc_d=0 || rc_d=$?
	if [ "${rc_d:-0}" -eq 0 ]; then
		echo "    ok: a valid file whose finding is in the baseline is GREEN"
	else
		echo "$SELF: FAIL (a matching baseline did not go GREEN): $out_d"
		fail=1
	fi

	# (e) 21 runs, one above the GitHub limit, against a matching baseline
	# that would otherwise be GREEN -> RED at the limit check.
	python3 -c "
import json
doc = json.load(open('$W/a.sarif'))
doc['runs'] = doc['runs'] + [{'tool': {'driver': {'name': 'extra%d' % i}}, 'results': []} for i in range(18)]
json.dump(doc, open('$W/e.sarif', 'w'))
"
	out_e="$(FINDINGS_BASELINE="$W/matching" FINDINGS_SUPPRESSIONS="$W/empty-suppressions.tsv" \
		run_check_only "$W/e.sarif" 2>&1)" && rc_e=0 || rc_e=$?
	if [ "${rc_e:-0}" -ne 0 ] && printf '%s\n' "$out_e" | grep -q "21 runs, above the limit of 20"; then
		echo "    ok: a file with 21 runs is RED at the upload limit check"
	else
		echo "$SELF: FAIL (21 runs did not go RED at the limit check): $out_e"
		fail=1
	fi

	# (f) annotate: the suppressed result stays in the file, gains the
	# suppressions entry, and the file still validates against the schema.
	cp "$W/a.sarif" "$W/f.sarif"
	printf 'GNU C23\tr1\ta.c\tD-01\n' >"$W/real-suppressions.tsv"
	out_f="$(python3 "$REPO_ROOT/tools/findings_gate.py" annotate "$W/f.sarif" "$W/real-suppressions.tsv" 2>&1)" && rc_f=0 || rc_f=$?
	if [ "${rc_f:-0}" -eq 0 ] \
		&& python3 "$REPO_ROOT/tools/sarif_merge.py" --validate-only "$W/f.sarif" >/dev/null \
		&& python3 -c "
import json, sys
r = json.load(open('$W/f.sarif'))['runs'][0]['results']
sys.exit(0 if len(r) == 1 and r[0]['suppressions'] == [{'kind': 'external', 'justification': 'D-01'}] else 1)
"; then
		echo "    ok: a suppressed result stays in the SARIF with kind external and its D-NN, and the file validates"
	else
		echo "$SELF: FAIL (annotate dropped or mis-wrote the suppression): rc=$rc_f $out_f"
		fail=1
	fi

	# (g) repeatability, task 17b: the cppcheck lane, run twice at -j1 on the
	# REAL runtime units, the second time with the database order reversed,
	# must give the same count for every (rule, file). misra.py's CTU pass
	# keys a cross-unit finding (an unused macro, rule 2.5, among others) by
	# NAME and reports it at the location in the FIRST unit summary it reads,
	# and that order follows -j's completion order. A name defined in two
	# files therefore moved its finding between them from run to run, which
	# made the shrink-only gate red with no change, and could make it green
	# over a real regression. -j1 makes the order the database order, so
	# reversing the database is a repeatable stand-in for -j's reordering.
	# RED before the GOO_PLATFORM_* names got one definition site
	# (include/runtime.h 4 + src/runtime/platform.h 6, reversed: platform.h 10).
	# REORDER_SCOPE widens the scan (for example to "src/"); the default is
	# the runtime units, which is where the defect was measured.
	reorder_scope="${REORDER_SCOPE:-src/runtime/}"
	if ! "$REPO_ROOT/tools/gen_compile_commands.sh" "$W/cc-full.json" >"$W/g-gen.log" 2>&1; then
		cat "$W/g-gen.log"
		echo "$SELF: FAIL (the repeatability control could not derive the compile database)"
		exit 1
	fi
	python3 - "$W/cc-full.json" "$reorder_scope" "$W/cc-fwd.json" "$W/cc-rev.json" <<'PYEOF'
import json, sys
full, scope, fwd, rev = sys.argv[1:5]
units = [e for e in json.load(open(full)) if e["file"].startswith(scope)]
if len(units) < 2:
    sys.exit("repeatability control: %d unit(s) under %r, need at least 2 to reorder" % (len(units), scope))
json.dump(units, open(fwd, "w"))
json.dump(units[::-1], open(rev, "w"))
PYEOF
	# Fix round 1 (M4): a LANE failure (cppcheck exit, 0 checked units, a
	# parse failure, a blind suppression) is not a repeatability verdict.
	# Report it as what it is, with the lane's own lines.
	rc_fwd=0
	rc_rev=0
	bash "$REPO_ROOT/scripts/analyzer_lanes_probe.sh" --cppcheck-lane "$W/cc-fwd.json" "$W/cppcheck-fwd" 1 >"$W/g-fwd.log" 2>&1 || rc_fwd=$?
	bash "$REPO_ROOT/scripts/analyzer_lanes_probe.sh" --cppcheck-lane "$W/cc-rev.json" "$W/cppcheck-rev" 1 >"$W/g-rev.log" 2>&1 || rc_rev=$?
	if [ "$rc_fwd" -ne 0 ] || [ "$rc_rev" -ne 0 ]; then
		echo "$SELF: FAIL (the cppcheck lane itself failed in the repeatability control on $reorder_scope: forward rc=$rc_fwd, reversed rc=$rc_rev; no repeatability verdict)"
		for g_log in "$W/g-fwd.log" "$W/g-rev.log"; do
			echo "    $(basename "$g_log"):"
			grep -E '^CPPCHECK_(SUMMARY|PARSE_FAILURE|BLIND_SUPPRESSION) |FAIL' "$g_log" | sed 's/^/        /' || true
		done
		fail=1
	else
		rc_g=0
		out_g="$(python3 - "$W/cppcheck-fwd/cppcheck.sarif" "$W/cppcheck-rev/cppcheck.sarif" <<'PYEOF' 2>&1
import collections, json, sys

def counts(path):
    c = collections.Counter()
    for run in json.load(open(path))["runs"]:
        for r in run.get("results", []):
            locs = r.get("locations") or [{}]
            uri = locs[0].get("physicalLocation", {}).get("artifactLocation", {}).get("uri", "")
            c[(r.get("ruleId", ""), uri)] += 1
    return c

fwd, rev = counts(sys.argv[1]), counts(sys.argv[2])
if sum(fwd.values()) == 0:
    sys.exit("forward run has 0 results: the lane analysed nothing, so equal counts prove nothing")
moved = sorted(k for k in set(fwd) | set(rev) if fwd[k] != rev[k])
for rule, uri in moved:
    print("    %s %s: forward %d, reversed %d" % (rule, uri, fwd[(rule, uri)], rev[(rule, uri)]))
print("%d results forward, %d reversed, %d (rule, file) group(s) differ" % (sum(fwd.values()), sum(rev.values()), len(moved)))
sys.exit(1 if moved else 0)
PYEOF
)" || rc_g=$?
		if [ "$rc_g" -eq 0 ]; then
			echo "    ok: the cppcheck lane gives the same (rule, file) counts with the $reorder_scope database reversed at -j1"
			echo "        ${out_g##*$'\n'}"
		else
			echo "$SELF: FAIL (the cppcheck lane is not repeatable on $reorder_scope: a reversed database order changes the counts)"
			printf '%s\n' "$out_g"
			grep -h '^CPPCHECK_SUMMARY ' "$W/g-fwd.log" "$W/g-rev.log" | sed 's/^/    /' || true
			fail=1
		fi
	fi

	if [ "$fail" -ne 0 ]; then
		exit 1
	fi
	echo "$SELF: PASS (a/b/c/e red and d/f green, on synthetic fixtures; g repeatable on the real $reorder_scope units; the real pipeline runs in findings-probe)"
	exit 0
fi

MODE=gate
case "${1:-}" in
	--update-baseline) MODE=update ;;
	"")                ;;
	*) echo "usage: $0 [--update-baseline|--self-test|--check-only]" >&2; exit 2 ;;
esac

# --- the real pipeline -----------------------------------------------------
mkdir -p "$SARIF_PROBES_DIR"
: >"$GATE_LOG"

# Neither sub-probe's own PASS/FAIL is a reason to abort THIS script: a
# lane or ratchet going red is DATA for step 4's gate-log conversion (a
# goo/gate-failed finding), not an infrastructure failure. set +e / -e
# brackets only the two calls whose exit status must be read without
# `set -e` tearing the script down first.
set +e
bash "$REPO_ROOT/scripts/analyzer_lanes_probe.sh" 2>&1 | tee "$GATE_LOG"
lanes_rc="${PIPESTATUS[0]}"
bash "$REPO_ROOT/scripts/pthread_return_probe.sh" 2>&1 | tee -a "$GATE_LOG"
pthread_rc="${PIPESTATUS[0]}"
set -e
echo "$PROBE: analyzer-lanes-probe exited $lanes_rc, pthread-return-probe exited $pthread_rc" \
	"(a nonzero exit here becomes a goo/gate-failed finding below, not an abort)"

CLANG_TIDY_VERSION="$(clang-tidy --version | sed -n 's/.*LLVM version[[:space:]]*//p')"
python3 "$REPO_ROOT/tools/clang_tidy_to_sarif.py" \
	--out "$SARIF_TIDY_DIR/clang-tidy.sarif" --version "$CLANG_TIDY_VERSION" "$SARIF_TIDY_DIR"

python3 "$REPO_ROOT/tools/probe_verdicts_to_sarif.py" pthread-baseline "$PTHREAD_BASELINE" \
	--out "$SARIF_PROBES_DIR/pthread-return.sarif"
python3 "$REPO_ROOT/tools/probe_verdicts_to_sarif.py" gate-log "$GATE_LOG" \
	--out "$SARIF_PROBES_DIR/gate-verdicts.sarif"

python3 "$REPO_ROOT/tools/sarif_merge.py" --out "$FINDINGS_SARIF" --validate \
	"$SARIF_GCC_DIR" "$SARIF_TIDY_DIR" "$SARIF_CPPCHECK_DIR" "$SARIF_PROBES_DIR"

# Task 18: mark the suppressed results for GitHub, then validate and check the
# upload limits on the file that the upload step reads.
python3 "$REPO_ROOT/tools/findings_gate.py" annotate "$FINDINGS_SARIF" "$FINDINGS_SUPPRESSIONS"
python3 "$REPO_ROOT/tools/sarif_merge.py" --validate-only "$FINDINGS_SARIF"
python3 "$REPO_ROOT/tools/sarif_merge.py" --check-limits "$FINDINGS_SARIF"

if [ "$MODE" = update ]; then
	python3 "$REPO_ROOT/tools/findings_gate.py" write "$FINDINGS_SARIF" "$FINDINGS_BASELINE" "$FINDINGS_SUPPRESSIONS"
	exit $?
fi

python3 "$REPO_ROOT/tools/findings_gate.py" check "$FINDINGS_SARIF" "$FINDINGS_BASELINE" "$FINDINGS_SUPPRESSIONS"
exit $?
