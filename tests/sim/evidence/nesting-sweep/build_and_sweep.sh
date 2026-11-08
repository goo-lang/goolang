#!/usr/bin/env bash
# Usage: tests/sim/evidence/nesting-sweep/build_and_sweep.sh <scratch-dir> <n-seeds>
# Builds sweep.c at -O0 and at -O2, sweeps seeds 1..n with each, and then
# compares the two sets of traces: the interleaving must depend on the seed
# only, not on the optimisation level. Exit 0 only when every seed passes at
# both levels and the trace sets are identical.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(git -C "$here" rev-parse --show-toplevel)" || exit 2
s=$1; n=$2
mkdir -p "$s" || exit 2
cd "$root" || exit 2
F=(-Wall -Wextra -std=c23 -g -I. -Iinclude -D_GNU_SOURCE -include include/xalloc.h -include include/goo_assert.h)
rc=0
for opt in O0 O2; do
  gcc "${F[@]}" -$opt tests/sim/sim_sched.c "$here/sweep.c" -o "$s/sweep_$opt" || { echo "build_$opt failed"; exit 2; }
  echo "-$opt:"; bash "$here/run.sh" "$s/sweep_$opt" "$n" "$s/w_$opt" || rc=1
done
if diff -r "$s/w_O0/a" "$s/w_O2/a" >/dev/null; then
  echo "traces -O0 vs -O2: identical ($n seeds)"
else
  echo "traces -O0 vs -O2: DIFFER"; rc=1
fi
exit $rc
