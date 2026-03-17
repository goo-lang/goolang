#include "time_travel_debug.h"
#include "repl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

// =============================================================================
// Color constants for output formatting
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

static uint64_t get_current_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static char* str_dup_safe(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

// =============================================================================
// Debug State Management
// =============================================================================

DebugState* debug_state_new(void) {
    DebugState* state = calloc(1, sizeof(DebugState));
    if (!state) return NULL;
    
    state->timeline = debug_timeline_new(1000); // Default: keep last 1000 snapshots
    if (!state->timeline) {
        free(state);
        return NULL;
    }
    
    // Default configuration
    state->enabled = false;
    state->auto_snapshot = true;
    state->snapshot_frequency = 1; // Snapshot every step
    state->snapshot_on_errors = true;
    state->snapshot_on_function_calls = true;
    state->snapshot_on_assignments = false;
    state->snapshot_on_control_flow = false;
    state->in_debug_session = false;
    
    return state;
}

void debug_state_free(DebugState* state) {
    if (!state) return;
    
    if (state->timeline) {
        debug_timeline_free(state->timeline);
    }
    
    if (state->session_name) {
        free(state->session_name);
    }
    
    free(state);
}

// =============================================================================
// Timeline Management
// =============================================================================

DebugTimeline* debug_timeline_new(size_t max_snapshots) {
    DebugTimeline* timeline = calloc(1, sizeof(DebugTimeline));
    if (!timeline) return NULL;
    
    timeline->max_snapshots = max_snapshots;
    timeline->next_snapshot_id = 1;
    
    return timeline;
}

void debug_timeline_free(DebugTimeline* timeline) {
    if (!timeline) return;
    
    debug_timeline_clear(timeline);
    free(timeline);
}

void debug_timeline_clear(DebugTimeline* timeline) {
    if (!timeline) return;
    
    DebugSnapshot* current = timeline->head;
    while (current) {
        DebugSnapshot* next = current->next;
        debug_snapshot_free(current);
        current = next;
    }
    
    timeline->head = NULL;
    timeline->tail = NULL;
    timeline->current = NULL;
    timeline->snapshot_count = 0;
    timeline->total_steps = 0;
    timeline->total_execution_time = 0.0;
    timeline->peak_memory_usage = 0;
}

// =============================================================================
// Snapshot Management
// =============================================================================

DebugSnapshot* debug_snapshot_create(DebugState* state, DebugEventType event_type, 
                                     const char* description) {
    if (!state) return NULL;
    
    DebugSnapshot* snapshot = calloc(1, sizeof(DebugSnapshot));
    if (!snapshot) return NULL;
    
    snapshot->timestamp = get_current_timestamp();
    snapshot->step_count = state->timeline->total_steps++;
    snapshot->event_type = event_type;
    snapshot->event_description = str_dup_safe(description);
    snapshot->snapshot_id = state->timeline->next_snapshot_id++;
    
    // Initialize performance metrics
    snapshot->cpu_time = 0.0; // Would be filled by actual execution context
    snapshot->instruction_count = 0;
    snapshot->heap_size = 0;
    snapshot->stack_size = 0;
    
    return snapshot;
}

void debug_snapshot_free(DebugSnapshot* snapshot) {
    if (!snapshot) return;
    
    if (snapshot->event_description) {
        free(snapshot->event_description);
    }
    
    if (snapshot->current_expression) {
        free(snapshot->current_expression);
    }
    
    if (snapshot->memory_layout) {
        free(snapshot->memory_layout);
    }
    
    // Free call stack
    DebugStackFrame* frame = snapshot->call_stack;
    while (frame) {
        DebugStackFrame* next = frame->next;
        debug_stack_frame_free(frame);
        frame = next;
    }
    
    // Free global variables
    debug_variable_list_free(snapshot->global_variables);
    
    free(snapshot);
}

bool debug_snapshot_add(DebugTimeline* timeline, DebugSnapshot* snapshot) {
    if (!timeline || !snapshot) return false;
    
    // Add to timeline
    if (!timeline->head) {
        timeline->head = timeline->tail = timeline->current = snapshot;
    } else {
        timeline->tail->next = snapshot;
        snapshot->previous = timeline->tail;
        timeline->tail = snapshot;
        timeline->current = snapshot; // Move to newest snapshot
    }
    
    timeline->snapshot_count++;
    
    // Remove old snapshots if we exceed max
    while (timeline->snapshot_count > timeline->max_snapshots && timeline->head) {
        DebugSnapshot* old_head = timeline->head;
        timeline->head = timeline->head->next;
        if (timeline->head) {
            timeline->head->previous = NULL;
        } else {
            timeline->tail = NULL;
        }
        
        // Don't free the current snapshot
        if (old_head != timeline->current) {
            debug_snapshot_free(old_head);
            timeline->snapshot_count--;
        } else {
            // If we're removing the current snapshot, move to next
            timeline->current = timeline->head;
        }
    }
    
    return true;
}

// =============================================================================
// Variable Management
// =============================================================================

DebugVariable* debug_variable_create(const char* name, Type* type, 
                                     const void* value, size_t value_size) {
    if (!name) return NULL;
    
    DebugVariable* var = calloc(1, sizeof(DebugVariable));
    if (!var) return NULL;
    
    var->name = str_dup_safe(name);
    var->type = type;
    var->value_size = value_size;
    
    if (value && value_size > 0) {
        var->value = malloc(value_size);
        if (var->value) {
            memcpy(var->value, value, value_size);
        }
    }
    
    // Create string representation
    if (type) {
        var->string_repr = debug_value_to_string(type, value);
    }
    
    return var;
}

void debug_variable_free(DebugVariable* var) {
    if (!var) return;
    
    if (var->name) free(var->name);
    if (var->value) free(var->value);
    if (var->string_repr) free(var->string_repr);
    
    free(var);
}

void debug_variable_list_free(DebugVariable* vars) {
    while (vars) {
        DebugVariable* next = vars->next;
        debug_variable_free(vars);
        vars = next;
    }
}

// =============================================================================
// Stack Frame Management
// =============================================================================

DebugStackFrame* debug_stack_frame_create(const char* function_name, 
                                          SourceLocation location) {
    DebugStackFrame* frame = calloc(1, sizeof(DebugStackFrame));
    if (!frame) return NULL;
    
    frame->function_name = str_dup_safe(function_name);
    frame->location = location;
    
    return frame;
}

void debug_stack_frame_free(DebugStackFrame* frame) {
    if (!frame) return;
    
    if (frame->function_name) {
        free(frame->function_name);
    }
    
    debug_variable_list_free(frame->local_variables);
    free(frame);
}

void debug_stack_frame_add_variable(DebugStackFrame* frame, DebugVariable* var) {
    if (!frame || !var) return;
    
    var->next = frame->local_variables;
    frame->local_variables = var;
}

// =============================================================================
// Time-Travel Navigation
// =============================================================================

bool debug_step_forward(DebugState* state) {
    if (!state || !state->timeline || !state->timeline->current) {
        return false;
    }
    
    if (state->timeline->current->next) {
        state->timeline->current = state->timeline->current->next;
        return true;
    }
    
    return false; // Already at end
}

bool debug_step_backward(DebugState* state) {
    if (!state || !state->timeline || !state->timeline->current) {
        return false;
    }
    
    if (state->timeline->current->previous) {
        state->timeline->current = state->timeline->current->previous;
        return true;
    }
    
    return false; // Already at beginning
}

bool debug_continue_forward(DebugState* state) {
    if (!state || !state->timeline) return false;
    
    // Move to the newest snapshot
    if (state->timeline->tail) {
        state->timeline->current = state->timeline->tail;
        return true;
    }
    
    return false;
}

bool debug_continue_backward(DebugState* state) {
    if (!state || !state->timeline) return false;
    
    // Move to the oldest snapshot
    if (state->timeline->head) {
        state->timeline->current = state->timeline->head;
        return true;
    }
    
    return false;
}

bool debug_goto_step(DebugState* state, uint64_t step_number) {
    if (!state || !state->timeline) return false;
    
    DebugSnapshot* snapshot = state->timeline->head;
    while (snapshot) {
        if (snapshot->step_count == step_number) {
            state->timeline->current = snapshot;
            return true;
        }
        snapshot = snapshot->next;
    }
    
    return false; // Step not found
}

bool debug_goto_snapshot(DebugState* state, uint64_t snapshot_id) {
    if (!state || !state->timeline) return false;
    
    DebugSnapshot* snapshot = state->timeline->head;
    while (snapshot) {
        if (snapshot->snapshot_id == snapshot_id) {
            state->timeline->current = snapshot;
            return true;
        }
        snapshot = snapshot->next;
    }
    
    return false; // Snapshot not found
}

// =============================================================================
// Position Queries
// =============================================================================

uint64_t debug_get_current_step(const DebugState* state) {
    if (!state || !state->timeline || !state->timeline->current) {
        return 0;
    }
    return state->timeline->current->step_count;
}

uint64_t debug_get_current_timestamp(const DebugState* state) {
    if (!state || !state->timeline || !state->timeline->current) {
        return 0;
    }
    return state->timeline->current->timestamp;
}

DebugSnapshot* debug_get_current_snapshot(const DebugState* state) {
    if (!state || !state->timeline) return NULL;
    return state->timeline->current;
}

bool debug_is_at_beginning(const DebugState* state) {
    if (!state || !state->timeline) return true;
    return state->timeline->current == state->timeline->head;
}

bool debug_is_at_end(const DebugState* state) {
    if (!state || !state->timeline) return true;
    return state->timeline->current == state->timeline->tail;
}

// =============================================================================
// Event Recording
// =============================================================================

void debug_record_expression_start(DebugState* state, const char* expression, 
                                   SourceLocation location) {
    if (!state || !state->enabled) return;
    
    DebugSnapshot* snapshot = debug_snapshot_create(state, DEBUG_EVENT_EXPRESSION_START,
                                                    "Starting expression evaluation");
    if (snapshot) {
        snapshot->current_expression = str_dup_safe(expression);
        snapshot->current_location = location;
        debug_snapshot_add(state->timeline, snapshot);
    }
}

void debug_record_expression_end(DebugState* state, const char* expression,
                                 void* result) {
    (void)result;
    if (!state || !state->enabled) return;
    
    DebugSnapshot* snapshot = debug_snapshot_create(state, DEBUG_EVENT_EXPRESSION_END,
                                                    "Finished expression evaluation");
    if (snapshot) {
        snapshot->current_expression = str_dup_safe(expression);
        debug_snapshot_add(state->timeline, snapshot);
    }
}

void debug_record_function_call(DebugState* state, const char* function_name, 
                                SourceLocation location) {
    if (!state || !state->enabled || !state->snapshot_on_function_calls) return;
    
    char description[256];
    snprintf(description, sizeof(description), "Calling function: %s", 
             function_name ? function_name : "<unknown>");
    
    DebugSnapshot* snapshot = debug_snapshot_create(state, DEBUG_EVENT_FUNCTION_CALL,
                                                    description);
    if (snapshot) {
        snapshot->current_location = location;
        debug_snapshot_add(state->timeline, snapshot);
    }
}

void debug_record_error(DebugState* state, const Error* error) {
    if (!state || !state->enabled || !state->snapshot_on_errors || !error) return;
    
    char description[256];
    snprintf(description, sizeof(description), "Error occurred: %s", 
             error->message ? error->message : "Unknown error");
    
    DebugSnapshot* snapshot = debug_snapshot_create(state, DEBUG_EVENT_ERROR_OCCURRED,
                                                    description);
    if (snapshot) {
        snapshot->current_location = error->location;
        debug_snapshot_add(state->timeline, snapshot);
    }
}

void debug_create_manual_snapshot(DebugState* state, const char* description) {
    if (!state || !state->enabled) return;
    
    DebugSnapshot* snapshot = debug_snapshot_create(state, DEBUG_EVENT_MANUAL_SNAPSHOT,
                                                    description ? description : "Manual snapshot");
    if (snapshot) {
        debug_snapshot_add(state->timeline, snapshot);
    }
}

// =============================================================================
// Configuration
// =============================================================================

void debug_enable(DebugState* state) {
    if (state) state->enabled = true;
}

void debug_disable(DebugState* state) {
    if (state) state->enabled = false;
}

bool debug_is_enabled(const DebugState* state) {
    return state && state->enabled;
}

void debug_set_auto_snapshot(DebugState* state, bool enabled) {
    if (state) state->auto_snapshot = enabled;
}

void debug_set_max_snapshots(DebugState* state, size_t max_snapshots) {
    if (state && state->timeline) {
        state->timeline->max_snapshots = max_snapshots;
    }
}

bool debug_start_session(DebugState* state, const char* session_name) {
    if (!state || state->in_debug_session) return false;
    
    state->in_debug_session = true;
    state->session_start_time = get_current_timestamp();
    
    if (session_name) {
        free(state->session_name);
        state->session_name = str_dup_safe(session_name);
    }
    
    // Clear existing timeline
    debug_timeline_clear(state->timeline);
    
    return true;
}

void debug_end_session(DebugState* state) {
    if (state) {
        state->in_debug_session = false;
    }
}

// =============================================================================
// Visualization and Display
// =============================================================================

void debug_print_timeline(const DebugState* state) {
    if (!state || !state->timeline) {
        printf("No debug timeline available.\n");
        return;
    }
    
    printf("%s=== Debug Timeline ===%s\n", COLOR_BOLD, COLOR_RESET);
    printf("Total snapshots: %zu\n", state->timeline->snapshot_count);
    printf("Total steps: %lu\n", state->timeline->total_steps);
    
    if (state->timeline->snapshot_count == 0) {
        printf("No snapshots recorded.\n");
        return;
    }
    
    printf("\n%sStep   | Event Type          | Description%s\n", COLOR_CYAN, COLOR_RESET);
    printf("-------|---------------------|----------------------------------\n");
    
    DebugSnapshot* snapshot = state->timeline->head;
    while (snapshot) {
        const char* marker = (snapshot == state->timeline->current) ? "→" : " ";
        const char* color = (snapshot == state->timeline->current) ? COLOR_YELLOW : COLOR_RESET;
        
        printf("%s%s%6lu | %-19s | %.40s%s\n",
               color, marker, snapshot->step_count,
               debug_format_event_type(snapshot->event_type),
               snapshot->event_description ? snapshot->event_description : "",
               COLOR_RESET);
        
        snapshot = snapshot->next;
    }
    printf("\n");
}

void debug_print_current_state(const DebugState* state) {
    if (!state || !state->timeline || !state->timeline->current) {
        printf("No current debug state available.\n");
        return;
    }
    
    DebugSnapshot* snapshot = state->timeline->current;
    
    printf("%s=== Current Debug State ===%s\n", COLOR_BOLD, COLOR_RESET);
    printf("Step: %lu\n", snapshot->step_count);
    printf("Event: %s\n", debug_format_event_type(snapshot->event_type));
    printf("Description: %s\n", snapshot->event_description ? snapshot->event_description : "N/A");
    
    if (snapshot->current_expression) {
        printf("Expression: %s\n", snapshot->current_expression);
    }
    
    if (snapshot->current_location.filename) {
        printf("Location: %s:%zu:%zu\n", 
               snapshot->current_location.filename,
               snapshot->current_location.line,
               snapshot->current_location.column);
    }
    
    printf("Memory usage: Heap=%zu, Stack=%zu\n", 
           snapshot->heap_size, snapshot->stack_size);
    printf("Performance: CPU=%.2fms, Instructions=%lu\n",
           snapshot->cpu_time, snapshot->instruction_count);
    printf("\n");
}

void debug_print_call_stack(const DebugSnapshot* snapshot) {
    if (!snapshot) {
        printf("No snapshot provided.\n");
        return;
    }
    
    printf("%s=== Call Stack ===%s\n", COLOR_BOLD, COLOR_RESET);
    
    if (!snapshot->call_stack) {
        printf("No call stack information available.\n");
        return;
    }
    
    int depth = 0;
    DebugStackFrame* frame = snapshot->call_stack;
    while (frame) {
        printf("#%d %s%s%s", depth, COLOR_GREEN, 
               frame->function_name ? frame->function_name : "<unknown>", COLOR_RESET);
        
        if (frame->location.filename) {
            printf(" at %s:%zu:%zu", frame->location.filename, 
                   frame->location.line, frame->location.column);
        }
        printf("\n");
        
        frame = frame->parent;
        depth++;
    }
    printf("\n");
}

// =============================================================================
// Utility Functions
// =============================================================================

char* debug_format_event_type(DebugEventType event_type) {
    switch (event_type) {
        case DEBUG_EVENT_EXPRESSION_START: return "Expression Start";
        case DEBUG_EVENT_EXPRESSION_END: return "Expression End";
        case DEBUG_EVENT_FUNCTION_CALL: return "Function Call";
        case DEBUG_EVENT_FUNCTION_RETURN: return "Function Return";
        case DEBUG_EVENT_VARIABLE_ASSIGNMENT: return "Variable Assignment";
        case DEBUG_EVENT_CONTROL_FLOW: return "Control Flow";
        case DEBUG_EVENT_ERROR_OCCURRED: return "Error";
        case DEBUG_EVENT_BREAKPOINT: return "Breakpoint";
        case DEBUG_EVENT_MANUAL_SNAPSHOT: return "Manual Snapshot";
        default: return "Unknown";
    }
}

char* debug_format_timestamp(uint64_t timestamp) {
    static char buffer[32];
    double ms = timestamp / 1000.0;
    snprintf(buffer, sizeof(buffer), "%.3fms", ms);
    return buffer;
}

char* debug_value_to_string(Type* type, const void* value) {
    if (!type || !value) return str_dup_safe("null");
    
    // Simplified value formatting - would need proper type system integration
    char* result = malloc(256);
    if (!result) return NULL;
    
    snprintf(result, 256, "<value of type %s>", type->name ? type->name : "unknown");
    return result;
}

void debug_report_error(DebugState* state, const char* message) {
    if (state && state->error_context && message) {
        SourceLocation loc = empty_source_location();
        report_error(state->error_context, ERROR_INTERNAL, message, loc);
    }
}

// =============================================================================
// Session management
// =============================================================================

bool debug_is_in_session(const DebugState* state) {
    return state && state->in_debug_session;
}

// =============================================================================
// Search functionality
// =============================================================================

DebugSnapshot* debug_find_event(DebugState* state, DebugEventType event_type, 
                                bool search_backward) {
    if (!state || !state->timeline || !state->timeline->current) return NULL;
    
    DebugSnapshot* start = state->timeline->current;
    DebugSnapshot* current = search_backward ? start->previous : start->next;
    
    while (current) {
        if (current->event_type == event_type) {
            return current;
        }
        current = search_backward ? current->previous : current->next;
    }
    
    return NULL; // Not found
}

DebugSnapshot* debug_find_variable_change(DebugState* state, const char* var_name, 
                                          bool search_backward) {
    if (!state || !state->timeline || !state->timeline->current || !var_name) return NULL;
    
    DebugSnapshot* start = state->timeline->current;
    DebugSnapshot* current = search_backward ? start->previous : start->next;
    
    while (current) {
        if (current->event_type == DEBUG_EVENT_VARIABLE_ASSIGNMENT) {
            // In a real implementation, we would check if the event description
            // contains the variable name or check stored variable information
            if (current->event_description && strstr(current->event_description, var_name)) {
                return current;
            }
        }
        current = search_backward ? current->previous : current->next;
    }
    
    return NULL; // Not found
}

DebugSnapshot* debug_find_function_call(DebugState* state, const char* function_name, 
                                        bool search_backward) {
    if (!state || !state->timeline || !state->timeline->current || !function_name) return NULL;
    
    DebugSnapshot* start = state->timeline->current;
    DebugSnapshot* current = search_backward ? start->previous : start->next;
    
    while (current) {
        if (current->event_type == DEBUG_EVENT_FUNCTION_CALL) {
            if (current->event_description && strstr(current->event_description, function_name)) {
                return current;
            }
        }
        current = search_backward ? current->previous : current->next;
    }
    
    return NULL; // Not found
}

DebugSnapshot* debug_find_location(DebugState* state, const char* filename, 
                                   size_t line, bool search_backward) {
    if (!state || !state->timeline || !state->timeline->current || !filename) return NULL;
    
    DebugSnapshot* start = state->timeline->current;
    DebugSnapshot* current = search_backward ? start->previous : start->next;
    
    while (current) {
        if (current->current_location.filename && 
            strcmp(current->current_location.filename, filename) == 0 &&
            current->current_location.line == line) {
            return current;
        }
        current = search_backward ? current->previous : current->next;
    }
    
    return NULL; // Not found
}