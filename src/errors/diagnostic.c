#include "diagnostic.h"
#include <stdint.h>
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
    // A NULL here is a caller bug that would silently drop the source line of
    // every later diagnostic for the file, so stop loudly. The early return
    // keeps a GOO_ASSERT_OFF build from crashing.
    GOO_ASSERT(filename != NULL);
    GOO_ASSERT(text != NULL || len == 0U);
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

static bool is_name_byte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           ((unsigned char)c >= 0x80U);
}

// Index of the byte after the closing `quote` of the literal whose opening
// quote is at `i`, or SIZE_MAX when it runs to a newline or the end of the
// text. A backslash escapes the next byte, except in a raw string.
static size_t skip_literal(const SourceEntry* e, size_t i, char quote) {
    const bool escapes = quote != '`';
    i++;
    while (i < e->len && e->text[i] != '\n') {
        if (escapes && e->text[i] == '\\') {
            i += 2U;
        } else if (e->text[i] == quote) {
            return i + 1U;
        } else {
            i++;
        }
    }
    return SIZE_MAX;
}

// Copies text[start, end) into buf and ends it with NUL. False when it does
// not fit.
static bool copy_span(const SourceEntry* e, size_t start, size_t end,
                      char* buf, size_t cap) {
    size_t n = end - start;
    if (n >= cap) {
        return false;
    }
    for (size_t k = 0U; k < n; k++) {
        buf[k] = e->text[start + k];
    }
    buf[n] = '\0';
    return true;
}

bool diag_source_call_text(Position pos, char* buf, size_t cap) {
    if (pos.filename == NULL || pos.line < 1 || pos.column < 1 || cap == 0U) {
        return false;
    }
    const SourceEntry* e = find_source(pos.filename);
    size_t i = 0U;
    if (e == NULL || !line_start(e, pos.line, &i)) {
        return false;
    }
    const size_t column = (size_t)pos.column;
    const size_t start = i + column - 1U;
    i = start;
    while (i < e->len && is_name_byte(e->text[i])) {
        i++;
    }
    if (i == start || i >= e->len || e->text[i] != '(') {
        return false;
    }
    int depth = 0;
    while (i < e->len && e->text[i] != '\n') {
        const char c = e->text[i];
        if (c == '"' || c == '\'' || c == '`') {
            i = skip_literal(e, i, c);
            if (i == SIZE_MAX) {
                return false;
            }
        } else {
            if (c == '(') {
                depth++;
            } else if (c == ')') {
                depth--;
            } else {
                /* any other byte only advances */
            }
            i++;
            if (depth == 0) {
                return copy_span(e, start, i, buf, cap);
            }
        }
    }
    return false;
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
    size_t i = start;
    while (i < end && remaining > 0) {
        unsigned char c = (unsigned char)e->text[i];
        if (c == 0x09U) {
            fputc('\t', out);
        } else if ((c & 0xC0U) != 0x80U) {
            fputc(' ', out);
        } else {
            // A UTF-8 continuation byte is part of the same character.
        }
        i++;
        remaining--;
    }
    fputs("^\n", out);
}

static void print_help(FILE* out, const char* help) {
    const char* lead = "  help: ";
    size_t i = 0U;
    while (help[i] != '\0') {
        size_t j = i;
        while (help[j] != '\0' && help[j] != '\n') {
            j++;
        }
        fputs(lead, out);
        fwrite(help + i, 1U, j - i, out);
        fputc('\n', out);
        lead = "        ";
        i = help[j] == '\n' ? j + 1U : j;
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
