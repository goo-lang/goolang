/* tests/sim/sim.h — the deterministic simulator's scheduler, virtual clock
 * and trace ring (spec: docs/superpowers/specs/2026-09-03-deterministic-
 * simulation-design.md, section 6). Every simulated OS thread is a ucontext
 * coroutine on ONE real thread; every seam call ends in sim_yield_point(),
 * where a seeded xorshift64* picks the next runnable coroutine. All storage
 * is static and sized by the constants below: nothing is allocated after
 * sim_init(). */
#ifndef GOO_SIM_H
#define GOO_SIM_H
#include <stdint.h>
#include <ucontext.h>

#include "goo_os.h"         /* goo_worker_tls_t: each coroutine holds its own copy */

#define SIM_THREADS_MAX    17u      /* GOO_MAX_OS_THREADS (16) workers + main */
#define SIM_STACK_BYTES    262144u
#define SIM_WAITERS_MAX    17u
#define SIM_MUTEXES_MAX    16384u   /* live mutexes; fanin_stress needs 8,001 (Task 7 fix round 1) */
#define SIM_TRACE_ENTRIES  65536u

/* Seeded preemption (spec section 6, "Seeded preemption"): at each
 * scheduling decision the seed deschedules the picked coroutine with
 * probability ppm / SIM_PPM_ONE, for a log-uniform d: an exponent e uniform
 * in [SIM_PREEMPT_EXPONENT_MIN, SIM_PREEMPT_EXPONENT_MAX], then d uniform in
 * [2^e, 2^(e+1)) ns, so about 1 us to 16.8 ms. Fixed by the Task 6a brief
 * before any measurement. SIM_PREEMPT_NS_MAX is written out, not derived
 * from the exponent, so that a raised exponent trips the bound assertion
 * at run time instead of moving the bound with it. */
#define SIM_PPM_ONE               1000000u
#define SIM_PREEMPT_PPM_DEFAULT   15625u    /* 1 in 64 decisions; GOO_SIM_PREEMPT_PPM=0 is off */
#define SIM_PREEMPT_EXPONENT_MIN  10u
#define SIM_PREEMPT_EXPONENT_MAX  23u
#define SIM_PREEMPT_NS_MAX        16777215u /* 2^(SIM_PREEMPT_EXPONENT_MAX + 1) - 1 */
/* With the model on (ppm > 0) each scheduling decision costs this much
 * virtual time, and every sleeper due by then is runnable before the pick.
 * Without it a coroutine that spins through seam calls holds the clock
 * still, and a sleeper or a preempted coroutine never resumes (fix round 1,
 * I1). Off at ppm 0, so a ppm-0 trace keeps its pre-model bytes. */
#define SIM_STEP_COST_NS          1000u

enum sim_verdict {
    SIM_PASS = 0, SIM_RUNTIME_DEADLOCK = 2, SIM_DEADLOCK = 10, SIM_INVARIANT = 11,
    SIM_STEP_LIMIT = 12, SIM_TIME_LIMIT = 13, SIM_REPLAY_MISMATCH = 14, SIM_LIMIT = 15,
};
enum sim_state { SIM_RUNNABLE, SIM_WAIT_COND, SIM_WAIT_MUTEX, SIM_WAIT_JOIN, SIM_SLEEP, SIM_EXITED };

typedef struct sim_thread {
    ucontext_t       ctx;
    enum sim_state   state;
    uint32_t         id;
    const void*      wait_on;        /* the cond, mutex, or thread it waits for */
    uint64_t         wake_at_ns;     /* SIM_SLEEP only */
    void*          (*func)(void*);
    void*            arg;
    void*            result;
    unsigned char    stack[SIM_STACK_BYTES];
    goo_worker_tls_t tls;            /* goo_os_tls() of this coroutine; zeroed at spawn */
} sim_thread_t;

/* trace_dir must be non-NULL and shorter than 256 bytes; the trace goes to
 * <trace_dir>/<seed>.trace. preempt_ppm is at most SIM_PPM_ONE; 0 turns the
 * preemption model off, and then the run draws exactly the random numbers
 * it drew before the model existed. */
void      sim_init(uint64_t seed, uint64_t step_limit, uint64_t time_limit_ns,
                   uint32_t preempt_ppm, const char* trace_dir);
/* Returns the new thread's id. A full table is the verdict SIM_LIMIT
 * ("limit-threads"): sim_exit_with, never a return. Slots are never reused,
 * so an exited thread's result stays readable for a join. */
uint32_t  sim_thread_spawn(void* (*func)(void*), void* arg);
sim_thread_t* sim_current(void);
void      sim_yield_point(const char* what);      /* the scheduling point; every seam call ends here */
void      sim_block(enum sim_state why, const void* on);   /* park the current thread, then switch */
uint32_t  sim_wake_one(const void* on, enum sim_state why);   /* seed-chosen waiter; returns count woken (0 or 1) */
uint32_t  sim_wake_all(const void* on, enum sim_state why);
void      sim_sleep_until(uint64_t wake_at_ns);
uint64_t  sim_now_ns(void);
uint32_t  sim_random_below(uint32_t n);          /* seeded; n > 0 */
/* Flushes stdio, writes the trace, then _exit: a verdict never runs the
 * atexit handlers, but the program's output survives it. */
[[noreturn]] void sim_exit_with(enum sim_verdict verdict, const char* detail);
void      sim_run_until_done(void);              /* Task 5's main loop: returns when every thread exited */
/* The program's main returned status. Ends the process the way C does,
 * exit(status) on the calling coroutine, so the program's atexit handlers
 * and the stdio flush run where a seam call is legal, never on the real
 * thread. The trace header names verdict=pass for 0, program-exit for any
 * other status. Call only from a coroutine. */
[[noreturn]] void sim_exit_program(int status);

/* tests/sim/sim_os.c, the seam over this scheduler. Faults are off until
 * this call turns one on; sim_main.c reads them from the environment. */
void      sim_os_set_faults(bool spurious_wake, bool thread_create_eagain);
#endif
