#!/usr/bin/env bash
# compile-commands-current-probe -- task 14 of the 2026-09-02 verification
# tooling plan. Keeps build/compile_commands.json a DERIVATION, never a file
# someone hand-edits or lets drift from the Makefile.
#
# THE ARTEFACT: build/compile_commands.json, produced by
# tools/gen_compile_commands.sh from GOO_OBJS (the Makefile's list of
# translation units linked into bin/goo through the object pattern rule)
# PLUS COMPILER_SRCS (the .c files $(COMPILER)'s own recipe compiles
# directly on its link line -- see that script's step 3b). build/ is
# git-ignored (.gitignore line 2), so this file is never committed -- this
# gate is what keeps it CURRENT on every machine that runs verify-core, and
# what task 15 (cppcheck's --project=build/compile_commands.json) and task
# 16 (the gcc strict lane) will both read.
#
# TWO INDEPENDENT CHECKS, because "the entry count matches" alone can pass
# for the wrong reason -- a file counted twice while a different file is
# missing entirely still gives the right total:
#   1. entry count == GOO_OBJS count + COMPILER_SRCS count (both the
#      Makefile's own $(words ...), never hand-copied -- see
#      read_object_count and read_link_sources_count below).
#   2. every entry's "file" is unique -- each src/*.c is in the list ONE
#      time, the task's own acceptance line, checked directly rather than
#      inferred from the count.
#
# SELF-CONSISTENCY: the generator is run TWICE in the same invocation --
# once to the real build/compile_commands.json (which this gate also keeps
# current as a side effect), once to a scratch path -- and the two outputs
# must be byte-identical. A derivation that gives a different answer on the
# same tree, seconds apart, is not safe for a downstream baseline (task 15)
# to compare against.
#
# Usage:
#   scripts/compile_commands_current_probe.sh              # the gate
#   scripts/compile_commands_current_probe.sh --self-test  # prove it can go red
set -u

PROBE="compile-commands-current-probe"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

GEN="${COMPILE_COMMANDS_GEN:-$REPO_ROOT/tools/gen_compile_commands.sh}"
OUT="${COMPILE_COMMANDS_OUT:-$REPO_ROOT/build/compile_commands.json}"

if [ ! -f "$GEN" ]; then
	echo "$PROBE: FAIL (generator not found: $GEN)"
	exit 1
fi

# read_object_count: $(words $(GOO_OBJS)) via an injected print-% rule, the
# same mechanism tools/gen_compile_commands.sh uses for the lists
# themselves -- one source of truth, never a hand-copied number.
#
# --no-print-directory and the GOO_MAKE_VAR= sentinel: this gate is itself
# invoked as a make recipe (`make compile-commands-current-probe`), so
# MAKELEVEL is already >= 1 when this nested `make` runs -- GNU make then
# auto-prints "Entering/Leaving directory" lines to the SAME stdout, with
# "Leaving directory" landing after our echo. A first version of this
# probe used a bare `tail -1` here: it passed every time it was run
# directly (`bash scripts/compile_commands_current_probe.sh`, MAKELEVEL=0,
# no announcement) and failed every time `make
# compile-commands-current-probe` ran it for real, reading the
# announcement as the count. Both guards close that: --no-print-directory
# removes the announcement at the source, and the sentinel means only a
# line we tagged ourselves is ever read as the value, whichever mode this
# runs in.
read_object_count() {
	make --no-print-directory -f "$REPO_ROOT/Makefile" \
		--eval='print-goo-objs-count: ; @echo GOO_MAKE_VAR=$(words $(GOO_OBJS))' \
		print-goo-objs-count 2>/dev/null \
		| sed -n 's/^GOO_MAKE_VAR=//p' \
		| tail -1
}

# read_link_sources_count: $(words $(COMPILER_SRCS)) -- the .c files
# $(COMPILER)'s recipe compiles directly on its link line (task 15 fix
# round 1: src/compiler/goo.c and src/compiler/test_discovery.c today).
# Same mechanism and same guards as read_object_count above; the two
# together are the full expected entry count.
read_link_sources_count() {
	make --no-print-directory -f "$REPO_ROOT/Makefile" \
		--eval='print-compiler-srcs-count: ; @echo GOO_MAKE_VAR=$(words $(COMPILER_SRCS))' \
		print-compiler-srcs-count 2>/dev/null \
		| sed -n 's/^GOO_MAKE_VAR=//p' \
		| tail -1
}

# check_json: $1 = path to a compile_commands.json, $2 = expected object
# count. Prints its reason(s) and returns 1 on any mismatch; prints nothing
# and returns 0 when both checks hold. This is the one place both the real
# gate and every --self-test mutation run their assertions, so a mutation
# proves the SAME code path the gate uses, not a second copy of the logic.
check_json() {
	local path="$1" want_count="$2"
	python3 - "$path" "$want_count" <<'PYEOF'
import json
import sys

path, want_count = sys.argv[1], int(sys.argv[2])
try:
    with open(path, encoding="utf-8") as f:
        entries = json.load(f)
except (OSError, json.JSONDecodeError) as exc:
    print(f"cannot read/parse {path}: {exc}")
    sys.exit(1)

files = [e.get("file") for e in entries]
got_count = len(files)
unique_count = len(set(files))

ok = True
if got_count != want_count:
    print(f"entry count {got_count} != expected count {want_count} "
          "(GOO_OBJS + COMPILER_SRCS)")
    ok = False
if unique_count != got_count:
    dupes = sorted({f for f in files if files.count(f) > 1})
    print(f"{got_count - unique_count} duplicate file entr(y/ies): {dupes}")
    ok = False

sys.exit(0 if ok else 1)
PYEOF
}

# ---------------------------------------------------------------------------
# --self-test: prove check_json can go red two independent ways, plus the
# positive control (an unmutated, freshly generated file must stay green).
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
	SELF="$PROBE --self-test"
	W="$(mktemp -d)"
	trap 'rm -rf "$W"' EXIT
	bad_self=0

	object_count="$(read_object_count)"
	link_sources_count="$(read_link_sources_count)"
	if ! [ "$object_count" -gt 0 ] 2>/dev/null; then
		echo "$SELF: FAIL (could not read a positive GOO_OBJS count from the Makefile)"
		exit 1
	fi
	if ! [ "$link_sources_count" -ge 0 ] 2>/dev/null; then
		echo "$SELF: FAIL (could not read a COMPILER_SRCS count from the Makefile)"
		exit 1
	fi
	expected_count="$((object_count + link_sources_count))"

	base="$W/base.json"
	if ! gen_out="$("$GEN" "$base" 2>&1)"; then
		echo "$SELF: FAIL (generator failed against the real tree)"
		printf '%s\n' "$gen_out" | sed 's/^/        /'
		exit 1
	fi

	# Control: the real, unmutated derivation must be green.
	if check_out="$(check_json "$base" "$expected_count")"; then
		echo "    ok: control (unmutated derivation) is GREEN"
	else
		echo "$SELF: FAIL (control already red -- the harness is broken, not the tree)"
		printf '%s\n' "$check_out" | sed 's/^/        /'
		bad_self=1
	fi

	# Mutation 1: remove one entry. Total count drops by one, so the
	# count check alone must catch this -- the task's own positive
	# control ("remove one entry, and the gate goes red").
	removed="$W/removed.json"
	python3 -c "
import json
data = json.load(open('$base'))
data.pop()
json.dump(data, open('$removed', 'w'))
"
	if check_out="$(check_json "$removed" "$expected_count")"; then
		echo "$SELF: FAIL (stayed GREEN after removing one entry)"
		bad_self=1
	else
		echo "    ok: removing one entry turns it red ($(printf '%s' "$check_out" | tr '\n' ' '))"
	fi

	# Mutation 2: duplicate one entry IN PLACE OF a different one, so the
	# array length -- and therefore the naive count -- is UNCHANGED. Only
	# the per-file uniqueness check can catch this: one file now appears
	# twice, a different file has silently dropped out of the list.
	dupe="$W/dupe.json"
	python3 -c "
import json
data = json.load(open('$base'))
assert len(data) >= 2, 'need at least two entries to duplicate one over another'
data[0] = dict(data[1])
json.dump(data, open('$dupe', 'w'))
"
	if check_out="$(check_json "$dupe" "$expected_count")"; then
		echo "$SELF: FAIL (stayed GREEN after duplicating one entry over another -- count alone is not enough)"
		bad_self=1
	else
		echo "    ok: duplicating one entry (same total count) turns it red ($(printf '%s' "$check_out" | tr '\n' ' '))"
	fi

	rm -rf "$W"
	trap - EXIT
	if [ "$bad_self" -ne 0 ]; then
		echo "$SELF: FAIL"
		exit 1
	fi
	echo "$SELF: PASS (control green; both mutations independently turn it red)"
	exit 0
fi

if [ -n "${1:-}" ]; then
	echo "usage: $0 [--self-test]" >&2
	exit 2
fi

# ---------------------------------------------------------------------------
# The gate.
# ---------------------------------------------------------------------------
object_count="$(read_object_count)"
link_sources_count="$(read_link_sources_count)"
if ! [ "$object_count" -gt 0 ] 2>/dev/null; then
	echo "$PROBE: FAIL (could not read a positive GOO_OBJS count from the Makefile)"
	exit 1
fi
if ! [ "$link_sources_count" -ge 0 ] 2>/dev/null; then
	echo "$PROBE: FAIL (could not read a COMPILER_SRCS count from the Makefile)"
	exit 1
fi
expected_count="$((object_count + link_sources_count))"

if ! gen_out="$("$GEN" "$OUT" 2>&1)"; then
	echo "$PROBE: FAIL (tools/gen_compile_commands.sh failed)"
	printf '%s\n' "$gen_out" | sed 's/^/  /'
	exit 1
fi

scratch_dir="$(mktemp -d)"
trap 'rm -rf "$scratch_dir"' EXIT
scratch_out="$scratch_dir/compile_commands.json"
if ! scratch_gen_out="$("$GEN" "$scratch_out" 2>&1)"; then
	echo "$PROBE: FAIL (a second, independent derivation failed)"
	printf '%s\n' "$scratch_gen_out" | sed 's/^/  /'
	exit 1
fi

if ! diff -q "$OUT" "$scratch_out" >/dev/null 2>&1; then
	echo "$PROBE: FAIL (two derivations of the same tree, seconds apart, do not agree byte-for-byte)"
	diff -u "$OUT" "$scratch_out" | head -40 | sed 's/^/  /'
	exit 1
fi

if ! check_out="$(check_json "$OUT" "$expected_count")"; then
	echo "$PROBE: FAIL ($OUT)"
	printf '%s\n' "$check_out" | sed 's/^/  /'
	exit 1
fi

entry_count="$(python3 -c "import json; print(len(json.load(open('$OUT'))))")"
echo "$PROBE: PASS ($OUT: $entry_count entries, GOO_OBJS: $object_count objects + COMPILER_SRCS: $link_sources_count link-line sources = $expected_count expected, self-consistent across two derivations)"
exit 0
