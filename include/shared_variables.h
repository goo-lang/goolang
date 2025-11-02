#ifndef GOO_SHARED_VARIABLES_H
#define GOO_SHARED_VARIABLES_H

#include "runtime.h"
#include "actor_system.h"
#include "error_hierarchies.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

// =============================================================================
// Shared Variables with Automatic Synchronization
// =============================================================================

// Forward declarations
typedef struct SharedVar SharedVar;
typedef struct SharedVarSystem SharedVarSystem;
typedef struct SharedVarRegistry SharedVarRegistry;
typedef struct AtomicOperation AtomicOperation;
typedef struct SharedVarWatcher SharedVarWatcher;
typedef struct ParallelContext ParallelContext;

// =============================================================================
// Atomic Operation Types
// =============================================================================

// Types of atomic operations supported
typedef enum {
    ATOMIC_OP_READ,         // Read value
    ATOMIC_OP_WRITE,        // Write value
    ATOMIC_OP_EXCHANGE,     // Exchange value
    ATOMIC_OP_COMPARE_SWAP, // Compare and swap
    ATOMIC_OP_ADD,          // Atomic addition
    ATOMIC_OP_SUB,          // Atomic subtraction
    ATOMIC_OP_MUL,          // Atomic multiplication
    ATOMIC_OP_DIV,          // Atomic division
    ATOMIC_OP_AND,          // Bitwise AND
    ATOMIC_OP_OR,           // Bitwise OR
    ATOMIC_OP_XOR,          // Bitwise XOR
    ATOMIC_OP_INCREMENT,    // Atomic increment
    ATOMIC_OP_DECREMENT,    // Atomic decrement
    ATOMIC_OP_MIN,          // Atomic minimum
    ATOMIC_OP_MAX           // Atomic maximum
} AtomicOperationType;

// Memory ordering for atomic operations
typedef enum {
    MEMORY_ORDER_RELAXED = __ATOMIC_RELAXED,
    MEMORY_ORDER_ACQUIRE = __ATOMIC_ACQUIRE,
    MEMORY_ORDER_RELEASE = __ATOMIC_RELEASE,
    MEMORY_ORDER_ACQ_REL = __ATOMIC_ACQ_REL,
    MEMORY_ORDER_SEQ_CST = __ATOMIC_SEQ_CST
} MemoryOrder;

// =============================================================================
// Shared Variable Types
// =============================================================================

// Supported types for shared variables
typedef enum {
    SHARED_TYPE_INT8,       // 8-bit signed integer
    SHARED_TYPE_INT16,      // 16-bit signed integer
    SHARED_TYPE_INT32,      // 32-bit signed integer
    SHARED_TYPE_INT64,      // 64-bit signed integer
    SHARED_TYPE_UINT8,      // 8-bit unsigned integer
    SHARED_TYPE_UINT16,     // 16-bit unsigned integer
    SHARED_TYPE_UINT32,     // 32-bit unsigned integer
    SHARED_TYPE_UINT64,     // 64-bit unsigned integer
    SHARED_TYPE_FLOAT32,    // 32-bit floating point
    SHARED_TYPE_FLOAT64,    // 64-bit floating point
    SHARED_TYPE_BOOL,       // Boolean value
    SHARED_TYPE_POINTER,    // Pointer value
    SHARED_TYPE_CUSTOM      // Custom type with user-defined operations
} SharedVarType;

// Configuration for shared variables
typedef struct SharedVarConfig {
    const char* name;               // Variable name (for debugging)
    SharedVarType type;             // Type of the shared variable
    size_t size;                    // Size in bytes
    MemoryOrder default_order;      // Default memory ordering
    bool enable_watching;           // Enable change notifications
    bool enable_statistics;         // Collect usage statistics
    uint64_t cache_line_padding;    // Padding to avoid false sharing
} SharedVarConfig;

// Statistics for shared variable usage
typedef struct SharedVarStats {
    uint64_t reads;                 // Total read operations
    uint64_t writes;                // Total write operations
    uint64_t atomic_ops;            // Total atomic operations
    uint64_t contentions;           // Contention events
    uint64_t cache_misses;          // Estimated cache misses
    double avg_access_time_ns;      // Average access time
    uint64_t last_modified;         // Last modification timestamp
} SharedVarStats;

// =============================================================================
// Shared Variable Structure
// =============================================================================

// Core shared variable structure
typedef struct SharedVar {
    // Identification
    uint64_t var_id;                // Unique variable ID
    const char* name;               // Variable name
    SharedVarType type;             // Variable type
    
    // Data storage (aligned to cache line)
    union {
        atomic_int_least8_t   int8_val;
        atomic_int_least16_t  int16_val;
        atomic_int_least32_t  int32_val;
        atomic_int_least64_t  int64_val;
        atomic_uint_least8_t  uint8_val;
        atomic_uint_least16_t uint16_val;
        atomic_uint_least32_t uint32_val;
        atomic_uint_least64_t uint64_val;
        atomic_bool           bool_val;
        atomic_uintptr_t      ptr_val;
        
        // For custom types, we use a pointer with locks
        struct {
            void* data;
            pthread_rwlock_t rwlock;
            size_t size;
        } custom;
    } storage __attribute__((aligned(64))); // Cache line aligned
    
    // Configuration
    SharedVarConfig config;         // Variable configuration
    
    // Watchers (for change notifications)
    SharedVarWatcher** watchers;    // Array of watchers
    int watcher_count;              // Number of watchers
    int watcher_capacity;           // Watcher array capacity
    pthread_mutex_t watcher_mutex;  // Watcher list protection
    
    // Statistics
    SharedVarStats stats;           // Usage statistics
    pthread_mutex_t stats_mutex;    // Statistics protection
    
    // Custom operations (for complex types)
    struct {
        void* (*read_func)(SharedVar* var, void* dest, MemoryOrder order);
        bool (*write_func)(SharedVar* var, const void* src, MemoryOrder order);
        bool (*compare_swap_func)(SharedVar* var, void* expected, const void* desired, MemoryOrder order);
        void* (*atomic_op_func)(SharedVar* var, AtomicOperationType op, const void* operand, MemoryOrder order);
    } custom_ops;
    
    // Memory management
    Arena* var_arena;               // Arena for variable-specific memory
    
} SharedVar;

// =============================================================================
// Change Notification System
// =============================================================================

// Types of change events
typedef enum {
    CHANGE_EVENT_WRITE,             // Variable was written
    CHANGE_EVENT_ATOMIC_OP,         // Atomic operation performed
    CHANGE_EVENT_DESTROYED          // Variable was destroyed
} ChangeEventType;

// Change event structure
typedef struct ChangeEvent {
    uint64_t event_id;              // Unique event ID
    SharedVar* variable;            // Variable that changed
    ChangeEventType type;           // Type of change
    uint64_t timestamp;             // When change occurred
    
    // Value information
    void* old_value;                // Previous value (if available)
    void* new_value;                // New value
    size_t value_size;              // Size of values
    
    // Operation context
    AtomicOperationType atomic_op;  // Atomic operation type (if applicable)
    MemoryOrder memory_order;       // Memory ordering used
    uint64_t thread_id;             // Thread that made the change
    
} ChangeEvent;

// Watcher callback function
typedef void (*SharedVarWatcherCallback)(const ChangeEvent* event, void* context);

// Watcher structure
typedef struct SharedVarWatcher {
    uint64_t watcher_id;            // Unique watcher ID
    SharedVarWatcherCallback callback; // Callback function
    void* context;                  // User context
    ChangeEventType event_mask;     // Which events to watch
    bool is_active;                 // Whether watcher is active
} SharedVarWatcher;

// =============================================================================
// Parallel Processing Framework
// =============================================================================

// Parallel loop types
typedef enum {
    PARALLEL_FOR,                   // Parallel for loop
    PARALLEL_FOREACH,               // Parallel foreach loop
    PARALLEL_MAP,                   // Parallel map operation
    PARALLEL_REDUCE,                // Parallel reduce operation
    PARALLEL_SCAN                   // Parallel scan operation
} ParallelLoopType;

// Parallel loop function types
typedef void (*ParallelForFunc)(int64_t index, void* context);
typedef void (*ParallelForeachFunc)(void* item, int64_t index, void* context);
typedef void* (*ParallelMapFunc)(void* item, int64_t index, void* context);
typedef void* (*ParallelReduceFunc)(void* accumulator, void* item, int64_t index, void* context);

// Parallel execution context
typedef struct ParallelContext {
    uint64_t context_id;            // Unique context ID
    ParallelLoopType type;          // Type of parallel operation
    
    // Range information
    int64_t start;                  // Start index
    int64_t end;                    // End index (exclusive)
    int64_t step;                   // Step size
    
    // Thread pool configuration
    int thread_count;               // Number of threads to use
    bool use_work_stealing;         // Use work-stealing scheduler
    
    // Shared variables accessible in this context
    SharedVar** shared_vars;        // Array of shared variables
    int shared_var_count;           // Number of shared variables
    
    // Error handling
    StructuredError** errors;       // Errors from parallel execution
    int error_count;                // Number of errors
    pthread_mutex_t error_mutex;    // Error array protection
    
    // Synchronization
    pthread_barrier_t start_barrier; // Synchronize thread start
    pthread_barrier_t end_barrier;   // Synchronize thread completion
    
    // Statistics
    struct {
        uint64_t start_time;
        uint64_t end_time;
        uint64_t total_operations;
        uint64_t cache_misses;
        double cpu_utilization;
    } stats;
    
} ParallelContext;

// =============================================================================
// Shared Variable System
// =============================================================================

// Global shared variable registry
typedef struct SharedVarRegistry {
    SharedVar** variables;          // Array of registered variables
    int var_count;                  // Number of variables
    int var_capacity;               // Capacity of variables array
    pthread_rwlock_t registry_lock; // Registry protection
    
    // ID generation
    atomic_uint_least64_t next_var_id;     // Next variable ID
    atomic_uint_least64_t next_watcher_id; // Next watcher ID
    atomic_uint_least64_t next_event_id;   // Next event ID
    
    // Statistics
    struct {
        uint64_t variables_created;
        uint64_t variables_destroyed;
        uint64_t watchers_registered;
        uint64_t events_generated;
        uint64_t parallel_contexts_created;
    } global_stats;
    
} SharedVarRegistry;

// Main shared variable system
typedef struct SharedVarSystem {
    const char* system_name;        // System name
    SharedVarRegistry* registry;    // Variable registry
    
    // Parallel execution
    pthread_t* worker_threads;      // Worker thread pool
    int worker_count;               // Number of worker threads
    bool workers_active;            // Whether workers are running
    
    // Work queue for parallel operations
    ParallelContext** work_queue;   // Queue of parallel contexts
    int queue_size;                 // Current queue size
    int queue_capacity;             // Queue capacity
    pthread_mutex_t queue_mutex;    // Queue protection
    pthread_cond_t work_available;  // Work availability signal
    
    // Configuration
    struct {
        int default_thread_count;   // Default number of threads
        bool enable_work_stealing;  // Enable work stealing
        bool numa_aware;            // NUMA-aware scheduling
        uint64_t cache_line_size;   // CPU cache line size
        bool enable_statistics;     // Collect statistics
    } config;
    
    // Memory management
    Arena* system_arena;            // System memory arena
    
} SharedVarSystem;

// =============================================================================
// Core API Functions
// =============================================================================

// System initialization
SharedVarSystem* shared_var_system_create(const char* name);
void shared_var_system_destroy(SharedVarSystem* system);
bool shared_var_system_start(SharedVarSystem* system);
void shared_var_system_stop(SharedVarSystem* system);

// Get global system
SharedVarSystem* get_global_shared_var_system(void);

// Variable creation and management
SharedVar* shared_var_create(const char* name, SharedVarType type, const void* initial_value);
SharedVar* shared_var_create_with_config(const SharedVarConfig* config, const void* initial_value);
void shared_var_destroy(SharedVar* var);

// Basic operations
void* shared_var_read(SharedVar* var, void* dest, MemoryOrder order);
bool shared_var_write(SharedVar* var, const void* src, MemoryOrder order);
bool shared_var_compare_swap(SharedVar* var, void* expected, const void* desired, MemoryOrder order);
void* shared_var_exchange(SharedVar* var, const void* new_value, MemoryOrder order);

// Atomic arithmetic operations
int64_t shared_var_atomic_add_int(SharedVar* var, int64_t value, MemoryOrder order);
int64_t shared_var_atomic_sub_int(SharedVar* var, int64_t value, MemoryOrder order);
int64_t shared_var_atomic_inc_int(SharedVar* var, MemoryOrder order);
int64_t shared_var_atomic_dec_int(SharedVar* var, MemoryOrder order);

// Atomic bitwise operations
uint64_t shared_var_atomic_and_uint(SharedVar* var, uint64_t value, MemoryOrder order);
uint64_t shared_var_atomic_or_uint(SharedVar* var, uint64_t value, MemoryOrder order);
uint64_t shared_var_atomic_xor_uint(SharedVar* var, uint64_t value, MemoryOrder order);

// Atomic min/max operations
int64_t shared_var_atomic_min_int(SharedVar* var, int64_t value, MemoryOrder order);
int64_t shared_var_atomic_max_int(SharedVar* var, int64_t value, MemoryOrder order);

// =============================================================================
// Change Notification API
// =============================================================================

// Watcher management
SharedVarWatcher* shared_var_add_watcher(SharedVar* var, SharedVarWatcherCallback callback, 
                                        void* context, ChangeEventType event_mask);
void shared_var_remove_watcher(SharedVar* var, SharedVarWatcher* watcher);
void shared_var_notify_watchers(SharedVar* var, const ChangeEvent* event);

// =============================================================================
// Parallel Processing API
// =============================================================================

// Parallel for loops
void parallel_for(int64_t start, int64_t end, ParallelForFunc func, void* context);
void parallel_for_with_threads(int64_t start, int64_t end, int thread_count, 
                              ParallelForFunc func, void* context);

// Parallel foreach
void parallel_foreach(void** items, int64_t count, ParallelForeachFunc func, void* context);

// Parallel map
void** parallel_map(void** items, int64_t count, ParallelMapFunc func, void* context);

// Parallel reduce
void* parallel_reduce(void** items, int64_t count, void* initial_value, 
                     ParallelReduceFunc func, void* context);

// Advanced parallel context
ParallelContext* parallel_context_create(ParallelLoopType type, int64_t start, int64_t end);
void parallel_context_add_shared_var(ParallelContext* context, SharedVar* var);
void parallel_context_execute(ParallelContext* context, void* func, void* user_context);
void parallel_context_destroy(ParallelContext* context);

// =============================================================================
// Convenience Macros and Type-Safe Wrappers
// =============================================================================

// Type-safe variable creation macros
#define SHARED_INT(name, initial) \
    shared_var_create((name), SHARED_TYPE_INT64, &(int64_t){(initial)})
    
#define SHARED_UINT(name, initial) \
    shared_var_create((name), SHARED_TYPE_UINT64, &(uint64_t){(initial)})
    
#define SHARED_FLOAT(name, initial) \
    shared_var_create((name), SHARED_TYPE_FLOAT64, &(double){(initial)})
    
#define SHARED_BOOL(name, initial) \
    shared_var_create((name), SHARED_TYPE_BOOL, &(bool){(initial)})

// Type-safe operation macros
#define SHARED_READ_INT(var) \
    ({ int64_t val; shared_var_read((var), &val, MEMORY_ORDER_SEQ_CST); val; })
    
#define SHARED_WRITE_INT(var, value) \
    shared_var_write((var), &(int64_t){(value)}, MEMORY_ORDER_SEQ_CST)
    
#define SHARED_INC(var) \
    shared_var_atomic_inc_int((var), MEMORY_ORDER_SEQ_CST)
    
#define SHARED_DEC(var) \
    shared_var_atomic_dec_int((var), MEMORY_ORDER_SEQ_CST)
    
#define SHARED_ADD(var, value) \
    shared_var_atomic_add_int((var), (value), MEMORY_ORDER_SEQ_CST)

// Parallel loop macros
#define PARALLEL_FOR(start, end, body) \
    parallel_for((start), (end), \
        (ParallelForFunc)[](int64_t i, void* ctx) { body; }, NULL)
        
#define PARALLEL_FOR_THREADS(start, end, threads, body) \
    parallel_for_with_threads((start), (end), (threads), \
        (ParallelForFunc)[](int64_t i, void* ctx) { body; }, NULL)

// =============================================================================
// Statistics and Monitoring
// =============================================================================

// Get variable statistics
SharedVarStats shared_var_get_stats(SharedVar* var);
void shared_var_reset_stats(SharedVar* var);

// System-wide statistics
void shared_var_system_print_stats(SharedVarSystem* system);
void shared_var_registry_print_stats(SharedVarRegistry* registry);

// Performance monitoring
typedef struct SharedVarPerformanceReport {
    uint64_t total_variables;
    uint64_t total_operations;
    uint64_t cache_misses;
    double avg_contention_ratio;
    double parallel_efficiency;
    uint64_t memory_usage_bytes;
} SharedVarPerformanceReport;

SharedVarPerformanceReport shared_var_system_get_performance_report(SharedVarSystem* system);

// =============================================================================
// Debugging and Introspection
// =============================================================================

// Debug information
void shared_var_dump_info(SharedVar* var);
void shared_var_dump_watchers(SharedVar* var);
void shared_var_system_dump_registry(SharedVarSystem* system);

// Memory layout analysis
void shared_var_analyze_memory_layout(SharedVar** vars, int count);
void shared_var_check_false_sharing(SharedVar** vars, int count);

// =============================================================================
// Integration with Actor System
// =============================================================================

// Actor-shared variable integration
void shared_var_register_with_actor(SharedVar* var, ActorRef* actor);
void shared_var_notify_actor_on_change(SharedVar* var, ActorRef* actor, const char* message);

// Shared variables in actor context
SharedVar* actor_create_shared_var(ActorRef* actor, const char* name, SharedVarType type, 
                                  const void* initial_value);
void actor_watch_shared_var(ActorRef* actor, SharedVar* var, const char* handler_name);

#endif // GOO_SHARED_VARIABLES_H