#ifndef GOO_DEADLOCK_PREVENTION_H
#define GOO_DEADLOCK_PREVENTION_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "ccomp_shim.h"
#include "ergonomic_errors.h"
#include "fearless_concurrency.h"
#include "shared_variables.h"
#include "structured_concurrency.h"
#include "advanced_channels.h"

// Forward declarations
typedef struct ResourceManager ResourceManager;
typedef struct DeadlockDetector DeadlockDetector;
typedef struct ResourceGraph ResourceGraph;
typedef struct LockOrderingSystem LockOrderingSystem;
typedef struct TimeoutManager TimeoutManager;

// Resource types for deadlock prevention
typedef enum {
    RESOURCE_TYPE_MUTEX,
    RESOURCE_TYPE_RWLOCK,
    RESOURCE_TYPE_CHANNEL,
    RESOURCE_TYPE_SHARED_VAR,
    RESOURCE_TYPE_ACTOR,
    RESOURCE_TYPE_TASK,
    RESOURCE_TYPE_MEMORY,
    RESOURCE_TYPE_FILE,
    RESOURCE_TYPE_NETWORK,
    RESOURCE_TYPE_CUSTOM
} ResourceType;

// Lock acquisition strategies
typedef enum {
    LOCK_STRATEGY_ORDERED,      // Always acquire locks in a predetermined order
    LOCK_STRATEGY_TIMEOUT,      // Use timeouts to avoid indefinite blocking
    LOCK_STRATEGY_BANKER,       // Banker's algorithm for resource allocation
    LOCK_STRATEGY_WOUND_WAIT,   // Wound-wait algorithm
    LOCK_STRATEGY_WAIT_DIE,     // Wait-die algorithm
    LOCK_STRATEGY_HIERARCHICAL  // Hierarchical lock ordering
} LockStrategy;

// Deadlock detection methods
typedef enum {
    DETECTION_NONE,           // No deadlock detection
    DETECTION_TIMEOUT,        // Simple timeout-based detection
    DETECTION_WAIT_FOR_GRAPH, // Wait-for graph analysis
    DETECTION_RESOURCE_GRAPH, // Resource allocation graph
    DETECTION_CYCLE_DETECTION,// Cycle detection in dependency graph
    DETECTION_BANKER_ALGORITHM // Banker's algorithm simulation
} DeadlockDetectionMethod;

// Resource access modes
typedef enum {
    ACCESS_MODE_READ,
    ACCESS_MODE_WRITE,
    ACCESS_MODE_EXCLUSIVE,
    ACCESS_MODE_SHARED
} ResourceAccessMode;

// Resource request information
typedef struct ResourceRequest {
    uint64_t request_id;
    uint64_t requesting_entity_id;  // Actor, task, or thread ID
    ResourceType resource_type;
    uint64_t resource_id;
    ResourceAccessMode access_mode;
    
    // Request metadata
    uint64_t request_time;
    uint64_t timeout_ms;
    int priority;
    
    // Dependency information
    struct ResourceRequest** dependencies;
    size_t dependency_count;
    
    // State tracking
    bool is_granted;
    bool is_waiting;
    bool is_timed_out;
    uint64_t grant_time;
    
    struct ResourceRequest* next;
} ResourceRequest;

// Resource descriptor
typedef struct Resource {
    uint64_t id;
    char name[64];
    ResourceType type;
    
    // Resource state
    bool is_allocated;
    bool is_exclusive;
    uint64_t current_holder_id;
    
    // Multiple holders for shared resources
    uint64_t* holders;
    size_t holder_count;
    size_t holder_capacity;
    
    // Request queue
    ResourceRequest* pending_requests;
    ResourceRequest* granted_requests;
    size_t pending_count;
    size_t granted_count;
    
    // Resource metadata
    uint64_t allocation_time;
    uint64_t last_access_time;
    size_t total_acquisitions;
    
    // Synchronization
    pthread_mutex_t resource_mutex;
    pthread_cond_t resource_available;
    
    // Custom resource data
    void* resource_data;
    void (*resource_destructor)(void* data);
    
    struct Resource* next;
} Resource;

// Wait-for graph node
typedef struct WaitForNode {
    uint64_t entity_id;        // Actor, task, or thread ID
    ResourceType entity_type;
    
    // What this entity is waiting for
    uint64_t* waiting_for;
    size_t waiting_for_count;
    size_t waiting_for_capacity;
    
    // What entities are waiting for this one
    uint64_t* waited_by;
    size_t waited_by_count;
    size_t waited_by_capacity;
    
    // Node metadata
    uint64_t last_update_time;
    bool is_active;
    
    struct WaitForNode* next;
} WaitForNode;

// Resource allocation graph
typedef struct ResourceGraph {
    Resource** resources;
    size_t resource_count;
    size_t resource_capacity;
    
    WaitForNode** entities;
    size_t entity_count;
    size_t entity_capacity;
    
    // Graph analysis
    bool has_cycles;
    uint64_t* cycle_entities;
    size_t cycle_length;
    
    // Graph synchronization
    pthread_rwlock_t graph_lock;
    
    // Statistics
    uint64_t total_allocations;
    uint64_t deadlock_detections;
    uint64_t false_positives;
} ResourceGraph;

// Lock ordering system for preventing deadlocks
typedef struct LockOrderingSystem {
    // Global lock hierarchy
    struct {
        ResourceType type;
        uint64_t resource_id;
        int order_value;
    } *lock_hierarchy;
    size_t hierarchy_size;
    size_t hierarchy_capacity;
    
    // Current thread lock state
    struct {
        pthread_t thread_id;
        int* held_locks;
        size_t held_count;
        size_t held_capacity;
        int highest_order;
    } *thread_states;
    size_t thread_count;
    size_t thread_capacity;
    
    pthread_mutex_t ordering_mutex;
    
    // Violation tracking
    uint64_t ordering_violations;
    uint64_t deadlocks_prevented;
} LockOrderingSystem;

// Deadlock detector configuration
typedef struct DeadlockDetectorConfig {
    DeadlockDetectionMethod method;
    uint64_t detection_interval_ms;
    uint64_t max_wait_time_ms;
    
    // Graph analysis settings
    bool enable_cycle_detection;
    bool enable_phantom_deadlock_filtering;
    size_t max_graph_depth;
    
    // Performance settings
    bool enable_preemptive_detection;
    bool enable_resource_prediction;
    size_t max_detection_threads;
    
    // Integration
    ActorSystem* actor_system;
    TaskScope* task_scope;
    SharedVarManager* shared_var_manager;
    ChannelBroker* channel_broker;
} DeadlockDetectorConfig;

// Main deadlock detector
typedef struct DeadlockDetector {
    DeadlockDetectorConfig config;
    ResourceGraph* resource_graph;
    LockOrderingSystem* lock_ordering;
    
    // Detection threads
    pthread_t* detector_threads;
    size_t detector_thread_count;
    
    // Detection state
    bool is_active;
    atomic_bool shutdown_requested;
    
    // Detection results
    struct {
        uint64_t* involved_entities;
        uint64_t* involved_resources;
        size_t entity_count;
        size_t resource_count;
        uint64_t detection_time;
        DeadlockDetectionMethod detection_method;
        char description[256];
    } last_deadlock;
    
    // Statistics
    uint64_t total_detections;
    uint64_t successful_preventions;
    uint64_t false_alarms;
    uint64_t avg_detection_time_ns;
    
    // Callbacks
    void (*on_deadlock_detected)(struct DeadlockDetector* detector, void* context);
    void (*on_deadlock_resolved)(struct DeadlockDetector* detector, void* context);
    void* callback_context;
    
    // Resource manager integration
    ResourceManager* resource_manager;
    
    pthread_mutex_t detector_mutex;
} DeadlockDetector;

// Resource manager for coordinated resource allocation
typedef struct ResourceManager {
    // Resource registry
    Resource** resources;
    size_t resource_count;
    size_t resource_capacity;
    pthread_mutex_t registry_mutex;
    
    // Resource pools by type
    struct {
        ResourceType type;
        Resource** pool;
        size_t pool_size;
        size_t pool_capacity;
    } *resource_pools;
    size_t pool_count;
    
    // Global allocation policy
    LockStrategy default_strategy;
    uint64_t default_timeout_ms;
    int max_concurrent_requests;
    
    // Request tracking
    ResourceRequest** pending_requests;
    size_t pending_request_count;
    size_t pending_request_capacity;
    
    // Deadlock prevention
    DeadlockDetector* deadlock_detector;
    
    // Performance optimization
    bool enable_request_batching;
    bool enable_resource_prediction;
    bool enable_load_balancing;
    
    // Statistics
    uint64_t total_requests;
    uint64_t successful_allocations;
    uint64_t failed_allocations;
    uint64_t timeouts;
    uint64_t deadlocks_prevented;
    
    // Integration with other systems
    ActorSystem* actor_system;
    TaskScope* task_scope;
    SharedVarManager* shared_var_manager;
    ChannelBroker* channel_broker;
} ResourceManager;

// Timeout management for preventing indefinite waits
typedef struct TimeoutManager {
    // Timeout tracking
    struct {
        uint64_t request_id;
        uint64_t timeout_time;
        ResourceRequest* request;
        void (*timeout_callback)(ResourceRequest* request, void* context);
        void* callback_context;
    } *timeouts;
    size_t timeout_count;
    size_t timeout_capacity;
    
    // Timeout processing
    pthread_t timeout_thread;
    bool is_active;
    atomic_bool shutdown_requested;
    
    pthread_mutex_t timeout_mutex;
    pthread_cond_t timeout_changed;
    
    // Statistics
    uint64_t total_timeouts;
    uint64_t expired_timeouts;
    uint64_t cancelled_timeouts;
} TimeoutManager;

// Performance optimization system
typedef struct PerformanceOptimizer {
    // Optimization strategies
    bool enable_lock_coarsening;
    bool enable_lock_elision;
    bool enable_adaptive_spinning;
    bool enable_numa_awareness;
    
    // Performance monitoring
    struct {
        ResourceType type;
        uint64_t contention_count;
        uint64_t avg_hold_time_ns;
        uint64_t avg_wait_time_ns;
        double utilization_rate;
    } *resource_metrics;
    size_t metric_count;
    
    // Optimization decisions
    struct {
        ResourceType type;
        uint64_t resource_id;
        enum {
            OPT_ACTION_NONE,
            OPT_ACTION_INCREASE_TIMEOUT,
            OPT_ACTION_CHANGE_STRATEGY,
            OPT_ACTION_ADD_RESOURCES,
            OPT_ACTION_REORDER_LOCKS
        } action;
        uint64_t action_time;
    } *optimization_actions;
    size_t action_count;
    
    // Optimization thread
    pthread_t optimizer_thread;
    bool is_active;
    uint64_t optimization_interval_ms;
    
    pthread_mutex_t optimizer_mutex;
    
    // Statistics
    uint64_t optimizations_applied;
    uint64_t performance_improvements;
    double avg_improvement_percentage;
} PerformanceOptimizer;

// Core deadlock prevention operations
ResourceManager* resource_manager_create(void);
void resource_manager_destroy(ResourceManager* manager);
Result_void_ptr resource_manager_start(ResourceManager* manager);
Result_void_ptr resource_manager_shutdown(ResourceManager* manager);

// Resource management
Resource* resource_create(ResourceManager* manager, ResourceType type, const char* name);
void resource_destroy(Resource* resource);
Resource* resource_find_by_id(ResourceManager* manager, uint64_t resource_id);
Resource* resource_find_by_name(ResourceManager* manager, const char* name);

// Resource allocation and deallocation
ResourceRequest* resource_request_create(uint64_t entity_id, ResourceType type, uint64_t resource_id, ResourceAccessMode mode);
void resource_request_destroy(ResourceRequest* request);
Result_void_ptr resource_acquire(ResourceManager* manager, ResourceRequest* request);
Result_void_ptr resource_acquire_timeout(ResourceManager* manager, ResourceRequest* request, uint64_t timeout_ms);
Result_void_ptr resource_release(ResourceManager* manager, uint64_t entity_id, uint64_t resource_id);
Result_void_ptr resource_release_all(ResourceManager* manager, uint64_t entity_id);

// Batch resource operations
typedef struct ResourceBatch {
    ResourceRequest** requests;
    size_t request_count;
    size_t request_capacity;
    bool all_or_nothing;  // Acquire all resources or none
} ResourceBatch;

ResourceBatch* resource_batch_create(size_t capacity);
void resource_batch_destroy(ResourceBatch* batch);
Result_void_ptr resource_batch_add(ResourceBatch* batch, ResourceRequest* request);
Result_void_ptr resource_batch_acquire(ResourceManager* manager, ResourceBatch* batch);
Result_void_ptr resource_batch_release(ResourceManager* manager, ResourceBatch* batch);

// Deadlock detection
DeadlockDetector* deadlock_detector_create(DeadlockDetectorConfig config);
void deadlock_detector_destroy(DeadlockDetector* detector);
Result_void_ptr deadlock_detector_start(DeadlockDetector* detector);
Result_void_ptr deadlock_detector_stop(DeadlockDetector* detector);
bool deadlock_detector_check_now(DeadlockDetector* detector);

// Lock ordering system
LockOrderingSystem* lock_ordering_create(void);
void lock_ordering_destroy(LockOrderingSystem* system);
Result_void_ptr lock_ordering_add_rule(LockOrderingSystem* system, ResourceType type1, ResourceType type2);
Result_void_ptr lock_ordering_add_resource_rule(LockOrderingSystem* system, uint64_t resource_id1, uint64_t resource_id2);
bool lock_ordering_check_order(LockOrderingSystem* system, ResourceType type, uint64_t resource_id);
Result_void_ptr lock_ordering_record_acquisition(LockOrderingSystem* system, ResourceType type, uint64_t resource_id);
Result_void_ptr lock_ordering_record_release(LockOrderingSystem* system, ResourceType type, uint64_t resource_id);

// Resource graph operations
ResourceGraph* resource_graph_create(void);
void resource_graph_destroy(ResourceGraph* graph);
Result_void_ptr resource_graph_add_resource(ResourceGraph* graph, Resource* resource);
Result_void_ptr resource_graph_add_entity(ResourceGraph* graph, uint64_t entity_id, ResourceType entity_type);
Result_void_ptr resource_graph_add_edge(ResourceGraph* graph, uint64_t from_entity, uint64_t to_resource);
Result_void_ptr resource_graph_remove_edge(ResourceGraph* graph, uint64_t from_entity, uint64_t to_resource);
bool resource_graph_has_cycle(ResourceGraph* graph);
Result_void_ptr resource_graph_find_cycles(ResourceGraph* graph, uint64_t** cycle_entities, size_t* cycle_length);

// Timeout management
TimeoutManager* timeout_manager_create(void);
void timeout_manager_destroy(TimeoutManager* manager);
Result_void_ptr timeout_manager_start(TimeoutManager* manager);
Result_void_ptr timeout_manager_stop(TimeoutManager* manager);
Result_void_ptr timeout_manager_add_timeout(TimeoutManager* manager, ResourceRequest* request, 
                                           void (*callback)(ResourceRequest*, void*), void* context);
Result_void_ptr timeout_manager_cancel_timeout(TimeoutManager* manager, uint64_t request_id);

// Performance optimization
PerformanceOptimizer* performance_optimizer_create(void);
void performance_optimizer_destroy(PerformanceOptimizer* optimizer);
Result_void_ptr performance_optimizer_start(PerformanceOptimizer* optimizer);
Result_void_ptr performance_optimizer_stop(PerformanceOptimizer* optimizer);
Result_void_ptr performance_optimizer_analyze_resource(PerformanceOptimizer* optimizer, Resource* resource);
Result_void_ptr performance_optimizer_apply_optimization(PerformanceOptimizer* optimizer, Resource* resource);

// Banker's algorithm implementation
typedef struct BankerState {
    // Resource allocation matrix
    int** allocation;    // allocation[i][j] = number of resource j allocated to process i
    int** max_need;      // max_need[i][j] = maximum number of resource j that process i may need
    int** need;          // need[i][j] = max_need[i][j] - allocation[i][j]
    int* available;      // available[j] = number of available instances of resource j
    
    size_t process_count;
    size_t resource_type_count;
    
    pthread_mutex_t banker_mutex;
} BankerState;

BankerState* banker_state_create(size_t process_count, size_t resource_type_count);
void banker_state_destroy(BankerState* state);
bool banker_is_safe_state(BankerState* state);
bool banker_can_grant_request(BankerState* state, size_t process_id, int* request);
Result_void_ptr banker_grant_request(BankerState* state, size_t process_id, int* request);
Result_void_ptr banker_release_resources(BankerState* state, size_t process_id, int* release);

// High-level deadlock prevention strategies
typedef struct DeadlockPreventionConfig {
    LockStrategy primary_strategy;
    LockStrategy fallback_strategy;
    DeadlockDetectionMethod detection_method;
    
    uint64_t default_timeout_ms;
    uint64_t detection_interval_ms;
    
    bool enable_wound_wait;
    bool enable_wait_die;
    bool enable_banker_algorithm;
    bool enable_lock_ordering;
    bool enable_timeout_based_prevention;
    
    // Resource limits
    int max_resources_per_entity;
    int max_concurrent_acquisitions;
    
    // Performance settings
    bool enable_performance_optimization;
    bool enable_adaptive_strategies;
    bool enable_load_balancing;
} DeadlockPreventionConfig;

typedef struct DeadlockPreventionSystem {
    DeadlockPreventionConfig config;
    ResourceManager* resource_manager;
    DeadlockDetector* deadlock_detector;
    LockOrderingSystem* lock_ordering;
    TimeoutManager* timeout_manager;
    PerformanceOptimizer* performance_optimizer;
    BankerState* banker_state;
    
    // System state
    bool is_active;
    atomic_bool shutdown_requested;
    
    // Integration
    ActorSystem* actor_system;
    TaskScope* task_scope;
    SharedVarManager* shared_var_manager;
    ChannelBroker* channel_broker;
    
    // Statistics
    uint64_t total_requests;
    uint64_t deadlocks_detected;
    uint64_t deadlocks_prevented;
    uint64_t false_positives;
    uint64_t successful_optimizations;
    
    pthread_mutex_t system_mutex;
} DeadlockPreventionSystem;

// High-level system operations
DeadlockPreventionSystem* deadlock_prevention_create(DeadlockPreventionConfig config);
void deadlock_prevention_destroy(DeadlockPreventionSystem* system);
Result_void_ptr deadlock_prevention_start(DeadlockPreventionSystem* system);
Result_void_ptr deadlock_prevention_shutdown(DeadlockPreventionSystem* system);

// Integration with other concurrency systems
Result_void_ptr deadlock_prevention_integrate_actors(DeadlockPreventionSystem* system, ActorSystem* actor_system);
Result_void_ptr deadlock_prevention_integrate_tasks(DeadlockPreventionSystem* system, TaskScope* task_scope);
Result_void_ptr deadlock_prevention_integrate_shared_vars(DeadlockPreventionSystem* system, SharedVarManager* var_manager);
Result_void_ptr deadlock_prevention_integrate_channels(DeadlockPreventionSystem* system, ChannelBroker* channel_broker);

// Monitoring and statistics
typedef struct DeadlockPreventionStats {
    uint64_t total_resources;
    uint64_t active_resources;
    uint64_t total_requests;
    uint64_t successful_acquisitions;
    uint64_t failed_acquisitions;
    uint64_t timeouts;
    uint64_t deadlocks_detected;
    uint64_t deadlocks_prevented;
    uint64_t false_positives;
    
    // Performance metrics
    uint64_t avg_acquisition_time_ns;
    uint64_t avg_detection_time_ns;
    double resource_utilization;
    double deadlock_prevention_effectiveness;
    
    // Strategy effectiveness
    uint64_t wound_wait_successes;
    uint64_t wait_die_successes;
    uint64_t banker_algorithm_successes;
    uint64_t timeout_based_successes;
    uint64_t lock_ordering_successes;
} DeadlockPreventionStats;

DeadlockPreventionStats deadlock_prevention_get_stats(DeadlockPreventionSystem* system);
void deadlock_prevention_reset_stats(DeadlockPreventionSystem* system);

// Configuration helpers
DeadlockPreventionConfig deadlock_prevention_config_default(void);
DeadlockPreventionConfig deadlock_prevention_config_conservative(void);
DeadlockPreventionConfig deadlock_prevention_config_aggressive(void);
DeadlockPreventionConfig deadlock_prevention_config_performance_focused(void);

// Utility macros
#define ACQUIRE_RESOURCES_ORDERED(manager, ...) \
    do { \
        ResourceRequest* __requests[] = {__VA_ARGS__}; \
        size_t __count = sizeof(__requests) / sizeof(__requests[0]); \
        ResourceBatch* __batch = resource_batch_create(__count); \
        for (size_t __i = 0; __i < __count; __i++) { \
            resource_batch_add(__batch, __requests[__i]); \
        } \
        Result_void_ptr __result = resource_batch_acquire(manager, __batch); \
        resource_batch_destroy(__batch); \
        if (__result.is_error) return __result; \
    } while(0)

#define RELEASE_RESOURCES_ALL(manager, entity_id) \
    resource_release_all(manager, entity_id)

#define WITH_RESOURCE_TIMEOUT(manager, request, timeout_ms, code) \
    do { \
        Result_void_ptr __acq_result = resource_acquire_timeout(manager, request, timeout_ms); \
        if (!__acq_result.is_error) { \
            code \
            resource_release(manager, (request)->requesting_entity_id, (request)->resource_id); \
        } else { \
            return __acq_result; \
        } \
    } while(0)

// Error codes specific to deadlock prevention
#define ERROR_DEADLOCK_DETECTED     0x4001
#define ERROR_RESOURCE_UNAVAILABLE  0x4002
#define ERROR_TIMEOUT_EXCEEDED      0x4003
#define ERROR_LOCK_ORDER_VIOLATION  0x4004
#define ERROR_RESOURCE_EXHAUSTED    0x4005
#define ERROR_BANKER_UNSAFE_STATE   0x4006
#define ERROR_CIRCULAR_DEPENDENCY   0x4007

#endif // GOO_DEADLOCK_PREVENTION_H