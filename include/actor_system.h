#ifndef GOO_ACTOR_SYSTEM_H
#define GOO_ACTOR_SYSTEM_H

#include "runtime.h"
#include "error_hierarchies.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

// =============================================================================
// Actor System Core - Fearless Concurrency Foundation
// =============================================================================

// Forward declarations
typedef struct Actor Actor;
typedef struct ActorSystem ActorSystem;
typedef struct ActorMessage ActorMessage;
typedef struct ActorMailbox ActorMailbox;
typedef struct ActorRef ActorRef;
typedef struct ActorFuture ActorFuture;
typedef struct ActorBehavior ActorBehavior;
typedef struct ActorContext ActorContext;
typedef struct ActorSupervisor ActorSupervisor;

// =============================================================================
// Actor Message System
// =============================================================================

// Message types for type-safe communication
typedef enum {
    ACTOR_MSG_USER,         // User-defined message
    ACTOR_MSG_SYSTEM,       // System control message
    ACTOR_MSG_LIFECYCLE,    // Lifecycle management
    ACTOR_MSG_ERROR,        // Error propagation
    ACTOR_MSG_SUPERVISION   // Supervisor commands
} ActorMessageType;

// Message priorities for scheduling
typedef enum {
    ACTOR_PRIORITY_LOW      = 0,
    ACTOR_PRIORITY_NORMAL   = 1,
    ACTOR_PRIORITY_HIGH     = 2,
    ACTOR_PRIORITY_URGENT   = 3,
    ACTOR_PRIORITY_SYSTEM   = 4
} ActorMessagePriority;

// System messages for actor lifecycle
typedef enum {
    ACTOR_SYSTEM_START,         // Start actor processing
    ACTOR_SYSTEM_STOP,          // Graceful shutdown
    ACTOR_SYSTEM_KILL,          // Immediate termination
    ACTOR_SYSTEM_RESTART,       // Restart after failure
    ACTOR_SYSTEM_SUSPEND,       // Temporarily suspend
    ACTOR_SYSTEM_RESUME,        // Resume from suspension
    ACTOR_SYSTEM_SUPERVISE      // Supervision directive
} ActorSystemMessageType;

// Actor message structure
typedef struct ActorMessage {
    // Message identification
    uint64_t message_id;            // Unique message ID
    ActorMessageType type;          // Message type
    ActorMessagePriority priority;  // Message priority
    
    // Content
    void* payload;                  // Message payload
    size_t payload_size;           // Size of payload
    void (*payload_destructor)(void*); // Cleanup function
    
    // Routing information
    ActorRef* sender;              // Sender reference
    ActorRef* receiver;            // Receiver reference
    const char* handler_name;      // Target handler method
    
    // Metadata
    uint64_t timestamp;            // Creation timestamp
    uint64_t timeout_ms;           // Message timeout
    uint32_t retry_count;          // Retry attempts
    uint32_t max_retries;          // Maximum retries
    
    // System messages
    union {
        ActorSystemMessageType system_type;
        struct {
            StructuredError* error;     // Error information
            const char* error_context;  // Error context
        } error_info;
    } system_data;
    
    // Memory management
    bool owns_payload;             // Whether message owns payload
    Arena* arena;                  // Arena for message memory
    
} ActorMessage;

// =============================================================================
// Actor Mailbox - Thread-Safe Message Queue
// =============================================================================

// Mailbox configuration
typedef struct ActorMailboxConfig {
    size_t max_capacity;           // Maximum messages in mailbox
    bool drop_on_overflow;         // Drop messages if full
    bool priority_queue;           // Use priority-based ordering
    uint64_t message_timeout_ms;   // Default message timeout
    bool enable_metrics;           // Collect mailbox metrics
} ActorMailboxConfig;

// Mailbox statistics
typedef struct ActorMailboxStats {
    uint64_t messages_received;    // Total messages received
    uint64_t messages_processed;   // Total messages processed
    uint64_t messages_dropped;     // Messages dropped due to overflow
    uint64_t messages_timed_out;   // Messages that timed out
    size_t current_size;           // Current mailbox size
    size_t peak_size;              // Peak mailbox size
    double avg_processing_time_ms; // Average message processing time
} ActorMailboxStats;

// Actor mailbox structure
typedef struct ActorMailbox {
    // Message storage
    ActorMessage** messages;       // Array of message pointers
    size_t capacity;               // Maximum capacity
    size_t size;                   // Current size
    size_t head;                   // Head of circular buffer
    size_t tail;                   // Tail of circular buffer
    
    // Priority queue (optional)
    ActorMessage** priority_heap;  // Priority heap for messages
    size_t heap_size;              // Current heap size
    
    // Synchronization
    pthread_mutex_t mutex;         // Mailbox protection
    pthread_cond_t not_empty;      // Signal for waiting actors
    pthread_cond_t not_full;       // Signal for senders
    
    // Configuration and statistics
    ActorMailboxConfig config;     // Mailbox configuration
    ActorMailboxStats stats;       // Performance statistics
    
    // Status
    bool is_closed;                // Whether mailbox is closed
    bool is_suspended;             // Whether message processing is suspended
    
} ActorMailbox;

// =============================================================================
// Actor Future System - Async Result Handling
// =============================================================================

// Future states
typedef enum {
    ACTOR_FUTURE_PENDING,          // Not yet completed
    ACTOR_FUTURE_COMPLETED,        // Successfully completed
    ACTOR_FUTURE_FAILED,           // Failed with error
    ACTOR_FUTURE_CANCELLED,        // Cancelled before completion
    ACTOR_FUTURE_TIMEOUT           // Timed out
} ActorFutureState;

// Future callbacks
typedef void (*ActorFutureCallback)(ActorFuture* future, void* context);
typedef void (*ActorFutureErrorCallback)(ActorFuture* future, StructuredError* error, void* context);

// Actor future structure
typedef struct ActorFuture {
    // Identification
    uint64_t future_id;            // Unique future ID
    uint64_t message_id;           // Associated message ID
    
    // State
    ActorFutureState state;        // Current state
    pthread_mutex_t mutex;         // State protection
    pthread_cond_t completed;      // Completion signal
    
    // Result
    void* result;                  // Future result
    size_t result_size;            // Size of result
    void (*result_destructor)(void*); // Result cleanup
    
    // Error handling
    StructuredError* error;        // Error if failed
    const char* error_context;     // Error context
    
    // Timing
    uint64_t created_at;           // Creation timestamp
    uint64_t completed_at;         // Completion timestamp
    uint64_t timeout_ms;           // Timeout duration
    
    // Callbacks
    ActorFutureCallback on_complete; // Success callback
    ActorFutureErrorCallback on_error; // Error callback
    void* callback_context;        // Callback context
    
    // Chaining
    ActorFuture** chained_futures; // Dependent futures
    int chained_count;             // Number of chained futures
    
    // Reference counting
    int ref_count;                 // Reference count
    pthread_mutex_t ref_mutex;     // Reference count protection
    
} ActorFuture;

// =============================================================================
// Actor Behavior and Handlers
// =============================================================================

// Handler function type
typedef ActorFuture* (*ActorHandler)(Actor* actor, ActorMessage* message, ActorContext* context);

// Message handler entry
typedef struct ActorHandlerEntry {
    const char* message_name;      // Message type name
    ActorHandler handler;          // Handler function
    bool is_async;                 // Whether handler is async
    uint64_t timeout_ms;           // Handler timeout
    int priority;                  // Handler priority
} ActorHandlerEntry;

// Actor behavior definition
typedef struct ActorBehavior {
    const char* behavior_name;     // Behavior name
    ActorHandlerEntry* handlers;   // Array of message handlers
    int handler_count;             // Number of handlers
    
    // Lifecycle hooks
    ActorHandler on_start;         // Called when actor starts
    ActorHandler on_stop;          // Called when actor stops
    ActorHandler on_error;         // Called on error
    ActorHandler on_restart;       // Called on restart
    
    // State management
    void* (*create_state)(void);   // Create initial state
    void (*destroy_state)(void*);  // Destroy actor state
    
    // Configuration
    size_t mailbox_capacity;       // Default mailbox capacity
    uint64_t message_timeout_ms;   // Default message timeout
    bool enable_supervision;       // Enable supervision
    
} ActorBehavior;

// =============================================================================
// Actor Context and State
// =============================================================================

// Actor execution context
typedef struct ActorContext {
    // Actor reference
    Actor* self;                   // Reference to self
    ActorRef* self_ref;            // Self reference for messaging
    
    // Current message
    ActorMessage* current_message; // Currently processing message
    ActorFuture* current_future;   // Current message future
    
    // Sender context
    ActorRef* sender;              // Current message sender
    
    // System access
    ActorSystem* system;           // Actor system reference
    ActorSupervisor* supervisor;   // Supervisor reference
    
    // Timing and statistics
    uint64_t start_time;           // Actor start time
    uint64_t message_count;        // Messages processed
    uint64_t error_count;          // Errors encountered
    
    // Memory management
    Arena* message_arena;          // Arena for message processing
    Arena* state_arena;            // Arena for actor state
    
} ActorContext;

// =============================================================================
// Actor Core Structure
// =============================================================================

// Actor states
typedef enum {
    ACTOR_STATE_CREATED,           // Just created
    ACTOR_STATE_STARTING,          // Starting up
    ACTOR_STATE_RUNNING,           // Normal operation
    ACTOR_STATE_SUSPENDED,         // Temporarily suspended
    ACTOR_STATE_STOPPING,          // Graceful shutdown
    ACTOR_STATE_STOPPED,           // Fully stopped
    ACTOR_STATE_FAILED,            // Failed and awaiting restart
    ACTOR_STATE_RESTARTING         // Restarting after failure
} ActorState;

// Actor structure
typedef struct Actor {
    // Identification
    uint64_t actor_id;             // Unique actor ID
    const char* actor_name;        // Human-readable name
    const char* actor_type;        // Actor type/behavior name
    
    // State and behavior
    ActorState state;              // Current actor state
    ActorBehavior* behavior;       // Actor behavior definition
    void* user_state;              // User-defined state
    
    // Communication
    ActorMailbox* mailbox;         // Message mailbox
    ActorRef* self_ref;            // Self reference
    
    // Context
    ActorContext* context;         // Execution context
    
    // Supervision
    ActorSupervisor* supervisor;   // Parent supervisor
    Actor** children;              // Child actors
    int child_count;               // Number of children
    int child_capacity;            // Children array capacity
    
    // Threading
    pthread_t thread;              // Actor thread
    bool owns_thread;              // Whether actor owns its thread
    
    // Synchronization
    pthread_mutex_t state_mutex;   // State protection
    pthread_cond_t state_changed;  // State change notification
    
    // Statistics
    struct {
        uint64_t messages_processed;
        uint64_t messages_failed;
        uint64_t restarts;
        uint64_t uptime_ms;
        double avg_message_time_ms;
    } stats;
    
    // Memory management
    Arena* actor_arena;            // Arena for actor memory
    
} Actor;

// =============================================================================
// Actor Reference System
// =============================================================================

// Actor reference for safe messaging
typedef struct ActorRef {
    uint64_t actor_id;             // Referenced actor ID
    ActorSystem* system;           // Actor system
    
    // Validation
    uint64_t version;              // Reference version for validation
    bool is_valid;                 // Whether reference is still valid
    
    // Caching
    Actor* cached_actor;           // Cached actor pointer (unsafe)
    uint64_t cache_version;        // Cache validation version
    
    // Reference counting
    int ref_count;                 // Reference count
    pthread_mutex_t ref_mutex;     // Reference protection
    
} ActorRef;

// =============================================================================
// Actor Supervision System
// =============================================================================

// Supervision strategies
typedef enum {
    ACTOR_SUPERVISE_ONE_FOR_ONE,   // Restart only failed child
    ACTOR_SUPERVISE_ONE_FOR_ALL,   // Restart all children if one fails
    ACTOR_SUPERVISE_REST_FOR_ONE,  // Restart failed child and those started after it
    ACTOR_SUPERVISE_ESCALATE       // Escalate error to parent supervisor
} ActorSupervisionStrategy;

// Restart strategies
typedef enum {
    ACTOR_RESTART_PERMANENT,       // Always restart
    ACTOR_RESTART_TEMPORARY,       // Never restart
    ACTOR_RESTART_TRANSIENT        // Restart only if terminated abnormally
} ActorRestartStrategy;

// Supervision configuration
typedef struct ActorSupervisionConfig {
    ActorSupervisionStrategy strategy;    // Supervision strategy
    ActorRestartStrategy restart_policy;  // Restart policy
    uint32_t max_restarts;               // Maximum restarts in time window
    uint64_t time_window_ms;             // Time window for restart counting
    uint64_t restart_delay_ms;           // Delay between restarts
    bool escalate_on_limit;              // Escalate if restart limit exceeded
} ActorSupervisionConfig;

// Actor supervisor
typedef struct ActorSupervisor {
    // Identification
    uint64_t supervisor_id;        // Unique supervisor ID
    const char* name;              // Supervisor name
    
    // Configuration
    ActorSupervisionConfig config; // Supervision configuration
    
    // Supervised actors
    Actor** children;              // Array of child actors
    int child_count;               // Number of children
    int child_capacity;            // Children array capacity
    
    // Parent/child relationships
    ActorSupervisor* parent;       // Parent supervisor
    ActorSupervisor** child_supervisors; // Child supervisors
    int child_supervisor_count;    // Number of child supervisors
    
    // Statistics
    struct {
        uint64_t children_started;
        uint64_t children_stopped;
        uint64_t children_restarted;
        uint64_t children_failed;
        uint64_t escalations;
    } stats;
    
    // Synchronization
    pthread_mutex_t children_mutex; // Children array protection
    
} ActorSupervisor;

// =============================================================================
// Actor System Management
// =============================================================================

// Actor system configuration
typedef struct ActorSystemConfig {
    // Threading
    int thread_pool_size;          // Number of worker threads
    bool use_dedicated_threads;    // Use dedicated threads for actors
    
    // Scheduling
    const char* scheduler_type;    // Scheduler type ("work_stealing", "round_robin")
    uint64_t message_timeout_ms;   // Default message timeout
    uint64_t actor_timeout_ms;     // Default actor timeout
    
    // Memory management
    size_t default_arena_size;     // Default arena size for actors
    size_t message_pool_size;      // Message pool size
    
    // Supervision
    ActorSupervisionConfig default_supervision; // Default supervision config
    
    // Performance
    bool enable_metrics;           // Enable performance metrics
    bool enable_tracing;           // Enable message tracing
    uint64_t gc_interval_ms;       // Garbage collection interval
    
    // Debugging
    bool debug_mode;               // Enable debug mode
    const char* log_level;         // Logging level
    
} ActorSystemConfig;

// Actor system statistics
typedef struct ActorSystemStats {
    // Actors
    uint64_t actors_created;       // Total actors created
    uint64_t actors_active;        // Currently active actors
    uint64_t actors_stopped;       // Actors that have stopped
    uint64_t actors_failed;        // Actors that have failed
    
    // Messages
    uint64_t messages_sent;        // Total messages sent
    uint64_t messages_processed;   // Total messages processed
    uint64_t messages_dropped;     // Messages dropped
    uint64_t messages_timed_out;   // Messages that timed out
    
    // Performance
    double avg_message_latency_ms; // Average message latency
    double system_cpu_usage;       // System CPU usage
    uint64_t total_memory_bytes;   // Total memory usage
    
    // Errors
    uint64_t system_errors;        // System-level errors
    uint64_t supervision_events;   // Supervision events
    
} ActorSystemStats;

// Main actor system structure
typedef struct ActorSystem {
    // Identification
    const char* system_name;       // System name
    uint64_t system_id;            // Unique system ID
    
    // Configuration
    ActorSystemConfig config;      // System configuration
    
    // Actor management
    Actor** actors;                // Array of all actors
    int actor_count;               // Number of actors
    int actor_capacity;            // Actor array capacity
    pthread_mutex_t actors_mutex;  // Actor array protection
    
    // Actor references
    ActorRef** actor_refs;         // Array of actor references
    int ref_count;                 // Number of references
    pthread_mutex_t refs_mutex;    // Reference array protection
    
    // Threading
    pthread_t* worker_threads;     // Worker thread pool
    int worker_count;              // Number of worker threads
    bool system_running;           // Whether system is running
    
    // Message dispatch
    ActorMessage** message_queue;  // Global message queue
    size_t queue_capacity;         // Queue capacity
    size_t queue_size;             // Current queue size
    size_t queue_head;             // Queue head
    size_t queue_tail;             // Queue tail
    pthread_mutex_t queue_mutex;   // Queue protection
    pthread_cond_t queue_not_empty; // Queue notification
    
    // Supervision
    ActorSupervisor* root_supervisor; // Root supervisor
    
    // Statistics and monitoring
    ActorSystemStats stats;        // System statistics
    uint64_t start_time;           // System start time
    
    // Memory management
    Arena* system_arena;           // Arena for system memory
    
    // Shutdown
    bool shutdown_requested;       // Shutdown flag
    pthread_mutex_t shutdown_mutex; // Shutdown synchronization
    
} ActorSystem;

// =============================================================================
// Core API Functions
// =============================================================================

// System lifecycle
ActorSystem* actor_system_create(const char* name, ActorSystemConfig* config);
void actor_system_destroy(ActorSystem* system);
bool actor_system_start(ActorSystem* system);
void actor_system_stop(ActorSystem* system);
void actor_system_await_termination(ActorSystem* system);

// Actor lifecycle
Actor* actor_create(ActorSystem* system, const char* name, ActorBehavior* behavior);
ActorRef* actor_spawn(ActorSystem* system, const char* name, ActorBehavior* behavior);
void actor_stop(ActorRef* actor_ref);
void actor_kill(ActorRef* actor_ref);
void actor_restart(ActorRef* actor_ref);

// Actor references
ActorRef* actor_ref_create(ActorSystem* system, uint64_t actor_id);
void actor_ref_retain(ActorRef* ref);
void actor_ref_release(ActorRef* ref);
bool actor_ref_is_valid(ActorRef* ref);
Actor* actor_ref_resolve(ActorRef* ref);

// Message passing
ActorFuture* actor_send(ActorRef* to, const char* handler, void* payload, size_t payload_size);
ActorFuture* actor_send_with_timeout(ActorRef* to, const char* handler, void* payload, 
                                     size_t payload_size, uint64_t timeout_ms);
void actor_send_system_message(ActorRef* to, ActorSystemMessageType msg_type);

// Future operations
void* actor_future_await(ActorFuture* future);
void* actor_future_await_timeout(ActorFuture* future, uint64_t timeout_ms);
bool actor_future_is_completed(ActorFuture* future);
bool actor_future_is_failed(ActorFuture* future);
StructuredError* actor_future_get_error(ActorFuture* future);
void actor_future_set_callback(ActorFuture* future, ActorFutureCallback callback, void* context);
ActorFuture* actor_future_chain(ActorFuture* future, ActorFutureCallback transform, void* context);
void actor_future_release(ActorFuture* future);

// Behavior definition
ActorBehavior* actor_behavior_create(const char* name);
void actor_behavior_destroy(ActorBehavior* behavior);
void actor_behavior_add_handler(ActorBehavior* behavior, const char* message_name, 
                               ActorHandler handler);
void actor_behavior_set_lifecycle_hooks(ActorBehavior* behavior,
                                       ActorHandler on_start,
                                       ActorHandler on_stop,
                                       ActorHandler on_error,
                                       ActorHandler on_restart);

// Supervision
ActorSupervisor* actor_supervisor_create(const char* name, ActorSupervisionConfig* config);
void actor_supervisor_destroy(ActorSupervisor* supervisor);
void actor_supervisor_add_child(ActorSupervisor* supervisor, Actor* child);
void actor_supervisor_remove_child(ActorSupervisor* supervisor, Actor* child);
void actor_supervisor_handle_failure(ActorSupervisor* supervisor, Actor* failed_child, 
                                    StructuredError* error);

// =============================================================================
// Utility Functions and Macros
// =============================================================================

// Convenience macros for actor definition
#define ACTOR_HANDLER(name) \
    ActorFuture* name(Actor* actor, ActorMessage* message, ActorContext* context)

#define ACTOR_DEFINE_BEHAVIOR(name) \
    ActorBehavior* name##_behavior_create(void)

#define ACTOR_REGISTER_HANDLER(behavior, msg_name, handler_func) \
    actor_behavior_add_handler((behavior), (msg_name), (handler_func))

// Message creation helpers
ActorMessage* actor_message_create(const char* handler_name, void* payload, size_t payload_size);
void actor_message_destroy(ActorMessage* message);
ActorMessage* actor_message_create_system(ActorSystemMessageType type);

// Mailbox operations
ActorMailbox* actor_mailbox_create(ActorMailboxConfig* config);
void actor_mailbox_destroy(ActorMailbox* mailbox);
bool actor_mailbox_send(ActorMailbox* mailbox, ActorMessage* message);
ActorMessage* actor_mailbox_receive(ActorMailbox* mailbox);
ActorMessage* actor_mailbox_try_receive(ActorMailbox* mailbox);
void actor_mailbox_close(ActorMailbox* mailbox);

// Statistics and monitoring
ActorSystemStats actor_system_get_stats(ActorSystem* system);
void actor_system_print_stats(ActorSystem* system);
ActorMailboxStats actor_mailbox_get_stats(ActorMailbox* mailbox);

// Debugging and introspection
void actor_system_dump_actors(ActorSystem* system);
void actor_dump_mailbox(Actor* actor);
void actor_trace_message(ActorMessage* message, const char* event);

// Performance utilities
void actor_system_optimize_scheduling(ActorSystem* system);
void actor_system_balance_load(ActorSystem* system);
void actor_system_gc(ActorSystem* system);

// Integration with error handling
void actor_register_error_handlers(ActorSystem* system);
ActorFuture* actor_propagate_error(Actor* actor, StructuredError* error);

#endif // GOO_ACTOR_SYSTEM_H