#ifndef TRANSPARENT_EXECUTION_H
#define TRANSPARENT_EXECUTION_H

#include "transparent_async.h"
#include <setjmp.h>

// Forward declarations
typedef struct FunctionRegistry FunctionRegistry;
typedef struct AsyncContext AsyncContext;
typedef struct TransparentFunction TransparentFunction;

// Function types for transparent execution
typedef enum {
    FUNC_TYPE_SYNC,          // Regular synchronous function
    FUNC_TYPE_ASYNC_NATIVE,  // Native async function (takes AsyncWaker*)
    FUNC_TYPE_ASYNC_TRANSPARENT, // Transparently async (auto-converted)
    FUNC_TYPE_ASYNC_HYBRID   // Can be called both sync and async
} FunctionType;

// Function execution context
typedef struct AsyncContext {
    AsyncRuntime* runtime;
    AsyncTask* current_task;
    AsyncWaker* waker;
    
    // Execution state
    bool is_async_context;
    bool should_yield;
    uint32_t yield_count;

    // Cooperative cancellation: points at the owning task/scope's cancellation
    // flag so an async function can poll atomic_load(async_ctx->is_cancelled).
    // NULL when the caller does not support cancellation.
    atomic_bool* is_cancelled;
    
    // Continuation support
    jmp_buf* continuation_point;
    void* continuation_data;
    size_t continuation_data_size;
    
    // Performance tracking
    uint64_t sync_execution_time_ns;
    uint64_t async_overhead_ns;
    
    // Parent context for nested calls
    struct AsyncContext* parent;
} AsyncContext;

// Transparent function wrapper
typedef struct TransparentFunction {
    const char* name;
    FunctionType type;
    void* original_function;
    void* async_wrapper;
    
    // Function characteristics
    uint64_t avg_execution_time_ns;
    bool is_io_bound;
    bool has_side_effects;
    uint32_t call_count;
    
    // Transformation metadata
    bool supports_cancellation;
    bool requires_context;
    size_t stack_size_hint;
} TransparentFunction;

// Function registry for tracking registered functions
typedef struct FunctionRegistry {
    TransparentFunction* functions;
    size_t function_count;
    size_t function_capacity;
    
    // Global execution context
    AsyncContext* current_context;
    pthread_key_t context_key;
    
    // Performance metrics
    atomic_uint_fast64_t total_transparent_calls;
    atomic_uint_fast64_t sync_execution_count;
    atomic_uint_fast64_t async_execution_count;
    atomic_uint_fast64_t yielding_calls;
} FunctionRegistry;

// Async yield point for transparent functions
typedef struct AsyncYieldPoint {
    void* function_address;
    uint32_t yield_id;
    void* continuation_data;
    size_t data_size;
    const char* yield_reason;
} AsyncYieldPoint;

// Transparent function signatures
typedef Result_void_ptr (*TransparentAsyncFunction)(void* args, AsyncContext* ctx);
typedef void* (*TransparentSyncFunction)(void* args);

// Automatic function transformation macros
#define TRANSPARENT_ASYNC(func_name) \
    Result_void_ptr func_name##_async(void* args, AsyncContext* ctx); \
    void* func_name##_sync(void* args); \
    static TransparentFunction func_name##_transparent = { \
        .name = #func_name, \
        .type = FUNC_TYPE_ASYNC_HYBRID, \
        .original_function = func_name##_sync, \
        .async_wrapper = func_name##_async \
    }

// Transparent execution statistics
typedef struct TransparentExecutionStats {
    uint64_t total_calls;
    uint64_t sync_executions;
    uint64_t async_executions;
    uint64_t yielding_calls;
    size_t registered_functions;
    double async_percentage;
    double yield_percentage;
} TransparentExecutionStats;

// Function registry operations
FunctionRegistry* function_registry_create(void);
void function_registry_destroy(FunctionRegistry* registry);
FunctionRegistry* function_registry_global(void);

Result_void_ptr function_registry_register(FunctionRegistry* registry,
                                         const char* name,
                                         void* original_function,
                                         void* async_wrapper,
                                         FunctionType type);

TransparentFunction* function_registry_find(FunctionRegistry* registry, const char* name);

// Async context management
AsyncContext* async_context_current(void);
void async_context_set_current(AsyncContext* ctx);
AsyncContext* async_context_create(AsyncRuntime* runtime, AsyncTask* task);
void async_context_destroy(AsyncContext* ctx);

// Transparent function execution
bool transparent_should_execute_async(TransparentFunction* func, void* args, size_t args_size);
Result_void_ptr transparent_function_execute(TransparentFunction* func, void* args, size_t args_size);

// Yielding and continuations
Result_void_ptr transparent_yield(AsyncContext* ctx, AsyncYieldPoint* yield_point);
bool transparent_should_yield(AsyncContext* ctx);

// Function transformation
Result_void_ptr transparent_create_wrapper(const char* name,
                                         void* original_function,
                                         void* analysis);

// System initialization
Result_void_ptr transparent_execution_init(void);
void transparent_execution_shutdown(void);
TransparentExecutionStats transparent_execution_get_stats(void);

// Helper macros for transparent async functions
#define ASYNC_YIELD(ctx) do { \
    if (transparent_should_yield(ctx)) { \
        AsyncYieldPoint yield_point = { \
            .function_address = __builtin_return_address(0), \
            .yield_id = __LINE__, \
            .continuation_data = NULL, \
            .data_size = 0, \
            .yield_reason = "voluntary yield" \
        }; \
        transparent_yield(ctx, &yield_point); \
    } \
} while(0)

#define ASYNC_CHECK_CANCELLED(ctx) do { \
    if ((ctx) && (ctx)->current_task && \
        atomic_load(&(ctx)->current_task->is_cancelled)) { \
        Error* error = malloc(sizeof(Error)); \
        *error = (Error){ \
            .code = ERROR_OPERATION_CANCELLED, \
            .severity = ERROR_SEVERITY_ERROR, \
            .category = ERROR_CATEGORY_RUNTIME, \
            .message = strdup("Operation cancelled"), \
            .hint = NULL, \
            .location = (SourceLocation){0}, \
            .next = NULL \
        }; \
        return ERR_PTR(error); \
    } \
} while(0)

#endif // TRANSPARENT_EXECUTION_H