/*
 * A deliberate signed-integer overflow, to prove --config=ubsan can report one.
 *
 * Without the sanitizer this wraps silently on every mainstream target and
 * exits 0, even though the C standard calls it undefined behaviour.
 */
int main(void) {
    volatile int a = 2147483647;   /* INT_MAX */
    volatile int b = 1;
    volatile int c;

    c = a + b;   /* signed overflow: undefined behaviour */
    return c == 0 ? 0 : 0;
}
