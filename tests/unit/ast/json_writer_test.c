// json_writer_test: the dump's writer must be byte-exact and deterministic.
// Rows are exact-string comparisons against a memstream, because the whole
// point of the writer is that two runs over the same tree produce the same
// bytes — a "looks like JSON" check would pass a writer that reorders keys.
#include "../goo_check.h"
#include "json_writer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define JW_ROWS 6

static char* capture(void (*fn)(JsonW*)) {
    char* buf = NULL; size_t n = 0;
    FILE* f = open_memstream(&buf, &n);
    JsonW w; jw_init(&w, f);
    fn(&w);
    fclose(f);
    return buf;
}

static void row1(JsonW* w) { jw_begin_object(w); jw_end_object(w); }
static void row2(JsonW* w) {
    jw_begin_object(w);
    jw_key(w, "a"); jw_int(w, 1);
    jw_key(w, "b"); jw_string(w, "x");
    jw_end_object(w);
}
static void row3(JsonW* w) {
    jw_begin_array(w); jw_int(w, -1); jw_bool(w, true); jw_null(w); jw_end_array(w);
}
static void row4(JsonW* w) { jw_string(w, "q\"b\\s\n\t\x01"); }
static void row5(JsonW* w) { jw_string_len(w, "a\0b", 3); }
static void row6(JsonW* w) {
    jw_begin_object(w);
    jw_key(w, "o"); jw_begin_object(w); jw_key(w, "k"); jw_begin_array(w); jw_end_array(w); jw_end_object(w);
    jw_end_object(w);
}

int main(void) {
    goo_check_expect(JW_ROWS);
    char* s;

    goo_check_row(1, "empty object");
    s = capture(row1); goo_check(strcmp(s, "{}") == 0, s); free(s);

    goo_check_row(2, "keys in call order, 2-space indent");
    s = capture(row2); goo_check(strcmp(s, "{\n  \"a\": 1,\n  \"b\": \"x\"\n}") == 0, s); free(s);

    goo_check_row(3, "array of scalars");
    s = capture(row3); goo_check(strcmp(s, "[\n  -1,\n  true,\n  null\n]") == 0, s); free(s);

    goo_check_row(4, "escapes: quote, backslash, newline, tab, control");
    s = capture(row4); goo_check(strcmp(s, "\"q\\\"b\\\\s\\n\\t\\u0001\"") == 0, s); free(s);

    goo_check_row(5, "embedded NUL survives as \\u0000");
    s = capture(row5); goo_check(strcmp(s, "\"a\\u0000b\"") == 0, s); free(s);

    goo_check_row(6, "nested empty containers stay on one line");
    s = capture(row6); goo_check(strcmp(s, "{\n  \"o\": {\n    \"k\": []\n  }\n}") == 0, s); free(s);

    return goo_check_done("json_writer_test");
}
