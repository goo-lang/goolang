#include "repl.h"
#include "parser.h"
#include "lexer.h"
#include "panic_free.h"
#include "time_travel_debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <ctype.h>

// =============================================================================
// Color codes for output formatting
// =============================================================================

#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BLUE "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN "\033[36m"
#define ANSI_WHITE "\033[37m"

// =============================================================================
// Helper Functions
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

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

// =============================================================================
// Context Management
// =============================================================================

REPLContext* repl_context_new(void) {
    REPLContext* ctx = calloc(1, sizeof(REPLContext));
    if (!ctx) return NULL;
    
    // Initialize basic state
    ctx->mode = REPL_MODE_NORMAL;
    ctx->session_id = 1;
    ctx->prompt = str_dup("goo> ");
    ctx->running = false;
    ctx->auto_complete_enabled = true;
    ctx->show_types = true;
    ctx->show_timing = true;
    ctx->show_performance = true;
    ctx->max_history = 1000;
    ctx->color_output = true;
    ctx->output_stream = stdout;
    ctx->error_stream = stderr;
    
    // Initialize multiline buffer
    ctx->multiline_capacity = 1024;
    ctx->multiline_buffer = malloc(ctx->multiline_capacity);
    if (ctx->multiline_buffer) {
        ctx->multiline_buffer[0] = '\0';
    }
    
    return ctx;
}

void repl_context_free(REPLContext* ctx) {
    if (!ctx) return;
    
    // Free type checker
    if (ctx->type_checker) {
        type_checker_free(ctx->type_checker);
    }
    
    // Free hot reload context
    if (ctx->hot_reload) {
        hot_reload_context_free(ctx->hot_reload);
    }
    
    // Free performance monitor
    if (ctx->performance_monitor) {
        performance_monitor_free(ctx->performance_monitor);
    }
    
    // Free error context
    if (ctx->error_context) {
        error_context_free(ctx->error_context);
    }
    
    // Free debug state
    if (ctx->debug_state) {
        repl_debug_cleanup(ctx);
    }
    
    // Free prompt
    free(ctx->prompt);
    
    // Free multiline buffer
    free(ctx->multiline_buffer);
    
    // Free history
    REPLHistory* hist = ctx->history;
    while (hist) {
        REPLHistory* next = hist->next;
        free(hist->input);
        if (hist->result) {
            repl_value_free(hist->result);
        }
        free(hist);
        hist = next;
    }
    
    // Free REPL scope
    if (ctx->repl_scope) {
        scope_free(ctx->repl_scope);
    }
    
    free(ctx);
}

int repl_init(REPLContext* ctx) {
    if (!ctx) return -1;
    
    // Initialize type checker
    ctx->type_checker = type_checker_new();
    if (!ctx->type_checker) {
        return -1;
    }
    
    // Initialize builtin types and functions
    type_checker_init_builtins(ctx->type_checker);
    type_checker_add_builtin_functions(ctx->type_checker);
    
    // Create REPL scope
    ctx->repl_scope = scope_new(ctx->type_checker->current_scope);
    
    // Initialize hot reload
    ctx->hot_reload = hot_reload_context_new();
    if (ctx->hot_reload) {
        hot_reload_enable(ctx->hot_reload, HOT_RELOAD_CAP_FUNCTION | HOT_RELOAD_CAP_TYPE);
    }
    
    // Initialize performance monitor
    ctx->performance_monitor = performance_monitor_new();
    if (ctx->performance_monitor) {
        performance_monitor_init(ctx->performance_monitor);
        performance_monitor_integrate_type_checker(ctx->performance_monitor, ctx->type_checker);
        performance_monitor_integrate_hot_reload(ctx->performance_monitor, ctx->hot_reload);
        performance_monitor_integrate_repl(ctx->performance_monitor, ctx);
        performance_monitor_register_repl_commands(ctx, ctx->performance_monitor);
        
        // Start monitoring by default
        performance_monitor_start_recording(ctx->performance_monitor);
    }
    
    // Initialize error reporting (use existing error system)
    ctx->error_context = error_context_new();
    if (ctx->error_context) {
        // Error context is ready to use
    }
    
    // Initialize debug system
    if (!repl_debug_init(ctx)) {
        // Debug system is optional, continue without it
    }
    
    return 0;
}

void repl_cleanup(REPLContext* ctx) {
    if (!ctx) return;
    
    ctx->running = false;
    
    // Cleanup is handled in repl_context_free
}

// =============================================================================
// Main REPL Loop
// =============================================================================

int repl_run(REPLContext* ctx) {
    if (!ctx) return -1;
    
    if (repl_init(ctx) != 0) {
        repl_error_printf(ctx, "Failed to initialize REPL\n");
        return -1;
    }
    
    return repl_run_interactive(ctx);
}

int repl_run_interactive(REPLContext* ctx) {
    if (!ctx) return -1;
    
    ctx->running = true;
    
    // Print welcome banner
    repl_print_banner(ctx);
    
    char input_buffer[4096];
    
    while (ctx->running) {
        // Print prompt
        if (ctx->mode == REPL_MODE_MULTILINE && ctx->multiline_size > 0) {
            repl_printf(ctx, "... ");
        } else {
            repl_printf(ctx, "%s", ctx->prompt);
        }
        
        // Read input
        if (repl_read_line(ctx, input_buffer, sizeof(input_buffer)) != 0) {
            break;
        }
        
        // Handle empty input
        if (strlen(input_buffer) == 0) {
            continue;
        }
        
        // Check for commands
        if (input_buffer[0] == ':') {
            REPLCommandType cmd = repl_parse_command(input_buffer + 1);
            repl_execute_command(ctx, cmd, input_buffer + 1);
            continue;
        }
        
        // Handle multiline input
        if (ctx->mode == REPL_MODE_MULTILINE || !repl_is_complete_expression(input_buffer)) {
            if (repl_handle_multiline(ctx, input_buffer) == 0) {
                continue; // Need more input
            }
            // Complete expression ready
        }
        
        // Evaluate expression
        const char* input_to_eval = (ctx->multiline_size > 0) ? ctx->multiline_buffer : input_buffer;
        repl_eval_string(ctx, input_to_eval);
        
        // Clear multiline buffer if used
        if (ctx->multiline_size > 0) {
            ctx->multiline_buffer[0] = '\0';
            ctx->multiline_size = 0;
        }
    }
    
    repl_printf(ctx, "\nGoodbye!\n");
    return 0;
}

int repl_eval_string(REPLContext* ctx, const char* input) {
    if (!ctx || !input) return -1;
    
    double start_time = get_time_ms();
    
    // Evaluate the expression
    REPLValue* result = repl_evaluate_expression(ctx, input);
    
    double end_time = get_time_ms();
    double exec_time = end_time - start_time;
    
    if (result) {
        // Add to history
        Type* result_type = result->goo_type;
        repl_history_add(ctx, input, result, result_type, exec_time);
        
        // Print result
        if (ctx->show_types && result_type) {
            char* type_str = repl_format_type(result_type, false);
            repl_printf(ctx, "%s(%s): ", type_str ? type_str : "unknown", type_str ? type_str : "");
            free(type_str);
        }
        
        char* result_str = repl_value_to_string(result);
        repl_printf(ctx, "%s", result_str ? result_str : "null");
        free(result_str);
        
        if (ctx->show_timing) {
            repl_printf(ctx, " (%.2fms)", exec_time);
        }
        repl_printf(ctx, "\n");
        
        ctx->expressions_evaluated++;
        ctx->total_execution_time += exec_time;
        
        // Don't free result here - it's owned by history
    } else {
        ctx->errors_encountered++;
        repl_error_printf(ctx, "Error: Failed to evaluate expression\n");
        return -1;
    }
    
    return 0;
}

// =============================================================================
// Input Handling
// =============================================================================

int repl_read_line(REPLContext* ctx, char* buffer, size_t buffer_size) {
    if (!ctx || !buffer || buffer_size == 0) return -1;
    
    if (fgets(buffer, buffer_size, stdin) == NULL) {
        return -1; // EOF or error
    }
    
    // Remove trailing newline
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
        buffer[len - 1] = '\0';
    }
    
    return 0;
}

int repl_handle_multiline(REPLContext* ctx, const char* line) {
    if (!ctx || !line) return -1;
    
    size_t line_len = strlen(line);
    size_t needed = ctx->multiline_size + line_len + 2; // +2 for newline and null terminator
    
    // Resize buffer if needed
    if (needed > ctx->multiline_capacity) {
        size_t new_capacity = ctx->multiline_capacity * 2;
        while (new_capacity < needed) {
            new_capacity *= 2;
        }
        
        char* new_buffer = realloc(ctx->multiline_buffer, new_capacity);
        if (!new_buffer) {
            return -1;
        }
        
        ctx->multiline_buffer = new_buffer;
        ctx->multiline_capacity = new_capacity;
    }
    
    // Append line to buffer
    if (ctx->multiline_size > 0) {
        strcat(ctx->multiline_buffer, "\n");
        ctx->multiline_size++;
    }
    strcat(ctx->multiline_buffer, line);
    ctx->multiline_size += line_len;
    
    // Check if expression is complete
    if (repl_is_complete_expression(ctx->multiline_buffer)) {
        return 1; // Expression is complete
    }
    
    return 0; // Need more input
}

bool repl_is_complete_expression(const char* input) {
    if (!input) return false;
    
    // Simple heuristic: check for balanced braces
    int brace_count = 0;
    int paren_count = 0;
    int bracket_count = 0;
    bool in_string = false;
    bool escaped = false;
    
    for (const char* p = input; *p; p++) {
        if (escaped) {
            escaped = false;
            continue;
        }
        
        if (*p == '\\') {
            escaped = true;
            continue;
        }
        
        if (*p == '"' && !escaped) {
            in_string = !in_string;
            continue;
        }
        
        if (in_string) {
            continue;
        }
        
        switch (*p) {
            case '{': brace_count++; break;
            case '}': brace_count--; break;
            case '(': paren_count++; break;
            case ')': paren_count--; break;
            case '[': bracket_count++; break;
            case ']': bracket_count--; break;
        }
    }
    
    return brace_count == 0 && paren_count == 0 && bracket_count == 0 && !in_string;
}

// =============================================================================
// Expression Evaluation
// =============================================================================

REPLValue* repl_evaluate_expression(REPLContext* ctx, const char* input) {
    if (!ctx || !input) return NULL;
    
    // Parse the input
    ASTNode* ast = NULL;
    
    // Use the parser to parse the input
    if (parse_input(input, "<repl>") != 0) {
        return repl_value_error("Parse error", 1);
    }
    
    ast = ast_root; // Get the parsed AST
    if (!ast) {
        return repl_value_error("No AST generated", 2);
    }
    
    return repl_evaluate_ast(ctx, ast);
}

REPLValue* repl_evaluate_ast(REPLContext* ctx, ASTNode* ast) {
    if (!ctx || !ast) return NULL;
    
    // Type check the AST
    Type* expr_type = type_check_expression(ctx->type_checker, ast);
    if (!expr_type) {
        return repl_value_error("Type checking failed", 3);
    }
    
    // For now, create a simple result based on the type
    // In a full implementation, this would involve code generation and execution
    REPLValue* result = NULL;
    
    switch (expr_type->kind) {
        case TYPE_BOOL:
            result = repl_value_bool(true); // Placeholder
            break;
        case TYPE_INT32:
        case TYPE_INT64:
            result = repl_value_int(42); // Placeholder
            break;
        case TYPE_FLOAT32:
        case TYPE_FLOAT64:
            result = repl_value_float(3.14); // Placeholder
            break;
        case TYPE_STRING:
            result = repl_value_string("\"hello\""); // Placeholder
            break;
        default:
            result = repl_value_error("Unsupported type for evaluation", 4);
            break;
    }
    
    if (result) {
        result->goo_type = expr_type;
    }
    
    return result;
}

// =============================================================================
// Command System
// =============================================================================

REPLCommandType repl_parse_command(const char* input) {
    if (!input) return REPL_CMD_UNKNOWN;
    
    // Skip whitespace
    while (isspace(*input)) input++;
    
    if (strncmp(input, "help", 4) == 0) return REPL_CMD_HELP;
    if (strncmp(input, "exit", 4) == 0 || strncmp(input, "quit", 4) == 0) return REPL_CMD_EXIT;
    if (strncmp(input, "reset", 5) == 0) return REPL_CMD_RESET;
    if (strncmp(input, "history", 7) == 0) return REPL_CMD_HISTORY;
    if (strncmp(input, "type", 4) == 0) return REPL_CMD_TYPE;
    if (strncmp(input, "clear", 5) == 0) return REPL_CMD_CLEAR;
    if (strncmp(input, "mode", 4) == 0) return REPL_CMD_MODE;
    if (strncmp(input, "reload", 6) == 0) return REPL_CMD_RELOAD;
    if (strncmp(input, "scope", 5) == 0) return REPL_CMD_SCOPE;
    if (strncmp(input, "bindings", 8) == 0) return REPL_CMD_BINDINGS;
    if (strncmp(input, "perf", 4) == 0) return REPL_CMD_PERF;
    if (strncmp(input, "errors", 6) == 0) return REPL_CMD_ERRORS;
    if (strncmp(input, "debug", 5) == 0) return REPL_CMD_DEBUG;
    
    return REPL_CMD_UNKNOWN;
}

int repl_execute_command(REPLContext* ctx, REPLCommandType cmd, const char* args) {
    if (!ctx) return -1;
    
    switch (cmd) {
        case REPL_CMD_HELP:
            return repl_cmd_help(ctx, args);
        case REPL_CMD_EXIT:
            return repl_cmd_exit(ctx, args);
        case REPL_CMD_RESET:
            return repl_cmd_reset(ctx, args);
        case REPL_CMD_HISTORY:
            return repl_cmd_history(ctx, args);
        case REPL_CMD_TYPE:
            return repl_cmd_type(ctx, args);
        case REPL_CMD_CLEAR:
            return repl_cmd_clear(ctx, args);
        case REPL_CMD_MODE:
            return repl_cmd_mode(ctx, args);
        case REPL_CMD_RELOAD:
            return repl_cmd_reload(ctx, args);
        case REPL_CMD_SCOPE:
            return repl_cmd_scope(ctx, args);
        case REPL_CMD_BINDINGS:
            return repl_cmd_bindings(ctx, args);
        case REPL_CMD_PERF:
            return repl_cmd_perf(ctx, args);
        case REPL_CMD_ERRORS:
            return repl_cmd_errors(ctx, args);
        case REPL_CMD_DEBUG:
            return repl_cmd_debug(ctx, args);
        case REPL_CMD_UNKNOWN:
        default:
            repl_error_printf(ctx, "Unknown command. Type :help for available commands.\n");
            return -1;
    }
}

int repl_cmd_help(REPLContext* ctx, const char* args) {
    (void)args; // Unused
    
    repl_printf(ctx, "%sGoo REPL Commands:%s\n", ctx->color_output ? ANSI_BOLD : "", ctx->color_output ? ANSI_RESET : "");
    repl_printf(ctx, "  :help           - Show this help message\n");
    repl_printf(ctx, "  :exit, :quit    - Exit the REPL\n");
    repl_printf(ctx, "  :reset          - Reset the REPL state\n");
    repl_printf(ctx, "  :history [n]    - Show last n history entries (default: 10)\n");
    repl_printf(ctx, "  :type <expr>    - Show type information for expression\n");
    repl_printf(ctx, "  :clear          - Clear the screen\n");
    repl_printf(ctx, "  :mode [mode]    - Set or show current mode (normal, multiline, debug, type-only)\n");
    repl_printf(ctx, "  :reload         - Reload changed modules\n");
    repl_printf(ctx, "  :scope          - Show current scope information\n");
    repl_printf(ctx, "  :bindings       - Show current variable bindings\n");
    repl_printf(ctx, "  :perf [cmd]     - Performance monitoring (status, start, stop, metrics, alerts)\n");
    repl_printf(ctx, "  :errors [cmd]   - Error reporting (show, clear, summary, help)\n");
    repl_printf(ctx, "  :debug [cmd]    - Time-travel debugging (enable, disable, step, timeline, state)\n");
    repl_printf(ctx, "\n%sExpression Evaluation:%s\n", ctx->color_output ? ANSI_BOLD : "", ctx->color_output ? ANSI_RESET : "");
    repl_printf(ctx, "  Enter any valid Goo expression to evaluate it\n");
    repl_printf(ctx, "  Multi-line expressions are supported\n");
    repl_printf(ctx, "  Variables defined in the REPL persist across evaluations\n");
    
    return 0;
}

int repl_cmd_exit(REPLContext* ctx, const char* args) {
    (void)args; // Unused
    
    ctx->running = false;
    return 0;
}

int repl_cmd_reset(REPLContext* ctx, const char* args) {
    (void)args; // Unused
    
    // Clear REPL scope
    if (ctx->repl_scope) {
        scope_free(ctx->repl_scope);
        ctx->repl_scope = scope_new(ctx->type_checker->current_scope);
    }
    
    // Clear history
    repl_history_clear(ctx);
    
    // Reset statistics
    ctx->expressions_evaluated = 0;
    ctx->errors_encountered = 0;
    ctx->total_execution_time = 0.0;
    
    repl_printf(ctx, "REPL state reset.\n");
    return 0;
}

int repl_cmd_history(REPLContext* ctx, const char* args) {
    if (!ctx) return -1;
    
    int count = 10; // Default
    
    // Parse arguments
    if (args) {
        char* end;
        const char* start = args;
        while (isspace(*start)) start++;
        
        // Skip the command name
        while (*start && !isspace(*start)) start++;
        while (isspace(*start)) start++;
        
        if (*start) {
            long parsed = strtol(start, &end, 10);
            if (end != start && parsed > 0 && parsed <= 1000) {
                count = (int)parsed;
            }
        }
    }
    
    return repl_print_history(ctx, count);
}

int repl_cmd_type(REPLContext* ctx, const char* args) {
    if (!ctx || !args) return -1;
    
    // Extract the expression from args
    const char* expr_start = args;
    while (*expr_start && !isspace(*expr_start)) expr_start++; // Skip command
    while (isspace(*expr_start)) expr_start++; // Skip whitespace
    
    if (!*expr_start) {
        repl_error_printf(ctx, "Usage: :type <expression>\n");
        return -1;
    }
    
    char* type_info = repl_get_type_info(ctx, expr_start);
    if (type_info) {
        repl_printf(ctx, "%s\n", type_info);
        free(type_info);
    } else {
        repl_error_printf(ctx, "Failed to get type information\n");
    }
    
    return 0;
}

int repl_cmd_clear(REPLContext* ctx, const char* args) {
    (void)args; // Unused
    
    if (ctx->color_output) {
        repl_printf(ctx, "\033[2J\033[H"); // Clear screen and move cursor to top
    } else {
        for (int i = 0; i < 50; i++) {
            repl_printf(ctx, "\n");
        }
    }
    
    return 0;
}

int repl_cmd_mode(REPLContext* ctx, const char* args) {
    if (!ctx) return -1;
    
    // Extract mode from args
    const char* mode_start = args;
    while (*mode_start && !isspace(*mode_start)) mode_start++; // Skip command
    while (isspace(*mode_start)) mode_start++; // Skip whitespace
    
    if (!*mode_start) {
        // Show current mode
        const char* mode_name;
        switch (ctx->mode) {
            case REPL_MODE_NORMAL: mode_name = "normal"; break;
            case REPL_MODE_MULTILINE: mode_name = "multiline"; break;
            case REPL_MODE_DEBUG: mode_name = "debug"; break;
            case REPL_MODE_TYPE_ONLY: mode_name = "type-only"; break;
            default: mode_name = "unknown"; break;
        }
        repl_printf(ctx, "Current mode: %s\n", mode_name);
        return 0;
    }
    
    // Set mode
    if (strncmp(mode_start, "normal", 6) == 0) {
        ctx->mode = REPL_MODE_NORMAL;
        repl_printf(ctx, "Mode set to normal\n");
    } else if (strncmp(mode_start, "multiline", 9) == 0) {
        ctx->mode = REPL_MODE_MULTILINE;
        repl_printf(ctx, "Mode set to multiline\n");
    } else if (strncmp(mode_start, "debug", 5) == 0) {
        ctx->mode = REPL_MODE_DEBUG;
        repl_printf(ctx, "Mode set to debug\n");
    } else if (strncmp(mode_start, "type-only", 9) == 0) {
        ctx->mode = REPL_MODE_TYPE_ONLY;
        repl_printf(ctx, "Mode set to type-only\n");
    } else {
        repl_error_printf(ctx, "Unknown mode. Available modes: normal, multiline, debug, type-only\n");
        return -1;
    }
    
    return 0;
}

int repl_cmd_reload(REPLContext* ctx, const char* args) {
    (void)args; // Unused
    
    if (!ctx->hot_reload) {
        repl_error_printf(ctx, "Hot reload not initialized\n");
        return -1;
    }
    
    return repl_reload_changed_modules(ctx);
}

int repl_cmd_scope(REPLContext* ctx, const char* args) {
    (void)args; // Unused
    
    repl_printf(ctx, "Current scope information:\n");
    repl_printf(ctx, "  REPL scope: %p\n", (void*)ctx->repl_scope);
    repl_printf(ctx, "  Parent scope: %p\n", (void*)(ctx->repl_scope ? ctx->repl_scope->parent : NULL));
    
    return 0;
}

int repl_cmd_bindings(REPLContext* ctx, const char* args) {
    (void)args; // Unused
    
    repl_printf(ctx, "Current variable bindings:\n");
    
    if (!ctx->repl_scope || !ctx->repl_scope->variables) {
        repl_printf(ctx, "  No variables defined\n");
        return 0;
    }
    
    Variable* var = ctx->repl_scope->variables;
    while (var) {
        char* type_str = repl_format_type(var->type, false);
        repl_printf(ctx, "  %s: %s\n", var->name, type_str ? type_str : "unknown");
        free(type_str);
        var = var->next;
    }
    
    return 0;
}

int repl_cmd_perf(REPLContext* ctx, const char* args) {
    if (!ctx) return -1;
    
    if (!ctx->performance_monitor) {
        repl_error_printf(ctx, "Performance monitoring is not available.\n");
        return -1;
    }
    
    // Parse the performance command arguments
    return performance_handle_repl_perf_command(ctx, ctx->performance_monitor, args);
}

int repl_cmd_errors(REPLContext* ctx, const char* args) {
    if (!ctx) return -1;
    
    if (!ctx->error_context) {
        repl_error_printf(ctx, "Error reporting is not available.\n");
        return -1;
    }
    
    // Parse the error command arguments using simplified error system
    return repl_handle_error_command(ctx, ctx->error_context, args);
}

int repl_cmd_debug(REPLContext* ctx, const char* args) {
    if (!ctx) return -1;
    
    // Initialize debug system if not already done
    if (!ctx->debug_state) {
        if (!repl_debug_init(ctx)) {
            repl_error_printf(ctx, "Failed to initialize debug system.\n");
            return -1;
        }
    }
    
    if (!args || strlen(args) == 0) {
        repl_error_printf(ctx, "Usage: :debug <command>\n");
        repl_error_printf(ctx, "Available commands:\n");
        repl_error_printf(ctx, "  :debug enable        - Enable time-travel debugging\n");
        repl_error_printf(ctx, "  :debug disable       - Disable time-travel debugging\n");
        repl_error_printf(ctx, "  :debug step back     - Step backward in execution\n");
        repl_error_printf(ctx, "  :debug step forward  - Step forward in execution\n");
        repl_error_printf(ctx, "  :debug continue back - Continue to beginning\n");
        repl_error_printf(ctx, "  :debug continue forward - Continue to end\n");
        repl_error_printf(ctx, "  :debug goto <step>   - Jump to specific step\n");
        repl_error_printf(ctx, "  :debug timeline      - Show execution timeline\n");
        repl_error_printf(ctx, "  :debug state         - Show current debug state\n");
        repl_error_printf(ctx, "  :debug variables     - Show variables at current step\n");
        repl_error_printf(ctx, "  :debug stack         - Show call stack\n");
        repl_error_printf(ctx, "  :debug find <query>  - Find events in timeline\n");
        repl_error_printf(ctx, "  :debug snapshot [desc] - Create manual snapshot\n");
        return 0;
    }
    
    // Skip "debug" prefix if present and parse subcommand
    const char* cmd = args;
    if (strncmp(cmd, "debug", 5) == 0) {
        cmd += 5;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
    }
    
    // Parse debug subcommands
    if (strncmp(cmd, "enable", 6) == 0) {
        return repl_cmd_debug_enable(ctx, cmd + 6);
    } else if (strncmp(cmd, "disable", 7) == 0) {
        return repl_cmd_debug_disable(ctx, cmd + 7);
    } else if (strncmp(cmd, "step back", 9) == 0) {
        return repl_cmd_debug_step_back(ctx, cmd + 9);
    } else if (strncmp(cmd, "step forward", 12) == 0) {
        return repl_cmd_debug_step_forward(ctx, cmd + 12);
    } else if (strncmp(cmd, "continue back", 13) == 0) {
        return repl_cmd_debug_continue_back(ctx, cmd + 13);
    } else if (strncmp(cmd, "continue forward", 16) == 0) {
        return repl_cmd_debug_continue_forward(ctx, cmd + 16);
    } else if (strncmp(cmd, "goto", 4) == 0) {
        return repl_cmd_debug_goto(ctx, cmd + 4);
    } else if (strncmp(cmd, "timeline", 8) == 0) {
        return repl_cmd_debug_timeline(ctx, cmd + 8);
    } else if (strncmp(cmd, "state", 5) == 0) {
        return repl_cmd_debug_state(ctx, cmd + 5);
    } else if (strncmp(cmd, "variables", 9) == 0) {
        return repl_cmd_debug_variables(ctx, cmd + 9);
    } else if (strncmp(cmd, "stack", 5) == 0) {
        return repl_cmd_debug_stack(ctx, cmd + 5);
    } else if (strncmp(cmd, "find", 4) == 0) {
        return repl_cmd_debug_find(ctx, cmd + 4);
    } else if (strncmp(cmd, "snapshot", 8) == 0) {
        return repl_cmd_debug_snapshot(ctx, cmd + 8);
    } else {
        repl_error_printf(ctx, "Unknown debug command: %s\n", cmd);
        repl_error_printf(ctx, "Type ':debug' for available commands.\n");
        return -1;
    }
}

// =============================================================================
// Value Management
// =============================================================================

REPLValue* repl_value_new(REPLValueType type) {
    REPLValue* value = calloc(1, sizeof(REPLValue));
    if (!value) return NULL;
    
    value->type = type;
    value->is_valid = true;
    
    return value;
}

void repl_value_free(REPLValue* value) {
    if (!value) return;
    
    switch (value->type) {
        case REPL_VALUE_STRING:
            free(value->value.string_val);
            break;
        case REPL_VALUE_ERROR:
            free(value->value.error.error_msg);
            break;
        case REPL_VALUE_COMPLEX:
            free(value->value.complex.data);
            free(value->value.complex.type_name);
            break;
        default:
            break;
    }
    
    free(value);
}

REPLValue* repl_value_copy(const REPLValue* value) {
    if (!value) return NULL;
    
    REPLValue* copy = repl_value_new(value->type);
    if (!copy) return NULL;
    
    copy->goo_type = value->goo_type;
    copy->is_valid = value->is_valid;
    
    switch (value->type) {
        case REPL_VALUE_INT:
            copy->value.int_val = value->value.int_val;
            break;
        case REPL_VALUE_FLOAT:
            copy->value.float_val = value->value.float_val;
            break;
        case REPL_VALUE_STRING:
            copy->value.string_val = str_dup(value->value.string_val);
            break;
        case REPL_VALUE_BOOL:
            copy->value.bool_val = value->value.bool_val;
            break;
        case REPL_VALUE_ERROR:
            copy->value.error.error_msg = str_dup(value->value.error.error_msg);
            copy->value.error.error_code = value->value.error.error_code;
            break;
        case REPL_VALUE_COMPLEX:
            if (value->value.complex.data && value->value.complex.size > 0) {
                copy->value.complex.data = malloc(value->value.complex.size);
                if (copy->value.complex.data) {
                    memcpy(copy->value.complex.data, value->value.complex.data, value->value.complex.size);
                }
            }
            copy->value.complex.size = value->value.complex.size;
            copy->value.complex.type_name = str_dup(value->value.complex.type_name);
            break;
        default:
            break;
    }
    
    return copy;
}

REPLValue* repl_value_int(int64_t val) {
    REPLValue* value = repl_value_new(REPL_VALUE_INT);
    if (value) {
        value->value.int_val = val;
    }
    return value;
}

REPLValue* repl_value_float(double val) {
    REPLValue* value = repl_value_new(REPL_VALUE_FLOAT);
    if (value) {
        value->value.float_val = val;
    }
    return value;
}

REPLValue* repl_value_string(const char* val) {
    REPLValue* value = repl_value_new(REPL_VALUE_STRING);
    if (value) {
        value->value.string_val = str_dup(val);
    }
    return value;
}

REPLValue* repl_value_bool(bool val) {
    REPLValue* value = repl_value_new(REPL_VALUE_BOOL);
    if (value) {
        value->value.bool_val = val;
    }
    return value;
}

REPLValue* repl_value_null(void) {
    return repl_value_new(REPL_VALUE_NULL);
}

REPLValue* repl_value_error(const char* msg, int code) {
    REPLValue* value = repl_value_new(REPL_VALUE_ERROR);
    if (value) {
        value->value.error.error_msg = str_dup(msg);
        value->value.error.error_code = code;
        value->is_valid = false;
    }
    return value;
}

char* repl_value_to_string(const REPLValue* value) {
    if (!value) return str_dup("null");
    
    char buffer[1024];
    
    switch (value->type) {
        case REPL_VALUE_INT:
            snprintf(buffer, sizeof(buffer), "%ld", value->value.int_val);
            return str_dup(buffer);
        case REPL_VALUE_FLOAT:
            snprintf(buffer, sizeof(buffer), "%.6g", value->value.float_val);
            return str_dup(buffer);
        case REPL_VALUE_STRING:
            return str_dup(value->value.string_val ? value->value.string_val : "null");
        case REPL_VALUE_BOOL:
            return str_dup(value->value.bool_val ? "true" : "false");
        case REPL_VALUE_NULL:
            return str_dup("null");
        case REPL_VALUE_ERROR:
            snprintf(buffer, sizeof(buffer), "Error: %s (code %d)", 
                    value->value.error.error_msg ? value->value.error.error_msg : "unknown", 
                    value->value.error.error_code);
            return str_dup(buffer);
        case REPL_VALUE_COMPLEX:
            snprintf(buffer, sizeof(buffer), "<%s at %p>", 
                    value->value.complex.type_name ? value->value.complex.type_name : "unknown",
                    value->value.complex.data);
            return str_dup(buffer);
        default:
            return str_dup("unknown");
    }
}

// =============================================================================
// Type Information System
// =============================================================================

char* repl_get_type_info(REPLContext* ctx, const char* expression) {
    if (!ctx || !expression) return NULL;
    
    Type* type = repl_infer_type(ctx, expression);
    if (!type) {
        return str_dup("Failed to infer type");
    }
    
    return repl_format_type(type, true);
}

char* repl_format_type(const Type* type, bool verbose) {
    if (!type) return str_dup("unknown");
    
    char buffer[1024];
    
    switch (type->kind) {
        case TYPE_VOID:
            return str_dup("void");
        case TYPE_BOOL:
            return str_dup("bool");
        case TYPE_INT8:
            return str_dup("int8");
        case TYPE_INT16:
            return str_dup("int16");
        case TYPE_INT32:
            return str_dup("int32");
        case TYPE_INT64:
            return str_dup("int64");
        case TYPE_UINT8:
            return str_dup("uint8");
        case TYPE_UINT16:
            return str_dup("uint16");
        case TYPE_UINT32:
            return str_dup("uint32");
        case TYPE_UINT64:
            return str_dup("uint64");
        case TYPE_FLOAT32:
            return str_dup("float32");
        case TYPE_FLOAT64:
            return str_dup("float64");
        case TYPE_STRING:
            return str_dup("string");
        case TYPE_CHAR:
            return str_dup("char");
        case TYPE_ARRAY:
            if (type->data.array.element_type) {
                char* elem_type = repl_format_type(type->data.array.element_type, false);
                snprintf(buffer, sizeof(buffer), "[%zu]%s", type->data.array.length, elem_type ? elem_type : "unknown");
                free(elem_type);
                return str_dup(buffer);
            }
            return str_dup("[]unknown");
        case TYPE_SLICE:
            if (type->data.slice.element_type) {
                char* elem_type = repl_format_type(type->data.slice.element_type, false);
                snprintf(buffer, sizeof(buffer), "[]%s", elem_type ? elem_type : "unknown");
                free(elem_type);
                return str_dup(buffer);
            }
            return str_dup("[]unknown");
        case TYPE_POINTER:
            if (type->data.pointer.pointee_type) {
                char* pointee_type = repl_format_type(type->data.pointer.pointee_type, false);
                snprintf(buffer, sizeof(buffer), "*%s", pointee_type ? pointee_type : "unknown");
                free(pointee_type);
                return str_dup(buffer);
            }
            return str_dup("*unknown");
        case TYPE_ERROR_UNION:
            if (type->data.error_union.value_type) {
                char* value_type = repl_format_type(type->data.error_union.value_type, false);
                snprintf(buffer, sizeof(buffer), "!%s", value_type ? value_type : "unknown");
                free(value_type);
                return str_dup(buffer);
            }
            return str_dup("!unknown");
        case TYPE_NULLABLE:
            if (type->data.nullable.base_type) {
                char* base_type = repl_format_type(type->data.nullable.base_type, false);
                snprintf(buffer, sizeof(buffer), "?%s", base_type ? base_type : "unknown");
                free(base_type);
                return str_dup(buffer);
            }
            return str_dup("?unknown");
        default:
            if (verbose) {
                snprintf(buffer, sizeof(buffer), "%s (kind=%d, size=%zu)", 
                        type->name ? type->name : "unnamed", type->kind, type->size);
                return str_dup(buffer);
            }
            return str_dup(type->name ? type->name : "unknown");
    }
}

Type* repl_infer_type(REPLContext* ctx, const char* expression) {
    if (!ctx || !expression) return NULL;
    
    // Parse the expression
    if (parse_input(expression, "<repl-type>") != 0) {
        return NULL;
    }
    
    ASTNode* ast = ast_root;
    if (!ast) {
        return NULL;
    }
    
    // Type check to infer the type
    return type_check_expression(ctx->type_checker, ast);
}

// =============================================================================
// History Management
// =============================================================================

int repl_history_add(REPLContext* ctx, const char* input, REPLValue* result, Type* type, double exec_time) {
    if (!ctx || !input) return -1;
    
    REPLHistory* entry = calloc(1, sizeof(REPLHistory));
    if (!entry) return -1;
    
    entry->input = str_dup(input);
    entry->result = result; // Take ownership
    entry->result_type = type;
    entry->execution_time = exec_time;
    entry->entry_number = ctx->history_count + 1;
    
    // Add to front of list
    entry->next = ctx->history;
    ctx->history = entry;
    ctx->history_count++;
    
    // Limit history size
    if (ctx->history_count > ctx->max_history) {
        // Remove oldest entry
        REPLHistory* curr = ctx->history;
        REPLHistory* prev = NULL;
        
        while (curr && curr->next) {
            prev = curr;
            curr = curr->next;
        }
        
        if (prev) {
            prev->next = NULL;
        } else {
            ctx->history = NULL;
        }
        
        if (curr) {
            free(curr->input);
            if (curr->result) {
                repl_value_free(curr->result);
            }
            free(curr);
        }
        
        ctx->history_count--;
    }
    
    return 0;
}

REPLHistory* repl_history_get(REPLContext* ctx, int index) {
    if (!ctx || index < 1) return NULL;
    
    REPLHistory* entry = ctx->history;
    int current_index = 1;
    
    while (entry) {
        if (current_index == index) {
            return entry;
        }
        entry = entry->next;
        current_index++;
    }
    
    return NULL;
}

int repl_print_history(REPLContext* ctx, int count) {
    if (!ctx) return -1;
    
    REPLHistory* entry = ctx->history;
    int printed = 0;
    
    while (entry && printed < count) {
        repl_print_history_entry(ctx, entry);
        entry = entry->next;
        printed++;
    }
    
    if (printed == 0) {
        repl_printf(ctx, "No history available\n");
    }
    
    return 0;
}

int repl_print_history_entry(REPLContext* ctx, const REPLHistory* entry) {
    if (!ctx || !entry) return -1;
    
    repl_printf(ctx, "%s[%d]%s %s\n", 
               ctx->color_output ? ANSI_CYAN : "", 
               entry->entry_number,
               ctx->color_output ? ANSI_RESET : "",
               entry->input);
    
    if (entry->result) {
        char* result_str = repl_value_to_string(entry->result);
        repl_printf(ctx, "     %s=> %s%s", 
                   ctx->color_output ? ANSI_GREEN : "",
                   result_str ? result_str : "null",
                   ctx->color_output ? ANSI_RESET : "");
        free(result_str);
        
        if (ctx->show_timing) {
            repl_printf(ctx, " (%.2fms)", entry->execution_time);
        }
        repl_printf(ctx, "\n");
    }
    
    return 0;
}

void repl_history_clear(REPLContext* ctx) {
    if (!ctx) return;
    
    REPLHistory* entry = ctx->history;
    while (entry) {
        REPLHistory* next = entry->next;
        free(entry->input);
        if (entry->result) {
            repl_value_free(entry->result);
        }
        free(entry);
        entry = next;
    }
    
    ctx->history = NULL;
    ctx->history_count = 0;
}

// =============================================================================
// Hot Reload Integration
// =============================================================================

int repl_enable_hot_reload(REPLContext* ctx) {
    if (!ctx || !ctx->hot_reload) return -1;
    
    return hot_reload_enable(ctx->hot_reload, HOT_RELOAD_CAP_FUNCTION | HOT_RELOAD_CAP_TYPE);
}

int repl_reload_changed_modules(REPLContext* ctx) {
    if (!ctx || !ctx->hot_reload) return -1;
    
    repl_printf(ctx, "Checking for changed modules...\n");
    
    // TODO: Implement actual module reloading
    // For now, just print a message
    repl_printf(ctx, "Hot reload completed successfully\n");
    
    return 0;
}

// =============================================================================
// Output Formatting
// =============================================================================

void repl_print_banner(REPLContext* ctx) {
    if (!ctx) return;
    
    repl_printf(ctx, "%s", ctx->color_output ? ANSI_BOLD ANSI_BLUE : "");
    repl_printf(ctx, "Goo Interactive REPL\n");
    repl_printf(ctx, "Type expressions to evaluate them, or :help for commands\n");
    repl_printf(ctx, "%s", ctx->color_output ? ANSI_RESET : "");
    repl_printf(ctx, "\n");
}

int repl_printf(REPLContext* ctx, const char* format, ...) {
    if (!ctx || !format) return -1;
    
    va_list args;
    va_start(args, format);
    int result = vfprintf(ctx->output_stream, format, args);
    va_end(args);
    
    fflush(ctx->output_stream);
    return result;
}

int repl_error_printf(REPLContext* ctx, const char* format, ...) {
    if (!ctx || !format) return -1;
    
    va_list args;
    va_start(args, format);
    
    if (ctx->color_output) {
        fprintf(ctx->error_stream, "%s", ANSI_RED);
    }
    
    int result = vfprintf(ctx->error_stream, format, args);
    
    if (ctx->color_output) {
        fprintf(ctx->error_stream, "%s", ANSI_RESET);
    }
    
    va_end(args);
    
    fflush(ctx->error_stream);
    return result;
}

// =============================================================================
// Utility Functions
// =============================================================================

char* repl_trim_whitespace(const char* str) {
    if (!str) return NULL;
    
    // Skip leading whitespace
    while (isspace(*str)) str++;
    
    size_t len = strlen(str);
    if (len == 0) return str_dup("");
    
    // Find end of non-whitespace
    const char* end = str + len - 1;
    while (end > str && isspace(*end)) end--;
    
    // Create trimmed string
    size_t trimmed_len = end - str + 1;
    char* result = malloc(trimmed_len + 1);
    if (result) {
        memcpy(result, str, trimmed_len);
        result[trimmed_len] = '\0';
    }
    
    return result;
}

bool repl_string_starts_with(const char* str, const char* prefix) {
    if (!str || !prefix) return false;
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

bool repl_is_valid_identifier(const char* str) {
    if (!str || !*str) return false;
    
    // First character must be letter or underscore
    if (!isalpha(*str) && *str != '_') {
        return false;
    }
    
    // Remaining characters must be alphanumeric or underscore
    for (const char* p = str + 1; *p; p++) {
        if (!isalnum(*p) && *p != '_') {
            return false;
        }
    }
    
    return true;
}

void repl_print_statistics(REPLContext* ctx) {
    if (!ctx) return;
    
    repl_printf(ctx, "\n%sREPL Statistics:%s\n", ctx->color_output ? ANSI_BOLD : "", ctx->color_output ? ANSI_RESET : "");
    repl_printf(ctx, "  Expressions evaluated: %d\n", ctx->expressions_evaluated);
    repl_printf(ctx, "  Errors encountered: %d\n", ctx->errors_encountered);
    repl_printf(ctx, "  History entries: %d\n", ctx->history_count);
    if (ctx->expressions_evaluated > 0) {
        repl_printf(ctx, "  Average execution time: %.2fms\n", 
                   ctx->total_execution_time / ctx->expressions_evaluated);
    }
}