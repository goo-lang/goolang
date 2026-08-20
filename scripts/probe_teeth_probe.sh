#!/usr/bin/env bash
# probe-teeth-probe — every probe must be able to report the OPPOSITE result.
#
# A probe with no negative control has never demonstrated it can go red. It can
# pass forever while testing nothing, and the failure is invisible by
# construction: green is exactly what you expect to see.
#
# Two real instances from 2026-08-20, both found only by running the negative:
#   - repro_build_probe.sh --self-test could not execute on CI at all. Its own
#     "did the injection land" check grepped the WORK TREE while the probe
#     builds `git archive HEAD`.
#   - doc_claims_probe.sh's shim assertion used a substring match and stayed
#     green under the very mutation it existed to catch.
#
# THE RULE: a new probe cannot enter this tree without teeth. Existing ones are
# grandfathered in a SHRINK-ONLY baseline, which this probe also keeps honest --
# a stale baseline is the failure mode scripts/safety-baseline.txt reached at
# 139 dead entries out of 218 before anyone noticed.
#
# IT SCANS scripts/*.sh, AND IT USED TO SCAN scripts/*probe*.sh. That glob was
# the rule as implemented, and it was not the rule as written. 20 of the 44
# scripts here do not have "probe" in the name, 16 of those had no teeth, and
# 11 of those are wired into the Makefile as real gates -- including
# run_golden.sh, which runs the 495-fixture golden suite this project leans on
# hardest, and assert_corpus.sh, which sweeps 754 fixtures through an
# assert-enabled compiler. None of them was listed as an exemption, because the
# scan never considered them at all.
#
# That is the worse kind of gap: the baseline showed 17 grandfathered entries,
# which made the exemption set look deliberate and bounded, while 16 further
# scripts were exempt by filename. An absence nobody chose reads exactly like
# an absence somebody did.
set -u

PROBE="probe-teeth-probe"
ROOT="${PROBE_TEETH_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
BASELINE="${PROBE_TEETH_BASELINE:-$ROOT/scripts/probe-teeth-baseline.txt}"
fails=0
bad() { echo "  FAIL: $*"; fails=$((fails+1)); }

# --self-test: the probe that demands negative controls must have one. Three
# injections plus a CONTROL -- without the control a broken temp tree turns
# everything red and reads as success.
if [ "${1:-}" = "--self-test" ]; then
	SELF="$PROBE --self-test"
	W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
	# Copies EVERY script, not just *probe*.sh. The scan was widened to
	# scripts/*.sh on 2026-08-20 and this line was not: the baseline then named
	# 15 files the temp tree did not contain, and the control went red with
	# "baseline names run_golden.sh, which is not in scripts/". What the
	# self-test copies has to match what the scan covers.
	mk() { rm -rf "$W/t"; mkdir -p "$W/t/scripts"; cp "$ROOT"/scripts/*.sh "$W/t/scripts/" 2>/dev/null
	       cp "$BASELINE" "$W/t/scripts/probe-teeth-baseline.txt"; }
	run() { PROBE_TEETH_ROOT="$W/t" PROBE_TEETH_BASELINE="$W/t/scripts/probe-teeth-baseline.txt" \
	        "$0" >"$W/out.log" 2>&1; }
	bad_self=0

	mk
	if run; then echo "    ok: control (unmutated copy) is GREEN"
	else echo "$SELF: FAIL (control already red -- the harness is broken, not the tree)"
	     sed 's/^/        /' "$W/out.log"; exit 1; fi

	checks_run=0
	check() { # name, mutation
		checks_run=$((checks_run + 1))
		mk; ( cd "$W/t" && eval "$2" ) >/dev/null 2>&1
		if run; then echo "$SELF: FAIL (stayed green after: $1)"; sed 's/^/        /' "$W/out.log"; bad_self=1
		else echo "    ok: '$1' turns it red"; fi
	}

	check "a NEW probe with no teeth" \
		'printf "#!/usr/bin/env bash\necho ok\n" > scripts/brandnew_probe.sh'
	check "a baseline entry for a probe that HAS teeth" \
		'echo doc_claims_probe.sh >> scripts/probe-teeth-baseline.txt'
	check "a baseline entry naming a file that does not exist" \
		'echo ghost_probe.sh >> scripts/probe-teeth-baseline.txt'
	check "a TEETH claim pointing at a file that is not in the tree" \
		'sed -i "s|^# TEETH: .*|# TEETH: scripts/no_such_probe.sh|" scripts/testcase_report.sh'
	check "a TEETH claim pointing at a file that never mentions it" \
		'sed -i "s|^# TEETH: .*|# TEETH: scripts/probe-teeth-baseline.txt|" scripts/testcase_report.sh'
	check "prose that merely MENTIONS a self-test, with no mechanism" \
		'printf "#!/usr/bin/env bash\n# see foo.sh --self-test for how this is checked\necho ok\n" > scripts/mention_only_probe.sh'

	[ "$bad_self" -ne 0 ] && exit 1
	echo "$SELF: PASS (control green; all $checks_run failure modes independently turn it red)"
	exit 0
fi

# A MENTION IS NOT A MECHANISM. This used to grep for the bare strings, and on
# 2026-08-20 a comment in testcase_report.sh reading "goo_testcase_probe.sh
# --self-test edits the logic below" satisfied it -- the file had no self-test
# at all and the probe went green. Require the shape of an invocation or a
# guard: the flag compared in a test, or the keyword at the start of a line.
has_teeth() {
	# MECHANISMS, NOT MENTIONS. Anchored so prose cannot satisfy them.
	grep -qE -- '^[[:space:]]*self_test\(\)'        "$1" && return 0  # a function
	grep -qE -- '^[[:space:]]*--self-test\)'         "$1" && return 0  # a case arm
	grep -qE -- '=[[:space:]]*"--self-test"[[:space:]]*\]' "$1" && return 0  # an if-test
	grep -qE -- '^# MUTATION-HARNESS:'               "$1" && return 0  # mutation IS the run
	return 1
}

listed() { grep -v '^#' "$BASELINE" | grep -qx "$1"; }

# A script can BORROW its negative control from another that mutates it.
# scripts/testcase_report.sh is the first: it holds no --self-test, and two of
# the three mutations in goo_testcase_probe.sh edit its logic lines directly. A
# baseline entry would say it is unguarded, which is false.
#
# THE CLAIM IS CHECKED, NOT TRUSTED. `# TEETH: <path>` must name a file that
# exists AND that mentions this one. A cross-reference nobody verifies is a
# comment, and a comment that drifts from its code is the defect doc-claims-probe
# and the stale Makefile notes of 2026-08-20 were both about.
toothed=0; grandfathered=0; delegated=0
for f in "$ROOT"/scripts/*.sh; do
	[ -f "$f" ] || continue
	b="$(basename "$f")"

	claim="$(grep -m1 -E '^# TEETH: ' "$f" | sed 's/^# TEETH:[[:space:]]*//')"
	if [ -n "$claim" ]; then
		if [ ! -f "$ROOT/$claim" ]; then
			bad "$b delegates its negative control to $claim, which is not in this tree"
		elif ! grep -q -- "$b" "$ROOT/$claim"; then
			bad "$b delegates its negative control to $claim, which never mentions $b"
			echo "    a cross-reference nothing verifies is a comment"
		else
			toothed=$((toothed+1)); delegated=$((delegated+1))
			if listed "$b"; then
				bad "$b HAS teeth (delegated to $claim) but is still in the baseline"
			fi
		fi
		continue
	fi

	if has_teeth "$f"; then
		toothed=$((toothed+1))
		if listed "$b"; then
			bad "$b HAS teeth but is still in the baseline"
			echo "    remove it from scripts/probe-teeth-baseline.txt in the same commit"
		fi
	elif listed "$b"; then
		grandfathered=$((grandfathered+1))
	else
		bad "$b has no negative control and is not in the baseline"
		echo "    add --self-test proving it can go red, or justify it in the baseline"
	fi
done

# The baseline must not name things that are not probes any more.
while IFS= read -r name; do
	case "$name" in ''|\#*) continue ;; esac
	[ -f "$ROOT/scripts/$name" ] || bad "baseline names $name, which is not in scripts/"
done < "$BASELINE"

if [ "$fails" -ne 0 ]; then
	echo "$PROBE: FAIL ($fails problem(s))"
	exit 1
fi
echo "$PROBE: PASS ($toothed with teeth ($delegated delegated), $grandfathered grandfathered, baseline clean)"
exit 0
