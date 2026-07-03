#include "runtime.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

// Global runtime state
static struct {
    int argc;
    char** argv;
    int initialized;
} goo_runtime = {0};

// Program initialization and cleanup

void goo_init(int argc, char** argv) {
    if (goo_runtime.initialized) {
        return;  // Already initialized
    }
    
    goo_runtime.argc = argc;
    goo_runtime.argv = argv;
    goo_runtime.initialized = 1;
    
    // Initialize deadlock detection
    goo_deadlock_init();
    
    // Platform-specific initialization could go here
}

void goo_exit(int code) {
    // Cleanup runtime resources
    goo_deadlock_shutdown();
    goo_runtime.initialized = 0;
    exit(code);
}

// Memory management

void* goo_alloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    
    void* ptr = malloc(size);
    if (!ptr) {
        goo_panic("Out of memory");
    }
    
    return ptr;
}

void* goo_realloc(void* ptr, size_t size) {
    if (size == 0) {
        goo_free(ptr);
        return NULL;
    }
    
    void* new_ptr = realloc(ptr, size);
    if (!new_ptr) {
        goo_panic("Out of memory");
    }
    
    return new_ptr;
}

void goo_free(void* ptr) {
    if (ptr) {
        free(ptr);
    }
}

// Error handling

// Decode the UTF-8 rune at data[i] (caller guarantees i < len). Writes the rune
// to *rune_out and returns its byte width (1..4). On an invalid lead byte,
// truncated sequence, or bad continuation byte, writes 0xFFFD (utf8.RuneError)
// and returns width 1 — matching Go's for-range-over-string behavior. v1
// limitation: overlong encodings and surrogate halves are decoded as-is rather
// than mapped to RuneError; valid UTF-8 (the common case) is exact.
int32_t goo_utf8_decode(const char* data, int64_t len, int64_t i, int32_t* rune_out) {
    unsigned char b0 = (unsigned char)data[i];
    if (b0 < 0x80) { *rune_out = (int32_t)b0; return 1; }
    int n;
    int32_t r;
    if ((b0 & 0xE0) == 0xC0)      { n = 2; r = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { n = 3; r = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { n = 4; r = b0 & 0x07; }
    else { *rune_out = 0xFFFD; return 1; }          // invalid lead byte
    if (i + n > len) { *rune_out = 0xFFFD; return 1; } // truncated
    for (int k = 1; k < n; k++) {
        unsigned char bk = (unsigned char)data[i + k];
        if ((bk & 0xC0) != 0x80) { *rune_out = 0xFFFD; return 1; } // bad continuation
        r = (r << 6) | (bk & 0x3F);
    }
    *rune_out = r;
    return (int32_t)n;
}

void goo_panic(const char* message) {
    fprintf(stderr, "panic: %s\n", message ? message : "unknown error");
    fflush(stderr);
    abort();
}

goo_error_t* goo_new_error(const char* message) {
    return goo_new_error_with_code(message, -1);
}

goo_error_t* goo_new_error_with_code(const char* message, int code) {
    goo_error_t* error = goo_alloc(sizeof(goo_error_t));
    
    if (message) {
        size_t len = strlen(message);
        char* msg_copy = goo_alloc(len + 1);
        strcpy(msg_copy, message);
        error->message = msg_copy;
    } else {
        error->message = NULL;
    }
    
    error->code = code;
    error->cause = NULL;
    
    return error;
}

// Build a heap goo_error from a goo_string message with an explicit cause.
// Copies length bytes plus a trailing NUL (goo_string.data is not assumed
// NUL-terminated). code=-1.
goo_error_t* goo_error_wrap(goo_string_t msg, goo_error_t* cause) {
    goo_error_t* error = goo_alloc(sizeof(goo_error_t));
    size_t len = msg.length;
    char* copy = goo_alloc(len + 1);
    if (msg.data && len > 0) {
        memcpy(copy, msg.data, len);
    }
    copy[len] = '\0';
    error->message = copy;
    error->code = -1;
    error->cause = cause;
    return error;
}

// Unchanged behavior: box a message with no cause.
goo_error_t* goo_error_from_string(goo_string_t msg) {
    return goo_error_wrap(msg, NULL);
}

// Return the wrapped cause (or NULL if none / e is NULL).
goo_error_t* goo_error_unwrap(goo_error_t* e) {
    return e ? e->cause : NULL;
}

// Return the message as a goo_string. strlen on read (embedded-NUL truncates —
// accepted v1 edge; error messages are ASCII in practice).
goo_string_t goo_error_message(goo_error_t* e) {
    goo_string_t s;
    if (e && e->message) {
        s.data = (char*)e->message;
        s.length = strlen(e->message);
    } else {
        s.data = NULL;
        s.length = 0;
    }
    return s;
}

void goo_error_free(goo_error_t* error) {
    if (!error) return;
    
    if (error->message) {
        goo_free((void*)error->message);
    }
    
    if (error->cause) {
        goo_error_free(error->cause);
    }
    
    goo_free(error);
}

// I/O functions

void goo_print(const char* message) {
    if (message) {
        printf("%s", message);
        fflush(stdout);
    }
}

void goo_println(const char* message) {
    if (message) {
        printf("%s\n", message);
    } else {
        printf("\n");
    }
    fflush(stdout);
}

void goo_print_string(goo_string_t str) {
    if (str.data && str.length > 0) {
        printf("%.*s", (int)str.length, str.data);
        fflush(stdout);
    }
}

void goo_println_string(goo_string_t str) {
    goo_print_string(str);
    printf("\n");
    fflush(stdout);
}

void goo_println_int(int64_t value) {
    printf("%lld\n", (long long)value);
    fflush(stdout);
}

void goo_println_bool(int value) {
    printf("%s\n", value ? "true" : "false");
    fflush(stdout);
}

// Unsigned integer printers: a uint64 value above INT64_MAX would print as a
// negative number through the signed goo_print_int, so unsigned types
// (uint/uint8/16/32/64, byte) route here. Codegen zero-extends the value to
// u64 before the call.
void goo_println_uint(uint64_t value) {
    printf("%llu\n", (unsigned long long)value);
    fflush(stdout);
}

void goo_println_float(double value) {
    printf("%g\n", value);
    fflush(stdout);
}

// Print-without-newline variants, used by variadic fmt.Println codegen
// (M10-variadic-println) to emit each arg, then a single trailing
// newline. Without these, codegen had to call the println variant per
// arg, producing N newlines for an N-arg call — wrong shape.
void goo_print_int(int64_t value) {
    printf("%lld", (long long)value);
    fflush(stdout);
}

void goo_print_bool(int value) {
    printf("%s", value ? "true" : "false");
    fflush(stdout);
}

void goo_print_uint(uint64_t value) {
    printf("%llu", (unsigned long long)value);
    fflush(stdout);
}

void goo_print_float(double value) {
    printf("%g", value);
    fflush(stdout);
}

// String operations

goo_string_t goo_string_new(const char* data) {
    if (!data) {
        return (goo_string_t){NULL, 0};
    }
    
    return goo_string_new_with_length(data, strlen(data));
}

goo_string_t goo_string_new_with_length(const char* data, size_t length) {
    if (!data || length == 0) {
        return (goo_string_t){NULL, 0};
    }
    
    char* copy = goo_alloc(length + 1);  // +1 for null terminator
    memcpy(copy, data, length);
    copy[length] = '\0';
    
    return (goo_string_t){copy, length};
}

void goo_string_free(goo_string_t str) {
    if (str.data) {
        goo_free(str.data);
    }
}

goo_string_t goo_string_concat(goo_string_t a, goo_string_t b) {
    if (!a.data || a.length == 0) {
        return goo_string_new_with_length(b.data, b.length);
    }
    
    if (!b.data || b.length == 0) {
        return goo_string_new_with_length(a.data, a.length);
    }
    
    size_t total_length = a.length + b.length;
    char* result = goo_alloc(total_length + 1);
    
    memcpy(result, a.data, a.length);
    memcpy(result + a.length, b.data, b.length);
    result[total_length] = '\0';

    return (goo_string_t){result, total_length};
}

// Scalar-to-string conversions — heap-allocated via goo_alloc (same allocator
// as goo_string_concat). Used by fmt.Sprintf / strconv.
goo_string_t goo_int_to_string(int64_t value) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)value);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        goo_panic("goo_int_to_string: snprintf overflow");
    }
    char* data = (char*)goo_alloc((size_t)n + 1);
    memcpy(data, buf, (size_t)n + 1);
    goo_string_t s; s.data = data; s.length = (size_t)n; return s;
}

goo_string_t goo_float_to_string(double value) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%g", value);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        goo_panic("goo_float_to_string: snprintf overflow");
    }
    char* data = (char*)goo_alloc((size_t)n + 1);
    memcpy(data, buf, (size_t)n + 1);
    goo_string_t s; s.data = data; s.length = (size_t)n; return s;
}

int goo_string_to_int(goo_string_t s, int64_t* out) {
    if (!out || s.length == 0) return 0;
    // Copy to a NUL-terminated stack buffer (s.data may be a non-terminated slice).
    char buf[32];
    if (s.length >= sizeof(buf)) return 0;  // too long to be a valid int64
    memcpy(buf, s.data, s.length);
    buf[s.length] = '\0';
    errno = 0;
    char* end = NULL;
    long long v = strtoll(buf, &end, 10);
    if (errno != 0) return 0;          // overflow/underflow
    if (end == buf || *end != '\0') return 0;  // empty or trailing junk
    *out = (int64_t)v;
    return 1;
}

goo_string_t goo_bool_to_string(int value) {
    const char* lit = value ? "true" : "false";
    size_t n = strlen(lit);
    char* data = (char*)goo_alloc(n + 1);
    memcpy(data, lit, n + 1);
    goo_string_t s; s.data = data; s.length = n; return s;
}

// P1-1: byte-wise string value equality for `==`/`!=`. Two strings are equal
// iff they have the same length and the same bytes. A nil/empty data pointer
// is treated as the empty string, so "" == "" and nil == "" are equal.
int goo_string_eq(goo_string_t a, goo_string_t b) {
    if (a.length != b.length) return 0;
    if (a.length == 0) return 1;            // both empty
    if (!a.data || !b.data) return a.data == b.data;
    return memcmp(a.data, b.data, a.length) == 0;
}

// P1-2: lexicographic comparison. Compare the common prefix; if equal there,
// the shorter string sorts first. Returns -1 / 0 / 1.
int goo_string_cmp(goo_string_t a, goo_string_t b) {
    size_t n = a.length < b.length ? a.length : b.length;
    int c = 0;
    if (n > 0 && a.data && b.data) c = memcmp(a.data, b.data, n);
    if (c != 0) return c < 0 ? -1 : 1;
    if (a.length < b.length) return -1;
    if (a.length > b.length) return 1;
    return 0;
}

// Stdlib package backings

int goo_strings_contains(const char* haystack, const char* needle) {
    if (!haystack || !needle) return 0;
    return strstr(haystack, needle) != NULL;
}

// Case-mapping helper shared by ToUpper/ToLower. NULL input yields the
// empty string rather than a NULL data pointer so downstream printing
// never has to null-check.
static goo_string_t goo_strings_map_case(const char* s, int (*mapper)(int)) {
    size_t n = s ? strlen(s) : 0;
    char* out = goo_alloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        out[i] = (char)mapper((unsigned char)s[i]);
    }
    out[n] = '\0';
    return (goo_string_t){out, n};
}

goo_string_t goo_strings_to_upper(const char* s) {
    return goo_strings_map_case(s, toupper);
}

goo_string_t goo_strings_to_lower(const char* s) {
    return goo_strings_map_case(s, tolower);
}

goo_string_t goo_os_getenv(const char* name) {
    // Go's Getenv contract: unset (and NULL-name) yields "".
    const char* v = name ? getenv(name) : NULL;
    return goo_string_new(v ? v : "");
}

void goo_strings_split(goo_slice_t* out, const char* s, const char* sep) {
    // MVP contract: empty/NULL sep yields the whole string as a single
    // element (Go's per-rune split for "" is future work). Result is a
    // []string: a 3-field goo_slice_t whose `data` points at a goo_string_t
    // array, with capacity == length (Split allocates exactly `count`).
    // Written through `out` (by pointer) to avoid by-value 24-byte ABI.
    if (!out) return;
    if (!s) s = "";
    size_t sep_len = sep ? strlen(sep) : 0;
    if (sep_len == 0) {
        goo_string_t* one = goo_alloc(sizeof(goo_string_t));
        one[0] = goo_string_new(s);
        *out = (goo_slice_t){one, 1, 1};
        return;
    }

    size_t count = 1;
    for (const char* p = strstr(s, sep); p; p = strstr(p + sep_len, sep)) count++;

    goo_string_t* parts = goo_alloc(sizeof(goo_string_t) * count);
    size_t i = 0;
    const char* start = s;
    for (const char* p = strstr(start, sep); p; p = strstr(start, sep)) {
        parts[i++] = goo_string_new_with_length(start, (size_t)(p - start));
        start = p + sep_len;
    }
    parts[i] = goo_string_new(start);
    *out = (goo_slice_t){parts, count, count};
}

goo_string_t goo_strings_join(const goo_slice_t* parts, const char* sep) {
    // parts is a []string (by pointer): data points at a goo_string_t array.
    if (!parts) return goo_string_new("");
    goo_string_t* elems = (goo_string_t*)parts->data;
    if (parts->length == 0 || !elems) return goo_string_new("");
    size_t sep_len = sep ? strlen(sep) : 0;

    size_t total = 0;
    for (size_t i = 0; i < parts->length; i++) total += elems[i].length;
    total += sep_len * (parts->length - 1);

    char* out = goo_alloc(total + 1);
    char* w = out;
    for (size_t i = 0; i < parts->length; i++) {
        memcpy(w, elems[i].data, elems[i].length);
        w += elems[i].length;
        if (sep_len && i + 1 < parts->length) {
            memcpy(w, sep, sep_len);
            w += sep_len;
        }
    }
    *w = '\0';
    return (goo_string_t){out, total};
}

goo_string_t goo_strings_trim_space(const char* s) {
    if (!s) return goo_strings_map_case(NULL, toupper);
    const char* start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    const char* end = s + strlen(s);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    size_t n = (size_t)(end - start);
    char* out = goo_alloc(n + 1);
    memcpy(out, start, n);
    out[n] = '\0';
    return (goo_string_t){out, n};
}

double goo_math_sqrt(double x) {
    return sqrt(x);
}

double goo_math_pow(double x, double y) {
    return pow(x, y);
}

double goo_math_abs(double x) {
    return fabs(x);
}

double goo_math_min(double x, double y) {
    return fmin(x, y);
}

double goo_math_max(double x, double y) {
    return fmax(x, y);
}

// Map runtime: linked-list {string → int64_t value slot}. Linear scan.
// The slot holds an integer or any pointer; codegen casts per the
// declared map value type V.
typedef struct GooMapEntrySV {
    const char* key;
    int64_t value;
    struct GooMapEntrySV* next;
} GooMapEntrySV;

GooMapSV* goo_map_new_sv(void) {
    GooMapSV* m = goo_alloc(sizeof(GooMapSV));
    if (m) m->head = NULL;
    return m;
}

void goo_map_set_sv(GooMapSV* m, const char* k, int64_t v) {
    if (!m || !k) return;
    GooMapEntrySV* e = (GooMapEntrySV*)m->head;
    while (e) {
        if (strcmp(e->key, k) == 0) { e->value = v; return; }
        e = e->next;
    }
    e = goo_alloc(sizeof(GooMapEntrySV));
    if (!e) return;
    e->key = k;
    e->value = v;
    e->next = (GooMapEntrySV*)m->head;
    m->head = e;
}

int64_t goo_map_get_sv(GooMapSV* m, const char* k) {
    if (!m || !k) return 0;
    GooMapEntrySV* e = (GooMapEntrySV*)m->head;
    while (e) {
        if (strcmp(e->key, k) == 0) return e->value;
        e = e->next;
    }
    return 0;  // zero-value default (no comma-ok presence signal yet)
}

void goo_map_get_sv_ok(GooMapSV* m, const char* k, int64_t* out, int* found) {
    *out = 0;
    *found = 0;
    if (!m || !k) return;
    GooMapEntrySV* e = (GooMapEntrySV*)m->head;
    while (e) {
        if (strcmp(e->key, k) == 0) {
            *out = e->value;
            *found = 1;
            return;
        }
        e = e->next;
    }
}

// Entry count: backs len(m). Linear scan (same cost model as the rest of
// this list-based map — correctness over performance).
int64_t goo_map_len_sv(GooMapSV* m) {
    if (!m) return 0;
    int64_t n = 0;
    GooMapEntrySV* e = (GooMapEntrySV*)m->head;
    while (e) {
        n++;
        e = e->next;
    }
    return n;
}

// Map iteration cursor advance. See include/runtime.h for the full contract
// (deterministic reverse-insertion order, mid-iteration delete caveat). On
// entry *cursor is either NULL (end) or a live GooMapEntrySV*; a NULL-map
// caller passes an already-NULL cursor (never dereferences GooMapSV here).
// Returns 1 with *key_out/*val_out filled and *cursor advanced to the next
// entry; returns 0 (outs untouched) once the list is exhausted.
int goo_map_iter_next_sv(GooMapEntrySV** cursor, const char** key_out, int64_t* val_out) {
    if (!cursor || !*cursor) return 0;
    if (key_out) *key_out = (*cursor)->key;
    if (val_out) *val_out = (*cursor)->value;
    *cursor = (*cursor)->next;
    return 1;
}

// Cursor init: NULL-safe head read. See include/runtime.h — a nil map
// (Go zero value, never made) must range as zero iterations, so the NULL
// check lives here rather than in every generated range loop.
GooMapEntrySV* goo_map_iter_init_sv(GooMapSV* m) {
    return m ? (GooMapEntrySV*)m->head : NULL;
}

// Unlinks and frees the entry for key k, if present (no-op if absent or
// m/k is NULL). Backs delete(m, k).
//
// Key ownership: goo_map_set_sv above stores the caller's pointer verbatim
// (`e->key = k;`) rather than duplicating it — the map never owns key
// storage. So this frees only the entry node itself (allocated via
// goo_alloc in goo_map_set_sv); freeing dead->key would free memory the
// map doesn't own (e.g. a string literal's constant data).
void goo_map_delete_sv(GooMapSV* m, const char* k) {
    if (!m || !k) return;
    GooMapEntrySV* prev = NULL;
    GooMapEntrySV* e = (GooMapEntrySV*)m->head;
    while (e) {
        if (strcmp(e->key, k) == 0) {
            if (prev) {
                prev->next = e->next;
            } else {
                m->head = e->next;
            }
            goo_free(e);
            return;
        }
        prev = e;
        e = e->next;
    }
}

// Slice operations

// Zero-initialized backing store for make([]T, n[, cap]). calloc (not
// goo_alloc/malloc) because Go guarantees zero values for made elements.
// Never returns NULL: count 0 still yields a valid 1-byte allocation so a
// zero-length slice keeps a non-NULL data pointer (matches the runtime's
// "null slice" panic convention in goo_slice_get), and OOM panics the way
// goo_alloc does rather than handing codegen a NULL to dereference.
void* goo_slice_alloc(int64_t count, int64_t elem_size) {
    void* data;
    if (count <= 0 || elem_size <= 0) {
        data = calloc(1, 1);
    } else {
        data = calloc((size_t)count, (size_t)elem_size);
    }
    if (!data) {
        goo_panic("Out of memory");
    }
    return data;
}

goo_slice_t goo_slice_new(size_t element_size, size_t capacity) {
    if (capacity == 0 || element_size == 0) {
        return (goo_slice_t){NULL, 0, 0};
    }
    
    void* data = goo_alloc(element_size * capacity);
    return (goo_slice_t){data, 0, capacity};
}

void goo_slice_free(goo_slice_t slice) {
    if (slice.data) {
        goo_free(slice.data);
    }
}

void* goo_slice_get(goo_slice_t slice, size_t index, size_t element_size) {
    if (!slice.data) {
        goo_panic("slice access on null slice");
    }
    
    if (index >= slice.length) {
        goo_panic("slice index out of bounds");
    }
    
    return (char*)slice.data + (index * element_size);
}

int goo_slice_append(goo_slice_t* slice, void* element, size_t element_size) {
    if (!slice || !element) {
        return 0;
    }
    
    if (slice->length >= slice->capacity) {
        // Need to grow the slice
        size_t new_capacity = slice->capacity * 2;
        if (new_capacity == 0) {
            new_capacity = 1;
        }
        
        void* new_data = goo_realloc(slice->data, new_capacity * element_size);
        if (!new_data) {
            return 0;
        }
        
        slice->data = new_data;
        slice->capacity = new_capacity;
    }
    
    // Copy element to the slice
    void* dest = (char*)slice->data + (slice->length * element_size);
    memcpy(dest, element, element_size);
    slice->length++;
    
    return 1;
}

// Bounds and null checking

void goo_bounds_check(size_t index, size_t length, const char* file, int line) {
    if (index >= length) {
        fprintf(stderr, "bounds check failed at %s:%d: index %zu >= length %zu\n", 
                file, line, index, length);
        goo_panic("bounds check failed");
    }
}

void goo_null_check(void* ptr, const char* file, int line) {
    if (!ptr) {
        fprintf(stderr, "null check failed at %s:%d\n", file, line);
        goo_panic("null pointer dereference");
    }
}