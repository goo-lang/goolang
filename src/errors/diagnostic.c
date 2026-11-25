#include "diagnostic.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* name;
    char* text;
    size_t len;
} SourceEntry;

static SourceEntry* g_sources = NULL;
static size_t g_count = 0;
static size_t g_cap = 0;
static FILE* g_stream = NULL;

static FILE* out_stream(void) { return g_stream ? g_stream : stderr; }

void diag_set_stream(FILE* out) { g_stream = out; }

static SourceEntry* find_source(const char* name) {
    for (size_t i = 0; i < g_count; i++) {
        if (strcmp(g_sources[i].name, name) == 0) return &g_sources[i];
    }
    return NULL;
}

void diag_register_source(const char* filename, const char* text, size_t len) {
    if (!filename || (!text && len > 0)) return;
    // The caller may free its buffer, so keep our own NUL-terminated copy.
    char* copy = xmalloc(len + 1);
    if (len > 0) memcpy(copy, text, len);
    copy[len] = '\0';

    SourceEntry* e = find_source(filename);
    if (e) {
        free(e->text);
        e->text = copy;
        e->len = len;
        return;
    }
    if (g_count == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 8;
        g_sources = xrealloc(g_sources, g_cap * sizeof *g_sources);
    }
    g_sources[g_count].name = xstrdup(filename);
    g_sources[g_count].text = copy;
    g_sources[g_count].len = len;
    g_count++;
}

void diag_free_sources(void) {
    for (size_t i = 0; i < g_count; i++) {
        free(g_sources[i].name);
        free(g_sources[i].text);
    }
    free(g_sources);
    g_sources = NULL;
    g_count = 0;
    g_cap = 0;
}

// Find line `line` (1-based). Returns false when the file has fewer lines.
// A trailing '\r' is not part of the line.
static bool find_line(const SourceEntry* e, int line, const char** start,
                      size_t* len) {
    const char* p = e->text;
    const char* end = e->text + e->len;
    for (int n = 1; n < line; n++) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) return false;
        p = nl + 1;
    }
    // A file ending in '\n' has no line after it.
    if (p == end && line > 1 && end[-1] == '\n') return false;
    const char* nl = memchr(p, '\n', (size_t)(end - p));
    size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
    if (l > 0 && p[l - 1] == '\r') l--;
    *start = p;
    *len = l;
    return true;
}

static void print_snippet(FILE* out, const SourceEntry* e, Position pos) {
    const char* ln;
    size_t ln_len;
    if (!find_line(e, pos.line, &ln, &ln_len)) return;

    fprintf(out, "%5d | %.*s\n", pos.line, (int)ln_len, ln);
    fputs("      | ", out);
    // A column outside the line clamps: 0 and below to the start, beyond the
    // end to just after the last byte.
    size_t target = pos.column > 1 ? (size_t)pos.column - 1 : 0;
    if (target > ln_len) target = ln_len;
    for (size_t i = 0; i < target; i++) {
        unsigned char c = (unsigned char)ln[i];
        if (c == '\t') fputc('\t', out);
        else if ((c & 0xC0) != 0x80) fputc(' ', out);
    }
    fputs("^\n", out);
}

static void print_help(FILE* out, const char* help) {
    const char* p = help;
    bool first = true;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        fprintf(out, "%s%.*s\n", first ? "  help: " : "        ", (int)l, p);
        first = false;
        p += l;
        if (*p == '\n') p++;
    }
}

void diag_vemit(DiagSeverity sev, Position pos, const char* code,
                const char* help, const char* format, va_list args) {
    (void)code;
    FILE* out = out_stream();
    const char* sev_name = sev == DIAG_WARNING ? "warning" : "error";
    bool positioned = pos.filename != NULL && pos.line > 0;

    if (positioned) {
        fprintf(out, "%s:%d:%d: %s: ", pos.filename, pos.line, pos.column,
                sev_name);
    } else {
        fprintf(out, "goo: %s: ", sev_name);
    }
    vfprintf(out, format, args);
    fputc('\n', out);

    if (positioned) {
        const SourceEntry* e = find_source(pos.filename);
        if (e) print_snippet(out, e, pos);
    }
    if (help && *help) print_help(out, help);
}

void diag_emit(DiagSeverity sev, Position pos, const char* code,
               const char* help, const char* format, ...) {
    va_list args;
    va_start(args, format);
    diag_vemit(sev, pos, code, help, format, args);
    va_end(args);
}
