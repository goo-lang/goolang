/*
 * A deliberate heap-buffer-overflow, to prove --config=asan can report one.
 *
 * Without the sanitizer this writes one byte past an 8-byte allocation and
 * exits 0: the overflow lands in malloc's own padding and nothing complains.
 * That is the point. A gate proven only by passing clean code is not proven.
 */
#include <stdlib.h>

int main(void) {
    /* volatile defeats constant folding, so the access survives to run time. */
    volatile int idx = 8;
    char *p = malloc(8);
    char c;

    if (p == NULL) {
        return 1;
    }
    p[idx] = 'x';   /* one past the end */
    c = p[idx];
    free(p);
    return c == 'x' ? 0 : 0;
}
