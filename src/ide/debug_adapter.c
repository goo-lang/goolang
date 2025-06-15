#include "debug_adapter.h"
#include "time_travel_debug.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

// Global debug adapter server instance
static DAPServer* g_dap_server = NULL;
static int g_sequence_number = 1;

// Initialize the debug adapter server
bool dap_server_init(FILE* input, FILE* output) {
    g_dap_server = malloc(sizeof(DAPServer));
    if (!g_dap_server) return false;
    
    memset(g_dap_server, 0, sizeof(DAPServer));
    
    g_dap_server->input_stream = input;
    g_dap_server->output_stream = output;
    g_dap_server->running = true;
    g_dap_server->initialized = false;
    
    // Set capabilities
    g_dap_server->supports_configuration_done_request = true;
    g_dap_server->supports_function_breakpoints = true;
    g_dap_server->supports_conditional_breakpoints = true;
    g_dap_server->supports_hit_conditional_breakpoints = true;
    g_dap_server->supports_evaluate_for_hovers = true;
    g_dap_server->supports_step_back = true; // Goo supports time-travel debugging
    g_dap_server->supports_set_variable = true;
    g_dap_server->supports_restart_frame = false;
    g_dap_server->supports_goto_targets_request = false;
    g_dap_server->supports_step_in_targets_request = true;
    g_dap_server->supports_completions_request = true;
    g_dap_server->supports_modules_request = true;
    g_dap_server->supports_restart_request = true;
    g_dap_server->supports_exception_options = true;
    g_dap_server->supports_value_formatting_options = true;
    g_dap_server->supports_exception_info_request = true;
    g_dap_server->supports_terminate_debuggee = true;
    g_dap_server->supports_delayed_stack_trace_loading = false;
    g_dap_server->supports_loaded_sources_request = true;
    g_dap_server->supports_log_points = true;
    g_dap_server->supports_terminate_threads_request = true;
    g_dap_server->supports_set_expression = true;
    g_dap_server->supports_terminate_request = true;
    g_dap_server->supports_data_breakpoints = false;
    g_dap_server->supports_read_memory_request = true;
    g_dap_server->supports_write_memory_request = false;
    g_dap_server->supports_disassemble_request = false;
    g_dap_server->supports_cancel_request = true;
    g_dap_server->supports_breakpoint_locations_request = true;
    g_dap_server->supports_clipboard_context = false;
    g_dap_server->supports_stepping_granularity = true;
    g_dap_server->supports_instruction_breakpoints = false;
    g_dap_server->supports_exception_filter_options = true;
    
    // Initialize storage
    g_dap_server->max_breakpoints = 100;
    g_dap_server->breakpoints = malloc(sizeof(DAPBreakpoint) * g_dap_server->max_breakpoints);
    
    g_dap_server->max_variables = 1000;
    g_dap_server->variables = malloc(sizeof(DAPVariable) * g_dap_server->max_variables);
    
    g_dap_server->max_frames = 100;
    g_dap_server->stack_frames = malloc(sizeof(DAPStackFrame) * g_dap_server->max_frames);
    
    g_dap_server->max_threads = 10;
    g_dap_server->threads = malloc(sizeof(DAPThread) * g_dap_server->max_threads);
    
    // Initialize current state
    g_dap_server->current_thread_id = 1;
    g_dap_server->current_frame_id = 0;
    
    return true;
}

// Send a DAP response message
void dap_send_response(int request_seq, const char* command, bool success, const char* message, const char* body) {
    if (!g_dap_server || !g_dap_server->output_stream) return;
    
    char response[4096];
    
    if (body && strlen(body) > 0) {
        snprintf(response, sizeof(response),
            "{"
                "\"seq\":%d,"
                "\"type\":\"response\","
                "\"request_seq\":%d,"
                "\"success\":%s,"
                "\"command\":\"%s\","
                "\"message\":\"%s\","
                "\"body\":%s"
            "}",
            g_sequence_number++, request_seq, success ? "true" : "false", 
            command, message ? message : "", body);
    } else {
        snprintf(response, sizeof(response),
            "{"
                "\"seq\":%d,"
                "\"type\":\"response\","
                "\"request_seq\":%d,"
                "\"success\":%s,"
                "\"command\":\"%s\","
                "\"message\":\"%s\""
            "}",
            g_sequence_number++, request_seq, success ? "true" : "false", 
            command, message ? message : "");
    }
    
    int content_length = strlen(response);
    fprintf(g_dap_server->output_stream, "Content-Length: %d\r\n\r\n%s", content_length, response);
    fflush(g_dap_server->output_stream);
}

// Send a DAP event message
void dap_send_event(DAPEventType event_type, const char* body) {
    if (!g_dap_server || !g_dap_server->output_stream) return;
    
    const char* event_name = "unknown";
    switch (event_type) {
        case DAP_INITIALIZED: event_name = "initialized"; break;
        case DAP_STOPPED: event_name = "stopped"; break;
        case DAP_CONTINUED: event_name = "continued"; break;
        case DAP_EXITED: event_name = "exited"; break;
        case DAP_TERMINATED: event_name = "terminated"; break;
        case DAP_THREAD: event_name = "thread"; break;
        case DAP_OUTPUT: event_name = "output"; break;
        case DAP_BREAKPOINT: event_name = "breakpoint"; break;
        case DAP_MODULE: event_name = "module"; break;
        case DAP_LOADED_SOURCE: event_name = "loadedSource"; break;
        case DAP_PROCESS: event_name = "process"; break;
        case DAP_CAPABILITIES: event_name = "capabilities"; break;
        case DAP_PROGRESS_START: event_name = "progressStart"; break;
        case DAP_PROGRESS_UPDATE: event_name = "progressUpdate"; break;
        case DAP_PROGRESS_END: event_name = "progressEnd"; break;
        case DAP_INVALIDATED: event_name = "invalidated"; break;
        case DAP_MEMORY: event_name = "memory"; break;
        default: break;
    }
    
    char event[4096];
    if (body && strlen(body) > 0) {
        snprintf(event, sizeof(event),
            "{"
                "\"seq\":%d,"
                "\"type\":\"event\","
                "\"event\":\"%s\","
                "\"body\":%s"
            "}",
            g_sequence_number++, event_name, body);
    } else {
        snprintf(event, sizeof(event),
            "{"
                "\"seq\":%d,"
                "\"type\":\"event\","
                "\"event\":\"%s\""
            "}",
            g_sequence_number++, event_name);
    }
    
    int content_length = strlen(event);
    fprintf(g_dap_server->output_stream, "Content-Length: %d\r\n\r\n%s", content_length, event);
    fflush(g_dap_server->output_stream);
}

// Handle initialize request
void dap_handle_initialize(int seq, const char* arguments) {
    (void)arguments; // Parse client capabilities if needed
    
    char capabilities[2048];
    snprintf(capabilities, sizeof(capabilities),
        "{"
            "\"supportsConfigurationDoneRequest\":%s,"
            "\"supportsFunctionBreakpoints\":%s,"
            "\"supportsConditionalBreakpoints\":%s,"
            "\"supportsHitConditionalBreakpoints\":%s,"
            "\"supportsEvaluateForHovers\":%s,"
            "\"supportsStepBack\":%s,"
            "\"supportsSetVariable\":%s,"
            "\"supportsRestartFrame\":%s,"
            "\"supportsGotoTargetsRequest\":%s,"
            "\"supportsStepInTargetsRequest\":%s,"
            "\"supportsCompletionsRequest\":%s,"
            "\"supportsModulesRequest\":%s,"
            "\"supportsRestartRequest\":%s,"
            "\"supportsExceptionOptions\":%s,"
            "\"supportsValueFormattingOptions\":%s,"
            "\"supportsExceptionInfoRequest\":%s,"
            "\"supportTerminateDebuggee\":%s,"
            "\"supportsDelayedStackTraceLoading\":%s,"
            "\"supportsLoadedSourcesRequest\":%s,"
            "\"supportsLogPoints\":%s,"
            "\"supportsTerminateThreadsRequest\":%s,"
            "\"supportsSetExpression\":%s,"
            "\"supportsTerminateRequest\":%s,"
            "\"supportsDataBreakpoints\":%s,"
            "\"supportsReadMemoryRequest\":%s,"
            "\"supportsWriteMemoryRequest\":%s,"
            "\"supportsDisassembleRequest\":%s,"
            "\"supportsCancelRequest\":%s,"
            "\"supportsBreakpointLocationsRequest\":%s,"
            "\"supportsClipboardContext\":%s,"
            "\"supportsSteppingGranularity\":%s,"
            "\"supportsInstructionBreakpoints\":%s,"
            "\"supportsExceptionFilterOptions\":%s,"
            "\"exceptionBreakpointFilters\":["
                "{"
                    "\"filter\":\"error_unions\","
                    "\"label\":\"Error Union Exceptions\","
                    "\"description\":\"Break when error unions propagate\","
                    "\"default\":false"
                "},"
                "{"
                    "\"filter\":\"panics\","
                    "\"label\":\"Panic Exceptions\","
                    "\"description\":\"Break on panic calls\","
                    "\"default\":true"
                "},"
                "{"
                    "\"filter\":\"memory_errors\","
                    "\"label\":\"Memory Safety Violations\","
                    "\"description\":\"Break on memory safety violations\","
                    "\"default\":true"
                "}"
            "]"
        "}",
        g_dap_server->supports_configuration_done_request ? "true" : "false",
        g_dap_server->supports_function_breakpoints ? "true" : "false",
        g_dap_server->supports_conditional_breakpoints ? "true" : "false",
        g_dap_server->supports_hit_conditional_breakpoints ? "true" : "false",
        g_dap_server->supports_evaluate_for_hovers ? "true" : "false",
        g_dap_server->supports_step_back ? "true" : "false",
        g_dap_server->supports_set_variable ? "true" : "false",
        g_dap_server->supports_restart_frame ? "true" : "false",
        g_dap_server->supports_goto_targets_request ? "true" : "false",
        g_dap_server->supports_step_in_targets_request ? "true" : "false",
        g_dap_server->supports_completions_request ? "true" : "false",
        g_dap_server->supports_modules_request ? "true" : "false",
        g_dap_server->supports_restart_request ? "true" : "false",
        g_dap_server->supports_exception_options ? "true" : "false",
        g_dap_server->supports_value_formatting_options ? "true" : "false",
        g_dap_server->supports_exception_info_request ? "true" : "false",
        g_dap_server->supports_terminate_debuggee ? "true" : "false",
        g_dap_server->supports_delayed_stack_trace_loading ? "true" : "false",
        g_dap_server->supports_loaded_sources_request ? "true" : "false",
        g_dap_server->supports_log_points ? "true" : "false",
        g_dap_server->supports_terminate_threads_request ? "true" : "false",
        g_dap_server->supports_set_expression ? "true" : "false",
        g_dap_server->supports_terminate_request ? "true" : "false",
        g_dap_server->supports_data_breakpoints ? "true" : "false",
        g_dap_server->supports_read_memory_request ? "true" : "false",
        g_dap_server->supports_write_memory_request ? "true" : "false",
        g_dap_server->supports_disassemble_request ? "true" : "false",
        g_dap_server->supports_cancel_request ? "true" : "false",
        g_dap_server->supports_breakpoint_locations_request ? "true" : "false",
        g_dap_server->supports_clipboard_context ? "true" : "false",
        g_dap_server->supports_stepping_granularity ? "true" : "false",
        g_dap_server->supports_instruction_breakpoints ? "true" : "false",
        g_dap_server->supports_exception_filter_options ? "true" : "false");
    
    dap_send_response(seq, "initialize", true, NULL, capabilities);
    g_dap_server->initialized = true;
    
    // Send initialized event
    dap_send_event(DAP_INITIALIZED, NULL);
}

// Handle launch request
void dap_handle_launch(int seq, const char* arguments) {
    (void)arguments; // Parse launch configuration
    
    // Initialize debugging session
    g_dap_server->is_debugging = true;
    g_dap_server->is_paused = false;
    
    // Add main thread
    dap_add_thread("main");
    
    // Initialize time-travel debugging integration
    dap_integrate_time_travel_debug();
    
    dap_send_response(seq, "launch", true, NULL, NULL);
    
    // Send process event
    char process_info[512];
    snprintf(process_info, sizeof(process_info),
        "{"
            "\"name\":\"goo-debug\","
            "\"systemProcessId\":12345,"
            "\"isLocalProcess\":true,"
            "\"startMethod\":\"launch\""
        "}");
    dap_send_event(DAP_PROCESS, process_info);
    
    // Send thread event
    char thread_info[256];
    snprintf(thread_info, sizeof(thread_info),
        "{"
            "\"reason\":\"started\","
            "\"threadId\":%d"
        "}", g_dap_server->current_thread_id);
    dap_send_event(DAP_THREAD, thread_info);
}

// Handle set breakpoints request
void dap_handle_set_breakpoints(int seq, const char* arguments) {
    // Parse source path and breakpoint locations from arguments
    // For demo, add a simple breakpoint
    
    const char* source_path = "test.goo"; // Extract from arguments
    int line = 10; // Extract from arguments
    
    int bp_id = dap_add_breakpoint(source_path, line, NULL);
    
    char breakpoints_response[1024];
    snprintf(breakpoints_response, sizeof(breakpoints_response),
        "{"
            "\"breakpoints\":["
                "{"
                    "\"id\":%d,"
                    "\"verified\":true,"
                    "\"line\":%d,"
                    "\"source\":{"
                        "\"name\":\"test.goo\","
                        "\"path\":\"/path/to/test.goo\""
                    "}"
                "}"
            "]"
        "}", bp_id, line);
    
    dap_send_response(seq, "setBreakpoints", true, NULL, breakpoints_response);
}

// Handle continue request
void dap_handle_continue(int seq, const char* arguments) {
    (void)arguments;
    
    g_dap_server->is_paused = false;
    
    char continue_response[256];
    snprintf(continue_response, sizeof(continue_response),
        "{"
            "\"allThreadsContinued\":true"
        "}");
    
    dap_send_response(seq, "continue", true, NULL, continue_response);
    
    // Send continued event
    dap_notify_continued(g_dap_server->current_thread_id, true);
}

// Handle next (step over) request
void dap_handle_next(int seq, const char* arguments) {
    (void)arguments;
    
    // Integrate with time-travel debugging for step over
    // time_travel_debug_step_forward();
    
    dap_send_response(seq, "next", true, NULL, NULL);
    
    // Simulate hitting next line
    dap_notify_stopped("step", "Stepped to next line", g_dap_server->current_thread_id, false, NULL);
}

// Handle step in request
void dap_handle_step_in(int seq, const char* arguments) {
    (void)arguments;
    
    dap_send_response(seq, "stepIn", true, NULL, NULL);
    
    // Simulate stepping into function
    dap_notify_stopped("step", "Stepped into function", g_dap_server->current_thread_id, false, NULL);
}

// Handle step out request
void dap_handle_step_out(int seq, const char* arguments) {
    (void)arguments;
    
    dap_send_response(seq, "stepOut", true, NULL, NULL);
    
    // Simulate stepping out of function
    dap_notify_stopped("step", "Stepped out of function", g_dap_server->current_thread_id, false, NULL);
}

// Handle threads request
void dap_handle_threads(int seq, const char* arguments) {
    (void)arguments;
    
    char threads_response[1024];
    snprintf(threads_response, sizeof(threads_response),
        "{"
            "\"threads\":["
                "{"
                    "\"id\":%d,"
                    "\"name\":\"%s\""
                "}"
            "]"
        "}", 
        g_dap_server->threads[0].id, 
        g_dap_server->threads[0].name ? g_dap_server->threads[0].name : "main");
    
    dap_send_response(seq, "threads", true, NULL, threads_response);
}

// Handle stack trace request
void dap_handle_stack_trace(int seq, const char* arguments) {
    (void)arguments;
    
    // Update stack trace from current execution state
    dap_update_stack_trace();
    
    char stack_trace_response[2048];
    snprintf(stack_trace_response, sizeof(stack_trace_response),
        "{"
            "\"stackFrames\":["
                "{"
                    "\"id\":0,"
                    "\"name\":\"main\","
                    "\"source\":{"
                        "\"name\":\"test.goo\","
                        "\"path\":\"/path/to/test.goo\""
                    "},"
                    "\"line\":10,"
                    "\"column\":1,"
                    "\"endLine\":10,"
                    "\"endColumn\":20"
                "},"
                "{"
                    "\"id\":1,"
                    "\"name\":\"calculate\","
                    "\"source\":{"
                        "\"name\":\"test.goo\","
                        "\"path\":\"/path/to/test.goo\""
                    "},"
                    "\"line\":25,"
                    "\"column\":5,"
                    "\"endLine\":25,"
                    "\"endColumn\":15"
                "}"
            "],"
            "\"totalFrames\":2"
        "}");
    
    dap_send_response(seq, "stackTrace", true, NULL, stack_trace_response);
}

// Handle scopes request
void dap_handle_scopes(int seq, const char* arguments) {
    (void)arguments; // Parse frame ID from arguments
    
    char scopes_response[1024];
    snprintf(scopes_response, sizeof(scopes_response),
        "{"
            "\"scopes\":["
                "{"
                    "\"name\":\"Local\","
                    "\"variablesReference\":1,"
                    "\"expensive\":false"
                "},"
                "{"
                    "\"name\":\"Global\","
                    "\"variablesReference\":2,"
                    "\"expensive\":false"
                "},"
                "{"
                    "\"name\":\"Goo Error Unions\","
                    "\"variablesReference\":3,"
                    "\"expensive\":false,"
                    "\"presentationHint\":\"registers\""
                "},"
                "{"
                    "\"name\":\"Channel State\","
                    "\"variablesReference\":4,"
                    "\"expensive\":false,"
                    "\"presentationHint\":\"registers\""
                "}"
            "]"
        "}");
    
    dap_send_response(seq, "scopes", true, NULL, scopes_response);
}

// Handle variables request
void dap_handle_variables(int seq, const char* arguments) {
    (void)arguments; // Parse variables reference from arguments
    
    char variables_response[2048];
    snprintf(variables_response, sizeof(variables_response),
        "{"
            "\"variables\":["
                "{"
                    "\"name\":\"counter\","
                    "\"value\":\"42\","
                    "\"type\":\"int\","
                    "\"variablesReference\":0"
                "},"
                "{"
                    "\"name\":\"message\","
                    "\"value\":\"\\\"Hello, Goo!\\\"\","
                    "\"type\":\"string\","
                    "\"variablesReference\":0"
                "},"
                "{"
                    "\"name\":\"result\","
                    "\"value\":\"Ok(3.14)\","
                    "\"type\":\"!float64\","
                    "\"variablesReference\":5,"
                    "\"presentationHint\":{"
                        "\"kind\":\"property\","
                        "\"attributes\":[\"readOnly\"]"
                    "}"
                "},"
                "{"
                    "\"name\":\"optional_data\","
                    "\"value\":\"Some(\\\"data\\\")\","
                    "\"type\":\"?string\","
                    "\"variablesReference\":6"
                "},"
                "{"
                    "\"name\":\"ch\","
                    "\"value\":\"chan int (buffered, 3/10)\","
                    "\"type\":\"chan int\","
                    "\"variablesReference\":7,"
                    "\"presentationHint\":{"
                        "\"kind\":\"property\","
                        "\"attributes\":[\"readOnly\"]"
                    "}"
                "}"
            "]"
        "}");
    
    dap_send_response(seq, "variables", true, NULL, variables_response);
}

// Handle evaluate request
void dap_handle_evaluate(int seq, const char* arguments) {
    // Parse expression and context from arguments
    const char* expression = "counter + 1"; // Extract from arguments
    
    char evaluate_response[512];
    snprintf(evaluate_response, sizeof(evaluate_response),
        "{"
            "\"result\":\"43\","
            "\"type\":\"int\","
            "\"variablesReference\":0,"
            "\"presentationHint\":{"
                "\"kind\":\"property\""
            "}"
        "}");
    
    dap_send_response(seq, "evaluate", true, NULL, evaluate_response);
}

// Add a breakpoint
int dap_add_breakpoint(const char* source_path, int line, const char* condition) {
    if (!g_dap_server || g_dap_server->breakpoint_count >= g_dap_server->max_breakpoints) {
        return -1;
    }
    
    DAPBreakpoint* bp = &g_dap_server->breakpoints[g_dap_server->breakpoint_count];
    bp->id = g_dap_server->breakpoint_count + 1;
    bp->source_path = strdup(source_path);
    bp->line = line;
    bp->column = 0;
    bp->condition = condition ? strdup(condition) : NULL;
    bp->hit_condition = NULL;
    bp->log_message = NULL;
    bp->verified = true;
    bp->enabled = true;
    
    g_dap_server->breakpoint_count++;
    return bp->id;
}

// Add a thread
int dap_add_thread(const char* name) {
    if (!g_dap_server || g_dap_server->thread_count >= g_dap_server->max_threads) {
        return -1;
    }
    
    DAPThread* thread = &g_dap_server->threads[g_dap_server->thread_count];
    thread->id = g_dap_server->thread_count + 1;
    thread->name = strdup(name);
    
    g_dap_server->thread_count++;
    return thread->id;
}

// Update stack trace
void dap_update_stack_trace(void) {
    // Integration point with time-travel debugging
    // Get current execution state and build stack frames
    
    if (!g_dap_server || g_dap_server->frame_count >= g_dap_server->max_frames) {
        return;
    }
    
    // Demo stack frame
    DAPStackFrame* frame = &g_dap_server->stack_frames[0];
    frame->id = 0;
    frame->name = strdup("main");
    frame->source_path = strdup("/path/to/test.goo");
    frame->line = 10;
    frame->column = 1;
    frame->end_line = 10;
    frame->end_column = 20;
    frame->can_restart = false;
    
    g_dap_server->frame_count = 1;
}

// Notify that execution stopped
void dap_notify_stopped(const char* reason, const char* description, int thread_id, bool preserve_focus_hint, const char* text) {
    g_dap_server->is_paused = true;
    
    char stopped_event[1024];
    snprintf(stopped_event, sizeof(stopped_event),
        "{"
            "\"reason\":\"%s\","
            "\"description\":\"%s\","
            "\"threadId\":%d,"
            "\"preserveFocusHint\":%s,"
            "\"text\":\"%s\","
            "\"allThreadsStopped\":true"
        "}",
        reason, description ? description : "", thread_id,
        preserve_focus_hint ? "true" : "false",
        text ? text : "");
    
    dap_send_event(DAP_STOPPED, stopped_event);
}

// Notify that execution continued
void dap_notify_continued(int thread_id, bool all_threads_continued) {
    char continued_event[256];
    snprintf(continued_event, sizeof(continued_event),
        "{"
            "\"threadId\":%d,"
            "\"allThreadsContinued\":%s"
        "}",
        thread_id, all_threads_continued ? "true" : "false");
    
    dap_send_event(DAP_CONTINUED, continued_event);
}

// Send output to debug console
void dap_send_output(const char* category, const char* output, const char* source_path, int line) {
    char output_event[1024];
    
    if (source_path && line > 0) {
        snprintf(output_event, sizeof(output_event),
            "{"
                "\"category\":\"%s\","
                "\"output\":\"%s\","
                "\"source\":{"
                    "\"path\":\"%s\""
                "},"
                "\"line\":%d"
            "}",
            category, output, source_path, line);
    } else {
        snprintf(output_event, sizeof(output_event),
            "{"
                "\"category\":\"%s\","
                "\"output\":\"%s\""
            "}",
            category, output);
    }
    
    dap_send_event(DAP_OUTPUT, output_event);
}

// Integration with time-travel debugging
void dap_integrate_time_travel_debug(void) {
    // Initialize time-travel debugging system
    // Set up callbacks for state capture
    // Configure snapshot points
}

// Simple message parsing and dispatch
static void dap_process_message(const char* content) {
    if (!content) return;
    
    // Parse JSON-RPC like structure
    char command[128] = "";
    int seq = 0;
    char arguments[2048] = "";
    
    // Simple parsing (in production, use proper JSON parser)
    const char* cmd_start = strstr(content, "\"command\":\"");
    if (cmd_start) {
        cmd_start += 11;
        const char* cmd_end = strchr(cmd_start, '"');
        if (cmd_end) {
            size_t cmd_len = cmd_end - cmd_start;
            if (cmd_len < sizeof(command) - 1) {
                strncpy(command, cmd_start, cmd_len);
                command[cmd_len] = '\0';
            }
        }
    }
    
    const char* seq_start = strstr(content, "\"seq\":");
    if (seq_start) {
        sscanf(seq_start + 6, "%d", &seq);
    }
    
    // Route to appropriate handler
    if (strcmp(command, "initialize") == 0) {
        dap_handle_initialize(seq, arguments);
    } else if (strcmp(command, "launch") == 0) {
        dap_handle_launch(seq, arguments);
    } else if (strcmp(command, "setBreakpoints") == 0) {
        dap_handle_set_breakpoints(seq, arguments);
    } else if (strcmp(command, "continue") == 0) {
        dap_handle_continue(seq, arguments);
    } else if (strcmp(command, "next") == 0) {
        dap_handle_next(seq, arguments);
    } else if (strcmp(command, "stepIn") == 0) {
        dap_handle_step_in(seq, arguments);
    } else if (strcmp(command, "stepOut") == 0) {
        dap_handle_step_out(seq, arguments);
    } else if (strcmp(command, "threads") == 0) {
        dap_handle_threads(seq, arguments);
    } else if (strcmp(command, "stackTrace") == 0) {
        dap_handle_stack_trace(seq, arguments);
    } else if (strcmp(command, "scopes") == 0) {
        dap_handle_scopes(seq, arguments);
    } else if (strcmp(command, "variables") == 0) {
        dap_handle_variables(seq, arguments);
    } else if (strcmp(command, "evaluate") == 0) {
        dap_handle_evaluate(seq, arguments);
    } else {
        dap_send_response(seq, command, false, "Command not supported", NULL);
    }
}

// Main debug adapter server loop
void dap_server_run(void) {
    if (!g_dap_server) return;
    
    char buffer[8192];
    char content[8192];
    
    while (g_dap_server->running && fgets(buffer, sizeof(buffer), g_dap_server->input_stream)) {
        // Handle Content-Length header
        if (strncmp(buffer, "Content-Length:", 15) == 0) {
            int content_length = atoi(buffer + 16);
            
            // Read the empty line
            fgets(buffer, sizeof(buffer), g_dap_server->input_stream);
            
            // Read the JSON content
            if (content_length > 0 && content_length < (int)sizeof(content) - 1) {
                size_t read_bytes = fread(content, 1, content_length, g_dap_server->input_stream);
                content[read_bytes] = '\0';
                
                // Process the message
                dap_process_message(content);
            }
        }
    }
}

// Shutdown the debug adapter server
void dap_server_shutdown(void) {
    if (!g_dap_server) return;
    
    g_dap_server->running = false;
    
    // Free allocated memory
    if (g_dap_server->breakpoints) {
        for (int i = 0; i < g_dap_server->breakpoint_count; i++) {
            free(g_dap_server->breakpoints[i].source_path);
            free(g_dap_server->breakpoints[i].condition);
            free(g_dap_server->breakpoints[i].hit_condition);
            free(g_dap_server->breakpoints[i].log_message);
        }
        free(g_dap_server->breakpoints);
    }
    
    if (g_dap_server->variables) {
        for (int i = 0; i < g_dap_server->variable_count; i++) {
            free(g_dap_server->variables[i].name);
            free(g_dap_server->variables[i].value);
            free(g_dap_server->variables[i].type);
            free(g_dap_server->variables[i].evaluate_name);
        }
        free(g_dap_server->variables);
    }
    
    if (g_dap_server->stack_frames) {
        for (int i = 0; i < g_dap_server->frame_count; i++) {
            free(g_dap_server->stack_frames[i].name);
            free(g_dap_server->stack_frames[i].source_path);
        }
        free(g_dap_server->stack_frames);
    }
    
    if (g_dap_server->threads) {
        for (int i = 0; i < g_dap_server->thread_count; i++) {
            free(g_dap_server->threads[i].name);
        }
        free(g_dap_server->threads);
    }
    
    free(g_dap_server);
    g_dap_server = NULL;
}

// Main entry point for debug adapter
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    if (!dap_server_init(stdin, stdout)) {
        fprintf(stderr, "Failed to initialize debug adapter\n");
        return 1;
    }
    
    dap_server_run();
    dap_server_shutdown();
    
    return 0;
}