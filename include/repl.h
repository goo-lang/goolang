#ifndef REPL_H
#define REPL_H

#include "ast.h"
#include "types.h"
#include "hot_reload.h"
#include "performance_monitor.h"
#include "repl_errors.h"
#include <stdio.h>
#include <stdbool.h>

// Forward declarations
typedef struct REPLContext REPLContext;
typedef struct REPLValue REPLValue;
typedef struct REPLHistory REPLHistory;
typedef struct REPLCompletion REPLCompletion;
typedef struct DebugState DebugState;

// REPL modes
typedef enum {
    REPL_MODE_NORMAL,       // Normal expression evaluation
    REPL_MODE_MULTILINE,    // Multi-line input mode
    REPL_MODE_DEBUG,        // Debug mode with extra information
    REPL_MODE_TYPE_ONLY     // Only show type information, don't evaluate
} REPLMode;

// REPL value types
typedef enum {
    REPL_VALUE_INT,
    REPL_VALUE_FLOAT,
    REPL_VALUE_STRING,
    REPL_VALUE_BOOL,
    REPL_VALUE_NULL,
    REPL_VALUE_ERROR,
    REPL_VALUE_FUNCTION,
    REPL_VALUE_TYPE,
    REPL_VALUE_COMPLEX      // For structs, arrays, etc.
} REPLValueType;

// REPL command types
typedef enum {
    REPL_CMD_HELP,
    REPL_CMD_EXIT,
    REPL_CMD_RESET,
    REPL_CMD_HISTORY,
    REPL_CMD_TYPE,
    REPL_CMD_INSPECT,
    REPL_CMD_CLEAR,
    REPL_CMD_MODE,
    REPL_CMD_RELOAD,
    REPL_CMD_SCOPE,
    REPL_CMD_BINDINGS,
    REPL_CMD_PERF,
    REPL_CMD_ERRORS,
    REPL_CMD_DEBUG,
    REPL_CMD_AUTOFIX,
    REPL_CMD_UNKNOWN
} REPLCommandType;

// REPL value representation
struct REPLValue {
    REPLValueType type;
    Type* goo_type;           // Associated Goo type
    bool is_valid;
    
    union {
        int64_t int_val;
        double float_val;
        char* string_val;
        bool bool_val;
        struct {
            char* error_msg;
            int error_code;
        } error;
        struct {
            void* data;
            size_t size;
            char* type_name;
        } complex;
    } value;
};

// REPL history entry
struct REPLHistory {
    char* input;
    REPLValue* result;
    Type* result_type;
    double execution_time;    // In milliseconds
    int entry_number;
    struct REPLHistory* next;
};

// REPL completion candidate
struct REPLCompletion {
    char* text;
    char* description;
    char* type_info;
    struct REPLCompletion* next;
};

// REPL context
struct REPLContext {
    // Core components
    TypeChecker* type_checker;
    HotReloadContext* hot_reload;
    PerformanceMonitor* performance_monitor;
    ErrorContext* error_context;
    DebugState* debug_state;
    
    // REPL state
    REPLMode mode;
    int session_id;
    char* prompt;
    bool running;
    bool auto_complete_enabled;
    bool show_types;
    bool show_timing;
    bool show_performance;
    
    // History and bindings
    REPLHistory* history;
    int history_count;
    int max_history;
    
    // Multiline input buffer
    char* multiline_buffer;
    size_t multiline_size;
    size_t multiline_capacity;
    
    // Temporary bindings (variables defined in REPL)
    Scope* repl_scope;
    
    // Output formatting
    int indent_level;
    bool color_output;
    FILE* output_stream;
    FILE* error_stream;
    
    // Statistics
    int expressions_evaluated;
    int errors_encountered;
    double total_execution_time;
};

// =============================================================================
// Core REPL Functions
// =============================================================================

// Context management
REPLContext* repl_context_new(void);
void repl_context_free(REPLContext* ctx);
int repl_init(REPLContext* ctx);
void repl_cleanup(REPLContext* ctx);

// Main REPL loop
int repl_run(REPLContext* ctx);
int repl_run_interactive(REPLContext* ctx);
int repl_eval_string(REPLContext* ctx, const char* input);

// Input handling
int repl_read_line(REPLContext* ctx, char* buffer, size_t buffer_size);
int repl_handle_multiline(REPLContext* ctx, const char* line);
bool repl_is_complete_expression(const char* input);

// Expression evaluation
REPLValue* repl_evaluate_expression(REPLContext* ctx, const char* input);
REPLValue* repl_evaluate_ast(REPLContext* ctx, ASTNode* ast);

// =============================================================================
// Command System
// =============================================================================

// Command parsing and execution
REPLCommandType repl_parse_command(const char* input);
int repl_execute_command(REPLContext* ctx, REPLCommandType cmd, const char* args);

// Individual command handlers
int repl_cmd_help(REPLContext* ctx, const char* args);
int repl_cmd_exit(REPLContext* ctx, const char* args);
int repl_cmd_reset(REPLContext* ctx, const char* args);
int repl_cmd_history(REPLContext* ctx, const char* args);
int repl_cmd_type(REPLContext* ctx, const char* args);
int repl_cmd_inspect(REPLContext* ctx, const char* args);
int repl_cmd_clear(REPLContext* ctx, const char* args);
int repl_cmd_mode(REPLContext* ctx, const char* args);
int repl_cmd_reload(REPLContext* ctx, const char* args);
int repl_cmd_scope(REPLContext* ctx, const char* args);
int repl_cmd_bindings(REPLContext* ctx, const char* args);
int repl_cmd_perf(REPLContext* ctx, const char* args);
int repl_cmd_errors(REPLContext* ctx, const char* args);
int repl_cmd_debug(REPLContext* ctx, const char* args);

// =============================================================================
// Value Management
// =============================================================================

// Value creation and management
REPLValue* repl_value_new(REPLValueType type);
void repl_value_free(REPLValue* value);
REPLValue* repl_value_copy(const REPLValue* value);

// Value creation helpers
REPLValue* repl_value_int(int64_t val);
REPLValue* repl_value_float(double val);
REPLValue* repl_value_string(const char* val);
REPLValue* repl_value_bool(bool val);
REPLValue* repl_value_null(void);
REPLValue* repl_value_error(const char* msg, int code);

// Value operations
char* repl_value_to_string(const REPLValue* value);
bool repl_value_is_truthy(const REPLValue* value);
int repl_value_compare(const REPLValue* a, const REPLValue* b);

// =============================================================================
// Type Information System
// =============================================================================

// Type introspection
char* repl_get_type_info(REPLContext* ctx, const char* expression);
char* repl_format_type(const Type* type, bool verbose);
int repl_print_type_hierarchy(REPLContext* ctx, const Type* type);

// Type operations
Type* repl_infer_type(REPLContext* ctx, const char* expression);
bool repl_type_is_displayable(const Type* type);
char* repl_type_get_documentation(const Type* type);

// =============================================================================
// History Management
// =============================================================================

// History operations
int repl_history_add(REPLContext* ctx, const char* input, REPLValue* result, Type* type, double exec_time);
REPLHistory* repl_history_get(REPLContext* ctx, int index);
int repl_history_save(REPLContext* ctx, const char* filename);
int repl_history_load(REPLContext* ctx, const char* filename);
void repl_history_clear(REPLContext* ctx);

// History display
int repl_print_history(REPLContext* ctx, int count);
int repl_print_history_entry(REPLContext* ctx, const REPLHistory* entry);

// =============================================================================
// Auto-completion
// =============================================================================

// Completion system
REPLCompletion* repl_get_completions(REPLContext* ctx, const char* partial_input, int cursor_pos);
void repl_completion_free(REPLCompletion* completion);
int repl_print_completions(REPLContext* ctx, REPLCompletion* completions);

// Completion providers
REPLCompletion* repl_complete_variables(REPLContext* ctx, const char* prefix);
REPLCompletion* repl_complete_functions(REPLContext* ctx, const char* prefix);
REPLCompletion* repl_complete_types(REPLContext* ctx, const char* prefix);
REPLCompletion* repl_complete_keywords(const char* prefix);
REPLCompletion* repl_complete_commands(const char* prefix);

// =============================================================================
// Hot Reload Integration
// =============================================================================

// Hot reload integration
int repl_enable_hot_reload(REPLContext* ctx);
int repl_reload_changed_modules(REPLContext* ctx);
void repl_on_file_changed(const char* filename, void* context);

// Module management
int repl_load_module(REPLContext* ctx, const char* module_path);
int repl_unload_module(REPLContext* ctx, const char* module_path);
int repl_list_modules(REPLContext* ctx);

// =============================================================================
// Output Formatting
// =============================================================================

// Output control
void repl_set_color_output(REPLContext* ctx, bool enabled);
void repl_set_output_stream(REPLContext* ctx, FILE* stream);
void repl_set_error_stream(REPLContext* ctx, FILE* stream);

// Formatted output
int repl_printf(REPLContext* ctx, const char* format, ...);
int repl_error_printf(REPLContext* ctx, const char* format, ...);
int repl_print_with_syntax_highlighting(REPLContext* ctx, const char* code);

// Color and formatting helpers
const char* repl_color_code(const char* color_name);
void repl_print_banner(REPLContext* ctx);
void repl_print_statistics(REPLContext* ctx);

// =============================================================================
// Utility Functions
// =============================================================================

// String utilities
char* repl_trim_whitespace(const char* str);
bool repl_string_starts_with(const char* str, const char* prefix);
bool repl_string_ends_with(const char* str, const char* suffix);
char** repl_split_string(const char* str, const char* delimiter, int* count);

// Input validation
bool repl_is_valid_identifier(const char* str);
bool repl_is_numeric_literal(const char* str);
bool repl_is_string_literal(const char* str);

// Error handling
void repl_report_error(REPLContext* ctx, const char* message, Position pos);
void repl_report_warning(REPLContext* ctx, const char* message, Position pos);

// Configuration
int repl_load_config(REPLContext* ctx, const char* config_file);
int repl_save_config(REPLContext* ctx, const char* config_file);

#endif // REPL_H