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
	mk() { rm -rf "$W/t"; mkdir -p "$W/t/scripts"; cp "$ROOT"/scripts/*probe*.sh "$W/t/scripts/" 2>/dev/null
	       cp "$BASELINE" "$W/t/scripts/probe-teeth-baseline.txt"; }
	run() { PROBE_TEETH_ROOT="$W/t" PROBE_TEETH_BASELINE="$W/t/scripts/probe-teeth-baseline.txt" \
	        "$0" >"$W/out.log" 2>&1; }
	bad_self=0

	mk
	if run; then echo "    ok: control (unmutated copy) is GREEN"
	else echo "$SELF: FAIL (control already red -- the harness is broken, not the tree)"
	     sed 's/^/        /' "$W/out.log"; exit 1; fi

	check() { # name, mutation
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

	[ "$bad_self" -ne 0 ] && exit 1
	echo "$SELF: PASS (control green; all 3 failure modes independently turn it red)"
	exit 0
fi

has_teeth() { grep -qE -- '--self-test|SELFTEST|MUTATE' "$1"; }

listed() { grep -v '^#' "$BASELINE" | grep -qx "$1"; }

toothed=0; grandfathered=0
for f in "$ROOT"/scripts/*probe*.sh; do
	[ -f "$f" ] || continue
	b="$(basename "$f")"
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
echo "$PROBE: PASS ($toothed probe(s) with teeth, $grandfathered grandfathered, baseline clean)"
exit 0
