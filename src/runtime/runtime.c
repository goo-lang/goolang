#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

// Global runtime state
static struct {
    int argc;
    char** argv;
    int initialized;
} goo_runtime = {0};

int goo_runtime_verbose(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("GOO_DEBUG");
        cached = (v && v[0] && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return cached;
}

// Program initialization and cleanup

void goo_init(int argc, char** argv) {
    if (goo_runtime.initialized) {
        return;  // Already initialized
    }
    
    goo_runtime.argc = argc;
    goo_runtime.argv = argv;
    goo_runtime.initialized = 1;
    
    // Initialize memory management
    goo_memory_init();
    
    // Initialize deadlock detection
    goo_deadlock_init();
    
    // Initialize error handling system
    goo_error_handling_integrate_runtime();
    
    // Platform-specific initialization could go here
}

void goo_exit(int code) {
    // Cleanup runtime resources
    goo_deadlock_shutdown();
    
    // Shutdown error handling system
    goo_error_system_shutdown();
    
    // Shutdown memory management
    goo_memory_shutdown();
    
    goo_runtime.initialized = 0;
    exit(code);
}

// Memory management

// Ownership-based memory management state
static struct {
    int initialized;
    
    // Arena allocators
    void** arenas;
    size_t arena_count;
    size_t arena_capacity;
    
    // Reference counting
    void** rc_objects;
    size_t rc_object_count;
    size_t rc_object_capacity;
    
    // Statistics
    size_t total_stack_allocations;
    size_t total_heap_allocations;
    size_t total_arena_allocations;
    size_t total_moves;
    size_t total_borrows;
    size_t active_references;
    
} goo_memory_state = {0};

// Initialize ownership-based memory management
void goo_memory_init(void) {
    if (goo_memory_state.initialized) {
        return;
    }
    
    // Initialize arena allocator pools
    goo_memory_state.arena_capacity = 16;
    goo_memory_state.arenas = malloc(sizeof(void*) * goo_memory_state.arena_capacity);
    if (!goo_memory_state.arenas) {
        goo_panic("Failed to initialize arena allocators");
    }
    goo_memory_state.arena_count = 0;
    
    // Initialize reference counting system
    goo_memory_state.rc_object_capacity = 32;
    goo_memory_state.rc_objects = malloc(sizeof(void*) * goo_memory_state.rc_object_capacity);
    if (!goo_memory_state.rc_objects) {
        goo_panic("Failed to initialize reference counting system");
    }
    goo_memory_state.rc_object_count = 0;
    
    // Reset statistics
    goo_memory_state.total_stack_allocations = 0;
    goo_memory_state.total_heap_allocations = 0;
    goo_memory_state.total_arena_allocations = 0;
    goo_memory_state.total_moves = 0;
    goo_memory_state.total_borrows = 0;
    goo_memory_state.active_references = 0;
    
    // Initialize arena allocation system
    goo_arena_init_system();
    
    // Initialize concurrency runtime (scheduler and channels)
    goo_scheduler_init(1); // Single-threaded by default, can be configured
    
    goo_memory_state.initialized = 1;
}

// Shutdown memory management
void goo_memory_shutdown(void) {
    if (!goo_memory_state.initialized) {
        return;
    }
    
    // Clean up arena allocators
    for (size_t i = 0; i < goo_memory_state.arena_count; i++) {
        if (goo_memory_state.arenas[i]) {
            free(goo_memory_state.arenas[i]);
        }
    }
    free(goo_memory_state.arenas);
    
    // Report any leaked references
    if (goo_memory_state.active_references > 0) {
        fprintf(stderr, "Warning: %zu references were not properly dropped\n", 
                goo_memory_state.active_references);
    }
    
    // Clean up reference counting objects
    for (size_t i = 0; i < goo_memory_state.rc_object_count; i++) {
        if (goo_memory_state.rc_objects[i]) {
            free(goo_memory_state.rc_objects[i]);
        }
    }
    free(goo_memory_state.rc_objects);
    
    // Print final statistics (diagnostic only)
    if (goo_runtime_verbose()) {
        printf("Memory management statistics:\n");
        printf("  Stack allocations: %zu\n", goo_memory_state.total_stack_allocations);
        printf("  Heap allocations: %zu\n", goo_memory_state.total_heap_allocations);
        printf("  Arena allocations: %zu\n", goo_memory_state.total_arena_allocations);
        printf("  Move operations: %zu\n", goo_memory_state.total_moves);
        printf("  Borrow operations: %zu\n", goo_memory_state.total_borrows);
    }
    
    // Cleanup concurrency runtime
    goo_scheduler_shutdown();
    
    // Cleanup arena allocation system
    goo_arena_cleanup_system();
    
    goo_memory_state.initialized = 0;
}

// Ownership-based allocation functions

// Stack allocation (compile-time determined)
void* goo_alloc_stack(size_t size) {
    (void)size;
    // Stack allocation is handled by the compiler (alloca in LLVM)
    // This function is mainly for tracking statistics
    goo_memory_state.total_stack_allocations++;
    return NULL; // Actual allocation done by compiler
}

// Heap allocation for escaping values
void* goo_alloc_heap(size_t size) {
    if (size == 0) {
        return NULL;
    }
    
    void* ptr = malloc(size);
    if (!ptr) {
        goo_panic("Out of memory");
    }
    
    goo_memory_state.total_heap_allocations++;
    return ptr;
}

// Arena allocation for temporary values
void* goo_alloc_arena(size_t size) {
    if (size == 0) {
        return NULL;
    }
    
    // Use the current arena scope if available
    Arena* current_arena = goo_arena_current_scope();
    if (current_arena) {
        void* ptr = goo_arena_alloc(current_arena, size);
        if (ptr) {
            goo_memory_state.total_arena_allocations++;
            return ptr;
        }
    }
    
    // Fallback to heap allocation if no arena scope
    void* ptr = malloc(size);
    if (!ptr) {
        goo_panic("Out of memory");
    }
    
    goo_memory_state.total_heap_allocations++;
    return ptr;
}

// Generic allocation (delegates to appropriate allocator)
void* goo_alloc(size_t size) {
    // Default to heap allocation
    return goo_alloc_heap(size);
}

void* goo_realloc(void* ptr, size_t size) {
    if (size == 0) {
        goo_free(ptr);
        return NULL;
    }
    
    // Use standard realloc for now - will be replaced with ownership-based allocator
    void* new_ptr = realloc(ptr, size);
    if (!new_ptr) {
        goo_panic("Out of memory");
    }
    
    return new_ptr;
}

void goo_free(void* ptr) {
    if (ptr) {
        // Use standard free for now - will be replaced with ownership-based allocator
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

int goo_check_bounds(size_t index, size_t length) {
    return (index < length) ? 1 : 0;
}

// Ownership tracking functions

// Move operation (transfer ownership)
void goo_move_value(void* dest, void* src, size_t size) {
    if (!dest || !src) return;
    
    // Copy the value
    memcpy(dest, src, size);
    
    // Mark source as moved (zero out)
    memset(src, 0, size);
    
    goo_memory_state.total_moves++;
}

// Borrow operation (create reference)
void* goo_borrow_value(void* value) {
    if (!value) return NULL;
    
    // In a real implementation, this would create a reference
    // For now, just return the pointer and track statistics
    goo_memory_state.total_borrows++;
    goo_memory_state.active_references++;
    
    return value;
}

// Drop reference
void goo_drop_reference(void* reference) {
    if (!reference) return;
    
    // In a real implementation, this would decrement reference count
    // For now, just track statistics
    if (goo_memory_state.active_references > 0) {
        goo_memory_state.active_references--;
    }
}

// Check if value was moved (for debugging)
int goo_is_moved(void* value, size_t size) {
    if (!value) return 1;
    
    // Check if all bytes are zero (moved)
    char* bytes = (char*)value;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != 0) {
            return 0;
        }
    }
    return 1;
}

// Reference counting functions

typedef struct goo_rc_object {
    void* data;
    size_t ref_count;
    void (*destructor)(void*);
} goo_rc_object_t;

// Create reference-counted object
goo_rc_object_t* goo_rc_new(void* data, void (*destructor)(void*)) {
    goo_rc_object_t* rc = malloc(sizeof(goo_rc_object_t));
    if (!rc) {
        goo_panic("Failed to create reference-counted object");
    }
    
    rc->data = data;
    rc->ref_count = 1;
    rc->destructor = destructor;
    
    return rc;
}

// Clone reference
goo_rc_object_t* goo_rc_clone(goo_rc_object_t* rc) {
    if (!rc) return NULL;
    
    rc->ref_count++;
    return rc;
}

// Drop reference
void goo_rc_drop(goo_rc_object_t* rc) {
    if (!rc) return;
    
    rc->ref_count--;
    if (rc->ref_count == 0) {
        if (rc->destructor && rc->data) {
            rc->destructor(rc->data);
        }
        free(rc);
    }
}

// Get data from reference-counted object
void* goo_rc_data(goo_rc_object_t* rc) {
    return rc ? rc->data : NULL;
}

// Basic printf implementation for Goo
void goo_printf(const char* format, ...) {
    if (!format) return;

    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
}

// String operations

// C-string concatenation - takes two null-terminated C strings, returns new concatenated string
// Used by codegen for simple string + string operations
char* goo_cstring_concat(const char* s1, const char* s2) {
    if (!s1 || !s2) {
        return NULL;
    }

    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);
    size_t total_len = len1 + len2;

    char* result = (char*)malloc(total_len + 1);
    if (!result) {
        goo_panic("Failed to allocate memory for string concatenation");
        return NULL;
    }

    memcpy(result, s1, len1);
    memcpy(result + len1, s2, len2);
    result[total_len] = '\0';

    return result;
}