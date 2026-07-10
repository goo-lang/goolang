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
