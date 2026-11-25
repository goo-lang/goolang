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
    goo_check_expect(12);

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

    diag_free_sources();
    return goo_check_done("diagnostic_test");
}
