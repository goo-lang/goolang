#!/usr/bin/env bash
# far-probe.sh (M2-B1): spawn a distributed lanes fixture as <world>
# processes over ipc:// in a temp dir, wait (30s timeout each — a teardown
# hang is a FAIL, not a stuck gate), and assert every rank's stdout is
# non-empty and all-"true" lines. Fixture arg contract:
#   <binary> far <rank> <urlBase> [extra-args...]
set -u
BIN="$1"
WORLD="$2"
shift 2
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
URL="ipc://$TMP/lane"
pids=()
for r in $(seq 0 $((WORLD - 1))); do
  timeout 30 "$BIN" far "$r" "$URL" "$@" > "$TMP/out.$r" 2> "$TMP/err.$r" &
  pids+=($!)
done
fail=0
for i in "${!pids[@]}"; do
  if ! wait "${pids[$i]}"; then
    echo "far-probe: rank $i FAILED (nonzero exit or 30s timeout)"
    sed "s/^/  rank$i stderr: /" "$TMP/err.$i"
    fail=1
  fi
done
if [ "$fail" -ne 0 ]; then exit 1; fi
for r in $(seq 0 $((WORLD - 1))); do
  if [ ! -s "$TMP/out.$r" ]; then
    echo "far-probe: rank $r produced no output"
    fail=1
    continue
  fi
  bad=$(grep -vx "true" "$TMP/out.$r")
  if [ -n "$bad" ]; then
    echo "far-probe: rank $r output not all-true:"
    sed "s/^/  rank$r: /" "$TMP/out.$r"
    fail=1
  fi
done
exit "$fail"
