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
// Nesting deeper than JW_MAX_DEPTH aborts rather than corrupting the output:
// a truncated dump that still parses would defeat the differential gates.
#define JW_MAX_DEPTH 512

typedef struct {
    FILE* out;
    int   depth;
    bool  first[JW_MAX_DEPTH];   // per open container: no element written yet
    bool  in_object[JW_MAX_DEPTH];
    bool  after_key;             // a key was written; the value follows inline
} JsonW;

void jw_init(JsonW* w, FILE* out);
void jw_begin_object(JsonW* w);
void jw_end_object(JsonW* w);
void jw_begin_array(JsonW* w);
void jw_end_array(JsonW* w);
void jw_key(JsonW* w, const char* key);
void jw_string(JsonW* w, const char* s);              // NULL -> null
void jw_string_len(JsonW* w, const char* s, size_t n); // embedded NUL safe
void jw_int(JsonW* w, long long v);
void jw_uint(JsonW* w, unsigned long long v);
void jw_bool(JsonW* w, bool v);
void jw_null(JsonW* w);

#endif
