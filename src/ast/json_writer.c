#include "json_writer.h"
#include <stdlib.h>
#include <string.h>

static void die(const char* what) {
    fprintf(stderr, "json_writer: %s\n", what);
    abort();
}

void jw_init(JsonW* w, FILE* out) {
    memset(w, 0, sizeof(*w));
    w->out = out;
}

static void indent(JsonW* w) {
    for (int i = 0; i < w->depth; i++) fputs("  ", w->out);
}

// Called before any value or key: writes the separator/newline/indent that
// positions the next element. A value directly after a key stays inline.
static void before_element(JsonW* w) {
    if (w->after_key) { w->after_key = false; return; }
    if (w->depth == 0) return;
    if (!w->first[w->depth - 1]) fputc(',', w->out);
    w->first[w->depth - 1] = false;
    fputc('\n', w->out);
    indent(w);
}

static void open_container(JsonW* w, char c, bool is_object) {
    before_element(w);
    if (w->depth >= JW_MAX_DEPTH) die("nesting exceeds JW_MAX_DEPTH");
    fputc(c, w->out);
    w->first[w->depth] = true;
    w->in_object[w->depth] = is_object;
    w->depth++;
}

static void close_container(JsonW* w, char c) {
    if (w->depth == 0) die("close with no open container");
    w->depth--;
    if (!w->first[w->depth]) { fputc('\n', w->out); indent(w); }
    fputc(c, w->out);
}

void jw_begin_object(JsonW* w) { open_container(w, '{', true); }
void jw_end_object(JsonW* w)   { close_container(w, '}'); }
void jw_begin_array(JsonW* w)  { open_container(w, '[', false); }
void jw_end_array(JsonW* w)    { close_container(w, ']'); }

static void write_escaped(JsonW* w, const char* s, size_t n) {
    fputc('"', w->out);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  fputs("\\\"", w->out); break;
            case '\\': fputs("\\\\", w->out); break;
            case '\n': fputs("\\n", w->out); break;
            case '\t': fputs("\\t", w->out); break;
            case '\r': fputs("\\r", w->out); break;
            default:
                if (c < 0x20) fprintf(w->out, "\\u%04x", c);
                else fputc(c, w->out);   // UTF-8 bytes pass through verbatim
        }
    }
    fputc('"', w->out);
}

void jw_key(JsonW* w, const char* key) {
    if (w->depth == 0 || !w->in_object[w->depth - 1]) die("key outside an object");
    before_element(w);
    write_escaped(w, key, strlen(key));
    fputs(": ", w->out);
    w->after_key = true;
}

void jw_string_len(JsonW* w, const char* s, size_t n) {
    before_element(w);
    if (!s) { fputs("null", w->out); return; }
    write_escaped(w, s, n);
}

void jw_string(JsonW* w, const char* s) { jw_string_len(w, s, s ? strlen(s) : 0); }

void jw_int(JsonW* w, long long v)           { before_element(w); fprintf(w->out, "%lld", v); }
void jw_uint(JsonW* w, unsigned long long v) { before_element(w); fprintf(w->out, "%llu", v); }
void jw_bool(JsonW* w, bool v)               { before_element(w); fputs(v ? "true" : "false", w->out); }
void jw_null(JsonW* w)                       { before_element(w); fputs("null", w->out); }
