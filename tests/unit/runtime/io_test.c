// The runtime's file I/O backing: goo_sys_write_file, goo_sys_read_byte,
// goo_sys_file_size, goo_os_read_file, goo_os_read_line, goo_os_file_write,
// and the two os.File globals (goo_os_stdout_file / goo_os_stderr_file).
//
// THREE FUNCTIONS HAVE NO PROTOTYPE IN include/runtime.h. goo_sys_write_file,
// goo_sys_read_byte and goo_sys_file_size are declared nowhere in the C
// headers -- src/codegen/runtime_integration.c registers their LLVM
// signatures by hand instead, and no C caller in the tree needed a prototype
// until this suite. Under -std=c23 (.bazelrc, Makefile:23) an undeclared call
// is a hard compile error, not a warning, so the three are declared below,
// copied character-for-character from their definitions in io.c.
//
// WHAT goo_os_file_write IS GIVEN. struct goo_os_file is forward-declared
// only ("struct goo_os_file;") -- io.c's own comment says the real struct is
// "one opaque machine word holding the file descriptor" with "the fd sits at
// offset 0". goo_os_file_write's first parameter is `void* file`, though, so
// this suite never needs the real struct: an int64_t holding a file
// descriptor, passed by address, satisfies the same layout by construction.
// goo_os_stdout_file and goo_os_stderr_file are exercised through their own
// extern declarations instead, since those already have the right (opaque)
// type.
//
// WHAT THIS SUITE CANNOT REACH. Every read(2)/write(2) call in io.c is
// direct, not routed through a replaceable table, so nothing here can force
// EINTR on the retry path or a short write from a full disk. That needs a
// seam io.c does not have (see the report handed back with this suite).
//
// Every fixture lives under TEST_TMPDIR (or /tmp if that variable is unset)
// and is removed again before the process moves on. Nothing here depends on
// the current working directory.

#include "runtime.h"
#include "../goo_check.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// See the file header comment: not in include/runtime.h, defined in io.c.
int goo_sys_write_file(const char* path, const char* data);
int goo_sys_read_byte(const char* path, int offset);
int goo_sys_file_size(const char* path);

// ---------------------------------------------------------------------------
// Fixture helpers. None of these is the code under test.

static const char* tmp_dir(void) {
    const char* d = getenv("TEST_TMPDIR");
    return (d && *d) ? d : "/tmp";
}

static void tmp_path(char* buf, size_t bufsz, const char* name) {
    snprintf(buf, bufsz, "%s/io_test_%d_%s", tmp_dir(), (int)getpid(), name);
}

// Writes exactly `len` bytes, embedded NULs included. goo_sys_write_file
// cannot build this fixture itself (it takes a NUL-terminated C string), so
// a plain fopen/fwrite stands in wherever a fixture needs one.
static int write_fixture(const char* path, const void* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    int ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    fclose(f);
    return ok;
}

static int redirect_stdin_from(const char* path) {
    return freopen(path, "r", stdin) != NULL;
}

// Builds one row's FAIL label in a reusable buffer, observed value included
// (goo_check() prints `label` only on failure, and only once -- it has to
// carry the number that was actually returned, not just what was wanted).
static char g_label[320];
static const char* label(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_label, sizeof g_label, fmt, ap);
    va_end(ap);
    return g_label;
}

int main(void) {
    goo_check_expect(20);

    // -----------------------------------------------------------------
    goo_check_row(0, "goo_sys_write_file rejects a NULL path and NULL data");
    {
        int rc_path = goo_sys_write_file(NULL, "hello");
        goo_check(rc_path == -EINVAL,
                  label("write_file(NULL, \"hello\") = %d, want %d", rc_path, -EINVAL));

        char p[512];
        tmp_path(p, sizeof p, "row0.txt");
        int rc_data = goo_sys_write_file(p, NULL);
        goo_check(rc_data == -EINVAL,
                  label("write_file(path, NULL) = %d, want %d", rc_data, -EINVAL));
    }

    // -----------------------------------------------------------------
    goo_check_row(1, "goo_sys_write_file writes the given bytes and returns the count");
    {
        char p[512];
        tmp_path(p, sizeof p, "row1.txt");
        const char* content = "goolang runtime io test";
        int n = goo_sys_write_file(p, content);
        goo_check((size_t)n == strlen(content),
                  label("write_file returned %d, want %zu", n, strlen(content)));

        char rbuf[128] = {0};
        FILE* rf = fopen(p, "rb");
        size_t got = rf ? fread(rbuf, 1, sizeof rbuf - 1, rf) : 0;
        if (rf) fclose(rf);
        goo_check(got == strlen(content) && memcmp(rbuf, content, got) == 0,
                  label("file on disk held \"%s\" (%zu bytes), want \"%s\"", rbuf, got, content));
        unlink(p);

        char pe[512];
        tmp_path(pe, sizeof pe, "row1_empty.txt");
        int ne = goo_sys_write_file(pe, "");
        goo_check(ne == 0, label("write_file(path, \"\") returned %d, want 0", ne));
        struct stat st;
        int src = stat(pe, &st);
        goo_check(src == 0 && st.st_size == 0,
                  label("an empty write left a file of %lld bytes, want 0",
                        src == 0 ? (long long)st.st_size : -1));
        unlink(pe);
    }

    // -----------------------------------------------------------------
    goo_check_row(2, "goo_sys_write_file truncates a file that already held more data");
    {
        char p[512];
        tmp_path(p, sizeof p, "row2.txt");
        goo_sys_write_file(p, "a longer string written first");
        int n = goo_sys_write_file(p, "short");
        goo_check(n == (int)strlen("short"),
                  label("the second write returned %d, want %d", n, (int)strlen("short")));

        struct stat st;
        int src = stat(p, &st);
        goo_check(src == 0 && st.st_size == (off_t)strlen("short"),
                  label("the file is %lld bytes after the second write, want %zu",
                        src == 0 ? (long long)st.st_size : -1, strlen("short")));

        char rbuf[64] = {0};
        FILE* rf = fopen(p, "rb");
        size_t got = rf ? fread(rbuf, 1, sizeof rbuf - 1, rf) : 0;
        if (rf) fclose(rf);
        goo_check(got == strlen("short") && memcmp(rbuf, "short", got) == 0,
                  label("file content after the truncating write is \"%s\"", rbuf));
        unlink(p);
    }

    // -----------------------------------------------------------------
    goo_check_row(3, "goo_sys_write_file reports -ENOENT when the parent directory is missing");
    {
        char p[512];
        tmp_path(p, sizeof p, "row3_missing_dir/file.txt");
        int n = goo_sys_write_file(p, "data");
        goo_check(n == -ENOENT, label("write into a missing directory returned %d, want %d", n, -ENOENT));
    }

    // -----------------------------------------------------------------
    goo_check_row(4, "goo_sys_read_byte rejects a NULL path and a negative offset");
    {
        int r1 = goo_sys_read_byte(NULL, 0);
        goo_check(r1 == -EINVAL, label("read_byte(NULL, 0) = %d, want %d", r1, -EINVAL));

        char p[512];
        tmp_path(p, sizeof p, "row4.txt");
        write_fixture(p, "x", 1);
        int r2 = goo_sys_read_byte(p, -1);
        goo_check(r2 == -EINVAL, label("read_byte(path, -1) = %d, want %d", r2, -EINVAL));
        unlink(p);
    }

    // -----------------------------------------------------------------
    goo_check_row(5, "goo_sys_read_byte reads in-range bytes and reports end-of-file and a missing file");
    {
        char p[512];
        tmp_path(p, sizeof p, "row5.txt");
        const char* content = "ABCDE";
        write_fixture(p, content, strlen(content));

        int first = goo_sys_read_byte(p, 0);
        goo_check(first == 'A', label("read_byte(offset 0) = %d, want %d ('A')", first, (int)'A'));
        int mid = goo_sys_read_byte(p, 2);
        goo_check(mid == 'C', label("read_byte(offset 2) = %d, want %d ('C')", mid, (int)'C'));
        int last = goo_sys_read_byte(p, 4);
        goo_check(last == 'E', label("read_byte(offset 4) = %d, want %d ('E')", last, (int)'E'));
        int at_len = goo_sys_read_byte(p, (int)strlen(content));
        goo_check(at_len == -1,
                  label("read_byte(offset == length) = %d, want -1 (past end of file)", at_len));
        int far = goo_sys_read_byte(p, 999);
        goo_check(far == -1, label("read_byte(offset 999) = %d, want -1", far));
        unlink(p);

        char pm[512];
        tmp_path(pm, sizeof pm, "row5_missing.txt");
        int miss = goo_sys_read_byte(pm, 0);
        goo_check(miss == -ENOENT, label("read_byte(missing file) = %d, want %d", miss, -ENOENT));
    }

    // -----------------------------------------------------------------
    goo_check_row(6, "goo_sys_file_size reports the byte count, 0 for an empty file, and an error for NULL or missing paths");
    {
        int null_sz = goo_sys_file_size(NULL);
        goo_check(null_sz == -EINVAL, label("file_size(NULL) = %d, want %d", null_sz, -EINVAL));

        char pm[512];
        tmp_path(pm, sizeof pm, "row6_missing.txt");
        int miss_sz = goo_sys_file_size(pm);
        goo_check(miss_sz == -ENOENT, label("file_size(missing) = %d, want %d", miss_sz, -ENOENT));

        char pe[512];
        tmp_path(pe, sizeof pe, "row6_empty.txt");
        write_fixture(pe, "", 0);
        int empty_sz = goo_sys_file_size(pe);
        goo_check(empty_sz == 0, label("file_size(empty file) = %d, want 0", empty_sz));
        unlink(pe);

        char p[512];
        tmp_path(p, sizeof p, "row6.txt");
        const char* content = "twelve bytes";
        write_fixture(p, content, strlen(content));
        int sz = goo_sys_file_size(p);
        goo_check((size_t)sz == strlen(content), label("file_size = %d, want %zu", sz, strlen(content)));
        unlink(p);
    }

    // -----------------------------------------------------------------
    goo_check_row(7, "goo_os_read_file tolerates a NULL out-param and reports a NULL path as an error");
    {
        int rc = goo_os_read_file("/anything", NULL);
        goo_check(rc == 0, label("read_file(path, NULL) = %d, want 0", rc));

        goo_string_t err;
        int rc2 = goo_os_read_file(NULL, &err);
        goo_check(rc2 == 0, label("read_file(NULL, out) = %d, want 0", rc2));
        goo_check(err.data != NULL && strstr(err.data, "os.ReadFile") != NULL,
                  label("error message = \"%s\", want it to name os.ReadFile",
                        err.data ? err.data : "(null)"));
        goo_check(err.data != NULL && strstr(err.data, strerror(EINVAL)) != NULL,
                  label("error message = \"%s\", want it to contain \"%s\"",
                        err.data ? err.data : "(null)", strerror(EINVAL)));
        // REVIEW FIX, and the reason is worth more than the two lines.
        // valgrind reported 107 bytes definitely lost here, in
        // goo_os_io_error. The message is built with PLAIN malloc -- io.c
        // calls malloc three times and goo_alloc zero times -- so it carries
        // NO ARC object header. It must therefore be released with free(),
        // never with goo_free(), which subtracts GOO_OBJ_HEADER_SIZE and
        // would hand a wrong pointer to free().
        free(err.data);
    }

    // -----------------------------------------------------------------
    goo_check_row(8, "goo_os_read_file reports a missing file with the path and strerror in the message");
    {
        char p[512];
        tmp_path(p, sizeof p, "row8_missing.txt");
        goo_string_t out;
        int rc = goo_os_read_file(p, &out);
        goo_check(rc == 0, label("read_file(missing) ok=%d, want 0", rc));
        goo_check(out.data != NULL && strstr(out.data, p) != NULL,
                  label("error message = \"%s\", want it to contain the path \"%s\"",
                        out.data ? out.data : "(null)", p));
        goo_check(out.data != NULL && strstr(out.data, strerror(ENOENT)) != NULL,
                  label("error message = \"%s\", want it to contain \"%s\"",
                        out.data ? out.data : "(null)", strerror(ENOENT)));
        free(out.data);   // plain free: no ARC header. See row 7.
    }

    // -----------------------------------------------------------------
    goo_check_row(9, "goo_os_read_file returns the exact content, byte count, and a NUL-terminated buffer");
    {
        char p[512];
        tmp_path(p, sizeof p, "row9.txt");
        const char* content = "the quick brown fox";
        write_fixture(p, content, strlen(content));

        goo_string_t out;
        int rc = goo_os_read_file(p, &out);
        goo_check(rc == 1, label("read_file ok=%d, want 1", rc));
        goo_check(out.length == strlen(content),
                  label("length = %zu, want %zu", out.length, strlen(content)));
        goo_check(out.data != NULL && memcmp(out.data, content, out.length) == 0,
                  label("content = \"%.*s\", want \"%s\"",
                        (int)out.length, out.data ? out.data : "", content));
        goo_check(out.data != NULL && out.data[out.length] == '\0',
                  "the buffer carries a defensive NUL terminator after the last byte");
        free(out.data);
        unlink(p);
    }

    // -----------------------------------------------------------------
    goo_check_row(10, "goo_os_read_file succeeds on an empty file with length 0 and a non-NULL buffer");
    {
        char p[512];
        tmp_path(p, sizeof p, "row10_empty.txt");
        write_fixture(p, "", 0);

        goo_string_t out;
        int rc = goo_os_read_file(p, &out);
        goo_check(rc == 1, label("read_file(empty) ok=%d, want 1", rc));
        goo_check(out.length == 0, label("length = %zu, want 0", out.length));
        goo_check(out.data != NULL, "the buffer for an empty file is still non-NULL");
        free(out.data);
        unlink(p);
    }

    // -----------------------------------------------------------------
    goo_check_row(11, "goo_os_read_file keeps an embedded NUL and reports the true byte length, not strlen");
    {
        char p[512];
        tmp_path(p, sizeof p, "row11_nul.txt");
        const char raw[5] = {'A', 'B', '\0', 'C', 'D'};
        write_fixture(p, raw, sizeof raw);

        goo_string_t out;
        int rc = goo_os_read_file(p, &out);
        goo_check(rc == 1, label("read_file(embedded NUL) ok=%d, want 1", rc));
        goo_check(out.length == sizeof raw,
                  label("length = %zu, want %zu (strlen would stop at 2)", out.length, sizeof raw));
        goo_check(out.data != NULL && memcmp(out.data, raw, sizeof raw) == 0,
                  "the 5 bytes read back match the 5 bytes written, embedded NUL included");
        free(out.data);
        unlink(p);
    }

    // -----------------------------------------------------------------
    goo_check_row(12, "goo_os_read_line tolerates a NULL out-param");
    {
        // Reaches its guard before touching stdin at all, so no redirection
        // is needed for this row.
        int rc = goo_os_read_line(NULL);
        goo_check(rc == 0, label("read_line(NULL) = %d, want 0", rc));
    }

    // -----------------------------------------------------------------
    goo_check_row(13, "goo_os_read_line returns one line without its trailing newline");
    {
        char p[512];
        tmp_path(p, sizeof p, "row13_stdin.txt");
        const char* fixture = "first line\nsecond line\n";
        write_fixture(p, fixture, strlen(fixture));
        goo_check(redirect_stdin_from(p), "stdin was redirected to the row13 fixture");

        goo_string_t out;
        int rc = goo_os_read_line(&out);
        goo_check(rc == 1, label("read_line ok=%d, want 1", rc));
        goo_check(out.data != NULL && out.length == strlen("first line") &&
                      memcmp(out.data, "first line", out.length) == 0,
                  label("line = \"%.*s\", want \"first line\"",
                        (int)out.length, out.data ? out.data : ""));
        free(out.data);
        unlink(p);
    }

    // -----------------------------------------------------------------
    goo_check_row(14, "goo_os_read_line strips a trailing \\r from a CRLF line");
    {
        char p[512];
        tmp_path(p, sizeof p, "row14_stdin.txt");
        const char* fixture = "windows style\r\n";
        write_fixture(p, fixture, strlen(fixture));
        goo_check(redirect_stdin_from(p), "stdin was redirected to the row14 fixture");

        goo_string_t out;
        int rc = goo_os_read_line(&out);
        goo_check(rc == 1, label("read_line ok=%d, want 1", rc));
        goo_check(out.data != NULL && out.length == strlen("windows style") &&
                      memcmp(out.data, "windows style", out.length) == 0,
                  label("line = \"%.*s\", want \"windows style\" with the \\r stripped",
                        (int)out.length, out.data ? out.data : ""));
        free(out.data);
        unlink(p);
    }

    // -----------------------------------------------------------------
    goo_check_row(15, "a final line with no trailing newline is still a successful read");
    {
        char p[512];
        tmp_path(p, sizeof p, "row15_stdin.txt");
        const char* fixture = "no newline at eof";
        write_fixture(p, fixture, strlen(fixture));
        goo_check(redirect_stdin_from(p), "stdin was redirected to the row15 fixture");

        goo_string_t out;
        int rc = goo_os_read_line(&out);
        goo_check(rc == 1, label("read_line ok=%d, want 1 (EOF-terminated read is still success)", rc));
        goo_check(out.data != NULL && out.length == strlen(fixture) &&
                      memcmp(out.data, fixture, out.length) == 0,
                  label("line = \"%.*s\", want \"%s\"",
                        (int)out.length, out.data ? out.data : "", fixture));
        free(out.data);
        unlink(p);
    }

    // -----------------------------------------------------------------
    goo_check_row(16, "a stream with zero bytes reports EOF and ok=0");
    {
        char p[512];
        tmp_path(p, sizeof p, "row16_stdin.txt");
        write_fixture(p, "", 0);
        goo_check(redirect_stdin_from(p), "stdin was redirected to the row16 (empty) fixture");

        goo_string_t out;
        int rc = goo_os_read_line(&out);
        goo_check(rc == 0, label("read_line(empty stream) ok=%d, want 0", rc));
        // The EOF branch hands back a STATIC message (io.c's eof_msg), not a
        // heap one -- this row must not free(out.data).
        goo_check(out.data != NULL && strstr(out.data, "EOF") != NULL,
                  label("message = \"%s\", want it to mention EOF", out.data ? out.data : "(null)"));
        unlink(p);
    }

    // -----------------------------------------------------------------
    goo_check_row(17, "goo_os_stdout_file and goo_os_stderr_file hold file descriptors 1 and 2");
    {
        char pout[512], perr[512];
        tmp_path(pout, sizeof pout, "row17_stdout.txt");
        tmp_path(perr, sizeof perr, "row17_stderr.txt");

        int saved_out = dup(1);
        int saved_err = dup(2);
        goo_check(saved_out >= 0 && saved_err >= 0, "the real fd 1 and fd 2 were saved for restoration");

        int fout = open(pout, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int ferr = open(perr, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        goo_check(fout >= 0 && ferr >= 0, "the two redirection targets were opened");

        dup2(fout, 1);
        dup2(ferr, 2);
        close(fout);
        close(ferr);

        const char* msg_out = "to stdout";
        const char* msg_err = "to stderr";
        int64_t wout = goo_os_file_write(&goo_os_stdout_file, msg_out, (int64_t)strlen(msg_out));
        int64_t werr = goo_os_file_write(&goo_os_stderr_file, msg_err, (int64_t)strlen(msg_err));

        // Restore the real descriptors before any further goo_check() call,
        // since a FAIL line prints to (real) stdout.
        if (saved_out >= 0) { dup2(saved_out, 1); close(saved_out); }
        if (saved_err >= 0) { dup2(saved_err, 2); close(saved_err); }

        goo_check(wout == (int64_t)strlen(msg_out),
                  label("write to goo_os_stdout_file returned %lld, want %zu",
                        (long long)wout, strlen(msg_out)));
        goo_check(werr == (int64_t)strlen(msg_err),
                  label("write to goo_os_stderr_file returned %lld, want %zu",
                        (long long)werr, strlen(msg_err)));

        char rout[64] = {0}, rerr[64] = {0};
        FILE* fo = fopen(pout, "rb");
        size_t go = fo ? fread(rout, 1, sizeof rout - 1, fo) : 0;
        if (fo) fclose(fo);
        FILE* fe = fopen(perr, "rb");
        size_t ge = fe ? fread(rerr, 1, sizeof rerr - 1, fe) : 0;
        if (fe) fclose(fe);

        goo_check(go == strlen(msg_out) && memcmp(rout, msg_out, go) == 0,
                  label("the file behind fd 1 holds \"%s\", proving goo_os_stdout_file's fd is 1", rout));
        goo_check(ge == strlen(msg_err) && memcmp(rerr, msg_err, ge) == 0,
                  label("the file behind fd 2 holds \"%s\", proving goo_os_stderr_file's fd is 2", rerr));

        unlink(pout);
        unlink(perr);
    }

    // -----------------------------------------------------------------
    goo_check_row(18, "goo_os_file_write rejects a NULL file, rejects NULL data only when n > 0, and accepts NULL data when n == 0");
    {
        int64_t w1 = goo_os_file_write(NULL, "x", 1);
        goo_check(w1 == -EINVAL, label("write(NULL file, \"x\", 1) = %lld, want %d", (long long)w1, -EINVAL));

        // Never dereferenced by either guard branch below: n > 0 with a NULL
        // buffer returns before any read of *file, and n == 0 never reaches
        // the write loop at all.
        int64_t unused_file = 1;
        int64_t w2 = goo_os_file_write(&unused_file, NULL, 1);
        goo_check(w2 == -EINVAL, label("write(file, NULL, 1) = %lld, want %d", (long long)w2, -EINVAL));

        int64_t w3 = goo_os_file_write(&unused_file, NULL, 0);
        goo_check(w3 == 0, label("write(file, NULL, 0) = %lld, want 0 (nothing to write)", (long long)w3));
    }

    // -----------------------------------------------------------------
    goo_check_row(19, "goo_os_file_write writes the exact byte count to a real fd, and reports -EBADF for an invalid one");
    {
        char p[512];
        tmp_path(p, sizeof p, "row19.txt");
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        goo_check(fd >= 0, label("a temp file was opened, fd = %d", fd));

        // struct goo_os_file is one int64_t (the fd) at offset 0 -- see the
        // file header comment. Built by hand since the real struct is
        // opaque outside io.c.
        int64_t file = fd;
        const char* content = "written through goo_os_file_write";
        int64_t w = goo_os_file_write(&file, content, (int64_t)strlen(content));
        close(fd);
        goo_check(w == (int64_t)strlen(content),
                  label("write returned %lld, want %zu", (long long)w, strlen(content)));

        char rbuf[64] = {0};
        FILE* rf = fopen(p, "rb");
        size_t got = rf ? fread(rbuf, 1, sizeof rbuf - 1, rf) : 0;
        if (rf) fclose(rf);
        goo_check(got == strlen(content) && memcmp(rbuf, content, got) == 0,
                  label("file on disk holds \"%s\", want \"%s\"", rbuf, content));
        unlink(p);

        int64_t bad_fd = -1;
        int64_t w_bad = goo_os_file_write(&bad_fd, "x", 1);
        goo_check(w_bad == -EBADF,
                  label("write to fd -1 returned %lld, want %d (-EBADF)", (long long)w_bad, -EBADF));
    }

    return goo_check_done("io");
}
