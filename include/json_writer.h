#ifndef JSON_WRITER_H
#define JSON_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// Streaming JSON writer for the program dump. Deterministic by construction:
// keys are written in call order, numbers are printed with fixed formats,
// and there is no buffering or sorting that could reorder output between
// runs. That property is what lets two producers of the same tree be
// compared with `diff`.
//
// Nesting deeper than JW_MAX_DEPTH is an error, not an abort: the writer stops
// emitting and records JW_ERR_NESTING in `error`. A truncated dump that still
// parses would defeat the differential gates, so the caller must check `error`
// and must not use the partial output.
#define JW_MAX_DEPTH 512
#define JW_ERR_NESTING 1

typedef struct {
    FILE* out;
    int   depth;
    bool  first[JW_MAX_DEPTH];   // per open container: no element written yet
    bool  in_object[JW_MAX_DEPTH];
    bool  after_key;             // a key was written; the value follows inline
    int   error;                 // 0, or JW_ERR_NESTING once the writer has failed
} JsonW;

void jw_init(JsonW* w, FILE* out);
// The begin functions return 0, or the error code that the writer holds.
int  jw_begin_object(JsonW* w);
void jw_end_object(JsonW* w);
int  jw_begin_array(JsonW* w);
void jw_end_array(JsonW* w);
void jw_key(JsonW* w, const char* key);
void jw_string(JsonW* w, const char* s);              // NULL -> null
void jw_string_len(JsonW* w, const char* s, size_t n); // embedded NUL safe
void jw_int(JsonW* w, long long v);
void jw_uint(JsonW* w, unsigned long long v);
void jw_bool(JsonW* w, bool v);
void jw_null(JsonW* w);

// Returns the fixed diagnostic text for a JW_ERR_* code.
const char* jw_error_message(int error);

#endif
