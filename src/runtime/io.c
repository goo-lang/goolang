// Goo runtime: minimal file I/O backing.
//
// These functions back the os.WriteFile / os.ReadByte / os.FileSize builtins
// that the compiler lowers in call_codegen.c. The interface is deliberately
// scalar (C strings and ints, no aggregate params/returns) so it crosses the
// Goo<->C ABI cleanly: Goo `string` args arrive as a NUL-terminated `char*`
// (the compiler extracts the pointer field) and Goo `int` maps to C `int`.
//
// This is the M1 milestone's "a program can read/write a file" capability, not
// a full os package (handles, []byte buffers, rich errors) — that depends on
// language features not yet implemented (methods, enums, slice-typed params).
// On error these return a negative value (typically -errno).
//
// P4.8 adds goo_os_read_file / goo_os_read_line, backing os.ReadFile(string)
// !string and os.ReadLine() !string. These are NOT scalar-only like the
// functions above: the language now has a real !T error union, so these
// report success/failure via an i32 ok flag + a goo_string_t* out-param
// (mirroring goo_string_to_int's shape exactly — see call_codegen.c's
// codegen_generate_string_result_call) rather than a bare negative errno.
// goo_string_t is 16 bytes and crosses the C ABI by value fine (see
// goo_os_getenv above); the out-param exists so the SAME function signature
// can hand back either the success value or an error message depending on
// the ok flag, not because the struct itself is too large to return by value.

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"

// Write `data` (NUL-terminated) to `path`, creating/truncating it.
// Returns the number of bytes written, or -errno on failure.
int goo_sys_write_file(const char* path, const char* data) {
    if (!path || !data) return -EINVAL;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;

    size_t len = strlen(data);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            int e = errno;
            close(fd);
            return -e;
        }
        off += (size_t)n;
    }
    if (close(fd) < 0) return -errno;
    return (int)off;
}

// Return the byte at `offset` in `path` as 0..255, or a negative value on
// failure (-errno, or -1 if the offset is past end-of-file).
int goo_sys_read_byte(const char* path, int offset) {
    if (!path || offset < 0) return -EINVAL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;

    unsigned char byte = 0;
    ssize_t n = pread(fd, &byte, 1, (off_t)offset);
    int saved = errno;
    close(fd);
    if (n < 0) return -saved;
    if (n == 0) return -1; // past end of file
    return (int)byte;
}

// Return the size of `path` in bytes, or -errno on failure.
int goo_sys_file_size(const char* path) {
    if (!path) return -EINVAL;
    struct stat st;
    if (stat(path, &st) < 0) return -errno;
    return (int)st.st_size;
}

// Build a "<op>: <strerror>" (path == NULL) or "<op> <path>: <strerror>"
// message, heap-allocated so the error union's error slot owns a real
// buffer like every other goo_string_t producer in this runtime. Shared by
// goo_os_read_file and goo_os_read_line's read-error branch (never has a
// path — EOF is reported separately, see below).
// KNOWN DIVERGENCE from Go (documented, review-flagged): Go's os.ReadFile
// error is a PathError reading "open <path>: no such file or directory"
// (syscall op name, lowercase); ours reads "os.ReadFile <path>: No such
// file or directory" (goo op name, strerror capitalization — which is also
// locale-dependent). Callers string-matching Go error text will differ;
// aligning would need a fixed errno->text table, tracked as a follow-up.
static goo_string_t goo_os_io_error(const char* op, const char* path, int err) {
    const char* msg = strerror(err);
    size_t need = path
        ? strlen(op) + 1 + strlen(path) + 2 + strlen(msg) + 1
        : strlen(op) + 2 + strlen(msg) + 1;
    char* buf = malloc(need);
    if (!buf) {
        // Allocation failed while building an error message for an already-
        // failing call — fall back to a static string rather than losing the
        // failure signal entirely (the ok=0 flag still reports the failure).
        static const char* oom = "goo runtime: out of memory building an I/O error message";
        return (goo_string_t){ .data = (char*)oom, .length = strlen(oom) };
    }
    int n = path
        ? snprintf(buf, need, "%s %s: %s", op, path, msg)
        : snprintf(buf, need, "%s: %s", op, msg);
    return (goo_string_t){ .data = buf, .length = (n > 0) ? (size_t)n : 0 };
}

// os.ReadFile(path) -> !string: read the whole file into a fresh heap
// buffer. Uses fstat's st_size + a read loop (never strlen) so the returned
// length is byte-honest — embedded NULs in the file survive in *out even
// though they can't currently be WRITTEN by os.WriteFile (which still takes
// a NUL-terminated C string; that's a pre-existing limitation of WriteFile,
// out of scope here — see the P4.8 design doc's C2 section).
// Returns 1 on success (*out holds the content), 0 on failure (*out holds
// "os.ReadFile <path>: <strerror>").
int goo_os_read_file(const char* path, goo_string_t* out) {
    if (!out) return 0;
    if (!path) {
        // A zero-value Goo string has data == NULL. The ok=0 contract
        // REQUIRES *out to hold the error message — codegen's error branch
        // loads it unconditionally, so returning without writing *out hands
        // the error union an uninitialized stack slot (was a segfault).
        *out = goo_os_io_error("os.ReadFile", NULL, EINVAL);
        return 0;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        *out = goo_os_io_error("os.ReadFile", path, errno);
        return 0;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        int e = errno;
        close(fd);
        *out = goo_os_io_error("os.ReadFile", path, e);
        return 0;
    }
    size_t size = (st.st_size > 0) ? (size_t)st.st_size : 0;

    // st_size is a HINT, not the truth: /proc and /sys pseudo-files, devices,
    // and concurrently-growing files under-report it (usually as 0), and Go's
    // os.ReadFile likewise treats it only as the initial capacity. So: size
    // the first allocation from the hint (with a floor for the size==0 case),
    // but read until a genuine EOF, growing as needed. The +1/-1 dance keeps
    // one spare byte for the defensive NUL terminator (out->length stays the
    // honest byte count; embedded NULs survive — the explicit length is what
    // makes that correct, not the terminator).
    size_t cap = (size > 0) ? size + 1 : 4096;
    char* buf = malloc(cap);
    if (!buf) {
        close(fd);
        *out = goo_os_io_error("os.ReadFile", path, ENOMEM);
        return 0;
    }

    size_t off = 0;
    for (;;) {
        if (off == cap - 1) {
            size_t grown_cap = cap * 2;
            char* grown = realloc(buf, grown_cap);
            if (!grown) {
                free(buf);
                close(fd);
                *out = goo_os_io_error("os.ReadFile", path, ENOMEM);
                return 0;
            }
            buf = grown;
            cap = grown_cap;
        }
        ssize_t n = read(fd, buf + off, cap - 1 - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            free(buf);
            close(fd);
            *out = goo_os_io_error("os.ReadFile", path, e);
            return 0;
        }
        if (n == 0) break; // genuine EOF
        off += (size_t)n;
    }
    close(fd);

    buf[off] = '\0';
    out->data = buf;
    out->length = off;
    return 1;
}

// os.ReadLine() -> !string: read one line from stdin, stripping the
// trailing \n (and a preceding \r, for CRLF input). A line read at EOF
// without a trailing newline is still a SUCCESSFUL read (matches Go's
// bufio.Scanner) — the error only surfaces on the NEXT call, once the
// stream is truly exhausted with zero bytes available. That next call is
// the loop-termination signal a cat-like program needs.
// Returns 1 on success (*out holds the line), 0 at EOF or on a read error
// (*out holds a message either way).
int goo_os_read_line(goo_string_t* out) {
    if (!out) return 0;

    size_t cap = 128, len = 0;
    char* buf = malloc(cap);
    if (!buf) {
        *out = goo_os_io_error("os.ReadLine", NULL, ENOMEM);
        return 0;
    }

    int c;
    int got_any = 0;
    while ((c = fgetc(stdin)) != EOF) {
        got_any = 1;
        if (c == '\n') break;
        if (len + 1 >= cap) {
            cap *= 2;
            char* grown = realloc(buf, cap);
            if (!grown) {
                free(buf);
                *out = goo_os_io_error("os.ReadLine", NULL, ENOMEM);
                return 0;
            }
            buf = grown;
        }
        buf[len++] = (char)c;
    }

    if (!got_any) {
        // Nothing read at all before hitting EOF/error: the loop-termination
        // signal. Distinguish a genuine stream error from plain EOF so the
        // message isn't a misleading "EOF" when stdin actually failed.
        free(buf);
        if (ferror(stdin)) {
            *out = goo_os_io_error("os.ReadLine", NULL, errno);
        } else {
            static const char* eof_msg = "os.ReadLine: EOF";
            *out = (goo_string_t){ .data = (char*)eof_msg, .length = strlen(eof_msg) };
        }
        return 0;
    }

    if (len > 0 && buf[len - 1] == '\r') len--; // strip CRLF's \r
    buf[len] = '\0';
    out->data = buf;
    out->length = len;
    return 1;
}
