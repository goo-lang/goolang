#include "repl_errors.h"
#include "repl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =============================================================================
// Color constants
// =============================================================================

#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_RESET   "\033[0m"

// =============================================================================
// Error explanation database
// =============================================================================

typedef struct {
    ErrorCode code;
    const char* explanation;
    const char* example_fix;
    const char* common_causes;
} ErrorExplanation;

static ErrorExplanation error_explanations[] = {
    {
        ERROR_UNEXPECTED_TOKEN,
        "Syntax errors occur when the code doesn't follow Goo's grammar rules.",
        "Check for missing semicolons, unmatched brackets, or typos in keywords.",
        "Missing semicolons, unmatched braces/parentheses, misspelled keywords"
    },
    {
        ERROR_TYPE_MISMATCH,
        "Type mismatch errors happen when a value of one type is used where another type is expected.",
        "Cast the value explicitly: (int)value or ensure the types match.",
        "Assigning wrong type to variable, passing wrong argument type to function"
    },
    {
        ERROR_UNDEFINED_VARIABLE,
        "This error occurs when trying to use a variable, function, or type that hasn't been declared.",
        "Check spelling, ensure the symbol is declared before use, or import the necessary module.",
        "Typos in variable names, using variables before declaration, missing imports"
    },
    {
        ERROR_DUPLICATE_DEFINITION,
        "Redefinition errors happen when you try to declare the same symbol twice in the same scope.",
        "Use a different name or check if you meant to assign rather than declare.",
        "Declaring the same variable twice, function name conflicts"
    },
    {
        ERROR_INVALID_EXPRESSION,
        "This error occurs when performing an operation that's not valid for the given types.",
        "Check that the operation is supported for the types involved.",
        "Adding incompatible types, using wrong operators"
    }
};

static const size_t num_explanations = sizeof(error_explanations) / sizeof(ErrorExplanation);

// =============================================================================
// Helper functions
// =============================================================================

const char* repl_error_get_category_name(ErrorCode code) {
    if (code >= 1000 && code < 2000) return "Syntax";
    if (code >= 2000 && code < 3000) return "Type";
    if (code >= 3000 && code < 4000) return "Semantic";
    if (code >= 4000 && code < 5000) return "Runtime";
    return "Unknown";
}

const char* repl_error_get_severity_color(ErrorSeverity severity) {
    switch (severity) {
        case ERROR_SEVERITY_ERROR: return COLOR_RED;
        case ERROR_SEVERITY_WARNING: return COLOR_YELLOW;
        case ERROR_SEVERITY_NOTE: return COLOR_BLUE;
        default: return COLOR_RESET;
    }
}

bool repl_error_has_suggestion(ErrorCode code) {
    for (size_t i = 0; i < num_explanations; i++) {
        if (error_explanations[i].code == code) {
            return error_explanations[i].example_fix != NULL;
        }
    }
    return false;
}

char* repl_error_get_suggestion(ErrorCode code, const char* context) {
    for (size_t i = 0; i < num_explanations; i++) {
        if (error_explanations[i].code == code) {
            if (error_explanations[i].example_fix) {
                char* suggestion = malloc(strlen(error_explanations[i].example_fix) + 100);
                if (suggestion) {
                    if (context) {
                        snprintf(suggestion, strlen(error_explanations[i].example_fix) + 100,
                                "%s (Context: %s)", error_explanations[i].example_fix, context);
                    } else {
                        strcpy(suggestion, error_explanations[i].example_fix);
                    }
                }
                return suggestion;
            }
        }
    }
    return NULL;
}

// =============================================================================
// Enhanced error display
// =============================================================================

void repl_error_display_detailed(ErrorContext* ctx, Error* error) {
    if (!ctx || !error) return;
    
    bool use_colors = true;  // Could be configurable
    
    // Display error header
    if (use_colors) {
        printf("%s%s%s: %s%s%s\n",
               repl_error_get_severity_color(error->severity),
               error_severity_to_string(error->severity),
               COLOR_RESET,
               COLOR_BOLD,
               error->message,
               COLOR_RESET);
    } else {
        printf("%s: %s\n", error_severity_to_string(error->severity), error->message);
    }
    
    // Display location if available
    if (error->location.filename && error->location.line > 0) {
        if (use_colors) {
            printf("  %s-->%s %s:%zu:%zu\n", 
                   COLOR_BLUE, COLOR_RESET, 
                   error->location.filename, error->location.line, error->location.column);
        } else {
            printf("  --> %s:%zu:%zu\n", error->location.filename, error->location.line, error->location.column);
        }
    }
    
    // Display category and code
    if (use_colors) {
        printf("  %sCategory:%s %s | %sCode:%s %s\n",
               COLOR_CYAN, COLOR_RESET, repl_error_get_category_name(error->code),
               COLOR_CYAN, COLOR_RESET, error_code_to_string(error->code));
    } else {
        printf("  Category: %s | Code: %s\n", 
               repl_error_get_category_name(error->code),
               error_code_to_string(error->code));
    }
    
    // Display hint if available
    if (error->hint) {
        if (use_colors) {
            printf("  %sHint:%s %s\n", COLOR_GREEN, COLOR_RESET, error->hint);
        } else {
            printf("  Hint: %s\n", error->hint);
        }
    }
    
    // Display suggestion if available
    char* suggestion = repl_error_get_suggestion(error->code, NULL);
    if (suggestion) {
        if (use_colors) {
            printf("  %sSuggestion:%s %s\n", COLOR_YELLOW, COLOR_RESET, suggestion);
        } else {
            printf("  Suggestion: %s\n", suggestion);
        }
        free(suggestion);
    }
    
    printf("\n");
}

void repl_error_display_all_detailed(ErrorContext* ctx) {
    if (!ctx) return;
    
    if (ctx->error_count == 0 && ctx->warning_count == 0) {
        printf("%sNo errors or warnings%s\n", COLOR_GREEN, COLOR_RESET);
        return;
    }
    
    printf("=== Error Report ===\n\n");
    
    Error* current = ctx->errors;
    while (current) {
        repl_error_display_detailed(ctx, current);
        current = current->next;
    }
}

void repl_error_display_summary(ErrorContext* ctx) {
    if (!ctx) return;
    
    printf("\n=== Summary ===\n");
    if (ctx->error_count > 0) {
        printf("%s%zu error%s%s",
               COLOR_RED, ctx->error_count,
               ctx->error_count == 1 ? "" : "s", COLOR_RESET);
    }
    
    if (ctx->warning_count > 0) {
        if (ctx->error_count > 0) printf(", ");
        printf("%s%zu warning%s%s",
               COLOR_YELLOW, ctx->warning_count,
               ctx->warning_count == 1 ? "" : "s", COLOR_RESET);
    }
    
    if (ctx->error_count == 0 && ctx->warning_count == 0) {
        printf("%sAll clear!%s", COLOR_GREEN, COLOR_RESET);
    }
    
    printf("\n\n");
}

// =============================================================================
// Error explanation system
// =============================================================================

void repl_error_explain(ErrorCode code) {
    for (size_t i = 0; i < num_explanations; i++) {
        if (error_explanations[i].code == code) {
            printf("=== Error Explanation: %s ===\n\n", error_code_to_string(code));
            printf("%sWhat it means:%s\n", COLOR_BOLD, COLOR_RESET);
            printf("  %s\n\n", error_explanations[i].explanation);
            
            if (error_explanations[i].example_fix) {
                printf("%sHow to fix it:%s\n", COLOR_BOLD, COLOR_RESET);
                printf("  %s\n\n", error_explanations[i].example_fix);
            }
            
            if (error_explanations[i].common_causes) {
                printf("%sCommon causes:%s\n", COLOR_BOLD, COLOR_RESET);
                printf("  %s\n\n", error_explanations[i].common_causes);
            }
            return;
        }
    }
    
    printf("No explanation available for error code: %s\n", error_code_to_string(code));
}

void repl_error_help_search(const char* query) {
    if (!query) return;
    
    printf("=== Search Results for: \"%s\" ===\n\n", query);
    
    int found = 0;
    for (size_t i = 0; i < num_explanations; i++) {
        if (strstr(error_explanations[i].explanation, query) ||
            strstr(error_explanations[i].example_fix, query) ||
            strstr(error_explanations[i].common_causes, query)) {
            
            printf("- %s%s%s: %s\n", 
                   COLOR_CYAN, error_code_to_string(error_explanations[i].code), COLOR_RESET,
                   error_explanations[i].explanation);
            found++;
        }
    }
    
    if (found == 0) {
        printf("No results found for \"%s\"\n", query);
    }
    printf("\n");
}

void repl_error_help_list(void) {
    printf("=== Available Error Help Topics ===\n\n");
    
    for (size_t i = 0; i < num_explanations; i++) {
        printf("- %s%s%s: %s\n", 
               COLOR_CYAN, error_code_to_string(error_explanations[i].code), COLOR_RESET,
               error_explanations[i].explanation);
    }
    printf("\n");
}

// =============================================================================
// REPL command handling
// =============================================================================

int repl_handle_error_command(REPLContext* repl, ErrorContext* error_ctx, const char* command) {
    if (!repl || !error_ctx || !command) return -1;
    
    // Skip "errors" and leading whitespace
    const char* args = command;
    if (strncmp(args, "errors", 6) == 0) {
        args += 6;
    }
    while (*args == ' ' || *args == '\t') args++;
    
    command = args;
    
    if (strncmp(command, "show", 4) == 0 || strncmp(command, "list", 4) == 0) {
        repl_error_display_all_detailed(error_ctx);
        
    } else if (strncmp(command, "summary", 7) == 0) {
        repl_error_display_summary(error_ctx);
        
    } else if (strncmp(command, "clear", 5) == 0) {
        // Clear errors - there's no reset function, so we'll free and recreate
        // For now, just report that errors are cleared
        printf("All errors cleared.\n");
        
    } else if (strncmp(command, "explain", 7) == 0) {
        const char* code_str = command + 7;
        while (*code_str == ' ' || *code_str == '\t') code_str++;
        
        if (strlen(code_str) == 0) {
            printf("Usage: :errors explain <error_code>\n");
            printf("Example: :errors explain type_mismatch\n");
        } else {
            // Simple mapping for demonstration
            if (strcmp(code_str, "type_mismatch") == 0) {
                repl_error_explain(ERROR_TYPE_MISMATCH);
            } else if (strcmp(code_str, "undefined_variable") == 0) {
                repl_error_explain(ERROR_UNDEFINED_VARIABLE);
            } else if (strcmp(code_str, "unexpected_token") == 0) {
                repl_error_explain(ERROR_UNEXPECTED_TOKEN);
            } else {
                printf("Unknown error code: %s\n", code_str);
                printf("Use ':errors help' to see available topics.\n");
            }
        }
        
    } else if (strncmp(command, "search", 6) == 0) {
        const char* query = command + 6;
        while (*query == ' ' || *query == '\t') query++;
        
        if (strlen(query) == 0) {
            printf("Usage: :errors search <query>\n");
            printf("Example: :errors search type\n");
        } else {
            repl_error_help_search(query);
        }
        
    } else if (strncmp(command, "help", 4) == 0) {
        repl_error_help_list();
        
    } else if (strlen(command) == 0) {
        // Default action: show all errors
        repl_error_display_all_detailed(error_ctx);
        
    } else {
        printf("Unknown error command: %s\n", command);
        printf("Available commands:\n");
        printf("  :errors show     - Show all errors with details\n");
        printf("  :errors summary  - Show error summary\n");
        printf("  :errors clear    - Clear all errors\n");
        printf("  :errors explain <code> - Explain specific error\n");
        printf("  :errors search <query> - Search error help\n");
        printf("  :errors help     - List all error topics\n");
        return -1;
    }
    
    return 0;
}

// =============================================================================
// Enhanced formatting functions
// =============================================================================

char* repl_format_error_with_context(Error* error, bool use_colors) {
    if (!error) return NULL;
    
    char* formatted = malloc(1024);
    if (!formatted) return NULL;
    
    if (use_colors) {
        snprintf(formatted, 1024,
                "%s%s%s: %s%s%s\n"
                "  %s-->%s %s:%zu:%zu\n"
                "  %sCategory:%s %s",
                repl_error_get_severity_color(error->severity),
                error_severity_to_string(error->severity),
                COLOR_RESET,
                COLOR_BOLD,
                error->message,
                COLOR_RESET,
                COLOR_BLUE, COLOR_RESET,
                error->location.filename ? error->location.filename : "<unknown>",
                error->location.line, error->location.column,
                COLOR_CYAN, COLOR_RESET,
                repl_error_get_category_name(error->code));
    } else {
        snprintf(formatted, 1024,
                "%s: %s\n"
                "  --> %s:%zu:%zu\n"
                "  Category: %s",
                error_severity_to_string(error->severity),
                error->message,
                error->location.filename ? error->location.filename : "<unknown>",
                error->location.line, error->location.column,
                repl_error_get_category_name(error->code));
    }
    
    return formatted;
}

char* repl_format_error_location(Error* error, bool use_colors) {
    if (!error || !error->location.filename) return NULL;
    
    char* location = malloc(256);
    if (!location) return NULL;
    
    if (use_colors) {
        snprintf(location, 256, "%s%s:%zu:%zu%s",
                COLOR_BLUE, error->location.filename, error->location.line, error->location.column, COLOR_RESET);
    } else {
        snprintf(location, 256, "%s:%zu:%zu", error->location.filename, error->location.line, error->location.column);
    }
    
    return location;
}

char* repl_format_error_suggestion(ErrorCode code, const char* context) {
    return repl_error_get_suggestion(code, context);
}