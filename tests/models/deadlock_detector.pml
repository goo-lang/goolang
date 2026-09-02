/*
 * deadlock_detector.pml — a Promela model of the Goo runtime's deadlock
 * detector AS IT IS TODAY (2026-09-02, branch build/bazel-phase4-probes).
 *
 * Modelled C code
 * ---------------
 *   src/runtime/concurrency.c  goo_scheduler_wait()        (the poll loop)
 *   src/runtime/concurrency.c  goo_sched_block_begin()     (blocked_goroutines++)
 *   src/runtime/concurrency.c  goo_sched_block_end()       (blocked_goroutines--)
 *   src/runtime/concurrency.c  scheduler_get_next_goroutine() / goo_goroutine_exit()
 *   src/runtime/channels.c     goo_chan_send() / goo_chan_recv(), buffered path
 *   src/runtime/deadlock.c     goo_deadlock_abort()        (exit(2))
 *
 * The claim
 * ---------
 *   At the abort point, real_deadlock holds:
 *     every live goroutine is WAITING, none is SIGNALLED, and the ready
 *     queue is empty.
 *   assert(rd) at the abort point is the whole claim. A counterexample is an
 *   abort of a live program.
 *
 * Parameters (set with spin -D... -a):
 *   N         goroutines, 2..4          (default 2)
 *   K         asleep_streak threshold   (default 3; the C code uses 100)
 *   DETECTOR  1 = detector on, 0 = detector never aborts (positive control)
 *   CAP       channel buffer capacity   (default 1)
 *   OPS       channel operations per goroutine (default 2)
 *
 * Goroutine i sends when i is even and receives when i is odd.
 */

#ifndef N
#define N 2
#endif

#ifndef K
#define K 3
#endif

#ifndef DETECTOR
#define DETECTOR 1
#endif

#ifndef CAP
#define CAP 1
#endif

#ifndef OPS
#define OPS 2
#endif

/* Goroutine states. G_SIGNALLED has no counterpart in the C code: it is the
 * pthread condvar state "signalled, has not yet reacquired the channel mutex
 * and returned from pthread_cond_wait". The C code cannot see it, and that is
 * the point of the model. */
#define G_READY     0   /* in ready_queue, no worker has popped it yet */
#define G_RUNNING   1
#define G_WAITING   2   /* parked in pthread_cond_wait, not signalled */
#define G_SIGNALLED 3   /* signalled, not yet running */
#define G_DONE      4

#define CV_NOT_FULL  0
#define CV_NOT_EMPTY 1

/* ---- shared state ---- */

byte gstate[N];         /* goroutine->state, plus the condvar-internal G_SIGNALLED */
bit  wcv[N];            /* which condvar a G_WAITING goroutine parks on */

byte nlive   = N;       /* g_scheduler->stats.num_goroutines */
byte nblocked = 0;      /* g_scheduler->deadlock_detector.blocked_goroutines */
byte readyq  = N;       /* length of g_scheduler->ready_queue; 0 means NULL */
byte chan_len = 0;      /* ch->length */

bit chmx  = 0;          /* ch->mutex */
bit schmx = 0;          /* g_scheduler->scheduler_mutex */

bit main_in_wait = 0;   /* deadlock_detector.main_in_wait; the C never reads it */
bit prog_exit = 0;      /* goo_deadlock_abort() -> exit(2) kills every thread */

/* scratch for the condvar inlines; always reset to 0 so the state vector
 * carries no residue */
byte sg_i = 0;
byte sg_n = 0;
byte sg_t = 0;
byte sg_c = 0;

/* ---- mutexes ---- */

inline lock(m)   { atomic { m == 0 -> m = 1 } }
inline unlock(m) { m = 0 }

/* ---- the scheduler-mutex block accounting ----
 *
 * goo_sched_block_begin(): the caller holds the CHANNEL mutex, takes the
 * SCHEDULER mutex, increments, releases the scheduler mutex. Lock order is
 * channel-then-scheduler. main takes the scheduler mutex only, so it can
 * observe the incremented counter while the caller still holds the channel
 * mutex and has not yet parked.
 */
inline block_begin() {
    atomic { schmx == 0 -> schmx = 1; nblocked++; schmx = 0 }
}

inline block_end() {
    atomic { schmx == 0 -> schmx = 1;
             if :: nblocked > 0 -> nblocked-- :: else -> skip fi;
             schmx = 0 }
}

/* ---- condition variables ---- */

/* pthread_cond_signal: wake exactly one waiter on cv, if any waits. The
 * choice of which waiter is nondeterministic. Called with the channel mutex
 * held, exactly as goo_chan_send() calls it. */
inline cond_signal(cv) {
    atomic {
        sg_n = 0;
        sg_i = 0;
        do
        :: sg_i < N ->
             if
             :: (gstate[sg_i] == G_WAITING && wcv[sg_i] == cv) -> sg_n++
             :: else -> skip
             fi;
             sg_i++
        :: else -> break
        od;
        if
        :: sg_n > 0 ->
             /* pthread_cond_signal does not say WHICH waiter wakes. Choose an
              * ordinal in 0 .. sg_n-1 nondeterministically. The choice loop is
              * bounded by N, so the atomic sequence always terminates. */
             sg_t = 0;
             do
             :: sg_t < sg_n - 1 -> sg_t++
             :: break
             od;
             sg_i = 0;
             sg_c = 0;
             do
             :: sg_i < N ->
                  if
                  :: (gstate[sg_i] == G_WAITING && wcv[sg_i] == cv) ->
                       if
                       :: sg_c == sg_t -> gstate[sg_i] = G_SIGNALLED; sg_i = N
                       :: else -> sg_c++; sg_i++
                       fi
                  :: else -> sg_i++
                  fi
             :: else -> break
             od
        :: else -> skip
        fi;
        sg_i = 0;
        sg_n = 0;
        sg_t = 0;
        sg_c = 0
    }
}

/* pthread_cond_broadcast: wake every waiter on cv. goo_chan_recv() broadcasts
 * not_full because two predicate classes share that condvar. */
inline cond_broadcast(cv) {
    d_step {
        sg_i = 0;
        do
        :: sg_i < N ->
             if
             :: (gstate[sg_i] == G_WAITING && wcv[sg_i] == cv) ->
                  gstate[sg_i] = G_SIGNALLED
             :: else -> skip
             fi;
             sg_i++
        :: else -> break
        od;
        sg_i = 0
    }
}

/* pthread_cond_wait(cv, chmx), given that gstate[_pid] is already G_WAITING
 * and wcv[_pid] is already cv (the C sets self->state = GOO_GOROUTINE_BLOCKED
 * before it calls goo_sched_block_begin(), while it still holds the channel
 * mutex, so no signaller can run in between).
 *
 * Releasing the mutex and parking is one atomic step, as POSIX requires.
 * Coming back is TWO steps: the condvar marks the waiter G_SIGNALLED, and the
 * waiter reacquires the channel mutex only when the OS scheduler runs it.
 * Promela's interleaving gives that second step no bound, which is exactly the
 * "wake interval has no limit" the model is here to test.
 */
inline cond_wait_park() {
    unlock(chmx);
    (gstate[_pid] == G_SIGNALLED || prog_exit);
    if
    :: prog_exit -> goto dead
    :: else -> lock(chmx)
    fi
}

/* ---- the channel, buffered path (ch->capacity > 0), no close ---- */

inline do_send() {
    lock(chmx);
    do
    :: chan_len >= CAP ->
         d_step { gstate[_pid] = G_WAITING; wcv[_pid] = CV_NOT_FULL };
         block_begin();
         cond_wait_park();
         block_end();
         gstate[_pid] = G_RUNNING
    :: else -> break
    od;
    chan_len++;
    cond_signal(CV_NOT_EMPTY);
    unlock(chmx)
}

inline do_recv() {
    lock(chmx);
    do
    :: chan_len == 0 ->
         d_step { gstate[_pid] = G_WAITING; wcv[_pid] = CV_NOT_EMPTY };
         block_begin();
         cond_wait_park();
         block_end();
         gstate[_pid] = G_RUNNING
    :: else -> break
    od;
    chan_len--;
    cond_broadcast(CV_NOT_FULL);
    unlock(chmx)
}

/* ---- the goroutines ---- */

active [N] proctype Goroutine()
{
    byte ops = 0;

    assert(_pid < N);       /* instrument check: the pid is the goroutine index */

    /* scheduler_get_next_goroutine(): pop under the scheduler mutex, then set
     * the state. Two steps, as in the C. */
    atomic { schmx == 0 -> schmx = 1; readyq--; schmx = 0 };
    gstate[_pid] = G_RUNNING;

    do
    :: ops < OPS ->
         if
         :: (_pid % 2) == 0 -> do_send()
         :: else -> do_recv()
         fi;
         ops++
    :: else -> break
    od;

    /* goo_goroutine_exit(): state first, then the counter under the mutex. */
    gstate[_pid] = G_DONE;
    atomic { schmx == 0 -> schmx = 1; nlive--; schmx = 0 };

dead:
    skip
}

/* ---- main: goo_scheduler_wait() ---- */

/* real_deadlock: every live goroutine is WAITING, none is SIGNALLED (nor
 * READY nor RUNNING), and the ready queue is empty.
 *
 * "live" is read off gstate, not off nlive: goo_goroutine_exit() sets
 * goroutine->state = DONE one step before it decrements num_goroutines, and a
 * poll that lands in that window would otherwise satisfy "every live goroutine
 * is WAITING" vacuously with every goroutine already DONE. sg_c counts the
 * goroutines that are not DONE, and the model demands at least one. That test
 * only ever makes real_deadlock HARDER to satisfy, so it strengthens the
 * claim; it never excuses an abort. */
inline compute_real_deadlock(res) {
    d_step {
        res = 1;
        sg_i = 0;
        sg_c = 0;
        do
        :: sg_i < N ->
             if
             :: gstate[sg_i] == G_DONE -> skip
             :: gstate[sg_i] == G_WAITING -> sg_c++
             :: else -> sg_c++; res = 0
             fi;
             sg_i++
        :: else -> break
        od;
        if :: readyq != 0 -> res = 0 :: else -> skip fi;
        if :: nlive  == 0 -> res = 0 :: else -> skip fi;
        if :: sg_c   == 0 -> res = 0 :: else -> skip fi;
        sg_i = 0;
        sg_c = 0
    }
}

active proctype Main()
{
    byte streak = 0;
    bit done_f = 0;
    bit all_asleep = 0;
    bit rd = 0;

    assert(_pid == N);      /* instrument check: Main is the last process */

    atomic { schmx == 0 -> schmx = 1; main_in_wait = 1; schmx = 0 };

    do
    ::  /* one poll. The 0.5 ms sleep is not modelled: time is not in the
         * model, so a poll is a step main takes whenever it is scheduled. */
        atomic {
            schmx == 0 -> schmx = 1;
            done_f = (nlive == 0 && readyq == 0);
            all_asleep = (nlive > 0 && nblocked == nlive && readyq == 0);
            /* model invariants, checked on every poll under the same mutex the
             * C reads these fields under */
            assert(nblocked <= nlive);
            assert(chan_len <= CAP);
            assert(readyq <= N);
            schmx = 0
        };

        if
        :: all_asleep ->
             /* clamped so the state space stays finite when DETECTOR is 0 */
             if :: streak < K -> streak++ :: else -> skip fi
        :: else -> streak = 0
        fi;

#if DETECTOR
        if
        :: streak >= K ->
             compute_real_deadlock(rd);
             assert(rd);            /* THE CLAIM */
             prog_exit = 1;         /* goo_deadlock_abort() -> exit(2) */
             goto stopped
        :: else -> skip
        fi;
#endif

        if
        :: done_f -> break
        :: else -> skip
        fi
    od;

stopped:
    skip
}
