#include "../../include/structured_concurrency_enhanced.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

// Global scheduler instance
static StructuredScheduler* g_global_scheduler = NULL;
static pthread_mutex_t g_global_scheduler_mutex = PTHREAD_MUTEX_INITIALIZER;

// Utility functions
static uint64_t get_current_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t generate_id(void) {
    static atomic_uint_fast64_t counter = 1;
    return atomic_fetch_add(&counter, 1);
}

// Configuration helpers
ConcurrentBlockConfig concurrent_block_config_default(void) {
    return (ConcurrentBlockConfig) {
        .max_concurrent_tasks = 8,
        .timeout_ms = 30000,  // 30 seconds
        .fail_fast = false,
        .collect_all_results = true,
        .default_priority = TASK_PRIORITY_NORMAL,
        .max_memory_per_task = 16 * 1024 * 1024,  // 16MB
        .max_total_memory = 256 * 1024 * 1024,    // 256MB
        .use_transparent_execution = true,
        .async_runtime = NULL  // Use global runtime
    };
}

ConcurrentBlockConfig concurrent_block_config_cpu_intensive(void) {
    ConcurrentBlockConfig config = concurrent_block_config_default();
    config.max_concurrent_tasks = 16;
    config.timeout_ms = 120000;  // 2 minutes
    config.default_priority = TASK_PRIORITY_HIGH;
    return config;
}

ConcurrentBlockConfig concurrent_block_config_io_intensive(void) {
    ConcurrentBlockConfig config = concurrent_block_config_default();
    config.max_concurrent_tasks = 64;
    config.timeout_ms = 300000;  // 5 minutes
    config.max_memory_per_task = 4 * 1024 * 1024;  // 4MB per task
    return config;
}

ConcurrentBlockConfig concurrent_block_config_fail_fast(void) {
    ConcurrentBlockConfig config = concurrent_block_config_default();
    config.fail_fast = true;
    config.collect_all_results = false;
    return config;
}

// Structured scheduler implementation
StructuredScheduler* structured_scheduler_create(AsyncRuntime* runtime) {
    StructuredScheduler* scheduler = calloc(1, sizeof(StructuredScheduler));
    if (!scheduler) return NULL;
    
    scheduler->id = generate_id();
    scheduler->async_runtime = runtime ? runtime : async_runtime_global();
    
    // Create global task scope
    TaskScopeConfig scope_config = task_scope_config_default();
    scope_config.max_concurrent_tasks = 32;
    scope_config.max_total_tasks = 10000;
    scope_config.use_work_stealing = true;
    scope_config.numa_aware_scheduling = true;
    
    scheduler->global_scope = task_scope_create(scope_config, "structured_scheduler_global");
    if (!scheduler->global_scope) {
        free(scheduler);
        return NULL;
    }
    
    // Initialize active blocks array
    scheduler->active_capacity = 100;
    scheduler->active_blocks = calloc(scheduler->active_capacity, sizeof(ConcurrentBlock*));
    if (!scheduler->active_blocks) {
        task_scope_destroy(scheduler->global_scope);
        free(scheduler);
        return NULL;
    }
    
    atomic_init(&scheduler->total_memory_used, 0);
    atomic_init(&scheduler->total_active_tasks, 0);
    
    if (pthread_mutex_init(&scheduler->scheduler_mutex, NULL) != 0) {
        free(scheduler->active_blocks);
        task_scope_destroy(scheduler->global_scope);
        free(scheduler);
        return NULL;
    }
    
    return scheduler;
}

Result_void_ptr structured_scheduler_start(StructuredScheduler* scheduler) {
    if (!scheduler) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid scheduler"));
    }
    
    // Start the global task scope
    Result_void_ptr result = task_scope_start(scheduler->global_scope);
    if (result.is_error) {
        return result;
    }
    
    return OK_PTR(scheduler);
}

Result_void_ptr structured_scheduler_shutdown(StructuredScheduler* scheduler, uint64_t timeout_ms) {
    if (!scheduler) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid scheduler"));
    }
    
    pthread_mutex_lock(&scheduler->scheduler_mutex);
    
    // Cancel all active blocks
    for (size_t i = 0; i < scheduler->active_count; i++) {
        if (scheduler->active_blocks[i]) {
            concurrent_block_cancel(scheduler->active_blocks[i]);
        }
    }
    
    pthread_mutex_unlock(&scheduler->scheduler_mutex);
    
    // Shutdown the global task scope
    Result_void_ptr result = task_scope_shutdown(scheduler->global_scope, timeout_ms);
    return result;
}

void structured_scheduler_destroy(StructuredScheduler* scheduler) {
    if (!scheduler) return;
    
    // Shutdown with a reasonable timeout
    structured_scheduler_shutdown(scheduler, 5000);
    
    // Clean up active blocks
    pthread_mutex_lock(&scheduler->scheduler_mutex);
    for (size_t i = 0; i < scheduler->active_count; i++) {
        if (scheduler->active_blocks[i]) {
            concurrent_block_destroy(scheduler->active_blocks[i]);
        }
    }
    pthread_mutex_unlock(&scheduler->scheduler_mutex);
    
    free(scheduler->active_blocks);
    task_scope_destroy(scheduler->global_scope);
    pthread_mutex_destroy(&scheduler->scheduler_mutex);
    free(scheduler);
}

// Global scheduler functions
void structured_set_global_scheduler(StructuredScheduler* scheduler) {
    pthread_mutex_lock(&g_global_scheduler_mutex);
    g_global_scheduler = scheduler;
    pthread_mutex_unlock(&g_global_scheduler_mutex);
}

StructuredScheduler* structured_get_global_scheduler(void) {
    pthread_mutex_lock(&g_global_scheduler_mutex);
    StructuredScheduler* scheduler = g_global_scheduler;
    pthread_mutex_unlock(&g_global_scheduler_mutex);
    return scheduler;
}

// Concurrent expression implementation
ConcurrentExpression* concurrent_expression_create(const char* name, 
    ConcurrentFunction function, void* args, size_t args_size) {
    
    if (!function) return NULL;
    
    ConcurrentExpression* expr = calloc(1, sizeof(ConcurrentExpression));
    if (!expr) return NULL;
    
    expr->id = generate_id();
    if (name) {
        strncpy(expr->name, name, sizeof(expr->name) - 1);
        expr->name[sizeof(expr->name) - 1] = '\0';
    } else {
        snprintf(expr->name, sizeof(expr->name), "expr_%llu", expr->id);
    }
    
    // Copy arguments
    if (args && args_size > 0) {
        expr->arguments = malloc(args_size);
        if (!expr->arguments) {
            free(expr);
            return NULL;
        }
        memcpy(expr->arguments, args, args_size);
        expr->args_size = args_size;
    }
    
    // Create a transparent function wrapper
    expr->function = malloc(sizeof(TransparentFunction));
    if (!expr->function) {
        free(expr->arguments);
        free(expr);
        return NULL;
    }
    
    // Initialize transparent function
    *(expr->function) = (TransparentFunction) {
        .name = strdup(expr->name),
        .type = FUNC_TYPE_ASYNC_NATIVE,
        .original_function = NULL,
        .async_wrapper = (AsyncFunction)function,
        .is_io_bound = false,
        .avg_execution_time_ns = 1000000,  // 1ms default estimate
        .execution_count = 0
    };
    
    expr->state = EXPR_STATE_CREATED;
    expr->cancel_token = cancellation_token_create();
    
    return expr;
}

Result_void_ptr concurrent_expression_add_dependency(ConcurrentExpression* expr, 
    ConcurrentExpression* dependency) {
    
    if (!expr || !dependency) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid expression or dependency"));
    }
    
    // Reallocate dependencies array if needed
    if (expr->dependency_count == 0) {
        expr->dependencies = malloc(sizeof(ConcurrentExpression*));
    } else {
        expr->dependencies = realloc(expr->dependencies, 
            (expr->dependency_count + 1) * sizeof(ConcurrentExpression*));
    }
    
    if (!expr->dependencies) {
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate dependency"));
    }
    
    expr->dependencies[expr->dependency_count++] = dependency;
    return OK_PTR(expr);
}

void concurrent_expression_destroy(ConcurrentExpression* expr) {
    if (!expr) return;
    
    free(expr->arguments);
    free(expr->dependencies);
    
    if (expr->function) {
        free((void*)expr->function->name);
        free(expr->function);
    }
    
    if (expr->future) {
        async_future_destroy(expr->future);
    }
    
    if (expr->cancel_token) {
        cancellation_token_destroy(expr->cancel_token);
    }
    
    free(expr->result);
    free(expr);
}

// Concurrent block implementation
ConcurrentBlock* concurrent_block_create(const char* name, ConcurrentBlockConfig config) {
    ConcurrentBlock* block = calloc(1, sizeof(ConcurrentBlock));
    if (!block) return NULL;
    
    block->id = generate_id();
    if (name) {
        strncpy(block->name, name, sizeof(block->name) - 1);
        block->name[sizeof(block->name) - 1] = '\0';
    } else {
        snprintf(block->name, sizeof(block->name), "block_%llu", block->id);
    }
    
    block->config = config;
    
    // Create task scope for this block
    TaskScopeConfig scope_config = task_scope_config_default();
    scope_config.max_concurrent_tasks = config.max_concurrent_tasks;
    scope_config.max_memory_per_task = config.max_memory_per_task;
    scope_config.max_total_memory = config.max_total_memory;
    scope_config.default_task_timeout_ms = config.timeout_ms;
    scope_config.propagate_errors = config.fail_fast;
    scope_config.collect_all_errors = config.collect_all_results;
    
    block->task_scope = task_scope_create(scope_config, block->name);
    if (!block->task_scope) {
        free(block);
        return NULL;
    }
    
    // Initialize synchronization
    if (pthread_mutex_init(&block->block_mutex, NULL) != 0) {
        task_scope_destroy(block->task_scope);
        free(block);
        return NULL;
    }
    
    if (pthread_cond_init(&block->all_completed, NULL) != 0) {
        pthread_mutex_destroy(&block->block_mutex);
        task_scope_destroy(block->task_scope);
        free(block);
        return NULL;
    }
    
    block->block_cancel_token = cancellation_token_create();
    if (!block->block_cancel_token) {
        pthread_cond_destroy(&block->all_completed);
        pthread_mutex_destroy(&block->block_mutex);
        task_scope_destroy(block->task_scope);
        free(block);
        return NULL;
    }
    
    // Add to global scheduler if available
    StructuredScheduler* scheduler = structured_get_global_scheduler();
    if (scheduler) {
        pthread_mutex_lock(&scheduler->scheduler_mutex);
        if (scheduler->active_count < scheduler->active_capacity) {
            scheduler->active_blocks[scheduler->active_count++] = block;
            scheduler->total_blocks_created++;
        }
        pthread_mutex_unlock(&scheduler->scheduler_mutex);
    }
    
    return block;
}

Result_void_ptr concurrent_block_add_expression(ConcurrentBlock* block, ConcurrentExpression* expr) {
    if (!block || !expr) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid block or expression"));
    }
    
    pthread_mutex_lock(&block->block_mutex);
    
    // Add expression to linked list
    if (!block->first_expression) {
        block->first_expression = expr;
        block->last_expression = expr;
        expr->next = NULL;
    } else {
        block->last_expression->next = expr;
        block->last_expression = expr;
        expr->next = NULL;
    }
    
    block->expression_count++;
    
    pthread_mutex_unlock(&block->block_mutex);
    
    return OK_PTR(block);
}

// Task wrapper for concurrent expressions with enhanced cancellation propagation
static Result_void_ptr concurrent_expression_task_wrapper(TaskContext* context, void* args) {
    ConcurrentExpression* expr = (ConcurrentExpression*)args;
    
    if (!expr || !expr->function) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid expression"));
    }
    
    // Set up cancellation propagation callback
    if (expr->cancel_token) {
        cancellation_token_add_callback(expr->cancel_token, 
            (void(*)(void*))^(void* ctx) {
                TaskContext* task_ctx = (TaskContext*)ctx;
                atomic_store(&task_ctx->is_cancelled, true);
                atomic_store(&task_ctx->cancel_requested, true);
            }, context);
    }
    
    // Check for cancellation before starting
    CHECK_CANCELLATION(context);
    
    // Wait for dependencies with cancellation checking
    for (size_t i = 0; i < expr->dependency_count; i++) {
        ConcurrentExpression* dep = expr->dependencies[i];
        if (dep) {
            // Wait for dependency completion
            while (dep->state != EXPR_STATE_COMPLETED && 
                   dep->state != EXPR_STATE_FAILED && 
                   dep->state != EXPR_STATE_CANCELLED) {
                
                // Check for cancellation while waiting
                CHECK_CANCELLATION(context);
                
                // Check if dependency was cancelled
                if (cancellation_token_is_cancelled(dep->cancel_token)) {
                    expr->state = EXPR_STATE_CANCELLED;
                    return ERR_PTR(error_create(ERROR_OPERATION_CANCELLED, "Dependency was cancelled"));
                }
                
                // Brief sleep to avoid busy waiting
                usleep(1000); // 1ms
            }
            
            // Check dependency result
            if (dep->state == EXPR_STATE_FAILED) {
                expr->state = EXPR_STATE_FAILED;
                return ERR_PTR(error_create(ERROR_OPERATION_FAILED, "Dependency failed"));
            }
            
            if (dep->state == EXPR_STATE_CANCELLED) {
                expr->state = EXPR_STATE_CANCELLED;
                return ERR_PTR(error_create(ERROR_OPERATION_CANCELLED, "Dependency was cancelled"));
            }
        }
    }
    
    // Final cancellation check before execution
    CHECK_CANCELLATION(context);
    
    // Execute the expression
    expr->state = EXPR_STATE_RUNNING;
    UPDATE_PROGRESS(context, 10, "Starting expression execution");
    
    // Create async context with enhanced cancellation support
    AsyncContext async_ctx = {
        .is_async_context = true,
        .cancel_token = expr->cancel_token,
        .is_cancelled = &context->is_cancelled,
        .runtime = async_runtime_global(),
        .task_id = context->task_id,
        .estimated_cpu_time_us = 1000,
        .estimated_memory_bytes = expr->args_size,
        .parent_context = context
    };
    
    // Call the async function with periodic cancellation checks
    AsyncFunction async_fn = (AsyncFunction)expr->function->async_wrapper;
    
    UPDATE_PROGRESS(context, 50, "Executing expression");
    Result_void_ptr result = async_fn(expr->arguments, &async_ctx);
    
    // Check for cancellation after execution
    if (atomic_load(&context->is_cancelled)) {
        expr->state = EXPR_STATE_CANCELLED;
        if (!result.is_error) {
            // Free result if operation was cancelled but succeeded
            free(result.value);
        }
        return ERR_PTR(error_create(ERROR_OPERATION_CANCELLED, "Expression was cancelled"));
    }
    
    if (result.is_error) {
        expr->state = EXPR_STATE_FAILED;
        expr->error = result.error;
        UPDATE_PROGRESS(context, 100, "Expression failed");
    } else {
        expr->state = EXPR_STATE_COMPLETED;
        expr->result = result.value;
        expr->result_size = async_ctx.estimated_memory_bytes; // Approximate result size
        UPDATE_PROGRESS(context, 100, "Expression completed successfully");
    }
    
    return result;
}

Result_void_ptr concurrent_block_execute(ConcurrentBlock* block) {
    if (!block) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid block"));
    }
    
    // Start the task scope
    Result_void_ptr start_result = task_scope_start(block->task_scope);
    if (start_result.is_error) {
        return start_result;
    }
    
    pthread_mutex_lock(&block->block_mutex);
    
    block->start_time_ns = get_current_time_ns();
    
    // Allocate result arrays
    if (block->config.collect_all_results && block->expression_count > 0) {
        block->results = calloc(block->expression_count, sizeof(void*));
        block->result_sizes = calloc(block->expression_count, sizeof(size_t));
        block->errors = calloc(block->expression_count, sizeof(Error*));
        
        if (!block->results || !block->result_sizes || !block->errors) {
            pthread_mutex_unlock(&block->block_mutex);
            return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate result arrays"));
        }
    }
    
    // Submit all expressions as tasks
    ConcurrentExpression* expr = block->first_expression;
    size_t expr_index = 0;
    
    while (expr) {
        // Create task for expression
        ConcurrentTask* task = task_create(block->task_scope, 
            concurrent_expression_task_wrapper, expr, sizeof(ConcurrentExpression), expr->name);
        
        if (task) {
            task->priority = block->config.default_priority;
            expr->state = EXPR_STATE_SCHEDULED;
            
            // Submit task
            Result_void_ptr submit_result = task_submit(task);
            if (submit_result.is_error && block->config.fail_fast) {
                pthread_mutex_unlock(&block->block_mutex);
                return submit_result;
            }
        } else if (block->config.fail_fast) {
            pthread_mutex_unlock(&block->block_mutex);
            return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create task"));
        }
        
        expr = expr->next;
        expr_index++;
    }
    
    pthread_mutex_unlock(&block->block_mutex);
    
    // Update global scheduler statistics
    StructuredScheduler* scheduler = structured_get_global_scheduler();
    if (scheduler) {
        pthread_mutex_lock(&scheduler->scheduler_mutex);
        scheduler->total_expressions_executed += block->expression_count;
        atomic_fetch_add(&scheduler->total_active_tasks, block->expression_count);
        pthread_mutex_unlock(&scheduler->scheduler_mutex);
    }
    
    return OK_PTR(block);
}

Result_void_ptr concurrent_block_wait(ConcurrentBlock* block, uint64_t timeout_ms) {
    if (!block) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid block"));
    }
    
    pthread_mutex_lock(&block->block_mutex);
    
    // Calculate deadline
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    
    // Wait for all expressions to complete with automatic error cancellation
    ConcurrentExpression* expr = block->first_expression;
    size_t expr_index = 0;
    Result_void_ptr first_error = OK_PTR(NULL);
    bool should_cancel_all = false;
    
    while (expr) {
        // Check expression state with enhanced error handling
        while (expr->state != EXPR_STATE_COMPLETED && 
               expr->state != EXPR_STATE_FAILED && 
               expr->state != EXPR_STATE_CANCELLED) {
            
            // Check if block cancellation was requested
            if (block->is_cancelled || cancellation_token_is_cancelled(block->block_cancel_token)) {
                should_cancel_all = true;
                break;
            }
            
            // Wait with timeout
            int wait_result;
            if (timeout_ms == UINT64_MAX) {
                wait_result = pthread_cond_wait(&block->all_completed, &block->block_mutex);
            } else {
                wait_result = pthread_cond_timedwait(&block->all_completed, &block->block_mutex, &deadline);
            }
            
            if (wait_result == ETIMEDOUT) {
                // Timeout - automatic cancellation of all remaining expressions
                printf("Concurrent block '%s' timed out after %llu ms - cancelling all expressions\n", 
                       block->name, timeout_ms);
                should_cancel_all = true;
                break;
            }
        }
        
        if (should_cancel_all) {
            concurrent_block_cancel(block);
            pthread_mutex_unlock(&block->block_mutex);
            return ERR_PTR(error_create(ERROR_OPERATION_FAILED, "Concurrent block timed out"));
        }
        
        // Collect results and handle automatic error cancellation
        if (block->config.collect_all_results) {
            if (expr->state == EXPR_STATE_COMPLETED) {
                block->results[expr_index] = expr->result;
                block->result_sizes[expr_index] = expr->result_size;
                block->completed_expressions++;
                printf("Expression '%s' completed successfully\n", expr->name);
            } else if (expr->state == EXPR_STATE_FAILED) {
                block->errors[expr_index] = expr->error;
                block->failed_expressions++;
                printf("Expression '%s' failed: %s\n", expr->name, 
                       expr->error ? expr->error->message : "Unknown error");
                
                if (first_error.is_error == false) {
                    first_error = ERR_PTR(expr->error);
                }
                
                // Automatic error cancellation for fail-fast mode
                if (block->config.fail_fast) {
                    printf("Fail-fast enabled - cancelling all remaining expressions due to failure in '%s'\n", 
                           expr->name);
                    
                    // Cancel all remaining expressions
                    ConcurrentExpression* cancel_expr = expr->next;
                    while (cancel_expr) {
                        if (cancel_expr->state != EXPR_STATE_COMPLETED && 
                            cancel_expr->state != EXPR_STATE_FAILED) {
                            
                            cancellation_token_cancel(cancel_expr->cancel_token);
                            cancel_expr->state = EXPR_STATE_CANCELLED;
                            printf("Cancelled expression '%s' due to fail-fast\n", cancel_expr->name);
                        }
                        cancel_expr = cancel_expr->next;
                    }
                    
                    // Cancel the block
                    concurrent_block_cancel(block);
                    break;
                }
            } else if (expr->state == EXPR_STATE_CANCELLED) {
                printf("Expression '%s' was cancelled\n", expr->name);
                block->failed_expressions++; // Count cancelled as failed
            }
        }
        
        expr = expr->next;
        expr_index++;
    }
    
    block->end_time_ns = get_current_time_ns();
    
    // Log block completion statistics
    uint64_t execution_time_ms = (block->end_time_ns - block->start_time_ns) / 1000000;
    printf("Concurrent block '%s' completed in %llu ms: %zu successful, %zu failed\n",
           block->name, execution_time_ms, block->completed_expressions, block->failed_expressions);
    
    pthread_mutex_unlock(&block->block_mutex);
    
    // Update global scheduler statistics
    StructuredScheduler* scheduler = structured_get_global_scheduler();
    if (scheduler) {
        pthread_mutex_lock(&scheduler->scheduler_mutex);
        atomic_fetch_sub(&scheduler->total_active_tasks, block->expression_count);
        
        // Update completion statistics
        scheduler->total_expressions_executed += block->expression_count;
        if (block->failed_expressions > 0) {
            scheduler->total_timeout_events++; // Track blocks that had failures
        }
        
        pthread_mutex_unlock(&scheduler->scheduler_mutex);
    }
    
    return first_error;
}

Result_void_ptr concurrent_block_cancel(ConcurrentBlock* block) {
    if (!block) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid block"));
    }
    
    pthread_mutex_lock(&block->block_mutex);
    
    block->is_cancelled = true;
    
    // Cancel the block's cancellation token
    if (block->block_cancel_token) {
        cancellation_token_cancel(block->block_cancel_token);
    }
    
    // Cancel all expressions
    ConcurrentExpression* expr = block->first_expression;
    while (expr) {
        if (expr->cancel_token) {
            cancellation_token_cancel(expr->cancel_token);
        }
        expr->state = EXPR_STATE_CANCELLED;
        expr = expr->next;
    }
    
    // Wake up waiting threads
    pthread_cond_broadcast(&block->all_completed);
    
    pthread_mutex_unlock(&block->block_mutex);
    
    return OK_PTR(block);
}

void concurrent_block_destroy(ConcurrentBlock* block) {
    if (!block) return;
    
    // Cancel and wait for completion
    concurrent_block_cancel(block);
    
    // Clean up expressions
    ConcurrentExpression* expr = block->first_expression;
    while (expr) {
        ConcurrentExpression* next = expr->next;
        concurrent_expression_destroy(expr);
        expr = next;
    }
    
    // Clean up result arrays
    free(block->results);
    free(block->result_sizes);
    free(block->errors);
    
    // Destroy task scope
    if (block->task_scope) {
        task_scope_shutdown(block->task_scope, 5000);
        task_scope_destroy(block->task_scope);
    }
    
    // Destroy cancellation token
    if (block->block_cancel_token) {
        cancellation_token_destroy(block->block_cancel_token);
    }
    
    // Destroy synchronization primitives
    pthread_mutex_destroy(&block->block_mutex);
    pthread_cond_destroy(&block->all_completed);
    
    // Remove from global scheduler
    StructuredScheduler* scheduler = structured_get_global_scheduler();
    if (scheduler) {
        pthread_mutex_lock(&scheduler->scheduler_mutex);
        for (size_t i = 0; i < scheduler->active_count; i++) {
            if (scheduler->active_blocks[i] == block) {
                // Move last element to this position
                scheduler->active_blocks[i] = scheduler->active_blocks[scheduler->active_count - 1];
                scheduler->active_count--;
                break;
            }
        }
        pthread_mutex_unlock(&scheduler->scheduler_mutex);
    }
    
    free(block);
}

// Timeout decorator implementation
TimeoutDecorator* timeout_decorator_create(uint64_t timeout_ms, TransparentFunction* function) {
    TimeoutDecorator* decorator = calloc(1, sizeof(TimeoutDecorator));
    if (!decorator) return NULL;
    
    decorator->timeout_ms = timeout_ms;
    decorator->wrapped_function = function;
    decorator->timeout_action = TIMEOUT_ACTION_THROW_ERROR;
    
    return decorator;
}

void timeout_decorator_destroy(TimeoutDecorator* decorator) {
    if (!decorator) return;
    free(decorator);
}

Result_void_ptr timeout_decorator_execute(TimeoutDecorator* decorator, void* args, size_t args_size) {
    if (!decorator || !decorator->wrapped_function) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid timeout decorator"));
    }
    
    uint64_t start_time = get_current_time_ns();
    
    // Create a simple timeout mechanism using transparent execution
    Result_void_ptr result = transparent_function_execute(decorator->wrapped_function, args, args_size);
    
    uint64_t elapsed_time = (get_current_time_ns() - start_time) / 1000000; // Convert to ms
    
    decorator->total_calls++;
    decorator->avg_execution_time_ms = (decorator->avg_execution_time_ms + elapsed_time) / 2;
    
    if (elapsed_time > decorator->timeout_ms) {
        decorator->timeout_count++;
        
        switch (decorator->timeout_action) {
            case TIMEOUT_ACTION_CANCEL:
                // Cancel the operation (simplified - in real implementation would use cancellation tokens)
                return ERR_PTR(error_create(ERROR_OPERATION_CANCELLED, "Operation timed out and was cancelled"));
                
            case TIMEOUT_ACTION_RETURN_NULL:
                return OK_PTR(NULL);
                
            case TIMEOUT_ACTION_THROW_ERROR:
            default:
                return ERR_PTR(error_create(ERROR_OPERATION_FAILED, "Operation timed out"));
        }
    }
    
    return result;
}

// Retry decorator implementation
RetryDecorator* retry_decorator_create(size_t max_attempts, uint64_t base_delay_ms, 
    TransparentFunction* function) {
    
    RetryDecorator* decorator = calloc(1, sizeof(RetryDecorator));
    if (!decorator) return NULL;
    
    decorator->max_attempts = max_attempts;
    decorator->base_delay_ms = base_delay_ms;
    decorator->backoff_multiplier = 2.0;
    decorator->max_delay_ms = 60000; // 1 minute max delay
    decorator->wrapped_function = function;
    
    return decorator;
}

void retry_decorator_destroy(RetryDecorator* decorator) {
    if (!decorator) return;
    free(decorator);
}

void retry_decorator_set_condition(RetryDecorator* decorator, 
    bool (*should_retry)(Error* error, size_t attempt, void* context), void* context) {
    
    if (!decorator) return;
    decorator->should_retry = should_retry;
    decorator->retry_context = context;
}

Result_void_ptr retry_decorator_execute(RetryDecorator* decorator, void* args, size_t args_size) {
    if (!decorator || !decorator->wrapped_function) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid retry decorator"));
    }
    
    decorator->total_calls++;
    
    Result_void_ptr last_result = OK_PTR(NULL);
    uint64_t current_delay = decorator->base_delay_ms;
    
    for (size_t attempt = 1; attempt <= decorator->max_attempts; attempt++) {
        Result_void_ptr result = transparent_function_execute(decorator->wrapped_function, args, args_size);
        
        if (!result.is_error) {
            // Success
            if (attempt > 1) {
                decorator->success_after_retry_count++;
            }
            return result;
        }
        
        last_result = result;
        
        // Check if we should retry
        bool should_retry = true;
        if (decorator->should_retry) {
            should_retry = decorator->should_retry(result.error, attempt, decorator->retry_context);
        }
        
        if (!should_retry || attempt == decorator->max_attempts) {
            // Don't retry or max attempts reached
            break;
        }
        
        decorator->retry_count++;
        
        // Sleep with exponential backoff
        if (current_delay > 0) {
            usleep(current_delay * 1000); // Convert ms to µs
        }
        
        current_delay = (uint64_t)(current_delay * decorator->backoff_multiplier);
        if (current_delay > decorator->max_delay_ms) {
            current_delay = decorator->max_delay_ms;
        }
    }
    
    return last_result;
}

// Resource management implementation
ConcurrentResource* concurrent_resource_create(void* resource, void (*cleanup_fn)(void*)) {
    if (!resource || !cleanup_fn) return NULL;
    
    ConcurrentResource* concurrent_resource = malloc(sizeof(ConcurrentResource));
    if (!concurrent_resource) return NULL;
    
    concurrent_resource->resource = resource;
    concurrent_resource->cleanup_function = cleanup_fn;
    concurrent_resource->is_acquired = true;
    
    if (pthread_mutex_init(&concurrent_resource->resource_mutex, NULL) != 0) {
        free(concurrent_resource);
        return NULL;
    }
    
    return concurrent_resource;
}

void concurrent_resource_destroy(ConcurrentResource* resource) {
    if (!resource) return;
    
    pthread_mutex_lock(&resource->resource_mutex);
    
    if (resource->is_acquired && resource->cleanup_function) {
        resource->cleanup_function(resource->resource);
        resource->is_acquired = false;
    }
    
    pthread_mutex_unlock(&resource->resource_mutex);
    pthread_mutex_destroy(&resource->resource_mutex);
    free(resource);
}

// Statistics implementation
StructuredConcurrencyStats structured_concurrency_get_stats(StructuredScheduler* scheduler) {
    StructuredConcurrencyStats stats = {0};
    
    if (!scheduler) return stats;
    
    pthread_mutex_lock(&scheduler->scheduler_mutex);
    
    stats.total_blocks_created = scheduler->total_blocks_created;
    stats.total_expressions_executed = scheduler->total_expressions_executed;
    stats.current_memory_usage = atomic_load(&scheduler->total_memory_used);
    stats.active_blocks = scheduler->active_count;
    stats.timeout_events = scheduler->total_timeout_events;
    stats.retry_events = scheduler->total_retry_events;
    
    // Calculate success rates
    if (stats.total_expressions_executed > 0) {
        stats.expression_success_rate = (double)stats.total_expressions_completed / stats.total_expressions_executed;
    }
    
    if (stats.total_blocks_created > 0) {
        stats.block_success_rate = (double)(stats.total_blocks_created - scheduler->active_count) / stats.total_blocks_created;
    }
    
    pthread_mutex_unlock(&scheduler->scheduler_mutex);
    
    return stats;
}

void structured_concurrency_reset_stats(StructuredScheduler* scheduler) {
    if (!scheduler) return;
    
    pthread_mutex_lock(&scheduler->scheduler_mutex);
    
    scheduler->total_blocks_created = 0;
    scheduler->total_expressions_executed = 0;
    scheduler->total_timeout_events = 0;
    scheduler->total_retry_events = 0;
    atomic_store(&scheduler->total_memory_used, 0);
    
    pthread_mutex_unlock(&scheduler->scheduler_mutex);
}

// For-each parallel execution
Result_void_ptr concurrent_for_each(void** items, size_t item_count, size_t item_size,
    ForEachFunction function, void* context, ConcurrentBlockConfig config) {
    
    if (!items || !function || item_count == 0) {
        return ERR_PTR(error_create(ERROR_INVALID_EXPRESSION, "Invalid parameters"));
    }
    
    // Create concurrent block
    ConcurrentBlock* block = concurrent_block_create("for_each_block", config);
    if (!block) {
        return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to create concurrent block"));
    }
    
    // Create expressions for each item
    for (size_t i = 0; i < item_count; i++) {
        // Create wrapper function for each item
        typedef struct {
            void* item;
            size_t index;
            ForEachFunction function;
            void* user_context;
        } ForEachArgs;
        
        ForEachArgs* args = malloc(sizeof(ForEachArgs));
        if (!args) {
            concurrent_block_destroy(block);
            return ERR_PTR(error_create(ERROR_OUT_OF_MEMORY, "Failed to allocate args"));
        }
        
        args->item = items[i];
        args->index = i;
        args->function = function;
        args->user_context = context;
        
        // Wrapper function
        ConcurrentFunction wrapper = (ConcurrentFunction)^Result_void_ptr(void* wrapper_args, AsyncContext* async_ctx) {
            ForEachArgs* fe_args = (ForEachArgs*)wrapper_args;
            return fe_args->function(fe_args->item, fe_args->index, fe_args->user_context);
        };
        
        char expr_name[64];
        snprintf(expr_name, sizeof(expr_name), "for_each_item_%zu", i);
        
        ConcurrentExpression* expr = concurrent_expression_create(expr_name, wrapper, args, sizeof(ForEachArgs));
        if (expr) {
            concurrent_block_add_expression(block, expr);
        }
        
        free(args); // Expression makes its own copy
    }
    
    // Execute and wait
    Result_void_ptr exec_result = concurrent_block_execute(block);
    if (exec_result.is_error) {
        concurrent_block_destroy(block);
        return exec_result;
    }
    
    Result_void_ptr wait_result = concurrent_block_wait(block, config.timeout_ms);
    concurrent_block_destroy(block);
    
    return wait_result;
}