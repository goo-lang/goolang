/* tests/sim/sim_sched_test.c — the scheduler, clock and trace with no
 * runtime linked (Task 4 of the 2026-09-03 deterministic-simulation plan).
 *
 * Each test runs its simulation in a forked child, because sim_exit_with
 * ends the process: a verdict in one test must not end the others. The
 * child writes its trace through the simulator's own writer, and the parent
 * compares trace FILES -- the same artifact a replay compares -- so the
 * tests judge what a person replaying a seed would see.
 *
 * T4 is the instrument behind Task 5's entry test: after sim_run_until_done
 * returns, the real thread has no coroutine, and sim_current() -- the first
 * thing every seam call in sim_os.c does -- must abort there. Without this,
 * tests/sim/sim_entry_test.sh could not tell "the exit path stayed on a
 * coroutine" from "the check never fires".
 *
 * T3 is the spike's step 2, the nesting measurement (spec section 9): a
 * seam call made from INSIDE a goroutine-shaped ucontext coroutine, which
 * itself runs on a simulated thread's coroutine. Its result selects
 * approach A (pass) or the named fallback B (fail).
 *
 * T1-T4 run with seeded preemption OFF (ppm 0). T5-T8 are Task 6a's tests
 * of the preemption model (spec section 6, "Seeded preemption"): T5 pins
 * the T1-T3 traces to the bytes they had before the model existed, T6-T8
 * run the model ON.
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
    PREEMPT_PPM_T6 = 250000,         /* about 25 preemptions in T6's ~100 decisions */
    PREEMPT_PPM_T7 = 500000,         /* no preemption in 64 decisions: 2^-64 */
    ONE_YIELDER_DECISIONS = 64,
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

[[noreturn]] static void run_two_yielders(uint64_t seed, const char* dir, uint32_t n, uint32_t ppm) {
    static uint32_t count;
    count = n;
    sim_init(seed, STEP_LIMIT, TIME_LIMIT_NS, ppm, dir);
    uint32_t a = sim_thread_spawn(yield_n, &count);
    uint32_t b = sim_thread_spawn(yield_n, &count);
    GOO_ASSERT(a != b);
    sim_run_until_done();
    sim_exit_with(SIM_PASS, "two-yielders");
}

/* T7's child: one coroutine, so every preemption leaves nothing runnable
 * and the clock must jump by exactly that preemption's d. */
[[noreturn]] static void run_one_yielder(uint64_t seed, const char* dir, uint32_t ppm) {
    static uint32_t count = ONE_YIELDER_DECISIONS;
    sim_init(seed, STEP_LIMIT, TIME_LIMIT_NS, ppm, dir);
    uint32_t id = sim_thread_spawn(yield_n, &count);
    GOO_ASSERT(id == 0);
    sim_run_until_done();
    sim_exit_with(SIM_PASS, "one-yielder");
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
    sim_init(seed, STEP_LIMIT, TIME_LIMIT_NS, 0, dir);
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

/* T4's child: a seam-shaped call on the real thread once every coroutine
 * exited. It must die by SIGABRT at sim_current()'s assertion. */
[[noreturn]] static void run_real_thread_call(uint64_t seed, const char* dir) {
    static uint32_t count = 3;
    sim_init(seed, STEP_LIMIT, TIME_LIMIT_NS, 0, dir);
    (void)sim_thread_spawn(yield_n, &count);
    sim_run_until_done();
    sim_thread_t* self = sim_current();       /* the assertion under test */
    fprintf(stderr, "sim_sched_test: T4 sim_current() returned thread %u on the real thread\n", self->id);
    sim_exit_with(SIM_PASS, "real-thread-call");
}

/* ---- the parent's side ---- */

enum workload { WORK_YIELD_10, WORK_YIELD_50, WORK_NESTING, WORK_YIELD_50_PREEMPT, WORK_ONE_YIELDER_PREEMPT };

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
        if (work == WORK_YIELD_10) run_two_yielders(seed, dir, 10, 0);
        if (work == WORK_YIELD_50) run_two_yielders(seed, dir, 50, 0);
        if (work == WORK_YIELD_50_PREEMPT) run_two_yielders(seed, dir, 50, PREEMPT_PPM_T6);
        if (work == WORK_ONE_YIELDER_PREEMPT) run_one_yielder(seed, dir, PREEMPT_PPM_T7);
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

/* T4: the child's stderr goes to <dir>/stderr, so the parent can read
 * WHICH assertion ended it, not only that something did. */
static int test_real_thread_call(void) {
    char dir[256], err_path[300];
    dir_join(dir, sizeof dir, "t4");
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return -1;
    snprintf(err_path, sizeof err_path, "%s/stderr", dir);
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int fd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || dup2(fd, STDERR_FILENO) < 0) _exit(3);
        run_real_thread_call(5, dir);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        fprintf(stderr, "sim_sched_test: T4 child did not die by SIGABRT (status 0x%x)\n", status);
        return -1;
    }
    long n = read_trace(dir, 5, g_file_a);   /* the SIGABRT handler wrote the trace */
    if (n < 0) return -1;
    if (strstr(g_file_a, " verdict=abort ") == nullptr) {
        fprintf(stderr, "sim_sched_test: T4 the trace header is not verdict=abort\n");
        return -1;
    }
    int fd = open(err_path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t got = read(fd, g_file_b, TRACE_FILE_BYTES_MAX - 1);
    close(fd);
    if (got <= 0) return -1;
    g_file_b[got] = '\0';
    if (strstr(g_file_b, "in sim_current()") == nullptr) {
        fprintf(stderr, "sim_sched_test: T4 the abort was not sim_current()'s assertion: %s\n", g_file_b);
        return -1;
    }
    printf("sim_sched_test: T4 real-thread call: PASS (SIGABRT in sim_current(), trace verdict=abort)\n");
    return 0;
}

/* ---- Task 6a: seeded preemption ---- */

/* FNV-1a 64 of a trace file's bytes. */
static uint64_t fnv1a64(const char* buf, long n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (long i = 0; i < n; i++) {
        h ^= (unsigned char)buf[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* T5's pins: the trace bytes T1-T3 wrote at commit 3acb6b8f, before the
 * preemption model existed, measured from the files themselves
 * (.superpowers/sdd/2026-09-03-deterministic-simulation/task-6a-evidence/
 * ref-head). A change here is a change to what every ppm-0 run does: make
 * it on purpose, with the new reference traces in the commit message. */
static const struct trace_pin {
    const char* leaf;
    uint64_t    seed;
    long        bytes;
    uint64_t    fnv1a64;
} k_off_pins[] = {
    { "t1a", 7,  467,  0x728c7380e248f791ull },
    { "t2a", 7,  1674, 0xef8dd1dc81d557e0ull },
    { "t2b", 8,  1674, 0x754673ed9fa8acd3ull },
    { "t3a", 11, 347,  0xde19fcda8946433aull },
};

/* T5, off is identical: with ppm 0 the model draws no random number, so
 * T1-T3's traces keep their pre-model bytes. Reads the files T1-T3 wrote. */
static int test_off_identical(void) {
    char dir[256];
    for (size_t i = 0; i < sizeof k_off_pins / sizeof k_off_pins[0]; i++) {
        const struct trace_pin* pin = &k_off_pins[i];
        dir_join(dir, sizeof dir, pin->leaf);
        long n = read_trace(dir, pin->seed, g_file_a);
        if (n < 0) return -1;
        uint64_t h = fnv1a64(g_file_a, n);
        if (n != pin->bytes || h != pin->fnv1a64) {
            fprintf(stderr, "sim_sched_test: T5 %s/%llu.trace is %ld bytes, fnv1a64 0x%016llx; "
                    "pinned %ld bytes, 0x%016llx\n", pin->leaf, (unsigned long long)pin->seed,
                    n, (unsigned long long)h, pin->bytes, (unsigned long long)pin->fnv1a64);
            return -1;
        }
    }
    printf("sim_sched_test: T5 off is identical: PASS (4 ppm-0 traces equal their pre-model bytes)\n");
    return 0;
}

typedef struct preempt_scan {
    uint32_t count;          /* preempt records */
    uint64_t sum_ns;         /* the sum of their d */
    uint32_t real_thread;    /* preempt records whose choice is the real thread ("-") */
    uint32_t out_of_bound;   /* d outside [2^SIM_PREEMPT_EXPONENT_MIN, SIM_PREEMPT_NS_MAX] */
    uint32_t malformed;      /* a preempt record without its d_ns= field */
} preempt_scan_t;

/* One record line: "step now_ns thread preempt choice d_ns=<d>". */
static void scan_preempt_line(const char* line, const char* eol, preempt_scan_t* scan) {
    const char* what = line;
    for (uint32_t field = 0; field < 3 && what < eol; field++) {
        what = memchr(what, ' ', (size_t)(eol - what));
        if (what == nullptr) return;
        what++;
    }
    if ((size_t)(eol - what) < 8 || memcmp(what, "preempt ", 8) != 0) return;
    scan->count++;
    const char* choice = what + 8;
    if (choice[0] == '-') scan->real_thread++;
    const char* d = strstr(choice, " d_ns=");
    if (d == nullptr || d > eol) {
        scan->malformed++;
        return;
    }
    unsigned long long d_ns = strtoull(d + 6, nullptr, 10);
    if (d_ns < (1ull << SIM_PREEMPT_EXPONENT_MIN) || d_ns > SIM_PREEMPT_NS_MAX) scan->out_of_bound++;
    scan->sum_ns += d_ns;
}

static preempt_scan_t scan_preempts(const char* trace) {
    preempt_scan_t scan = { 0 };
    for (const char* line = trace; *line != '\0';) {
        const char* eol = strchr(line, '\n');
        if (eol == nullptr) break;
        if (line[0] != '#') scan_preempt_line(line, eol, &scan);
        line = eol + 1;
    }
    return scan;
}

/* T6, on is deterministic: with the model on, seed 7 twice gives the same
 * bytes, seed 8 gives other bytes, and both runs really preempted. */
static int test_on_deterministic(void) {
    char da[256], db[256], dc[256];
    dir_join(da, sizeof da, "t6a");
    dir_join(db, sizeof db, "t6b");
    dir_join(dc, sizeof dc, "t6c");
    if (run_child(WORK_YIELD_50_PREEMPT, 7, da) != 0) return -1;
    if (run_child(WORK_YIELD_50_PREEMPT, 7, db) != 0) return -1;
    if (run_child(WORK_YIELD_50_PREEMPT, 8, dc) != 0) return -1;
    long na = read_trace(da, 7, g_file_a);
    long nb = read_trace(db, 7, g_file_b);
    if (na < 0 || nb < 0) return -1;
    if (na != nb || memcmp(g_file_a, g_file_b, (size_t)na) != 0) {
        fprintf(stderr, "sim_sched_test: T6 seed 7 traces differ with preemption on (%ld vs %ld bytes)\n", na, nb);
        return -1;
    }
    preempt_scan_t s7 = scan_preempts(g_file_a);
    long nc = read_trace(dc, 8, g_file_b);
    if (nc < 0) return -1;
    preempt_scan_t s8 = scan_preempts(g_file_b);
    if (s7.count == 0 || s8.count == 0) {
        fprintf(stderr, "sim_sched_test: T6 no preempt record at ppm %d (seed 7: %u, seed 8: %u)\n",
                PREEMPT_PPM_T6, s7.count, s8.count);
        return -1;
    }
    if (na == nc && memcmp(g_file_a, g_file_b, (size_t)na) == 0) {
        fprintf(stderr, "sim_sched_test: T6 seeds 7 and 8 wrote the same trace\n");
        return -1;
    }
    printf("sim_sched_test: T6 on is deterministic: PASS (seed 7 replays, %u preemptions; seed 8 differs, %u)\n",
           s7.count, s8.count);
    return 0;
}

/* T7, a preemption happens and costs time: one coroutine, 64 decisions, a
 * ppm that makes a preemption certain; the virtual clock at the end is at
 * least the sum of every d. Nothing else advances this clock. */
static int test_preempt_costs_time(void) {
    char dir[256];
    dir_join(dir, sizeof dir, "t7");
    if (run_child(WORK_ONE_YIELDER_PREEMPT, 3, dir) != 0) return -1;
    long n = read_trace(dir, 3, g_file_a);
    if (n < 0) return -1;
    preempt_scan_t scan = scan_preempts(g_file_a);
    unsigned long long now_ns = header_field(g_file_a, "now_ns=");
    if (scan.count == 0) {
        fprintf(stderr, "sim_sched_test: T7 no preempt record in %d decisions at ppm %d\n",
                ONE_YIELDER_DECISIONS, PREEMPT_PPM_T7);
        return -1;
    }
    if (now_ns < scan.sum_ns) {
        fprintf(stderr, "sim_sched_test: T7 the clock ends at %llu ns, below the %llu ns of %u preemptions\n",
                now_ns, (unsigned long long)scan.sum_ns, scan.count);
        return -1;
    }
    printf("sim_sched_test: T7 a preemption costs time: PASS (%u preemptions, sum d %llu ns, clock %llu ns)\n",
           scan.count, (unsigned long long)scan.sum_ns, now_ns);
    return 0;
}

/* T8, the bounds, read from every trace T6 and T7 wrote: no preempt record
 * names the real thread, and every d is inside its bound. The simulator
 * asserts both as well; this reads what the trace shows. */
static int test_preempt_bounds(void) {
    static const struct { const char* leaf; uint64_t seed; } runs[] = {
        { "t6a", 7 }, { "t6c", 8 }, { "t7", 3 },
    };
    char dir[256];
    uint32_t total = 0;
    for (size_t i = 0; i < sizeof runs / sizeof runs[0]; i++) {
        dir_join(dir, sizeof dir, runs[i].leaf);
        if (read_trace(dir, runs[i].seed, g_file_a) < 0) return -1;
        preempt_scan_t scan = scan_preempts(g_file_a);
        if (scan.real_thread != 0 || scan.out_of_bound != 0 || scan.malformed != 0) {
            fprintf(stderr, "sim_sched_test: T8 %s: %u on the real thread, %u out of bound, %u malformed\n",
                    runs[i].leaf, scan.real_thread, scan.out_of_bound, scan.malformed);
            return -1;
        }
        total += scan.count;
    }
    if (total == 0) {
        fprintf(stderr, "sim_sched_test: T8 no preempt record to check\n");
        return -1;
    }
    printf("sim_sched_test: T8 preemption bounds: PASS (%u records, none on the real thread, d in [%llu, %u] ns)\n",
           total, 1ull << SIM_PREEMPT_EXPONENT_MIN, SIM_PREEMPT_NS_MAX);
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
    if (test_real_thread_call() != 0) { printf("sim_sched_test: T4 real-thread call: FAIL\n"); failed++; }
    if (test_off_identical() != 0) { printf("sim_sched_test: T5 off is identical: FAIL\n"); failed++; }
    if (test_on_deterministic() != 0) { printf("sim_sched_test: T6 on is deterministic: FAIL\n"); failed++; }
    if (test_preempt_costs_time() != 0) { printf("sim_sched_test: T7 a preemption costs time: FAIL\n"); failed++; }
    if (test_preempt_bounds() != 0) { printf("sim_sched_test: T8 preemption bounds: FAIL\n"); failed++; }
    if (failed != 0) {
        printf("sim_sched_test: %d of 8 FAILED\n", failed);
        return 1;
    }
    printf("sim_sched_test: 8 of 8 PASS\n");
    return 0;
}
