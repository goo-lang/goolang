#include "errors/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ANSI color codes for terminal output
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_RESET   "\033[0m"

// Default maximum errors before stopping
#define DEFAULT_MAX_ERRORS 100

// Create a new error context
ErrorContext* error_context_new(void) {
    ErrorContext* ctx = (ErrorContext*)calloc(1, sizeof(ErrorContext));
    if (!ctx) return NULL;
    
    ctx->max_errors = DEFAULT_MAX_ERRORS;
    ctx->treat_warnings_as_errors = false;
    ctx->suppress_warnings = false;
    ctx->in_panic_mode = false;
    ctx->panic_mode_depth = 0;
    
    return ctx;
}

// Free error context and all associated errors
void error_context_free(ErrorContext* ctx) {
    if (!ctx) return;
    
    Error* current = ctx->errors;
    while (current) {
        Error* next = current->next;
        free((void*)current->message);
        free((void*)current->hint);
        free(current);
        current = next;
    }
    
    free(ctx);
}

// Create a new error and add it to the context
static Error* create_error(ErrorContext* ctx, ErrorCode code, 
                          ErrorSeverity severity, const char* message,
                          const char* hint, SourceLocation location) {
    Error* error = (Error*)calloc(1, sizeof(Error));
    if (!error) return NULL;
    
    error->code = code;
    error->severity = severity;
    error->category = (ErrorCategory)(code / 1000);
    error->message = message ? strdup(message) : NULL;
    error->hint = hint ? strdup(hint) : NULL;
    error->location = location;
    error->next = NULL;
    
    // Add to error list
    if (!ctx->errors) {
        ctx->errors = error;
    } else {
        ctx->last_error->next = error;
    }
    ctx->last_error = error;
    
    // Update counts
    if (severity == ERROR_SEVERITY_WARNING) {
        ctx->warning_count++;
    } else if (severity >= ERROR_SEVERITY_ERROR) {
        ctx->error_count++;
    }
    
    // Call custom error handler if set
    if (ctx->error_handler) {
        ctx->error_handler(error, ctx->error_handler_data);
    }
    
    return error;
}

// Report an error
void report_error(ErrorContext* ctx, ErrorCode code, const char* message,
                  SourceLocation location) {
    if (!ctx || ctx->error_count >= ctx->max_errors) return;
    
    Error* error = create_error(ctx, code, ERROR_SEVERITY_ERROR, 
                               message, NULL, location);
    if (error && !ctx->in_panic_mode) {
        print_error(error);
    }
}

// Report an error with hint
void report_error_with_hint(ErrorContext* ctx, ErrorCode code,
                            const char* message, const char* hint,
                            SourceLocation location) {
    if (!ctx || ctx->error_count >= ctx->max_errors) return;
    
    Error* error = create_error(ctx, code, ERROR_SEVERITY_ERROR,
                               message, hint, location);
    if (error && !ctx->in_panic_mode) {
        print_error(error);
    }
}

// Report a warning
void report_warning(ErrorContext* ctx, ErrorCode code, const char* message,
                    SourceLocation location) {
    if (!ctx || ctx->suppress_warnings) return;
    
    ErrorSeverity severity = ctx->treat_warnings_as_errors ? 
                            ERROR_SEVERITY_ERROR : ERROR_SEVERITY_WARNING;
    
    Error* error = create_error(ctx, code, severity, message, NULL, location);
    if (error && !ctx->in_panic_mode) {
        print_error(error);
    }
}

// Report a note
void report_note(ErrorContext* ctx, const char* message, SourceLocation location) {
    if (!ctx) return;
    
    Error* error = create_error(ctx, 0, ERROR_SEVERITY_NOTE, 
                               message, NULL, location);
    if (error && !ctx->in_panic_mode) {
        print_error(error);
    }
}

// Report a fatal error
void report_fatal(ErrorContext* ctx, ErrorCode code, const char* message,
                  SourceLocation location) {
    if (!ctx) return;
    
    Error* error = create_error(ctx, code, ERROR_SEVERITY_FATAL,
                               message, NULL, location);
    if (error) {
        print_error(error);
    }
}

// Clear all errors
void clear_errors(ErrorContext* ctx) {
    if (!ctx) return;
    
    Error* current = ctx->errors;
    while (current) {
        Error* next = current->next;
        free((void*)current->message);
        free((void*)current->hint);
        free(current);
        current = next;
    }
    
    ctx->errors = NULL;
    ctx->last_error = NULL;
    ctx->error_count = 0;
    ctx->warning_count = 0;
    ctx->in_panic_mode = false;
    ctx->panic_mode_depth = 0;
}

// Check if context has errors
bool has_errors(const ErrorContext* ctx) {
    return ctx && ctx->error_count > 0;
}

// Check if context has fatal errors
bool has_fatal_errors(const ErrorContext* ctx) {
    if (!ctx || !ctx->errors) return false;
    
    for (Error* e = ctx->errors; e; e = e->next) {
        if (e->severity == ERROR_SEVERITY_FATAL) {
            return true;
        }
    }
    return false;
}

// Get error count
size_t get_error_count(const ErrorContext* ctx) {
    return ctx ? ctx->error_count : 0;
}

// Get warning count
size_t get_warning_count(const ErrorContext* ctx) {
    return ctx ? ctx->warning_count : 0;
}

// Enter panic mode
void enter_panic_mode(ErrorContext* ctx, size_t depth) {
    if (ctx) {
        ctx->in_panic_mode = true;
        ctx->panic_mode_depth = depth;
    }
}

// Exit panic mode
void exit_panic_mode(ErrorContext* ctx) {
    if (ctx) {
        ctx->in_panic_mode = false;
        ctx->panic_mode_depth = 0;
    }
}

// Check if in panic mode
bool is_in_panic_mode(const ErrorContext* ctx) {
    return ctx && ctx->in_panic_mode;
}

// Get severity color
static const char* get_severity_color(ErrorSeverity severity) {
    switch (severity) {
        case ERROR_SEVERITY_NOTE:    return COLOR_CYAN;
        case ERROR_SEVERITY_WARNING: return COLOR_YELLOW;
        case ERROR_SEVERITY_ERROR:   return COLOR_RED;
        case ERROR_SEVERITY_FATAL:   return COLOR_RED COLOR_BOLD;
        default: return "";
    }
}

// Print an error to stderr
void print_error(const Error* error) {
    if (!error) return;
    
    const char* color = get_severity_color(error->severity);
    const char* severity_str = error_severity_to_string(error->severity);
    
    // Print location if available
    if (source_location_is_valid(&error->location)) {
        fprintf(stderr, "%s%s:%zu:%zu: %s%s%s: ",
                COLOR_BOLD,
                error->location.filename,
                error->location.line,
                error->location.column,
                color,
                severity_str,
                COLOR_RESET);
    } else {
        fprintf(stderr, "%s%s%s: ", color, severity_str, COLOR_RESET);
    }
    
    // Print message
    fprintf(stderr, "%s\n", error->message ? error->message : "");
    
    // Print hint if available
    if (error->hint) {
        fprintf(stderr, "%snote:%s %s\n", COLOR_CYAN, COLOR_RESET, error->hint);
    }
    
    // TODO: Print source line with error indicator
}

// Print all errors
void print_all_errors(const ErrorContext* ctx) {
    if (!ctx || !ctx->errors) return;
    
    for (Error* e = ctx->errors; e; e = e->next) {
        print_error(e);
    }
    
    // Print summary
    if (ctx->error_count > 0 || ctx->warning_count > 0) {
        fprintf(stderr, "\n%s%s%s: %zu error%s, %zu warning%s\n",
                COLOR_BOLD,
                ctx->error_count > 0 ? "error" : "warning",
                COLOR_RESET,
                ctx->error_count,
                ctx->error_count == 1 ? "" : "s",
                ctx->warning_count,
                ctx->warning_count == 1 ? "" : "s");
    }
}

// Format error to string
char* format_error(const Error* error) {
    if (!error) return NULL;
    
    char buffer[1024];
    const char* severity_str = error_severity_to_string(error->severity);
    
    if (source_location_is_valid(&error->location)) {
        snprintf(buffer, sizeof(buffer), "%s:%zu:%zu: %s: %s",
                error->location.filename,
                error->location.line,
                error->location.column,
                severity_str,
                error->message ? error->message : "");
    } else {
        snprintf(buffer, sizeof(buffer), "%s: %s",
                severity_str,
                error->message ? error->message : "");
    }
    
    return strdup(buffer);
}

// Convert error code to string
const char* error_code_to_string(ErrorCode code) {
    switch (code) {
        // Lexer errors
        case ERROR_INVALID_CHARACTER:     return "E1000";
        case ERROR_UNTERMINATED_STRING:   return "E1001";
        case ERROR_INVALID_NUMBER:        return "E1002";
        case ERROR_INVALID_ESCAPE:        return "E1003";
        
        // Parser errors
        case ERROR_UNEXPECTED_TOKEN:      return "E2000";
        case ERROR_MISSING_SEMICOLON:     return "E2001";
        case ERROR_MISSING_CLOSING_PAREN: return "E2002";
        case ERROR_MISSING_CLOSING_BRACE: return "E2003";
        case ERROR_INVALID_EXPRESSION:    return "E2004";
        case ERROR_INVALID_STATEMENT:     return "E2005";
        
        // Type errors
        case ERROR_TYPE_MISMATCH:         return "E3000";
        case ERROR_UNDEFINED_VARIABLE:    return "E3001";
        case ERROR_UNDEFINED_TYPE:        return "E3002";
        case ERROR_INVALID_CAST:          return "E3003";
        case ERROR_INCOMPATIBLE_TYPES:    return "E3004";
        case ERROR_DUPLICATE_DEFINITION:  return "E3005";
        
        // Code generation errors
        case ERROR_CODEGEN_FAILED:        return "E4000";
        case ERROR_INVALID_TARGET:        return "E4001";
        case ERROR_LLVM_ERROR:            return "E4002";
        
        // Runtime errors
        case ERROR_OUT_OF_MEMORY:         return "E5000";
        case ERROR_STACK_OVERFLOW:        return "E5001";
        
        // Internal errors
        case ERROR_INTERNAL:              return "E9000";
        case ERROR_NOT_IMPLEMENTED:       return "E9001";
        
        default: return "E0000";
    }
}

// Convert error severity to string
const char* error_severity_to_string(ErrorSeverity severity) {
    switch (severity) {
        case ERROR_SEVERITY_NOTE:    return "note";
        case ERROR_SEVERITY_WARNING: return "warning";
        case ERROR_SEVERITY_ERROR:   return "error";
        case ERROR_SEVERITY_FATAL:   return "fatal error";
        default: return "unknown";
    }
}

// Convert error category to string
const char* error_category_to_string(ErrorCategory category) {
    switch (category) {
        case ERROR_CATEGORY_LEXER:   return "lexer";
        case ERROR_CATEGORY_PARSER:  return "parser";
        case ERROR_CATEGORY_TYPE:    return "type";
        case ERROR_CATEGORY_CODEGEN: return "codegen";
        case ERROR_CATEGORY_RUNTIME: return "runtime";
        case ERROR_CATEGORY_INTERNAL: return "internal";
        default: return "unknown";
    }
}

// Create source location
SourceLocation make_source_location(const char* filename, size_t line,
                                   size_t column, size_t offset, size_t length) {
    SourceLocation loc = {
        .filename = filename,
        .line = line,
        .column = column,
        .offset = offset,
        .length = length
    };
    return loc;
}

// Create empty source location
SourceLocation empty_source_location(void) {
    SourceLocation loc = {0};
    return loc;
}

// Check if source location is valid
bool source_location_is_valid(const SourceLocation* loc) {
    return loc && loc->filename && loc->line > 0;
}

// Error builder implementation
ErrorBuilder* error_builder_new(ErrorContext* ctx, ErrorCode code) {
    if (!ctx) return NULL;
    
    ErrorBuilder* builder = (ErrorBuilder*)calloc(1, sizeof(ErrorBuilder));
    if (!builder) return NULL;
    
    builder->ctx = ctx;
    builder->code = code;
    builder->severity = ERROR_SEVERITY_ERROR;
    builder->location = empty_source_location();
    
    return builder;
}

void error_builder_free(ErrorBuilder* builder) {
    if (!builder) return;
    free(builder->message);
    free(builder->hint);
    free(builder);
}

ErrorBuilder* error_builder_with_message(ErrorBuilder* builder, const char* fmt, ...) {
    if (!builder || !fmt) return builder;
    
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    free(builder->message);
    builder->message = strdup(buffer);
    
    return builder;
}

ErrorBuilder* error_builder_with_hint(ErrorBuilder* builder, const char* hint) {
    if (!builder || !hint) return builder;
    
    free(builder->hint);
    builder->hint = strdup(hint);
    
    return builder;
}

ErrorBuilder* error_builder_at_location(ErrorBuilder* builder, SourceLocation loc) {
    if (!builder) return builder;
    
    builder->location = loc;
    return builder;
}

void error_builder_emit(ErrorBuilder* builder) {
    if (!builder || !builder->ctx) return;
    
    if (builder->hint) {
        report_error_with_hint(builder->ctx, builder->code,
                              builder->message, builder->hint,
                              builder->location);
    } else {
        report_error(builder->ctx, builder->code,
                    builder->message, builder->location);
    }
    
    error_builder_free(builder);
}