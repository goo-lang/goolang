#ifndef GOO_FEARLESS_CONCURRENCY_H
#define GOO_FEARLESS_CONCURRENCY_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include "ergonomic_errors.h"

// Forward declarations
typedef struct Actor Actor;
typedef struct ActorSystem ActorSystem;
typedef struct Message Message;
typedef struct Future Future;

// Actor system configuration
typedef struct {
    size_t max_actors;
    size_t thread_pool_size;
    size_t message_queue_size;
    size_t max_messages_per_actor;
    
    // Performance tuning
    bool numa_aware;
    bool work_stealing_enabled;
    size_t work_steal_batch_size;
    
    // Monitoring and debugging
    bool enable_actor_monitoring;
    bool enable_deadlock_detection;
    bool enable_performance_tracking;
    
    // Memory management
    size_t actor_memory_pool_size;
    size_t message_memory_pool_size;
    bool use_memory_pools;
} ActorSystemConfig;

// Message types for actor communication
typedef enum {
    MSG_TYPE_USER,          // User-defined message
    MSG_TYPE_SYSTEM,        // System management message
    MSG_TYPE_TERMINATE,     // Actor termination
    MSG_TYPE_SUPERVISION,   // Supervision tree messages
    MSG_TYPE_LIFECYCLE      // Actor lifecycle events
} MessageType;

// Message structure
typedef struct Message {
    MessageType type;
    uint64_t message_id;
    uint64_t timestamp;
    
    // Sender information
    Actor* sender;
    uint64_t sender_id;
    
    // Message payload
    void* payload;
    size_t payload_size;
    void (*payload_destructor)(void* payload);
    
    // Message metadata
    const char* message_name;
    int priority;
    uint64_t timeout_ms;
    
    // Response handling
    Future* response_future;
    bool expects_response;
    
    // Memory management
    bool owns_payload;
    struct Message* next;
} Message;

// Actor state management
typedef enum {
    ACTOR_STATE_INITIALIZING,
    ACTOR_STATE_ACTIVE,
    ACTOR_STATE_SUSPENDED,
    ACTOR_STATE_TERMINATING,
    ACTOR_STATE_TERMINATED,
    ACTOR_STATE_ERROR
} ActorState;

// Actor behavior definition
typedef struct ActorBehavior {
    // Message handlers
    Result_void_ptr (*handle_message)(Actor* self, Message* msg);
    Result_void_ptr (*handle_error)(Actor* self, Error* error);
    Result_void_ptr (*on_start)(Actor* self);
    Result_void_ptr (*on_stop)(Actor* self);
    
    // Supervision
    Result_void_ptr (*on_child_terminated)(Actor* self, Actor* child, Error* reason);
    
    // State management
    size_t state_size;
    void (*init_state)(void* state);
    void (*cleanup_state)(void* state);
} ActorBehavior;

// Actor structure
typedef struct Actor {
    uint64_t id;
    char name[64];
    ActorState state;
    
    // Actor hierarchy
    Actor* parent;
    Actor** children;
    size_t child_count;
    size_t child_capacity;
    
    // Message processing
    Message* message_queue;
    Message* message_queue_tail;
    size_t queue_size;
    size_t max_queue_size;
    
    // Synchronization
    pthread_mutex_t actor_mutex;
    pthread_cond_t message_available;
    
    // Behavior and state
    ActorBehavior behavior;
    void* actor_state;
    
    // Runtime information
    pthread_t thread_id;
    ActorSystem* system;
    uint64_t messages_processed;
    uint64_t total_processing_time_ns;
    
    // Error handling
    Error* last_error;
    int error_count;
    bool restart_on_error;
    
    // Resource management
    atomic_int ref_count;
    bool is_system_actor;
} Actor;

// Future for asynchronous results
typedef struct Future {
    uint64_t id;
    Actor* waiting_actor;
    
    // Result storage
    void* result;
    Error* error;
    bool is_completed;
    bool is_error;
    
    // Synchronization
    pthread_mutex_t future_mutex;
    pthread_cond_t completed;
    
    // Timeout handling
    uint64_t timeout_ms;
    uint64_t creation_time;
    
    // Cleanup
    void (*result_destructor)(void* result);
    
    // Chaining
    struct Future* next;
} Future;

// Actor system for managing all actors
typedef struct ActorSystem {
    ActorSystemConfig config;
    
    // Actor management
    Actor** actors;
    size_t actor_count;
    size_t actor_capacity;
    uint64_t next_actor_id;
    
    // Thread pool for actor execution
    pthread_t* worker_threads;
    size_t worker_count;
    
    // System-wide message queue for work distribution
    Message* global_message_queue;
    Message* global_queue_tail;
    size_t global_queue_size;
    
    // Synchronization
    pthread_mutex_t system_mutex;
    pthread_cond_t work_available;
    pthread_cond_t system_shutdown;
    
    // System state
    bool is_running;
    bool shutdown_requested;
    
    // Monitoring and metrics
    uint64_t total_messages_sent;
    uint64_t total_messages_processed;
    uint64_t total_actors_created;
    uint64_t total_actors_terminated;
    
    // Error handling
    Error** system_errors;
    size_t error_count;
    size_t error_capacity;
    
    // Memory pools
    void* actor_memory_pool;
    void* message_memory_pool;
    size_t pool_allocations;
    
    // Deadlock detection
    bool deadlock_detection_enabled;
    pthread_t deadlock_detector_thread;
    
    // System actors
    Actor* guardian_actor;      // Root supervisor
    Actor* deadlock_detector;   // Deadlock detection
    Actor* system_monitor;      // System monitoring
} ActorSystem;

// Supervision strategies
typedef enum {
    SUPERVISION_ONE_FOR_ONE,    // Restart only failed child
    SUPERVISION_ONE_FOR_ALL,    // Restart all children if one fails
    SUPERVISION_REST_FOR_ONE,   // Restart failed child and all younger siblings
    SUPERVISION_ESCALATE        // Escalate to parent supervisor
} SupervisionStrategy;

typedef struct SupervisionConfig {
    SupervisionStrategy strategy;
    int max_restarts;
    uint64_t time_window_ms;
    uint64_t restart_delay_ms;
    bool restart_jitter;
} SupervisionConfig;

// Actor system management
ActorSystem* actor_system_create(ActorSystemConfig config);
void actor_system_destroy(ActorSystem* system);
Result_void_ptr actor_system_start(ActorSystem* system);
Result_void_ptr actor_system_shutdown(ActorSystem* system, uint64_t timeout_ms);

// Actor lifecycle
Actor* actor_spawn(ActorSystem* system, ActorBehavior behavior, const char* name);
Actor* actor_spawn_child(Actor* parent, ActorBehavior behavior, const char* name);
Result_void_ptr actor_start(Actor* actor);
Result_void_ptr actor_stop(Actor* actor);
Result_void_ptr actor_terminate(Actor* actor, Error* reason);

// Message passing
Future* actor_send(Actor* target, const char* message_name, void* payload, size_t payload_size);
Future* actor_send_with_timeout(Actor* target, const char* message_name, 
                               void* payload, size_t payload_size, uint64_t timeout_ms);
Result_void_ptr actor_send_async(Actor* target, const char* message_name, 
                                void* payload, size_t payload_size);

// Future operations
Result_void_ptr future_await(Future* future);
Result_void_ptr future_await_timeout(Future* future, uint64_t timeout_ms);
bool future_is_ready(Future* future);
void future_destroy(Future* future);

// Combinators for futures
Future* future_then(Future* future, Result_void_ptr (*callback)(void* result));
Future* future_catch(Future* future, Result_void_ptr (*error_handler)(Error* error));
Future* future_all(Future** futures, size_t count);
Future* future_any(Future** futures, size_t count);
Future* future_timeout(Future* future, uint64_t timeout_ms);

// Actor supervision
Result_void_ptr actor_supervise(Actor* supervisor, Actor* child, SupervisionConfig config);
Result_void_ptr actor_restart(Actor* actor);
Result_void_ptr actor_escalate_error(Actor* actor, Error* error);

// Actor discovery and management
Actor* actor_find_by_name(ActorSystem* system, const char* name);
Actor* actor_find_by_id(ActorSystem* system, uint64_t id);
Actor** actor_get_children(Actor* actor, size_t* count);
Actor* actor_get_parent(Actor* actor);

// Message creation and management
Message* message_create(const char* name, void* payload, size_t payload_size);
Message* message_create_with_timeout(const char* name, void* payload, 
                                    size_t payload_size, uint64_t timeout_ms);
void message_destroy(Message* message);

// Actor behavior helpers
#define ACTOR_BEHAVIOR(name) \
    static ActorBehavior name##_behavior = { \
        .handle_message = name##_handle_message, \
        .handle_error = name##_handle_error, \
        .on_start = name##_on_start, \
        .on_stop = name##_on_stop, \
        .state_size = sizeof(name##_State), \
        .init_state = name##_init_state, \
        .cleanup_state = name##_cleanup_state \
    }

// Convenience macros for actor definition
#define DEFINE_ACTOR_STATE(name) \
    typedef struct name##_State name##_State

#define ACTOR_HANDLE_MESSAGE(name) \
    Result_void_ptr name##_handle_message(Actor* self, Message* msg)

#define ACTOR_ON_START(name) \
    Result_void_ptr name##_on_start(Actor* self)

#define ACTOR_ON_STOP(name) \
    Result_void_ptr name##_on_stop(Actor* self)

#define ACTOR_HANDLE_ERROR(name) \
    Result_void_ptr name##_handle_error(Actor* self, Error* error)

#define GET_ACTOR_STATE(type, actor) \
    ((type*)((actor)->actor_state))

// System monitoring and metrics
typedef struct ActorSystemMetrics {
    uint64_t total_actors;
    uint64_t active_actors;
    uint64_t total_messages_sent;
    uint64_t total_messages_processed;
    uint64_t average_message_latency_ns;
    uint64_t peak_message_queue_size;
    
    // Performance metrics
    double cpu_utilization;
    size_t memory_usage_bytes;
    uint64_t thread_pool_efficiency;
    
    // Error metrics
    uint64_t total_actor_errors;
    uint64_t total_actor_restarts;
    uint64_t deadlocks_detected;
    uint64_t deadlocks_resolved;
} ActorSystemMetrics;

ActorSystemMetrics actor_system_get_metrics(ActorSystem* system);
void actor_system_reset_metrics(ActorSystem* system);

// Deadlock detection and prevention
typedef struct DeadlockInfo {
    Actor** actors_involved;
    size_t actor_count;
    Message** messages_involved;
    size_t message_count;
    uint64_t detection_time;
    const char* deadlock_description;
} DeadlockInfo;

bool actor_system_detect_deadlock(ActorSystem* system, DeadlockInfo* info);
Result_void_ptr actor_system_resolve_deadlock(ActorSystem* system, DeadlockInfo* info);

// Configuration helpers
ActorSystemConfig actor_system_default_config(void);
ActorSystemConfig actor_system_performance_config(void);
ActorSystemConfig actor_system_reliable_config(void);

// Advanced actor patterns
typedef struct ActorPool {
    Actor** workers;
    size_t worker_count;
    size_t current_worker;
    ActorBehavior worker_behavior;
    SupervisionConfig supervision;
} ActorPool;

ActorPool* actor_pool_create(ActorSystem* system, size_t worker_count, 
                           ActorBehavior behavior, const char* pool_name);
Result_void_ptr actor_pool_send(ActorPool* pool, const char* message_name, 
                               void* payload, size_t payload_size);
void actor_pool_destroy(ActorPool* pool);

// Router patterns for load balancing
typedef enum {
    ROUTER_ROUND_ROBIN,
    ROUTER_RANDOM,
    ROUTER_LEAST_LOADED,
    ROUTER_CONSISTENT_HASH,
    ROUTER_BROADCAST
} RouterStrategy;

typedef struct ActorRouter {
    Actor** destinations;
    size_t destination_count;
    RouterStrategy strategy;
    size_t current_index;
    uint64_t (*hash_function)(void* payload, size_t size);
} ActorRouter;

ActorRouter* actor_router_create(RouterStrategy strategy, Actor** destinations, size_t count);
Result_void_ptr actor_router_send(ActorRouter* router, const char* message_name, 
                                 void* payload, size_t payload_size);
void actor_router_destroy(ActorRouter* router);

// Integration with existing error handling
void actor_integrate_error_handling(ActorSystem* system, ErgoErrorContext* error_context);

// System lifecycle hooks
typedef struct SystemLifecycleHooks {
    void (*on_system_start)(ActorSystem* system);
    void (*on_system_stop)(ActorSystem* system);
    void (*on_actor_created)(Actor* actor);
    void (*on_actor_terminated)(Actor* actor, Error* reason);
    void (*on_deadlock_detected)(DeadlockInfo* info);
    void (*on_error_escalated)(Actor* actor, Error* error);
} SystemLifecycleHooks;

void actor_system_set_lifecycle_hooks(ActorSystem* system, SystemLifecycleHooks hooks);

#endif // GOO_FEARLESS_CONCURRENCY_H