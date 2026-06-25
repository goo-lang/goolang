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

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

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
