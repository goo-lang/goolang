#ifndef GOO_ERROR_CONTEXT_H
#define GOO_ERROR_CONTEXT_H

#include "errors/error.h"
#include "runtime.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>

// =============================================================================
// Automatic Error Context Propagation System
// =============================================================================

// Forward declarations
typedef struct ErrorContextFrame ErrorContextFrame;
typedef struct ErrorContextStack ErrorContextStack;
typedef struct FunctionCallContext FunctionCallContext;

// Error context types
typedef enum {
    ERROR_CONTEXT_FILE_IO,        // File operations
    ERROR_CONTEXT_NETWORK,        // Network operations
    ERROR_CONTEXT_PARSING,        // Parsing operations
    ERROR_CONTEXT_VALIDATION,     // Validation operations
    ERROR_CONTEXT_COMPUTATION,    // Mathematical operations
    ERROR_CONTEXT_MEMORY,         // Memory operations
    ERROR_CONTEXT_CONCURRENCY,    // Concurrency operations
    ERROR_CONTEXT_CUSTOM          // User-defined context
} ErrorContextType;

// Stack frame information for error context
typedef struct ErrorStackFrame {
    const char* function_name;
    const char* file_name;
    int line_number;
    int column;
    const char* operation_description;
    ErrorContextType context_type;
    
    // Timing information
    uint64_t start_time_ns;
    uint64_t end_time_ns;
    
    // Additional context data
    void* context_data;
    size_t context_data_size;
    void (*context_data_destructor)(void*);
    
    struct ErrorStackFrame* parent;
} ErrorStackFrame;

// Enhanced error structure with automatic context
typedef struct EnhancedError {
    goo_error_t* base_error;        // Original error
    ErrorStackFrame* call_stack;    // Complete call stack
    ErrorContextType primary_context;
    
    // Automatic context information
    char* auto_generated_message;   // Context-enhanced message
    char* suggested_fix;            // Automatic fix suggestion
    char* related_documentation;    // Link to relevant docs
    
    // Error aggregation support
    struct EnhancedError** related_errors;
    int related_error_count;
    int related_error_capacity;
    
    // Metadata
    uint64_t creation_time_ns;
    const char* error_uuid;         // Unique identifier
    int severity_level;             // 0-10 severity scale
    
} EnhancedError;

// Error context manager for automatic propagation
typedef struct ErrorContextManager {
    bool enabled;
    bool auto_context_generation;
    bool stack_trace_capture;
    bool timing_enabled;
    
    // Global context registry (thread-local data managed separately)
    char** registered_context_names;
    ErrorContextType* registered_context_types;
    int registered_context_count;
    int max_stack_depth;
    
    // Error transformation rules
    struct {
        ErrorCode from_code;
        ErrorCode to_code;
        const char* transformation_reason;
        bool (*should_transform)(const goo_error_t*, const ErrorStackFrame*);
    }* transformation_rules;
    int transformation_rule_count;
    
    // Statistics
    uint64_t errors_enhanced;
    uint64_t contexts_captured;
    uint64_t auto_fixes_suggested;
    
} ErrorContextManager;

// =============================================================================
// Core Functions
// =============================================================================

// Initialize the error context system
void error_context_system_init(void);
void error_context_system_shutdown(void);

// Error context manager
ErrorContextManager* get_error_context_manager(void);
void error_context_manager_configure(bool auto_context, bool stack_trace, bool timing);

// Error stack frame management
ErrorStackFrame* error_context_push_frame(const char* function_name, 
                                          const char* file_name,
                                          int line_number,
                                          ErrorContextType context_type,
                                          const char* operation_description);
void error_context_pop_frame(void);
void error_context_clear_stack(void);

// Enhanced error creation and management
EnhancedError* enhanced_error_create(goo_error_t* base_error);
EnhancedError* enhanced_error_create_with_context(goo_error_t* base_error, 
                                                 const char* context_description);
void enhanced_error_free(EnhancedError* error);

// Automatic error enhancement
EnhancedError* enhance_error_automatically(goo_error_t* base_error);
void generate_error_context_message(EnhancedError* error);
void generate_suggested_fix(EnhancedError* error);

// Error propagation with context preservation
goo_error_union_t* propagate_error_with_context(goo_error_union_t* source_union);
EnhancedError* propagate_enhanced_error(EnhancedError* source_error);

// Context-aware error checking
typedef struct {
    const char* context_name;
    ErrorContextType type;
    bool (*validator)(const void* context_data);
    char* (*error_message_generator)(const void* context_data);
} ErrorContextValidator;

int register_error_context_validator(const ErrorContextValidator* validator);
bool validate_error_context(ErrorContextType type, const void* context_data);

// =============================================================================
// Automatic Context Macros
// =============================================================================

// Automatic error context generation
#define ERROR_CONTEXT_AUTO(func_name, operation) \
    ErrorStackFrame* __error_frame = error_context_push_frame( \
        (func_name), __FILE__, __LINE__, ERROR_CONTEXT_CUSTOM, (operation)); \
    if (!__error_frame) { /* Handle frame creation failure */ } \
    __attribute__((cleanup(error_context_auto_cleanup))) void* __cleanup_marker = __error_frame;

// Cleanup function for automatic context
void error_context_auto_cleanup(void* frame_ptr);

// Try with automatic context
#define TRY_WITH_CONTEXT(expr, context_type, description) ({ \
    ErrorStackFrame* __frame = error_context_push_frame( \
        __func__, __FILE__, __LINE__, (context_type), (description)); \
    typeof(expr) __result = (expr); \
    if (goo_error_union_is_error(__result)) { \
        /* Enhance error with current context */ \
        goo_error_t* __base_error = goo_error_union_get_error(__result); \
        EnhancedError* __enhanced = enhance_error_automatically(__base_error); \
        error_context_pop_frame(); \
        return goo_error_union_new_error((goo_error_t*)__enhanced); \
    } \
    error_context_pop_frame(); \
    __result; \
})

// Propagate with context
#define PROPAGATE_ERROR_WITH_CONTEXT(error_union) ({ \
    if (goo_error_union_is_error(error_union)) { \
        return propagate_error_with_context(error_union); \
    } \
    error_union; \
})

// =============================================================================
// Error Context Data Structures
// =============================================================================

// File I/O context
typedef struct {
    const char* file_path;
    const char* operation; // "read", "write", "open", etc.
    size_t bytes_processed;
    int file_descriptor;
    mode_t file_mode;
} FileIOContext;

// Network context
typedef struct {
    const char* host;
    int port;
    const char* protocol;
    const char* operation;
    size_t bytes_transferred;
    int socket_fd;
    struct sockaddr* address;
} NetworkContext;

// Parsing context
typedef struct {
    const char* input_type; // "JSON", "XML", "TOML", etc.
    const char* current_token;
    int line_number;
    int column_number;
    size_t byte_offset;
    const char* parsing_rule;
} ParsingContext;

// Validation context
typedef struct {
    const char* validation_type;
    const char* field_name;
    const char* expected_value;
    const char* actual_value;
    const char* constraint_description;
    bool is_required_field;
} ValidationContext;

// =============================================================================
// Context-Aware Error Messages
// =============================================================================

// Generate contextual error messages
char* generate_file_io_error_message(const FileIOContext* ctx, const goo_error_t* base_error);
char* generate_network_error_message(const NetworkContext* ctx, const goo_error_t* base_error);
char* generate_parsing_error_message(const ParsingContext* ctx, const goo_error_t* base_error);
char* generate_validation_error_message(const ValidationContext* ctx, const goo_error_t* base_error);

// Generate suggested fixes
char* suggest_file_io_fix(const FileIOContext* ctx, const goo_error_t* base_error);
char* suggest_network_fix(const NetworkContext* ctx, const goo_error_t* base_error);
char* suggest_parsing_fix(const ParsingContext* ctx, const goo_error_t* base_error);
char* suggest_validation_fix(const ValidationContext* ctx, const goo_error_t* base_error);

// =============================================================================
// Error Aggregation Support
// =============================================================================

// Error collector for multiple errors
typedef struct ErrorCollector {
    EnhancedError** errors;
    int error_count;
    int error_capacity;
    bool stop_on_first_error;
    
    // Collection statistics
    int errors_by_category[ERROR_CATEGORY_UNKNOWN + 1];
    int errors_by_severity[ERROR_SEVERITY_FATAL + 1];
    
} ErrorCollector;

ErrorCollector* error_collector_new(bool stop_on_first);
void error_collector_free(ErrorCollector* collector);
void error_collector_add(ErrorCollector* collector, EnhancedError* error);
bool error_collector_has_errors(const ErrorCollector* collector);
goo_error_union_t* error_collector_finish(ErrorCollector* collector);

// =============================================================================
// Debugging and Diagnostics
// =============================================================================

// Print enhanced error with full context
void print_enhanced_error(const EnhancedError* error);
void print_error_stack_trace(const ErrorStackFrame* frame);
void print_error_context_statistics(void);

// Error context visualization
char* visualize_error_context_tree(const EnhancedError* error);
char* generate_error_context_json(const EnhancedError* error);

// Performance monitoring
typedef struct {
    uint64_t total_errors_processed;
    uint64_t context_capture_time_ns;
    uint64_t error_enhancement_time_ns;
    uint64_t memory_used_bytes;
    double average_stack_depth;
} ErrorContextPerformanceStats;

ErrorContextPerformanceStats get_error_context_performance_stats(void);

// =============================================================================
// Integration Functions
// =============================================================================

// Integration with existing error handling
void integrate_with_existing_error_system(void);
goo_error_t* enhanced_error_to_goo_error(const EnhancedError* enhanced);
EnhancedError* goo_error_to_enhanced_error(const goo_error_t* goo_err);

// Integration with compiler
void register_compiler_error_contexts(void);
void enhance_compiler_errors(ErrorContext* compiler_ctx);

// Integration with runtime
void register_runtime_error_contexts(void);
void enhance_runtime_errors(void);

#endif // GOO_ERROR_CONTEXT_H