#ifndef GOO_ERGONOMIC_ERRORS_H
#define GOO_ERGONOMIC_ERRORS_H

#include "errors/error.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// Forward declarations
typedef struct ASTNode ASTNode;
typedef struct TypeChecker TypeChecker;

// Error context frame for automatic propagation
typedef struct ErrorContextFrame {
    const char* function_name;
    const char* file_name;
    size_t line_number;
    const char* operation_description;
    
    // Automatic context information
    const char* current_file_being_processed;
    const char* current_function_being_analyzed;
    size_t current_expression_depth;
    
    // Chain to previous frame
    struct ErrorContextFrame* parent;
    
    // Error transformation rules
    void (*error_transformer)(Error* error, struct ErrorContextFrame* frame);
    
    // Cleanup function called when frame is popped
    void (*cleanup)(struct ErrorContextFrame* frame);
    void* cleanup_data;
} ErrorContextFrame;

// Enhanced error context with automatic propagation
typedef struct ErgoErrorContext {
    ErrorContext* base_context;
    ErrorContextFrame* current_frame;
    
    // Automatic context tracking
    bool auto_context_enabled;
    bool preserve_call_stack;
    size_t max_context_depth;
    size_t current_depth;
    
    // Error transformation and enrichment
    bool auto_suggestion_enabled;
    bool context_preservation_enabled;
    
    // Recovery patterns
    bool auto_retry_enabled;
    int max_retry_attempts;
    double retry_backoff_factor;
    
    // Error aggregation
    Error** error_collection;
    size_t error_collection_size;
    size_t error_collection_capacity;
    bool collecting_errors;
    
    // Performance tracking
    uint64_t error_handling_overhead_ns;
    size_t total_error_propagations;
} ErgoErrorContext;

// Error union result type for ergonomic handling
#define DECLARE_ERROR_UNION(T) \
    typedef struct { \
        union { \
            T value; \
            Error* error; \
        }; \
        bool is_error; \
        const char* context_info; \
    } Result_##T;

// Commonly used error union types
DECLARE_ERROR_UNION(int)
DECLARE_ERROR_UNION(double)
DECLARE_ERROR_UNION(bool)

// Special types for pointers
typedef struct {
    union {
        char* value;
        Error* error;
    };
    bool is_error;
    const char* context_info;
} Result_char_ptr;

typedef struct {
    union {
        void* value;
        Error* error;
    };
    bool is_error;
    const char* context_info;
} Result_void_ptr;

// Macro for creating successful results
#define OK(type, val) ((Result_##type){ .value = (val), .is_error = false, .context_info = NULL })

// Macro for creating error results
#define ERR(type, error_ptr) ((Result_##type){ .error = (error_ptr), .is_error = true, .context_info = __func__ })

// Special macros for pointer types
#define OK_PTR(val) ((Result_void_ptr){ .value = (val), .is_error = false, .context_info = NULL })
#define ERR_PTR(error_ptr) ((Result_void_ptr){ .error = (error_ptr), .is_error = true, .context_info = __func__ })

// Macro for error propagation with automatic context
#define TRY(expr) \
    do { \
        typeof(expr) __result = (expr); \
        if (__result.is_error) { \
            _ergo_propagate_error(__result.error, __FILE__, __LINE__, __func__, #expr); \
            return __result; \
        } \
        return (typeof(__result)){ .value = __result.value, .is_error = false, .context_info = NULL }; \
    } while(0)

// Macro for error propagation with context transformation
#define TRY_WITH_CONTEXT(expr, context_msg) \
    ({ \
        auto result = (expr); \
        if (result.is_error) { \
            _ergo_add_context(result.error, context_msg); \
            _ergo_propagate_error(result.error, __FILE__, __LINE__, __func__, #expr); \
            return result; \
        } \
        result.value; \
    })

// Macro for error propagation with transformation
#define TRY_MAP_ERROR(expr, error_transformer) \
    ({ \
        auto result = (expr); \
        if (result.is_error) { \
            error_transformer(result.error); \
            _ergo_propagate_error(result.error, __FILE__, __LINE__, __func__, #expr); \
            return result; \
        } \
        result.value; \
    })

// Error context management
ErgoErrorContext* ergo_error_context_new(void);
void ergo_error_context_free(ErgoErrorContext* ctx);

// Context frame management
ErrorContextFrame* ergo_push_context_frame(ErgoErrorContext* ctx, 
                                          const char* function_name,
                                          const char* operation_description);
void ergo_pop_context_frame(ErgoErrorContext* ctx);

// Automatic error context functions (called by macros)
void _ergo_propagate_error(Error* error, const char* file, size_t line, 
                          const char* function, const char* expression);
void _ergo_add_context(Error* error, const char* context);

// Error enrichment and transformation
void ergo_enrich_error_with_context(Error* error, const ErrorContextFrame* frame);
void ergo_suggest_fix_for_error(Error* error);
void ergo_add_stack_trace_to_error(Error* error, const ErgoErrorContext* ctx);

// Error recovery patterns
typedef enum {
    RECOVERY_PATTERN_RETRY,
    RECOVERY_PATTERN_FALLBACK,
    RECOVERY_PATTERN_CIRCUIT_BREAKER,
    RECOVERY_PATTERN_TIMEOUT,
    RECOVERY_PATTERN_CUSTOM
} RecoveryPattern;

typedef struct {
    RecoveryPattern pattern;
    int max_attempts;
    double backoff_factor;
    uint64_t timeout_ms;
    void* custom_data;
    
    // Recovery function
    bool (*recover)(Error* error, void* custom_data);
} RecoveryConfig;

// Apply recovery pattern to function call
#define WITH_RECOVERY(config, expr) \
    _ergo_apply_recovery(&(config), (expr))

// Error aggregation system
typedef struct ErrorCollector {
    ErgoErrorContext* ctx;
    Error** errors;
    size_t count;
    size_t capacity;
    
    // Collection behavior
    bool stop_on_first_error;
    bool deduplicate_errors;
    size_t max_errors;
    
    // Result aggregation
    bool has_fatal_errors;
    size_t total_errors;
    size_t total_warnings;
} ErrorCollector;

ErrorCollector* error_collector_new(ErgoErrorContext* ctx);
void error_collector_free(ErrorCollector* collector);

// Add error to collector (returns true if should continue)
bool error_collector_try(ErrorCollector* collector, Error* error);

// Finish collection and return aggregated result
Error* error_collector_finish(ErrorCollector* collector);

// Structured error types with hierarchy
typedef enum {
    STRUCTURED_ERROR_CONFIG = 7000,
    STRUCTURED_ERROR_NETWORK,
    STRUCTURED_ERROR_DATABASE,
    STRUCTURED_ERROR_VALIDATION,
    STRUCTURED_ERROR_AUTHENTICATION,
    STRUCTURED_ERROR_AUTHORIZATION,
    STRUCTURED_ERROR_BUSINESS_LOGIC
} StructuredErrorType;

typedef struct StructuredError {
    Error base;
    StructuredErrorType error_type;
    
    // Structured data
    const char* domain;
    const char* component;
    int error_subcode;
    
    // Machine-readable data
    const char* error_id;        // Unique error identifier
    const char* help_url;        // Link to documentation
    const char* suggested_action; // What user should do
    
    // Context data
    char** context_keys;
    char** context_values;
    size_t context_count;
    
    // Internationalization
    const char* message_key;     // For i18n lookup
    char** message_params;       // Parameters for message formatting
    size_t param_count;
} StructuredError;

StructuredError* structured_error_new(StructuredErrorType type, 
                                     const char* domain,
                                     const char* component);
void structured_error_free(StructuredError* error);

// Add context to structured error
void structured_error_add_context(StructuredError* error, 
                                 const char* key, const char* value);

// Set help information
void structured_error_set_help(StructuredError* error, 
                              const char* help_url, 
                              const char* suggested_action);

// Set internationalization data
void structured_error_set_i18n(StructuredError* error, 
                               const char* message_key,
                               char** params, size_t param_count);

// Error transformation system
typedef struct ErrorTransformRule {
    ErrorCode from_code;
    ErrorCode to_code;
    const char* context_pattern;    // Regex pattern for context matching
    
    // Transformation function
    Error* (*transform)(const Error* original, void* transform_data);
    void* transform_data;
    
    struct ErrorTransformRule* next;
} ErrorTransformRule;

typedef struct ErrorTransformer {
    ErrorTransformRule* rules;
    bool auto_transform_enabled;
    
    // Default transformations
    bool add_suggestions;
    bool add_help_urls;
    bool localize_messages;
} ErrorTransformer;

ErrorTransformer* error_transformer_new(void);
void error_transformer_free(ErrorTransformer* transformer);

// Add transformation rule
void error_transformer_add_rule(ErrorTransformer* transformer,
                               ErrorCode from_code, ErrorCode to_code,
                               const char* context_pattern,
                               Error* (*transform_func)(const Error*, void*),
                               void* transform_data);

// Apply transformations to error
Error* error_transformer_apply(ErrorTransformer* transformer, const Error* error);

// Convenience functions for common error patterns
#define RETURN_IF_ERROR(expr) \
    do { \
        auto __result = (expr); \
        if (__result.is_error) { \
            return ERR(void*, __result.error); \
        } \
    } while(0)

#define UNWRAP_OR_RETURN(expr, default_val) \
    ({ \
        auto __result = (expr); \
        __result.is_error ? (default_val) : __result.value; \
    })

#define EXPECT(expr, msg) \
    ({ \
        auto __result = (expr); \
        if (__result.is_error) { \
            _ergo_add_context(__result.error, msg); \
            _ergo_propagate_error(__result.error, __FILE__, __LINE__, __func__, #expr); \
            abort(); \
        } \
        __result.value; \
    })

// Integration with existing error system
void ergo_integrate_with_error_context(ErgoErrorContext* ergo_ctx, ErrorContext* base_ctx);
ErrorContext* ergo_get_base_context(ErgoErrorContext* ergo_ctx);

// Performance monitoring
typedef struct ErrorHandlingStats {
    uint64_t total_propagations;
    uint64_t total_transformations;
    uint64_t total_recoveries;
    uint64_t avg_propagation_time_ns;
    uint64_t max_context_depth;
    size_t memory_used_bytes;
} ErrorHandlingStats;

ErrorHandlingStats ergo_get_stats(const ErgoErrorContext* ctx);
void ergo_reset_stats(ErgoErrorContext* ctx);

// Configuration
typedef struct ErgoErrorConfig {
    bool enable_auto_context;
    bool enable_stack_traces;
    bool enable_suggestions;
    bool enable_recovery_patterns;
    bool enable_error_aggregation;
    
    size_t max_context_depth;
    size_t max_error_collection_size;
    double default_retry_backoff;
    int default_max_retries;
    
    // Performance settings
    bool track_performance;
    bool lazy_context_generation;
} ErgoErrorConfig;

extern ErgoErrorConfig ergo_default_config;

void ergo_apply_config(ErgoErrorContext* ctx, const ErgoErrorConfig* config);

#endif // GOO_ERGONOMIC_ERRORS_H