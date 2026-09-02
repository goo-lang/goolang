/*
 * deadlock_detector.pml — a Promela model of the Goo runtime's deadlock
 * detector. ONE file, TWO protocols, selected by FIX:
 *
 *   FIX = 0   the detector AS IT IS TODAY (2026-09-02, branch
 *             build/bazel-phase4-probes). Task 5 proved it aborts a live
 *             program for every finite threshold.
 *   FIX = 1   the CORRECTED protocol. The blocked count is a SET of parked
 *             goroutines, not a count of block_begin calls. Every wake is a
 *             BROADCAST, so the set woken is the whole waiter set, and the
 *             broadcaster removes exactly that set from the blocked set, under
 *             the scheduler mutex. goo_sched_block_end no longer touches the
 *             count.
 *
 *             The broadcast is not decoration. pthread_cond_signal does not
 *             say WHICH waiter the OS woke, so no signal site can pair itself
 *             with the removal of one named waiter. See do_send.
 *
 * The two protocols live in one file on purpose: a second file would drift.
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
 *   abort of a live program. The claim is the SAME for both protocols.
 *
 * Parameters (set with spin -D... -a):
 *   N          goroutines, 2..4          (default 2)
 *   K          asleep_streak threshold   (default 3; the C code uses 100)
 *   DETECTOR   1 = detector on, 0 = detector never aborts (positive control)
 *   FIX        0 = the protocol as it is, 1 = the corrected protocol
 *   CAP        channel buffer capacity   (default 1)
 *   OPS        channel operations per goroutine (default 2)
 *
 * Teeth. Each of these is ONE change to the FIX=1 protocol and each must make
 * pan report an error. They exist so a reader can see the proof is not vacuous.
 *   DOUBLE_DEC 1  block_end decrements as well as the signaller. The count
 *                 then goes below zero. FIX=1 only.
 *   BCAST_ONE  1  a broadcast decrements by ONE however many waiters it woke,
 *                 instead of by the number woken. FIX=1 only.
 *
 * Spurious wakeups.
 *   SPURIOUS   1  pthread_cond_wait may return with no signal, which POSIX
 *                 permits. Every wait in channels.c sits inside a while loop
 *                 that re-checks the predicate and waits again (channels.c:280,
 *                 :336, :359, :425, :484), so the model re-checks too.
 *   END_UNREG  1  block_end unregisters this goroutine IF it is still in the
 *                 set. This is the amendment a spurious wakeup needs; with
 *                 SPURIOUS=0 it is dead code, and the SPURIOUS=0 runs prove
 *                 that with assert(counted[_pid] == 0) inside block_end.
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

#ifndef FIX
#define FIX 0
#endif

#ifndef DOUBLE_DEC
#define DOUBLE_DEC 0
#endif

#ifndef BCAST_ONE
#define BCAST_ONE 0
#endif

#ifndef SPURIOUS
#define SPURIOUS 0
#endif

#ifndef END_UNREG
#define END_UNREG 0
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

#if FIX
/* FIX=1 only. counted[i] is goroutine i's membership of the blocked SET, and
 * nblocked is the size of that set. The C has no such array today. The fix
 * needs one bit for each goroutine AND a waiter LIST on each condvar: the
 * broadcaster must clear the bit of each waiter it wakes, and a bare waiter
 * COUNT does not give it their identities. See the README section "The
 * C-level fix", finding 2. This array does not exist under FIX=0, so the
 * FIX=0 state vector is byte for byte the one task 5 measured. */
bit counted[N];
#endif

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
 *
 * FIX=0 keeps that as a TALLY of block_begin calls, which is what the C ships
 * and what task 5 broke. FIX=1 makes nblocked the SIZE OF THE SET counted[],
 * and moves the removal to the signaller. Both use the same lock and the same
 * order; only the arithmetic and the place of the decrement differ.
 */
#if FIX

/* FIX=1: registering is IDEMPOTENT. The count is the size of the blocked set,
 * so a second call with the bit already set adds nothing. Without this a
 * spurious return that re-waits would count the same goroutine twice. */
inline block_begin() {
    atomic { schmx == 0 -> schmx = 1;
             if
             :: counted[_pid] == 0 -> counted[_pid] = 1; nblocked++
             :: else -> skip
             fi;
             schmx = 0 }
}

#if DOUBLE_DEC
/* TEETH mutant, FIX=1 only: the wake-time decrement is put back BESIDE the
 * signal-time one. Both fire for one wake, so the count goes below zero. The
 * assertion is the never-below-zero rule, and it is what must break. */
inline block_end() {
    atomic { schmx == 0 -> schmx = 1;
             assert(nblocked > 0);
             nblocked--;
             schmx = 0 }
}
#elif END_UNREG
/* The amendment a spurious wakeup needs: leave the set only if still in it.
 * This can never drive the count below zero, and it is a no-op whenever the
 * signaller got there first. */
inline block_end() {
    atomic { schmx == 0 -> schmx = 1;
             if
             :: counted[_pid] == 1 -> counted[_pid] = 0;
                                      assert(nblocked > 0);
                                      nblocked--
             :: else -> skip
             fi;
             schmx = 0 }
}
#else
/* The corrected protocol as specified: block_end does not touch the count.
 * The assertion turns "it does not need to" into a THEOREM of the model: on
 * every reachable path the signaller has already cleared this goroutine's
 * bit. Under SPURIOUS=1 that theorem is false, and this assertion says so. */
inline block_end() {
    atomic { schmx == 0 -> schmx = 1;
             assert(counted[_pid] == 0);
             schmx = 0 }
}
#endif

#else

inline block_begin() {
    atomic { schmx == 0 -> schmx = 1; nblocked++; schmx = 0 }
}

inline block_end() {
    atomic { schmx == 0 -> schmx = 1;
             if :: nblocked > 0 -> nblocked-- :: else -> skip fi;
             schmx = 0 }
}

#endif

/* ---- condition variables ---- */

/* pthread_cond_signal: wake exactly one waiter on cv, if any waits. The
 * choice of which waiter is nondeterministic. Called with the channel mutex
 * held, exactly as goo_chan_send() calls it.
 *
 * FIX=0 ONLY. Under FIX=1 do_send broadcasts instead, and nothing calls this
 * inline. The reason is at the call site: the choice of waiter here is the
 * model MAKING a choice that POSIX leaves to the OS, so a decrement paired
 * with it would assume the answer. The FIX=1 decrement lives in
 * cond_broadcast, where the set woken is the whole waiter set. */
inline cond_signal(cv) {
    atomic {
#if FIX
        /* The caller already holds the CHANNEL mutex. Take the SCHEDULER mutex
         * as well, which keeps the channel-then-scheduler order that
         * block_begin uses, so the fix adds no new lock order. Taking the lock
         * is the FIRST statement of the atomic sequence, which makes it the
         * sequence's guard: if the mutex is busy the sequence is simply not
         * enabled, so no scratch variable is ever left half written. */
        schmx == 0 -> schmx = 1;
#endif
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
                       :: sg_c == sg_t ->
#if FIX
                            /* the woken waiter leaves the blocked set HERE,
                             * not when it later returns from the wait */
                            assert(counted[sg_i] == 1);
                            assert(nblocked > 0);
                            counted[sg_i] = 0;
                            nblocked--;
#endif
                            gstate[sg_i] = G_SIGNALLED; sg_i = N
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
#if FIX
        ; schmx = 0
#endif
    }
}

/* pthread_cond_broadcast: wake every waiter on cv. goo_chan_recv() already
 * broadcasts not_full because two predicate classes share that condvar
 * (the comments at channels.c:466-473 and :499-504 give the reason).
 *
 * Under FIX=1 this is the ONLY wake primitive: do_send broadcasts not_empty
 * too. That is what makes the set woken equal the set unregistered.
 *
 * A broadcast can wake more than one waiter, so under FIX=1 it must decrement
 * by the NUMBER it woke. BCAST_ONE=1 is the mutant that decrements by one. */
inline cond_broadcast(cv) {
    d_step {
#if FIX
        schmx == 0 -> schmx = 1;
        sg_n = 0;
#endif
        sg_i = 0;
        do
        :: sg_i < N ->
             if
             :: (gstate[sg_i] == G_WAITING && wcv[sg_i] == cv) ->
                  gstate[sg_i] = G_SIGNALLED
#if FIX
                  ; assert(counted[sg_i] == 1);
                  counted[sg_i] = 0;
                  sg_n++
#endif
             :: else -> skip
             fi;
             sg_i++
        :: else -> break
        od;
        sg_i = 0
#if FIX
        ;
#if BCAST_ONE
        /* TEETH mutant: one decrement, however many waiters woke. */
        if
        :: sg_n > 0 -> assert(nblocked > 0); nblocked--
        :: else -> skip
        fi;
#else
        assert(nblocked >= sg_n);
        nblocked = nblocked - sg_n;
#endif
        sg_n = 0;
        schmx = 0
#endif
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
#if SPURIOUS
    /* POSIX lets pthread_cond_wait return with no signal at all. The second
     * branch is that return: the goroutine is still G_WAITING and no signaller
     * touched the blocked set. Every wait in channels.c is inside a while loop
     * that re-checks the predicate (channels.c:280, :336, :359, :425, :484),
     * so the caller re-checks and waits again. */
    if
    :: (gstate[_pid] == G_SIGNALLED || prog_exit) -> skip
    :: skip
    fi;
#else
    (gstate[_pid] == G_SIGNALLED || prog_exit);
#endif
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
#if FIX
    /* 2026-09-02 review, finding 1. Under FIX=1 this site BROADCASTS, where
     * the C ships a pthread_cond_signal (channels.c:325, and :356 and :543 on
     * the paths this model does not carry).
     *
     * The reason is that pthread_cond_signal gives the caller no way to learn
     * WHICH waiter the OS woke. A signal paired with "clear the bit of the one
     * waiter this will wake" assumes an answer POSIX does not give: if the C
     * clears X's bit and the OS wakes Y, then Y is awake and still registered,
     * which is the shipped defect again. A broadcast removes the question,
     * because the set woken is then the whole waiter set, which is exactly the
     * set the signaller unregisters. */
    cond_broadcast(CV_NOT_EMPTY);
#else
    cond_signal(CV_NOT_EMPTY);
#endif
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
