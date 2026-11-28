#!/usr/bin/env bash
# Gate T-4 stdlib-api-ratchet (trusted-stdlib design section 7).
#
# The metric: scripts/stdlib-coverage.py --counts prints two lines,
#     linux-amd64 <covered>/<total>
#     application <covered>/<total>
# A symbol is COVERED only when scripts/check_stdlib_coverage.sh finds a
# golden fixture or a goostd test that calls it (anti-goal A4). A symbol that
# is merely vendored does not count.
#
# The gate: tests/stdlib/api-floor.txt holds one line,
#     linux-amd64 <count> application <count>
# The gate fails when either covered count falls below its floor, and when the
# measurement produces nothing. A package task raises the floor in the same
# commit that adds the coverage.
#
# Usage: stdlib_api_ratchet.sh [--self-test]
# Exit codes are read directly, never through a pipe.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FLOOR_REL="tests/stdlib/api-floor.txt"

# The gate never skips: it needs python3 and bash only (the API denominator is
# a committed snapshot, so no Go is needed). A missing python3 is a FAIL
# whether or not GOO_PROBE_NO_SKIP is set, because a skip would read as a pass.
if ! command -v python3 >/dev/null 2>&1; then
	echo "stdlib-api-ratchet: FAIL (python3 not found; this gate has no skip, GOO_PROBE_NO_SKIP=${GOO_PROBE_NO_SKIP:-0})"
	exit 1
fi

# gate ROOT: run the ratchet against the tree at ROOT. The FAIL tag is what the
# self-test matches, so each fault must be refused for its own reason.
gate() {
	local root=$1 out rc lin app flin fapp
	out=$(python3 "$root/scripts/stdlib-coverage.py" --counts 2>&1)
	rc=$?
	if [ "$rc" -ne 0 ]; then
		echo "stdlib-api-ratchet: FAIL[nothing] the measurement exited $rc"
		printf '%s\n' "$out" | sed -n '1,20p'
		return 1
	fi
	lin=$(printf '%s\n' "$out" | sed -nE 's#^linux-amd64 ([0-9]+)/[0-9]+$#\1#p')
	app=$(printf '%s\n' "$out" | sed -nE 's#^application ([0-9]+)/[0-9]+$#\1#p')
	if [ -z "$lin" ] || [ -z "$app" ] || [ "$lin" -eq 0 ]; then
		echo "stdlib-api-ratchet: FAIL[nothing] the measurement produced nothing (linux-amd64='${lin}' application='${app}')"
		return 1
	fi
	if [ ! -f "$root/$FLOOR_REL" ]; then
		echo "stdlib-api-ratchet: FAIL[floor-file] $FLOOR_REL is missing"
		return 1
	fi
	read -r _ flin _ fapp < "$root/$FLOOR_REL"
	case "$flin$fapp" in
	'' | *[!0-9]*)
		echo "stdlib-api-ratchet: FAIL[floor-file] $FLOOR_REL is not 'linux-amd64 <n> application <n>'"
		return 1
		;;
	esac
	local bad=0
	if [ "$lin" -lt "$flin" ]; then
		echo "stdlib-api-ratchet: FAIL[floor] linux-amd64 count $lin is below the floor $flin"
		bad=1
	fi
	if [ "$app" -lt "$fapp" ]; then
		echo "stdlib-api-ratchet: FAIL[floor] application count $app is below the floor $fapp"
		bad=1
	fi
	[ "$bad" -eq 0 ] || return 1
	echo "stdlib-api-ratchet: PASS (linux-amd64 $lin >= $flin, application $app >= $fapp)"
}

if [ "${1:-}" != "--self-test" ]; then
	gate "$ROOT"
	exit $?
fi

# --- self-test: each fault in a temporary copy ------------------------------
# The copy holds exactly what the metric reads. Each fault must (1) change the
# copy, which it proves before it runs the gate, and (2) be refused with its
# OWN tag, so a gate that fails for another reason is not accepted as RED.
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILS=0
bad() { echo "self-test FAIL: $*"; FAILS=$((FAILS + 1)); }

fresh_copy() {
	local d=$1
	mkdir -p "$d/scripts" "$d/src/types" "$d/tests"
	cp "$ROOT"/scripts/stdlib-coverage.py "$ROOT"/scripts/check_stdlib_coverage.sh "$d/scripts/"
	cp "$ROOT"/src/types/shim_signatures.c "$ROOT"/src/types/type_checker.c \
		"$ROOT"/src/types/expression_checker.c "$d/src/types/"
	cp -R "$ROOT/examples" "$ROOT/goostd" "$d/"
	cp -R "$ROOT/tests/stdlib" "$d/tests/"
}

# expect_red NAME DIR TAG: the gate must fail and print FAIL[TAG].
expect_red() {
	local name=$1 dir=$2 tag=$3 out rc
	out=$(gate "$dir" 2>&1)
	rc=$?
	if [ "$rc" -eq 0 ]; then
		bad "$name: gate passed, expected FAIL[$tag]"
	elif ! printf '%s\n' "$out" | grep -q "FAIL\[$tag\]"; then
		bad "$name: gate failed for another reason: $(printf '%s\n' "$out" | sed -n 1p)"
	else
		echo "self-test ok: $name -> FAIL[$tag]"
	fi
}

# (d) control: the unchanged tree must pass.
D="$TMP/control"
fresh_copy "$D"
if out=$(gate "$D" 2>&1); then
	echo "self-test ok: (d) unchanged tree -> PASS"
else
	bad "(d) unchanged tree must pass: $out"
fi

# (a) delete the only golden fixture that calls a counted symbol.
D="$TMP/a"
fresh_copy "$D"
sole=$(python3 "$D/scripts/stdlib-coverage.py" --counts --sole-callers 2>/dev/null |
	awk -F'\t' '$1=="SOLE" && $4 ~ /^examples\/[^\/]*\.goo$/ {print $4; exit}')
if [ -z "$sole" ] || [ ! -f "$D/$sole" ]; then
	bad "(a) no golden fixture is the only caller of a counted symbol; cannot build the fault"
else
	rm -f "$D/$sole" "$D/${sole%.goo}.expected.txt"
	if [ -e "$D/$sole" ]; then
		bad "(a) the mutation did not remove $sole"
	else
		expect_red "(a) delete $sole, the only caller of a counted symbol" "$D" floor
	fi
fi

# (b) floor one above the count, once for each of the two counts.
for which in linux application; do
	D="$TMP/b-$which"
	fresh_copy "$D"
	read -r _ flin _ fapp < "$D/$FLOOR_REL"
	if [ "$which" = linux ]; then
		printf 'linux-amd64 %d application %d\n' $((flin + 1)) "$fapp" > "$D/$FLOOR_REL"
	else
		printf 'linux-amd64 %d application %d\n' "$flin" $((fapp + 1)) > "$D/$FLOOR_REL"
	fi
	if cmp -s "$D/$FLOOR_REL" "$ROOT/$FLOOR_REL"; then
		bad "(b) the mutation did not change the floor"
	else
		expect_red "(b) $which floor set one above the count" "$D" floor
	fi
done

# (c) an empty fixture tree.
D="$TMP/c"
fresh_copy "$D"
rm -rf "$D/examples" "$D/goostd"
mkdir "$D/examples" "$D/goostd"
if [ -n "$(ls -A "$D/examples")" ] || [ -n "$(ls -A "$D/goostd")" ]; then
	bad "(c) the mutation did not empty the fixture tree"
else
	expect_red "(c) empty fixture tree" "$D" nothing
fi

if [ "$FAILS" -ne 0 ]; then
	echo "stdlib-api-ratchet-selftest: FAIL ($FAILS)"
	exit 1
fi
echo "stdlib-api-ratchet-selftest: PASS"
