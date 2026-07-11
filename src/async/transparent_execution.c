#include "../../include/transparent_execution.h"
#include "../../include/errors/error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

// Global function registry
static FunctionRegistry* g_registry = NULL;
static pthread_mutex_t g_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread-local storage for async context
static pthread_key_t g_context_key;
static pthread_once_t g_context_key_once = PTHREAD_ONCE_INIT;

static void init_context_key(void) {
    pthread_key_create(&g_context_key, NULL);
}

// Get current async context
AsyncContext* async_context_current(void) {
    pthread_once(&g_context_key_once, init_context_key);
    return (AsyncContext*)pthread_getspecific(g_context_key);
}

// Set current async context
void async_context_set_current(AsyncContext* ctx) {
    pthread_once(&g_context_key_once, init_context_key);
    pthread_setspecific(g_context_key, ctx);
}

// Create new async context
AsyncContext* async_context_create(AsyncRuntime* runtime, AsyncTask* task) {
    AsyncContext* ctx = xcalloc(1, sizeof(AsyncContext));
    if (!ctx) return NULL;
    
    ctx->runtime = runtime;
    ctx->current_task = task;
    ctx->waker = task ? task->waker : NULL;
    ctx->is_async_context = (task != NULL);
    ctx->should_yield = false;
    ctx->yield_count = 0;
    ctx->sync_execution_time_ns = 0;
    ctx->async_overhead_ns = 0;
    ctx->parent = async_context_current();
    
    return ctx;
}

// Destroy async context
void async_context_destroy(AsyncContext* ctx) {
    if (!ctx) return;
    
    if (ctx->continuation_data) {
        free(ctx->continuation_data);
    }
    
    free(ctx);
}


// Function registry operations
FunctionRegistry* function_registry_create(void) {
    FunctionRegistry* registry = xcalloc(1, sizeof(FunctionRegistry));
    if (!registry) return NULL;
    
    registry->function_capacity = 1024;
    registry->functions = calloc(registry->function_capacity, sizeof(TransparentFunction));
    if (!registry->functions) {
        free(registry);
        return NULL;
    }
    
    pthread_key_create(&registry->context_key, NULL);
    
    atomic_init(&registry->total_transparent_calls, 0);
    atomic_init(&registry->sync_execution_count, 0);
    atomic_init(&registry->async_execution_count, 0);
    atomic_init(&registry->yielding_calls, 0);
    
    return registry;
}

void function_registry_destroy(FunctionRegistry* registry) {
    if (!registry) return;
    
    pthread_key_delete(registry->context_key);
    
    for (size_t i = 0; i < registry->function_count; i++) {
        TransparentFunction* func = &registry->functions[i];
        free((void*)func->name);
    }
    
    free(registry->functions);
    free(registry);
}

// Get or create global function registry
FunctionRegistry* function_registry_global(void) {
    pthread_mutex_lock(&g_registry_mutex);
    
    if (!g_registry) {
        g_registry = function_registry_create();
    }
    
    pthread_mutex_unlock(&g_registry_mutex);
    
    return g_registry;
}

// Register a transparent function
Result_void_ptr function_registry_register(FunctionRegistry* registry,
                                         const char* name,
                                         void* original_function,
                                         void* async_wrapper,
                                         FunctionType type) {
    // A registration needs a name and at least one callable. ASYNC_NATIVE functions
    // supply only an async_wrapper (original_function is NULL); sync functions supply
    // only original_function. Requiring original_function specifically wrongly rejects
    // the former.
    if (!registry || !name || (!original_function && !async_wrapper)) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Invalid function registration parameters"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    pthread_mutex_lock(&g_registry_mutex);
    
    // Check if we need to grow the array
    if (registry->function_count >= registry->function_capacity) {
        size_t new_capacity = registry->function_capacity * 2;
        TransparentFunction* new_functions = realloc(registry->functions,
                                                    new_capacity * sizeof(TransparentFunction));
        if (!new_functions) {
            pthread_mutex_unlock(&g_registry_mutex);
            
            Error* error = xmalloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_OUT_OF_MEMORY,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_RUNTIME,
                .message = strdup("Failed to grow function registry"),
                .hint = NULL,
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
        }
        
        registry->functions = new_functions;
        registry->function_capacity = new_capacity;
    }
    
    // Add the function
    TransparentFunction* func = &registry->functions[registry->function_count];
    func->name = strdup(name);
    func->type = type;
    func->original_function = original_function;
    func->async_wrapper = async_wrapper;
    func->avg_execution_time_ns = 0;
    func->is_io_bound = false;
    func->has_side_effects = true;  // Conservative default
    func->call_count = 0;
    func->supports_cancellation = false;
    func->requires_context = (type != FUNC_TYPE_SYNC);
    func->stack_size_hint = 8192;  // 8KB default
    
    registry->function_count++;
    
    pthread_mutex_unlock(&g_registry_mutex);
    
    return OK_PTR(func);
}

// Find a registered function by name
TransparentFunction* function_registry_find(FunctionRegistry* registry, const char* name) {
    if (!registry || !name) return NULL;
    
    for (size_t i = 0; i < registry->function_count; i++) {
        if (strcmp(registry->functions[i].name, name) == 0) {
            return &registry->functions[i];
        }
    }
    
    return NULL;
}

// Function analysis for transparent async detection
typedef struct FunctionAnalysis {
    bool contains_io_operations;
    bool contains_blocking_calls;
    bool contains_async_calls;
    bool contains_yield_points;
    size_t estimated_stack_usage;
    uint64_t estimated_execution_time_ns;
} FunctionAnalysis;

// Analyze a function to determine its characteristics
static FunctionAnalysis analyze_function(void* function_address, size_t function_size) {
    (void)function_size; // Suppress unused parameter warning
    FunctionAnalysis analysis = {
        .contains_io_operations = false,
        .contains_blocking_calls = false,
        .contains_async_calls = false,
        .contains_yield_points = false,
        .estimated_stack_usage = 1024,  // Default 1KB
        .estimated_execution_time_ns = 1000  // Default 1us
    };
    
    // In a real implementation, this would analyze the function's machine code
    // or use compile-time annotations to determine characteristics
    // For now, we'll use heuristics based on function name patterns
    
    // Check for common I/O patterns in symbol names
    Dl_info info;
    if (dladdr(function_address, &info) && info.dli_sname) {
        const char* name = info.dli_sname;
        
        // Check for I/O operations
        if (strstr(name, "read") || strstr(name, "write") ||
            strstr(name, "send") || strstr(name, "recv") ||
            strstr(name, "file") || strstr(name, "socket")) {
            analysis.contains_io_operations = true;
            analysis.estimated_execution_time_ns = 100000;  // 100us for I/O
        }
        
        // Check for blocking operations
        if (strstr(name, "sleep") || strstr(name, "wait") ||
            strstr(name, "lock") || strstr(name, "mutex")) {
            analysis.contains_blocking_calls = true;
            analysis.estimated_execution_time_ns = 1000000;  // 1ms for blocking
        }
        
        // Check for async operations
        if (strstr(name, "async") || strstr(name, "await") ||
            strstr(name, "future") || strstr(name, "promise")) {
            analysis.contains_async_calls = true;
        }
        
        // Check for yield points
        if (strstr(name, "yield") || strstr(name, "suspend")) {
            analysis.contains_yield_points = true;
        }
    }
    
    return analysis;
}

// Determine if a function should be executed asynchronously
bool transparent_should_execute_async(TransparentFunction* func, void* args, size_t args_size) {
    (void)args;      // Suppress unused parameter warning
    (void)args_size; // Suppress unused parameter warning
    if (!func) return false;
    
    // Explicit async functions always run async
    if (func->type == FUNC_TYPE_ASYNC_NATIVE || func->type == FUNC_TYPE_ASYNC_TRANSPARENT) {
        return true;
    }
    
    // Sync functions never run async
    if (func->type == FUNC_TYPE_SYNC) {
        return false;
    }
    
    // For hybrid functions, analyze runtime conditions
    if (func->type == FUNC_TYPE_ASYNC_HYBRID) {
        // Check if we're already in async context
        AsyncContext* ctx = async_context_current();
        if (ctx && ctx->is_async_context) {
            return true;  // Maintain async execution in async context
        }
        
        // Use function characteristics
        if (func->is_io_bound || func->avg_execution_time_ns > 10000) {
            return true;  // I/O bound or slow functions should be async
        }
        
        // Check call frequency - hot functions might benefit from async
        if (func->call_count > 1000 && func->avg_execution_time_ns > 1000) {
            return true;
        }
    }
    
    return false;
}

// Execute a transparent function
Result_void_ptr transparent_function_execute(TransparentFunction* func, void* args, size_t args_size) {
    // ASYNC_NATIVE functions carry only an async_wrapper (original_function is NULL),
    // so accept a function that has either callable, not original_function specifically.
    if (!func || (!func->original_function && !func->async_wrapper)) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Invalid transparent function"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    atomic_fetch_add(&g_registry->total_transparent_calls, 1);
    func->call_count++;
    
    uint64_t start_time = async_get_timestamp_ns();
    
    // Determine execution mode (telemetry only — see ABI note below).
    bool should_async = transparent_should_execute_async(func, args, args_size);

    Result_void_ptr result;

    // ABI note: the transparent_async runtime invokes tasks as AsyncFunction
    // (void*, AsyncWaker*), but every wrapper registered here is a
    // TransparentAsyncFunction (void*, AsyncContext*) — incompatible signatures.
    // Dispatching through the runtime would pass a waker where the function expects a
    // context and crash on the first ctx field access. The inline executor blocks the
    // caller anyway, so we invoke the wrapper directly with a correct AsyncContext:
    // ABI-safe and behaviorally equivalent to the (blocking) inline runtime path.
    if (func->type == FUNC_TYPE_SYNC && func->original_function) {
        // Plain synchronous function
        atomic_fetch_add(&g_registry->sync_execution_count, 1);
        TransparentSyncFunction sync_fn = (TransparentSyncFunction)func->original_function;
        void* sync_result = sync_fn(args);
        result = OK_PTR(sync_result);
    } else {
        atomic_fetch_add(should_async ? &g_registry->async_execution_count
                                      : &g_registry->sync_execution_count, 1);

        // Cooperative-cancellation flag the function can poll via
        // atomic_load(ctx->is_cancelled). No external canceller here, so it stays false.
        atomic_bool cancel_flag;
        atomic_init(&cancel_flag, false);

        AsyncContext ctx = {
            .runtime = NULL,
            .current_task = NULL,
            .waker = NULL,
            .is_async_context = true,
            .should_yield = false,
            .yield_count = 0,
            .is_cancelled = &cancel_flag,
            .parent = async_context_current()
        };

        AsyncContext* prev_ctx = async_context_current();
        async_context_set_current(&ctx);

        // Prefer original_function; fall back to async_wrapper for ASYNC_NATIVE
        // functions, which have no original_function to call.
        TransparentAsyncFunction async_fn = (TransparentAsyncFunction)
            (func->original_function ? func->original_function : func->async_wrapper);
        result = async_fn(args, &ctx);

        async_context_set_current(prev_ctx);
    }
    
    // Update function statistics
    uint64_t end_time = async_get_timestamp_ns();
    uint64_t execution_time = end_time - start_time;
    
    // Update average execution time (exponential moving average)
    func->avg_execution_time_ns = (func->avg_execution_time_ns * 7 + execution_time) / 8;
    
    return result;
}

// Yield execution in transparent async context
Result_void_ptr transparent_yield(AsyncContext* ctx, AsyncYieldPoint* yield_point) {
    if (!ctx) {
        ctx = async_context_current();
    }
    
    if (!ctx || !ctx->is_async_context) {
        // Not in async context, can't yield
        return OK_PTR(NULL);
    }
    
    ctx->should_yield = true;
    ctx->yield_count++;
    
    if (ctx->waker) {
        atomic_fetch_add(&g_registry->yielding_calls, 1);
        
        // Save continuation data if provided
        if (yield_point && yield_point->continuation_data) {
            if (ctx->continuation_data) {
                free(ctx->continuation_data);
            }
            ctx->continuation_data = malloc(yield_point->data_size);
            if (ctx->continuation_data) {
                memcpy(ctx->continuation_data, yield_point->continuation_data, yield_point->data_size);
                ctx->continuation_data_size = yield_point->data_size;
            }
        }
        
        // Wake up the task to reschedule
        async_waker_wake(ctx->waker);
    }
    
    return OK_PTR(NULL);
}

// Check if function should yield
bool transparent_should_yield(AsyncContext* ctx) {
    if (!ctx) {
        ctx = async_context_current();
    }
    
    if (!ctx || !ctx->is_async_context) {
        return false;
    }
    
    // Check if we've been running too long
    if (ctx->current_task) {
        uint64_t now = async_get_timestamp_ns();
        uint64_t elapsed = now - ctx->current_task->context.start_time;
        
        // Yield after 1ms of continuous execution
        if (elapsed > 1000000) {
            return true;
        }
    }
    
    // Check if runtime requests yielding
    if (ctx->runtime) {
        RuntimeStats stats = async_runtime_get_stats(ctx->runtime);
        if (stats.pending_tasks > stats.active_workers * 2) {
            return true;  // Many pending tasks, yield to let others run
        }
    }
    
    return ctx->should_yield;
}

// Create a transparent async wrapper for a function
Result_void_ptr transparent_create_wrapper(const char* name,
                                         void* original_function,
                                         void* analysis) {
    FunctionAnalysis* func_analysis = (FunctionAnalysis*)analysis;
    if (!name || !original_function) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Invalid wrapper parameters"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // In a real implementation, this would generate or select appropriate
    // async wrapper code. For now, we'll return a generic wrapper function.
    
    // Register the function
    FunctionType type = FUNC_TYPE_ASYNC_HYBRID;
    if (func_analysis) {
        if (func_analysis->contains_async_calls || func_analysis->contains_yield_points) {
            type = FUNC_TYPE_ASYNC_TRANSPARENT;
        } else if (!func_analysis->contains_io_operations && !func_analysis->contains_blocking_calls) {
            type = FUNC_TYPE_SYNC;
        }
    }
    
    FunctionRegistry* registry = function_registry_global();
    Result_void_ptr reg_result = function_registry_register(registry, name,
                                                           original_function, NULL, type);
    
    if (reg_result.is_error) {
        return reg_result;
    }
    
    TransparentFunction* func = (TransparentFunction*)reg_result.value;
    
    // Update function characteristics from analysis
    if (func_analysis) {
        func->is_io_bound = func_analysis->contains_io_operations;
        func->supports_cancellation = func_analysis->contains_yield_points;
        func->stack_size_hint = func_analysis->estimated_stack_usage;
        func->avg_execution_time_ns = func_analysis->estimated_execution_time_ns;
    }
    
    return OK_PTR(func);
}

// Initialize transparent execution system
Result_void_ptr transparent_execution_init(void) {
    FunctionRegistry* registry = function_registry_global();
    if (!registry) {
        Error* error = xmalloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Failed to create function registry"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    return OK_PTR(registry);
}

// Shutdown transparent execution system
void transparent_execution_shutdown(void) {
    pthread_mutex_lock(&g_registry_mutex);
    
    if (g_registry) {
        function_registry_destroy(g_registry);
        g_registry = NULL;
    }
    
    pthread_mutex_unlock(&g_registry_mutex);
}

// Get transparent execution statistics
TransparentExecutionStats transparent_execution_get_stats(void) {
    TransparentExecutionStats stats = {0};
    
    if (g_registry) {
        stats.total_calls = atomic_load(&g_registry->total_transparent_calls);
        stats.sync_executions = atomic_load(&g_registry->sync_execution_count);
        stats.async_executions = atomic_load(&g_registry->async_execution_count);
        stats.yielding_calls = atomic_load(&g_registry->yielding_calls);
        stats.registered_functions = g_registry->function_count;
        
        // Calculate percentages
        if (stats.total_calls > 0) {
            stats.async_percentage = (double)stats.async_executions / stats.total_calls * 100.0;
            stats.yield_percentage = (double)stats.yielding_calls / stats.total_calls * 100.0;
        }
    }
    
    return stats;
}