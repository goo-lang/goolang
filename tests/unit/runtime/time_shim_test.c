// time.Sleep / time.Now / time.After: the runtime shim behind Go's time
// package (time_shim.c). Three exported functions, and none of them had a
// unit suite before this one -- the only prior coverage was whatever a full
// Goo program happened to exercise at the golden-fixture level.
//
// TIME MAKES TESTS FLAKY, so the rule here is stricter than in most suites:
// every check below asserts an ORDER (this reading came no earlier than
// that one) or a wide BOUND, never an exact duration. Every requested sleep
// is at most 15ms, and every lower bound leaves generous room below it so a
// loaded machine still passes.
//
// ORDERING IS DELIBERATE. goo_time_unix_ns is verified FIRST (rows 0-1),
// because every later row uses it as the stopwatch for goo_time_sleep_ns and
// goo_time_after. goo_time_sleep_ns is verified SECOND (rows 2-3), because
// the goo_time_after poll helper below uses it to pace its own retries. Only
// after both primitives are trusted do rows 4-6 turn to goo_time_after.
//
// WHY THE POLL HELPER CANNOT HANG. goo_time_after hands back a channel that
// a background goroutine sends to later. goo_chan_recv would block forever
// if that goroutine never sends, and goo_chan_recv_timeout is not a real
// escape hatch here -- channels.c's own comment says it "for now, just
// tr[ies] non-blocking operation", i.e. it IS goo_chan_try_recv, TODO and
// all. So every wait below is a hand-rolled loop over goo_chan_try_recv,
// capped at POLL_MAX_ITERS attempts. A timer that never fires exhausts the
// budget, fails the check that asked for the value, and returns like any
// other row -- it can never keep the process from reaching goo_check_done.
//
// WHAT THIS DOES NOT PIN. Exact sleep duration (rule above), and the
// internal shape time.Time gets from the type checker (a one-field struct of
// one int64) -- this suite can only confirm the channel's element is 8
// bytes, via goo_chan_try_recv succeeding into an int64_t, not that the
// checker built that struct. See the report for the one branch in
// time_shim.c that this suite cannot reach at all: goo_go's failure path.

#include "runtime.h"
#include "../goo_check.h"

// A wall-clock reading (CLOCK_REALTIME nanoseconds since 1970-01-01) must
// fall between these two, comfortably far from what a MONOTONIC clock (which
// counts from an arbitrary point, often boot) would read instead. 2020 and
// 2100 in epoch nanoseconds -- a margin of decades either side of "now".
#define YEAR_2020_NS ((int64_t)1577836800000000000LL)
#define YEAR_2100_NS ((int64_t)4102444800000000000LL)

// A non-positive Sleep/After must return well under this. 200ms is roughly
// 10,000x a syscall's normal cost -- loose enough that scheduler noise on a
// busy CI box cannot trip it.
#define PROMPT_BOUND_NS ((int64_t)200000000LL)

// The one duration this suite actually asks the runtime to wait for.
// Capped at 15ms per the anti-flakiness rule (at most 20ms).
#define SHORT_SLEEP_NS ((int64_t)15000000LL)

// A negative duration used to probe the ns<=0 guard. Any negative value
// exercises the same cast-to-uint64 hazard the guard exists to prevent
// (see time_shim.c's comment on goo_time_sleep_ns), so the exact magnitude
// does not matter.
#define NEGATIVE_NS ((int64_t)(-5000000LL))

// A real sleep/fire must take at least this long -- far below the 15ms
// requested, but clearly more than "instant". Catches a mutation that fires
// or wakes immediately while ignoring the duration.
#define LOWER_BOUND_NS ((int64_t)3000000LL)

// Nothing this suite requests should ever take this long. Catches a
// unit-scale bug (e.g. treating nanoseconds as milliseconds).
#define UPPER_SANITY_NS ((int64_t)2000000000LL)

// Poll pacing for goo_time_after's channel: 2ms between tries, up to 300
// tries -- a 600ms budget, about 40x the 15ms duration this suite ever
// requests. Uses goo_time_sleep_ns itself, which rows 2-3 already prove
// returns promptly for a small positive duration, so the poll makes steady
// forward progress rather than busy-spinning.
#define POLL_INTERVAL_NS ((int64_t)2000000LL)
#define POLL_MAX_ITERS 300

// Waits for one value on ch, retrying goo_chan_try_recv until it succeeds or
// the budget above runs out. Returns 1 and fills *out on success, 0 on
// exhaustion -- never blocks past that fixed number of iterations.
static int poll_recv(goo_channel_t* ch, int64_t* out) {
    for (int i = 0; i < POLL_MAX_ITERS; i++) {
        if (goo_chan_try_recv(ch, out)) {
            return 1;
        }
        goo_time_sleep_ns(POLL_INTERVAL_NS);
    }
    return 0;
}

int main(void) {
    goo_check_expect(7);

    // -------------------------------------------------------------------
    // goo_time_unix_ns takes no argument, so there is no NULL/zero case for
    // it -- but every later row leans on it as a stopwatch, so its own
    // contract is checked first: it must read the WALL clock, not the
    // MONOTONIC one goo_platform_time_ns exposes internally.
    goo_check_row(0, "goo_time_unix_ns reads the wall clock, not a boot-relative one");
    int64_t now = goo_time_unix_ns();
    goo_check(now > YEAR_2020_NS, "unix_ns() reading is after 2020");
    goo_check(now < YEAR_2100_NS, "unix_ns() reading is before 2100");

    // -------------------------------------------------------------------
    goo_check_row(1, "two back-to-back unix_ns readings never go backward");
    int64_t t_a = goo_time_unix_ns();
    int64_t t_b = goo_time_unix_ns();
    goo_check(t_b >= t_a, "the second reading is not earlier than the first");

    // -------------------------------------------------------------------
    goo_check_row(2, "goo_time_sleep_ns(0) and a negative duration both return promptly");
    int64_t z0 = goo_time_unix_ns();
    goo_time_sleep_ns(0);
    int64_t z1 = goo_time_unix_ns();
    goo_check(z1 - z0 < PROMPT_BOUND_NS, "sleep_ns(0) returned in under 200ms");

    int64_t n0 = goo_time_unix_ns();
    goo_time_sleep_ns(NEGATIVE_NS);
    int64_t n1 = goo_time_unix_ns();
    goo_check(n1 - n0 < PROMPT_BOUND_NS, "sleep_ns(negative) returned in under 200ms");

    // -------------------------------------------------------------------
    goo_check_row(3, "goo_time_sleep_ns pauses for at least part of a positive duration");
    int64_t s0 = goo_time_unix_ns();
    goo_time_sleep_ns(SHORT_SLEEP_NS);
    int64_t s1 = goo_time_unix_ns();
    int64_t slept = s1 - s0;
    goo_check(slept >= LOWER_BOUND_NS, "sleep_ns(15ms) took at least 3ms");
    goo_check(slept < UPPER_SANITY_NS, "sleep_ns(15ms) took under 2s");

    // -------------------------------------------------------------------
    goo_check_row(4, "goo_time_after tolerates a non-positive duration");
    goo_channel_t* ch_zero = goo_time_after(0);
    goo_check(ch_zero != NULL, "time_after(0) returned a channel");
    if (ch_zero != NULL) {
        goo_check(goo_chan_cap(ch_zero) == 1, "time_after(0)'s channel has capacity 1");
        int64_t v_zero = 0;
        goo_check(poll_recv(ch_zero, &v_zero), "time_after(0) delivered a value within budget");
    }

    goo_channel_t* ch_neg = goo_time_after(NEGATIVE_NS);
    goo_check(ch_neg != NULL, "time_after(negative) returned a channel");
    if (ch_neg != NULL) {
        int64_t v_neg = 0;
        goo_check(poll_recv(ch_neg, &v_neg), "time_after(negative) delivered a value within budget");
    }

    // -------------------------------------------------------------------
    // REVIEW FIX. Rows 4 and 5 obtained channels from goo_time_after and never
    // released them, which valgrind reported as 528 bytes definitely lost in
    // goo_make_chan. The leak was the suite's, not the runtime's: goo_chan_free
    // is a public entry point and the suite simply did not call it.
    goo_chan_free(ch_zero);
    goo_chan_free(ch_neg);

    goo_check_row(5, "goo_time_after(15ms) fires only after some time passes, with a plausible payload");
    int64_t before = goo_time_unix_ns();
    goo_channel_t* ch = goo_time_after(SHORT_SLEEP_NS);
    goo_check(ch != NULL, "time_after(15ms) returned a channel");

    int64_t fired_at = 0;
    int got = 0;
    if (ch != NULL) {
        got = poll_recv(ch, &fired_at);
        goo_check(got, "time_after(15ms) delivered a value within budget");
    }
    int64_t after = goo_time_unix_ns();

    if (got) {
        goo_check(fired_at >= before, "the delivered time is not earlier than the call");
        goo_check(fired_at <= after, "the delivered time is not later than receipt");
        goo_check(after - before >= LOWER_BOUND_NS,
                  "receipt took at least 3ms, so the duration was not ignored");
    }

    // -------------------------------------------------------------------
    // Capacity-1 and single-shot: once the one buffered value is drained,
    // the channel reports empty and a further non-blocking read finds
    // nothing waiting (the channel is never closed by time_after, so this
    // is a genuine "would block", not a closed-channel zero value).
    goo_check_row(6, "the channel drains to empty after its one delivery");
    if (ch != NULL && got) {
        goo_check(goo_chan_cap(ch) == 1, "time_after(15ms)'s channel has capacity 1");
        goo_check(goo_chan_len(ch) == 0, "the channel is empty after the one receive");
        int64_t leftover = 0;
        goo_check(!goo_chan_try_recv(ch, &leftover), "a second immediate receive finds nothing");
    } else {
        // The row still ran and its checks still executed above the branch
        // that could skip it (goo_check_row already counted); nothing
        // further to assert if the fixture itself did not come up.
        goo_check(0, "row 6's fixture (a delivered value from row 5) was not available");
    }

    goo_chan_free(ch);

    return goo_check_done("time_shim");
}
