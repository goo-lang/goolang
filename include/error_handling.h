#ifndef ERROR_HANDLING_H
#define ERROR_HANDLING_H

#include "ccomp_shim.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ast.h"
#include "types.h"
#include "contracts.h"

// C23 compatibility
_Static_assert(sizeof(bool) == 1, "bool should be 1 byte");

// =============================================================================
// Error Handling Types and Categories
// =============================================================================

// Error categories for classification
typedef enum GOO_ENUM_U8 {
    ERROR_CATEGORY_MEMORY = 0,      // Memory allocation, bounds, etc.
    ERROR_CATEGORY_IO,              // File I/O, network, etc.  
    ERROR_CATEGORY_LOGIC,           // Logic errors, assertions
    ERROR_CATEGORY_RESOURCE,        // Resource exhaustion
    ERROR_CATEGORY_SECURITY,        // Security violations
    ERROR_CATEGORY_CONCURRENCY,     // Race conditions, deadlocks
    ERROR_CATEGORY_PANIC,           // Unrecoverable errors
    ERROR_CATEGORY_USER_DEFINED,    // Application-specific errors
    ERROR_CATEGORY_COUNT
} ErrorCategory;

// Error severity levels
typedef enum GOO_ENUM_U8 {
    ERROR_SEVERITY_INFO = 0,        // Informational, can continue
    ERROR_SEVERITY_WARNING,         // Warning, should investigate
    ERROR_SEVERITY_ERROR,           // Error, operation failed
    ERROR_SEVERITY_CRITICAL,        // Critical, system unstable
    ERROR_SEVERITY_FATAL,           // Fatal, must terminate
    ERROR_SEVERITY_COUNT
} ErrorSeverity;

// Error handling strategies
typedef enum GOO_ENUM_U8 {
    ERROR_STRATEGY_PROPAGATE = 0,   // Propagate error up call stack
    ERROR_STRATEGY_RECOVER,         // Attempt recovery
    ERROR_STRATEGY_RETRY,           // Retry operation
    ERROR_STRATEGY_FALLBACK,        // Use fallback value/operation
    ERROR_STRATEGY_PANIC,           // Panic (unrecoverable)
    ERROR_STRATEGY_IGNORE,          // Ignore error (dangerous!)
    ERROR_STRATEGY_COUNT
} ErrorStrategy;

// Exception safety guarantees
typedef enum GOO_ENUM_U8 {
    EXCEPTION_SAFETY_NONE = 0,      // No guarantees
    EXCEPTION_SAFETY_BASIC,         // Basic guarantee (no leaks)
    EXCEPTION_SAFETY_STRONG,        // Strong guarantee (rollback)
    EXCEPTION_SAFETY_NOTHROW,       // No-throw guarantee
    EXCEPTION_SAFETY_COUNT
} ExceptionSafety;

// =============================================================================
// Core Error Structures
// =============================================================================

// Error information structure
typedef struct ErrorInfo {
    ErrorCategory category;
    ErrorSeverity severity;
    uint32_t error_code;             // Unique error identifier
    char* message;                   // Human-readable message
    char* details;                   // Detailed error information
    
    // Source location using C23 anonymous struct
    struct {
        const char* filename;
        int line;
        int column;
        const char* function_name;
    };
    
    // Error context and causality
    struct ErrorInfo* cause;         // Root cause error
    struct ErrorInfo* next;          // For error chains
    void* context_data;              // Additional context
    size_t context_size;
    
    // Timestamps and metadata
    uint64_t timestamp;              // When error occurred
    uint32_t thread_id;              // Thread that generated error
    bool is_recoverable;             // Can this error be recovered from?
} ErrorInfo;

// Error handler function type
typedef struct ASTNode* (*ErrorHandler)(
    ErrorInfo* error,
    struct ASTNode* context,
    void* user_data
);

// Error handling context
typedef struct ErrorHandlingContext {
    ErrorHandler handlers[ERROR_CATEGORY_COUNT];
    void* user_data[ERROR_CATEGORY_COUNT];
    ErrorStrategy default_strategy;
    bool panic_on_unhandled;
    bool collect_stack_traces;
    size_t max_error_chain_length;
    
    // Error statistics
    uint64_t error_counts[ERROR_CATEGORY_COUNT];
    uint64_t total_errors;
    uint64_t recovered_errors;
    uint64_t fatal_errors;
} ErrorHandlingContext;

// =============================================================================
// Result and Option Types
// =============================================================================

// Result type for operations that can fail
typedef struct Result {
    bool is_ok;                      // true if success, false if error
    union {
        void* ok_value;              // Success value
        ErrorInfo* error;            // Error information
    };
    struct Type* value_type;         // Type of the success value
    ExceptionSafety safety_level;    // Exception safety guarantee
} Result;

// Option type for nullable values  
typedef struct Option {
    bool is_some;                    // true if value present
    void* value;                     // The value (if present)
    struct Type* value_type;         // Type of the value
} Option;

// =============================================================================
// Resource Management and RAII
// =============================================================================

// Resource cleanup function type
typedef void (*ResourceCleanup)(void* resource);

// Resource descriptor for RAII
typedef struct Resource {
    void* data;                      // Resource data
    ResourceCleanup cleanup;         // Cleanup function
    const char* resource_name;       // For debugging
    bool is_acquired;                // Resource acquisition state
    struct Resource* next;           // For resource stacks
} Resource;

// RAII scope for automatic resource management
typedef struct RAIIScope {
    Resource* resources;             // Stack of acquired resources
    bool is_active;                  // Scope is active
    ExceptionSafety safety_level;    // Safety guarantee for this scope
    struct RAIIScope* parent;        // Parent scope for nesting
} RAIIScope;

// =============================================================================
// Panic-Free Programming Support
// =============================================================================

// Panic-free operation descriptor
typedef struct PanicFreeOp {
    const char* operation_name;
    ContractExpression* preconditions;   // Contracts that must hold
    ContractExpression* postconditions;  // Guarantees after operation
    bool is_total_function;             // Function is total (no panics)
    ErrorStrategy fallback_strategy;    // What to do if contracts fail
} PanicFreeOp;

// Panic prevention context
typedef struct PanicPreventionContext {
    bool panic_free_mode;            // Enforce panic-free programming
    bool allow_runtime_checks;       // Allow runtime contract checks
    bool eliminate_bounds_checks;    // Optimize away bounds checks
    uint32_t max_stack_depth;        // Prevent stack overflow
    size_t max_allocation_size;      // Prevent OOM
} PanicPreventionContext;

// =============================================================================
// Error Handling API Functions
// =============================================================================

// Error creation and management
ErrorInfo* error_info_create(
    ErrorCategory category,
    ErrorSeverity severity,
    uint32_t error_code,
    const char* message,
    const char* filename,
    int line,
    const char* function_name
);

ErrorInfo* error_info_create_with_cause(
    ErrorCategory category,
    ErrorSeverity severity, 
    uint32_t error_code,
    const char* message,
    ErrorInfo* cause,
    const char* filename,
    int line,
    const char* function_name
);

void error_info_free(ErrorInfo* error);
ErrorInfo* error_info_chain(ErrorInfo* error, ErrorInfo* next);
char* error_info_to_string(const ErrorInfo* error);

// Result type operations
Result result_ok(void* value, struct Type* type);
Result result_error(ErrorInfo* error);
bool result_is_ok(const Result* result);
bool result_is_error(const Result* result);
void* result_unwrap(const Result* result);
void* result_unwrap_or(const Result* result, void* default_value);
Result result_map(const Result* result, void* (*func)(void*));
Result result_and_then(const Result* result, Result (*func)(void*));
void result_free(Result* result);

// Option type operations
Option option_some(void* value, struct Type* type);
Option option_none(struct Type* type);
bool option_is_some(const Option* option);
bool option_is_none(const Option* option);
void* option_unwrap(const Option* option);
void* option_unwrap_or(const Option* option, void* default_value);
Option option_map(const Option* option, void* (*func)(void*));
Option option_and_then(const Option* option, Option (*func)(void*));
void option_free(Option* option);

// =============================================================================
// RAII Resource Management
// =============================================================================

RAIIScope* raii_scope_create(ExceptionSafety safety_level);
void raii_scope_free(RAIIScope* scope);
Resource* raii_acquire_resource(
    RAIIScope* scope,
    void* data,
    ResourceCleanup cleanup,
    const char* resource_name
);
void raii_release_resource(RAIIScope* scope, Resource* resource);
void raii_scope_rollback(RAIIScope* scope);  // For strong exception safety

// =============================================================================
// Error Handler Management
// =============================================================================

ErrorHandlingContext* error_handling_context_create(void);
void error_handling_context_free(ErrorHandlingContext* context);

int register_error_handler(
    ErrorHandlingContext* context,
    ErrorCategory category,
    ErrorHandler handler,
    void* user_data
);

struct ASTNode* handle_error(
    ErrorHandlingContext* context,
    ErrorInfo* error,
    struct ASTNode* ast_context
);

// =============================================================================
// Panic-Free Programming API
// =============================================================================

PanicPreventionContext* panic_prevention_context_create(void);
void panic_prevention_context_free(PanicPreventionContext* context);

PanicFreeOp* panic_free_operation_create(
    const char* operation_name,
    ContractExpression* preconditions,
    ContractExpression* postconditions
);

void panic_free_operation_free(PanicFreeOp* operation);

bool verify_panic_free_operation(
    PanicFreeOp* operation,
    struct ASTNode* ast_context,
    PanicPreventionContext* context
);

// =============================================================================
// Error Analysis and Reporting
// =============================================================================

// Error analysis report
typedef struct ErrorAnalysisReport {
    uint64_t total_error_sites;
    uint64_t handled_error_sites;
    uint64_t unhandled_error_sites;
    uint64_t potential_panic_sites;
    
    // By category
    uint64_t category_counts[ERROR_CATEGORY_COUNT];
    uint64_t severity_counts[ERROR_SEVERITY_COUNT];
    
    // Safety analysis
    uint64_t panic_free_functions;
    uint64_t exception_safe_functions;
    uint64_t resource_leak_potential;
    
    char* recommendations;
} ErrorAnalysisReport;

ErrorAnalysisReport* analyze_error_handling(struct ASTNode* ast);
void error_analysis_report_free(ErrorAnalysisReport* report);
void error_analysis_report_print(const ErrorAnalysisReport* report);

// =============================================================================
// Integration with Contract System
// =============================================================================

// Convert contract violations to errors
ErrorInfo* contract_violation_to_error(
    ContractExpression* contract,
    struct ASTNode* violation_site
);

// Verify error handling contracts
bool verify_error_handling_contracts(
    struct ASTNode* function,
    ErrorHandlingContext* error_context,
    ContractContext* contract_context
);

// =============================================================================
// Error Macros for C23
// =============================================================================

// Error creation macros with source location
#define ERROR_CREATE(category, severity, code, message) \
    error_info_create((category), (severity), (code), (message), \
                     __FILE__, __LINE__, __func__)

#define ERROR_CREATE_WITH_CAUSE(category, severity, code, message, cause) \
    error_info_create_with_cause((category), (severity), (code), (message), \
                                (cause), __FILE__, __LINE__, __func__)

// Result creation macros
#define OK(value, type) result_ok((value), (type))
#define ERR(error) result_error((error))

// Option creation macros  
#define SOME(value, type) option_some((value), (type))
#define NONE(type) option_none((type))

// RAII macros for scope management
#define RAII_SCOPE(safety) \
    RAIIScope* __raii_scope = raii_scope_create((safety)); \
    __attribute__((cleanup(raii_scope_cleanup))) \
    RAIIScope* _cleanup_scope = __raii_scope

#define RAII_ACQUIRE(scope, data, cleanup, name) \
    raii_acquire_resource((scope), (data), (cleanup), (name))

// =============================================================================
// Utility Functions
// =============================================================================

// Error code registry
void register_error_code(uint32_t code, const char* description);
const char* get_error_description(uint32_t code);

// Error formatting and logging
void log_error(const ErrorInfo* error);
void log_error_chain(const ErrorInfo* error);

// Exception safety verification
bool verify_exception_safety(
    struct ASTNode* function,
    ExceptionSafety required_level
);

// Memory cleanup helper for C23 cleanup attribute
void raii_scope_cleanup(RAIIScope** scope);

#endif // ERROR_HANDLING_H
