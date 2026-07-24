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

// Definition of the shared zero-size-allocation sentinel declared in
// runtime.h (see the comment there). A single static byte: its address is
// what's shared, never its contents, so it never needs writing or reading.
unsigned char goo_zerobase;

void* goo_alloc(size_t size) {
    if (size == 0) {
        return &goo_zerobase;
    }

    void* ptr = malloc(size);
    if (!ptr) {
        goo_panic("Out of memory");
    }

    return ptr;
}

void* goo_realloc(void* ptr, size_t size) {
    // The sentinel was never handed to malloc(), so it must never be handed
    // to realloc() either — that would be undefined behavior. Growing
    // "from" it is really just a fresh allocation: by construction, every
    // zero-size allocation has nothing to preserve.
    if (ptr == &goo_zerobase) {
        ptr = NULL;
    }

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
    // The sentinel is a static byte, not a heap allocation — freeing it
    // would be undefined behavior. Every zero-size allocation aliases it,
    // so this is reached routinely (e.g. releasing an empty slice literal's
    // backing "allocation") and must be a silent no-op, not a bug.
    if (ptr && ptr != &goo_zerobase) {
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

// Go-conformant default: an unrecovered panic prints to stderr and exits
// with status 2 (no core dump). GOO_PANIC_ABORT=1 restores the old abort()
// behavior — SIGABRT + core dump — as a debugging escape hatch so a panic
// can still be caught live in a debugger or produce a core for postmortem
// analysis.
//
// The env var is read exactly once, via a constructor that runs before
// main() (same pattern as the self-registering test fixtures in
// test_framework.h) rather than a check on every goo_panic call. A
// repeated getenv() per panic would be harmless — panics are by
// definition not a hot path — but it would also make g_panic_abort racy
// under concurrent goroutines calling goo_panic on separate OS threads
// (this runtime is multithreaded; see concurrency.c). Resolving it once,
// before any thread exists, sidesteps that entirely rather than relying on
// the race being benign.
static int g_panic_abort = 0;

__attribute__((constructor))
static void goo_panic_init_abort_flag(void) {
    const char* env = getenv("GOO_PANIC_ABORT");
    g_panic_abort = (env != NULL && strcmp(env, "1") == 0);
}

void goo_panic(const char* message) {
    fprintf(stderr, "panic: %s\n", message ? message : "unknown error");
    fflush(stderr);
    if (g_panic_abort) {
        abort();
    }
    exit(2);
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

goo_string_t goo_uint_to_string(uint64_t value) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)value);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        goo_panic("goo_uint_to_string: snprintf overflow");
    }
    return goo_string_new_with_length(buf, (size_t)len);
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

// UTF-8-encode a single Unicode code point into buf (which must have room for
// at least 4 bytes). Returns the byte width written (1-4). Mirrors Go's
// utf8.EncodeRune, and is the inverse of goo_utf8_decode above. An invalid
// code point — negative, a UTF-16 surrogate half (D800-DFFF, which can never
// be a real Unicode scalar value), or beyond the max code point 10FFFF — is
// replaced with U+FFFD (the Unicode replacement character), matching Go's
// string(rune) behavior for an out-of-range conversion.
static int goo_utf8_encode_rune(int32_t r, unsigned char buf[4]) {
    uint32_t cp = (uint32_t)r;
    if (r < 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        cp = 0xFFFD;
    }
    if (cp <= 0x7F) {
        buf[0] = (unsigned char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        buf[0] = (unsigned char)(0xC0 | (cp >> 6));
        buf[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        buf[0] = (unsigned char)(0xE0 | (cp >> 12));
        buf[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (unsigned char)(0xF0 | (cp >> 18));
    buf[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

// goo_string_t goo_string_from_rune(int32_t r): backs the `string(rune)` /
// `string(byte)` conversion (Go: converting any integer type to string
// yields the UTF-8 encoding of that value's Unicode code point — see
// goo_utf8_encode_rune's doc comment for the invalid-code-point rule).
// Allocates and returns by the SAME by-value {ptr,len} convention as
// goo_string_new/goo_string_new_with_length above (mirrored exactly, not
// reinvented — this 16-byte struct is the proven precedent for the C-ABI
// struct-return boundary codegen crosses for every string-producing runtime
// call, e.g. goo_int_to_string).
goo_string_t goo_string_from_rune(int32_t r) {
    unsigned char buf[4];
    int n = goo_utf8_encode_rune(r, buf);
    return goo_string_new_with_length((const char*)buf, (size_t)n);
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

// os.Args: raw argc/argv, captured once by the generated executable's
// entry point (function_codegen.c's is_entry_main prologue — the only
// caller). Deliberately independent of the `goo_runtime` struct/goo_init
// above: goo_init is declared but never called from codegen today (see
// the comment at its call site), so piggy-backing on it would leave
// os.Args silently empty until that changes.
static struct {
    int argc;
    char** argv;
} goo_os_args_raw = {0, NULL};

void goo_os_args_init(int argc, char** argv) {
    goo_os_args_raw.argc = argc;
    goo_os_args_raw.argv = argv;
}

void goo_os_args(goo_slice_t* out) {
    if (!out) return;
    // Cached on first read: argv's backing storage is stable for the
    // process lifetime (the OS/libc owns it), so the []string built from
    // it needs building only once.
    static goo_string_t* built = NULL;
    static size_t built_count = 0;
    static int built_done = 0;
    if (!built_done) {
        size_t count = goo_os_args_raw.argc > 0 ? (size_t)goo_os_args_raw.argc : 0;
        if (count > 0) {
            built = goo_alloc(sizeof(goo_string_t) * count);
            for (size_t i = 0; i < count; i++) {
                const char* a = goo_os_args_raw.argv[i];
                built[i] = goo_string_new(a ? a : "");
            }
        }
        built_count = count;
        built_done = 1;
    }
    *out = (goo_slice_t){built, built_count, built_count};
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

// Map runtime: linked-list {int64_t key slot → int64_t value slot}. Linear
// scan. The key slot holds either a char* (STRING key_kind) or the key's
// raw bits (INLINE key_kind); the value slot holds an integer or any
// pointer. Codegen casts each slot per the declared map K/V types.

// Interface-typed map keys (Task 2): compare two boxed `{vtable, data}` key
// slots. Each of a/b is a pointer to one such heap-copied pair (see
// codegen_map_key_to_slot's TYPE_INTERFACE arm). Go interface equality: same
// dynamic type (vtable identity) AND equal dynamic value (dispatched through
// the vtable's slot 0 — now the per-type descriptor, whose field 0 is the
// concrete's value-equality comparator, codegen_get_or_emit_type_eq) on the
// two data words.
int goo_iface_key_eq(int64_t a, int64_t b) {
    void** ia = (void**)(intptr_t)a;  // -> { vtable, data }
    void** ib = (void**)(intptr_t)b;
    void* vta = ia[0]; void* vtb = ib[0];
    if (vta == NULL && vtb == NULL) return 1;   // both nil interfaces
    if (vta != vtb) return 0;                    // different dynamic type (or one nil)
    void* desc = ((void**)vta)[0];          // vtable slot 0 -> descriptor
    GooKeyEqFn eq = (GooKeyEqFn)((void**)desc)[0];  // descriptor field 0 -> eq_fn
    return eq((int64_t)(intptr_t)ia[1], (int64_t)(intptr_t)ib[1]); // compare the data words
}

// Format an interface value {vtable,data} as its %v string. nil vtable -> "<nil>".
// Encapsulates the null-check + descriptor hop so both fmt.Println's codegen
// site (call_codegen.c) and a later fmt.Sprintf site can share it.
goo_string_t goo_iface_format(void* vtable, void* data) {
    if (!vtable) return goo_string_new("<nil>");
    void* desc = ((void**)vtable)[0];              // vtable slot 0 -> descriptor
    typedef goo_string_t (*GooFmtFn)(void*);
    GooFmtFn fmt = (GooFmtFn)((void**)desc)[2];    // descriptor field 2 -> fmt_fn
    return fmt(data);
}

// Task 4: failed type-assertion panic naming the DYNAMIC (actually held)
// type. Reads descriptor field 1 (type_name) behind the same vtable slot-0
// hop as goo_iface_key_eq/goo_iface_format above; a NULL vtable (nil
// interface) renders as "<nil>". Builds the message at RUNTIME because the
// dynamic type is only known then — the static source/target names are
// still baked in by codegen as C-string globals, matching Go's own
// "interface conversion: X is Y, not Z" wording.
void goo_panic_iface_conversion(const char* iface_name, void* vtable,
                                 const char* target_name) {
    const char* dynamic = "<nil>";
    if (vtable) {
        void* desc = ((void**)vtable)[0];       // vtable slot 0 -> descriptor
        dynamic = ((const char**)desc)[1];      // descriptor field 1 -> type_name
    }
    char buf[320];
    snprintf(buf, sizeof(buf), "interface conversion: %s is %s, not %s",
             iface_name ? iface_name : "interface", dynamic,
             target_name ? target_name : "?");
    goo_panic(buf);
}

// Interface-target RTTI, Task 1: failed `x.(I)` where I is an INTERFACE —
// see the doc comment on the declaration (runtime.h) for why this is a
// distinct format from goo_panic_iface_conversion above rather than a second
// call to it (that fixed "X is Y, not Z" format has no way to render Go's
// "is not" phrasing).
void goo_panic_iface_notimpl(void* vtable, const char* target_name) {
    const char* dynamic = "<nil>";
    if (vtable) {
        void* desc = ((void**)vtable)[0];       // vtable slot 0 -> descriptor
        dynamic = ((const char**)desc)[1];      // descriptor field 1 -> type_name
    }
    char buf[320];
    snprintf(buf, sizeof(buf), "interface conversion: %s is not %s",
             dynamic, target_name ? target_name : "?");
    goo_panic(buf);
}

// Compare two int64 key slots per the map's key_kind. STRING: the slots hold
// char* — strcmp. INLINE: the slots hold the key's bits — direct ==. STRUCT:
// dispatch to the map's per-map comparator. IFACE: dispatch to the map's
// per-map comparator (goo_iface_key_eq, set at map-creation time).
static int goo_map_key_eq(const GooMapSV* m, int64_t a, int64_t b) {
    if (m->key_kind == GOO_MAPKEY_STRING) {
        const char* sa = (const char*)(intptr_t)a;
        const char* sb = (const char*)(intptr_t)b;
        if (sa == sb) return 1;
        if (!sa || !sb) return 0;
        return strcmp(sa, sb) == 0;
    }
    if (m->key_kind == GOO_MAPKEY_STRUCT) {
        // Struct keys are stored as pointers to heap copies; a synthesized
        // per-field comparator does value-equality. NULL key_eq should never
        // happen for a STRUCT map (codegen always supplies it) — fall back to
        // pointer identity defensively.
        return m->key_eq ? m->key_eq(a, b) : (a == b);
    }
    if (m->key_kind == GOO_MAPKEY_IFACE) {
        return m->key_eq ? m->key_eq(a, b) : (a == b);
    }
    return a == b;
}

typedef struct GooMapEntrySV {
    int64_t key;
    int64_t value;
    struct GooMapEntrySV* next;
} GooMapEntrySV;

GooMapSV* goo_map_new_sv(int32_t key_kind, GooKeyEqFn key_eq) {
    GooMapSV* m = goo_alloc(sizeof(GooMapSV));
    if (m) { m->head = NULL; m->key_kind = key_kind; m->key_eq = key_eq; }
    return m;
}

void goo_map_set_sv(GooMapSV* m, int64_t k, int64_t v) {
    // Go parity (P3.9, user-decided): writing to a nil map panics — unlike
    // every read-shaped op below (get/get_ok/len/delete/iter), which stay
    // NULL-tolerant and zero-value/no-op on purpose. Compound assign
    // (m[k]+=1) routes get-then-set, so this single guard covers both direct
    // and compound writes without touching codegen. Message is Go's exact
    // wording (see nil_map_write_abort_probe).
    if (!m) goo_panic("assignment to entry in nil map");
    GooMapEntrySV* e = (GooMapEntrySV*)m->head;
    while (e) {
        if (goo_map_key_eq(m, e->key, k)) { e->value = v; return; }
        e = e->next;
    }
    e = goo_alloc(sizeof(GooMapEntrySV));
    if (!e) return;
    e->key = k;
    e->value = v;
    e->next = (GooMapEntrySV*)m->head;
    m->head = e;
}

int64_t goo_map_get_sv(GooMapSV* m, int64_t k) {
    if (!m) return 0;
    GooMapEntrySV* e = (GooMapEntrySV*)m->head;
    while (e) {
        if (goo_map_key_eq(m, e->key, k)) return e->value;
        e = e->next;
    }
    return 0;  // zero-value default (no comma-ok presence signal yet)
}

void goo_map_get_sv_ok(GooMapSV* m, int64_t k, int64_t* out, int* found) {
    *out = 0;
    *found = 0;
    if (!m) return;
    GooMapEntrySV* e = (GooMapEntrySV*)m->head;
    while (e) {
        if (goo_map_key_eq(m, e->key, k)) {
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
int goo_map_iter_next_sv(GooMapEntrySV** cursor, int64_t* key_out, int64_t* val_out) {
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

// Unlinks and frees the entry for key k, if present (no-op if absent or m
// is NULL). Backs delete(m, k).
//
// Key ownership: goo_map_set_sv above stores the caller's pointer verbatim
// (`e->key = k;`) rather than duplicating it — the map never owns key
// storage. So this frees only the entry node itself (allocated via
// goo_alloc in goo_map_set_sv); freeing dead->key would free memory the
// map doesn't own (e.g. a string literal's constant data).
void goo_map_delete_sv(GooMapSV* m, int64_t k) {
    if (!m) return;
    GooMapEntrySV* prev = NULL;
    GooMapEntrySV* e = (GooMapEntrySV*)m->head;
    while (e) {
        if (goo_map_key_eq(m, e->key, k)) {
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

// Unlinks and frees every entry (no-op if m is NULL or already empty).
// Backs clear(m) (Go 1.21). Same key-ownership contract as
// goo_map_delete_sv above (frees only the entry nodes, never the key/value
// payloads a caller's own storage still owns) — one linear pass instead of
// clear's naive "delete every key" O(n^2) equivalent.
void goo_map_clear_sv(GooMapSV* m) {
    if (!m) return;
    GooMapEntrySV* e = (GooMapEntrySV*)m->head;
    while (e) {
        GooMapEntrySV* next = e->next;
        goo_free(e);
        e = next;
    }
    m->head = NULL;
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

// []byte(s) / string(b) conversion cores (Task 2, stdlib unblocker). Go
// copies on both conversions — codegen builds the destination header
// ({ptr,len,cap} slice or {ptr,len} string) around whatever buffer these
// return, so the result never aliases the source's backing store.
void* goo_bytes_from_string(const char* p, int64_t len) {
    if (len < 0) len = 0;
    void* data = goo_alloc(len > 0 ? (size_t)len : 1);
    if (len > 0) memcpy(data, p, (size_t)len);
    return data;
}

char* goo_cstr_from_bytes(void* data, int64_t len) {
    if (len < 0) len = 0;
    char* s = (char*)goo_alloc((size_t)len + 1);
    if (len > 0) memcpy(s, data, (size_t)len);
    s[len] = '\0';   // known rep limitation: embedded NULs truncate downstream C-string ops
    return s;
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

// Capacity-doubling policy shared by goo_slice_append and
// goo_slice_append_bulk: double from `capacity` (or start at 1 if empty),
// looping until it covers `need`. A single-element append only ever needs
// one doubling step, but a bulk append's src_len can outgrow that in one
// call, hence the loop (a no-op when one step already covers `need`).
static size_t goo_slice_grow_capacity(size_t capacity, size_t need) {
    size_t new_capacity = capacity * 2;
    if (new_capacity == 0) {
        new_capacity = 1;
    }
    while (new_capacity < need) {
        new_capacity *= 2;
    }
    return new_capacity;
}

int goo_slice_append(goo_slice_t* slice, void* element, size_t element_size) {
    if (!slice || !element) {
        return 0;
    }

    if (slice->length >= slice->capacity) {
        // Need to grow the slice
        size_t new_capacity = goo_slice_grow_capacity(slice->capacity, slice->length + 1);

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

int64_t goo_slice_copy_raw(void* dst, int64_t dst_len,
                           const void* src, int64_t src_len, int64_t elem_size) {
    int64_t n = dst_len < src_len ? dst_len : src_len;
    if (n <= 0 || elem_size <= 0) {
        return 0;
    }
    memmove(dst, src, (size_t)(n * elem_size));
    return n;
}

void goo_slice_append_bulk(goo_slice_t* dst, const void* src,
                           int64_t src_len, int64_t elem_size) {
    if (!dst || !src || src_len <= 0 || elem_size <= 0) {
        return;
    }

    size_t bytes = (size_t)src_len * (size_t)elem_size;
    // Snapshot BEFORE growing dst: self-append (append(b, b...)) has src
    // aliasing dst's CURRENT backing array, and the grow below may
    // goo_realloc that exact block out from under src (freed or moved).
    void* snap = goo_alloc(bytes);
    memcpy(snap, src, bytes);

    size_t need = dst->length + (size_t)src_len;
    if (need > dst->capacity) {
        // Same capacity-doubling policy as goo_slice_append, via the shared helper.
        size_t new_capacity = goo_slice_grow_capacity(dst->capacity, need);

        void* new_data = goo_realloc(dst->data, new_capacity * elem_size);
        if (!new_data) {
            goo_free(snap);
            return;
        }

        dst->data = new_data;
        dst->capacity = new_capacity;
    }

    memcpy((char*)dst->data + dst->length * (size_t)elem_size, snap, bytes);
    dst->length += (size_t)src_len;
    goo_free(snap);
}

// Bounds and null checking
//
// arc-17: the unconditional-fail bodies below are what USED TO be inline in
// goo_bounds_check/goo_slice_bounds_check's `if` blocks. Codegen now emits
// the `index >= length` (resp. the three slice-bound comparisons) as an
// inline icmp + cond-br directly in the caller's IR and only calls into the
// runtime on the (cold, noreturn) failure edge — see
// codegen_emit_bounds_check in src/codegen/composite_codegen.c for why: the
// runtime is a prebuilt archive (no LTO), so no attribute set on a
// conditionally-invoked opaque call can let LLVM treat the common case as
// branch-free or hoist/eliminate provably-safe checks. Message formats are
// BYTE-IDENTICAL to the pre-arc-17 text (golden probes pin them).
void goo_bounds_fail(size_t index, size_t length, const char* file, int line) {
    fprintf(stderr, "bounds check failed at %s:%d: index %zu >= length %zu\n",
            file, line, index, length);
    goo_panic("bounds check failed");
}

// ADR 0001: cold noreturn target of the inline nil checks
// (codegen_emit_nil_check) at pointer-deref/field/interface-dispatch
// sites — same shape as goo_bounds_fail above. The message text is Go's
// canonical nil-panic wording and is pinned by scripts/nil_deref_probe.sh;
// changing it is a contract change, not a wording tweak.
void goo_nil_deref_fail(const char* file, int line) {
    fprintf(stderr, "nil dereference at %s:%d\n", file, line);
    goo_panic("runtime error: invalid memory address or nil pointer dereference");
}

// F5 follow-up: bounds check for slice/substring EXPRESSIONS `base[low:high]`,
// the sibling of goo_bounds_fail (which guards single-element index reads/
// writes). Go's rule: 0 <= low <= high <= max, where max is cap(base) for a
// slice and len(base) for a string (strings have no cap). Signed int64_t
// params (not size_t) because a negative low must compare as negative, not
// wrap to a huge unsigned value — the caller widens via sign-extension so a
// literal -1 arrives here as -1, and low < 0 catches it directly.
void goo_slice_bounds_fail(int64_t low, int64_t high, int64_t max, const char* file, int line) {
    fprintf(stderr,
            "slice bounds out of range at %s:%d: [%lld:%lld] with max %lld\n",
            file, line, (long long)low, (long long)high, (long long)max);
    goo_panic("slice bounds out of range");
}

// Thin conditional wrappers kept for ABI/source compatibility (any
// pre-arc-17-compiled object, or a caller outside codegen's own emitter,
// still links and behaves identically) — codegen itself no longer emits
// calls to these; see goo_bounds_fail/goo_slice_bounds_fail above.
void goo_bounds_check(size_t index, size_t length, const char* file, int line) {
    if (index >= length) {
        goo_bounds_fail(index, length, file, line);
    }
}

void goo_slice_bounds_check(int64_t low, int64_t high, int64_t max, const char* file, int line) {
    if (low < 0 || high < low || high > max) {
        goo_slice_bounds_fail(low, high, max, file, line);
    }
}
