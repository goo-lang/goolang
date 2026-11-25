#ifndef GOO_DIAGNOSTIC_H
#define GOO_DIAGNOSTIC_H

// The one printer for user-facing compiler messages.
//
// First line:  <file>:<line>:<col>: <severity>: <message>
// No position: goo: <severity>: <message>
// Then, when the file's text is registered, a source snippet and a caret line,
// and an optional help block. Extra lines always start with whitespace so a
// tool that reads only the first line of each message sees one line each.

#include "token.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

typedef enum { DIAG_ERROR, DIAG_WARNING } DiagSeverity;

// `code` is accepted and ignored. `help` may be NULL; a multi-line help text
// is split on '\n'. Position.column is a 1-based BYTE offset.
[[gnu::format(printf, 5, 6)]]
void diag_emit(DiagSeverity sev, Position pos, const char* code,
               const char* help, const char* format, ...);

void diag_vemit(DiagSeverity sev, Position pos, const char* code,
                const char* help, const char* format, va_list args);

// Stores a private copy of the text. A second call for the same name
// replaces the first.
void diag_register_source(const char* filename, const char* text, size_t len);

void diag_free_sources(void);

// Test seam. NULL restores stderr.
void diag_set_stream(FILE* out);

#endif
