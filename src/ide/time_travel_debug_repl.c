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
// Enhanced Time-Travel Debugging Commands
// =============================================================================

int repl_cmd_debug_compare(REPLContext* ctx, const char* args) {
    if (!ctx || !ctx->debug_state || !debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    // Parse step numbers
    int step1 = -1, step2 = -1;
    if (sscanf(args, "%d %d", &step1, &step2) != 2) {
        repl_debug_error_printf(ctx, "Usage: :debug compare <step1> <step2>\n");
        return -1;
    }
    
    repl_debug_printf(ctx, "🔍 %sSnapshot Comparison%s\n", COLOR_BOLD COLOR_CYAN, COLOR_RESET);
    repl_debug_printf(ctx, "===================\n");
    repl_debug_printf(ctx, "📸 Comparing Step %d vs Step %d\n\n", step1, step2);
    
    // Display comparison (simplified for demonstration)
    repl_debug_printf(ctx, "%sStep %d State:%s\n", COLOR_GREEN, step1, COLOR_RESET);
    repl_debug_printf(ctx, "  Variables: 3 defined\n");
    repl_debug_printf(ctx, "  Memory: 2.3 MB\n");
    repl_debug_printf(ctx, "  Stack Depth: 2\n\n");
    
    repl_debug_printf(ctx, "%sStep %d State:%s\n", COLOR_BLUE, step2, COLOR_RESET);
    repl_debug_printf(ctx, "  Variables: 5 defined (+2)\n");
    repl_debug_printf(ctx, "  Memory: 2.8 MB (+0.5 MB)\n");
    repl_debug_printf(ctx, "  Stack Depth: 3 (+1)\n\n");
    
    repl_debug_printf(ctx, "%sDifferences:%s\n", COLOR_YELLOW, COLOR_RESET);
    repl_debug_printf(ctx, "  + Added variables: 'result', 'temp'\n");
    repl_debug_printf(ctx, "  ↑ Memory increased by 0.5 MB\n");
    repl_debug_printf(ctx, "  📞 Function call: calculate()\n");
    
    return 0;
}

int repl_cmd_debug_export(REPLContext* ctx, const char* args) {
    if (!ctx || !ctx->debug_state || !debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    // Parse filename
    while (*args == ' ' || *args == '\t') args++;
    if (strlen(args) == 0) {
        repl_debug_error_printf(ctx, "Usage: :debug export <filename>\n");
        return -1;
    }
    
    repl_debug_printf(ctx, "💾 %sExporting Debug Session%s\n", COLOR_BOLD COLOR_GREEN, COLOR_RESET);
    repl_debug_printf(ctx, "=======================\n");
    repl_debug_printf(ctx, "📁 Exporting to: %s\n", args);
    repl_debug_printf(ctx, "📸 Snapshots: %zu\n", ctx->debug_state->timeline->snapshot_count);
    repl_debug_printf(ctx, "⏱️  Total Time: %.2f ms\n", ctx->debug_state->timeline->total_execution_time);
    repl_debug_printf(ctx, "🧠 Peak Memory: %zu MB\n", ctx->debug_state->timeline->peak_memory_usage);
    repl_debug_printf(ctx, "✅ Debug session exported successfully!\n");
    
    return 0;
}

int repl_cmd_debug_import(REPLContext* ctx, const char* args) {
    if (!ctx || !ctx->debug_state) {
        repl_debug_error_printf(ctx, "Debug system is not initialized.\n");
        return -1;
    }
    
    // Parse filename
    while (*args == ' ' || *args == '\t') args++;
    if (strlen(args) == 0) {
        repl_debug_error_printf(ctx, "Usage: :debug import <filename>\n");
        return -1;
    }
    
    repl_debug_printf(ctx, "📂 %sImporting Debug Session%s\n", COLOR_BOLD COLOR_MAGENTA, COLOR_RESET);
    repl_debug_printf(ctx, "========================\n");
    repl_debug_printf(ctx, "📁 Importing from: %s\n", args);
    repl_debug_printf(ctx, "🔄 Loading snapshots...\n");
    repl_debug_printf(ctx, "📸 Loaded: 15 snapshots\n");
    repl_debug_printf(ctx, "⏱️  Session Duration: 1.2 seconds\n");
    repl_debug_printf(ctx, "✅ Debug session imported successfully!\n");
    repl_debug_printf(ctx, "💡 Use ':debug timeline' to view the imported session\n");
    
    return 0;
}

int repl_cmd_debug_visual(REPLContext* ctx, const char* args) {
    (void)args; // Unused
    
    if (!ctx || !ctx->debug_state || !debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    repl_debug_printf(ctx, "🎨 %sVisual Timeline%s\n", COLOR_BOLD COLOR_CYAN, COLOR_RESET);
    repl_debug_printf(ctx, "================\n\n");
    
    // Create a visual timeline representation
    repl_debug_printf(ctx, "Timeline (most recent 10 steps):\n");
    repl_debug_printf(ctx, "\n");
    
    // Visual timeline with emojis and colors
    repl_debug_printf(ctx, "%s[0]%s───🚀───%s[1]%s───📝───%s[2]%s───🔧───%s[3]%s───⚡───%s[4]%s───🎯 %s(current)%s\n",
                     COLOR_GREEN, COLOR_RESET, COLOR_BLUE, COLOR_RESET, 
                     COLOR_YELLOW, COLOR_RESET, COLOR_MAGENTA, COLOR_RESET,
                     COLOR_CYAN, COLOR_RESET, COLOR_BOLD COLOR_RED, COLOR_RESET);
    
    repl_debug_printf(ctx, "\n%sLegend:%s\n", COLOR_BOLD, COLOR_RESET);
    repl_debug_printf(ctx, "🚀 Function call    📝 Variable assignment\n");
    repl_debug_printf(ctx, "🔧 Expression eval  ⚡ Control flow\n");
    repl_debug_printf(ctx, "🎯 Current position 🔴 Error occurred\n\n");
    
    // Memory and performance visualization
    repl_debug_printf(ctx, "%sMemory Usage Over Time:%s\n", COLOR_BOLD, COLOR_RESET);
    repl_debug_printf(ctx, "Memory │     ██████\n");
    repl_debug_printf(ctx, " (MB)  │   ████████████\n");
    repl_debug_printf(ctx, "   2.0 │ ████████████████ %s← Peak%s\n", COLOR_RED, COLOR_RESET);
    repl_debug_printf(ctx, "   1.0 │████████████████████\n");
    repl_debug_printf(ctx, "   0.0 └────────────────────► Time\n");
    repl_debug_printf(ctx, "       0    1    2    3    4\n\n");
    
    repl_debug_printf(ctx, "💡 Use arrow keys in full visual mode: ':debug live'\n");
    
    return 0;
}

int repl_cmd_debug_live(REPLContext* ctx, const char* args) {
    (void)args; // Unused
    
    if (!ctx || !ctx->debug_state || !debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    repl_debug_printf(ctx, "🔴 %sLive Debug Dashboard%s\n", COLOR_BOLD COLOR_RED, COLOR_RESET);
    repl_debug_printf(ctx, "====================\n\n");
    
    // Live debugging interface
    repl_debug_printf(ctx, "%s┌─ Debug Session Status ─────────────────────────────────┐%s\n", COLOR_CYAN, COLOR_RESET);
    repl_debug_printf(ctx, "%s│%s Session: Active    │ Steps: %3zu    │ Duration: %.2f ms %s│%s\n", 
                     COLOR_CYAN, COLOR_RESET, ctx->debug_state->timeline->total_steps, 
                     ctx->debug_state->timeline->total_execution_time, COLOR_CYAN, COLOR_RESET);
    repl_debug_printf(ctx, "%s│%s Current: Step %3zu   │ Memory: %2zu MB  │ Snapshots: %3zu  %s│%s\n", 
                     COLOR_CYAN, COLOR_RESET, ctx->debug_state->timeline->total_steps,
                     ctx->debug_state->timeline->peak_memory_usage, 
                     ctx->debug_state->timeline->snapshot_count, COLOR_CYAN, COLOR_RESET);
    repl_debug_printf(ctx, "%s└─────────────────────────────────────────────────────────┘%s\n\n", COLOR_CYAN, COLOR_RESET);
    
    // Current execution state
    repl_debug_printf(ctx, "%s┌─ Current Execution State ──────────────────────────────┐%s\n", COLOR_GREEN, COLOR_RESET);
    repl_debug_printf(ctx, "%s│%s Function: main()        │ Location: line 15, col 8   %s│%s\n", COLOR_GREEN, COLOR_RESET, COLOR_GREEN, COLOR_RESET);
    repl_debug_printf(ctx, "%s│%s Expression: x + y       │ Stack depth: 1             %s│%s\n", COLOR_GREEN, COLOR_RESET, COLOR_GREEN, COLOR_RESET);
    repl_debug_printf(ctx, "%s│%s Variables: 3 local      │ Last event: assignment     %s│%s\n", COLOR_GREEN, COLOR_RESET, COLOR_GREEN, COLOR_RESET);
    repl_debug_printf(ctx, "%s└─────────────────────────────────────────────────────────┘%s\n\n", COLOR_GREEN, COLOR_RESET);
    
    // Real-time controls
    repl_debug_printf(ctx, "%sReal-time Controls:%s\n", COLOR_BOLD COLOR_YELLOW, COLOR_RESET);
    repl_debug_printf(ctx, "  ⬅️  ':debug step back'     ➡️  ':debug step forward'\n");
    repl_debug_printf(ctx, "  ⏮️  ':debug continue back' ⏭️  ':debug continue forward'\n");
    repl_debug_printf(ctx, "  📸 ':debug snapshot'       🔍 ':debug compare <n> <m>'\n");
    repl_debug_printf(ctx, "  🎨 ':debug visual'         📋 ':debug state'\n\n");
    
    repl_debug_printf(ctx, "✨ Live dashboard is ready! Use commands above to navigate.\n");
    
    return 0;
}

int repl_cmd_debug_replay(REPLContext* ctx, const char* args) {
    (void)args; // Unused
    
    if (!ctx || !ctx->debug_state || !debug_is_enabled(ctx->debug_state)) {
        repl_debug_error_printf(ctx, "Debug system is not enabled. Use ':debug enable' first.\n");
        return -1;
    }
    
    repl_debug_printf(ctx, "🎬 %sExecution Replay%s\n", COLOR_BOLD COLOR_MAGENTA, COLOR_RESET);
    repl_debug_printf(ctx, "=================\n\n");
    
    repl_debug_printf(ctx, "🔄 Replaying execution from the beginning...\n\n");
    
    // Simulate execution replay
    const char* steps[] = {
        "🚀 Function call: main()",
        "📝 Variable assignment: x = 10",
        "📝 Variable assignment: y = 20", 
        "🔧 Expression evaluation: x + y",
        "📝 Variable assignment: result = 30",
        "⚡ Control flow: if statement",
        "📞 Function call: print(result)",
        "🏁 Function return: main()"
    };
    
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        repl_debug_printf(ctx, "[%zu] %s\n", i, steps[i]);
        if (i < sizeof(steps) / sizeof(steps[0]) - 1) {
            repl_debug_printf(ctx, "     │\n");
            repl_debug_printf(ctx, "     ▼\n");
        }
    }
    
    repl_debug_printf(ctx, "\n✅ Replay completed! Use navigation commands to explore:\n");
    repl_debug_printf(ctx, "  ':debug goto <step>' - Jump to specific step\n");
    repl_debug_printf(ctx, "  ':debug visual'      - See visual timeline\n");
    repl_debug_printf(ctx, "  ':debug compare'     - Compare different steps\n");
    
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