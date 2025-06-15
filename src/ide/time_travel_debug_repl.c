#include "time_travel_debug.h"
#include "repl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

// =============================================================================
// Color constants
// =============================================================================

#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_RESET   "\033[0m"

// =============================================================================
// Helper Functions
// =============================================================================

static void repl_debug_printf(REPLContext* ctx, const char* format, ...) {
    if (!ctx) return;
    
    va_list args;
    va_start(args, format);
    if (ctx->output_stream) {
        vfprintf(ctx->output_stream, format, args);
    } else {
        vprintf(format, args);
    }
    va_end(args);
}

static void repl_debug_error_printf(REPLContext* ctx, const char* format, ...) {
    if (!ctx) return;
    
    va_list args;
    va_start(args, format);
    if (ctx->error_stream) {
        vfprintf(ctx->error_stream, format, args);
    } else {
        vfprintf(stderr, format, args);
    }
    va_end(args);
}

static int parse_int_arg(const char* str, long* result) {
    if (!str || !result) return -1;
    
    char* endptr;
    *result = strtol(str, &endptr, 10);
    
    if (endptr == str || *endptr != '\0') {
        return -1; // Not a valid number
    }
    
    return 0;
}

// =============================================================================
// REPL Command Implementations
// =============================================================================

int repl_cmd_debug_step_back(REPLContext* ctx, const char* args) {
    (void)args; // Unused parameter
    
    if (!ctx || !ctx->debug_state) {
        repl_debug_error_printf(ctx, "Debug system not available.\n");
        return -1;
    }
    
    if (!debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    if (debug_step_backward(ctx->debug_state)) {
        uint64_t current_step = debug_get_current_step(ctx->debug_state);
        repl_debug_printf(ctx, "%sStep backward to step %lu%s\n", 
                         COLOR_GREEN, current_step, COLOR_RESET);
        
        // Show current state
        debug_print_current_state(ctx->debug_state);
    } else {
        repl_debug_printf(ctx, "%sAlready at the beginning of execution history%s\n",
                         COLOR_YELLOW, COLOR_RESET);
    }
    
    return 0;
}

int repl_cmd_debug_step_forward(REPLContext* ctx, const char* args) {
    (void)args; // Unused parameter
    
    if (!ctx || !ctx->debug_state) {
        repl_debug_error_printf(ctx, "Debug system not available.\n");
        return -1;
    }
    
    if (!debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    if (debug_step_forward(ctx->debug_state)) {
        uint64_t current_step = debug_get_current_step(ctx->debug_state);
        repl_debug_printf(ctx, "%sStep forward to step %lu%s\n", 
                         COLOR_GREEN, current_step, COLOR_RESET);
        
        // Show current state
        debug_print_current_state(ctx->debug_state);
    } else {
        repl_debug_printf(ctx, "%sAlready at the end of execution history%s\n",
                         COLOR_YELLOW, COLOR_RESET);
    }
    
    return 0;
}

int repl_cmd_debug_continue_back(REPLContext* ctx, const char* args) {
    (void)args; // Unused parameter
    
    if (!ctx || !ctx->debug_state) {
        repl_debug_error_printf(ctx, "Debug system not available.\n");
        return -1;
    }
    
    if (!debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    if (debug_continue_backward(ctx->debug_state)) {
        uint64_t current_step = debug_get_current_step(ctx->debug_state);
        repl_debug_printf(ctx, "%sContinued to beginning (step %lu)%s\n", 
                         COLOR_GREEN, current_step, COLOR_RESET);
        
        // Show current state
        debug_print_current_state(ctx->debug_state);
    } else {
        repl_debug_printf(ctx, "%sNo execution history available%s\n",
                         COLOR_YELLOW, COLOR_RESET);
    }
    
    return 0;
}

int repl_cmd_debug_continue_forward(REPLContext* ctx, const char* args) {
    (void)args; // Unused parameter
    
    if (!ctx || !ctx->debug_state) {
        repl_debug_error_printf(ctx, "Debug system not available.\n");
        return -1;
    }
    
    if (!debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    if (debug_continue_forward(ctx->debug_state)) {
        uint64_t current_step = debug_get_current_step(ctx->debug_state);
        repl_debug_printf(ctx, "%sContinued to end (step %lu)%s\n", 
                         COLOR_GREEN, current_step, COLOR_RESET);
        
        // Show current state
        debug_print_current_state(ctx->debug_state);
    } else {
        repl_debug_printf(ctx, "%sNo execution history available%s\n",
                         COLOR_YELLOW, COLOR_RESET);
    }
    
    return 0;
}

int repl_cmd_debug_goto(REPLContext* ctx, const char* args) {
    if (!ctx || !ctx->debug_state) {
        repl_debug_error_printf(ctx, "Debug system not available.\n");
        return -1;
    }
    
    if (!debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    if (!args || strlen(args) == 0) {
        repl_debug_error_printf(ctx, "Usage: :debug goto <step_number>\n");
        repl_debug_error_printf(ctx, "Example: :debug goto 42\n");
        return -1;
    }
    
    long step_number;
    if (parse_int_arg(args, &step_number) != 0 || step_number < 0) {
        repl_debug_error_printf(ctx, "Invalid step number: %s\n", args);
        return -1;
    }
    
    if (debug_goto_step(ctx->debug_state, (uint64_t)step_number)) {
        repl_debug_printf(ctx, "%sJumped to step %ld%s\n", 
                         COLOR_GREEN, step_number, COLOR_RESET);
        
        // Show current state
        debug_print_current_state(ctx->debug_state);
    } else {
        repl_debug_error_printf(ctx, "Step %ld not found in execution history\n", step_number);
    }
    
    return 0;
}

int repl_cmd_debug_timeline(REPLContext* ctx, const char* args) {
    (void)args; // Unused parameter
    
    if (!ctx || !ctx->debug_state) {
        repl_debug_error_printf(ctx, "Debug system not available.\n");
        return -1;
    }
    
    if (!debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    debug_print_timeline(ctx->debug_state);
    return 0;
}

int repl_cmd_debug_state(REPLContext* ctx, const char* args) {
    (void)args; // Unused parameter
    
    if (!ctx || !ctx->debug_state) {
        repl_debug_error_printf(ctx, "Debug system not available.\n");
        return -1;
    }
    
    if (!debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    debug_print_current_state(ctx->debug_state);
    return 0;
}

int repl_cmd_debug_variables(REPLContext* ctx, const char* args) {
    (void)args; // Unused parameter
    
    if (!ctx || !ctx->debug_state) {
        repl_debug_error_printf(ctx, "Debug system not available.\n");
        return -1;
    }
    
    if (!debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    DebugSnapshot* snapshot = debug_get_current_snapshot(ctx->debug_state);
    if (!snapshot) {
        repl_debug_printf(ctx, "No current debug snapshot available.\n");
        return 0;
    }
    
    repl_debug_printf(ctx, "%s=== Variables ===%s\n", COLOR_BOLD, COLOR_RESET);
    
    // Show global variables
    if (snapshot->global_variables) {
        repl_debug_printf(ctx, "%sGlobal Variables:%s\n", COLOR_CYAN, COLOR_RESET);
        DebugVariable* var = snapshot->global_variables;
        while (var) {
            repl_debug_printf(ctx, "  %s: %s\n", var->name, 
                             var->string_repr ? var->string_repr : "<no value>");
            var = var->next;
        }
    } else {
        repl_debug_printf(ctx, "No global variables available.\n");
    }
    
    // Show local variables from current frame
    if (snapshot->call_stack && snapshot->call_stack->local_variables) {
        repl_debug_printf(ctx, "%sLocal Variables:%s\n", COLOR_CYAN, COLOR_RESET);
        DebugVariable* var = snapshot->call_stack->local_variables;
        while (var) {
            repl_debug_printf(ctx, "  %s: %s\n", var->name, 
                             var->string_repr ? var->string_repr : "<no value>");
            var = var->next;
        }
    } else {
        repl_debug_printf(ctx, "No local variables available.\n");
    }
    
    return 0;
}

int repl_cmd_debug_stack(REPLContext* ctx, const char* args) {
    (void)args; // Unused parameter
    
    if (!ctx || !ctx->debug_state) {
        repl_debug_error_printf(ctx, "Debug system not available.\n");
        return -1;
    }
    
    if (!debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    DebugSnapshot* snapshot = debug_get_current_snapshot(ctx->debug_state);
    if (!snapshot) {
        repl_debug_printf(ctx, "No current debug snapshot available.\n");
        return 0;
    }
    
    debug_print_call_stack(snapshot);
    return 0;
}

int repl_cmd_debug_snapshot(REPLContext* ctx, const char* args) {
    if (!ctx || !ctx->debug_state) {
        repl_debug_error_printf(ctx, "Debug system not available.\n");
        return -1;
    }
    
    if (!debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    const char* description = args && strlen(args) > 0 ? args : "Manual REPL snapshot";
    debug_create_manual_snapshot(ctx->debug_state, description);
    
    repl_debug_printf(ctx, "%sCreated manual snapshot: %s%s\n", 
                     COLOR_GREEN, description, COLOR_RESET);
    
    return 0;
}

int repl_cmd_debug_enable(REPLContext* ctx, const char* args) {
    (void)args; // Unused parameter
    
    if (!ctx || !ctx->debug_state) {
        repl_debug_error_printf(ctx, "Debug system not available.\n");
        return -1;
    }
    
    debug_enable(ctx->debug_state);
    repl_debug_printf(ctx, "%sTime-travel debugging enabled%s\n", 
                     COLOR_GREEN, COLOR_RESET);
    
    // Start a debug session
    if (!debug_is_in_session(ctx->debug_state)) {
        debug_start_session(ctx->debug_state, "REPL Debug Session");
        repl_debug_printf(ctx, "%sDebug session started%s\n", 
                         COLOR_BLUE, COLOR_RESET);
    }
    
    return 0;
}

int repl_cmd_debug_disable(REPLContext* ctx, const char* args) {
    (void)args; // Unused parameter
    
    if (!ctx || !ctx->debug_state) {
        repl_debug_error_printf(ctx, "Debug system not available.\n");
        return -1;
    }
    
    debug_disable(ctx->debug_state);
    repl_debug_printf(ctx, "%sTime-travel debugging disabled%s\n", 
                     COLOR_YELLOW, COLOR_RESET);
    
    if (debug_is_in_session(ctx->debug_state)) {
        debug_end_session(ctx->debug_state);
        repl_debug_printf(ctx, "%sDebug session ended%s\n", 
                         COLOR_BLUE, COLOR_RESET);
    }
    
    return 0;
}

int repl_cmd_debug_find(REPLContext* ctx, const char* args) {
    if (!ctx || !ctx->debug_state) {
        repl_debug_error_printf(ctx, "Debug system not available.\n");
        return -1;
    }
    
    if (!debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    if (!args || strlen(args) == 0) {
        repl_debug_error_printf(ctx, "Usage: :debug find <search_term>\n");
        repl_debug_error_printf(ctx, "Available searches:\n");
        repl_debug_error_printf(ctx, "  :debug find error         - Find error events\n");
        repl_debug_error_printf(ctx, "  :debug find function <name> - Find function calls\n");
        repl_debug_error_printf(ctx, "  :debug find variable <name> - Find variable changes\n");
        return -1;
    }
    
    // Parse search type
    if (strncmp(args, "error", 5) == 0) {
        DebugSnapshot* found = debug_find_event(ctx->debug_state, DEBUG_EVENT_ERROR_OCCURRED, false);
        if (found) {
            ctx->debug_state->timeline->current = found;
            repl_debug_printf(ctx, "%sFound error at step %lu%s\n", 
                             COLOR_GREEN, found->step_count, COLOR_RESET);
            debug_print_current_state(ctx->debug_state);
        } else {
            repl_debug_printf(ctx, "%sNo error events found%s\n", COLOR_YELLOW, COLOR_RESET);
        }
    } else if (strncmp(args, "function ", 9) == 0) {
        const char* function_name = args + 9;
        DebugSnapshot* found = debug_find_function_call(ctx->debug_state, function_name, false);
        if (found) {
            ctx->debug_state->timeline->current = found;
            repl_debug_printf(ctx, "%sFound function '%s' at step %lu%s\n", 
                             COLOR_GREEN, function_name, found->step_count, COLOR_RESET);
            debug_print_current_state(ctx->debug_state);
        } else {
            repl_debug_printf(ctx, "%sNo calls to function '%s' found%s\n", 
                             COLOR_YELLOW, function_name, COLOR_RESET);
        }
    } else if (strncmp(args, "variable ", 9) == 0) {
        const char* var_name = args + 9;
        DebugSnapshot* found = debug_find_variable_change(ctx->debug_state, var_name, false);
        if (found) {
            ctx->debug_state->timeline->current = found;
            repl_debug_printf(ctx, "%sFound variable '%s' change at step %lu%s\n", 
                             COLOR_GREEN, var_name, found->step_count, COLOR_RESET);
            debug_print_current_state(ctx->debug_state);
        } else {
            repl_debug_printf(ctx, "%sNo changes to variable '%s' found%s\n", 
                             COLOR_YELLOW, var_name, COLOR_RESET);
        }
    } else {
        repl_debug_error_printf(ctx, "Unknown search type: %s\n", args);
        return -1;
    }
    
    return 0;
}

// =============================================================================
// REPL Integration
// =============================================================================

bool repl_debug_init(REPLContext* ctx) {
    if (!ctx) return false;
    
    // Create debug state if it doesn't exist
    if (!ctx->debug_state) {
        ctx->debug_state = debug_state_new();
        if (!ctx->debug_state) {
            return false;
        }
        
        // Link error context
        ctx->debug_state->error_context = ctx->error_context;
        ctx->debug_state->repl_context = ctx;
    }
    
    return true;
}

void repl_debug_cleanup(REPLContext* ctx) {
    if (!ctx || !ctx->debug_state) return;
    
    debug_state_free(ctx->debug_state);
    ctx->debug_state = NULL;
}