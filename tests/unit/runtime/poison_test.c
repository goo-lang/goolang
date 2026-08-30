// POISON ON FREE: proof that the use-after-free detector detects.
//
// SQLite's instrumented allocator overwrites every byte of a freed block with
// a nonsense bit pattern, so a read through a dangling pointer returns garbage
// instead of the value that used to be there (https://sqlite.org/malloc.html).
// goo_free implements the same technique behind GOO_ARC_POISON, and
// `make test-golden-poison` runs the whole golden corpus with it on.
//
// WHAT WAS MISSING. Nothing proved the DETECTOR works. The corpus gate shows
// 495 fixtures passing with poison enabled, which is a statement about the
// fixtures, not about the mechanism: a poison that never wrote a byte would
// give exactly the same 495. This suite makes the mechanism itself the thing
// under test.
//
// EVERY ROW IS A PAIRED MEASUREMENT. Each one reads the same freed block twice
// -- once with the poison off and once with it on -- and asserts the two
// differ. A one-sided check would pass against a runtime that poisoned nothing
// if the allocator happened to hand back a scrubbed page.
//
// READING FREED MEMORY IS UNDEFINED BEHAVIOUR, and this file does it on
// purpose, because that is the only way to observe what a use-after-free would
// observe. It is confined to this suite. Do not copy the pattern into a test
// of anything else.

#include "runtime.h"
#include "../goo_check.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>

// goo_poison_enabled caches its answer on first use, so a single process
// cannot see both settings. Each row therefore re-executes itself in a child
// with the environment it needs, and reports one byte back through a pipe.
static int probe_in_child(int poison_on, unsigned char* out_byte) {
    int fds[2];
    if (pipe(fds) != 0) {
        return 0;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        if (poison_on) {
            setenv("GOO_ARC_POISON", "1", 1);
        } else {
            unsetenv("GOO_ARC_POISON");
        }
        unsigned char* p = goo_alloc(64);
        memset(p, 0x11, 64);
        goo_free(p);
        // The read that a use-after-free would perform.
        unsigned char seen = p[0];
        ssize_t ignored = write(fds[1], &seen, 1);
        (void)ignored;
        _exit(0);
    }
    close(fds[1]);
    ssize_t n = read(fds[0], out_byte, 1);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return n == 1;
}

int main(void) {
    goo_check_expect(3);
    char label[192];

    // ---------------------------------------------------------------------
    goo_check_row(0, "with poison OFF, a freed byte still reads as what was written");
    unsigned char off_byte = 0;
    int got_off = probe_in_child(0, &off_byte);
    goo_check(got_off, "the control child reported a byte");
    snprintf(label, sizeof label,
             "freed payload reads 0x%02X, the value written before the free", off_byte);
    goo_check(got_off && off_byte == 0x11, label);

    // ---------------------------------------------------------------------
    // THE LOAD-BEARING ROW. If this reads 0x11 as well, the poison wrote
    // nothing and every green run of test-golden-poison was vacuous.
    goo_check_row(1, "with poison ON, the same byte reads as the poison pattern");
    unsigned char on_byte = 0;
    int got_on = probe_in_child(1, &on_byte);
    goo_check(got_on, "the poison child reported a byte");
    snprintf(label, sizeof label,
             "freed payload reads 0x%02X, not the 0x11 that was written", on_byte);
    goo_check(got_on && on_byte != 0x11, label);

    // ---------------------------------------------------------------------
    // The pair is the assertion. Either reading alone could be explained by
    // the allocator rather than by goo_free.
    goo_check_row(2, "the two settings disagree, which is what makes either meaningful");
    snprintf(label, sizeof label,
             "poison off = 0x%02X, poison on = 0x%02X", off_byte, on_byte);
    goo_check(got_off && got_on && off_byte != on_byte, label);

    return goo_check_done("poison");
}
