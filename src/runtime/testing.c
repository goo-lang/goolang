// `goo test` runtime: test execution, result reporting, and exit status.
//
// The Goo-visible testing.T is a one-field opaque struct (the sync.Mutex
// pattern — see sync_shim.c) whose sole field holds a GooTest* minted here.
// Goo code never sees inside it.
//
// WHY THE RUNTIME OWNS THE CALL. testing.Run takes the test FUNCTION as a
// value, not just its name, so the frame the test runs in belongs to this file.
// That is what makes t.Fatal implementable: nothing in Goo can unwind another
// function's frame, so only a caller that owns the frame can setjmp before the
// call and longjmp out of it. A design that passed only a name could set a
// failed flag but could not stop anything, and a fail-fast test would run on
// past its own failure.
//
// OUTPUT FORMAT matches `go test` with NO flags, which is terser than -v: a
// passing test prints nothing at all, and a failing one prints its --- FAIL
// header FOLLOWED BY its log lines. That ordering is why a test's log output is
// buffered here rather than streamed — the header carries the duration, which
// is not known until the test has returned, so the lines cannot be printed as
// they arrive.
//
// TWO KNOWN LIMITATIONS of the longjmp stop, both measured rather than assumed:
//
//   1. DEFERS DO NOT RUN. Go stops a test with runtime.Goexit, which unwinds the
//      stack and runs deferred calls. A longjmp does neither.
//   2. AN OPEN `arena { }` BLOCK IS NOT RECLAIMED. Its free is emitted at block
//      exit (statement_codegen.c), and a longjmp is not an exit path codegen
//      knows about, so the jump skips it. Measured: a t.Fatal inside an arena
//      produces correct output, the following test still runs, and nothing is
//      corrupted — the arena simply leaks, bounded by one per such test.
//
// Both are v1 limitations documented here and in the arc's design record, not
// silent behavior. Fixing either needs a real unwind mechanism, which is the
// same machinery a `recover()` implementation would need (roadmap P3.5).
//
// ONE DELIBERATE DIVERGENCE from Go: the package summary line is a bare `FAIL`
// / `ok` with no elapsed time. Go prints `FAIL\tpkg\t0.002s`. A duration is not
// reproducible, and the accept fixtures are exact-match goldens; the per-test
// `(0.00s)` is kept because it is stable for a fast test.
// scripts/goo_test_probe.sh covers the general case by normalizing durations.

#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <time.h>

typedef struct {
    const char* name;
    int         failed;
    char*       log;      // owned; accumulated, already-formatted log lines
    size_t      log_len;
    // Set for the duration of the test's own call, below. abort_valid guards
    // the jump: a longjmp with a stale buffer would return into a frame that
    // has already gone.
    jmp_buf     abort_to;
    int         abort_valid;
} GooTest;

// Whole-run tallies. `goo test` compiles exactly one package into one binary,
// so a single static set is the entire state this file needs.
static int g_total;
static int g_failed;

static double goo_testing_now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Go reports the BASE name of the file, not the path it was compiled from.
static const char* goo_testing_basename(const char* path) {
    if (!path) return "?";
    const char* slash = strrchr(path, '/');
    return (slash && slash[1]) ? slash + 1 : path;
}

void goo_testing_fail(void* handle) {
    GooTest* t = (GooTest*)handle;
    if (t) t->failed = 1;
}

// t.Fatal / t.Fatalf / t.FailNow: mark failed AND stop the test.
//
// The longjmp lands back in goo_testing_run, which owns the frame the test is
// running in. Go stops a test with runtime.Goexit, which unwinds and runs
// defers; this does not run them. That difference is documented rather than
// hidden — see this file's header and the arc's design record.
void goo_testing_failnow(void* handle) {
    GooTest* t = (GooTest*)handle;
    if (!t) return;
    t->failed = 1;
    if (t->abort_valid) longjmp(t->abort_to, 1);
}

// Append one already-rendered message as a Go-shaped log line:
//     <file>:<line>: <msg>
// `file` and `line` are the CALL SITE's position, threaded down from codegen —
// the runtime has no other way to know where in the user's source this came
// from.
void goo_testing_log(void* handle, goo_string_t file, int64_t line, goo_string_t msg) {
    GooTest* t = (GooTest*)handle;
    if (!t) return;

    const char* base = goo_testing_basename(file.data);
    const char* text = msg.data ? msg.data : "";

    // Codegen builds the message with fmt.Sprintln's lowering, which appends a
    // newline; this adds its own. Trim the trailing newlines so one log call
    // produces exactly one line. Go does the same thing for the same reason —
    // t.Error is defined as t.log(fmt.Sprintln(args...)).
    size_t text_len = strlen(text);
    while (text_len > 0 && (text[text_len - 1] == '\n' || text[text_len - 1] == '\r')) text_len--;

    // 4-space indent + "file:line: " + text + newline.
    size_t need = strlen(base) + text_len + 32;
    char* line_buf = (char*)malloc(need);
    if (!line_buf) return;
    int n = snprintf(line_buf, need, "    %s:%lld: %.*s\n", base, (long long)line,
                     (int)text_len, text);
    if (n < 0) { free(line_buf); return; }

    char* grown = (char*)realloc(t->log, t->log_len + (size_t)n + 1);
    if (!grown) { free(line_buf); return; }
    memcpy(grown + t->log_len, line_buf, (size_t)n + 1);
    t->log = grown;
    t->log_len += (size_t)n;
    free(line_buf);
}

// `fn`/`env` are the two halves of a Goo function VALUE — the universal fat
// pointer {fn_ptr, env_ptr}, called as fn_ptr(env, args...). Codegen extracts
// both and passes them separately; taking only fn and calling fn(&t) would hand
// the test pointer to the thunk's env parameter instead of its argument.
void goo_testing_run(goo_string_t name, void (*fn)(void*, void*), void* env) {
    GooTest t;
    t.name = (name.data && name.data[0]) ? name.data : "?";
    t.failed = 0;
    t.log = NULL;
    t.log_len = 0;

    t.abort_valid = 0;

    double start = goo_testing_now_sec();
    // setjmp must run in THIS frame, which is exactly why the runtime performs
    // the call instead of the generated main: a Goo-side caller could not offer
    // a frame for Fatal to return to.
    if (setjmp(t.abort_to) == 0) {
        t.abort_valid = 1;
        if (fn) fn(env, &t);
    }
    t.abort_valid = 0;
    double dur = goo_testing_now_sec() - start;

    g_total++;
    if (t.failed) {
        g_failed++;
        // Header first, then the buffered lines — Go's order.
        printf("--- FAIL: %s (%.2fs)\n", t.name, dur);
        if (t.log) fputs(t.log, stdout);
    }
    // A passing test prints nothing, matching `go test` without -v.

    free(t.log);
}

void goo_testing_summary(void) {
    if (g_failed > 0) {
        printf("FAIL\n");
        goo_exit(1);
    }
    printf("ok\n");
    goo_exit(0);
}
