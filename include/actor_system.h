#ifndef GOO_ACTOR_SYSTEM_H
#define GOO_ACTOR_SYSTEM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <time.h>
#include "ergonomic_errors.h"

// Forward declarations
typedef struct Actor Actor;
typedef struct ActorSystem ActorSystem;
typedef struct ActorMessage ActorMessage;
typedef struct ActorQueue ActorQueue;
typedef struct ActorFuture ActorFuture;
typedef struct ActorRef ActorRef;
typedef struct ActorMailbox ActorMailbox;

// Actor system configuration
#define MAX_ACTORS 10000
#define MAX_MESSAGE_SIZE 4096
#define DEFAULT_MAILBOX_SIZE 1000
#define ACTOR_THREAD_POOL_SIZE 8

// Actor states
typedef enum {
    ACTOR_STATE_CREATED,
    ACTOR_STATE_RUNNING,
    ACTOR_STATE_SUSPENDED,
    ACTOR_STATE_TERMINATED,
    ACTOR_STATE_ERROR
} ActorState;

// Message types
typedef enum {
    MESSAGE_TYPE_USER,
    MESSAGE_TYPE_SYSTEM_START,
    MESSAGE_TYPE_SYSTEM_STOP,
    MESSAGE_TYPE_SYSTEM_RESTART,
    MESSAGE_TYPE_SYSTEM_SUPERVISION
} MessageType;

// Future states
typedef enum {
    FUTURE_STATE_PENDING,
    FUTURE_STATE_COMPLETED,
    FUTURE_STATE_ERROR,
    FUTURE_STATE_CANCELLED
} FutureState;

// Actor supervision strategies
typedef enum {
    SUPERVISION_ONE_FOR_ONE,
    SUPERVISION_ONE_FOR_ALL,
    SUPERVISION_REST_FOR_ONE
} SupervisionStrategy;

// Actor message structure
typedef struct ActorMessage {
    uint64_t message_id;
    MessageType type;
    uint64_t sender_id;
    uint64_t receiver_id;
    
    // Message payload
    void* data;
    size_t data_size;
    void (*data_destructor)(void*);
    
    // Response handling
    ActorFuture* response_future;
    bool expects_response;
    uint64_t correlation_id;
    
    // Timing
    struct timespec created_at;
    struct timespec deadline;
    
    struct ActorMessage* next;
} ActorMessage;

// Actor future for asynchronous responses
typedef struct ActorFuture {
    uint64_t future_id;
    uint64_t correlation_id;
    FutureState state;
    
    // Result data
    void* result;
    size_t result_size;
    Error* error;
    
    // Synchronization
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    
    // Callbacks
    void (*completion_callback)(ActorFuture*, void*);
    void* callback_context;
    
    // Timing
    struct timespec created_at;
    struct timespec completed_at;
    uint64_t timeout_ms;
    
    // Reference counting
    uint32_t ref_count;
    
    struct ActorFuture* next;
} ActorFuture;

// Actor mailbox with message queue
typedef struct ActorMailbox {
    ActorMessage* head;
    ActorMessage* tail;
    size_t message_count;
    size_t max_messages;
    
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    
    bool closed;
} ActorMailbox;

// Actor reference for lightweight actor addressing
typedef struct ActorRef {
    uint64_t actor_id;
    char actor_name[128];
    bool is_valid;
    
    // Remote actor support
    bool is_remote;
    char remote_address[256];
    uint16_t remote_port;
    
    // Reference counting
    uint32_t ref_count;
    pthread_mutex_t ref_mutex;
} ActorRef;

// Actor behavior definition
typedef struct ActorBehavior {
    // Message handlers
    void (*handle_message)(Actor* self, ActorMessage* message);
    void (*pre_start)(Actor* self);
    void (*post_stop)(Actor* self);
    void (*pre_restart)(Actor* self, Error* reason);
    void (*post_restart)(Actor* self, Error* reason);
    
    // Supervision
    SupervisionStrategy supervision_strategy;
    int max_restarts;
    uint64_t restart_window_ms;
} ActorBehavior;

// Main actor structure
typedef struct Actor {
    uint64_t id;
    char name[128];
    ActorState state;
    
    // Behavior and context
    ActorBehavior behavior;
    void* context;
    size_t context_size;
    
    // Mailbox for incoming messages
    ActorMailbox* mailbox;
    
    // Reference to self
    ActorRef* self_ref;
    
    // Parent-child relationships
    ActorRef* parent;
    ActorRef** children;
    size_t child_count;
    size_t max_children;
    
    // Supervision and error handling
    int restart_count;
    struct timespec last_restart;
    struct timespec created_at;
    
    // Thread management
    pthread_t thread;
    bool thread_running;
    
    // Statistics
    uint64_t messages_processed;
    uint64_t messages_sent;
    uint64_t errors_count;
    struct timespec last_activity;
    
    // System references
    ActorSystem* system;
    
    struct Actor* next;
} Actor;

// Actor system runtime
typedef struct ActorSystem {
    char name[128];
    bool is_running;
    
    // Actor management
    Actor** actors;
    size_t actor_count;
    size_t max_actors;
    uint64_t next_actor_id;
    pthread_mutex_t actors_mutex;
    
    // Thread pool for actor execution
    pthread_t* thread_pool;
    size_t thread_pool_size;
    bool* thread_active;
    
    // Future management
    ActorFuture** futures;
    size_t future_count;
    size_t max_futures;
    uint64_t next_future_id;
    pthread_mutex_t futures_mutex;
    
    // System-wide message queue
    ActorQueue* system_queue;
    
    // Supervision
    ActorRef* guardian;
    
    // Configuration
    size_t default_mailbox_size;
    uint64_t message_timeout_ms;
    uint64_t actor_idle_timeout_ms;
    
    // Statistics
    uint64_t total_messages_sent;
    uint64_t total_messages_processed;
    uint64_t total_actors_created;
    uint64_t total_actors_terminated;
    
    // Shutdown
    bool shutdown_requested;
    pthread_mutex_t shutdown_mutex;
    pthread_cond_t shutdown_condition;
} ActorSystem;

// Actor system message queue
typedef struct ActorQueue {
    ActorMessage** messages;
    size_t head;
    size_t tail;
    size_t capacity;
    size_t count;
    
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    
    bool closed;
} ActorQueue;

// Core actor system functions
ActorSystem* actor_system_create(const char* name, size_t thread_pool_size);
void actor_system_destroy(ActorSystem* system);
Result_void_ptr actor_system_start(ActorSystem* system);
Result_void_ptr actor_system_shutdown(ActorSystem* system, uint64_t timeout_ms);
bool actor_system_is_running(const ActorSystem* system);

// Actor lifecycle management
ActorRef* actor_spawn(ActorSystem* system, const char* name, ActorBehavior behavior, void* context, size_t context_size);
ActorRef* actor_spawn_child(ActorRef* parent, const char* name, ActorBehavior behavior, void* context, size_t context_size);
Result_void_ptr actor_terminate(ActorRef* actor_ref);
Result_void_ptr actor_restart(ActorRef* actor_ref);

// Message passing
ActorFuture* actor_send(ActorRef* to, void* message, size_t message_size);
ActorFuture* actor_send_with_timeout(ActorRef* to, void* message, size_t message_size, uint64_t timeout_ms);
Result_void_ptr actor_send_fire_and_forget(ActorRef* to, void* message, size_t message_size);
Result_void_ptr actor_reply(ActorMessage* original_message, void* response, size_t response_size);

// Future operations
Result_void_ptr actor_future_await(ActorFuture* future);
Result_void_ptr actor_future_await_timeout(ActorFuture* future, uint64_t timeout_ms);
bool actor_future_is_ready(const ActorFuture* future);
void actor_future_set_callback(ActorFuture* future, void (*callback)(ActorFuture*, void*), void* context);
void actor_future_destroy(ActorFuture* future);

// Actor reference management
ActorRef* actor_ref_create(uint64_t actor_id, const char* name);
ActorRef* actor_ref_copy(ActorRef* ref);
void actor_ref_destroy(ActorRef* ref);
bool actor_ref_is_valid(const ActorRef* ref);
Result_void_ptr actor_ref_resolve(ActorSystem* system, const char* path, ActorRef** result);

// Actor behavior helpers
void actor_behavior_init(ActorBehavior* behavior);
void actor_become(Actor* actor, ActorBehavior new_behavior);
void actor_supervise_child(ActorRef* parent, ActorRef* child, SupervisionStrategy strategy);

// Mailbox operations
ActorMailbox* actor_mailbox_create(size_t max_messages);
void actor_mailbox_destroy(ActorMailbox* mailbox);
Result_void_ptr actor_mailbox_enqueue(ActorMailbox* mailbox, ActorMessage* message);
ActorMessage* actor_mailbox_dequeue(ActorMailbox* mailbox, uint64_t timeout_ms);
size_t actor_mailbox_size(const ActorMailbox* mailbox);
void actor_mailbox_close(ActorMailbox* mailbox);

// Message utilities
ActorMessage* actor_message_create(MessageType type, void* data, size_t data_size);
void actor_message_destroy(ActorMessage* message);
uint64_t actor_message_get_correlation_id(const ActorMessage* message);

// System utilities
Actor* actor_system_find_actor(ActorSystem* system, uint64_t actor_id);
Actor* actor_system_find_actor_by_name(ActorSystem* system, const char* name);
void actor_system_get_statistics(ActorSystem* system, uint64_t* total_actors, uint64_t* total_messages, uint64_t* active_futures);

// Error handling
#define ERROR_ACTOR_NOT_FOUND         0xA001
#define ERROR_ACTOR_TERMINATED        0xA002
#define ERROR_MAILBOX_FULL           0xA003
#define ERROR_MESSAGE_TIMEOUT         0xA004
#define ERROR_FUTURE_CANCELLED        0xA005
#define ERROR_ACTOR_SYSTEM_SHUTDOWN   0xA006
#define ERROR_SUPERVISION_FAILURE     0xA007
#define ERROR_ACTOR_SPAWN_FAILED      0xA008

// Convenience macros
#define ACTOR_DEFINE_HANDLER(name) \
    void name(Actor* self, ActorMessage* message)

#define ACTOR_SEND_AND_FORGET(to, msg) \
    actor_send_fire_and_forget(to, &(msg), sizeof(msg))

#define ACTOR_AWAIT(future) \
    ({ \
        actor_future_await(future); \
        future->result; \
    })

#endif // GOO_ACTOR_SYSTEM_H