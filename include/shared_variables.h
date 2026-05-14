#ifndef GOO_SHARED_VARIABLES_H
#define GOO_SHARED_VARIABLES_H

#include <stdint.h>
#include <stdbool.h>
#include "ccomp_shim.h"
#include <pthread.h>
#include <stddef.h>
#include <time.h>
#include "ergonomic_errors.h"

// Forward declarations
typedef struct SharedVariable SharedVariable;
typedef struct SharedVarManager SharedVarManager;
typedef struct SyncGroup SyncGroup;

// Synchronization modes for shared variables
typedef enum {
    SYNC_MODE_ATOMIC,      // Lock-free atomic operations
    SYNC_MODE_MUTEX,       // Traditional mutex-based synchronization
    SYNC_MODE_RW_LOCK,     // Reader-writer lock for read-heavy workloads
    SYNC_MODE_SPIN_LOCK,   // Spin lock for short critical sections
    SYNC_MODE_ADAPTIVE,    // Automatically choose best synchronization
    SYNC_MODE_WAIT_FREE,   // Wait-free algorithms when possible
    SYNC_MODE_CUSTOM       // Custom synchronization with user-provided operations
} SyncMode;

// Data types that can be shared safely
typedef enum {
    SHARED_TYPE_INT32,
    SHARED_TYPE_INT64,
    SHARED_TYPE_UINT32,
    SHARED_TYPE_UINT64,
    SHARED_TYPE_FLOAT32,
    SHARED_TYPE_FLOAT64,
    SHARED_TYPE_BOOL,
    SHARED_TYPE_PTR,
    SHARED_TYPE_STRING,
    SHARED_TYPE_CUSTOM     // User-defined types with custom synchronization
} SharedVarType;

// Access patterns for optimization
typedef enum {
    ACCESS_PATTERN_UNKNOWN,
    ACCESS_PATTERN_READ_HEAVY,    // Mostly reads, few writes
    ACCESS_PATTERN_WRITE_HEAVY,   // Mostly writes, few reads
    ACCESS_PATTERN_BALANCED,      // Equal reads and writes
    ACCESS_PATTERN_EXCLUSIVE,     // Single writer, multiple readers
    ACCESS_PATTERN_PRODUCER_CONSUMER, // Producer-consumer pattern
    ACCESS_PATTERN_ACCUMULATOR    // Accumulating values (sum, count, etc.)
} AccessPattern;

// Consistency levels
typedef enum {
    CONSISTENCY_EVENTUAL,     // Eventually consistent (fastest)
    CONSISTENCY_CAUSAL,       // Causally consistent
    CONSISTENCY_SEQUENTIAL,   // Sequentially consistent
    CONSISTENCY_LINEARIZABLE  // Linearizable (strongest, slowest)
} ConsistencyLevel;

// Custom synchronization functions for user-defined types
typedef struct {
    // Lock acquisition and release
    bool (*try_lock)(void* sync_state);
    void (*lock)(void* sync_state);
    void (*unlock)(void* sync_state);
    
    // Read operations
    void (*read_begin)(void* sync_state);
    void (*read_end)(void* sync_state);
    
    // Write operations
    void (*write_begin)(void* sync_state);
    void (*write_end)(void* sync_state);
    
    // Custom serialization for consistency
    void (*serialize)(const void* value, void* buffer, size_t* size);
    void (*deserialize)(void* value, const void* buffer, size_t size);
    
    // Cleanup
    void (*destroy)(void* sync_state);
} CustomSyncOps;

// Shared variable configuration
typedef struct {
    const char* name;
    SharedVarType type;
    SyncMode sync_mode;
    AccessPattern access_pattern;
    ConsistencyLevel consistency;
    
    // Performance tuning
    size_t initial_capacity;
    bool enable_statistics;
    bool enable_contention_detection;
    
    // Memory management
    bool use_memory_pool;
    size_t alignment_requirement;
    
    // Custom synchronization (for SHARED_TYPE_CUSTOM)
    CustomSyncOps* custom_ops;
    void* custom_sync_state;
    
    // Validation and constraints
    bool (*validate_value)(const void* value, size_t size);
    void* (*transform_on_write)(const void* old_value, const void* new_value, void* context);
    void* transform_context;
} SharedVarConfig;

// Performance statistics for shared variables
typedef struct {
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t total_cas_attempts;
    uint64_t successful_cas;
    uint64_t contention_events;
    
    // Timing statistics (in nanoseconds)
    uint64_t avg_read_time_ns;
    uint64_t avg_write_time_ns;
    uint64_t max_contention_time_ns;
    
    // Memory usage
    size_t current_memory_usage;
    size_t peak_memory_usage;
    
    // Synchronization effectiveness
    double lock_efficiency;
    uint64_t adaptive_mode_switches;
} SharedVarStats;

// Synchronization state for different modes
typedef union {
    struct {
        atomic_int_fast32_t value_i32;
        atomic_int_fast64_t value_i64;
        atomic_uint_fast32_t value_u32;
        atomic_uint_fast64_t value_u64;
        atomic_bool value_bool;
        GOO_ATOMIC_PTR value_ptr;
    } atomic;
    
    struct {
        pthread_mutex_t mutex;
        void* value;
    } mutex;
    
    struct {
        pthread_rwlock_t rwlock;
        void* value;
    } rwlock;
    
    struct {
        atomic_flag spinlock;
        void* value;
    } spinlock;
    
    struct {
        // Adaptive synchronization state
        SyncMode current_mode;
        uint64_t contention_count;
        uint64_t last_adaptation_time;
        union {
            atomic_int_fast64_t atomic_value;
            pthread_mutex_t mutex;
            pthread_rwlock_t rwlock;
        } sync;
        void* value;
    } adaptive;
    
    struct {
        // Custom synchronization state
        void* sync_state;
        void* value;
        CustomSyncOps* ops;
    } custom;
} SyncState;

// Shared variable structure
typedef struct SharedVariable {
    uint64_t id;
    char name[64];
    SharedVarConfig config;
    
    // Synchronization and value storage
    SyncState sync_state;
    size_t value_size;
    
    // Metadata
    uint64_t version;        // For optimistic concurrency
    uint64_t creation_time;
    uint64_t last_access_time;
    
    // Statistics
    SharedVarStats stats;
    
    // Reference counting
    atomic_int ref_count;
    
    // Manager reference
    SharedVarManager* manager;
    
    // Linked list for manager
    struct SharedVariable* next;
} SharedVariable;

// Shared variable manager
typedef struct SharedVarManager {
    // Configuration
    size_t max_variables;
    bool enable_global_statistics;
    bool enable_automatic_optimization;
    
    // Variable registry
    SharedVariable** variables;
    size_t variable_count;
    size_t variable_capacity;
    pthread_mutex_t registry_mutex;
    
    // Global synchronization groups
    SyncGroup** sync_groups;
    size_t sync_group_count;
    size_t sync_group_capacity;
    
    // Memory management
    void* memory_pool;
    size_t pool_size;
    size_t pool_used;
    pthread_mutex_t pool_mutex;
    
    // Optimization engine
    pthread_t optimizer_thread;
    bool optimization_enabled;
    uint64_t optimization_interval_ms;
    
    // Global statistics
    uint64_t total_operations;
    uint64_t total_contentions;
    uint64_t total_optimizations;
    
    // Future: Actor system integration
    void* actor_system;
} SharedVarManager;

// Synchronization group for coordinated access
typedef struct SyncGroup {
    uint64_t id;
    char name[64];
    
    // Variables in this group
    SharedVariable** variables;
    size_t variable_count;
    size_t variable_capacity;
    
    // Group-wide synchronization
    pthread_mutex_t group_mutex;
    ConsistencyLevel consistency_level;
    
    // Transaction support
    bool supports_transactions;
    uint64_t transaction_id_counter;
    
    // Group statistics
    uint64_t coordinated_operations;
    uint64_t transaction_commits;
    uint64_t transaction_aborts;
} SyncGroup;

// Transaction context for atomic multi-variable operations
typedef struct {
    uint64_t transaction_id;
    SyncGroup* group;
    
    // Variables involved in transaction
    SharedVariable** variables;
    void** old_values;
    void** new_values;
    size_t variable_count;
    
    // Transaction state
    bool is_active;
    bool is_committed;
    uint64_t start_time;
    uint64_t timeout_ms;
    
    // Rollback information
    uint64_t* original_versions;
    bool can_rollback;
} Transaction;

// Result types for shared variable operations (only declare new ones)
DECLARE_ERROR_UNION(int32_t)
DECLARE_ERROR_UNION(int64_t)
DECLARE_ERROR_UNION(uint32_t)
DECLARE_ERROR_UNION(uint64_t)
DECLARE_ERROR_UNION(float)
// Note: double and bool are already declared in ergonomic_errors.h

// Shared variable manager operations
SharedVarManager* shared_var_manager_create(size_t max_variables);
void shared_var_manager_destroy(SharedVarManager* manager);
Result_void_ptr shared_var_manager_start_optimization(SharedVarManager* manager);
Result_void_ptr shared_var_manager_stop_optimization(SharedVarManager* manager);

// Shared variable creation and management
SharedVariable* shared_var_create(SharedVarManager* manager, SharedVarConfig config);
void shared_var_destroy(SharedVariable* var);
SharedVariable* shared_var_find_by_name(SharedVarManager* manager, const char* name);
SharedVariable* shared_var_find_by_id(SharedVarManager* manager, uint64_t id);

// Typed read operations
Result_int32_t shared_var_get_int32(SharedVariable* var);
Result_int64_t shared_var_get_int64(SharedVariable* var);
Result_uint32_t shared_var_get_uint32(SharedVariable* var);
Result_uint64_t shared_var_get_uint64(SharedVariable* var);
Result_float shared_var_get_float(SharedVariable* var);
Result_double shared_var_get_double(SharedVariable* var);
Result_bool shared_var_get_bool(SharedVariable* var);
Result_void_ptr shared_var_get_ptr(SharedVariable* var);

// Typed write operations
Result_void_ptr shared_var_set_int32(SharedVariable* var, int32_t value);
Result_void_ptr shared_var_set_int64(SharedVariable* var, int64_t value);
Result_void_ptr shared_var_set_uint32(SharedVariable* var, uint32_t value);
Result_void_ptr shared_var_set_uint64(SharedVariable* var, uint64_t value);
Result_void_ptr shared_var_set_float(SharedVariable* var, float value);
Result_void_ptr shared_var_set_double(SharedVariable* var, double value);
Result_void_ptr shared_var_set_bool(SharedVariable* var, bool value);
Result_void_ptr shared_var_set_ptr(SharedVariable* var, void* value);

// String operations
Result_void_ptr shared_var_get_string(SharedVariable* var, char* buffer, size_t buffer_size);
Result_void_ptr shared_var_set_string(SharedVariable* var, const char* value);

// Atomic compare-and-swap operations
Result_bool shared_var_cas_int32(SharedVariable* var, int32_t expected, int32_t desired);
Result_bool shared_var_cas_int64(SharedVariable* var, int64_t expected, int64_t desired);
Result_bool shared_var_cas_uint32(SharedVariable* var, uint32_t expected, uint32_t desired);
Result_bool shared_var_cas_uint64(SharedVariable* var, uint64_t expected, uint64_t desired);
Result_bool shared_var_cas_bool(SharedVariable* var, bool expected, bool desired);
Result_bool shared_var_cas_ptr(SharedVariable* var, void* expected, void* desired);

// Atomic arithmetic operations
Result_int32_t shared_var_fetch_add_int32(SharedVariable* var, int32_t value);
Result_int64_t shared_var_fetch_add_int64(SharedVariable* var, int64_t value);
Result_uint32_t shared_var_fetch_add_uint32(SharedVariable* var, uint32_t value);
Result_uint64_t shared_var_fetch_add_uint64(SharedVariable* var, uint64_t value);

// Atomic bitwise operations
Result_int32_t shared_var_fetch_and_int32(SharedVariable* var, int32_t value);
Result_int32_t shared_var_fetch_or_int32(SharedVariable* var, int32_t value);
Result_int32_t shared_var_fetch_xor_int32(SharedVariable* var, int32_t value);

// Custom value operations
Result_void_ptr shared_var_get_custom(SharedVariable* var, void* buffer, size_t buffer_size);
Result_void_ptr shared_var_set_custom(SharedVariable* var, const void* value, size_t value_size);
Result_bool shared_var_cas_custom(SharedVariable* var, const void* expected, const void* desired, size_t size);

// Batch operations for efficiency
typedef struct {
    SharedVariable* var;
    enum {
        BATCH_OP_READ,
        BATCH_OP_WRITE,
        BATCH_OP_CAS
    } operation;
    
    union {
        struct { void* buffer; size_t size; } read;
        struct { const void* value; size_t size; } write;
        struct { const void* expected; const void* desired; size_t size; } cas;
    } params;
    
    // Results
    bool success;
    Error* error;
} BatchOperation;

Result_void_ptr shared_var_batch_execute(BatchOperation* operations, size_t count);

// Synchronization groups
SyncGroup* sync_group_create(SharedVarManager* manager, const char* name, ConsistencyLevel consistency);
void sync_group_destroy(SyncGroup* group);
Result_void_ptr sync_group_add_variable(SyncGroup* group, SharedVariable* var);
Result_void_ptr sync_group_remove_variable(SyncGroup* group, SharedVariable* var);

// Transactions for atomic multi-variable operations
Transaction* transaction_begin(SyncGroup* group, uint64_t timeout_ms);
Result_void_ptr transaction_read(Transaction* tx, SharedVariable* var, void* buffer, size_t size);
Result_void_ptr transaction_write(Transaction* tx, SharedVariable* var, const void* value, size_t size);
Result_void_ptr transaction_commit(Transaction* tx);
Result_void_ptr transaction_rollback(Transaction* tx);
void transaction_destroy(Transaction* tx);

// Performance and monitoring
SharedVarStats shared_var_get_stats(SharedVariable* var);
void shared_var_reset_stats(SharedVariable* var);

typedef struct {
    uint64_t total_variables;
    uint64_t active_variables;
    uint64_t total_operations;
    uint64_t total_contentions;
    
    // Performance metrics
    double avg_operation_time_ns;
    double contention_rate;
    size_t memory_usage_bytes;
    
    // Synchronization mode distribution
    uint64_t atomic_operations;
    uint64_t mutex_operations;
    uint64_t rwlock_operations;
    uint64_t adaptive_operations;
    
    // Optimization statistics
    uint64_t optimization_events;
    uint64_t mode_switches;
    double optimization_effectiveness;
} GlobalSharedVarStats;

GlobalSharedVarStats shared_var_manager_get_stats(SharedVarManager* manager);
void shared_var_manager_reset_stats(SharedVarManager* manager);

// Configuration helpers
SharedVarConfig shared_var_config_default(const char* name, SharedVarType type);
SharedVarConfig shared_var_config_atomic(const char* name, SharedVarType type);
SharedVarConfig shared_var_config_read_heavy(const char* name, SharedVarType type);
SharedVarConfig shared_var_config_write_heavy(const char* name, SharedVarType type);
SharedVarConfig shared_var_config_high_contention(const char* name, SharedVarType type);

// Convenience macros for common patterns
#define SHARED_VAR_DECLARE(name, type) \
    static SharedVariable* name = NULL; \
    static void name##_init(SharedVarManager* manager) { \
        if (!name) { \
            SharedVarConfig config = shared_var_config_default(#name, type); \
            name = shared_var_create(manager, config); \
        } \
    }

#define SHARED_VAR_GET(name, result_type) \
    shared_var_get_##result_type(name)

#define SHARED_VAR_SET(name, value, value_type) \
    shared_var_set_##value_type(name, value)

#define SHARED_VAR_CAS(name, expected, desired, value_type) \
    shared_var_cas_##value_type(name, expected, desired)

// Integration with actor system (future implementation)
Result_void_ptr shared_var_integrate_with_actors(SharedVarManager* manager, void* actor_system);
void shared_var_notify_actors_on_change(SharedVariable* var, const void* old_value, const void* new_value);

// Software Transactional Memory (STM)
typedef struct STMTransaction STMTransaction;

// STM Transaction operations
STMTransaction* stm_begin_transaction(void);
Result_void_ptr stm_read(STMTransaction* tx, SharedVariable* var, void* buffer, size_t buffer_size);
Result_void_ptr stm_write(STMTransaction* tx, SharedVariable* var, const void* value, size_t value_size);
Result_void_ptr stm_commit(STMTransaction* tx);
void stm_abort(STMTransaction* tx);
void stm_destroy_transaction(STMTransaction* tx);

// Read/write optimized operations
Result_int32_t shared_var_read_optimized_int32(SharedVariable* var);
Result_int64_t shared_var_read_optimized_int64(SharedVariable* var);

// Bulk operations for performance
typedef struct {
    SharedVariable* var;
    void* buffer;
    size_t buffer_size;
    bool success;
    Error* error;
} BulkReadOperation;

typedef struct {
    enum {
        RW_OP_READ,
        RW_OP_WRITE
    } operation_type;
    SharedVariable* var;
    union {
        struct {
            void* buffer;
            size_t buffer_size;
        } read;
        struct {
            const void* value;
            size_t value_size;
        } write;
    } params;
    bool success;
    Error* error;
} ReadWriteOperation;

Result_void_ptr shared_var_bulk_read(BulkReadOperation* operations, size_t count);
Result_void_ptr shared_var_batch_read_write(ReadWriteOperation* operations, size_t count);

// Advanced synchronization patterns
typedef struct {
    SharedVariable* condition_var;
    SharedVariable* mutex_var;
    
    // Wait queue for blocked threads
    pthread_t* waiting_threads;
    size_t waiting_count;
    size_t waiting_capacity;
    
    pthread_mutex_t wait_queue_mutex;
} ThreadConditionVariable;

ThreadConditionVariable* thread_condition_create(SharedVarManager* manager, const char* name);
void thread_condition_destroy(ThreadConditionVariable* cond);
Result_void_ptr thread_condition_wait(ThreadConditionVariable* cond, pthread_t thread);
Result_void_ptr thread_condition_signal(ThreadConditionVariable* cond);
Result_void_ptr thread_condition_broadcast(ThreadConditionVariable* cond);

// Wait-free data structures
typedef struct {
    SharedVariable* head;
    SharedVariable* tail;
    size_t capacity;
    GOO_ATOMIC_SIZE_T size;
} WaitFreeQueue;

WaitFreeQueue* wait_free_queue_create(SharedVarManager* manager, const char* name, size_t capacity);
void wait_free_queue_destroy(WaitFreeQueue* queue);
Result_bool wait_free_queue_enqueue(WaitFreeQueue* queue, const void* item, size_t item_size);
Result_void_ptr wait_free_queue_dequeue(WaitFreeQueue* queue, void* buffer, size_t buffer_size);
bool wait_free_queue_is_empty(WaitFreeQueue* queue);
bool wait_free_queue_is_full(WaitFreeQueue* queue);

#endif // GOO_SHARED_VARIABLES_H