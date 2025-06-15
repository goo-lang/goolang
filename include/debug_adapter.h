#ifndef DEBUG_ADAPTER_H
#define DEBUG_ADAPTER_H

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

// Debug Adapter Protocol (DAP) implementation for Goo
// Provides VS Code and IDE debugging integration

// DAP message types
typedef enum {
    DAP_REQUEST,
    DAP_RESPONSE,
    DAP_EVENT
} DAPMessageType;

// DAP request types
typedef enum {
    DAP_INITIALIZE,
    DAP_LAUNCH,
    DAP_ATTACH,
    DAP_DISCONNECT,
    DAP_SET_BREAKPOINTS,
    DAP_CONTINUE,
    DAP_NEXT,
    DAP_STEP_IN,
    DAP_STEP_OUT,
    DAP_PAUSE,
    DAP_STACK_TRACE,
    DAP_SCOPES,
    DAP_VARIABLES,
    DAP_EVALUATE,
    DAP_SET_VARIABLE,
    DAP_THREADS,
    DAP_TERMINATE
} DAPRequestType;

// DAP event types
typedef enum {
    DAP_INITIALIZED,
    DAP_STOPPED,
    DAP_CONTINUED,
    DAP_EXITED,
    DAP_TERMINATED,
    DAP_THREAD,
    DAP_OUTPUT,
    DAP_BREAKPOINT,
    DAP_MODULE,
    DAP_LOADED_SOURCE,
    DAP_PROCESS,
    DAP_CAPABILITIES,
    DAP_PROGRESS_START,
    DAP_PROGRESS_UPDATE,
    DAP_PROGRESS_END,
    DAP_INVALIDATED,
    DAP_MEMORY
} DAPEventType;

// Breakpoint information
typedef struct {
    int id;
    char* source_path;
    int line;
    int column;
    char* condition;
    char* hit_condition;
    char* log_message;
    bool verified;
    bool enabled;
} DAPBreakpoint;

// Variable information for debugging
typedef struct {
    char* name;
    char* value;
    char* type;
    char* evaluate_name;
    int variables_reference;
    int named_variables;
    int indexed_variables;
    char* memory_reference;
    bool presentation_hint_lazy;
    char* presentation_hint_kind;
} DAPVariable;

// Stack frame information
typedef struct {
    int id;
    char* name;
    char* source_path;
    int line;
    int column;
    int end_line;
    int end_column;
    bool can_restart;
    char* instruction_pointer_reference;
    char* module_id;
    char* presentation_hint;
} DAPStackFrame;

// Thread information
typedef struct {
    int id;
    char* name;
} DAPThread;

// Scope information
typedef struct {
    char* name;
    char* presentation_hint;
    int variables_reference;
    int named_variables;
    int indexed_variables;
    bool expensive;
    char* source_path;
    int line;
    int column;
    int end_line;
    int end_column;
} DAPScope;

// Debug adapter server state
typedef struct {
    FILE* input_stream;
    FILE* output_stream;
    bool running;
    bool initialized;
    bool supports_configuration_done_request;
    bool supports_function_breakpoints;
    bool supports_conditional_breakpoints;
    bool supports_hit_conditional_breakpoints;
    bool supports_evaluate_for_hovers;
    bool supports_step_back;
    bool supports_set_variable;
    bool supports_restart_frame;
    bool supports_goto_targets_request;
    bool supports_step_in_targets_request;
    bool supports_completions_request;
    bool supports_modules_request;
    bool supports_restart_request;
    bool supports_exception_options;
    bool supports_value_formatting_options;
    bool supports_exception_info_request;
    bool supports_terminate_debuggee;
    bool supports_delayed_stack_trace_loading;
    bool supports_loaded_sources_request;
    bool supports_log_points;
    bool supports_terminate_threads_request;
    bool supports_set_expression;
    bool supports_terminate_request;
    bool supports_data_breakpoints;
    bool supports_read_memory_request;
    bool supports_write_memory_request;
    bool supports_disassemble_request;
    bool supports_cancel_request;
    bool supports_breakpoint_locations_request;
    bool supports_clipboard_context;
    bool supports_stepping_granularity;
    bool supports_instruction_breakpoints;
    bool supports_exception_filter_options;
    
    // Current debug state
    bool is_debugging;
    bool is_paused;
    int current_thread_id;
    int current_frame_id;
    
    // Breakpoints storage
    DAPBreakpoint* breakpoints;
    int breakpoint_count;
    int max_breakpoints;
    
    // Variables storage
    DAPVariable* variables;
    int variable_count;
    int max_variables;
    
    // Stack frames
    DAPStackFrame* stack_frames;
    int frame_count;
    int max_frames;
    
    // Threads
    DAPThread* threads;
    int thread_count;
    int max_threads;
} DAPServer;

// Core DAP server functions
bool dap_server_init(FILE* input, FILE* output);
void dap_server_run(void);
void dap_server_shutdown(void);

// Message handling
void dap_send_response(int request_seq, const char* command, bool success, const char* message, const char* body);
void dap_send_event(DAPEventType event_type, const char* body);
void dap_send_error_response(int request_seq, const char* command, const char* error_message);

// Request handlers
void dap_handle_initialize(int seq, const char* arguments);
void dap_handle_launch(int seq, const char* arguments);
void dap_handle_attach(int seq, const char* arguments);
void dap_handle_disconnect(int seq, const char* arguments);
void dap_handle_set_breakpoints(int seq, const char* arguments);
void dap_handle_continue(int seq, const char* arguments);
void dap_handle_next(int seq, const char* arguments);
void dap_handle_step_in(int seq, const char* arguments);
void dap_handle_step_out(int seq, const char* arguments);
void dap_handle_pause(int seq, const char* arguments);
void dap_handle_stack_trace(int seq, const char* arguments);
void dap_handle_scopes(int seq, const char* arguments);
void dap_handle_variables(int seq, const char* arguments);
void dap_handle_evaluate(int seq, const char* arguments);
void dap_handle_set_variable(int seq, const char* arguments);
void dap_handle_threads(int seq, const char* arguments);
void dap_handle_terminate(int seq, const char* arguments);

// Breakpoint management
int dap_add_breakpoint(const char* source_path, int line, const char* condition);
bool dap_remove_breakpoint(int breakpoint_id);
bool dap_enable_breakpoint(int breakpoint_id, bool enabled);
DAPBreakpoint* dap_find_breakpoint(const char* source_path, int line);
void dap_verify_breakpoints(void);

// Variable management
int dap_add_variable(const char* name, const char* value, const char* type, int parent_reference);
DAPVariable* dap_get_variable(int variables_reference);
DAPVariable* dap_get_variables_in_scope(int scope_reference, int* count);
bool dap_set_variable_value(int variables_reference, const char* name, const char* value);

// Stack frame management
void dap_update_stack_trace(void);
DAPStackFrame* dap_get_current_frame(void);
DAPStackFrame* dap_get_frame(int frame_id);

// Thread management
int dap_add_thread(const char* name);
void dap_remove_thread(int thread_id);
DAPThread* dap_get_current_thread(void);

// Execution control integration with time-travel debugging
void dap_notify_stopped(const char* reason, const char* description, int thread_id, bool preserve_focus_hint, const char* text);
void dap_notify_continued(int thread_id, bool all_threads_continued);
void dap_notify_exited(int exit_code);
void dap_notify_terminated(bool restart);

// Output and logging
void dap_send_output(const char* category, const char* output, const char* source_path, int line);
void dap_log_message(const char* level, const char* message);

// Goo-specific debugging features
void dap_handle_goo_error_union_inspection(int seq, const char* arguments);
void dap_handle_goo_ownership_tracking(int seq, const char* arguments);
void dap_handle_goo_channel_inspection(int seq, const char* arguments);
void dap_handle_goo_memory_safety_check(int seq, const char* arguments);

// Integration with existing debugging infrastructure
void dap_integrate_time_travel_debug(void);
void dap_integrate_repl_commands(void);
void dap_integrate_performance_monitoring(void);

// Utility functions
char* dap_escape_json_string(const char* str);
void dap_parse_json_message(const char* message);
int dap_get_next_sequence_number(void);

#endif // DEBUG_ADAPTER_H