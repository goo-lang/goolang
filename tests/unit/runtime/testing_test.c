// The `goo test` runtime: goo_testing_fail, goo_testing_failnow,
// goo_testing_log, goo_testing_run, goo_testing_summary.
//
// WHAT THIS SUITE CANNOT DO DIRECTLY. GooTest is a struct private to
// testing.c -- the header exposes only an opaque `void*` handle. The only way
// to get a live handle is the one testing.c itself hands out: the `fn`
// callback goo_testing_run invokes. Every check below that needs a handle
// therefore runs INSIDE such a callback, the same way codegen's own generated
// _testmain.goo would.
//
// WHY TWO ROWS FORK. goo_testing_summary ends in goo_exit() (runtime.c),
// which is a real exit(3) -- calling it in this process would kill the suite
// before goo_check_done() ever ran. Rows 0 and 1 fork a child, tie its stdout
// to a pipe, let it call goo_testing_summary(), and read the exit status back
// with waitpid. Every other row captures stdout via the same pipe-and-dup2
// trick without forking, since _run/_fail/_failnow/_log never call exit.
//
// WHY THE "ok" FORK RUNS FIRST. g_total/g_failed (testing.c) are file-scope
// statics for the whole process, never reset between goo_testing_run calls.
// fork() gives a child a COPY of them as they stand at that instant, so the
// child that must observe "nothing has failed" has to fork before this
// process's OWN later rows call goo_testing_fail/_failnow. The FAIL-side fork
// (row 1) carries no such ordering requirement: it runs its own failing test
// inside the child, so it reaches g_failed > 0 regardless of what ran before.
//
// DURATION IS NOT ASSERTED NUMERICALLY. The "(%.2fs)" field comes from
// clock_gettime(CLOCK_MONOTONIC), which this suite cannot control. Every
// header check below verifies the shape -- a parseable, non-negative number
// followed by "s)" -- never a specific digit.

#include "runtime.h"
#include "../goo_check.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

// ---------------------------------------------------------------------------
// stdout capture, in-process (pipe + dup2, no exit involved).

static int g_saved_stdout = -1;
static int g_capture_read = -1;

static void capture_begin(void) {
    int p[2];
    if (pipe(p) != 0) {
        g_saved_stdout = -1;
        g_capture_read = -1;
        return;
    }
    fflush(stdout);
    g_saved_stdout = dup(STDOUT_FILENO);
    dup2(p[1], STDOUT_FILENO);
    close(p[1]);
    g_capture_read = p[0];
}

// Returns a malloc'd, NUL-terminated copy of everything written to stdout
// since capture_begin(), and restores real stdout as a side effect. Caller
// frees the result.
static char* capture_end(void) {
    fflush(stdout);
    if (g_saved_stdout >= 0) {
        dup2(g_saved_stdout, STDOUT_FILENO);
        close(g_saved_stdout);
        g_saved_stdout = -1;
    }
    size_t cap = 256, len = 0;
    char* buf = (char*)malloc(cap);
    if (buf && g_capture_read >= 0) {
        for (;;) {
            if (len + 1 >= cap) {
                cap *= 2;
                char* grown = (char*)realloc(buf, cap);
                if (!grown) break;
                buf = grown;
            }
            ssize_t n = read(g_capture_read, buf + len, cap - len - 1);
            if (n <= 0) break;
            len += (size_t)n;
        }
    }
    if (buf) buf[len] = '\0';
    if (g_capture_read >= 0) {
        close(g_capture_read);
        g_capture_read = -1;
    }
    return buf ? buf : strdup("");
}

// ---------------------------------------------------------------------------
// A separate process, for the two rows that must call goo_testing_summary.

typedef void (*child_body_fn)(void);

// Runs `body` in a fork with its stdout tied to a pipe. `body` is expected to
// end by calling goo_testing_summary(), which exits the child. Returns a
// malloc'd copy of everything the child wrote to stdout (caller frees) and
// the raw waitpid status in *status.
static char* run_in_child(child_body_fn body, int* status) {
    int p[2];
    if (pipe(p) != 0) {
        *status = -1;
        return strdup("");
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        *status = -1;
        return strdup("");
    }
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], STDOUT_FILENO);
        close(p[1]);
        body();
        // body() is supposed to exit via goo_testing_summary(). Reaching
        // here means it returned instead -- a distinct, recognizable exit
        // code so that defect would be visible rather than read as FAIL/ok.
        fflush(stdout);
        _exit(66);
    }
    close(p[1]);
    size_t cap = 256, len = 0;
    char* buf = (char*)malloc(cap);
    if (buf) {
        for (;;) {
            if (len + 1 >= cap) {
                cap *= 2;
                char* grown = (char*)realloc(buf, cap);
                if (!grown) break;
                buf = grown;
            }
            ssize_t n = read(p[0], buf + len, cap - len - 1);
            if (n <= 0) break;
            len += (size_t)n;
        }
        buf[len] = '\0';
    }
    close(p[0]);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    *status = wstatus;
    return buf ? buf : strdup("");
}

static void always_fails(void* env, void* t) {
    (void)env;
    goo_testing_fail(t);
}

static void child_ok_body(void) {
    // Nothing has run in this process since it started (see the fork-order
    // note at the top of the file): g_total == 0, g_failed == 0.
    goo_testing_summary();
}


// ---------------------------------------------------------------------------
// STRING OWNERSHIP. goo_testing_run and goo_testing_log do NOT take ownership
// of the goo_string_t values they are given: goo_testing_run stores name.data
// in t.name and prints it, and frees only t.log. In a real run every such name
// is a string literal, which ARC marks IMMORTAL and goo_free skips, so nothing
// leaks. This suite builds them with goo_string_new instead, which allocates
// through goo_alloc and therefore DOES carry an object header.
//
// valgrind reported 577 bytes in 24 blocks before this helper existed. own()
// records each string so free_owned() can release them at the end of main.
// goo_free is the correct release here, unlike the message from
// goo_os_io_error in io_test.c, which is plain malloc and needs free().
// ---------------------------------------------------------------------------
#define MAX_OWNED 64
static goo_string_t owned_strings[MAX_OWNED];
static int owned_count;

static goo_string_t own(goo_string_t s) {
    if (s.data && owned_count < MAX_OWNED) {
        owned_strings[owned_count++] = s;
    }
    return s;
}

static void free_owned(void) {
    for (int i = 0; i < owned_count; i++) {
        goo_free(owned_strings[i].data);
    }
    owned_count = 0;
}

static void child_fail_body(void) {
    goo_string_t a = goo_string_new("A");
    goo_string_t b = goo_string_new("B");
    goo_testing_run(a, always_fails, NULL);
    goo_testing_run(b, always_fails, NULL);
    goo_free(a.data);
    goo_free(b.data);
    goo_testing_summary();
}

// ---------------------------------------------------------------------------
// Small helpers and callbacks shared by the log-format rows.

// True if `buf` starts with a line shaped exactly like goo_testing_run's
// failure header for `name`: "--- FAIL: <name> (<number>s)\n". Does not pin
// the duration's digits, only that a non-negative number sits there.
static int header_ok(const char* buf, const char* name) {
    char prefix[160];
    snprintf(prefix, sizeof(prefix), "--- FAIL: %s (", name);
    size_t plen = strlen(prefix);
    if (strncmp(buf, prefix, plen) != 0) return 0;
    const char* nl = strchr(buf, '\n');
    if (!nl || nl < buf + 2 || nl[-1] != ')' || nl[-2] != 's') return 0;
    double dur = -1.0;
    if (sscanf(buf + plen, "%lf", &dur) != 1) return 0;
    return dur >= 0.0;
}

// Everything in `buf` after its first line (own trailing '\n' included).
// NULL if `buf` has no newline at all.
static const char* after_first_line(const char* buf) {
    const char* nl = strchr(buf, '\n');
    return nl ? nl + 1 : NULL;
}

typedef struct {
    goo_string_t file;
    int64_t      line;
    goo_string_t msg;
} log_item_t;

typedef struct {
    log_item_t* items;
    int         n;
} log_batch_t;

// Logs every item in `env` (a log_batch_t*) in order, then fails once at the
// end. The shared shape every log-formatting row below needs.
static void log_batch_and_fail(void* env, void* t) {
    log_batch_t* batch = (log_batch_t*)env;
    for (int i = 0; i < batch->n; i++) {
        goo_testing_log(t, batch->items[i].file, batch->items[i].line, batch->items[i].msg);
    }
    goo_testing_fail(t);
}

// Logs `env` (a log_item_t*) and never fails -- row 4's "a pass discards its
// buffered log" case.
static void log_one_no_fail(void* env, void* t) {
    log_item_t* item = (log_item_t*)env;
    goo_testing_log(t, item->file, item->line, item->msg);
}

// Row 5: logs, fails, then logs again -- proving Fail (unlike FailNow) does
// not stop the test.
static void fail_between_logs(void* env, void* t) {
    (void)env;
    goo_testing_log(t, own(goo_string_new("dir/one.goo")), 10, own(goo_string_new("first")));
    goo_testing_fail(t);
    goo_testing_log(t, own(goo_string_new("dir/two.goo")), 20, own(goo_string_new("second")));
}

// Row 9: `env` is an int* set to 1 right before FailNow and 2 right after --
// the 2 must never land.
static void failnow_probe(void* env, void* t) {
    int* reached = (int*)env;
    *reached = 1;
    goo_testing_failnow(t);
    *reached = 2;
}

// Row 10: the failing half of the two-test sequence.
static void failnow_only(void* env, void* t) {
    (void)env;
    goo_testing_failnow(t);
}

// Row 10: the following test. `env` is an int* this sets to 1 so the row can
// prove the run reached it at all.
static void marks_ran(void* env, void* t) {
    (void)t;
    *(int*)env = 1;
}

// ---------------------------------------------------------------------------

int main(void) {
    goo_check_expect(11);
    char label[512];

    // -------------------------------------------------------------------
    goo_check_row(0, "goo_testing_summary reports ok and exits 0 when nothing failed");
    int status0 = 0;
    char* out0 = run_in_child(child_ok_body, &status0);
    snprintf(label, sizeof(label), "child exited normally, wait status 0x%x", status0);
    goo_check(WIFEXITED(status0), label);
    snprintf(label, sizeof(label), "exit code was %d", WIFEXITED(status0) ? WEXITSTATUS(status0) : -1);
    goo_check(WIFEXITED(status0) && WEXITSTATUS(status0) == 0, label);
    snprintf(label, sizeof(label), "stdout was \"%s\"", out0);
    goo_check(strcmp(out0, "ok\n") == 0, label);
    free(out0);

    // -------------------------------------------------------------------
    goo_check_row(1, "goo_testing_summary reports FAIL and exits 1, one bare FAIL line, after failures");
    int status1 = 0;
    char* out1 = run_in_child(child_fail_body, &status1);
    snprintf(label, sizeof(label), "exit code was %d, full stdout was \"%s\"",
             WIFEXITED(status1) ? WEXITSTATUS(status1) : -1, out1);
    goo_check(WIFEXITED(status1) && WEXITSTATUS(status1) == 1, label);
    snprintf(label, sizeof(label), "first line was a FAIL header for \"A\", full stdout was \"%s\"", out1);
    goo_check(header_ok(out1, "A"), label);
    const char* rest1 = after_first_line(out1);
    int rest1_ok = rest1 && header_ok(rest1, "B");
    snprintf(label, sizeof(label), "second line was a FAIL header for \"B\": \"%s\"", rest1 ? rest1 : "(none)");
    goo_check(rest1_ok, label);
    const char* rest2 = rest1_ok ? after_first_line(rest1) : NULL;
    snprintf(label, sizeof(label), "the only line left was the bare summary: \"%s\"", rest2 ? rest2 : "(none)");
    goo_check(rest2 && strcmp(rest2, "FAIL\n") == 0, label);
    free(out1);

    // -------------------------------------------------------------------
    goo_check_row(2, "NULL is tolerated by fail, failnow, and log");
    goo_testing_fail(NULL);
    goo_testing_failnow(NULL);   // must return, not longjmp with no jmp_buf set
    goo_testing_log(NULL, own(goo_string_new("x")), 1, own(goo_string_new("y")));
    goo_check(1, "goo_testing_fail/failnow/log(NULL, ...) returned instead of crashing");

    // -------------------------------------------------------------------
    goo_check_row(3, "goo_testing_run tolerates a NULL fn and an empty test name");
    capture_begin();
    goo_testing_run((goo_string_t){NULL, 0}, NULL, NULL);
    char* out3 = capture_end();
    snprintf(label, sizeof(label), "a NULL fn produced no output, got \"%s\"", out3);
    goo_check(out3[0] == '\0', label);
    free(out3);

    // -------------------------------------------------------------------
    goo_check_row(4, "a passing test, even one that logged, prints nothing");
    log_item_t pass_item = { own(goo_string_new("p.goo")), 3, own(goo_string_new("should not appear")) };
    capture_begin();
    goo_testing_run(own(goo_string_new("PassWithLog")), log_one_no_fail, &pass_item);
    char* out4 = capture_end();
    snprintf(label, sizeof(label), "a passing test's buffered log was discarded, stdout was \"%s\"", out4);
    goo_check(out4[0] == '\0', label);
    free(out4);

    // -------------------------------------------------------------------
    goo_check_row(5, "a failing test prints the FAIL header before its buffered log lines, and Fail does not stop it");
    capture_begin();
    goo_testing_run(own(goo_string_new("RowFive")), fail_between_logs, NULL);
    char* out5 = capture_end();
    snprintf(label, sizeof(label), "header line for RowFive: \"%s\"", out5);
    goo_check(header_ok(out5, "RowFive"), label);
    const char* rem5 = after_first_line(out5);
    snprintf(label, sizeof(label), "both log lines, in order, after the header: \"%s\"", rem5 ? rem5 : "(none)");
    goo_check(rem5 && strcmp(rem5, "    one.goo:10: first\n    two.goo:20: second\n") == 0, label);
    free(out5);

    // -------------------------------------------------------------------
    goo_check_row(6, "goo_testing_log uses the call site's base file name, not its full path");
    log_item_t basename_items[3] = {
        { own(goo_string_new("a/b/c/site_test.goo")), 7, own(goo_string_new("m1")) },
        { own(goo_string_new("bare.goo")),            1, own(goo_string_new("m2")) },
        { (goo_string_t){NULL, 0},                2, own(goo_string_new("m3")) },
    };
    log_batch_t basename_batch = { basename_items, 3 };
    capture_begin();
    goo_testing_run(own(goo_string_new("RowSix")), log_batch_and_fail, &basename_batch);
    char* out6 = capture_end();
    const char* rem6 = after_first_line(out6);
    snprintf(label, sizeof(label), "three lines with basenames only, \"?\" for a NULL path: \"%s\"", rem6 ? rem6 : "(none)");
    goo_check(rem6 && strcmp(rem6,
        "    site_test.goo:7: m1\n"
        "    bare.goo:1: m2\n"
        "    ?:2: m3\n") == 0, label);
    snprintf(label, sizeof(label), "the directory portion \"a/b/c/\" is absent, stdout was \"%s\"", out6);
    goo_check(strstr(out6, "a/b/c/") == NULL, label);
    free(out6);

    // -------------------------------------------------------------------
    goo_check_row(7, "goo_testing_log collapses a trailing newline in the message to exactly one");
    log_item_t nl_item = { own(goo_string_new("t.goo")), 5, own(goo_string_new("hello\n\n")) };
    log_batch_t nl_batch = { &nl_item, 1 };
    capture_begin();
    goo_testing_run(own(goo_string_new("RowSeven")), log_batch_and_fail, &nl_batch);
    char* out7 = capture_end();
    const char* rem7 = after_first_line(out7);
    snprintf(label, sizeof(label), "\"hello\\n\\n\" collapsed to one line: \"%s\"", rem7 ? rem7 : "(none)");
    goo_check(rem7 && strcmp(rem7, "    t.goo:5: hello\n") == 0, label);
    free(out7);

    // -------------------------------------------------------------------
    goo_check_row(8, "goo_testing_log tolerates a NULL message");
    log_item_t null_msg_item = { own(goo_string_new("u.goo")), 9, (goo_string_t){NULL, 0} };
    log_batch_t null_msg_batch = { &null_msg_item, 1 };
    capture_begin();
    goo_testing_run(own(goo_string_new("RowEight")), log_batch_and_fail, &null_msg_batch);
    char* out8 = capture_end();
    const char* rem8 = after_first_line(out8);
    snprintf(label, sizeof(label), "a NULL message left the text blank: \"%s\"", rem8 ? rem8 : "(none)");
    goo_check(rem8 && strcmp(rem8, "    u.goo:9: \n") == 0, label);
    free(out8);

    // -------------------------------------------------------------------
    goo_check_row(9, "goo_testing_failnow marks the test failed and stops it at the call site");
    int reached = 0;
    capture_begin();
    goo_testing_run(own(goo_string_new("FailNowTest")), failnow_probe, &reached);
    char* out9 = capture_end();
    snprintf(label, sizeof(label), "code after failnow did not run, flag ended at %d", reached);
    goo_check(reached == 1, label);
    snprintf(label, sizeof(label), "the test was still reported failed: \"%s\"", out9);
    goo_check(header_ok(out9, "FailNowTest"), label);
    free(out9);

    // -------------------------------------------------------------------
    goo_check_row(10, "the run continues with the next test after a failnow-terminated one");
    int second_ran = 0;
    capture_begin();
    goo_testing_run(own(goo_string_new("First")), failnow_only, NULL);
    goo_testing_run(own(goo_string_new("Second")), marks_ran, &second_ran);
    char* out10 = capture_end();
    snprintf(label, sizeof(label), "the second test's body executed, flag ended at %d", second_ran);
    goo_check(second_ran == 1, label);
    const char* rem10 = after_first_line(out10);
    snprintf(label, sizeof(label), "only First's FAIL header printed, Second passed silently: \"%s\"", out10);
    goo_check(header_ok(out10, "First") && rem10 && rem10[0] == '\0', label);
    free(out10);

    free_owned();

    return goo_check_done("testing");
}
