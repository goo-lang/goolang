/* tests/sim/sim_sched.c — the simulator's scheduler, virtual clock and
 * trace ring (spec section 6). One real thread runs every simulated OS
 * thread as a ucontext coroutine; the seed alone picks the interleaving.
 *
 * Tiger Style (spec section 7): all storage is static, sized by the sim.h
 * constants, and touched only from this one real thread; every loop is
 * bounded by a table size; there is no recursion. The trace writer formats
 * its own integers and calls only open/write/close, so the SIGABRT handler
 * can use it too. */
#include "sim.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SIM_NONE              UINT32_MAX   /* g_current while the real thread's own context runs */
#define SIM_WHAT_BYTES        12u
#define SIM_TRACE_DIR_BYTES   256u
#define SIM_TRACE_PATH_BYTES  300u
#define SIM_OUT_BUFFER_BYTES  65536u
#define SIM_OUT_LINE_BYTES_MAX 512u        /* the longest line one out_* sequence appends */
#define SIM_DETAIL_BYTES_MAX  128u

typedef struct sim_trace_record {
    uint64_t step;
    uint64_t now_ns;
    uint32_t thread;                 /* who reached the point; SIM_NONE = the real thread */
    char     what[SIM_WHAT_BYTES];   /* the seam call, NUL-padded */
    uint32_t choice;                 /* the thread picked or woken; SIM_NONE = back to the real thread */
    uint32_t preempt_ns;             /* a preempt record's d; 0 on every other record */
} sim_trace_record_t;
/* The brief's record is 36 bytes of fields, 40 with alignment; the spec's
 * table says 32. The brief's fields are kept verbatim, so the ring is
 * 2.5 MiB, not 2 MiB. Pinned here so a field change is a visible decision.
 * Task 6a put preempt_ns in the 4 bytes that were alignment padding. */
static_assert(sizeof(sim_trace_record_t) == 40, "trace record size");
static_assert(SIM_PREEMPT_NS_MAX <= UINT32_MAX, "a preempt record holds d in 32 bits");
static_assert(SIM_PREEMPT_EXPONENT_MIN <= SIM_PREEMPT_EXPONENT_MAX, "a non-empty exponent range");
static_assert(SIM_PREEMPT_PPM_DEFAULT <= SIM_PPM_ONE, "the default is a probability");
static_assert(SIM_WAITERS_MAX >= SIM_THREADS_MAX, "every thread can wait on one object");
static_assert(SIM_OUT_BUFFER_BYTES > 2 * SIM_OUT_LINE_BYTES_MAX, "a flush leaves room for one line");

static sim_thread_t       g_threads[SIM_THREADS_MAX];   /* static; never allocated after sim_init */
static uint32_t           g_thread_count;
static uint32_t           g_current;
static uint64_t           g_seed;
static uint64_t           g_rng;                        /* xorshift64* state; never 0 */
static uint64_t           g_step, g_step_limit;
static uint64_t           g_now_ns, g_time_limit_ns;
static uint32_t           g_preempt_ppm;                /* 0 = the model is off */
static ucontext_t         g_sched_ctx;                  /* the real thread's own context */
static sim_trace_record_t g_trace[SIM_TRACE_ENTRIES];
static uint64_t           g_trace_count;                /* total appended; the ring keeps the last SIM_TRACE_ENTRIES */
static char               g_trace_dir[SIM_TRACE_DIR_BYTES];
static char               g_trace_path[SIM_TRACE_PATH_BYTES];
static bool               g_initialized;
static bool               g_handlers_installed;
static volatile sig_atomic_t g_trace_written;
static const char*        g_exit_label;                 /* set by sim_exit_program for the atexit write */
static char               g_out[SIM_OUT_BUFFER_BYTES];
static uint32_t           g_out_len;
static int                g_out_fd;

/* ---- the seeded generator ---- */

uint32_t sim_random_below(uint32_t n) {
    GOO_ASSERT(n > 0);
    GOO_ASSERT(g_rng != 0);
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    uint64_t r = g_rng * 2685821657736338717ULL;
    uint32_t result = (uint32_t)(r % n);
    GOO_ASSERT(result < n);
    return result;
}

/* ---- the trace ring and its writer ---- */

static void trace_append(const char* what, uint32_t choice, uint32_t preempt_ns) {
    GOO_ASSERT(what != nullptr);
    GOO_ASSERT(choice < g_thread_count || choice == SIM_NONE);
    GOO_ASSERT(preempt_ns <= SIM_PREEMPT_NS_MAX);
    sim_trace_record_t* rec = &g_trace[g_trace_count % SIM_TRACE_ENTRIES];
    memset(rec, 0, sizeof *rec);
    rec->step = g_step;
    rec->now_ns = g_now_ns;
    rec->thread = g_current;
    for (uint32_t i = 0; i + 1 < SIM_WHAT_BYTES && what[i] != '\0'; i++) {
        rec->what[i] = what[i];
    }
    rec->choice = choice;
    rec->preempt_ns = preempt_ns;
    g_trace_count++;
    GOO_ASSERT(rec->what[SIM_WHAT_BYTES - 1] == '\0');
}

static void out_flush(void) {
    GOO_ASSERT(g_out_len <= SIM_OUT_BUFFER_BYTES);
    uint32_t done = 0;
    for (uint32_t tries = 0; done < g_out_len && tries < 1024; tries++) {
        ssize_t n = write(g_out_fd, g_out + done, g_out_len - done);
        if (n <= 0) {
            static const char msg[] = "goo-sim: trace write failed; the trace is incomplete\n";
            (void)!write(STDERR_FILENO, msg, sizeof msg - 1);
            break;
        }
        done += (uint32_t)n;
    }
    g_out_len = 0;
    GOO_ASSERT(g_out_len == 0);
}

/* Appends at most SIM_DETAIL_BYTES_MAX bytes of s. With one_line, a
 * newline in s becomes a space, so caller text cannot split a record. */
static void out_bytes(const char* s, bool one_line) {
    GOO_ASSERT(s != nullptr);
    for (uint32_t i = 0; s[i] != '\0' && i < SIM_DETAIL_BYTES_MAX; i++) {
        GOO_ASSERT(g_out_len < SIM_OUT_BUFFER_BYTES);
        g_out[g_out_len++] = (one_line && s[i] == '\n') ? ' ' : s[i];
    }
    GOO_ASSERT(g_out_len <= SIM_OUT_BUFFER_BYTES);
}

static void out_str(const char* s) { out_bytes(s, false); }
static void out_text(const char* s) { out_bytes(s, true); }

/* Writes n in decimal into buf (at least 21 bytes); returns the length. */
static uint32_t fmt_u64(char* buf, uint64_t n) {
    char rev[20];
    uint32_t len = 0;
    do {
        GOO_ASSERT(len < sizeof rev);
        rev[len++] = (char)('0' + (n % 10));
        n /= 10;
    } while (n != 0);
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = rev[len - 1 - i];
    }
    buf[len] = '\0';
    GOO_ASSERT(len >= 1);
    GOO_ASSERT(len <= 20);
    return len;
}

static void out_u64(uint64_t n) {
    char buf[21];
    uint32_t before = g_out_len;
    uint32_t len = fmt_u64(buf, n);
    out_str(buf);
    GOO_ASSERT(len >= 1);
    GOO_ASSERT(g_out_len == before + len);
}

static void out_thread(uint32_t id) {
    GOO_ASSERT(id < SIM_THREADS_MAX || id == SIM_NONE);
    if (id == SIM_NONE) {
        out_str("-");
        return;
    }
    GOO_ASSERT(id != SIM_NONE);
    out_u64(id);
}

static const char* verdict_label(enum sim_verdict verdict) {
    switch (verdict) {
    case SIM_PASS:             return "pass";
    case SIM_RUNTIME_DEADLOCK: return "runtime-deadlock-abort";
    case SIM_DEADLOCK:         return "sim-deadlock";
    case SIM_INVARIANT:        return "invariant";
    case SIM_STEP_LIMIT:       return "step-limit";
    case SIM_TIME_LIMIT:       return "time-limit";
    case SIM_REPLAY_MISMATCH:  return "replay-mismatch";
    case SIM_LIMIT:            return "limit-";
    }
    return nullptr;
}

static void out_header(const char* label, const char* detail, uint64_t retained) {
    GOO_ASSERT(label != nullptr);
    GOO_ASSERT(detail != nullptr);
    out_str("# goo-sim trace v1 seed="); out_u64(g_seed);
    /* The model is part of the experiment, so the header names it. At ppm 0
     * there are no fields, so a ppm-0 trace keeps its pre-model bytes. */
    if (g_preempt_ppm != 0) {
        out_str(" preempt_ppm=");   out_u64(g_preempt_ppm);
        out_str(" preempt_e_min="); out_u64(SIM_PREEMPT_EXPONENT_MIN);
        out_str(" preempt_e_max="); out_u64(SIM_PREEMPT_EXPONENT_MAX);
        out_str(" step_cost_ns=");  out_u64(SIM_STEP_COST_NS);
    }
    out_str(" steps=");    out_u64(g_step);
    out_str(" now_ns=");   out_u64(g_now_ns);
    out_str(" records=");  out_u64(g_trace_count);
    out_str(" retained="); out_u64(retained);
    out_str(" verdict=");  out_str(label);
    if (strcmp(label, "limit-") == 0) out_text(detail);
    out_str(" detail=");   out_text(detail);
    out_str("\n# step now_ns thread what choice\n");
}

/* Writes the ring, oldest retained record first, once per process. Called
 * by sim_exit_with, the atexit handler and the SIGABRT handler. */
static void trace_write(const char* label, const char* detail) {
    GOO_ASSERT(label != nullptr);
    GOO_ASSERT(g_initialized);
    if (g_trace_written) return;
    g_trace_written = 1;
    g_out_fd = open(g_trace_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_out_fd < 0) {
        static const char msg[] = "goo-sim: cannot open the trace file; no trace written\n";
        (void)!write(STDERR_FILENO, msg, sizeof msg - 1);
        return;
    }
    uint64_t retained = g_trace_count < SIM_TRACE_ENTRIES ? g_trace_count : SIM_TRACE_ENTRIES;
    g_out_len = 0;
    out_header(label, detail, retained);
    for (uint64_t i = g_trace_count - retained; i < g_trace_count; i++) {
        const sim_trace_record_t* rec = &g_trace[i % SIM_TRACE_ENTRIES];
        out_u64(rec->step);     out_str(" ");
        out_u64(rec->now_ns);   out_str(" ");
        out_thread(rec->thread); out_str(" ");
        out_text(rec->what);    out_str(" ");
        out_thread(rec->choice);
        /* Only a preempt record has a d, so a ppm-0 trace keeps its bytes. */
        if (rec->preempt_ns != 0) { out_str(" d_ns="); out_u64(rec->preempt_ns); }
        out_str("\n");
        if (g_out_len > SIM_OUT_BUFFER_BYTES - SIM_OUT_LINE_BYTES_MAX) out_flush();
    }
    out_flush();
    if (close(g_out_fd) != 0) {
        static const char msg[] = "goo-sim: closing the trace file failed\n";
        (void)!write(STDERR_FILENO, msg, sizeof msg - 1);
    }
    GOO_ASSERT(g_trace_written == 1);
}

/* The label is the program's own verdict when its main returned
 * (sim_exit_program), else "exit": the runtime or the program called exit
 * itself, as goo_deadlock_abort does with status 2. */
static void on_process_exit(void) {
    GOO_ASSERT(g_handlers_installed);
    if (!g_initialized) return;
    if (g_exit_label != nullptr) {
        trace_write(g_exit_label, "main returned");
        return;
    }
    trace_write("exit", "the process called exit");
}

static void on_abort_signal(int signo) {
    GOO_ASSERT(signo == SIGABRT);
    if (g_initialized) trace_write("abort", "SIGABRT");
    struct sigaction dfl = { .sa_handler = SIG_DFL };
    sigemptyset(&dfl.sa_mask);
    (void)sigaction(SIGABRT, &dfl, nullptr);
    (void)raise(SIGABRT);
}

static void install_handlers_once(void) {
    GOO_ASSERT(!g_initialized);
    if (g_handlers_installed) return;
    int rc = atexit(on_process_exit);
    GOO_ASSERT(rc == 0);
    struct sigaction sa = { .sa_handler = on_abort_signal };
    sigemptyset(&sa.sa_mask);
    rc = sigaction(SIGABRT, &sa, nullptr);
    GOO_ASSERT(rc == 0);
    g_handlers_installed = true;
}

/* ---- the thread table ---- */

static bool all_exited(void) {
    GOO_ASSERT(g_thread_count <= SIM_THREADS_MAX);
    for (uint32_t i = 0; i < g_thread_count; i++) {
        if (g_threads[i].state != SIM_EXITED) return false;
    }
    GOO_ASSERT(g_thread_count > 0);
    return true;
}

static bool any_sleeper(void) {
    GOO_ASSERT(g_thread_count <= SIM_THREADS_MAX);
    for (uint32_t i = 0; i < g_thread_count; i++) {
        if (g_threads[i].state == SIM_SLEEP) return true;
    }
    GOO_ASSERT(g_thread_count == 0 || g_threads[0].state != SIM_SLEEP);
    return false;
}

static void make_runnable(uint32_t id) {
    GOO_ASSERT(id < g_thread_count);
    GOO_ASSERT(g_threads[id].state != SIM_RUNNABLE);
    GOO_ASSERT(g_threads[id].state != SIM_EXITED);
    g_threads[id].state = SIM_RUNNABLE;
    g_threads[id].wait_on = nullptr;
    g_threads[id].wake_at_ns = 0;
}

static uint32_t runnable_pick(void) {
    uint32_t ids[SIM_THREADS_MAX];
    uint32_t count = 0;
    GOO_ASSERT(g_thread_count <= SIM_THREADS_MAX);
    for (uint32_t i = 0; i < g_thread_count; i++) {
        if (g_threads[i].state == SIM_RUNNABLE) ids[count++] = i;
    }
    if (count == 0) return SIM_NONE;
    uint32_t pick = ids[sim_random_below(count)];
    GOO_ASSERT(pick < g_thread_count);
    GOO_ASSERT(g_threads[pick].state == SIM_RUNNABLE);
    return pick;
}

/* Makes every sleeper whose deadline is at or before now runnable. */
static uint32_t wake_due_sleepers(void) {
    GOO_ASSERT(g_thread_count <= SIM_THREADS_MAX);
    uint32_t woken = 0;
    for (uint32_t i = 0; i < g_thread_count; i++) {
        if (g_threads[i].state == SIM_SLEEP && g_threads[i].wake_at_ns <= g_now_ns) {
            make_runnable(i);
            woken++;
        }
    }
    GOO_ASSERT(woken <= g_thread_count);
    return woken;
}

static void clock_advance_to_earliest_sleeper(void) {
    uint64_t earliest = UINT64_MAX;
    GOO_ASSERT(any_sleeper());
    for (uint32_t i = 0; i < g_thread_count; i++) {
        if (g_threads[i].state == SIM_SLEEP && g_threads[i].wake_at_ns < earliest) {
            earliest = g_threads[i].wake_at_ns;
        }
    }
    GOO_ASSERT(earliest > g_now_ns);   /* a sleeper due now was woken by the last advance */
    g_now_ns = earliest;
    if (g_now_ns > g_time_limit_ns) sim_exit_with(SIM_TIME_LIMIT, "virtual clock");
    uint32_t woken = wake_due_sleepers();
    GOO_ASSERT(woken >= 1);
}

/* ---- switching ---- */

/* The pick is uniform over every runnable thread, the current one included,
 * so picking the current thread is a legal decision and needs no swap. */
static void switch_to(uint32_t next) {
    GOO_ASSERT(next < g_thread_count);
    GOO_ASSERT(g_threads[next].state == SIM_RUNNABLE);
    uint32_t prev = g_current;
    if (prev == next) return;
    g_current = next;
    ucontext_t* from = (prev == SIM_NONE) ? &g_sched_ctx : &g_threads[prev].ctx;
    int rc = swapcontext(from, &g_threads[next].ctx);
    GOO_ASSERT(rc == 0);
    GOO_ASSERT(g_current == prev);      /* resumed only by a switch that picked us */
}

/* The last thread exited: hand the real thread its own context back. The
 * exited coroutine is never resumed. */
[[noreturn]] static void return_to_scheduler(void) {
    uint32_t prev = g_current;
    GOO_ASSERT(prev < g_thread_count);
    GOO_ASSERT(g_threads[prev].state == SIM_EXITED);
    g_current = SIM_NONE;
    int rc = swapcontext(&g_threads[prev].ctx, &g_sched_ctx);
    GOO_ASSERT(rc == 0);
    sim_exit_with(SIM_INVARIANT, "an exited thread was resumed");
}

/* With the model on, a decision costs SIM_STEP_COST_NS, and every sleeper
 * due by then (a preempted coroutine or an ordinary sleeper) is runnable
 * before the pick. A spinner then cannot hold the clock still. */
static void step_cost(void) {
    GOO_ASSERT(g_preempt_ppm > 0);
    GOO_ASSERT(g_now_ns <= g_time_limit_ns);
    uint64_t room_ns = g_time_limit_ns - g_now_ns;
    if (room_ns < SIM_STEP_COST_NS) sim_exit_with(SIM_TIME_LIMIT, "virtual clock");
    g_now_ns += SIM_STEP_COST_NS;
    (void)wake_due_sleepers();
}

/* One scheduling decision: a step, and with the model on its cost. */
static void step_begin(const char* what) {
    GOO_ASSERT(what != nullptr);
    g_step++;
    if (g_step > g_step_limit) sim_exit_with(SIM_STEP_LIMIT, what);
    if (g_preempt_ppm > 0) step_cost();
}

/* The seeded pick, with the clock jump when nothing is runnable. Returns a
 * coroutine, never SIM_NONE: the last exit hands the real thread its own
 * context back, and nothing runnable with no sleeper is sim-deadlock. */
static uint32_t pick_or_advance(const char* what) {
    uint32_t next = runnable_pick();
    if (next != SIM_NONE) return next;
    if (all_exited()) {
        trace_append(what, SIM_NONE, 0);
        return_to_scheduler();
    }
    /* nothing runnable: advance the clock, or it is a simulated deadlock */
    if (!any_sleeper()) sim_exit_with(SIM_DEADLOCK, what);
    clock_advance_to_earliest_sleeper();
    next = runnable_pick();
    GOO_ASSERT(next != SIM_NONE);
    GOO_ASSERT(g_threads[next].state == SIM_RUNNABLE);
    return next;
}

/* ---- seeded preemption (spec section 6, "Seeded preemption") ---- */

/* One test draw per decision while the model is on, and two more on a hit
 * (preempt_duration_ns). With ppm 0 there is NO draw, so the generator, and
 * so every trace, is what it was before the model existed. */
static bool preempt_drawn(void) {
    GOO_ASSERT(g_preempt_ppm <= SIM_PPM_ONE);
    if (g_preempt_ppm == 0) return false;
    uint32_t draw = sim_random_below(SIM_PPM_ONE);
    GOO_ASSERT(draw < SIM_PPM_ONE);
    return draw < g_preempt_ppm;
}

/* Log-uniform: an exponent e uniform in [MIN, MAX], then a mantissa
 * uniform in [2^e, 2^(e+1)). The bound check can fail: SIM_PREEMPT_NS_MAX
 * is a literal, so an exponent raised past it trips here. */
static uint32_t preempt_duration_ns(void) {
    uint32_t exponent = SIM_PREEMPT_EXPONENT_MIN
        + sim_random_below(SIM_PREEMPT_EXPONENT_MAX - SIM_PREEMPT_EXPONENT_MIN + 1);
    GOO_ASSERT(exponent < 32);
    uint64_t base_ns = 1ull << exponent;
    uint64_t d_ns = base_ns + sim_random_below((uint32_t)base_ns);
    GOO_ASSERT(d_ns >= (1ull << SIM_PREEMPT_EXPONENT_MIN));
    GOO_ASSERT(d_ns <= SIM_PREEMPT_NS_MAX);
    return (uint32_t)d_ns;
}

/* The OS deschedules coroutine id for d: it becomes a sleeper with the
 * deadline now + d, and resumes later from the point where it is parked.
 * That is not a seam call: the program sees only the elapsed time. */
static void preempt(uint32_t id) {
    GOO_ASSERT(id != SIM_NONE);              /* never the real thread */
    GOO_ASSERT(id < g_thread_count);
    GOO_ASSERT(g_threads[id].state == SIM_RUNNABLE);
    uint32_t d_ns = preempt_duration_ns();
    uint64_t wake_at_ns = (d_ns > UINT64_MAX - g_now_ns) ? UINT64_MAX : g_now_ns + d_ns;
    g_threads[id].state = SIM_SLEEP;
    g_threads[id].wake_at_ns = wake_at_ns;
    trace_append("preempt", id, d_ns);
    GOO_ASSERT(g_threads[id].wake_at_ns > g_now_ns);
}

void sim_yield_point(const char* what) {
    GOO_ASSERT(g_initialized);
    GOO_ASSERT(what != nullptr);
    step_begin(what);
    uint32_t next = pick_or_advance(what);
    /* Each preemption is one more decision: it takes a step, so the step
     * limit bounds this loop, and the pick runs again without `next`. */
    for (uint64_t preempt_rounds = 0; preempt_drawn(); preempt_rounds++) {
        GOO_ASSERT(preempt_rounds < g_step_limit);
        preempt(next);
        step_begin(what);
        next = pick_or_advance(what);
    }
    trace_append(what, next, 0);
    switch_to(next);
    GOO_ASSERT(g_current == SIM_NONE || g_threads[g_current].state == SIM_RUNNABLE);
    GOO_ASSERT(g_current == SIM_NONE || g_threads[g_current].state != SIM_EXITED);
}

/* Every coroutine starts here; switch_to set g_current to it first. */
static void thread_trampoline(void) {
    uint32_t id = g_current;
    GOO_ASSERT(id < g_thread_count);
    sim_thread_t* t = &g_threads[id];
    GOO_ASSERT(t->state == SIM_RUNNABLE);
    t->result = t->func(t->arg);
    GOO_ASSERT(g_current == id);
    GOO_ASSERT(t->state == SIM_RUNNABLE);
    t->state = SIM_EXITED;
    (void)sim_wake_all(t, SIM_WAIT_JOIN);
    sim_yield_point("exit");
    sim_exit_with(SIM_INVARIANT, "an exited thread was resumed");
}

/* ---- the public surface ---- */

void sim_init(uint64_t seed, uint64_t step_limit, uint64_t time_limit_ns,
              uint32_t preempt_ppm, const char* trace_dir) {
    GOO_ASSERT(step_limit > 0);
    GOO_ASSERT(time_limit_ns > 0);
    GOO_ASSERT(preempt_ppm <= SIM_PPM_ONE);
    GOO_ASSERT(trace_dir != nullptr);
    size_t dir_bytes = strnlen(trace_dir, SIM_TRACE_DIR_BYTES);
    GOO_ASSERT(dir_bytes > 0);
    GOO_ASSERT(dir_bytes < SIM_TRACE_DIR_BYTES);
    g_initialized = false;
    install_handlers_once();
    g_thread_count = 0;
    g_current = SIM_NONE;
    g_seed = seed;
    /* splitmix64 spreads neighbouring seeds (7, 8) across the state space */
    uint64_t mix = seed + 0x9E3779B97F4A7C15ull;
    mix = (mix ^ (mix >> 30)) * 0xBF58476D1CE4E5B9ull;
    mix = (mix ^ (mix >> 27)) * 0x94D049BB133111EBull;
    g_rng = mix ^ (mix >> 31);
    if (g_rng == 0) g_rng = 0x9E3779B97F4A7C15ull;   /* xorshift's one fixed point */
    g_step = 0;
    g_step_limit = step_limit;
    g_now_ns = 0;
    g_time_limit_ns = time_limit_ns;
    g_preempt_ppm = preempt_ppm;
    g_trace_count = 0;
    g_trace_written = 0;
    g_exit_label = nullptr;
    memcpy(g_trace_dir, trace_dir, dir_bytes);
    g_trace_dir[dir_bytes] = '\0';
    char seed_digits[21];
    uint32_t seed_bytes = fmt_u64(seed_digits, seed);
    GOO_ASSERT(dir_bytes + 1 + seed_bytes + sizeof ".trace" <= SIM_TRACE_PATH_BYTES);
    memcpy(g_trace_path, g_trace_dir, dir_bytes);
    g_trace_path[dir_bytes] = '/';
    memcpy(g_trace_path + dir_bytes + 1, seed_digits, seed_bytes);
    memcpy(g_trace_path + dir_bytes + 1 + seed_bytes, ".trace", sizeof ".trace");
    g_initialized = true;
    GOO_ASSERT(g_rng != 0);
}

uint32_t sim_thread_spawn(void* (*func)(void*), void* arg) {
    GOO_ASSERT(g_initialized);
    GOO_ASSERT(func != nullptr);
    if (g_thread_count >= SIM_THREADS_MAX) sim_exit_with(SIM_LIMIT, "threads");
    uint32_t id = g_thread_count;
    sim_thread_t* t = &g_threads[id];
    t->state = SIM_RUNNABLE;
    t->id = id;
    t->wait_on = nullptr;
    t->wake_at_ns = 0;
    t->func = func;
    t->arg = arg;
    t->result = nullptr;
    memset(&t->tls, 0, sizeof t->tls);
    int rc = getcontext(&t->ctx);
    GOO_ASSERT(rc == 0);
    t->ctx.uc_stack.ss_sp = t->stack;
    t->ctx.uc_stack.ss_size = SIM_STACK_BYTES;
    t->ctx.uc_link = nullptr;   /* the trampoline never returns */
    makecontext(&t->ctx, thread_trampoline, 0);
    g_thread_count++;
    GOO_ASSERT(g_thread_count <= SIM_THREADS_MAX);
    return id;
}

sim_thread_t* sim_current(void) {
    GOO_ASSERT(g_current != SIM_NONE);
    GOO_ASSERT(g_current < g_thread_count);
    return &g_threads[g_current];
}

static const char* block_label(enum sim_state why) {
    switch (why) {
    case SIM_WAIT_COND:  return "wait_cond";
    case SIM_WAIT_MUTEX: return "wait_mutex";
    case SIM_WAIT_JOIN:  return "wait_join";
    case SIM_RUNNABLE:
    case SIM_SLEEP:
    case SIM_EXITED:     break;
    }
    return nullptr;
}

void sim_block(enum sim_state why, const void* on) {
    const char* label = block_label(why);
    GOO_ASSERT(label != nullptr);            /* only the three wait states park here */
    GOO_ASSERT(on != nullptr);
    sim_thread_t* self = sim_current();
    GOO_ASSERT(self->state == SIM_RUNNABLE);
    self->state = why;
    self->wait_on = on;
    sim_yield_point(label);
    GOO_ASSERT(self->state == SIM_RUNNABLE);
    GOO_ASSERT(self->wait_on == nullptr);
}

static uint32_t collect_waiters(const void* on, enum sim_state why, uint32_t* out) {
    GOO_ASSERT(on != nullptr);
    GOO_ASSERT(block_label(why) != nullptr);
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_thread_count; i++) {
        if (g_threads[i].state == why && g_threads[i].wait_on == on) {
            GOO_ASSERT(n < SIM_WAITERS_MAX);
            out[n++] = i;
        }
    }
    GOO_ASSERT(n <= g_thread_count);
    return n;
}

uint32_t sim_wake_one(const void* on, enum sim_state why) {
    uint32_t waiters[SIM_WAITERS_MAX];
    uint32_t n = collect_waiters(on, why, waiters);
    if (n == 0) return 0;
    uint32_t id = waiters[sim_random_below(n)];
    make_runnable(id);
    trace_append("wake_one", id, 0);
    GOO_ASSERT(g_threads[id].state == SIM_RUNNABLE);
    GOO_ASSERT(g_threads[id].wait_on == nullptr);
    return 1;
}

uint32_t sim_wake_all(const void* on, enum sim_state why) {
    uint32_t waiters[SIM_WAITERS_MAX];
    uint32_t n = collect_waiters(on, why, waiters);
    GOO_ASSERT(n <= g_thread_count);
    for (uint32_t i = 0; i < n; i++) {
        make_runnable(waiters[i]);
    }
    GOO_ASSERT(collect_waiters(on, why, waiters) == 0);
    return n;
}

void sim_sleep_until(uint64_t wake_at_ns) {
    sim_thread_t* self = sim_current();
    GOO_ASSERT(self->state == SIM_RUNNABLE);
    GOO_ASSERT(self->state != SIM_EXITED);
    if (wake_at_ns > g_now_ns) {
        self->state = SIM_SLEEP;
        self->wake_at_ns = wake_at_ns;
    }
    sim_yield_point("sleep");
    GOO_ASSERT(self->state == SIM_RUNNABLE);
    GOO_ASSERT(g_now_ns >= wake_at_ns);
}

uint64_t sim_now_ns(void) {
    GOO_ASSERT(g_initialized);
    GOO_ASSERT(g_now_ns <= g_time_limit_ns);
    return g_now_ns;
}

/* _exit runs no atexit handler and flushes no stdio buffer, so the flush
 * comes first: without it the program's output is lost on every verdict.
 * This is never called from a signal handler, so fflush is safe here. */
void sim_exit_with(enum sim_verdict verdict, const char* detail) {
    const char* label = verdict_label(verdict);
    GOO_ASSERT(label != nullptr);
    GOO_ASSERT(detail != nullptr);
    GOO_ASSERT(g_initialized);
    if (fflush(nullptr) != 0) {
        static const char msg[] = "goo-sim: flushing the program's stdio failed; its output is incomplete\n";
        (void)!write(STDERR_FILENO, msg, sizeof msg - 1);
    }
    trace_write(label, detail);
    _exit((int)verdict);
}

void sim_exit_program(int status) {
    GOO_ASSERT(g_initialized);
    GOO_ASSERT(g_current != SIM_NONE);   /* the exit path must run where a seam call is legal */
    GOO_ASSERT(g_exit_label == nullptr); /* main returns once */
    g_exit_label = (status == 0) ? "pass" : "program-exit";
    exit(status);
}

void sim_run_until_done(void) {
    GOO_ASSERT(g_initialized);
    GOO_ASSERT(g_current == SIM_NONE);
    GOO_ASSERT(g_thread_count > 0);
    sim_yield_point("start");
    GOO_ASSERT(g_current == SIM_NONE);
    GOO_ASSERT(all_exited());
}
