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

// Returns the length (2-4) of the well-formed UTF-8 sequence starting at
// s[i] in a buffer of n bytes, or 0 if s[i] does not start one (a stray
// continuation byte, an overlong/surrogate lead, or a lead with no room for
// its continuations). A Goo string literal is a raw byte sequence, not
// necessarily valid UTF-8 (`"\xff"` decodes to one such byte), so a lone
// invalid byte must be escaped byte-wise to keep the JSON output well-formed
// -- but a GENUINE multi-byte character next to it (the corpus has "café",
// "中文", "°C") must still pass through untouched, which is what this check
// buys: only a byte that fails it falls back to \u00XX below.
static size_t utf8_seq_len(const unsigned char* s, size_t n, size_t i) {
    unsigned char c = s[i], lo2 = 0x80, hi2 = 0xBF;
    size_t len;
    if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; if (c == 0xE0) lo2 = 0xA0; if (c == 0xED) hi2 = 0x9F; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; if (c == 0xF0) lo2 = 0x90; if (c == 0xF4) hi2 = 0x8F; }
    else return 0;
    if (i + len > n) return 0;
    if (s[i + 1] < lo2 || s[i + 1] > hi2) return 0;
    for (size_t k = 2; k < len; k++) if (s[i + k] < 0x80 || s[i + k] > 0xBF) return 0;
    return len;
}

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
                if (c < 0x20) { fprintf(w->out, "\\u%04x", c); break; }
                if (c < 0x80) { fputc(c, w->out); break; }   // ASCII, verbatim
                {
                    size_t len = utf8_seq_len((const unsigned char*)s, n, i);
                    if (len) { fwrite(s + i, 1, len, w->out); i += len - 1; }   // valid UTF-8, verbatim
                    else fprintf(w->out, "\\u%04x", c);                        // invalid lone byte
                }
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
