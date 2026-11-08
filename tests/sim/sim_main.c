/* tests/sim/sim_main.c — the process entry of a program under the simulator
 * (spec section 8). The program's own main is renamed goo_sim_program_main
 * at compile time (-Dmain=goo_sim_program_main); this main reads the run
 * from the environment and runs that program as coroutine 0.
 *
 * The environment contract:
 *   GOO_SIM_SEED                        decimal uint64, default 1
 *   GOO_SIM_STEP_LIMIT                  decimal uint64 > 0, default 1000000
 *   GOO_SIM_TIME_LIMIT_NS               decimal uint64 > 0, default 10000000000
 *   GOO_SIM_TRACE_DIR                   required; the trace is <dir>/<seed>.trace
 *   GOO_SIM_FAULT_SPURIOUS_WAKE         0 or 1, default 0
 *   GOO_SIM_FAULT_THREAD_CREATE_EAGAIN  0 or 1, default 0
 * A malformed value is a usage error, exit 64, before the run starts: a
 * typo must not quietly fall back to a default and run a different test.
 *
 * How the process ends. When the program's main returns, coroutine 0 calls
 * sim_exit_program, which is C's exit(status) on that coroutine: production
 * ends the same way while its workers still run, and every seam call of the
 * exit path (atexit handlers, the stdio flush) stays on a coroutine. So
 * sim_run_until_done does not return while a program runs; if it does, no
 * seam call may follow it on the real thread, and the run ends at once. */
#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* TRACE_DIR_BYTES is sim_sched.c's buffer; sim_init asserts the same bound. */
enum { USAGE_EXIT = 64, U64_DIGITS_MAX = 20, TRACE_DIR_BYTES = 256 };

extern int goo_sim_program_main(void);

[[noreturn]] static void usage_exit(const char* name, const char* want) {
    GOO_ASSERT(name != nullptr);
    GOO_ASSERT(want != nullptr);
    fprintf(stderr, "goo-sim: %s must be %s\n", name, want);
    fflush(stderr);
    _exit(USAGE_EXIT);
}

/* Strict decimal: digits only, no sign, no space, no overflow. */
static uint64_t env_u64(const char* name, uint64_t fallback) {
    GOO_ASSERT(name != nullptr);
    const char* text = getenv(name);
    if (text == nullptr) return fallback;
    uint32_t length = (uint32_t)strnlen(text, U64_DIGITS_MAX + 1);
    if (length == 0) usage_exit(name, "a decimal uint64");
    if (length > U64_DIGITS_MAX) usage_exit(name, "a decimal uint64");
    uint64_t value = 0;
    for (uint32_t i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') usage_exit(name, "a decimal uint64");
        uint64_t digit = (uint64_t)(text[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) usage_exit(name, "a decimal uint64");
        value = value * 10 + digit;
    }
    return value;
}

static bool env_flag(const char* name) {
    GOO_ASSERT(name != nullptr);
    const char* text = getenv(name);
    if (text == nullptr) return false;
    if (strcmp(text, "0") == 0) return false;
    if (strcmp(text, "1") == 0) return true;
    usage_exit(name, "0 or 1");
}

/* Coroutine 0: the program's main thread (spec section 6). */
static void* program_thread(void* arg) {
    GOO_ASSERT(arg == nullptr);
    GOO_ASSERT(sim_current()->id == 0);
    int status = goo_sim_program_main();
    sim_exit_program(status);
}

int main(void) {
    uint64_t seed = env_u64("GOO_SIM_SEED", 1);
    uint64_t step_limit = env_u64("GOO_SIM_STEP_LIMIT", 1000000);
    uint64_t time_limit_ns = env_u64("GOO_SIM_TIME_LIMIT_NS", 10000000000ull);
    if (step_limit == 0) usage_exit("GOO_SIM_STEP_LIMIT", "greater than 0");
    if (time_limit_ns == 0) usage_exit("GOO_SIM_TIME_LIMIT_NS", "greater than 0");
    const char* trace_dir = getenv("GOO_SIM_TRACE_DIR");
    if (trace_dir == nullptr) usage_exit("GOO_SIM_TRACE_DIR", "set to a directory");
    if (trace_dir[0] == '\0') usage_exit("GOO_SIM_TRACE_DIR", "set to a directory");
    if (strnlen(trace_dir, TRACE_DIR_BYTES) >= TRACE_DIR_BYTES) usage_exit("GOO_SIM_TRACE_DIR", "shorter than 256 bytes");
    sim_os_set_faults(env_flag("GOO_SIM_FAULT_SPURIOUS_WAKE"),
                      env_flag("GOO_SIM_FAULT_THREAD_CREATE_EAGAIN"));

    sim_init(seed, step_limit, time_limit_ns, trace_dir);
    uint32_t id = sim_thread_spawn(program_thread, nullptr);
    GOO_ASSERT(id == 0);
    sim_run_until_done();
    /* Every coroutine exited, coroutine 0 included, so its main never
     * reached sim_exit_program. The real thread runs no seam call here. */
    sim_exit_with(SIM_INVARIANT, "main-coroutine-exited");
}
