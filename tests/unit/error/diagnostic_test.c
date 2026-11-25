// Unit tests for the diagnostic printer (include/diagnostic.h).
//
// Each row writes through diag_set_stream(tmpfile()), rewinds, and compares the
// WHOLE output string, so a stray byte anywhere fails the row.

#include "diagnostic.h"
#include "../goo_check.h"
#include <stdio.h>
#include <string.h>

static const char* SRC = "package main\n\nfunc main() {\n    x := f()\n}\n";

// Run one emission and return the captured output in a static buffer.
static const char* capture(DiagSeverity sev, Position pos, const char* help,
                           const char* msg) {
    static char buf[1024];
    FILE* f = tmpfile();
    diag_set_stream(f);
    diag_emit(sev, pos, NULL, help, "%s", msg);
    fflush(f);
    rewind(f);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    diag_set_stream(NULL);
    return buf;
}

static Position at(const char* file, int line, int col) {
    Position p = {.line = line, .column = col, .offset = 0, .filename = file};
    return p;
}

static void expect_eq(const char* got, const char* want, const char* what) {
    char label[256];
    snprintf(label, sizeof label, "%s: got \"%.100s\"", what, got);
    goo_check(strcmp(got, want) == 0, label);
}

int main(void) {
    goo_check_expect(14);

    goo_check_row(1, "first_line_and_caret");
    diag_register_source("t.goo", SRC, strlen(SRC));
    expect_eq(capture(DIAG_ERROR, at("t.goo", 4, 10), NULL, "m"),
              "t.goo:4:10: error: m\n    4 |     x := f()\n      |          ^\n",
              "caret");

    goo_check_row(2, "no_position");
    expect_eq(capture(DIAG_ERROR, at(NULL, 4, 10), NULL, "m"),
              "goo: error: m\n", "null filename");
    expect_eq(capture(DIAG_ERROR, at("t.goo", 0, 10), NULL, "m"),
              "goo: error: m\n", "line 0");

    goo_check_row(3, "warning_severity");
    goo_check(strstr(capture(DIAG_WARNING, at("t.goo", 4, 10), NULL, "m"),
                     ": warning: m") != NULL, "warning text");

    goo_check_row(4, "tab_pad");
    diag_register_source("tab.goo", "\tx := 1", 7);
    expect_eq(capture(DIAG_ERROR, at("tab.goo", 1, 2), NULL, "m"),
              "tab.goo:1:2: error: m\n    1 | \tx := 1\n      | \t^\n", "tab");

    goo_check_row(5, "utf8_pad");
    // pi is two bytes, so y sits at byte column 7.
    diag_register_source("u.goo", "\xcf\x80 := y", 7);
    expect_eq(capture(DIAG_ERROR, at("u.goo", 1, 7), NULL, "m"),
              "u.goo:1:7: error: m\n    1 | \xcf\x80 := y\n      |      ^\n",
              "utf8");

    goo_check_row(6, "line_1");
    expect_eq(capture(DIAG_ERROR, at("t.goo", 1, 1), NULL, "m"),
              "t.goo:1:1: error: m\n    1 | package main\n      | ^\n", "line 1");

    goo_check_row(7, "last_line_no_newline");
    diag_register_source("n.goo", "a\nbcd", 5);
    expect_eq(capture(DIAG_ERROR, at("n.goo", 2, 3), NULL, "m"),
              "n.goo:2:3: error: m\n    2 | bcd\n      |   ^\n", "last line");

    goo_check_row(8, "unregistered_file");
    expect_eq(capture(DIAG_ERROR, at("zzz.goo", 1, 1), NULL, "m"),
              "zzz.goo:1:1: error: m\n", "unregistered");

    goo_check_row(9, "past_eof");
    diag_register_source("three.goo", "a\nb\nc\n", 6);
    expect_eq(capture(DIAG_ERROR, at("three.goo", 99, 1), NULL, "m"),
              "three.goo:99:1: error: m\n", "past eof");

    goo_check_row(10, "crlf");
    diag_register_source("crlf.goo", "ab\r\ncd\r\n", 8);
    expect_eq(capture(DIAG_ERROR, at("crlf.goo", 1, 2), NULL, "m"),
              "crlf.goo:1:2: error: m\n    1 | ab\n      |  ^\n", "crlf");

    goo_check_row(11, "col_out_of_range");
    diag_register_source("r.goo", "abcdef", 6);
    expect_eq(capture(DIAG_ERROR, at("r.goo", 1, 0), NULL, "m"),
              "r.goo:1:0: error: m\n    1 | abcdef\n      | ^\n", "col 0");
    expect_eq(capture(DIAG_ERROR, at("r.goo", 1, 500), NULL, "m"),
              "r.goo:1:500: error: m\n    1 | abcdef\n      |       ^\n",
              "col 500");

    goo_check_row(12, "help_lines");
    expect_eq(capture(DIAG_ERROR, at("t.goo", 4, 10),
                      "use try f()\nor f() catch e { ... }", "m"),
              "t.goo:4:10: error: m\n    4 |     x := f()\n      |          ^\n"
              "  help: use try f()\n        or f() catch e { ... }\n", "help");

    goo_check_row(13, "source_call_text");
    const char* csrc = "x := f(1, g(2))\ny := h(\")\", ')')\nz := k(1,\n2)\n"
                       "w := qir.Alloc()\nv := 7\n";
    diag_register_source("c.goo", csrc, strlen(csrc));
    char cb[64];
    goo_check(diag_source_call_text(at("c.goo", 1, 6), cb, sizeof cb) &&
              strcmp(cb, "f(1, g(2))") == 0, "call with arguments");
    goo_check(diag_source_call_text(at("c.goo", 2, 6), cb, sizeof cb) &&
              strcmp(cb, "h(\")\", ')')") == 0, "string and rune hold ')'");
    goo_check(!diag_source_call_text(at("c.goo", 3, 6), cb, sizeof cb),
              "call that runs past a newline");
    goo_check(diag_source_call_text(at("c.goo", 5, 6), cb, sizeof cb) &&
              strcmp(cb, "qir.Alloc()") == 0, "qualified call");
    goo_check(!diag_source_call_text(at("c.goo", 1, 6), cb, 5), "buffer too small");
    goo_check(!diag_source_call_text(at("c.goo", 6, 6), cb, sizeof cb),
              "not a call");

    goo_check_row(14, "source_call_text_tail");
    const char* tsrc = "x := t.m(1).n(2)\nx := f(1)[0]\nx := f(1) // c\n"
                       "x := f(\"a\\\")\")\nx := f(`)`)\nx := f(1);\nx := f(1) y\n";
    diag_register_source("t2.goo", tsrc, strlen(tsrc));
    goo_check(!diag_source_call_text(at("t2.goo", 1, 6), cb, sizeof cb),
              "chained call");
    goo_check(!diag_source_call_text(at("t2.goo", 2, 6), cb, sizeof cb),
              "index after call");
    goo_check(diag_source_call_text(at("t2.goo", 3, 6), cb, sizeof cb) &&
              strcmp(cb, "f(1)") == 0, "comment after call");
    goo_check(diag_source_call_text(at("t2.goo", 4, 6), cb, sizeof cb) &&
              strcmp(cb, "f(\"a\\\")\")") == 0, "escaped quote in string");
    goo_check(diag_source_call_text(at("t2.goo", 5, 6), cb, sizeof cb) &&
              strcmp(cb, "f(`)`)") == 0, "raw string holds )");
    goo_check(diag_source_call_text(at("t2.goo", 6, 6), cb, sizeof cb) &&
              strcmp(cb, "f(1)") == 0, "semicolon after call");
    goo_check(!diag_source_call_text(at("t2.goo", 7, 6), cb, sizeof cb),
              "unknown tail");

    diag_free_sources();
    return goo_check_done("diagnostic_test");
}
