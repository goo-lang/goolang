#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

// =============================================================================
// Error Union and Nullable Type Runtime Support
// =============================================================================

// Error union tag values
typedef enum {
    GOO_ERROR_UNION_VALUE = 0,   // Contains a value (no error)
    GOO_ERROR_UNION_ERROR = 1    // Contains an error
} goo_error_union_tag_t;

// Nullable type tag values
typedef enum {
    GOO_NULLABLE_NULL = 0,       // Contains null
    GOO_NULLABLE_VALUE = 1       // Contains a value
} goo_nullable_tag_t;

// Error union runtime structure
typedef struct goo_error_union {
    goo_error_union_tag_t tag;   // Discriminant: value or error
    union {
        void* value;             // The actual value (if tag == VALUE)
        goo_error_t* error;      // The error (if tag == ERROR)
    } data;
    size_t value_size;           // Size of the value type
    void (*value_destructor)(void*); // Destructor for the value
} goo_error_union_t;

// Nullable type runtime structure
typedef struct goo_nullable {
    goo_nullable_tag_t tag;      // Discriminant: null or value
    void* value;                 // The actual value (if tag == VALUE)
    size_t value_size;           // Size of the value type
    void (*value_destructor)(void*); // Destructor for the value
} goo_nullable_t;

// Global error handling state
static struct {
    int initialized;
    
    // Error statistics
    size_t error_unions_created;
    size_t error_unions_destroyed;
    size_t errors_propagated;
    size_t errors_handled;
    
    // Nullable statistics
    size_t nullables_created;
    size_t nullables_destroyed;
    size_t null_checks_performed;
    size_t null_access_prevented;
    
    // Error registry for common errors
    goo_error_t** registered_errors;
    size_t registered_error_count;
    size_t registered_error_capacity;
    
} g_error_state = {0};

// =============================================================================
// Error Handling System Initialization
// =============================================================================

void goo_error_system_init(void) {
    if (g_error_state.initialized) {
        return;
    }
    
    // Initialize error registry
    g_error_state.registered_error_capacity = 16;
    g_error_state.registered_errors = malloc(sizeof(goo_error_t*) * g_error_state.registered_error_capacity);
    if (!g_error_state.registered_errors) {
        goo_panic("Failed to initialize error system");
    }
    
    g_error_state.registered_error_count = 0;
    
    // Reset statistics
    g_error_state.error_unions_created = 0;
    g_error_state.error_unions_destroyed = 0;
    g_error_state.errors_propagated = 0;
    g_error_state.errors_handled = 0;
    g_error_state.nullables_created = 0;
    g_error_state.nullables_destroyed = 0;
    g_error_state.null_checks_performed = 0;
    g_error_state.null_access_prevented = 0;
    
    g_error_state.initialized = 1;
    
    printf("🛡️  Error handling system initialized\n");
}

void goo_error_system_shutdown(void) {
    if (!g_error_state.initialized) {
        return;
    }
    
    // Clean up registered errors
    for (size_t i = 0; i < g_error_state.registered_error_count; i++) {
        if (g_error_state.registered_errors[i]) {
            goo_error_free(g_error_state.registered_errors[i]);
        }
    }
    free(g_error_state.registered_errors);
    
    // Print statistics
    printf("🛡️  Error handling system shutdown\n");
    printf("📊 Error handling statistics:\n");
    printf("   - Error unions created: %zu\n", g_error_state.error_unions_created);
    printf("   - Error unions destroyed: %zu\n", g_error_state.error_unions_destroyed);
    printf("   - Errors propagated: %zu\n", g_error_state.errors_propagated);
    printf("   - Errors handled: %zu\n", g_error_state.errors_handled);
    printf("   - Nullables created: %zu\n", g_error_state.nullables_created);
    printf("   - Nullables destroyed: %zu\n", g_error_state.nullables_destroyed);
    printf("   - Null checks performed: %zu\n", g_error_state.null_checks_performed);
    printf("   - Null access prevented: %zu\n", g_error_state.null_access_prevented);
    
    g_error_state.initialized = 0;
}

// =============================================================================
// Error Union Implementation
// =============================================================================

goo_error_union_t* goo_error_union_new_value(void* value, size_t value_size, void (*destructor)(void*)) {
    if (!g_error_state.initialized) {
        goo_error_system_init();
    }
    
    goo_error_union_t* error_union = goo_alloc(sizeof(goo_error_union_t));
    if (!error_union) {
        goo_panic("Out of memory creating error union");
    }
    
    error_union->tag = GOO_ERROR_UNION_VALUE;
    error_union->value_size = value_size;
    error_union->value_destructor = destructor;
    
    // Copy the value
    if (value && value_size > 0) {
        error_union->data.value = goo_alloc(value_size);
        if (!error_union->data.value) {
            goo_free(error_union);
            goo_panic("Out of memory copying value to error union");
        }
        memcpy(error_union->data.value, value, value_size);
    } else {
        error_union->data.value = NULL;
    }
    
    g_error_state.error_unions_created++;
    return error_union;
}

goo_error_union_t* goo_error_union_new_error(goo_error_t* error) {
    if (!g_error_state.initialized) {
        goo_error_system_init();
    }
    
    goo_error_union_t* error_union = goo_alloc(sizeof(goo_error_union_t));
    if (!error_union) {
        goo_panic("Out of memory creating error union");
    }
    
    error_union->tag = GOO_ERROR_UNION_ERROR;
    error_union->data.error = error;
    error_union->value_size = 0;
    error_union->value_destructor = NULL;
    
    g_error_state.error_unions_created++;
    return error_union;
}

void goo_error_union_free(goo_error_union_t* error_union) {
    if (!error_union) return;
    
    if (error_union->tag == GOO_ERROR_UNION_VALUE) {
        if (error_union->data.value) {
            if (error_union->value_destructor) {
                error_union->value_destructor(error_union->data.value);
            }
            goo_free(error_union->data.value);
        }
    } else if (error_union->tag == GOO_ERROR_UNION_ERROR) {
        if (error_union->data.error) {
            goo_error_free(error_union->data.error);
        }
    }
    
    goo_free(error_union);
    g_error_state.error_unions_destroyed++;
}

int goo_error_union_is_value(const goo_error_union_t* error_union) {
    if (!error_union) return 0;
    return error_union->tag == GOO_ERROR_UNION_VALUE;
}

int goo_error_union_is_error(const goo_error_union_t* error_union) {
    if (!error_union) return 1; // NULL is considered an error
    return error_union->tag == GOO_ERROR_UNION_ERROR;
}

void* goo_error_union_get_value(const goo_error_union_t* error_union) {
    if (!error_union || error_union->tag != GOO_ERROR_UNION_VALUE) {
        goo_panic("Attempted to get value from error union that contains an error");
    }
    return error_union->data.value;
}

goo_error_t* goo_error_union_get_error(const goo_error_union_t* error_union) {
    if (!error_union || error_union->tag != GOO_ERROR_UNION_ERROR) {
        goo_panic("Attempted to get error from error union that contains a value");
    }
    return error_union->data.error;
}

// Safe accessors that don't panic
int goo_error_union_try_get_value(const goo_error_union_t* error_union, void** value_out) {
    if (!error_union || !value_out) return 0;
    
    if (error_union->tag == GOO_ERROR_UNION_VALUE) {
        *value_out = error_union->data.value;
        return 1;
    }
    
    *value_out = NULL;
    return 0;
}

int goo_error_union_try_get_error(const goo_error_union_t* error_union, goo_error_t** error_out) {
    if (!error_union || !error_out) return 0;
    
    if (error_union->tag == GOO_ERROR_UNION_ERROR) {
        *error_out = error_union->data.error;
        return 1;
    }
    
    *error_out = NULL;
    return 0;
}

// =============================================================================
// Nullable Type Implementation
// =============================================================================

goo_nullable_t* goo_nullable_new_null(void) {
    if (!g_error_state.initialized) {
        goo_error_system_init();
    }
    
    goo_nullable_t* nullable = goo_alloc(sizeof(goo_nullable_t));
    if (!nullable) {
        goo_panic("Out of memory creating nullable");
    }
    
    nullable->tag = GOO_NULLABLE_NULL;
    nullable->value = NULL;
    nullable->value_size = 0;
    nullable->value_destructor = NULL;
    
    g_error_state.nullables_created++;
    return nullable;
}

goo_nullable_t* goo_nullable_new_value(void* value, size_t value_size, void (*destructor)(void*)) {
    if (!g_error_state.initialized) {
        goo_error_system_init();
    }
    
    goo_nullable_t* nullable = goo_alloc(sizeof(goo_nullable_t));
    if (!nullable) {
        goo_panic("Out of memory creating nullable");
    }
    
    nullable->tag = GOO_NULLABLE_VALUE;
    nullable->value_size = value_size;
    nullable->value_destructor = destructor;
    
    // Copy the value
    if (value && value_size > 0) {
        nullable->value = goo_alloc(value_size);
        if (!nullable->value) {
            goo_free(nullable);
            goo_panic("Out of memory copying value to nullable");
        }
        memcpy(nullable->value, value, value_size);
    } else {
        nullable->value = NULL;
    }
    
    g_error_state.nullables_created++;
    return nullable;
}

void goo_nullable_free(goo_nullable_t* nullable) {
    if (!nullable) return;
    
    if (nullable->tag == GOO_NULLABLE_VALUE && nullable->value) {
        if (nullable->value_destructor) {
            nullable->value_destructor(nullable->value);
        }
        goo_free(nullable->value);
    }
    
    goo_free(nullable);
    g_error_state.nullables_destroyed++;
}

int goo_nullable_is_null(const goo_nullable_t* nullable) {
    if (!nullable) return 1; // NULL is considered null
    g_error_state.null_checks_performed++;
    return nullable->tag == GOO_NULLABLE_NULL;
}

int goo_nullable_is_value(const goo_nullable_t* nullable) {
    if (!nullable) return 0;
    g_error_state.null_checks_performed++;
    return nullable->tag == GOO_NULLABLE_VALUE;
}

void* goo_nullable_get_value(const goo_nullable_t* nullable) {
    if (!nullable || nullable->tag != GOO_NULLABLE_VALUE) {
        g_error_state.null_access_prevented++;
        goo_panic("Attempted to get value from null nullable");
    }
    return nullable->value;
}

// Safe accessor that doesn't panic
int goo_nullable_try_get_value(const goo_nullable_t* nullable, void** value_out) {
    if (!nullable || !value_out) return 0;
    
    g_error_state.null_checks_performed++;
    
    if (nullable->tag == GOO_NULLABLE_VALUE) {
        *value_out = nullable->value;
        return 1;
    }
    
    *value_out = NULL;
    return 0;
}

// =============================================================================
// Error Propagation and Unwrapping
// =============================================================================

goo_error_union_t* goo_error_propagate(goo_error_union_t* source) {
    if (!source) {
        return goo_error_union_new_error(goo_new_error("Propagated null error union"));
    }
    
    if (source->tag == GOO_ERROR_UNION_ERROR) {
        g_error_state.errors_propagated++;
        // Create a new error union with the same error (don't modify the source)
        return goo_error_union_new_error(source->data.error);
    }
    
    // Return the source unchanged if it contains a value
    return source;
}

void* goo_error_unwrap(goo_error_union_t* error_union) {
    if (!error_union) {
        goo_panic("Attempted to unwrap null error union");
    }
    
    if (error_union->tag == GOO_ERROR_UNION_ERROR) {
        goo_panic("Attempted to unwrap error union that contains an error");
    }
    
    g_error_state.errors_handled++;
    return error_union->data.value;
}

void* goo_error_unwrap_or(goo_error_union_t* error_union, void* default_value) {
    if (!error_union || error_union->tag == GOO_ERROR_UNION_ERROR) {
        g_error_state.errors_handled++;
        return default_value;
    }
    
    return error_union->data.value;
}

void* goo_nullable_unwrap(goo_nullable_t* nullable) {
    if (!nullable || nullable->tag == GOO_NULLABLE_NULL) {
        g_error_state.null_access_prevented++;
        goo_panic("Attempted to unwrap null nullable");
    }
    
    return nullable->value;
}

void* goo_nullable_unwrap_or(goo_nullable_t* nullable, void* default_value) {
    if (!nullable || nullable->tag == GOO_NULLABLE_NULL) {
        return default_value;
    }
    
    return nullable->value;
}

// =============================================================================
// Try/Catch Style Error Handling
// =============================================================================

typedef struct goo_try_context {
    jmp_buf jump_buffer;
    goo_error_t* caught_error;
    struct goo_try_context* previous;
} goo_try_context_t;

// Thread-local try context stack
static __thread goo_try_context_t* g_try_context_stack = NULL;

int goo_try_begin(goo_try_context_t* context) {
    if (!context) return 0;
    
    context->caught_error = NULL;
    context->previous = g_try_context_stack;
    g_try_context_stack = context;
    
    return setjmp(context->jump_buffer);
}

void goo_try_end(void) {
    if (g_try_context_stack) {
        g_try_context_stack = g_try_context_stack->previous;
    }
}

void goo_throw_error(goo_error_t* error) {
    if (!error) {
        error = goo_new_error("Unknown error");
    }
    
    if (g_try_context_stack) {
        g_try_context_stack->caught_error = error;
        longjmp(g_try_context_stack->jump_buffer, 1);
    } else {
        // No try context - panic
        goo_panic(error->message);
    }
}

goo_error_t* goo_try_get_error(void) {
    if (!g_try_context_stack) return NULL;
    return g_try_context_stack->caught_error;
}

// =============================================================================
// Common Error Types and Registry
// =============================================================================

int goo_register_error_type(goo_error_t* error_template) {
    if (!g_error_state.initialized) {
        goo_error_system_init();
    }
    
    if (!error_template) return 0;
    
    // Resize array if needed
    if (g_error_state.registered_error_count >= g_error_state.registered_error_capacity) {
        size_t new_capacity = g_error_state.registered_error_capacity * 2;
        goo_error_t** new_errors = realloc(g_error_state.registered_errors,
                                          sizeof(goo_error_t*) * new_capacity);
        if (!new_errors) return 0;
        
        g_error_state.registered_errors = new_errors;
        g_error_state.registered_error_capacity = new_capacity;
    }
    
    // Create a copy of the error template
    goo_error_t* error_copy = goo_new_error_with_code(error_template->message, error_template->code);
    if (error_template->cause) {
        error_copy->cause = goo_new_error(error_template->cause->message);
    }
    
    g_error_state.registered_errors[g_error_state.registered_error_count++] = error_copy;
    return 1;
}

goo_error_t* goo_create_registered_error(int error_code) {
    for (size_t i = 0; i < g_error_state.registered_error_count; i++) {
        if (g_error_state.registered_errors[i]->code == error_code) {
            goo_error_t* template = g_error_state.registered_errors[i];
            return goo_new_error_with_code(template->message, template->code);
        }
    }
    
    return goo_new_error("Unknown registered error");
}

// =============================================================================
// Error Handling Macros Support
// =============================================================================

// These functions support compile-time generated error handling code

int goo_error_check_and_propagate(goo_error_union_t* error_union, goo_error_union_t** result_out) {
    if (!error_union || !result_out) return 0;
    
    if (error_union->tag == GOO_ERROR_UNION_ERROR) {
        *result_out = goo_error_propagate(error_union);
        return 1; // Error should be propagated
    }
    
    *result_out = error_union;
    return 0; // No error, continue execution
}

int goo_nullable_check_and_return(goo_nullable_t* nullable, void** value_out) {
    if (!nullable || !value_out) return 0;
    
    if (nullable->tag == GOO_NULLABLE_NULL) {
        *value_out = NULL;
        return 1; // Null encountered
    }
    
    *value_out = nullable->value;
    return 0; // Has value, continue execution
}

// =============================================================================
// Debug and Utility Functions
// =============================================================================

void goo_error_print_statistics(void) {
    printf("=== Error Handling Statistics ===\n");
    printf("Error Unions:\n");
    printf("  Created: %zu\n", g_error_state.error_unions_created);
    printf("  Destroyed: %zu\n", g_error_state.error_unions_destroyed);
    printf("  Active: %zu\n", g_error_state.error_unions_created - g_error_state.error_unions_destroyed);
    printf("  Errors propagated: %zu\n", g_error_state.errors_propagated);
    printf("  Errors handled: %zu\n", g_error_state.errors_handled);
    
    printf("\nNullable Types:\n");
    printf("  Created: %zu\n", g_error_state.nullables_created);
    printf("  Destroyed: %zu\n", g_error_state.nullables_destroyed);
    printf("  Active: %zu\n", g_error_state.nullables_created - g_error_state.nullables_destroyed);
    printf("  Null checks: %zu\n", g_error_state.null_checks_performed);
    printf("  Null access prevented: %zu\n", g_error_state.null_access_prevented);
    
    printf("\nRegistered Error Types: %zu\n", g_error_state.registered_error_count);
}

const char* goo_error_union_tag_to_string(const goo_error_union_t* error_union) {
    if (!error_union) return "null";
    return error_union->tag == GOO_ERROR_UNION_VALUE ? "value" : "error";
}

const char* goo_nullable_tag_to_string(const goo_nullable_t* nullable) {
    if (!nullable) return "null";
    return nullable->tag == GOO_NULLABLE_VALUE ? "value" : "null";
}

// =============================================================================
// Integration with Runtime System
// =============================================================================

void goo_error_handling_integrate_runtime(void) {
    // Called from goo_init() to integrate error handling with the main runtime
    goo_error_system_init();
    
    // Register common error types
    goo_error_t* out_of_memory = goo_new_error_with_code("Out of memory", 1);
    goo_error_t* invalid_argument = goo_new_error_with_code("Invalid argument", 2);
    goo_error_t* file_not_found = goo_new_error_with_code("File not found", 3);
    goo_error_t* permission_denied = goo_new_error_with_code("Permission denied", 4);
    goo_error_t* timeout = goo_new_error_with_code("Operation timed out", 5);
    
    goo_register_error_type(out_of_memory);
    goo_register_error_type(invalid_argument);
    goo_register_error_type(file_not_found);
    goo_register_error_type(permission_denied);
    goo_register_error_type(timeout);
    
    goo_error_free(out_of_memory);
    goo_error_free(invalid_argument);
    goo_error_free(file_not_found);
    goo_error_free(permission_denied);
    goo_error_free(timeout);
}