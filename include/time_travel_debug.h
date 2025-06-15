#ifndef GOO_TIME_TRAVEL_DEBUG_H
#define GOO_TIME_TRAVEL_DEBUG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "errors/error.h"
#include "ast.h"
#include "types.h"

// Forward declarations
typedef struct REPLContext REPLContext;
typedef struct DebugState DebugState;
typedef struct DebugSnapshot DebugSnapshot;
typedef struct DebugTimeline DebugTimeline;

// =============================================================================
// Debug State Tracking
// =============================================================================

// Types of debug events that create snapshots
typedef enum {
    DEBUG_EVENT_EXPRESSION_START,
    DEBUG_EVENT_EXPRESSION_END,
    DEBUG_EVENT_FUNCTION_CALL,
    DEBUG_EVENT_FUNCTION_RETURN,
    DEBUG_EVENT_VARIABLE_ASSIGNMENT,
    DEBUG_EVENT_CONTROL_FLOW,
    DEBUG_EVENT_ERROR_OCCURRED,
    DEBUG_EVENT_BREAKPOINT,
    DEBUG_EVENT_MANUAL_SNAPSHOT
} DebugEventType;

// Variable state at a point in time
typedef struct DebugVariable {
    char* name;
    Type* type;
    void* value;        // Serialized value data
    size_t value_size;
    char* string_repr;  // Human-readable representation
    struct DebugVariable* next;
} DebugVariable;

// Call stack frame information
typedef struct DebugStackFrame {
    char* function_name;
    SourceLocation location;
    DebugVariable* local_variables;
    struct DebugStackFrame* parent;
    struct DebugStackFrame* next;
} DebugStackFrame;

// Complete program state snapshot
struct DebugSnapshot {
    uint64_t timestamp;                 // Execution timestamp
    uint64_t step_count;               // Step number in execution
    DebugEventType event_type;         // What triggered this snapshot
    char* event_description;           // Human-readable event description
    
    // Program state
    DebugStackFrame* call_stack;       // Current call stack
    DebugVariable* global_variables;   // Global variable state
    char* current_expression;          // Current expression being evaluated
    SourceLocation current_location;   // Current source location
    
    // Execution context
    ASTNode* current_ast;              // Current AST node
    void* execution_context;           // Runtime execution state
    
    // Memory state
    size_t heap_size;                  // Current heap usage
    size_t stack_size;                 // Current stack usage
    char* memory_layout;               // Serialized memory layout
    
    // Performance metrics
    double cpu_time;                   // CPU time used
    uint64_t instruction_count;        // Instructions executed
    
    // Navigation
    struct DebugSnapshot* previous;
    struct DebugSnapshot* next;
    uint64_t snapshot_id;
};

// Debug timeline - manages the sequence of snapshots
struct DebugTimeline {
    DebugSnapshot* head;               // First snapshot (oldest)
    DebugSnapshot* tail;               // Last snapshot (newest)
    DebugSnapshot* current;            // Current position in timeline
    size_t snapshot_count;
    size_t max_snapshots;              // Maximum snapshots to keep
    uint64_t next_snapshot_id;
    
    // Timeline statistics
    uint64_t total_steps;
    double total_execution_time;
    size_t peak_memory_usage;
};

// Main debug state
struct DebugState {
    DebugTimeline* timeline;
    bool enabled;                      // Is time-travel debugging enabled
    bool auto_snapshot;                // Automatic snapshot creation
    size_t snapshot_frequency;         // Steps between auto snapshots
    
    // Snapshot filtering
    bool snapshot_on_errors;
    bool snapshot_on_function_calls;
    bool snapshot_on_assignments;
    bool snapshot_on_control_flow;
    
    // Current debugging session
    bool in_debug_session;
    char* session_name;
    uint64_t session_start_time;
    
    // Integration with other systems
    ErrorContext* error_context;
    REPLContext* repl_context;
};

// =============================================================================
// Debug State Management
// =============================================================================

// Create and destroy debug state
DebugState* debug_state_new(void);
void debug_state_free(DebugState* state);

// Timeline management
DebugTimeline* debug_timeline_new(size_t max_snapshots);
void debug_timeline_free(DebugTimeline* timeline);
void debug_timeline_clear(DebugTimeline* timeline);

// Snapshot creation and management
DebugSnapshot* debug_snapshot_create(DebugState* state, DebugEventType event_type, 
                                     const char* description);
void debug_snapshot_free(DebugSnapshot* snapshot);
bool debug_snapshot_add(DebugTimeline* timeline, DebugSnapshot* snapshot);

// Variable state tracking
DebugVariable* debug_variable_create(const char* name, Type* type, 
                                     const void* value, size_t value_size);
void debug_variable_free(DebugVariable* var);
void debug_variable_list_free(DebugVariable* vars);

// Stack frame management
DebugStackFrame* debug_stack_frame_create(const char* function_name, 
                                          SourceLocation location);
void debug_stack_frame_free(DebugStackFrame* frame);
void debug_stack_frame_add_variable(DebugStackFrame* frame, DebugVariable* var);

// =============================================================================
// Time-Travel Navigation
// =============================================================================

// Navigation commands
bool debug_step_forward(DebugState* state);
bool debug_step_backward(DebugState* state);
bool debug_continue_forward(DebugState* state);
bool debug_continue_backward(DebugState* state);
bool debug_goto_step(DebugState* state, uint64_t step_number);
bool debug_goto_timestamp(DebugState* state, uint64_t timestamp);
bool debug_goto_snapshot(DebugState* state, uint64_t snapshot_id);

// Position queries
uint64_t debug_get_current_step(const DebugState* state);
uint64_t debug_get_current_timestamp(const DebugState* state);
DebugSnapshot* debug_get_current_snapshot(const DebugState* state);
bool debug_is_at_beginning(const DebugState* state);
bool debug_is_at_end(const DebugState* state);

// Search functionality
DebugSnapshot* debug_find_event(DebugState* state, DebugEventType event_type, 
                                bool search_backward);
DebugSnapshot* debug_find_variable_change(DebugState* state, const char* var_name, 
                                          bool search_backward);
DebugSnapshot* debug_find_function_call(DebugState* state, const char* function_name, 
                                        bool search_backward);
DebugSnapshot* debug_find_location(DebugState* state, const char* filename, 
                                   size_t line, bool search_backward);

// =============================================================================
// State Inspection
// =============================================================================

// Variable inspection
DebugVariable* debug_get_variable(const DebugSnapshot* snapshot, const char* name);
DebugVariable* debug_get_local_variable(const DebugSnapshot* snapshot, const char* name);
DebugVariable* debug_get_global_variable(const DebugSnapshot* snapshot, const char* name);
char* debug_format_variable_value(const DebugVariable* var);

// Call stack inspection
size_t debug_get_stack_depth(const DebugSnapshot* snapshot);
DebugStackFrame* debug_get_current_frame(const DebugSnapshot* snapshot);
DebugStackFrame* debug_get_frame_at_depth(const DebugSnapshot* snapshot, size_t depth);

// Memory inspection
size_t debug_get_heap_usage(const DebugSnapshot* snapshot);
size_t debug_get_stack_usage(const DebugSnapshot* snapshot);
char* debug_format_memory_layout(const DebugSnapshot* snapshot);

// Program state inspection
char* debug_get_current_expression(const DebugSnapshot* snapshot);
SourceLocation debug_get_current_location(const DebugSnapshot* snapshot);
ASTNode* debug_get_current_ast(const DebugSnapshot* snapshot);

// =============================================================================
// Event Recording
// =============================================================================

// Automatic event recording
void debug_record_expression_start(DebugState* state, const char* expression, 
                                   SourceLocation location);
void debug_record_expression_end(DebugState* state, const char* expression, 
                                 void* result);
void debug_record_function_call(DebugState* state, const char* function_name, 
                                SourceLocation location);
void debug_record_function_return(DebugState* state, const char* function_name, 
                                  void* return_value);
void debug_record_variable_assignment(DebugState* state, const char* var_name, 
                                      Type* type, const void* value, size_t value_size);
void debug_record_control_flow(DebugState* state, const char* flow_type, 
                               SourceLocation location);
void debug_record_error(DebugState* state, const Error* error);

// Manual event recording
void debug_create_manual_snapshot(DebugState* state, const char* description);
void debug_set_breakpoint(DebugState* state, SourceLocation location);
void debug_remove_breakpoint(DebugState* state, SourceLocation location);

// =============================================================================
// Configuration and Settings
// =============================================================================

// Enable/disable time-travel debugging
void debug_enable(DebugState* state);
void debug_disable(DebugState* state);
bool debug_is_enabled(const DebugState* state);

// Configure snapshot behavior
void debug_set_auto_snapshot(DebugState* state, bool enabled);
void debug_set_snapshot_frequency(DebugState* state, size_t frequency);
void debug_set_max_snapshots(DebugState* state, size_t max_snapshots);

// Configure event filtering
void debug_set_snapshot_on_errors(DebugState* state, bool enabled);
void debug_set_snapshot_on_function_calls(DebugState* state, bool enabled);
void debug_set_snapshot_on_assignments(DebugState* state, bool enabled);
void debug_set_snapshot_on_control_flow(DebugState* state, bool enabled);

// Session management
bool debug_start_session(DebugState* state, const char* session_name);
void debug_end_session(DebugState* state);
bool debug_is_in_session(const DebugState* state);

// =============================================================================
// Visualization and Display
// =============================================================================

// Timeline visualization
void debug_print_timeline(const DebugState* state);
void debug_print_timeline_summary(const DebugState* state);
char* debug_format_timeline_graph(const DebugState* state, size_t width);

// State visualization
void debug_print_current_state(const DebugState* state);
void debug_print_call_stack(const DebugSnapshot* snapshot);
void debug_print_variables(const DebugSnapshot* snapshot);
void debug_print_memory_usage(const DebugSnapshot* snapshot);

// Comparison between states
void debug_compare_snapshots(const DebugSnapshot* snapshot1, 
                             const DebugSnapshot* snapshot2);
char* debug_format_state_diff(const DebugSnapshot* before, 
                              const DebugSnapshot* after);

// =============================================================================
// REPL Integration
// =============================================================================

// REPL command handling
int repl_cmd_debug_step_back(REPLContext* ctx, const char* args);
int repl_cmd_debug_step_forward(REPLContext* ctx, const char* args);
int repl_cmd_debug_continue_back(REPLContext* ctx, const char* args);
int repl_cmd_debug_continue_forward(REPLContext* ctx, const char* args);
int repl_cmd_debug_goto(REPLContext* ctx, const char* args);
int repl_cmd_debug_timeline(REPLContext* ctx, const char* args);
int repl_cmd_debug_state(REPLContext* ctx, const char* args);
int repl_cmd_debug_variables(REPLContext* ctx, const char* args);
int repl_cmd_debug_stack(REPLContext* ctx, const char* args);
int repl_cmd_debug_find(REPLContext* ctx, const char* args);
int repl_cmd_debug_snapshot(REPLContext* ctx, const char* args);
int repl_cmd_debug_enable(REPLContext* ctx, const char* args);
int repl_cmd_debug_disable(REPLContext* ctx, const char* args);

// Initialize debug system in REPL
bool repl_debug_init(REPLContext* ctx);
void repl_debug_cleanup(REPLContext* ctx);

// =============================================================================
// Utility Functions
// =============================================================================

// String formatting helpers
char* debug_format_timestamp(uint64_t timestamp);
char* debug_format_step_count(uint64_t step_count);
char* debug_format_event_type(DebugEventType event_type);
char* debug_format_location(const SourceLocation* location);

// Value serialization
void* debug_serialize_value(Type* type, const void* value, size_t* out_size);
void* debug_deserialize_value(Type* type, const void* data, size_t size);
char* debug_value_to_string(Type* type, const void* value);

// Memory management helpers
void debug_cleanup_old_snapshots(DebugTimeline* timeline);
size_t debug_calculate_memory_usage(const DebugState* state);
void debug_optimize_memory_usage(DebugState* state);

// Error handling
void debug_report_error(DebugState* state, const char* message);
bool debug_validate_state(const DebugState* state);

#endif // GOO_TIME_TRAVEL_DEBUG_H