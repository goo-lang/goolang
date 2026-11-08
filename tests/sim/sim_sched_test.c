/* tests/sim/sim_sched_test.c — the scheduler, clock and trace with no
 * runtime linked (Task 4 of the 2026-09-03 deterministic-simulation plan).
 *
 * Each test runs its simulation in a forked child, because sim_exit_with
 * ends the process: a verdict in one test must not end the others. The
 * child writes its trace through the simulator's own writer, and the parent
 * compares trace FILES -- the same artifact a replay compares -- so the
 * tests judge what a person replaying a seed would see.
 *
 * T3 is the spike's step 2, the nesting measurement (spec section 9): a
 * seam call made from INSIDE a goroutine-shaped ucontext coroutine, which
 * itself runs on a simulated thread's coroutine. Its result selects
 * approach A (pass) or the named fallback B (fail).
 *
 * Usage: sim_sched_test <trace-base-dir> */
#include "sim.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    TRACE_FILE_BYTES_MAX = 1 << 20,
    CHOICES_MAX = 4096,
    STEP_LIMIT = 100000,
    CO_STACK_BYTES = 65536,
};
static const uint64_t TIME_LIMIT_NS = 10ull * 1000 * 1000 * 1000;

static char g_base_dir[200];

/* ---- workloads, run inside a child ---- */

static void* yield_n(void* arg) {
    uint32_t n = *(const uint32_t*)arg;
    GOO_ASSERT(n > 0);
    for (uint32_t i = 0; i < n; i++) {
        sim_yield_point("yield");
    }
    GOO_ASSERT(sim_current()->state == SIM_RUNNABLE);
    return arg;
}

[[noreturn]] static void run_two_yielders(uint64_t seed, const char* dir, uint32_t n) {
    static uint32_t count;
    count = n;
    sim_init(seed, STEP_LIMIT, TIME_LIMIT_NS, dir);
    uint32_t a = sim_thread_spawn(yield_n, &count);
    uint32_t b = sim_thread_spawn(yield_n, &count);
    GOO_ASSERT(a != b);
    sim_run_until_done();
    sim_exit_with(SIM_PASS, "two-yielders");
}

/* T3's coroutine, in the shape of concurrency.c: getcontext + makecontext
 * on a stack of its own, entered by swapcontext from the thread's own
 * context, leaving once by an explicit swap back (goo_yield's path) and
 * once by returning through uc_link (goroutine_wrapper's path). */
static ucontext_t g_worker_ctx;
static ucontext_t g_co_ctx;
static unsigned char g_co_stack[CO_STACK_BYTES];
static uint32_t g_nest_thread;
static volatile uint32_t g_b_progress;
static volatile uint32_t g_nested_yields;
static volatile uint32_t g_nested_switched_away;
static volatile uint32_t g_co_canary_ok;

static void nested_coroutine(void) {
    volatile uint64_t canary = 0xC0FFEE00DEADBEEFull;
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t before = g_b_progress;
        sim_yield_point("nested");
        GOO_ASSERT(sim_current()->id == g_nest_thread);
        GOO_ASSERT(canary == 0xC0FFEE00DEADBEEFull);
        g_nested_yields++;
        if (g_b_progress != before) {
            g_nested_switched_away++;
        }
        if (i == 0) {
            /* goo_yield's shape: back to the worker, which yields on its
             * own stack and then resumes this coroutine where it left. */
            int rc = swapcontext(&g_co_ctx, &g_worker_ctx);
            GOO_ASSERT(rc == 0);
        }
    }
    g_co_canary_ok = (canary == 0xC0FFEE00DEADBEEFull);
}

static void* nesting_worker(void* arg) {
    volatile uint64_t canary = 0x5EED5EED5EED5EEDull;
    g_nest_thread = sim_current()->id;
    int rc = getcontext(&g_co_ctx);
    GOO_ASSERT(rc == 0);
    g_co_ctx.uc_stack.ss_sp = g_co_stack;
    g_co_ctx.uc_stack.ss_size = sizeof g_co_stack;
    g_co_ctx.uc_link = &g_worker_ctx;
    makecontext(&g_co_ctx, nested_coroutine, 0);

    rc = swapcontext(&g_worker_ctx, &g_co_ctx);      /* enter */
    GOO_ASSERT(rc == 0);
    GOO_ASSERT(g_nested_yields == 1);                /* came back by the explicit swap */
    sim_yield_point("worker");                       /* the worker's own stack yields */
    GOO_ASSERT(canary == 0x5EED5EED5EED5EEDull);
    rc = swapcontext(&g_worker_ctx, &g_co_ctx);      /* re-enter */
    GOO_ASSERT(rc == 0);
    GOO_ASSERT(g_nested_yields == 3);                /* came back through uc_link */
    GOO_ASSERT(canary == 0x5EED5EED5EED5EEDull);
    GOO_ASSERT(sim_current()->id == g_nest_thread);
    return arg;
}

static void* progress_yielder(void* arg) {
    for (uint32_t i = 0; i < 8; i++) {
        g_b_progress++;
        sim_yield_point("plain");
    }
    GOO_ASSERT(g_b_progress == 8);
    return arg;
}

[[noreturn]] static void run_nesting(uint64_t seed, const char* dir) {
    sim_init(seed, STEP_LIMIT, TIME_LIMIT_NS, dir);
    (void)sim_thread_spawn(nesting_worker, nullptr);
    (void)sim_thread_spawn(progress_yielder, nullptr);
    sim_run_until_done();
    GOO_ASSERT(g_nested_yields == 3);
    GOO_ASSERT(g_co_canary_ok == 1);
    printf("sim_sched_test: T3 detail: nested yields=%u, switched away inside the coroutine=%u\n",
           g_nested_yields, g_nested_switched_away);
    fflush(stdout);
    GOO_ASSERT(g_nested_switched_away >= 1);         /* the seam call really crossed threads */
    sim_exit_with(SIM_PASS, "nesting");
}

/* ---- the parent's side ---- */

enum workload { WORK_YIELD_10, WORK_YIELD_50, WORK_NESTING };

static int run_child(enum workload work, uint64_t seed, const char* dir) {
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "sim_sched_test: mkdir %s: %s\n", dir, strerror(errno));
        return -1;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "sim_sched_test: fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        if (work == WORK_YIELD_10) run_two_yielders(seed, dir, 10);
        if (work == WORK_YIELD_50) run_two_yielders(seed, dir, 50);
        run_nesting(seed, dir);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        fprintf(stderr, "sim_sched_test: waitpid: %s\n", strerror(errno));
        return -1;
    }
    if (!WIFEXITED(status)) {
        fprintf(stderr, "sim_sched_test: child for seed %llu died by signal %d\n",
                (unsigned long long)seed, WIFSIGNALED(status) ? WTERMSIG(status) : -1);
        return -1;
    }
    if (WEXITSTATUS(status) != 0) {
        fprintf(stderr, "sim_sched_test: child for seed %llu exited %d\n",
                (unsigned long long)seed, WEXITSTATUS(status));
        return -1;
    }
    return 0;
}

static char g_file_a[TRACE_FILE_BYTES_MAX];
static char g_file_b[TRACE_FILE_BYTES_MAX];

/* Returns the byte count, or -1. An empty or oversized trace is a failure:
 * an empty file would compare equal to another empty file. */
static long read_trace(const char* dir, uint64_t seed, char* buf) {
    char path[300];
    snprintf(path, sizeof path, "%s/%llu.trace", dir, (unsigned long long)seed);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "sim_sched_test: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    long total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, (size_t)(TRACE_FILE_BYTES_MAX - 1 - total));
        if (n < 0) {
            fprintf(stderr, "sim_sched_test: read %s: %s\n", path, strerror(errno));
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total += n;
        if (total >= TRACE_FILE_BYTES_MAX - 1) {
            fprintf(stderr, "sim_sched_test: %s exceeds %d bytes\n", path, TRACE_FILE_BYTES_MAX);
            close(fd);
            return -1;
        }
    }
    close(fd);
    buf[total] = '\0';
    if (total == 0) {
        fprintf(stderr, "sim_sched_test: %s is empty\n", path);
        return -1;
    }
    return total;
}

static unsigned long long header_field(const char* trace, const char* key) {
    const char* p = strstr(trace, key);
    const char* eol = strchr(trace, '\n');
    if (p == nullptr || eol == nullptr || p > eol) return 0;
    return strtoull(p + strlen(key), nullptr, 10);
}

/* The choice column (the last field) of every record line, in order. */
static size_t choices_of(const char* trace, unsigned long* out) {
    size_t n = 0;
    for (const char* line = trace; *line != '\0' && n < CHOICES_MAX;) {
        const char* eol = strchr(line, '\n');
        if (eol == nullptr) break;
        if (line[0] != '#') {
            const char* sp = eol;
            while (sp > line && sp[-1] != ' ') sp--;
            out[n++] = strtoul(sp, nullptr, 10);
        }
        line = eol + 1;
    }
    return n;
}

static void dir_join(char* out, size_t cap, const char* leaf) {
    snprintf(out, cap, "%s/%s", g_base_dir, leaf);
}

/* T1: two threads yield ten times each; the trace of seed 7 equals the
 * trace of seed 7 run again. */
static int test_replay(void) {
    char da[256], db[256];
    dir_join(da, sizeof da, "t1a");
    dir_join(db, sizeof db, "t1b");
    if (run_child(WORK_YIELD_10, 7, da) != 0) return -1;
    if (run_child(WORK_YIELD_10, 7, db) != 0) return -1;
    long na = read_trace(da, 7, g_file_a);
    long nb = read_trace(db, 7, g_file_b);
    if (na < 0 || nb < 0) return -1;
    if (na != nb || memcmp(g_file_a, g_file_b, (size_t)na) != 0) {
        fprintf(stderr, "sim_sched_test: T1 seed 7 traces differ (%ld vs %ld bytes)\n", na, nb);
        return -1;
    }
    printf("sim_sched_test: T1 replay: PASS (steps=%llu, trace bytes=%ld)\n",
           header_field(g_file_a, "steps="), na);
    return 0;
}

/* T2: seed 7 and seed 8 produce different first choices (at least one
 * differs across 100 yields). */
static int test_seeds_differ(void) {
    static unsigned long ca[CHOICES_MAX], cb[CHOICES_MAX];
    char da[256], db[256];
    dir_join(da, sizeof da, "t2a");
    dir_join(db, sizeof db, "t2b");
    if (run_child(WORK_YIELD_50, 7, da) != 0) return -1;
    if (run_child(WORK_YIELD_50, 8, db) != 0) return -1;
    long na = read_trace(da, 7, g_file_a);
    long nb = read_trace(db, 8, g_file_b);
    if (na < 0 || nb < 0) return -1;
    size_t ka = choices_of(g_file_a, ca);
    size_t kb = choices_of(g_file_b, cb);
    size_t k = ka < kb ? ka : kb;
    if (k < 100) {
        fprintf(stderr, "sim_sched_test: T2 too few records (%zu, %zu)\n", ka, kb);
        return -1;
    }
    size_t differ = 0;
    for (size_t i = 0; i < 100; i++) {
        if (ca[i] != cb[i]) differ++;
    }
    if (differ == 0) {
        fprintf(stderr, "sim_sched_test: T2 seeds 7 and 8 made the same 100 choices\n");
        return -1;
    }
    printf("sim_sched_test: T2 seeds differ: PASS (steps=%llu, %zu of 100 choices differ)\n",
           header_field(g_file_a, "steps="), differ);
    return 0;
}

/* T3, the nesting measurement: a thread creates a ucontext coroutine on its
 * own stack the way concurrency.c does (getcontext/makecontext/swapcontext),
 * enters it, and INSIDE it calls sim_yield_point("nested") three times while
 * a second thread also yields; the coroutine returns to its thread, the
 * thread exits, and the trace replays identically for seed 11. */
static int test_nesting(void) {
    char da[256], db[256];
    dir_join(da, sizeof da, "t3a");
    dir_join(db, sizeof db, "t3b");
    if (run_child(WORK_NESTING, 11, da) != 0) return -1;
    if (run_child(WORK_NESTING, 11, db) != 0) return -1;
    long na = read_trace(da, 11, g_file_a);
    long nb = read_trace(db, 11, g_file_b);
    if (na < 0 || nb < 0) return -1;
    if (na != nb || memcmp(g_file_a, g_file_b, (size_t)na) != 0) {
        fprintf(stderr, "sim_sched_test: T3 seed 11 traces differ (%ld vs %ld bytes)\n", na, nb);
        return -1;
    }
    printf("sim_sched_test: T3 nesting: PASS (steps=%llu, trace bytes=%ld)\n",
           header_field(g_file_a, "steps="), na);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2 || strlen(argv[1]) >= sizeof g_base_dir - 8) {
        fprintf(stderr, "usage: sim_sched_test <trace-base-dir>\n");
        return 2;
    }
    snprintf(g_base_dir, sizeof g_base_dir, "%s", argv[1]);
    if (mkdir(g_base_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "sim_sched_test: mkdir %s: %s\n", g_base_dir, strerror(errno));
        return 2;
    }
    int failed = 0;
    if (test_replay() != 0) { printf("sim_sched_test: T1 replay: FAIL\n"); failed++; }
    if (test_seeds_differ() != 0) { printf("sim_sched_test: T2 seeds differ: FAIL\n"); failed++; }
    if (test_nesting() != 0) { printf("sim_sched_test: T3 nesting: FAIL\n"); failed++; }
    if (failed != 0) {
        printf("sim_sched_test: %d of 3 FAILED\n", failed);
        return 1;
    }
    printf("sim_sched_test: 3 of 3 PASS\n");
    return 0;
}
