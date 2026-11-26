#!/usr/bin/env bash
# Task 5 fix round 1: four mutants of tests/sim/sim_os.c, each linked into
# its own copy of the sim archive in a new directory, run through
# tests/sim/sim_entry_test.sh. The tracked sim_os.c is never edited.
# Usage: tests/sim/evidence/invariant-mutants/task5-fix1-mutants.sh, after
# make build/sim/sim_entry_test (it links the build/runtime objects and
# build/sim/sim_main.o). M0 must pass; M1-M4 must each fail their own case.
set -u
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)" || exit 2
SP="$(mktemp -d "${TMPDIR:-/tmp}/task5-fix1-XXXXXX")" || exit 2
echo "work dir: $SP"
CF=(-Wall -Wextra -std=c23 -g -I. -Iinclude -D_GNU_SOURCE -include include/xalloc.h -include include/goo_assert.h -Itests/sim)
RT=(build/runtime/runtime.o build/runtime/platform.o build/runtime/concurrency.o build/runtime/channels.o
    build/runtime/sync.o build/runtime/sync_shim.o build/runtime/time_shim.o build/runtime/testing.o
    build/runtime/deadlock.o build/runtime/io.o build/runtime/arena.o build/runtime/defer.o build/sim/sim_sched.o)

sed 's|    return g_scheduler->scheduler_mutex;   /\* nullptr while goo_scheduler_init runs \*/|    return nullptr;   /* MUTANT M1: the lookup fails */|' tests/sim/sim_os.c > "$SP/M1_lookup_fails.c"
sed 's|    sim_exit_with(SIM_INVARIANT, "lock-order");|    /* MUTANT M2: the check never fires */|' tests/sim/sim_os.c > "$SP/M2_check_removed.c"
sed 's|sim_exit_with(SIM_INVARIANT, "recursive-lock");|(void)0; /* MUTANT M3 */|' tests/sim/sim_os.c > "$SP/M3_recursive_removed.c"
sed 's|sim_exit_with(SIM_INVARIANT, "join-detached");|sim_exit_with(SIM_INVARIANT, "join-self"); /* MUTANT M4: wrong detail */|' tests/sim/sim_os.c > "$SP/M4_wrong_detail.c"
cp tests/sim/sim_os.c "$SP/M0_control.c"

for m in M0_control M1_lookup_fails M2_check_removed M3_recursive_removed M4_wrong_detail; do
    if [ "$m" != M0_control ] && cmp -s tests/sim/sim_os.c "$SP/$m.c"; then
        echo "== $m: the sed changed nothing (instrument broken)"
        continue
    fi
    gcc "${CF[@]}" -c "$SP/$m.c" -o "$SP/$m.o" > "$SP/$m.cc" 2>&1 || { echo "== $m: cc failed"; continue; }
    ar -Drcs "$SP/$m.a" "${RT[@]}" "$SP/$m.o"
    gcc "${CF[@]}" -Dmain=goo_sim_program_main tests/sim/sim_entry_test.c build/sim/sim_main.o "$SP/$m.a" -lm -o "$SP/$m.bin" > "$SP/$m.ld" 2>&1 || { echo "== $m: link failed"; continue; }
    bash tests/sim/sim_entry_test.sh "$SP/$m.bin" "$SP/$m.work" > "$SP/$m.run" 2>&1
    rc=$?
    echo "== $m driver exit=$rc"
    grep -E 'FAIL|PASS' "$SP/$m.run" | head -3
done
