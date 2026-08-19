// P4.6 (packages-C, C1): time.Sleep / time.Now runtime shim.
//
// time.Duration and time.Time are synthesized by the checker (goo.c's time
// package seeding) exactly like sync.Mutex/WaitGroup (sync_shim.c) — Duration
// is a named int64 (nanoseconds), Time a one-field struct holding wall-clock
// nanoseconds since the Unix epoch. Unlike sync's opaque-pointer field, a
// plain int64 IS its own valid zero value, so there is no lazy-init dance
// here: every call below is a direct, stateless wrap of a platform primitive.
#include "runtime.h"
#include "platform.h"

// Go parity: Sleep pauses for at least the duration; a non-positive duration
// returns immediately rather than underflowing into a multi-century sleep if
// naively cast to unsigned (goo_platform_sleep_ns takes uint64_t).
void goo_time_sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    goo_platform_sleep_ns((uint64_t)ns);
}

// time.Now().UnixNano(): wall-clock nanoseconds since 1970-01-01, NOT the
// monotonic clock goo_platform_time_ns exposes for internal scheduler use —
// see goo_platform_wall_time_ns's doc comment (platform.h).
int64_t goo_time_unix_ns(void) {
    return (int64_t)goo_platform_wall_time_ns();
}

// time.After(d) — Go parity: a channel that delivers the time once at least d
// has passed.
//
// WHY A REAL CHANNEL, not goo_select's timeout_ns. goo_select already accepts a
// deadline, and lowering `case <-time.After(x)` straight onto it would be
// cheaper. It was rejected because it only works for that one syntactic shape:
// `t := time.After(d)` then `case <-t`, a plain `<-time.After(d)` outside any
// select, and two timeout cases in one select all have to keep working, and Go
// programs write all three. A genuine channel needs no select codegen at all —
// select blocks on -1 and this goroutine's send wakes it, through exactly the
// path an ordinary producer goroutine uses. timeout_ns stays available as a
// later optimisation for the common shape.
//
// CAPACITY 1 IS LOAD-BEARING, and it is Go's rule too. The timer must never
// block on send, because the usual case is that some other select arm won and
// nobody ever receives from this channel. With capacity 0 that goroutine would
// wait forever holding the buffer.
//
// The channel is not freed. That is the pre-existing behaviour of EVERY channel
// in this runtime — codegen emits no goo_chan_free, and TYPE_CHANNEL is not in
// the ARC release decision, so `make(chan T)` leaks its channel too. time.After
// is not a new leak class, and it must not be given a free path on its own
// while the timer goroutine can still hold the pointer.
typedef struct {
    goo_channel_t* ch;
    int64_t        ns;
} goo_after_arg_t;

static void goo_time_after_worker(void* arg) {
    goo_after_arg_t* a = (goo_after_arg_t*)arg;
    if (a->ns > 0) {
        goo_platform_sleep_ns((uint64_t)a->ns);
    }
    // Go sends the time AT firing, not at the call, so read the clock here.
    int64_t fired_at = goo_time_unix_ns();
    goo_chan_send(a->ch, &fired_at);
    free(a);
}

goo_channel_t* goo_time_after(int64_t ns) {
    // Element type is time.Time, which the checker builds as a one-field
    // struct { int64 _nanos } — so 8 bytes, matching what the worker sends.
    goo_channel_t* ch = goo_make_chan(sizeof(int64_t), 1);
    if (!ch) return NULL;

    goo_after_arg_t* arg = xmalloc(sizeof(goo_after_arg_t));
    arg->ch = ch;
    arg->ns = ns;

    if (!goo_go(goo_time_after_worker, arg)) {
        free(arg);
        // Leave ch alive: a receiver that blocks forever is a better failure
        // than a dangling pointer, and freeing here races nothing only because
        // the goroutine did not start.
        goo_chan_free(ch);
        return NULL;
    }
    return ch;
}
