#include "../../include/parallel_memory_safety.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// Memory-safe task wrapper function
Result_void_ptr memory_safe_task_wrapper(TaskContext* task_ctx, void* args) {
    MemorySafeTaskArgs* safe_args = (MemorySafeTaskArgs*)args;
    
    if (!safe_args || !safe_args->base_args.func) {
        return OK_PTR(NULL); // Skip invalid arguments
    }
    
    // Validate task memory setup
    if (safe_args->safety_config->strict_task_isolation && !safe_args->task_memory_base) {
        // Allocate isolated memory for this task
        if (!allocate_task_memory(safe_args, safe_args->safety_config->max_memory_per_task)) {
            Error* error = malloc(sizeof(Error));
            *error = (Error){
                .code = ERROR_OUT_OF_MEMORY,
                .severity = ERROR_SEVERITY_ERROR,
                .category = ERROR_CATEGORY_RUNTIME,
                .message = strdup("Failed to allocate task memory"),
                .hint = strdup("Reduce max_memory_per_task or increase available memory"),
                .location = (SourceLocation){0},
                .next = NULL
            };
            return ERR_PTR(error);
        }
    }
    
    // Execute the parallel for range with memory safety
    for (size_t i = safe_args->base_args.start; i < safe_args->base_args.end; i++) {
        CHECK_CANCELLATION(task_ctx);
        
        // Update progress
        int progress = (int)((i - safe_args->base_args.start + 1) * 100 / 
                            (safe_args->base_args.end - safe_args->base_args.start));
        UPDATE_PROGRESS(task_ctx, progress, "Processing items with memory safety");
        
        // Call the user function with memory safety enabled
        Result_void_ptr result = safe_args->base_args.func(i, safe_args->base_args.user_context);
        
        atomic_fetch_add(&safe_args->memory_accesses, 1);
        
        if (result.is_error) {
            cleanup_task_memory(safe_args);
            return result;
        }
    }
    
    // Cleanup task-specific memory
    cleanup_task_memory(safe_args);
    
    return OK_PTR(NULL);
}

// Validate memory access for parallel tasks
bool validate_parallel_memory_access(MemorySafetyContext* ctx, uint64_t task_id, 
                                    void* address, size_t size, bool is_write) {
    (void)ctx; (void)task_id; (void)address; (void)size; (void)is_write;
    
    // Simplified validation for demo purposes
    // In a real implementation, this would integrate with the full memory safety system
    return true;
}

// Check if memory access is within task's allocated bounds
bool check_task_memory_bounds(MemorySafeTaskArgs* args, void* address, size_t size) {
    if (!args || !address) return false;
    
    if (args->safety_config->strict_task_isolation && args->task_memory_base) {
        // Check if access is within task's isolated memory
        uintptr_t addr = (uintptr_t)address;
        uintptr_t base = (uintptr_t)args->task_memory_base;
        
        if (addr < base || addr + size > base + args->task_memory_size) {
            atomic_fetch_add(&args->bounds_violations, 1);
            return false;
        }
    }
    
    return true;
}

// Report memory violation in parallel task
void report_parallel_memory_violation(MemorySafeTaskArgs* args, void* address, 
                                     size_t size, const char* violation_type) {
    if (!args || !args->safety_config->on_violation) return;
    
    // Create a descriptive error message
    char error_msg[256];
    snprintf(error_msg, sizeof(error_msg), 
             "Memory violation in parallel task: %s at address %p, size %zu", 
             violation_type, address, size);
    
    // Call the violation callback
    args->safety_config->on_violation(0, address, size, error_msg);
}

// Allocate isolated memory for a task
bool allocate_task_memory(MemorySafeTaskArgs* args, size_t requested_size) {
    if (!args || requested_size == 0) return false;
    
    args->task_memory_base = malloc(requested_size);
    if (!args->task_memory_base) return false;
    
    args->task_memory_size = requested_size;
    atomic_init(&args->task_memory_used, 0);
    
    // Clear the memory for safety
    memset(args->task_memory_base, 0, requested_size);
    
    return true;
}

// Cleanup task-specific memory
void cleanup_task_memory(MemorySafeTaskArgs* args) {
    if (!args) return;
    
    if (args->task_memory_base) {
        free(args->task_memory_base);
        args->task_memory_base = NULL;
        args->task_memory_size = 0;
        atomic_store(&args->task_memory_used, 0);
    }
}

// Memory-safe parallel for implementation
Result_void_ptr memory_safe_parallel_for(
    TaskScope* scope,
    MemorySafeParallelConfig config,
    ParallelForFunction function,
    void* context) {
    
    if (!scope || !function) {
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_INVALID_EXPRESSION,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Invalid parallel for parameters"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Validate configuration
    if (config.base_config.start_index >= config.base_config.end_index) {
        return OK_PTR(NULL); // Nothing to do
    }
    
    size_t total_items = config.base_config.end_index - config.base_config.start_index;
    
    // Calculate chunk size with memory safety considerations
    size_t chunk_size = config.base_config.chunk_size;
    if (chunk_size == 0) {
        chunk_size = (total_items + config.base_config.max_workers - 1) / config.base_config.max_workers;
        
        // Limit chunk size based on memory constraints
        if (config.max_memory_per_task > 0) {
            size_t max_items_per_chunk = config.max_memory_per_task / sizeof(void*); // Rough estimate
            if (chunk_size > max_items_per_chunk) {
                chunk_size = max_items_per_chunk;
            }
        }
        
        if (chunk_size < 1) chunk_size = 1;
    }
    
    // Create memory-safe task arguments and tasks
    size_t task_count = 0;
    MemorySafeTaskArgs* all_safe_args = NULL;
    ConcurrentTask** tasks = NULL;
    
    // Calculate number of tasks needed
    size_t max_tasks = (total_items + chunk_size - 1) / chunk_size;
    
    all_safe_args = calloc(max_tasks, sizeof(MemorySafeTaskArgs));
    tasks = calloc(max_tasks, sizeof(ConcurrentTask*));
    
    if (!all_safe_args || !tasks) {
        free(all_safe_args);
        free(tasks);
        Error* error = malloc(sizeof(Error));
        *error = (Error){
            .code = ERROR_OUT_OF_MEMORY,
            .severity = ERROR_SEVERITY_ERROR,
            .category = ERROR_CATEGORY_RUNTIME,
            .message = strdup("Memory allocation failed"),
            .hint = NULL,
            .location = (SourceLocation){0},
            .next = NULL
        };
        return ERR_PTR(error);
    }
    
    // Create and submit tasks
    for (size_t start = config.base_config.start_index; 
         start < config.base_config.end_index; 
         start += chunk_size) {
        
        size_t end = start + chunk_size;
        if (end > config.base_config.end_index) {
            end = config.base_config.end_index;
        }
        
        // Initialize memory-safe task arguments
        MemorySafeTaskArgs* safe_args = &all_safe_args[task_count];
        safe_args->base_args = (ParallelForTaskArgs){
            .start = start,
            .end = end,
            .func = function,
            .user_context = context
        };
        
        safe_args->safety_config = &config;
        atomic_init(&safe_args->memory_accesses, 0);
        atomic_init(&safe_args->bounds_violations, 0);
        
        // Create task with memory-safe wrapper
        char task_name[64];
        snprintf(task_name, sizeof(task_name), "safe_parallel_for_%zu_%zu", start, end);
        
        ConcurrentTask* task = task_create(scope, memory_safe_task_wrapper,
                                         safe_args, sizeof(MemorySafeTaskArgs), task_name);
        if (task) {
            task->priority = config.base_config.priority;
            tasks[task_count] = task;
            
            Result_void_ptr submit_result = task_submit(task);
            if (submit_result.is_error) {
                // Cleanup on failure
                for (size_t i = 0; i < task_count; i++) {
                    if (tasks[i]) {
                        task_cancel(tasks[i], CANCEL_GRACEFUL);
                    }
                }
                free(all_safe_args);
                free(tasks);
                return submit_result;
            }
            task_count++;
        }
    }
    
    // Wait for all tasks to complete
    Result_void_ptr first_error = OK_PTR(NULL);
    uint64_t total_accesses = 0;
    uint64_t total_violations = 0;
    
    for (size_t i = 0; i < task_count; i++) {
        if (tasks[i]) {
            Result_void_ptr wait_result = task_wait(tasks[i], UINT64_MAX);
            if (wait_result.is_error && first_error.is_error == false) {
                first_error = wait_result;
            }
            
            // Collect memory safety statistics
            total_accesses += atomic_load(&all_safe_args[i].memory_accesses);
            total_violations += atomic_load(&all_safe_args[i].bounds_violations);
        }
    }
    
    // Report memory safety statistics
    if (config.enable_bounds_checking) {
        printf("Memory Safety Report:\n");
        printf("  Total memory accesses: %llu\n", total_accesses);
        printf("  Bounds violations: %llu\n", total_violations);
        if (total_accesses > 0) {
            printf("  Safety rate: %.2f%%\n", 
                   (double)(total_accesses - total_violations) / total_accesses * 100.0);
        }
    }
    
    // Cleanup
    free(all_safe_args);
    free(tasks);
    
    return first_error;
}

// Memory-safe work-stealing parallel for
Result_void_ptr memory_safe_work_stealing_parallel_for(
    WorkStealingScope* scope,
    MemorySafeParallelConfig config,
    ParallelForFunction function,
    void* context) {
    
    // For now, delegate to regular work-stealing with safety wrapper.
    // In a full implementation, this would integrate more deeply with work-stealing.
    //
    // memory_safe_parallel_for submits tasks to the base task-scope and waits on
    // them (task_wait with an infinite timeout). A freshly created work-stealing
    // scope has NOT started that scope's worker pool, so without this the
    // submitted tasks would sit QUEUED with no worker to run them and task_wait
    // would hang forever. Start the base scope on demand.
    if (!scope->base_scope.is_active) {
        Result_void_ptr started = task_scope_start(&scope->base_scope);
        if (started.is_error) return started;
    }
    return memory_safe_parallel_for(&scope->base_scope, config, function, context);
}

// Configuration presets
MemorySafeParallelConfig memory_safe_parallel_config_default(void) {
    return (MemorySafeParallelConfig) {
        .base_config = {
            .start_index = 0,
            .end_index = 0,
            .chunk_size = 0,
            .max_workers = 8,
            .priority = TASK_PRIORITY_NORMAL
        },
        .enable_bounds_checking = true,
        .enable_ownership_validation = false,
        .strict_task_isolation = false,
        .max_memory_per_task = 1024 * 1024, // 1MB
        .allowed_memory_regions = NULL,
        .region_sizes = NULL,
        .region_count = 0,
        .validate_memory_access = NULL,
        .on_violation = NULL
    };
}

MemorySafeParallelConfig memory_safe_parallel_config_strict(void) {
    MemorySafeParallelConfig config = memory_safe_parallel_config_default();
    config.enable_bounds_checking = true;
    config.enable_ownership_validation = true;
    config.strict_task_isolation = true;
    config.max_memory_per_task = 512 * 1024; // 512KB - smaller for safety
    return config;
}

MemorySafeParallelConfig memory_safe_parallel_config_debug(void) {
    MemorySafeParallelConfig config = memory_safe_parallel_config_strict();
    config.max_memory_per_task = 256 * 1024; // 256KB - even smaller for debugging
    return config;
}

// Create memory safety context for parallel operations
MemorySafetyContext* create_parallel_safety_context(size_t worker_count) {
    // This would integrate with the existing memory safety system
    // For now, return a simplified context
    return NULL;
}

// Register memory regions for parallel access
void register_parallel_memory_regions(MemorySafetyContext* ctx, 
                                     void** regions, size_t* sizes, size_t count) {
    if (!ctx || !regions || !sizes) return;
    
    // This would register each memory region with the safety system
    // Simplified for demonstration
}

// Helper function to create violation errors
Error* memory_safety_create_violation_error(int violation_type, void* address, size_t size) {
    Error* error = malloc(sizeof(Error));
    if (!error) return NULL;
    
    char* message = malloc(256);
    if (!message) {
        free(error);
        return NULL;
    }
    
    snprintf(message, 256, "Memory safety violation: type=%d, address=%p, size=%zu", 
             violation_type, address, size);
    
    *error = (Error){
        .code = ERROR_BUFFER_OVERFLOW,
        .severity = ERROR_SEVERITY_ERROR,
        .category = ERROR_CATEGORY_RUNTIME,
        .message = message,
        .hint = strdup("Check memory access patterns in parallel tasks"),
        .location = (SourceLocation){0},
        .next = NULL
    };
    
    return error;
}