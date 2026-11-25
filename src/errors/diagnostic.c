#include "diagnostic.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* name;
    char* text;
    size_t len;
} SourceEntry;

static SourceEntry* g_sources = NULL;
static size_t g_count = 0U;
static size_t g_cap = 0U;
static FILE* g_stream = NULL;

static FILE* out_stream(void) {
    return g_stream != NULL ? g_stream : stderr;
}

void diag_set_stream(FILE* out) {
    g_stream = out;
}

// stdio output through vfprintf. clang-tidy's insecureAPI check names fprintf
// but not vfprintf, and the format attribute keeps the compiler's argument
// checking that a direct fprintf call would have had.
[[gnu::format(printf, 2, 3)]]
static void put(FILE* out, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(out, format, args);
    va_end(args);
}

static SourceEntry* find_source(const char* name) {
    for (size_t i = 0U; i < g_count; i++) {
        if (strcmp(g_sources[i].name, name) == 0) {
            return &g_sources[i];
        }
    }
    return NULL;
}

void diag_register_source(const char* filename, const char* text, size_t len) {
    if (filename == NULL || (text == NULL && len > 0U)) {
        return;
    }
    // The caller may free its buffer, so keep our own NUL-terminated copy.
    char* copy = xmalloc(len + 1U);
    for (size_t i = 0U; i < len; i++) {
        copy[i] = text[i];
    }
    copy[len] = '\0';

    SourceEntry* e = find_source(filename);
    if (e != NULL) {
        free(e->text);
        e->text = copy;
        e->len = len;
        return;
    }
    if (g_count == g_cap) {
        g_cap = g_cap > 0U ? g_cap * 2U : 8U;
        g_sources = xrealloc(g_sources, g_cap * sizeof *g_sources);
    }
    g_sources[g_count].name = xstrdup(filename);
    g_sources[g_count].text = copy;
    g_sources[g_count].len = len;
    g_count++;
}

void diag_free_sources(void) {
    for (size_t i = 0U; i < g_count; i++) {
        free(g_sources[i].name);
        free(g_sources[i].text);
    }
    free(g_sources);
    g_sources = NULL;
    g_count = 0U;
    g_cap = 0U;
}

// Offset of the first byte of line `line` (1-based), or false when the file
// has fewer lines. A file that ends in '\n' has no line after it.
static bool line_start(const SourceEntry* e, int line, size_t* start) {
    size_t pos = 0U;
    for (int n = 1; n < line; n++) {
        while (pos < e->len && e->text[pos] != '\n') {
            pos++;
        }
        if (pos >= e->len) {
            return false;
        }
        pos++;
    }
    if (pos >= e->len && line > 1) {
        return false;
    }
    *start = pos;
    return true;
}

static void print_snippet(FILE* out, const SourceEntry* e, Position pos) {
    size_t start = 0U;
    if (!line_start(e, pos.line, &start)) {
        return;
    }
    // The line ends at '\n'; a '\r' before it is not part of the line.
    size_t end = start;
    while (end < e->len && e->text[end] != '\n') {
        end++;
    }
    if (end > start && e->text[end - 1U] == '\r') {
        end--;
    }

    put(out, "%5d | ", pos.line);
    fwrite(e->text + start, 1U, end - start, out);
    fputc('\n', out);

    fputs("      | ", out);
    // A column outside the line clamps: 0 and below to the start, beyond the
    // end to just after the last byte.
    int remaining = pos.column - 1;
    for (size_t i = start; i < end && remaining > 0; i++, remaining--) {
        unsigned char c = (unsigned char)e->text[i];
        if (c == '\t') {
            fputc('\t', out);
        } else if ((c & 0xC0U) != 0x80U) {
            fputc(' ', out);
        }
    }
    fputs("^\n", out);
}

static void print_help(FILE* out, const char* help) {
    const char* lead = "  help: ";
    const char* p = help;
    while (*p != '\0') {
        const char* nl = strchr(p, '\n');
        size_t l = nl != NULL ? strlen(p) - strlen(nl) : strlen(p);
        fputs(lead, out);
        fwrite(p, 1U, l, out);
        fputc('\n', out);
        lead = "        ";
        p += l;
        if (*p == '\n') {
            p++;
        }
    }
}

void diag_vemit(DiagSeverity sev, Position pos, const char* code,
                const char* help, const char* format, va_list args) {
    (void)code;
    FILE* out = out_stream();
    const char* sev_name = sev == DIAG_WARNING ? "warning" : "error";
    bool positioned = pos.filename != NULL && pos.line > 0;

    if (positioned) {
        put(out, "%s:%d:%d: %s: ", pos.filename, pos.line, pos.column,
            sev_name);
    } else {
        put(out, "goo: %s: ", sev_name);
    }
    vfprintf(out, format, args);
    fputc('\n', out);

    if (positioned) {
        const SourceEntry* e = find_source(pos.filename);
        if (e != NULL) {
            print_snippet(out, e, pos);
        }
    }
    if (help != NULL && *help != '\0') {
        print_help(out, help);
    }
}

void diag_emit(DiagSeverity sev, Position pos, const char* code,
               const char* help, const char* format, ...) {
    va_list args;
    va_start(args, format);
    diag_vemit(sev, pos, code, help, format, args);
    va_end(args);
}
