#ifndef PARALLEL_MEMORY_SAFETY_H
#define PARALLEL_MEMORY_SAFETY_H

#include "structured_concurrency.h"
#include "work_stealing.h"

// Forward declaration for memory safety context
typedef struct MemorySafetyContext MemorySafetyContext;

// Define ParallelForTaskArgs here since it's needed
typedef struct ParallelForTaskArgs {
    size_t start;
    size_t end;
    ParallelForFunction func;
    void* user_context;
} ParallelForTaskArgs;

// Memory-safe parallel for configuration
typedef struct MemorySafeParallelConfig {
    ParallelForConfig base_config;
    
    // Memory safety options
    bool enable_bounds_checking;
    bool enable_ownership_validation;
    bool strict_task_isolation;
    size_t max_memory_per_task;
    
    // Access control
    void** allowed_memory_regions;
    size_t* region_sizes;
    size_t region_count;
    
    // Validation callbacks
    bool (*validate_memory_access)(void* address, size_t size, bool is_write, uint64_t task_id);
    void (*on_violation)(uint64_t task_id, void* address, size_t size, const char* error);
} MemorySafeParallelConfig;

// Memory-safe task wrapper arguments
typedef struct MemorySafeTaskArgs {
    ParallelForTaskArgs base_args;
    
    // Memory safety context
    MemorySafetyContext* safety_context;
    MemorySafeParallelConfig* safety_config;
    
    // Task-specific memory limits
    void* task_memory_base;
    size_t task_memory_size;
    atomic_size_t task_memory_used;
    
    // Access tracking
    atomic_uint_fast32_t memory_accesses;
    atomic_uint_fast32_t bounds_violations;
} MemorySafeTaskArgs;

// Function prototypes
Result_void_ptr memory_safe_parallel_for(
    TaskScope* scope,
    MemorySafeParallelConfig config,
    ParallelForFunction function,
    void* context
);

Result_void_ptr memory_safe_work_stealing_parallel_for(
    WorkStealingScope* scope,
    MemorySafeParallelConfig config,
    ParallelForFunction function,
    void* context
);

// Memory-safe task wrapper
Result_void_ptr memory_safe_task_wrapper(TaskContext* task_ctx, void* args);

// Memory validation functions
bool validate_parallel_memory_access(MemorySafetyContext* ctx, uint64_t task_id, 
                                    void* address, size_t size, bool is_write);
bool check_task_memory_bounds(MemorySafeTaskArgs* args, void* address, size_t size);
void report_parallel_memory_violation(MemorySafeTaskArgs* args, void* address, 
                                     size_t size, const char* violation_type);

// Task memory management
bool allocate_task_memory(MemorySafeTaskArgs* args, size_t requested_size);
void cleanup_task_memory(MemorySafeTaskArgs* args);

// Configuration helpers
MemorySafeParallelConfig memory_safe_parallel_config_default(void);
MemorySafeParallelConfig memory_safe_parallel_config_strict(void);
MemorySafeParallelConfig memory_safe_parallel_config_debug(void);

// Integration with existing safety systems
MemorySafetyContext* create_parallel_safety_context(size_t worker_count);
void register_parallel_memory_regions(MemorySafetyContext* ctx, 
                                     void** regions, size_t* sizes, size_t count);

#endif // PARALLEL_MEMORY_SAFETY_H