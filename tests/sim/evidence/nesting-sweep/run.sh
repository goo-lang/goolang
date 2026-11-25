#!/usr/bin/env bash
# Usage: run.sh <binary> <n-seeds> <workdir>. Runs each seed two times and
# compares the two traces with cmp. A crash, a non-zero exit or an empty
# trace counts as a failure, so the harness can report the negative.
set -u
bin=$1; n=$2; w=$3
rm -rf "$w"; mkdir -p "$w/a" "$w/b"
pass=0; crash=0; mismatch=0; away_total=0; away_zero=0; away_max=0
for ((seed = 1; seed <= n; seed++)); do
  out=$("$bin" "$seed" "$w/a" 2>/dev/null); rc=$?
  "$bin" "$seed" "$w/b" >/dev/null 2>&1; rc2=$?
  if [ $rc -ne 0 ] || [ $rc2 -ne 0 ]; then crash=$((crash+1)); continue; fi
  if ! cmp -s "$w/a/$seed.trace" "$w/b/$seed.trace"; then mismatch=$((mismatch+1)); continue; fi
  if [ ! -s "$w/a/$seed.trace" ]; then crash=$((crash+1)); continue; fi
  pass=$((pass+1))
  a=${out#away=}
  away_total=$((away_total+a)); [ "$a" -eq 0 ] && away_zero=$((away_zero+1)); [ "$a" -gt "$away_max" ] && away_max=$a
done
echo "seeds=$n pass=$pass crash_or_nonzero=$crash replay_mismatch=$mismatch switched_away_total=$away_total seeds_with_zero_switches=$away_zero max_switches_in_one_seed=$away_max"
[ "$pass" -eq "$n" ]
