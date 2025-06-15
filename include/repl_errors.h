#ifndef REPL_ERRORS_H
#define REPL_ERRORS_H

#include "errors/error.h"
#include <stdbool.h>

// Forward declaration
typedef struct REPLContext REPLContext;

// =============================================================================
// REPL Error Integration
// =============================================================================

// Enhanced error display for REPL
void repl_error_display_detailed(ErrorContext* ctx, Error* error);
void repl_error_display_all_detailed(ErrorContext* ctx);
void repl_error_display_summary(ErrorContext* ctx);

// Error explanation system for REPL
void repl_error_explain(ErrorCode code);
void repl_error_help_search(const char* query);
void repl_error_help_list(void);

// REPL-specific error commands
int repl_handle_error_command(REPLContext* repl, ErrorContext* error_ctx, const char* command);

// Error categorization for display
const char* repl_error_get_category_name(ErrorCode code);
const char* repl_error_get_severity_color(ErrorSeverity severity);
bool repl_error_has_suggestion(ErrorCode code);
char* repl_error_get_suggestion(ErrorCode code, const char* context);

// Enhanced error formatting
char* repl_format_error_with_context(Error* error, bool use_colors);
char* repl_format_error_location(Error* error, bool use_colors);
char* repl_format_error_suggestion(ErrorCode code, const char* context);

#endif // REPL_ERRORS_H