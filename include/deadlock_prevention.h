#ifndef GOO_DEADLOCK_PREVENTION_H
#define GOO_DEADLOCK_PREVENTION_H

#include "runtime.h"
#include "structured_concurrency.h"
#include "advanced_channels.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

// =============================================================================
// Deadlock Prevention and Performance Optimization System
// =============================================================================

// Forward declarations
typedef struct LockNode LockNode;
typedef struct LockGraph LockGraph;
typedef struct DeadlockDetector DeadlockDetector;
typedef struct WorkStealingScheduler WorkStealingScheduler;
typedef struct ThreadPool ThreadPool;
typedef struct NUMAManager NUMAManager;
typedef struct PerformanceMonitor PerformanceMonitor;

// =============================================================================
// Lock Ordering and Deadlock Detection
// =============================================================================

// Resource types for lock ordering
typedef enum {
    RESOURCE_TYPE_MUTEX,
    RESOURCE_TYPE_RWLOCK,
    RESOURCE_TYPE_SPINLOCK,
    RESOURCE_TYPE_CHANNEL,
    RESOURCE_TYPE_ACTOR,
    RESOURCE_TYPE_SHARED_VAR,
    RESOURCE_TYPE_MEMORY_REGION,
    RESOURCE_TYPE_FILE,
    RESOURCE_TYPE_NETWORK,
    RESOURCE_TYPE_CUSTOM
} ResourceType;

// Lock acquisition order policies
typedef enum {
    LOCK_ORDER_RESOURCE_ID,     // Order by resource ID
    LOCK_ORDER_ADDRESS,         // Order by memory address
    LOCK_ORDER_HIERARCHY,       // Hierarchical ordering
    LOCK_ORDER_TIMESTAMP,       // Order by creation timestamp
    LOCK_ORDER_PRIORITY,        // Order by priority
    LOCK_ORDER_CUSTOM          // Custom ordering function
} LockOrderPolicy;

// Lock acquisition mode
typedef enum {
    LOCK_MODE_SHARED,           // Read lock
    LOCK_MODE_EXCLUSIVE,        // Write lock
    LOCK_MODE_UPGRADE,          // Upgrade from shared to exclusive
    LOCK_MODE_TRY,             // Non-blocking try lock
    LOCK_MODE_TIMED            // Timed lock with timeout
} LockMode;

// Resource descriptor for lock ordering
typedef struct ResourceDescriptor {
    uint64_t resource_id;       // Unique resource identifier
    ResourceType type;          // Type of resource
    void* resource_ptr;         // Pointer to actual resource
    const char* name;           // Human-readable name
    uint32_t hierarchy_level;   // Hierarchy level for ordering
    uint64_t creation_time;     // Creation timestamp
    uint32_t priority;          // Priority for priority-based ordering
    
    // Custom ordering
    int (*compare_func)(const struct ResourceDescriptor* a, 
                       const struct ResourceDescriptor* b);
    
    // Lock statistics
    atomic_uint_least64_t acquisition_count;
    atomic_uint_least64_t contention_count;
    double avg_hold_time_ns;
    
} ResourceDescriptor;

// Lock acquisition request
typedef struct LockRequest {
    ResourceDescriptor* resource;
    LockMode mode;
    uint64_t timeout_ns;        // Timeout in nanoseconds (0 = no timeout)
    uint64_t thread_id;         // Requesting thread ID
    uint64_t timestamp;         // Request timestamp
    
    // Deadlock prevention
    uint64_t* dependency_chain; // Chain of resources already held
    size_t dependency_count;    // Number of dependencies
    
    // Callback for lock acquisition
    void (*acquired_callback)(struct LockRequest* request, bool success);
    void* callback_context;
    
} LockRequest;

// Lock node in the resource allocation graph
typedef struct LockNode {
    ResourceDescriptor* resource;
    LockNode** dependencies;    // Resources this depends on
    size_t dependency_count;
    LockNode** dependents;      // Resources that depend on this
    size_t dependent_count;
    
    // Thread ownership
    uint64_t owner_thread_id;
    LockMode current_mode;
    atomic_int ref_count;
    
    // Timing information
    uint64_t acquired_time;
    uint64_t last_access_time;
    
} LockNode;

// Resource allocation graph for deadlock detection
typedef struct LockGraph {
    LockNode** nodes;           // Array of lock nodes
    size_t node_count;
    size_t node_capacity;
    
    // Graph operations
    pthread_rwlock_t graph_lock; // Protects graph structure
    
    // Cycle detection cache
    bool* visited;              // For cycle detection algorithm
    bool* recursion_stack;      // For DFS cycle detection
    size_t cache_size;
    
    // Statistics
    uint64_t cycle_detection_count;
    uint64_t deadlocks_prevented;
    double avg_detection_time_ns;
    
} LockGraph;

// Deadlock detector
typedef struct DeadlockDetector {
    LockGraph* resource_graph;  // Resource allocation graph
    LockOrderPolicy order_policy; // Lock ordering policy
    
    // Detection configuration
    bool enable_static_analysis; // Enable compile-time detection
    bool enable_runtime_detection; // Enable runtime detection
    uint64_t detection_interval_ms; // Detection interval
    uint64_t timeout_threshold_ms; // Deadlock timeout threshold
    
    // Detection thread
    pthread_t detector_thread;
    atomic_bool detector_active;
    
    // Deadlock recovery
    struct {
        bool enable_timeout_based; // Timeout-based recovery
        bool enable_preemption;    // Preemptive lock release
        bool enable_restart;       // Transaction restart
        uint64_t recovery_timeout_ms; // Recovery timeout
    } recovery;
    
    // Statistics
    struct {
        atomic_uint_least64_t deadlocks_detected;
        atomic_uint_least64_t deadlocks_prevented;
        atomic_uint_least64_t false_positives;
        atomic_uint_least64_t recovery_attempts;
        atomic_uint_least64_t successful_recoveries;
    } stats;
    
} DeadlockDetector;

// =============================================================================
// Work-Stealing Scheduler
// =============================================================================

// Task representation for work-stealing
typedef struct WorkTask {
    uint64_t task_id;           // Unique task identifier
    void (*function)(void* context); // Task function
    void* context;              // Task context
    
    // Task properties
    uint32_t priority;          // Task priority
    bool is_parallel;           // Can be parallelized
    size_t estimated_work;      // Estimated work units
    
    // Dependencies
    struct WorkTask** dependencies; // Task dependencies
    size_t dependency_count;
    atomic_int pending_deps;    // Remaining dependencies
    
    // Completion
    atomic_bool completed;
    void (*completion_callback)(struct WorkTask* task, bool success);
    void* completion_context;
    
    // Statistics
    uint64_t created_time;
    uint64_t started_time;
    uint64_t completed_time;
    uint32_t steal_count;       // Number of times stolen
    
} WorkTask;

// Work-stealing deque for each worker
typedef struct WorkDeque {
    WorkTask** tasks;           // Task array (circular buffer)
    size_t capacity;
    atomic_size_t top;          // Top index (owner access)
    atomic_size_t bottom;       // Bottom index (owner access)
    
    // Synchronization
    pthread_mutex_t steal_mutex; // Protects stealing operations
    
    // Statistics
    atomic_uint_least64_t tasks_added;
    atomic_uint_least64_t tasks_stolen;
    atomic_uint_least64_t steal_attempts;
    
} WorkDeque;

// Worker thread in work-stealing scheduler
typedef struct WorkerThread {
    uint64_t worker_id;         // Unique worker ID
    pthread_t thread;           // Worker thread
    
    // Work deque
    WorkDeque* deque;           // Local work deque
    
    // Worker state
    atomic_bool active;         // Worker is active
    atomic_bool sleeping;       // Worker is sleeping
    uint32_t cpu_affinity;      // CPU affinity mask
    uint32_t numa_node;         // NUMA node assignment
    
    // Random state for work stealing
    uint64_t random_state;      // Random state for stealing
    
    // Statistics
    struct {
        atomic_uint_least64_t tasks_executed;
        atomic_uint_least64_t tasks_stolen;
        atomic_uint_least64_t steal_attempts;
        atomic_uint_least64_t failed_steals;
        double cpu_utilization;
        uint64_t idle_time_ns;
    } stats;
    
} WorkerThread;

// Work-stealing scheduler
typedef struct WorkStealingScheduler {
    WorkerThread* workers;      // Array of worker threads
    size_t worker_count;        // Number of workers
    
    // Global task queue for overflow
    WorkTask** global_queue;
    size_t global_capacity;
    atomic_size_t global_head;
    atomic_size_t global_tail;
    pthread_mutex_t global_mutex;
    pthread_cond_t global_not_empty;
    
    // Scheduler configuration
    bool enable_work_stealing;  // Enable work stealing
    bool enable_load_balancing; // Enable load balancing
    uint32_t steal_batch_size;  // Number of tasks to steal
    uint64_t idle_sleep_ns;     // Sleep time when idle
    
    // Load balancing
    atomic_uint_least32_t* worker_loads; // Load per worker
    uint64_t load_balance_interval_ms; // Load balancing interval
    pthread_t load_balancer_thread;
    
    // Scheduler state
    atomic_bool scheduler_active;
    atomic_bool shutdown_requested;
    
    // Performance monitoring
    PerformanceMonitor* monitor;
    
} WorkStealingScheduler;

// =============================================================================
// NUMA-Aware Memory and Thread Management
// =============================================================================

// NUMA node information
typedef struct NUMANode {
    uint32_t node_id;           // NUMA node ID
    size_t memory_size;         // Total memory on this node
    size_t free_memory;         // Free memory on this node
    uint32_t cpu_count;         // Number of CPUs on this node
    uint32_t* cpu_list;         // List of CPU IDs
    
    // Memory pools
    Arena* memory_arena;        // Memory arena for this node
    void** free_blocks;         // Free memory blocks
    size_t free_block_count;
    
    // Thread affinity
    WorkerThread** threads;     // Threads assigned to this node
    size_t thread_count;
    
    // Statistics
    double memory_utilization;
    double cpu_utilization;
    uint64_t allocation_count;
    uint64_t deallocation_count;
    
} NUMANode;

// NUMA-aware manager
typedef struct NUMAManager {
    NUMANode* nodes;            // Array of NUMA nodes
    size_t node_count;          // Number of NUMA nodes
    bool numa_available;        // NUMA support available
    
    // Memory allocation policy
    enum {
        NUMA_POLICY_LOCAL,      // Allocate on local node
        NUMA_POLICY_INTERLEAVED, // Interleave across nodes
        NUMA_POLICY_PREFERRED,  // Prefer specific node
        NUMA_POLICY_BIND        // Bind to specific node
    } allocation_policy;
    
    uint32_t preferred_node;    // Preferred NUMA node
    
    // Thread placement policy
    enum {
        THREAD_POLICY_ROUND_ROBIN, // Round-robin placement
        THREAD_POLICY_LOAD_BASED,  // Load-based placement
        THREAD_POLICY_AFFINITY,    // CPU affinity based
        THREAD_POLICY_MANUAL       // Manual placement
    } thread_policy;
    
    // Statistics
    struct {
        uint64_t local_allocations;
        uint64_t remote_allocations;
        uint64_t thread_migrations;
        double avg_memory_latency_ns;
    } stats;
    
} NUMAManager;

// =============================================================================
// Performance Monitoring and Optimization
// =============================================================================

// Performance metrics
typedef struct PerformanceMetrics {
    // CPU metrics
    double cpu_utilization;     // Overall CPU utilization
    double* per_core_utilization; // Per-core utilization
    uint32_t active_threads;    // Number of active threads
    
    // Memory metrics
    size_t memory_usage;        // Current memory usage
    size_t peak_memory_usage;   // Peak memory usage
    double memory_fragmentation; // Memory fragmentation ratio
    uint64_t allocation_rate;   // Allocations per second
    
    // Concurrency metrics
    uint32_t active_locks;      // Number of active locks
    double lock_contention_ratio; // Lock contention ratio
    uint64_t context_switches;  // Number of context switches
    double avg_task_queue_depth; // Average task queue depth
    
    // Channel metrics
    uint64_t messages_per_second; // Message throughput
    double avg_message_latency_ns; // Average message latency
    uint32_t blocked_channels;  // Number of blocked channels
    
    // System metrics
    double load_average;        // System load average
    uint64_t page_faults;       // Page fault count
    uint64_t cache_misses;      // Cache miss count
    double network_utilization; // Network utilization
    
} PerformanceMetrics;

// Performance monitor
typedef struct PerformanceMonitor {
    PerformanceMetrics current;   // Current metrics
    PerformanceMetrics* history;  // Historical metrics
    size_t history_size;
    size_t history_capacity;
    
    // Monitoring configuration
    uint64_t sample_interval_ms;  // Sampling interval
    bool enable_detailed_metrics; // Detailed metrics collection
    bool enable_profiling;        // Performance profiling
    
    // Monitoring thread
    pthread_t monitor_thread;
    atomic_bool monitor_active;
    
    // Optimization triggers
    struct {
        double cpu_threshold;     // CPU utilization threshold
        double memory_threshold;  // Memory usage threshold
        double contention_threshold; // Lock contention threshold
        void (*optimization_callback)(struct PerformanceMonitor* monitor);
    } triggers;
    
    // Adaptive optimization
    struct {
        bool enable_auto_scaling;  // Auto-scale thread pool
        bool enable_load_balancing; // Auto load balancing
        bool enable_memory_tuning;  // Auto memory tuning
        uint64_t adaptation_interval_ms; // Adaptation interval
    } adaptive;
    
} PerformanceMonitor;

// =============================================================================
// Lock-Free Data Structures
// =============================================================================

// Lock-free stack
typedef struct LockFreeStack {
    struct StackNode {
        void* data;
        struct StackNode* next;
    } * volatile head;
    
    atomic_uint_least64_t push_count;
    atomic_uint_least64_t pop_count;
    
} LockFreeStack;

// Lock-free queue
typedef struct LockFreeQueue {
    struct QueueNode {
        void* data;
        atomic_intptr_t next;
    } * volatile head;
    atomic_intptr_t tail;
    
    atomic_uint_least64_t enqueue_count;
    atomic_uint_least64_t dequeue_count;
    
} LockFreeQueue;

// Lock-free hash table
typedef struct LockFreeHashTable {
    struct HashBucket {
        atomic_intptr_t head;   // Head of bucket chain
    } * buckets;
    size_t bucket_count;
    
    uint32_t (*hash_function)(const void* key, size_t key_size);
    bool (*key_compare)(const void* key1, const void* key2, size_t key_size);
    
    atomic_uint_least64_t insert_count;
    atomic_uint_least64_t lookup_count;
    atomic_uint_least64_t delete_count;
    
} LockFreeHashTable;

// =============================================================================
// Core API Functions
// =============================================================================

// Deadlock detection and prevention
DeadlockDetector* deadlock_detector_create(LockOrderPolicy policy);
void deadlock_detector_destroy(DeadlockDetector* detector);
bool deadlock_detector_start(DeadlockDetector* detector);
void deadlock_detector_stop(DeadlockDetector* detector);

// Resource management
ResourceDescriptor* resource_descriptor_create(uint64_t id, ResourceType type, 
                                              void* resource, const char* name);
void resource_descriptor_destroy(ResourceDescriptor* desc);
bool resource_register(DeadlockDetector* detector, ResourceDescriptor* resource);
bool resource_unregister(DeadlockDetector* detector, uint64_t resource_id);

// Lock acquisition with deadlock prevention
bool acquire_lock_ordered(DeadlockDetector* detector, LockRequest* request);
bool release_lock_ordered(DeadlockDetector* detector, uint64_t resource_id, uint64_t thread_id);
bool try_acquire_multiple_locks(DeadlockDetector* detector, LockRequest* requests, size_t count);

// Work-stealing scheduler
WorkStealingScheduler* work_stealing_scheduler_create(size_t worker_count);
void work_stealing_scheduler_destroy(WorkStealingScheduler* scheduler);
bool work_stealing_scheduler_start(WorkStealingScheduler* scheduler);
void work_stealing_scheduler_stop(WorkStealingScheduler* scheduler);

// Task management
WorkTask* work_task_create(void (*function)(void*), void* context, uint32_t priority);
void work_task_destroy(WorkTask* task);
bool work_task_submit(WorkStealingScheduler* scheduler, WorkTask* task);
bool work_task_wait(WorkTask* task, uint64_t timeout_ms);

// NUMA management
NUMAManager* numa_manager_create(void);
void numa_manager_destroy(NUMAManager* manager);
void* numa_alloc(NUMAManager* manager, size_t size, uint32_t preferred_node);
void numa_free(NUMAManager* manager, void* ptr, size_t size);
bool numa_bind_thread(NUMAManager* manager, pthread_t thread, uint32_t node);

// Performance monitoring
PerformanceMonitor* performance_monitor_create(uint64_t sample_interval_ms);
void performance_monitor_destroy(PerformanceMonitor* monitor);
bool performance_monitor_start(PerformanceMonitor* monitor);
void performance_monitor_stop(PerformanceMonitor* monitor);
PerformanceMetrics performance_monitor_get_current(PerformanceMonitor* monitor);

// Lock-free data structures
LockFreeStack* lockfree_stack_create(void);
void lockfree_stack_destroy(LockFreeStack* stack);
bool lockfree_stack_push(LockFreeStack* stack, void* data);
void* lockfree_stack_pop(LockFreeStack* stack);

LockFreeQueue* lockfree_queue_create(void);
void lockfree_queue_destroy(LockFreeQueue* queue);
bool lockfree_queue_enqueue(LockFreeQueue* queue, void* data);
void* lockfree_queue_dequeue(LockFreeQueue* queue);

LockFreeHashTable* lockfree_hashtable_create(size_t bucket_count,
                                             uint32_t (*hash_func)(const void*, size_t),
                                             bool (*key_cmp)(const void*, const void*, size_t));
void lockfree_hashtable_destroy(LockFreeHashTable* table);
bool lockfree_hashtable_insert(LockFreeHashTable* table, const void* key, size_t key_size, void* value);
void* lockfree_hashtable_lookup(LockFreeHashTable* table, const void* key, size_t key_size);
bool lockfree_hashtable_delete(LockFreeHashTable* table, const void* key, size_t key_size);

// =============================================================================
// Integration with Existing Systems
// =============================================================================

// Integration with structured concurrency
bool deadlock_detector_register_scope(DeadlockDetector* detector, ConcurrencyScope* scope);
bool deadlock_detector_unregister_scope(DeadlockDetector* detector, ConcurrencyScope* scope);

// Integration with advanced channels
bool deadlock_detector_register_channel(DeadlockDetector* detector, AdvancedChannel* channel);
bool deadlock_detector_unregister_channel(DeadlockDetector* detector, AdvancedChannel* channel);

// Integration with actor system
bool deadlock_detector_register_actor(DeadlockDetector* detector, ActorRef* actor);
bool deadlock_detector_unregister_actor(DeadlockDetector* detector, ActorRef* actor);

// =============================================================================
// Utilities and Debugging
// =============================================================================

// Debugging and diagnostics
void deadlock_detector_dump_graph(DeadlockDetector* detector);
void work_stealing_scheduler_dump_stats(WorkStealingScheduler* scheduler);
void numa_manager_dump_topology(NUMAManager* manager);
void performance_monitor_dump_metrics(PerformanceMonitor* monitor);

// Configuration
typedef struct DeadlockPreventionConfig {
    LockOrderPolicy lock_order_policy;
    bool enable_static_analysis;
    bool enable_runtime_detection;
    uint64_t detection_interval_ms;
    uint64_t timeout_threshold_ms;
    bool enable_work_stealing;
    size_t worker_thread_count;
    bool enable_numa_awareness;
    bool enable_performance_monitoring;
    uint64_t monitor_sample_interval_ms;
} DeadlockPreventionConfig;

// Global initialization
bool deadlock_prevention_init(const DeadlockPreventionConfig* config);
void deadlock_prevention_shutdown(void);
DeadlockDetector* get_global_deadlock_detector(void);
WorkStealingScheduler* get_global_scheduler(void);
NUMAManager* get_global_numa_manager(void);
PerformanceMonitor* get_global_performance_monitor(void);

#endif // GOO_DEADLOCK_PREVENTION_H