#include "../../include/actor_system.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>

// Global actor system instance
static ActorSystem* global_actor_system = NULL;
static pthread_mutex_t global_system_mutex = PTHREAD_MUTEX_INITIALIZER;

// Utility functions
static uint64_t get_current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static uint64_t generate_id(uint64_t* counter) {
    return __atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
}

// Actor system message queue implementation
ActorQueue* actor_queue_create(size_t capacity) {
    ActorQueue* queue = xmalloc(sizeof(ActorQueue));
    if (!queue) return NULL;
    
    queue->messages = calloc(capacity, sizeof(ActorMessage*));
    if (!queue->messages) {
        free(queue);
        return NULL;
    }
    
    queue->head = 0;
    queue->tail = 0;
    queue->capacity = capacity;
    queue->count = 0;
    queue->closed = false;
    
    if (pthread_mutex_init(&queue->mutex, NULL) != 0 ||
        pthread_cond_init(&queue->not_empty, NULL) != 0 ||
        pthread_cond_init(&queue->not_full, NULL) != 0) {
        free(queue->messages);
        free(queue);
        return NULL;
    }
    
    return queue;
}

void actor_queue_destroy(ActorQueue* queue) {
    if (!queue) return;
    
    pthread_mutex_lock(&queue->mutex);
    queue->closed = true;
    
    // Clean up remaining messages
    while (queue->count > 0) {
        ActorMessage* msg = queue->messages[queue->head];
        queue->head = (queue->head + 1) % queue->capacity;
        queue->count--;
        actor_message_destroy(msg);
    }
    
    pthread_cond_broadcast(&queue->not_empty);
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
    
    free(queue->messages);
    free(queue);
}

Result_void_ptr actor_queue_enqueue(ActorQueue* queue, ActorMessage* message) {
    if (!queue || !message) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid queue or message";
        }
        return ERR_PTR(err);
    }
    
    pthread_mutex_lock(&queue->mutex);
    
    if (queue->closed) {
        pthread_mutex_unlock(&queue->mutex);
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_ACTOR_SYSTEM_SHUTDOWN;
            err->message = "Queue is closed";
        }
        return ERR_PTR(err);
    }
    
    while (queue->count >= queue->capacity) {
        pthread_cond_wait(&queue->not_full, &queue->mutex);
        if (queue->closed) {
            pthread_mutex_unlock(&queue->mutex);
            Error* err = xmalloc(sizeof(Error));
            if (err) {
                err->code = ERROR_ACTOR_SYSTEM_SHUTDOWN;
                err->message = "Queue closed while waiting";
            }
            return ERR_PTR(err);
        }
    }
    
    queue->messages[queue->tail] = message;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;
    
    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
    
    return OK_PTR(message);
}

ActorMessage* actor_queue_dequeue(ActorQueue* queue, uint64_t timeout_ms) {
    if (!queue) return NULL;
    
    pthread_mutex_lock(&queue->mutex);
    
    struct timespec deadline;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }
    
    while (queue->count == 0 && !queue->closed) {
        int result;
        if (timeout_ms > 0) {
            result = pthread_cond_timedwait(&queue->not_empty, &queue->mutex, &deadline);
            if (result == ETIMEDOUT) {
                pthread_mutex_unlock(&queue->mutex);
                return NULL;
            }
        } else {
            pthread_cond_wait(&queue->not_empty, &queue->mutex);
        }
    }
    
    if (queue->count == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }
    
    ActorMessage* message = queue->messages[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    
    return message;
}

// Actor mailbox implementation
ActorMailbox* actor_mailbox_create(size_t max_messages) {
    ActorMailbox* mailbox = xmalloc(sizeof(ActorMailbox));
    if (!mailbox) return NULL;
    
    mailbox->head = NULL;
    mailbox->tail = NULL;
    mailbox->message_count = 0;
    mailbox->max_messages = max_messages;
    mailbox->closed = false;
    
    if (pthread_mutex_init(&mailbox->mutex, NULL) != 0 ||
        pthread_cond_init(&mailbox->not_empty, NULL) != 0 ||
        pthread_cond_init(&mailbox->not_full, NULL) != 0) {
        free(mailbox);
        return NULL;
    }
    
    return mailbox;
}

void actor_mailbox_destroy(ActorMailbox* mailbox) {
    if (!mailbox) return;
    
    pthread_mutex_lock(&mailbox->mutex);
    mailbox->closed = true;
    
    // Clean up remaining messages
    ActorMessage* current = mailbox->head;
    while (current) {
        ActorMessage* next = current->next;
        actor_message_destroy(current);
        current = next;
    }
    
    pthread_cond_broadcast(&mailbox->not_empty);
    pthread_cond_broadcast(&mailbox->not_full);
    pthread_mutex_unlock(&mailbox->mutex);
    
    pthread_mutex_destroy(&mailbox->mutex);
    pthread_cond_destroy(&mailbox->not_empty);
    pthread_cond_destroy(&mailbox->not_full);
    
    free(mailbox);
}

Result_void_ptr actor_mailbox_enqueue(ActorMailbox* mailbox, ActorMessage* message) {
    if (!mailbox || !message) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid mailbox or message";
        }
        return ERR_PTR(err);
    }
    
    pthread_mutex_lock(&mailbox->mutex);
    
    if (mailbox->closed) {
        pthread_mutex_unlock(&mailbox->mutex);
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_ACTOR_TERMINATED;
            err->message = "Actor mailbox is closed";
        }
        return ERR_PTR(err);
    }
    
    while (mailbox->message_count >= mailbox->max_messages) {
        pthread_cond_wait(&mailbox->not_full, &mailbox->mutex);
        if (mailbox->closed) {
            pthread_mutex_unlock(&mailbox->mutex);
            Error* err = xmalloc(sizeof(Error));
            if (err) {
                err->code = ERROR_ACTOR_TERMINATED;
                err->message = "Mailbox closed while waiting";
            }
            return ERR_PTR(err);
        }
    }
    
    message->next = NULL;
    if (mailbox->tail) {
        mailbox->tail->next = message;
    } else {
        mailbox->head = message;
    }
    mailbox->tail = message;
    mailbox->message_count++;
    
    pthread_cond_signal(&mailbox->not_empty);
    pthread_mutex_unlock(&mailbox->mutex);
    
    return OK_PTR(message);
}

ActorMessage* actor_mailbox_dequeue(ActorMailbox* mailbox, uint64_t timeout_ms) {
    if (!mailbox) return NULL;
    
    pthread_mutex_lock(&mailbox->mutex);
    
    struct timespec deadline;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }
    
    while (mailbox->message_count == 0 && !mailbox->closed) {
        int result;
        if (timeout_ms > 0) {
            result = pthread_cond_timedwait(&mailbox->not_empty, &mailbox->mutex, &deadline);
            if (result == ETIMEDOUT) {
                pthread_mutex_unlock(&mailbox->mutex);
                return NULL;
            }
        } else {
            pthread_cond_wait(&mailbox->not_empty, &mailbox->mutex);
        }
    }
    
    if (mailbox->message_count == 0) {
        pthread_mutex_unlock(&mailbox->mutex);
        return NULL;
    }
    
    ActorMessage* message = mailbox->head;
    mailbox->head = message->next;
    if (!mailbox->head) {
        mailbox->tail = NULL;
    }
    mailbox->message_count--;
    
    pthread_cond_signal(&mailbox->not_full);
    pthread_mutex_unlock(&mailbox->mutex);
    
    return message;
}

size_t actor_mailbox_size(const ActorMailbox* mailbox) {
    if (!mailbox) return 0;
    
    pthread_mutex_lock((pthread_mutex_t*)&mailbox->mutex);
    size_t count = mailbox->message_count;
    pthread_mutex_unlock((pthread_mutex_t*)&mailbox->mutex);
    
    return count;
}

void actor_mailbox_close(ActorMailbox* mailbox) {
    if (!mailbox) return;
    
    pthread_mutex_lock(&mailbox->mutex);
    mailbox->closed = true;
    pthread_cond_broadcast(&mailbox->not_empty);
    pthread_cond_broadcast(&mailbox->not_full);
    pthread_mutex_unlock(&mailbox->mutex);
}

// Actor message implementation
ActorMessage* actor_message_create(MessageType type, void* data, size_t data_size) {
    ActorMessage* message = xmalloc(sizeof(ActorMessage));
    if (!message) return NULL;
    
    static uint64_t message_id_counter = 1;
    message->message_id = generate_id(&message_id_counter);
    message->type = type;
    message->sender_id = 0;
    message->receiver_id = 0;
    
    if (data && data_size > 0) {
        message->data = malloc(data_size);
        if (!message->data) {
            free(message);
            return NULL;
        }
        memcpy(message->data, data, data_size);
        message->data_size = data_size;
    } else {
        message->data = NULL;
        message->data_size = 0;
    }
    
    message->data_destructor = free;
    message->response_future = NULL;
    message->expects_response = false;
    message->correlation_id = 0;
    
    clock_gettime(CLOCK_REALTIME, &message->created_at);
    message->deadline = message->created_at;
    message->deadline.tv_sec += 30; // 30 second default deadline
    
    message->next = NULL;
    
    return message;
}

void actor_message_destroy(ActorMessage* message) {
    if (!message) return;
    
    if (message->data && message->data_destructor) {
        message->data_destructor(message->data);
    }
    
    if (message->response_future) {
        actor_future_destroy(message->response_future);
    }
    
    free(message);
}

uint64_t actor_message_get_correlation_id(const ActorMessage* message) {
    return message ? message->correlation_id : 0;
}

// Actor future implementation
ActorFuture* actor_future_create(uint64_t correlation_id, uint64_t timeout_ms) {
    ActorFuture* future = xmalloc(sizeof(ActorFuture));
    if (!future) return NULL;
    
    static uint64_t future_id_counter = 1;
    future->future_id = generate_id(&future_id_counter);
    future->correlation_id = correlation_id;
    future->state = FUTURE_STATE_PENDING;
    
    future->result = NULL;
    future->result_size = 0;
    future->error = NULL;
    
    if (pthread_mutex_init(&future->mutex, NULL) != 0 ||
        pthread_cond_init(&future->condition, NULL) != 0) {
        free(future);
        return NULL;
    }
    
    future->completion_callback = NULL;
    future->callback_context = NULL;
    
    clock_gettime(CLOCK_REALTIME, &future->created_at);
    future->completed_at = (struct timespec){0, 0};
    future->timeout_ms = timeout_ms;
    
    future->ref_count = 1;
    future->next = NULL;
    
    return future;
}

void actor_future_destroy(ActorFuture* future) {
    if (!future) return;
    
    pthread_mutex_lock(&future->mutex);
    future->ref_count--;
    
    if (future->ref_count > 0) {
        pthread_mutex_unlock(&future->mutex);
        return;
    }
    
    if (future->result) {
        free(future->result);
    }
    
    if (future->error) {
        free(future->error);
    }
    
    pthread_mutex_unlock(&future->mutex);
    
    pthread_mutex_destroy(&future->mutex);
    pthread_cond_destroy(&future->condition);
    
    free(future);
}

Result_void_ptr actor_future_await(ActorFuture* future) {
    return actor_future_await_timeout(future, future->timeout_ms);
}

Result_void_ptr actor_future_await_timeout(ActorFuture* future, uint64_t timeout_ms) {
    if (!future) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid future";
        }
        return ERR_PTR(err);
    }
    
    pthread_mutex_lock(&future->mutex);
    
    if (future->state == FUTURE_STATE_COMPLETED) {
        pthread_mutex_unlock(&future->mutex);
        return OK_PTR(future->result);
    }
    
    if (future->state == FUTURE_STATE_ERROR) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            memcpy(err, future->error, sizeof(Error));
        }
        pthread_mutex_unlock(&future->mutex);
        return ERR_PTR(err);
    }
    
    struct timespec deadline;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }
    
    while (future->state == FUTURE_STATE_PENDING) {
        int result;
        if (timeout_ms > 0) {
            result = pthread_cond_timedwait(&future->condition, &future->mutex, &deadline);
            if (result == ETIMEDOUT) {
                future->state = FUTURE_STATE_ERROR;
                if (!future->error) {
                    future->error = xmalloc(sizeof(Error));
                    if (future->error) {
                        future->error->code = ERROR_MESSAGE_TIMEOUT;
                        future->error->message = "Future timed out";
                    }
                }
                Error* err = xmalloc(sizeof(Error));
                if (err) {
                    err->code = ERROR_MESSAGE_TIMEOUT;
                    err->message = "Future await timed out";
                }
                pthread_mutex_unlock(&future->mutex);
                return ERR_PTR(err);
            }
        } else {
            pthread_cond_wait(&future->condition, &future->mutex);
        }
    }
    
    if (future->state == FUTURE_STATE_COMPLETED) {
        pthread_mutex_unlock(&future->mutex);
        return OK_PTR(future->result);
    } else {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            if (future->error) {
                memcpy(err, future->error, sizeof(Error));
            } else {
                err->code = ERROR_INTERNAL;
                err->message = "Future completed with error";
            }
        }
        pthread_mutex_unlock(&future->mutex);
        return ERR_PTR(err);
    }
}

bool actor_future_is_ready(const ActorFuture* future) {
    if (!future) return false;
    
    pthread_mutex_lock((pthread_mutex_t*)&future->mutex);
    bool ready = (future->state != FUTURE_STATE_PENDING);
    pthread_mutex_unlock((pthread_mutex_t*)&future->mutex);
    
    return ready;
}

void actor_future_set_callback(ActorFuture* future, void (*callback)(ActorFuture*, void*), void* context) {
    if (!future || !callback) return;
    
    pthread_mutex_lock(&future->mutex);
    future->completion_callback = callback;
    future->callback_context = context;
    
    // If already completed, call callback immediately
    if (future->state != FUTURE_STATE_PENDING) {
        pthread_mutex_unlock(&future->mutex);
        callback(future, context);
    } else {
        pthread_mutex_unlock(&future->mutex);
    }
}

static void actor_future_complete(ActorFuture* future, void* result, size_t result_size) {
    if (!future) return;
    
    pthread_mutex_lock(&future->mutex);
    
    if (future->state != FUTURE_STATE_PENDING) {
        pthread_mutex_unlock(&future->mutex);
        return;
    }
    
    future->state = FUTURE_STATE_COMPLETED;
    
    if (result && result_size > 0) {
        future->result = malloc(result_size);
        if (future->result) {
            memcpy(future->result, result, result_size);
            future->result_size = result_size;
        }
    }
    
    clock_gettime(CLOCK_REALTIME, &future->completed_at);
    
    pthread_cond_broadcast(&future->condition);
    
    if (future->completion_callback) {
        void (*callback)(ActorFuture*, void*) = future->completion_callback;
        void* context = future->callback_context;
        pthread_mutex_unlock(&future->mutex);
        callback(future, context);
    } else {
        pthread_mutex_unlock(&future->mutex);
    }
}

static void actor_future_set_error(ActorFuture* future, Error* error) {
    if (!future) return;
    
    pthread_mutex_lock(&future->mutex);
    
    if (future->state != FUTURE_STATE_PENDING) {
        pthread_mutex_unlock(&future->mutex);
        return;
    }
    
    future->state = FUTURE_STATE_ERROR;
    
    if (error) {
        future->error = xmalloc(sizeof(Error));
        if (future->error) {
            memcpy(future->error, error, sizeof(Error));
        }
    }
    
    clock_gettime(CLOCK_REALTIME, &future->completed_at);
    
    pthread_cond_broadcast(&future->condition);
    
    if (future->completion_callback) {
        void (*callback)(ActorFuture*, void*) = future->completion_callback;
        void* context = future->callback_context;
        pthread_mutex_unlock(&future->mutex);
        callback(future, context);
    } else {
        pthread_mutex_unlock(&future->mutex);
    }
}

// Actor reference implementation
ActorRef* actor_ref_create(uint64_t actor_id, const char* name) {
    ActorRef* ref = xmalloc(sizeof(ActorRef));
    if (!ref) return NULL;
    
    ref->actor_id = actor_id;
    ref->is_valid = true;
    ref->is_remote = false;
    ref->remote_port = 0;
    ref->ref_count = 1;
    
    if (name) {
        strncpy(ref->actor_name, name, sizeof(ref->actor_name) - 1);
        ref->actor_name[sizeof(ref->actor_name) - 1] = '\0';
    } else {
        snprintf(ref->actor_name, sizeof(ref->actor_name), "actor_%llu", actor_id);
    }
    
    ref->remote_address[0] = '\0';
    
    if (pthread_mutex_init(&ref->ref_mutex, NULL) != 0) {
        free(ref);
        return NULL;
    }
    
    return ref;
}

ActorRef* actor_ref_copy(ActorRef* ref) {
    if (!ref) return NULL;
    
    pthread_mutex_lock(&ref->ref_mutex);
    ref->ref_count++;
    pthread_mutex_unlock(&ref->ref_mutex);
    
    return ref;
}

void actor_ref_destroy(ActorRef* ref) {
    if (!ref) return;
    
    pthread_mutex_lock(&ref->ref_mutex);
    ref->ref_count--;
    
    if (ref->ref_count > 0) {
        pthread_mutex_unlock(&ref->ref_mutex);
        return;
    }
    
    pthread_mutex_unlock(&ref->ref_mutex);
    pthread_mutex_destroy(&ref->ref_mutex);
    
    free(ref);
}

bool actor_ref_is_valid(const ActorRef* ref) {
    return ref && ref->is_valid;
}

// Actor behavior initialization
void actor_behavior_init(ActorBehavior* behavior) {
    if (!behavior) return;
    
    behavior->handle_message = NULL;
    behavior->pre_start = NULL;
    behavior->post_stop = NULL;
    behavior->pre_restart = NULL;
    behavior->post_restart = NULL;
    behavior->supervision_strategy = SUPERVISION_ONE_FOR_ONE;
    behavior->max_restarts = 3;
    behavior->restart_window_ms = 60000; // 1 minute
}

// Actor execution thread function
static void* actor_thread_function(void* arg) {
    Actor* actor = (Actor*)arg;
    if (!actor) return NULL;
    
    actor->thread_running = true;
    
    // Call pre_start lifecycle method
    if (actor->behavior.pre_start) {
        actor->behavior.pre_start(actor);
    }
    
    actor->state = ACTOR_STATE_RUNNING;
    
    // Main message processing loop
    while (actor->state == ACTOR_STATE_RUNNING && !actor->system->shutdown_requested) {
        ActorMessage* message = actor_mailbox_dequeue(actor->mailbox, 1000); // 1 second timeout
        
        if (message) {
            clock_gettime(CLOCK_REALTIME, &actor->last_activity);
            
            // Handle system messages
            if (message->type == MESSAGE_TYPE_SYSTEM_STOP) {
                actor->state = ACTOR_STATE_TERMINATED;
                actor_message_destroy(message);
                break;
            }
            
            // Process user message
            if (actor->behavior.handle_message) {
                actor->behavior.handle_message(actor, message);
            }
            
            actor->messages_processed++;
            actor_message_destroy(message);
        }
    }
    
    // Call post_stop lifecycle method
    if (actor->behavior.post_stop) {
        actor->behavior.post_stop(actor);
    }
    
    actor->thread_running = false;
    return NULL;
}

// Actor system implementation
ActorSystem* actor_system_create(const char* name, size_t thread_pool_size) {
    ActorSystem* system = xmalloc(sizeof(ActorSystem));
    if (!system) return NULL;
    
    // Initialize system name
    if (name) {
        strncpy(system->name, name, sizeof(system->name) - 1);
        system->name[sizeof(system->name) - 1] = '\0';
    } else {
        strcpy(system->name, "DefaultActorSystem");
    }
    
    system->is_running = false;
    system->shutdown_requested = false;
    
    // Initialize actor management
    system->max_actors = MAX_ACTORS;
    system->actors = calloc(system->max_actors, sizeof(Actor*));
    if (!system->actors) {
        free(system);
        return NULL;
    }
    
    system->actor_count = 0;
    system->next_actor_id = 1;
    
    if (pthread_mutex_init(&system->actors_mutex, NULL) != 0) {
        free(system->actors);
        free(system);
        return NULL;
    }
    
    // Initialize thread pool
    system->thread_pool_size = thread_pool_size > 0 ? thread_pool_size : ACTOR_THREAD_POOL_SIZE;
    system->thread_pool = calloc(system->thread_pool_size, sizeof(pthread_t));
    system->thread_active = calloc(system->thread_pool_size, sizeof(bool));
    
    if (!system->thread_pool || !system->thread_active) {
        pthread_mutex_destroy(&system->actors_mutex);
        free(system->actors);
        free(system->thread_pool);
        free(system->thread_active);
        free(system);
        return NULL;
    }
    
    // Initialize future management
    system->max_futures = MAX_ACTORS * 10; // 10 futures per actor max
    system->futures = calloc(system->max_futures, sizeof(ActorFuture*));
    if (!system->futures) {
        pthread_mutex_destroy(&system->actors_mutex);
        free(system->actors);
        free(system->thread_pool);
        free(system->thread_active);
        free(system);
        return NULL;
    }
    
    system->future_count = 0;
    system->next_future_id = 1;
    
    if (pthread_mutex_init(&system->futures_mutex, NULL) != 0) {
        pthread_mutex_destroy(&system->actors_mutex);
        free(system->actors);
        free(system->thread_pool);
        free(system->thread_active);
        free(system->futures);
        free(system);
        return NULL;
    }
    
    // Initialize system queue
    system->system_queue = actor_queue_create(1000);
    if (!system->system_queue) {
        pthread_mutex_destroy(&system->actors_mutex);
        pthread_mutex_destroy(&system->futures_mutex);
        free(system->actors);
        free(system->thread_pool);
        free(system->thread_active);
        free(system->futures);
        free(system);
        return NULL;
    }
    
    // Initialize configuration
    system->default_mailbox_size = DEFAULT_MAILBOX_SIZE;
    system->message_timeout_ms = 30000; // 30 seconds
    system->actor_idle_timeout_ms = 300000; // 5 minutes
    
    // Initialize statistics
    system->total_messages_sent = 0;
    system->total_messages_processed = 0;
    system->total_actors_created = 0;
    system->total_actors_terminated = 0;
    
    // Initialize shutdown synchronization
    if (pthread_mutex_init(&system->shutdown_mutex, NULL) != 0 ||
        pthread_cond_init(&system->shutdown_condition, NULL) != 0) {
        actor_queue_destroy(system->system_queue);
        pthread_mutex_destroy(&system->actors_mutex);
        pthread_mutex_destroy(&system->futures_mutex);
        free(system->actors);
        free(system->thread_pool);
        free(system->thread_active);
        free(system->futures);
        free(system);
        return NULL;
    }
    
    system->guardian = NULL;
    
    return system;
}

void actor_system_destroy(ActorSystem* system) {
    if (!system) return;
    
    // Shutdown system if still running
    if (system->is_running) {
        actor_system_shutdown(system, 5000); // 5 second timeout
    }
    
    // Clean up all actors
    pthread_mutex_lock(&system->actors_mutex);
    for (size_t i = 0; i < system->actor_count; i++) {
        if (system->actors[i]) {
            Actor* actor = system->actors[i];
            
            // Terminate actor thread
            if (actor->thread_running) {
                actor->state = ACTOR_STATE_TERMINATED;
                actor_mailbox_close(actor->mailbox);
                pthread_join(actor->thread, NULL);
            }
            
            // Clean up actor resources
            actor_mailbox_destroy(actor->mailbox);
            actor_ref_destroy(actor->self_ref);
            
            if (actor->context) {
                free(actor->context);
            }
            
            if (actor->children) {
                for (size_t j = 0; j < actor->child_count; j++) {
                    actor_ref_destroy(actor->children[j]);
                }
                free(actor->children);
            }
            
            if (actor->parent) {
                actor_ref_destroy(actor->parent);
            }
            
            free(actor);
        }
    }
    pthread_mutex_unlock(&system->actors_mutex);
    
    // Clean up all futures
    pthread_mutex_lock(&system->futures_mutex);
    for (size_t i = 0; i < system->future_count; i++) {
        if (system->futures[i]) {
            actor_future_destroy(system->futures[i]);
        }
    }
    pthread_mutex_unlock(&system->futures_mutex);
    
    // Clean up system resources
    actor_queue_destroy(system->system_queue);
    
    if (system->guardian) {
        actor_ref_destroy(system->guardian);
    }
    
    pthread_mutex_destroy(&system->actors_mutex);
    pthread_mutex_destroy(&system->futures_mutex);
    pthread_mutex_destroy(&system->shutdown_mutex);
    pthread_cond_destroy(&system->shutdown_condition);
    
    free(system->actors);
    free(system->thread_pool);
    free(system->thread_active);
    free(system->futures);
    free(system);
}

Result_void_ptr actor_system_start(ActorSystem* system) {
    if (!system) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid actor system";
        }
        return ERR_PTR(err);
    }
    
    if (system->is_running) {
        return OK_PTR(system);
    }
    
    system->is_running = true;
    system->shutdown_requested = false;
    
    // Set global system reference
    pthread_mutex_lock(&global_system_mutex);
    global_actor_system = system;
    pthread_mutex_unlock(&global_system_mutex);
    
    return OK_PTR(system);
}

Result_void_ptr actor_system_shutdown(ActorSystem* system, uint64_t timeout_ms) {
    if (!system) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid actor system";
        }
        return ERR_PTR(err);
    }
    
    if (!system->is_running) {
        return OK_PTR(system);
    }
    
    pthread_mutex_lock(&system->shutdown_mutex);
    system->shutdown_requested = true;
    
    // Send stop messages to all actors
    pthread_mutex_lock(&system->actors_mutex);
    for (size_t i = 0; i < system->actor_count; i++) {
        if (system->actors[i] && system->actors[i]->state == ACTOR_STATE_RUNNING) {
            ActorMessage* stop_msg = actor_message_create(MESSAGE_TYPE_SYSTEM_STOP, NULL, 0);
            if (stop_msg) {
                actor_mailbox_enqueue(system->actors[i]->mailbox, stop_msg);
            }
        }
    }
    pthread_mutex_unlock(&system->actors_mutex);
    
    // Wait for shutdown with timeout
    struct timespec deadline;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
        
        pthread_cond_timedwait(&system->shutdown_condition, &system->shutdown_mutex, &deadline);
    }
    
    system->is_running = false;
    pthread_mutex_unlock(&system->shutdown_mutex);
    
    // Clear global system reference
    pthread_mutex_lock(&global_system_mutex);
    if (global_actor_system == system) {
        global_actor_system = NULL;
    }
    pthread_mutex_unlock(&global_system_mutex);
    
    return OK_PTR(system);
}

bool actor_system_is_running(const ActorSystem* system) {
    return system && system->is_running;
}

// Actor lifecycle implementation
ActorRef* actor_spawn(ActorSystem* system, const char* name, ActorBehavior behavior, void* context, size_t context_size) {
    if (!system || !behavior.handle_message) {
        return NULL;
    }
    
    pthread_mutex_lock(&system->actors_mutex);
    
    if (system->actor_count >= system->max_actors) {
        pthread_mutex_unlock(&system->actors_mutex);
        return NULL;
    }
    
    // Create new actor
    Actor* actor = xmalloc(sizeof(Actor));
    if (!actor) {
        pthread_mutex_unlock(&system->actors_mutex);
        return NULL;
    }
    
    actor->id = generate_id(&system->next_actor_id);
    actor->state = ACTOR_STATE_CREATED;
    actor->behavior = behavior;
    actor->system = system;
    
    // Set actor name
    if (name) {
        strncpy(actor->name, name, sizeof(actor->name) - 1);
        actor->name[sizeof(actor->name) - 1] = '\0';
    } else {
        snprintf(actor->name, sizeof(actor->name), "actor_%llu", actor->id);
    }
    
    // Copy context if provided
    if (context && context_size > 0) {
        actor->context = malloc(context_size);
        if (!actor->context) {
            free(actor);
            pthread_mutex_unlock(&system->actors_mutex);
            return NULL;
        }
        memcpy(actor->context, context, context_size);
        actor->context_size = context_size;
    } else {
        actor->context = NULL;
        actor->context_size = 0;
    }
    
    // Create mailbox
    actor->mailbox = actor_mailbox_create(system->default_mailbox_size);
    if (!actor->mailbox) {
        free(actor->context);
        free(actor);
        pthread_mutex_unlock(&system->actors_mutex);
        return NULL;
    }
    
    // Create self reference
    actor->self_ref = actor_ref_create(actor->id, actor->name);
    if (!actor->self_ref) {
        actor_mailbox_destroy(actor->mailbox);
        free(actor->context);
        free(actor);
        pthread_mutex_unlock(&system->actors_mutex);
        return NULL;
    }
    
    // Initialize parent-child relationships
    actor->parent = NULL;
    actor->children = NULL;
    actor->child_count = 0;
    actor->max_children = 100; // Default max children
    
    // Initialize supervision
    actor->restart_count = 0;
    clock_gettime(CLOCK_REALTIME, &actor->created_at);
    actor->last_restart = actor->created_at;
    
    // Initialize statistics
    actor->messages_processed = 0;
    actor->messages_sent = 0;
    actor->errors_count = 0;
    actor->last_activity = actor->created_at;
    
    actor->thread_running = false;
    actor->next = NULL;
    
    // Add to system
    system->actors[system->actor_count] = actor;
    system->actor_count++;
    system->total_actors_created++;
    
    // Start actor thread
    if (pthread_create(&actor->thread, NULL, actor_thread_function, actor) != 0) {
        // Remove from system
        system->actor_count--;
        system->actors[system->actor_count] = NULL;
        
        actor_ref_destroy(actor->self_ref);
        actor_mailbox_destroy(actor->mailbox);
        free(actor->context);
        free(actor);
        pthread_mutex_unlock(&system->actors_mutex);
        return NULL;
    }
    
    pthread_mutex_unlock(&system->actors_mutex);
    
    return actor_ref_copy(actor->self_ref);
}

Actor* actor_system_find_actor(ActorSystem* system, uint64_t actor_id) {
    if (!system) return NULL;
    
    pthread_mutex_lock(&system->actors_mutex);
    
    for (size_t i = 0; i < system->actor_count; i++) {
        if (system->actors[i] && system->actors[i]->id == actor_id) {
            Actor* actor = system->actors[i];
            pthread_mutex_unlock(&system->actors_mutex);
            return actor;
        }
    }
    
    pthread_mutex_unlock(&system->actors_mutex);
    return NULL;
}

Actor* actor_system_find_actor_by_name(ActorSystem* system, const char* name) {
    if (!system || !name) return NULL;
    
    pthread_mutex_lock(&system->actors_mutex);
    
    for (size_t i = 0; i < system->actor_count; i++) {
        if (system->actors[i] && strcmp(system->actors[i]->name, name) == 0) {
            Actor* actor = system->actors[i];
            pthread_mutex_unlock(&system->actors_mutex);
            return actor;
        }
    }
    
    pthread_mutex_unlock(&system->actors_mutex);
    return NULL;
}

// Message passing implementation
ActorFuture* actor_send(ActorRef* to, void* message, size_t message_size) {
    return actor_send_with_timeout(to, message, message_size, 30000); // 30 second default timeout
}

ActorFuture* actor_send_with_timeout(ActorRef* to, void* message, size_t message_size, uint64_t timeout_ms) {
    if (!to || !actor_ref_is_valid(to) || !global_actor_system) {
        return NULL;
    }
    
    // Find the target actor
    Actor* target_actor = actor_system_find_actor(global_actor_system, to->actor_id);
    if (!target_actor || target_actor->state != ACTOR_STATE_RUNNING) {
        return NULL;
    }
    
    // Create the message
    ActorMessage* msg = actor_message_create(MESSAGE_TYPE_USER, message, message_size);
    if (!msg) {
        return NULL;
    }
    
    // Create future for response
    ActorFuture* future = actor_future_create(msg->message_id, timeout_ms);
    if (!future) {
        actor_message_destroy(msg);
        return NULL;
    }
    
    // Link message and future
    msg->response_future = future;
    msg->expects_response = true;
    msg->correlation_id = future->correlation_id;
    msg->receiver_id = to->actor_id;
    
    // Add future to system tracking
    pthread_mutex_lock(&global_actor_system->futures_mutex);
    if (global_actor_system->future_count < global_actor_system->max_futures) {
        global_actor_system->futures[global_actor_system->future_count] = future;
        global_actor_system->future_count++;
    }
    pthread_mutex_unlock(&global_actor_system->futures_mutex);
    
    // Send message to target actor
    Result_void_ptr result = actor_mailbox_enqueue(target_actor->mailbox, msg);
    if (result.is_error) {
        // Remove future from tracking
        pthread_mutex_lock(&global_actor_system->futures_mutex);
        for (size_t i = 0; i < global_actor_system->future_count; i++) {
            if (global_actor_system->futures[i] == future) {
                global_actor_system->futures[i] = global_actor_system->futures[global_actor_system->future_count - 1];
                global_actor_system->future_count--;
                break;
            }
        }
        pthread_mutex_unlock(&global_actor_system->futures_mutex);
        
        actor_future_destroy(future);
        actor_message_destroy(msg);
        return NULL;
    }
    
    global_actor_system->total_messages_sent++;
    return future;
}

Result_void_ptr actor_send_fire_and_forget(ActorRef* to, void* message, size_t message_size) {
    if (!to || !actor_ref_is_valid(to) || !global_actor_system) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_ACTOR_NOT_FOUND;
            err->message = "Invalid actor reference";
        }
        return ERR_PTR(err);
    }
    
    // Find the target actor
    Actor* target_actor = actor_system_find_actor(global_actor_system, to->actor_id);
    if (!target_actor || target_actor->state != ACTOR_STATE_RUNNING) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_ACTOR_NOT_FOUND;
            err->message = "Target actor not found or not running";
        }
        return ERR_PTR(err);
    }
    
    // Create the message
    ActorMessage* msg = actor_message_create(MESSAGE_TYPE_USER, message, message_size);
    if (!msg) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Failed to create message";
        }
        return ERR_PTR(err);
    }
    
    msg->expects_response = false;
    msg->receiver_id = to->actor_id;
    
    // Send message to target actor
    Result_void_ptr result = actor_mailbox_enqueue(target_actor->mailbox, msg);
    if (result.is_error) {
        actor_message_destroy(msg);
        return result;
    }
    
    global_actor_system->total_messages_sent++;
    return OK_PTR(to);
}

Result_void_ptr actor_reply(ActorMessage* original_message, void* response, size_t response_size) {
    if (!original_message || !original_message->expects_response || !original_message->response_future) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid message or no response expected";
        }
        return ERR_PTR(err);
    }
    
    // Complete the future with the response
    actor_future_complete(original_message->response_future, response, response_size);
    
    return OK_PTR(response);
}

// Additional actor lifecycle functions
Result_void_ptr actor_terminate(ActorRef* actor_ref) {
    if (!actor_ref || !actor_ref_is_valid(actor_ref) || !global_actor_system) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_ACTOR_NOT_FOUND;
            err->message = "Invalid actor reference";
        }
        return ERR_PTR(err);
    }
    
    Actor* actor = actor_system_find_actor(global_actor_system, actor_ref->actor_id);
    if (!actor) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_ACTOR_NOT_FOUND;
            err->message = "Actor not found";
        }
        return ERR_PTR(err);
    }
    
    // Send termination message
    ActorMessage* stop_msg = actor_message_create(MESSAGE_TYPE_SYSTEM_STOP, NULL, 0);
    if (stop_msg) {
        actor_mailbox_enqueue(actor->mailbox, stop_msg);
    }
    
    // Mark actor reference as invalid
    actor_ref->is_valid = false;
    
    global_actor_system->total_actors_terminated++;
    
    return OK_PTR(actor_ref);
}

ActorRef* actor_spawn_child(ActorRef* parent, const char* name, ActorBehavior behavior, void* context, size_t context_size) {
    if (!parent || !actor_ref_is_valid(parent) || !global_actor_system) {
        return NULL;
    }
    
    Actor* parent_actor = actor_system_find_actor(global_actor_system, parent->actor_id);
    if (!parent_actor) {
        return NULL;
    }
    
    // Spawn the child actor
    ActorRef* child_ref = actor_spawn(global_actor_system, name, behavior, context, context_size);
    if (!child_ref) {
        return NULL;
    }
    
    Actor* child_actor = actor_system_find_actor(global_actor_system, child_ref->actor_id);
    if (!child_actor) {
        actor_terminate(child_ref);
        actor_ref_destroy(child_ref);
        return NULL;
    }
    
    // Set up parent-child relationship
    child_actor->parent = actor_ref_copy(parent);
    
    // Add child to parent's children list
    if (!parent_actor->children) {
        parent_actor->children = malloc(parent_actor->max_children * sizeof(ActorRef*));
        if (!parent_actor->children) {
            actor_terminate(child_ref);
            actor_ref_destroy(child_ref);
            return NULL;
        }
    }
    
    if (parent_actor->child_count < parent_actor->max_children) {
        parent_actor->children[parent_actor->child_count] = actor_ref_copy(child_ref);
        parent_actor->child_count++;
    }
    
    return child_ref;
}

// Actor system utilities
void actor_system_get_statistics(ActorSystem* system, uint64_t* total_actors, uint64_t* total_messages, uint64_t* active_futures) {
    if (!system) return;
    
    if (total_actors) {
        pthread_mutex_lock(&system->actors_mutex);
        *total_actors = system->actor_count;
        pthread_mutex_unlock(&system->actors_mutex);
    }
    
    if (total_messages) {
        *total_messages = system->total_messages_sent;
    }
    
    if (active_futures) {
        pthread_mutex_lock(&system->futures_mutex);
        *active_futures = system->future_count;
        pthread_mutex_unlock(&system->futures_mutex);
    }
}

Result_void_ptr actor_ref_resolve(ActorSystem* system, const char* path, ActorRef** result) {
    if (!system || !path || !result) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_INTERNAL;
            err->message = "Invalid parameters";
        }
        return ERR_PTR(err);
    }
    
    // Simple path resolution - just find by name
    Actor* actor = actor_system_find_actor_by_name(system, path);
    if (!actor) {
        Error* err = xmalloc(sizeof(Error));
        if (err) {
            err->code = ERROR_ACTOR_NOT_FOUND;
            err->message = "Actor not found";
        }
        return ERR_PTR(err);
    }
    
    *result = actor_ref_copy(actor->self_ref);
    return OK_PTR(*result);
}

void actor_become(Actor* actor, ActorBehavior new_behavior) {
    if (!actor) return;
    
    actor->behavior = new_behavior;
}

void actor_supervise_child(ActorRef* parent, ActorRef* child, SupervisionStrategy strategy) {
    if (!parent || !child || !global_actor_system) return;
    
    Actor* parent_actor = actor_system_find_actor(global_actor_system, parent->actor_id);
    Actor* child_actor = actor_system_find_actor(global_actor_system, child->actor_id);
    
    if (!parent_actor || !child_actor) return;
    
    // Set supervision strategy
    child_actor->behavior.supervision_strategy = strategy;
    
    // Ensure child is in parent's children list
    bool found = false;
    for (size_t i = 0; i < parent_actor->child_count; i++) {
        if (parent_actor->children[i]->actor_id == child->actor_id) {
            found = true;
            break;
        }
    }
    
    if (!found && parent_actor->child_count < parent_actor->max_children) {
        if (!parent_actor->children) {
            parent_actor->children = malloc(parent_actor->max_children * sizeof(ActorRef*));
        }
        if (parent_actor->children) {
            parent_actor->children[parent_actor->child_count] = actor_ref_copy(child);
            parent_actor->child_count++;
        }
    }
}