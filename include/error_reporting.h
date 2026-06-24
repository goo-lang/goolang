#ifndef ERROR_REPORTING_H
#define ERROR_REPORTING_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// Forward declarations
typedef struct ErrorContext ErrorContext;  // defined in errors/error.h
typedef struct ErrorReportingContext ErrorReportingContext;
typedef struct ErrorReport ErrorReport;
typedef struct ErrorExplanation ErrorExplanation;
typedef struct ErrorLocation ErrorLocation;
typedef struct ErrorSuggestion ErrorSuggestion;
typedef struct REPLContext REPLContext;

// =============================================================================
// Enhanced Error Reporting System for Goo Compiler
// =============================================================================

// Error severity levels
typedef enum {
    ERROR_SEVERITY_NOTE,
    ERROR_SEVERITY_WARNING,
    ERROR_SEVERITY_ERROR,
    ERROR_SEVERITY_FATAL
} ErrorSeverity;

// Error categories for better organization
typedef enum {
    ERROR_CATEGORY_SYNTAX,
    ERROR_CATEGORY_TYPE,
    ERROR_CATEGORY_SEMANTIC,
    ERROR_CATEGORY_OWNERSHIP,
    ERROR_CATEGORY_MEMORY,
    ERROR_CATEGORY_CONCURRENCY,
    ERROR_CATEGORY_IO,
    ERROR_CATEGORY_RUNTIME,
    ERROR_CATEGORY_INTERNAL
} ErrorCategory;

// Error codes for specific error types
typedef enum {
    // Syntax errors
    ERROR_UNEXPECTED_TOKEN = 1000,
    ERROR_MISSING_SEMICOLON,
    ERROR_UNMATCHED_BRACE,
    ERROR_INVALID_SYNTAX,
    
    // Type errors
    ERROR_TYPE_MISMATCH = 2000,
    ERROR_UNDEFINED_TYPE,
    ERROR_INCOMPATIBLE_TYPES,
    ERROR_INVALID_CAST,
    ERROR_MISSING_GENERIC_PARAMETER,
    
    // Semantic errors
    ERROR_UNDEFINED_VARIABLE = 3000,
    ERROR_UNDEFINED_FUNCTION,
    ERROR_REDEFINITION,
    ERROR_INVALID_ASSIGNMENT,
    ERROR_UNREACHABLE_CODE,
    
    // Ownership errors
    ERROR_USE_AFTER_MOVE = 4000,
    ERROR_DOUBLE_FREE,
    ERROR_INVALID_BORROW,
    ERROR_LIFETIME_MISMATCH,
    
    // Memory errors
    ERROR_NULL_DEREFERENCE = 5000,
    ERROR_BUFFER_OVERFLOW,
    ERROR_MEMORY_LEAK,
    ERROR_STACK_OVERFLOW,
    
    // Concurrency errors
    ERROR_DATA_RACE = 6000,
    ERROR_DEADLOCK,
    ERROR_CHANNEL_CLOSED,
    ERROR_INVALID_SYNC,
    
    // I/O errors
    ERROR_FILE_NOT_FOUND = 7000,
    ERROR_PERMISSION_DENIED,
    ERROR_IO_ERROR,
    
    // Runtime errors
    ERROR_DIVISION_BY_ZERO = 8000,
    ERROR_INDEX_OUT_OF_BOUNDS,
    ERROR_ASSERTION_FAILED,
    ERROR_PANIC,
    
    // Internal errors
    ERROR_COMPILER_BUG = 9000,
    ERROR_OUT_OF_MEMORY,
    ERROR_INTERNAL_ERROR
} ErrorCode;

// Source location information
struct ErrorLocation {
    const char* file_path;
    uint32_t line;
    uint32_t column;
    uint32_t length;  // Length of the problematic text
    const char* source_line;  // The actual source line
    struct ErrorLocation* next;  // For multi-location errors
};

// Error explanation with detailed information
struct ErrorExplanation {
    const char* title;
    const char* description;
    const char* why_error;
    const char* how_to_fix;
    const char* example_good;
    const char* example_bad;
    const char* related_concepts;
    const char* documentation_link;
};

// Simple suggestion for fixing the error
struct ErrorSuggestion {
    const char* description;
    const char* suggested_fix;
    bool is_automatic;  // Can be applied automatically
    struct ErrorSuggestion* next;
};

// Complete error report
struct ErrorReport {
    ErrorCode code;
    ErrorSeverity severity;
    ErrorCategory category;
    
    const char* message;
    const char* detailed_message;
    
    ErrorLocation* primary_location;
    ErrorLocation* secondary_locations;  // Related locations
    
    ErrorExplanation* explanation;
    ErrorSuggestion* suggestions;
    
    // Context information
    const char* function_context;
    const char* type_context;
    const char* expression_context;
    
    // Metadata
    uint64_t timestamp;
    uint32_t error_id;
    bool is_suppressed;
    
    struct ErrorReport* next;
};

// Enhanced error reporting context
struct ErrorReportingContext {
    ErrorReport* errors;
    ErrorReport* warnings;
    ErrorReport* notes;
    
    uint32_t error_count;
    uint32_t warning_count;
    uint32_t note_count;
    
    // Display options
    bool show_source_context;
    bool show_explanations;
    bool show_suggestions;
    bool use_colors;
    bool show_line_numbers;
    uint32_t context_lines;  // Lines of context to show
    
    // Output streams
    FILE* error_stream;
    FILE* warning_stream;
    FILE* note_stream;
    
    // Error filtering
    ErrorSeverity min_severity;
    ErrorCategory* enabled_categories;
    uint32_t num_enabled_categories;
    
    // Statistics
    uint32_t total_errors_reported;
    uint32_t errors_suppressed;
};

// =============================================================================
// Error Context Management
// =============================================================================

ErrorContext* error_context_new(void);
void error_context_free(ErrorContext* ctx);

int error_context_init(ErrorContext* ctx);
int error_context_reset(ErrorContext* ctx);

// =============================================================================
// Error Location Management
// =============================================================================

ErrorLocation* error_location_new(const char* file_path, uint32_t line, uint32_t column, uint32_t length);
void error_location_free(ErrorLocation* location);

ErrorLocation* error_location_from_source(const char* file_path, const char* source, uint32_t offset, uint32_t length);
char* error_location_get_source_context(ErrorLocation* location, uint32_t context_lines);

// =============================================================================
// Error Explanation System
// =============================================================================

ErrorExplanation* error_explanation_new(const char* title, const char* description);
void error_explanation_free(ErrorExplanation* explanation);

ErrorExplanation* error_explanation_get(ErrorCode code);
void error_explanation_set_details(ErrorExplanation* explanation, const char* why_error, const char* how_to_fix);
void error_explanation_set_examples(ErrorExplanation* explanation, const char* good_example, const char* bad_example);
void error_explanation_set_documentation(ErrorExplanation* explanation, const char* concepts, const char* doc_link);

// =============================================================================
// Error Suggestion System
// =============================================================================

ErrorSuggestion* error_suggestion_new(const char* description, const char* fix);
void error_suggestion_free(ErrorSuggestion* suggestion);

void error_suggestion_add(ErrorReport* report, ErrorSuggestion* suggestion);
ErrorSuggestion* error_suggestion_create_simple(const char* description, const char* fix);
ErrorSuggestion* error_suggestion_create_automatic(const char* description, const char* fix);

// =============================================================================
// Error Report Creation
// =============================================================================

ErrorReport* error_report_new(ErrorCode code, ErrorSeverity severity, const char* message);
void error_report_free(ErrorReport* report);

ErrorReport* error_report_create_syntax(ErrorCode code, const char* message, ErrorLocation* location);
ErrorReport* error_report_create_type(ErrorCode code, const char* message, ErrorLocation* location, const char* expected_type, const char* actual_type);
ErrorReport* error_report_create_semantic(ErrorCode code, const char* message, ErrorLocation* location, const char* context);

void error_report_set_location(ErrorReport* report, ErrorLocation* location);
void error_report_add_secondary_location(ErrorReport* report, ErrorLocation* location);
void error_report_set_explanation(ErrorReport* report, ErrorExplanation* explanation);
void error_report_set_context(ErrorReport* report, const char* function_context, const char* type_context);

// =============================================================================
// Error Reporting
// =============================================================================

int error_report_emit(ErrorContext* ctx, ErrorReport* report);
int error_report_syntax_error(ErrorContext* ctx, const char* message, const char* file_path, uint32_t line, uint32_t column);
int error_report_type_error(ErrorContext* ctx, const char* message, const char* file_path, uint32_t line, uint32_t column, const char* expected, const char* actual);
int error_report_semantic_error(ErrorContext* ctx, const char* message, const char* file_path, uint32_t line, uint32_t column);

int error_report_warning(ErrorContext* ctx, const char* message, const char* file_path, uint32_t line, uint32_t column);
int error_report_note(ErrorContext* ctx, const char* message, const char* file_path, uint32_t line, uint32_t column);

// =============================================================================
// Error Display and Formatting
// =============================================================================

void error_display_report(ErrorContext* ctx, ErrorReport* report);
void error_display_all(ErrorContext* ctx);
void error_display_summary(ErrorContext* ctx);

char* error_format_message(ErrorReport* report, bool include_explanation);
char* error_format_location(ErrorLocation* location, bool show_context);
char* error_format_source_highlight(ErrorLocation* location, bool use_colors);

// =============================================================================
// Error Filtering and Configuration
// =============================================================================

void error_context_set_severity_filter(ErrorContext* ctx, ErrorSeverity min_severity);
void error_context_enable_category(ErrorContext* ctx, ErrorCategory category);
void error_context_disable_category(ErrorContext* ctx, ErrorCategory category);

void error_context_set_display_options(ErrorContext* ctx, bool show_source, bool show_explanations, bool show_suggestions);
void error_context_set_color_output(ErrorContext* ctx, bool use_colors);
void error_context_set_context_lines(ErrorContext* ctx, uint32_t lines);

// =============================================================================
// Error Statistics and Analysis
// =============================================================================

uint32_t error_context_get_error_count(ErrorContext* ctx);
uint32_t error_context_get_warning_count(ErrorContext* ctx);
uint32_t error_context_get_total_count(ErrorContext* ctx);

ErrorReport* error_context_get_errors_by_category(ErrorContext* ctx, ErrorCategory category);
ErrorReport* error_context_get_errors_by_severity(ErrorContext* ctx, ErrorSeverity severity);

char* error_context_generate_statistics_report(ErrorContext* ctx);

// =============================================================================
// Error Persistence and Export
// =============================================================================

int error_context_export_json(ErrorContext* ctx, const char* file_path);
int error_context_export_xml(ErrorContext* ctx, const char* file_path);
int error_context_export_text(ErrorContext* ctx, const char* file_path);

int error_context_load_suppression_rules(ErrorContext* ctx, const char* file_path);
int error_context_save_suppression_rules(ErrorContext* ctx, const char* file_path);

// =============================================================================
// Integration with REPL
// =============================================================================

int error_reporting_integrate_repl(ErrorContext* ctx, REPLContext* repl);
int error_handle_repl_error_command(REPLContext* repl, ErrorContext* ctx, const char* command);

// REPL commands:
// :errors - Show all current errors
// :warnings - Show all warnings  
// :explain <error_id> - Show detailed explanation for error
// :suppress <error_id> - Suppress specific error
// :clear-errors - Clear all errors

// =============================================================================
// Utility Functions
// =============================================================================

const char* error_severity_to_string(ErrorSeverity severity);
const char* error_category_to_string(ErrorCategory category);
const char* error_code_to_string(ErrorCode code);

bool error_is_fatal(ErrorCode code);
ErrorCategory error_get_category(ErrorCode code);
ErrorSeverity error_get_default_severity(ErrorCode code);

// =============================================================================
// Error Help System
// =============================================================================

typedef struct ErrorHelpEntry {
    ErrorCode code;
    const char* title;
    const char* description;
    const char* common_causes;
    const char* solutions;
    const char* examples;
    const char* related_errors;
} ErrorHelpEntry;

ErrorHelpEntry* error_help_get_entry(ErrorCode code);
void error_help_display(ErrorCode code);
void error_help_search(const char* query);
void error_help_list_all(void);

// =============================================================================
// Color and Formatting Support
// =============================================================================

#define ERROR_COLOR_RESET   "\033[0m"
#define ERROR_COLOR_BOLD    "\033[1m"
#define ERROR_COLOR_RED     "\033[31m"
#define ERROR_COLOR_YELLOW  "\033[33m"
#define ERROR_COLOR_BLUE    "\033[34m"
#define ERROR_COLOR_GREEN   "\033[32m"
#define ERROR_COLOR_CYAN    "\033[36m"
#define ERROR_COLOR_GRAY    "\033[90m"

// Helper macros for colored output
#define ERROR_FORMAT_SEVERITY(ctx, severity) \
    ((ctx)->use_colors ? \
        (severity == ERROR_SEVERITY_ERROR ? ERROR_COLOR_RED "error" ERROR_COLOR_RESET : \
         severity == ERROR_SEVERITY_WARNING ? ERROR_COLOR_YELLOW "warning" ERROR_COLOR_RESET : \
         severity == ERROR_SEVERITY_NOTE ? ERROR_COLOR_BLUE "note" ERROR_COLOR_RESET : \
         ERROR_COLOR_RED "fatal" ERROR_COLOR_RESET) : \
        (severity == ERROR_SEVERITY_ERROR ? "error" : \
         severity == ERROR_SEVERITY_WARNING ? "warning" : \
         severity == ERROR_SEVERITY_NOTE ? "note" : "fatal"))

#define ERROR_FORMAT_LOCATION(ctx, file, line, col) \
    ((ctx)->use_colors ? \
        ERROR_COLOR_BOLD "%s:%u:%u" ERROR_COLOR_RESET : "%s:%u:%u"), \
        file, line, col

#endif // ERROR_REPORTING_H