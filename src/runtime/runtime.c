#include "runtime.h"
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

void goo_println_float(double value) {
    printf("%g\n", value);
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

// Stdlib package backings

int goo_strings_contains(const char* haystack, const char* needle) {
    if (!haystack || !needle) return 0;
    return strstr(haystack, needle) != NULL;
}

double goo_math_sqrt(double x) {
    return sqrt(x);
}

// Map runtime: linked-list {string → int}. Linear scan.
typedef struct GooMapEntrySI {
    const char* key;
    int value;
    struct GooMapEntrySI* next;
} GooMapEntrySI;

GooMapSI* goo_map_new_si(void) {
    GooMapSI* m = goo_alloc(sizeof(GooMapSI));
    if (m) m->head = NULL;
    return m;
}

void goo_map_set_si(GooMapSI* m, const char* k, int v) {
    if (!m || !k) return;
    GooMapEntrySI* e = (GooMapEntrySI*)m->head;
    while (e) {
        if (strcmp(e->key, k) == 0) { e->value = v; return; }
        e = e->next;
    }
    e = goo_alloc(sizeof(GooMapEntrySI));
    if (!e) return;
    e->key = k;
    e->value = v;
    e->next = (GooMapEntrySI*)m->head;
    m->head = e;
}

int goo_map_get_si(GooMapSI* m, const char* k) {
    if (!m || !k) return 0;
    GooMapEntrySI* e = (GooMapEntrySI*)m->head;
    while (e) {
        if (strcmp(e->key, k) == 0) return e->value;
        e = e->next;
    }
    return 0;  // zero-value default
}

// Slice operations

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