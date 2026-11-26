# tests/sim/evidence — the instruments behind tests/sim/README.md

Scripts and their logs only. The traces they write (about 17 MB for the
nesting sweep) stay in the scratch directory each script is given, and are
not committed: every trace here can be re-made from its seed.

None of these scripts is a gate. They are the measurements of Tasks 4 and 5
of the 2026-09-03 deterministic-simulation plan, kept so that each number in
`tests/sim/README.md` has an instrument a person can run again.

## nesting-sweep/ (Task 4: nested `ucontext` under one real thread)

| File | What |
|---|---|
| `sweep.c` | the T3 nesting workload of `sim_sched_test.c` over one seed, without T3's "switched away >= 1" assertion, so a seed with no switch is counted, not aborted |
| `run.sh` | runs a sweep binary on seeds 1..n, two times each, and compares the traces with `cmp`; a crash, a non-zero exit or an empty trace counts as a failure |
| `build_and_sweep.sh <dir> <n>` | builds `sweep.c` at `-O0` and `-O2`, sweeps both, then compares the two trace sets with `diff -r` |
| `mutants.sh <dir>` | two mutants of `sim_sched.c` (the process id in the seed; the seed ignored); `sim_sched_test` must fail on each |
| `sweep-ff97af61.txt`, `mutants-ff97af61.txt` | the first measurement, commit `ff97af61`, 2026-11-08 |
| `sweep-8233d02f.txt`, `mutants-8233d02f.txt` | the same scripts at commit `8233d02f`, 2026-11-08 |

Result, both commits: 1000 of 1000 seeds pass at `-O0` and at `-O2`, 0
crashes, 0 replay mismatches; 1,399 switches inside the nested coroutine in
all, 144 seeds with none, at most 3 (of 3) in one seed; the `-O0` and `-O2`
traces are identical. Both mutants fail (exit 1). The harness can report the
negative: the first run at `ff97af61` failed to build (a zsh word-splitting
error) and reported `pass=0 crash_or_nonzero=1000`.

The sweep was first measured before seeded preemption existed. `sweep.c`
now calls `sim_init` with ppm 0, which draws exactly the random numbers of
that first model, and the counts at `8233d02f` equal the counts at
`ff97af61`. At `8233d02f` the mutants also fail tests that did not exist at
`ff97af61`: M1 fails T5 and T6, and M2 fails T5 to T9.

## invariant-mutants/ (Task 5 fix round 1: each invariant fires on its own)

| File | What |
|---|---|
| `task5-fix1-mutants.sh` | four mutants of `sim_os.c`, each linked into its own copy of the sim archive and run through `tests/sim/sim_entry_test.sh`; M0 is the unchanged file |
| `task5-fix1-mutants-8233d02f.txt` | its output at commit `8233d02f`, 2026-11-08, work-dir path replaced by `<work>` |

Run it after `make build/sim/sim_entry_test`. Result: M0 passes (exit 0).
M1 (the scheduler-mutex lookup returns nothing) and M2 (the lock-order check
removed) fail the `lock-order` case; M3 (the recursive-lock check removed)
fails `recursive-lock` with `sim-deadlock`; M4 (the wrong detail text) fails
`join-detached` on the header text. Each mutant fails only its own case.
The script only prints; read the `driver exit=` lines.
