#!/usr/bin/env bash
# Usage: tests/sim/evidence/nesting-sweep/mutants.sh <scratch-dir>
# Two mutants of tests/sim/sim_sched.c, each a scratch copy with one sed
# edit; sim_sched_test must exit non-zero on each. The tracked file is never
# edited. A sed that changes nothing is reported as a broken instrument.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
wt="$(git -C "$here" rev-parse --show-toplevel)" || exit 2
s=$1
F=(-Wall -Wextra -std=c23 -g -I"$wt" -I"$wt/include" -D_GNU_SOURCE -include "$wt/include/xalloc.h" -include "$wt/include/goo_assert.h")
rc=0
run_mutant() {
  name=$1; expr=$2
  d="$s/mut_$name"; rm -rf "$d"; mkdir -p "$d"
  cp "$wt/tests/sim/sim.h" "$wt/tests/sim/sim_sched.c" "$wt/tests/sim/sim_sched_test.c" "$d/"
  sed -i "$expr" "$d/sim_sched.c"
  if cmp -s "$d/sim_sched.c" "$wt/tests/sim/sim_sched.c"; then echo "$name: SED DID NOT APPLY"; rc=2; return; fi
  gcc "${F[@]}" "$d/sim_sched.c" "$d/sim_sched_test.c" -o "$d/t" || { echo "$name: build failed"; rc=2; return; }
  "$d/t" "$d/traces" > "$d/out.log" 2>&1; mrc=$?
  echo "$name: exit=$mrc"; grep -E 'PASS|FAIL|ASSERT' "$d/out.log"
  [ "$mrc" -ne 0 ] || rc=1
}
# M1: the process id enters the seed, so a replay in a new process is not a replay.
run_mutant M1_pid_seed 's|g_rng = mix ^ (mix >> 31);|g_rng = mix ^ (mix >> 31) ^ (uint64_t)getpid();|'
# M2: the scheduler ignores the seed and always takes the first runnable thread.
run_mutant M2_seed_ignored 's|uint32_t result = (uint32_t)(r % n);|uint32_t result = (uint32_t)(r % n) * 0u;|'
exit $rc
