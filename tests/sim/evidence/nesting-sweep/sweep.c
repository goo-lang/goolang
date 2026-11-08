/* The T3 nesting workload of tests/sim/sim_sched_test.c over ONE seed,
 * without T3's "switched away >= 1" assertion, so that a seed with no
 * cross-thread switch is counted rather than aborted. Evidence for Task 4 of
 * the 2026-09-03 deterministic-simulation plan (approach A: nested ucontext
 * under one real thread). Usage: sweep <seed> <trace-dir>
 *
 * The preemption model did not exist when this sweep was first measured
 * (commit ff97af61). ppm 0 is that model: with it the run draws exactly the
 * random numbers it drew then (spec section 6, "Determinism"). */
#define main sim_sched_test_main
#include "tests/sim/sim_sched_test.c"
#undef main
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    uint64_t seed = strtoull(argv[1], nullptr, 10);
    sim_init(seed, STEP_LIMIT, TIME_LIMIT_NS, 0u, argv[2]);
    (void)sim_thread_spawn(nesting_worker, nullptr);
    (void)sim_thread_spawn(progress_yielder, nullptr);
    sim_run_until_done();
    GOO_ASSERT(g_nested_yields == 3);
    GOO_ASSERT(g_co_canary_ok == 1);
    printf("away=%u\n", g_nested_switched_away);
    fflush(stdout);
    sim_exit_with(SIM_PASS, "nesting");
}
