#include "error_reporting.h"
#include "repl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

// =============================================================================
// Static Error Explanations Database
// =============================================================================

static ErrorExplanation error_explanations[] = {
    // Syntax errors
    {
        .title = "Unexpected Token",
        .description = "The parser encountered a token that doesn't belong in this context.",
        .why_error = "The syntax at this location doesn't match any valid Goo language construct.",
        .how_to_fix = "Check for missing semicolons, unmatched braces, or typos in keywords.",
        .example_good = "let x: int = 42;",
        .example_bad = "let x: int = 42",
        .related_concepts = "Syntax, Parsing, Tokens",
        .documentation_link = "https://docs.goo-lang.org/syntax"
    },
    {
        .title = "Missing Semicolon",
        .description = "A statement is missing a required semicolon terminator.",
        .why_error = "Goo requires semicolons to terminate statements for clarity and parsing.",
        .how_to_fix = "Add a semicolon ';' at the end of the statement.",
        .example_good = "let x = 42;",
        .example_bad = "let x = 42",
        .related_concepts = "Statements, Syntax",
        .documentation_link = "https://docs.goo-lang.org/statements"
    },
    
    // Type errors
    {
        .title = "Type Mismatch",
        .description = "The provided value doesn't match the expected type.",
        .why_error = "Goo is statically typed and requires exact type matches for safety.",
        .how_to_fix = "Either change the value to match the expected type or cast it explicitly.",
        .example_good = "let x: int = 42;",
        .example_bad = "let x: int = \"hello\";",
        .related_concepts = "Type System, Static Typing, Type Safety",
        .documentation_link = "https://docs.goo-lang.org/types"
    },
    {
        .title = "Undefined Type",
        .description = "The referenced type is not defined or not in scope.",
        .why_error = "All types must be declared before use in Goo.",
        .how_to_fix = "Check spelling, import the required module, or define the type.",
        .example_good = "type MyStruct struct { field: int }",
        .example_bad = "let x: UndefinedType;",
        .related_concepts = "Type Definitions, Imports, Scope",
        .documentation_link = "https://docs.goo-lang.org/type-definitions"
    },
    
    // Semantic errors
    {
        .title = "Undefined Variable",
        .description = "The referenced variable is not defined or not in scope.",
        .why_error = "All variables must be declared before use in Goo.",
        .how_to_fix = "Check spelling, declare the variable, or check if it's in scope.",
        .example_good = "let x = 42; print(x);",
        .example_bad = "print(undefinedVar);",
        .related_concepts = "Variable Declaration, Scope, Identifiers",
        .documentation_link = "https://docs.goo-lang.org/variables"
    },
    {
        .title = "Redefinition",
        .description = "A variable, function, or type is being defined multiple times.",
        .why_error = "Goo doesn't allow shadowing in the same scope for clarity.",
        .how_to_fix = "Use a different name or check if you meant to reassign instead.",
        .example_good = "let x = 42; x = 43;",
        .example_bad = "let x = 42; let x = 43;",
        .related_concepts = "Shadowing, Scope, Assignment",
        .documentation_link = "https://docs.goo-lang.org/scope"
    },
    
    // Ownership errors
    {
        .title = "Use After Move",
        .description = "Attempted to use a value after it has been moved.",
        .why_error = "Goo's ownership system prevents use-after-move to ensure memory safety.",
        .how_to_fix = "Clone the value before moving, or restructure to avoid the move.",
        .example_good = "let y = x.clone(); move(x); use(y);",
        .example_bad = "move(x); use(x);",
        .related_concepts = "Ownership, Move Semantics, Memory Safety",
        .documentation_link = "https://docs.goo-lang.org/ownership"
    },
    {
        .title = "Invalid Borrow",
        .description = "Attempted to borrow a value in an invalid way.",
        .why_error = "Goo's borrow checker ensures memory safety by preventing invalid borrows.",
        .how_to_fix = "Check borrow lifetimes and ensure no conflicting borrows exist.",
        .example_good = "let r = &x; use(r);",
        .example_bad = "let r = &x; move(x); use(r);",
        .related_concepts = "Borrowing, References, Lifetimes",
        .documentation_link = "https://docs.goo-lang.org/borrowing"
    },
    
    // Memory errors
    {
        .title = "Null Dereference",
        .description = "Attempted to dereference a null pointer or nullable value.",
        .why_error = "Dereferencing null pointers leads to undefined behavior and crashes.",
        .how_to_fix = "Check for null before dereferencing or use safe nullable operators.",
        .example_good = "if value != null { use(*value); }",
        .example_bad = "use(*nullable_value);",
        .related_concepts = "Null Safety, Nullable Types, Dereferencing",
        .documentation_link = "https://docs.goo-lang.org/null-safety"
    }
};

static const size_t num_error_explanations = sizeof(error_explanations) / sizeof(ErrorExplanation);

// =============================================================================
// Utility Functions
// =============================================================================

static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

static uint64_t get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

const char* error_severity_to_string(ErrorSeverity severity) {
    switch (severity) {
        case ERROR_SEVERITY_NOTE: return "note";
        case ERROR_SEVERITY_WARNING: return "warning";
        case ERROR_SEVERITY_ERROR: return "error";
        case ERROR_SEVERITY_FATAL: return "fatal";
        default: return "unknown";
    }
}

const char* error_category_to_string(ErrorCategory category) {
    switch (category) {
        case ERROR_CATEGORY_SYNTAX: return "syntax";
        case ERROR_CATEGORY_TYPE: return "type";
        case ERROR_CATEGORY_SEMANTIC: return "semantic";
        case ERROR_CATEGORY_OWNERSHIP: return "ownership";
        case ERROR_CATEGORY_MEMORY: return "memory";
        case ERROR_CATEGORY_CONCURRENCY: return "concurrency";
        case ERROR_CATEGORY_IO: return "io";
        case ERROR_CATEGORY_RUNTIME: return "runtime";
        case ERROR_CATEGORY_INTERNAL: return "internal";
        default: return "unknown";
    }
}

const char* error_code_to_string(ErrorCode code) {
    switch (code) {
        case ERROR_UNEXPECTED_TOKEN: return "unexpected_token";
        case ERROR_MISSING_SEMICOLON: return "missing_semicolon";
        case ERROR_UNMATCHED_BRACE: return "unmatched_brace";
        case ERROR_INVALID_SYNTAX: return "invalid_syntax";
        
        case ERROR_TYPE_MISMATCH: return "type_mismatch";
        case ERROR_UNDEFINED_TYPE: return "undefined_type";
        case ERROR_INCOMPATIBLE_TYPES: return "incompatible_types";
        case ERROR_INVALID_CAST: return "invalid_cast";
        case ERROR_MISSING_GENERIC_PARAMETER: return "missing_generic_parameter";
        
        case ERROR_UNDEFINED_VARIABLE: return "undefined_variable";
        case ERROR_UNDEFINED_FUNCTION: return "undefined_function";
        case ERROR_REDEFINITION: return "redefinition";
        case ERROR_INVALID_ASSIGNMENT: return "invalid_assignment";
        case ERROR_UNREACHABLE_CODE: return "unreachable_code";
        
        case ERROR_USE_AFTER_MOVE: return "use_after_move";
        case ERROR_DOUBLE_FREE: return "double_free";
        case ERROR_INVALID_BORROW: return "invalid_borrow";
        case ERROR_LIFETIME_MISMATCH: return "lifetime_mismatch";
        
        case ERROR_NULL_DEREFERENCE: return "null_dereference";
        case ERROR_BUFFER_OVERFLOW: return "buffer_overflow";
        case ERROR_MEMORY_LEAK: return "memory_leak";
        case ERROR_STACK_OVERFLOW: return "stack_overflow";
        
        case ERROR_DATA_RACE: return "data_race";
        case ERROR_DEADLOCK: return "deadlock";
        case ERROR_CHANNEL_CLOSED: return "channel_closed";
        case ERROR_INVALID_SYNC: return "invalid_sync";
        
        case ERROR_FILE_NOT_FOUND: return "file_not_found";
        case ERROR_PERMISSION_DENIED: return "permission_denied";
        case ERROR_IO_ERROR: return "io_error";
        
        case ERROR_DIVISION_BY_ZERO: return "division_by_zero";
        case ERROR_INDEX_OUT_OF_BOUNDS: return "index_out_of_bounds";
        case ERROR_ASSERTION_FAILED: return "assertion_failed";
        case ERROR_PANIC: return "panic";
        
        case ERROR_COMPILER_BUG: return "compiler_bug";
        case ERROR_OUT_OF_MEMORY: return "out_of_memory";
        case ERROR_INTERNAL_ERROR: return "internal_error";
        
        default: return "unknown_error";
    }
}

bool error_is_fatal(ErrorCode code) {
    return code == ERROR_COMPILER_BUG || 
           code == ERROR_OUT_OF_MEMORY || 
           code == ERROR_INTERNAL_ERROR ||
           code == ERROR_STACK_OVERFLOW;
}

ErrorCategory error_get_category(ErrorCode code) {
    if (code >= 1000 && code < 2000) return ERROR_CATEGORY_SYNTAX;
    if (code >= 2000 && code < 3000) return ERROR_CATEGORY_TYPE;
    if (code >= 3000 && code < 4000) return ERROR_CATEGORY_SEMANTIC;
    if (code >= 4000 && code < 5000) return ERROR_CATEGORY_OWNERSHIP;
    if (code >= 5000 && code < 6000) return ERROR_CATEGORY_MEMORY;
    if (code >= 6000 && code < 7000) return ERROR_CATEGORY_CONCURRENCY;
    if (code >= 7000 && code < 8000) return ERROR_CATEGORY_IO;
    if (code >= 8000 && code < 9000) return ERROR_CATEGORY_RUNTIME;
    if (code >= 9000) return ERROR_CATEGORY_INTERNAL;
    return ERROR_CATEGORY_INTERNAL;
}

ErrorSeverity error_get_default_severity(ErrorCode code) {
    if (error_is_fatal(code)) return ERROR_SEVERITY_FATAL;
    
    ErrorCategory category = error_get_category(code);
    switch (category) {
        case ERROR_CATEGORY_SYNTAX:
        case ERROR_CATEGORY_TYPE:
        case ERROR_CATEGORY_SEMANTIC:
        case ERROR_CATEGORY_OWNERSHIP:
        case ERROR_CATEGORY_MEMORY:
            return ERROR_SEVERITY_ERROR;
        case ERROR_CATEGORY_CONCURRENCY:
        case ERROR_CATEGORY_RUNTIME:
            return ERROR_SEVERITY_ERROR;
        case ERROR_CATEGORY_IO:
            return ERROR_SEVERITY_WARNING;
        case ERROR_CATEGORY_INTERNAL:
            return ERROR_SEVERITY_FATAL;
        default:
            return ERROR_SEVERITY_ERROR;
    }
}

// =============================================================================
// Error Context Management
// =============================================================================

ErrorContext* error_context_new(void) {
    ErrorContext* ctx = calloc(1, sizeof(ErrorContext));
    if (!ctx) return NULL;
    
    // Initialize display options
    ctx->show_source_context = true;
    ctx->show_explanations = true;
    ctx->show_suggestions = true;
    ctx->use_colors = true;
    ctx->show_line_numbers = true;
    ctx->context_lines = 3;
    
    // Initialize output streams
    ctx->error_stream = stderr;
    ctx->warning_stream = stderr;
    ctx->note_stream = stdout;
    
    // Initialize filtering
    ctx->min_severity = ERROR_SEVERITY_NOTE;
    ctx->enabled_categories = NULL;
    ctx->num_enabled_categories = 0;
    
    return ctx;
}

void error_context_free(ErrorContext* ctx) {
    if (!ctx) return;
    
    // Free error lists
    ErrorReport* error = ctx->errors;
    while (error) {
        ErrorReport* next = error->next;
        error_report_free(error);
        error = next;
    }
    
    ErrorReport* warning = ctx->warnings;
    while (warning) {
        ErrorReport* next = warning->next;
        error_report_free(warning);
        warning = next;
    }
    
    ErrorReport* note = ctx->notes;
    while (note) {
        ErrorReport* next = note->next;
        error_report_free(note);
        note = next;
    }
    
    free(ctx->enabled_categories);
    free(ctx);
}

int error_context_init(ErrorContext* ctx) {
    if (!ctx) return -1;
    
    // Enable all categories by default
    ctx->num_enabled_categories = 9;
    ctx->enabled_categories = malloc(sizeof(ErrorCategory) * ctx->num_enabled_categories);
    if (!ctx->enabled_categories) return -1;
    
    for (int i = 0; i < 9; i++) {
        ctx->enabled_categories[i] = (ErrorCategory)i;
    }
    
    return 0;
}

int error_context_reset(ErrorContext* ctx) {
    if (!ctx) return -1;
    
    // Free and reset error lists
    ErrorReport* error = ctx->errors;
    while (error) {
        ErrorReport* next = error->next;
        error_report_free(error);
        error = next;
    }
    ctx->errors = NULL;
    
    ErrorReport* warning = ctx->warnings;
    while (warning) {
        ErrorReport* next = warning->next;
        error_report_free(warning);
        warning = next;
    }
    ctx->warnings = NULL;
    
    ErrorReport* note = ctx->notes;
    while (note) {
        ErrorReport* next = note->next;
        error_report_free(note);
        note = next;
    }
    ctx->notes = NULL;
    
    // Reset counters
    ctx->error_count = 0;
    ctx->warning_count = 0;
    ctx->note_count = 0;
    
    return 0;
}

// =============================================================================
// Error Location Management
// =============================================================================

ErrorLocation* error_location_new(const char* file_path, uint32_t line, uint32_t column, uint32_t length) {
    ErrorLocation* location = calloc(1, sizeof(ErrorLocation));
    if (!location) return NULL;
    
    location->file_path = str_dup(file_path);
    location->line = line;
    location->column = column;
    location->length = length;
    
    return location;
}

void error_location_free(ErrorLocation* location) {
    if (!location) return;
    
    free((char*)location->file_path);
    free((char*)location->source_line);
    
    if (location->next) {
        error_location_free(location->next);
    }
    
    free(location);
}

ErrorLocation* error_location_from_source(const char* file_path, const char* source, uint32_t offset, uint32_t length) {
    if (!file_path || !source) return NULL;
    
    // Find line and column from offset
    uint32_t line = 1;
    uint32_t column = 1;
    uint32_t line_start = 0;
    
    for (uint32_t i = 0; i < offset && source[i]; i++) {
        if (source[i] == '\n') {
            line++;
            column = 1;
            line_start = i + 1;
        } else {
            column++;
        }
    }
    
    // Extract the source line
    uint32_t line_end = line_start;
    while (source[line_end] && source[line_end] != '\n') {
        line_end++;
    }
    
    ErrorLocation* location = error_location_new(file_path, line, column, length);
    if (location) {
        size_t line_len = line_end - line_start;
        char* source_line = malloc(line_len + 1);
        if (source_line) {
            strncpy(source_line, source + line_start, line_len);
            source_line[line_len] = '\0';
            location->source_line = source_line;
        }
    }
    
    return location;
}

char* error_location_get_source_context(ErrorLocation* location, uint32_t context_lines) {
    if (!location || !location->source_line) return NULL;
    
    // For now, just return the current line
    // In a full implementation, this would read the file and get surrounding lines
    return str_dup(location->source_line);
}

// =============================================================================
// Error Explanation System
// =============================================================================

ErrorExplanation* error_explanation_new(const char* title, const char* description) {
    ErrorExplanation* explanation = calloc(1, sizeof(ErrorExplanation));
    if (!explanation) return NULL;
    
    explanation->title = str_dup(title);
    explanation->description = str_dup(description);
    
    return explanation;
}

void error_explanation_free(ErrorExplanation* explanation) {
    if (!explanation) return;
    
    free((char*)explanation->title);
    free((char*)explanation->description);
    free((char*)explanation->why_error);
    free((char*)explanation->how_to_fix);
    free((char*)explanation->example_good);
    free((char*)explanation->example_bad);
    free((char*)explanation->related_concepts);
    free((char*)explanation->documentation_link);
    free(explanation);
}

ErrorExplanation* error_explanation_get(ErrorCode code) {
    // Find explanation by error code (simplified implementation)
    for (size_t i = 0; i < num_error_explanations; i++) {
        // Map error codes to explanations (simplified)
        if ((code >= ERROR_UNEXPECTED_TOKEN && code <= ERROR_INVALID_SYNTAX && i < 2) ||
            (code >= ERROR_TYPE_MISMATCH && code <= ERROR_MISSING_GENERIC_PARAMETER && i >= 2 && i < 4) ||
            (code >= ERROR_UNDEFINED_VARIABLE && code <= ERROR_UNREACHABLE_CODE && i >= 4 && i < 6) ||
            (code >= ERROR_USE_AFTER_MOVE && code <= ERROR_LIFETIME_MISMATCH && i >= 6 && i < 8) ||
            (code >= ERROR_NULL_DEREFERENCE && code <= ERROR_STACK_OVERFLOW && i >= 8)) {
            
            ErrorExplanation* copy = error_explanation_new(error_explanations[i].title, 
                                                         error_explanations[i].description);
            if (copy) {
                copy->why_error = str_dup(error_explanations[i].why_error);
                copy->how_to_fix = str_dup(error_explanations[i].how_to_fix);
                copy->example_good = str_dup(error_explanations[i].example_good);
                copy->example_bad = str_dup(error_explanations[i].example_bad);
                copy->related_concepts = str_dup(error_explanations[i].related_concepts);
                copy->documentation_link = str_dup(error_explanations[i].documentation_link);
            }
            return copy;
        }
    }
    
    return NULL;
}

void error_explanation_set_details(ErrorExplanation* explanation, const char* why_error, const char* how_to_fix) {
    if (!explanation) return;
    
    free((char*)explanation->why_error);
    free((char*)explanation->how_to_fix);
    
    explanation->why_error = str_dup(why_error);
    explanation->how_to_fix = str_dup(how_to_fix);
}

void error_explanation_set_examples(ErrorExplanation* explanation, const char* good_example, const char* bad_example) {
    if (!explanation) return;
    
    free((char*)explanation->example_good);
    free((char*)explanation->example_bad);
    
    explanation->example_good = str_dup(good_example);
    explanation->example_bad = str_dup(bad_example);
}

void error_explanation_set_documentation(ErrorExplanation* explanation, const char* concepts, const char* doc_link) {
    if (!explanation) return;
    
    free((char*)explanation->related_concepts);
    free((char*)explanation->documentation_link);
    
    explanation->related_concepts = str_dup(concepts);
    explanation->documentation_link = str_dup(doc_link);
}

// =============================================================================
// Error Suggestion System
// =============================================================================

ErrorSuggestion* error_suggestion_new(const char* description, const char* fix) {
    ErrorSuggestion* suggestion = calloc(1, sizeof(ErrorSuggestion));
    if (!suggestion) return NULL;
    
    suggestion->description = str_dup(description);
    suggestion->suggested_fix = str_dup(fix);
    suggestion->is_automatic = false;
    
    return suggestion;
}

void error_suggestion_free(ErrorSuggestion* suggestion) {
    if (!suggestion) return;
    
    free((char*)suggestion->description);
    free((char*)suggestion->suggested_fix);
    
    if (suggestion->next) {
        error_suggestion_free(suggestion->next);
    }
    
    free(suggestion);
}

void error_suggestion_add(ErrorReport* report, ErrorSuggestion* suggestion) {
    if (!report || !suggestion) return;
    
    suggestion->next = report->suggestions;
    report->suggestions = suggestion;
}

ErrorSuggestion* error_suggestion_create_simple(const char* description, const char* fix) {
    return error_suggestion_new(description, fix);
}

ErrorSuggestion* error_suggestion_create_automatic(const char* description, const char* fix) {
    ErrorSuggestion* suggestion = error_suggestion_new(description, fix);
    if (suggestion) {
        suggestion->is_automatic = true;
    }
    return suggestion;
}

// =============================================================================
// Error Report Creation
// =============================================================================

static uint32_t next_error_id = 1;

ErrorReport* error_report_new(ErrorCode code, ErrorSeverity severity, const char* message) {
    ErrorReport* report = calloc(1, sizeof(ErrorReport));
    if (!report) return NULL;
    
    report->code = code;
    report->severity = severity;
    report->category = error_get_category(code);
    report->message = str_dup(message);
    report->timestamp = get_timestamp_ms();
    report->error_id = next_error_id++;
    report->is_suppressed = false;
    
    // Auto-add explanation if available
    report->explanation = error_explanation_get(code);
    
    return report;
}

void error_report_free(ErrorReport* report) {
    if (!report) return;
    
    free((char*)report->message);
    free((char*)report->detailed_message);
    free((char*)report->function_context);
    free((char*)report->type_context);
    free((char*)report->expression_context);
    
    error_location_free(report->primary_location);
    error_location_free(report->secondary_locations);
    error_explanation_free(report->explanation);
    error_suggestion_free(report->suggestions);
    
    free(report);
}

ErrorReport* error_report_create_syntax(ErrorCode code, const char* message, ErrorLocation* location) {
    ErrorReport* report = error_report_new(code, ERROR_SEVERITY_ERROR, message);
    if (report) {
        report->primary_location = location;
    }
    return report;
}

ErrorReport* error_report_create_type(ErrorCode code, const char* message, ErrorLocation* location, 
                                    const char* expected_type, const char* actual_type) {
    ErrorReport* report = error_report_new(code, ERROR_SEVERITY_ERROR, message);
    if (report) {
        report->primary_location = location;
        
        // Create detailed message with type information
        char detailed[512];
        snprintf(detailed, sizeof(detailed), 
                "%s\n  Expected type: %s\n  Actual type: %s", 
                message, expected_type, actual_type);
        report->detailed_message = str_dup(detailed);
        
        // Add suggestion
        char suggestion[256];
        snprintf(suggestion, sizeof(suggestion), 
                "Cast the value to %s or change the expected type", expected_type);
        ErrorSuggestion* fix = error_suggestion_create_simple("Type mismatch", suggestion);
        error_suggestion_add(report, fix);
    }
    return report;
}

ErrorReport* error_report_create_semantic(ErrorCode code, const char* message, ErrorLocation* location, const char* context) {
    ErrorReport* report = error_report_new(code, ERROR_SEVERITY_ERROR, message);
    if (report) {
        report->primary_location = location;
        report->expression_context = str_dup(context);
    }
    return report;
}

void error_report_set_location(ErrorReport* report, ErrorLocation* location) {
    if (!report) return;
    
    if (report->primary_location) {
        error_location_free(report->primary_location);
    }
    report->primary_location = location;
}

void error_report_add_secondary_location(ErrorReport* report, ErrorLocation* location) {
    if (!report || !location) return;
    
    location->next = report->secondary_locations;
    report->secondary_locations = location;
}

void error_report_set_explanation(ErrorReport* report, ErrorExplanation* explanation) {
    if (!report) return;
    
    if (report->explanation) {
        error_explanation_free(report->explanation);
    }
    report->explanation = explanation;
}

void error_report_set_context(ErrorReport* report, const char* function_context, const char* type_context) {
    if (!report) return;
    
    free((char*)report->function_context);
    free((char*)report->type_context);
    
    report->function_context = str_dup(function_context);
    report->type_context = str_dup(type_context);
}

// =============================================================================
// Error Reporting
// =============================================================================

int error_report_emit(ErrorContext* ctx, ErrorReport* report) {
    if (!ctx || !report) return -1;
    
    // Check if error should be filtered
    if (report->severity < ctx->min_severity) {
        return 0;  // Filtered out
    }
    
    // Check category filter
    bool category_enabled = false;
    for (uint32_t i = 0; i < ctx->num_enabled_categories; i++) {
        if (ctx->enabled_categories[i] == report->category) {
            category_enabled = true;
            break;
        }
    }
    
    if (!category_enabled) {
        return 0;  // Category disabled
    }
    
    // Add to appropriate list
    switch (report->severity) {
        case ERROR_SEVERITY_ERROR:
        case ERROR_SEVERITY_FATAL:
            report->next = ctx->errors;
            ctx->errors = report;
            ctx->error_count++;
            break;
        case ERROR_SEVERITY_WARNING:
            report->next = ctx->warnings;
            ctx->warnings = report;
            ctx->warning_count++;
            break;
        case ERROR_SEVERITY_NOTE:
            report->next = ctx->notes;
            ctx->notes = report;
            ctx->note_count++;
            break;
    }
    
    ctx->total_errors_reported++;
    
    // Display immediately
    error_display_report(ctx, report);
    
    return 0;
}

int error_report_syntax_error(ErrorContext* ctx, const char* message, const char* file_path, 
                             uint32_t line, uint32_t column) {
    if (!ctx || !message) return -1;
    
    ErrorLocation* location = error_location_new(file_path, line, column, 1);
    ErrorReport* report = error_report_create_syntax(ERROR_INVALID_SYNTAX, message, location);
    
    return error_report_emit(ctx, report);
}

int error_report_type_error(ErrorContext* ctx, const char* message, const char* file_path, 
                           uint32_t line, uint32_t column, const char* expected, const char* actual) {
    if (!ctx || !message) return -1;
    
    ErrorLocation* location = error_location_new(file_path, line, column, 1);
    ErrorReport* report = error_report_create_type(ERROR_TYPE_MISMATCH, message, location, expected, actual);
    
    return error_report_emit(ctx, report);
}

int error_report_semantic_error(ErrorContext* ctx, const char* message, const char* file_path, 
                               uint32_t line, uint32_t column) {
    if (!ctx || !message) return -1;
    
    ErrorLocation* location = error_location_new(file_path, line, column, 1);
    ErrorReport* report = error_report_create_semantic(ERROR_UNDEFINED_VARIABLE, message, location, NULL);
    
    return error_report_emit(ctx, report);
}

int error_report_warning(ErrorContext* ctx, const char* message, const char* file_path, 
                        uint32_t line, uint32_t column) {
    if (!ctx || !message) return -1;
    
    ErrorLocation* location = error_location_new(file_path, line, column, 1);
    ErrorReport* report = error_report_new(ERROR_UNREACHABLE_CODE, ERROR_SEVERITY_WARNING, message);
    if (report) {
        report->primary_location = location;
        return error_report_emit(ctx, report);
    }
    
    return -1;
}

int error_report_note(ErrorContext* ctx, const char* message, const char* file_path, 
                     uint32_t line, uint32_t column) {
    if (!ctx || !message) return -1;
    
    ErrorLocation* location = error_location_new(file_path, line, column, 1);
    ErrorReport* report = error_report_new(ERROR_UNREACHABLE_CODE, ERROR_SEVERITY_NOTE, message);
    if (report) {
        report->primary_location = location;
        return error_report_emit(ctx, report);
    }
    
    return -1;
}

// =============================================================================
// Error Display and Formatting
// =============================================================================

void error_display_report(ErrorContext* ctx, ErrorReport* report) {
    if (!ctx || !report) return;
    
    FILE* output = ctx->error_stream;
    if (report->severity == ERROR_SEVERITY_WARNING) {
        output = ctx->warning_stream;
    } else if (report->severity == ERROR_SEVERITY_NOTE) {
        output = ctx->note_stream;
    }
    
    // Display main error message
    if (ctx->use_colors) {
        const char* severity_str = (report->severity == ERROR_SEVERITY_ERROR ? ERROR_COLOR_RED "error" ERROR_COLOR_RESET :
                                  report->severity == ERROR_SEVERITY_WARNING ? ERROR_COLOR_YELLOW "warning" ERROR_COLOR_RESET :
                                  report->severity == ERROR_SEVERITY_NOTE ? ERROR_COLOR_BLUE "note" ERROR_COLOR_RESET :
                                  ERROR_COLOR_RED "fatal" ERROR_COLOR_RESET);
        if (report->primary_location) {
            fprintf(output, "%s at %s%s:%u:%u%s: %s\n", 
                    severity_str,
                    ERROR_COLOR_BOLD, report->primary_location->file_path,
                    report->primary_location->line, report->primary_location->column,
                    ERROR_COLOR_RESET, report->message);
        } else {
            fprintf(output, "%s: %s\n", severity_str, report->message);
        }
    } else {
        if (report->primary_location) {
            fprintf(output, "%s at %s:%u:%u: %s\n",
                    error_severity_to_string(report->severity),
                    report->primary_location->file_path,
                    report->primary_location->line,
                    report->primary_location->column,
                    report->message);
        } else {
            fprintf(output, "%s: %s\n", error_severity_to_string(report->severity), report->message);
        }
    }
    
    // Display source context if available and enabled
    if (ctx->show_source_context && report->primary_location && report->primary_location->source_line) {
        fprintf(output, "   |\n");
        if (ctx->show_line_numbers) {
            fprintf(output, "%3u | %s\n", report->primary_location->line, report->primary_location->source_line);
        } else {
            fprintf(output, "   | %s\n", report->primary_location->source_line);
        }
        
        // Show pointer to error location
        fprintf(output, "   | ");
        for (uint32_t i = 1; i < report->primary_location->column; i++) {
            fprintf(output, " ");
        }
        if (ctx->use_colors) {
            fprintf(output, "%s^", ERROR_COLOR_RED);
            for (uint32_t i = 1; i < report->primary_location->length; i++) {
                fprintf(output, "~");
            }
            fprintf(output, "%s\n", ERROR_COLOR_RESET);
        } else {
            fprintf(output, "^");
            for (uint32_t i = 1; i < report->primary_location->length; i++) {
                fprintf(output, "~");
            }
            fprintf(output, "\n");
        }
    }
    
    // Display detailed message if available
    if (report->detailed_message) {
        fprintf(output, "\nDetails:\n%s\n", report->detailed_message);
    }
    
    // Display explanation if enabled and available
    if (ctx->show_explanations && report->explanation) {
        fprintf(output, "\nExplanation:\n");
        fprintf(output, "  %s\n", report->explanation->description);
        if (report->explanation->why_error) {
            fprintf(output, "  Why: %s\n", report->explanation->why_error);
        }
        if (report->explanation->how_to_fix) {
            fprintf(output, "  Fix: %s\n", report->explanation->how_to_fix);
        }
    }
    
    // Display suggestions if enabled and available
    if (ctx->show_suggestions && report->suggestions) {
        fprintf(output, "\nSuggestions:\n");
        ErrorSuggestion* suggestion = report->suggestions;
        int count = 1;
        while (suggestion) {
            fprintf(output, "  %d. %s\n", count++, suggestion->description);
            if (suggestion->suggested_fix) {
                fprintf(output, "     %s\n", suggestion->suggested_fix);
            }
            suggestion = suggestion->next;
        }
    }
    
    fprintf(output, "\n");
}

void error_display_all(ErrorContext* ctx) {
    if (!ctx) return;
    
    // Display errors
    ErrorReport* error = ctx->errors;
    while (error) {
        error_display_report(ctx, error);
        error = error->next;
    }
    
    // Display warnings
    ErrorReport* warning = ctx->warnings;
    while (warning) {
        error_display_report(ctx, warning);
        warning = warning->next;
    }
    
    // Display notes
    ErrorReport* note = ctx->notes;
    while (note) {
        error_display_report(ctx, note);
        note = note->next;
    }
}

void error_display_summary(ErrorContext* ctx) {
    if (!ctx) return;
    
    if (ctx->error_count == 0 && ctx->warning_count == 0) {
        if (ctx->use_colors) {
            fprintf(ctx->error_stream, "%sNo errors or warnings%s\n", 
                    ERROR_COLOR_GREEN, ERROR_COLOR_RESET);
        } else {
            fprintf(ctx->error_stream, "No errors or warnings\n");
        }
        return;
    }
    
    fprintf(ctx->error_stream, "\nSummary: ");
    
    if (ctx->error_count > 0) {
        if (ctx->use_colors) {
            fprintf(ctx->error_stream, "%s%u error%s%s", 
                    ERROR_COLOR_RED, ctx->error_count, 
                    ctx->error_count == 1 ? "" : "s", ERROR_COLOR_RESET);
        } else {
            fprintf(ctx->error_stream, "%u error%s", 
                    ctx->error_count, ctx->error_count == 1 ? "" : "s");
        }
    }
    
    if (ctx->warning_count > 0) {
        if (ctx->error_count > 0) {
            fprintf(ctx->error_stream, ", ");
        }
        if (ctx->use_colors) {
            fprintf(ctx->error_stream, "%s%u warning%s%s", 
                    ERROR_COLOR_YELLOW, ctx->warning_count, 
                    ctx->warning_count == 1 ? "" : "s", ERROR_COLOR_RESET);
        } else {
            fprintf(ctx->error_stream, "%u warning%s", 
                    ctx->warning_count, ctx->warning_count == 1 ? "" : "s");
        }
    }
    
    fprintf(ctx->error_stream, "\n");
}

// =============================================================================
// Error Filtering and Configuration
// =============================================================================

void error_context_set_severity_filter(ErrorContext* ctx, ErrorSeverity min_severity) {
    if (!ctx) return;
    ctx->min_severity = min_severity;
}

void error_context_enable_category(ErrorContext* ctx, ErrorCategory category) {
    if (!ctx) return;
    
    // Check if already enabled
    for (uint32_t i = 0; i < ctx->num_enabled_categories; i++) {
        if (ctx->enabled_categories[i] == category) {
            return;  // Already enabled
        }
    }
    
    // Add to enabled categories
    ErrorCategory* new_categories = realloc(ctx->enabled_categories, 
                                          sizeof(ErrorCategory) * (ctx->num_enabled_categories + 1));
    if (new_categories) {
        ctx->enabled_categories = new_categories;
        ctx->enabled_categories[ctx->num_enabled_categories] = category;
        ctx->num_enabled_categories++;
    }
}

void error_context_disable_category(ErrorContext* ctx, ErrorCategory category) {
    if (!ctx) return;
    
    // Find and remove category
    for (uint32_t i = 0; i < ctx->num_enabled_categories; i++) {
        if (ctx->enabled_categories[i] == category) {
            // Shift remaining categories
            for (uint32_t j = i; j < ctx->num_enabled_categories - 1; j++) {
                ctx->enabled_categories[j] = ctx->enabled_categories[j + 1];
            }
            ctx->num_enabled_categories--;
            break;
        }
    }
}

void error_context_set_display_options(ErrorContext* ctx, bool show_source, bool show_explanations, bool show_suggestions) {
    if (!ctx) return;
    
    ctx->show_source_context = show_source;
    ctx->show_explanations = show_explanations;
    ctx->show_suggestions = show_suggestions;
}

void error_context_set_color_output(ErrorContext* ctx, bool use_colors) {
    if (!ctx) return;
    ctx->use_colors = use_colors;
}

void error_context_set_context_lines(ErrorContext* ctx, uint32_t lines) {
    if (!ctx) return;
    ctx->context_lines = lines;
}

// =============================================================================
// Error Statistics and Analysis
// =============================================================================

uint32_t error_context_get_error_count(ErrorContext* ctx) {
    return ctx ? ctx->error_count : 0;
}

uint32_t error_context_get_warning_count(ErrorContext* ctx) {
    return ctx ? ctx->warning_count : 0;
}

uint32_t error_context_get_total_count(ErrorContext* ctx) {
    return ctx ? (ctx->error_count + ctx->warning_count + ctx->note_count) : 0;
}

ErrorReport* error_context_get_errors_by_category(ErrorContext* ctx, ErrorCategory category) {
    if (!ctx) return NULL;
    
    // Simple implementation - would need more sophisticated filtering in practice
    ErrorReport* result = NULL;
    ErrorReport* error = ctx->errors;
    
    while (error) {
        if (error->category == category) {
            // Create a copy for the result list
            ErrorReport* copy = error_report_new(error->code, error->severity, error->message);
            if (copy) {
                copy->next = result;
                result = copy;
            }
        }
        error = error->next;
    }
    
    return result;
}

ErrorReport* error_context_get_errors_by_severity(ErrorContext* ctx, ErrorSeverity severity) {
    if (!ctx) return NULL;
    
    switch (severity) {
        case ERROR_SEVERITY_ERROR:
        case ERROR_SEVERITY_FATAL:
            return ctx->errors;
        case ERROR_SEVERITY_WARNING:
            return ctx->warnings;
        case ERROR_SEVERITY_NOTE:
            return ctx->notes;
        default:
            return NULL;
    }
}

char* error_context_generate_statistics_report(ErrorContext* ctx) {
    if (!ctx) return NULL;
    
    char* report = malloc(1024);
    if (!report) return NULL;
    
    snprintf(report, 1024,
        "Error Statistics Report\n"
        "======================\n"
        "Total Errors: %u\n"
        "Total Warnings: %u\n"
        "Total Notes: %u\n"
        "Total Reported: %u\n"
        "Errors Suppressed: %u\n"
        "\nCategories:\n"
        "- Syntax: %u\n"
        "- Type: %u\n"
        "- Semantic: %u\n"
        "- Ownership: %u\n"
        "- Memory: %u\n"
        "- Concurrency: %u\n"
        "- I/O: %u\n"
        "- Runtime: %u\n"
        "- Internal: %u\n",
        ctx->error_count,
        ctx->warning_count,
        ctx->note_count,
        ctx->total_errors_reported,
        ctx->errors_suppressed,
        0, 0, 0, 0, 0, 0, 0, 0, 0  // Would need proper category counting
    );
    
    return report;
}

// =============================================================================
// REPL Integration (Simplified)
// =============================================================================

int error_reporting_integrate_repl(ErrorContext* ctx, REPLContext* repl) {
    if (!ctx || !repl) return -1;
    // Integration logic would go here
    return 0;
}

int error_handle_repl_error_command(REPLContext* repl, ErrorContext* ctx, const char* command) {
    if (!repl || !ctx || !command) return -1;
    
    if (strstr(command, "errors")) {
        error_display_all(ctx);
    } else if (strstr(command, "warnings")) {
        ErrorReport* warning = ctx->warnings;
        while (warning) {
            error_display_report(ctx, warning);
            warning = warning->next;
        }
    } else if (strstr(command, "clear-errors")) {
        error_context_reset(ctx);
        printf("All errors cleared.\n");
    } else if (strstr(command, "summary")) {
        error_display_summary(ctx);
    } else {
        printf("Unknown error command. Available: errors, warnings, clear-errors, summary\n");
        return -1;
    }
    
    return 0;
}

// =============================================================================
// Error Help System (Simplified)
// =============================================================================

ErrorHelpEntry* error_help_get_entry(ErrorCode code) {
    // Simplified implementation
    return NULL;
}

void error_help_display(ErrorCode code) {
    ErrorExplanation* explanation = error_explanation_get(code);
    if (explanation) {
        printf("Help for %s:\n", error_code_to_string(code));
        printf("Description: %s\n", explanation->description);
        if (explanation->why_error) {
            printf("Why: %s\n", explanation->why_error);
        }
        if (explanation->how_to_fix) {
            printf("How to fix: %s\n", explanation->how_to_fix);
        }
        if (explanation->example_good) {
            printf("Good example: %s\n", explanation->example_good);
        }
        if (explanation->example_bad) {
            printf("Bad example: %s\n", explanation->example_bad);
        }
        error_explanation_free(explanation);
    } else {
        printf("No help available for error code %d\n", code);
    }
}

void error_help_search(const char* query) {
    printf("Searching for: %s\n", query);
    // Would implement search through error explanations
}

void error_help_list_all(void) {
    printf("Available error help topics:\n");
    for (size_t i = 0; i < num_error_explanations; i++) {
        printf("- %s: %s\n", error_explanations[i].title, error_explanations[i].description);
    }
}

// =============================================================================
// Simplified Export Functions
// =============================================================================

int error_context_export_json(ErrorContext* ctx, const char* file_path) {
    if (!ctx || !file_path) return -1;
    
    FILE* file = fopen(file_path, "w");
    if (!file) return -1;
    
    fprintf(file, "{\n");
    fprintf(file, "  \"error_count\": %u,\n", ctx->error_count);
    fprintf(file, "  \"warning_count\": %u,\n", ctx->warning_count);
    fprintf(file, "  \"note_count\": %u\n", ctx->note_count);
    fprintf(file, "}\n");
    
    fclose(file);
    return 0;
}

int error_context_export_xml(ErrorContext* ctx, const char* file_path) {
    if (!ctx || !file_path) return -1;
    // Would implement XML export
    return 0;
}

int error_context_export_text(ErrorContext* ctx, const char* file_path) {
    if (!ctx || !file_path) return -1;
    
    FILE* file = fopen(file_path, "w");
    if (!file) return -1;
    
    // Redirect output to file temporarily
    FILE* old_error_stream = ctx->error_stream;
    ctx->error_stream = file;
    
    error_display_all(ctx);
    error_display_summary(ctx);
    
    ctx->error_stream = old_error_stream;
    fclose(file);
    
    return 0;
}

int error_context_load_suppression_rules(ErrorContext* ctx, const char* file_path) {
    if (!ctx || !file_path) return -1;
    // Would implement loading suppression rules
    return 0;
}

int error_context_save_suppression_rules(ErrorContext* ctx, const char* file_path) {
    if (!ctx || !file_path) return -1;
    // Would implement saving suppression rules  
    return 0;
}