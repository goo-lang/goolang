#ifndef GOO_ERROR_H
#define GOO_ERROR_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// Error severity levels
typedef enum {
    ERROR_SEVERITY_NOTE,    // Informational note
    ERROR_SEVERITY_WARNING, // Warning - compilation continues
    ERROR_SEVERITY_ERROR,   // Error - compilation may continue for recovery
    ERROR_SEVERITY_FATAL    // Fatal error - compilation stops
} ErrorSeverity;

// Error categories for better organization
typedef enum {
    ERROR_CATEGORY_LEXER,
    ERROR_CATEGORY_PARSER,
    ERROR_CATEGORY_TYPE,
    ERROR_CATEGORY_CODEGEN,
    ERROR_CATEGORY_RUNTIME,
    ERROR_CATEGORY_INTERNAL
} ErrorCategory;

// Source location information
typedef struct {
    const char* filename;
    size_t line;
    size_t column;
    size_t offset;      // Byte offset in file
    size_t length;      // Length of the error span
} SourceLocation;

// Error codes for different error types
typedef enum {
    // Lexer errors (1000-1999)
    ERROR_INVALID_CHARACTER = 1000,
    ERROR_UNTERMINATED_STRING,
    ERROR_INVALID_NUMBER,
    ERROR_INVALID_ESCAPE,
    
    // Parser errors (2000-2999)
    ERROR_UNEXPECTED_TOKEN = 2000,
    ERROR_MISSING_SEMICOLON,
    ERROR_MISSING_CLOSING_PAREN,
    ERROR_MISSING_CLOSING_BRACE,
    ERROR_INVALID_EXPRESSION,
    ERROR_INVALID_STATEMENT,
    
    // Type errors (3000-3999)
    ERROR_TYPE_MISMATCH = 3000,
    ERROR_UNDEFINED_VARIABLE,
    ERROR_UNDEFINED_TYPE,
    ERROR_INVALID_CAST,
    ERROR_INCOMPATIBLE_TYPES,
    ERROR_DUPLICATE_DEFINITION,
    
    // Code generation errors (4000-4999)
    ERROR_CODEGEN_FAILED = 4000,
    ERROR_INVALID_TARGET,
    ERROR_LLVM_ERROR,
    
    // Runtime errors (5000-5999)
    ERROR_OUT_OF_MEMORY = 5000,
    ERROR_STACK_OVERFLOW,
    ERROR_OPERATION_CANCELLED,
    ERROR_OPERATION_FAILED,
    ERROR_BUFFER_OVERFLOW,
    ERROR_INVALID_STATE,
    ERROR_ITERATOR_EXHAUSTED,
    ERROR_BUFFER_CLOSED,
    ERROR_OPERATION_TIMEOUT,
    ERROR_BACKPRESSURE_DROP,
    ERROR_BACKPRESSURE_ERROR,
    ERROR_THREAD_CREATION,
    ERROR_RATE_LIMITED,
    ERROR_INITIALIZATION_FAILED,
    
    // Internal errors (9000-9999)
    ERROR_INTERNAL = 9000,
    ERROR_NOT_IMPLEMENTED
} ErrorCode;

// Error structure
typedef struct Error {
    ErrorCode code;
    ErrorSeverity severity;
    ErrorCategory category;
    const char* message;
    const char* hint;           // Optional hint for fixing the error
    SourceLocation location;
    struct Error* next;         // For error chaining
} Error;

// Error context for managing errors during compilation
typedef struct ErrorContext {
    Error* errors;              // Linked list of errors
    Error* last_error;          // Last error in the list
    size_t error_count;
    size_t warning_count;
    size_t max_errors;          // Maximum errors before stopping
    bool treat_warnings_as_errors;
    bool suppress_warnings;
    
    // Error recovery state
    bool in_panic_mode;         // Parser is in panic mode
    size_t panic_mode_depth;    // Nesting depth when panic started
    
    // Callbacks for custom error handling
    void (*error_handler)(const Error* error, void* user_data);
    void* error_handler_data;
} ErrorContext;

// Error context creation and destruction
ErrorContext* error_context_new(void);
void error_context_free(ErrorContext* ctx);

// Error reporting functions
void report_error(ErrorContext* ctx, ErrorCode code, const char* message, 
                  SourceLocation location);
void report_error_with_hint(ErrorContext* ctx, ErrorCode code, 
                            const char* message, const char* hint,
                            SourceLocation location);
void report_warning(ErrorContext* ctx, ErrorCode code, const char* message,
                    SourceLocation location);
void report_note(ErrorContext* ctx, const char* message, SourceLocation location);
void report_fatal(ErrorContext* ctx, ErrorCode code, const char* message,
                  SourceLocation location);

// Error context management
void clear_errors(ErrorContext* ctx);
bool has_errors(const ErrorContext* ctx);
bool has_fatal_errors(const ErrorContext* ctx);
size_t get_error_count(const ErrorContext* ctx);
size_t get_warning_count(const ErrorContext* ctx);

// Error recovery
void enter_panic_mode(ErrorContext* ctx, size_t depth);
void exit_panic_mode(ErrorContext* ctx);
bool is_in_panic_mode(const ErrorContext* ctx);

// Error formatting and display
void print_error(const Error* error);
void print_all_errors(const ErrorContext* ctx);
char* format_error(const Error* error); // Caller must free

// Error code utilities
const char* error_code_to_string(ErrorCode code);
const char* error_severity_to_string(ErrorSeverity severity);
const char* error_category_to_string(ErrorCategory category);

// Simple error creation function for runtime use
Error* error_create(ErrorCode code, const char* message);

// Source location utilities
SourceLocation make_source_location(const char* filename, size_t line, 
                                   size_t column, size_t offset, size_t length);
SourceLocation empty_source_location(void);
bool source_location_is_valid(const SourceLocation* loc);

// Error builder for complex error construction
typedef struct ErrorBuilder {
    ErrorContext* ctx;
    ErrorCode code;
    ErrorSeverity severity;
    char* message;
    char* hint;
    SourceLocation location;
} ErrorBuilder;

ErrorBuilder* error_builder_new(ErrorContext* ctx, ErrorCode code);
void error_builder_free(ErrorBuilder* builder);
ErrorBuilder* error_builder_with_message(ErrorBuilder* builder, const char* fmt, ...);
ErrorBuilder* error_builder_with_hint(ErrorBuilder* builder, const char* hint);
ErrorBuilder* error_builder_at_location(ErrorBuilder* builder, SourceLocation loc);
void error_builder_emit(ErrorBuilder* builder);

// Common error macros
#define REPORT_ERROR(ctx, code, msg, loc) \
    report_error((ctx), (code), (msg), (loc))

#define REPORT_WARNING(ctx, code, msg, loc) \
    report_warning((ctx), (code), (msg), (loc))

#define REPORT_FATAL(ctx, code, msg, loc) \
    report_fatal((ctx), (code), (msg), (loc))

#define CHECK_ERROR_LIMIT(ctx) \
    do { \
        if ((ctx)->error_count >= (ctx)->max_errors) { \
            REPORT_FATAL((ctx), ERROR_INTERNAL, \
                "Too many errors, stopping compilation", \
                empty_source_location()); \
            return NULL; \
        } \
    } while (0)


#endif // GOO_ERROR_H