#!/usr/bin/env bash
# far_world_probe.sh: run the far_world fixture at world 1, 2, 4 and 8 over
# ipc:// via scripts/far-probe.sh. Every world must pass.
#
#   far_world_probe.sh <binary>              the gate
#   far_world_probe.sh --self-test <binary>  prove the gate can go RED
#
# The fixture contract is `<binary> far <rank> <urlBase> <world> [fault rank]`.
# `fault` is passed ONLY by --self-test.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WORLDS="1 2 4 8"

# Run one world. Output goes to the caller; the status is far-probe.sh's.
run_world() { # binary world [fault rank]
  local bin="$1" world="$2"
  shift 2
  bash "$HERE/far-probe.sh" "$bin" "$world" "$world" "$@"
}

if [ "${1:-}" = "--self-test" ]; then
  BIN="${2:?usage: far_world_probe.sh --self-test <binary>}"
  SELF="far-world-selftest"
  OUT="$(mktemp)"
  trap 'rm -f "$OUT"' EXIT
  bad=0

  # control: no fault. Without it a broken fixture turns every case red
  # and reads as success.
  if run_world "$BIN" 4 > "$OUT" 2>&1; then
    echo "    ok: control (world 4, no fault) is GREEN"
  else
    echo "$SELF: FAIL (control is red, so the harness is broken)"
    sed 's/^/        /' "$OUT"
    exit 1
  fi

  # red_case <name> <world> <fault> <rank> <must-match> <must-not-match>
  # RED alone is not enough: the failure must name the right rank, so a
  # crash or a timeout cannot pass for the injected fault.
  red_case() {
    local name="$1" world="$2" fault="$3" rank="$4" want="$5" notwant="$6"
    if run_world "$BIN" "$world" "$fault" "$rank" > "$OUT" 2>&1; then
      echo "$SELF: FAIL (stayed green after: $name)"
      bad=1
      return
    fi
    if ! grep -qE "$want" "$OUT"; then
      echo "$SELF: FAIL ($name went red for the wrong reason; wanted /$want/)"
      sed 's/^/        /' "$OUT"
      bad=1
      return
    fi
    if [ -n "$notwant" ] && grep -qE "$notwant" "$OUT"; then
      echo "$SELF: FAIL ($name blamed a rank that had no fault; /$notwant/)"
      sed 's/^/        /' "$OUT"
      bad=1
      return
    fi
    echo "    ok: '$name' turns it RED, for the right reason"
  }

  red_case "world 4, rank 1 corrupts one cell" 4 value 1 \
    'rank 1 output not all-true' 'rank [023] output not all-true'
  red_case "world 4, rank 2 miscounts its rounds" 4 rounds 2 \
    'rank 2 output not all-true' 'rank [013] output not all-true'
  # The last rank gets world-1, so RunFar refuses its rank. The other rank
  # waits for a peer that never joins, which ends at far-probe's 30 s limit.
  red_case "world 2, rank 1 runs with world 1" 2 world 1 \
    'rank1 stderr: panic: lanes.RunFar: rank out of range' 'rank0 stderr'

  if [ "$bad" -ne 0 ]; then exit 1; fi
  echo "$SELF: PASS (value fault, rounds fault, world disagreement all RED; control GREEN)"
  exit 0
fi

BIN="${1:?usage: far_world_probe.sh <binary>}"
for w in $WORLDS; do
  if ! run_world "$BIN" "$w"; then
    echo "far-world-probe: FAIL (world $w)"
    exit 1
  fi
done
echo "far-world-probe: PASS (worlds $WORLDS, bit-identical to the serial reference)"
