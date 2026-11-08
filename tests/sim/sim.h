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

#define SIM_THREADS_MAX    17u      /* GOO_MAX_OS_THREADS (16) workers + main */
#define SIM_STACK_BYTES    262144u
#define SIM_WAITERS_MAX    17u
#define SIM_MUTEXES_MAX    64u
#define SIM_TRACE_ENTRIES  65536u

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
    /* Task 5 adds the goo_worker_tls_t copy here. */
} sim_thread_t;

/* trace_dir must be non-NULL and shorter than 256 bytes; the trace goes to
 * <trace_dir>/<seed>.trace. */
void      sim_init(uint64_t seed, uint64_t step_limit, uint64_t time_limit_ns, const char* trace_dir);
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
[[noreturn]] void sim_exit_with(enum sim_verdict verdict, const char* detail);   /* writes the trace, never returns */
void      sim_run_until_done(void);              /* Task 5's main loop: returns when every thread exited */
#endif
