#include "error_context.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

// =============================================================================
// Global Error Context Manager
// =============================================================================

static ErrorContextManager g_error_context_manager = {
    .enabled = true,
    .auto_context_generation = true,
    .stack_trace_capture = true,
    .timing_enabled = true,
    .max_stack_depth = 100,
    .registered_context_count = 0,
    .transformation_rule_count = 0,
    .errors_enhanced = 0,
    .contexts_captured = 0,
    .auto_fixes_suggested = 0
};

// Thread-local storage for error context
static __thread ErrorStackFrame* g_current_frame = NULL;
static __thread int g_current_depth = 0;

// Mutex for global state protection
static pthread_mutex_t g_context_mutex = PTHREAD_MUTEX_INITIALIZER;

// =============================================================================
// Utility Functions
// =============================================================================

static uint64_t get_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static char* generate_uuid(void) {
    // Simple UUID-like string generation
    char* uuid_str = malloc(37);
    if (!uuid_str) return NULL;
    
    static uint64_t counter = 0;
    uint64_t timestamp = get_timestamp_ns();
    counter++;
    
    snprintf(uuid_str, 37, "%08lx-%04lx-%04lx-%04lx-%012lx",
        (timestamp >> 32) & 0xFFFFFFFF,
        (timestamp >> 16) & 0xFFFF,
        (timestamp & 0xFFFF),
        (counter >> 32) & 0xFFFF,
        counter & 0xFFFFFFFFFFFF);
    
    return uuid_str;
}

static char* duplicate_string(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

static char* format_string(const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    int size = vsnprintf(NULL, 0, format, args);
    va_end(args);
    
    if (size < 0) return NULL;
    
    char* result = malloc(size + 1);
    if (!result) return NULL;
    
    va_start(args, format);
    vsnprintf(result, size + 1, format, args);
    va_end(args);
    
    return result;
}

// =============================================================================
// Error Context System Lifecycle
// =============================================================================

void error_context_system_init(void) {
    pthread_mutex_lock(&g_context_mutex);
    
    if (g_error_context_manager.enabled) {
        pthread_mutex_unlock(&g_context_mutex);
        return; // Already initialized
    }
    
    // Initialize arrays
    g_error_context_manager.registered_context_names = calloc(16, sizeof(char*));
    g_error_context_manager.registered_context_types = calloc(16, sizeof(ErrorContextType));
    g_error_context_manager.transformation_rules = calloc(32, sizeof(*g_error_context_manager.transformation_rules));
    
    if (!g_error_context_manager.registered_context_names ||
        !g_error_context_manager.registered_context_types ||
        !g_error_context_manager.transformation_rules) {
        pthread_mutex_unlock(&g_context_mutex);
        goo_panic("Failed to initialize error context system");
    }
    
    g_error_context_manager.enabled = true;
    
    pthread_mutex_unlock(&g_context_mutex);
    
    printf("🔍 Error context system initialized\n");
}

void error_context_system_shutdown(void) {
    pthread_mutex_lock(&g_context_mutex);
    
    if (!g_error_context_manager.enabled) {
        pthread_mutex_unlock(&g_context_mutex);
        return;
    }
    
    // Clean up registered contexts
    for (int i = 0; i < g_error_context_manager.registered_context_count; i++) {
        free(g_error_context_manager.registered_context_names[i]);
    }
    
    free(g_error_context_manager.registered_context_names);
    free(g_error_context_manager.registered_context_types);
    free(g_error_context_manager.transformation_rules);
    
    // Clear thread-local data
    error_context_clear_stack();
    
    printf("🔍 Error context system shutdown\n");
    printf("📊 Context capture statistics:\n");
    printf("   - Errors enhanced: %lu\n", g_error_context_manager.errors_enhanced);
    printf("   - Contexts captured: %lu\n", g_error_context_manager.contexts_captured);
    printf("   - Auto-fixes suggested: %lu\n", g_error_context_manager.auto_fixes_suggested);
    
    g_error_context_manager.enabled = false;
    
    pthread_mutex_unlock(&g_context_mutex);
}

ErrorContextManager* get_error_context_manager(void) {
    return &g_error_context_manager;
}

void error_context_manager_configure(bool auto_context, bool stack_trace, bool timing) {
    pthread_mutex_lock(&g_context_mutex);
    
    g_error_context_manager.auto_context_generation = auto_context;
    g_error_context_manager.stack_trace_capture = stack_trace;
    g_error_context_manager.timing_enabled = timing;
    
    pthread_mutex_unlock(&g_context_mutex);
}

// =============================================================================
// Error Stack Frame Management
// =============================================================================

ErrorStackFrame* error_context_push_frame(const char* function_name,
                                          const char* file_name,
                                          int line_number,
                                          ErrorContextType context_type,
                                          const char* operation_description) {
    if (!g_error_context_manager.enabled) {
        return NULL;
    }
    
    if (g_current_depth >= g_error_context_manager.max_stack_depth) {
        // Stack overflow protection
        return NULL;
    }
    
    ErrorStackFrame* frame = calloc(1, sizeof(ErrorStackFrame));
    if (!frame) {
        return NULL;
    }
    
    frame->function_name = duplicate_string(function_name);
    frame->file_name = duplicate_string(file_name);
    frame->line_number = line_number;
    frame->context_type = context_type;
    frame->operation_description = duplicate_string(operation_description);
    frame->parent = g_current_frame;
    
    if (g_error_context_manager.timing_enabled) {
        frame->start_time_ns = get_timestamp_ns();
    }
    
    g_current_frame = frame;
    g_current_depth++;
    
    g_error_context_manager.contexts_captured++;
    
    return frame;
}

void error_context_pop_frame(void) {
    if (!g_current_frame) {
        return;
    }
    
    ErrorStackFrame* frame = g_current_frame;
    
    if (g_error_context_manager.timing_enabled) {
        frame->end_time_ns = get_timestamp_ns();
    }
    
    g_current_frame = frame->parent;
    g_current_depth--;
    
    // Note: We don't free the frame here because it might be needed for error reporting
    // It will be freed when the enhanced error is freed
}

void error_context_clear_stack(void) {
    while (g_current_frame) {
        ErrorStackFrame* frame = g_current_frame;
        g_current_frame = frame->parent;
        
        free((void*)frame->function_name);
        free((void*)frame->file_name);
        free((void*)frame->operation_description);
        if (frame->context_data && frame->context_data_destructor) {
            frame->context_data_destructor(frame->context_data);
        }
        free(frame);
    }
    g_current_depth = 0;
}

// Cleanup function for automatic context
void error_context_auto_cleanup(void* frame_ptr) {
    (void)frame_ptr; // Suppress unused parameter warning
    error_context_pop_frame();
}

// =============================================================================
// Enhanced Error Management
// =============================================================================

EnhancedError* enhanced_error_create(goo_error_t* base_error) {
    if (!base_error) return NULL;
    
    EnhancedError* enhanced = calloc(1, sizeof(EnhancedError));
    if (!enhanced) return NULL;
    
    enhanced->base_error = base_error;
    enhanced->creation_time_ns = get_timestamp_ns();
    enhanced->error_uuid = generate_uuid();
    enhanced->severity_level = 5; // Default medium severity
    enhanced->related_error_capacity = 4;
    enhanced->related_errors = calloc(enhanced->related_error_capacity, sizeof(EnhancedError*));
    
    return enhanced;
}

EnhancedError* enhanced_error_create_with_context(goo_error_t* base_error,
                                                 const char* context_description) {
    (void)context_description;
    EnhancedError* enhanced = enhanced_error_create(base_error);
    if (!enhanced) return NULL;
    
    // Capture current call stack
    if (g_error_context_manager.stack_trace_capture && g_current_frame) {
        enhanced->call_stack = g_current_frame;
        enhanced->primary_context = g_current_frame->context_type;
    }
    
    // Generate enhanced message and suggestions
    generate_error_context_message(enhanced);
    generate_suggested_fix(enhanced);
    
    return enhanced;
}

void enhanced_error_free(EnhancedError* error) {
    if (!error) return;
    
    // Free base error
    if (error->base_error) {
        goo_error_free(error->base_error);
    }
    
    // Free call stack (but not the frames themselves, they might be shared)
    // The frames are owned by the context system and freed elsewhere
    
    // Free generated strings
    free(error->auto_generated_message);
    free(error->suggested_fix);
    free(error->related_documentation);
    free((void*)error->error_uuid);
    
    // Free related errors
    if (error->related_errors) {
        for (int i = 0; i < error->related_error_count; i++) {
            enhanced_error_free(error->related_errors[i]);
        }
        free(error->related_errors);
    }
    
    free(error);
}

// =============================================================================
// Automatic Error Enhancement
// =============================================================================

EnhancedError* enhance_error_automatically(goo_error_t* base_error) {
    if (!base_error || !g_error_context_manager.auto_context_generation) {
        return enhanced_error_create(base_error);
    }
    
    EnhancedError* enhanced = enhanced_error_create_with_context(base_error, NULL);
    if (!enhanced) return NULL;
    
    g_error_context_manager.errors_enhanced++;
    
    return enhanced;
}

void generate_error_context_message(EnhancedError* error) {
    if (!error || !error->call_stack) return;
    
    char* message = NULL;
    ErrorStackFrame* frame = error->call_stack;
    
    // Generate context-aware message based on the primary context type
    switch (error->primary_context) {
        case ERROR_CONTEXT_FILE_IO:
            if (frame->context_data) {
                message = generate_file_io_error_message(
                    (FileIOContext*)frame->context_data, error->base_error);
            }
            break;
            
        case ERROR_CONTEXT_NETWORK:
            if (frame->context_data) {
                message = generate_network_error_message(
                    (NetworkContext*)frame->context_data, error->base_error);
            }
            break;
            
        case ERROR_CONTEXT_PARSING:
            if (frame->context_data) {
                message = generate_parsing_error_message(
                    (ParsingContext*)frame->context_data, error->base_error);
            }
            break;
            
        case ERROR_CONTEXT_VALIDATION:
            if (frame->context_data) {
                message = generate_validation_error_message(
                    (ValidationContext*)frame->context_data, error->base_error);
            }
            break;
            
        default:
            // Generic context message
            message = format_string(
                "Error in %s() at %s:%d during %s: %s",
                frame->function_name ? frame->function_name : "unknown",
                frame->file_name ? frame->file_name : "unknown",
                frame->line_number,
                frame->operation_description ? frame->operation_description : "operation",
                error->base_error->message
            );
            break;
    }
    
    error->auto_generated_message = message;
}

void generate_suggested_fix(EnhancedError* error) {
    if (!error || !error->call_stack) return;
    
    char* suggestion = NULL;
    ErrorStackFrame* frame = error->call_stack;
    
    // Generate context-aware fix suggestion
    switch (error->primary_context) {
        case ERROR_CONTEXT_FILE_IO:
            if (frame->context_data) {
                suggestion = suggest_file_io_fix(
                    (FileIOContext*)frame->context_data, error->base_error);
            }
            break;
            
        case ERROR_CONTEXT_NETWORK:
            if (frame->context_data) {
                suggestion = suggest_network_fix(
                    (NetworkContext*)frame->context_data, error->base_error);
            }
            break;
            
        case ERROR_CONTEXT_PARSING:
            if (frame->context_data) {
                suggestion = suggest_parsing_fix(
                    (ParsingContext*)frame->context_data, error->base_error);
            }
            break;
            
        case ERROR_CONTEXT_VALIDATION:
            if (frame->context_data) {
                suggestion = suggest_validation_fix(
                    (ValidationContext*)frame->context_data, error->base_error);
            }
            break;
            
        default:
            // Generic suggestion based on error code
            if (error->base_error->code == 3) { // File not found
                suggestion = duplicate_string("Check if the file exists and you have the correct permissions");
            } else if (error->base_error->code == 1) { // Out of memory
                suggestion = duplicate_string("Reduce memory usage or increase available memory");
            }
            break;
    }
    
    if (suggestion) {
        error->suggested_fix = suggestion;
        g_error_context_manager.auto_fixes_suggested++;
    }
}

// =============================================================================
// Error Propagation with Context
// =============================================================================

goo_error_union_t* propagate_error_with_context(goo_error_union_t* source_union) {
    if (!source_union || !goo_error_union_is_error(source_union)) {
        return source_union;
    }
    
    goo_error_t* base_error = goo_error_union_get_error(source_union);
    EnhancedError* enhanced = enhance_error_automatically(base_error);
    
    if (!enhanced) {
        // Fallback to regular propagation
        return goo_error_propagate(source_union);
    }
    
    // Convert enhanced error back to goo_error_t for compatibility
    goo_error_t* enhanced_goo_error = enhanced_error_to_goo_error(enhanced);
    enhanced_error_free(enhanced);
    
    return goo_error_union_new_error(enhanced_goo_error);
}

EnhancedError* propagate_enhanced_error(EnhancedError* source_error) {
    if (!source_error) return NULL;
    
    // Create a new enhanced error that chains to the source
    EnhancedError* propagated = enhanced_error_create_with_context(
        source_error->base_error, "error propagation");
    
    if (!propagated) return source_error;
    
    // Add source error as a related error
    if (propagated->related_error_count < propagated->related_error_capacity) {
        propagated->related_errors[propagated->related_error_count++] = source_error;
    }
    
    return propagated;
}

// =============================================================================
// Context-Aware Error Messages
// =============================================================================

char* generate_file_io_error_message(const FileIOContext* ctx, const goo_error_t* base_error) {
    if (!ctx || !base_error) return NULL;
    
    return format_string(
        "File I/O error during %s operation on '%s': %s (processed %zu bytes)",
        ctx->operation ? ctx->operation : "unknown",
        ctx->file_path ? ctx->file_path : "unknown file",
        base_error->message,
        ctx->bytes_processed
    );
}

char* generate_network_error_message(const NetworkContext* ctx, const goo_error_t* base_error) {
    if (!ctx || !base_error) return NULL;
    
    return format_string(
        "Network error during %s operation to %s:%d using %s: %s (transferred %zu bytes)",
        ctx->operation ? ctx->operation : "unknown",
        ctx->host ? ctx->host : "unknown host",
        ctx->port,
        ctx->protocol ? ctx->protocol : "unknown protocol",
        base_error->message,
        ctx->bytes_transferred
    );
}

char* generate_parsing_error_message(const ParsingContext* ctx, const goo_error_t* base_error) {
    if (!ctx || !base_error) return NULL;
    
    return format_string(
        "Parsing error in %s at line %d, column %d (token: '%s', rule: '%s'): %s",
        ctx->input_type ? ctx->input_type : "unknown format",
        ctx->line_number,
        ctx->column_number,
        ctx->current_token ? ctx->current_token : "unknown",
        ctx->parsing_rule ? ctx->parsing_rule : "unknown",
        base_error->message
    );
}

char* generate_validation_error_message(const ValidationContext* ctx, const goo_error_t* base_error) {
    if (!ctx || !base_error) return NULL;
    
    return format_string(
        "Validation error in %s for field '%s': expected '%s', got '%s' (%s) - %s",
        ctx->validation_type ? ctx->validation_type : "unknown validation",
        ctx->field_name ? ctx->field_name : "unknown field",
        ctx->expected_value ? ctx->expected_value : "unknown",
        ctx->actual_value ? ctx->actual_value : "unknown",
        ctx->constraint_description ? ctx->constraint_description : "no constraint info",
        base_error->message
    );
}

// =============================================================================
// Suggested Fix Generation
// =============================================================================

char* suggest_file_io_fix(const FileIOContext* ctx, const goo_error_t* base_error) {
    if (!ctx || !base_error) return NULL;
    
    if (base_error->code == 3) { // File not found
        return format_string(
            "Check if file '%s' exists and is accessible. Verify the file path and permissions.",
            ctx->file_path ? ctx->file_path : "unknown"
        );
    } else if (base_error->code == 4) { // Permission denied
        return format_string(
            "Permission denied accessing '%s'. Check file permissions or run with appropriate privileges.",
            ctx->file_path ? ctx->file_path : "unknown"
        );
    }
    
    return duplicate_string("Check file path, permissions, and disk space");
}

char* suggest_network_fix(const NetworkContext* ctx, const goo_error_t* base_error) {
    if (!ctx || !base_error) return NULL;
    
    return format_string(
        "Check network connectivity to %s:%d. Verify the %s protocol is supported and the service is running.",
        ctx->host ? ctx->host : "unknown host",
        ctx->port,
        ctx->protocol ? ctx->protocol : "unknown protocol"
    );
}

char* suggest_parsing_fix(const ParsingContext* ctx, const goo_error_t* base_error) {
    if (!ctx || !base_error) return NULL;
    
    return format_string(
        "Check %s syntax at line %d, column %d. Look for missing quotes, brackets, or invalid characters near '%s'.",
        ctx->input_type ? ctx->input_type : "input",
        ctx->line_number,
        ctx->column_number,
        ctx->current_token ? ctx->current_token : "unknown token"
    );
}

char* suggest_validation_fix(const ValidationContext* ctx, const goo_error_t* base_error) {
    if (!ctx || !base_error) return NULL;
    
    return format_string(
        "Fix field '%s': %s. %s",
        ctx->field_name ? ctx->field_name : "unknown field",
        ctx->constraint_description ? ctx->constraint_description : "check the field value",
        ctx->is_required_field ? "This field is required." : "This field is optional but must be valid when provided."
    );
}

// =============================================================================
// Integration Functions
// =============================================================================

goo_error_t* enhanced_error_to_goo_error(const EnhancedError* enhanced) {
    if (!enhanced || !enhanced->base_error) return NULL;
    
    // Create a new goo_error_t with enhanced message
    const char* message = enhanced->auto_generated_message ? 
                         enhanced->auto_generated_message : 
                         enhanced->base_error->message;
    
    return goo_new_error_with_code(message, enhanced->base_error->code);
}

EnhancedError* goo_error_to_enhanced_error(const goo_error_t* goo_err) {
    if (!goo_err) return NULL;
    
    // Create a copy of the goo_error_t
    goo_error_t* error_copy = goo_new_error_with_code(goo_err->message, goo_err->code);
    
    return enhance_error_automatically(error_copy);
}

// =============================================================================
// Debug and Diagnostics
// =============================================================================

void print_enhanced_error(const EnhancedError* error) {
    if (!error) return;
    
    printf("🚨 Enhanced Error Report\n");
    printf("═══════════════════════\n");
    printf("UUID: %s\n", error->error_uuid ? error->error_uuid : "unknown");
    printf("Severity: %d/10\n", error->severity_level);
    printf("Original Error: %s (code: %d)\n", 
           error->base_error ? error->base_error->message : "unknown",
           error->base_error ? error->base_error->code : 0);
    
    if (error->auto_generated_message) {
        printf("Enhanced Message: %s\n", error->auto_generated_message);
    }
    
    if (error->suggested_fix) {
        printf("💡 Suggested Fix: %s\n", error->suggested_fix);
    }
    
    if (error->call_stack) {
        printf("\n📍 Call Stack:\n");
        print_error_stack_trace(error->call_stack);
    }
    
    if (error->related_error_count > 0) {
        printf("\n🔗 Related Errors (%d):\n", error->related_error_count);
        for (int i = 0; i < error->related_error_count; i++) {
            if (error->related_errors[i]) {
                printf("  %d. %s\n", i + 1, 
                       error->related_errors[i]->auto_generated_message ?
                       error->related_errors[i]->auto_generated_message :
                       "Unknown error");
            }
        }
    }
    
    printf("═══════════════════════\n");
}

void print_error_stack_trace(const ErrorStackFrame* frame) {
    int depth = 0;
    while (frame) {
        printf("  %d. %s() at %s:%d\n", 
               depth,
               frame->function_name ? frame->function_name : "unknown",
               frame->file_name ? frame->file_name : "unknown",
               frame->line_number);
        
        if (frame->operation_description) {
            printf("     Operation: %s\n", frame->operation_description);
        }
        
        if (frame->start_time_ns > 0 && frame->end_time_ns > 0) {
            uint64_t duration_ns = frame->end_time_ns - frame->start_time_ns;
            printf("     Duration: %lu.%03lu ms\n", 
                   duration_ns / 1000000, 
                   (duration_ns % 1000000) / 1000);
        }
        
        frame = frame->parent;
        depth++;
        
        if (depth > 20) { // Prevent infinite loops
            printf("  ... (truncated)\n");
            break;
        }
    }
}

void print_error_context_statistics(void) {
    pthread_mutex_lock(&g_context_mutex);
    
    printf("🔍 Error Context System Statistics\n");
    printf("═══════════════════════════════════\n");
    printf("System Status: %s\n", g_error_context_manager.enabled ? "Enabled" : "Disabled");
    printf("Auto Context Generation: %s\n", g_error_context_manager.auto_context_generation ? "On" : "Off");
    printf("Stack Trace Capture: %s\n", g_error_context_manager.stack_trace_capture ? "On" : "Off");
    printf("Timing: %s\n", g_error_context_manager.timing_enabled ? "On" : "Off");
    printf("\nStatistics:\n");
    printf("  Errors Enhanced: %lu\n", g_error_context_manager.errors_enhanced);
    printf("  Contexts Captured: %lu\n", g_error_context_manager.contexts_captured);
    printf("  Auto-fixes Suggested: %lu\n", g_error_context_manager.auto_fixes_suggested);
    printf("  Current Stack Depth: %d/%d\n", g_current_depth, g_error_context_manager.max_stack_depth);
    printf("  Registered Contexts: %d\n", g_error_context_manager.registered_context_count);
    
    pthread_mutex_unlock(&g_context_mutex);
}